#include "../firmware/RehabEsp32/src/protocol.h"
#include <assert.h>
#include <iostream>

void pulsesAndQueues() {
  rehab::Pulse pulse;
  assert(!pulse.tick(1000));
  pulse.start(rehab::PulseKind::Start,100,7);
  assert(pulse.high&&!pulse.tick(599));
  assert(pulse.tick(600)&&!pulse.high&&pulse.generation==7);
  assert(!pulse.tick(601)); // Completion is delivered only once.
  pulse.start(rehab::PulseKind::Stop,1000,8);
  assert(!pulse.tick(3999)&&pulse.high);
  assert(!pulse.tick(4000)&&!pulse.high); // 500 ms settling remains.
  assert(!pulse.tick(4499));
  assert(pulse.tick(4500)&&!pulse.high);
  assert(!pulse.tick(4501));
  pulse.start(rehab::PulseKind::Start,UINT32_MAX-100,9);
  assert(!pulse.tick(398)&&pulse.high);
  assert(pulse.tick(399)&&!pulse.high);

  assert(!rehab::canApplyConfig(true,rehab::PulseKind::None,0));
  assert(!rehab::canApplyConfig(false,rehab::PulseKind::Stop,0));
  assert(!rehab::canApplyConfig(false,rehab::PulseKind::Start,0));
  assert(!rehab::canApplyConfig(false,rehab::PulseKind::None,1));
  assert(rehab::canApplyConfig(false,rehab::PulseKind::None,0));

  rehab::Fifo<int,3> fifo;
  assert(fifo.push(1)&&fifo.push(2)&&fifo.push(3)&&!fifo.push(4));
  int value=-1; assert(fifo.pop(value)&&value==1);
  assert(fifo.push(4));
  for(int expected:{2,3,4}) assert(fifo.pop(value)&&value==expected);
  assert(!fifo.peek()&&!fifo.pop(value));
  fifo.push(5); fifo.clear(); assert(fifo.size()==0&&!fifo.pop(value));
}
void machineValues() {
  rehab::MachineValues values;
  const uint8_t data28[]={0x01,0x02,0x3B,0x03,0x04,0x05,0x06,0x50,0x01,0x00,0x07,0x08};
  const auto sample=rehab::makeFrame(0x28,data28,sizeof(data28));
  assert(values.accept(sample,1234)&&values.has28&&!values.has3f);
  assert(values.minutes==258&&values.seconds==59&&values.distance==772);
  assert(values.calories==1286&&values.bpm==80&&values.rpm==256&&values.watt28==1800);
  assert(values.at28==1234);
  const uint8_t data3f[]={0xFF,0xFF,0,0x20,0,0x30};
  assert(values.accept(rehab::makeFrame(0x3F,data3f,6),1500));
  assert(values.level==65535&&values.watt3f==32&&values.torque==48&&values.at3f==1500);
  // Malformed reports must not partially mutate either source's values.
  for(size_t size=0;size<sizeof(data28);++size)
    assert(!values.accept(rehab::makeFrame(0x28,data28,size),2000));
  assert(values.rpm==256&&values.at28==1234&&values.level==65535);
  for(size_t size=0;size<6;++size)
    assert(!values.accept(rehab::makeFrame(0x3F,data3f,size),2001));
  assert(values.level==65535&&values.at3f==1500);
}
void telemetry() {
  rehab::TelemetryOutbox box;
  rehab::TelemetryOutbox::Token token;
  assert(!box.take(1,0,token));
  assert(!box.publish(3,10,1,0,1000));
  assert(!box.publish(0,256,1,0,1000));
  assert(!box.publish(0,-1,1,0,1000));
  assert(!box.publish(2,0,1,0,1000));
  assert(!box.publish(1,10,1,0,0));
  assert(box.publish(0,60,1,100,5000));
  assert(box.publish(0,61,1,101,5000));
  assert(box.take(1,102,token)&&token.value==61); // One latest sample, no backlog.
  assert(!box.pending(1,102)&&box.current(token,1,102));
  const auto selected=token;
  assert(box.publish(0,62,1,103,5000));
  assert(!box.current(selected,1,103)); // Changed after worker selected a packet.
  assert(box.take(1,104,token)&&token.value==62);
  box.cancel(0); assert(!box.current(token,1,105));
  assert(box.publish(2,72,1,100,10000));
  assert(box.take(1,101,token));
  box.cancel(2); // BLE reset after selection but before the socket write.
  assert(!box.current(token,1,101)&&!box.pending(1,101));
  assert(box.publish(2,72,1,200,10000));
  assert(box.take(1,201,token)&&box.current(token,1,201));
  assert(!box.current(token,2,201)); // Never send a former connection's sample.
  assert(!box.current(token,1,10200)); // Observation-time deadline is exact.
  assert(box.publish(0,1,1,300,5000));
  box.clear(); assert(!box.take(1,301,token));
  assert(box.publish(1,3,1,400,5000));
  assert(!box.take(2,401,token));
  assert(box.publish(0,60,2,500,5000)&&box.publish(1,4,2,500,5000)&&box.publish(2,73,2,500,10000));
  unsigned selectors=0;
  for(int i=0;i<3;++i) {assert(box.take(2,501,token)); selectors|=1u<<token.selector;}
  assert(selectors==7&&!box.take(2,501,token));
  assert(box.publish(0,80,2,UINT32_MAX-99,1000));
  assert(box.take(2,0,token)&&box.current(token,2,899)&&!box.current(token,2,900));
}
int main() {
  pulsesAndQueues(); machineValues(); telemetry();
  std::cout<<"Pulse, FIFO, UART values, configuration gate and telemetry freshness tests passed\n";
}
