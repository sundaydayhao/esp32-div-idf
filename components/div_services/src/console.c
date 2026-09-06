#include "console.h"
static bool owns_console;
void div_console_open(uint32_t baud) {
  uart_set_baudrate(UART_NUM_0, baud);
  if (!owns_console && !uart_is_driver_installed(UART_NUM_0))
    owns_console =
        uart_driver_install(UART_NUM_0, 2048, 0, 0, NULL, 0) == ESP_OK;
}
void div_console_close(void) {
  if (owns_console) {
    uart_driver_delete(UART_NUM_0);
    owns_console = false;
  }
  uart_set_baudrate(UART_NUM_0, 115200);
}
int div_console_pending(void) {
  size_t n = 0;
  return uart_is_driver_installed(UART_NUM_0) &&
                 uart_get_buffered_data_len(UART_NUM_0, &n) == ESP_OK
             ? (int)n
             : 0;
}
int div_console_read_byte(void) {
  uint8_t ch;
  return uart_is_driver_installed(UART_NUM_0) &&
                 uart_read_bytes(UART_NUM_0, &ch, 1, 0) == 1
             ? ch
             : -1;
}
size_t div_console_write(text_t value) {
  return fwrite(value.data, 1, text_length(value), stdout);
}
size_t div_console_line(text_t value) {
  return div_console_write(value) + div_console_write(text_string("\n"));
}
