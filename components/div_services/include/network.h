#pragma once
#include "c_util.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "text_format.h"
esp_err_t network_init(void);
esp_err_t network_mode(wifi_mode_t mode);
esp_err_t network_connect(const char* ssid, const char* password);
void network_disconnect(void);
bool network_connected(void);
esp_err_t network_ap_start(const char* ssid, const char* password,
                           uint8_t channel);
void network_stop(void);
esp_netif_t* network_station(void);
esp_netif_t* network_ap(void);
uint32_t network_ipv4(bool ap);
void network_ip_text(uint32_t address, char output[16]);
esp_err_t network_scan_start(bool passive);
int network_scan_results(wifi_ap_record_t* records, size_t capacity);
bool network_sync_clock(uint32_t timeout_ms);
enum { NETWORK_SCAN_RUNNING = -1, NETWORK_SCAN_FAILED = -2 };
bool network_set_mode(wifi_mode_t mode);
wifi_mode_t network_get_mode(void);
void network_disconnect_options(bool stop_radio, bool clear_credentials);
bool network_start_ap(const char* ssid, const char* password, int channel);
void network_stop_ap(bool stop_radio);
void network_connect_config(const char* ssid, const char* password,
                            uint8_t channel, const uint8_t* bssid);
void network_reconnect(void);
uint32_t network_netmask(void);
uint32_t network_gateway(void);
text_t network_address_text(uint32_t address);
text_t network_mac_address(void);
int network_ap_client_count(void);
int network_scan(bool asynchronous, bool hidden, bool passive,
                 uint32_t dwell_ms);
int network_scan_result_count(void);
void network_clear_scan(void);
text_t network_ssid(int index);
const uint8_t* network_bssid(int index);
text_t network_bssid_text(int index);
int network_rssi(int index);
int network_channel(int index);
wifi_auth_mode_t network_auth_mode(int index);
