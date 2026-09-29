#include "ccid_service.h"
#include "ccid_codec.h"

#include <new>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

namespace {
constexpr uint32_t kCommandDeadlineMs = 3000;
constexpr uint32_t kPollMs = 300;
constexpr uint32_t kRetryMs = 3000;
bool due(uint32_t now, uint32_t when) { return int32_t(now - when) >= 0; }

String hexBytes(const uint8_t* bytes, size_t length) {
  static const char digits[] = "0123456789ABCDEF";
  String result;
  result.reserve(length * 2);
  for (size_t i = 0; i < length; ++i) {
    result += digits[bytes[i] >> 4];
    result += digits[bytes[i] & 15];
  }
  return result;
}
String usbError(const char* operation, esp_err_t code) {
  return String(operation) + ": " + esp_err_to_name(code);
}
struct CardEvent { char uid[511]; };
}  // namespace

struct CcidService::Impl {
  SemaphoreHandle_t mutex = nullptr;
  QueueHandle_t cards = nullptr;
  CcidSnapshot data;
  usb_host_client_handle_t client = nullptr;
  usb_device_handle_t device = nullptr;
  ccid::Descriptor descriptor;
  ccid::Presentation presentation;
  usb_transfer_t* tx = nullptr;
  usb_transfer_t* rx = nullptr;
  usb_transfer_t* interrupt = nullptr;
  bool txPending = false;
  bool rxPending = false;
  bool interruptPending = false;
  bool interruptDone = false;
  bool claimed = false;
  bool gone = false;
  bool recover = false;
  bool closing = false;
  bool rescan = true;
  bool pollRequested = true;
  bool cleanupWarning = false;
  uint8_t sequence = 0;
  uint8_t rejectedAddress = 0;
  size_t rxCapacity = 0;
  uint32_t nextPoll = 0;
  uint32_t nextRead = 0;
  uint32_t nextScan = 0;
  uint32_t interruptRetry = 0;
  uint32_t cleanupStarted = 0;
  uint32_t nextCleanup = 0;

