#include "gps.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>

#include "ble_service.h"
#include "cJSON.h"
#include "display.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gnss_receiver.h"
#include "icon.h"
#include "keyboard_ui.h"
#include "limits.h"
#include "mbedtls/base64.h"
#include "network.h"
#include "shared.h"
#include "text_format.h"
#include "touch_ui.h"
#include "ui_utils.h"

#ifndef GPS_UART_NUM
#define GPS_UART_NUM 2
#endif
#ifndef GPS_UART_BAUD
#define GPS_UART_BAUD 9600
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static gnss_parser_t gps = GNSS_PARSER_INITIALIZER;
static gnss_receiver_t gps_receiver;
static sprite_t gScanPanel = {.parent_ = &tft,
                              .colorDepth_ = 16,
                              .textFg_ = TFT_WHITE,
                              .textBg_ = TFT_BLACK,
                              .textSize_ = 1,
                              .textFont_ = 1};
static bool gScanPanelReady = false;
static int gScanPanelAllocW = 0;
static int gScanPanelAllocH = 0;

enum { kScanSpriteHMin = 200 };

static bool ensureScanPanelSprite(int panelW, int panelH) {
  if (panelW <= 0 || panelH <= 32) {
    return false;
  }
  if (gScanPanelReady && panelW == gScanPanelAllocW &&
      gScanPanelAllocH <= panelH && gScanPanelAllocH >= kScanSpriteHMin) {
    return true;
  }

  if (gScanPanelReady) {
    sprite_delete_sprite(&(gScanPanel));
    gScanPanelReady = false;
    gScanPanelAllocW = gScanPanelAllocH = 0;
  }

  for (int h = panelH; h >= kScanSpriteHMin; h -= 18) {
    {
      __auto_type c_first = 8;
      __typeof__(c_first) c_values[] = {8, 16};
      for (size_t c_range_index = 0; c_range_index < DIV_ARRAY_SIZE(c_values);
           ++c_range_index) {
        __auto_type bpp = (c_values[c_range_index]);
        {
          sprite_set_color_depth(&(gScanPanel), bpp);
          if (sprite_create_sprite(&(gScanPanel), panelW, h)) {
            gScanPanelReady = true;
            gScanPanelAllocW = panelW;
            gScanPanelAllocH = h;
            return true;
          }
          sys_sleep_ms(20);
        }
      }
    }
  }

  sprite_set_color_depth(&(gScanPanel), 16);
  gScanPanelAllocW = gScanPanelAllocH = 0;
  return false;
}

static bool gWardSuppressBottomTouchExit = false;

enum { kRedrawMs = 450 };

enum { kGfxTop = 22 };
enum { kSnrBarMax = 50 };

enum { kSkyRDefault = 52 };
enum { kSkyCyDefault = 110 };
enum { kSkyCyBig = 118 };
enum { kSkyRBig = 70 };
enum { kListRowsVisible = 9 };

enum { kUiPad = 5 };
enum { kUiGap = 3 };
enum { kHeaderY = 2 };
enum { kTabsY = 24 };
enum { kCardsY = 46 };
enum { kCardH = 29 };
enum { kBodyTop = 82 };

enum { kNavCooldownMs = 520 };

typedef uint8_t ViewMode;
enum { ViewMode_Combined = 0, ViewMode_SkyOnly, ViewMode_ListOnly };
ViewMode viewMode = ViewMode_Combined;
int listScroll = 0;
uint32_t lastViewBtnMs = 0;

static bool prevHwLeft = false;
static bool prevHwRight = false;
static bool prevHwUp = false;
static bool prevHwDown = false;
static bool touchNavArm = true;

const char* systemShortTag(uint8_t sys) {
  switch (sys) {
    case GNSS_SYS_GPS:
      return "GPS";
    case GNSS_SYS_GLO:
      return "GLO";
    case GNSS_SYS_GAL:
      return "GAL";
    case GNSS_SYS_BDS:
      return "BDS";
    case GNSS_SYS_QZS:
      return "QZS";
    default:
      return "?";
  }
}

uint16_t baseColorForSystem(uint8_t sys) {
  switch (sys) {
    case GNSS_SYS_GPS:
      return 0x07FF;
    case GNSS_SYS_GLO:
      return 0xFA20;
    case GNSS_SYS_GAL:
      return 0xAFE5;
    case GNSS_SYS_BDS:
      return ORANGE;
    case GNSS_SYS_QZS:
      return 0xFC9F;
    default:
      return LIGHT_GRAY;
  }
}

static uint16_t scaleRgb565(uint16_t c, int num, int den) {
  if (den <= 0) {
    return c;
  }
  uint16_t r = ((c >> 11) & 0x1F);
  uint16_t g = ((c >> 5) & 0x3F);
  uint16_t b = (c & 0x1F);
  r = (uint16_t)((r * num) / den);
  g = (uint16_t)((g * num) / den);
  b = (uint16_t)((b * num) / den);
  return (uint16_t)((r << 11) | (g << 5) | b);
}

