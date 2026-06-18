#include "XPT2046_Touchscreen.h"

#include "Arduino.h"

namespace {

static constexpr uint16_t kAdcMax = 4095;

static uint16_t transfer12(SPIClass* spi, uint8_t csPin, uint8_t cmd) {
  if (spi == nullptr) {
    return 0;
  }

  spi->beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  if (csPin < 255) {
    digitalWrite(csPin, LOW);
  }
  (void)spi->transfer(cmd);
  uint16_t hi = spi->transfer(0x00);
  uint16_t lo = spi->transfer(0x00);
  if (csPin < 255) {
    digitalWrite(csPin, HIGH);
  }
  spi->endTransaction();

  uint16_t val = (uint16_t)((hi << 8) | lo);
  return (uint16_t)((val >> 3) & 0x0FFF);
}

}  // namespace

XPT2046_Touchscreen::XPT2046_Touchscreen(uint8_t csPin, uint8_t irqPin)
    : csPin_(csPin), irqPin_(irqPin), rotation_(0), spi_(nullptr) {}

void XPT2046_Touchscreen::begin(SPIClass& spi) {
  spi_ = &spi;
  if (csPin_ < 255) {
    pinMode(csPin_, OUTPUT);
    digitalWrite(csPin_, HIGH);
  }
  if (irqPin_ < 255) {
    pinMode(irqPin_, INPUT_PULLUP);
  }
}

void XPT2046_Touchscreen::setRotation(uint8_t rotation) {
  rotation_ = rotation;
}

bool XPT2046_Touchscreen::tirqTouched() const {
  if (irqPin_ >= 255) {
    return true;
  }
  return digitalRead(irqPin_) == LOW;
}

bool XPT2046_Touchscreen::touched() {
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t z = 0;
  return readRaw(x, y, z) && z > 0;
}

uint16_t XPT2046_Touchscreen::getTouchRawZ() {
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t z = 0;
  if (!readRaw(x, y, z)) {
    return 0;
  }
  return z;
}

void XPT2046_Touchscreen::getTouchRaw(uint16_t* x, uint16_t* y) {
  uint16_t rx = 0;
  uint16_t ry = 0;
  uint16_t rz = 0;
  if (!readRaw(rx, ry, rz)) {
    if (x) *x = 0;
    if (y) *y = 0;
    return;
  }
  if (x) *x = rx;
  if (y) *y = ry;
}

TS_Point XPT2046_Touchscreen::getPoint() {
  uint16_t rx = 0;
  uint16_t ry = 0;
  uint16_t rz = 0;
  if (!readRaw(rx, ry, rz)) {
    return TS_Point();
  }
  return TS_Point((int16_t)rx, (int16_t)ry, (int16_t)rz);
}

bool XPT2046_Touchscreen::readRaw(uint16_t& x, uint16_t& y, uint16_t& z) {
  if (spi_ == nullptr) {
    x = y = z = 0;
    return false;
  }

  if (csPin_ < 255) {
    digitalWrite(csPin_, LOW);
  }
  uint16_t x1 = transfer12(spi_, csPin_, 0xD0);
  uint16_t y1 = transfer12(spi_, csPin_, 0x90);
  uint16_t z1 = transfer12(spi_, csPin_, 0xB0);
  uint16_t z2 = transfer12(spi_, csPin_, 0xC0);

  x = x1;
  y = y1;
  z = (z1 > z2) ? (z1 - z2) : (z2 - z1);
  return true;
}
