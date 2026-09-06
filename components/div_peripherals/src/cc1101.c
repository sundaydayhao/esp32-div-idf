#include "cc1101.h"

#include "board_pins.h"

static const uint8_t WRITE_BURST = 0x40;
static const uint8_t READ_SINGLE = 0x80;
static const uint8_t READ_BURST = 0xC0;
static const uint8_t BYTES_IN_RXFIFO = 0x7F;

uint8_t paTable[8] = {0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t paTable315[8] = {0x12, 0x0D, 0x1C, 0x34,
                                      0x51, 0x85, 0xCB, 0xC2};
static const uint8_t paTable433[8] = {0x12, 0x0E, 0x1D, 0x34,
                                      0x60, 0x84, 0xC8, 0xC0};
static const uint8_t paTable868[10] = {0x03, 0x17, 0x1D, 0x26, 0x37,
                                       0x50, 0x86, 0xCD, 0xC5, 0xC0};
static const uint8_t paTable915[10] = {0x03, 0x0E, 0x1E, 0x27, 0x38,
                                       0x8E, 0x84, 0xCC, 0xC3, 0xC0};

static uint8_t choosePa(const uint8_t* table, size_t n, int p, bool hasMinus6) {
  if (p <= -30) return table[0];
  if (p <= -20) return table[1];
  if (p <= -15) return table[2];
  if (p <= -10) return table[3];
  if (hasMinus6 && p <= -6) return table[4];
  if (p <= 0) return table[hasMinus6 ? 5 : 4];
  if (p <= 5) return table[hasMinus6 ? 6 : 5];
  if (p <= 7) return table[hasMinus6 ? 7 : 6];
  if (n > 8 && p <= 10) return table[8];
  return table[n - 1];
}

static uint8_t clampByte(int v) {
  return ((uint8_t)(DIV_MAX(0, DIV_MIN(255, v))));
}

cc1101_t cc1101 = CC1101_INITIALIZER;

void cc1101_spi_start(cc1101_t* self) {
  spi_device_init(&self->spi_, DIV_SD_HOST, self->sck_, self->miso_,
                  self->mosi_, self->ss_, 4000000, false);
}

void cc1101_spi_end(cc1101_t* self) { spi_device_deselect(&self->spi_); }

uint8_t cc1101_transfer(cc1101_t* self, uint8_t value) {
  return spi_device_transfer(&self->spi_, value);
}

void cc1101_select(cc1101_t* self) {
  if (spi_device_select(&self->spi_)) cc1101_wait_miso_low(self, 10000);
}

void cc1101_deselect(cc1101_t* self) { spi_device_deselect(&self->spi_); }

void cc1101_wait_miso_low(cc1101_t* self, uint32_t timeoutUs) {
  const uint32_t start = sys_monotonic_us();
  while (gpio_get_level(((gpio_num_t)(self->miso_))) &&
         (sys_monotonic_us() - start) < timeoutUs) {
    esp_rom_delay_us(1);
  }
}

void cc1101_gdo_set(cc1101_t* self) {
  if (self->gdo0_ != 0xFF) sys_configure_gpio(self->gdo0_, GPIO_MODE_OUTPUT);
  if (self->gdo2_ != 0xFF) sys_configure_gpio(self->gdo2_, GPIO_MODE_INPUT);
}

void cc1101_gdo0_set(cc1101_t* self) {
  if (self->gdo0_ != 0xFF) sys_configure_gpio(self->gdo0_, GPIO_MODE_INPUT);
}

void cc1101_reset(cc1101_t* self) {
  cc1101_spi_start(self);
  spi_device_select(&self->spi_);
  esp_rom_delay_us(10);
  spi_device_deselect(&self->spi_);
  esp_rom_delay_us(40);
  cc1101_select(self);
  cc1101_transfer(self, CC1101_SRES);
  cc1101_wait_miso_low(self, 10000);
  cc1101_deselect(self);
  sys_sleep_ms(1);
}

void cc1101_set_spi_defaults(cc1101_t* self) {
  if (!self->customSpi_) {
    self->sck_ = CC1101_SCK;
    self->miso_ = CC1101_MISO;
    self->mosi_ = CC1101_MOSI;
    self->ss_ = CC1101_CS;
  }
}

void cc1101_init(cc1101_t* self) {
  cc1101_set_spi_defaults(self);
  cc1101_reset(self);
  cc1101_reg_config_settings(self);
}

void cc1101_spi_write_reg(cc1101_t* self, uint8_t addr, uint8_t value) {
  cc1101_spi_start(self);
  cc1101_select(self);
  cc1101_transfer(self, addr);
  cc1101_transfer(self, value);
  cc1101_deselect(self);
}

void cc1101_spi_write_burst_reg(cc1101_t* self, uint8_t addr, uint8_t* buffer,
                                uint8_t num) {
  cc1101_spi_start(self);
  cc1101_select(self);
  cc1101_transfer(self, addr | WRITE_BURST);
  for (uint8_t i = 0; i < num; ++i) cc1101_transfer(self, buffer[i]);
  cc1101_deselect(self);
}

void cc1101_spi_strobe(cc1101_t* self, uint8_t strobe) {
  cc1101_spi_start(self);
  cc1101_select(self);
  cc1101_transfer(self, strobe);
  cc1101_deselect(self);
}

uint8_t cc1101_spi_read_reg(cc1101_t* self, uint8_t addr) {
  cc1101_spi_start(self);
  cc1101_select(self);
  cc1101_transfer(self, addr | READ_SINGLE);
  const uint8_t value = cc1101_transfer(self, 0);
  cc1101_deselect(self);
  return value;
}

void cc1101_spi_read_burst_reg(cc1101_t* self, uint8_t addr, uint8_t* buffer,
                               uint8_t num) {
  cc1101_spi_start(self);
  cc1101_select(self);
  cc1101_transfer(self, addr | READ_BURST);
  for (uint8_t i = 0; i < num; ++i) buffer[i] = cc1101_transfer(self, 0);
  cc1101_deselect(self);
}

uint8_t cc1101_spi_read_status(cc1101_t* self, uint8_t addr) {
  cc1101_spi_start(self);
  cc1101_select(self);
  cc1101_transfer(self, addr | READ_BURST);
  const uint8_t value = cc1101_transfer(self, 0);
  cc1101_deselect(self);
  return value;
}

void cc1101_set_spi_pin(cc1101_t* self, uint8_t sck, uint8_t miso, uint8_t mosi,
                        uint8_t ss) {
  self->customSpi_ = true;
  self->sck_ = sck;
  self->miso_ = miso;
  self->mosi_ = mosi;
  self->ss_ = ss;
}

void cc1101_add_spi_pin(cc1101_t* self, uint8_t sck, uint8_t miso, uint8_t mosi,
                        uint8_t ss, uint8_t modul) {
  if (modul >= CC1101_kMaxModules) return;
  self->customSpi_ = true;
  self->sckM_[modul] = sck;
  self->misoM_[modul] = miso;
  self->mosiM_[modul] = mosi;
  self->ssM_[modul] = ss;
}

void cc1101_set_gdo(cc1101_t* self, uint8_t gdo0, uint8_t gdo2) {
  self->gdo0_ = gdo0;
  self->gdo2_ = gdo2;
  cc1101_gdo_set(self);
}

void cc1101_set_gdo0(cc1101_t* self, uint8_t gdo0) {
  self->gdo0_ = gdo0;
  cc1101_gdo0_set(self);
}

void cc1101_add_gdo(cc1101_t* self, uint8_t gdo0, uint8_t gdo2, uint8_t modul) {
  if (modul >= CC1101_kMaxModules) return;
  self->gdo0M_[modul] = gdo0;
  self->gdo2M_[modul] = gdo2;
  self->gdoSetMode_ = 2;
}

void cc1101_add_gdo0(cc1101_t* self, uint8_t gdo0, uint8_t modul) {
  if (modul >= CC1101_kMaxModules) return;
  self->gdo0M_[modul] = gdo0;
  self->gdoSetMode_ = 1;
}

void cc1101_set_modul(cc1101_t* self, uint8_t modul) {
  if (modul >= CC1101_kMaxModules) return;
  if (self->customSpi_) {
    self->sck_ = self->sckM_[modul];
    self->miso_ = self->misoM_[modul];
    self->mosi_ = self->mosiM_[modul];
    self->ss_ = self->ssM_[modul];
  }
  if (self->gdoSetMode_ >= 1) self->gdo0_ = self->gdo0M_[modul];
  if (self->gdoSetMode_ >= 2) self->gdo2_ = self->gdo2M_[modul];
  if (self->gdoSetMode_ == 1) cc1101_gdo0_set(self);
  if (self->gdoSetMode_ == 2) cc1101_gdo_set(self);
}

void cc1101_set_cc_mode(cc1101_t* self, bool s) {
  self->ccMode_ = s;
  if (s) {
    cc1101_spi_write_reg(self, CC1101_IOCFG2, 0x0B);
    cc1101_spi_write_reg(self, CC1101_IOCFG0, 0x06);
    cc1101_spi_write_reg(self, CC1101_PKTCTRL0, 0x05);
    cc1101_spi_write_reg(self, CC1101_MDMCFG3, 0xF8);
    cc1101_spi_write_reg(self, CC1101_MDMCFG2, 0x93);
    cc1101_spi_write_reg(self, CC1101_MDMCFG1, 0x22);
    cc1101_spi_write_reg(self, CC1101_MDMCFG0, 0xF8);
    cc1101_spi_write_reg(self, CC1101_DEVIATN, 0x47);
  } else {
    cc1101_spi_write_reg(self, CC1101_IOCFG2, 0x0D);
    cc1101_spi_write_reg(self, CC1101_IOCFG0, 0x0D);
    cc1101_spi_write_reg(self, CC1101_PKTCTRL0, 0x32);
    cc1101_spi_write_reg(self, CC1101_MDMCFG3, 0x93);
    cc1101_spi_write_reg(self, CC1101_MDMCFG2, 0x30);
    cc1101_spi_write_reg(self, CC1101_MDMCFG1, 0x22);
    cc1101_spi_write_reg(self, CC1101_MDMCFG0, 0xF8);
    cc1101_spi_write_reg(self, CC1101_DEVIATN, 0x00);
  }
}

void cc1101_set_modulation(cc1101_t* self, uint8_t m) {
  if (m > 4) m = 4;
  self->modulation_ = m;
  cc1101_split_mdmcfg2(self);
  static const uint8_t modBits[] = {0x00, 0x10, 0x30, 0x40, 0x70};
  self->m2MODFM_ = modBits[m];
  cc1101_spi_write_reg(
      self, CC1101_MDMCFG2,
      self->m2DCOFF_ + self->m2MODFM_ + self->m2MANCH_ + self->m2SYNCM_);
  cc1101_set_pa(self, self->pa_);
}

void cc1101_set_pa(cc1101_t* self, int p) {
  self->pa_ = p;
  uint8_t a = 0xC0;
  if (self->mhz_ >= 300 && self->mhz_ <= 348) {
    a = choosePa(paTable315, 8, p, false);
    self->lastPaBand_ = 1;
  } else if (self->mhz_ >= 378 && self->mhz_ <= 464) {
    a = choosePa(paTable433, 8, p, false);
    self->lastPaBand_ = 2;
  } else if (self->mhz_ >= 779 && self->mhz_ <= 899.99f) {
    a = choosePa(paTable868, 10, p, true);
    self->lastPaBand_ = 3;
  } else if (self->mhz_ >= 900 && self->mhz_ <= 928) {
    a = choosePa(paTable915, 10, p, true);
    self->lastPaBand_ = 4;
  }
  if (self->modulation_ == 2) {
    paTable[0] = 0;
    paTable[1] = a;
  } else {
    paTable[0] = a;
    paTable[1] = 0;
  }
  cc1101_spi_write_burst_reg(self, CC1101_PATABLE, paTable, sizeof(paTable));
}

void cc1101_set_mhz(cc1101_t* self, float mhz) {
  self->mhz_ = mhz;
  const uint32_t freq = ((uint32_t)((mhz * 65536.0f) / 26.0f));
  cc1101_spi_write_reg(self, CC1101_FREQ2, (freq >> 16) & 0xFF);
  cc1101_spi_write_reg(self, CC1101_FREQ1, (freq >> 8) & 0xFF);
  cc1101_spi_write_reg(self, CC1101_FREQ0, freq & 0xFF);
  cc1101_calibrate(self);
}

void cc1101_calibrate(cc1101_t* self) {
  if (self->mhz_ >= 300 && self->mhz_ <= 348) {
    cc1101_spi_write_reg(self, CC1101_FSCTRL0,
                         clampByte(sys_scale(((long)(self->mhz_)), 300, 348,
                                             self->clb1_[0], self->clb1_[1])));
    cc1101_spi_write_reg(self, CC1101_TEST0,
                         self->mhz_ < 322.88f ? 0x0B : 0x09);
    if (self->lastPaBand_ != 1) cc1101_set_pa(self, self->pa_);
  } else if (self->mhz_ >= 378 && self->mhz_ <= 464) {
    cc1101_spi_write_reg(self, CC1101_FSCTRL0,
                         clampByte(sys_scale(((long)(self->mhz_)), 378, 464,
                                             self->clb2_[0], self->clb2_[1])));
    cc1101_spi_write_reg(self, CC1101_TEST0, self->mhz_ < 430.5f ? 0x0B : 0x09);
    if (self->lastPaBand_ != 2) cc1101_set_pa(self, self->pa_);
  } else if (self->mhz_ >= 779 && self->mhz_ <= 899.99f) {
    cc1101_spi_write_reg(self, CC1101_FSCTRL0,
                         clampByte(sys_scale(((long)(self->mhz_)), 779, 899,
                                             self->clb3_[0], self->clb3_[1])));
    cc1101_spi_write_reg(self, CC1101_TEST0, self->mhz_ < 861 ? 0x0B : 0x09);
    if (self->lastPaBand_ != 3) cc1101_set_pa(self, self->pa_);
  } else if (self->mhz_ >= 900 && self->mhz_ <= 928) {
    cc1101_spi_write_reg(self, CC1101_FSCTRL0,
                         clampByte(sys_scale(((long)(self->mhz_)), 900, 928,
                                             self->clb4_[0], self->clb4_[1])));
    cc1101_spi_write_reg(self, CC1101_TEST0, 0x09);
    if (self->lastPaBand_ != 4) cc1101_set_pa(self, self->pa_);
  }
  int s = cc1101_spi_read_status(self, CC1101_FSCAL2);
  if (s < 32) cc1101_spi_write_reg(self, CC1101_FSCAL2, ((uint8_t)(s + 32)));
}

void cc1101_set_channel(cc1101_t* self, uint8_t chnl) {
  self->channel_ = chnl;
  cc1101_spi_write_reg(self, CC1101_CHANNR, chnl);
}

void cc1101_set_chsp(cc1101_t* self, float f) {
  cc1101_split_mdmcfg1(self);
  uint8_t mdmcfg0 = 0;
  int e = 0;
  float c = f;
  if (c > 405.456543f) c = 405.456543f;
  if (c < 25.390625f) c = 25.390625f;
  for (int i = 0; i < 3; ++i) {
    if (c <= 50.682068f) {
      c = (c - 25.390625f) / 0.0991825f;
      mdmcfg0 = ((uint8_t)(c + 0.5f));
      break;
    }
    e++;
    c /= 2;
  }
  self->m1CHSP_ = ((uint8_t)(e));
  cc1101_spi_write_reg(self, CC1101_MDMCFG1,
                       self->m1FEC_ + self->m1PRE_ + self->m1CHSP_);
  cc1101_spi_write_reg(self, CC1101_MDMCFG0, mdmcfg0);
}

void cc1101_set_rx_bw(cc1101_t* self, float f) {
  cc1101_split_mdmcfg4(self);
  int s1 = 3;
  int s2 = 3;
  for (int i = 0; i < 3 && f > 101.5625f; ++i) {
    f /= 2;
    s1--;
  }
  for (int i = 0; i < 3 && f > 58.1f; ++i) {
    f /= 1.25f;
    s2--;
  }
  self->m4RxBw_ = ((uint8_t)(s1 * 64 + s2 * 16));
  cc1101_spi_write_reg(self, CC1101_MDMCFG4, self->m4RxBw_ + self->m4DaRa_);
}

void cc1101_set_d_rate(cc1101_t* self, float d) {
  cc1101_split_mdmcfg4(self);
  d = DIV_MAX(0.0247955f, DIV_MIN(1621.83f, d));
  self->m4DaRa_ = 0;
  uint8_t mdmcfg3 = 0;
  for (int i = 0; i < 20; ++i) {
    if (d <= 0.0494942f) {
      float c = (d - 0.0247955f) / 0.00009685f;
      mdmcfg3 = ((uint8_t)(c + 0.5f));
      break;
    }
    self->m4DaRa_++;
    d /= 2;
  }
  cc1101_spi_write_reg(self, CC1101_MDMCFG4, self->m4RxBw_ + self->m4DaRa_);
  cc1101_spi_write_reg(self, CC1101_MDMCFG3, mdmcfg3);
}

void cc1101_set_deviation(cc1101_t* self, float d) {
  d = DIV_MAX(1.586914f, DIV_MIN(380.859375f, d));
  float f = 1.586914f;
  float v = 0.19836425f;
  int c = 0;
  for (int i = 0; i < 255; ++i) {
    f += v;
    if (c == 7) {
      v *= 2;
      c = -1;
      i += 8;
    }
    if (f >= d) {
      c = i;
      break;
    }
    c++;
  }
  cc1101_spi_write_reg(self, CC1101_DEVIATN, clampByte(c));
}

void cc1101_reg_config_settings(cc1101_t* self) {
  cc1101_spi_write_reg(self, CC1101_FSCTRL1, 0x06);
  cc1101_set_cc_mode(self, self->ccMode_);
  cc1101_set_mhz(self, self->mhz_);
  cc1101_spi_write_reg(self, CC1101_MDMCFG1, 0x02);
  cc1101_spi_write_reg(self, CC1101_MDMCFG0, 0xF8);
  cc1101_spi_write_reg(self, CC1101_CHANNR, self->channel_);
  cc1101_spi_write_reg(self, CC1101_DEVIATN, 0x47);
  cc1101_spi_write_reg(self, CC1101_FREND1, 0x56);
  cc1101_spi_write_reg(self, CC1101_MCSM0, 0x18);
  cc1101_spi_write_reg(self, CC1101_FOCCFG, 0x16);
  cc1101_spi_write_reg(self, CC1101_BSCFG, 0x1C);
  cc1101_spi_write_reg(self, CC1101_AGCCTRL2, 0xC7);
  cc1101_spi_write_reg(self, CC1101_AGCCTRL1, 0x00);
  cc1101_spi_write_reg(self, CC1101_AGCCTRL0, 0xB2);
  cc1101_spi_write_reg(self, CC1101_FSCAL3, 0xE9);
  cc1101_spi_write_reg(self, CC1101_FSCAL2, 0x2A);
  cc1101_spi_write_reg(self, CC1101_FSCAL1, 0x00);
  cc1101_spi_write_reg(self, CC1101_FSCAL0, 0x1F);
  cc1101_spi_write_reg(self, CC1101_FSTEST, 0x59);
  cc1101_spi_write_reg(self, CC1101_TEST2, 0x81);
  cc1101_spi_write_reg(self, CC1101_TEST1, 0x35);
  cc1101_spi_write_reg(self, CC1101_TEST0, 0x09);
  cc1101_spi_write_reg(self, CC1101_PKTCTRL1, 0x04);
  cc1101_spi_write_reg(self, CC1101_ADDR, 0x00);
  cc1101_spi_write_reg(self, CC1101_PKTLEN, 0x00);
}

void cc1101_set_tx_0(cc1101_t* self) {
  cc1101_spi_strobe(self, CC1101_SIDLE);
  cc1101_spi_strobe(self, CC1101_STX);
  self->trxstate_ = 1;
}

void cc1101_set_rx_0(cc1101_t* self) {
  cc1101_spi_strobe(self, CC1101_SIDLE);
  cc1101_spi_strobe(self, CC1101_SRX);
  self->trxstate_ = 2;
}

void cc1101_set_tx_1(cc1101_t* self, float mhz) {
  cc1101_spi_strobe(self, CC1101_SIDLE);
  cc1101_set_mhz(self, mhz);
  cc1101_spi_strobe(self, CC1101_STX);
  self->trxstate_ = 1;
}

void cc1101_set_rx_1(cc1101_t* self, float mhz) {
  cc1101_spi_strobe(self, CC1101_SIDLE);
  cc1101_set_mhz(self, mhz);
  cc1101_spi_strobe(self, CC1101_SRX);
  self->trxstate_ = 2;
}

int cc1101_get_rssi(cc1101_t* self) {
  int rssi = cc1101_spi_read_status(self, CC1101_RSSI);
  return (rssi >= 128) ? ((rssi - 256) / 2 - 74) : (rssi / 2 - 74);
}

uint8_t cc1101_get_lqi(cc1101_t* self) {
  return cc1101_spi_read_status(self, CC1101_LQI);
}

void cc1101_set_sres(cc1101_t* self) {
  cc1101_spi_strobe(self, CC1101_SRES);
  self->trxstate_ = 0;
}

void cc1101_set_sidle(cc1101_t* self) {
  cc1101_spi_strobe(self, CC1101_SIDLE);
  self->trxstate_ = 0;
}

void cc1101_go_sleep(cc1101_t* self) {
  self->trxstate_ = 0;
  cc1101_spi_strobe(self, CC1101_SIDLE);
  cc1101_spi_strobe(self, CC1101_SPWD);
}

void cc1101_send_data_1(cc1101_t* self, char* txchar) {
  cc1101_send_data_2_bytes(self, ((uint8_t*)(txchar)),
                           ((uint8_t)(DIV_MIN(strlen(txchar), 61))));
}

void cc1101_send_data_2_bytes(cc1101_t* self, uint8_t* txBuffer, uint8_t size) {
  size = DIV_MIN(size, 61);
  cc1101_spi_write_reg(self, CC1101_TXFIFO, size);
  cc1101_spi_write_burst_reg(self, CC1101_TXFIFO, txBuffer, size);
  cc1101_spi_strobe(self, CC1101_SIDLE);
  cc1101_spi_strobe(self, CC1101_STX);
  if (self->gdo0_ != 0xFF) {
    uint32_t start = sys_monotonic_ms();
    while (!gpio_get_level(((gpio_num_t)(self->gdo0_))) &&
           sys_monotonic_ms() - start < 1000)
      sys_sleep_ms(1);
    start = sys_monotonic_ms();
    while (gpio_get_level(((gpio_num_t)(self->gdo0_))) &&
           sys_monotonic_ms() - start < 1000)
      sys_sleep_ms(1);
  }
  cc1101_spi_strobe(self, CC1101_SFTX);
  self->trxstate_ = 1;
}

void cc1101_send_data_2_text(cc1101_t* self, char* txchar, int t) {
  cc1101_send_data_3(self, ((uint8_t*)(txchar)),
                     ((uint8_t)(DIV_MIN(strlen(txchar), 61))), t);
}

void cc1101_send_data_3(cc1101_t* self, uint8_t* txBuffer, uint8_t size,
                        int t) {
  size = DIV_MIN(size, 61);
  cc1101_spi_write_reg(self, CC1101_TXFIFO, size);
  cc1101_spi_write_burst_reg(self, CC1101_TXFIFO, txBuffer, size);
  cc1101_spi_strobe(self, CC1101_SIDLE);
  cc1101_spi_strobe(self, CC1101_STX);
  sys_sleep_ms(t);
  cc1101_spi_strobe(self, CC1101_SFTX);
  self->trxstate_ = 1;
}

bool cc1101_check_crc(cc1101_t* self) {
  const uint8_t lqi = cc1101_spi_read_status(self, CC1101_LQI);
  if (lqi & 0x80) return true;
  cc1101_spi_strobe(self, CC1101_SFRX);
  cc1101_spi_strobe(self, CC1101_SRX);
  return false;
}

bool cc1101_check_rx_fifo(cc1101_t* self, int t) {
  if (self->trxstate_ != 2) cc1101_set_rx_0(self);
  if (cc1101_spi_read_status(self, CC1101_RXBYTES) & BYTES_IN_RXFIFO) {
    sys_sleep_ms(t);
    return true;
  }
  return false;
}

uint8_t cc1101_check_receive_flag(cc1101_t* self) {
  if (self->trxstate_ != 2) cc1101_set_rx_0(self);
  if (self->gdo0_ != 0xFF && gpio_get_level(((gpio_num_t)(self->gdo0_)))) {
    while (gpio_get_level(((gpio_num_t)(self->gdo0_)))) sys_sleep_ms(1);
    return 1;
  }
  return (cc1101_spi_read_status(self, CC1101_RXBYTES) & BYTES_IN_RXFIFO) ? 1
                                                                          : 0;
}

uint8_t cc1101_receive_data(cc1101_t* self, uint8_t* rxBuffer) {
  if (!(cc1101_spi_read_status(self, CC1101_RXBYTES) & BYTES_IN_RXFIFO)) {
    cc1101_spi_strobe(self, CC1101_SFRX);
    cc1101_spi_strobe(self, CC1101_SRX);
    return 0;
  }
  uint8_t size = cc1101_spi_read_reg(self, CC1101_RXFIFO);
  if (size > 64) size = 64;
  cc1101_spi_read_burst_reg(self, CC1101_RXFIFO, rxBuffer, size);
  uint8_t status[2];
  cc1101_spi_read_burst_reg(self, CC1101_RXFIFO, status, 2);
  cc1101_spi_strobe(self, CC1101_SFRX);
  cc1101_spi_strobe(self, CC1101_SRX);
  return size;
}

void cc1101_set_clb(cc1101_t* self, uint8_t b, uint8_t s, uint8_t e) {
  uint8_t* clb = NULL;
  if (b == 1)
    clb = self->clb1_;
  else if (b == 2)
    clb = self->clb2_;
  else if (b == 3)
    clb = self->clb3_;
  else if (b == 4)
    clb = self->clb4_;
  if (clb) {
    clb[0] = s;
    clb[1] = e;
  }
}

bool cc1101_get_cc1101(cc1101_t* self) {
  cc1101_set_spi_defaults(self);
  uint8_t version = cc1101_spi_read_status(self, CC1101_VERSION);
  return version != 0x00 && version != 0xFF;
}

uint8_t cc1101_get_mode(cc1101_t* self) { return self->trxstate_; }

void cc1101_set_sync_word(cc1101_t* self, uint8_t sh, uint8_t sl) {
  cc1101_spi_write_reg(self, CC1101_SYNC1, sh);
  cc1101_spi_write_reg(self, CC1101_SYNC0, sl);
}

void cc1101_set_addr(cc1101_t* self, uint8_t v) {
  cc1101_spi_write_reg(self, CC1101_ADDR, v);
}

void cc1101_split_pktctrl1(cc1101_t* self) {
  int calc = cc1101_spi_read_status(self, CC1101_PKTCTRL1);
  self->pc1PQT_ = calc & 0xE0;
  self->pc1CRC_AF_ = calc & 0x08;
  self->pc1APP_ST_ = calc & 0x04;
  self->pc1ADRCHK_ = calc & 0x03;
}

void cc1101_split_pktctrl0(cc1101_t* self) {
  int calc = cc1101_spi_read_status(self, CC1101_PKTCTRL0);
  self->pc0WDATA_ = calc & 0x40;
  self->pc0PktForm_ = calc & 0x30;
  self->pc0CRC_EN_ = calc & 0x04;
  self->pc0LenConf_ = calc & 0x03;
}

void cc1101_split_mdmcfg1(cc1101_t* self) {
  int calc = cc1101_spi_read_status(self, CC1101_MDMCFG1);
  self->m1FEC_ = calc & 0x80;
  self->m1PRE_ = calc & 0x70;
  self->m1CHSP_ = calc & 0x03;
}

void cc1101_split_mdmcfg2(cc1101_t* self) {
  int calc = cc1101_spi_read_status(self, CC1101_MDMCFG2);
  self->m2DCOFF_ = calc & 0x80;
  self->m2MODFM_ = calc & 0x70;
  self->m2MANCH_ = calc & 0x08;
  self->m2SYNCM_ = calc & 0x07;
}

void cc1101_split_mdmcfg4(cc1101_t* self) {
  int calc = cc1101_spi_read_status(self, CC1101_MDMCFG4);
  self->m4RxBw_ = calc & 0xF0;
  self->m4DaRa_ = calc & 0x0F;
}

void cc1101_set_white_data(cc1101_t* self, bool v) {
  cc1101_split_pktctrl0(self);
  self->pc0WDATA_ = v ? 0x40 : 0x00;
  cc1101_spi_write_reg(self, CC1101_PKTCTRL0,
                       self->pc0WDATA_ + self->pc0PktForm_ + self->pc0CRC_EN_ +
                           self->pc0LenConf_);
}

void cc1101_set_pkt_format(cc1101_t* self, uint8_t v) {
  cc1101_split_pktctrl0(self);
  self->pc0PktForm_ = (v & 0x03) << 4;
  cc1101_spi_write_reg(self, CC1101_PKTCTRL0,
                       self->pc0WDATA_ + self->pc0PktForm_ + self->pc0CRC_EN_ +
                           self->pc0LenConf_);
}

void cc1101_set_crc(cc1101_t* self, bool v) {
  cc1101_split_pktctrl0(self);
  self->pc0CRC_EN_ = v ? 0x04 : 0x00;
  cc1101_spi_write_reg(self, CC1101_PKTCTRL0,
                       self->pc0WDATA_ + self->pc0PktForm_ + self->pc0CRC_EN_ +
                           self->pc0LenConf_);
}

void cc1101_set_length_config(cc1101_t* self, uint8_t v) {
  cc1101_split_pktctrl0(self);
  self->pc0LenConf_ = v & 0x03;
  cc1101_spi_write_reg(self, CC1101_PKTCTRL0,
                       self->pc0WDATA_ + self->pc0PktForm_ + self->pc0CRC_EN_ +
                           self->pc0LenConf_);
}

void cc1101_set_packet_length(cc1101_t* self, uint8_t v) {
  cc1101_spi_write_reg(self, CC1101_PKTLEN, v);
}

void cc1101_set_dc_filter_off(cc1101_t* self, bool v) {
  cc1101_split_mdmcfg2(self);
  self->m2DCOFF_ = v ? 0x80 : 0;
  cc1101_spi_write_reg(
      self, CC1101_MDMCFG2,
      self->m2DCOFF_ + self->m2MODFM_ + self->m2MANCH_ + self->m2SYNCM_);
}

void cc1101_set_manchester(cc1101_t* self, bool v) {
  cc1101_split_mdmcfg2(self);
  self->m2MANCH_ = v ? 0x08 : 0;
  cc1101_spi_write_reg(
      self, CC1101_MDMCFG2,
      self->m2DCOFF_ + self->m2MODFM_ + self->m2MANCH_ + self->m2SYNCM_);
}

void cc1101_set_sync_mode(cc1101_t* self, uint8_t v) {
  cc1101_split_mdmcfg2(self);
  self->m2SYNCM_ = v & 0x07;
  cc1101_spi_write_reg(
      self, CC1101_MDMCFG2,
      self->m2DCOFF_ + self->m2MODFM_ + self->m2MANCH_ + self->m2SYNCM_);
}

void cc1101_set_fec(cc1101_t* self, bool v) {
  cc1101_split_mdmcfg1(self);
  self->m1FEC_ = v ? 0x80 : 0;
  cc1101_spi_write_reg(self, CC1101_MDMCFG1,
                       self->m1FEC_ + self->m1PRE_ + self->m1CHSP_);
}

void cc1101_set_pre(cc1101_t* self, uint8_t v) {
  cc1101_split_mdmcfg1(self);
  self->m1PRE_ = (v & 0x07) << 4;
  cc1101_spi_write_reg(self, CC1101_MDMCFG1,
                       self->m1FEC_ + self->m1PRE_ + self->m1CHSP_);
}

void cc1101_set_pqt(cc1101_t* self, uint8_t v) {
  cc1101_split_pktctrl1(self);
  self->pc1PQT_ = (v & 0x07) << 5;
  cc1101_spi_write_reg(
      self, CC1101_PKTCTRL1,
      self->pc1PQT_ + self->pc1CRC_AF_ + self->pc1APP_ST_ + self->pc1ADRCHK_);
}

void cc1101_set_crc_af(cc1101_t* self, bool v) {
  cc1101_split_pktctrl1(self);
  self->pc1CRC_AF_ = v ? 0x08 : 0;
  cc1101_spi_write_reg(
      self, CC1101_PKTCTRL1,
      self->pc1PQT_ + self->pc1CRC_AF_ + self->pc1APP_ST_ + self->pc1ADRCHK_);
}

void cc1101_set_append_status(cc1101_t* self, bool v) {
  cc1101_split_pktctrl1(self);
  self->pc1APP_ST_ = v ? 0x04 : 0;
  cc1101_spi_write_reg(
      self, CC1101_PKTCTRL1,
      self->pc1PQT_ + self->pc1CRC_AF_ + self->pc1APP_ST_ + self->pc1ADRCHK_);
}

void cc1101_set_adr_chk(cc1101_t* self, uint8_t v) {
  cc1101_split_pktctrl1(self);
  self->pc1ADRCHK_ = v & 0x03;
  cc1101_spi_write_reg(
      self, CC1101_PKTCTRL1,
      self->pc1PQT_ + self->pc1CRC_AF_ + self->pc1APP_ST_ + self->pc1ADRCHK_);
}

bool cc1101_ready(const cc1101_t* self) {
  return spi_device_ready(&self->spi_);
}
