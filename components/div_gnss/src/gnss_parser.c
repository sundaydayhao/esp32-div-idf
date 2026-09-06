#include "gnss_parser.h"

static void strip_checksum(char* s) {
  char* star = strchr(s, '*');
  if (star) {
    *star = '\0';
  }
  char* cr = strchr(s, '\r');
  if (cr) {
    *cr = '\0';
  }
}

static int split_comma_fields(char* s, const char** fields, int maxFields) {
  int nf = 0;
  fields[nf++] = s;
  for (; *s && nf < maxFields; ++s) {
    if (*s == ',') {
      *s = '\0';
      fields[nf++] = s + 1;
    }
  }
  return nf;
}

static uint8_t talker_system(char c1, char c2) {
  if (c1 == 'G' && c2 == 'P') {
    return GNSS_SYS_GPS;
  }
  if (c1 == 'G' && c2 == 'L') {
    return GNSS_SYS_GLO;
  }
  if (c1 == 'G' && c2 == 'A') {
    return GNSS_SYS_GAL;
  }
  if (c1 == 'G' && c2 == 'B') {
    return GNSS_SYS_BDS;
  }
  if (c1 == 'G' && c2 == 'Q') {
    return GNSS_SYS_QZS;
  }
  if (c1 == 'G' && c2 == 'N') {
    return GNSS_SYS_GPS;
  }
  return GNSS_SYS_UNK;
}

static void parse_optional_int16(const char* p, int16_t* out) {
  if (!p || !*p) {
    (*out) = -1;
    return;
  }
  (*out) = (int16_t)strtol(p, NULL, 10);
}

static double dm_to_deg(const char* dm, char dir) {
  if (!dm || !*dm || (dir != 'N' && dir != 'S' && dir != 'E' && dir != 'W')) {
    return NAN;
  }
  double v = strtod(dm, NULL);
  int deg = (int)(v / 100.0);
  double minutes = v - (double)deg * 100.0;
  double dec = (double)deg + minutes / 60.0;
  if (dir == 'S' || dir == 'W') {
    dec = -dec;
  }
  return dec;
}

static void format_utc_from_field(gnss_parser_t* parser, const char* field) {
  if (!field || strlen(field) < 6) {
    strncpy(parser->state.utc, "--:--:--", sizeof(parser->state.utc));
    return;
  }
  int h = (field[0] - '0') * 10 + (field[1] - '0');
  int m = (field[2] - '0') * 10 + (field[3] - '0');
  int s = (field[4] - '0') * 10 + (field[5] - '0');
  snprintf(parser->state.utc, sizeof(parser->state.utc), "%02d:%02d:%02d",
           h % 100, m % 100, s % 100);
}

static void format_date_from_rmc(gnss_parser_t* parser, const char* field) {
  if (!field || strlen(field) < 6) {
    strncpy(parser->state.date, "--/--/--", sizeof(parser->state.date));
    return;
  }
  char dd[3] = {field[0], field[1], '\0'};
  char mm[3] = {field[2], field[3], '\0'};
  char yy[3] = {field[4], field[5], '\0'};
  snprintf(parser->state.date, sizeof(parser->state.date), "%s/%s/%s", dd, mm,
           yy);
}

static void purge_constellation(gnss_parser_t* parser, uint8_t sys) {
  int w = 0;
  for (int i = 0; i < parser->state.satellite_count; ++i) {
    if (parser->state.satellites[i].system != sys) {
      parser->state.satellites[w++] = parser->state.satellites[i];
    }
  }
  parser->state.satellite_count = w;
}

static void clear_solution_flags(gnss_parser_t* parser, uint8_t sys) {
  for (int i = 0; i < parser->state.satellite_count; ++i) {
    if (parser->state.satellites[i].system == sys) {
      parser->state.satellites[i].inSolution = false;
    }
  }
}

static void mark_solution(gnss_parser_t* parser, uint8_t sys, int prn) {
  if (prn <= 0 || prn > 255) {
    return;
  }
  for (int i = 0; i < parser->state.satellite_count; ++i) {
    if (parser->state.satellites[i].system == sys &&
        parser->state.satellites[i].prn == (uint8_t)prn) {
      parser->state.satellites[i].inSolution = true;
      break;
    }
  }
}

