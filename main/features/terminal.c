#include "c_vector.h"
#include "config.h"
#include "icon.h"
#include "storage_file.h"
#include "system_features.h"
#include "touch_ui.h"
#include "ui_utils.h"

#define TEXT_HEIGHT 16
#define BOT_FIXED_AREA 0
#define TOP_FIXED_AREA 86
#define DISPLAY_WIDTH TFT_WIDTH
#define DISPLAY_HEIGHT TFT_HEIGHT
#define SCREEN_WIDTH TFT_WIDTH
#define SCREENHEIGHT TFT_HEIGHT

static bool Terminal_uiDrawn = false;

uint16_t Terminal_yStart = TOP_FIXED_AREA;
uint16_t Terminal_yArea = DISPLAY_HEIGHT - TOP_FIXED_AREA - BOT_FIXED_AREA;
uint16_t Terminal_yDraw = DISPLAY_HEIGHT - BOT_FIXED_AREA - TEXT_HEIGHT;

uint16_t Terminal_xPos = 0;

uint8_t Terminal_data = 0;

bool Terminal_change_colour = 1;
bool Terminal_selected = 1;
bool Terminal_terminalActive = true;

int Terminal_blank[19];

long Terminal_baudRates[] = {9600, 19200, 38400, 57600, 115200};
uint8_t Terminal_baudIndex = 0;

static int Terminal_terminalContentBottom(void) {
  return featureHasTouchNavBar() ? (int)touchNavContentBottomY()
                                 : DISPLAY_HEIGHT;
}

static int Terminal_terminalBotFixedArea(void) {
  const int bottom = Terminal_terminalContentBottom();
  return (bottom < DISPLAY_HEIGHT) ? (DISPLAY_HEIGHT - bottom) : BOT_FIXED_AREA;
}

static void Terminal_terminalUpdateNavLabels(void) {
  if (!featureHasTouchNavBar()) {
    return;
  }
  setTouchNavLabels("Exit", NULL, Terminal_terminalActive ? "Pause" : "Active",
                    NULL, "Baud");
  redrawTouchButtonBar();
}

static void Terminal_terminalCycleBaud(void) {
  if (Terminal_terminalActive) {
    Terminal_terminalActive = false;
    Terminal_terminalUpdateNavLabels();
    return;
  }
  Terminal_baudIndex = (Terminal_baudIndex + 1) % 5;
  div_console_close();
  sys_sleep_ms(100);
  div_console_open(Terminal_baudRates[Terminal_baudIndex]);
  display_fill_rect(&(tft), 0, 37, DISPLAY_WIDTH, 16, ORANGE);
  display_set_text_color_2(&(tft), TFT_WHITE, TFT_WHITE);
  text_t baudMsg = text_join_cstr(
      (text_join_cstr(" Serial Terminal - ",
                      (TEXT(Terminal_baudRates[Terminal_baudIndex])).data))
          .data,
      " baud ");
  display_draw_centre_string(&(tft), (baudMsg).data, DISPLAY_WIDTH / 2, 37, 1);
  sys_sleep_ms(10);
}

static void Terminal_terminalSetActive(bool active) {
  Terminal_terminalActive = active;
  sys_sleep_ms(10);
  display_fill_rect(&(tft), 0, 37, DISPLAY_WIDTH, 16, ORANGE);
  display_set_text_color_2(&(tft), TFT_WHITE, TFT_WHITE);
  if (active) {
    display_draw_centre_string(&(tft), " Serial Terminal Active ",
                               DISPLAY_WIDTH / 2, 37, 1);
  } else {
    text_t baudMsg = text_join_cstr(
        (text_join_cstr(" Serial Terminal - ",
                        (TEXT(Terminal_baudRates[Terminal_baudIndex])).data))
            .data,
        " baud ");
    display_draw_centre_string(&(tft), (baudMsg).data, DISPLAY_WIDTH / 2, 37,
                               1);
  }
  Terminal_terminalUpdateNavLabels();
}

