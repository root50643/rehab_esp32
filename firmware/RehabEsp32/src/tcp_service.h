#pragma once
#include <Arduino.h>
#include "cJSON.h"
#include "protocol.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
struct ReceivedFrame { rehab::Frame frame; uint32_t generation; };
struct TcpSnapshot { bool connected=false; uint32_t generation=0; };
class TcpService {
  struct Outgoing { rehab::Frame frame; uint32_t generation; bool ack; };
  SemaphoreHandle_t mutex_=nullptr;
  QueueHandle_t rx_=nullptr,tx_=nullptr,urgent_=nullptr;
  rehab::TelemetryOutbox telemetry_;
  String server_,error_,rxHex_,txHex_,lastQuery_,lastReply_,cardResult_="尚未刷卡";
  uint32_t configVersion_=0,generation_=0,reconnects_=0,rxAt_=0,txAt_=0;
  uint32_t checksumErrors_=0,framingErrors_=0,timeouts_=0,partialTimeouts_=0;
  bool connected_=false,hasRx_=false,hasTx_=false,writing_=false,waiting_=false;
  int sent_[3]={-1,-1,-1}; uint32_t sentAt_[3]={};
  void run();
  enum class WriteResult { Sent, Discarded, Failed };
  WriteResult writeFrame(int fd,const rehab::Frame& frame,const rehab::TelemetryOutbox::Token* token=nullptr);
  static void task(void* self) { static_cast<TcpService*>(self)->run(); }
public:
  void begin(const String& server);
  void configure(const String& server);
  TcpSnapshot snapshot();
  bool receive(ReceivedFrame& message);
  bool send(const rehab::Frame& frame,bool ack=false,bool urgent=false,uint32_t generation=0);
  bool publishTelemetry(uint8_t selector,int value,uint32_t observedAt,uint32_t validForMs);
  void cancelTelemetry(uint8_t selector);
  void cancelTelemetryAll();
  bool idle();
  void appendStatus(cJSON* root,uint32_t now);
};
extern TcpService tcpService;
