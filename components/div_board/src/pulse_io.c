#include "pulse_io.h"

#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_heap_caps.h"
#include "soc/soc_caps.h"
#include "spi_device.h"
#define RECEIVE_SYMBOLS 512

esp_err_t pulse_send(int gpio, const pulse_t* pulses, size_t count,
                     uint32_t carrier_hz) {
  if (!GPIO_IS_VALID_OUTPUT_GPIO(gpio) || !pulses || !count || count > 8192)
    return ESP_ERR_INVALID_ARG;
  size_t halves = 0;
  for (size_t i = 0; i < count; ++i) {
    halves += (pulses[i].us + 32766ULL) / 32767;
    if (halves > 32768) return ESP_ERR_INVALID_SIZE;
  }
  if (!halves) return ESP_ERR_INVALID_SIZE;
  size_t words = (halves + 1) / 2;
  rmt_symbol_word_t* symbols = calloc(words, sizeof(*symbols));
  if (!symbols) return ESP_ERR_NO_MEM;
  size_t pos = 0;
  for (size_t i = 0; i < count; ++i) {
    uint32_t remaining = pulses[i].us;
    while (remaining) {
      uint32_t duration = DIV_MIN(remaining, 32767u);
      remaining -= duration;
      if (pos & 1) {
        symbols[pos / 2].duration1 = duration;
        symbols[pos / 2].level1 = pulses[i].high;
      } else {
        symbols[pos / 2].duration0 = duration;
        symbols[pos / 2].level0 = pulses[i].high;
      }
      ++pos;
    }
  }
  spi_device_t lease = {0};
  rmt_channel_handle_t channel = NULL;
  rmt_encoder_handle_t encoder = NULL;
  bool enabled = false;
  esp_err_t err = ESP_OK;
  if (gpio == SD_CS) {
    err = spi_device_init(&lease, DIV_SD_HOST, SD_SCLK, SD_MISO, SD_MOSI, SD_CS,
                          4000000, false);
    if (err != ESP_OK) goto done;
    if (!spi_device_select(&lease)) {
      err = ESP_ERR_INVALID_STATE;
      goto done;
    }
  }
  rmt_tx_channel_config_t cfg = {
      .gpio_num = gpio,
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .resolution_hz = 1000000,
      .mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL,
      .trans_queue_depth = 1};
  err = rmt_new_tx_channel(&cfg, &channel);
  if (err != ESP_OK) goto done;
  rmt_copy_encoder_config_t encoding = {};
  err = rmt_new_copy_encoder(&encoding, &encoder);
  if (err != ESP_OK) goto done;
  if (carrier_hz) {
    rmt_carrier_config_t carrier = {.frequency_hz = carrier_hz,
                                    .duty_cycle = 0.33f};
    err = rmt_apply_carrier(channel, &carrier);
    if (err != ESP_OK) goto done;
  }
  err = rmt_enable(channel);
  if (err != ESP_OK) goto done;
  enabled = true;
  rmt_transmit_config_t tx = {0};
  err = rmt_transmit(channel, encoder, symbols, words * sizeof(*symbols), &tx);
  if (err == ESP_OK) err = rmt_tx_wait_all_done(channel, -1);
done:
  if (enabled) rmt_disable(channel);
  if (encoder) rmt_del_encoder(encoder);
  if (channel) {
    rmt_del_channel(channel);
    gpio_reset_pin(gpio);
    gpio_set_level(gpio, gpio == SD_CS);
    gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
  }
  spi_device_close(&lease);
  free(symbols);
  return err;
}
static bool received(rmt_channel_handle_t channel,
                     const rmt_rx_done_event_data_t* event, void* ctx) {
  (void)channel;
  pulse_receiver_t* receiver = ctx;
  atomic_store_explicit(&receiver->count, event->num_symbols,
                        memory_order_release);
  return false;
}
static esp_err_t arm(pulse_receiver_t* receiver) {
  atomic_store(&receiver->count, 0);
  rmt_receive_config_t cfg = {.signal_range_min_ns = 1000,
                              .signal_range_max_ns = receiver->idle_us * 1000};
  return rmt_receive(receiver->channel, receiver->symbols,
                     RECEIVE_SYMBOLS * sizeof(*receiver->symbols), &cfg);
}
esp_err_t pulse_receiver_start(pulse_receiver_t* receiver, int gpio,
                               uint32_t idle_us) {
  if (!receiver || !GPIO_IS_VALID_GPIO(gpio)) return ESP_ERR_INVALID_ARG;
  pulse_receiver_stop(receiver);
  receiver->symbols =
      heap_caps_calloc(RECEIVE_SYMBOLS, sizeof(*receiver->symbols),
                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!receiver->symbols) return ESP_ERR_NO_MEM;
  receiver->idle_us = DIV_CLAMP(idle_us, 100u, 30000u);
  rmt_rx_channel_config_t cfg = {
      .gpio_num = gpio,
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .resolution_hz = 1000000,
      .mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL};
  esp_err_t err = rmt_new_rx_channel(&cfg, &receiver->channel);
  rmt_rx_event_callbacks_t cb = {.on_recv_done = received};
  if (err == ESP_OK)
    err = rmt_rx_register_event_callbacks(receiver->channel, &cb, receiver);
  if (err == ESP_OK) err = rmt_enable(receiver->channel);
  if (err == ESP_OK) err = arm(receiver);
  if (err != ESP_OK) pulse_receiver_stop(receiver);
  return err;
}
void pulse_receiver_stop(pulse_receiver_t* receiver) {
  if (!receiver) return;
  if (receiver->channel) {
    rmt_disable(receiver->channel);
    rmt_del_channel(receiver->channel);
    receiver->channel = NULL;
  }
  heap_caps_free(receiver->symbols);
  receiver->symbols = NULL;
  atomic_store(&receiver->count, 0);
}
size_t pulse_receiver_read(pulse_receiver_t* receiver, pulse_t* output,
                           size_t capacity) {
  if (!receiver || !receiver->channel || !output) return 0;
  size_t count = atomic_load_explicit(&receiver->count, memory_order_acquire),
         used = 0;
  if (!count) return 0;
  for (size_t i = 0; i < DIV_MIN(count, (size_t)RECEIVE_SYMBOLS); ++i) {
    rmt_symbol_word_t word = receiver->symbols[i];
    if (word.duration0 && used < capacity)
      output[used++] = (pulse_t){word.duration0, word.level0};
    if (word.duration1 && used < capacity)
      output[used++] = (pulse_t){word.duration1, word.level1};
  }
  arm(receiver);
  return used;
}
