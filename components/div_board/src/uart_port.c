#include "uart_port.h"
bool uart_port_open(uart_port_state_t* port, uint32_t baud, int rx, int tx) {
  uart_port_close(port);
  uart_config_t cfg = {.baud_rate = (int)baud,
                       .data_bits = UART_DATA_8_BITS,
                       .parity = UART_PARITY_DISABLE,
                       .stop_bits = UART_STOP_BITS_1,
                       .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                       .source_clk = UART_SCLK_DEFAULT};
  if (uart_param_config(port->uart, &cfg) != ESP_OK ||
      uart_set_pin(port->uart, tx, rx, UART_PIN_NO_CHANGE,
                   UART_PIN_NO_CHANGE) != ESP_OK)
    return false;
  port->installed =
      uart_driver_install(port->uart, 4096, 0, 0, NULL, 0) == ESP_OK;
  return port->installed;
}
void uart_port_close(uart_port_state_t* port) {
  if (port->installed) {
    uart_driver_delete(port->uart);
    port->installed = false;
  }
}
int uart_port_pending(uart_port_state_t* port) {
  size_t n = 0;
  return port->installed && uart_get_buffered_data_len(port->uart, &n) == ESP_OK
             ? (int)n
             : 0;
}
int uart_port_read_byte(uart_port_state_t* port) {
  uint8_t ch;
  return port->installed && uart_read_bytes(port->uart, &ch, 1, 0) == 1 ? ch
                                                                        : -1;
}
size_t uart_port_read_until(uart_port_state_t* port, char delim, char* buffer,
                            size_t capacity) {
  if (!port->installed || !buffer) return 0;
  size_t n = 0;
  while (n < capacity) {
    uint8_t ch;
    if (uart_read_bytes(port->uart, &ch, 1, pdMS_TO_TICKS(20)) != 1 ||
        ch == (uint8_t)delim)
      break;
    buffer[n++] = ch;
  }
  return n;
}
