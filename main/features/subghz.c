#include "button_input.h"
#include "c_vector.h"
#include "config.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_random.h"
#include "icon.h"
#include "keyboard_ui.h"
#include "math.h"
#include "shared.h"
#include "signal_dsp.h"
#include "touch_ui.h"
#include "ui_utils.h"

static inline void gpioOutputWrite(int pin, bool high) {
  if (pin < 0 || pin >= GPIO_NUM_MAX) {
    return;
  }
  gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)pin, high ? 1 : 0);
}

static const char* SUBGHZ_DIR = "/subghz";
static const char* SUBGHZ_EXPORT_PREFIX = "/subghz/profiles_";
static const char* SUBGHZ_CURRENT_PATH = "/subghz/profiles_current.bin";
enum { SUBGHZ_EXPORT_MAGIC = 0x315A4753 };

typedef struct __attribute__((packed)) SubGhzProfile {
  uint32_t frequency;
  uint32_t value;
  uint16_t bitLength;
  uint16_t protocol;
  char name[16];
} SubGhzProfile;
#define SubGhzProfile_INITIALIZER {0}
;

enum { MAX_NAME_LENGTH = 16 };
enum { PROFILE_SIZE = sizeof(SubGhzProfile) };

enum { MAX_PROFILES = 5 };

typedef struct __attribute__((packed)) SubGhzExportHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t count;
  uint16_t profileSize;
  uint16_t reserved;
} SubGhzExportHeader;
#define SubGhzExportHeader_INITIALIZER {0}
;

static bool subghz_sd_mounted = false;
static bool subghzMountSD(void) {
  if (subghz_sd_mounted) {
    if (storage_exists("/")) return true;
    subghz_sd_mounted = false;
  }

  subghz_sd_mounted = isSDCardAvailable();
  return subghz_sd_mounted;
}

static bool subghzEnsureDir(const char* dirPath) {
  if (!subghzMountSD()) return false;
  if (storage_exists(dirPath)) return true;
  if (storage_mkdir(dirPath)) return true;
  if (dirPath && dirPath[0] == '/') return storage_mkdir(dirPath + 1);
  return false;
}

static void clearProfilesInNvs(void) {
  uint16_t zero = 0;
  NV_PUT("subghz", "count", zero);
  SubGhzProfile empty = SubGhzProfile_INITIALIZER;
  for (uint16_t i = 0; i < MAX_PROFILES; i++) {
    NV_PUT("subghz", (nv_key("profile", i)).data, empty);
  }
}

static bool makeNextExportPath(text_t* outPath) {
  for (uint16_t i = 0; i < 10000; i++) {
    char buf[48];
    snprintf(buf, sizeof(buf), "%s%04u.bin", SUBGHZ_EXPORT_PREFIX, (unsigned)i);
    if (!storage_exists(buf)) {
      (*outPath) = TEXT(buf);
      return true;
    }
  }
  return false;
}

static bool findLatestExportPath(text_t* outPath) {
  for (int i = 9999; i >= 0; i--) {
    char buf[48];
    snprintf(buf, sizeof(buf), "%s%04u.bin", SUBGHZ_EXPORT_PREFIX, (unsigned)i);
    if (storage_exists(buf)) {
      (*outPath) = TEXT(buf);
      return true;
    }
  }
  return false;
}

static bool exportProfilesToSD(text_t* outPath, text_t* errOut) {
  if (!subghzEnsureDir(SUBGHZ_DIR)) {
    if (errOut) *errOut = TEXT("SD not mounted");
    return false;
  }

  uint16_t count = 0;
  NV_GET("subghz", "count", count);
  if (count > MAX_PROFILES) count = MAX_PROFILES;

  if (!makeNextExportPath(&((*outPath)))) {
    if (errOut) *errOut = TEXT("No free filename");
    return false;
  }

  storage_file_t f __attribute__((cleanup(storage_file_close))) =
      storage_file_open(((*outPath)).data, "wb");
  if (!storage_file_valid(&(f))) {
    if (errOut) *errOut = TEXT("Open failed");
    return false;
  }

  SubGhzExportHeader h = SubGhzExportHeader_INITIALIZER;
  h.magic = SUBGHZ_EXPORT_MAGIC;
  h.version = 1;
  h.count = count;
  h.profileSize = PROFILE_SIZE;
  h.reserved = 0;

  bool ok =
      (storage_file_write(&(f), (const uint8_t*)&h, sizeof(h)) == sizeof(h));
  for (uint16_t i = 0; ok && i < count; i++) {
    SubGhzProfile p = SubGhzProfile_INITIALIZER;
    __auto_type key = nv_key("profile", i);
    NV_GET("subghz", (key).data, p);
    ok = (storage_file_write(&(f), (const uint8_t*)&p, sizeof(p)) == sizeof(p));
  }
  storage_file_close(&(f));

  if (!ok && errOut) *errOut = TEXT("Write failed");
  return ok;
}

static bool syncCurrentProfilesToSD(text_t* errOut) {
  if (!subghzEnsureDir(SUBGHZ_DIR)) {
    if (errOut) *errOut = TEXT("SD not mounted");
    return false;
  }

  uint16_t count = 0;
  NV_GET("subghz", "count", count);
  if (count > MAX_PROFILES) count = MAX_PROFILES;

  if (storage_exists(SUBGHZ_CURRENT_PATH)) storage_remove(SUBGHZ_CURRENT_PATH);
  storage_file_t f __attribute__((cleanup(storage_file_close))) =
      storage_file_open(SUBGHZ_CURRENT_PATH, "wb");
  if (!storage_file_valid(&(f))) {
    if (errOut) *errOut = TEXT("Open failed");
    return false;
  }

  SubGhzExportHeader h = SubGhzExportHeader_INITIALIZER;
  h.magic = SUBGHZ_EXPORT_MAGIC;
  h.version = 1;
  h.count = count;
  h.profileSize = PROFILE_SIZE;
  h.reserved = 0;

  bool ok =
      (storage_file_write(&(f), (const uint8_t*)&h, sizeof(h)) == sizeof(h));
  for (uint16_t i = 0; ok && i < count; i++) {
    SubGhzProfile p = SubGhzProfile_INITIALIZER;
    __auto_type key = nv_key("profile", i);
    NV_GET("subghz", (key).data, p);
    ok = (storage_file_write(&(f), (const uint8_t*)&p, sizeof(p)) == sizeof(p));
  }
  storage_file_close(&(f));
  if (!ok && errOut) *errOut = TEXT("Write failed");
  return ok;
}

static bool importProfilesFromSD(text_t path, text_t* errOut) {
  if (!subghzMountSD()) {
    if (errOut) *errOut = TEXT("SD not mounted");
    return false;
  }
  if ((strlen((path).data) == 0) || !storage_exists((path).data)) {
    if (errOut) *errOut = TEXT("File not found");
    return false;
  }

  storage_file_t f __attribute__((cleanup(storage_file_close))) =
      storage_file_open((path).data, "rb");
  if (!storage_file_valid(&(f))) {
    if (errOut) *errOut = TEXT("Open failed");
    return false;
  }

  SubGhzExportHeader h = SubGhzExportHeader_INITIALIZER;
  if (storage_file_read(&(f), (uint8_t*)&h, sizeof(h)) != sizeof(h)) {
    storage_file_close(&(f));
    if (errOut) *errOut = TEXT("Bad header");
    return false;
  }
  if (h.magic != SUBGHZ_EXPORT_MAGIC || h.version != 1) {
    storage_file_close(&(f));
    if (errOut) *errOut = TEXT("Wrong file");
    return false;
  }
  if (h.profileSize != PROFILE_SIZE) {
    storage_file_close(&(f));
    if (errOut) *errOut = TEXT("Size mismatch");
    return false;
  }

  uint16_t count = h.count;
  if (count > MAX_PROFILES) count = MAX_PROFILES;

  clearProfilesInNvs();
  for (uint16_t i = 0; i < count; i++) {
    SubGhzProfile p = SubGhzProfile_INITIALIZER;
    if (storage_file_read(&(f), (uint8_t*)&p, sizeof(p)) != sizeof(p)) {
      storage_file_close(&(f));
      if (errOut) *errOut = TEXT("Read failed");
      return false;
    }
    p.name[MAX_NAME_LENGTH - 1] = '\0';
    NV_PUT("subghz", (nv_key("profile", i)).data, p);
  }
  NV_PUT("subghz", "count", count);
  storage_file_close(&(f));
  return true;
}

typedef struct SubGhzFileEntry {
  text_t path;
  uint16_t count;
  bool isCurrent;
} SubGhzFileEntry;
#define SubGhzFileEntry_INITIALIZER {.count = 0, .isCurrent = false}
;

static bool readExportHeader(storage_file_t* f, SubGhzExportHeader* out,
                             text_t* errOut) {
  if (storage_file_read(&((*f)), (uint8_t*)&(*out), sizeof((*out))) !=
      sizeof((*out))) {
    if (errOut) *errOut = TEXT("Bad header");
    return false;
  }
  if ((*out).magic != SUBGHZ_EXPORT_MAGIC || (*out).version != 1) {
    if (errOut) *errOut = TEXT("Wrong file");
    return false;
  }
  if ((*out).profileSize != PROFILE_SIZE) {
    if (errOut) *errOut = TEXT("Size mismatch");
    return false;
  }
  return true;
}

static bool readProfileAt(text_t path, uint16_t localIndex, SubGhzProfile* out,
                          text_t* errOut) {
  if (!subghzMountSD()) {
    if (errOut) *errOut = TEXT("SD not mounted");
    return false;
  }
  storage_file_t f __attribute__((cleanup(storage_file_close))) =
      storage_file_open((path).data, "rb");
  if (!storage_file_valid(&(f))) {
    if (errOut) *errOut = TEXT("Open failed");
    return false;
  }
  SubGhzExportHeader h = SubGhzExportHeader_INITIALIZER;
  if (!readExportHeader(&(f), &(h), errOut)) {
    storage_file_close(&(f));
    return false;
  }
  uint16_t count = h.count;
  if (count > MAX_PROFILES) count = MAX_PROFILES;
  if (localIndex >= count) {
    storage_file_close(&(f));
    if (errOut) *errOut = TEXT("Index OOR");
    return false;
  }
  uint32_t off = (uint32_t)sizeof(SubGhzExportHeader) +
                 (uint32_t)localIndex * (uint32_t)PROFILE_SIZE;
  if (!storage_file_seek(&(f), off)) {
    storage_file_close(&(f));
    if (errOut) *errOut = TEXT("Seek failed");
    return false;
  }
  if (storage_file_read(&(f), (uint8_t*)&(*out), sizeof((*out))) !=
      sizeof((*out))) {
    storage_file_close(&(f));
    if (errOut) *errOut = TEXT("Read failed");
    return false;
  }
  (*out).name[MAX_NAME_LENGTH - 1] = '\0';
  storage_file_close(&(f));
  return true;
}

static int subghz_file_order(const void* left, const void* right) {
  const SubGhzFileEntry *a = left, *b = right;
  if (a->isCurrent != b->isCurrent) return a->isCurrent ? -1 : 1;
  return strcmp(b->path.data, a->path.data);
}
static bool listAllProfileFiles(c_vector_t* out, text_t* errOut) {
  c_vector_free(&((*out)));
  if (!subghzMountSD()) {
    if (errOut) *errOut = TEXT("SD not mounted");
    return false;
  }
  if (!storage_exists(SUBGHZ_DIR)) {
    if (errOut) *errOut = TEXT("No /subghz");
    return false;
  }
  storage_file_t d __attribute__((cleanup(storage_file_close))) =
      storage_file_open(SUBGHZ_DIR, "rb");
  if (!storage_file_valid(&(d))) {
    if (errOut) *errOut = TEXT("Open dir failed");
    return false;
  }

  for (;;) {
    storage_file_t f __attribute__((cleanup(storage_file_close))) =
        storage_file_next(&(d));
    if (!storage_file_valid(&(f))) break;
    if (((f).directory != NULL)) {
      storage_file_close(&(f));
      continue;
    }
    text_t name = TEXT((f).name);

    text_t fullPath = text_join_cstr(
        (text_join_cstr((TEXT(SUBGHZ_DIR)).data, "/")).data, (name).data);

    bool isCurrent = ((strcmp((name).data, "profiles_current.bin") == 0));
    bool isArchive = text_prefix_cstr((name).data, "profiles_") &&
                     text_suffix_cstr((name).data, ".bin") && !isCurrent;
    if (!isCurrent && !isArchive) {
      storage_file_close(&(f));
      continue;
    }

    SubGhzExportHeader h = SubGhzExportHeader_INITIALIZER;
    text_t herr = {0};
    bool ok = readExportHeader(&(f), &(h), &herr);
    storage_file_close(&(f));
    if (!ok) continue;

    uint16_t cnt = h.count;
    if (cnt > MAX_PROFILES) cnt = MAX_PROFILES;
    if (cnt == 0) continue;

    SubGhzFileEntry e = SubGhzFileEntry_INITIALIZER;
    e.path = fullPath;
    e.count = cnt;
    e.isCurrent = isCurrent;
    C_VECTOR_PUSH_AS(&((*out)), SubGhzFileEntry, e);
  }
  storage_file_close(&(d));

  qsort(((SubGhzFileEntry*)((*out)).data),
        (((SubGhzFileEntry*)((*out)).data + ((*out)).size)) -
            (((SubGhzFileEntry*)((*out)).data)),
        sizeof(*(((SubGhzFileEntry*)((*out)).data))), subghz_file_order);
  return true;
}

static uint16_t totalProfilesInIndex(const c_vector_t* files) {
  uint32_t total = 0;
  {
    for (size_t c_range_index = 0; c_range_index < ((*files)).size;
         ++c_range_index) {
      __auto_type f = &(((SubGhzFileEntry*)((*files)).data)[c_range_index]);
      total += (*f).count;
    }
  }
  if (total > 65535) total = 65535;
  return (uint16_t)total;
}

static bool locateGlobalIndex(const c_vector_t* files, uint16_t globalIndex,
                              text_t* outPath, uint16_t* outLocalIdx) {
  uint32_t idx = globalIndex;
  {
    for (size_t c_range_index = 0; c_range_index < ((*files)).size;
         ++c_range_index) {
      __auto_type fe = &(((SubGhzFileEntry*)((*files)).data)[c_range_index]);
      {
        if (idx < (*fe).count) {
          (*outPath) = (*fe).path;
          (*outLocalIdx) = (uint16_t)idx;
          return true;
        }
        idx -= (*fe).count;
      }
    }
  }
  return false;
}

static bool deleteProfileFromFile(text_t path, uint16_t localIndex,
                                  text_t* errOut) {
  if (!subghzMountSD()) {
    if (errOut) *errOut = TEXT("SD not mounted");
    return false;
  }
  storage_file_t f __attribute__((cleanup(storage_file_close))) =
      storage_file_open((path).data, "rb");
  if (!storage_file_valid(&(f))) {
    if (errOut) *errOut = TEXT("Open failed");
    return false;
  }
  SubGhzExportHeader h = SubGhzExportHeader_INITIALIZER;
  if (!readExportHeader(&(f), &(h), errOut)) {
    storage_file_close(&(f));
    return false;
  }
  uint16_t count = h.count;
  if (count > MAX_PROFILES) count = MAX_PROFILES;
  if (localIndex >= count) {
    storage_file_close(&(f));
    if (errOut) *errOut = TEXT("Index OOR");
    return false;
  }

  SubGhzProfile buf[MAX_PROFILES] = SubGhzProfile_INITIALIZER;
  for (uint16_t i = 0; i < count; i++) {
    if (storage_file_read(&(f), (uint8_t*)&buf[i], sizeof(SubGhzProfile)) !=
        sizeof(SubGhzProfile)) {
      storage_file_close(&(f));
      if (errOut) *errOut = TEXT("Read failed");
      return false;
    }
    buf[i].name[MAX_NAME_LENGTH - 1] = '\0';
  }
  storage_file_close(&(f));

  for (uint16_t i = localIndex; i + 1 < count; i++) buf[i] = buf[i + 1];
  count--;

  if (storage_exists((path).data)) storage_remove((path).data);
  storage_file_t w __attribute__((cleanup(storage_file_close))) =
      storage_file_open((path).data, "wb");
  if (!storage_file_valid(&(w))) {
    if (errOut) *errOut = TEXT("Open write failed");
    return false;
  }

  SubGhzExportHeader nh = SubGhzExportHeader_INITIALIZER;
  nh.magic = SUBGHZ_EXPORT_MAGIC;
  nh.version = 1;
  nh.count = count;
  nh.profileSize = PROFILE_SIZE;
  nh.reserved = 0;
  bool ok =
      (storage_file_write(&(w), (const uint8_t*)&nh, sizeof(nh)) == sizeof(nh));
  for (uint16_t i = 0; ok && i < count; i++) {
    ok = (storage_file_write(&(w), (const uint8_t*)&buf[i],
                             sizeof(SubGhzProfile)) == sizeof(SubGhzProfile));
  }
  storage_file_close(&(w));
  if (!ok && errOut) *errOut = TEXT("Write failed");

  if (ok && text_suffix_cstr((path).data, "profiles_current.bin")) {
    importProfilesFromSD(path, NULL);
  }
  return ok;
}

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

static int subghzScreenH(void) { return display_height(&(tft)); }

static int subghzContentBottom(void) {
  return featureHasTouchNavBar() ? touchNavContentBottomY() : subghzScreenH();
}

static void subghzClearBody(uint16_t color) {
  if (featureHasTouchNavBar()) {
    featureClearContent(color);
  } else {
    display_fill_screen(&(tft), color);
  }
}

static const unsigned long kSubghzNavDebounceMs = 200;

static void subghzWaitNavRelease(int pin) {
  while (isTouchNavButtonPressed(pin)) {
    sys_sleep_ms(10);
  }
  sys_sleep_ms(kSubghzNavDebounceMs);
}

static void subghzRedrawNavChrome(void) {
  if (!featureHasTouchNavBar()) {
    return;
  }
  invalidateTouchButtonCue();
  redrawTouchButtonBar();
  maintainTouchNavBar();
}

static bool subghzWaitWithNav(uint32_t ms) {
  const uint32_t until = sys_monotonic_ms() + ms;
  while ((int32_t)(sys_monotonic_ms() - until) < 0) {
    if (feature_exit_requested || featureExitButtonPressed()) {
      return false;
    }
    if (featureHasTouchNavBar()) {
      maintainTouchNavBar();
    }
    sys_sleep_ms(50);
  }
  return true;
}

static void subghzSetReplayNavLabels(void) {
  setTouchNavLabels("Freq-", "Save", "Exit", "Send", "Freq+");
}

static void subghzSetJammerNavLabels(void) {
  setTouchNavLabels("Freq-", "Auto", "Exit", "Toggle", "Freq+");
}

static void subghzSetProfileNavLabels(void) {
  setTouchNavLabels("Delete", "Next", "Exit", "Prev", "TX");
}

void replayat_replayHandleNavButtons(void);

void subjammer_subjammerHandleNavButtons(void);

void SavedProfile_profileHandleNavButtons(void);

#define MAX_PROFILES 5

#define SCREEN_WIDTH 240
#define SCREENHEIGHT 320
#undef SCREEN_HEIGHT
#define SCREEN_HEIGHT 320

static bool replayat_uiDrawn = false;

void replayat_runUI(void);
void replayat_sendSignal(void);
void replayat_saveProfile(void);
void replayat_updateDisplay(void);

#define MAX_NAME_LENGTH 16

const char* replayat_randomNames[] = {
    "Signal", "Remote",  "KeyFob",  "GateOpener", "DoorLock",
    "RFTest", "Profile", "Control", "Switch",     "Beacon"};
enum { replayat_numRandomNames = 10 };

typedef struct __attribute__((packed)) replayat_Profile {
  uint32_t frequency;
  uint32_t value;
  uint16_t bitLength;
  uint16_t protocol;
  char name[MAX_NAME_LENGTH];
} replayat_Profile;
#define replayat_Profile_INITIALIZER {0}
;

#define PROFILE_SIZE sizeof(Profile)

uint16_t replayat_profileCount = 0;

ook_radio_t replayat_mySwitch = OOK_RADIO_INITIALIZER;

enum { replayat_samplesSUB = 256 };
const double replayat_FrequencySUB = 5000;

double replayat_attenuation_num = 10;

unsigned int replayat_sampling_period;
unsigned long replayat_micro_s;

double replayat_vRealSUB[replayat_samplesSUB];
double replayat_vImagSUB[replayat_samplesSUB];

uint8_t replayat_red[128], replayat_green[128], replayat_blue[128];

unsigned int replayat_epochSUB = 0;
unsigned int replayat_colorcursor = 2016;

int replayat_rssi;

enum { replayat_REPLAY_RX_PIN = SUBGHZ_RX_PIN };
enum { replayat_REPLAY_TX_PIN = SUBGHZ_TX_PIN };

uint32_t replayat_receivedValue = 0;
uint16_t replayat_receivedBitLength = 0;
uint16_t replayat_receivedProtocol = 0;
const int replayat_rssi_threshold = -75;

static const uint32_t replayat_subghz_frequency_list[] = {
    300000000, 303875000, 304250000, 310000000, 314000000, 315000000,
    318000000, 390000000, 418000000, 433075000, 433420000, 433920000,
    434420000, 434775000, 438900000, 868350000, 915000000, 925000000};

uint16_t replayat_currentFrequencyIndex = 0;
int replayat_yshift = 20;

static bool replayat_autoScanEnabled = false;
static uint16_t replayat_scanIndex = 0;
static uint32_t replayat_lastHopMs = 0;
static uint32_t replayat_lockUntilMs = 0;
enum { replayat_SCAN_DWELL_MS = 220 };
enum { replayat_SCAN_DWELL_LOW_MS = 360 };
enum { replayat_SCAN_SETTLE_MS = 70 };
enum { replayat_SCAN_SETTLE_LOW_MS = 95 };
enum { replayat_SCAN_SETTLE_BAND_MS = 140 };
enum { replayat_LOCK_HOLD_MS = 2500 };
enum { replayat_RSSI_LOCK_MS = 1200 };
enum { replayat_RSSI_DETECT_THRESHOLD = -58 };
enum { replayat_RSSI_DETECT_THRESHOLD_LOW = -70 };
enum { replayat_RSSI_CLEAR_THRESHOLD = -66 };
enum { replayat_RSSI_CLEAR_THRESHOLD_LOW = -76 };
enum { replayat_RSSI_DECODE_THRESHOLD = -55 };
enum { replayat_RSSI_DECODE_THRESHOLD_LOW = -66 };
enum { replayat_RSSI_SAMPLE_MS = 45 };
enum { replayat_RSSI_DETECT_HITS = 3 };
enum { replayat_RSSI_DETECT_HITS_LOW = 2 };
enum { replayat_DECODE_MIN_DWELL_MS = 130 };
enum { replayat_DECODE_MIN_DWELL_LOW_MS = 180 };
enum { replayat_UI_SCAN_UPDATE_MS = 250 };
static uint32_t replayat_lastUiScanUpdateMs = 0;
static uint32_t replayat_scanSettledAtMs = 0;
static uint32_t replayat_lastRssiSampleMs = 0;
static uint8_t replayat_rssiDetectStreak = 0;
static bool replayat_rssiHot = false;

static uint32_t replayat_lastDetectAlertMs = 0;
static uint16_t replayat_lastDetectAlertFreq = 0xFFFF;
static uint32_t replayat_notifHideAtMs = 0;
static bool replayat_notifActive = false;

static bool replayat_buzzerArmed = false;
#if BUZZER_USE_PWM
static bool replayat_buzzerPwmReady = false;
#endif
static uint32_t replayat_buzzerOffAtMs = 0;

