#include "sniff.h"
#include "config.h"
#include "utils.h"
#include "wifi_scan.h"
#include "wifi_attack.h"
#include "web.h"
#include "ble.h"

#include "wifi_conf.h"
#include "wifi_util.h"
#include "osdep_service.h"
#include "FreeRTOS.h"
#include "wiring_watchdog.h"
#include "WiFi.h"

// =========================
// Variable Definitions
// =========================
volatile SniffMode currentMode = SNIFF_STOP;
volatile bool isSniffing = false;

volatile bool isHopping = false;
unsigned long lastHopTime = 0;
const unsigned long HOP_INTERVAL = 500;
const int CHANNELS_2GHZ[13] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
const int CHANNELS_5GHZ[25] = {36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165};
int currentChannelIndex = 0;
bool hopUse5GHz = false;
bool hopResetPending = true;
volatile int currentChannel = WIFI_CHANNEL;

char sniffLogRing[LOG_RING_SIZE][LOG_STR_LEN];
volatile uint8_t logHead = 0;
volatile uint8_t logTail = 0;
volatile uint32_t droppedLogCount = 0;

char webLogHistory[WEB_LOG_HISTORY_SIZE][LOG_STR_LEN];
volatile uint8_t webLogHead = 0;
volatile uint8_t webLogCount = 0;
volatile uint32_t webLogTotalSeq = 0;

volatile uint32_t promiscCallbackInFlight = 0;
volatile bool emergencyStopRequested = false;
volatile bool priorityStopPending = false;

volatile unsigned long lastMgmtBeaconLogMs = 0;
volatile unsigned long lastMgmtProbeLogMs = 0;
volatile unsigned long lastCtrlLogMs = 0;
volatile unsigned long lastDataLogMs = 0;

volatile bool deauth_sniff_enabled = false;
bool deauth_sniff_prev_hop = false;
uint8_t sniffed_deauth_src[6] = {0};
volatile int sniffed_deauth_ch = WIFI_CHANNEL;
volatile bool has_sniffed_deauth = false;
unsigned long last_deauth_sniff_attack = 0;
unsigned long deauth_sniff_interval = 500;

// =========================
// PromiscFlightGuard Implementation
// =========================
PromiscFlightGuard::PromiscFlightGuard() {
  rtw_enter_critical(NULL, NULL);
  promiscCallbackInFlight++;
  rtw_exit_critical(NULL, NULL);
}

PromiscFlightGuard::~PromiscFlightGuard() {
  rtw_enter_critical(NULL, NULL);
  if (promiscCallbackInFlight > 0) {
    promiscCallbackInFlight--;
  }
  rtw_exit_critical(NULL, NULL);
}

// =========================
// resetPromiscLogThrottle
// =========================
void resetPromiscLogThrottle() {
  rtw_enter_critical(NULL, NULL);
  lastMgmtBeaconLogMs = 0;
  lastMgmtProbeLogMs = 0;
  lastCtrlLogMs = 0;
  lastDataLogMs = 0;
  rtw_exit_critical(NULL, NULL);
}

/*
 * promisc_callback — Realtek Wi-Fi RX Driver Context Sniff Callback
 *
 * Concurrency & Rate-Limiting Design:
 * 1. Flight Tracking (`PromiscFlightGuard`): Increments `promiscCallbackInFlight` under critical
 *    section on entry and decrements on all return paths. Mode teardown (`haltPromiscSniff`) and
 *    cloned-deauth TX pause (`loop`) disable promiscuous mode first, then spin-wait via
 *    `waitForPromiscCallbacksIdle()` so `wifi_off()` / channel switches never race an active RX callback.
 * 2. Zero Heap & Zero UART Blocking: Uses fixed stack buffers (`char logBuf[LOG_STR_LEN]`) and IE TLV
 *    bounds-checked UTF-8 sanitization (`extractSsidCstr`), enqueuing into `sniffLogRing` + `webLogHistory`.
 *    UART printing and BLE notifications happen asynchronously on `main_task` (`loop()`).
 * 3. Multi-Tier Per-Category Throttling:
 *    - Security-critical frames (EAPOL, Deauth, Disassoc, Pwnagotchi Beacon): unthrottled in RX callback.
 *    - Management Beacon / Probe frames: independent 80ms cooldowns (`lastMgmtBeaconLogMs` / `lastMgmtProbeLogMs`).
 *    - High-rate Control (ACK/RTS/CTS) & routine Data frames under `SNIFF_ALL`: independent 300ms cooldowns.
 */