static bool handle_gsv_sentence(gnss_parser_t* parser, char* sentence) {
  if (sentence[0] != '$' || strlen(sentence) < 12) {
    return false;
  }
  /* $xxGSV,t,m,n,... after talker IDs (GP, GN, GL, ...) */
  if (!(sentence[3] == 'G' && sentence[4] == 'S' && sentence[5] == 'V' &&
        sentence[6] == ',')) {
    return false;
  }

  strip_checksum(sentence);
  uint8_t sys = talker_system(sentence[1], sentence[2]);

  const char* fields[28];
  int nf = split_comma_fields(sentence, fields, (int)DIV_ARRAY_SIZE(fields));
  if (nf < 4) {
    return false;
  }

  int totalMsgs = (int)strtol(fields[1], NULL, 10);
  int msgNum = (int)strtol(fields[2], NULL, 10);
  (void)strtol(fields[3], NULL, 10);

  if (totalMsgs >= 1 && msgNum == 1 && sys != GNSS_SYS_UNK) {
    purge_constellation(parser, sys);
  }

  for (int i = 4; i + 3 < nf; i += 4) {
    int prn = (int)strtol(fields[i], NULL, 10);
    if (prn <= 0 || prn > 255) {
      continue;
    }

    int16_t elev = -1, azim = -1, snr = -1;
    parse_optional_int16(fields[i + 1], &(elev));
    parse_optional_int16(fields[i + 2], &(azim));
    if (fields[i + 3][0] != '\0') {
      parse_optional_int16(fields[i + 3], &(snr));
    }

    int idx = parser->state.satellite_count;
    for (int j = 0; j < parser->state.satellite_count; ++j) {
      if (parser->state.satellites[j].system == sys &&
          parser->state.satellites[j].prn == (uint8_t)prn) {
        idx = j;
        break;
      }
    }
    if (idx == parser->state.satellite_count &&
        parser->state.satellite_count < GNSS_MAX_SATELLITES) {
      parser->state.satellite_count++;
    }
    if (idx < GNSS_MAX_SATELLITES) {
      parser->state.satellites[idx].prn = (uint8_t)prn;
      parser->state.satellites[idx].system = sys;
      parser->state.satellites[idx].elev = elev;
      parser->state.satellites[idx].azim = azim;
      parser->state.satellites[idx].snr = snr;
    }
  }

  parser->state.last_satellite_ms = parser->now_ms;
  return true;
}

static bool handle_gsa_sentence(gnss_parser_t* parser, char* sentence) {
  if (sentence[0] != '$' || strlen(sentence) < 10) {
    return false;
  }
  if (!(sentence[3] == 'G' && sentence[4] == 'S' && sentence[5] == 'A' &&
        sentence[6] == ',')) {
    return false;
  }

  strip_checksum(sentence);
  uint8_t sys = talker_system(sentence[1], sentence[2]);

  const char* fields[24];
  int nf = split_comma_fields(sentence, fields, (int)DIV_ARRAY_SIZE(fields));
  if (nf < 7) {
    return false;
  }

  parser->state.fix_mode = (uint8_t)strtol(fields[2], NULL, 10);
  if (parser->state.fix_mode > 3) {
    parser->state.fix_mode = 1;
  }

  int dopBase = nf - 3;
  if (dopBase < 3) {
    return false;
  }

  clear_solution_flags(parser, sys);

  for (int i = 3; i < dopBase; ++i) {
    if (!fields[i][0]) {
      continue;
    }
    int prn = (int)strtol(fields[i], NULL, 10);
    mark_solution(parser, sys, prn);
  }

  if (fields[dopBase][0]) {
    parser->state.pdop = strtof(fields[dopBase], NULL);
  }
  if (fields[dopBase + 1][0]) {
    parser->state.hdop = strtof(fields[dopBase + 1], NULL);
  }

  parser->state.last_navigation_ms = parser->now_ms;
  return true;
}

