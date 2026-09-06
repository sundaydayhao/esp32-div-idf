#include "app.h"

#include "ble_service.h"
#include "board_services.h"
#include "button_input.h"
#include "config.h"
#include "display.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "ducky.h"
#include "esp_log.h"
#include "feature_lifecycle.h"
#include "feature_text.h"
#include "gps.h"
#include "icon.h"
#include "ir.h"
#include "pcf8574.h"
#include "rfid.h"
#include "settings_store.h"
#include "shared.h"
#include "spi_device.h"
#include "stdio.h"
#include "system_features.h"
#include "touch_ui.h"
#include "ui_self_test.h"
#include "ui_utils.h"

static void drawTouchNavBar(void);
void handleButtons(void);

display_t tft = DISPLAY_INITIALIZER;

static const char* TAG = "esp32_div";
static const char* TOUCH_TAG = "esp32_div_touch_hit";
static bool s_backlightPwmReady = false;

static void initBuzzerIdle(void) {
#if BUZZER_PIN >= 0
  gpio_set_direction((gpio_num_t)BUZZER_PIN, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)BUZZER_PIN, BUZZER_INACTIVE_LEVEL ? 1 : 0);
#endif
}

static void initBacklightPwm(void) {
  if (BACKLIGHT_PIN < 0 || s_backlightPwmReady) {
    return;
  }

  const ledc_timer_config_t timerConfig = {
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .duty_resolution = LEDC_TIMER_8_BIT,
      .timer_num = LEDC_TIMER_0,
      .freq_hz = PWM_FREQ,
      .clk_cfg = LEDC_AUTO_CLK,
      .deconfigure = false,
  };
  ESP_ERROR_CHECK(ledc_timer_config(&timerConfig));

  const ledc_channel_config_t channelConfig = {
      .gpio_num = BACKLIGHT_PIN,
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .channel = LEDC_CHANNEL_0,
      .intr_type = LEDC_INTR_DISABLE,
      .timer_sel = LEDC_TIMER_0,
      .duty = 0,
      .hpoint = 0,
      .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
      .flags = {.output_invert = 0},
  };
  ESP_ERROR_CHECK(ledc_channel_config(&channelConfig));
  s_backlightPwmReady = true;
}

void setBrightness(uint8_t value) {
#ifdef BACKLIGHT_PCF_PIN
  pcf8574_write_pin(button_input_device(), BACKLIGHT_PCF_PIN, value > 0);
  return;
#endif
  if (BACKLIGHT_PIN < 0) {
    return;
  }
  if (!s_backlightPwmReady) {
    initBacklightPwm();
  }
  ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, value));
  ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
}

bool feature_exit_requested = false;

enum { NUM_MENU_ITEMS = 8 };
const char* menu_items[NUM_MENU_ITEMS] = {"WiFi",     "2.4GHz",    "More",
                                          "Settings", "Bluetooth", "SubGHz",
                                          "Tools",    "About"};

const unsigned char* bitmap_icons[NUM_MENU_ITEMS] = {
    bitmap_icon_wifi,    bitmap_icon_jammer,  bitmap_icon_dialog,
    bitmap_icon_setting, bitmap_icon_spoofer, bitmap_icon_analyzer,
    bitmap_icon_stat,    bitmap_icon_question};

int current_menu_index = 0;
bool is_main_menu = false;

enum { NUM_SUBMENU_ITEMS = 12 };
const char* submenu_items[NUM_SUBMENU_ITEMS] = {
    "Packet Monitor",      "Beacon Spammer",  "WiFi Deauther",
    "Probe Request Flood", "Deauth Detector", "WiFi Scanner",
    "Captive Portal",      "Hidden SSID",     "WPS Scanner",
    "ARP Scanner",         "Karma",           "Back"};

enum { bluetooth_NUM_SUBMENU_ITEMS = 10 };
const char* bluetooth_submenu_items[bluetooth_NUM_SUBMENU_ITEMS] = {
    "BLE Jammer",     "BLE Spoofer",
    "Sour Apple",     "Sniffer",
    "BLE Scanner",    "BLE Rubber Ducky",
    "AirTag Spoofer", "AirTag Sniffer",
    "BLE Skimmer",    "Back"};

enum { nrf_NUM_SUBMENU_ITEMS = 7 };
const char* nrf_submenu_items[nrf_NUM_SUBMENU_ITEMS] = {
    "Scanner",        "Proto Kill",       "ESB Sniffer", "ESB Replay",
    "MouseJack Scan", "MouseJack Inject", "Back"};

enum { subghz_NUM_SUBMENU_ITEMS = 6 };
const char* subghz_submenu_items[subghz_NUM_SUBMENU_ITEMS] = {
    "Replay Attack", "SubGHz Jammer",    "Saved Profile",
    "SubGHz Brute",  "Jamming Detector", "Back"};

enum { tools_NUM_SUBMENU_ITEMS = 5 };
const char* tools_submenu_items[tools_NUM_SUBMENU_ITEMS] = {
    "Serial Console", "Update Firmware", "Touch Calibrate", "SD File Manager",
    "Back"};

enum { OTHER_LAYER_HOME = 0 };
enum { OTHER_LAYER_IR = 1 };
enum { OTHER_LAYER_RFID = 2 };
enum { OTHER_LAYER_GPS = 3 };

enum { other_NUM_SUBMENU_ITEMS = 4 };
enum { OTHER_GRID_COLS = 2 };
const char* other_submenu_items[other_NUM_SUBMENU_ITEMS] = {
    "IR Remote", "RFID/NFC", "GPS", "Main Menu"};

enum { rfid_NUM_SUBMENU_ITEMS = 9 };
const char* rfid_submenu_items[rfid_NUM_SUBMENU_ITEMS] = {
    "Card Reader", "Card Clone",      "Erase",
    "Dump",        "Decode Access",   "Jam Reader",
    "Tag Disrupt", "Disrupt Emulate", "Back to Main Menu"};

enum { gps_NUM_SUBMENU_ITEMS = 3 };
const char* gps_submenu_items[gps_NUM_SUBMENU_ITEMS] = {
    "Wardriver", "Satellite Scanner", "Back to Main Menu"};

enum { ir_NUM_SUBMENU_ITEMS = 4 };
const char* ir_submenu_items[ir_NUM_SUBMENU_ITEMS] = {
    "Record", "Saved Profile", "Universal Controller", "Back to Main Menu"};

enum { about_NUM_SUBMENU_ITEMS = 1 };
const char* about_submenu_items[about_NUM_SUBMENU_ITEMS] = {
    "Back to Main Menu"};

enum { setting_NUM_SUBMENU_ITEMS = 1 };
const char* setting_submenu_items[setting_NUM_SUBMENU_ITEMS] = {
    "Back to Main Menu"};

int current_submenu_index = 0;
bool in_sub_menu = false;
uint8_t other_layer = OTHER_LAYER_HOME;
int last_other_menu_index = -1;
bool other_menu_grid_initialized = false;

const char** active_submenu_items = NULL;
int active_submenu_size = 0;

const unsigned char* wifi_submenu_icons[NUM_SUBMENU_ITEMS] = {
    bitmap_icon_list, bitmap_icon_list, bitmap_icon_list, bitmap_icon_list,
    bitmap_icon_list, bitmap_icon_list, bitmap_icon_list, bitmap_icon_list,
    bitmap_icon_list, bitmap_icon_list, bitmap_icon_list, bitmap_icon_go_back};

const unsigned char* bluetooth_submenu_icons[bluetooth_NUM_SUBMENU_ITEMS] = {
    bitmap_icon_list, bitmap_icon_list,   bitmap_icon_list, bitmap_icon_list,
    bitmap_icon_list, bitmap_icon_list,   bitmap_icon_list, bitmap_icon_list,
    bitmap_icon_list, bitmap_icon_go_back};

const unsigned char* nrf_submenu_icons[nrf_NUM_SUBMENU_ITEMS] = {
    bitmap_icon_list, bitmap_icon_list, bitmap_icon_list,   bitmap_icon_list,
    bitmap_icon_list, bitmap_icon_list, bitmap_icon_go_back};

const unsigned char* subghz_submenu_icons[subghz_NUM_SUBMENU_ITEMS] = {
    bitmap_icon_list, bitmap_icon_list, bitmap_icon_list,
    bitmap_icon_list, bitmap_icon_list, bitmap_icon_go_back};

const unsigned char* tools_submenu_icons[tools_NUM_SUBMENU_ITEMS] = {
    bitmap_icon_list, bitmap_icon_list, bitmap_icon_list, bitmap_icon_list,
    bitmap_icon_go_back};

