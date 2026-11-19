package io.github.xiangwen.wifideck.ui

import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.ViewModelStore
import io.github.xiangwen.wifideck.data.BleGatePrompt
import io.github.xiangwen.wifideck.data.BleManager
import io.github.xiangwen.wifideck.data.BleTransport
import io.github.xiangwen.wifideck.data.ReconnectPhase
import io.github.xiangwen.wifideck.data.ReconnectUiState
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.test.setMain
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

private class FakeBleTransport : BleTransport {
    private val _connectionState = MutableStateFlow(BleManager.ConnectionState.DISCONNECTED)
    override val connectionState: StateFlow<BleManager.ConnectionState> = _connectionState

    private val _reconnectState = MutableStateFlow(ReconnectUiState())
    override val reconnectState: StateFlow<ReconnectUiState> = _reconnectState

    private val _mtu = MutableStateFlow(247)
    override val mtu: StateFlow<Int> = _mtu

    override var onLine: ((String) -> Unit)? = null
    override var onScanError: ((String) -> Unit)? = null

    val sentCommands = mutableListOf<String>()
    val sentTargetedCommands = mutableListOf<Pair<String, List<Int>>>()
    var emergencyStopCount = 0
    var disconnectCalls = 0
    var retryReconnectCalls = 0
    var retryReconnectResult = false

    fun setState(state: BleManager.ConnectionState) {
        _connectionState.value = state
    }

    fun setReconnectUiState(state: ReconnectUiState) {
        _reconnectState.value = state
    }

    fun emitLine(json: String) {
        onLine?.invoke(json)
    }

    override fun retryReconnect(): Boolean {
        retryReconnectCalls++
        return retryReconnectResult
    }

    override fun sendCommand(cmd: String): Boolean {
        if (_connectionState.value != BleManager.ConnectionState.CONNECTED) return false
        sentCommands.add(cmd)
        return true
    }

    override fun sendEmergencyStop(): Boolean {
        if (_connectionState.value != BleManager.ConnectionState.CONNECTED) return false
        emergencyStopCount++
        sentCommands.add("stop")
        return true
    }

    override fun sendTargetedCommand(baseCmd: String, indices: Collection<Int>): Boolean {
        if (_connectionState.value != BleManager.ConnectionState.CONNECTED) return false
        val sorted = indices.distinct().sorted()
        sentTargetedCommands.add(baseCmd to sorted)
        return true
    }

    override fun disconnect(userInitiated: Boolean) {
        disconnectCalls++
        _connectionState.value = BleManager.ConnectionState.DISCONNECTED
    }

    override fun onBluetoothAdapterOff(): Boolean {
        val wasActive = _connectionState.value != BleManager.ConnectionState.DISCONNECTED
        _connectionState.value = BleManager.ConnectionState.DISCONNECTED
        return wasActive
    }

    override fun onBluetoothAdapterOn() = Unit
}

@OptIn(ExperimentalCoroutinesApi::class)
class MainViewModelTest {

    private val testDispatcher = StandardTestDispatcher()
    private lateinit var fakeBle: FakeBleTransport
    private lateinit var store: ViewModelStore
    private lateinit var viewModel: MainViewModel

    @Before
    fun setUp() {
        Dispatchers.setMain(testDispatcher)
        fakeBle = FakeBleTransport()
        viewModel = MainViewModel(fakeBle)
        store = ViewModelStore().also { it.put("vm", viewModel) }
    }

    @After
    fun tearDown() {
        store.clear()
        Dispatchers.resetMain()
    }

    private fun TestScope.connectAndPushAps(vararg aps: Pair<Int, String>) {
        fakeBle.setState(BleManager.ConnectionState.CONNECTED)
        runCurrent()
        fakeBle.emitLine("""{"t":"scan_begin"}""")
        aps.forEach { (idx, ssid) ->
            val macSuffix = "%02X".format(idx)
            fakeBle.emitLine("""{"t":"ap","i":$idx,"s":"$ssid","b":"AA:BB:CC:DD:EE:$macSuffix","c":6,"r":-${50 + idx}}""")
        }
        fakeBle.emitLine("""{"t":"scan_end","n":${aps.size}}""")
        runCurrent()
        viewModel.clearToast()
    }

