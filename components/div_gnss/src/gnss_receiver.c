#include "gnss_receiver.h"

#include "system_utils.h"

bool gnss_receiver_open(gnss_receiver_t* receiver, gnss_parser_t* parser,
                        int uart, uint32_t baud, int rx) {
  if (!receiver || !parser) return false;
  gnss_receiver_close(receiver);
  receiver->parser = parser;
  receiver->port.uart = (uart_port_t)uart;
  return uart_port_open(&receiver->port, baud, rx, UART_PIN_NO_CHANGE);
}
void gnss_receiver_close(gnss_receiver_t* receiver) {
  if (receiver) uart_port_close(&receiver->port);
}
void gnss_receiver_poll(gnss_receiver_t* receiver) {
  if (!receiver || !receiver->parser) return;
  while (uart_port_pending(&receiver->port)) {
    int byte = uart_port_read_byte(&receiver->port);
    if (byte < 0) break;
    char c = (char)byte;
    gnss_parser_feed(receiver->parser, &c, 1, sys_monotonic_ms());
  }
}
