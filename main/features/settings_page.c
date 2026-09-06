#include "c_vector.h"
#include "config.h"
#include "icon.h"
#include "storage_file.h"
#include "system_features.h"
#include "board_services.h"
#include "touch_ui.h"
#include "ui_utils.h"

static int AppSettingsUI_screenW(void) { return display_width(&(tft)); }
enum { AppSettingsUI_BAR_H = 22 };
static const int AppSettingsUI_TITLE_Y = AppSettingsUI_BAR_H + 4;
enum { AppSettingsUI_TITLE_H = 16 };
enum { AppSettingsUI_ROW_H = 32 };
enum { AppSettingsUI_GAP_Y = 4 };
enum { AppSettingsUI_PAD_X = 12 };
enum { AppSettingsUI_LABEL_W = 92 };
enum { AppSettingsUI_RADIUS = 8 };

static inline int AppSettingsUI_rowY(int i) {
  return AppSettingsUI_TITLE_Y + AppSettingsUI_TITLE_H + 6 +
         i * (AppSettingsUI_ROW_H + AppSettingsUI_GAP_Y);
}

typedef struct AppSettingsUI_Rect {
  int x, y, w, h;
} AppSettingsUI_Rect;
#define AppSettingsUI_Rect_INITIALIZER {0}
;
static inline AppSettingsUI_Rect AppSettingsUI_makeRect(int x, int y, int w,
                                                        int h) {
  return (AppSettingsUI_Rect){x, y, w, h};
}
static inline void AppSettingsUI_fillRound(const AppSettingsUI_Rect* r,
                                           uint16_t c) {
  display_fill_round_rect(&(tft), (*r).x, (*r).y, (*r).w, (*r).h,
                          AppSettingsUI_RADIUS, c);
}
static inline void AppSettingsUI_drawRound(const AppSettingsUI_Rect* r,
                                           uint16_t c) {
  display_draw_round_rect(&(tft), (*r).x, (*r).y, (*r).w, (*r).h,
                          AppSettingsUI_RADIUS, c);
}

static uint16_t AppSettingsUI_cardBG, AppSettingsUI_cardEdge,
    AppSettingsUI_textDim, AppSettingsUI_textStrong, AppSettingsUI_accentSoft,
    AppSettingsUI_btnBg, AppSettingsUI_btnFg, AppSettingsUI_btnStroke;

static uint16_t AppSettingsUI_blend565(uint16_t a, uint16_t b, uint8_t k) {
  uint32_t rb = (((a & 0xF81F) * (255 - k)) + ((b & 0xF81F) * k)) >> 8;
  uint32_t g = (((a & 0x07E0) * (255 - k)) + ((b & 0x07E0) * k)) >> 8;
  return (rb & 0xF81F) | (g & 0x07E0);
}
static void AppSettingsUI_buildPalette(void) {
  AppSettingsUI_cardBG = UI_FG;
  AppSettingsUI_cardEdge = UI_LINE;
  AppSettingsUI_textDim = UI_TEXT;
  AppSettingsUI_textStrong = UI_TEXT;
  AppSettingsUI_accentSoft = UI_ICON;

  AppSettingsUI_btnBg = UI_BG;
  AppSettingsUI_btnFg = UI_FG;
  AppSettingsUI_btnStroke = UI_LINE;
}

static int AppSettingsUI_sel = 0;
static bool AppSettingsUI_dirtySettings = false;
static bool AppSettingsUI_uiDirty = false;

static const char* AppSettingsUI_items[] = {"Brightness", "Theme", "Accent",
                                            "NeoPixel", "Auto Scan"};
static const int AppSettingsUI_N =
    sizeof(AppSettingsUI_items) / sizeof(AppSettingsUI_items[0]);

static uint8_t AppSettingsUI_last_brightness;
static Theme AppSettingsUI_last_theme;
static uint8_t AppSettingsUI_last_accent;
static bool AppSettingsUI_last_neopixel;
static bool AppSettingsUI_last_autoScan;
static int AppSettingsUI_last_sel;

static bool AppSettingsUI_dragging = false;

static uint32_t AppSettingsUI_lastChangeMs = 0;

static AppSettingsUI_Rect AppSettingsUI_rowRect(int i) {
  return AppSettingsUI_makeRect(
      AppSettingsUI_PAD_X, AppSettingsUI_rowY(i),
      AppSettingsUI_screenW() - AppSettingsUI_PAD_X * 2, AppSettingsUI_ROW_H);
}

