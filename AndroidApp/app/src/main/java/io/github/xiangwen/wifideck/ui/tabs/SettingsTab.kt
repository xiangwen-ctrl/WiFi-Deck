package io.github.xiangwen.wifideck.ui.tabs

import android.os.SystemClock
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
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.BluetoothConnected
import androidx.compose.material.icons.filled.BluetoothDisabled
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.github.xiangwen.wifideck.data.BleManager
import io.github.xiangwen.wifideck.ui.MainViewModel
import io.github.xiangwen.wifideck.ui.components.SectionHeader
import io.github.xiangwen.wifideck.ui.theme.DeckColors

private const val SETTINGS_SUBMIT_DEBOUNCE_MS = 350L

private fun utf8ByteLength(s: String): Int = s.toByteArray(Charsets.UTF_8).size

private fun sanitizeSingleLineInput(raw: String): String =
    raw.replace("\r", "").replace("\n", "")

@Composable
fun SettingsTab(viewModel: MainViewModel) {
    val state by viewModel.connectionState.collectAsStateWithLifecycle()
    val mtu by viewModel.mtu.collectAsStateWithLifecycle()
    val config by viewModel.config.collectAsStateWithLifecycle()
    val configSynced by viewModel.configSynced.collectAsStateWithLifecycle()
    val savingConfig by viewModel.savingConfig.collectAsStateWithLifecycle()
    val canWriteSettings by viewModel.canWriteSettings.collectAsStateWithLifecycle()
    val configResetSeq by viewModel.configResetSeq.collectAsStateWithLifecycle()

    val isConnected = state == BleManager.ConnectionState.CONNECTED
    val canSyncRefresh = isConnected && !savingConfig

    var bleDirty by rememberSaveable { mutableStateOf(false) }
    var ssidDirty by rememberSaveable { mutableStateOf(false) }
    var passDirty by rememberSaveable { mutableStateOf(false) }
    var bleNameInput by rememberSaveable { mutableStateOf(config.bleName) }
    var apSsidInput by rememberSaveable { mutableStateOf(config.apSsid) }
    var apPassInput by rememberSaveable { mutableStateOf(config.apPass) }
    var lastHandledResetSeq by rememberSaveable { mutableLongStateOf(configResetSeq) }

    // 仅在已连接且完整同步后回填非 dirty / 已确认字段，断连回落默认值时不误清本地草稿
    LaunchedEffect(config.bleName, configSynced, isConnected) {
        if (isConnected && configSynced) {
            if (!bleDirty || bleNameInput.trim() == config.bleName) {
                bleNameInput = config.bleName
                bleDirty = false
            }
        } else if (!bleDirty) {
            bleNameInput = config.bleName
        }
    }
    LaunchedEffect(config.apSsid, configSynced, isConnected) {
        if (isConnected && configSynced) {
            if (!ssidDirty || apSsidInput.trim() == config.apSsid) {
                apSsidInput = config.apSsid
                ssidDirty = false
            }
        } else if (!ssidDirty) {
            apSsidInput = config.apSsid
        }
    }
    LaunchedEffect(config.apPass, configSynced, isConnected) {
        if (isConnected && configSynced) {
            if (!passDirty || apPassInput.trim() == config.apPass) {
                apPassInput = config.apPass
                passDirty = false
            }
        } else if (!passDirty) {
            apPassInput = config.apPass
        }
    }
    // configResetSeq 仅在 reset_config 获固件确认时递增
    LaunchedEffect(configResetSeq) {
        if (configResetSeq > lastHandledResetSeq) {
            lastHandledResetSeq = configResetSeq
            bleNameInput = config.bleName
            apSsidInput = config.apSsid
            apPassInput = config.apPass
            bleDirty = false
            ssidDirty = false
            passDirty = false
        }
    }

    val scrollState = rememberScrollState()

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(scrollState)
            .padding(horizontal = 14.dp, vertical = 10.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp)
    ) {
        // 1. BLE 链路管理卡片
        SectionHeader(
            title = "蓝牙 GATT 链路",
            subtitle = if (isConnected) "Nordic UART · MTU $mtu" else "Nordic UART · 等待连接"
        )
        Card(
            modifier = Modifier.fillMaxWidth(),
            colors = CardDefaults.cardColors(containerColor = DeckColors.BgSurface),
            shape = RoundedCornerShape(12.dp),
            border = BorderStroke(1.dp, DeckColors.BorderSubtle)
        ) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(14.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Box(
                    modifier = Modifier
                        .size(40.dp)
                        .background(
                            if (isConnected) DeckColors.GreenDim else DeckColors.BgElevated,
                            RoundedCornerShape(10.dp)
                        ),
                    contentAlignment = Alignment.Center
                ) {
                    Icon(
                        imageVector = if (isConnected) {
                            Icons.Default.BluetoothConnected
                        } else {
                            Icons.Default.BluetoothDisabled
                        },
                        contentDescription = null,
                        tint = if (isConnected) DeckColors.BrightGreen else DeckColors.TextMuted,
                        modifier = Modifier.size(20.dp)
                    )
                }
                Spacer(modifier = Modifier.width(12.dp))
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        text = when (state) {
                            BleManager.ConnectionState.CONNECTED -> "已连接至 ${config.bleName}"
                            BleManager.ConnectionState.CONNECTING -> "正在握手连接..."
                            BleManager.ConnectionState.SCANNING -> "正在扫描 BLE 广播..."
                            BleManager.ConnectionState.DISCONNECTED -> "未连接设备"
                        },
                        color = DeckColors.TextPrimary,
                        fontSize = 14.sp,
                        fontWeight = FontWeight.SemiBold
                    )
                    Spacer(modifier = Modifier.height(2.dp))
                    Text(
                        text = "NUS: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E",
                        color = DeckColors.TextMuted,
                        fontSize = 10.sp,
                        fontFamily = FontFamily.Monospace
                    )
                }
                if (state == BleManager.ConnectionState.DISCONNECTED) {
                    Button(
                        onClick = { viewModel.requestBleConnect() },
                        colors = ButtonDefaults.buttonColors(containerColor = DeckColors.Blue),
                        shape = RoundedCornerShape(8.dp),
                        contentPadding = PaddingValues(horizontal = 12.dp, vertical = 0.dp),
                        modifier = Modifier.height(34.dp)
                    ) {
                        Text("扫描连接", fontSize = 12.sp, fontWeight = FontWeight.SemiBold, color = Color.White)
                    }
                } else {
                    OutlinedButton(
                        onClick = { viewModel.disconnectBle() },
                        border = BorderStroke(1.dp, DeckColors.Crimson.copy(alpha = 0.6f)),
                        shape = RoundedCornerShape(8.dp),
                        contentPadding = PaddingValues(horizontal = 12.dp, vertical = 0.dp),
                        modifier = Modifier.height(34.dp)
                    ) {
                        Text("断开", fontSize = 12.sp, color = DeckColors.Crimson)
                    }
                }
            }
        }

        // 2. 设备身份与名称伪装 (掉电记忆)
        val hasDraft = bleDirty || ssidDirty || passDirty
        val identitySubtitle = when {
            !isConnected -> if (hasDraft) "未连接 · 已保留本地草稿" else "未连接 · 仅展示默认值"
            savingConfig -> "等待开发板确认写入..."
            configSynced -> "已同步 (${config.bleName} / ${config.apSsid})"
            else -> "正在同步板载配置..."
        }
        SectionHeader(title = "设备身份与名称伪装", subtitle = identitySubtitle)
        Card(
            modifier = Modifier.fillMaxWidth(),
            colors = CardDefaults.cardColors(containerColor = DeckColors.BgSurface),
            shape = RoundedCornerShape(12.dp),
            border = BorderStroke(1.dp, DeckColors.BorderSubtle)
        ) {
            Column(
                modifier = Modifier.padding(14.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                IdentityEditRow(
                    label = "BLE 蓝牙广播名称 (${utf8ByteLength(bleNameInput)}/26B" +
                        (if (isConnected && configSynced) " · 板载: ${config.bleName}" else "") + ")",
                    hint = "如: Mi Band 8 / WiFi-Deck",
                    value = bleNameInput,
                    onValueChange = { raw ->
                        val sanitized = sanitizeSingleLineInput(raw)
                        if (utf8ByteLength(sanitized) <= 26) {
                            bleNameInput = sanitized
                            bleDirty = (sanitized.trim() != config.bleName) || !isConnected || !configSynced
                        }
                    },
                    buttonText = "保存蓝牙名",
                    accentColor = DeckColors.Cyan,
                    readOnly = savingConfig,
                    buttonEnabled = canWriteSettings,
                    onSave = { viewModel.sendSetBleName(bleNameInput) }
                )

                HorizontalDivider(color = DeckColors.BorderSubtle)

                IdentityEditRow(
                    label = "Wi-Fi 热点名称 (${utf8ByteLength(apSsidInput)}/32B" +
                        (if (isConnected && configSynced) " · 板载: ${config.apSsid}" else "") + ")",
                    hint = "如: TP-LINK_5G / WiFi-Deck",
                    value = apSsidInput,
                    onValueChange = { raw ->
                        val sanitized = sanitizeSingleLineInput(raw)
                        if (utf8ByteLength(sanitized) <= 32) {
                            apSsidInput = sanitized
                            ssidDirty = (sanitized.trim() != config.apSsid) || !isConnected || !configSynced
                        }
                    },
                    buttonText = "保存 SSID",
                    accentColor = DeckColors.Blue,
                    readOnly = savingConfig,
                    buttonEnabled = canWriteSettings,
                    onSave = { viewModel.sendSetApSsid(apSsidInput) }
                )

                IdentityEditRow(
                    label = "Wi-Fi 热点密码 (${utf8ByteLength(apPassInput)}/63B · WPA2 8~63位)",
                    hint = "至少 8 位字符",
                    value = apPassInput,
                    onValueChange = { raw ->
                        val sanitized = sanitizeSingleLineInput(raw)
                        if (utf8ByteLength(sanitized) <= 63) {
                            apPassInput = sanitized
                            passDirty = (sanitized.trim() != config.apPass) || !isConnected || !configSynced
                        }
                    },
                    buttonText = "保存密码",
                    accentColor = DeckColors.Blue,
                    readOnly = savingConfig,
                    buttonEnabled = canWriteSettings,
                    onSave = { viewModel.sendSetApPass(apPassInput) }
                )

                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text(
                        text = if (savingConfig) "正在写入 Flash 并等待确认..." else "修改后自动写入板载 Flash，断电重启依然生效",
                        color = DeckColors.TextMuted,
                        fontSize = 11.sp,
                        modifier = Modifier.weight(1f)
                    )
                    Spacer(modifier = Modifier.width(8.dp))
                    OutlinedButton(
                        onClick = { viewModel.refreshDeviceInfo(userTriggered = true) },
                        enabled = canSyncRefresh,
                        border = BorderStroke(
                            1.dp,
                            if (canSyncRefresh) DeckColors.BorderStrong else DeckColors.BorderSubtle
                        ),
                        shape = RoundedCornerShape(7.dp),
                        contentPadding = PaddingValues(horizontal = 10.dp, vertical = 0.dp),
                        modifier = Modifier.height(28.dp)
                    ) {
                        Text(
                            text = "同步刷新",
                            fontSize = 11.sp,
                            color = if (canSyncRefresh) DeckColors.Cyan else DeckColors.TextMuted
                        )
                    }
                    Spacer(modifier = Modifier.width(6.dp))
                    OutlinedButton(
                        onClick = { viewModel.sendResetConfig() },
                        enabled = canWriteSettings,
                        border = BorderStroke(
                            1.dp,
                            if (canWriteSettings) DeckColors.BorderStrong else DeckColors.BorderSubtle
                        ),
                        shape = RoundedCornerShape(7.dp),
                        contentPadding = PaddingValues(horizontal = 10.dp, vertical = 0.dp),
                        modifier = Modifier.height(28.dp)
                    ) {
                        Text(
                            text = "恢复默认",
                            fontSize = 11.sp,
                            color = if (canWriteSettings) DeckColors.TextSecondary else DeckColors.TextMuted
                        )
                    }
                }
            }
        }

        // 3. 无线隐身与硬件开关卡片
        val writeLockSubtitle = when {
            !isConnected -> "未连接 · 已禁用"
            savingConfig -> "配置写入中 · 已锁定"
            !configSynced -> "正在同步板载配置..."
            else -> null
        }
        SectionHeader(
            title = "无线隐身与硬件控制",
            subtitle = writeLockSubtitle ?: "双向实时同步"
        )
        Card(
            modifier = Modifier.fillMaxWidth(),
            colors = CardDefaults.cardColors(containerColor = DeckColors.BgSurface),
            shape = RoundedCornerShape(12.dp),
            border = BorderStroke(1.dp, DeckColors.BorderSubtle)
        ) {
            Column(modifier = Modifier.padding(14.dp)) {
                SettingSwitchRow(
                    title = "WebUI 热点广播 (ap on/off)",
                    subtitle = if (config.apEnabled) {
                        "已开启: 热点 ${config.apSsid} (192.168.1.1) 可访问"
                    } else {
                        "已关闭: 处于 BLE-only 隐身模式，STA 扫描与注入保持激活"
                    },
                    checked = config.apEnabled,
                    enabled = canWriteSettings,
                    onCheckedChange = { viewModel.sendApToggle(it) }
                )

                HorizontalDivider(
                    color = DeckColors.BorderSubtle,
                    modifier = Modifier.padding(vertical = 10.dp)
                )

                SettingSwitchRow(
                    title = "板载 RGB LED 状态指示灯",
                    subtitle = "扫描绿灯、发包蓝灯闪烁提示（关闭可增强隐蔽性）",
                    checked = config.ledEnabled,
                    enabled = canWriteSettings,
                    onCheckedChange = { viewModel.sendLedToggle(it) }
                )

                HorizontalDivider(
                    color = DeckColors.BorderSubtle,
                    modifier = Modifier.padding(vertical = 10.dp)
                )

                SettingSwitchRow(
                    title = "攻击循环间自动重新扫描",
                    subtitle = "每轮周期前重新扫描周围信道（会增加循环耗时）",
                    checked = config.scanBetweenCycles,
                    enabled = canWriteSettings,
                    onCheckedChange = { viewModel.sendSetScanCycles(it) }
                )
            }
        }

        // 4. 射频与扫描参数调优卡片
        SectionHeader(
            title = "射频注入与扫描参数",
            subtitle = writeLockSubtitle ?: "点击立即下发至固件"
        )
        Card(
            modifier = Modifier.fillMaxWidth(),
            colors = CardDefaults.cardColors(containerColor = DeckColors.BgSurface),
            shape = RoundedCornerShape(12.dp),
            border = BorderStroke(1.dp, DeckColors.BorderSubtle)
        ) {
            Column(
                modifier = Modifier.padding(14.dp),
                verticalArrangement = Arrangement.spacedBy(14.dp)
            ) {
                SegmentedParamSelector(
                    label = "单次突发帧数 (num_frames)",
                    currentValue = "${config.numSendFrames} 帧",
                    options = listOf(1 to "1", 3 to "3", 5 to "5", 10 to "10", 20 to "20"),
                    selectedKey = config.numSendFrames,
                    enabled = canWriteSettings,
                    onPick = { viewModel.sendSetFrames(it) }
                )

                SegmentedParamSelector(
                    label = "攻击循环间隔 (cycle_delay)",
                    currentValue = "${config.cycleDelayMs} ms",
                    options = listOf(500 to "0.5s", 1000 to "1.0s", 2000 to "2.0s", 5000 to "5.0s"),
                    selectedKey = config.cycleDelayMs.toInt(),
                    enabled = canWriteSettings,
                    onPick = { viewModel.sendSetCycleDelay(it.toLong()) }
                )

                SegmentedParamSelector(
                    label = "双频扫描时长 (scan_time)",
                    currentValue = "${config.scanTimeMs} ms",
                    options = listOf(3000 to "3.0s", 5000 to "5.0s", 8000 to "8.0s", 10000 to "10s"),
                    selectedKey = config.scanTimeMs.toInt(),
                    enabled = canWriteSettings,
                    onPick = { viewModel.sendSetScanTime(it.toLong()) }
                )

                SegmentedParamSelector(
                    label = "扫描起始频段 (start_channel)",
                    currentValue = if (config.startChannel >= 36) "仅 5GHz (CH 36+)" else "2.4G + 5G 双频 (CH 1+)",
                    options = listOf(1 to "2.4G + 5G 全频段", 36 to "仅 5GHz 频段"),
                    selectedKey = config.startChannel,
                    enabled = canWriteSettings,
                    onPick = { viewModel.sendSetStartChannel(it) }
                )
            }
        }

        Text(
            text = "免责声明：本工具仅用于无线网络安全教学、802.11 协议分析与合法授权测试。",
            color = DeckColors.TextMuted,
            fontSize = 11.sp,
            modifier = Modifier.padding(horizontal = 4.dp)
        )
    }
}

