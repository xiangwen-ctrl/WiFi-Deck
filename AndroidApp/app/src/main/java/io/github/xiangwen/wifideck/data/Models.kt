package io.github.xiangwen.wifideck.data

/** 固件 BLE JSON 上行消息类型（对应字段 "t"） */
enum class BleMessageType(val wireType: String) {
    SCAN_BEGIN("scan_begin"),
    SCAN_END("scan_end"),
    SYNC_BEGIN("sync_begin"),
    SYNC_END("sync_end"),
    AP("ap"),
    STATUS("st"),
    CONFIG("cfg"),
    IDENTITY("id"),
    LOG("log");

    companion object {
        fun fromWireType(type: String): BleMessageType? =
            entries.firstOrNull { it.wireType == type }
    }
}

/** 802.11 混杂抓包过滤模式（与固件 enum SniffMode 0..6 严格对齐） */
enum class SniffMode(val code: Int, val commandArg: String, val label: String) {
    ALL(0, "all", "ALL 全量帧"),
    BEACON(1, "beacon", "Beacon 信标"),
    PROBE(2, "probe", "Probe 探测"),
    DEAUTH(3, "deauth", "Deauth 解认证"),
    EAPOL(4, "eapol", "EAPOL 握手包"),
    PWNAGOTCHI(5, "pwnagotchi", "Pwnagotchi"),
    STOP(6, "stop", "混杂模式");

    companion object {
        fun fromCode(code: Int): SniffMode =
            entries.firstOrNull { it.code == code } ?: STOP
    }
}

/** 单个 Wi-Fi 热点（对应固件 JSON Lines 里的 "ap" 消息） */
data class ApItem(
    val index: Int,
    val ssid: String,
    val bssid: String,
    val channel: Int,
    val rssi: Int,
    val selected: Boolean = false
) {
    val band: String get() = if (channel >= 36) "5G" else "2.4G"
}

/** 设备状态（对应固件 "st" 状态心跳） */
data class DeviceStatus(
    val attackEnabled: Boolean = false,
    val disassocEnabled: Boolean = false,
    val beaconFlood: Boolean = false,
    val probeFlood: Boolean = false,
    val signalJam: Boolean = false,
    val deauthSniff: Boolean = false,
    val sniffing: Boolean = false,
    val sniffMode: Int = SniffMode.STOP.code,
    val hopping: Boolean = false,
    val channel: Int = 0,
    val txOk: Long = 0,
    val txFail: Long = 0,
    val timedRemainingSec: Int = 0
) {
    val attacking: Boolean
        get() = attackEnabled || disassocEnabled || beaconFlood || probeFlood || signalJam || deauthSniff

    val isSniffing: Boolean
        get() = sniffing

    val isBusy: Boolean
        get() = attacking || sniffing

    val sniffModeEnum: SniffMode
        get() = SniffMode.fromCode(sniffMode)
}

/** 设备配置（对应固件 "cfg" 与 "id" 消息） */
data class DeviceConfig(
    val cycleDelayMs: Long = 0,
    val scanTimeMs: Long = 0,
    val numSendFrames: Int = 0,
    val startChannel: Int = 0,
    val scanBetweenCycles: Boolean = false,
    val ledEnabled: Boolean = true,
    val apEnabled: Boolean = true,
    val apSsid: String = "WiFi-Deck",
    val apPass: String = "12345678",
    val bleName: String = "WiFi-Deck"
)

/** 配置握手双帧同步状态（需同时收到完整 "cfg" 与 "id" 才视为已同步） */
data class ConfigSyncState(
    val cfgReceived: Boolean = false,
    val idReceived: Boolean = false
) {
    val isSynced: Boolean get() = cfgReceived && idReceived
}

/** BLE 扫描/连接前置门禁引导弹窗状态（权限解释、永久拒绝跳转、蓝牙开关、Android 8~11 定位开关） */
sealed interface BleGatePrompt {
    data class PermissionRationale(val isAndroid12Plus: Boolean) : BleGatePrompt
    data class PermissionPermanentlyDenied(val isAndroid12Plus: Boolean) : BleGatePrompt
    data object BluetoothDisabled : BleGatePrompt
    data object LocationDisabled : BleGatePrompt
}

/** 抓包日志预解析分类（避免 Compose 列表行内重复字符串匹配） */
enum class SniffLogCategory(val tagLabel: String, val isHighlight: Boolean) {
    EAPOL("EAPOL ", true),
    DEAUTH("DEAUTH", true),
    BEACON("BEACON", false),
    PROBE("PROBE ", false),
    FRAME("FRAME ", false);

    companion object {
        fun parse(rawLog: String): SniffLogCategory = when {
            rawLog.contains("EAPOL", ignoreCase = true) -> EAPOL
            rawLog.contains("Deauth", ignoreCase = true) ||
                rawLog.contains("Disassoc", ignoreCase = true) -> DEAUTH
            rawLog.contains("Beacon", ignoreCase = true) -> BEACON
            rawLog.contains("Probe", ignoreCase = true) -> PROBE
            else -> FRAME
        }
    }
}

/** 抓包终端日志条目：id 全局单调递增供 LazyColumn 稳定 key，seq 与预解析展示字段消除滑窗重绘 */
data class SniffLogEntry(
    val id: Long,
    val seq: Long,
    val formattedSeq: String,
    val category: SniffLogCategory,
    val displayText: String,
    val text: String
) {
    companion object {
        fun fromRaw(id: Long, seq: Long, rawLog: String): SniffLogEntry {
            val category = SniffLogCategory.parse(rawLog)
            val displayText = rawLog
                .removePrefix("[MGMT] ")
                .removePrefix("[DATA] ")
                .removePrefix("[CTRL] ")
                .removePrefix("[EXT] ")
            return SniffLogEntry(
                id = id,
                seq = seq,
                formattedSeq = "%03d".format(seq),
                category = category,
                displayText = displayText,
                text = rawLog
            )
        }
    }
}

/** 扫描页列表与频段计数派生状态（在 ViewModel 内合批计算，减少 Compose 重组与过滤排序开销） */
data class ScanListState(
    val totalCount: Int = 0,
    val count24G: Int = 0,
    val count5G: Int = 0,
    val filteredAps: List<ApItem> = emptyList()
)