static void replayat_replayBuzzerWrite(bool on) {
  if (BUZZER_PIN < 0) {
    return;
  }
  gpioOutputWrite(BUZZER_PIN,
                  (on ? BUZZER_ACTIVE_LEVEL : BUZZER_INACTIVE_LEVEL) != 0);
}

static bool replayat_replayBuzzerInit(uint16_t hz) {
  if (BUZZER_PIN < 0) {
    return false;
  }
#if !BUZZER_USE_PWM
  (void)hz;
  replayat_replayBuzzerWrite(false);
  return true;
#else
  if (!replayat_buzzerPwmReady) {
    const ledc_timer_config_t timerConfig = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = hz,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false,
    };
    if (ledc_timer_config(&timerConfig) != ESP_OK) {
      return false;
    }

    const ledc_channel_config_t channelConfig = {
        .gpio_num = BUZZER_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_1,
        .duty = 0,
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
        .flags = {.output_invert = BUZZER_ACTIVE_LEVEL ? 0U : 1U},
        .deconfigure = false,
    };
    if (ledc_channel_config(&channelConfig) != ESP_OK) {
      return false;
    }
    replayat_buzzerPwmReady = true;
  }

  return ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_1, hz) == ESP_OK;
#endif
}

static void replayat_replayBeep(uint16_t hz, uint16_t ms) {
  if (!replayat_replayBuzzerInit(hz)) {
    return;
  }
#if !BUZZER_USE_PWM
  replayat_replayBuzzerWrite(true);
#else
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 128);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
#endif
  replayat_buzzerArmed = true;
  replayat_buzzerOffAtMs = sys_monotonic_ms() + ms;
}

static void replayat_replayBeepPoll(void) {
  if (!replayat_buzzerArmed) return;
  if ((int32_t)(sys_monotonic_ms() - replayat_buzzerOffAtMs) < 0) return;
#if !BUZZER_USE_PWM
  replayat_replayBuzzerWrite(false);
#else
  ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, BUZZER_INACTIVE_LEVEL);
#endif
  replayat_buzzerArmed = false;
}

static void replayat_replayShowDetectNotice(text_t reason, int replayat_rssi) {
  uint32_t now = sys_monotonic_ms();

  if (now - replayat_lastDetectAlertMs < 1200 &&
      replayat_lastDetectAlertFreq == replayat_currentFrequencyIndex)
    return;
  replayat_lastDetectAlertMs = now;
  replayat_lastDetectAlertFreq = replayat_currentFrequencyIndex;

  char msg[96];
  float mhz = replayat_subghz_frequency_list[replayat_currentFrequencyIndex] /
              1000000.0f;

  snprintf(msg, sizeof(msg), "%.16s @ %.2f MHz | RSSI %d", (reason).data, mhz,
           replayat_rssi);
  showNotificationActions("SubGHz Detected", msg, true);
  replayat_replayBeep((strcmp((reason).data, "DECODE") == 0) ? 2600 : 2000, 70);
  replayat_notifActive = true;
  replayat_notifHideAtMs = 0;
}

static inline uint16_t replayat_freqCount(void) {
  return (uint16_t)(sizeof(replayat_subghz_frequency_list) /
                    sizeof(replayat_subghz_frequency_list[0]));
}

static bool replayat_replayFreqIsLowBand(uint16_t idx) {
  return replayat_subghz_frequency_list[idx % replayat_freqCount()] <
         350000000UL;
}

static uint8_t replayat_replayFreqBandId(uint16_t idx) {
  const uint32_t hz =
      replayat_subghz_frequency_list[idx % replayat_freqCount()];
  if (hz < 350000000UL) {
    return 0;
  }
  if (hz < 500000000UL) {
    return 1;
  }
  return 2;
}

static bool replayat_replayFreqBandChanged(uint16_t prevIdx, uint16_t newIdx) {
  return replayat_replayFreqBandId(prevIdx) !=
         replayat_replayFreqBandId(newIdx);
}

static int replayat_replayRssiDetectThreshold(void) {
  return replayat_replayFreqIsLowBand(replayat_currentFrequencyIndex)
             ? replayat_RSSI_DETECT_THRESHOLD_LOW
             : replayat_RSSI_DETECT_THRESHOLD;
}

static int replayat_replayRssiClearThreshold(void) {
  return replayat_replayFreqIsLowBand(replayat_currentFrequencyIndex)
             ? replayat_RSSI_CLEAR_THRESHOLD_LOW
             : replayat_RSSI_CLEAR_THRESHOLD;
}

static int replayat_replayRssiDecodeThreshold(void) {
  return replayat_replayFreqIsLowBand(replayat_currentFrequencyIndex)
             ? replayat_RSSI_DECODE_THRESHOLD_LOW
             : replayat_RSSI_DECODE_THRESHOLD;
}

static uint32_t replayat_replayScanDwellMs(void) {
  return replayat_replayFreqIsLowBand(replayat_currentFrequencyIndex)
             ? replayat_SCAN_DWELL_LOW_MS
             : replayat_SCAN_DWELL_MS;
}

static uint32_t replayat_replayDecodeMinDwellMs(void) {
  return replayat_replayFreqIsLowBand(replayat_currentFrequencyIndex)
             ? replayat_DECODE_MIN_DWELL_LOW_MS
             : replayat_DECODE_MIN_DWELL_MS;
}

static void replayat_tuneToIndex(uint16_t idx, bool persist) {
  replayat_currentFrequencyIndex = idx % replayat_freqCount();
  cc1101_set_sidle(&(cc1101));
  cc1101_set_mhz(
      &(cc1101),
      replayat_subghz_frequency_list[replayat_currentFrequencyIndex] /
          1000000.0);
  cc1101_set_rx_0(&(cc1101));
  if (persist) {
    NV_PUT("subghz", "frequency", replayat_currentFrequencyIndex);
  }
}

static void replayat_replayClearScanLock(void) {
  replayat_lockUntilMs = 0;
  replayat_rssiHot = false;
  replayat_rssiDetectStreak = 0;
}

static bool replayat_replayLooksLikeRealDecode(uint32_t value, uint16_t bits,
                                               uint16_t proto) {
  if (value == 0) {
    return false;
  }
  if (bits < 8 || bits > 64) {
    return false;
  }
  if (proto < 1 || proto > 12) {
    return false;
  }
  return true;
}

static void replayat_replayScanHopTo(uint16_t idx, uint16_t fromIdx) {
  replayat_tuneToIndex(idx, false);
  ook_radio_reset_available(&(replayat_mySwitch));
  ook_radio_set_receive_tolerance(&(replayat_mySwitch),
                                  replayat_replayFreqIsLowBand(idx) ? 50 : 40);

  uint32_t settleMs = replayat_SCAN_SETTLE_MS;
  if (replayat_replayFreqBandChanged(fromIdx, idx)) {
    settleMs = replayat_SCAN_SETTLE_BAND_MS;
  } else if (replayat_replayFreqIsLowBand(idx)) {
    settleMs = replayat_SCAN_SETTLE_LOW_MS;
  }
  replayat_scanSettledAtMs = sys_monotonic_ms() + settleMs;
  replayat_rssiDetectStreak = 0;
  replayat_lastRssiSampleMs = 0;
}

static void replayat_replayBeginAutoScan(void) {
  replayat_scanIndex = replayat_currentFrequencyIndex;
  replayat_lastHopMs = 0;
  replayat_scanSettledAtMs = 0;
  replayat_lastRssiSampleMs = 0;
  replayat_replayClearScanLock();
  replayat_lastUiScanUpdateMs = 0;
  ook_radio_reset_available(&(replayat_mySwitch));
}

static bool replayat_replayAutoScanReadyForDecode(uint32_t now) {
  if (replayat_lastHopMs == 0 ||
      (now - replayat_lastHopMs) < replayat_replayDecodeMinDwellMs()) {
    return false;
  }
  if (now < replayat_scanSettledAtMs) {
    return false;
  }
  return cc1101_get_rssi(&(cc1101)) > replayat_replayRssiDecodeThreshold();
}

static void replayat_replaySampleRssiForScan(uint32_t now) {
  if (now < replayat_scanSettledAtMs) {
    return;
  }
  if (replayat_lastRssiSampleMs != 0 &&
      (now - replayat_lastRssiSampleMs) < replayat_RSSI_SAMPLE_MS) {
    return;
  }

  const int replayat_rssi = cc1101_get_rssi(&(cc1101));
  const int detectThreshold = replayat_replayRssiDetectThreshold();
  const uint8_t detectHits =
      replayat_replayFreqIsLowBand(replayat_currentFrequencyIndex)
          ? replayat_RSSI_DETECT_HITS_LOW
          : replayat_RSSI_DETECT_HITS;
  if (replayat_rssi > detectThreshold) {
    if (replayat_rssiDetectStreak < 255) {
      replayat_rssiDetectStreak++;
    }
    if (!replayat_rssiHot && replayat_rssiDetectStreak >= detectHits) {
      replayat_rssiHot = true;
      replayat_lockUntilMs = now + replayat_RSSI_LOCK_MS;
      NV_PUT("subghz", "frequency", replayat_currentFrequencyIndex);
      replayat_replayShowDetectNotice(TEXT("RSSI"), replayat_rssi);
    }
  } else {
    replayat_rssiDetectStreak = 0;
    if (replayat_rssiHot &&
        replayat_rssi < replayat_replayRssiClearThreshold()) {
      replayat_rssiHot = false;
    }
  }
  replayat_lastRssiSampleMs = now;
}

static void replayat_replayFreqNext(void) {
  replayat_autoScanEnabled = false;
  replayat_replayClearScanLock();
  replayat_tuneToIndex(
      (uint16_t)((replayat_currentFrequencyIndex + 1) % replayat_freqCount()),
      true);
  replayat_updateDisplay();
}

static void replayat_replayFreqPrev(void) {
  replayat_autoScanEnabled = false;
  replayat_replayClearScanLock();
  replayat_tuneToIndex(
      (uint16_t)((replayat_currentFrequencyIndex + replayat_freqCount() - 1) %
                 replayat_freqCount()),
      true);
  replayat_updateDisplay();
}

static void replayat_replayToggleAuto(void) {
  replayat_autoScanEnabled = !replayat_autoScanEnabled;
  if (replayat_autoScanEnabled) {
    replayat_replayBeginAutoScan();
  } else {
    replayat_replayClearScanLock();
  }
  replayat_updateDisplay();
}

static void replayat_replayTrySave(void) {
  if (replayat_receivedValue == 0) {
    return;
  }
  replayat_autoScanEnabled = false;
  replayat_replayClearScanLock();
  replayat_saveProfile();
}

static bool replayat_s_replayStaticDrawn = false;

typedef struct replayat_ReplayDisplayCache {
  uint16_t freqIndex;
  uint8_t modeState;
  uint16_t bitLength;
  int16_t rssi;
  uint16_t protocol;
  uint32_t value;
  bool valid;
} replayat_ReplayDisplayCache;
#define replayat_ReplayDisplayCache_INITIALIZER \
  {.freqIndex = 0xFFFF,                         \
   .modeState = 0xFF,                           \
   .bitLength = 0xFFFF,                         \
   .rssi = -9999,                               \
   .protocol = 0xFFFF,                          \
   .value = 0xFFFFFFFF,                         \
   .valid = false}
;

static replayat_ReplayDisplayCache replayat_s_replayDisp =
    replayat_ReplayDisplayCache_INITIALIZER;

static void replayat_replayInvalidateDisplay(void) {
  replayat_s_replayStaticDrawn = false;
  replayat_s_replayDisp =
      ((replayat_ReplayDisplayCache)replayat_ReplayDisplayCache_INITIALIZER);
}

static void replayat_replayRestoreStatusPanel(void) {
  replayat_replayInvalidateDisplay();
  replayat_updateDisplay();
}

static uint8_t replayat_replayModeState(void) {
  const bool locked =
      (replayat_autoScanEnabled && replayat_lockUntilMs != 0 &&
       (int32_t)(sys_monotonic_ms() - replayat_lockUntilMs) < 0);
  if (locked) {
    return 2;
  }
  return replayat_autoScanEnabled ? 1 : 0;
}

enum { replayat_kReplayStatusLineY = 80 };
enum { replayat_kReplayValueLineH = 11 };

static void replayat_replayDrawStatusSeparator(void) {
  display_draw_fast_h_line(&(tft), 0, replayat_kReplayStatusLineY, 240,
                           UI_LINE);
}

static void replayat_replayDrawValueCell(int x, int y, int w, int h,
                                         text_t text, uint16_t color) {
  const int maxH = replayat_kReplayStatusLineY - y;
  if (maxH <= 0) {
    return;
  }
  const int clipH = DIV_MIN(h, maxH);
  display_fill_rect(&(tft), x, y, w, clipH, TFT_BLACK);
  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), color, TFT_BLACK);
  display_set_cursor(&(tft), x, y);
  display_print(&(tft), (text).data);
}

static void replayat_replayDrawStaticChrome(void) {
  if (replayat_s_replayStaticDrawn) {
    return;
  }

  const int bodyBottom = subghzContentBottom();
  const int infoH = DIV_MIN(replayat_kReplayStatusLineY - 40, bodyBottom - 40);
  if (infoH > 0) {
    display_fill_rect(&(tft), 0, 40, 240, infoH, TFT_BLACK);
  }
  replayat_replayDrawStatusSeparator();

  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), UI_TEXT, TFT_BLACK);
  display_set_cursor(&(tft), 5, 20 + replayat_yshift);
  display_print(&(tft), "Freq:");
  display_set_cursor(&(tft), 5, 35 + replayat_yshift);
  display_print(&(tft), "Bit:");
  display_set_cursor(&(tft), 130, 35 + replayat_yshift);
  display_print(&(tft), "RSSI:");
  display_set_cursor(&(tft), 130, 20 + replayat_yshift);
  display_print(&(tft), "Ptc:");
  display_set_cursor(&(tft), 5, 50 + replayat_yshift);
  display_print(&(tft), "Val:");

  replayat_s_replayStaticDrawn = true;
}

void replayat_replayHandleNavButtons(void) {
  if (!featureHasTouchNavBar()) {
    return;
  }

  if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
    replayat_replayFreqPrev();
    subghzWaitNavRelease(BTN_LEFT);
  }
  if (isTouchNavButtonPressedEdge(BTN_RIGHT)) {
    replayat_replayFreqNext();
    subghzWaitNavRelease(BTN_RIGHT);
  }
  if (isTouchNavButtonPressedEdge(BTN_UP)) {
    if (replayat_receivedValue != 0) {
      replayat_autoScanEnabled = false;
      replayat_replayClearScanLock();
      replayat_sendSignal();
    }
    subghzWaitNavRelease(BTN_UP);
  }
  if (isTouchNavButtonPressedEdge(BTN_DOWN)) {
    replayat_replayTrySave();
    subghzWaitNavRelease(BTN_DOWN);
  }
}

void replayat_updateDisplay(void) {
  replayat_replayDrawStaticChrome();

  const uint8_t modeState = replayat_replayModeState();
  const int16_t replayat_rssi = cc1101_get_rssi(&(cc1101));
  char freqBuf[16];
  char modeBuf[8];
  char bitBuf[8];
  char rssiBuf[8];
  char ptcBuf[8];
  char valBuf[16];

  snprintf(freqBuf, sizeof(freqBuf), "%.2f MHz",
           replayat_subghz_frequency_list[replayat_currentFrequencyIndex] /
               1000000.0);
  if (modeState == 2) {
    snprintf(modeBuf, sizeof(modeBuf), "LOCK");
  } else {
    div_copy_string(modeBuf, sizeof(modeBuf), modeState == 1 ? "AUTO" : "MAN ");
  }
  snprintf(bitBuf, sizeof(bitBuf), "%d", replayat_receivedBitLength);
  snprintf(rssiBuf, sizeof(rssiBuf), "%d", replayat_rssi);
  snprintf(ptcBuf, sizeof(ptcBuf), "%d", replayat_receivedProtocol);
  snprintf(valBuf, sizeof(valBuf), "%lu",
           (unsigned long)replayat_receivedValue);

  const bool fullRedraw = !replayat_s_replayDisp.valid;
  if (fullRedraw ||
      replayat_s_replayDisp.freqIndex != replayat_currentFrequencyIndex) {
    replayat_replayDrawValueCell(50, 20 + replayat_yshift, 72,
                                 replayat_kReplayValueLineH, TEXT(freqBuf),
                                 UI_WARN);
    replayat_s_replayDisp.freqIndex = replayat_currentFrequencyIndex;
  }
  if (fullRedraw || replayat_s_replayDisp.modeState != modeState) {
    replayat_replayDrawValueCell(175, 20 + replayat_yshift, 40,
                                 replayat_kReplayValueLineH, TEXT(modeBuf),
                                 UI_WARN);
    replayat_s_replayDisp.modeState = modeState;
  }
  if (fullRedraw ||
      replayat_s_replayDisp.bitLength != replayat_receivedBitLength) {
    replayat_replayDrawValueCell(50, 35 + replayat_yshift, 40,
                                 replayat_kReplayValueLineH, TEXT(bitBuf),
                                 UI_WARN);
    replayat_s_replayDisp.bitLength = replayat_receivedBitLength;
  }
  if (fullRedraw || replayat_s_replayDisp.rssi != replayat_rssi) {
    replayat_replayDrawValueCell(170, 35 + replayat_yshift, 48,
                                 replayat_kReplayValueLineH, TEXT(rssiBuf),
                                 UI_WARN);
    replayat_s_replayDisp.rssi = replayat_rssi;
  }
  if (fullRedraw ||
      replayat_s_replayDisp.protocol != replayat_receivedProtocol) {
    replayat_replayDrawValueCell(170, 20 + replayat_yshift, 40,
                                 replayat_kReplayValueLineH, TEXT(ptcBuf),
                                 UI_WARN);
    replayat_s_replayDisp.protocol = replayat_receivedProtocol;
  }
  if (fullRedraw || replayat_s_replayDisp.value != replayat_receivedValue) {
    replayat_replayDrawValueCell(50, 50 + replayat_yshift, 180,
                                 replayat_kReplayValueLineH, TEXT(valBuf),
                                 UI_WARN);
    replayat_s_replayDisp.value = replayat_receivedValue;
  }

  replayat_replayDrawStatusSeparator();

  replayat_s_replayDisp.valid = true;

  if (!replayat_autoScanEnabled) {
    cc1101_set_sidle(&(cc1101));
    cc1101_set_mhz(
        &(cc1101),
        replayat_subghz_frequency_list[replayat_currentFrequencyIndex] /
            1000000.0);
    cc1101_set_rx_0(&(cc1101));
  }
}

text_t replayat_getUserInputName(void) {
  OnScreenKeyboardConfig cfg = OnScreenKeyboardConfig_INITIALIZER;
  cfg.titleLine1 = "[!] Set a name for the saved profile.";
  cfg.titleLine2 = "(max 15 chars, ^ caps, # sym)";
  osKeyboardUseStandardLayout(&(cfg));
  cfg.maxLen = MAX_NAME_LENGTH - 1;
  cfg.shuffleNames = replayat_randomNames;
  cfg.shuffleCount = replayat_numRandomNames;
  cfg.buttonsY = 195;
  cfg.backLabel = "Back";
  cfg.middleLabel = "Shuffle";
  cfg.okLabel = "OK";
  cfg.enableShuffle = true;
  cfg.requireNonEmpty = true;
  cfg.emptyErrorMsg = "Name cannot be empty!";

  OnScreenKeyboardResult r = showOnScreenKeyboard(&(cfg), TEXT(""));

  if (!r.accepted) {
    subghzClearBody(TFT_BLACK);
    replayat_uiDrawn = false;
    replayat_replayRestoreStatusPanel();
    replayat_runUI();
    subghzRedrawNavChrome();
  }
  return r.text;
}

void replayat_sendSignal(void) {
  ook_radio_disable_receive(&(replayat_mySwitch));
  sys_sleep_ms(100);
  ook_radio_enable_transmit(&(replayat_mySwitch), replayat_REPLAY_TX_PIN);
  cc1101_set_tx_0(&(cc1101));

  display_fill_rect(&(tft), 0, 40, 240, replayat_kReplayStatusLineY - 40,
                    TFT_BLACK);

  display_set_cursor(&(tft), 10, 30 + replayat_yshift);
  display_print(&(tft), "Sending...");
  display_set_cursor(&(tft), 10, 40 + replayat_yshift);
  display_print(&(tft), (TEXT(replayat_receivedValue)).data);

  ook_radio_set_protocol(&(replayat_mySwitch), replayat_receivedProtocol);
  ook_radio_send(&(replayat_mySwitch), replayat_receivedValue,
                 replayat_receivedBitLength);

  sys_sleep_ms(500);
  display_fill_rect(&(tft), 0, 40, 240, replayat_kReplayStatusLineY - 40,
                    TFT_BLACK);
  display_set_cursor(&(tft), 10, 30 + replayat_yshift);
  display_print(&(tft), "Done!");

  cc1101_set_rx_0(&(cc1101));
  ook_radio_disable_transmit(&(replayat_mySwitch));
  sys_sleep_ms(100);
  ook_radio_enable_receive(&(replayat_mySwitch), replayat_REPLAY_RX_PIN);

  sys_sleep_ms(500);
  replayat_replayRestoreStatusPanel();
}

void replayat_do_sampling(void) {
  const unsigned int kGraphYOffset = 81;
  const int plotY = (int)replayat_epochSUB + (int)kGraphYOffset;
  if (plotY >= subghzContentBottom()) {
    return;
  }

  replayat_micro_s = sys_monotonic_us();

#define ALPHA 0.2
  float ewmaRSSI = -50;

  for (int i = 0; i < replayat_samplesSUB; i++) {
    int replayat_rssi = cc1101_get_rssi(&(cc1101));
    replayat_rssi += 100;

    ewmaRSSI = (ALPHA * replayat_rssi) + ((1 - ALPHA) * ewmaRSSI);

    replayat_vRealSUB[i] = ewmaRSSI * 2;
    replayat_vImagSUB[i] = 1;

    while (sys_monotonic_us() < replayat_micro_s + replayat_sampling_period);
    replayat_micro_s += replayat_sampling_period;
  }

  double mean = 0;

  for (uint16_t i = 0; i < replayat_samplesSUB; i++)
    mean += replayat_vRealSUB[i];
  mean /= replayat_samplesSUB;
  for (uint16_t i = 0; i < replayat_samplesSUB; i++)
    replayat_vRealSUB[i] -= mean;

  replayat_micro_s = sys_monotonic_us();

  signalDspComputeMagnitude(replayat_vRealSUB, replayat_vRealSUB,
                            replayat_samplesSUB);

  unsigned int left_x = 120;
  unsigned int graph_y_offset = kGraphYOffset;
  int max_k = 0;

  for (int j = 0; j < replayat_samplesSUB >> 1; j++) {
    int k = replayat_vRealSUB[j] / replayat_attenuation_num;
    if (k > max_k) max_k = k;
    if (k > 127) k = 127;

    unsigned int color =
        replayat_red[k] << 11 | replayat_green[k] << 5 | replayat_blue[k];
    unsigned int vertical_x = left_x + j;

    display_draw_pixel(&(tft), vertical_x, replayat_epochSUB + graph_y_offset,
                       color);
  }

  for (int j = 0; j < replayat_samplesSUB >> 1; j++) {
    int k = replayat_vRealSUB[j] / replayat_attenuation_num;
    if (k > max_k) max_k = k;
    if (k > 127) k = 127;

    unsigned int color =
        replayat_red[k] << 11 | replayat_green[k] << 5 | replayat_blue[k];
    unsigned int mirrored_x = left_x - j;
    display_draw_pixel(&(tft), mirrored_x, replayat_epochSUB + graph_y_offset,
                       color);
  }

  double tattenuation = max_k / 127.0;

  if (tattenuation > replayat_attenuation_num)
    replayat_attenuation_num = tattenuation;

  sys_sleep_ms(10);
}

