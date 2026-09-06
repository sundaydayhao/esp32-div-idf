#include "ui_utils.h"

#include "board_services.h"
#include "c_vector.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gps.h"
#include "icon.h"
#include "math.h"
#include "shared.h"
#include "storage_file.h"
#include "touch_ui.h"

bool notificationVisible = false;
static bool notificationHasSave = false;
static int notifX, notifY, notifWidth, notifHeight;
static int closeButtonX, closeButtonY, closeButtonSize = 16;
static int okButtonX, okButtonY, okButtonWidth = 84, okButtonHeight = 24;
static int saveButtonX, saveButtonY, saveButtonWidth = 84,
                                     saveButtonHeight = 24;

static char s_notifTitleCache[64];
static char s_notifMsgCache[400];
static bool s_notifShowSaveCache = false;

/** Wrapped line count for modal text (font 1, size 1 - must match draw path).
 */
static int countWrappedNotifLines(text_t msgIn, int innerPixelW) {
  display_set_text_font(&(tft), 1);
  display_set_text_size(&(tft), 1);
  text_t msg = msgIn;
  text_trim(&(msg));
  int lines = 0;
  while (strlen((msg).data) > 0 && lines < 48) {
    int lineEnd = strlen((msg).data);
    while (lineEnd > 0 &&
           display_text_width(&(tft), (text_slice(msg, 0, lineEnd)).data, 0) >
               innerPixelW) {
      lineEnd--;
    }
    if (lineEnd <= 0) {
      ++lines;
      break;
    }
    if (lineEnd < strlen((msg).data)) {
      const int lastSpace =
          text_rfind_index(text_slice(msg, 0, lineEnd), text_character(' '));
      if (lastSpace > 0) {
        lineEnd = lastSpace;
      }
    }
    ++lines;
    msg = text_slice(msg, lineEnd, SIZE_MAX);
    text_trim(&(msg));
  }
  return lines;
}

static size_t decodeObfTo(char* out, size_t outSize, const uint8_t* in,
                          size_t inLen, uint8_t key) {
  if (!out || outSize == 0) return 0;
  size_t n = inLen;
  if (n > outSize - 1) n = outSize - 1;
  for (size_t i = 0; i < n; ++i) out[i] = (char)(in[i] ^ key);
  out[n] = '\0';
  return n;
}

void tftPrintObf(const uint8_t* data, size_t len, uint8_t key) {
  char buf[96];
  decodeObfTo(buf, sizeof(buf), data, len, key);
  display_print(&(tft), (TEXT(buf)).data);
}

void tftPrintlnObf(const uint8_t* data, size_t len, uint8_t key) {
  char buf[96];
  decodeObfTo(buf, sizeof(buf), data, len, key);
  display_print_line_cstr(&(tft), (TEXT(buf)).data);
}

void serialPrintObf(const uint8_t* data, size_t len, bool newline,
                    uint8_t key) {
  char buf[128];
  decodeObfTo(buf, sizeof(buf), data, len, key);
  if (newline)
    div_console_line(TEXT(buf));
  else
    div_console_write(TEXT(buf));
}

static inline bool inRect(int x, int y, int rx, int ry, int rw, int rh) {
  return x >= rx && x < (rx + rw) && y >= ry && y < (ry + rh);
}

