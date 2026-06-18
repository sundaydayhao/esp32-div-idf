#pragma once

#include <Arduino.h>
#include <cstdint>

#define CC1101_TXFIFO 0x3F
#define CC1101_STX 0x35

class ELECHOUSE_CC1101_SRC_DRV {
 public:
  void setSpiPin(int, int, int, int) {}
  void Init() {}
  void setModulation(int) {}
  void setRxBW(double) {}
  void setGDO(int, int) {}
  void SetRx() {}
  void SetTx() {}
  void setSidle() {}
  void setMHZ(double) {}
  void setPA(int) {}
  int getRssi() const { return -127; }
  void SpiWriteReg(uint8_t, uint8_t) {}
  void SpiStrobe(uint8_t) {}
};

extern ELECHOUSE_CC1101_SRC_DRV ELECHOUSE_cc1101;
