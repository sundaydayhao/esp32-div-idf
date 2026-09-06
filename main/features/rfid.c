#include "rfid.h"

#include <pn532.h>

#include "console.h"
#include "feature_text.h"
#include "icon.h"
#include "settings_store.h"
#include "shared.h"
#include "stdio.h"
#include "string.h"
#include "touch_ui.h"
#include "ui_utils.h"

#ifndef RFID_UID_CLONE
#define RFID_UID_CLONE 1
#endif

#ifndef RFID_TAG_LISTEN_TIMEOUT_MS
#define RFID_TAG_LISTEN_TIMEOUT_MS 20000
#endif
/** Clone flow waits for source / blank tag (ms). */
#ifndef RFID_CLONE_PHASE_TIMEOUT_MS
#define RFID_CLONE_PHASE_TIMEOUT_MS 45000
#endif
/** Jam / disrupt: wait for external reader to poll target (ms). 0 = until Stop.
 */
#ifndef RFID_TARGET_WAIT_TIMEOUT_MS
#define RFID_TARGET_WAIT_TIMEOUT_MS 120000
#endif

static pn532_t s_nfc = {.sck_ = PN532_SCK,
                        .miso_ = PN532_MISO,
                        .mosi_ = PN532_MOSI,
                        .ss_ = PN532_SS,
                        .inListedTag_ = 1};
static bool s_hwOk = false;
static char s_pn532VerStr[28] = "";
static bool s_rfidRetrySession = false;
static FeatureUI_Button s_rfFoot[2];

/* NullTag-style card family */
typedef enum CardType {
  UNKNOWN,
  MIFARE_CLASSIC,
  MIFARE_ULTRALIGHT,
  NTAG,
  MIFARE_DESFIRE
} CardType;

typedef enum RfidUiEvt {
  RfidUiEvt_None,
  RfidUiEvt_Back,
  RfidUiEvt_Primary
} RfidUiEvt;

static void rfidAttachBus(void) {
  if (s_hwOk) {
    pn532_begin(&(s_nfc));
    pn532_sam_config(&(s_nfc));
    pn532_set_passive_activation_retries(&(s_nfc), 0xFF);
  }
}

static void rfidPn532VerStr(uint32_t ver) {
  if (!ver) {
    s_pn532VerStr[0] = '\0';
    return;
  }
  snprintf(s_pn532VerStr, sizeof(s_pn532VerStr), "PN532 0x%06lX",
           (unsigned long)ver);
}

static void rfidReleaseNavButtons(void) {
  sys_sleep_ms(12);
  for (int i = 0; i < 80; i++) {
    if (!isButtonPressed(BTN_SELECT) && !isButtonPressed(BTN_LEFT) &&
        !isButtonPressed(BTN_RIGHT) && !isButtonPressed(BTN_UP) &&
        !isButtonPressed(BTN_DOWN)) {
      break;
    }
    sys_sleep_ms(5);
  }
}

static int rfidScreenH(void) { return display_height(&(tft)); }
enum { kRfidToolbarY = 20 };
enum { kRfidToolbarH = 16 };
enum { kRfidToolbarBottom = kRfidToolbarY + kRfidToolbarH };
enum { kRfidIconBackX = 10 };
enum { kRfidIconSize = 16 };
static bool s_rfidUiDrawn = false;

static int rfidContentBottom(void) {
  return featureHasTouchNavBar() ? touchNavContentBottomY() : rfidScreenH();
}

static void rfidClearBody(uint16_t color) {
  if (featureHasTouchNavBar()) {
    featureClearContent(color);
  } else {
    display_fill_screen(&(tft), color);
  }
}

static void rfidRedrawNavChrome(void) {
  redrawTouchButtonBar();
  maintainTouchNavBar();
}

static bool rfidSessionExitRequested(void) { return feature_exit_requested; }

static void rfidPollToolbar(void) {
  if (!feature_active) {
    return;
  }
  static unsigned long s_lastToolbarCheck = 0;
  if (sys_monotonic_ms() - s_lastToolbarCheck < 50) {
    return;
  }
  s_lastToolbarCheck = sys_monotonic_ms();
  int x = 0;
  int y = 0;
  if (!readTouchXY(&(x), &(y))) {
    return;
  }
  if (y > kRfidToolbarY && y < kRfidToolbarBottom && x > kRfidIconBackX &&
      x < (kRfidIconBackX + kRfidIconSize)) {
    feature_exit_requested = true;
  }
}

static bool rfidPumpSessionUi(void) {
  if (rfidSessionExitRequested()) {
    return true;
  }
  maintainTouchNavBar();
  rfidPollToolbar();
  if (s_rfidUiDrawn) {
    display_draw_fast_h_line(&(tft), 0, kRfidToolbarY, TFT_WIDTH, UI_LINE);
    display_draw_fast_h_line(&(tft), 0, kRfidToolbarBottom, TFT_WIDTH, UI_LINE);
  }
  return false;
}

static RfidUiEvt rfidPollFooter(FeatureUI_Button* btns, int n,
                                bool mapSelectToPrimary) {
  if (isButtonPressed(BTN_LEFT)) {
    rfidReleaseNavButtons();
    return RfidUiEvt_Back;
  }
  if (n == 1 && isButtonPressed(BTN_SELECT)) {
    rfidReleaseNavButtons();
    return RfidUiEvt_Back;
  }
  if (mapSelectToPrimary && n >= 2 && isButtonPressed(BTN_SELECT)) {
    if (!btns[1].disabled) {
      rfidReleaseNavButtons();
      return RfidUiEvt_Primary;
    }
  }
  if (featureHasTouchNavBar()) {
    if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
      rfidReleaseNavButtons();
      return RfidUiEvt_Back;
    }
    if (isTouchNavButtonPressedEdge(BTN_SELECT)) {
      if (mapSelectToPrimary && n >= 2 && !btns[1].disabled) {
        rfidReleaseNavButtons();
        return RfidUiEvt_Primary;
      }
      rfidReleaseNavButtons();
      return RfidUiEvt_Back;
    }
    return RfidUiEvt_None;
  }
  int x = 0, y = 0;
  if (!readTouchXYDismiss(&(x), &(y)) && !readTouchXY(&(x), &(y))) {
    return RfidUiEvt_None;
  }
  int h = FeatureUI_hit(btns, n, x, y);
  if (h < 0) {
    return RfidUiEvt_None;
  }
  if (btns[h].disabled) {
    return RfidUiEvt_None;
  }
  rfidReleaseNavButtons();
  if (h == 0) {
    return RfidUiEvt_Back;
  }
  return RfidUiEvt_Primary;
}

enum { RF_PAD_X = 10 };
enum { RF_RESULT_LINE_PX = 17 };
enum { RF_BODY_LINE_PX = 18 };
enum { RF_TAG_H = 14 };
enum { RF_TAG_GAP = 4 };

enum { RF_DYN_TOP_MAIN = 0 };
enum { RF_DYN_TOP_PROG = 0 };

/** IR Record / 2.4GHz Scanner rhythm */
enum { kRfidToolbarGap = 8 };
enum { kRfidBoxHeaderH = 15 };
enum { kRfidLineHeight = 12 };
enum { kRfidStatusY = kRfidToolbarBottom + kRfidToolbarGap };
enum { kRfidStatusBoxH = 91 };
enum { kRfidLogGap = 4 };
enum { kRfidLogBoxH = 49 };
enum { kRfidLogBoxTop = kRfidStatusY + kRfidStatusBoxH + kRfidLogGap };
enum { kRfidLogStartY = kRfidLogBoxTop + kRfidBoxHeaderH };
enum { kRfidLogEndY = kRfidLogBoxTop + kRfidLogBoxH - 2 };
enum { kRfidDetailTop = kRfidLogBoxTop + kRfidLogBoxH + 4 };
enum { kRfidStatusLineCount = 6 };
enum { kRfidStatusTextY = kRfidStatusY + kRfidBoxHeaderH };
enum { kRfidMaxLogLines = 48 };
enum { kRfidDetailPadX = RF_PAD_X };
enum { kRfidDetailInnerW = 220 };
enum { kRfidDetailHeaderH = 18 };

static int rfidHintY(void) {
  if (featureHasTouchNavBar()) {
    return rfidContentBottom() - 16;
  }
  return TFT_HEIGHT - FeatureUI_FOOTER_H - 16;
}

static int rfidDetailBottom(void) {
  if (featureHasTouchNavBar()) {
    return rfidContentBottom() - 8;
  }
  return rfidHintY() - 8;
}

static int rfidEffectiveDetailTop(void) {
  const int bottom = rfidDetailBottom();
  enum { minDetailH = 80 };
  int top = kRfidDetailTop;
  if (bottom - top < minDetailH) {
    top = bottom - minDetailH;
    const int logBottom = kRfidLogBoxTop + kRfidLogBoxH;
    if (top < logBottom + 2) {
      top = logBottom + 2;
    }
  }
  return top;
}

static int rfidDetailBodyTop(void) {
  return rfidEffectiveDetailTop() + kRfidBoxHeaderH + 2;
}

static int rfidDetailLinesPerPage(void) {
  const int bodyBottom = rfidDetailBottom() - 4;
  const int n = (bodyBottom - rfidDetailBodyTop()) / kRfidLineHeight;
  return n < 1 ? 1 : n;
}

static bool s_rfidBoxesDrawn = false;
static bool s_rfidStatusStaticDrawn = false;
static bool s_rfidDetailBoxVisible = false;
static char s_featureInfoTitle[32];
static char s_featureInfoBody[640];
static bool s_featureInfoAvailable = false;
static char s_listenCenterLabel[24];
enum { s_rfidLogBuffer_COUNT = kRfidMaxLogLines, s_rfidLogBuffer_OFFSET = 0 };
_Static_assert(s_rfidLogBuffer_OFFSET + s_rfidLogBuffer_COUNT <=
                   FEATURE_TEXT_SLOTS,
               "Feature text capacity");
#define s_rfidLogBuffer                  \
  (*(text_t(*)[s_rfidLogBuffer_COUNT]) & \
   feature_text_slots[s_rfidLogBuffer_OFFSET])
static uint16_t s_rfidLogColor[kRfidMaxLogLines];
static int s_rfidLogIndex = 0;
enum {
  s_rfidStatusLineText_COUNT = kRfidStatusLineCount,
  s_rfidStatusLineText_OFFSET = s_rfidLogBuffer_OFFSET + s_rfidLogBuffer_COUNT
};
_Static_assert(s_rfidStatusLineText_OFFSET + s_rfidStatusLineText_COUNT <=
                   FEATURE_TEXT_SLOTS,
               "Feature text capacity");
#define s_rfidStatusLineText                  \
  (*(text_t(*)[s_rfidStatusLineText_COUNT]) & \
   feature_text_slots[s_rfidStatusLineText_OFFSET])
static uint16_t s_rfidStatusLineColor[kRfidStatusLineCount];

static void rfidPrintWrappedPageBg(int x, int y, int maxWidth, const char* text,
                                   int lineStepPx, uint16_t fg, uint16_t bg,
                                   int firstLine, int maxLines);
static int rfidCountWrappedLines(int maxWidth, const char* text);
static bool rfidDetailTagNeedsPill(const char* tagLabel);

enum { kRfidInfoHeaderH = 18 };
enum { kRfidInfoTitleH = 15 };
enum { kRfidInfoPanelPad = 8 };
enum { kRfidInfoLinePx = 16 };
/** Match Activity / log box chrome (drawRoundRect at x=4, w=TFT_WIDTH-8). */
enum { kRfidInfoBoxX = 4 };
enum { kRfidInfoBoxW = TFT_WIDTH - 8 };
enum { kRfidInfoInnerX = 6 };
enum { kRfidInfoInnerW = TFT_WIDTH - 12 };
enum { kRfidInfoTextW = TFT_WIDTH - RF_PAD_X * 2 };

static int rfidInfoPanelPageCountForLines(const char* body, int linesPerPage) {
  int total = rfidCountWrappedLines(kRfidInfoTextW, body);
  int pageCount = (total + linesPerPage - 1) / linesPerPage;
  return pageCount < 1 ? 1 : pageCount;
}

static int rfidDetailOverlayTop(void) { return kRfidStatusY; }

static int rfidInfoContentTop(bool withFeatureTitle) {
  int y = rfidDetailOverlayTop() + kRfidInfoHeaderH + 1 + 5;
  if (withFeatureTitle) {
    y += kRfidInfoTitleH + 4;
  }
  return y + kRfidInfoPanelPad;
}

static int rfidInfoContentBottom(void) { return rfidDetailBottom() - 6; }

static int rfidInfoOverlayLinesPerPage(bool withFeatureTitle) {
  const int panelTop = rfidInfoContentTop(withFeatureTitle) - kRfidInfoPanelPad;
  const int panelH = rfidInfoContentBottom() - panelTop;
  const int innerH = panelH - kRfidInfoPanelPad * 2;
  const int n = innerH / kRfidInfoLinePx;
  return n < 1 ? 1 : n;
}

static void rfidDrawInfoPill(int x, int y, int w, const char* label) {
  if (w < 28) {
    w = 28;
  }
  display_fill_round_rect(&(tft), x, y, w, RF_TAG_H, 7, DARK_GRAY);
  display_set_text_color_2(&(tft), UI_ICON, DARK_GRAY);
  display_set_cursor(&(tft), x + 6, y + 3);
  display_print(&(tft), (TEXT(label)).data);
}

