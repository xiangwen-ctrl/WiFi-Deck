#include "ble.h"
#include "config.h"
#include "utils.h"
#include "sniff.h"
#include "wifi_scan.h"
#include "wifi_attack.h"
#include "command.h"

#include <strings.h>
#include "wifi_conf.h"
#include "wifi_util.h"
#include "osdep_service.h"
#include "FreeRTOS.h"
#include "wiring_watchdog.h"

extern "C" {
#include "bt_intf.h"
#include "app_msg.h"
}

//==========================================================
// BLE object definitions
//==========================================================
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

BleGapTelemetry bleGapDiag;
volatile uint32_t bleDroppedLogCount = 0;
unsigned long lastBleDroppedWarnMs = 0;

unsigned long bleLogBackoffMs = BLE_SNIFF_BASE_INTERVAL_MS;
unsigned long lastBleRoutineLogMs = 0;
unsigned long lastBlePriorityLogMs = 0;

// Cross-thread command ring queue (BLE task -> loop)
char cmdRing[CMD_RING_SIZE][BLE_BUF_SIZE];
volatile uint8_t cmdHead = 0;
volatile uint8_t cmdTail = 0;

// Non-blocking Serial line buffer & pending command queue (main loop only)
char serialRxBuf[BLE_BUF_SIZE];
size_t serialRxLen = 0;
bool serialRxOverflow = false;
char serialCmdQueue[SERIAL_CMD_QUEUE_SIZE][BLE_BUF_SIZE];
uint8_t serialCmdHead = 0;
uint8_t serialCmdTail = 0;

volatile bool bleCmdQueueOverflow = false;
volatile bool bleCmdOversizeDrop = false;

//==========================================================
// BLE helpers (Nordic UART Service)
//==========================================================

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