@Composable
private fun SettingSwitchRow(
    title: String,
    subtitle: String,
    checked: Boolean,
    enabled: Boolean,
    onCheckedChange: (Boolean) -> Unit
) {
    var lastSubmitAtMs by remember { mutableLongStateOf(0L) }

    Row(
        modifier = Modifier
            .fillMaxWidth()
            .alpha(if (enabled) 1f else 0.5f),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Column(modifier = Modifier.weight(1f).padding(end = 10.dp)) {
            Text(
                text = title,
                color = DeckColors.TextPrimary,
                fontSize = 13.sp,
                fontWeight = FontWeight.Medium
            )
            Spacer(modifier = Modifier.height(2.dp))
            Text(
                text = subtitle,
                color = DeckColors.TextMuted,
                fontSize = 11.sp,
                lineHeight = 15.sp
            )
        }
        Switch(
            checked = checked,
            enabled = enabled,
            onCheckedChange = { next ->
                if (!enabled) return@Switch
                val now = SystemClock.elapsedRealtime()
                if (now - lastSubmitAtMs >= SETTINGS_SUBMIT_DEBOUNCE_MS) {
                    lastSubmitAtMs = now
                    onCheckedChange(next)
                }
            },
            colors = SwitchDefaults.colors(
                checkedThumbColor = Color.White,
                checkedTrackColor = DeckColors.Blue,
                uncheckedThumbColor = DeckColors.TextSecondary,
                uncheckedTrackColor = DeckColors.BgElevated,
                disabledCheckedThumbColor = DeckColors.TextSecondary,
                disabledCheckedTrackColor = DeckColors.BgElevated,
                disabledUncheckedThumbColor = DeckColors.TextMuted,
                disabledUncheckedTrackColor = DeckColors.BgTerminal
            )
        )
    }
}

