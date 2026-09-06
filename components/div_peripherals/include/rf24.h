#pragma once
#include "c_util.h"
#include "spi_device.h"

#define RF24_PA_MAX 3
#define RF24_PA_HIGH 2
#define RF24_PA_LOW 1
#define RF24_PA_MIN 0
#define RF24_2MBPS 2
#define RF24_1MBPS 1
#define RF24_250KBPS 0
#define RF24_CRC_DISABLED 0
#define RF24_CRC_8 1
#define RF24_CRC_16 2

enum rf24_crclength_e {
  RF24_CRC_DISABLED_E = RF24_CRC_DISABLED,
  RF24_CRC_8_E = RF24_CRC_8,
  RF24_CRC_16_E = RF24_CRC_16,
};

enum rf24_datarate_e {
  RF24_250KBPS_E = RF24_250KBPS,
  RF24_1MBPS_E = RF24_1MBPS,
  RF24_2MBPS_E = RF24_2MBPS,
};

enum rf24_pa_dbm_e {
  RF24_PA_MIN_E = RF24_PA_MIN,
  RF24_PA_LOW_E = RF24_PA_LOW,
  RF24_PA_HIGH_E = RF24_PA_HIGH,
  RF24_PA_MAX_E = RF24_PA_MAX,
};

#define RF24_CONFIG (0x00)

#define RF24_EN_AA (0x01)

#define RF24_EN_RXADDR (0x02)

#define RF24_SETUP_AW (0x03)

#define RF24_SETUP_RETR (0x04)

#define RF24_RF_CH (0x05)

#define RF24_RF_SETUP (0x06)

#define RF24_STATUS (0x07)

#define RF24_OBSERVE_TX (0x08)

#define RF24_RPD (0x09)

#define RF24_RX_ADDR_P0 (0x0A)

#define RF24_RX_ADDR_P1 (0x0B)

#define RF24_RX_ADDR_P2 (0x0C)

#define RF24_TX_ADDR (0x10)

#define RF24_RX_PW_P0 (0x11)

#define RF24_FIFO_STATUS (0x17)

#define RF24_DYNPD (0x1C)

#define RF24_FEATURE (0x1D)

#define RF24_R_REGISTER (0x00)

#define RF24_W_REGISTER (0x20)

#define RF24_REGISTER_MASK (0x1F)

#define RF24_ACTIVATE (0x50)

#define RF24_R_RX_PL_WID (0x60)

#define RF24_R_RX_PAYLOAD (0x61)

#define RF24_W_TX_PAYLOAD (0xA0)

#define RF24_W_ACK_PAYLOAD (0xA8)

#define RF24_FLUSH_TX (0xE1)

#define RF24_FLUSH_RX (0xE2)

#define RF24_REUSE_TX_PL (0xE3)

#define RF24_NOP (0xFF)

typedef struct {
  spi_device_t spi_;
  uint8_t cePin_;
  uint8_t csnPin_;
  uint32_t spiSpeed_;
  uint8_t payloadSize_;
  uint8_t addrWidth_;
  bool dynamicPayloads_;
  bool ackPayloadAvailable_;
  bool pVariant_;
  bool failureDetected_;
} rf24_t;
#define RF24_INITIALIZER          \
  {.payloadSize_ = 32,            \
   .addrWidth_ = 5,               \
   .dynamicPayloads_ = false,     \
   .ackPayloadAvailable_ = false, \
   .pVariant_ = false,            \
   .failureDetected_ = false}

void rf24_construct(rf24_t* self, uint8_t cePin, uint8_t csnPin,
                    uint32_t spiSpeed);
bool rf24_begin(rf24_t* self);
bool rf24_is_chip_connected(rf24_t* self);
void rf24_power_up(rf24_t* self);
void rf24_power_down(rf24_t* self);
void rf24_start_listening(rf24_t* self);
void rf24_stop_listening(rf24_t* self);
bool rf24_available_0(rf24_t* self);
bool rf24_available_1(rf24_t* self, uint8_t* pipeNum);
void rf24_read(rf24_t* self, void* buf, uint8_t len);
bool rf24_write(rf24_t* self, const void* buf, uint8_t len);
bool rf24_write_fast(rf24_t* self, const void* buf, uint8_t len);
bool rf24_write_blocking(rf24_t* self, const void* buf, uint8_t len,
                         uint32_t timeout);
