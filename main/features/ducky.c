#include "ble_service.h"
#include "c_vector.h"
#include "display.h"
#include "icon.h"
#include "pcf8574.h"
#include "shared.h"
#include "storage_file.h"
#include "touch_ui.h"
#include "ui_utils.h"

extern bool feature_exit_requested;

extern display_t tft;

#ifdef TFT_BLACK
#undef TFT_BLACK
#endif
#define TFT_BLACK FEATURE_BG

#ifndef FEATURE_TEXT
#define FEATURE_TEXT ORANGE
#endif
#ifndef FEATURE_WHITE
#define FEATURE_WHITE 0xFFFF
#endif

#ifdef TFT_WHITE
#undef TFT_WHITE
#endif
#define TFT_WHITE FEATURE_TEXT

#ifdef WHITE
#undef WHITE
#endif
#define WHITE FEATURE_WHITE

#ifdef DARK_GRAY
#undef DARK_GRAY
#endif
#define DARK_GRAY UI_FG

static const char* Ducky_DUCKY_DIR = "/ducky";
static const char* Ducky_DEV_NAME = "ESP32S3 Ducky";

#define COL_BG TFT_BLACK
#define COL_FG TFT_WHITE
#define COL_ACCENT FEATURE_TEXT
#define COL_WARN UI_WARN
#define COL_ROWSEL L_Dark

enum { Ducky_HEADER_H = 20 };

static const int Ducky_TEXT_HEADER_H = Ducky_HEADER_H;
static const int Ducky_FOOTER_H = FeatureUI_FOOTER_H;
static const int Ducky_BTN_H = FeatureUI_BTN_H;

enum { Ducky_LIST_ROW_H = 40 };

enum { Ducky_LIST_ROW_GAP = 6 };
static const int Ducky_PADDING = FeatureUI_PAD_X;

enum { Ducky_TOOLBAR_Y = 37 };

static int Ducky_duckyFooterReserve(void) {
  return featureHasTouchNavBar() ? touchNavReservedHeight() : Ducky_FOOTER_H;
}

static int Ducky_duckyToastTop(void) {
  return DISPLAY_HEIGHT - Ducky_duckyFooterReserve() - 28;
}

static int Ducky_duckyToastCenterY(void) {
  return DISPLAY_HEIGHT - Ducky_duckyFooterReserve() - 24;
}

static bool Ducky_sd_mounted = false;

#ifndef SD_CD_RELIABLE
#define SD_CD_RELIABLE 1
#endif
static bool Ducky_ui_inited = false;
static bool Ducky_ui_busy = false;
static bool Ducky_needsRedraw = false;

static bool Ducky_ducky_active = false;

typedef struct Ducky_EdgeBtn {
  uint8_t pin;
  bool last;
  uint32_t tLast;
} Ducky_EdgeBtn;
#define Ducky_EdgeBtn_INITIALIZER {0}
;

static Ducky_EdgeBtn Ducky_ebUp = {.pin = BTN_UP, .last = true},
                     Ducky_ebDown = {.pin = BTN_DOWN, .last = true},
                     Ducky_ebLeft = {.pin = BTN_LEFT, .last = true},
                     Ducky_ebRight = {.pin = BTN_RIGHT, .last = true},
                     Ducky_ebSelect = {.pin = BTN_SELECT, .last = true};

static bool Ducky_uiDrawn = false;
static unsigned long Ducky_lastAnimationTime = 0;
static int Ducky_animationState = 0;
static int Ducky_activeIcon = -1;

enum { Ducky_ICON_NUM = 2 };
static int Ducky_iconX[Ducky_ICON_NUM] = {210, 10};
static int Ducky_iconY = 20;

static const unsigned char* Ducky_icons[Ducky_ICON_NUM] = {bitmap_icon_list,
                                                           bitmap_icon_go_back};

enum { Ducky_ICON_SIZE = 16 };

static bool Ducky_pcfPressedEdge(Ducky_EdgeBtn* eb, uint16_t debounceMs) {
  bool cur = isButtonPressed((*eb).pin) ? 0 : 1;
  uint32_t now = sys_monotonic_ms();
  bool pressedEvent = (!cur && (*eb).last && (now - (*eb).tLast) > debounceMs);
  if (pressedEvent) (*eb).tLast = now;
  (*eb).last = cur;
  return pressedEvent;
}

static bool Ducky_hid;

static _Atomic bool Ducky_bleConnected = false;
static void ducky_connection_changed(bool connected) {
  Ducky_bleConnected = connected;
}

typedef struct Ducky_ScriptItem {
  text_t path, name, desc;
  uint32_t size;
} Ducky_ScriptItem;
#define Ducky_ScriptItem_INITIALIZER {.size = 0}
;
static c_vector_t Ducky_items = {0};
static int Ducky_sel = 0;

typedef enum Ducky_Page {
  Ducky_Page_List,
  Ducky_Page_Details,
  Ducky_Page_Settings
} Ducky_Page;
static Ducky_Page Ducky_page = Ducky_Page_List;

static void Ducky_duckyUpdateNavLabels(void) {
  if (!featureHasTouchNavBar()) {
    return;
  }
  if (Ducky_page == Ducky_Page_List) {
    setTouchNavLabels("Exit", "Reload", "Open", "Prev", "Next");
  } else if (Ducky_page == Ducky_Page_Details) {
    setTouchNavLabels("Back", "Delete", "Run", "List", "Run");
  } else if (Ducky_page == Ducky_Page_Settings) {
    setTouchNavLabels("Back", "Reload", "Open", "Prev", "Next");
  }
  redrawTouchButtonBar();
}

typedef enum Ducky_Dialog {
  Ducky_Dialog_None,
  Ducky_Dialog_ConfirmDelete
} Ducky_Dialog;
static Ducky_Dialog Ducky_dialog = Ducky_Dialog_None;

static uint32_t Ducky_defaultDelay = 0;
static text_t Ducky_lastError = {0};

static text_t Ducky_trimCR(text_t s) {
  text_t x = s;
  if (text_suffix_cstr((x).data, "\r"))
    text_erase(&(x), strlen((x).data) - 1, SIZE_MAX);
  return x;
}
static void Ducky_clampSel(void) {
  int last = (int)(Ducky_items).size - 1;
  if (last < 0) last = 0;
  if (Ducky_sel > last) Ducky_sel = last;
  if (Ducky_sel < 0) Ducky_sel = 0;
}
static text_t Ducky_fitText(text_t s, int pixWidth, int font) {
  text_t out = s;
  while (display_text_width(&(tft), (out).data, font) > pixWidth &&
         strlen((out).data) > 1) {
    text_erase(&(out), strlen((out).data) - 1, SIZE_MAX);
  }
  if (strlen((out).data) < strlen((s).data) &&
      pixWidth >= display_text_width(&(tft), "...", font) + 4)
    text_append_cstr(&(out), "...");
  return out;
}

static int Ducky_wrapLineNoEllipsis(text_t s, int pixWidth, int font,
                                    text_t* outLine) {
  if (strlen((s).data) == 0) {
    (*outLine) = TEXT("");
    return 0;
  }

  int nl = text_find_index(s, text_character('\n'), 0);
  if (nl >= 0) {
    text_t pre = text_slice(s, 0, nl);
    if (display_text_width(&(tft), (pre).data, font) <= pixWidth) {
      (*outLine) = pre;
      text_trim(&((*outLine)));
      return nl + 1;
    }
  }

  int best = 0;
  int len = (int)strlen((s).data);
  for (int i = 1; i <= len; i++) {
    text_t sub = text_slice(s, 0, i);
    if (display_text_width(&(tft), (sub).data, font) <= pixWidth)
      best = i;
    else
      break;
  }
  if (best <= 0) best = 1;

  int breakAt = best;
  int lastSpace = -1;
  for (int i = best - 1; i >= 0; i--) {
    if ((s).data[i] == ' ') {
      lastSpace = i;
      break;
    }
  }
  if (lastSpace > 0) {
    const int tail = best - lastSpace;

    if (tail <= 12) breakAt = lastSpace;
  }
  if (breakAt <= 0) breakAt = best;

  (*outLine) = text_slice(s, 0, breakAt);
  text_trim(&((*outLine)));
  return breakAt;
}

typedef struct Ducky_Tap {
  bool has;
  int x, y;
  unsigned long time;
} Ducky_Tap;
#define Ducky_Tap_INITIALIZER {.has = false, .x = 0, .y = 0, .time = 0}
;

static bool Ducky_touching = false;
static uint32_t Ducky_touchDownMs = 0;
static int Ducky_lastX = 0, Ducky_lastY = 0;
enum { Ducky_TOUCH_DEBOUNCE_MS = 120 };
enum { Ducky_DOUBLE_TAP_MS = 300 };

static Ducky_Tap Ducky_readTap(void) {
  Ducky_Tap t = Ducky_Tap_INITIALIZER;
  int x, y;
  bool now = readTouchXY(&(x), &(y));
  uint32_t ms = sys_monotonic_ms();

  if (!Ducky_touching && now) {
    Ducky_touching = true;
    Ducky_touchDownMs = ms;
    Ducky_lastX = x;
    Ducky_lastY = y;
  } else if (Ducky_touching && now) {
    Ducky_lastX = x;
    Ducky_lastY = y;
  } else if (Ducky_touching && !now) {
    Ducky_touching = false;
    if (ms - Ducky_touchDownMs >= 20 && ms - Ducky_touchDownMs <= 800) {
      t.has = true;
      t.x = Ducky_lastX;
      t.y = Ducky_lastY;
      t.time = ms;
    }
  }
  return t;
}

static uint32_t Ducky_lastTapTime = 0;
static int Ducky_lastTapX = 0, Ducky_lastTapY = 0;

static bool Ducky_isDoubleTap(const Ducky_Tap* tap) {
  if (!(*tap).has) return false;

  uint32_t timeDiff = (*tap).time - Ducky_lastTapTime;
  int dist = abs((*tap).x - Ducky_lastTapX) + abs((*tap).y - Ducky_lastTapY);

  Ducky_lastTapTime = (*tap).time;
  Ducky_lastTapX = (*tap).x;
  Ducky_lastTapY = (*tap).y;

  return (timeDiff < Ducky_DOUBLE_TAP_MS && dist < 50);
}

