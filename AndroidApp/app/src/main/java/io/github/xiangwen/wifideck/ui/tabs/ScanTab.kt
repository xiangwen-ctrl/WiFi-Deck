package io.github.xiangwen.wifideck.ui.tabs

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
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
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowForward
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Radar
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.SwapVert
import androidx.compose.material.icons.filled.Wifi
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
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
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.github.xiangwen.wifideck.data.ApItem
import io.github.xiangwen.wifideck.ui.BandFilter
import io.github.xiangwen.wifideck.ui.MainViewModel
import io.github.xiangwen.wifideck.ui.SortMode
import io.github.xiangwen.wifideck.ui.components.SignalStrengthBars
import io.github.xiangwen.wifideck.ui.theme.DeckColors

@Composable
fun ScanTab(
    viewModel: MainViewModel,
    onNavigateToControl: () -> Unit
) {
    Box(modifier = Modifier.fillMaxSize()) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(horizontal = 14.dp, vertical = 10.dp)
        ) {
            // 1. 顶部搜索框 + 扫描双频按钮
            ScanSearchAndActionRow(viewModel = viewModel)

            Spacer(modifier = Modifier.height(10.dp))

            // 2. 分段频段筛选器 + 右侧排序/全选工具条
            ScanFilterAndSortBar(viewModel = viewModel)

            Spacer(modifier = Modifier.height(10.dp))

            // 3. 热点列表
            ScanApLazyList(
                viewModel = viewModel,
                modifier = Modifier.weight(1f)
            )
        }

        // 4. 底部悬浮目标锁定操作栏（选中目标时从底部平滑浮出，不挤压列表跳动）
        ScanFloatingSelectionBar(
            viewModel = viewModel,
            onNavigateToControl = onNavigateToControl,
            modifier = Modifier.align(Alignment.BottomCenter)
        )
    }
}

