#pragma once
#include <Arduino.h>
#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error This firmware requires ESP32-S3 native USB Host and BLE.
#endif
#if ARDUINO_USB_CDC_ON_BOOT || ARDUINO_USB_MSC_ON_BOOT || ARDUINO_USB_DFU_ON_BOOT
#error Native USB must be reserved for Host. Use the external USB-UART bridge.
#endif
namespace board {
constexpr int machineTx = 17, machineRx = 18, startPin = 4, stopPin = 5;
constexpr uint32_t machineBaud = 57600;
constexpr uint16_t serverPort = 9999;
constexpr char firmwareVersion[] = "0.1.1";
constexpr char defaultServer[] = "192.168.0.100";
constexpr char defaultHeartAddress[] = ""; // Select the sensor in the portal on first boot.
}
