package com.example.wifideck.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Switch
import androidx.compose.material3.Tab
import androidx.compose.material3.TabRow
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.example.wifideck.data.ApItem
import com.example.wifideck.data.BleManager
import com.example.wifideck.data.DeviceStatus

private val DarkBg = Color(0xFF121212)
private val Surface = Color(0xFF1E1E1E)
private val Accent = Color(0xFFE94560)
private val Green = Color(0xFF2ECC71)
private val Blue = Color(0xFF3498DB)
private val Amber = Color(0xFFF39C12)
private val TextPrimary = Color(0xFFE0E0E0)
private val TextMuted = Color(0xFF888888)

@Composable
fun MainScreen(viewModel: MainViewModel) {
    val connectionState by viewModel.connectionState.collectAsState()
    val apList by viewModel.apList.collectAsState()
    val status by viewModel.status.collectAsState()
    val logs by viewModel.logs.collectAsState()
    val scanning by viewModel.scanning.collectAsState()
    val selected by viewModel.selected.collectAsState()
    val toast by viewModel.toast.collectAsState()

    var tabIndex by remember { mutableIntStateOf(0) }

    LaunchedEffect(toast) {
        if (toast != null) {
            kotlinx.coroutines.delay(2000)
            viewModel.clearToast()
        }
    }

    Box(modifier = Modifier.fillMaxSize().background(DarkBg)) {
        Column(modifier = Modifier.fillMaxSize()) {
            // 顶部状态栏
            StatusBar(connectionState, status, apList.size, onConnect = { BleManager.startScan() })

            // 内容区
            Box(modifier = Modifier.weight(1f)) {
                when (tabIndex) {
                    0 -> ScanTab(viewModel, apList, scanning, selected)
                    1 -> ControlTab(viewModel, status)
                    2 -> LogTab(viewModel, logs)
                    3 -> SettingsTab(viewModel)
                }
            }

            // 底部 Tab
            TabRow(
                selectedTabIndex = tabIndex,
                containerColor = Surface,
                contentColor = Accent
            ) {
                Tab(
                    selected = tabIndex == 0,
                    onClick = { tabIndex = 0 },
                    text = { Text("扫描", fontSize = 12.sp) }
                )
                Tab(
                    selected = tabIndex == 1,
                    onClick = { tabIndex = 1 },
                    text = { Text("控制", fontSize = 12.sp) }
                )
                Tab(
                    selected = tabIndex == 2,
                    onClick = { tabIndex = 2 },
                    text = { Text("抓包", fontSize = 12.sp) }
                )
                Tab(
                    selected = tabIndex == 3,
                    onClick = { tabIndex = 3 },
                    text = { Text("设置", fontSize = 12.sp) }
                )
            }
        }

        // Toast
        val toastMsg = toast
        if (toastMsg != null) {
            Text(
                text = toastMsg,
                modifier = Modifier
                    .align(Alignment.BottomCenter)
                    .padding(bottom = 72.dp)
                    .background(Accent, RoundedCornerShape(8.dp))
                    .padding(horizontal = 16.dp, vertical = 8.dp),
                color = Color.White,
                fontSize = 13.sp
            )
        }
    }
}

@Composable
private fun StatusBar(
    state: BleManager.ConnectionState,
    status: DeviceStatus,
    apCount: Int,
    onConnect: () -> Unit
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(Surface)
            .padding(horizontal = 16.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        val dotColor = when (state) {
            BleManager.ConnectionState.CONNECTED -> if (status.attacking) Accent else Green
            BleManager.ConnectionState.CONNECTING -> Amber
            BleManager.ConnectionState.SCANNING -> Blue
            else -> TextMuted
        }
        Box(
            modifier = Modifier
                .size(10.dp)
                .background(dotColor, CircleShape)
        )
        Spacer(modifier = Modifier.width(8.dp))
        Text(
            text = when (state) {
                BleManager.ConnectionState.CONNECTED -> "已连接 · CH ${status.channel}"
                BleManager.ConnectionState.CONNECTING -> "连接中..."
                BleManager.ConnectionState.SCANNING -> "扫描设备中..."
                BleManager.ConnectionState.DISCONNECTED -> "未连接"
            },
            color = TextPrimary,
            fontSize = 14.sp,
            fontWeight = FontWeight.Medium
        )
        Spacer(modifier = Modifier.weight(1f))
        Text(
            text = "AP $apCount · TX ${status.txOk}/${status.txFail}",
            color = TextMuted,
            fontSize = 12.sp
        )
        if (state == BleManager.ConnectionState.DISCONNECTED) {
            Spacer(modifier = Modifier.width(12.dp))
            OutlinedButton(
                onClick = onConnect,
                modifier = Modifier.height(32.dp)
            ) {
                Text("连接", fontSize = 12.sp)
            }
        }
    }
}

