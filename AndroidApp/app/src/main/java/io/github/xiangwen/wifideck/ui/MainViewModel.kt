package io.github.xiangwen.wifideck.ui

import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.CreationExtras
import io.github.xiangwen.wifideck.data.ApItem
import io.github.xiangwen.wifideck.data.BleGatePrompt
import io.github.xiangwen.wifideck.data.BleManager
import io.github.xiangwen.wifideck.data.BleMessageType
import io.github.xiangwen.wifideck.data.BleTransport
import io.github.xiangwen.wifideck.data.ConfigSyncState
import io.github.xiangwen.wifideck.data.DeviceConfig
import io.github.xiangwen.wifideck.data.DeviceStatus
import io.github.xiangwen.wifideck.data.ReconnectUiState
import io.github.xiangwen.wifideck.data.ScanListState
import io.github.xiangwen.wifideck.data.SniffLogEntry
import io.github.xiangwen.wifideck.data.SniffMode
import kotlinx.coroutines.Job
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.receiveAsFlow
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.yield
import org.json.JSONObject
import java.util.concurrent.atomic.AtomicLong

enum class BandFilter { ALL, G24, G5 }
enum class SortMode { RSSI, CHANNEL }

/** ViewModel 向 MainActivity 派发的系统门禁动作（权限请求 / 跳转设置 / 拉起蓝牙开启） */
sealed interface BleGateAction {
    data class CheckAndConnect(val autoTriggered: Boolean) : BleGateAction
    data object LaunchPermissionRequest : BleGateAction
    data object LaunchAppSettings : BleGateAction
    data object LaunchEnableBluetooth : BleGateAction
    data object LaunchLocationSettings : BleGateAction
}