static void AppSettingsUI_setTitleFont(void) {
  display_set_text_font(&(tft), 1);
}
static void AppSettingsUI_setLabelFont(void) {
  display_set_text_font(&(tft), 1);
}

static void AppSettingsUI_drawTitle(void) {
  AppSettingsUI_setTitleFont();
  display_set_text_color_2(&(tft), AppSettingsUI_textStrong, UI.bg);
  display_set_cursor(&(tft), AppSettingsUI_PAD_X, AppSettingsUI_TITLE_Y);
  display_print(&(tft), "Settings");
}

static void AppSettingsUI_drawCardStatic(int i, bool selected) {
  AppSettingsUI_Rect r = AppSettingsUI_rowRect(i);

  display_fill_rect(&(tft), 0, r.y, AppSettingsUI_screenW(), r.h, UI_BG);

  if (selected) {
    display_fill_rect(&(tft), 0, r.y, 3, r.h, UI.accent);
  }

  AppSettingsUI_setLabelFont();
  display_set_text_color_2(&(tft), AppSettingsUI_textDim, UI_BG);
  int ty = r.y + (r.h / 2 - 6);
  display_set_cursor(&(tft), r.x, ty);
  display_print(&(tft), (TEXT(AppSettingsUI_items[i])).data);

  display_draw_line(&(tft), AppSettingsUI_PAD_X, r.y + r.h - 1,
                    AppSettingsUI_screenW() - AppSettingsUI_PAD_X,
                    r.y + r.h - 1, UI_LINE);
}

static void AppSettingsUI_wipeBrightnessWidgetArea(void) {
  AppSettingsUI_Rect r = AppSettingsUI_rowRect(0);
  display_fill_rect(&(tft), r.x + AppSettingsUI_LABEL_W, r.y + 2,
                    r.w - AppSettingsUI_LABEL_W - 6, r.h - 4, UI_BG);
}
static void AppSettingsUI_wipeThemeWidgetArea(void) {
  AppSettingsUI_Rect r = AppSettingsUI_rowRect(1);
  display_fill_rect(&(tft), r.x + AppSettingsUI_LABEL_W, r.y + 2,
                    r.w - AppSettingsUI_LABEL_W - 6, r.h - 4, UI_BG);
}
static void AppSettingsUI_wipeAccentWidgetArea(void) {
  AppSettingsUI_Rect r = AppSettingsUI_rowRect(2);
  display_fill_rect(&(tft), r.x + AppSettingsUI_LABEL_W, r.y + 2,
                    r.w - AppSettingsUI_LABEL_W - 6, r.h - 4, UI_BG);
}
static void AppSettingsUI_wipeSwitchWidgetArea(int row) {
  AppSettingsUI_Rect r = AppSettingsUI_rowRect(row);
  display_fill_rect(&(tft), r.x + AppSettingsUI_LABEL_W, r.y + 2,
                    r.w - AppSettingsUI_LABEL_W - 6, r.h - 4, UI_BG);
}

static AppSettingsUI_Rect AppSettingsUI_rBrightTrack(void) {
  AppSettingsUI_Rect r = AppSettingsUI_rowRect(0);
  int left = r.x + AppSettingsUI_LABEL_W + 6;
  int right = r.x + r.w - 44;
  int w = DIV_MAX(70, right - left);
  return AppSettingsUI_makeRect(left, r.y + (r.h / 2 - 8), w, 16);
}
static AppSettingsUI_Rect AppSettingsUI_rBrightKnob(uint8_t v) {
  AppSettingsUI_Rect tr = AppSettingsUI_rBrightTrack();
  int d = 18;
  int x = tr.x + (int)((uint32_t)v * (tr.w - d) / 255u);
  int y = tr.y + tr.h / 2 - d / 2;
  return AppSettingsUI_makeRect(x, y, d, d);
}
static void AppSettingsUI_drawBrightnessWidget(uint8_t v, bool selected) {
  AppSettingsUI_wipeBrightnessWidgetArea();

  AppSettingsUI_Rect tr = AppSettingsUI_rBrightTrack();

  display_fill_round_rect(&(tft), tr.x, tr.y, tr.w, tr.h, tr.h / 2, UI_BG);
  display_draw_round_rect(&(tft), tr.x, tr.y, tr.w, tr.h, tr.h / 2,
                          AppSettingsUI_cardEdge);

  int fw = (int)((uint32_t)v * tr.w / 255u);
  display_fill_round_rect(&(tft), tr.x, tr.y, fw + 2, tr.h, tr.h / 2, UI_ICON);

  AppSettingsUI_Rect kb = AppSettingsUI_rBrightKnob(v);
  display_fill_circle(&(tft), kb.x + kb.w / 2, kb.y + kb.h / 2, kb.w / 2,
                      UI_FG);
  display_draw_circle(&(tft), kb.x + kb.w / 2, kb.y + kb.h / 2, kb.w / 2,
                      selected ? UI_ICON : UI_FG);

  int bx = tr.x + tr.w + 6, by = tr.y - 2, bw = 34, bh = tr.h + 4;
  display_fill_round_rect(&(tft), bx + 1, by + 1, bw, bh, 4,
                          AppSettingsUI_blend565(UI_BG, UI_FG, 28));
  display_fill_round_rect(&(tft), bx, by, bw, bh, 4, AppSettingsUI_cardBG);
  display_draw_round_rect(&(tft), bx, by, bw, bh, 4, AppSettingsUI_cardEdge);
  AppSettingsUI_setLabelFont();
  display_set_text_color_2(&(tft), AppSettingsUI_textStrong,
                           AppSettingsUI_cardBG);
  char buf[8];
  snprintf(buf, sizeof(buf), "%3u", v);
  display_set_cursor(&(tft), bx + 5, by + 2);
  display_print(&(tft), (TEXT(buf)).data);
}
static void AppSettingsUI_drawBrightness(uint8_t v, bool selected) {
  AppSettingsUI_drawCardStatic(0, selected);
  AppSettingsUI_drawBrightnessWidget(v, selected);
}

