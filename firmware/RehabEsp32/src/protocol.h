#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace rehab {
struct Frame {
  uint8_t command = 0, length = 0;
  uint8_t data[255] = {};
};
inline uint8_t checksum(const Frame& f) {
  uint8_t sum = f.command + f.length;
  for (size_t i = 0; i < f.length; ++i) sum += f.data[i];
  return uint8_t(0xff - sum);
}
inline size_t encode(const Frame& f, uint8_t head, uint8_t tail, uint8_t* out,
                     size_t capacity = 260) {
  if (!out || capacity < size_t(f.length) + 5) return 0;
  out[0] = head; out[1] = f.command; out[2] = f.length;
  memcpy(out + 3, f.data, f.length);
  out[3 + f.length] = checksum(f); out[4 + f.length] = tail;
  return size_t(f.length) + 5;
}
inline Frame makeFrame(uint8_t cmd, const uint8_t* data, size_t length) {
  Frame f; f.command = cmd;
  f.length = length <= 255 && (!length || data) ? uint8_t(length) : 0;
  if (f.length) memcpy(f.data, data, f.length);
  return f;
}
inline uint16_t be16(const uint8_t* p) { return (uint16_t(p[0]) << 8) | p[1]; }
inline bool due(uint32_t now, uint32_t then, uint32_t interval) {
  return uint32_t(now - then) >= interval;
}
// Length-delimited stream: delimiters inside payload are ordinary bytes.
class Parser {
  uint8_t bytes_[260] = {}, head_, tail_;
  uint32_t arrived_[260] = {};
  size_t used_ = 0;
  uint32_t started_ = 0, now_ = 0;
  bool resynchronizing_ = false;
  void drop(size_t n) {
    memmove(bytes_, bytes_ + n, used_ - n);
    memmove(arrived_, arrived_ + n, (used_ - n) * sizeof(arrived_[0]));
    used_ -= n;
    started_ = used_ ? arrived_[0] : now_;
  }
public:
  uint32_t checksumErrors = 0, framingErrors = 0, timeouts = 0;
  Parser(uint8_t head, uint8_t tail) : head_(head), tail_(tail) {}
  void reset() { used_ = 0; resynchronizing_ = false; }
  void expire(uint32_t now) {
    now_ = now;
    if (used_ && due(now, started_, 2000)) {
      drop(1); ++timeouts; resynchronizing_ = used_ != 0;
    }
  }
  void feed(uint8_t b, uint32_t now) {
    expire(now);
    if (!used_) { if (b != head_) return; started_ = now; }
    if (used_ == sizeof(bytes_)) { drop(1); ++framingErrors; }
    arrived_[used_] = now; bytes_[used_++] = b;
  }
  bool next(Frame& f) {
    while (used_) {
      if (bytes_[0] != head_) { drop(1); continue; }
      if (used_ < 3) {
        if (resynchronizing_ && due(now_,started_,2000)) used_ = 0;
        resynchronizing_ = false;
        return false;
      }
      size_t length = bytes_[2], total = length + 5;
      if (used_ < total) {
        if (resynchronizing_ && due(now_,started_,2000)) { drop(1); continue; }
        resynchronizing_ = false;
        return false;
      }
      if (bytes_[total - 1] != tail_) { ++framingErrors; drop(1); continue; }
      Frame candidate = makeFrame(bytes_[1], bytes_ + 3, length);
      if (checksum(candidate) != bytes_[total - 2]) { ++checksumErrors; drop(1); continue; }
      f = candidate; drop(total); resynchronizing_ = false; return true;
    }
    resynchronizing_ = false;
    return false;
  }
};

