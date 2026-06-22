#pragma once

#include "IRremoteESP8266.h"

class IRsend {
 public:
  explicit IRsend(uint16_t) {}
  void begin() {}
  void sendRaw(const uint16_t*, uint16_t, uint16_t) {}
  bool send(decode_type_t, uint64_t, uint16_t, uint16_t = 0) { return true; }
};