void replayat_readProfileCount(void) {
  NV_GET("subghz", "count", replayat_profileCount);
  if (replayat_profileCount > MAX_PROFILES) replayat_profileCount = 0;
}

void replayat_saveProfile(void) {
  replayat_readProfileCount();

  if (replayat_profileCount >= MAX_PROFILES) {
    text_t err = {0}, outPath = {0};
    if (exportProfilesToSD(&(outPath), &err)) {
      clearProfilesInNvs();
      replayat_profileCount = 0;

      syncCurrentProfilesToSD(NULL);
    } else {
      subghzClearBody(TFT_BLACK);
      display_set_text_size(&(tft), 1);
      display_set_cursor(&(tft), 10, 30 + replayat_yshift);
      display_set_text_color_2(&(tft), UI_WARN, TFT_BLACK);
      display_print(&(tft), "Storage full!");
      display_set_cursor(&(tft), 10, 45 + replayat_yshift);
      display_set_text_color_2(&(tft), UI_TEXT, TFT_BLACK);
      display_print(&(tft), "Insert SD / export fail");
      display_set_cursor(&(tft), 10, 60 + replayat_yshift);
      display_print(&(tft), (err).data);
      replayat_uiDrawn = false;
      replayat_runUI();
      subghzRedrawNavChrome();
      if (!subghzWaitWithNav(2000)) {
        return;
      }
      replayat_replayRestoreStatusPanel();
      float currentBatteryVoltage = readBatteryVoltage();
      drawStatusBar(currentBatteryVoltage, true, false);
      replayat_uiDrawn = false;
      replayat_runUI();
      subghzRedrawNavChrome();
      return;
    }
  }

  if (replayat_profileCount < MAX_PROFILES) {
    text_t customName = replayat_getUserInputName();

    display_set_text_size(&(tft), 1);

    replayat_Profile newProfile = replayat_Profile_INITIALIZER;
    newProfile.frequency =
        replayat_subghz_frequency_list[replayat_currentFrequencyIndex];
    newProfile.value = (uint32_t)replayat_receivedValue;
    newProfile.bitLength = (uint16_t)replayat_receivedBitLength;
    newProfile.protocol = (uint16_t)replayat_receivedProtocol;
    strncpy(newProfile.name, (customName).data, MAX_NAME_LENGTH - 1);
    newProfile.name[MAX_NAME_LENGTH - 1] = '\0';

    __auto_type key = nv_key("profile", replayat_profileCount);
    NV_PUT("subghz", (key).data, newProfile);

    replayat_profileCount++;

    NV_PUT("subghz", "count", replayat_profileCount);

    syncCurrentProfilesToSD(NULL);

    subghzClearBody(TFT_BLACK);
    display_set_cursor(&(tft), 10, 30 + replayat_yshift);
    display_print(&(tft), "Profile saved!");
    display_set_cursor(&(tft), 10, 40 + replayat_yshift);
    display_print(&(tft), "Name: ");
    display_print(&(tft), (TEXT(newProfile.name)).data);
    display_set_cursor(&(tft), 10, 50 + replayat_yshift);
    display_print(&(tft), "Profiles saved: ");
    display_print_line_cstr(&(tft), (TEXT(replayat_profileCount)).data);

  } else {
    subghzClearBody(TFT_BLACK);
    display_set_text_size(&(tft), 1);
    display_set_cursor(&(tft), 10, 30 + replayat_yshift);
    display_set_text_color_2(&(tft), UI_TEXT, TFT_BLACK);
    display_print(&(tft), "Profile storage full!");
  }

  replayat_uiDrawn = false;
  replayat_runUI();
  subghzRedrawNavChrome();
  if (!subghzWaitWithNav(2000)) {
    return;
  }
  replayat_replayRestoreStatusPanel();
  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true, false);
  replayat_uiDrawn = false;
  replayat_runUI();
  subghzRedrawNavChrome();
}

void replayat_loadProfileCount(void) { replayat_readProfileCount(); }

void replayat_runUI(void) {
#undef STATUS_BAR_Y_OFFSET
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#undef ICON_NUM
#define ICON_NUM 6

  static int iconX[ICON_NUM] = {90, 130, 170, 210, 50, 10};
  static int iconY = STATUS_BAR_Y_OFFSET;

  static const unsigned char* icons[ICON_NUM] = {
      bitmap_icon_sort_up_plus, bitmap_icon_sort_down_minus,
      bitmap_icon_antenna,      bitmap_icon_floppy,
      bitmap_icon_random,       bitmap_icon_go_back};

  if (!replayat_uiDrawn) {
    display_fill_rect(&(tft), 0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH,
                      STATUS_BAR_HEIGHT, DARK_GRAY);

    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        display_draw_bitmap(&(tft), iconX[i], iconY, icons[i], ICON_SIZE,
                            ICON_SIZE, UI_ICON);
      }
    }
    display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
    display_draw_fast_h_line(&(tft), 0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT,
                             240, UI_LINE);
    replayat_uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  if (animationState > 0 && sys_monotonic_ms() - lastAnimationTime >= 150) {
    if (animationState == 1) {
      display_draw_bitmap(&(tft), iconX[activeIcon], iconY, icons[activeIcon],
                          ICON_SIZE, ICON_SIZE, UI_ICON);
      animationState = 2;

      switch (activeIcon) {
        case 0:
          replayat_autoScanEnabled = false;
          replayat_currentFrequencyIndex =
              (replayat_currentFrequencyIndex + 1) %
              (sizeof(replayat_subghz_frequency_list) /
               sizeof(replayat_subghz_frequency_list[0]));
          replayat_tuneToIndex(replayat_currentFrequencyIndex, true);
          replayat_updateDisplay();
          break;
        case 1:
          replayat_autoScanEnabled = false;
          replayat_currentFrequencyIndex =
              (replayat_currentFrequencyIndex - 1 +
               (sizeof(replayat_subghz_frequency_list) /
                sizeof(replayat_subghz_frequency_list[0]))) %
              (sizeof(replayat_subghz_frequency_list) /
               sizeof(replayat_subghz_frequency_list[0]));
          replayat_tuneToIndex(replayat_currentFrequencyIndex, true);
          replayat_updateDisplay();
          break;
        case 2:
          replayat_sendSignal();
          break;
        case 3:
          replayat_saveProfile();
          break;
        case 4:
          replayat_autoScanEnabled = !replayat_autoScanEnabled;
          if (replayat_autoScanEnabled) {
            replayat_replayBeginAutoScan();
          } else {
            replayat_replayClearScanLock();
          }
          replayat_updateDisplay();
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

void replayat_ReplayAttackSetup(void) {
  div_console_open(115200);
  setTouchButtonInputEnabled(true);
  subghzSetReplayNavLabels();

  cc1101_set_spi_pin(&(cc1101), CC1101_SCK, CC1101_MISO, CC1101_MOSI,
                     CC1101_CS);

  cc1101_init(&(cc1101));
  cc1101_set_modulation(&(cc1101), 2);
  cc1101_set_rx_bw(&(cc1101), 500.0);

  cc1101_set_gdo(&(cc1101), CC1101_GDO0, CC1101_GDO2);

  cc1101_set_rx_0(&(cc1101));

  ook_radio_enable_receive(&(replayat_mySwitch), replayat_REPLAY_RX_PIN);
  ook_radio_enable_transmit(&(replayat_mySwitch), replayat_REPLAY_TX_PIN);
  ook_radio_set_repeat_transmit(&(replayat_mySwitch), 8);

  replayat_readProfileCount();

  NV_GET("subghz", "value", replayat_receivedValue);
  NV_GET("subghz", "bits", replayat_receivedBitLength);
  NV_GET("subghz", "protocol", replayat_receivedProtocol);
  NV_GET("subghz", "frequency", replayat_currentFrequencyIndex);

  const uint16_t replayat_freqCount =
      (uint16_t)(sizeof(replayat_subghz_frequency_list) /
                 sizeof(replayat_subghz_frequency_list[0]));
  if (replayat_currentFrequencyIndex >= replayat_freqCount)
    replayat_currentFrequencyIndex = 0;

  replayat_tuneToIndex(replayat_currentFrequencyIndex, false);
  ook_radio_set_receive_tolerance(
      &(replayat_mySwitch),
      replayat_replayFreqIsLowBand(replayat_currentFrequencyIndex) ? 50 : 40);

  subghzClearBody(TFT_BLACK);
  display_set_rotation(&(tft), TFT_ROTATION);

  drawStatusBar(readBatteryVoltage(), true, false);
  subghzRedrawNavChrome();

#if HAS_PCF8574_BUTTONS
  pcf8574_configure_pin(button_input_device(), BTN_LEFT, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_RIGHT, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_UP, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_DOWN, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_SELECT, GPIO_MODE_INPUT);
#endif

  replayat_sampling_period = round(1000000 * (1.0 / replayat_FrequencySUB));

  for (int i = 0; i < 32; i++) {
    replayat_red[i] = i / 2;
    replayat_green[i] = 0;
    replayat_blue[i] = i;
  }
  for (int i = 32; i < 64; i++) {
    replayat_red[i] = i / 2;
    replayat_green[i] = 0;
    replayat_blue[i] = 63 - i;
  }
  for (int i = 64; i < 96; i++) {
    replayat_red[i] = 31;
    replayat_green[i] = (i - 64) * 2;
    replayat_blue[i] = 0;
  }
  for (int i = 96; i < 128; i++) {
    replayat_red[i] = 31;
    replayat_green[i] = 63;
    replayat_blue[i] = i - 96;
  }

  replayat_replayInvalidateDisplay();
  replayat_updateDisplay();
  replayat_uiDrawn = false;
  subghzRedrawNavChrome();
}

void replayat_ReplayAttackLoop(void) {
  if (feature_active &&
      (feature_exit_requested || featureExitButtonPressed())) {
    feature_exit_requested = true;
    return;
  }

  maintainTouchNavBar();
  replayat_runUI();
  if (replayat_uiDrawn) {
    display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
    display_draw_fast_h_line(&(tft), 0, 36, 240, UI_LINE);
    if (replayat_s_replayDisp.valid) {
      replayat_replayDrawStatusSeparator();
    }
  }
  replayat_replayHandleNavButtons();

  static unsigned long lastDebounceTime = 0;
  const unsigned long debounceDelay = 200;

  static bool prevLeft = false, prevRight = false, prevUp = false,
              prevDown = false;
  const bool leftPressed = isPhysicalButtonPressed(BTN_LEFT);
  const bool rightPressed = isPhysicalButtonPressed(BTN_RIGHT);
  const bool upPressed = isPhysicalButtonPressed(BTN_UP);
  const bool downPressed = isPhysicalButtonPressed(BTN_DOWN);

  replayat_replayBeepPoll();

  if (replayat_notifActive && isNotificationVisible()) {
    int x, y;
    if (readTouchXY(&(x), &(y))) {
      NotificationAction act = notificationHandleTouch(x, y);
      if (act == NotificationAction_Save) {
        replayat_notifActive = false;

        subghzClearBody(TFT_BLACK);
        replayat_uiDrawn = false;
        replayat_replayInvalidateDisplay();
        float v = readBatteryVoltage();
        drawStatusBar(v, true, false);
        replayat_runUI();
        replayat_updateDisplay();
        subghzRedrawNavChrome();

        replayat_autoScanEnabled = false;
        replayat_saveProfile();

        subghzClearBody(TFT_BLACK);
        replayat_uiDrawn = false;
        replayat_replayInvalidateDisplay();
        v = readBatteryVoltage();
        drawStatusBar(v, true, false);
        replayat_runUI();
        replayat_updateDisplay();
        subghzRedrawNavChrome();
      } else if (act == NotificationAction_Ok ||
                 act == NotificationAction_Close) {
        replayat_notifActive = false;

        replayat_lastDetectAlertMs = sys_monotonic_ms();
        replayat_lastDetectAlertFreq = replayat_currentFrequencyIndex;
        replayat_lockUntilMs = sys_monotonic_ms() + 1500;
        replayat_rssiHot = true;

        subghzClearBody(TFT_BLACK);
        replayat_uiDrawn = false;
        replayat_replayInvalidateDisplay();
        float v = readBatteryVoltage();
        drawStatusBar(v, true, false);
        replayat_runUI();
        replayat_updateDisplay();
        subghzRedrawNavChrome();
      }
    }

    return;
  } else if (replayat_notifActive && !isNotificationVisible()) {
    replayat_notifActive = false;
    subghzClearBody(TFT_BLACK);
    replayat_uiDrawn = false;
    replayat_replayInvalidateDisplay();
    float v = readBatteryVoltage();
    drawStatusBar(v, true, false);
    replayat_runUI();
    replayat_updateDisplay();
    subghzRedrawNavChrome();
  }

  if (rightPressed && !prevRight &&
      sys_monotonic_ms() - lastDebounceTime > debounceDelay) {
    replayat_replayFreqNext();
    lastDebounceTime = sys_monotonic_ms();
  }
  if (leftPressed && !prevLeft &&
      sys_monotonic_ms() - lastDebounceTime > debounceDelay) {
    replayat_replayFreqPrev();
    lastDebounceTime = sys_monotonic_ms();
  }
  if (upPressed && !prevUp && replayat_receivedValue != 0 &&
      sys_monotonic_ms() - lastDebounceTime > debounceDelay) {
    replayat_autoScanEnabled = false;
    replayat_replayClearScanLock();
    replayat_sendSignal();
    lastDebounceTime = sys_monotonic_ms();
  }
  if (downPressed && !prevDown &&
      sys_monotonic_ms() - lastDebounceTime > debounceDelay) {
    replayat_replayTrySave();
    lastDebounceTime = sys_monotonic_ms();
  }

  prevLeft = leftPressed;
  prevRight = rightPressed;
  prevUp = upPressed;
  prevDown = downPressed;

  if (replayat_autoScanEnabled) {
    const uint32_t now = sys_monotonic_ms();
    const bool scanLocked = (replayat_lockUntilMs != 0 &&
                             (int32_t)(now - replayat_lockUntilMs) < 0);

    if (!scanLocked &&
        (replayat_lastHopMs == 0 ||
         (now - replayat_lastHopMs) >= replayat_replayScanDwellMs())) {
      const uint16_t fromIdx = replayat_currentFrequencyIndex;
      replayat_scanIndex =
          (uint16_t)((replayat_scanIndex + 1) % replayat_freqCount());
      replayat_replayScanHopTo(replayat_scanIndex, fromIdx);
      replayat_lastHopMs = now;
      replayat_rssiHot = false;
    }

    replayat_replaySampleRssiForScan(now);

    if (replayat_lastUiScanUpdateMs == 0 ||
        (now - replayat_lastUiScanUpdateMs) >= replayat_UI_SCAN_UPDATE_MS) {
      replayat_updateDisplay();
      replayat_lastUiScanUpdateMs = now;
    }
  }

  if (!replayat_autoScanEnabled) {
    replayat_do_sampling();
  }
  sys_sleep_ms(10);
  replayat_epochSUB++;

  if (replayat_epochSUB >= display_width(&(tft))) replayat_epochSUB = 0;

  if (ook_radio_available(&(replayat_mySwitch))) {
    const uint32_t val = ook_radio_get_received_value(&(replayat_mySwitch));
    const uint16_t bits =
        ook_radio_get_received_bitlength(&(replayat_mySwitch));
    const uint16_t proto =
        ook_radio_get_received_protocol(&(replayat_mySwitch));
    ook_radio_reset_available(&(replayat_mySwitch));

    const uint32_t now = sys_monotonic_ms();
    const bool validDecode =
        replayat_replayLooksLikeRealDecode(val, bits, proto) &&
        (!replayat_autoScanEnabled ||
         replayat_replayAutoScanReadyForDecode(now));

    if (validDecode) {
      replayat_receivedValue = val;
      replayat_receivedBitLength = bits;
      replayat_receivedProtocol = proto;

      NV_PUT("subghz", "value", replayat_receivedValue);
      NV_PUT("subghz", "bits", replayat_receivedBitLength);
      NV_PUT("subghz", "protocol", replayat_receivedProtocol);

      replayat_updateDisplay();

      if (replayat_autoScanEnabled) {
        replayat_lockUntilMs = now + replayat_LOCK_HOLD_MS;
        replayat_scanIndex = replayat_currentFrequencyIndex;
        replayat_rssiHot = false;
        replayat_rssiDetectStreak = 0;

        NV_PUT("subghz", "frequency", replayat_currentFrequencyIndex);
        replayat_replayShowDetectNotice(TEXT("DECODE"),
                                        cc1101_get_rssi(&(cc1101)));
      }
    }
  }
}

void SavedProfile_updateDisplay(void);
void SavedProfile_runUI(void);
void SavedProfile_transmitProfile(int index);
void SavedProfile_deleteProfile(int index);

static bool SavedProfile_uiDrawn = false;

#define MAX_PROFILES 5
#define MAX_NAME_LENGTH 16

#define SCREEN_WIDTH 240
#undef SCREEN_HEIGHT
#define SCREEN_HEIGHT 320

ook_radio_t SavedProfile_mySwitch = OOK_RADIO_INITIALIZER;
typedef struct __attribute__((packed)) SavedProfile_Profile {
  uint32_t frequency;
  uint32_t value;
  uint16_t bitLength;
  uint16_t protocol;
  char name[MAX_NAME_LENGTH];
} SavedProfile_Profile;
#define SavedProfile_Profile_INITIALIZER {0}
;

#define PROFILE_SIZE sizeof(Profile)

uint16_t SavedProfile_profileCount = 0;
uint16_t SavedProfile_currentProfileIndex = 0;

int SavedProfile_yshift = 16;

static c_vector_t SavedProfile_sdFiles = {0};
static uint16_t SavedProfile_sdTotalProfiles = 0;
static text_t SavedProfile_sdLastErr = {.data = ""};
static text_t SavedProfile_selectedPath = {.data = ""};
static uint16_t SavedProfile_selectedLocalIdx = 0;
static SavedProfile_Profile SavedProfile_selectedProfile =
    SavedProfile_Profile_INITIALIZER;
static bool SavedProfile_selectedValid = false;

enum { SavedProfile_ITEMS_PER_PAGE = 7 };
enum { SavedProfile_PROFILE_PAD_X = 10 };
enum { SavedProfile_LIST_X = SavedProfile_PROFILE_PAD_X };
enum { SavedProfile_LIST_W = 220 };

enum { SavedProfile_PROFILE_HEADER_Y = 50 };
enum { SavedProfile_PROFILE_HEADER_H = 14 };
enum {
  SavedProfile_LIST_Y = SavedProfile_PROFILE_HEADER_Y +
  SavedProfile_PROFILE_HEADER_H + 2
};
enum { SavedProfile_ROW_H = 18 };
enum { SavedProfile_PROFILE_LINE_H = 14 };
enum { SavedProfile_PROFILE_LINE_GAP = 3 };
enum {
  SavedProfile_PROFILE_LINE_STEP = SavedProfile_PROFILE_LINE_H +
  SavedProfile_PROFILE_LINE_GAP
};
enum { SavedProfile_PROFILE_INFO_LINES = 4 };
enum {
  SavedProfile_PROFILE_INFO_CONTENT_H = SavedProfile_PROFILE_LINE_STEP *
      (SavedProfile_PROFILE_INFO_LINES - 1) +
  SavedProfile_PROFILE_LINE_H
};
enum { SavedProfile_PROFILE_LABEL_X = SavedProfile_PROFILE_PAD_X };
enum { SavedProfile_PROFILE_VALUE_X = 50 };
enum { SavedProfile_PROFILE_COL2_LABEL_X = 130 };
enum { SavedProfile_PROFILE_COL2_VALUE_X = 165 };

enum { SavedProfile_UI_GAP_Y = 6 };

static int SavedProfile_profileBottomY(void) { return subghzContentBottom(); }

static int SavedProfile_profileListBottom(void) {
  return SavedProfile_LIST_Y +
         (SavedProfile_ITEMS_PER_PAGE * SavedProfile_ROW_H);
}

static int SavedProfile_profileDetailsY(void) {
  const int areaTop = SavedProfile_profileListBottom();
  const int areaBottom = SavedProfile_profileBottomY();
  const int areaH = areaBottom - areaTop;
  if (areaH <= SavedProfile_PROFILE_INFO_CONTENT_H) {
    return areaTop + SavedProfile_UI_GAP_Y;
  }
  return areaTop + (areaH - SavedProfile_PROFILE_INFO_CONTENT_H) / 2;
}

static void SavedProfile_profileClearContentArea(uint16_t color) {
  enum { top = 40 };
  const int h = subghzContentBottom() - top;
  if (h > 0) {
    display_fill_rect(&(tft), 0, top, 240, h, color);
  }
}

static void SavedProfile_profileRestoreChrome(void) {
  drawStatusBar(readBatteryVoltage(), true, false);
  SavedProfile_uiDrawn = false;
  SavedProfile_updateDisplay();
  SavedProfile_runUI();
  subghzRedrawNavChrome();
}

static void SavedProfile_updateSelectionUI(uint16_t oldIndex,
                                           bool forceListRedraw);

static uint16_t SavedProfile_cachedPageStart = 0xFFFF;
static SubGhzProfile SavedProfile_cachedPage[SavedProfile_ITEMS_PER_PAGE] =
    SubGhzProfile_INITIALIZER;
static bool SavedProfile_cachedOk[SavedProfile_ITEMS_PER_PAGE] = {0};
static bool SavedProfile_cacheDirty = true;

static bool SavedProfile_deleteArmed = false;
static uint32_t SavedProfile_deleteArmUntilMs = 0;

static void SavedProfile_refreshSdIndex(bool keepSelection) {
  uint16_t oldIdx = SavedProfile_currentProfileIndex;
  text_t err = {0};
  if (!listAllProfileFiles(&(SavedProfile_sdFiles), &err)) {
    c_vector_free(&(SavedProfile_sdFiles));
    SavedProfile_sdTotalProfiles = 0;
    SavedProfile_currentProfileIndex = 0;
    SavedProfile_selectedValid = false;
    SavedProfile_sdLastErr = err;
    SavedProfile_cacheDirty = true;
    return;
  }
  SavedProfile_sdLastErr = TEXT("");
  SavedProfile_sdTotalProfiles = totalProfilesInIndex(&(SavedProfile_sdFiles));
  if (SavedProfile_sdTotalProfiles == 0) {
    SavedProfile_currentProfileIndex = 0;
    SavedProfile_selectedValid = false;
    SavedProfile_sdLastErr = TEXT("No profiles found");
    SavedProfile_cacheDirty = true;
    return;
  }
  if (keepSelection) SavedProfile_currentProfileIndex = oldIdx;
  if (SavedProfile_currentProfileIndex >= SavedProfile_sdTotalProfiles)
    SavedProfile_currentProfileIndex =
        (uint16_t)(SavedProfile_sdTotalProfiles - 1);
  SavedProfile_selectedValid = false;
  SavedProfile_cacheDirty = true;
}

static bool SavedProfile_loadSelectedFromSd(text_t* errOut) {
  if (SavedProfile_sdTotalProfiles == 0) {
    SavedProfile_selectedValid = false;
    return false;
  }
  if (!locateGlobalIndex(
          &(SavedProfile_sdFiles), SavedProfile_currentProfileIndex,
          &(SavedProfile_selectedPath), &(SavedProfile_selectedLocalIdx))) {
    SavedProfile_selectedValid = false;
    if (errOut) *errOut = TEXT("Locate failed");
    return false;
  }
  SubGhzProfile p = SubGhzProfile_INITIALIZER;
  if (!readProfileAt(SavedProfile_selectedPath, SavedProfile_selectedLocalIdx,
                     &(p), errOut)) {
    SavedProfile_selectedValid = false;
    return false;
  }

  SavedProfile_selectedProfile.frequency = p.frequency;
  SavedProfile_selectedProfile.value = p.value;
  SavedProfile_selectedProfile.bitLength = p.bitLength;
  SavedProfile_selectedProfile.protocol = p.protocol;
  memcpy(SavedProfile_selectedProfile.name, p.name, MAX_NAME_LENGTH);
  SavedProfile_selectedProfile.name[MAX_NAME_LENGTH - 1] = '\0';
  SavedProfile_selectedValid = true;
  return true;
}

static uint16_t SavedProfile_pageStartForIndex(uint16_t idx) {
  return (uint16_t)((idx / SavedProfile_ITEMS_PER_PAGE) *
                    SavedProfile_ITEMS_PER_PAGE);
}

static void SavedProfile_ensurePageCache(void) {
  if (SavedProfile_sdTotalProfiles == 0) return;
  uint16_t start =
      SavedProfile_pageStartForIndex(SavedProfile_currentProfileIndex);
  if (!SavedProfile_cacheDirty && SavedProfile_cachedPageStart == start) return;
  SavedProfile_cachedPageStart = start;
  for (uint8_t i = 0; i < SavedProfile_ITEMS_PER_PAGE; i++) {
    SavedProfile_cachedOk[i] = false;
    uint16_t globalIdx = (uint16_t)(start + i);
    if (globalIdx >= SavedProfile_sdTotalProfiles) continue;
    text_t pth = {0};
    uint16_t li = 0;
    if (!locateGlobalIndex(&(SavedProfile_sdFiles), globalIdx, &(pth), &(li)))
      continue;
    text_t err = {0};
    SavedProfile_cachedOk[i] =
        readProfileAt(pth, li, &(SavedProfile_cachedPage[i]), &err);
    if (!SavedProfile_cachedOk[i])
      memset(&SavedProfile_cachedPage[i], 0, sizeof(SubGhzProfile));
  }
  SavedProfile_cacheDirty = false;
}

static void SavedProfile_drawHeaderLine(void) {
  const int hy = SavedProfile_PROFILE_HEADER_Y;
  display_fill_rect(&(tft), SavedProfile_LIST_X, hy, SavedProfile_LIST_W,
                    SavedProfile_PROFILE_HEADER_H, TFT_BLACK);
  display_set_text_color_2(&(tft), UI_WARN, TFT_BLACK);
  display_set_cursor(&(tft), SavedProfile_LIST_X, hy);
  display_printf(&(tft), "Profile %d/%d",
                 (int)SavedProfile_currentProfileIndex + 1,
                 (int)SavedProfile_sdTotalProfiles);
}

static void SavedProfile_profileSelectNext(void) {
  if (SavedProfile_sdTotalProfiles == 0) {
    return;
  }
  uint16_t oldIdx = SavedProfile_currentProfileIndex;
  SavedProfile_currentProfileIndex =
      (uint16_t)((SavedProfile_currentProfileIndex + 1) %
                 SavedProfile_sdTotalProfiles);
  SavedProfile_selectedValid = false;
  SavedProfile_updateSelectionUI(oldIdx, false);
}

static void SavedProfile_profileSelectPrev(void) {
  if (SavedProfile_sdTotalProfiles == 0) {
    return;
  }
  uint16_t oldIdx = SavedProfile_currentProfileIndex;
  SavedProfile_currentProfileIndex =
      (uint16_t)((SavedProfile_currentProfileIndex +
                  SavedProfile_sdTotalProfiles - 1) %
                 SavedProfile_sdTotalProfiles);
  SavedProfile_selectedValid = false;
  SavedProfile_updateSelectionUI(oldIdx, false);
}

static void SavedProfile_profileRefreshSd(void) {
  SavedProfile_refreshSdIndex(true);
  SavedProfile_selectedValid = false;
  SavedProfile_cacheDirty = true;
  SavedProfile_deleteArmed = false;
  SavedProfile_updateDisplay();
}

void SavedProfile_profileHandleNavButtons(void) {
  if (!featureHasTouchNavBar()) {
    return;
  }

  if (isTouchNavButtonPressedEdge(BTN_SELECT)) {
    feature_exit_requested = true;
    return;
  }
  if (isTouchNavButtonPressedEdge(BTN_UP)) {
    SavedProfile_profileSelectPrev();
    subghzWaitNavRelease(BTN_UP);
  }
  if (isTouchNavButtonPressedEdge(BTN_DOWN)) {
    SavedProfile_profileSelectNext();
    subghzWaitNavRelease(BTN_DOWN);
  }
  if (isTouchNavButtonPressedEdge(BTN_RIGHT)) {
    if (SavedProfile_sdTotalProfiles > 0) {
      SavedProfile_transmitProfile(SavedProfile_currentProfileIndex);
    }
    subghzWaitNavRelease(BTN_RIGHT);
  }
  if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
    if (SavedProfile_sdTotalProfiles > 0) {
      SavedProfile_deleteProfile(SavedProfile_currentProfileIndex);
    }
    subghzWaitNavRelease(BTN_LEFT);
  }
}

static void SavedProfile_drawRow(uint16_t pageStart, uint8_t row) {
  uint16_t globalIdx = (uint16_t)(pageStart + row);
  if (globalIdx >= SavedProfile_sdTotalProfiles) return;

  bool isSel = (globalIdx == SavedProfile_currentProfileIndex);
  int y = SavedProfile_LIST_Y + (row * SavedProfile_ROW_H);

  uint16_t bg = isSel ? DARK_GRAY : TFT_BLACK;
  uint16_t fg = isSel ? UI_WARN : UI_DIM_TEXT;
  display_fill_rect(&(tft), SavedProfile_LIST_X, y, SavedProfile_LIST_W,
                    SavedProfile_ROW_H - 1, bg);
  display_set_text_color_2(&(tft), fg, bg);
  display_set_cursor(&(tft), SavedProfile_LIST_X + 2, y + 4);
  display_printf(&(tft), "%2d.", (int)globalIdx + 1);
  display_set_cursor(&(tft), SavedProfile_LIST_X + 34, y + 4);

  if (SavedProfile_cachedOk[row]) {
    char nameBuf[17];
    memcpy(nameBuf, SavedProfile_cachedPage[row].name, 16);
    nameBuf[16] = '\0';
    text_t nm = TEXT(nameBuf);
    if (strlen((nm).data) > 10) nm = text_slice(nm, 0, 10);
    display_print(&(tft), (nm).data);

    char fbuf[16];
    snprintf(fbuf, sizeof(fbuf), "%.2f",
             SavedProfile_cachedPage[row].frequency / 1000000.0);
    int tw = display_text_width(&(tft), fbuf, 1);
    display_set_cursor(
        &(tft), SavedProfile_LIST_X + SavedProfile_LIST_W - 4 - tw, y + 4);
    display_print(&(tft), (TEXT(fbuf)).data);
  } else {
    display_print(&(tft), "<?>");
  }
}

static void SavedProfile_drawListPage(uint16_t pageStart) {
  SavedProfile_ensurePageCache();

  display_fill_rect(
      &(tft), SavedProfile_LIST_X, SavedProfile_LIST_Y, SavedProfile_LIST_W,
      (SavedProfile_ITEMS_PER_PAGE * SavedProfile_ROW_H), TFT_BLACK);
  for (uint8_t row = 0; row < SavedProfile_ITEMS_PER_PAGE; row++) {
    if ((uint16_t)(pageStart + row) >= SavedProfile_sdTotalProfiles) break;
    SavedProfile_drawRow(pageStart, row);
  }
}

static void SavedProfile_drawDetails(void) {
  const int detailsY = SavedProfile_profileDetailsY();
  const int gapTop = SavedProfile_profileListBottom();
  const int gapH = SavedProfile_profileBottomY() - gapTop;
  if (gapH > 0) {
    display_fill_rect(&(tft), SavedProfile_LIST_X, gapTop, SavedProfile_LIST_W,
                      gapH, TFT_BLACK);
  }
  display_draw_fast_h_line(&(tft), SavedProfile_LIST_X,
                           SavedProfile_profileListBottom(),
                           SavedProfile_LIST_W, UI_LINE);

  text_t err = {0};
  if (!SavedProfile_selectedValid) {
    SavedProfile_loadSelectedFromSd(&err);
  }

  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), UI_TEXT, TFT_BLACK);
  if (!SavedProfile_selectedValid) {
    display_set_cursor(&(tft), SavedProfile_PROFILE_LABEL_X, detailsY);
    display_print(&(tft), "Read failed:");
    display_set_cursor(&(tft), SavedProfile_PROFILE_VALUE_X, detailsY);
    display_print(&(tft), (err).data);
    return;
  }

  display_set_cursor(&(tft), SavedProfile_PROFILE_LABEL_X, detailsY);
  display_print(&(tft), "Name:");
  display_set_cursor(&(tft), SavedProfile_PROFILE_VALUE_X, detailsY);
  display_print(&(tft), (TEXT(SavedProfile_selectedProfile.name)).data);

  display_set_cursor(&(tft), SavedProfile_PROFILE_LABEL_X,
                     detailsY + SavedProfile_PROFILE_LINE_STEP);
  display_print(&(tft), "Freq:");
  display_set_cursor(&(tft), SavedProfile_PROFILE_VALUE_X,
                     detailsY + SavedProfile_PROFILE_LINE_STEP);
  display_printf(&(tft), "%.2f MHz",
                 SavedProfile_selectedProfile.frequency / 1000000.0);
  display_set_cursor(&(tft), SavedProfile_PROFILE_COL2_LABEL_X,
                     detailsY + SavedProfile_PROFILE_LINE_STEP);
  display_print(&(tft), "Ptc:");
  display_set_cursor(&(tft), SavedProfile_PROFILE_COL2_VALUE_X,
                     detailsY + SavedProfile_PROFILE_LINE_STEP);
  display_print(&(tft), (TEXT(SavedProfile_selectedProfile.protocol)).data);

  display_set_cursor(&(tft), SavedProfile_PROFILE_LABEL_X,
                     detailsY + (SavedProfile_PROFILE_LINE_STEP * 2));
  display_print(&(tft), "Val:");
  display_set_cursor(&(tft), SavedProfile_PROFILE_VALUE_X,
                     detailsY + (SavedProfile_PROFILE_LINE_STEP * 2));
  display_print(&(tft),
                (TEXT((unsigned long)SavedProfile_selectedProfile.value)).data);
  display_set_cursor(&(tft), SavedProfile_PROFILE_COL2_LABEL_X,
                     detailsY + (SavedProfile_PROFILE_LINE_STEP * 2));
  display_print(&(tft), "Bit:");
  display_set_cursor(&(tft), SavedProfile_PROFILE_COL2_VALUE_X,
                     detailsY + (SavedProfile_PROFILE_LINE_STEP * 2));
  display_print(&(tft), (TEXT(SavedProfile_selectedProfile.bitLength)).data);

  display_set_text_color_2(&(tft), UI_DIM_TEXT, TFT_BLACK);
  display_set_cursor(&(tft), SavedProfile_PROFILE_LABEL_X,
                     detailsY + (SavedProfile_PROFILE_LINE_STEP * 3));
  display_print(&(tft), "SRC:");
  display_set_cursor(&(tft), SavedProfile_PROFILE_VALUE_X,
                     detailsY + (SavedProfile_PROFILE_LINE_STEP * 3));
  if (text_suffix_cstr((SavedProfile_selectedPath).data,
                       "profiles_current.bin")) {
    display_print(&(tft), "current");
  } else {
    const int slash =
        text_rfind_index(SavedProfile_selectedPath, text_character('/'));
    display_print(&(tft), ((slash >= 0 ? text_slice(SavedProfile_selectedPath,
                                                    slash + 1, SIZE_MAX)
                                       : SavedProfile_selectedPath))
                              .data);
  }

  if (SavedProfile_deleteArmed &&
      (int32_t)(sys_monotonic_ms() - SavedProfile_deleteArmUntilMs) < 0) {
    int hintY = detailsY + (SavedProfile_PROFILE_LINE_STEP * 4);
    if (hintY >= SavedProfile_profileBottomY() - 12) {
      hintY = SavedProfile_profileBottomY() - 12;
    }
    display_set_cursor(&(tft), SavedProfile_PROFILE_LABEL_X, hintY);
    display_set_text_color_2(&(tft), UI_WARN, TFT_BLACK);
    display_print(&(tft), "Press Delete again to confirm");
  }
}

