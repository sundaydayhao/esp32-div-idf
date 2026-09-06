#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
static inline size_t dns_reply(uint8_t* packet, size_t size, size_t capacity,
                               uint32_t address) {
  if (!packet || size < 12 || size > capacity || (packet[2] & 0xf8) ||
      packet[4] || packet[5] != 1)
    return 0;
  size_t pos = 12;
  while (pos < size && packet[pos]) {
    uint8_t length = packet[pos++];
    if (length > 63 || pos + length >= size || pos + length - 12 > 253)
      return 0;
    pos += length;
  }
  if (pos + 5 > size) return 0;
  ++pos;
  uint16_t type = ((uint16_t)packet[pos] << 8) | packet[pos + 1];
  bool answer = (type == 1 || type == 255) && packet[pos + 2] == 0 &&
                packet[pos + 3] == 1;
  pos += 4;
  if (pos + (answer ? 16 : 0) > capacity) return 0;
  packet[2] = 0x81;
  packet[3] = 0x80;
  packet[6] = 0;
  packet[7] = answer;
  for (size_t i = 8; i < 12; ++i) packet[i] = 0;
  if (answer) {
    const uint8_t record[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4};
    for (size_t i = 0; i < sizeof(record); ++i) packet[pos++] = record[i];
    for (unsigned shift = 0; shift < 32; shift += 8)
      packet[pos++] = address >> shift;
  }
  return pos;
}