@Composable
private fun SegmentedParamSelector(
    label: String,
    currentValue: String,
    options: List<Pair<Int, String>>,
    selectedKey: Int,
    enabled: Boolean,
    onPick: (Int) -> Unit
) {
    var lastSubmitAtMs by remember { mutableLongStateOf(0L) }

    Column(modifier = Modifier.alpha(if (enabled) 1f else 0.5f)) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                text = label,
                color = DeckColors.TextSecondary,
                fontSize = 12.sp
            )
            Text(
                text = currentValue,
                color = if (enabled) DeckColors.Cyan else DeckColors.TextMuted,
                fontSize = 11.sp,
                fontFamily = FontFamily.Monospace,
                fontWeight = FontWeight.Bold
            )
        }
        Spacer(modifier = Modifier.height(6.dp))
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(6.dp)
        ) {
            options.forEach { (key, text) ->
                val isSelected = (key == selectedKey)
                Box(
                    modifier = Modifier
                        .weight(1f)
                        .height(32.dp)
                        .clip(RoundedCornerShape(8.dp))
                        .background(if (isSelected) DeckColors.BlueDim else DeckColors.BgElevated)
                        .border(
                            1.dp,
                            if (isSelected) DeckColors.Blue else DeckColors.BorderSubtle,
                            RoundedCornerShape(8.dp)
                        )
                        .clickable(enabled = enabled) {
                            val now = SystemClock.elapsedRealtime()
                            if (now - lastSubmitAtMs >= SETTINGS_SUBMIT_DEBOUNCE_MS) {
                                lastSubmitAtMs = now
                                onPick(key)
                            }
                        },
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        text = text,
                        color = if (isSelected) DeckColors.Blue else DeckColors.TextPrimary,
                        fontSize = 12.sp,
                        fontWeight = if (isSelected) FontWeight.Bold else FontWeight.Medium
                    )
                }
            }
        }
    }
}

