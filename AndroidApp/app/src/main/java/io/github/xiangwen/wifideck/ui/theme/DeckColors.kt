package io.github.xiangwen.wifideck.ui.theme

import androidx.compose.ui.graphics.Color

object DeckColors {
    val BgPrimary = Color(0xFF0D1117)
    val BgSurface = Color(0xFF161B22)
    val BgElevated = Color(0xFF1F2631)
    val BgTerminal = Color(0xFF090C10)
    val BorderSubtle = Color(0xFF2B323C)

    val Crimson = Color(0xFFE94560)
    val CrimsonDim = Color(0xFF2D161E)

    val Green = Color(0xFF2EA043)
    val BrightGreen = Color(0xFF3FB950)
    val GreenDim = Color(0xFF12261A)

    val Blue = Color(0xFF2F81F7)
    val BlueDim = Color(0xFF14233A)

    val Purple = Color(0xFF8957E5)
    val PurpleDim = Color(0xFF231934)

    val Amber = Color(0xFFD29922)
    val AmberDim = Color(0xFF2E2210)

    val Cyan = Color(0xFF39C5BB)
    val CyanDim = Color(0xFF102828)

    val TextPrimary = Color(0xFFF0F6FC)
    val TextSecondary = Color(0xFF8B949E)
    val TextMuted = Color(0xFF6E7681)

    fun rssiColor(rssi: Int): Color = when {
        rssi >= -60 -> BrightGreen
        rssi >= -75 -> Amber
        else -> Crimson
    }

    fun rssiProgress(rssi: Int): Float {
        // Maps -100 dBm..-35 dBm to 0.08f..1.0f
        return ((rssi + 100).coerceIn(5, 65) / 65f)
    }
}
