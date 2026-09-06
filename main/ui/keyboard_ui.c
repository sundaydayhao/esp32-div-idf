#include "keyboard_ui.h"

#include "touch_ui.h"
#include "ui_utils.h"

#ifndef FEATURE_TEXT
#define FEATURE_TEXT ORANGE
#endif

static inline uint16_t KB_BG(void) { return FEATURE_BG; }
static inline uint16_t KB_SURF(void) { return UI_FG; }
static inline uint16_t KB_BORDER(void) { return FEATURE_TEXT; }
static inline uint16_t KB_TEXT(void) { return WHITE; }
static inline uint16_t KB_WARN(void) { return UI_WARN; }

enum { INPUT_X = 10 };
enum { INPUT_Y = 55 };
enum { INPUT_W = 220 };
enum { INPUT_H = 25 };

enum { KEY_W = 22 };
enum { KEY_H = 22 };
enum { KEY_SPACING = 2 };
enum { KEY_START_X = 1 };
enum { KEY_START_Y = 95 };

const unsigned long CURSOR_BLINK_MS = 500;

/* Row 3 ends with ^ (caps); row 4 ends with # (symbols) and < (backspace). */
static const char* const kStdRowsLower[OS_KEYBOARD_ROW_COUNT] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjk ^",
    "zxcvbnm#<",
};

static const char* const kStdRowsUpper[OS_KEYBOARD_ROW_COUNT] = {
    "1234567890",
    "QWERTYUIOP",
    "ASDFGHJK ^",
    "ZXCVBNM#<",
};

static const char* const kStdRowsSymbol[OS_KEYBOARD_ROW_COUNT] = {
    "1234567890",
    "!@#$%^&*()",
    "[]\\;':, ^",
    "(){}[]=+#<",
};

static const char* const* kbStdRowsFor(bool upperCase, bool symbolMode) {
  if (symbolMode) return kStdRowsSymbol;
  if (upperCase) return kStdRowsUpper;
  return kStdRowsLower;
}

static bool kbIsCapsKey(uint8_t row, size_t col, size_t rowLen, char c) {
  return c == '^' && row == 2 && col + 1 == rowLen;
}

static bool kbIsSymbolKey(uint8_t row, size_t col, size_t rowLen, char c) {
  return c == '#' && row == 3 && col + 2 == rowLen;
}

static bool kbIsBackspaceKey(uint8_t row, size_t col, size_t rowLen, char c) {
  return c == '<' && row == 3 && col + 1 == rowLen;
}

static bool kbIsModifierKeyAt(uint8_t row, size_t col, size_t rowLen, char c) {
  return kbIsCapsKey(row, col, rowLen, c) || kbIsSymbolKey(row, col, rowLen, c);
}

static uint16_t kbKeyFill(char c, bool upperCase, bool symbolMode, uint8_t row,
                          size_t col, size_t rowLen) {
  if (kbIsCapsKey(row, col, rowLen, c) && upperCase && !symbolMode)
    return KB_BORDER();
  if (kbIsSymbolKey(row, col, rowLen, c) && symbolMode) return KB_BORDER();
  return KB_SURF();
}

static void kbDrawKeyLabel(char c, int x, int y, bool upperCase,
                           bool symbolMode, uint8_t row, size_t col,
                           size_t rowLen) {
  if (kbIsCapsKey(row, col, rowLen, c)) {
    display_print(&(tft), (text_character('^')).data);
    return;
  }
  if (kbIsSymbolKey(row, col, rowLen, c)) {
    display_print(&(tft), (text_character('#')).data);
    return;
  }
  if (c == ' ') {
    display_draw_rect(&(tft), x + 4, y + KEY_H - 6, KEY_W - 8, 2, KB_TEXT());
    return;
  }
  (void)upperCase;
  (void)symbolMode;
  display_print(&(tft), (TEXT(c)).data);
}

static uint16_t kbKeyTextColor(uint16_t fill) {
  return (fill == KB_BORDER()) ? KB_BG() : KB_TEXT();
}

static bool readKeyboardTouch(int* x, int* y) {
  return readTouchXY(&((*x)), &((*y)));
}

