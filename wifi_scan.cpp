#include "wifi_scan.h"
#include "config.h"
#include "utils.h"
#include "sniff.h"
#include "ble.h"
#ifdef rand
#undef rand
#endif
#include <algorithm>
#include "wifi_conf.h"
#include "wifi_util.h"
#include "osdep_service.h"

//==========================================================
// Global Vectors & Scan Mutex
//==========================================================
std::vector<WiFiScanResult> scan_results;
std::vector<WiFiScanResult> target_aps;
SemaphoreHandle_t scanMutex = NULL;

//==========================================================
// Scan State
//==========================================================
volatile bool scan_done = false;
volatile bool scan_accepting = false;  // 扫描期间才允许驱动线程写入 scan_results
bool scan_enabled               = false;
bool target_mode                = false;

//==========================================================
// Attack State
//==========================================================
bool attack_enabled             = false;
uint32_t tx_success_count       = 0;
uint32_t tx_fail_count          = 0;

//==========================================================
// WLAN Runtime Mode
//==========================================================
volatile WlanRuntimeMode wlanRuntimeMode = WLAN_MODE_UNINIT;
bool wlanLwipInitialized = false;

//==========================================================
// Sliced TX & BLE Scan Push
//==========================================================
SlicedTxCycle deauthSlice;
SlicedTxCycle disassocSlice;
BleScanPushState bleScanPush;

//==========================================================
// Flood / Jam attacks
//==========================================================
bool beacon_flood_enabled    = false;
unsigned long beacon_flood_delay = 50;   // ms between beacons
unsigned long last_beacon_flood  = 0;
uint16_t beacon_flood_count  = 0;

bool probe_flood_enabled     = false;
unsigned long probe_flood_delay = 30;   // ms between probes
unsigned long last_probe_flood  = 0;
uint16_t probe_flood_count   = 0;

bool signal_jam_enabled     = false;
unsigned long signal_jam_hop_delay = 100; // ms per channel
unsigned long last_signal_jam  = 0;
int signal_jam_ch_index     = 0;
uint16_t signal_jam_count   = 0;

//==========================================================
// Snapshot Helpers
//==========================================================
std::vector<WiFiScanResult> snapshotScanResults() {
  std::vector<WiFiScanResult> copy;
  if (lockScanMutex()) {
    copy = scan_results;
    unlockScanMutex();
  }
  return copy;
}

std::vector<WiFiScanResult> snapshotTargetAps() {
  std::vector<WiFiScanResult> copy;
  if (lockScanMutex()) {
    copy = target_aps;
    unlockScanMutex();
  }
  return copy;
}

std::vector<WiFiScanResult> snapshotDisassocTargets() {
  std::vector<WiFiScanResult> copy;
  if (lockScanMutex()) {
    // 决策 A：严禁在未选择目标或 target_mode == false 时回退全量 scan_results
    if (target_mode && !target_aps.empty()) {
      copy = target_aps;
    }
    unlockScanMutex();
  }
  return copy;
}

//==========================================================
// Count Helpers
//==========================================================
size_t getScanCountSafe() {
  size_t count = 0;
  if (lockScanMutex()) {
    count = scan_results.size();
    unlockScanMutex();
  }
  return count;
}

size_t getTargetCountSafe() {
  size_t count = 0;
  if (lockScanMutex()) {
    count = target_aps.size();
    unlockScanMutex();
  }
  return count;
}

void getScanAndTargetCountsSafe(size_t &scanCountOut, size_t &targetCountOut, bool &targetModeOut) {
  scanCountOut = 0;
  targetCountOut = 0;
  targetModeOut = false;
  if (lockScanMutex()) {
    scanCountOut = scan_results.size();
    targetCountOut = target_aps.size();
    targetModeOut = target_mode;
    unlockScanMutex();
  }
}

bool hasSelectedTargetsSafe() {
  bool ok = false;
  if (lockScanMutex()) {
    ok = (target_mode && !target_aps.empty());
    unlockScanMutex();
  }
  return ok;
}

