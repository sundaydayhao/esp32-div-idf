#include "storage_file.h"

#include <stdarg.h>
#include <sys/stat.h>
storage_file_t storage_file_open(const char* path, const char* mode) {
  storage_file_t f = {0};
  if (!storage_path(path, f.path, sizeof(f.path))) return f;
  div_copy_string(f.name, sizeof(f.name), path);
  struct stat st;
  if (!stat(f.path, &st) && S_ISDIR(st.st_mode))
    f.directory = opendir(f.path);
  else
    f.file = storage_open(path, mode);
  return f;
}
void storage_file_close(storage_file_t* f) {
  if (!f) return;
  if (f->file) fclose(f->file);
  if (f->directory) closedir(f->directory);
  *f = (storage_file_t){0};
}
storage_file_t storage_file_next(storage_file_t* f) {
  if (!f->directory) return (storage_file_t){0};
  struct dirent* entry;
  while ((entry = readdir(f->directory))) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    char path[STORAGE_PATH_MAX];
    int n = snprintf(
        path, sizeof(path), "%s%s%s", f->name,
        strlen(f->name) && f->name[strlen(f->name) - 1] == '/' ? "" : "/",
        entry->d_name);
    if (n < 0 || n >= sizeof(path)) continue;
    return storage_file_open(path, "rb");
  }
  return (storage_file_t){0};
}
size_t storage_file_size(storage_file_t* f) {
  struct stat st;
  return f->file && !fstat(fileno(f->file), &st) ? st.st_size : 0;
}
bool storage_file_seek(storage_file_t* f, size_t offset) {
  return f->file && offset <= LONG_MAX &&
         !fseek(f->file, (long)offset, SEEK_SET);
}
int storage_file_available(storage_file_t* f) {
  if (!f->file) return 0;
  long pos = ftell(f->file);
  size_t n = storage_file_size(f);
  return pos >= 0 && (size_t)pos < n ? DIV_MIN(n - pos, INT_MAX) : 0;
}
int storage_file_read(storage_file_t* f, void* buffer, size_t capacity) {
  return f->file ? (int)fread(buffer, 1, capacity, f->file) : -1;
}
int storage_file_read_byte(storage_file_t* f) {
  return f->file ? fgetc(f->file) : -1;
}
size_t storage_file_read_until(storage_file_t* f, char delim, void* buffer,
                               size_t capacity) {
  if (!f->file || !buffer) return 0;
  size_t n = 0;
  while (n < capacity) {
    int c = fgetc(f->file);
    if (c == EOF || c == (unsigned char)delim) break;
    ((char*)buffer)[n++] = c;
  }
  return n;
}
text_t storage_file_read_line(storage_file_t* f, char delim) {
  text_t text = {0};
  if (!f || !f->file) return text;
  size_t n = 0;
  int c;
  /* The bounded preview still consumes the entire logical line. */
  while ((c = fgetc(f->file)) != EOF && c != (unsigned char)delim) {
    if (n < sizeof(text.data) - 1) text.data[n++] = (char)c;
  }
  return text;
}
size_t storage_file_write(storage_file_t* f, const void* buffer, size_t size) {
  return f->file ? fwrite(buffer, 1, size, f->file) : 0;
}
size_t storage_file_print(storage_file_t* f, text_t text) {
  return storage_file_write(f, text.data, text_length(text));
}
size_t storage_file_println(storage_file_t* f, text_t text) {
  return storage_file_print(f, text) + storage_file_write(f, "\n", 1);
}
size_t storage_file_printf(storage_file_t* f, const char* format, ...) {
  if (!f->file) return 0;
  va_list args;
  va_start(args, format);
  int n = vfprintf(f->file, format, args);
  va_end(args);
  return n > 0 ? n : 0;
}
