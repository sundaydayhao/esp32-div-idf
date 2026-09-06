#include "c_vector.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ir_recv.h"
#include "ir_send.h"
#include "pulse_io.h"
#include "string.h"
#include "system_utils.h"

enum { kRawTickUs = 50 };
enum { kMaxRaw = 512 };
enum { kGapUs = 15000 };

pulse_receiver_t receiver;
static bool ir_read_pulses(c_vector_t* pulses) {
  if (!c_vector_resize(pulses, 1024, sizeof(pulse_t))) return false;
  pulses->size = pulse_receiver_read(&receiver, pulses->data, pulses->capacity);
  return pulses->size != 0;
}

static uint16_t ticks(uint32_t us) {
  return ((uint16_t)(DIV_MIN(0xFFFF,
                             DIV_MAX(1, (us + kRawTickUs / 2) / kRawTickUs))));
}

static bool match(uint32_t measured, uint32_t desired, uint8_t tolerance) {
  const uint32_t low = desired - (desired * tolerance / 100);
  const uint32_t high = desired + (desired * tolerance / 100);
  return measured >= low && measured <= high;
}

static uint32_t usecAt(const IrFrame* r, uint16_t idx) {
  return ((uint32_t)(r->rawbuf[idx])) * kRawTickUs;
}

static bool decodePulseDistance(IrFrame* r, uint16_t offset, uint16_t nbits,
                                uint32_t oneMark, uint32_t oneSpace,
                                uint32_t zeroMark, uint32_t zeroSpace,
                                bool msbFirst, uint64_t* value) {
  (*value) = 0;
  if (r->rawlen < offset + nbits * 2) return false;
  for (uint16_t i = 0; i < nbits; ++i) {
    const uint32_t mark = usecAt(r, offset + i * 2);
    const uint32_t space = usecAt(r, offset + i * 2 + 1);
    bool bit;
    if (match(mark, oneMark, 30) && match(space, oneSpace, 30))
      bit = true;
    else if (match(mark, zeroMark, 30) && match(space, zeroSpace, 30))
      bit = false;
    else
      return false;
    if (msbFirst)
      (*value) = ((*value) << 1) | (bit ? 1 : 0);
    else if (bit)
      (*value) |= (1ULL << i);
  }
  return true;
}

static bool decodeKnown(IrFrame* r) {
  uint64_t value = 0;
  if (r->rawlen >= 68 && match(usecAt(r, 1), 9000, 30) &&
      match(usecAt(r, 2), 4500, 30) &&
      decodePulseDistance(r, 3, 32, 560, 1690, 560, 560, false, &(value))) {
    r->decode_type = NEC;
    r->value = value;
    r->bits = 32;
    return true;
  }
  if (r->rawlen >= 68 && match(usecAt(r, 1), 4500, 30) &&
      match(usecAt(r, 2), 4500, 30) &&
      decodePulseDistance(r, 3, 32, 560, 1600, 560, 560, true, &(value))) {
    r->decode_type = SAMSUNG36;
    r->value = value;
    r->bits = 32;
    return true;
  }
  if (r->rawlen >= 26 && match(usecAt(r, 1), 2400, 30) &&
      match(usecAt(r, 2), 600, 30) &&
      decodePulseDistance(r, 3, DIV_MIN(20, (r->rawlen - 3) / 2), 1200, 600,
                          600, 600, false, &(value))) {
    r->decode_type = SONY;
    r->value = value;
    r->bits = DIV_MIN(20, (r->rawlen - 3) / 2);
    return true;
  }
  if (r->rawlen >= 68 && match(usecAt(r, 1), 8000, 30) &&
      match(usecAt(r, 2), 4000, 30) &&
      decodePulseDistance(r, 3, 32, 560, 1690, 560, 560, false, &(value))) {
    r->decode_type = LG;
    r->value = value;
    r->bits = 32;
    return true;
  }
  r->decode_type = UNKNOWN;
  r->value = 0;
  r->bits = 0;
  return false;
}

