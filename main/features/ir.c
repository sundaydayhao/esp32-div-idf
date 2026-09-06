#include "ir.h"

#include "feature_text.h"
static int ir_tick_pixels(unsigned width, uint32_t window, uint32_t tick) {
  return tick && window ? (int)((uint64_t)tick * width / window) : 9999;
}
#include <cJSON.h>
#include <ir_recv.h>
#include <ir_send.h>
#include <ir_utils.h>

#include "c_vector.h"
#include "icon.h"
#include "ir_protocol.h"
#include "keyboard_ui.h"
#include "shared.h"
#include "storage_file.h"
#include "text_format.h"
#include "touch_ui.h"
#include "ui_utils.h"

static int irScreenH(void) { return display_height(&(tft)); }

static int irContentBottom(void) {
  return featureHasTouchNavBar() ? touchNavContentBottomY() : irScreenH();
}

static void irClearBody(uint16_t color) {
  if (featureHasTouchNavBar()) {
    featureClearContent(color);
  } else {
    display_fill_screen(&(tft), color);
  }
}

static void irClearContentFrom(int topY, uint16_t color) {
  const int bottom = irContentBottom();
  if (bottom > topY) {
    display_fill_rect(&(tft), 0, topY, 240, bottom - topY, color);
  }
}

static void irRedrawNavChrome(void) {
  redrawTouchButtonBar();
  maintainTouchNavBar();
}

static void irSetCaptureNavLabels(void) {
  setTouchNavLabels("Rep-", "Save", "Exit", "Send", "Rep+");
}

static void irSetSavedNavLabels(void) {
  setTouchNavLabels("Delete", "Next", "Exit", "Prev", "TX");
}

static void irSetUniversalRemoteNavLabels(void) {
  setTouchNavLabels("Prev", "Browse", "Exit", "Reload", "Next");
}

static void irSetUniversalBrowseNavLabels(void) {
  setTouchNavLabels("Back", "Next", "Exit", "Prev", "Select");
}

enum { IRRemoteFeature_kRecvPin = IR_RX_PIN };
enum { IRRemoteFeature_kSendPin = IR_TX_PIN };
enum { IRRemoteFeature_kKhz = IR_DEFAULT_KHZ };

enum { IRRemoteFeature_kToolbarY = 20 };
enum { IRRemoteFeature_kToolbarH = 16 };
enum { IRRemoteFeature_kIconSize = 16 };

enum { IRRemoteFeature_kIrToolbarBottom = 36 };
enum { IRRemoteFeature_kIrToolbarGap = 8 };
enum { IRRemoteFeature_kIrBoxHeaderH = 15 };
enum {
  IRRemoteFeature_kIrStatusY = IRRemoteFeature_kIrToolbarBottom +
  IRRemoteFeature_kIrToolbarGap
};
enum { IRRemoteFeature_kIrStatusBoxH = 91 };
enum { IRRemoteFeature_kIrLogGap = 4 };
enum { IRRemoteFeature_kIrLogBoxH = 49 };
enum {
  IRRemoteFeature_kIrLogBoxTop = IRRemoteFeature_kIrStatusY +
  IRRemoteFeature_kIrStatusBoxH + IRRemoteFeature_kIrLogGap
};
enum {
  IRRemoteFeature_kIrLogStartY = IRRemoteFeature_kIrLogBoxTop +
  IRRemoteFeature_kIrBoxHeaderH
};
enum {
  IRRemoteFeature_kIrLogEndY = IRRemoteFeature_kIrLogBoxTop +
  IRRemoteFeature_kIrLogBoxH - 2
};
enum {
  IRRemoteFeature_kIrGraphTop = IRRemoteFeature_kIrLogBoxTop +
  IRRemoteFeature_kIrLogBoxH + 4
};
enum { IRRemoteFeature_kIrGraphMarginX = 6 };
enum { IRRemoteFeature_kIrLineHeight = 12 };
enum { IRRemoteFeature_kIrStatusLineCount = 6 };
enum {
  IRRemoteFeature_kIrStatusTextY = IRRemoteFeature_kIrStatusY +
  IRRemoteFeature_kIrBoxHeaderH
};
enum { IRRemoteFeature_kIrMaxLogLines = 48 };
enum { IRRemoteFeature_kIrPlotBg = 0x0842 };
enum { IRRemoteFeature_kIrGridColor = 0x2945 };
enum { IRRemoteFeature_kIrGridDivisions = 4 };

typedef struct IRRemoteFeature_IrPlotLayout {
  int graphTop;
  int axisX;
  int plotRight;
  int plotTop;
  int plotBottom;
  int plotHeight;
  int plotWidth;
  bool valid;
} IRRemoteFeature_IrPlotLayout;
#define IRRemoteFeature_IrPlotLayout_INITIALIZER \
  {.graphTop = 0,                                \
   .axisX = 0,                                   \
   .plotRight = 0,                               \
   .plotTop = 0,                                 \
   .plotBottom = 0,                              \
   .plotHeight = 0,                              \
   .plotWidth = 0,                               \
   .valid = false}
;

static IRRemoteFeature_IrPlotLayout IRRemoteFeature_s_irPlot =
    IRRemoteFeature_IrPlotLayout_INITIALIZER;
static bool IRRemoteFeature_s_irBoxesDrawn = false;
static bool IRRemoteFeature_s_irGraphChromeDrawn = false;
enum {
  IRRemoteFeature_s_irLogBuffer_COUNT = IRRemoteFeature_kIrMaxLogLines,
  IRRemoteFeature_s_irLogBuffer_OFFSET = 0
};
_Static_assert(IRRemoteFeature_s_irLogBuffer_OFFSET +
                       IRRemoteFeature_s_irLogBuffer_COUNT <=
                   FEATURE_TEXT_SLOTS,
               "Feature text capacity");
#define IRRemoteFeature_s_irLogBuffer                  \
  (*(text_t(*)[IRRemoteFeature_s_irLogBuffer_COUNT]) & \
   feature_text_slots[IRRemoteFeature_s_irLogBuffer_OFFSET])
static uint16_t IRRemoteFeature_s_irLogColor[IRRemoteFeature_kIrMaxLogLines];
static int IRRemoteFeature_s_irLogIndex = 0;
enum {
  IRRemoteFeature_s_irStatusLineText_COUNT = IRRemoteFeature_kIrStatusLineCount,
  IRRemoteFeature_s_irStatusLineText_OFFSET =
          IRRemoteFeature_s_irLogBuffer_OFFSET +
      IRRemoteFeature_s_irLogBuffer_COUNT
};
_Static_assert(IRRemoteFeature_s_irStatusLineText_OFFSET +
                       IRRemoteFeature_s_irStatusLineText_COUNT <=
                   FEATURE_TEXT_SLOTS,
               "Feature text capacity");
#define IRRemoteFeature_s_irStatusLineText                  \
  (*(text_t(*)[IRRemoteFeature_s_irStatusLineText_COUNT]) & \
   feature_text_slots[IRRemoteFeature_s_irStatusLineText_OFFSET])
static uint16_t
    IRRemoteFeature_s_irStatusLineColor[IRRemoteFeature_kIrStatusLineCount];
static bool IRRemoteFeature_s_irStatusStaticDrawn = false;
static uint32_t IRRemoteFeature_s_irLastStatusDrawMs = 0;

enum { IRRemoteFeature_kMaxRawLen = 512 };
enum { IRRemoteFeature_kRawTick = 50 };

static ir_receiver_t IRRemoteFeature_s_recv = {.recvpin_ =
                                                   IRRemoteFeature_kRecvPin};
static ir_transmitter_t IRRemoteFeature_s_send = {
    .sendpin_ = IRRemoteFeature_kSendPin, .khz_ = 38};
static IrFrame IRRemoteFeature_s_results;

static bool IRRemoteFeature_s_hasCapture = false;

static uint16_t IRRemoteFeature_s_raw[IRRemoteFeature_kMaxRawLen];
static uint16_t IRRemoteFeature_s_rawLen = 0;

static decode_type_t IRRemoteFeature_s_decodeType = UNKNOWN;
static uint64_t IRRemoteFeature_s_value = 0;
static uint16_t IRRemoteFeature_s_bits = 0;

static uint8_t IRRemoteFeature_s_repeat = 1;
static bool IRRemoteFeature_s_autoTx = false;
static uint32_t IRRemoteFeature_s_autoIntervalMs = 450;
static uint32_t IRRemoteFeature_s_lastAutoMs = 0;

static bool IRRemoteFeature_s_uiDrawn = false;
static bool IRRemoteFeature_s_contentDirty = true;
static uint32_t IRRemoteFeature_s_lastActionMs = 0;
static bool IRRemoteFeature_s_captureUiDirty = false;
static uint32_t IRRemoteFeature_s_lastCaptureUiMs = 0;
static uint32_t IRRemoteFeature_s_lastWaveHash = 0;
static uint32_t IRRemoteFeature_s_lastStableHash = 0;

static bool IRRemoteFeature_s_hasParsedKey = false;
static uint64_t IRRemoteFeature_s_keyAddr = 0;
static uint64_t IRRemoteFeature_s_keyCmd = 0;
static text_t IRRemoteFeature_s_keyText = {.data = ""};

static uint8_t IRRemoteFeature_s_waveZoomIdx = 0;
static uint32_t IRRemoteFeature_s_wavePanUs = 0;
static const uint32_t IRRemoteFeature_kWaveZoomUs[] = {0, 100000, 50000, 20000,
                                                       10000};
enum { IRRemoteFeature_kWaveZoomCount = 5 };

static uint8_t IRRemoteFeature_s_scope[240] = {0};
static uint16_t IRRemoteFeature_s_scopePos = 0;
static uint32_t IRRemoteFeature_s_lastScopeSampleUs = 0;
static uint32_t IRRemoteFeature_s_lastScopeDrawMs = 0;

static uint32_t IRRemoteFeature_waveformWindowUs(uint32_t totalUs) {
  const uint32_t w =
      IRRemoteFeature_kWaveZoomUs[IRRemoteFeature_s_waveZoomIdx %
                                  IRRemoteFeature_kWaveZoomCount];
  if (w == 0) return totalUs;
  return (totalUs < w) ? totalUs : w;
}

static const char* IRRemoteFeature_zoomLabel(void) {
  switch (IRRemoteFeature_s_waveZoomIdx % IRRemoteFeature_kWaveZoomCount) {
    case 0:
      return "FIT";
    case 1:
      return "100ms";
    case 2:
      return "50ms";
    case 3:
      return "20ms";
    default:
      return "10ms";
  }
}

static uint32_t IRRemoteFeature_irWaveTotalUs(void) {
  uint32_t total = 0;
  for (uint16_t i = 0; i < IRRemoteFeature_s_rawLen; i++) {
    total += IRRemoteFeature_s_raw[i];
  }
  return total;
}

static void IRRemoteFeature_irWaveClampPan(void) {
  const uint32_t total = IRRemoteFeature_irWaveTotalUs();
  const uint32_t window = IRRemoteFeature_waveformWindowUs(total);
  if (window == 0 || window >= total) {
    IRRemoteFeature_s_wavePanUs = 0;
    return;
  }
  const uint32_t maxPan = total - window;
  if (IRRemoteFeature_s_wavePanUs > maxPan) {
    IRRemoteFeature_s_wavePanUs = maxPan;
  }
}

static text_t IRRemoteFeature_irGraphTitle(void) {
  if (!IRRemoteFeature_s_hasCapture) {
    return TEXT("Live IR Scope");
  }
  text_t title =
      text_join_cstr("Waveform ", (TEXT(IRRemoteFeature_zoomLabel())).data);
  if (IRRemoteFeature_s_waveZoomIdx != 0 && IRRemoteFeature_s_wavePanUs > 0) {
    text_append_cstr(
        &(title),
        (text_join_cstr(
             (text_join_cstr(" @+",
                             (TEXT(IRRemoteFeature_s_wavePanUs / 1000)).data))
                 .data,
             "ms"))
            .data);
  }
  return title;
}

static void IRRemoteFeature_irDrawGraphChrome(void);
static void IRRemoteFeature_redrawWaveformPlotOnly(void);
static void IRRemoteFeature_irPrint(text_t text, uint16_t color,
                                    bool extraSpace);
static void IRRemoteFeature_irEnsurePlotLayout(void);
static void IRRemoteFeature_redrawGraphTitleOnly(bool captured);

static void IRRemoteFeature_irGraphRefreshWaveform(void) {
  if (!IRRemoteFeature_s_irPlot.valid) {
    IRRemoteFeature_irEnsurePlotLayout();
  }
  IRRemoteFeature_redrawGraphTitleOnly(true);
  IRRemoteFeature_redrawWaveformPlotOnly();
}

static void IRRemoteFeature_irWaveZoomIn(void) {
  if (!IRRemoteFeature_s_hasCapture) {
    return;
  }
  if (IRRemoteFeature_s_waveZoomIdx + 1 < IRRemoteFeature_kWaveZoomCount) {
    IRRemoteFeature_s_waveZoomIdx++;
  }
  IRRemoteFeature_irWaveClampPan();
  IRRemoteFeature_irGraphRefreshWaveform();
}

static void IRRemoteFeature_irWaveZoomOut(void) {
  if (!IRRemoteFeature_s_hasCapture) {
    return;
  }
  if (IRRemoteFeature_s_waveZoomIdx > 0) {
    IRRemoteFeature_s_waveZoomIdx--;
  }
  if (IRRemoteFeature_s_waveZoomIdx == 0) {
    IRRemoteFeature_s_wavePanUs = 0;
  }
  IRRemoteFeature_irWaveClampPan();
  IRRemoteFeature_irGraphRefreshWaveform();
}

static void IRRemoteFeature_irWaveCycleZoom(void) {
  if (!IRRemoteFeature_s_hasCapture) {
    return;
  }
  IRRemoteFeature_s_waveZoomIdx =
      (uint8_t)((IRRemoteFeature_s_waveZoomIdx + 1) %
                IRRemoteFeature_kWaveZoomCount);
  if (IRRemoteFeature_s_waveZoomIdx == 0) {
    IRRemoteFeature_s_wavePanUs = 0;
  }
  IRRemoteFeature_irWaveClampPan();
  IRRemoteFeature_irGraphRefreshWaveform();
}

static void IRRemoteFeature_irWavePanBy(int32_t deltaUs) {
  if (!IRRemoteFeature_s_hasCapture || IRRemoteFeature_s_waveZoomIdx == 0) {
    return;
  }
  const uint32_t total = IRRemoteFeature_irWaveTotalUs();
  const uint32_t window = IRRemoteFeature_waveformWindowUs(total);
  if (window == 0 || window >= total) {
    return;
  }
  const uint32_t maxPan = total - window;
  const int64_t next = (int64_t)IRRemoteFeature_s_wavePanUs + deltaUs;
  if (next <= 0) {
    IRRemoteFeature_s_wavePanUs = 0;
  } else if ((uint32_t)next >= maxPan) {
    IRRemoteFeature_s_wavePanUs = maxPan;
  } else {
    IRRemoteFeature_s_wavePanUs = (uint32_t)next;
  }
  IRRemoteFeature_irGraphRefreshWaveform();
}

static void IRRemoteFeature_irHandleGraphTouch(int x, int y) {
  if (!IRRemoteFeature_s_hasCapture || !IRRemoteFeature_s_irPlot.valid) {
    return;
  }
  if (x < IRRemoteFeature_s_irPlot.axisX ||
      x > IRRemoteFeature_s_irPlot.plotRight ||
      y < IRRemoteFeature_s_irPlot.plotTop ||
      y > IRRemoteFeature_s_irPlot.plotBottom) {
    return;
  }
  const uint32_t now = sys_monotonic_ms();
  if (now - IRRemoteFeature_s_lastActionMs < 220) {
    return;
  }

  const int plotW =
      IRRemoteFeature_s_irPlot.plotRight - IRRemoteFeature_s_irPlot.axisX;
  const int relX = x - IRRemoteFeature_s_irPlot.axisX;
  const uint32_t total = IRRemoteFeature_irWaveTotalUs();
  const uint32_t window = IRRemoteFeature_waveformWindowUs(total);
  const uint32_t panStep = (window / 4 > 1000U) ? (window / 4) : 1000U;

  if (relX < plotW / 3) {
    IRRemoteFeature_irWavePanBy(-(int32_t)panStep);
    IRRemoteFeature_irPrint(TEXT("[*] Graph pan -"), UI_DIM_TEXT, false);
  } else if (relX > (plotW * 2) / 3) {
    IRRemoteFeature_irWavePanBy((int32_t)panStep);
    IRRemoteFeature_irPrint(TEXT("[*] Graph pan +"), UI_DIM_TEXT, false);
  } else {
    IRRemoteFeature_irWaveCycleZoom();
    IRRemoteFeature_irPrint(
        text_join_cstr("[*] Zoom ", (TEXT(IRRemoteFeature_zoomLabel())).data),
        UI_DIM_TEXT, false);
  }
  IRRemoteFeature_s_lastActionMs = now;
}

static void IRRemoteFeature_irEnsurePlotLayout(void) {
  const int screenW = display_width(&(tft));
  IRRemoteFeature_s_irPlot.graphTop = IRRemoteFeature_kIrGraphTop;
  IRRemoteFeature_s_irPlot.axisX = IRRemoteFeature_kIrGraphMarginX;
  IRRemoteFeature_s_irPlot.plotRight =
      screenW - IRRemoteFeature_kIrGraphMarginX;
  IRRemoteFeature_s_irPlot.plotTop = IRRemoteFeature_s_irPlot.graphTop + 12;
  IRRemoteFeature_s_irPlot.plotBottom = irContentBottom() - 13;
  IRRemoteFeature_s_irPlot.plotHeight =
      IRRemoteFeature_s_irPlot.plotBottom - IRRemoteFeature_s_irPlot.plotTop;
  IRRemoteFeature_s_irPlot.plotWidth =
      IRRemoteFeature_s_irPlot.plotRight - IRRemoteFeature_s_irPlot.axisX;
  IRRemoteFeature_s_irPlot.valid = IRRemoteFeature_s_irPlot.plotWidth >= 32 &&
                                   IRRemoteFeature_s_irPlot.plotHeight >= 10;
}

static text_t IRRemoteFeature_irFitStatusText(text_t text) {
  const int maxWidth = display_width(&(tft)) - 16;
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

static void IRRemoteFeature_irScrollLog(void) {
  for (int i = 0; i < IRRemoteFeature_kIrMaxLogLines - 1; i++) {
    IRRemoteFeature_s_irLogBuffer[i] = IRRemoteFeature_s_irLogBuffer[i + 1];
    IRRemoteFeature_s_irLogColor[i] = IRRemoteFeature_s_irLogColor[i + 1];
  }
}

static int IRRemoteFeature_irLogVisibleLines(void) {
  const int h = IRRemoteFeature_kIrLogEndY - IRRemoteFeature_kIrLogStartY;
  if (h <= 0 || IRRemoteFeature_kIrLineHeight <= 0) {
    return 1;
  }
  return h / IRRemoteFeature_kIrLineHeight;
}

static void IRRemoteFeature_irRedrawActivityLog(void) {
  const int visible = IRRemoteFeature_irLogVisibleLines();
  display_fill_rect(
      &(tft), 8, IRRemoteFeature_kIrLogStartY, display_width(&(tft)) - 16,
      IRRemoteFeature_kIrLogEndY - IRRemoteFeature_kIrLogStartY, TFT_BLACK);
  if (visible <= 0 || IRRemoteFeature_s_irLogIndex <= 0) {
    return;
  }

  const int start = (IRRemoteFeature_s_irLogIndex > visible)
                        ? (IRRemoteFeature_s_irLogIndex - visible)
                        : 0;
  for (int row = 0; row < visible; row++) {
    const int bufIndex = start + row;
    if (bufIndex >= IRRemoteFeature_s_irLogIndex) {
      break;
    }
    if (strlen((IRRemoteFeature_s_irLogBuffer[bufIndex]).data) == 0) {
      continue;
    }
    const int yPos =
        IRRemoteFeature_kIrLogStartY + row * IRRemoteFeature_kIrLineHeight;
    display_set_text_size(&(tft), 1);
    display_set_text_color_2(&(tft), IRRemoteFeature_s_irLogColor[bufIndex],
                             TFT_BLACK);
    display_set_cursor(&(tft), 8, yPos);
    display_print(&(tft), (IRRemoteFeature_s_irLogBuffer[bufIndex]).data);
  }
}

static void IRRemoteFeature_irPrint(text_t text, uint16_t color,
                                    bool extraSpace) {
  if (IRRemoteFeature_s_irLogIndex >= IRRemoteFeature_kIrMaxLogLines) {
    IRRemoteFeature_irScrollLog();
    IRRemoteFeature_s_irLogIndex = IRRemoteFeature_kIrMaxLogLines - 1;
  }

  IRRemoteFeature_s_irLogBuffer[IRRemoteFeature_s_irLogIndex] = text;
  IRRemoteFeature_s_irLogColor[IRRemoteFeature_s_irLogIndex] = color;
  IRRemoteFeature_s_irLogIndex++;

  if (extraSpace &&
      IRRemoteFeature_s_irLogIndex < IRRemoteFeature_kIrMaxLogLines) {
    IRRemoteFeature_s_irLogBuffer[IRRemoteFeature_s_irLogIndex] = TEXT("");
    IRRemoteFeature_s_irLogColor[IRRemoteFeature_s_irLogIndex] = TFT_WHITE;
    IRRemoteFeature_s_irLogIndex++;
  }

  IRRemoteFeature_irRedrawActivityLog();
}

static void IRRemoteFeature_irDrawStatusLine(int line, text_t text,
                                             uint16_t color) {
  const int y =
      IRRemoteFeature_kIrStatusTextY + line * IRRemoteFeature_kIrLineHeight;
  text_t fitted = IRRemoteFeature_irFitStatusText(text);
  display_fill_rect(&(tft), 8, y, display_width(&(tft)) - 16,
                    IRRemoteFeature_kIrLineHeight, TFT_BLACK);
  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), color, TFT_BLACK);
  display_set_cursor(&(tft), 8, y);
  display_print(&(tft), (fitted).data);
}

static void IRRemoteFeature_irDrawStatusLineIfChanged(int line, text_t text,
                                                      uint16_t color) {
  if (line < 0 || line >= IRRemoteFeature_kIrStatusLineCount) {
    return;
  }
  if ((strcmp((IRRemoteFeature_s_irStatusLineText[line]).data, (text).data) ==
       0) &&
      IRRemoteFeature_s_irStatusLineColor[line] == color) {
    return;
  }
  IRRemoteFeature_s_irStatusLineText[line] = text;
  IRRemoteFeature_s_irStatusLineColor[line] = color;
  IRRemoteFeature_irDrawStatusLine(line, text, color);
}

