#pragma once
#include <ctype.h>
#include <inttypes.h>
#include <limits.h>

#include "c_util.h"
/* UI labels, user input and relative SD paths have a bounded value lifetime.
 * Large documents are read with stdio and parsed separately. */
#define TEXT_CAPACITY 256
typedef struct {
  char data[TEXT_CAPACITY];
} text_t;
static inline bool text_prefix_cstr(const char* s, const char* prefix) {
  return strncmp(s, prefix, strlen(prefix)) == 0;
}
static inline bool text_suffix_cstr(const char* s, const char* suffix) {
  size_t a = strlen(s), b = strlen(suffix);
  return a >= b && !memcmp(s + a - b, suffix, b);
}
static inline void text_append_cstr(text_t* s, const char* value) {
  size_t a = strlen(s->data), b = strlen(value);
  size_t n = DIV_MIN(b, sizeof(s->data) - a - 1);
  memmove(s->data + a, value, n);
  s->data[a + n] = 0;
}
static inline text_t text_join_cstr(const char* a, const char* b) {
  text_t out = {0};
  div_copy_string(out.data, sizeof(out.data), a);
  text_append_cstr(&out, b);
  return out;
}
static inline text_t text_string(const char* s) {
  text_t out = {0};
  if (s) div_copy_string(out.data, sizeof(out.data), s);
  return out;
}
static inline text_t text_value(text_t s) { return s; }
static inline text_t text_character(char c) {
  text_t s = {.data = {c, 0}};
  return s;
}
static inline text_t text_signed(int64_t value) {
  text_t s;
  snprintf(s.data, sizeof(s.data), "%" PRId64, value);
  return s;
}
static inline text_t text_unsigned(uint64_t value) {
  text_t s;
  snprintf(s.data, sizeof(s.data), "%" PRIu64, value);
  return s;
}
static inline text_t text_float(double value) {
  text_t s;
  snprintf(s.data, sizeof(s.data), "%.2f", value);
  return s;
}
#define TEXT(x)                          \
  _Generic((x),                          \
      text_t: text_value,                \
      char*: text_string,                \
      const char*: text_string,          \
      char: text_character,              \
      float: text_float,                 \
      double: text_float,                \
      unsigned char: text_unsigned,      \
      unsigned short: text_unsigned,     \
      unsigned int: text_unsigned,       \
      unsigned long: text_unsigned,      \
      unsigned long long: text_unsigned, \
      default: text_signed)(x)
static inline size_t text_length(text_t s) { return strlen(s.data); }
static inline text_t text_concat(text_t a, text_t b) {
  size_t n = strlen(a.data);
  div_copy_string(a.data + n, sizeof(a.data) - n, b.data);
  return a;
}
static inline void text_append(text_t* s, text_t value) {
  if (s) *s = text_concat(*s, value);
}
static inline text_t text_slice(text_t s, size_t begin, size_t end) {
  size_t n = text_length(s);
  begin = DIV_MIN(begin, n);
  end = DIV_MIN(end, n);
  if (end <= begin) return (text_t){0};
  s.data[end] = 0;
  memmove(s.data, s.data + begin, end - begin + 1);
  return s;
}
static inline text_t text_substr(text_t s, size_t begin, size_t count) {
  return text_slice(s, begin, count == SIZE_MAX ? count : begin + count);
}
static inline void text_erase(text_t* s, size_t begin, size_t count) {
  size_t len = text_length(*s);
  if (begin >= len) return;
  size_t end = count == SIZE_MAX ? len : DIV_MIN(len, begin + count);
  memmove(s->data + begin, s->data + end, len - end + 1);
}
static inline bool text_equal(text_t a, text_t b) {
  return strcmp(a.data, b.data) == 0;
}
static inline bool text_equal_fold(text_t a, text_t b) {
  return strcasecmp(a.data, b.data) == 0;
}
static inline bool text_starts_with(text_t s, text_t prefix) {
  return strncmp(s.data, prefix.data, text_length(prefix)) == 0;
}
static inline bool text_ends_with(text_t s, text_t suffix) {
  size_t a = text_length(s), b = text_length(suffix);
  return a >= b && !strcmp(s.data + a - b, suffix.data);
}
static inline int text_find_index(text_t s, text_t needle, size_t from) {
  size_t n = text_length(s);
  if (from > n) return -1;
  char* p = strstr(s.data + from, needle.data);
  return p ? (int)(p - s.data) : -1;
}
static inline int text_rfind_index(text_t s, text_t needle) {
  int last = -1;
  size_t n = text_length(needle);
  if (!n) return text_length(s);
  for (size_t i = 0; i + n <= text_length(s); ++i)
    if (!strncmp(s.data + i, needle.data, n)) last = i;
  return last;
}
static inline void text_trim(text_t* s) {
  if (!s) return;
  size_t begin = 0, end = text_length(*s);
  while (begin < end && isspace((unsigned char)s->data[begin])) ++begin;
  while (end > begin && isspace((unsigned char)s->data[end - 1])) --end;
  *s = text_slice(*s, begin, end);
}
static inline void text_upper(text_t* s) {
  for (char* p = s->data; *p; ++p) *p = toupper((unsigned char)*p);
}
static inline void text_lower(text_t* s) {
  for (char* p = s->data; *p; ++p) *p = tolower((unsigned char)*p);
}
static inline long text_integer(text_t s) { return strtol(s.data, NULL, 10); }
static inline float text_decimal(text_t s) { return strtof(s.data, NULL); }
static inline void text_copy(text_t s, char* out, size_t capacity) {
  if (out && capacity) div_copy_string(out, capacity, s.data);
}
static inline void text_replace_all(text_t* s, text_t from, text_t to) {
  size_t a = text_length(from);
  if (!a) return;
  text_t result = {0};
  const char *p = s->data, *match;
  while ((match = strstr(p, from.data))) {
    size_t n = match - p;
    text_t part = text_string(p);
    if (n < TEXT_CAPACITY) part.data[n] = 0;
    text_append(&result, part);
    text_append(&result, to);
    p = match + a;
  }
  text_append(&result, text_string(p));
  *s = result;
}
static inline text_t text_base(uint64_t value, int base) {
  text_t s = {0};
  if (base < 2 || base > 16) return s;
  char digits[65];
  size_t n = sizeof(digits);
  digits[--n] = 0;
  do {
    digits[--n] = "0123456789ABCDEF"[value % base];
    value /= base;
  } while (value && n);
  return text_string(digits + n);
}
static inline text_t text_precision(double value, int places) {
  text_t s;
  snprintf(s.data, sizeof(s.data), "%.*f", DIV_CLAMP(places, 0, 8), value);
  return s;
}
static inline text_t text_bytes(const char* value, size_t count) {
  text_t s = {0};
  if (value) memcpy(s.data, value, DIV_MIN(count, sizeof(s.data) - 1));
  return s;
}
#define TEXT_FORMAT(x, n)      \
  _Generic((x),                \
      char*: text_bytes,       \
      const char*: text_bytes, \
      float: text_precision,   \
      double: text_precision,  \
      default: text_base)((x), (n))