const unsigned char* other_submenu_icons[other_NUM_SUBMENU_ITEMS] = {
    bitmap_icon_led, bitmap_icon_rfid_chip, bitmap_icon_satellite,
    bitmap_icon_go_back};

const unsigned char* rfid_submenu_icons[rfid_NUM_SUBMENU_ITEMS] = {
    bitmap_icon_magnifying_glass,
    bitmap_icon_follow,
    bitmap_icon_recycle,
    bitmap_icon_dot_matrix,
    bitmap_icon_key,
    bitmap_icon_kill,
    bitmap_icon_flash,
    bitmap_icon_devil,
    bitmap_icon_go_back};

const unsigned char* gps_submenu_icons[gps_NUM_SUBMENU_ITEMS] = {
    bitmap_icon_satellite, bitmap_icon_satellite_dish, bitmap_icon_go_back};

const unsigned char* ir_submenu_icons[ir_NUM_SUBMENU_ITEMS] = {
    bitmap_icon_led, bitmap_icon_list, bitmap_icon_remote_control,
    bitmap_icon_go_back};

const unsigned char* about_submenu_icons[about_NUM_SUBMENU_ITEMS] = {
    bitmap_icon_go_back};

const unsigned char* setting_submenu_icons[setting_NUM_SUBMENU_ITEMS] = {
    bitmap_icon_go_back};

const unsigned char** active_submenu_icons = NULL;

void updateActiveSubmenu(void) {
  switch (current_menu_index) {
    case 0:
      active_submenu_items = submenu_items;
      active_submenu_size = NUM_SUBMENU_ITEMS;
      active_submenu_icons = wifi_submenu_icons;
      break;
    case 1:
      active_submenu_items = nrf_submenu_items;
      active_submenu_size = nrf_NUM_SUBMENU_ITEMS;
      active_submenu_icons = nrf_submenu_icons;
      break;
    case 2:
      if (other_layer == OTHER_LAYER_HOME) {
        active_submenu_items = other_submenu_items;
        active_submenu_size = other_NUM_SUBMENU_ITEMS;
        active_submenu_icons = other_submenu_icons;
      } else if (other_layer == OTHER_LAYER_IR) {
        active_submenu_items = ir_submenu_items;
        active_submenu_size = ir_NUM_SUBMENU_ITEMS;
        active_submenu_icons = ir_submenu_icons;
      } else if (other_layer == OTHER_LAYER_RFID) {
        active_submenu_items = rfid_submenu_items;
        active_submenu_size = rfid_NUM_SUBMENU_ITEMS;
        active_submenu_icons = rfid_submenu_icons;
      } else if (other_layer == OTHER_LAYER_GPS) {
        active_submenu_items = gps_submenu_items;
        active_submenu_size = gps_NUM_SUBMENU_ITEMS;
        active_submenu_icons = gps_submenu_icons;
      } else {
        active_submenu_items = other_submenu_items;
        active_submenu_size = other_NUM_SUBMENU_ITEMS;
        active_submenu_icons = other_submenu_icons;
      }
      break;
    case 3:
      active_submenu_items = NULL;
      active_submenu_size = 0;
      active_submenu_icons = NULL;
      break;
    case 4:
      active_submenu_items = bluetooth_submenu_items;
      active_submenu_size = bluetooth_NUM_SUBMENU_ITEMS;
      active_submenu_icons = bluetooth_submenu_icons;
      break;
    case 5:
      active_submenu_items = subghz_submenu_items;
      active_submenu_size = subghz_NUM_SUBMENU_ITEMS;
      active_submenu_icons = subghz_submenu_icons;
      break;
    case 6:
      active_submenu_items = tools_submenu_items;
      active_submenu_size = tools_NUM_SUBMENU_ITEMS;
      active_submenu_icons = tools_submenu_icons;
      break;
    case 7:
      active_submenu_items = NULL;
      active_submenu_size = 0;
      active_submenu_icons = NULL;
      break;

    default:
      active_submenu_items = NULL;
      active_submenu_size = 0;
      active_submenu_icons = NULL;
      break;
  }
}

static bool touchButtonInputEnabled = false;
static bool touchButtonCueDrawn = false;
static bool s_touchNavLabelsConfigured = false;
static bool s_touchNavHeld[5] = {false, false, false, false, false};
static bool s_touchNavSampleValid = false;
static int s_touchNavSampleSlot = -1;
static uint32_t s_touchNavSampleMs = 0;
#if HAS_PCF8574_BUTTONS
static bool s_pcfButtonLastState[8] = {true, true, true, true,
                                       true, true, true, true};
#endif
static FeatureUI_Button s_touchNavBtns[5];
static const char* s_touchNavLabels[5] = {NULL, NULL, NULL, NULL, NULL};
enum {
  TOUCH_NAV_BAR_H = (FeatureUI_FOOTER_H * 4) / 5
};  // 20% shorter than footer
enum { TOUCH_NAV_SAMPLE_CACHE_MS = 20 };

static int touchNavPinForIndex(int idx) {
  switch (idx) {
    case 0:
      return BTN_LEFT;
    case 1:
      return BTN_DOWN;
    case 2:
      return BTN_SELECT;
    case 3:
      return BTN_UP;
    case 4:
      return BTN_RIGHT;
    default:
      return -1;
  }
}

static void invalidateTouchNavSampleCache(void) {
  s_touchNavSampleValid = false;
  s_touchNavSampleSlot = -1;
  s_touchNavSampleMs = 0;
}

void setTouchButtonInputEnabled(bool enabled) {
  if (touchButtonInputEnabled != enabled) {
    touchButtonCueDrawn = false;
    invalidateTouchNavSampleCache();
    if (!enabled) {
      for (int i = 0; i < 5; ++i) {
        s_touchNavLabels[i] = NULL;
      }
      s_touchNavLabelsConfigured = false;
      for (int i = 0; i < 5; ++i) {
        s_touchNavHeld[i] = false;
      }
    }
  }
  touchButtonInputEnabled = enabled;
#if TOUCH_BUTTON_CUE_ENABLED
  if (enabled && feature_active) {
    drawTouchNavBar();
    touchButtonCueDrawn = true;
  }
#endif
}

bool featureHasTouchNavBar(void) {
#if TOUCH_BUTTON_CUE_ENABLED
  return touchButtonInputEnabled && feature_active;
#else
  return false;
#endif
}

void setTouchNavLabels(const char* left, const char* down, const char* center,
                       const char* up, const char* right) {
  s_touchNavLabels[0] = left;
  s_touchNavLabels[1] = down;
  s_touchNavLabels[2] = center;
  s_touchNavLabels[3] = up;
  s_touchNavLabels[4] = right;
  s_touchNavLabelsConfigured = true;
  invalidateTouchButtonCue();
}

void invalidateTouchButtonCue(void) { touchButtonCueDrawn = false; }

void resetTouchNavHeldState(void) {
  for (int i = 0; i < 5; ++i) {
    s_touchNavHeld[i] = false;
  }
  invalidateTouchNavSampleCache();
}

void redrawTouchButtonBar(void) {
  invalidateTouchButtonCue();
  drawTouchButtonCue();
}

static void layoutTouchNavBtns(void) {
  const int barY = display_height(&(tft)) - TOUCH_NAV_BAR_H;
  const int barH = TOUCH_NAV_BAR_H;
  const int totalW = display_width(&(tft));
  const int cellW = totalW / 5;

  for (int i = 0; i < 5; ++i) {
    const int x = i * cellW;
    const int w = (i == 4) ? (totalW - x) : cellW;
    s_touchNavBtns[i] = (FeatureUI_Button){
        (int16_t)x,    (int16_t)barY, (int16_t)w,
        (int16_t)barH, NULL,          FeatureUI_ButtonStyle_Secondary,
        false};
  }
}

static text_t fitTouchNavLabel(const char* label, int maxWidth) {
  if (!label || !label[0]) {
    return (text_t){0};
  }
  text_t out = TEXT(label);
  if (display_text_width(&(tft), (out).data, 0) <= maxWidth) {
    return out;
  }
  while (strlen((out).data) > 1 &&
         display_text_width(&(tft), (text_join_cstr((out).data, "...")).data,
                            0) > maxWidth) {
    text_erase(&(out), strlen((out).data) - 1, SIZE_MAX);
  }
  return text_join_cstr((out).data, "...");
}

