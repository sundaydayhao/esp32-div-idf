#pragma once
#include "c_util.h"
#include "esp_err.h"
#include "text_format.h"
esp_err_t nv_read(const char* domain, const char* key, void* data, size_t size);
esp_err_t nv_write(const char* domain, const char* key, const void* data,
                   size_t size);
esp_err_t nv_erase(const char* domain, const char* key);
void nv_indexed_key(const char* prefix, unsigned index, char key[16]);
static inline text_t nv_key(const char* prefix, unsigned index) {
  text_t key = {0};
  nv_indexed_key(prefix, index, key.data);
  return key;
}
#define NV_GET(domain, key, value)                        \
  ({                                                      \
    __typeof__(value) nv_candidate = {0};                 \
    bool nv_ok = nv_read((domain), (key), &nv_candidate,  \
                         sizeof(nv_candidate)) == ESP_OK; \
    if (nv_ok) (value) = nv_candidate;                    \
    nv_ok;                                                \
  })
#define NV_PUT(domain, key, value)                                    \
  ({                                                                  \
    __auto_type nv_value = (value);                                   \
    nv_write((domain), (key), &nv_value, sizeof(nv_value)) == ESP_OK; \
  })
static inline int nv_count(const char* domain) {
  int count = 0;
  NV_GET(domain, "count", count);
  return count;
}