/** Wardriver / FeatureUI expect font 1 + size 1; keyboard uses size 2 for
 * input. */
static void restoreTftAfterOsKeyboard(void) {
  display_set_text_size(&(tft), 1);
  display_set_text_font(&(tft), 1);
  display_set_text_datum(&(tft), TL_DATUM);
}

static void dismissKeyboardTouch(void) {
  sys_sleep_ms(100);
  for (int i = 0; i < 80; ++i) {
    int tx = 0;
    int ty = 0;
    if (!readTouchXY(&(tx), &(ty))) {
      break;
    }
    sys_sleep_ms(10);
  }
  resetTouchNavHeldState();
}

static text_t kbInputVisibleText(text_t value, bool cursorOn, int maxPixelW) {
  text_t tail = value;
  if (cursorOn) {
    text_append_cstr(&(tail), "|");
  }
  display_set_text_font(&(tft), 1);
  display_set_text_size(&(tft), 1);
  while (strlen((tail).data) > 1 &&
         (int)display_text_width(&(tft), (tail).data, 0) > maxPixelW) {
    text_erase(&(tail), 0, 1);
  }
  return tail;
}

void drawInputField(text_t value, bool cursorOn) {
  enum { padX = 5 };
  enum { padY = 5 };
  const int innerW = INPUT_W - padX * 2;

  display_fill_rect(&(tft), INPUT_X, INPUT_Y, INPUT_W, INPUT_H, KB_SURF());
  display_draw_rect(&(tft), INPUT_X - 1, INPUT_Y - 1, INPUT_W + 2, INPUT_H + 2,
                    KB_BORDER());
  display_set_text_color_2(&(tft), KB_TEXT(), KB_SURF());
  display_set_text_font(&(tft), 1);
  display_set_text_size(&(tft), 1);
  display_set_text_wrap(&(tft), false);
  display_set_cursor(&(tft), INPUT_X + padX, INPUT_Y + padY);
  display_print(&(tft), (kbInputVisibleText(value, cursorOn, innerW)).data);
}

void drawTitles(const OnScreenKeyboardConfig* cfg) {
  display_set_text_color_2(&(tft), KB_BORDER(), KB_BG());
  display_set_text_size(&(tft), 1);
  if ((*cfg).titleLine1 && (*cfg).titleLine1[0]) {
    display_set_cursor(&(tft), 1, 230);
    display_print_line_cstr(&(tft), (TEXT((*cfg).titleLine1)).data);
  }
  if ((*cfg).titleLine2 && (*cfg).titleLine2[0]) {
    display_set_cursor(&(tft), 20, 245);
    display_print_line_cstr(&(tft), (TEXT((*cfg).titleLine2)).data);
  }
}

void drawKeyboardKeys(const char* const* rows, uint8_t rowCount, bool upperCase,
                      bool symbolMode) {
  int y = KEY_START_Y;
  for (uint8_t row = 0; row < rowCount; ++row) {
    int x = KEY_START_X;
    const char* rowStr = rows[row];
    size_t len = strlen(rowStr);
    for (size_t col = 0; col < len; ++col) {
      char c = rowStr[col];
      const uint16_t fill = kbKeyFill(c, upperCase, symbolMode, row, col, len);
      const uint16_t text = kbKeyTextColor(fill);

      display_fill_rect(&(tft), x, y, KEY_W, KEY_H, fill);
      display_set_text_color_2(&(tft), text, fill);
      display_set_text_size(&(tft), 1);
      display_set_cursor(&(tft), x + 6, y + 5);
      kbDrawKeyLabel(c, x, y, upperCase, symbolMode, row, col, len);
      x += KEY_W + KEY_SPACING;
    }
    y += KEY_H + KEY_SPACING;
  }
}

