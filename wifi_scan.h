/*
 * wifi_scan.h — Dual-band Wi-Fi scanning, channel sorting, scanMutex & target selection
 * Part of WiFi-Deck multi-file refactor.
 */
#pragma once

#include <Arduino.h>
#include <vector>
#include "FreeRTOS.h"
#include "semphr.h"
#include "wifi_conf.h"
#include "wifi_structures.h"

//==========================================================
// Data Structures (fixed char buffers — zero per-AP heap allocations)
//==========================================================
struct WiFiScanResult {
  bool selected = false;
  char ssid[33] = {0};
  char bssid_str[18] = {0};
  uint8_t bssid[6] = {0};
  short rssi = 0;
  uint channel = 0;
};

enum WlanRuntimeMode {
  WLAN_MODE_UNINIT = 0,
  WLAN_MODE_AP,
  WLAN_MODE_STA,
  WLAN_MODE_PROMISC
};

#define MAX_SCAN_RESULTS 64

//==========================================================
// Global Vectors & Scan Mutex
//==========================================================
extern std::vector<WiFiScanResult> scan_results;
extern std::vector<WiFiScanResult> target_aps;
extern SemaphoreHandle_t scanMutex;

//==========================================================
// Scan State
//==========================================================
extern volatile bool scan_done;
extern volatile bool scan_accepting;
extern bool scan_enabled;
extern bool target_mode;

//==========================================================
// Attack & Telemetry Counters
//==========================================================
extern bool attack_enabled;
extern bool disassoc_enabled;
extern uint32_t tx_success_count;
extern uint32_t tx_fail_count;

//==========================================================
// WLAN Runtime Mode
//==========================================================
extern volatile WlanRuntimeMode wlanRuntimeMode;
extern bool wlanLwipInitialized;

//==========================================================
// Sliced non-blocking TX state for multi-target deauth / disassoc cycles
//==========================================================
struct SlicedTxCycle {
  bool active = false;
  size_t targetIdx = 0;
  unsigned long frameIdx = 0;
  bool aborted = false;
  std::vector<WiFiScanResult> targets;

  void reset() {
    active = false;
    targetIdx = 0;
    frameIdx = 0;
    aborted = false;
    targets.clear();
  }
};

// Async non-blocking BLE scan/sync push cursor (2A flow control)
struct BleScanPushState {
  bool active = false;
  bool emitBegin = false;
  bool isSync = false;
  bool beginSent = false;
  size_t idx = 0;
  unsigned long lastProgressAt = 0;
  std::vector<WiFiScanResult> snap;

  void reset() {
    active = false;
    emitBegin = false;
    isSync = false;
    beginSent = false;
    idx = 0;
    lastProgressAt = 0;
    snap.clear();
  }
};

extern SlicedTxCycle deauthSlice;
extern SlicedTxCycle disassocSlice;
extern BleScanPushState bleScanPush;

//==========================================================
// Flood / Jam Attack State
//==========================================================
extern bool beacon_flood_enabled;
extern unsigned long beacon_flood_delay;
extern unsigned long last_beacon_flood;
extern uint16_t beacon_flood_count;

extern bool probe_flood_enabled;
extern unsigned long probe_flood_delay;
extern unsigned long last_probe_flood;
extern uint16_t probe_flood_count;

extern bool signal_jam_enabled;
extern unsigned long signal_jam_hop_delay;
extern unsigned long last_signal_jam;
extern int signal_jam_ch_index;
extern uint16_t signal_jam_count;

//==========================================================
// Inline Mutex Helpers
//==========================================================
static inline bool lockScanMutex(TickType_t ticksToWait = portMAX_DELAY) {
  if (scanMutex == NULL) return true;
  return xSemaphoreTake(scanMutex, ticksToWait) == pdTRUE;
}

static inline void unlockScanMutex() {
  if (scanMutex != NULL) {
    xSemaphoreGive(scanMutex);
  }
}

//==========================================================
// Function Prototypes
//==========================================================
std::vector<WiFiScanResult> snapshotScanResults();
std::vector<WiFiScanResult> snapshotTargetAps();
std::vector<WiFiScanResult> snapshotDisassocTargets();

size_t getScanCountSafe();
size_t getTargetCountSafe();
void getScanAndTargetCountsSafe(size_t &scanCountOut, size_t &targetCountOut, bool &targetModeOut);
bool hasSelectedTargetsSafe();

rtw_result_t scanResultHandler(rtw_scan_handler_result_t *scan_result);
int scanNetworks(bool emitBegin = true, bool isSync = false);
void printScanResults();
void sortByChannel(std::vector<WiFiScanResult> &results);
bool applyTargetSpec(const String &spec, bool allowClear, String &errMsg);
