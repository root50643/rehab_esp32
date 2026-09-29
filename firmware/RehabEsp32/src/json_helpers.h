#pragma once
#include <Arduino.h>
#include "cJSON.h"
inline void js(cJSON* o, const char* key, const String& value) { cJSON_AddStringToObject(o, key, value.c_str()); }
inline void jn(cJSON* o, const char* key, double value) { cJSON_AddNumberToObject(o, key, value); }
inline void jb(cJSON* o, const char* key, bool value) { cJSON_AddBoolToObject(o, key, value); }
inline void jnullable(cJSON* o, const char* key, double value, bool valid) {
  if (valid) jn(o, key, value); else cJSON_AddNullToObject(o, key);
}
inline String hexBytes(const uint8_t* data, size_t len) {
  const char digits[] = "0123456789ABCDEF";
  String s; s.reserve(len * 3);
  for (size_t i = 0; i < len; ++i) { if (i) s += ' '; s += digits[data[i] >> 4]; s += digits[data[i] & 15]; }
  return s;
}