static void drawNotificationInternal(const char* title, const char* message,
                                     bool showSave) {
  div_copy_string(s_notifTitleCache, sizeof(s_notifTitleCache),
                  title ? title : "");
  div_copy_string(s_notifMsgCache, sizeof(s_notifMsgCache),
                  message ? message : "");
  s_notifShowSaveCache = showSave;
  notificationHasSave = showSave;

  const int scrW = display_width(&(tft));
  const int scrH = display_height(&(tft));
  notifWidth = (scrW > 232) ? 224 : (scrW - 16);
  if (notifWidth < 160) {
    notifWidth = 160;
  }

  const int messageBoxWidth = notifWidth - 10;
  const int innerTextW = messageBoxWidth - 6;
  enum { lineH = 12 };
  const int footH = showSave ? 40 : 30;
  enum { topBand = 25 };

  display_set_text_font(&(tft), 1);
  display_set_text_size(&(tft), 1);
  text_t msgProbe = (message ? TEXT(message) : TEXT(""));
  text_trim(&(msgProbe));
  int lineCount = countWrappedNotifLines(msgProbe, innerTextW);
  if (lineCount < 2) {
    lineCount = 2;
  }
  const int maxLinesByScreen = (scrH - topBand - footH - 14) / lineH;
  int useLines = lineCount;
  if (useLines > maxLinesByScreen) {
    useLines = maxLinesByScreen > 2 ? maxLinesByScreen : 2;
  }

  const int messageBoxHeight = 8 + useLines * lineH;
  notifHeight = topBand + messageBoxHeight + footH;
  if (notifHeight > scrH - 8) {
    notifHeight = scrH - 8;
    const int availBody = notifHeight - topBand - footH;
    useLines = (availBody - 8) / lineH;
    if (useLines < 2) {
      useLines = 2;
    }
  }
  notifX = (scrW - notifWidth) / 2;
  notifY = (scrH - notifHeight) / 2;
  if (notifY < 4) {
    notifY = 4;
  }

  display_fill_rect(&(tft), notifX, notifY, notifWidth, notifHeight,
                    LIGHT_GRAY);
  display_draw_rect(&(tft), notifX, notifY, notifWidth, notifHeight, DARK_GRAY);
  display_fill_rect(&(tft), notifX, notifY, notifWidth, 20, BLUE);

  display_set_text_color_2(&(tft), WHITE, BLUE);
  display_set_text_size(&(tft), 1);
  display_set_cursor(&(tft), notifX + 6, notifY + 5);
  display_print(&(tft), (TEXT(title)).data);

  closeButtonX = notifX + notifWidth - closeButtonSize - 6;
  closeButtonY = notifY + 3;
  display_fill_rect(&(tft), closeButtonX, closeButtonY, closeButtonSize,
                    closeButtonSize, RED);
  display_set_text_color_2(&(tft), WHITE, RED);
  display_set_cursor(&(tft), closeButtonX + 5, closeButtonY + 4);
  display_print(&(tft), "X");

  int messageBoxX = notifX + 5;
  int messageBoxY = notifY + 25;
  const int messageBoxDrawH = notifHeight - topBand - footH;
  display_fill_rect(&(tft), messageBoxX, messageBoxY, messageBoxWidth,
                    messageBoxDrawH, WHITE);
  display_set_text_color_2(&(tft), BLACK, WHITE);

  {
    const int maxY = messageBoxY + messageBoxDrawH - lineH;
    text_t msg = (message ? TEXT(message) : TEXT(""));
    text_trim(&(msg));
    int cursorX = messageBoxX + 3;
    int cursorY = messageBoxY + 4;
    while (strlen((msg).data) > 0 && cursorY <= maxY) {
      int lineEnd = strlen((msg).data);
      while (lineEnd > 0 &&
             display_text_width(&(tft), (text_slice(msg, 0, lineEnd)).data, 0) >
                 innerTextW) {
        lineEnd--;
      }
      if (lineEnd <= 0) {
        break;
      }
      if (lineEnd < strlen((msg).data)) {
        int lastSpace =
            text_rfind_index(text_slice(msg, 0, lineEnd), text_character(' '));
        if (lastSpace > 0) {
          lineEnd = lastSpace;
        }
      }
      display_set_cursor(&(tft), cursorX, cursorY);
      display_print(&(tft), (text_slice(msg, 0, lineEnd)).data);
      msg = text_slice(msg, lineEnd, SIZE_MAX);
      text_trim(&(msg));
      cursorY += lineH;
    }

    if (strlen((msg).data) > 0) {
      display_set_cursor(&(tft), cursorX, maxY);
      display_print(&(tft), "...");
    }
  }

  int btnY = notifY + notifHeight - 25;
  if (showSave) {
    okButtonWidth = 72;
    saveButtonWidth = 72;
    okButtonHeight = saveButtonHeight = 20;
    okButtonX = notifX + 10;
    saveButtonX = notifX + notifWidth - 10 - saveButtonWidth;
    okButtonY = saveButtonY = btnY;

    display_fill_rect(&(tft), okButtonX, okButtonY, okButtonWidth,
                      okButtonHeight, GRAY);
    display_draw_rect(&(tft), okButtonX, okButtonY, okButtonWidth,
                      okButtonHeight, DARK_GRAY);
    display_draw_line(&(tft), okButtonX, okButtonY, okButtonX + okButtonWidth,
                      okButtonY, WHITE);
    display_draw_line(&(tft), okButtonX, okButtonY, okButtonX,
                      okButtonY + okButtonHeight, WHITE);
    display_set_text_color_2(&(tft), BLACK, GRAY);
    display_set_cursor(&(tft), okButtonX + 28, okButtonY + 5);
    display_print(&(tft), "OK");

    display_fill_rect(&(tft), saveButtonX, saveButtonY, saveButtonWidth,
                      saveButtonHeight, GREEN);
    display_draw_rect(&(tft), saveButtonX, saveButtonY, saveButtonWidth,
                      saveButtonHeight, DARK_GRAY);
    display_draw_line(&(tft), saveButtonX, saveButtonY,
                      saveButtonX + saveButtonWidth, saveButtonY, WHITE);
    display_draw_line(&(tft), saveButtonX, saveButtonY, saveButtonX,
                      saveButtonY + saveButtonHeight, WHITE);
    display_set_text_color_2(&(tft), BLACK, GREEN);
    display_set_cursor(&(tft), saveButtonX + 18, saveButtonY + 5);
    display_print(&(tft), "SAVE");
  } else {
    okButtonWidth = 72;
    okButtonHeight = 20;
    okButtonX = notifX + (notifWidth - okButtonWidth) / 2;
    okButtonY = btnY;
    display_fill_rect(&(tft), okButtonX, okButtonY, okButtonWidth,
                      okButtonHeight, GRAY);
    display_draw_rect(&(tft), okButtonX, okButtonY, okButtonWidth,
                      okButtonHeight, DARK_GRAY);
    display_draw_line(&(tft), okButtonX, okButtonY, okButtonX + okButtonWidth,
                      okButtonY, WHITE);
    display_draw_line(&(tft), okButtonX, okButtonY, okButtonX,
                      okButtonY + okButtonHeight, WHITE);
    display_set_text_color_2(&(tft), BLACK, GRAY);
    display_set_cursor(&(tft), okButtonX + 28, okButtonY + 5);
    display_print(&(tft), "OK");
  }

  notificationVisible = true;
}

void showNotificationActions(const char* title, const char* message,
                             bool showSave) {
  drawNotificationInternal(title, message, showSave);
}

void showNotification(const char* title, const char* message) {
  drawNotificationInternal(title, message, false);
}

void hideNotification(void) {
  applyThemeToPalette((*settings()).theme);
  display_fill_rect(&(tft), notifX, notifY, notifWidth, notifHeight, UI_BG);
  notificationVisible = false;
}

bool isNotificationVisible(void) { return notificationVisible; }