static void IRRemoteFeature_irDrawTextBoxes(void) {
  display_fill_rect(
      &(tft), 0, IRRemoteFeature_kIrStatusY - 2, display_width(&(tft)),
      IRRemoteFeature_kIrGraphTop - IRRemoteFeature_kIrStatusY + 2, TFT_BLACK);
  display_draw_fast_h_line(&(tft), 0, 19, display_width(&(tft)), UI_LINE);
  display_draw_round_rect(&(tft), 4, IRRemoteFeature_kIrStatusY,
                          display_width(&(tft)) - 8,
                          IRRemoteFeature_kIrStatusBoxH, 3, UI_LINE);
  display_draw_round_rect(&(tft), 4, IRRemoteFeature_kIrLogBoxTop,
                          display_width(&(tft)) - 8, IRRemoteFeature_kIrLogBoxH,
                          3, UI_LINE);
  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), UI_DIM_TEXT, TFT_BLACK);
  display_draw_string(&(tft), "Signal Status", 8,
                      IRRemoteFeature_kIrStatusY + 3, 0);
  display_draw_string(&(tft), "Activity", 8, IRRemoteFeature_kIrLogBoxTop + 3,
                      0);
  IRRemoteFeature_s_irBoxesDrawn = true;
}

static void IRRemoteFeature_irDrawStaticStatusLines(void) {
  if (IRRemoteFeature_s_irStatusStaticDrawn) {
    return;
  }
  IRRemoteFeature_irDrawStatusLineIfChanged(
      5,
      text_join_cstr(
          (text_join_cstr("Carrier: ", (TEXT(IRRemoteFeature_kKhz)).data)).data,
          " kHz IR"),
      UI_DIM_TEXT);
  IRRemoteFeature_s_irStatusStaticDrawn = true;
}

static void IRRemoteFeature_irUpdateGraphHintLine(void) {
  if (IRRemoteFeature_s_hasCapture) {
    IRRemoteFeature_irDrawStatusLineIfChanged(
        5, TEXT("Graph: +/- zoom  tap L/R pan"), UI_DIM_TEXT);
  } else {
    IRRemoteFeature_s_irStatusLineText[5] = TEXT("");
    IRRemoteFeature_s_irStatusLineColor[5] = 0;
    IRRemoteFeature_irDrawStatusLineIfChanged(
        5,
        text_join_cstr(
            (text_join_cstr("Carrier: ", (TEXT(IRRemoteFeature_kKhz)).data))
                .data,
            " kHz IR"),
        UI_DIM_TEXT);
  }
}

static void IRRemoteFeature_irUpdateStatusPanel(bool force) {
  const uint32_t now = sys_monotonic_ms();
  if (!force && now - IRRemoteFeature_s_irLastStatusDrawMs < 120) {
    return;
  }
  IRRemoteFeature_s_irLastStatusDrawMs = now;

  IRRemoteFeature_irDrawStaticStatusLines();
  IRRemoteFeature_irDrawStatusLineIfChanged(
      4,
      text_join_cstr(
          (text_join_cstr(
               (text_join_cstr("Repeat: ",
                               (TEXT((unsigned)IRRemoteFeature_s_repeat)).data))
                   .data,
               "   Auto: "))
              .data,
          (TEXT(IRRemoteFeature_s_autoTx ? "ON" : "OFF")).data),
      IRRemoteFeature_s_autoTx ? UI_WARN : UI_DIM_TEXT);

  if (!IRRemoteFeature_s_hasCapture) {
    IRRemoteFeature_irDrawStatusLineIfChanged(0, TEXT("State: Listening"),
                                              UI_WARN);
    IRRemoteFeature_irDrawStatusLineIfChanged(1, TEXT("Type: --"), UI_DIM_TEXT);
    IRRemoteFeature_irDrawStatusLineIfChanged(2, TEXT("Key: --"), UI_DIM_TEXT);
    IRRemoteFeature_irDrawStatusLineIfChanged(3, TEXT("Raw: --"), UI_DIM_TEXT);
    IRRemoteFeature_irUpdateGraphHintLine();
    return;
  }

  IRRemoteFeature_irDrawStatusLineIfChanged(0, TEXT("State: Captured"), UI_OK);
  IRRemoteFeature_irDrawStatusLineIfChanged(
      1,
      text_join_cstr("Type: ",
                     (TEXT(typeToString(IRRemoteFeature_s_decodeType))).data),
      UI_TEXT);
  text_t keyLine = IRRemoteFeature_s_keyText;
  if (strlen((keyLine).data) > 28) {
    keyLine = text_slice(keyLine, 0, 28);
  }
  IRRemoteFeature_irDrawStatusLineIfChanged(
      2, text_join_cstr("Key: ", (keyLine).data), UI_TEXT);
  IRRemoteFeature_irDrawStatusLineIfChanged(
      3,
      text_join_cstr(
          (text_join_cstr(
               (text_join_cstr("Raw: ",
                               (TEXT((unsigned)IRRemoteFeature_s_rawLen)).data))
                   .data,
               " pulses  Bits: "))
              .data,
          (TEXT((unsigned)IRRemoteFeature_s_bits)).data),
      UI_TEXT);
  IRRemoteFeature_irUpdateGraphHintLine();
}

static void IRRemoteFeature_irDrawGraphGrid(void) {
  for (int g = 1; g < IRRemoteFeature_kIrGridDivisions; g++) {
    const int gy = IRRemoteFeature_s_irPlot.plotTop +
                   ((IRRemoteFeature_s_irPlot.plotHeight * g) +
                    (IRRemoteFeature_kIrGridDivisions / 2)) /
                       IRRemoteFeature_kIrGridDivisions;
    display_draw_fast_h_line(&(tft), IRRemoteFeature_s_irPlot.axisX + 1, gy,
                             IRRemoteFeature_s_irPlot.plotWidth - 2,
                             IRRemoteFeature_kIrGridColor);
  }
  for (int v = 1; v < IRRemoteFeature_kIrGridDivisions; v++) {
    const int vx = IRRemoteFeature_s_irPlot.axisX +
                   ((IRRemoteFeature_s_irPlot.plotWidth * v) +
                    (IRRemoteFeature_kIrGridDivisions / 2)) /
                       IRRemoteFeature_kIrGridDivisions;
    display_draw_fast_v_line(&(tft), vx, IRRemoteFeature_s_irPlot.plotTop + 1,
                             IRRemoteFeature_s_irPlot.plotHeight - 2,
                             IRRemoteFeature_kIrGridColor);
  }
}

static void IRRemoteFeature_irDrawGraphChrome(void) {
  IRRemoteFeature_irEnsurePlotLayout();
  if (!IRRemoteFeature_s_irPlot.valid) {
    return;
  }

  const int screenW = display_width(&(tft));
  const int graphBottom = irContentBottom() - 2;
  display_fill_rect(&(tft), 0, IRRemoteFeature_kIrGraphTop, screenW,
                    graphBottom - IRRemoteFeature_kIrGraphTop + 2, TFT_BLACK);
  display_fill_rect(
      &(tft), IRRemoteFeature_s_irPlot.axisX, IRRemoteFeature_s_irPlot.plotTop,
      IRRemoteFeature_s_irPlot.plotWidth, IRRemoteFeature_s_irPlot.plotHeight,
      IRRemoteFeature_kIrPlotBg);
  IRRemoteFeature_irDrawGraphGrid();
  display_draw_rect(&(tft), IRRemoteFeature_s_irPlot.axisX,
                    IRRemoteFeature_s_irPlot.plotTop,
                    IRRemoteFeature_s_irPlot.plotWidth,
                    IRRemoteFeature_s_irPlot.plotHeight, UI_LINE);

  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), UI_DIM_TEXT, TFT_BLACK);
  if (IRRemoteFeature_s_hasCapture) {
    text_t title = IRRemoteFeature_irGraphTitle();
    display_draw_string(
        &(tft), (title).data,
        (screenW - display_text_width(&(tft), (title).data, 0)) / 2,
        IRRemoteFeature_s_irPlot.graphTop + 2, 0);
  } else {
    display_draw_string(&(tft), "Live IR Scope", (screenW - 66) / 2,
                        IRRemoteFeature_s_irPlot.graphTop + 2, 0);
  }

  IRRemoteFeature_s_irGraphChromeDrawn = true;
}

static void IRRemoteFeature_irResetUiState(void) {
  IRRemoteFeature_s_irBoxesDrawn = false;
  IRRemoteFeature_s_irGraphChromeDrawn = false;
  IRRemoteFeature_s_irPlot.valid = false;
  IRRemoteFeature_s_irStatusStaticDrawn = false;
  IRRemoteFeature_s_irLastStatusDrawMs = 0;
  IRRemoteFeature_s_waveZoomIdx = 0;
  IRRemoteFeature_s_wavePanUs = 0;
  IRRemoteFeature_s_irLogIndex = 0;
  for (int i = 0; i < IRRemoteFeature_kIrMaxLogLines; i++) {
    IRRemoteFeature_s_irLogBuffer[i] = TEXT("");
    IRRemoteFeature_s_irLogColor[i] = 0;
  }
  for (int i = 0; i < IRRemoteFeature_kIrStatusLineCount; i++) {
    IRRemoteFeature_s_irStatusLineText[i] = TEXT("");
    IRRemoteFeature_s_irStatusLineColor[i] = 0;
  }
}

static void IRRemoteFeature_redrawGraphTitleOnly(bool captured) {
  (void)captured;
  if (IRRemoteFeature_s_irGraphChromeDrawn) {
    const int screenW = display_width(&(tft));
    display_fill_rect(&(tft), 0, IRRemoteFeature_s_irPlot.graphTop, screenW, 12,
                      TFT_BLACK);
    display_set_text_size(&(tft), 1);
    display_set_text_color_2(&(tft), UI_DIM_TEXT, TFT_BLACK);
    if (IRRemoteFeature_s_hasCapture) {
      text_t title = IRRemoteFeature_irGraphTitle();
      display_draw_string(
          &(tft), (title).data,
          (screenW - display_text_width(&(tft), (title).data, 0)) / 2,
          IRRemoteFeature_s_irPlot.graphTop + 2, 0);
    } else {
      display_draw_string(&(tft), "Live IR Scope", (screenW - 66) / 2,
                          IRRemoteFeature_s_irPlot.graphTop + 2, 0);
    }
  }
}

static void IRRemoteFeature_redrawScopePlotOnly(void) {
  if (!IRRemoteFeature_s_irGraphChromeDrawn) {
    return;
  }
  if (!IRRemoteFeature_s_irPlot.valid) {
    IRRemoteFeature_irEnsurePlotLayout();
  }
  if (!IRRemoteFeature_s_irPlot.valid) {
    return;
  }

  const int px0 = IRRemoteFeature_s_irPlot.axisX + 1;
  const int py0 = IRRemoteFeature_s_irPlot.plotTop + 1;
  const int pw = IRRemoteFeature_s_irPlot.plotWidth - 2;
  const int ph = IRRemoteFeature_s_irPlot.plotHeight - 2;

  display_fill_rect(&(tft), px0, py0, pw, ph, IRRemoteFeature_kIrPlotBg);

  const int highY = py0 + 8;
  const int lowY = py0 + ph - 9;

  for (int x = 0; x < pw && x < 240; x++) {
    uint16_t idx = (uint16_t)((IRRemoteFeature_s_scopePos + x) % 240);
    bool levelHigh = (IRRemoteFeature_s_scope[idx] != 0);
    int y = levelHigh ? highY : lowY;
    display_draw_pixel(&(tft), px0 + x, y, levelHigh ? UI_WARN : UI_OK);
    if (x > 0) {
      uint16_t pidx = (uint16_t)((IRRemoteFeature_s_scopePos + x - 1) % 240);
      bool prevHigh = (IRRemoteFeature_s_scope[pidx] != 0);
      int py = prevHigh ? highY : lowY;
      if (py != y) {
        display_draw_fast_v_line(&(tft), px0 + x, highY, (lowY - highY + 1),
                                 IRRemoteFeature_kIrGridColor);
      }
    }
  }
}

static void IRRemoteFeature_redrawWaveformPlotOnly(void) {
  if (!IRRemoteFeature_s_irGraphChromeDrawn) {
    return;
  }
  if (!IRRemoteFeature_s_irPlot.valid) {
    IRRemoteFeature_irEnsurePlotLayout();
  }
  if (!IRRemoteFeature_s_irPlot.valid) {
    return;
  }

  const int px0 = IRRemoteFeature_s_irPlot.axisX + 1;
  const int py0 = IRRemoteFeature_s_irPlot.plotTop + 1;
  const int pw = IRRemoteFeature_s_irPlot.plotWidth - 2;
  const int ph = IRRemoteFeature_s_irPlot.plotHeight - 2;

  const uint16_t colGridMajor = UI_LINE;
  const uint16_t colGridMinor = IRRemoteFeature_kIrGridColor;
  const uint16_t colMark = UI_OK;
  const uint16_t colSpace = UI_WARN;
  const uint16_t colText = UI_DIM_TEXT;

  display_fill_rect(&(tft), px0, py0, pw, ph, IRRemoteFeature_kIrPlotBg);

  const int labelY = py0 + ph - 10;
  const int highY = py0 + 10;
  const int lowY = py0 + ph - 18;

  const uint32_t totalUs = IRRemoteFeature_irWaveTotalUs();
  if (totalUs == 0 || IRRemoteFeature_s_rawLen == 0) {
    return;
  }

  const uint32_t windowUs = IRRemoteFeature_waveformWindowUs(totalUs);
  if (windowUs == 0) {
    return;
  }

  const uint32_t panUs = IRRemoteFeature_s_wavePanUs;
  const uint32_t viewEnd = panUs + windowUs;

  uint32_t tickUs = 2000;
  if (windowUs > 80000)
    tickUs = 10000;
  else if (windowUs > 30000)
    tickUs = 5000;
  else if (windowUs > 15000)
    tickUs = 5000;

  while (ir_tick_pixels(pw, windowUs, tickUs) < 10 && tickUs < 50000)
    tickUs *= 2;

  uint32_t majorEvery = 1;
  while (ir_tick_pixels(pw, windowUs, (uint32_t)(tickUs * majorEvery)) < 36 &&
         majorEvery < 16)
    majorEvery *= 2;

  display_set_text_font(&(tft), 1);
  display_set_text_color_2(&(tft), colText, IRRemoteFeature_kIrPlotBg);
  int lastLabelRight = -10000;

  for (uint32_t t = 0, n = 0; t <= windowUs; t += tickUs, n++) {
    int x = px0 + (int)((t * (uint32_t)pw) / windowUs);
    if (x < px0 || x >= (px0 + pw)) continue;

    const bool isMajor = (n % majorEvery) == 0;
    display_draw_fast_v_line(
        &(tft), x, py0, ph,
        (t == 0) ? colGridMajor : (isMajor ? colGridMajor : colGridMinor));

    if (!isMajor) continue;

    uint32_t ms = (t + panUs) / 1000;
    char buf[10];
    snprintf(buf, sizeof(buf), "%lums", (unsigned long)ms);
    int tw = display_text_width(&(tft), buf, 1);
    int lx = x - (tw / 2);
    if (lx < px0) lx = px0;
    if (lx + tw > (px0 + pw)) lx = (px0 + pw) - tw;

    if (lx <= lastLabelRight) continue;
    display_set_cursor(&(tft), lx, labelY);
    display_print(&(tft), (TEXT(buf)).data);
    lastLabelRight = lx + tw + 2;
  }

  bool isMark = true;
  uint32_t tUs = 0;

  for (uint16_t i = 0; i < IRRemoteFeature_s_rawLen; i++) {
    const uint32_t segStart = tUs;
    const uint32_t segEnd = tUs + IRRemoteFeature_s_raw[i];
    tUs = segEnd;

    if (segEnd <= panUs) {
      isMark = !isMark;
      continue;
    }
    if (segStart >= viewEnd) {
      break;
    }

    const uint32_t clipStart = (segStart < panUs) ? panUs : segStart;
    const uint32_t clipEnd = (segEnd > viewEnd) ? viewEnd : segEnd;

    int x1 = px0 + (int)(((clipStart - panUs) * (uint32_t)pw) / windowUs);
    int x2 = px0 + (int)(((clipEnd - panUs) * (uint32_t)pw) / windowUs);
    if (x1 < px0) x1 = px0;
    if (x2 > px0 + pw) x2 = px0 + pw;

    const int y = isMark ? highY : lowY;
    const uint16_t c = isMark ? colMark : colSpace;

    if (x2 > x1) {
      display_draw_fast_h_line(&(tft), x1, y, x2 - x1, c);
      display_draw_fast_h_line(&(tft), x1, y + (isMark ? 1 : -1), x2 - x1, c);
    } else {
      display_draw_pixel(&(tft), x1, y, c);
    }

    if (x2 != x1) {
      display_draw_fast_v_line(&(tft), x2, highY, (lowY - highY + 1),
                               colGridMinor);
    }

    isMark = !isMark;
  }
}

static void IRRemoteFeature_redrawCapturedDetailsOnly(void) {
  IRRemoteFeature_irUpdateStatusPanel(true);
  IRRemoteFeature_redrawGraphTitleOnly(true);
  IRRemoteFeature_redrawWaveformPlotOnly();
}

static const char* IRRemoteFeature_kRandomNames[] = {
    "TV",        "AC",    "Lamp", "Fan",   "Receiver",
    "Projector", "Power", "Mute", "VolUp", "VolDn"};

static void ir_hash_mix(uint32_t* hash, uint32_t value) {
  *hash ^= value;
  *hash *= 16777619u;
}
static uint32_t IRRemoteFeature_hashWaveform(void) {
  uint32_t h = 2166136261u;

  ir_hash_mix(&h, (uint32_t)IRRemoteFeature_s_rawLen);
  ir_hash_mix(&h, (uint32_t)IRRemoteFeature_s_decodeType);
  ir_hash_mix(&h, (uint32_t)IRRemoteFeature_s_bits);
  ir_hash_mix(&h, (uint32_t)IRRemoteFeature_s_value);
  ir_hash_mix(&h, (uint32_t)(IRRemoteFeature_s_value >> 32));
  const uint16_t n =
      (IRRemoteFeature_s_rawLen < 32) ? IRRemoteFeature_s_rawLen : 32;
  for (uint16_t i = 0; i < n; i++)
    ir_hash_mix(&h, (uint32_t)IRRemoteFeature_s_raw[i]);
  return h;
}

static void IRRemoteFeature_tryReplay(void);
static void IRRemoteFeature_saveCapture(void);
static void IRRemoteFeature_runUI(void);
static void IRRemoteFeature_irRestoreChrome(void);

static void IRRemoteFeature_irRestoreChrome(void) {
  currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true, false);

  IRRemoteFeature_s_uiDrawn = false;
  IRRemoteFeature_s_irBoxesDrawn = false;
  IRRemoteFeature_s_irGraphChromeDrawn = false;
  IRRemoteFeature_s_irStatusStaticDrawn = false;
  for (int i = 0; i < IRRemoteFeature_kIrStatusLineCount; i++) {
    IRRemoteFeature_s_irStatusLineText[i] = TEXT("");
    IRRemoteFeature_s_irStatusLineColor[i] = 0;
  }

  IRRemoteFeature_runUI();
  IRRemoteFeature_irDrawTextBoxes();
  IRRemoteFeature_irEnsurePlotLayout();
  IRRemoteFeature_irDrawGraphChrome();
  IRRemoteFeature_irUpdateStatusPanel(true);

  IRRemoteFeature_irRedrawActivityLog();

  if (IRRemoteFeature_s_hasCapture) {
    IRRemoteFeature_redrawWaveformPlotOnly();
  } else {
    IRRemoteFeature_redrawScopePlotOnly();
  }

  irRedrawNavChrome();
  display_draw_fast_h_line(&(tft), 0, IRRemoteFeature_kToolbarY, 240, UI_LINE);
  display_draw_fast_h_line(
      &(tft), 0, IRRemoteFeature_kToolbarY + IRRemoteFeature_kToolbarH, 240,
      UI_LINE);
}

static text_t IRRemoteFeature_getUserInputName(void) {
  OnScreenKeyboardConfig cfg = OnScreenKeyboardConfig_INITIALIZER;
  cfg.titleLine1 = "[!] Name this IR capture";
  cfg.titleLine2 = "(max 15 chars, ^ caps, # sym)";
  osKeyboardUseStandardLayout(&(cfg));
  cfg.maxLen = 15;
  cfg.shuffleNames = IRRemoteFeature_kRandomNames;
  cfg.shuffleCount = (uint8_t)(sizeof(IRRemoteFeature_kRandomNames) /
                               sizeof(IRRemoteFeature_kRandomNames[0]));
  cfg.buttonsY = 195;
  cfg.backLabel = "Back";
  cfg.middleLabel = "Shuffle";
  cfg.okLabel = "OK";
  cfg.enableShuffle = true;
  cfg.requireNonEmpty = true;
  cfg.emptyErrorMsg = "Name cannot be empty!";

  OnScreenKeyboardResult r = showOnScreenKeyboard(&(cfg), TEXT(""));
  IRRemoteFeature_irRestoreChrome();
  if (!r.accepted) {
    return TEXT("");
  }
  return r.text;
}

static const char* IRRemoteFeature_IR_DIR = "/ir";
static const char* IRRemoteFeature_IR_FILE_PREFIX = "/ir/ir_";
enum { IRRemoteFeature_IR_MAGIC = 0x31525249 };

typedef struct __attribute__((packed)) IRRemoteFeature_IrCaptureHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t khz;
  uint16_t rawLen;
  uint8_t decodeType;
  uint8_t reserved;
  uint16_t bits;
  uint64_t value;
  char name[16];
} IRRemoteFeature_IrCaptureHeader;
#define IRRemoteFeature_IrCaptureHeader_INITIALIZER {0}
;

static bool IRRemoteFeature_irEnsureDir(const char* dirPath) {
  if (storage_exists(dirPath)) return true;
  if (storage_mkdir(dirPath)) return true;
  if (dirPath && dirPath[0] == '/') return storage_mkdir(dirPath + 1);
  return false;
}

static bool IRRemoteFeature_makeNextIrPath(text_t* outPath) {
  char buf[32];
  for (uint16_t i = 0; i < 10000; i++) {
    snprintf(buf, sizeof(buf), "%s%04u.bin", IRRemoteFeature_IR_FILE_PREFIX,
             (unsigned)i);
    if (!storage_exists(buf)) {
      (*outPath) = TEXT(buf);
      return true;
    }
  }
  return false;
}