  template <class F> void update(F function) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    function(data);
    xSemaphoreGive(mutex);
  }
  void error(const String& message, bool unavailable = false) {
    update([&](CcidSnapshot& s) {
      s.lastError = message;
      ++s.errors;
      if (unavailable) { s.ready = false; s.state = "reader_error"; }
    });
  }
  void observed(bool present) {
    presentation.observe(present);
    update([&](CcidSnapshot& s) {
      s.cardPresent = present;
      if (!present) { s.uid = ""; s.atr = ""; s.lastSw = -1; }
    });
  }

  static void clientEvent(const usb_host_client_event_msg_t* event, void* arg) {
    auto* self = static_cast<Impl*>(arg);
    if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
      self->rescan = true;
      if (self->rejectedAddress == event->new_dev.address) self->rejectedAddress = 0;
    } else if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE &&
               event->dev_gone.dev_hdl == self->device) {
      self->gone = true;
    }
  }
  static void transferDone(usb_transfer_t* transfer) {
    auto* self = static_cast<Impl*>(transfer->context);
    if (transfer == self->tx) self->txPending = false;
    else if (transfer == self->rx) self->rxPending = false;
    else if (transfer == self->interrupt) {
      self->interruptPending = false;
      self->interruptDone = true;
    }
  }
  bool submit(usb_transfer_t* transfer, bool& pending) {
    pending = true;
    const esp_err_t result = usb_host_transfer_submit(transfer);
    if (result == ESP_OK) return true;
    pending = false;
    error(usbError("USB transfer submit", result), true);
    recover = true;
    return false;
  }

  void serviceInterrupt() {
    if (!interrupt || gone || recover || closing) return;
    if (interruptDone) {
      interruptDone = false;
      if (interrupt->status == USB_TRANSFER_STATUS_COMPLETED) {
        const uint8_t* bytes = interrupt->data_buffer;
        if (interrupt->actual_num_bytes >= 2 && bytes[0] == 0x50) {
          // Slot zero: bit 0 = presence, bit 1 = state changed.
          observed((bytes[1] & 1) != 0);
          pollRequested = true;
        } else if (interrupt->actual_num_bytes >= 4 && bytes[0] == 0x51) {
          error("CCID hardware-error interrupt", true);
          pollRequested = true;
        }
      } else {
        error(String("USB interrupt status ") + int(interrupt->status));
        // Bulk status polling remains usable if interrupt delivery fails.
        usb_host_endpoint_halt(device, descriptor.interruptIn);
        usb_host_endpoint_flush(device, descriptor.interruptIn);
        usb_host_endpoint_clear(device, descriptor.interruptIn);
        interruptRetry = millis() + 5000;
      }
    }
    if (!interruptPending && due(millis(), interruptRetry)) {
      interrupt->num_bytes = descriptor.interruptPacket;
      interruptPending = true;
      const esp_err_t result = usb_host_transfer_submit(interrupt);
      if (result != ESP_OK) {
        interruptPending = false;
        error(usbError("USB interrupt submit", result));
        interruptRetry = millis() + 5000;
      }
    }
  }
  void pump() {
    usb_host_client_handle_events(client, pdMS_TO_TICKS(10));
    serviceInterrupt();
  }
  bool waitTransfer(bool& pending, uint32_t started) {
    while (pending && !gone && !recover) {
      if (uint32_t(millis() - started) >= kCommandDeadlineMs) {
        error("CCID command deadline; cancelling USB transfers", true);
        recover = true;
        return false;
      }
      pump();
    }
    return !gone && !recover && !pending;
  }

  // All calls are serialized in the client task. timeout_ms is intentionally
  // unused: this ESP-IDF host implementation does not implement transfer timeouts.
  bool command(uint8_t type, uint8_t responseType, ccid::Reply& reply,
               const uint8_t* payload = nullptr, size_t length = 0) {
    if (!device || gone || recover || closing) return false;
    const uint8_t requestSequence = sequence++;
    const size_t bytes = ccid::makeCommand(tx->data_buffer, descriptor.maxMessage,
                                         type, requestSequence, payload, length);
    if (!bytes) { error("CCID command exceeds reader capacity", true); return false; }
    tx->num_bytes = int(bytes);
    update([&](CcidSnapshot& s) { s.lastTx = hexBytes(tx->data_buffer, bytes); });
    const uint32_t started = millis();
    if (!submit(tx, txPending) || !waitTransfer(txPending, started)) return false;
    if (tx->status != USB_TRANSFER_STATUS_COMPLETED || tx->actual_num_bytes != tx->num_bytes) {
      error(String("CCID USB OUT failed, status ") + int(tx->status), true);
      recover = true;
      return false;
    }
    do {
      if (uint32_t(millis() - started) >= kCommandDeadlineMs) {
        error("CCID time extensions exceeded command deadline", true);
        recover = true;
        return false;
      }
      rx->num_bytes = int(rxCapacity);
      if (!submit(rx, rxPending) || !waitTransfer(rxPending, started)) return false;
      if (rx->status != USB_TRANSFER_STATUS_COMPLETED || rx->actual_num_bytes < 0) {
        error(String("CCID USB IN failed, status ") + int(rx->status), true);
        recover = true;
        return false;
      }
      const size_t received = size_t(rx->actual_num_bytes);
      update([&](CcidSnapshot& s) { s.lastRx = hexBytes(rx->data_buffer, received); });
      const auto decoded = ccid::decode(rx->data_buffer, received, responseType,
                                        requestSequence, reply);
      if (decoded == ccid::DecodeResult::Sequence) continue; // Drain a stale reply.
      if (decoded != ccid::DecodeResult::Ok || received > descriptor.maxMessage) {
        error(String("Invalid CCID reply, reason ") + int(decoded), true);
        recover = true;
        return false;
      }
      if (reply.extension()) continue; // Receive again; never resend the APDU.
      update([](CcidSnapshot& s) { s.lastUpdateMs = millis(); s.hasUpdate = true; });
      return true;
    } while (!gone && !recover);
    return false;
  }

  void apduStatus(const ccid::Reply& reply) {
    const int sw = reply.length >= 2 ?
        (int(reply.data[reply.length - 2]) << 8) | reply.data[reply.length - 1] : -1;
    update([&](CcidSnapshot& s) { s.lastSw = sw; });
  }
  void readCard() {
    nextRead = millis() + 1000;
    ccid::Reply reply;
    if (!command(ccid::kPowerOn, ccid::kDataBlock, reply)) return;
    if (reply.icc == 2) { observed(false); return; }
    if (reply.failed() || reply.icc != 0 || reply.length < 2) {
      error(String("Card activation failed, CCID error ") + reply.error);
      return;
    }
    update([&](CcidSnapshot& s) { s.atr = hexBytes(reply.data, reply.length); });
    if (!presentation.buzzerAttempted) {
      presentation.buzzerAttempted = true;
      if (!command(ccid::kXfrBlock, ccid::kDataBlock, reply,
                   ccid::kBuzzerApdu, sizeof(ccid::kBuzzerApdu))) return;
      apduStatus(reply);
      if (reply.failed() || reply.length < 2 ||
          reply.data[reply.length - 2] != 0x90 || reply.data[reply.length - 1] != 0) {
        // A rejected buzzer must not discard an otherwise readable UID.
        error("Buzzer APDU rejected; continuing UID read");
      }
    }
    if (!command(ccid::kXfrBlock, ccid::kDataBlock, reply,
                 ccid::kUidApdu, sizeof(ccid::kUidApdu))) return;
    apduStatus(reply);
    CardEvent event{};
    if (ccid::uidHex(reply, event.uid, sizeof(event.uid)) && presentation.present) {
      presentation.delivered = true;
      const char* uidText = event.uid;
      update([&](CcidSnapshot& s) {
        s.uid = uidText;
        ++s.cards;
        s.state = "card_read";
      });
      if (xQueueSend(cards, &event, 0) != pdTRUE)
        error("Card event queue full; UID event dropped");
    } else if (reply.icc == 2) {
      observed(false);
    } else {
      error("UID APDU failed or response does not end with 9000");
    }
    // Matches SCARD_UNPOWER_CARD; inactive-present does not rearm presentation.
    if (command(ccid::kPowerOff, ccid::kSlotStatus, reply) && reply.failed())
      error("CCID card power-off failed");
  }
  void poll() {
    pollRequested = false;
    nextPoll = millis() + kPollMs;
    ccid::Reply reply;
    if (!command(ccid::kGetSlotStatus, ccid::kSlotStatus, reply)) return;
    if (reply.failed()) {
      error(String("CCID slot status failed, error ") + reply.error, true);
      nextPoll = millis() + 1000;
      return;
    }
    observed(reply.icc != 2);
    update([&](CcidSnapshot& s) {
      s.ready = true;
      s.state = !presentation.present ? "ready_no_card" :
                presentation.delivered ? "card_read" : "reading_card";
    });
    if (presentation.needsRead() && due(millis(), nextRead)) readCard();
  }

  void configureTransfer(usb_transfer_t* transfer, uint8_t endpoint) {
    transfer->device_handle = device;
    transfer->bEndpointAddress = endpoint;
    transfer->callback = transferDone;
    transfer->context = this;
    transfer->timeout_ms = 0;
  }
  void scanDevices() {
    rescan = false;
    nextScan = millis() + kRetryMs;
    uint8_t addresses[16];
    int count = 0;
    if (usb_host_device_addr_list_fill(16, addresses, &count) != ESP_OK) return;
    for (int n = 0; n < count && !device; ++n) {
      if (addresses[n] == rejectedAddress) continue;
      usb_device_handle_t candidate = nullptr;
      if (usb_host_device_open(client, addresses[n], &candidate) != ESP_OK) continue;
      const usb_device_desc_t* devDescriptor = nullptr;
      if (usb_host_get_device_descriptor(candidate, &devDescriptor) != ESP_OK ||
          devDescriptor->idVendor != ccid::kVendor || devDescriptor->idProduct != ccid::kProduct) {
        usb_host_device_close(client, candidate);
        continue;
      }
      update([&](CcidSnapshot& s) { s.vid = devDescriptor->idVendor; s.pid = devDescriptor->idProduct; });
      const usb_config_desc_t* config = nullptr;
      const char* reason = "Cannot read USB configuration";
      const bool valid = usb_host_get_active_config_descriptor(candidate, &config) == ESP_OK &&
          ccid::parseConfiguration(reinterpret_cast<const uint8_t*>(config),
                                   config->wTotalLength, descriptor, reason);
      if (!valid) {
        error(reason ? reason : "Unsupported CCID descriptor", true);
        update([](CcidSnapshot& s) { s.state = "unsupported_reader"; });
        rejectedAddress = addresses[n];
        usb_host_device_close(client, candidate);
        continue;
      }
      device = candidate;
      gone = recover = closing = cleanupWarning = false;
      presentation = ccid::Presentation{};
      const esp_err_t result = usb_host_interface_claim(client, device,
          descriptor.interfaceNumber, descriptor.alternate);
      if (result != ESP_OK) { error(usbError("Claim CCID interface", result), true); recover = true; return; }
      claimed = true;
      rxCapacity = ((descriptor.maxMessage + descriptor.bulkInPacket - 1) /
                    descriptor.bulkInPacket) * descriptor.bulkInPacket;
      if (usb_host_transfer_alloc(ccid::kMaxMessage, 0, &tx) != ESP_OK ||
          usb_host_transfer_alloc(rxCapacity, 0, &rx) != ESP_OK ||
          (descriptor.interruptIn && usb_host_transfer_alloc(descriptor.interruptPacket, 0, &interrupt) != ESP_OK)) {
        error("Cannot allocate USB CCID transfers", true);
        recover = true;
        return;
      }
      configureTransfer(tx, descriptor.bulkOut);
      configureTransfer(rx, descriptor.bulkIn);
      if (interrupt) configureTransfer(interrupt, descriptor.interruptIn);
      update([&](CcidSnapshot& s) {
        s.state = "checking_reader";
        s.features = descriptor.features;
        s.maxMessageLength = descriptor.maxMessage;
      });
      interruptRetry = nextRead = nextPoll = millis();
      pollRequested = true;
      serviceInterrupt();
    }
  }

  void closeDevice() {
    if (!device) return;
    if (!closing) {
      closing = true;
      cleanupStarted = millis();
      nextCleanup = millis();
      observed(false);
      xQueueReset(cards);
      update([&](CcidSnapshot& s) { s.ready = false; s.state = gone ? "reader_disconnected" : "recovering"; });
      if (claimed) {
        const uint8_t endpoints[] = {descriptor.bulkIn, descriptor.bulkOut, descriptor.interruptIn};
        for (uint8_t endpoint : endpoints) if (endpoint) {
          // After physical removal these can return INVALID_STATE; completion
          // callbacks still own the transfers until all pending flags are clear.
          usb_host_endpoint_halt(device, endpoint);
          usb_host_endpoint_flush(device, endpoint);
        }
      }
    }
    if (txPending || rxPending || interruptPending) {
      if (!cleanupWarning && uint32_t(millis() - cleanupStarted) > 5000) {
        cleanupWarning = true;
        error("USB cancellation pending; buffers retained until callbacks finish", true);
      }
      return;
    }
    if (!due(millis(), nextCleanup)) return;
    nextCleanup = millis() + 1000;
    if (claimed) {
      const esp_err_t result = usb_host_interface_release(client, device, descriptor.interfaceNumber);
      if (result != ESP_OK) { error(usbError("Release CCID interface", result), true); return; }
      claimed = false;
    }
    const esp_err_t result = usb_host_device_close(client, device);
    if (result != ESP_OK) { error(usbError("Close CCID device", result), true); return; }
    device = nullptr;
    usb_host_transfer_free(tx); tx = nullptr;
    usb_host_transfer_free(rx); rx = nullptr;
    usb_host_transfer_free(interrupt); interrupt = nullptr;
    interruptDone = false;
    closing = false;
    gone = recover = false;
    nextScan = millis() + kRetryMs;
    rescan = false;
    update([](CcidSnapshot& s) {
      s.state = "waiting_reader";
      s.vid = s.pid = 0;
      s.features = s.maxMessageLength = 0;
    });
  }

  static void clientTask(void* arg) {
    auto* self = static_cast<Impl*>(arg);
    usb_host_client_config_t config{};
    config.is_synchronous = false;
    config.max_num_event_msg = 8;
    config.async.client_event_callback = clientEvent;
    config.async.callback_arg = self;
    const esp_err_t result = usb_host_client_register(&config, &self->client);
    if (result != ESP_OK) {
      self->error(usbError("Register USB client", result), true);
      vTaskDelete(nullptr);
      return;
    }
    self->update([](CcidSnapshot& s) { s.state = "waiting_reader"; });
    for (;;) {
      self->pump();
      if (self->device && (self->gone || self->recover || self->closing)) {
        self->closeDevice();
      } else if (!self->device) {
        if (self->rescan || due(millis(), self->nextScan)) self->scanDevices();
      } else if (self->pollRequested || due(millis(), self->nextPoll)) {
        self->poll();
      }
      vTaskDelay(1);
    }
  }
  static void daemonTask(void* arg) {
    auto* self = static_cast<Impl*>(arg);
    usb_host_config_t config{};
    config.skip_phy_setup = false;
    config.intr_flags = ESP_INTR_FLAG_LEVEL1;
    const esp_err_t result = usb_host_install(&config);
    if (result != ESP_OK) {
      self->error(usbError("Install USB Host", result), true);
      vTaskDelete(nullptr);
      return;
    }
    if (xTaskCreate(clientTask, "ccid_client", 6144, self, 4, nullptr) != pdPASS)
      self->error("Cannot start USB CCID client task", true);
    for (;;) {
      uint32_t flags = 0;
      usb_host_lib_handle_events(pdMS_TO_TICKS(100), &flags);
      if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
  }
};