static void Terminal_terminalHandleNavButtons(void) {
  if (!featureHasTouchNavBar()) {
    return;
  }
  if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
    feature_exit_requested = true;
    return;
  }
  if (isTouchNavButtonPressedEdge(BTN_RIGHT)) {
    Terminal_terminalCycleBaud();
    return;
  }
  if (isTouchNavButtonPressedEdge(BTN_SELECT)) {
    Terminal_terminalSetActive(!Terminal_terminalActive);
  }
}

void Terminal_runUI(void) {
#undef STATUS_BAR_Y_OFFSET
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#undef ICON_NUM
#define ICON_NUM 3

  static int iconX[ICON_NUM] = {210, 170, 10};
  static int iconY = STATUS_BAR_Y_OFFSET;

  static const unsigned char* icons[ICON_NUM] = {
      bitmap_icon_sort_up_plus, bitmap_icon_power, bitmap_icon_go_back};

  if (!Terminal_uiDrawn) {
    display_draw_line(&(tft), 0, 19, display_width(&(tft)), 19, TFT_WHITE);
    display_fill_rect(&(tft), 0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH,
                      STATUS_BAR_HEIGHT, DARK_GRAY);

    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        display_draw_bitmap(&(tft), iconX[i], iconY, icons[i], ICON_SIZE,
                            ICON_SIZE, TFT_WHITE);
      }
    }
    display_draw_line(&(tft), 0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT,
                      SCREEN_WIDTH, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT,
                      ORANGE);
    Terminal_uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  if (animationState > 0 && sys_monotonic_ms() - lastAnimationTime >= 150) {
    if (animationState == 1) {
      display_draw_bitmap(&(tft), iconX[activeIcon], iconY, icons[activeIcon],
                          ICON_SIZE, ICON_SIZE, TFT_WHITE);
      animationState = 2;

      switch (activeIcon) {
        case 0:
          if (Terminal_terminalActive) {
            Terminal_terminalActive = false;
          } else if (!Terminal_terminalActive) {
            Terminal_baudIndex = (Terminal_baudIndex + 1) % 5;
            div_console_close();
            sys_sleep_ms(100);
            div_console_open(Terminal_baudRates[Terminal_baudIndex]);
            display_fill_rect(&(tft), 0, 37, DISPLAY_WIDTH, 16, ORANGE);
            display_set_text_color_2(&(tft), TFT_WHITE, TFT_WHITE);
            text_t baudMsg = text_join_cstr(
                (text_join_cstr(
                     " Serial Terminal - ",
                     (TEXT(Terminal_baudRates[Terminal_baudIndex])).data))
                    .data,
                " baud ");
            display_draw_centre_string(&(tft), (baudMsg).data,
                                       DISPLAY_WIDTH / 2, 37, 1);
            sys_sleep_ms(10);
          }
          break;
        case 1:
          sys_sleep_ms(10);
          display_fill_rect(&(tft), 0, 37, DISPLAY_WIDTH, 16, ORANGE);
          display_set_text_color_2(&(tft), TFT_WHITE, TFT_WHITE);
          display_draw_centre_string(&(tft), " Serial Terminal Active ",
                                     DISPLAY_WIDTH / 2, 37, 1);
          Terminal_terminalActive = true;
          break;

        case 2:
          feature_exit_requested = true;
          break;
      }
    } else if (animationState == 2) {
      animationState = 0;
      activeIcon = -1;
    }
    lastAnimationTime = sys_monotonic_ms();
  }

  static unsigned long lastTouchCheck = 0;
  const unsigned long touchCheckInterval = 50;

  if (sys_monotonic_ms() - lastTouchCheck >= touchCheckInterval) {
    int x, y;
    if (!featureHasTouchNavBar() && feature_active && readTouchXY(&(x), &(y))) {
      if (y > STATUS_BAR_Y_OFFSET &&
          y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x > iconX[i] && x < iconX[i] + ICON_SIZE) {
            if (icons[i] != NULL && animationState == 0) {
              display_draw_bitmap(&(tft), iconX[i], iconY, icons[i], ICON_SIZE,
                                  ICON_SIZE, TFT_BLACK);
              animationState = 1;
              activeIcon = i;
              lastAnimationTime = sys_monotonic_ms();
            }
            break;
          }
        }
      }
    }
    lastTouchCheck = sys_monotonic_ms();
  }
}