class MainViewModel(
    private val ble: BleTransport
) : ViewModel() {

    constructor() : this(BleManager)

    class Factory(
        private val ble: BleTransport = BleManager
    ) : ViewModelProvider.Factory {
        @Suppress("UNCHECKED_CAST")
        override fun <T : ViewModel> create(modelClass: Class<T>): T {
            require(modelClass.isAssignableFrom(MainViewModel::class.java)) {
                "Unknown ViewModel class: ${modelClass.name}"
            }
            return MainViewModel(ble) as T
        }

        @Suppress("UNCHECKED_CAST")
        override fun <T : ViewModel> create(modelClass: Class<T>, extras: CreationExtras): T {
            return create(modelClass)
        }
    }

    companion object {
        private const val MAX_SNIFF_LOGS = 500
        private const val AP_BATCH_INTERVAL_MS = 75L
        private const val LOG_BATCH_INTERVAL_MS = 75L
        private const val MAX_LINES_PER_TURN = 64
        private const val SETTING_DEBOUNCE_MS = 350L
        private const val MAX_FIRMWARE_SCAN_TIME_MS = 30_000L
        private const val BT_OFF_RACE_WINDOW_MS = 5_000L
    }

    val connectionState: StateFlow<BleManager.ConnectionState> = ble.connectionState
    val reconnectState: StateFlow<ReconnectUiState> = ble.reconnectState
    val mtu: StateFlow<Int> = ble.mtu

    private val _apList = MutableStateFlow<List<ApItem>>(emptyList())
    val apList: StateFlow<List<ApItem>> = _apList

    private val _status = MutableStateFlow(DeviceStatus())
    val status: StateFlow<DeviceStatus> = _status

    private val _config = MutableStateFlow(DeviceConfig())
    val config: StateFlow<DeviceConfig> = _config

    private val _configSynced = MutableStateFlow(false)
    val configSynced: StateFlow<Boolean> = _configSynced

    private val _savingConfig = MutableStateFlow(false)
    val savingConfig: StateFlow<Boolean> = _savingConfig

    private val _configResetSeq = MutableStateFlow(0L)
    val configResetSeq: StateFlow<Long> = _configResetSeq

    private val _logs = MutableStateFlow<List<SniffLogEntry>>(emptyList())
    val logs: StateFlow<List<SniffLogEntry>> = _logs

    private val _scanning = MutableStateFlow(false)
    val scanning: StateFlow<Boolean> = _scanning

    private val _selected = MutableStateFlow<Set<Int>>(emptySet())
    val selected: StateFlow<Set<Int>> = _selected

    private val _bandFilter = MutableStateFlow(BandFilter.ALL)
    val bandFilter: StateFlow<BandFilter> = _bandFilter

    private val _sortMode = MutableStateFlow(SortMode.RSSI)
    val sortMode: StateFlow<SortMode> = _sortMode

    private val _searchQuery = MutableStateFlow("")
    val searchQuery: StateFlow<String> = _searchQuery

    private val _autoScroll = MutableStateFlow(true)
    val autoScroll: StateFlow<Boolean> = _autoScroll

    private val _toast = MutableStateFlow<String?>(null)
    val toast: StateFlow<String?> = _toast

    private val _bleGatePrompt = MutableStateFlow<BleGatePrompt?>(null)
    val bleGatePrompt: StateFlow<BleGatePrompt?> = _bleGatePrompt

    private val _gateActions = Channel<BleGateAction>(Channel.BUFFERED)
    val gateActions: Flow<BleGateAction> = _gateActions.receiveAsFlow()

    // ---- 供 UI 下沉收集的细粒度派生 StateFlow ----

    val apCount: StateFlow<Int> = _apList
        .map { it.size }
        .distinctUntilChanged()
        .stateIn(viewModelScope, SharingStarted.Eagerly, 0)

    val selectedCount: StateFlow<Int> = _selected
        .map { it.size }
        .distinctUntilChanged()
        .stateIn(viewModelScope, SharingStarted.Eagerly, 0)

    val isAttacking: StateFlow<Boolean> = _status
        .map { it.attacking }
        .distinctUntilChanged()
        .stateIn(viewModelScope, SharingStarted.Eagerly, false)

    val isSniffing: StateFlow<Boolean> = _status
        .map { it.sniffing }
        .distinctUntilChanged()
        .stateIn(viewModelScope, SharingStarted.Eagerly, false)

    val canWriteSettings: StateFlow<Boolean> = combine(
        connectionState,
        _configSynced,
        _savingConfig
    ) { state, synced, saving ->
        state == BleManager.ConnectionState.CONNECTED && synced && !saving
    }.distinctUntilChanged()
        .stateIn(viewModelScope, SharingStarted.Eagerly, false)

    val selectedAps: StateFlow<List<ApItem>> = combine(_apList, _selected) { aps, sel ->
        if (sel.isEmpty()) emptyList() else aps.filter { it.index in sel }
    }.stateIn(viewModelScope, SharingStarted.Eagerly, emptyList())

    val scanListState: StateFlow<ScanListState> = combine(
        _apList,
        _bandFilter,
        _sortMode,
        _searchQuery
    ) { aps, filter, sort, query ->
        computeScanListState(aps, filter, sort, query)
    }.stateIn(viewModelScope, SharingStarted.Eagerly, ScanListState())

    // ---- 主线程独占状态（由单线程 Channel 消费者与主线程方法统一读写，消除 Binder 竞态） ----

    private val nextSniffLogId = AtomicLong(1L)
    private var nextSniffLogSeq = 1L

    private val incomingLineChannel = Channel<String>(Channel.UNLIMITED)
    private var lineConsumerJob: Job? = null

    private val apBackingMap = LinkedHashMap<Int, ApItem>()
    private val pendingSelectedIndices = LinkedHashSet<Int>()
    private var apBatchDirty = false
    private var apFlushJob: Job? = null

    private val logRingBuffer = ArrayDeque<SniffLogEntry>(MAX_SNIFF_LOGS)
    private var logBatchDirty = false
    private var logFlushJob: Job? = null
    private var lastLogFlushMs = monotonicMs() - LOG_BATCH_INTERVAL_MS

    private var inSyncMode = false
    private var syncPreservedBssids: Set<String> = emptySet()
    private var configSyncState = ConfigSyncState()
    private var idSsidReceived = false
    private var idPassReceived = false
    private var idBleReceived = false

    private var scanWatchdogJob: Job? = null
    private var initialSyncProbeJob: Job? = null
    private var configConfirmJob: Job? = null
    private var pendingConfigMatcher: ((JSONObject, DeviceConfig) -> Boolean)? = null
    private var pendingConfigToastProvider: ((DeviceConfig, DeviceStatus) -> String)? = null
    private var pendingClearDraftsOnConfirm: Boolean = false

    private var pendingReconnectOnBtOn = false
    private var _connectionStateSnapshot: BleManager.ConnectionState = ble.connectionState.value
    private var lastActiveDisconnectMs: Long = 0L
    private var userDisconnectedExplicitly: Boolean = false
    @Volatile
    private var onClearedCleanup: (() -> Unit)? = null
    private val settingWriteTimestamps = HashMap<String, Long>()

    private val lineListener: (String) -> Unit = { line ->
        incomingLineChannel.trySend(line)
    }

    private val errorListener: (String) -> Unit = { msg ->
        viewModelScope.launch {
            showToast(msg)
        }
    }

    init {
        ble.onLine = lineListener
        ble.onScanError = errorListener

        lineConsumerJob = viewModelScope.launch {
            for (firstLine in incomingLineChannel) {
                handleLineOnMain(firstLine)
                var drained = 1
                while (drained < MAX_LINES_PER_TURN) {
                    val next = incomingLineChannel.tryReceive().getOrNull() ?: break
                    handleLineOnMain(next)
                    drained++
                }
                if (drained >= MAX_LINES_PER_TURN) {
                    yield()
                }
            }
        }

        viewModelScope.launch {
            ble.connectionState.collect { state ->
                when (state) {
                    BleManager.ConnectionState.DISCONNECTED -> {
                        if (!userDisconnectedExplicitly &&
                            _connectionStateSnapshot != BleManager.ConnectionState.DISCONNECTED
                        ) {
                            lastActiveDisconnectMs = monotonicMs()
                        } else {
                            _connectionStateSnapshot = BleManager.ConnectionState.DISCONNECTED
                            lastActiveDisconnectMs = 0L
                        }
                        scanWatchdogJob?.cancel()
                        scanWatchdogJob = null
                        initialSyncProbeJob?.cancel()
                        initialSyncProbeJob = null
                        flushApBatchNow()
                        clearPendingConfigSave()
                        resetSyncProgress()
                        _scanning.value = false
                        _status.value = DeviceStatus()
                        _config.value = DeviceConfig()
                        _configSynced.value = false
                        // 3.3：断连不递增 configResetSeq，保护未保存的本地编辑草稿
                    }
                    BleManager.ConnectionState.CONNECTING -> {
                        userDisconnectedExplicitly = false
                        _connectionStateSnapshot = state
                        lastActiveDisconnectMs = 0L
                        resetSyncProgress()
                        _configSynced.value = false
                    }
                    BleManager.ConnectionState.CONNECTED -> {
                        userDisconnectedExplicitly = false
                        _connectionStateSnapshot = state
                        lastActiveDisconnectMs = 0L
                        pendingReconnectOnBtOn = false
                        _bleGatePrompt.value = null
                        initialSyncProbeJob?.cancel()
                        initialSyncProbeJob = viewModelScope.launch {
                            delay(1200)
                            if (!_configSynced.value && connectionState.value == BleManager.ConnectionState.CONNECTED) {
                                ble.sendCommand("info")
                            }
                        }
                    }
                    BleManager.ConnectionState.SCANNING -> {
                        userDisconnectedExplicitly = false
                        _connectionStateSnapshot = state
                        lastActiveDisconnectMs = 0L
                        _bleGatePrompt.value = null
                    }
                }
            }
        }
    }

    fun setOnClearedCleanup(cleanup: (() -> Unit)?) {
        onClearedCleanup = cleanup
    }

    override fun onCleared() {
        if (ble.onLine === lineListener) {
            ble.onLine = null
        }
        if (ble.onScanError === errorListener) {
            ble.onScanError = null
        }
        lineConsumerJob?.cancel()
        lineConsumerJob = null
        incomingLineChannel.close()
        _gateActions.close()
        apFlushJob?.cancel()
        apFlushJob = null
        logFlushJob?.cancel()
        logFlushJob = null
        scanWatchdogJob?.cancel()
        scanWatchdogJob = null
        initialSyncProbeJob?.cancel()
        initialSyncProbeJob = null
        configConfirmJob?.cancel()
        configConfirmJob = null
        ble.disconnect(userInitiated = true)
        onClearedCleanup?.invoke()
        onClearedCleanup = null
        super.onCleared()
    }

    // ---- 3.1 统一前置门禁与系统蓝牙状态联动 ----

    fun requestBleConnect(autoTriggered: Boolean = false) {
        if (connectionState.value != BleManager.ConnectionState.DISCONNECTED) {
            return
        }
        _gateActions.trySend(BleGateAction.CheckAndConnect(autoTriggered))
    }

    fun retryOrConnectBle() {
        if (connectionState.value != BleManager.ConnectionState.DISCONNECTED) {
            return
        }
        if (ble.retryReconnect()) {
            _bleGatePrompt.value = null
            return
        }
        requestBleConnect(autoTriggered = false)
    }

    fun disconnectBle() {
        userDisconnectedExplicitly = true
        pendingReconnectOnBtOn = false
        _connectionStateSnapshot = BleManager.ConnectionState.DISCONNECTED
        lastActiveDisconnectMs = 0L
        _bleGatePrompt.value = null
        ble.disconnect(userInitiated = true)
    }

    fun showBleGatePrompt(prompt: BleGatePrompt) {
        _bleGatePrompt.value = prompt
    }

    fun dismissBleGatePrompt() {
        _bleGatePrompt.value = null
    }

    fun confirmBleGatePrompt() {
        val prompt = _bleGatePrompt.value ?: return
        _bleGatePrompt.value = null
        when (prompt) {
            is BleGatePrompt.PermissionRationale ->
                _gateActions.trySend(BleGateAction.LaunchPermissionRequest)
            is BleGatePrompt.PermissionPermanentlyDenied ->
                _gateActions.trySend(BleGateAction.LaunchAppSettings)
            BleGatePrompt.BluetoothDisabled ->
                _gateActions.trySend(BleGateAction.LaunchEnableBluetooth)
            BleGatePrompt.LocationDisabled ->
                _gateActions.trySend(BleGateAction.LaunchLocationSettings)
        }
    }

    fun onBluetoothStateChanged(enabled: Boolean) {
        if (!enabled) {
            val stateBeforeOff = ble.connectionState.value
            val wasReconnecting = reconnectState.value.isReconnecting
            val wasBleActive = ble.onBluetoothAdapterOff()
            val wasSnapshotActive = !userDisconnectedExplicitly && (
                stateBeforeOff != BleManager.ConnectionState.DISCONNECTED ||
                    (_connectionStateSnapshot != BleManager.ConnectionState.DISCONNECTED &&
                        lastActiveDisconnectMs > 0L &&
                        monotonicMs() - lastActiveDisconnectMs <= BT_OFF_RACE_WINDOW_MS)
                )
            if (wasBleActive || wasReconnecting || wasSnapshotActive) {
                val newlyLatched = !pendingReconnectOnBtOn
                pendingReconnectOnBtOn = true
                _connectionStateSnapshot = BleManager.ConnectionState.DISCONNECTED
                lastActiveDisconnectMs = 0L
                if (newlyLatched) {
                    showToast("系统蓝牙已关闭，BLE 连接已断开")
                }
            }
        } else {
            ble.onBluetoothAdapterOn()
            val wasWaitingBtPrompt = (_bleGatePrompt.value == BleGatePrompt.BluetoothDisabled)
            if (wasWaitingBtPrompt) {
                _bleGatePrompt.value = null
            }
            if (pendingReconnectOnBtOn) {
                pendingReconnectOnBtOn = false
                showToast("系统蓝牙已开启，正在重新连接 WiFi-Deck...")
                retryOrConnectBle()
            } else if (wasWaitingBtPrompt) {
                requestBleConnect(autoTriggered = false)
            }
        }
    }

    // ---- 内部同步与合批辅助 ----

    private fun monotonicMs(): Long = System.nanoTime() / 1_000_000L

    private fun resetSyncProgress() {
        inSyncMode = false
        syncPreservedBssids = emptySet()
        configSyncState = ConfigSyncState()
        idSsidReceived = false
        idPassReceived = false
        idBleReceived = false
    }

    private fun startScanWatchdog() {
        scanWatchdogJob?.cancel()
        scanWatchdogJob = viewModelScope.launch {
            val rawScanMs = _config.value.scanTimeMs
            val maxScanMs = if (rawScanMs >= 500L) {
                rawScanMs.coerceAtMost(MAX_FIRMWARE_SCAN_TIME_MS)
            } else {
                MAX_FIRMWARE_SCAN_TIME_MS
            }
            delay(maxOf(15_000L, maxScanMs + 10_000L))
            if (_scanning.value) {
                flushApBatchNow()
                _scanning.value = false
                showToast("扫描超时，未收到开发板返回")
            }
        }
    }

    private fun scheduleApBatchFlush() {
        if (apFlushJob?.isActive == true) return
        apFlushJob = viewModelScope.launch {
            delay(AP_BATCH_INTERVAL_MS)
            apFlushJob = null
            flushApBatchNow()
        }
    }

    private fun flushApBatchNow() {
        apFlushJob?.cancel()
        apFlushJob = null
        if (apBatchDirty) {
            apBatchDirty = false
            _apList.value = apBackingMap.values.toList()
        }
        if (pendingSelectedIndices.isNotEmpty()) {
            val toAdd = pendingSelectedIndices.toSet()
            pendingSelectedIndices.clear()
            _selected.update { it + toAdd }
        }
    }

    private fun scheduleOrFlushLogBatch() {
        val now = monotonicMs()
        val elapsed = now - lastLogFlushMs
        if (elapsed >= LOG_BATCH_INTERVAL_MS && logFlushJob?.isActive != true) {
            flushLogBatchNow(now)
        } else if (logFlushJob?.isActive != true) {
            val waitMs = (LOG_BATCH_INTERVAL_MS - elapsed).coerceIn(10L, LOG_BATCH_INTERVAL_MS)
            logFlushJob = viewModelScope.launch {
                delay(waitMs)
                logFlushJob = null
                flushLogBatchNow(monotonicMs())
            }
        }
    }

    private fun flushLogBatchNow(nowMs: Long = monotonicMs()) {
        logFlushJob?.cancel()
        logFlushJob = null
        if (!logBatchDirty) return
        logBatchDirty = false
        lastLogFlushMs = nowMs
        _logs.value = logRingBuffer.toList()
    }

    private fun handleLineOnMain(line: String) {
        try {
            val obj = JSONObject(line)
            when (BleMessageType.fromWireType(obj.optString("t"))) {
                BleMessageType.SCAN_BEGIN -> {
                    inSyncMode = false
                    syncPreservedBssids = emptySet()
                    apFlushJob?.cancel()
                    apFlushJob = null
                    apBatchDirty = false
                    apBackingMap.clear()
                    pendingSelectedIndices.clear()
                    _apList.value = emptyList()
                    _selected.value = emptySet()
                    _scanning.value = true
                    startScanWatchdog()
                }
                BleMessageType.SYNC_BEGIN -> {
                    // 攻击间隙同步 (scan_between_cycles) 与 results 回读均走 sync_begin/sync_end，绝不干扰主动扫描态
                    inSyncMode = true
                    val curSelected = _selected.value + pendingSelectedIndices
                    syncPreservedBssids = apBackingMap.values
                        .filter { it.index in curSelected }
                        .map { it.bssid }
                        .toSet()
                    apFlushJob?.cancel()
                    apFlushJob = null
                    apBatchDirty = false
                    apBackingMap.clear()
                    pendingSelectedIndices.clear()
                    _apList.value = emptyList()
                    _selected.value = emptySet()
                }
                BleMessageType.AP -> {
                    val itemIndex = obj.optInt("i", -1)
                    if (itemIndex < 0) return
                    val itemBssid = obj.optString("b", "")
                    val isSelectedFlag = obj.optInt("sel", 0) != 0
                    val shouldSelect = isSelectedFlag ||
                        (inSyncMode && itemBssid.isNotEmpty() && itemBssid in syncPreservedBssids)
                    val item = ApItem(
                        index = itemIndex,
                        ssid = obj.optString("s", "(隐藏)"),
                        bssid = itemBssid,
                        channel = obj.optInt("c"),
                        rssi = obj.optInt("r"),
                        selected = shouldSelect
                    )
                    val prev = apBackingMap[item.index]
                    apBackingMap[item.index] = item
                    apBatchDirty = true
                    if (shouldSelect) {
                        pendingSelectedIndices.add(item.index)
                    } else {
                        pendingSelectedIndices.remove(item.index)
                        if (prev?.selected == true) {
                            _selected.update { it - item.index }
                        }
                    }
                    if (_scanning.value && !inSyncMode) {
                        startScanWatchdog()
                    }
                    scheduleApBatchFlush()
                }
                BleMessageType.SCAN_END -> {
                    flushApBatchNow()
                    inSyncMode = false
                    syncPreservedBssids = emptySet()
                    scanWatchdogJob?.cancel()
                    scanWatchdogJob = null
                    _scanning.value = false
                    if (obj.optInt("err", 0) != 0) {
                        showToast("扫描失败，请重试")
                    } else {
                        showToast("扫描完成: ${obj.optInt("n")} 个网络")
                    }
                }
                BleMessageType.SYNC_END -> {
                    flushApBatchNow()
                    inSyncMode = false
                    syncPreservedBssids = emptySet()
                }
                BleMessageType.STATUS -> {
                    _status.value = DeviceStatus(
                        attackEnabled = obj.optInt("atk") != 0,
                        disassocEnabled = obj.optInt("dis") != 0,
                        beaconFlood = obj.optInt("bcn") != 0,
                        probeFlood = obj.optInt("prb") != 0,
                        signalJam = obj.optInt("jam") != 0,
                        deauthSniff = obj.optInt("snf") != 0,
                        sniffing = obj.optInt("snfOn") != 0,
                        sniffMode = obj.optInt("sMode", SniffMode.STOP.code),
                        hopping = obj.optInt("hop") != 0,
                        channel = obj.optInt("ch"),
                        txOk = obj.optLong("ok"),
                        txFail = obj.optLong("fail"),
                        timedRemainingSec = obj.optInt("rem", 0)
                    )
                }
                BleMessageType.CONFIG -> {
                    _config.update {
                        it.copy(
                            cycleDelayMs = obj.optLong("delay"),
                            scanTimeMs = obj.optLong("scan"),
                            numSendFrames = obj.optInt("frames"),
                            startChannel = obj.optInt("ch"),
                            scanBetweenCycles = obj.optInt("sc") != 0,
                            ledEnabled = obj.optInt("led") != 0,
                            apEnabled = obj.optInt("ap", 1) != 0,
                            firmwareVersion = if (obj.has("ver")) {
                                obj.optString("ver", it.firmwareVersion)
                            } else {
                                it.firmwareVersion
                            }
                        )
                    }
                    configSyncState = configSyncState.copy(cfgReceived = true)
                    _configSynced.value = configSyncState.isSynced
                }
                BleMessageType.IDENTITY -> {
                    val updated = _config.value.copy(
                        apSsid = if (obj.has("ssid")) obj.optString("ssid", _config.value.apSsid) else _config.value.apSsid,
                        apPass = if (obj.has("pass")) obj.optString("pass", _config.value.apPass) else _config.value.apPass,
                        bleName = if (obj.has("ble")) obj.optString("ble", _config.value.bleName) else _config.value.bleName
                    )
                    _config.value = updated
                    if (obj.has("ssid")) idSsidReceived = true
                    if (obj.has("pass")) idPassReceived = true
                    if (obj.has("ble")) idBleReceived = true
                    if (idSsidReceived && idPassReceived && idBleReceived) {
                        configSyncState = configSyncState.copy(idReceived = true)
                    }
                    _configSynced.value = configSyncState.isSynced

                    val matcher = pendingConfigMatcher
                    if (_savingConfig.value && matcher != null && matcher(obj, updated)) {
                        val confirmToast = pendingConfigToastProvider?.invoke(updated, _status.value)
                        val clearAll = pendingClearDraftsOnConfirm
                        val jobToCancel = configConfirmJob
                        configConfirmJob = null
                        pendingConfigMatcher = null
                        pendingConfigToastProvider = null
                        pendingClearDraftsOnConfirm = false
                        _savingConfig.value = false
                        jobToCancel?.cancel()
                        if (clearAll) {
                            _configResetSeq.update { it + 1 }
                        }
                        if (confirmToast != null) {
                            showToast(confirmToast)
                        }
                    }
                }
                BleMessageType.LOG -> {
                    val msg = obj.optString("m")
                    val isPacketFrame = msg.startsWith("[MGMT]") ||
                        msg.startsWith("[CTRL]") ||
                        msg.startsWith("[DATA]") ||
                        msg.startsWith("[EXT]")
                    if (isPacketFrame) {
                        val entry = SniffLogEntry.fromRaw(
                            id = nextSniffLogId.getAndIncrement(),
                            seq = nextSniffLogSeq++,
                            rawLog = msg
                        )
                        while (logRingBuffer.size >= MAX_SNIFF_LOGS) {
                            logRingBuffer.removeFirst()
                        }
                        logRingBuffer.addLast(entry)
                        logBatchDirty = true
                        scheduleOrFlushLogBatch()
                    } else if (msg.startsWith("[ERROR]") || msg.startsWith("[WARN]") || msg.startsWith("TX Stats:")) {
                        if (msg.startsWith("[ERROR]") && _savingConfig.value) {
                            clearPendingConfigSave()
                        }
                        showToast(msg)
                    }
                }
                null -> Unit
            }
        } catch (_: Exception) {
            // 忽略无法解析的行
        }
    }

    private fun computeScanListState(
        aps: List<ApItem>,
        filter: BandFilter,
        sort: SortMode,
        query: String
    ): ScanListState {
        var count24 = 0
        var count5 = 0
        for (ap in aps) {
            if (ap.channel < 36) count24++ else count5++
        }
        val q = query.trim().lowercase()
        val filtered = aps.filter { ap ->
            val bandMatch = when (filter) {
                BandFilter.ALL -> true
                BandFilter.G24 -> ap.channel < 36
                BandFilter.G5 -> ap.channel >= 36
            }
            val queryMatch = q.isEmpty() ||
                ap.ssid.lowercase().contains(q) ||
                ap.bssid.lowercase().contains(q)
            bandMatch && queryMatch
        }.sortedWith(
            when (sort) {
                SortMode.RSSI -> compareByDescending<ApItem> { it.rssi }.thenBy { it.index }
                SortMode.CHANNEL -> compareBy<ApItem> { it.channel }.thenByDescending { it.rssi }
            }
        )
        return ScanListState(
            totalCount = aps.size,
            count24G = count24,
            count5G = count5,
            filteredAps = filtered
        )
    }

    fun showToast(msg: String) {
        _toast.value = msg
    }

    fun clearToast() {
        _toast.value = null
    }

    fun toggleSelect(index: Int) {
        val cur = _selected.value
        _selected.value = if (cur.contains(index)) cur - index else cur + index
    }

    fun setBandFilter(f: BandFilter) {
        _bandFilter.value = f
    }

    fun setSortMode(s: SortMode) {
        _sortMode.value = s
    }

    fun setSearchQuery(q: String) {
        _searchQuery.value = q.replace("\r", "").replace("\n", "")
    }

    fun toggleAutoScroll() {
        _autoScroll.value = !_autoScroll.value
    }

    fun clearSelection() {
        pendingSelectedIndices.clear()
        _selected.value = emptySet()
    }

    fun selectAll() {
        flushApBatchNow()
        val filter = _bandFilter.value
        val query = _searchQuery.value.trim().lowercase()
        _selected.value = _apList.value
            .filter { ap ->
                val bandMatch = when (filter) {
                    BandFilter.ALL -> true
                    BandFilter.G24 -> ap.channel < 36
                    BandFilter.G5 -> ap.channel >= 36
                }
                val queryMatch = query.isEmpty() ||
                    ap.ssid.lowercase().contains(query) ||
                    ap.bssid.lowercase().contains(query)
                bandMatch && queryMatch
            }
            .map { it.index }
            .toSet()
    }

    fun clearLogs() {
        logFlushJob?.cancel()
        logFlushJob = null
        logBatchDirty = false
        logRingBuffer.clear()
        nextSniffLogSeq = 1L
        _logs.value = emptyList()
        if (ble.connectionState.value == BleManager.ConnectionState.CONNECTED) {
            if (ble.sendCommand("clear_logs")) {
                showToast("已清空抓包日志并同步至开发板")
            }
        } else {
            showToast("已清空本地抓包日志")
        }
    }

    // ---- 命令发送（复用固件 executeCommand 指令集）----

    fun sendScan() {
        if (ble.connectionState.value != BleManager.ConnectionState.CONNECTED) {
            ble.sendCommand("scan")
            return
        }
        if (_scanning.value) {
            return
        }
        if (!ble.sendCommand("scan")) {
            return
        }
        clearSelection()
        _scanning.value = true
        startScanWatchdog()
    }

    fun sendStartDeauth() {
        if (_status.value.attackEnabled) {
            sendStopAll()
            return
        }
        sendTargetedAttackCommand("start deauther")
    }

    fun sendDisassoc() {
        if (_status.value.disassocEnabled) {
            if (ble.sendCommand("disassoc")) {
                showToast("已关闭 Disassoc 剥离")
            }
            return
        }
        sendTargetedAttackCommand("disassoc")
    }

    fun sendRandomAttack() {
        flushApBatchNow()
        if (_apList.value.isEmpty()) {
            showToast("请先扫描周围 Wi-Fi 网络")
            return
        }
        if (ble.sendCommand("random_attack")) {
            showToast("已触发随机信道抽样测试")
        }
    }

    fun sendStopAll() {
        if (ble.sendEmergencyStop()) {
            showToast("已发送停止指令")
        }
    }

    fun sendTimedAttack(durationMs: Long) {
        if (durationMs <= 0L) {
            showToast("无效的定时攻击时长")
            return
        }
        sendTargetedAttackCommand(
            baseCmd = "attack_time $durationMs",
            successToast = "已启动 ${durationMs / 1000} 秒定时测试"
        )
    }

    fun sendBeaconFlood() {
        ble.sendCommand("beacon_flood")
    }

    fun sendProbeFlood() {
        ble.sendCommand("probe_flood")
    }

    fun sendSignalJam() {
        ble.sendCommand("signal_jam")
    }

    fun sendDeauthSniff() {
        ble.sendCommand("deauth_sniff")
    }

    fun sendSingleAp() {
        val first = _selected.value.minOrNull()
        if (first == null) {
            showToast("请先在扫描页选择一个目标")
            return
        }
        if (ble.sendCommand("single_ap $first")) {
            showToast("已对目标 #$first 突发测试帧")
        }
    }

    fun sendResetTxStats() {
        if (ble.sendCommand("reset_txstats")) {
            showToast("TX 统计计数已清零")
        }
    }

    fun sendStartSniff() {
        if (_status.value.sniffing) {
            sendStopSniff()
            return
        }
        ble.sendCommand("start sniff")
    }

    fun sendStopSniff() {
        ble.sendCommand("stop sniff")
    }

    fun sendSniffMode(mode: String) {
        ble.sendCommand("sniff $mode")
    }

    fun sendHopToggle(on: Boolean) {
        ble.sendCommand(if (on) "hop on" else "hop off")
    }

    fun sendSetChannel(ch: Int) {
        if (ble.connectionState.value != BleManager.ConnectionState.CONNECTED) {
            ble.sendCommand("set ch $ch")
            return
        }
        if (!_status.value.sniffing && _config.value.apEnabled) {
            showToast("热点开启中不可直接锁频，请先开启抓包或关闭热点")
            return
        }
        if (ble.sendCommand("set ch $ch")) {
            showToast("已锁定信道 CH $ch")
        }
    }

    private fun sendDebouncedSettingCommand(settingKey: String, cmd: String): Boolean {
        if (ble.connectionState.value != BleManager.ConnectionState.CONNECTED) {
            ble.sendCommand(cmd)
            return false
        }
        if (!_configSynced.value) {
            showToast("正在同步设备配置，请稍候")
            return false
        }
        if (_savingConfig.value) {
            showToast("正在写入配置，请稍候")
            return false
        }
        val now = monotonicMs()
        val last = settingWriteTimestamps[settingKey] ?: 0L
        if (now - last < SETTING_DEBOUNCE_MS) {
            return false
        }
        settingWriteTimestamps[settingKey] = now
        return ble.sendCommand(cmd)
    }

    fun sendLedToggle(on: Boolean) {
        sendDebouncedSettingCommand("led", if (on) "set led on" else "set led off")
    }

    fun sendApToggle(on: Boolean) {
        sendDebouncedSettingCommand("ap", if (on) "ap on" else "ap off")
    }

    fun sendSetFrames(n: Int) {
        sendDebouncedSettingCommand("num_frames", "set num_frames $n")
    }

    fun sendSetCycleDelay(ms: Long) {
        sendDebouncedSettingCommand("cycle_delay", "set cycle_delay $ms")
    }

    fun sendSetScanTime(ms: Long) {
        sendDebouncedSettingCommand("scan_time", "set scan_time $ms")
    }

    fun sendSetStartChannel(ch: Int) {
        sendDebouncedSettingCommand("start_channel", "set start_channel $ch")
    }

    fun sendSetScanCycles(on: Boolean) {
        sendDebouncedSettingCommand("scan_cycles", if (on) "set scan_cycles on" else "set scan_cycles off")
    }

    private fun clearPendingConfigSave() {
        val jobToCancel = configConfirmJob
        configConfirmJob = null
        pendingConfigMatcher = null
        pendingConfigToastProvider = null
        pendingClearDraftsOnConfirm = false
        _savingConfig.value = false
        jobToCancel?.cancel()
    }

    private fun submitIdentityUpdate(
        cmd: String,
        timeoutToast: String,
        clearAllDraftsOnConfirm: Boolean = false,
        confirmToastProvider: (DeviceConfig, DeviceStatus) -> String,
        matcher: (JSONObject, DeviceConfig) -> Boolean
    ) {
        if (ble.connectionState.value != BleManager.ConnectionState.CONNECTED) {
            ble.sendCommand(cmd)
            return
        }
        if (_savingConfig.value) {
            showToast("正在写入配置，请稍候")
            return
        }
        configConfirmJob?.cancel()
        configConfirmJob = null
        pendingConfigMatcher = matcher
        pendingConfigToastProvider = confirmToastProvider
        pendingClearDraftsOnConfirm = clearAllDraftsOnConfirm
        _savingConfig.value = true

        if (!ble.sendCommand(cmd)) {
            clearPendingConfigSave()
            return
        }

        val newJob = viewModelScope.launch {
            delay(1500)
            if (_savingConfig.value && connectionState.value == BleManager.ConnectionState.CONNECTED) {
                ble.sendCommand("info")
            }
            delay(1800)
            if (_savingConfig.value) {
                clearPendingConfigSave()
                showToast(timeoutToast)
            }
        }
        if (_savingConfig.value) {
            configConfirmJob = newJob
        } else {
            newJob.cancel()
        }
    }

    fun refreshDeviceInfo(userTriggered: Boolean = false) {
        if (ble.connectionState.value != BleManager.ConnectionState.CONNECTED) {
            if (userTriggered) {
                showToast("设备未连接，请先连接 WiFi-Deck")
            }
            return
        }
        if (ble.sendCommand("info") && userTriggered) {
            showToast("正在从开发板同步最新配置...")
        }
    }

    fun sendSetBleName(name: String) {
        val trimmed = name.replace("\r", "").replace("\n", "").trim()
        val byteLen = trimmed.toByteArray(Charsets.UTF_8).size
        if (byteLen !in 1..26) {
            showToast("BLE 蓝牙名称长度需为 1~26 字节")
            return
        }
        submitIdentityUpdate(
            cmd = "set ble_name $trimmed",
            timeoutToast = "BLE 名称保存超时，请刷新核对",
            confirmToastProvider = { _, _ -> "BLE 名称已确认写入: $trimmed (重连后广播生效)" }
        ) { obj, cfg -> obj.has("ble") && cfg.bleName == trimmed }
    }

    fun sendSetApSsid(ssid: String) {
        val trimmed = ssid.replace("\r", "").replace("\n", "").trim()
        val byteLen = trimmed.toByteArray(Charsets.UTF_8).size
        if (byteLen !in 1..32) {
            showToast("Wi-Fi 热点名称长度需为 1~32 字节")
            return
        }
        submitIdentityUpdate(
            cmd = "set ap_ssid $trimmed",
            timeoutToast = "热点名称保存超时，请刷新核对",
            confirmToastProvider = { cfg, st ->
                when {
                    !cfg.apEnabled -> "热点名称已确认写入: $trimmed（隐身模式已保存，开启热点后生效）"
                    st.isSniffing || st.deauthSniff -> "热点名称已确认写入: $trimmed（抓包中已保存，停止抓包恢复热点后生效）"
                    else -> "热点名称已确认写入: $trimmed，正重启热点"
                }
            }
        ) { obj, cfg -> obj.has("ssid") && cfg.apSsid == trimmed }
    }

    fun sendSetApPass(pass: String) {
        val trimmed = pass.replace("\r", "").replace("\n", "").trim()
        val byteLen = trimmed.toByteArray(Charsets.UTF_8).size
        if (byteLen !in 8..63) {
            showToast("WPA2 热点密码长度需为 8~63 字符/字节")
            return
        }
        submitIdentityUpdate(
            cmd = "set ap_pass $trimmed",
            timeoutToast = "热点密码保存超时，请刷新核对",
            confirmToastProvider = { cfg, st ->
                when {
                    !cfg.apEnabled -> "热点密码已确认写入（隐身模式已保存，开启热点后生效）"
                    st.isSniffing || st.deauthSniff -> "热点密码已确认写入（抓包中已保存，停止抓包恢复热点后生效）"
                    else -> "热点密码已确认写入并重启热点"
                }
            }
        ) { obj, cfg -> obj.has("pass") && cfg.apPass == trimmed }
    }

    fun sendResetConfig() {
        submitIdentityUpdate(
            cmd = "reset_config",
            timeoutToast = "恢复默认超时，请刷新核对",
            clearAllDraftsOnConfirm = true,
            confirmToastProvider = { _, _ -> "已确认恢复出厂默认名称与参数" }
        ) { obj, cfg ->
            obj.has("pass") &&
                cfg.apSsid == "WiFi-Deck" &&
                cfg.apPass == "12345678" &&
                cfg.bleName == "WiFi-Deck"
        }
    }

    /** 原子下发带目标索引（或显式全量 all）的攻击指令，禁止无目标时回退全攻 */
    private fun sendTargetedAttackCommand(
        baseCmd: String,
        successToast: String? = null
    ): Boolean {
        flushApBatchNow()
        val selectedIndices = _selected.value
        if (selectedIndices.isEmpty()) {
            showToast("请先在扫描页选择至少一个目标")
            return false
        }
        if (ble.connectionState.value != BleManager.ConnectionState.CONNECTED) {
            ble.sendCommand(baseCmd)
            return false
        }
        val apSnapshot = _apList.value
        val validSelected = if (apSnapshot.isNotEmpty()) {
            val validSet = apSnapshot.map { it.index }.toHashSet()
            selectedIndices.filter { it in validSet }
        } else {
            selectedIndices.filter { it >= 0 }
        }
        if (validSelected.isEmpty()) {
            showToast("请先在扫描页选择至少一个目标")
            return false
        }
        val allExplicitlySelected = apSnapshot.size > 1 && apSnapshot.all { it.index in validSelected }
        val ok = if (allExplicitlySelected) {
            ble.sendCommand("$baseCmd all")
        } else {
            ble.sendTargetedCommand(baseCmd, validSelected)
        }
        if (ok && successToast != null) {
            showToast(successToast)
        }
        return ok
    }
}
