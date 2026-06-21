#include "SettingsStore.h"
#include "Touchscreen.h"
#include <TFT_eSPI.h>
#include "esp_log.h"

extern TFT_eSPI tft;

#ifndef XPT2046_IRQ
#define XPT2046_IRQ 255
#endif

XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);
bool feature_active = false;

static bool s_touchInitialized = false;
static const char* TAG = "esp32_div_touch";
static uint32_t s_lastTouchDebugMs = 0;

#ifndef TOUCH_ROTATION
#if defined(BOARD_ESP32_DIV_V2)
#define TOUCH_ROTATION 0
#else
#define TOUCH_ROTATION TFT_ROTATION
#endif
#endif

static bool touchSampleOk(uint16_t zThresh, int16_t& rawX, int16_t& rawY) {
  if (!s_touchInitialized) {
    return false;
  }
  if (!ts.tirqTouched()) {
    return false;
  }
  TS_Point p = ts.getPoint();
  if (p.z < (int16_t)zThresh) {
    return false;
  }
  if (p.x <= 5 || p.x >= 4090 || p.y <= 5 || p.y >= 4090) {
    return false;
  }
  rawX = p.x;
  rawY = p.y;
  const uint32_t now = millis();
  if (now - s_lastTouchDebugMs >= 250) {
    s_lastTouchDebugMs = now;
    ESP_LOGI(TAG, "raw touch x=%d y=%d z=%d irq=%d", (int)p.x, (int)p.y, (int)p.z,
             (int)ts.tirqTouched());
  }
  return true;
}

void setupTouchscreen() {
  if (s_touchInitialized) {
    return;
  }

  ts.begin();
  ts.setRotation(TOUCH_ROTATION);

  s_touchInitialized = true;
}

bool isTouchDown(uint16_t zThresh) {
  int16_t x = 0;
  int16_t y = 0;
  return touchSampleOk(zThresh, x, y);
}

bool isTouchDownDismiss(uint16_t zThresh) {
  return isTouchDown(zThresh);
}

bool readTouchRawXY(int16_t& x, int16_t& y, uint16_t zThresh) {
  return touchSampleOk(zThresh, x, y);
}

static void mapTouchToScreen(int16_t rawX, int16_t rawY, int& x, int& y) {
  auto& s = settings();
  auto clampToScreen = [](long v, long maxV) -> int {
    if (v < 0) return 0;
    if (v > maxV) return (int)maxV;
    return (int)v;
  };
  int x0 = 0;
  int y0 = 0;

#if defined(BOARD_CYD)
  x0 = ::map(rawX, s.touchXMin, s.touchXMax, 0, TFT_WIDTH - 1);
  y0 = ::map(rawY, s.touchYMin, s.touchYMax, 0, TFT_HEIGHT - 1);
#else
  x0 = ::map(rawX, s.touchXMin, s.touchXMax, 0, TFT_WIDTH - 1);
  y0 = ::map(rawY, s.touchYMax, s.touchYMin, 0, TFT_HEIGHT - 1);
#endif

  switch (TOUCH_ROTATION & 3) {
    case 0:
      x = x0;
      y = y0;
      break;
    case 1:
      x = TFT_WIDTH - 1 - y0;
      y = x0;
      break;
    case 2:
      x = TFT_WIDTH - 1 - x0;
      y = TFT_HEIGHT - 1 - y0;
      break;
    case 3:
      x = y0;
      y = TFT_HEIGHT - 1 - x0;
      break;
  }

  x = clampToScreen(x, TFT_WIDTH - 1);
  y = clampToScreen(y, TFT_HEIGHT - 1);
}

bool readTouchXY(int& x, int& y) {
  int16_t rawX = 0;
  int16_t rawY = 0;
  if (!touchSampleOk(200, rawX, rawY)) {
    return false;
  }
  mapTouchToScreen(rawX, rawY, x, y);
  return true;
}

bool readTouchXYDismiss(int& x, int& y) {
  int16_t rawX = 0;
  int16_t rawY = 0;
  if (!touchSampleOk(120, rawX, rawY)) {
    return false;
  }
  mapTouchToScreen(rawX, rawY, x, y);
  return true;
}
