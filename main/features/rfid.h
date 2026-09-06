#include "c_util.h"
#pragma once

#include <stdint.h>

/** PN532-based RFID/NFC sessions backed by the project-native Pn532 SPI driver.
 */

/** Probe PN532 on SPI (shared bus). On failure, remounts SD; on success, hands
 * SPI to SD pins until session exit. */
bool RfidNfc_begin(void);

/** True after successful begin(). */
bool RfidNfc_hardwareOk(void);

/** Clear cached clone source (optional). */
void RfidNfc_resetCloneBuffer(void);

/** Clear session-retry flag before entering a feature loop. */
void RfidNfc_clearSessionRetry(void);

/** @return true once if user pressed Ready on a failure result screen. */
bool RfidNfc_consumeSessionRetry(void);

void RfidNfc_sessionCardReader(void);
void RfidNfc_sessionClone(void);
void RfidNfc_sessionErase(void);
void RfidNfc_sessionDump(void);
void RfidNfc_sessionDecodeAccess(void);
void RfidNfc_sessionJamReader(void);
void RfidNfc_sessionDisruptEmulate(void);
void RfidNfc_sessionTagDisrupt(void);
