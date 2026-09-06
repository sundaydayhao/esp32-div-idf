#pragma once
#include "c_util.h"
#include "driver/i2c_master.h"
typedef struct {
  i2c_master_dev_handle_t device;
  uint8_t address;
  uint8_t output;
} pcf8574_t;
esp_err_t pcf8574_init(pcf8574_t* device);
bool pcf8574_read(pcf8574_t* device, uint8_t* value);
bool pcf8574_write_pin(pcf8574_t* device, unsigned pin, bool high);
bool pcf8574_begin(pcf8574_t* device, uint8_t address);
bool pcf8574_read_pin(pcf8574_t* device, unsigned pin);
void pcf8574_configure_pin(pcf8574_t* device, unsigned pin, int mode);