//==========================================================
// Sorting Helper (in-place O(n log n), zero heap allocation)
//==========================================================
void sortByChannel(std::vector<WiFiScanResult> &results) {
  std::sort(results.begin(), results.end(), [](const WiFiScanResult &a, const WiFiScanResult &b) {
    return a.channel < b.channel;
  });
}

//==========================================================
// Wi-Fi Scan Callback (zero heap allocation inside scanMutex)
//==========================================================
rtw_result_t scanResultHandler(rtw_scan_handler_result_t *scan_result) {
  if (!scan_result) return RTW_SUCCESS;

  if (scan_result->scan_complete == 0) {
    rtw_scan_result_t *record = &scan_result->ap_details;
    if ((int)record->channel < start_channel) {
      return RTW_SUCCESS;
    }

    WiFiScanResult result;
    uint8_t ssidLen = (record->SSID.len > 32) ? 32 : record->SSID.len;
    sanitizeUtf8BytesToCstr((const uint8_t *)record->SSID.val, (size_t)ssidLen,
                            result.ssid, sizeof(result.ssid), false);
    formatMac(record->BSSID.octet, result.bssid_str, sizeof(result.bssid_str));
    result.channel = record->channel;
    result.rssi = record->signal_strength;
    memcpy(&result.bssid, &record->BSSID, 6);

    if (!lockScanMutex(pdMS_TO_TICKS(500))) return RTW_SUCCESS;
    if (scan_accepting && scan_results.size() < MAX_SCAN_RESULTS) {
      scan_results.push_back(result);
    }
    unlockScanMutex();
  } else {
    if (lockScanMutex(pdMS_TO_TICKS(500))) {
      scan_accepting = false;
      scan_done = true;
      unlockScanMutex();
    } else {
      scan_accepting = false;
      scan_done = true;
    }
  }
  return RTW_SUCCESS;
}

//==========================================================
// Start a WiFi Scan
//==========================================================
int scanNetworks(bool emitBegin, bool isSync) {
  Serial.println("Starting WiFi scan...");
  if (lockScanMutex()) {
    scan_results.clear();
    scan_results.reserve(MAX_SCAN_RESULTS);
    scan_done = false;
    scan_accepting = true;
    unlockScanMutex();
  }
  if (wifi_scan_networks(scanResultHandler, NULL) == RTW_SUCCESS) {
    if (USE_LED) digitalWrite(LED_G, HIGH);
    // Wait for the driver to signal scan completion, using scan_time only as
    // a maximum-timeout guard so we never block forever.
    unsigned long start = millis();
    feedWatchdog();
    while (!scan_done && (millis() - start < scan_time)) {
      feedWatchdog();
      pollSerialInput();
      if (emergencyStopRequested || priorityStopPending) break;
      delay(50);
    }
    feedWatchdog();
    // 在 scanMutex 保护下关闭写入窗口、排序并同步选中目标，杜绝与延迟到达回调并发写 vector
    if (lockScanMutex()) {
      scan_accepting = false;
      sortByChannel(scan_results);
      if (target_mode && !target_aps.empty()) {
        for (size_t t = 0; t < target_aps.size(); t++) {
          for (size_t i = 0; i < scan_results.size(); i++) {
            if (memcmp(scan_results[i].bssid, target_aps[t].bssid, 6) == 0) {
              scan_results[i].selected = true;
              target_aps[t].channel = scan_results[i].channel;
              target_aps[t].rssi = scan_results[i].rssi;
              break;
            }
          }
        }
      }
      unlockScanMutex();
    }
    Serial.println("Scan completed!");
    blePushScanResults(emitBegin, isSync);
    if (USE_LED) digitalWrite(LED_G, LOW);
    return 0;
  } else {
    if (lockScanMutex()) {
      scan_accepting = false;
      unlockScanMutex();
    }
    if (USE_LED) digitalWrite(LED_G, LOW);
    Serial.println("Failed to start the scan!");
    return 1;
  }
}

