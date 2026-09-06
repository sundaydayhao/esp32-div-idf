#include "rf24.h"

#include "board_pins.h"
#include "system_utils.h"

#ifndef NRF24_SCK
#define NRF24_SCK SD_SCLK
#endif

#ifndef NRF24_MISO
#define NRF24_MISO SD_MISO
#endif

#ifndef NRF24_MOSI
#define NRF24_MOSI SD_MOSI
#endif

void rf24_construct(rf24_t* self, uint8_t cePin, uint8_t csnPin,
                    uint32_t spiSpeed) {
  *self = (rf24_t)RF24_INITIALIZER;
  self->cePin_ = cePin;
  self->csnPin_ = csnPin;
  self->spiSpeed_ = spiSpeed;
}

uint8_t rf24_transfer(rf24_t* self, uint8_t value) {
  return spi_device_transfer(&self->spi_, value);
}

void rf24_csn(rf24_t* self, bool high) {
  if (high)
    spi_device_deselect(&self->spi_);
  else
    spi_device_select(&self->spi_);
}

void rf24_ce(rf24_t* self, bool high) {
  gpio_set_level((gpio_num_t)self->cePin_, high);
}

bool rf24_begin(rf24_t* self) {
  if (self->csnPin_ == SD_CS || self->csnPin_ == XPT2046_CLK) return false;
  if (spi_device_init(&self->spi_, DIV_SD_HOST, NRF24_SCK, NRF24_MISO,
                      NRF24_MOSI, self->csnPin_,
                      DIV_MIN(self->spiSpeed_, 10000000), false) != ESP_OK)
    return false;
  gpio_set_direction((gpio_num_t)self->cePin_, GPIO_MODE_OUTPUT);
  rf24_ce(self, false);
  sys_sleep_ms(5);

  rf24_write_register_2(self, RF24_CONFIG, 0x0C);
  sys_sleep_ms(5);
  rf24_set_retries(self, 5, 15);
  rf24_set_data_rate(self, RF24_1MBPS);
  rf24_set_pa_level(self, RF24_PA_MAX, true);
  rf24_set_crc_length(self, RF24_CRC_16);
  rf24_set_channel(self, 76);
  rf24_set_payload_size(self, 32);
  rf24_write_register_2(self, RF24_EN_AA, 0x3F);
  rf24_write_register_2(self, RF24_EN_RXADDR, 0x03);
  rf24_set_address_width(self, 5);
  rf24_flush_rx(self);
  rf24_flush_tx(self);
  rf24_write_register_2(self, RF24_STATUS, 0x70);
  self->pVariant_ = true;
  return rf24_is_chip_connected(self);
}

bool rf24_is_chip_connected(rf24_t* self) {
  const uint8_t aw = rf24_read_register_1(self, RF24_SETUP_AW);
  return aw >= 1 && aw <= 3;
}

uint8_t rf24_read_register_1(rf24_t* self, uint8_t reg) {
  rf24_csn(self, false);
  rf24_transfer(self, RF24_R_REGISTER | (RF24_REGISTER_MASK & reg));
  const uint8_t result = rf24_transfer(self, 0xFF);
  rf24_csn(self, true);
  return result;
}

void rf24_read_register_3(rf24_t* self, uint8_t reg, uint8_t* buf,
                          uint8_t len) {
  rf24_csn(self, false);
  rf24_transfer(self, RF24_R_REGISTER | (RF24_REGISTER_MASK & reg));
  for (uint8_t i = 0; i < len; ++i) buf[i] = rf24_transfer(self, 0xFF);
  rf24_csn(self, true);
}

uint8_t rf24_write_register_2(rf24_t* self, uint8_t reg, uint8_t value) {
  rf24_csn(self, false);
  const uint8_t status =
      rf24_transfer(self, RF24_W_REGISTER | (RF24_REGISTER_MASK & reg));
  rf24_transfer(self, value);
  rf24_csn(self, true);
  return status;
}

uint8_t rf24_write_register_3(rf24_t* self, uint8_t reg, const uint8_t* buf,
                              uint8_t len) {
  rf24_csn(self, false);
  const uint8_t status =
      rf24_transfer(self, RF24_W_REGISTER | (RF24_REGISTER_MASK & reg));
  for (uint8_t i = 0; i < len; ++i) rf24_transfer(self, buf[i]);
  rf24_csn(self, true);
  return status;
}

