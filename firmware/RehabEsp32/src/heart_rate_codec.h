#pragma once

#include <stddef.h>
#include <stdint.h>

// Portable decoding/state rules shared by the firmware and host-side tests.
namespace heart_rate {

enum class Error : uint8_t { None, TooShort, MissingValue, MissingEnergy, InvalidIntervals };

struct Measurement {
  uint16_t bpm = 0;
  Error error = Error::None;
  bool ok() const { return error == Error::None; }
};

inline Measurement decode(const uint8_t* data, size_t size) {
  if (data == nullptr || size < 2) return {0, Error::TooShort};
  const uint8_t flags = data[0];
  const size_t valueSize = (flags & 1U) ? 2U : 1U;
  size_t end = 1U + valueSize;
  if (size < end) return {0, Error::MissingValue};
  const uint16_t bpm = static_cast<uint16_t>(data[1]) |
      ((valueSize == 2) ? static_cast<uint16_t>(data[2]) << 8 : 0);
  if (flags & 0x08U) {
    end += 2;
    if (size < end) return {0, Error::MissingEnergy};
  }
  if ((flags & 0x10U) && (size - end < 2 || ((size - end) & 1U)))
    return {0, Error::InvalidIntervals};
  return {bpm, Error::None};
}

inline const char* errorText(Error error) {
  switch (error) {
    case Error::TooShort: return "Heart rate notification is too short";
    case Error::MissingValue: return "Incomplete 16-bit heart rate value";
    case Error::MissingEnergy: return "Incomplete Energy Expended field";
    case Error::InvalidIntervals: return "Incomplete RR-Interval field";
    default: return "";
  }
}

inline bool socketRepresentable(uint16_t value) { return value > 0 && value <= 255; }

struct Reading {
  int bpm = -1;
  bool valid = false;
  bool ready = false;
  bool hasReceived = false;
  uint32_t receivedMs = 0;
  uint32_t notifications = 0;
  uint32_t consecutive = 0;

  // The lifetime notification counter deliberately survives reconnection.
  void clear() {
    bpm = -1;
    valid = ready = hasReceived = false;
    receivedMs = consecutive = 0;
  }

  bool accept(const Measurement& measurement, uint32_t now) {
    if (!measurement.ok()) return false;
    bpm = measurement.bpm;
    if (measurement.bpm == 0) {
      valid = ready = false;
      consecutive = 0;
      return true;  // Zero does not extend the last valid notification deadline.
    }
    valid = hasReceived = true;
    receivedMs = now;
    ++notifications;
    if (consecutive != UINT32_MAX) ++consecutive;
    ready = consecutive >= 3;
    return true;
  }

  bool expired(uint32_t now, uint32_t subscribedMs, uint32_t timeoutMs = 10000) const {
    return static_cast<uint32_t>(now - (hasReceived ? receivedMs : subscribedMs)) >= timeoutMs;
  }
};

}  // namespace heart_rate
