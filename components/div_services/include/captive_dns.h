#pragma once
#include <stdint.h>

#include "esp_err.h"
typedef struct {
  int socket;
  uint32_t address;
} captive_dns_t;
#define CAPTIVE_DNS_INITIALIZER {.socket = -1}
esp_err_t captive_dns_start(captive_dns_t* server, uint32_t address);
void captive_dns_stop(captive_dns_t* server);
void captive_dns_poll(captive_dns_t* server);