void ir_receiver_construct(ir_receiver_t* self, uint16_t recvpin) {
  *self = (ir_receiver_t){.recvpin_ = recvpin};
}
void ir_receiver_enable_ir_in(ir_receiver_t* self) {
  self->enabled_ =
      pulse_receiver_start(&receiver, self->recvpin_, 15000) == ESP_OK;
}
void ir_receiver_disable_ir_in(ir_receiver_t* self) {
  pulse_receiver_stop(&receiver);
  self->enabled_ = false;
}
void ir_receiver_resume(ir_receiver_t* self) {}
bool ir_receiver_decode(ir_receiver_t* self, IrFrame* results) {
  if (!self->enabled_ || !results) return false;
  c_vector_t pulses __attribute__((cleanup(c_vector_free))) = {0};
  if (!ir_read_pulses(&pulses)) return false;
  *results = (IrFrame){0};
  results->rawbuf[0] = ticks(kGapUs);
  results->rawlen = 1;
  bool started = false;
  {
    for (size_t c_range_index = 0; c_range_index < (pulses).size;
         ++c_range_index) {
      __auto_type pulse = &(((pulse_t*)(pulses).data)[c_range_index]);
      {
        if (!started && (*pulse).high) continue;
        started = true;
        if ((*pulse).us >= kGapUs) break;
        if (results->rawlen == kMaxRaw) break;
        results->rawbuf[results->rawlen++] = ticks((*pulse).us);
      }
    }
  }
  if (results->rawlen < 6) return false;
  decodeKnown(results);
  return true;
}

void ir_transmitter_construct(ir_transmitter_t* self, uint16_t sendpin) {
  *self = (ir_transmitter_t){.sendpin_ = sendpin, .khz_ = 38};
}

void ir_transmitter_begin(ir_transmitter_t* self) {
  sys_configure_gpio(self->sendpin_, GPIO_MODE_OUTPUT);
  gpio_set_level(((gpio_num_t)(self->sendpin_)), 0);
}

void ir_transmitter_mark(ir_transmitter_t* self, uint32_t usec) {
  if (usec) C_VECTOR_PUSH(&self->pending_, ((pulse_t){usec, true}));
}
void ir_transmitter_space(ir_transmitter_t* self, uint32_t usec) {
  if (usec) {
    C_VECTOR_PUSH(&self->pending_, ((pulse_t){usec, false}));
    return;
  }
  if (!(self->pending_.size == 0)) {
    const esp_err_t err = pulse_send(self->sendpin_, self->pending_.data,
                                     self->pending_.size, self->khz_ * 1000);
    if (err != ESP_OK)
      ESP_LOGE("ir", "transmit failed: %s", esp_err_to_name(err));
    c_vector_free(&self->pending_);
  }
}
void ir_transmitter_send_raw(ir_transmitter_t* self, const uint16_t* buf,
                             uint16_t len, uint16_t hz) {
  if (!buf || !len) return;
  c_vector_free(&self->pending_);
  self->khz_ = hz ? hz : 38;
  for (uint16_t i = 0; i < len; ++i)
    C_VECTOR_PUSH(&self->pending_, ((pulse_t){buf[i], !(i & 1)}));
  ir_transmitter_space(self, 0);
}

void ir_transmitter_send_pulse_distance(ir_transmitter_t* self, uint64_t data,
                                        uint16_t nbits, uint32_t headerMark,
                                        uint32_t headerSpace, uint32_t oneMark,
                                        uint32_t oneSpace, uint32_t zeroMark,
                                        uint32_t zeroSpace, uint32_t footerMark,
                                        bool msbFirst) {
  ir_transmitter_mark(self, headerMark);
  ir_transmitter_space(self, headerSpace);
  for (uint16_t i = 0; i < nbits; ++i) {
    const bool bit =
        msbFirst ? ((data >> (nbits - 1 - i)) & 1ULL) : ((data >> i) & 1ULL);
    ir_transmitter_mark(self, bit ? oneMark : zeroMark);
    ir_transmitter_space(self, bit ? oneSpace : zeroSpace);
  }
  if (footerMark) ir_transmitter_mark(self, footerMark);
  ir_transmitter_space(self, 0);
}

void ir_transmitter_send_manchester(ir_transmitter_t* self, uint64_t data,
                                    uint16_t nbits, uint32_t halfPeriod,
                                    bool startHigh) {
  bool level = startHigh;
  for (uint16_t i = 0; i < nbits; ++i) {
    const bool bit = (data >> (nbits - 1 - i)) & 1ULL;
    const bool first = bit ? !level : level;
    if (first)
      ir_transmitter_mark(self, halfPeriod);
    else
      ir_transmitter_space(self, halfPeriod);
    if (!first)
      ir_transmitter_mark(self, halfPeriod);
    else
      ir_transmitter_space(self, halfPeriod);
  }
  ir_transmitter_space(self, 0);
}

