#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
namespace rehab {
inline bool validMac(const char* s) {
  if (!s) return false;
  if (!*s) return true; // Explicitly disable the BLE target.
  if (strlen(s) != 17) return false;
  for (size_t i=0;i<17;++i) if (i%3 == 2 ? s[i] != ':' : !isxdigit((unsigned char)s[i])) return false;
  return true;
}
inline bool validPassword(const char* p) {
  if (!p) return false;
  size_t n = strlen(p);
  if (!n || (n >= 8 && n <= 63)) return true;
  if (n != 64) return false;
  for (size_t i=0;i<n;++i) if (!isxdigit((unsigned char)p[i])) return false;
  return true;
}
inline bool validHost(const char* h) {
  if (!h) return false;
  size_t n=strlen(h); if (!n || n>253) return false;
  bool numeric=true;
  for(size_t i=0;i<n;++i) if(h[i]!='.' && !isdigit((unsigned char)h[i])) numeric=false;
  if(numeric) {
    int parts=0, value=0, digits=0;
    for(size_t i=0;i<=n;++i) {
      if(h[i]=='.' || !h[i]) { if(!digits || value>255 || digits>3) return false; ++parts; value=digits=0; }
      else {if(digits==3) return false; value=value*10+h[i]-'0'; ++digits;}
    }
    return parts==4;
  }
  size_t label=0;
  for(size_t i=0;i<=n;++i) {
    unsigned char c=h[i];
    if(c=='.'||!c) { if(!label || label>63 || h[i-1]=='-') return false; label=0; }
    else { if(!(isalnum(c)&&c<128) && c!='-') return false; if(!label && c=='-') return false; ++label; }
  }
  return true;
}
inline bool validRevision(double revision) {
  return revision >= 1 && revision <= UINT32_MAX && uint32_t(revision) == revision;
}

// The settings schema is flat. Reject deep input before cJSON's recursive
// parser, and distinguish a real Unicode NUL escape from a literal "\\u0000".
inline bool boundedSettingsJson(const char* text,size_t length) {
  if(!text||!length||length>2048) return false;
  bool quoted=false,escaped=false;
  int depth=0;
  for(size_t i=0;i<length;++i) {
    const char c=text[i];
    if(!c) return false;
    if(quoted) {
      if(escaped) {
        escaped=false;
        if(c=='u'&&i+4<length&&!memcmp(text+i+1,"0000",4)) return false;
      } else if(c=='\\') escaped=true;
      else if(c=='"') quoted=false;
    } else if(c=='"') quoted=true;
    else if(c=='['||c==']') return false;
    else if(c=='{') {if(++depth>1) return false;}
    else if(c=='}') {if(--depth<0) return false;}
  }
  return !quoted&&depth==0;
}

// Kept portable so malformed captive-DNS packets can be tested without Wi-Fi.
// Only a single, uncompressed standard question is accepted. Additional query
// records (e.g. EDNS) are not copied into the response.
inline size_t captiveDnsResponse(uint8_t* packet,size_t length,size_t capacity,const uint8_t ip[4]) {
  if(!packet||!ip||length<12||length>capacity||(packet[2]&0xf8)||packet[4]||packet[5]!=1) return 0;
  size_t end=12;
  while(end<length&&packet[end]) {
    size_t label=packet[end];
    if(label>63||end+label+1>=length||end+label+1-12>=255) return 0;
    end+=label+1;
  }
  if(end>=length||end+5>length||end+1-12>255) return 0;
  ++end;
  const uint16_t type=uint16_t(packet[end])<<8|packet[end+1];
  const uint16_t cls=uint16_t(packet[end+2])<<8|packet[end+3];
  end+=4;
  const bool wantsAnswer=type==1&&cls==1;
  const bool fits=capacity-end>=16;
  const bool answer=wantsAnswer&&fits;
  packet[2]=0x80|(packet[2]&1)|((wantsAnswer&&!fits)?2:0);
  packet[3]=0x80;
  packet[6]=0; packet[7]=answer?1:0;
  memset(packet+8,0,4);
  if(answer) {
    const uint8_t record[]={0xc0,0x0c,0,1,0,1,0,0,0,30,0,4};
    memcpy(packet+end,record,sizeof record); end+=sizeof record;
    memcpy(packet+end,ip,4); end+=4;
  }
  return end;
}
}