static void Ducky_typeASCII(const char* s, uint16_t charDown);
static bool Ducky_execLine(text_t raw, uint32_t* interDelay, text_t* err);
static bool Ducky_execFullLine(char* raw, uint32_t* delay, text_t* error) {
  while (*raw && isspace((unsigned char)*raw)) ++raw;
  size_t length = strlen(raw);
  while (length && isspace((unsigned char)raw[length - 1])) raw[--length] = 0;
  if (length < TEXT_CAPACITY) return Ducky_execLine(TEXT(raw), delay, error);
  if (!strncmp(raw, "REM", 3)) return true;
  const char* rest = raw;
  while (*rest && !isspace((unsigned char)*rest)) ++rest;
  size_t command_length = rest - raw;
  while (*rest && isspace((unsigned char)*rest)) ++rest;
  if (command_length == 6 && !strncasecmp(raw, "STRING", 6)) {
    Ducky_typeASCII(rest, 10);
    if (Ducky_defaultDelay) sys_sleep_ms(Ducky_defaultDelay);
    return true;
  }
  *error = TEXT("Command too long");
  return false;
}

static bool Ducky_execFile(text_t path);
static void Ducky_drawListPage(bool forceReload);
static void Ducky_updateListSelection(int newSel);
static void Ducky_drawDetailsPage(void);
static void Ducky_drawSettingsPage(void);

static void Ducky_drawIconHeader(void) {
  if (!Ducky_uiDrawn) {
    display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
    display_fill_rect(&(tft), 0, 20, 240, 16, DARK_GRAY);

    for (int i = 0; i < Ducky_ICON_NUM; i++) {
      if (Ducky_icons[i] != NULL) {
        display_draw_bitmap(&(tft), Ducky_iconX[i], Ducky_iconY, Ducky_icons[i],
                            Ducky_ICON_SIZE, Ducky_ICON_SIZE, TFT_WHITE);
      }
    }
    display_draw_fast_h_line(&(tft), 0, 36, 240, UI_LINE);
    Ducky_uiDrawn = true;
  }
}

static void Ducky_handleIconTouch(int x, int y) {
  const int y0 = Ducky_iconY - 4;
  const int y1 = Ducky_iconY + Ducky_ICON_SIZE + 4;
  if (y >= y0 && y <= y1) {
    for (int i = 0; i < Ducky_ICON_NUM; i++) {
      const int x0 = Ducky_iconX[i] - 6;
      const int x1 = Ducky_iconX[i] + Ducky_ICON_SIZE + 6;
      if (x >= x0 && x <= x1) {
        if (Ducky_icons[i] != NULL && Ducky_animationState == 0) {
          if (i == 1) {
            feature_exit_requested = true;
            break;
          }

          if (i == 0) {
            if (Ducky_page != Ducky_Page_Settings) {
              Ducky_page = Ducky_Page_Settings;
              Ducky_drawSettingsPage();
            } else {
              Ducky_page = Ducky_Page_List;
              Ducky_drawListPage(false);
            }

            display_draw_bitmap(&(tft), Ducky_iconX[i], Ducky_iconY,
                                Ducky_icons[i], Ducky_ICON_SIZE,
                                Ducky_ICON_SIZE, TFT_BLACK);
            Ducky_animationState = 1;
            Ducky_activeIcon = i;
            Ducky_lastAnimationTime = sys_monotonic_ms();
          }
        }
        break;
      }
    }
  }
}

static void Ducky_updateIconAnimation(void) {
  if (Ducky_animationState > 0 &&
      sys_monotonic_ms() - Ducky_lastAnimationTime >= 150) {
    if (Ducky_animationState == 1) {
      display_draw_bitmap(&(tft), Ducky_iconX[Ducky_activeIcon], Ducky_iconY,
                          Ducky_icons[Ducky_activeIcon], Ducky_ICON_SIZE,
                          Ducky_ICON_SIZE, TFT_WHITE);
      Ducky_animationState = 2;
    } else if (Ducky_animationState == 2) {
      Ducky_animationState = 0;
      Ducky_activeIcon = -1;
    }
    Ducky_lastAnimationTime = sys_monotonic_ms();
  }
}

static bool Ducky_mountSD(void) {
  if (Ducky_sd_mounted && storage_exists("/")) return true;
  Ducky_sd_mounted = isSDCardAvailable();
  return Ducky_sd_mounted;
}
static bool Ducky_ensureDuckyDir(void) {
  if (!Ducky_mountSD()) return false;
  if (!storage_exists(Ducky_DUCKY_DIR)) return storage_mkdir(Ducky_DUCKY_DIR);
  return true;
}
static void Ducky_parseMeta(storage_file_t* f, Ducky_ScriptItem* it) {
  (*it).desc = TEXT("");
  storage_file_seek(&((*f)), 0);
  for (int i = 0; i < 10 && storage_file_available(&((*f))); ++i) {
    text_t line = Ducky_trimCR(storage_file_read_line(&((*f)), '\n'));
    if (!text_prefix_cstr((line).data, "REM")) continue;
    text_t body = text_slice(line, 3, SIZE_MAX);
    text_trim(&(body));
    if (text_prefix_cstr((body).data, "Name:")) {
      (*it).name = text_slice(body, 5, SIZE_MAX);
      text_trim(&((*it).name));
    } else if (text_prefix_cstr((body).data, "Description:")) {
      (*it).desc = text_slice(body, 12, SIZE_MAX);
      text_trim(&((*it).desc));
    }
  }
  if (!strlen(((*it).name).data)) {
    text_t fn = text_slice(
        (*it).path, text_rfind_index((*it).path, text_character('/')) + 1,
        SIZE_MAX);
    int dot = text_rfind_index(fn, text_character('.'));
    if (dot > 0) text_erase(&(fn), dot, SIZE_MAX);
    (*it).name = fn;
  }
}
static void Ducky_reloadList(void) {
  c_vector_free(&(Ducky_items));
  if (!Ducky_ensureDuckyDir()) return;
  storage_file_t d __attribute__((cleanup(storage_file_close))) =
      storage_file_open(Ducky_DUCKY_DIR, "rb");
  if (!storage_file_valid(&(d))) return;
  for (;;) {
    storage_file_t f __attribute__((cleanup(storage_file_close))) =
        storage_file_next(&(d));
    if (!storage_file_valid(&(f))) break;
    if (((f).directory != NULL)) {
      storage_file_close(&(f));
      continue;
    }
    text_t p =
        text_join_cstr((text_join_cstr((TEXT(Ducky_DUCKY_DIR)).data, "/")).data,
                       (TEXT((f).name)).data);
    text_t ext =
        text_slice(p, text_rfind_index(p, text_character('.')) + 1, SIZE_MAX);
    text_lower(&(ext));
    if (!((strcmp((ext).data, "duck") == 0) ||
          (strcmp((ext).data, "txt") == 0))) {
      storage_file_close(&(f));
      continue;
    }
    Ducky_ScriptItem it = Ducky_ScriptItem_INITIALIZER;
    it.path = p;
    it.size = storage_file_size(&(f));
    Ducky_parseMeta(&(f), &(it));
    C_VECTOR_PUSH_AS(&(Ducky_items), Ducky_ScriptItem, it);
    storage_file_close(&(f));
  }
  storage_file_close(&(d));
  Ducky_clampSel();
}

enum {
  Ducky_KEY_A = 0x04,
  Ducky_KEY_B,
  Ducky_KEY_C,
  Ducky_KEY_D,
  Ducky_KEY_E,
  Ducky_KEY_F,
  Ducky_KEY_G,
  Ducky_KEY_H,
  Ducky_KEY_I,
  Ducky_KEY_J,
  Ducky_KEY_K,
  Ducky_KEY_L,
  Ducky_KEY_M,
  Ducky_KEY_N,
  Ducky_KEY_O,
  Ducky_KEY_P,
  Ducky_KEY_Q,
  Ducky_KEY_R,
  Ducky_KEY_S,
  Ducky_KEY_T,
  Ducky_KEY_U,
  Ducky_KEY_V,
  Ducky_KEY_W,
  Ducky_KEY_X,
  Ducky_KEY_Y,
  Ducky_KEY_Z,
  Ducky_KEY_1 = 0x1E,
  Ducky_KEY_2,
  Ducky_KEY_3,
  Ducky_KEY_4,
  Ducky_KEY_5,
  Ducky_KEY_6,
  Ducky_KEY_7,
  Ducky_KEY_8,
  Ducky_KEY_9,
  Ducky_KEY_0,
  Ducky_KEY_ENTER = 0x28,
  Ducky_KEY_ESC = 0x29,
  Ducky_KEY_BACKSPACE = 0x2A,
  Ducky_KEY_TAB = 0x2B,
  Ducky_KEY_SPACE = 0x2C,
  Ducky_KEY_MINUS = 0x2D,
  Ducky_KEY_EQUAL = 0x2E,
  Ducky_KEY_LEFTBRACE = 0x2F,
  Ducky_KEY_RIGHTBRACE = 0x30,
  Ducky_KEY_BACKSLASH = 0x31,
  Ducky_KEY_SEMICOLON = 0x33,
  Ducky_KEY_APOSTROPHE = 0x34,
  Ducky_KEY_GRAVE = 0x35,
  Ducky_KEY_COMMA = 0x36,
  Ducky_KEY_DOT = 0x37,
  Ducky_KEY_SLASH = 0x38,
  Ducky_KEY_CAPSLOCK = 0x39,
  Ducky_KEY_F1 = 0x3A,
  Ducky_KEY_F2,
  Ducky_KEY_F3,
  Ducky_KEY_F4,
  Ducky_KEY_F5,
  Ducky_KEY_F6,
  Ducky_KEY_F7,
  Ducky_KEY_F8,
  Ducky_KEY_F9,
  Ducky_KEY_F10,
  Ducky_KEY_F11,
  Ducky_KEY_F12,
  Ducky_KEY_HOME = 0x4A,
  Ducky_KEY_PAGEUP = 0x4B,
  Ducky_KEY_DELETE = 0x4C,
  Ducky_KEY_END = 0x4D,
  Ducky_KEY_PAGEDOWN = 0x4E,
  Ducky_KEY_RIGHT = 0x4F,
  Ducky_KEY_LEFT = 0x50,
  Ducky_KEY_DOWN = 0x51,
  Ducky_KEY_UP = 0x52
};
enum {
  Ducky_MOD_CTRL = 0x01,
  Ducky_MOD_SHIFT = 0x02,
  Ducky_MOD_ALT = 0x04,
  Ducky_MOD_GUI = 0x08
};
typedef struct __attribute__((packed)) Ducky_KbdReport {
  uint8_t modifiers;
  uint8_t reserved;
  uint8_t keys[6];
} Ducky_KbdReport;
#define Ducky_KbdReport_INITIALIZER {0}
;