static text_t rfidFitInfoTitle(const char* title) {
  if (!title || !title[0]) {
    return (text_t){0};
  }
  text_t t = TEXT(title);
  const int maxW = TFT_WIDTH - RF_PAD_X * 2;
  const bool needsEllipsis = display_text_width(&(tft), (t).data, 0) > maxW;
  while (strlen((t).data) > 0 &&
         display_text_width(
             &(tft),
             ((needsEllipsis ? text_join_cstr((t).data, "...") : t)).data,
             0) > maxW) {
    text_erase(&(t), strlen((t).data) - 1, SIZE_MAX);
  }
  if (needsEllipsis && strlen((t).data) > 0) {
    text_append_cstr(&(t), "...");
  }
  return t;
}

static void rfidDrawInfoOverlay(const char* featureTitle, const char* body,
                                int page, int pageCount, int linesPerPage) {
  const int top = rfidDetailOverlayTop();
  const int bottom = rfidDetailBottom();
  const bool showTitle = featureTitle && featureTitle[0];

  display_fill_rect(&(tft), 0, top - 2, TFT_WIDTH, bottom - top + 2, TFT_BLACK);
  display_draw_fast_h_line(&(tft), 0, 19, TFT_WIDTH, UI_LINE);

  display_set_text_font(&(tft), 1);
  display_set_text_size(&(tft), 1);

  const int headerY = top + 3;
  display_set_text_color_2(&(tft), UI_TEXT, TFT_BLACK);
  display_draw_string(&(tft), "Info", RF_PAD_X, headerY, 0);

  if (pageCount > 1) {
    text_t pageLabel =
        text_join_cstr((text_join_cstr((TEXT(page + 1)).data, "/")).data,
                       (TEXT(pageCount)).data);
    const int pw = (int)display_text_width(&(tft), (pageLabel).data, 0) + 14;
    rfidDrawInfoPill(TFT_WIDTH - RF_PAD_X - pw, headerY, pw, (pageLabel).data);
  }

  int y = top + kRfidInfoHeaderH;
  display_draw_fast_h_line(&(tft), kRfidInfoBoxX, y, kRfidInfoBoxW, UI_LINE);
  y += 5;

  if (showTitle) {
    display_set_text_color_2(&(tft), UI_TEXT, TFT_BLACK);
    display_draw_string(&(tft), (rfidFitInfoTitle(featureTitle)).data, RF_PAD_X,
                        y, 0);
    y += kRfidInfoTitleH;
    display_draw_fast_h_line(&(tft), kRfidInfoBoxX, y - 2, kRfidInfoBoxW,
                             UI_LINE);
    y += 4;
  }

  const int panelY = y;
  const int panelH = bottom - panelY - 4;
  if (panelH > 8) {
    display_draw_round_rect(&(tft), kRfidInfoBoxX, panelY, kRfidInfoBoxW,
                            panelH, 3, UI_LINE);
    if (panelH > 4) {
      display_fill_rect(&(tft), kRfidInfoInnerX, panelY + 2, kRfidInfoInnerW,
                        panelH - 4, DARK_GRAY);
    }
  }

  const int textX = RF_PAD_X;
  const int textY = panelY + kRfidInfoPanelPad;
  const int textBottom = panelY + panelH - kRfidInfoPanelPad;
  const int maxLinesInPanel = (textBottom - textY) / kRfidInfoLinePx;
  const int linesToDraw =
      (linesPerPage < maxLinesInPanel) ? linesPerPage : maxLinesInPanel;

  rfidPrintWrappedPageBg(textX, textY, kRfidInfoTextW, body, kRfidInfoLinePx,
                         UI_TEXT, DARK_GRAY, page * linesPerPage, linesToDraw);

  if (pageCount > 1 && panelH > 20) {
    display_set_text_color_2(&(tft), UI_DIM_TEXT, DARK_GRAY);
    if (page > 0) {
      display_set_cursor(&(tft), kRfidInfoInnerX + 4, panelY + panelH - 12);
      display_print(&(tft), "< Prev");
    }
    if (page < pageCount - 1) {
      const char* nextHint = "Next >";
      const int nw = display_text_width(&(tft), nextHint, 0);
      display_set_cursor(&(tft), kRfidInfoInnerX + kRfidInfoInnerW - nw - 4,
                         panelY + panelH - 12);
      display_print(&(tft), (TEXT(nextHint)).data);
    }
  }
}

static void rfidDynamicBand(int topY, const char* line1, const char* line2);
static void rfidTryShowInfo(void);
static void rfidWaitTwoBoxDismiss(void);
static void rfidRunToolbar(const char* title);
static void rfidDrawTextBoxes(void);
static void rfidResetUiState(void);
static void rfidPrint(text_t text, uint16_t color, bool extraSpace);
static void rfidDrawStatusLineIfChanged(int line, text_t text, uint16_t color);

/** Dim one-line caption for NFC Status (listen/progress); cleared in
 * rfidFeatureSetup. */
static char s_rfidBandAux[72];

static void rfidFeatureSetup(const char* title) {
  applyThemeToPalette((*settings()).theme);
  rfidResetUiState();
  rfidClearBody(TFT_BLACK);
  drawStatusBar(readBatteryVoltage(), true, false);
  s_rfidBandAux[0] = '\0';
  rfidRunToolbar(title);
  rfidDrawTextBoxes();
  rfidRedrawNavChrome();
}

static void rfidSetDialogNavLabels(const char* primaryLabel) {
  setTouchNavLabels("Back", NULL, primaryLabel, NULL, NULL);
  redrawTouchButtonBar();
}

static void rfidSetListenNavLabels(const char* actionLabel, bool withInfo) {
  setTouchNavLabels(NULL, NULL, actionLabel, NULL, withInfo ? "Info" : NULL);
  redrawTouchButtonBar();
}

static void rfidSetInfoOverlayNavLabels(int pageCount) {
  if (pageCount > 1) {
    setTouchNavLabels("Back", "Prev", "Back", "Next", NULL);
  } else {
    setTouchNavLabels("Back", NULL, "Back", NULL, NULL);
  }
  redrawTouchButtonBar();
}

static void rfidSetTimeoutNavLabels(bool withInfo) {
  setTouchNavLabels("Exit", NULL, "Ready", NULL, withInfo ? "Info" : NULL);
  redrawTouchButtonBar();
}

static void rfidDrawShellFooter1(const char* actionLabel) {
  FeatureUI_layoutFooter1(&(s_rfFoot[0]), actionLabel,
                          FeatureUI_ButtonStyle_Secondary, false);
  if (actionLabel && actionLabel[0]) {
    strncpy(s_listenCenterLabel, actionLabel, sizeof(s_listenCenterLabel) - 1);
    s_listenCenterLabel[sizeof(s_listenCenterLabel) - 1] = '\0';
  } else {
    s_listenCenterLabel[0] = '\0';
  }
  if (featureHasTouchNavBar()) {
    rfidSetListenNavLabels(actionLabel, s_featureInfoAvailable);
  } else {
    FeatureUI_drawFooterBg();
    FeatureUI_drawButton(&(s_rfFoot[0]), false);
  }
}

static void rfidDrawShellFooter2(const char* primaryLabel,
                                 FeatureUI_ButtonStyle primaryStyle,
                                 bool primaryDisabled) {
  FeatureUI_layoutFooter2(&(s_rfFoot), "Back", FeatureUI_ButtonStyle_Secondary,
                          primaryLabel, primaryStyle, false, primaryDisabled);
  if (featureHasTouchNavBar()) {
    rfidSetDialogNavLabels(primaryLabel);
  } else {
    FeatureUI_drawFooterBg();
    FeatureUI_drawButton(&(s_rfFoot[0]), false);
    FeatureUI_drawButton(&(s_rfFoot[1]), false);
  }
}

static void rfidSetBandAux(const char* s) {
  if (!s || !s[0]) {
    s_rfidBandAux[0] = '\0';
    return;
  }
  strncpy(s_rfidBandAux, s, sizeof(s_rfidBandAux) - 1);
  s_rfidBandAux[sizeof(s_rfidBandAux) - 1] = '\0';
}

/** IR Record-style toolbar: DARK_GRAY bar, title left, back icon. */
static void rfidRunToolbar(const char* title) {
  if (s_rfidUiDrawn) {
    return;
  }
  display_fill_rect(&(tft), 0, kRfidToolbarY, TFT_WIDTH, kRfidToolbarH,
                    DARK_GRAY);
  display_draw_bitmap(&(tft), kRfidIconBackX, kRfidToolbarY,
                      bitmap_icon_go_back, kRfidIconSize, kRfidIconSize,
                      UI_ICON);
  display_set_text_color_2(&(tft), UI_TEXT, DARK_GRAY);
  display_set_text_size(&(tft), 1);
  if (title && title[0]) {
    enum { titleX = 35 };
    const int maxTitleW = TFT_WIDTH - titleX - 8;
    text_t t = TEXT(title);
    const bool needsEllipsis =
        display_text_width(&(tft), (t).data, 0) > maxTitleW;
    while (strlen((t).data) > 0 &&
           display_text_width(
               &(tft),
               ((needsEllipsis ? text_join_cstr((t).data, "...") : t)).data,
               0) > maxTitleW) {
      text_erase(&(t), strlen((t).data) - 1, SIZE_MAX);
    }
    if (needsEllipsis) {
      text_append_cstr(&(t), "...");
    }
    display_set_cursor(&(tft), titleX, kRfidToolbarY + 4);
    display_print(&(tft), (t).data);
  }
  display_draw_fast_h_line(&(tft), 0, 19, TFT_WIDTH, UI_LINE);
  display_draw_fast_h_line(&(tft), 0, kRfidToolbarBottom, TFT_WIDTH, UI_LINE);
  s_rfidUiDrawn = true;
}

static void rfidUpdateToolbarTitle(const char* title) {
  s_rfidUiDrawn = false;
  rfidRunToolbar(title);
}

static void rfidPrintWrappedStepBg(int x, int y, int maxWidth, const char* text,
                                   int lineStepPx, uint16_t fg, uint16_t bg,
                                   int bottomY) {
  text_t message = TEXT(text ? text : "");
  int cursorY = y;
  bool clipped = false;

  while (strlen((message).data) > 0) {
    if (cursorY > bottomY - lineStepPx) {
      clipped = true;
      break;
    }

    int nl = text_find_index(message, text_character('\n'), 0);
    text_t paragraph = ((nl >= 0) ? text_slice(message, 0, nl) : message);
    message = ((nl >= 0) ? text_slice(message, nl + 1, SIZE_MAX) : TEXT(""));

    if (strlen((paragraph).data) == 0) {
      cursorY += lineStepPx;
      continue;
    }

    while (strlen((paragraph).data) > 0) {
      if (cursorY > bottomY - lineStepPx) {
        clipped = true;
        break;
      }

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

      display_set_text_color_2(&(tft), fg, bg);
      display_set_cursor(&(tft), x, cursorY);
      display_print(&(tft), (text_slice(paragraph, 0, lineEnd)).data);
      paragraph = text_slice(paragraph, lineEnd, SIZE_MAX);
      text_trim(&(paragraph));
      cursorY += lineStepPx;
    }
  }

  if (clipped && bottomY >= y + lineStepPx) {
    display_set_text_color_2(&(tft), UI_DIM_TEXT, bg);
    display_set_cursor(&(tft), x, bottomY - lineStepPx);
    display_print(&(tft), "...");
  }
}

static void rfidPrintWrappedStep(int x, int y, int maxWidth, const char* text,
                                 int lineStepPx) {
  rfidPrintWrappedStepBg(x, y, maxWidth, text, lineStepPx, UI_TEXT, FEATURE_BG,
                         TFT_HEIGHT);
}

static int rfidCountWrappedLines(int maxWidth, const char* text) {
  text_t message = TEXT(text ? text : "");
  int total = 0;
  while (strlen((message).data) > 0) {
    int nl = text_find_index(message, text_character('\n'), 0);
    text_t paragraph = ((nl >= 0) ? text_slice(message, 0, nl) : message);
    message = ((nl >= 0) ? text_slice(message, nl + 1, SIZE_MAX) : TEXT(""));

    if (strlen((paragraph).data) == 0) {
      total++;
      continue;
    }

    while (strlen((paragraph).data) > 0) {
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
      paragraph = text_slice(paragraph, lineEnd, SIZE_MAX);
      text_trim(&(paragraph));
      total++;
    }
  }
  return total > 0 ? total : 1;
}

