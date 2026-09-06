#pragma once
#include "text_format.h"

/* Only one foreground feature owns these UI text slots at a time. Background
 * Wi-Fi/BLE scans keep separate records; stop callbacks before switching owner.
 * Sharing the scratch space keeps the ESP32 boards independent of PSRAM. */
enum { FEATURE_TEXT_SLOTS = 80 };
extern text_t feature_text_slots[FEATURE_TEXT_SLOTS];
void feature_text_reset(void);
