#pragma once
#include "gnss_parser.h"
#include "uart_port.h"

typedef struct {
  gnss_parser_t* parser;
  uart_port_state_t port;
} gnss_receiver_t;
/* Receive-only UART preserves the console TX line on the official V1 board. */
bool gnss_receiver_open(gnss_receiver_t* receiver, gnss_parser_t* parser,
                        int uart, uint32_t baud, int rx);
void gnss_receiver_close(gnss_receiver_t* receiver);
void gnss_receiver_poll(gnss_receiver_t* receiver);