uint8_t rf24_get_status(rf24_t* self) {
  rf24_csn(self, false);
  const uint8_t status = rf24_transfer(self, RF24_NOP);
  rf24_csn(self, true);
  return status;
}

void rf24_power_up(rf24_t* self) {
  uint8_t cfg = rf24_read_register_1(self, RF24_CONFIG);
  if (!(cfg & 0x02)) {
    rf24_write_register_2(self, RF24_CONFIG, cfg | 0x02);
    sys_sleep_ms(5);
  }
}

void rf24_power_down(rf24_t* self) {
  if (!spi_device_ready(&self->spi_)) return;
  rf24_ce(self, false);
  rf24_write_register_2(self, RF24_CONFIG,
                        rf24_read_register_1(self, RF24_CONFIG) & ~0x02);
}

void rf24_start_listening(rf24_t* self) {
  rf24_power_up(self);
  rf24_write_register_2(self, RF24_CONFIG,
                        rf24_read_register_1(self, RF24_CONFIG) | 0x01);
  rf24_write_register_2(self, RF24_STATUS, 0x70);
  rf24_ce(self, true);
  esp_rom_delay_us(150);
}

void rf24_stop_listening(rf24_t* self) {
  rf24_ce(self, false);
  esp_rom_delay_us(150);
  if (rf24_read_register_1(self, RF24_FEATURE) & 0x02) esp_rom_delay_us(150);
  rf24_write_register_2(self, RF24_CONFIG,
                        rf24_read_register_1(self, RF24_CONFIG) & ~0x01);
}

bool rf24_available_0(rf24_t* self) {
  uint8_t pipe = 0;
  return rf24_available_1(self, &pipe);
}

bool rf24_available_1(rf24_t* self, uint8_t* pipeNum) {
  const uint8_t status = rf24_get_status(self);
  const bool result = !(rf24_read_register_1(self, RF24_FIFO_STATUS) & 0x01);
  if (result && pipeNum) *pipeNum = (status >> 1) & 0x07;
  if (status & 0x40) rf24_write_register_2(self, RF24_STATUS, 0x40);
  return result;
}

uint8_t rf24_read_payload(rf24_t* self, void* buf, uint8_t len) {
  uint8_t* current = ((uint8_t*)(buf));
  const uint8_t dataLen = DIV_MIN(len, self->payloadSize_);
  rf24_csn(self, false);
  const uint8_t status = rf24_transfer(self, RF24_R_RX_PAYLOAD);
  for (uint8_t i = 0; i < dataLen; ++i) current[i] = rf24_transfer(self, 0xFF);
  for (uint8_t i = dataLen; i < self->payloadSize_; ++i)
    rf24_transfer(self, 0xFF);
  rf24_csn(self, true);
  return status;
}

void rf24_read(rf24_t* self, void* buf, uint8_t len) {
  rf24_read_payload(self, buf,
                    self->dynamicPayloads_
                        ? DIV_MIN(len, rf24_get_dynamic_payload_size(self))
                        : len);
  rf24_write_register_2(self, RF24_STATUS, 0x40);
}

uint8_t rf24_write_payload(rf24_t* self, const void* buf, uint8_t len,
                           uint8_t command) {
  const uint8_t* current = ((const uint8_t*)(buf));
  const uint8_t dataLen = DIV_MIN(len, self->payloadSize_);
  rf24_csn(self, false);
  const uint8_t status = rf24_transfer(self, command);
  for (uint8_t i = 0; i < dataLen; ++i) rf24_transfer(self, current[i]);
  for (uint8_t i = dataLen; i < self->payloadSize_; ++i) rf24_transfer(self, 0);
  rf24_csn(self, true);
  return status;
}

bool rf24_write(rf24_t* self, const void* buf, uint8_t len) {
  rf24_start_write(self, buf, len, false);
  return rf24_tx_stand_by_0(self);
}

bool rf24_write_fast(rf24_t* self, const void* buf, uint8_t len) {
  rf24_write_payload(self, buf, len, RF24_W_TX_PAYLOAD);
  rf24_ce(self, true);
  return !(rf24_get_status(self) & 0x20);
}