    @Test
    fun decisionA_blocksZeroTargetAttacks_andUsesTargetedOrExplicitAllCommands() = runTest(testDispatcher) {
        connectAndPushAps(0 to "Alpha", 1 to "Beta", 2 to "Gamma")

        // 0 targets -> blocked for start deauther, disassoc, timed attack
        viewModel.sendStartDeauth()
        assertEquals("请先在扫描页选择至少一个目标", viewModel.toast.value)
        viewModel.clearToast()

        viewModel.sendDisassoc()
        assertEquals("请先在扫描页选择至少一个目标", viewModel.toast.value)
        viewModel.clearToast()

        viewModel.sendTimedAttack(10_000L)
        assertEquals("请先在扫描页选择至少一个目标", viewModel.toast.value)
        assertTrue(fakeBle.sentCommands.isEmpty())
        assertTrue(fakeBle.sentTargetedCommands.isEmpty())

        // Partial selection (2 of 3) -> targeted command with sorted indices
        viewModel.toggleSelect(2)
        viewModel.toggleSelect(0)
        viewModel.sendStartDeauth()
        viewModel.sendDisassoc()
        viewModel.sendTimedAttack(30_000L)
        assertEquals(
            listOf(
                "start deauther" to listOf(0, 2),
                "disassoc" to listOf(0, 2),
                "attack_time 30000" to listOf(0, 2)
            ),
            fakeBle.sentTargetedCommands
        )
        assertTrue(fakeBle.sentCommands.isEmpty())

        // Full selection (>1 AP) -> explicit 'all'
        fakeBle.sentTargetedCommands.clear()
        viewModel.toggleSelect(1)
        viewModel.sendStartDeauth()
        viewModel.sendDisassoc()
        viewModel.sendTimedAttack(60_000L)
        assertEquals(
            listOf("start deauther all", "disassoc all", "attack_time 60000 all"),
            fakeBle.sentCommands
        )
        assertTrue(fakeBle.sentTargetedCommands.isEmpty())
    }

    @Test
    fun decisionA_singleApSelectedSendsIndexedCommandNotAll() = runTest(testDispatcher) {
        connectAndPushAps(0 to "SoloAP")

        viewModel.toggleSelect(0)
        viewModel.sendStartDeauth()

        assertEquals(listOf("start deauther" to listOf(0)), fakeBle.sentTargetedCommands)
        assertTrue(fakeBle.sentCommands.isEmpty())
    }

    @Test
    fun syncBeginAndSyncEnd_doNotMutateScanningStateOrTriggerScanToastsAndPreserveSelectedBssid() = runTest(testDispatcher) {
        connectAndPushAps(0 to "First", 1 to "TargetAP")
        viewModel.toggleSelect(1) // AA:BB:CC:DD:EE:01

        assertFalse(viewModel.scanning.value)
        assertNull(viewModel.toast.value)

        // Attack-cycle rescan / results sync uses sync_begin..sync_end
        fakeBle.emitLine("""{"t":"sync_begin"}""")
        runCurrent()
        assertFalse(viewModel.scanning.value)

        // TargetAP shifts from index 1 to index 0 without sel=1 in JSON; sync mode preserves it by BSSID
        fakeBle.emitLine("""{"t":"ap","i":0,"s":"TargetAP","b":"AA:BB:CC:DD:EE:01","c":6,"r":-52}""")
        fakeBle.emitLine("""{"t":"sync_end","n":1}""")
        runCurrent()

        assertFalse(viewModel.scanning.value)
        assertNull(viewModel.toast.value)
        assertEquals(1, viewModel.apList.value.size)
        assertEquals(setOf(0), viewModel.selected.value)

        // Advance well beyond scan watchdog timeout; no watchdog toast fires
        advanceTimeBy(45_000L)
        runCurrent()
        assertFalse(viewModel.scanning.value)
        assertNull(viewModel.toast.value)
    }

    @Test
    fun apBackingMap_deduplicatesByIndexAndReconcilesSelectionFlag() = runTest(testDispatcher) {
        fakeBle.setState(BleManager.ConnectionState.CONNECTED)
        runCurrent()

        fakeBle.emitLine("""{"t":"scan_begin"}""")
        fakeBle.emitLine("""{"t":"ap","i":1,"s":"OldName","b":"AA:BB:CC:DD:EE:01","c":1,"r":-70,"sel":1}""")
        // Duplicate index 1 arrives with updated signal and sel cleared
        fakeBle.emitLine("""{"t":"ap","i":1,"s":"NewName","b":"AA:BB:CC:DD:EE:01","c":1,"r":-48}""")
        fakeBle.emitLine("""{"t":"ap","i":2,"s":"Other","b":"AA:BB:CC:DD:EE:02","c":36,"r":-60,"sel":1}""")
        runCurrent()

        // Flush batch (75ms)
        advanceTimeBy(100L)
        runCurrent()

        assertEquals(2, viewModel.apList.value.size)
        val ap1 = viewModel.apList.value.first { it.index == 1 }
        assertEquals("NewName", ap1.ssid)
        assertEquals(-48, ap1.rssi)
        assertFalse(ap1.selected)
        assertEquals(setOf(2), viewModel.selected.value)
    }