/*
 * BLE GATT TX Credit Headroom & Backpressure Gate
 *
 * AmebaD's BLE stack shares a finite LE LL buffer credit pool (`GAP_PARAM_LE_REMAIN_CREDITS`,
 * typically 8-10 credits). Exhausting credits with non-critical packet logs starves the 1s
 * `st` status heartbeat and Link-Layer supervision frames, causing Android 0x0108 timeouts.
 *
 * Strategy:
 * - Auto-calibrates `maxSeenCredits` to support varying controller buffer sizes.
 * - Critical control frames (`cfg`, `id`, `scan_begin/ap/scan_end`) and high-priority security
 *   alerts (`EAPOL`, `Deauth`, `Disassoc`, `Pwnagotchi`) require `minCredits = 1` (with up to
 *   8ms cooperative wait for critical control lines).
 * - Routine sniff logs require `minCredits = 3` (`waitMs = 0` non-blocking drop + AIMD backoff
 *   100ms..800ms in `loop()`), guaranteeing >= 2 credits remain reserved for control/heartbeats.
 */
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
      } else if (maxSeenCredits == 3 && effectiveMin > 2) {
        effectiveMin = 2;
      } else if (maxSeenCredits >= 4 && effectiveMin >= maxSeenCredits) {
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
  // Routine logs require >=4 free credits (auto-scaled on smaller pools) to leave ample headroom for heartbeat/control;
  // high-priority security alerts (EAPOL/Deauth/Disassoc/Pwnagotchi) require >=2 credits so LL supervision never starves.
  uint16_t minCredits = highPriority ? 2 : 4;
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
            if ((ci < 24 || ct < 400) && bleGapDiag.negotiateAttempts < 3) {
              bleGapDiag.negotiatePending = true;
            }
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
        // 1C: 拦截过紧的连接参数请求（interval < 30ms、supv_tout < 4000ms 或 latency > 10）
        // 双频扫描/跳频期间 5GHz 批次最长驻留 ~700-960ms，必须保证 supervision_timeout >= 4000ms
        bool reject = (cmin < 24 || ctout < 400 || clat > 10);
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
    enforceBleCoexPta(!softApActive);
  }
  if (snap.evDisconnected) {
    cancelBleScanPush();
    clearSniffLogRingSafe();
    resetBleLogFlowControl();
    char buf[80];
    snprintf(buf, sizeof(buf), "[BLE] Disconnected conn=%u disc_cause=0x%04X",
             (unsigned)snap.discConnId, (unsigned)snap.discCause);
    Serial.println(buf);
    enforceBleCoexPta(!softApActive);
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
    enforceBleCoexPta(!softApActive);
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

  // 1C: CCCD 订阅完成 (>=750ms) 或建链 1.5s 后协商双频抗扰连接参数：
  // interval 30-50ms (24..40*1.25ms) 确保每次 75-85ms 2.4GHz 喘息窗口内必含 >=1-2 个 BLE 锚点，
  // slave_latency=4 降低静置功耗，supervision_timeout=6000ms (600*10ms) 提供充裕跨频容错余量。
  uint8_t curStateCid = 0;
  bool notifyReady = false;
  getBleStateSafe(curStateCid, notifyReady);
  bool readyForParamReq = (nowMs - snap.connectedAtMs >= 1500UL) ||
                          (notifyReady && curStateCid == snap.connId && (nowMs - snap.connectedAtMs >= 750UL));
  if (snap.negotiatePending && BLE.connected(snap.connId) && readyForParamReq) {
    uint16_t curInt = 0, curLat = 0, curTout = 0;
    le_get_conn_param(GAP_PARAM_CONN_INTERVAL, &curInt, snap.connId);
    le_get_conn_param(GAP_PARAM_CONN_LATENCY, &curLat, snap.connId);
    le_get_conn_param(GAP_PARAM_CONN_TIMEOUT, &curTout, snap.connId);
    if (curInt >= 24 && curInt <= 72 && curLat <= 6 && curTout >= 450) {
      rtw_enter_critical(NULL, NULL);
      bleGapDiag.negotiatePending = false;
      rtw_exit_critical(NULL, NULL);
    } else if (snap.negotiateAttempts < 3 &&
               (snap.lastNegotiateMs == 0 || (nowMs - snap.lastNegotiateMs >= 2000UL))) {
      uint16_t targetTout = (snap.negotiateAttempts < 2) ? 600 : 500;
      T_GAP_CAUSE cause = le_update_conn_param(snap.connId, 24, 40, 4, targetTout, 46, 78);
      rtw_enter_critical(NULL, NULL);
      bleGapDiag.negotiateAttempts++;
      bleGapDiag.lastNegotiateMs = nowMs;
      if (bleGapDiag.negotiateAttempts >= 3) {
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
  restoreBleCoex(true);
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
      jsonEscapeTruncatedToBuf(ap.ssid, escSsid, sizeof(escSsid), 96, "...");
      char line[256];
      int n = snprintf(line, sizeof(line),
                       "{\"t\":\"ap\",\"i\":%lu,\"s\":\"%s\",\"b\":\"%s\",\"c\":%u,\"r\":%d%s}",
                       (unsigned long)bleScanPush.idx,
                       escSsid,
                       ap.bssid_str,
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

  char line[240];
  int n = snprintf(line, sizeof(line),
                   "{\"t\":\"st\",\"atk\":%d,\"dis\":%d,\"bcn\":%d,\"prb\":%d,\"jam\":%d,\"snf\":%d,\"snfOn\":%d,\"sMode\":%d,\"hop\":%d,\"ch\":%d,\"ok\":%lu,\"fail\":%lu,\"rem\":%lu,\"ap\":%d}",
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
                   remSec,
                   apEnabled ? 1 : 0);
  if (n > 0 && (size_t)n < sizeof(line)) {
    bleSendLineCstr(line, (size_t)n, critical, !critical);
  }
}

void blePushInfo() {
  uint8_t connId = 0;
  if (!isBleReadyToNotifySafe(connId)) return;
  char cfgLine[192];
  int cn = snprintf(cfgLine, sizeof(cfgLine),
                    "{\"t\":\"cfg\",\"delay\":%lu,\"scan\":%lu,\"frames\":%lu,\"ch\":%d,\"sc\":%d,\"led\":%d,\"ap\":%d,\"ver\":\"%s\"}",
                    cycle_delay,
                    scan_time,
                    num_send_frames,
                    start_channel,
                    scan_between_cycles ? 1 : 0,
                    USE_LED ? 1 : 0,
                    apEnabled ? 1 : 0,
                    FIRMWARE_VERSION);
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