bool rf24_write_blocking(rf24_t* self, const void* buf, uint8_t len,
                         uint32_t timeout) {
  const uint32_t start = sys_monotonic_ms();
  while ((rf24_read_register_1(self, RF24_FIFO_STATUS) & 0x20) &&
         sys_monotonic_ms() - start < timeout) {
    if (rf24_get_status(self) & 0x20) {
      rf24_write_register_2(self, RF24_STATUS, 0x20);
      rf24_flush_tx(self);
    }
    sys_sleep_ms(1);
  }
  if (sys_monotonic_ms() - start >= timeout) return false;
  return rf24_write_fast(self, buf, len);
}

void rf24_start_write(rf24_t* self, const void* buf, uint8_t len,
                      bool multicast) {
  rf24_write_register_2(
      self, RF24_CONFIG,
      (rf24_read_register_1(self, RF24_CONFIG) | 0x02) & ~0x01);
  rf24_write_payload(self, buf, len, multicast ? 0xB0 : RF24_W_TX_PAYLOAD);
  rf24_ce(self, true);
  esp_rom_delay_us(15);
  rf24_ce(self, false);
}

bool rf24_tx_stand_by_0(rf24_t* self) {
  const uint32_t start = sys_monotonic_ms();
  while (!(rf24_get_status(self) & (0x20 | 0x10))) {
    if (sys_monotonic_ms() - start > 95) {
      self->failureDetected_ = true;
      return false;
    }
    sys_sleep_ms(1);
  }
  const uint8_t status = rf24_get_status(self);
  rf24_write_register_2(self, RF24_STATUS, 0x30);
  if (status & 0x10) {
    rf24_flush_tx(self);
    return false;
  }
  return true;
}

bool rf24_tx_stand_by_2(rf24_t* self, uint32_t timeout, bool startTx) {
  if (startTx) rf24_ce(self, true);
  const uint32_t start = sys_monotonic_ms();
  while (!(rf24_read_register_1(self, RF24_FIFO_STATUS) & 0x10)) {
    if (rf24_get_status(self) & 0x10) {
      rf24_write_register_2(self, RF24_STATUS, 0x10);
      rf24_ce(self, false);
      rf24_flush_tx(self);
      return false;
    }
    if (sys_monotonic_ms() - start >= timeout) {
      rf24_ce(self, false);
      return false;
    }
    sys_sleep_ms(1);
  }
  rf24_ce(self, false);
  return true;
}

void rf24_what_happened(rf24_t* self, bool* txOk, bool* txFail, bool* rxReady) {
  const uint8_t status = rf24_get_status(self);
  (*txOk) = status & 0x20;
  (*txFail) = status & 0x10;
  (*rxReady) = status & 0x40;
  rf24_write_register_2(self, RF24_STATUS, 0x70);
}

void rf24_write_address(rf24_t* self, uint8_t reg, uint64_t address) {
  uint8_t buf[5];
  for (uint8_t i = 0; i < self->addrWidth_; ++i) {
    buf[i] = ((uint8_t)(address >> (8 * i)));
  }
  rf24_write_register_3(self, reg, buf, self->addrWidth_);
}

void rf24_open_writing_pipe(rf24_t* self, uint64_t address) {
  rf24_write_address(self, RF24_RX_ADDR_P0, address);
  rf24_write_address(self, RF24_TX_ADDR, address);
  rf24_write_register_2(self, RF24_RX_PW_P0, self->payloadSize_);
}

void rf24_open_reading_pipe(rf24_t* self, uint8_t child, uint64_t address) {
  if (child > 5) return;
  if (child < 2) {
    rf24_write_address(self, RF24_RX_ADDR_P0 + child, address);
  } else {
    rf24_write_register_2(self, RF24_RX_ADDR_P0 + child, ((uint8_t)(address)));
  }
  rf24_write_register_2(self, RF24_RX_PW_P0 + child, self->payloadSize_);
  rf24_write_register_2(
      self, RF24_EN_RXADDR,
      rf24_read_register_1(self, RF24_EN_RXADDR) | (1 << child));
}

void rf24_close_reading_pipe(rf24_t* self, uint8_t pipe) {
  if (pipe <= 5)
    rf24_write_register_2(
        self, RF24_EN_RXADDR,
        rf24_read_register_1(self, RF24_EN_RXADDR) & ~(1 << pipe));
}

