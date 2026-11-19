/*
 * wifi_attack.h — Frame injection, attack orchestration & radio management
 * Part of WiFi-Deck multi-file refactor.
 */
#pragma once

#include <Arduino.h>
#include <cstdint>
#include <cstddef>
#include <vector>

#include "wifi_scan.h"   // WiFiScanResult, SlicedTxCycle
#include "sniff.h"       // SniffMode

//==========================================================
// Frame Structures
//==========================================================
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
// Realtek SDK Externs
//==========================================================
extern uint8_t* rltk_wlan_info;
extern "C" void* alloc_mgtxmitframe(void* ptr);
extern "C" void update_mgntframe_attrib(void* ptr, void* frame_control);
extern "C" int dump_mgntframe(void* ptr, void* frame_control);
extern "C" void rtw_free_xmitbuf(void* pxmitpriv, void* pxmitbuf);
extern "C" void rtw_free_xmitframe(void* pxmitpriv, void* pxmitframe);
extern "C" int wifi_get_mac_address(char *mac);

//==========================================================
// Reason Code Arrays (extern)
//==========================================================
extern const uint16_t deauth_reasons[];
extern const uint16_t disassoc_reasons[];
extern const uint8_t num_deauth_reasons;
extern const uint8_t num_disassoc_reasons;

//==========================================================
// Disassociation Attack State (extern)
//==========================================================
extern bool disassoc_enabled;
extern unsigned long disassoc_interval;
extern unsigned long last_disassoc_attack;

//==========================================================
// Beacon / Probe Flood Constants
//==========================================================
#define BEACON_MAX_SSID_LEN 20

//==========================================================
// Raw Frame Injection
//==========================================================
void wifi_tx_raw_frame(void* frame, size_t length);
void wifi_tx_deauth_frame(const void* src_mac, const void* dst_mac, uint16_t reason = 0x06);
void wifi_tx_disassoc_frame(const void* src_mac, const void* dst_mac, uint16_t reason = 0x08);
void wifi_tx_beacon_frame(const char* ssid, int ssid_len, uint8_t channel);
void wifi_tx_probe_req_frame(const char* ssid, int ssid_len);

//==========================================================
// Attack Orchestration
//==========================================================
void targetAttack();
void startTimedAttack(unsigned long durationMs);
void checkTimedAttack();
bool shouldAbortCycle();
void stopAllActiveAttacks();
void stopConflictingModesBeforeAttack();
bool stepSlicedTxCycle(SlicedTxCycle &st, bool sendDeauth, bool sendDisassoc, unsigned long frameBudgetPerTick);

//==========================================================
// Radio Management
//==========================================================
void enforceBleCoexPta(bool forceBlePriorityTable = false);
void restoreBleCoex(bool resetCoex = true);
void refreshBleCoexOnBandSwitch(int prevCh, int newCh);
void serviceBleBreathingWindow(unsigned long windowMs, bool parkOn2G = true);
void maintainBle5GHzKeepalive();
void restoreDefaultChannelIfNeeded(bool force = false);
void restoreRadioAfterSniff();
bool startSoftAP();
bool enterStaMode(const char *context);
bool isSoftApHealthy();
void scheduleApRestart();