@Composable
private fun ScanSearchAndActionRow(viewModel: MainViewModel) {
    val searchQuery by viewModel.searchQuery.collectAsStateWithLifecycle()
    val scanning by viewModel.scanning.collectAsStateWithLifecycle()

    Row(
        modifier = Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        Row(
            modifier = Modifier
                .weight(1f)
                .height(42.dp)
                .background(DeckColors.BgSurface, RoundedCornerShape(10.dp))
                .border(1.dp, DeckColors.BorderSubtle, RoundedCornerShape(10.dp))
                .padding(horizontal = 12.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Icon(
                imageVector = Icons.Default.Search,
                contentDescription = null,
                tint = DeckColors.TextMuted,
                modifier = Modifier.size(16.dp)
            )
            Spacer(modifier = Modifier.width(8.dp))
            Box(modifier = Modifier.weight(1f), contentAlignment = Alignment.CenterStart) {
                if (searchQuery.isEmpty()) {
                    Text(
                        text = "搜索 SSID / BSSID...",
                        color = DeckColors.TextMuted,
                        fontSize = 13.sp
                    )
                }
                BasicTextField(
                    value = searchQuery,
                    onValueChange = { raw ->
                        viewModel.setSearchQuery(raw.replace("\r", "").replace("\n", ""))
                    },
                    singleLine = true,
                    textStyle = TextStyle(color = DeckColors.TextPrimary, fontSize = 13.sp),
                    cursorBrush = SolidColor(DeckColors.Cyan),
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

        Button(
            onClick = { viewModel.sendScan() },
            enabled = !scanning,
            shape = RoundedCornerShape(10.dp),
            colors = ButtonDefaults.buttonColors(
                containerColor = DeckColors.Blue,
                disabledContainerColor = DeckColors.BlueDim
            ),
            contentPadding = PaddingValues(horizontal = 14.dp, vertical = 0.dp),
            modifier = Modifier.height(42.dp)
        ) {
            if (scanning) {
                CircularProgressIndicator(
                    modifier = Modifier.size(15.dp),
                    color = DeckColors.Cyan,
                    strokeWidth = 2.dp
                )
                Spacer(modifier = Modifier.width(6.dp))
                Text(
                    text = "扫描中",
                    fontSize = 13.sp,
                    fontWeight = FontWeight.SemiBold,
                    color = DeckColors.TextPrimary
                )
            } else {
                Icon(
                    imageVector = Icons.Default.Radar,
                    contentDescription = null,
                    modifier = Modifier.size(16.dp),
                    tint = Color.White
                )
                Spacer(modifier = Modifier.width(6.dp))
                Text(
                    text = "扫描双频",
                    fontSize = 13.sp,
                    fontWeight = FontWeight.SemiBold,
                    color = Color.White
                )
            }
        }
    }
}

@Composable
private fun ScanFilterAndSortBar(viewModel: MainViewModel) {
    val scanListState by viewModel.scanListState.collectAsStateWithLifecycle()
    val bandFilter by viewModel.bandFilter.collectAsStateWithLifecycle()
    val sortMode by viewModel.sortMode.collectAsStateWithLifecycle()
    val selected by viewModel.selected.collectAsStateWithLifecycle()

    Row(
        modifier = Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.SpaceBetween
    ) {
        // 左侧：统一分段频段切换器
        Row(
            modifier = Modifier
                .background(DeckColors.BgSurface, RoundedCornerShape(8.dp))
                .border(1.dp, DeckColors.BorderSubtle, RoundedCornerShape(8.dp))
                .padding(2.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            BandSegmentItem(
                label = "全部 ${scanListState.totalCount}",
                selected = bandFilter == BandFilter.ALL,
                activeColor = DeckColors.Cyan
            ) { viewModel.setBandFilter(BandFilter.ALL) }

            BandSegmentItem(
                label = "2.4G ${scanListState.count24G}",
                selected = bandFilter == BandFilter.G24,
                activeColor = DeckColors.Blue
            ) { viewModel.setBandFilter(BandFilter.G24) }

            BandSegmentItem(
                label = "5G ${scanListState.count5G}",
                selected = bandFilter == BandFilter.G5,
                activeColor = DeckColors.Purple
            ) { viewModel.setBandFilter(BandFilter.G5) }
        }

        // 右侧：排序切换 + 全选/清空
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(6.dp)
        ) {
            Row(
                modifier = Modifier
                    .clip(RoundedCornerShape(8.dp))
                    .background(DeckColors.BgSurface)
                    .border(1.dp, DeckColors.BorderSubtle, RoundedCornerShape(8.dp))
                    .clickable {
                        val next = if (sortMode == SortMode.RSSI) SortMode.CHANNEL else SortMode.RSSI
                        viewModel.setSortMode(next)
                    }
                    .padding(horizontal = 8.dp, vertical = 6.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Icon(
                    imageVector = Icons.Default.SwapVert,
                    contentDescription = null,
                    tint = DeckColors.Amber,
                    modifier = Modifier.size(13.dp)
                )
                Spacer(modifier = Modifier.width(3.dp))
                Text(
                    text = if (sortMode == SortMode.RSSI) "信号↓" else "信道↑",
                    color = DeckColors.TextSecondary,
                    fontSize = 11.sp,
                    fontWeight = FontWeight.Medium
                )
            }

            val allSelected = scanListState.filteredAps.isNotEmpty() &&
                scanListState.filteredAps.all { selected.contains(it.index) }
            Box(
                modifier = Modifier
                    .clip(RoundedCornerShape(8.dp))
                    .background(DeckColors.BgSurface)
                    .border(1.dp, DeckColors.BorderSubtle, RoundedCornerShape(8.dp))
                    .clickable {
                        if (allSelected || selected.isNotEmpty()) {
                            viewModel.clearSelection()
                        } else {
                            viewModel.selectAll()
                        }
                    }
                    .padding(horizontal = 9.dp, vertical = 6.dp),
                contentAlignment = Alignment.Center
            ) {
                Text(
                    text = if (selected.isNotEmpty()) "清空" else "全选",
                    color = if (selected.isNotEmpty()) DeckColors.Crimson else DeckColors.TextSecondary,
                    fontSize = 11.sp,
                    fontWeight = FontWeight.Medium
                )
            }
        }
    }
}

@Composable
private fun ScanApLazyList(
    viewModel: MainViewModel,
    modifier: Modifier = Modifier
) {
    val scanListState by viewModel.scanListState.collectAsStateWithLifecycle()
    val selected by viewModel.selected.collectAsStateWithLifecycle()
    val scanning by viewModel.scanning.collectAsStateWithLifecycle()
    val listState = rememberLazyListState()
    val onToggle: (Int) -> Unit = remember(viewModel) {
        { idx -> viewModel.toggleSelect(idx) }
    }

    LazyColumn(
        state = listState,
        modifier = modifier,
        verticalArrangement = Arrangement.spacedBy(8.dp),
        contentPadding = PaddingValues(bottom = if (selected.isNotEmpty()) 76.dp else 12.dp)
    ) {
        items(
            items = scanListState.filteredAps,
            key = { it.index },
            contentType = { "ap_item" }
        ) { ap ->
            ApItemCard(
                ap = ap,
                isSelected = selected.contains(ap.index),
                onToggle = onToggle
            )
        }

        if (scanListState.filteredAps.isEmpty() && !scanning) {
            item(key = "empty_placeholder", contentType = "empty") {
                EmptyScanPlaceholder(
                    hasApList = scanListState.totalCount > 0,
                    onScanClick = { viewModel.sendScan() }
                )
            }
        }
    }
}

@Composable
private fun ScanFloatingSelectionBar(
    viewModel: MainViewModel,
    onNavigateToControl: () -> Unit,
    modifier: Modifier = Modifier
) {
    val selectedCount by viewModel.selectedCount.collectAsStateWithLifecycle()

    AnimatedVisibility(
        visible = selectedCount > 0,
        enter = slideInVertically(initialOffsetY = { it }) + fadeIn(),
        exit = slideOutVertically(targetOffsetY = { it }) + fadeOut(),
        modifier = modifier
            .fillMaxWidth()
            .padding(horizontal = 14.dp, vertical = 12.dp)
    ) {
        Surface(
            shape = RoundedCornerShape(12.dp),
            color = DeckColors.BgElevated,
            border = BorderStroke(1.dp, DeckColors.Crimson.copy(alpha = 0.65f)),
            shadowElevation = 8.dp
        ) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .clickable { onNavigateToControl() }
                    .padding(horizontal = 14.dp, vertical = 11.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Box(
                    modifier = Modifier
                        .background(DeckColors.Crimson, RoundedCornerShape(6.dp))
                        .padding(horizontal = 7.dp, vertical = 2.dp)
                ) {
                    Text(
                        text = "$selectedCount",
                        color = Color.White,
                        fontSize = 12.sp,
                        fontFamily = FontFamily.Monospace,
                        fontWeight = FontWeight.Bold
                    )
                }
                Spacer(modifier = Modifier.width(10.dp))
                Text(
                    text = "个目标热点已锁定",
                    color = DeckColors.TextPrimary,
                    fontSize = 13.sp,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier.weight(1f)
                )
                Text(
                    text = "前往控制台",
                    color = DeckColors.Crimson,
                    fontSize = 13.sp,
                    fontWeight = FontWeight.Bold
                )
                Spacer(modifier = Modifier.width(4.dp))
                Icon(
                    imageVector = Icons.AutoMirrored.Filled.ArrowForward,
                    contentDescription = null,
                    tint = DeckColors.Crimson,
                    modifier = Modifier.size(16.dp)
                )
            }
        }
    }
}

@Composable
private fun BandSegmentItem(
    label: String,
    selected: Boolean,
    activeColor: Color,
    onClick: () -> Unit
) {
    Box(
        modifier = Modifier
            .clip(RoundedCornerShape(6.dp))
            .background(if (selected) activeColor.copy(alpha = 0.18f) else Color.Transparent)
            .clickable { onClick() }
            .padding(horizontal = 9.dp, vertical = 4.dp),
        contentAlignment = Alignment.Center
    ) {
        Text(
            text = label,
            color = if (selected) activeColor else DeckColors.TextSecondary,
            fontSize = 11.sp,
            fontWeight = if (selected) FontWeight.Bold else FontWeight.Medium
        )
    }
}

@Composable
private fun ApItemCard(ap: ApItem, isSelected: Boolean, onToggle: (Int) -> Unit) {
    val is5G = ap.channel >= 36
    val bandColor = if (is5G) DeckColors.Purple else DeckColors.Blue
    val bandBg = if (is5G) DeckColors.PurpleDim else DeckColors.BlueDim
    val signalColor = DeckColors.rssiColor(ap.rssi)
    val displaySsid = ap.ssid.ifBlank { "(隐藏网络)" }

    Card(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(12.dp))
            .clickable { onToggle(ap.index) },
        colors = CardDefaults.cardColors(
            containerColor = if (isSelected) DeckColors.CrimsonDim else DeckColors.BgSurface
        ),
        shape = RoundedCornerShape(12.dp),
        border = BorderStroke(
            width = 1.dp,
            color = if (isSelected) DeckColors.Crimson.copy(alpha = 0.75f) else DeckColors.BorderSubtle
        )
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 12.dp, vertical = 11.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            // 左侧：4阶硬件信号柱 + dBm 读数
            Column(
                modifier = Modifier
                    .width(48.dp)
                    .background(DeckColors.BgTerminal, RoundedCornerShape(8.dp))
                    .border(0.5.dp, DeckColors.BorderSubtle, RoundedCornerShape(8.dp))
                    .padding(vertical = 6.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.Center
            ) {
                SignalStrengthBars(rssi = ap.rssi)
                Spacer(modifier = Modifier.height(3.dp))
                Text(
                    text = "${ap.rssi}",
                    color = signalColor,
                    fontSize = 11.sp,
                    fontWeight = FontWeight.Bold,
                    fontFamily = FontFamily.Monospace
                )
            }

            Spacer(modifier = Modifier.width(12.dp))

            // 中间：SSID + BSSID + 频段/信道胶囊
            Column(modifier = Modifier.weight(1f)) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text(
                        text = displaySsid,
                        color = if (ap.ssid.isBlank()) DeckColors.TextSecondary else DeckColors.TextPrimary,
                        fontSize = 15.sp,
                        fontWeight = FontWeight.SemiBold,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        modifier = Modifier.weight(1f)
                    )
                    Spacer(modifier = Modifier.width(8.dp))
                    Text(
                        text = ap.band,
                        color = bandColor,
                        fontSize = 10.sp,
                        fontWeight = FontWeight.SemiBold,
                        modifier = Modifier
                            .background(bandBg, RoundedCornerShape(4.dp))
                            .padding(horizontal = 5.dp, vertical = 2.dp)
                    )
                    Spacer(modifier = Modifier.width(4.dp))
                    Text(
                        text = "CH ${ap.channel}",
                        color = DeckColors.TextSecondary,
                        fontSize = 10.sp,
                        fontFamily = FontFamily.Monospace,
                        fontWeight = FontWeight.Medium,
                        modifier = Modifier
                            .background(DeckColors.BgElevated, RoundedCornerShape(4.dp))
                            .padding(horizontal = 5.dp, vertical = 2.dp)
                    )
                }

                Spacer(modifier = Modifier.height(4.dp))

                Text(
                    text = "#${ap.index}  ${ap.bssid}",
                    color = DeckColors.TextMuted,
                    fontSize = 11.sp,
                    fontFamily = FontFamily.Monospace
                )
            }

            Spacer(modifier = Modifier.width(10.dp))

            // 右侧：精致勾选框
            Box(
                modifier = Modifier
                    .size(22.dp)
                    .background(
                        if (isSelected) DeckColors.Crimson else DeckColors.BgElevated,
                        RoundedCornerShape(6.dp)
                    )
                    .border(
                        1.dp,
                        if (isSelected) DeckColors.Crimson else DeckColors.BorderStrong,
                        RoundedCornerShape(6.dp)
                    ),
                contentAlignment = Alignment.Center
            ) {
                if (isSelected) {
                    Icon(
                        imageVector = Icons.Default.Check,
                        contentDescription = null,
                        tint = Color.White,
                        modifier = Modifier.size(14.dp)
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
            .padding(vertical = 56.dp),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Box(
            modifier = Modifier
                .size(56.dp)
                .background(DeckColors.BgSurface, RoundedCornerShape(16.dp))
                .border(1.dp, DeckColors.BorderSubtle, RoundedCornerShape(16.dp)),
            contentAlignment = Alignment.Center
        ) {
            Icon(
                imageVector = Icons.Default.Wifi,
                contentDescription = null,
                tint = DeckColors.TextMuted,
                modifier = Modifier.size(26.dp)
            )
        }
        Spacer(modifier = Modifier.height(12.dp))
        Text(
            text = if (hasApList) "没有匹配筛选条件的热点" else "暂无扫描结果",
            color = DeckColors.TextSecondary,
            fontSize = 14.sp,
            fontWeight = FontWeight.SemiBold
        )
        Spacer(modifier = Modifier.height(4.dp))
        Text(
            text = if (hasApList) "请尝试切换频段标签或清空搜索词" else "点击右上角「扫描双频」探测 2.4GHz 与 5GHz 网络",
            color = DeckColors.TextMuted,
            fontSize = 12.sp
        )
        if (!hasApList) {
            Spacer(modifier = Modifier.height(16.dp))
            Button(
                onClick = onScanClick,
                colors = ButtonDefaults.buttonColors(containerColor = DeckColors.Blue),
                shape = RoundedCornerShape(10.dp),
                contentPadding = PaddingValues(horizontal = 18.dp, vertical = 10.dp)
            ) {
                Icon(Icons.Default.Radar, contentDescription = null, modifier = Modifier.size(16.dp))
                Spacer(modifier = Modifier.width(6.dp))
                Text("立即扫描双频", fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
            }
        }
    }
}