static bool handle_gga_sentence(gnss_parser_t* parser, char* sentence) {
  if (sentence[0] != '$' || strlen(sentence) < 14) {
    return false;
  }
  if (!(sentence[3] == 'G' && sentence[4] == 'G' && sentence[5] == 'A' &&
        sentence[6] == ',')) {
    return false;
  }

  strip_checksum(sentence);

  const char* fields[20];
  int nf = split_comma_fields(sentence, fields, (int)DIV_ARRAY_SIZE(fields));
  if (nf < 10) {
    return false;
  }

  format_utc_from_field(parser, fields[1]);

  char ns = fields[3][0];
  char ew = fields[5][0];
  parser->state.latitude = dm_to_deg(fields[2], ns);
  parser->state.longitude = dm_to_deg(fields[4], ew);

  parser->state.fix_quality = (uint8_t)strtol(fields[6], NULL, 10);
  parser->state.satellites_used = (uint8_t)strtol(fields[7], NULL, 10);
  if (fields[8][0]) {
    float h = strtof(fields[8], NULL);
    if (h >= 0.f && h < 99.f) {
      parser->state.hdop = h;
    }
  }
  parser->state.altitude_m = NAN;
  if (fields[9][0]) {
    parser->state.altitude_m = strtof(fields[9], NULL);
  }

  parser->state.last_navigation_ms = parser->now_ms;
  return true;
}

static bool handle_rmc_sentence(gnss_parser_t* parser, char* sentence) {
  if (sentence[0] != '$' || strlen(sentence) < 12) {
    return false;
  }
  if (!(sentence[3] == 'R' && sentence[4] == 'M' && sentence[5] == 'C' &&
        sentence[6] == ',')) {
    return false;
  }

  strip_checksum(sentence);

  const char* fields[14];
  int nf = split_comma_fields(sentence, fields, (int)DIV_ARRAY_SIZE(fields));
  if (nf < 10) {
    return false;
  }

  format_utc_from_field(parser, fields[1]);
  parser->state.navigation_valid = (fields[2][0] == 'A');

  char ns = fields[4][0];
  char ew = fields[6][0];
  parser->state.latitude = dm_to_deg(fields[3], ns);
  parser->state.longitude = dm_to_deg(fields[5], ew);

  format_date_from_rmc(parser, fields[9]);

  parser->state.last_navigation_ms = parser->now_ms;
  return true;
}

static void dispatch_nmea_line(gnss_parser_t* parser, char* line) {
  if (line[0] != '$' || strlen(line) < 7) {
    return;
  }
  char t4 = line[3];
  char t5 = line[4];
  char t6 = line[5];

  if (t4 == 'G' && t5 == 'S' && t6 == 'V') {
    handle_gsv_sentence(parser, line);
    return;
  }
  if (t4 == 'G' && t5 == 'S' && t6 == 'A') {
    handle_gsa_sentence(parser, line);
    return;
  }
  if (t4 == 'G' && t5 == 'G' && t6 == 'A') {
    handle_gga_sentence(parser, line);
    return;
  }
  if (t4 == 'R' && t5 == 'M' && t6 == 'C') {
    handle_rmc_sentence(parser, line);
    return;
  }
}

void gnss_parser_reset(gnss_parser_t* parser) {
  if (parser) *parser = (gnss_parser_t)GNSS_PARSER_INITIALIZER;
}

void gnss_parser_feed(gnss_parser_t* parser, const char* bytes, size_t length,
                      uint32_t now_ms) {
  if (!parser || (!bytes && length)) return;
  parser->now_ms = now_ms;
  for (size_t i = 0; i < length; ++i) {
    char c = bytes[i];
    if (c == '\n') {
      parser->line[parser->line_length] = 0;
      if (parser->line_length && !parser->discard_line)
        dispatch_nmea_line(parser, parser->line);
      parser->line_length = 0;
      parser->discard_line = false;
    } else if (c != '\r' && !parser->discard_line) {
      if (parser->line_length + 1 < sizeof(parser->line))
        parser->line[parser->line_length++] = c;
      else
        parser->discard_line = true;
    }
  }
}
