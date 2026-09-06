#pragma once
#include "c_util.h"
#include "driver/gpio.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static inline uint32_t sys_monotonic_ms(void) {
  return (uint32_t)(esp_timer_get_time() / 1000);
}
static inline uint32_t sys_monotonic_us(void) {
  return (uint32_t)esp_timer_get_time();
}
static inline void sys_sleep_ms(uint32_t ms) {
  vTaskDelay(DIV_MAX((TickType_t)1, pdMS_TO_TICKS(ms)));
}
static inline void sys_yield_task(void) { vTaskDelay(1); }
static inline long sys_scale(long x, long in_min, long in_max, long out_min,
                             long out_max) {
  return in_max == in_min
             ? out_min
             : ((int64_t)x - in_min) * (out_max - out_min) / (in_max - in_min) +
                   out_min;
}
float sys_temperature_celsius(void);
static inline long sys_random_range_1(long max) {
  return max > 0 ? esp_random() % (uint32_t)max : 0;
}
static inline long sys_random_range_2(long min, long max) {
  return max > min ? min + sys_random_range_1(max - min) : min;
}
static inline esp_err_t sys_configure_gpio(int pin, gpio_mode_t mode) {
  if (!GPIO_IS_VALID_GPIO(pin)) return ESP_ERR_INVALID_ARG;
  gpio_config_t cfg = {.pin_bit_mask = 1ULL << pin, .mode = mode};
  return gpio_config(&cfg);
}
static inline esp_err_t sys_configure_gpio_pull(int pin, gpio_mode_t mode,
                                                gpio_pullup_t pull) {
  if (!GPIO_IS_VALID_GPIO(pin)) return ESP_ERR_INVALID_ARG;
  gpio_config_t cfg = {
      .pin_bit_mask = 1ULL << pin, .mode = mode, .pull_up_en = pull};
  return gpio_config(&cfg);
}