void drawButtons(const OnScreenKeyboardConfig* cfg) {
  const int btnY = (*cfg).buttonsY;

  display_set_text_color_2(&(tft), KB_BORDER(), KB_SURF());
  display_set_text_size(&(tft), 1);
  display_set_text_datum(&(tft), MC_DATUM);

  display_fill_round_rect(&(tft), 5, btnY, 70, 25, 4, KB_SURF());
  display_draw_round_rect(&(tft), 5, btnY, 70, 25, 4, KB_BORDER());
  display_draw_string(&(tft), (*cfg).backLabel ? (*cfg).backLabel : "Back", 40,
                      btnY + 12, 0);

  display_fill_round_rect(&(tft), 85, btnY, 70, 25, 4, KB_SURF());
  display_draw_round_rect(&(tft), 85, btnY, 70, 25, 4, KB_BORDER());
  display_draw_string(&(tft), (*cfg).middleLabel ? (*cfg).middleLabel : "", 120,
                      btnY + 12, 0);

  display_fill_round_rect(&(tft), 165, btnY, 70, 25, 4, KB_SURF());
  display_draw_round_rect(&(tft), 165, btnY, 70, 25, 4, KB_BORDER());
  display_draw_string(&(tft), (*cfg).okLabel ? (*cfg).okLabel : "OK", 200,
                      btnY + 12, 0);

  display_set_text_datum(&(tft), TL_DATUM);
}

void showEmptyError(const OnScreenKeyboardConfig* cfg) {
  if (!(*cfg).emptyErrorMsg || !(*cfg).emptyErrorMsg[0]) return;
  display_fill_rect(&(tft), INPUT_X, INPUT_Y + INPUT_H + 5, INPUT_W, 12,
                    KB_BG());
  display_set_text_color_2(&(tft), KB_WARN(), KB_BG());
  display_set_text_size(&(tft), 1);
  display_set_cursor(&(tft), INPUT_X, INPUT_Y + INPUT_H + 7);
  display_print(&(tft), (TEXT((*cfg).emptyErrorMsg)).data);
}

bool handleKeyPress(char c, text_t* text, uint8_t maxLen, bool* upperCase,
                    bool* symbolMode, uint8_t row, size_t col, size_t rowLen) {
  if (kbIsCapsKey(row, col, rowLen, c)) {
    if ((*symbolMode)) {
      (*symbolMode) = false;
    }
    (*upperCase) = !(*upperCase);
    return true;
  }
  if (kbIsSymbolKey(row, col, rowLen, c)) {
    (*symbolMode) = !(*symbolMode);
    return true;
  }
  if (kbIsBackspaceKey(row, col, rowLen, c)) {
    if (strlen(((*text)).data) > 0) {
      text_erase(&((*text)), strlen(((*text)).data) - 1, SIZE_MAX);
    }
    return true;
  }
  if (c == '-') {
    (*text) = TEXT("");
    return true;
  }
  if (c == ' ') {
    if (strlen(((*text)).data) < maxLen) {
      text_append_cstr(&((*text)), (text_character(' ')).data);
    }
    return true;
  }
  if (strlen(((*text)).data) < maxLen) {
    text_append_cstr(&((*text)), (TEXT(c)).data);
  }
  return true;
}

void osKeyboardUseStandardLayout(OnScreenKeyboardConfig* cfg) {
  (*cfg).rows = NULL;
  (*cfg).rowCount = OS_KEYBOARD_ROW_COUNT;
}

