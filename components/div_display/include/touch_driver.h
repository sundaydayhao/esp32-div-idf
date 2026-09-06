#pragma once
#include "c_util.h"
#include "esp_err.h"
typedef struct {
  int16_t x, y, z;
} touch_sample_t;
esp_err_t touch_driver_init(void);
bool touch_driver_read(touch_sample_t* sample);
bool touch_driver_pending(void);
static inline touch_sample_t touch_driver_sample(void) {
  touch_sample_t sample = {0};
  touch_driver_read(&sample);
  return sample;
}