void Terminal_scrollAddress(uint16_t vsp) {
#ifndef ILI9341_VSCRSADD
#define ILI9341_VSCRSADD 0x37
#endif
  display_writecommand(&(tft), ILI9341_VSCRSADD);
  display_writedata(&(tft), vsp >> 8);
  display_writedata(&(tft), vsp);
}

int Terminal_scroll_line(void) {
  int yTemp = Terminal_yStart;
  display_fill_rect(
      &(tft), 0, Terminal_yStart,
      Terminal_blank[(Terminal_yStart - TOP_FIXED_AREA) / TEXT_HEIGHT],
      TEXT_HEIGHT, TFT_BLACK);

  Terminal_yStart += TEXT_HEIGHT;
  const int scrollLimit = DISPLAY_HEIGHT - Terminal_terminalBotFixedArea();
  if (Terminal_yStart >= scrollLimit) {
    Terminal_yStart = TOP_FIXED_AREA + (Terminal_yStart - scrollLimit);
  }
  Terminal_scrollAddress(Terminal_yStart);
  sys_sleep_ms(1);
  return yTemp;
}

void Terminal_setupScrollArea(uint16_t tfa, uint16_t bfa) {
#ifndef ILI9341_VSCRDEF
#define ILI9341_VSCRDEF 0x33
#endif
  display_writecommand(&(tft), ILI9341_VSCRDEF);
  display_writedata(&(tft), tfa >> 8);
  display_writedata(&(tft), tfa);
  display_writedata(&(tft), (DISPLAY_HEIGHT - tfa - bfa) >> 8);
  display_writedata(&(tft), DISPLAY_HEIGHT - tfa - bfa);
  display_writedata(&(tft), bfa >> 8);
  display_writedata(&(tft), bfa);
}

void Terminal_terminalSetup(void) {
  setupTouchscreen();
  display_fill_screen(&(tft), TFT_BLACK);

  display_fill_rect(&(tft), 0, 37, DISPLAY_WIDTH, 16, ORANGE);
  display_set_text_color_2(&(tft), TFT_WHITE, TFT_WHITE);
  text_t baudMsg = text_join_cstr(
      (text_join_cstr(" Serial Terminal - ",
                      (TEXT(Terminal_baudRates[Terminal_baudIndex])).data))
          .data,
      " baud ");
  display_draw_centre_string(&(tft), (baudMsg).data, DISPLAY_WIDTH / 2, 37, 1);

  float currentBatteryVoltage = readBatteryVoltage();

  drawStatusBar(currentBatteryVoltage, true, false);

  Terminal_uiDrawn = false;

  div_console_open(Terminal_baudRates[Terminal_baudIndex]);

  const int bfa = Terminal_terminalBotFixedArea();
  Terminal_yArea = DISPLAY_HEIGHT - TOP_FIXED_AREA - bfa;
  Terminal_yDraw = DISPLAY_HEIGHT - bfa - TEXT_HEIGHT;
  Terminal_setupScrollArea(TOP_FIXED_AREA, bfa);

  for (uint8_t i = 0; i < 19; i++) Terminal_blank[i] = 0;

  Terminal_terminalUpdateNavLabels();
}

void Terminal_terminalLoop(void) {
  updateStatusBar();
  if (featureHasTouchNavBar()) {
    maintainTouchNavBar();
  }
  Terminal_terminalHandleNavButtons();
  Terminal_runUI();

  if (Terminal_terminalActive) {
    uint8_t charCount = 0;
    while (div_console_pending() && charCount < 10) {
      Terminal_data = div_console_read_byte();
      if (Terminal_data == '\r' || Terminal_xPos > 231) {
        Terminal_xPos = 0;
        Terminal_yDraw = Terminal_scroll_line();
      }
      if (Terminal_data > 31 && Terminal_data < 128) {
        Terminal_xPos += display_draw_char(&(tft), Terminal_data, Terminal_xPos,
                                           Terminal_yDraw, 2);
        Terminal_blank[(18 + (Terminal_yStart - TOP_FIXED_AREA) / TEXT_HEIGHT) %
                       19] = Terminal_xPos;
      }
      charCount++;
    }
  }
}