OnScreenKeyboardResult showOnScreenKeyboard(const OnScreenKeyboardConfig* cfg,
                                            text_t initial) {
  OnScreenKeyboardResult res = OnScreenKeyboardResult_INITIALIZER;
  res.text = initial;
  res.accepted = false;
  res.cancelled = false;

  const bool useStandard = ((*cfg).rows == NULL);
  bool upperCase = false;
  bool symbolMode = false;
  const char* const* activeRows =
      useStandard ? kbStdRowsFor(upperCase, symbolMode) : (*cfg).rows;
  const uint8_t activeRowCount =
      useStandard ? OS_KEYBOARD_ROW_COUNT : (*cfg).rowCount;

  display_fill_rect(&(tft), 0, 37, display_width(&(tft)),
                    display_height(&(tft)) - 37, KB_BG());

  bool cursorOn = true;
  unsigned long lastBlink = sys_monotonic_ms();

  drawTitles(&((*cfg)));
  drawInputField(res.text, cursorOn);
  drawKeyboardKeys(activeRows, activeRowCount, upperCase, symbolMode);
  drawButtons(&((*cfg)));

  uint8_t shuffleIndex = 0;

  while (true) {
    unsigned long now = sys_monotonic_ms();
    if (now - lastBlink >= CURSOR_BLINK_MS) {
      cursorOn = !cursorOn;
      drawInputField(res.text, cursorOn);
      lastBlink = now;
    }

    int tx, ty;
    if (!readKeyboardTouch(&(tx), &(ty))) {
      sys_sleep_ms(10);
      continue;
    }

    sys_sleep_ms(80);

    int y = KEY_START_Y;
    bool hitSomething = false;
    for (uint8_t row = 0; row < activeRowCount; ++row) {
      int x = KEY_START_X;
      const char* rowStr = activeRows[row];
      size_t len = strlen(rowStr);
      for (size_t col = 0; col < len; ++col) {
        if (tx >= x && tx <= x + KEY_W && ty >= y && ty <= y + KEY_H) {
          char c = rowStr[col];

          display_fill_rect(&(tft), x, y, KEY_W, KEY_H, KB_BORDER());
          display_set_text_color_2(&(tft), KB_TEXT(), KB_BORDER());
          display_set_text_size(&(tft), 1);
          display_set_cursor(&(tft), x + 6, y + 5);
          if (useStandard && kbIsModifierKeyAt(row, col, len, c)) {
            kbDrawKeyLabel(c, x, y, upperCase, symbolMode, row, col, len);
          } else if (c == ' ') {
            display_draw_rect(&(tft), x + 4, y + KEY_H - 6, KEY_W - 8, 2,
                              KB_TEXT());
          } else {
            display_print(&(tft), (TEXT(c)).data);
          }
          sys_sleep_ms(100);

          if (useStandard) {
            handleKeyPress(c, &(res.text), (*cfg).maxLen, &(upperCase),
                           &(symbolMode), row, col, len);
            activeRows = kbStdRowsFor(upperCase, symbolMode);
          } else if (c == '<') {
            if (strlen((res.text).data) > 0) {
              text_erase(&(res.text), strlen((res.text).data) - 1, SIZE_MAX);
            }
          } else if (c == '-') {
            res.text = TEXT("");
          } else if (c != ' ') {
            if (strlen((res.text).data) < (*cfg).maxLen) {
              text_append_cstr(&(res.text), (TEXT(c)).data);
            }
          } else if (strlen((res.text).data) < (*cfg).maxLen) {
            text_append_cstr(&(res.text), (text_character(' ')).data);
          }

          drawKeyboardKeys(activeRows, activeRowCount, upperCase, symbolMode);
          drawInputField(res.text, cursorOn);
          hitSomething = true;
          break;
        }
        x += KEY_W + KEY_SPACING;
      }
      if (hitSomething) break;
      y += KEY_H + KEY_SPACING;
    }

    if (hitSomething) continue;

    const int btnY = (*cfg).buttonsY;

    if (tx >= 5 && tx <= 75 && ty >= btnY && ty <= btnY + 25) {
      res.cancelled = true;
      res.accepted = false;
      dismissKeyboardTouch();
      restoreTftAfterOsKeyboard();
      return res;
    }

    if (tx >= 85 && tx <= 155 && ty >= btnY && ty <= btnY + 25) {
      if ((*cfg).enableShuffle && (*cfg).shuffleNames &&
          (*cfg).shuffleCount > 0) {
        res.text = TEXT((*cfg).shuffleNames[shuffleIndex]);
        shuffleIndex = (shuffleIndex + 1) % (*cfg).shuffleCount;
        drawInputField(res.text, cursorOn);
      } else {
        if (strlen((res.text).data) > 0) {
          text_erase(&(res.text), strlen((res.text).data) - 1, SIZE_MAX);
          drawInputField(res.text, cursorOn);
        }
      }
      continue;
    }

    if (tx >= 165 && tx <= 235 && ty >= btnY && ty <= btnY + 25) {
      if ((*cfg).requireNonEmpty && strlen((res.text).data) == 0) {
        showEmptyError(&((*cfg)));
        continue;
      }
      res.accepted = true;
      res.cancelled = false;
      dismissKeyboardTouch();
      restoreTftAfterOsKeyboard();
      return res;
    }
  }
}
