#pragma once
#include "c_vector.h"
#include "ir_protocol.h"
#include "pulse_io.h"
typedef struct {
  uint16_t recvpin_;
  bool enabled_;
} ir_receiver_t;
void ir_receiver_construct(ir_receiver_t* self, uint16_t recvpin);
void ir_receiver_enable_ir_in(ir_receiver_t* self);
void ir_receiver_disable_ir_in(ir_receiver_t* self);
bool ir_receiver_decode(ir_receiver_t* self, IrFrame* results);
void ir_receiver_resume(ir_receiver_t* self);