static void IRRemoteFeature_updateDisplay(void) {
  if (!IRRemoteFeature_s_irBoxesDrawn) {
    IRRemoteFeature_irDrawTextBoxes();
    IRRemoteFeature_irRedrawActivityLog();
  }
  IRRemoteFeature_irUpdateStatusPanel(true);

  if (!IRRemoteFeature_s_irGraphChromeDrawn) {
    IRRemoteFeature_irDrawGraphChrome();
    if (!IRRemoteFeature_s_hasCapture) {
      IRRemoteFeature_redrawScopePlotOnly();
    } else {
      IRRemoteFeature_redrawWaveformPlotOnly();
    }
  }
}

static void IRRemoteFeature_handleTouchNavButtons(void) {
  if (!featureHasTouchNavBar()) {
    return;
  }

  if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
    IRRemoteFeature_s_autoTx = false;
    if (IRRemoteFeature_s_repeat > 1) IRRemoteFeature_s_repeat--;
    IRRemoteFeature_s_contentDirty = true;
    IRRemoteFeature_irUpdateStatusPanel(true);
  }
  if (isTouchNavButtonPressedEdge(BTN_RIGHT)) {
    IRRemoteFeature_s_autoTx = false;
    if (IRRemoteFeature_s_repeat < 10) IRRemoteFeature_s_repeat++;
    IRRemoteFeature_s_contentDirty = true;
    IRRemoteFeature_irUpdateStatusPanel(true);
  }
  if (isTouchNavButtonPressedEdge(BTN_UP)) {
    IRRemoteFeature_s_autoTx = false;
    if (IRRemoteFeature_s_hasCapture) IRRemoteFeature_tryReplay();
  }
  if (isTouchNavButtonPressedEdge(BTN_DOWN)) {
    if (IRRemoteFeature_s_hasCapture) IRRemoteFeature_saveCapture();
  }
}

static void IRRemoteFeature_runUI(void) {
  enum { kIconBackX = 10 };
  enum { kIconZoomOutX = 130 };
  enum { kIconZoomInX = 170 };
  enum { kIconAutoX = 210 };
  static int iconY = IRRemoteFeature_kToolbarY;

  if (!IRRemoteFeature_s_uiDrawn) {
    display_fill_rect(&(tft), 0, IRRemoteFeature_kToolbarY, 160,
                      IRRemoteFeature_kToolbarH, DARK_GRAY);
    display_set_text_color_2(&(tft), UI_TEXT, DARK_GRAY);
    display_set_cursor(&(tft), 35, IRRemoteFeature_kToolbarY + 4);
    display_print(&(tft), "IR Record");

    display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
    display_fill_rect(&(tft), 160, IRRemoteFeature_kToolbarY, 80,
                      IRRemoteFeature_kToolbarH, DARK_GRAY);

    display_draw_bitmap(&(tft), kIconBackX, iconY, bitmap_icon_go_back,
                        IRRemoteFeature_kIconSize, IRRemoteFeature_kIconSize,
                        UI_ICON);
    if (IRRemoteFeature_s_hasCapture) {
      display_draw_bitmap(
          &(tft), kIconZoomOutX, iconY, bitmap_icon_sort_down_minus,
          IRRemoteFeature_kIconSize, IRRemoteFeature_kIconSize, UI_ICON);
      display_draw_bitmap(&(tft), kIconZoomInX, iconY, bitmap_icon_sort_up_plus,
                          IRRemoteFeature_kIconSize, IRRemoteFeature_kIconSize,
                          UI_ICON);
    }
    display_draw_bitmap(&(tft), kIconAutoX, iconY, bitmap_icon_random,
                        IRRemoteFeature_kIconSize, IRRemoteFeature_kIconSize,
                        UI_ICON);

    display_draw_fast_h_line(
        &(tft), 0, IRRemoteFeature_kToolbarY + IRRemoteFeature_kToolbarH, 240,
        UI_LINE);
    IRRemoteFeature_s_uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  if (animationState > 0 && sys_monotonic_ms() - lastAnimationTime >= 150) {
    if (animationState == 1) {
      switch (activeIcon) {
        case 0:
          IRRemoteFeature_irWaveZoomOut();
          IRRemoteFeature_irPrint(
              text_join_cstr("[*] Zoom ",
                             (TEXT(IRRemoteFeature_zoomLabel())).data),
              UI_DIM_TEXT, false);
          break;
        case 1:
          IRRemoteFeature_irWaveZoomIn();
          IRRemoteFeature_irPrint(
              text_join_cstr("[*] Zoom ",
                             (TEXT(IRRemoteFeature_zoomLabel())).data),
              UI_DIM_TEXT, false);
          break;
        case 2:
          if (IRRemoteFeature_s_hasCapture) {
            IRRemoteFeature_s_autoTx = !IRRemoteFeature_s_autoTx;
            IRRemoteFeature_s_lastAutoMs = 0;
            IRRemoteFeature_s_contentDirty = true;
            IRRemoteFeature_irPrint(
                text_join_cstr(
                    "[*] Auto ",
                    (TEXT((IRRemoteFeature_s_autoTx ? "ON" : "OFF"))).data),
                IRRemoteFeature_s_autoTx ? UI_WARN : UI_DIM_TEXT, false);
            IRRemoteFeature_irUpdateStatusPanel(true);
          } else {
            IRRemoteFeature_irPrint(TEXT("[!] No capture for auto"),
                                    UI_DIM_TEXT, false);
          }
          break;
        default:
          break;
      }
      if (activeIcon == 0) {
        display_draw_bitmap(
            &(tft), kIconZoomOutX, iconY, bitmap_icon_sort_down_minus,
            IRRemoteFeature_kIconSize, IRRemoteFeature_kIconSize, UI_ICON);
      } else if (activeIcon == 1) {
        display_draw_bitmap(&(tft), kIconZoomInX, iconY,
                            bitmap_icon_sort_up_plus, IRRemoteFeature_kIconSize,
                            IRRemoteFeature_kIconSize, UI_ICON);
      } else if (activeIcon == 2) {
        display_draw_bitmap(&(tft), kIconAutoX, iconY, bitmap_icon_random,
                            IRRemoteFeature_kIconSize,
                            IRRemoteFeature_kIconSize, UI_ICON);
      }
      animationState = 0;
      activeIcon = -1;
    }
    lastAnimationTime = sys_monotonic_ms();
  }

  static unsigned long lastTouchCheck = 0;
  const unsigned long touchCheckInterval = 50;

  if (sys_monotonic_ms() - lastTouchCheck >= touchCheckInterval) {
    int x, y;
    if (feature_active && readTouchXY(&(x), &(y))) {
      if (y > IRRemoteFeature_kToolbarY &&
          y < (IRRemoteFeature_kToolbarY + IRRemoteFeature_kToolbarH)) {
        if (x > kIconBackX && x < (kIconBackX + IRRemoteFeature_kIconSize)) {
          feature_exit_requested = true;
        } else if (IRRemoteFeature_s_hasCapture && x > kIconZoomOutX &&
                   x < (kIconZoomOutX + IRRemoteFeature_kIconSize) &&
                   animationState == 0) {
          display_draw_bitmap(
              &(tft), kIconZoomOutX, iconY, bitmap_icon_sort_down_minus,
              IRRemoteFeature_kIconSize, IRRemoteFeature_kIconSize, TFT_BLACK);
          animationState = 1;
          activeIcon = 0;
          lastAnimationTime = sys_monotonic_ms();
        } else if (IRRemoteFeature_s_hasCapture && x > kIconZoomInX &&
                   x < (kIconZoomInX + IRRemoteFeature_kIconSize) &&
                   animationState == 0) {
          display_draw_bitmap(
              &(tft), kIconZoomInX, iconY, bitmap_icon_sort_up_plus,
              IRRemoteFeature_kIconSize, IRRemoteFeature_kIconSize, TFT_BLACK);
          animationState = 1;
          activeIcon = 1;
          lastAnimationTime = sys_monotonic_ms();
        } else if (x > kIconAutoX &&
                   x < (kIconAutoX + IRRemoteFeature_kIconSize) &&
                   animationState == 0) {
          display_draw_bitmap(&(tft), kIconAutoX, iconY, bitmap_icon_random,
                              IRRemoteFeature_kIconSize,
                              IRRemoteFeature_kIconSize, TFT_BLACK);
          animationState = 1;
          activeIcon = 2;
          lastAnimationTime = sys_monotonic_ms();
        }
      }
    }
    lastTouchCheck = sys_monotonic_ms();
  }
}

static void IRRemoteFeature_tryReplay(void) {
  if (!IRRemoteFeature_s_hasCapture || IRRemoteFeature_s_rawLen == 0) return;

  ir_receiver_disable_ir_in(&(IRRemoteFeature_s_recv));
  IRRemoteFeature_irPrint(TEXT("[!] Sending..."), UI_TEXT, false);
  IRRemoteFeature_irDrawStatusLineIfChanged(0, TEXT("State: Sending"), UI_WARN);

  for (uint8_t i = 0; i < IRRemoteFeature_s_repeat; i++) {
    ir_transmitter_send_raw(&(IRRemoteFeature_s_send), IRRemoteFeature_s_raw,
                            IRRemoteFeature_s_rawLen, IRRemoteFeature_kKhz);
    sys_sleep_ms(35);
  }

  if (!IRRemoteFeature_s_autoTx) {
    IRRemoteFeature_irPrint(TEXT("[+] Send done"), UI_WARN, false);
  }

  sys_sleep_ms(50);
  ir_receiver_enable_ir_in(&(IRRemoteFeature_s_recv));
  IRRemoteFeature_s_lastActionMs = sys_monotonic_ms();
  IRRemoteFeature_irUpdateStatusPanel(true);
}

static void IRRemoteFeature_saveCapture(void) {
  if (!IRRemoteFeature_s_hasCapture || IRRemoteFeature_s_rawLen == 0) {
    showNotification("IR", "No capture to save yet.");
    return;
  }
  if (!isSDCardAvailable()) {
    showNotification("IR", "SD not available.");
    return;
  }
  if (!IRRemoteFeature_irEnsureDir(IRRemoteFeature_IR_DIR)) {
    showNotification("IR", "Cannot create /ir");
    return;
  }

  text_t name = IRRemoteFeature_getUserInputName();
  if (strlen((name).data) == 0) {
    return;
  }

  IRRemoteFeature_irPrint(TEXT("[!] Saving..."), UI_TEXT, false);

  text_t path = {0};
  if (!IRRemoteFeature_makeNextIrPath(&(path))) {
    showNotification("IR", "Too many files in /ir");
    return;
  }

  IRRemoteFeature_IrCaptureHeader h =
      IRRemoteFeature_IrCaptureHeader_INITIALIZER;
  h.magic = IRRemoteFeature_IR_MAGIC;
  h.version = 1;
  h.khz = IRRemoteFeature_kKhz;
  h.rawLen = IRRemoteFeature_s_rawLen;
  h.decodeType = (uint8_t)IRRemoteFeature_s_decodeType;
  h.reserved = 0;
  h.bits = IRRemoteFeature_s_bits;
  h.value = IRRemoteFeature_s_value;
  memset(h.name, 0, sizeof(h.name));
  strncpy(h.name, (name).data, sizeof(h.name) - 1);

  storage_file_t f __attribute__((cleanup(storage_file_close))) =
      storage_file_open((path).data, "wb");
  if (!storage_file_valid(&(f))) {
    showNotification("IR", "Open file failed");
    return;
  }
  bool ok = true;
  ok = ok &&
       (storage_file_write(&(f), (const uint8_t*)&h, sizeof(h)) == sizeof(h));
  ok = ok && (storage_file_write(
                  &(f), (const uint8_t*)IRRemoteFeature_s_raw,
                  (size_t)IRRemoteFeature_s_rawLen * sizeof(uint16_t)) ==
              (size_t)IRRemoteFeature_s_rawLen * sizeof(uint16_t));
  storage_file_close(&(f));

  if (!ok) {
    showNotification("IR", "Write failed");
    return;
  }

  IRRemoteFeature_irPrint(text_join_cstr("[+] Saved ", (name).data), UI_OK,
                          false);
}

static bool IRRemoteFeature_extractHexAfterLabel(text_t s, const char* label,
                                                 uint64_t* out) {
  int li = text_find_index(s, TEXT(label), 0);
  if (li < 0) return false;
  int ox = text_find_index(s, TEXT("0x"), li);
  if (ox < 0) return false;
  int i = ox + 2;
  uint64_t v = 0;
  bool any = false;
  while (i < (int)strlen((s).data)) {
    char c = (s).data[i];
    uint8_t d;
    if (c >= '0' && c <= '9')
      d = (uint8_t)(c - '0');
    else if (c >= 'a' && c <= 'f')
      d = (uint8_t)(10 + c - 'a');
    else if (c >= 'A' && c <= 'F')
      d = (uint8_t)(10 + c - 'A');
    else
      break;
    any = true;
    v = (v << 4) | d;
    i++;
  }
  if (!any) return false;
  (*out) = v;
  return true;
}

static uint32_t IRRemoteFeature_stableHash(void) {
  if (!IRRemoteFeature_s_hasParsedKey) return IRRemoteFeature_hashWaveform();
  uint32_t h = 2166136261u;

  ir_hash_mix(&h, (uint32_t)IRRemoteFeature_s_decodeType);
  ir_hash_mix(&h, (uint32_t)IRRemoteFeature_s_bits);
  ir_hash_mix(&h, (uint32_t)IRRemoteFeature_s_keyAddr);
  ir_hash_mix(&h, (uint32_t)(IRRemoteFeature_s_keyAddr >> 32));
  ir_hash_mix(&h, (uint32_t)IRRemoteFeature_s_keyCmd);
  ir_hash_mix(&h, (uint32_t)(IRRemoteFeature_s_keyCmd >> 32));
  return h;
}

static void IRRemoteFeature_pollCapture(void) {
  if (ir_receiver_decode(&(IRRemoteFeature_s_recv),
                         &IRRemoteFeature_s_results)) {
    const uint16_t rawlen = IRRemoteFeature_s_results.rawlen;
    const uint16_t want = (rawlen > 1) ? (rawlen - 1) : 0;

    if (want == 0 || want > IRRemoteFeature_kMaxRawLen) {
      ir_receiver_resume(&(IRRemoteFeature_s_recv));
      return;
    }

    const bool hadCapture = IRRemoteFeature_s_hasCapture;

    const bool allOnes =
        (IRRemoteFeature_s_results.value == 0xFFFFFFFFULL) ||
        (IRRemoteFeature_s_results.value == 0xFFFFULL) ||
        (IRRemoteFeature_s_results.value == 0xFFFFFFFFFFFFFFFFULL);
    const bool shortFrame = (want > 0 && want < 12);
    const bool looksLikeRepeat =
        hadCapture &&
        ((shortFrame) || (allOnes && IRRemoteFeature_s_results.decode_type ==
                                         IRRemoteFeature_s_decodeType));
    if (looksLikeRepeat) {
      ir_receiver_resume(&(IRRemoteFeature_s_recv));
      return;
    }

    for (uint16_t i = 1; i < rawlen; i++) {
      IRRemoteFeature_s_raw[i - 1] =
          IRRemoteFeature_s_results.rawbuf[i] * IRRemoteFeature_kRawTick;
    }
    IRRemoteFeature_s_rawLen = want;

    IRRemoteFeature_s_decodeType = IRRemoteFeature_s_results.decode_type;
    IRRemoteFeature_s_value = IRRemoteFeature_s_results.value;
    IRRemoteFeature_s_bits = IRRemoteFeature_s_results.bits;

    IRRemoteFeature_s_hasParsedKey = false;
    IRRemoteFeature_s_keyAddr = 0;
    IRRemoteFeature_s_keyCmd = 0;
    IRRemoteFeature_s_keyText = TEXT("");
    {
      text_t human = resultToHumanReadableBasic(&IRRemoteFeature_s_results);
      uint64_t addr = 0, cmd = 0;
      const bool hasAddr =
          IRRemoteFeature_extractHexAfterLabel(human, "Address", &(addr)) ||
          IRRemoteFeature_extractHexAfterLabel(human, "Addr", &(addr));
      const bool hasCmd =
          IRRemoteFeature_extractHexAfterLabel(human, "Command", &(cmd)) ||
          IRRemoteFeature_extractHexAfterLabel(human, "Cmd", &(cmd));
      if (hasAddr && hasCmd) {
        IRRemoteFeature_s_hasParsedKey = true;
        IRRemoteFeature_s_keyAddr = addr;
        IRRemoteFeature_s_keyCmd = cmd;
        IRRemoteFeature_s_keyText = text_join_cstr(
            (text_join_cstr(
                 (text_join_cstr(
                      (text_join_cstr(
                           (TEXT(typeToString(IRRemoteFeature_s_decodeType)))
                               .data,
                           " A:0x"))
                          .data,
                      (uint64ToString(addr, 16)).data))
                     .data,
                 " C:0x"))
                .data,
            (uint64ToString(cmd, 16)).data);
      } else {
        IRRemoteFeature_s_keyText = text_join_cstr(
            (text_join_cstr(
                 (TEXT(typeToString(IRRemoteFeature_s_decodeType))).data,
                 " V:0x"))
                .data,
            (uint64ToString(IRRemoteFeature_s_value, 16)).data);
      }
    }

    IRRemoteFeature_s_hasCapture = true;

    const uint32_t newHash = IRRemoteFeature_stableHash();

    if (!hadCapture) {
      IRRemoteFeature_s_contentDirty = true;
      IRRemoteFeature_s_irGraphChromeDrawn = false;
      IRRemoteFeature_s_uiDrawn = false;
      IRRemoteFeature_s_waveZoomIdx = 0;
      IRRemoteFeature_s_wavePanUs = 0;
      IRRemoteFeature_irPrint(TEXT("[+] Signal captured"), UI_OK, false);
      IRRemoteFeature_s_lastWaveHash = newHash;
      IRRemoteFeature_s_lastStableHash = newHash;
    } else {
      if (newHash != IRRemoteFeature_s_lastStableHash) {
        IRRemoteFeature_s_lastStableHash = newHash;
        const uint32_t now = sys_monotonic_ms();
        if (IRRemoteFeature_s_lastCaptureUiMs == 0 ||
            (now - IRRemoteFeature_s_lastCaptureUiMs) >= 150) {
          IRRemoteFeature_s_captureUiDirty = true;
          IRRemoteFeature_s_lastCaptureUiMs = now;
        }
      }
    }

    ir_receiver_resume(&(IRRemoteFeature_s_recv));
  }
}

void IRRemoteFeature_setup(void) {
  setTouchButtonInputEnabled(true);
  irSetCaptureNavLabels();
  IRRemoteFeature_s_repeat = 1;
  IRRemoteFeature_s_autoTx = false;
  IRRemoteFeature_s_lastAutoMs = 0;
  IRRemoteFeature_s_uiDrawn = false;
  IRRemoteFeature_s_contentDirty = true;
  IRRemoteFeature_s_lastActionMs = 0;

  sys_configure_gpio_pull(IRRemoteFeature_kRecvPin, GPIO_MODE_INPUT,
                          GPIO_PULLUP_ENABLE);
  sys_configure_gpio(IRRemoteFeature_kSendPin, GPIO_MODE_OUTPUT);
  ir_transmitter_begin(&(IRRemoteFeature_s_send));
  ir_receiver_enable_ir_in(&(IRRemoteFeature_s_recv));

  IRRemoteFeature_s_hasCapture = false;
  IRRemoteFeature_s_rawLen = 0;
  IRRemoteFeature_s_decodeType = UNKNOWN;
  IRRemoteFeature_s_value = 0;
  IRRemoteFeature_s_bits = 0;

  IRRemoteFeature_irResetUiState();
  irClearBody(TFT_BLACK);
  currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true, false);
  IRRemoteFeature_runUI();
  IRRemoteFeature_irDrawTextBoxes();
  IRRemoteFeature_irEnsurePlotLayout();
  IRRemoteFeature_irDrawGraphChrome();
  IRRemoteFeature_irUpdateStatusPanel(true);
  IRRemoteFeature_irPrint(TEXT("[+] IR Record ready"), UI_WARN, false);
  IRRemoteFeature_irPrint(TEXT("[*] Point remote and press"), UI_DIM_TEXT,
                          false);
  irRedrawNavChrome();
  IRRemoteFeature_s_contentDirty = false;
}

