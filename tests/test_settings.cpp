#include "../firmware/RehabEsp32/src/settings_validation.h"
#include <cassert>
#include <iostream>
#include <array>
#include <limits>
#include <string>

static size_t dnsQuery(std::array<uint8_t,512>& packet,uint16_t type=1) {
  packet.fill(0); packet[0]=0x12; packet[1]=0x34; packet[2]=1; packet[5]=1;
  const uint8_t question[]={3,'a','p','p',4,'t','e','s','t',0,0,0,0,1};
  memcpy(packet.data()+12,question,sizeof question);
  packet[12+sizeof question-4]=uint8_t(type>>8); packet[12+sizeof question-3]=uint8_t(type);
  return 12+sizeof question;
}
int main() {
  using namespace rehab;
  assert(validMac("")); assert(validMac("AA:BB:CC:DD:EE:FF")); assert(validMac("aa:bb:cc:dd:ee:ff"));
  assert(!validMac("AA:BB:CC:DD:EE")); assert(!validMac("AA:BB:CC:DD:EE:GG")); assert(!validMac("AA-BB-CC-DD-EE-FF"));
  assert(validPassword("")); assert(validPassword("test-pass-123")); assert(!validPassword("short"));
  assert(validPassword("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
  assert(!validPassword("x123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
  assert(validHost("192.168.0.100")); assert(validHost("rehab.test")); assert(validHost("server-01.local"));
  for(auto h:{"","http://host","host:9999","256.1.1.1","1.2.3","1..2.3","-bad.test","bad-.test","x..test","bad/host","x\nheader"}) assert(!validHost(h));
  assert(!validMac(nullptr) && !validPassword(nullptr) && !validHost(nullptr));
  assert(!validHost(std::string(253,'9').c_str())); // Reject before integer overflow.
  assert(!validHost((std::string(64,'a')+".test").c_str()));
  assert(validHost((std::string(63,'a')+".test").c_str()));
  assert(validRevision(1) && validRevision(UINT32_MAX));
  assert(!validRevision(0) && !validRevision(-1) && !validRevision(1.5));
  assert(!validRevision(double(UINT32_MAX)+1));
  assert(!validRevision(std::numeric_limits<double>::infinity()));
  assert(!validRevision(std::numeric_limits<double>::quiet_NaN()));
  auto bounded=[](const char* text){return boundedSettingsJson(text,strlen(text));};
  assert(bounded("{\"wifi_ssid\":\"x\",\"revision\":1}"));
  assert(bounded(R"({"wifi_password":"literal\\u0000"})"));
  assert(bounded(R"({"wifi_password":"[{}] \\\" ok"})"));
  assert(!bounded(R"({"wifi_password":"null\u0000tail"})"));
  assert(!bounded(R"({"wifi_password":"slash\\\u0000tail"})"));
  assert(!bounded(R"({"nested":{"deep":1}})"));
  assert(!bounded(R"({"nested":[1]})"));
  const char embeddedNull[]={'{','}',0,'{','}'};
  assert(!boundedSettingsJson(embeddedNull,sizeof embeddedNull));
  assert(!boundedSettingsJson(nullptr,1) && !boundedSettingsJson("",0));
  const std::string oversized(2049,' ');
  assert(!boundedSettingsJson(oversized.c_str(),oversized.size()));

  const uint8_t ap[]={192,168,4,1};
  std::array<uint8_t,512> packet;
  size_t question=dnsQuery(packet);
  size_t response=captiveDnsResponse(packet.data(),question,packet.size(),ap);
  assert(response==question+16 && packet[0]==0x12 && packet[1]==0x34);
  assert(packet[2]==0x81 && packet[7]==1 && !memcmp(packet.data()+response-4,ap,4));
  assert(packet[question]==0xc0 && packet[question+1]==0x0c);
  question=dnsQuery(packet,28); // AAAA has a valid empty answer; no invented IPv6 address.
  assert(captiveDnsResponse(packet.data(),question,packet.size(),ap)==question && packet[7]==0);
  question=dnsQuery(packet);
  assert(captiveDnsResponse(packet.data(),question,question,ap)==question);
  assert((packet[2]&2) && packet[7]==0); // Small reply capacity: TC, never a missing claimed answer.
  question=dnsQuery(packet); packet[2]=0;
  assert(captiveDnsResponse(packet.data(),question,packet.size(),ap)>0 && (packet[2]&1)==0);
  for(size_t length=0;length<question;++length) {
    dnsQuery(packet); assert(captiveDnsResponse(packet.data(),length,packet.size(),ap)==0);
  }
  dnsQuery(packet); packet[2]=0x80; assert(captiveDnsResponse(packet.data(),question,packet.size(),ap)==0);
  dnsQuery(packet); packet[2]=0x08; assert(captiveDnsResponse(packet.data(),question,packet.size(),ap)==0);
  dnsQuery(packet); packet[5]=2; assert(captiveDnsResponse(packet.data(),question,packet.size(),ap)==0);
  dnsQuery(packet); packet[12]=0xc0; assert(captiveDnsResponse(packet.data(),question,packet.size(),ap)==0);
  dnsQuery(packet); packet[12]=63; assert(captiveDnsResponse(packet.data(),question,packet.size(),ap)==0);
  dnsQuery(packet); assert(captiveDnsResponse(packet.data(),513,packet.size(),ap)==0);
  assert(captiveDnsResponse(nullptr,0,0,ap)==0);
  dnsQuery(packet); packet[11]=1; // Discard optional additional records instead of echoing arbitrary bytes.
  packet[question]=0x42;
  assert(captiveDnsResponse(packet.data(),question+1,packet.size(),ap)==question+16 && packet[11]==0);
  packet.fill(0); packet[5]=1;
  size_t end=12;
  for(int i=0;i<4;++i) {packet[end++]=63; memset(packet.data()+end,'a',63); end+=63;}
  packet[end++]=0; packet[end++]=0; packet[end++]=1; packet[end++]=0; packet[end++]=1;
  assert(captiveDnsResponse(packet.data(),end,packet.size(),ap)==0); // Domain wire size exceeds 255.

  std::cout << "Settings and captive DNS validation cases passed\n";
}
