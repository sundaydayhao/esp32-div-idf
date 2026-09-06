#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Raw, pressure-filtered panel samples; calibration and screen layout belong to
 * the UI. */
esp_err_t touch_input_init(void);
bool touch_input_read_raw(uint16_t pressure, int16_t* x, int16_t* y);
