#pragma once

#include "sdkconfig.h"

#if defined(CONFIG_ESP32_DIV_BOARD_V2) && !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "ESP32-DIV V2 requires the esp32s3 target"
#endif
#if (defined(CONFIG_ESP32_DIV_BOARD_V1) ||   \
     defined(CONFIG_ESP32_DIV_BOARD_CYD)) && \
    !defined(CONFIG_IDF_TARGET_ESP32)
#error "ESP32-DIV V1 and CYD require the esp32 target"
#endif

#if defined(CONFIG_ESP32_DIV_BOARD_CYD)
#define BOARD_CYD 1
#elif defined(CONFIG_ESP32_DIV_BOARD_V1)
#define BOARD_ESP32_DIV_V1 1
#elif defined(CONFIG_ESP32_DIV_BOARD_V2)
#define BOARD_ESP32_DIV_V2 1
#else
#error "Select an ESP32-DIV board in Kconfig"
#endif

#define TFT_PANEL_ILI9341 1

#if defined(CONFIG_ESP32_DIV_PCF8574_ENABLED) && \
    CONFIG_ESP32_DIV_PCF8574_ENABLED
#define pcf_ADDR CONFIG_ESP32_DIV_PCF8574_ADDR
#endif

#if defined(CONFIG_ESP32_DIV_TOUCH_BUTTON_CUE_ENABLED)
#define TOUCH_BUTTON_CUE_ENABLED CONFIG_ESP32_DIV_TOUCH_BUTTON_CUE_ENABLED
#endif

#if defined(CONFIG_ESP32_DIV_BUZZER_PIN)
#define BUZZER_PIN CONFIG_ESP32_DIV_BUZZER_PIN
#endif

#if defined(CONFIG_ESP32_DIV_BUZZER_USE_PWM)
#define BUZZER_USE_PWM 1
#else
#define BUZZER_USE_PWM 0
#endif

#if defined(CONFIG_ESP32_DIV_BUZZER_ACTIVE_LEVEL)
#define BUZZER_ACTIVE_LEVEL CONFIG_ESP32_DIV_BUZZER_ACTIVE_LEVEL
#endif

#if defined(CONFIG_ESP32_DIV_TFT_ROTATION)
#define TFT_ROTATION CONFIG_ESP32_DIV_TFT_ROTATION
#endif

#if !defined(BOARD_ESP32_DIV_V2) && !defined(BOARD_CYD) && \
    !defined(BOARD_ESP32_DIV_V1)
#error "No board selected by Kconfig"
#endif

#if defined(BOARD_CYD)
#define ESP32DIV_BOARD_NAME "CYD ESP32-2432S028R"
#define HAS_PCF8574_BUTTONS 0
#define BOARD_HAS_ESP32S3 0
#elif defined(BOARD_ESP32_DIV_V1)
#define ESP32DIV_BOARD_NAME "ESP32-DIV V1 ESP32U"
#ifdef CONFIG_ESP32_DIV_PCF8574_ENABLED
#define HAS_PCF8574_BUTTONS 1
#else
#define HAS_PCF8574_BUTTONS 0
#endif
#define BOARD_HAS_ESP32S3 0
#elif defined(BOARD_ESP32_DIV_V2)
#define ESP32DIV_BOARD_NAME "ESP32-DIV V2"
#ifdef CONFIG_ESP32_DIV_PCF8574_ENABLED
#define HAS_PCF8574_BUTTONS 1
#else
#define HAS_PCF8574_BUTTONS 0
#endif
#define BOARD_HAS_ESP32S3 1
#endif

#if !defined(TFT_PANEL_ILI9341)
#error "Supported ESP32-DIV board profiles require TFT_PANEL_ILI9341"
#endif

#if defined(BOARD_CYD)
#define TOUCH_PROFILE_ID "CYD"
#define TOUCH_X_MIN 200
#define TOUCH_X_MAX 3700
#define TOUCH_Y_MIN 240
#define TOUCH_Y_MAX 3800
#elif defined(BOARD_ESP32_DIV_V1)
#define TOUCH_PROFILE_ID "ESP32_DIV_V1"
#define TOUCH_X_MIN 300
#define TOUCH_X_MAX 3800
#define TOUCH_Y_MIN 300
#define TOUCH_Y_MAX 3800
#elif defined(BOARD_ESP32_DIV_V2)
#define TOUCH_PROFILE_ID "ESP32_DIV_V2"
#define TOUCH_X_MIN 280
#define TOUCH_X_MAX 3850
#define TOUCH_Y_MIN 320
#define TOUCH_Y_MAX 3750
#endif

#ifndef TOUCH_PROFILE_ID
#define TOUCH_PROFILE_ID "GENERIC"
#endif

#ifndef TFT_ROTATION
#define TFT_ROTATION 0
#endif
