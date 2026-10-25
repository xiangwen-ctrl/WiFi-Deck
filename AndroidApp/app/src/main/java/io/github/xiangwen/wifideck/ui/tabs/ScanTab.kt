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
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowForward
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Radar
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.Wifi
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import io.github.xiangwen.wifideck.data.ApItem
import io.github.xiangwen.wifideck.ui.BandFilter
import io.github.xiangwen.wifideck.ui.MainViewModel
import io.github.xiangwen.wifideck.ui.SortMode
import io.github.xiangwen.wifideck.ui.components.DeckFilterChip
import io.github.xiangwen.wifideck.ui.theme.DeckColors

@Composable
fun ScanTab(
    viewModel: MainViewModel,
    apList: List<ApItem>,
    scanning: Boolean,
    selected: Set<Int>,
    bandFilter: BandFilter,
    sortMode: SortMode,
    searchQuery: String,
    onNavigateToControl: () -> Unit
) {
    val count24 = remember(apList) { apList.count { it.channel < 36 } }
    val count5 = remember(apList) { apList.count { it.channel >= 36 } }

    val filtered = remember(apList, bandFilter, sortMode, searchQuery) {
        val q = searchQuery.trim().lowercase()
        apList
            .filter { ap ->
                val bandMatch = when (bandFilter) {
                    BandFilter.ALL -> true
                    BandFilter.G24 -> ap.channel < 36
                    BandFilter.G5 -> ap.channel >= 36
                }
                val queryMatch = q.isEmpty() ||
                    ap.ssid.lowercase().contains(q) ||
                    ap.bssid.lowercase().contains(q)
                bandMatch && queryMatch
            }
            .sortedWith(
                when (sortMode) {
                    SortMode.RSSI -> compareByDescending { it.rssi }
                    SortMode.CHANNEL -> compareBy { it.channel }
                }
            )
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(horizontal = 12.dp, vertical = 10.dp)
    ) {
        // 顶部搜索框 + 扫描按钮
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            // 搜索输入框
            Row(
                modifier = Modifier
                    .weight(1f)
                    .height(40.dp)
                    .background(DeckColors.BgSurface, RoundedCornerShape(8.dp))
                    .border(1.dp, DeckColors.BorderSubtle, RoundedCornerShape(8.dp))
                    .padding(horizontal = 10.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Icon(
                    imageVector = Icons.Default.Search,
                    contentDescription = null,
                    tint = DeckColors.TextMuted,
                    modifier = Modifier.size(16.dp)
                )
                Spacer(modifier = Modifier.width(6.dp))
                Box(modifier = Modifier.weight(1f), contentAlignment = Alignment.CenterStart) {
                    if (searchQuery.isEmpty()) {
                        Text("搜索 SSID / BSSID...", color = DeckColors.TextMuted, fontSize = 12.sp)
                    }
                    BasicTextField(
                        value = searchQuery,
                        onValueChange = { viewModel.setSearchQuery(it) },
                        singleLine = true,
                        textStyle = TextStyle(color = DeckColors.TextPrimary, fontSize = 12.sp),
                        cursorBrush = SolidColor(DeckColors.Crimson),
                        modifier = Modifier.fillMaxWidth()
                    )
                }
                if (searchQuery.isNotEmpty()) {
                    Icon(
                        imageVector = Icons.Default.Close,
                        contentDescription = "清除",
                        tint = DeckColors.TextSecondary,
                        modifier = Modifier
                            .size(16.dp)
                            .clickable { viewModel.setSearchQuery("") }
                    )
                }
            }

            // 扫描双频按钮
            Button(
                onClick = { viewModel.sendScan() },
                enabled = !scanning,
                shape = RoundedCornerShape(8.dp),
                colors = ButtonDefaults.buttonColors(containerColor = DeckColors.Blue),
                contentPadding = PaddingValues(horizontal = 14.dp, vertical = 0.dp),
                modifier = Modifier.height(40.dp)
            ) {
                if (scanning) {
                    CircularProgressIndicator(
                        modifier = Modifier.size(15.dp),
                        color = Color.White,
                        strokeWidth = 2.dp
                    )
                    Spacer(modifier = Modifier.width(6.dp))
                    Text("扫描中", fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
                } else {
                    Icon(
                        imageVector = Icons.Default.Radar,
                        contentDescription = null,
                        modifier = Modifier.size(16.dp)
                    )
                    Spacer(modifier = Modifier.width(5.dp))
                    Text("扫描双频", fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
                }
            }
        }

        Spacer(modifier = Modifier.height(8.dp))

        // 筛选、排序与全选操作条
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .horizontalScroll(rememberScrollState()),
            horizontalArrangement = Arrangement.spacedBy(6.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            DeckFilterChip(
                label = "全部 (${apList.size})",
                selected = bandFilter == BandFilter.ALL,
                accentColor = DeckColors.Cyan
            ) { viewModel.setBandFilter(BandFilter.ALL) }

            DeckFilterChip(
                label = "2.4G ($count24)",
                selected = bandFilter == BandFilter.G24,
                accentColor = DeckColors.Blue
            ) { viewModel.setBandFilter(BandFilter.G24) }

            DeckFilterChip(
                label = "5G ($count5)",
                selected = bandFilter == BandFilter.G5,
                accentColor = DeckColors.Purple
            ) { viewModel.setBandFilter(BandFilter.G5) }

            DeckFilterChip(
                label = "信号↓",
                selected = sortMode == SortMode.RSSI,
                accentColor = DeckColors.Amber
            ) { viewModel.setSortMode(SortMode.RSSI) }

            DeckFilterChip(
                label = "信道↑",
                selected = sortMode == SortMode.CHANNEL,
                accentColor = DeckColors.Amber
            ) { viewModel.setSortMode(SortMode.CHANNEL) }

            DeckFilterChip(
                label = "全选",
                selected = false,
                accentColor = DeckColors.Crimson
            ) { viewModel.selectAll() }

            if (selected.isNotEmpty()) {
                DeckFilterChip(
                    label = "清空",
                    selected = false,
                    accentColor = DeckColors.TextSecondary
                ) { viewModel.clearSelection() }
            }
        }

        // 已选目标快捷跳转横幅
        if (selected.isNotEmpty()) {
            Spacer(modifier = Modifier.height(8.dp))
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(DeckColors.CrimsonDim, RoundedCornerShape(8.dp))
                    .border(1.dp, DeckColors.Crimson.copy(alpha = 0.6f), RoundedCornerShape(8.dp))
                    .clickable { onNavigateToControl() }
                    .padding(horizontal = 12.dp, vertical = 8.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    text = "已锁定 ${selected.size} 个目标热点",
                    color = DeckColors.TextPrimary,
                    fontSize = 12.sp,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier.weight(1f)
                )
                Text(
                    text = "前往控制页操作",
                    color = DeckColors.Crimson,
                    fontSize = 12.sp,
                    fontWeight = FontWeight.Bold
                )
                Spacer(modifier = Modifier.width(4.dp))
                Icon(
                    imageVector = Icons.AutoMirrored.Filled.ArrowForward,
                    contentDescription = null,
                    tint = DeckColors.Crimson,
                    modifier = Modifier.size(14.dp)
                )
            }
        }

        Spacer(modifier = Modifier.height(8.dp))

        // 热点列表
        LazyColumn(
            modifier = Modifier.weight(1f),
            verticalArrangement = Arrangement.spacedBy(6.dp)
        ) {
            items(filtered, key = { it.index }) { ap ->
                ApItemCard(
                    ap = ap,
                    isSelected = selected.contains(ap.index),
                    onClick = { viewModel.toggleSelect(ap.index) }
                )
            }

            if (filtered.isEmpty() && !scanning) {
                item {
                    EmptyScanPlaceholder(
                        hasApList = apList.isNotEmpty(),
                        onScanClick = { viewModel.sendScan() }
                    )
                }
            }
        }
    }
}

@Composable
private fun ApItemCard(ap: ApItem, isSelected: Boolean, onClick: () -> Unit) {
    val is5G = ap.channel >= 36
    val bandColor = if (is5G) DeckColors.Purple else DeckColors.Blue
    val bandBg = if (is5G) DeckColors.PurpleDim else DeckColors.BlueDim
    val signalColor = DeckColors.rssiColor(ap.rssi)
    val signalProgress = DeckColors.rssiProgress(ap.rssi)
    val displaySsid = ap.ssid.ifBlank { "(隐藏 SSID)" }

    Card(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(10.dp))
            .clickable { onClick() },
        colors = CardDefaults.cardColors(
            containerColor = if (isSelected) DeckColors.CrimsonDim else DeckColors.BgSurface
        ),
        shape = RoundedCornerShape(10.dp),
        border = BorderStroke(
            width = 1.dp,
            color = if (isSelected) DeckColors.Crimson else DeckColors.BorderSubtle
        )
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 12.dp, vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            // 左侧：信号强度数值徽章
            Column(
                modifier = Modifier
                    .width(52.dp)
                    .background(DeckColors.BgPrimary.copy(alpha = 0.6f), RoundedCornerShape(8.dp))
                    .padding(vertical = 6.dp),
                horizontalAlignment = Alignment.CenterHorizontally
            ) {
                Text(
                    text = "${ap.rssi}",
                    color = signalColor,
                    fontSize = 14.sp,
                    fontWeight = FontWeight.Bold,
                    fontFamily = FontFamily.Monospace
                )
                Text(
                    text = "dBm",
                    color = DeckColors.TextMuted,
                    fontSize = 9.sp,
                    fontFamily = FontFamily.Monospace
                )
            }

            Spacer(modifier = Modifier.width(10.dp))

            // 中间：SSID + 频段/信道标签 + BSSID + 信号条
            Column(modifier = Modifier.weight(1f)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        text = displaySsid,
                        color = if (ap.ssid.isBlank()) DeckColors.TextSecondary else DeckColors.TextPrimary,
                        fontSize = 14.sp,
                        fontWeight = FontWeight.SemiBold,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        modifier = Modifier.weight(1f, fill = false)
                    )
                    Spacer(modifier = Modifier.width(6.dp))
                    Text(
                        text = ap.band,
                        color = bandColor,
                        fontSize = 10.sp,
                        fontWeight = FontWeight.Bold,
                        modifier = Modifier
                            .background(bandBg, RoundedCornerShape(4.dp))
                            .border(0.5.dp, bandColor.copy(alpha = 0.5f), RoundedCornerShape(4.dp))
                            .padding(horizontal = 5.dp, vertical = 1.dp)
                    )
                    Spacer(modifier = Modifier.width(4.dp))
                    Text(
                        text = "CH ${ap.channel}",
                        color = DeckColors.TextSecondary,
                        fontSize = 10.sp,
                        fontFamily = FontFamily.Monospace,
                        modifier = Modifier
                            .background(DeckColors.BgElevated, RoundedCornerShape(4.dp))
                            .padding(horizontal = 5.dp, vertical = 1.dp)
                    )
                }

                Spacer(modifier = Modifier.height(4.dp))

                Text(
                    text = "#${ap.index} · ${ap.bssid}",
                    color = DeckColors.TextMuted,
                    fontSize = 11.sp,
                    fontFamily = FontFamily.Monospace
                )

                Spacer(modifier = Modifier.height(5.dp))

                // 信号质量条
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(4.dp)
                        .clip(RoundedCornerShape(2.dp))
                        .background(DeckColors.BgElevated)
                ) {
                    Box(
                        modifier = Modifier
                            .fillMaxWidth(signalProgress)
                            .fillMaxHeight()
                            .background(signalColor, RoundedCornerShape(2.dp))
                    )
                }
            }

            Spacer(modifier = Modifier.width(10.dp))

            // 右侧：勾选框
            Box(
                modifier = Modifier
                    .size(24.dp)
                    .background(
                        if (isSelected) DeckColors.Crimson else DeckColors.BgElevated,
                        RoundedCornerShape(6.dp)
                    )
                    .border(
                        1.dp,
                        if (isSelected) DeckColors.Crimson else DeckColors.BorderSubtle,
                        RoundedCornerShape(6.dp)
                    ),
                contentAlignment = Alignment.Center
            ) {
                if (isSelected) {
                    Icon(
                        imageVector = Icons.Default.Check,
                        contentDescription = null,
                        tint = Color.White,
                        modifier = Modifier.size(15.dp)
                    )
                }
            }
        }
    }
}