    @Test
    fun clearLogs_clearsLocalCacheAndSyncsClearLogsOnlyWhenConnected() = runTest(testDispatcher) {
        fakeBle.setState(BleManager.ConnectionState.CONNECTED)
        runCurrent()

        fakeBle.emitLine("""{"t":"log","m":"[MGMT] Beacon A"}""")
        fakeBle.emitLine("""{"t":"log","m":"[DATA] EAPOL M1"}""")
        runCurrent()
        advanceTimeBy(100L)
        runCurrent()

        assertEquals(2, viewModel.logs.value.size)
        assertEquals(1L, viewModel.logs.value[0].seq)
        assertEquals(2L, viewModel.logs.value[1].seq)

        // Connected: clears local cache, resets seq counter, and sends clear_logs
        viewModel.clearLogs()
        assertTrue(viewModel.logs.value.isEmpty())
        assertTrue(fakeBle.sentCommands.contains("clear_logs"))
        assertEquals("已清空抓包日志并同步至开发板", viewModel.toast.value)

        // Next incoming frame starts at seq = 1 again
        fakeBle.emitLine("""{"t":"log","m":"[MGMT] Probe B"}""")
        runCurrent()
        advanceTimeBy(100L)
        runCurrent()
        assertEquals(1, viewModel.logs.value.size)
        assertEquals("001", viewModel.logs.value.single().formattedSeq)

        // Disconnected: clears local cache without sending BLE command
        fakeBle.sentCommands.clear()
        fakeBle.setState(BleManager.ConnectionState.DISCONNECTED)
        runCurrent()
        viewModel.clearLogs()
        assertTrue(viewModel.logs.value.isEmpty())
        assertTrue(fakeBle.sentCommands.isEmpty())
        assertEquals("已清空本地抓包日志", viewModel.toast.value)
    }

    @Test
    fun disconnect_preservesConfigResetSeq_whileResetConfigIncrementsIt() = runTest(testDispatcher) {
        fakeBle.setState(BleManager.ConnectionState.CONNECTED)
        runCurrent()

        // Partial cfg alone is not synced
        fakeBle.emitLine("""{"t":"cfg","delay":1500,"scan":4000,"frames":5,"ch":1,"sc":0,"led":1,"ap":1}""")
        runCurrent()
        assertFalse(viewModel.configSynced.value)
        assertFalse(viewModel.canWriteSettings.value)

        // Completing id frame finishes handshake sync
        fakeBle.emitLine("""{"t":"id","ssid":"LabAP","pass":"secret123","ble":"Deck-01"}""")
        runCurrent()
        assertTrue(viewModel.configSynced.value)
        assertTrue(viewModel.canWriteSettings.value)
        assertEquals(0L, viewModel.configResetSeq.value)

        // Unexpected disconnect clears synced flag & remote config snapshot, but does NOT bump configResetSeq
        fakeBle.setState(BleManager.ConnectionState.DISCONNECTED)
        runCurrent()
        assertFalse(viewModel.configSynced.value)
        assertFalse(viewModel.canWriteSettings.value)
        assertEquals(0L, viewModel.configResetSeq.value)

        // Explicit reset_config confirmed by default id frame increments configResetSeq to clear drafts
        fakeBle.setState(BleManager.ConnectionState.CONNECTED)
        runCurrent()
        viewModel.sendResetConfig()
        assertTrue(fakeBle.sentCommands.contains("reset_config"))
        fakeBle.emitLine("""{"t":"id","ssid":"WiFi-Deck","pass":"12345678","ble":"WiFi-Deck"}""")
        runCurrent()
        assertEquals(1L, viewModel.configResetSeq.value)
    }

    @Test
    fun btOffRace_disconnectBeforeAdapterOffBroadcast_latchesPendingReconnectAndTriggersOnBtOn() = runTest(testDispatcher) {
        val gateActions = mutableListOf<BleGateAction>()
        val collectJob = launch {
            viewModel.gateActions.collect { gateActions.add(it) }
        }
        try {
            fakeBle.setState(BleManager.ConnectionState.CONNECTED)
            runCurrent()

            // STATE_DISCONNECTED arrives before ACTION_STATE_CHANGED(OFF) broadcast
            fakeBle.setState(BleManager.ConnectionState.DISCONNECTED)
            runCurrent()

            viewModel.onBluetoothStateChanged(false)
            runCurrent()
            assertEquals("系统蓝牙已关闭，BLE 连接已断开", viewModel.toast.value)
            viewModel.clearToast()

            viewModel.onBluetoothStateChanged(true)
            runCurrent()
            assertEquals("系统蓝牙已开启，正在重新连接 WiFi-Deck...", viewModel.toast.value)
            assertEquals(listOf(BleGateAction.CheckAndConnect(autoTriggered = false)), gateActions)

            // Latched flag is consumed once: a second BT-ON event does not re-trigger
            viewModel.clearToast()
            viewModel.onBluetoothStateChanged(true)
            runCurrent()
            assertNull(viewModel.toast.value)
            assertEquals(1, gateActions.size)
        } finally {
            collectJob.cancel()
        }
    }

