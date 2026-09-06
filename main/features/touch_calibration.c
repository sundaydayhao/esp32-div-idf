#include "c_vector.h"
#include "config.h"
#include "icon.h"
#include "storage_file.h"
#include "system_features.h"
#include "touch_ui.h"
#include "ui_utils.h"

static int TouchCalib_stepIdx = 0;
static bool TouchCalib_waitingRelease = false;
static uint16_t TouchCalib_xs[4], TouchCalib_ys[4];
static const int TouchCalib_pts[4][2] = {{20, 20},
                                         {TFT_WIDTH - 20, 20},
                                         {TFT_WIDTH - 20, TFT_HEIGHT - 20},
                                         {20, TFT_HEIGHT - 20}};

static void TouchCalib_drawTarget(int x, int y) {
  display_fill_screen(&(tft), TFT_NAVY);
  display_draw_rect(&(tft), 0, 0, TFT_WIDTH, TFT_HEIGHT, UI_ICON);
  display_draw_circle(&(tft), x, y, 10, UI_ICON);
  display_fill_circle(&(tft), x, y, 3, UI_ICON);
  display_draw_line(&(tft), x - 14, y, x + 14, y, UI_ICON);
  display_draw_line(&(tft), x, y - 14, x, y + 14, UI_ICON);
  display_set_text_color_2(&(tft), UI_TEXT, TFT_NAVY);
  display_set_text_font(&(tft), 1);
  display_set_cursor(&(tft), 8, 8);
  display_print(&(tft), "Touch Calibration");
  display_set_text_font(&(tft), 1);
  display_set_cursor(&(tft), 8, 28);
  display_printf(&(tft), "Step %d / 4", TouchCalib_stepIdx + 1);
  display_set_cursor(&(tft), 8, 42);
  display_set_text_color_2(&(tft), UI_TEXT, UI_BG);
  display_print(&(tft), "Touch the target");
  ESP_LOGI("esp32_div_touch", "TouchCalib draw step=%d target=(%d,%d)",
           TouchCalib_stepIdx + 1, x, y);
}

static void TouchCalib_drawReleasePrompt(void) {
  display_fill_rect(&(tft), 8, TFT_HEIGHT - 20, TFT_WIDTH - 16, 12, TFT_NAVY);
  display_set_text_color_2(&(tft), UI_TEXT, TFT_NAVY);
  display_set_text_font(&(tft), 1);
  display_set_cursor(&(tft), 8, TFT_HEIGHT - 18);
  display_print(&(tft), "Release touch");
}

static uint16_t TouchCalib_avg2(uint16_t a, uint16_t b) {
  return (uint16_t)(((uint32_t)a + (uint32_t)b) / 2);
}

static bool TouchCalib_rangeWideEnough(uint16_t a, uint16_t b) {
  int d = (int)a - (int)b;
  if (d < 0) d = -d;
  return d >= 500;
}

static void TouchCalib_saveCalibration(void) {
  const uint16_t xLeft = TouchCalib_avg2(TouchCalib_xs[0], TouchCalib_xs[3]);
  const uint16_t xRight = TouchCalib_avg2(TouchCalib_xs[1], TouchCalib_xs[2]);
  const uint16_t yTop = TouchCalib_avg2(TouchCalib_ys[0], TouchCalib_ys[1]);
  const uint16_t yBottom = TouchCalib_avg2(TouchCalib_ys[2], TouchCalib_ys[3]);
  const bool rangesOk = TouchCalib_rangeWideEnough(xLeft, xRight) &&
                        TouchCalib_rangeWideEnough(yTop, yBottom);

  __auto_type s = &((*settings()));
  bool ok = false;
  if (rangesOk) {
    (*s).touchXMin = xLeft;
    (*s).touchXMax = xRight;
#if defined(BOARD_CYD)
    (*s).touchYMin = yTop;
    (*s).touchYMax = yBottom;
#else
    (*s).touchYMin = yBottom;
    (*s).touchYMax = yTop;
#endif
    ok = settingsSave();
  }

  display_fill_screen(&(tft), UI_BG);
  display_set_text_color_2(&(tft), ok ? UI.ok : UI.warn, UI_BG);
  display_set_cursor(&(tft), 70, 8);
  display_print(&(tft),
                (TEXT(ok ? "Calibration Saved"
                         : (rangesOk ? "Save FAILED" : "Calibration FAILED")))
                    .data);

  display_set_text_color_2(&(tft), UI_TEXT, UI_BG);
  display_set_cursor(&(tft), 70, 28);
  display_printf(
      &(tft), "X:[%u..%u] Y:[%u..%u]", rangesOk ? (*s).touchXMin : xLeft,
      rangesOk ? (*s).touchXMax : xRight, rangesOk ? (*s).touchYMin : yBottom,
      rangesOk ? (*s).touchYMax : yTop);

  ESP_LOGI("esp32_div_touch",
           "TouchCalib %s xLeft=%u xRight=%u yTop=%u yBottom=%u "
           "saved=[%u..%u,%u..%u]",
           ok ? "saved" : "failed", xLeft, xRight, yTop, yBottom,
           (*s).touchXMin, (*s).touchXMax, (*s).touchYMin, (*s).touchYMax);

  sys_sleep_ms(1200);
  feature_exit_requested = true;
}

void TouchCalib_setup(void) {
  TouchCalib_stepIdx = 0;
  TouchCalib_waitingRelease = false;
  ESP_LOGI("esp32_div_touch", "TouchCalib setup");
  TouchCalib_drawTarget(TouchCalib_pts[0][0], TouchCalib_pts[0][1]);
}

void TouchCalib_loop(void) {
  if (TouchCalib_waitingRelease) {
    if (isTouchDownDismiss(120)) {
      sys_sleep_ms(40);
      return;
    }
    TouchCalib_waitingRelease = false;
    if (TouchCalib_stepIdx >= 4) {
      TouchCalib_saveCalibration();
      return;
    }
    TouchCalib_drawTarget(TouchCalib_pts[TouchCalib_stepIdx][0],
                          TouchCalib_pts[TouchCalib_stepIdx][1]);
    sys_sleep_ms(120);
    return;
  }

  if (TouchCalib_stepIdx >= 4) {
    TouchCalib_saveCalibration();
    return;
  }

  int16_t rx = 0, ry = 0;
  if (readTouchRawXY(&(rx), &(ry), 200)) {
    ESP_LOGI("esp32_div_touch", "TouchCalib hit step=%d raw=(%d,%d)",
             TouchCalib_stepIdx + 1, (int)rx, (int)ry);
    TouchCalib_xs[TouchCalib_stepIdx] = (uint16_t)rx;
    TouchCalib_ys[TouchCalib_stepIdx] = (uint16_t)ry;
    TouchCalib_stepIdx++;
    TouchCalib_waitingRelease = true;
    TouchCalib_drawReleasePrompt();
  }
  sys_sleep_ms(40);
}
