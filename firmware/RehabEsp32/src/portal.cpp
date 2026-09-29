#include "portal.h"
#include "board_config.h"
#include "settings.h"
#include "settings_validation.h"
#include "controller.h"
#include "ble_service.h"
#include "ccid_service.h"
#include "tcp_service.h"
#include "json_helpers.h"
#include "web_assets.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <algorithm>

namespace {
IPAddress apIp(192,168,4,1),mask(255,255,255,0);
// Listen on both Wi-Fi interfaces; the AP remains available when STA changes IP.
WebServer web(80);
WiFiUDP dns;
String apName,wifiState="unconfigured",wifiError,scanState="idle",scanError;
String scanResults="[]";
IPAddress announcedStaIp;
Settings active;
uint32_t connectAt=0,retryAt=0,scanAt=0,scanPrepareAt=0,scanQueuedAt=0;
bool connecting=false,scanRequested=false,scanRunning=false,scanPreparing=false;
volatile uint8_t disconnectReason=0;
bool allowed() {
  const IPAddress local=web.client().localIP();
  const bool onAp=local==apIp;
  const bool onSta=WiFi.status()==WL_CONNECTED && local!=IPAddress() && local==WiFi.localIP();
  if(!onAp&&!onSta) {web.send(403,"text/plain; charset=utf-8","請透過裝置的設定 Wi-Fi 或目標 Wi-Fi 區域網路連線"); return false;}
  const String address=local.toString(),host=web.hostHeader();
  // Settings use literal interface IPs. Do not trust a matching but arbitrary
  // Host/Origin pair supplied through a rebound DNS name on the uplink LAN.
  if(host!=address && host!=address+":80") {
    if(onAp && web.method()==HTTP_GET) {
      web.sendHeader("Location","http://"+address+"/"); web.sendHeader("Cache-Control","no-store");
      web.send(302,"text/plain","Open configuration portal");
    } else web.send(403,"text/plain","Use the device IP address");
    return false;
  }
  String origin=web.header("Origin");
  if(web.method()!=HTTP_GET && origin.length() && origin!="http://"+address && origin!="http://"+address+":80") {
    web.send(403,"text/plain","Cross-origin request rejected"); return false;
  }
  return true;
}
void reply(cJSON* value,int code=200) {
  char* text=cJSON_PrintUnformatted(value);
  web.sendHeader("Cache-Control","no-store");
  web.sendHeader("X-Content-Type-Options","nosniff");
  if(text) {web.send(code,"application/json; charset=utf-8",text); cJSON_free(text);}
  else web.send(503,"application/json","{\"error\":\"記憶體不足\"}");
  cJSON_Delete(value);
}
void error(int code,const String& message) {auto o=cJSON_CreateObject(); jb(o,"ok",false); js(o,"error",message); reply(o,code);}
void configGet() {if(!allowed()) return; auto o=cJSON_CreateObject(); settingsStore.append(o); reply(o);}
void configPost() {
  if(!allowed()) return;
  String body=web.arg("plain");
  String contentType=web.header("Content-Type"); int separator=contentType.indexOf(';');
  if(separator>=0) contentType=contentType.substring(0,separator);
  contentType.trim(); contentType.toLowerCase();
  if(contentType!="application/json" || !rehab::boundedSettingsJson(body.c_str(),body.length())) {error(400,"請使用有效的 JSON 設定"); return;}
  const char* end=nullptr; cJSON* request=cJSON_ParseWithOpts(body.c_str(),&end,true);
  if(!request) {error(400,"JSON 格式錯誤"); return;}
  String message; int code=settingsStore.save(request,message); cJSON_Delete(request);
  if(code!=200) {error(code,message); return;}
  auto o=cJSON_CreateObject(); jb(o,"ok",true); settingsStore.append(o); reply(o);
  settingsStore.allowApply();
}
void statusGet() {
  if(!allowed()) return; uint32_t now=millis(); auto o=cJSON_CreateObject();
  js(o,"firmware",board::firmwareVersion); jn(o,"uptime_ms",now); jb(o,"pending",settingsStore.pending());
  auto ap=cJSON_AddObjectToObject(o,"ap"); js(ap,"ssid",apName); js(ap,"ip",apIp.toString()); jn(ap,"clients",WiFi.softAPgetStationNum());
  auto wifi=cJSON_AddObjectToObject(o,"wifi"); js(wifi,"state",wifiState); js(wifi,"ssid",active.ssid);
  js(wifi,"ip",WiFi.status()==WL_CONNECTED?WiFi.localIP().toString():"");
  jnullable(wifi,"rssi",WiFi.RSSI(),WiFi.status()==WL_CONNECTED); js(wifi,"last_error",wifiError);
  tcpService.appendStatus(cJSON_AddObjectToObject(o,"tcp"),now);
  bleService.appendStatus(cJSON_AddObjectToObject(o,"ble"),now);
  controller.appendStatus(cJSON_AddObjectToObject(o,"machine"),now);
  ccidService.appendStatus(cJSON_AddObjectToObject(o,"rfid"),now);
  auto health=cJSON_AddObjectToObject(o,"health"); jn(health,"free_heap",ESP.getFreeHeap()); jn(health,"min_free_heap",ESP.getMinFreeHeap());
  reply(o);
}
void wifiScanGet() {
  if(!allowed()) return; auto o=cJSON_CreateObject(); js(o,"state",scanState); js(o,"error",scanError);
  cJSON* results=cJSON_Parse(scanResults.c_str()); cJSON_AddItemToObject(o,"results",results?results:cJSON_CreateArray()); reply(o);
}
void bleScanGet() {if(!allowed()) return; auto o=cJSON_CreateObject(); bleService.appendScan(o); reply(o);}
void asset(const char* mime,const uint8_t* bytes,size_t length) {
  if(!allowed()) return;
  web.sendHeader("Cache-Control","no-cache"); web.sendHeader("X-Content-Type-Options","nosniff");
  web.sendHeader("Content-Security-Policy","default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self' data:; frame-ancestors 'none'");
  web.send_P(200,mime,reinterpret_cast<const char*>(bytes),length);
}
// Bind DNS to the AP address, so the uplink LAN is never given captive answers.
void dnsTick() {
  for(int budget=0;budget<4;++budget) {
    int n=dns.parsePacket(); if(!n) break;
    uint8_t packet[512]; int length=dns.read(packet,sizeof(packet));
    // parsePacket() will not accept another packet while unread UDP bytes
    // remain; oversized input must therefore be drained as well as rejected.
    dns.clear();
    if(length<12 || n>int(sizeof(packet)) || length!=n) continue;
    const uint8_t ip[]={apIp[0],apIp[1],apIp[2],apIp[3]};
    const size_t responseLength=rehab::captiveDnsResponse(packet,length,sizeof(packet),ip);
    if(!responseLength) continue;
    dns.beginPacket(dns.remoteIP(),dns.remotePort()); dns.write(packet,responseLength); dns.endPacket();
  }
}
void networkTick() {
  uint32_t now=millis(); auto latest=settingsStore.active();
  if(latest.revision!=active.revision) {
    bool changed=strcmp(active.ssid,latest.ssid)||strcmp(active.password,latest.password);
    active=latest;
    if(changed) {
      if(scanRunning) {
        esp_wifi_scan_stop(); WiFi.scanDelete(); scanRunning=false;
        scanState="error"; scanError="設定已變更，請重新掃描";
        bleService.setScanAllowed(!scanRequested);
      }
      scanPreparing=false;
      WiFi.disconnect(false,false); connecting=false; retryAt=now-5000;
    }
  }
  if(scanRequested) {
    bleService.setScanAllowed(false);
    if(!bleService.scanBusy()) {
      if(connecting) {
        WiFi.disconnect(false,false); connecting=false;
        scanPreparing=true; scanPrepareAt=now;
      }
      // A disconnect request is asynchronous. Let the connection attempt
      // finish stopping before asking the driver to start an explicit scan.
      if(!scanPreparing || rehab::due(now,scanPrepareAt,100)) {
        int result=WiFi.scanNetworks(true,true,false,120);
        if(result==WIFI_SCAN_RUNNING) {
          scanRunning=true; scanAt=now; scanState="scanning"; scanRequested=false; scanPreparing=false;
        } else if(rehab::due(now,scanQueuedAt,3000)) {
          scanState="error"; scanError="Wi-Fi 掃描無法啟動";
          scanRequested=scanPreparing=false; bleService.setScanAllowed(true); retryAt=now;
        } else {scanPreparing=true; scanPrepareAt=now;}
      }
    } else if(rehab::due(now,scanQueuedAt,10000)) {
      scanState="error"; scanError="Wi-Fi 掃描等待逾時，請重試";
      scanRequested=scanPreparing=false; bleService.setScanAllowed(true); retryAt=now;
    }
  }
  if(scanRunning) {
    int count=WiFi.scanComplete();
    if(count>=0) {
      struct Candidate {String name; int rssi=0,channel=0; bool secure=false;};
      Candidate candidates[32]; size_t total=0;
      for(int i=0;i<count;++i) {
        String name=WiFi.SSID(i); if(name.isEmpty()) continue;
        int slot=-1; const int strength=WiFi.RSSI(i);
        for(size_t j=0;j<total;++j) if(candidates[j].name==name) {slot=int(j); break;}
        if(slot>=0 && candidates[slot].rssi>=strength) continue;
        if(slot<0) {
          if(total<32) slot=int(total++);
          else {
            slot=0;
            for(size_t j=1;j<total;++j) if(candidates[j].rssi<candidates[slot].rssi) slot=int(j);
            if(candidates[slot].rssi>=strength) continue;
          }
        }
        candidates[slot]={name,strength,WiFi.channel(i),WiFi.encryptionType(i)!=WIFI_AUTH_OPEN};
      }
      std::sort(candidates,candidates+total,[](const Candidate& a,const Candidate& b){return a.rssi>b.rssi;});
      auto results=cJSON_CreateArray();
      for(size_t i=0;i<total;++i) {
        auto entry=cJSON_CreateObject(); js(entry,"ssid",candidates[i].name); jn(entry,"rssi",candidates[i].rssi);
        jn(entry,"channel",candidates[i].channel); jb(entry,"secure",candidates[i].secure); cJSON_AddItemToArray(results,entry);
      }
      char* data=cJSON_PrintUnformatted(results); scanResults=data?data:"[]"; cJSON_free(data); cJSON_Delete(results);
      WiFi.scanDelete(); scanState="done"; scanRunning=false; bleService.setScanAllowed(true); retryAt=now-5000;
    } else if(count==WIFI_SCAN_FAILED||rehab::due(now,scanAt,15000)) {
      esp_wifi_scan_stop(); WiFi.scanDelete(); scanState="error"; scanError="Wi-Fi 掃描失敗或逾時";
      scanRunning=false; bleService.setScanAllowed(true); retryAt=now;
    }
  }
  if(WiFi.status()!=WL_CONNECTED) announcedStaIp=IPAddress();
  if(WiFi.status()==WL_CONNECTED) {
    wifiState="connected"; connecting=false; wifiError="";
    const IPAddress ip=WiFi.localIP();
    if(ip!=IPAddress() && ip!=announcedStaIp) {
      Serial.printf("LAN portal: http://%s/\n",ip.toString().c_str());
      announcedStaIp=ip;
    }
  }
  else if(!active.ssid[0]) wifiState="unconfigured";
  else if(scanRunning||scanRequested) wifiState="scanning";
  else if(connecting) {
    wifiState="connecting";
    if(rehab::due(now,connectAt,15000)) {
      WiFi.disconnect(false,false); connecting=false; retryAt=now;
      wifiError="Wi-Fi 連線失敗，請檢查 2.4GHz 網路及密碼（原因 "+String(disconnectReason)+"）";
    }
  } else {
    wifiState="retrying";
    if(rehab::due(now,retryAt,5000)) {
      WiFi.begin(active.ssid,active.password); connectAt=now; connecting=true;
    }
  }
}
}
void portalBegin() {
  active=settingsStore.active(); retryAt=millis()-5000;
  WiFi.persistent(false); WiFi.mode(WIFI_AP_STA); WiFi.setAutoReconnect(false);
  // AP netif may not be started yet: softAPmacAddress() can return all zeroes
  // here. Read the hardware-derived AP MAC directly before forming the SSID.
  uint8_t mac[6];
  ESP_ERROR_CHECK(esp_read_mac(mac,ESP_MAC_WIFI_SOFTAP));
  char apSsid[29];
  snprintf(apSsid,sizeof(apSsid),"RehabSetup_%02X:%02X:%02X:%02X:%02X:%02X",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  apName=apSsid;
  WiFi.softAPConfig(apIp,apIp,mask); WiFi.softAP(apName.c_str(),nullptr,1,false,4);
  WiFi.AP.enableDhcpCaptivePortal();
  WiFi.onEvent([](arduino_event_id_t event,arduino_event_info_t info) {
    if(event==ARDUINO_EVENT_WIFI_STA_DISCONNECTED) disconnectReason=info.wifi_sta_disconnected.reason;
  });
  dns.begin(apIp,53);
  const char* headers[]={"Origin","Content-Type"}; web.collectHeaders(headers,2);
  web.on("/",HTTP_GET,[]{asset("text/html; charset=utf-8",web_index_html,web_index_html_len);});
  web.on("/style.css",HTTP_GET,[]{asset("text/css; charset=utf-8",web_style_css,web_style_css_len);});
  web.on("/app.js",HTTP_GET,[]{asset("application/javascript; charset=utf-8",web_app_js,web_app_js_len);});
  web.on("/api/status",HTTP_GET,statusGet); web.on("/api/config",HTTP_GET,configGet); web.on("/api/config",HTTP_POST,configPost);
  web.on("/api/scan/wifi",HTTP_GET,wifiScanGet);
  web.on("/api/scan/wifi",HTTP_POST,[]{if(!allowed()) return; if(!scanRequested&&!scanRunning) {scanRequested=true; scanPreparing=false; scanQueuedAt=millis(); scanState="queued"; scanError="";} wifiScanGet();});
  web.on("/api/scan/ble",HTTP_GET,bleScanGet);
  web.on("/api/scan/ble",HTTP_POST,[]{if(!allowed()) return; bleService.requestScan(); bleScanGet();});
  web.onNotFound([]{
    if(!allowed()) return;
    if(web.uri().startsWith("/api/")) {error(404,"找不到 API"); return;}
    // Captive redirects belong to the setup network only. A LAN client must
    // never be sent to an AP-only address that it cannot route to.
    if(web.client().localIP()!=apIp) {web.send(404,"text/plain; charset=utf-8","找不到頁面，請開啟此裝置 IP 的首頁 /"); return;}
    web.sendHeader("Location","http://192.168.4.1/"); web.sendHeader("Cache-Control","no-store"); web.send(302,"text/plain","Open configuration portal");
  });
  web.begin();
  Serial.printf("Rehab ESP32 %s ready\nAP: %s\nPortal: http://192.168.4.1/\n",board::firmwareVersion,apName.c_str());
}
void portalTick() {networkTick(); dnsTick(); web.handleClient();}