    @Test
    fun disconnectBle_clearsPendingReconnectOnBtOn_andSuppressesAutoReconnect() = runTest(testDispatcher) {
        val gateActions = mutableListOf<BleGateAction>()
        val collectJob = launch {
            viewModel.gateActions.collect { gateActions.add(it) }
        }
        try {
            // Case 1: BT off latched pendingReconnectOnBtOn, then user calls disconnectBle()
            fakeBle.setState(BleManager.ConnectionState.CONNECTED)
            runCurrent()
            fakeBle.setState(BleManager.ConnectionState.DISCONNECTED)
            runCurrent()

            viewModel.onBluetoothStateChanged(false)
            runCurrent()
            assertEquals("系统蓝牙已关闭，BLE 连接已断开", viewModel.toast.value)
            viewModel.clearToast()

            viewModel.disconnectBle()
            runCurrent()

            viewModel.onBluetoothStateChanged(true)
            runCurrent()
            assertNull(viewModel.toast.value)
            assertTrue(gateActions.isEmpty())

            // Case 2: User calls disconnectBle() before BT turns off -> no latch set
            fakeBle.setState(BleManager.ConnectionState.CONNECTED)
            runCurrent()
            viewModel.disconnectBle()
            runCurrent()
            viewModel.onBluetoothStateChanged(false)
            runCurrent()
            assertNull(viewModel.toast.value)
            viewModel.onBluetoothStateChanged(true)
            runCurrent()
            assertNull(viewModel.toast.value)
            assertTrue(gateActions.isEmpty())
        } finally {
            collectJob.cancel()
        }
    }

    @Test
    fun scanWatchdog_usesScanTimePlus10sHeadroom_andResetsOnScanBeginAndApFrames() = runTest(testDispatcher) {
        fakeBle.setState(BleManager.ConnectionState.CONNECTED)
        runCurrent()

        // Configure scanTimeMs = 12_000ms -> watchdog timeout = max(15_000, 12_000 + 10_000) = 22_000ms
        fakeBle.emitLine("""{"t":"cfg","delay":1000,"scan":12000,"frames":4,"ch":1,"sc":0,"led":1,"ap":1}""")
        runCurrent()

        viewModel.sendScan()
        runCurrent()
        assertTrue(viewModel.scanning.value)

        // At 15_000ms (past 15s floor, within 22s headroom), scan is still active; SCAN_BEGIN resets watchdog
        advanceTimeBy(15_000L)
        runCurrent()
        assertTrue(viewModel.scanning.value)
        assertNull(viewModel.toast.value)

        fakeBle.emitLine("""{"t":"scan_begin"}""")
        runCurrent()

        // Advance 21_000ms (total 36_000ms, well past initial 22s deadline): still active, then AP frame resets again
        advanceTimeBy(21_000L)
        runCurrent()
        assertTrue(viewModel.scanning.value)
        assertNull(viewModel.toast.value)

        fakeBle.emitLine("""{"t":"ap","i":0,"s":"StreamAP1","b":"AA:BB:CC:DD:EE:01","c":6,"r":-50}""")
        runCurrent()

        // Advance 21_000ms after first AP: still active, second AP resets watchdog again
        advanceTimeBy(21_000L)
        runCurrent()
        assertTrue(viewModel.scanning.value)
        assertNull(viewModel.toast.value)

        fakeBle.emitLine("""{"t":"ap","i":1,"s":"StreamAP2","b":"AA:BB:CC:DD:EE:02","c":36,"r":-60}""")
        runCurrent()

        // Just before 22_000ms headroom expires: still scanning
        advanceTimeBy(21_999L)
        runCurrent()
        assertTrue(viewModel.scanning.value)
        assertNull(viewModel.toast.value)

        // Crossing 22_000ms without scan_end triggers timeout and flushes buffered APs
        advanceTimeBy(2L)
        runCurrent()
        assertFalse(viewModel.scanning.value)
        assertEquals("扫描超时，未收到开发板返回", viewModel.toast.value)
        assertEquals(2, viewModel.apList.value.size)
    }

    @Test
    fun missingStatusHeartbeat_doesNotDropConnection() = runTest(testDispatcher) {
        fakeBle.setState(BleManager.ConnectionState.CONNECTED)
        runCurrent()
        fakeBle.emitLine("""{"t":"cfg","delay":1500,"scan":4000,"frames":5,"ch":1,"sc":0,"led":1,"ap":1}""")
        fakeBle.emitLine("""{"t":"id","ssid":"LabAP","pass":"secret123","ble":"Deck-01"}""")
        runCurrent()
        assertTrue(viewModel.configSynced.value)

        // Advance 2 minutes with zero {"t":"st",...} heartbeat frames
        advanceTimeBy(120_000L)
        runCurrent()

        assertEquals(BleManager.ConnectionState.CONNECTED, viewModel.connectionState.value)
        assertEquals(0, fakeBle.disconnectCalls)
        assertNull(viewModel.toast.value)
        assertTrue(viewModel.configSynced.value)
    }

    private class DummyOtherViewModel : ViewModel()

