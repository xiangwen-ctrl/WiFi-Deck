/*
 * ble.h — BLE GATT Nordic UART Service (NUS), TX credit flow control, GAP coex & Serial/BLE command queues
 * Part of WiFi-Deck multi-file refactor.
 */
#pragma once

#include <Arduino.h>
#include "BLEDevice.h"

extern "C" {
#include "gap.h"
#include "gap_le.h"
#include "gap_adv.h"
#include "gap_config.h"
#include "gap_conn_le.h"
#include "gap_msg.h"
#include "gap_callback_le.h"
#include "gap_bond_le.h"
#include "gatt_builtin_services.h"
}

//==========================================================
// BLE (Nordic UART Service) — Android App control channel
//==========================================================
#define UART_SERVICE_UUID       "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX  "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX  "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
#define BLE_BUF_SIZE            220   // < CHAR_VALUE_MAX_LEN (230)

// Adaptive BLE sniff-log congestion control & pacing (P0-2)
#define BLE_SNIFF_BASE_INTERVAL_MS     100UL
#define BLE_SNIFF_LOW_MTU_INTERVAL_MS  220UL
#define BLE_SNIFF_MAX_BACKOFF_MS       800UL

// Cross-thread command ring queue (BLE task -> loop)
#define CMD_RING_SIZE 16

// Non-blocking Serial line buffer & pending command queue (main loop only)
#define SERIAL_CMD_QUEUE_SIZE 8

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
  uint16_t initRxPhy = 0;

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

// BLE objects
extern BLEService UartService;
extern BLECharacteristic BleRx;
extern BLECharacteristic BleTx;
extern BLEAdvertData advdata;
extern BLEAdvertData scndata;

// BLE state flags (volatile — accessed from BLE task & main loop)
extern volatile bool bleNotifyEnabled;
extern volatile uint8_t activeConnId;
extern volatile bool bleStarted;
extern volatile bool bleAdvUpdateInProgress;
extern volatile T_GAP_CONN_STATE bleGapConnState;

// GAP diagnostics
extern BleGapTelemetry bleGapDiag;
extern volatile uint32_t bleDroppedLogCount;
extern unsigned long lastBleDroppedWarnMs;

// Adaptive congestion control pacing
extern unsigned long bleLogBackoffMs;
extern unsigned long lastBleRoutineLogMs;
extern unsigned long lastBlePriorityLogMs;

// Cross-thread command ring queue (BLE task -> loop)
extern char cmdRing[CMD_RING_SIZE][BLE_BUF_SIZE];
extern volatile uint8_t cmdHead;
extern volatile uint8_t cmdTail;

// Non-blocking Serial line buffer & pending command queue
extern char serialRxBuf[BLE_BUF_SIZE];
extern size_t serialRxLen;
extern bool serialRxOverflow;
extern char serialCmdQueue[SERIAL_CMD_QUEUE_SIZE][BLE_BUF_SIZE];
extern uint8_t serialCmdHead;
extern uint8_t serialCmdTail;

// BLE command queue diagnostics
extern volatile bool bleCmdQueueOverflow;
extern volatile bool bleCmdOversizeDrop;

// ---- Function prototypes ----

void getBleStateSafe(uint8_t &connIdOut, bool &notifyOut);
bool isBleReadyToNotifySafe(uint8_t &connIdOut);
bool isBleConnectedSafe();
uint16_t getEffectiveBleAttMtu(uint8_t connId);
bool waitForBleTxCredit(uint8_t connId);

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

void bleWriteCB(BLECharacteristic* chr, uint8_t connID);
void bleNotifCB(BLECharacteristic* chr, uint8_t connID, uint16_t cccd);

void processBleDiagAndNegotiation();
void ensureBleAdvertisingIfDisconnected();
void updateBleName(const char *newName);
void setupBLE();
void resetBleLogFlowControl();

void pollSerialInput();
bool isStopCmdCstr(const char *s);
bool hasQueuedStopInCmdRing();
bool consumePriorityStopIfPending();