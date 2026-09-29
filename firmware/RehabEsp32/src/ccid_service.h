#pragma once

#include <Arduino.h>
#include "cJSON.h"

struct CcidSnapshot {
  bool ready = false;
  bool cardPresent = false;
  String state = "not_started";
  String uid;
  String lastError;
  String atr;
  String lastTx;
  String lastRx;
  uint32_t cards = 0;
  uint32_t errors = 0;
  uint32_t lastUpdateMs = 0;
  bool hasUpdate = false;
  uint16_t vid = 0;
  uint16_t pid = 0;
  uint32_t features = 0;
  uint32_t maxMessageLength = 0;
  int lastSw = -1;
};

class CcidService {
 public:
  void begin();
  CcidSnapshot snapshot();
  bool takeCard(String& hexUid);
  // Adds fields to the supplied object; caller chooses the containing JSON key.
  void appendStatus(cJSON* object, uint32_t now);

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

extern CcidService ccidService;
