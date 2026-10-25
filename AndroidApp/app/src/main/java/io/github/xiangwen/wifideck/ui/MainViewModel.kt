package io.github.xiangwen.wifideck.ui

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import io.github.xiangwen.wifideck.data.ApItem
import io.github.xiangwen.wifideck.data.BleManager
import io.github.xiangwen.wifideck.data.DeviceConfig
import io.github.xiangwen.wifideck.data.DeviceStatus
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import org.json.JSONObject

enum class BandFilter { ALL, G24, G5 }
enum class SortMode { RSSI, CHANNEL }

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

    private val _bandFilter = MutableStateFlow(BandFilter.ALL)
    val bandFilter: StateFlow<BandFilter> = _bandFilter

    private val _sortMode = MutableStateFlow(SortMode.RSSI)
    val sortMode: StateFlow<SortMode> = _sortMode

    private val _searchQuery = MutableStateFlow("")
    val searchQuery: StateFlow<String> = _searchQuery

    private val _autoScroll = MutableStateFlow(true)
    val autoScroll: StateFlow<Boolean> = _autoScroll

    private val _toast = MutableStateFlow<String?>(null)
    val toast: StateFlow<String?> = _toast

    init {
        BleManager.onLine = ::handleLine
        BleManager.onScanError = { msg -> showToast(msg) }
        viewModelScope.launch {
            BleManager.connectionState.collect { state ->
                if (state == BleManager.ConnectionState.DISCONNECTED) {
                    _scanning.value = false
                    _status.value = DeviceStatus()
                }
            }
        }
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
                        sniffMode = obj.optInt("sMode", 6),
                        hopping = obj.optInt("hop") != 0,
                        channel = obj.optInt("ch"),
                        txOk = obj.optLong("ok"),
                        txFail = obj.optLong("fail")
                    )
                }
                "cfg" -> {
                    _config.update {
                        it.copy(
                            cycleDelayMs = obj.optLong("delay"),
                            scanTimeMs = obj.optLong("scan"),
                            numSendFrames = obj.optInt("frames"),
                            startChannel = obj.optInt("ch"),
                            scanBetweenCycles = obj.optInt("sc") != 0,
                            ledEnabled = obj.optInt("led") != 0,
                            apEnabled = obj.optInt("ap", 1) != 0
                        )
                    }
                }
                "id" -> {
                    _config.update {
                        it.copy(
                            apSsid = obj.optString("ssid", it.apSsid),
                            apPass = obj.optString("pass", it.apPass),
                            bleName = obj.optString("ble", it.bleName)
                        )
                    }
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

    fun setBandFilter(f: BandFilter) {
        _bandFilter.value = f
    }

    fun setSortMode(s: SortMode) {
        _sortMode.value = s
    }

    fun setSearchQuery(q: String) {
        _searchQuery.value = q
    }

    fun toggleAutoScroll() {
        _autoScroll.value = !_autoScroll.value
    }

    fun clearSelection() {
        _selected.value = emptySet()
    }

    fun selectAll() {
        val filter = _bandFilter.value
        val query = _searchQuery.value.trim().lowercase()
        _selected.value = _apList.value
            .filter { ap ->
                val bandMatch = when (filter) {
                    BandFilter.ALL -> true
                    BandFilter.G24 -> ap.channel < 36
                    BandFilter.G5 -> ap.channel >= 36
                }
                val queryMatch = query.isEmpty() ||
                    ap.ssid.lowercase().contains(query) ||
                    ap.bssid.lowercase().contains(query)
                bandMatch && queryMatch
            }
            .map { it.index }
            .toSet()
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
        if (_status.value.attackEnabled) {
            sendStopAll()
            return
        }
        if (_selected.value.isEmpty()) {
            showToast("请先在扫描页选择至少一个目标")
            return
        }
        applyTargets()
        BleManager.sendCommand("start deauther")
    }

    fun sendDisassoc() {
        if (_status.value.disassocEnabled) {
            sendStopAll()
            return
        }
        if (_selected.value.isEmpty()) {
            showToast("请先在扫描页选择至少一个目标")
            return
        }
        applyTargets()
        BleManager.sendCommand("disassoc")
    }

    fun sendRandomAttack() {
        if (_apList.value.isEmpty()) {
            showToast("请先扫描周围 Wi-Fi 网络")
            return
        }
        BleManager.sendCommand("random_attack")
        showToast("已触发随机信道抽样测试")
    }

    fun sendStopAll() {
        BleManager.sendCommand("stop")
        showToast("已发送停止指令")
    }

    fun sendTimedAttack(durationMs: Long) {
        if (_selected.value.isEmpty()) {
            showToast("请先在扫描页选择至少一个目标")
            return
        }
        applyTargets()
        BleManager.sendCommand("attack_time $durationMs")
        showToast("已启动 ${durationMs / 1000} 秒定时测试")
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
        showToast("已对目标 #$first 突发测试帧")
    }

    fun sendResetTxStats() {
        BleManager.sendCommand("reset_txstats")
        showToast("TX 统计计数已清零")
    }

    fun sendStartSniff() {
        if (_status.value.sniffing) {
            sendStopSniff()
            return
        }
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

    fun sendSetChannel(ch: Int) {
        BleManager.sendCommand("set ch $ch")
        showToast("已锁定信道 CH $ch")
    }

    fun sendLedToggle(on: Boolean) {
        BleManager.sendCommand(if (on) "set led on" else "set led off")
    }

    fun sendApToggle(on: Boolean) {
        BleManager.sendCommand(if (on) "ap on" else "ap off")
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

    fun sendSetBleName(name: String) {
        val trimmed = name.trim()
        val byteLen = trimmed.toByteArray(Charsets.UTF_8).size
        if (byteLen !in 1..26) {
            showToast("BLE 蓝牙名称长度需为 1~26 字节")
            return
        }
        BleManager.sendCommand("set ble_name $trimmed")
        showToast("BLE 名称已保存 (断线重连后广播生效)")
    }

    fun sendSetApSsid(ssid: String) {
        val trimmed = ssid.trim()
        val byteLen = trimmed.toByteArray(Charsets.UTF_8).size
        if (byteLen !in 1..32) {
            showToast("Wi-Fi 热点名称长度需为 1~32 字节")
            return
        }
        BleManager.sendCommand("set ap_ssid $trimmed")
        showToast("Wi-Fi 热点名称已保存并重启热点")
    }

    fun sendSetApPass(pass: String) {
        val trimmed = pass.trim()
        if (trimmed.length !in 8..63) {
            showToast("WPA2 热点密码长度需为 8~63 位字符")
            return
        }
        BleManager.sendCommand("set ap_pass $trimmed")
        showToast("Wi-Fi 热点密码已保存并重启热点")
    }

    fun sendResetConfig() {
        BleManager.sendCommand("reset_config")
        showToast("已恢复出厂默认名称与参数")
    }

    fun sendCustomCommand(rawCmd: String) {
        val cmd = rawCmd.trim()
        if (cmd.isEmpty()) return
        BleManager.sendCommand(cmd)
        showToast("已发送指令: $cmd")
    }

    private fun applyTargets() {
        if (_selected.value.size == _apList.value.size && _apList.value.isNotEmpty()) {
            BleManager.sendCommand("set target all")
        } else {
            val indices = _selected.value.sorted().joinToString(",")
            BleManager.sendCommand("set target $indices")
        }
    }
}
