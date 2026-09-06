#include "pn532.h"

#include "board_pins.h"
#include "system_utils.h"

static const uint8_t PN532_PREAMBLE = 0x00;
static const uint8_t PN532_STARTCODE1 = 0x00;
static const uint8_t PN532_STARTCODE2 = 0xFF;
static const uint8_t PN532_POSTAMBLE = 0x00;
static const uint8_t PN532_HOSTTOPN532 = 0xD4;
static const uint8_t PN532_PN532TOHOST = 0xD5;

static const uint8_t PN532_SPI_DATAWRITE = 0x01;
static const uint8_t PN532_SPI_STATUSREAD = 0x02;
static const uint8_t PN532_SPI_DATAREAD = 0x03;
static const uint8_t PN532_SPI_READY = 0x01;

static const uint8_t PN532_COMMAND_GETFIRMWAREVERSION = 0x02;
static const uint8_t PN532_COMMAND_SAMCONFIGURATION = 0x14;
static const uint8_t PN532_COMMAND_RFCONFIGURATION = 0x32;
static const uint8_t PN532_COMMAND_INLISTPASSIVETARGET = 0x4A;
static const uint8_t PN532_COMMAND_INDATAEXCHANGE = 0x40;
static const uint8_t PN532_COMMAND_TGINITASTARGET = 0x8C;
static const uint8_t PN532_COMMAND_TGGETDATA = 0x86;
static const uint8_t PN532_COMMAND_TGSETDATA = 0x8E;

static const uint8_t PN532_MIFARE_READ = 0x30;
static const uint8_t PN532_MIFARE_WRITE = 0xA0;
static const uint8_t PN532_MIFARE_UL_WRITE = 0xA2;

static const uint8_t ackFrame[] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};

void pn532_construct(pn532_t* self, int8_t sck, int8_t miso, int8_t mosi,
                     int8_t ss) {
  *self = (pn532_t)PN532_INITIALIZER;
  self->sck_ = sck;
  self->miso_ = miso;
  self->mosi_ = mosi;
  self->ss_ = ss;
}

void pn532_begin(pn532_t* self) {
  // PN532 shifts least significant bit first (unlike the radio devices).
  spi_device_init(&self->spi_, DIV_SD_HOST, self->sck_, self->miso_,
                  self->mosi_, self->ss_, 1000000, true);
}

void pn532_cs(pn532_t* self, bool high) {
  if (high)
    spi_device_deselect(&self->spi_);
  else
    spi_device_select(&self->spi_);
}

uint8_t pn532_spi_transfer(pn532_t* self, uint8_t value) {
  return spi_device_transfer(&self->spi_, value);
}

void pn532_spi_write(pn532_t* self, uint8_t value) {
  pn532_spi_transfer(self, value);
}

uint8_t pn532_spi_read(pn532_t* self) { return pn532_spi_transfer(self, 0x00); }

bool pn532_is_ready(pn532_t* self) {
  pn532_cs(self, false);
  pn532_spi_write(self, PN532_SPI_STATUSREAD);
  const bool ready = pn532_spi_read(self) == PN532_SPI_READY;
  pn532_cs(self, true);
  return ready;
}

bool pn532_wait_ready(pn532_t* self, uint16_t timeout) {
  const uint32_t start = sys_monotonic_ms();
  while (!pn532_is_ready(self)) {
    if (timeout && (sys_monotonic_ms() - start) > timeout) return false;
    sys_sleep_ms(5);
  }
  return true;
}

void pn532_write_frame(pn532_t* self, const uint8_t* data, uint8_t len) {
  uint8_t checksum = PN532_HOSTTOPN532;
  pn532_cs(self, false);
  pn532_spi_write(self, PN532_SPI_DATAWRITE);
  pn532_spi_write(self, PN532_PREAMBLE);
  pn532_spi_write(self, PN532_STARTCODE1);
  pn532_spi_write(self, PN532_STARTCODE2);
  pn532_spi_write(self, len + 1);
  pn532_spi_write(self, ((uint8_t)(~(len + 1) + 1)));
  pn532_spi_write(self, PN532_HOSTTOPN532);
  for (uint8_t i = 0; i < len; ++i) {
    pn532_spi_write(self, data[i]);
    checksum += data[i];
  }
  pn532_spi_write(self, ((uint8_t)(~checksum + 1)));
  pn532_spi_write(self, PN532_POSTAMBLE);
  pn532_cs(self, true);
}

bool pn532_send_command_check_ack(pn532_t* self, const uint8_t* cmd,
                                  uint8_t cmdlen, uint16_t timeout) {
  pn532_write_frame(self, cmd, cmdlen);
  if (!pn532_wait_ready(self, timeout)) return false;
  uint8_t ack[sizeof(ackFrame)] = {0};
  pn532_cs(self, false);
  pn532_spi_write(self, PN532_SPI_DATAREAD);
  for (size_t index = 0; index < DIV_ARRAY_SIZE(ack); ++index) {
    uint8_t* b = &ack[index];
    (*b) = pn532_spi_read(self);
  }
  pn532_cs(self, true);
  return memcmp(ack, ackFrame, sizeof(ackFrame)) == 0;
}