void rf24_set_address_width(rf24_t* self, uint8_t width) {
  self->addrWidth_ = DIV_MAX(3, DIV_MIN(5, width));
  rf24_write_register_2(self, RF24_SETUP_AW, self->addrWidth_ - 2);
}

void rf24_set_payload_size(rf24_t* self, uint8_t size) {
  self->payloadSize_ = DIV_MAX(1, DIV_MIN(32, size));
}

uint8_t rf24_get_dynamic_payload_size(rf24_t* self) {
  rf24_csn(self, false);
  rf24_transfer(self, RF24_R_RX_PL_WID);
  const uint8_t result = rf24_transfer(self, 0xFF);
  rf24_csn(self, true);
  if (result > 32) {
    rf24_flush_rx(self);
    return 0;
  }
  return result;
}

void rf24_toggle_features(rf24_t* self) {
  rf24_csn(self, false);
  rf24_transfer(self, RF24_ACTIVATE);
  rf24_transfer(self, 0x73);
  rf24_csn(self, true);
}

void rf24_enable_dynamic_payloads(rf24_t* self) {
  uint8_t feature = rf24_read_register_1(self, RF24_FEATURE);
  if (!feature) {
    rf24_toggle_features(self);
    feature = rf24_read_register_1(self, RF24_FEATURE);
  }
  rf24_write_register_2(self, RF24_FEATURE, feature | 0x04);
  rf24_write_register_2(self, RF24_DYNPD, 0x3F);
  self->dynamicPayloads_ = true;
}

void rf24_disable_dynamic_payloads(rf24_t* self) {
  rf24_write_register_2(self, RF24_FEATURE,
                        rf24_read_register_1(self, RF24_FEATURE) & ~0x04);
  rf24_write_register_2(self, RF24_DYNPD, 0);
  self->dynamicPayloads_ = false;
}

void rf24_enable_ack_payload(rf24_t* self) {
  uint8_t feature = rf24_read_register_1(self, RF24_FEATURE);
  if (!feature) {
    rf24_toggle_features(self);
    feature = rf24_read_register_1(self, RF24_FEATURE);
  }
  rf24_write_register_2(self, RF24_FEATURE, feature | 0x06);
  rf24_write_register_2(self, RF24_DYNPD,
                        rf24_read_register_1(self, RF24_DYNPD) | 0x03);
}

void rf24_write_ack_payload(rf24_t* self, uint8_t pipe, const void* buf,
                            uint8_t len) {
  if (pipe <= 5)
    rf24_write_payload(self, buf, DIV_MIN(len, 32), RF24_W_ACK_PAYLOAD | pipe);
}

bool rf24_is_ack_payload_available(rf24_t* self) {
  const bool result = self->ackPayloadAvailable_;
  self->ackPayloadAvailable_ = false;
  return result;
}

void rf24_set_auto_ack_1(rf24_t* self, bool enable) {
  rf24_write_register_2(self, RF24_EN_AA, enable ? 0x3F : 0);
}

void rf24_set_auto_ack_2(rf24_t* self, uint8_t pipe, bool enable) {
  if (pipe > 5) return;
  uint8_t en = rf24_read_register_1(self, RF24_EN_AA);
  en = enable ? (en | (1 << pipe)) : (en & ~(1 << pipe));
  rf24_write_register_2(self, RF24_EN_AA, en);
}

void rf24_set_retries(rf24_t* self, uint8_t retry_delay, uint8_t count) {
  rf24_write_register_2(self, RF24_SETUP_RETR,
                        ((retry_delay & 0x0F) << 4) | (count & 0x0F));
}

void rf24_set_channel(rf24_t* self, uint8_t channel) {
  rf24_write_register_2(self, RF24_RF_CH, DIV_MIN(channel, 125));
}

uint8_t rf24_get_channel(rf24_t* self) {
  return rf24_read_register_1(self, RF24_RF_CH);
}

void rf24_set_pa_level(rf24_t* self, uint8_t level, bool lnaEnable) {
  uint8_t setup = rf24_read_register_1(self, RF24_RF_SETUP) & ~0x06;
  setup |= (DIV_MIN(level, 3) << 1);
  rf24_write_register_2(self, RF24_RF_SETUP, setup);
}