static void drawTouchNavBar(void) {
  static const unsigned char* kIcons[5] = {
      bitmap_icon_LEFT, bitmap_icon_DOWN,  bitmap_icon_go_back,
      bitmap_icon_UP,   bitmap_icon_RIGHT,
  };
  enum { kIconSize = 16 };

  const int barY = display_height(&(tft)) - TOUCH_NAV_BAR_H;
  const int barH = TOUCH_NAV_BAR_H;
  const int barW = display_width(&(tft));

  layoutTouchNavBtns();

  display_fill_rect(&(tft), 0, barY, barW, barH, UI_FG);
  display_draw_fast_h_line(&(tft), 0, barY, barW, UI_LINE);

  for (int i = 0; i < 5; ++i) {
    __auto_type b = &(s_touchNavBtns[i]);
    if (i > 0) {
      display_draw_fast_v_line(&(tft), (*b).x, barY + 3, barH - 6, UI_LINE);
    }

    if (s_touchNavLabels[i] && s_touchNavLabels[i][0]) {
      display_set_text_datum(&(tft), MC_DATUM);
      const uint16_t txtColor = (i == 2) ? UI_ICON : UI_TEXT;
      display_set_text_color_2(&(tft), txtColor, UI_FG);
      text_t fit = fitTouchNavLabel(s_touchNavLabels[i], (*b).w - 8);
      display_draw_string(&(tft), (fit).data, (*b).x + (*b).w / 2,
                          (*b).y + (*b).h / 2, 1);
    } else {
      const int ix = (*b).x + ((*b).w - kIconSize) / 2;
      const int iy = (*b).y + ((*b).h - kIconSize) / 2;
      const bool inactiveSlot =
          s_touchNavLabelsConfigured && !s_touchNavLabels[i];
      const unsigned char* icon = inactiveSlot ? bitmap_icon_dots : kIcons[i];
      const uint16_t iconColor =
          inactiveSlot ? LIGHT_GRAY : ((i == 2) ? UI_ICON : UI_TEXT);
      display_draw_bitmap(&(tft), ix, iy, icon, kIconSize, kIconSize,
                          iconColor);
    }
  }

  display_set_text_datum(&(tft), TL_DATUM);
  display_set_text_font(&(tft), 1);
  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), UI_TEXT, FEATURE_BG);
}

static int sampleTouchNavSlot(void) {
  if (!feature_active || !touchButtonInputEnabled) {
    invalidateTouchNavSampleCache();
    return -1;
  }

  const uint32_t now = sys_monotonic_ms();
  if (s_touchNavSampleValid &&
      (uint32_t)(now - s_touchNavSampleMs) <= TOUCH_NAV_SAMPLE_CACHE_MS) {
    return s_touchNavSampleSlot;
  }

  int slot = -1;
  int x = 0;
  int y = 0;
  if (readTouchXYDismiss(&(x), &(y))) {
    layoutTouchNavBtns();
    const int stripTop = display_height(&(tft)) - TOUCH_NAV_BAR_H;
    if (y >= stripTop) {
      slot = FeatureUI_hit(s_touchNavBtns, 5, x, y);
    }
  }

  s_touchNavSampleSlot = slot;
  s_touchNavSampleMs = now;
  s_touchNavSampleValid = true;
  return slot;
}

void maintainTouchNavBar(void) {
#if TOUCH_BUTTON_CUE_ENABLED
  if (!touchButtonInputEnabled || !feature_active || touchButtonCueDrawn) {
    return;
  }
  drawTouchNavBar();
  touchButtonCueDrawn = true;
#endif
}

void featureClearContent(uint16_t color) {
  const int bottom = touchNavContentBottomY();
  if (bottom > 0) {
    display_fill_rect(&(tft), 0, 0, display_width(&(tft)), bottom, color);
  } else {
    display_fill_screen(&(tft), color);
  }
}

int16_t touchNavReservedHeight(void) {
#if TOUCH_BUTTON_CUE_ENABLED
  if (touchButtonInputEnabled && feature_active) {
    return TOUCH_NAV_BAR_H;
  }
#endif
  return 0;
}

int16_t touchNavContentBottomY(void) {
  return (int16_t)(display_height(&(tft)) - touchNavReservedHeight());
}

void drawTouchButtonCue(void) {
#if TOUCH_BUTTON_CUE_ENABLED
  if (!touchButtonInputEnabled || !feature_active) {
    return;
  }
  drawTouchNavBar();
  touchButtonCueDrawn = true;
#endif
}

static int touchNavIndexForPin(int buttonPin) {
  for (int i = 0; i < 5; ++i) {
    if (touchNavPinForIndex(i) == buttonPin) {
      return i;
    }
  }
  return -1;
}

static bool isTouchNavSlotDown(int idx) {
  if (idx < 0 || !feature_active || !touchButtonInputEnabled) {
    return false;
  }

  return sampleTouchNavSlot() == idx;
}

bool isPhysicalButtonPressed(int buttonPin) {
  return button_input_pressed(buttonPin);
}

bool isTouchNavButtonPressed(int buttonPin) {
  const int idx = touchNavIndexForPin(buttonPin);
  if (idx < 0) {
    return false;
  }
  return isTouchNavSlotDown(idx);
}

bool isButtonPressed(int buttonPin) {
  if (isPhysicalButtonPressed(buttonPin)) {
    logPcf8574ButtonSnapshot("level", buttonPin);
    return true;
  }
  return isTouchNavButtonPressed(buttonPin);
}

bool isTouchNavButtonPressedEdge(int buttonPin) {
  if (!feature_active || !touchButtonInputEnabled) {
    return false;
  }

  const int navIdx = touchNavIndexForPin(buttonPin);
  if (navIdx < 0) {
    return false;
  }

  const bool down = isTouchNavSlotDown(navIdx);
  const bool edge = down && !s_touchNavHeld[navIdx];
  s_touchNavHeld[navIdx] = down;
  return edge;
}

bool isButtonPressedEdge(int buttonPin) {
#if HAS_PCF8574_BUTTONS
  if (getPcf8574Address() != 0) {
    const int idx = buttonPin % 8;
    const bool cur = pcf8574_read_pin(button_input_device(), buttonPin);
    const bool edge = !cur && s_pcfButtonLastState[idx];
    s_pcfButtonLastState[idx] = cur;
    if (edge) {
      logPcf8574ButtonSnapshot("edge", buttonPin);
      return true;
    }
  }
#endif

  return isTouchNavButtonPressedEdge(buttonPin);
}

bool featureExitButtonPressed(void) {
  return isPhysicalButtonPressed(BTN_SELECT) ||
         isTouchNavButtonPressed(BTN_SELECT);
}

static void showFeatureUnavailable(const char* featureName,
                                   const char* requirement) {
  feature_active = false;
  feature_exit_requested = false;
  showNotification(featureName, requirement);
  sys_sleep_ms(250);
}

float currentBatteryVoltage = 0;
unsigned long last_interaction_time = 0;

int last_submenu_index = -1;
bool submenu_initialized = false;
int last_menu_index = -1;
bool menu_initialized = false;
enum { Y_START = 30 };
enum { Y_SPACING = 75 };
enum { MENU_TILE_W = 100 };
enum { MENU_TILE_H = 60 };
enum { MAIN_MENU_ROWS = 4 };
enum { MENU_TOUCH_PAD_X = 10 };
enum { MENU_TOUCH_PAD_Y = 12 };

static int menuGridLeftX(void) {
  const int screenW = display_width(&(tft));
  const int gap = (screenW > 240) ? 40 : 20;
  int x = (screenW - MENU_TILE_W * 2 - gap) / 2;
  return (x > 4) ? x : 4;
}

static int menuGridRightX(void) {
  return menuGridLeftX() + MENU_TILE_W +
         ((display_width(&(tft)) > 240) ? 40 : 20);
}

static int menuGridSpacingY(int rows) {
  if (rows <= 1) {
    return Y_SPACING;
  }
  enum { contentTop = 30 };
  const int contentBottom = display_height(&(tft)) - 12;
  const int availableStep =
      (contentBottom - contentTop - MENU_TILE_H) / (rows - 1);
  if (display_height(&(tft)) <= 320) {
    return Y_SPACING;
  }
  if (availableStep < Y_SPACING) {
    return Y_SPACING;
  }
  return (availableStep > 105) ? 105 : availableStep;
}

static int menuGridTopY(int rows) {
  const int contentBottom = display_height(&(tft));
  const int legacyTop = Y_START;
  const int totalH = MENU_TILE_H + menuGridSpacingY(rows) * (rows - 1);
  const int centered = 20 + ((contentBottom - 20 - totalH) / 2);
  return (display_height(&(tft)) > 320 && centered > legacyTop) ? centered
                                                                : legacyTop;
}

static int menuTileX(int column) {
  return (column == 0) ? menuGridLeftX() : menuGridRightX();
}

