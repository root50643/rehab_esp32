#include "ble_service.h"
#include "heart_rate_codec.h"

#include <BLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <host/ble_hs_adv.h>
#include <host/ble_hs_id.h>
#include <os/os_mbuf.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <new>

#if !defined(CONFIG_IDF_TARGET_ESP32S3) || !defined(CONFIG_BT_NIMBLE_ENABLED)
#error "RehabEsp32 BLE requires ESP32-S3 and Arduino ESP32 3.3.11's NimBLE backend"
#endif

namespace rehab_ble_detail {
constexpr uint32_t kRetryMs = 3000;
constexpr uint32_t kConnectMs = 10000;
constexpr uint32_t kGattMs = 5000;
constexpr size_t kCandidateLimit = 64;
constexpr size_t kNotificationLimit = 512;
const ble_uuid16_t kHeartService = BLE_UUID16_INIT(0x180D);
const ble_uuid16_t kBatteryService = BLE_UUID16_INIT(0x180F);
const ble_uuid16_t kMeasurement = BLE_UUID16_INIT(0x2A37);
const ble_uuid16_t kBattery = BLE_UUID16_INIT(0x2A19);
const ble_uuid16_t kCccd = BLE_UUID16_INIT(0x2902);

class Lock {
 public:
  explicit Lock(SemaphoreHandle_t mutex) : mutex_(mutex) { xSemaphoreTake(mutex_, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mutex_); }
 private:
  SemaphoreHandle_t mutex_;
};

template <size_t N> void copyText(char (&target)[N], const char* source) {
  snprintf(target, N, "%s", source ? source : "");
}

bool normalizeAddress(const String& input, char (&output)[18]) {
  if (input.length() != 17) return false;
  for (size_t i = 0; i < 17; ++i) {
    char c = input[i];
    if (i % 3 == 2) {
      if (c != ':') return false;
    } else {
      if (c >= 'a' && c <= 'f') c -= 'a' - 'A';
      if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
    }
    output[i] = c;
  }
  output[17] = 0;
  return true;
}

void formatAddress(const ble_addr_t& address, char (&text)[18]) {
  snprintf(text, sizeof text, "%02X:%02X:%02X:%02X:%02X:%02X", address.val[5], address.val[4],
           address.val[3], address.val[2], address.val[1], address.val[0]);
}

const char* addressTypeName(int type) {
  switch (type) {
    case 0: return "public";
    case 1: return "random";
    case 2: return "public_identity";
    case 3: return "random_identity";
    default: return "unknown";
  }
}

struct Candidate {
  char name[64] = {};
  char address[18] = {};
  ble_addr_t native = {};
  int rssi = 0;
  bool heartRate = false;
};

enum class Operation : uint8_t { None, HeartService, HeartCharacteristics, Cccd, Subscribe,
                               BatteryService, BatteryCharacteristic, BatteryRead };

bool optionalOperation(Operation operation) {
  return operation == Operation::BatteryService || operation == Operation::BatteryCharacteristic ||
         operation == Operation::BatteryRead;
}
}  // namespace rehab_ble_detail

using namespace rehab_ble_detail;

// The bundled C++ BLE wrappers have unbounded GATT waits, and their S3 scan
// wrapper exposes no cache limit. Initialize that same bundled host, then use
// its asynchronous NimBLE APIs so every operation has a firmware deadline and
// the only advertisement storage is the fixed array below.
struct BleService::Impl {
  SemaphoreHandle_t mutex = nullptr;
  TaskHandle_t task = nullptr;
  bool initialized = false;
  bool scanAllowed = true;
  uint32_t generation = 1;
  char target[18] = {};
  char state[24] = "starting";
  char lastError[128] = {};
  char lastHex[132] = {};  // First 64 bytes; "..." explicitly marks truncation.
  char name[64] = {};
  int addressType = -1;
  int rssi = 0;
  int battery = -1;
  bool connected = false;
  bool subscribed = false;
  bool hasDisconnect = false;
  uint32_t subscribedMs = 0;
  uint32_t disconnects = 0;
  uint32_t lastDisconnectMs = 0;
  uint32_t retryAt = 0;
  heart_rate::Reading reading;

