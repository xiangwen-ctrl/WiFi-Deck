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
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Bolt
import androidx.compose.material.icons.filled.CellTower
import androidx.compose.material.icons.filled.ContentCopy
import androidx.compose.material.icons.filled.GpsFixed
import androidx.compose.material.icons.filled.LinkOff
import androidx.compose.material.icons.filled.Podcasts
import androidx.compose.material.icons.filled.RestartAlt
import androidx.compose.material.icons.filled.Shuffle
import androidx.compose.material.icons.filled.StopCircle
import androidx.compose.material.icons.filled.Timer
import androidx.compose.material.icons.filled.WifiOff
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.github.xiangwen.wifideck.data.ApItem
import io.github.xiangwen.wifideck.data.DeviceStatus
import io.github.xiangwen.wifideck.ui.MainViewModel
import io.github.xiangwen.wifideck.ui.components.SectionHeader
import io.github.xiangwen.wifideck.ui.components.TacticalActionCard
import io.github.xiangwen.wifideck.ui.theme.DeckColors

private val TIMED_ATTACK_PRESETS = listOf(
    10_000L to "10s",
    30_000L to "30s",
    60_000L to "60s",
    120_000L to "120s"
)

@Composable
fun ControlTab(
    viewModel: MainViewModel,
    onNavigateToScan: () -> Unit
) {
    val scrollState = rememberScrollState()

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(scrollState)
            .padding(horizontal = 14.dp, vertical = 10.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp)
    ) {
        // 1. 目标概览与射频计数器卡片
        TargetAndStatsSection(
            viewModel = viewModel,
            onNavigateToScan = onNavigateToScan
        )

        // 2-5. 定向测试、空口泛洪、定时测试与急停复位
        TacticalControlsSection(viewModel = viewModel)
    }
}

@Composable
private fun TargetAndStatsSection(
    viewModel: MainViewModel,
    onNavigateToScan: () -> Unit
) {
    val selectedAps by viewModel.selectedAps.collectAsStateWithLifecycle()
    val status by viewModel.status.collectAsStateWithLifecycle()
    val onResetStats = remember(viewModel) { { viewModel.sendResetTxStats() } }

    TargetAndStatsCard(
        selectedAps = selectedAps,
        status = status,
        onNavigateToScan = onNavigateToScan,
        onResetStats = onResetStats
    )
}

