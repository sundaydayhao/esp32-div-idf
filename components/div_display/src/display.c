#include "display.h"
static int32_t display_interpolate(int32_t xa, int32_t ya, int32_t xb,
                                   int32_t yb, int32_t y) {
  return yb == ya ? xa : xa + (xb - xa) * (y - ya) / (yb - ya);
}
static void display_rounded_rect(display_t* canvas, int x, int y, int w, int h,
                                 int radius, uint16_t color, bool filled) {
  if (w <= 0 || h <= 0) return;
  int r = DIV_CLAMP(radius, 0, DIV_MIN(w, h) / 2);
  if (!r) {
    if (filled)
      display_fill_rect(canvas, x, y, w, h, color);
    else
      display_draw_rect(canvas, x, y, w, h, color);
    return;
  }
  int previous = r;
  for (int row = 0; row < h; ++row) {
    int edge = DIV_MIN(row, h - 1 - row);
    int inset =
        edge < r
            ? r - ((int)(sqrt(((double)(r * r - (r - edge) * (r - edge))))))
            : 0;
    if (filled || row == 0 || row == h - 1)
      display_fill_rect(canvas, x + inset, y + row, w - 2 * inset, 1, color);
    else {
      int thickness = DIV_MAX(1, abs(inset - previous));
      display_fill_rect(canvas, x + inset, y + row, thickness, 1, color);
      display_fill_rect(canvas, x + w - inset - thickness, y + row, thickness,
                        1, color);
    }
    previous = inset;
  }
}
static void sprite_rounded_rect(sprite_t* canvas, int x, int y, int w, int h,
                                int radius, uint16_t color, bool filled) {
  if (w <= 0 || h <= 0) return;
  int r = DIV_CLAMP(radius, 0, DIV_MIN(w, h) / 2);
  if (!r) {
    if (filled)
      sprite_fill_rect(canvas, x, y, w, h, color);
    else
      sprite_draw_rect(canvas, x, y, w, h, color);
    return;
  }
  int previous = r;
  for (int row = 0; row < h; ++row) {
    int edge = DIV_MIN(row, h - 1 - row);
    int inset =
        edge < r
            ? r - ((int)(sqrt(((double)(r * r - (r - edge) * (r - edge))))))
            : 0;
    if (filled || row == 0 || row == h - 1)
      sprite_fill_rect(canvas, x + inset, y + row, w - 2 * inset, 1, color);
    else {
      int thickness = DIV_MAX(1, abs(inset - previous));
      sprite_fill_rect(canvas, x + inset, y + row, thickness, 1, color);
      sprite_fill_rect(canvas, x + w - inset - thickness, y + row, thickness, 1,
                       color);
    }
    previous = inset;
  }
}
#include "board_pins.h"
#include "display.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/semphr.h"
#include "spi_bus.h"

static const char* kTag = "Display";
esp_lcd_panel_io_handle_t g_io = NULL;
esp_lcd_panel_handle_t g_panel = NULL;
StaticSemaphore_t g_doneStorage;
SemaphoreHandle_t g_done = NULL;

uint16_t beColor(uint16_t color) { return __builtin_bswap16(color); }

bool colorDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*,
               void*) {
  BaseType_t wake = pdFALSE;
  xSemaphoreGiveFromISR(g_done, &wake);
  return wake == pdTRUE;
}

void sendBytes(const uint8_t* data, size_t len) {
  if (!g_io || !data || !len) return;
  // The caller may use a stack buffer. Wait until DMA has consumed it.
  ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(g_io, -1, data, len));
  xSemaphoreTake(g_done, portMAX_DELAY);
}

