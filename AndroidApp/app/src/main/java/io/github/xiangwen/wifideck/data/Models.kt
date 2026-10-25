package io.github.xiangwen.wifideck.data

/** 单个 Wi-Fi 热点（对应固件 JSON Lines 里的 "ap" 消息） */
data class ApItem(
    val index: Int,
    val ssid: String,
    val bssid: String,
    val channel: Int,
    val rssi: Int
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
    val sniffMode: Int = 6,
    val hopping: Boolean = false,
    val channel: Int = 0,
    val txOk: Long = 0,
    val txFail: Long = 0
) {
    val attacking: Boolean
        get() = attackEnabled || disassocEnabled || beaconFlood || probeFlood || signalJam || deauthSniff
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
