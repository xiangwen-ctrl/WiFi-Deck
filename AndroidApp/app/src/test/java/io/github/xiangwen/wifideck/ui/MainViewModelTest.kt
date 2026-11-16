package io.github.xiangwen.wifideck.ui

import androidx.lifecycle.ViewModelStore
import io.github.xiangwen.wifideck.data.BleManager
import io.github.xiangwen.wifideck.data.BleTransport
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

    private val _mtu = MutableStateFlow(247)
    override val mtu: StateFlow<Int> = _mtu

    override var onLine: ((String) -> Unit)? = null
    override var onScanError: ((String) -> Unit)? = null

    val sentCommands = mutableListOf<String>()
    val sentTargetedCommands = mutableListOf<Pair<String, List<Int>>>()
    var emergencyStopCount = 0
    var disconnectCalls = 0

    fun setState(state: BleManager.ConnectionState) {
        _connectionState.value = state
    }

    fun emitLine(json: String) {
        onLine?.invoke(json)
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
}