static void rfidPrintWrappedPageBg(int x, int y, int maxWidth, const char* text,
                                   int lineStepPx, uint16_t fg, uint16_t bg,
                                   int firstLine, int maxLines) {
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

static text_t rfidFitStatusText(text_t text) {
  const int maxWidth = TFT_WIDTH - 16;
  display_set_text_size(&(tft), 1);
  if (display_text_width(&(tft), (text).data, 0) <= maxWidth) {
    return text;
  }
  text_t out = text;
  while (strlen((out).data) > 1 &&
         display_text_width(&(tft), (text_join_cstr((out).data, "...")).data,
                            0) > maxWidth) {
    text_erase(&(out), strlen((out).data) - 1, SIZE_MAX);
  }
  if (strlen((out).data) > 0) {
    text_append_cstr(&(out), "...");
  }
  return out;
}

enum { kRfidLogTextX = 10 };
enum { kRfidLogTextW = TFT_WIDTH - kRfidLogTextX * 2 };

static text_t rfidFitLogText(text_t text) {
  display_set_text_size(&(tft), 1);
  if (display_text_width(&(tft), (text).data, 0) <= kRfidLogTextW) {
    return text;
  }
  text_t out = text;
  while (strlen((out).data) > 1 &&
         display_text_width(&(tft), (text_join_cstr((out).data, "...")).data,
                            0) > kRfidLogTextW) {
    text_erase(&(out), strlen((out).data) - 1, SIZE_MAX);
  }
  if (strlen((out).data) > 0) {
    text_append_cstr(&(out), "...");
  }
  return out;
}

static void rfidScrollLog(void) {
  for (int i = 0; i < kRfidMaxLogLines - 1; i++) {
    s_rfidLogBuffer[i] = s_rfidLogBuffer[i + 1];
    s_rfidLogColor[i] = s_rfidLogColor[i + 1];
  }
}

static int rfidLogAreaBottom(void) {
  if (s_rfidDetailBoxVisible) {
    return kRfidLogEndY;
  }
  const int logH = rfidDetailBottom() - kRfidLogBoxTop;
  return kRfidLogBoxTop + logH - 3;
}

static int rfidLogBoxDrawHeight(void) {
  if (s_rfidDetailBoxVisible) {
    return kRfidLogBoxH;
  }
  return rfidDetailBottom() - kRfidLogBoxTop;
}

static void rfidSetDetailBoxVisible(bool visible) {
  if (s_rfidDetailBoxVisible == visible) {
    return;
  }
  s_rfidDetailBoxVisible = visible;
  s_rfidBoxesDrawn = false;
}

static int rfidLogVisibleLines(void) {
  const int h = rfidLogAreaBottom() - kRfidLogStartY;
  if (h <= 0 || kRfidLineHeight <= 0) {
    return 1;
  }
  return h / kRfidLineHeight;
}

static void rfidRedrawActivityLog(void) {
  const int visible = rfidLogVisibleLines();
  const int logBottom = rfidLogAreaBottom();
  display_fill_rect(&(tft), 6, kRfidLogStartY, TFT_WIDTH - 12,
                    logBottom - kRfidLogStartY, TFT_BLACK);
  if (visible <= 0 || s_rfidLogIndex <= 0) {
    return;
  }
  const int start = (s_rfidLogIndex > visible) ? (s_rfidLogIndex - visible) : 0;
  for (int row = 0; row < visible; row++) {
    const int bufIndex = start + row;
    if (bufIndex >= s_rfidLogIndex) {
      break;
    }
    if (strlen((s_rfidLogBuffer[bufIndex]).data) == 0) {
      continue;
    }
    const int yPos = kRfidLogStartY + row * kRfidLineHeight;
    display_set_text_size(&(tft), 1);
    display_set_text_color_2(&(tft), s_rfidLogColor[bufIndex], TFT_BLACK);
    display_set_cursor(&(tft), kRfidLogTextX, yPos);
    display_print(&(tft), (rfidFitLogText(s_rfidLogBuffer[bufIndex])).data);
  }
}

static void rfidAppendLogLine(text_t line, uint16_t color) {
  if (strlen((line).data) == 0) {
    return;
  }
  if (s_rfidLogIndex >= kRfidMaxLogLines) {
    rfidScrollLog();
    s_rfidLogIndex = kRfidMaxLogLines - 1;
  }
  s_rfidLogBuffer[s_rfidLogIndex] = rfidFitLogText(line);
  s_rfidLogColor[s_rfidLogIndex] = color;
  s_rfidLogIndex++;
}

static void rfidPrint(text_t text, uint16_t color, bool extraSpace) {
  display_set_text_size(&(tft), 1);
  text_t remaining = text;
  while (strlen((remaining).data) > 0) {
    const int nl = text_find_index(remaining, text_character('\n'), 0);
    if (nl >= 0) {
      rfidAppendLogLine(text_slice(remaining, 0, nl), color);
      remaining = text_slice(remaining, nl + 1, SIZE_MAX);
      continue;
    }
    if (display_text_width(&(tft), (remaining).data, 0) <= kRfidLogTextW) {
      rfidAppendLogLine(remaining, color);
      break;
    }
    int end = (int)strlen((remaining).data);
    while (end > 1 &&
           display_text_width(&(tft), (text_slice(remaining, 0, end)).data, 0) >
               kRfidLogTextW) {
      end--;
    }
    if (end < (int)strlen((remaining).data)) {
      const int sp =
          text_rfind_index(text_slice(remaining, 0, end), text_character(' '));
      if (sp > 0) {
        end = sp;
      }
    }
    text_t line = text_slice(remaining, 0, end);
    text_trim(&(line));
    remaining = text_slice(remaining, end, SIZE_MAX);
    text_trim(&(remaining));
    if (strlen((line).data) == 0 && strlen((remaining).data) > 0) {
      line = rfidFitLogText(remaining);
      remaining = TEXT("");
    }
    rfidAppendLogLine(line, color);
  }
  if (extraSpace && s_rfidLogIndex < kRfidMaxLogLines) {
    s_rfidLogBuffer[s_rfidLogIndex] = TEXT("");
    s_rfidLogColor[s_rfidLogIndex] = TFT_WHITE;
    s_rfidLogIndex++;
  }
  rfidRedrawActivityLog();
}

static void rfidDrawStatusLine(int line, text_t text, uint16_t color) {
  const int y = kRfidStatusTextY + line * kRfidLineHeight;
  text_t fitted = rfidFitStatusText(text);
  display_fill_rect(&(tft), 8, y, TFT_WIDTH - 16, kRfidLineHeight, TFT_BLACK);
  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), color, TFT_BLACK);
  display_set_cursor(&(tft), 8, y);
  display_print(&(tft), (fitted).data);
}

static void rfidDrawStatusLineIfChanged(int line, text_t text, uint16_t color) {
  if (line < 0 || line >= kRfidStatusLineCount) {
    return;
  }
  if ((strcmp((s_rfidStatusLineText[line]).data, (text).data) == 0) &&
      s_rfidStatusLineColor[line] == color) {
    return;
  }
  s_rfidStatusLineText[line] = text;
  s_rfidStatusLineColor[line] = color;
  rfidDrawStatusLine(line, text, color);
}

/** Repaint NFC Status lines after overlay/box chrome cleared the framebuffer.
 */
static void rfidRepaintStatusFromCache(void) {
  for (int i = 0; i < kRfidStatusLineCount; i++) {
    if (strlen((s_rfidStatusLineText[i]).data) > 0) {
      rfidDrawStatusLine(i, s_rfidStatusLineText[i], s_rfidStatusLineColor[i]);
    }
  }
}

static void rfidDrawTextBoxes(void) {
  const int detailBottom = rfidDetailBottom();
  const int detailTop = rfidEffectiveDetailTop();
  const int logH = rfidLogBoxDrawHeight();
  display_fill_rect(&(tft), 0, kRfidStatusY - 2, TFT_WIDTH,
                    detailBottom - kRfidStatusY + 2, TFT_BLACK);
  display_draw_fast_h_line(&(tft), 0, 19, TFT_WIDTH, UI_LINE);
  display_draw_round_rect(&(tft), 4, kRfidLogBoxTop, TFT_WIDTH - 8, logH, 3,
                          UI_LINE);
  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), UI_DIM_TEXT, TFT_BLACK);
  display_draw_string(&(tft), "NFC Status", 8, kRfidStatusY + 3, 0);
  display_draw_string(&(tft), "Activity", 8, kRfidLogBoxTop + 3, 0);
  if (s_rfidDetailBoxVisible) {
    const int detailH = detailBottom - detailTop;
    if (detailH > 8) {
      display_draw_round_rect(&(tft), 4, detailTop, TFT_WIDTH - 8, detailH, 3,
                              UI_LINE);
    }
    display_draw_string(&(tft), "Tag Details", 8, detailTop + 3, 0);
  } else {
    const int innerH = logH - kRfidBoxHeaderH - 2;
    if (innerH > 0) {
      display_fill_rect(&(tft), 6, kRfidLogBoxTop + kRfidBoxHeaderH,
                        TFT_WIDTH - 12, innerH, TFT_BLACK);
    }
  }
  s_rfidBoxesDrawn = true;
}

static void rfidDrawStaticStatusLines(void) {
  if (s_rfidStatusStaticDrawn) {
    return;
  }
  rfidDrawStatusLineIfChanged(4, TEXT("Protocol: ISO14443A"), UI_DIM_TEXT);
  if (s_pn532VerStr[0] != '\0') {
    rfidDrawStatusLineIfChanged(
        5, text_join_cstr("Reader: ", (TEXT(s_pn532VerStr)).data), UI_DIM_TEXT);
  } else {
    rfidDrawStatusLineIfChanged(5, TEXT("Reader: PN532"), UI_DIM_TEXT);
  }
  s_rfidStatusStaticDrawn = true;
}

static void rfidResetUiState(void) {
  s_rfidUiDrawn = false;
  s_rfidBoxesDrawn = false;
  s_rfidStatusStaticDrawn = false;
  s_rfidDetailBoxVisible = false;
  s_featureInfoAvailable = false;
  s_featureInfoTitle[0] = '\0';
  s_featureInfoBody[0] = '\0';
  s_listenCenterLabel[0] = '\0';
  s_rfidLogIndex = 0;
  for (int i = 0; i < kRfidMaxLogLines; i++) {
    s_rfidLogBuffer[i] = TEXT("");
    s_rfidLogColor[i] = 0;
  }
  for (int i = 0; i < kRfidStatusLineCount; i++) {
    s_rfidStatusLineText[i] = TEXT("");
    s_rfidStatusLineColor[i] = 0;
  }
}

static char s_dynPrev1[72];
static char s_dynPrev2[72];
static char s_dynPrevAux[72];
static char s_dynPrevPill[16];
static char s_dynPillLabel[16] = "LIVE";
static uint16_t s_dynPillColor = 0xFBE4;

static unsigned s_listenSec = 0xFFFFFFFFu;
static uint8_t s_listenSpin = 0;
static unsigned long s_listenSpinMs = 0;

static void rfidListenShellReset(void) {
  s_listenSec = 0xFFFFFFFFu;
  s_listenSpin = 0;
  s_listenSpinMs = 0;
}

static void rfidDynClearPrev(void) {
  s_dynPrev1[0] = '\0';
  s_dynPrev2[0] = '\0';
  s_dynPrevAux[0] = '\0';
  s_dynPrevPill[0] = '\0';
}

static void rfidSetDynamicPill(const char* label, uint16_t color) {
  if (!label) {
    return;
  }
  strncpy(s_dynPillLabel, label, sizeof(s_dynPillLabel) - 1);
  s_dynPillLabel[sizeof(s_dynPillLabel) - 1] = '\0';
  s_dynPillColor = color;
}

static void rfidSetFeatureInfo(const char* title, const char* body) {
  if (title && title[0]) {
    strncpy(s_featureInfoTitle, title, sizeof(s_featureInfoTitle) - 1);
    s_featureInfoTitle[sizeof(s_featureInfoTitle) - 1] = '\0';
  }
  if (!body) {
    body = "";
  }
  strncpy(s_featureInfoBody, body, sizeof(s_featureInfoBody) - 1);
  s_featureInfoBody[sizeof(s_featureInfoBody) - 1] = '\0';
  s_featureInfoAvailable = s_featureInfoBody[0] != '\0';
}

static int rfidInfoPanelPageCount(const char* body) {
  return rfidInfoPanelPageCountForLines(body, rfidDetailLinesPerPage());
}

static bool rfidDetailTagNeedsPill(const char* tagLabel) {
  if (!tagLabel || !tagLabel[0]) {
    return false;
  }
  return strcmp(tagLabel, "INFO") != 0 && strcmp(tagLabel, "PROGRESS") != 0 &&
         strcmp(tagLabel, "SCAN") != 0 && strcmp(tagLabel, "RESULT") != 0;
}

static void rfidDrawInfoPanel(const char* tagLabel, const char* body, int page,
                              int pageCount, int linesPerPage) {
  const int detailBottom = rfidDetailBottom();
  const int detailTop = rfidEffectiveDetailTop();
  const int bodyTop = rfidDetailBodyTop();
  display_fill_rect(&(tft), 5, detailTop + kRfidBoxHeaderH, TFT_WIDTH - 10,
                    detailBottom - detailTop - kRfidBoxHeaderH - 1, TFT_BLACK);
  display_set_text_font(&(tft), 1);
  const int tagTop = detailTop + kRfidBoxHeaderH + 2;
  int textTop = bodyTop;
  if (pageCount > 1) {
    display_fill_round_rect(&(tft), kRfidDetailPadX, tagTop, 62, RF_TAG_H, 7,
                            DARK_GRAY);
    display_set_text_color_2(&(tft), UI_ICON, DARK_GRAY);
    display_set_cursor(&(tft), kRfidDetailPadX + 6, tagTop + 3);
    display_printf(&(tft), "%d/%d", page + 1, pageCount);
    textTop = tagTop + RF_TAG_H + RF_TAG_GAP;
  } else if (rfidDetailTagNeedsPill(tagLabel)) {
    int pillW = (int)display_text_width(&(tft), tagLabel, 0) + 14;
    if (pillW < 48) {
      pillW = 48;
    }
    display_fill_round_rect(&(tft), kRfidDetailPadX, tagTop, pillW, RF_TAG_H, 7,
                            DARK_GRAY);
    display_set_text_color_2(&(tft), UI_ICON, DARK_GRAY);
    display_set_cursor(&(tft), kRfidDetailPadX + 6, tagTop + 3);
    display_print(&(tft), (TEXT(tagLabel)).data);
    textTop = tagTop + RF_TAG_H + RF_TAG_GAP;
  }
  rfidPrintWrappedPageBg(kRfidDetailPadX, textTop, kRfidDetailInnerW, body,
                         kRfidLineHeight, UI_TEXT, TFT_BLACK,
                         page * linesPerPage, linesPerPage);
}

static void rfidRefreshInfoPanel(const char* tagLabel, const char* body) {
  (void)tagLabel;
  (void)body;
}

