package io.github.xiangwen.wifideck.ui.components

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.BluetoothSearching
import androidx.compose.material.icons.filled.StopCircle
import androidx.compose.material.icons.filled.WifiTethering
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import io.github.xiangwen.wifideck.data.BleManager
import io.github.xiangwen.wifideck.data.DeviceConfig
import io.github.xiangwen.wifideck.data.DeviceStatus
import io.github.xiangwen.wifideck.ui.theme.DeckColors

@Composable
fun TopStatusHeader(
    state: BleManager.ConnectionState,
    status: DeviceStatus,
    config: DeviceConfig,
    apCount: Int,
    selectedCount: Int,
    onConnect: () -> Unit,
    onStopAll: () -> Unit
) {
    val isBusy = status.attacking || status.sniffing
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .background(DeckColors.BgSurface)
    ) {
        // 第一行：品牌标识 + 连接状态胶囊 + 快捷操作
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 14.dp, vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Box(
                modifier = Modifier
                    .size(34.dp)
                    .background(
                        if (status.attacking) DeckColors.CrimsonDim else DeckColors.BlueDim,
                        RoundedCornerShape(8.dp)
                    )
                    .border(
                        1.dp,
                        if (status.attacking) DeckColors.Crimson else DeckColors.Blue.copy(alpha = 0.5f),
                        RoundedCornerShape(8.dp)
                    ),
                contentAlignment = Alignment.Center
            ) {
                Icon(
                    imageVector = Icons.Default.WifiTethering,
                    contentDescription = null,
                    tint = if (status.attacking) DeckColors.Crimson else DeckColors.Blue,
                    modifier = Modifier.size(20.dp)
                )
            }
            Spacer(modifier = Modifier.width(10.dp))
            Column(modifier = Modifier.weight(1f)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        text = config.bleName.ifBlank { "WiFi-Deck" },
                        color = DeckColors.TextPrimary,
                        fontSize = 16.sp,
                        fontWeight = FontWeight.Bold,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis
                    )
                    Spacer(modifier = Modifier.width(6.dp))
                    Text(
                        text = if (config.apEnabled) "2.4G / 5G" else "STEALTH",
                        color = if (config.apEnabled) DeckColors.Cyan else DeckColors.Purple,
                        fontSize = 9.sp,
                        fontWeight = FontWeight.Bold,
                        modifier = Modifier
                            .background(
                                if (config.apEnabled) DeckColors.CyanDim else DeckColors.PurpleDim,
                                RoundedCornerShape(4.dp)
                            )
                            .padding(horizontal = 5.dp, vertical = 2.dp)
                    )
                }
                Spacer(modifier = Modifier.height(2.dp))
                ConnectionBadge(state, status)
            }

            if (isBusy) {
                Button(
                    onClick = onStopAll,
                    colors = ButtonDefaults.buttonColors(containerColor = DeckColors.Crimson),
                    contentPadding = PaddingValues(horizontal = 10.dp, vertical = 0.dp),
                    shape = RoundedCornerShape(8.dp),
                    modifier = Modifier.height(32.dp)
                ) {
                    Icon(
                        imageVector = Icons.Default.StopCircle,
                        contentDescription = null,
                        modifier = Modifier.size(15.dp),
                        tint = Color.White
                    )
                    Spacer(modifier = Modifier.width(4.dp))
                    Text("急停", fontSize = 12.sp, fontWeight = FontWeight.Bold, color = Color.White)
                }
            } else if (state == BleManager.ConnectionState.DISCONNECTED) {
                OutlinedButton(
                    onClick = onConnect,
                    border = BorderStroke(1.dp, DeckColors.Blue),
                    contentPadding = PaddingValues(horizontal = 12.dp, vertical = 0.dp),
                    shape = RoundedCornerShape(8.dp),
                    modifier = Modifier.height(32.dp)
                ) {
                    Icon(
                        imageVector = Icons.AutoMirrored.Filled.BluetoothSearching,
                        contentDescription = null,
                        modifier = Modifier.size(15.dp),
                        tint = DeckColors.Blue
                    )
                    Spacer(modifier = Modifier.width(4.dp))
                    Text("连接", fontSize = 12.sp, color = DeckColors.Blue, fontWeight = FontWeight.SemiBold)
                }
            }
        }

        // 第二行：实时遥测仪表盘条
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .background(DeckColors.BgPrimary.copy(alpha = 0.55f))
                .padding(horizontal = 14.dp, vertical = 6.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            TelemetryPill(
                label = "CH",
                value = if (status.channel > 0) "${status.channel}" else "--",
                accent = if (status.hopping) DeckColors.Purple else DeckColors.Blue
            )
            TelemetryPill(
                label = "APs",
                value = "$apCount",
                accent = DeckColors.Cyan
            )
            TelemetryPill(
                label = "目标",
                value = "$selectedCount",
                accent = if (selectedCount > 0) DeckColors.Crimson else DeckColors.TextSecondary
            )
            TelemetryPill(
                label = "TX OK",
                value = "${status.txOk}",
                accent = DeckColors.BrightGreen
            )
            TelemetryPill(
                label = "FAIL",
                value = "${status.txFail}",
                accent = if (status.txFail > 0) DeckColors.Amber else DeckColors.TextMuted
            )
        }

        // 第三行：当前激活任务横幅（仅在攻击或嗅探进行中展示）
        val activeTaskLabel = activeTaskDescription(status)
        if (activeTaskLabel != null) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(if (status.attacking) DeckColors.CrimsonDim else DeckColors.PurpleDim)
                    .padding(horizontal = 14.dp, vertical = 5.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Box(
                    modifier = Modifier
                        .size(7.dp)
                        .background(
                            if (status.attacking) DeckColors.Crimson else DeckColors.Purple,
                            CircleShape
                        )
                )
                Spacer(modifier = Modifier.width(8.dp))
                Text(
                    text = activeTaskLabel,
                    color = if (status.attacking) DeckColors.Crimson else DeckColors.TextPrimary,
                    fontSize = 11.sp,
                    fontWeight = FontWeight.Medium,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
            }
        }

        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height(1.dp)
                .background(DeckColors.BorderSubtle)
        )
    }
}

