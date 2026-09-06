#pragma once
#include "c_util.h"
#include "spi_device.h"
#include "system_utils.h"

#define CC1101_IOCFG2 0x00
#define CC1101_IOCFG1 0x01
#define CC1101_IOCFG0 0x02
#define CC1101_FIFOTHR 0x03
#define CC1101_SYNC1 0x04
#define CC1101_SYNC0 0x05
#define CC1101_PKTLEN 0x06
#define CC1101_PKTCTRL1 0x07
#define CC1101_PKTCTRL0 0x08
#define CC1101_ADDR 0x09
#define CC1101_CHANNR 0x0A
#define CC1101_FSCTRL1 0x0B
#define CC1101_FSCTRL0 0x0C
#define CC1101_FREQ2 0x0D
#define CC1101_FREQ1 0x0E
#define CC1101_FREQ0 0x0F
#define CC1101_MDMCFG4 0x10
#define CC1101_MDMCFG3 0x11
#define CC1101_MDMCFG2 0x12
#define CC1101_MDMCFG1 0x13
#define CC1101_MDMCFG0 0x14
#define CC1101_DEVIATN 0x15
#define CC1101_MCSM2 0x16
#define CC1101_MCSM1 0x17
#define CC1101_MCSM0 0x18
#define CC1101_FOCCFG 0x19
#define CC1101_BSCFG 0x1A
#define CC1101_AGCCTRL2 0x1B
#define CC1101_AGCCTRL1 0x1C
#define CC1101_AGCCTRL0 0x1D
#define CC1101_WOREVT1 0x1E
#define CC1101_WOREVT0 0x1F
#define CC1101_WORCTRL 0x20
#define CC1101_FREND1 0x21
#define CC1101_FREND0 0x22
#define CC1101_FSCAL3 0x23
#define CC1101_FSCAL2 0x24
#define CC1101_FSCAL1 0x25
#define CC1101_FSCAL0 0x26
#define CC1101_RCCTRL1 0x27
#define CC1101_RCCTRL0 0x28
#define CC1101_FSTEST 0x29
#define CC1101_PTEST 0x2A
#define CC1101_AGCTEST 0x2B
#define CC1101_TEST2 0x2C
#define CC1101_TEST1 0x2D
#define CC1101_TEST0 0x2E

#define CC1101_SRES 0x30
#define CC1101_SFSTXON 0x31
#define CC1101_SXOFF 0x32
#define CC1101_SCAL 0x33
#define CC1101_SRX 0x34
#define CC1101_TXFIFO 0x3F
#define CC1101_STX 0x35
#define CC1101_SIDLE 0x36
#define CC1101_SAFC 0x37
#define CC1101_SWOR 0x38
#define CC1101_SPWD 0x39
#define CC1101_SFRX 0x3A
#define CC1101_SFTX 0x3B
#define CC1101_SWORRST 0x3C
#define CC1101_SNOP 0x3D

#define CC1101_PARTNUM 0x30
#define CC1101_VERSION 0x31
#define CC1101_FREQEST 0x32
#define CC1101_LQI 0x33
#define CC1101_RSSI 0x34
#define CC1101_MARCSTATE 0x35
#define CC1101_WORTIME1 0x36
#define CC1101_WORTIME0 0x37
#define CC1101_PKTSTATUS 0x38
#define CC1101_VCO_VC_DAC 0x39
#define CC1101_TXBYTES 0x3A
#define CC1101_RXBYTES 0x3B

#define CC1101_PATABLE 0x3E
#define CC1101_RXFIFO 0x3F

#define CC1101_kMaxModules (6)