static void rfidDrawHint(bool twoActions, int padX) {
  if (featureHasTouchNavBar()) {
    return;
  }
  const int hintY = rfidHintY();
  display_fill_rect(&(tft), 0, hintY - 3, TFT_WIDTH, 17, FEATURE_BG);
  display_set_text_font(&(tft), 1);
  display_set_text_color_2(&(tft), UI_DIM_TEXT, FEATURE_BG);
  display_set_cursor(&(tft), padX, hintY);
  display_print(&(tft), (TEXT(twoActions ? "LEFT: Back   SELECT: action"
                                         : "LEFT / SELECT: close"))
                            .data);
}

static void rfidDynamicBand(int topY, const char* line1, const char* line2) {
  (void)topY;
  const char* l1 = line1 ? line1 : "";
  const char* l2 = line2 ? line2 : "";
  if (strcmp(s_dynPrev1, l1) == 0 && strcmp(s_dynPrev2, l2) == 0 &&
      strcmp(s_dynPrevAux, s_rfidBandAux) == 0 &&
      strcmp(s_dynPrevPill, s_dynPillLabel) == 0) {
    return;
  }

  rfidDrawStaticStatusLines();
  text_t modeLine = text_join_cstr("Mode: ", (TEXT(s_dynPillLabel)).data);
  rfidDrawStatusLineIfChanged(0, modeLine, s_dynPillColor);
  rfidDrawStatusLineIfChanged(1, TEXT(l1), UI_TEXT);
  rfidDrawStatusLineIfChanged(2, TEXT(l2), UI_DIM_TEXT);
  if (s_rfidBandAux[0]) {
    rfidDrawStatusLineIfChanged(3, TEXT(s_rfidBandAux), UI_DIM_TEXT);
  }

  strncpy(s_dynPrev1, l1, sizeof(s_dynPrev1) - 1);
  s_dynPrev1[sizeof(s_dynPrev1) - 1] = '\0';
  strncpy(s_dynPrev2, l2, sizeof(s_dynPrev2) - 1);
  s_dynPrev2[sizeof(s_dynPrev2) - 1] = '\0';
  strncpy(s_dynPrevAux, s_rfidBandAux, sizeof(s_dynPrevAux) - 1);
  s_dynPrevAux[sizeof(s_dynPrevAux) - 1] = '\0';
  strncpy(s_dynPrevPill, s_dynPillLabel, sizeof(s_dynPrevPill) - 1);
  s_dynPrevPill[sizeof(s_dynPrevPill) - 1] = '\0';
}

static void rfidRedrawListenChrome(void) {
  rfidSetDetailBoxVisible(false);
  rfidDrawTextBoxes();
  rfidRepaintStatusFromCache();
  rfidRedrawActivityLog();
  if (featureHasTouchNavBar() && s_listenCenterLabel[0]) {
    rfidSetListenNavLabels(s_listenCenterLabel, s_featureInfoAvailable);
  }
}

/** Wait for Exit on 2-box chrome; feature info stays on Info nav. */
static void rfidWaitTwoBoxDismiss(void) {
  rfidSetDetailBoxVisible(false);
  rfidDrawShellFooter1("Exit");
  rfidDrawTextBoxes();
  rfidRepaintStatusFromCache();
  rfidRedrawActivityLog();
  if (featureHasTouchNavBar()) {
    rfidSetListenNavLabels("Exit", s_featureInfoAvailable);
  }

  for (;;) {
    if (rfidPumpSessionUi()) {
      return;
    }
    rfidTryShowInfo();
    if (rfidPollFooter(s_rfFoot, 1, false) != RfidUiEvt_None) {
      return;
    }
    sys_sleep_ms(8);
  }
}

/** Timeout dismiss: Exit leaves, Ready retries. @return true when Ready
 * pressed. */
static bool rfidWaitTimeoutDismiss(void) {
  rfidSetDetailBoxVisible(false);
  FeatureUI_layoutFooter2(&(s_rfFoot), "Exit", FeatureUI_ButtonStyle_Secondary,
                          "Ready", FeatureUI_ButtonStyle_Primary, false, false);
  rfidDrawTextBoxes();
  rfidRepaintStatusFromCache();
  rfidRedrawActivityLog();
  if (featureHasTouchNavBar()) {
    rfidSetTimeoutNavLabels(s_featureInfoAvailable);
  } else {
    FeatureUI_drawFooterBg();
    FeatureUI_drawButton(&(s_rfFoot[0]), false);
    FeatureUI_drawButton(&(s_rfFoot[1]), false);
  }

  for (;;) {
    if (rfidPumpSessionUi()) {
      return false;
    }
    rfidTryShowInfo();
    RfidUiEvt e = rfidPollFooter(s_rfFoot, 2, true);
    if (e == RfidUiEvt_Primary) {
      return true;
    }
    if (e == RfidUiEvt_Back) {
      return false;
    }
    sys_sleep_ms(8);
  }
}

static bool rfidDismissTimeoutPanel(const char* statusLine,
                                    const char* message) {
  rfidSetDynamicPill("WARN", UI_WARN);
  rfidSetBandAux(NULL);
  rfidDynClearPrev();
  rfidDynamicBand(0, statusLine, "No tag detected");

  text_t logLine = TEXT(message);
  const int nl = text_find_index(logLine, text_character('\n'), 0);
  if (nl >= 0) {
    logLine = text_slice(logLine, 0, nl);
  }
  rfidPrint(text_join_cstr("[!] ", (logLine).data), UI_WARN, false);
  return rfidWaitTimeoutDismiss();
}

static void rfidShowInfoPanel(void) {
  const bool showTitle = s_featureInfoTitle[0] != '\0';
  const int linesPerPage = rfidInfoOverlayLinesPerPage(showTitle);
  const int pageCount =
      rfidInfoPanelPageCountForLines(s_featureInfoBody, linesPerPage);
  int page = 0;
  bool redraw = false;
  rfidSetInfoOverlayNavLabels(pageCount);
  rfidDrawInfoOverlay(s_featureInfoTitle, s_featureInfoBody, page, pageCount,
                      linesPerPage);

  for (;;) {
    if (rfidPumpSessionUi()) {
      rfidRedrawListenChrome();
      return;
    }
    if (redraw) {
      rfidDrawInfoOverlay(s_featureInfoTitle, s_featureInfoBody, page,
                          pageCount, linesPerPage);
      redraw = false;
    }

    if (isButtonPressed(BTN_LEFT)) {
      rfidReleaseNavButtons();
      break;
    }
    if (pageCount == 1 && isButtonPressed(BTN_SELECT)) {
      rfidReleaseNavButtons();
      break;
    }
    if (pageCount > 1 && isButtonPressed(BTN_UP) && page < pageCount - 1) {
      rfidReleaseNavButtons();
      page++;
      redraw = true;
      continue;
    }
    if (pageCount > 1 && isButtonPressed(BTN_DOWN) && page > 0) {
      rfidReleaseNavButtons();
      page--;
      redraw = true;
      continue;
    }

    if (featureHasTouchNavBar()) {
      if (isTouchNavButtonPressedEdge(BTN_LEFT) ||
          isTouchNavButtonPressedEdge(BTN_SELECT)) {
        rfidReleaseNavButtons();
        break;
      }
      if (pageCount > 1 && isTouchNavButtonPressedEdge(BTN_UP) &&
          page < pageCount - 1) {
        rfidReleaseNavButtons();
        page++;
        redraw = true;
        continue;
      }
      if (pageCount > 1 && isTouchNavButtonPressedEdge(BTN_DOWN) && page > 0) {
        rfidReleaseNavButtons();
        page--;
        redraw = true;
        continue;
      }
    }

    sys_sleep_ms(8);
  }

  rfidRedrawListenChrome();
}

static void rfidTryShowInfo(void) {
  if (!s_featureInfoAvailable || !s_featureInfoBody[0]) {
    return;
  }

  bool open = false;
  if (featureHasTouchNavBar()) {
    if (isTouchNavButtonPressedEdge(BTN_RIGHT)) {
      rfidReleaseNavButtons();
      open = true;
    }
  } else if (isButtonPressed(BTN_RIGHT)) {
    rfidReleaseNavButtons();
    open = true;
  }
  if (!open) {
    return;
  }

  rfidShowInfoPanel();
}

static void rfidListenTick(const char* statusLine, unsigned long t0) {
  const char* spin = "|/-\\";
  unsigned sec = (unsigned)((sys_monotonic_ms() - t0) / 1000UL);
  char detail[40];
  if (sec != s_listenSec) {
    s_listenSec = sec;
    snprintf(detail, sizeof(detail), "%u s", sec);
  } else {
    if (sys_monotonic_ms() - s_listenSpinMs > 320) {
      s_listenSpin = (uint8_t)((s_listenSpin + 1) % 4);
      s_listenSpinMs = sys_monotonic_ms();
    }
    snprintf(detail, sizeof(detail), "%u s  %c", sec, spin[s_listenSpin]);
  }
  rfidDynamicBand(0, statusLine, detail);
}

static void rfidShellListen(const char* title, const char* footerLabel,
                            const char* infoBody) {
  rfidListenShellReset();
  rfidDynClearPrev();
  rfidSetDynamicPill("SCAN", UI_WARN);
  rfidFeatureSetup(title);
  rfidSetDetailBoxVisible(false);
  const char* sub = (s_pn532VerStr[0] != '\0') ? s_pn532VerStr : NULL;
  rfidSetBandAux(sub);
  const char* info =
      (infoBody && infoBody[0]) ? infoBody : "Waiting for tag...";
  rfidSetFeatureInfo(title, info);
  rfidDrawShellFooter1(footerLabel);
  rfidDrawHint(false, RF_PAD_X);
  rfidDrawStaticStatusLines();
  rfidDynamicBand(0, "Ready", "");
  rfidPrint(TEXT("[*] Waiting for tag..."), UI_DIM_TEXT, false);
}

static void rfidShellProgress(const char* mainTitle, const char* stepLine,
                              const char* footerLabel, const char* infoBody) {
  rfidListenShellReset();
  rfidDynClearPrev();
  rfidSetDynamicPill("WORK", UI_OK);
  rfidFeatureSetup(mainTitle);
  rfidSetDetailBoxVisible(false);
  rfidDrawTextBoxes();
  rfidSetBandAux(stepLine);
  const char* info = (infoBody && infoBody[0]) ? infoBody : stepLine;
  rfidSetFeatureInfo(mainTitle, info);
  rfidDrawShellFooter1(footerLabel);
  rfidDrawHint(false, RF_PAD_X);
  rfidDrawStaticStatusLines();
  rfidDynamicBand(0, stepLine, "");
  rfidPrint(text_join_cstr("[!] ", (TEXT(stepLine)).data), UI_TEXT, false);
  rfidRedrawNavChrome();
}

static void rfidTransitionToProgress(const char* title, const char* stepLine,
                                     const char* footerLabel,
                                     const char* infoBody) {
  (void)infoBody;
  if (title && title[0]) {
    rfidUpdateToolbarTitle(title);
  }
  rfidListenShellReset();
  rfidDynClearPrev();
  rfidSetDynamicPill("WORK", UI_OK);
  rfidSetDetailBoxVisible(false);
  rfidSetBandAux(stepLine);
  rfidDrawShellFooter1(footerLabel);
  rfidDrawTextBoxes();
  rfidDrawStaticStatusLines();
  rfidDynamicBand(0, stepLine, "");
  rfidPrint(text_join_cstr("[!] ", (TEXT(stepLine)).data), UI_TEXT, false);
}

static void rfidLayoutFull(const char* title, const char* subtitleOrNull,
                           const char* body, bool twoButtons,
                           const char* primaryLabel,
                           FeatureUI_ButtonStyle primaryStyle,
                           bool primaryDisabled) {
  applyThemeToPalette((*settings()).theme);
  rfidResetUiState();
  rfidClearBody(TFT_BLACK);
  drawStatusBar(readBatteryVoltage(), true, false);
  s_rfidBandAux[0] = '\0';
  rfidRunToolbar(title);

  const int boxY = kRfidStatusY;
  const int boxBottom = rfidDetailBottom();
  const int boxH = boxBottom - boxY;
  if (boxH > 12) {
    display_draw_round_rect(&(tft), 4, boxY, TFT_WIDTH - 8, boxH, 3, UI_LINE);
    display_set_text_size(&(tft), 1);
    display_set_text_color_2(&(tft), UI_DIM_TEXT, TFT_BLACK);
    display_draw_string(&(tft), "Details", 8, boxY + 3, 0);
    const int innerX = kRfidDetailPadX;
    const int innerW = kRfidDetailInnerW;
    const bool hasSub = subtitleOrNull && subtitleOrNull[0];
    int tagTop = boxY + kRfidBoxHeaderH + 4;
    int bodyTop = tagTop + RF_TAG_H + RF_TAG_GAP;
    if (hasSub) {
      display_set_text_font(&(tft), 1);
      display_set_text_color_2(&(tft), UI_DIM_TEXT, TFT_BLACK);
      text_t sub = TEXT(subtitleOrNull);
      const bool wasLong = display_text_width(&(tft), (sub).data, 0) > innerW;
      while (strlen((sub).data) > 0 &&
             display_text_width(
                 &(tft),
                 ((wasLong ? text_join_cstr((sub).data, "...") : sub)).data,
                 0) > innerW) {
        text_erase(&(sub), strlen((sub).data) - 1, SIZE_MAX);
      }
      if (wasLong) {
        text_append_cstr(&(sub), "...");
      }
      display_set_cursor(&(tft), innerX, boxY + kRfidBoxHeaderH + 2);
      display_print(&(tft), (sub).data);
      tagTop = boxY + kRfidBoxHeaderH + 16;
      bodyTop = tagTop + RF_TAG_H + RF_TAG_GAP;
    }
    const char* tag =
        (primaryStyle == FeatureUI_ButtonStyle_Danger) ? "CAUTION" : "DETAILS";
    display_set_text_font(&(tft), 1);
    display_fill_round_rect(&(tft), innerX, tagTop, 52, RF_TAG_H, 7, DARK_GRAY);
    display_set_text_color_2(&(tft), UI_ICON, DARK_GRAY);
    display_set_cursor(&(tft), innerX + 6, tagTop + 3);
    display_print(&(tft), (TEXT(tag)).data);
    rfidPrintWrappedStepBg(innerX, bodyTop, innerW, body, RF_BODY_LINE_PX,
                           UI_TEXT, TFT_BLACK, boxBottom - 4);
  }

  rfidRedrawNavChrome();

  if (twoButtons && primaryLabel) {
    rfidDrawShellFooter2(primaryLabel, primaryStyle, primaryDisabled);
    rfidDrawHint(true, RF_PAD_X);
  } else {
    rfidDrawShellFooter1("Back");
    rfidDrawHint(false, RF_PAD_X);
  }
}

