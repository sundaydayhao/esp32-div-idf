#include "pcf8574.h"

#include "board_pins.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static i2c_master_bus_handle_t bus;
static SemaphoreHandle_t mutex;

esp_err_t pcf8574_init(pcf8574_t* device) {
  if (!device) return ESP_ERR_INVALID_ARG;
  if (device->device) return ESP_OK;
#if !HAS_PCF8574_BUTTONS
  return ESP_ERR_NOT_SUPPORTED;
#else
  if (!bus) {
    i2c_master_bus_config_t config = {.i2c_port = I2C_NUM_0,
                                      .sda_io_num = PCF8574_I2C_SDA,
                                      .scl_io_num = PCF8574_I2C_SCL,
                                      .clk_source = I2C_CLK_SRC_DEFAULT,
                                      .glitch_ignore_cnt = 7,
                                      .flags.enable_internal_pullup = true};
    esp_err_t err = i2c_new_master_bus(&config, &bus);
    if (err != ESP_OK) return err;
    mutex = xSemaphoreCreateMutex();
    if (!mutex) return ESP_ERR_NO_MEM;
  }
  uint8_t address = 0;
  uint8_t first = device->address ? device->address : PCF8574_ADDR_MIN;
  uint8_t last = device->address ? device->address : PCF8574_ADDR_MAX;
  for (uint8_t i = first; i <= last; ++i)
    if (i2c_master_probe(bus, i, 50) == ESP_OK) {
      address = i;
      break;
    }
  if (!address) return ESP_ERR_NOT_FOUND;
  i2c_device_config_t config = {.dev_addr_length = I2C_ADDR_BIT_LEN_7,
                                .device_address = address,
                                .scl_speed_hz = PCF8574_I2C_FREQ_HZ};
  esp_err_t err = i2c_master_bus_add_device(bus, &config, &device->device);
  if (err != ESP_OK) return err;
  device->address = address;
  device->output = 0xff;
  return i2c_master_transmit(device->device, &device->output, 1, 50);
#endif
}
bool pcf8574_read(pcf8574_t* device, uint8_t* value) {
  if (!device || !device->device || !value || !mutex) return false;
  if (xSemaphoreTake(mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
  esp_err_t err = i2c_master_receive(device->device, value, 1, 20);
  xSemaphoreGive(mutex);
  return err == ESP_OK;
}
bool pcf8574_write_pin(pcf8574_t* device, unsigned pin, bool high) {
  if (!device || !device->device || pin > 7 || !mutex) return false;
  if (xSemaphoreTake(mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
  if (high)
    device->output |= (uint8_t)(1u << pin);
  else
    device->output &= (uint8_t)~(1u << pin);
  esp_err_t err = i2c_master_transmit(device->device, &device->output, 1, 20);
  xSemaphoreGive(mutex);
  return err == ESP_OK;
}
bool pcf8574_begin(pcf8574_t* device, uint8_t address) {
  device->address = address;
  return pcf8574_init(device) == ESP_OK;
}
bool pcf8574_read_pin(pcf8574_t* device, unsigned pin) {
  uint8_t value = 255;
  return pin > 7 || !pcf8574_read(device, &value) || (value & (1u << pin));
}
void pcf8574_configure_pin(pcf8574_t* device, unsigned pin, int mode) {
  if (mode == GPIO_MODE_INPUT) pcf8574_write_pin(device, pin, true);
}
