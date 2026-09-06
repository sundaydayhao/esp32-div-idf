#include "ook_radio.h"

#include "esp_log.h"
typedef struct {
  uint16_t unit;
  uint8_t sh, sl, zh, zl, oh, ol;
  bool inverted;
} protocol_t;
static const protocol_t protocols[] = {
    {350, 1, 31, 1, 3, 3, 1, false},   {650, 1, 10, 1, 2, 2, 1, false},
    {100, 30, 71, 4, 11, 9, 6, false}, {380, 1, 6, 1, 3, 3, 1, false},
    {500, 6, 14, 1, 2, 2, 1, false},   {450, 23, 1, 1, 2, 2, 1, true},
    {150, 2, 62, 1, 6, 6, 1, false},   {200, 3, 130, 7, 16, 3, 16, false},
    {200, 130, 7, 16, 7, 16, 3, true}, {365, 18, 1, 3, 1, 1, 3, true},
    {270, 36, 1, 1, 2, 2, 1, true},    {320, 36, 1, 1, 2, 2, 1, true}};
static bool close_time(uint32_t a, uint32_t b, int tolerance) {
  return llabs((int64_t)a - b) * 100 <= (int64_t)b * tolerance;
}
bool ook_decode(const pulse_t* pulses, size_t count, int tolerance,
                ook_code_t* result) {
  if (!pulses || !result) return false;
  for (size_t p = 0; p < DIV_ARRAY_SIZE(protocols); ++p) {
    const protocol_t* proto = &protocols[p];
    uint32_t code = 0;
    unsigned bits = 0;
    for (size_t i = 0; i + 1 < count; ++i) {
      if (pulses[i].high == proto->inverted ||
          pulses[i + 1].high != proto->inverted)
        continue;
      uint32_t hi = pulses[i].us, lo = pulses[i + 1].us;
      bool zero = close_time(hi, proto->unit * proto->zh, tolerance) &&
                  close_time(lo, proto->unit * proto->zl, tolerance);
      bool one = close_time(hi, proto->unit * proto->oh, tolerance) &&
                 close_time(lo, proto->unit * proto->ol, tolerance);
      if (zero || one) {
        if (bits < 32) {
          code = (code << 1) | one;
          ++bits;
        } else {
          bits = 0;
          code = 0;
        }
        ++i;
        continue;
      }
      bool sync =
          close_time(hi, proto->unit * proto->sh, tolerance) &&
          (close_time(lo, proto->unit * proto->sl, tolerance) || lo >= 14000);
      if (bits >= 8 && sync) {
        *result = (ook_code_t){code, bits, p + 1};
        return true;
      }
      code = 0;
      bits = 0;
    }
  }
  return false;
}
void ook_radio_enable_receive(ook_radio_t* r, int pin) {
  ook_radio_disable_receive(r);
  r->pulses = calloc(1024, sizeof(*r->pulses));
  if (!r->pulses) return;
  if (pulse_receiver_start(&r->rx, pin, 15000) != ESP_OK) {
    free(r->pulses);
    r->pulses = NULL;
  }
}
void ook_radio_disable_receive(ook_radio_t* r) {
  pulse_receiver_stop(&r->rx);
  free(r->pulses);
  r->pulses = NULL;
}
bool ook_radio_available(ook_radio_t* r) {
  if (r->available) return true;
  if (!r->pulses) return false;
  size_t n = pulse_receiver_read(&r->rx, r->pulses, 1024);
  ook_code_t code;
  if (ook_decode(r->pulses, n, r->tolerance, &code)) {
    r->value = code.value;
    r->bits = code.bits;
    r->received_protocol = code.protocol;
    r->available = true;
  }
  return r->available;
}
void ook_radio_send(ook_radio_t* r, uint32_t value, unsigned bits) {
  esp_err_t err =
      ook_send(r->tx_pin, value, bits, r->protocol, r->repeat, r->pulse_length);
  if (err != ESP_OK)
    ESP_LOGE("ook", "transmit failed: %s", esp_err_to_name(err));
}
esp_err_t ook_send(int gpio, uint32_t value, unsigned bits, unsigned protocol,
                   unsigned repeat, unsigned pulse_us) {
  if (!bits || bits > 32 || !protocol || protocol > DIV_ARRAY_SIZE(protocols))
    return ESP_ERR_INVALID_ARG;
  repeat = DIV_CLAMP(repeat, 1u, 20u);
  const protocol_t* p = &protocols[protocol - 1];
  size_t count = repeat * (bits + 1) * 2, pos = 0;
  pulse_t* pulses = calloc(count, sizeof(*pulses));
  if (!pulses) return ESP_ERR_NO_MEM;
  uint32_t unit = pulse_us ? pulse_us : p->unit;
  for (unsigned r = 0; r < repeat; ++r) {
    for (unsigned i = bits; i > 0; --i) {
      bool bit = (value >> (i - 1)) & 1;
      pulses[pos++] = (pulse_t){unit * (bit ? p->oh : p->zh), !p->inverted};
      pulses[pos++] = (pulse_t){unit * (bit ? p->ol : p->zl), p->inverted};
    }
    pulses[pos++] = (pulse_t){unit * p->sh, !p->inverted};
    pulses[pos++] = (pulse_t){unit * p->sl, p->inverted};
  }
  esp_err_t err = pulse_send(gpio, pulses, count, 0);
  free(pulses);
  return err;
}
