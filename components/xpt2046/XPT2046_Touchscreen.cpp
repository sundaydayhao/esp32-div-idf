#include "XPT2046_Touchscreen.h"

#include <string.h>

#include "shared.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

namespace {

static const char* TAG = "xpt2046";
static constexpr int kSpiClockHz = 1000000;
static constexpr int kDelayUs = 1;
static constexpr uint16_t kAdcMax = 4095;

}  // namespace

XPT2046_Touchscreen::XPT2046_Touchscreen(uint8_t csPin, uint8_t irqPin)
    : csPin_(csPin),
      irqPin_(irqPin),
      rotation_(0),
      clkPin_(XPT2046_CLK),
      mosiPin_(XPT2046_MOSI),
      misoPin_(XPT2046_MISO) {}

void XPT2046_Touchscreen::begin() {
  gpio_config_t io = {};

  io.pin_bit_mask = (1ULL << csPin_) | (1ULL << clkPin_) | (1ULL << mosiPin_);
  io.mode = GPIO_MODE_OUTPUT;
  io.pull_up_en = GPIO_PULLUP_DISABLE;
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&io);

  io.pin_bit_mask = 1ULL << misoPin_;
  io.mode = GPIO_MODE_INPUT;
  io.pull_up_en = GPIO_PULLUP_DISABLE;
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&io);

  if (irqPin_ < 255) {
    io.pin_bit_mask = 1ULL << irqPin_;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io);
  }

  gpio_set_level((gpio_num_t)csPin_, 1);
  gpio_set_level((gpio_num_t)clkPin_, 0);
  gpio_set_level((gpio_num_t)mosiPin_, 0);
  ESP_LOGI(TAG, "begin cs=%u clk=%u mosi=%u miso=%u irq=%u",
           (unsigned)csPin_, (unsigned)clkPin_, (unsigned)mosiPin_,
           (unsigned)misoPin_, (unsigned)irqPin_);
}

void XPT2046_Touchscreen::setRotation(uint8_t rotation) {
  rotation_ = rotation;
}

bool XPT2046_Touchscreen::tirqTouched() const {
  if (irqPin_ >= 255) {
    return true;
  }
  return gpio_get_level((gpio_num_t)irqPin_) == 0;
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
  if (csPin_ < 255) gpio_set_level((gpio_num_t)csPin_, 0);
  esp_rom_delay_us(kDelayUs);
  uint16_t x1 = transfer12(0xD0);
  uint16_t y1 = transfer12(0x90);
  uint16_t z1 = transfer12(0xB0);
  uint16_t z2 = transfer12(0xC0);
  esp_rom_delay_us(kDelayUs);

  x = x1;
  y = y1;
  if ((z1 == 0 && z2 == 0) || (z1 == kAdcMax && z2 == kAdcMax)) {
    z = 0;
  } else {
    int32_t pressure = (int32_t)z1 + kAdcMax - (int32_t)z2;
    if (pressure < 0) pressure = 0;
    if (pressure > kAdcMax) pressure = kAdcMax;
    z = (uint16_t)pressure;
  }

  if (csPin_ < 255) gpio_set_level((gpio_num_t)csPin_, 1);
  return true;
}

uint16_t XPT2046_Touchscreen::transfer12(uint8_t cmd) {
  uint16_t value = 0;

  for (int8_t bit = 7; bit >= 0; --bit) {
    gpio_set_level((gpio_num_t)mosiPin_, (cmd >> bit) & 0x1);
    gpio_set_level((gpio_num_t)clkPin_, 1);
    esp_rom_delay_us(kDelayUs);
    gpio_set_level((gpio_num_t)clkPin_, 0);
    esp_rom_delay_us(kDelayUs);
  }

  gpio_set_level((gpio_num_t)clkPin_, 1);
  esp_rom_delay_us(kDelayUs);
  gpio_set_level((gpio_num_t)clkPin_, 0);
  esp_rom_delay_us(kDelayUs);

  for (int i = 0; i < 16; ++i) {
    gpio_set_level((gpio_num_t)clkPin_, 1);
    esp_rom_delay_us(kDelayUs);
    value <<= 1;
    value |= (uint16_t)gpio_get_level((gpio_num_t)misoPin_) & 0x1;
    gpio_set_level((gpio_num_t)clkPin_, 0);
    esp_rom_delay_us(kDelayUs);
  }

  return (uint16_t)((value >> 4) & 0x0FFF);
}
