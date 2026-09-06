#pragma once
#include <stdatomic.h>

#include "c_util.h"
#include "driver/rmt_rx.h"
typedef struct {
  uint32_t us;
  bool high;
} pulse_t;
typedef struct {
  rmt_channel_handle_t channel;
  rmt_symbol_word_t* symbols;
  atomic_size_t count;
  uint32_t idle_us;
} pulse_receiver_t;
esp_err_t pulse_send(int gpio, const pulse_t* pulses, size_t count,
                     uint32_t carrier_hz);
esp_err_t pulse_receiver_start(pulse_receiver_t* receiver, int gpio,
                               uint32_t idle_us);
void pulse_receiver_stop(pulse_receiver_t* receiver);
size_t pulse_receiver_read(pulse_receiver_t* receiver, pulse_t* output,
                           size_t capacity);
