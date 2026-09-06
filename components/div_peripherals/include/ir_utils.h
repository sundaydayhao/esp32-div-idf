#include "c_util.h"
#pragma once

#include "ir_protocol.h"
#include "stdio.h"
#include "text_format.h"

static inline text_t uint64ToString(uint64_t value, uint8_t base) {
  char buf[32];
  if (base == 16) {
    snprintf(buf, sizeof(buf), "%llX", (unsigned long long)value);
  } else {
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)value);
  }
  return TEXT(buf);
}

static inline text_t resultToHumanReadableBasic(const IrFrame* results) {
  if (!results) {
    return TEXT("UNKNOWN");
  }
  text_t out = TEXT(typeToString(results->decode_type));
  text_append(&(out), TEXT(" 0x"));
  text_append(&(out), uint64ToString(results->value, 16));
  return out;
}
