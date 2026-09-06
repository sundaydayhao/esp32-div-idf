#include "storage.h"

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include "board_pins.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "spi_bus.h"
static sdmmc_card_t* card;
bool storage_mounted(void) { return card != NULL; }
void storage_unmount(void) {
  if (card && esp_vfs_fat_sdcard_unmount("/sdcard", card) == ESP_OK)
    card = NULL;
}
bool storage_mount_card(int cs) {
  return cs == SD_CS && storage_mount() == ESP_OK;
}
bool storage_path(const char* path, char* output, size_t capacity) {
  if (!path || !output || capacity < 9) return false;
  while (*path == '/') ++path;
  const char* part = path;
  while (*part) {
    const char* end = strchr(part, '/');
    size_t n = end ? (size_t)(end - part) : strlen(part);
    if ((n == 1 && part[0] == '.') ||
        (n == 2 && part[0] == '.' && part[1] == '.') || memchr(part, '\\', n))
      return false;
    if (!end) break;
    part = end + 1;
  }
  int n = snprintf(output, capacity, "/sdcard/%s", path);
  return n >= 0 && (size_t)n < capacity;
}
esp_err_t storage_mount(void) {
  if (card) return ESP_OK;
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = DIV_SD_HOST;
  host.max_freq_khz = 4000;
  spi_bus_config_t bus = {.mosi_io_num = SD_MOSI,
                          .miso_io_num = SD_MISO,
                          .sclk_io_num = SD_SCLK,
                          .quadwp_io_num = -1,
                          .quadhd_io_num = -1,
                          .max_transfer_sz = 40960};
  esp_err_t err =
      div_spi_bus_init(DIV_SD_HOST, &bus, SPI_DMA_CH_AUTO, "sdcard");
  if (err != ESP_OK) return err;
  sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot.host_id = DIV_SD_HOST;
  slot.gpio_cs = SD_CS;
  esp_vfs_fat_sdmmc_mount_config_t cfg = {.format_if_mount_failed = false,
                                          .max_files = 8,
                                          .allocation_unit_size = 16384};
  return esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &cfg, &card);
}
static bool make_parents(char* path, bool include_last) {
  for (char* p = path + 8; *p; ++p)
    if (*p == '/') {
      *p = 0;
      if (mkdir(path, 0775) != 0 && errno != EEXIST) {
        *p = '/';
        return false;
      }
      *p = '/';
    }
  return !include_last || mkdir(path, 0775) == 0 || errno == EEXIST;
}
FILE* storage_open(const char* path, const char* mode) {
  char full[STORAGE_PATH_MAX];
  if (!card || !mode || !storage_path(path, full, sizeof(full))) return NULL;
  if ((strchr(mode, 'w') || strchr(mode, 'a')) && !make_parents(full, false))
    return NULL;
  return fopen(full, mode);
}
bool storage_exists(const char* path) {
  char full[STORAGE_PATH_MAX];
  struct stat st;
  return card && storage_path(path, full, sizeof(full)) && stat(full, &st) == 0;
}
bool storage_mkdir(const char* path) {
  char full[STORAGE_PATH_MAX];
  return card && storage_path(path, full, sizeof(full)) &&
         make_parents(full, true);
}
bool storage_remove(const char* path) {
  char full[STORAGE_PATH_MAX];
  struct stat st;
  if (!card || !storage_path(path, full, sizeof(full)) || stat(full, &st))
    return false;
  return (S_ISDIR(st.st_mode) ? rmdir(full) : unlink(full)) == 0;
}
static int entry_order(const void* a, const void* b) {
  const storage_entry_t *x = a, *y = b;
  if (x->directory != y->directory) return x->directory ? -1 : 1;
  return strcasecmp(x->name, y->name);
}
size_t storage_list(const char* path, storage_entry_t* entries,
                    size_t capacity) {
  char full[STORAGE_PATH_MAX];
  if (!card || !entries || !storage_path(path, full, sizeof(full))) return 0;
  DIR* dir = opendir(full);
  if (!dir) return 0;
  size_t used = 0;
  struct dirent* entry;
  while (used < capacity && (entry = readdir(dir))) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    char child[STORAGE_PATH_MAX];
    int n = snprintf(child, sizeof(child), "%s/%s", full, entry->d_name);
    if (n < 0 || (size_t)n >= sizeof(child)) continue;
    struct stat st;
    if (stat(child, &st)) continue;
    storage_entry_t* out = &entries[used++];
    div_copy_string(out->name, sizeof(out->name), entry->d_name);
    out->directory = S_ISDIR(st.st_mode);
    out->size = st.st_size;
  }
  closedir(dir);
  qsort(entries, used, sizeof(*entries), entry_order);
  return used;
}
bool storage_next_path(const char* directory, const char* prefix,
                       const char* ext, char* output, size_t capacity) {
  if (!storage_mkdir(directory)) return false;
  for (unsigned i = 0; i < 10000; ++i) {
    int n =
        snprintf(output, capacity, "%s/%s_%04u.%s", directory, prefix, i, ext);
    if (n < 0 || (size_t)n >= capacity) return false;
    if (!storage_exists(output)) return true;
  }
  return false;
}