void notificationRedrawIfVisible(void) {
  if (!notificationVisible) {
    return;
  }
  drawNotificationInternal(s_notifTitleCache, s_notifMsgCache,
                           s_notifShowSaveCache);
}

NotificationAction notificationHandleTouch(int x, int y) {
  if (!notificationVisible) return NotificationAction_None;

  if (inRect(x, y, closeButtonX, closeButtonY, closeButtonSize,
             closeButtonSize)) {
    hideNotification();
    return NotificationAction_Close;
  }
  if (inRect(x, y, okButtonX, okButtonY, okButtonWidth, okButtonHeight)) {
    hideNotification();
    return NotificationAction_Ok;
  }
  if (notificationHasSave && inRect(x, y, saveButtonX, saveButtonY,
                                    saveButtonWidth, saveButtonHeight)) {
    hideNotification();
    return NotificationAction_Save;
  }
  return NotificationAction_None;
}

void printWrappedText(int x, int y, int maxWidth, const char* text) {
  text_t message = TEXT(text);
  int cursorX = x, cursorY = y;

  while (strlen((message).data) > 0) {
    int lineEnd = strlen((message).data);

    while (display_text_width(&(tft), (text_slice(message, 0, lineEnd)).data,
                              0) > maxWidth) {
      lineEnd--;
    }

    if (lineEnd < strlen((message).data)) {
      int lastSpace = text_rfind_index(text_slice(message, 0, lineEnd),
                                       text_character(' '));
      if (lastSpace > 0) lineEnd = lastSpace;
    }

    display_set_cursor(&(tft), cursorX, cursorY);
    display_print(&(tft), (text_slice(message, 0, lineEnd)).data);

    message = text_slice(message, lineEnd, SIZE_MAX);
    text_trim(&(message));

    cursorY += 15;
  }
}

#ifndef FEATURE_TEXT
#define FEATURE_TEXT ORANGE
#endif

static inline uint16_t FeatureUI_btnFill(FeatureUI_ButtonStyle style,
                                         bool pressed, bool disabled) {
  if (disabled) return UI_LINE;
  if (pressed) return UI_FG;
  switch (style) {
    case FeatureUI_ButtonStyle_Primary:
      return FEATURE_TEXT;
    case FeatureUI_ButtonStyle_Secondary:
      return UI_FG;
    case FeatureUI_ButtonStyle_Danger:
      return UI_WARN;
  }
  return UI_FG;
}

static inline uint16_t FeatureUI_btnBorder(FeatureUI_ButtonStyle style,
                                           bool disabled) {
  if (disabled) return UI_LINE;
  switch (style) {
    case FeatureUI_ButtonStyle_Primary:
      return FEATURE_TEXT;
    case FeatureUI_ButtonStyle_Secondary:
      return FEATURE_TEXT;
    case FeatureUI_ButtonStyle_Danger:
      return UI_WARN;
  }
  return FEATURE_TEXT;
}

static inline uint16_t FeatureUI_btnText(FeatureUI_ButtonStyle style,
                                         bool disabled) {
  if (disabled) return UI_TEXT;

  switch (style) {
    case FeatureUI_ButtonStyle_Secondary:
      return UI_ICON;
    case FeatureUI_ButtonStyle_Primary:
    case FeatureUI_ButtonStyle_Danger:
      return FEATURE_BG;
  }
  return UI_ICON;
}

void FeatureUI_drawFooterBg(void) {
  display_fill_rect(&(tft), 0, display_height(&(tft)) - FeatureUI_FOOTER_H,
                    display_width(&(tft)), FeatureUI_FOOTER_H, FEATURE_BG);
}

void FeatureUI_drawButtonRect(int x, int y, int w, int h, const char* label,
                              FeatureUI_ButtonStyle style, bool pressed,
                              bool disabled, uint8_t font) {
  if (!label) label = "";
  int r = h / 2;
  if (r > 6) r = 6;
  if (r < 0) r = 0;
  uint16_t fill = FeatureUI_btnFill(style, pressed, disabled);
  uint16_t edge = FeatureUI_btnBorder(style, disabled);
  uint16_t txt = FeatureUI_btnText(style, disabled);

  display_fill_round_rect(&(tft), x, y, w, h, r, fill);
  display_draw_round_rect(&(tft), x, y, w, h, r, edge);

  display_set_text_datum(&(tft), MC_DATUM);
  display_set_text_color_2(&(tft), txt, fill);

  display_draw_string(&(tft), label, x + w / 2, y + h / 2, font);

  display_set_text_datum(&(tft), TL_DATUM);
  display_set_text_font(&(tft), 1);
  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), UI_TEXT, FEATURE_BG);
}

void FeatureUI_layoutFooter3(FeatureUI_Button (*btns)[3], const char* l0,
                             FeatureUI_ButtonStyle s0, const char* l1,
                             FeatureUI_ButtonStyle s1, const char* l2,
                             FeatureUI_ButtonStyle s2, bool d0, bool d1,
                             bool d2) {
  int availW = display_width(&(tft)) - 2 * FeatureUI_PAD_X;
  int w = (availW - 2 * FeatureUI_GAP_X) / 3;
  int y = display_height(&(tft)) - FeatureUI_FOOTER_H +
          (FeatureUI_FOOTER_H - FeatureUI_BTN_H) / 2;
  int x0 = FeatureUI_PAD_X;
  int x1 = x0 + w + FeatureUI_GAP_X;
  int x2 = x1 + w + FeatureUI_GAP_X;
  (*btns)[0] = (FeatureUI_Button){
      (int16_t)x0, (int16_t)y, (int16_t)w, (int16_t)FeatureUI_BTN_H,
      l0,          s0,         d0};
  (*btns)[1] = (FeatureUI_Button){
      (int16_t)x1, (int16_t)y, (int16_t)w, (int16_t)FeatureUI_BTN_H,
      l1,          s1,         d1};
  (*btns)[2] = (FeatureUI_Button){
      (int16_t)x2, (int16_t)y, (int16_t)w, (int16_t)FeatureUI_BTN_H,
      l2,          s2,         d2};
}