uint16_t snrTint(uint16_t base, int16_t snr) {
  if (snr < 0) {
    return scaleRgb565(base, 10, 16);
  }
  int k = snr;
  if (k > 45) {
    k = 45;
  }
  uint8_t bump = (uint8_t)(k * 3 / 2);
  uint16_t r = ((base >> 11) & 0x1F);
  uint16_t g = ((base >> 5) & 0x3F);
  uint16_t b = (base & 0x1F);
  r = (uint16_t)DIV_MIN(31, (int)r + (bump >> 3));
  g = (uint16_t)DIV_MIN(63, (int)g + (bump >> 2));
  b = (uint16_t)DIV_MIN(31, (int)b + (bump >> 3));
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static int snrSortKey(int16_t snr) { return snr < 0 ? -999 : (int)snr; }

void sortRowsBySnr(void) {
  for (int i = 0; i + 1 < gps.state.satellite_count; ++i) {
    for (int j = i + 1; j < gps.state.satellite_count; ++j) {
      if (snrSortKey(gps.state.satellites[j].snr) >
          snrSortKey(gps.state.satellites[i].snr)) {
        gnss_satellite_t t = gps.state.satellites[i];
        gps.state.satellites[i] = gps.state.satellites[j];
        gps.state.satellites[j] = t;
      }
    }
  }
}

static int countSnrKnown(void) {
  int n = 0;
  for (int i = 0; i < gps.state.satellite_count; ++i) {
    if (gps.state.satellites[i].snr >= 0) {
      n++;
    }
  }
  return n;
}

int countInSolutionTotal(void) {
  int n = 0;
  for (int i = 0; i < gps.state.satellite_count; ++i) {
    if (gps.state.satellites[i].inSolution) {
      n++;
    }
  }
  return n;
}

void drawFixBadgeGx(sprite_t* g, const int PW, int x, int y) {
  const char* tag = "NOFIX";
  uint16_t fg = UI_WARN;
  if (gps.state.navigation_valid && gps.state.fix_quality > 0 &&
      gps.state.fix_mode >= 2) {
    fg = UI_OK;
    tag = (gps.state.fix_mode == 3) ? "FIX 3D" : "FIX 2D";
  } else if (gps.state.fix_quality > 0 && gps.state.fix_mode >= 2) {
    fg = UI_OK;
    tag = (gps.state.fix_mode == 3) ? "FIX 3D" : "FIX 2D";
  }

  sprite_set_text_font(&((*g)), 1);
  sprite_set_text_size(&((*g)), 1);
  sprite_set_text_datum(&((*g)), TL_DATUM);
  sprite_set_text_color_2(&((*g)), fg, FEATURE_BG);
  sprite_draw_string(&((*g)), tag, x, y, 0);
}

void drawSkyPlotGx(sprite_t* g, int cx, int cy, int R) {
  sprite_draw_circle(&((*g)), cx, cy, R, UI_LINE);
  for (int el = 30; el <= 60; el += 30) {
    int rr = R * (90 - el) / 90;
    if (rr > 2) {
      sprite_draw_circle(&((*g)), cx, cy, rr, LINE_Dark);
    }
  }

  sprite_draw_line(&((*g)), cx, cy - R, cx, cy + R, LINE_Dark);
  sprite_draw_line(&((*g)), cx - R, cy, cx + R, cy, LINE_Dark);

  enum { triGap = 9 };
  enum { triHalf = 4 };
  enum { triLen = 7 };
  const int nTipY = cy - R - triGap;
  const int sTipY = cy + R + triGap;
  const int wTipX = cx - R - triGap;
  const int eTipX = cx + R + triGap;

  // Compass markers are intentionally symmetric; keep labels farther out.
  sprite_fill_triangle(&((*g)), cx, nTipY, cx - triHalf, nTipY + triLen,
                       cx + triHalf, nTipY + triLen, ORANGE);
  sprite_fill_triangle(&((*g)), cx, sTipY, cx - triHalf, sTipY - triLen,
                       cx + triHalf, sTipY - triLen, ORANGE);
  sprite_fill_triangle(&((*g)), wTipX, cy, wTipX + triLen, cy - triHalf,
                       wTipX + triLen, cy + triHalf, ORANGE);
  sprite_fill_triangle(&((*g)), eTipX, cy, eTipX - triLen, cy - triHalf,
                       eTipX - triLen, cy + triHalf, ORANGE);

  sprite_set_text_font(&((*g)), 1);
  sprite_set_text_size(&((*g)), 1);
  sprite_set_text_datum(&((*g)), MC_DATUM);
  sprite_set_text_color_2(&((*g)), UI_DIM_TEXT, FEATURE_BG);
  enum { labelInset = 9 };
  sprite_draw_string(&((*g)), "N", cx, cy - R + labelInset, 0);
  sprite_draw_string(&((*g)), "S", cx, cy + R - labelInset, 0);
  sprite_draw_string(&((*g)), "W", cx - R + labelInset, cy, 0);
  sprite_draw_string(&((*g)), "E", cx + R - labelInset, cy, 0);

  sprite_set_text_datum(&((*g)), TC_DATUM);
  sprite_draw_string(&((*g)), "Zenith", cx, cy - 10, 0);

  for (int i = 0; i < gps.state.satellite_count; ++i) {
    int16_t el = gps.state.satellites[i].elev;
    int16_t az = gps.state.satellites[i].azim;
    bool posEstimated = false;

    if (az == 360) {
      az = 0;
    }

    /* Many receivers omit elev/az until the SV is tracked; still show a dot */
    if (el < 0 || el > 90 || az < 0 || az > 359) {
      uint32_t seed = ((uint32_t)gps.state.satellites[i].prn * 251u + 17u) ^
                      ((uint32_t)gps.state.satellites[i].system * 131071u +
                       (uint32_t)i * 65537u);
      seed ^= seed >> 16;
      seed *= 2654435761u;
      az = (int16_t)(seed % 360u);
      el = (int16_t)(12 + ((seed >> 11) % 34u));
      posEstimated = true;
    }

    double azRad = (double)az * M_PI / 180.0;
    float rr = (float)R * (float)(90 - el) / 90.f;
    int sx = cx + (int)(rr * sin(azRad));
    int sy = cy - (int)(rr * cos(azRad));

    uint16_t bc = baseColorForSystem(gps.state.satellites[i].system);
    uint16_t fill = snrTint(bc, gps.state.satellites[i].snr);
    /* Elev/az missing: keep hue, wash slightly darker (not flat grey) */
    if (posEstimated) {
      fill = scaleRgb565(fill, 12, 16);
    }

    int mr = 4;
    if (!posEstimated && gps.state.satellites[i].snr >= 0) {
      mr += gps.state.satellites[i].snr / 20;
      if (mr > 7) {
        mr = 7;
      }
    }

    sprite_fill_circle(&((*g)), sx, sy, mr, fill);
    sprite_draw_circle(&((*g)), sx, sy, mr,
                       posEstimated ? UI_DIM_TEXT : (uint16_t)BLACK);

    if (gps.state.satellites[i].inSolution) {
      sprite_draw_circle(&((*g)), sx, sy, mr + 2, WHITE);
    }
  }
}

void drawLegendStripGx(sprite_t* g, const int PW, int y) {
  sprite_set_text_font(&((*g)), 1);
  sprite_set_text_size(&((*g)), 1);
  sprite_set_text_datum(&((*g)), TL_DATUM);

  struct {
    uint8_t sys;
    const char* name;
  } items[] = {{GNSS_SYS_GPS, "GPS"},
               {GNSS_SYS_GLO, "GL"},
               {GNSS_SYS_GAL, "GA"},
               {GNSS_SYS_BDS, "BD"},
               {GNSS_SYS_QZS, "QZ"}};

  int x = 4;
  for (unsigned i = 0; i < DIV_ARRAY_SIZE(items); ++i) {
    uint16_t c = baseColorForSystem(items[i].sys);
    sprite_fill_rect(&((*g)), x, y + 1, 7, 7, c);
    sprite_draw_rect(&((*g)), x, y + 1, 7, 7, UI_LINE);
    sprite_set_text_color_2(&((*g)), UI_DIM_TEXT, FEATURE_BG);
    sprite_draw_string(&((*g)), items[i].name, x + 9, y, 0);
    x += 36;
    if (x > PW - 50) {
      break;
    }
  }

  sprite_set_text_color_2(&((*g)), UI_DIM_TEXT, FEATURE_BG);
  sprite_draw_string(&((*g)), "ring=fix", x + 2, y, 0);
}

void drawSnrTableGx(sprite_t* g, const int PW, const int PH, int yStart,
                    int maxRows, int startIdx, int footerReserve) {
  sortRowsBySnr();
  enum { barX = 54 };
  const int barW = PW - barX - 30;
  enum { rowH = 13 };

  int y = yStart;
  int drawn = 0;
  const int ymax = PH - footerReserve;
  for (int i = startIdx; i < gps.state.satellite_count && drawn < maxRows;
       ++i) {
    if (y + rowH > ymax) {
      break;
    }

    char tag[14];
    snprintf(tag, sizeof(tag), "%s%02u",
             systemShortTag(gps.state.satellites[i].system),
             (unsigned)gps.state.satellites[i].prn);

    sprite_set_text_font(&((*g)), 1);
    sprite_set_text_size(&((*g)), 1);
    sprite_set_text_datum(&((*g)), TL_DATUM);
    sprite_set_text_color_2(
        &((*g)),
        snrTint(baseColorForSystem(gps.state.satellites[i].system),
                gps.state.satellites[i].snr),
        FEATURE_BG);
    sprite_draw_string(&((*g)), tag, 2, y, 0);

    int snr = gps.state.satellites[i].snr < 0 ? 0 : gps.state.satellites[i].snr;
    if (snr > kSnrBarMax) {
      snr = kSnrBarMax;
    }
    int fill = (barW * snr) / kSnrBarMax;

    sprite_fill_rect(&((*g)), barX, y + 2, barW, 8, UI_LINE);
    if (fill > 0) {
      uint16_t bc = baseColorForSystem(gps.state.satellites[i].system);
      sprite_fill_rect(&((*g)), barX, y + 2, fill, 8,
                       snrTint(bc, gps.state.satellites[i].snr));
      if (gps.state.satellites[i].inSolution) {
        sprite_draw_rect(&((*g)), barX, y + 2, fill, 8, WHITE);
      }
    }

    sprite_set_text_color_2(&((*g)), UI_DIM_TEXT, FEATURE_BG);
    char sb[8];
    if (gps.state.satellites[i].snr >= 0) {
      snprintf(sb, sizeof(sb), "%2d", gps.state.satellites[i].snr);
    } else {
      strncpy(sb, "--", sizeof(sb));
      sb[sizeof(sb) - 1] = '\0';
    }
    sprite_draw_string(&((*g)), sb, barX + barW + 4, y, 0);

    y += rowH;
    drawn++;
  }
}

void drawPanelFrameGx(sprite_t* g, int x, int y, int w, int h,
                      uint16_t outline) {
  sprite_fill_round_rect(&((*g)), x, y, w, h, 6,
                         UI.bg == BG_Dark ? BLACK : UI_BG);
  sprite_draw_round_rect(&((*g)), x, y, w, h, 6, outline);
}

void drawMetricCardGx(sprite_t* g, int x, int y, int w, const char* label,
                      const char* value, uint16_t valueColor,
                      const char* value2) {
  drawPanelFrameGx(&((*g)), x, y, w, kCardH, UI_LINE);
  sprite_set_text_font(&((*g)), 1);
  sprite_set_text_size(&((*g)), 1);
  sprite_set_text_datum(&((*g)), TL_DATUM);
  sprite_set_text_color_2(&((*g)), UI_DIM_TEXT, FEATURE_BG);
  sprite_draw_string(&((*g)), label, x + 4, y + 2, 0);
  sprite_set_text_color_2(&((*g)), valueColor, FEATURE_BG);
  if (value2 != NULL && value2[0] != '\0') {
    sprite_draw_string(&((*g)), value, x + 4, y + 11, 0);
    sprite_draw_string(&((*g)), value2, x + 4, y + 19, 0);
  } else {
    sprite_draw_string(&((*g)), value, x + 4, y + 15, 0);
  }
}

/** Same x positions and width as drawMetricCardGx columns so tabs align with
 * cards. */
void drawModeTabsGx(sprite_t* g, int x0, int x1, int x2, int colW, int y) {
  const char* labels[] = {"Split", "Sky", "Signals"};
  const int xs[3] = {x0, x1, x2};
  const int active = (int)viewMode;
  enum { tabH = 15 };

  for (int i = 0; i < 3; ++i) {
    const int x = xs[i];
    const bool selected = (i == active);
    uint16_t fill =
        selected ? UI_ACCENT : (UI.bg == BG_Dark ? DARK_GRAY : LIGHT_GRAY);
    uint16_t fg = selected ? WHITE : UI_DIM_TEXT;
    sprite_fill_round_rect(&((*g)), x, y, colW, tabH, 5, fill);
    sprite_draw_round_rect(&((*g)), x, y, colW, tabH, 5,
                           selected ? UI_OK : UI_LINE);
    sprite_set_text_font(&((*g)), 1);
    sprite_set_text_size(&((*g)), 1);
    sprite_set_text_datum(&((*g)), MC_DATUM);
    sprite_set_text_color_2(&((*g)), fg, fill);
    sprite_draw_string(&((*g)), labels[i], x + colW / 2, y + tabH / 2, 0);
  }
}

void drawFooterHintGx(sprite_t* g, const int PW, const int PH, int footerH) {
  if (featureHasTouchNavBar()) {
    return;
  }
  const int y = PH - footerH;
  sprite_fill_rect(&((*g)), 0, y, PW, footerH,
                   UI.bg == BG_Dark ? DARK_GRAY : UI_LABLE);
  sprite_draw_line(&((*g)), 0, y, PW, y, UI_ACCENT);

  sprite_set_text_font(&((*g)), 1);
  sprite_set_text_size(&((*g)), 1);
  sprite_set_text_datum(&((*g)), TL_DATUM);
  sprite_set_text_color_2(&((*g)), UI_TEXT,
                          UI.bg == BG_Dark ? DARK_GRAY : UI_LABLE);

  if (viewMode == ViewMode_ListOnly) {
    sprite_draw_string(&((*g)), "Touch: L/R view  center scroll", 5, y + 5, 0);
  } else {
    sprite_draw_string(&((*g)), "Touch: edge L/R switches view", 5, y + 5, 0);
  }

  sprite_set_text_color_2(&((*g)), UI_DIM_TEXT,
                          UI.bg == BG_Dark ? DARK_GRAY : UI_LABLE);
  sprite_draw_string(&((*g)), "SELECT or bottom tap exits", 5, y + 18, 0);
}

void clampListScroll(void) {
  if (listScroll < 0) {
    listScroll = 0;
  }
  int maxS = gps.state.satellite_count - kListRowsVisible;
  if (maxS < 0) {
    maxS = 0;
  }
  if (listScroll > maxS) {
    listScroll = maxS;
  }
}

static const char* viewModeTag(ViewMode m) {
  switch (m) {
    case ViewMode_Combined:
      return "Split";
    case ViewMode_SkyOnly:
      return "Sky";
    case ViewMode_ListOnly:
      return "Signals";
    default:
      return "?";
  }
}

static inline bool navCooldownElapsed(uint32_t t) {
  return ((uint32_t)(t - lastViewBtnMs)) >= kNavCooldownMs;
}

static void resetSatScannerNavInputState(void) {
  prevHwLeft = isButtonPressed(BTN_LEFT);
  prevHwRight = isButtonPressed(BTN_RIGHT);
  prevHwUp = isButtonPressed(BTN_UP);
  prevHwDown = isButtonPressed(BTN_DOWN);
  touchNavArm = !isTouchDownDismiss(120);
}

/** @return True if redraw should occur immediately */
static void gps_bump_mode(int d) {
  int v = (int)viewMode + d;
  v = (v % 3 + 3) % 3;
  viewMode = (ViewMode)(uint8_t)v;
  listScroll = 0;
  lastViewBtnMs = sys_monotonic_ms();
}
bool pollViewNavigation(void) {
  uint32_t t = sys_monotonic_ms();
  bool redraw = false;

  const bool L = isButtonPressed(BTN_LEFT);
  const bool R = isButtonPressed(BTN_RIGHT);
  const bool U = isButtonPressed(BTN_UP);
  const bool D = isButtonPressed(BTN_DOWN);

  const bool edgeL = L && !prevHwLeft;
  const bool edgeR = R && !prevHwRight;
  const bool edgeU = U && !prevHwUp;
  const bool edgeD = D && !prevHwDown;

  prevHwLeft = L;
  prevHwRight = R;
  prevHwUp = U;
  prevHwDown = D;

  if (navCooldownElapsed(t)) {
    if (edgeR) {
      gps_bump_mode(1);
      redraw = true;
    } else if (edgeL) {
      gps_bump_mode(-1);
      redraw = true;
    } else if (viewMode == ViewMode_ListOnly && edgeU) {
      if (listScroll > 0) {
        listScroll--;
      }
      lastViewBtnMs = t;
      redraw = true;
    } else if (viewMode == ViewMode_ListOnly && edgeD) {
      clampListScroll();
      int maxS = gps.state.satellite_count - kListRowsVisible;
      if (maxS < 0) {
        maxS = 0;
      }
      if (listScroll < maxS) {
        listScroll++;
      }
      lastViewBtnMs = t;
      redraw = true;
    }
  }

  if (!isTouchDownDismiss(120)) {
    touchNavArm = true;
    return redraw;
  }

  int tx = 0, ty = 0;
  if (!readTouchXY(&(tx), &(ty))) {
    return redraw;
  }

  const int touchBottom = featureHasTouchNavBar()
                              ? touchNavContentBottomY()
                              : (int)display_height(&(tft)) - 52;
  if (ty < kGfxTop || ty >= touchBottom) {
    return redraw;
  }

  if (!touchNavArm || !navCooldownElapsed(t)) {
    return redraw;
  }

  const int PW = display_width(&(tft));
  const int PH = gScanPanelReady ? sprite_height(&(gScanPanel))
                                 : (display_height(&(tft)) - kGfxTop);
  const int px = tx;
  const int py = ty - kGfxTop;
  if (py < 0 || py >= PH) {
    return redraw;
  }

  /* Avoid bottom strip (exit hint overlaps panel bottom). */
  if (py >= PH - 40) {
    return redraw;
  }

  const int xThird = PW / 3;

  touchNavArm = false;

  if (px < xThird) {
    gps_bump_mode(-1);
    redraw = true;
  } else if (px >= xThird * 2) {
    gps_bump_mode(1);
    redraw = true;
  } else if (viewMode == ViewMode_ListOnly) {
    lastViewBtnMs = t;
    const int mid = PH / 2;
    if (py < mid) {
      if (listScroll > 0) {
        listScroll--;
      }
    } else {
      clampListScroll();
      int maxS = gps.state.satellite_count - kListRowsVisible;
      if (maxS < 0) {
        maxS = 0;
      }
      if (listScroll < maxS) {
        listScroll++;
      }
    }
    redraw = true;
  } else {
    /* Center tap Split/Sky: no-op; finger up will re-arm touchNavArm */
    touchNavArm = true;
  }

  return redraw;
}

/** All Y coordinates relative to sprite top (below status bar). */
void renderPanelGx(sprite_t* g) {
  const int PW = sprite_width(&((*g)));
  const int PH = sprite_height(&((*g)));

  enum { footerH = 34 };
  const int footerReserve = featureHasTouchNavBar() ? 2 : footerH;
  const int cardW = (PW - (kUiPad * 2) - (kUiGap * 2)) / 3;
  const int cardX0 = kUiPad;
  const int cardX1 = cardX0 + cardW + kUiGap;
  const int cardX2 = cardX1 + cardW + kUiGap;
  sprite_fill_screen(&((*g)), FEATURE_BG);

  int skyCy = kSkyCyDefault;
  int skyR = kSkyRDefault;
  uint32_t now = sys_monotonic_ms();
  const bool staleGsv =
      (gps.state.last_satellite_ms != 0) &&
      (((uint32_t)(now - gps.state.last_satellite_ms)) > 8000);

  sprite_set_text_font(&((*g)), 1);
  sprite_set_text_size(&((*g)), 1);
  sprite_set_text_datum(&((*g)), TL_DATUM);
  sprite_set_text_color_2(&((*g)), UI_TEXT, FEATURE_BG);
  sprite_draw_string(&((*g)), "GNSS Satellites", kUiPad, kHeaderY, 0);

  drawModeTabsGx(&((*g)), cardX0, cardX1, cardX2, cardW, kTabsY);

  drawFixBadgeGx(&((*g)), PW, PW - 48, 5);

  if (staleGsv) {
    sprite_set_text_datum(&((*g)), TR_DATUM);
    sprite_set_text_color_2(&((*g)), UI_WARN, FEATURE_BG);
    sprite_draw_string(&((*g)), "no GSV", PW - 5, 16, 0);
    sprite_set_text_datum(&((*g)), TL_DATUM);
  }

  if (viewMode == ViewMode_SkyOnly) {
    skyCy = 139;
    skyR = 74;
    sprite_set_text_datum(&((*g)), TL_DATUM);
    sprite_set_text_font(&((*g)), 1);
    sprite_set_text_size(&((*g)), 1);
    sprite_set_text_color_2(&((*g)), UI_DIM_TEXT, FEATURE_BG);
    sprite_draw_string(&((*g)), "Center=overhead  Edge=horizon", kUiPad,
                       kBodyTop - 39, 0);

    drawSkyPlotGx(&((*g)), PW / 2, skyCy, skyR);
    drawLegendStripGx(&((*g)), PW, skyCy + skyR + 16);

  } else if (viewMode == ViewMode_ListOnly) {
    clampListScroll();

    char lineUtc[42];
    div_copy_string(lineUtc, sizeof(lineUtc), gps.state.utc);
    char dopLine1[48];
    char dopLine2[48];
    int inFix = countInSolutionTotal();
    if (gps.state.hdop >= 0.f && gps.state.pdop >= 0.f) {
      snprintf(dopLine1, sizeof(dopLine1), "H%.1f", gps.state.hdop);
      snprintf(dopLine2, sizeof(dopLine2), "P%.1f", gps.state.pdop);
    } else {
      dopLine2[0] = '\0';
      snprintf(dopLine1, sizeof(dopLine1), "SV%02d S%02d",
               gps.state.satellite_count, countSnrKnown());
    }

    drawMetricCardGx(&((*g)), cardX0, kCardsY, cardW, "UTC", lineUtc, UI_TEXT,
                     NULL);
    drawMetricCardGx(&((*g)), cardX1, kCardsY, cardW, "DOP", dopLine1, UI_OK,
                     dopLine2[0] ? dopLine2 : NULL);
    char countBuf[48];
    snprintf(countBuf, sizeof(countBuf), "U%u F%d",
             (unsigned)gps.state.satellites_used, inFix);
    drawMetricCardGx(&((*g)), cardX2, kCardsY, cardW, "FIX", countBuf,
                     UI_ACCENT, NULL);

    sprite_set_text_font(&((*g)), 1);
    sprite_set_text_size(&((*g)), 1);
    sprite_set_text_datum(&((*g)), TL_DATUM);
    sprite_set_text_color_2(&((*g)), UI_TEXT, FEATURE_BG);
    sprite_draw_string(&((*g)), "Strongest signals", kUiPad, kBodyTop, 0);

    drawSnrTableGx(&((*g)), PW, PH, kBodyTop + 15, kListRowsVisible - 2,
                   listScroll, footerReserve);

  } else {
    char lineUtc[42];
    div_copy_string(lineUtc, sizeof(lineUtc), gps.state.utc);
    int inFix = countInSolutionTotal();
    char dopLine1[48];
    char dopLine2[48];
    dopLine2[0] = '\0';
    if (gps.state.hdop >= 0.f && gps.state.pdop >= 0.f) {
      snprintf(dopLine1, sizeof(dopLine1), "H%.1f", gps.state.hdop);
      snprintf(dopLine2, sizeof(dopLine2), "P%.1f", gps.state.pdop);
    } else if (gps.state.hdop >= 0.f) {
      snprintf(dopLine1, sizeof(dopLine1), "H%.1f U%u", gps.state.hdop,
               (unsigned)gps.state.satellites_used);
    } else {
      snprintf(dopLine1, sizeof(dopLine1), "SV%02d S%02d",
               gps.state.satellite_count, countSnrKnown());
    }

    char fixBuf[48];
    snprintf(fixBuf, sizeof(fixBuf), "U%u F%d",
             (unsigned)gps.state.satellites_used, inFix);
    drawMetricCardGx(&((*g)), cardX0, kCardsY, cardW, "UTC", lineUtc, UI_TEXT,
                     NULL);
    drawMetricCardGx(&((*g)), cardX1, kCardsY, cardW, "DOP", dopLine1, UI_OK,
                     dopLine2[0] ? dopLine2 : NULL);
    drawMetricCardGx(&((*g)), cardX2, kCardsY, cardW, "FIX", fixBuf, UI_ACCENT,
                     NULL);

    if (!isnan(gps.state.latitude) && !isnan(gps.state.longitude)) {
      char posBuf[40];
      snprintf(posBuf, sizeof(posBuf), "%.4f  %.4f", gps.state.latitude,
               gps.state.longitude);
      sprite_set_text_font(&((*g)), 1);
      sprite_set_text_size(&((*g)), 1);
      sprite_set_text_datum(&((*g)), TL_DATUM);
      sprite_set_text_color_2(&((*g)), UI_DIM_TEXT, FEATURE_BG);
      sprite_draw_string(&((*g)), posBuf, kUiPad, kBodyTop - 3, 0);
    } else {
      sprite_set_text_font(&((*g)), 1);
      sprite_set_text_size(&((*g)), 1);
      sprite_set_text_datum(&((*g)), TL_DATUM);
      sprite_set_text_color_2(&((*g)), UI_DIM_TEXT, FEATURE_BG);
      sprite_draw_string(&((*g)), "Waiting for position / GGA", kUiPad,
                         kBodyTop - 3, 0);
    }

    skyCy = 151;
    skyR = kSkyRDefault;

    drawSkyPlotGx(&((*g)), PW / 2, skyCy, skyR);

    int legendY = skyCy + skyR + 18;
    if (legendY > PH - 75) {
      legendY = PH - 74;
    }
    drawLegendStripGx(&((*g)), PW, legendY);
    drawSnrTableGx(&((*g)), PW, PH, legendY + 13, 2, 0, footerH);
  }

  drawFooterHintGx(&((*g)), PW, PH, footerH);
}

void redrawFeaturePanel(bool statusBarForceFull) {
  float v = readBatteryVoltage();
  drawStatusBar(v, statusBarForceFull, false);

  static uint32_t s_lastSpriteRetryMs = 0;
  if (!gScanPanelReady) {
    const uint32_t tn = sys_monotonic_ms();
    if ((uint32_t)(tn - s_lastSpriteRetryMs) >= 800u) {
      s_lastSpriteRetryMs = tn;
      (void)ensureScanPanelSprite(display_width(&(tft)),
                                  display_height(&(tft)) - kGfxTop);
    }
  }

  if (gScanPanelReady) {
    renderPanelGx(&(gScanPanel));
    sprite_push_sprite(&(gScanPanel), 0, kGfxTop);
    const int sh = sprite_height(&(gScanPanel));
    const int fullH = display_height(&(tft)) - kGfxTop;
    if (sh < fullH) {
      display_fill_rect(&(tft), 0, kGfxTop + sh, display_width(&(tft)),
                        fullH - sh, FEATURE_BG);
    }
    return;
  }

  display_fill_rect(&(tft), 0, kGfxTop, display_width(&(tft)),
                    display_height(&(tft)) - kGfxTop, FEATURE_BG);
  display_set_text_datum(&(tft), MC_DATUM);
  display_set_text_font(&(tft), 1);
  display_set_text_color_2(&(tft), UI_WARN, FEATURE_BG);
  display_draw_string(&(tft), "Display buffer failed",
                      display_width(&(tft)) / 2,
                      kGfxTop + (display_height(&(tft)) - kGfxTop) / 2, 0);
  display_set_text_font(&(tft), 1);
  display_set_text_datum(&(tft), TL_DATUM);
}

bool touchRequestsExit(void) {
  if (featureHasTouchNavBar()) {
    return false;
  }
  if (gWardSuppressBottomTouchExit) {
    return false;
  }
  int x = 0, y = 0;
  if (!readTouchXY(&(x), &(y))) {
    return false;
  }
  return y >= (int)display_height(&(tft)) - 48;
}

bool shouldExit(void) {
  return feature_exit_requested || isButtonPressed(BTN_SELECT) ||
         touchRequestsExit();
}

void drainButtons(void) {
  sys_sleep_ms(180);
  for (int i = 0; i < 80; i++) {
    if (!isButtonPressed(BTN_SELECT)) {
      break;
    }
    sys_sleep_ms(10);
  }
}

void GpsSatelliteScanner_session(void) {
  GpsWardriver_stopBackgroundIfRunning();
  gnss_parser_reset(&gps);
  viewMode = ViewMode_Combined;
  listScroll = 0;
  lastViewBtnMs = 0;

  resetSatScannerNavInputState();

  gnss_receiver_close(&gps_receiver);
  gnss_receiver_open(&gps_receiver, &gps, GPS_UART_NUM, GPS_UART_BAUD,
                     GPS_UART_RX);

  const int panelH = display_height(&(tft)) - kGfxTop;
  (void)ensureScanPanelSprite(display_width(&(tft)), panelH);

  uint32_t lastDraw = 0;
  uint32_t lastStatusForced = 0;
  bool firstPanelPaint = true;

  while (!shouldExit()) {
    gnss_receiver_poll(&gps_receiver);

    uint32_t now = sys_monotonic_ms();
    const bool forced = pollViewNavigation();
    if (forced || (now - lastDraw >= kRedrawMs)) {
      lastDraw = now;

      const bool statusFull =
          firstPanelPaint || (((uint32_t)(now - lastStatusForced)) >= 2200u);
      if (statusFull) {
        lastStatusForced = now;
      }
      redrawFeaturePanel(statusFull);
      firstPanelPaint = false;
    }

    sys_sleep_ms(2);
  }

  // often prevents createSprite() from succeeding when re-entering this screen.

  gnss_receiver_close(&gps_receiver);
  drainButtons();
}

#ifndef WARDRIVE_SCAN_INTERVAL_MS
#define WARDRIVE_SCAN_INTERVAL_MS 4200
#endif
#ifndef WARDRIVE_UI_INTERVAL_MS
#define WARDRIVE_UI_INTERVAL_MS 120
#endif
#ifndef WARDRIVE_MAX_APS_PER_SCAN
#define WARDRIVE_MAX_APS_PER_SCAN 48
#endif

/** Same top strip style as the other features: 20px status bar + 16px DARK_GRAY
 * icon row. */
enum { GpsWardriver_kWardIconRowY = 20 };
enum { GpsWardriver_kWardIconRowH = 16 };
enum { GpsWardriver_kWardIconSz = 16 };
enum {
  GpsWardriver_kWardBodyY = GpsWardriver_kWardIconRowY +
  GpsWardriver_kWardIconRowH + 4
};
enum { GpsWardriver_kWardUiRadius = 3 };
enum { GpsWardriver_kWardInnerPad = 5 };

static text_t GpsWardriver_wardFitTextWidth(const char* text, int maxW) {
  text_t t = TEXT(text ? text : "");
  if (maxW <= 0) {
    return t;
  }
  while (strlen((t).data) > 0 &&
         display_text_width(&(tft), (t).data, 0) > maxW) {
    text_erase(&(t), strlen((t).data) - 1, SIZE_MAX);
  }
  return t;
}

static int GpsWardriver_wardContentBottom(void) {
  return featureHasTouchNavBar() ? touchNavContentBottomY()
                                 : (int)display_height(&(tft));
}

enum { GpsWardriver_kWardCardIconCol = 24 };
enum { GpsWardriver_kWardCardIconSz = 16 };
enum { GpsWardriver_kWardTextLineH = 8 };

static int GpsWardriver_wardCardIconY(int cardY, int cardH) {
  return cardY + (cardH - GpsWardriver_kWardCardIconSz) / 2;
}

static int GpsWardriver_wardCardTextX(int cardPad) {
  return cardPad + GpsWardriver_kWardCardIconCol;
}

static int GpsWardriver_wardCardTextMaxW(int cardPad, int cardW,
                                         int rightReserve) {
  const int textX = GpsWardriver_wardCardTextX(cardPad);
  return cardW - (textX - cardPad) - GpsWardriver_kWardInnerPad - rightReserve;
}

/** Place `lineCount` text rows evenly inside card vertical padding (font height
 * 8px). */
static void GpsWardriver_wardCardTextRowYs(int cardY, int cardH, int lineCount,
                                           int rowY[3]) {
  const int innerTop = cardY + GpsWardriver_kWardInnerPad;
  const int innerBot = cardY + cardH - GpsWardriver_kWardInnerPad;
  if (lineCount <= 1) {
    rowY[0] = innerTop;
    return;
  }
  if (lineCount == 2) {
    rowY[0] = innerTop;
    rowY[1] = innerBot - GpsWardriver_kWardTextLineH;
    return;
  }
  const int innerH = innerBot - innerTop;
  const int totalText = lineCount * GpsWardriver_kWardTextLineH;
  const int gap = (innerH - totalText) / (lineCount - 1);
  for (int i = 0; i < lineCount; ++i) {
    rowY[i] = innerTop + i * (GpsWardriver_kWardTextLineH + gap);
  }
}

/** Settings body layout (must match `wardLayoutSettingsButtons` + draw). */
enum { GpsWardriver_kWSetBtnH = 22 };
enum { GpsWardriver_kWSetPad = 4 };
enum { GpsWardriver_kWSetGap = 4 };
enum { GpsWardriver_kWSetSectionGap = 8 };
enum { GpsWardriver_kWSetCardBtnGap = 4 };
enum { GpsWardriver_kWSetCardH = 20 };
enum { GpsWardriver_kWSetHeaderH = 20 };

typedef struct GpsWardriver_WardSetLayout {
  int headerY;
  int modeBtnY;
  int intervalCardY;
  int intervalBtnY;
  int capCardY;
  int capBtnY;
} GpsWardriver_WardSetLayout;
#define GpsWardriver_WardSetLayout_INITIALIZER {0}
;

static GpsWardriver_WardSetLayout GpsWardriver_wardComputeSetLayout(void) {
  GpsWardriver_WardSetLayout L = GpsWardriver_WardSetLayout_INITIALIZER;
  const int bodyTop = GpsWardriver_kWardBodyY + 4;

  L.headerY = bodyTop;
  L.modeBtnY =
      L.headerY + GpsWardriver_kWSetHeaderH + GpsWardriver_kWSetSectionGap;
  L.intervalCardY =
      L.modeBtnY + GpsWardriver_kWSetBtnH + GpsWardriver_kWSetSectionGap;
  L.intervalBtnY =
      L.intervalCardY + GpsWardriver_kWSetCardH + GpsWardriver_kWSetCardBtnGap;
  L.capCardY =
      L.intervalBtnY + GpsWardriver_kWSetBtnH + GpsWardriver_kWSetSectionGap;
  L.capBtnY =
      L.capCardY + GpsWardriver_kWSetCardH + GpsWardriver_kWSetCardBtnGap;
  return L;
}

static void GpsWardriver_wardDrawSetHeader(
    int pad, int cardW, const GpsWardriver_WardSetLayout* layout) {
  const int iconY =
      (*layout).headerY +
      (GpsWardriver_kWSetHeaderH - GpsWardriver_kWardCardIconSz) / 2;
  const int textX = GpsWardriver_wardCardTextX(pad);
  const int textY =
      (*layout).headerY +
      (GpsWardriver_kWSetHeaderH - GpsWardriver_kWardTextLineH) / 2;
  display_fill_round_rect(&(tft), pad, (*layout).headerY, cardW,
                          GpsWardriver_kWSetHeaderH, GpsWardriver_kWardUiRadius,
                          UI_FG);
  display_draw_round_rect(&(tft), pad, (*layout).headerY, cardW,
                          GpsWardriver_kWSetHeaderH, GpsWardriver_kWardUiRadius,
                          UI_LINE);
  display_fill_round_rect(&(tft), pad + 3, (*layout).headerY + 3, 2,
                          GpsWardriver_kWSetHeaderH - 6, 1, ORANGE);
  display_draw_bitmap(&(tft), pad + GpsWardriver_kWardInnerPad, iconY,
                      bitmap_icon_document_gear, GpsWardriver_kWardCardIconSz,
                      GpsWardriver_kWardCardIconSz, UI_ICON);
  display_set_text_font(&(tft), 1);
  display_set_text_color_2(&(tft), UI_TEXT, UI_FG);
  display_draw_string(&(tft), "Setup", textX, textY, 0);
}

static void GpsWardriver_wardDrawSetInlineCard(int pad, int cardW, int cardY,
                                               const char* label,
                                               const char* value,
                                               uint16_t valueColor) {
  const int textY =
      cardY + (GpsWardriver_kWSetCardH - GpsWardriver_kWardTextLineH) / 2;
  display_fill_round_rect(&(tft), pad, cardY, cardW, GpsWardriver_kWSetCardH,
                          GpsWardriver_kWardUiRadius, UI_FG);
  display_draw_round_rect(&(tft), pad, cardY, cardW, GpsWardriver_kWSetCardH,
                          GpsWardriver_kWardUiRadius, UI_LINE);
  display_set_text_font(&(tft), 1);
  display_set_text_color_2(&(tft), UI_ICON, UI_FG);
  display_set_text_datum(&(tft), TL_DATUM);
  display_draw_string(&(tft), label, pad + 4, textY, 0);
  display_set_text_datum(&(tft), TR_DATUM);
  display_set_text_color_2(&(tft), valueColor, UI_FG);
  display_draw_string(&(tft), value ? value : "", pad + cardW - 4,
                      cardY + GpsWardriver_kWSetCardH / 2, 0);
  display_set_text_datum(&(tft), TL_DATUM);
}

enum { GpsWardriver_kWardSetBtnCount = 7 };
enum { GpsWardriver_kWardSetBtnTotal = 9 };
static FeatureUI_Button
    GpsWardriver_s_wardSetBtns[GpsWardriver_kWardSetBtnTotal];

static volatile uint32_t GpsWardriver_s_cfgScanMs = WARDRIVE_SCAN_INTERVAL_MS;
static volatile int GpsWardriver_s_cfgMaxAps = WARDRIVE_MAX_APS_PER_SCAN;

/** WiFi-only, BLE-only, or both each scan interval (stored as uint8_t for
 * volatile). */
typedef uint8_t GpsWardriver_WardRadioMode;
enum {
  GpsWardriver_WardRadioMode_WiFi = 0,
  GpsWardriver_WardRadioMode_Ble = 1,
  GpsWardriver_WardRadioMode_Both = 2
};
static volatile uint8_t GpsWardriver_s_cfgRadioMode =
    (uint8_t)GpsWardriver_WardRadioMode_Both;

static TaskHandle_t GpsWardriver_s_bgTask = NULL;
/** True only inside `GpsWardriver_session` main loop (not background handoff
 * return). */
static volatile bool GpsWardriver_s_wardFgSessionForStatusBar = false;
static volatile bool GpsWardriver_s_bgStop = false;
static volatile bool GpsWardriver_s_bgLog = true;
static volatile uint32_t GpsWardriver_s_bgLines = 0;
static volatile uint32_t GpsWardriver_s_bgScans = 0;
static char GpsWardriver_s_bgPath[48] = "";

static TaskHandle_t GpsWardriver_s_fgScanTask = NULL;
static volatile bool GpsWardriver_s_fgScanStop = false;
static volatile bool GpsWardriver_s_fgScanPaused = false;
static volatile uint32_t GpsWardriver_s_fgLines = 0;
static volatile uint32_t GpsWardriver_s_fgScans = 0;

typedef uint8_t GpsWardriver_WardPage;
enum {
  GpsWardriver_WardPage_Main = 0,
  GpsWardriver_WardPage_Settings,
  GpsWardriver_WardPage_WigleCfg
};
static uint32_t GpsWardriver_s_editScanMs = WARDRIVE_SCAN_INTERVAL_MS;
static int GpsWardriver_s_editMaxAps = WARDRIVE_MAX_APS_PER_SCAN;
static GpsWardriver_WardRadioMode GpsWardriver_s_editRadioMode =
    GpsWardriver_WardRadioMode_Both;
static uint32_t GpsWardriver_s_lastTbMs = 0;
static uint32_t GpsWardriver_s_lastWardInputMs = 0;
static bool GpsWardriver_s_wardRetrySession = false;

static void GpsWardriver_wardPumpNavChrome(void) {
  if (featureHasTouchNavBar()) {
    maintainTouchNavBar();
  }
}

static void GpsWardriver_wardSetNavLabels(GpsWardriver_WardPage page, bool bgOn,
                                          bool logOn) {
  if (!featureHasTouchNavBar()) {
    return;
  }
  switch (page) {
    case GpsWardriver_WardPage_Main:
      setTouchNavLabels("Setup", logOn ? "Pause" : "Log", "Exit", NULL,
                        bgOn ? "Stop" : "RSS");
      break;
    case GpsWardriver_WardPage_Settings:
      setTouchNavLabels("Back", "Upload", "Save", NULL, "WiGLE");
      break;
    case GpsWardriver_WardPage_WigleCfg:
      setTouchNavLabels("Back", NULL, "Save", NULL, NULL);
      break;
  }
  redrawTouchButtonBar();
}

typedef uint8_t GpsWardriver_WardNavEvt;
enum {
  GpsWardriver_WardNavEvt_None,
  GpsWardriver_WardNavEvt_Back,
  GpsWardriver_WardNavEvt_Exit,
  GpsWardriver_WardNavEvt_Save,
  GpsWardriver_WardNavEvt_Setup,
  GpsWardriver_WardNavEvt_LogToggle,
  GpsWardriver_WardNavEvt_Rss,
  GpsWardriver_WardNavEvt_Upload,
  GpsWardriver_WardNavEvt_WigleCfg
};

static GpsWardriver_WardNavEvt GpsWardriver_wardPollNavButtons(
    GpsWardriver_WardPage page) {
  if (!featureHasTouchNavBar()) {
    return GpsWardriver_WardNavEvt_None;
  }
  if (page == GpsWardriver_WardPage_Main) {
    if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
      return GpsWardriver_WardNavEvt_Setup;
    }
    if (isTouchNavButtonPressedEdge(BTN_DOWN)) {
      return GpsWardriver_WardNavEvt_LogToggle;
    }
    if (isTouchNavButtonPressedEdge(BTN_SELECT)) {
      return GpsWardriver_WardNavEvt_Exit;
    }
    if (isTouchNavButtonPressedEdge(BTN_RIGHT)) {
      return GpsWardriver_WardNavEvt_Rss;
    }
  } else if (page == GpsWardriver_WardPage_Settings) {
    if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
      return GpsWardriver_WardNavEvt_Back;
    }
    if (isTouchNavButtonPressedEdge(BTN_DOWN)) {
      return GpsWardriver_WardNavEvt_Upload;
    }
    if (isTouchNavButtonPressedEdge(BTN_SELECT)) {
      return GpsWardriver_WardNavEvt_Save;
    }
    if (isTouchNavButtonPressedEdge(BTN_RIGHT)) {
      return GpsWardriver_WardNavEvt_WigleCfg;
    }
  } else if (page == GpsWardriver_WardPage_WigleCfg) {
    if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
      return GpsWardriver_WardNavEvt_Back;
    }
    if (isTouchNavButtonPressedEdge(BTN_SELECT)) {
      return GpsWardriver_WardNavEvt_Save;
    }
  }
  return GpsWardriver_WardNavEvt_None;
}

