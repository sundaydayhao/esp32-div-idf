#pragma once

#include "board_config.h"
#include "driver/spi_master.h"

// Wiring follows hardware/schematic/v1 and hardware/schematic/v2.
#define DIV_TFT_HOST SPI2_HOST
#define DIV_SD_HOST SPI3_HOST
#define TFT_WIDTH 240
#define TFT_HEIGHT 320
#define TFT_RST -1
#define TFT_COL_OFFSET 0
#define TFT_ROW_OFFSET 0
#define TFT_RGB_ORDER 0x08
#define SPI_FREQUENCY 27000000
#define TFT_BACKLIGHT_ON 1
#define PCF8574_ADDR_MIN 0x20
#define PCF8574_ADDR_MAX 0x27
#define PCF8574_I2C_ADDR CONFIG_ESP32_DIV_PCF8574_ADDR
#define PCF8574_ENABLED HAS_PCF8574_BUTTONS
#define PCF8574_I2C_FREQ_HZ 100000

#if defined(BOARD_ESP32_DIV_V2)
#define TFT_MISO 37
#define TFT_MOSI 35
#define TFT_SCLK 36
#define TFT_CS 17
#define TFT_DC 16
#define BACKLIGHT_PIN 7
#define XPT2046_CS 18
#define XPT2046_CLK 36
#define XPT2046_MISO 37
#define XPT2046_MOSI 35
#define XPT2046_IRQ 255
#define TOUCH_SOFTWARE_SPI 0
#define XPT2046_HOST DIV_TFT_HOST
#define SD_CS 10
#define SD_MOSI 11
#define SD_MISO 13
#define SD_SCLK 12
#define SD_CD 38
#define PCF8574_I2C_SDA 8
#define PCF8574_I2C_SCL 9
#define BTN_UP 7
#define BTN_DOWN 5
#define BTN_LEFT 3
#define BTN_RIGHT 4
#define BTN_SELECT 6
#define CC1101_CS 5
#define SUBGHZ_RX_PIN 3
#define SUBGHZ_TX_PIN 6
#define CE_PIN_1 15
#define CSN_PIN_1 4
#define CE_PIN_2 47
#define CSN_PIN_2 48
#define CE_PIN_3 14
#define CSN_PIN_3 21
#define IR_RX_PIN 21
#define IR_TX_PIN 14
#define GPS_UART_RX 47
#define GPS_UART_TX 48
#define RX_PIN 6
#define TX_PIN 3
#define BATTERY_ADC_PIN 2
#define DIV_LED_PIN 1
#define DIV_LED_COUNT 4
#else
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS 15
#define TFT_DC 2
#define XPT2046_CS 33
#define XPT2046_CLK 25
#define XPT2046_MOSI 32
// Three physically distinct SPI wire groups, but only two hardware hosts.
// Touch uses a dedicated GPIO transport; display and SD keep DMA hosts.
#define TOUCH_SOFTWARE_SPI 1
#define SD_CS 5
#define SD_MOSI 23
#define SD_MISO 19
#define SD_SCLK 18
#define PCF8574_I2C_SDA 21
#define PCF8574_I2C_SCL 22
#define DIV_LED_PIN -1
#define DIV_LED_COUNT 0
#if defined(BOARD_ESP32_DIV_V1)
#define BACKLIGHT_PIN -1
#define BACKLIGHT_PCF_PIN 0
#define XPT2046_MISO 35
#define XPT2046_IRQ 34
#define BTN_UP 6
#define BTN_DOWN 3
#define BTN_LEFT 4
#define BTN_RIGHT 5
#define BTN_SELECT 7
#define CE_PIN_1 4
#define CSN_PIN_1 5
#define CE_PIN_2 26
#define CSN_PIN_2 27
#define CE_PIN_3 16
#define CSN_PIN_3 17
#define SUBGHZ_RX_PIN 16
#define SUBGHZ_TX_PIN 26
#define IR_RX_PIN 4
#define IR_TX_PIN 5
#define GPS_UART_RX 3
#define GPS_UART_TX 1
#define RX_PIN 16
#define TX_PIN 26
#define BATTERY_ADC_PIN 36
#else
#define BACKLIGHT_PIN 21
#define XPT2046_MISO 39
#define XPT2046_IRQ 36
#define TFT_INVERT_COLOR 1
#define BTN_UP 7
#define BTN_DOWN 5
#define BTN_LEFT 3
#define BTN_RIGHT 4
#define BTN_SELECT 6
#define CE_PIN_1 16
#define CSN_PIN_1 17
#define CE_PIN_2 22
#define CSN_PIN_2 27
#define CE_PIN_3 4
#define CSN_PIN_3 25
#define SUBGHZ_RX_PIN 35
#define SUBGHZ_TX_PIN 22
#define IR_RX_PIN 35
#define IR_TX_PIN 4
#define GPS_UART_RX 35
#define GPS_UART_TX 22
#define RX_PIN 35
#define TX_PIN 22
#define BATTERY_ADC_PIN -1
#endif
#define CC1101_CS 27
#endif

#define TFT_BL BACKLIGHT_PIN
#define DIV_SD_CS SD_CS
#define DIV_SD_MOSI SD_MOSI
#define DIV_SD_MISO SD_MISO
#define DIV_SD_SCLK SD_SCLK
#define CC1101_SCK SD_SCLK
#define CC1101_MOSI SD_MOSI
#define CC1101_MISO SD_MISO
#define CC1101_GDO0 SUBGHZ_TX_PIN
#define CC1101_GDO2 SUBGHZ_RX_PIN
#define NRF24_SCK SD_SCLK
#define NRF24_MOSI SD_MOSI
#define NRF24_MISO SD_MISO
#define PN532_SCK SD_SCLK
#define PN532_MISO SD_MISO
#define PN532_MOSI SD_MOSI
#define PN532_SS CONFIG_ESP32_DIV_PN532_CS
#define IR_DEFAULT_KHZ 38
#define PWM_FREQ 5000
#define PWM_CHANNEL 0
#define PWM_RESOLUTION 8
#define BUZZER_INACTIVE_LEVEL (BUZZER_ACTIVE_LEVEL ? 0 : 1)
#define BATTERY_VDIV_R1 100000.0f
#define BATTERY_VDIV_R2 100000.0f

#if defined(BOARD_CYD)
#define NRF24_SCAN_CE CE_PIN_1
#define NRF24_SCAN_CSN CSN_PIN_1
#else
#define NRF24_SCAN_CE CE_PIN_3
#define NRF24_SCAN_CSN CSN_PIN_3
#endif
