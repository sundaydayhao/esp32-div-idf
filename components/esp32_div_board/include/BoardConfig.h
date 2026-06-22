#pragma once

// Select the hardware target.
// Leave all BOARD_* lines commented to use the ESP32-DIV V4 wiring.

// #define BOARD_CYD
// #define BOARD_ESP32_DIV_V1
// #define BOARD_ESP32_DIV_V2
#define BOARD_ESP32_DIV_V3
// #define BOARD_ESP32_DIV_V4

// Select the TFT panel attached to the board.
// Leave both commented to use the board default:
// V1/V2/V3 use ILI9341, V4 uses the LCDWiki 4.0" ST7796/ST7796S SPI module.
// #define TFT_PANEL_ILI9341
// #define TFT_PANEL_ST7796

// Button expander selection.
// Leave commented to skip PCF8574 probing and use touch-only navigation.
// V2/V3/V4 schematic uses PCF8574 address 0x20.
#define pcf_ADDR 0x20

// Set to 0 to hide the on-screen touch nav bar (5 footer buttons).
// Touch button input will still work when this is disabled.
#define TOUCH_BUTTON_CUE_ENABLED 1

// V3 BUZZER on IO2. The simple 2-pin BUZZER is an active buzzer, so drive it
// with DC on/off instead of the PWM used for the original CMT transducer.
#define BUZZER_PIN 2
#define BUZZER_USE_PWM 0
#define BUZZER_ACTIVE_LEVEL 1

// Optional per-board touch calibration overrides (raw XPT2046 ADC range).
// CYD defaults (portrait): X 200..3700, Y 240..3800 — run Touch Calibrate if needed.
//#define TOUCH_X_MIN 200
//#define TOUCH_X_MAX 3700
//#define TOUCH_Y_MIN 240
//#define TOUCH_Y_MAX 3800

#if !defined(BOARD_ESP32_DIV_V4) && !defined(BOARD_ESP32_DIV_V3) && !defined(BOARD_ESP32_DIV_V2) && !defined(BOARD_CYD) && !defined(BOARD_ESP32_DIV_V1)
#define BOARD_ESP32_DIV_V4
#endif

#if !defined(TFT_PANEL_ILI9341) && !defined(TFT_PANEL_ST7796)
#if defined(BOARD_ESP32_DIV_V4)
#define TFT_PANEL_ST7796
#else
#define TFT_PANEL_ILI9341
#endif
#endif

#if defined(TFT_PANEL_ILI9341) && defined(TFT_PANEL_ST7796)
#error "Select only one TFT panel: TFT_PANEL_ILI9341 or TFT_PANEL_ST7796"
#endif

#if defined(BOARD_ESP32_DIV_V2) && !defined(TFT_ROTATION)
#define TFT_ROTATION 0
#endif

#if defined(BOARD_ESP32_DIV_V4) && !defined(TFT_ROTATION)
// V4 keeps the V3 board feature set and swaps only the TFT module to ST7796.
#define TFT_ROTATION 0
#endif

#if defined(BOARD_ESP32_DIV_V3) && !defined(TFT_ROTATION)
// V3 keeps the same UI layout as V2, but the 3.2" module is mounted
// differently. Rotate another 90 degrees clockwise to match the physical mount.
#define TFT_ROTATION 0
#endif