static void Ducky_sendKey(uint8_t usage, uint8_t mods, uint16_t downMs) {
  if (!Ducky_bleConnected) return;
  Ducky_KbdReport idle = Ducky_KbdReport_INITIALIZER;
  ble_hid_send((uint8_t*)&idle, sizeof(idle));
  sys_sleep_ms(3);
  Ducky_KbdReport rpt = Ducky_KbdReport_INITIALIZER;
  rpt.modifiers = mods;
  rpt.keys[0] = usage;
  ble_hid_send((uint8_t*)&rpt, sizeof(rpt));
  sys_sleep_ms(downMs);
  ble_hid_send((uint8_t*)&idle, sizeof(idle));
  sys_sleep_ms(5);
}

static uint8_t Ducky_keyFromChar(char c, bool* needShift) {
  (*needShift) = false;
  if (c >= 'a' && c <= 'z') return Ducky_KEY_A + (c - 'a');
  if (c >= 'A' && c <= 'Z') {
    (*needShift) = true;
    return Ducky_KEY_A + (c - 'A');
  }
  if (c >= '1' && c <= '9') return Ducky_KEY_1 + (c - '1');
  if (c == '0') return Ducky_KEY_0;
  if (c == ' ') return Ducky_KEY_SPACE;
  if (c == '\n' || c == '\r') return Ducky_KEY_ENTER;
  if (c == '-') return 0x2D;
  if (c == '_') {
    (*needShift) = true;
    return 0x2D;
  }
  if (c == '=') return 0x2E;
  if (c == '+') {
    (*needShift) = true;
    return 0x2E;
  }
  if (c == '[') return 0x2F;
  if (c == '{') {
    (*needShift) = true;
    return 0x2F;
  }
  if (c == ']') return 0x30;
  if (c == '}') {
    (*needShift) = true;
    return 0x30;
  }
  if (c == '\\') return 0x31;
  if (c == '|') {
    (*needShift) = true;
    return 0x31;
  }
  if (c == ';') return 0x33;
  if (c == ':') {
    (*needShift) = true;
    return 0x33;
  }
  if (c == '\'') return 0x34;
  if (c == '"') {
    (*needShift) = true;
    return 0x34;
  }
  if (c == ',') return 0x36;
  if (c == '<') {
    (*needShift) = true;
    return 0x36;
  }
  if (c == '.') return 0x37;
  if (c == '>') {
    (*needShift) = true;
    return 0x37;
  }
  if (c == '/') return 0x38;
  if (c == '?') {
    (*needShift) = true;
    return 0x38;
  }
  if (c == '`') return 0x35;
  if (c == '~') {
    (*needShift) = true;
    return 0x35;
  }
  return 0;
}

static void Ducky_typeASCII(const char* s, uint16_t charDown) {
  for (size_t i = 0; i < strlen(s); ++i) {
    bool shift = false;
    uint8_t u = Ducky_keyFromChar(s[i], &(shift));
    if (u) Ducky_sendKey(u, shift ? Ducky_MOD_SHIFT : 0, charDown);
  }
}

static bool Ducky_resolveKeyWord(text_t kw, uint8_t* usage, uint8_t* mods) {
  text_t k = kw;
  text_upper(&(k));
  (*usage) = 0;
  (*mods) = 0;
  if ((strcmp((k).data, "ENTER") == 0) || (strcmp((k).data, "RETURN") == 0))
    (*usage) = Ducky_KEY_ENTER;
  else if ((strcmp((k).data, "ESC") == 0) || (strcmp((k).data, "ESCAPE") == 0))
    (*usage) = Ducky_KEY_ESC;
  else if ((strcmp((k).data, "TAB") == 0))
    (*usage) = Ducky_KEY_TAB;
  else if ((strcmp((k).data, "SPACE") == 0))
    (*usage) = Ducky_KEY_SPACE;
  else if ((strcmp((k).data, "BACKSPACE") == 0) ||
           (strcmp((k).data, "DELETE") == 0))
    (*usage) = Ducky_KEY_BACKSPACE;
  else if ((strcmp((k).data, "UP") == 0))
    (*usage) = Ducky_KEY_UP;
  else if ((strcmp((k).data, "DOWN") == 0))
    (*usage) = Ducky_KEY_DOWN;
  else if ((strcmp((k).data, "LEFT") == 0))
    (*usage) = Ducky_KEY_LEFT;
  else if ((strcmp((k).data, "RIGHT") == 0))
    (*usage) = Ducky_KEY_RIGHT;
  else if ((strcmp((k).data, "HOME") == 0))
    (*usage) = Ducky_KEY_HOME;
  else if ((strcmp((k).data, "END") == 0))
    (*usage) = Ducky_KEY_END;
  else if ((strcmp((k).data, "PGUP") == 0) || (strcmp((k).data, "PAGEUP") == 0))
    (*usage) = Ducky_KEY_PAGEUP;
  else if ((strcmp((k).data, "PGDN") == 0) ||
           (strcmp((k).data, "PAGEDOWN") == 0))
    (*usage) = Ducky_KEY_PAGEDOWN;
  else if (text_prefix_cstr((k).data, "F")) {
    int n = text_integer(text_slice(k, 1, SIZE_MAX));
    if (n >= 1 && n <= 12) (*usage) = Ducky_KEY_F1 + (n - 1);
  }
  if ((*usage)) return true;
  if (strlen((k).data) == 1) {
    char c = (k).data[0];
    bool sh = false;
    uint8_t u = Ducky_keyFromChar(c, &(sh));
    if (u) {
      (*usage) = u;
      (*mods) = sh ? Ducky_MOD_SHIFT : 0;
      return true;
    }
  }
  return false;
}
static uint8_t Ducky_modsFromTokens(const c_vector_t* toks, size_t* i) {
  uint8_t m = 0;
  while ((*i) < ((*toks)).size) {
    text_t t = ((text_t*)((*toks)).data)[(*i)];
    text_upper(&(t));
    if ((strcmp((t).data, "CTRL") == 0) || (strcmp((t).data, "CONTROL") == 0))
      m |= Ducky_MOD_CTRL;
    else if ((strcmp((t).data, "ALT") == 0))
      m |= Ducky_MOD_ALT;
    else if ((strcmp((t).data, "SHIFT") == 0))
      m |= Ducky_MOD_SHIFT;
    else if ((strcmp((t).data, "GUI") == 0) || (strcmp((t).data, "WIN") == 0) ||
             (strcmp((t).data, "WINDOWS") == 0) ||
             (strcmp((t).data, "CMD") == 0))
      m |= Ducky_MOD_GUI;
    else
      break;
    ++(*i);
  }
  return m;
}
static uint32_t Ducky_clampDelay(int v) {
  if (v < 0) v = 0;
  if (v > 600000) v = 600000;
  return (uint32_t)v;
}
static void Ducky_pressCombo(uint8_t usage, uint8_t mods, uint16_t downMs) {
  if (!Ducky_bleConnected) return;
  Ducky_KbdReport idle = Ducky_KbdReport_INITIALIZER;
  ble_hid_send((uint8_t*)&idle, sizeof(idle));
  sys_sleep_ms(3);
  Ducky_KbdReport rpt = Ducky_KbdReport_INITIALIZER;
  rpt.modifiers = mods;
  rpt.keys[0] = usage;
  ble_hid_send((uint8_t*)&rpt, sizeof(rpt));
  sys_sleep_ms(downMs);
  ble_hid_send((uint8_t*)&idle, sizeof(idle));
  sys_sleep_ms(5);
}
static bool Ducky_execLine(text_t raw, uint32_t* interDelay, text_t* err) {
  text_t line = raw;
  text_trim(&(line));
  if (!strlen((line).data)) return true;
  if (text_prefix_cstr((line).data, "REM")) return true;

  c_vector_t tok __attribute__((cleanup(c_vector_free))) = {0};
  {
    text_t tmp = {0};
    for (size_t i = 0; i < strlen((line).data); ++i) {
      char c = (line).data[i];
      if (c == ' ' || c == '\t') {
        if (strlen((tmp).data)) {
          C_VECTOR_PUSH_AS(&(tok), text_t, tmp);
          tmp = TEXT("");
        }
      } else
        text_append_cstr(&(tmp), (TEXT(c)).data);
    }
    if (strlen((tmp).data)) C_VECTOR_PUSH_AS(&(tok), text_t, tmp);
  }
  if (((tok).size == 0)) return true;

  text_t cmd = ((text_t*)(tok).data)[0];
  text_t rest = text_slice(line, strlen((cmd).data), SIZE_MAX);
  text_trim(&(rest));
  text_t ucmd = cmd;
  text_upper(&(ucmd));

  if ((strcmp((ucmd).data, "DELAY") == 0)) {
    (*interDelay) = Ducky_clampDelay(text_integer(rest));
    sys_sleep_ms((*interDelay));
    return true;
  }
  if ((strcmp((ucmd).data, "DEFAULT_DELAY") == 0) ||
      (strcmp((ucmd).data, "DEFAULTDELAY") == 0)) {
    Ducky_defaultDelay = Ducky_clampDelay(text_integer(rest));
    return true;
  }
  if ((strcmp((ucmd).data, "WAIT_FOR") == 0)) {
    sys_sleep_ms(Ducky_clampDelay(text_integer(rest)));
    return true;
  }
  if ((strcmp((ucmd).data, "STRING") == 0)) {
    Ducky_typeASCII(rest.data, 10);
    if (Ducky_defaultDelay) sys_sleep_ms(Ducky_defaultDelay);
    return true;
  }
  if ((strcmp((ucmd).data, "ENTER") == 0)) {
    Ducky_sendKey(Ducky_KEY_ENTER, 0, 10);
    if (Ducky_defaultDelay) sys_sleep_ms(Ducky_defaultDelay);
    return true;
  }

  if ((strcmp((ucmd).data, "GUI") == 0) ||
      (strcmp((ucmd).data, "WINDOWS") == 0) ||
      (strcmp((ucmd).data, "WIN") == 0)) {
    uint8_t usage = 0;
    if (strlen((rest).data)) {
      uint8_t m = 0;
      if (!Ducky_resolveKeyWord(rest, &(usage), &(m))) {
        (*err) = text_join_cstr("Unknown key: ", (rest).data);
        return false;
      }
    }
    if (!usage) usage = Ducky_KEY_R;
    Ducky_pressCombo(usage, Ducky_MOD_GUI, 50);
    if (Ducky_defaultDelay) sys_sleep_ms(Ducky_defaultDelay);
    return true;
  }
  if ((strcmp((ucmd).data, "CTRL") == 0) ||
      (strcmp((ucmd).data, "CONTROL") == 0) ||
      (strcmp((ucmd).data, "ALT") == 0) ||
      (strcmp((ucmd).data, "SHIFT") == 0)) {
    size_t i = 0;
    c_vector_t tokens __attribute__((cleanup(c_vector_free))) = {0};
    {
      text_t s = line;
      text_t t = {0};
      for (size_t j = 0; j < strlen((s).data); ++j) {
        char ch = (s).data[j];
        if (ch == ' ' || ch == '\t') {
          if (strlen((t).data)) {
            C_VECTOR_PUSH_AS(&(tokens), text_t, t);
            t = TEXT("");
          }
        } else
          text_append_cstr(&(t), (TEXT(ch)).data);
      }
      if (strlen((t).data)) C_VECTOR_PUSH_AS(&(tokens), text_t, t);
    }
    i = 0;
    uint8_t mods = Ducky_modsFromTokens(&(tokens), &(i));
    if (i >= (tokens).size) {
      (*err) = TEXT("Missing key after modifiers");
      return false;
    }
    uint8_t usage = 0, dummy = 0;
    if (!Ducky_resolveKeyWord(((text_t*)(tokens).data)[i], &(usage),
                              &(dummy))) {
      (*err) =
          text_join_cstr("Unknown key: ", (((text_t*)(tokens).data)[i]).data);
      return false;
    }
    Ducky_pressCombo(usage, mods, 50);
    if (Ducky_defaultDelay) sys_sleep_ms(Ducky_defaultDelay);
    return true;
  }
  if ((strcmp((ucmd).data, "TAB") == 0) || (strcmp((ucmd).data, "ESC") == 0) ||
      (strcmp((ucmd).data, "SPACE") == 0) ||
      (strcmp((ucmd).data, "BACKSPACE") == 0) ||
      (strcmp((ucmd).data, "DELETE") == 0) ||
      (strcmp((ucmd).data, "UP") == 0) || (strcmp((ucmd).data, "DOWN") == 0) ||
      (strcmp((ucmd).data, "LEFT") == 0) ||
      (strcmp((ucmd).data, "RIGHT") == 0) ||
      (strcmp((ucmd).data, "HOME") == 0) || (strcmp((ucmd).data, "END") == 0) ||
      (strcmp((ucmd).data, "PGUP") == 0) ||
      (strcmp((ucmd).data, "PAGEUP") == 0) ||
      (strcmp((ucmd).data, "PGDN") == 0) ||
      (strcmp((ucmd).data, "PAGEDOWN") == 0) ||
      (strlen((ucmd).data) == 2 && (ucmd).data[0] == 'F' &&
       isdigit((ucmd).data[1])) ||
      (strlen((ucmd).data) == 3 && (ucmd).data[0] == 'F' &&
       isdigit((ucmd).data[1]) && isdigit((ucmd).data[2]))) {
    uint8_t usage = 0, m = 0;
    if (!Ducky_resolveKeyWord(ucmd, &(usage), &(m))) {
      (*err) = text_join_cstr("Unknown key: ", (ucmd).data);
      return false;
    }
    Ducky_pressCombo(usage, m, 50);
    if (Ducky_defaultDelay) sys_sleep_ms(Ducky_defaultDelay);
    return true;
  }
  if ((strcmp((ucmd).data, "REPEAT") == 0)) {
    int n = ((int)(text_integer(rest)));
    if (n < 0) n = 0;
    while (n--) {
      Ducky_sendKey(Ducky_KEY_ENTER, 0, 10);
      if (Ducky_defaultDelay) sys_sleep_ms(Ducky_defaultDelay);
    }
    return true;
  }
  (*err) = text_join_cstr("Unknown command: ", (cmd).data);
  return false;
}