void promisc_callback(unsigned char *buf, unsigned int len, void * /*userdata*/) {
  PromiscFlightGuard flightGuard;

  SniffMode modeSnapshot;
  bool sniffingSnapshot;
  bool deauthSniffSnapshot;
  int channelSnapshot;
  rtw_enter_critical(NULL, NULL);
  modeSnapshot = currentMode;
  sniffingSnapshot = isSniffing;
  deauthSniffSnapshot = deauth_sniff_enabled;
  channelSnapshot = currentChannel;
  rtw_exit_critical(NULL, NULL);

  if (!sniffingSnapshot || modeSnapshot == SNIFF_STOP) return;

  if (!buf || len < sizeof(wifi_ieee80211_mac_hdr)) {
    return;
  }

  wifi_ieee80211_mac_hdr *hdr = (wifi_ieee80211_mac_hdr *)buf;
  uint16_t fc = hdr->frame_control;
  uint8_t ftype = ieee80211_get_type(fc);
  uint8_t fsubtype = ieee80211_get_subtype(fc);

  switch (modeSnapshot) {
    case SNIFF_ALL:
      break;
    case SNIFF_BEACON:
      if (!(ftype == 0 && fsubtype == 8)) return;
      break;
    case SNIFF_PROBE:
      if (!(ftype == 0 && (fsubtype == 4 || fsubtype == 5))) return;
      break;
    case SNIFF_DEAUTH:
      if (!(ftype == 0 && (fsubtype == 12 || fsubtype == 10))) return;
      break;
    case SNIFF_EAPOL:
      if (ftype != 2 || !isEAPOL(buf, len)) return;
      break;
    case SNIFF_PWNAGOTCHI:
      if (!(ftype == 0 && fsubtype == 8 && isPwnagotchiMac(hdr->addr2))) return;
      break;
    case SNIFF_STOP:
    default:
      return;
  }

  char logBuf[LOG_STR_LEN];
  const uint8_t *framePtr = (const uint8_t *)buf;

  if (ftype == 0) {
    if (fsubtype == 8) {
      bool isPwn = isPwnagotchiMac(hdr->addr2);
      if (!isPwn) {
        unsigned long now = millis();
        rtw_enter_critical(NULL, NULL);
        if (lastMgmtBeaconLogMs != 0 && (now - lastMgmtBeaconLogMs < 80)) {
          rtw_exit_critical(NULL, NULL);
          return;
        }
        lastMgmtBeaconLogMs = now;
        rtw_exit_critical(NULL, NULL);
      }
      char mac2[18];
      char ssidBuf[36];
      formatMac(hdr->addr2, mac2, sizeof(mac2));
      extractSsidCstr(framePtr, len, 36, ssidBuf, sizeof(ssidBuf));
      if (ssidBuf[0] != '\0') {
        snprintf(logBuf, sizeof(logBuf), "[MGMT] Beacon detected %s SSID: %s%s",
                 mac2, ssidBuf,
                 isPwn ? " Pwnagotchi Beacon!" : "");
      } else {
        snprintf(logBuf, sizeof(logBuf), "[MGMT] Beacon detected %s", mac2);
      }
    }
    else if (fsubtype == 12) {
      char mac2[18], mac1[18];
      formatMac(hdr->addr2, mac2, sizeof(mac2));
      formatMac(hdr->addr1, mac1, sizeof(mac1));
      if (len >= 26) {
        uint16_t reasonCode = (uint16_t)buf[24] | ((uint16_t)buf[25] << 8);
        snprintf(logBuf, sizeof(logBuf), "[MGMT] Deauth detected Sender MAC: %s Receiver MAC: %s Reason code: %u",
                 mac2, mac1, (unsigned)reasonCode);
      } else {
        snprintf(logBuf, sizeof(logBuf), "[MGMT] Deauth detected Sender MAC: %s Receiver MAC: %s", mac2, mac1);
      }
      // Capture deauth source MAC and channel for deauth-sniff-and-clone mode
      if (deauthSniffSnapshot && !is_broadcast_mac(hdr->addr1)) {
        rtw_enter_critical(NULL, NULL);
        memcpy(sniffed_deauth_src, hdr->addr2, 6);
        sniffed_deauth_ch = channelSnapshot;
        has_sniffed_deauth = true;
        rtw_exit_critical(NULL, NULL);
      }
    }
    else if (fsubtype == 4 || fsubtype == 5) {
      unsigned long now = millis();
      rtw_enter_critical(NULL, NULL);
      if (lastMgmtProbeLogMs != 0 && (now - lastMgmtProbeLogMs < 80)) {
        rtw_exit_critical(NULL, NULL);
        return;
      }
      lastMgmtProbeLogMs = now;
      rtw_exit_critical(NULL, NULL);

      char mac2[18];
      char ssidBuf[36];
      formatMac(hdr->addr2, mac2, sizeof(mac2));
      if (fsubtype == 4) {
        extractSsidCstr(framePtr, len, 24, ssidBuf, sizeof(ssidBuf));
        if (ssidBuf[0] != '\0') {
          snprintf(logBuf, sizeof(logBuf), "[MGMT] Probe Request Source MAC: %s Probe SSID: %s", mac2, ssidBuf);
        } else {
          snprintf(logBuf, sizeof(logBuf), "[MGMT] Probe Request Source MAC: %s", mac2);
        }
      } else {
        extractSsidCstr(framePtr, len, 36, ssidBuf, sizeof(ssidBuf));
        if (ssidBuf[0] != '\0') {
          snprintf(logBuf, sizeof(logBuf), "[MGMT] Probe Response Source MAC: %s SSID: %s", mac2, ssidBuf);
        } else {
          snprintf(logBuf, sizeof(logBuf), "[MGMT] Probe Response Source MAC: %s", mac2);
        }
      }
    }
    else if (fsubtype == 10) {
      char mac2[18], mac1[18];
      formatMac(hdr->addr2, mac2, sizeof(mac2));
      formatMac(hdr->addr1, mac1, sizeof(mac1));
      if (len >= 26) {
        uint16_t reasonCode = (uint16_t)buf[24] | ((uint16_t)buf[25] << 8);
        snprintf(logBuf, sizeof(logBuf), "[MGMT] Disassoc detected Sender MAC: %s Receiver MAC: %s Reason code: %u",
                 mac2, mac1, (unsigned)reasonCode);
      } else {
        snprintf(logBuf, sizeof(logBuf), "[MGMT] Disassoc detected Sender MAC: %s Receiver MAC: %s", mac2, mac1);
      }
    }
    else {
      snprintf(logBuf, sizeof(logBuf), "[MGMT] Other subtype = %u", fsubtype);
    }
  }
  else if (ftype == 1) {
    // SNIFF_ALL 下对高频 ACK/RTS 等控制帧做 300ms 独立限频，限流通过后再构造日志
    unsigned long now = millis();
    rtw_enter_critical(NULL, NULL);
    if (lastCtrlLogMs != 0 && (now - lastCtrlLogMs < 300)) {
      rtw_exit_critical(NULL, NULL);
      return;
    }
    lastCtrlLogMs = now;
    rtw_exit_critical(NULL, NULL);
    snprintf(logBuf, sizeof(logBuf), "[CTRL] Subtype = %u", fsubtype);
  }
  else if (ftype == 2) {
    if (isEAPOL(buf, len)) {
      char mac2[18], mac1[18];
      formatMac(hdr->addr2, mac2, sizeof(mac2));
      formatMac(hdr->addr1, mac1, sizeof(mac1));
      snprintf(logBuf, sizeof(logBuf), "[DATA] EAPOL detected! Source MAC: %s Destination MAC: %s", mac2, mac1);
    } else {
      unsigned long now = millis();
      rtw_enter_critical(NULL, NULL);
      if (lastDataLogMs != 0 && (now - lastDataLogMs < 300)) {
        rtw_exit_critical(NULL, NULL);
        return;
      }
      lastDataLogMs = now;
      rtw_exit_critical(NULL, NULL);
      snprintf(logBuf, sizeof(logBuf), "[DATA] Other data frame.");
    }
  }
  else {
    snprintf(logBuf, sizeof(logBuf), "[EXT] Type = %u", ftype);
  }

  // 严禁在 Wi-Fi RX 回调中同步 Serial.println 阻塞串口；统一入环后由主循环消费并输出
  pushSniffLog(logBuf);
}