static int menuTileY(int row, int rows) {
  return menuGridTopY(rows) + row * menuGridSpacingY(rows);
}

void displayMenu(void);
void displaySubmenu(void);
void displayOtherMenuGrid(void);
void handleAboutPage(void);
void handleSettingsSubmenuButtons(void);

static void launchCurrentMainMenuItem(void) {
  ESP_LOGI(TOUCH_TAG, "launch main menu idx=%d main=%d feature=%d in_sub=%d",
           current_menu_index, (int)is_main_menu, (int)feature_active,
           (int)in_sub_menu);
  if (current_menu_index == 3) {
    handleSettingsSubmenuButtons();
    return;
  }
  if (current_menu_index == 7) {
    handleAboutPage();
    return;
  }

  pauseBackgroundRadioTasks();
  updateActiveSubmenu();
  if (active_submenu_items && active_submenu_size > 0) {
    current_submenu_index = 0;
    if (current_menu_index == 2) {
      other_layer = OTHER_LAYER_HOME;
      other_menu_grid_initialized = false;
      last_other_menu_index = -1;
    }
    in_sub_menu = true;
    is_main_menu = false;
    submenu_initialized = false;
    displaySubmenu();
  }
}

static void waitForTouchRelease(uint32_t timeoutMs) {
  const uint32_t start = sys_monotonic_ms();
  while (isTouchDown(120) && (sys_monotonic_ms() - start < timeoutMs)) {
    sys_sleep_ms(10);
  }
}

// Wait until a physical button is released before entering a feature.
// Without this, the just-pressed SELECT is still held when the feature's
// loop starts and is read as an immediate "exit", bouncing back to the menu.
static bool waitForButtonRelease(int buttonPin, uint32_t timeoutMs) {
  const uint32_t start = sys_monotonic_ms();
  while (isPhysicalButtonPressed(buttonPin) &&
         (sys_monotonic_ms() - start < timeoutMs)) {
    sys_sleep_ms(10);
  }
  const bool released = !isPhysicalButtonPressed(buttonPin);
#if HAS_PCF8574_BUTTONS
  if (released && buttonPin >= 0) {
    s_pcfButtonLastState[buttonPin % 8] = true;
  }
#endif
  return released;
}

static void returnToMainMenu(void) {
  displayMenu();
  waitForButtonRelease(BTN_SELECT, 600);
  waitForTouchRelease(600);
}

static bool navButtonPressed(int buttonPin, const char* action) {
  if (!isButtonPressedEdge(buttonPin)) {
    return false;
  }

  ESP_LOGI(TAG,
           "button ui action=%s main_idx=%d sub_idx=%d main=%d in_sub=%d "
           "feature=%d other_layer=%u",
           action ? action : "unknown", current_menu_index,
           current_submenu_index, (int)is_main_menu, (int)in_sub_menu,
           (int)feature_active, (unsigned)other_layer);
  return true;
}

enum { kMenuRows = 8 };
enum { kMenuRowY = 30 };
enum { kMenuRowH = 30 };
static int submenuPage(void) { return current_submenu_index / kMenuRows; }
void displaySubmenu(void) {
  in_sub_menu = true;
  is_main_menu = false;
  feature_active = false;
  feature_exit_requested = false;
  setTouchButtonInputEnabled(false);
  if (current_menu_index == 2 && other_layer == OTHER_LAYER_HOME) {
    displayOtherMenuGrid();
    return;
  }
  menu_initialized = false;
  last_menu_index = -1;
  if (submenu_initialized && last_submenu_index == current_submenu_index)
    return;
  display_fill_screen(&(tft), UI_BG);
  display_set_text_font(&(tft), 1);
  display_set_text_size(&(tft), 1);
  const int first = submenuPage() * kMenuRows;
  for (int i = first; i < DIV_MIN(first + kMenuRows, active_submenu_size);
       ++i) {
    const int y = kMenuRowY + (i - first) * kMenuRowH;
    const bool selected = i == current_submenu_index;
    const uint16_t color = selected ? UI_ICON : UI_TEXT;
    display_draw_bitmap(&(tft), 10, y + 5, active_submenu_icons[i], 16, 16,
                        color);
    if (selected)
      display_draw_round_rect(&(tft), 3, y, display_width(&(tft)) - 6,
                              kMenuRowH - 2, 4, UI_ICON);
    display_set_text_color_2(&(tft), color, UI_BG);
    display_set_cursor(&(tft), 32, y + 8);
    display_print(&(tft), (TEXT(active_submenu_items[i])).data);
  }
  display_set_text_color_2(&(tft), UI_DIM_TEXT, UI_BG);
  display_set_cursor(&(tft), 12, 288);
  display_print(&(tft), "< PREV");
  display_set_cursor(&(tft), 90, 288);
  display_printf(&(tft), "%d / %d", submenuPage() + 1,
                 (active_submenu_size + kMenuRows - 1) / kMenuRows);
  display_set_cursor(&(tft), 182, 288);
  display_print(&(tft), "NEXT >");
  drawStatusBar(currentBatteryVoltage, true, false);
  submenu_initialized = true;
  last_submenu_index = current_submenu_index;
}

void displayOtherMenuGrid(void) {
  applyThemeToPalette((*settings()).theme);
  const int gridRows =
      (other_NUM_SUBMENU_ITEMS + OTHER_GRID_COLS - 1) / OTHER_GRID_COLS;

  submenu_initialized = false;
  last_submenu_index = -1;
  menu_initialized = false;
  last_menu_index = -1;

  display_set_text_font(&(tft), 1);

  if (!other_menu_grid_initialized) {
    display_fill_screen(&(tft), UI_BG);

    for (int i = 0; i < other_NUM_SUBMENU_ITEMS; i++) {
      int column = i % OTHER_GRID_COLS;
      int row = i / OTHER_GRID_COLS;
      int x_position = menuTileX(column);
      int y_position = menuTileY(row, gridRows);

      display_fill_round_rect(&(tft), x_position, y_position, MENU_TILE_W,
                              MENU_TILE_H, 5, UI_FG);
      display_draw_round_rect(&(tft), x_position, y_position, MENU_TILE_W,
                              MENU_TILE_H, 5, UI_LINE);
      display_draw_bitmap(&(tft), x_position + (MENU_TILE_W - 16) / 2,
                          y_position + 10, other_submenu_icons[i], 16, 16,
                          UI_ICON);

      display_set_text_color_2(&(tft), UI_TEXT, UI_FG);
      int textWidth = display_text_width(&(tft), other_submenu_items[i], 0);
      int textX = x_position + (MENU_TILE_W - textWidth) / 2;
      int textY = y_position + 30;
      display_set_cursor(&(tft), textX, textY);
      display_print(&(tft), (TEXT(other_submenu_items[i])).data);
    }

    other_menu_grid_initialized = true;
    last_other_menu_index = -1;
  }

  if (last_other_menu_index != current_submenu_index) {
    for (int i = 0; i < other_NUM_SUBMENU_ITEMS; i++) {
      int column = i % OTHER_GRID_COLS;
      int row = i / OTHER_GRID_COLS;
      int x_position = menuTileX(column);
      int y_position = menuTileY(row, gridRows);

      if (i == last_other_menu_index) {
        display_fill_round_rect(&(tft), x_position, y_position, MENU_TILE_W,
                                MENU_TILE_H, 5, UI_FG);
        display_draw_round_rect(&(tft), x_position, y_position, MENU_TILE_W,
                                MENU_TILE_H, 5, UI_LINE);
        display_set_text_color_2(&(tft), UI_TEXT, UI_FG);
        display_draw_bitmap(
            &(tft), x_position + (MENU_TILE_W - 16) / 2, y_position + 10,
            other_submenu_icons[last_other_menu_index], 16, 16, UI_ICON);
        int textWidth = display_text_width(
            &(tft), other_submenu_items[last_other_menu_index], 0);
        int textX = x_position + (MENU_TILE_W - textWidth) / 2;
        int textY = y_position + 30;
        display_set_cursor(&(tft), textX, textY);
        display_print(&(tft),
                      (TEXT(other_submenu_items[last_other_menu_index])).data);
      }
    }

    int column = current_submenu_index % OTHER_GRID_COLS;
    int row = current_submenu_index / OTHER_GRID_COLS;
    int x_position = menuTileX(column);
    int y_position = menuTileY(row, gridRows);

    display_fill_round_rect(&(tft), x_position, y_position, MENU_TILE_W,
                            MENU_TILE_H, 5, UI_FG);
    display_draw_round_rect(&(tft), x_position, y_position, MENU_TILE_W,
                            MENU_TILE_H, 5, UI_ICON);

    display_set_text_color_2(&(tft), UI_ICON, UI_FG);
    display_draw_bitmap(&(tft), x_position + (MENU_TILE_W - 16) / 2,
                        y_position + 10,
                        other_submenu_icons[current_submenu_index], 16, 16,
                        SELECTED_ICON_COLOR);
    int textWidth = display_text_width(
        &(tft), other_submenu_items[current_submenu_index], 0);
    int textX = x_position + (MENU_TILE_W - textWidth) / 2;
    int textY = y_position + 30;
    display_set_cursor(&(tft), textX, textY);
    display_print(&(tft),
                  (TEXT(other_submenu_items[current_submenu_index])).data);

    last_other_menu_index = current_submenu_index;
  }

  drawStatusBar(currentBatteryVoltage, true, false);
}

