#include "c_util.h"
#ifndef TOUCH_UI_H
#define TOUCH_UI_H

#include "settings_store.h"
#include "shared.h"

#ifndef TOUCH_X_MIN
#define TOUCH_X_MIN 300
#endif
#ifndef TOUCH_X_MAX
#define TOUCH_X_MAX 3800
#endif
#ifndef TOUCH_Y_MIN
#define TOUCH_Y_MIN 300
#endif
#ifndef TOUCH_Y_MAX
#define TOUCH_Y_MAX 3800
#endif
#ifndef DISPLAY_WIDTH
#define DISPLAY_WIDTH 240
#endif
#ifndef DISPLAY_HEIGHT
#define DISPLAY_HEIGHT 320
#endif

extern bool feature_active;

void setupTouchscreen(void);

/** True when the panel is pressed. Use this instead of touch_driver_pending()
 * directly. */
bool isTouchDown(uint16_t zThresh);
/** Lower pressure threshold for release detection and light taps. */
bool isTouchDownDismiss(uint16_t zThresh);
/** Raw ADC coords for calibration (before screen mapping). */
bool readTouchRawXY(int16_t* x, int16_t* y, uint16_t zThresh);

bool readTouchXY(int* x, int* y);

bool readTouchXYDismiss(int* x, int* y);

void injectTouchRawXYForTest(int16_t x, int16_t y, uint16_t z, uint8_t reads);
void injectTouchScreenXYForTest(int x, int y, uint8_t reads);
void clearInjectedTouchForTest(void);

#endif