static void SavedProfile_updateSelectionUI(uint16_t oldIndex,
                                           bool forceListRedraw) {
  if (SavedProfile_sdTotalProfiles == 0) return;
  uint16_t oldPage = SavedProfile_pageStartForIndex(oldIndex);
  uint16_t newPage =
      SavedProfile_pageStartForIndex(SavedProfile_currentProfileIndex);

  SavedProfile_drawHeaderLine();

  if (forceListRedraw || oldPage != newPage) {
    SavedProfile_drawListPage(newPage);
  } else {
    uint8_t oldRow = (uint8_t)(oldIndex - oldPage);
    uint8_t newRow = (uint8_t)(SavedProfile_currentProfileIndex - newPage);
    SavedProfile_ensurePageCache();

    SavedProfile_drawRow(newPage, oldRow);
    SavedProfile_drawRow(newPage, newRow);
  }

  SavedProfile_drawDetails();
}

void SavedProfile_updateDisplay(void) {
  const int bodyH = subghzContentBottom() - 40;
  if (bodyH > 0) {
    display_fill_rect(&(tft), 0, 40, 240, bodyH, TFT_BLACK);
  }

  if (SavedProfile_sdTotalProfiles == 0) {
    display_set_text_size(&(tft), 1);
    display_set_cursor(
        &(tft), SavedProfile_PROFILE_LABEL_X,
        SavedProfile_PROFILE_HEADER_Y + SavedProfile_PROFILE_LINE_H);
    display_set_text_color_2(&(tft), UI_TEXT, TFT_BLACK);
    if (text_find_index(SavedProfile_sdLastErr, TEXT("SD not mounted"), 0) >=
        0) {
      display_print(&(tft), "SD card not inserted.");
    } else {
      display_print(&(tft), "No profiles on storage::");
    }
    if (strlen((SavedProfile_sdLastErr).data)) {
      display_set_cursor(
          &(tft), SavedProfile_PROFILE_LABEL_X,
          SavedProfile_PROFILE_HEADER_Y + (SavedProfile_PROFILE_LINE_H * 2));
      display_set_text_color_2(&(tft), UI_DIM_TEXT, TFT_BLACK);
      display_print(&(tft), (SavedProfile_sdLastErr).data);
    }
    return;
  }

  SavedProfile_drawHeaderLine();
  SavedProfile_drawListPage(
      SavedProfile_pageStartForIndex(SavedProfile_currentProfileIndex));
  SavedProfile_drawDetails();
}

void SavedProfile_transmitProfile(int index) {
  (void)index;
  text_t err = {0};
  SavedProfile_loadSelectedFromSd(&err);
  if (!SavedProfile_selectedValid) return;
  SavedProfile_Profile profileToSend = SavedProfile_selectedProfile;

  cc1101_set_sidle(&(cc1101));
  cc1101_set_mhz(&(cc1101), profileToSend.frequency / 1000000.0);

  ook_radio_disable_receive(&(SavedProfile_mySwitch));
  sys_sleep_ms(100);
  ook_radio_enable_transmit(&(SavedProfile_mySwitch), SUBGHZ_TX_PIN);
  cc1101_set_tx_0(&(cc1101));

  SavedProfile_profileClearContentArea(TFT_BLACK);
  display_set_cursor(&(tft), 10, 30 + SavedProfile_yshift);
  display_set_text_color_1(&(tft), TFT_WHITE);
  display_print(&(tft), "Sending ");
  display_print(&(tft), (TEXT(profileToSend.name)).data);
  display_print(&(tft), "...");
  display_set_cursor(&(tft), 10, 50 + SavedProfile_yshift);
  display_print(&(tft), "Value: ");
  display_print(&(tft), (TEXT(profileToSend.value)).data);

  ook_radio_set_protocol(&(SavedProfile_mySwitch), profileToSend.protocol);
  ook_radio_send(&(SavedProfile_mySwitch), profileToSend.value,
                 profileToSend.bitLength);

  sys_sleep_ms(500);
  SavedProfile_profileClearContentArea(TFT_BLACK);
  display_set_cursor(&(tft), 10, 30 + SavedProfile_yshift);
  display_print(&(tft), "Done!");

  cc1101_set_rx_0(&(cc1101));
  ook_radio_disable_transmit(&(SavedProfile_mySwitch));
  sys_sleep_ms(100);
  ook_radio_enable_receive(&(SavedProfile_mySwitch), SUBGHZ_RX_PIN);

  sys_sleep_ms(500);
  SavedProfile_profileRestoreChrome();
}

void SavedProfile_loadProfileCount(void) { SavedProfile_refreshSdIndex(true); }

void SavedProfile_printProfiles(void) {
  div_console_line(TEXT("Saved Profiles (SD index):"));
  text_t err = {0};
  SavedProfile_refreshSdIndex(false);
  printf("Total profiles: %d\n", (int)SavedProfile_sdTotalProfiles);
  if (!SavedProfile_sdTotalProfiles) return;

  uint16_t n =
      SavedProfile_sdTotalProfiles > 10 ? 10 : SavedProfile_sdTotalProfiles;
  for (uint16_t i = 0; i < n; i++) {
    text_t pth = {0};
    uint16_t li = 0;
    if (!locateGlobalIndex(&(SavedProfile_sdFiles), i, &(pth), &(li))) continue;
    SubGhzProfile p = SubGhzProfile_INITIALIZER;
    if (!readProfileAt(pth, li, &(p), &err)) continue;
    printf("  [%d] %s @ %.2f MHz (val=%lu)\n", (int)i, p.name,
           p.frequency / 1000000.0, (unsigned long)p.value);
  }
}

void SavedProfile_deleteProfile(int index) {
  (void)index;
  if (SavedProfile_sdTotalProfiles == 0) return;
  text_t err = {0};
  SavedProfile_loadSelectedFromSd(&err);
  if (!SavedProfile_selectedValid) return;

  text_t path = SavedProfile_selectedPath;
  uint16_t local = SavedProfile_selectedLocalIdx;

  uint32_t now = sys_monotonic_ms();
  if (!SavedProfile_deleteArmed ||
      (int32_t)(now - SavedProfile_deleteArmUntilMs) >= 0) {
    SavedProfile_deleteArmed = true;
    SavedProfile_deleteArmUntilMs = now + 3000;
    SavedProfile_updateDisplay();
    return;
  }
  SavedProfile_deleteArmed = false;

  if (!deleteProfileFromFile(path, local, &err)) {
    SavedProfile_profileClearContentArea(TFT_BLACK);
    display_set_cursor(&(tft), 10, 30 + SavedProfile_yshift);
    display_set_text_color_1(&(tft), UI_WARN);
    display_print(&(tft), "Delete FAILED");
    display_set_cursor(&(tft), 10, 45 + SavedProfile_yshift);
    display_set_text_color_1(&(tft), TFT_WHITE);
    display_print(&(tft), (err).data);
    sys_sleep_ms(1200);
    SavedProfile_profileRestoreChrome();
    return;
  }

  SavedProfile_refreshSdIndex(false);
  if (SavedProfile_sdTotalProfiles == 0)
    SavedProfile_currentProfileIndex = 0;
  else if (SavedProfile_currentProfileIndex >= SavedProfile_sdTotalProfiles)
    SavedProfile_currentProfileIndex =
        (uint16_t)(SavedProfile_sdTotalProfiles - 1);
  SavedProfile_selectedValid = false;
  SavedProfile_cacheDirty = true;
  SavedProfile_updateDisplay();
}