/** Main menu "Other" tile (index 2): triple preview icons (LED / satellite /
 * dots). */
enum { MAIN_MENU_OTHER_IDX = 2 };
enum { MAIN_MENU_OTHER_ICON_GAP = 4 };

static void drawMainMenuOtherTripleIcons(int x_position, int y_position,
                                         uint16_t iconColor) {
  const int tripleW = 16 * 3 + MAIN_MENU_OTHER_ICON_GAP * 2;
  int ix = x_position + (MENU_TILE_W - tripleW) / 2;
  const int iy = y_position + 10;
  display_draw_bitmap(&(tft), ix, iy, bitmap_icon_led, 16, 16, iconColor);
  display_draw_bitmap(&(tft), ix + 16 + MAIN_MENU_OTHER_ICON_GAP, iy,
                      bitmap_icon_satellite, 16, 16, iconColor);
  display_draw_bitmap(&(tft), ix + 32 + MAIN_MENU_OTHER_ICON_GAP * 2, iy,
                      bitmap_icon_down_dots, 16, 16, iconColor);
}

static uint16_t contrastColorFor565(uint16_t color) {
  const uint8_t r = (uint8_t)((((color >> 11) & 0x1F) * 255) / 31);
  const uint8_t g = (uint8_t)((((color >> 5) & 0x3F) * 255) / 63);
  const uint8_t b = (uint8_t)(((color & 0x1F) * 255) / 31);
  const uint16_t luma = (uint16_t)(r * 3 + g * 6 + b);
  return (luma >= 1280) ? BLACK : WHITE;
}

static void drawMainMenuTile(int index, bool selected,
                             const uint16_t* iconColors) {
  const int column = index / 4;
  const int row = index % 4;
  const int x_position = menuTileX(column);
  const int y_position = menuTileY(row, MAIN_MENU_ROWS);
  const uint16_t fill = selected ? UI_ICON : UI_FG;
  const uint16_t selectedFg = contrastColorFor565(fill);
  const uint16_t edge = selected ? selectedFg : UI_LINE;
  const uint16_t fg = selected ? selectedFg : UI_TEXT;
  const uint16_t icon = selected ? selectedFg : iconColors[index];

  display_fill_round_rect(&(tft), x_position, y_position, MENU_TILE_W,
                          MENU_TILE_H, 5, fill);
  display_draw_round_rect(&(tft), x_position, y_position, MENU_TILE_W,
                          MENU_TILE_H, 5, edge);

  if (index == MAIN_MENU_OTHER_IDX) {
    drawMainMenuOtherTripleIcons(x_position, y_position, icon);
  } else {
    display_draw_bitmap(&(tft), x_position + (MENU_TILE_W - 16) / 2,
                        y_position + 10, bitmap_icons[index], 16, 16, icon);
  }

  display_set_text_color_2(&(tft), fg, fill);
  int textWidth = display_text_width(&(tft), menu_items[index], 0);
  int textX = x_position + (MENU_TILE_W - textWidth) / 2;
  int textY = y_position + 30;
  display_set_cursor(&(tft), textX, textY);
  display_print(&(tft), (TEXT(menu_items[index])).data);
}

void displayMenu(void) {
  feature_active = false;
  feature_exit_requested = false;
  in_sub_menu = false;
  is_main_menu = true;
  setTouchButtonInputEnabled(false);
  applyThemeToPalette((*settings()).theme);
  ESP_LOGI(TAG, "draw main menu idx=%d last=%d full=%d", current_menu_index,
           last_menu_index, (int)!menu_initialized);

  const uint16_t icon_colors[NUM_MENU_ITEMS] = {
      UI_ICON, UI_ICON, UI_ICON, UI_ICON, UI_ICON, UI_ICON, UI_ICON, UI_ICON};

  submenu_initialized = false;
  last_submenu_index = -1;
  other_menu_grid_initialized = false;
  last_other_menu_index = -1;
  display_set_text_font(&(tft), 1);

  if (!menu_initialized) {
    display_fill_screen(&(tft), UI_BG);

    for (int i = 0; i < NUM_MENU_ITEMS; i++) {
      drawMainMenuTile(i, false, icon_colors);
    }
    menu_initialized = true;
    last_menu_index = -1;
  }

  if (last_menu_index != current_menu_index) {
    if (last_menu_index >= 0 && last_menu_index < NUM_MENU_ITEMS) {
      drawMainMenuTile(last_menu_index, false, icon_colors);
    }

    if (current_menu_index >= 0 && current_menu_index < NUM_MENU_ITEMS) {
      drawMainMenuTile(current_menu_index, true, icon_colors);
      last_menu_index = current_menu_index;
    }
  }
  drawStatusBar(currentBatteryVoltage, true, false);
}

static void redrawMainMenu(void) { displayMenu(); }

static void otherDismissSession(void) {
  sys_sleep_ms(25);
  while (isButtonPressed(BTN_SELECT) || isButtonPressed(BTN_LEFT)) {
    sys_sleep_ms(5);
  }
  while (!isButtonPressed(BTN_SELECT) && !isButtonPressed(BTN_LEFT)) {
    int x = 0, y = 0;
    if (!readTouchXYDismiss(&(x), &(y)) && !readTouchXY(&(x), &(y))) {
      sys_sleep_ms(12);
      continue;
    }
    if (isNotificationVisible()) {
      NotificationAction a = notificationHandleTouch(x, y);
      if (a != NotificationAction_None) {
        break;
      }
      hideNotification();
    }
    break;
  }
  if (in_sub_menu) {
    submenu_initialized = false;
    if (current_menu_index == 2 && other_layer == OTHER_LAYER_HOME) {
      other_menu_grid_initialized = false;
      last_other_menu_index = -1;
    }
    displaySubmenu();
  }
}

static void otherRfidReturnGuard(void) {
  sys_sleep_ms(120);
  for (int i = 0; i < 120; i++) {
    if (!isButtonPressed(BTN_SELECT) && !isButtonPressed(BTN_LEFT) &&
        !isButtonPressed(BTN_RIGHT) && !isButtonPressed(BTN_UP) &&
        !isButtonPressed(BTN_DOWN) && !isTouchDownDismiss(120)) {
      break;
    }
    sys_sleep_ms(5);
  }
  sys_sleep_ms(120);
}

static void otherRfidSessionAction(int idx) {
  feature_active = true;
  if (!RfidNfc_begin()) {
    showNotification("RFID/NFC", "PN532 not found. Check SPI wiring/pins.");
    otherDismissSession();
    feature_active = false;
    return;
  }
  feature_exit_requested = false;
  setTouchButtonInputEnabled(true);
  for (;;) {
    RfidNfc_clearSessionRetry();
    switch (idx) {
      case 0:
        RfidNfc_sessionCardReader();
        break;
      case 1:
        RfidNfc_sessionClone();
        break;
      case 2:
        RfidNfc_sessionErase();
        break;
      case 3:
        RfidNfc_sessionDump();
        break;
      case 4:
        RfidNfc_sessionDecodeAccess();
        break;
      case 5:
        RfidNfc_sessionJamReader();
        break;
      case 6:
        RfidNfc_sessionTagDisrupt();
        break;
      case 7:
        RfidNfc_sessionDisruptEmulate();
        break;
      default:
        feature_active = false;
        refreshSdMount();
        return;
    }
    if (feature_exit_requested || !RfidNfc_consumeSessionRetry()) {
      break;
    }
    feature_exit_requested = false;
  }
  refreshSdMount();
  otherRfidReturnGuard();
  submenu_initialized = false;
  displaySubmenu();
  feature_active = false;
}

