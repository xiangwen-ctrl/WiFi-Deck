package io.github.xiangwen.wifideck.ui.tabs

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
import androidx.compose.material.icons.automirrored.filled.Send
import androidx.compose.material.icons.filled.BluetoothConnected
import androidx.compose.material.icons.filled.BluetoothDisabled
import androidx.compose.material.icons.filled.Terminal
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
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import io.github.xiangwen.wifideck.data.BleManager
import io.github.xiangwen.wifideck.ui.MainViewModel
import io.github.xiangwen.wifideck.ui.components.SectionHeader
import io.github.xiangwen.wifideck.ui.theme.DeckColors

@Composable
fun SettingsTab(viewModel: MainViewModel) {
    val state by viewModel.connectionState.collectAsState()
    val config by viewModel.config.collectAsState()
    var customCmd by remember { mutableStateOf("") }
    var bleNameInput by remember(config.bleName) { mutableStateOf(config.bleName) }
    var apSsidInput by remember(config.apSsid) { mutableStateOf(config.apSsid) }
    var apPassInput by remember(config.apPass) { mutableStateOf(config.apPass) }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 12.dp, vertical = 10.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp)
    ) {
        // 1. BLE 链路管理卡片
        SectionHeader(title = "蓝牙 GATT 链路", subtitle = "Nordic UART Service · MTU 247")
        Card(
            modifier = Modifier.fillMaxWidth(),
            colors = CardDefaults.cardColors(containerColor = DeckColors.BgSurface),
            shape = RoundedCornerShape(10.dp),
            border = BorderStroke(1.dp, DeckColors.BorderSubtle)
        ) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(12.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Icon(
                    imageVector = if (state == BleManager.ConnectionState.CONNECTED) {
                        Icons.Default.BluetoothConnected
                    } else {
                        Icons.Default.BluetoothDisabled
                    },
                    contentDescription = null,
                    tint = if (state == BleManager.ConnectionState.CONNECTED) DeckColors.BrightGreen else DeckColors.TextMuted,
                    modifier = Modifier.size(24.dp)
                )
                Spacer(modifier = Modifier.width(10.dp))
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
                    Text(
                        text = "UUID: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E",
                        color = DeckColors.TextMuted,
                        fontSize = 10.sp,
                        fontFamily = FontFamily.Monospace
                    )
                }
                if (state == BleManager.ConnectionState.DISCONNECTED) {
                    Button(
                        onClick = { BleManager.startScan() },
                        colors = ButtonDefaults.buttonColors(containerColor = DeckColors.Blue),
                        shape = RoundedCornerShape(8.dp),
                        contentPadding = PaddingValues(horizontal = 12.dp, vertical = 0.dp),
                        modifier = Modifier.height(34.dp)
                    ) {
                        Text("扫描连接", fontSize = 12.sp)
                    }
                } else {
                    OutlinedButton(
                        onClick = { BleManager.disconnect() },
                        border = BorderStroke(1.dp, DeckColors.Crimson),
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
        SectionHeader(title = "设备身份与名称伪装", subtitle = "写入 Flash 掉电记忆")
        Card(
            modifier = Modifier.fillMaxWidth(),
            colors = CardDefaults.cardColors(containerColor = DeckColors.BgSurface),
            shape = RoundedCornerShape(10.dp),
            border = BorderStroke(1.dp, DeckColors.BorderSubtle)
        ) {
            Column(
                modifier = Modifier.padding(12.dp),
                verticalArrangement = Arrangement.spacedBy(10.dp)
            ) {
                IdentityEditRow(
                    label = "BLE 蓝牙名称 (1~26 字节)",
                    hint = "如: Mi Band 8 / WiFi-Deck",
                    value = bleNameInput,
                    onValueChange = { if (it.length <= 26) bleNameInput = it },
                    buttonText = "修改蓝牙名",
                    accentColor = DeckColors.Cyan,
                    onSave = { viewModel.sendSetBleName(bleNameInput) }
                )

                HorizontalDivider(color = DeckColors.BorderSubtle)

                IdentityEditRow(
                    label = "Wi-Fi 热点名称 (SSID, 1~32 字节)",
                    hint = "如: TP-LINK_5G / WiFi-Deck",
                    value = apSsidInput,
                    onValueChange = { if (it.length <= 32) apSsidInput = it },
                    buttonText = "修改 Wi-Fi 名",
                    accentColor = DeckColors.Blue,
                    onSave = { viewModel.sendSetApSsid(apSsidInput) }
                )

                IdentityEditRow(
                    label = "Wi-Fi 热点密码 (WPA2, 8~63 位)",
                    hint = "至少 8 位字符",
                    value = apPassInput,
                    onValueChange = { if (it.length <= 63) apPassInput = it },
                    buttonText = "修改密码",
                    accentColor = DeckColors.Blue,
                    onSave = { viewModel.sendSetApPass(apPassInput) }
                )

                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text(
                        text = "修改后自动写入板载 Flash，断电重启依然生效",
                        color = DeckColors.TextMuted,
                        fontSize = 10.sp,
                        modifier = Modifier.weight(1f)
                    )
                    Spacer(modifier = Modifier.width(8.dp))
                    OutlinedButton(
                        onClick = { viewModel.sendResetConfig() },
                        border = BorderStroke(1.dp, DeckColors.BorderSubtle),
                        shape = RoundedCornerShape(6.dp),
                        contentPadding = PaddingValues(horizontal = 10.dp, vertical = 0.dp),
                        modifier = Modifier.height(28.dp)
                    ) {
                        Text("恢复默认", fontSize = 11.sp, color = DeckColors.TextSecondary)
                    }
                }
            }
        }

        // 3. 无线隐身与硬件开关卡片
        SectionHeader(title = "无线隐身与硬件控制", subtitle = "双向实时同步")
        Card(
            modifier = Modifier.fillMaxWidth(),
            colors = CardDefaults.cardColors(containerColor = DeckColors.BgSurface),
            shape = RoundedCornerShape(10.dp),
            border = BorderStroke(1.dp, DeckColors.BorderSubtle)
        ) {
            Column(modifier = Modifier.padding(12.dp)) {
                SettingSwitchRow(
                    title = "WebUI 热点广播 (ap on/off)",
                    subtitle = if (config.apEnabled) {
                        "已开启: 热点 ${config.apSsid} (192.168.1.1) 可访问"
                    } else {
                        "已关闭: 处于 BLE-only 隐身模式，STA 扫描与注入保持激活"
                    },
                    checked = config.apEnabled,
                    onCheckedChange = { viewModel.sendApToggle(it) }
                )

                HorizontalDivider(
                    color = DeckColors.BorderSubtle,
                    modifier = Modifier.padding(vertical = 8.dp)
                )

                SettingSwitchRow(
                    title = "板载 RGB LED 状态指示灯",
                    subtitle = "扫描绿灯、发包蓝灯闪烁提示（关闭可增强隐蔽性）",
                    checked = config.ledEnabled,
                    onCheckedChange = { viewModel.sendLedToggle(it) }
                )

                HorizontalDivider(
                    color = DeckColors.BorderSubtle,
                    modifier = Modifier.padding(vertical = 8.dp)
                )

                SettingSwitchRow(
                    title = "攻击循环间自动重新扫描",
                    subtitle = "每轮周期前重新扫描周围信道（会增加循环耗时）",
                    checked = config.scanBetweenCycles,
                    onCheckedChange = { viewModel.sendSetScanCycles(it) }
                )
            }
        }

        // 3. 射频与扫描参数调优卡片
        SectionHeader(title = "射频注入与扫描参数", subtitle = "点击立即下发至固件")
        Card(
            modifier = Modifier.fillMaxWidth(),
            colors = CardDefaults.cardColors(containerColor = DeckColors.BgSurface),
            shape = RoundedCornerShape(10.dp),
            border = BorderStroke(1.dp, DeckColors.BorderSubtle)
        ) {
            Column(
                modifier = Modifier.padding(12.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                SegmentedParamSelector(
                    label = "单次突发帧数 (num_frames)",
                    currentValue = "${config.numSendFrames} 帧",
                    options = listOf(1 to "1", 3 to "3", 5 to "5", 10 to "10", 20 to "20"),
                    selectedKey = config.numSendFrames,
                    onPick = { viewModel.sendSetFrames(it) }
                )

                SegmentedParamSelector(
                    label = "攻击循环间隔 (cycle_delay)",
                    currentValue = "${config.cycleDelayMs} ms",
                    options = listOf(500 to "0.5s", 1000 to "1.0s", 2000 to "2.0s", 5000 to "5.0s"),
                    selectedKey = config.cycleDelayMs.toInt(),
                    onPick = { viewModel.sendSetCycleDelay(it.toLong()) }
                )

                SegmentedParamSelector(
                    label = "双频扫描时长 (scan_time)",
                    currentValue = "${config.scanTimeMs} ms",
                    options = listOf(3000 to "3.0s", 5000 to "5.0s", 8000 to "8.0s", 10000 to "10s"),
                    selectedKey = config.scanTimeMs.toInt(),
                    onPick = { viewModel.sendSetScanTime(it.toLong()) }
                )

                SegmentedParamSelector(
                    label = "扫描起始频段 (start_channel)",
                    currentValue = if (config.startChannel >= 36) "仅 5GHz (CH 36+)" else "2.4G + 5G 双频 (CH 1+)",
                    options = listOf(1 to "2.4G + 5G 全频段", 36 to "仅 5GHz 频段"),
                    selectedKey = config.startChannel,
                    onPick = { viewModel.sendSetStartChannel(it) }
                )
            }
        }

        // 4. 串口级自定义指令直发终端
        SectionHeader(title = "CLI 指令直发终端", subtitle = "兼容全部串口 handleCommand 指令")
        Card(
            modifier = Modifier.fillMaxWidth(),
            colors = CardDefaults.cardColors(containerColor = DeckColors.BgSurface),
            shape = RoundedCornerShape(10.dp),
            border = BorderStroke(1.dp, DeckColors.BorderSubtle)
        ) {
            Column(modifier = Modifier.padding(12.dp)) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    Row(
                        modifier = Modifier
                            .weight(1f)
                            .height(38.dp)
                            .background(DeckColors.BgTerminal, RoundedCornerShape(7.dp))
                            .border(1.dp, DeckColors.BorderSubtle, RoundedCornerShape(7.dp))
                            .padding(horizontal = 10.dp),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Icon(
                            imageVector = Icons.Default.Terminal,
                            contentDescription = null,
                            tint = DeckColors.Cyan,
                            modifier = Modifier.size(14.dp)
                        )
                        Spacer(modifier = Modifier.width(6.dp))
                        Box(modifier = Modifier.weight(1f), contentAlignment = Alignment.CenterStart) {
                            if (customCmd.isEmpty()) {
                                Text(
                                    text = "输入如: set ch 149 / info / reset_txstats",
                                    color = DeckColors.TextMuted,
                                    fontSize = 11.sp,
                                    fontFamily = FontFamily.Monospace
                                )
                            }
                            BasicTextField(
                                value = customCmd,
                                onValueChange = { customCmd = it },
                                singleLine = true,
                                textStyle = TextStyle(
                                    color = DeckColors.TextPrimary,
                                    fontSize = 12.sp,
                                    fontFamily = FontFamily.Monospace
                                ),
                                cursorBrush = SolidColor(DeckColors.Cyan),
                                modifier = Modifier.fillMaxWidth()
                            )
                        }
                    }

                    Button(
                        onClick = {
                            viewModel.sendCustomCommand(customCmd)
                            customCmd = ""
                        },
                        enabled = customCmd.isNotBlank(),
                        colors = ButtonDefaults.buttonColors(containerColor = DeckColors.Cyan),
                        shape = RoundedCornerShape(7.dp),
                        contentPadding = PaddingValues(horizontal = 12.dp, vertical = 0.dp),
                        modifier = Modifier.height(38.dp)
                    ) {
                        Icon(
                            imageVector = Icons.AutoMirrored.Filled.Send,
                            contentDescription = null,
                            tint = DeckColors.BgPrimary,
                            modifier = Modifier.size(15.dp)
                        )
                    }
                }

                Spacer(modifier = Modifier.height(8.dp))
                Text(
                    text = "⚠️ 免责声明：本工具仅用于无线网络安全教学、802.11 协议分析与合法授权测试。",
                    color = DeckColors.TextMuted,
                    fontSize = 11.sp
                )
            }
        }
    }
}

