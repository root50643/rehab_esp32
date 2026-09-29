#pragma once

#include <Arduino.h>
#include <cJSON.h>

struct BleSnapshot {
  bool subscribed = false;
  bool ready = false;
  bool valid = false;
  bool hasDisconnect = false;
  int bpm = -1;
  uint32_t receivedMs = 0;
  uint32_t notifications = 0;
  uint32_t disconnects = 0;
  uint32_t lastDisconnectMs = 0;
  String name;
  String address;
};

class BleService {
 public:
  void begin(const String& address);
  void configure(const String& address);
  // A queued UI scan is retained while the Wi-Fi scan arbiter denies access.
  void setScanAllowed(bool allowed);
  bool scanBusy();
  bool requestScan();
  BleSnapshot snapshot();
  void appendStatus(cJSON* object, uint32_t now);
  void appendScan(cJSON* object);

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

extern BleService bleService;
