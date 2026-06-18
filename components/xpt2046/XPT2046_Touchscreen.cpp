#include "XPT2046_Touchscreen.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"

namespace {

static const char* TAG = "xpt2046";
static constexpr int kSpiClockHz = 2000000;
static constexpr uint32_t kSpiFlags = SPI_DEVICE_HALFDUPLEX;
static constexpr uint8_t kDummyTx = 0x00;

}  // namespace

XPT2046_Touchscreen::XPT2046_Touchscreen(uint8_t csPin, uint8_t irqPin)
    : csPin_(csPin), irqPin_(irqPin), rotation_(0), host_(SPI2_HOST), device_(nullptr) {}

void XPT2046_Touchscreen::begin() {
  if (csPin_ < 255) {
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << csPin_;
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io);
    gpio_set_level((gpio_num_t)csPin_, 1);
  }
  if (irqPin_ < 255) {
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << irqPin_;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io);
  }

  spi_bus_config_t buscfg = {};
  buscfg.sclk_io_num = -1;
  buscfg.mosi_io_num = -1;
  buscfg.miso_io_num = -1;
  buscfg.quadwp_io_num = -1;
  buscfg.quadhd_io_num = -1;
  buscfg.max_transfer_sz = 4;

  esp_err_t err = spi_bus_initialize(host_, &buscfg, SPI_DMA_CH_AUTO);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
    return;
  }

  spi_device_interface_config_t devcfg = {};
  devcfg.clock_speed_hz = kSpiClockHz;
  devcfg.mode = 0;
  devcfg.spics_io_num = -1;
  devcfg.queue_size = 1;
  devcfg.flags = kSpiFlags;

  if (device_ == nullptr) {
    err = spi_bus_add_device(host_, &devcfg, &device_);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
      device_ = nullptr;
    }
  }
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
  if (device_ == nullptr) {
    x = y = z = 0;
    return false;
  }

  if (csPin_ < 255) {
    gpio_set_level((gpio_num_t)csPin_, 0);
  }
  uint16_t x1 = transfer12(0xD0);
  uint16_t y1 = transfer12(0x90);
  uint16_t z1 = transfer12(0xB0);
  uint16_t z2 = transfer12(0xC0);

  x = x1;
  y = y1;
  z = (z1 > z2) ? (z1 - z2) : (z2 - z1);

  if (csPin_ < 255) {
    gpio_set_level((gpio_num_t)csPin_, 1);
  }
  return true;
}

uint16_t XPT2046_Touchscreen::transfer12(uint8_t cmd) {
  if (device_ == nullptr) {
    return 0;
  }

  spi_transaction_t t = {};
  uint8_t rx[2] = {0, 0};
  t.length = 8;
  t.rxlength = 16;
  t.tx_buffer = &cmd;
  t.rx_buffer = rx;
  esp_err_t err = spi_device_transmit(device_, &t);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "spi_device_transmit failed: %s", esp_err_to_name(err));
    return 0;
  }

  uint16_t val = (uint16_t)((rx[0] << 8) | rx[1]);
  return (uint16_t)((val >> 3) & 0x0FFF);
}