CcidService ccidService;

void CcidService::begin() {
  if (impl_) return;
  impl_ = new (std::nothrow) Impl;
  if (!impl_) return;
  impl_->mutex = xSemaphoreCreateMutex();
  impl_->cards = xQueueCreate(4, sizeof(CardEvent));
  if (!impl_->mutex || !impl_->cards) {
    impl_->data.state = "reader_error";
    impl_->data.lastError = "Cannot allocate USB CCID service state";
    return;
  }
  impl_->data.state = "starting";
  if (xTaskCreate(Impl::daemonTask, "usb_daemon", 4096, impl_, 3, nullptr) != pdPASS)
    impl_->error("Cannot start USB host daemon", true);
}

CcidSnapshot CcidService::snapshot() {
  if (!impl_) return CcidSnapshot{};
  if (impl_->mutex) xSemaphoreTake(impl_->mutex, portMAX_DELAY);
  const CcidSnapshot result = impl_->data;
  if (impl_->mutex) xSemaphoreGive(impl_->mutex);
  return result;
}

bool CcidService::takeCard(String& hexUid) {
  if (!impl_ || !impl_->cards) return false;
  CardEvent event{};
  if (xQueueReceive(impl_->cards, &event, 0) != pdTRUE) return false;
  hexUid = event.uid;
  return true;
}