void FeatureUI_layoutFooter2(FeatureUI_Button (*btns)[2], const char* l0,
                             FeatureUI_ButtonStyle s0, const char* l1,
                             FeatureUI_ButtonStyle s1, bool d0, bool d1) {
  int availW = display_width(&(tft)) - 2 * FeatureUI_PAD_X;
  int w = (availW - FeatureUI_GAP_X) / 2;
  int y = display_height(&(tft)) - FeatureUI_FOOTER_H +
          (FeatureUI_FOOTER_H - FeatureUI_BTN_H) / 2;
  int x0 = FeatureUI_PAD_X;
  int x1 = x0 + w + FeatureUI_GAP_X;
  (*btns)[0] = (FeatureUI_Button){
      (int16_t)x0, (int16_t)y, (int16_t)w, (int16_t)FeatureUI_BTN_H,
      l0,          s0,         d0};
  (*btns)[1] = (FeatureUI_Button){
      (int16_t)x1, (int16_t)y, (int16_t)w, (int16_t)FeatureUI_BTN_H,
      l1,          s1,         d1};
}

void FeatureUI_layoutFooter4(FeatureUI_Button (*btns)[4], const char* l0,
                             FeatureUI_ButtonStyle s0, const char* l1,
                             FeatureUI_ButtonStyle s1, const char* l2,
                             FeatureUI_ButtonStyle s2, const char* l3,
                             FeatureUI_ButtonStyle s3, bool d0, bool d1,
                             bool d2, bool d3) {
  int availW = display_width(&(tft)) - 2 * FeatureUI_PAD_X;
  int w = (availW - 3 * FeatureUI_GAP_X) / 4;
  int y = display_height(&(tft)) - FeatureUI_FOOTER_H +
          (FeatureUI_FOOTER_H - FeatureUI_BTN_H) / 2;
  int x0 = FeatureUI_PAD_X;
  int x1 = x0 + w + FeatureUI_GAP_X;
  int x2 = x1 + w + FeatureUI_GAP_X;
  int x3 = x2 + w + FeatureUI_GAP_X;
  (*btns)[0] = (FeatureUI_Button){
      (int16_t)x0, (int16_t)y, (int16_t)w, (int16_t)FeatureUI_BTN_H,
      l0,          s0,         d0};
  (*btns)[1] = (FeatureUI_Button){
      (int16_t)x1, (int16_t)y, (int16_t)w, (int16_t)FeatureUI_BTN_H,
      l1,          s1,         d1};
  (*btns)[2] = (FeatureUI_Button){
      (int16_t)x2, (int16_t)y, (int16_t)w, (int16_t)FeatureUI_BTN_H,
      l2,          s2,         d2};
  (*btns)[3] = (FeatureUI_Button){
      (int16_t)x3, (int16_t)y, (int16_t)w, (int16_t)FeatureUI_BTN_H,
      l3,          s3,         d3};
}

void FeatureUI_layoutFooter1(FeatureUI_Button* btn, const char* label,
                             FeatureUI_ButtonStyle style, bool disabled) {
  int x = FeatureUI_PAD_X;
  int w = display_width(&(tft)) - 2 * FeatureUI_PAD_X;
  int y = display_height(&(tft)) - FeatureUI_FOOTER_H +
          (FeatureUI_FOOTER_H - FeatureUI_BTN_H) / 2;
  (*btn) = (FeatureUI_Button){
      (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)FeatureUI_BTN_H,
      label,      style,      disabled};
}

void FeatureUI_layoutFooter5(FeatureUI_Button (*btns)[5], const char* l0,
                             FeatureUI_ButtonStyle s0, const char* l1,
                             FeatureUI_ButtonStyle s1, const char* l2,
                             FeatureUI_ButtonStyle s2, const char* l3,
                             FeatureUI_ButtonStyle s3, const char* l4,
                             FeatureUI_ButtonStyle s4, bool d0, bool d1,
                             bool d2, bool d3, bool d4) {
  int availW = display_width(&(tft)) - 2 * FeatureUI_PAD_X;
  int w = (availW - 4 * FeatureUI_GAP_X) / 5;
  int y = display_height(&(tft)) - FeatureUI_FOOTER_H +
          (FeatureUI_FOOTER_H - FeatureUI_BTN_H) / 2;
  int x0 = FeatureUI_PAD_X;
  int x1 = x0 + w + FeatureUI_GAP_X;
  int x2 = x1 + w + FeatureUI_GAP_X;
  int x3 = x2 + w + FeatureUI_GAP_X;
  int x4 = x3 + w + FeatureUI_GAP_X;
  (*btns)[0] = (FeatureUI_Button){
      (int16_t)x0, (int16_t)y, (int16_t)w, (int16_t)FeatureUI_BTN_H,
      l0,          s0,         d0};
  (*btns)[1] = (FeatureUI_Button){
      (int16_t)x1, (int16_t)y, (int16_t)w, (int16_t)FeatureUI_BTN_H,
      l1,          s1,         d1};
  (*btns)[2] = (FeatureUI_Button){
      (int16_t)x2, (int16_t)y, (int16_t)w, (int16_t)FeatureUI_BTN_H,
      l2,          s2,         d2};
  (*btns)[3] = (FeatureUI_Button){
      (int16_t)x3, (int16_t)y, (int16_t)w, (int16_t)FeatureUI_BTN_H,
      l3,          s3,         d3};
  (*btns)[4] = (FeatureUI_Button){
      (int16_t)x4, (int16_t)y, (int16_t)w, (int16_t)FeatureUI_BTN_H,
      l4,          s4,         d4};
}