static bool Ducky_execFile(text_t path) {
  Ducky_lastError = TEXT("");
  if (!Ducky_bleConnected) {
    Ducky_lastError = TEXT("BLE not connected");
    return false;
  }
  if (!Ducky_mountSD()) {
    Ducky_lastError = TEXT("SD not mounted");
    return false;
  }
  storage_file_t f __attribute__((cleanup(storage_file_close))) =
      storage_file_open((path).data, "rb");
  if (!storage_file_valid(&(f))) {
    Ducky_lastError = TEXT("Open failed");
    return false;
  }

  Ducky_defaultDelay = 0;
  uint32_t interDelay = 0;

  const int execTop = Ducky_TOOLBAR_Y + Ducky_HEADER_H + Ducky_HEADER_H + 1;
  display_fill_rect(&(tft), 0, execTop, DISPLAY_WIDTH, DISPLAY_HEIGHT - execTop,
                    COL_BG);
  display_set_text_color_2(&(tft), COL_FG, COL_BG);
  display_draw_centre_string(&(tft), "Executing...", DISPLAY_WIDTH / 2,
                             execTop + 4, 1);
  display_draw_string(&(tft), "File:", Ducky_PADDING, execTop + 24, 1);
  display_draw_string(&(tft), (path).data, 48, execTop + 24, 1);

  if (Ducky_bleConnected) {
    Ducky_KbdReport idle = Ducky_KbdReport_INITIALIZER;
    ble_hid_send((uint8_t*)&idle, sizeof(idle));
    sys_sleep_ms(10);
  }

  int y = execTop + 44;
  const int yMax = DISPLAY_HEIGHT - 6;

  Ducky_ui_busy = true;

  char* script_line __attribute__((cleanup(c_free_pointer))) = NULL;
  size_t script_capacity = 0;
  while (storage_file_available(&(f))) {
    ptrdiff_t length = storage_file_getline(&f, &script_line, &script_capacity);
    text_t line = TEXT(length >= 0 ? script_line : "");
    text_t err = TEXT("Unable to read script line");
    if (length < 0 || !Ducky_execFullLine(script_line, &(interDelay), &(err))) {
      Ducky_lastError = err;
      storage_file_close(&(f));
      if (Ducky_bleConnected) {
        Ducky_KbdReport idle = Ducky_KbdReport_INITIALIZER;
        ble_hid_send((uint8_t*)&idle, sizeof(idle));
        sys_sleep_ms(10);
      }
      Ducky_ui_busy = false;
      return false;
    }
    if (Ducky_defaultDelay) sys_sleep_ms(Ducky_defaultDelay);

    if (y < yMax) {
      text_t disp = Ducky_fitText(line, DISPLAY_WIDTH - 2 * Ducky_PADDING, 1);
      display_draw_string(&(tft), (disp).data, Ducky_PADDING, y, 1);
      y += 18;
    }
    sys_yield_task();
  }
  storage_file_close(&(f));

  if (Ducky_bleConnected) {
    Ducky_KbdReport idle = Ducky_KbdReport_INITIALIZER;
    ble_hid_send((uint8_t*)&idle, sizeof(idle));
    sys_sleep_ms(20);
  }

  display_fill_round_rect(&(tft), Ducky_PADDING, DISPLAY_HEIGHT - 40,
                          DISPLAY_WIDTH - 2 * Ducky_PADDING, 28, 6, COL_ACCENT);
  display_set_text_color_2(&(tft), COL_BG, COL_ACCENT);
  display_draw_centre_string(&(tft), "Done!", DISPLAY_WIDTH / 2,
                             DISPLAY_HEIGHT - 36, 1);

  Ducky_ui_busy = false;
  sys_sleep_ms(500);
  return true;
}

static FeatureUI_Button Ducky_listBtns[3];
static FeatureUI_Button Ducky_detailsBtns[3];
static FeatureUI_Button Ducky_confirmBtns[2];
static FeatureUI_Button Ducky_infoBtn = FeatureUI_Button_INITIALIZER;

typedef enum Ducky_HeaderType {
  Ducky_HeaderType_TextOnly,
  Ducky_HeaderType_IconBar,
  Ducky_HeaderType_TextWithInfo
} Ducky_HeaderType;

static void Ducky_drawHeader(Ducky_HeaderType type, text_t title,
                             bool drawFooterBg) {
  int currentY = Ducky_TOOLBAR_Y;

  if (type == Ducky_HeaderType_IconBar) {
    Ducky_drawIconHeader();

    currentY = Ducky_TOOLBAR_Y;
  }

  if (type == Ducky_HeaderType_TextOnly ||
      type == Ducky_HeaderType_TextWithInfo) {
    display_fill_rect(&(tft), 0, currentY, DISPLAY_WIDTH, Ducky_TEXT_HEADER_H,
                      COL_BG);

    if (type == Ducky_HeaderType_TextOnly && !(strlen((title).data) == 0)) {
      display_set_text_color_2(&(tft), COL_FG, COL_BG);
      display_set_text_font(&(tft), 1);
      display_draw_string(&(tft), (title).data, Ducky_PADDING, currentY + 1, 1);
    }

    if (type == Ducky_HeaderType_TextWithInfo) {
      int infoY = currentY + 6;
      display_set_text_font(&(tft), 1);

      display_set_text_color_2(&(tft), Ducky_bleConnected ? UI_OK : UI_WARN,
                               COL_BG);
      const char* bleTxt =
          Ducky_bleConnected ? "BLE: Connected" : "BLE: Not connected";
      display_draw_string(&(tft), bleTxt, Ducky_PADDING, infoY, 1);

      display_set_text_color_2(&(tft), Ducky_sd_mounted ? UI_OK : UI_WARN,
                               COL_BG);
      const char* sdTxt = Ducky_sd_mounted ? "SD: OK" : "SD: Not mounted";
      int sdTw = display_text_width(&(tft), sdTxt, 1);
      display_draw_string(&(tft), sdTxt, DISPLAY_WIDTH - Ducky_PADDING - sdTw,
                          infoY, 1);
    }

    display_draw_line(&(tft), 0, currentY + Ducky_TEXT_HEADER_H, DISPLAY_WIDTH,
                      currentY + Ducky_TEXT_HEADER_H, UI_LINE);
  }

  if (drawFooterBg && type != Ducky_HeaderType_IconBar &&
      !featureHasTouchNavBar()) {
    FeatureUI_drawFooterBg();
  }
}

static uint32_t Ducky_lastStatusPollMs = 0;
static bool Ducky_lastHeaderBle = false;
static bool Ducky_lastHeaderSd = false;