@Composable
private fun ConnectionBadge(state: BleManager.ConnectionState, status: DeviceStatus) {
    val (dotColor, text) = when (state) {
        BleManager.ConnectionState.CONNECTED -> {
            val c = when {
                status.attacking -> DeckColors.Crimson
                status.sniffing -> DeckColors.Purple
                else -> DeckColors.BrightGreen
            }
            val modeTxt = if (status.hopping) "跳频中" else "就绪"
            c to "BLE 已连接 · $modeTxt"
        }
        BleManager.ConnectionState.CONNECTING -> DeckColors.Amber to "正在建立 GATT 连接..."
        BleManager.ConnectionState.SCANNING -> DeckColors.Blue to "正在搜索 WiFi-Deck..."
        BleManager.ConnectionState.DISCONNECTED -> DeckColors.TextMuted to "设备未连接"
    }
    Row(verticalAlignment = Alignment.CenterVertically) {
        Box(
            modifier = Modifier
                .size(7.dp)
                .background(dotColor, CircleShape)
        )
        Spacer(modifier = Modifier.width(5.dp))
        Text(
            text = text,
            color = DeckColors.TextSecondary,
            fontSize = 11.sp
        )
    }
}

@Composable
private fun TelemetryPill(label: String, value: String, accent: Color) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(
            text = "$label ",
            color = DeckColors.TextMuted,
            fontSize = 10.sp,
            fontFamily = FontFamily.Monospace
        )
        Text(
            text = value,
            color = accent,
            fontSize = 11.sp,
            fontWeight = FontWeight.Bold,
            fontFamily = FontFamily.Monospace
        )
    }
}

private fun activeTaskDescription(status: DeviceStatus): String? {
    val active = mutableListOf<String>()
    if (status.attackEnabled) active.add("Deauth 压制")
    if (status.disassocEnabled) active.add("Disassoc 剥离")
    if (status.beaconFlood) active.add("Beacon 伪热点泛洪")
    if (status.probeFlood) active.add("Probe 探测泛洪")
    if (status.signalJam) active.add("双频全信道阻塞 (Signal Jam)")
    if (status.deauthSniff) active.add("Deauth 嗅探克隆")
    if (status.sniffing && !status.deauthSniff) {
        val modeName = when (status.sniffMode) {
            0 -> "ALL 全量帧"
            1 -> "Beacon 信标"
            2 -> "Probe 探测"
            3 -> "Deauth 解认证"
            4 -> "EAPOL 握手包"
            5 -> "Pwnagotchi"
            else -> "混杂模式"
        }
        active.add("抓包监听 ($modeName)")
    }
    return if (active.isEmpty()) null else "运行中: ${active.joinToString(" + ")}"
}

