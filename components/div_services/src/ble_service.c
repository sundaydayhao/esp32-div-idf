#include "ble_service.h"

#include <stdatomic.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

int ble_hid_register(void);
void ble_store_config_init(void);
int ble_hid_gap_event(struct ble_gap_event* event);
void ble_hid_host_reset(void);
void ble_hid_host_sync(void);
enum { HOST_READY = 1, SCAN_DONE = 2, SCAN_CAPACITY = 64 };
static ble_scan_t scanner = {.interval_ms = 100,
                             .window_ms = 50,
                             .max_results = 48,
                             .active = true,
                             .duplicate_filter = true};
static ble_advertiser_t advertiser = {
    .min_interval = 160,
    .max_interval = 320,
    .connectable_mode = BLE_GAP_CONN_MODE_NON};
static EventGroupHandle_t events;
static SemaphoreHandle_t scan_lock;
static ble_scan_callback_t scan_callback;
static void* scan_context;
static ble_device_t *scan_records, *snapshot;
static size_t scan_count;
static uint8_t own_address_type;
static bool initialized;
static _Atomic bool scanning;

const uint8_t* ble_device_field(const ble_device_t* d, uint8_t type,
                                size_t* length) {
  if (length) *length = 0;
  if (!d) return NULL;
  for (size_t i = 0; i < d->length;) {
    size_t n = d->data[i];
    if (!n || i + n >= d->length) break;
    if (d->data[i + 1] == type) {
      if (length) *length = n - 1;
      return d->data + i + 2;
    }
    i += n + 1;
  }
  return NULL;
}
bool ble_device_has_field(const ble_device_t* d, uint8_t type) {
  return ble_device_field(d, type, NULL) != NULL;
}
text_t ble_device_field_text(const ble_device_t* d, uint8_t type) {
  size_t n;
  const uint8_t* p = ble_device_field(d, type, &n);
  return text_bytes((const char*)p, n);
}
text_t ble_device_name(const ble_device_t* d) {
  return ble_device_field_text(d, ble_device_has_field(d, 9) ? 9 : 8);
}
text_t ble_device_address(const ble_device_t* d) {
  text_t out = {0};
  if (d)
    snprintf(out.data, sizeof(out.data), "%02x:%02x:%02x:%02x:%02x:%02x",
             d->address[5], d->address[4], d->address[3], d->address[2],
             d->address[1], d->address[0]);
  return out;
}
text_t ble_device_service_uuid(const ble_device_t* d) {
  text_t out = {0};
  size_t n;
  const uint8_t* p;
  for (uint8_t type = 2; type <= 7; ++type) {
    p = ble_device_field(d, type, &n);
    if (!p) continue;
    if (type <= 3 && n >= 2)
      snprintf(out.data, sizeof(out.data), "%04x", p[0] | (p[1] << 8));
    else if (type <= 5 && n >= 4)
      snprintf(out.data, sizeof(out.data), "%08lx",
               (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
                   ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24));
    else if (n >= 16) {
      ble_uuid128_t uuid = {.u.type = BLE_UUID_TYPE_128};
      memcpy(uuid.value, p, 16);
      ble_uuid_to_str(&uuid.u, out.data);
    }
    return out;
  }
  return out;
}
int ble_device_tx_power(const ble_device_t* d) {
  size_t n;
  const uint8_t* p = ble_device_field(d, 10, &n);
  return p && n ? (int8_t)*p : 0;
}

