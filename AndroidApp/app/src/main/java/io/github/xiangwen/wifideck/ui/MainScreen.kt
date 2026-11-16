package io.github.xiangwen.wifideck.ui

import androidx.compose.animation.core.animateDpAsState
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.consumeWindowInsets
import androidx.compose.foundation.layout.displayCutout
import androidx.compose.foundation.layout.displayCutoutPadding
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.union
import androidx.compose.foundation.layout.windowInsetsPadding
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
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.saveable.rememberSaveableStateHolder
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.github.xiangwen.wifideck.data.BleManager
import io.github.xiangwen.wifideck.ui.components.BleGatePromptDialog
import io.github.xiangwen.wifideck.ui.components.TopStatusHeader
import io.github.xiangwen.wifideck.ui.tabs.ControlTab
import io.github.xiangwen.wifideck.ui.tabs.ScanTab
import io.github.xiangwen.wifideck.ui.tabs.SettingsTab
import io.github.xiangwen.wifideck.ui.tabs.SniffTab
import io.github.xiangwen.wifideck.ui.theme.DeckColors
import kotlinx.coroutines.delay

@Composable
fun MainScreen(viewModel: MainViewModel) {
    var tabIndex by rememberSaveable { mutableIntStateOf(0) }
    val stateHolder = rememberSaveableStateHolder()

    SettingsAutoSyncEffect(tabIndex = tabIndex, viewModel = viewModel)
    BleGatePromptHost(viewModel = viewModel)

    Scaffold(
        containerColor = DeckColors.BgPrimary,
        contentWindowInsets = WindowInsets(0, 0, 0, 0),
        topBar = {
            TopStatusHeaderContainer(viewModel = viewModel)
        },
        bottomBar = {
            DeckBottomBar(
                tabIndex = tabIndex,
                onSelectTab = { tabIndex = it },
                viewModel = viewModel
            )
        }
    ) { innerPadding ->
        Box(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding)
                .consumeWindowInsets(innerPadding)
                .displayCutoutPadding()
                .imePadding()
                .background(DeckColors.BgPrimary)
        ) {
            Box(modifier = Modifier.fillMaxSize()) {
                stateHolder.SaveableStateProvider(key = tabIndex) {
                    when (tabIndex) {
                        0 -> ScanTab(
                            viewModel = viewModel,
                            onNavigateToControl = { tabIndex = 1 }
                        )
                        1 -> ControlTab(
                            viewModel = viewModel,
                            onNavigateToScan = { tabIndex = 0 }
                        )
                        2 -> SniffTab(viewModel = viewModel)
                        3 -> SettingsTab(viewModel = viewModel)
                    }
                }
            }

            ToastOverlay(
                viewModel = viewModel,
                isScanTab = (tabIndex == 0),
                modifier = Modifier.align(Alignment.BottomCenter)
            )
        }
    }
}

@Composable
private fun SettingsAutoSyncEffect(tabIndex: Int, viewModel: MainViewModel) {
    val connectionState by viewModel.connectionState.collectAsStateWithLifecycle()
    LaunchedEffect(tabIndex, connectionState) {
        if (tabIndex == 3 && connectionState == BleManager.ConnectionState.CONNECTED) {
            viewModel.refreshDeviceInfo()
        }
    }
}

@Composable
private fun BleGatePromptHost(viewModel: MainViewModel) {
    val prompt by viewModel.bleGatePrompt.collectAsStateWithLifecycle()
    prompt?.let { activePrompt ->
        BleGatePromptDialog(
            prompt = activePrompt,
            onConfirm = { viewModel.confirmBleGatePrompt() },
            onDismiss = { viewModel.dismissBleGatePrompt() }
        )
    }
}

@Composable
private fun TopStatusHeaderContainer(viewModel: MainViewModel) {
    val connectionState by viewModel.connectionState.collectAsStateWithLifecycle()
    val reconnectState by viewModel.reconnectState.collectAsStateWithLifecycle()
    val status by viewModel.status.collectAsStateWithLifecycle()
    val config by viewModel.config.collectAsStateWithLifecycle()
    val apCount by viewModel.apCount.collectAsStateWithLifecycle()
    val selectedCount by viewModel.selectedCount.collectAsStateWithLifecycle()

    TopStatusHeader(
        state = connectionState,
        reconnectState = reconnectState,
        status = status,
        config = config,
        apCount = apCount,
        selectedCount = selectedCount,
        onConnect = { viewModel.requestBleConnect() },
        onRetryReconnect = { viewModel.retryOrConnectBle() },
        onCancelConnect = { viewModel.disconnectBle() },
        onStopAll = { viewModel.sendStopAll() }
    )
}