bool pn532_read_response(pn532_t* self, uint8_t* response,
                         uint8_t* responseLength, uint16_t timeout) {
  if (!pn532_wait_ready(self, timeout)) return false;
  pn532_cs(self, false);
  pn532_spi_write(self, PN532_SPI_DATAREAD);
  if (pn532_spi_read(self) != PN532_PREAMBLE ||
      pn532_spi_read(self) != PN532_STARTCODE1 ||
      pn532_spi_read(self) != PN532_STARTCODE2) {
    pn532_cs(self, true);
    return false;
  }
  uint8_t len = pn532_spi_read(self);
  uint8_t lcs = pn532_spi_read(self);
  if (((uint8_t)(len + lcs)) != 0) {
    pn532_cs(self, true);
    return false;
  }
  if (len < 2 || pn532_spi_read(self) != PN532_PN532TOHOST) {
    pn532_cs(self, true);
    return false;
  }
  uint8_t checksum = PN532_PN532TOHOST;
  len--;
  const uint8_t copyLen = DIV_MIN(len, responseLength ? *responseLength : len);
  for (uint8_t i = 0; i < len; ++i) {
    const uint8_t b = pn532_spi_read(self);
    checksum += b;
    if (i < copyLen && response) response[i] = b;
  }
  const uint8_t dcs = pn532_spi_read(self);
  (void)pn532_spi_read(self);
  pn532_cs(self, true);
  if (((uint8_t)(checksum + dcs)) != 0) return false;
  if (responseLength) *responseLength = copyLen;
  return true;
}

bool pn532_command_response(pn532_t* self, uint8_t command, const uint8_t* cmd,
                            uint8_t cmdlen, uint8_t* response,
                            uint8_t* responseLength, uint16_t timeout) {
  self->packetBuffer_[0] = command;
  if (cmdlen && cmd) memcpy(self->packetBuffer_ + 1, cmd, cmdlen);
  if (!pn532_send_command_check_ack(self, self->packetBuffer_, cmdlen + 1,
                                    timeout))
    return false;
  uint8_t maxLen = responseLength ? *responseLength : 0;
  if (!pn532_read_response(self, self->packetBuffer_, &maxLen, timeout))
    return false;
  if (maxLen < 1 || self->packetBuffer_[0] != ((uint8_t)(command + 1)))
    return false;
  const uint8_t payloadLen = maxLen - 1;
  const uint8_t copyLen =
      responseLength ? DIV_MIN(*responseLength, payloadLen) : 0;
  if (response && copyLen) memcpy(response, self->packetBuffer_ + 1, copyLen);
  if (responseLength) *responseLength = copyLen;
  return true;
}

uint32_t pn532_get_firmware_version(pn532_t* self) {
  uint8_t response[4] = {0};
  uint8_t len = sizeof(response);
  if (!pn532_command_response(self, PN532_COMMAND_GETFIRMWAREVERSION, NULL, 0,
                              response, &len, 1000) ||
      len < 4) {
    return 0;
  }
  return (((uint32_t)(response[0])) << 24) | (((uint32_t)(response[1])) << 16) |
         (((uint32_t)(response[2])) << 8) | response[3];
}

void pn532_sam_config(pn532_t* self) {
  const uint8_t cmd[] = {0x01, 0x14, 0x01};
  uint8_t response[2] = {0};
  uint8_t len = sizeof(response);
  pn532_command_response(self, PN532_COMMAND_SAMCONFIGURATION, cmd, sizeof(cmd),
                         response, &len, 1000);
}

void pn532_set_passive_activation_retries(pn532_t* self, uint8_t maxRetries) {
  const uint8_t cmd[] = {0x05, 0xFF, 0x01, maxRetries};
  uint8_t response[2] = {0};
  uint8_t len = sizeof(response);
  pn532_command_response(self, PN532_COMMAND_RFCONFIGURATION, cmd, sizeof(cmd),
                         response, &len, 1000);
}

bool pn532_read_passive_target_id(pn532_t* self, uint8_t cardbaudrate,
                                  uint8_t* uid, uint8_t* uidLength,
                                  uint16_t timeout) {
  const uint8_t cmd[] = {0x01, cardbaudrate};
  uint8_t response[24] = {0};
  uint8_t len = sizeof(response);
  if (!pn532_command_response(self, PN532_COMMAND_INLISTPASSIVETARGET, cmd,
                              sizeof(cmd), response, &len,
                              timeout ? timeout : 1000)) {
    return false;
  }
  if (len < 6 || response[0] < 1) return false;
  self->inListedTag_ = response[1];
  const uint8_t nfcidLen = response[5];
  if ((size_t)len < 6u + nfcidLen) return false;
  if (uid && uidLength) {
    *uidLength = DIV_MIN(nfcidLen, *uidLength ? *uidLength : nfcidLen);
    memcpy(uid, response + 6, *uidLength);
  }
  return true;
}