static AppSettingsUI_Rect AppSettingsUI_rThemeDark(void) {
  AppSettingsUI_Rect r = AppSettingsUI_rowRect(1);
  int right = r.x + r.w - 6;
  display_set_text_font(&(tft), 1);
  int wD = (int)display_text_width(&(tft), "[Dark]", 0);
  int wL = (int)display_text_width(&(tft), "Light", 0);
  int gap = 6;
  return AppSettingsUI_makeRect(right - wD - gap - wL, r.y + 8, wD, r.h - 16);
}
static AppSettingsUI_Rect AppSettingsUI_rThemeLight(void) {
  AppSettingsUI_Rect r = AppSettingsUI_rowRect(1);
  int right = r.x + r.w - 6;
  display_set_text_font(&(tft), 1);
  int wL = (int)display_text_width(&(tft), "[Light]", 0);
  return AppSettingsUI_makeRect(right - wL, r.y + 8, wL, r.h - 16);
}

static void AppSettingsUI_drawThemeWidget(Theme th, bool) {
  AppSettingsUI_wipeThemeWidgetArea();

  AppSettingsUI_Rect r = AppSettingsUI_rowRect(1);
  int right = r.x + r.w - 6;
  int ty = r.y + (r.h / 2 - 6);

  AppSettingsUI_setLabelFont();

  const char* darkLabel = (th == Theme_Dark) ? "[Dark]" : "Dark";
  const char* lightLabel = (th == Theme_Light) ? "[Light]" : "Light";

  int wD = (int)display_text_width(&(tft), darkLabel, 0);
  int wL = (int)display_text_width(&(tft), lightLabel, 0);
  int gap = 6;

  int lx = right - wL;
  int dx = lx - gap - wD;

  display_set_text_color_2(&(tft), AppSettingsUI_textStrong, UI_BG);
  display_set_cursor(&(tft), dx, ty);
  display_print(&(tft), (TEXT(darkLabel)).data);
  display_set_cursor(&(tft), lx, ty);
  display_print(&(tft), (TEXT(lightLabel)).data);
}
static void AppSettingsUI_drawTheme(Theme th, bool selected) {
  AppSettingsUI_drawCardStatic(1, selected);
  AppSettingsUI_drawThemeWidget(th, selected);
}

