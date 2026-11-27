/*
 * config.h — PersistConfig, Flash R/W, global constants & user settings
 * Part of WiFi-Deck multi-file refactor.
 */
#pragma once

#include <Arduino.h>
#include <FlashMemory.h>
#include <cstddef>
#include <cstring>
#include <errno.h>

//==========================
// Firmware Version & Defaults
//==========================
#define FIRMWARE_VERSION  "2.2.1"
#define DEFAULT_WIFI_SSID "WiFi-Deck"
#define DEFAULT_WIFI_PASS "12345678"
#define DEFAULT_BLE_NAME  "WiFi-Deck"
#define WIFI_CHANNEL      1

//==========================
// Flash Persistence Constants
//==========================
#define CFG_MAGIC   0x57444B33UL  // "WDK3"
#define CFG_VERSION 3

//==========================
// Flash Persistence Structure
//==========================
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

//==========================
// Global Configuration Variables (extern)
//==========================
extern char ap_ssid[33];
extern char ap_pass[65];
extern char ble_name[27];

extern bool USE_LED;
extern bool apEnabled;
extern volatile bool softApActive;
extern bool pendingApRestart;
extern unsigned long pendingApRestartAt;

// Attack parameters
extern unsigned long last_cycle;
extern unsigned long cycle_delay;
extern unsigned long scan_time;
extern unsigned long num_send_frames;
extern int start_channel;
extern bool scan_between_cycles;

extern uint8_t dst_mac[6];

// Timed Attack & Timed Sniff
extern bool timedAttackEnabled;
extern unsigned long attackStartTime;
extern unsigned long attackDuration;
extern bool timedSniffEnabled;
extern unsigned long sniffStartTime;
extern unsigned long sniffDuration;

extern int single_ap_index;

// Rotating sequence number counter
extern uint16_t seq_num_counter;

//==========================
// Function Prototypes
//==========================
uint32_t calcCrc32(const uint8_t *data, size_t len);
void loadConfigFromFlash();
void saveConfigToFlash();
bool parseStrictLong(const String &s, long &out);
bool validateConfigSetting(const String &key, const String &rawValue, String &errMsg);
bool applyConfigSetting(const String &key, const String &rawValue, bool &apChanged);
