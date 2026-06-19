#pragma once

// Select the hardware target.
// Leave all lines commented to use the ESP32-DIV V2 wiring.

// #define BOARD_CYD
// #define BOARD_ESP32_DIV_V1
 #define BOARD_ESP32_DIV_V2

// Select the TFT panel attached to the board.
// ILI9341 is the default for ESP32-DIV V2 hardware.
// #define TFT_PANEL_ST7796
#define TFT_PANEL_ILI9341

// Button expander selection.
// Leave commented to skip PCF8574 probing and use touch-only navigation.
// Define the address only when the board really has a PCF8574 at that I2C address.
// #define pcf_ADDR 0x20

// Set to 0 to hide the on-screen touch nav bar (5 footer buttons).
// Touch button input will still work when this is disabled.
#define TOUCH_BUTTON_CUE_ENABLED 1

// Optional per-board touch calibration overrides (raw XPT2046 ADC range).
// CYD defaults (portrait): X 200..3700, Y 240..3800 — run Touch Calibrate if needed.
//#define TOUCH_X_MIN 200
//#define TOUCH_X_MAX 3700
//#define TOUCH_Y_MIN 240
//#define TOUCH_Y_MAX 3800

#if !defined(BOARD_ESP32_DIV_V2) && !defined(BOARD_CYD) && !defined(BOARD_ESP32_DIV_V1)
#define BOARD_ESP32_DIV_V2
#endif

#if !defined(TFT_PANEL_ILI9341) && !defined(TFT_PANEL_ST7796)
#define TFT_PANEL_ILI9341
#endif

#if defined(TFT_PANEL_ILI9341) && defined(TFT_PANEL_ST7796)
#error "Select only one TFT panel: TFT_PANEL_ILI9341 or TFT_PANEL_ST7796"
#endif

#if defined(BOARD_ESP32_DIV_V2) && !defined(TOUCH_ROTATION)
// Match the vendor touch demo for the MSP3218/ILI9341 module.
#define TOUCH_ROTATION 3
#endif
