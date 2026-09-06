#pragma once
#include "c_util.h"
#include "driver/uart.h"
typedef struct {
  uart_port_t uart;
  bool installed;
} uart_port_state_t;
bool uart_port_open(uart_port_state_t* port, uint32_t baud, int rx, int tx);
void uart_port_close(uart_port_state_t* port);
int uart_port_pending(uart_port_state_t* port);
int uart_port_read_byte(uart_port_state_t* port);
size_t uart_port_read_until(uart_port_state_t* port, char delim, char* buffer,
                            size_t capacity);