static void GpsWardriver_wardPrintWrappedPageBg(int x, int y, int maxWidth,
                                                const char* text,
                                                int lineStepPx, uint16_t fg,
                                                uint16_t bg, int firstLine,
                                                int maxLines) {
  text_t message = TEXT(text ? text : "");
  int lineNo = 0;
  int drawn = 0;
  while (strlen((message).data) > 0 && drawn < maxLines) {
    int nl = text_find_index(message, text_character('\n'), 0);
    text_t paragraph = ((nl >= 0) ? text_slice(message, 0, nl) : message);
    message = ((nl >= 0) ? text_slice(message, nl + 1, SIZE_MAX) : TEXT(""));

    if (strlen((paragraph).data) == 0) {
      if (lineNo >= firstLine) {
        drawn++;
      }
      lineNo++;
      continue;
    }

    while (strlen((paragraph).data) > 0 && drawn < maxLines) {
      int lineEnd = (int)strlen((paragraph).data);
      while (display_text_width(&(tft),
                                (text_slice(paragraph, 0, lineEnd)).data,
                                0) > maxWidth &&
             lineEnd > 1) {
        lineEnd--;
      }
      if (lineEnd < (int)strlen((paragraph).data)) {
        int lastSpace = text_rfind_index(text_slice(paragraph, 0, lineEnd),
                                         text_character(' '));
        if (lastSpace > 0) {
          lineEnd = lastSpace;
        }
      }

      if (lineNo >= firstLine) {
        display_set_text_color_2(&(tft), fg, bg);
        display_set_cursor(&(tft), x, y + drawn * lineStepPx);
        display_print(&(tft), (text_slice(paragraph, 0, lineEnd)).data);
        drawn++;
      }
      paragraph = text_slice(paragraph, lineEnd, SIZE_MAX);
      text_trim(&(paragraph));
      lineNo++;
    }
  }
}

static void GpsWardriver_wardDrawStartupFailureScreen(const char* title,
                                                      const char* msg) {
  const int scrW = display_width(&(tft));
  const int contentBottom = GpsWardriver_wardContentBottom();
  enum { pad = 8 };
  const int cardW = scrW - 2 * pad;

  display_fill_rect(&(tft), 0, GpsWardriver_kWardBodyY, scrW,
                    contentBottom - GpsWardriver_kWardBodyY, FEATURE_BG);
  display_set_text_datum(&(tft), TL_DATUM);
  display_set_text_font(&(tft), 1);

  const int statusY = GpsWardriver_kWardBodyY + 8;
  display_fill_round_rect(&(tft), pad, statusY, cardW, 36,
                          GpsWardriver_kWardUiRadius, UI_FG);
  display_draw_round_rect(&(tft), pad, statusY, cardW, 36,
                          GpsWardriver_kWardUiRadius, UI_LINE);
  display_set_text_color_2(&(tft), UI_TEXT, UI_FG);
  display_draw_string(&(tft), title ? title : "Wardriver", pad + 8, statusY + 6,
                      0);
  display_set_text_color_2(&(tft), UI_WARN, UI_FG);
  display_draw_string(&(tft), "Startup failed", pad + 8, statusY + 20, 0);

  const int actY = statusY + 44;
  const int actH = contentBottom - actY - 8;
  if (actH >= 24) {
    display_fill_round_rect(&(tft), pad, actY, cardW, actH,
                            GpsWardriver_kWardUiRadius, UI_FG);
    display_draw_round_rect(&(tft), pad, actY, cardW, actH,
                            GpsWardriver_kWardUiRadius, UI_LINE);
    display_set_text_color_2(&(tft), UI_DIM_TEXT, UI_FG);
    GpsWardriver_wardPrintWrappedPageBg(pad + 6, actY + 6, cardW - 12,
                                        msg ? msg : "", 10, UI_TEXT, UI_FG, 0,
                                        (actH - 12) / 10);
  }
}

/** @return true when Ready pressed (retry session). */
static bool GpsWardriver_wardWaitStartupFailureDismiss(const char* title,
                                                       const char* msg) {
  GpsWardriver_wardDrawStartupFailureScreen(title, msg);
  if (featureHasTouchNavBar()) {
    setTouchNavLabels("Exit", NULL, "Ready", NULL, NULL);
    redrawTouchButtonBar();
  }

  for (;;) {
    if (feature_exit_requested) {
      return false;
    }
    GpsWardriver_wardPumpNavChrome();
    if (isButtonPressed(BTN_LEFT)) {
      sys_sleep_ms(180);
      drainButtons();
      return false;
    }
    if (isButtonPressed(BTN_SELECT)) {
      sys_sleep_ms(180);
      drainButtons();
      return true;
    }
    if (featureHasTouchNavBar()) {
      if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
        return false;
      }
      if (isTouchNavButtonPressedEdge(BTN_SELECT)) {
        return true;
      }
    }
    sys_sleep_ms(8);
  }
}

/** Avoid full-screen clears every UI tick (reduces SPI TFT flicker). */
typedef struct GpsWardriver_WardTbSnap {
  bool valid;
  GpsWardriver_WardPage page;
  bool bgOn;
  bool logOn;
} GpsWardriver_WardTbSnap;
#define GpsWardriver_WardTbSnap_INITIALIZER {0}
;
static GpsWardriver_WardTbSnap GpsWardriver_s_wardTbSnap = {
    false, GpsWardriver_WardPage_Main, false, false};

typedef struct GpsWardriver_WardMainSnap {
  bool valid;
  uint32_t lines;
  uint32_t scans;
  char path[48];
  bool logOn;
  bool bgMode;
  uint8_t radio;
  uint32_t scanMs;
  int maxAps;
  int fixQ;
  unsigned sats;
  int hdopCenti;
  int32_t latE5;
  int32_t lonE5;
  int16_t altM;
} GpsWardriver_WardMainSnap;
#define GpsWardriver_WardMainSnap_INITIALIZER {0}
;
static GpsWardriver_WardMainSnap GpsWardriver_s_wardMainSnap =
    GpsWardriver_WardMainSnap_INITIALIZER;

typedef struct GpsWardriver_WardSetSnap {
  bool valid;
  uint32_t scanMs;
  int maxAps;
  GpsWardriver_WardRadioMode radio;
} GpsWardriver_WardSetSnap;
#define GpsWardriver_WardSetSnap_INITIALIZER {0}
;
static GpsWardriver_WardSetSnap GpsWardriver_s_wardSetSnap = {
    false, 0, 0, GpsWardriver_WardRadioMode_Both};

/** Bottom panel on main ward screen: rolling lines (newest at bottom). */
enum { GpsWardriver_kWardLogLines = 8 };
enum { GpsWardriver_kWardLogCols = 34 };
enum { GpsWardriver_kWardLogLineH = 11 };
static char GpsWardriver_s_wardLog[GpsWardriver_kWardLogLines]
                                  [GpsWardriver_kWardLogCols + 1];
static bool GpsWardriver_s_wardLogGreen[GpsWardriver_kWardLogLines];
static bool GpsWardriver_s_wardLogDirty = false;
static portMUX_TYPE GpsWardriver_s_wardLogMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t GpsWardriver_s_wardLogGen = 0;
/** GPS fix edge logging in ward session (-1 = re-arm after clear). */
static int8_t GpsWardriver_s_wardGpsFixLog = -1;

static void GpsWardriver_wardLogClear(void) {
  taskENTER_CRITICAL(&GpsWardriver_s_wardLogMux);
  for (int i = 0; i < GpsWardriver_kWardLogLines; ++i) {
    GpsWardriver_s_wardLog[i][0] = '\0';
    GpsWardriver_s_wardLogGreen[i] = false;
  }
  GpsWardriver_s_wardLogDirty = true;
  GpsWardriver_s_wardLogGen = GpsWardriver_s_wardLogGen + 1;
  GpsWardriver_s_wardGpsFixLog = -1;
  taskEXIT_CRITICAL(&GpsWardriver_s_wardLogMux);
}

static void GpsWardriver_wardLogPushLine(const char* line, bool green) {
  const char* s = line ? line : "";
  char one[GpsWardriver_kWardLogCols + 1];
  div_copy_string(one, sizeof(one), s);
  one[GpsWardriver_kWardLogCols] = '\0';
  taskENTER_CRITICAL(&GpsWardriver_s_wardLogMux);
  for (int i = 0; i < GpsWardriver_kWardLogLines - 1; ++i) {
    memcpy(GpsWardriver_s_wardLog[i], GpsWardriver_s_wardLog[i + 1],
           sizeof(GpsWardriver_s_wardLog[0]));
    GpsWardriver_s_wardLogGreen[i] = GpsWardriver_s_wardLogGreen[i + 1];
  }
  memcpy(GpsWardriver_s_wardLog[GpsWardriver_kWardLogLines - 1], one,
         sizeof(GpsWardriver_s_wardLog[0]));
  GpsWardriver_s_wardLogGreen[GpsWardriver_kWardLogLines - 1] = green;
  GpsWardriver_s_wardLogDirty = true;
  GpsWardriver_s_wardLogGen = GpsWardriver_s_wardLogGen + 1;
  taskEXIT_CRITICAL(&GpsWardriver_s_wardLogMux);
}

static void GpsWardriver_wardLogPush(const char* line) {
  GpsWardriver_wardLogPushLine(line, false);
}

static void GpsWardriver_wardLogPushf(const char* fmt, ...) {
  char b[GpsWardriver_kWardLogCols + 1];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  b[GpsWardriver_kWardLogCols] = '\0';
  GpsWardriver_wardLogPush(b);
}

static void GpsWardriver_wardLogPushWifiAp(const char* ssid, int ch, int rssi,
                                           const char* macFallback) {
  const char* name = (ssid && ssid[0])                 ? ssid
                     : (macFallback && macFallback[0]) ? macFallback
                                                       : "(hidden)";
  char b[GpsWardriver_kWardLogCols + 1];
  snprintf(b, sizeof(b), "W ch%2d %3ddBm %.8s", ch, rssi, name);
  GpsWardriver_wardLogPushLine(b, true);
}

static void GpsWardriver_wardLogPushBleDev(const char* name, int rssi,
                                           const char* mac) {
  const char* show = (name && name[0]) ? name : ((mac && mac[0]) ? mac : "?");
  char b[GpsWardriver_kWardLogCols + 1];
  snprintf(b, sizeof(b), "B %3ddBm %.16s", rssi, show);
  GpsWardriver_wardLogPushLine(b, true);
}

/** Append one line to the ward log and show the usual modal (for errors /
 * WiGLE). */
static void GpsWardriver_wardNotify(const char* title, const char* msg) {
  char b[GpsWardriver_kWardLogCols + 1];
  snprintf(b, sizeof(b), "%s: %s", title ? title : "", msg ? msg : "");
  b[GpsWardriver_kWardLogCols] = '\0';
  GpsWardriver_wardLogPush(b);
  showNotification(title, msg);
}

static void GpsWardriver_wardUiInvalidateAll(void) {
  GpsWardriver_s_wardTbSnap.valid = false;
  GpsWardriver_s_wardMainSnap.valid = false;
  GpsWardriver_s_wardSetSnap.valid = false;
}

static bool GpsWardriver_wardTbDirty(GpsWardriver_WardPage page, bool bgOn,
                                     bool logOn) {
  if (!GpsWardriver_s_wardTbSnap.valid) {
    return true;
  }
  return page != GpsWardriver_s_wardTbSnap.page ||
         bgOn != GpsWardriver_s_wardTbSnap.bgOn ||
         logOn != GpsWardriver_s_wardTbSnap.logOn;
}

static void GpsWardriver_wardTbSave(GpsWardriver_WardPage page, bool bgOn,
                                    bool logOn) {
  GpsWardriver_s_wardTbSnap =
      (GpsWardriver_WardTbSnap){true, page, bgOn, logOn};
}

static bool GpsWardriver_wardMainDirty(uint32_t dispL, uint32_t dispS,
                                       const char* dispP, bool logOn,
                                       bool bgMode) {
  if (!GpsWardriver_s_wardMainSnap.valid) {
    return true;
  }
  if (dispL != GpsWardriver_s_wardMainSnap.lines ||
      dispS != GpsWardriver_s_wardMainSnap.scans) {
    return true;
  }
  if (logOn != GpsWardriver_s_wardMainSnap.logOn ||
      bgMode != GpsWardriver_s_wardMainSnap.bgMode) {
    return true;
  }
  const char* p = dispP ? dispP : "";
  if (strncmp(p, GpsWardriver_s_wardMainSnap.path,
              sizeof(GpsWardriver_s_wardMainSnap.path)) != 0) {
    return true;
  }
  if ((uint8_t)GpsWardriver_s_cfgRadioMode !=
          GpsWardriver_s_wardMainSnap.radio ||
      GpsWardriver_s_cfgScanMs != GpsWardriver_s_wardMainSnap.scanMs ||
      GpsWardriver_s_cfgMaxAps != GpsWardriver_s_wardMainSnap.maxAps) {
    return true;
  }
  if (gps.state.fix_quality != GpsWardriver_s_wardMainSnap.fixQ ||
      gps.state.satellites_used != GpsWardriver_s_wardMainSnap.sats) {
    return true;
  }
  int hc = -1;
  if (gps.state.hdop >= 0.f) {
    hc = (int)lroundf(gps.state.hdop * 100.f);
  }
  if (hc != GpsWardriver_s_wardMainSnap.hdopCenti) {
    return true;
  }
  int32_t le = INT32_MIN;
  int32_t ln = INT32_MIN;
  if (!isnan(gps.state.latitude)) {
    le = (int32_t)lroundf((float)gps.state.latitude * 1e5f);
  }
  if (!isnan(gps.state.longitude)) {
    ln = (int32_t)lroundf((float)gps.state.longitude * 1e5f);
  }
  if (le != GpsWardriver_s_wardMainSnap.latE5 ||
      ln != GpsWardriver_s_wardMainSnap.lonE5) {
    return true;
  }
  int16_t am = INT16_MIN;
  if (!isnan(gps.state.altitude_m)) {
    am = (int16_t)lroundf((float)gps.state.altitude_m);
  }
  if (GpsWardriver_s_wardLogDirty) {
    return true;
  }
  return am != GpsWardriver_s_wardMainSnap.altM;
}

static void GpsWardriver_wardMainSave(uint32_t dispL, uint32_t dispS,
                                      const char* dispP, bool logOn,
                                      bool bgMode) {
  GpsWardriver_s_wardMainSnap.valid = true;
  GpsWardriver_s_wardMainSnap.lines = dispL;
  GpsWardriver_s_wardMainSnap.scans = dispS;
  strncpy(GpsWardriver_s_wardMainSnap.path, dispP && dispP[0] ? dispP : "",
          sizeof(GpsWardriver_s_wardMainSnap.path));
  GpsWardriver_s_wardMainSnap
      .path[sizeof(GpsWardriver_s_wardMainSnap.path) - 1] = '\0';
  GpsWardriver_s_wardMainSnap.logOn = logOn;
  GpsWardriver_s_wardMainSnap.bgMode = bgMode;
  GpsWardriver_s_wardMainSnap.radio = (uint8_t)GpsWardriver_s_cfgRadioMode;
  GpsWardriver_s_wardMainSnap.scanMs = GpsWardriver_s_cfgScanMs;
  GpsWardriver_s_wardMainSnap.maxAps = GpsWardriver_s_cfgMaxAps;
  GpsWardriver_s_wardMainSnap.fixQ = gps.state.fix_quality;
  GpsWardriver_s_wardMainSnap.sats = gps.state.satellites_used;
  if (gps.state.hdop >= 0.f) {
    GpsWardriver_s_wardMainSnap.hdopCenti =
        (int)lroundf(gps.state.hdop * 100.f);
  } else {
    GpsWardriver_s_wardMainSnap.hdopCenti = -1;
  }
  if (!isnan(gps.state.latitude)) {
    GpsWardriver_s_wardMainSnap.latE5 =
        (int32_t)lroundf((float)gps.state.latitude * 1e5f);
  } else {
    GpsWardriver_s_wardMainSnap.latE5 = INT32_MIN;
  }
  if (!isnan(gps.state.longitude)) {
    GpsWardriver_s_wardMainSnap.lonE5 =
        (int32_t)lroundf((float)gps.state.longitude * 1e5f);
  } else {
    GpsWardriver_s_wardMainSnap.lonE5 = INT32_MIN;
  }
  if (!isnan(gps.state.altitude_m)) {
    GpsWardriver_s_wardMainSnap.altM =
        (int16_t)lroundf((float)gps.state.altitude_m);
  } else {
    GpsWardriver_s_wardMainSnap.altM = INT16_MIN;
  }
}

