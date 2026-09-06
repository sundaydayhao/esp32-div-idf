#pragma once
#include "c_util.h"

enum { GNSS_MAX_SATELLITES = 48, GNSS_LINE_CAPACITY = 128 };
enum {
  GNSS_SYS_GPS,
  GNSS_SYS_GLO,
  GNSS_SYS_GAL,
  GNSS_SYS_BDS,
  GNSS_SYS_QZS,
  GNSS_SYS_UNK
};
typedef struct {
  uint8_t prn, system;
  int16_t elev, azim, snr;
  bool inSolution;
} gnss_satellite_t;
#define GNSS_SATELLITE_INITIALIZER {0}
typedef struct {
  gnss_satellite_t satellites[GNSS_MAX_SATELLITES];
  int satellite_count;
  uint32_t last_satellite_ms, last_navigation_ms;
  char utc[12], date[12];
  float hdop, pdop;
  uint8_t fix_quality, fix_mode, satellites_used;
  double latitude, longitude;
  float altitude_m;
  bool navigation_valid;
} gnss_state_t;
typedef struct {
  gnss_state_t state;
  char line[GNSS_LINE_CAPACITY];
  size_t line_length;
  uint32_t now_ms;
  bool discard_line;
} gnss_parser_t;
#define GNSS_PARSER_INITIALIZER   \
  {                               \
    .state = {.utc = "--:--:--",  \
              .date = "--/--/--", \
              .hdop = -1.f,       \
              .pdop = -1.f,       \
              .fix_mode = 1,      \
              .latitude = NAN,    \
              .longitude = NAN,   \
              .altitude_m = NAN}  \
  }

/* Caller owns state and serialization. Feed arbitrarily split UART byte chunks.
 */
void gnss_parser_reset(gnss_parser_t* parser);
void gnss_parser_feed(gnss_parser_t* parser, const char* bytes, size_t length,
                      uint32_t now_ms);