void IRRemoteFeature_loop(void) {
  if (feature_active &&
      (feature_exit_requested || featureExitButtonPressed())) {
    feature_exit_requested = true;
    return;
  }

  maintainTouchNavBar();
  IRRemoteFeature_runUI();
  if (IRRemoteFeature_s_uiDrawn) {
    display_draw_fast_h_line(&(tft), 0, IRRemoteFeature_kToolbarY, 240,
                             UI_LINE);
    display_draw_fast_h_line(
        &(tft), 0, IRRemoteFeature_kToolbarY + IRRemoteFeature_kToolbarH, 240,
        UI_LINE);
  }
  IRRemoteFeature_handleTouchNavButtons();

  IRRemoteFeature_pollCapture();

  if (IRRemoteFeature_s_hasCapture) {
    int x, y;
    if (readTouchXY(&(x), &(y))) {
      if (!IRRemoteFeature_s_irPlot.valid) {
        IRRemoteFeature_irEnsurePlotLayout();
      }
      IRRemoteFeature_irHandleGraphTouch(x, y);
    }
  }

  if (IRRemoteFeature_s_captureUiDirty && IRRemoteFeature_s_hasCapture) {
    IRRemoteFeature_redrawCapturedDetailsOnly();
    IRRemoteFeature_s_captureUiDirty = false;
  }

  if (!IRRemoteFeature_s_hasCapture) {
    uint32_t nowUs = sys_monotonic_us();
    if (IRRemoteFeature_s_lastScopeSampleUs == 0)
      IRRemoteFeature_s_lastScopeSampleUs = nowUs;

    uint8_t steps = 0;
    while ((uint32_t)(nowUs - IRRemoteFeature_s_lastScopeSampleUs) >= 1000 &&
           steps < 8) {
      IRRemoteFeature_s_lastScopeSampleUs += 1000;

      IRRemoteFeature_s_scope[IRRemoteFeature_s_scopePos] =
          (uint8_t)(gpio_get_level(((gpio_num_t)(IRRemoteFeature_kRecvPin)))
                        ? 1
                        : 0);
      IRRemoteFeature_s_scopePos =
          (uint16_t)((IRRemoteFeature_s_scopePos + 1) % 240);
      steps++;
    }

    uint32_t nowMs = sys_monotonic_ms();
    if (IRRemoteFeature_s_lastScopeDrawMs == 0)
      IRRemoteFeature_s_lastScopeDrawMs = nowMs;
    if ((uint32_t)(nowMs - IRRemoteFeature_s_lastScopeDrawMs) >= 80) {
      IRRemoteFeature_redrawScopePlotOnly();
      IRRemoteFeature_s_lastScopeDrawMs = nowMs;
    }
  }

  static unsigned long lastDebounceTime = 0;
  const unsigned long debounceDelay = 200;
  static bool prevLeft = false, prevRight = false, prevUp = false,
              prevDown = false;
  const bool leftPressed = isPhysicalButtonPressed(BTN_LEFT);
  const bool rightPressed = isPhysicalButtonPressed(BTN_RIGHT);
  const bool upPressed = isPhysicalButtonPressed(BTN_UP);
  const bool downPressed = isPhysicalButtonPressed(BTN_DOWN);

  if (rightPressed && !prevRight &&
      sys_monotonic_ms() - lastDebounceTime > debounceDelay) {
    IRRemoteFeature_s_autoTx = false;
    if (IRRemoteFeature_s_repeat < 10) IRRemoteFeature_s_repeat++;
    IRRemoteFeature_s_contentDirty = true;
    IRRemoteFeature_irUpdateStatusPanel(true);
    lastDebounceTime = sys_monotonic_ms();
  }
  if (leftPressed && !prevLeft &&
      sys_monotonic_ms() - lastDebounceTime > debounceDelay) {
    IRRemoteFeature_s_autoTx = false;
    if (IRRemoteFeature_s_repeat > 1) IRRemoteFeature_s_repeat--;
    IRRemoteFeature_s_contentDirty = true;
    IRRemoteFeature_irUpdateStatusPanel(true);
    lastDebounceTime = sys_monotonic_ms();
  }
  if (upPressed && !prevUp && IRRemoteFeature_s_hasCapture &&
      sys_monotonic_ms() - lastDebounceTime > debounceDelay) {
    IRRemoteFeature_s_autoTx = false;
    IRRemoteFeature_tryReplay();
    lastDebounceTime = sys_monotonic_ms();
  }
  if (downPressed && !prevDown && IRRemoteFeature_s_hasCapture &&
      sys_monotonic_ms() - lastDebounceTime > debounceDelay) {
    IRRemoteFeature_saveCapture();
    lastDebounceTime = sys_monotonic_ms();
  }

  prevLeft = leftPressed;
  prevRight = rightPressed;
  prevUp = upPressed;
  prevDown = downPressed;

  if (IRRemoteFeature_s_autoTx && IRRemoteFeature_s_hasCapture) {
    uint32_t now = sys_monotonic_ms();
    if (IRRemoteFeature_s_lastAutoMs == 0 ||
        (now - IRRemoteFeature_s_lastAutoMs) >=
            IRRemoteFeature_s_autoIntervalMs) {
      IRRemoteFeature_tryReplay();
      IRRemoteFeature_s_lastAutoMs = now;
    }
  }

  if (IRRemoteFeature_s_contentDirty) {
    currentBatteryVoltage = readBatteryVoltage();
    drawStatusBar(currentBatteryVoltage, false, false);
    IRRemoteFeature_runUI();
    IRRemoteFeature_updateDisplay();
    IRRemoteFeature_s_contentDirty = false;
  }

  sys_sleep_ms(10);
}

static const char* IRSavedProfile_IR_DIR = "/ir";
enum { IRSavedProfile_IR_MAGIC = 0x31525249 };

typedef struct __attribute__((packed)) IRSavedProfile_IrCaptureHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t khz;
  uint16_t rawLen;
  uint8_t decodeType;
  uint8_t reserved;
  uint16_t bits;
  uint64_t value;
  char name[16];
} IRSavedProfile_IrCaptureHeader;
#define IRSavedProfile_IrCaptureHeader_INITIALIZER {0}
;

static bool IRSavedProfile_uiDrawn = false;
static int IRSavedProfile_yshift = 16;

enum { IRSavedProfile_ITEMS_PER_PAGE = 7 };
enum { IRSavedProfile_LIST_X = 10 };
enum { IRSavedProfile_LIST_W = 220 };
enum { IRSavedProfile_HEADER_Y = 50 };
enum { IRSavedProfile_HEADER_H = 14 };
enum {
  IRSavedProfile_LIST_Y = IRSavedProfile_HEADER_Y + IRSavedProfile_HEADER_H + 2
};
enum { IRSavedProfile_ROW_H = 18 };
enum { IRSavedProfile_UI_GAP_Y = 6 };
enum { IRSavedProfile_DETAIL_LINE_H = 14 };
enum { IRSavedProfile_DETAIL_LINE_GAP = 3 };
enum {
  IRSavedProfile_DETAIL_LINE_STEP = IRSavedProfile_DETAIL_LINE_H +
  IRSavedProfile_DETAIL_LINE_GAP
};
enum { IRSavedProfile_DETAIL_LINES = 4 };
enum {
  IRSavedProfile_DETAIL_CONTENT_H = IRSavedProfile_DETAIL_LINE_STEP *
      (IRSavedProfile_DETAIL_LINES - 1) +
  IRSavedProfile_DETAIL_LINE_H
};
enum { IRSavedProfile_DETAIL_LABEL_X = IRSavedProfile_LIST_X };
enum { IRSavedProfile_DETAIL_VALUE_X = 50 };
enum { IRSavedProfile_DETAIL_COL2_LABEL_X = 130 };
enum { IRSavedProfile_DETAIL_COL2_VALUE_X = 165 };

static int IRSavedProfile_profileBottomY(void) { return irContentBottom(); }

static int IRSavedProfile_listBottomY(void) {
  return IRSavedProfile_LIST_Y +
         (IRSavedProfile_ITEMS_PER_PAGE * IRSavedProfile_ROW_H);
}

static int IRSavedProfile_detailsY(void) {
  const int areaTop = IRSavedProfile_listBottomY();
  const int areaBottom = IRSavedProfile_profileBottomY();
  const int areaH = areaBottom - areaTop;
  if (areaH <= IRSavedProfile_DETAIL_CONTENT_H) {
    return areaTop + IRSavedProfile_UI_GAP_Y;
  }
  return areaTop + (areaH - IRSavedProfile_DETAIL_CONTENT_H) / 2;
}

static c_vector_t IRSavedProfile_irFiles = {0};
static uint16_t IRSavedProfile_irTotal = 0;
static uint16_t IRSavedProfile_currentIndex = 0;
static text_t IRSavedProfile_sdLastErr = {.data = ""};

static text_t IRSavedProfile_selectedPath = {.data = ""};
static IRSavedProfile_IrCaptureHeader IRSavedProfile_selectedHeader =
    IRSavedProfile_IrCaptureHeader_INITIALIZER;
static bool IRSavedProfile_selectedValid = false;

static uint16_t IRSavedProfile_cachedPageStart = 0xFFFF;
static IRSavedProfile_IrCaptureHeader
    IRSavedProfile_cachedPage[IRSavedProfile_ITEMS_PER_PAGE] =
        IRSavedProfile_IrCaptureHeader_INITIALIZER;
static bool IRSavedProfile_cachedOk[IRSavedProfile_ITEMS_PER_PAGE] = {0};
static bool IRSavedProfile_cacheDirty = true;

static bool IRSavedProfile_deleteArmed = false;
static uint32_t IRSavedProfile_deleteArmUntilMs = 0;

enum { IRSavedProfile_kMaxRawLen2 = 512 };
static uint16_t IRSavedProfile_txRaw[IRSavedProfile_kMaxRawLen2] = {0};

static ir_transmitter_t IRSavedProfile_irsend = {.sendpin_ = IR_TX_PIN,
                                                 .khz_ = 38};
static void IRSavedProfile_transmitProfile(uint16_t idx);
static void IRSavedProfile_deleteProfile(uint16_t idx);

static bool IRSavedProfile_endsWith(text_t s, const char* suf) {
  int sl = strlen((s).data);
  int tl = (int)strlen(suf);
  if (tl > sl) return false;
  return (strcmp((text_slice(s, sl - tl, SIZE_MAX)).data, (TEXT(suf)).data) ==
          0);
}

static text_t IRSavedProfile_baseName(text_t path) {
  int slash = text_rfind_index(path, text_character('/'));
  return ((slash >= 0) ? text_slice(path, slash + 1, SIZE_MAX) : path);
}

static bool IRSavedProfile_readHeader(text_t path,
                                      IRSavedProfile_IrCaptureHeader* out,
                                      text_t* errOut) {
  storage_file_t f __attribute__((cleanup(storage_file_close))) =
      storage_file_open((path).data, "rb");
  if (!storage_file_valid(&(f))) {
    if (errOut) *errOut = TEXT("Open failed");
    return false;
  }
  if (storage_file_read(&(f), (uint8_t*)&(*out), sizeof((*out))) !=
      (int)sizeof((*out))) {
    storage_file_close(&(f));
    if (errOut) *errOut = TEXT("Read hdr failed");
    return false;
  }
  storage_file_close(&(f));
  if ((*out).magic != IRSavedProfile_IR_MAGIC || (*out).version != 1) {
    if (errOut) *errOut = TEXT("Bad file");
    return false;
  }
  (*out).name[15] = '\0';
  if ((*out).rawLen == 0 || (*out).rawLen > IRSavedProfile_kMaxRawLen2) {
    if (errOut) *errOut = TEXT("Bad rawLen");
    return false;
  }
  if ((*out).khz == 0 || (*out).khz > 100) {
    if (errOut) *errOut = TEXT("Bad khz");
    return false;
  }
  return true;
}

static bool IRSavedProfile_readRaw(text_t path,
                                   const IRSavedProfile_IrCaptureHeader* h,
                                   uint16_t* outRaw, text_t* errOut) {
  storage_file_t f __attribute__((cleanup(storage_file_close))) =
      storage_file_open((path).data, "rb");
  if (!storage_file_valid(&(f))) {
    if (errOut) *errOut = TEXT("Open failed");
    return false;
  }

  if (!storage_file_seek(&(f), sizeof(IRSavedProfile_IrCaptureHeader))) {
    storage_file_close(&(f));
    if (errOut) *errOut = TEXT("Seek failed");
    return false;
  }
  const size_t want = (size_t)(*h).rawLen * sizeof(uint16_t);
  if (storage_file_read(&(f), (uint8_t*)outRaw, want) != (int)want) {
    storage_file_close(&(f));
    if (errOut) *errOut = TEXT("Read raw failed");
    return false;
  }
  storage_file_close(&(f));
  return true;
}

static uint16_t IRSavedProfile_pageStartForIndex(uint16_t idx) {
  return (uint16_t)((idx / IRSavedProfile_ITEMS_PER_PAGE) *
                    IRSavedProfile_ITEMS_PER_PAGE);
}

static void IRSavedProfile_refreshSdIndex(bool keepSelection) {
  text_t oldSel = (keepSelection && IRSavedProfile_irTotal > 0
                       ? ((text_t*)(IRSavedProfile_irFiles)
                              .data)[IRSavedProfile_currentIndex]
                       : TEXT(""));

  c_vector_free(&(IRSavedProfile_irFiles));
  IRSavedProfile_irTotal = 0;
  IRSavedProfile_sdLastErr = TEXT("");

  if (!isSDCardAvailable()) {
    IRSavedProfile_sdLastErr = TEXT("SD not available");
    IRSavedProfile_currentIndex = 0;
    IRSavedProfile_selectedValid = false;
    IRSavedProfile_cacheDirty = true;
    return;
  }

  if (!storage_exists(IRSavedProfile_IR_DIR)) {
    IRSavedProfile_sdLastErr = TEXT("No /ir");
    IRSavedProfile_currentIndex = 0;
    IRSavedProfile_selectedValid = false;
    IRSavedProfile_cacheDirty = true;
    return;
  }

  storage_file_t d __attribute__((cleanup(storage_file_close))) =
      storage_file_open(IRSavedProfile_IR_DIR, "rb");
  if (!storage_file_valid(&(d))) {
    IRSavedProfile_sdLastErr = TEXT("Open /ir failed");
    IRSavedProfile_currentIndex = 0;
    IRSavedProfile_selectedValid = false;
    IRSavedProfile_cacheDirty = true;
    return;
  }

  for (;;) {
    storage_file_t f __attribute__((cleanup(storage_file_close))) =
        storage_file_next(&(d));
    if (!storage_file_valid(&(f))) break;
    if (!((f).directory != NULL)) {
      text_t name = TEXT((f).name);

      if (IRSavedProfile_endsWith(name, ".bin") &&
          (text_prefix_cstr((name).data, "ir_") ||
           text_prefix_cstr((name).data, "/ir/ir_") ||
           text_prefix_cstr(
               (name).data,
               (text_join_cstr((TEXT(IRSavedProfile_IR_DIR)).data, "/ir_"))
                   .data))) {
        text_t full =
            (text_prefix_cstr((name).data, "/")
                 ? name
                 : (text_join_cstr(
                       (text_join_cstr((TEXT(IRSavedProfile_IR_DIR)).data, "/"))
                           .data,
                       (name).data)));
        C_VECTOR_PUSH_AS(&(IRSavedProfile_irFiles), text_t, full);
      }
    }
    storage_file_close(&(f));
  }
  storage_file_close(&(d));

  if (((IRSavedProfile_irFiles).size == 0)) {
    IRSavedProfile_sdLastErr = TEXT("No IR captures");
    IRSavedProfile_currentIndex = 0;
    IRSavedProfile_selectedValid = false;
    IRSavedProfile_cacheDirty = true;
    return;
  }

  C_SORT(((text_t*)IRSavedProfile_irFiles.data),
         ((text_t*)IRSavedProfile_irFiles.data) + IRSavedProfile_irFiles.size);

  IRSavedProfile_irTotal = (uint16_t)(IRSavedProfile_irFiles).size;

  if (keepSelection && strlen((oldSel).data)) {
    text_t* it = c_text_find(((text_t*)(IRSavedProfile_irFiles).data),
                             ((text_t*)(IRSavedProfile_irFiles).data +
                              (IRSavedProfile_irFiles).size),
                             oldSel);
    if (it != ((text_t*)(IRSavedProfile_irFiles).data +
               (IRSavedProfile_irFiles).size))
      IRSavedProfile_currentIndex =
          (uint16_t)(it - ((text_t*)(IRSavedProfile_irFiles).data));
  }
  if (IRSavedProfile_currentIndex >= IRSavedProfile_irTotal)
    IRSavedProfile_currentIndex = (uint16_t)(IRSavedProfile_irTotal - 1);

  IRSavedProfile_selectedValid = false;
  IRSavedProfile_cacheDirty = true;
}

static bool IRSavedProfile_loadSelected(text_t* errOut) {
  if (IRSavedProfile_irTotal == 0) {
    IRSavedProfile_selectedValid = false;
    return false;
  }
  IRSavedProfile_selectedPath =
      ((text_t*)(IRSavedProfile_irFiles).data)[IRSavedProfile_currentIndex];
  IRSavedProfile_IrCaptureHeader h = IRSavedProfile_IrCaptureHeader_INITIALIZER;
  if (!IRSavedProfile_readHeader(IRSavedProfile_selectedPath, &(h), errOut)) {
    IRSavedProfile_selectedValid = false;
    return false;
  }
  IRSavedProfile_selectedHeader = h;
  IRSavedProfile_selectedValid = true;
  return true;
}

static void IRSavedProfile_ensurePageCache(void) {
  if (IRSavedProfile_irTotal == 0) return;
  uint16_t start =
      IRSavedProfile_pageStartForIndex(IRSavedProfile_currentIndex);
  if (!IRSavedProfile_cacheDirty && IRSavedProfile_cachedPageStart == start)
    return;
  IRSavedProfile_cachedPageStart = start;
  for (uint8_t i = 0; i < IRSavedProfile_ITEMS_PER_PAGE; i++) {
    IRSavedProfile_cachedOk[i] = false;
    uint16_t idx = (uint16_t)(start + i);
    if (idx >= IRSavedProfile_irTotal) continue;
    text_t err = {0};
    IRSavedProfile_cachedOk[i] =
        IRSavedProfile_readHeader(((text_t*)(IRSavedProfile_irFiles).data)[idx],
                                  &(IRSavedProfile_cachedPage[i]), &err);
    if (!IRSavedProfile_cachedOk[i])
      memset(&IRSavedProfile_cachedPage[i], 0,
             sizeof(IRSavedProfile_IrCaptureHeader));
  }
  IRSavedProfile_cacheDirty = false;
}

static void IRSavedProfile_drawHeaderLine(void) {
  display_fill_rect(&(tft), IRSavedProfile_LIST_X, IRSavedProfile_HEADER_Y,
                    IRSavedProfile_LIST_W, IRSavedProfile_HEADER_H, FEATURE_BG);
  display_set_cursor(&(tft), IRSavedProfile_LIST_X, IRSavedProfile_HEADER_Y);
  display_set_text_color_2(&(tft), UI_ICON, FEATURE_BG);
  display_printf(&(tft), "Profile %d/%d", (int)IRSavedProfile_currentIndex + 1,
                 (int)IRSavedProfile_irTotal);
}

static void IRSavedProfile_drawRow(uint16_t pageStart, uint8_t row) {
  uint16_t idx = (uint16_t)(pageStart + row);
  if (idx >= IRSavedProfile_irTotal) return;
  bool isSel = (idx == IRSavedProfile_currentIndex);
  int y = IRSavedProfile_LIST_Y + (row * IRSavedProfile_ROW_H);

  uint16_t bg = isSel ? UI_FG : FEATURE_BG;
  uint16_t fg = isSel ? UI_ICON : UI_TEXT;
  display_fill_rect(&(tft), IRSavedProfile_LIST_X, y, IRSavedProfile_LIST_W,
                    IRSavedProfile_ROW_H - 1, bg);
  display_set_text_color_2(&(tft), fg, bg);
  display_set_cursor(&(tft), IRSavedProfile_LIST_X, y + 4);
  display_printf(&(tft), "%2d. ", (int)idx + 1);

  if (IRSavedProfile_cachedOk[row]) {
    char nameBuf[17];
    memcpy(nameBuf, IRSavedProfile_cachedPage[row].name, 16);
    nameBuf[16] = '\0';
    text_t nm = TEXT(nameBuf);
    if (strlen((nm).data) == 0)
      nm = IRSavedProfile_baseName(
          ((text_t*)(IRSavedProfile_irFiles).data)[idx]);
    if (strlen((nm).data) > 10) nm = text_slice(nm, 0, 10);
    display_print(&(tft), (nm).data);

    decode_type_t dt = (decode_type_t)IRSavedProfile_cachedPage[row].decodeType;
    text_t ty = TEXT(typeToString(dt));
    if (strlen((ty).data) > 6) ty = text_slice(ty, 0, 6);
    int tw = display_text_width(&(tft), (ty).data, 1);
    display_set_cursor(
        &(tft), IRSavedProfile_LIST_X + IRSavedProfile_LIST_W - 4 - tw, y + 4);
    display_print(&(tft), (ty).data);
  } else {
    display_print(&(tft), "<?>");
  }
}

static void IRSavedProfile_drawListPage(uint16_t pageStart) {
  IRSavedProfile_ensurePageCache();
  display_fill_rect(&(tft), IRSavedProfile_LIST_X, IRSavedProfile_LIST_Y,
                    IRSavedProfile_LIST_W,
                    (IRSavedProfile_ITEMS_PER_PAGE * IRSavedProfile_ROW_H),
                    FEATURE_BG);
  for (uint8_t row = 0; row < IRSavedProfile_ITEMS_PER_PAGE; row++) {
    if ((uint16_t)(pageStart + row) >= IRSavedProfile_irTotal) break;
    IRSavedProfile_drawRow(pageStart, row);
  }
}

static void IRSavedProfile_drawDetails(void) {
  const int dy = IRSavedProfile_detailsY();
  const int gapTop = IRSavedProfile_listBottomY();
  const int gapH = IRSavedProfile_profileBottomY() - gapTop;
  if (gapH > 0) {
    display_fill_rect(&(tft), IRSavedProfile_LIST_X, gapTop,
                      IRSavedProfile_LIST_W, gapH, FEATURE_BG);
  }
  display_draw_fast_h_line(&(tft), IRSavedProfile_LIST_X,
                           IRSavedProfile_listBottomY(), IRSavedProfile_LIST_W,
                           UI_LINE);

  text_t err = {0};
  if (!IRSavedProfile_selectedValid) IRSavedProfile_loadSelected(&err);
  display_set_text_color_2(&(tft), UI_TEXT, FEATURE_BG);
  if (!IRSavedProfile_selectedValid) {
    display_set_cursor(&(tft), IRSavedProfile_DETAIL_LABEL_X, dy);
    display_print(&(tft), "Read failed:");
    display_set_cursor(&(tft), IRSavedProfile_DETAIL_VALUE_X, dy);
    display_print(&(tft), (err).data);
    return;
  }

  decode_type_t dt = (decode_type_t)IRSavedProfile_selectedHeader.decodeType;
  display_set_cursor(&(tft), IRSavedProfile_DETAIL_LABEL_X, dy);
  display_print(&(tft), "Type:");
  display_set_cursor(&(tft), IRSavedProfile_DETAIL_VALUE_X, dy);
  display_print(&(tft), (TEXT(typeToString(dt))).data);
  display_set_cursor(&(tft), IRSavedProfile_DETAIL_COL2_LABEL_X, dy);
  display_print(&(tft), "kHz:");
  display_set_cursor(&(tft), IRSavedProfile_DETAIL_COL2_VALUE_X, dy);
  display_printf(&(tft), "%ukHz", (unsigned)IRSavedProfile_selectedHeader.khz);

  display_set_cursor(&(tft), IRSavedProfile_DETAIL_LABEL_X,
                     dy + IRSavedProfile_DETAIL_LINE_STEP);
  display_print(&(tft), "Bits:");
  display_set_cursor(&(tft), IRSavedProfile_DETAIL_VALUE_X,
                     dy + IRSavedProfile_DETAIL_LINE_STEP);
  display_print(&(tft),
                (TEXT((unsigned)IRSavedProfile_selectedHeader.bits)).data);
  display_set_cursor(&(tft), IRSavedProfile_DETAIL_COL2_LABEL_X,
                     dy + IRSavedProfile_DETAIL_LINE_STEP);
  display_print(&(tft), "Raw:");
  display_set_cursor(&(tft), IRSavedProfile_DETAIL_COL2_VALUE_X,
                     dy + IRSavedProfile_DETAIL_LINE_STEP);
  display_print(&(tft),
                (TEXT((unsigned)IRSavedProfile_selectedHeader.rawLen)).data);

  display_set_cursor(&(tft), IRSavedProfile_DETAIL_LABEL_X,
                     dy + (IRSavedProfile_DETAIL_LINE_STEP * 2));
  display_print(&(tft), "Val:");
  display_set_cursor(&(tft), IRSavedProfile_DETAIL_VALUE_X,
                     dy + (IRSavedProfile_DETAIL_LINE_STEP * 2));
  display_print(&(tft), "0x");
  display_print(&(tft),
                (uint64ToString(IRSavedProfile_selectedHeader.value, 16)).data);

  display_set_text_color_2(&(tft), UI_DIM_TEXT, FEATURE_BG);
  display_set_cursor(&(tft), IRSavedProfile_DETAIL_LABEL_X,
                     dy + (IRSavedProfile_DETAIL_LINE_STEP * 3));
  display_print(&(tft), "SRC:");
  display_set_cursor(&(tft), IRSavedProfile_DETAIL_VALUE_X,
                     dy + (IRSavedProfile_DETAIL_LINE_STEP * 3));
  display_print(&(tft),
                (IRSavedProfile_baseName(IRSavedProfile_selectedPath)).data);

  if (IRSavedProfile_deleteArmed &&
      (int32_t)(sys_monotonic_ms() - IRSavedProfile_deleteArmUntilMs) < 0) {
    int hintY = dy + (IRSavedProfile_DETAIL_LINE_STEP * 4);
    if (hintY >= IRSavedProfile_profileBottomY() - 12)
      hintY = IRSavedProfile_profileBottomY() - 12;
    display_set_cursor(&(tft), IRSavedProfile_DETAIL_LABEL_X, hintY);
    display_set_text_color_2(&(tft), UI_WARN, FEATURE_BG);
    display_print(&(tft), "Press Delete again to confirm");
  }
}

