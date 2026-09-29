#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "cJSON.h"
#include "freertos/semphr.h"

struct Settings {
  char ssid[33] = {}, password[65] = {}, server[254] = {}, heartAddress[18] = {};
  uint8_t reserved[2] = {}; // Explicit bytes: no indeterminate padding in persisted CRC.
  uint32_t revision = 1;
};
static_assert(sizeof(Settings)==376,"Bump the stored record version when changing Settings layout");
class SettingsStore {
  Preferences prefs_;
  SemaphoreHandle_t mutex_ = nullptr;
  Settings active_, saved_;
  bool pending_ = false, healthy_ = false, armed_ = false;
  uint32_t applyAfter_ = 0;
public:
  void begin();
  Settings active();
  bool pending();
  void allowApply();
  bool apply(Settings& before, Settings& after);
  int save(cJSON* request, String& error);
  void append(cJSON* root);
};
extern SettingsStore settingsStore;