  bool userScanRequested = false;
  bool scanStarting = false;
  bool scanRunning = false;
  bool scanIsUser = false;
  bool scanCompleted = false;
  uint32_t scanGeneration = 0;
  uint32_t scanStartedMs = 0;
  char scanState[16] = "idle";
  char scanError[96] = {};
  Candidate candidates[kCandidateLimit];
  size_t candidateCount = 0;
  Candidate foundTarget;
  bool targetFound = false;

  // One stable callback context, never recycled until GAP and GATT have both
  // completed. Configuration changes invalidate its immutable generation.
  struct Session {
    bool used = false;
    bool connecting = false;
    bool gapClosed = false;
    bool closeRequested = false;
    bool closeSent = false;
    bool gattPending = false;
    bool optionalTimedOut = false;
    bool wasSubscribed = false;
    uint32_t generation = 0;
    uint32_t operationMs = 0;
    uint32_t closeMs = 0;
    uint16_t connection = BLE_HS_CONN_HANDLE_NONE;
    uint16_t serviceStart = 0;
    uint16_t serviceEnd = 0;
    uint16_t measurementHandle = 0;
    uint16_t descriptorEnd = 0;
    uint16_t cccdHandle = 0;
    uint16_t batteryStart = 0;
    uint16_t batteryEnd = 0;
    uint16_t batteryHandle = 0;
    uint8_t serviceCount = 0;
    uint8_t measurementCount = 0;
    uint8_t cccdCount = 0;
    bool notifySupported = false;
    Operation next = Operation::None;
    Operation inflight = Operation::None;
  } session;

  bool current(uint16_t handle) const {
    return session.used && session.generation == generation && !session.closeRequested &&
           session.connection == handle;
  }

  void clearReading() {
    reading.clear();
    subscribed = false;
    battery = -1;
    lastHex[0] = 0;
  }

  void fail(const char* message) {  // Caller owns mutex.
    copyText(lastError, message);
    session.closeRequested = true;
    session.next = Operation::None;
    clearReading();
    copyText(state, "disconnecting");
  }

  void operationFailed(const char* message) {
    if (optionalOperation(session.inflight)) {
      session.next = Operation::None;
      battery = -1;
      // Battery is optional; keep notifications and the healthy connection.
    } else {
      fail(message);
    }
  }

  static void workerEntry(void* argument) { static_cast<Impl*>(argument)->run(); }