static bool GpsWardriver_wardSetDirty(void) {
  if (!GpsWardriver_s_wardSetSnap.valid) {
    return true;
  }
  return GpsWardriver_s_editScanMs != GpsWardriver_s_wardSetSnap.scanMs ||
         GpsWardriver_s_editMaxAps != GpsWardriver_s_wardSetSnap.maxAps ||
         GpsWardriver_s_editRadioMode != GpsWardriver_s_wardSetSnap.radio;
}

static void GpsWardriver_wardSetSave(void) {
  GpsWardriver_s_wardSetSnap.valid = true;
  GpsWardriver_s_wardSetSnap.scanMs = GpsWardriver_s_editScanMs;
  GpsWardriver_s_wardSetSnap.maxAps = GpsWardriver_s_editMaxAps;
  GpsWardriver_s_wardSetSnap.radio = GpsWardriver_s_editRadioMode;
}

static const char* GpsWardriver_wardAuthLabel(wifi_auth_mode_t t) {
  switch (t) {
    case WIFI_AUTH_OPEN:
      return "OPEN";
    case WIFI_AUTH_WEP:
      return "WEP";
    case WIFI_AUTH_WPA_PSK:
      return "WPA_PSK";
    case WIFI_AUTH_WPA2_PSK:
      return "WPA2_PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:
      return "WPA_WPA2_PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE:
      return "WPA2_ENT";
    case WIFI_AUTH_WPA3_PSK:
      return "WPA3_PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK:
      return "WPA2_WPA3_PSK";
    case WIFI_AUTH_WAPI_PSK:
      return "WAPI_PSK";
    default:
      return "OTHER";
  }
}

static const char* GpsWardriver_wardRadioModeLabel(
    GpsWardriver_WardRadioMode m) {
  switch (m) {
    case GpsWardriver_WardRadioMode_WiFi:
      return "WiFi only";
    case GpsWardriver_WardRadioMode_Ble:
      return "BLE only";
    case GpsWardriver_WardRadioMode_Both:
      return "WiFi+BLE";
    default:
      return "?";
  }
}

/** Short label for tight UI rows (avoids clipping on 240px). */
static const char* GpsWardriver_wardRadioModeShort(
    GpsWardriver_WardRadioMode m) {
  switch (m) {
    case GpsWardriver_WardRadioMode_WiFi:
      return "WiFi";
    case GpsWardriver_WardRadioMode_Ble:
      return "BLE";
    case GpsWardriver_WardRadioMode_Both:
      return "W+B";
    default:
      return "?";
  }
}

static void GpsWardriver_wardAppendCsvString(storage_file_t* f, const char* s) {
  storage_file_print_cstr(&((*f)), (text_character('"')).data);
  if (s) {
    for (const char* p = s; *p; ++p) {
      if (*p == '"') {
        storage_file_print_cstr(&((*f)), "\"\"");
      } else if ((uint8_t)*p < 32) {
        storage_file_print_cstr(&((*f)), (text_character(' ')).data);
      } else {
        storage_file_print_cstr(&((*f)), (TEXT(*p)).data);
      }
    }
  }
  storage_file_print_cstr(&((*f)), (text_character('"')).data);
}

static void GpsWardriver_wardDrawPill(int x, int y, int w, int h,
                                      const char* text, uint16_t fill,
                                      uint16_t txt) {
  const int r = h > 8 ? 4 : 2;
  display_fill_round_rect(&(tft), x, y, w, h, r, fill);
  display_set_text_font(&(tft), 1);
  display_set_text_datum(&(tft), MC_DATUM);
  display_set_text_color_2(&(tft), txt, fill);
  display_draw_string(&(tft), text ? text : "", x + w / 2, y + h / 2, 0);
  display_set_text_datum(&(tft), TL_DATUM);
}

typedef struct GpsWardriver_WardMainLayout {
  int heroY;
  int statY;
  int csvY;
  int gpsY;
  int radioY;
  int hintY;
  int heroH;
  int statH;
  int csvH;
  int gpsH;
  int radioH;
  int hintH;
} GpsWardriver_WardMainLayout;
#define GpsWardriver_WardMainLayout_INITIALIZER {0}
;

/** Pack status cards from the top; log panel fills remaining space above the
 * nav bar. */
static GpsWardriver_WardMainLayout GpsWardriver_wardComputeMainLayout(
    int contentBottom) {
  GpsWardriver_WardMainLayout L = GpsWardriver_WardMainLayout_INITIALIZER;
  const int bodyTop = GpsWardriver_kWardBodyY + 4;
  enum { bottomPad = 4 };
  const int gap = featureHasTouchNavBar() ? 3 : 5;
  const bool compact = featureHasTouchNavBar();

  L.heroH = compact ? 26 : 28;
  L.statH = compact ? 30 : 34;
  L.csvH = L.statH;
  L.gpsH = compact ? 46 : 52;
  L.radioH = compact ? 30 : 32;

  L.heroY = bodyTop;
  L.statY = L.heroY + L.heroH + gap;
  L.csvY = L.statY + L.statH + gap;
  L.gpsY = L.csvY + L.csvH + gap;
  L.radioY = L.gpsY + L.gpsH + gap;
  L.hintY = L.radioY + L.radioH + gap;
  L.hintH = contentBottom - bottomPad - L.hintY;

  const int minLogH = compact ? 88 : 76;
  if (L.hintH < minLogH) {
    L.hintH = minLogH;
    L.hintY = contentBottom - bottomPad - L.hintH;
    L.radioY = L.hintY - gap - L.radioH;
    L.gpsY = L.radioY - gap - L.gpsH;
    L.csvY = L.gpsY - gap - L.csvH;
    L.statY = L.csvY - gap - L.statH;
    L.heroY = L.statY - gap - L.heroH;
    if (L.heroY < bodyTop) {
      L.heroY = bodyTop;
      L.statY = L.heroY + L.heroH + gap;
      L.csvY = L.statY + L.statH + gap;
      L.gpsY = L.csvY + L.csvH + gap;
      L.radioY = L.gpsY + L.gpsH + gap;
      L.hintY = L.radioY + L.radioH + gap;
      L.hintH = contentBottom - bottomPad - L.hintY;
    }
  }
  return L;
}

/** Main: back left; gear, play, RSS flush-right. Settings/WiGLE: back left,
 * save right (no ?/+ strip). */
static void GpsWardriver_wardToolbarIconXs(GpsWardriver_WardPage page,
                                           int16_t xs[4]) {
  const int W = display_width(&(tft));
  const int sz = (int)GpsWardriver_kWardIconSz;
  enum { margin = 6 };
  enum { gap = 10 };

  if (page == GpsWardriver_WardPage_Settings ||
      page == GpsWardriver_WardPage_WigleCfg) {
    xs[0] = (int16_t)margin;
    xs[3] = (int16_t)(W - margin - sz);
    xs[1] = -1;
    xs[2] = -1;
    return;
  }

  xs[3] = (int16_t)margin;
  xs[2] = (int16_t)(W - margin - sz);
  xs[1] = (int16_t)(xs[2] - gap - sz);
  xs[0] = (int16_t)(xs[1] - gap - sz);
  const int backRight = (int)xs[3] + sz;
  if ((int)xs[0] < backRight + gap) {
    const int clusterW = 3 * sz + 2 * gap;
    int c0 = W - margin - clusterW;
    const int minLeft = backRight + gap;
    if (c0 < minLeft) {
      c0 = minLeft;
    }
    xs[0] = (int16_t)c0;
    xs[1] = (int16_t)(c0 + gap + sz);
    xs[2] = (int16_t)(c0 + 2 * (gap + sz));
  }
}

static void GpsWardriver_wardDrawIconToolbar(GpsWardriver_WardPage page,
                                             bool bgActive, bool logOn) {
  const int scrW = display_width(&(tft));
  int16_t ix[4];
  GpsWardriver_wardToolbarIconXs(page, ix);
  display_draw_line(&(tft), 0, 19, scrW, 19, TFT_WHITE);
  display_fill_rect(&(tft), 0, GpsWardriver_kWardIconRowY, scrW,
                    GpsWardriver_kWardIconRowH, DARK_GRAY);
  const int iy = GpsWardriver_kWardIconRowY;
  if (page == GpsWardriver_WardPage_Settings ||
      page == GpsWardriver_WardPage_WigleCfg) {
    display_draw_bitmap(&(tft), ix[0], iy, bitmap_icon_go_back,
                        GpsWardriver_kWardIconSz, GpsWardriver_kWardIconSz,
                        TFT_WHITE);
    display_draw_bitmap(&(tft), ix[3], iy, bitmap_icon_checks,
                        GpsWardriver_kWardIconSz, GpsWardriver_kWardIconSz,
                        TFT_WHITE);
    display_draw_line(
        &(tft), 0, GpsWardriver_kWardIconRowY + GpsWardriver_kWardIconRowH,
        scrW, GpsWardriver_kWardIconRowY + GpsWardriver_kWardIconRowH, ORANGE);
    return;
  }
  const unsigned char* i0 = bitmap_icon_document_gear;
  const unsigned char* i1 =
      logOn ? bitmap_icon_sign_forbidden : bitmap_icon_start;
  const unsigned char* i2 = bgActive ? bitmap_icon_power : bitmap_icon_rss;
  const unsigned char* i3 = bitmap_icon_go_back;
  const unsigned char* const icons[4] = {i0, i1, i2, i3};
  for (int i = 0; i < 4; i++) {
    uint16_t col = TFT_WHITE;
    if (i == 1 && logOn) {
      col = RED;
    } else if (i == 2 && bgActive) {
      col = ORANGE;
    }
    display_draw_bitmap(&(tft), ix[i], iy, icons[i], GpsWardriver_kWardIconSz,
                        GpsWardriver_kWardIconSz, col);
  }
  display_draw_line(
      &(tft), 0, GpsWardriver_kWardIconRowY + GpsWardriver_kWardIconRowH, scrW,
      GpsWardriver_kWardIconRowY + GpsWardriver_kWardIconRowH, ORANGE);
}

static void GpsWardriver_wardDrawBody(uint32_t linesWr, uint32_t scans,
                                      const char* path, bool logOn,
                                      bool bgMode) {
  const int scrW = display_width(&(tft));
  const int contentBottom = GpsWardriver_wardContentBottom();
  const int pad = GpsWardriver_kWSetPad;
  const int cardW = scrW - 2 * pad;
  const GpsWardriver_WardMainLayout layout =
      GpsWardriver_wardComputeMainLayout(contentBottom);
  char buf[128];

  const bool fullPaint = !GpsWardriver_s_wardMainSnap.valid;
  const char* safePath = path ? path : "";
  const bool heroDirty = fullPaint ||
                         logOn != GpsWardriver_s_wardMainSnap.logOn ||
                         bgMode != GpsWardriver_s_wardMainSnap.bgMode;
  const bool statsDirty = fullPaint ||
                          linesWr != GpsWardriver_s_wardMainSnap.lines ||
                          scans != GpsWardriver_s_wardMainSnap.scans;
  const bool csvDirty =
      fullPaint || strncmp(safePath, GpsWardriver_s_wardMainSnap.path,
                           sizeof(GpsWardriver_s_wardMainSnap.path)) != 0;
  const GpsWardriver_WardRadioMode rm =
      (GpsWardriver_WardRadioMode)GpsWardriver_s_cfgRadioMode;
  const bool radioDirty =
      fullPaint || (uint8_t)rm != GpsWardriver_s_wardMainSnap.radio ||
      GpsWardriver_s_cfgScanMs != GpsWardriver_s_wardMainSnap.scanMs ||
      GpsWardriver_s_cfgMaxAps != GpsWardriver_s_wardMainSnap.maxAps;
  const bool fix = (gps.state.fix_quality > 0) && !isnan(gps.state.latitude) &&
                   !isnan(gps.state.longitude);
  int hcNow = -1;
  if (gps.state.hdop >= 0.f) {
    hcNow = (int)lroundf(gps.state.hdop * 100.f);
  }
  int32_t latNow = INT32_MIN;
  int32_t lonNow = INT32_MIN;
  if (!isnan(gps.state.latitude)) {
    latNow = (int32_t)lroundf((float)gps.state.latitude * 1e5f);
  }
  if (!isnan(gps.state.longitude)) {
    lonNow = (int32_t)lroundf((float)gps.state.longitude * 1e5f);
  }
  int16_t altNow = INT16_MIN;
  if (!isnan(gps.state.altitude_m)) {
    altNow = (int16_t)lroundf((float)gps.state.altitude_m);
  }
  const bool gpsDirty =
      fullPaint || gps.state.fix_quality != GpsWardriver_s_wardMainSnap.fixQ ||
      gps.state.satellites_used != GpsWardriver_s_wardMainSnap.sats ||
      hcNow != GpsWardriver_s_wardMainSnap.hdopCenti ||
      latNow != GpsWardriver_s_wardMainSnap.latE5 ||
      lonNow != GpsWardriver_s_wardMainSnap.lonE5 ||
      altNow != GpsWardriver_s_wardMainSnap.altM;
  if (fullPaint) {
    display_fill_rect(&(tft), 0, GpsWardriver_kWardBodyY, scrW,
                      contentBottom - GpsWardriver_kWardBodyY, FEATURE_BG);
  }
  display_set_text_datum(&(tft), TL_DATUM);

  if (heroDirty) {
    const int innerY = layout.heroY + GpsWardriver_kWardInnerPad;
    const int textX = GpsWardriver_wardCardTextX(pad);
    display_fill_round_rect(&(tft), pad, layout.heroY, cardW, layout.heroH,
                            GpsWardriver_kWardUiRadius, UI_FG);
    display_draw_round_rect(&(tft), pad, layout.heroY, cardW, layout.heroH,
                            GpsWardriver_kWardUiRadius, UI_LINE);
    display_fill_round_rect(&(tft), pad + 3, innerY, 3,
                            layout.heroH - GpsWardriver_kWardInnerPad * 2, 2,
                            ORANGE);
    display_draw_bitmap(&(tft), pad + GpsWardriver_kWardInnerPad, innerY,
                        bitmap_icon_satellite, 16, 16, UI_ICON);
    display_set_text_font(&(tft), 1);
    display_set_text_color_2(&(tft), UI_TEXT, UI_FG);
    display_draw_string(&(tft), "Wardrive", textX, innerY + 1, 0);
    enum { kWardStatusPillW = 40 };
    const int statusPillX = scrW - pad - kWardStatusPillW - 4;
    const int pillY = layout.heroY + (layout.heroH - 18) / 2;
    if (logOn) {
      GpsWardriver_wardDrawPill(statusPillX, pillY, kWardStatusPillW, 18, "REC",
                                RED, WHITE);
    } else {
      GpsWardriver_wardDrawPill(statusPillX, pillY, kWardStatusPillW, 18, "OFF",
                                DARK_GRAY, UI_DIM_TEXT);
    }
    if (bgMode) {
      display_set_text_font(&(tft), 1);
      display_set_text_color_2(&(tft), UI_DIM_TEXT, UI_FG);
      const int bgX = statusPillX - display_text_width(&(tft), "BG", 0) - 4;
      if (bgX >= textX + display_text_width(&(tft), "Wardrive", 0) + 4) {
        display_draw_string(&(tft), "BG", bgX, innerY + 2, 0);
      }
    }
  }

  if (statsDirty) {
    enum { colGap = 6 };
    const int colW = (cardW - colGap) / 2;
    const int labelY = layout.statY + GpsWardriver_kWardInnerPad;
    const int valueY =
        layout.statY + layout.statH - GpsWardriver_kWardInnerPad - 8;
    display_fill_round_rect(&(tft), pad, layout.statY, colW, layout.statH,
                            GpsWardriver_kWardUiRadius, UI_FG);
    display_draw_round_rect(&(tft), pad, layout.statY, colW, layout.statH,
                            GpsWardriver_kWardUiRadius, UI_LINE);
    display_set_text_font(&(tft), 1);
    display_set_text_color_2(&(tft), UI_ICON, UI_FG);
    display_draw_string(&(tft), "ROWS", pad + GpsWardriver_kWardInnerPad,
                        labelY, 0);
    display_set_text_color_2(&(tft), UI_TEXT, UI_FG);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)linesWr);
    display_draw_string(&(tft), buf, pad + GpsWardriver_kWardInnerPad, valueY,
                        0);

    const int x2 = pad + colW + colGap;
    display_fill_round_rect(&(tft), x2, layout.statY, colW, layout.statH,
                            GpsWardriver_kWardUiRadius, UI_FG);
    display_draw_round_rect(&(tft), x2, layout.statY, colW, layout.statH,
                            GpsWardriver_kWardUiRadius, UI_LINE);
    display_set_text_color_2(&(tft), UI_ICON, UI_FG);
    display_draw_string(&(tft), "SCANS", x2 + GpsWardriver_kWardInnerPad,
                        labelY, 0);
    display_set_text_color_2(&(tft), UI_TEXT, UI_FG);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)scans);
    display_draw_string(&(tft), buf, x2 + GpsWardriver_kWardInnerPad, valueY,
                        0);
  }

  if (csvDirty) {
    const int iconX = pad + GpsWardriver_kWardInnerPad;
    const int textX = GpsWardriver_wardCardTextX(pad);
    const int maxTextW = GpsWardriver_wardCardTextMaxW(pad, cardW, 0);
    const int iconY = GpsWardriver_wardCardIconY(layout.csvY, layout.csvH);
    int rowY[3];
    GpsWardriver_wardCardTextRowYs(layout.csvY, layout.csvH, 2, rowY);
    display_fill_round_rect(&(tft), pad, layout.csvY, cardW, layout.csvH,
                            GpsWardriver_kWardUiRadius, UI_FG);
    display_draw_round_rect(&(tft), pad, layout.csvY, cardW, layout.csvH,
                            GpsWardriver_kWardUiRadius, UI_LINE);
    display_draw_bitmap(&(tft), iconX, iconY, bitmap_icon_document_tag,
                        GpsWardriver_kWardCardIconSz,
                        GpsWardriver_kWardCardIconSz, UI_ICON);
    display_set_text_font(&(tft), 1);
    display_set_text_color_2(&(tft), UI_ICON, UI_FG);
    display_draw_string(&(tft), "CSV", textX, rowY[0], 0);
    display_set_text_color_2(&(tft), UI_DIM_TEXT, UI_FG);
    if (!safePath[0]) {
      display_draw_string(&(tft), "no file", textX, rowY[1], 0);
    } else {
      const size_t pathLen = strlen(safePath);
      if (pathLen > 18u) {
        snprintf(buf, sizeof(buf), "%.8s..%.8s", safePath,
                 safePath + pathLen - 8);
      } else {
        div_copy_string(buf, sizeof(buf), safePath);
      }
      display_draw_string(&(tft),
                          (GpsWardriver_wardFitTextWidth(buf, maxTextW)).data,
                          textX, rowY[1], 0);
    }
  }

  if (gpsDirty) {
    const int iconX = pad + GpsWardriver_kWardInnerPad;
    const int textX = GpsWardriver_wardCardTextX(pad);
    const int iconY = GpsWardriver_wardCardIconY(layout.gpsY, layout.gpsH);
    int rowY[3];
    GpsWardriver_wardCardTextRowYs(layout.gpsY, layout.gpsH, 3, rowY);
    enum { kFixPillW = 34 };
    enum { kWaitPillW = 40 };
    const int pillW = fix ? kFixPillW : kWaitPillW;
    enum { pillH = 16 };
    const int pillX = scrW - pad - pillW - 2;
    const int pillY = rowY[0] + (GpsWardriver_kWardTextLineH - pillH) / 2;
    const int maxTextW = GpsWardriver_wardCardTextMaxW(pad, cardW, 0);
    display_fill_round_rect(&(tft), pad, layout.gpsY, cardW, layout.gpsH,
                            GpsWardriver_kWardUiRadius, UI_FG);
    display_draw_round_rect(&(tft), pad, layout.gpsY, cardW, layout.gpsH,
                            GpsWardriver_kWardUiRadius, UI_LINE);
    display_draw_bitmap(&(tft), iconX, iconY, bitmap_icon_satellite_dish,
                        GpsWardriver_kWardCardIconSz,
                        GpsWardriver_kWardCardIconSz, UI_ICON);
    display_set_text_font(&(tft), 1);
    display_set_text_color_2(&(tft), UI_TEXT, UI_FG);
    display_draw_string(&(tft), "GPS", textX, rowY[0], 0);
    if (fix) {
      GpsWardriver_wardDrawPill(pillX, pillY, kFixPillW, pillH, "FIX", GREEN,
                                BLACK);
    } else {
      GpsWardriver_wardDrawPill(pillX, pillY, kWaitPillW, pillH, "WAIT", ORANGE,
                                FEATURE_BG);
    }
    display_set_text_color_2(&(tft), UI_DIM_TEXT, UI_FG);
    if (!isnan(gps.state.altitude_m) && fix) {
      snprintf(buf, sizeof(buf), "Sats %u  HDOP %.1f  Alt %.0fm",
               (unsigned)gps.state.satellites_used, (double)gps.state.hdop,
               (double)gps.state.altitude_m);
    } else {
      snprintf(buf, sizeof(buf), "Sats %u  HDOP %.1f",
               (unsigned)gps.state.satellites_used,
               gps.state.hdop >= 0.f ? (double)gps.state.hdop : -1.0);
    }
    display_draw_string(&(tft),
                        (GpsWardriver_wardFitTextWidth(buf, maxTextW)).data,
                        textX, rowY[1], 0);
    if (fix) {
      snprintf(buf, sizeof(buf), "%.4f  %.4f", gps.state.latitude,
               gps.state.longitude);
      display_draw_string(&(tft),
                          (GpsWardriver_wardFitTextWidth(buf, maxTextW)).data,
                          textX, rowY[2], 0);
    } else {
      display_draw_string(&(tft), "No fix yet", textX, rowY[2], 0);
    }
  }

  if (radioDirty) {
    const int radioTextX = pad + 58;
    const int maxTextW =
        cardW - (radioTextX - pad) - GpsWardriver_kWardInnerPad;
    const int row1Y = layout.radioY + GpsWardriver_kWardInnerPad;
    const int row2Y =
        layout.radioY + layout.radioH - GpsWardriver_kWardInnerPad - 8;
    display_fill_round_rect(&(tft), pad, layout.radioY, cardW, layout.radioH,
                            GpsWardriver_kWardUiRadius, UI_FG);
    display_draw_round_rect(&(tft), pad, layout.radioY, cardW, layout.radioH,
                            GpsWardriver_kWardUiRadius, UI_LINE);
    const int radioIconY = layout.radioY + (layout.radioH - 16) / 2;
    int ix = pad + GpsWardriver_kWardInnerPad;
    if (rm == GpsWardriver_WardRadioMode_WiFi ||
        rm == GpsWardriver_WardRadioMode_Both) {
      display_draw_bitmap(
          &(tft), ix, radioIconY, bitmap_icon_wifi, 16, 16,
          rm == GpsWardriver_WardRadioMode_WiFi ? ORANGE : UI_DIM_TEXT);
      ix += 20;
    }
    if (rm == GpsWardriver_WardRadioMode_Ble ||
        rm == GpsWardriver_WardRadioMode_Both) {
      display_draw_bitmap(
          &(tft), ix, radioIconY, bitmap_icon_ble, 16, 16,
          rm == GpsWardriver_WardRadioMode_Ble ? ORANGE : UI_DIM_TEXT);
    }
    display_set_text_font(&(tft), 1);
    display_set_text_color_2(&(tft), UI_TEXT, UI_FG);
    display_draw_string(&(tft), GpsWardriver_wardRadioModeShort(rm), radioTextX,
                        row1Y, 0);
    snprintf(buf, sizeof(buf), "%ums cap %d",
             (unsigned)GpsWardriver_s_cfgScanMs, (int)GpsWardriver_s_cfgMaxAps);
    display_set_text_color_2(&(tft), UI_DIM_TEXT, UI_FG);
    display_draw_string(&(tft),
                        (GpsWardriver_wardFitTextWidth(buf, maxTextW)).data,
                        radioTextX, row2Y, 0);
  }

  const bool logPanelDirty = fullPaint || GpsWardriver_s_wardLogDirty;
  if (logPanelDirty && layout.hintH >= 20) {
    char snap[GpsWardriver_kWardLogLines][GpsWardriver_kWardLogCols + 1];
    bool snapGreen[GpsWardriver_kWardLogLines];
    uint32_t genSnap = 0;
    taskENTER_CRITICAL(&GpsWardriver_s_wardLogMux);
    memcpy(snap, GpsWardriver_s_wardLog, sizeof(snap));
    memcpy(snapGreen, GpsWardriver_s_wardLogGreen, sizeof(snapGreen));
    genSnap = GpsWardriver_s_wardLogGen;
    taskEXIT_CRITICAL(&GpsWardriver_s_wardLogMux);

    display_fill_round_rect(&(tft), pad, layout.hintY, cardW, layout.hintH,
                            GpsWardriver_kWardUiRadius, UI_FG);
    display_draw_round_rect(&(tft), pad, layout.hintY, cardW, layout.hintH,
                            GpsWardriver_kWardUiRadius, UI_LINE);
    display_set_text_font(&(tft), 1);
    display_set_text_datum(&(tft), TL_DATUM);
    const int lx = pad + GpsWardriver_kWardInnerPad;
    const int innerTop = layout.hintY + GpsWardriver_kWardInnerPad;
    const int innerBot =
        layout.hintY + layout.hintH - GpsWardriver_kWardInnerPad;
    const int innerH = innerBot - innerTop;
    int linesToDraw = innerH / GpsWardriver_kWardLogLineH;
    if (linesToDraw > GpsWardriver_kWardLogLines) {
      linesToDraw = GpsWardriver_kWardLogLines;
    }
    if (linesToDraw < 1) {
      linesToDraw = 1;
    }
    const int skip = GpsWardriver_kWardLogLines - linesToDraw;
    const int blockH = linesToDraw * GpsWardriver_kWardLogLineH;
    int ty = innerBot - blockH;
    if (ty < innerTop) {
      ty = innerTop;
    }
    for (int i = 0; i < linesToDraw; ++i, ty += GpsWardriver_kWardLogLineH) {
      const int src = skip + i;
      if (snap[src][0] == '\0') {
        continue;
      }
      display_set_text_color_2(&(tft), snapGreen[src] ? UI_OK : UI_DIM_TEXT,
                               UI_FG);
      display_draw_string(&(tft), snap[src], lx, ty, 0);
    }
    taskENTER_CRITICAL(&GpsWardriver_s_wardLogMux);
    if (GpsWardriver_s_wardLogGen == genSnap) {
      GpsWardriver_s_wardLogDirty = false;
    }
    taskEXIT_CRITICAL(&GpsWardriver_s_wardLogMux);
  }
}

