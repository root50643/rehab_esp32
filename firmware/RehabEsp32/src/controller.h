#pragma once
#include <Arduino.h>
#include "protocol.h"
#include "cJSON.h"
#include "freertos/semphr.h"
class Controller {
  struct Action { rehab::PulseKind kind; uint32_t generation; };
  SemaphoreHandle_t mutex_=nullptr;
  rehab::Parser uart_{0x55,0x90}; rehab::MachineValues values_;
  rehab::Pulse pulse_; rehab::Fifo<Action,8> actions_; rehab::Fifo<rehab::Frame,32> uartReplies_;
  rehab::Frame pulseAck_; bool pulseAckPending_=false; uint32_t pulseAckGeneration_=0;
  bool tracking_=false,hadUart_=false,hadAnyByte_=false,levelPending_=false,levelQueued_=false,seenConnection_=false;
  uint8_t machineState_=3,levelChecksum_=0;
  int targetLevel_=-1,sentLevel_=-1,seenRpm_=-1,seenLevel_=-1,seenBpm_=-1,reportedState_=-1;
  uint32_t uartAt_=0,lastByte_=0,missingTick_=0,emptyReads_=0,levelAt_=0,stateAt_=0;
  uint32_t generation_=0,timeouts_=0,bleDisconnectReported_=0;
  String rxHex_,txHex_,error_;
  void run(); void tick(uint32_t now); void command(const rehab::Frame& f,uint32_t gen,uint32_t now);
  void machine(const rehab::Frame& f,uint32_t now); bool uartSend(const rehab::Frame& f);
  bool uartWrite(const rehab::Frame& f); void flushUart(uint32_t now);
  bool schedule(rehab::PulseKind kind,uint32_t gen); void abnormalStop();
  static void task(void* self) { static_cast<Controller*>(self)->run(); }
public:
  void begin(); void appendStatus(cJSON* object,uint32_t now);
};
extern Controller controller;
