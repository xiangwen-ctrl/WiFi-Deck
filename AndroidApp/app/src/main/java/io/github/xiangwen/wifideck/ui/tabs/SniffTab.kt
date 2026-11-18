package io.github.xiangwen.wifideck.ui.tabs

import android.content.ActivityNotFoundException
import android.content.Intent
import android.os.SystemClock
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
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material.icons.filled.Terminal
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.github.xiangwen.wifideck.data.SniffLogCategory
import io.github.xiangwen.wifideck.data.SniffLogEntry
import io.github.xiangwen.wifideck.data.SniffMode
import io.github.xiangwen.wifideck.ui.MainViewModel
import io.github.xiangwen.wifideck.ui.components.DeckFilterChip
import io.github.xiangwen.wifideck.ui.theme.DeckColors
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map

private const val AUTO_SCROLL_MIN_INTERVAL_MS = 80L

private val QUICK_SNIFF_CHANNELS = listOf(1, 6, 11, 13, 36, 44, 149, 157, 165)

@Immutable
private data class SniffControlState(
    val sniffing: Boolean = false,
    val sniffMode: Int = SniffMode.STOP.code,
    val hopping: Boolean = false,
    val channel: Int = 0
)

@Immutable
private data class LogCategoryStyle(
    val tagColor: Color,
    val tagBg: Color,
    val textColor: Color,
    val rowBg: Color
)

private val STYLE_EAPOL = LogCategoryStyle(
    tagColor = DeckColors.Amber,
    tagBg = DeckColors.AmberDim,
    textColor = DeckColors.Amber,
    rowBg = DeckColors.AmberDim.copy(alpha = 0.35f)
)
private val STYLE_DEAUTH = LogCategoryStyle(
    tagColor = DeckColors.Crimson,
    tagBg = DeckColors.CrimsonDim,
    textColor = DeckColors.TextPrimary,
    rowBg = DeckColors.CrimsonDim.copy(alpha = 0.35f)
)
private val STYLE_BEACON = LogCategoryStyle(
    tagColor = DeckColors.Blue,
    tagBg = DeckColors.BlueDim,
    textColor = DeckColors.TextPrimary,
    rowBg = Color.Transparent
)
private val STYLE_PROBE = LogCategoryStyle(
    tagColor = DeckColors.Purple,
    tagBg = DeckColors.PurpleDim,
    textColor = DeckColors.TextPrimary,
    rowBg = Color.Transparent
)
private val STYLE_FRAME = LogCategoryStyle(
    tagColor = DeckColors.Cyan,
    tagBg = DeckColors.CyanDim,
    textColor = DeckColors.TextPrimary,
    rowBg = Color.Transparent
)

private fun SniffLogCategory.style(): LogCategoryStyle = when (this) {
    SniffLogCategory.EAPOL -> STYLE_EAPOL
    SniffLogCategory.DEAUTH -> STYLE_DEAUTH
    SniffLogCategory.BEACON -> STYLE_BEACON
    SniffLogCategory.PROBE -> STYLE_PROBE
    SniffLogCategory.FRAME -> STYLE_FRAME
}

@Composable
fun SniffTab(viewModel: MainViewModel) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(horizontal = 14.dp, vertical = 10.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp)
    ) {
        // 1. 紧凑型抓包控制与信道/协议过滤栏（独立订阅控制字段，避免高频日志触发重组）
        SniffControlCard(viewModel = viewModel)

        // 2. 终端窗口主体
        PacketStreamTerminalCard(
            viewModel = viewModel,
            modifier = Modifier
                .weight(1f)
                .fillMaxWidth()
        )
    }
}