static AppSettingsUI_Rect AppSettingsUI_rAccentSwatch(void) {
  AppSettingsUI_Rect r = AppSettingsUI_rowRect(2);
  int sw = 18;
  int sx = r.x + r.w - sw - 6;
  int sy = r.y + (r.h - sw) / 2;
  return AppSettingsUI_makeRect(sx, sy, sw, sw);
}
static void AppSettingsUI_drawAccentWidget(uint8_t preset, bool) {
  AppSettingsUI_wipeAccentWidgetArea();

  AppSettingsUI_Rect r = AppSettingsUI_rowRect(2);
  int right = r.x + r.w - 6;
  int ty = r.y + (r.h / 2 - 6);

  AppSettingsUI_setLabelFont();
  const char* name = accentPresetName(preset);
  int nameW = (int)display_text_width(&(tft), name, 0);
  AppSettingsUI_Rect sw = AppSettingsUI_rAccentSwatch();
  int nameX = sw.x - 8 - nameW;

  display_set_text_color_2(&(tft), AppSettingsUI_textStrong, UI_BG);
  display_set_cursor(&(tft), nameX, ty);
  display_print(&(tft), (TEXT(name)).data);

  display_fill_round_rect(&(tft), sw.x, sw.y, sw.w, sw.h, 4,
                          accentColor565(preset));
  display_draw_round_rect(&(tft), sw.x, sw.y, sw.w, sw.h, 4,
                          AppSettingsUI_cardEdge);
}
static void AppSettingsUI_drawAccent(uint8_t preset, bool selected) {
  AppSettingsUI_drawCardStatic(2, selected);
  AppSettingsUI_drawAccentWidget(preset, selected);
}

static AppSettingsUI_Rect AppSettingsUI_rSwitchTrack(int row) {
  AppSettingsUI_Rect r = AppSettingsUI_rowRect(row);
  enum { w = 34 };
  enum { h = 14 };
  int x = r.x + r.w - w - 10;
  int y = r.y + (r.h - h) / 2;
  return AppSettingsUI_makeRect(x, y, w, h);
}
static AppSettingsUI_Rect AppSettingsUI_rSwitchKnob(bool on, int row) {
  AppSettingsUI_Rect tr = AppSettingsUI_rSwitchTrack(row);
  int d = tr.h - 4;
  int x = on ? (tr.x + tr.w - d - 2) : (tr.x + 2);
  int y = tr.y + (tr.h - d) / 2;
  return AppSettingsUI_makeRect(x, y, d, d);
}
static void AppSettingsUI_drawSwitchWidgetRow(bool on, bool, int row) {
  AppSettingsUI_wipeSwitchWidgetArea(row);

  AppSettingsUI_Rect tr = AppSettingsUI_rSwitchTrack(row);

  display_fill_round_rect(&(tft), tr.x, tr.y, tr.w, tr.h, tr.h / 2, UI_BG);
  display_draw_round_rect(&(tft), tr.x, tr.y, tr.w, tr.h, tr.h / 2,
                          AppSettingsUI_cardEdge);

  if (on) {
    display_fill_round_rect(&(tft), tr.x + 1, tr.y + 1, tr.w - 2, tr.h - 2,
                            tr.h / 2, UI_ICON);
  }

  AppSettingsUI_Rect kb = AppSettingsUI_rSwitchKnob(on, row);
  uint16_t knobBody = UI_FG;
  uint16_t knobEdge = on ? UI.accent : UI_FG;
  display_fill_circle(&(tft), kb.x + kb.w / 2, kb.y + kb.h / 2, kb.w / 2,
                      knobBody);
  display_draw_circle(&(tft), kb.x + kb.w / 2, kb.y + kb.h / 2, kb.w / 2,
                      knobEdge);

  AppSettingsUI_setLabelFont();
  display_set_text_color_2(&(tft), AppSettingsUI_textStrong, UI_BG);
  int labelX = tr.x - 26;
  int labelY = tr.y + 1;
  display_set_cursor(&(tft), labelX, labelY);
  display_print(&(tft), (TEXT(on ? "ON" : "OFF")).data);
}
static void AppSettingsUI_drawSwitchRow(bool on, bool selected, int row) {
  AppSettingsUI_drawCardStatic(row, selected);
  AppSettingsUI_drawSwitchWidgetRow(on, selected, row);
}

static void AppSettingsUI_drawNeoPixel(bool on, bool selected) {
  AppSettingsUI_drawSwitchRow(on, selected, 3);
}
static void AppSettingsUI_drawAutoScan(bool on, bool selected) {
  AppSettingsUI_drawSwitchRow(on, selected, 4);
}