@Composable
private fun ScanTab(
    viewModel: MainViewModel,
    apList: List<ApItem>,
    scanning: Boolean,
    selected: Set<Int>
) {
    Column(modifier = Modifier.fillMaxSize().padding(12.dp)) {
        Row {
            Button(
                onClick = { viewModel.sendScan() },
                enabled = !scanning,
                modifier = Modifier.weight(1f),
                colors = ButtonDefaults.buttonColors(containerColor = Blue)
            ) {
                Text("扫描 WiFi")
            }
            Spacer(modifier = Modifier.width(8.dp))
            OutlinedButton(onClick = { viewModel.clearSelection() }) {
                Text("清空选择")
            }
        }

        if (scanning) {
            Row(
                modifier = Modifier.padding(16.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                CircularProgressIndicator(modifier = Modifier.size(20.dp), color = Accent)
                Spacer(modifier = Modifier.width(12.dp))
                Text("扫描中... (约5秒)", color = TextMuted, fontSize = 13.sp)
            }
        }

        LazyColumn(verticalArrangement = Arrangement.spacedBy(6.dp)) {
            items(apList) { ap ->
                ApCard(ap, selected.contains(ap.index)) {
                    viewModel.toggleSelect(ap.index)
                }
            }
            if (apList.isEmpty() && !scanning) {
                item {
                    Text(
                        "暂无扫描结果，请先扫描",
                        modifier = Modifier.fillMaxWidth().padding(24.dp),
                        color = TextMuted,
                        fontSize = 13.sp
                    )
                }
            }
        }
    }
}

@Composable
private fun ApCard(ap: ApItem, isSelected: Boolean, onClick: () -> Unit) {
    val bandColor = if (ap.channel >= 36) Accent else Blue
    Card(
        modifier = Modifier
            .fillMaxWidth()
            .clickable { onClick() },
        colors = CardDefaults.cardColors(
            containerColor = if (isSelected) Color(0xFF2A1A1E) else Surface
        ),
        shape = RoundedCornerShape(8.dp),
        border = if (isSelected) androidx.compose.foundation.BorderStroke(1.dp, Accent) else null
    ) {
        Row(
            modifier = Modifier.padding(12.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(ap.ssid, color = TextPrimary, fontSize = 15.sp, fontWeight = FontWeight.SemiBold)
                Spacer(modifier = Modifier.height(4.dp))
                Text(
                    "CH ${ap.channel} · ${ap.bssid} · ${ap.rssi} dBm",
                    color = TextMuted,
                    fontSize = 11.sp
                )
            }
            Text(
                ap.band,
                modifier = Modifier
                    .background(bandColor, RoundedCornerShape(4.dp))
                    .padding(horizontal = 6.dp, vertical = 2.dp),
                color = Color.White,
                fontSize = 10.sp
            )
            Spacer(modifier = Modifier.width(8.dp))
            Box(
                modifier = Modifier
                    .size(22.dp)
                    .background(
                        if (isSelected) Accent else Color.Transparent,
                        RoundedCornerShape(4.dp)
                    ),
                contentAlignment = Alignment.Center
            ) {
                if (isSelected) Text("✓", color = Color.White, fontSize = 14.sp)
            }
        }
    }
}

@Composable
private fun ControlTab(viewModel: MainViewModel, status: DeviceStatus) {
    Column(modifier = Modifier.fillMaxSize().padding(12.dp)) {
        LazyColumn(verticalArrangement = Arrangement.spacedBy(6.dp)) {
            item { SectionTitle("Deauth 攻击") }
            item { ActionButton("开始 Deauth", Accent, status.attackEnabled) { viewModel.sendStartDeauth() } }
            item { ActionButton("开始 Disassoc", Amber, status.disassocEnabled) { viewModel.sendDisassoc() } }
            item { ActionButton("随机攻击", Surface) { viewModel.sendRandomAttack() } }
            item { ActionButton("单 AP 攻击 (选中的第一个)", Surface) { viewModel.sendSingleAp() } }

            item { HorizontalDivider(color = Surface, modifier = Modifier.padding(vertical = 6.dp)) }
            item { SectionTitle("Flood / Jam") }
            item { ActionButton("Beacon Flood", Blue, status.beaconFlood) { viewModel.sendBeaconFlood() } }
            item { ActionButton("Probe Flood", Blue, status.probeFlood) { viewModel.sendProbeFlood() } }
            item { ActionButton("Signal Jam", Accent, status.signalJam) { viewModel.sendSignalJam() } }
            item { ActionButton("Deauth 嗅探克隆", Surface, status.deauthSniff) { viewModel.sendDeauthSniff() } }

            item { HorizontalDivider(color = Surface, modifier = Modifier.padding(vertical = 6.dp)) }
            item { SectionTitle("定时攻击 (attack_time)") }
            item {
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    listOf(10_000L to "10秒", 30_000L to "30秒", 60_000L to "60秒").forEach { (ms, label) ->
                        OutlinedButton(
                            onClick = { viewModel.sendTimedAttack(ms) },
                            modifier = Modifier.weight(1f)
                        ) {
                            Text(label, fontSize = 13.sp)
                        }
                    }
                }
            }

            item { HorizontalDivider(color = Surface, modifier = Modifier.padding(vertical = 6.dp)) }
            item { SectionTitle("嗅探") }
            item { ActionButton("开始嗅探 (ALL)", Surface, status.sniffing) { viewModel.sendStartSniff() } }
            item { ActionButton("停止嗅探", Surface) { viewModel.sendStopSniff() } }

            item { HorizontalDivider(color = Surface, modifier = Modifier.padding(vertical = 6.dp)) }
            item {
                Button(
                    onClick = { viewModel.sendStopAll() },
                    modifier = Modifier.fillMaxWidth().height(48.dp),
                    colors = ButtonDefaults.buttonColors(containerColor = Green)
                ) {
                    Text("停止所有攻击", fontSize = 15.sp, fontWeight = FontWeight.Bold)
                }
            }
        }
    }
}