    @Test
    fun factory_injectsCustomBleTransportAndPropagatesReconnectState() = runTest(testDispatcher) {
        val customTransport = FakeBleTransport()
        val factory: ViewModelProvider.Factory = MainViewModel.Factory(customTransport)
        val localStore = ViewModelStore()
        try {
            val vm = ViewModelProvider(localStore, factory)[MainViewModel::class.java]
            val reconnecting = ReconnectUiState(phase = ReconnectPhase.RECONNECTING_SCAN, attempt = 2, maxAttempts = 5)
            customTransport.setReconnectUiState(reconnecting)
            runCurrent()
            assertEquals(reconnecting, vm.reconnectState.value)

            var threw = false
            try {
                factory.create(DummyOtherViewModel::class.java)
            } catch (_: IllegalArgumentException) {
                threw = true
            }
            assertTrue(threw)
        } finally {
            localStore.clear()
        }
    }

    @Test
    fun retryOrConnectBle_prefersTransportRetryReconnectBeforeGateCheck() = runTest(testDispatcher) {
        val gateActions = mutableListOf<BleGateAction>()
        val collectJob = launch {
            viewModel.gateActions.collect { gateActions.add(it) }
        }
        try {
            viewModel.showBleGatePrompt(BleGatePrompt.BluetoothDisabled)
            fakeBle.retryReconnectResult = true
            viewModel.retryOrConnectBle()
            runCurrent()

            assertEquals(1, fakeBle.retryReconnectCalls)
            assertNull(viewModel.bleGatePrompt.value)
            assertTrue(gateActions.isEmpty())

            fakeBle.retryReconnectResult = false
            viewModel.retryOrConnectBle()
            runCurrent()

            assertEquals(2, fakeBle.retryReconnectCalls)
            assertEquals(listOf(BleGateAction.CheckAndConnect(autoTriggered = false)), gateActions)
        } finally {
            collectJob.cancel()
        }
    }

    @Test
    fun statusMessage_parsesAllFieldsAndUpdatesDerivedFlows() = runTest(testDispatcher) {
        fakeBle.setState(BleManager.ConnectionState.CONNECTED)
        runCurrent()

        fakeBle.emitLine(
            """{"t":"st","atk":1,"dis":0,"bcn":1,"prb":0,"jam":0,"snf":0,"snfOn":1,"sMode":4,"hop":1,"ch":149,"ok":128,"fail":3,"rem":12}"""
        )
        runCurrent()

        val st = viewModel.status.value
        assertTrue(st.attackEnabled)
        assertFalse(st.disassocEnabled)
        assertTrue(st.beaconFlood)
        assertTrue(st.sniffing)
        assertEquals(4, st.sniffMode)
        assertTrue(st.hopping)
        assertEquals(149, st.channel)
        assertEquals(128L, st.txOk)
        assertEquals(3L, st.txFail)
        assertEquals(12, st.timedRemainingSec)
        assertTrue(viewModel.isAttacking.value)
        assertTrue(viewModel.isSniffing.value)
    }

    @Test
    fun scanListState_filtersByBandAndSanitizedQueryAndSortsCorrectly() = runTest(testDispatcher) {
        fakeBle.setState(BleManager.ConnectionState.CONNECTED)
        runCurrent()
        fakeBle.emitLine("""{"t":"scan_begin"}""")
        fakeBle.emitLine("""{"t":"ap","i":0,"s":"Home24","b":"AA:BB:CC:00:00:01","c":11,"r":-70}""")
        fakeBle.emitLine("""{"t":"ap","i":1,"s":"Office5G","b":"AA:BB:CC:00:00:02","c":36,"r":-55}""")
        fakeBle.emitLine("""{"t":"ap","i":2,"s":"Cafe24","b":"AA:BB:CC:00:00:03","c":1,"r":-45}""")
        fakeBle.emitLine("""{"t":"scan_end","n":3}""")
        runCurrent()

        assertEquals(3, viewModel.scanListState.value.totalCount)
        assertEquals(2, viewModel.scanListState.value.count24G)
        assertEquals(1, viewModel.scanListState.value.count5G)
        // Default sort RSSI descending: Cafe24(-45), Office5G(-55), Home24(-70)
        assertEquals(listOf(2, 1, 0), viewModel.scanListState.value.filteredAps.map { it.index })

        viewModel.setSortMode(SortMode.CHANNEL)
        runCurrent()
        assertEquals(listOf(2, 0, 1), viewModel.scanListState.value.filteredAps.map { it.index })

        viewModel.setBandFilter(BandFilter.G5)
        runCurrent()
        assertEquals(listOf(1), viewModel.scanListState.value.filteredAps.map { it.index })

        viewModel.selectAll()
        assertEquals(setOf(1), viewModel.selected.value)

        viewModel.setBandFilter(BandFilter.ALL)
        viewModel.setSearchQuery("cafe\r\n")
        runCurrent()
        assertEquals("cafe", viewModel.searchQuery.value)
        assertEquals(listOf(2), viewModel.scanListState.value.filteredAps.map { it.index })
    }