// =========================
// extractSsidCstr
// =========================
void extractSsidCstr(const uint8_t *frame, int totalLen, int possibleOffset, char *out, size_t outSize) {
  if (!out || outSize == 0) return;
  out[0] = '\0';
  if (!frame || possibleOffset < 0 || totalLen < possibleOffset + 2) {
    return;
  }
  int offset = possibleOffset;
  // 遍历 IE TLV，直到找到 SSID (tag 0) 或超出边界（全程零堆内存分配，可在 RX 线程安全运行）
  while (offset + 2 <= totalLen) {
    uint8_t tagNumber = frame[offset];
    uint8_t tagLength = frame[offset + 1];
    if (offset + 2 + tagLength > totalLen) break;  // 畸形长度，停止
    if (tagNumber == 0) {
      sanitizeUtf8BytesToCstr(&frame[offset + 2], (size_t)tagLength, out, outSize, true);
      return;
    }
    offset += 2 + tagLength;
  }
}

// =========================
// isPwnagotchiMac
// =========================
bool isPwnagotchiMac(const uint8_t *mac) {
  const uint8_t pwnMac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD};
  for (int i = 0; i < 6; i++) {
    if (mac[i] != pwnMac[i]) return false;
  }
  return true;
}

// =========================
// isEAPOL
// =========================
bool isEAPOL(const uint8_t *buf, int len) {
  if (!buf || len < 32) return false;
  uint8_t ftype = (buf[0] & 0x0C) >> 2;
  if (ftype != 2) return false; // 必须是 Data 帧
  // 精确计算 802.11 帧头长度，覆盖 4-Address(WDS) / QoS / QoS+HTC 变体
  int hdrLen = 24;
  if ((buf[1] & 0x03) == 0x03) hdrLen += 6; // ToDS=1 & FromDS=1 -> 4-Address，多 6 字节
  bool isQoS = ((buf[0] & 0x80) != 0);
  if (isQoS) {
    hdrLen += 2; // QoS Data (Subtype bit3)
    if ((buf[1] & 0x80) != 0) {
      hdrLen += 4; // +HTC 仅在 QoS Data 且 Order 置位时存在
    }
  }
  if (len < hdrLen + 8) return false;
  // LLC/SNAP (AA AA 03 00 00 00) + EtherType 0x888E (EAPOL)
  return (buf[hdrLen] == 0xAA && buf[hdrLen + 1] == 0xAA && buf[hdrLen + 2] == 0x03 &&
          buf[hdrLen + 3] == 0x00 && buf[hdrLen + 4] == 0x00 && buf[hdrLen + 5] == 0x00 &&
          buf[hdrLen + 6] == 0x88 && buf[hdrLen + 7] == 0x8E);
}

