#include "controller.h"
#include "board_config.h"
#include "ble_service.h"
#include "ccid_service.h"
#include "tcp_service.h"
#include "settings.h"
#include "json_helpers.h"
#include "driver/gpio.h"
Controller controller;
static HardwareSerial machineSerial(1);
void Controller::begin() {
  mutex_=xSemaphoreCreateMutex();
  if(!mutex_) return;
  // Set hardware output latches before enabling GPIO output; Arduino's
  // digitalWrite before pinMode would fail its peripheral ownership check.
  gpio_set_level(static_cast<gpio_num_t>(board::startPin),0);
  gpio_set_level(static_cast<gpio_num_t>(board::stopPin),0);
  pinMode(board::startPin,OUTPUT); pinMode(board::stopPin,OUTPUT);
  digitalWrite(board::startPin,LOW); digitalWrite(board::stopPin,LOW);
  machineSerial.setRxBufferSize(2048); machineSerial.begin(board::machineBaud,SERIAL_8N1,board::machineRx,board::machineTx);
  missingTick_=millis();
  if(xTaskCreate(task,"machine",6144,this,3,nullptr)!=pdPASS) error_="無法啟動機台執行緒";
}
bool Controller::schedule(rehab::PulseKind kind,uint32_t gen) {
  if(!actions_.push({kind,gen})) {error_="控制佇列已滿"; return false;}
  return true;
}
bool Controller::uartSend(const rehab::Frame& f) {
  if(uartReplies_.push(f)) return true;
  error_="UART 回覆佇列已滿"; return false;
}
bool Controller::uartWrite(const rehab::Frame& f) {
  uint8_t bytes[260]; size_t n=rehab::encode(f,0x55,0x90,bytes);
  // Only this task writes UART. Reserve a complete frame in the driver buffer
  // before calling write(), so UART traffic cannot block the control clock.
  if(machineSerial.availableForWrite()<int(n)) return false;
  if(machineSerial.write(bytes,n)!=n) {error_="UART 寫入失敗"; return false;}
  txHex_=hexBytes(bytes,n); return true;
}
void Controller::flushUart(uint32_t now) {
  while(const auto* reply=uartReplies_.peek()) {
    if(!uartWrite(*reply)) return;
    rehab::Frame discarded; uartReplies_.pop(discarded);
  }
  if(!levelPending_&&levelQueued_) {
    const uint8_t payload[]={uint8_t(targetLevel_>>8),uint8_t(targetLevel_),0,0};
    const auto request=rehab::makeFrame(0x44,payload,4);
    if(uartWrite(request)) {
      sentLevel_=targetLevel_; levelQueued_=false; levelPending_=true;
      levelAt_=now; levelChecksum_=rehab::checksum(request);
    }
  }
}
void Controller::abnormalStop() {
  uint8_t reason=0; tcpService.send(rehab::makeFrame(0x24,&reason,1),true);
  if(!bleService.snapshot().subscribed) {uint8_t state[]={2,0xc9}; tcpService.send(rehab::makeFrame(0x20,state,2),true);}
}
void Controller::command(const rehab::Frame& f,uint32_t gen,uint32_t now) {
  (void)now;
  if(f.command==0x20 && f.length==1 && f.data[0]<=2) {
    bool good=f.data[0]==0?ccidService.snapshot().ready:f.data[0]==1?machineState_==1:bleService.snapshot().subscribed;
    uint8_t data[]={f.data[0],uint8_t(good?0xc8:0xc9)};
    tcpService.send(rehab::makeFrame(0x20,data,2),false,true,gen);
  } else if(f.command==0x22 && f.length==1 && f.data[0]<=2) {
    if(f.data[0]==2) {uint8_t data[]={2,0xc8}; tcpService.send(rehab::makeFrame(0x22,data,2),false,true,gen);}
    else if(!schedule(f.data[0]?rehab::PulseKind::Start:rehab::PulseKind::Stop,gen)) {
      uint8_t data[]={f.data[0],0xc9}; tcpService.send(rehab::makeFrame(0x22,data,2),false,true,gen);
    }
  } else if(f.command==0x25 && (f.length==1||f.length==2)) {
    targetLevel_=f.length==1?f.data[0]:rehab::be16(f.data);
    // ACK means received, not hardware applied; zero retains legacy no-op behavior.
    // Coalesce new targets while one hardware ACK is outstanding. A zero target
    // is an acknowledged no-op and also cancels a not-yet-written target.
    levelQueued_=targetLevel_>0;
    uint8_t ok=0xc8; tcpService.send(rehab::makeFrame(0x25,&ok,1),false,true,gen);
  }
  // Known replies and unknown commands are not echoed back into an ACK loop.
}
void Controller::machine(const rehab::Frame& f,uint32_t now) {
  bool valid=(f.command==0x27&&f.length==1&&f.data[0]<=3)||(f.command==0x28&&f.length==12)||
    (f.command==0x30&&f.length==0)||(f.command==0x3f&&f.length==6)||(f.command==0x44&&f.length==1);
  if(!valid) {error_="UART 指令或資料長度不符"; return;}
  uint8_t bytes[260]; size_t n=rehab::encode(f,0x55,0x90,bytes); rxHex_=hexBytes(bytes,n);
  uartAt_=now; hadUart_=true;
  if(f.command==0x27) {machineState_=f.data[0]; reportedState_=machineState_; stateAt_=now; uartSend(f);}
  else if(f.command==0x30) {machineState_=1; reportedState_=machineState_; stateAt_=now; emptyReads_=0; uint8_t ok=1; uartSend(rehab::makeFrame(0x30,&ok,1));}
  else if(f.command==0x44) {
    // A 44/01 frame is the resistance ACK. It must not be acknowledged again.
    if(levelPending_ && f.data[0]==levelChecksum_) levelPending_=false;
    else error_="未對應目前阻力指令的 UART ACK";
  } else {
    values_.accept(f,now); uint8_t check=rehab::checksum(f); uartSend(rehab::makeFrame(f.command,&check,1));
  }
}
void Controller::tick(uint32_t now) {
  auto network=tcpService.snapshot(); auto heart=bleService.snapshot();
  now=millis(); // Snapshot callbacks may have advanced receivedMs since entry.
  if(network.generation!=generation_ || (!network.connected&&seenConnection_)) {
    actions_.clear(); levelQueued_=false; pulseAckPending_=false; generation_=network.generation;
    // Reconnection establishes a fresh comparison baseline, not an event replay.
    seenRpm_=values_.has28?values_.rpm:-1; seenLevel_=values_.has3f?values_.level:-1; seenBpm_=heart.valid?heart.bpm:-1;
  }
  seenConnection_=network.connected;
  ReceivedFrame incoming;
  for(int i=0;i<16 && tcpService.receive(incoming);++i)
    if(network.connected&&incoming.generation==network.generation) command(incoming.frame,incoming.generation,now);
  size_t budget=512;
  while(budget-- && machineSerial.available()) {
    uint8_t b=machineSerial.read(); lastByte_=now; hadAnyByte_=true; uart_.feed(b,now); rehab::Frame f;
    while(uart_.next(f)) machine(f,now);
  }
  uart_.expire(now);
  rehab::Frame recovered; while(uart_.next(recovered)) machine(recovered,now);
  if(rehab::due(now,missingTick_,1000)) {
    missingTick_=now;
    if(!hadAnyByte_ || rehab::due(now,lastByte_,1000)) {
      if(tracking_ && emptyReads_>30) { actions_.clear(); levelQueued_=false; schedule(rehab::PulseKind::Stop,0); emptyReads_=0; ++timeouts_; error_="機台 UART 長時間未回應，停止追蹤"; }
      ++emptyReads_;
    }
  }
  if(levelPending_ && rehab::due(now,levelAt_,3000)) {levelPending_=false; ++timeouts_; error_="阻力指令 UART ACK 逾時";}
  flushUart(now);
  if(pulse_.kind==rehab::PulseKind::None && !pulseAckPending_ && actions_.size()) {
    Action next{}; actions_.pop(next); pulse_.start(next.kind,now,next.generation);
  }
  if(pulse_.kind!=rehab::PulseKind::None) {
    bool completed=pulse_.tick(now);
    digitalWrite(board::startPin,pulse_.kind==rehab::PulseKind::Start&&pulse_.high?HIGH:LOW);
    digitalWrite(board::stopPin,pulse_.kind==rehab::PulseKind::Stop&&pulse_.high?HIGH:LOW);
    if(completed) {
      bool starting=pulse_.kind==rehab::PulseKind::Start; tracking_=starting;
      if(starting) {
        seenRpm_=seenLevel_=seenBpm_=-1;
        // Idle time before START must not consume the new session's UART grace
        // period. Reset its one-second clock when the START pulse completes.
        emptyReads_=0; missingTick_=now;
      }
      else if(!pulse_.generation) {machineState_=3; abnormalStop();}
      if(pulse_.generation) {
        const uint8_t data[]={uint8_t(starting?1:0),0xc8};
        pulseAck_=rehab::makeFrame(0x22,data,2); pulseAckGeneration_=pulse_.generation; pulseAckPending_=true;
      }
      pulse_.kind=rehab::PulseKind::None;
    }
  }
  if(pulseAckPending_) {
    if(!network.connected||pulseAckGeneration_!=network.generation) pulseAckPending_=false;
    else if(tcpService.send(pulseAck_,false,true,pulseAckGeneration_)) pulseAckPending_=false;
  }
  if(heart.hasDisconnect && heart.disconnects!=bleDisconnectReported_ && !tracking_ && rehab::due(now,heart.lastDisconnectMs,5000)) {
    abnormalStop(); bleDisconnectReported_=heart.disconnects;
  }
  String uid;
  while(ccidService.takeCard(uid)) {
    uint8_t bytes[255]; size_t len=0;
    for(size_t i=0;i+1<uid.length()&&len<255;i+=2) {char pair[]={uid[i],uid[i+1],0}; bytes[len++]=strtoul(pair,nullptr,16);}
    if(network.connected&&len) tcpService.send(rehab::makeFrame(0x21,bytes,len),true);
  }
  const bool freshRpm=values_.has28&&!rehab::due(now,values_.at28,5000);
  const bool freshLevel=values_.has3f&&!rehab::due(now,values_.at3f,5000);
  const bool freshHeart=heart.valid&&heart.bpm>0&&heart.bpm<=255&&
      !rehab::due(now,heart.receivedMs,10000);
  if(!freshRpm||values_.rpm>255) {tcpService.cancelTelemetry(0); seenRpm_=-1;}
  if(!freshLevel||values_.level>255) {tcpService.cancelTelemetry(1); seenLevel_=-1;}
  if(!freshHeart) {tcpService.cancelTelemetry(2); seenBpm_=-1;}
  if(!tracking_||!network.connected) tcpService.cancelTelemetryAll();
  if(tracking_&&network.connected) {
    auto telemetry=[&](uint8_t selector,int value,int& seen,bool fresh,uint32_t observed,uint32_t ttl) {
      if(!fresh||value<0||value>255) {if(fresh&&value>255) seen=value; return;}
      if(value!=seen&&tcpService.publishTelemetry(selector,value,observed,ttl)) seen=value;
    };
    telemetry(0,values_.rpm,seenRpm_,freshRpm,values_.at28,5000);
    telemetry(1,values_.level,seenLevel_,freshLevel,values_.at3f,5000);
    telemetry(2,heart.bpm,seenBpm_,freshHeart,heart.receivedMs,10000);
  }
  if(settingsStore.pending() && rehab::canApplyConfig(tracking_,pulse_.kind,actions_.size()) &&
      !pulseAckPending_&&!levelQueued_&&!levelPending_&&!uartReplies_.size()&&tcpService.idle()) {
    Settings before,after;
    if(settingsStore.apply(before,after)) {
      if(strcmp(before.heartAddress,after.heartAddress)) {bleService.configure(after.heartAddress); seenBpm_=-1;}
      if(strcmp(before.server,after.server)||strcmp(before.ssid,after.ssid)||strcmp(before.password,after.password)) tcpService.configure(after.server);
    }
  }
}
void Controller::run() { for(;;) {xSemaphoreTake(mutex_,portMAX_DELAY); tick(millis()); xSemaphoreGive(mutex_); vTaskDelay(pdMS_TO_TICKS(5));} }
void Controller::appendStatus(cJSON* o,uint32_t now) {
  if(!mutex_) {js(o,"last_error","機台控制尚未初始化"); return;}
  xSemaphoreTake(mutex_,portMAX_DELAY);
  now=millis();
  const char* names[]={"Idle","Start","Pause","End"};
  if(reportedState_>=0) js(o,"state",names[reportedState_]); else cJSON_AddNullToObject(o,"state");
  jnullable(o,"state_age_ms",now-stateAt_,reportedState_>=0);
  js(o,"control_state",names[machineState_]); jb(o,"tracking",tracking_);
  bool fresh=hadUart_&&!rehab::due(now,uartAt_,5000);
  jb(o,"uart_online",fresh); jnullable(o,"age_ms",now-uartAt_,hadUart_);
  jnullable(o,"rpm",values_.rpm,values_.has28); jnullable(o,"machine_bpm",values_.bpm,values_.has28);
  jnullable(o,"minutes",values_.minutes,values_.has28); jnullable(o,"seconds",values_.seconds,values_.has28);
  jnullable(o,"distance_raw",values_.distance,values_.has28); jnullable(o,"calories_raw",values_.calories,values_.has28);
  jnullable(o,"watt28_raw",values_.watt28,values_.has28); jnullable(o,"watt3f_raw",values_.watt3f,values_.has3f);
  jnullable(o,"torque_raw",values_.torque,values_.has3f); jnullable(o,"reported_level",values_.level,values_.has3f);
  jnullable(o,"age28_ms",now-values_.at28,values_.has28); jnullable(o,"age3f_ms",now-values_.at3f,values_.has3f);
  jnullable(o,"target_level",targetLevel_,targetLevel_>=0); jnullable(o,"sent_level",sentLevel_,sentLevel_>=0);
  jb(o,"level_ack_pending",levelPending_); js(o,"last_error",error_);
  jb(o,"level_queued",levelQueued_); jn(o,"queued_controls",actions_.size());
  jb(o,"control_ack_queued",pulseAckPending_); jn(o,"queued_uart_replies",uartReplies_.size());
  js(o,"pulse",pulse_.kind==rehab::PulseKind::None?"idle":pulse_.kind==rehab::PulseKind::Start?"start":pulse_.high?"stop":"stop_settling");
  js(o,"rx_hex",rxHex_); js(o,"tx_hex",txHex_); jn(o,"checksum_errors",uart_.checksumErrors);
  jn(o,"framing_errors",uart_.framingErrors); jn(o,"timeouts",timeouts_+uart_.timeouts);
  xSemaphoreGive(mutex_);
}
