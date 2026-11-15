package io.github.xiangwen.wifideck.ui.theme

import androidx.compose.ui.graphics.Color

object DeckColors {
    // 硬件仪器级深色基底 (Matte Obsidian & Graphite)
    val BgPrimary = Color(0xFF0B0D12)
    val BgSurface = Color(0xFF131720)
    val BgElevated = Color(0xFF1C212E)
    val BgTerminal = Color(0xFF07090D)
    val BorderSubtle = Color(0xFF242B3A)
    val BorderStrong = Color(0xFF333D52)

    // 核心信号色 (克制、高辨识度)
    val Crimson = Color(0xFFF43F5E)       // 主控/目标锁定/压制
    val CrimsonDim = Color(0xFF2D1420)

    val Green = Color(0xFF10B981)         // 运行态/正常连接/强信号
    val BrightGreen = Color(0xFF34D399)
    val GreenDim = Color(0xFF0F2922)

    val Blue = Color(0xFF38BDF8)          // 2.4GHz 频段/扫描操作
    val BlueDim = Color(0xFF102436)

    val Purple = Color(0xFF818CF8)        // 5GHz 频段/混杂抓包
    val PurpleDim = Color(0xFF1B1E38)

    val Amber = Color(0xFFF59E0B)         // EAPOL 握手包/中信号/警告
    val AmberDim = Color(0xFF2B2111)

    val Cyan = Color(0xFF2DD4BF)          // 信道/遥测高亮
    val CyanDim = Color(0xFF0F292A)

    // 字体层级色
    val TextPrimary = Color(0xFFF3F4F6)
    val TextSecondary = Color(0xFF9CA3AF)
    val TextMuted = Color(0xFF64748B)

    fun rssiColor(rssi: Int): Color = when {
        rssi >= -60 -> BrightGreen
        rssi >= -75 -> Amber
        else -> Crimson
    }

    fun rssiBars(rssi: Int): Int = when {
        rssi >= -55 -> 4
        rssi >= -67 -> 3
        rssi >= -78 -> 2
        else -> 1
    }

    fun rssiProgress(rssi: Int): Float {
        return ((rssi + 100).coerceIn(6, 65) / 65f)
    }
}
