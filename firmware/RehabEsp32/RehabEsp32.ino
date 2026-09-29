#include "src/board_config.h"
#include "src/settings.h"
#include "src/ble_service.h"
#include "src/ccid_service.h"
#include "src/tcp_service.h"
#include "src/controller.h"
#include "src/portal.h"
void setup() {
  Serial.begin(115200);
  Serial.printf("Flash: %lu bytes; PSRAM: %lu bytes\n",static_cast<unsigned long>(ESP.getFlashChipSize()),static_cast<unsigned long>(ESP.getPsramSize()));
  settingsStore.begin(); auto settings=settingsStore.active();
  bleService.begin(settings.heartAddress);
  ccidService.begin();
  tcpService.begin(settings.server);
  controller.begin();
  portalBegin();
}
void loop() {portalTick(); delay(2);}
