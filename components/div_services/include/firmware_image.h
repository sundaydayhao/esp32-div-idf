#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
static inline bool firmware_matches_image(const uint8_t* data, size_t size,
                                          uint16_t chip, const char* project) {
  size_t length = project ? strlen(project) : 0;
  if (!data || size < 288 || !length || length >= 32) return false;
  if (data[0] != 0xe9 || (data[12] | ((uint16_t)data[13] << 8)) != chip)
    return false;
  if (data[32] != 0x32 || data[33] != 0x54 || data[34] != 0xcd ||
      data[35] != 0xab)
    return false;
  return memcmp(data + 80, project, length) == 0 && data[80 + length] == 0;
}
