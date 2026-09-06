#pragma once
#include <dirent.h>

#include "storage.h"
#include "text_format.h"
typedef struct {
  FILE* file;
  DIR* directory;
  char path[STORAGE_PATH_MAX];
  char name[STORAGE_PATH_MAX];
} storage_file_t;
storage_file_t storage_file_open(const char* path, const char* mode);
storage_file_t storage_file_next(storage_file_t* file);
void storage_file_close(storage_file_t* file);
static inline storage_file_t storage_file_move(storage_file_t* file) {
  storage_file_t out = *file;
  *file = (storage_file_t){0};
  return out;
}
static inline void storage_file_replace(storage_file_t* file,
                                        storage_file_t value) {
  storage_file_close(file);
  *file = value;
}
size_t storage_file_size(storage_file_t* file);
bool storage_file_seek(storage_file_t* file, size_t offset);
int storage_file_available(storage_file_t* file);
int storage_file_read(storage_file_t* file, void* buffer, size_t capacity);
int storage_file_read_byte(storage_file_t* file);
size_t storage_file_read_until(storage_file_t* file, char delim, void* buffer,
                               size_t capacity);
text_t storage_file_read_line(storage_file_t* file, char delim);
static inline ptrdiff_t storage_file_getline(storage_file_t* file, char** line,
                                             size_t* capacity) {
  return file ? c_read_line(file->file, line, capacity) : -1;
}
size_t storage_file_write(storage_file_t* file, const void* buffer,
                          size_t size);
static inline size_t storage_file_print_cstr(storage_file_t* file,
                                             const char* text) {
  return storage_file_write(file, text, strlen(text));
}
static inline size_t storage_file_print_line_cstr(storage_file_t* file,
                                                  const char* text) {
  return storage_file_print_cstr(file, text) +
         storage_file_write(file, "\n", 1);
}
size_t storage_file_print(storage_file_t* file, text_t text);
size_t storage_file_println(storage_file_t* file, text_t text);
size_t storage_file_printf(storage_file_t* file, const char* format, ...)
    __attribute__((format(printf, 2, 3)));
static inline bool storage_file_valid(const storage_file_t* f) {
  return f->file || f->directory;
}
static inline void storage_file_flush(storage_file_t* f) {
  if (f->file) fflush(f->file);
}