static void ducky_no_sd(const char* title, const char* subtitle) {
  Ducky_sd_mounted = false;

  display_fill_rect(&(tft), 0, Ducky_TOOLBAR_Y - 1, DISPLAY_WIDTH,
                    DISPLAY_HEIGHT - (Ducky_TOOLBAR_Y - 1), COL_BG);
  display_set_text_color_2(&(tft), UI_WARN, COL_BG);
  display_set_text_font(&(tft), 1);
  display_draw_centre_string(&(tft), title, DISPLAY_WIDTH / 2,
                             DISPLAY_HEIGHT / 2 - 14, 1);
  display_set_text_font(&(tft), 1);
  display_set_text_color_2(&(tft), UI_TEXT, COL_BG);
  display_draw_centre_string(&(tft), subtitle, DISPLAY_WIDTH / 2,
                             DISPLAY_HEIGHT / 2 + 10, 1);
  sys_sleep_ms(900);

  Ducky_ducky_active = false;
  feature_exit_requested = true;
}
static void Ducky_refreshHeaderStatusIfNeeded(bool force) {
  static bool lastCardPresent = true;
  const uint32_t now = sys_monotonic_ms();
  if (force || (now - Ducky_lastStatusPollMs) > 500) {
    Ducky_lastStatusPollMs = now;
#if defined(SD_CD) && SD_CD_RELIABLE
    sys_configure_gpio_pull(SD_CD, GPIO_MODE_INPUT, GPIO_PULLUP_ENABLE);
    bool cardPresent = !gpio_get_level(((gpio_num_t)(SD_CD)));
#else
    bool cardPresent = true;
#endif

    if (lastCardPresent && !cardPresent) {
      ducky_no_sd("SD card removed", "Returning...");
      lastCardPresent = cardPresent;
      return;
    }
    lastCardPresent = cardPresent;
    if (!cardPresent) {
      Ducky_sd_mounted = false;
    } else if (cardPresent && !Ducky_sd_mounted) {
      Ducky_mountSD();
    }
  }

  if (force || Ducky_lastHeaderBle != Ducky_bleConnected ||
      Ducky_lastHeaderSd != Ducky_sd_mounted) {
    Ducky_uiDrawn = false;
    Ducky_drawIconHeader();

    Ducky_drawHeader(Ducky_HeaderType_TextWithInfo, TEXT(""), false);
    Ducky_lastHeaderBle = Ducky_bleConnected;
    Ducky_lastHeaderSd = Ducky_sd_mounted;
  }
}

static bool Ducky_listUiValid = false;
static bool Ducky_listLoadedOnce = false;
static int Ducky_lastListCount = -1;
static int Ducky_lastListStartItem = -1;
static int Ducky_lastListMaxVisible = -1;
static bool Ducky_lastListHasScrollbar = false;
static int Ducky_lastListHeaderBottom = 0;
static int Ducky_lastListHintY = 0;
static int Ducky_lastListTop = 0;
static int Ducky_lastListRowStride = 0;
static int Ducky_lastListRw = 0;
static int Ducky_lastListRh = 0;

static void Ducky_calcListLayout(int* headerBottom, int* hintY, int* listTop,
                                 int* rowStride, int* maxVisibleItems,
                                 bool* hasScrollbar, int* rw, int* rh) {
  const int barY = Ducky_TOOLBAR_Y;
  (*headerBottom) = barY + Ducky_TEXT_HEADER_H + 1;
  (*rowStride) = Ducky_LIST_ROW_H + Ducky_LIST_ROW_GAP;

  const int contentBottom = DISPLAY_HEIGHT - Ducky_duckyFooterReserve();
  enum { hintBandH = 18 };
  (*hintY) = (*headerBottom) + 6;
  const int listAreaTop = (*headerBottom) + hintBandH;
  const int listAreaH = contentBottom - listAreaTop - 4;

  (*maxVisibleItems) = listAreaH / (*rowStride);
  if ((*maxVisibleItems) < 1) (*maxVisibleItems) = 1;

  const int itemCount = (int)(Ducky_items).size;
  const int viewportH = (*maxVisibleItems) * (*rowStride) - Ducky_LIST_ROW_GAP;
  if (itemCount > 0 && itemCount < (*maxVisibleItems)) {
    const int blockH = itemCount * (*rowStride) - Ducky_LIST_ROW_GAP;
    (*listTop) = listAreaTop + (listAreaH - blockH) / 2;
  } else {
    (*listTop) = listAreaTop + (listAreaH - viewportH) / 2;
  }

  (*hasScrollbar) = (itemCount > (*maxVisibleItems));
  (*rw) = DISPLAY_WIDTH - 2 * Ducky_PADDING - ((*hasScrollbar) ? 12 : 0);
  (*rh) = Ducky_LIST_ROW_H - 2;
}

static void Ducky_calcListWindow(int maxVisibleItems, int* startItem,
                                 int* endItem) {
  (*startItem) = DIV_MAX(0, Ducky_sel - maxVisibleItems / 2);
  (*endItem) = DIV_MIN((int)(Ducky_items).size, (*startItem) + maxVisibleItems);
}

static void Ducky_drawListHints(int hintY) {
  display_fill_rect(&(tft), 0, hintY - 1, DISPLAY_WIDTH, 14, COL_BG);
  if (((Ducky_items).size == 0)) return;

  display_set_text_font(&(tft), 1);
  display_set_text_color_2(&(tft), UI_TEXT, COL_BG);

  text_t counter =
      text_join_cstr((text_join_cstr((TEXT(Ducky_sel + 1)).data, "/")).data,
                     (TEXT((int)(Ducky_items).size)).data);
  int counterWidth = display_text_width(&(tft), (counter).data, 1);
  display_draw_string(&(tft), (counter).data,
                      DISPLAY_WIDTH - counterWidth - Ducky_PADDING, hintY, 1);

  display_draw_string(&(tft), "Double-tap to execute", Ducky_PADDING, hintY, 1);
}

static void Ducky_drawListScrollbar(int listTop, int rowStride,
                                    int maxVisibleItems, int startItem) {
  if ((int)(Ducky_items).size <= maxVisibleItems) return;
  int scrollBarX = DISPLAY_WIDTH - 8;
  const int viewportH = maxVisibleItems * rowStride - Ducky_LIST_ROW_GAP;
  int scrollBarHeight = viewportH - 8;
  int scrollBarY = listTop + 4;
  int indicatorHeight =
      (maxVisibleItems * scrollBarHeight) / (int)(Ducky_items).size;
  int indicatorY =
      scrollBarY + (startItem * (scrollBarHeight - indicatorHeight)) /
                       (int)(Ducky_items).size;

  display_fill_rect(&(tft), scrollBarX, scrollBarY, 4, scrollBarHeight,
                    UI_LINE);
  display_fill_rect(&(tft), scrollBarX, indicatorY, 4, indicatorHeight,
                    COL_ACCENT);
}

static void Ducky_drawListRow(int i, int startItem, int listTop, int rowStride,
                              int rx, int rw, int rh) {
  const int maxVisibleItems = Ducky_lastListMaxVisible;
  const int endItem =
      DIV_MIN((int)(Ducky_items).size, startItem + maxVisibleItems);
  if (i < startItem || i >= endItem) return;

  int y = listTop + (i - startItem) * rowStride;
  bool selected = (i == Ducky_sel);
  uint16_t body = selected ? COL_ROWSEL : COL_BG;
  uint16_t border = selected ? COL_ACCENT : UI_LINE;

  int wedgeX = rx - 8;
  if (wedgeX < 0) wedgeX = 0;
  display_fill_rect(&(tft), wedgeX, y, rx - wedgeX, rh, COL_BG);

  display_fill_round_rect(&(tft), rx, y, rw, rh, 6, body);
  display_draw_round_rect(&(tft), rx, y, rw, rh, 6, border);

  if (selected) {
    display_fill_triangle(&(tft), rx - 6, y + rh / 2 - 4, rx - 6,
                          y + rh / 2 + 4, rx - 2, y + rh / 2, COL_ACCENT);
  }

  display_set_text_color_2(&(tft), COL_FG, body);
  display_set_text_font(&(tft), 1);
  text_t name = Ducky_fitText(((Ducky_ScriptItem*)(Ducky_items).data)[i].name,
                              rw - 40, 1);
  display_draw_string(&(tft), (name).data, rx + 12, y + 6, 1);

  uint32_t kb = (((Ducky_ScriptItem*)(Ducky_items).data)[i].size + 1023) / 1024;
  char sbuf[16];
  snprintf(sbuf, sizeof(sbuf), "%lukB", (unsigned long)kb);
  display_set_text_font(&(tft), 1);
  int sx = rx + rw - display_text_width(&(tft), sbuf, 1) - 8;
  display_set_text_color_2(&(tft), selected ? UI_OK : UI_TEXT, body);
  display_draw_string(&(tft), sbuf, sx, y + 8, 1);

  display_set_text_font(&(tft), 1);
  text_t desc = (strlen((((Ducky_ScriptItem*)(Ducky_items).data)[i].desc).data)
                     ? ((Ducky_ScriptItem*)(Ducky_items).data)[i].desc
                     : TEXT("(no description)"));
  desc = Ducky_fitText(desc, rw - 24, 1);
  display_set_text_color_2(&(tft), UI_TEXT, body);
  display_draw_string(&(tft), (desc).data, rx + 12, y + 26, 1);
}