    @Test
    fun logRingBuffer_capsAt500EntriesAndErrorLogAbortsPendingConfigSave() = runTest(testDispatcher) {
        fakeBle.setState(BleManager.ConnectionState.CONNECTED)
        runCurrent()

        for (i in 1..505) {
            fakeBle.emitLine("""{"t":"log","m":"[MGMT] Beacon #$i"}""")
            if (i % 64 == 0) runCurrent()
        }
        runCurrent()
        advanceTimeBy(100L)
        runCurrent()

        assertEquals(500, viewModel.logs.value.size)
        assertEquals("Beacon #6", viewModel.logs.value.first().displayText)
        assertEquals("Beacon #505", viewModel.logs.value.last().displayText)

        // Start an identity save, then simulate firmware [ERROR] log aborting savingConfig
        viewModel.sendSetBleName("DeckPro")
        assertTrue(viewModel.savingConfig.value)

        fakeBle.emitLine("""{"t":"log","m":"[ERROR] Invalid setting"}""")
        runCurrent()
        assertFalse(viewModel.savingConfig.value)
        assertEquals("[ERROR] Invalid setting", viewModel.toast.value)
    }

    @Test
    fun identityConfigSave_validatesUtf8BoundsProbesInfoAt1500msAndTimesOutAt3300ms() = runTest(testDispatcher) {
        fakeBle.setState(BleManager.ConnectionState.CONNECTED)
        runCurrent()

        // 9 Chinese chars = 27 UTF-8 bytes (> 26B limit) -> rejected locally
        viewModel.sendSetBleName("一二三四五六七八九")
        assertEquals("BLE 蓝牙名称长度需为 1~26 字节", viewModel.toast.value)
        assertTrue(fakeBle.sentCommands.isEmpty())

        // Short WPA2 password (< 8B) -> rejected locally
        viewModel.sendSetApPass("1234567")
        assertEquals("WPA2 热点密码长度需为 8~63 字符/字节", viewModel.toast.value)
        assertTrue(fakeBle.sentCommands.isEmpty())

        // Valid SSID update triggers auto "info" probe at 1500ms and times out cleanly at 3300ms if unconfirmed
        viewModel.clearToast()
        viewModel.sendSetApSsid("LabDeck")
        assertTrue(viewModel.savingConfig.value)
        assertEquals(listOf("set ap_ssid LabDeck"), fakeBle.sentCommands)

        advanceTimeBy(1_500L)
        runCurrent()
        assertTrue(fakeBle.sentCommands.contains("info"))
        assertTrue(viewModel.savingConfig.value)

        advanceTimeBy(1_800L)
        runCurrent()
        assertFalse(viewModel.savingConfig.value)
        assertEquals("热点名称保存超时，请刷新核对", viewModel.toast.value)
    }

    @Test
    fun debouncedSettings_gateOnConfigSyncAndEnforcePerKey350msDebounce() = runTest(testDispatcher) {
        fakeBle.setState(BleManager.ConnectionState.CONNECTED)
        runCurrent()

        // Blocked before cfg + id sync completes
        viewModel.sendLedToggle(false)
        assertEquals("正在同步设备配置，请稍候", viewModel.toast.value)
        assertTrue(fakeBle.sentCommands.isEmpty())

        fakeBle.emitLine("""{"t":"cfg","delay":2000,"scan":5000,"frames":3,"ch":1,"sc":0,"led":1,"ap":1}""")
        fakeBle.emitLine("""{"t":"id","ssid":"WiFi-Deck","pass":"12345678","ble":"WiFi-Deck"}""")
        runCurrent()
        assertTrue(viewModel.canWriteSettings.value)

        viewModel.sendLedToggle(false)
        // Immediate duplicate key within 350ms is debounced; different key goes through
        viewModel.sendLedToggle(true)
        viewModel.sendSetFrames(5)
        assertEquals(listOf("set led off", "set num_frames 5"), fakeBle.sentCommands)
    }

    @Test
    fun confirmBleGatePrompt_dispatchesExpectedActionsAndSetChannelGuardsActiveSoftAp() = runTest(testDispatcher) {
        val gateActions = mutableListOf<BleGateAction>()
        val collectJob = launch {
            viewModel.gateActions.collect { gateActions.add(it) }
        }
        try {
            viewModel.showBleGatePrompt(BleGatePrompt.PermissionRationale(isAndroid12Plus = true))
            viewModel.confirmBleGatePrompt()
            viewModel.showBleGatePrompt(BleGatePrompt.PermissionPermanentlyDenied(isAndroid12Plus = true))
            viewModel.confirmBleGatePrompt()
            viewModel.showBleGatePrompt(BleGatePrompt.BluetoothDisabled)
            viewModel.confirmBleGatePrompt()
            viewModel.showBleGatePrompt(BleGatePrompt.LocationDisabled)
            viewModel.confirmBleGatePrompt()
            runCurrent()

            assertEquals(
                listOf(
                    BleGateAction.LaunchPermissionRequest,
                    BleGateAction.LaunchAppSettings,
                    BleGateAction.LaunchEnableBluetooth,
                    BleGateAction.LaunchLocationSettings
                ),
                gateActions
            )

            // SoftAP active + not sniffing blocks channel lock; sniffing=true allows channel lock
            fakeBle.setState(BleManager.ConnectionState.CONNECTED)
            fakeBle.emitLine("""{"t":"cfg","delay":2000,"scan":5000,"frames":3,"ch":1,"sc":0,"led":1,"ap":1}""")
            fakeBle.emitLine("""{"t":"st","atk":0,"dis":0,"bcn":0,"prb":0,"jam":0,"snf":0,"snfOn":0,"sMode":6,"hop":0,"ch":1,"ok":0,"fail":0,"rem":0}""")
            runCurrent()

            viewModel.sendSetChannel(36)
            assertEquals("热点开启中不可直接锁频，请先开启抓包或关闭热点", viewModel.toast.value)
            assertFalse(fakeBle.sentCommands.contains("set ch 36"))

            fakeBle.emitLine("""{"t":"st","atk":0,"dis":0,"bcn":0,"prb":0,"jam":0,"snf":0,"snfOn":1,"sMode":0,"hop":0,"ch":1,"ok":0,"fail":0,"rem":0}""")
            runCurrent()
            viewModel.sendSetChannel(36)
            assertTrue(fakeBle.sentCommands.contains("set ch 36"))
            assertEquals("已锁定信道 CH 36", viewModel.toast.value)
        } finally {
            collectJob.cancel()
        }
    }