//==========================================================
// Print Scan Results
//==========================================================
void printScanResults() {
  std::vector<WiFiScanResult> snap = snapshotScanResults();
  Serial.println("Detected networks:");
  for (size_t i = 0; i < snap.size(); i++) {
    const char *freq = (snap[i].channel >= 36) ? "5GHz" : "2.4GHz";
    Serial.print(i);
    Serial.print("\tSSID: ");
    Serial.print(snap[i].ssid);
    Serial.print("\tBSSID: ");
    Serial.print(snap[i].bssid_str);
    Serial.print("\tChannel: ");
    Serial.print(snap[i].channel);
    Serial.print("\tRSSI: ");
    Serial.print(snap[i].rssi);
    Serial.print(" dBm\t");
    Serial.println(freq);
  }
}

//==========================================================
// Apply Target Spec
//==========================================================
bool applyTargetSpec(const String &rawSpec, bool allowClearEmpty, String &errMsg) {
  String spec = rawSpec;
  spec.replace(" ", "");
  spec.trim();

  if (spec.length() == 0) {
    if (!allowClearEmpty) {
      errMsg = "No target indices provided.";
      return false;
    }
    if (lockScanMutex()) {
      target_aps.clear();
      target_mode = false;
      for (size_t i = 0; i < scan_results.size(); i++) {
        scan_results[i].selected = false;
      }
      unlockScanMutex();
    }
    deauthSlice.reset();
    disassocSlice.reset();
    return true;
  }

  if (spec.equalsIgnoreCase("all")) {
    bool ok = false;
    if (lockScanMutex()) {
      if (scan_results.empty()) {
        errMsg = "No scan results available. Run 'scan' first.";
      } else {
        target_aps.clear();
        target_mode = false;
        for (size_t i = 0; i < scan_results.size(); i++) {
          scan_results[i].selected = true;
          target_aps.push_back(scan_results[i]);
        }
        target_mode = !target_aps.empty();
        ok = target_mode;
      }
      unlockScanMutex();
    }
    if (ok) {
      deauthSlice.reset();
      disassocSlice.reset();
    }
    return ok;
  }

  std::vector<long> parsedIndices;
  int start = 0;
  while (start <= (int)spec.length()) {
    int end = spec.indexOf(',', start);
    String tok = (end == -1) ? spec.substring(start) : spec.substring(start, end);
    tok.trim();
    if (tok.length() == 0) {
      errMsg = "Empty token in target index list.";
      return false;
    }
    long parsedIdx = -1;
    if (!parseStrictLong(tok, parsedIdx) || parsedIdx < 0) {
      errMsg = "Invalid target index: " + tok;
      return false;
    }
    parsedIndices.push_back(parsedIdx);
    if (end == -1) break;
    start = end + 1;
  }
  if (parsedIndices.empty()) {
    errMsg = "No valid target indices provided.";
    return false;
  }

  bool ok = false;
  if (lockScanMutex()) {
    if (scan_results.empty()) {
      errMsg = "No scan results available. Run 'scan' first.";
    } else {
      bool allInRange = true;
      for (size_t k = 0; k < parsedIndices.size(); k++) {
        if (parsedIndices[k] < 0 || parsedIndices[k] >= (long)scan_results.size()) {
          errMsg = "Target index out of range: " + String(parsedIndices[k]);
          allInRange = false;
          break;
        }
      }
      if (allInRange) {
        target_aps.clear();
        target_mode = false;
        for (size_t i = 0; i < scan_results.size(); i++) {
          scan_results[i].selected = false;
        }
        for (size_t k = 0; k < parsedIndices.size(); k++) {
          size_t idx = (size_t)parsedIndices[k];
          if (!scan_results[idx].selected) {
            scan_results[idx].selected = true;
            target_aps.push_back(scan_results[idx]);
          }
        }
        target_mode = !target_aps.empty();
        ok = target_mode;
      }
    }
    unlockScanMutex();
  }
  if (ok) {
    deauthSlice.reset();
    disassocSlice.reset();
  }
  return ok;
}
