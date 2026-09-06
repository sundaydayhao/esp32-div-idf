#pragma once
#include "esp_bt.h"
#include "esp_err.h"
#include "host/ble_gap.h"
#include "text_format.h"
/* Scan records own their bytes. Callbacks run on the NimBLE host task. */
typedef struct {
  uint8_t address[6], address_type;
  int8_t rssi;
  uint8_t length, data[62];
} ble_device_t;
typedef void (*ble_scan_callback_t)(const ble_device_t*, void*);
typedef struct {
  uint16_t interval_ms, window_ms, max_results;
  bool active, duplicate_filter;
} ble_scan_t;
typedef struct {
  const ble_device_t* devices;
  size_t count;
} ble_results_t;
typedef struct {
  uint8_t data[31], length;
} ble_adv_data_t;
typedef struct {
  ble_adv_data_t data, response;
  uint16_t min_interval, max_interval;
  uint8_t connectable_mode;
} ble_advertiser_t;
esp_err_t ble_service_init(const char* name);
bool ble_service_ready(void);
ble_scan_t* ble_service_scan(void);
ble_advertiser_t* ble_service_advertiser(void);
bool ble_scan_start(ble_scan_t*, uint32_t duration_ms, bool keep_results);
void ble_scan_stop(ble_scan_t*);
bool ble_scan_is_active(const ble_scan_t*);
void ble_scan_clear(ble_scan_t*);
void ble_scan_set_callback(ble_scan_t*, ble_scan_callback_t, void* context);
ble_results_t ble_scan_results(ble_scan_t*, uint32_t duration_ms,
                               bool keep_results);
const ble_device_t* ble_results_device(const ble_results_t*, size_t index);
const uint8_t* ble_device_field(const ble_device_t*, uint8_t type,
                                size_t* length);
text_t ble_device_address(const ble_device_t*);
text_t ble_device_name(const ble_device_t*);
text_t ble_device_service_uuid(const ble_device_t*);
text_t ble_device_field_text(const ble_device_t*, uint8_t type);
bool ble_device_has_field(const ble_device_t*, uint8_t type);
int ble_device_tx_power(const ble_device_t*);
bool ble_adv_append(ble_adv_data_t*, const uint8_t*, size_t);
bool ble_adv_field(ble_adv_data_t*, uint8_t type, const void*, size_t);
bool ble_adv_flags(ble_adv_data_t*, uint8_t flags);
bool ble_adv_name(ble_adv_data_t*, const char* name);
bool ble_adv_appearance(ble_adv_data_t*, uint16_t appearance);
bool ble_advertising_start(ble_advertiser_t*);
void ble_advertising_stop(ble_advertiser_t*);
bool ble_advertising_service_uuid(ble_advertiser_t*, const char* uuid);
void ble_advertising_preferred(ble_advertiser_t*, uint16_t minimum,
                               uint16_t maximum);
typedef void (*ble_hid_connection_cb_t)(bool connected);
bool ble_hid_start(const char* name, ble_hid_connection_cb_t);
bool ble_hid_connected(void);
bool ble_hid_send(const void* report, size_t length);
void ble_hid_stop(void);