// =========================
// setChannel
// =========================
bool setChannel(int newChannel) {
  bool sniffingNow = false;
  rtw_enter_critical(NULL, NULL);
  sniffingNow = isSniffing;
  rtw_exit_critical(NULL, NULL);

  // SoftAP 实际在空中工作且未处于嗅探态时，切离热点信道会破坏 WebUI 连接
  if (softApActive && !sniffingNow && newChannel != WIFI_CHANNEL) {
    Serial.println("[WARN] SoftAP active on CH 1; skipping channel lock.");
    return false;
  }
  if (wifi_set_channel(newChannel) == RTW_SUCCESS) {
    setCurrentChannelSafe(newChannel);
    return true;
  }
  Serial.print("[WARN] wifi_set_channel(");
  Serial.print(newChannel);
  Serial.println(") failed");
  return false;
}

// =========================
// resetHopState
// =========================
void resetHopState() {
  hopUse5GHz = false;
  currentChannelIndex = 0;
  hopResetPending = true;
  lastHopTime = millis();
}

// =========================
// hopChannel
// =========================
void hopChannel() {
  if (!isHopping) return;
  unsigned long now = millis();
  if (!hopResetPending && (now - lastHopTime < HOP_INTERVAL)) return;

  const int num2G = sizeof(CHANNELS_2GHZ) / sizeof(CHANNELS_2GHZ[0]);
  const int num5G = sizeof(CHANNELS_5GHZ) / sizeof(CHANNELS_5GHZ[0]);
  int nextChannel = WIFI_CHANNEL;

  if (!hopUse5GHz) {
    if (currentChannelIndex < 0 || currentChannelIndex >= num2G) {
      currentChannelIndex = 0;
    }
    nextChannel = CHANNELS_2GHZ[currentChannelIndex];
    currentChannelIndex++;
    if (currentChannelIndex >= num2G) {
      currentChannelIndex = 0;
      hopUse5GHz = true;
    }
  } else {
    if (currentChannelIndex < 0 || currentChannelIndex >= num5G) {
      currentChannelIndex = 0;
    }
    nextChannel = CHANNELS_5GHZ[currentChannelIndex];
    currentChannelIndex++;
    if (currentChannelIndex >= num5G) {
      currentChannelIndex = 0;
      hopUse5GHz = false;
    }
  }

  hopResetPending = false;
  lastHopTime = now;
  if (setChannel(nextChannel)) {
    Serial.print("[HOP] Switched to channel ");
    Serial.println(nextChannel);
  }
}