static AppSettingsUI_Rect AppSettingsUI_backRect(void) {
  int h = display_height(&(tft));
  int bwTotal = AppSettingsUI_screenW() - AppSettingsUI_PAD_X * 2;
  int bh = 24;
  int bx = AppSettingsUI_PAD_X;
  int by = h - bh - 8;
  enum { gap = 8 };
  int bw = (bwTotal - gap) / 2;
  return AppSettingsUI_makeRect(bx, by, bw, bh);
}
static AppSettingsUI_Rect AppSettingsUI_saveRect(void) {
  AppSettingsUI_Rect b = AppSettingsUI_backRect();
  enum { gap = 8 };
  return AppSettingsUI_makeRect(b.x + b.w + gap, b.y, b.w, b.h);
}
static void AppSettingsUI_drawFooterButton(const AppSettingsUI_Rect* b,
                                           const char* label, uint16_t body,
                                           uint16_t edge) {
  FeatureUI_ButtonStyle style = (body == UI.accent)
                                    ? FeatureUI_ButtonStyle_Primary
                                    : FeatureUI_ButtonStyle_Secondary;
  FeatureUI_drawButtonRect((*b).x, (*b).y, (*b).w, (*b).h, label, style, false,
                           false, 1);
}
static void AppSettingsUI_footerToast(const char* msg, uint16_t color) {
  AppSettingsUI_Rect b = AppSettingsUI_backRect();
  int y = b.y - 18;
  int h = 14;
  display_fill_rect(&(tft), AppSettingsUI_PAD_X, y,
                    AppSettingsUI_screenW() - AppSettingsUI_PAD_X * 2, h,
                    UI_BG);
  display_set_text_color_2(&(tft), color, UI_BG);
  display_set_text_font(&(tft), 1);
  display_set_text_size(&(tft), 1);
  display_set_cursor(&(tft), AppSettingsUI_PAD_X, y + 3);
  display_print(&(tft), (TEXT(msg)).data);
}
static void AppSettingsUI_drawFooter(bool backPressed, bool savePressed) {
  AppSettingsUI_Rect b = AppSettingsUI_backRect();
  AppSettingsUI_Rect s = AppSettingsUI_saveRect();

  int clearX = AppSettingsUI_PAD_X - 2;
  int clearW = AppSettingsUI_screenW() - (AppSettingsUI_PAD_X * 2) + 4;
  display_fill_rect(&(tft), clearX, b.y - 2, clearW, b.h + 4, UI.bg);

  FeatureUI_drawButtonRect(b.x, b.y, b.w, b.h, "Back",
                           backPressed ? FeatureUI_ButtonStyle_Primary
                                       : FeatureUI_ButtonStyle_Secondary,
                           false, false, 1);

  FeatureUI_drawButtonRect(s.x, s.y, s.w, s.h, "Save",
                           FeatureUI_ButtonStyle_Primary, false, false, 1);
}
static bool AppSettingsUI_touchInRect(const AppSettingsUI_Rect* r, int x,
                                      int y) {
  return (x >= (*r).x && x <= (*r).x + (*r).w && y >= (*r).y &&
          y <= (*r).y + (*r).h);
}

static void AppSettingsUI_drawAll(void) {
  display_fill_screen(&(tft), UI_BG);
  AppSettingsUI_buildPalette();

  drawStatusBar(currentBatteryVoltage, true, false);
  AppSettingsUI_setTitleFont();
  AppSettingsUI_drawTitle();

  __auto_type s = &((*settings()));
  AppSettingsUI_drawBrightness((*s).brightness, AppSettingsUI_sel == 0);
  AppSettingsUI_drawTheme((*s).theme, AppSettingsUI_sel == 1);
  AppSettingsUI_drawAccent((*s).accentColor, AppSettingsUI_sel == 2);
  AppSettingsUI_drawNeoPixel((*s).neopixelEnabled, AppSettingsUI_sel == 3);
  bool autoScan = ((*s).autoWifiScan || (*s).autoBleScan);
  AppSettingsUI_drawAutoScan(autoScan, AppSettingsUI_sel == 4);

  AppSettingsUI_drawFooter(false, false);

  AppSettingsUI_last_sel = AppSettingsUI_sel;
  AppSettingsUI_last_brightness = (*s).brightness;
  AppSettingsUI_last_theme = (*s).theme;
  AppSettingsUI_last_accent = (*s).accentColor;
  AppSettingsUI_last_neopixel = (*s).neopixelEnabled;
  AppSettingsUI_last_autoScan = autoScan;
  AppSettingsUI_uiDirty = false;
}