static void IRSavedProfile_updateSelectionUI(uint16_t oldIndex,
                                             bool forceListRedraw) {
  if (IRSavedProfile_irTotal == 0) return;
  uint16_t oldPage = IRSavedProfile_pageStartForIndex(oldIndex);
  uint16_t newPage =
      IRSavedProfile_pageStartForIndex(IRSavedProfile_currentIndex);

  IRSavedProfile_drawHeaderLine();
  if (forceListRedraw || oldPage != newPage) {
    IRSavedProfile_drawListPage(newPage);
  } else {
    uint16_t pageStart = newPage;
    uint8_t oldRow = (uint8_t)(oldIndex - pageStart);
    uint8_t newRow = (uint8_t)(IRSavedProfile_currentIndex - pageStart);
    IRSavedProfile_ensurePageCache();
    if (oldRow < IRSavedProfile_ITEMS_PER_PAGE)
      IRSavedProfile_drawRow(pageStart, oldRow);
    if (newRow < IRSavedProfile_ITEMS_PER_PAGE)
      IRSavedProfile_drawRow(pageStart, newRow);
  }
  IRSavedProfile_drawDetails();
}

static void IRSavedProfile_selectNext(void) {
  if (IRSavedProfile_irTotal == 0) return;
  uint16_t oldIdx = IRSavedProfile_currentIndex;
  IRSavedProfile_currentIndex =
      (uint16_t)((IRSavedProfile_currentIndex + 1) % IRSavedProfile_irTotal);
  IRSavedProfile_selectedValid = false;
  IRSavedProfile_cacheDirty = true;
  IRSavedProfile_deleteArmed = false;
  IRSavedProfile_updateSelectionUI(oldIdx, false);
}

static void IRSavedProfile_selectPrev(void) {
  if (IRSavedProfile_irTotal == 0) return;
  uint16_t oldIdx = IRSavedProfile_currentIndex;
  IRSavedProfile_currentIndex =
      (uint16_t)((IRSavedProfile_currentIndex + IRSavedProfile_irTotal - 1) %
                 IRSavedProfile_irTotal);
  IRSavedProfile_selectedValid = false;
  IRSavedProfile_cacheDirty = true;
  IRSavedProfile_deleteArmed = false;
  IRSavedProfile_updateSelectionUI(oldIdx, false);
}

static void IRSavedProfile_handleTouchNavButtons(void) {
  if (!featureHasTouchNavBar()) {
    return;
  }

  if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
    if (IRSavedProfile_irTotal > 0)
      IRSavedProfile_deleteProfile(IRSavedProfile_currentIndex);
  }
  if (isTouchNavButtonPressedEdge(BTN_DOWN)) {
    IRSavedProfile_selectNext();
  }
  if (isTouchNavButtonPressedEdge(BTN_UP)) {
    IRSavedProfile_selectPrev();
  }
  if (isTouchNavButtonPressedEdge(BTN_RIGHT)) {
    if (IRSavedProfile_irTotal > 0)
      IRSavedProfile_transmitProfile(IRSavedProfile_currentIndex);
  }
}

static void IRSavedProfile_updateDisplay(void) {
  irClearBody(FEATURE_BG);
  float v = readBatteryVoltage();
  drawStatusBar(v, true, false);
  IRSavedProfile_uiDrawn = false;

  IRSavedProfile_drawHeaderLine();
  if (IRSavedProfile_irTotal == 0) {
    display_set_cursor(&(tft), IRSavedProfile_LIST_X,
                       IRSavedProfile_HEADER_Y + IRSavedProfile_DETAIL_LINE_H);
    display_set_text_color_2(&(tft), UI_TEXT, FEATURE_BG);
    display_print(&(tft), ((strlen((IRSavedProfile_sdLastErr).data)
                                ? IRSavedProfile_sdLastErr
                                : TEXT("No profiles on storage::")))
                              .data);
    return;
  }
  IRSavedProfile_drawListPage(
      IRSavedProfile_pageStartForIndex(IRSavedProfile_currentIndex));
  IRSavedProfile_drawDetails();
}

static void IRSavedProfile_transmitProfile(uint16_t idx) {
  if (IRSavedProfile_irTotal == 0 || idx >= IRSavedProfile_irTotal) return;

  text_t path = ((text_t*)(IRSavedProfile_irFiles).data)[idx];
  text_t err = {0};
  IRSavedProfile_IrCaptureHeader h = IRSavedProfile_IrCaptureHeader_INITIALIZER;
  if (!IRSavedProfile_readHeader(path, &(h), &err)) {
    showNotification("IR", "Read failed");
    IRSavedProfile_selectedValid = false;
    IRSavedProfile_cacheDirty = true;
    IRSavedProfile_updateDisplay();
    return;
  }
  if (!IRSavedProfile_readRaw(path, &(h), IRSavedProfile_txRaw, &err)) {
    showNotification("IR", "Raw read failed");
    IRSavedProfile_updateDisplay();
    return;
  }

  ir_transmitter_begin(&(IRSavedProfile_irsend));
  irClearContentFrom(40, FEATURE_BG);
  display_set_cursor(&(tft), 10, 44);
  display_set_text_color_2(&(tft), UI_TEXT, FEATURE_BG);
  display_print(&(tft), "Sending...");

  ir_transmitter_send_raw(&(IRSavedProfile_irsend), IRSavedProfile_txRaw,
                          h.rawLen, h.khz);
  sys_sleep_ms(250);

  irClearContentFrom(40, FEATURE_BG);
  display_set_cursor(&(tft), 10, 44);
  display_print(&(tft), "Done!");
  sys_sleep_ms(300);

  IRSavedProfile_updateDisplay();
}

static void IRSavedProfile_deleteProfile(uint16_t idx) {
  if (IRSavedProfile_irTotal == 0 || idx >= IRSavedProfile_irTotal) return;

  uint32_t now = sys_monotonic_ms();
  if (!IRSavedProfile_deleteArmed ||
      (int32_t)(now - IRSavedProfile_deleteArmUntilMs) >= 0) {
    IRSavedProfile_deleteArmed = true;
    IRSavedProfile_deleteArmUntilMs = now + 3000;
    IRSavedProfile_drawDetails();
    return;
  }

  IRSavedProfile_deleteArmed = false;

  text_t path = ((text_t*)(IRSavedProfile_irFiles).data)[idx];
  if (!storage_remove((path).data)) {
    irClearContentFrom(40, FEATURE_BG);
    display_set_cursor(&(tft), 10, 30 + IRSavedProfile_yshift);
    display_set_text_color_2(&(tft), UI_WARN, FEATURE_BG);
    display_print(&(tft), "Delete FAILED");
    display_set_cursor(&(tft), 10, 45 + IRSavedProfile_yshift);
    display_set_text_color_2(&(tft), UI_TEXT, FEATURE_BG);
    display_print(&(tft), (IRSavedProfile_baseName(path)).data);
    sys_sleep_ms(1200);
    IRSavedProfile_updateDisplay();
    return;
  }

  IRSavedProfile_refreshSdIndex(false);
  if (IRSavedProfile_irTotal == 0)
    IRSavedProfile_currentIndex = 0;
  else if (IRSavedProfile_currentIndex >= IRSavedProfile_irTotal)
    IRSavedProfile_currentIndex = (uint16_t)(IRSavedProfile_irTotal - 1);
  IRSavedProfile_selectedValid = false;
  IRSavedProfile_cacheDirty = true;
  IRSavedProfile_updateDisplay();
}

static void IRSavedProfile_runUI(void) {
  enum { ICON_NUM = 4 };
  static int iconX[ICON_NUM] = {130, 170, 210, 10};
  static int iconY = 20;

  static const unsigned char* icons[ICON_NUM] = {
      bitmap_icon_antenna, bitmap_icon_recycle, bitmap_icon_undo,
      bitmap_icon_go_back};

  if (!IRSavedProfile_uiDrawn) {
    display_fill_rect(&(tft), 0, 20, 240, 16, UI_FG);
    display_draw_line(&(tft), 0, 20, 240, 20, UI_LINE);
    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i])
        display_draw_bitmap(&(tft), iconX[i], iconY, icons[i], 16, 16, UI_ICON);
    }
    display_draw_line(&(tft), 0, 20 + 16, 240, 20 + 16, UI_LINE);
    IRSavedProfile_uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  if (animationState > 0 && sys_monotonic_ms() - lastAnimationTime >= 150) {
    if (animationState == 1) {
      display_draw_bitmap(&(tft), iconX[activeIcon], iconY, icons[activeIcon],
                          16, 16, UI_ICON);
      animationState = 2;

      switch (activeIcon) {
        case 0:
          if (IRSavedProfile_irTotal > 0)
            IRSavedProfile_transmitProfile(IRSavedProfile_currentIndex);
          break;
        case 1:
          if (IRSavedProfile_irTotal > 0)
            IRSavedProfile_deleteProfile(IRSavedProfile_currentIndex);
          break;
        case 2:
          IRSavedProfile_refreshSdIndex(true);
          IRSavedProfile_selectedValid = false;
          IRSavedProfile_cacheDirty = true;
          IRSavedProfile_deleteArmed = false;
          IRSavedProfile_updateDisplay();
          break;
        case 3:
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
    if (feature_active && readTouchXY(&(x), &(y))) {
      if (y >= IRSavedProfile_LIST_Y &&
          y < (IRSavedProfile_LIST_Y +
               (IRSavedProfile_ITEMS_PER_PAGE * IRSavedProfile_ROW_H)) &&
          x >= IRSavedProfile_LIST_X &&
          x < (IRSavedProfile_LIST_X + IRSavedProfile_LIST_W)) {
        uint8_t row =
            (uint8_t)((y - IRSavedProfile_LIST_Y) / IRSavedProfile_ROW_H);
        uint16_t oldIdx = IRSavedProfile_currentIndex;
        uint16_t start =
            IRSavedProfile_pageStartForIndex(IRSavedProfile_currentIndex);
        uint16_t idx = (uint16_t)(start + row);
        if (idx < IRSavedProfile_irTotal) {
          IRSavedProfile_currentIndex = idx;
          IRSavedProfile_selectedValid = false;
          IRSavedProfile_cacheDirty = true;
          IRSavedProfile_deleteArmed = false;
          IRSavedProfile_updateSelectionUI(oldIdx, false);
        }
      }

      if (y > 20 && y < (20 + 16)) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x > iconX[i] && x < iconX[i] + 16) {
            if (icons[i] && animationState == 0) {
              if (i == 3) {
                feature_exit_requested = true;
              } else {
                display_draw_bitmap(&(tft), iconX[i], iconY, icons[i], 16, 16,
                                    FEATURE_BG);
                animationState = 1;
                activeIcon = i;
                lastAnimationTime = sys_monotonic_ms();
              }
            }
            break;
          }
        }
      }
    }
    lastTouchCheck = sys_monotonic_ms();
  }
}

void IRSavedProfile_setup(void) {
  setTouchButtonInputEnabled(true);
  irSetSavedNavLabels();
  ir_transmitter_begin(&(IRSavedProfile_irsend));
  irClearBody(FEATURE_BG);
  float v = readBatteryVoltage();
  drawStatusBar(v, true, false);
  IRSavedProfile_uiDrawn = false;

  IRSavedProfile_refreshSdIndex(false);
  IRSavedProfile_cacheDirty = true;
  IRSavedProfile_deleteArmed = false;
  IRSavedProfile_updateDisplay();
  irRedrawNavChrome();
}

void IRSavedProfile_loop(void) {
  if (feature_active &&
      (feature_exit_requested || featureExitButtonPressed())) {
    feature_exit_requested = true;
    return;
  }

  maintainTouchNavBar();
  IRSavedProfile_runUI();
  if (IRSavedProfile_uiDrawn) {
    display_draw_fast_h_line(&(tft), 0, 20, 240, UI_LINE);
    display_draw_fast_h_line(&(tft), 0, 36, 240, UI_LINE);
  }
  IRSavedProfile_handleTouchNavButtons();

  static unsigned long lastDebounceTime = 0;
  const unsigned long debounceDelay = 200;
  static bool prevUp = false;
  static bool prevDown = false;
  static bool prevRight = false;
  static bool prevLeft = false;

  bool prevPressed = isPhysicalButtonPressed(BTN_UP);
  bool nextPressed = isPhysicalButtonPressed(BTN_DOWN);
  bool txPressed = isPhysicalButtonPressed(BTN_RIGHT);
  bool deletePressed = isPhysicalButtonPressed(BTN_LEFT);

  if (IRSavedProfile_irTotal > 0) {
    if (nextPressed && !prevDown &&
        sys_monotonic_ms() - lastDebounceTime > debounceDelay) {
      IRSavedProfile_selectNext();
      lastDebounceTime = sys_monotonic_ms();
    }

    if (prevPressed && !prevUp &&
        sys_monotonic_ms() - lastDebounceTime > debounceDelay) {
      IRSavedProfile_selectPrev();
      lastDebounceTime = sys_monotonic_ms();
    }

    if (txPressed && !prevRight &&
        sys_monotonic_ms() - lastDebounceTime > debounceDelay) {
      IRSavedProfile_transmitProfile(IRSavedProfile_currentIndex);
      lastDebounceTime = sys_monotonic_ms();
    }

    if (deletePressed && !prevLeft &&
        sys_monotonic_ms() - lastDebounceTime > debounceDelay) {
      IRSavedProfile_deleteProfile(IRSavedProfile_currentIndex);
      lastDebounceTime = sys_monotonic_ms();
    }
  } else {
    display_set_cursor(&(tft), 10, 50 + IRSavedProfile_yshift);
    display_set_text_color_2(&(tft), UI_TEXT, FEATURE_BG);
    display_print(&(tft), ((strlen((IRSavedProfile_sdLastErr).data)
                                ? IRSavedProfile_sdLastErr
                                : TEXT("No profiles on storage::")))
                              .data);
  }

  prevUp = prevPressed;
  prevDown = nextPressed;
  prevRight = txPressed;
  prevLeft = deletePressed;
}

static const char* IRUniversalController_PROFILES_PATH = "/ir_profiles.json";
static const char* IRUniversalController_PROFILES_DIR = "/ir_profiles";

static ir_transmitter_t IRUniversalController_s_send = {.sendpin_ = IR_TX_PIN,
                                                        .khz_ = 38};

enum { IRUniversalController_kMaxProfiles = 500 };

enum { IRUniversalController_kToolbarY = 20 };
enum { IRUniversalController_kToolbarH = 16 };
enum { IRUniversalController_kIconSize = 16 };
enum {
  IRUniversalController_kToolbarBottom = IRUniversalController_kToolbarY +
  IRUniversalController_kToolbarH
};
enum {
  IRUniversalController_kBodyTop = IRUniversalController_kToolbarBottom + 1
};

enum { IRUniversalController_kPadX = 10 };
enum { IRUniversalController_kListW = 220 };
enum { IRUniversalController_kDetailLineH = 14 };
enum { IRUniversalController_kDetailLineGap = 3 };
enum {
  IRUniversalController_kDetailLineStep = IRUniversalController_kDetailLineH +
  IRUniversalController_kDetailLineGap
};
enum { IRUniversalController_kDetailValueX = 50 };
enum { IRUniversalController_kDetailCol2LabelX = 138 };
enum { IRUniversalController_kDetailCol2ValueX = 173 };

enum { IRUniversalController_kInfoBoxX = 4 };
enum { IRUniversalController_kInfoBoxW = 232 };
enum {
  IRUniversalController_kInfoBoxRight = IRUniversalController_kInfoBoxX +
  IRUniversalController_kInfoBoxW
};
enum { IRUniversalController_kInfoBoxHeaderH = 15 };
enum { IRUniversalController_kInfoLines = 3 };
enum {
  IRUniversalController_kInfoContentH = IRUniversalController_kDetailLineStep *
      (IRUniversalController_kInfoLines - 1) +
  IRUniversalController_kDetailLineH
};
enum { IRUniversalController_kInfoBoxY = IRUniversalController_kBodyTop + 6 };
enum {
  IRUniversalController_kInfoBoxH = IRUniversalController_kInfoBoxHeaderH +
  IRUniversalController_kInfoContentH + 8
};
enum {
  IRUniversalController_kInfoTextY = IRUniversalController_kInfoBoxY +
  IRUniversalController_kInfoBoxHeaderH + 2
};
enum {
  IRUniversalController_kRemoteKeysTop = IRUniversalController_kInfoBoxY +
  IRUniversalController_kInfoBoxH + 10
};

enum {
  IRUniversalController_kBrowseHeaderY = IRUniversalController_kToolbarBottom +
  8
};
enum { IRUniversalController_kBrowseHeaderH = 14 };
enum {
  IRUniversalController_kListY = IRUniversalController_kBrowseHeaderY +
  IRUniversalController_kBrowseHeaderH + 2
};

enum { IRUniversalController_kIconBackX = 10 };
enum { IRUniversalController_kIconBrowseX = 170 };
enum { IRUniversalController_kIconReloadX = 210 };

typedef uint8_t IRUniversalController_KeyId;
enum {
  IRUniversalController_Power = 0,
  IRUniversalController_Mute,
  IRUniversalController_VolUp,
  IRUniversalController_VolDn,
  IRUniversalController_ChUp,
  IRUniversalController_ChDn,
  IRUniversalController_Up,
  IRUniversalController_Down,
  IRUniversalController_Left,
  IRUniversalController_Right,
  IRUniversalController_Ok,
  IRUniversalController_Back,
  IRUniversalController_KeyCount
};

static const char* IRUniversalController_keyLabel(
    IRUniversalController_KeyId k) {
  switch (k) {
    case IRUniversalController_Power:
      return "PWR";
    case IRUniversalController_Mute:
      return "MUTE";
    case IRUniversalController_VolUp:
      return "VOL+";
    case IRUniversalController_VolDn:
      return "VOL-";
    case IRUniversalController_ChUp:
      return "CH+";
    case IRUniversalController_ChDn:
      return "CH-";
    case IRUniversalController_Up:
      return "UP";
    case IRUniversalController_Down:
      return "DOWN";
    case IRUniversalController_Left:
      return "LEFT";
    case IRUniversalController_Right:
      return "RIGHT";
    case IRUniversalController_Ok:
      return "OK";
    case IRUniversalController_Back:
      return "BACK";
    default:
      return "?";
  }
}

typedef struct IRUniversalController_Profile {
  text_t name;
  text_t category;
  text_t brand;
  decode_type_t proto;
  uint16_t bits;
  // Keep as 64-bit so we can support protocols like RC6 (36-bit), Panasonic
  // (48-bit), Pioneer (64-bit), etc. Avoid the legacy JSON parser's 64-bit
  // requirement by only parsing 64-bit values from strings (not
  // Variant::as<uint64_t>).
  uint64_t code[IRUniversalController_KeyCount];
  bool has[IRUniversalController_KeyCount];
} IRUniversalController_Profile;
#define IRUniversalController_Profile_INITIALIZER \
  {.proto = UNKNOWN, .bits = 0, .code = {0}, .has = {0}}
;

static c_vector_t IRUniversalController_s_profiles = {0};
static int IRUniversalController_s_profileIdx = 0;
static bool IRUniversalController_s_uiDrawn = false;
static text_t IRUniversalController_s_lastErr = {.data = ""};
static bool IRUniversalController_s_loadedFromSd = false;

static FeatureUI_Button
    IRUniversalController_s_keyBtns[IRUniversalController_KeyCount];

// Browser state.
typedef uint8_t IRUniversalController_Screen;
enum {
  IRUniversalController_Screen_Remote,
  IRUniversalController_Screen_Category,
  IRUniversalController_Screen_Brand,
  IRUniversalController_Screen_Profile
};
static IRUniversalController_Screen IRUniversalController_s_screen =
    IRUniversalController_Screen_Remote;
static c_vector_t IRUniversalController_s_categories = {0};
static c_vector_t IRUniversalController_s_brands = {0};
static c_vector_t IRUniversalController_s_filteredProfileIdx = {0};
static text_t IRUniversalController_s_selectedCategory = {.data = ""};
static text_t IRUniversalController_s_selectedBrand = {.data = ""};
static int IRUniversalController_s_listSel = 0;
static uint32_t IRUniversalController_s_browseVersion = 0;

static const unsigned char* IRUniversalController_keyIcon(
    IRUniversalController_KeyId k) {
  switch (k) {
    case IRUniversalController_Power:
      return bitmap_icon_power;
    case IRUniversalController_Mute:
      return bitmap_icon_dialog;
    case IRUniversalController_VolUp:
      return bitmap_icon_sort_up_plus;
    case IRUniversalController_VolDn:
      return bitmap_icon_sort_down_minus;
    case IRUniversalController_ChUp:
      return bitmap_icon_UP;
    case IRUniversalController_ChDn:
      return bitmap_icon_DOWN;
    case IRUniversalController_Up:
      return bitmap_icon_UP;
    case IRUniversalController_Down:
      return bitmap_icon_DOWN;
    case IRUniversalController_Left:
      return bitmap_icon_LEFT;
    case IRUniversalController_Right:
      return bitmap_icon_RIGHT;
    case IRUniversalController_Ok:
      return bitmap_icon_start;
    case IRUniversalController_Back:
      return bitmap_icon_go_back;
    default:
      return NULL;
  }
}

