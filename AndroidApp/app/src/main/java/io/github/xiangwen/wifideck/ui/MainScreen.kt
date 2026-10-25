package io.github.xiangwen.wifideck.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Radar
import androidx.compose.material.icons.filled.Security
import androidx.compose.material.icons.filled.Terminal
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material3.Badge
import androidx.compose.material3.BadgedBox
import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.NavigationBarItemDefaults
import androidx.compose.material3.Scaffold
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
import io.github.xiangwen.wifideck.data.BleManager
import io.github.xiangwen.wifideck.ui.components.TopStatusHeader
import io.github.xiangwen.wifideck.ui.tabs.ControlTab
import io.github.xiangwen.wifideck.ui.tabs.ScanTab
import io.github.xiangwen.wifideck.ui.tabs.SettingsTab
import io.github.xiangwen.wifideck.ui.tabs.SniffTab
import io.github.xiangwen.wifideck.ui.theme.DeckColors
import kotlinx.coroutines.delay

@Composable
fun MainScreen(viewModel: MainViewModel) {
    val connectionState by viewModel.connectionState.collectAsState()
    val apList by viewModel.apList.collectAsState()
    val status by viewModel.status.collectAsState()
    val config by viewModel.config.collectAsState()
    val logs by viewModel.logs.collectAsState()
    val scanning by viewModel.scanning.collectAsState()
    val selected by viewModel.selected.collectAsState()
    val bandFilter by viewModel.bandFilter.collectAsState()
    val sortMode by viewModel.sortMode.collectAsState()
    val searchQuery by viewModel.searchQuery.collectAsState()
    val autoScroll by viewModel.autoScroll.collectAsState()
    val toast by viewModel.toast.collectAsState()

    var tabIndex by remember { mutableIntStateOf(0) }

    LaunchedEffect(toast) {
        if (toast != null) {
            delay(2200)
            viewModel.clearToast()
        }
    }

    Scaffold(
        containerColor = DeckColors.BgPrimary,
        topBar = {
            TopStatusHeader(
                state = connectionState,
                status = status,
                config = config,
                apCount = apList.size,
                selectedCount = selected.size,
                onConnect = { BleManager.startScan() },
                onStopAll = { viewModel.sendStopAll() }
            )
        },
        bottomBar = {
            NavigationBar(
                containerColor = DeckColors.BgSurface,
                tonalElevation = 0.dp
            ) {
                val itemColors = NavigationBarItemDefaults.colors(
                    selectedIconColor = DeckColors.Crimson,
                    selectedTextColor = DeckColors.Crimson,
                    indicatorColor = DeckColors.CrimsonDim,
                    unselectedIconColor = DeckColors.TextSecondary,
                    unselectedTextColor = DeckColors.TextSecondary
                )

                // Tab 0: 扫描
                NavigationBarItem(
                    selected = tabIndex == 0,
                    onClick = { tabIndex = 0 },
                    colors = itemColors,
                    icon = {
                        BadgedBox(
                            badge = {
                                if (apList.isNotEmpty()) {
                                    Badge(
                                        containerColor = DeckColors.Blue,
                                        contentColor = Color.White
                                    ) {
                                        Text("${apList.size}", fontSize = 9.sp)
                                    }
                                }
                            }
                        ) {
                            Icon(Icons.Default.Radar, contentDescription = "扫描", modifier = Modifier.size(20.dp))
                        }
                    },
                    label = { Text("雷达扫描", fontSize = 11.sp, fontWeight = FontWeight.Medium) }
                )

                // Tab 1: 控制
                NavigationBarItem(
                    selected = tabIndex == 1,
                    onClick = { tabIndex = 1 },
                    colors = itemColors,
                    icon = {
                        BadgedBox(
                            badge = {
                                if (status.attacking) {
                                    Badge(containerColor = DeckColors.BrightGreen)
                                } else if (selected.isNotEmpty()) {
                                    Badge(
                                        containerColor = DeckColors.Crimson,
                                        contentColor = Color.White
                                    ) {
                                        Text("${selected.size}", fontSize = 9.sp)
                                    }
                                }
                            }
                        ) {
                            Icon(Icons.Default.Security, contentDescription = "控制", modifier = Modifier.size(20.dp))
                        }
                    },
                    label = { Text("战术控制", fontSize = 11.sp, fontWeight = FontWeight.Medium) }
                )

                // Tab 2: 抓包
                NavigationBarItem(
                    selected = tabIndex == 2,
                    onClick = { tabIndex = 2 },
                    colors = itemColors,
                    icon = {
                        BadgedBox(
                            badge = {
                                if (status.sniffing) {
                                    Badge(containerColor = DeckColors.Purple)
                                }
                            }
                        ) {
                            Icon(Icons.Default.Terminal, contentDescription = "抓包", modifier = Modifier.size(20.dp))
                        }
                    },
                    label = { Text("抓包终端", fontSize = 11.sp, fontWeight = FontWeight.Medium) }
                )

                // Tab 3: 设置
                NavigationBarItem(
                    selected = tabIndex == 3,
                    onClick = { tabIndex = 3 },
                    colors = itemColors,
                    icon = {
                        Icon(Icons.Default.Tune, contentDescription = "设置", modifier = Modifier.size(20.dp))
                    },
                    label = { Text("参数配置", fontSize = 11.sp, fontWeight = FontWeight.Medium) }
                )
            }
        }
    ) { innerPadding ->
        Box(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding)
                .background(DeckColors.BgPrimary)
        ) {
            Column(modifier = Modifier.fillMaxSize()) {
                when (tabIndex) {
                    0 -> ScanTab(
                        viewModel = viewModel,
                        apList = apList,
                        scanning = scanning,
                        selected = selected,
                        bandFilter = bandFilter,
                        sortMode = sortMode,
                        searchQuery = searchQuery,
                        onNavigateToControl = { tabIndex = 1 }
                    )
                    1 -> ControlTab(
                        viewModel = viewModel,
                        status = status,
                        apList = apList,
                        selected = selected,
                        onNavigateToScan = { tabIndex = 0 }
                    )
                    2 -> SniffTab(
                        viewModel = viewModel,
                        logs = logs,
                        status = status,
                        autoScroll = autoScroll
                    )
                    3 -> SettingsTab(viewModel = viewModel)
                }
            }

            // 悬浮通知胶囊 (Toast)
            val toastMsg = toast
            if (toastMsg != null) {
                Text(
                    text = toastMsg,
                    modifier = Modifier
                        .align(Alignment.BottomCenter)
                        .padding(bottom = 16.dp)
                        .background(DeckColors.BgElevated, RoundedCornerShape(20.dp))
                        .border(1.dp, DeckColors.Crimson, RoundedCornerShape(20.dp))
                        .padding(horizontal = 18.dp, vertical = 9.dp),
                    color = DeckColors.TextPrimary,
                    fontSize = 12.sp,
                    fontWeight = FontWeight.Medium
                )
            }
        }
    }
}
