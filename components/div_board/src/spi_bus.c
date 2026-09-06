#include "spi_bus.h"

#include "c_util.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
  bool initialized;
  spi_bus_config_t config;
  spi_dma_chan_t dma;
  const char* owner;
} bus_state_t;
static bus_state_t buses[2];
static StaticSemaphore_t lock_storage;
static SemaphoreHandle_t lock;
static portMUX_TYPE guard = portMUX_INITIALIZER_UNLOCKED;

esp_err_t div_spi_bus_init(spi_host_device_t host,
                           const spi_bus_config_t* config, spi_dma_chan_t dma,
                           const char* owner) {
  if (!config || (host != SPI2_HOST && host != SPI3_HOST))
    return ESP_ERR_INVALID_ARG;
  portENTER_CRITICAL(&guard);
  if (!lock) lock = xSemaphoreCreateMutexStatic(&lock_storage);
  portEXIT_CRITICAL(&guard);
  if (!lock || xSemaphoreTake(lock, portMAX_DELAY) != pdTRUE)
    return ESP_ERR_NO_MEM;
  bus_state_t* state = &buses[host == SPI3_HOST];
  esp_err_t err = ESP_OK;
  if (state->initialized) {
    const spi_bus_config_t* old = &state->config;
    if (config->mosi_io_num != old->mosi_io_num ||
        config->miso_io_num != old->miso_io_num ||
        config->sclk_io_num != old->sclk_io_num ||
        config->quadwp_io_num != old->quadwp_io_num ||
        config->quadhd_io_num != old->quadhd_io_num || dma != state->dma ||
        config->max_transfer_sz > old->max_transfer_sz) {
      ESP_LOGE("spi_bus", "%s conflicts with %s on SPI%d", owner, state->owner,
               (int)host);
      err = ESP_ERR_INVALID_STATE;
    }
  } else {
    err = spi_bus_initialize(host, config, dma);
    if (err == ESP_OK) {
      state->initialized = true;
      state->config = *config;
      state->dma = dma;
      state->owner = owner;
    }
  }
  xSemaphoreGive(lock);
  return err;
}