@Composable
private fun EmptyScanPlaceholder(hasApList: Boolean, onScanClick: () -> Unit) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 48.dp),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Icon(
            imageVector = Icons.Default.Wifi,
            contentDescription = null,
            tint = DeckColors.TextMuted,
            modifier = Modifier.size(42.dp)
        )
        Spacer(modifier = Modifier.height(10.dp))
        Text(
            text = if (hasApList) "没有匹配当前筛选条件的热点" else "暂无扫描结果",
            color = DeckColors.TextSecondary,
            fontSize = 14.sp,
            fontWeight = FontWeight.Medium
        )
        Spacer(modifier = Modifier.height(4.dp))
        Text(
            text = if (hasApList) "请尝试切换频段标签或清空搜索词" else "点击右上角「扫描双频」探测 2.4GHz 与 5GHz 网络",
            color = DeckColors.TextMuted,
            fontSize = 12.sp
        )
        if (!hasApList) {
            Spacer(modifier = Modifier.height(14.dp))
            Button(
                onClick = onScanClick,
                colors = ButtonDefaults.buttonColors(containerColor = DeckColors.Blue),
                shape = RoundedCornerShape(8.dp)
            ) {
                Icon(Icons.Default.Radar, contentDescription = null, modifier = Modifier.size(16.dp))
                Spacer(modifier = Modifier.width(6.dp))
                Text("立即扫描")
            }
        }
    }
}