typedef struct {
  spi_device_t spi_;
  uint8_t modulation_;
  uint8_t frend0_;
  uint8_t channel_;
  int pa_;
  uint8_t lastPaBand_;
  uint8_t sck_;
  uint8_t miso_;
  uint8_t mosi_;
  uint8_t ss_;
  uint8_t gdo0_;
  uint8_t gdo2_;
  uint8_t sckM_[CC1101_kMaxModules];
  uint8_t misoM_[CC1101_kMaxModules];
  uint8_t mosiM_[CC1101_kMaxModules];
  uint8_t ssM_[CC1101_kMaxModules];
  uint8_t gdo0M_[CC1101_kMaxModules];
  uint8_t gdo2M_[CC1101_kMaxModules];
  uint8_t gdoSetMode_;
  bool customSpi_;
  bool ccMode_;
  float mhz_;
  uint8_t m4RxBw_;
  uint8_t m4DaRa_;
  uint8_t m2DCOFF_;
  uint8_t m2MODFM_;
  uint8_t m2MANCH_;
  uint8_t m2SYNCM_;
  uint8_t m1FEC_;
  uint8_t m1PRE_;
  uint8_t m1CHSP_;
  uint8_t pc1PQT_;
  uint8_t pc1CRC_AF_;
  uint8_t pc1APP_ST_;
  uint8_t pc1ADRCHK_;
  uint8_t pc0WDATA_;
  uint8_t pc0PktForm_;
  uint8_t pc0CRC_EN_;
  uint8_t pc0LenConf_;
  uint8_t trxstate_;
  uint8_t clb1_[2];
  uint8_t clb2_[2];
  uint8_t clb3_[2];
  uint8_t clb4_[2];
} cc1101_t;
#define CC1101_INITIALIZER \
  {.modulation_ = 2,       \
   .frend0_ = 0,           \
   .channel_ = 0,          \
   .pa_ = 12,              \
   .lastPaBand_ = 0,       \
   .sck_ = 12,             \
   .miso_ = 13,            \
   .mosi_ = 11,            \
   .ss_ = 5,               \
   .gdo0_ = 0xFF,          \
   .gdo2_ = 0xFF,          \
   .gdoSetMode_ = 0,       \
   .customSpi_ = false,    \
   .ccMode_ = false,       \
   .mhz_ = 433.92f,        \
   .m4RxBw_ = 0,           \
   .m4DaRa_ = 0,           \
   .m2DCOFF_ = 0,          \
   .m2MODFM_ = 0,          \
   .m2MANCH_ = 0,          \
   .m2SYNCM_ = 0,          \
   .m1FEC_ = 0,            \
   .m1PRE_ = 0,            \
   .m1CHSP_ = 0,           \
   .pc1PQT_ = 0,           \
   .pc1CRC_AF_ = 0,        \
   .pc1APP_ST_ = 0,        \
   .pc1ADRCHK_ = 0,        \
   .pc0WDATA_ = 0,         \
   .pc0PktForm_ = 0,       \
   .pc0CRC_EN_ = 0,        \
   .pc0LenConf_ = 0,       \
   .trxstate_ = 0,         \
   .clb1_ = {24, 28},      \
   .clb2_ = {31, 38},      \
   .clb3_ = {65, 76},      \
   .clb4_ = {77, 79}}

bool cc1101_ready(const cc1101_t* self);
void cc1101_init(cc1101_t* self);
uint8_t cc1101_spi_read_status(cc1101_t* self, uint8_t addr);
void cc1101_set_spi_pin(cc1101_t* self, uint8_t sck, uint8_t miso, uint8_t mosi,
                        uint8_t ss);
void cc1101_add_spi_pin(cc1101_t* self, uint8_t sck, uint8_t miso, uint8_t mosi,
                        uint8_t ss, uint8_t modul);
