#include "wifi_scan.h"
#include "config.h"
#include "utils.h"
#include "sniff.h"
#include "ble.h"
#include "wifi_attack.h"
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
    if (scan_accepting) {
      bool merged = false;
      for (size_t i = 0; i < scan_results.size(); i++) {
        if (memcmp(scan_results[i].bssid, result.bssid, 6) == 0) {
          if (result.rssi > scan_results[i].rssi) {
            scan_results[i].rssi = result.rssi;
            scan_results[i].channel = result.channel;
          }
          if (scan_results[i].ssid[0] == '\0' && result.ssid[0] != '\0') {
            memcpy(scan_results[i].ssid, result.ssid, sizeof(scan_results[i].ssid));
          }
          merged = true;
          break;
        }
      }
      if (!merged) {
        if (scan_results.size() < MAX_SCAN_RESULTS) {
          scan_results.push_back(result);
        } else {
          // Replace weakest AP when full so strong 5GHz APs in later batches are never starved
          size_t weakestIdx = 0;
          for (size_t i = 1; i < scan_results.size(); i++) {
            if (scan_results[i].rssi < scan_results[weakestIdx].rssi) {
              weakestIdx = i;
            }
          }
          if (result.rssi > scan_results[weakestIdx].rssi) {
            scan_results[weakestIdx] = result;
          }
        }
      }
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
// Start a WiFi Scan (Segmented Dual-Band PartialScan + BLE Coex Windows)
//==========================================================
int scanNetworks(bool emitBegin, bool isSync) {
  Serial.println("Starting WiFi scan...");
  processBleDiagAndNegotiation();

  if (lockScanMutex()) {
    scan_results.clear();
    scan_results.reserve(MAX_SCAN_RESULTS);
    scan_done = false;
    scan_accepting = false;
    unlockScanMutex();
  }

  // Why segmented PartialScan is mandatory on RTL8720DN (verified in application.asm):
  // 1) Monolithic wifi_scan_networks() under BLE coex (0x0202) stretches active channels
  //    (ScanType==1, max_step=2) via rtl8721d_btcoex_wifi_tdma_scan_start/handle to 50+50=100ms/ch
  //    and passive DFS channels (ScanType==0, CH 52-144 & CH 12-13, max_step=4) to 60+100+60+100=320ms/ch
  //    (~7.76s total), locking the single shared RF PLL (RF_0x18) on 5GHz continuously for
  //    9*100 + 16*320 = 6,020ms without retuning to 2.4GHz -> 100% BLE 0x0108 Supervision Timeout!
  // 2) rtw_wx_set_pscan_freq (0x0e0312b4) preserves init_channel_set & DFS passive ScanType
  //    only when length >= 2, and PSCAN_ENABLE (0x01, without PSCAN_FAST_SURVEY) scans each
  //    enabled channel in 1 dwell. By grouping passive DFS channels in pairs of 2 (2*320ms = 640ms)
  //    and active 5GHz channels in groups of 2-4 (200-400ms), every 5GHz batch stays on 5GHz <= 640ms
  //    and then parks on 2.4GHz CH 1 for 95ms with 0x55555555 BLE PTA priority so BLE never times out.
  struct ScanBatchSpec {
    uint8_t count;
    bool is5GHz;
    uint8_t chans[5];
  };
  static const ScanBatchSpec kBatches[] = {
    {5, false, {1, 2, 3, 4, 5}},
    {5, false, {6, 7, 8, 9, 10}},
    {3, false, {11, 12, 13, 0, 0}},
    // All 5GHz channels dwell ~330ms/ch under AmebaD TDMA coex; cap every 5GHz batch to 2 channels
    // (~690ms max continuous 5GHz RF PLL lock) followed by a 95ms 2.4GHz BLE breathing window.
    {2, true,  {36, 40, 0, 0, 0}},
    {2, true,  {44, 48, 0, 0, 0}},
    {2, true,  {52, 56, 0, 0, 0}},
    {2, true,  {60, 64, 0, 0, 0}},
    {2, true,  {100, 104, 0, 0, 0}},
    {2, true,  {108, 112, 0, 0, 0}},
    {2, true,  {116, 120, 0, 0, 0}},
    {2, true,  {124, 128, 0, 0, 0}},
    {2, true,  {132, 136, 0, 0, 0}},
    {2, true,  {140, 144, 0, 0, 0}},
    {2, true,  {149, 153, 0, 0, 0}},
    {2, true,  {157, 161, 0, 0, 0}},
    {2, true,  {161, 165, 0, 0, 0}},
  };
  const size_t totalBatches = sizeof(kBatches) / sizeof(kBatches[0]);

  const int minCh = start_channel;
  const unsigned long minWallBudgetMs = (minCh >= 36) ? 12000UL : 16500UL;
  unsigned long totalBudgetMs = (scan_time > minWallBudgetMs) ? (scan_time + 4500UL) : minWallBudgetMs;
  if (totalBudgetMs > 28000UL) totalBudgetMs = 28000UL;
  const unsigned long perBatchTimeoutMs = 2200UL;

  if (USE_LED) digitalWrite(LED_G, HIGH);

  // Anchor driver oper_channel ([padapter + 0x561]) on 2.4GHz CH 1 before scanning so that
  // site_survey's end-of-batch set_channel_bwmode(padapter, oper_channel, ...) always returns
  // the shared RF PLL (RF_0x18) to 2.4GHz rather than a previously locked 5GHz channel.
  restoreDefaultChannelIfNeeded(true);
  enforceBleCoexPta(!softApActive);

  unsigned long overallStartMs = millis();
  size_t startedBatches = 0;
  size_t succeededBatches = 0;

  for (size_t b = 0; b < totalBatches; b++) {
    feedWatchdog();
    pollSerialInput();
    processBleDiagAndNegotiation();
    if (emergencyStopRequested || priorityStopPending || hasQueuedStopInCmdRing()) {
      break;
    }
    if (startedBatches > 0 && (millis() - overallStartMs >= totalBudgetMs)) {
      Serial.println("[SCAN] Reached overall scan budget guard; finalizing results.");
      break;
    }

    uint8_t batchChans[5] = {0};
    uint8_t pscanCfg[5] = {0};
    uint8_t batchLen = 0;
    for (uint8_t k = 0; k < kBatches[b].count; k++) {
      uint8_t ch = kBatches[b].chans[k];
      if ((int)ch >= minCh) {
        batchChans[batchLen] = ch;
        pscanCfg[batchLen] = PSCAN_ENABLE;
        batchLen++;
      }
    }
    if (batchLen == 0) {
      continue;
    }
    // Always pass length >= 2 to wifi_set_pscan_chan so rtw_wx_set_pscan_freq preserves the
    // full 38-channel regulatory table; scanResultHandler filters record->channel < start_channel.
    if (batchLen == 1 && kBatches[b].count >= 2) {
      batchChans[0] = kBatches[b].chans[kBatches[b].count - 2];
      batchChans[1] = kBatches[b].chans[kBatches[b].count - 1];
      pscanCfg[0] = PSCAN_ENABLE;
      pscanCfg[1] = PSCAN_ENABLE;
      batchLen = 2;
    }

    if (lockScanMutex()) {
      scan_done = false;
      scan_accepting = true;
      unlockScanMutex();
    }

    startedBatches++;
    bool pscanOk = (wifi_set_pscan_chan(batchChans, pscanCfg, batchLen) >= 0);
    if (!pscanOk && startedBatches > 1) {
      if (lockScanMutex()) {
        scan_accepting = false;
        unlockScanMutex();
      }
      Serial.println("[WARN] wifi_set_pscan_chan failed on batch; skipping.");
      continue;
    }

    enforceBleCoexPta(true);
    if (wifi_scan_networks(scanResultHandler, NULL) != RTW_SUCCESS) {
      if (lockScanMutex()) {
        scan_accepting = false;
        unlockScanMutex();
      }
      Serial.println("[WARN] wifi_scan_networks batch start failed.");
      if (!pscanOk) {
        break;
      }
      serviceBleBreathingWindow(60UL, true);
      continue;
    }

    succeededBatches++;
    unsigned long batchStartMs = millis();
    bool stopTriggered = false;
    // Wait for current short batch's wifi_scan_done_hdl to set scan_done before breaking out,
    // preventing an in-flight site_survey in rtw_cmd_thread from racing subsequent commands.
    // Do NOT touch pcoex/PTA registers mid-survey: cmd_thread & survey_timer_hdl own TDMA scan slots.
    while (!scan_done && (millis() - batchStartMs < perBatchTimeoutMs)) {
      feedWatchdog();
      pollSerialInput();
      processBleDiagAndNegotiation();
      if (!stopTriggered &&
          (emergencyStopRequested || priorityStopPending || hasQueuedStopInCmdRing())) {
        stopTriggered = true;
      }
      delay(15);
    }

    if (lockScanMutex()) {
      scan_accepting = false;
      unlockScanMutex();
    }
    // Allow wifi_scan_done_hdl (0x0e06e540) to finish freeing ap_details & clearing scan_running
    delay(15);
    feedWatchdog();
    processBleDiagAndNegotiation();

    if (!pscanOk || stopTriggered || emergencyStopRequested || priorityStopPending || hasQueuedStopInCmdRing()) {
      break;
    }

    // Check if there is any remaining batch >= minCh; if so, park on 2.4GHz CH 1 for BLE & SoftAP
    bool hasMoreBatches = false;
    for (size_t nextB = b + 1; nextB < totalBatches; nextB++) {
      for (uint8_t k = 0; k < kBatches[nextB].count; k++) {
        if ((int)kBatches[nextB].chans[k] >= minCh) {
          hasMoreBatches = true;
          break;
        }
      }
      if (hasMoreBatches) break;
    }
    if (hasMoreBatches) {
      serviceBleBreathingWindow(kBatches[b].is5GHz ? 95UL : 45UL, true);
    }
  }

  restoreDefaultChannelIfNeeded(true);
  enforceBleCoexPta(!softApActive);

  if (startedBatches > 0 && succeededBatches == 0) {
    if (lockScanMutex()) {
      scan_accepting = false;
      unlockScanMutex();
    }
    if (USE_LED) digitalWrite(LED_G, LOW);
    Serial.println("Failed to start the scan!");
    return 1;
  }

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
