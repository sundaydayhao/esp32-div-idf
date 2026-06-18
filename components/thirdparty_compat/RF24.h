#pragma once

#include <Arduino.h>
#include <cstdint>

#define RF24_PA_MAX 3
#define RF24_2MBPS 2
#define RF24_CRC_DISABLED 0

class RF24 {
 public:
  RF24(uint8_t, uint8_t, uint32_t = 16000000UL) {}
  bool begin() { return true; }
  void powerDown() {}
  void setChannel(uint8_t) {}
  void setPALevel(uint8_t, bool = false) {}
  void setDataRate(uint8_t) {}
  void setCRCLength(uint8_t) {}
  void startConstCarrier(uint8_t, uint8_t) {}
  void setAutoAck(bool) {}
  void stopListening() {}
  void setRetries(uint8_t, uint8_t) {}
  void printPrettyDetails() {}
};
