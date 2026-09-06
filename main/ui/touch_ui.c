#include "touch_ui.h"

#include "system_utils.h"
#include "touch_input.h"

/* The application owns calibration, screen rotation and test event injection.
 */
static int16_t s_injectedRawX, s_injectedRawY;
static uint16_t s_injectedRawZ;
static uint8_t s_injectedRawReads;
static int s_injectedScreenX, s_injectedScreenY;
static uint8_t s_injectedScreenReads;

#ifndef TOUCH_ROTATION
#define TOUCH_ROTATION TFT_ROTATION
#endif

static bool touchSampleOk(uint16_t threshold, int16_t* x, int16_t* y) {
  if (s_injectedRawReads > 0) {
    if (s_injectedRawZ < threshold) return false;
    --s_injectedRawReads;
    *x = s_injectedRawX;
    *y = s_injectedRawY;
    return true;
  }
  return touch_input_read_raw(threshold, x, y);
}

void setupTouchscreen(void) { ESP_ERROR_CHECK(touch_input_init()); }

bool isTouchDown(uint16_t zThresh) {
  if (s_injectedScreenReads > 0) {
    return true;
  }
  int16_t x = 0;
  int16_t y = 0;
  return touchSampleOk(zThresh, &(x), &(y));
}

bool isTouchDownDismiss(uint16_t zThresh) { return isTouchDown(zThresh); }

bool readTouchRawXY(int16_t* x, int16_t* y, uint16_t zThresh) {
  return touchSampleOk(zThresh, &((*x)), &((*y)));
}

static void mapTouchToScreen(int16_t rawX, int16_t rawY, int* x, int* y) {
  __auto_type s = &((*settings()));

  int x0 = 0;
  int y0 = 0;

#if defined(BOARD_CYD)
  x0 = sys_scale(rawX, (*s).touchXMin, (*s).touchXMax, 0, TFT_WIDTH - 1);
  y0 = sys_scale(rawY, (*s).touchYMin, (*s).touchYMax, 0, TFT_HEIGHT - 1);
#else
  x0 = sys_scale(rawX, (*s).touchXMin, (*s).touchXMax, 0, TFT_WIDTH - 1);
  y0 = sys_scale(rawY, (*s).touchYMax, (*s).touchYMin, 0, TFT_HEIGHT - 1);
#endif

  switch (TOUCH_ROTATION & 3) {
    case 0:
      (*x) = x0;
      (*y) = y0;
      break;
    case 1:
      (*x) = TFT_HEIGHT - 1 - y0;
      (*y) = x0;
      break;
    case 2:
      (*x) = TFT_WIDTH - 1 - x0;
      (*y) = TFT_HEIGHT - 1 - y0;
      break;
    case 3:
      (*x) = y0;
      (*y) = TFT_WIDTH - 1 - x0;
      break;
  }

  (*x) = DIV_CLAMP((*x), 0, TFT_WIDTH - 1);
  (*y) = DIV_CLAMP((*y), 0, TFT_HEIGHT - 1);
}

bool readTouchXY(int* x, int* y) {
  if (s_injectedScreenReads > 0) {
    --s_injectedScreenReads;
    (*x) = s_injectedScreenX;
    (*y) = s_injectedScreenY;
    return true;
  }

  int16_t rawX = 0;
  int16_t rawY = 0;
  if (!touchSampleOk(200, &(rawX), &(rawY))) {
    return false;
  }
  mapTouchToScreen(rawX, rawY, &((*x)), &((*y)));
  return true;
}

bool readTouchXYDismiss(int* x, int* y) {
  if (s_injectedScreenReads > 0) {
    --s_injectedScreenReads;
    (*x) = s_injectedScreenX;
    (*y) = s_injectedScreenY;
    return true;
  }

  int16_t rawX = 0;
  int16_t rawY = 0;
  if (!touchSampleOk(120, &(rawX), &(rawY))) {
    return false;
  }
  mapTouchToScreen(rawX, rawY, &((*x)), &((*y)));
  return true;
}

void injectTouchRawXYForTest(int16_t x, int16_t y, uint16_t z, uint8_t reads) {
  s_injectedRawX = x;
  s_injectedRawY = y;
  s_injectedRawZ = z;
  s_injectedRawReads = reads;
}

void injectTouchScreenXYForTest(int x, int y, uint8_t reads) {
  s_injectedScreenX = x;
  s_injectedScreenY = y;
  s_injectedScreenReads = reads;
}

void clearInjectedTouchForTest(void) {
  s_injectedRawReads = 0;
  s_injectedScreenReads = 0;
}