void CcidService::appendStatus(cJSON* object, uint32_t now) {
  if (!object) return;
  const CcidSnapshot s = snapshot();
  now = millis();
  cJSON_AddStringToObject(object, "state", s.state.c_str());
  cJSON_AddBoolToObject(object, "ready", s.ready);
  cJSON_AddBoolToObject(object, "card_present", s.cardPresent);
  cJSON_AddStringToObject(object, "uid", s.uid.c_str());
  cJSON_AddStringToObject(object, "last_error", s.lastError.c_str());
  cJSON_AddNumberToObject(object, "cards", s.cards);
  cJSON_AddNumberToObject(object, "errors", s.errors);
  if (s.hasUpdate) cJSON_AddNumberToObject(object, "age_ms", uint32_t(now - s.lastUpdateMs));
  else cJSON_AddNullToObject(object, "age_ms");
  cJSON_AddNumberToObject(object, "vid", s.vid);
  cJSON_AddNumberToObject(object, "pid", s.pid);
  cJSON_AddStringToObject(object, "atr", s.atr.c_str());
  cJSON_AddStringToObject(object, "last_tx", s.lastTx.c_str());
  cJSON_AddStringToObject(object, "last_rx", s.lastRx.c_str());
  cJSON_AddNumberToObject(object, "features", s.features);
  cJSON_AddNumberToObject(object, "max_message_length", s.maxMessageLength);
  if (s.lastSw >= 0) cJSON_AddNumberToObject(object, "last_sw", s.lastSw);
  else cJSON_AddNullToObject(object, "last_sw");
}