@Composable
private fun TacticalControlsSection(viewModel: MainViewModel) {
    val status by viewModel.status.collectAsStateWithLifecycle()

    // 2. 802.11 定向与解认证测试 (2x2 网格)
    SectionHeader(
        title = "802.11 定向测试",
        subtitle = "需先在扫描页锁定目标"
    )
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(10.dp)
    ) {
        TacticalActionCard(
            title = "Deauth 压制",
            subtitle = "持续广播解认证帧",
            icon = Icons.Default.WifiOff,
            accentColor = DeckColors.Crimson,
            active = status.attackEnabled,
            modifier = Modifier.weight(1f)
        ) { viewModel.sendStartDeauth() }

        TacticalActionCard(
            title = "Disassoc 剥离",
            subtitle = "持续发送解关联帧",
            icon = Icons.Default.LinkOff,
            accentColor = DeckColors.Amber,
            active = status.disassocEnabled,
            modifier = Modifier.weight(1f)
        ) { viewModel.sendDisassoc() }
    }

    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(10.dp)
    ) {
        TacticalActionCard(
            title = "单目标点射",
            subtitle = "对首个目标突发注入",
            icon = Icons.Default.GpsFixed,
            accentColor = DeckColors.Cyan,
            active = false,
            modifier = Modifier.weight(1f)
        ) { viewModel.sendSingleAp() }

        TacticalActionCard(
            title = "随机信道抽样",
            subtitle = "随机抽取热点突发测试",
            icon = Icons.Default.Shuffle,
            accentColor = DeckColors.Blue,
            active = false,
            modifier = Modifier.weight(1f)
        ) { viewModel.sendRandomAttack() }
    }

    // 3. 泛洪与全频段干扰 (2x2 网格)
    SectionHeader(
        title = "空口泛洪与双频干扰",
        subtitle = "全频段信标/探测与克隆重放"
    )
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(10.dp)
    ) {
        TacticalActionCard(
            title = "Beacon 伪热点",
            subtitle = "高频广播随机 SSID",
            icon = Icons.Default.CellTower,
            accentColor = DeckColors.Blue,
            active = status.beaconFlood,
            modifier = Modifier.weight(1f)
        ) { viewModel.sendBeaconFlood() }

        TacticalActionCard(
            title = "Probe 探测泛洪",
            subtitle = "模拟海量客户端探测",
            icon = Icons.Default.Podcasts,
            accentColor = DeckColors.Purple,
            active = status.probeFlood,
            modifier = Modifier.weight(1f)
        ) { viewModel.sendProbeFlood() }
    }

    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(10.dp)
    ) {
        TacticalActionCard(
            title = "Signal Jam 阻塞",
            subtitle = "2.4G+5G 全信道跳频压制",
            icon = Icons.Default.Bolt,
            accentColor = DeckColors.Crimson,
            active = status.signalJam,
            modifier = Modifier.weight(1f)
        ) { viewModel.sendSignalJam() }

        TacticalActionCard(
            title = "Deauth 嗅探克隆",
            subtitle = "捕获空中帧并克隆重放",
            icon = Icons.Default.ContentCopy,
            accentColor = DeckColors.Amber,
            active = status.deauthSniff,
            modifier = Modifier.weight(1f)
        ) { viewModel.sendDeauthSniff() }
    }

    // 4. 定时自动测试卡片
    SectionHeader(
        title = "定时自动测试",
        subtitle = if (status.timedRemainingSec > 0) "倒计时剩余 ${status.timedRemainingSec}s" else "到期自动停机恢复"
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
                .padding(10.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            TIMED_ATTACK_PRESETS.forEach { (ms, label) ->
                Box(
                    modifier = Modifier
                        .weight(1f)
                        .height(36.dp)
                        .clip(RoundedCornerShape(8.dp))
                        .background(DeckColors.BgElevated)
                        .border(1.dp, DeckColors.BorderSubtle, RoundedCornerShape(8.dp))
                        .clickable { viewModel.sendTimedAttack(ms) },
                    contentAlignment = Alignment.Center
                ) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Icon(
                            imageVector = Icons.Default.Timer,
                            contentDescription = null,
                            tint = DeckColors.Cyan,
                            modifier = Modifier.size(13.dp)
                        )
                        Spacer(modifier = Modifier.width(4.dp))
                        Text(
                            text = label,
                            fontSize = 12.sp,
                            fontWeight = FontWeight.SemiBold,
                            color = DeckColors.TextPrimary,
                            fontFamily = FontFamily.Monospace
                        )
                    }
                }
            }
        }
    }

    Spacer(modifier = Modifier.height(2.dp))

    // 5. 底部急停与复位按钮
    val isRunning = status.attacking || status.sniffing
    Button(
        onClick = { viewModel.sendStopAll() },
        modifier = Modifier
            .fillMaxWidth()
            .height(48.dp),
        shape = RoundedCornerShape(12.dp),
        colors = ButtonDefaults.buttonColors(
            containerColor = if (isRunning) DeckColors.Crimson else DeckColors.BgElevated
        ),
        border = if (!isRunning) BorderStroke(1.dp, DeckColors.BorderStrong) else null
    ) {
        Icon(
            imageVector = Icons.Default.StopCircle,
            contentDescription = null,
            tint = if (isRunning) Color.White else DeckColors.TextSecondary,
            modifier = Modifier.size(18.dp)
        )
        Spacer(modifier = Modifier.width(8.dp))
        Text(
            text = if (isRunning) "紧急停止所有运行任务 (STOP ALL)" else "停止所有任务并复位信道",
            fontSize = 13.sp,
            fontWeight = FontWeight.Bold,
            color = if (isRunning) Color.White else DeckColors.TextSecondary
        )
    }
}

