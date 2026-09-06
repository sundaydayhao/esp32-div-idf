#include "spi_device.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "spi_bus.h"
static spi_device_t* devices[16];

esp_err_t spi_device_init(spi_device_t* device, spi_host_device_t host,
                          int sclk, int miso, int mosi, int cs, int frequency,
                          bool lsb_first) {
  if (!device || !GPIO_IS_VALID_OUTPUT_GPIO(cs)) return ESP_ERR_INVALID_ARG;
  if (device->handle) return cs == device->cs ? ESP_OK : ESP_ERR_INVALID_STATE;
  size_t slot = 0;
  while (slot < DIV_ARRAY_SIZE(devices) && devices[slot]) ++slot;
  if (slot == DIV_ARRAY_SIZE(devices)) return ESP_ERR_NO_MEM;
  spi_bus_config_t bus = {.mosi_io_num = mosi,
                          .miso_io_num = miso,
                          .sclk_io_num = sclk,
                          .quadwp_io_num = -1,
                          .quadhd_io_num = -1,
                          .max_transfer_sz = 40960};
  esp_err_t err = div_spi_bus_init(host, &bus, SPI_DMA_CH_AUTO, "radio");
  if (err != ESP_OK) return err;
  gpio_set_level((gpio_num_t)cs, 1);
  gpio_set_direction((gpio_num_t)cs, GPIO_MODE_OUTPUT);
  spi_device_interface_config_t cfg = {
      .clock_speed_hz = frequency,
      .spics_io_num = -1,
      .flags = lsb_first ? SPI_DEVICE_BIT_LSBFIRST : 0,
      .queue_size = 1};
  err = spi_bus_add_device(host, &cfg, &device->handle);
  if (err == ESP_OK) {
    device->cs = cs;
    device->selected = false;
    devices[slot] = device;
  }
  return err;
}
bool spi_device_select(spi_device_t* device) {
  if (!device || !device->handle || device->selected) return false;
  if (spi_device_acquire_bus(device->handle, portMAX_DELAY) != ESP_OK)
    return false;
  device->selected = true;
  gpio_set_level((gpio_num_t)device->cs, 0);
  return true;
}
void spi_device_deselect(spi_device_t* device) {
  if (!device || !device->handle || !device->selected) return;
  gpio_set_level((gpio_num_t)device->cs, 1);
  device->selected = false;
  spi_device_release_bus(device->handle);
}
uint8_t spi_device_transfer(spi_device_t* device, uint8_t value) {
  if (!device || !device->selected) return 0xff;
  spi_transaction_t transaction = {
      .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA, .length = 8};
  transaction.tx_data[0] = value;
  return spi_device_polling_transmit(device->handle, &transaction) == ESP_OK
             ? transaction.rx_data[0]
             : 0xff;
}
void spi_device_close(spi_device_t* device) {
  if (!device || !device->handle) return;
  spi_device_deselect(device);
  spi_bus_remove_device(device->handle);
  device->handle = NULL;
  for (size_t i = 0; i < DIV_ARRAY_SIZE(devices); ++i)
    if (devices[i] == device) devices[i] = NULL;
}
void spi_device_release_all(void) {
  for (size_t i = 0; i < DIV_ARRAY_SIZE(devices); ++i)
    if (devices[i]) spi_device_close(devices[i]);
}
