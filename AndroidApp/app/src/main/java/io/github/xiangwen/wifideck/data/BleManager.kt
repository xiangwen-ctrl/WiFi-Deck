package io.github.xiangwen.wifideck.data

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.content.pm.PackageManager
import android.location.LocationManager
import android.os.Build
import android.os.ParcelUuid
import android.os.SystemClock
import android.util.Log
import androidx.core.content.ContextCompat
import androidx.core.location.LocationManagerCompat
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull
import java.util.UUID

/**
 * BLE 管理器：扫描 -> 连接 -> MTU 协商 -> 服务发现 -> 订阅 Notify。
 * 所有 BluetoothGatt 操作都是异步且不可并发的，必须严格按链式回调顺序执行。
 */
object BleManager : BleTransport {
    private const val TAG = "BleManager"

    val SERVICE_UUID: UUID = UUID.fromString("6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
    val CHAR_RX_UUID: UUID = UUID.fromString("6E400002-B5A3-F393-E0A9-E50E24DCCA9E")
    val CHAR_TX_UUID: UUID = UUID.fromString("6E400003-B5A3-F393-E0A9-E50E24DCCA9E")

    private val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

    private const val MAX_CMD_QUEUE_SIZE = WriteAckTracker.MAX_CMD_QUEUE_SIZE
    private const val GATT_CLOSE_TIMEOUT_MS = 800L
    private const val MAX_WRITE_RETRIES = 8
    private const val MAX_URGENT_WRITE_RETRIES = 10
    private const val WRITE_ACK_TIMEOUT_MS = 90L
    private const val MIN_WRITE_INTERVAL_MS = 25L

    enum class ConnectionState { DISCONNECTED, SCANNING, CONNECTING, CONNECTED }

    private class PendingGattClose(
        val timeoutJob: Job,
        val completed: CompletableDeferred<Unit> = CompletableDeferred()
    )

    private val _connectionState = MutableStateFlow(ConnectionState.DISCONNECTED)
    override val connectionState: StateFlow<ConnectionState> = _connectionState

    private val reconnectTracker = BleReconnectTracker()
    private val _reconnectState = MutableStateFlow(ReconnectUiState())
    override val reconnectState: StateFlow<ReconnectUiState> = _reconnectState

    private val _mtu = MutableStateFlow(23)
    override val mtu: StateFlow<Int> = _mtu

    /** 收到一条完整 JSON 行（已去掉末尾 \n）时回调 */
    @Volatile
    override var onLine: ((String) -> Unit)? = null

    /** 扫描/连接/写入出错时回调（用于 UI Toast 提示） */
    @Volatile
    override var onScanError: ((String) -> Unit)? = null

    private var appContext: Context? = null
    private var bluetoothAdapter: BluetoothAdapter? = null
    private val gattLock = Any()
    private val scanLock = Any()
    private var scanActive = false
    private var scanGeneration = 0L
    @Volatile private var gatt: BluetoothGatt? = null
    @Volatile private var txCharacteristic: BluetoothGattCharacteristic? = null
    @Volatile private var rxCharacteristic: BluetoothGattCharacteristic? = null
    private val pendingCloseGatts = HashMap<BluetoothGatt, PendingGattClose>()
    private var lastDevice: BluetoothDevice? = null
    @Volatile private var userInitiatedDisconnect = false
    @Volatile private var btDisabledBySystem = false
    private var reconnectJob: Job? = null
    private var connectJob: Job? = null
    private var scanStartJob: Job? = null

    private val rxLock = Any()
    private val rxFramer = BleLineFramer(BleLineFramer.DEFAULT_MAX_LINE_BYTES)

    // 串行命令发送队列：有界容量 + 急停优先通道 + writeCharacteristic 返回值校验与背压重试。
    private val scope = CoroutineScope(Dispatchers.IO + SupervisorJob())
    private val cmdQueue = ArrayDeque<String>()
    private val writeLock = Any()
    private val writeAckTracker = WriteAckTracker()
    private var activeWriteSeq: Long = 0L
    private var pendingEmergencyStop: String? = null
    private var pendingWriteAck: CompletableDeferred<Int>? = null
    private var urgentWakeSignal: CompletableDeferred<Unit>? = null
    @Volatile private var writeSession = 0L
    private var writeDrainJob: Job? = null
    private var scanTimeoutJob: Job? = null
    private var connectTimeoutJob: Job? = null
    private var mtuSetupJob: Job? = null
    private var mtuFallbackJob: Job? = null
    @Volatile private var discoveryStarted = false
    @Volatile private var retriedDiscovery = false
    @Volatile private var handshakeCompleted = false
    @Volatile private var physicalConnected = false
    @Volatile private var currentAttemptWasDirect = false
    @Volatile private var currentAttemptWasManualRetryDirect = false

    /** 返回当前有效 ATT MTU 下单次写入允许的最大指令 UTF-8 字节数（始终受 (MTU - 3) 约束） */
    fun maxCommandPayloadBytes(): Int = BleCommandCodec.maxCommandPayloadBytes(_mtu.value)

    fun init(context: Context) {
        val app = context.applicationContext
        appContext = app
        val manager = app.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
        bluetoothAdapter = manager?.adapter
    }

    fun isDeviceSupported(): Boolean = bluetoothAdapter != null

    fun isBluetoothEnabled(): Boolean = bluetoothAdapter?.isEnabled == true

    fun isLocationReadyForScan(): Boolean {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) return true
        val ctx = appContext ?: return false
        val lm = ctx.getSystemService(Context.LOCATION_SERVICE) as? LocationManager ?: return false
        return LocationManagerCompat.isLocationEnabled(lm)
    }

