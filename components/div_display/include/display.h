#pragma once
#include <math.h>
#include <stdarg.h>

#include "board_config.h"
#include "board_pins.h"
#include "c_util.h"
#include "system_utils.h"

#define TFT_BLACK 0x0000
#define TFT_NAVY 0x000F
#define TFT_DARKGREEN 0x03E0
#define TFT_DARKCYAN 0x03EF
#define TFT_MAROON 0x7800
#define TFT_PURPLE 0x780F
#define TFT_OLIVE 0x7BE0
#define TFT_LIGHTGREY 0xC618
#define TFT_DARKGREY 0x7BEF
#define TFT_BLUE 0x001F
#define TFT_GREEN 0x07E0
#define TFT_CYAN 0x07FF
#define TFT_RED 0xF800
#define TFT_MAGENTA 0xF81F
#define TFT_YELLOW 0xFFE0
#define TFT_WHITE 0xFFFF
#define TFT_ORANGE 0xFD20

#define TL_DATUM 0
#define TC_DATUM 1
#define TR_DATUM 2
#define ML_DATUM 3
#define MC_DATUM 4
#define MR_DATUM 5
#define BL_DATUM 6
#define BC_DATUM 7
#define BR_DATUM 8

#define ILI9341_VSCRDEF 0x33
#define ILI9341_VSCRSADD 0x37

typedef struct {
  int16_t width_;
  int16_t height_;
  uint8_t rotation_;
  uint16_t textFg_;
  uint16_t textBg_;
  uint8_t textSize_;
  uint8_t textFont_;
  uint8_t textDatum_;
  bool textWrap_;
  int32_t cursorX_;
  int32_t cursorY_;
} display_t;
#define DISPLAY_INITIALIZER \
  {.width_ = TFT_WIDTH,     \
   .height_ = TFT_HEIGHT,   \
   .textFg_ = TFT_WHITE,    \
   .textBg_ = TFT_BLACK,    \
   .textSize_ = 1,          \
   .textFont_ = 1,          \
   .textWrap_ = true}
typedef struct {
  display_t* parent_;
  uint8_t* pixels_;
  size_t pixelCount_;
  int16_t width_;
  int16_t height_;
  uint8_t colorDepth_;
  uint16_t textFg_;
  uint16_t textBg_;
  uint8_t textSize_;
  uint8_t textFont_;
  uint8_t textDatum_;
} sprite_t;
#define SPRITE_INITIALIZER \
  {.pixels_ = NULL,        \
   .pixelCount_ = 0,       \
   .width_ = 0,            \
   .height_ = 0,           \
   .colorDepth_ = 16,      \
   .textFg_ = TFT_WHITE,   \
   .textBg_ = TFT_BLACK,   \
   .textSize_ = 1,         \
   .textFont_ = 1,         \
   .textDatum_ = TL_DATUM}
void display_construct(display_t* self);
void display_init(display_t* self);
void display_set_rotation(display_t* self, uint8_t rotation);
int16_t display_width(const display_t* self);
int16_t display_height(const display_t* self);
void display_fill_screen(display_t* self, uint16_t color);
void display_fill_rect(display_t* self, int32_t x, int32_t y, int32_t w,
                       int32_t h, uint16_t color);
void display_draw_rect(display_t* self, int32_t x, int32_t y, int32_t w,
                       int32_t h, uint16_t color);
void display_draw_round_rect(display_t* self, int32_t x, int32_t y, int32_t w,
                             int32_t h, int32_t r, uint16_t color);
void display_fill_round_rect(display_t* self, int32_t x, int32_t y, int32_t w,
                             int32_t h, int32_t r, uint16_t color);
void display_draw_fast_h_line(display_t* self, int32_t x, int32_t y, int32_t w,
                              uint16_t color);
void display_draw_fast_v_line(display_t* self, int32_t x, int32_t y, int32_t h,
                              uint16_t color);
void display_draw_line(display_t* self, int32_t x0, int32_t y0, int32_t x1,
                       int32_t y1, uint16_t color);
void display_draw_pixel(display_t* self, int32_t x, int32_t y, uint16_t color);
void display_draw_circle(display_t* self, int32_t x0, int32_t y0, int32_t r,
                         uint16_t color);
void display_fill_circle(display_t* self, int32_t x0, int32_t y0, int32_t r,
                         uint16_t color);
void display_fill_triangle(display_t* self, int32_t x0, int32_t y0, int32_t x1,
                           int32_t y1, int32_t x2, int32_t y2, uint16_t color);
void display_draw_bitmap(display_t* self, int32_t x, int32_t y,
                         const uint8_t* bitmap, int32_t w, int32_t h,
                         uint16_t color);
void display_set_cursor(display_t* self, int32_t x, int32_t y);
void display_set_text_color_1(display_t* self, uint16_t fg);
void display_set_text_color_2(display_t* self, uint16_t fg, uint16_t bg);
void display_set_text_size(display_t* self, uint8_t size);
void display_set_text_font(display_t* self, uint8_t font);
void display_set_text_datum(display_t* self, uint8_t datum);
void display_set_text_wrap(display_t* self, bool wrap);
int16_t display_text_width(const display_t* self, const char* text,
                           uint8_t font);
