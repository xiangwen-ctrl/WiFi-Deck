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
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.displayCutout
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.union
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.BluetoothSearching
import androidx.compose.material.icons.filled.StopCircle
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
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
import io.github.xiangwen.wifideck.data.BleGatePrompt
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
    onCancelConnect: () -> Unit = {},
    onStopAll: () -> Unit
) {
    val isBusy = status.attacking || status.sniffing
    val (ledColor, statusText) = when (state) {
        BleManager.ConnectionState.CONNECTED -> {
            val c = when {
                status.attacking -> DeckColors.Crimson
                status.sniffing -> DeckColors.Purple
                else -> DeckColors.BrightGreen
            }
            val sub = when {
                status.attacking -> "射频注入运行中"
                status.sniffing -> if (status.hopping) "混杂抓包 · 跳频中" else "混杂抓包 · 锁定 CH ${status.channel}"
                else -> "BLE 链路就绪"
            }
            c to sub
        }
        BleManager.ConnectionState.CONNECTING -> DeckColors.Amber to "正在建立 GATT 握手..."
        BleManager.ConnectionState.SCANNING -> DeckColors.Blue to "正在搜索附近设备..."
        BleManager.ConnectionState.DISCONNECTED -> DeckColors.TextMuted to "未连接开发板"
    }

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .background(DeckColors.BgSurface)
            .windowInsetsPadding(
                WindowInsets.statusBars
                    .union(WindowInsets.displayCutout)
                    .only(WindowInsetsSides.Top + WindowInsetsSides.Horizontal)
            ) // 自动避让系统状态栏与横屏/异形屏挖孔区域
    ) {
        // 第一行：硬件状态指示灯 + 设备名 + 动作胶囊
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            // 双层呼吸感指示灯
            Box(
                modifier = Modifier
                    .size(28.dp)
                    .background(ledColor.copy(alpha = 0.14f), CircleShape),
                contentAlignment = Alignment.Center
            ) {
                Box(
                    modifier = Modifier
                        .size(10.dp)
                        .background(ledColor, CircleShape)
                )
            }

            Spacer(modifier = Modifier.width(12.dp))

            Column(modifier = Modifier.weight(1f)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        text = config.bleName.ifBlank { "WiFi-Deck" },
                        color = DeckColors.TextPrimary,
                        fontSize = 17.sp,
                        fontWeight = FontWeight.Bold,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        modifier = Modifier.weight(1f, fill = false)
                    )
                    Spacer(modifier = Modifier.width(8.dp))
                    Box(
                        modifier = Modifier
                            .background(
                                if (config.apEnabled) DeckColors.BgElevated else DeckColors.PurpleDim,
                                RoundedCornerShape(5.dp)
                            )
                            .padding(horizontal = 6.dp, vertical = 2.dp)
                    ) {
                        Text(
                            text = if (config.apEnabled) "2.4G / 5G" else "STEALTH",
                            color = if (config.apEnabled) DeckColors.TextSecondary else DeckColors.Purple,
                            fontSize = 10.sp,
                            fontFamily = FontFamily.Monospace,
                            fontWeight = FontWeight.SemiBold
                        )
                    }
                }
                Spacer(modifier = Modifier.height(2.dp))
                Text(
                    text = statusText,
                    color = DeckColors.TextSecondary,
                    fontSize = 12.sp,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
            }

            if (isBusy) {
                Button(
                    onClick = onStopAll,
                    colors = ButtonDefaults.buttonColors(containerColor = DeckColors.Crimson),
                    contentPadding = PaddingValues(horizontal = 12.dp, vertical = 0.dp),
                    shape = RoundedCornerShape(8.dp),
                    modifier = Modifier.height(34.dp)
                ) {
                    Icon(
                        imageVector = Icons.Default.StopCircle,
                        contentDescription = null,
                        modifier = Modifier.size(15.dp),
                        tint = Color.White
                    )
                    Spacer(modifier = Modifier.width(5.dp))
                    Text("急停", fontSize = 12.sp, fontWeight = FontWeight.Bold, color = Color.White)
                }
            } else if (state == BleManager.ConnectionState.SCANNING || state == BleManager.ConnectionState.CONNECTING) {
                Button(
                    onClick = onCancelConnect,
                    colors = ButtonDefaults.buttonColors(containerColor = DeckColors.BgElevated),
                    contentPadding = PaddingValues(horizontal = 12.dp, vertical = 0.dp),
                    shape = RoundedCornerShape(8.dp),
                    modifier = Modifier
                        .height(34.dp)
                        .border(1.dp, DeckColors.BorderStrong, RoundedCornerShape(8.dp))
                ) {
                    Text(
                        text = if (state == BleManager.ConnectionState.SCANNING) "取消搜索" else "取消连接",
                        fontSize = 12.sp,
                        color = DeckColors.Amber,
                        fontWeight = FontWeight.Medium
                    )
                }
            } else if (state == BleManager.ConnectionState.DISCONNECTED) {
                Button(
                    onClick = onConnect,
                    colors = ButtonDefaults.buttonColors(containerColor = DeckColors.BgElevated),
                    contentPadding = PaddingValues(horizontal = 12.dp, vertical = 0.dp),
                    shape = RoundedCornerShape(8.dp),
                    modifier = Modifier
                        .height(34.dp)
                        .border(1.dp, DeckColors.BorderStrong, RoundedCornerShape(8.dp))
                ) {
                    Icon(
                        imageVector = Icons.AutoMirrored.Filled.BluetoothSearching,
                        contentDescription = null,
                        modifier = Modifier.size(15.dp),
                        tint = DeckColors.Blue
                    )
                    Spacer(modifier = Modifier.width(5.dp))
                    Text("连接设备", fontSize = 12.sp, color = DeckColors.TextPrimary, fontWeight = FontWeight.Medium)
                }
            }
        }

        // 第二行：四格仪器遥测读数条
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp)
                .padding(bottom = 10.dp)
                .clip(RoundedCornerShape(10.dp))
                .background(DeckColors.BgPrimary)
                .border(1.dp, DeckColors.BorderSubtle, RoundedCornerShape(10.dp))
                .padding(vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            TelemetryCell(
                label = "当前信道",
                value = if (status.channel > 0) "CH ${status.channel}" else "--",
                valueColor = if (status.hopping) DeckColors.Purple else DeckColors.Cyan,
                modifier = Modifier.weight(1f)
            )
            VerticalDividerLine()
            TelemetryCell(
                label = "发现热点",
                value = "$apCount",
                valueColor = DeckColors.TextPrimary,
                modifier = Modifier.weight(1f)
            )
            VerticalDividerLine()
            TelemetryCell(
                label = "锁定目标",
                value = "$selectedCount",
                valueColor = if (selectedCount > 0) DeckColors.Crimson else DeckColors.TextSecondary,
                modifier = Modifier.weight(1f)
            )
            VerticalDividerLine()
            TelemetryCell(
                label = "TX 成功/失败",
                value = "${status.txOk}/${status.txFail}",
                valueColor = if (status.txOk > 0) DeckColors.BrightGreen else DeckColors.TextSecondary,
                modifier = Modifier.weight(1.25f)
            )
        }

        // 第三行：当前激活任务横幅（仅在攻击或嗅探进行中展示）
        val activeTaskLabel = activeTaskDescription(status)
        if (activeTaskLabel != null) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(if (status.attacking) DeckColors.CrimsonDim else DeckColors.PurpleDim)
                    .padding(horizontal = 16.dp, vertical = 6.dp),
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
                    color = DeckColors.TextPrimary,
                    fontSize = 12.sp,
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
private fun TelemetryCell(
    label: String,
    value: String,
    valueColor: Color,
    modifier: Modifier = Modifier
) {
    Column(
        modifier = modifier,
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Text(
            text = value,
            color = valueColor,
            fontSize = 13.sp,
            fontWeight = FontWeight.Bold,
            fontFamily = FontFamily.Monospace,
            maxLines = 1
        )
        Spacer(modifier = Modifier.height(1.dp))
        Text(
            text = label,
            color = DeckColors.TextMuted,
            fontSize = 10.sp,
            maxLines = 1
        )
    }
}