  static int scanCallback(ble_gap_event* event, void* argument) {
    Impl& self = *static_cast<Impl*>(argument);
    if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
      Lock guard(self.mutex);
      self.scanCompleted = true;
      return 0;
    }
    if (event->type != BLE_GAP_EVENT_DISC) return 0;
    const auto& result = event->disc;
    Candidate candidate;
    candidate.native = result.addr;
    candidate.rssi = result.rssi;
    formatAddress(result.addr, candidate.address);
    ble_hs_adv_fields fields = {};
    if (ble_hs_adv_parse_fields(&fields, result.data, result.length_data) == 0) {
      if (fields.name && fields.name_len) {
        const size_t size = std::min<size_t>(fields.name_len, sizeof candidate.name - 1);
        memcpy(candidate.name, fields.name, size);
        candidate.name[size] = 0;
      }
      for (uint8_t i = 0; i < fields.num_uuids16; ++i)
        candidate.heartRate |= fields.uuids16[i].value == 0x180D;
    }
    Lock guard(self.mutex);
    if (!self.scanRunning || self.scanGeneration != self.generation) return 0;
    if (strcmp(candidate.address, self.target) == 0) {
      if (!candidate.name[0] && self.targetFound) copyText(candidate.name, self.foundTarget.name);
      candidate.heartRate |= self.targetFound && self.foundTarget.heartRate;
      self.foundTarget = candidate;
      self.targetFound = true;
    }
    if (!self.scanIsUser) return 0;
    for (size_t i = 0; i < self.candidateCount; ++i) {
      Candidate& existing = self.candidates[i];
      if (strcmp(existing.address, candidate.address) == 0 && existing.native.type == candidate.native.type) {
        if (candidate.name[0]) copyText(existing.name, candidate.name);
        existing.rssi = candidate.rssi;
        existing.heartRate |= candidate.heartRate;
        return 0;
      }
    }
    if (self.candidateCount < kCandidateLimit) self.candidates[self.candidateCount++] = candidate;
    return 0;
  }

  static int gapCallback(ble_gap_event* event, void* argument) {
    Impl& self = *static_cast<Impl*>(argument);
    if (event->type == BLE_GAP_EVENT_NOTIFY_RX) {
      const size_t length = OS_MBUF_PKTLEN(event->notify_rx.om);
      uint8_t data[kNotificationLimit];
      if (length > sizeof data || os_mbuf_copydata(event->notify_rx.om, 0, length, data) != 0) {
        Lock guard(self.mutex);
        if (self.current(event->notify_rx.conn_handle)) copyText(self.lastError, "Oversized heart rate notification");
        return 0;
      }
      const heart_rate::Measurement decoded = heart_rate::decode(data, length);
      char hex[132] = {};
      const size_t shown = std::min<size_t>(length, 64);
      for (size_t i = 0; i < shown; ++i) snprintf(hex + i * 2, sizeof hex - i * 2, "%02X", data[i]);
      if (shown != length) memcpy(hex + shown * 2, "...", 4);
      Lock guard(self.mutex);
      if (!self.current(event->notify_rx.conn_handle) || !self.subscribed ||
          event->notify_rx.attr_handle != self.session.measurementHandle) return 0;
      copyText(self.lastHex, hex);
      if (!self.reading.accept(decoded, millis())) copyText(self.lastError, heart_rate::errorText(decoded.error));
      else if (decoded.bpm > 0) self.lastError[0] = 0;
      return 0;
    }
    Lock guard(self.mutex);
    Session& session = self.session;
    if (!session.used) return 0;
    if (event->type == BLE_GAP_EVENT_CONNECT) {
      session.connecting = false;
      if (event->connect.status != 0) {
        session.gapClosed = true;
        session.closeRequested = true;
        if (session.generation == self.generation) {
          snprintf(self.lastError, sizeof self.lastError, "BLE connection failed (%d)", event->connect.status);
          copyText(self.state, "retrying");
        }
      } else {
        session.connection = event->connect.conn_handle;
        if (session.generation != self.generation || session.closeRequested) session.closeRequested = true;
        else {
          self.connected = true;
          copyText(self.state, "discovering");
          session.next = Operation::HeartService;
        }
      }
    } else if (event->type == BLE_GAP_EVENT_DISCONNECT) {
      if (session.connection != event->disconnect.conn.conn_handle) return 0;
      session.connection = BLE_HS_CONN_HANDLE_NONE;
      session.connecting = false;
      session.gapClosed = true;
      session.closeRequested = true;
      session.next = Operation::None;
      if (session.generation == self.generation) {
        self.connected = false;
        if (session.wasSubscribed) {
          self.hasDisconnect = true;
          self.lastDisconnectMs = millis();
          ++self.disconnects;
        }
        self.clearReading();
        if (!self.lastError[0])
          snprintf(self.lastError, sizeof self.lastError, "BLE disconnected (%d)", event->disconnect.reason);
        copyText(self.state, "retrying");
      }
    }
    return 0;
  }

  static int serviceCallback(uint16_t handle, const ble_gatt_error* error, const ble_gatt_svc* service, void* argument) {
    Impl& self = *static_cast<Impl*>(argument);
    Lock guard(self.mutex);
    Session& session = self.session;
    if (error->status != 0) session.gattPending = false;
    if (!self.current(handle)) return 0;
    const bool battery = session.inflight == Operation::BatteryService;
    if (session.optionalTimedOut && battery) return 0;
    if (error->status == 0 && service) {
      if (battery) {
        if (!session.batteryStart) { session.batteryStart = service->start_handle; session.batteryEnd = service->end_handle; }
      } else {
        ++session.serviceCount;
        session.serviceStart = service->start_handle;
        session.serviceEnd = service->end_handle;
      }
    } else if (error->status == BLE_HS_EDONE) {
      if (battery) session.next = session.batteryStart ? Operation::BatteryCharacteristic : Operation::None;
      else if (session.serviceCount != 1) self.fail("Expected one heart rate service (180D)");
      else { session.descriptorEnd = session.serviceEnd; session.next = Operation::HeartCharacteristics; }
    } else if (error->status != 0) self.operationFailed("BLE service discovery failed");
    return 0;
  }

  static int characteristicCallback(uint16_t handle, const ble_gatt_error* error, const ble_gatt_chr* characteristic, void* argument) {
    Impl& self = *static_cast<Impl*>(argument);
    Lock guard(self.mutex);
    Session& session = self.session;
    if (error->status != 0) session.gattPending = false;
    if (!self.current(handle)) return 0;
    const bool battery = session.inflight == Operation::BatteryCharacteristic;
    if (session.optionalTimedOut && battery) return 0;
    if (error->status == 0 && characteristic) {
      if (battery) {
        if (characteristic->properties & BLE_GATT_CHR_PROP_READ) session.batteryHandle = characteristic->val_handle;
      } else if (ble_uuid_cmp(&characteristic->uuid.u, &kMeasurement.u) == 0) {
        ++session.measurementCount;
        session.measurementHandle = characteristic->val_handle;
        session.notifySupported = (characteristic->properties & BLE_GATT_CHR_PROP_NOTIFY) != 0;
      } else if (session.measurementHandle && characteristic->def_handle > session.measurementHandle &&
                 characteristic->def_handle <= session.descriptorEnd) {
        session.descriptorEnd = characteristic->def_handle - 1;
      }
    } else if (error->status == BLE_HS_EDONE) {
      if (battery) session.next = session.batteryHandle ? Operation::BatteryRead : Operation::None;
      else if (session.measurementCount != 1 || !session.notifySupported)
        self.fail("Expected one notifying heart rate measurement (2A37)");
      else if (session.descriptorEnd <= session.measurementHandle) self.fail("Heart rate CCCD missing");
      else session.next = Operation::Cccd;
    } else if (error->status != 0) self.operationFailed("BLE characteristic discovery failed");
    return 0;
  }

  static int descriptorCallback(uint16_t handle, const ble_gatt_error* error, uint16_t, const ble_gatt_dsc* descriptor, void* argument) {
    Impl& self = *static_cast<Impl*>(argument);
    Lock guard(self.mutex);
    Session& session = self.session;
    if (error->status != 0) session.gattPending = false;
    if (!self.current(handle)) return 0;
    if (error->status == 0 && descriptor && ble_uuid_cmp(&descriptor->uuid.u, &kCccd.u) == 0) {
      session.cccdHandle = descriptor->handle;
      ++session.cccdCount;
    } else if (error->status == BLE_HS_EDONE) {
      if (session.cccdCount != 1) self.fail("Expected one heart rate CCCD (2902)");
      else session.next = Operation::Subscribe;
    } else if (error->status != 0) self.fail("BLE descriptor discovery failed");
    return 0;
  }

  static int attributeCallback(uint16_t handle, const ble_gatt_error* error, ble_gatt_attr* attribute, void* argument) {
    Impl& self = *static_cast<Impl*>(argument);
    Lock guard(self.mutex);
    Session& session = self.session;
    session.gattPending = false;  // Read/write callbacks are single completions.
    if (!self.current(handle)) return 0;
    if (session.inflight == Operation::BatteryRead) {
      if (session.optionalTimedOut) return 0;
      uint8_t value = 0;
      if (error->status == 0 && attribute && attribute->om && OS_MBUF_PKTLEN(attribute->om) == 1 &&
          os_mbuf_copydata(attribute->om, 0, 1, &value) == 0 && value <= 100) self.battery = value;
      session.next = Operation::None;
    } else if (error->status == 0) {
      self.subscribed = session.wasSubscribed = true;
      self.hasDisconnect = false;
      self.subscribedMs = millis();
      self.lastError[0] = 0;
      copyText(self.state, "connected");
      session.next = Operation::BatteryService;
    } else self.fail("Heart rate notification subscription failed");
    return 0;
  }

  void startOperation(Operation operation) {
    Session copy;
    {
      Lock guard(mutex);
      if (session.closeRequested || session.generation != generation || session.gattPending) return;
      session.next = Operation::None;
      session.inflight = operation;
      session.gattPending = true;
      session.operationMs = millis();
      session.optionalTimedOut = false;
      copy = session;
    }
    int result = BLE_HS_EINVAL;
    const uint8_t enableNotifications[] = {1, 0};
    switch (operation) {
      case Operation::HeartService:
        result = ble_gattc_disc_svc_by_uuid(copy.connection, &kHeartService.u, serviceCallback, this); break;
      case Operation::HeartCharacteristics:
        result = ble_gattc_disc_all_chrs(copy.connection, copy.serviceStart, copy.serviceEnd, characteristicCallback, this); break;
      case Operation::Cccd:
        result = ble_gattc_disc_all_dscs(copy.connection, copy.measurementHandle, copy.descriptorEnd, descriptorCallback, this); break;
      case Operation::Subscribe:
        result = ble_gattc_write_flat(copy.connection, copy.cccdHandle, enableNotifications, sizeof enableNotifications, attributeCallback, this); break;
      case Operation::BatteryService:
        result = ble_gattc_disc_svc_by_uuid(copy.connection, &kBatteryService.u, serviceCallback, this); break;
      case Operation::BatteryCharacteristic:
        result = ble_gattc_disc_chrs_by_uuid(copy.connection, copy.batteryStart, copy.batteryEnd, &kBattery.u, characteristicCallback, this); break;
      case Operation::BatteryRead:
        result = ble_gattc_read(copy.connection, copy.batteryHandle, attributeCallback, this); break;
      default: break;
    }
    if (result != 0) {
      Lock guard(mutex);
      session.gattPending = false;
      if (session.generation == generation) operationFailed("Unable to start BLE GATT operation");
    }
  }

  void startConnection(const Candidate& candidate) {
    uint8_t ownType = 0;
    if (ble_hs_id_infer_auto(0, &ownType) != 0) {
      Lock guard(mutex);
      targetFound = false;
      retryAt = millis() + kRetryMs;
      copyText(state, "retrying");
      copyText(lastError, "BLE host is not synchronized");
      return;
    }
    {
      Lock guard(mutex);
      if (session.used || strcmp(target, candidate.address) != 0 || !scanAllowed) return;
      session = Session{};
      session.used = session.connecting = true;
      session.generation = generation;
      session.operationMs = millis();
      clearReading();
      connected = false;
      copyText(name, candidate.name);
      addressType = candidate.native.type;
      rssi = candidate.rssi;
      copyText(state, "connecting");
      targetFound = false;
    }
    const int result = ble_gap_connect(ownType, &candidate.native, kConnectMs, nullptr, gapCallback, this);
    if (result != 0) {
      Lock guard(mutex);
      session.connecting = false;
      session.gapClosed = session.closeRequested = true;
      if (session.generation == generation) {
        snprintf(lastError, sizeof lastError, "Unable to start BLE connection (%d)", result);
        copyText(state, "retrying");
      }
    }
  }

  void startScan(bool user) {
    uint8_t ownType = 0;
    if (ble_hs_id_infer_auto(0, &ownType) != 0) {
      Lock guard(mutex);
      retryAt = millis() + kRetryMs;
      if (user) { userScanRequested = false; copyText(scanState, "error"); copyText(scanError, "BLE host is not synchronized"); }
      return;
    }
    {
      Lock guard(mutex);
      if (!scanAllowed || scanRunning || scanStarting) return;
      scanStarting = scanRunning = true;
      scanCompleted = false;
      scanIsUser = user;
      scanGeneration = generation;
      scanStartedMs = millis();
      targetFound = false;
      if (user) {
        candidateCount = 0;
        userScanRequested = false;
        scanError[0] = 0;
        copyText(scanState, "scanning");
        // A connected sensor normally no longer advertises; keep it selectable.
        if (subscribed) {
          Candidate& entry = candidates[candidateCount++];
          entry = Candidate{};
          copyText(entry.name, name);
          copyText(entry.address, target);
          entry.native.type = addressType;
          entry.rssi = rssi;
          entry.heartRate = true;
        }
      } else if (!connected) copyText(state, "scanning");
    }
    ble_gap_disc_params parameters = {};
    parameters.itvl = 160;    // 100 ms, units of 0.625 ms.
    parameters.window = 80;   // 50 ms leaves radio airtime for the persistent AP.
    parameters.passive = 0;   // Scan responses carry many devices' names.
    parameters.filter_duplicates = 1;
    const int result = ble_gap_disc(ownType, user ? 5000 : 3000, &parameters, scanCallback, this);
    Lock guard(mutex);
    scanStarting = false;
    if (result != 0) {
      scanRunning = false;
      retryAt = millis() + kRetryMs;
      if (user) { copyText(scanState, "error"); snprintf(scanError, sizeof scanError, "BLE scan failed (%d)", result); }
      if (!connected) { copyText(state, "retrying"); snprintf(lastError, sizeof lastError, "BLE scan failed (%d)", result); }
    }
  }

  void finishScan(bool interrupted) {
    Lock guard(mutex);
    if (!scanRunning) return;
    scanRunning = scanStarting = false;
    if (scanIsUser) {
      if (interrupted) { userScanRequested = true; copyText(scanState, "queued"); }
      else copyText(scanState, "done");
    }
    if (scanGeneration != generation || interrupted) targetFound = false;
    if (!session.used) {
      retryAt = millis() + kRetryMs;
      copyText(state, target[0] ? "retrying" : "unconfigured");
      if (!targetFound && target[0] && !interrupted) copyText(lastError, "Heart rate device not found; retrying");
    }
  }

  void tick() {
    uint32_t now = 0;
    bool cancelScan = false;
    bool completeScan = false;
    bool close = false;
    bool cancelConnection = false;
    uint16_t closeHandle = BLE_HS_CONN_HANDLE_NONE;
    bool connect = false;
    Candidate targetCopy;
    bool startUserScan = false;
    bool startBackgroundScan = false;
    Operation operation = Operation::None;
    const bool actualScan = ble_gap_disc_active() != 0;
    {
      Lock guard(mutex);
      // A GAP callback may publish a newer timestamp while the worker waits
      // for the host or this mutex. Read the clock after taking the snapshot
      // lock so unsigned elapsed-time arithmetic cannot underflow.
      now = millis();
      if (scanRunning && !scanStarting) {
        cancelScan = !scanAllowed || scanGeneration != generation || now - scanStartedMs > 7000;
        completeScan = scanCompleted || !actualScan;
      }
      if (session.used) {
        if (session.generation != generation) session.closeRequested = true;
        if (!session.closeRequested && session.connecting && now - session.operationMs >= kConnectMs + 500)
          fail("BLE connection timed out");
        if (!session.closeRequested && session.gattPending && now - session.operationMs >= kGattMs) {
          if (optionalOperation(session.inflight)) session.optionalTimedOut = true;
          else fail("BLE service discovery or subscription timed out");
        }
        if (!session.closeRequested && subscribed && reading.expired(now, subscribedMs))
          fail("No valid heart rate notification for 10 seconds");
        if (session.closeRequested && !session.gapClosed &&
            (!session.closeSent || now - session.closeMs >= 2000)) {
          close = true;
          closeHandle = session.connection;
          cancelConnection = session.connecting && closeHandle == BLE_HS_CONN_HANDLE_NONE;
          session.closeSent = true;
          session.closeMs = now;
        }
        if (session.gapClosed && !session.gattPending) {
          session = Session{};
          retryAt = now + kRetryMs;
          if (!target[0]) copyText(state, "unconfigured");
        } else if (!session.closeRequested && !session.gattPending) operation = session.next;
      }
      if (!scanRunning && !scanStarting && !actualScan && scanAllowed && !close) {
        // Optional battery discovery must not postpone a user-requested scan.
        const bool discoveryBusy = session.used && !subscribed;
        if (userScanRequested && !discoveryBusy) startUserScan = true;
        else if (!session.used && targetFound && scanGeneration == generation) { connect = true; targetCopy = foundTarget; }
        else if (!session.used && target[0] && static_cast<int32_t>(now - retryAt) >= 0) startBackgroundScan = true;
      }
    }
    if (cancelScan) { ble_gap_disc_cancel(); finishScan(true); }
    else if (completeScan) finishScan(false);
    if (close) {
      int result = 0;
      if (cancelConnection) result = ble_gap_conn_cancel();
      else if (closeHandle != BLE_HS_CONN_HANDLE_NONE) result = ble_gap_terminate(closeHandle, BLE_ERR_REM_USER_CONN_TERM);
      if (result == BLE_HS_ENOTCONN || (result == BLE_HS_EALREADY && cancelConnection)) {
        Lock guard(mutex);
        // No procedure/connection remains to deliver a GAP callback.
        session.gapClosed = true;
        session.connecting = false;
        session.connection = BLE_HS_CONN_HANDLE_NONE;
        if (session.generation == generation) connected = false;
      }
    }
    if (operation != Operation::None) startOperation(operation);
    if (startUserScan) startScan(true);
    else if (connect) startConnection(targetCopy);
    else if (startBackgroundScan) startScan(false);
  }

  void run() {
    if (!BLEDevice::init("C521M")) {
      { Lock guard(mutex); copyText(state, "error"); copyText(lastError, "BLE initialization failed"); }
      vTaskDelete(nullptr);
      return;
    }
    {
      Lock guard(mutex);
      initialized = true;
      retryAt = millis();
      copyText(state, target[0] ? "retrying" : "unconfigured");
    }
    for (;;) { tick(); vTaskDelay(pdMS_TO_TICKS(20)); }
  }
};

