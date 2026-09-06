#pragma once
#include "driver/uart.h"
#include "text_format.h"
void div_console_open(uint32_t baud);
void div_console_close(void);
int div_console_pending(void);
int div_console_read_byte(void);
size_t div_console_write(text_t value);
size_t div_console_line(text_t value);
