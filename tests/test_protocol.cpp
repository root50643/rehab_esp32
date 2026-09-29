#include "../firmware/RehabEsp32/src/protocol.h"
#include <assert.h>
#include <iostream>
#include <vector>

using Bytes=std::vector<uint8_t>;
Bytes hex(const char* text) {
  Bytes bytes;
  auto digit=[](char c) {return c>='0'&&c<='9'?c-'0':c-'A'+10;};
  for(size_t i=0;text[i];i+=2) bytes.push_back(uint8_t(digit(text[i])*16+digit(text[i+1])));
  return bytes;
}
std::vector<rehab::Frame> feed(rehab::Parser& parser,const Bytes& bytes,uint32_t now) {
  std::vector<rehab::Frame> result;
  for(uint8_t byte:bytes) {
    parser.feed(byte,now); rehab::Frame frame;
    while(parser.next(frame)) result.push_back(frame);
  }
  return result;
}
void same(const rehab::Frame& a,const rehab::Frame& b) {
  assert(a.command==b.command&&a.length==b.length);
  assert(memcmp(a.data,b.data,a.length)==0);
}
void goldens() {
  struct Case {uint8_t command; const char* payload; const char* wire;};
  const Case cases[]={
    {0x21,"01234567","7A2104012345670A7F"}, {0x21,"C8","7A2101C8157F"},
    {0x21,"C9","7A2101C9147F"}, {0x23,"003C","7A2302003C9E7F"},
    {0x23,"010A","7A2302010ACF7F"}, {0x23,"0250","7A23020250887F"},
    {0x24,"00","7A240100DA7F"}, {0x20,"00","7A200100DE7F"},
    {0x20,"00C8","7A200200C8157F"}, {0x20,"02C9","7A200202C9127F"},
    {0x22,"01","7A220101DB7F"}, {0x22,"01C8","7A220201C8127F"},
    {0x25,"0A","7A25010ACF7F"}, {0x25,"000A","7A2502000ACE7F"},
    {0x25,"C8","7A2501C8117F"}
  };
  for(const auto& test:cases) {
    const auto payload=hex(test.payload),wire=hex(test.wire);
    const auto frame=rehab::makeFrame(test.command,payload.data(),payload.size());
    uint8_t encoded[260];
    assert(rehab::encode(frame,0x7A,0x7F,encoded)==wire.size());
    assert(memcmp(encoded,wire.data(),wire.size())==0);
    assert(rehab::encode(frame,0x7A,0x7F,encoded,wire.size()-1)==0);
    for(size_t split=0;split<=wire.size();++split) {
      rehab::Parser parser(0x7A,0x7F);
      auto first=feed(parser,Bytes(wire.begin(),wire.begin()+split),0);
      auto second=feed(parser,Bytes(wire.begin()+split,wire.end()),50);
      first.insert(first.end(),second.begin(),second.end());
      assert(first.size()==1); same(first[0],frame);
      assert(!parser.checksumErrors&&!parser.framingErrors&&!parser.timeouts);
    }
  }
  // UART uses identical checksum math but different sentinels.
  rehab::Parser uart(0x55,0x90);
  auto frames=feed(uart,hex("554404000A0000AD90"),0);
  assert(frames.size()==1&&frames[0].command==0x44);
  assert(rehab::be16(frames[0].data)==10&&frames[0].length==4);
}
void streams() {
  uint8_t payload[255];
  for(size_t i=0;i<255;++i) payload[i]=uint8_t(i);
  auto maximum=rehab::makeFrame(0x21,payload,255);
  uint8_t wire[260]; assert(rehab::encode(maximum,0x7A,0x7F,wire)==260);
  rehab::Parser parser(0x7A,0x7F);
  auto frames=feed(parser,Bytes(wire,wire+260),0);
  assert(frames.size()==1); same(frames[0],maximum);
  // Payload includes both sentinels and zero; coalesced frames stay separate.
  Bytes combined=hex("7A2104007A7F00E17F7A2501C8117F");
  frames=feed(parser,combined,10);
  assert(frames.size()==2&&frames[0].length==4&&frames[0].data[0]==0);
  assert(frames[0].data[1]==0x7A&&frames[0].data[2]==0x7F);
  assert(frames[1].command==0x25&&frames[1].data[0]==200);
  assert(feed(parser,hex("0102037A200100DF7F7A200100DE007A200100DE7F"),20).size()==1);
  assert(parser.checksumErrors==1&&parser.framingErrors==1);

  rehab::Parser blocked(0x7A,0x7F);
  assert(feed(blocked,hex("7A99FF7A200102DC7F"),0).empty());
  blocked.expire(1999); rehab::Frame recovered; assert(!blocked.next(recovered));
  blocked.expire(2000); assert(blocked.next(recovered));
  assert(recovered.command==0x20&&recovered.data[0]==2&&blocked.timeouts==1);
  assert(!blocked.next(recovered));
  // New bytes arriving exactly at the old fragment's deadline are not lost.
  rehab::Parser timed(0x7A,0x7F);
  feed(timed,hex("7A2302"),0);
  frames=feed(timed,hex("7A200100DE7F"),2000);
  assert(frames.size()==1&&timed.timeouts==1);
  // Reset prevents a fragment from one TCP generation completing in another.
  feed(timed,hex("7A2001"),2100); timed.reset();
  assert(feed(timed,hex("00DE7F"),2101).empty());
  assert(feed(timed,hex("7A200100DE7F"),2102).size()==1);
  rehab::Parser wrapping(0x7A,0x7F);
  feed(wrapping,hex("7A2001"),UINT32_MAX-100);
  assert(feed(wrapping,hex("00DE7F"),50).size()==1);
  assert(wrapping.timeouts==0);
}
int main() {
  goldens(); streams();
  std::cout<<"Protocol golden, fragmentation, bounds, resync and rollover tests passed\n";
}