static void GpsWardriver_wardLayoutSettingsButtons(void) {
  const GpsWardriver_WardSetLayout L = GpsWardriver_wardComputeSetLayout();
  const int16_t W = display_width(&(tft));
  const int16_t bw3 =
      (int16_t)((W - 2 * GpsWardriver_kWSetPad - 2 * GpsWardriver_kWSetGap) /
                3);
  const int16_t bw2 =
      (int16_t)((W - 2 * GpsWardriver_kWSetPad - GpsWardriver_kWSetGap) / 2);
  const int16_t halfW =
      (int16_t)((W - 2 * GpsWardriver_kWSetPad - GpsWardriver_kWSetGap) / 2);

  GpsWardriver_s_wardSetBtns[0] = (FeatureUI_Button){
      (int16_t)GpsWardriver_kWSetPad,
      (int16_t)L.modeBtnY,
      bw3,
      (int16_t)GpsWardriver_kWSetBtnH,
      "WiFi",
      (GpsWardriver_s_editRadioMode == GpsWardriver_WardRadioMode_WiFi)
          ? FeatureUI_ButtonStyle_Primary
          : FeatureUI_ButtonStyle_Secondary,
      false};
  GpsWardriver_s_wardSetBtns[1] = (FeatureUI_Button){
      (int16_t)(GpsWardriver_kWSetPad + bw3 + GpsWardriver_kWSetGap),
      (int16_t)L.modeBtnY,
      bw3,
      (int16_t)GpsWardriver_kWSetBtnH,
      "BLE",
      (GpsWardriver_s_editRadioMode == GpsWardriver_WardRadioMode_Ble)
          ? FeatureUI_ButtonStyle_Primary
          : FeatureUI_ButtonStyle_Secondary,
      false};
  GpsWardriver_s_wardSetBtns[2] = (FeatureUI_Button){
      (int16_t)(GpsWardriver_kWSetPad + 2 * (bw3 + GpsWardriver_kWSetGap)),
      (int16_t)L.modeBtnY,
      bw3,
      (int16_t)GpsWardriver_kWSetBtnH,
      "Both",
      (GpsWardriver_s_editRadioMode == GpsWardriver_WardRadioMode_Both)
          ? FeatureUI_ButtonStyle_Primary
          : FeatureUI_ButtonStyle_Secondary,
      false};

  GpsWardriver_s_wardSetBtns[3] =
      (FeatureUI_Button){(int16_t)GpsWardriver_kWSetPad,
                         (int16_t)L.intervalBtnY,
                         bw2,
                         (int16_t)GpsWardriver_kWSetBtnH,
                         "-500ms",
                         FeatureUI_ButtonStyle_Secondary,
                         false};
  GpsWardriver_s_wardSetBtns[4] = (FeatureUI_Button){
      (int16_t)(GpsWardriver_kWSetPad + bw2 + GpsWardriver_kWSetGap),
      (int16_t)L.intervalBtnY,
      bw2,
      (int16_t)GpsWardriver_kWSetBtnH,
      "+500ms",
      FeatureUI_ButtonStyle_Secondary,
      false};

  GpsWardriver_s_wardSetBtns[5] =
      (FeatureUI_Button){(int16_t)GpsWardriver_kWSetPad,
                         (int16_t)L.capBtnY,
                         bw2,
                         (int16_t)GpsWardriver_kWSetBtnH,
                         "Rows -",
                         FeatureUI_ButtonStyle_Secondary,
                         false};
  GpsWardriver_s_wardSetBtns[6] = (FeatureUI_Button){
      (int16_t)(GpsWardriver_kWSetPad + bw2 + GpsWardriver_kWSetGap),
      (int16_t)L.capBtnY,
      bw2,
      (int16_t)GpsWardriver_kWSetBtnH,
      "Rows +",
      FeatureUI_ButtonStyle_Secondary,
      false};

  if (!featureHasTouchNavBar()) {
    const int16_t wigleY = (int16_t)(L.capBtnY + GpsWardriver_kWSetBtnH +
                                     GpsWardriver_kWSetSectionGap);
    GpsWardriver_s_wardSetBtns[7] =
        (FeatureUI_Button){(int16_t)GpsWardriver_kWSetPad,
                           wigleY,
                           halfW,
                           (int16_t)GpsWardriver_kWSetBtnH,
                           "Upload",
                           FeatureUI_ButtonStyle_Primary,
                           false};
    GpsWardriver_s_wardSetBtns[8] = (FeatureUI_Button){
        (int16_t)(GpsWardriver_kWSetPad + halfW + GpsWardriver_kWSetGap),
        wigleY,
        halfW,
        (int16_t)GpsWardriver_kWSetBtnH,
        "WiGLE cfg",
        FeatureUI_ButtonStyle_Secondary,
        false};
  }
}

static void GpsWardriver_wardDrawSettingsPage(void) {
  const int scrW = display_width(&(tft));
  const int contentBottom = GpsWardriver_wardContentBottom();
  const int pad = GpsWardriver_kWSetPad;
  const int cardW = scrW - 2 * pad;
  const GpsWardriver_WardSetLayout layout = GpsWardriver_wardComputeSetLayout();
  char buf[96];
  const bool fullPaint = !GpsWardriver_s_wardSetSnap.valid;
  const bool radioDirty = fullPaint || GpsWardriver_s_editRadioMode !=
                                           GpsWardriver_s_wardSetSnap.radio;
  const bool intervalDirty = fullPaint || GpsWardriver_s_editScanMs !=
                                              GpsWardriver_s_wardSetSnap.scanMs;
  const bool maxDirty = fullPaint || GpsWardriver_s_editMaxAps !=
                                         GpsWardriver_s_wardSetSnap.maxAps;

  if (fullPaint) {
    display_fill_rect(&(tft), 0, GpsWardriver_kWardBodyY, scrW,
                      contentBottom - GpsWardriver_kWardBodyY, FEATURE_BG);
  }
  display_set_text_datum(&(tft), TL_DATUM);

  GpsWardriver_wardLayoutSettingsButtons();

  if (fullPaint) {
    GpsWardriver_wardDrawSetHeader(pad, cardW, &(layout));
  }

  if (radioDirty) {
    for (int i = 0; i < 3; ++i) {
      FeatureUI_drawButton(&(GpsWardriver_s_wardSetBtns[i]), false);
    }
  }

  if (intervalDirty) {
    snprintf(buf, sizeof(buf), "%lu ms",
             (unsigned long)GpsWardriver_s_editScanMs);
    GpsWardriver_wardDrawSetInlineCard(pad, cardW, layout.intervalCardY,
                                       "INTERVAL", buf, UI_TEXT);
    FeatureUI_drawButton(&(GpsWardriver_s_wardSetBtns[3]), false);
    FeatureUI_drawButton(&(GpsWardriver_s_wardSetBtns[4]), false);
  }

  if (maxDirty) {
    snprintf(buf, sizeof(buf), "%d rows", GpsWardriver_s_editMaxAps);
    GpsWardriver_wardDrawSetInlineCard(pad, cardW, layout.capCardY, "ROW CAP",
                                       buf, UI_TEXT);
    FeatureUI_drawButton(&(GpsWardriver_s_wardSetBtns[5]), false);
    FeatureUI_drawButton(&(GpsWardriver_s_wardSetBtns[6]), false);
  }

  if (!featureHasTouchNavBar()) {
    FeatureUI_drawButton(&(GpsWardriver_s_wardSetBtns[7]), false);
    FeatureUI_drawButton(&(GpsWardriver_s_wardSetBtns[8]), false);
  }
}

#ifndef WARD_WIGLE_CFG_PATH
#define WARD_WIGLE_CFG_PATH "/config/wigle.txt"
#endif
#ifndef WARD_WIGLE_TMP_CSV
#define WARD_WIGLE_TMP_CSV "/wd_wigle_upload.csv"
#endif

static void GpsWardriver_wardTrimEnd(char* s) {
  if (!s) {
    return;
  }
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ' ||
                   s[n - 1] == '\t')) {
    s[--n] = '\0';
  }
}

// ---- WiGLE on-device editor (uses KeyboardUI; writes WARD_WIGLE_CFG_PATH)
// ----
static char GpsWardriver_s_wigUser[96];
static char GpsWardriver_s_wigTok[160];
static char GpsWardriver_s_wigSsid[80];
static char GpsWardriver_s_wigPass[80];
static bool GpsWardriver_s_wigUseSavedWifi = false;
static bool GpsWardriver_s_wigleCfgDirty = true;

static void GpsWardriver_wardWigleSanitizeOneLine(char* s) {
  if (!s) {
    return;
  }
  for (char* p = s; *p; ++p) {
    if (*p == '\r' || *p == '\n') {
      *p = ' ';
    }
  }
}

static void ward_read_data_line(storage_file_t* file, char* buffer,
                                size_t capacity) {
  if (!buffer || !capacity) return;
  buffer[0] = 0;
  while (storage_file_available(file)) {
    size_t n = storage_file_read_until(file, '\n', buffer, capacity - 1);
    buffer[n] = 0;
    GpsWardriver_wardTrimEnd(buffer);
    const char* p = buffer;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p && *p != '#') return;
  }
}

static void GpsWardriver_wardWigleLoadEditBuffers(void) {
  memset(GpsWardriver_s_wigUser, 0, sizeof(GpsWardriver_s_wigUser));
  memset(GpsWardriver_s_wigTok, 0, sizeof(GpsWardriver_s_wigTok));
  memset(GpsWardriver_s_wigSsid, 0, sizeof(GpsWardriver_s_wigSsid));
  memset(GpsWardriver_s_wigPass, 0, sizeof(GpsWardriver_s_wigPass));
  GpsWardriver_s_wigUseSavedWifi = false;
  storage_file_t f __attribute__((cleanup(storage_file_close))) =
      storage_file_open(WARD_WIGLE_CFG_PATH, "rb");
  if (!storage_file_valid(&(f))) {
    GpsWardriver_s_wigleCfgDirty = true;
    return;
  }

  ward_read_data_line(&f, GpsWardriver_s_wigUser,
                      sizeof(GpsWardriver_s_wigUser));
  ward_read_data_line(&f, GpsWardriver_s_wigTok, sizeof(GpsWardriver_s_wigTok));
  ward_read_data_line(&f, GpsWardriver_s_wigSsid,
                      sizeof(GpsWardriver_s_wigSsid));
  ward_read_data_line(&f, GpsWardriver_s_wigPass,
                      sizeof(GpsWardriver_s_wigPass));
  storage_file_close(&(f));
  bool autoWifi =
      !GpsWardriver_s_wigSsid[0] || strcmp(GpsWardriver_s_wigSsid, "-") == 0;
  if (!autoWifi) {
    const char* p = GpsWardriver_s_wigSsid;
    const char* q = "auto";
    for (; *p && *q; ++p, ++q) {
      if (tolower((unsigned char)*p) != tolower((unsigned char)*q)) {
        break;
      }
    }
    autoWifi = (*p == '\0' && *q == '\0');
  }
  if (autoWifi) {
    GpsWardriver_s_wigUseSavedWifi = true;
    GpsWardriver_s_wigSsid[0] = '\0';
    GpsWardriver_s_wigPass[0] = '\0';
  }
  GpsWardriver_wardWigleSanitizeOneLine(GpsWardriver_s_wigUser);
  GpsWardriver_wardWigleSanitizeOneLine(GpsWardriver_s_wigTok);
  GpsWardriver_wardWigleSanitizeOneLine(GpsWardriver_s_wigSsid);
  GpsWardriver_wardWigleSanitizeOneLine(GpsWardriver_s_wigPass);
  GpsWardriver_s_wigleCfgDirty = true;
}

static bool GpsWardriver_wardWigleEnsureConfigDir(void) {
  if (!isSDCardAvailable()) {
    return false;
  }
  if (storage_exists("/config")) {
    return true;
  }
  return storage_mkdir("/config");
}

static bool GpsWardriver_wardWigleSaveCfgToSd(void) {
  GpsWardriver_wardWigleSanitizeOneLine(GpsWardriver_s_wigUser);
  GpsWardriver_wardWigleSanitizeOneLine(GpsWardriver_s_wigTok);
  GpsWardriver_wardWigleSanitizeOneLine(GpsWardriver_s_wigSsid);
  GpsWardriver_wardWigleSanitizeOneLine(GpsWardriver_s_wigPass);
  if (!GpsWardriver_s_wigUser[0] || !GpsWardriver_s_wigTok[0]) {
    GpsWardriver_wardNotify("WiGLE", "API name and token are required.");
    return false;
  }
  if (!GpsWardriver_wardWigleEnsureConfigDir()) {
    GpsWardriver_wardNotify("WiGLE", "Cannot create /config on storage::");
    return false;
  }
  storage_file_t out __attribute__((cleanup(storage_file_close))) =
      storage_file_open(WARD_WIGLE_CFG_PATH, "wb");
  if (!storage_file_valid(&(out))) {
    GpsWardriver_wardNotify("WiGLE", "Cannot write wigle.txt");
    return false;
  }
  storage_file_print_line_cstr(&(out), (TEXT(GpsWardriver_s_wigUser)).data);
  storage_file_print_line_cstr(&(out), (TEXT(GpsWardriver_s_wigTok)).data);
  if (GpsWardriver_s_wigUseSavedWifi) {
    storage_file_print_line_cstr(&(out), "AUTO");
    storage_file_print_line_cstr(&(out), "");
  } else {
    storage_file_print_line_cstr(&(out), (TEXT(GpsWardriver_s_wigSsid)).data);
    storage_file_print_line_cstr(&(out), (TEXT(GpsWardriver_s_wigPass)).data);
  }
  storage_file_close(&(out));
  GpsWardriver_wardNotify("WiGLE", "Saved /config/wigle.txt");
  return true;
}

static void GpsWardriver_wardDrawWigleCfgPage(void);

static void GpsWardriver_wardWigleRunFieldEditor(const char* title, char* buf,
                                                 size_t cap, uint8_t maxKbLen,
                                                 bool requireNonEmpty) {
  GpsWardriver_s_fgScanPaused = true;
  sys_sleep_ms(200);
  OnScreenKeyboardConfig cfg = OnScreenKeyboardConfig_INITIALIZER;
  cfg.titleLine1 = title;
  cfg.titleLine2 = "^ caps, # sym";
  osKeyboardUseStandardLayout(&(cfg));
  cfg.maxLen = maxKbLen;
  cfg.shuffleNames = NULL;
  cfg.shuffleCount = 0;
  cfg.buttonsY = 195;
  cfg.backLabel = "Back";
  cfg.middleLabel = "Del";
  cfg.okLabel = "OK";
  cfg.enableShuffle = false;
  cfg.requireNonEmpty = requireNonEmpty;
  cfg.emptyErrorMsg = requireNonEmpty ? "Cannot be empty" : "";
  text_t init = TEXT(buf);
  const OnScreenKeyboardResult r = showOnScreenKeyboard(&(cfg), init);
  GpsWardriver_s_fgScanPaused = false;
  if (r.accepted) {
    strncpy(buf, (r.text).data, cap - 1);
    buf[cap - 1] = '\0';
  }
  GpsWardriver_wardUiInvalidateAll();
  GpsWardriver_wardDrawWigleCfgPage();
  GpsWardriver_wardSetNavLabels(GpsWardriver_WardPage_WigleCfg, false, false);
  GpsWardriver_wardDrawIconToolbar(GpsWardriver_WardPage_WigleCfg,
                                   GpsWardriver_s_bgTask != NULL, false);
  GpsWardriver_wardPumpNavChrome();
  GpsWardriver_s_lastWardInputMs = sys_monotonic_ms();
}

enum { GpsWardriver_kWigleRowH = 30 };
enum { GpsWardriver_kWigleRowGap = 4 };
enum { GpsWardriver_kWigleFirstRowY = GpsWardriver_kWardBodyY + 34 };

static int GpsWardriver_wardWigleHitBodyRow(int x, int y) {
  if (x < 8 || x >= display_width(&(tft)) - 8) {
    return -1;
  }
  const int ry = y - GpsWardriver_kWigleFirstRowY;
  const int step = GpsWardriver_kWigleRowH + GpsWardriver_kWigleRowGap;
  if (ry < 0 || step <= 0) {
    return -1;
  }
  const int idx = ry / step;
  if (idx < 0 || idx > 4) {
    return -1;
  }
  const int within = ry - idx * step;
  if (within > GpsWardriver_kWigleRowH) {
    return -1;
  }
  return idx;
}

