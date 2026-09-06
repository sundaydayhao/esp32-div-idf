#include "button_input.h"
#include "c_util.h"
#pragma once

#include <esp_event.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <nvs_flash.h>
#include <rf24.h>
#include <soc/soc_caps.h>

#include "console.h"
#include "display.h"
#include "nvs_records.h"
#include "ook_radio.h"
#include "touch_driver.h"

#if defined(CONFIG_BT_BLUEDROID_ENABLED) && CONFIG_BT_BLUEDROID_ENABLED
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#endif

#include "cc1101.h"
#include "pcf8574.h"
#include "shared.h"
#include "ui_utils.h"

extern display_t tft;

void BleJammer_blejamSetup(void);
void BleJammer_blejamLoop(void);
void BleJammer_exit(void);

void BleSpoofer_spooferSetup(void);
void BleSpoofer_spooferLoop(void);
void BleSpoofer_exit(void);

void SourApple_sourappleSetup(void);
void SourApple_sourappleLoop(void);
void SourApple_exit(void);

void BleScan_bleScanSetup(void);
void BleScan_bleScanLoop(void);
void BleScan_exit(void);
// Background BLE scanner: runs periodically in its own task, caching results.
void BleScan_startBackgroundScanner(void);
void BleScan_pauseBackgroundScanner(void);
void BleScan_pollBackgroundScanner(void);
// Last known BLE device count from background/foreground scans.
int BleScan_getLastCount(void);

void Scanner_scannerSetup(void);
void Scanner_scannerLoop(void);

void ProtoKill_prokillSetup(void);
void ProtoKill_prokillLoop(void);

void BleSniffer_blesnifferSetup(void);
void BleSniffer_blesnifferLoop(void);
void BleSniffer_exit(void);

void replayat_ReplayAttackSetup(void);
void replayat_ReplayAttackLoop(void);

void SavedProfile_saveSetup(void);
void SavedProfile_saveLoop(void);

void subjammer_subjammerSetup(void);
void subjammer_subjammerLoop(void);

void PacketMonitor_ptmSetup(void);
void PacketMonitor_ptmLoop(void);

void BeaconSpammer_beaconSpamSetup(void);
void BeaconSpammer_beaconSpamLoop(void);

void DeauthDetect_deauthdetectSetup(void);
void DeauthDetect_deauthdetectLoop(void);

void WifiScan_wifiscanSetup(void);
void WifiScan_wifiscanLoop(void);
// Background WiFi scanner: runs when no feature is active, caching results.
void WifiScan_startBackgroundScanner(void);
void WifiScan_pauseBackgroundScanner(void);
void WifiScan_pollBackgroundScanner(void);
// Last known WiFi network count from background/foreground scans.
int WifiScan_getLastCount(void);
bool WifiScan_wifiCacheValidForReuse(void);
int WifiScan_staWifiScanSync(void);
bool WifiScan_loadApListFromWifiCache(wifi_ap_record_t** ap_list,
                                      int* network_count,
                                      int (*compare_ap)(const void*,
                                                        const void*));

void CaptivePortal_cportalSetup(void);
void CaptivePortal_cportalLoop(void);

void Deauther_deautherSetup(void);
void Deauther_deautherLoop(void);

void ProbeRequestFlood_probeRequestFloodSetup(void);
void ProbeRequestFlood_probeRequestFloodLoop(void);

void FirmwareUpdate_updateSetup(void);
void FirmwareUpdate_updateLoop(void);

void HiddenSsidReveal_hiddenSsidSetup(void);
void HiddenSsidReveal_hiddenSsidLoop(void);

void WpsScanner_wpsScannerSetup(void);
void WpsScanner_wpsScannerLoop(void);

void ArpScanner_arpScannerSetup(void);
void ArpScanner_arpScannerLoop(void);

void KarmaAttack_karmaSetup(void);
void KarmaAttack_karmaLoop(void);

void AirTagSpoofer_airTagSetup(void);
void AirTagSpoofer_airTagLoop(void);
void AirTagSpoofer_exit(void);

void AirTagSniffer_airTagSnifferSetup(void);
void AirTagSniffer_airTagSnifferLoop(void);
void AirTagSniffer_exit(void);

void BleSkimmer_bleSkimmerSetup(void);
void BleSkimmer_bleSkimmerLoop(void);
void BleSkimmer_exit(void);

void EsbSniffer_esbSnifferSetup(void);
void EsbSniffer_esbSnifferLoop(void);
void EsbSniffer_exit(void);

void EsbReplay_esbReplaySetup(void);
void EsbReplay_esbReplayLoop(void);
void EsbReplay_exit(void);

void MouseJack_mouseJackSetup(void);
void MouseJack_mouseJackLoop(void);
void MouseJack_exit(void);

void MouseJackInject_mouseJackInjectSetup(void);
void MouseJackInject_mouseJackInjectLoop(void);
void MouseJackInject_exit(void);

void SubBrute_subBruteSetup(void);
void SubBrute_subBruteLoop(void);

void jammingdetector_Setup(void);
void jammingdetector_Loop(void);
