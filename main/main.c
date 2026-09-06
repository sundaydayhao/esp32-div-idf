#include "app.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char* TAG = "app_main";

static void init_nvs(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, NULL);
    if (partition != NULL) {
      ESP_ERROR_CHECK(esp_partition_erase_range(partition, 0, partition->size));
    }
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
}

void app_main(void) {
  ESP_LOGI(TAG, "starting ESP-IDF application");
  init_nvs();
  ESP_ERROR_CHECK(esp_netif_init());
  esp_err_t event_loop_err = esp_event_loop_create_default();
  if (event_loop_err != ESP_ERR_INVALID_STATE) {
    ESP_ERROR_CHECK(event_loop_err);
  }

  esp32_div_app_init();
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) ==
          ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
  }
  while (true) {
    esp32_div_app_run_once();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}