// =========================
// startSniffing
// =========================
void startSniffing() {
  stopAllActiveAttacks();
  bool alreadySniffing = false;
  rtw_enter_critical(NULL, NULL);
  alreadySniffing = isSniffing;
  rtw_exit_critical(NULL, NULL);

  if (!alreadySniffing) {
    Serial.println("[INFO] Enabling promiscuous mode...");
    // Shut down HTTP listening socket cleanly before wifi_enter_promisc_mode() calls wifi_off()
    prepareRadioForSniff(false);
    resetPromiscLogThrottle();
    WlanRuntimeMode prevMode = wlanRuntimeMode;
    feedWatchdog();
    int preWextMode = -1;
    if (wlanRuntimeMode == WLAN_MODE_UNINIT || wext_get_mode(WLAN0_NAME, &preWextMode) != RTW_SUCCESS) {
      if (wifi_on(RTW_MODE_PROMISC) != RTW_SUCCESS) {
        wlanRuntimeMode = WLAN_MODE_UNINIT;
        timedSniffEnabled = false;
        rtw_enter_critical(NULL, NULL);
        isSniffing = false;
        currentMode = SNIFF_STOP;
        rtw_exit_critical(NULL, NULL);
        Serial.println("[ERROR] startSniffing: wifi_on(RTW_MODE_PROMISC) failed.");
        restoreRadioAfterSniff();
        return;
      }
      delay(20);
      feedWatchdog();
    } else {
      wifi_enter_promisc_mode();
      feedWatchdog();
    }
    int wextMode = -1;
    wext_get_mode(WLAN0_NAME, &wextMode);
    bool stayedSta = (prevMode == WLAN_MODE_STA && wextMode == RTW_MODE_INFRA);
    wlanRuntimeMode = stayedSta ? WLAN_MODE_STA : WLAN_MODE_PROMISC;
    WiFi.disablePowerSave();
    restoreBleCoex(!stayedSta);
    int targetCh = getCurrentChannelSafe();
    setChannel(targetCh);

    // 先设置状态标志再启用回调，避免首波回调丢失或读到 SNIFF_STOP
    rtw_enter_critical(NULL, NULL);
    if (currentMode == SNIFF_STOP) {
      currentMode = SNIFF_ALL;
    }
    isSniffing = true;
    rtw_exit_critical(NULL, NULL);

    if (wifi_set_promisc(RTW_PROMISC_ENABLE_2, promisc_callback, 1) != RTW_SUCCESS) {
      rtw_enter_critical(NULL, NULL);
      isSniffing = false;
      currentMode = SNIFF_STOP;
      rtw_exit_critical(NULL, NULL);
      timedSniffEnabled = false;
      Serial.println("[ERROR] Failed to enable promiscuous callback.");
      restoreRadioAfterSniff();
      return;
    }
    Serial.println("[INFO] Sniffer initialized and running.");
  }
}

