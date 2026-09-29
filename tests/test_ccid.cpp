#include "../firmware/RehabEsp32/src/ccid_codec.h"
#include <assert.h>
#include <iostream>
#include <vector>

using Bytes = std::vector<uint8_t>;

Bytes configuration() {
  Bytes bytes = {9, 2, 93, 0, 1, 1, 0, 0x80, 50,
                 9, 4, 0, 0, 3, 0x0B, 0, 0, 0};
  Bytes descriptor(54, 0);
  descriptor[0] = 54; descriptor[1] = 0x21;
  descriptor[2] = 0x10; descriptor[3] = 1;
  descriptor[5] = 7;
  ccid::put32(descriptor.data() + 6, 3);
  ccid::put32(descriptor.data() + 40, 0x0004047E);
  ccid::put32(descriptor.data() + 44, 273);
  descriptor[48] = descriptor[49] = 0xFF;
  descriptor[53] = 1;
  bytes.insert(bytes.end(), descriptor.begin(), descriptor.end());
  const uint8_t endpoints[] = {7, 5, 0x81, 2, 64, 0, 0,
                              7, 5, 0x02, 2, 64, 0, 0,
                              7, 5, 0x83, 3, 8, 0, 16};
  bytes.insert(bytes.end(), endpoints, endpoints + sizeof(endpoints));
  return bytes;
}

Bytes reply(uint8_t type, uint8_t sequence, const Bytes& payload,
            uint8_t status = 0, uint8_t error = 0, uint8_t chain = 0) {
  Bytes bytes(10 + payload.size(), 0);
  bytes[0] = type; ccid::put32(bytes.data() + 1, uint32_t(payload.size()));
  bytes[6] = sequence; bytes[7] = status; bytes[8] = error; bytes[9] = chain;
  if (!payload.empty()) memcpy(bytes.data() + 10, payload.data(), payload.size());
  return bytes;
}

void descriptors() {
  ccid::Descriptor parsed;
  const char* error;
  auto bytes = configuration();
  assert(bytes.size() == 93);
  assert(ccid::parseConfiguration(bytes.data(), bytes.size(), parsed, error));
  assert(parsed.features == 0x0004047E && parsed.maxMessage == 273);
  assert(parsed.bulkIn == 0x81 && parsed.bulkOut == 2 && parsed.interruptIn == 0x83);
  assert(parsed.bulkInPacket == 64 && parsed.interruptPacket == 8);
  // Every truncation is rejected, including truncations before an endpoint.
  for (size_t length = 0; length < bytes.size(); ++length)
    assert(!ccid::parseConfiguration(bytes.data(), length, parsed, error));
  // Also truncate with a self-consistent configuration header: this exercises
  // each inner descriptor's boundary checks rather than just wTotalLength.
  for (size_t length = 9; length < bytes.size(); ++length) {
    auto truncated = bytes;
    truncated.resize(length);
    truncated[2] = uint8_t(length);
    assert(!ccid::parseConfiguration(truncated.data(), truncated.size(), parsed, error));
  }
  auto bad = bytes; bad[18] = 0;
  assert(!ccid::parseConfiguration(bad.data(), bad.size(), parsed, error));
  bad = bytes; ccid::put32(bad.data() + 18 + 40, 0x0001047E); // TPDU
  assert(!ccid::parseConfiguration(bad.data(), bad.size(), parsed, error));
  bad = bytes; ccid::put32(bad.data() + 18 + 40, 0x0004043E); // manual negotiation
  assert(!ccid::parseConfiguration(bad.data(), bad.size(), parsed, error));
  bad = bytes; ccid::put32(bad.data() + 18 + 44, 65536);
  assert(!ccid::parseConfiguration(bad.data(), bad.size(), parsed, error));
  bad = bytes; bad[18 + 4] = 1; // Multiple slots not supported
  assert(!ccid::parseConfiguration(bad.data(), bad.size(), parsed, error));
  bad = bytes; bad[79 + 2] = 0x81; // duplicate bulk IN, no OUT
  assert(!ccid::parseConfiguration(bad.data(), bad.size(), parsed, error));
  bad = bytes; bad[72 + 4] = 0;
  assert(!ccid::parseConfiguration(bad.data(), bad.size(), parsed, error));
  bad = bytes; bad[86 + 2] = 0x81; // duplicate address across endpoint types
  assert(!ccid::parseConfiguration(bad.data(), bad.size(), parsed, error));
  // Polling-only readers may omit the interrupt endpoint.
  bad = bytes; bad.resize(86); bad[2] = 86; bad[9 + 4] = 2;
  assert(ccid::parseConfiguration(bad.data(), bad.size(), parsed, error));
  assert(parsed.interruptIn == 0);
}