@Composable
fun SectionHeader(title: String, subtitle: String? = null) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(top = 6.dp, bottom = 4.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Box(
            modifier = Modifier
                .width(3.dp)
                .height(13.dp)
                .background(DeckColors.Crimson, RoundedCornerShape(2.dp))
        )
        Spacer(modifier = Modifier.width(7.dp))
        Text(
            text = title,
            color = DeckColors.TextPrimary,
            fontSize = 13.sp,
            fontWeight = FontWeight.SemiBold
        )
        if (subtitle != null) {
            Spacer(modifier = Modifier.width(8.dp))
            Text(
                text = subtitle,
                color = DeckColors.TextMuted,
                fontSize = 11.sp
            )
        }
    }
}

@Composable
fun DeckFilterChip(
    label: String,
    selected: Boolean = false,
    accentColor: Color = DeckColors.Crimson,
    onClick: () -> Unit
) {
    val bgColor = if (selected) accentColor.copy(alpha = 0.18f) else DeckColors.BgSurface
    val borderColor = if (selected) accentColor else DeckColors.BorderSubtle
    val textColor = if (selected) accentColor else DeckColors.TextSecondary
    Box(
        modifier = Modifier
            .height(30.dp)
            .clip(RoundedCornerShape(7.dp))
            .background(bgColor)
            .border(1.dp, borderColor, RoundedCornerShape(7.dp))
            .clickable { onClick() }
            .padding(horizontal = 10.dp),
        contentAlignment = Alignment.Center
    ) {
        Text(
            text = label,
            fontSize = 11.sp,
            fontWeight = if (selected) FontWeight.Bold else FontWeight.Medium,
            color = textColor
        )
    }
}

@Composable
fun TacticalActionCard(
    title: String,
    subtitle: String,
    icon: ImageVector,
    accentColor: Color,
    active: Boolean = false,
    modifier: Modifier = Modifier,
    onClick: () -> Unit
) {
    val containerColor = if (active) DeckColors.GreenDim else DeckColors.BgSurface
    val strokeColor = if (active) DeckColors.BrightGreen else DeckColors.BorderSubtle
    val iconTint = if (active) DeckColors.BrightGreen else accentColor

    Card(
        modifier = modifier
            .clip(RoundedCornerShape(10.dp))
            .clickable { onClick() },
        colors = CardDefaults.cardColors(containerColor = containerColor),
        shape = RoundedCornerShape(10.dp),
        border = BorderStroke(1.dp, strokeColor)
    ) {
        Column(modifier = Modifier.padding(12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    modifier = Modifier
                        .size(30.dp)
                        .background(iconTint.copy(alpha = 0.15f), RoundedCornerShape(7.dp)),
                    contentAlignment = Alignment.Center
                ) {
                    Icon(
                        imageVector = icon,
                        contentDescription = null,
                        tint = iconTint,
                        modifier = Modifier.size(17.dp)
                    )
                }
                Spacer(modifier = Modifier.weight(1f))
                if (active) {
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        modifier = Modifier
                            .background(DeckColors.BrightGreen.copy(alpha = 0.2f), RoundedCornerShape(4.dp))
                            .padding(horizontal = 6.dp, vertical = 2.dp)
                    ) {
                        Box(
                            modifier = Modifier
                                .size(6.dp)
                                .background(DeckColors.BrightGreen, CircleShape)
                        )
                        Spacer(modifier = Modifier.width(4.dp))
                        Text(
                            text = "运行中",
                            color = DeckColors.BrightGreen,
                            fontSize = 10.sp,
                            fontWeight = FontWeight.Bold
                        )
                    }
                }
            }
            Spacer(modifier = Modifier.height(8.dp))
            Text(
                text = title,
                color = DeckColors.TextPrimary,
                fontSize = 13.sp,
                fontWeight = FontWeight.SemiBold,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
            Spacer(modifier = Modifier.height(2.dp))
            Text(
                text = if (active) "再次点击立即停止" else subtitle,
                color = if (active) DeckColors.BrightGreen.copy(alpha = 0.85f) else DeckColors.TextSecondary,
                fontSize = 10.sp,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
        }
    }
}