static void ward_row_card(int idx, const char* label, const char* val,
                          bool dim) {
  const int pad = GpsWardriver_kWSetPad;
  const int cardW = display_width(&tft) - 2 * pad;
  const int y = GpsWardriver_kWigleFirstRowY +
                idx * (GpsWardriver_kWigleRowH + GpsWardriver_kWigleRowGap);
  const int labelY = y + GpsWardriver_kWardInnerPad;
  const int valueY =
      y + GpsWardriver_kWigleRowH - GpsWardriver_kWardInnerPad - 8;
  const int valueMaxW = cardW - GpsWardriver_kWardInnerPad * 2;
  display_fill_round_rect(&(tft), pad, y, cardW, GpsWardriver_kWigleRowH,
                          GpsWardriver_kWardUiRadius, UI_FG);
  display_draw_round_rect(&(tft), pad, y, cardW, GpsWardriver_kWigleRowH,
                          GpsWardriver_kWardUiRadius, UI_LINE);
  display_set_text_color_2(&(tft), dim ? UI_DIM_TEXT : UI_ICON, UI_FG);
  display_draw_string(&(tft), label, pad + GpsWardriver_kWardInnerPad, labelY,
                      0);
  display_set_text_color_2(&(tft), dim ? UI_DIM_TEXT : UI_TEXT, UI_FG);
  const char* show = (val && val[0]) ? val : "(empty)";
  display_draw_string(&(tft),
                      (GpsWardriver_wardFitTextWidth(show, valueMaxW)).data,
                      pad + GpsWardriver_kWardInnerPad, valueY, 0);
}
static void GpsWardriver_wardDrawWigleCfgPage(void) {
  const int scrW = display_width(&(tft));
  const int contentBottom = GpsWardriver_wardContentBottom();
  const int pad = GpsWardriver_kWSetPad;
  const int cardW = scrW - 2 * pad;
  display_fill_rect(&(tft), 0, GpsWardriver_kWardBodyY, scrW,
                    contentBottom - GpsWardriver_kWardBodyY, FEATURE_BG);
  display_set_text_datum(&(tft), TL_DATUM);
  display_set_text_font(&(tft), 1);
  display_set_text_color_2(&(tft), UI_TEXT, FEATURE_BG);
  display_draw_string(&(tft), "WiGLE config", pad, GpsWardriver_kWardBodyY + 2,
                      0);
  display_set_text_font(&(tft), 1);
  display_set_text_color_2(&(tft), UI_DIM_TEXT, FEATURE_BG);
  if (!featureHasTouchNavBar()) {
    display_draw_string(&(tft), "Back", pad, GpsWardriver_kWardBodyY + 18, 0);
    display_draw_string(&(tft), "Check = save to SD", pad + 42,
                        GpsWardriver_kWardBodyY + 18, 0);
  }

  ward_row_card(0, "API name", GpsWardriver_s_wigUser, false);
  ward_row_card(1, "API token", GpsWardriver_s_wigTok, false);
  ward_row_card(2, "Wi-Fi SSID", GpsWardriver_s_wigSsid,
                GpsWardriver_s_wigUseSavedWifi);
  ward_row_card(3, "Wi-Fi password", GpsWardriver_s_wigPass,
                GpsWardriver_s_wigUseSavedWifi);
  const int y4 = GpsWardriver_kWigleFirstRowY +
                 4 * (GpsWardriver_kWigleRowH + GpsWardriver_kWigleRowGap);
  display_fill_round_rect(&(tft), pad, y4, cardW, GpsWardriver_kWigleRowH,
                          GpsWardriver_kWardUiRadius, UI_FG);
  display_draw_round_rect(&(tft), pad, y4, cardW, GpsWardriver_kWigleRowH,
                          GpsWardriver_kWardUiRadius, UI_LINE);
  display_set_text_color_2(&(tft), UI_TEXT, UI_FG);
  const char* autoLine = GpsWardriver_s_wigUseSavedWifi
                             ? "Auto Wi-Fi ON (tap to edit SSID)"
                             : "Auto Wi-Fi OFF (tap for saved NVS)";
  display_draw_string(&(tft),
                      (GpsWardriver_wardFitTextWidth(
                           autoLine, cardW - GpsWardriver_kWardInnerPad * 2))
                          .data,
                      pad + GpsWardriver_kWardInnerPad,
                      y4 + (GpsWardriver_kWigleRowH - 8) / 2, 0);
}

static int GpsWardriver_wardPollWigleCfgCombinedHit(void) {
  int x = 0, y = 0;
  if (!readTouchXYDismiss(&(x), &(y))) {
    return -1;
  }
  const uint32_t t = sys_monotonic_ms();
  if ((uint32_t)(t - GpsWardriver_s_lastWardInputMs) < 120u) {
    return -1;
  }
  GpsWardriver_s_lastWardInputMs = t;
  if (y > GpsWardriver_kWardIconRowY &&
      y < GpsWardriver_kWardIconRowY + GpsWardriver_kWardIconRowH) {
    int16_t ix[4];
    GpsWardriver_wardToolbarIconXs(GpsWardriver_WardPage_WigleCfg, ix);
    if (x > ix[0] && x < ix[0] + GpsWardriver_kWardIconSz) {
      GpsWardriver_s_lastTbMs = t;
      return 0;
    }
    if (x > ix[3] && x < ix[3] + GpsWardriver_kWardIconSz) {
      GpsWardriver_s_lastTbMs = t;
      return 3;
    }
    return -1;
  }
  const int row = GpsWardriver_wardWigleHitBodyRow(x, y);
  if (row >= 0) {
    return 10 + row;
  }
  return -1;
}

static int GpsWardriver_wardWifiChannelToMHz(int ch) {
  if (ch >= 1 && ch <= 13) {
    return 2412 + (ch - 1) * 5;
  }
  if (ch == 14) {
    return 2484;
  }
  if (ch >= 32) {
    return 5000 + ch * 5;
  }
  return 0;
}

static void GpsWardriver_wardMacToLowerColons(char* mac) {
  if (!mac) {
    return;
  }
  for (char* p = mac; *p; ++p) {
    if (*p >= 'A' && *p <= 'Z') {
      *p = (char)(*p - 'A' + 'a');
    }
  }
}

static void GpsWardriver_wardAuthToWigleCaps(const char* auth, char* out,
                                             size_t outSz) {
  if (!out || outSz < 8u) {
    return;
  }
  out[0] = '\0';
  if (!auth || !auth[0]) {
    snprintf(out, outSz, "[ESS]");
    return;
  }
  if (!strcmp(auth, "OPEN")) {
    snprintf(out, outSz, "[ESS]");
  } else if (!strcmp(auth, "WEP")) {
    snprintf(out, outSz, "[WEP][ESS]");
  } else if (!strcmp(auth, "WPA_PSK")) {
    snprintf(out, outSz, "[WPA-PSK-CCMP][ESS]");
  } else if (!strcmp(auth, "WPA2_PSK")) {
    snprintf(out, outSz, "[WPA2-PSK-CCMP][ESS]");
  } else if (!strcmp(auth, "WPA_WPA2_PSK")) {
    snprintf(out, outSz, "[WPA2-PSK-CCMP][WPA-PSK-CCMP][ESS]");
  } else if (!strcmp(auth, "WPA2_ENT")) {
    snprintf(out, outSz, "[WPA2-EAP-CCMP][ESS]");
  } else if (!strcmp(auth, "WPA3_PSK")) {
    snprintf(out, outSz, "[WPA3-SAE-CCMP][ESS]");
  } else if (!strcmp(auth, "WPA2_WPA3_PSK")) {
    snprintf(out, outSz, "[WPA2-PSK-CCMP][WPA3-SAE-CCMP][ESS]");
  } else {
    snprintf(out, outSz, "[%s][ESS]", auth);
  }
}

/** Split ward CSV line into fields (handles quoted SSID/name with commas).
 * Returns field count. */
static int GpsWardriver_wardSplitWardCsvLine(char* line, char* fields[],
                                             int maxFields) {
  bool inq = false;
  char* start = line;
  int nf = 0;
  for (char* p = line; *p && nf < maxFields; ++p) {
    if (*p == '"') {
      if (inq && p[1] == '"') {
        ++p;
        continue;
      }
      inq = !inq;
      continue;
    }
    if (*p == ',' && !inq) {
      *p = '\0';
      fields[nf++] = start;
      start = p + 1;
    }
  }
  fields[nf++] = start;
  return nf;
}

/** Strip RFC4180 outer quotes and `""` escapes (field points into split line
 * buffer). */
static void GpsWardriver_wardStripCsvQuotes(char* s) {
  if (!s || !s[0]) {
    return;
  }
  size_t n = strlen(s);
  if (n >= 2u && s[0] == '"' && s[n - 1u] == '"') {
    memmove(s, s + 1, n - 2u);
    s[n - 2u] = '\0';
    char* w = s;
    for (const char* p = s; *p;) {
      if (*p == '"' && p[1] == '"') {
        *w++ = '"';
        p += 2;
      } else {
        *w++ = *p++;
      }
    }
    *w = '\0';
  }
}

/** `dd/mm/yy` from GPS log -> `yyyy-mm-dd` (UTC assumption). */
static bool GpsWardriver_wardDdMmYyToIso(const char* ddmmyy, char* iso,
                                         size_t isoSz) {
  if (!ddmmyy || !iso || isoSz < 12u) {
    return false;
  }
  int d0 = 0, m0 = 0, y0 = 0;
  if (sscanf(ddmmyy, "%d/%d/%d", &d0, &m0, &y0) != 3) {
    return false;
  }
  int yFull = y0;
  if (yFull < 70) {
    yFull += 2000;
  } else if (yFull < 100) {
    yFull += 1900;
  }
  if (yFull < 1970 || yFull > 9999 || m0 < 1 || m0 > 12 || d0 < 1 || d0 > 31)
    return false;
  snprintf(iso, isoSz, "%04d-%02d-%02d", yFull, m0, d0);
  return true;
}

static bool GpsWardriver_wardConvertWardCsvToWigle(const char* srcPath,
                                                   storage_file_t* out) {
  storage_file_t in __attribute__((cleanup(storage_file_close))) =
      storage_file_open(srcPath, "rb");
  if (!storage_file_valid(&(in))) {
    return false;
  }
  storage_file_print_cstr(
      &((*out)),
      "WigleWifi-1.6,appRelease=1.0,model=ESP32-DIV,release=1.0,device="
      "wardrive,display=,board=ESP32,"
      "brand=ESP32-DIV,star=Sol,body=3,subBody=0\r\n");
  storage_file_print_cstr(
      &((*out)),
      "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,"
      "CurrentLongitude,AltitudeMeters,"
      "AccuracyMeters,RCOIs,MfgrId,Type\r\n");

  char line[400];
  while (storage_file_available(&(in))) {
    size_t n = storage_file_read_until(&(in), '\n', line, sizeof(line) - 1u);
    line[n] = '\0';
    GpsWardriver_wardTrimEnd(line);
    if (!line[0]) {
      continue;
    }
    if (strncmp(line, "epoch_ms", 8) == 0) {
      continue;
    }
    char* fld[20];
    int nf = GpsWardriver_wardSplitWardCsvLine(line, fld, 20);
    if (nf < 14) {
      continue;
    }
    const char* utc = fld[1];
    const char* date = fld[2];
    const char* lat = fld[3];
    const char* lon = fld[4];
    const char* alt = fld[5];
    const char* hdop = fld[8];
    const char* radio = fld[9];
    const char* bssid = fld[11];
    const char* chs = fld[12];
    const char* rssi = fld[13];
    const char* auth = (nf >= 15) ? fld[14] : "";
    GpsWardriver_wardStripCsvQuotes(fld[10]);

    char isoDate[16];
    if (!GpsWardriver_wardDdMmYyToIso(date, isoDate, sizeof(isoDate))) {
      snprintf(isoDate, sizeof(isoDate), "1970-01-01");
    }
    char firstSeen[40];
    snprintf(firstSeen, sizeof(firstSeen), "%s %s", isoDate, utc);

    double acc = 25.0;
    if (hdop && hdop[0]) {
      double h = strtod(hdop, NULL);
      if (h > 0.0 && h < 200.0) {
        acc = h * 4.0;
      }
    }
    int altM = 0;
    if (alt && alt[0]) {
      altM = (int)lroundf((float)strtod(alt, NULL));
    }

    const char* latS = (lat && lat[0]) ? lat : "0.0";
    const char* lonS = (lon && lon[0]) ? lon : "0.0";

    if (!strcmp(radio, "WIFI")) {
      char caps[96];
      GpsWardriver_wardAuthToWigleCaps(auth, caps, sizeof(caps));
      char mac[24];
      div_copy_string(mac, sizeof(mac), bssid);
      GpsWardriver_wardMacToLowerColons(mac);
      int ch = (int)strtol(chs, NULL, 10);
      int mhz = GpsWardriver_wardWifiChannelToMHz(ch);
      storage_file_print_cstr(&((*out)), (TEXT(mac)).data);
      storage_file_print_cstr(&((*out)), (text_character(',')).data);
      GpsWardriver_wardAppendCsvString(&((*out)), fld[10]);
      storage_file_print_cstr(&((*out)), (text_character(',')).data);
      GpsWardriver_wardAppendCsvString(&((*out)), caps);
      storage_file_printf(&((*out)), ",%s,%d,%d,%s,%s,%s,%d,%.2f,,,WIFI\r\n",
                          firstSeen, ch, mhz, rssi, latS, lonS, altM, acc);
    } else if (!strcmp(radio, "BLE")) {
      char mac[24];
      div_copy_string(mac, sizeof(mac), bssid);
      GpsWardriver_wardMacToLowerColons(mac);
      storage_file_print_cstr(&((*out)), (TEXT(mac)).data);
      storage_file_print_cstr(&((*out)), (text_character(',')).data);
      GpsWardriver_wardAppendCsvString(&((*out)), fld[10]);
      storage_file_print_cstr(&((*out)), (text_character(',')).data);
      GpsWardriver_wardAppendCsvString(&((*out)), "Misc [LE]");
      storage_file_printf(&((*out)), ",%s,0,,%s,%s,%s,%d,%.2f,,,BLE\r\n",
                          firstSeen, rssi, latS, lonS, altM, acc);
    }
  }
  storage_file_close(&(in));
  return true;
}

/** If line 3 is empty, `-`, or `auto`, use `network::connect()` (ESP32 NVS
 * credentials from a prior connect). */
static bool GpsWardriver_wardReadWigleConfig(char* apiUser, size_t userSz,
                                             char* apiTok, size_t tokSz,
                                             char* staSsid, size_t ssidSz,
                                             char* staPass, size_t passSz,
                                             bool* useStoredWifi) {
  if (!apiUser || !apiTok || !staSsid || !staPass) {
    return false;
  }
  if (useStoredWifi) {
    *useStoredWifi = false;
  }
  apiUser[0] = apiTok[0] = staSsid[0] = staPass[0] = '\0';
  storage_file_t f __attribute__((cleanup(storage_file_close))) =
      storage_file_open(WARD_WIGLE_CFG_PATH, "rb");
  if (!storage_file_valid(&(f))) {
    return false;
  }

  /** Next non-empty line that is not a # comment (for hand-edited templates).
   */

  ward_read_data_line(&f, apiUser, userSz);
  ward_read_data_line(&f, apiTok, tokSz);
  ward_read_data_line(&f, staSsid, ssidSz);
  ward_read_data_line(&f, staPass, passSz);
  storage_file_close(&(f));
  if (!apiUser[0] || !apiTok[0]) {
    return false;
  }
  bool autoWifi = !staSsid[0] || strcmp(staSsid, "-") == 0;
  if (!autoWifi) {
    const char* p = staSsid;
    const char* q = "auto";
    for (; *p && *q; ++p, ++q) {
      if (tolower((unsigned char)*p) != tolower((unsigned char)*q)) {
        break;
      }
    }
    autoWifi = (*p == '\0' && *q == '\0');
  }
  if (autoWifi) {
    if (useStoredWifi) {
      *useStoredWifi = true;
    }
    staSsid[0] = '\0';
    staPass[0] = '\0';
  }
  return true;
}

static bool GpsWardriver_wardBuildBasicAuth(const char* user, const char* token,
                                            char* b64, size_t b64Cap) {
  char plain[320];
  snprintf(plain, sizeof(plain), "%s:%s", user, token);
  size_t plainLen = strlen(plain);
  size_t olen = 0;
  int err = mbedtls_base64_encode((unsigned char*)b64, b64Cap, &olen,
                                  (const unsigned char*)plain, plainLen);
  if (err != 0 || olen == 0 || olen >= b64Cap) {
    return false;
  }
  b64[olen] = '\0';
  return true;
}

static bool ward_http_write_all(esp_http_client_handle_t client,
                                const char* data, size_t length) {
  while (length) {
    int n = esp_http_client_write(client, data, length);
    if (n <= 0) return false;
    data += n;
    length -= n;
  }
  return true;
}
static bool GpsWardriver_wardWigleStreamUpload(storage_file_t* csv,
                                               const char* uploadName,
                                               const char* b64Auth,
                                               char* errBuf, size_t errCap) {
  const char* reason = NULL;
  bool accepted = false;
  esp_http_client_handle_t client = NULL;
  char filename[256], head[512], tail[64], auth[544], content_type[96];
  const char* boundary = "----esp32divWigle7aF3";
  if (!network_sync_clock(10000)) {
    reason = "Clock sync failed; check Internet access";
    goto done;
  }
  div_copy_string(filename, sizeof(filename),
                  uploadName ? uploadName : "wardrive.csv");
  for (char* c = filename; *c; ++c)
    if (*c == '\r' || *c == '\n' || *c == '"' || *c == '\\') *c = '_';
  int header_length = snprintf(
      head, sizeof(head),
      "--%s\r\nContent-Disposition: form-data; "
      "name=\"donate\"\r\n\r\nfalse\r\n--%s\r\nContent-Disposition: form-data; "
      "name=\"file\"; filename=\"%s\"\r\nContent-Type: text/csv\r\n\r\n",
      boundary, boundary, filename);
  int tail_length = snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", boundary);
  size_t file_size = storage_file_size(csv);
  if (header_length < 0 || header_length >= sizeof(head) || tail_length < 0 ||
      tail_length >= sizeof(tail) || !file_size ||
      file_size > INT_MAX - header_length - tail_length ||
      !storage_file_seek(csv, 0)) {
    reason = "Invalid CSV file";
    goto done;
  }
  esp_http_client_config_t config = {
      .url = "https://api.wigle.net/api/v2/file/upload",
      .method = HTTP_METHOD_POST,
      .timeout_ms = 15000,
      .crt_bundle_attach = esp_crt_bundle_attach};
  client = esp_http_client_init(&config);
  if (!client) {
    reason = "HTTP client allocation failed";
    goto done;
  }
  if (snprintf(auth, sizeof(auth), "Basic %s", b64Auth) >= sizeof(auth)) {
    reason = "Auth too long";
    goto done;
  }
  snprintf(content_type, sizeof(content_type),
           "multipart/form-data; boundary=%s", boundary);
  esp_http_client_set_header(client, "Authorization", auth);
  esp_http_client_set_header(client, "Content-Type", content_type);
  if (esp_http_client_open(client, header_length + file_size + tail_length) !=
      ESP_OK) {
    reason = "HTTPS connection failed";
    goto done;
  }
  if (!ward_http_write_all(client, head, header_length)) {
    reason = "Upload header failed";
    goto done;
  }
  char chunk[1024];
  size_t remaining = file_size;
  while (remaining) {
    int n = storage_file_read(csv, chunk, DIV_MIN(remaining, sizeof(chunk)));
    if (n <= 0 || !ward_http_write_all(client, chunk, n)) {
      reason = "CSV upload interrupted";
      goto done;
    }
    remaining -= n;
    sys_yield_task();
  }
  if (!ward_http_write_all(client, tail, tail_length)) {
    reason = "Upload interrupted";
    goto done;
  }
  if (esp_http_client_fetch_headers(client) < 0) {
    reason = "No HTTP response";
    goto done;
  }
  int status = esp_http_client_get_status_code(client),
      n = esp_http_client_read_response(client, chunk, sizeof(chunk) - 1);
  if (n < 0) {
    reason = "Invalid HTTP response";
    goto done;
  }
  chunk[n] = 0;
  if (status < 200 || status >= 300) {
    if (errBuf && errCap) snprintf(errBuf, errCap, "HTTP %d", status);
    goto done;
  }
  cJSON* json = cJSON_Parse(chunk);
  accepted =
      json && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "success"));
  cJSON_Delete(json);
  if (!accepted) reason = "WiGLE did not accept this file";
done:
  if (reason && errBuf && errCap) div_copy_string(errBuf, errCap, reason);
  if (client) esp_http_client_cleanup(client);
  return accepted;
}

