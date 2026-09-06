#include "firmware_update.h"

#include "esp_app_desc.h"
#include "firmware_image.h"
#include "sdkconfig.h"
#include "storage.h"
void firmware_abort(firmware_update_t* update) {
  if (!update) return;
  if (update->active) esp_ota_abort(update->handle);
  update->active = false;
}
esp_err_t firmware_begin(firmware_update_t* update, size_t size) {
  if (!update || update->active || size < sizeof(update->prefix))
    return ESP_ERR_INVALID_SIZE;
  memset(update, 0, sizeof(*update));
  update->partition = esp_ota_get_next_update_partition(NULL);
  if (!update->partition ||
      update->partition == esp_ota_get_running_partition())
    return ESP_ERR_NOT_FOUND;
  if (size > update->partition->size) return ESP_ERR_INVALID_SIZE;
  update->expected = size;
  return ESP_OK;
}
esp_err_t firmware_write(firmware_update_t* update, const void* input,
                         size_t size) {
  if (!update) return ESP_ERR_INVALID_ARG;
  if (update->error) return update->error;
  if (!update->expected || !input || size > update->expected - update->received)
    return update->error = ESP_ERR_INVALID_SIZE;
  const uint8_t* data = input;
  update->received += size;
  if (update->prefix_size < sizeof(update->prefix)) {
    size_t take = DIV_MIN(size, sizeof(update->prefix) - update->prefix_size);
    memcpy(update->prefix + update->prefix_size, data, take);
    update->prefix_size += take;
    data += take;
    size -= take;
    if (update->prefix_size != sizeof(update->prefix)) return ESP_OK;
    if (!firmware_matches_image(update->prefix, sizeof(update->prefix),
                                CONFIG_IDF_FIRMWARE_CHIP_ID,
                                esp_app_get_description()->project_name))
      return update->error = ESP_ERR_INVALID_ARG;
    update->error =
        esp_ota_begin(update->partition, update->expected, &update->handle);
    if (update->error) return update->error;
    update->active = true;
    update->error =
        esp_ota_write(update->handle, update->prefix, sizeof(update->prefix));
    if (update->error) return update->error;
  }
  return update->error =
             size ? esp_ota_write(update->handle, data, size) : ESP_OK;
}
esp_err_t firmware_finish(firmware_update_t* update) {
  if (!update || !update->active || update->received != update->expected)
    return ESP_ERR_INVALID_SIZE;
  if (update->error) return update->error;
  esp_err_t err = esp_ota_end(update->handle);
  update->active = false;
  return err == ESP_OK ? esp_ota_set_boot_partition(update->partition) : err;
}
esp_err_t firmware_from_file(const char* path,
                             void (*progress)(size_t, size_t)) {
  FILE* file = storage_open(path, "rb");
  if (!file) return ESP_ERR_NOT_FOUND;
  if (fseek(file, 0, SEEK_END)) {
    fclose(file);
    return ESP_FAIL;
  }
  long total = ftell(file);
  rewind(file);
  firmware_update_t update = {0};
  esp_err_t err = firmware_begin(&update, total > 0 ? (size_t)total : 0);
  uint8_t buffer[1024];
  while (err == ESP_OK && update.received < update.expected) {
    size_t n =
        fread(buffer, 1,
              DIV_MIN(sizeof(buffer), update.expected - update.received), file);
    if (!n) {
      err = ESP_FAIL;
      break;
    }
    err = firmware_write(&update, buffer, n);
    if (progress) progress(update.received, update.expected);
  }
  if (err == ESP_OK) err = firmware_finish(&update);
  firmware_abort(&update);
  fclose(file);
  return err;
}