@Composable
private fun TargetAndStatsCard(
    selectedAps: List<ApItem>,
    status: DeviceStatus,
    onNavigateToScan: () -> Unit,
    onResetStats: () -> Unit
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = DeckColors.BgSurface),
        shape = RoundedCornerShape(12.dp),
        border = BorderStroke(1.dp, DeckColors.BorderSubtle)
    ) {
        Column(modifier = Modifier.padding(12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        text = if (selectedAps.isEmpty()) "当前未锁定目标" else "已锁定 ${selectedAps.size} 个目标热点",
                        color = if (selectedAps.isEmpty()) DeckColors.Amber else DeckColors.TextPrimary,
                        fontSize = 14.sp,
                        fontWeight = FontWeight.SemiBold
                    )
                    Spacer(modifier = Modifier.height(2.dp))
                    Text(
                        text = if (selectedAps.isEmpty()) "请先前往扫描页勾选测试目标" else "点击下方功能卡片将自动同步目标并执行",
                        color = DeckColors.TextMuted,
                        fontSize = 11.sp
                    )
                }
                Box(
                    modifier = Modifier
                        .clip(RoundedCornerShape(8.dp))
                        .background(DeckColors.BlueDim)
                        .border(1.dp, DeckColors.Blue.copy(alpha = 0.45f), RoundedCornerShape(8.dp))
                        .clickable { onNavigateToScan() }
                        .padding(horizontal = 10.dp, vertical = 6.dp)
                ) {
                    Text(
                        text = if (selectedAps.isEmpty()) "去选目标" else "修改目标",
                        fontSize = 11.sp,
                        fontWeight = FontWeight.SemiBold,
                        color = DeckColors.Blue
                    )
                }
            }

            if (selectedAps.isNotEmpty()) {
                Spacer(modifier = Modifier.height(10.dp))
                LazyRow(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(6.dp)
                ) {
                    items(
                        items = selectedAps,
                        key = { it.index },
                        contentType = { "selected_ap_chip" }
                    ) { ap ->
                        Row(
                            modifier = Modifier
                                .background(DeckColors.BgTerminal, RoundedCornerShape(6.dp))
                                .border(0.5.dp, DeckColors.BorderStrong, RoundedCornerShape(6.dp))
                                .padding(horizontal = 8.dp, vertical = 4.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Text(
                                text = ap.ssid.ifBlank { "(隐藏)" },
                                color = DeckColors.TextPrimary,
                                fontSize = 11.sp,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis
                            )
                            Spacer(modifier = Modifier.width(5.dp))
                            Text(
                                text = "CH${ap.channel}",
                                color = DeckColors.Cyan,
                                fontSize = 10.sp,
                                fontFamily = FontFamily.Monospace,
                                fontWeight = FontWeight.Bold
                            )
                        }
                    }
                }
            }

            Spacer(modifier = Modifier.height(10.dp))

            // 底层 TX 帧统计条
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(DeckColors.BgTerminal, RoundedCornerShape(8.dp))
                    .border(0.5.dp, DeckColors.BorderSubtle, RoundedCornerShape(8.dp))
                    .padding(horizontal = 10.dp, vertical = 7.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    text = "802.11 注入计数",
                    color = DeckColors.TextMuted,
                    fontSize = 11.sp
                )
                Spacer(modifier = Modifier.width(10.dp))
                Text(
                    text = "OK ${status.txOk}",
                    color = DeckColors.BrightGreen,
                    fontSize = 12.sp,
                    fontWeight = FontWeight.Bold,
                    fontFamily = FontFamily.Monospace
                )
                Spacer(modifier = Modifier.width(12.dp))
                Text(
                    text = "FAIL ${status.txFail}",
                    color = if (status.txFail > 0) DeckColors.Amber else DeckColors.TextMuted,
                    fontSize = 12.sp,
                    fontWeight = FontWeight.Bold,
                    fontFamily = FontFamily.Monospace
                )
                Spacer(modifier = Modifier.weight(1f))
                Row(
                    modifier = Modifier
                        .clip(RoundedCornerShape(5.dp))
                        .background(DeckColors.BgElevated)
                        .clickable { onResetStats() }
                        .padding(horizontal = 7.dp, vertical = 3.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Icon(
                        imageVector = Icons.Default.RestartAlt,
                        contentDescription = null,
                        tint = DeckColors.TextSecondary,
                        modifier = Modifier.size(12.dp)
                    )
                    Spacer(modifier = Modifier.width(3.dp))
                    Text("清零", color = DeckColors.TextSecondary, fontSize = 10.sp)
                }
            }
        }
    }
}
