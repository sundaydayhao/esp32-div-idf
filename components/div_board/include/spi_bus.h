#pragma once
#include "driver/spi_master.h"
esp_err_t div_spi_bus_init(spi_host_device_t host,
                           const spi_bus_config_t* config, spi_dma_chan_t dma,
                           const char* owner);
