package io.github.xiangwen.wifideck.data

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
import android.location.LocationManager
import android.os.Build
import android.os.ParcelUuid
import android.os.SystemClock
import android.util.Log
import androidx.core.location.LocationManagerCompat
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
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

    private const val MAX_CMD_QUEUE_SIZE = 24
    private const val GATT_CLOSE_TIMEOUT_MS = 800L
    private const val MAX_WRITE_RETRIES = 5
    private const val MAX_URGENT_WRITE_RETRIES = 8
    private const val WRITE_ACK_TIMEOUT_MS = 60L
    private const val MIN_WRITE_INTERVAL_MS = 25L

    enum class ConnectionState { DISCONNECTED, SCANNING, CONNECTING, CONNECTED }

    private val _connectionState = MutableStateFlow(ConnectionState.DISCONNECTED)
    override val connectionState: StateFlow<ConnectionState> = _connectionState

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
    @Volatile private var gatt: BluetoothGatt? = null
    @Volatile private var txCharacteristic: BluetoothGattCharacteristic? = null
    @Volatile private var rxCharacteristic: BluetoothGattCharacteristic? = null
    private val pendingCloseGatts = HashMap<BluetoothGatt, Job>()
    private var lastDevice: BluetoothDevice? = null
    @Volatile private var userInitiatedDisconnect = false
    @Volatile private var btDisabledBySystem = false
    private var reconnectJob: Job? = null

    private val rxLock = Any()
    private val rxFramer = BleLineFramer(BleLineFramer.DEFAULT_MAX_LINE_BYTES)

    // 串行命令发送队列：有界容量 + 急停优先通道 + writeCharacteristic 返回值校验与背压重试。
    private val scope = CoroutineScope(Dispatchers.IO + SupervisorJob())
    private val cmdQueue = ArrayDeque<String>()
    private val writeLock = Any()
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

    fun startScan() {
        val adapter = bluetoothAdapter
        if (adapter == null) {
            _connectionState.value = ConnectionState.DISCONNECTED
            onScanError?.invoke("当前设备不支持 BLE 或蓝牙不可用")
            return
        }
        if (!adapter.isEnabled) {
            _connectionState.value = ConnectionState.DISCONNECTED
            onScanError?.invoke("请先打开蓝牙")
            return
        }
        if (!isLocationReadyForScan()) {
            _connectionState.value = ConnectionState.DISCONNECTED
            onScanError?.invoke("Android 8~11 扫描蓝牙设备需开启系统定位开关")
            return
        }
        val scanner = adapter.bluetoothLeScanner
        if (scanner == null) {
            _connectionState.value = ConnectionState.DISCONNECTED
            onScanError?.invoke("当前设备不支持 BLE 或蓝牙不可用")
            return
        }

        // 先清理上一次可能残留的扫描/握手状态与 GATT 句柄
        stopScan()
        reconnectJob?.cancel()
        reconnectJob = null
        cancelHandshakeJobs()
        resetWriteQueue()
        resetRxBuffer()
        _mtu.value = 23
        closeCurrentGatt()
        btDisabledBySystem = false
        userInitiatedDisconnect = false
        handshakeCompleted = false
        discoveryStarted = false
        retriedDiscovery = false

        _connectionState.value = ConnectionState.SCANNING

        val filter = ScanFilter.Builder()
            .setServiceUuid(ParcelUuid(SERVICE_UUID))
            .build()
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        try {
            @SuppressLint("MissingPermission")
            scanner.startScan(listOf(filter), settings, scanCallback)
        } catch (e: SecurityException) {
            _connectionState.value = ConnectionState.DISCONNECTED
            onScanError?.invoke("缺少蓝牙权限，请在系统设置中授权")
            return
        }

        // 10 秒扫描超时：没扫到设备自动停止，避免永久卡在 SCANNING
        scanTimeoutJob?.cancel()
        scanTimeoutJob = scope.launch {
            delay(10_000)
            if (_connectionState.value == ConnectionState.SCANNING) {
                stopScan()
                _connectionState.value = ConnectionState.DISCONNECTED
                onScanError?.invoke("扫描超时，未找到设备（请确认板子已上电且未被系统蓝牙占用）")
            }
        }
    }

    fun stopScan() {
        scanTimeoutJob?.cancel()
        scanTimeoutJob = null
        try {
            @SuppressLint("MissingPermission")
            bluetoothAdapter?.bluetoothLeScanner?.stopScan(scanCallback)
        } catch (_: Exception) {
        }
    }

    private fun cancelHandshakeJobs() {
        connectTimeoutJob?.cancel()
        connectTimeoutJob = null
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

    @SuppressLint("MissingPermission")
    private fun closeCurrentGatt(alreadyDisconnected: Boolean = false) {
        val current: BluetoothGatt?
        synchronized(gattLock) {
            current = gatt
            gatt = null
            txCharacteristic = null
            rxCharacteristic = null
        }
        if (current != null) {
            closeGattGracefully(current, alreadyDisconnected)
        }
    }

    @SuppressLint("MissingPermission")
    private fun closeGattGracefully(target: BluetoothGatt, alreadyDisconnected: Boolean) {
        val existingTimeout: Job?
        synchronized(gattLock) {
            existingTimeout = pendingCloseGatts.remove(target)
        }
        existingTimeout?.cancel()

        if (alreadyDisconnected) {
            try {
                target.close()
            } catch (_: Exception) {
            }
            return
        }

        val timeoutJob = scope.launch {
            delay(GATT_CLOSE_TIMEOUT_MS)
            val shouldClose = synchronized(gattLock) {
                pendingCloseGatts.remove(target) != null
            }
            if (shouldClose) {
                Log.w(TAG, "GATT disconnect callback timeout (${GATT_CLOSE_TIMEOUT_MS}ms), closing GATT")
                try {
                    target.close()
                } catch (_: Exception) {
                }
            }
        }

        synchronized(gattLock) {
            pendingCloseGatts[target] = timeoutJob
        }

        val disconnectOk = try {
            target.disconnect()
            true
        } catch (_: Exception) {
            false
        }

        if (!disconnectOk) {
            val removed = synchronized(gattLock) {
                pendingCloseGatts.remove(target)
            }
            removed?.cancel()
            try {
                target.close()
            } catch (_: Exception) {
            }
        }
    }

    override fun disconnect(userInitiated: Boolean) {
        btDisabledBySystem = false
        performDisconnect(userInitiated = userInitiated, alreadyDisconnected = false)
    }

    /** 系统蓝牙关闭广播到来时立即清理连接并阻断无意义的自动重连；返回关闭前是否处于连接/扫描活动态 */
    override fun onBluetoothAdapterOff(): Boolean {
        btDisabledBySystem = true
        val wasActive = _connectionState.value != ConnectionState.DISCONNECTED
        reconnectJob?.cancel()
        reconnectJob = null
        performDisconnect(
            userInitiated = false,
            alreadyDisconnected = (bluetoothAdapter?.isEnabled != true)
        )
        return wasActive
    }

    override fun onBluetoothAdapterOn() {
        btDisabledBySystem = false
    }

    private fun performDisconnect(userInitiated: Boolean, alreadyDisconnected: Boolean) {
        if (userInitiated) {
            userInitiatedDisconnect = true
            lastDevice = null
            reconnectJob?.cancel()
            reconnectJob = null
        }
        stopScan()
        cancelHandshakeJobs()
        handshakeCompleted = false
        discoveryStarted = false
        retriedDiscovery = false
        _mtu.value = 23
        resetWriteQueue()
        // 清空残余半包，避免断线重连后第一条消息解析失败
        resetRxBuffer()
        closeCurrentGatt(alreadyDisconnected = alreadyDisconnected)
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
                    synchronized(writeLock) {
                        if (writeSession != expectedSession) {
                            if (pendingWriteAck === ack) pendingWriteAck = null
                            return
                        }
                        dequeueHandledCommandLocked(expectedSession, isUrgent, cmd)
                    }
                    lastAttemptCmd = null
                    retryCount = 0

                    val ackStatus = withTimeoutOrNull(WRITE_ACK_TIMEOUT_MS) {
                        ack.await()
                    }
                    synchronized(writeLock) {
                        if (pendingWriteAck === ack) {
                            pendingWriteAck = null
                        }
                    }
                    val elapsedMs = SystemClock.elapsedRealtime() - writeStartMs
                    val minGapMs = if (ackStatus != null && ackStatus != BluetoothGatt.GATT_SUCCESS) {
                        60L
                    } else {
                        MIN_WRITE_INTERVAL_MS
                    }
                    val waitMs = (minGapMs - elapsedMs).coerceAtLeast(0L)
                    delayInterruptibleForUrgent(waitMs, expectedSession)
                }

                WriteAttemptResult.Busy -> {
                    synchronized(writeLock) {
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
                        val baseDelay = if (isUrgent) 15L else 25L
                        val backoffMs = (baseDelay shl (retryCount - 1)).coerceAtMost(200L)
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
            // 重入守卫：低延迟扫描下 stopScan 生效前可能连续回调，避免重复 connectGatt 泄漏句柄
            if (_connectionState.value != ConnectionState.SCANNING) return
            val device = result.device
            Log.i(TAG, "Found: ${device.name} ${device.address}")
            stopScan()
            connect(device)
        }

        override fun onScanFailed(errorCode: Int) {
            Log.e(TAG, "Scan failed: $errorCode")
            scanTimeoutJob?.cancel()
            scanTimeoutJob = null
            _connectionState.value = ConnectionState.DISCONNECTED
            onScanError?.invoke("扫描失败 (code=$errorCode)")
        }
    }

    @SuppressLint("MissingPermission")
    private fun connect(device: BluetoothDevice) {
        cancelHandshakeJobs()
        reconnectJob?.cancel()
        reconnectJob = null
        resetWriteQueue()
        resetRxBuffer()
        closeCurrentGatt(alreadyDisconnected = false)

        userInitiatedDisconnect = false
        lastDevice = device
        handshakeCompleted = false
        discoveryStarted = false
        retriedDiscovery = false
        _mtu.value = 23
        _connectionState.value = ConnectionState.CONNECTING

        val ctx = appContext ?: run {
            _connectionState.value = ConnectionState.DISCONNECTED
            return
        }
        val newGatt = try {
            device.connectGatt(ctx, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
        } catch (e: SecurityException) {
            _connectionState.value = ConnectionState.DISCONNECTED
            onScanError?.invoke("缺少蓝牙连接权限，请在系统设置中授权")
            return
        }
        if (newGatt == null) {
            _connectionState.value = ConnectionState.DISCONNECTED
            onScanError?.invoke("无法发起 BLE GATT 连接")
            return
        }
        synchronized(gattLock) {
            gatt = newGatt
        }

        // 15 秒总握手超时：覆盖物理连接、MTU 协商、服务发现和 Notify 订阅，直到进入 CONNECTED 才取消
        connectTimeoutJob = scope.launch {
            delay(15_000)
            if (gatt === newGatt && _connectionState.value == ConnectionState.CONNECTING) {
                failHandshake("GATT 握手超时，请复位开发板或取消后重试", newGatt)
            }
        }
    }

    private fun failHandshake(reason: String, targetGatt: BluetoothGatt? = null) {
        if (targetGatt != null && targetGatt != gatt) return
        Log.e(TAG, "Handshake failed: $reason")
        disconnect(userInitiated = false)
        onScanError?.invoke(reason)
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
        if (gatt != this.gatt || discoveryStarted) return
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
        if (gatt != this.gatt) return
        cancelHandshakeJobs()
        handshakeCompleted = true
        _connectionState.value = ConnectionState.CONNECTED
        Log.i(TAG, "Notify channel ready")
        syncAfterConnect()
    }

    private val gattCallback = object : BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                val closingTimeout = synchronized(gattLock) {
                    pendingCloseGatts.remove(gatt)
                }
                if (closingTimeout != null) {
                    closingTimeout.cancel()
                    Log.i(TAG, "Graceful disconnect completed (status=$status), closing GATT")
                    try {
                        gatt.close()
                    } catch (_: Exception) {
                    }
                    return
                }
            }
            if (gatt !== this@BleManager.gatt) return
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    if (status == BluetoothGatt.GATT_SUCCESS) {
                        reconnectJob?.cancel()
                        reconnectJob = null
                        Log.i(TAG, "Connected, scheduling MTU 247 & service discovery")
                        // 留出 400ms 错开板子端 slave_init_mtu_req 协商，避免双向 MTU Exchange 冲突；2.2s 兜底继续发现服务
                        mtuSetupJob?.cancel()
                        mtuSetupJob = scope.launch {
                            delay(400)
                            if (gatt !== this@BleManager.gatt || _connectionState.value != ConnectionState.CONNECTING) {
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
                        mtuFallbackJob?.cancel()
                        mtuFallbackJob = scope.launch {
                            delay(2_200)
                            if (gatt === this@BleManager.gatt &&
                                _connectionState.value == ConnectionState.CONNECTING &&
                                !discoveryStarted
                            ) {
                                Log.w(TAG, "MTU negotiation timed out, continuing to discoverServices()")
                                startServiceDiscovery(gatt)
                            }
                        }
                    } else {
                        Log.e(TAG, "Connected with error status=$status")
                        failHandshake("连接失败 (code=$status)", gatt)
                    }
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    val btOff = btDisabledBySystem || (bluetoothAdapter?.isEnabled != true)
                    val unexpected = !userInitiatedDisconnect && !btOff
                    val wasConnected = handshakeCompleted
                    val savedDev = lastDevice
                    Log.i(TAG, "Disconnected (status=$status, btOff=$btOff, unexpected=$unexpected, wasConnected=$wasConnected)")
                    performDisconnect(userInitiated = false, alreadyDisconnected = true)
                    if (btOff) {
                        return
                    }
                    if (unexpected && wasConnected && savedDev != null) {
                        onScanError?.invoke("蓝牙连接意外断开，正在自动重连...")
                        reconnectJob?.cancel()
                        reconnectJob = scope.launch {
                            delay(1500)
                            if (isActive && !userInitiatedDisconnect && _connectionState.value == ConnectionState.DISCONNECTED) {
                                connect(savedDev)
                            }
                        }
                    } else if (unexpected) {
                        onScanError?.invoke("连接/GATT 握手中断 (code=$status)，请确认未在系统蓝牙配对并重试")
                    } else if (status != BluetoothGatt.GATT_SUCCESS) {
                        onScanError?.invoke("连接失败 (code=$status)")
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
            startServiceDiscovery(gatt)
        }

        @SuppressLint("MissingPermission")
        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            if (gatt != this@BleManager.gatt) return
            if (status != BluetoothGatt.GATT_SUCCESS) {
                Log.e(TAG, "Service discovery failed: $status")
                failHandshake("GATT 服务发现失败 (code=$status)", gatt)
                return
            }
            val service = gatt.getService(SERVICE_UUID)
            val tx = service?.getCharacteristic(CHAR_TX_UUID)
            val rx = service?.getCharacteristic(CHAR_RX_UUID)
            if (service == null || tx == null || rx == null) {
                if (!retriedDiscovery) {
                    retriedDiscovery = true
                    discoveryStarted = false
                    val refreshed = refreshGattCache(gatt)
                    Log.w(TAG, "NUS service not ready (refreshed=$refreshed), retrying service discovery")
                    mtuSetupJob?.cancel()
                    mtuSetupJob = scope.launch {
                        delay(300)
                        if (gatt == this@BleManager.gatt && _connectionState.value == ConnectionState.CONNECTING) {
                            startServiceDiscovery(gatt)
                        }
                    }
                } else {
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
                    mtuSetupJob = scope.launch {
                        delay(200)
                        if (gatt == this@BleManager.gatt && _connectionState.value == ConnectionState.CONNECTING) {
                            if (!writeCccd(gatt, descriptor)) {
                                failHandshake("订阅 Notify CCCD 失败", gatt)
                            }
                        }
                    }
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
            if (gatt != this@BleManager.gatt) return
            if (status == BluetoothGatt.GATT_SUCCESS) {
                markConnected(gatt)
            } else {
                Log.e(TAG, "onDescriptorWrite failed: $status")
                failHandshake("开启数据通知失败 (code=$status)", gatt)
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
            val ack = synchronized(writeLock) { pendingWriteAck }
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