    private fun hasBlePermissions(requireScan: Boolean = true): Boolean {
        val ctx = appContext ?: return false
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            val connectOk = ContextCompat.checkSelfPermission(
                ctx,
                Manifest.permission.BLUETOOTH_CONNECT
            ) == PackageManager.PERMISSION_GRANTED
            val scanOk = !requireScan || ContextCompat.checkSelfPermission(
                ctx,
                Manifest.permission.BLUETOOTH_SCAN
            ) == PackageManager.PERMISSION_GRANTED
            connectOk && scanOk
        } else {
            !requireScan || ContextCompat.checkSelfPermission(
                ctx,
                Manifest.permission.ACCESS_FINE_LOCATION
            ) == PackageManager.PERMISSION_GRANTED
        }
    }

    /** 手动重试重连：清除 EXHAUSTED 状态，有缓存设备时发起直连，否则回退扫描；蓝牙关闭或缺权限时返回 false */
    override fun retryReconnect(): Boolean {
        val adapter = bluetoothAdapter ?: return false
        if (!adapter.isEnabled || !hasBlePermissions(requireScan = true)) {
            return false
        }
        val currentState = _connectionState.value
        if (currentState == ConnectionState.CONNECTED ||
            currentState == ConnectionState.CONNECTING ||
            currentState == ConnectionState.SCANNING
        ) {
            return true
        }
        val cached = lastDevice
        if (cached == null && !isLocationReadyForScan()) {
            return false
        }
        btDisabledBySystem = false
        userInitiatedDisconnect = false
        _reconnectState.value = reconnectTracker.onManualRetry(hasCachedDevice = cached != null)
        if (cached != null) {
            connect(cached, fromReconnect = true, fromScan = false, fromManualRetry = true)
        } else {
            startScanInternal(fromReconnect = false)
        }
        return true
    }

    fun startScan() {
        startScanInternal(fromReconnect = false)
    }

    private fun startScanInternal(fromReconnect: Boolean) {
        if (fromReconnect && (userInitiatedDisconnect || btDisabledBySystem || !reconnectTracker.shouldAutoReconnect)) {
            return
        }
        val adapter = bluetoothAdapter
        if (adapter == null) {
            if (fromReconnect) {
                _reconnectState.value = reconnectTracker.exhaustNow("当前设备不支持 BLE 或蓝牙不可用")
            }
            _connectionState.value = ConnectionState.DISCONNECTED
            onScanError?.invoke("当前设备不支持 BLE 或蓝牙不可用")
            return
        }
        if (!adapter.isEnabled) {
            if (fromReconnect) {
                reconnectTracker.onDisconnectWhileBtOff(wasActiveSession = true)
                _reconnectState.value = reconnectTracker.uiState
            }
            _connectionState.value = ConnectionState.DISCONNECTED
            if (!fromReconnect) {
                onScanError?.invoke("请先打开蓝牙")
            }
            return
        }
        if (!isLocationReadyForScan()) {
            val msg = "Android 8~11 扫描蓝牙设备需开启系统定位开关"
            if (fromReconnect) {
                _reconnectState.value = reconnectTracker.exhaustNow(msg)
            }
            _connectionState.value = ConnectionState.DISCONNECTED
            onScanError?.invoke(msg)
            return
        }
        val scanner = adapter.bluetoothLeScanner
        if (scanner == null) {
            if (fromReconnect) {
                _reconnectState.value = reconnectTracker.exhaustNow("当前设备不支持 BLE 或蓝牙不可用")
            }
            _connectionState.value = ConnectionState.DISCONNECTED
            onScanError?.invoke("当前设备不支持 BLE 或蓝牙不可用")
            return
        }

        // 先清理上一次可能残留的扫描/握手状态与 GATT 句柄
        stopScan()
        connectJob?.cancel()
        connectJob = null
        if (!fromReconnect) {
            btDisabledBySystem = false
            userInitiatedDisconnect = false
            reconnectJob?.cancel()
            reconnectJob = null
            _reconnectState.value = reconnectTracker.onSessionAbandoned()
        }
        cancelHandshakeJobs()
        resetWriteQueue()
        resetRxBuffer()
        _mtu.value = 23
        closeCurrentGatt(alreadyDisconnected = false)
        handshakeCompleted = false
        physicalConnected = false
        discoveryStarted = false
        retriedDiscovery = false
        currentAttemptWasDirect = false
        currentAttemptWasManualRetryDirect = false

        _connectionState.value = ConnectionState.SCANNING

        val filter = ScanFilter.Builder()
            .setServiceUuid(ParcelUuid(SERVICE_UUID))
            .build()
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        scanStartJob?.cancel()
        val startJob = scope.launch(start = CoroutineStart.LAZY) {
            val myJob = currentCoroutineContext()[Job]
            awaitPendingGattClose(GATT_CLOSE_TIMEOUT_MS)
            val myScanGen = synchronized(scanLock) {
                if (!isActive ||
                    scanStartJob !== myJob ||
                    userInitiatedDisconnect ||
                    btDisabledBySystem ||
                    (fromReconnect && !reconnectTracker.shouldAutoReconnect) ||
                    _connectionState.value != ConnectionState.SCANNING
                ) {
                    null
                } else {
                    scanActive = true
                    ++scanGeneration
                }
            } ?: return@launch
            try {
                @SuppressLint("MissingPermission")
                scanner.startScan(listOf(filter), settings, scanCallback)
            } catch (e: SecurityException) {
                synchronized(scanLock) {
                    if (scanGeneration == myScanGen) {
                        scanActive = false
                        scanGeneration++
                    }
                }
                val msg = "缺少蓝牙权限，请在系统设置中授权"
                if (fromReconnect) {
                    _reconnectState.value = reconnectTracker.exhaustNow(msg)
                }
                _connectionState.value = ConnectionState.DISCONNECTED
                onScanError?.invoke(msg)
                return@launch
            } catch (e: Exception) {
                synchronized(scanLock) {
                    if (scanGeneration == myScanGen) {
                        scanActive = false
                        scanGeneration++
                    }
                }
                Log.e(TAG, "startScan failed: ${e.message}")
                if (fromReconnect || reconnectTracker.shouldAutoReconnect) {
                    handleReconnectFailure(
                        status = -1,
                        alreadyDisconnected = true,
                        errorMessage = "启动 BLE 扫描失败",
                        wasDirectAttempt = false,
                        wasManualRetryDirect = false
                    )
                } else {
                    _connectionState.value = ConnectionState.DISCONNECTED
                    onScanError?.invoke("启动 BLE 扫描失败")
                }
                return@launch
            }

            // 10 秒扫描超时：没扫到设备自动停止，避免永久卡在 SCANNING
            scanTimeoutJob?.cancel()
            val timeoutJob = scope.launch(start = CoroutineStart.LAZY) {
                val myTimeoutJob = currentCoroutineContext()[Job]
                delay(BleReconnectTracker.SCAN_TIMEOUT_MS)
                val timedOut = synchronized(scanLock) {
                    if (isActive &&
                        scanTimeoutJob === myTimeoutJob &&
                        scanActive &&
                        scanGeneration == myScanGen &&
                        _connectionState.value == ConnectionState.SCANNING
                    ) {
                        scanActive = false
                        scanGeneration++
                        true
                    } else {
                        false
                    }
                }
                if (!timedOut) return@launch
                stopScan()
                if (reconnectTracker.shouldAutoReconnect && !userInitiatedDisconnect && !btDisabledBySystem) {
                    handleReconnectFailure(
                        status = -1,
                        alreadyDisconnected = true,
                        errorMessage = "重连扫描超时，未找到设备",
                        wasDirectAttempt = false,
                        wasManualRetryDirect = false
                    )
                } else {
                    _connectionState.value = ConnectionState.DISCONNECTED
                    onScanError?.invoke("扫描超时，未找到设备（请确认板子已上电且未被系统蓝牙占用）")
                }
            }
            val stillScanning = synchronized(scanLock) {
                if (isActive && scanActive && scanGeneration == myScanGen && _connectionState.value == ConnectionState.SCANNING) {
                    scanTimeoutJob = timeoutJob
                    true
                } else {
                    false
                }
            }
            if (stillScanning) {
                timeoutJob.start()
            } else {
                timeoutJob.cancel()
            }
        }
        scanStartJob = startJob
        startJob.start()
    }

    fun stopScan() {
        synchronized(scanLock) {
            scanActive = false
            scanGeneration++
        }
        scanStartJob?.cancel()
        scanStartJob = null
        scanTimeoutJob?.cancel()
        scanTimeoutJob = null
        try {
            @SuppressLint("MissingPermission")
            bluetoothAdapter?.bluetoothLeScanner?.stopScan(scanCallback)
        } catch (_: Exception) {
        }
    }

    private fun cancelHandshakeJobs() {
        val timeoutToCancel = synchronized(gattLock) {
            connectTimeoutJob.also { connectTimeoutJob = null }
        }
        timeoutToCancel?.cancel()
        mtuSetupJob?.cancel()
        mtuSetupJob = null
        mtuFallbackJob?.cancel()
        mtuFallbackJob = null
    }

    private sealed class WriteAttemptResult {
        data object Accepted : WriteAttemptResult()
        data object Busy : WriteAttemptResult()
        data class Failed(val reason: String, val fatal: Boolean) : WriteAttemptResult()
    }

    private fun resetWriteQueue() {
        synchronized(writeLock) {
            writeSession++
            writeAckTracker.reset()
            activeWriteSeq = 0L
            writeDrainJob?.cancel()
            writeDrainJob = null
            pendingWriteAck?.cancel()
            pendingWriteAck = null
            urgentWakeSignal?.cancel()
            urgentWakeSignal = null
            pendingEmergencyStop = null
            cmdQueue.clear()
        }
    }

    private fun resetRxBuffer() {
        synchronized(rxLock) {
            rxFramer.reset()
        }
    }

    private fun isHandshakeTimerActive(): Boolean = synchronized(gattLock) {
        connectTimeoutJob?.isActive == true
    }

    @SuppressLint("MissingPermission")
    private fun closeCurrentGatt(alreadyDisconnected: Boolean = false) {
        claimAndCloseActiveGatt(expectedGatt = null, alreadyDisconnected = alreadyDisconnected)
    }

    /**
     * 原子认领活跃 GATT 句柄并在需要时登记优雅关闭流程，避免 timeout 与 STATE_DISCONNECTED 并发重复清理/重连。
     * @return 当 expectedGatt 非空但当前活跃句柄已变更时返回 false；其余情况返回 true。
     */
    @SuppressLint("MissingPermission")
    private fun claimAndCloseActiveGatt(
        expectedGatt: BluetoothGatt? = null,
        alreadyDisconnected: Boolean = false
    ): Boolean {
        var claimed: BluetoothGatt? = null
        var timeoutToCancel: Job? = null
        var oldPending: PendingGattClose? = null
        lateinit var pendingEntry: PendingGattClose
        val lazyCloseTimeoutJob = if (!alreadyDisconnected) {
            scope.launch(start = CoroutineStart.LAZY) {
                delay(GATT_CLOSE_TIMEOUT_MS)
                val target = claimed ?: return@launch
                val removed = synchronized(gattLock) {
                    if (pendingCloseGatts[target] === pendingEntry) {
                        pendingCloseGatts.remove(target)
                    } else {
                        null
                    }
                }
                if (removed != null) {
                    Log.w(TAG, "GATT disconnect callback timeout (${GATT_CLOSE_TIMEOUT_MS}ms), closing GATT")
                    try {
                        target.close()
                    } catch (_: Exception) {
                    }
                    removed.completed.complete(Unit)
                }
            }
        } else {
            null
        }
        if (lazyCloseTimeoutJob != null) {
            pendingEntry = PendingGattClose(lazyCloseTimeoutJob)
        }

        val matched = synchronized(gattLock) {
            val current = gatt
            if (expectedGatt != null && current !== expectedGatt) {
                false
            } else {
                claimed = current
                gatt = null
                txCharacteristic = null
                rxCharacteristic = null
                physicalConnected = false
                timeoutToCancel = connectTimeoutJob
                connectTimeoutJob = null
                if (current != null && lazyCloseTimeoutJob != null) {
                    oldPending = pendingCloseGatts.remove(current)
                    pendingCloseGatts[current] = pendingEntry
                }
                true
            }
        }

        timeoutToCancel?.cancel()
        if (!matched) {
            lazyCloseTimeoutJob?.cancel()
            return false
        }

        if (oldPending != null) {
            oldPending!!.timeoutJob.cancel()
            oldPending!!.completed.complete(Unit)
        }

        val target = claimed
        if (target == null) {
            lazyCloseTimeoutJob?.cancel()
            return true
        }

        if (alreadyDisconnected) {
            lazyCloseTimeoutJob?.cancel()
            try {
                target.close()
            } catch (_: Exception) {
            }
            return true
        }

        lazyCloseTimeoutJob?.start()
        val disconnectOk = try {
            target.disconnect()
            true
        } catch (_: Exception) {
            false
        }

        if (!disconnectOk) {
            val removed = synchronized(gattLock) {
                if (pendingCloseGatts[target] === pendingEntry) {
                    pendingCloseGatts.remove(target)
                } else {
                    null
                }
            }
            if (removed != null) {
                removed.timeoutJob.cancel()
                try {
                    target.close()
                } catch (_: Exception) {
                }
                removed.completed.complete(Unit)
            }
        }
        return true
    }

    /** 挂起等待正在关闭的旧 GATT 回调或 800ms 超时完成，避免并发 connectGatt / startScan 争抢唯一硬件槽位 */
    suspend fun awaitPendingGattClose(timeoutMs: Long = GATT_CLOSE_TIMEOUT_MS) {
        val pendingDeferreds = synchronized(gattLock) {
            pendingCloseGatts.values.map { it.completed }
        }
        if (pendingDeferreds.isEmpty()) return
        withTimeoutOrNull(timeoutMs) {
            for (deferred in pendingDeferreds) {
                deferred.await()
            }
        }
    }

    override fun disconnect(userInitiated: Boolean) {
        btDisabledBySystem = false
        performDisconnect(userInitiated = userInitiated, alreadyDisconnected = false)
    }

    /** 系统蓝牙关闭广播到来时立即清理连接并阻断无意义的自动重连；返回关闭前是否处于连接/扫描/重连活动态 */
    override fun onBluetoothAdapterOff(): Boolean {
        btDisabledBySystem = true
        val currentActive = _connectionState.value != ConnectionState.DISCONNECTED
        val wasActive = reconnectTracker.onBluetoothAdapterOff(currentActive)
        _reconnectState.value = reconnectTracker.uiState
        reconnectJob?.cancel()
        reconnectJob = null
        performDisconnect(
            userInitiated = false,
            alreadyDisconnected = (bluetoothAdapter?.isEnabled != true),
            resetTracker = false
        )
        return wasActive
    }

    override fun onBluetoothAdapterOn() {
        btDisabledBySystem = false
        _reconnectState.value = reconnectTracker.onBluetoothAdapterOn()
    }

    private fun cleanupConnectionResources(alreadyDisconnected: Boolean) {
        stopScan()
        connectJob?.cancel()
        connectJob = null
        cancelHandshakeJobs()
        handshakeCompleted = false
        physicalConnected = false
        discoveryStarted = false
        retriedDiscovery = false
        currentAttemptWasDirect = false
        currentAttemptWasManualRetryDirect = false
        _mtu.value = 23
        resetWriteQueue()
        // 清空残余半包，避免断线重连后第一条消息解析失败
        resetRxBuffer()
        closeCurrentGatt(alreadyDisconnected = alreadyDisconnected)
    }

    private fun performDisconnect(
        userInitiated: Boolean,
        alreadyDisconnected: Boolean,
        resetTracker: Boolean = true
    ) {
        if (userInitiated) {
            userInitiatedDisconnect = true
            lastDevice = null
            reconnectJob?.cancel()
            reconnectJob = null
            _reconnectState.value = reconnectTracker.onUserDisconnect()
        } else if (resetTracker) {
            reconnectJob?.cancel()
            reconnectJob = null
            _reconnectState.value = reconnectTracker.onSessionAbandoned()
        }
        cleanupConnectionResources(alreadyDisconnected = alreadyDisconnected)
        _connectionState.value = ConnectionState.DISCONNECTED
    }

    /** 将命令入队并触发串行写出（写到 RX 特征值 6E400002，手机 -> 板子）；未连接时返回 false */
    override fun sendCommand(cmd: String): Boolean {
        if (_connectionState.value != ConnectionState.CONNECTED) {
            onScanError?.invoke("设备未连接，请先连接 WiFi-Deck")
            return false
        }
        return when (val prepared = BleCommandCodec.prepareCommand(cmd, _mtu.value)) {
            BleCommandCodec.PreparedCommand.Empty -> false
            is BleCommandCodec.PreparedCommand.Rejected -> {
                onScanError?.invoke(prepared.reason)
                false
            }
            is BleCommandCodec.PreparedCommand.Ready ->
                enqueueCommand(prepared.command, urgent = prepared.urgent)
        }
    }

    /** 急停专用优先通道：绕过普通队列上限并清除尚未下发的普通待发指令 */
    override fun sendEmergencyStop(): Boolean {
        if (_connectionState.value != ConnectionState.CONNECTED) {
            onScanError?.invoke("设备未连接，请先连接 WiFi-Deck")
            return false
        }
        return enqueueCommand("stop", urgent = true)
    }

    /** 发送带目标索引列表的原子指令（如 start deauther <i,j,k> / disassoc <i,j,k> / attack_time <ms> <i,j,k>） */
    override fun sendTargetedCommand(baseCmd: String, indices: Collection<Int>): Boolean {
        if (_connectionState.value != ConnectionState.CONNECTED) {
            onScanError?.invoke("设备未连接，请先连接 WiFi-Deck")
            return false
        }
        return when (val prepared = BleCommandCodec.prepareTargetedCommand(baseCmd, indices, _mtu.value)) {
            BleCommandCodec.PreparedCommand.Empty -> false
            is BleCommandCodec.PreparedCommand.Rejected -> {
                onScanError?.invoke(prepared.reason)
                false
            }
            is BleCommandCodec.PreparedCommand.Ready ->
                enqueueCommand(prepared.command, urgent = false)
        }
    }

    private fun enqueueCommand(cmd: String, urgent: Boolean): Boolean {
        var disconnected = false
        val enqueued = synchronized(writeLock) {
            if (gatt == null || rxCharacteristic == null || _connectionState.value != ConnectionState.CONNECTED) {
                disconnected = true
                false
            } else if (urgent) {
                cmdQueue.clear()
                pendingEmergencyStop = cmd
                urgentWakeSignal?.complete(Unit)
                urgentWakeSignal = null
                ensureDrainLoopLocked()
                true
            } else if (cmdQueue.size >= MAX_CMD_QUEUE_SIZE) {
                false
            } else {
                cmdQueue.addLast(cmd)
                ensureDrainLoopLocked()
                true
            }
        }
        if (!enqueued) {
            onScanError?.invoke(
                if (disconnected) "设备未连接，请先连接 WiFi-Deck"
                else "指令队列已满 (${MAX_CMD_QUEUE_SIZE})，请稍后重试"
            )
        }
        return enqueued
    }

    private fun ensureDrainLoopLocked() {
        if (writeDrainJob?.isActive == true) return
        val expectedSession = writeSession
        writeDrainJob = scope.launch {
            drainWriteQueue(expectedSession)
        }
    }

    private suspend fun delayInterruptibleForUrgent(ms: Long, expectedSession: Long) {
        if (ms <= 0L) return
        val signal = synchronized(writeLock) {
            if (writeSession != expectedSession || pendingEmergencyStop != null) {
                null
            } else {
                CompletableDeferred<Unit>().also { urgentWakeSignal = it }
            }
        } ?: return
        withTimeoutOrNull(ms) {
            signal.await()
        }
        synchronized(writeLock) {
            if (urgentWakeSignal === signal) {
                urgentWakeSignal = null
            }
        }
    }

    @SuppressLint("MissingPermission")
    private fun writeCharacteristicCompat(
        g: BluetoothGatt,
        rx: BluetoothGattCharacteristic,
        wireBytes: ByteArray
    ): WriteAttemptResult {
        return try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                when (
                    val status = g.writeCharacteristic(
                        rx,
                        wireBytes,
                        BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
                    )
                ) {
                    BluetoothStatusCodes.SUCCESS -> WriteAttemptResult.Accepted
                    BluetoothStatusCodes.ERROR_GATT_WRITE_REQUEST_BUSY -> WriteAttemptResult.Busy
                    BluetoothStatusCodes.ERROR_MISSING_BLUETOOTH_CONNECT_PERMISSION ->
                        WriteAttemptResult.Failed("缺少蓝牙连接权限，无法发送指令", fatal = true)
                    BluetoothStatusCodes.ERROR_BLUETOOTH_NOT_ENABLED,
                    BluetoothStatusCodes.ERROR_PROFILE_SERVICE_NOT_BOUND ->
                        WriteAttemptResult.Failed("蓝牙服务未就绪，请重新连接设备", fatal = true)
                    else ->
                        WriteAttemptResult.Failed("指令写入被拒绝 (code=$status)", fatal = false)
                }
            } else {
                @Suppress("DEPRECATION")
                rx.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
                @Suppress("DEPRECATION")
                rx.value = wireBytes
                @Suppress("DEPRECATION")
                if (g.writeCharacteristic(rx)) {
                    WriteAttemptResult.Accepted
                } else if (_connectionState.value == ConnectionState.CONNECTED && gatt === g) {
                    WriteAttemptResult.Busy
                } else {
                    WriteAttemptResult.Failed("设备已断开，指令发送取消", fatal = true)
                }
            }
        } catch (e: SecurityException) {
            Log.e(TAG, "writeCharacteristic SecurityException: ${e.message}")
            WriteAttemptResult.Failed("缺少蓝牙连接权限，无法发送指令", fatal = true)
        } catch (e: Exception) {
            Log.e(TAG, "writeCharacteristic failed: ${e.message}")
            WriteAttemptResult.Failed("指令写入失败", fatal = false)
        }
    }

    private fun dequeueHandledCommandLocked(expectedSession: Long, isUrgent: Boolean, cmd: String) {
        if (writeSession != expectedSession) return
        if (isUrgent) {
            if (pendingEmergencyStop == cmd) {
                pendingEmergencyStop = null
            }
        } else if (cmdQueue.firstOrNull() === cmd) {
            cmdQueue.removeFirstOrNull()
        }
    }

    private suspend fun drainWriteQueue(expectedSession: Long) {
        val myJob = currentCoroutineContext()[Job]
        var retryCount = 0
        var lastAttemptCmd: String? = null
        var lastAttemptWasUrgent = false

        while (currentCoroutineContext().isActive) {
            var cmdToSend: String? = null
            var isUrgent = false
            var currentGatt: BluetoothGatt? = null
            var currentRx: BluetoothGattCharacteristic? = null
            var ackDeferred: CompletableDeferred<Int>? = null
            var writeSeq = 0L

            synchronized(writeLock) {
                if (writeSession != expectedSession || _connectionState.value != ConnectionState.CONNECTED) {
                    if (writeDrainJob === myJob) writeDrainJob = null
                    return
                }
                currentGatt = gatt
                currentRx = rxCharacteristic
                if (currentGatt == null || currentRx == null) {
                    if (writeDrainJob === myJob) writeDrainJob = null
                    return
                }
                val urgentCmd = pendingEmergencyStop
                if (urgentCmd != null) {
                    cmdToSend = urgentCmd
                    isUrgent = true
                } else {
                    val head = cmdQueue.firstOrNull()
                    if (head == null) {
                        if (writeDrainJob === myJob) writeDrainJob = null
                        return
                    }
                    cmdToSend = head
                    isUrgent = false
                }
                if (cmdToSend != lastAttemptCmd || isUrgent != lastAttemptWasUrgent) {
                    retryCount = 0
                    lastAttemptCmd = cmdToSend
                    lastAttemptWasUrgent = isUrgent
                }
                writeSeq = writeAckTracker.nextSeq()
                activeWriteSeq = writeSeq
                ackDeferred = CompletableDeferred()
                pendingWriteAck = ackDeferred
            }

            val g = currentGatt!!
            val rx = currentRx!!
            val cmd = cmdToSend!!
            val ack = ackDeferred!!

            val wireBytes = cmd.toByteArray(Charsets.UTF_8)
            val maxAllowed = maxCommandPayloadBytes()
            if (wireBytes.size > maxAllowed) {
                synchronized(writeLock) {
                    writeAckTracker.disarm(writeSeq)
                    if (activeWriteSeq == writeSeq) activeWriteSeq = 0L
                    if (pendingWriteAck === ack) pendingWriteAck = null
                    dequeueHandledCommandLocked(expectedSession, isUrgent, cmd)
                }
                lastAttemptCmd = null
                retryCount = 0
                onScanError?.invoke("当前蓝牙 MTU(${_mtu.value}) 负载上限 ${maxAllowed}B，已拦截超长指令 (${wireBytes.size}B)")
                continue
            }

            val writeStartMs = SystemClock.elapsedRealtime()
            when (val result = writeCharacteristicCompat(g, rx, wireBytes)) {
                WriteAttemptResult.Accepted -> {
                    val ackStatus = withTimeoutOrNull(WRITE_ACK_TIMEOUT_MS) {
                        ack.await()
                    }
                    val sessionValid = synchronized(writeLock) {
                        writeAckTracker.disarm(writeSeq)
                        if (activeWriteSeq == writeSeq) activeWriteSeq = 0L
                        if (pendingWriteAck === ack) {
                            pendingWriteAck = null
                        }
                        writeSession == expectedSession
                    }
                    if (!sessionValid) {
                        return
                    }

                    if (ackStatus != null && ackStatus != BluetoothGatt.GATT_SUCCESS) {
                        val preemptedByUrgent = !isUrgent && synchronized(writeLock) { pendingEmergencyStop != null }
                        if (preemptedByUrgent) {
                            lastAttemptCmd = null
                            retryCount = 0
                            continue
                        }
                        retryCount++
                        val maxRetries = if (isUrgent) MAX_URGENT_WRITE_RETRIES else MAX_WRITE_RETRIES
                        if (retryCount > maxRetries) {
                            synchronized(writeLock) {
                                dequeueHandledCommandLocked(expectedSession, isUrgent, cmd)
                            }
                            lastAttemptCmd = null
                            retryCount = 0
                            onScanError?.invoke("BLE 写入失败 (code=$ackStatus)，指令未能发出: $cmd")
                        } else {
                            val baseDelay = if (isUrgent) 25L else 60L
                            val backoffMs = (baseDelay shl (retryCount - 1)).coerceAtMost(240L)
                            Log.w(TAG, "GATT write ack status=$ackStatus for '$cmd', retry #$retryCount in ${backoffMs}ms")
                            if (isUrgent) {
                                delay(backoffMs)
                            } else {
                                delayInterruptibleForUrgent(backoffMs, expectedSession)
                            }
                        }
                    } else {
                        synchronized(writeLock) {
                            dequeueHandledCommandLocked(expectedSession, isUrgent, cmd)
                        }
                        lastAttemptCmd = null
                        retryCount = 0
                        val elapsedMs = SystemClock.elapsedRealtime() - writeStartMs
                        val waitMs = (MIN_WRITE_INTERVAL_MS - elapsedMs).coerceAtLeast(0L)
                        delayInterruptibleForUrgent(waitMs, expectedSession)
                    }
                }

                WriteAttemptResult.Busy -> {
                    synchronized(writeLock) {
                        writeAckTracker.disarm(writeSeq)
                        if (activeWriteSeq == writeSeq) activeWriteSeq = 0L
                        if (pendingWriteAck === ack) {
                            pendingWriteAck = null
                        }
                        if (writeSession != expectedSession) return
                    }
                    val preemptedByUrgent = !isUrgent && synchronized(writeLock) { pendingEmergencyStop != null }
                    if (preemptedByUrgent) {
                        lastAttemptCmd = null
                        retryCount = 0
                        continue
                    }
                    retryCount++
                    val maxRetries = if (isUrgent) MAX_URGENT_WRITE_RETRIES else MAX_WRITE_RETRIES
                    if (retryCount > maxRetries) {
                        synchronized(writeLock) {
                            dequeueHandledCommandLocked(expectedSession, isUrgent, cmd)
                        }
                        lastAttemptCmd = null
                        retryCount = 0
                        onScanError?.invoke("BLE 写入繁忙超时，指令未能发出: $cmd")
                    } else {
                        val baseDelay = if (isUrgent) 15L else 30L
                        val backoffMs = (baseDelay shl (retryCount - 1)).coerceAtMost(250L)
                        Log.w(TAG, "GATT write busy for '$cmd', retry #$retryCount in ${backoffMs}ms")
                        if (isUrgent) {
                            delay(backoffMs)
                        } else {
                            delayInterruptibleForUrgent(backoffMs, expectedSession)
                        }
                    }
                }

                is WriteAttemptResult.Failed -> {
                    synchronized(writeLock) {
                        writeAckTracker.disarm(writeSeq)
                        if (activeWriteSeq == writeSeq) activeWriteSeq = 0L
                        if (pendingWriteAck === ack) {
                            pendingWriteAck = null
                        }
                        dequeueHandledCommandLocked(expectedSession, isUrgent, cmd)
                    }
                    lastAttemptCmd = null
                    retryCount = 0
                    onScanError?.invoke(result.reason)
                    if (result.fatal) {
                        synchronized(writeLock) {
                            if (writeDrainJob === myJob) writeDrainJob = null
                        }
                        return
                    }
                }
            }
        }
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val device = result.device
            // 原子重入与目标守卫：
            // 1) 自动重连阶段按 lastDevice MAC 匹配，避免周边无关 NUS 设备劫持重连会话；
            // 2) 通过 scanLock 原子消费本次扫描，防止低延迟连续回调或扫描超时竞争触发重复 connectGatt。
            val captured = synchronized(scanLock) {
                if (!scanActive || _connectionState.value != ConnectionState.SCANNING) {
                    false
                } else {
                    val expectedAddress = if (reconnectTracker.shouldAutoReconnect) lastDevice?.address else null
                    if (expectedAddress != null && device.address != expectedAddress) {
                        false
                    } else {
                        scanActive = false
                        scanGeneration++
                        true
                    }
                }
            }
            if (!captured) return
            Log.i(TAG, "Found: ${device.name} ${device.address}")
            stopScan()
            connect(device, fromReconnect = reconnectTracker.shouldAutoReconnect, fromScan = true)
        }

        override fun onScanFailed(errorCode: Int) {
            Log.e(TAG, "Scan failed: $errorCode")
            synchronized(scanLock) {
                scanActive = false
                scanGeneration++
            }
            scanStartJob?.cancel()
            scanStartJob = null
            scanTimeoutJob?.cancel()
            scanTimeoutJob = null
            if (reconnectTracker.shouldAutoReconnect && !userInitiatedDisconnect && !btDisabledBySystem) {
                handleReconnectFailure(
                    status = errorCode,
                    alreadyDisconnected = true,
                    errorMessage = "扫描失败 (code=$errorCode)",
                    wasDirectAttempt = false,
                    wasManualRetryDirect = false
                )
            } else {
                _connectionState.value = ConnectionState.DISCONNECTED
                onScanError?.invoke("扫描失败 (code=$errorCode)")
            }
        }
    }

    @SuppressLint("MissingPermission")
    private fun connect(
        device: BluetoothDevice,
        fromReconnect: Boolean = false,
        fromScan: Boolean = false,
        fromManualRetry: Boolean = false
    ) {
        if (fromReconnect && (userInitiatedDisconnect || btDisabledBySystem || !reconnectTracker.shouldAutoReconnect)) {
            return
        }
        cancelHandshakeJobs()
        connectJob?.cancel()
        connectJob = null
        if (!fromReconnect) {
            btDisabledBySystem = false
            userInitiatedDisconnect = false
            reconnectJob?.cancel()
            reconnectJob = null
        }
        resetWriteQueue()
        resetRxBuffer()
        closeCurrentGatt(alreadyDisconnected = false)

        lastDevice = device
        handshakeCompleted = false
        physicalConnected = false
        discoveryStarted = false
        retriedDiscovery = false
        currentAttemptWasDirect = !fromScan
        currentAttemptWasManualRetryDirect = fromManualRetry && !fromScan
        _mtu.value = 23
        _connectionState.value = ConnectionState.CONNECTING

        val ctx = appContext ?: run {
            currentAttemptWasDirect = false
            currentAttemptWasManualRetryDirect = false
            _connectionState.value = ConnectionState.DISCONNECTED
            return
        }

        val job = scope.launch(start = CoroutineStart.LAZY) {
            val myJob = currentCoroutineContext()[Job]
            awaitPendingGattClose(GATT_CLOSE_TIMEOUT_MS)
            if (!isActive ||
                connectJob !== myJob ||
                userInitiatedDisconnect ||
                btDisabledBySystem ||
                (fromReconnect && !reconnectTracker.shouldAutoReconnect) ||
                _connectionState.value != ConnectionState.CONNECTING
            ) {
                return@launch
            }

            val newGatt = try {
                device.connectGatt(ctx, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
            } catch (e: SecurityException) {
                currentAttemptWasDirect = false
                currentAttemptWasManualRetryDirect = false
                val msg = "缺少蓝牙连接权限，请在系统设置中授权"
                if (reconnectTracker.shouldAutoReconnect) {
                    _reconnectState.value = reconnectTracker.exhaustNow(msg)
                }
                _connectionState.value = ConnectionState.DISCONNECTED
                onScanError?.invoke(msg)
                return@launch
            }
            if (newGatt == null) {
                if (reconnectTracker.shouldAutoReconnect && !userInitiatedDisconnect && !btDisabledBySystem) {
                    handleReconnectFailure(
                        status = BleReconnectTracker.GATT_ERROR_133,
                        alreadyDisconnected = true,
                        errorMessage = "无法发起 BLE GATT 连接",
                        wasDirectAttempt = currentAttemptWasDirect,
                        wasManualRetryDirect = currentAttemptWasManualRetryDirect
                    )
                } else {
                    currentAttemptWasDirect = false
                    currentAttemptWasManualRetryDirect = false
                    _connectionState.value = ConnectionState.DISCONNECTED
                    onScanError?.invoke("无法发起 BLE GATT 连接")
                }
                return@launch
            }

            // 两阶段超时保护：物理建链采用短超时（直连 4.5s / 扫描 8s），进入 STATE_CONNECTED 后在 gattLock 下无缝切换为 10s GATT 握手超时。
            val physicalTimeoutMs = BleReconnectTracker.physicalConnectTimeoutMs(fromScan)
            lateinit var lazyTimeoutJob: Job
            lazyTimeoutJob = scope.launch(start = CoroutineStart.LAZY) {
                delay(physicalTimeoutMs)
                val shouldTimeout = synchronized(gattLock) {
                    isActive &&
                        connectTimeoutJob === lazyTimeoutJob &&
                        gatt === newGatt &&
                        !physicalConnected &&
                        !handshakeCompleted &&
                        _connectionState.value == ConnectionState.CONNECTING
                }
                if (shouldTimeout) {
                    val timeoutReason = if (!fromScan && reconnectTracker.shouldAutoReconnect) {
                        "BLE 直连超时，正在切换扫描重连"
                    } else {
                        "GATT 连接超时，请复位开发板或取消后重试"
                    }
                    failHandshake(
                        reason = timeoutReason,
                        targetGatt = newGatt,
                        status = BleReconnectTracker.GATT_ERROR_CONNECT_TIMEOUT
                    )
                }
            }

            var prevTimeoutJob: Job? = null
            val adopted = synchronized(gattLock) {
                if (isActive &&
                    connectJob === myJob &&
                    !userInitiatedDisconnect &&
                    !btDisabledBySystem &&
                    (!fromReconnect || reconnectTracker.shouldAutoReconnect) &&
                    _connectionState.value == ConnectionState.CONNECTING
                ) {
                    gatt = newGatt
                    physicalConnected = false
                    prevTimeoutJob = connectTimeoutJob
                    connectTimeoutJob = lazyTimeoutJob
                    true
                } else {
                    false
                }
            }
            prevTimeoutJob?.cancel()
            if (!adopted) {
                lazyTimeoutJob.cancel()
                try {
                    newGatt.close()
                } catch (_: Exception) {
                }
                return@launch
            }
            lazyTimeoutJob.start()
        }
        connectJob = job
        job.start()
    }

    private fun handleReconnectFailure(
        status: Int,
        alreadyDisconnected: Boolean,
        errorMessage: String,
        wasDirectAttempt: Boolean = currentAttemptWasDirect,
        wasManualRetryDirect: Boolean = currentAttemptWasManualRetryDirect
    ) {
        val savedDev = lastDevice
        cleanupConnectionResources(alreadyDisconnected = alreadyDisconnected)
        val step = reconnectTracker.nextReconnectStep(
            status = status,
            hasCachedDevice = savedDev != null,
            errorMessage = errorMessage,
            forceScanFallback = wasDirectAttempt
        )
        when (step) {
            ReconnectStep.Ignore -> {
                _reconnectState.value = reconnectTracker.uiState
                _connectionState.value = ConnectionState.DISCONNECTED
                onScanError?.invoke(errorMessage)
            }
            is ReconnectStep.Retry -> {
                _reconnectState.value = reconnectTracker.uiState
                _connectionState.value = if (step.useScan) {
                    ConnectionState.SCANNING
                } else {
                    ConnectionState.CONNECTING
                }
                if (step.attempt == 1) {
                    val toastMsg = if (wasManualRetryDirect) {
                        "直连未成功，正在切换扫描重连..."
                    } else {
                        "蓝牙连接意外断开，正在自动重连..."
                    }
                    onScanError?.invoke(toastMsg)
                }
                reconnectJob?.cancel()
                val delayJob = scope.launch(start = CoroutineStart.LAZY) {
                    val myReconnectJob = currentCoroutineContext()[Job]
                    delay(step.delayMs)
                    if (!isActive ||
                        reconnectJob !== myReconnectJob ||
                        userInitiatedDisconnect ||
                        btDisabledBySystem ||
                        !reconnectTracker.shouldAutoReconnect
                    ) {
                        return@launch
                    }
                    if (step.useScan || savedDev == null) {
                        startScanInternal(fromReconnect = true)
                    } else {
                        connect(savedDev, fromReconnect = true, fromScan = false)
                    }
                }
                reconnectJob = delayJob
                delayJob.start()
            }
            is ReconnectStep.Exhausted -> {
                _reconnectState.value = step.uiState
                _connectionState.value = ConnectionState.DISCONNECTED
                onScanError?.invoke(step.uiState.lastError ?: "自动重连失败，请点击重试")
            }
        }
    }

    private fun failHandshake(
        reason: String,
        targetGatt: BluetoothGatt? = null,
        status: Int = BluetoothGatt.GATT_FAILURE
    ) {
        val btOff = btDisabledBySystem || (bluetoothAdapter?.isEnabled != true)
        val wasDirectAttempt = currentAttemptWasDirect
        val wasManualRetryDirect = currentAttemptWasManualRetryDirect
        if (!claimAndCloseActiveGatt(expectedGatt = targetGatt, alreadyDisconnected = btOff)) {
            return
        }
        Log.e(TAG, "Handshake failed: $reason")
        if (btOff) {
            if (!userInitiatedDisconnect) {
                reconnectTracker.onDisconnectWhileBtOff(
                    wasActiveSession = _connectionState.value != ConnectionState.DISCONNECTED
                )
                _reconnectState.value = reconnectTracker.uiState
            }
            reconnectJob?.cancel()
            reconnectJob = null
            cleanupConnectionResources(alreadyDisconnected = true)
            _connectionState.value = ConnectionState.DISCONNECTED
            return
        }
        if (!userInitiatedDisconnect && reconnectTracker.shouldAutoReconnect) {
            handleReconnectFailure(
                status = status,
                alreadyDisconnected = false,
                errorMessage = reason,
                wasDirectAttempt = wasDirectAttempt,
                wasManualRetryDirect = wasManualRetryDirect
            )
        } else {
            performDisconnect(userInitiated = false, alreadyDisconnected = false)
            onScanError?.invoke(reason)
        }
    }

    private fun refreshGattCache(gatt: BluetoothGatt): Boolean {
        return try {
            val method = gatt.javaClass.getMethod("refresh")
            (method.invoke(gatt) as? Boolean) ?: false
        } catch (e: Exception) {
            Log.w(TAG, "BluetoothGatt.refresh() unavailable: ${e.message}")
            false
        }
    }

    @SuppressLint("MissingPermission")
    private fun startServiceDiscovery(gatt: BluetoothGatt) {
        if (gatt !== this.gatt || discoveryStarted || _connectionState.value != ConnectionState.CONNECTING) return
        discoveryStarted = true
        mtuFallbackJob?.cancel()
        mtuFallbackJob = null
        val started = try {
            gatt.discoverServices()
        } catch (e: SecurityException) {
            false
        }
        if (!started) {
            failHandshake("无法启动 GATT 服务发现", gatt)
        }
    }

    /** 服务发现失败或服务列表暂未就绪时刷新 GATT 缓存并安排一次延迟重试 */
    private fun retryServiceDiscoveryIfEligible(
        gatt: BluetoothGatt,
        reasonFormatter: (refreshed: Boolean) -> String
    ): Boolean {
        if (retriedDiscovery || !isHandshakeTimerActive()) return false
        retriedDiscovery = true
        discoveryStarted = false
        val refreshed = refreshGattCache(gatt)
        Log.w(TAG, reasonFormatter(refreshed))
        mtuSetupJob?.cancel()
        val retryJob = scope.launch(start = CoroutineStart.LAZY) {
            val myRetryJob = currentCoroutineContext()[Job]
            delay(300)
            if (isActive &&
                mtuSetupJob === myRetryJob &&
                gatt === this@BleManager.gatt &&
                _connectionState.value == ConnectionState.CONNECTING
            ) {
                startServiceDiscovery(gatt)
            }
        }
        mtuSetupJob = retryJob
        retryJob.start()
        return true
    }

    @SuppressLint("MissingPermission")
    private fun writeCccd(gatt: BluetoothGatt, descriptor: BluetoothGattDescriptor): Boolean {
        return try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                gatt.writeDescriptor(
                    descriptor,
                    BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                ) == BluetoothStatusCodes.SUCCESS
            } else {
                @Suppress("DEPRECATION")
                descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                @Suppress("DEPRECATION")
                gatt.writeDescriptor(descriptor)
            }
        } catch (e: Exception) {
            Log.e(TAG, "writeCccd exception: ${e.message}")
            false
        }
    }

    private fun markConnected(gatt: BluetoothGatt) {
        var timeoutToCancel: Job? = null
        val accepted = synchronized(gattLock) {
            if (gatt !== this.gatt ||
                userInitiatedDisconnect ||
                btDisabledBySystem ||
                _connectionState.value != ConnectionState.CONNECTING
            ) {
                false
            } else {
                handshakeCompleted = true
                timeoutToCancel = connectTimeoutJob
                connectTimeoutJob = null
                currentAttemptWasDirect = false
                currentAttemptWasManualRetryDirect = false
                true
            }
        }
        if (!accepted) return
        timeoutToCancel?.cancel()
        mtuSetupJob?.cancel()
        mtuSetupJob = null
        mtuFallbackJob?.cancel()
        mtuFallbackJob = null
        _reconnectState.value = reconnectTracker.markConnected()
        _connectionState.value = ConnectionState.CONNECTED
        Log.i(TAG, "Notify channel ready")
        syncAfterConnect()
    }

    private val gattCallback = object : BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                val pending = synchronized(gattLock) {
                    pendingCloseGatts.remove(gatt)
                }
                if (pending != null) {
                    pending.timeoutJob.cancel()
                    Log.i(TAG, "Graceful disconnect completed (status=$status), closing GATT")
                    try {
                        gatt.close()
                    } catch (_: Exception) {
                    }
                    pending.completed.complete(Unit)
                    return
                }
            }
            if (gatt !== this@BleManager.gatt) return
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    if (status == BluetoothGatt.GATT_SUCCESS) {
                        lateinit var handshakeTimeoutJob: Job
                        handshakeTimeoutJob = scope.launch(start = CoroutineStart.LAZY) {
                            delay(BleReconnectTracker.GATT_HANDSHAKE_TIMEOUT_MS)
                            val shouldTimeout = synchronized(gattLock) {
                                isActive &&
                                    connectTimeoutJob === handshakeTimeoutJob &&
                                    this@BleManager.gatt === gatt &&
                                    !handshakeCompleted &&
                                    _connectionState.value == ConnectionState.CONNECTING
                            }
                            if (shouldTimeout) {
                                failHandshake("GATT 握手超时，请复位开发板或取消后重试", gatt)
                            }
                        }
                        var prevTimeoutJob: Job? = null
                        val stillActive = synchronized(gattLock) {
                            if (gatt === this@BleManager.gatt &&
                                !userInitiatedDisconnect &&
                                !btDisabledBySystem &&
                                _connectionState.value == ConnectionState.CONNECTING
                            ) {
                                physicalConnected = true
                                prevTimeoutJob = connectTimeoutJob
                                connectTimeoutJob = handshakeTimeoutJob
                                true
                            } else {
                                false
                            }
                        }
                        prevTimeoutJob?.cancel()
                        if (!stillActive) {
                            handshakeTimeoutJob.cancel()
                            return
                        }
                        handshakeTimeoutJob.start()
                        reconnectJob?.cancel()
                        reconnectJob = null
                        // 仅允许 BALANCED（避免 HIGH 触发固件 appGapCallback 拒绝），随后交由固件 1.5s 协商接管
                        try {
                            gatt.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_BALANCED)
                        } catch (_: Exception) {
                        }
                        Log.i(TAG, "Connected, scheduling MTU 247 & service discovery")
                        // 留出 400ms 错开板子端 slave_init_mtu_req 协商，避免双向 MTU Exchange 冲突；2.2s 兜底继续发现服务
                        mtuSetupJob?.cancel()
                        val setupJob = scope.launch(start = CoroutineStart.LAZY) {
                            val myMtuJob = currentCoroutineContext()[Job]
                            delay(400)
                            if (!isActive ||
                                mtuSetupJob !== myMtuJob ||
                                gatt !== this@BleManager.gatt ||
                                _connectionState.value != ConnectionState.CONNECTING
                            ) {
                                return@launch
                            }
                            val ok = try {
                                gatt.requestMtu(247)
                            } catch (_: Exception) {
                                false
                            }
                            if (!ok) {
                                Log.w(TAG, "requestMtu(247) returned false, falling back to discoverServices()")
                                startServiceDiscovery(gatt)
                            }
                        }
                        mtuSetupJob = setupJob
                        setupJob.start()

                        mtuFallbackJob?.cancel()
                        val fallbackJob = scope.launch(start = CoroutineStart.LAZY) {
                            val myFallbackJob = currentCoroutineContext()[Job]
                            delay(2_200)
                            if (isActive &&
                                mtuFallbackJob === myFallbackJob &&
                                gatt === this@BleManager.gatt &&
                                _connectionState.value == ConnectionState.CONNECTING &&
                                !discoveryStarted
                            ) {
                                Log.w(TAG, "MTU negotiation timed out, continuing to discoverServices()")
                                startServiceDiscovery(gatt)
                            }
                        }
                        mtuFallbackJob = fallbackJob
                        fallbackJob.start()
                    } else {
                        Log.e(TAG, "Connected with error status=$status")
                        failHandshake("连接失败 (code=$status)", gatt, status = status)
                    }
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    val btOff = btDisabledBySystem || (bluetoothAdapter?.isEnabled != true)
                    val unexpected = !userInitiatedDisconnect && !btOff
                    val wasActiveSession =
                        _connectionState.value != ConnectionState.DISCONNECTED ||
                            reconnectTracker.wasSessionActiveForBtOff()
                    val wasDirectAttempt = currentAttemptWasDirect
                    val wasManualRetryDirect = currentAttemptWasManualRetryDirect
                    if (!claimAndCloseActiveGatt(expectedGatt = gatt, alreadyDisconnected = true)) {
                        return
                    }
                    Log.i(
                        TAG,
                        "Disconnected (status=$status, btOff=$btOff, unexpected=$unexpected, shouldReconnect=${reconnectTracker.shouldAutoReconnect})"
                    )
                    if (btOff) {
                        if (!userInitiatedDisconnect && wasActiveSession) {
                            reconnectTracker.onDisconnectWhileBtOff(wasActiveSession = true)
                            _reconnectState.value = reconnectTracker.uiState
                        }
                        reconnectJob?.cancel()
                        reconnectJob = null
                        cleanupConnectionResources(alreadyDisconnected = true)
                        _connectionState.value = ConnectionState.DISCONNECTED
                        return
                    }
                    if (unexpected) {
                        val msg = if (reconnectTracker.shouldAutoReconnect) {
                            "蓝牙连接断开 (code=$status)"
                        } else {
                            "连接/GATT 握手中断 (code=$status)，请确认未在系统蓝牙配对并重试"
                        }
                        handleReconnectFailure(
                            status = status,
                            alreadyDisconnected = true,
                            errorMessage = msg,
                            wasDirectAttempt = wasDirectAttempt,
                            wasManualRetryDirect = wasManualRetryDirect
                        )
                    } else {
                        performDisconnect(userInitiated = false, alreadyDisconnected = true)
                        if (status != BluetoothGatt.GATT_SUCCESS) {
                            onScanError?.invoke("连接失败 (code=$status)")
                        }
                    }
                }
            }
        }

        override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
            if (gatt !== this@BleManager.gatt) return
            Log.i(TAG, "MTU changed to $mtu (status=$status)")
            if (status == BluetoothGatt.GATT_SUCCESS && mtu >= 23) {
                _mtu.value = mtu
            }
            mtuFallbackJob?.cancel()
            mtuFallbackJob = null
            if (_connectionState.value == ConnectionState.CONNECTING) {
                startServiceDiscovery(gatt)
            }
        }

        @SuppressLint("MissingPermission")
        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            if (gatt !== this@BleManager.gatt || _connectionState.value != ConnectionState.CONNECTING) return
            if (status != BluetoothGatt.GATT_SUCCESS) {
                if (!retryServiceDiscoveryIfEligible(gatt) { refreshed ->
                        "Service discovery failed (status=$status, refreshed=$refreshed), retrying in 300ms"
                    }
                ) {
                    Log.e(TAG, "Service discovery failed: $status")
                    failHandshake("GATT 服务发现失败 (code=$status)", gatt, status = status)
                }
                return
            }
            val service = gatt.getService(SERVICE_UUID)
            val tx = service?.getCharacteristic(CHAR_TX_UUID)
            val rx = service?.getCharacteristic(CHAR_RX_UUID)
            if (service == null || tx == null || rx == null) {
                if (!retryServiceDiscoveryIfEligible(gatt) { refreshed ->
                        "NUS service not ready (refreshed=$refreshed), retrying service discovery"
                    }
                ) {
                    failHandshake("未发现 WiFi-Deck NUS 服务，请检查固件并重试", gatt)
                }
                return
            }

            txCharacteristic = tx
            rxCharacteristic = rx
            if (!gatt.setCharacteristicNotification(tx, true)) {
                failHandshake("无法启用 Notify 监听", gatt)
                return
            }
            val descriptor = tx.getDescriptor(CCCD_UUID)
            if (descriptor != null) {
                if (!writeCccd(gatt, descriptor)) {
                    Log.w(TAG, "writeCccd returned false, retrying once after 200ms")
                    mtuSetupJob?.cancel()
                    val retryJob = scope.launch(start = CoroutineStart.LAZY) {
                        val myRetryJob = currentCoroutineContext()[Job]
                        delay(200)
                        if (isActive &&
                            mtuSetupJob === myRetryJob &&
                            gatt === this@BleManager.gatt &&
                            _connectionState.value == ConnectionState.CONNECTING
                        ) {
                            if (!writeCccd(gatt, descriptor)) {
                                failHandshake("订阅 Notify CCCD 失败", gatt)
                            }
                        }
                    }
                    mtuSetupJob = retryJob
                    retryJob.start()
                }
            } else {
                markConnected(gatt)
            }
        }

        @SuppressLint("MissingPermission")
        override fun onDescriptorWrite(
            gatt: BluetoothGatt,
            descriptor: BluetoothGattDescriptor,
            status: Int
        ) {
            if (gatt !== this@BleManager.gatt || _connectionState.value != ConnectionState.CONNECTING) return
            if (status == BluetoothGatt.GATT_SUCCESS) {
                markConnected(gatt)
            } else {
                Log.e(TAG, "onDescriptorWrite failed: $status")
                failHandshake("开启数据通知失败 (code=$status)", gatt, status = status)
            }
        }

        override fun onCharacteristicWrite(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int
        ) {
            if (gatt !== this@BleManager.gatt) return
            if (status != BluetoothGatt.GATT_SUCCESS) {
                Log.e(TAG, "Write callback failed: $status")
            }
            val ack = synchronized(writeLock) {
                if (writeAckTracker.matches(activeWriteSeq, characteristic.uuid, CHAR_RX_UUID)) {
                    writeAckTracker.disarm(activeWriteSeq)
                    activeWriteSeq = 0L
                    pendingWriteAck.also { pendingWriteAck = null }
                } else {
                    null
                }
            }
            ack?.complete(status)
        }

        // Android 13+ (API 33+) 走这里
        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray
        ) {
            if (gatt !== this@BleManager.gatt) return
            handleIncomingBytes(value)
        }

        // Android 8.0 ~ 12 (API 26 ~ 32) 走这里
        @Deprecated("Deprecated in API 33")
        @Suppress("DEPRECATION")
        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic
        ) {
            if (gatt !== this@BleManager.gatt) return
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
                characteristic.value?.let { handleIncomingBytes(it) }
            }
        }
    }

    /** 连接建立后，拉取板子已有的扫描结果与配置 */
    private fun syncAfterConnect() {
        sendCommand("info")
        sendCommand("results")
    }

    private fun handleIncomingBytes(value: ByteArray) {
        if (value.isEmpty()) return
        val completedLines = synchronized(rxLock) {
            rxFramer.feed(value) { warning -> Log.w(TAG, warning) }
        }
        for (line in completedLines) {
            onLine?.invoke(line)
        }
    }
}