uint8_t rf24_get_pa_level(rf24_t* self) {
  return (rf24_read_register_1(self, RF24_RF_SETUP) & 0x06) >> 1;
}

bool rf24_set_data_rate(rf24_t* self, uint8_t speed) {
  uint8_t setup = rf24_read_register_1(self, RF24_RF_SETUP) & ~0x28;
  if (speed == RF24_250KBPS) {
    setup |= 0x20;
  } else if (speed == RF24_2MBPS) {
    setup |= 0x08;
  }
  rf24_write_register_2(self, RF24_RF_SETUP, setup);
  return rf24_read_register_1(self, RF24_RF_SETUP) == setup;
}

uint8_t rf24_get_data_rate(rf24_t* self) {
  const uint8_t setup = rf24_read_register_1(self, RF24_RF_SETUP);
  if (setup & 0x20) return RF24_250KBPS;
  if (setup & 0x08) return RF24_2MBPS;
  return RF24_1MBPS;
}

void rf24_set_crc_length(rf24_t* self, uint8_t length) {
  uint8_t cfg = rf24_read_register_1(self, RF24_CONFIG) & ~0x0C;
  if (length == RF24_CRC_DISABLED) {
    cfg &= ~0x08;
  } else if (length == RF24_CRC_8) {
    cfg |= 0x08;
  } else {
    cfg |= 0x0C;
  }
  rf24_write_register_2(self, RF24_CONFIG, cfg);
}

uint8_t rf24_get_crc_length(rf24_t* self) {
  const uint8_t cfg = rf24_read_register_1(self, RF24_CONFIG) & 0x0C;
  if (!(cfg & 0x08)) return RF24_CRC_DISABLED;
  return (cfg & 0x04) ? RF24_CRC_16 : RF24_CRC_8;
}

void rf24_disable_crc(rf24_t* self) {
  rf24_set_crc_length(self, RF24_CRC_DISABLED);
}

void rf24_mask_irq(rf24_t* self, bool txOk, bool txFail, bool rxReady) {
  uint8_t cfg = rf24_read_register_1(self, RF24_CONFIG) & ~0x70;
  if (rxReady) cfg |= 0x40;
  if (txOk) cfg |= 0x20;
  if (txFail) cfg |= 0x10;
  rf24_write_register_2(self, RF24_CONFIG, cfg);
}

bool rf24_test_carrier(rf24_t* self) {
  return rf24_read_register_1(self, 0x09) & 0x01;
}

bool rf24_test_rpd(rf24_t* self) {
  return rf24_read_register_1(self, RF24_RPD) & 0x01;
}

void rf24_flush_rx(rf24_t* self) {
  rf24_csn(self, false);
  rf24_transfer(self, RF24_FLUSH_RX);
  rf24_csn(self, true);
}

void rf24_flush_tx(rf24_t* self) {
  rf24_csn(self, false);
  rf24_transfer(self, RF24_FLUSH_TX);
  rf24_csn(self, true);
}

void rf24_start_const_carrier(rf24_t* self, uint8_t level, uint8_t channel) {
  rf24_stop_listening(self);
  rf24_set_auto_ack_1(self, false);
  rf24_set_retries(self, 0, 0);
  rf24_set_pa_level(self, level, true);
  rf24_set_channel(self, channel);
  rf24_write_register_2(self, RF24_RF_SETUP,
                        rf24_read_register_1(self, RF24_RF_SETUP) | 0x90);
  uint8_t ff = 0xFF;
  rf24_write_payload(self, &ff, 1, RF24_W_TX_PAYLOAD);
  rf24_ce(self, true);
}

void rf24_stop_const_carrier(rf24_t* self) {
  rf24_ce(self, false);
  rf24_write_register_2(self, RF24_RF_SETUP,
                        rf24_read_register_1(self, RF24_RF_SETUP) & ~0x90);
  rf24_flush_tx(self);
}

void rf24_print_details(rf24_t* self) {}

void rf24_print_pretty_details(rf24_t* self) {}

uint8_t rf24_get_payload_size(const rf24_t* self) { return self->payloadSize_; }

uint8_t rf24_failure_detected(const rf24_t* self) {
  return self->failureDetected_;
}