void SavedProfile_runUI(void) {
#undef STATUS_BAR_Y_OFFSET
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#undef ICON_NUM
#define ICON_NUM 4

  static int iconX[ICON_NUM] = {130, 170, 210, 10};
  static int iconY = STATUS_BAR_Y_OFFSET;

  static const unsigned char* icons[ICON_NUM] = {
      bitmap_icon_antenna, bitmap_icon_recycle, bitmap_icon_undo,
      bitmap_icon_go_back};

  if (!SavedProfile_uiDrawn) {
    display_fill_rect(&(tft), 0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH,
                      STATUS_BAR_HEIGHT, DARK_GRAY);

    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        display_draw_bitmap(&(tft), iconX[i], iconY, icons[i], ICON_SIZE,
                            ICON_SIZE, UI_ICON);
      }
    }
    display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
    display_draw_fast_h_line(&(tft), 0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT,
                             240, UI_LINE);
    SavedProfile_uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  if (animationState > 0 && sys_monotonic_ms() - lastAnimationTime >= 150) {
    if (animationState == 1) {
      display_draw_bitmap(&(tft), iconX[activeIcon], iconY, icons[activeIcon],
                          ICON_SIZE, ICON_SIZE, UI_ICON);
      animationState = 2;

      switch (activeIcon) {
        case 0:
          if (SavedProfile_sdTotalProfiles > 0) {
            SavedProfile_transmitProfile(SavedProfile_currentProfileIndex);
          }
          break;
        case 1:
          if (SavedProfile_sdTotalProfiles > 0) {
            SavedProfile_deleteProfile(SavedProfile_currentProfileIndex);
          }
          break;
        case 2: {
          SavedProfile_refreshSdIndex(true);
          SavedProfile_selectedValid = false;
          SavedProfile_cacheDirty = true;
          SavedProfile_deleteArmed = false;
          SavedProfile_updateDisplay();
          break;
        }
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
      if (y >= SavedProfile_LIST_Y &&
          y < (SavedProfile_LIST_Y +
               (SavedProfile_ITEMS_PER_PAGE * SavedProfile_ROW_H)) &&
          x >= SavedProfile_LIST_X &&
          x < (SavedProfile_LIST_X + SavedProfile_LIST_W)) {
        uint8_t row = (uint8_t)((y - SavedProfile_LIST_Y) / SavedProfile_ROW_H);
        uint16_t oldIdx = SavedProfile_currentProfileIndex;
        uint16_t start =
            SavedProfile_pageStartForIndex(SavedProfile_currentProfileIndex);
        uint16_t idx = (uint16_t)(start + row);
        if (idx < SavedProfile_sdTotalProfiles) {
          SavedProfile_currentProfileIndex = idx;
          SavedProfile_selectedValid = false;
          SavedProfile_cacheDirty = true;
          SavedProfile_deleteArmed = false;
          SavedProfile_updateSelectionUI(oldIdx, false);
        }
      }
      if (y > STATUS_BAR_Y_OFFSET &&
          y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x > iconX[i] && x < iconX[i] + ICON_SIZE) {
            if (icons[i] != NULL && animationState == 0) {
              if (i == 3) {
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

void SavedProfile_saveSetup(void) {
  div_console_open(115200);
  setTouchButtonInputEnabled(true);
  subghzSetProfileNavLabels();

  cc1101_set_spi_pin(&(cc1101), CC1101_SCK, CC1101_MISO, CC1101_MOSI,
                     CC1101_CS);

  SavedProfile_loadProfileCount();
  SavedProfile_printProfiles();

#if HAS_PCF8574_BUTTONS
  pcf8574_configure_pin(button_input_device(), BTN_UP, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_DOWN, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_LEFT, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_RIGHT, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_SELECT, GPIO_MODE_INPUT);
#endif

  subghzClearBody(TFT_BLACK);
  display_set_text_color_1(&(tft), UI_TEXT);

  setupTouchscreen();

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true, false);
  subghzRedrawNavChrome();
  SavedProfile_uiDrawn = false;

  cc1101_init(&(cc1101));
  cc1101_set_gdo(&(cc1101), CC1101_GDO0, CC1101_GDO2);
  cc1101_set_rx_0(&(cc1101));

  ook_radio_enable_receive(&(SavedProfile_mySwitch), SUBGHZ_RX_PIN);
  ook_radio_enable_transmit(&(SavedProfile_mySwitch), SUBGHZ_TX_PIN);
  ook_radio_set_repeat_transmit(&(SavedProfile_mySwitch), 8);

  SavedProfile_refreshSdIndex(false);
  SavedProfile_cacheDirty = true;
  SavedProfile_deleteArmed = false;
  SavedProfile_updateDisplay();
  SavedProfile_uiDrawn = false;
  SavedProfile_runUI();
  subghzRedrawNavChrome();
}

void SavedProfile_saveLoop(void) {
  if (feature_active &&
      (feature_exit_requested || featureExitButtonPressed())) {
    feature_exit_requested = true;
    return;
  }

  maintainTouchNavBar();
  SavedProfile_runUI();
  SavedProfile_profileHandleNavButtons();

  static unsigned long lastDebounceTime = 0;
  const unsigned long debounceDelay = 200;

  static bool prevUp = false;
  static bool prevDown = false;
  static bool prevRight = false;
  static bool prevLeft = false;
  const bool prevPressed = isPhysicalButtonPressed(BTN_UP);
  const bool nextPressed = isPhysicalButtonPressed(BTN_DOWN);
  const bool txPressed = isPhysicalButtonPressed(BTN_RIGHT);
  const bool deletePressed = isPhysicalButtonPressed(BTN_LEFT);

  if (SavedProfile_sdTotalProfiles > 0) {
    if (nextPressed && !prevDown &&
        sys_monotonic_ms() - lastDebounceTime > debounceDelay) {
      SavedProfile_profileSelectNext();
      lastDebounceTime = sys_monotonic_ms();
    }

    if (prevPressed && !prevUp &&
        sys_monotonic_ms() - lastDebounceTime > debounceDelay) {
      SavedProfile_profileSelectPrev();
      lastDebounceTime = sys_monotonic_ms();
    }

    if (txPressed && !prevRight &&
        sys_monotonic_ms() - lastDebounceTime > debounceDelay) {
      SavedProfile_transmitProfile(SavedProfile_currentProfileIndex);
      lastDebounceTime = sys_monotonic_ms();
    }

    if (deletePressed && !prevLeft &&
        sys_monotonic_ms() - lastDebounceTime > debounceDelay) {
      SavedProfile_deleteProfile(SavedProfile_currentProfileIndex);
      lastDebounceTime = sys_monotonic_ms();
    }
  }

  prevUp = prevPressed;
  prevDown = nextPressed;
  prevRight = txPressed;
  prevLeft = deletePressed;
}

void subjammer_updateDisplay(void);

static bool subjammer_uiDrawn = false;

static unsigned long subjammer_lastDebounceTime = 0;
const unsigned long subjammer_debounceDelay = 200;

#define SCREEN_WIDTH 240
#undef SCREEN_HEIGHT
#define SCREEN_HEIGHT 64

enum { subjammer_JAM_BTN_LEFT = 4 };
enum { subjammer_JAM_BTN_RIGHT = 5 };
enum { subjammer_JAM_BTN_DOWN = 3 };
enum { subjammer_JAM_BTN_UP = 6 };

bool subjammer_jammingRunning = false;
bool subjammer_continuousMode = true;
bool subjammer_autoMode = false;
unsigned long subjammer_lastSweepTime = 0;
const unsigned long subjammer_sweepInterval = 1000;

static const uint32_t subjammer_subghz_frequency_list[] = {
    300000000, 303875000, 304250000, 310000000, 314000000, 315000000,
    318000000, 390000000, 418000000, 433075000, 433420000, 433920000,
    434420000, 434775000, 438900000, 868350000, 915000000, 925000000};
const int subjammer_numFrequencies = sizeof(subjammer_subghz_frequency_list) /
                                     sizeof(subjammer_subghz_frequency_list[0]);
int subjammer_currentFrequencyIndex = 5;
float subjammer_targetFrequency = 315.0f;

enum { subjammer_kJammerStatusLineY = 79 };
enum { subjammer_kJammerYSHIFT = 20 };
enum { subjammer_kJammerValueLineH = 11 };
enum { subjammer_kJammerProgressY = 60 + subjammer_kJammerYSHIFT };

static bool subjammer_s_jammerStaticDrawn = false;

typedef struct subjammer_JammerDisplayCache {
  bool valid;
  int freqMHz100;
  bool autoMode;
  bool continuousMode;
  bool jammingRunning;
  int progress;
  bool blinkOn;
} subjammer_JammerDisplayCache;
#define subjammer_JammerDisplayCache_INITIALIZER \
  {.valid = false,                               \
   .freqMHz100 = -1,                             \
   .autoMode = false,                            \
   .continuousMode = false,                      \
   .jammingRunning = false,                      \
   .progress = -1,                               \
   .blinkOn = false}
;

static subjammer_JammerDisplayCache subjammer_s_jammerDisp =
    subjammer_JammerDisplayCache_INITIALIZER;

static void subjammer_jammerInvalidateDisplay(void) {
  subjammer_s_jammerStaticDrawn = false;
  subjammer_s_jammerDisp =
      ((subjammer_JammerDisplayCache)subjammer_JammerDisplayCache_INITIALIZER);
}

static void subjammer_subjammerToggleJam(void) {
  subjammer_jammingRunning = !subjammer_jammingRunning;
  if (subjammer_jammingRunning) {
    div_console_line(TEXT("Jamming started"));
    cc1101_set_mhz(&(cc1101), subjammer_targetFrequency);
    cc1101_set_tx_0(&(cc1101));
  } else {
    div_console_line(TEXT("Jamming stopped"));
    cc1101_set_sidle(&(cc1101));
    gpioOutputWrite(TX_PIN, false);
  }
  subjammer_updateDisplay();
  subjammer_lastDebounceTime = sys_monotonic_ms();
}

static void subjammer_subjammerFreqNext(void) {
  if (subjammer_autoMode) {
    return;
  }
  subjammer_currentFrequencyIndex =
      (subjammer_currentFrequencyIndex + 1) % subjammer_numFrequencies;
  subjammer_targetFrequency =
      subjammer_subghz_frequency_list[subjammer_currentFrequencyIndex] /
      1000000.0;
  cc1101_set_mhz(&(cc1101), subjammer_targetFrequency);
  subjammer_updateDisplay();
  subjammer_lastDebounceTime = sys_monotonic_ms();
}

static void subjammer_subjammerFreqPrev(void) {
  if (subjammer_autoMode) {
    return;
  }
  subjammer_currentFrequencyIndex =
      (subjammer_currentFrequencyIndex - 1 + subjammer_numFrequencies) %
      subjammer_numFrequencies;
  subjammer_targetFrequency =
      subjammer_subghz_frequency_list[subjammer_currentFrequencyIndex] /
      1000000.0;
  cc1101_set_mhz(&(cc1101), subjammer_targetFrequency);
  subjammer_updateDisplay();
  subjammer_lastDebounceTime = sys_monotonic_ms();
}

static void subjammer_subjammerApplyFrequency(void) {
  cc1101_set_mhz(&(cc1101), subjammer_targetFrequency);
  if (subjammer_jammingRunning) {
    cc1101_set_tx_0(&(cc1101));
  } else {
    cc1101_set_sidle(&(cc1101));
    gpioOutputWrite(TX_PIN, false);
  }
}

static void subjammer_subjammerAutoSweepIfDue(void) {
  if (!subjammer_autoMode ||
      sys_monotonic_ms() - subjammer_lastSweepTime < subjammer_sweepInterval) {
    return;
  }

  subjammer_currentFrequencyIndex =
      (subjammer_currentFrequencyIndex + 1) % subjammer_numFrequencies;
  subjammer_targetFrequency =
      subjammer_subghz_frequency_list[subjammer_currentFrequencyIndex] /
      1000000.0;
  subjammer_subjammerApplyFrequency();
  subjammer_updateDisplay();
  subjammer_lastSweepTime = sys_monotonic_ms();
}

static void subjammer_subjammerToggleAuto(void) {
  subjammer_autoMode = !subjammer_autoMode;
  div_console_write(TEXT("Frequency mode: "));
  div_console_line(TEXT(subjammer_autoMode ? "Automatic" : "Manual"));
  if (subjammer_autoMode) {
    subjammer_currentFrequencyIndex = 0;
    subjammer_targetFrequency =
        subjammer_subghz_frequency_list[subjammer_currentFrequencyIndex] /
        1000000.0;
    subjammer_lastSweepTime = sys_monotonic_ms();
    subjammer_subjammerApplyFrequency();
    subjammer_s_jammerDisp.freqMHz100 = -1;
  }
  subjammer_updateDisplay();
  subjammer_lastDebounceTime = sys_monotonic_ms();
}

void subjammer_subjammerHandleNavButtons(void) {
  if (!featureHasTouchNavBar()) {
    return;
  }

  if (isTouchNavButtonPressedEdge(BTN_UP)) {
    subjammer_subjammerToggleJam();
    subghzWaitNavRelease(BTN_UP);
  }
  if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
    subjammer_subjammerFreqPrev();
    subghzWaitNavRelease(BTN_LEFT);
  }
  if (isTouchNavButtonPressedEdge(BTN_RIGHT)) {
    subjammer_subjammerFreqNext();
    subghzWaitNavRelease(BTN_RIGHT);
  }
  if (isTouchNavButtonPressedEdge(BTN_DOWN)) {
    subjammer_subjammerToggleAuto();
    subghzWaitNavRelease(BTN_DOWN);
  }
}

static void subjammer_jammerDrawStatusSeparator(void) {
  display_draw_fast_h_line(&(tft), 0, subjammer_kJammerStatusLineY, 240,
                           UI_LINE);
}

static void subjammer_jammerDrawValueCell(int x, int y, int w, int h,
                                          text_t text, uint16_t color) {
  const int maxH = subjammer_kJammerStatusLineY - y;
  if (maxH <= 0) {
    return;
  }
  const int clipH = DIV_MIN(h, maxH);
  display_fill_rect(&(tft), x, y, w, clipH, TFT_BLACK);
  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), color, TFT_BLACK);
  display_set_cursor(&(tft), x, y);
  display_print(&(tft), (text).data);
}

static void subjammer_jammerDrawStaticChrome(void) {
  if (subjammer_s_jammerStaticDrawn) {
    return;
  }

  const int bodyBottom = subghzContentBottom();
  const int bodyH = DIV_MIN(subjammer_kJammerStatusLineY - 40, bodyBottom - 40);
  if (bodyH > 0) {
    display_fill_rect(&(tft), 0, 40, 240, bodyH, TFT_BLACK);
  }
  subjammer_jammerDrawStatusSeparator();

  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), UI_TEXT, TFT_BLACK);
  display_set_cursor(&(tft), 5, 22 + subjammer_kJammerYSHIFT);
  display_print(&(tft), "Freq:");
  display_set_cursor(&(tft), 130, 22 + subjammer_kJammerYSHIFT);
  display_print(&(tft), "Mode:");
  display_set_cursor(&(tft), 5, 42 + subjammer_kJammerYSHIFT);
  display_print(&(tft), "Status:");

  subjammer_s_jammerStaticDrawn = true;
}

static void subjammer_jammerDrawProgressBar(int progress) {
  display_fill_rect(&(tft), 0, subjammer_kJammerProgressY, 240, 4, TFT_BLACK);
  if (progress > 0) {
    display_fill_rect(&(tft), 0, subjammer_kJammerProgressY, progress, 4,
                      UI_WARN);
  }
}

static void subjammer_jammerDrawBlinkDot(bool on) {
  enum { cx = 220 };
  const int cy = 22 + subjammer_kJammerYSHIFT;
  enum { r = 2 };
  if (on) {
    display_fill_circle(&(tft), cx, cy, r, UI_WARN);
  } else {
    display_fill_rect(&(tft), cx - r, cy - r, r * 2 + 1, r * 2 + 1, TFT_BLACK);
  }
}

static void subjammer_jammerPollBlinkIndicator(void) {
  const bool wantBlink = subjammer_autoMode && subjammer_jammingRunning;
  const bool blinkOn = wantBlink && ((sys_monotonic_ms() % 1000) < 500);
  if (!subjammer_s_jammerDisp.valid) {
    return;
  }
  if (blinkOn != subjammer_s_jammerDisp.blinkOn) {
    subjammer_jammerDrawBlinkDot(blinkOn);
    subjammer_s_jammerDisp.blinkOn = blinkOn;
  }
}

void subjammer_updateDisplay(void) {
  subjammer_jammerDrawStaticChrome();

  char freqBuf[20];
  char modeBuf[8];
  char statusBuf[8];

  if (subjammer_autoMode) {
    snprintf(freqBuf, sizeof(freqBuf), "Auto:%.1f", subjammer_targetFrequency);
  } else {
    snprintf(freqBuf, sizeof(freqBuf), "%.2f MHz", subjammer_targetFrequency);
  }
  div_copy_string(modeBuf, sizeof(modeBuf),
                  subjammer_continuousMode ? "Cont" : "Noise");
  div_copy_string(statusBuf, sizeof(statusBuf),
                  subjammer_jammingRunning ? "Jamming" : "Idle   ");

  const int freqKey = (int)(subjammer_targetFrequency * 100.0f + 0.5f);
  const bool fullRedraw = !subjammer_s_jammerDisp.valid;
  if (fullRedraw || subjammer_s_jammerDisp.freqMHz100 != freqKey ||
      subjammer_s_jammerDisp.autoMode != subjammer_autoMode) {
    subjammer_jammerDrawValueCell(40, 22 + subjammer_kJammerYSHIFT, 96,
                                  subjammer_kJammerValueLineH, TEXT(freqBuf),
                                  subjammer_autoMode ? UI_WARN : UI_TEXT);
    subjammer_s_jammerDisp.freqMHz100 = freqKey;
    subjammer_s_jammerDisp.autoMode = subjammer_autoMode;
  }

  if (fullRedraw ||
      subjammer_s_jammerDisp.continuousMode != subjammer_continuousMode) {
    subjammer_jammerDrawValueCell(165, 22 + subjammer_kJammerYSHIFT, 40,
                                  subjammer_kJammerValueLineH, TEXT(modeBuf),
                                  subjammer_continuousMode ? UI_WARN : UI_TEXT);
    subjammer_s_jammerDisp.continuousMode = subjammer_continuousMode;
  }

  if (fullRedraw ||
      subjammer_s_jammerDisp.jammingRunning != subjammer_jammingRunning) {
    subjammer_jammerDrawValueCell(50, 42 + subjammer_kJammerYSHIFT, 72,
                                  subjammer_kJammerValueLineH, TEXT(statusBuf),
                                  subjammer_jammingRunning ? UI_WARN : UI_TEXT);
    subjammer_s_jammerDisp.jammingRunning = subjammer_jammingRunning;
  }

  if (subjammer_autoMode) {
    const int progress = sys_scale(subjammer_currentFrequencyIndex, 0,
                                   subjammer_numFrequencies - 1, 0, 240);
    if (fullRedraw || subjammer_s_jammerDisp.progress != progress) {
      subjammer_jammerDrawProgressBar(progress);
      subjammer_s_jammerDisp.progress = progress;
    }
  } else if (subjammer_s_jammerDisp.progress != -1) {
    subjammer_jammerDrawProgressBar(0);
    subjammer_s_jammerDisp.progress = -1;
    if (subjammer_s_jammerDisp.blinkOn) {
      subjammer_jammerDrawBlinkDot(false);
      subjammer_s_jammerDisp.blinkOn = false;
    }
  }

  subjammer_jammerDrawStatusSeparator();
  subjammer_s_jammerDisp.valid = true;
}

