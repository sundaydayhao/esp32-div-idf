#pragma once
#include "c_util.h"
#include "esp_err.h"
esp_err_t board_init(void);
float board_battery_voltage(void);
void board_set_status_light(bool enabled, uint8_t red, uint8_t green,
                            uint8_t blue);