@Composable
private fun SniffControlCard(viewModel: MainViewModel) {
    val control by remember(viewModel) {
        viewModel.status
            .map { SniffControlState(it.sniffing, it.sniffMode, it.hopping, it.channel) }
            .distinctUntilChanged()
    }.collectAsStateWithLifecycle(initialValue = SniffControlState())

    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = DeckColors.BgSurface),
        shape = RoundedCornerShape(12.dp),
        border = BorderStroke(1.dp, DeckColors.BorderSubtle)
    ) {
        Column(
            modifier = Modifier.padding(10.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            // 第一行：混杂模式主开关 + 跳频开关 + 常用双频信道横向速选
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(6.dp)
            ) {
                Button(
                    onClick = { viewModel.sendStartSniff() },
                    colors = ButtonDefaults.buttonColors(
                        containerColor = if (control.sniffing) DeckColors.Crimson else DeckColors.Purple
                    ),
                    shape = RoundedCornerShape(8.dp),
                    contentPadding = PaddingValues(horizontal = 11.dp, vertical = 0.dp),
                    modifier = Modifier.height(32.dp)
                ) {
                    Icon(
                        imageVector = if (control.sniffing) Icons.Default.Stop else Icons.Default.PlayArrow,
                        contentDescription = null,
                        modifier = Modifier.size(14.dp),
                        tint = Color.White
                    )
                    Spacer(modifier = Modifier.width(4.dp))
                    Text(
                        text = if (control.sniffing) "停止" else "抓包",
                        fontSize = 12.sp,
                        fontWeight = FontWeight.Bold,
                        color = Color.White
                    )
                }

                DeckFilterChip(
                    label = if (control.hopping) "跳频: 开" else "跳频: 关",
                    selected = control.hopping,
                    accentColor = DeckColors.Cyan
                ) { viewModel.sendHopToggle(!control.hopping) }

                Row(
                    modifier = Modifier
                        .weight(1f)
                        .horizontalScroll(rememberScrollState()),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(4.dp)
                ) {
                    QUICK_SNIFF_CHANNELS.forEach { ch ->
                        val isCurrent = !control.hopping && control.channel == ch
                        Box(
                            modifier = Modifier
                                .clip(RoundedCornerShape(6.dp))
                                .background(if (isCurrent) DeckColors.BlueDim else DeckColors.BgElevated)
                                .border(
                                    0.5.dp,
                                    if (isCurrent) DeckColors.Blue else DeckColors.BorderSubtle,
                                    RoundedCornerShape(6.dp)
                                )
                                .clickable { viewModel.sendSetChannel(ch) }
                                .padding(horizontal = 7.dp, vertical = 4.dp)
                        ) {
                            Text(
                                text = "CH$ch",
                                color = if (isCurrent) DeckColors.Blue else DeckColors.TextSecondary,
                                fontSize = 10.sp,
                                fontFamily = FontFamily.Monospace,
                                fontWeight = if (isCurrent) FontWeight.Bold else FontWeight.Normal
                            )
                        }
                    }
                }
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
                    selected = control.sniffing && control.sniffMode == 0,
                    accentColor = DeckColors.Cyan
                ) { viewModel.sendSniffMode("all") }

                DeckFilterChip(
                    label = "EAPOL 握手",
                    selected = control.sniffing && control.sniffMode == 4,
                    accentColor = DeckColors.Amber
                ) { viewModel.sendSniffMode("eapol") }

                DeckFilterChip(
                    label = "Deauth 告警",
                    selected = control.sniffing && control.sniffMode == 3,
                    accentColor = DeckColors.Crimson
                ) { viewModel.sendSniffMode("deauth") }

                DeckFilterChip(
                    label = "Beacon 信标",
                    selected = control.sniffing && control.sniffMode == 1,
                    accentColor = DeckColors.Blue
                ) { viewModel.sendSniffMode("beacon") }

                DeckFilterChip(
                    label = "Probe 探测",
                    selected = control.sniffing && control.sniffMode == 2,
                    accentColor = DeckColors.Purple
                ) { viewModel.sendSniffMode("probe") }

                DeckFilterChip(
                    label = "Pwnagotchi",
                    selected = control.sniffing && control.sniffMode == 5,
                    accentColor = DeckColors.BrightGreen
                ) { viewModel.sendSniffMode("pwnagotchi") }
            }
        }
    }
}