static void IRUniversalController_drawKeyButton(IRUniversalController_KeyId k,
                                                bool pressed) {
  __auto_type b = &(IRUniversalController_s_keyBtns[(int)k]);
  if ((*b).w <= 0 || (*b).h <= 0) return;

  const bool disabled = (*b).disabled;
  const uint16_t fill = disabled ? UI_BG : (pressed ? UI_FG : UI_BG);
  const uint16_t edge = disabled ? UI_LINE : (pressed ? UI_ICON : UI_LINE);

  display_fill_round_rect(&(tft), (*b).x, (*b).y, (*b).w, (*b).h, 6, fill);
  display_draw_round_rect(&(tft), (*b).x, (*b).y, (*b).w, (*b).h, 6, edge);

  const unsigned char* ico = IRUniversalController_keyIcon(k);
  if (ico) {
    enum { iconSize = 16 };
    const int ix = (*b).x + ((*b).w - iconSize) / 2;
    const int iy = (*b).y + ((*b).h - iconSize) / 2;
    display_draw_bitmap(&(tft), ix, iy, ico, iconSize, iconSize,
                        disabled ? UI_DIM_TEXT : UI_ICON);
  }
}

static text_t IRUniversalController_shortStatusLine(text_t s, int maxLen) {
  if ((int)strlen((s).data) <= maxLen) return s;
  return text_slice(s, 0, maxLen);
}

static text_t IRUniversalController_truncateWithEllipsis(text_t s, int maxPx) {
  if (maxPx <= 0 || strlen((s).data) == 0) return s;
  if ((int)display_text_width(&(tft), (s).data, 1) <= maxPx) return s;
  const int ellW = display_text_width(&(tft), "...", 1);
  text_t out = s;
  while (strlen((out).data) > 0 &&
         (int)display_text_width(&(tft), (out).data, 1) + ellW > maxPx) {
    text_erase(&(out), strlen((out).data) - 1, SIZE_MAX);
  }
  return text_join_cstr((out).data, "...");
}

static text_t IRUniversalController_normalizeToken(text_t in) {
  text_t s = {0};

  for (int i = 0; i < (int)strlen((in).data); i++) {
    char c = (in).data[i];
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
      text_append_cstr(&(s), (TEXT(c)).data);
  }
  return s;
}

static bool IRUniversalController_keyFromToken(
    text_t tok, IRUniversalController_KeyId* out) {
  text_t k = IRUniversalController_normalizeToken(tok);
  if ((strcmp((k).data, "POWER") == 0) || (strcmp((k).data, "PWR") == 0)) {
    (*out) = IRUniversalController_Power;
    return true;
  }
  if ((strcmp((k).data, "MUTE") == 0)) {
    (*out) = IRUniversalController_Mute;
    return true;
  }
  if ((strcmp((k).data, "VOLUP") == 0) || (strcmp((k).data, "VOLUMEUP") == 0) ||
      (strcmp((k).data, "VOLPLUS") == 0) || (strcmp((k).data, "VUP") == 0) ||
      (strcmp((k).data, "VOLP") == 0)) {
    (*out) = IRUniversalController_VolUp;
    return true;
  }
  if ((strcmp((k).data, "VOLDN") == 0) || (strcmp((k).data, "VOLDOWN") == 0) ||
      (strcmp((k).data, "VOLUMEDOWN") == 0) ||
      (strcmp((k).data, "VOLMINUS") == 0) || (strcmp((k).data, "VDN") == 0) ||
      (strcmp((k).data, "VOLM") == 0)) {
    (*out) = IRUniversalController_VolDn;
    return true;
  }
  if ((strcmp((k).data, "CHUP") == 0) || (strcmp((k).data, "CHANUP") == 0) ||
      (strcmp((k).data, "CHANNELUP") == 0) ||
      (strcmp((k).data, "CHPLUS") == 0)) {
    (*out) = IRUniversalController_ChUp;
    return true;
  }
  if ((strcmp((k).data, "CHDN") == 0) || (strcmp((k).data, "CHDOWN") == 0) ||
      (strcmp((k).data, "CHANDOWN") == 0) ||
      (strcmp((k).data, "CHANNELDOWN") == 0) ||
      (strcmp((k).data, "CHMINUS") == 0)) {
    (*out) = IRUniversalController_ChDn;
    return true;
  }
  if ((strcmp((k).data, "UP") == 0)) {
    (*out) = IRUniversalController_Up;
    return true;
  }
  if ((strcmp((k).data, "DOWN") == 0) || (strcmp((k).data, "DN") == 0)) {
    (*out) = IRUniversalController_Down;
    return true;
  }
  if ((strcmp((k).data, "LEFT") == 0)) {
    (*out) = IRUniversalController_Left;
    return true;
  }
  if ((strcmp((k).data, "RIGHT") == 0)) {
    (*out) = IRUniversalController_Right;
    return true;
  }
  if ((strcmp((k).data, "OK") == 0) || (strcmp((k).data, "ENTER") == 0) ||
      (strcmp((k).data, "SELECT") == 0)) {
    (*out) = IRUniversalController_Ok;
    return true;
  }
  if ((strcmp((k).data, "BACK") == 0) || (strcmp((k).data, "RETURN") == 0) ||
      (strcmp((k).data, "EXIT") == 0)) {
    (*out) = IRUniversalController_Back;
    return true;
  }
  return false;
}

static bool IRUniversalController_protoFromToken(text_t tok,
                                                 decode_type_t* out) {
  text_t p = IRUniversalController_normalizeToken(tok);
  if ((strcmp((p).data, "NEC") == 0)) {
    (*out) = NEC;
    return true;
  }
  if ((strcmp((p).data, "NECLIKE") == 0)) {
    (*out) = NEC_LIKE;
    return true;
  }
  if ((strcmp((p).data, "SONY") == 0) || (strcmp((p).data, "SIRC") == 0)) {
    (*out) = SONY;
    return true;
  }
  if ((strcmp((p).data, "SAMSUNG") == 0) ||
      (strcmp((p).data, "SAMSUNG32") == 0) ||
      (strcmp((p).data, "SAMSUNG36") == 0)) {
    (*out) = SAMSUNG36;
    return true;
  }
  if ((strcmp((p).data, "LG") == 0)) {
    (*out) = LG;
    return true;
  }
  if ((strcmp((p).data, "LG2") == 0)) {
    (*out) = LG2;
    return true;
  }
  if ((strcmp((p).data, "JVC") == 0)) {
    (*out) = JVC;
    return true;
  }
  if ((strcmp((p).data, "DENON") == 0)) {
    (*out) = DENON;
    return true;
  }
  if ((strcmp((p).data, "PANASONIC") == 0) ||
      (strcmp((p).data, "KASEIKYO") == 0)) {
    (*out) = PANASONIC;
    return true;
  }
  if ((strcmp((p).data, "RC5") == 0) || (strcmp((p).data, "RC5X") == 0)) {
    (*out) = RC5;
    return true;
  }
  if ((strcmp((p).data, "RC6") == 0)) {
    (*out) = RC6;
    return true;
  }
  if ((strcmp((p).data, "PIONEER") == 0)) {
    (*out) = PIONEER;
    return true;
  }
  if ((strcmp((p).data, "DISH") == 0)) {
    (*out) = DISH;
    return true;
  }
  if ((strcmp((p).data, "GICABLE") == 0)) {
    (*out) = GICABLE;
    return true;
  }
  if ((strcmp((p).data, "EPSON") == 0)) {
    (*out) = EPSON;
    return true;
  }

  return false;
}

static text_t IRUniversalController_inferCategoryFromName(text_t name) {
  text_t n = name;
  text_upper(&(n));
  if (text_find_index(n, TEXT("TV"), 0) >= 0) return TEXT("TV");
  if (text_find_index(n, TEXT("AVR"), 0) >= 0 ||
      text_find_index(n, TEXT("RECEIVER"), 0) >= 0 ||
      text_find_index(n, TEXT("SOUNDBAR"), 0) >= 0)
    return TEXT("AVR");
  if (text_find_index(n, TEXT("PROJECTOR"), 0) >= 0) return TEXT("PROJECTOR");
  if (text_find_index(n, TEXT("XBOX"), 0) >= 0 ||
      text_find_index(n, TEXT("PLAYSTATION"), 0) >= 0)
    return TEXT("GAME");
  if (text_find_index(n, TEXT("AC"), 0) >= 0 ||
      text_find_index(n, TEXT("AIR"), 0) >= 0)
    return TEXT("AC");
  if (text_find_index(n, TEXT("CABLE"), 0) >= 0 ||
      text_find_index(n, TEXT("DISH"), 0) >= 0 ||
      text_find_index(n, TEXT("STB"), 0) >= 0)
    return TEXT("STB");
  return TEXT("OTHER");
}

static text_t IRUniversalController_inferBrandFromName(text_t name) {
  int par = text_find_index(name, text_character('('), 0);
  text_t s = ((par > 0) ? text_slice(name, 0, par) : name);
  text_trim(&(s));
  int sp = text_find_index(s, text_character(' '), 0);
  if (sp > 0) s = text_slice(s, 0, sp);
  text_trim(&(s));
  if (!strlen((s).data)) return TEXT("UNKNOWN");
  text_upper(&(s));
  return s;
}

static void IRUniversalController_loadBuiltinProfiles(void) {
  c_vector_free(&(IRUniversalController_s_profiles));

  {
    IRUniversalController_Profile p = IRUniversalController_Profile_INITIALIZER;
    p.name = TEXT("Samsung TV (common)");
    p.proto = SAMSUNG36;
    p.bits = 32;
    p.code[IRUniversalController_Power] = 0xE0E040BFu;
    p.has[IRUniversalController_Power] = true;
    p.code[IRUniversalController_Mute] = 0xE0E0F00Fu;
    p.has[IRUniversalController_Mute] = true;
    p.code[IRUniversalController_VolUp] = 0xE0E0E01Fu;
    p.has[IRUniversalController_VolUp] = true;
    p.code[IRUniversalController_VolDn] = 0xE0E0D02Fu;
    p.has[IRUniversalController_VolDn] = true;
    p.code[IRUniversalController_ChUp] = 0xE0E048B7u;
    p.has[IRUniversalController_ChUp] = true;
    p.code[IRUniversalController_ChDn] = 0xE0E008F7u;
    p.has[IRUniversalController_ChDn] = true;
    p.code[IRUniversalController_Up] = 0xE0E006F9u;
    p.has[IRUniversalController_Up] = true;
    p.code[IRUniversalController_Down] = 0xE0E08679u;
    p.has[IRUniversalController_Down] = true;
    p.code[IRUniversalController_Left] = 0xE0E0A659u;
    p.has[IRUniversalController_Left] = true;
    p.code[IRUniversalController_Right] = 0xE0E046B9u;
    p.has[IRUniversalController_Right] = true;
    p.code[IRUniversalController_Ok] = 0xE0E016E9u;
    p.has[IRUniversalController_Ok] = true;
    p.code[IRUniversalController_Back] = 0xE0E01AE5u;
    p.has[IRUniversalController_Back] = true;
    C_VECTOR_PUSH_AS(&(IRUniversalController_s_profiles),
                     IRUniversalController_Profile, p);
  }

  {
    IRUniversalController_Profile p = IRUniversalController_Profile_INITIALIZER;
    p.name = TEXT("LG TV (common)");
    p.proto = NEC;
    p.bits = 32;
    p.code[IRUniversalController_Power] = 0x20DF10EFu;
    p.has[IRUniversalController_Power] = true;
    p.code[IRUniversalController_Mute] = 0x20DF906Fu;
    p.has[IRUniversalController_Mute] = true;
    p.code[IRUniversalController_VolUp] = 0x20DF40BFu;
    p.has[IRUniversalController_VolUp] = true;
    p.code[IRUniversalController_VolDn] = 0x20DFC03Fu;
    p.has[IRUniversalController_VolDn] = true;
    p.code[IRUniversalController_ChUp] = 0x20DF00FFu;
    p.has[IRUniversalController_ChUp] = true;
    p.code[IRUniversalController_ChDn] = 0x20DF807Fu;
    p.has[IRUniversalController_ChDn] = true;
    p.code[IRUniversalController_Up] = 0x20DF02FDu;
    p.has[IRUniversalController_Up] = true;
    p.code[IRUniversalController_Down] = 0x20DF827Du;
    p.has[IRUniversalController_Down] = true;
    p.code[IRUniversalController_Left] = 0x20DFE01Fu;
    p.has[IRUniversalController_Left] = true;
    p.code[IRUniversalController_Right] = 0x20DF609Fu;
    p.has[IRUniversalController_Right] = true;
    p.code[IRUniversalController_Ok] = 0x20DF22DDu;
    p.has[IRUniversalController_Ok] = true;
    p.code[IRUniversalController_Back] = 0x20DF14EBu;
    p.has[IRUniversalController_Back] = true;
    C_VECTOR_PUSH_AS(&(IRUniversalController_s_profiles),
                     IRUniversalController_Profile, p);
  }

  {
    IRUniversalController_Profile p = IRUniversalController_Profile_INITIALIZER;
    p.name = TEXT("Sony TV (common)");
    p.proto = SONY;
    p.bits = 12;
    p.code[IRUniversalController_Power] = 0x0A90u;
    p.has[IRUniversalController_Power] = true;
    p.code[IRUniversalController_Mute] = 0x0290u;
    p.has[IRUniversalController_Mute] = true;
    p.code[IRUniversalController_VolUp] = 0x0490u;
    p.has[IRUniversalController_VolUp] = true;
    p.code[IRUniversalController_VolDn] = 0x0C90u;
    p.has[IRUniversalController_VolDn] = true;
    p.code[IRUniversalController_ChUp] = 0x0090u;
    p.has[IRUniversalController_ChUp] = true;
    p.code[IRUniversalController_ChDn] = 0x0890u;
    p.has[IRUniversalController_ChDn] = true;
    C_VECTOR_PUSH_AS(&(IRUniversalController_s_profiles),
                     IRUniversalController_Profile, p);
  }

  {
    IRUniversalController_Profile p = IRUniversalController_Profile_INITIALIZER;
    p.name = TEXT("Xbox 360");
    p.proto = RC6;
    p.bits = 36;
    p.code[IRUniversalController_Power] = 0x0C800F740Cu;
    p.has[IRUniversalController_Power] = true;
    C_VECTOR_PUSH_AS(&(IRUniversalController_s_profiles),
                     IRUniversalController_Profile, p);
  }

  {
    IRUniversalController_Profile p = IRUniversalController_Profile_INITIALIZER;
    p.name = TEXT("Epson Projector");
    p.proto = EPSON;
    p.bits = 32;
    p.code[IRUniversalController_Power] = 0xC1AA09F6u;
    p.has[IRUniversalController_Power] = true;
    C_VECTOR_PUSH_AS(&(IRUniversalController_s_profiles),
                     IRUniversalController_Profile, p);
  }

  {
    IRUniversalController_Profile p = IRUniversalController_Profile_INITIALIZER;
    p.name = TEXT("JVC VCR");
    p.proto = JVC;
    p.bits = 16;
    p.code[IRUniversalController_Power] = 0xC2B8u;
    p.has[IRUniversalController_Power] = true;
    C_VECTOR_PUSH_AS(&(IRUniversalController_s_profiles),
                     IRUniversalController_Profile, p);
  }

  {
    IRUniversalController_Profile p = IRUniversalController_Profile_INITIALIZER;
    p.name = TEXT("Denon AVR");
    p.proto = DENON;
    p.bits = 15;
    p.code[IRUniversalController_Power] = 0x2278u;
    p.has[IRUniversalController_Power] = true;
    C_VECTOR_PUSH_AS(&(IRUniversalController_s_profiles),
                     IRUniversalController_Profile, p);
  }

  {
    IRUniversalController_Profile p = IRUniversalController_Profile_INITIALIZER;
    p.name = TEXT("Denon AVR");
    p.proto = DENON;
    p.bits = 48;
    p.code[IRUniversalController_Power] = 0x2A4C028D6CE3ULL;
    p.has[IRUniversalController_Power] = true;
    C_VECTOR_PUSH_AS(&(IRUniversalController_s_profiles),
                     IRUniversalController_Profile, p);
  }

  {
    IRUniversalController_Profile p = IRUniversalController_Profile_INITIALIZER;
    p.name = TEXT("Panasonic");
    p.proto = PANASONIC;
    p.bits = 48;
    p.code[IRUniversalController_Power] = 0x40040190ED7CULL;
    p.has[IRUniversalController_Power] = true;
    C_VECTOR_PUSH_AS(&(IRUniversalController_s_profiles),
                     IRUniversalController_Profile, p);
  }

  {
    IRUniversalController_Profile p = IRUniversalController_Profile_INITIALIZER;
    p.name = TEXT("Pioneer");
    p.proto = PIONEER;
    p.bits = 64;
    p.code[IRUniversalController_Power] = 0x55FF00AAAA00FF55ULL;
    p.has[IRUniversalController_Power] = true;
    C_VECTOR_PUSH_AS(&(IRUniversalController_s_profiles),
                     IRUniversalController_Profile, p);
  }

  {
    IRUniversalController_Profile p = IRUniversalController_Profile_INITIALIZER;
    p.name = TEXT("DISH");
    p.proto = DISH;
    p.bits = 16;
    p.code[IRUniversalController_Power] = 0x9C00u;
    p.has[IRUniversalController_Power] = true;
    C_VECTOR_PUSH_AS(&(IRUniversalController_s_profiles),
                     IRUniversalController_Profile, p);
  }

  {
    IRUniversalController_Profile p = IRUniversalController_Profile_INITIALIZER;
    p.name = TEXT("GI Cable");
    p.proto = GICABLE;
    p.bits = 16;
    p.code[IRUniversalController_Power] = 0x8807u;
    p.has[IRUniversalController_Power] = true;
    C_VECTOR_PUSH_AS(&(IRUniversalController_s_profiles),
                     IRUniversalController_Profile, p);
  }

  IRUniversalController_s_lastErr = TEXT("Built-in profiles");
}

static bool IRUniversalController_endsWith(text_t s, const char* suf) {
  int sl = strlen((s).data);
  int tl = (int)strlen(suf);
  if (tl > sl) return false;
  return (strcmp((text_slice(s, sl - tl, SIZE_MAX)).data, (TEXT(suf)).data) ==
          0);
}

static bool IRUniversalController_cjsonGetString(cJSON* obj, const char* key,
                                                 const char** out) {
  cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (!cJSON_IsString(item) || item->valuestring == NULL) {
    (*out) = "";
    return false;
  }
  (*out) = item->valuestring;
  return true;
}

static uint16_t IRUniversalController_cjsonGetBits(cJSON* obj,
                                                   uint16_t fallback) {
  cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, "bits");
  if (cJSON_IsNumber(item) && item->valuedouble >= 0.0) {
    return (uint16_t)item->valuedouble;
  }
  return fallback;
}

static bool IRUniversalController_cjsonParseCodeValue(cJSON* value,
                                                      uint64_t* code) {
  if (cJSON_IsNumber(value)) {
    (*code) = (uint64_t)value->valuedouble;
    return true;
  }
  if (cJSON_IsString(value) && value->valuestring != NULL) {
    text_t s = TEXT((value->valuestring));
    text_trim(&(s));
    if (!strlen((s).data)) {
      return false;
    }
    if (text_prefix_cstr((s).data, "0x") || text_prefix_cstr((s).data, "0X")) {
      (*code) = strtoull((s).data + 2, NULL, 16);
      return true;
    }
    (*code) = strtoull((s).data, NULL, 10);
    return true;
  }
  return false;
}

static bool IRUniversalController_cjsonFillProfileFromObject(
    cJSON* obj, IRUniversalController_Profile* p, text_t* errOut) {
  const char* nm = "";
  const char* pr = "";
  const char* cat = "";
  const char* br = "";
  IRUniversalController_cjsonGetString(obj, "name", &(nm));
  IRUniversalController_cjsonGetString(obj, "protocol", &(pr));
  IRUniversalController_cjsonGetString(obj, "category", &(cat));
  IRUniversalController_cjsonGetString(obj, "brand", &(br));

  decode_type_t proto = UNKNOWN;
  if (!IRUniversalController_protoFromToken(TEXT(pr), &(proto))) {
    if (errOut) *errOut = TEXT("Bad protocol");
    return false;
  }

  (*p) = ((
      IRUniversalController_Profile)IRUniversalController_Profile_INITIALIZER);
  (*p).name = TEXT(nm);
  if (!strlen(((*p).name).data)) (*p).name = TEXT(pr);
  (*p).category = TEXT(cat);
  (*p).brand = TEXT(br);
  if (!strlen(((*p).category).data))
    (*p).category = IRUniversalController_inferCategoryFromName((*p).name);
  if (!strlen(((*p).brand).data))
    (*p).brand = IRUniversalController_inferBrandFromName((*p).name);
  (*p).proto = proto;
  (*p).bits =
      IRUniversalController_cjsonGetBits(obj, (proto == SONY) ? 12 : 32);

  cJSON* codes = cJSON_GetObjectItemCaseSensitive(obj, "codes");
  if (cJSON_IsObject(codes)) {
    cJSON* kv = NULL;
    cJSON_ArrayForEach(kv, codes) {
      if (kv->string == NULL) continue;
      IRUniversalController_KeyId kid;
      if (!IRUniversalController_keyFromToken(TEXT(kv->string), &(kid)))
        continue;
      uint64_t code = 0;
      if (!IRUniversalController_cjsonParseCodeValue(kv, &(code))) continue;
      (*p).code[kid] = code;
      (*p).has[kid] = true;
    }
  }

  return true;
}

static bool IRUniversalController_parseProfilesJson(storage_file_t* f,
                                                    size_t docCapacity,
                                                    text_t* errOut) {
  (void)docCapacity;
  size_t size = storage_file_size(f);
  char* json = size < SIZE_MAX ? malloc(size + 1) : NULL;
  if (!json) {
    if (errOut) *errOut = TEXT("Not enough memory");
    return false;
  }
  if (storage_file_read(f, json, size) != (int)size) {
    free(json);
    if (errOut) *errOut = TEXT("SD read failed");
    return false;
  }
  json[size] = 0;
  cJSON* root = cJSON_ParseWithLength(json, size + 1);
  free(json);
  if (!root) {
    if (errOut) *errOut = TEXT("JSON parse failed");
    return false;
  }

  bool ok = false;
  cJSON* profiles = cJSON_GetObjectItemCaseSensitive(root, "profiles");
  if (cJSON_IsArray(profiles)) {
    cJSON* item = NULL;
    cJSON_ArrayForEach(item, profiles) {
      if (!cJSON_IsObject(item)) continue;
      IRUniversalController_Profile p =
          IRUniversalController_Profile_INITIALIZER;
      if (!IRUniversalController_cjsonFillProfileFromObject(item, &(p), errOut))
        continue;
      C_VECTOR_PUSH_AS(&(IRUniversalController_s_profiles),
                       IRUniversalController_Profile, p);
      ok = true;
      if ((int)(IRUniversalController_s_profiles).size >=
          (int)IRUniversalController_kMaxProfiles)
        break;
    }
  } else if (cJSON_IsObject(root)) {
    IRUniversalController_Profile p = IRUniversalController_Profile_INITIALIZER;
    if (IRUniversalController_cjsonFillProfileFromObject(root, &(p), errOut)) {
      C_VECTOR_PUSH_AS(&(IRUniversalController_s_profiles),
                       IRUniversalController_Profile, p);
      ok = true;
    }
  }

  cJSON_Delete(root);
  if (ok) return true;
  if (errOut) *errOut = TEXT("No profiles");
  return false;
}