static void AppSettingsUI_redrawIfChanged(void) {
  __auto_type s = &((*settings()));

  if ((*s).theme != AppSettingsUI_last_theme ||
      (*s).accentColor != AppSettingsUI_last_accent) {
    applyThemeToPalette((*s).theme);
    AppSettingsUI_buildPalette();
    AppSettingsUI_drawAll();
    return;
  }

  if (AppSettingsUI_sel != AppSettingsUI_last_sel) {
    AppSettingsUI_drawCardStatic(0, AppSettingsUI_sel == 0);
    AppSettingsUI_drawBrightnessWidget((*s).brightness, AppSettingsUI_sel == 0);
    AppSettingsUI_drawCardStatic(1, AppSettingsUI_sel == 1);
    AppSettingsUI_drawThemeWidget((*s).theme, AppSettingsUI_sel == 1);
    AppSettingsUI_drawCardStatic(2, AppSettingsUI_sel == 2);
    AppSettingsUI_drawAccentWidget((*s).accentColor, AppSettingsUI_sel == 2);
    AppSettingsUI_drawCardStatic(3, AppSettingsUI_sel == 3);
    AppSettingsUI_drawSwitchWidgetRow((*s).neopixelEnabled,
                                      AppSettingsUI_sel == 3, 3);
    bool autoScan = ((*s).autoWifiScan || (*s).autoBleScan);
    AppSettingsUI_drawCardStatic(4, AppSettingsUI_sel == 4);
    AppSettingsUI_drawSwitchWidgetRow(autoScan, AppSettingsUI_sel == 4, 4);
    AppSettingsUI_last_sel = AppSettingsUI_sel;
  } else {
    if ((*s).brightness != AppSettingsUI_last_brightness) {
      AppSettingsUI_drawBrightnessWidget((*s).brightness,
                                         AppSettingsUI_sel == 0);
      AppSettingsUI_last_brightness = (*s).brightness;
    }
    if ((*s).neopixelEnabled != AppSettingsUI_last_neopixel) {
      AppSettingsUI_drawSwitchWidgetRow((*s).neopixelEnabled,
                                        AppSettingsUI_sel == 3, 3);
      AppSettingsUI_last_neopixel = (*s).neopixelEnabled;
    }
    bool autoScan = ((*s).autoWifiScan || (*s).autoBleScan);
    if (autoScan != AppSettingsUI_last_autoScan) {
      AppSettingsUI_drawSwitchWidgetRow(autoScan, AppSettingsUI_sel == 4, 4);
      AppSettingsUI_last_autoScan = autoScan;
    }
    if ((*s).theme != AppSettingsUI_last_theme) {
      AppSettingsUI_drawThemeWidget((*s).theme, AppSettingsUI_sel == 1);
      AppSettingsUI_last_theme = (*s).theme;
    }
  }

  AppSettingsUI_uiDirty = false;
}

static bool AppSettingsUI_applyBrightness(uint8_t v) {
  __auto_type s = &((*settings()));
  if ((*s).brightness == v) return false;
  (*s).brightness = v;
  setBrightness(v);
  AppSettingsUI_dirtySettings = true;
  AppSettingsUI_uiDirty = true;
  AppSettingsUI_lastChangeMs = sys_monotonic_ms();
  return true;
}
static bool AppSettingsUI_applyTheme(Theme t) {
  __auto_type s = &((*settings()));
  if ((*s).theme == t) return false;
  (*s).theme = t;
  applyThemeToPalette(t);
  AppSettingsUI_buildPalette();
  AppSettingsUI_dirtySettings = true;
  AppSettingsUI_uiDirty = true;
  AppSettingsUI_lastChangeMs = sys_monotonic_ms();
  return true;
}
static bool AppSettingsUI_applyAccent(uint8_t preset) {
  __auto_type s = &((*settings()));
  preset = accentPresetClamp(preset);
  if ((*s).accentColor == preset) return false;
  (*s).accentColor = preset;
  applyThemeToPalette((*s).theme);
  AppSettingsUI_buildPalette();
  AppSettingsUI_dirtySettings = true;
  AppSettingsUI_uiDirty = true;
  AppSettingsUI_lastChangeMs = sys_monotonic_ms();
  return true;
}
static bool AppSettingsUI_applyNeoPixel(bool en) {
  __auto_type s = &((*settings()));
  if ((*s).neopixelEnabled == en) return false;
  (*s).neopixelEnabled = en;
  board_set_status_light(en, 0, 16, 24);
  AppSettingsUI_dirtySettings = true;
  AppSettingsUI_uiDirty = true;
  AppSettingsUI_lastChangeMs = sys_monotonic_ms();
  return true;
}

static bool AppSettingsUI_applyAutoScan(bool en) {
  __auto_type s = &((*settings()));
  if ((*s).autoWifiScan == en && (*s).autoBleScan == en) return false;
  (*s).autoWifiScan = en;
  (*s).autoBleScan = en;
  AppSettingsUI_dirtySettings = true;
  AppSettingsUI_uiDirty = true;
  AppSettingsUI_lastChangeMs = sys_monotonic_ms();
  return true;
}

