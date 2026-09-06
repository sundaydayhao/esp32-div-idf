#include "c_util.h"
#pragma once

/** Neo-6M / GNSS: GSV sky plot, GGA/RMC fix + time, GSA DOP + satellites in
 * solution. */

void GpsSatelliteScanner_session(void);

/** GPS + WiFi scan logger to SD (CSV). Shares UART/NMEA helpers with satellite
 * scanner in gps.c. */

void GpsWardriver_session(void);

/** Stops background wardriving (UART/SD) before other GPS features use the same
 * UART. */
void GpsWardriver_stopBackgroundIfRunning(void);

/** True while wardriver UI session is active or background wardrive task is
 * running (status bar GPS icon). */
bool GpsWardriver_statusBarGpsIconActive(void);

void GpsWardriver_clearSessionRetry(void);

bool GpsWardriver_consumeSessionRetry(void);