static void Ducky_drawListContentFull(void) {
  const int rx = Ducky_PADDING;

  int headerBottom, hintY, listTop, rowStride, maxVisibleItems, rw, rh;
  bool hasScrollbar;
  Ducky_calcListLayout(&(headerBottom), &(hintY), &(listTop), &(rowStride),
                       &(maxVisibleItems), &(hasScrollbar), &(rw), &(rh));

  display_fill_rect(&(tft), 0, headerBottom, DISPLAY_WIDTH,
                    DISPLAY_HEIGHT - headerBottom - Ducky_duckyFooterReserve(),
                    COL_BG);

  int startItem, endItem;
  Ducky_calcListWindow(maxVisibleItems, &(startItem), &(endItem));

  Ducky_drawListScrollbar(listTop, rowStride, maxVisibleItems, startItem);

  int y = listTop;
  for (int i = startItem; i < endItem; ++i) {
    bool selected = (i == Ducky_sel);
    uint16_t body = selected ? COL_ROWSEL : COL_BG;
    uint16_t border = selected ? COL_ACCENT : UI_LINE;

    display_fill_round_rect(&(tft), rx, y, rw, rh, 6, body);
    display_draw_round_rect(&(tft), rx, y, rw, rh, 6, border);
    if (selected) {
      display_fill_triangle(&(tft), rx - 6, y + rh / 2 - 4, rx - 6,
                            y + rh / 2 + 4, rx - 2, y + rh / 2, COL_ACCENT);
    }

    display_set_text_color_2(&(tft), COL_FG, body);
    display_set_text_font(&(tft), 1);
    text_t name = Ducky_fitText(((Ducky_ScriptItem*)(Ducky_items).data)[i].name,
                                rw - 40, 1);
    display_draw_string(&(tft), (name).data, rx + 12, y + 6, 1);

    uint32_t kb =
        (((Ducky_ScriptItem*)(Ducky_items).data)[i].size + 1023) / 1024;
    char sbuf[16];
    snprintf(sbuf, sizeof(sbuf), "%lukB", (unsigned long)kb);
    display_set_text_font(&(tft), 1);
    int sx = rx + rw - display_text_width(&(tft), sbuf, 1) - 8;
    display_set_text_color_2(&(tft), selected ? UI_OK : UI_TEXT, body);
    display_draw_string(&(tft), sbuf, sx, y + 8, 1);

    display_set_text_font(&(tft), 1);
    text_t desc =
        (strlen((((Ducky_ScriptItem*)(Ducky_items).data)[i].desc).data)
             ? ((Ducky_ScriptItem*)(Ducky_items).data)[i].desc
             : TEXT("(no description)"));
    desc = Ducky_fitText(desc, rw - 24, 1);
    display_set_text_color_2(&(tft), UI_TEXT, body);
    display_draw_string(&(tft), (desc).data, rx + 12, y + 26, 1);

    y += rowStride;
  }

  Ducky_drawListHints(hintY);

  Ducky_listUiValid = true;
  Ducky_lastListCount = (int)(Ducky_items).size;
  Ducky_lastListStartItem = startItem;
  Ducky_lastListMaxVisible = maxVisibleItems;
  Ducky_lastListHasScrollbar = hasScrollbar;
  Ducky_lastListHeaderBottom = headerBottom;
  Ducky_lastListHintY = hintY;
  Ducky_lastListTop = listTop;
  Ducky_lastListRowStride = rowStride;
  Ducky_lastListRw = rw;
  Ducky_lastListRh = rh;
}

static void Ducky_updateListSelection(int newSel) {
  if (newSel == Ducky_sel) return;
  int oldSel = Ducky_sel;
  Ducky_sel = newSel;
  Ducky_clampSel();

  if (!Ducky_sd_mounted || ((Ducky_items).size == 0) || !Ducky_listUiValid) {
    Ducky_drawListPage(false);
    return;
  }

  int headerBottom, hintY, listTop, rowStride, maxVisibleItems, rw, rh;
  bool hasScrollbar;
  Ducky_calcListLayout(&(headerBottom), &(hintY), &(listTop), &(rowStride),
                       &(maxVisibleItems), &(hasScrollbar), &(rw), &(rh));

  int startItem, endItem;
  Ducky_calcListWindow(maxVisibleItems, &(startItem), &(endItem));

  if ((int)(Ducky_items).size != Ducky_lastListCount ||
      startItem != Ducky_lastListStartItem ||
      maxVisibleItems != Ducky_lastListMaxVisible ||
      hasScrollbar != Ducky_lastListHasScrollbar ||
      headerBottom != Ducky_lastListHeaderBottom ||
      hintY != Ducky_lastListHintY || listTop != Ducky_lastListTop ||
      rowStride != Ducky_lastListRowStride || rw != Ducky_lastListRw ||
      rh != Ducky_lastListRh) {
    Ducky_drawListContentFull();
    return;
  }

  const int rx = Ducky_PADDING;
  Ducky_drawListRow(oldSel, startItem, listTop, rowStride, rx, rw, rh);
  Ducky_drawListRow(Ducky_sel, startItem, listTop, rowStride, rx, rw, rh);
  Ducky_drawListHints(hintY);
}

static void Ducky_drawListPage(bool forceReload) {
  Ducky_dialog = Ducky_Dialog_None;

  Ducky_mountSD();

  Ducky_drawHeader(Ducky_HeaderType_IconBar, TEXT(""), true);
  Ducky_drawHeader(Ducky_HeaderType_TextWithInfo, TEXT("BLE Ducky"), true);

  Ducky_listUiValid = false;

  if (!Ducky_sd_mounted) {
    Ducky_listLoadedOnce = false;
    display_set_text_color_2(&(tft), COL_FG, COL_BG);
    display_draw_centre_string(&(tft), "SD not mounted", DISPLAY_WIDTH / 2,
                               DISPLAY_HEIGHT / 2 - 8, 1);
  } else {
    if (forceReload || !Ducky_listLoadedOnce) {
      Ducky_reloadList();
      Ducky_listLoadedOnce = true;
    }
    Ducky_clampSel();
    Ducky_drawListContentFull();
  }
  if (featureHasTouchNavBar()) {
    Ducky_duckyUpdateNavLabels();
  } else {
    FeatureUI_layoutFooter3(
        &(Ducky_listBtns), "Open", FeatureUI_ButtonStyle_Primary, "Refresh",
        FeatureUI_ButtonStyle_Secondary, "Exit",
        FeatureUI_ButtonStyle_Secondary, false, false, false);
    {
      for (size_t c_range_index = 0;
           c_range_index < DIV_ARRAY_SIZE(Ducky_listBtns); ++c_range_index) {
        __auto_type b = &(Ducky_listBtns[c_range_index]);
        FeatureUI_drawButton(&((*b)), false);
      }
    }
  }
}

static void Ducky_drawDetailsPage(void) {
  Ducky_drawHeader(Ducky_HeaderType_IconBar, TEXT(""), true);
  Ducky_drawHeader(Ducky_HeaderType_TextWithInfo, TEXT("Script details"), true);

  display_set_text_color_2(&(tft), COL_FG, COL_BG);
  const int barY = Ducky_TOOLBAR_Y;
  const int baseY = barY + Ducky_TEXT_HEADER_H + 1;

  display_fill_rect(&(tft), 0, baseY, DISPLAY_WIDTH,
                    DISPLAY_HEIGHT - baseY - Ducky_duckyFooterReserve(),
                    COL_BG);

  if (((Ducky_items).size == 0)) {
    display_draw_centre_string(&(tft), "No script selected", DISPLAY_WIDTH / 2,
                               baseY + 24, 1);
    display_set_text_font(&(tft), 1);
    display_set_text_color_2(&(tft), UI_TEXT, COL_BG);
    display_draw_centre_string(&(tft), "Use 'Refresh' to scan for scripts",
                               DISPLAY_WIDTH / 2, baseY + 48, 1);
  } else {
    __auto_type it = &(((Ducky_ScriptItem*)(Ducky_items).data)[Ducky_sel]);
    int y = baseY + 8;

    display_set_text_font(&(tft), 1);

    display_set_text_color_2(&(tft), UI_TEXT, COL_BG);
    text_t displayName =
        Ducky_fitText((*it).name, DISPLAY_WIDTH - 2 * Ducky_PADDING, 1);
    display_draw_string(&(tft), (displayName).data, Ducky_PADDING, y, 1);
    y += 24;

    display_draw_line(&(tft), Ducky_PADDING, y, DISPLAY_WIDTH - Ducky_PADDING,
                      y, UI_LINE);
    y += 8;

    display_set_text_font(&(tft), 1);
    display_set_text_color_2(&(tft), COL_FG, COL_BG);

    display_draw_string(&(tft), "Size:", Ducky_PADDING, y, 1);
    display_draw_number(&(tft), (*it).size, 100, y, 1);
    display_draw_string(&(tft), "bytes", 140, y, 1);
    y += 28;

    display_draw_string(&(tft), "Path:", Ducky_PADDING, y, 1);
    display_set_text_font(&(tft), 1);
    text_t path =
        Ducky_fitText((*it).path, DISPLAY_WIDTH - 100 - Ducky_PADDING, 1);
    display_draw_string(&(tft), (path).data, 100, y + 2, 1);
    y += 28;

    display_set_text_font(&(tft), 1);
    display_draw_string(&(tft), "Description:", Ducky_PADDING, y, 1);
    y += 24;

    display_set_text_font(&(tft), 1);
    text_t desc =
        (strlen(((*it).desc).data) ? (*it).desc
                                   : TEXT("No description available"));

    int descY = y;
    int maxWidth = DISPLAY_WIDTH - 2 * Ducky_PADDING;
    text_t remaining = desc;
    const int bottomY = DISPLAY_HEIGHT - Ducky_duckyFooterReserve() - 6;
    while (strlen((remaining).data) > 0 && descY < bottomY) {
      text_t line = {0};
      int consumed = Ducky_wrapLineNoEllipsis(remaining, maxWidth, 1, &(line));
      if (consumed <= 0) break;
      display_draw_string(&(tft), (line).data, Ducky_PADDING, descY, 1);
      remaining = text_slice(remaining, consumed, SIZE_MAX);
      text_trim(&(remaining));
      descY += 14;
    }
  }
  if (featureHasTouchNavBar()) {
    Ducky_duckyUpdateNavLabels();
  } else {
    FeatureUI_layoutFooter3(
        &(Ducky_detailsBtns), "Execute", FeatureUI_ButtonStyle_Primary,
        "Delete", FeatureUI_ButtonStyle_Danger, "Back",
        FeatureUI_ButtonStyle_Secondary, false, false, false);
    {
      for (size_t c_range_index = 0;
           c_range_index < DIV_ARRAY_SIZE(Ducky_detailsBtns); ++c_range_index) {
        __auto_type b = &(Ducky_detailsBtns[c_range_index]);
        FeatureUI_drawButton(&((*b)), false);
      }
    }
  }
}

