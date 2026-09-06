#include <stdatomic.h>

#include "ble_service.h"
#include "freertos/FreeRTOS.h"
#include "host/ble_hs.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"

static _Atomic bool enabled;
static _Atomic uint16_t connection = BLE_HS_CONN_HANDLE_NONE;
static uint16_t input_handle;
static _Atomic(ble_hid_connection_cb_t) connection_callback;
static portMUX_TYPE report_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t report[8], leds, protocol = 1;

static void notify_connection(bool connected) {
  ble_hid_connection_cb_t callback = atomic_load(&connection_callback);
  if (callback) callback(connected);
}
static const uint8_t report_map[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19,
    0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08,
    0x81, 0x02, 0x75, 0x08, 0x95, 0x01, 0x81, 0x01, 0x75, 0x08, 0x95,
    0x06, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
    0x81, 0x00, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x95, 0x05, 0x75,
    0x01, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0xc0};
enum {
  INFO,
  MAP,
  INPUT,
  OUTPUT,
  CONTROL,
  PROTOCOL,
  INPUT_REF,
  OUTPUT_REF,
  BATTERY,
  MANUFACTURER,
  PNP
};
static int access_value(uint16_t conn, uint16_t attr,
                        struct ble_gatt_access_ctxt* ctx, void* arg) {
  (void)conn;
  (void)attr;
  static const uint8_t info[] = {0x11, 1, 0, 2}, input_ref[] = {1, 1},
                       output_ref[] = {1, 2};
  static const uint8_t battery = 100, pnp[] = {2, 2, 0xe5, 0x11, 0xa1, 0x10, 2};
  static const char manufacturer[] = "ESP32 Community";
  const void* value = NULL;
  uint8_t input[sizeof(report)];
  size_t length = 0;
  unsigned id = (uintptr_t)arg;
  if (ctx->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
    if (id != OUTPUT && id != PROTOCOL && id != CONTROL)
      return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    if (OS_MBUF_PKTLEN(ctx->om) != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    uint8_t v;
    if (os_mbuf_copydata(ctx->om, 0, 1, &v)) return BLE_ATT_ERR_UNLIKELY;
    if (id == OUTPUT) leds = v;
    if (id == PROTOCOL) protocol = v;
    return 0;
  }
  switch (id) {
    case INFO:
      value = info;
      length = sizeof(info);
      break;
    case MAP:
      value = report_map;
      length = sizeof(report_map);
      break;
    case INPUT:
      portENTER_CRITICAL(&report_lock);
      memcpy(input, report, sizeof(input));
      portEXIT_CRITICAL(&report_lock);
      value = input;
      length = sizeof(report);
      break;
    case OUTPUT:
      value = &leds;
      length = 1;
      break;
    case PROTOCOL:
      value = &protocol;
      length = 1;
      break;
    case INPUT_REF:
      value = input_ref;
      length = 2;
      break;
    case OUTPUT_REF:
      value = output_ref;
      length = 2;
      break;
    case BATTERY:
      value = &battery;
      length = 1;
      break;
    case MANUFACTURER:
      value = manufacturer;
      length = sizeof(manufacturer) - 1;
      break;
    case PNP:
      value = pnp;
      length = sizeof(pnp);
      break;
    default:
      return BLE_ATT_ERR_READ_NOT_PERMITTED;
  }
  return os_mbuf_append(ctx->om, value, length) ? BLE_ATT_ERR_INSUFFICIENT_RES
                                                : 0;
}
#define ARG(n) ((void*)(uintptr_t)(n))
#define CHR(n, f, a)              \
  {.uuid = BLE_UUID16_DECLARE(n), \
   .access_cb = access_value,     \
   .flags = (f),                  \
   .arg = ARG(a)}
static struct ble_gatt_dsc_def input_desc[] = {
    {.uuid = BLE_UUID16_DECLARE(0x2908),
     .att_flags = BLE_ATT_F_READ,
     .access_cb = access_value,
     .arg = ARG(INPUT_REF)},
    {0}};
static struct ble_gatt_dsc_def output_desc[] = {
    {.uuid = BLE_UUID16_DECLARE(0x2908),
     .att_flags = BLE_ATT_F_READ,
     .access_cb = access_value,
     .arg = ARG(OUTPUT_REF)},
    {0}};
static const struct ble_gatt_chr_def hid_chars[] = {
    CHR(0x2a4a, BLE_GATT_CHR_F_READ, INFO),
    CHR(0x2a4b, BLE_GATT_CHR_F_READ, MAP),
    {.uuid = BLE_UUID16_DECLARE(0x2a4d),
     .access_cb = access_value,
     .flags =
         BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY,
     .arg = ARG(INPUT),
     .val_handle = &input_handle,
     .descriptors = input_desc},
    {.uuid = BLE_UUID16_DECLARE(0x2a4d),
     .access_cb = access_value,
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
              BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
     .arg = ARG(OUTPUT),
     .descriptors = output_desc},
    CHR(0x2a4c, BLE_GATT_CHR_F_WRITE_NO_RSP, CONTROL),
    CHR(0x2a4e, BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP, PROTOCOL),
    {0}};
static const struct ble_gatt_chr_def battery_chars[] = {
    CHR(0x2a19, BLE_GATT_CHR_F_READ, BATTERY), {0}};
static const struct ble_gatt_chr_def device_chars[] = {
    CHR(0x2a29, BLE_GATT_CHR_F_READ, MANUFACTURER),
    CHR(0x2a50, BLE_GATT_CHR_F_READ, PNP),
    {0}};
static const struct ble_gatt_svc_def services[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = BLE_UUID16_DECLARE(0x1812),
     .characteristics = hid_chars},
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = BLE_UUID16_DECLARE(0x180f),
     .characteristics = battery_chars},
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = BLE_UUID16_DECLARE(0x180a),
     .characteristics = device_chars},
    {0}};