enum class PulseKind : uint8_t { None, Start, Stop };
struct Pulse {
  PulseKind kind = PulseKind::None;
  uint32_t since = 0, generation = 0;
  bool high = false, completed = false;
  void start(PulseKind k, uint32_t now, uint32_t gen) {
    kind = k; since = now; generation = gen; high = k != PulseKind::None; completed = false;
  }
  // Return completion only once. STOP has 500ms low settling after 3s high.
  bool tick(uint32_t now) {
    if (kind == PulseKind::None || completed) return false;
    uint32_t duration = kind == PulseKind::Start ? 500 : 3000;
    if (due(now, since, duration)) high = false;
    if (due(now, since, duration + (kind == PulseKind::Stop ? 500 : 0))) {
      completed = true; return true;
    }
    return false;
  }
};
inline bool canApplyConfig(bool tracking, PulseKind pulse, size_t queuedControls) {
  return !tracking && pulse == PulseKind::None && queuedControls == 0;
}
template <class T, size_t Capacity> class Fifo {
  static_assert(Capacity > 0, "FIFO must have storage");
  T items_[Capacity]{};
  size_t head_ = 0, count_ = 0;
public:
  bool push(const T& value) {
    if (count_ == Capacity) return false;
    items_[(head_ + count_) % Capacity] = value; ++count_; return true;
  }
  const T* peek() const { return count_ ? &items_[head_] : nullptr; }
  bool pop(T& value) {
    if (!count_) return false;
    value = items_[head_]; head_ = (head_ + 1) % Capacity; --count_; return true;
  }
  size_t size() const { return count_; }
  void clear() { head_ = count_ = 0; }
};

// Coalesces samples, retaining their observation time rather than extending
// freshness every time a worker looks at them. A cancellation invalidates a
// token already selected by the TCP worker but not yet written to the socket.
class TelemetryOutbox {
public:
  struct Token {
    uint8_t selector = 0, value = 0;
    uint32_t generation = 0, revision = 0, observed = 0, lifetime = 0;
  };
private:
  struct Slot { Token token; bool valid = false, pending = false; } slots_[3];
  uint8_t next_ = 0;
public:
  bool publish(uint8_t selector, int value, uint32_t generation,
               uint32_t observed, uint32_t lifetime) {
    if (selector >= 3 || value < 0 || value > 255 || !lifetime ||
        (selector == 2 && value == 0)) return false;
    Slot& slot = slots_[selector];
    ++slot.token.revision;
    slot.token.selector = selector; slot.token.value = uint8_t(value);
    slot.token.generation = generation; slot.token.observed = observed;
    slot.token.lifetime = lifetime; slot.valid = slot.pending = true;
    return true;
  }
  void cancel(uint8_t selector) {
    if (selector >= 3) return;
    Slot& slot = slots_[selector];
    slot.valid = slot.pending = false; ++slot.token.revision;
  }
  void clear() { for (uint8_t i = 0; i < 3; ++i) cancel(i); }
  bool current(const Token& token, uint32_t generation, uint32_t now) const {
    if (token.selector >= 3) return false;
    const Slot& slot = slots_[token.selector];
    return slot.valid && token.revision == slot.token.revision &&
           token.generation == generation && !due(now, token.observed, token.lifetime);
  }
  bool pending(uint32_t generation, uint32_t now) const {
    for (const Slot& slot : slots_)
      if (slot.pending && current(slot.token, generation, now)) return true;
    return false;
  }
  bool take(uint32_t generation, uint32_t now, Token& token) {
    for (unsigned n = 0; n < 3; ++n) {
      const uint8_t index = next_; next_ = (next_ + 1) % 3;
      Slot& slot = slots_[index];
      if (!slot.pending) continue;
      slot.pending = false;
      if (!current(slot.token, generation, now)) continue;
      token = slot.token; return true;
    }
    return false;
  }
};
struct MachineValues {
  bool has28 = false, has3f = false;
  uint32_t at28 = 0, at3f = 0;
  uint16_t minutes = 0, distance = 0, calories = 0, rpm = 0, watt28 = 0;
  uint8_t seconds = 0, bpm = 0;
  uint16_t level = 0, watt3f = 0, torque = 0;
  bool accept(const Frame& f, uint32_t now) {
    if (f.command == 0x28 && f.length == 12) {
      minutes = be16(f.data); seconds = f.data[2]; distance = be16(f.data + 3);
      calories = be16(f.data + 5); bpm = f.data[7]; rpm = be16(f.data + 8);
      watt28 = be16(f.data + 10); has28 = true; at28 = now; return true;
    }
    if (f.command == 0x3f && f.length == 6) {
      level = be16(f.data); watt3f = be16(f.data + 2); torque = be16(f.data + 4);
      has3f = true; at3f = now; return true;
    }
    return false;
  }
};
}