static int gap_event(struct ble_gap_event* event, void* arg) {
  (void)arg;
  if (event->type == BLE_GAP_EVENT_DISC) {
    if (!scanning) return 0;
    ble_device_t record = {.rssi = event->disc.rssi,
                           .address_type = event->disc.addr.type};
    memcpy(record.address, event->disc.addr.val, 6);
    record.length = DIV_MIN(event->disc.length_data, sizeof(record.data));
    memcpy(record.data, event->disc.data, record.length);
    xSemaphoreTake(scan_lock, portMAX_DELAY);
    size_t index = 0;
    for (; index < scan_count; ++index)
      if (scan_records[index].address_type == record.address_type &&
          !memcmp(scan_records[index].address, record.address, 6))
        break;
    if (scan_records && index < scanner.max_results && index < SCAN_CAPACITY) {
      if (index < scan_count &&
          event->disc.event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP) {
        size_t old = DIV_MIN(scan_records[index].length,
                             sizeof(record.data) - record.length);
        memmove(record.data + old, record.data, record.length);
        memcpy(record.data, scan_records[index].data, old);
        record.length += old;
      }
      scan_records[index] = record;
      if (index == scan_count) ++scan_count;
    }
    if (scan_callback) scan_callback(&record, scan_context);
    xSemaphoreGive(scan_lock);
    return 0;
  }
  if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
    scanning = false;
    xEventGroupSetBits(events, SCAN_DONE);
    return 0;
  }
  return ble_hid_gap_event(event);
}
static void host_sync(void) {
  if (!ble_hs_util_ensure_addr(0) &&
      !ble_hs_id_infer_auto(0, &own_address_type)) {
    xEventGroupSetBits(events, HOST_READY);
    ble_hid_host_sync();
  }
}
static void host_reset(int reason) {
  ESP_LOGW("ble", "Host reset: %d", reason);
  xEventGroupClearBits(events, HOST_READY);
  ble_hid_host_reset();
  scanning = false;
  xEventGroupSetBits(events, SCAN_DONE);
}
static void host_task(void* arg) {
  (void)arg;
  nimble_port_run();
  nimble_port_freertos_deinit();
}
esp_err_t ble_service_init(const char* name) {
  if (initialized) return ble_service_ready() ? ESP_OK : ESP_ERR_INVALID_STATE;
  if (!events) events = xEventGroupCreate();
  if (!scan_lock) scan_lock = xSemaphoreCreateMutex();
  if (!events || !scan_lock) return ESP_ERR_NO_MEM;
  esp_err_t err = nimble_port_init();
  if (err != ESP_OK) return err;
  ble_hs_cfg.sync_cb = host_sync;
  ble_hs_cfg.reset_cb = host_reset;
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
  ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_sc = 1;
  ble_hs_cfg.sm_our_key_dist =
      BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist =
      BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_svc_gap_init();
  ble_svc_gatt_init();
  ble_svc_gap_device_name_set(name ? name : "ESP32-DIV");
  if (ble_hid_register()) {
    nimble_port_deinit();
    return ESP_FAIL;
  }
  ble_store_config_init();
  initialized = true;
  nimble_port_freertos_init(host_task);
  return xEventGroupWaitBits(events, HOST_READY, pdFALSE, pdTRUE,
                             pdMS_TO_TICKS(5000)) &
                 HOST_READY
             ? ESP_OK
             : ESP_ERR_TIMEOUT;
}
bool ble_service_ready(void) {
  return events && (xEventGroupGetBits(events) & HOST_READY);
}
ble_scan_t* ble_service_scan(void) {
  return ble_service_ready() ? &scanner : NULL;
}
ble_advertiser_t* ble_service_advertiser(void) {
  return ble_service_ready() ? &advertiser : NULL;
}
void ble_scan_set_callback(ble_scan_t* s, ble_scan_callback_t cb,
                           void* context) {
  if (!s || !scan_lock) return;
  xSemaphoreTake(scan_lock, portMAX_DELAY);
  scan_callback = cb;
  scan_context = context;
  xSemaphoreGive(scan_lock);
}
void ble_scan_stop(ble_scan_t* s) {
  if (s && scanning) {
    ble_gap_disc_cancel();
    scanning = false;
    xEventGroupSetBits(events, SCAN_DONE);
  }
}
bool ble_scan_is_active(const ble_scan_t* s) { return s && scanning; }
void ble_scan_clear(ble_scan_t* s) {
  if (!s) return;
  xSemaphoreTake(scan_lock, portMAX_DELAY);
  scan_count = 0;
  xSemaphoreGive(scan_lock);
}
bool ble_scan_start(ble_scan_t* s, uint32_t duration, bool keep) {
  if (!s || !ble_service_ready() || scanning) return false;
  if (s->max_results && !scan_records) {
    scan_records = calloc(SCAN_CAPACITY, sizeof(*scan_records));
    if (!scan_records) return false;
  }
  if (!keep) ble_scan_clear(s);
  struct ble_gap_disc_params p = {
      .passive = !s->active,
      .filter_duplicates = s->duplicate_filter,
      .itvl = (uint16_t)DIV_MAX(4, s->interval_ms * 8 / 5),
      .window =
          (uint16_t)DIV_MAX(4, DIV_MIN(s->window_ms, s->interval_ms) * 8 / 5)};
  xEventGroupClearBits(events, SCAN_DONE);
  scanning = true;
  int rc = ble_gap_disc(own_address_type, duration ? duration : BLE_HS_FOREVER,
                        &p, gap_event, NULL);
  if (rc) {
    scanning = false;
    xEventGroupSetBits(events, SCAN_DONE);
  }
  return rc == 0;
}
ble_results_t ble_scan_results(ble_scan_t* s, uint32_t duration, bool keep) {
  if (!s) return (ble_results_t){0};
  if (duration) {
    if (!ble_scan_start(s, duration, keep)) return (ble_results_t){0};
    xEventGroupWaitBits(events, SCAN_DONE, pdFALSE, pdTRUE,
                        pdMS_TO_TICKS(duration + 2000));
    if (scanning) ble_scan_stop(s);
  }
  if (!snapshot) snapshot = calloc(SCAN_CAPACITY, sizeof(*snapshot));
  if (!snapshot) return (ble_results_t){0};
  xSemaphoreTake(scan_lock, portMAX_DELAY);
  size_t count = scan_count;
  if (count) memcpy(snapshot, scan_records, count * sizeof(*snapshot));
  xSemaphoreGive(scan_lock);
  return (ble_results_t){snapshot, count};
}
const ble_device_t* ble_results_device(const ble_results_t* r, size_t i) {
  return r && i < r->count ? r->devices + i : NULL;
}
bool ble_adv_append(ble_adv_data_t* d, const uint8_t* p, size_t n) {
  if (!d || !p || n > sizeof(d->data) - d->length) return false;
  memcpy(d->data + d->length, p, n);
  d->length += n;
  return true;
}
bool ble_adv_field(ble_adv_data_t* d, uint8_t type, const void* p, size_t n) {
  if (!d || n > 29 || n + 2 > sizeof(d->data) - d->length) return false;
  uint8_t header[] = {(uint8_t)(n + 1), type};
  return ble_adv_append(d, header, 2) && ble_adv_append(d, p, n);
}
bool ble_adv_flags(ble_adv_data_t* d, uint8_t flags) {
  return ble_adv_field(d, 1, &flags, 1);
}
bool ble_adv_name(ble_adv_data_t* d, const char* name) {
  return name && ble_adv_field(d, 9, name, strlen(name));
}
bool ble_adv_appearance(ble_adv_data_t* d, uint16_t appearance) {
  uint8_t b[] = {(uint8_t)appearance, (uint8_t)(appearance >> 8)};
  return ble_adv_field(d, 0x19, b, 2);
}
void ble_advertising_stop(ble_advertiser_t* a) {
  if (a && ble_service_ready()) ble_gap_adv_stop();
}
bool ble_advertising_start(ble_advertiser_t* a) {
  if (!a || !ble_service_ready()) return false;
  struct ble_gap_adv_params p = {.conn_mode = a->connectable_mode,
                                 .disc_mode = BLE_GAP_DISC_MODE_GEN,
                                 .itvl_min = a->min_interval,
                                 .itvl_max = a->max_interval};
  int rc = ble_gap_adv_set_data(a->data.data, a->data.length);
  if (!rc) rc = ble_gap_adv_rsp_set_data(a->response.data, a->response.length);
  if (!rc)
    rc = ble_gap_adv_start(own_address_type, NULL, BLE_HS_FOREVER, &p,
                           gap_event, NULL);
  return rc == 0;
}
bool ble_advertising_service_uuid(ble_advertiser_t* a, const char* text) {
  if (!a || !text) return false;
  ble_uuid_any_t uuid;
  if (ble_uuid_from_str(&uuid, text)) return false;
  uint8_t b[16];
  size_t len = uuid.u.type / 8;
  if (len == 2) {
    b[0] = uuid.u16.value;
    b[1] = uuid.u16.value >> 8;
  } else if (len == 4) {
    for (int i = 0; i < 4; i++) b[i] = uuid.u32.value >> (i * 8);
  } else if (len == 16)
    memcpy(b, uuid.u128.value, 16);
  else
    return false;
  return ble_adv_field(&a->data, len == 2 ? 3 : len == 4 ? 5 : 7, b, len);
}
void ble_advertising_preferred(ble_advertiser_t* a, uint16_t minimum,
                               uint16_t maximum) {
  if (!a) return;
  uint8_t p[] = {(uint8_t)minimum, (uint8_t)(minimum >> 8), (uint8_t)maximum,
                 (uint8_t)(maximum >> 8)};
  ble_adv_field(&a->data, 0x12, p, 4);
}
