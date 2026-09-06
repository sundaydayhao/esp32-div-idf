#include "network.h"

#include <stdatomic.h>

#include "esp_event.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static esp_netif_t *station, *access_point;
static bool initialized, scan_pending;
static atomic_bool connected, scan_done;
static SemaphoreHandle_t mutex;
static wifi_ap_record_t* cached_records;
static int cached_count;
static bool cache_ready;
static void event(void* arg, esp_event_base_t base, int32_t id, void* data) {
  (void)arg;
  (void)data;
  if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE)
    atomic_store(&scan_done, true);
  if (base == WIFI_EVENT &&
      (id == WIFI_EVENT_STA_DISCONNECTED || id == WIFI_EVENT_STA_STOP))
    atomic_store(&connected, false);
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    atomic_store(&connected, true);
}
esp_err_t network_init(void) {
  if (initialized) return ESP_OK;
  if (!mutex) mutex = xSemaphoreCreateRecursiveMutex();
  if (!mutex) return ESP_ERR_NO_MEM;
  xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
  if (initialized) {
    xSemaphoreGiveRecursive(mutex);
    return ESP_OK;
  }
  if (!station) station = esp_netif_create_default_wifi_sta();
  if (!access_point) access_point = esp_netif_create_default_wifi_ap();
  if (!station || !access_point) {
    xSemaphoreGiveRecursive(mutex);
    return ESP_ERR_NO_MEM;
  }
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_err_t err = esp_wifi_init(&cfg);
  if (err == ESP_OK)
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event, NULL);
  if (err == ESP_OK)
    err =
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event, NULL);
  if (err == ESP_OK) {
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    initialized = true;
  }
  xSemaphoreGiveRecursive(mutex);
  return err;
}
esp_err_t network_mode(wifi_mode_t mode) {
  esp_err_t err = network_init();
  if (err != ESP_OK) return err;
  xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
  err = esp_wifi_set_mode(mode);
  if (err == ESP_OK) err = esp_wifi_start();
  xSemaphoreGiveRecursive(mutex);
  return err;
}
esp_err_t network_connect(const char* ssid, const char* password) {
  if (!ssid || strlen(ssid) > 32 || (password && strlen(password) > 63))
    return ESP_ERR_INVALID_ARG;
  esp_err_t err = network_mode(WIFI_MODE_STA);
  if (err != ESP_OK) return err;
  wifi_config_t cfg = {0};
  memcpy(cfg.sta.ssid, ssid, strlen(ssid));
  if (password) memcpy(cfg.sta.password, password, strlen(password));
  esp_wifi_disconnect();
  atomic_store(&connected, false);
  err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
  return err == ESP_OK ? esp_wifi_connect() : err;
}
void network_disconnect(void) {
  if (initialized) esp_wifi_disconnect();
  atomic_store(&connected, false);
}
bool network_connected(void) { return atomic_load(&connected); }
esp_err_t network_ap_start(const char* ssid, const char* password,
                           uint8_t channel) {
  size_t len = ssid ? strlen(ssid) : 0, plen = password ? strlen(password) : 0;
  if (!len || len > 32 || plen > 63 || (plen && plen < 8) || channel < 1 ||
      channel > 14)
    return ESP_ERR_INVALID_ARG;
  esp_err_t err = network_mode(WIFI_MODE_AP);
  if (err != ESP_OK) return err;
  wifi_config_t cfg = {0};
  memcpy(cfg.ap.ssid, ssid, len);
  cfg.ap.ssid_len = len;
  if (plen) memcpy(cfg.ap.password, password, plen);
  cfg.ap.authmode = plen ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
  cfg.ap.channel = channel;
  cfg.ap.max_connection = 4;
  return esp_wifi_set_config(WIFI_IF_AP, &cfg);
}
void network_stop(void) {
  if (!initialized) return;
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(NULL);
  esp_wifi_scan_stop();
  esp_wifi_clear_ap_list();
  scan_pending = false;
  atomic_store(&scan_done, false);
  network_disconnect();
  esp_wifi_stop();
}
esp_netif_t* network_station(void) { return station; }
esp_netif_t* network_ap(void) { return access_point; }
uint32_t network_ipv4(bool ap) {
  esp_netif_ip_info_t info = {0};
  esp_netif_t* netif = ap ? access_point : station;
  if (netif) esp_netif_get_ip_info(netif, &info);
  return info.ip.addr;
}
void network_ip_text(uint32_t address, char out[16]) {
  esp_ip4_addr_t ip = {.addr = address};
  esp_ip4addr_ntoa(&ip, out, 16);
}
esp_err_t network_scan_start(bool passive) {
  esp_err_t err = network_mode(WIFI_MODE_STA);
  if (err != ESP_OK) return err;
  esp_wifi_scan_stop();
  atomic_store(&scan_done, false);
  scan_pending = false;
  wifi_scan_config_t cfg = {
      .show_hidden = true,
      .scan_type = passive ? WIFI_SCAN_TYPE_PASSIVE : WIFI_SCAN_TYPE_ACTIVE};
  cfg.scan_time.active.min = 40;
  cfg.scan_time.active.max = 120;
  cfg.scan_time.passive = 200;
  err = esp_wifi_scan_start(&cfg, false);
  scan_pending = err == ESP_OK;
  return err;
}
int network_scan_results(wifi_ap_record_t* records, size_t capacity) {
  if (!scan_pending) return -2;
  if (!atomic_load(&scan_done)) return -1;
  scan_pending = false;
  uint16_t count = 0;
  esp_wifi_scan_get_ap_num(&count);
  count = DIV_MIN(count, capacity);
  if (count && records &&
      esp_wifi_scan_get_ap_records(&count, records) != ESP_OK)
    count = 0;
  esp_wifi_clear_ap_list();
  return count;
}
bool network_sync_clock(uint32_t timeout_ms) {
  static bool started = false;
  if (!network_connected()) return false;
  if (!started) {
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if (esp_netif_sntp_init(&cfg) != ESP_OK) return false;
    started = true;
  }
  return esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms)) == ESP_OK;
}
bool network_set_mode(wifi_mode_t mode) { return network_mode(mode) == ESP_OK; }
wifi_mode_t network_get_mode(void) {
  wifi_mode_t mode = WIFI_MODE_NULL;
  esp_wifi_get_mode(&mode);
  return mode;
}
void network_disconnect_options(bool stop_radio, bool clear_credentials) {
  if (!initialized) return;
  xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
  network_disconnect();
  if (clear_credentials) {
    wifi_config_t cfg = {0};
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
  }
  if (stop_radio) esp_wifi_stop();
  xSemaphoreGiveRecursive(mutex);
}
bool network_start_ap(const char* ssid, const char* password, int channel) {
  if (network_init() != ESP_OK) return false;
  xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
  wifi_mode_t old = network_get_mode();
  esp_err_t err = network_ap_start(ssid, password, channel);
  if (err == ESP_OK && old == WIFI_MODE_APSTA) err = esp_wifi_set_mode(old);
  xSemaphoreGiveRecursive(mutex);
  return err == ESP_OK;
}
void network_stop_ap(bool stop_radio) {
  if (!initialized) return;
  xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
  if (stop_radio)
    esp_wifi_stop();
  else
    esp_wifi_set_mode(network_get_mode() == WIFI_MODE_APSTA ? WIFI_MODE_STA
                                                            : WIFI_MODE_NULL);
  xSemaphoreGiveRecursive(mutex);
}
void network_connect_config(const char* ssid, const char* password,
                            uint8_t channel, const uint8_t* bssid) {
  if (!ssid || strlen(ssid) > 32 || (password && strlen(password) > 63) ||
      network_init() != ESP_OK)
    return;
  xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
  if (network_set_mode(WIFI_MODE_STA)) {
    wifi_config_t cfg = {0};
    memcpy(cfg.sta.ssid, ssid, strlen(ssid));
    if (password) memcpy(cfg.sta.password, password, strlen(password));
    cfg.sta.channel = channel;
    if (bssid) {
      memcpy(cfg.sta.bssid, bssid, 6);
      cfg.sta.bssid_set = true;
    }
    atomic_store(&connected, false);
    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) == ESP_OK) esp_wifi_connect();
  }
  xSemaphoreGiveRecursive(mutex);
}
void network_reconnect(void) {
  if (network_set_mode(WIFI_MODE_STA)) {
    atomic_store(&connected, false);
    esp_wifi_connect();
  }
}
uint32_t network_netmask(void) {
  esp_netif_ip_info_t info = {0};
  if (station) esp_netif_get_ip_info(station, &info);
  return info.netmask.addr;
}
uint32_t network_gateway(void) {
  esp_netif_ip_info_t info = {0};
  if (station) esp_netif_get_ip_info(station, &info);
  return info.gw.addr;
}
text_t network_address_text(uint32_t address) {
  text_t text = {0};
  network_ip_text(address, text.data);
  return text;
}
text_t network_mac_address(void) {
  uint8_t mac[6] = {0};
  esp_wifi_get_mac(WIFI_IF_STA, mac);
  text_t text;
  snprintf(text.data, sizeof(text.data), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return text;
}
int network_ap_client_count(void) {
  wifi_sta_list_t list = {0};
  return esp_wifi_ap_get_sta_list(&list) == ESP_OK ? list.num : 0;
}
static void cache_scan_results(void) {
  uint16_t n = 0;
  esp_wifi_scan_get_ap_num(&n);
  n = DIV_MIN(n, 64);
  if (!cached_records && n)
    cached_records = calloc(64, sizeof(*cached_records));
  if (n && cached_records &&
      esp_wifi_scan_get_ap_records(&n, cached_records) == ESP_OK)
    cached_count = n;
  else
    cached_count = 0;
  esp_wifi_clear_ap_list();
  cache_ready = true;
  scan_pending = false;
}
int network_scan(bool asynchronous, bool hidden, bool passive,
                 uint32_t dwell_ms) {
  if (!network_set_mode(WIFI_MODE_STA)) return NETWORK_SCAN_FAILED;
  xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
  wifi_scan_config_t cfg = {
      .show_hidden = hidden,
      .scan_type = passive ? WIFI_SCAN_TYPE_PASSIVE : WIFI_SCAN_TYPE_ACTIVE};
  cfg.scan_time.active.min = 40;
  cfg.scan_time.active.max = DIV_CLAMP(dwell_ms, 40, 1500);
  cfg.scan_time.passive = DIV_CLAMP(dwell_ms, 40, 1500);
  atomic_store(&scan_done, false);
  cache_ready = false;
  cached_count = 0;
  esp_err_t err = esp_wifi_scan_start(&cfg, !asynchronous);
  scan_pending = err == ESP_OK;
  if (err == ESP_OK && !asynchronous) cache_scan_results();
  int result = err != ESP_OK  ? NETWORK_SCAN_FAILED
               : asynchronous ? NETWORK_SCAN_RUNNING
                              : cached_count;
  xSemaphoreGiveRecursive(mutex);
  return result;
}
int network_scan_result_count(void) {
  if (!initialized) return NETWORK_SCAN_FAILED;
  xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
  if (!cache_ready && scan_pending && atomic_load(&scan_done))
    cache_scan_results();
  int result = cache_ready    ? cached_count
               : scan_pending ? NETWORK_SCAN_RUNNING
                              : NETWORK_SCAN_FAILED;
  xSemaphoreGiveRecursive(mutex);
  return result;
}
void network_clear_scan(void) {
  if (!initialized) return;
  xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
  free(cached_records);
  cached_records = NULL;
  cached_count = 0;
  cache_ready = false;
  scan_pending = false;
  atomic_store(&scan_done, false);
  esp_wifi_clear_ap_list();
  xSemaphoreGiveRecursive(mutex);
}
static wifi_ap_record_t cached_record(int index) {
  wifi_ap_record_t record = {0};
  if (!initialized) return record;
  xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
  if (index >= 0 && index < cached_count) record = cached_records[index];
  xSemaphoreGiveRecursive(mutex);
  return record;
}
text_t network_ssid(int index) {
  wifi_ap_record_t record = cached_record(index);
  return text_bytes((const char*)record.ssid,
                    strnlen((const char*)record.ssid, 32));
}
const uint8_t* network_bssid(int index) {
  static _Thread_local uint8_t mac[6];
  wifi_ap_record_t record = cached_record(index);
  memcpy(mac, record.bssid, 6);
  return mac;
}
text_t network_bssid_text(int index) {
  wifi_ap_record_t record = cached_record(index);
  text_t out;
  uint8_t* b = record.bssid;
  snprintf(out.data, sizeof(out.data), "%02X:%02X:%02X:%02X:%02X:%02X", b[0],
           b[1], b[2], b[3], b[4], b[5]);
  return out;
}
int network_rssi(int index) { return cached_record(index).rssi; }
int network_channel(int index) { return cached_record(index).primary; }
wifi_auth_mode_t network_auth_mode(int index) {
  return cached_record(index).authmode;
}