BleService bleService;

void BleService::begin(const String& address) {
  if (impl_) { configure(address); return; }
  impl_ = new (std::nothrow) Impl;
  if (!impl_) return;
  impl_->mutex = xSemaphoreCreateMutex();
  if (!impl_->mutex) { delete impl_; impl_ = nullptr; return; }
  configure(address);
  if (xTaskCreatePinnedToCore(Impl::workerEntry, "ble-service", 6144, impl_, 1, &impl_->task, 1) != pdPASS) {
    Lock guard(impl_->mutex);
    copyText(impl_->state, "error");
    copyText(impl_->lastError, "Unable to create BLE worker");
  }
}

void BleService::configure(const String& address) {
  if (!impl_) return;
  char normalized[18] = {};
  const bool valid = normalizeAddress(address, normalized);
  Lock guard(impl_->mutex);
  if (valid && strcmp(impl_->target, normalized) == 0) return;
  ++impl_->generation;
  copyText(impl_->target, valid ? normalized : "");
  impl_->clearReading();
  impl_->connected = false;
  impl_->hasDisconnect = false;
  impl_->targetFound = false;
  impl_->name[0] = 0;
  impl_->addressType = -1;
  impl_->rssi = 0;
  impl_->retryAt = millis();
  copyText(impl_->state, valid ? "retrying" : "unconfigured");
  copyText(impl_->lastError, valid || address.isEmpty() ? "" : "Invalid heart rate MAC address");
}

