#pragma once
#include "c_util.h"
#include "spi_device.h"

#define PN532_BRTY_ISO14443A_106KBPS 0x00
#define PN532_MIFARE_ISO14443A 0x00
#define PN532_AUTH_WITH_KEYA 0x60
#define PN532_AUTH_WITH_KEYB 0x61

typedef struct {
  spi_device_t spi_;
  int8_t sck_;
  int8_t miso_;
  int8_t mosi_;
  int8_t ss_;
  uint8_t packetBuffer_[265];
  uint8_t inListedTag_;
} pn532_t;
#define PN532_INITIALIZER {.inListedTag_ = 1}

void pn532_construct(pn532_t* self, int8_t sck, int8_t miso, int8_t mosi,
                     int8_t ss);
void pn532_begin(pn532_t* self);
void pn532_sam_config(pn532_t* self);
void pn532_set_passive_activation_retries(pn532_t* self, uint8_t maxRetries);
uint32_t pn532_get_firmware_version(pn532_t* self);
bool pn532_read_passive_target_id(pn532_t* self, uint8_t cardbaudrate,
                                  uint8_t* uid, uint8_t* uidLength,
                                  uint16_t timeout);
bool pn532_in_data_exchange(pn532_t* self, const uint8_t* send,
                            uint8_t sendLength, uint8_t* response,
                            uint8_t* responseLength);
bool pn532_mifare_classic_authenticate_block(pn532_t* self, uint8_t* uid,
                                             uint8_t uidLen,
                                             uint8_t blockNumber,
                                             uint8_t keyNumber,
                                             uint8_t* keyData);
bool pn532_mifare_classic_read_data_block(pn532_t* self, uint8_t blockNumber,
                                          uint8_t* data);
bool pn532_mifare_classic_write_data_block(pn532_t* self, uint8_t blockNumber,
                                           const uint8_t* data);
bool pn532_mifare_ultralight_read_page(pn532_t* self, uint8_t page,
                                       uint8_t* buffer);
bool pn532_mifare_ultralight_write_page(pn532_t* self, uint8_t page,
                                        const uint8_t* data);
bool pn532_as_target(pn532_t* self);
bool pn532_get_data_target(pn532_t* self, uint8_t* cmd, uint8_t* cmdlen);
bool pn532_set_data_target(pn532_t* self, const uint8_t* cmd, uint8_t cmdlen);
bool pn532_send_command_check_ack(pn532_t* self, const uint8_t* cmd,
                                  uint8_t cmdlen, uint16_t timeout);
bool pn532_read_response(pn532_t* self, uint8_t* response,
                         uint8_t* responseLength, uint16_t timeout);
bool pn532_is_ready(pn532_t* self);
void pn532_write_frame(pn532_t* self, const uint8_t* data, uint8_t len);
uint8_t pn532_spi_read(pn532_t* self);
void pn532_spi_write(pn532_t* self, uint8_t value);
uint8_t pn532_spi_transfer(pn532_t* self, uint8_t value);
void pn532_cs(pn532_t* self, bool high);
bool pn532_wait_ready(pn532_t* self, uint16_t timeout);
bool pn532_command_response(pn532_t* self, uint8_t command, const uint8_t* cmd,
                            uint8_t cmdlen, uint8_t* response,
                            uint8_t* responseLength, uint16_t timeout);
