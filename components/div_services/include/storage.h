#pragma once
#include "c_util.h"
#include "esp_err.h"
#define STORAGE_PATH_MAX 256
typedef struct {
  char name[256];
  bool directory;
  size_t size;
} storage_entry_t;
esp_err_t storage_mount(void);
void storage_unmount(void);
bool storage_mount_card(int cs);
bool storage_mounted(void);
bool storage_path(const char* path, char* output, size_t capacity);
FILE* storage_open(const char* path, const char* mode);
bool storage_exists(const char* path);
bool storage_mkdir(const char* path);
bool storage_remove(const char* path);
size_t storage_list(const char* path, storage_entry_t* entries,
                    size_t capacity);
bool storage_next_path(const char* directory, const char* prefix,
                       const char* extension, char* output, size_t capacity);