@Composable
private fun SettingSwitchRow(
    title: String,
    subtitle: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit
) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Column(modifier = Modifier.weight(1f).padding(end = 8.dp)) {
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
            onCheckedChange = onCheckedChange,
            colors = SwitchDefaults.colors(
                checkedThumbColor = Color.White,
                checkedTrackColor = DeckColors.Crimson,
                uncheckedThumbColor = DeckColors.TextSecondary,
                uncheckedTrackColor = DeckColors.BgElevated
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
    onPick: (Int) -> Unit
) {
    Column {
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
                color = DeckColors.Crimson,
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
                        .clip(RoundedCornerShape(7.dp))
                        .background(if (isSelected) DeckColors.CrimsonDim else DeckColors.BgElevated)
                        .border(
                            1.dp,
                            if (isSelected) DeckColors.Crimson else DeckColors.BorderSubtle,
                            RoundedCornerShape(7.dp)
                        )
                        .clickable { onPick(key) },
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        text = text,
                        color = if (isSelected) DeckColors.Crimson else DeckColors.TextPrimary,
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
    onSave: () -> Unit
) {
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
                    .height(36.dp)
                    .background(DeckColors.BgTerminal, RoundedCornerShape(7.dp))
                    .border(1.dp, DeckColors.BorderSubtle, RoundedCornerShape(7.dp))
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
                    onValueChange = onValueChange,
                    singleLine = true,
                    textStyle = TextStyle(
                        color = DeckColors.TextPrimary,
                        fontSize = 12.sp,
                        fontFamily = FontFamily.Monospace
                    ),
                    cursorBrush = SolidColor(accentColor),
                    modifier = Modifier.fillMaxWidth()
                )
            }
            Button(
                onClick = onSave,
                enabled = value.isNotBlank(),
                colors = ButtonDefaults.buttonColors(containerColor = accentColor),
                shape = RoundedCornerShape(7.dp),
                contentPadding = PaddingValues(horizontal = 12.dp, vertical = 0.dp),
                modifier = Modifier.height(36.dp)
            ) {
                Text(
                    text = buttonText,
                    fontSize = 11.sp,
                    fontWeight = FontWeight.SemiBold,
                    color = Color.White
                )
            }
        }
    }
}