bool ir_transmitter_send(ir_transmitter_t* self, decode_type_t type,
                         uint64_t data, uint16_t nbits, uint16_t repeat) {
  ir_transmitter_begin(self);
  c_vector_free(&self->pending_);
  uint16_t reps = repeat ? repeat : 1;
  switch (type) {
    case NEC:
    case NEC_LIKE:
      while (reps--) {
        ir_transmitter_send_pulse_distance(self, data, nbits ? nbits : 32, 9000,
                                           4500, 560, 1690, 560, 560, 560,
                                           false);
        ir_transmitter_space(self, 40000);
      }
      ir_transmitter_space(self, 0);
      return true;
    case SONY:
    case SONY_38K:
      while (reps--) {
        ir_transmitter_send_pulse_distance(self, data, nbits ? nbits : 12, 2400,
                                           600, 1200, 600, 600, 600, 0, false);
        ir_transmitter_space(self, 45000);
      }
      ir_transmitter_space(self, 0);
      return true;
    case SAMSUNG36:
      while (reps--) {
        ir_transmitter_send_pulse_distance(self, data, nbits ? nbits : 32, 4500,
                                           4500, 560, 1600, 560, 560, 560,
                                           true);
        ir_transmitter_space(self, 45000);
      }
      ir_transmitter_space(self, 0);
      return true;
    case LG:
    case LG2:
      while (reps--) {
        ir_transmitter_send_pulse_distance(self, data, nbits ? nbits : 32, 8000,
                                           4000, 560, 1690, 560, 560, 560,
                                           false);
        ir_transmitter_space(self, 45000);
      }
      ir_transmitter_space(self, 0);
      return true;
    case JVC:
      while (reps--) {
        ir_transmitter_send_pulse_distance(self, data, nbits ? nbits : 16, 8000,
                                           4000, 526, 1574, 526, 526, 526,
                                           false);
        ir_transmitter_space(self, 45000);
      }
      ir_transmitter_space(self, 0);
      return true;
    case DENON:
      while (reps--) {
        ir_transmitter_send_pulse_distance(self, data, nbits ? nbits : 15, 260,
                                           780, 260, 1820, 260, 780, 0, true);
        ir_transmitter_space(self, 45000);
      }
      ir_transmitter_space(self, 0);
      return true;
    case PANASONIC:
    case PIONEER:
      while (reps--) {
        ir_transmitter_send_pulse_distance(self, data, nbits ? nbits : 48, 3502,
                                           1750, 502, 1244, 502, 400, 502,
                                           true);
        ir_transmitter_space(self, 45000);
      }
      ir_transmitter_space(self, 0);
      return true;
    case RC5:
      while (reps--) {
        ir_transmitter_send_manchester(self, data, nbits ? nbits : 14, 889,
                                       true);
        ir_transmitter_space(self, 90000);
      }
      ir_transmitter_space(self, 0);
      return true;
    case RC6:
      while (reps--) {
        ir_transmitter_mark(self, 2666);
        ir_transmitter_space(self, 889);
        ir_transmitter_send_manchester(self, data, nbits ? nbits : 20, 444,
                                       true);
        ir_transmitter_space(self, 90000);
      }
      ir_transmitter_space(self, 0);
      return true;
    case DISH:
      while (reps--) {
        ir_transmitter_send_pulse_distance(self, data, nbits ? nbits : 16, 400,
                                           6100, 400, 1700, 400, 2800, 0, true);
        ir_transmitter_space(self, 45000);
      }
      ir_transmitter_space(self, 0);
      return true;
    case GICABLE:
    case EPSON:
      while (reps--) {
        ir_transmitter_send_pulse_distance(self, data, nbits ? nbits : 32, 9000,
                                           4500, 560, 1680, 560, 560, 560,
                                           true);
        ir_transmitter_space(self, 45000);
      }
      ir_transmitter_space(self, 0);
      return true;
    default:
      return false;
  }
}