static void Ducky_drawSettingsPage(void) {
  Ducky_drawHeader(Ducky_HeaderType_IconBar, TEXT(""), true);
  Ducky_drawHeader(Ducky_HeaderType_TextWithInfo, TEXT("Info"), true);

  display_set_text_color_2(&(tft), COL_FG, COL_BG);
  const int barY = Ducky_TOOLBAR_Y;
  const int baseY = barY + Ducky_TEXT_HEADER_H + 1;

  display_fill_rect(&(tft), 0, baseY, DISPLAY_WIDTH,
                    DISPLAY_HEIGHT - baseY - Ducky_duckyFooterReserve(),
                    COL_BG);

  enum { rowGap = 24 };
  int y = baseY + 8;

  display_set_text_font(&(tft), 1);
  display_set_text_color_2(&(tft), UI_TEXT, COL_BG);
  display_draw_string(&(tft), "Info", Ducky_PADDING, y, 1);
  y += rowGap;

  display_set_text_color_2(&(tft), COL_FG, COL_BG);
  display_draw_string(&(tft), "Default Delay:", Ducky_PADDING, y, 1);
  display_draw_number(&(tft), Ducky_defaultDelay, 150, y, 1);
  display_draw_string(&(tft), "ms", 190, y, 1);
  y += rowGap;

  display_draw_string(&(tft), "BLE Status:", Ducky_PADDING, y, 1);
  display_set_text_color_2(&(tft), Ducky_bleConnected ? UI_OK : UI_WARN,
                           COL_BG);
  display_draw_string(&(tft), Ducky_bleConnected ? "Connected" : "Disconnected",
                      150, y, 1);
  display_set_text_color_2(&(tft), COL_FG, COL_BG);
  y += rowGap;

  display_draw_string(&(tft), "SD Status:", Ducky_PADDING, y, 1);
  display_set_text_color_2(&(tft), Ducky_sd_mounted ? UI_OK : UI_WARN, COL_BG);
  display_draw_string(&(tft), Ducky_sd_mounted ? "Mounted" : "Not mounted", 150,
                      y, 1);
  display_set_text_color_2(&(tft), COL_FG, COL_BG);
  y += rowGap;

  display_draw_string(&(tft), "Scripts Found:", Ducky_PADDING, y, 1);
  display_draw_number(&(tft), (int)(Ducky_items).size, 150, y, 1);
  y += rowGap;

  display_draw_string(&(tft), "Device Name:", Ducky_PADDING, y, 1);
  display_draw_string(&(tft), Ducky_DEV_NAME, 150, y, 1);
  y += rowGap;

  y += 4;
  display_set_text_font(&(tft), 1);
  display_draw_string(&(tft), "Use icons above for quick actions",
                      Ducky_PADDING, y, 1);
  y += 14;
  display_draw_string(&(tft), "Touch and hold for more options", Ducky_PADDING,
                      y, 1);

  if (featureHasTouchNavBar()) {
    Ducky_duckyUpdateNavLabels();
  } else {
    FeatureUI_layoutFooter1(&(Ducky_infoBtn), "Back",
                            FeatureUI_ButtonStyle_Secondary, false);
    FeatureUI_drawButton(&(Ducky_infoBtn), false);
  }
}

static void Ducky_drawConfirmDelete(void) {
  const int w = DISPLAY_WIDTH - 2 * Ducky_PADDING;
  enum { h = 96 };
  const int x = Ducky_PADDING;
  const int y = DISPLAY_HEIGHT / 2 - h / 2;

  display_fill_round_rect(&(tft), x, y, w, h, 8, UI_FG);
  display_draw_round_rect(&(tft), x, y, w, h, 8, UI_LINE);
  display_set_text_color_2(&(tft), UI_TEXT, UI_FG);
  display_draw_centre_string(&(tft), "Delete this script?", DISPLAY_WIDTH / 2,
                             y + 10, 1);
  display_set_text_color_2(&(tft), UI_TEXT, UI_FG);
  display_draw_centre_string(&(tft), "(This cannot be undone)",
                             DISPLAY_WIDTH / 2, y + 30, 1);

  int bw = (w - 3 * Ducky_PADDING) / 2;
  int bx1 = x + Ducky_PADDING;
  int bx2 = x + 2 * Ducky_PADDING + bw;
  int by = y + h - Ducky_PADDING - Ducky_BTN_H;

  Ducky_confirmBtns[0] =
      (FeatureUI_Button){(int16_t)bx1, (int16_t)by,
                         (int16_t)bw,  (int16_t)Ducky_BTN_H,
                         "Yes",        FeatureUI_ButtonStyle_Danger,
                         false};
  Ducky_confirmBtns[1] =
      (FeatureUI_Button){(int16_t)bx2, (int16_t)by,
                         (int16_t)bw,  (int16_t)Ducky_BTN_H,
                         "No",         FeatureUI_ButtonStyle_Secondary,
                         false};

  FeatureUI_drawButton(&(Ducky_confirmBtns[0]), false);
  FeatureUI_drawButton(&(Ducky_confirmBtns[1]), false);
}

static void Ducky_startBleKeyboard(const char* name) {
  Ducky_hid = ble_hid_start(name, ducky_connection_changed);
  if (!Ducky_hid) feature_exit_requested = true;
}

void Ducky_enter(void) {
#if defined(SD_CD) && SD_CD_RELIABLE
  sys_configure_gpio_pull(SD_CD, GPIO_MODE_INPUT, GPIO_PULLUP_ENABLE);
  if (gpio_get_level(((gpio_num_t)(SD_CD)))) {
    display_fill_rect(&(tft), 0, Ducky_TOOLBAR_Y - 1, DISPLAY_WIDTH,
                      DISPLAY_HEIGHT - (Ducky_TOOLBAR_Y - 1), COL_BG);
    display_set_text_color_2(&(tft), UI_WARN, COL_BG);
    display_set_text_font(&(tft), 1);
    display_draw_centre_string(&(tft), "SD card not detected",
                               DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2 - 14, 1);
    display_set_text_font(&(tft), 1);
    display_set_text_color_2(&(tft), UI_TEXT, COL_BG);
    display_draw_centre_string(&(tft), "Returning...", DISPLAY_WIDTH / 2,
                               DISPLAY_HEIGHT / 2 + 10, 1);
    sys_sleep_ms(900);
    Ducky_ducky_active = false;
    feature_exit_requested = true;
    return;
  }
#endif

  Ducky_ducky_active = true;
  setTouchButtonInputEnabled(true);
  Ducky_ui_busy = false;
  Ducky_dialog = Ducky_Dialog_None;
  Ducky_ui_inited = false;

  Ducky_startBleKeyboard(Ducky_DEV_NAME);

  if (Ducky_hid && Ducky_bleConnected) {
    Ducky_KbdReport idle = Ducky_KbdReport_INITIALIZER;
    ble_hid_send((uint8_t*)&idle, sizeof(idle));
  }

  if (!Ducky_mountSD() || !Ducky_ensureDuckyDir()) {
    display_fill_rect(
        &(tft), 0, Ducky_TOOLBAR_Y - 1, DISPLAY_WIDTH,
        DISPLAY_HEIGHT - (Ducky_TOOLBAR_Y - 1) - Ducky_duckyFooterReserve(),
        COL_BG);
    display_set_text_color_2(&(tft), UI_WARN, COL_BG);
    display_set_text_font(&(tft), 1);
    display_draw_centre_string(&(tft), "SD mount failed", DISPLAY_WIDTH / 2,
                               DISPLAY_HEIGHT / 2 - 14, 1);
    display_set_text_font(&(tft), 1);
    display_set_text_color_2(&(tft), UI_TEXT, COL_BG);
    display_draw_centre_string(&(tft), "Returning...", DISPLAY_WIDTH / 2,
                               DISPLAY_HEIGHT / 2 + 10, 1);
    sys_sleep_ms(900);
    Ducky_ducky_active = false;
    feature_exit_requested = true;
    return;
  }
  Ducky_needsRedraw = true;
}

bool Ducky_active(void) { return Ducky_ducky_active; }

void Ducky_exit(void) {
  Ducky_ducky_active = false;
  setTouchButtonInputEnabled(false);
  setTouchNavLabels(NULL, NULL, NULL, NULL, NULL);
  Ducky_ui_busy = false;
  Ducky_dialog = Ducky_Dialog_None;
  Ducky_uiDrawn = false;

  if (Ducky_hid && Ducky_bleConnected) {
    Ducky_KbdReport idle = Ducky_KbdReport_INITIALIZER;
    ble_hid_send((uint8_t*)&idle, sizeof(idle));
  }

  ble_hid_stop();
}
void Ducky_setup(void) {
  Ducky_ducky_active = false;
  display_set_text_datum(&(tft), TL_DATUM);
  Ducky_mountSD();
  Ducky_ensureDuckyDir();

  Ducky_ui_inited = false;
  Ducky_needsRedraw = true;
  Ducky_ui_busy = false;
  Ducky_dialog = Ducky_Dialog_None;
  Ducky_uiDrawn = false;
}

