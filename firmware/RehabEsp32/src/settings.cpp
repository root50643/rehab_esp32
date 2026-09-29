#include "settings.h"
#include "settings_validation.h"
#include "board_config.h"
#include "json_helpers.h"
#include "esp_crc.h"
SettingsStore settingsStore;
namespace {
struct Record { uint32_t magic = 0x52534231; uint32_t version = 1; Settings value; uint32_t crc = 0; };
uint32_t crc(const Settings& s) { return esp_crc32_le(0, reinterpret_cast<const uint8_t*>(&s), sizeof(s)); }
bool terminated(const Settings& s) {
  return memchr(s.ssid,0,sizeof(s.ssid)) && memchr(s.password,0,sizeof(s.password)) &&
    memchr(s.server,0,sizeof(s.server)) && memchr(s.heartAddress,0,sizeof(s.heartAddress));
}
void jsonSettings(cJSON* o,const Settings& s,bool includePasswordFlag) {
  js(o,"wifi_ssid",s.ssid); js(o,"socket_server",s.server); js(o,"heart_rate_address",s.heartAddress);
  if(includePasswordFlag) jb(o,"has_password",s.password[0]);
}
}
void SettingsStore::begin() {
  if(mutex_) return;
  strlcpy(saved_.server,board::defaultServer,sizeof(saved_.server));
  strlcpy(saved_.heartAddress,board::defaultHeartAddress,sizeof(saved_.heartAddress));
  mutex_=xSemaphoreCreateMutex();
  if(!mutex_) {active_=saved_; return;}
  healthy_=prefs_.begin("rehab",false);
  Record record;
  if(healthy_ && prefs_.getBytesLength("settings")==sizeof(record) && prefs_.getBytes("settings",&record,sizeof(record))==sizeof(record)
    && record.magic==0x52534231 && record.version==1 && record.crc==crc(record.value) && terminated(record.value)
    && record.value.revision && rehab::validHost(record.value.server) && rehab::validMac(record.value.heartAddress)
    && rehab::validPassword(record.value.password)) saved_=record.value;
  active_=saved_;
}
Settings SettingsStore::active() { if(!mutex_) return active_; xSemaphoreTake(mutex_,portMAX_DELAY); Settings s=active_; xSemaphoreGive(mutex_); return s; }
bool SettingsStore::pending() { if(!mutex_) return false; xSemaphoreTake(mutex_,portMAX_DELAY); bool p=pending_; xSemaphoreGive(mutex_); return p; }
void SettingsStore::allowApply() {
  if(!mutex_) return;
  xSemaphoreTake(mutex_,portMAX_DELAY);
  armed_=true; applyAfter_=millis()+500; // Give the already-written HTTP response time to leave the AP.
  xSemaphoreGive(mutex_);
}
bool SettingsStore::apply(Settings& before,Settings& after) {
  if(!mutex_) {before=after=active_; return false;}
  xSemaphoreTake(mutex_,portMAX_DELAY);
  bool changed=pending_&&armed_&&int32_t(millis()-applyAfter_)>=0;
  before=active_; if(changed) {active_=saved_; pending_=false; armed_=false;} after=active_;
  xSemaphoreGive(mutex_); return changed;
}
int SettingsStore::save(cJSON* req,String& error) {
  if(!cJSON_IsObject(req)) { error="設定必須是 JSON 物件"; return 400; }
  const char* fields[]={"wifi_ssid","wifi_password","socket_server","heart_rate_address","revision"};
  bool seen[5]={};
  for(cJSON* item=req->child;item;item=item->next) {
    int field=-1;
    for(int i=0;i<5;++i) if(item->string&&!strcmp(item->string,fields[i])) {field=i; break;}
    if(field<0||seen[field]) {error="設定包含未知或重複欄位"; return 400;}
    seen[field]=true;
  }
  auto ssid=cJSON_GetObjectItemCaseSensitive(req,"wifi_ssid");
  auto password=cJSON_GetObjectItemCaseSensitive(req,"wifi_password");
  auto host=cJSON_GetObjectItemCaseSensitive(req,"socket_server");
  auto mac=cJSON_GetObjectItemCaseSensitive(req,"heart_rate_address");
  auto rev=cJSON_GetObjectItemCaseSensitive(req,"revision");
  if(!cJSON_IsString(ssid)||!cJSON_IsString(host)||!cJSON_IsString(mac)||!cJSON_IsNumber(rev)||(password&&!cJSON_IsString(password))) {
    error="設定欄位型別不正確"; return 400;
  }
  if(!rehab::validRevision(rev->valuedouble)||strlen(ssid->valuestring)>32 || !rehab::validHost(host->valuestring)||!rehab::validMac(mac->valuestring)
    || (password&&!rehab::validPassword(password->valuestring))) { error="請檢查 SSID 長度、Wi-Fi 密碼、主機位址及 MAC 格式"; return 400; }
  if(!mutex_) {error="設定儲存服務無法使用"; return 503;}
  xSemaphoreTake(mutex_,portMAX_DELAY);
  int result=200;
  if(rev->valuedouble!=saved_.revision) {error="設定已被其他頁面修改，請重新載入目前設定"; result=409;}
  else if(!healthy_) {error="NVS 無法使用，設定尚未保存"; result=503;}
  else if(saved_.revision==UINT32_MAX) {error="設定版本已達上限"; result=503;}
  else {
    Settings next;
    strlcpy(next.ssid,ssid->valuestring,sizeof(next.ssid)); strlcpy(next.server,host->valuestring,sizeof(next.server));
    strlcpy(next.heartAddress,mac->valuestring,sizeof(next.heartAddress));
    for(char* p=next.heartAddress;*p;++p) *p=toupper((unsigned char)*p);
    strlcpy(next.password,password?password->valuestring:saved_.password,sizeof(next.password));
    next.revision=saved_.revision+1;
    Record record; record.value=next; record.crc=crc(next);
    if(prefs_.putBytes("settings",&record,sizeof(record))!=sizeof(record)) {error="設定寫入失敗，原設定仍保留"; result=503;}
    else {saved_=next; pending_=true; armed_=false;}
  }
  xSemaphoreGive(mutex_); return result;
}
void SettingsStore::append(cJSON* root) {
  if(!root) return;
  if(mutex_) xSemaphoreTake(mutex_,portMAX_DELAY);
  Settings saved=saved_,active=active_; bool p=pending_,h=healthy_;
  if(mutex_) xSemaphoreGive(mutex_);
  jn(root,"revision",saved.revision); jb(root,"pending",p); jb(root,"storage_ok",h);
  jsonSettings(cJSON_AddObjectToObject(root,"settings"),saved,true);
  jsonSettings(cJSON_AddObjectToObject(root,"active"),active,false);
}