static void otherGpsSessionAction(int idx) {
  feature_active = true;
  if (idx == 0) {
    feature_exit_requested = false;
    setTouchButtonInputEnabled(true);
    for (;;) {
      GpsWardriver_clearSessionRetry();
      GpsWardriver_session();
      if (feature_exit_requested || !GpsWardriver_consumeSessionRetry()) {
        break;
      }
      feature_exit_requested = false;
    }
    setTouchButtonInputEnabled(false);
  } else {
    switch (idx) {
      case 1:
        GpsSatelliteScanner_session();
        break;
      default:
        feature_active = false;
        return;
    }
  }
  otherRfidReturnGuard();
  submenu_initialized = false;
  displaySubmenu();
  feature_active = false;
}

typedef struct Feature {
  void (*start)(void);
  void (*tick)(void);
  void (*stop)(void);
  bool select_exits;
} Feature;
#define Feature_INITIALIZER {0}
;
static void duckyStart(void) {
#if FEATURE_BLE_DUCKY
  Ducky_enter();
#else
  showFeatureUnavailable("BLE Rubber Ducky", "ESP32-S3 required");
  feature_exit_requested = true;
#endif
}
static void duckyTick(void) {
#if FEATURE_BLE_DUCKY
  Ducky_loop();
#endif
}
static void duckyStop(void) {
#if FEATURE_BLE_DUCKY
  Ducky_exit();
#endif
}
static const Feature submenu_items_features[] = {
    {PacketMonitor_ptmSetup, PacketMonitor_ptmLoop, NULL, true},
    {BeaconSpammer_beaconSpamSetup, BeaconSpammer_beaconSpamLoop, NULL, true},
    {Deauther_deautherSetup, Deauther_deautherLoop, NULL, true},
    {ProbeRequestFlood_probeRequestFloodSetup,
     ProbeRequestFlood_probeRequestFloodLoop, NULL, true},
    {DeauthDetect_deauthdetectSetup, DeauthDetect_deauthdetectLoop, NULL, true},
    {WifiScan_wifiscanSetup, WifiScan_wifiscanLoop, NULL, false},
    {CaptivePortal_cportalSetup, CaptivePortal_cportalLoop, NULL, false},
    {HiddenSsidReveal_hiddenSsidSetup, HiddenSsidReveal_hiddenSsidLoop, NULL,
     false},
    {WpsScanner_wpsScannerSetup, WpsScanner_wpsScannerLoop, NULL, false},
    {ArpScanner_arpScannerSetup, ArpScanner_arpScannerLoop, NULL, false},
    {KarmaAttack_karmaSetup, KarmaAttack_karmaLoop, NULL, false},
};
static const Feature bluetooth_submenu_items_features[] = {
    {BleJammer_blejamSetup, BleJammer_blejamLoop, BleJammer_exit, true},
    {BleSpoofer_spooferSetup, BleSpoofer_spooferLoop, BleSpoofer_exit, true},
    {SourApple_sourappleSetup, SourApple_sourappleLoop, SourApple_exit, true},
    {BleSniffer_blesnifferSetup, BleSniffer_blesnifferLoop, BleSniffer_exit,
     true},
    {BleScan_bleScanSetup, BleScan_bleScanLoop, BleScan_exit, false},
    {duckyStart, duckyTick, duckyStop, false},
    {AirTagSpoofer_airTagSetup, AirTagSpoofer_airTagLoop, AirTagSpoofer_exit,
     false},
    {AirTagSniffer_airTagSnifferSetup, AirTagSniffer_airTagSnifferLoop,
     AirTagSniffer_exit, false},
    {BleSkimmer_bleSkimmerSetup, BleSkimmer_bleSkimmerLoop, BleSkimmer_exit,
     false},
};
static const Feature nrf_submenu_items_features[] = {
    {Scanner_scannerSetup, Scanner_scannerLoop, NULL, true},
    {ProtoKill_prokillSetup, ProtoKill_prokillLoop, NULL, true},
    {EsbSniffer_esbSnifferSetup, EsbSniffer_esbSnifferLoop, EsbSniffer_exit,
     false},
    {EsbReplay_esbReplaySetup, EsbReplay_esbReplayLoop, EsbReplay_exit, false},
    {MouseJack_mouseJackSetup, MouseJack_mouseJackLoop, MouseJack_exit, false},
    {MouseJackInject_mouseJackInjectSetup, MouseJackInject_mouseJackInjectLoop,
     MouseJackInject_exit, false},
};
static const Feature subghz_submenu_items_features[] = {
    {replayat_ReplayAttackSetup, replayat_ReplayAttackLoop, NULL, true},
    {subjammer_subjammerSetup, subjammer_subjammerLoop, NULL, true},
    {SavedProfile_saveSetup, SavedProfile_saveLoop, NULL, true},
    {SubBrute_subBruteSetup, SubBrute_subBruteLoop, NULL, false},
    {jammingdetector_Setup, jammingdetector_Loop, NULL, false},
};
static const Feature tools_submenu_items_features[] = {
    {Terminal_terminalSetup, Terminal_terminalLoop, NULL, false},
    {FirmwareUpdate_updateSetup, FirmwareUpdate_updateLoop, NULL, false},
    {TouchCalib_setup, TouchCalib_loop, NULL, false},
    {SdFileManager_setup, SdFileManager_loop, NULL, false},
};
static const Feature ir_features[] = {
    {IRRemoteFeature_setup, IRRemoteFeature_loop, NULL, false},
    {IRSavedProfile_setup, IRSavedProfile_loop, NULL, false},
    {IRUniversalController_setup, IRUniversalController_loop, NULL, false}};
static const Feature* selectedFeature(void) {
  switch (current_menu_index) {
    case 0:
      return &submenu_items_features[current_submenu_index];
    case 1:
      return &nrf_submenu_items_features[current_submenu_index];
    case 4:
      return &bluetooth_submenu_items_features[current_submenu_index];
    case 5:
      return &subghz_submenu_items_features[current_submenu_index];
    case 6:
      return &tools_submenu_items_features[current_submenu_index];
    case 2:
      if (other_layer == OTHER_LAYER_IR)
        return &ir_features[current_submenu_index];
  }
  return NULL;
}
static void finishFeature(void) {
  // Stop feature-owned work before allowing the background scanners to resume.
  if (!GpsWardriver_statusBarGpsIconActive()) {
    stopForegroundWifi();
    stopForegroundRadios();
  }
  stopSubGhzSession();
  stopIrSession();
  spi_device_release_all();
  hideNotification();
  setTouchButtonInputEnabled(false);
  setTouchNavLabels(NULL, NULL, NULL, NULL, NULL);
  resetTouchNavHeldState();
  waitForButtonRelease(BTN_SELECT, 800);
  waitForTouchRelease(500);
  feature_active = false;
  feature_exit_requested = false;
  submenu_initialized = false;
  displaySubmenu();
}
static void launchSubmenuItem(void) {
  feature_text_reset();
  if (current_submenu_index < 0 || current_submenu_index >= active_submenu_size)
    return;
  if (current_menu_index == 2 && other_layer == OTHER_LAYER_HOME) {
    if (current_submenu_index == other_NUM_SUBMENU_ITEMS - 1) {
      returnToMainMenu();
      return;
    }
    other_layer = current_submenu_index + 1;
    current_submenu_index = 0;
    updateActiveSubmenu();
    submenu_initialized = false;
    displaySubmenu();
    return;
  }
  if (current_submenu_index == active_submenu_size - 1) {
    if (current_menu_index == 2) {
      other_layer = OTHER_LAYER_HOME;
      current_submenu_index = 0;
      updateActiveSubmenu();
      submenu_initialized = false;
      displaySubmenu();
    } else
      returnToMainMenu();
    return;
  }
  waitForTouchRelease(500);
  if (!waitForButtonRelease(BTN_SELECT, 3000)) return;
  feature_active = true;
  feature_exit_requested = false;
  pauseBackgroundRadioTasks();
  const bool calibration =
      current_menu_index == 6 && current_submenu_index == 2;
  setTouchButtonInputEnabled(!calibration);
  const Feature* feature = selectedFeature();
  if (current_menu_index == 2 && other_layer == OTHER_LAYER_RFID) {
    otherRfidSessionAction(current_submenu_index);
  } else if (current_menu_index == 2 && other_layer == OTHER_LAYER_GPS) {
    otherGpsSessionAction(current_submenu_index);
  } else if (feature) {
    feature->start();
    while (feature_active && !feature_exit_requested) {
      feature->tick();
      maintainTouchNavBar();
      updateStatusBar();
      if (feature->select_exits && featureExitButtonPressed())
        feature_exit_requested = true;
      sys_sleep_ms(1);
    }
    if (feature->stop) feature->stop();
  }
  finishFeature();
}
static void changeMenuPage(int delta) {
  int pages = (active_submenu_size + kMenuRows - 1) / kMenuRows;
  current_submenu_index = ((submenuPage() + delta + pages) % pages) * kMenuRows;
  submenu_initialized = false;
  displaySubmenu();
}
static void handleSubmenu(void) {
  const bool grid = current_menu_index == 2 && other_layer == OTHER_LAYER_HOME;
  if (navButtonPressed(BTN_UP, "submenu_up")) {
    current_submenu_index =
        (current_submenu_index + active_submenu_size - 1) % active_submenu_size;
    displaySubmenu();
  } else if (navButtonPressed(BTN_DOWN, "submenu_down")) {
    current_submenu_index = (current_submenu_index + 1) % active_submenu_size;
    displaySubmenu();
  } else if (navButtonPressed(BTN_LEFT, "submenu_prev")) {
    if (!grid) changeMenuPage(-1);
  } else if (navButtonPressed(BTN_RIGHT, "submenu_next")) {
    if (!grid) changeMenuPage(1);
  } else if (navButtonPressed(BTN_SELECT, "submenu_select")) {
    launchSubmenuItem();
    return;
  }
  int x, y;
  if (!readTouchXY(&(x), &(y))) return;
  waitForTouchRelease(500);
  if (!grid && y >= 275 && y < 318) {
    if (x < 80)
      changeMenuPage(-1);
    else if (x > 160)
      changeMenuPage(1);
    return;
  }
  int idx = -1;
  if (grid) {
    for (int i = 0; i < other_NUM_SUBMENU_ITEMS; ++i) {
      int tx = menuTileX(i % OTHER_GRID_COLS),
          ty = menuTileY(i / OTHER_GRID_COLS, 2);
      if (x >= tx - 10 && x <= tx + MENU_TILE_W + 10 && y >= ty - 12 &&
          y <= ty + MENU_TILE_H + 12) {
        idx = i;
        break;
      }
    }
  } else if (y >= kMenuRowY && y < kMenuRowY + kMenuRows * kMenuRowH) {
    idx = submenuPage() * kMenuRows + (y - kMenuRowY) / kMenuRowH;
  }
  if (idx >= 0 && idx < active_submenu_size) {
    current_submenu_index = idx;
    displaySubmenu();
    launchSubmenuItem();
  }
}