void cc1101_set_gdo(cc1101_t* self, uint8_t gdo0, uint8_t gdo2);
void cc1101_set_gdo0(cc1101_t* self, uint8_t gdo0);
void cc1101_add_gdo(cc1101_t* self, uint8_t gdo0, uint8_t gdo2, uint8_t modul);
void cc1101_add_gdo0(cc1101_t* self, uint8_t gdo0, uint8_t modul);
void cc1101_set_modul(cc1101_t* self, uint8_t modul);
void cc1101_set_cc_mode(cc1101_t* self, bool s);
void cc1101_set_modulation(cc1101_t* self, uint8_t m);
void cc1101_set_pa(cc1101_t* self, int p);
void cc1101_set_mhz(cc1101_t* self, float mhz);
void cc1101_set_channel(cc1101_t* self, uint8_t chnl);
void cc1101_set_chsp(cc1101_t* self, float f);
void cc1101_set_rx_bw(cc1101_t* self, float f);
void cc1101_set_d_rate(cc1101_t* self, float d);
void cc1101_set_deviation(cc1101_t* self, float d);
void cc1101_set_tx_0(cc1101_t* self);
void cc1101_set_rx_0(cc1101_t* self);
void cc1101_set_tx_1(cc1101_t* self, float mhz);
void cc1101_set_rx_1(cc1101_t* self, float mhz);
int cc1101_get_rssi(cc1101_t* self);
uint8_t cc1101_get_lqi(cc1101_t* self);
void cc1101_set_sres(cc1101_t* self);
void cc1101_set_sidle(cc1101_t* self);
void cc1101_go_sleep(cc1101_t* self);
void cc1101_send_data_2_bytes(cc1101_t* self, uint8_t* txBuffer, uint8_t size);
void cc1101_send_data_1(cc1101_t* self, char* txchar);
void cc1101_send_data_3(cc1101_t* self, uint8_t* txBuffer, uint8_t size, int t);
void cc1101_send_data_2_text(cc1101_t* self, char* txchar, int t);
uint8_t cc1101_check_receive_flag(cc1101_t* self);
uint8_t cc1101_receive_data(cc1101_t* self, uint8_t* rxBuffer);
bool cc1101_check_crc(cc1101_t* self);
void cc1101_spi_strobe(cc1101_t* self, uint8_t strobe);
void cc1101_spi_write_reg(cc1101_t* self, uint8_t addr, uint8_t value);
void cc1101_spi_write_burst_reg(cc1101_t* self, uint8_t addr, uint8_t* buffer,
                                uint8_t num);
uint8_t cc1101_spi_read_reg(cc1101_t* self, uint8_t addr);
void cc1101_spi_read_burst_reg(cc1101_t* self, uint8_t addr, uint8_t* buffer,
                               uint8_t num);
void cc1101_set_clb(cc1101_t* self, uint8_t b, uint8_t s, uint8_t e);
bool cc1101_get_cc1101(cc1101_t* self);
uint8_t cc1101_get_mode(cc1101_t* self);
void cc1101_set_sync_word(cc1101_t* self, uint8_t sh, uint8_t sl);
void cc1101_set_addr(cc1101_t* self, uint8_t v);
void cc1101_set_white_data(cc1101_t* self, bool v);
void cc1101_set_pkt_format(cc1101_t* self, uint8_t v);
void cc1101_set_crc(cc1101_t* self, bool v);
void cc1101_set_length_config(cc1101_t* self, uint8_t v);
void cc1101_set_packet_length(cc1101_t* self, uint8_t v);
void cc1101_set_dc_filter_off(cc1101_t* self, bool v);
void cc1101_set_manchester(cc1101_t* self, bool v);
void cc1101_set_sync_mode(cc1101_t* self, uint8_t v);
void cc1101_set_fec(cc1101_t* self, bool v);
void cc1101_set_pre(cc1101_t* self, uint8_t v);
void cc1101_set_pqt(cc1101_t* self, uint8_t v);
void cc1101_set_crc_af(cc1101_t* self, bool v);
void cc1101_set_append_status(cc1101_t* self, bool v);
void cc1101_set_adr_chk(cc1101_t* self, uint8_t v);
bool cc1101_check_rx_fifo(cc1101_t* self, int t);
void cc1101_spi_start(cc1101_t* self);
void cc1101_spi_end(cc1101_t* self);
void cc1101_gdo_set(cc1101_t* self);
void cc1101_gdo0_set(cc1101_t* self);
void cc1101_reset(cc1101_t* self);
void cc1101_set_spi_defaults(cc1101_t* self);
void cc1101_reg_config_settings(cc1101_t* self);
void cc1101_calibrate(cc1101_t* self);
void cc1101_split_pktctrl0(cc1101_t* self);
void cc1101_split_pktctrl1(cc1101_t* self);
void cc1101_split_mdmcfg1(cc1101_t* self);
void cc1101_split_mdmcfg2(cc1101_t* self);
void cc1101_split_mdmcfg4(cc1101_t* self);
uint8_t cc1101_transfer(cc1101_t* self, uint8_t value);
void cc1101_select(cc1101_t* self);
void cc1101_deselect(cc1101_t* self);
void cc1101_wait_miso_low(cc1101_t* self, uint32_t timeoutUs);
extern cc1101_t cc1101;