static bool IRUniversalController_loadProfilesFromSd(text_t* errOut) {
  if (!isSDCardAvailable()) {
    if (errOut) *errOut = TEXT("SD not available");
    return false;
  }
  c_vector_free(&(IRUniversalController_s_profiles));

  if (storage_exists(IRUniversalController_PROFILES_PATH)) {
    storage_file_t f __attribute__((cleanup(storage_file_close))) =
        storage_file_open(IRUniversalController_PROFILES_PATH, "rb");
    if (!storage_file_valid(&(f))) {
      if (errOut) *errOut = TEXT("Open /ir_profiles.json failed");
      return false;
    }
    text_t perr = {0};

    bool ok = IRUniversalController_parseProfilesJson(&(f), 8192, &perr);
    storage_file_close(&(f));
    if (!ok) {
      if (errOut) *errOut = perr;
      return false;
    }
    if (errOut)
      *errOut = text_join_cstr(
          (text_join_cstr(
               "Loaded ",
               (TEXT((int)(IRUniversalController_s_profiles).size)).data))
              .data,
          " profiles");
    return true;
  }

  if (!storage_exists(IRUniversalController_PROFILES_DIR)) {
    if (errOut) *errOut = TEXT("No /ir_profiles.json or /ir_profiles/");
    return false;
  }
  storage_file_t d __attribute__((cleanup(storage_file_close))) =
      storage_file_open(IRUniversalController_PROFILES_DIR, "rb");
  if (!storage_file_valid(&(d))) {
    if (errOut) *errOut = TEXT("Open /ir_profiles failed");
    return false;
  }

  uint16_t loaded = 0;
  for (;;) {
    storage_file_t f __attribute__((cleanup(storage_file_close))) =
        storage_file_next(&(d));
    if (!storage_file_valid(&(f))) break;
    if (!((f).directory != NULL)) {
      text_t name = TEXT((f).name);
      if (IRUniversalController_endsWith(name, ".json")) {
        text_t perr = {0};
        if (IRUniversalController_parseProfilesJson(&(f), 2048, &perr)) {
          loaded++;
          if ((int)(IRUniversalController_s_profiles).size >=
              (int)IRUniversalController_kMaxProfiles) {
            storage_file_close(&(f));
            break;
          }
        }
      }
    }
    storage_file_close(&(f));
  }
  storage_file_close(&(d));

  if (((IRUniversalController_s_profiles).size == 0)) {
    if (errOut) *errOut = TEXT("No profiles in /ir_profiles/");
    return false;
  }
  if (errOut)
    *errOut = text_join_cstr(
        (text_join_cstr(
             "Loaded ",
             (TEXT((int)(IRUniversalController_s_profiles).size)).data))
            .data,
        " profiles");
  return true;
}

static void IRUniversalController_refreshProfiles(void) {
  IRUniversalController_s_lastErr = TEXT("");
  IRUniversalController_s_loadedFromSd = false;

  text_t err = {0};
  if (IRUniversalController_loadProfilesFromSd(&err)) {
    IRUniversalController_s_loadedFromSd = true;
    IRUniversalController_s_lastErr = text_join_cstr(
        "SD: ", (((strlen((err).data)
                       ? err
                       : TEXT((int)(IRUniversalController_s_profiles).size))))
                    .data);
  } else {
    IRUniversalController_loadBuiltinProfiles();
    IRUniversalController_s_loadedFromSd = false;
    if (strlen((err).data))
      IRUniversalController_s_lastErr = text_join_cstr(
          (text_join_cstr("Built-in (SD: ", (err).data)).data, ")");
    else
      IRUniversalController_s_lastErr = TEXT("Built-in");
  }

  if (IRUniversalController_s_profileIdx < 0)
    IRUniversalController_s_profileIdx = 0;
  if (IRUniversalController_s_profileIdx >=
      (int)(IRUniversalController_s_profiles).size)
    IRUniversalController_s_profileIdx = 0;
}

static void IRUniversalController_browseBack(void);
static void IRUniversalController_openCategoryBrowser(void);
static void IRUniversalController_reloadUniversalProfiles(void);

static void IRUniversalController_drawToolbar(void) {
  display_fill_rect(&(tft), 0, IRUniversalController_kToolbarY, 240,
                    IRUniversalController_kToolbarH, UI_FG);
  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
  display_draw_bitmap(&(tft), IRUniversalController_kIconBackX,
                      IRUniversalController_kToolbarY, bitmap_icon_go_back,
                      IRUniversalController_kIconSize,
                      IRUniversalController_kIconSize, UI_ICON);

  if (IRUniversalController_s_screen == IRUniversalController_Screen_Remote) {
    display_draw_bitmap(&(tft), IRUniversalController_kIconBrowseX,
                        IRUniversalController_kToolbarY, bitmap_icon_list,
                        IRUniversalController_kIconSize,
                        IRUniversalController_kIconSize, UI_ICON);
    display_draw_bitmap(&(tft), IRUniversalController_kIconReloadX,
                        IRUniversalController_kToolbarY, bitmap_icon_undo,
                        IRUniversalController_kIconSize,
                        IRUniversalController_kIconSize, UI_ICON);
  }

  display_draw_fast_h_line(&(tft), 0, IRUniversalController_kToolbarBottom, 240,
                           UI_LINE);
}

static void IRUniversalController_drawInfoPanel(void) {
  const int panelH =
      IRUniversalController_kRemoteKeysTop - IRUniversalController_kBodyTop;
  display_fill_rect(&(tft), 0, IRUniversalController_kBodyTop, 240, panelH,
                    FEATURE_BG);

  display_draw_round_rect(&(tft), IRUniversalController_kInfoBoxX,
                          IRUniversalController_kInfoBoxY,
                          IRUniversalController_kInfoBoxW,
                          IRUniversalController_kInfoBoxH, 3, UI_LINE);
  display_set_text_font(&(tft), 1);
  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), UI_DIM_TEXT, FEATURE_BG);
  display_draw_string(&(tft), "Profile", IRUniversalController_kInfoBoxX + 4,
                      IRUniversalController_kInfoBoxY + 3, 0);

  if (((IRUniversalController_s_profiles).size == 0)) {
    display_set_text_color_2(&(tft), UI_WARN, FEATURE_BG);
    display_set_cursor(&(tft), IRUniversalController_kPadX,
                       IRUniversalController_kInfoTextY);
    display_print(&(tft), (IRUniversalController_shortStatusLine(
                               (strlen((IRUniversalController_s_lastErr).data)
                                    ? IRUniversalController_s_lastErr
                                    : TEXT("No profiles")),
                               32))
                              .data);
    return;
  }

  __auto_type p =
      &(((IRUniversalController_Profile*)(IRUniversalController_s_profiles)
             .data)[IRUniversalController_s_profileIdx]);

  display_set_text_color_2(&(tft), UI_DIM_TEXT, FEATURE_BG);
  display_set_cursor(&(tft), IRUniversalController_kPadX,
                     IRUniversalController_kInfoTextY);
  display_print(&(tft), "Name:");
  display_set_text_color_2(&(tft), UI_TEXT, FEATURE_BG);
  display_set_cursor(&(tft), IRUniversalController_kDetailValueX,
                     IRUniversalController_kInfoTextY);
  const int nameMaxW = IRUniversalController_kDetailCol2LabelX -
                       IRUniversalController_kDetailValueX - 4;
  display_print(
      &(tft),
      (IRUniversalController_truncateWithEllipsis((*p).name, nameMaxW)).data);
  display_set_text_color_2(&(tft), UI_DIM_TEXT, FEATURE_BG);
  display_set_cursor(&(tft), IRUniversalController_kDetailCol2LabelX,
                     IRUniversalController_kInfoTextY);
  display_print(&(tft), "Idx:");
  display_set_text_color_2(&(tft), UI_TEXT, FEATURE_BG);
  display_set_cursor(&(tft), IRUniversalController_kDetailCol2ValueX,
                     IRUniversalController_kInfoTextY);
  display_printf(&(tft), "%d/%d", IRUniversalController_s_profileIdx + 1,
                 (int)(IRUniversalController_s_profiles).size);

  display_set_text_color_2(&(tft), UI_DIM_TEXT, FEATURE_BG);
  display_set_cursor(
      &(tft), IRUniversalController_kPadX,
      IRUniversalController_kInfoTextY + IRUniversalController_kDetailLineStep);
  display_print(&(tft), "Type:");
  display_set_text_color_2(&(tft), UI_TEXT, FEATURE_BG);
  display_set_cursor(
      &(tft), IRUniversalController_kDetailValueX,
      IRUniversalController_kInfoTextY + IRUniversalController_kDetailLineStep);
  const int typeMaxW = IRUniversalController_kDetailCol2LabelX -
                       IRUniversalController_kDetailValueX - 4;
  display_print(&(tft), (IRUniversalController_truncateWithEllipsis(
                             TEXT(typeToString((*p).proto)), typeMaxW))
                            .data);
  display_set_text_color_2(&(tft), UI_DIM_TEXT, FEATURE_BG);
  display_set_cursor(
      &(tft), IRUniversalController_kDetailCol2LabelX,
      IRUniversalController_kInfoTextY + IRUniversalController_kDetailLineStep);
  display_print(&(tft), "Bits:");
  display_set_text_color_2(&(tft), UI_TEXT, FEATURE_BG);
  display_set_cursor(
      &(tft), IRUniversalController_kDetailCol2ValueX,
      IRUniversalController_kInfoTextY + IRUniversalController_kDetailLineStep);
  display_print(&(tft), (TEXT((unsigned)(*p).bits)).data);

  display_set_text_color_2(&(tft), UI_DIM_TEXT, FEATURE_BG);
  display_set_cursor(&(tft), IRUniversalController_kPadX,
                     IRUniversalController_kInfoTextY +
                         (IRUniversalController_kDetailLineStep * 2));
  display_print(&(tft), "Src:");
  display_set_text_color_2(
      &(tft), IRUniversalController_s_loadedFromSd ? UI_OK : UI_DIM_TEXT,
      FEATURE_BG);
  display_set_cursor(&(tft), IRUniversalController_kDetailValueX,
                     IRUniversalController_kInfoTextY +
                         (IRUniversalController_kDetailLineStep * 2));
  display_print(
      &(tft),
      (TEXT(IRUniversalController_s_loadedFromSd ? "SD card" : "Built-in"))
          .data);
}

static void IRUniversalController_drawBrowseHeader(void) {
  display_fill_rect(
      &(tft), 0, IRUniversalController_kBodyTop, 240,
      IRUniversalController_kListY - IRUniversalController_kBodyTop,
      FEATURE_BG);
  display_set_text_font(&(tft), 1);
  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), UI_ICON, FEATURE_BG);
  display_set_cursor(&(tft), IRUniversalController_kPadX,
                     IRUniversalController_kBrowseHeaderY);
  if (IRUniversalController_s_screen == IRUniversalController_Screen_Category) {
    display_print(&(tft), "Select Category");
  } else if (IRUniversalController_s_screen ==
             IRUniversalController_Screen_Brand) {
    display_print(&(tft), "Brand:");
    display_set_text_color_2(&(tft), UI_TEXT, FEATURE_BG);
    display_set_cursor(&(tft), IRUniversalController_kDetailValueX,
                       IRUniversalController_kBrowseHeaderY);
    text_t cat = (strlen((IRUniversalController_s_selectedCategory).data)
                      ? IRUniversalController_s_selectedCategory
                      : TEXT("ALL"));
    if (strlen((cat).data) > 18) cat = text_slice(cat, 0, 18);
    display_print(&(tft), (cat).data);
  } else if (IRUniversalController_s_screen ==
             IRUniversalController_Screen_Profile) {
    display_print(&(tft), "Profile:");
    display_set_text_color_2(&(tft), UI_TEXT, FEATURE_BG);
    display_set_cursor(&(tft), IRUniversalController_kDetailValueX,
                       IRUniversalController_kBrowseHeaderY);
    text_t br = (strlen((IRUniversalController_s_selectedBrand).data)
                     ? IRUniversalController_s_selectedBrand
                     : TEXT("ALL"));
    if (strlen((br).data) > 18) br = text_slice(br, 0, 18);
    display_print(&(tft), (br).data);
  }
  display_draw_fast_h_line(&(tft), IRUniversalController_kPadX,
                           IRUniversalController_kListY - 1,
                           IRUniversalController_kListW, UI_LINE);
}

static bool IRUniversalController_handleToolbarTouch(int x, int y) {
  if (y <= IRUniversalController_kToolbarY ||
      y >= IRUniversalController_kToolbarBottom)
    return false;

  if (x > IRUniversalController_kIconBackX &&
      x < (IRUniversalController_kIconBackX +
           IRUniversalController_kIconSize)) {
    if (IRUniversalController_s_screen == IRUniversalController_Screen_Remote) {
      feature_exit_requested = true;
    } else {
      IRUniversalController_browseBack();
      IRUniversalController_s_uiDrawn = false;
    }
    return true;
  }

  if (IRUniversalController_s_screen != IRUniversalController_Screen_Remote)
    return false;

  if (x > IRUniversalController_kIconBrowseX &&
      x < (IRUniversalController_kIconBrowseX +
           IRUniversalController_kIconSize)) {
    IRUniversalController_openCategoryBrowser();
    return true;
  }
  if (x > IRUniversalController_kIconReloadX &&
      x < (IRUniversalController_kIconReloadX +
           IRUniversalController_kIconSize)) {
    IRUniversalController_reloadUniversalProfiles();
    return true;
  }
  return false;
}

static void IRUniversalController_rebuildCategoriesAndBrands(void) {
  c_vector_free(&(IRUniversalController_s_categories));
  c_vector_free(&(IRUniversalController_s_brands));
  if (((IRUniversalController_s_profiles).size == 0)) return;

  {
    for (size_t c_range_index = 0;
         c_range_index < (IRUniversalController_s_profiles).size;
         ++c_range_index) {
      __auto_type p =
          &(((IRUniversalController_Profile*)(IRUniversalController_s_profiles)
                 .data)[c_range_index]);
      {
        c_text_add_unique(
            &IRUniversalController_s_categories,
            (strlen(((*p).category).data) ? (*p).category : TEXT("OTHER")));
      }
    }
  }
  C_SORT(((text_t*)(IRUniversalController_s_categories).data),
         ((text_t*)(IRUniversalController_s_categories).data +
          (IRUniversalController_s_categories).size));
  IRUniversalController_s_browseVersion++;
}

static void IRUniversalController_rebuildBrandsForCategory(text_t cat) {
  c_vector_free(&(IRUniversalController_s_brands));
  if (((IRUniversalController_s_profiles).size == 0)) return;

  c_text_add_unique(&IRUniversalController_s_brands, TEXT("ALL"));
  {
    for (size_t c_range_index = 0;
         c_range_index < (IRUniversalController_s_profiles).size;
         ++c_range_index) {
      __auto_type p =
          &(((IRUniversalController_Profile*)(IRUniversalController_s_profiles)
                 .data)[c_range_index]);
      {
        if ((strcmp(((*p).category).data, (cat).data) == 0))
          c_text_add_unique(
              &IRUniversalController_s_brands,
              (strlen(((*p).brand).data) ? (*p).brand : TEXT("UNKNOWN")));
      }
    }
  }
  C_SORT(((text_t*)(IRUniversalController_s_brands).data) + 1,
         ((text_t*)(IRUniversalController_s_brands).data +
          (IRUniversalController_s_brands).size));
  IRUniversalController_s_browseVersion++;
}

static void IRUniversalController_rebuildFilteredProfiles(void) {
  c_vector_free(&(IRUniversalController_s_filteredProfileIdx));
  if (((IRUniversalController_s_profiles).size == 0)) return;

  for (int i = 0; i < (int)(IRUniversalController_s_profiles).size; i++) {
    __auto_type p =
        &(((IRUniversalController_Profile*)(IRUniversalController_s_profiles)
               .data)[i]);
    if (strlen((IRUniversalController_s_selectedCategory).data) &&
        !(strcmp(((*p).category).data,
                 (IRUniversalController_s_selectedCategory).data) == 0))
      continue;
    if (strlen((IRUniversalController_s_selectedBrand).data) &&
        !(strcmp((IRUniversalController_s_selectedBrand).data, "ALL") == 0) &&
        !(strcmp(((*p).brand).data,
                 (IRUniversalController_s_selectedBrand).data) == 0))
      continue;
    C_VECTOR_PUSH_AS(&(IRUniversalController_s_filteredProfileIdx), int, i);
  }
  IRUniversalController_s_browseVersion++;
}

enum { IRUniversalController_LIST_ROW_H = 18 };

static int IRUniversalController_listTopY(void) {
  return IRUniversalController_kListY;
}
static int IRUniversalController_universalContentBottom(void) {
  return irContentBottom();
}
static int IRUniversalController_listRowsVisible(void) {
  int h = IRUniversalController_universalContentBottom() -
          IRUniversalController_listTopY() - 4;
  int rows = h / IRUniversalController_LIST_ROW_H;
  if (rows < 4) rows = 4;
  if (rows > 10) rows = 10;
  return rows;
}

static void IRUniversalController_drawListRow(int y, text_t left, text_t right,
                                              bool selected) {
  uint16_t bg = selected ? UI_FG : FEATURE_BG;
  uint16_t fg = selected ? UI_ICON : UI_TEXT;
  display_fill_rect(&(tft), IRUniversalController_kPadX, y,
                    IRUniversalController_kListW,
                    IRUniversalController_LIST_ROW_H - 1, bg);
  display_set_text_font(&(tft), 1);
  display_set_text_color_2(&(tft), fg, bg);
  display_set_cursor(&(tft), IRUniversalController_kPadX, y + 4);
  display_print(&(tft), (IRUniversalController_truncateWithEllipsis(
                             left, IRUniversalController_kListW - 4))
                            .data);
  if (strlen((right).data)) {
    int tw = display_text_width(&(tft), (right).data, 1);
    display_set_cursor(
        &(tft), IRUniversalController_kPadX + IRUniversalController_kListW - tw,
        y + 4);
    display_print(&(tft), (right).data);
  }
}

static void IRUniversalController_drawListScreen(const c_vector_t* items) {
  static IRUniversalController_Screen s_lastListScreen =
      IRUniversalController_Screen_Remote;
  static int s_lastSel = -1;
  static int s_lastPageStart = -1;
  static uint32_t s_lastBrowseVersion = 0;

  const int top = IRUniversalController_listTopY();
  const int rows = IRUniversalController_listRowsVisible();
  const int total = (int)((*items)).size;
  if (IRUniversalController_s_listSel < 0) IRUniversalController_s_listSel = 0;
  if (total > 0 && IRUniversalController_s_listSel >= total)
    IRUniversalController_s_listSel = total - 1;

  int pageStart =
      (rows > 0) ? ((IRUniversalController_s_listSel / rows) * rows) : 0;
  const bool needFull =
      (s_lastListScreen != IRUniversalController_s_screen) ||
      (s_lastBrowseVersion != IRUniversalController_s_browseVersion) ||
      (s_lastPageStart != pageStart) || (s_lastSel < 0);

  if (needFull) {
    display_fill_rect(&(tft), 0, IRUniversalController_listTopY(), 240,
                      IRUniversalController_universalContentBottom() -
                          IRUniversalController_listTopY(),
                      FEATURE_BG);
    for (int r = 0; r < rows; r++) {
      int idx = pageStart + r;
      if (idx >= total) break;
      IRUniversalController_drawListRow(
          top + r * IRUniversalController_LIST_ROW_H,
          ((text_t*)((*items)).data)[idx], TEXT(""),
          idx == IRUniversalController_s_listSel);
    }
  } else {
    int oldSel = s_lastSel;
    int newSel = IRUniversalController_s_listSel;
    int oldRow = oldSel - pageStart;
    int newRow = newSel - pageStart;
    if (oldRow >= 0 && oldRow < rows && oldSel >= 0 && oldSel < total) {
      IRUniversalController_drawListRow(
          top + oldRow * IRUniversalController_LIST_ROW_H,
          ((text_t*)((*items)).data)[oldSel], TEXT(""), false);
    }
    if (newRow >= 0 && newRow < rows && newSel >= 0 && newSel < total) {
      IRUniversalController_drawListRow(
          top + newRow * IRUniversalController_LIST_ROW_H,
          ((text_t*)((*items)).data)[newSel], TEXT(""), true);
    }
  }

  s_lastListScreen = IRUniversalController_s_screen;
  s_lastSel = IRUniversalController_s_listSel;
  s_lastPageStart = pageStart;
  s_lastBrowseVersion = IRUniversalController_s_browseVersion;
}

static int IRUniversalController_browseItemCount(void) {
  if (IRUniversalController_s_screen == IRUniversalController_Screen_Category)
    return (int)(IRUniversalController_s_categories).size;
  if (IRUniversalController_s_screen == IRUniversalController_Screen_Brand)
    return (int)(IRUniversalController_s_brands).size;
  if (IRUniversalController_s_screen == IRUniversalController_Screen_Profile)
    return (int)(IRUniversalController_s_filteredProfileIdx).size;
  return 0;
}

static c_vector_t IRUniversalController_profileBrowseNames(void) {
  c_vector_t names __attribute__((cleanup(c_vector_free))) = {0};
  c_vector_reserve(&(names), (IRUniversalController_s_filteredProfileIdx).size,
                   sizeof(text_t));
  {
    for (size_t c_range_index = 0;
         c_range_index < (IRUniversalController_s_filteredProfileIdx).size;
         ++c_range_index) {
      __auto_type idx = (((int*)(IRUniversalController_s_filteredProfileIdx)
                              .data)[c_range_index]);
      {
        if (idx < 0 || idx >= (int)(IRUniversalController_s_profiles).size)
          continue;
        C_VECTOR_PUSH_AS(
            &(names), text_t,
            ((IRUniversalController_Profile*)(IRUniversalController_s_profiles)
                 .data)[idx]
                .name);
      }
    }
  }
  return c_vector_move(&(names));
}

static void IRUniversalController_redrawBrowseListOnly(void) {
  if (IRUniversalController_s_screen == IRUniversalController_Screen_Category)
    IRUniversalController_drawListScreen(&(IRUniversalController_s_categories));
  else if (IRUniversalController_s_screen == IRUniversalController_Screen_Brand)
    IRUniversalController_drawListScreen(&(IRUniversalController_s_brands));
  else if (IRUniversalController_s_screen ==
           IRUniversalController_Screen_Profile)
    do {
      c_vector_t names = IRUniversalController_profileBrowseNames();
      IRUniversalController_drawListScreen(&names);
      c_vector_free(&names);
    } while (0);
}

