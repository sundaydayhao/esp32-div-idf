#pragma once

#include "IRremoteESP8266.h"

class IRrecv {
 public:
  explicit IRrecv(uint16_t) {}
  void enableIRIn() {}
  void disableIRIn() {}
  bool decode(decode_results*) { return false; }
  void resume() {}
};