@Composable
private fun IdentityEditRow(
    label: String,
    hint: String,
    value: String,
    onValueChange: (String) -> Unit,
    buttonText: String,
    accentColor: Color,
    readOnly: Boolean,
    buttonEnabled: Boolean,
    onSave: () -> Unit
) {
    val canClickSave = buttonEnabled && !readOnly && value.isNotBlank()
    Column {
        Text(
            text = label,
            color = DeckColors.TextSecondary,
            fontSize = 11.sp
        )
        Spacer(modifier = Modifier.height(5.dp))
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            Box(
                modifier = Modifier
                    .weight(1f)
                    .height(38.dp)
                    .background(DeckColors.BgTerminal, RoundedCornerShape(8.dp))
                    .border(
                        1.dp,
                        if (readOnly) DeckColors.BorderStrong else DeckColors.BorderSubtle,
                        RoundedCornerShape(8.dp)
                    )
                    .padding(horizontal = 10.dp),
                contentAlignment = Alignment.CenterStart
            ) {
                if (value.isEmpty()) {
                    Text(
                        text = hint,
                        color = DeckColors.TextMuted,
                        fontSize = 11.sp,
                        fontFamily = FontFamily.Monospace
                    )
                }
                BasicTextField(
                    value = value,
                    onValueChange = { raw ->
                        onValueChange(sanitizeSingleLineInput(raw))
                    },
                    readOnly = readOnly,
                    singleLine = true,
                    textStyle = TextStyle(
                        color = if (readOnly) DeckColors.TextSecondary else DeckColors.TextPrimary,
                        fontSize = 12.sp,
                        fontFamily = FontFamily.Monospace
                    ),
                    cursorBrush = SolidColor(accentColor),
                    modifier = Modifier.fillMaxWidth()
                )
            }
            Button(
                onClick = onSave,
                enabled = canClickSave,
                colors = ButtonDefaults.buttonColors(
                    containerColor = accentColor,
                    disabledContainerColor = DeckColors.BgElevated
                ),
                shape = RoundedCornerShape(8.dp),
                contentPadding = PaddingValues(horizontal = 12.dp, vertical = 0.dp),
                modifier = Modifier.height(38.dp)
            ) {
                Text(
                    text = buttonText,
                    fontSize = 11.sp,
                    fontWeight = FontWeight.SemiBold,
                    color = when {
                        !canClickSave -> DeckColors.TextMuted
                        accentColor == DeckColors.Cyan -> DeckColors.BgPrimary
                        else -> Color.White
                    }
                )
            }
        }
    }
}