static void IRUniversalController_bumpBrowseSelection(int delta) {
  const int total = IRUniversalController_browseItemCount();
  if (total <= 0) return;
  IRUniversalController_s_listSel += delta;
  if (IRUniversalController_s_listSel < 0) IRUniversalController_s_listSel = 0;
  if (IRUniversalController_s_listSel >= total)
    IRUniversalController_s_listSel = total - 1;
  IRUniversalController_redrawBrowseListOnly();
}

static void IRUniversalController_layoutKeyButtons(void) {
  const int topY = IRUniversalController_kRemoteKeysTop;
  const int bottomY = IRUniversalController_universalContentBottom() - 6;

  enum { gap = 6 };
  enum { topBtnH = 34 };
  const int topBtnW = (IRUniversalController_kInfoBoxW - 2 * gap) / 3;
  const int yTop = topY;
  const int x0 = IRUniversalController_kInfoBoxX;
  const int x1 = x0 + topBtnW + gap;
  const int x2 = IRUniversalController_kInfoBoxRight - topBtnW;
  IRUniversalController_s_keyBtns[(int)IRUniversalController_Power] =
      (FeatureUI_Button){
          (int16_t)x0,
          (int16_t)yTop,
          (int16_t)topBtnW,
          (int16_t)topBtnH,
          IRUniversalController_keyLabel(IRUniversalController_Power),
          FeatureUI_ButtonStyle_Secondary,
          true};
  IRUniversalController_s_keyBtns[(int)IRUniversalController_Mute] =
      (FeatureUI_Button){
          (int16_t)x1,
          (int16_t)yTop,
          (int16_t)topBtnW,
          (int16_t)topBtnH,
          IRUniversalController_keyLabel(IRUniversalController_Mute),
          FeatureUI_ButtonStyle_Secondary,
          true};
  IRUniversalController_s_keyBtns[(int)IRUniversalController_Back] =
      (FeatureUI_Button){
          (int16_t)x2,
          (int16_t)yTop,
          (int16_t)topBtnW,
          (int16_t)topBtnH,
          IRUniversalController_keyLabel(IRUniversalController_Back),
          FeatureUI_ButtonStyle_Secondary,
          true};

  const int yMidTop = yTop + topBtnH + 8;
  const int midH = bottomY - yMidTop;
  enum { rockerW = 54 };
  enum { rockerH = 72 };
  enum { dpadSize = 92 };
  const int dpadX = (240 - dpadSize) / 2;
  const int dpadY = yMidTop + (midH - dpadSize) / 2;

  const int volX = IRUniversalController_kInfoBoxX;
  const int chX = IRUniversalController_kInfoBoxRight - rockerW;
  const int rockerY = yMidTop + (midH - rockerH) / 2;
  IRUniversalController_s_keyBtns[(int)IRUniversalController_VolUp] =
      (FeatureUI_Button){
          (int16_t)volX,
          (int16_t)rockerY,
          (int16_t)rockerW,
          (int16_t)(rockerH / 2 - 2),
          IRUniversalController_keyLabel(IRUniversalController_VolUp),
          FeatureUI_ButtonStyle_Secondary,
          true};
  IRUniversalController_s_keyBtns[(int)IRUniversalController_VolDn] =
      (FeatureUI_Button){
          (int16_t)volX,
          (int16_t)(rockerY + rockerH / 2 + 2),
          (int16_t)rockerW,
          (int16_t)(rockerH / 2 - 2),
          IRUniversalController_keyLabel(IRUniversalController_VolDn),
          FeatureUI_ButtonStyle_Secondary,
          true};
  IRUniversalController_s_keyBtns[(int)IRUniversalController_ChUp] =
      (FeatureUI_Button){
          (int16_t)chX,
          (int16_t)rockerY,
          (int16_t)rockerW,
          (int16_t)(rockerH / 2 - 2),
          IRUniversalController_keyLabel(IRUniversalController_ChUp),
          FeatureUI_ButtonStyle_Secondary,
          true};
  IRUniversalController_s_keyBtns[(int)IRUniversalController_ChDn] =
      (FeatureUI_Button){
          (int16_t)chX,
          (int16_t)(rockerY + rockerH / 2 + 2),
          (int16_t)rockerW,
          (int16_t)(rockerH / 2 - 2),
          IRUniversalController_keyLabel(IRUniversalController_ChDn),
          FeatureUI_ButtonStyle_Secondary,
          true};

  enum { unit = 28 };
  enum { okSize = 38 };
  const int okX = dpadX + (dpadSize - okSize) / 2;
  const int okY = dpadY + (dpadSize - okSize) / 2;
  IRUniversalController_s_keyBtns[(int)IRUniversalController_Ok] =
      (FeatureUI_Button){
          (int16_t)okX,
          (int16_t)okY,
          (int16_t)okSize,
          (int16_t)okSize,
          IRUniversalController_keyLabel(IRUniversalController_Ok),
          FeatureUI_ButtonStyle_Secondary,
          true};
  IRUniversalController_s_keyBtns[(int)IRUniversalController_Up] =
      (FeatureUI_Button){
          (int16_t)(dpadX + (dpadSize - unit) / 2),
          (int16_t)dpadY,
          (int16_t)unit,
          (int16_t)unit,
          IRUniversalController_keyLabel(IRUniversalController_Up),
          FeatureUI_ButtonStyle_Secondary,
          true};
  IRUniversalController_s_keyBtns[(int)IRUniversalController_Down] =
      (FeatureUI_Button){
          (int16_t)(dpadX + (dpadSize - unit) / 2),
          (int16_t)(dpadY + dpadSize - unit),
          (int16_t)unit,
          (int16_t)unit,
          IRUniversalController_keyLabel(IRUniversalController_Down),
          FeatureUI_ButtonStyle_Secondary,
          true};
  IRUniversalController_s_keyBtns[(int)IRUniversalController_Left] =
      (FeatureUI_Button){
          (int16_t)dpadX,
          (int16_t)(dpadY + (dpadSize - unit) / 2),
          (int16_t)unit,
          (int16_t)unit,
          IRUniversalController_keyLabel(IRUniversalController_Left),
          FeatureUI_ButtonStyle_Secondary,
          true};
  IRUniversalController_s_keyBtns[(int)IRUniversalController_Right] =
      (FeatureUI_Button){
          (int16_t)(dpadX + dpadSize - unit),
          (int16_t)(dpadY + (dpadSize - unit) / 2),
          (int16_t)unit,
          (int16_t)unit,
          IRUniversalController_keyLabel(IRUniversalController_Right),
          FeatureUI_ButtonStyle_Secondary,
          true};
}

static void IRUniversalController_drawKeys(void) {
  IRUniversalController_layoutKeyButtons();
  display_fill_rect(&(tft), 0, IRUniversalController_kRemoteKeysTop, 240,
                    IRUniversalController_universalContentBottom() -
                        IRUniversalController_kRemoteKeysTop,
                    FEATURE_BG);

  if (((IRUniversalController_s_profiles).size == 0)) {
    display_set_text_font(&(tft), 1);
    display_set_text_color_2(&(tft), UI_TEXT, FEATURE_BG);
    display_set_cursor(&(tft), IRUniversalController_kPadX,
                       IRUniversalController_kRemoteKeysTop + 12);
    display_print(&(tft), "Add SD profiles:");
    display_set_cursor(&(tft), IRUniversalController_kPadX,
                       IRUniversalController_kRemoteKeysTop + 26);
    display_print(&(tft), "/ir_profiles.json");
    display_set_cursor(&(tft), IRUniversalController_kPadX,
                       IRUniversalController_kRemoteKeysTop + 40);
    display_print(&(tft), "or /ir_profiles/*.json");
    return;
  }

  __auto_type p =
      &(((IRUniversalController_Profile*)(IRUniversalController_s_profiles)
             .data)[IRUniversalController_s_profileIdx]);
  for (int i = 0; i < (int)IRUniversalController_KeyCount; i++) {
    IRUniversalController_s_keyBtns[i].disabled = !(*p).has[i];
  }
  for (int i = 0; i < (int)IRUniversalController_KeyCount; i++) {
    IRUniversalController_drawKeyButton((IRUniversalController_KeyId)i, false);
  }
}

static void IRUniversalController_drawFooter(void) {
  if (IRUniversalController_s_screen == IRUniversalController_Screen_Remote) {
    irSetUniversalRemoteNavLabels();
  } else {
    irSetUniversalBrowseNavLabels();
  }
  irRedrawNavChrome();
}

static void IRUniversalController_drawAll(void) {
  irClearBody(FEATURE_BG);
  float v = readBatteryVoltage();
  drawStatusBar(v, true, false);
  IRUniversalController_drawToolbar();

  if (IRUniversalController_s_screen == IRUniversalController_Screen_Remote) {
    IRUniversalController_drawInfoPanel();
    IRUniversalController_drawKeys();
    IRUniversalController_drawFooter();
    IRUniversalController_s_uiDrawn = true;
    return;
  }

  IRUniversalController_drawBrowseHeader();
  IRUniversalController_redrawBrowseListOnly();
  IRUniversalController_drawFooter();
  IRUniversalController_s_uiDrawn = true;
}

static void IRUniversalController_sendKey(IRUniversalController_KeyId k) {
  if (((IRUniversalController_s_profiles).size == 0)) return;
  __auto_type p =
      &(((IRUniversalController_Profile*)(IRUniversalController_s_profiles)
             .data)[IRUniversalController_s_profileIdx]);
  if (!(*p).has[k]) {
    showNotification("IR", "Key not available in this profile");
    return;
  }

  ir_transmitter_begin(&(IRUniversalController_s_send));
  uint16_t repeat = 0;
  if ((*p).proto == SONY || (*p).proto == SONY_38K) repeat = 2;

  if (!ir_transmitter_send(&(IRUniversalController_s_send), (*p).proto,
                           (*p).code[k], (*p).bits, repeat)) {
    showNotification("IR", "Protocol not enabled in build");
  }
}

static void IRUniversalController_changeProfile(int delta) {
  if (((IRUniversalController_s_profiles).size == 0)) return;
  int n = (int)(IRUniversalController_s_profiles).size;
  IRUniversalController_s_profileIdx =
      (IRUniversalController_s_profileIdx + delta) % n;
  if (IRUniversalController_s_profileIdx < 0)
    IRUniversalController_s_profileIdx += n;
  IRUniversalController_drawInfoPanel();
  IRUniversalController_drawKeys();
}

static void IRUniversalController_openCategoryBrowser(void) {
  IRUniversalController_s_screen = IRUniversalController_Screen_Category;
  IRUniversalController_s_listSel = 0;
  IRUniversalController_rebuildCategoriesAndBrands();
  IRUniversalController_s_uiDrawn = false;
}

static void IRUniversalController_reloadUniversalProfiles(void) {
  IRUniversalController_refreshProfiles();
  IRUniversalController_rebuildCategoriesAndBrands();
  IRUniversalController_s_uiDrawn = false;
}

static void IRUniversalController_browseBack(void) {
  if (IRUniversalController_s_screen == IRUniversalController_Screen_Category)
    IRUniversalController_s_screen = IRUniversalController_Screen_Remote;
  else if (IRUniversalController_s_screen == IRUniversalController_Screen_Brand)
    IRUniversalController_s_screen = IRUniversalController_Screen_Category;
  else if (IRUniversalController_s_screen ==
           IRUniversalController_Screen_Profile)
    IRUniversalController_s_screen = IRUniversalController_Screen_Brand;
  IRUniversalController_s_uiDrawn = false;
}

static void IRUniversalController_browseSelect(void) {
  if (IRUniversalController_s_screen == IRUniversalController_Screen_Category) {
    if (((IRUniversalController_s_categories).size == 0)) return;
    IRUniversalController_s_selectedCategory =
        ((text_t*)(IRUniversalController_s_categories).data)[DIV_MAX(
            0, DIV_MIN(IRUniversalController_s_listSel,
                       (int)(IRUniversalController_s_categories).size - 1))];
    IRUniversalController_rebuildBrandsForCategory(
        IRUniversalController_s_selectedCategory);
    IRUniversalController_s_screen = IRUniversalController_Screen_Brand;
    IRUniversalController_s_listSel = 0;
    IRUniversalController_s_uiDrawn = false;
  } else if (IRUniversalController_s_screen ==
             IRUniversalController_Screen_Brand) {
    if (((IRUniversalController_s_brands).size == 0)) return;
    IRUniversalController_s_selectedBrand =
        ((text_t*)(IRUniversalController_s_brands).data)[DIV_MAX(
            0, DIV_MIN(IRUniversalController_s_listSel,
                       (int)(IRUniversalController_s_brands).size - 1))];
    IRUniversalController_rebuildFilteredProfiles();
    IRUniversalController_s_screen = IRUniversalController_Screen_Profile;
    IRUniversalController_s_listSel = 0;
    IRUniversalController_s_uiDrawn = false;
  } else if (IRUniversalController_s_screen ==
             IRUniversalController_Screen_Profile) {
    if (((IRUniversalController_s_filteredProfileIdx).size == 0)) return;
    int idx = ((int*)(IRUniversalController_s_filteredProfileIdx).data)[DIV_MAX(
        0,
        DIV_MIN(IRUniversalController_s_listSel,
                (int)(IRUniversalController_s_filteredProfileIdx).size - 1))];
    if (idx >= 0 && idx < (int)(IRUniversalController_s_profiles).size) {
      IRUniversalController_s_profileIdx = idx;
      IRUniversalController_s_screen = IRUniversalController_Screen_Remote;
      IRUniversalController_s_uiDrawn = false;
    }
  }
}

static void IRUniversalController_handleUniversalNavButtons(void) {
  if (!featureHasTouchNavBar()) {
    return;
  }

  if (IRUniversalController_s_screen == IRUniversalController_Screen_Remote) {
    if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
      IRUniversalController_changeProfile(-1);
    }
    if (isTouchNavButtonPressedEdge(BTN_DOWN)) {
      IRUniversalController_openCategoryBrowser();
    }
    if (isTouchNavButtonPressedEdge(BTN_UP)) {
      IRUniversalController_reloadUniversalProfiles();
    }
    if (isTouchNavButtonPressedEdge(BTN_RIGHT)) {
      IRUniversalController_changeProfile(+1);
    }
    return;
  }

  if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
    IRUniversalController_browseBack();
  }
  if (isTouchNavButtonPressedEdge(BTN_DOWN)) {
    IRUniversalController_bumpBrowseSelection(1);
  }
  if (isTouchNavButtonPressedEdge(BTN_UP)) {
    IRUniversalController_bumpBrowseSelection(-1);
  }
  if (isTouchNavButtonPressedEdge(BTN_RIGHT)) {
    IRUniversalController_browseSelect();
  }
}

void IRUniversalController_setup(void) {
  setTouchButtonInputEnabled(true);
  irSetUniversalRemoteNavLabels();
  ir_transmitter_begin(&(IRUniversalController_s_send));
  IRUniversalController_s_profileIdx = 0;
  IRUniversalController_s_uiDrawn = false;
  IRUniversalController_s_screen = IRUniversalController_Screen_Remote;
  IRUniversalController_refreshProfiles();
  IRUniversalController_rebuildCategoriesAndBrands();
  IRUniversalController_drawAll();
}

void IRUniversalController_loop(void) {
  if (feature_active &&
      (feature_exit_requested || featureExitButtonPressed())) {
    feature_exit_requested = true;
    return;
  }

  if (!IRUniversalController_s_uiDrawn) IRUniversalController_drawAll();
  maintainTouchNavBar();
  IRUniversalController_handleUniversalNavButtons();

  const uint32_t now = sys_monotonic_ms();
  static uint32_t lastBtnMs = 0;
  static bool prevLeft = false, prevRight = false, prevUp = false,
              prevDown = false;
  const bool leftNow = isPhysicalButtonPressed(BTN_LEFT);
  const bool rightNow = isPhysicalButtonPressed(BTN_RIGHT);
  const bool upNow = isPhysicalButtonPressed(BTN_UP);
  const bool downNow = isPhysicalButtonPressed(BTN_DOWN);
  enum { kBtnDebounceMs = 320 };
  if ((uint32_t)(now - lastBtnMs) > kBtnDebounceMs) {
    if (IRUniversalController_s_screen == IRUniversalController_Screen_Remote) {
      if (leftNow && !prevLeft) {
        IRUniversalController_changeProfile(-1);
        lastBtnMs = now;
      } else if (rightNow && !prevRight) {
        IRUniversalController_changeProfile(+1);
        lastBtnMs = now;
      } else if (upNow && !prevUp) {
        IRUniversalController_sendKey(IRUniversalController_VolUp);
        lastBtnMs = now;
      } else if (downNow && !prevDown) {
        IRUniversalController_sendKey(IRUniversalController_VolDn);
        lastBtnMs = now;
      }
    } else {
      if (upNow && !prevUp) {
        IRUniversalController_bumpBrowseSelection(-1);
        lastBtnMs = now;
      } else if (downNow && !prevDown) {
        IRUniversalController_bumpBrowseSelection(1);
        lastBtnMs = now;
      } else if (leftNow && !prevLeft) {
        if (IRUniversalController_s_screen ==
            IRUniversalController_Screen_Category) {
          IRUniversalController_s_screen = IRUniversalController_Screen_Remote;
        } else if (IRUniversalController_s_screen ==
                   IRUniversalController_Screen_Brand) {
          IRUniversalController_s_screen =
              IRUniversalController_Screen_Category;
        } else if (IRUniversalController_s_screen ==
                   IRUniversalController_Screen_Profile) {
          IRUniversalController_s_screen = IRUniversalController_Screen_Brand;
        }
        IRUniversalController_s_uiDrawn = false;
        lastBtnMs = now;
      } else if (rightNow && !prevRight) {
        if (IRUniversalController_s_screen ==
            IRUniversalController_Screen_Category) {
          if (!((IRUniversalController_s_categories).size == 0)) {
            IRUniversalController_s_selectedCategory =
                ((text_t*)(IRUniversalController_s_categories).data)[DIV_MAX(
                    0, DIV_MIN(IRUniversalController_s_listSel,
                               (int)(IRUniversalController_s_categories).size -
                                   1))];
            IRUniversalController_rebuildBrandsForCategory(
                IRUniversalController_s_selectedCategory);
            IRUniversalController_s_screen = IRUniversalController_Screen_Brand;
            IRUniversalController_s_listSel = 0;
            IRUniversalController_s_uiDrawn = false;
            lastBtnMs = now;
          }
        } else if (IRUniversalController_s_screen ==
                   IRUniversalController_Screen_Brand) {
          if (!((IRUniversalController_s_brands).size == 0)) {
            IRUniversalController_s_selectedBrand =
                ((text_t*)(IRUniversalController_s_brands).data)[DIV_MAX(
                    0,
                    DIV_MIN(IRUniversalController_s_listSel,
                            (int)(IRUniversalController_s_brands).size - 1))];
            IRUniversalController_rebuildFilteredProfiles();
            IRUniversalController_s_screen =
                IRUniversalController_Screen_Profile;
            IRUniversalController_s_listSel = 0;
            IRUniversalController_s_uiDrawn = false;
            lastBtnMs = now;
          }
        } else if (IRUniversalController_s_screen ==
                   IRUniversalController_Screen_Profile) {
          if (!((IRUniversalController_s_filteredProfileIdx).size == 0)) {
            int idx = ((int*)(IRUniversalController_s_filteredProfileIdx)
                           .data)[DIV_MAX(
                0,
                DIV_MIN(IRUniversalController_s_listSel,
                        (int)(IRUniversalController_s_filteredProfileIdx).size -
                            1))];
            if (idx >= 0 &&
                idx < (int)(IRUniversalController_s_profiles).size) {
              IRUniversalController_s_profileIdx = idx;
              IRUniversalController_s_screen =
                  IRUniversalController_Screen_Remote;
              IRUniversalController_s_uiDrawn = false;
              lastBtnMs = now;
            }
          }
        }
      }
    }
  }
  prevLeft = leftNow;
  prevRight = rightNow;
  prevUp = upNow;
  prevDown = downNow;

  static uint32_t lastTouchMs = 0;
  static bool touchWasDown = false;
  const bool touchNow = isTouchDownDismiss(120);
  enum { kTouchDebounceMs = 320 };
  if (touchNow && !touchWasDown) {
    int x, y;
    touchWasDown = true;
    if (!readTouchXY(&(x), &(y))) return;
    if ((uint32_t)(now - lastTouchMs) < kTouchDebounceMs) return;
    lastTouchMs = now;

    if (IRUniversalController_handleToolbarTouch(x, y)) return;

    if (IRUniversalController_s_screen == IRUniversalController_Screen_Remote) {
      int k = FeatureUI_hit(IRUniversalController_s_keyBtns,
                            (int)IRUniversalController_KeyCount, x, y);
      if (k >= 0) {
        if (!IRUniversalController_s_keyBtns[k].disabled) {
          IRUniversalController_drawKeyButton((IRUniversalController_KeyId)k,
                                              true);
          IRUniversalController_sendKey((IRUniversalController_KeyId)k);
          IRUniversalController_drawKeyButton((IRUniversalController_KeyId)k,
                                              false);
        }
        return;
      }
    } else {
      const int top = IRUniversalController_listTopY();
      const int rows = IRUniversalController_listRowsVisible();
      if (y >= top && y < top + rows * IRUniversalController_LIST_ROW_H) {
        int row = (y - top) / IRUniversalController_LIST_ROW_H;
        int pageStart =
            (rows > 0) ? ((IRUniversalController_s_listSel / rows) * rows) : 0;
        int idx = pageStart + row;
        int max = 0;
        if (IRUniversalController_s_screen ==
            IRUniversalController_Screen_Category)
          max = (int)(IRUniversalController_s_categories).size;
        else if (IRUniversalController_s_screen ==
                 IRUniversalController_Screen_Brand)
          max = (int)(IRUniversalController_s_brands).size;
        else if (IRUniversalController_s_screen ==
                 IRUniversalController_Screen_Profile)
          max = (int)(IRUniversalController_s_filteredProfileIdx).size;
        if (idx >= 0 && idx < max) {
          IRUniversalController_s_listSel = idx;
          IRUniversalController_redrawBrowseListOnly();
          return;
        }
      }
    }
  }

  if (!touchNow) touchWasDown = false;

  sys_sleep_ms(10);
}

void stopIrSession(void) {
  IRRemoteFeature_s_autoTx = false;
  ir_receiver_disable_ir_in(&(IRRemoteFeature_s_recv));
}
