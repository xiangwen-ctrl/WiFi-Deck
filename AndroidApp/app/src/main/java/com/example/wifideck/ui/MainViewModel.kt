package com.example.wifideck.ui

import androidx.lifecycle.ViewModel
import com.example.wifideck.data.ApItem
import com.example.wifideck.data.BleManager
import com.example.wifideck.data.DeviceConfig
import com.example.wifideck.data.DeviceStatus
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import org.json.JSONObject

class MainViewModel : ViewModel() {

    val connectionState: StateFlow<BleManager.ConnectionState> = BleManager.connectionState

    private val _apList = MutableStateFlow<List<ApItem>>(emptyList())
    val apList: StateFlow<List<ApItem>> = _apList

    private val _status = MutableStateFlow(DeviceStatus())
    val status: StateFlow<DeviceStatus> = _status

    private val _config = MutableStateFlow(DeviceConfig())
    val config: StateFlow<DeviceConfig> = _config

    private val _logs = MutableStateFlow<List<String>>(emptyList())
    val logs: StateFlow<List<String>> = _logs

    private val _scanning = MutableStateFlow(false)
    val scanning: StateFlow<Boolean> = _scanning

    private val _selected = MutableStateFlow<Set<Int>>(emptySet())
    val selected: StateFlow<Set<Int>> = _selected

    private val _toast = MutableStateFlow<String?>(null)
    val toast: StateFlow<String?> = _toast

    init {
        BleManager.onLine = ::handleLine
        BleManager.onScanError = { msg -> showToast(msg) }
    }

    private fun handleLine(line: String) {
        try {
            val obj = JSONObject(line)
            when (obj.optString("t")) {
                "scan_begin" -> {
                    _apList.value = emptyList()
                    _scanning.value = true
                }
                "ap" -> {
                    val item = ApItem(
                        index = obj.optInt("i"),
                        ssid = obj.optString("s", "(隐藏)"),
                        bssid = obj.optString("b", ""),
                        channel = obj.optInt("c"),
                        rssi = obj.optInt("r")
                    )
                    _apList.update { it + item }
                }
                "scan_end" -> {
                    _scanning.value = false
                    showToast("扫描完成: ${obj.optInt("n")} 个网络")
                }
                "st" -> {
                    _status.value = DeviceStatus(
                        attackEnabled = obj.optInt("atk") != 0,
                        disassocEnabled = obj.optInt("dis") != 0,
                        beaconFlood = obj.optInt("bcn") != 0,
                        probeFlood = obj.optInt("prb") != 0,
                        signalJam = obj.optInt("jam") != 0,
                        deauthSniff = obj.optInt("snf") != 0,
                        sniffing = obj.optInt("snfOn") != 0,
                        hopping = obj.optInt("hop") != 0,
                        channel = obj.optInt("ch"),
                        txOk = obj.optLong("ok"),
                        txFail = obj.optLong("fail")
                    )
                }
                "cfg" -> {
                    _config.value = DeviceConfig(
                        cycleDelayMs = obj.optLong("delay"),
                        scanTimeMs = obj.optLong("scan"),
                        numSendFrames = obj.optInt("frames"),
                        startChannel = obj.optInt("ch"),
                        scanBetweenCycles = obj.optInt("sc") != 0,
                        ledEnabled = obj.optInt("led") != 0
                    )
                }
                "log" -> {
                    val msg = obj.optString("m")
                    _logs.update { (it + msg).takeLast(500) }
                }
            }
        } catch (_: Exception) {
            // 忽略无法解析的行
        }
    }

    fun showToast(msg: String) {
        _toast.value = msg
    }

    fun clearToast() {
        _toast.value = null
    }

    fun toggleSelect(index: Int) {
        val cur = _selected.value
        _selected.value = if (cur.contains(index)) cur - index else cur + index
    }

    fun clearSelection() {
        _selected.value = emptySet()
    }

    fun clearLogs() {
        _logs.value = emptyList()
    }

    // ---- 命令发送（复用固件 handleCommand 指令集）----

    fun sendScan() {
        clearSelection()
        BleManager.sendCommand("scan")
    }

    fun sendStartDeauth() {
        if (_selected.value.isEmpty()) {
            showToast("请先在扫描页选择至少一个目标")
            return
        }
        applyTargets()
        BleManager.sendCommand("start deauther")
    }

    fun sendDisassoc() {
        if (_selected.value.isEmpty()) {
            showToast("请先在扫描页选择至少一个目标")
            return
        }
        applyTargets()
        BleManager.sendCommand("disassoc")
    }

    fun sendRandomAttack() {
        BleManager.sendCommand("random_attack")
    }

    fun sendStopAll() {
        BleManager.sendCommand("stop")
    }

    fun sendTimedAttack(durationMs: Long) {
        BleManager.sendCommand("attack_time $durationMs")
    }

    fun sendBeaconFlood() {
        BleManager.sendCommand("beacon_flood")
    }

    fun sendProbeFlood() {
        BleManager.sendCommand("probe_flood")
    }

    fun sendSignalJam() {
        BleManager.sendCommand("signal_jam")
    }

    fun sendDeauthSniff() {
        BleManager.sendCommand("deauth_sniff")
    }

    fun sendSingleAp() {
        val first = _selected.value.minOrNull()
        if (first == null) {
            showToast("请先在扫描页选择一个目标")
            return
        }
        BleManager.sendCommand("single_ap $first")
    }

    fun sendStartSniff() {
        BleManager.sendCommand("start sniff")
    }

    fun sendStopSniff() {
        BleManager.sendCommand("stop sniff")
    }

    fun sendSniffMode(mode: String) {
        BleManager.sendCommand("sniff $mode")
    }

    fun sendHopToggle(on: Boolean) {
        BleManager.sendCommand(if (on) "hop on" else "hop off")
    }

    fun sendLedToggle(on: Boolean) {
        BleManager.sendCommand(if (on) "set led on" else "set led off")
    }

    fun sendSetFrames(n: Int) {
        BleManager.sendCommand("set num_frames $n")
    }

    fun sendSetCycleDelay(ms: Long) {
        BleManager.sendCommand("set cycle_delay $ms")
    }

    fun sendSetScanTime(ms: Long) {
        BleManager.sendCommand("set scan_time $ms")
    }

    fun sendSetStartChannel(ch: Int) {
        BleManager.sendCommand("set start_channel $ch")
    }

    fun sendSetScanCycles(on: Boolean) {
        BleManager.sendCommand(if (on) "set scan_cycles on" else "set scan_cycles off")
    }

    private fun applyTargets() {
        val indices = _selected.value.sorted().joinToString(",")
        BleManager.sendCommand("set target $indices")
    }
}