static bool rfidSubtitleIsFailure(const char* sub) {
  if (!sub || !sub[0]) {
    return false;
  }
  return strstr(sub, "fail") || strstr(sub, "Fail") || strstr(sub, "Timeout") ||
         strstr(sub, "Skipped") || strstr(sub, "No tag") ||
         strstr(sub, "Unsupported") || strstr(sub, "Aborted") ||
         strstr(sub, "Stopped") || strstr(sub, "No write") ||
         strstr(sub, "lost") || strstr(sub, "Lost") ||
         strstr(sub, "Incomplete");
}

/** Result dismiss: Exit only on success; Exit + Ready when failed. */
static void rfidWaitResultDismiss(bool offerRetry) {
  if (offerRetry) {
    if (rfidWaitTimeoutDismiss()) {
      s_rfidRetrySession = true;
    }
    return;
  }
  rfidWaitTwoBoxDismiss();
}

static void rfidPresentResult(const char* title, const char* sub,
                              const char* body, const char* detailTag,
                              const char* pillLabel) {
  (void)detailTag;
  if (!s_rfidBoxesDrawn) {
    rfidFeatureSetup(title ? title : "RFID");
  } else if (title && title[0]) {
    rfidUpdateToolbarTitle(title);
  }

  rfidListenShellReset();
  rfidDynClearPrev();

  const bool failed = rfidSubtitleIsFailure(sub);
  if (pillLabel && pillLabel[0]) {
    rfidSetDynamicPill(pillLabel, failed ? UI_WARN : UI_OK);
  } else {
    rfidSetDynamicPill(failed ? "WARN" : "DONE", failed ? UI_WARN : UI_OK);
  }
  rfidSetBandAux(NULL);
  rfidDrawStaticStatusLines();
  rfidDynamicBand(0, title ? title : "Done", sub && sub[0] ? sub : "Complete");
  if (sub && sub[0]) {
    rfidPrint(
        text_join_cstr(
            (text_join_cstr(
                 (text_join_cstr("[", (TEXT((failed ? "!" : "+"))).data)).data,
                 "] "))
                .data,
            (TEXT(sub)).data),
        failed ? UI_WARN : UI_OK, false);
  }
  if (body && body[0]) {
    text_t remaining = TEXT(body);
    int lines = 0;
    while (strlen((remaining).data) > 0 && lines < 8) {
      const int nl = text_find_index(remaining, text_character('\n'), 0);
      text_t line = ((nl >= 0) ? text_slice(remaining, 0, nl) : remaining);
      remaining =
          ((nl >= 0) ? text_slice(remaining, nl + 1, SIZE_MAX) : TEXT(""));
      text_trim(&(line));
      if (strlen((line).data) > 0) {
        rfidPrint(line, failed ? UI_WARN : UI_TEXT, false);
        lines++;
      }
    }
  }
  rfidWaitResultDismiss(failed);
}

static void rfidResultAndDismiss(const char* title, const char* sub,
                                 const char* body) {
  rfidPresentResult(title, sub, body, "RESULT", NULL);
}

/** @return true = primary (right) action, false = back */
static bool rfidRunTwoButtonDialog(const char* title, const char* sub,
                                   const char* body, const char* primaryLabel,
                                   FeatureUI_ButtonStyle primaryStyle) {
  rfidLayoutFull(title, sub, body, true, primaryLabel, primaryStyle, false);
  for (;;) {
    if (rfidPumpSessionUi()) {
      return false;
    }
    RfidUiEvt e = rfidPollFooter(s_rfFoot, 2, true);
    if (e == RfidUiEvt_Back) {
      return false;
    }
    if (e == RfidUiEvt_Primary) {
      return true;
    }
    sys_sleep_ms(8);
  }
}

/** 2-box confirm: Back cancels, primary continues. */
static bool rfidRunTwoBoxConfirm(const char* title, const char* statusLine,
                                 const char* statusDetail,
                                 const char* activityMsg, const char* infoBody,
                                 const char* primaryLabel) {
  rfidListenShellReset();
  rfidDynClearPrev();
  rfidSetDynamicPill("READY", UI_OK);
  rfidFeatureSetup(title);
  rfidSetDetailBoxVisible(false);
  rfidSetFeatureInfo(title, infoBody ? infoBody : statusLine);
  rfidSetBandAux(NULL);
  rfidDynamicBand(0, statusLine, statusDetail ? statusDetail : "");
  if (activityMsg && activityMsg[0]) {
    rfidPrint(TEXT(activityMsg), UI_TEXT, false);
  }
  rfidDrawShellFooter2(primaryLabel, FeatureUI_ButtonStyle_Primary, false);
  rfidDrawHint(true, RF_PAD_X);

  for (;;) {
    if (rfidPumpSessionUi()) {
      return false;
    }
    rfidTryShowInfo();
    RfidUiEvt e = rfidPollFooter(s_rfFoot, 2, true);
    if (e == RfidUiEvt_Back) {
      return false;
    }
    if (e == RfidUiEvt_Primary) {
      return true;
    }
    sys_sleep_ms(8);
  }
}

static bool rfidPollListenUi(void) {
  if (rfidPumpSessionUi()) {
    return true;
  }
  rfidTryShowInfo();
  return rfidPollFooter(s_rfFoot, 1, false) != RfidUiEvt_None;
}

static bool rfidPollCancel(void) { return rfidPollListenUi(); }

static bool tryMagicBackdoor(void) {
  uint8_t commands[][2] = {{0x40, 0x00}, {0x43, 0x00}};
  uint8_t response[32];
  uint8_t responseLength = 32;
  for (int i = 0; i < 2; i++) {
    if (pn532_in_data_exchange(&(s_nfc), commands[i], sizeof(commands[i]),
                               response, &responseLength)) {
      return true;
    }
  }
  return false;
}

/** True if the same ISO14443A tag is still on the coil (short poll). */
static bool rfidTagStillPresent(const uint8_t* uid, uint8_t uidLen) {
  uint8_t uidNow[7] = {0};
  uint8_t lenNow = 0;
  if (!pn532_read_passive_target_id(&(s_nfc), PN532_MIFARE_ISO14443A, uidNow,
                                    &lenNow, 80)) {
    return false;
  }
  if (lenNow != uidLen) {
    return false;
  }
  return memcmp(uid, uidNow, uidLen) == 0;
}

static bool rfidDumpTagLost(const uint8_t* uid, uint8_t uidLen,
                            unsigned blockOrPage) {
  if (rfidTagStillPresent(uid, uidLen)) {
    return false;
  }
  char body[160];
  snprintf(body, sizeof(body),
           "Tag left the field during dump\n(at block/page %u).\n\n"
           "Keep the tag on the coil until reading finishes.",
           blockOrPage);
  rfidResultAndDismiss("Dump", "Tag lost", body);
  return true;
}

static CardType detectCardType(uint8_t* uid, uint8_t uidLength) {
  if (uidLength != 4 && uidLength != 7) {
    return UNKNOWN;
  }
  uint8_t keyA[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  if (pn532_mifare_classic_authenticate_block(&(s_nfc), uid, uidLength, 0, 0,
                                              keyA)) {
    return MIFARE_CLASSIC;
  }
  uint8_t data[16];
  if (pn532_mifare_ultralight_read_page(&(s_nfc), 4, data)) {
    uint8_t cmd[] = {0x60};
    uint8_t response[32];
    uint8_t responseLength = 32;
    if (pn532_in_data_exchange(&(s_nfc), cmd, sizeof(cmd), response,
                               &responseLength) &&
        responseLength >= 8) {
      if (response[0] == 0x00 && response[1] == 0x04) {
        return NTAG;
      }
    }
    return MIFARE_ULTRALIGHT;
  }
  uint8_t cmd[] = {0x5A, 0x00, 0x00, 0x00};
  uint8_t response[32];
  uint8_t responseLength = 32;
  if (pn532_in_data_exchange(&(s_nfc), cmd, sizeof(cmd), response,
                             &responseLength) &&
      responseLength > 0) {
    return MIFARE_DESFIRE;
  }
  return UNKNOWN;
}

static const char* cardTypeStr(CardType t) {
  switch (t) {
    case MIFARE_CLASSIC:
      return "MIFARE Classic";
    case MIFARE_ULTRALIGHT:
      return "Ultralight";
    case NTAG:
      return "NTAG";
    case MIFARE_DESFIRE:
      return "DESFire";
    default:
      return "Unknown";
  }
}

static void uidToHex(char* out, size_t outLen, uint8_t* uid, uint8_t uidLen) {
  size_t pos = 0;
  out[0] = '\0';
  for (uint8_t i = 0; i < uidLen && i < 7u && pos + 3 < outLen; i++) {
    pos += (size_t)snprintf(out + pos, outLen - pos, "%02X", uid[i]);
  }
}

static void formatClassicBlock0(char* out, size_t outLen, const uint8_t* d) {
  snprintf(out, outLen,
           "Sector 0 block 0 (read with key A = FF FF FF FF FF FF):\n"
           "%02X %02X %02X %02X %02X %02X %02X %02X\n"
           "%02X %02X %02X %02X %02X %02X %02X %02X",
           d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10],
           d[11], d[12], d[13], d[14], d[15]);
}

bool RfidNfc_begin(void) {
  s_hwOk = false;
  rfidAttachBus();
  pn532_begin(&(s_nfc));
  uint32_t ver = pn532_get_firmware_version(&(s_nfc));
  if (!ver) {
    rfidPn532VerStr(0);
    refreshSdMount();
    return false;
  }
  rfidPn532VerStr(ver);
  pn532_sam_config(&(s_nfc));
  pn532_set_passive_activation_retries(&(s_nfc), 0xFF);
  s_hwOk = true;
  return true;
}

bool RfidNfc_hardwareOk(void) { return s_hwOk; }

/** Clear retry flag before starting a session (menu loop). */
void RfidNfc_clearSessionRetry(void) { s_rfidRetrySession = false; }

/** @return true once if the user chose Ready on a failure result screen. */
bool RfidNfc_consumeSessionRetry(void) {
  if (!s_rfidRetrySession) {
    return false;
  }
  s_rfidRetrySession = false;
  return true;
}

void RfidNfc_resetCloneBuffer(void) {}

static bool RfidNfc_rfidTargetSendResponse(const uint8_t* payload,
                                           uint8_t payloadLen) {
  uint8_t tx[64];
  if (payloadLen + 1u > sizeof(tx)) {
    payloadLen = (uint8_t)(sizeof(tx) - 1u);
  }
  tx[0] = 0x8E;
  memcpy(tx + 1, payload, payloadLen);
  return pn532_set_data_target(&(s_nfc), tx, (uint8_t)(payloadLen + 1u)) != 0;
}

/** Wait until an external reader polls us in ISO14443A target mode. */
static bool RfidNfc_rfidTargetWaitReader(const char* tickMsg,
                                         unsigned long timeoutMs, uint8_t* rx,
                                         uint8_t* rxLen, bool showWaitLog) {
  for (;;) {
    unsigned long t0 = sys_monotonic_ms();
    if (showWaitLog) {
      rfidListenShellReset();
      rfidPrint(TEXT("[*] Waiting for external reader..."), UI_DIM_TEXT, false);
    }

    for (;;) {
      if (rfidPumpSessionUi()) {
        return false;
      }
      if (rfidPollCancel()) {
        return false;
      }
      rfidListenTick(tickMsg, t0);

      if (pn532_as_target(&(s_nfc))) {
        const uint8_t maxLen = *rxLen;
        *rxLen = maxLen;
        if (pn532_get_data_target(&(s_nfc), rx, rxLen)) {
          return true;
        }
      }

      if (timeoutMs > 0 && (sys_monotonic_ms() - t0) >= timeoutMs) {
        char tmsg[128];
        snprintf(tmsg, sizeof(tmsg),
                 "No external reader in %lu s.\nTap Ready to wait again.",
                 timeoutMs / 1000UL);
        if (!rfidDismissTimeoutPanel("Timeout", tmsg)) {
          return false;
        }
        rfidPrint(TEXT("[*] Ready - waiting for reader..."), UI_DIM_TEXT,
                  false);
        break;
      }
      sys_sleep_ms(25);
    }
  }
}

