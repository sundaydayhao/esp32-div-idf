#include "touch_driver.h"

#include "board_pins.h"
#include "esp_lcd_panel_io_interface.h"
#include "esp_lcd_touch_xpt2046.h"
#include "esp_rom_sys.h"
#include "spi_bus.h"

esp_lcd_touch_handle_t touch = NULL;
esp_lcd_panel_io_handle_t io = NULL;

#if TOUCH_SOFTWARE_SPI
// IDF panel IO transport for the third SPI wire group on ESP32 boards.
// XPT2046 decoding, filtering and pressure measurement remain in the component.
esp_lcd_panel_io_t gpio_io = {0};
static esp_err_t gpio_io_delete(esp_lcd_panel_io_t* unused) {
  (void)unused;
  return ESP_OK;
}
uint8_t transfer(uint8_t value) {
  uint8_t result = 0;
  for (int bit = 7; bit >= 0; --bit) {
    gpio_set_level((gpio_num_t)XPT2046_MOSI, (value >> bit) & 1);
    esp_rom_delay_us(2);
    gpio_set_level((gpio_num_t)XPT2046_CLK, 1);
    esp_rom_delay_us(2);
    result = (result << 1) | gpio_get_level((gpio_num_t)XPT2046_MISO);
    gpio_set_level((gpio_num_t)XPT2046_CLK, 0);
  }
  return result;
}
esp_err_t read_param(esp_lcd_panel_io_t* unused, int command, void* data,
                     size_t size) {
  if (!data || size == 0) return ESP_ERR_INVALID_ARG;
  gpio_set_level((gpio_num_t)XPT2046_CS, 0);
  if (command >= 0) transfer(command);
  uint8_t* bytes = data;
  for (size_t i = 0; i < size; ++i) bytes[i] = transfer(0);
  gpio_set_level((gpio_num_t)XPT2046_CS, 1);
  return ESP_OK;
}
#endif

esp_err_t touch_driver_init(void) {
  if (touch) return ESP_OK;
#if TOUCH_SOFTWARE_SPI
  gpio_config_t pins = {0};
  pins.pin_bit_mask =
      (1ULL << XPT2046_CS) | (1ULL << XPT2046_CLK) | (1ULL << XPT2046_MOSI);
  pins.mode = GPIO_MODE_OUTPUT;
  esp_err_t err = gpio_config(&pins);
  if (err != ESP_OK) return err;
  gpio_set_level((gpio_num_t)XPT2046_CS, 1);
  gpio_set_level((gpio_num_t)XPT2046_CLK, 0);
  gpio_set_direction((gpio_num_t)XPT2046_MISO, GPIO_MODE_INPUT);
  gpio_io.rx_param = read_param;
  gpio_io.del = gpio_io_delete;
  io = &gpio_io;
#else
  spi_bus_config_t bus = {0};
  bus.mosi_io_num = XPT2046_MOSI;
  bus.miso_io_num = XPT2046_MISO;
  bus.sclk_io_num = XPT2046_CLK;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = 40960;
  esp_err_t err =
      div_spi_bus_init(XPT2046_HOST, &bus, SPI_DMA_CH_AUTO, "touch");
  if (err != ESP_OK) return err;
  esp_lcd_panel_io_spi_config_t cfg =
      ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(XPT2046_CS);
  cfg.pclk_hz = 1000000;
  err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)XPT2046_HOST, &cfg,
                                 &io);
  if (err != ESP_OK) return err;
#endif
  esp_lcd_touch_config_t cfg_touch = {0};
  cfg_touch.x_max = 4096;
  cfg_touch.y_max = 4096;
  cfg_touch.rst_gpio_num = GPIO_NUM_NC;
  // Polling supports both IRQ-connected and IRQ-unconnected official boards.
  cfg_touch.int_gpio_num = GPIO_NUM_NC;
  return esp_lcd_touch_new_spi_xpt2046(io, &cfg_touch, &touch);
}

bool touch_driver_read(touch_sample_t* sample) {
  if (!sample || !touch || esp_lcd_touch_read_data(touch) != ESP_OK)
    return false;
  esp_lcd_touch_point_data_t point = {0};
  uint8_t count = 0;
  if (esp_lcd_touch_get_data(touch, &point, &count, 1) != ESP_OK || !count)
    return false;
  *sample = (touch_sample_t){point.x, point.y, point.strength};
  return true;
}
bool touch_driver_pending(void) { return touch != NULL; }
