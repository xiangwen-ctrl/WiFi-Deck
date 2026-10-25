package io.github.xiangwen.wifideck.ui.tabs

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
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
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ContentCopy
import androidx.compose.material.icons.filled.DeleteSweep
import androidx.compose.material.icons.filled.Pause
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material.icons.filled.SyncAlt
import androidx.compose.material.icons.filled.Terminal
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import io.github.xiangwen.wifideck.data.DeviceStatus
import io.github.xiangwen.wifideck.ui.MainViewModel
import io.github.xiangwen.wifideck.ui.components.DeckFilterChip
import io.github.xiangwen.wifideck.ui.theme.DeckColors

@Composable
fun SniffTab(
    viewModel: MainViewModel,
    logs: List<String>,
    status: DeviceStatus,
    autoScroll: Boolean
) {
    val listState = rememberLazyListState()
    val clipboard = LocalClipboardManager.current

    LaunchedEffect(logs.size, autoScroll) {
        if (autoScroll && logs.isNotEmpty()) {
            listState.animateScrollToItem(logs.size - 1)
        }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(horizontal = 12.dp, vertical = 10.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        // 1. 抓包控制与协议过滤卡片
        Card(
            modifier = Modifier.fillMaxWidth(),
            colors = CardDefaults.cardColors(containerColor = DeckColors.BgSurface),
            shape = RoundedCornerShape(10.dp),
            border = BorderStroke(1.dp, DeckColors.BorderSubtle)
        ) {
            Column(
                modifier = Modifier.padding(10.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                // 第一行：混杂模式主开关 + 自动跳频开关 + 定点信道速选
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(6.dp)
                ) {
                    Button(
                        onClick = { viewModel.sendStartSniff() },
                        colors = ButtonDefaults.buttonColors(
                            containerColor = if (status.sniffing) DeckColors.Crimson else DeckColors.Purple
                        ),
                        shape = RoundedCornerShape(7.dp),
                        contentPadding = PaddingValues(horizontal = 12.dp, vertical = 0.dp),
                        modifier = Modifier.height(32.dp)
                    ) {
                        Icon(
                            imageVector = if (status.sniffing) Icons.Default.Stop else Icons.Default.PlayArrow,
                            contentDescription = null,
                            modifier = Modifier.size(15.dp),
                            tint = Color.White
                        )
                        Spacer(modifier = Modifier.width(4.dp))
                        Text(
                            text = if (status.sniffing) "停止抓包" else "开启抓包",
                            fontSize = 12.sp,
                            fontWeight = FontWeight.Bold,
                            color = Color.White
                        )
                    }

                    // 跳频切换
                    DeckFilterChip(
                        label = if (status.hopping) "跳频: 开 (CH ${status.channel})" else "跳频: 关 (CH ${status.channel})",
                        selected = status.hopping,
                        accentColor = DeckColors.Cyan
                    ) { viewModel.sendHopToggle(!status.hopping) }
                }

                // 第二行：协议帧类型过滤
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .horizontalScroll(rememberScrollState()),
                    horizontalArrangement = Arrangement.spacedBy(6.dp)
                ) {
                    DeckFilterChip(
                        label = "ALL 全量",
                        selected = status.sniffing && status.sniffMode == 0,
                        accentColor = DeckColors.Cyan
                    ) { viewModel.sendSniffMode("all") }

                    DeckFilterChip(
                        label = "EAPOL 握手",
                        selected = status.sniffing && status.sniffMode == 4,
                        accentColor = DeckColors.Amber
                    ) { viewModel.sendSniffMode("eapol") }

                    DeckFilterChip(
                        label = "Deauth 告警",
                        selected = status.sniffing && status.sniffMode == 3,
                        accentColor = DeckColors.Crimson
                    ) { viewModel.sendSniffMode("deauth") }

                    DeckFilterChip(
                        label = "Beacon 信标",
                        selected = status.sniffing && status.sniffMode == 1,
                        accentColor = DeckColors.Blue
                    ) { viewModel.sendSniffMode("beacon") }

                    DeckFilterChip(
                        label = "Probe 探测",
                        selected = status.sniffing && status.sniffMode == 2,
                        accentColor = DeckColors.Purple
                    ) { viewModel.sendSniffMode("probe") }

                    DeckFilterChip(
                        label = "Pwnagotchi",
                        selected = status.sniffing && status.sniffMode == 5,
                        accentColor = DeckColors.BrightGreen
                    ) { viewModel.sendSniffMode("pwnagotchi") }
                }

                // 第三行：常用双频信道快速锁定
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .horizontalScroll(rememberScrollState()),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(5.dp)
                ) {
                    Text(
                        text = "锁定信道:",
                        color = DeckColors.TextMuted,
                        fontSize = 10.sp
                    )
                    listOf(1, 6, 11, 13, 36, 44, 149, 157, 165).forEach { ch ->
                        val isCurrent = !status.hopping && status.channel == ch
                        Box(
                            modifier = Modifier
                                .clip(RoundedCornerShape(5.dp))
                                .background(if (isCurrent) DeckColors.BlueDim else DeckColors.BgElevated)
                                .border(
                                    0.5.dp,
                                    if (isCurrent) DeckColors.Blue else DeckColors.BorderSubtle,
                                    RoundedCornerShape(5.dp)
                                )
                                .clickable { viewModel.sendSetChannel(ch) }
                                .padding(horizontal = 7.dp, vertical = 3.dp)
                        ) {
                            Text(
                                text = "$ch",
                                color = if (isCurrent) DeckColors.Blue else DeckColors.TextSecondary,
                                fontSize = 10.sp,
                                fontFamily = FontFamily.Monospace,
                                fontWeight = if (isCurrent) FontWeight.Bold else FontWeight.Normal
                            )
                        }
                    }
                }
            }
        }

        // 2. 终端窗口主体
        Column(
            modifier = Modifier
                .weight(1f)
                .fillMaxWidth()
                .clip(RoundedCornerShape(10.dp))
                .background(DeckColors.BgTerminal)
                .border(1.dp, DeckColors.BorderSubtle, RoundedCornerShape(10.dp))
        ) {
            // 终端顶部状态与工具栏
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(DeckColors.BgSurface)
                    .padding(horizontal = 10.dp, vertical = 7.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Icon(
                    imageVector = Icons.Default.Terminal,
                    contentDescription = null,
                    tint = DeckColors.Cyan,
                    modifier = Modifier.size(14.dp)
                )
                Spacer(modifier = Modifier.width(6.dp))
                Text(
                    text = "802.11 PACKET STREAM (${logs.size}/500)",
                    color = DeckColors.TextSecondary,
                    fontSize = 11.sp,
                    fontFamily = FontFamily.Monospace,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier.weight(1f)
                )

                // 自动滚屏开关
                TerminalToolButton(
                    label = if (autoScroll) "自动滚屏" else "滚屏已锁",
                    active = autoScroll,
                    onClick = { viewModel.toggleAutoScroll() }
                )
                Spacer(modifier = Modifier.width(6.dp))
                TerminalToolButton(
                    label = "复制",
                    active = false,
                    onClick = {
                        if (logs.isNotEmpty()) {
                            clipboard.setText(AnnotatedString(logs.joinToString("\n")))
                            viewModel.showToast("已复制 ${logs.size} 条抓包记录")
                        }
                    }
                )
                Spacer(modifier = Modifier.width(6.dp))
                TerminalToolButton(
                    label = "清空",
                    active = false,
                    onClick = { viewModel.clearLogs() }
                )
            }

            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(1.dp)
                    .background(DeckColors.BorderSubtle)
            )

            // 抓包日志流
            LazyColumn(
                state = listState,
                modifier = Modifier
                    .fillMaxSize()
                    .padding(horizontal = 8.dp, vertical = 6.dp),
                verticalArrangement = Arrangement.spacedBy(4.dp)
            ) {
                itemsIndexed(logs) { idx, log ->
                    PacketLogRow(index = idx + 1, rawLog = log)
                }

                if (logs.isEmpty()) {
                    item {
                        Column(
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(vertical = 56.dp),
                            horizontalAlignment = Alignment.CenterHorizontally
                        ) {
                            Text(
                                text = if (status.sniffing) "正在监听空中 802.11 帧..." else "混杂模式嗅探未开启",
                                color = DeckColors.TextSecondary,
                                fontSize = 13.sp,
                                fontFamily = FontFamily.Monospace
                            )
                            Spacer(modifier = Modifier.height(4.dp))
                            Text(
                                text = "点击上方「开启抓包」或任一协议标签（如 EAPOL / Deauth）即可实时捕获",
                                color = DeckColors.TextMuted,
                                fontSize = 11.sp
                            )
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun TerminalToolButton(label: String, active: Boolean, onClick: () -> Unit) {
    Box(
        modifier = Modifier
            .clip(RoundedCornerShape(5.dp))
            .background(if (active) DeckColors.CyanDim else DeckColors.BgElevated)
            .border(
                0.5.dp,
                if (active) DeckColors.Cyan else DeckColors.BorderSubtle,
                RoundedCornerShape(5.dp)
            )
            .clickable { onClick() }
            .padding(horizontal = 7.dp, vertical = 3.dp)
    ) {
        Text(
            text = label,
            color = if (active) DeckColors.Cyan else DeckColors.TextSecondary,
            fontSize = 10.sp
        )
    }
}

@Composable
private fun PacketLogRow(index: Int, rawLog: String) {
    val (tagText, tagColor, tagBg) = when {
        rawLog.contains("EAPOL", ignoreCase = true) ->
            Triple("EAPOL", DeckColors.Amber, DeckColors.AmberDim)
        rawLog.contains("Deauth", ignoreCase = true) || rawLog.contains("Disassoc", ignoreCase = true) ->
            Triple("DEAUTH", DeckColors.Crimson, DeckColors.CrimsonDim)
        rawLog.contains("Beacon", ignoreCase = true) ->
            Triple("BEACON", DeckColors.Blue, DeckColors.BlueDim)
        rawLog.contains("Probe", ignoreCase = true) ->
            Triple("PROBE", DeckColors.Purple, DeckColors.PurpleDim)
        else ->
            Triple("FRAME", DeckColors.Cyan, DeckColors.CyanDim)
    }

    val cleanedText = rawLog
        .removePrefix("[MGMT] ")
        .removePrefix("[DATA] ")
        .removePrefix("[CTRL] ")
        .removePrefix("[EXT] ")

    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(
                if (tagText == "EAPOL" || tagText == "DEAUTH") tagBg.copy(alpha = 0.4f) else Color.Transparent,
                RoundedCornerShape(4.dp)
            )
            .padding(horizontal = 4.dp, vertical = 2.dp),
        verticalAlignment = Alignment.Top
    ) {
        Text(
            text = "%03d".format(index),
            color = DeckColors.TextMuted,
            fontSize = 10.sp,
            fontFamily = FontFamily.Monospace,
            modifier = Modifier.width(26.dp)
        )
        Text(
            text = tagText,
            color = tagColor,
            fontSize = 9.sp,
            fontFamily = FontFamily.Monospace,
            fontWeight = FontWeight.Bold,
            modifier = Modifier
                .background(tagBg, RoundedCornerShape(3.dp))
                .padding(horizontal = 4.dp, vertical = 1.dp)
        )
        Spacer(modifier = Modifier.width(6.dp))
        Text(
            text = cleanedText,
            color = if (tagText == "EAPOL") DeckColors.Amber else DeckColors.TextPrimary,
            fontSize = 11.sp,
            fontFamily = FontFamily.Monospace,
            lineHeight = 15.sp
        )
    }
}