int ble_hid_register(void) {
  int rc = ble_gatts_count_cfg(services);
  return rc ? rc : ble_gatts_add_svcs(services);
}
int ble_hid_gap_event(struct ble_gap_event* event) {
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (!event->connect.status) {
        if (!enabled) {
          ble_gap_terminate(event->connect.conn_handle,
                            BLE_ERR_REM_USER_CONN_TERM);
          break;
        }
        connection = event->connect.conn_handle;
        notify_connection(true);
        ble_gap_security_initiate(connection);
      } else if (enabled)
        ble_advertising_start(ble_service_advertiser());
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      connection = BLE_HS_CONN_HANDLE_NONE;
      notify_connection(false);
      if (enabled) ble_advertising_start(ble_service_advertiser());
      break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
      struct ble_gap_conn_desc desc;
      if (!ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc))
        ble_store_util_delete_peer(&desc.peer_id_addr);
      return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    default:
      break;
  }
  return 0;
}
void ble_hid_host_reset(void) {
  connection = BLE_HS_CONN_HANDLE_NONE;
  notify_connection(false);
}
void ble_hid_host_sync(void) {
  if (enabled) ble_advertising_start(ble_service_advertiser());
}
bool ble_hid_connected(void) { return connection != BLE_HS_CONN_HANDLE_NONE; }
bool ble_hid_start(const char* name, ble_hid_connection_cb_t callback) {
  if (!ble_service_ready()) return false;
  ble_scan_t* scan = ble_service_scan();
  ble_scan_stop(scan);
  ble_scan_set_callback(scan, NULL, NULL);
  ble_advertiser_t* adv = ble_service_advertiser();
  ble_advertising_stop(adv);
  connection_callback = callback;
  enabled = true;
  ble_svc_gap_device_name_set(name);
  ble_svc_gap_device_appearance_set(0x03c1);
  *adv = (ble_advertiser_t){.min_interval = 160,
                            .max_interval = 320,
                            .connectable_mode = BLE_GAP_CONN_MODE_UND};
  ble_adv_flags(&adv->data, 6);
  ble_adv_appearance(&adv->data, 0x03c1);
  ble_advertising_service_uuid(adv, "1812");
  ble_adv_name(&adv->response, name);
  if (callback) callback(ble_hid_connected());
  return ble_hid_connected() || ble_advertising_start(adv);
}
bool ble_hid_send(const void* bytes, size_t length) {
  uint16_t conn = connection;
  if (!bytes || length != 8 || conn == BLE_HS_CONN_HANDLE_NONE) return false;
  portENTER_CRITICAL(&report_lock);
  memcpy(report, bytes, sizeof(report));
  portEXIT_CRITICAL(&report_lock);
  struct os_mbuf* om = ble_hs_mbuf_from_flat(bytes, length);
  return om && ble_gatts_notify_custom(conn, input_handle, om) == 0;
}
void ble_hid_stop(void) {
  enabled = false;
  ble_advertising_stop(ble_service_advertiser());
  uint16_t conn = connection;
  if (conn != BLE_HS_CONN_HANDLE_NONE)
    ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
}
