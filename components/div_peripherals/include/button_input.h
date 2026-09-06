#pragma once
#include "pcf8574.h"

/* Shared PCF8574 instance also drives V1 backlight P0. */
pcf8574_t* button_input_device(void);
bool button_input_pressed(int pin);
bool initPcf8574Buttons(void);
uint8_t getPcf8574Address(void);
void logPcf8574ButtonSnapshot(const char* reason, int buttonPin);
void startPcf8574SelfTest(void);
void pollPcf8574SelfTest(void);
void startPcf8574ScanMode(void);
void pollPcf8574ScanMode(void);