@Composable
private fun DeckBottomBar(
    tabIndex: Int,
    onSelectTab: (Int) -> Unit,
    viewModel: MainViewModel
) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .background(DeckColors.BgSurface)
            .windowInsetsPadding(
                WindowInsets.navigationBars
                    .union(WindowInsets.displayCutout)
                    .only(WindowInsetsSides.Bottom + WindowInsetsSides.Horizontal)
            )
    ) {
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height(1.dp)
                .background(DeckColors.BorderSubtle)
        )
        NavigationBar(
            containerColor = DeckColors.BgSurface,
            tonalElevation = 0.dp,
            windowInsets = WindowInsets(0, 0, 0, 0)
        ) {
            val itemColors = NavigationBarItemDefaults.colors(
                selectedIconColor = DeckColors.TextPrimary,
                selectedTextColor = DeckColors.TextPrimary,
                indicatorColor = DeckColors.BgElevated,
                unselectedIconColor = DeckColors.TextMuted,
                unselectedTextColor = DeckColors.TextMuted
            )

            // Tab 0: 扫描
            NavigationBarItem(
                selected = tabIndex == 0,
                onClick = { onSelectTab(0) },
                colors = itemColors,
                icon = { ScanTabIconWithBadge(viewModel) },
                label = { Text("频谱扫描", fontSize = 11.sp, fontWeight = FontWeight.Medium) }
            )

            // Tab 1: 控制
            NavigationBarItem(
                selected = tabIndex == 1,
                onClick = { onSelectTab(1) },
                colors = itemColors,
                icon = { ControlTabIconWithBadge(viewModel) },
                label = { Text("射频控制台", fontSize = 11.sp, fontWeight = FontWeight.Medium) }
            )

            // Tab 2: 抓包
            NavigationBarItem(
                selected = tabIndex == 2,
                onClick = { onSelectTab(2) },
                colors = itemColors,
                icon = { SniffTabIconWithBadge(viewModel) },
                label = { Text("协议抓包", fontSize = 11.sp, fontWeight = FontWeight.Medium) }
            )

            // Tab 3: 设置
            NavigationBarItem(
                selected = tabIndex == 3,
                onClick = { onSelectTab(3) },
                colors = itemColors,
                icon = {
                    Icon(Icons.Default.Tune, contentDescription = "设置", modifier = Modifier.size(20.dp))
                },
                label = { Text("设备参数", fontSize = 11.sp, fontWeight = FontWeight.Medium) }
            )
        }
    }
}

@Composable
private fun ScanTabIconWithBadge(viewModel: MainViewModel) {
    val apCount by viewModel.apCount.collectAsStateWithLifecycle()
    BadgedBox(
        badge = {
            if (apCount > 0) {
                Badge(
                    containerColor = DeckColors.Blue,
                    contentColor = DeckColors.BgPrimary
                ) {
                    Text("$apCount", fontSize = 9.sp, fontWeight = FontWeight.Bold)
                }
            }
        }
    ) {
        Icon(Icons.Default.Radar, contentDescription = "扫描", modifier = Modifier.size(20.dp))
    }
}

@Composable
private fun ControlTabIconWithBadge(viewModel: MainViewModel) {
    val isAttacking by viewModel.isAttacking.collectAsStateWithLifecycle()
    val selectedCount by viewModel.selectedCount.collectAsStateWithLifecycle()
    BadgedBox(
        badge = {
            if (isAttacking) {
                Badge(containerColor = DeckColors.BrightGreen)
            } else if (selectedCount > 0) {
                Badge(
                    containerColor = DeckColors.Crimson,
                    contentColor = Color.White
                ) {
                    Text("$selectedCount", fontSize = 9.sp, fontWeight = FontWeight.Bold)
                }
            }
        }
    ) {
        Icon(Icons.Default.Security, contentDescription = "控制", modifier = Modifier.size(20.dp))
    }
}

@Composable
private fun SniffTabIconWithBadge(viewModel: MainViewModel) {
    val isSniffing by viewModel.isSniffing.collectAsStateWithLifecycle()
    BadgedBox(
        badge = {
            if (isSniffing) {
                Badge(containerColor = DeckColors.Purple)
            }
        }
    ) {
        Icon(Icons.Default.Terminal, contentDescription = "抓包", modifier = Modifier.size(20.dp))
    }
}

@Composable
private fun ToastOverlay(
    viewModel: MainViewModel,
    isScanTab: Boolean,
    modifier: Modifier = Modifier
) {
    val toast by viewModel.toast.collectAsStateWithLifecycle()
    val selectedCount by viewModel.selectedCount.collectAsStateWithLifecycle()

    LaunchedEffect(toast) {
        if (toast != null) {
            delay(2200)
            viewModel.clearToast()
        }
    }

    val toastMsg = toast ?: return
    val bottomOffset by animateDpAsState(
        targetValue = if (isScanTab && selectedCount > 0) 84.dp else 20.dp,
        label = "toastBottomOffset"
    )
    Text(
        text = toastMsg,
        modifier = modifier
            .padding(horizontal = 16.dp)
            .padding(bottom = bottomOffset)
            .background(DeckColors.BgElevated, RoundedCornerShape(24.dp))
            .border(1.dp, DeckColors.BorderStrong, RoundedCornerShape(24.dp))
            .padding(horizontal = 20.dp, vertical = 10.dp),
        color = DeckColors.TextPrimary,
        fontSize = 13.sp,
        fontWeight = FontWeight.Medium
    )
}
