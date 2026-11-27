/*
 * config.cpp — PersistConfig, Flash R/W, global constants & user settings
 * Part of WiFi-Deck multi-file refactor.
 */
#include "config.h"
#include "ble.h"
#include "utils.h"

//==========================
// Global Configuration Variable Definitions
//==========================
char ap_ssid[33]  = DEFAULT_WIFI_SSID;
char ap_pass[65]  = DEFAULT_WIFI_PASS;
char ble_name[27] = DEFAULT_BLE_NAME;

bool USE_LED = true;
bool apEnabled = true;
volatile bool softApActive = false;
bool pendingApRestart = false;
unsigned long pendingApRestartAt = 0;

// Attack parameters
unsigned long last_cycle     = 0;
unsigned long cycle_delay    = 2000;
unsigned long scan_time      = 5000;
unsigned long num_send_frames = 3;
int start_channel            = 1;
bool scan_between_cycles     = false;

uint8_t dst_mac[6]  = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Timed Attack & Timed Sniff
bool timedAttackEnabled      = false;
unsigned long attackStartTime = 0;
unsigned long attackDuration  = 10000;

bool timedSniffEnabled       = false;
unsigned long sniffStartTime  = 0;
unsigned long sniffDuration   = 15000;

int single_ap_index = 0;

// Rotating sequence number counter (must be nonzero; real APs increment)
uint16_t seq_num_counter = 1;

//==========================
// CRC32 (used by Flash persistence)
//==========================
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

//==========================
// Flash Load / Save
//==========================
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

  size_t ssidLen = boundedStrLen(cfg.ap_ssid, sizeof(cfg.ap_ssid));
  if (ssidLen >= 1 && ssidLen <= 32) {
    boundedStrCopy(ap_ssid, sizeof(ap_ssid), cfg.ap_ssid);
  }
  size_t passLen = boundedStrLen(cfg.ap_pass, sizeof(cfg.ap_pass));
  if (passLen >= 8 && passLen <= 63) {
    boundedStrCopy(ap_pass, sizeof(ap_pass), cfg.ap_pass);
  }
  size_t bleLen = boundedStrLen(cfg.ble_name, sizeof(cfg.ble_name));
  if (bleLen >= 1 && bleLen <= 26) {
    boundedStrCopy(ble_name, sizeof(ble_name), cfg.ble_name);
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
  boundedStrCopy(cfg.ap_ssid, sizeof(cfg.ap_ssid), ap_ssid);
  boundedStrCopy(cfg.ap_pass, sizeof(cfg.ap_pass), ap_pass);
  boundedStrCopy(cfg.ble_name, sizeof(cfg.ble_name), ble_name);
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

//==========================
// Config Validation Helpers
//==========================
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
      boundedStrCopy(ap_ssid, sizeof(ap_ssid), rawValue.c_str());
      apChanged = true;
    }
    Serial.println("[CFG] AP SSID updated to: " + String(ap_ssid));
    return true;
  } else if (key.equalsIgnoreCase("ap_pass")) {
    if (String(ap_pass) != rawValue) {
      boundedStrCopy(ap_pass, sizeof(ap_pass), rawValue.c_str());
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
