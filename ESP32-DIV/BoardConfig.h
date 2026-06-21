#pragma once

// Select the hardware target.
// Leave all lines commented to use the ESP32-DIV V3 wiring.

// #define BOARD_CYD
// #define BOARD_ESP32_DIV_V1
// #define BOARD_ESP32_DIV_V2
#define BOARD_ESP32_DIV_V3

// Select the TFT panel attached to the board.
// ILI9341 is the default for ESP32-DIV V2 hardware.
// #define TFT_PANEL_ST7796
#define TFT_PANEL_ILI9341

// Button expander selection.
// Leave commented to skip PCF8574 probing and use touch-only navigation.
// v3 schematic uses PCF8574 address 0x20.
#define pcf_ADDR 0x20

// Set to 0 to hide the on-screen touch nav bar (5 footer buttons).
// Touch button input will still work when this is disabled.
#define TOUCH_BUTTON_CUE_ENABLED 1

// Disable buzzer for now. Set this to the wired GPIO, e.g. 2, to re-enable later.
#define BUZZER_PIN -1

// Optional per-board touch calibration overrides (raw XPT2046 ADC range).
// CYD defaults (portrait): X 200..3700, Y 240..3800 — run Touch Calibrate if needed.
//#define TOUCH_X_MIN 200
//#define TOUCH_X_MAX 3700
//#define TOUCH_Y_MIN 240
//#define TOUCH_Y_MAX 3800

#if !defined(BOARD_ESP32_DIV_V3) && !defined(BOARD_ESP32_DIV_V2) && !defined(BOARD_CYD) && !defined(BOARD_ESP32_DIV_V1)
#define BOARD_ESP32_DIV_V2
#endif

#if !defined(TFT_PANEL_ILI9341) && !defined(TFT_PANEL_ST7796)
#define TFT_PANEL_ILI9341
#endif

#if defined(TFT_PANEL_ILI9341) && defined(TFT_PANEL_ST7796)
#error "Select only one TFT panel: TFT_PANEL_ILI9341 or TFT_PANEL_ST7796"
#endif

#if defined(BOARD_ESP32_DIV_V3) && !defined(TOUCH_ROTATION)
// Match the board display rotation so touch coordinates land in the same space
// as the TFT menu layout.
#define TOUCH_ROTATION 0
#endif

#if defined(BOARD_ESP32_DIV_V2) && !defined(TOUCH_ROTATION)
// Match the board display rotation so touch coordinates land in the same space
// as the TFT menu layout.
#define TOUCH_ROTATION 0
#endif