uint8_t glyphRow(char c, int row) {
  if (c < 32 || c > 126) {
    c = '?';
  }
  if (c == ' ') {
    return 0;
  }
  if (c >= '0' && c <= '9') {
    static const uint8_t digits[10][7] = {
        {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},
        {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
        {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
        {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E},
        {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
        {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
        {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},
        {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
        {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
        {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
    };
    return digits[c - '0'][row];
  }
  if (c >= 'a' && c <= 'z') {
    static const uint8_t letters[26][7] = {
        {0, 0, 14, 1, 15, 17, 15},    {16, 16, 30, 17, 17, 17, 30},
        {0, 0, 14, 16, 16, 17, 14},   {1, 1, 15, 17, 17, 17, 15},
        {0, 0, 14, 17, 31, 16, 14},   {6, 8, 8, 28, 8, 8, 8},
        {0, 15, 17, 17, 15, 1, 14},   {16, 16, 30, 17, 17, 17, 17},
        {4, 0, 12, 4, 4, 4, 14},      {2, 0, 6, 2, 2, 18, 12},
        {16, 16, 18, 20, 24, 20, 18}, {12, 4, 4, 4, 4, 4, 14},
        {0, 0, 26, 21, 21, 21, 21},   {0, 0, 30, 17, 17, 17, 17},
        {0, 0, 14, 17, 17, 17, 14},   {0, 0, 30, 17, 30, 16, 16},
        {0, 0, 15, 17, 15, 1, 1},     {0, 0, 22, 25, 16, 16, 16},
        {0, 0, 15, 16, 14, 1, 30},    {8, 8, 28, 8, 8, 9, 6},
        {0, 0, 17, 17, 17, 19, 13},   {0, 0, 17, 17, 17, 10, 4},
        {0, 0, 17, 17, 21, 21, 10},   {0, 0, 17, 10, 4, 10, 17},
        {0, 0, 17, 17, 15, 1, 14},    {0, 0, 31, 2, 4, 8, 31}};
    return letters[c - 'a'][row];
  }
  switch (c) {
    case 'A': {
      static const uint8_t r[7] = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
      return r[row];
    }
    case 'B': {
      static const uint8_t r[7] = {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
      return r[row];
    }
    case 'C': {
      static const uint8_t r[7] = {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E};
      return r[row];
    }
    case 'D': {
      static const uint8_t r[7] = {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
      return r[row];
    }
    case 'E': {
      static const uint8_t r[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
      return r[row];
    }
    case 'F': {
      static const uint8_t r[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
      return r[row];
    }
    case 'G': {
      static const uint8_t r[7] = {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F};
      return r[row];
    }
    case 'H': {
      static const uint8_t r[7] = {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
      return r[row];
    }
    case 'I': {
      static const uint8_t r[7] = {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E};
      return r[row];
    }
    case 'J': {
      static const uint8_t r[7] = {0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C};
      return r[row];
    }
    case 'K': {
      static const uint8_t r[7] = {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
      return r[row];
    }
    case 'L': {
      static const uint8_t r[7] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
      return r[row];
    }
    case 'M': {
      static const uint8_t r[7] = {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
      return r[row];
    }
    case 'N': {
      static const uint8_t r[7] = {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
      return r[row];
    }
    case 'O': {
      static const uint8_t r[7] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
      return r[row];
    }
    case 'P': {
      static const uint8_t r[7] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
      return r[row];
    }
    case 'Q': {
      static const uint8_t r[7] = {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D};
      return r[row];
    }
    case 'R': {
      static const uint8_t r[7] = {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
      return r[row];
    }
    case 'S': {
      static const uint8_t r[7] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
      return r[row];
    }
    case 'T': {
      static const uint8_t r[7] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
      return r[row];
    }
    case 'U': {
      static const uint8_t r[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
      return r[row];
    }
    case 'V': {
      static const uint8_t r[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04};
      return r[row];
    }
    case 'W': {
      static const uint8_t r[7] = {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A};
      return r[row];
    }
    case 'X': {
      static const uint8_t r[7] = {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11};
      return r[row];
    }
    case 'Y': {
      static const uint8_t r[7] = {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};
      return r[row];
    }
    case 'Z': {
      static const uint8_t r[7] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F};
      return r[row];
    }
    case '.': {
      static const uint8_t r[7] = {0, 0, 0, 0, 0, 0x0C, 0x0C};
      return r[row];
    }
    case ',': {
      static const uint8_t r[7] = {0, 0, 0, 0, 0, 0x0C, 0x08};
      return r[row];
    }
    case ':': {
      static const uint8_t r[7] = {0, 0x0C, 0x0C, 0, 0x0C, 0x0C, 0};
      return r[row];
    }
    case ';': {
      static const uint8_t r[7] = {0, 0x0C, 0x0C, 0, 0x0C, 0x08, 0};
      return r[row];
    }
    case '-': {
      static const uint8_t r[7] = {0, 0, 0, 0x1F, 0, 0, 0};
      return r[row];
    }
    case '_': {
      static const uint8_t r[7] = {0, 0, 0, 0, 0, 0, 0x1F};
      return r[row];
    }
    case '/': {
      static const uint8_t r[7] = {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10};
      return r[row];
    }
    case '\\': {
      static const uint8_t r[7] = {0x10, 0x08, 0x08, 0x04, 0x02, 0x02, 0x01};
      return r[row];
    }
    case '+': {
      static const uint8_t r[7] = {0, 0x04, 0x04, 0x1F, 0x04, 0x04, 0};
      return r[row];
    }
    case '*': {
      static const uint8_t r[7] = {0, 0x15, 0x0E, 0x1F, 0x0E, 0x15, 0};
      return r[row];
    }
    case '!': {
      static const uint8_t r[7] = {0x04, 0x04, 0x04, 0x04, 0x04, 0, 0x04};
      return r[row];
    }
    case '?': {
      static const uint8_t r[7] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0, 0x04};
      return r[row];
    }
    case '%': {
      static const uint8_t r[7] = {0x19, 0x1A, 0x02, 0x04, 0x08, 0x0B, 0x13};
      return r[row];
    }
    case '#': {
      static const uint8_t r[7] = {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A};
      return r[row];
    }
    case '[': {
      static const uint8_t r[7] = {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E};
      return r[row];
    }
    case ']': {
      static const uint8_t r[7] = {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E};
      return r[row];
    }
    case '(': {
      static const uint8_t r[7] = {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02};
      return r[row];
    }
    case ')': {
      static const uint8_t r[7] = {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08};
      return r[row];
    }
    case '<': {
      static const uint8_t r[7] = {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02};
      return r[row];
    }
    case '>': {
      static const uint8_t r[7] = {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08};
      return r[row];
    }
    default: {
      static const uint8_t r[7] = {0x1F, 0x11, 0x15, 0x11, 0x15, 0x11, 0x1F};
      return r[row];
    }
  }
}

void display_construct(display_t* self) {
  *self = (display_t)DISPLAY_INITIALIZER;
  self->width_ = TFT_WIDTH;
  self->height_ = TFT_HEIGHT;
  self->rotation_ = 0;
  self->textFg_ = TFT_WHITE;
  self->textBg_ = TFT_BLACK;
  self->textSize_ = 1;
  self->textFont_ = 1;
  self->textDatum_ = TL_DATUM;
  self->textWrap_ = true;
  self->cursorX_ = 0;
  self->cursorY_ = 0;
}

void display_init(display_t* self) {
  if (g_panel) return;
  spi_bus_config_t bus = {0};
  bus.mosi_io_num = TFT_MOSI;
  bus.miso_io_num = TFT_MISO;
  bus.sclk_io_num = TFT_SCLK;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = 40960;
  ESP_ERROR_CHECK(
      div_spi_bus_init(DIV_TFT_HOST, &(bus), SPI_DMA_CH_AUTO, "display"));
  g_done = xSemaphoreCreateBinaryStatic(&g_doneStorage);
  esp_lcd_panel_io_spi_config_t io = {0};
  io.cs_gpio_num = (gpio_num_t)TFT_CS;
  io.dc_gpio_num = (gpio_num_t)TFT_DC;
  io.pclk_hz = SPI_FREQUENCY;
  io.trans_queue_depth = 1;
  io.on_color_trans_done = colorDone;
  io.lcd_cmd_bits = 8;
  io.lcd_param_bits = 8;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
      (esp_lcd_spi_bus_handle_t)DIV_TFT_HOST, &io, &g_io));
  esp_lcd_panel_dev_config_t cfg = {0};
  cfg.reset_gpio_num = (gpio_num_t)TFT_RST;
  cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
  cfg.bits_per_pixel = 16;
  ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(g_io, &cfg, &g_panel));
  display_init_panel(self);
  display_set_rotation(self, TFT_ROTATION);
  display_fill_screen(self, TFT_BLACK);
}

void display_init_panel(display_t* self) {
  ESP_ERROR_CHECK(esp_lcd_panel_reset(g_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(g_panel));
#if defined(TFT_INVERT_COLOR)
  ESP_ERROR_CHECK(esp_lcd_panel_invert_color(g_panel, true));
#else
  ESP_ERROR_CHECK(esp_lcd_panel_invert_color(g_panel, false));
#endif
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(g_panel, true));
}

void display_send_command(display_t* self, uint8_t cmd, const uint8_t* data,
                          size_t len) {
  if (g_io) ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(g_io, cmd, data, len));
}

void display_set_rotation(display_t* self, uint8_t rotation) {
  self->rotation_ = rotation & 3;
  self->width_ = (self->rotation_ & 1) ? TFT_HEIGHT : TFT_WIDTH;
  self->height_ = (self->rotation_ & 1) ? TFT_WIDTH : TFT_HEIGHT;
  uint8_t madctl = 0x40 | TFT_RGB_ORDER;
  switch (self->rotation_) {
    case 1:
      madctl = 0x20 | TFT_RGB_ORDER;
      break;
    case 2:
      madctl = 0x80 | TFT_RGB_ORDER;
      break;
    case 3:
      madctl = 0xE0 | TFT_RGB_ORDER;
      break;
    default:
      madctl = 0x40 | TFT_RGB_ORDER;
      break;
  }
  display_send_command(self, 0x36, &madctl, 1);
}

void display_set_addr_window(display_t* self, int32_t x, int32_t y, int32_t w,
                             int32_t h) {
  const uint16_t x0 = x + TFT_COL_OFFSET;
  const uint16_t y0 = y + TFT_ROW_OFFSET;
  const uint16_t x1 = x + w - 1 + TFT_COL_OFFSET;
  const uint16_t y1 = y + h - 1 + TFT_ROW_OFFSET;
  uint8_t col[4] = {((uint8_t)(x0 >> 8)), ((uint8_t)(x0)), ((uint8_t)(x1 >> 8)),
                    ((uint8_t)(x1))};
  uint8_t row[4] = {((uint8_t)(y0 >> 8)), ((uint8_t)(y0)), ((uint8_t)(y1 >> 8)),
                    ((uint8_t)(y1))};
  display_send_command(self, 0x2A, col, sizeof(col));
  display_send_command(self, 0x2B, row, sizeof(row));
  display_send_command(self, 0x2C, NULL, 0);
}

void display_fill_screen(display_t* self, uint16_t color) {
  display_fill_rect(self, 0, 0, self->width_, self->height_, color);
}

void display_write_fill_rect(display_t* self, int32_t x, int32_t y, int32_t w,
                             int32_t h, uint16_t color) {
  display_fill_rect(self, x, y, w, h, color);
}

void display_fill_rect(display_t* self, int32_t x, int32_t y, int32_t w,
                       int32_t h, uint16_t color) {
  if (w <= 0 || h <= 0 || x >= self->width_ || y >= self->height_) return;
  if (x == 0 && w == 240 && self->width_ > 240) {
    w = self->width_;
  }
  if (y >= 0 && h == 320 && self->height_ > 320) {
    h = self->height_ - y;
  }
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > self->width_) w = self->width_ - x;
  if (y + h > self->height_) h = self->height_ - y;
  if (w <= 0 || h <= 0) return;
  display_set_addr_window(self, x, y, w, h);
  const uint16_t c = beColor(color);
  uint16_t line[128];
  DIV_FILL((line), (line + DIV_ARRAY_SIZE(line)), c);
  int32_t remaining = w * h;
  while (remaining > 0) {
    const int32_t n = DIV_MIN(remaining, ((int32_t)(DIV_ARRAY_SIZE(line))));
    sendBytes(((const uint8_t*)(line)), n * 2);
    remaining -= n;
  }
}

void display_draw_rect(display_t* self, int32_t x, int32_t y, int32_t w,
                       int32_t h, uint16_t color) {
  display_draw_fast_h_line(self, x, y, w, color);
  display_draw_fast_h_line(self, x, y + h - 1, w, color);
  display_draw_fast_v_line(self, x, y, h, color);
  display_draw_fast_v_line(self, x + w - 1, y, h, color);
}

void display_draw_round_rect(display_t* self, int32_t x, int32_t y, int32_t w,
                             int32_t h, int32_t radius, uint16_t color) {
  display_rounded_rect(self, x, y, w, h, radius, color, false);
}

void display_fill_round_rect(display_t* self, int32_t x, int32_t y, int32_t w,
                             int32_t h, int32_t radius, uint16_t color) {
  display_rounded_rect(self, x, y, w, h, radius, color, true);
}

void display_draw_fast_h_line(display_t* self, int32_t x, int32_t y, int32_t w,
                              uint16_t color) {
  display_fill_rect(self, x, y, w, 1, color);
}

void display_draw_fast_v_line(display_t* self, int32_t x, int32_t y, int32_t h,
                              uint16_t color) {
  display_fill_rect(self, x, y, 1, h, color);
}

void display_write_pixel(display_t* self, int32_t x, int32_t y,
                         uint16_t color) {
  display_draw_pixel(self, x, y, color);
}

void display_draw_pixel(display_t* self, int32_t x, int32_t y, uint16_t color) {
  if (x < 0 || y < 0 || x >= self->width_ || y >= self->height_) return;
  const uint16_t c = beColor(color);
  display_set_addr_window(self, x, y, 1, 1);
  sendBytes(((const uint8_t*)(&c)), 2);
}

void display_draw_line(display_t* self, int32_t x0, int32_t y0, int32_t x1,
                       int32_t y1, uint16_t color) {
  if (y0 == y1 && x0 == 0 && x1 == 240 && self->width_ > 240) {
    x1 = self->width_ - 1;
  }
  if (x0 == x1 && y0 == 0 && y1 == 320 && self->height_ > 320) {
    y1 = self->height_ - 1;
  }
  const int32_t dx = abs(x1 - x0);
  const int32_t sx = x0 < x1 ? 1 : -1;
  const int32_t dy = -abs(y1 - y0);
  const int32_t sy = y0 < y1 ? 1 : -1;
  int32_t err = dx + dy;
  while (true) {
    display_draw_pixel(self, x0, y0, color);
    if (x0 == x1 && y0 == y1) break;
    const int32_t e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

void display_draw_circle(display_t* self, int32_t x0, int32_t y0, int32_t r,
                         uint16_t color) {
  int32_t f = 1 - r, ddF_x = 1, ddF_y = -2 * r, x = 0, y = r;
  display_draw_pixel(self, x0, y0 + r, color);
  display_draw_pixel(self, x0, y0 - r, color);
  display_draw_pixel(self, x0 + r, y0, color);
  display_draw_pixel(self, x0 - r, y0, color);
  while (x < y) {
    if (f >= 0) {
      y--;
      ddF_y += 2;
      f += ddF_y;
    }
    x++;
    ddF_x += 2;
    f += ddF_x;
    display_draw_pixel(self, x0 + x, y0 + y, color);
    display_draw_pixel(self, x0 - x, y0 + y, color);
    display_draw_pixel(self, x0 + x, y0 - y, color);
    display_draw_pixel(self, x0 - x, y0 - y, color);
    display_draw_pixel(self, x0 + y, y0 + x, color);
    display_draw_pixel(self, x0 - y, y0 + x, color);
    display_draw_pixel(self, x0 + y, y0 - x, color);
    display_draw_pixel(self, x0 - y, y0 - x, color);
  }
}

void display_fill_circle(display_t* self, int32_t x0, int32_t y0, int32_t r,
                         uint16_t color) {
  for (int32_t y = -r; y <= r; ++y) {
    const int32_t dx = ((int32_t)(sqrt(((float)(r * r - y * y)))));
    display_draw_fast_h_line(self, x0 - dx, y0 + y, dx * 2 + 1, color);
  }
}

void display_fill_triangle(display_t* self, int32_t x0, int32_t y0, int32_t x1,
                           int32_t y1, int32_t x2, int32_t y2, uint16_t color) {
  if (y0 > y1) {
    DIV_SWAP(y0, y1);
    DIV_SWAP(x0, x1);
  }
  if (y1 > y2) {
    DIV_SWAP(y2, y1);
    DIV_SWAP(x2, x1);
  }
  if (y0 > y1) {
    DIV_SWAP(y0, y1);
    DIV_SWAP(x0, x1);
  }

  for (int32_t y = y0; y <= y2; ++y) {
    const bool lower = y > y1;
    const int32_t xa = display_interpolate(x0, y0, x2, y2, y);
    const int32_t xb = lower ? display_interpolate(x1, y1, x2, y2, y)
                             : display_interpolate(x0, y0, x1, y1, y);
    display_draw_fast_h_line(self, DIV_MIN(xa, xb), y, abs(xa - xb) + 1, color);
  }
}

void display_draw_bitmap(display_t* self, int32_t x, int32_t y,
                         const uint8_t* bitmap, int32_t w, int32_t h,
                         uint16_t color) {
  if (!bitmap) return;
  const int32_t byteWidth = (w + 7) / 8;
  for (int32_t yy = 0; yy < h; ++yy) {
    for (int32_t xx = 0; xx < w; ++xx) {
      if (bitmap[yy * byteWidth + xx / 8] & (0x80 >> (xx & 7))) {
        display_draw_pixel(self, x + xx, y + yy, color);
      }
    }
  }
}

void display_set_cursor(display_t* self, int32_t x, int32_t y) {
  self->cursorX_ = x;
  self->cursorY_ = y;
}

void display_set_text_color_1(display_t* self, uint16_t fg) {
  self->textFg_ = fg;
  self->textBg_ = fg;
}

void display_set_text_color_2(display_t* self, uint16_t fg, uint16_t bg) {
  self->textFg_ = fg;
  self->textBg_ = bg;
}

void display_set_text_size(display_t* self, uint8_t size) {
  self->textSize_ = DIV_MAX(1, size);
}

void display_set_text_font(display_t* self, uint8_t font) {
  self->textFont_ = font;
}

void display_set_text_datum(display_t* self, uint8_t datum) {
  self->textDatum_ = datum;
}

void display_set_text_wrap(display_t* self, bool wrap) {
  self->textWrap_ = wrap;
}

int16_t display_char_width(const display_t* self, uint8_t font) {
  const uint8_t scale = DIV_MAX(self->textSize_, font > 1 ? 2 : 1);
  return 6 * scale;
}

int16_t display_char_height(const display_t* self, uint8_t font) {
  const uint8_t scale = DIV_MAX(self->textSize_, font > 1 ? 2 : 1);
  return 8 * scale;
}

int16_t display_text_width(const display_t* self, const char* text,
                           uint8_t font) {
  return text ? ((int16_t)(strlen(text) *
                           display_char_width(self,
                                              font ? font : self->textFont_)))
              : 0;
}

void display_draw_glyph(display_t* self, char c, int32_t x, int32_t y,
                        uint8_t scale, uint16_t fg, uint16_t bg) {
  for (int row = 0; row < 7; ++row) {
    const uint8_t bits = glyphRow(c, row);
    for (int col = 0; col < 5; ++col) {
      const bool on = bits & (1 << (4 - col));
      const uint16_t pix = on ? fg : bg;
      if (pix != bg || fg != bg) {
        display_fill_rect(self, x + col * scale, y + row * scale, scale, scale,
                          pix);
      }
    }
  }
  if (fg != bg) {
    display_fill_rect(self, x + 5 * scale, y, scale, 8 * scale, bg);
  }
}

int16_t display_draw_string(display_t* self, const char* text, int32_t x,
                            int32_t y, uint8_t font) {
  if (!text) return 0;
  const uint8_t useFont = font ? font : self->textFont_;
  const uint8_t scale = DIV_MAX(self->textSize_, useFont > 1 ? 2 : 1);
  const int16_t w = display_text_width(self, text, useFont);
  const int16_t h = display_char_height(self, useFont);
  if (self->textDatum_ == TC_DATUM || self->textDatum_ == MC_DATUM ||
      self->textDatum_ == BC_DATUM)
    x -= w / 2;
  if (self->textDatum_ == TR_DATUM || self->textDatum_ == MR_DATUM ||
      self->textDatum_ == BR_DATUM)
    x -= w;
  if (self->textDatum_ == ML_DATUM || self->textDatum_ == MC_DATUM ||
      self->textDatum_ == MR_DATUM)
    y -= h / 2;
  if (self->textDatum_ == BL_DATUM || self->textDatum_ == BC_DATUM ||
      self->textDatum_ == BR_DATUM)
    y -= h;
  int32_t cx = x;
  for (const char* p = text; *p; ++p) {
    if (*p == '\n') {
      cx = x;
      y += h;
      continue;
    }
    display_draw_glyph(self, *p, cx, y, scale, self->textFg_, self->textBg_);
    cx += 6 * scale;
  }
  return w;
}

int16_t display_draw_centre_string(display_t* self, const char* text, int32_t x,
                                   int32_t y, uint8_t font) {
  const uint8_t oldDatum = self->textDatum_;
  self->textDatum_ = TC_DATUM;
  const int16_t w = display_draw_string(self, text, x, y, font);
  self->textDatum_ = oldDatum;
  return w;
}

int16_t display_draw_number(display_t* self, long n, int32_t x, int32_t y,
                            uint8_t font) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%ld", n);
  return display_draw_string(self, buf, x, y, font);
}

int16_t display_draw_char(display_t* self, char c, int32_t x, int32_t y,
                          uint8_t font) {
  char s[2] = {c, 0};
  return display_draw_string(self, s, x, y, font);
}

size_t display_print(display_t* self, const char* s) {
  if (!s) return 0;
  size_t written = 0;
  for (const char* p = s; *p; ++p) {
    if (*p == '\n') {
      self->cursorX_ = 0;
      self->cursorY_ += display_char_height(self, self->textFont_);
    } else {
      display_draw_char(self, *p, self->cursorX_, self->cursorY_,
                        self->textFont_);
      self->cursorX_ += display_char_width(self, self->textFont_);
      if (self->textWrap_ &&
          self->cursorX_ + display_char_width(self, self->textFont_) >
              self->width_) {
        self->cursorX_ = 0;
        self->cursorY_ += display_char_height(self, self->textFont_);
      }
    }
    ++written;
  }
  return written;
}

size_t display_println(display_t* self) { return display_print(self, "\n"); }

size_t display_printf(display_t* self, const char* fmt, ...) {
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n <= 0) return 0;
  if (((size_t)(n)) < sizeof(buf)) return display_print(self, buf);
  char* dyn = malloc((size_t)n + 1);
  if (!dyn) return 0;
  va_start(ap, fmt);
  vsnprintf(dyn, (size_t)n + 1, fmt, ap);
  va_end(ap);
  size_t written = display_print(self, dyn);
  free(dyn);
  return written;
}

void display_writecommand(display_t* self, uint8_t cmd) {
  display_send_command(self, cmd, NULL, 0);
}

void display_writedata(display_t* self, uint8_t data) {
  if (g_io) ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(g_io, -1, &data, 1));
}

void display_push_pixels(display_t* self, int32_t x, int32_t y, int32_t w,
                         int32_t h, const uint16_t* pixels) {
  if (!pixels || w <= 0 || h <= 0) return;
  if (x < 0 || y < 0 || x + w > self->width_ || y + h > self->height_) {
    for (int32_t yy = 0; yy < h; ++yy) {
      for (int32_t xx = 0; xx < w; ++xx) {
        display_draw_pixel(self, x + xx, y + yy, pixels[yy * w + xx]);
      }
    }
    return;
  }
  display_set_addr_window(self, x, y, w, h);
  uint16_t line[320];
  if (w > 320) return;
  for (int32_t yy = 0; yy < h; ++yy) {
    for (int32_t xx = 0; xx < w; ++xx) line[xx] = beColor(pixels[yy * w + xx]);
    sendBytes(((const uint8_t*)(line)), w * 2);
  }
}

void sprite_construct(sprite_t* self, display_t* parent) {
  *self = (sprite_t)SPRITE_INITIALIZER;
  self->parent_ = parent;
}

void sprite_destroy(sprite_t* self) { sprite_delete_sprite(self); }

bool sprite_create_sprite(sprite_t* self, int16_t w, int16_t h) {
  if (w <= 0 || h <= 0) return false;
  const size_t count = ((size_t)(w)) * ((size_t)(h));
  if (count > (SIZE_MAX / sizeof(uint16_t))) {
    ESP_LOGW(kTag, "sprite size overflow: %dx%d", (int)w, (int)h);
    return false;
  }
  const size_t bytes = count * (self->colorDepth_ == 8 ? 1 : 2);
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (largest < bytes) {
    ESP_LOGW(kTag,
             "sprite alloc skipped: %dx%d needs %u bytes, largest block %u",
             (int)w, (int)h, (unsigned)bytes, (unsigned)largest);
    return false;
  }

  uint8_t* next = ((uint8_t*)(heap_caps_malloc(bytes, MALLOC_CAP_8BIT)));
  if (!next) {
    ESP_LOGW(kTag,
             "sprite alloc failed: %dx%d needs %u bytes, largest block %u",
             (int)w, (int)h, (unsigned)bytes, (unsigned)largest);
    return false;
  }

  sprite_delete_sprite(self);
  self->width_ = w;
  self->height_ = h;
  self->pixels_ = next;
  self->pixelCount_ = count;
  memset(self->pixels_, 0, bytes);
  return true;
}

void sprite_delete_sprite(sprite_t* self) {
  if (self->pixels_) {
    heap_caps_free(self->pixels_);
    self->pixels_ = NULL;
  }
  self->pixelCount_ = 0;
  self->width_ = self->height_ = 0;
}

void sprite_push_sprite(sprite_t* self, int32_t x, int32_t y) {
  if (!self->parent_ || !self->pixels_) return;
  if (self->colorDepth_ == 16) {
    display_push_pixels(self->parent_, x, y, self->width_, self->height_,
                        ((uint16_t*)(self->pixels_)));
    return;
  }
  uint16_t row[320];
  if (self->width_ > 320) return;
  for (int yy = 0; yy < self->height_; ++yy) {
    for (int xx = 0; xx < self->width_; ++xx) {
      uint8_t c = self->pixels_[yy * self->width_ + xx];
      uint16_t r = (c >> 5) * 31 / 7, g = ((c >> 2) & 7) * 63 / 7,
               b = (c & 3) * 31 / 3;
      row[xx] = (r << 11) | (g << 5) | b;
    }
    display_push_pixels(self->parent_, x, y + yy, self->width_, 1, row);
  }
}

void sprite_write_pixel(sprite_t* self, int32_t x, int32_t y, uint16_t color) {
  if (x < 0 || y < 0 || x >= self->width_ || y >= self->height_ ||
      !self->pixels_)
    return;
  size_t offset = ((size_t)(y)) * self->width_ + x;
  if (self->colorDepth_ == 8)
    self->pixels_[offset] =
        ((color >> 8) & 0xe0) | ((color >> 6) & 0x1c) | ((color >> 3) & 3);
  else
    ((uint16_t*)(self->pixels_))[offset] = color;
}

void sprite_write_fill_rect(sprite_t* self, int32_t x, int32_t y, int32_t w,
                            int32_t h, uint16_t color) {
  if (w <= 0 || h <= 0 || !self->pixels_) return;
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > self->width_) w = self->width_ - x;
  if (y + h > self->height_) h = self->height_ - y;
  if (w <= 0 || h <= 0) return;
  for (int32_t yy = 0; yy < h; ++yy) {
    const size_t offset = ((size_t)(y + yy)) * self->width_ + x;
    if (self->colorDepth_ == 8)
      DIV_FILL_N(self->pixels_ + offset, w,
                 ((uint8_t)(((color >> 8) & 0xe0) | ((color >> 6) & 0x1c) |
                            ((color >> 3) & 3))));
    else
      DIV_FILL_N(((uint16_t*)(self->pixels_)) + offset, w, color);
  }
}

void sprite_fill_screen(sprite_t* self, uint16_t color) {
  sprite_write_fill_rect(self, 0, 0, self->width_, self->height_, color);
}

void sprite_fill_rect(sprite_t* self, int32_t x, int32_t y, int32_t w,
                      int32_t h, uint16_t color) {
  sprite_write_fill_rect(self, x, y, w, h, color);
}

void sprite_draw_rect(sprite_t* self, int32_t x, int32_t y, int32_t w,
                      int32_t h, uint16_t color) {
  sprite_draw_line(self, x, y, x + w - 1, y, color);
  sprite_draw_line(self, x, y + h - 1, x + w - 1, y + h - 1, color);
  sprite_draw_line(self, x, y, x, y + h - 1, color);
  sprite_draw_line(self, x + w - 1, y, x + w - 1, y + h - 1, color);
}

void sprite_draw_round_rect(sprite_t* self, int32_t x, int32_t y, int32_t w,
                            int32_t h, int32_t radius, uint16_t color) {
  sprite_rounded_rect(self, x, y, w, h, radius, color, false);
}

void sprite_fill_round_rect(sprite_t* self, int32_t x, int32_t y, int32_t w,
                            int32_t h, int32_t radius, uint16_t color) {
  sprite_rounded_rect(self, x, y, w, h, radius, color, true);
}

void sprite_draw_bitmap(sprite_t* self, int32_t x, int32_t y,
                        const uint8_t* bitmap, int32_t w, int32_t h,
                        uint16_t color) {
  if (!bitmap || !self->pixels_) return;
  const int32_t byteWidth = (w + 7) / 8;
  for (int32_t yy = 0; yy < h; ++yy) {
    for (int32_t xx = 0; xx < w; ++xx) {
      if (bitmap[yy * byteWidth + xx / 8] & (0x80 >> (xx & 7))) {
        sprite_write_pixel(self, x + xx, y + yy, color);
      }
    }
  }
}

void sprite_draw_line(sprite_t* self, int32_t x0, int32_t y0, int32_t x1,
                      int32_t y1, uint16_t color) {
  const int32_t dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  const int32_t dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int32_t err = dx + dy;
  while (true) {
    sprite_write_pixel(self, x0, y0, color);
    if (x0 == x1 && y0 == y1) break;
    const int32_t e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

void sprite_draw_circle(sprite_t* self, int32_t x0, int32_t y0, int32_t r,
                        uint16_t color) {
  for (int32_t a = 0; a < 360; a += 5) {
    const float rad = a * 3.1415926f / 180.0f;
    sprite_write_pixel(self, x0 + ((int32_t)(cos(rad) * r)),
                       y0 + ((int32_t)(sin(rad) * r)), color);
  }
}

void sprite_fill_circle(sprite_t* self, int32_t x0, int32_t y0, int32_t r,
                        uint16_t color) {
  for (int32_t y = -r; y <= r; ++y) {
    const int32_t dx = ((int32_t)(sqrt(((float)(r * r - y * y)))));
    sprite_write_fill_rect(self, x0 - dx, y0 + y, dx * 2 + 1, 1, color);
  }
}

void sprite_fill_triangle(sprite_t* self, int32_t x0, int32_t y0, int32_t x1,
                          int32_t y1, int32_t x2, int32_t y2, uint16_t color) {
  if (y0 > y1) {
    DIV_SWAP(y0, y1);
    DIV_SWAP(x0, x1);
  }
  if (y1 > y2) {
    DIV_SWAP(y2, y1);
    DIV_SWAP(x2, x1);
  }
  if (y0 > y1) {
    DIV_SWAP(y0, y1);
    DIV_SWAP(x0, x1);
  }

  for (int32_t y = y0; y <= y2; ++y) {
    const bool lower = y > y1;
    const int32_t xa = display_interpolate(x0, y0, x2, y2, y);
    const int32_t xb = lower ? display_interpolate(x1, y1, x2, y2, y)
                             : display_interpolate(x0, y0, x1, y1, y);
    sprite_write_fill_rect(self, DIV_MIN(xa, xb), y, abs(xa - xb) + 1, 1,
                           color);
  }
}

int16_t sprite_text_width(const sprite_t* self, const char* text,
                          uint8_t font) {
  const uint8_t scale = DIV_MAX(self->textSize_, font > 1 ? 2 : 1);
  return text ? strlen(text) * 6 * scale : 0;
}

void sprite_draw_glyph(sprite_t* self, char c, int32_t x, int32_t y,
                       uint8_t scale, uint16_t fg, uint16_t bg) {
  for (int row = 0; row < 7; ++row) {
    const uint8_t bits = glyphRow(c, row);
    for (int col = 0; col < 5; ++col) {
      sprite_write_fill_rect(self, x + col * scale, y + row * scale, scale,
                             scale, (bits & (1 << (4 - col))) ? fg : bg);
    }
  }
}

void sprite_draw_string(sprite_t* self, const char* text, int32_t x, int32_t y,
                        uint8_t font) {
  if (!text) return;
  const uint8_t scale = DIV_MAX(self->textSize_, font > 1 ? 2 : 1);
  const int16_t w = sprite_text_width(self, text, font);
  if (self->textDatum_ == TC_DATUM || self->textDatum_ == MC_DATUM ||
      self->textDatum_ == BC_DATUM)
    x -= w / 2;
  for (const char* p = text; *p; ++p) {
    sprite_draw_glyph(self, *p, x, y, scale, self->textFg_, self->textBg_);
    x += 6 * scale;
  }
}

void sprite_set_text_color_1(sprite_t* self, uint16_t fg) {
  self->textFg_ = fg;
  self->textBg_ = fg;
}

void sprite_set_text_color_2(sprite_t* self, uint16_t fg, uint16_t bg) {
  self->textFg_ = fg;
  self->textBg_ = bg;
}

void sprite_set_text_size(sprite_t* self, uint8_t size) {
  self->textSize_ = DIV_MAX(1, size);
}

void sprite_set_text_font(sprite_t* self, uint8_t font) {
  self->textFont_ = font;
}

void sprite_set_text_datum(sprite_t* self, uint8_t datum) {
  self->textDatum_ = datum;
}

int16_t display_width(const display_t* self) { return self->width_; }

int16_t display_height(const display_t* self) { return self->height_; }

void sprite_set_color_depth(sprite_t* self, uint8_t bpp) {
  if (!self->pixels_) self->colorDepth_ = bpp <= 8 ? 8 : 16;
}

int16_t sprite_width(const sprite_t* self) { return self->width_; }

int16_t sprite_height(const sprite_t* self) { return self->height_; }