void rf24_start_write(rf24_t* self, const void* buf, uint8_t len,
                      bool multicast);
bool rf24_tx_stand_by_0(rf24_t* self);
bool rf24_tx_stand_by_2(rf24_t* self, uint32_t timeout, bool startTx);
void rf24_what_happened(rf24_t* self, bool* txOk, bool* txFail, bool* rxReady);
void rf24_open_writing_pipe(rf24_t* self, uint64_t address);
void rf24_open_reading_pipe(rf24_t* self, uint8_t child, uint64_t address);
void rf24_close_reading_pipe(rf24_t* self, uint8_t pipe);
void rf24_set_address_width(rf24_t* self, uint8_t width);
void rf24_set_payload_size(rf24_t* self, uint8_t size);
uint8_t rf24_get_payload_size(const rf24_t* self);
uint8_t rf24_get_dynamic_payload_size(rf24_t* self);
void rf24_enable_dynamic_payloads(rf24_t* self);
void rf24_disable_dynamic_payloads(rf24_t* self);
void rf24_enable_ack_payload(rf24_t* self);
void rf24_write_ack_payload(rf24_t* self, uint8_t pipe, const void* buf,
                            uint8_t len);
bool rf24_is_ack_payload_available(rf24_t* self);
void rf24_set_auto_ack_1(rf24_t* self, bool enable);
void rf24_set_auto_ack_2(rf24_t* self, uint8_t pipe, bool enable);
void rf24_set_retries(rf24_t* self, uint8_t retry_delay, uint8_t count);
void rf24_set_channel(rf24_t* self, uint8_t channel);
uint8_t rf24_get_channel(rf24_t* self);
void rf24_set_pa_level(rf24_t* self, uint8_t level, bool lnaEnable);
uint8_t rf24_get_pa_level(rf24_t* self);
bool rf24_set_data_rate(rf24_t* self, uint8_t speed);
uint8_t rf24_get_data_rate(rf24_t* self);
void rf24_set_crc_length(rf24_t* self, uint8_t length);
uint8_t rf24_get_crc_length(rf24_t* self);
void rf24_disable_crc(rf24_t* self);
void rf24_mask_irq(rf24_t* self, bool txOk, bool txFail, bool rxReady);
bool rf24_test_carrier(rf24_t* self);
bool rf24_test_rpd(rf24_t* self);
void rf24_flush_rx(rf24_t* self);
void rf24_flush_tx(rf24_t* self);
uint8_t rf24_get_status(rf24_t* self);
uint8_t rf24_failure_detected(const rf24_t* self);
void rf24_start_const_carrier(rf24_t* self, uint8_t level, uint8_t channel);
void rf24_stop_const_carrier(rf24_t* self);
void rf24_print_details(rf24_t* self);
void rf24_print_pretty_details(rf24_t* self);
uint8_t rf24_transfer(rf24_t* self, uint8_t value);
void rf24_csn(rf24_t* self, bool high);
void rf24_ce(rf24_t* self, bool high);
uint8_t rf24_read_register_1(rf24_t* self, uint8_t reg);
void rf24_read_register_3(rf24_t* self, uint8_t reg, uint8_t* buf, uint8_t len);
uint8_t rf24_write_register_2(rf24_t* self, uint8_t reg, uint8_t value);
uint8_t rf24_write_register_3(rf24_t* self, uint8_t reg, const uint8_t* buf,
                              uint8_t len);
uint8_t rf24_write_payload(rf24_t* self, const void* buf, uint8_t len,
                           uint8_t command);
uint8_t rf24_read_payload(rf24_t* self, void* buf, uint8_t len);
void rf24_toggle_features(rf24_t* self);
void rf24_write_address(rf24_t* self, uint8_t reg, uint64_t address);