void handleAboutPage(void) {
  feature_active = true;
  feature_exit_requested = false;
  ESP_LOGI(TOUCH_TAG, "enter about page");
  waitForTouchRelease(800);
  waitForButtonRelease(BTN_SELECT, 800);
  waitForButtonRelease(BTN_LEFT, 800);

  display_fill_screen(&(tft), UI_BG);
  currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true, false);

  display_set_text_color_2(&(tft), UI_TEXT, UI_BG);
  display_set_text_size(&(tft), 1);
  display_set_text_font(&(tft), 1);

  const char* title = "[About This Project]";
  display_set_cursor(&(tft), 10, 42);
  display_print_line_cstr(&(tft), (TEXT(title)).data);

  int lineHeight = 12;
  int text_x = 10;
  int text_y = 62;
  display_set_cursor(&(tft), text_x, text_y);
  display_print(&(tft), "- ");
  tftPrintlnObf(OBF_PN, sizeof(OBF_PN), k0());
  text_y += lineHeight;
  display_set_cursor(&(tft), text_x, text_y);
  display_print(&(tft), "- Developed by: ");
  tftPrintlnObf(OBF_DN, sizeof(OBF_DN), k0());
  text_y += lineHeight;
  display_set_cursor(&(tft), text_x, text_y);
  display_print(&(tft), "- Version: ");
  display_print_line_cstr(&(tft), (TEXT(ESP32DIV_VERSION)).data);
  text_y += lineHeight;
  display_set_cursor(&(tft), text_x, text_y);
  display_print(&(tft), "- Contact: ");
  tftPrintlnObf(OBF_EM, sizeof(OBF_EM), k0());
  text_y += lineHeight;
  display_set_cursor(&(tft), text_x, text_y);
  display_print(&(tft), "- GitHub: ");
  tftPrintlnObf(OBF_GH, sizeof(OBF_GH), k0());
  text_y += lineHeight;
  display_set_cursor(&(tft), text_x, text_y);
  display_print(&(tft), "- Website: ");
  tftPrintlnObf(OBF_WB, sizeof(OBF_WB), k0());

  display_set_text_font(&(tft), 1);
  display_set_cursor(&(tft), 10, 300);
  display_print_line_cstr(&(tft), "Press SELECT / touch to go back");

  while (!feature_exit_requested) {
    if (isButtonPressed(BTN_SELECT) || isButtonPressed(BTN_LEFT)) {
      last_interaction_time = sys_monotonic_ms();
      ESP_LOGI(TOUCH_TAG, "about exit by button");
      feature_exit_requested = true;
      sys_sleep_ms(200);
      break;
    }

    int x, y;
    if (readTouchXY(&(x), &(y))) {
      last_interaction_time = sys_monotonic_ms();
      ESP_LOGI(TOUCH_TAG, "about exit by touch raw=(%d,%d)", x, y);
      feature_exit_requested = true;
      sys_sleep_ms(200);
      break;
    }

    sys_sleep_ms(20);
  }

  feature_active = false;
  feature_exit_requested = false;
  in_sub_menu = false;
  submenu_initialized = false;

  menu_initialized = false;
  last_menu_index = -1;
  is_main_menu = false;
  displayMenu();
}

void handleSettingsSubmenuButtons(void) {
  feature_active = true;
  feature_exit_requested = false;
  ESP_LOGI(TOUCH_TAG, "enter settings page");
  waitForTouchRelease(800);
  waitForButtonRelease(BTN_SELECT, 800);

  AppSettingsUI_setup();
  while (!feature_exit_requested) {
    AppSettingsUI_loop();
  }

  feature_active = false;
  feature_exit_requested = false;

  in_sub_menu = false;
  submenu_initialized = false;

  menu_initialized = false;
  last_menu_index = -1;
  is_main_menu = false;
  displayMenu();
}

void handleButtons(void) {
  if (is_main_menu && (feature_active || in_sub_menu)) {
    ESP_LOGW(TAG, "main menu state corrected feature=%d in_sub=%d idx=%d",
             (int)feature_active, (int)in_sub_menu, current_menu_index);
    feature_active = false;
    feature_exit_requested = false;
    in_sub_menu = false;
  }

  if (in_sub_menu) {
    handleSubmenu();
  } else {
    const bool on_main_menu = !feature_active && !in_sub_menu;

    if (on_main_menu && navButtonPressed(BTN_UP, "main_up")) {
      const int oldIndex = current_menu_index;
      current_menu_index--;
      if (current_menu_index < 0) {
        current_menu_index = NUM_MENU_ITEMS - 1;
      }
      ESP_LOGI(TAG, "main menu action=UP index %d -> %d", oldIndex,
               current_menu_index);
      last_interaction_time = sys_monotonic_ms();
      redrawMainMenu();
      sys_sleep_ms(200);
      return;
    }

    if (on_main_menu && navButtonPressed(BTN_DOWN, "main_down")) {
      const int oldIndex = current_menu_index;
      current_menu_index++;
      if (current_menu_index >= NUM_MENU_ITEMS) {
        current_menu_index = 0;
      }
      ESP_LOGI(TAG, "main menu action=DOWN index %d -> %d", oldIndex,
               current_menu_index);
      last_interaction_time = sys_monotonic_ms();
      redrawMainMenu();
      sys_sleep_ms(200);
      return;
    }

    if (on_main_menu && navButtonPressed(BTN_LEFT, "main_left")) {
      const int oldIndex = current_menu_index;
      int row = current_menu_index % 4;
      if (current_menu_index >= 4) {
        current_menu_index = row;
      } else if (current_menu_index == 0) {
        current_menu_index = 3;
      } else {
        current_menu_index = row - 1;
      }
      ESP_LOGI(TAG, "main menu action=LEFT index %d -> %d", oldIndex,
               current_menu_index);
      last_interaction_time = sys_monotonic_ms();
      redrawMainMenu();
      sys_sleep_ms(200);
      return;
    }

    if (on_main_menu && navButtonPressed(BTN_RIGHT, "main_right")) {
      const int oldIndex = current_menu_index;
      int row = current_menu_index % 4;
      if (current_menu_index < 4) {
        current_menu_index = row + 4;
      } else if (current_menu_index == 7) {
        current_menu_index = 0;
      } else {
        current_menu_index = row + 5;
      }
      ESP_LOGI(TAG, "main menu action=RIGHT index %d -> %d", oldIndex,
               current_menu_index);
      last_interaction_time = sys_monotonic_ms();
      redrawMainMenu();
      sys_sleep_ms(200);
      return;
    }

    if (on_main_menu && navButtonPressed(BTN_SELECT, "main_select")) {
      last_interaction_time = sys_monotonic_ms();
      if (!waitForButtonRelease(BTN_SELECT, 3000)) {
        return;
      }
      launchCurrentMainMenuItem();
      return;
    }

    static unsigned long lastTouchTime = 0;
    static unsigned long lastTouchPollTime = 0;
    const unsigned long touchFeedbackDelay = 100;
    const unsigned long touchPollInterval = 40;

    if (!feature_active &&
        (sys_monotonic_ms() - lastTouchTime >= touchFeedbackDelay)) {
      if (sys_monotonic_ms() - lastTouchPollTime < touchPollInterval) {
        return;
      }
      lastTouchPollTime = sys_monotonic_ms();
      int x, y;
      if (!readTouchXY(&(x), &(y))) {
        return;
      }
      sys_sleep_ms(10);
      for (int i = 0; i < NUM_MENU_ITEMS; i++) {
        int column = i / 4;
        int row = i % 4;
        int x_position = menuTileX(column);
        int y_position = menuTileY(row, MAIN_MENU_ROWS);

        int button_x1 = x_position - MENU_TOUCH_PAD_X;
        int button_y1 = y_position - MENU_TOUCH_PAD_Y;
        int button_x2 = x_position + MENU_TILE_W + MENU_TOUCH_PAD_X;
        int button_y2 = y_position + MENU_TILE_H + MENU_TOUCH_PAD_Y;

        if (x >= button_x1 && x <= button_x2 && y >= button_y1 &&
            y <= button_y2) {
          ESP_LOGI(TOUCH_TAG,
                   "main menu hit idx=%d raw=(%d,%d) box=[%d..%d,%d..%d] "
                   "main=%d feature=%d",
                   i, x, y, button_x1, button_x2, button_y1, button_y2,
                   (int)is_main_menu, (int)feature_active);
          current_menu_index = i;
          last_interaction_time = sys_monotonic_ms();
          displayMenu();
          waitForTouchRelease(600);
          launchCurrentMainMenuItem();

          lastTouchTime = sys_monotonic_ms();
          sys_sleep_ms(200);
          break;
        }
      }
    }
  }
}

