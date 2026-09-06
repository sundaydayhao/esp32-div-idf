#pragma once
#include "c_util.h"
#include "driver/spi_master.h"
typedef struct {
  spi_device_handle_t handle;
  int cs;
  bool selected;
} spi_device_t;
esp_err_t spi_device_init(spi_device_t* device, spi_host_device_t host,
                          int sclk, int miso, int mosi, int cs, int frequency,
                          bool lsb_first);
bool spi_device_select(spi_device_t* device);
void spi_device_deselect(spi_device_t* device);
uint8_t spi_device_transfer(spi_device_t* device, uint8_t value);
void spi_device_close(spi_device_t* device);
void spi_device_release_all(void);
static inline bool spi_device_ready(const spi_device_t* device) {
  return device && device->handle;
}