static bool RfidNfc_rfidListenIso14443a(const char* title, const char* footer,
                                        uint8_t* uid, uint8_t* uidLenOut,
                                        unsigned long timeoutMs,
                                        const char* tickMsg,
                                        const char* infoBody) {
  const char* info = (infoBody && infoBody[0]) ? infoBody : tickMsg;
  for (;;) {
    unsigned long t0 = sys_monotonic_ms();
    rfidShellListen(title, footer, info);
    for (;;) {
      if (rfidPumpSessionUi()) {
        return false;
      }
      rfidListenTick(tickMsg, t0);
      if (rfidPollCancel()) {
        return false;
      }
      if (pn532_read_passive_target_id(&(s_nfc), PN532_MIFARE_ISO14443A, uid,
                                       uidLenOut, 150)) {
        return true;
      }
      if (sys_monotonic_ms() - t0 > timeoutMs) {
        char tmsg[96];
        snprintf(tmsg, sizeof(tmsg),
                 "No ISO14443A tag in %lu s.\nTap Ready to listen again.",
                 (unsigned long)(timeoutMs / 1000UL));
        if (!rfidDismissTimeoutPanel("Timeout", tmsg)) {
          return false;
        }
        rfidPrint(TEXT("[*] Ready - listening again..."), UI_DIM_TEXT, false);
        break;
      }
      sys_sleep_ms(12);
    }
  }
}