void esp32_div_app_init(void) {
  ESP_LOGI(TAG, "boot start");

  ESP_ERROR_CHECK(board_init());
  initBuzzerIdle();
#if HAS_PCF8574_BUTTONS
  if (!initPcf8574Buttons()) {
    ESP_LOGW(TAG, "PCF8574 buttons unavailable");
  }
#else
  ESP_LOGI(TAG, "PCF8574 buttons disabled for this board");
#endif

  display_init(&(tft));
  ESP_LOGI(TAG, "tft init done");
  display_set_rotation(&(tft), TFT_ROTATION);

  initBacklightPwm();
  setBrightness(100);
  ESP_LOGI(TAG, "backlight ready");

  applyThemeToPalette((*settings()).theme);

  display_fill_screen(&(tft), TFT_BLACK);

  loading(100, UI_ICON, 0, 0, 2, true);
  ESP_LOGI(TAG, "loading animation done");

  display_fill_screen(&(tft), TFT_BLACK);

  displayLogo(TFT_WHITE, 500);
  ESP_LOGI(TAG, "logo done");

  initSDCard();
  ESP_LOGI(TAG, "sd init done");

  settingsLoad();
  applyThemeToPalette((*settings()).theme);
  setBrightness((*settings()).brightness);
  board_set_status_light((*settings()).neopixelEnabled, 0, 16, 24);
  ESP_LOGI(TAG, "settings loaded");

  ESP_LOGI(TAG, "BLE init");
  ble_service_init(ESP32DIV_NAME);
  ESP_LOGI(TAG, "BLE init done");

#if FEATURE_BLE_DUCKY
  Ducky_setup();
#endif

  ESP_LOGI(TAG, "background tasks");
  WifiScan_startBackgroundScanner();
  BleScan_startBackgroundScanner();
  ESP_LOGI(TAG, "background tasks done");

  ESP_LOGI(TAG, "draw menu");
  menu_initialized = false;
  currentBatteryVoltage = readBatteryVoltage();
  displayMenu();
  is_main_menu = true;
  drawStatusBar(currentBatteryVoltage, false, false);
  ESP_LOGI(TAG, "menu drawn");

  setupTouchscreen();
  ESP_LOGI(TAG, "touch ready");

  last_interaction_time = sys_monotonic_ms();
  ESP_LOGI(TAG, "ready");
}

void AppUiTestHooks_resetToMenu(void) {
  clearInjectedTouchForTest();
  feature_exit_requested = false;
  feature_active = false;
  in_sub_menu = false;
  is_main_menu = true;
  other_layer = OTHER_LAYER_HOME;
  setTouchButtonInputEnabled(false);
  setTouchNavLabels(NULL, NULL, NULL, NULL, NULL);
  resetTouchNavHeldState();
  menu_initialized = false;
  submenu_initialized = false;
  other_menu_grid_initialized = false;
  displayMenu();
  sys_sleep_ms(80);
}

void AppUiTestHooks_tapMain(int mainIdx) {
  current_menu_index = mainIdx;
  displayMenu();
  const int column = mainIdx / 4;
  const int row = mainIdx % 4;
  const int x = menuTileX(column) + MENU_TILE_W / 2;
  const int y = menuTileY(row, MAIN_MENU_ROWS) + MENU_TILE_H / 2;
  ESP_LOGI(TOUCH_TAG, "ui-test tap main[%d]=%s at %d,%d", mainIdx,
           menu_items[mainIdx], x, y);
  injectTouchScreenXYForTest(x, y, 1);
  sys_sleep_ms(60);
  clearInjectedTouchForTest();
}

void AppUiTestHooks_tapSubmenu(const char* groupName, int subIdx) {
  current_submenu_index = subIdx;
  submenu_initialized = false;
  displaySubmenu();
  const char* label =
      (active_submenu_items && subIdx >= 0 && subIdx < active_submenu_size)
          ? active_submenu_items[subIdx]
          : "?";
  int yPos = kMenuRowY + (subIdx % kMenuRows) * kMenuRowH;
  if (subIdx == active_submenu_size - 1) {
  }
  ESP_LOGI(TOUCH_TAG, "ui-test tap %s[%d]=%s at %d,%d",
           groupName ? groupName : "submenu", subIdx, label, 60, yPos + 12);
  injectTouchScreenXYForTest(60, yPos + 12, 1);
  sys_sleep_ms(60);
  clearInjectedTouchForTest();
}

void AppUiTestHooks_openTopSubmenu(int mainIdx) {
  AppUiTestHooks_tapMain(mainIdx);
  updateActiveSubmenu();
  current_submenu_index = 0;
  in_sub_menu = true;
  is_main_menu = false;
  submenu_initialized = false;
  displaySubmenu();
}

void AppUiTestHooks_openOtherLayer(uint8_t layer, int homeIdx,
                                   const char* name) {
  AppUiTestHooks_tapMain(2);
  other_layer = OTHER_LAYER_HOME;
  current_menu_index = 2;
  current_submenu_index = homeIdx;
  updateActiveSubmenu();
  in_sub_menu = true;
  submenu_initialized = false;
  displaySubmenu();
  const int gridRows =
      (other_NUM_SUBMENU_ITEMS + OTHER_GRID_COLS - 1) / OTHER_GRID_COLS;
  const int column = homeIdx % OTHER_GRID_COLS;
  const int row = homeIdx / OTHER_GRID_COLS;
  ESP_LOGI(TOUCH_TAG, "ui-test tap More/%s at %d,%d", name ? name : "layer",
           menuTileX(column) + MENU_TILE_W / 2,
           menuTileY(row, gridRows) + MENU_TILE_H / 2);
  sys_sleep_ms(60);
  other_layer = layer;
  current_submenu_index = 0;
  updateActiveSubmenu();
  submenu_initialized = false;
  displaySubmenu();
}

void esp32_div_app_run_once(void) {
  applyThemeToPalette((*settings()).theme);
  pollPcf8574ScanMode();
  pollPcf8574SelfTest();
  UiSelfTest_pollAutoRun();
  handleButtons();
  WifiScan_pollBackgroundScanner();
  BleScan_pollBackgroundScanner();
  updateStatusBar();
}

/* Foreground feature lifecycle is application state. */
bool feature_active = false;
