#pragma once

#include <Arduino.h>
#include <cstdint>

class RCSwitch {
 public:
  RCSwitch() = default;
  void resetAvailable() { available_ = false; }
  void setReceiveTolerance(int) {}
  void disableReceive() {}
  void disableTransmit() {}
  void enableTransmit(int) {}
  void enableReceive(int) {}
  void setRepeatTransmit(uint8_t repeat) { repeat_ = repeat; }
  void setProtocol(int) {}
  bool available() const { return available_; }
  unsigned long getReceivedValue() const { return receivedValue_; }
  unsigned int getReceivedBitlength() const { return receivedBits_; }
  unsigned int getReceivedProtocol() const { return receivedProtocol_; }
  void send(unsigned long value, unsigned int bits) {
    receivedValue_ = value;
    receivedBits_ = bits;
    available_ = false;
  }
  void send(unsigned long value, unsigned int bits, unsigned int) { send(value, bits); }

 private:
  bool available_ = false;
  unsigned long receivedValue_ = 0;
  unsigned int receivedBits_ = 0;
  unsigned int receivedProtocol_ = 1;
  uint8_t repeat_ = 1;
};
