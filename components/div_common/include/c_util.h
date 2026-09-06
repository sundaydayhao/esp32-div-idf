#pragma once
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Read an entire logical line with reusable ownership; callers free *line. */
static inline ptrdiff_t c_read_line(FILE* file, char** line, size_t* capacity) {
  if (!file || !line || !capacity) return -1;
  size_t used = 0;
  for (;;) {
    int c = fgetc(file);
    if (c == EOF) {
      if (ferror(file) || !used) return -1;
      break;
    }
    if (used + 1 >= *capacity) {
      size_t next = *capacity ? *capacity * 2 : 128;
      if (next <= *capacity || next > PTRDIFF_MAX) return -1;
      char* grown = realloc(*line, next);
      if (!grown) return -1;
      *line = grown;
      *capacity = next;
    }
    if (c == '\n') break;
    (*line)[used++] = (char)c;
  }
  (*line)[used] = 0;
  return (ptrdiff_t)used;
}
/* Bounded C string copy; return the full source length, like snprintf("%s"). */
static inline size_t div_copy_string(char* output, size_t capacity,
                                     const char* input) {
  if (!input) input = "";
  size_t length = strlen(input);
  if (output && capacity) {
    size_t copied = length < capacity ? length : capacity - 1;
    memmove(output, input, copied);
    output[copied] = 0;
  }
  return length;
}
static inline void c_free_pointer(void* slot) {
  void* pointer;
  memcpy(&pointer, slot, sizeof(pointer));
  free(pointer);
}
#define DIV_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define DIV_MIN(a, b)              \
  ({                               \
    __auto_type div_a = (a);       \
    __auto_type div_b = (b);       \
    div_a < div_b ? div_a : div_b; \
  })
#define DIV_MAX(a, b)              \
  ({                               \
    __auto_type div_a = (a);       \
    __auto_type div_b = (b);       \
    div_a > div_b ? div_a : div_b; \
  })
#define DIV_CLAMP(x, lo, hi) DIV_MIN(DIV_MAX((x), (lo)), (hi))
#define DIV_SWAP(a, b)         \
  do {                         \
    __auto_type div_tmp = (a); \
    (a) = (b);                 \
    (b) = div_tmp;             \
  } while (0)
#define DIV_FILL(first, last, value)                             \
  do {                                                           \
    __auto_type div_end = (last);                                \
    for (__auto_type div_p = (first); div_p != div_end; ++div_p) \
      *div_p = (value);                                          \
  } while (0)
#define DIV_FILL_N(first, count, value)                \
  do {                                                 \
    __auto_type div_first = (first);                   \
    size_t div_count = (count);                        \
    for (size_t div_i = 0; div_i < div_count; ++div_i) \
      div_first[div_i] = (value);                      \
  } while (0)
