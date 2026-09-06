#pragma once
#include "text_format.h"
typedef struct {
  void* data;
  size_t size, capacity, element_size;
} c_vector_t;
static inline void c_vector_free(c_vector_t* v) {
  free(v->data);
  *v = (c_vector_t){0};
}
static inline bool c_vector_reserve(c_vector_t* v, size_t capacity,
                                    size_t element_size) {
  if (!element_size || (v->element_size && v->element_size != element_size))
    return false;
  v->element_size = element_size;
  if (capacity <= v->capacity) return true;
  if (capacity > SIZE_MAX / element_size) return false;
  void* p = realloc(v->data, capacity * element_size);
  if (!p) return false;
  v->data = p;
  v->capacity = capacity;
  return true;
}
static inline bool c_vector_push(c_vector_t* v, const void* value,
                                 size_t element_size) {
  if (!value || !element_size ||
      (v->element_size && v->element_size != element_size) ||
      v->size > v->capacity || v->size == SIZE_MAX / element_size)
    return false;
  if (v->size == v->capacity) {
    size_t limit = SIZE_MAX / element_size;
    size_t capacity = v->capacity
                          ? (v->capacity <= limit / 2 ? v->capacity * 2 : limit)
                          : DIV_MIN((size_t)8, limit);
    if (!c_vector_reserve(v, capacity, element_size)) return false;
  }
  memcpy((char*)v->data + v->size * element_size, value, element_size);
  ++v->size;
  return true;
}
static inline bool c_vector_resize(c_vector_t* v, size_t count,
                                   size_t element_size) {
  size_t old = v->size;
  if (!c_vector_reserve(v, count, element_size)) return false;
  if (count > old)
    memset((char*)v->data + old * element_size, 0,
           (count - old) * element_size);
  v->size = count;
  return true;
}
static inline c_vector_t c_vector_move(c_vector_t* v) {
  c_vector_t result = *v;
  *v = (c_vector_t){0};
  return result;
}
#define C_VECTOR_PUSH(v, value)                      \
  ({                                                 \
    __auto_type cv_value = (value);                  \
    c_vector_push((v), &cv_value, sizeof(cv_value)); \
  })
#define C_VECTOR_PUSH_AS(v, type, value)             \
  ({                                                 \
    type cv_value = (value);                         \
    c_vector_push((v), &cv_value, sizeof(cv_value)); \
  })
static inline bool c_number_less(long double a, long double b) { return a < b; }
static inline bool c_text_less(text_t a, text_t b) {
  return strcmp(a.data, b.data) < 0;
}
static inline text_t* c_text_find(text_t* begin, text_t* end, text_t value) {
  while (begin != end && !text_equal(*begin, value)) ++begin;
  return begin;
}
static inline bool c_text_add_unique(c_vector_t* v, text_t value) {
  text_t* items = v->data;
  for (size_t i = 0; i < v->size; ++i)
    if (text_equal(items[i], value)) return true;
  return c_vector_push(v, &value, sizeof(value));
}
#define C_LESS(a, b) \
  _Generic((a), text_t: c_text_less, default: c_number_less)((a), (b))
#define C_SORT(first, last)                                  \
  do {                                                       \
    __auto_type cv_first = (first);                          \
    size_t cv_count = (last) - cv_first;                     \
    for (size_t cv_i = 1; cv_i < cv_count; ++cv_i) {         \
      __auto_type cv_value = cv_first[cv_i];                 \
      size_t cv_j = cv_i;                                    \
      while (cv_j && C_LESS(cv_value, cv_first[cv_j - 1])) { \
        cv_first[cv_j] = cv_first[cv_j - 1];                 \
        --cv_j;                                              \
      }                                                      \
      cv_first[cv_j] = cv_value;                             \
    }                                                        \
  } while (0)
