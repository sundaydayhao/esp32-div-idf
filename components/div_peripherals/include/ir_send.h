#pragma once
#include "c_vector.h"
#include "ir_protocol.h"
#include "pulse_io.h"
typedef struct {
  c_vector_t pending_;
  uint16_t sendpin_, khz_;
} ir_transmitter_t;
void ir_transmitter_construct(ir_transmitter_t* self, uint16_t sendpin);
void ir_transmitter_begin(ir_transmitter_t* self);
void ir_transmitter_send_raw(ir_transmitter_t* self, const uint16_t* buf,
                             uint16_t len, uint16_t hz);
bool ir_transmitter_send(ir_transmitter_t* self, decode_type_t type,
                         uint64_t data, uint16_t nbits, uint16_t repeat);
void ir_transmitter_mark(ir_transmitter_t* self, uint32_t usec);
void ir_transmitter_space(ir_transmitter_t* self, uint32_t usec);
void ir_transmitter_send_pulse_distance(ir_transmitter_t* self, uint64_t data,
                                        uint16_t nbits, uint32_t headerMark,
                                        uint32_t headerSpace, uint32_t oneMark,
                                        uint32_t oneSpace, uint32_t zeroMark,
                                        uint32_t zeroSpace, uint32_t footerMark,
                                        bool msbFirst);
void ir_transmitter_send_manchester(ir_transmitter_t* self, uint64_t data,
                                    uint16_t nbits, uint32_t halfPeriod,
                                    bool startHigh);