void subjammer_runUI(void) {
#define SCREEN_WIDTH 240
#define SCREENHEIGHT 320
#undef STATUS_BAR_Y_OFFSET
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#undef ICON_NUM
#define ICON_NUM 6

  static int iconX[ICON_NUM] = {50, 90, 130, 170, 210, 10};
  static int iconY = STATUS_BAR_Y_OFFSET;

  static const unsigned char* icons[ICON_NUM] = {
      bitmap_icon_power,        bitmap_icon_antenna,
      bitmap_icon_random,       bitmap_icon_sort_down_minus,
      bitmap_icon_sort_up_plus, bitmap_icon_go_back};

  if (!subjammer_uiDrawn) {
    display_fill_rect(&(tft), 0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH,
                      STATUS_BAR_HEIGHT, DARK_GRAY);

    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        display_draw_bitmap(&(tft), iconX[i], iconY, icons[i], ICON_SIZE,
                            ICON_SIZE, UI_ICON);
      }
    }
    display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
    display_draw_fast_h_line(&(tft), 0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT,
                             240, UI_LINE);
    subjammer_uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  if (animationState > 0 && sys_monotonic_ms() - lastAnimationTime >= 150) {
    if (animationState == 1) {
      display_draw_bitmap(&(tft), iconX[activeIcon], iconY, icons[activeIcon],
                          ICON_SIZE, ICON_SIZE, UI_ICON);
      animationState = 2;

      switch (activeIcon) {
        case 0:
          subjammer_jammingRunning = !subjammer_jammingRunning;
          if (subjammer_jammingRunning) {
            div_console_line(TEXT("Jamming started"));
            cc1101_set_mhz(&(cc1101), subjammer_targetFrequency);
            cc1101_set_tx_0(&(cc1101));
          } else {
            div_console_line(TEXT("Jamming stopped"));
            cc1101_set_sidle(&(cc1101));
            gpioOutputWrite(TX_PIN, false);
          }
          subjammer_updateDisplay();
          subjammer_lastDebounceTime = sys_monotonic_ms();
          break;
        case 1:
          subjammer_continuousMode = !subjammer_continuousMode;
          div_console_write(TEXT("Jamming mode: "));
          div_console_line(
              TEXT(subjammer_continuousMode ? "Continuous Carrier" : "Noise"));
          subjammer_updateDisplay();
          subjammer_lastDebounceTime = sys_monotonic_ms();
          break;
        case 2:
          subjammer_autoMode = !subjammer_autoMode;
          div_console_write(TEXT("Frequency mode: "));
          div_console_line(TEXT(subjammer_autoMode ? "Automatic" : "Manual"));
          if (subjammer_autoMode) {
            subjammer_currentFrequencyIndex = 0;
            subjammer_targetFrequency = subjammer_subghz_frequency_list
                                            [subjammer_currentFrequencyIndex] /
                                        1000000.0;
            subjammer_lastSweepTime = sys_monotonic_ms();
            subjammer_subjammerApplyFrequency();
            subjammer_s_jammerDisp.freqMHz100 = -1;
          }
          subjammer_updateDisplay();
          subjammer_lastDebounceTime = sys_monotonic_ms();
          break;
        case 3:
          subjammer_currentFrequencyIndex =
              (subjammer_currentFrequencyIndex - 1 + subjammer_numFrequencies) %
              subjammer_numFrequencies;
          subjammer_targetFrequency =
              subjammer_subghz_frequency_list[subjammer_currentFrequencyIndex] /
              1000000.0;
          cc1101_set_mhz(&(cc1101), subjammer_targetFrequency);
          div_console_write(TEXT("Switched to: "));
          div_console_write(TEXT(subjammer_targetFrequency));
          div_console_line(TEXT(" MHz"));
          subjammer_updateDisplay();
          subjammer_lastDebounceTime = sys_monotonic_ms();
          break;
        case 4:
          subjammer_currentFrequencyIndex =
              (subjammer_currentFrequencyIndex + 1) % subjammer_numFrequencies;
          subjammer_targetFrequency =
              subjammer_subghz_frequency_list[subjammer_currentFrequencyIndex] /
              1000000.0;
          cc1101_set_mhz(&(cc1101), subjammer_targetFrequency);
          div_console_write(TEXT("Switched to: "));
          div_console_write(TEXT(subjammer_targetFrequency));
          div_console_line(TEXT(" MHz"));
          subjammer_updateDisplay();
          subjammer_lastDebounceTime = sys_monotonic_ms();
          break;
        case 5:
          feature_exit_requested = true;
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

void subjammer_subjammerSetup(void) {
  div_console_open(115200);
  setTouchButtonInputEnabled(true);
  subghzSetJammerNavLabels();
  subghzClearBody(TFT_BLACK);
  drawStatusBar(readBatteryVoltage(), true, false);
  subghzRedrawNavChrome();

  cc1101_set_spi_pin(&(cc1101), CC1101_SCK, CC1101_MISO, CC1101_MOSI,
                     CC1101_CS);

  cc1101_init(&(cc1101));
  cc1101_set_modulation(&(cc1101), 0);
  cc1101_set_rx_bw(&(cc1101), 500.0);
  cc1101_set_pa(&(cc1101), 12);
  cc1101_set_mhz(&(cc1101), subjammer_targetFrequency);
  cc1101_set_tx_0(&(cc1101));

#if HAS_PCF8574_BUTTONS
  pcf8574_configure_pin(button_input_device(), BTN_LEFT, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_RIGHT, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_DOWN, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_UP, GPIO_MODE_INPUT);
#endif
  sys_sleep_ms(100);

  subghzClearBody(TFT_BLACK);
  drawStatusBar(readBatteryVoltage(), true, false);

  setupTouchscreen();

  subjammer_jammerInvalidateDisplay();
  subjammer_updateDisplay();
  subjammer_uiDrawn = false;
  subghzRedrawNavChrome();
}

void subjammer_subjammerLoop(void) {
  if (feature_active &&
      (feature_exit_requested || featureExitButtonPressed())) {
    feature_exit_requested = true;
    return;
  }

  maintainTouchNavBar();
  subjammer_runUI();
  if (subjammer_uiDrawn) {
    display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
    display_draw_fast_h_line(&(tft), 0, 36, 240, UI_LINE);
    if (subjammer_s_jammerDisp.valid) {
      subjammer_jammerDrawStatusSeparator();
    }
  }
  subjammer_jammerPollBlinkIndicator();
  subjammer_subjammerHandleNavButtons();

#if HAS_PCF8574_BUTTONS
  int btnLeftState =
      pcf8574_read_pin(button_input_device(), subjammer_JAM_BTN_LEFT);
  int btnRightState =
      pcf8574_read_pin(button_input_device(), subjammer_JAM_BTN_RIGHT);
  int btnUpState =
      pcf8574_read_pin(button_input_device(), subjammer_JAM_BTN_UP);
  int btnDownState =
      pcf8574_read_pin(button_input_device(), subjammer_JAM_BTN_DOWN);
#else
  int btnLeftState = isPhysicalButtonPressed(BTN_LEFT) ? 0 : 1;
  int btnRightState = isPhysicalButtonPressed(BTN_RIGHT) ? 0 : 1;
  int btnUpState = isPhysicalButtonPressed(BTN_UP) ? 0 : 1;
  int btnDownState = isPhysicalButtonPressed(BTN_DOWN) ? 0 : 1;
#endif

  if (btnUpState == 0 && sys_monotonic_ms() - subjammer_lastDebounceTime >
                             subjammer_debounceDelay) {
    subjammer_subjammerToggleJam();
  }

  if (btnRightState == 0 && !subjammer_autoMode &&
      sys_monotonic_ms() - subjammer_lastDebounceTime >
          subjammer_debounceDelay) {
    subjammer_subjammerFreqNext();
  }

  if (btnLeftState == 0 && !subjammer_autoMode &&
      sys_monotonic_ms() - subjammer_lastDebounceTime >
          subjammer_debounceDelay) {
    subjammer_subjammerFreqPrev();
  }

  if (btnDownState == 0 && sys_monotonic_ms() - subjammer_lastDebounceTime >
                               subjammer_debounceDelay) {
    subjammer_subjammerToggleAuto();
  }

  subjammer_subjammerAutoSweepIfDue();

  if (subjammer_jammingRunning) {
    cc1101_set_tx_0(&(cc1101));

    if (subjammer_continuousMode) {
      cc1101_spi_write_reg(&(cc1101), CC1101_TXFIFO, 0xFF);
      cc1101_spi_strobe(&(cc1101), CC1101_STX);
      gpioOutputWrite(TX_PIN, true);
    } else {
      for (int i = 0; i < 10; i++) {
        uint32_t noise = sys_random_range_1(16777216);
        cc1101_spi_write_reg(&(cc1101), CC1101_TXFIFO, noise >> 16);
        cc1101_spi_write_reg(&(cc1101), CC1101_TXFIFO, (noise >> 8) & 0xFF);
        cc1101_spi_write_reg(&(cc1101), CC1101_TXFIFO, noise & 0xFF);
        cc1101_spi_strobe(&(cc1101), CC1101_STX);
        esp_rom_delay_us(50);
      }
    }
  }
}

static void SubBrute_subghzSetBruteNavLabels(void) {
  setTouchNavLabels("Freq-", "Proto", "Exit", "Start", "Freq+");
}

enum { SubBrute_BRUTE_TX_PIN = SUBGHZ_TX_PIN };

static const uint32_t SubBrute_kBruteFreqList[] = {
    300000000, 303875000, 304250000, 310000000, 314000000, 315000000,
    318000000, 390000000, 418000000, 433075000, 433420000, 433920000,
    434420000, 434775000, 438900000, 868350000, 915000000, 925000000};
enum {
  SubBrute_kBruteFreqCount = (int)(sizeof(SubBrute_kBruteFreqList) /
                                   sizeof(SubBrute_kBruteFreqList[0]))
};

static const uint8_t SubBrute_kBitsChoices[] = {8, 10, 12, 16, 18, 20, 24};
enum {
  SubBrute_kBitsChoiceCount =
      (int)(sizeof(SubBrute_kBitsChoices) / sizeof(SubBrute_kBitsChoices[0]))
};

static const uint16_t SubBrute_kPulseChoicesUs[] = {250, 350, 400, 500};
enum {
  SubBrute_kPulseChoiceCount = (int)(sizeof(SubBrute_kPulseChoicesUs) /
                                     sizeof(SubBrute_kPulseChoicesUs[0]))
};

enum { SubBrute_kBarBottom = 36 };
enum { SubBrute_kPanelTop = 44 };
enum { SubBrute_kBoxHeaderH = 14 };
enum { SubBrute_kLineH = 14 };
enum { SubBrute_kPanelPadX = 4 };
enum { SubBrute_kPanelW = 240 - (SubBrute_kPanelPadX * 2) };
enum { SubBrute_kLabelX = 12 };
enum { SubBrute_kValueX = 58 };
enum { SubBrute_kValueW = 170 };
enum { SubBrute_kSettingsRows = 4 };
enum {
  SubBrute_kSettingsInnerH = SubBrute_kBoxHeaderH +
  (SubBrute_kSettingsRows * SubBrute_kLineH) + 4
};
enum {
  SubBrute_kProgressInnerH = SubBrute_kBoxHeaderH + (SubBrute_kLineH * 2) + 16
};
enum { SubBrute_kRadius = 3 };

typedef uint8_t SubBrute_FocusRow;
enum {
  SubBrute_FOCUS_FREQ = 0,
  SubBrute_FOCUS_BITS,
  SubBrute_FOCUS_MODE,
  SubBrute_FOCUS_OPT,
  SubBrute_FOCUS_COUNT
};
typedef uint8_t SubBrute_RunMode;
enum { SubBrute_MODE_DEBRUIJN = 0, SubBrute_MODE_BRUTE = 1 };
typedef uint8_t SubBrute_RunState;
enum {
  SubBrute_ST_IDLE = 0,
  SubBrute_ST_RUNNING,
  SubBrute_ST_DONE,
  SubBrute_ST_STOPPED
};

static ook_radio_t SubBrute_s_switch = OOK_RADIO_INITIALIZER;
static bool SubBrute_s_uiDrawn = false;
static unsigned long SubBrute_s_lastDebounce = 0;
static const unsigned long SubBrute_kDebounceMs = 200;

static int SubBrute_s_freqIndex = 11;  // 433.92 MHz
static int SubBrute_s_bitsIndex = 2;   // 12-bit
static int SubBrute_s_pulseIndex = 1;  // 350 us
static int SubBrute_s_protocol = 1;
static SubBrute_FocusRow SubBrute_s_focus = SubBrute_FOCUS_FREQ;
static SubBrute_RunMode SubBrute_s_mode = SubBrute_MODE_DEBRUIJN;
static SubBrute_RunState SubBrute_s_runState = SubBrute_ST_IDLE;
static bool SubBrute_s_stopRequested = false;
static bool SubBrute_s_running = false;

static uint32_t SubBrute_s_progressDone = 0;
static uint32_t SubBrute_s_progressTotal = 0;
static uint32_t SubBrute_s_lastUiProgress = 0xFFFFFFFFu;
static uint8_t SubBrute_s_lastUiPct = 255;
static SubBrute_RunState SubBrute_s_lastUiState = SubBrute_ST_IDLE;
static SubBrute_FocusRow SubBrute_s_lastUiFocus = SubBrute_FOCUS_COUNT;
static SubBrute_RunMode SubBrute_s_lastUiMode = SubBrute_MODE_BRUTE;
static int SubBrute_s_lastUiFreq = -1;
static int SubBrute_s_lastUiBits = -1;
static int SubBrute_s_lastUiPulse = -1;
static int SubBrute_s_lastUiProto = -1;
static bool SubBrute_s_chromeDrawn = false;

static float SubBrute_bruteFreqMHz(void) {
  return SubBrute_kBruteFreqList[SubBrute_s_freqIndex %
                                 SubBrute_kBruteFreqCount] /
         1000000.0f;
}

static uint8_t SubBrute_bruteBits(void) {
  return SubBrute_kBitsChoices[SubBrute_s_bitsIndex %
                               SubBrute_kBitsChoiceCount];
}

static uint16_t SubBrute_brutePulseUs(void) {
  return SubBrute_kPulseChoicesUs[SubBrute_s_pulseIndex %
                                  SubBrute_kPulseChoiceCount];
}

static int SubBrute_settingsPanelY(void) { return SubBrute_kPanelTop; }
static int SubBrute_settingsPanelH(void) { return SubBrute_kSettingsInnerH; }
static int SubBrute_progressPanelY(void) {
  return SubBrute_kPanelTop + SubBrute_kSettingsInnerH + 6;
}
static int SubBrute_progressPanelH(void) { return SubBrute_kProgressInnerH; }
static int SubBrute_hintY0(void) {
  return SubBrute_progressPanelY() + SubBrute_kProgressInnerH + 6;
}

static int SubBrute_rowTextY(int row) {
  return SubBrute_settingsPanelY() + SubBrute_kBoxHeaderH + 2 +
         row * SubBrute_kLineH;
}

// Fibonacci LFSR feedback: taps are 1-indexed from LSB.
static uint32_t SubBrute_bruteLfsrFeedback(uint8_t n, uint32_t state) {
  uint32_t fb = 0;
  switch (n) {
    case 8:
      fb ^= (state >> (8 - 1)) & 1u;
      fb ^= (state >> (6 - 1)) & 1u;
      fb ^= (state >> (5 - 1)) & 1u;
      fb ^= (state >> (4 - 1)) & 1u;
      break;
    case 10:
      fb ^= (state >> (10 - 1)) & 1u;
      fb ^= (state >> (7 - 1)) & 1u;
      break;
    case 12:
      fb ^= (state >> (12 - 1)) & 1u;
      fb ^= (state >> (11 - 1)) & 1u;
      fb ^= (state >> (8 - 1)) & 1u;
      fb ^= (state >> (6 - 1)) & 1u;
      break;
    case 16:
      fb ^= (state >> (16 - 1)) & 1u;
      fb ^= (state >> (14 - 1)) & 1u;
      fb ^= (state >> (13 - 1)) & 1u;
      fb ^= (state >> (11 - 1)) & 1u;
      break;
    case 18:
      fb ^= (state >> (18 - 1)) & 1u;
      fb ^= (state >> (11 - 1)) & 1u;
      break;
    case 20:
      fb ^= (state >> (20 - 1)) & 1u;
      fb ^= (state >> (17 - 1)) & 1u;
      break;
    case 24:
      fb ^= (state >> (24 - 1)) & 1u;
      fb ^= (state >> (23 - 1)) & 1u;
      fb ^= (state >> (22 - 1)) & 1u;
      fb ^= (state >> (17 - 1)) & 1u;
      break;
    default:
      fb = (state >> (n - 1)) & 1u;
      break;
  }
  (void)n;
  return fb & 1u;
}

static void SubBrute_bruteRadioIdle(void) {
  cc1101_set_sidle(&(cc1101));
  gpio_set_level(((gpio_num_t)(SubBrute_BRUTE_TX_PIN)), 0);
}

static void SubBrute_brutePrepareTx(void) {
#if defined(SD_CS)
  sys_configure_gpio(SD_CS, GPIO_MODE_OUTPUT);
  gpio_set_level(((gpio_num_t)(SD_CS)), 1);
#endif
#if defined(CC1101_CS)
  sys_configure_gpio(CC1101_CS, GPIO_MODE_OUTPUT);
  gpio_set_level(((gpio_num_t)(CC1101_CS)), 1);
#endif
  cc1101_set_spi_pin(&(cc1101), CC1101_SCK, CC1101_MISO, CC1101_MOSI,
                     CC1101_CS);
  cc1101_set_gdo(&(cc1101), CC1101_GDO0, CC1101_GDO2);
  cc1101_set_sidle(&(cc1101));
  cc1101_set_mhz(&(cc1101), SubBrute_bruteFreqMHz());
  cc1101_set_cc_mode(&(cc1101), 0);
  cc1101_set_modulation(&(cc1101), 2);
  cc1101_set_pa(&(cc1101), 12);
  sys_configure_gpio(SubBrute_BRUTE_TX_PIN, GPIO_MODE_OUTPUT);
  gpio_set_level(((gpio_num_t)(SubBrute_BRUTE_TX_PIN)), 0);
  cc1101_set_tx_0(&(cc1101));
}

static void SubBrute_bruteFinishTx(void) {
  ook_radio_disable_transmit(&(SubBrute_s_switch));
  ook_radio_disable_receive(&(SubBrute_s_switch));
  SubBrute_bruteRadioIdle();
  sys_configure_gpio(SubBrute_BRUTE_TX_PIN, GPIO_MODE_OUTPUT);
  gpio_set_level(((gpio_num_t)(SubBrute_BRUTE_TX_PIN)), 0);
}

static bool SubBrute_bruteShouldAbort(void) {
  if (feature_exit_requested || featureExitButtonPressed()) {
    feature_exit_requested = true;
    SubBrute_s_stopRequested = true;
    return true;
  }
  if (SubBrute_s_stopRequested) {
    return true;
  }
  maintainTouchNavBar();
  if (isTouchNavButtonPressedEdge(BTN_UP)) {
    SubBrute_s_stopRequested = true;
    return true;
  }
  static bool prevPhysUp = false;
  const bool physUp = isPhysicalButtonPressed(BTN_UP);
  if (physUp && !prevPhysUp) {
    prevPhysUp = physUp;
    SubBrute_s_stopRequested = true;
    return true;
  }
  prevPhysUp = physUp;

  int x, y;
  if (readTouchXY(&(x), &(y))) {
    if (y > 20 && y < SubBrute_kBarBottom && x >= 10 && x < 26) {
      feature_exit_requested = true;
      SubBrute_s_stopRequested = true;
      return true;
    }
  }
  return false;
}

static void SubBrute_bruteWaitGoRelease(void) {
  const uint32_t t0 = sys_monotonic_ms();
  while ((isTouchNavButtonPressed(BTN_UP) || isPhysicalButtonPressed(BTN_UP)) &&
         (sys_monotonic_ms() - t0) < 800) {
    sys_sleep_ms(5);
  }
  sys_sleep_ms(30);
  (void)isTouchNavButtonPressedEdge(BTN_UP);
}

static void SubBrute_drawPanelFrame(int y, int h, const char* title) {
  display_draw_round_rect(&(tft), SubBrute_kPanelPadX, y, SubBrute_kPanelW, h,
                          SubBrute_kRadius, UI_LINE);
  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), UI_DIM_TEXT, TFT_BLACK);
  display_set_cursor(&(tft), SubBrute_kLabelX, y + 3);
  display_print(&(tft), (TEXT(title)).data);
}

static void SubBrute_clearValueCell(int y) {
  display_fill_rect(&(tft), SubBrute_kValueX, y, SubBrute_kValueW,
                    SubBrute_kLineH - 1, TFT_BLACK);
}

static void SubBrute_drawSettingRow(int row, bool focused) {
  const int y = SubBrute_rowTextY(row);
  display_fill_rect(&(tft), SubBrute_kLabelX, y,
                    SubBrute_kValueX - SubBrute_kLabelX - 2,
                    SubBrute_kLineH - 1, TFT_BLACK);
  SubBrute_clearValueCell(y);

  const char* label = "?";
  char value[28];
  value[0] = '\0';

  switch (row) {
    case SubBrute_FOCUS_FREQ:
      label = "Freq";
      snprintf(value, sizeof(value), "%.2f MHz", SubBrute_bruteFreqMHz());
      break;
    case SubBrute_FOCUS_BITS:
      label = "Bits";
      snprintf(value, sizeof(value), "%u", (unsigned)SubBrute_bruteBits());
      break;
    case SubBrute_FOCUS_MODE:
      label = "Mode";
      div_copy_string(
          value, sizeof(value),
          SubBrute_s_mode == SubBrute_MODE_DEBRUIJN ? "De Bruijn" : "Brute");
      break;
    case SubBrute_FOCUS_OPT:
      if (SubBrute_s_mode == SubBrute_MODE_DEBRUIJN) {
        label = "Pulse";
        snprintf(value, sizeof(value), "%u us",
                 (unsigned)SubBrute_brutePulseUs());
      } else {
        label = "Proto";
        snprintf(value, sizeof(value), "%d", SubBrute_s_protocol);
      }
      break;
    default:
      break;
  }

  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), focused ? ORANGE : UI_DIM_TEXT, TFT_BLACK);
  display_set_cursor(&(tft), SubBrute_kLabelX, y);
  display_print(&(tft), (TEXT(focused ? ">" : " ")).data);
  display_print(&(tft), (TEXT(label)).data);

  display_set_text_color_2(&(tft), focused ? ORANGE : UI_TEXT, TFT_BLACK);
  display_set_cursor(&(tft), SubBrute_kValueX, y);
  display_print(&(tft), (TEXT(value)).data);
}

static void SubBrute_drawProgressBody(bool force) {
  const int py = SubBrute_progressPanelY();
  const int statusY = py + SubBrute_kBoxHeaderH + 2;
  const int countY = statusY + SubBrute_kLineH;
  const int barX = SubBrute_kLabelX;
  const int barW = SubBrute_kPanelW - 16;
  enum { barH = 6 };

  uint8_t pct = 0;
  if (SubBrute_s_progressTotal > 0) {
    pct =
        (uint8_t)((SubBrute_s_progressDone * 100UL) / SubBrute_s_progressTotal);
    if (pct > 100) pct = 100;
  }

  const bool stateChanged =
      force || SubBrute_s_lastUiState != SubBrute_s_runState;
  const bool progChanged =
      force || SubBrute_s_lastUiProgress != SubBrute_s_progressDone ||
      SubBrute_s_lastUiPct != pct;

  if (stateChanged) {
    display_fill_rect(&(tft), SubBrute_kLabelX, statusY, SubBrute_kPanelW - 16,
                      SubBrute_kLineH - 1, TFT_BLACK);
    display_set_text_size(&(tft), 1);
    const char* st = "Idle";
    uint16_t col = UI_DIM_TEXT;
    if (SubBrute_s_runState == SubBrute_ST_RUNNING) {
      st = "Running";
      col = ORANGE;
    } else if (SubBrute_s_runState == SubBrute_ST_DONE) {
      st = "Done";
      col = UI_TEXT;
    } else if (SubBrute_s_runState == SubBrute_ST_STOPPED) {
      st = "Stopped";
      col = UI_WARN;
    }
    display_set_text_color_2(&(tft), UI_DIM_TEXT, TFT_BLACK);
    display_set_cursor(&(tft), SubBrute_kLabelX, statusY);
    display_print(&(tft), "Status");
    display_set_text_color_2(&(tft), col, TFT_BLACK);
    display_set_cursor(&(tft), SubBrute_kValueX, statusY);
    display_print(&(tft), (TEXT(st)).data);
    SubBrute_s_lastUiState = SubBrute_s_runState;
  }

  if (progChanged) {
    display_fill_rect(&(tft), SubBrute_kLabelX, countY, SubBrute_kPanelW - 16,
                      SubBrute_kLineH - 1, TFT_BLACK);
    char buf[40];
    if (SubBrute_s_progressTotal == 0) {
      snprintf(buf, sizeof(buf), "0 / 0");
    } else if (SubBrute_s_progressTotal >= 1000000UL) {
      snprintf(buf, sizeof(buf), "%lu/%lu %u%%",
               (unsigned long)SubBrute_s_progressDone,
               (unsigned long)SubBrute_s_progressTotal, (unsigned)pct);
    } else {
      snprintf(buf, sizeof(buf), "%lu / %lu  %u%%",
               (unsigned long)SubBrute_s_progressDone,
               (unsigned long)SubBrute_s_progressTotal, (unsigned)pct);
    }
    display_set_text_size(&(tft), 1);
    display_set_text_color_2(&(tft), UI_DIM_TEXT, TFT_BLACK);
    display_set_cursor(&(tft), SubBrute_kLabelX, countY);
    display_print(&(tft), "Count");
    display_set_text_color_2(&(tft), UI_TEXT, TFT_BLACK);
    display_set_cursor(&(tft), SubBrute_kValueX, countY);
    display_print(&(tft), (TEXT(buf)).data);

    const int barY2 = countY + SubBrute_kLineH + 2;
    display_fill_rect(&(tft), barX, barY2, barW, barH, DARK_GRAY);
    const int fill = (SubBrute_s_progressTotal > 0)
                         ? (int)((SubBrute_s_progressDone * (uint32_t)barW) /
                                 SubBrute_s_progressTotal)
                         : 0;
    if (fill > 0) {
      display_fill_rect(&(tft), barX, barY2, DIV_MIN(fill, barW), barH, ORANGE);
    }
    SubBrute_s_lastUiProgress = SubBrute_s_progressDone;
    SubBrute_s_lastUiPct = pct;
  }
}

static void SubBrute_drawHints(void) {
  const int y0 = SubBrute_hintY0();
  const int bottom = subghzContentBottom();
  if (y0 + 20 >= bottom) {
    return;
  }
  display_fill_rect(&(tft), 0, y0, 240, DIV_MIN(28, bottom - y0), TFT_BLACK);
  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), UI_DIM_TEXT, TFT_BLACK);
  display_set_cursor(&(tft), SubBrute_kLabelX, y0);
  display_print(&(tft), "Sel focus   Prev/Next adjust");
  if (y0 + 12 < bottom) {
    display_set_cursor(&(tft), SubBrute_kLabelX, y0 + 12);
    display_print(&(tft), "Go start/stop");
  }
}

static void SubBrute_updateDisplay(bool force) {
  if (force || !SubBrute_s_chromeDrawn) {
    const int bodyBottom = subghzContentBottom();
    if (bodyBottom > SubBrute_kBarBottom) {
      display_fill_rect(&(tft), 0, SubBrute_kBarBottom + 1, 240,
                        bodyBottom - SubBrute_kBarBottom - 1, TFT_BLACK);
    }
    display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
    display_draw_fast_h_line(&(tft), 0, SubBrute_kBarBottom, 240, UI_LINE);

    SubBrute_drawPanelFrame(SubBrute_settingsPanelY(),
                            SubBrute_settingsPanelH(), "Settings");
    SubBrute_drawPanelFrame(SubBrute_progressPanelY(),
                            SubBrute_progressPanelH(), "Progress");
    SubBrute_drawHints();

    for (int r = 0; r < SubBrute_kSettingsRows; r++) {
      SubBrute_drawSettingRow(r, (SubBrute_FocusRow)r == SubBrute_s_focus);
    }
    SubBrute_s_lastUiFocus = SubBrute_s_focus;
    SubBrute_s_lastUiMode = SubBrute_s_mode;
    SubBrute_s_lastUiFreq = SubBrute_s_freqIndex;
    SubBrute_s_lastUiBits = SubBrute_s_bitsIndex;
    SubBrute_s_lastUiPulse = SubBrute_s_pulseIndex;
    SubBrute_s_lastUiProto = SubBrute_s_protocol;
    SubBrute_s_lastUiState = (SubBrute_RunState)255;
    SubBrute_s_lastUiProgress = 0xFFFFFFFFu;
    SubBrute_s_lastUiPct = 255;
    SubBrute_s_chromeDrawn = true;
    SubBrute_drawProgressBody(true);
    return;
  }

  const bool settingsDirty = SubBrute_s_lastUiFocus != SubBrute_s_focus ||
                             SubBrute_s_lastUiMode != SubBrute_s_mode ||
                             SubBrute_s_lastUiFreq != SubBrute_s_freqIndex ||
                             SubBrute_s_lastUiBits != SubBrute_s_bitsIndex ||
                             SubBrute_s_lastUiPulse != SubBrute_s_pulseIndex ||
                             SubBrute_s_lastUiProto != SubBrute_s_protocol;

  if (settingsDirty) {
    for (int r = 0; r < SubBrute_kSettingsRows; r++) {
      SubBrute_drawSettingRow(r, (SubBrute_FocusRow)r == SubBrute_s_focus);
    }
    SubBrute_s_lastUiFocus = SubBrute_s_focus;
    SubBrute_s_lastUiMode = SubBrute_s_mode;
    SubBrute_s_lastUiFreq = SubBrute_s_freqIndex;
    SubBrute_s_lastUiBits = SubBrute_s_bitsIndex;
    SubBrute_s_lastUiPulse = SubBrute_s_pulseIndex;
    SubBrute_s_lastUiProto = SubBrute_s_protocol;
  }

  SubBrute_drawProgressBody(false);
}

static void SubBrute_invalidateChrome(void) { SubBrute_s_chromeDrawn = false; }

static void SubBrute_adjustFocused(int dir) {
  if (SubBrute_s_running) {
    return;
  }
  switch (SubBrute_s_focus) {
    case SubBrute_FOCUS_FREQ:
      SubBrute_s_freqIndex =
          (SubBrute_s_freqIndex + dir + SubBrute_kBruteFreqCount) %
          SubBrute_kBruteFreqCount;
      cc1101_set_mhz(&(cc1101), SubBrute_bruteFreqMHz());
      break;
    case SubBrute_FOCUS_BITS:
      SubBrute_s_bitsIndex =
          (SubBrute_s_bitsIndex + dir + SubBrute_kBitsChoiceCount) %
          SubBrute_kBitsChoiceCount;
      break;
    case SubBrute_FOCUS_MODE:
      SubBrute_s_mode = (SubBrute_s_mode == SubBrute_MODE_DEBRUIJN)
                            ? SubBrute_MODE_BRUTE
                            : SubBrute_MODE_DEBRUIJN;
      break;
    case SubBrute_FOCUS_OPT:
      if (SubBrute_s_mode == SubBrute_MODE_DEBRUIJN) {
        SubBrute_s_pulseIndex =
            (SubBrute_s_pulseIndex + dir + SubBrute_kPulseChoiceCount) %
            SubBrute_kPulseChoiceCount;
      } else {
        SubBrute_s_protocol += dir;
        if (SubBrute_s_protocol < 1) SubBrute_s_protocol = 12;
        if (SubBrute_s_protocol > 12) SubBrute_s_protocol = 1;
      }
      break;
    default:
      break;
  }
  SubBrute_updateDisplay(false);
  SubBrute_s_lastDebounce = sys_monotonic_ms();
}