int16_t display_draw_string(display_t* self, const char* text, int32_t x,
                            int32_t y, uint8_t font);
int16_t display_draw_centre_string(display_t* self, const char* text, int32_t x,
                                   int32_t y, uint8_t font);
int16_t display_draw_number(display_t* self, long n, int32_t x, int32_t y,
                            uint8_t font);
int16_t display_draw_char(display_t* self, char c, int32_t x, int32_t y,
                          uint8_t font);
size_t display_print(display_t* self, const char* s);
size_t display_println(display_t* self);
size_t display_printf(display_t* self, const char* fmt, ...);
void display_writecommand(display_t* self, uint8_t cmd);
void display_writedata(display_t* self, uint8_t data);
void display_write_pixel(display_t* self, int32_t x, int32_t y, uint16_t color);
void display_write_fill_rect(display_t* self, int32_t x, int32_t y, int32_t w,
                             int32_t h, uint16_t color);
void display_push_pixels(display_t* self, int32_t x, int32_t y, int32_t w,
                         int32_t h, const uint16_t* pixels);
void display_init_panel(display_t* self);
void display_send_command(display_t* self, uint8_t cmd, const uint8_t* data,
                          size_t len);
void display_set_addr_window(display_t* self, int32_t x, int32_t y, int32_t w,
                             int32_t h);
int16_t display_char_width(const display_t* self, uint8_t font);
int16_t display_char_height(const display_t* self, uint8_t font);
void display_draw_glyph(display_t* self, char c, int32_t x, int32_t y,
                        uint8_t scale, uint16_t fg, uint16_t bg);
void sprite_construct(sprite_t* self, display_t* parent);
void sprite_destroy(sprite_t* self);
bool sprite_create_sprite(sprite_t* self, int16_t w, int16_t h);
void sprite_delete_sprite(sprite_t* self);
void sprite_set_color_depth(sprite_t* self, uint8_t bpp);
void sprite_push_sprite(sprite_t* self, int32_t x, int32_t y);
int16_t sprite_width(const sprite_t* self);
int16_t sprite_height(const sprite_t* self);
void sprite_fill_screen(sprite_t* self, uint16_t color);
void sprite_fill_rect(sprite_t* self, int32_t x, int32_t y, int32_t w,
                      int32_t h, uint16_t color);
void sprite_draw_rect(sprite_t* self, int32_t x, int32_t y, int32_t w,
                      int32_t h, uint16_t color);
void sprite_draw_round_rect(sprite_t* self, int32_t x, int32_t y, int32_t w,
                            int32_t h, int32_t r, uint16_t color);
void sprite_fill_round_rect(sprite_t* self, int32_t x, int32_t y, int32_t w,
                            int32_t h, int32_t r, uint16_t color);
void sprite_draw_bitmap(sprite_t* self, int32_t x, int32_t y,
                        const uint8_t* bitmap, int32_t w, int32_t h,
                        uint16_t color);
void sprite_draw_line(sprite_t* self, int32_t x0, int32_t y0, int32_t x1,
                      int32_t y1, uint16_t color);
void sprite_draw_circle(sprite_t* self, int32_t x0, int32_t y0, int32_t r,
                        uint16_t color);
void sprite_fill_circle(sprite_t* self, int32_t x0, int32_t y0, int32_t r,
                        uint16_t color);
void sprite_fill_triangle(sprite_t* self, int32_t x0, int32_t y0, int32_t x1,
                          int32_t y1, int32_t x2, int32_t y2, uint16_t color);
void sprite_draw_string(sprite_t* self, const char* text, int32_t x, int32_t y,
                        uint8_t font);
void sprite_set_text_color_1(sprite_t* self, uint16_t fg);
void sprite_set_text_color_2(sprite_t* self, uint16_t fg, uint16_t bg);
void sprite_set_text_size(sprite_t* self, uint8_t size);
void sprite_set_text_font(sprite_t* self, uint8_t font);
void sprite_set_text_datum(sprite_t* self, uint8_t datum);
void sprite_write_pixel(sprite_t* self, int32_t x, int32_t y, uint16_t color);
void sprite_write_fill_rect(sprite_t* self, int32_t x, int32_t y, int32_t w,
                            int32_t h, uint16_t color);
int16_t sprite_text_width(const sprite_t* self, const char* text, uint8_t font);
void sprite_draw_glyph(sprite_t* self, char c, int32_t x, int32_t y,
                       uint8_t scale, uint16_t fg, uint16_t bg);

#include "text_format.h"
static inline size_t display_print_line_cstr(display_t* self,
                                             const char* value) {
  return display_print(self, value) + display_print(self, "\n");
}
static inline size_t display_print_value(display_t* self, text_t value) {
  return display_print(self, value.data);
}
static inline size_t display_print_line(display_t* self, text_t value) {
  return display_print_value(self, value) + display_print(self, "\n");
}
