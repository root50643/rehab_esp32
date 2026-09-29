#include "tcp_service.h"
#include "json_helpers.h"
#include "board_config.h"
#include "ble_service.h"
#include <WiFi.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <lwip/tcp.h>
#include <fcntl.h>
#include <errno.h>

TcpService tcpService;
namespace {
bool hasRoute() { return WiFi.status()==WL_CONNECTED || WiFi.softAPgetStationNum()>0; }
bool routeLost(uint32_t localAddress,bool viaAp) {
  if(viaAp) return WiFi.softAPgetStationNum()==0 || localAddress!=uint32_t(WiFi.softAPIP());
  return WiFi.status()!=WL_CONNECTED || localAddress!=uint32_t(WiFi.localIP());
}
}
void TcpService::begin(const String& server) {
  mutex_=xSemaphoreCreateMutex(); rx_=xQueueCreate(16,sizeof(ReceivedFrame));
  tx_=xQueueCreate(16,sizeof(Outgoing)); urgent_=xQueueCreate(16,sizeof(Outgoing));
  server_=server;
  if(!mutex_||!rx_||!tx_||!urgent_) {error_="無法配置 TCP 工作佇列"; return;}
  if(xTaskCreate(task,"tcp",6144,this,2,nullptr)!=pdPASS) error_="無法啟動 TCP 工作執行緒";
}
void TcpService::configure(const String& server) {
  if(!mutex_) return;
  xSemaphoreTake(mutex_,portMAX_DELAY); server_=server; ++configVersion_; xSemaphoreGive(mutex_);
}
TcpSnapshot TcpService::snapshot() {
  if(!mutex_) return {};
  xSemaphoreTake(mutex_,portMAX_DELAY); TcpSnapshot s{connected_,generation_}; xSemaphoreGive(mutex_); return s;
}
bool TcpService::receive(ReceivedFrame& message) { return rx_&&xQueueReceive(rx_,&message,0)==pdTRUE; }
bool TcpService::send(const rehab::Frame& f,bool ack,bool urgent,uint32_t gen) {
  if(!mutex_||!tx_||!urgent_) return false;
  xSemaphoreTake(mutex_,portMAX_DELAY);
  bool accepted=false;
  if(connected_ && (!gen||gen==generation_)) {
    if(f.command==0x23&&f.length==2&&f.data[0]<3)
      accepted=telemetry_.publish(f.data[0],f.data[1],generation_,millis(),1000);
    else {
      // Only unsolicited notifications enter the single ACK wait slot.
      Outgoing message{f,generation_,ack&&!urgent};
      accepted=xQueueSend(urgent?urgent_:tx_,&message,0)==pdTRUE;
      if(!accepted) error_="TCP 傳送佇列已滿";
    }
  }
  xSemaphoreGive(mutex_); return accepted;
}
bool TcpService::publishTelemetry(uint8_t selector,int value,uint32_t observedAt,uint32_t validForMs) {
  if(!mutex_) return false;
  xSemaphoreTake(mutex_,portMAX_DELAY);
  const bool accepted=connected_&&telemetry_.publish(selector,value,generation_,observedAt,validForMs);
  xSemaphoreGive(mutex_); return accepted;
}
void TcpService::cancelTelemetry(uint8_t selector) {
  if(!mutex_) return;
  xSemaphoreTake(mutex_,portMAX_DELAY); telemetry_.cancel(selector); xSemaphoreGive(mutex_);
}
void TcpService::cancelTelemetryAll() {
  if(!mutex_) return;
  xSemaphoreTake(mutex_,portMAX_DELAY); telemetry_.clear(); xSemaphoreGive(mutex_);
}
bool TcpService::idle() {
  if(!mutex_||!tx_||!urgent_||!rx_) return false;
  xSemaphoreTake(mutex_,portMAX_DELAY);
  const bool idle=!writing_&&!waiting_&&!uxQueueMessagesWaiting(tx_)&&
      !uxQueueMessagesWaiting(urgent_)&&!uxQueueMessagesWaiting(rx_)&&
      !telemetry_.pending(generation_,millis());
  xSemaphoreGive(mutex_); return idle;
}
TcpService::WriteResult TcpService::writeFrame(int fd,const rehab::Frame& f,
    const rehab::TelemetryOutbox::Token* token) {
  uint8_t bytes[260]; const size_t length=rehab::encode(f,0x7a,0x7f,bytes);
  size_t offset=0; const uint32_t started=millis();
  while(offset<length && !rehab::due(millis(),started,500)) {
    if(token) {
      xSemaphoreTake(mutex_,portMAX_DELAY);
      bool current=telemetry_.current(*token,generation_,millis());
      xSemaphoreGive(mutex_);
      // Also inspect the live BLE value at the actual socket write: a BLE
      // callback can invalidate a sample before the controller's next tick.
      if(current && token->selector==2) {
        const auto heart=bleService.snapshot();
        current=heart.valid&&heart.bpm==token->value&&
            !rehab::due(millis(),heart.receivedMs,10000);
      }
      if(!current) return offset?WriteResult::Failed:WriteResult::Discarded;
    }
    const int count=::send(fd,bytes+offset,length-offset,0);
    if(count>0) offset+=size_t(count);
    else if(count==0 || (errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)) break;
    else vTaskDelay(pdMS_TO_TICKS(2));
  }
  const bool ok=offset==length;
  xSemaphoreTake(mutex_,portMAX_DELAY);
  if(ok) {
    txHex_=hexBytes(bytes,length); txAt_=millis(); hasTx_=true;
    if(f.command==0x23&&f.length==2&&f.data[0]<3) {sent_[f.data[0]]=f.data[1]; sentAt_[f.data[0]]=txAt_;}
    if(f.command==0x20) lastReply_=txHex_;
    if(f.command==0x21) cardResult_="已寫入 TCP，等待回覆";
  } else error_="TCP 寫入逾時或失敗";
  xSemaphoreGive(mutex_);
  // The caller clears writing_ together with waiting_, without an idle gap.
  return ok?WriteResult::Sent:WriteResult::Failed;
}
void TcpService::run() {
  int fd=-1;
  uint32_t version=0,lastAttempt=millis()-1000,ackAt=0,localAddress=0;
  bool viaAp=false,awaiting=false;
  rehab::Parser parser(0x7a,0x7f);
  Outgoing pending{}; unsigned attempts=0;
  for(;;) {
    xSemaphoreTake(mutex_,portMAX_DELAY);
    const String host=server_; const uint32_t requested=configVersion_;
    xSemaphoreGive(mutex_);
    bool closeNow=fd>=0&&(requested!=version||routeLost(localAddress,viaAp));
    if(fd<0&&hasRoute()&&rehab::due(millis(),lastAttempt,1000)) {
      lastAttempt=millis(); version=requested;
      addrinfo hints{}; hints.ai_family=AF_INET; hints.ai_socktype=SOCK_STREAM;
      addrinfo* address=nullptr;
      if(getaddrinfo(host.c_str(),"9999",&hints,&address)==0&&address) {
        fd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        if(fd>=0) {
          int result=-1;
          const int flags=fcntl(fd,F_GETFL,0);
          if(flags>=0&&fcntl(fd,F_SETFL,flags|O_NONBLOCK)==0) {
            result=::connect(fd,address->ai_addr,address->ai_addrlen);
            if(result<0&&errno==EINPROGRESS) {
              fd_set set; FD_ZERO(&set); FD_SET(fd,&set); timeval timeout{3,0};
              result=select(fd+1,nullptr,&set,nullptr,&timeout);
              int socketError=0; socklen_t length=sizeof(socketError);
              result=result>0&&getsockopt(fd,SOL_SOCKET,SO_ERROR,&socketError,&length)==0&&socketError==0?0:-1;
            }
          }
          sockaddr_in local{}; socklen_t localLength=sizeof(local);
          if(result==0&&getsockname(fd,reinterpret_cast<sockaddr*>(&local),&localLength)==0) {
            localAddress=local.sin_addr.s_addr;
            viaAp=localAddress==uint32_t(WiFi.softAPIP());
            int one=1; setsockopt(fd,SOL_SOCKET,SO_KEEPALIVE,&one,sizeof(one));
            setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
            int idleSeconds=10,intervalSeconds=5,count=3;
            setsockopt(fd,IPPROTO_TCP,TCP_KEEPIDLE,&idleSeconds,sizeof(idleSeconds));
            setsockopt(fd,IPPROTO_TCP,TCP_KEEPINTVL,&intervalSeconds,sizeof(intervalSeconds));
            setsockopt(fd,IPPROTO_TCP,TCP_KEEPCNT,&count,sizeof(count));
          } else {close(fd); fd=-1;}
        }
      }
      if(address) freeaddrinfo(address);
      xSemaphoreTake(mutex_,portMAX_DELAY);
      if(fd>=0&&requested==configVersion_&&!routeLost(localAddress,viaAp)) {
        connected_=true; ++generation_; ++reconnects_; error_=""; parser.reset();
        telemetry_.clear();
      } else {
        if(fd>=0) close(fd);
        fd=-1; error_="TCP 連線失敗，將重試";
      }
      xSemaphoreGive(mutex_);
    }
    if(fd>=0&&!closeNow) {
      auto dispatch=[&](const rehab::Frame& f) {
        uint8_t encoded[260]; const size_t size=rehab::encode(f,0x7a,0x7f,encoded);
        xSemaphoreTake(mutex_,portMAX_DELAY);
        rxHex_=hexBytes(encoded,size); rxAt_=millis(); hasRx_=true;
        if(f.command==0x20&&f.length==1) lastQuery_=rxHex_;
        if(awaiting&&pending.frame.command==0x21) {
          if(f.command==0x21&&f.length==1&&(f.data[0]==0xc8||f.data[0]==0xc9))
            cardResult_=f.data[0]==0xc8?"主機確認 UID":"主機拒絕 UID";
          else cardResult_="收到資料，並非明確 UID ACK";
        }
        // Legacy accepts any valid RX for its wait. Every control is still
        // delivered to the controller; an ACK wait never consumes a command.
        awaiting=false; waiting_=false;
        ReceivedFrame message{f,generation_};
        if(xQueueSend(rx_,&message,0)!=pdTRUE) {closeNow=true; error_="TCP 接收佇列已滿";}
        xSemaphoreGive(mutex_);
      };
      uint8_t incoming[512]; const int count=recv(fd,incoming,sizeof(incoming),0);
      if(count==0||(count<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)) closeNow=true;
      for(int i=0;i<count&&!closeNow;++i) {
        parser.feed(incoming[i],millis()); rehab::Frame f;
        while(parser.next(f)&&!closeNow) dispatch(f);
      }
      parser.expire(millis()); rehab::Frame recovered;
      while(!closeNow&&parser.next(recovered)) dispatch(recovered);
      if(!closeNow) {
        Outgoing next{}; rehab::TelemetryOutbox::Token token;
        bool isTelemetry=false,have=false;
        xSemaphoreTake(mutex_,portMAX_DELAY);
        have=xQueueReceive(urgent_,&next,0)==pdTRUE;
        if(!have&&!awaiting) have=xQueueReceive(tx_,&next,0)==pdTRUE;
        if(!have&&telemetry_.take(generation_,millis(),token)) {
          const uint8_t payload[]={token.selector,token.value};
          next={rehab::makeFrame(0x23,payload,2),generation_,false};
          have=isTelemetry=true;
        }
        if(have&&next.generation!=generation_) have=false;
        if(have) writing_=true; // Reserve before dequeue becomes visible to idle().
        xSemaphoreGive(mutex_);
        if(have) {
          const WriteResult result=writeFrame(fd,next.frame,isTelemetry?&token:nullptr);
          if(result==WriteResult::Failed) closeNow=true;
          else if(result==WriteResult::Sent&&next.ack) {
            pending=next; awaiting=true; attempts=1; ackAt=millis();
          }
          xSemaphoreTake(mutex_,portMAX_DELAY);
          writing_=false; waiting_=awaiting;
          xSemaphoreGive(mutex_);
        }
        if(!closeNow&&awaiting&&rehab::due(millis(),ackAt,500)) {
          if(attempts>=3) {
            awaiting=false;
            xSemaphoreTake(mutex_,portMAX_DELAY); ++timeouts_; waiting_=false;
            if(pending.frame.command==0x21) cardResult_="逾時未確認（不代表未送達）";
            xSemaphoreGive(mutex_);
          } else {
            xSemaphoreTake(mutex_,portMAX_DELAY); writing_=true; xSemaphoreGive(mutex_);
            if(writeFrame(fd,pending.frame)==WriteResult::Failed) closeNow=true;
            ++attempts; ackAt=millis();
            xSemaphoreTake(mutex_,portMAX_DELAY); writing_=false; xSemaphoreGive(mutex_);
          }
        }
      }
    }
    if(closeNow) {if(fd>=0) close(fd); fd=-1; lastAttempt=millis();}
    if(fd<0) {
      awaiting=false; parser.reset();
      xSemaphoreTake(mutex_,portMAX_DELAY);
      if(connected_) {
        error_="TCP 已中斷";
        if(cardResult_.startsWith("已寫入")) cardResult_="連線中斷，UID 未確認";
      }
      connected_=writing_=waiting_=false;
      // A producer cannot enqueue an old generation between these resets and
      // connected_=false: both use this same mutex.
      xQueueReset(rx_); xQueueReset(tx_); xQueueReset(urgent_); telemetry_.clear();
      xSemaphoreGive(mutex_);
    }
    xSemaphoreTake(mutex_,portMAX_DELAY);
    waiting_=awaiting; checksumErrors_=parser.checksumErrors;
    framingErrors_=parser.framingErrors; partialTimeouts_=parser.timeouts;
    xSemaphoreGive(mutex_);
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
void TcpService::appendStatus(cJSON* o,uint32_t now) {
  if(!mutex_) {jb(o,"connected",false); js(o,"last_error","TCP 尚未初始化"); return;}
  xSemaphoreTake(mutex_,portMAX_DELAY);
  now=millis();
  jb(o,"connected",connected_); js(o,"server",server_); jn(o,"port",board::serverPort); jn(o,"generation",generation_);
  jb(o,"writing",writing_); jb(o,"awaiting_ack",waiting_);
  jn(o,"queued_events",tx_?uxQueueMessagesWaiting(tx_):0);
  jn(o,"queued_replies",urgent_?uxQueueMessagesWaiting(urgent_):0);
  jb(o,"telemetry_pending",telemetry_.pending(generation_,now));
  jn(o,"reconnects",reconnects_); js(o,"last_error",error_); js(o,"rx_hex",rxHex_); js(o,"tx_hex",txHex_);
  jnullable(o,"rx_age_ms",now-rxAt_,hasRx_); jnullable(o,"tx_age_ms",now-txAt_,hasTx_);
  jn(o,"checksum_errors",checksumErrors_); jn(o,"framing_errors",framingErrors_); jn(o,"timeouts",timeouts_+partialTimeouts_);
  js(o,"last_query",lastQuery_); js(o,"last_reply",lastReply_); js(o,"card_result",cardResult_);
  cJSON* telemetry=cJSON_AddObjectToObject(o,"telemetry"); const char* names[]={"rpm","level","bpm"};
  for(int i=0;i<3;++i) {auto v=cJSON_AddObjectToObject(telemetry,names[i]); jnullable(v,"value",sent_[i],sent_[i]>=0); jnullable(v,"age_ms",now-sentAt_[i],sent_[i]>=0);}
  xSemaphoreGive(mutex_);
}
