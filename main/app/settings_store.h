#include "c_util.h"
#pragma once

#include <stdint.h>

#include "shared.h"

#ifndef SETTINGS_NVS_NAMESPACE
#define SETTINGS_NVS_NAMESPACE "app_settings"
#endif

typedef struct AppSettings {
  uint8_t brightness;
  Theme theme;
  uint8_t accentColor;

  bool neopixelEnabled;

  bool autoWifiScan;
  bool autoBleScan;

  uint16_t touchXMin;
  uint16_t touchXMax;
  uint16_t touchYMin;
  uint16_t touchYMax;

} AppSettings;
#define AppSettings_INITIALIZER \
  {.brightness = BKL_LEVEL_MED, \
   .theme = Theme_Dark,         \
   .accentColor = 0,            \
   .neopixelEnabled = false,    \
   .autoWifiScan = true,        \
   .autoBleScan = true,         \
   .touchXMin = TOUCH_X_MIN,    \
   .touchXMax = TOUCH_X_MAX,    \
   .touchYMin = TOUCH_Y_MIN,    \
   .touchYMax = TOUCH_Y_MAX}
;

typedef struct AccentOption {
  const char* name;
  uint16_t color565;
} AccentOption;
#define AccentOption_INITIALIZER {0}
;

enum { ACCENT_PRESET_COUNT = 7 };

AppSettings* settings(void);
uint8_t accentPresetClamp(uint8_t preset);
uint16_t accentColor565(uint8_t preset);
const char* accentPresetName(uint8_t preset);

const char* settingsBoardProfileId(void);
void settingsApplyBoardTouchDefaults(void);
bool settingsLoad(void);
bool settingsSave(void);

_Static_assert(sizeof(AppSettings) == 14, "Preserve the NVS settings layout");
