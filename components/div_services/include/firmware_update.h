#pragma once
#include "c_util.h"
#include "esp_ota_ops.h"
typedef struct {
  const esp_partition_t* partition;
  esp_ota_handle_t handle;
  bool active;
  esp_err_t error;
  size_t expected, received, prefix_size;
  uint8_t prefix[288];
} firmware_update_t;
esp_err_t firmware_begin(firmware_update_t* update, size_t size);
esp_err_t firmware_write(firmware_update_t* update, const void* data,
                         size_t size);
esp_err_t firmware_finish(firmware_update_t* update);
void firmware_abort(firmware_update_t* update);
esp_err_t firmware_from_file(const char* path,
                             void (*progress)(size_t, size_t));