int FeatureUI_hit(const FeatureUI_Button* btns, int n, int x, int y) {
  for (int i = 0; i < n; ++i) {
    __auto_type b = &(btns[i]);
    if (x >= (*b).x && x <= ((*b).x + (*b).w) && y >= (*b).y &&
        y <= ((*b).y + (*b).h))
      return i;
  }
  return -1;
}

unsigned long lastStatusBarUpdate = 0;
enum { STATUS_BAR_UPDATE_INTERVAL = 2000 };
float lastBatteryVoltage = 0.0;
bool sdCardPresent = false;
bool lastSdCardState = false;

static bool statusBarDirty = true;
enum { kStatusBarWardBlinkHalfMs = 900 };
static bool s_sdMounted = false;
static uint32_t s_sdNextMountAttemptMs = 0;
enum { kSdMountRetryDelayMs = 10000 };

#ifndef SD_CD_RELIABLE
#define SD_CD_RELIABLE 1
#endif

void requestStatusBarRedraw(void) { statusBarDirty = true; }

float readBatteryVoltage(void) { return board_battery_voltage(); }
float readInternalTemperature(void) { return sys_temperature_celsius(); }

void updateSdCardStatus(void) {
  bool cardDetected = true;
#if defined(SD_CD) && SD_CD_RELIABLE
  sys_configure_gpio_pull(SD_CD, GPIO_MODE_INPUT, GPIO_PULLUP_ENABLE);
  cardDetected = !gpio_get_level(((gpio_num_t)(SD_CD)));
#endif

  if (!cardDetected && SD_CD_RELIABLE) {
    s_sdMounted = false;
    s_sdNextMountAttemptMs = 0;
  }

  if (cardDetected != lastSdCardState) {
    sdCardPresent = cardDetected;
    lastSdCardState = cardDetected;
    if (cardDetected) {
      s_sdNextMountAttemptMs = 0;
    }

    div_console_line(
        TEXT(sdCardPresent ? "SD Card detected" : "SD Card removed"));
    requestStatusBarRedraw();
  }
}

static bool statusBarBatteryMeaningfulChange(int newPct, int oldPct,
                                             bool force) {
  if (force || oldPct == -1) {
    return true;
  }
  if (newPct <= 25 || oldPct <= 25) {
    return newPct != oldPct;
  }
  const int d = newPct - oldPct;
  return (d >= 3 || d <= -3);
}

static int statusBarTempBand(float t) {
  if (fabs(((double)(t)) - 53.33) < 0.51) {
    return 1;
  }
  if (t > 55.f) {
    return 2;
  }
  return 0;
}

