#pragma once

#include <WString.h>
#include <cstdio>
#include "IRremoteESP8266.h"

inline String uint64ToString(uint64_t value, uint8_t base = 10) {
  char buf[32];
  if (base == 16) {
    snprintf(buf, sizeof(buf), "%llX", (unsigned long long)value);
  } else {
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)value);
  }
  return String(buf);
}

inline String resultToHumanReadableBasic(const decode_results* results) {
  if (!results) {
    return String("UNKNOWN");
  }
  String out = String(typeToString(results->decode_type));
  out += " 0x";
  out += uint64ToString(results->value, 16);
  return out;
}
