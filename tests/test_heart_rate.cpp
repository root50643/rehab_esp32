#include "../firmware/RehabEsp32/src/heart_rate_codec.h"

#include <assert.h>
#include <stdint.h>

using namespace heart_rate;

int main() {
  const uint8_t ordinary[] = {0x00, 72};
  const uint8_t wide[] = {0x01, 0x2C, 0x01};
  const uint8_t zero[] = {0x00, 0x00};
  const uint8_t maximum[] = {0x01, 0xFF, 0xFF};
  const uint8_t allOptional[] = {0x19, 0x2C, 0x01, 0x45, 0x03, 0x00, 0x04, 0x10, 0x04};
  assert(decode(ordinary, sizeof ordinary).bpm == 72);
  assert(decode(wide, sizeof wide).bpm == 300);
  assert(decode(zero, sizeof zero).ok() && decode(zero, sizeof zero).bpm == 0);
  assert(decode(maximum, sizeof maximum).bpm == 65535);
  assert(decode(allOptional, sizeof allOptional).ok());
  assert(decode(nullptr, 0).error == Error::TooShort);
  assert(decode(ordinary, 1).error == Error::TooShort);
  assert(decode(wide, 2).error == Error::MissingValue);
  assert(decode(allOptional, 3).error == Error::MissingEnergy);
  assert(decode(allOptional, 4).error == Error::MissingEnergy);
  assert(decode(allOptional, 5).error == Error::InvalidIntervals);
  assert(decode(allOptional, 6).error == Error::InvalidIntervals);
  assert(decode(allOptional, 7).ok());
  assert(decode(allOptional, 8).error == Error::InvalidIntervals);
  const uint8_t rrOnly[] = {0x10, 99, 0x00, 0x04};
  assert(decode(rrOnly, sizeof rrOnly).bpm == 99);
  assert(!decode(rrOnly, 3).ok());
  assert(!socketRepresentable(0));
  assert(socketRepresentable(1) && socketRepresentable(255));
  assert(!socketRepresentable(256) && !socketRepresentable(65535));

  Reading reading;
  assert(!reading.valid && !reading.ready);
  assert(!reading.expired(10099, 100));
  assert(reading.expired(10100, 100));
  reading.accept(decode(ordinary, sizeof ordinary), 0);  // millis()==0 is valid.
  assert(reading.hasReceived && reading.valid && !reading.ready);
  assert(reading.expired(10000, 100));
  reading.accept(decode(ordinary, sizeof ordinary), 1000);
  reading.accept(decode(ordinary, sizeof ordinary), 2000);
  assert(reading.ready && reading.notifications == 3 && reading.consecutive == 3);
  assert(!reading.accept(decode(wide, 2), 3000));
  assert(reading.receivedMs == 2000 && reading.ready && reading.notifications == 3);
  reading.accept(decode(zero, sizeof zero), 11000);
  assert(!reading.valid && !reading.ready && reading.consecutive == 0);
  assert(reading.receivedMs == 2000 && reading.expired(12000, 100));
  reading.accept(decode(wide, sizeof wide), 12500);
  assert(reading.valid && reading.bpm == 300 && reading.consecutive == 1);
  reading.clear();
  assert(!reading.valid && !reading.hasReceived && reading.bpm == -1);
  assert(reading.notifications == 4 && reading.consecutive == 0);
  reading.accept(decode(ordinary, sizeof ordinary), UINT32_MAX - 5000);
  assert(!reading.expired(4998, 0));
  assert(reading.expired(4999, 0));
  return 0;
}
