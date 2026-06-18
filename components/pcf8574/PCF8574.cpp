#include "PCF8574.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"

#ifndef PCF8574_I2C_PORT
#define PCF8574_I2C_PORT I2C_NUM_0
#endif

#ifndef PCF8574_I2C_SDA
#define PCF8574_I2C_SDA GPIO_NUM_8
#endif

#ifndef PCF8574_I2C_SCL
#define PCF8574_I2C_SCL GPIO_NUM_9
#endif

#ifndef PCF8574_I2C_FREQ_HZ
#define PCF8574_I2C_FREQ_HZ 400000
#endif

namespace {

struct BusState {
  i2c_master_bus_handle_t bus = nullptr;
  bool ready = false;
};

BusState& busState() {
  static BusState state;
  return state;
}

bool ensureBus() {
  auto& state = busState();
  if (state.ready) {
    return true;
  }

  const i2c_master_bus_config_t bus_config = {
      .i2c_port = PCF8574_I2C_PORT,
      .sda_io_num = PCF8574_I2C_SDA,
      .scl_io_num = PCF8574_I2C_SCL,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .intr_priority = 0,
      .trans_queue_depth = 4,
      .flags = {
          .enable_internal_pullup = 1,
          .allow_pd = 0,
      },
  };

  esp_err_t err = i2c_new_master_bus(&bus_config, &state.bus);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    return false;
  }
  if (err == ESP_ERR_INVALID_STATE) {
    esp_err_t probe = i2c_master_get_bus_handle(PCF8574_I2C_PORT, &state.bus);
    if (probe != ESP_OK || state.bus == nullptr) {
      return false;
    }
  }

  state.ready = true;
  return true;
}

}  // namespace

PCF8574::PCF8574(uint8_t address)
    : address_(address), pinModes_(0x00), pinStates_(0xFF), ready_(false), device_(nullptr) {}

bool PCF8574::begin(uint8_t address) {
  address_ = address;
  pinModes_ = 0x00;
  pinStates_ = 0xFF;
  ready_ = false;
  if (device_ != nullptr) {
    auto& state = busState();
    (void)i2c_master_bus_rm_device(device_);
    device_ = nullptr;
    if (state.bus == nullptr) {
      state.ready = false;
    }
  }
  device_ = nullptr;

  if (!ensureBus()) {
    return false;
  }

  auto& state = busState();
  if (state.bus == nullptr) {
    return false;
  }

  if (i2c_master_probe(state.bus, address_, 20) != ESP_OK) {
    return false;
  }

  const i2c_device_config_t dev_config = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = address_,
      .scl_speed_hz = PCF8574_I2C_FREQ_HZ,
      .scl_wait_us = 0,
      .flags = {
          .disable_ack_check = 0,
      },
  };

  i2c_master_dev_handle_t dev = nullptr;
  if (i2c_master_bus_add_device(state.bus, &dev_config, &dev) != ESP_OK) {
    return false;
  }

  device_ = dev;
  ready_ = true;

  uint8_t initial = 0xFF;
  if (!readPort(initial)) {
    (void)i2c_master_bus_rm_device(dev);
    device_ = nullptr;
    ready_ = false;
    return false;
  }

  pinStates_ = initial;
  return true;
}

void PCF8574::pinMode(uint8_t pin, uint8_t mode) {
  if (pin > 7) {
    return;
  }
  if (mode == 0) {
    pinModes_ |= (uint8_t)(1U << pin);
    pinStates_ |= (uint8_t)(1U << pin);
    (void)writePort(pinStates_);
  } else {
    pinModes_ &= (uint8_t)~(1U << pin);
    pinStates_ |= (uint8_t)(1U << pin);
    (void)writePort(pinStates_);
  }
}

int PCF8574::digitalRead(uint8_t pin) {
  uint8_t value = pinStates_;
  if (!readPort(value)) {
    return 1;
  }
  pinStates_ = value;
  if (pin > 7) {
    return 1;
  }
  return (value & (uint8_t)(1U << pin)) ? 1 : 0;
}

void PCF8574::digitalWrite(uint8_t pin, uint8_t level) {
  if (pin > 7) {
    return;
  }
  if (level) {
    pinStates_ |= (uint8_t)(1U << pin);
  } else {
    pinStates_ &= (uint8_t)~(1U << pin);
  }
  (void)writePort(pinStates_);
}

uint8_t PCF8574::address() const {
  return address_;
}

bool PCF8574::ensureReady() {
  return ensureBus() && device_ != nullptr && ready_;
}

bool PCF8574::readPort(uint8_t& value) {
  if (!ensureReady()) {
    return false;
  }

  uint8_t buf = 0xFF;
  if (i2c_master_receive(device_, &buf, 1, 20) != ESP_OK) {
    return false;
  }
  value = buf;
  return true;
}

bool PCF8574::writePort(uint8_t value) {
  if (!ensureReady()) {
    return false;
  }

  const uint8_t out = value;
  return i2c_master_transmit(device_, &out, 1, 20) == ESP_OK;
}