void drawStatusBar(float batteryVoltage, bool forceUpdate,
                   bool bottomSeparator) {
  static int lastBatteryPercentage = -1;
  static int lastWifiHalf = -100000;
  static int lastBleHalf = -100000;
  static int lastTempBand = -100;
  static int lastSdSnap = -1;
  static bool lastWardGpsIcon = false;
  static uint32_t lastWardBlinkPhase = 0;

  int batteryPercentage = sys_scale(batteryVoltage * 100, 300, 420, 0, 100);
  batteryPercentage = DIV_CLAMP(batteryPercentage, 0, 100);

  int wifiDevices = 0;
  int bleDevices = 0;

  wifiDevices = WifiScan_getLastCount();
  bleDevices = BleScan_getLastCount();

  const int wifiHalf = wifiDevices / 2;
  const int bleHalf = bleDevices / 2;

  const bool wardGpsIcon = GpsWardriver_statusBarGpsIconActive();
  const uint32_t wardBlinkPhase =
      wardGpsIcon ? (sys_monotonic_ms() / kStatusBarWardBlinkHalfMs) : 0u;
  const bool wardSatVisible = wardGpsIcon && ((wardBlinkPhase & 1u) == 0u);

  const float internalTemp = readInternalTemperature();
  const int tempBand = statusBarTempBand(internalTemp);
  const int sdSnap = sdCardPresent ? 1 : 0;

  const bool battCh = statusBarBatteryMeaningfulChange(
      batteryPercentage, lastBatteryPercentage, forceUpdate);
  const bool wardBlinkOnly = !forceUpdate && wardGpsIcon && lastWardGpsIcon &&
                             (wardBlinkPhase != lastWardBlinkPhase) &&
                             !battCh && wifiHalf == lastWifiHalf &&
                             bleHalf == lastBleHalf &&
                             tempBand == lastTempBand && sdSnap == lastSdSnap;

  if (wardBlinkOnly) {
    enum { kBarH = 20 };
    enum { kY = 4 };
    enum { kIconY = kY - 2 };
    enum { kIconW = 16 };
    enum { kGap = 3 };
    enum { kBleIx = 130 };
    enum { kBleDrawX = kBleIx + 25 };
    const int wardGpsX = kBleDrawX - kGap - kIconW;
    if (wardSatVisible) {
      display_draw_bitmap(&(tft), wardGpsX, kIconY, bitmap_icon_satellite,
                          kIconW, kIconW, TFT_ORANGE);
    } else {
      display_fill_rect(&(tft), wardGpsX, kIconY, kIconW, kIconW, UI_LABLE);
    }
    if (bottomSeparator) {
      display_draw_fast_h_line(&(tft), 0, kBarH - 1, display_width(&(tft)),
                               TFT_WHITE);
    }
    lastWardBlinkPhase = wardBlinkPhase;
    return;
  }

  if (battCh || wifiHalf != lastWifiHalf || bleHalf != lastBleHalf ||
      tempBand != lastTempBand || sdSnap != lastSdSnap ||
      wardGpsIcon != lastWardGpsIcon ||
      (wardGpsIcon && wardBlinkPhase != lastWardBlinkPhase) || forceUpdate) {
    int barHeight = 20;
    int x = 7;
    int y = 4;
    const int rightShift =
        display_width(&(tft)) > 240 ? (display_width(&(tft)) - 240) : 0;

    display_fill_rect(&(tft), 0, 0, display_width(&(tft)), barHeight, UI_LABLE);

    display_draw_round_rect(&(tft), x, y, 22, 10, 2, TFT_WHITE);
    display_fill_rect(&(tft), x + 22, y + 3, 2, 4, TFT_WHITE);

    int batteryLevelWidth = sys_scale(batteryPercentage, 0, 100, 0, 20);
    uint16_t batteryColor = (batteryPercentage > 20) ? TFT_GREEN : TFT_RED;
    display_fill_round_rect(&(tft), x + 2, y + 2, batteryLevelWidth, 6, 1,
                            batteryColor);

    display_set_cursor(&(tft), x + 30, y + 2);
    display_set_text_color_2(&(tft), TFT_GREEN, UI_LABLE);
    display_set_text_font(&(tft), 1);
    display_set_text_size(&(tft), 1);
    display_print(&(tft),
                  ((batteryVoltage > 0.5f
                        ? text_join_cstr((TEXT(batteryPercentage)).data, "%")
                        : TEXT("--")))
                      .data);

    enum { iconW = 16 };
    enum { gap = 3 };
    enum { wifiBarsWidth = 24 };

    const int bleIconX = 130 + rightShift;
    const int bleTextX = bleIconX + iconW + gap;
    const int wifiBarsX = bleTextX + 12 + gap;
    const int tempIconX = wifiBarsX + wifiBarsWidth + gap;
    const int sdIconX = tempIconX + iconW + gap;
    int iconY = y - 2;

    int clearWidth = display_width(&(tft)) - bleIconX;
    display_fill_rect(&(tft), bleIconX, 0, clearWidth, barHeight, UI_LABLE);

    const int bleDrawX = bleIconX + 25;
    if (wardSatVisible) {
      const int wardGpsX = bleDrawX - gap - iconW;
      display_draw_bitmap(&(tft), wardGpsX, iconY, bitmap_icon_satellite, iconW,
                          iconW, TFT_ORANGE);
    }

    uint16_t wifiColor = (wifiDevices > 0) ? TFT_GREEN : TFT_WHITE;
    uint16_t bleColor = (bleDevices > 0) ? TFT_CYAN : TFT_WHITE;

    int wifiStrength = 0;
    if (wifiDevices > 0) {
      wifiStrength = DIV_CLAMP(sys_scale(wifiDevices, 0, 15, 0, 100), 0, 100);
    }

    int wifiX = wifiBarsX + 10;
    int wifiY = y + 11;

    for (int i = 0; i < 4; i++) {
      const int sigBarH = (i + 1) * 3;
      enum { barWidth = 4 };
      const int barX = wifiX + i * 6;

      if (wifiStrength > i * 25) {
        display_fill_round_rect(&(tft), barX, wifiY - sigBarH, barWidth,
                                sigBarH, 1, TFT_GREEN);
      } else {
        display_draw_round_rect(&(tft), barX, wifiY - sigBarH, barWidth,
                                sigBarH, 1, TFT_WHITE);
      }
    }

    display_draw_bitmap(&(tft), bleIconX + 25, iconY, bitmap_icon_ble, 16, 16,
                        bleColor);
    display_set_text_color_2(&(tft), bleColor, UI_LABLE);

    if (internalTemp == 53.33f) {
      display_draw_bitmap(&(tft), tempIconX + 10, y - 2, bitmap_icon_temp, 16,
                          16, TFT_YELLOW);
    } else if (internalTemp > 55) {
      display_draw_bitmap(&(tft), tempIconX + 10, y - 2, bitmap_icon_temp, 16,
                          16, TFT_RED);
    } else {
      display_draw_bitmap(&(tft), tempIconX + 10, y - 2, bitmap_icon_temp, 16,
                          16, TFT_GREEN);
    }

    if (sdCardPresent) {
      display_draw_bitmap(&(tft), sdIconX + 10, y - 2, bitmap_icon_sdcard, 16,
                          16, TFT_GREEN);
    } else {
      display_draw_bitmap(&(tft), sdIconX + 10, y - 2, bitmap_icon_nullsdcard,
                          16, 16, TFT_RED);
    }

    if (bottomSeparator) {
      display_draw_fast_h_line(&(tft), 0, barHeight - 1, display_width(&(tft)),
                               TFT_WHITE);
    }

    lastBatteryPercentage = batteryPercentage;
    lastWifiHalf = wifiHalf;
    lastBleHalf = bleHalf;
    lastTempBand = tempBand;
    lastSdSnap = sdSnap;
    lastWardGpsIcon = wardGpsIcon;
    lastWardBlinkPhase = wardGpsIcon ? wardBlinkPhase : 0u;
  }
}

void pauseBackgroundRadioTasks(void) {
  WifiScan_pauseBackgroundScanner();
  BleScan_pauseBackgroundScanner();
  GpsWardriver_stopBackgroundIfRunning();
}