/** Pause scans, join Wi?Fi, convert CSV to WiGLE v1.6, upload, disconnect. */
static void GpsWardriver_wardPerformWigleUpload(const char* srcCsvPath,
                                                storage_file_t* liveLog) {
  char apiUser[96];
  char apiTok[128];
  char staSsid[80];
  char staPass[80];
  bool useStoredWifi = false;
  if (!GpsWardriver_wardReadWigleConfig(
          apiUser, sizeof(apiUser), apiTok, sizeof(apiTok), staSsid,
          sizeof(staSsid), staPass, sizeof(staPass), &useStoredWifi)) {
    GpsWardriver_wardNotify(
        "WiGLE",
        "SD:/config/wigle.txt - line1: WiGLE API name, line2: token. "
        "If this ESP32 already "
        "joined Wi?Fi once, stop at line 2. Else add line3=SSID "
        "line4=password. Optional "
        "line3 AUTO or - uses saved Wi?Fi only.");
    return;
  }

  char b64[512];
  if (!GpsWardriver_wardBuildBasicAuth(apiUser, apiTok, b64, sizeof(b64))) {
    GpsWardriver_wardNotify("WiGLE", "Auth encode failed.");
    return;
  }

  GpsWardriver_wardLogPush("WiGLE: start");
  GpsWardriver_s_fgScanPaused = true;
  sys_sleep_ms(450);

  if (liveLog) {
    storage_file_flush(liveLog);
  }

  network_clear_scan();
  network_disconnect_options(false, false);
  sys_sleep_ms(150);
  network_set_mode(WIFI_MODE_STA);
  if (useStoredWifi) {
    network_reconnect();
  } else {
    network_connect_config(staSsid, staPass, 0, NULL);
  }

  bool gotIp = false;
  for (int i = 0; i < 70; i++) {
    if (network_connected() == true) {
      gotIp = true;
      break;
    }
    sys_sleep_ms(400);
  }
  if (!gotIp) {
    GpsWardriver_wardNotify(
        "WiGLE",
        useStoredWifi
            ? "WiFi failed. Connect this device to Wi?Fi once in another tool, "
              "or add SSID + password on lines 3-4 of wigle.txt."
            : "WiFi connect failed (check wigle.txt SSID/pass).");
    network_disconnect_options(false, false);
    sys_sleep_ms(100);
    GpsWardriver_s_fgScanPaused = false;
    return;
  }

  GpsWardriver_wardLogPush("WiGLE: WiFi OK");
  storage_file_t out __attribute__((cleanup(storage_file_close))) =
      storage_file_open(WARD_WIGLE_TMP_CSV, "wb");
  if (!storage_file_valid(&(out))) {
    GpsWardriver_wardNotify("WiGLE", "Could not write temp CSV on storage::");
    network_disconnect_options(false, false);
    sys_sleep_ms(100);
    GpsWardriver_s_fgScanPaused = false;
    return;
  }
  GpsWardriver_wardLogPush("WiGLE: convert");
  if (!GpsWardriver_wardConvertWardCsvToWigle(srcCsvPath, &(out))) {
    storage_file_close(&(out));
    storage_remove(WARD_WIGLE_TMP_CSV);
    GpsWardriver_wardNotify("WiGLE", "Could not read ward CSV or convert.");
    network_disconnect_options(false, false);
    sys_sleep_ms(100);
    GpsWardriver_s_fgScanPaused = false;
    return;
  }
  storage_file_flush(&(out));
  storage_file_close(&(out));

  storage_file_t up __attribute__((cleanup(storage_file_close))) =
      storage_file_open(WARD_WIGLE_TMP_CSV, "rb");
  if (!storage_file_valid(&(up)) || storage_file_size(&(up)) < 64) {
    if (storage_file_valid(&(up))) {
      storage_file_close(&(up));
    }
    storage_remove(WARD_WIGLE_TMP_CSV);
    GpsWardriver_wardNotify("WiGLE", "Converted file empty or missing.");
    network_disconnect_options(false, false);
    sys_sleep_ms(100);
    GpsWardriver_s_fgScanPaused = false;
    return;
  }

  const char* slash = strrchr(srcCsvPath, '/');
  const char* base = slash ? slash + 1 : srcCsvPath;
  char uploadName[48];
  div_copy_string(uploadName, sizeof(uploadName), base);
  for (char* p = uploadName; *p; ++p) {
    if (*p == '"' || *p == '\\' || *p == '\r' || *p == '\n') {
      *p = '_';
    }
  }

  GpsWardriver_wardLogPush("WiGLE: POST");
  char err[160];
  bool ok = GpsWardriver_wardWigleStreamUpload(
      &(up), uploadName[0] ? uploadName : "wardrive.csv", b64, err,
      sizeof(err));
  storage_file_close(&(up));
  storage_remove(WARD_WIGLE_TMP_CSV);

  network_disconnect_options(false, false);
  sys_sleep_ms(120);

  GpsWardriver_s_fgScanPaused = false;

  if (ok) {
    GpsWardriver_wardNotify("WiGLE", "Upload OK. Check wigle.net uploads.");
  } else {
    GpsWardriver_wardNotify("WiGLE", err[0] ? err : "Upload failed.");
  }
}

static void GpsWardriver_wardPrintCsvGpsPrefix(storage_file_t* logf,
                                               uint32_t now) {
  storage_file_print_cstr(&((*logf)), (TEXT((unsigned long)now)).data);
  storage_file_print_cstr(&((*logf)), (text_character(',')).data);
  storage_file_print_cstr(&((*logf)), (TEXT(gps.state.utc)).data);
  storage_file_print_cstr(&((*logf)), (text_character(',')).data);
  storage_file_print_cstr(&((*logf)), (TEXT(gps.state.date)).data);
  storage_file_print_cstr(&((*logf)), (text_character(',')).data);
  if (!isnan(gps.state.latitude) && !isnan(gps.state.longitude)) {
    storage_file_printf(&((*logf)), "%.7f,%.7f,", gps.state.latitude,
                        gps.state.longitude);
  } else {
    storage_file_print_cstr(&((*logf)), ",,");
  }
  if (!isnan(gps.state.altitude_m)) {
    storage_file_printf(&((*logf)), "%.1f,", (double)gps.state.altitude_m);
  } else {
    storage_file_print_cstr(&((*logf)), (text_character(',')).data);
  }
  storage_file_print_cstr(&((*logf)), (TEXT((int)gps.state.fix_quality)).data);
  storage_file_print_cstr(&((*logf)), (text_character(',')).data);
  storage_file_print_cstr(&((*logf)),
                          (TEXT((unsigned)gps.state.satellites_used)).data);
  storage_file_print_cstr(&((*logf)), (text_character(',')).data);
  if (gps.state.hdop >= 0.f) {
    storage_file_printf(&((*logf)), "%.2f,", (double)gps.state.hdop);
  } else {
    storage_file_print_cstr(&((*logf)), (text_character(',')).data);
  }
}

static void GpsWardriver_wardDoWifiScanLog(storage_file_t* logf, uint32_t now,
                                           uint32_t* linesWr,
                                           uint32_t* scanCt) {
  network_clear_scan();
  const int n = network_scan(false, true, false, 120);
  (*scanCt)++;
  if (n <= 0) {
    GpsWardriver_wardLogPush("WiFi: 0 AP");
    network_clear_scan();
    return;
  }
  int cap = n;
  if (cap > (int)GpsWardriver_s_cfgMaxAps) {
    cap = (int)GpsWardriver_s_cfgMaxAps;
  }
  for (int i = 0; i < cap; ++i) {
    text_t ss = network_ssid(i);
    const char* ssidc = (ss).data;
    const uint8_t* bssid = network_bssid(i);
    char mac[20];
    if (bssid) {
      snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", bssid[0],
               bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
    } else {
      mac[0] = '\0';
    }
    GpsWardriver_wardLogPushWifiAp(ssidc, network_channel(i), network_rssi(i),
                                   mac);
    GpsWardriver_wardPrintCsvGpsPrefix(&((*logf)), now);
    storage_file_print_cstr(&((*logf)), "WIFI,");
    GpsWardriver_wardAppendCsvString(&((*logf)), ssidc);
    storage_file_print_cstr(&((*logf)), (text_character(',')).data);
    storage_file_print_cstr(&((*logf)), (TEXT(mac)).data);
    storage_file_print_cstr(&((*logf)), (text_character(',')).data);
    storage_file_print_cstr(&((*logf)), (TEXT(network_channel(i))).data);
    storage_file_print_cstr(&((*logf)), (text_character(',')).data);
    storage_file_print_cstr(&((*logf)), (TEXT(network_rssi(i))).data);
    storage_file_print_cstr(&((*logf)), (text_character(',')).data);
    storage_file_print_line_cstr(
        &((*logf)),
        (TEXT(GpsWardriver_wardAuthLabel(network_auth_mode(i)))).data);
    (*linesWr)++;
  }
  storage_file_flush(&((*logf)));
  network_clear_scan();
}

static bool GpsWardriver_s_wardBleScanReady = false;

static void GpsWardriver_wardEnsureBleScan(void) {
  if (GpsWardriver_s_wardBleScanReady) {
    return;
  }
  ble_scan_t* s = ble_service_scan();
  if (!s) {
    return;
  }
  (s->active = true);
  (s->interval_ms = 134);
  (s->window_ms = 99);
  GpsWardriver_s_wardBleScanReady = true;
}

static void GpsWardriver_wardDoBleScanLog(storage_file_t* logf, uint32_t now,
                                          uint32_t* linesWr, uint32_t* scanCt) {
  GpsWardriver_wardEnsureBleScan();
  ble_scan_t* scan = ble_service_scan();
  if (!scan) {
    (*scanCt)++;
    GpsWardriver_wardLogPush("BLE: no scan");
    return;
  }
  ble_results_t results = ble_scan_results(scan, 2000, false);
  (*scanCt)++;
  const int n = (results).count;
  if (n <= 0) {
    GpsWardriver_wardLogPush("BLE: 0 dev");
    storage_file_flush(&((*logf)));
    return;
  }
  int cap = n;
  if (cap > (int)GpsWardriver_s_cfgMaxAps) {
    cap = (int)GpsWardriver_s_cfgMaxAps;
  }
  for (int i = 0; i < cap; ++i) {
    const ble_device_t* d = ble_results_device(&(results), i);
    if (!d) {
      continue;
    }
    char mac[24];
    div_copy_string(mac, sizeof(mac), (ble_device_address(d)).data);
    text_t bleName = ble_device_name(d);
    const char* namec = strlen((bleName).data) ? (bleName).data : "";
    GpsWardriver_wardLogPushBleDev(namec, d->rssi, mac);
    GpsWardriver_wardPrintCsvGpsPrefix(&((*logf)), now);
    storage_file_print_cstr(&((*logf)), "BLE,");
    GpsWardriver_wardAppendCsvString(&((*logf)), namec);
    storage_file_print_cstr(&((*logf)), (text_character(',')).data);
    storage_file_print_cstr(&((*logf)), (TEXT(mac)).data);
    storage_file_print_cstr(&((*logf)), (text_character(',')).data);
    storage_file_print_cstr(&((*logf)), (TEXT(0)).data);
    storage_file_print_cstr(&((*logf)), (text_character(',')).data);
    storage_file_print_cstr(&((*logf)), (TEXT(d->rssi)).data);
    storage_file_print_cstr(&((*logf)), (text_character(',')).data);
    storage_file_print_line_cstr(&((*logf)), "");
    (*linesWr)++;
  }
  storage_file_flush(&((*logf)));
}

static void GpsWardriver_wardRunScansForMode(storage_file_t* logf, uint32_t now,
                                             uint32_t* linesWr,
                                             uint32_t* scanCt) {
  const GpsWardriver_WardRadioMode mode =
      (GpsWardriver_WardRadioMode)GpsWardriver_s_cfgRadioMode;
  if (mode == GpsWardriver_WardRadioMode_WiFi ||
      mode == GpsWardriver_WardRadioMode_Both) {
    GpsWardriver_wardDoWifiScanLog(&((*logf)), now, linesWr, scanCt);
  }
  if (mode == GpsWardriver_WardRadioMode_Ble ||
      mode == GpsWardriver_WardRadioMode_Both) {
    GpsWardriver_wardDoBleScanLog(&((*logf)), now, linesWr, scanCt);
  }
}

static void GpsWardriver_wardFgScanTask(void* param) {
  storage_file_t* logf = (storage_file_t*)param;
  uint32_t lines = 0;
  uint32_t scans = 0;
  uint32_t lastScan = sys_monotonic_ms();
  GpsWardriver_s_fgLines = 0;
  GpsWardriver_s_fgScans = 0;

  while (!GpsWardriver_s_fgScanStop) {
    const uint32_t now = sys_monotonic_ms();
    if (!GpsWardriver_s_fgScanPaused && logf &&
        (uint32_t)(now - lastScan) >= GpsWardriver_s_cfgScanMs) {
      lastScan = now;
      uint32_t addL = 0;
      uint32_t oneScan = 0;
      GpsWardriver_wardRunScansForMode(&(*logf), now, &addL, &oneScan);
      lines += addL;
      scans += oneScan;
      GpsWardriver_s_fgLines = lines;
      GpsWardriver_s_fgScans = scans;
    }
    vTaskDelay(pdMS_TO_TICKS(15));
  }

  GpsWardriver_s_fgScanTask = NULL;
  vTaskDelete(NULL);
}

static bool GpsWardriver_wardStartForegroundScanTask(storage_file_t* logf) {
  if (GpsWardriver_s_fgScanTask) {
    return true;
  }
  GpsWardriver_s_fgScanStop = false;
  GpsWardriver_s_fgScanPaused = false;
  GpsWardriver_s_fgLines = 0;
  GpsWardriver_s_fgScans = 0;
  BaseType_t ok =
      xTaskCreatePinnedToCore(GpsWardriver_wardFgScanTask, "wardfg", 10240,
                              &(*logf), 1, &GpsWardriver_s_fgScanTask, 0);
  return ok == pdPASS;
}

static void GpsWardriver_wardStopForegroundScanTask(void) {
  if (!GpsWardriver_s_fgScanTask) {
    return;
  }
  GpsWardriver_s_fgScanStop = true;
  for (int i = 0; i < 1200 && GpsWardriver_s_fgScanTask; i++) {
    sys_sleep_ms(10);
  }
  if (!GpsWardriver_s_fgScanTask) {
    GpsWardriver_s_fgScanStop = false;
  }
}