@Composable
private fun LogTab(viewModel: MainViewModel, logs: List<String>) {
    val listState = rememberLazyListState()
    LaunchedEffect(logs.size) {
        if (logs.isNotEmpty()) listState.animateScrollToItem(logs.size - 1)
    }
    Column(modifier = Modifier.fillMaxSize().padding(12.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("抓包终端", color = TextPrimary, fontSize = 15.sp, fontWeight = FontWeight.Medium)
            Spacer(modifier = Modifier.weight(1f))
            OutlinedButton(onClick = { viewModel.clearLogs() }) {
                Text("清空", fontSize = 12.sp)
            }
        }
        Spacer(modifier = Modifier.height(8.dp))
        Row(
            modifier = Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()),
            horizontalArrangement = Arrangement.spacedBy(6.dp)
        ) {
            FilterChip("ALL") { viewModel.sendSniffMode("all") }
            FilterChip("Beacon") { viewModel.sendSniffMode("beacon") }
            FilterChip("Probe") { viewModel.sendSniffMode("probe") }
            FilterChip("Deauth") { viewModel.sendSniffMode("deauth") }
            FilterChip("EAPOL") { viewModel.sendSniffMode("eapol") }
            FilterChip("Hop开") { viewModel.sendHopToggle(true) }
            FilterChip("Hop关") { viewModel.sendHopToggle(false) }
            FilterChip("停止") { viewModel.sendStopSniff() }
        }
        Spacer(modifier = Modifier.height(8.dp))
        LazyColumn(state = listState) {
            items(logs) { log ->
                Text(
                    log,
                    modifier = Modifier.fillMaxWidth().padding(vertical = 2.dp),
                    color = TextMuted,
                    fontSize = 11.sp
                )
            }
            if (logs.isEmpty()) {
                item {
                    Text(
                        "暂无抓包日志，开启嗅探后实时显示",
                        modifier = Modifier.fillMaxWidth().padding(24.dp),
                        color = TextMuted,
                        fontSize = 13.sp
                    )
                }
            }
        }
    }
}