void updateStatusBar(void) {
  const unsigned long currentMillis = sys_monotonic_ms();
  const bool wardOn = GpsWardriver_statusBarGpsIconActive();
  static uint32_t s_noTaskLastWardPhase = UINT32_MAX;
  const uint32_t wardPh =
      wardOn ? (currentMillis / kStatusBarWardBlinkHalfMs) : 0u;
  const bool wardBlinkTick = wardOn && (s_noTaskLastWardPhase == UINT32_MAX ||
                                        wardPh != s_noTaskLastWardPhase);
  if (wardOn) {
    s_noTaskLastWardPhase = wardPh;
  } else {
    s_noTaskLastWardPhase = UINT32_MAX;
  }

  if (statusBarDirty ||
      currentMillis - lastStatusBarUpdate > STATUS_BAR_UPDATE_INTERVAL ||
      wardBlinkTick) {
    float batteryVoltage = readBatteryVoltage();
    updateSdCardStatus();
    currentBatteryVoltage = batteryVoltage;
    drawStatusBar(batteryVoltage, false, false);
    lastBatteryVoltage = batteryVoltage;
    lastStatusBarUpdate = currentMillis;
    statusBarDirty = false;
  }
}

bool sdMountChipSelect(uint8_t cs) { return storage_mount_card(cs); }

void initSDCard(void) {
#ifdef SD_CD
  sys_configure_gpio_pull(SD_CD, GPIO_MODE_INPUT, GPIO_PULLUP_ENABLE);
#endif

  updateSdCardStatus();
  isSDCardAvailable();
}

/** GPIO CS that last mounted SD (-1 = unknown). Tried first to avoid long
 * storage::mount_card on wrong CS. */
static int8_t s_sdLastGoodCs = -1;

static bool sdPinIsOkForSdMount(uint8_t pin) {
#if defined(SD_CS)
  if (pin == SD_CS) {
    return true;
  }
#endif
#if defined(SD_CS_PIN)
  if (pin == SD_CS_PIN) {
    return true;
  }
#endif
  return false;
}

static void sdAddPinUnique(uint8_t* pins, int* nTry, uint8_t p) {
  if (!sdPinIsOkForSdMount(p)) {
    return;
  }
  for (int j = 0; j < *nTry; j++) {
    if (pins[j] == p) {
      return;
    }
  }
  if (*nTry < 3) {
    pins[(*nTry)++] = p;
  }
}

/** SPI must already be SD wiring. Returns true if storage::mount_card
 * succeeded. */
static bool sdTryBeginOrder(void) {
  uint8_t tryPins[3];
  int nTry = 0;
  if (s_sdLastGoodCs >= 0) {
    sdAddPinUnique(tryPins, &nTry, (uint8_t)s_sdLastGoodCs);
  }
#if defined(SD_CS)
  sdAddPinUnique(tryPins, &nTry, SD_CS);
#endif
#if defined(SD_CS_PIN)
  sdAddPinUnique(tryPins, &nTry, SD_CS_PIN);
#endif
  for (int i = 0; i < nTry; i++) {
    if (sdMountChipSelect(tryPins[i])) {
      s_sdLastGoodCs = (int8_t)tryPins[i];
      return true;
    }
  }
  s_sdLastGoodCs = -1;
  return false;
}

bool isSDCardAvailable(void) {
  updateSdCardStatus();
  if (!sdCardPresent && SD_CD_RELIABLE) return false;

  if (s_sdMounted) {
    if (storage_exists("/")) return true;
    s_sdMounted = false;
  }

  const uint32_t now = sys_monotonic_ms();
  if (s_sdNextMountAttemptMs != 0 &&
      (int32_t)(now - s_sdNextMountAttemptMs) < 0) {
    return false;
  }

#ifdef SD_SCLK
#ifdef SD_MISO
#endif
#endif

  if (sdTryBeginOrder()) {
    s_sdMounted = true;
    sdCardPresent = true;
    lastSdCardState = true;
    s_sdNextMountAttemptMs = 0;
    requestStatusBarRedraw();
    return true;
  }
  s_sdMounted = false;
  s_sdNextMountAttemptMs = now + kSdMountRetryDelayMs;
  return false;
}

void refreshSdMount(void) {
  if (storage_exists("/")) {
    s_sdMounted = true;
    return;
  }
  storage_unmount();
  s_sdMounted = false;
  if (isSDCardAvailable()) {
    s_sdMounted = true;
  }
}

void loading(int frameDelay, uint16_t color, int16_t x, int16_t y, int repeats,
             bool center) {
  const int actualFrameDelay = (frameDelay > 60) ? 60 : frameDelay;
  int16_t bitmapWidth = 100;
  int16_t bitmapHeight = 120;
  int16_t logoX = x;
  int16_t logoY = y;

  if (center) {
    int16_t screenWidth = display_width(&(tft));
    int16_t screenHeight = display_height(&(tft));
    logoX = (screenWidth - bitmapWidth) / 2;
    logoY = (screenHeight - bitmapHeight) / 2;
  }

  const unsigned char* bitmaps[] = {
      bitmap_icon_skull_loading_1, bitmap_icon_skull_loading_2,
      bitmap_icon_skull_loading_3, bitmap_icon_skull_loading_4,
      bitmap_icon_skull_loading_5, bitmap_icon_skull_loading_6,
      bitmap_icon_skull_loading_7, bitmap_icon_skull_loading_8,
      bitmap_icon_skull_loading_9, bitmap_icon_skull_loading_10};
  enum { numFrames = 10 };

  sprite_t frame = {.parent_ = &tft,
                    .colorDepth_ = 16,
                    .textFg_ = TFT_WHITE,
                    .textBg_ = TFT_BLACK,
                    .textSize_ = 1,
                    .textFont_ = 1};
  const bool useSprite =
      sprite_create_sprite(&(frame), bitmapWidth, bitmapHeight);
  if (useSprite) {
    sprite_set_color_depth(&(frame), 16);
  }

  for (int r = 0; r < repeats; r++) {
    for (int i = 0; i < numFrames; i++) {
      if (useSprite) {
        sprite_fill_screen(&(frame), TFT_BLACK);
        sprite_draw_bitmap(&(frame), 0, 0, bitmaps[i], bitmapWidth,
                           bitmapHeight, color);
        sprite_push_sprite(&(frame), logoX, logoY);
      } else {
        display_fill_rect(&(tft), logoX, logoY, bitmapWidth, bitmapHeight,
                          TFT_BLACK);
        display_draw_bitmap(&(tft), logoX, logoY, bitmaps[i], bitmapWidth,
                            bitmapHeight, color);
      }
      sys_sleep_ms(actualFrameDelay);
    }
  }
}

