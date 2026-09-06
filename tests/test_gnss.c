#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gnss_parser.h"

static void feed(gnss_parser_t* parser, const char* sentence, uint32_t now) {
  gnss_parser_feed(parser, sentence, strlen(sentence), now);
}

int main(void) {
  gnss_parser_t first = GNSS_PARSER_INITIALIZER;
  gnss_parser_t second = GNSS_PARSER_INITIALIZER;
  assert(isnan(first.state.latitude) && first.state.fix_mode == 1);

  const char* gga =
      "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n";
  size_t length = strlen(gga);
  gnss_parser_feed(&first, gga, 12, 10);
  assert(first.state.fix_quality == 0);
  gnss_parser_feed(&first, gga + 12, length - 12, 20);
  assert(first.state.fix_quality == 1 && first.state.satellites_used == 8);
  assert(fabs(first.state.latitude - 48.1173) < 0.00001);
  assert(fabs(first.state.longitude - 11.51666667) < 0.00001);
  assert(fabsf(first.state.altitude_m - 545.4f) < 0.001f);
  assert(!strcmp(first.state.utc, "12:35:19"));
  assert(first.state.last_navigation_ms == 20);
  assert(isnan(second.state.latitude));

  feed(&first, "$GPGSV,1,1,02,04,77,123,42,05,45,067,35\r\n", 30);
  assert(first.state.satellite_count == 2);
  assert(first.state.satellites[0].prn == 4 &&
         first.state.satellites[0].snr == 42);
  assert(first.state.last_satellite_ms == 30);
  feed(&first, "$GPGSA,A,3,04,05,,,,,,,,,,,1.8,1.0,1.5\n", 40);
  assert(first.state.fix_mode == 3);
  assert(first.state.satellites[0].inSolution &&
         first.state.satellites[1].inSolution);
  assert(fabsf(first.state.pdop - 1.8f) < 0.001f);

  feed(&second, "$GPRMC,235959,A,3450.000,S,05822.000,W,0.0,0.0,060926,,,A\n",
       50);
  assert(second.state.navigation_valid);
  assert(second.state.latitude < 0 && second.state.longitude < 0);
  assert(!strcmp(second.state.date, "06/09/26"));
  assert(first.state.latitude > 0);

  /* An oversized line cannot inject its tail as a separate NMEA sentence. */
  char oversized[GNSS_LINE_CAPACITY + 30];
  memset(oversized, 'x', sizeof(oversized));
  gnss_parser_feed(&second, oversized, sizeof(oversized), 60);
  feed(&second, gga, 60);
  assert(second.state.latitude < 0);
  feed(&second, gga, 70);
  assert(second.state.latitude > 0 && second.state.last_navigation_ms == 70);

  feed(&first, "$GPGGA,\n$GPGSV\nnoise\n", 80);
  assert(first.state.last_navigation_ms == 40);
  gnss_parser_reset(&first);
  assert(isnan(first.state.latitude) && first.state.satellite_count == 0);
  assert(!strcmp(first.state.utc, "--:--:--"));
  assert(!first.line_length && !first.discard_line);
  assert(second.state.fix_quality == 1);
  puts("GNSS parser checks passed");
  return 0;
}
