#pragma once
#include "pulse_io.h"
typedef struct {
  uint32_t value;
  unsigned bits, protocol;
} ook_code_t;
bool ook_decode(const pulse_t* pulses, size_t count, int tolerance,
                ook_code_t* code);
esp_err_t ook_send(int gpio, uint32_t value, unsigned bits, unsigned protocol,
                   unsigned repeat, unsigned pulse_us);
typedef struct {
  pulse_receiver_t rx;
  pulse_t* pulses;
  bool available;
  int tolerance, tx_pin, protocol, pulse_length;
  unsigned bits, received_protocol;
  uint32_t value;
  uint8_t repeat;
} ook_radio_t;
#define OOK_RADIO_INITIALIZER \
  {.tolerance = 40, .tx_pin = -1, .protocol = 1, .repeat = 1}
void ook_radio_enable_receive(ook_radio_t* radio, int pin);
void ook_radio_disable_receive(ook_radio_t* radio);
bool ook_radio_available(ook_radio_t* radio);
void ook_radio_send(ook_radio_t* radio, uint32_t value, unsigned bits);
static inline void ook_radio_reset_available(ook_radio_t* r) {
  r->available = false;
}
static inline void ook_radio_set_receive_tolerance(ook_radio_t* r, int value) {
  r->tolerance = value;
}
static inline void ook_radio_disable_transmit(ook_radio_t* r) {
  r->tx_pin = -1;
}
static inline void ook_radio_enable_transmit(ook_radio_t* r, int pin) {
  r->tx_pin = pin;
}
static inline void ook_radio_set_repeat_transmit(ook_radio_t* r, uint8_t n) {
  r->repeat = n;
}
static inline void ook_radio_set_protocol(ook_radio_t* r, int protocol) {
  r->protocol = protocol;
  r->pulse_length = 0;
}
static inline void ook_radio_send_protocol(ook_radio_t* r, uint32_t value,
                                           unsigned bits, unsigned protocol) {
  ook_radio_set_protocol(r, protocol);
  ook_radio_send(r, value, bits);
}
static inline void ook_radio_set_pulse_length(ook_radio_t* r, int us) {
  r->pulse_length = us > 0 && us <= 10000 ? us : 0;
}
static inline uint32_t ook_radio_get_received_value(const ook_radio_t* r) {
  return r->value;
}
static inline unsigned ook_radio_get_received_bitlength(const ook_radio_t* r) {
  return r->bits;
}
static inline unsigned ook_radio_get_received_protocol(const ook_radio_t* r) {
  return r->received_protocol;
}