void displayLogo(uint16_t color, int displayTime) {
  int16_t bitmapWidth = 150;
  int16_t bitmapHeight = 150;
  int16_t screenWidth = display_width(&(tft));
  int16_t screenHeight = display_height(&(tft));
  int16_t logoX = (screenWidth - bitmapWidth) / 2;
  int16_t logoY = (screenHeight - bitmapHeight) / 2 - 20;

  const int16_t textY = logoY + bitmapHeight + 10;
  const int16_t textBlockH = 82;
  const int16_t fullW = bitmapWidth;
  const int16_t fullH = bitmapHeight + 10 + textBlockH;
  const int16_t fullX = logoX;
  const int16_t fullY = logoY;

  sprite_t screen = {.parent_ = &tft,
                     .colorDepth_ = 16,
                     .textFg_ = TFT_WHITE,
                     .textBg_ = TFT_BLACK,
                     .textSize_ = 1,
                     .textFont_ = 1};
  const bool useSprite = sprite_create_sprite(&(screen), fullW, fullH);
  if (useSprite) {
    sprite_set_color_depth(&(screen), 16);
    sprite_fill_screen(&(screen), TFT_BLACK);
    sprite_draw_bitmap(&(screen), 0, 0, bitmap_icon_cifer, bitmapWidth,
                       bitmapHeight, color);
    sprite_set_text_color_2(&(screen), color, TFT_BLACK);
    sprite_set_text_font(&(screen), 1);

    char pnBuf[96];
    char dnBuf[96];
    decodeObfTo(pnBuf, sizeof(pnBuf), OBF_PN, sizeof(OBF_PN), k0());
    decodeObfTo(dnBuf, sizeof(dnBuf), OBF_DN, sizeof(OBF_DN), k0());

    sprite_set_text_size(&(screen), 1);
    int16_t textX = ((int16_t)(screenWidth / 3.5)) - fullX;
    sprite_draw_string(&(screen), pnBuf, textX, bitmapHeight + 10, 1);

    sprite_set_text_size(&(screen), 1);
    textX = ((int16_t)(screenWidth / 3.5)) - fullX;
    sprite_draw_string(&(screen), "by ", textX, bitmapHeight + 30, 1);
    sprite_draw_string(&(screen), dnBuf, textX + 18, bitmapHeight + 30, 1);

    textX = ((int16_t)(screenWidth / 2.5)) - fullX;
    sprite_draw_string(&(screen), ESP32DIV_VERSION, textX, bitmapHeight + 80,
                       1);
    sprite_push_sprite(&(screen), fullX, fullY);
  } else {
    display_fill_rect(&(tft), logoX, logoY, bitmapWidth, bitmapHeight,
                      TFT_BLACK);
    display_draw_bitmap(&(tft), logoX, logoY, bitmap_icon_cifer, bitmapWidth,
                        bitmapHeight, color);

    display_set_text_color_1(&(tft), color);
    display_set_text_font(&(tft), 1);

    display_set_text_size(&(tft), 1);
    int16_t textX = screenWidth / 3.5;
    display_set_cursor(&(tft), textX, textY);
    tftPrintObf(OBF_PN, sizeof(OBF_PN), k0());

    display_set_text_size(&(tft), 1);
    textX = screenWidth / 3.5;
    int16_t textY2 = textY + 20;
    display_set_cursor(&(tft), textX, textY2);
    display_print(&(tft), "by ");
    tftPrintObf(OBF_DN, sizeof(OBF_DN), k0());

    textX = screenWidth / 2.5;
    textY2 += 50;
    display_set_cursor(&(tft), textX, textY2);
    // Version is intentionally NOT obfuscated.
    display_print(&(tft), (TEXT(ESP32DIV_VERSION)).data);
  }

  div_console_line(TEXT("=================================="));
  serialPrintObf(OBF_PN, sizeof(OBF_PN), true, k0());
  div_console_write(TEXT("Developed by: "));
  serialPrintObf(OBF_DN, sizeof(OBF_DN), true, k0());
  // Version is intentionally NOT obfuscated.
  div_console_write(TEXT("Version:      "));
  div_console_line(TEXT(ESP32DIV_VERSION));
  div_console_write(TEXT("Contact:      "));
  serialPrintObf(OBF_EM, sizeof(OBF_EM), true, k0());
  div_console_write(TEXT("GitHub:       "));
  serialPrintObf(OBF_GH, sizeof(OBF_GH), true, k0());
  div_console_write(TEXT("Website:      "));
  serialPrintObf(OBF_WB, sizeof(OBF_WB), true, k0());
  div_console_line(TEXT("=================================="));

  sys_sleep_ms(displayTime);
}