bool pn532_in_data_exchange(pn532_t* self, const uint8_t* send,
                            uint8_t sendLength, uint8_t* response,
                            uint8_t* responseLength) {
  if (sendLength == UINT8_MAX) return false;
  self->packetBuffer_[0] = self->inListedTag_;
  memcpy(self->packetBuffer_ + 1, send, sendLength);
  uint8_t len = responseLength ? *responseLength : 0;
  if (!pn532_command_response(self, PN532_COMMAND_INDATAEXCHANGE,
                              self->packetBuffer_, sendLength + 1,
                              self->packetBuffer_, &len, 1000)) {
    return false;
  }
  if (len < 1 || self->packetBuffer_[0] != 0x00) return false;
  const uint8_t payloadLen = len - 1;
  const uint8_t copyLen =
      responseLength ? DIV_MIN(*responseLength, payloadLen) : 0;
  if (response && copyLen) memcpy(response, self->packetBuffer_ + 1, copyLen);
  if (responseLength) *responseLength = copyLen;
  return true;
}

bool pn532_mifare_classic_authenticate_block(pn532_t* self, uint8_t* uid,
                                             uint8_t uidLen,
                                             uint8_t blockNumber,
                                             uint8_t keyNumber,
                                             uint8_t* keyData) {
  uint8_t cmd[13] = {0};
  cmd[0] = keyNumber ? PN532_AUTH_WITH_KEYB : PN532_AUTH_WITH_KEYA;
  cmd[1] = blockNumber;
  memcpy(cmd + 2, keyData, 6);
  memcpy(cmd + 8, uid, DIV_MIN(uidLen, 4));
  uint8_t response[2] = {0};
  uint8_t len = sizeof(response);
  return pn532_in_data_exchange(self, cmd, 12, response, &len);
}

bool pn532_mifare_classic_read_data_block(pn532_t* self, uint8_t blockNumber,
                                          uint8_t* data) {
  const uint8_t cmd[] = {PN532_MIFARE_READ, blockNumber};
  uint8_t len = 16;
  return pn532_in_data_exchange(self, cmd, sizeof(cmd), data, &len) &&
         len == 16;
}

bool pn532_mifare_classic_write_data_block(pn532_t* self, uint8_t blockNumber,
                                           const uint8_t* data) {
  uint8_t cmd[18] = {0};
  cmd[0] = PN532_MIFARE_WRITE;
  cmd[1] = blockNumber;
  memcpy(cmd + 2, data, 16);
  uint8_t response[2] = {0};
  uint8_t len = sizeof(response);
  return pn532_in_data_exchange(self, cmd, sizeof(cmd), response, &len);
}

bool pn532_mifare_ultralight_read_page(pn532_t* self, uint8_t page,
                                       uint8_t* buffer) {
  const uint8_t cmd[] = {PN532_MIFARE_READ, page};
  uint8_t len = 16;
  return pn532_in_data_exchange(self, cmd, sizeof(cmd), buffer, &len) &&
         len >= 4;
}

bool pn532_mifare_ultralight_write_page(pn532_t* self, uint8_t page,
                                        const uint8_t* data) {
  uint8_t cmd[6] = {0};
  cmd[0] = PN532_MIFARE_UL_WRITE;
  cmd[1] = page;
  memcpy(cmd + 2, data, 4);
  uint8_t response[2] = {0};
  uint8_t len = sizeof(response);
  return pn532_in_data_exchange(self, cmd, sizeof(cmd), response, &len);
}

bool pn532_as_target(pn532_t* self) {
  const uint8_t cmd[] = {0x00, 0x04, 0x00, 0x12, 0x34, 0x56, 0x20, 0x01,
                         0xFE, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xC0,
                         0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xFF,
                         0xFF, 0xAA, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44,
                         0x33, 0x22, 0x11, 0x00, 0x00};
  uint8_t response[4] = {0};
  uint8_t len = sizeof(response);
  return pn532_command_response(self, PN532_COMMAND_TGINITASTARGET, cmd,
                                sizeof(cmd), response, &len, 1000) &&
         len >= 1;
}

bool pn532_get_data_target(pn532_t* self, uint8_t* cmd, uint8_t* cmdlen) {
  uint8_t len = cmdlen ? *cmdlen : 0;
  if (!pn532_command_response(self, PN532_COMMAND_TGGETDATA, NULL, 0,
                              self->packetBuffer_, &len, 1000))
    return false;
  if (len < 1 || self->packetBuffer_[0] != 0x00) return false;
  const uint8_t payloadLen = len - 1;
  const uint8_t copyLen = cmdlen ? DIV_MIN(*cmdlen, payloadLen) : 0;
  if (cmd && copyLen) memcpy(cmd, self->packetBuffer_ + 1, copyLen);
  if (cmdlen) *cmdlen = copyLen;
  return true;
}

bool pn532_set_data_target(pn532_t* self, const uint8_t* cmd, uint8_t cmdlen) {
  uint8_t response[2] = {0};
  uint8_t len = sizeof(response);
  return pn532_command_response(self, PN532_COMMAND_TGSETDATA, cmd, cmdlen,
                                response, &len, 1000) &&
         len >= 1 && response[0] == 0x00;
}
