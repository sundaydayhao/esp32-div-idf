#include "nvs_records.h"

#include <stdio.h>

#include "esp_log.h"
#include "nvs.h"

esp_err_t nv_read(const char* domain, const char* key, void* data,
                  size_t size) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(domain, NVS_READONLY, &handle);
  if (err != ESP_OK) return err;
  size_t stored = 0;
  err = nvs_get_blob(handle, key, NULL, &stored);
  if (err == ESP_OK && stored != size) err = ESP_ERR_NVS_INVALID_LENGTH;
  if (err == ESP_OK) err = nvs_get_blob(handle, key, data, &stored);
  nvs_close(handle);
  return err;
}
esp_err_t nv_write(const char* domain, const char* key, const void* data,
                   size_t size) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(domain, NVS_READWRITE, &handle);
  if (err != ESP_OK) return err;
  err = nvs_set_blob(handle, key, data, size);
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  if (err != ESP_OK)
    ESP_LOGE("nvs_records", "%s/%s: %s", domain, key, esp_err_to_name(err));
  return err;
}
esp_err_t nv_erase(const char* domain, const char* key) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(domain, NVS_READWRITE, &handle);
  if (err != ESP_OK) return err;
  err = nvs_erase_key(handle, key);
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  return err;
}
void nv_indexed_key(const char* prefix, unsigned index, char key[16]) {
  snprintf(key, 16, "%.8s_%u", prefix, index % 100000);
}