// =========================
// waitForPromiscCallbacksIdle
// =========================
void waitForPromiscCallbacksIdle() {
  unsigned long waitedMs = 0;
  while (waitedMs < 60) {
    uint32_t inFlight = 0;
    rtw_enter_critical(NULL, NULL);
    inFlight = promiscCallbackInFlight;
    rtw_exit_critical(NULL, NULL);
    if (inFlight == 0) break;
    feedWatchdog();
    delay(2);
    waitedMs += 2;
  }
  delay(5);
}

// =========================
// haltPromiscSniff
// =========================
bool haltPromiscSniff() {
  timedSniffEnabled = false;
  bool active = false;
  rtw_enter_critical(NULL, NULL);
  active = (isSniffing || deauth_sniff_enabled);
  isSniffing = false;
  currentMode = SNIFF_STOP;
  rtw_exit_critical(NULL, NULL);
  if (!active) return false;

  // 先关闭底层混杂回调并等待在途 RX 回调彻底退出，防止后续 wifi_off() 撕裂驱动状态
  wifi_set_promisc(RTW_PROMISC_DISABLE, NULL, 0);
  waitForPromiscCallbacksIdle();
  resetPromiscLogThrottle();
  clearDeauthSniffState();
  resetHopState();
  return true;
}

// =========================
// stopSniffing
// =========================
void stopSniffing() {
  if (haltPromiscSniff()) {
    Serial.println("[CMD] Sniffer stopped");
    restoreRadioAfterSniff();
  }
}

// =========================
// clearDeauthSniffState
// =========================
void clearDeauthSniffState() {
  rtw_enter_critical(NULL, NULL);
  bool wasDeauthSniff = deauth_sniff_enabled;
  if (wasDeauthSniff) {
    isHopping = deauth_sniff_prev_hop;
    deauth_sniff_enabled = false;
    deauth_sniff_prev_hop = false;
  }
  has_sniffed_deauth = false;
  rtw_exit_critical(NULL, NULL);
}

// =========================
// prepareRadioForSniff
// =========================
void prepareRadioForSniff(bool turnOffAp) {
  stopWebServer();
  softApActive = false;
  if (turnOffAp) {
    wifi_off();
    feedWatchdog();
    delay(40);
    wlanRuntimeMode = WLAN_MODE_UNINIT;
  }
}

// =========================
// startMonitorSniff
// =========================
void startMonitorSniff(SniffMode mode, bool timed, unsigned long dur) {
  if (mode == SNIFF_STOP) return;
  clearDeauthSniffState();
  resetHopState();
  timedSniffEnabled = timed;
  if (timed) {
    sniffStartTime = millis();
    sniffDuration = dur;
  }
  rtw_enter_critical(NULL, NULL);
  currentMode = mode;
  rtw_exit_critical(NULL, NULL);
  startSniffing();
}

// =========================
// clearSniffLogRingSafe
// =========================
void clearSniffLogRingSafe() {
  rtw_enter_critical(NULL, NULL);
  logHead = 0;
  logTail = 0;
  droppedLogCount = 0;
  rtw_exit_critical(NULL, NULL);
}

// =========================
// pushSniffLog
// =========================
void pushSniffLog(const char *msg) {
  char safeMsg[LOG_STR_LEN];
  utf8SafeCopy(safeMsg, sizeof(safeMsg), msg);
  size_t copyBytes = strlen(safeMsg) + 1;
  if (copyBytes > LOG_STR_LEN) copyBytes = LOG_STR_LEN;

  rtw_enter_critical(NULL, NULL);
  uint8_t next = (logHead + 1) % LOG_RING_SIZE;
  if (next != logTail) {
    memcpy(sniffLogRing[logHead], safeMsg, copyBytes);
    logHead = next;
  } else {
    droppedLogCount++;
  }
  // Also record into WebUI circular history so /api/logs can inspect captured frames incrementally
  memcpy(webLogHistory[webLogHead], safeMsg, copyBytes);
  webLogHead = (webLogHead + 1) % WEB_LOG_HISTORY_SIZE;
  if (webLogCount < WEB_LOG_HISTORY_SIZE) {
    webLogCount++;
  }
  webLogTotalSeq++;
  rtw_exit_critical(NULL, NULL);
}