    @Test
    fun configMessage_parsesFirmwareVersionAndResetsOnDisconnect() = runTest(testDispatcher) {
        fakeBle.setState(BleManager.ConnectionState.CONNECTED)
        runCurrent()

        assertEquals("", viewModel.config.value.firmwareVersion)

        fakeBle.emitLine(
            """{"t":"cfg","delay":2000,"scan":5000,"frames":3,"ch":1,"sc":0,"led":1,"ap":1,"ver":"2.2.0"}"""
        )
        runCurrent()
        assertEquals("2.2.0", viewModel.config.value.firmwareVersion)

        // Legacy cfg frame without "ver" preserves already known firmwareVersion during active session
        fakeBle.emitLine(
            """{"t":"cfg","delay":1000,"scan":5000,"frames":5,"ch":1,"sc":0,"led":1,"ap":1}"""
        )
        runCurrent()
        assertEquals("2.2.0", viewModel.config.value.firmwareVersion)
        assertEquals(5, viewModel.config.value.numSendFrames)

        // Disconnect resets remote DeviceConfig snapshot cleanly
        fakeBle.setState(BleManager.ConnectionState.DISCONNECTED)
        runCurrent()
        assertEquals("", viewModel.config.value.firmwareVersion)
    }

    @Test
    fun apToggle_optimisticallyUpdatesSwitchAndReconcilesFromStatusHeartbeat() = runTest(testDispatcher) {
        fakeBle.setState(BleManager.ConnectionState.CONNECTED)
        fakeBle.emitLine("""{"t":"cfg","delay":2000,"scan":5000,"frames":3,"ch":1,"sc":0,"led":1,"ap":1,"ver":"2.2.0"}""")
        fakeBle.emitLine("""{"t":"id","ssid":"WiFi-Deck","pass":"12345678","ble":"WiFi-Deck"}""")
        runCurrent()
        assertTrue(viewModel.config.value.apEnabled)

        // Optimistic toggle off sends "ap off" and flips apEnabled immediately
        viewModel.sendApToggle(false)
        assertEquals(listOf("ap off"), fakeBle.sentCommands)
        assertFalse(viewModel.config.value.apEnabled)

        // Even if cfg frame was missed during RF switch, 1s st frame with "ap":0/1 reconciles apEnabled
        fakeBle.emitLine(
            """{"t":"st","atk":0,"dis":0,"bcn":0,"prb":0,"jam":0,"snf":0,"snfOn":0,"sMode":6,"hop":0,"ch":1,"ok":0,"fail":0,"rem":0,"ap":1}"""
        )
        runCurrent()
        assertTrue(viewModel.config.value.apEnabled)

        fakeBle.emitLine(
            """{"t":"st","atk":0,"dis":0,"bcn":0,"prb":0,"jam":0,"snf":0,"snfOn":1,"sMode":0,"hop":1,"ch":6,"ok":0,"fail":0,"rem":0,"ap":0}"""
        )
        runCurrent()
        assertFalse(viewModel.config.value.apEnabled)
    }

