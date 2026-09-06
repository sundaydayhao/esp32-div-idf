#include "board_services.h"

#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "led_strip.h"
#include "soc/soc_caps.h"

adc_oneshot_unit_handle_t adc = NULL;
adc_cali_handle_t calibration = NULL;
adc_channel_t channel;
led_strip_handle_t strip = NULL;
static const char* tag = "board";

esp_err_t board_init(void) {
  const int selects[] = {TFT_CS,    XPT2046_CS, SD_CS,    CC1101_CS,
                         CSN_PIN_1, CSN_PIN_2,  CSN_PIN_3};
  for (size_t i = 0; i < DIV_ARRAY_SIZE(selects); ++i) {
    int pin = selects[i];
    if (!GPIO_IS_VALID_OUTPUT_GPIO(pin)) continue;
    gpio_set_level((gpio_num_t)pin, 1);
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
  }

  ESP_LOGI(tag, "%s; display SPI%d; SD/radio SPI%d", ESP32DIV_BOARD_NAME,
           (int)DIV_TFT_HOST + 1, (int)DIV_SD_HOST + 1);
#if BATTERY_ADC_PIN >= 0 && BUZZER_PIN != BATTERY_ADC_PIN
  adc_unit_t unit;
  esp_err_t err = adc_oneshot_io_to_channel(BATTERY_ADC_PIN, &unit, &channel);
  if (err != ESP_OK) return err;
  adc_oneshot_unit_init_cfg_t cfg = {0};
  cfg.unit_id = unit;
  err = adc_oneshot_new_unit(&cfg, &adc);
  if (err != ESP_OK) return err;
  adc_oneshot_chan_cfg_t chan = {0};
  chan.atten = ADC_ATTEN_DB_12;
  chan.bitwidth = ADC_BITWIDTH_DEFAULT;
  err = adc_oneshot_config_channel(adc, channel, &chan);
  if (err != ESP_OK) return err;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
  adc_cali_curve_fitting_config_t cal = {0};
  cal.unit_id = unit;
  cal.chan = channel;
  cal.atten = chan.atten;
  cal.bitwidth = chan.bitwidth;
  err = adc_cali_create_scheme_curve_fitting(&cal, &calibration);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
  adc_cali_line_fitting_config_t cal = {0};
  cal.unit_id = unit;
  cal.atten = chan.atten;
  cal.bitwidth = chan.bitwidth;
  cal.default_vref = 1100;
  err = adc_cali_create_scheme_line_fitting(&cal, &calibration);
#endif
  if (err != ESP_OK)
    ESP_LOGW(tag, "battery calibration unavailable: %s", esp_err_to_name(err));
#endif
#if DIV_LED_PIN >= 0
  led_strip_config_t cfg_led = {0};
  cfg_led.strip_gpio_num = DIV_LED_PIN;
  cfg_led.max_leds = DIV_LED_COUNT;
  cfg_led.led_model = LED_MODEL_WS2812;
  cfg_led.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
  led_strip_rmt_config_t cfg_rmt = {0};
  cfg_rmt.clk_src = RMT_CLK_SRC_DEFAULT;
  cfg_rmt.resolution_hz = 10000000;
  cfg_rmt.mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL;
  const esp_err_t led_err =
      led_strip_new_rmt_device(&cfg_led, &cfg_rmt, &strip);
  if (led_err != ESP_OK)
    ESP_LOGW(tag, "status light unavailable: %s", esp_err_to_name(led_err));
  if (strip) led_strip_clear(strip);
#endif
  return ESP_OK;
}

float board_battery_voltage(void) {
  if (!adc || !calibration) return 0.0f;
  int sum = 0, count = 0;
  for (int i = 0; i < 8; ++i) {
    int raw = 0, mv = 0;
    if (adc_oneshot_read(adc, channel, &raw) == ESP_OK &&
        adc_cali_raw_to_voltage(calibration, raw, &mv) == ESP_OK) {
      sum += mv;
      ++count;
    }
  }
  return count ? (sum / (1000.0f * count)) *
                     (BATTERY_VDIV_R1 + BATTERY_VDIV_R2) / BATTERY_VDIV_R2
               : 0.0f;
}

void board_set_status_light(bool enabled, uint8_t red, uint8_t green,
                            uint8_t blue) {
  if (!strip) return;
  if (!enabled) {
    led_strip_clear(strip);
    return;
  }
  for (int i = 0; i < DIV_LED_COUNT; ++i)
    led_strip_set_pixel(strip, i, red, green, blue);
  led_strip_refresh(strip);
}