void BleService::setScanAllowed(bool allowed) {
  if (!impl_) return;
  Lock guard(impl_->mutex);
  impl_->scanAllowed = allowed;
}

bool BleService::scanBusy() {
  if (!impl_) return false;
  bool initialized;
  bool reserved;
  { Lock guard(impl_->mutex); initialized = impl_->initialized; reserved = impl_->scanStarting || impl_->scanRunning; }
  return reserved || (initialized && ble_gap_disc_active() != 0);
}

bool BleService::requestScan() {
  if (!impl_) return false;
  Lock guard(impl_->mutex);
  if (!impl_->initialized || impl_->userScanRequested || (impl_->scanRunning && impl_->scanIsUser)) return false;
  impl_->userScanRequested = true;
  copyText(impl_->scanState, "queued");
  impl_->scanError[0] = 0;
  return true;
}

BleSnapshot BleService::snapshot() {
  BleSnapshot result;
  if (!impl_) return result;
  Lock guard(impl_->mutex);
  result.subscribed = impl_->subscribed;
  result.ready = impl_->reading.ready;
  result.valid = impl_->reading.valid;
  result.hasDisconnect = impl_->hasDisconnect;
  result.bpm = impl_->reading.bpm;
  result.receivedMs = impl_->reading.receivedMs;
  result.notifications = impl_->reading.notifications;
  result.disconnects = impl_->disconnects;
  result.lastDisconnectMs = impl_->lastDisconnectMs;
  result.name = impl_->name;
  result.address = impl_->target;
  return result;
}