@Composable
private fun PacketStreamTerminalCard(
    viewModel: MainViewModel,
    modifier: Modifier = Modifier
) {
    Column(
        modifier = modifier
            .clip(RoundedCornerShape(12.dp))
            .background(DeckColors.BgTerminal)
            .border(1.dp, DeckColors.BorderSubtle, RoundedCornerShape(12.dp))
    ) {
        PacketStreamToolbar(viewModel = viewModel)

        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height(1.dp)
                .background(DeckColors.BorderSubtle)
        )

        PacketLogLazyList(
            viewModel = viewModel,
            modifier = Modifier.fillMaxSize()
        )
    }
}

@Composable
private fun PacketStreamToolbar(viewModel: MainViewModel) {
    val logs by viewModel.logs.collectAsStateWithLifecycle()
    val isSniffing by viewModel.isSniffing.collectAsStateWithLifecycle()
    val autoScroll by viewModel.autoScroll.collectAsStateWithLifecycle()
    val clipboard = LocalClipboardManager.current
    val context = LocalContext.current

    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(DeckColors.BgSurface)
            .padding(horizontal = 12.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Box(
            modifier = Modifier
                .size(7.dp)
                .background(
                    if (isSniffing) DeckColors.Purple else DeckColors.TextMuted,
                    CircleShape
                )
        )
        Spacer(modifier = Modifier.width(7.dp))
        Text(
            text = "PACKET STREAM (${logs.size}/500)",
            color = DeckColors.TextSecondary,
            fontSize = 11.sp,
            fontFamily = FontFamily.Monospace,
            fontWeight = FontWeight.SemiBold,
            modifier = Modifier.weight(1f)
        )

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
                    clipboard.setText(AnnotatedString(logs.joinToString("\n") { it.text }))
                    viewModel.showToast("已复制 ${logs.size} 条抓包记录")
                } else {
                    viewModel.showToast("暂无抓包记录可复制")
                }
            }
        )
        Spacer(modifier = Modifier.width(6.dp))
        TerminalToolButton(
            label = "导出",
            active = false,
            onClick = {
                if (logs.isNotEmpty()) {
                    val sendIntent = Intent(Intent.ACTION_SEND).apply {
                        type = "text/plain"
                        putExtra(Intent.EXTRA_SUBJECT, "WiFi-Deck 802.11 Packet Logs")
                        putExtra(Intent.EXTRA_TEXT, logs.joinToString("\n") { it.text })
                    }
                    try {
                        context.startActivity(Intent.createChooser(sendIntent, "导出抓包日志"))
                    } catch (_: ActivityNotFoundException) {
                        viewModel.showToast("未找到可用于分享日志的应用")
                    }
                } else {
                    viewModel.showToast("暂无抓包日志可导出")
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
}

@Composable
private fun PacketLogLazyList(
    viewModel: MainViewModel,
    modifier: Modifier = Modifier
) {
    val logs by viewModel.logs.collectAsStateWithLifecycle()
    val autoScroll by viewModel.autoScroll.collectAsStateWithLifecycle()
    val isSniffing by viewModel.isSniffing.collectAsStateWithLifecycle()
    val listState = rememberLazyListState()
    val currentLogs by rememberUpdatedState(logs)

    // 高频流节流 + 非动画 scrollToItem，避免每帧取消重启平滑动画造成掉帧
    LaunchedEffect(autoScroll) {
        if (!autoScroll) return@LaunchedEffect
        var lastScrolledId = -1L
        var lastScrollAtMs = 0L
        snapshotFlow { currentLogs.lastOrNull()?.id }
            .collect { targetId ->
                if (targetId == null || targetId == lastScrolledId) return@collect
                val now = SystemClock.elapsedRealtime()
                val remaining = AUTO_SCROLL_MIN_INTERVAL_MS - (now - lastScrollAtMs)
                if (remaining > 0L) {
                    delay(remaining)
                }
                val latest = currentLogs
                val latestId = latest.lastOrNull()?.id
                if (latestId != null && latestId != lastScrolledId && latest.isNotEmpty()) {
                    listState.scrollToItem(latest.lastIndex)
                    lastScrolledId = latestId
                    lastScrollAtMs = SystemClock.elapsedRealtime()
                }
            }
    }

    LazyColumn(
        state = listState,
        modifier = modifier.padding(horizontal = 8.dp, vertical = 6.dp),
        verticalArrangement = Arrangement.spacedBy(4.dp)
    ) {
        items(
            items = logs,
            key = { it.id },
            contentType = { "packet_log" }
        ) { entry ->
            PacketLogRow(entry = entry)
        }

        if (logs.isEmpty()) {
            item(key = "sniff_empty", contentType = "empty") {
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(vertical = 64.dp),
                    horizontalAlignment = Alignment.CenterHorizontally
                ) {
                    Icon(
                        imageVector = Icons.Default.Terminal,
                        contentDescription = null,
                        tint = DeckColors.TextMuted,
                        modifier = Modifier.size(28.dp)
                    )
                    Spacer(modifier = Modifier.height(8.dp))
                    Text(
                        text = if (isSniffing) "正在监听空中 802.11 帧..." else "混杂模式嗅探未开启",
                        color = DeckColors.TextSecondary,
                        fontSize = 13.sp,
                        fontFamily = FontFamily.Monospace
                    )
                    Spacer(modifier = Modifier.height(4.dp))
                    Text(
                        text = "点击上方「抓包」或协议标签（如 EAPOL / Deauth）即可实时捕获",
                        color = DeckColors.TextMuted,
                        fontSize = 11.sp
                    )
                }
            }
        }
    }
}

@Composable
private fun TerminalToolButton(label: String, active: Boolean, onClick: () -> Unit) {
    Box(
        modifier = Modifier
            .clip(RoundedCornerShape(6.dp))
            .background(if (active) DeckColors.CyanDim else DeckColors.BgElevated)
            .border(
                0.5.dp,
                if (active) DeckColors.Cyan else DeckColors.BorderSubtle,
                RoundedCornerShape(6.dp)
            )
            .clickable { onClick() }
            .padding(horizontal = 7.dp, vertical = 3.dp)
    ) {
        Text(
            text = label,
            color = if (active) DeckColors.Cyan else DeckColors.TextSecondary,
            fontSize = 10.sp,
            fontWeight = FontWeight.Medium
        )
    }
}

@Composable
private fun PacketLogRow(entry: SniffLogEntry) {
    val style = entry.category.style()

    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(style.rowBg, RoundedCornerShape(4.dp))
            .padding(horizontal = 4.dp, vertical = 3.dp),
        verticalAlignment = Alignment.Top
    ) {
        Text(
            text = entry.formattedSeq,
            color = DeckColors.TextMuted,
            fontSize = 10.sp,
            fontFamily = FontFamily.Monospace,
            maxLines = 1,
            modifier = Modifier
                .widthIn(min = 28.dp)
                .padding(end = 6.dp)
        )
        Text(
            text = entry.category.tagLabel,
            color = style.tagColor,
            fontSize = 9.sp,
            fontFamily = FontFamily.Monospace,
            fontWeight = FontWeight.Bold,
            modifier = Modifier
                .background(style.tagBg, RoundedCornerShape(3.dp))
                .padding(horizontal = 4.dp, vertical = 1.dp)
        )
        Spacer(modifier = Modifier.width(6.dp))
        Text(
            text = entry.displayText,
            color = style.textColor,
            fontSize = 11.sp,
            fontFamily = FontFamily.Monospace,
            lineHeight = 15.sp
        )
    }
}
