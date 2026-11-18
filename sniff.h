/*
 * sniff.h — 802.11 promiscuous mode sniffer, channel hopping & thread-safe log ring buffers
 * Part of WiFi-Deck multi-file refactor.
 */
#pragma once

#include <Arduino.h>
#include <cstdint>
#include <cstddef>
#include "osdep_service.h"

// =========================
// Sniff Log Ring Defines
// =========================
#define LOG_STR_LEN             128
#define LOG_RING_SIZE           24
#define WEB_LOG_HISTORY_SIZE    32

// =========================
// Sniff Mode Enum
// =========================
enum SniffMode {
  SNIFF_ALL,
  SNIFF_BEACON,
  SNIFF_PROBE,
  SNIFF_DEAUTH,
  SNIFF_EAPOL,
  SNIFF_PWNAGOTCHI,
  SNIFF_STOP
};

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

// =========================
// Promiscuous Callback Flight Guard (RAII)
// =========================
struct PromiscFlightGuard {
  PromiscFlightGuard();
  ~PromiscFlightGuard();
};

// =========================
// Extern Variable Declarations
// =========================
extern volatile SniffMode currentMode;
extern volatile bool isSniffing;

// Channel hopping (explicit bounds allow sizeof() & range-based for across modules)
extern volatile bool isHopping;
extern unsigned long lastHopTime;
extern const unsigned long HOP_INTERVAL;
extern const int CHANNELS_2GHZ[13];
extern const int CHANNELS_5GHZ[25];
extern int currentChannelIndex;
extern bool hopUse5GHz;
extern bool hopResetPending;
extern volatile int currentChannel;

// Cross-thread sniff log ring buffer (WiFi promisc thread -> loop)
extern char sniffLogRing[LOG_RING_SIZE][LOG_STR_LEN];
extern volatile uint8_t logHead;
extern volatile uint8_t logTail;
extern volatile uint32_t droppedLogCount;

// WebUI sniff log history buffer & monotonic sequence cursor (for /api/logs?since=<seq>)
extern char webLogHistory[WEB_LOG_HISTORY_SIZE][LOG_STR_LEN];
extern volatile uint8_t webLogHead;
extern volatile uint8_t webLogCount;
extern volatile uint32_t webLogTotalSeq;

// Promiscuous callback flight tracking & emergency stop flags
extern volatile uint32_t promiscCallbackInFlight;
extern volatile bool emergencyStopRequested;
extern volatile bool priorityStopPending;

// RX callback per-category log throttle timestamps
extern volatile unsigned long lastMgmtBeaconLogMs;
extern volatile unsigned long lastMgmtProbeLogMs;
extern volatile unsigned long lastCtrlLogMs;
extern volatile unsigned long lastDataLogMs;

// Deauth sniff-and-clone state
extern volatile bool deauth_sniff_enabled;
extern bool deauth_sniff_prev_hop;
extern uint8_t sniffed_deauth_src[6];
extern volatile int sniffed_deauth_ch;
extern volatile bool has_sniffed_deauth;
extern unsigned long last_deauth_sniff_attack;
extern unsigned long deauth_sniff_interval;

// =========================
// Inline Utility Functions
// =========================
static inline uint8_t ieee80211_get_type(uint16_t fc) {
  return (fc & 0x0C) >> 2;
}
static inline uint8_t ieee80211_get_subtype(uint16_t fc) {
  return (fc & 0xF0) >> 4;
}
static inline bool is_broadcast_mac(const uint8_t *mac) {
  return mac[0]==0xFF && mac[1]==0xFF && mac[2]==0xFF && mac[3]==0xFF && mac[4]==0xFF && mac[5]==0xFF;
}

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

// =========================
// Function Prototypes
// =========================
void promisc_callback(unsigned char *buf, unsigned int len, void *userdata);
void resetPromiscLogThrottle();
void extractSsidCstr(const uint8_t *frame, int totalLen, int possibleOffset, char *out, size_t outSize);
bool isPwnagotchiMac(const uint8_t *mac);
bool isEAPOL(const uint8_t *buf, int len);

bool setChannel(int newChannel);
void resetHopState();
void hopChannel();

void startSniffing();
void stopSniffing();
bool haltPromiscSniff();
void waitForPromiscCallbacksIdle();
void clearDeauthSniffState();
void prepareRadioForSniff(bool turnOffAp);
void startMonitorSniff(SniffMode mode, bool timed = false, unsigned long dur = 0);

void pushSniffLog(const char *msg);
void clearSniffLogRingSafe();
