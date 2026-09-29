#pragma once

// Protocol-only code: shared by the USB driver and desktop regression tests.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace ccid {
constexpr uint16_t kVendor = 0x1206;
constexpr uint16_t kProduct = 0x2107;
constexpr size_t kHeader = 10;
constexpr size_t kMaxMessage = 273;
constexpr uint8_t kGetSlotStatus = 0x65;
constexpr uint8_t kPowerOn = 0x62;
constexpr uint8_t kPowerOff = 0x63;
constexpr uint8_t kXfrBlock = 0x6F;
constexpr uint8_t kDataBlock = 0x80;
constexpr uint8_t kSlotStatus = 0x81;
constexpr uint8_t kUidApdu[] = {0xFF, 0xCA, 0x00, 0x00, 0x00};
constexpr uint8_t kBuzzerApdu[] = {
    0xFF, 0xE1, 0x02, 0x01, 0x0C, 0xA0, 0xFC, 0x5C, 0xA0,
    0xFD, 0x0A, 0xA0, 0xFE, 0x0A, 0xA0, 0xFF, 0x03};

inline uint16_t le16(const uint8_t* p) {
  return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
inline uint32_t le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
         (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline void put32(uint8_t* p, uint32_t value) {
  for (unsigned n = 0; n < 4; ++n) p[n] = uint8_t(value >> (n * 8));
}

struct Descriptor {
  uint8_t interfaceNumber = 0;
  uint8_t alternate = 0;
  uint8_t bulkIn = 0;
  uint8_t bulkOut = 0;
  uint8_t interruptIn = 0;
  uint16_t bulkInPacket = 0;
  uint16_t bulkOutPacket = 0;
  uint16_t interruptPacket = 0;
  uint32_t features = 0;
  uint32_t maxMessage = 0;
};

// This driver deliberately supports the CL-2000's automatic APDU-level
// interface only; a TPDU reader would require another protocol implementation.
inline bool parseConfiguration(const uint8_t* bytes, size_t size,
                               Descriptor& result, const char*& error) {
  error = nullptr;
  result = Descriptor{};
  if (!bytes || size < 9 || bytes[0] < 9 || bytes[1] != 2 ||
      le16(bytes + 2) != size) {
    error = "Invalid USB configuration length";
    return false;
  }
  bool selected = false;
  bool classFound = false;
  unsigned endpoints = 0;
  uint8_t expectedEndpoints = 0;
  for (size_t pos = 0; pos < size;) {
    if (size - pos < 2 || bytes[pos] < 2 || bytes[pos] > size - pos) {
      error = "Truncated USB descriptor";
      return false;
    }
    const uint8_t* d = bytes + pos;
    const size_t length = d[0];
    if (d[1] == 4) {
      if (length < 9) { error = "Short USB interface"; return false; }
      if (selected) break;
      selected = d[5] == 0x0B && d[6] == 0 && d[7] == 0 && d[3] == 0;
      if (selected) {
        result.interfaceNumber = d[2];
        result.alternate = d[3];
        expectedEndpoints = d[4];
      }
    } else if (selected && d[1] == 0x21) {
      if (classFound || length < 54 || d[4] != 0 || d[53] == 0) {
        error = "Unsupported CCID slot descriptor";
        return false;
      }
      classFound = true;
      result.features = le32(d + 40);
      result.maxMessage = le32(d + 44);
      const uint32_t exchange = result.features & 0x00070000UL;
      if ((exchange != 0x00020000UL && exchange != 0x00040000UL) ||
          !(result.features & 0x40)) {
        error = "Reader needs unsupported TPDU/manual negotiation";
        return false;
      }
      if (result.maxMessage < kHeader + sizeof(kBuzzerApdu) ||
          result.maxMessage > kMaxMessage) {
        error = "Unsupported CCID message capacity";
        return false;
      }
    } else if (selected && d[1] == 5) {
      if (length < 7) { error = "Short USB endpoint"; return false; }
      ++endpoints;
      const uint8_t address = d[2];
      const uint8_t type = d[3] & 3;
      const uint16_t packet = le16(d + 4);
      if (!(address & 0x0F) || (address & 0x70) || !packet || packet > 64) {
        error = "Unsupported USB endpoint packet size";
        return false;
      }
      if (address == result.bulkIn || address == result.bulkOut ||
          address == result.interruptIn) {
        error = "Duplicate CCID endpoint address";
        return false;
      }
      if (type == 2 && (address & 0x80) && !result.bulkIn) {
        result.bulkIn = address;
        result.bulkInPacket = packet;
      } else if (type == 2 && !(address & 0x80) && !result.bulkOut) {
        result.bulkOut = address;
        result.bulkOutPacket = packet;
      } else if (type == 3 && (address & 0x80) && !result.interruptIn && packet >= 2) {
        result.interruptIn = address;
        result.interruptPacket = packet;
      } else {
        error = "Unexpected/duplicate CCID endpoint";
        return false;
      }
    }
    pos += length;
  }
  if (!selected || !classFound || !result.bulkIn || !result.bulkOut ||
      endpoints != expectedEndpoints) {
    error = "Incomplete CCID APDU interface";
    return false;
  }
  return true;
}

inline size_t makeCommand(uint8_t* out, size_t capacity, uint8_t command,
                          uint8_t sequence, const uint8_t* data = nullptr,
                          size_t length = 0, uint8_t specific0 = 0) {
  if (!out || length > kMaxMessage - kHeader || capacity < kHeader + length ||
      (length && !data)) return 0;
  memset(out, 0, kHeader);
  out[0] = command;
  put32(out + 1, uint32_t(length));
  out[6] = sequence;
  out[7] = specific0;
  if (length) memcpy(out + kHeader, data, length);
  return kHeader + length;
}

enum class DecodeResult { Ok, Short, Length, Type, Slot, Sequence, Status, Chained };
struct Reply {
  uint8_t icc = 2;
  uint8_t commandStatus = 0;
  uint8_t error = 0;
  uint8_t chain = 0;
  const uint8_t* data = nullptr;
  size_t length = 0;
  bool failed() const { return commandStatus == 0x40; }
  bool extension() const { return commandStatus == 0x80; }
};

inline DecodeResult decode(const uint8_t* bytes, size_t size, uint8_t expectedType,
                           uint8_t sequence, Reply& result) {
  result = Reply{};
  if (!bytes || size < kHeader) return DecodeResult::Short;
  const uint32_t length = le32(bytes + 1);
  if (length > kMaxMessage - kHeader || size != kHeader + length)
    return DecodeResult::Length;
  if (bytes[0] != expectedType) return DecodeResult::Type;
  if (bytes[5] != 0) return DecodeResult::Slot;
  if (bytes[6] != sequence) return DecodeResult::Sequence;
  if (expectedType == kSlotStatus && length != 0) return DecodeResult::Length;
  if ((bytes[7] & 0x3C) || (bytes[7] & 3) == 3 || (bytes[7] & 0xC0) == 0xC0)
    return DecodeResult::Status;
  result.icc = bytes[7] & 3;
  result.commandStatus = bytes[7] & 0xC0;
  result.error = bytes[8];
  result.chain = bytes[9];
  result.data = bytes + kHeader;
  result.length = length;
  // The only APDUs issued by this application fit within one CCID message.
  if (expectedType == kDataBlock && result.chain != 0 &&
      !result.failed() && !result.extension()) return DecodeResult::Chained;
  return DecodeResult::Ok;
}

inline bool uidHex(const Reply& reply, char* output, size_t capacity) {
  if (!output || reply.failed() || reply.extension() || reply.icc != 0 || reply.length < 3 ||
      reply.length > 257 || !reply.data ||
      reply.data[reply.length - 2] != 0x90 || reply.data[reply.length - 1] != 0 ||
      capacity < (reply.length - 2) * 2 + 1) return false;
  static constexpr char hex[] = "0123456789ABCDEF";
  for (size_t n = 0; n < reply.length - 2; ++n) {
    output[n * 2] = hex[reply.data[n] >> 4];
    output[n * 2 + 1] = hex[reply.data[n] & 15];
  }
  output[(reply.length - 2) * 2] = 0;
  return true;
}

struct Presentation {
  bool present = false;
  bool delivered = false;
  bool buzzerAttempted = false;
  void observe(bool value) {
    present = value;
    if (!value) { delivered = false; buzzerAttempted = false; }
  }
  bool needsRead() const { return present && !delivered; }
};
}  // namespace ccid