void BleService::appendStatus(cJSON* object, uint32_t now) {
  if (!object) return;
  struct View {
    char state[24] = "error", name[64] = {}, address[18] = {}, error[128] = "BLE service unavailable", hex[132] = {};
    int addressType = -1, rssi = 0, battery = -1;
    bool connected = false, subscribed = false;
    uint32_t disconnects = 0;
    heart_rate::Reading reading;
  } view;
  if (impl_) {
    Lock guard(impl_->mutex);
    copyText(view.state, impl_->state); copyText(view.name, impl_->name); copyText(view.address, impl_->target);
    copyText(view.error, impl_->lastError); copyText(view.hex, impl_->lastHex);
    view.addressType = impl_->addressType; view.rssi = impl_->rssi; view.battery = impl_->battery;
    view.connected = impl_->connected; view.subscribed = impl_->subscribed;
    view.disconnects = impl_->disconnects; view.reading = impl_->reading;
  }
  now = millis();
  // cJSON allocations happen only after releasing the service mutex.
  cJSON_AddStringToObject(object, "state", view.state);
  cJSON_AddBoolToObject(object, "connected", view.connected);
  cJSON_AddBoolToObject(object, "subscribed", view.subscribed);
  cJSON_AddBoolToObject(object, "ready", view.reading.ready);
  if (view.reading.valid) cJSON_AddNumberToObject(object, "bpm", view.reading.bpm); else cJSON_AddNullToObject(object, "bpm");
  if (view.battery >= 0) cJSON_AddNumberToObject(object, "battery", view.battery); else cJSON_AddNullToObject(object, "battery");
  cJSON_AddStringToObject(object, "name", view.name);
  cJSON_AddStringToObject(object, "address", view.address);
  if (view.addressType >= 0) cJSON_AddStringToObject(object, "address_type", addressTypeName(view.addressType));
  else cJSON_AddNullToObject(object, "address_type");
  if (view.addressType >= 0) cJSON_AddNumberToObject(object, "rssi", view.rssi); else cJSON_AddNullToObject(object, "rssi");
  cJSON_AddNumberToObject(object, "notifications", view.reading.notifications);
  cJSON_AddNumberToObject(object, "consecutive", view.reading.consecutive);
  if (view.reading.hasReceived) cJSON_AddNumberToObject(object, "age_ms", static_cast<uint32_t>(now - view.reading.receivedMs));
  else cJSON_AddNullToObject(object, "age_ms");
  cJSON_AddNumberToObject(object, "disconnects", view.disconnects);
  cJSON_AddStringToObject(object, "last_error", view.error);
  cJSON_AddStringToObject(object, "last_notification_hex", view.hex);
}