static void SubBrute_cycleFocus(void) {
  if (SubBrute_s_running) {
    return;
  }
  SubBrute_s_focus =
      (SubBrute_FocusRow)((SubBrute_s_focus + 1) % SubBrute_FOCUS_COUNT);
  SubBrute_updateDisplay(false);
  SubBrute_s_lastDebounce = sys_monotonic_ms();
}

static void SubBrute_focusRowAtY(int y) {
  if (SubBrute_s_running) {
    return;
  }
  for (int r = 0; r < SubBrute_kSettingsRows; r++) {
    const int ry = SubBrute_rowTextY(r);
    if (y >= ry && y < ry + SubBrute_kLineH) {
      SubBrute_s_focus = (SubBrute_FocusRow)r;
      SubBrute_updateDisplay(false);
      SubBrute_s_lastDebounce = sys_monotonic_ms();
      return;
    }
  }
}

static void SubBrute_runDeBruijnStream(void) {
  const uint8_t n = SubBrute_bruteBits();
  const uint16_t pulse = SubBrute_brutePulseUs();
  const uint32_t mask = (n >= 32) ? 0xFFFFFFFFu : ((1UL << n) - 1UL);
  const uint32_t total = (n >= 32) ? 0xFFFFFFFFu : ((1UL << n) - 1UL);

  SubBrute_s_progressTotal = total;
  SubBrute_s_progressDone = 0;
  SubBrute_s_runState = SubBrute_ST_RUNNING;
  SubBrute_updateDisplay(true);

  SubBrute_brutePrepareTx();

  uint32_t state = 1u;
  for (uint32_t i = 0; i < total; i++) {
    if ((i & 0xFFu) == 0) {
      if (SubBrute_bruteShouldAbort()) {
        break;
      }
      sys_yield_task();
      sys_sleep_ms(0);
      SubBrute_s_progressDone = i;
      SubBrute_drawProgressBody(false);
    }

    const uint8_t bit = (uint8_t)(state & 1u);
    gpio_set_level(((gpio_num_t)(SubBrute_BRUTE_TX_PIN)), bit ? 1 : 0);
    esp_rom_delay_us(pulse);

    const uint32_t fb = SubBrute_bruteLfsrFeedback(n, state);
    state = ((state << 1) | fb) & mask;
  }

  gpio_set_level(((gpio_num_t)(SubBrute_BRUTE_TX_PIN)), 0);
  SubBrute_bruteFinishTx();

  if (SubBrute_s_stopRequested || feature_exit_requested) {
    SubBrute_s_runState = SubBrute_ST_STOPPED;
  } else {
    SubBrute_s_progressDone = total;
    SubBrute_s_runState = SubBrute_ST_DONE;
  }
  SubBrute_s_running = false;
  SubBrute_s_stopRequested = false;
  SubBrute_updateDisplay(true);
}

static void SubBrute_runBruteForce(void) {
  const uint8_t bits = SubBrute_bruteBits();
  // Practical cap: framed RCSwitch brute beyond 12 bits is extremely slow.
  // Still allow larger sizes, but keep the loop responsive.
  const uint32_t total = (bits >= 31) ? 0x7FFFFFFFu : (1UL << bits);

  SubBrute_s_progressTotal = total;
  SubBrute_s_progressDone = 0;
  SubBrute_s_runState = SubBrute_ST_RUNNING;
  SubBrute_updateDisplay(true);

  SubBrute_brutePrepareTx();
  ook_radio_disable_receive(&(SubBrute_s_switch));
  ook_radio_enable_transmit(&(SubBrute_s_switch), SubBrute_BRUTE_TX_PIN);
  ook_radio_set_protocol(&(SubBrute_s_switch), SubBrute_s_protocol);
  ook_radio_set_pulse_length(&(SubBrute_s_switch), SubBrute_brutePulseUs());
  // Default RCSwitch repeats (~10) make each code ~0.5–1s → looks frozen.
  ook_radio_set_repeat_transmit(&(SubBrute_s_switch), 1);

  uint32_t lastUiMs = sys_monotonic_ms();
  for (uint32_t code = 0; code < total; code++) {
    // Poll stop often — send() itself blocks briefly per code.
    if ((code & 0x03u) == 0) {
      if (SubBrute_bruteShouldAbort()) {
        SubBrute_s_progressDone = code;
        break;
      }
      sys_yield_task();
    }

    ook_radio_send(&(SubBrute_s_switch), code, bits);
    SubBrute_s_progressDone = code + 1;

    // Keep progress alive so the UI doesn't look hung.
    const uint32_t now = sys_monotonic_ms();
    if (now - lastUiMs >= 150) {
#if defined(CC1101_CS)
      gpio_set_level(((gpio_num_t)(CC1101_CS)), 1);
#endif
      SubBrute_drawProgressBody(false);
      maintainTouchNavBar();
      // TFT SPI can disturb CC1101 — re-enter TX for the next burst.
      cc1101_set_tx_0(&(cc1101));
      lastUiMs = now;
      sys_yield_task();
    }
  }

  SubBrute_bruteFinishTx();

  if (SubBrute_s_stopRequested || feature_exit_requested) {
    SubBrute_s_runState = SubBrute_ST_STOPPED;
  } else {
    SubBrute_s_progressDone = total;
    SubBrute_s_runState = SubBrute_ST_DONE;
  }
  SubBrute_s_running = false;
  SubBrute_s_stopRequested = false;
  SubBrute_updateDisplay(true);
}

static void SubBrute_startOrStop(void) {
  if (SubBrute_s_running) {
    SubBrute_s_stopRequested = true;
    SubBrute_s_lastDebounce = sys_monotonic_ms();
    return;
  }

  SubBrute_bruteWaitGoRelease();
  SubBrute_s_stopRequested = false;
  SubBrute_s_running = true;
  SubBrute_s_runState = SubBrute_ST_RUNNING;
  SubBrute_s_progressDone = 0;
  SubBrute_s_progressTotal = 0;
  SubBrute_updateDisplay(true);

  if (SubBrute_s_mode == SubBrute_MODE_DEBRUIJN) {
    SubBrute_runDeBruijnStream();
  } else {
    SubBrute_runBruteForce();
  }
  SubBrute_s_lastDebounce = sys_monotonic_ms();
}

void SubBrute_bruteHandleNavButtons(void) {
  if (!featureHasTouchNavBar()) {
    return;
  }
  if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
    SubBrute_adjustFocused(-1);
    subghzWaitNavRelease(BTN_LEFT);
  }
  if (isTouchNavButtonPressedEdge(BTN_RIGHT)) {
    SubBrute_adjustFocused(+1);
    subghzWaitNavRelease(BTN_RIGHT);
  }
  if (isTouchNavButtonPressedEdge(BTN_DOWN)) {
    SubBrute_cycleFocus();
    subghzWaitNavRelease(BTN_DOWN);
  }
  if (isTouchNavButtonPressedEdge(BTN_UP)) {
    SubBrute_startOrStop();
    subghzWaitNavRelease(BTN_UP);
  }
}

void SubBrute_runUI(void) {
  // Avoid jammer/replay macros (SCREEN_WIDTH, ICON_NUM, …) leaking into this
  // scope.
  enum { kBarY = 20 };
  enum { kBarH = 16 };
  enum { kIconSz = 16 };
  enum { kIconN = 6 };
  enum { kScreenW = 240 };

  static int iconX[kIconN] = {50, 90, 130, 170, 210, 10};
  static int iconY = kBarY;

  static const unsigned char* icons[kIconN] = {
      bitmap_icon_power,        bitmap_icon_antenna,
      bitmap_icon_random,       bitmap_icon_sort_down_minus,
      bitmap_icon_sort_up_plus, bitmap_icon_go_back};

  if (!SubBrute_s_uiDrawn) {
    display_fill_rect(&(tft), 0, kBarY, kScreenW, kBarH, DARK_GRAY);
    for (int i = 0; i < kIconN; i++) {
      if (icons[i] != NULL) {
        display_draw_bitmap(&(tft), iconX[i], iconY, icons[i], kIconSz, kIconSz,
                            UI_ICON);
      }
    }
    display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
    display_draw_fast_h_line(&(tft), 0, kBarY + kBarH, 240, UI_LINE);
    SubBrute_s_uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  if (animationState > 0 && sys_monotonic_ms() - lastAnimationTime >= 150) {
    if (animationState == 1) {
      display_draw_bitmap(&(tft), iconX[activeIcon], iconY, icons[activeIcon],
                          kIconSz, kIconSz, UI_ICON);
      animationState = 2;
      switch (activeIcon) {
        case 0:
          SubBrute_startOrStop();
          break;
        case 1:
          if (!SubBrute_s_running) {
            SubBrute_s_mode = (SubBrute_s_mode == SubBrute_MODE_DEBRUIJN)
                                  ? SubBrute_MODE_BRUTE
                                  : SubBrute_MODE_DEBRUIJN;
            SubBrute_updateDisplay(false);
          }
          break;
        case 2:
          SubBrute_cycleFocus();
          break;
        case 3:
          SubBrute_adjustFocused(-1);
          break;
        case 4:
          SubBrute_adjustFocused(+1);
          break;
        case 5:
          feature_exit_requested = true;
          SubBrute_s_stopRequested = true;
          break;
      }
    } else if (animationState == 2) {
      animationState = 0;
      activeIcon = -1;
    }
    lastAnimationTime = sys_monotonic_ms();
  }

  static unsigned long lastTouchCheck = 0;
  if (sys_monotonic_ms() - lastTouchCheck >= 50) {
    int x, y;
    if (feature_active && readTouchXY(&(x), &(y))) {
      if (y > kBarY && y < kBarY + kBarH) {
        for (int i = 0; i < kIconN; i++) {
          if (x > iconX[i] && x < iconX[i] + kIconSz) {
            if (icons[i] != NULL && animationState == 0) {
              if (i == 5) {
                feature_exit_requested = true;
                SubBrute_s_stopRequested = true;
              } else {
                display_draw_bitmap(&(tft), iconX[i], iconY, icons[i], kIconSz,
                                    kIconSz, TFT_BLACK);
                animationState = 1;
                activeIcon = i;
                lastAnimationTime = sys_monotonic_ms();
              }
            }
            break;
          }
        }
      } else if (!SubBrute_s_running && y >= SubBrute_settingsPanelY() &&
                 y < SubBrute_settingsPanelY() + SubBrute_settingsPanelH()) {
        SubBrute_focusRowAtY(y);
      }
    }
    lastTouchCheck = sys_monotonic_ms();
  }
}

void SubBrute_subBruteSetup(void) {
  div_console_open(115200);
  setTouchButtonInputEnabled(true);
  SubBrute_subghzSetBruteNavLabels();
  subghzClearBody(TFT_BLACK);
  drawStatusBar(readBatteryVoltage(), true, false);
  subghzRedrawNavChrome();

#if defined(SD_CS)
  sys_configure_gpio(SD_CS, GPIO_MODE_OUTPUT);
  gpio_set_level(((gpio_num_t)(SD_CS)), 1);
#endif
#if defined(CC1101_CS)
  sys_configure_gpio(CC1101_CS, GPIO_MODE_OUTPUT);
  gpio_set_level(((gpio_num_t)(CC1101_CS)), 1);
#endif

  cc1101_set_spi_pin(&(cc1101), CC1101_SCK, CC1101_MISO, CC1101_MOSI,
                     CC1101_CS);
  cc1101_set_gdo(&(cc1101), CC1101_GDO0, CC1101_GDO2);
  cc1101_init(&(cc1101));
  cc1101_set_cc_mode(&(cc1101), 0);
  cc1101_set_modulation(&(cc1101), 2);
  cc1101_set_rx_bw(&(cc1101), 500.0);
  cc1101_set_pa(&(cc1101), 12);
  cc1101_set_mhz(&(cc1101), SubBrute_bruteFreqMHz());
  cc1101_set_sidle(&(cc1101));
  sys_configure_gpio(SubBrute_BRUTE_TX_PIN, GPIO_MODE_INPUT);

  SubBrute_s_freqIndex = 11;
  SubBrute_s_bitsIndex = 2;
  SubBrute_s_pulseIndex = 1;
  SubBrute_s_protocol = 1;
  SubBrute_s_focus = SubBrute_FOCUS_FREQ;
  SubBrute_s_mode = SubBrute_MODE_DEBRUIJN;
  SubBrute_s_runState = SubBrute_ST_IDLE;
  SubBrute_s_running = false;
  SubBrute_s_stopRequested = false;
  SubBrute_s_progressDone = 0;
  SubBrute_s_progressTotal = 0;

#if HAS_PCF8574_BUTTONS
  pcf8574_configure_pin(button_input_device(), BTN_LEFT, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_RIGHT, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_DOWN, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_UP, GPIO_MODE_INPUT);
#endif
  sys_sleep_ms(100);

  subghzClearBody(TFT_BLACK);
  drawStatusBar(readBatteryVoltage(), true, false);
  setupTouchscreen();

  SubBrute_invalidateChrome();
  SubBrute_s_uiDrawn = false;
  SubBrute_updateDisplay(true);
  subghzRedrawNavChrome();
}

void SubBrute_subBruteLoop(void) {
  if (feature_active &&
      (feature_exit_requested || featureExitButtonPressed())) {
    feature_exit_requested = true;
    SubBrute_s_stopRequested = true;
    return;
  }

  maintainTouchNavBar();
  SubBrute_runUI();
  if (SubBrute_s_uiDrawn) {
    display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
    display_draw_fast_h_line(&(tft), 0, SubBrute_kBarBottom, 240, UI_LINE);
  }
  SubBrute_bruteHandleNavButtons();

#if HAS_PCF8574_BUTTONS
  const int btnLeftState = pcf8574_read_pin(button_input_device(), BTN_LEFT);
  const int btnRightState = pcf8574_read_pin(button_input_device(), BTN_RIGHT);
  const int btnUpState = pcf8574_read_pin(button_input_device(), BTN_UP);
  const int btnDownState = pcf8574_read_pin(button_input_device(), BTN_DOWN);
#else
  const int btnLeftState = isPhysicalButtonPressed(BTN_LEFT) ? 0 : 1;
  const int btnRightState = isPhysicalButtonPressed(BTN_RIGHT) ? 0 : 1;
  const int btnUpState = isPhysicalButtonPressed(BTN_UP) ? 0 : 1;
  const int btnDownState = isPhysicalButtonPressed(BTN_DOWN) ? 0 : 1;
#endif

  if (btnLeftState == 0 &&
      sys_monotonic_ms() - SubBrute_s_lastDebounce > SubBrute_kDebounceMs) {
    SubBrute_adjustFocused(-1);
  }
  if (btnRightState == 0 &&
      sys_monotonic_ms() - SubBrute_s_lastDebounce > SubBrute_kDebounceMs) {
    SubBrute_adjustFocused(+1);
  }
  if (btnDownState == 0 &&
      sys_monotonic_ms() - SubBrute_s_lastDebounce > SubBrute_kDebounceMs) {
    SubBrute_cycleFocus();
  }
  if (btnUpState == 0 &&
      sys_monotonic_ms() - SubBrute_s_lastDebounce > SubBrute_kDebounceMs) {
    SubBrute_startOrStop();
  }
}

enum { jammingdetector_JD_SAMPLES = ESP32DIV_JD_RSSI_SAMPLES };
static const double jammingdetector_JD_SAMPLE_HZ = 5000.0;
static const double jammingdetector_JD_RXBW = 650.0;
enum { jammingdetector_JD_MARGIN_DB = 18 };
enum { jammingdetector_JD_ABS_THRESH_DBM = -75 };
static const float jammingdetector_JD_BUSY_WIN_DUTY = 0.50f;
enum { jammingdetector_JD_JAM_STREAK_MS = 400 };
static const float jammingdetector_JD_JAM_AVG_DUTY = 0.80f;
static const float jammingdetector_JD_ACTIVITY_DUTY = 0.10f;
enum { jammingdetector_JD_RING = 20 };
enum { jammingdetector_JD_FLOOR_INIT_DBM = -95 };

static const uint32_t jammingdetector_kFreqHz[] = {433920000UL, 434420000UL,
                                                   315000000UL, 868350000UL};
static const char* jammingdetector_kFreqLabel[] = {"433.92", "434.42", "315.00",
                                                   "868.35"};
enum {
  jammingdetector_kFreqCount = sizeof(jammingdetector_kFreqHz) /
  sizeof(jammingdetector_kFreqHz[0])
};
static uint8_t jammingdetector_freqIdx = 1;

static unsigned int jammingdetector_samplingPeriod = 0;

static float jammingdetector_noiseFloor = jammingdetector_JD_FLOOR_INIT_DBM;
static uint32_t jammingdetector_busyStreakMs = 0;
static float jammingdetector_dutyRing[jammingdetector_JD_RING];
static uint8_t jammingdetector_dutyRingPos = 0;
static bool jammingdetector_jamActive = false;
static uint32_t jammingdetector_jamStartMs = 0;
static int jammingdetector_jamPeakDbm = -127;
static uint32_t jammingdetector_eventCount = 0;

static bool jammingdetector_logEnabled = false;
static bool jammingdetector_logMounted = false;
static bool jammingdetector_prevLeft = false, jammingdetector_prevRight = false,
            jammingdetector_prevUp = false, jammingdetector_prevDown = false;

enum { jammingdetector_kJdBarBottom = 36 };
enum { jammingdetector_kJdSectionGap = 6 };
enum { jammingdetector_kJdPad = 4 };
enum { jammingdetector_kJdPanelW = 240 - (jammingdetector_kJdPad * 2) };
enum { jammingdetector_kJdRadius = 3 };
enum { jammingdetector_kJdLabelX = 10 };
enum { jammingdetector_kJdCol2LabelX = 128 };
enum { jammingdetector_kJdValueX = 44 };
enum { jammingdetector_kJdCol2ValueX = 162 };
enum { jammingdetector_kJdCol1ValueW = 78 };
enum { jammingdetector_kJdCol2ValueW = 70 };
enum { jammingdetector_kJdLineH = 12 };
enum { jammingdetector_kJdPanelHeader = 14 };
enum { jammingdetector_kJdInfoY = jammingdetector_kJdBarBottom + 4 };
enum { jammingdetector_kJdInfoH = 46 };
enum {
  jammingdetector_kJdInfoRow1Y = jammingdetector_kJdInfoY +
  jammingdetector_kJdPanelHeader + 2
};
enum {
  jammingdetector_kJdInfoRow2Y = jammingdetector_kJdInfoRow1Y +
  jammingdetector_kJdLineH + 2
};
enum {
  jammingdetector_kJdStatusY = jammingdetector_kJdInfoY +
  jammingdetector_kJdInfoH + jammingdetector_kJdSectionGap
};
enum { jammingdetector_kJdStatusH = 30 };
enum {
  jammingdetector_kJdWaveY = jammingdetector_kJdStatusY +
  jammingdetector_kJdStatusH + jammingdetector_kJdSectionGap
};
enum { jammingdetector_kJdWaveHeader = 14 };

enum { jammingdetector_kWaveW = ESP32DIV_JD_WAVE_WIDTH };
enum { jammingdetector_kWaveRssiMin = -100 };
enum { jammingdetector_kWaveRssiMax = -35 };
static int8_t jammingdetector_waveBuf[jammingdetector_kWaveW];
static uint16_t jammingdetector_waveWrite = 0;
static float jammingdetector_waveSmooth = -95.0f;

static bool jammingdetector_s_chromeDrawn = false;
static bool jammingdetector_s_jdUiDrawn = false;
static bool jammingdetector_s_waveHasPrev = false;
static int16_t jammingdetector_s_wavePrevY[jammingdetector_kWaveW];
static uint8_t jammingdetector_s_statusState = 255;

typedef struct jammingdetector_JdDisp {
  bool valid;
  uint8_t freqIdx;
  int rssi;
  int floor;
  int dutyPct;
  uint32_t events;
  bool logOn;
} jammingdetector_JdDisp;
#define jammingdetector_JdDisp_INITIALIZER \
  {.valid = false,                         \
   .freqIdx = 255,                         \
   .rssi = -999,                           \
   .floor = -999,                          \
   .dutyPct = -1,                          \
   .events = 0xFFFFFFFFu,                  \
   .logOn = false}
;
static jammingdetector_JdDisp jammingdetector_s_disp =
    jammingdetector_JdDisp_INITIALIZER;

static void jammingdetector_cc1101BeginRx(void) {
  cc1101_set_spi_pin(&(cc1101), CC1101_SCK, CC1101_MISO, CC1101_MOSI,
                     CC1101_CS);
  cc1101_init(&(cc1101));
  cc1101_set_modulation(&(cc1101), 2);
  cc1101_set_rx_bw(&(cc1101), jammingdetector_JD_RXBW);
  cc1101_set_gdo(&(cc1101), CC1101_GDO0, CC1101_GDO2);
  cc1101_set_mhz(&(cc1101),
                 jammingdetector_kFreqHz[jammingdetector_freqIdx] / 1000000.0);
  cc1101_set_rx_0(&(cc1101));
}

static void jammingdetector_tuneTo(uint8_t idx) {
  jammingdetector_freqIdx = idx % jammingdetector_kFreqCount;
  cc1101_set_sidle(&(cc1101));
  cc1101_set_mhz(&(cc1101),
                 jammingdetector_kFreqHz[jammingdetector_freqIdx] / 1000000.0);
  cc1101_set_rx_0(&(cc1101));
  jammingdetector_s_disp.freqIdx = 255;
}

static bool jammingdetector_jdMountSD(void) {
  if (jammingdetector_logMounted && storage_exists("/")) return true;

#if defined(CC1101_CS)
  sys_configure_gpio(CC1101_CS, GPIO_MODE_OUTPUT);
  gpio_set_level(((gpio_num_t)(CC1101_CS)), 1);
#endif

  refreshSdMount();
  jammingdetector_logMounted = isSDCardAvailable();
  return jammingdetector_logMounted;
}

static void jammingdetector_logEvent(uint32_t whenMs, uint32_t durMs,
                                     int peakDbm, int dutyPct) {
  if (!jammingdetector_logEnabled) return;

  refreshSdMount();
  if (jammingdetector_jdMountSD()) {
    if (!storage_exists(LOG_DIR)) storage_mkdir(LOG_DIR);
    storage_file_t f __attribute__((cleanup(storage_file_close))) =
        storage_file_open(LOG_DIR "/jamdet.csv", "ab");
    if (storage_file_valid(&(f))) {
      storage_file_printf(&(f), "%lu,%s,JAM,%d,%lu,%d\n", (unsigned long)whenMs,
                          jammingdetector_kFreqLabel[jammingdetector_freqIdx],
                          peakDbm, (unsigned long)durMs, dutyPct);
      storage_file_close(&(f));
    }
  }
  jammingdetector_cc1101BeginRx();
}

static int jammingdetector_jdWaveBottom(void) {
  return subghzContentBottom() - 2;
}

static int jammingdetector_jdPlotTop(void) {
  return jammingdetector_kJdWaveY + jammingdetector_kJdWaveHeader + 4;
}

static int jammingdetector_jdPlotHeight(void) {
  const int h =
      jammingdetector_jdWaveBottom() - jammingdetector_jdPlotTop() - 2;
  return h > 8 ? h : 8;
}

static int jammingdetector_jdRssiToY(int dbm) {
  dbm = DIV_CLAMP(dbm, jammingdetector_kWaveRssiMin,
                  jammingdetector_kWaveRssiMax);
  const int plotH = jammingdetector_jdPlotHeight();
  return jammingdetector_jdPlotTop() + plotH - 2 -
         ((dbm - jammingdetector_kWaveRssiMin) * (plotH - 4) /
          (jammingdetector_kWaveRssiMax - jammingdetector_kWaveRssiMin));
}

static void jammingdetector_jdDrawValueCell(int x, int y, int w,
                                            const char* text, uint16_t color) {
  display_fill_rect(&(tft), x, y, w, jammingdetector_kJdLineH, TFT_BLACK);
  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), color, TFT_BLACK);
  display_set_cursor(&(tft), x, y);
  display_print(&(tft), (TEXT(text)).data);
}