    @Test
    fun initialSyncProbe_reRequestsMissingInfoAndResultsAt1200ms_unlessAlreadySyncedOrScanning() = runTest(testDispatcher) {
        // Case 1: Neither cfg/id nor sync_begin/sync_end arrived within 1200ms -> probes both "info" and "results"
        fakeBle.setState(BleManager.ConnectionState.CONNECTED)
        runCurrent()
        advanceTimeBy(1_200L)
        runCurrent()
        assertEquals(listOf("info", "results"), fakeBle.sentCommands)

        // Case 2: Reconnect where cfg+id+sync_end arrive before 1200ms -> zero redundant probes
        fakeBle.sentCommands.clear()
        fakeBle.setState(BleManager.ConnectionState.DISCONNECTED)
        runCurrent()
        fakeBle.setState(BleManager.ConnectionState.CONNECTED)
        runCurrent()
        fakeBle.emitLine("""{"t":"cfg","delay":2000,"scan":5000,"frames":3,"ch":1,"sc":0,"led":1,"ap":0,"ver":"2.2.0"}""")
        fakeBle.emitLine("""{"t":"id","ssid":"WiFi-Deck","pass":"12345678","ble":"WiFi-Deck"}""")
        fakeBle.emitLine("""{"t":"sync_begin"}""")
        fakeBle.emitLine("""{"t":"sync_end","n":0}""")
        runCurrent()
        advanceTimeBy(1_500L)
        runCurrent()
        assertTrue(fakeBle.sentCommands.isEmpty())

        // Case 3: User starts active scan right after connect -> 1200ms probe must NOT inject "results" mid-scan
        fakeBle.sentCommands.clear()
        fakeBle.setState(BleManager.ConnectionState.DISCONNECTED)
        runCurrent()
        fakeBle.setState(BleManager.ConnectionState.CONNECTED)
        runCurrent()
        fakeBle.emitLine("""{"t":"cfg","delay":2000,"scan":5000,"frames":3,"ch":1,"sc":0,"led":1,"ap":0,"ver":"2.2.0"}""")
        fakeBle.emitLine("""{"t":"id","ssid":"WiFi-Deck","pass":"12345678","ble":"WiFi-Deck"}""")
        runCurrent()
        viewModel.sendScan()
        runCurrent()
        assertEquals(listOf("scan"), fakeBle.sentCommands)
        advanceTimeBy(1_500L)
        runCurrent()
        assertEquals(listOf("scan"), fakeBle.sentCommands)
    }

    @Test
    fun segmentedDualBandScan_streamsAcross12SecondsWithLowScanTimeConfigWithoutWatchdogFalsePositive() = runTest(testDispatcher) {
        fakeBle.setState(BleManager.ConnectionState.CONNECTED)
        runCurrent()
        // Even with user-configured minimum scanTimeMs = 500ms, 15s floor covers ~10-12s segmented 2.4G/5G scan
        fakeBle.emitLine("""{"t":"cfg","delay":2000,"scan":500,"frames":3,"ch":1,"sc":0,"led":1,"ap":0,"ver":"2.2.0"}""")
        fakeBle.emitLine("""{"t":"id","ssid":"WiFi-Deck","pass":"12345678","ble":"WiFi-Deck"}""")
        runCurrent()

        viewModel.sendScan()
        runCurrent()
        fakeBle.emitLine("""{"t":"scan_begin"}""")
        runCurrent()
        assertTrue(viewModel.scanning.value)

        // Simulate 16 segmented 2.4G + 5G batches taking 11.5 seconds before first AP result push
        advanceTimeBy(11_500L)
        runCurrent()
        assertTrue(viewModel.scanning.value)
        assertNull(viewModel.toast.value)

        fakeBle.emitLine("""{"t":"ap","i":0,"s":"Stealth2G","b":"AA:BB:CC:00:00:01","c":6,"r":-48}""")
        fakeBle.emitLine("""{"t":"ap","i":1,"s":"Stealth5G","b":"AA:BB:CC:00:00:02","c":149,"r":-54}""")
        fakeBle.emitLine("""{"t":"scan_end","n":2}""")
        runCurrent()

        assertFalse(viewModel.scanning.value)
        assertEquals("扫描完成: 2 个网络", viewModel.toast.value)
        assertEquals(1, viewModel.scanListState.value.count24G)
        assertEquals(1, viewModel.scanListState.value.count5G)
    }

    @Test
    fun reconnectDuring5GHzSniffing_restoresSniffingHoppingChannelAndStealthApStateImmediately() = runTest(testDispatcher) {
        fakeBle.setState(BleManager.ConnectionState.CONNECTED)
        runCurrent()

        // Simulate immediate 'info' response pushing cfg + id + critical st on reconnect during 5GHz hopping sniff
        fakeBle.emitLine("""{"t":"cfg","delay":2000,"scan":5000,"frames":3,"ch":1,"sc":0,"led":1,"ap":0,"ver":"2.2.0"}""")
        fakeBle.emitLine("""{"t":"id","ssid":"WiFi-Deck","pass":"12345678","ble":"WiFi-Deck"}""")
        fakeBle.emitLine(
            """{"t":"st","atk":0,"dis":0,"bcn":0,"prb":0,"jam":0,"snf":1,"snfOn":1,"sMode":3,"hop":1,"ch":157,"ok":9,"fail":0,"rem":0,"ap":0}"""
        )
        runCurrent()

        assertTrue(viewModel.configSynced.value)
        assertFalse(viewModel.config.value.apEnabled)
        assertTrue(viewModel.isSniffing.value)
        assertTrue(viewModel.isAttacking.value)
        assertTrue(viewModel.status.value.deauthSniff)
        assertTrue(viewModel.status.value.hopping)
        assertEquals(157, viewModel.status.value.channel)
        assertEquals(3, viewModel.status.value.sniffMode)
    }
}