void BleService::appendScan(cJSON* object) {
  if (!object) return;
  struct View { char state[16] = "error", error[96] = "BLE service unavailable"; size_t count = 0; Candidate results[kCandidateLimit]; };
  std::unique_ptr<View> view(new (std::nothrow) View);
  if (!view) { cJSON_AddStringToObject(object, "state", "error"); cJSON_AddStringToObject(object, "error", "Out of memory"); return; }
  if (impl_) {
    Lock guard(impl_->mutex);
    copyText(view->state, impl_->scanState); copyText(view->error, impl_->scanError);
    view->count = impl_->candidateCount;
    std::copy_n(impl_->candidates, view->count, view->results);
  }
  std::sort(view->results, view->results + view->count, [](const Candidate& a, const Candidate& b) {
    if (a.heartRate != b.heartRate) return a.heartRate;
    return a.rssi > b.rssi;
  });
  cJSON_AddStringToObject(object, "state", view->state);
  cJSON_AddStringToObject(object, "error", view->error);
  cJSON* results = cJSON_AddArrayToObject(object, "results");
  if (!results) return;
  for (size_t i = 0; i < view->count; ++i) {
    const Candidate& candidate = view->results[i];
    cJSON* item = cJSON_CreateObject();
    if (!item) break;
    cJSON_AddStringToObject(item, "name", candidate.name);
    cJSON_AddStringToObject(item, "address", candidate.address);
    cJSON_AddStringToObject(item, "address_type", addressTypeName(candidate.native.type));
    cJSON_AddNumberToObject(item, "rssi", candidate.rssi);
    cJSON_AddBoolToObject(item, "heart_rate", candidate.heartRate);
    cJSON_AddItemToArray(results, item);
  }
}