static void AppSettingsUI_handleTouch(void) {
  int tx, ty;
  static uint32_t lastToggleMs = 0;
  if (!readTouchXY(&(tx), &(ty))) {
    AppSettingsUI_dragging = false;
    return;
  }

  AppSettingsUI_Rect br = AppSettingsUI_backRect();
  AppSettingsUI_Rect sr = AppSettingsUI_saveRect();
  if (AppSettingsUI_touchInRect(&(br), tx, ty)) {
    AppSettingsUI_drawFooter(true, false);
    sys_sleep_ms(25);
    AppSettingsUI_drawFooter(false, false);

    feature_exit_requested = true;
    return;
  }
  if (AppSettingsUI_touchInRect(&(sr), tx, ty)) {
    AppSettingsUI_drawFooter(false, true);
    sys_sleep_ms(25);
    AppSettingsUI_drawFooter(false, false);
    if (AppSettingsUI_dirtySettings) {
      bool ok = settingsSave();
      if (ok) {
        AppSettingsUI_dirtySettings = false;
        AppSettingsUI_footerToast("Saved", UI.ok);
        sys_sleep_ms(250);
        AppSettingsUI_footerToast("      ", UI_BG);
      } else {
        AppSettingsUI_footerToast("Save FAILED", UI.warn);
      }
    } else {
      AppSettingsUI_footerToast("No changes", UI_TEXT);
      sys_sleep_ms(250);
      AppSettingsUI_footerToast("         ", UI_BG);
    }
    return;
  }

  for (int i = 0; i < AppSettingsUI_N; ++i) {
    AppSettingsUI_Rect rr = AppSettingsUI_rowRect(i);
    if (ty >= rr.y && ty <= rr.y + rr.h) {
      AppSettingsUI_sel = i;
      break;
    }
  }

  __auto_type s = &((*settings()));

  if (AppSettingsUI_sel == 0) {
    AppSettingsUI_Rect tr = AppSettingsUI_rBrightTrack();
    AppSettingsUI_Rect kb = AppSettingsUI_rBrightKnob((*s).brightness);

    bool inTrack = (tx >= tr.x && tx <= tr.x + tr.w && ty >= tr.y - 10 &&
                    ty <= tr.y + tr.h + 10);
    bool inKnob =
        (tx >= kb.x && tx <= kb.x + kb.w && ty >= kb.y && ty <= kb.y + kb.h);

    if (inTrack || inKnob) {
      AppSettingsUI_dragging = true;
      long rel = tx - tr.x;
      if (rel < 0) rel = 0;
      if (rel > tr.w - 1) rel = tr.w - 1;
      uint8_t v = (uint8_t)((rel * 255L) / (tr.w - 1));
      AppSettingsUI_applyBrightness(v);
    }
  } else if (AppSettingsUI_sel == 1) {
    AppSettingsUI_Rect d = AppSettingsUI_rThemeDark();
    AppSettingsUI_Rect l = AppSettingsUI_rThemeLight();
    if (tx >= d.x && tx <= d.x + d.w && ty >= d.y && ty <= d.y + d.h) {
      AppSettingsUI_applyTheme(Theme_Dark);
    } else if (tx >= l.x && tx <= l.x + l.w && ty >= l.y && ty <= l.y + l.h) {
      AppSettingsUI_applyTheme(Theme_Light);
    }
  } else if (AppSettingsUI_sel == 2) {
    AppSettingsUI_Rect sw = AppSettingsUI_rAccentSwatch();
    if (tx >= sw.x - 80 && tx <= sw.x + sw.w && ty >= sw.y - 8 &&
        ty <= sw.y + sw.h + 8) {
      uint8_t next = ((*s).accentColor + 1) % ACCENT_PRESET_COUNT;
      AppSettingsUI_applyAccent(next);
    }
  } else if (AppSettingsUI_sel == 3) {
    AppSettingsUI_Rect tr = AppSettingsUI_rSwitchTrack(3);
    if (tx >= tr.x && tx <= tr.x + tr.w && ty >= tr.y - 10 &&
        ty <= tr.y + tr.h + 10) {
      uint32_t now = sys_monotonic_ms();
      if (now - lastToggleMs > 120) {
        AppSettingsUI_applyNeoPixel(!(*s).neopixelEnabled);
        lastToggleMs = now;
      }
    }
  } else if (AppSettingsUI_sel == 4) {
    AppSettingsUI_Rect tr = AppSettingsUI_rSwitchTrack(4);
    if (tx >= tr.x && tx <= tr.x + tr.w && ty >= tr.y - 10 &&
        ty <= tr.y + tr.h + 10) {
      uint32_t now = sys_monotonic_ms();
      if (now - lastToggleMs > 120) {
        bool autoScan = ((*s).autoWifiScan || (*s).autoBleScan);
        AppSettingsUI_applyAutoScan(!autoScan);
        lastToggleMs = now;
      }
    }
  }
}

