#include "SettingsStore.h"
#include "Touchscreen.h"
#include <TFT_eSPI.h>

extern TFT_eSPI tft;

#ifndef XPT2046_IRQ
#define XPT2046_IRQ 255
#endif

XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);
bool feature_active = false;

static bool s_touchInitialized = false;

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
  rawX = p.x;
  rawY = p.y;
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
#if defined(BOARD_CYD)
  // Same axis order as the RNT CYD touch test (no inverted Y).
  x = ::map(rawX, s.touchXMin, s.touchXMax, 0, TFT_WIDTH - 1);
  y = ::map(rawY, s.touchYMin, s.touchYMax, 0, TFT_HEIGHT - 1);
#else
  x = ::map(rawX, s.touchXMin, s.touchXMax, 0, TFT_WIDTH - 1);
  y = ::map(rawY, s.touchYMax, s.touchYMin, 0, TFT_HEIGHT - 1);
#endif
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