@Composable
private fun VerticalDividerLine() {
    Box(
        modifier = Modifier
            .width(1.dp)
            .height(22.dp)
            .background(DeckColors.BorderSubtle)
    )
}

private fun activeTaskDescription(status: DeviceStatus): String? {
    val active = mutableListOf<String>()
    if (status.attackEnabled) active.add("Deauth 压制")
    if (status.disassocEnabled) active.add("Disassoc 剥离")
    if (status.beaconFlood) active.add("Beacon 伪热点泛洪")
    if (status.probeFlood) active.add("Probe 探测泛洪")
    if (status.signalJam) active.add("双频全信道阻塞")
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
    if (active.isEmpty()) return null
    val suffix = if (status.timedRemainingSec > 0) " · 倒计时 ${status.timedRemainingSec}s" else ""
    return "运行中: ${active.joinToString(" + ")}$suffix"
}

@Composable
fun SectionHeader(title: String, subtitle: String? = null) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(top = 6.dp, bottom = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.SpaceBetween
    ) {
        Text(
            text = title,
            color = DeckColors.TextPrimary,
            fontSize = 14.sp,
            fontWeight = FontWeight.SemiBold
        )
        if (subtitle != null) {
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
    val bgColor = if (selected) accentColor.copy(alpha = 0.16f) else DeckColors.BgSurface
    val borderColor = if (selected) accentColor.copy(alpha = 0.65f) else DeckColors.BorderSubtle
    val textColor = if (selected) DeckColors.TextPrimary else DeckColors.TextSecondary
    Box(
        modifier = Modifier
            .height(32.dp)
            .clip(RoundedCornerShape(8.dp))
            .background(bgColor)
            .border(1.dp, borderColor, RoundedCornerShape(8.dp))
            .clickable { onClick() }
            .padding(horizontal = 11.dp),
        contentAlignment = Alignment.Center
    ) {
        Text(
            text = label,
            fontSize = 12.sp,
            fontWeight = if (selected) FontWeight.SemiBold else FontWeight.Normal,
            color = textColor
        )
    }
}

@Composable
fun SignalStrengthBars(rssi: Int, modifier: Modifier = Modifier) {
    val activeBars = DeckColors.rssiBars(rssi)
    val barColor = DeckColors.rssiColor(rssi)
    Row(
        modifier = modifier.height(14.dp),
        verticalAlignment = Alignment.Bottom,
        horizontalArrangement = Arrangement.spacedBy(2.dp)
    ) {
        for (i in 1..4) {
            val h = (4 + i * 2.5f).dp
            Box(
                modifier = Modifier
                    .width(3.dp)
                    .height(h)
                    .background(
                        if (i <= activeBars) barColor else DeckColors.BorderStrong,
                        RoundedCornerShape(1.dp)
                    )
            )
        }
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
            .clip(RoundedCornerShape(12.dp))
            .clickable { onClick() },
        colors = CardDefaults.cardColors(containerColor = containerColor),
        shape = RoundedCornerShape(12.dp),
        border = BorderStroke(1.dp, strokeColor)
    ) {
        Column(modifier = Modifier.padding(14.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Surface(
                    modifier = Modifier.size(34.dp),
                    shape = RoundedCornerShape(9.dp),
                    color = iconTint.copy(alpha = 0.14f)
                ) {
                    Box(contentAlignment = Alignment.Center) {
                        Icon(
                            imageVector = icon,
                            contentDescription = null,
                            tint = iconTint,
                            modifier = Modifier.size(18.dp)
                        )
                    }
                }
                Spacer(modifier = Modifier.weight(1f))
                if (active) {
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        modifier = Modifier
                            .background(DeckColors.BrightGreen.copy(alpha = 0.18f), RoundedCornerShape(6.dp))
                            .padding(horizontal = 7.dp, vertical = 3.dp)
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
            Spacer(modifier = Modifier.height(10.dp))
            Text(
                text = title,
                color = DeckColors.TextPrimary,
                fontSize = 14.sp,
                fontWeight = FontWeight.SemiBold,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
            Spacer(modifier = Modifier.height(3.dp))
            Text(
                text = if (active) "点击立即停止" else subtitle,
                color = if (active) DeckColors.BrightGreen else DeckColors.TextSecondary,
                fontSize = 11.sp,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
        }
    }
}

@Composable
fun BleGatePromptDialog(
    prompt: BleGatePrompt,
    onConfirm: () -> Unit,
    onDismiss: () -> Unit
) {
    val (title, message, confirmLabel) = when (prompt) {
        is BleGatePrompt.PermissionRationale -> Triple(
            if (prompt.isAndroid12Plus) "需要蓝牙连接权限" else "需要蓝牙与定位权限",
            if (prompt.isAndroid12Plus) {
                "WiFi-Deck 需要「附近设备（蓝牙扫描与连接）」权限以发现并连接 BW16 (RTL8720DN) 开发板，仅用于 BLE 串口通信。"
            } else {
                "Android 8–11 系统要求授予「位置信息」权限才能执行 BLE 扫描。WiFi-Deck 仅用于搜索开发板，不会采集或上传位置。"
            },
            "继续授权"
        )
        is BleGatePrompt.PermissionPermanentlyDenied -> Triple(
            "权限已被拒绝",
            if (prompt.isAndroid12Plus) {
                "缺少「附近设备」权限，无法扫描或连接开发板。请前往应用设置中手动开启蓝牙相关权限。"
            } else {
                "缺少「位置信息」权限，Android 8–11 无法执行 BLE 扫描。请前往应用设置中开启位置权限。"
            },
            "前往设置"
        )
        BleGatePrompt.BluetoothDisabled -> Triple(
            "系统蓝牙未开启",
            "检测到蓝牙开关处于关闭状态，请开启蓝牙以扫描并连接 WiFi-Deck 开发板。",
            "开启蓝牙"
        )
        BleGatePrompt.LocationDisabled -> Triple(
            "系统定位开关未开启",
            "Android 8–11 需要开启系统「位置信息」总开关才能返回 BLE 扫描结果，请前往系统设置开启后返回。",
            "开启定位"
        )
    }

    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = DeckColors.BgSurface,
        titleContentColor = DeckColors.TextPrimary,
        textContentColor = DeckColors.TextSecondary,
        shape = RoundedCornerShape(14.dp),
        title = {
            Text(
                text = title,
                fontSize = 16.sp,
                fontWeight = FontWeight.Bold
            )
        },
        text = {
            Text(
                text = message,
                fontSize = 13.sp,
                lineHeight = 19.sp
            )
        },
        confirmButton = {
            Button(
                onClick = onConfirm,
                colors = ButtonDefaults.buttonColors(containerColor = DeckColors.Blue),
                shape = RoundedCornerShape(8.dp)
            ) {
                Text(
                    text = confirmLabel,
                    fontSize = 13.sp,
                    fontWeight = FontWeight.SemiBold,
                    color = Color.White
                )
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) {
                Text(
                    text = "暂不",
                    fontSize = 13.sp,
                    color = DeckColors.TextSecondary
                )
            }
        }
    )
}
