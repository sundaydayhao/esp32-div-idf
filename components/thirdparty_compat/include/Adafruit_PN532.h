#pragma once

#include <cstdint>
#include <cstring>

#define PN532_BRTY_ISO14443A_106KBPS 0x00
#define PN532_MIFARE_ISO14443A 0x00

class Adafruit_PN532 {
 public:
  Adafruit_PN532(int8_t, int8_t, int8_t, int8_t) {}
  void begin() {}
  void SAMConfig() {}
  void setPassiveActivationRetries(uint8_t) {}
  uint32_t getFirmwareVersion() { return 0; }
  bool readPassiveTargetID(uint8_t, uint8_t*, uint8_t*, uint16_t = 0) { return false; }
  bool inDataExchange(const uint8_t*, uint8_t, uint8_t*, uint8_t*) { return false; }
  bool mifareclassic_AuthenticateBlock(uint8_t*, uint8_t, uint8_t, uint8_t, uint8_t*) { return false; }
  bool mifareclassic_ReadDataBlock(uint8_t, uint8_t*) { return false; }
  bool mifareclassic_WriteDataBlock(uint8_t, const uint8_t*) { return false; }
  bool mifareultralight_ReadPage(uint8_t, uint8_t*) { return false; }
  bool mifareultralight_WritePage(uint8_t, const uint8_t*) { return false; }
  bool AsTarget() { return false; }
  bool getDataTarget(uint8_t*, uint8_t*) { return false; }
  bool setDataTarget(const uint8_t*, uint8_t) { return false; }
};