void commandsAndReplies() {
  uint8_t buffer[273]{};
  assert(ccid::makeCommand(buffer, sizeof(buffer), ccid::kXfrBlock, 9,
                          ccid::kUidApdu, sizeof(ccid::kUidApdu)) == 15);
  const uint8_t golden[] = {0x6F, 5, 0, 0, 0, 0, 9, 0, 0, 0, 0xFF, 0xCA, 0, 0, 0};
  assert(memcmp(buffer, golden, sizeof(golden)) == 0);
  assert(ccid::makeCommand(buffer, sizeof(buffer), ccid::kXfrBlock, 255,
                          ccid::kBuzzerApdu, sizeof(ccid::kBuzzerApdu)) == 27);
  assert(buffer[6] == 255 && memcmp(buffer + 10, ccid::kBuzzerApdu, 17) == 0);
  assert(ccid::makeCommand(buffer, 10, ccid::kXfrBlock, 0, ccid::kUidApdu, 5) == 0);
  assert(ccid::makeCommand(buffer, 273, ccid::kXfrBlock, 0, nullptr, 1) == 0);
  assert(ccid::makeCommand(buffer, 273, ccid::kGetSlotStatus, 0) == 10);
  const uint8_t statusGolden[] = {0x65, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  assert(memcmp(buffer, statusGolden, 10) == 0);

  ccid::Reply result;
  auto bytes = reply(0x80, 9, {0, 1, 0xAB, 0xCD, 0x90, 0});
  assert(ccid::decode(bytes.data(), bytes.size(), 0x80, 9, result) == ccid::DecodeResult::Ok);
  char uid[511];
  assert(ccid::uidHex(result, uid, sizeof(uid)) && strcmp(uid, "0001ABCD") == 0);
  assert(!ccid::uidHex(result, uid, 8));
  assert(!ccid::uidHex(result, nullptr, 511));
  assert(ccid::decode(bytes.data(), bytes.size(), 0x80, 10, result) == ccid::DecodeResult::Sequence);
  assert(ccid::decode(bytes.data(), bytes.size(), 0x81, 9, result) == ccid::DecodeResult::Type);
  for (size_t n = 0; n < bytes.size(); ++n)
    assert(ccid::decode(bytes.data(), n, 0x80, 9, result) != ccid::DecodeResult::Ok);
  auto bad = bytes; bad[5] = 1;
  assert(ccid::decode(bad.data(), bad.size(), 0x80, 9, result) == ccid::DecodeResult::Slot);
  bad = bytes; ccid::put32(bad.data() + 1, 0xFFFFFFFFUL);
  assert(ccid::decode(bad.data(), bad.size(), 0x80, 9, result) == ccid::DecodeResult::Length);
  bad = bytes; bad[7] = 0xC0;
  assert(ccid::decode(bad.data(), bad.size(), 0x80, 9, result) == ccid::DecodeResult::Status);
  bad = bytes; bad[9] = 1;
  assert(ccid::decode(bad.data(), bad.size(), 0x80, 9, result) == ccid::DecodeResult::Chained);

  for (uint8_t status : {uint8_t(0x80), uint8_t(0x40), uint8_t(2), uint8_t(1)}) {
    bytes = reply(0x80, 9, {0, 1, 0x90, 0}, status);
    assert(ccid::decode(bytes.data(), bytes.size(), 0x80, 9, result) == ccid::DecodeResult::Ok);
    assert(!ccid::uidHex(result, uid, sizeof(uid)));
    assert(result.extension() == (status == 0x80));
    assert(result.failed() == (status == 0x40));
  }
  for (const auto& payload : {Bytes{0x90, 0}, Bytes{1, 0x6A, 0x82}, Bytes{1}}) {
    bytes = reply(0x80, 9, payload);
    assert(ccid::decode(bytes.data(), bytes.size(), 0x80, 9, result) == ccid::DecodeResult::Ok);
    assert(!ccid::uidHex(result, uid, sizeof(uid)));
  }
  Bytes longest(257, 0xAB); longest[255] = 0x90; longest[256] = 0;
  bytes = reply(0x80, 9, longest);
  assert(ccid::decode(bytes.data(), bytes.size(), 0x80, 9, result) == ccid::DecodeResult::Ok);
  assert(ccid::uidHex(result, uid, sizeof(uid)) && strlen(uid) == 510);
}

void presentations() {
  ccid::Presentation state;
  assert(!state.needsRead());
  state.observe(true); assert(state.needsRead());
  state.buzzerAttempted = true;
  state.observe(true); assert(state.needsRead() && state.buzzerAttempted);
  state.delivered = true;
  state.observe(true); assert(!state.needsRead());
  // Powered off but physically present is still 'present' and does not rearm.
  state.observe(true); assert(!state.needsRead());
  state.observe(false); assert(!state.delivered && !state.buzzerAttempted);
  state.observe(true); assert(state.needsRead());
}

int main() {
  descriptors();
  commandsAndReplies();
  presentations();
  std::cout << "CCID descriptor, packet, UID, and presentation tests passed\n";
}
