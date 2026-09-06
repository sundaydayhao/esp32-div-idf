#include "system_utils.h"

#include "soc/soc_caps.h"
#if SOC_TEMPERATURE_SENSOR_SUPPORTED
#include "driver/temperature_sensor.h"
#endif
#include "esp_err.h"
#include "esp_log.h"
#include "soc/soc_caps.h"

static const char* kTag = "system";

#if SOC_TEMPERATURE_SENSOR_SUPPORTED
temperature_sensor_handle_t g_temperatureSensor = NULL;

bool ensureTemperatureSensor() {
  if (g_temperatureSensor) {
    return true;
  }
  temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
  esp_err_t err = temperature_sensor_install(&cfg, &g_temperatureSensor);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "temperature sensor install failed: %s",
             esp_err_to_name(err));
    return false;
  }
  err = temperature_sensor_enable(g_temperatureSensor);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(kTag, "temperature sensor enable failed: %s",
             esp_err_to_name(err));
    temperature_sensor_uninstall(g_temperatureSensor);
    g_temperatureSensor = NULL;
    return false;
  }
  return true;
}
#endif

float sys_temperature_celsius(void) {
#if SOC_TEMPERATURE_SENSOR_SUPPORTED
  if (!ensureTemperatureSensor()) {
    return 0.0f;
  }
  float value = 0.0f;
  esp_err_t err = temperature_sensor_get_celsius(g_temperatureSensor, &value);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "temperature sensor read failed: %s", esp_err_to_name(err));
    return 0.0f;
  }
  return value;
#else
  return 0.0f;
#endif
}