static storage_file_t GpsWardriver_wardOpenLogFile(char* outPath,
                                                   size_t outPathLen) {
  if (!outPath || outPathLen == 0) {
    return (storage_file_t){0};
  }

  outPath[0] = '\0';
  snprintf(outPath, outPathLen, "/wardriver/wd_%lu.csv",
           (unsigned long)sys_monotonic_ms());
  storage_file_t logf __attribute__((cleanup(storage_file_close))) =
      storage_file_open(outPath, "wb");
  if (storage_file_valid(&(logf))) {
    return storage_file_move(&(logf));
  }

  printf("[Wardriver] SD open failed for %s; remounting and retrying\n",
         outPath);
  storage_unmount();
  sys_sleep_ms(40);

  isSDCardAvailable();

  storage_file_replace(&(logf), storage_file_open(outPath, "wb"));
  if (storage_file_valid(&(logf))) {
    return storage_file_move(&(logf));
  }

  snprintf(outPath, outPathLen, "/wd_%lu.csv",
           (unsigned long)sys_monotonic_ms());
  storage_file_replace(&(logf), storage_file_open(outPath, "wb"));
  if (storage_file_valid(&(logf))) {
    printf("[Wardriver] using root fallback log path %s\n", outPath);
  }
  return storage_file_move(&(logf));
}
static void GpsWardriver_wardBgTask(void*) {
  gnss_parser_reset(&gps);

  gnss_receiver_close(&gps_receiver);
  gnss_receiver_open(&gps_receiver, &gps, GPS_UART_NUM, GPS_UART_BAUD,
                     GPS_UART_RX);
  network_set_mode(WIFI_MODE_STA);
  network_disconnect_options(false, false);
  sys_sleep_ms(80);

  char path[48];
  storage_file_t logf __attribute__((cleanup(storage_file_close))) =
      GpsWardriver_wardOpenLogFile(path, sizeof(path));
  strncpy(GpsWardriver_s_bgPath, path, sizeof(GpsWardriver_s_bgPath));
  if (!storage_file_valid(&(logf))) {
    GpsWardriver_s_bgPath[0] = '\0';
    gnss_receiver_close(&gps_receiver);
    GpsWardriver_s_bgTask = NULL;
    vTaskDelete(NULL);
    return;
  }
  storage_file_print_line_cstr(&(logf),
                               "epoch_ms,utc,date,lat,lon,alt_m,fix,sats,"
                               "hdop,radio,ssid,bssid,ch,rssi_"
                               "dbm,auth");
  storage_file_flush(&(logf));

  uint32_t lines = 0;
  uint32_t scans = 0;
  uint32_t lastScan = 0;
  GpsWardriver_s_bgLines = 0;
  GpsWardriver_s_bgScans = 0;

  while (!GpsWardriver_s_bgStop) {
    gnss_receiver_poll(&gps_receiver);
    const uint32_t now = sys_monotonic_ms();
    if (GpsWardriver_s_bgLog &&
        (uint32_t)(now - lastScan) >= GpsWardriver_s_cfgScanMs) {
      lastScan = now;
      uint32_t addL = 0;
      uint32_t oneScan = 0;
      GpsWardriver_wardRunScansForMode(&(logf), now, &addL, &oneScan);
      lines += addL;
      scans += oneScan;
      GpsWardriver_s_bgLines = lines;
      GpsWardriver_s_bgScans = scans;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  storage_file_flush(&(logf));
  storage_file_close(&(logf));
  network_clear_scan();
  network_disconnect_options(false, false);
  gnss_receiver_close(&gps_receiver);
  GpsWardriver_s_bgPath[0] = '\0';
  GpsWardriver_s_bgTask = NULL;
  vTaskDelete(NULL);
}

bool GpsWardriver_statusBarGpsIconActive(void) {
  return (GpsWardriver_s_bgTask != NULL) ||
         GpsWardriver_s_wardFgSessionForStatusBar;
}

void GpsWardriver_stopBackgroundIfRunning(void) {
  if (!GpsWardriver_s_bgTask) {
    return;
  }
  GpsWardriver_s_bgStop = true;
  for (int i = 0; i < 200; i++) {
    sys_sleep_ms(15);
    if (!GpsWardriver_s_bgTask) {
      break;
    }
  }
  GpsWardriver_s_bgStop = false;
  drawStatusBar(readBatteryVoltage(), true, false);
}

static void GpsWardriver_wardStartBackground(void) {
  if (GpsWardriver_s_bgTask) {
    GpsWardriver_wardNotify("Wardriver", "Background run already active.");
    return;
  }
  if (!isSDCardAvailable()) {
    GpsWardriver_wardNotify("Wardriver", "SD required for background log.");
    return;
  }
  GpsWardriver_s_bgStop = false;
  GpsWardriver_s_bgLog = true;
  GpsWardriver_s_bgLines = 0;
  GpsWardriver_s_bgScans = 0;
  GpsWardriver_s_bgPath[0] = '\0';
  BaseType_t ok =
      xTaskCreatePinnedToCore(GpsWardriver_wardBgTask, "wardrive", 10240, NULL,
                              1, &GpsWardriver_s_bgTask, 0);
  if (ok != pdPASS) {
    GpsWardriver_s_bgTask = NULL;
    GpsWardriver_wardNotify("Wardriver", "Could not start background task.");
    return;
  }
  GpsWardriver_wardNotify("Wardriver",
                          "Running in background. Open Wardriver to stop.");
}

static int GpsWardriver_wardPollToolbarHit(void) {
  int x = 0, y = 0;
  if (!readTouchXYDismiss(&(x), &(y))) {
    return -1;
  }
  const uint32_t t = sys_monotonic_ms();
  if ((uint32_t)(t - GpsWardriver_s_lastTbMs) < 120u) {
    return -1;
  }
  if (!(y > GpsWardriver_kWardIconRowY &&
        y < GpsWardriver_kWardIconRowY + GpsWardriver_kWardIconRowH)) {
    return -1;
  }
  int16_t ix[4];
  GpsWardriver_wardToolbarIconXs(GpsWardriver_WardPage_Main, ix);
  for (int i = 0; i < 4; i++) {
    if (x > ix[i] && x < ix[i] + GpsWardriver_kWardIconSz) {
      GpsWardriver_s_lastTbMs = t;
      GpsWardriver_s_lastWardInputMs = t;
      return i;
    }
  }
  return -1;
}

static int GpsWardriver_wardPollSettingsCombinedHit(void) {
  int x = 0, y = 0;
  if (!readTouchXYDismiss(&(x), &(y))) {
    return -1;
  }
  const uint32_t t = sys_monotonic_ms();
  if ((uint32_t)(t - GpsWardriver_s_lastWardInputMs) < 120u) {
    return -1;
  }
  GpsWardriver_s_lastWardInputMs = t;

  if (y > GpsWardriver_kWardIconRowY &&
      y < GpsWardriver_kWardIconRowY + GpsWardriver_kWardIconRowH) {
    int16_t ix[4];
    GpsWardriver_wardToolbarIconXs(GpsWardriver_WardPage_Settings, ix);
    if (x > ix[0] && x < ix[0] + GpsWardriver_kWardIconSz) {
      GpsWardriver_s_lastTbMs = t;
      return 0;
    }
    if (x > ix[3] && x < ix[3] + GpsWardriver_kWardIconSz) {
      GpsWardriver_s_lastTbMs = t;
      return 3;
    }
    return -1;
  }

  GpsWardriver_wardLayoutSettingsButtons();
  const int btnCount = featureHasTouchNavBar() ? GpsWardriver_kWardSetBtnCount
                                               : GpsWardriver_kWardSetBtnTotal;
  const int h = FeatureUI_hit(GpsWardriver_s_wardSetBtns, btnCount, x, y);
  if (h < 0) {
    return -1;
  }
  return 10 + h;
}

static bool GpsWardriver_wardSessionShouldExit(void) {
  if (feature_exit_requested) {
    return true;
  }
  if (featureHasTouchNavBar()) {
    return isPhysicalButtonPressed(BTN_SELECT);
  }
  return isButtonPressed(BTN_SELECT) || touchRequestsExit();
}

void GpsWardriver_session(void) {
  feature_exit_requested = false;

  const bool bgWas = (GpsWardriver_s_bgTask != NULL);

  if (!bgWas && !isSDCardAvailable()) {
    display_fill_screen(&(tft), FEATURE_BG);
    drawStatusBar(readBatteryVoltage(), true, true);
    GpsWardriver_wardLogClear();
    GpsWardriver_wardLogPush("Wardrive");
    GpsWardriver_wardLogPush("SD required");
    if (GpsWardriver_wardWaitStartupFailureDismiss(
            "Wardriver", "SD card required. Insert card and try again.")) {
      GpsWardriver_s_wardRetrySession = true;
    }
    return;
  }

  // Clear menu leftovers immediately on entry, before UART/SD setup can delay
  // first paint.
  display_fill_screen(&(tft), FEATURE_BG);
  drawStatusBar(readBatteryVoltage(), true, true);
  GpsWardriver_wardLogClear();
  GpsWardriver_wardLogPush("Wardrive");
  GpsWardriver_wardLogPush("WiFi + BLE logging");

  GpsWardriver_WardPage page = GpsWardriver_WardPage_Main;
  bool fgLog = true;
  uint32_t linesWritten = 0;
  uint32_t scanCount = 0;
  uint32_t lastScanMs = 0;
  uint32_t lastUiMs = 0;
  char path[48] = "";

  storage_file_t logf __attribute__((cleanup(storage_file_close))) = {0};
  bool fgFile = false;

  if (!bgWas) {
    gnss_parser_reset(&gps);

    gnss_receiver_close(&gps_receiver);
    gnss_receiver_open(&gps_receiver, &gps, GPS_UART_NUM, GPS_UART_BAUD,
                       GPS_UART_RX);
    network_set_mode(WIFI_MODE_STA);
    network_disconnect_options(false, false);
    sys_sleep_ms(80);

    storage_file_replace(&(logf),
                         GpsWardriver_wardOpenLogFile(path, sizeof(path)));
    if (!storage_file_valid(&(logf))) {
      if (GpsWardriver_wardWaitStartupFailureDismiss(
              "Wardriver", "Could not create log file on SD.")) {
        GpsWardriver_s_wardRetrySession = true;
      }
      gnss_receiver_close(&gps_receiver);
      return;
    }
    fgFile = true;
    storage_file_print_line_cstr(
        &(logf),
        "epoch_ms,utc,date,lat,lon,alt_m,fix,sats,hdop,radio,ssid,bssid,ch,"
        "rssi_dbm,auth");
    storage_file_flush(&(logf));
    if (!GpsWardriver_wardStartForegroundScanTask(&(logf))) {
      if (GpsWardriver_wardWaitStartupFailureDismiss(
              "Wardriver", "Could not start scan worker.")) {
        GpsWardriver_s_wardRetrySession = true;
      }
      storage_file_close(&(logf));
      gnss_receiver_close(&gps_receiver);
      return;
    }
    GpsWardriver_wardLogPushf("CSV %s", path);
  }

  GpsWardriver_s_editScanMs = GpsWardriver_s_cfgScanMs;
  GpsWardriver_s_editMaxAps = GpsWardriver_s_cfgMaxAps;
  GpsWardriver_s_editRadioMode =
      (GpsWardriver_WardRadioMode)GpsWardriver_s_cfgRadioMode;

  GpsWardriver_wardUiInvalidateAll();
  const bool initBgOn = (GpsWardriver_s_bgTask != NULL);
  GpsWardriver_wardSetNavLabels(page, initBgOn, fgLog);
  uint32_t lastBarMs = sys_monotonic_ms();
  lastUiMs = lastBarMs - (uint32_t)WARDRIVE_UI_INTERVAL_MS;

  GpsWardriver_s_wardFgSessionForStatusBar = true;
  drawStatusBar(readBatteryVoltage(), true, true);

  while (true) {
    gWardSuppressBottomTouchExit = (page != GpsWardriver_WardPage_Main);
    GpsWardriver_wardPumpNavChrome();
    if (GpsWardriver_wardSessionShouldExit()) {
      break;
    }
    if (!bgWas) {
      gnss_receiver_poll(&gps_receiver);
      const bool nf = (gps.state.fix_quality > 0) &&
                      !isnan(gps.state.latitude) && !isnan(gps.state.longitude);
      if (GpsWardriver_s_wardGpsFixLog < 0) {
        GpsWardriver_s_wardGpsFixLog = (int8_t)(nf ? 1 : 0);
      } else {
        const int8_t pf = GpsWardriver_s_wardGpsFixLog;
        const int8_t nowSt = (int8_t)(nf ? 1 : 0);
        if (nowSt != pf) {
          GpsWardriver_s_wardGpsFixLog = nowSt;
          GpsWardriver_wardLogPush(nf ? "GPS: fix" : "GPS: lost");
        }
      }
    }

    const uint32_t now = sys_monotonic_ms();

    if (isNotificationVisible()) {
      int nx = 0, ny = 0;
      if (readTouchXYDismiss(&(nx), &(ny))) {
        GpsWardriver_s_lastWardInputMs = sys_monotonic_ms();
        const NotificationAction na = notificationHandleTouch(nx, ny);
        if (na != NotificationAction_None) {
          GpsWardriver_wardUiInvalidateAll();
          if (page == GpsWardriver_WardPage_WigleCfg) {
            GpsWardriver_s_wigleCfgDirty = true;
          }
          lastUiMs = now - (uint32_t)WARDRIVE_UI_INTERVAL_MS;
        }
        sys_sleep_ms(4);
        continue;
      }
    }

    int hit = -1;
    const GpsWardriver_WardNavEvt navEvt =
        GpsWardriver_wardPollNavButtons(page);
    if (navEvt != GpsWardriver_WardNavEvt_None) {
      lastUiMs = now - (uint32_t)WARDRIVE_UI_INTERVAL_MS;
      GpsWardriver_s_lastWardInputMs = sys_monotonic_ms();
      if (page == GpsWardriver_WardPage_Main) {
        if (navEvt == GpsWardriver_WardNavEvt_Setup) {
          hit = 0;
        } else if (navEvt == GpsWardriver_WardNavEvt_LogToggle) {
          hit = 1;
        } else if (navEvt == GpsWardriver_WardNavEvt_Rss) {
          hit = 2;
        } else if (navEvt == GpsWardriver_WardNavEvt_Exit) {
          hit = 3;
        }
      } else if (navEvt == GpsWardriver_WardNavEvt_Back) {
        hit = 0;
      } else if (navEvt == GpsWardriver_WardNavEvt_Save) {
        hit = 3;
      } else if (page == GpsWardriver_WardPage_Settings &&
                 navEvt == GpsWardriver_WardNavEvt_Upload) {
        hit = 17;
      } else if (page == GpsWardriver_WardPage_Settings &&
                 navEvt == GpsWardriver_WardNavEvt_WigleCfg) {
        hit = 18;
      }
    }

    if (hit < 0) {
      if (page == GpsWardriver_WardPage_Settings) {
        hit = GpsWardriver_wardPollSettingsCombinedHit();
      } else if (page == GpsWardriver_WardPage_WigleCfg) {
        hit = GpsWardriver_wardPollWigleCfgCombinedHit();
      } else {
        hit = GpsWardriver_wardPollToolbarHit();
      }
    }
    if (hit >= 0) {
      lastUiMs = now - (uint32_t)WARDRIVE_UI_INTERVAL_MS;
      const bool bgOn = (GpsWardriver_s_bgTask != NULL);
      const bool logOn = bgOn ? (bool)GpsWardriver_s_bgLog : fgLog;
      if (page == GpsWardriver_WardPage_WigleCfg) {
        if (hit >= 10) {
          const int bh = hit - 10;
          if (bh == 0) {
            GpsWardriver_wardWigleRunFieldEditor(
                "API name", GpsWardriver_s_wigUser,
                sizeof(GpsWardriver_s_wigUser), 63, true);
          } else if (bh == 1) {
            GpsWardriver_wardWigleRunFieldEditor(
                "API token", GpsWardriver_s_wigTok,
                sizeof(GpsWardriver_s_wigTok), 127, true);
          } else if (bh == 2) {
            if (GpsWardriver_s_wigUseSavedWifi) {
              GpsWardriver_wardNotify(
                  "WiGLE",
                  "Turn Auto Wi-Fi off (tap bottom row) to edit SSID.");
            } else {
              GpsWardriver_wardWigleRunFieldEditor(
                  "Wi-Fi SSID", GpsWardriver_s_wigSsid,
                  sizeof(GpsWardriver_s_wigSsid), 31, false);
            }
          } else if (bh == 3) {
            if (GpsWardriver_s_wigUseSavedWifi) {
              GpsWardriver_wardNotify("WiGLE",
                                      "Turn Auto Wi-Fi off to edit password.");
            } else {
              GpsWardriver_wardWigleRunFieldEditor(
                  "Wi-Fi password", GpsWardriver_s_wigPass,
                  sizeof(GpsWardriver_s_wigPass), 63, false);
            }
          } else if (bh == 4) {
            GpsWardriver_s_wigUseSavedWifi = !GpsWardriver_s_wigUseSavedWifi;
            if (GpsWardriver_s_wigUseSavedWifi) {
              GpsWardriver_s_wigSsid[0] = '\0';
              GpsWardriver_s_wigPass[0] = '\0';
            }
            GpsWardriver_wardLogPush(GpsWardriver_s_wigUseSavedWifi
                                         ? "WiGLE: auto Wi-Fi ON"
                                         : "WiGLE: auto Wi-Fi OFF");
            GpsWardriver_s_wigleCfgDirty = true;
          }
        } else if (hit == 0) {
          GpsWardriver_wardLogPush("WiGLE cfg: back");
          page = GpsWardriver_WardPage_Settings;
          GpsWardriver_wardSetNavLabels(page, bgOn, logOn);
          GpsWardriver_wardUiInvalidateAll();
        } else if (hit == 3) {
          GpsWardriver_wardWigleSaveCfgToSd();
          GpsWardriver_wardLogPush("WiGLE cfg: saved");
          page = GpsWardriver_WardPage_Settings;
          GpsWardriver_wardSetNavLabels(page, bgOn, logOn);
          GpsWardriver_wardUiInvalidateAll();
        }
      } else if (page == GpsWardriver_WardPage_Settings) {
        if (hit >= 10) {
          const int bodyHit = hit - 10;
          if (bodyHit == 0) {
            GpsWardriver_s_editRadioMode = GpsWardriver_WardRadioMode_WiFi;
            GpsWardriver_wardLogPush("Pick: WiFi");
          } else if (bodyHit == 1) {
            GpsWardriver_s_editRadioMode = GpsWardriver_WardRadioMode_Ble;
            GpsWardriver_wardLogPush("Pick: BLE");
          } else if (bodyHit == 2) {
            GpsWardriver_s_editRadioMode = GpsWardriver_WardRadioMode_Both;
            GpsWardriver_wardLogPush("Pick: WiFi+BLE");
          } else if (bodyHit == 3) {
            if (GpsWardriver_s_editScanMs > 2500u) {
              GpsWardriver_s_editScanMs -= 500u;
              GpsWardriver_wardLogPushf(
                  "Scan %lums", (unsigned long)GpsWardriver_s_editScanMs);
            }
          } else if (bodyHit == 4) {
            if (GpsWardriver_s_editScanMs < 20000u) {
              GpsWardriver_s_editScanMs += 500u;
              GpsWardriver_wardLogPushf(
                  "Scan %lums", (unsigned long)GpsWardriver_s_editScanMs);
            }
          } else if (bodyHit == 5) {
            if (GpsWardriver_s_editMaxAps > 8) {
              GpsWardriver_s_editMaxAps -= 4;
              GpsWardriver_wardLogPushf("Cap %d", GpsWardriver_s_editMaxAps);
            }
          } else if (bodyHit == 6) {
            if (GpsWardriver_s_editMaxAps < 64) {
              GpsWardriver_s_editMaxAps += 4;
              GpsWardriver_wardLogPushf("Cap %d", GpsWardriver_s_editMaxAps);
            }
          } else if (bodyHit == 7) {
            if (GpsWardriver_s_bgTask) {
              GpsWardriver_wardNotify("WiGLE",
                                      "Stop background wardriver first.");
            } else if (!path[0]) {
              GpsWardriver_wardNotify("WiGLE", "No CSV (foreground log only).");
            } else {
              GpsWardriver_wardLogPush("WiGLE: upload tap");
              if (fgFile) {
                storage_file_flush(&(logf));
              }
              GpsWardriver_wardPerformWigleUpload(path, fgFile ? &logf : NULL);
              GpsWardriver_wardUiInvalidateAll();
            }
          } else if (bodyHit == 8) {
            if (!isSDCardAvailable()) {
              GpsWardriver_wardNotify("WiGLE", "SD required for WiGLE config.");
            } else {
              GpsWardriver_wardWigleLoadEditBuffers();
              page = GpsWardriver_WardPage_WigleCfg;
              GpsWardriver_wardSetNavLabels(page, bgOn, logOn);
              GpsWardriver_s_wigleCfgDirty = true;
              GpsWardriver_wardUiInvalidateAll();
              GpsWardriver_wardLogPush("WiGLE editor");
            }
          }
        } else if (hit == 17) {
          if (GpsWardriver_s_bgTask) {
            GpsWardriver_wardNotify("WiGLE",
                                    "Stop background wardriver first.");
          } else if (!path[0]) {
            GpsWardriver_wardNotify("WiGLE", "No CSV (foreground log only).");
          } else {
            GpsWardriver_wardLogPush("WiGLE: upload tap");
            if (fgFile) {
              storage_file_flush(&(logf));
            }
            GpsWardriver_wardPerformWigleUpload(path, fgFile ? &logf : NULL);
            GpsWardriver_wardUiInvalidateAll();
          }
        } else if (hit == 18) {
          if (!isSDCardAvailable()) {
            GpsWardriver_wardNotify("WiGLE", "SD required for WiGLE config.");
          } else {
            GpsWardriver_wardWigleLoadEditBuffers();
            page = GpsWardriver_WardPage_WigleCfg;
            GpsWardriver_wardSetNavLabels(page, bgOn, logOn);
            GpsWardriver_s_wigleCfgDirty = true;
            GpsWardriver_wardUiInvalidateAll();
            GpsWardriver_wardLogPush("WiGLE editor");
          }
        } else if (hit == 0) {
          GpsWardriver_wardLogPush("Setup: back");
          page = GpsWardriver_WardPage_Main;
          GpsWardriver_wardSetNavLabels(page, bgOn, logOn);
          GpsWardriver_wardUiInvalidateAll();
        } else if (hit == 3) {
          GpsWardriver_wardLogPush("Setup: saved");
          GpsWardriver_s_cfgScanMs = GpsWardriver_s_editScanMs;
          GpsWardriver_s_cfgMaxAps = GpsWardriver_s_editMaxAps;
          GpsWardriver_s_cfgRadioMode = (uint8_t)GpsWardriver_s_editRadioMode;
          GpsWardriver_wardLogPushf(
              "Cfg %lums %s cap%d", (unsigned long)GpsWardriver_s_cfgScanMs,
              GpsWardriver_wardRadioModeShort(
                  (GpsWardriver_WardRadioMode)GpsWardriver_s_cfgRadioMode),
              (int)GpsWardriver_s_cfgMaxAps);
          page = GpsWardriver_WardPage_Main;
          GpsWardriver_wardSetNavLabels(page, bgOn, logOn);
          GpsWardriver_wardUiInvalidateAll();
          drainButtons();
        }
      } else {
        if (hit == 0) {
          GpsWardriver_wardLogPush("Open setup");
          GpsWardriver_s_editScanMs = GpsWardriver_s_cfgScanMs;
          GpsWardriver_s_editMaxAps = GpsWardriver_s_cfgMaxAps;
          GpsWardriver_s_editRadioMode =
              (GpsWardriver_WardRadioMode)GpsWardriver_s_cfgRadioMode;
          page = GpsWardriver_WardPage_Settings;
          GpsWardriver_wardSetNavLabels(page, bgOn, logOn);
          GpsWardriver_wardUiInvalidateAll();
        } else if (hit == 1) {
          if (GpsWardriver_s_bgTask) {
            GpsWardriver_s_bgLog = !GpsWardriver_s_bgLog;
            GpsWardriver_wardLogPush(GpsWardriver_s_bgLog ? "BG log ON"
                                                          : "BG log OFF");
          } else {
            fgLog = !fgLog;
            GpsWardriver_s_fgScanPaused = !fgLog;
            GpsWardriver_wardLogPush(fgLog ? "Log ON" : "Log OFF");
          }
          GpsWardriver_wardSetNavLabels(
              page, bgOn,
              GpsWardriver_s_bgTask ? (bool)GpsWardriver_s_bgLog : fgLog);
        } else if (hit == 2) {
          if (GpsWardriver_s_bgTask) {
            GpsWardriver_wardLogPush("Stop background");
            GpsWardriver_s_bgStop = true;
            for (int w = 0; w < 200 && GpsWardriver_s_bgTask; w++) {
              sys_sleep_ms(15);
            }
            GpsWardriver_s_bgStop = false;
            GpsWardriver_wardSetNavLabels(page, false, fgLog);
          } else {
            if (!fgLog) {
              GpsWardriver_wardNotify(
                  "Wardriver",
                  "Turn logging on (play), then tap RSS for Auto.");
            } else {
              GpsWardriver_wardLogPush("Handoff to BG");
              GpsWardriver_wardStopForegroundScanTask();
              storage_file_flush(&(logf));
              storage_file_close(&(logf));
              fgFile = false;
              network_clear_scan();
              gnss_receiver_close(&gps_receiver);
              GpsWardriver_wardStartBackground();
              GpsWardriver_s_wardFgSessionForStatusBar = false;
              drawStatusBar(readBatteryVoltage(), true, false);
              drainButtons();
              gWardSuppressBottomTouchExit = false;
              return;
            }
          }
        } else if (hit == 3) {
          GpsWardriver_wardLogPush("Exit");
          feature_exit_requested = true;
        }
      }
    }

    if ((uint32_t)(now - lastUiMs) >= (uint32_t)WARDRIVE_UI_INTERVAL_MS) {
      lastUiMs = now;
      if (lastBarMs == 0 || (uint32_t)(now - lastBarMs) >= 1000u) {
        lastBarMs = now;
        drawStatusBar(readBatteryVoltage(), false, true);
      }
      const bool bgOn = (GpsWardriver_s_bgTask != NULL);
      const bool logOn = bgOn ? (bool)GpsWardriver_s_bgLog : fgLog;
      bool wardRepaintedBody = false;
      if (GpsWardriver_wardTbDirty(page, bgOn, logOn)) {
        GpsWardriver_wardDrawIconToolbar(page, bgOn, logOn);
        GpsWardriver_wardSetNavLabels(page, bgOn, logOn);
        GpsWardriver_wardTbSave(page, bgOn, logOn);
      }
      if (page == GpsWardriver_WardPage_WigleCfg) {
        if (GpsWardriver_s_wigleCfgDirty) {
          GpsWardriver_wardDrawWigleCfgPage();
          GpsWardriver_s_wigleCfgDirty = false;
          wardRepaintedBody = true;
        }
      } else if (page == GpsWardriver_WardPage_Settings) {
        if (GpsWardriver_wardSetDirty()) {
          GpsWardriver_wardDrawSettingsPage();
          GpsWardriver_wardSetSave();
          wardRepaintedBody = true;
        }
      } else {
        const bool bg = bgOn;
        const uint32_t dispL =
            bg ? GpsWardriver_s_bgLines : GpsWardriver_s_fgLines;
        const uint32_t dispS =
            bg ? GpsWardriver_s_bgScans : GpsWardriver_s_fgScans;
        const char* dispP = bg ? GpsWardriver_s_bgPath : path;
        if (GpsWardriver_wardMainDirty(dispL, dispS, dispP,
                                       bg ? GpsWardriver_s_bgLog : fgLog, bg)) {
          GpsWardriver_wardDrawBody(dispL, dispS, dispP,
                                    bg ? GpsWardriver_s_bgLog : fgLog, bg);
          GpsWardriver_wardMainSave(dispL, dispS, dispP,
                                    bg ? GpsWardriver_s_bgLog : fgLog, bg);
          wardRepaintedBody = true;
        }
      }
      if (wardRepaintedBody && isNotificationVisible()) {
        notificationRedrawIfVisible();
      }
    }

    if (!bgWas) {
      GpsWardriver_s_fgScanPaused =
          !fgLog || page != GpsWardriver_WardPage_Main ||
          (uint32_t)(now - GpsWardriver_s_lastWardInputMs) < 650u;
      linesWritten = GpsWardriver_s_fgLines;
      scanCount = GpsWardriver_s_fgScans;
      lastScanMs = now;
    }

    sys_sleep_ms(4);
  }
  gWardSuppressBottomTouchExit = false;
  GpsWardriver_s_wardFgSessionForStatusBar = false;

  if (!bgWas && fgFile) {
    GpsWardriver_wardStopForegroundScanTask();
    storage_file_flush(&(logf));
    storage_file_close(&(logf));
    network_clear_scan();
    network_disconnect_options(false, false);
    gnss_receiver_close(&gps_receiver);
  }

  feature_exit_requested = false;
  drainButtons();
  drawStatusBar(readBatteryVoltage(), true, false);
}

void GpsWardriver_clearSessionRetry(void) {
  GpsWardriver_s_wardRetrySession = false;
}

bool GpsWardriver_consumeSessionRetry(void) {
  if (!GpsWardriver_s_wardRetrySession) {
    return false;
  }
  GpsWardriver_s_wardRetrySession = false;
  return true;
}