void AppSettingsUI_setup(void) {
  applyThemeToPalette((*settings()).theme);
  AppSettingsUI_buildPalette();
  setBrightness((*settings()).brightness);
  AppSettingsUI_sel = 0;
  AppSettingsUI_dirtySettings = false;
  AppSettingsUI_uiDirty = false;
  AppSettingsUI_dragging = false;

  AppSettingsUI_drawAll();
}

void AppSettingsUI_loop(void) {
  bool changedByButtons = false;

  static bool upWasDown = false;
  static bool downWasDown = false;
  static bool leftWasDown = false;
  static bool rightWasDown = false;
  static bool selectWasDown = false;
  static uint32_t lastNavMs = 0;
  static uint32_t lastActionMs = 0;
  const uint32_t NAV_DEBOUNCE_MS = 140;
  const uint32_t ACTION_DEBOUNCE_MS = 140;

  uint32_t now = sys_monotonic_ms();

  bool upNow = isButtonPressed(BTN_UP);
  bool downNow = isButtonPressed(BTN_DOWN);
  bool leftNow = isButtonPressed(BTN_LEFT);
  bool rightNow = isButtonPressed(BTN_RIGHT);
  bool selectNow = isButtonPressed(BTN_SELECT);

  if (selectNow && !selectWasDown &&
      (now - lastActionMs > ACTION_DEBOUNCE_MS)) {
    feature_exit_requested = true;
    lastActionMs = now;
    return;
  }

  if (upNow && !upWasDown && (now - lastNavMs > NAV_DEBOUNCE_MS)) {
    AppSettingsUI_sel =
        (AppSettingsUI_sel + AppSettingsUI_N - 1) % AppSettingsUI_N;
    changedByButtons = true;
    lastNavMs = now;
  }
  if (downNow && !downWasDown && (now - lastNavMs > NAV_DEBOUNCE_MS)) {
    AppSettingsUI_sel = (AppSettingsUI_sel + 1) % AppSettingsUI_N;
    changedByButtons = true;
    lastNavMs = now;
  }

  if (leftNow && !leftWasDown && (now - lastActionMs > ACTION_DEBOUNCE_MS)) {
    __auto_type s = &((*settings()));
    if (AppSettingsUI_sel == 0 && (*s).brightness > 0) {
      AppSettingsUI_applyBrightness((*s).brightness > 8 ? (*s).brightness - 8
                                                        : 0);
    } else if (AppSettingsUI_sel == 1) {
      AppSettingsUI_applyTheme(Theme_Dark);
    } else if (AppSettingsUI_sel == 2) {
      AppSettingsUI_applyAccent(((*s).accentColor + ACCENT_PRESET_COUNT - 1) %
                                ACCENT_PRESET_COUNT);
    } else if (AppSettingsUI_sel == 3) {
      AppSettingsUI_applyNeoPixel(false);
    } else if (AppSettingsUI_sel == 4) {
      AppSettingsUI_applyAutoScan(false);
    }
    changedByButtons = true;
    lastActionMs = now;
  }
  if ((rightNow && !rightWasDown) &&
      (now - lastActionMs > ACTION_DEBOUNCE_MS)) {
    __auto_type s = &((*settings()));
    if (AppSettingsUI_sel == 0 && (*s).brightness < 255) {
      AppSettingsUI_applyBrightness((*s).brightness + 8);
    } else if (AppSettingsUI_sel == 1) {
      AppSettingsUI_applyTheme(Theme_Light);
    } else if (AppSettingsUI_sel == 2) {
      AppSettingsUI_applyAccent(((*s).accentColor + 1) % ACCENT_PRESET_COUNT);
    } else if (AppSettingsUI_sel == 3) {
      AppSettingsUI_applyNeoPixel(true);
    } else if (AppSettingsUI_sel == 4) {
      AppSettingsUI_applyAutoScan(true);
    }
    changedByButtons = true;
    lastActionMs = now;
  }

  upWasDown = upNow;
  downWasDown = downNow;
  leftWasDown = leftNow;
  rightWasDown = rightNow;
  selectWasDown = selectNow;

  AppSettingsUI_handleTouch();

  if (changedByButtons || AppSettingsUI_uiDirty)
    AppSettingsUI_redrawIfChanged();

  sys_sleep_ms(2);
}
