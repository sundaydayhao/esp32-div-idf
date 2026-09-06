#include "ui_self_test.h"

#include "button_input.h"
#include "config.h"
#include "ducky.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gps.h"
#include "ir.h"
#include "rfid.h"
#include "shared.h"
#include "system_features.h"
#include "touch_ui.h"
#include "ui_utils.h"

static const char* UI_TEST_TAG = "esp32_div_ui_test";

extern int current_submenu_index;
extern void handleAboutPage(void);

enum { OTHER_LAYER_IR = 1 };
enum { OTHER_LAYER_RFID = 2 };
enum { OTHER_LAYER_GPS = 3 };
enum { TOOLS_IDX_TOUCH = 2 };
static volatile bool s_uiSelfTestExitArmed = false;
static volatile uint32_t s_uiSelfTestExitDeadline = 0;

static void uiSelfTestExitTask(void*) {
  for (;;) {
    if (s_uiSelfTestExitArmed &&
        (int32_t)(sys_monotonic_ms() - s_uiSelfTestExitDeadline) >= 0) {
      feature_exit_requested = true;
      s_uiSelfTestExitArmed = false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

static void uiSelfTestArmExit(uint32_t ms) {
  feature_exit_requested = false;
  s_uiSelfTestExitDeadline = sys_monotonic_ms() + ms;
  s_uiSelfTestExitArmed = true;
}

static void uiSelfTestDisarmExit(void) { s_uiSelfTestExitArmed = false; }

static void uiSelfTestRunLoopFeature(const char* path, int mainIdx, int subIdx,
                                     void (*setupFn)(), void (*loopFn)(),
                                     void (*exitFn)(), uint32_t runMs) {
  ESP_LOGI(UI_TEST_TAG, "START %s", path);
  AppUiTestHooks_openTopSubmenu(mainIdx);
  AppUiTestHooks_tapSubmenu(path, subIdx);
  current_submenu_index = subIdx;
  in_sub_menu = true;
  feature_active = true;
  feature_exit_requested = false;
  setTouchButtonInputEnabled(true);
  setupFn();
  uiSelfTestArmExit(runMs);
  const uint32_t start = sys_monotonic_ms();
  while (!feature_exit_requested &&
         (uint32_t)(sys_monotonic_ms() - start) < runMs + 600u) {
    current_submenu_index = subIdx;
    in_sub_menu = true;
    feature_active = true;
    loopFn();
    sys_sleep_ms(3);
  }
  uiSelfTestDisarmExit();
  feature_exit_requested = true;
  if (exitFn) {
    exitFn();
  }
  ESP_LOGI(UI_TEST_TAG, "END %s elapsed=%lu", path,
           (unsigned long)(sys_monotonic_ms() - start));
  AppUiTestHooks_resetToMenu();
}

static void uiSelfTestRunOtherLoopFeature(const char* path, uint8_t layer,
                                          int homeIdx, int subIdx,
                                          void (*setupFn)(), void (*loopFn)(),
                                          uint32_t runMs) {
  ESP_LOGI(UI_TEST_TAG, "START %s", path);
  AppUiTestHooks_openOtherLayer(layer, homeIdx, path);
  AppUiTestHooks_tapSubmenu(path, subIdx);
  current_submenu_index = subIdx;
  in_sub_menu = true;
  feature_active = true;
  feature_exit_requested = false;
  setTouchButtonInputEnabled(true);
  setupFn();
  uiSelfTestArmExit(runMs);
  const uint32_t start = sys_monotonic_ms();
  while (!feature_exit_requested &&
         (uint32_t)(sys_monotonic_ms() - start) < runMs + 600u) {
    current_submenu_index = subIdx;
    in_sub_menu = true;
    feature_active = true;
    loopFn();
    sys_sleep_ms(3);
  }
  uiSelfTestDisarmExit();
  ESP_LOGI(UI_TEST_TAG, "END %s elapsed=%lu", path,
           (unsigned long)(sys_monotonic_ms() - start));
  AppUiTestHooks_resetToMenu();
}

static void uiSelfTestRunBlockingSession(const char* path, uint8_t layer,
                                         int homeIdx, int subIdx,
                                         void (*sessionFn)(),
                                         uint32_t timeoutMs) {
  ESP_LOGI(UI_TEST_TAG, "START %s", path);
  AppUiTestHooks_openOtherLayer(layer, homeIdx, path);
  AppUiTestHooks_tapSubmenu(path, subIdx);
  current_submenu_index = subIdx;
  in_sub_menu = true;
  feature_active = true;
  feature_exit_requested = false;
  setTouchButtonInputEnabled(true);
  uiSelfTestArmExit(timeoutMs);
  const uint32_t start = sys_monotonic_ms();
  sessionFn();
  uiSelfTestDisarmExit();
  ESP_LOGI(UI_TEST_TAG, "END %s elapsed=%lu", path,
           (unsigned long)(sys_monotonic_ms() - start));
  AppUiTestHooks_resetToMenu();
}

static void uiSelfTestRunRfidSession(const char* path, int subIdx,
                                     void (*sessionFn)()) {
  ESP_LOGI(UI_TEST_TAG, "START %s", path);
  AppUiTestHooks_openOtherLayer(OTHER_LAYER_RFID, 1, "RFID/NFC");
  AppUiTestHooks_tapSubmenu(path, subIdx);
  current_submenu_index = subIdx;
  in_sub_menu = true;
  feature_active = true;
  feature_exit_requested = false;
  setTouchButtonInputEnabled(true);
  if (!RfidNfc_begin()) {
    ESP_LOGW(UI_TEST_TAG, "SKIP %s: PN532 not found", path);
    refreshSdMount();
    AppUiTestHooks_resetToMenu();
    return;
  }
  uiSelfTestArmExit(2200);
  const uint32_t start = sys_monotonic_ms();
  sessionFn();
  uiSelfTestDisarmExit();
  refreshSdMount();
  ESP_LOGI(UI_TEST_TAG, "END %s elapsed=%lu", path,
           (unsigned long)(sys_monotonic_ms() - start));
  AppUiTestHooks_resetToMenu();
}

static void uiSelfTestRunTouchCalibration(void) {
  ESP_LOGI(UI_TEST_TAG, "START Tools/Touch Calibrate");
  AppUiTestHooks_openTopSubmenu(6);
  AppUiTestHooks_tapSubmenu("Tools/Touch Calibrate", TOOLS_IDX_TOUCH);
  current_submenu_index = TOOLS_IDX_TOUCH;
  in_sub_menu = true;
  feature_active = true;
  feature_exit_requested = false;
  TouchCalib_setup();
  const int16_t xs[4] = {300, 3800, 3800, 300};
  const int16_t ys[4] = {3800, 3800, 300, 300};
  for (int i = 0; i < 4 && !feature_exit_requested; ++i) {
    injectTouchRawXYForTest(xs[i], ys[i], 900, 3);
    TouchCalib_loop();
    clearInjectedTouchForTest();
    for (int r = 0; r < 4; ++r) {
      TouchCalib_loop();
    }
  }
  for (int i = 0; i < 4 && !feature_exit_requested; ++i) {
    TouchCalib_loop();
  }
  ESP_LOGI(UI_TEST_TAG, "END Tools/Touch Calibrate");
  AppUiTestHooks_resetToMenu();
}

void UiSelfTest_runOnce(void) {
  static bool s_exitTaskStarted = false;
  if (!s_exitTaskStarted) {
    xTaskCreate(uiSelfTestExitTask, "ui_test_exit", 2048, NULL, 5, NULL);
    s_exitTaskStarted = true;
  }

  ESP_LOGI(UI_TEST_TAG, "BEGIN full UI traversal");

  uiSelfTestRunLoopFeature("WiFi/Packet Monitor", 0, 0, PacketMonitor_ptmSetup,
                           PacketMonitor_ptmLoop, NULL, 1400);
  uiSelfTestRunLoopFeature("WiFi/Beacon Spammer", 0, 1,
                           BeaconSpammer_beaconSpamSetup,
                           BeaconSpammer_beaconSpamLoop, NULL, 1400);
  uiSelfTestRunLoopFeature("WiFi/WiFi Deauther", 0, 2, Deauther_deautherSetup,
                           Deauther_deautherLoop, NULL, 1400);
  uiSelfTestRunLoopFeature("WiFi/Probe Request Flood", 0, 3,
                           ProbeRequestFlood_probeRequestFloodSetup,
                           ProbeRequestFlood_probeRequestFloodLoop, NULL, 1400);
  uiSelfTestRunLoopFeature("WiFi/Deauth Detector", 0, 4,
                           DeauthDetect_deauthdetectSetup,
                           DeauthDetect_deauthdetectLoop, NULL, 1400);
  uiSelfTestRunLoopFeature("WiFi/WiFi Scanner", 0, 5, WifiScan_wifiscanSetup,
                           WifiScan_wifiscanLoop, NULL, 1400);
  uiSelfTestRunLoopFeature("WiFi/Captive Portal", 0, 6,
                           CaptivePortal_cportalSetup,
                           CaptivePortal_cportalLoop, NULL, 1400);

  uiSelfTestRunLoopFeature("2.4GHz/Scanner", 1, 0, Scanner_scannerSetup,
                           Scanner_scannerLoop, NULL, 1400);
  uiSelfTestRunLoopFeature("2.4GHz/Proto Kill", 1, 1, ProtoKill_prokillSetup,
                           ProtoKill_prokillLoop, NULL, 1400);

  uiSelfTestRunOtherLoopFeature("More/IR Remote/Record", OTHER_LAYER_IR, 0, 0,
                                IRRemoteFeature_setup, IRRemoteFeature_loop,
                                1400);
  uiSelfTestRunOtherLoopFeature("More/IR Remote/Saved Profile", OTHER_LAYER_IR,
                                0, 1, IRSavedProfile_setup, IRSavedProfile_loop,
                                1400);
  uiSelfTestRunOtherLoopFeature(
      "More/IR Remote/Universal Controller", OTHER_LAYER_IR, 0, 2,
      IRUniversalController_setup, IRUniversalController_loop, 1400);

  uiSelfTestRunRfidSession("More/RFID/Card Reader", 0,
                           RfidNfc_sessionCardReader);
  uiSelfTestRunRfidSession("More/RFID/Card Clone", 1, RfidNfc_sessionClone);
  uiSelfTestRunRfidSession("More/RFID/Erase", 2, RfidNfc_sessionErase);
  uiSelfTestRunRfidSession("More/RFID/Dump", 3, RfidNfc_sessionDump);
  uiSelfTestRunRfidSession("More/RFID/Decode Access", 4,
                           RfidNfc_sessionDecodeAccess);
  uiSelfTestRunRfidSession("More/RFID/Jam Reader", 5, RfidNfc_sessionJamReader);
  uiSelfTestRunRfidSession("More/RFID/Tag Disrupt", 6,
                           RfidNfc_sessionTagDisrupt);
  uiSelfTestRunRfidSession("More/RFID/Disrupt Emulate", 7,
                           RfidNfc_sessionDisruptEmulate);

  uiSelfTestRunBlockingSession("More/GPS/Wardriver", OTHER_LAYER_GPS, 2, 0,
                               GpsWardriver_session, 3200);
  uiSelfTestRunBlockingSession("More/GPS/Satellite Scanner", OTHER_LAYER_GPS, 2,
                               1, GpsSatelliteScanner_session, 2600);

  uiSelfTestRunLoopFeature("Bluetooth/BLE Jammer", 4, 0, BleJammer_blejamSetup,
                           BleJammer_blejamLoop, BleJammer_exit, 1400);
  uiSelfTestRunLoopFeature("Bluetooth/BLE Spoofer", 4, 1,
                           BleSpoofer_spooferSetup, BleSpoofer_spooferLoop,
                           BleSpoofer_exit, 1400);
  uiSelfTestRunLoopFeature("Bluetooth/Sour Apple", 4, 2,
                           SourApple_sourappleSetup, SourApple_sourappleLoop,
                           SourApple_exit, 1400);
  uiSelfTestRunLoopFeature("Bluetooth/Sniffer", 4, 3,
                           BleSniffer_blesnifferSetup,
                           BleSniffer_blesnifferLoop, BleSniffer_exit, 1400);
  uiSelfTestRunLoopFeature("Bluetooth/BLE Scanner", 4, 4, BleScan_bleScanSetup,
                           BleScan_bleScanLoop, BleScan_exit, 1400);
  uiSelfTestRunLoopFeature("Bluetooth/BLE Rubber Ducky", 4, 5, Ducky_enter,
                           Ducky_loop, Ducky_exit, 1400);

  uiSelfTestRunLoopFeature("SubGHz/Replay Attack", 5, 0,
                           replayat_ReplayAttackSetup,
                           replayat_ReplayAttackLoop, NULL, 1400);
  uiSelfTestRunLoopFeature("SubGHz/SubGHz Jammer", 5, 1,
                           subjammer_subjammerSetup, subjammer_subjammerLoop,
                           NULL, 1400);
  uiSelfTestRunLoopFeature("SubGHz/Saved Profile", 5, 2, SavedProfile_saveSetup,
                           SavedProfile_saveLoop, NULL, 1400);

  uiSelfTestRunLoopFeature("Tools/Serial Console", 6, 0, Terminal_terminalSetup,
                           Terminal_terminalLoop, NULL, 1400);
  uiSelfTestRunLoopFeature("Tools/Update Firmware", 6, 1,
                           FirmwareUpdate_updateSetup,
                           FirmwareUpdate_updateLoop, NULL, 1400);
  uiSelfTestRunTouchCalibration();
  uiSelfTestRunLoopFeature("Tools/SD File Manager", 6, 3, SdFileManager_setup,
                           SdFileManager_loop, NULL, 1400);

  ESP_LOGI(UI_TEST_TAG, "START Settings");
  AppUiTestHooks_tapMain(3);
  feature_active = true;
  feature_exit_requested = false;
  AppSettingsUI_setup();
  uiSelfTestArmExit(1400);
  while (!feature_exit_requested) {
    AppSettingsUI_loop();
    sys_sleep_ms(3);
  }
  uiSelfTestDisarmExit();
  AppUiTestHooks_resetToMenu();
  ESP_LOGI(UI_TEST_TAG, "END Settings");

  ESP_LOGI(UI_TEST_TAG, "START About");
  AppUiTestHooks_tapMain(7);
  uiSelfTestArmExit(900);
  handleAboutPage();
  uiSelfTestDisarmExit();
  AppUiTestHooks_resetToMenu();
  ESP_LOGI(UI_TEST_TAG, "END About");

  ESP_LOGI(UI_TEST_TAG, "DONE full UI traversal");
}

void UiSelfTest_armExit(uint32_t ms) { uiSelfTestArmExit(ms); }

void UiSelfTest_disarmExit(void) { uiSelfTestDisarmExit(); }

void UiSelfTest_pollAutoRun(void) {
#if CONFIG_ESP32_DIV_UI_SELF_TEST_AUTO_RUN
  static bool s_uiSelfTestDone = false;
  if (!s_uiSelfTestDone && sys_monotonic_ms() > 4000) {
    s_uiSelfTestDone = true;
    UiSelfTest_runOnce();
  }
#endif
}