static void jammingdetector_jdInvalidateContent(void) {
  jammingdetector_s_chromeDrawn = false;
  jammingdetector_s_statusState = 255;
  jammingdetector_s_disp.valid = false;
  jammingdetector_s_waveHasPrev = false;
  jammingdetector_waveWrite = 0;
  jammingdetector_waveSmooth = -95.0f;
  memset(jammingdetector_waveBuf, jammingdetector_kWaveRssiMin,
         sizeof(jammingdetector_waveBuf));
}

static void jammingdetector_jdInvalidateAll(void) {
  jammingdetector_jdInvalidateContent();
  jammingdetector_s_jdUiDrawn = false;
}

static void jammingdetector_jdResetStats(void) {
  jammingdetector_eventCount = 0;
  jammingdetector_busyStreakMs = 0;
  jammingdetector_jamActive = false;
  jammingdetector_noiseFloor = jammingdetector_JD_FLOOR_INIT_DBM;
  for (uint8_t i = 0; i < jammingdetector_JD_RING; i++)
    jammingdetector_dutyRing[i] = 0;
  jammingdetector_jdInvalidateContent();
}

static void jammingdetector_jdRunUI(void) {
  enum { kBarY = 20 };
  enum { kBarH = 16 };
  enum { kIconSz = 16 };
  enum { kIconN = 5 };
  enum { kBackIdx = 4 };

  static int iconX[kIconN] = {90, 130, 170, 210, 10};
  static int iconY = kBarY;

  static const unsigned char* icons[kIconN] = {
      bitmap_icon_sort_down_minus, bitmap_icon_floppy,  bitmap_icon_undo,
      bitmap_icon_sort_up_plus,    bitmap_icon_go_back,
  };

  if (!jammingdetector_s_jdUiDrawn) {
    display_fill_rect(&(tft), 0, kBarY, 240, kBarH, DARK_GRAY);
    for (int i = 0; i < kIconN; i++) {
      display_draw_bitmap(&(tft), iconX[i], iconY, icons[i], kIconSz, kIconSz,
                          UI_ICON);
    }
    display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
    display_draw_fast_h_line(&(tft), 0, kBarY + kBarH, 240, UI_LINE);
    jammingdetector_s_jdUiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  if (animationState > 0 && sys_monotonic_ms() - lastAnimationTime >= 150) {
    if (animationState == 1) {
      if (activeIcon >= 0 && activeIcon != kBackIdx) {
        display_draw_bitmap(&(tft), iconX[activeIcon], iconY, icons[activeIcon],
                            kIconSz, kIconSz, UI_ICON);
      }
      animationState = 2;
      switch (activeIcon) {
        case 0:
          jammingdetector_tuneTo(jammingdetector_freqIdx +
                                 jammingdetector_kFreqCount - 1);
          jammingdetector_s_disp.freqIdx = 255;
          break;
        case 1:
          jammingdetector_logEnabled = !jammingdetector_logEnabled;
          jammingdetector_s_disp.logOn = !jammingdetector_logEnabled;
          break;
        case 2:
          jammingdetector_jdResetStats();
          break;
        case 3:
          jammingdetector_tuneTo(jammingdetector_freqIdx + 1);
          jammingdetector_s_disp.freqIdx = 255;
          break;
        case kBackIdx:
          feature_exit_requested = true;
          break;
        default:
          break;
      }
    } else if (animationState == 2) {
      animationState = 0;
      activeIcon = -1;
    }
    lastAnimationTime = sys_monotonic_ms();
  }

  static unsigned long lastTouchCheck = 0;
  if (sys_monotonic_ms() - lastTouchCheck >= 50) {
    int x, y;
    if (feature_active && readTouchXY(&(x), &(y))) {
      if (y > kBarY && y < kBarY + kBarH) {
        for (int i = 0; i < kIconN; i++) {
          if (x > iconX[i] && x < iconX[i] + kIconSz) {
            if (animationState == 0) {
              if (i == kBackIdx) {
                feature_exit_requested = true;
              } else {
                display_fill_rect(&(tft), iconX[i], iconY, kIconSz, kIconSz,
                                  DARK_GRAY);
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

static void jammingdetector_jdDrawPanelFrame(int y, int h, const char* title) {
  display_draw_round_rect(&(tft), jammingdetector_kJdPad, y,
                          jammingdetector_kJdPanelW, h,
                          jammingdetector_kJdRadius, UI_LINE);
  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), UI_DIM_TEXT, TFT_BLACK);
  display_set_cursor(&(tft), jammingdetector_kJdLabelX, y + 3);
  display_print(&(tft), (TEXT(title)).data);
}

static void jammingdetector_jdDrawPlotGridLines(void) {
  const int plotX = jammingdetector_kJdPad + 4;
  const int plotW = jammingdetector_kJdPanelW - 8;
  const int plotTop = jammingdetector_jdPlotTop();
  const int plotH = jammingdetector_jdPlotHeight();
  if (plotH < 8) {
    return;
  }

  for (int g = 1; g < 4; g++) {
    const int gy = plotTop + (plotH * g) / 4;
    display_draw_fast_h_line(&(tft), plotX + 1, gy, plotW - 2, DARK_GRAY);
  }
  for (int g = 1; g < 4; g++) {
    const int gx = plotX + (plotW * g) / 4;
    display_draw_fast_v_line(&(tft), gx, plotTop + 1, plotH - 2, DARK_GRAY);
  }
}

static void jammingdetector_jdDrawPlotBackground(void) {
  const int plotX = jammingdetector_kJdPad + 4;
  const int plotW = jammingdetector_kJdPanelW - 8;
  const int plotTop = jammingdetector_jdPlotTop();
  const int plotH = jammingdetector_jdPlotHeight();
  if (plotH < 8) {
    return;
  }

  display_fill_rect(&(tft), plotX, plotTop, plotW, plotH, TFT_BLACK);
  jammingdetector_jdDrawPlotGridLines();
}

static void jammingdetector_jdDrawStaticChrome(void) {
  if (jammingdetector_s_chromeDrawn) {
    return;
  }

  const int bodyBottom = jammingdetector_jdWaveBottom();
  display_fill_rect(&(tft), 0, jammingdetector_kJdBarBottom + 1, 240,
                    bodyBottom - jammingdetector_kJdBarBottom - 1, TFT_BLACK);

  jammingdetector_jdDrawPanelFrame(jammingdetector_kJdInfoY,
                                   jammingdetector_kJdInfoH, "Monitor");
  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), UI_DIM_TEXT, TFT_BLACK);
  display_set_cursor(&(tft), jammingdetector_kJdLabelX,
                     jammingdetector_kJdInfoRow1Y);
  display_print(&(tft), "Freq");
  display_set_cursor(&(tft), jammingdetector_kJdLabelX,
                     jammingdetector_kJdInfoRow2Y);
  display_print(&(tft), "RSSI");
  display_set_cursor(&(tft), jammingdetector_kJdCol2LabelX,
                     jammingdetector_kJdInfoRow1Y);
  display_print(&(tft), "Duty");
  display_set_cursor(&(tft), jammingdetector_kJdCol2LabelX,
                     jammingdetector_kJdInfoRow2Y);
  display_print(&(tft), "Log");

  const int waveH = bodyBottom - jammingdetector_kJdWaveY;
  if (waveH > jammingdetector_kJdWaveHeader + 12) {
    jammingdetector_jdDrawPanelFrame(jammingdetector_kJdWaveY, waveH, "Signal");
    jammingdetector_jdDrawPlotBackground();
  }

  jammingdetector_s_chromeDrawn = true;
  jammingdetector_s_disp.valid = false;
  jammingdetector_s_statusState = 255;
}

static void jammingdetector_jdDrawStatusBox(bool jam, bool activity) {
  const uint8_t st = jam ? 2 : (activity ? 1 : 0);
  if (st == jammingdetector_s_statusState && jammingdetector_s_disp.valid) {
    return;
  }
  jammingdetector_s_statusState = st;

  uint16_t bg = jam ? ORANGE : (activity ? UI_WARN : UI_OK);
  uint16_t fg = (jam || activity) ? TFT_BLACK : UI_FG;
  const char* s = jam ? "JAMMING DETECTED" : (activity ? "ACTIVITY" : "CLEAR");

  display_fill_round_rect(&(tft), jammingdetector_kJdPad,
                          jammingdetector_kJdStatusY, jammingdetector_kJdPanelW,
                          jammingdetector_kJdStatusH, jammingdetector_kJdRadius,
                          bg);
  display_draw_round_rect(&(tft), jammingdetector_kJdPad,
                          jammingdetector_kJdStatusY, jammingdetector_kJdPanelW,
                          jammingdetector_kJdStatusH, jammingdetector_kJdRadius,
                          UI_LINE);
  display_set_text_color_2(&(tft), fg, bg);
  uint8_t sz = 2;
  display_set_text_size(&(tft), sz);
  if (display_text_width(&(tft), s, 0) > jammingdetector_kJdPanelW - 8) {
    sz = 1;
    display_set_text_size(&(tft), sz);
  }
  const int16_t tw = display_text_width(&(tft), s, 0);
  const int16_t th = 8 * sz;
  display_set_cursor(
      &(tft), jammingdetector_kJdPad + (jammingdetector_kJdPanelW - tw) / 2,
      jammingdetector_kJdStatusY + (jammingdetector_kJdStatusH - th) / 2);
  display_print(&(tft), (TEXT(s)).data);
  display_set_text_size(&(tft), 1);
}

static void jammingdetector_jdUpdateInfo(int rssiNow, int dutyPct) {
  jammingdetector_jdDrawStaticChrome();

  const bool full = !jammingdetector_s_disp.valid;
  char buf[28];

  if (full || jammingdetector_s_disp.freqIdx != jammingdetector_freqIdx) {
    snprintf(buf, sizeof(buf), "%s MHz",
             jammingdetector_kFreqLabel[jammingdetector_freqIdx]);
    jammingdetector_jdDrawValueCell(
        jammingdetector_kJdValueX, jammingdetector_kJdInfoRow1Y,
        jammingdetector_kJdCol1ValueW, buf, UI_TEXT);
    jammingdetector_s_disp.freqIdx = jammingdetector_freqIdx;
  }

  if (full || abs(jammingdetector_s_disp.rssi - rssiNow) >= 2 ||
      abs(jammingdetector_s_disp.floor - (int)jammingdetector_noiseFloor) >=
          2) {
    snprintf(buf, sizeof(buf), "%d/%d dBm", rssiNow,
             (int)jammingdetector_noiseFloor);
    jammingdetector_jdDrawValueCell(
        jammingdetector_kJdValueX, jammingdetector_kJdInfoRow2Y,
        jammingdetector_kJdCol1ValueW, buf, UI_TEXT);
    jammingdetector_s_disp.rssi = rssiNow;
    jammingdetector_s_disp.floor = (int)jammingdetector_noiseFloor;
  }

  if (full || abs(jammingdetector_s_disp.dutyPct - dutyPct) >= 5 ||
      jammingdetector_s_disp.events != jammingdetector_eventCount) {
    snprintf(buf, sizeof(buf), "%d%% E:%lu", dutyPct,
             (unsigned long)jammingdetector_eventCount);
    jammingdetector_jdDrawValueCell(
        jammingdetector_kJdCol2ValueX, jammingdetector_kJdInfoRow1Y,
        jammingdetector_kJdCol2ValueW, buf, UI_TEXT);
    jammingdetector_s_disp.dutyPct = dutyPct;
    jammingdetector_s_disp.events = jammingdetector_eventCount;
  }

  if (full || jammingdetector_s_disp.logOn != jammingdetector_logEnabled) {
    jammingdetector_jdDrawValueCell(
        jammingdetector_kJdCol2ValueX, jammingdetector_kJdInfoRow2Y,
        jammingdetector_kJdCol2ValueW,
        jammingdetector_logEnabled ? "on" : "off",
        jammingdetector_logEnabled ? UI_OK : UI_DIM_TEXT);
    jammingdetector_s_disp.logOn = jammingdetector_logEnabled;
  }

  jammingdetector_s_disp.valid = true;
}

static void jammingdetector_jdDrawWaveform(bool jam, bool activity) {
  const int plotX = jammingdetector_kJdPad + 4;
  const int plotW = jammingdetector_kJdPanelW - 8;
  if (jammingdetector_jdPlotHeight() < 8 || plotW < 2 ||
      jammingdetector_kWaveW < 2) {
    return;
  }

  const uint16_t waveColor = jam ? ORANGE : (activity ? UI_WARN : UI_OK);

  if (jammingdetector_s_waveHasPrev) {
    for (int x = 0; x < plotW - 1; x++) {
      const int px0 = (x * (jammingdetector_kWaveW - 1)) / (plotW - 1);
      const int px1 = ((x + 1) * (jammingdetector_kWaveW - 1)) / (plotW - 1);
      display_draw_line(&(tft), plotX + x, jammingdetector_s_wavePrevY[px0],
                        plotX + x + 1, jammingdetector_s_wavePrevY[px1],
                        TFT_BLACK);
    }
  }

  for (int i = 0; i < jammingdetector_kWaveW; i++) {
    jammingdetector_s_wavePrevY[i] = jammingdetector_jdRssiToY(
        jammingdetector_waveBuf[(jammingdetector_waveWrite + i) %
                                jammingdetector_kWaveW]);
  }

  for (int x = 0; x < plotW - 1; x++) {
    const int i0 = (x * (jammingdetector_kWaveW - 1)) / (plotW - 1);
    const int i1 = ((x + 1) * (jammingdetector_kWaveW - 1)) / (plotW - 1);
    display_draw_line(&(tft), plotX + x, jammingdetector_s_wavePrevY[i0],
                      plotX + x + 1, jammingdetector_s_wavePrevY[i1],
                      waveColor);
  }

  jammingdetector_s_waveHasPrev = true;
  jammingdetector_jdDrawPlotGridLines();
}

typedef struct jammingdetector_WindowStat {
  int peakDbm;
  int minDbm;
  float duty;
  uint32_t elapsedMs;
} jammingdetector_WindowStat;
#define jammingdetector_WindowStat_INITIALIZER {0}
;

static jammingdetector_WindowStat jammingdetector_sampleWindow(void) {
  const int busyThresh =
      DIV_MAX(jammingdetector_JD_ABS_THRESH_DBM,
              (int)(jammingdetector_noiseFloor + jammingdetector_JD_MARGIN_DB));
  int peak = -127, lo = 0;
  uint16_t busy = 0;
  const float kEwmaAlpha = 0.35f;

  const uint32_t t0 = sys_monotonic_ms();
  uint32_t micro_s = sys_monotonic_us();
  for (int i = 0; i < jammingdetector_JD_SAMPLES; i++) {
    const int dbm = cc1101_get_rssi(&(cc1101));
    if (dbm > peak) peak = dbm;
    if (dbm < lo) lo = dbm;
    if (dbm > busyThresh) busy++;

    jammingdetector_waveSmooth =
        (kEwmaAlpha * dbm) + ((1.0f - kEwmaAlpha) * jammingdetector_waveSmooth);
    if ((i & 1) == 0) {
      jammingdetector_waveBuf[jammingdetector_waveWrite] = (int8_t)DIV_CLAMP(
          (int)lroundf(jammingdetector_waveSmooth),
          jammingdetector_kWaveRssiMin, jammingdetector_kWaveRssiMax);
      jammingdetector_waveWrite =
          (jammingdetector_waveWrite + 1) % jammingdetector_kWaveW;
    }

    while (sys_monotonic_us() < micro_s + jammingdetector_samplingPeriod) {
    }
    micro_s += jammingdetector_samplingPeriod;
  }

  jammingdetector_WindowStat st = jammingdetector_WindowStat_INITIALIZER;
  st.peakDbm = peak;
  st.minDbm = lo;
  st.duty = (float)busy / jammingdetector_JD_SAMPLES;
  st.elapsedMs = sys_monotonic_ms() - t0;
  return st;
}

static void jammingdetector_evaluate(const jammingdetector_WindowStat* st) {
  if ((*st).duty < 0.2f)
    jammingdetector_noiseFloor =
        0.95f * jammingdetector_noiseFloor + 0.05f * (*st).minDbm;

  jammingdetector_dutyRing[jammingdetector_dutyRingPos] = (*st).duty;
  jammingdetector_dutyRingPos =
      (jammingdetector_dutyRingPos + 1) % jammingdetector_JD_RING;
  float avgDuty = 0;
  for (uint8_t i = 0; i < jammingdetector_JD_RING; i++)
    avgDuty += jammingdetector_dutyRing[i];
  avgDuty /= jammingdetector_JD_RING;

  if ((*st).duty >= jammingdetector_JD_BUSY_WIN_DUTY)
    jammingdetector_busyStreakMs += (*st).elapsedMs;
  else
    jammingdetector_busyStreakMs = 0;

  const bool jam =
      (jammingdetector_busyStreakMs >= jammingdetector_JD_JAM_STREAK_MS) ||
      (avgDuty >= jammingdetector_JD_JAM_AVG_DUTY);

  if (jam && !jammingdetector_jamActive) {
    jammingdetector_jamActive = true;
    jammingdetector_jamStartMs = sys_monotonic_ms();
    jammingdetector_jamPeakDbm = (*st).peakDbm;
    jammingdetector_eventCount++;
    jammingdetector_s_disp.events = 0xFFFFFFFFu;
  } else if (jam && jammingdetector_jamActive) {
    if ((*st).peakDbm > jammingdetector_jamPeakDbm)
      jammingdetector_jamPeakDbm = (*st).peakDbm;
  } else if (!jam && jammingdetector_jamActive) {
    jammingdetector_jamActive = false;
    jammingdetector_logEvent(jammingdetector_jamStartMs,
                             sys_monotonic_ms() - jammingdetector_jamStartMs,
                             jammingdetector_jamPeakDbm, (int)(avgDuty * 100));
  }
}

static bool jammingdetector_edge(int pin, bool* prev) {
  const bool now = isPhysicalButtonPressed(pin);
  const bool e = now && !(*prev);
  (*prev) = now;
  return e;
}

static void jammingdetector_handleInput(void) {
  const bool navFreqDown =
      featureHasTouchNavBar() && isTouchNavButtonPressedEdge(BTN_LEFT);
  const bool navFreqUp =
      featureHasTouchNavBar() && isTouchNavButtonPressedEdge(BTN_RIGHT);
  const bool navReset =
      featureHasTouchNavBar() && isTouchNavButtonPressedEdge(BTN_UP);
  const bool navLog =
      featureHasTouchNavBar() && isTouchNavButtonPressedEdge(BTN_DOWN);

  if (jammingdetector_edge(BTN_LEFT, &(jammingdetector_prevLeft)) ||
      navFreqDown)
    jammingdetector_tuneTo(jammingdetector_freqIdx +
                           jammingdetector_kFreqCount - 1);
  if (jammingdetector_edge(BTN_RIGHT, &(jammingdetector_prevRight)) ||
      navFreqUp)
    jammingdetector_tuneTo(jammingdetector_freqIdx + 1);
  if (jammingdetector_edge(BTN_UP, &(jammingdetector_prevUp)) || navReset) {
    jammingdetector_jdResetStats();
  }
  if (jammingdetector_edge(BTN_DOWN, &(jammingdetector_prevDown)) || navLog) {
    jammingdetector_logEnabled = !jammingdetector_logEnabled;
    jammingdetector_s_disp.logOn = !jammingdetector_logEnabled;
  }
}

static void jammingdetector_exitCleanup(void) {
  cc1101_set_sidle(&(cc1101));
  refreshSdMount();
}

void jammingdetector_Setup(void) {
  setTouchButtonInputEnabled(true);
  setTouchNavLabels("Freq-", "Log", "Exit", "Reset", "Freq+");

#if defined(SD_CS)
  sys_configure_gpio(SD_CS, GPIO_MODE_OUTPUT);
  gpio_set_level(((gpio_num_t)(SD_CS)), 1);
#endif
#if defined(CC1101_CS)
  sys_configure_gpio(CC1101_CS, GPIO_MODE_OUTPUT);
  gpio_set_level(((gpio_num_t)(CC1101_CS)), 1);
#endif

  jammingdetector_cc1101BeginRx();
  jammingdetector_tuneTo(jammingdetector_freqIdx);

  jammingdetector_samplingPeriod =
      round(1000000.0 * (1.0 / jammingdetector_JD_SAMPLE_HZ));

  jammingdetector_noiseFloor = jammingdetector_JD_FLOOR_INIT_DBM;
  jammingdetector_busyStreakMs = 0;
  jammingdetector_jamActive = false;
  jammingdetector_logMounted = false;
  for (uint8_t i = 0; i < jammingdetector_JD_RING; i++)
    jammingdetector_dutyRing[i] = 0;
  jammingdetector_prevLeft = jammingdetector_prevRight =
      jammingdetector_prevUp = jammingdetector_prevDown = false;
  jammingdetector_jdInvalidateAll();

#if HAS_PCF8574_BUTTONS
  pcf8574_configure_pin(button_input_device(), BTN_LEFT, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_RIGHT, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_UP, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_DOWN, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_SELECT, GPIO_MODE_INPUT);
#endif

  display_set_rotation(&(tft), TFT_ROTATION);
  subghzClearBody(TFT_BLACK);
  drawStatusBar(readBatteryVoltage(), true, false);
  subghzRedrawNavChrome();
  setupTouchscreen();
  jammingdetector_jdRunUI();
  jammingdetector_jdDrawStaticChrome();
  jammingdetector_jdDrawStatusBox(false, false);
}

void jammingdetector_Loop(void) {
  if (feature_active &&
      (feature_exit_requested || featureExitButtonPressed())) {
    jammingdetector_exitCleanup();
    feature_exit_requested = true;
    return;
  }

  maintainTouchNavBar();
  jammingdetector_jdRunUI();
  jammingdetector_handleInput();

  const jammingdetector_WindowStat st = jammingdetector_sampleWindow();
  jammingdetector_evaluate(&(st));

  const bool activity = !jammingdetector_jamActive &&
                        (st.duty >= jammingdetector_JD_ACTIVITY_DUTY);
  const int dutyPct =
      (int)(jammingdetector_dutyRing[(jammingdetector_dutyRingPos +
                                      jammingdetector_JD_RING - 1) %
                                     jammingdetector_JD_RING] *
            100.0f);

  jammingdetector_jdUpdateInfo(st.peakDbm, dutyPct);
  jammingdetector_jdDrawStatusBox(jammingdetector_jamActive, activity);
  jammingdetector_jdDrawWaveform(jammingdetector_jamActive, activity);
}

void stopSubGhzSession(void) {
  ook_radio_disable_receive(&(replayat_mySwitch));
  ook_radio_disable_receive(&(SavedProfile_mySwitch));
  if (cc1101_ready(&(cc1101))) cc1101_set_sidle(&(cc1101));
}