void Ducky_loop(void) {
  if (!Ducky_ui_inited) {
    display_fill_rect(
        &(tft), 0, Ducky_TOOLBAR_Y - 1, DISPLAY_WIDTH,
        DISPLAY_HEIGHT - (Ducky_TOOLBAR_Y - 1) - Ducky_duckyFooterReserve(),
        COL_BG);
    Ducky_sel = 0;
    Ducky_page = Ducky_Page_List;
    Ducky_uiDrawn = false;
    Ducky_drawListPage(false);
    Ducky_ui_inited = true;
    Ducky_needsRedraw = false;
    Ducky_refreshHeaderStatusIfNeeded(true);
  }

  Ducky_updateIconAnimation();

  Ducky_refreshHeaderStatusIfNeeded(false);
  if (feature_exit_requested) return;

  if (Ducky_ui_busy) {
    sys_sleep_ms(5);
    return;
  }

  if (Ducky_dialog != Ducky_Dialog_ConfirmDelete) {
    if (Ducky_page == Ducky_Page_List) {
      if (featureHasTouchNavBar()) {
        if (Ducky_pcfPressedEdge(&(Ducky_ebUp), 40)) {
          if (Ducky_sel > 0) {
            Ducky_updateListSelection(Ducky_sel - 1);
          }
          return;
        }
        if (Ducky_pcfPressedEdge(&(Ducky_ebDown), 40)) {
          Ducky_drawListPage(true);
          return;
        }
        if (Ducky_pcfPressedEdge(&(Ducky_ebRight), 40)) {
          if (Ducky_sel < (int)(Ducky_items).size - 1) {
            Ducky_updateListSelection(Ducky_sel + 1);
          }
          return;
        }
        if (Ducky_pcfPressedEdge(&(Ducky_ebSelect), 40)) {
          if (!((Ducky_items).size == 0)) {
            Ducky_page = Ducky_Page_Details;
            Ducky_drawDetailsPage();
          }
          return;
        }
        if (Ducky_pcfPressedEdge(&(Ducky_ebLeft), 40)) {
          Ducky_ducky_active = false;
          feature_exit_requested = true;
          return;
        }
      } else {
        if (Ducky_pcfPressedEdge(&(Ducky_ebUp), 40)) {
          if (Ducky_sel > 0) {
            Ducky_updateListSelection(Ducky_sel - 1);
          } else {
          }
          return;
        }
        if (Ducky_pcfPressedEdge(&(Ducky_ebDown), 40)) {
          if (Ducky_sel < (int)(Ducky_items).size - 1) {
            Ducky_updateListSelection(Ducky_sel + 1);
          }
          return;
        }
        if (Ducky_pcfPressedEdge(&(Ducky_ebRight), 40) ||
            Ducky_pcfPressedEdge(&(Ducky_ebSelect), 40)) {
          if (!((Ducky_items).size == 0)) {
            Ducky_page = Ducky_Page_Details;
            Ducky_drawDetailsPage();
          }
          return;
        }
        if (Ducky_pcfPressedEdge(&(Ducky_ebLeft), 40)) {
          Ducky_ducky_active = false;
          feature_exit_requested = true;
          return;
        }
      }
    } else if (Ducky_page == Ducky_Page_Details) {
      if (featureHasTouchNavBar()) {
        if (Ducky_pcfPressedEdge(&(Ducky_ebUp), 40)) {
          Ducky_page = Ducky_Page_List;
          Ducky_drawListPage(false);
          return;
        }
        if (Ducky_pcfPressedEdge(&(Ducky_ebDown), 40)) {
          Ducky_dialog = Ducky_Dialog_ConfirmDelete;
          Ducky_drawConfirmDelete();
          return;
        }
        if (Ducky_pcfPressedEdge(&(Ducky_ebRight), 40) ||
            Ducky_pcfPressedEdge(&(Ducky_ebSelect), 40)) {
          if (!Ducky_bleConnected) {
            display_fill_round_rect(
                &(tft), Ducky_PADDING, Ducky_duckyToastTop(),
                DISPLAY_WIDTH - 2 * Ducky_PADDING, 22, 6, COL_WARN);
            display_set_text_color_2(&(tft), COL_BG, COL_WARN);
            display_draw_centre_string(&(tft), "Not paired/connected",
                                       DISPLAY_WIDTH / 2,
                                       Ducky_duckyToastCenterY(), 2);
          } else if (!((Ducky_items).size == 0)) {
            bool ok = Ducky_execFile(
                ((Ducky_ScriptItem*)(Ducky_items).data)[Ducky_sel].path);
            Ducky_drawDetailsPage();
            if (!ok) {
              display_fill_round_rect(
                  &(tft), Ducky_PADDING, Ducky_duckyToastTop(),
                  DISPLAY_WIDTH - 2 * Ducky_PADDING, 22, 6, COL_WARN);
              display_set_text_color_2(&(tft), COL_BG, COL_WARN);
              display_draw_centre_string(
                  &(tft),
                  (text_join_cstr("Error: ", (Ducky_lastError).data)).data,
                  DISPLAY_WIDTH / 2, Ducky_duckyToastCenterY(), 2);
            }
          }
          return;
        }
        if (Ducky_pcfPressedEdge(&(Ducky_ebLeft), 40)) {
          Ducky_page = Ducky_Page_List;
          Ducky_drawListPage(false);
          return;
        }
      } else {
        if (Ducky_pcfPressedEdge(&(Ducky_ebRight), 40) ||
            Ducky_pcfPressedEdge(&(Ducky_ebSelect), 40)) {
          if (!Ducky_bleConnected) {
            display_fill_round_rect(
                &(tft), Ducky_PADDING, Ducky_duckyToastTop(),
                DISPLAY_WIDTH - 2 * Ducky_PADDING, 22, 6, COL_WARN);
            display_set_text_color_2(&(tft), COL_BG, COL_WARN);
            display_draw_centre_string(&(tft), "Not paired/connected",
                                       DISPLAY_WIDTH / 2,
                                       Ducky_duckyToastCenterY(), 2);
          } else if (!((Ducky_items).size == 0)) {
            bool ok = Ducky_execFile(
                ((Ducky_ScriptItem*)(Ducky_items).data)[Ducky_sel].path);
            Ducky_drawDetailsPage();
            if (!ok) {
              display_fill_round_rect(
                  &(tft), Ducky_PADDING, Ducky_duckyToastTop(),
                  DISPLAY_WIDTH - 2 * Ducky_PADDING, 22, 6, COL_WARN);
              display_set_text_color_2(&(tft), COL_BG, COL_WARN);
              display_draw_centre_string(
                  &(tft),
                  (text_join_cstr("Error: ", (Ducky_lastError).data)).data,
                  DISPLAY_WIDTH / 2, Ducky_duckyToastCenterY(), 2);
            }
          }
          return;
        }
        if (Ducky_pcfPressedEdge(&(Ducky_ebLeft), 40)) {
          Ducky_page = Ducky_Page_List;
          Ducky_drawListPage(false);
          return;
        }
      }
    } else if (Ducky_page == Ducky_Page_Settings) {
      if (Ducky_pcfPressedEdge(&(Ducky_ebLeft), 40)) {
        Ducky_page = Ducky_Page_List;
        Ducky_drawListPage(false);
        return;
      }
    }
  }
  Ducky_Tap tap = Ducky_readTap();
  if (!tap.has) {
    sys_sleep_ms(5);
    return;
  }

  if (Ducky_dialog == Ducky_Dialog_ConfirmDelete) {
    int id = FeatureUI_hit(Ducky_confirmBtns, 2, tap.x, tap.y);
    if (id == 0) {
      if (!((Ducky_items).size == 0) && Ducky_mountSD() &&
          storage_exists(
              (((Ducky_ScriptItem*)(Ducky_items).data)[Ducky_sel].path).data)) {
        storage_remove(
            (((Ducky_ScriptItem*)(Ducky_items).data)[Ducky_sel].path).data);
        Ducky_reloadList();
        Ducky_page = Ducky_Page_List;
        Ducky_dialog = Ducky_Dialog_None;
        Ducky_drawListPage(false);
      } else {
        Ducky_dialog = Ducky_Dialog_None;
        Ducky_drawDetailsPage();
      }
    } else if (id == 1) {
      Ducky_dialog = Ducky_Dialog_None;
      Ducky_drawDetailsPage();
    } else {
    }
    return;
  }

  Ducky_handleIconTouch(tap.x, tap.y);

  bool doubleTap = Ducky_isDoubleTap(&(tap));

  if (Ducky_page == Ducky_Page_List) {
    bool handledFooter = false;
    if (!featureHasTouchNavBar()) {
      int id = FeatureUI_hit(Ducky_listBtns, 3, tap.x, tap.y);
      if (id == 0) {
        if (!((Ducky_items).size == 0)) {
          Ducky_page = Ducky_Page_Details;
          Ducky_drawDetailsPage();
        }
        handledFooter = true;
      } else if (id == 1) {
        Ducky_drawListPage(true);
        handledFooter = true;
      } else if (id == 2) {
        Ducky_ducky_active = false;
        feature_exit_requested = true;
        return;
      }
    }

    if (!handledFooter) {
      int headerBottom, hintY, listTop, rowStride, maxVisibleItems, rw, rh;
      bool hasScrollbar;
      Ducky_calcListLayout(&(headerBottom), &(hintY), &(listTop), &(rowStride),
                           &(maxVisibleItems), &(hasScrollbar), &(rw), &(rh));

      int y = tap.y;
      if (y >= listTop && y < DISPLAY_HEIGHT - Ducky_duckyFooterReserve()) {
        int startItem, endItem;
        Ducky_calcListWindow(maxVisibleItems, &(startItem), &(endItem));
        int clickedRow = (y - listTop) / rowStride;
        int actualIdx = startItem + clickedRow;

        if (actualIdx >= 0 && actualIdx < (int)(Ducky_items).size) {
          if (doubleTap && Ducky_bleConnected) {
            bool ok = Ducky_execFile(
                ((Ducky_ScriptItem*)(Ducky_items).data)[actualIdx].path);
            Ducky_drawListPage(false);
            if (!ok) {
              display_fill_round_rect(
                  &(tft), Ducky_PADDING, Ducky_duckyToastTop(),
                  DISPLAY_WIDTH - 2 * Ducky_PADDING, 22, 6, COL_WARN);
              display_set_text_color_2(&(tft), COL_BG, COL_WARN);
              display_draw_centre_string(
                  &(tft),
                  (text_join_cstr("Error: ", (Ducky_lastError).data)).data,
                  DISPLAY_WIDTH / 2, Ducky_duckyToastCenterY(), 2);
            }
          } else {
            Ducky_updateListSelection(actualIdx);
          }
        }
      }
    }

  } else if (Ducky_page == Ducky_Page_Details) {
    if (!featureHasTouchNavBar()) {
      int id = FeatureUI_hit(Ducky_detailsBtns, 3, tap.x, tap.y);
      if (id == 0) {
        if (!Ducky_bleConnected) {
          display_fill_round_rect(&(tft), Ducky_PADDING, Ducky_duckyToastTop(),
                                  DISPLAY_WIDTH - 2 * Ducky_PADDING, 22, 6,
                                  COL_WARN);
          display_set_text_color_2(&(tft), COL_BG, COL_WARN);
          display_draw_centre_string(&(tft), "Not paired/connected",
                                     DISPLAY_WIDTH / 2,
                                     Ducky_duckyToastCenterY(), 2);
        } else if (!((Ducky_items).size == 0)) {
          bool ok = Ducky_execFile(
              ((Ducky_ScriptItem*)(Ducky_items).data)[Ducky_sel].path);
          Ducky_drawDetailsPage();
          if (!ok) {
            display_fill_round_rect(
                &(tft), Ducky_PADDING, Ducky_duckyToastTop(),
                DISPLAY_WIDTH - 2 * Ducky_PADDING, 22, 6, COL_WARN);
            display_set_text_color_2(&(tft), COL_BG, COL_WARN);
            display_draw_centre_string(
                &(tft),
                (text_join_cstr("Error: ", (Ducky_lastError).data)).data,
                DISPLAY_WIDTH / 2, Ducky_duckyToastCenterY(), 2);
          }
        }
      } else if (id == 1) {
        Ducky_dialog = Ducky_Dialog_ConfirmDelete;
        Ducky_drawConfirmDelete();
      } else if (id == 2) {
        Ducky_page = Ducky_Page_List;
        Ducky_drawListPage(false);
      }
    }
  } else if (Ducky_page == Ducky_Page_Settings) {
    if (!featureHasTouchNavBar()) {
      int id = FeatureUI_hit(&Ducky_infoBtn, 1, tap.x, tap.y);
      if (id == 0) {
        Ducky_page = Ducky_Page_List;
        Ducky_drawListPage(false);
      }
    }
  }

  sys_sleep_ms(5);
}