void RfidNfc_sessionErase(void) {
  static const char kIntro[] =
      "Clears MIFARE Classic data block 4 (16 bytes to 00) using default key "
      "A.\n\n"
      "Ultralight / NTAG are not erased by this mode.\n\n"
      "Hold a Classic tag on the coil.";

  rfidAttachBus();
  uint8_t uid[7] = {0};
  uint8_t uidLength = 0;
  if (!RfidNfc_rfidListenIso14443a("Erase", "Cancel", uid, &uidLength,
                                   (unsigned long)RFID_TAG_LISTEN_TIMEOUT_MS,
                                   "Classic tag - block 4 erase", kIntro)) {
    return;
  }

  char uidStr[24] = "";
  uidToHex(uidStr, sizeof(uidStr), uid, uidLength);
  rfidSetDynamicPill("OK", UI_OK);
  rfidSetBandAux(NULL);
  rfidDynamicBand(0, "Tag detected", uidStr);
  rfidPrint(text_join_cstr("[+] Tag detected ", (TEXT(uidStr)).data), UI_OK,
            false);

  CardType ct = detectCardType(uid, uidLength);
  if (ct != MIFARE_CLASSIC) {
    rfidPresentResult("Erase", "Skipped",
                      "This erase only targets MIFARE Classic.", "RESULT",
                      NULL);
    return;
  }

  uint8_t keyA[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t emptyBlock[16] = {0};
  if (!pn532_mifare_classic_authenticate_block(&(s_nfc), uid, uidLength, 4, 0,
                                               keyA)) {
    rfidResultAndDismiss("Erase", "Auth failed",
                         "Could not authenticate block 4 with key A.");
    return;
  }
  if (!pn532_mifare_classic_write_data_block(&(s_nfc), 4, emptyBlock)) {
    rfidResultAndDismiss("Erase", "Write failed",
                         "Block 4 write was rejected.");
    return;
  }
  char body[180];
  snprintf(body, sizeof(body), "UID %s\nBlock 4 cleared (16 bytes zero).",
           uidStr);
  rfidResultAndDismiss("Erase", "Done", body);
}

void RfidNfc_sessionDump(void) {
  static const char kIntro[] =
      "Reads Classic blocks 0-63 or UL/NTAG user pages.\n\n"
      "Summary here; full HEX prints on Serial Console.\n\n"
      "Present tag on the coil.";

  rfidAttachBus();
  uint8_t uid[7] = {0};
  uint8_t uidLength = 0;
  if (!RfidNfc_rfidListenIso14443a("Dump", "Cancel", uid, &uidLength,
                                   (unsigned long)RFID_TAG_LISTEN_TIMEOUT_MS,
                                   "Present tag to dump", kIntro)) {
    return;
  }

  char uidStr[24] = "";
  uidToHex(uidStr, sizeof(uidStr), uid, uidLength);
  CardType ct = detectCardType(uid, uidLength);
  rfidSetDynamicPill("OK", UI_OK);
  rfidSetBandAux(NULL);
  rfidDynamicBand(0, "Tag detected", uidStr);
  rfidPrint(text_join_cstr("[+] Tag detected ", (TEXT(uidStr)).data), UI_OK,
            false);

  uint8_t keyA[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t data[16];
  int blocksWithData = 0;
  int blocksRead = 0;
  int segmentsExpected = 0;

  if (ct == MIFARE_CLASSIC) {
    segmentsExpected = 64;
    rfidTransitionToProgress(
        "Dump", "Reading Classic blocks", "Cancel",
        "Reading MIFARE Classic blocks 0-63.\nFull HEX on Serial Console.");
    div_console_line(TEXT("\n--- RFID Dump Classic ---"));
    for (uint8_t block = 0; block < 64; block++) {
      char l2[48];
      snprintf(l2, sizeof(l2), "Block %u / 63", (unsigned)block);
      rfidDynamicBand(RF_DYN_TOP_PROG, "Dumping MIFARE Classic", l2);
      char infoLine[72];
      snprintf(infoLine, sizeof(infoLine),
               "Reading block %u of 63.\nFull HEX on Serial Console.",
               (unsigned)block);
      rfidRefreshInfoPanel("PROGRESS", infoLine);
      if (rfidPollCancel()) {
        if (!rfidSessionExitRequested()) {
          rfidResultAndDismiss("Dump", "Stopped", "Dump aborted.");
        }
        return;
      }
      const bool ok =
          pn532_mifare_classic_authenticate_block(&(s_nfc), uid, uidLength,
                                                  block, 0, keyA) &&
          pn532_mifare_classic_read_data_block(&(s_nfc), block, data);
      if (ok) {
        blocksRead++;
        bool nz = false;
        for (int i = 0; i < 16; i++) {
          if (data[i] != 0) {
            nz = true;
          }
        }
        if (nz) {
          blocksWithData++;
        }
        printf("B%u:", (unsigned)block);
        for (int i = 0; i < 16; i++) {
          printf(" %02X", data[i]);
        }
        div_console_line((text_t){0});
      } else if (rfidDumpTagLost(uid, uidLength, (unsigned)block)) {
        return;
      }
      sys_sleep_ms(25);
    }
  } else if (ct == MIFARE_ULTRALIGHT || ct == NTAG) {
    uint8_t maxPages = (ct == MIFARE_ULTRALIGHT) ? 16u : 36u;
    segmentsExpected = (int)maxPages - 4;
    rfidTransitionToProgress(
        "Dump", "Reading user pages", "Cancel",
        "Reading Ultralight / NTAG user pages.\nFull HEX on Serial Console.");
    div_console_line(TEXT("\n--- RFID Dump Ultralight / NTAG ---"));
    for (uint8_t page = 4; page < maxPages; page++) {
      char l2[48];
      snprintf(l2, sizeof(l2), "Page %u / %u", (unsigned)page,
               (unsigned)(maxPages - 1));
      rfidDynamicBand(RF_DYN_TOP_PROG, "Dumping pages", l2);
      char infoLine[72];
      snprintf(infoLine, sizeof(infoLine),
               "Reading page %u of %u.\nFull HEX on Serial Console.",
               (unsigned)page, (unsigned)(maxPages - 1));
      rfidRefreshInfoPanel("PROGRESS", infoLine);
      if (rfidPollCancel()) {
        if (!rfidSessionExitRequested()) {
          rfidResultAndDismiss("Dump", "Stopped", "Dump aborted.");
        }
        return;
      }
      uint8_t pg[4];
      if (pn532_mifare_ultralight_read_page(&(s_nfc), page, pg)) {
        blocksRead++;
        bool nz = false;
        for (int i = 0; i < 4; i++) {
          if (pg[i] != 0) {
            nz = true;
          }
        }
        if (nz) {
          blocksWithData++;
        }
        printf("P%u:", (unsigned)page);
        for (int i = 0; i < 4; i++) {
          printf(" %02X", pg[i]);
        }
        div_console_line((text_t){0});
      } else if (rfidDumpTagLost(uid, uidLength, (unsigned)page)) {
        return;
      }
      sys_sleep_ms(18);
    }
  } else {
    rfidResultAndDismiss(
        "Dump", "Unsupported",
        "Dump targets Classic or Ultralight/NTAG.\nTag did not match.");
    return;
  }

  const bool tagGoneAtEnd = segmentsExpected > 0 && blocksRead > 0 &&
                            blocksRead < segmentsExpected &&
                            !rfidTagStillPresent(uid, uidLength);

  if (blocksRead == 0) {
    rfidResultAndDismiss(
        "Dump", "Read failed",
        "No blocks/pages could be read.\nCheck tag position and keys.");
    return;
  }

  if (tagGoneAtEnd) {
    char body[160];
    snprintf(body, sizeof(body),
             "UID: %s\nType: %s\n\nOnly %d of %d blocks read before tag left "
             "the field.",
             uidStr, cardTypeStr(ct), blocksRead, segmentsExpected);
    rfidResultAndDismiss("Dump", "Tag lost", body);
    return;
  }

  char body[420];
  const bool partial = segmentsExpected > 0 && blocksRead < segmentsExpected;
  snprintf(body, sizeof(body),
           "UID: %s\nType: %s\n\n"
           "Segments read OK: %d%s\nBlocks/pages with any non-zero byte: %d\n\n"
           "%sOpen Serial Console for full HEX.",
           uidStr, cardTypeStr(ct), blocksRead,
           partial ? " (partial - some blocks need other keys)" : "",
           blocksWithData,
           partial ? "Some blocks were skipped (auth/key).\n" : "");
  rfidResultAndDismiss("Dump", "Done", body);
}

void RfidNfc_sessionDecodeAccess(void) {
  static const char kIntro[] =
      "Reads MIFARE Classic sector 1 trailer (block 7) and shows raw access "
      "bytes.\n"
      "Uses default keys A/B.\n\n"
      "Present a Classic tag on the coil.";

  rfidAttachBus();
  uint8_t uid[7] = {0};
  uint8_t uidLength = 0;
  if (!RfidNfc_rfidListenIso14443a("Decode Access", "Cancel", uid, &uidLength,
                                   (unsigned long)RFID_TAG_LISTEN_TIMEOUT_MS,
                                   "Classic tag", kIntro)) {
    return;
  }

  char uidStr[24] = "";
  uidToHex(uidStr, sizeof(uidStr), uid, uidLength);
  rfidSetDynamicPill("OK", UI_OK);
  rfidSetBandAux(NULL);
  rfidDynamicBand(0, "Tag detected", uidStr);
  rfidPrint(text_join_cstr("[+] Tag detected ", (TEXT(uidStr)).data), UI_OK,
            false);

  CardType ct = detectCardType(uid, uidLength);
  if (ct != MIFARE_CLASSIC) {
    rfidPresentResult("Decode Access", "Skipped",
                      "Only MIFARE Classic sector trailers.", "RESULT", NULL);
    return;
  }

  uint8_t keyA[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t keyB[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t blk[16] = {0};
  bool authenticated = false;
  (void)tryMagicBackdoor();
  if (pn532_mifare_classic_authenticate_block(&(s_nfc), uid, uidLength, 7, 0,
                                              keyA)) {
    authenticated = true;
  } else if (pn532_mifare_classic_authenticate_block(&(s_nfc), uid, uidLength,
                                                     7, 1, keyB)) {
    authenticated = true;
  }

  if (!authenticated ||
      !pn532_mifare_classic_read_data_block(&(s_nfc), 7, blk)) {
    rfidResultAndDismiss("Decode Access", "Failed",
                         "Could not read block 7 (sector 1 trailer).");
    return;
  }

  uint8_t a6 = blk[6], a7 = blk[7], a8 = blk[8];
  uint8_t c1 = (uint8_t)((a7 >> 5) & 0x01);
  uint8_t c2 = (uint8_t)((a8 >> 1) & 0x01);
  uint8_t c3 = (uint8_t)((a8 >> 5) & 0x01);
  uint8_t accessCond = (uint8_t)((c1 << 2) | (c2 << 1) | c3);

  char body[420];
  snprintf(body, sizeof(body),
           "UID %s\nSector 1 trailer (block 7):\n"
           "%02X %02X %02X %02X %02X %02X %02X %02X\n"
           "%02X %02X %02X %02X %02X %02X %02X %02X\n\n"
           "Access bytes [6..8]: %02X %02X %02X\n"
           "Condition bits C1..C3 (nibble-style): %u %u %u - code %u\n\n"
           "(Interpret with MFC access-bit tables for your sector.)",
           uidStr, blk[0], blk[1], blk[2], blk[3], blk[4], blk[5], blk[6],
           blk[7], blk[8], blk[9], blk[10], blk[11], blk[12], blk[13], blk[14],
           blk[15], a6, a7, a8, (unsigned)c1, (unsigned)c2, (unsigned)c3,
           (unsigned)accessCond);
  rfidResultAndDismiss("Decode Access", "Block 7", body);
}

void RfidNfc_sessionJamReader(void) {
  static const char kIntro[] =
      "Puts the PN532 into ISO14443A target mode and sends rapid replies "
      "toward "
      "an external reader.\n\nKeep antennas close to the reader coil.\n\n"
      "Stop ends the session.";

  rfidAttachBus();
  rfidShellProgress("Jam Reader", "Preparing target mode...", "Stop", kIntro);

  uint8_t rx[64];
  uint8_t rxLen = sizeof(rx);
  if (!RfidNfc_rfidTargetWaitReader("Waiting for reader",
                                    RFID_TARGET_WAIT_TIMEOUT_MS, rx, &rxLen,
                                    true)) {
    return;
  }

  rfidSetDynamicPill("JAM", UI_WARN);
  rfidSetBandAux(NULL);
  rfidDynamicBand(0, "Jam active", "Flooding external reader");
  rfidPrint(TEXT("[+] Reader detected"), UI_OK, false);

  static const uint8_t kJamResp[] = {0xFF, 0xFF, 0xFF, 0xFF};
  unsigned long lastUiMs = 0;
  bool pendingRx = true;

  for (;;) {
    if (rfidPumpSessionUi()) {
      break;
    }
    if (rfidPollCancel()) {
      break;
    }

    if (pendingRx) {
      if (!RfidNfc_rfidTargetSendResponse(kJamResp, sizeof(kJamResp))) {
        rfidDynamicBand(0, "Jam active", "Waiting for reader...");
        pendingRx = RfidNfc_rfidTargetWaitReader("Waiting for reader", 0, rx,
                                                 &rxLen, false);
        if (!pendingRx) {
          break;
        }
        rfidPrint(TEXT("[+] Reader reconnected"), UI_OK, false);
        continue;
      }
      pendingRx = false;
    }

    rxLen = sizeof(rx);
    if (pn532_get_data_target(&(s_nfc), rx, &rxLen)) {
      pendingRx = true;
      const unsigned long now = sys_monotonic_ms();
      if (now - lastUiMs >= 200) {
        rfidDynamicBand(0, "Jam active", "Reader RF collision / flood");
        lastUiMs = now;
      }
    } else {
      const unsigned long now = sys_monotonic_ms();
      if (now - lastUiMs >= 200) {
        rfidDynamicBand(0, "Jam active", "Waiting for reader...");
        lastUiMs = now;
      }
      pendingRx = RfidNfc_rfidTargetWaitReader("Waiting for reader", 0, rx,
                                               &rxLen, false);
      if (!pendingRx) {
        break;
      }
      rfidPrint(TEXT("[+] Reader reconnected"), UI_OK, false);
    }
    sys_sleep_ms(5);
  }

  pn532_sam_config(&(s_nfc));
  pn532_set_passive_activation_retries(&(s_nfc), 0xFF);
  if (rfidSessionExitRequested()) {
    return;
  }
  rfidResultAndDismiss("Jam Reader", "Stopped",
                       "SAM reconfigured; reader mode restored.");
}

void RfidNfc_sessionDisruptEmulate(void) {
  static const char kIntro[] =
      "Target mode: cycles payloads (FF / 00 / random) on each exchange.\n"
      "Use only on gear you own or are permitted to test.\n\n"
      "Stop ends the session.";

  rfidAttachBus();
  rfidShellProgress("Disrupt Emulate", "Preparing target mode...", "Stop",
                    kIntro);

  uint8_t rx[64];
  uint8_t rxLen = sizeof(rx);
  if (!RfidNfc_rfidTargetWaitReader("Waiting for reader",
                                    RFID_TARGET_WAIT_TIMEOUT_MS, rx, &rxLen,
                                    true)) {
    return;
  }

  rfidSetDynamicPill("TX", UI_WARN);
  rfidSetBandAux(NULL);
  rfidDynamicBand(0, "Disrupt active", "FF / 00 / random payloads");
  rfidPrint(TEXT("[+] Reader detected"), UI_OK, false);

  uint8_t txPayload[16];
  uint8_t cycle = 0;
  unsigned long lastUiMs = 0;
  bool pendingRx = true;

  for (;;) {
    if (rfidPumpSessionUi()) {
      break;
    }
    if (rfidPollCancel()) {
      break;
    }

    if (pendingRx) {
      if (cycle % 3 == 0) {
        memset(txPayload, 0xFF, sizeof(txPayload));
      } else if (cycle % 3 == 1) {
        memset(txPayload, 0x00, sizeof(txPayload));
      } else {
        for (unsigned i = 0; i < sizeof(txPayload); i++) {
          txPayload[i] = (uint8_t)(sys_random_range_1(256));
        }
      }
      cycle++;

      if (!RfidNfc_rfidTargetSendResponse(txPayload, sizeof(txPayload))) {
        rfidDynamicBand(0, "Disrupt active", "Waiting for reader...");
        pendingRx = RfidNfc_rfidTargetWaitReader("Waiting for reader", 0, rx,
                                                 &rxLen, false);
        if (!pendingRx) {
          break;
        }
        rfidPrint(TEXT("[+] Reader reconnected"), UI_OK, false);
        continue;
      }
      pendingRx = false;
    }

    rxLen = sizeof(rx);
    if (pn532_get_data_target(&(s_nfc), rx, &rxLen)) {
      pendingRx = true;
      const unsigned long now = sys_monotonic_ms();
      if (now - lastUiMs >= 200) {
        rfidDynamicBand(0, "Disrupt active", "FF / 00 / random payloads");
        lastUiMs = now;
      }
    } else {
      const unsigned long now = sys_monotonic_ms();
      if (now - lastUiMs >= 200) {
        rfidDynamicBand(0, "Disrupt active", "Waiting for reader...");
        lastUiMs = now;
      }
      pendingRx = RfidNfc_rfidTargetWaitReader("Waiting for reader", 0, rx,
                                               &rxLen, false);
      if (!pendingRx) {
        break;
      }
      rfidPrint(TEXT("[+] Reader reconnected"), UI_OK, false);
    }
    sys_sleep_ms(5);
  }

  pn532_sam_config(&(s_nfc));
  pn532_set_passive_activation_retries(&(s_nfc), 0xFF);
  if (rfidSessionExitRequested()) {
    return;
  }
  rfidResultAndDismiss("Disrupt Emulate", "Stopped", "SAM reconfigured.");
}

void RfidNfc_sessionCardReader(void) {
  static const char kIntro[] =
      "Passive ISO14443A read.\n\n"
      "Hold the tag flat on the coil. UID, tag type, and memory samples appear "
      "here after scan.";

  uint8_t uid[7] = {0};
  uint8_t uidLength = 0;
  rfidAttachBus();

  if (!RfidNfc_rfidListenIso14443a("Card Reader", "Cancel", uid, &uidLength,
                                   (unsigned long)RFID_TAG_LISTEN_TIMEOUT_MS,
                                   "Listening - present ISO14443A tag",
                                   kIntro)) {
    return;
  }

  char uidStr[24] = "";
  uidToHex(uidStr, sizeof(uidStr), uid, uidLength);
  CardType ct = detectCardType(uid, uidLength);

  char classicBlk[200] = "";
  if (ct == MIFARE_CLASSIC) {
    uint8_t keyA[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t data[16];
    if (pn532_mifare_classic_authenticate_block(&(s_nfc), uid, uidLength, 0, 0,
                                                keyA) &&
        pn532_mifare_classic_read_data_block(&(s_nfc), 0, data)) {
      formatClassicBlock0(classicBlk, sizeof(classicBlk), data);
    } else {
      snprintf(classicBlk, sizeof(classicBlk),
               "Sector 0 block 0 was not readable using default key A (FF FF "
               "FF FF FF FF).");
    }
  }

  char ulSample[240] = "";
  if (ct == MIFARE_ULTRALIGHT || ct == NTAG) {
    uint8_t pg[4][4] = {0};
    bool ok = true;
    for (int pi = 0; pi < 4; pi++) {
      if (!pn532_mifare_ultralight_read_page(&(s_nfc), (uint8_t)(4 + pi),
                                             pg[pi])) {
        ok = false;
      }
    }
    if (ok) {
      snprintf(ulSample, sizeof(ulSample),
               "User memory sample (pages 4-7, 4 bytes each):\n"
               "P4  %02X %02X %02X %02X\n"
               "P5  %02X %02X %02X %02X\n"
               "P6  %02X %02X %02X %02X\n"
               "P7  %02X %02X %02X %02X",
               pg[0][0], pg[0][1], pg[0][2], pg[0][3], pg[1][0], pg[1][1],
               pg[1][2], pg[1][3], pg[2][0], pg[2][1], pg[2][2], pg[2][3],
               pg[3][0], pg[3][1], pg[3][2], pg[3][3]);
    } else {
      snprintf(ulSample, sizeof(ulSample), "Pages 4-7 could not all be read.");
    }
  }

  const char* fw =
      s_pn532VerStr[0] ? s_pn532VerStr : "PN532 (version not cached)";
  char body[640];
  snprintf(body, sizeof(body),
           "Reader IC: %s\n\n"
           "UID (HEX): %s\n"
           "UID length: %u bytes\n"
           "Detected type: %s\n"
           "Type code (internal): %d\n",
           fw, uidStr, (unsigned)uidLength, cardTypeStr(ct), (int)ct);

  if (ct == MIFARE_CLASSIC && classicBlk[0]) {
    size_t len = strlen(body);
    snprintf(body + len, sizeof(body) - len, "\n%s", classicBlk);
  }
  if ((ct == MIFARE_ULTRALIGHT || ct == NTAG) && ulSample[0]) {
    size_t len = strlen(body);
    snprintf(body + len, sizeof(body) - len, "\n\n%s", ulSample);
  }
  if (ct == MIFARE_DESFIRE) {
    size_t len = strlen(body);
    snprintf(
        body + len, sizeof(body) - len,
        "\n\n"
        "DESFire / ISO-DEP-style tag detected.\n"
        "This tool only dumps UID here; full DESFire ops need other APDUs.");
  }
  if (ct == UNKNOWN) {
    size_t len = strlen(body);
    snprintf(body + len, sizeof(body) - len,
             "\n\n"
             "Family could not be determined from quick probes.");
  }

  rfidSetDynamicPill("OK", UI_OK);
  rfidSetBandAux(NULL);
  rfidDynamicBand(0, "Tag detected", uidStr);
  rfidPrint(
      text_join_cstr(
          (text_join_cstr(
               (text_join_cstr("[+] ", (TEXT(cardTypeStr(ct))).data)).data,
               "  "))
              .data,
          (TEXT(uidStr)).data),
      UI_OK, false);
  if (ct == MIFARE_CLASSIC && classicBlk[0]) {
    rfidPrint(TEXT(classicBlk), UI_DIM_TEXT, false);
  }
  (void)body;
  rfidWaitTwoBoxDismiss();
}

static uint8_t RfidNfc_s_srcData[16][3][16];
static uint8_t RfidNfc_s_srcPages[256][4];

void RfidNfc_sessionClone(void) {
  uint8_t uid[7] = {0};
  uint8_t uidLength = 0;
  uint8_t keyA[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t keyB[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t keyZero[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  uint8_t data[16];
  uint8_t srcUidCopy[7];

#if RFID_UID_CLONE
  const char* introFoot =
      "Default keys (A/B) and zero.\nUID byte clone on Gen2 \"magic\" cards "
      "when detected.";
#else
  const char* introFoot =
      "Default keys (A/B) and zero. UID block may stay factory.";
#endif

  char introBody[220];
  snprintf(introBody, sizeof(introBody),
           "Two-tag flow: read source, confirm, then write a blank of the same "
           "type.\n\n%s\n\n"
           "Step 1/4: present the SOURCE tag on the coil.",
           introFoot);

  rfidAttachBus();
  if (!RfidNfc_rfidListenIso14443a("Clone", "Cancel", uid, &uidLength,
                                   (unsigned long)RFID_CLONE_PHASE_TIMEOUT_MS,
                                   "Step 1/4 - Present SOURCE tag",
                                   introBody)) {
    return;
  }

  memcpy(srcUidCopy, uid, uidLength);
  char uidStr[24] = "";
  uidToHex(uidStr, sizeof(uidStr), uid, uidLength);
  rfidSetDynamicPill("OK", UI_OK);
  rfidSetBandAux(NULL);
  rfidDynamicBand(0, "Source tag detected", uidStr);
  rfidPrint(text_join_cstr("[+] Source ", (TEXT(uidStr)).data), UI_OK, false);

  CardType srcType = detectCardType(uid, uidLength);
  if (srcType == UNKNOWN) {
    rfidResultAndDismiss(
        "Clone", "Unsupported",
        "Could not classify tag.\nNeed Classic, Ultralight, or NTAG.");
    return;
  }

  memset(RfidNfc_s_srcData, 0, sizeof(RfidNfc_s_srcData));
  memset(RfidNfc_s_srcPages, 0, sizeof(RfidNfc_s_srcPages));
  bool readOk = true;

  if (srcType == MIFARE_CLASSIC) {
    rfidTransitionToProgress("Clone", "Step 2/4 - Read source", "Cancel",
                             "Reading source Classic data blocks 0-47.");
    for (uint8_t sector = 0; sector < 16 && readOk; sector++) {
      char l1[48];
      char l2[48];
      snprintf(l1, sizeof(l1), "Classic - reading data blocks");
      snprintf(l2, sizeof(l2), "Sector %u / 15", (unsigned)sector);
      rfidDynamicBand(RF_DYN_TOP_PROG, l1, l2);
      char infoLine[64];
      snprintf(infoLine, sizeof(infoLine), "Reading sector %u of 15.",
               (unsigned)sector);
      rfidRefreshInfoPanel("PROGRESS", infoLine);
      if (rfidPollCancel()) {
        return;
      }
      for (uint8_t block = 0; block < 3; block++) {
        uint8_t blockNum = (uint8_t)(sector * 4 + block);
        bool auth = pn532_mifare_classic_authenticate_block(
            &(s_nfc), uid, uidLength, blockNum, 0, keyA);
        if (!auth) {
          auth = pn532_mifare_classic_authenticate_block(
              &(s_nfc), uid, uidLength, blockNum, 1, keyB);
        }
        if (auth) {
          if (pn532_mifare_classic_read_data_block(&(s_nfc), blockNum, data)) {
            memcpy(RfidNfc_s_srcData[sector][block], data, 16);
          } else {
            readOk = false;
          }
        } else {
          readOk = false;
        }
        sys_sleep_ms(18);
      }
    }
  } else if (srcType == MIFARE_ULTRALIGHT || srcType == NTAG) {
    uint8_t maxPages = (srcType == MIFARE_ULTRALIGHT) ? 16u : 36u;
    rfidTransitionToProgress("Clone", "Step 2/4 - Read source", "Cancel",
                             "Reading source Ultralight / NTAG user pages.");
    for (uint8_t page = 4; page < maxPages; page++) {
      char l2[40];
      snprintf(l2, sizeof(l2), "Page %u / %u", (unsigned)page,
               (unsigned)(maxPages - 1));
      rfidDynamicBand(RF_DYN_TOP_PROG, "Ultralight / NTAG - user pages", l2);
      char infoLine[64];
      snprintf(infoLine, sizeof(infoLine), "Reading page %u of %u.",
               (unsigned)page, (unsigned)(maxPages - 1));
      rfidRefreshInfoPanel("PROGRESS", infoLine);
      if (rfidPollCancel()) {
        return;
      }
      if (pn532_mifare_ultralight_read_page(&(s_nfc), page, data)) {
        memcpy(RfidNfc_s_srcPages[page], data, 4);
      } else {
        readOk = false;
      }
      sys_sleep_ms(12);
    }
  }

  if (!readOk) {
    rfidResultAndDismiss(
        "Clone", "Read failed",
        "Some blocks/pages did not read.\nCheck keys or tag damage.");
    return;
  }

  static const char kConfirmInfo[] =
      "Source data is stored in device memory.\n\n"
      "Remove the source tag from the coil, then tap Clone to present a blank "
      "tag "
      "of the same type.\n\n"
      "Back cancels without writing anything.";

  char confirmDetail[56];
  snprintf(confirmDetail, sizeof(confirmDetail), "%s  %s", uidStr,
           cardTypeStr(srcType));
  char confirmLog[128];
  snprintf(confirmLog, sizeof(confirmLog),
           "[+] Source copied (%s, %s).\n[*] Remove source - tap Clone.",
           uidStr, cardTypeStr(srcType));

  if (!rfidRunTwoBoxConfirm("Clone", "Step 3/4 - Confirm clone", confirmDetail,
                            confirmLog, kConfirmInfo, "Clone")) {
    return;
  }

  static const char kBlankInfo[] =
      "Step 4/4: present a blank tag of the same type as the source.\n\n"
      "Writing starts automatically after the blank tag is detected.";

  rfidAttachBus();
  if (!RfidNfc_rfidListenIso14443a("Clone", "Cancel", uid, &uidLength,
                                   (unsigned long)RFID_CLONE_PHASE_TIMEOUT_MS,
                                   "Step 4/4 - Present BLANK tag",
                                   kBlankInfo)) {
    return;
  }

  char blankUidStr[24] = "";
  uidToHex(blankUidStr, sizeof(blankUidStr), uid, uidLength);
  rfidSetDynamicPill("OK", UI_OK);
  rfidSetBandAux(NULL);
  rfidDynamicBand(0, "Blank tag detected", blankUidStr);
  rfidPrint(text_join_cstr("[+] Blank ", (TEXT(blankUidStr)).data), UI_OK,
            false);

  CardType blankType = detectCardType(uid, uidLength);
  if (blankType != srcType) {
    rfidResultAndDismiss("Clone", "Type mismatch",
                         "Blank must match source family.");
    return;
  }

  bool cloneOk = true;
  int blocksDone = 0;
#if RFID_UID_CLONE
  bool gen2MagicDetected = false;
#endif

  if (blankType == MIFARE_CLASSIC) {
    bool magic = tryMagicBackdoor();
#if RFID_UID_CLONE
    gen2MagicDetected = magic;
#else
    (void)magic;
#endif
    bool uidWritten = false;
#if RFID_UID_CLONE
    if (magic) {
      bool ok = pn532_mifare_classic_authenticate_block(&(s_nfc), uid,
                                                        uidLength, 0, 0, keyA);
      if (!ok) {
        ok = pn532_mifare_classic_authenticate_block(&(s_nfc), uid, uidLength,
                                                     0, 1, keyB);
      }
      if (!ok) {
        ok = pn532_mifare_classic_authenticate_block(&(s_nfc), uid, uidLength,
                                                     0, 0, keyZero);
      }
      if (ok) {
        uint8_t block0Data[16];
        memcpy(block0Data, srcUidCopy, uidLength);
        block0Data[4] = (uint8_t)(block0Data[0] ^ block0Data[1] ^
                                  block0Data[2] ^ block0Data[3]);
        memcpy(&block0Data[5], &RfidNfc_s_srcData[0][0][5], 11);
        if (pn532_mifare_classic_write_data_block(&(s_nfc), 0, block0Data)) {
          blocksDone++;
          uidWritten = true;
        } else {
          cloneOk = false;
        }
      } else {
        cloneOk = false;
      }
    }
#else
    (void)magic;
    (void)uidWritten;
#endif
    rfidTransitionToProgress("Clone", "Writing - Classic", "Cancel",
                             "Writing copied Classic blocks to blank tag.");
    for (uint8_t sector = 0; sector < 16; sector++) {
      char l2[40];
      snprintf(l2, sizeof(l2), "Sector %u / 15", (unsigned)sector);
      rfidDynamicBand(RF_DYN_TOP_PROG, "Writing copied blocks", l2);
      char infoLine[64];
      snprintf(infoLine, sizeof(infoLine), "Writing sector %u of 15.",
               (unsigned)sector);
      rfidRefreshInfoPanel("PROGRESS", infoLine);
      if (rfidPollCancel()) {
        if (!rfidSessionExitRequested()) {
          rfidResultAndDismiss("Clone", "Stopped", "Clone aborted mid-write.");
        }
        return;
      }
      for (uint8_t block = 0; block < 3; block++) {
        uint8_t blockNum = (uint8_t)(sector * 4 + block);
#if RFID_UID_CLONE
        if (magic && uidWritten && sector == 0 && block == 0) {
          continue;
        }
#endif
        bool auth = pn532_mifare_classic_authenticate_block(
            &(s_nfc), uid, uidLength, blockNum, 0, keyA);
        if (!auth) {
          auth = pn532_mifare_classic_authenticate_block(
              &(s_nfc), uid, uidLength, blockNum, 1, keyB);
        }
        if (!auth) {
          auth = pn532_mifare_classic_authenticate_block(
              &(s_nfc), uid, uidLength, blockNum, 0, keyZero);
        }
        if (auth) {
          if (pn532_mifare_classic_write_data_block(
                  &(s_nfc), blockNum, RfidNfc_s_srcData[sector][block])) {
            blocksDone++;
          } else {
            cloneOk = false;
          }
        } else {
          cloneOk = false;
        }
        sys_sleep_ms(28);
      }
    }
  } else if (blankType == MIFARE_ULTRALIGHT || blankType == NTAG) {
    uint8_t maxPages = (blankType == MIFARE_ULTRALIGHT) ? 16u : 36u;
    rfidTransitionToProgress("Clone", "Writing - Ultralight / NTAG", "Cancel",
                             "Writing copied pages to blank tag.");
    for (uint8_t page = 4; page < maxPages; page++) {
      char l2[40];
      snprintf(l2, sizeof(l2), "Page %u / %u", (unsigned)page,
               (unsigned)(maxPages - 1));
      rfidDynamicBand(RF_DYN_TOP_PROG, "Writing copied pages", l2);
      char infoLine[64];
      snprintf(infoLine, sizeof(infoLine), "Writing page %u of %u.",
               (unsigned)page, (unsigned)(maxPages - 1));
      rfidRefreshInfoPanel("PROGRESS", infoLine);
      if (rfidPollCancel()) {
        if (!rfidSessionExitRequested()) {
          rfidResultAndDismiss("Clone", "Stopped", "Clone aborted mid-write.");
        }
        return;
      }
      if (pn532_mifare_ultralight_write_page(&(s_nfc), page,
                                             RfidNfc_s_srcPages[page])) {
        blocksDone++;
      } else {
        cloneOk = false;
      }
      sys_sleep_ms(18);
    }
  }

  char summary[512];
  const char* fw = s_pn532VerStr[0] ? s_pn532VerStr : "PN532";
#if RFID_UID_CLONE
  snprintf(
      summary, sizeof(summary),
      "%s\n\n"
      "Source UID: %s\n"
      "Blank UID:    %s\n"
      "Tag type:     %s\n"
      "Reader IC:    %s\n\n"
      "Approx. write ops OK: %d\n"
      "Gen2 magic UID path: %s",
      cloneOk ? "Clone finished." : "Clone finished with some errors.", uidStr,
      blankUidStr, cardTypeStr(srcType), fw, blocksDone,
      gen2MagicDetected ? "detected (UID block attempted)" : "not detected");
#else
  snprintf(summary, sizeof(summary),
           "%s\n\n"
           "Source UID: %s\n"
           "Blank UID:    %s\n"
           "Tag type:     %s\n"
           "Reader IC:    %s\n\n"
           "Approx. write ops OK: %d",
           cloneOk ? "Clone finished." : "Clone finished with some errors.",
           uidStr, blankUidStr, cardTypeStr(srcType), fw, blocksDone);
#endif

  rfidResultAndDismiss("Clone", "Done", summary);
}

void RfidNfc_sessionTagDisrupt(void) {
  static const char kIntro[] =
      "Writes sector trailer blocks on MIFARE Classic (default key A).\n"
      "Bad access bits can brick a tag.\n\n"
      "Only use tags you own or are allowed to modify.\n\n"
      "Present a Classic tag (~12 s window).";

  uint8_t uid[7] = {0};
  uint8_t uidLength = 0;
  uint8_t maliciousData[16];
  uint8_t key[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  static const uint8_t sectorTrailers[] = {3,  7,  11, 15, 19, 23, 27, 31,
                                           35, 39, 43, 47, 51, 55, 59, 63};
  bool any = false;
  int trailersOk = 0;
  int authFail = 0;

  rfidAttachBus();
  if (!RfidNfc_rfidListenIso14443a(
          "Tag Disrupt", "Cancel", uid, &uidLength, 12000UL,
          "Classic tag - hold on coil (~12 s window)", kIntro)) {
    return;
  }

  CardType ct = detectCardType(uid, uidLength);
  char uidStr[24] = "";
  uidToHex(uidStr, sizeof(uidStr), uid, uidLength);
  rfidSetDynamicPill("OK", UI_OK);
  rfidSetBandAux(NULL);
  rfidDynamicBand(0, "Tag detected", uidStr);
  rfidPrint(text_join_cstr("[+] Tag detected ", (TEXT(uidStr)).data), UI_OK,
            false);

  if (ct != MIFARE_CLASSIC) {
    char msg[120];
    snprintf(msg, sizeof(msg),
             "Tag types as %s.\nDisrupt targets Classic trailers only.",
             cardTypeStr(ct));
    rfidResultAndDismiss("Tag Disrupt", "Skipped", msg);
    return;
  }

  rfidTransitionToProgress(
      "Tag Disrupt", "Writing sector trailers", "Cancel",
      "Rotating trailer write patterns.\n16 sector trailers targeted.");
  rfidSetDynamicPill("WRITE", UI_WARN);
  for (uint8_t i = 0; i < 16; i++) {
    uint8_t block = sectorTrailers[i];
    char l2[48];
    snprintf(l2, sizeof(l2), "Trailer block %u - sector %u", (unsigned)block,
             (unsigned)i);
    rfidDynamicBand(RF_DYN_TOP_PROG, "Rotating trailer pattern", l2);
    char infoLine[72];
    snprintf(infoLine, sizeof(infoLine),
             "Writing trailer block %u (sector %u).", (unsigned)block,
             (unsigned)i);
    rfidRefreshInfoPanel("PROGRESS", infoLine);
    if (rfidPollCancel()) {
      if (!rfidSessionExitRequested()) {
        rfidResultAndDismiss("Tag Disrupt", "Stopped", "Aborted mid-write.");
      }
      return;
    }

    if (i % 3 == 0) {
      memset(maliciousData, 0xFF, 16);
    } else if (i % 3 == 1) {
      memset(maliciousData, 0x00, 16);
    } else {
      for (uint8_t j = 0; j < 16; j++) {
        maliciousData[j] = (uint8_t)sys_random_range_1(256);
      }
    }
    maliciousData[6] = 0x00;
    maliciousData[7] = 0x00;
    maliciousData[8] = 0x00;

    if (!pn532_mifare_classic_authenticate_block(&(s_nfc), uid, uidLength,
                                                 block, 0, key)) {
      authFail++;
    } else if (pn532_mifare_classic_write_data_block(&(s_nfc), block,
                                                     maliciousData)) {
      any = true;
      trailersOk++;
    }
    sys_sleep_ms(35);
  }

  char msg[320];
  const char* fw = s_pn532VerStr[0] ? s_pn532VerStr : "PN532";
  snprintf(msg, sizeof(msg),
           "Tag UID: %s\n"
           "Reader IC: %s\n\n"
           "Sector trailers touched: %d / 16\n"
           "Key A auth failures:    %d\n\n"
           "%s\n\n"
           "Patterns alternate FF / 00 / random on trailers.\n"
           "Access bits may brick the tag.",
           uidStr, fw, trailersOk, authFail,
           any ? "At least one trailer write succeeded."
               : "No trailer writes succeeded.");

  rfidResultAndDismiss("Tag Disrupt", any ? "Finished" : "No writes", msg);
}