@Composable
private fun SettingsTab(viewModel: MainViewModel) {
    val state by viewModel.connectionState.collectAsState()
    val config by viewModel.config.collectAsState()

    Column(modifier = Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp)) {
        SectionTitle("连接")
        when (state) {
            BleManager.ConnectionState.DISCONNECTED -> {
                Button(
                    onClick = { BleManager.startScan() },
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Text("扫描并连接设备")
                }
            }
            else -> {
                OutlinedButton(
                    onClick = { BleManager.disconnect() },
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Text("断开连接")
                }
            }
        }

        Spacer(modifier = Modifier.height(24.dp))
        SectionTitle("参数配置 (实时同步)")

        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("LED 指示灯", color = TextPrimary, fontSize = 14.sp, modifier = Modifier.weight(1f))
            Switch(checked = config.ledEnabled, onCheckedChange = { viewModel.sendLedToggle(it) })
        }

        ParamRow("每次发送帧数", config.numSendFrames, listOf(1, 3, 5, 10)) { viewModel.sendSetFrames(it) }
        ParamRow("循环间隔 (ms)", config.cycleDelayMs.toInt(), listOf(500, 1000, 2000, 5000)) { viewModel.sendSetCycleDelay(it.toLong()) }
        ParamRow("扫描时长 (ms)", config.scanTimeMs.toInt(), listOf(3000, 5000, 10000)) { viewModel.sendSetScanTime(it.toLong()) }
        ParamRow("起始信道", config.startChannel, listOf(1, 36)) { viewModel.sendSetStartChannel(it) }

        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("攻击间隔重扫", color = TextPrimary, fontSize = 14.sp, modifier = Modifier.weight(1f))
            Switch(checked = config.scanBetweenCycles, onCheckedChange = { viewModel.sendSetScanCycles(it) })
        }

        Spacer(modifier = Modifier.height(24.dp))
        SectionTitle("说明")
        Text(
            "设备上电后会自动广播 BLE 名称 \"WiFi-Deck\"。\n" +
                "连接后即可像串口一样收发指令，支持全部 handleCommand 指令集。\n\n" +
                "⚠️ 仅用于教育目的和授权的安全测试。",
            color = TextMuted,
            fontSize = 13.sp,
            lineHeight = 20.sp
        )
    }
}

@Composable
private fun SectionTitle(text: String) {
    Text(
        text,
        color = TextMuted,
        fontSize = 12.sp,
        fontWeight = FontWeight.Medium,
        modifier = Modifier.padding(top = 4.dp, bottom = 4.dp)
    )
}

@Composable
private fun ActionButton(label: String, color: Color, active: Boolean = false, onClick: () -> Unit) {
    Button(
        onClick = onClick,
        modifier = Modifier.fillMaxWidth().height(44.dp),
        colors = ButtonDefaults.buttonColors(
            containerColor = if (active) Green else color
        )
    ) {
        Text(if (active) "● $label" else label, fontSize = 14.sp)
    }
}

@Composable
private fun ParamRow(label: String, currentValue: Int, options: List<Int>, onPick: (Int) -> Unit) {
    Column(modifier = Modifier.padding(vertical = 6.dp)) {
        Text("$label (当前: $currentValue)", color = TextPrimary, fontSize = 13.sp)
        Spacer(modifier = Modifier.height(4.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            options.forEach { v ->
                val selected = (v == currentValue)
                OutlinedButton(
                    onClick = { onPick(v) },
                    modifier = Modifier.weight(1f),
                    colors = ButtonDefaults.outlinedButtonColors(
                        containerColor = if (selected) Color(0xFF2A1A1E) else Color.Transparent
                    )
                ) {
                    Text("$v", fontSize = 13.sp, color = if (selected) Accent else TextPrimary)
                }
            }
        }
    }
}

@Composable
private fun FilterChip(label: String, onClick: () -> Unit) {
    OutlinedButton(onClick = onClick, modifier = Modifier.height(32.dp)) {
        Text(label, fontSize = 11.sp)
    }
}
