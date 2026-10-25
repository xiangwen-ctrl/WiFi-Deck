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
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.ParcelUuid
import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import java.util.UUID
import java.util.concurrent.ConcurrentLinkedQueue

/**
 * BLE 管理器：扫描 -> 连接 -> MTU 协商 -> 服务发现 -> 订阅 Notify。
 * 所有 BluetoothGatt 操作都是异步且不可并发的，必须严格按链式回调顺序执行。
 */
object BleManager {
    private const val TAG = "BleManager"

    val SERVICE_UUID: UUID = UUID.fromString("6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
    val CHAR_RX_UUID: UUID = UUID.fromString("6E400002-B5A3-F393-E0A9-E50E24DCCA9E")
    val CHAR_TX_UUID: UUID = UUID.fromString("6E400003-B5A3-F393-E0A9-E50E24DCCA9E")

    private val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

    enum class ConnectionState { DISCONNECTED, SCANNING, CONNECTING, CONNECTED }

    private val _connectionState = MutableStateFlow(ConnectionState.DISCONNECTED)
    val connectionState: StateFlow<ConnectionState> = _connectionState

    /** 收到一条完整 JSON 行（已去掉末尾 \n）时回调 */
    var onLine: ((String) -> Unit)? = null

    /** 扫描/连接/写入出错时回调（用于 UI Toast 提示） */
    var onScanError: ((String) -> Unit)? = null

    private var appContext: Context? = null
    private var bluetoothAdapter: BluetoothAdapter? = null
    private var gatt: BluetoothGatt? = null
    private var txCharacteristic: BluetoothGattCharacteristic? = null
    private var rxCharacteristic: BluetoothGattCharacteristic? = null

    private val lineBuffer = StringBuilder()

    // 串行命令发送队列：Android GATT 内部有 mDeviceBusy 锁，连续 write 会静默丢弃。
    // 用 writing 标志 + 40ms 间隔串行写出（no-response 写无可靠完成回调，故用时间间隔而非回调驱动）。
    private val scope = CoroutineScope(Dispatchers.IO + SupervisorJob())
    private val cmdQueue = ConcurrentLinkedQueue<String>()
    @Volatile private var writing = false
    private var scanTimeoutJob: Job? = null
    private var connectTimeoutJob: Job? = null

    fun init(context: Context) {
        appContext = context.applicationContext
        val manager = context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
        bluetoothAdapter = manager.adapter
    }

    fun startScan() {
        val adapter = bluetoothAdapter
        if (adapter == null) return
        if (!adapter.isEnabled) {
            _connectionState.value = ConnectionState.DISCONNECTED
            onScanError?.invoke("请先打开蓝牙")
            return
        }
        val scanner = adapter.bluetoothLeScanner
        if (scanner == null) {
            _connectionState.value = ConnectionState.DISCONNECTED
            onScanError?.invoke("当前设备不支持 BLE 或蓝牙不可用")
            return
        }
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
                onScanError?.invoke("扫描超时，未找到设备")
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

    fun disconnect() {
        scanTimeoutJob?.cancel()
        scanTimeoutJob = null
        connectTimeoutJob?.cancel()
        connectTimeoutJob = null
        cmdQueue.clear()
        writing = false
        // 清空残余半包，避免断线重连后第一条消息解析失败
        synchronized(lineBuffer) { lineBuffer.clear() }
        try {
            @SuppressLint("MissingPermission")
            gatt?.disconnect()
        } catch (_: Exception) {
        }
        gatt?.close()
        gatt = null
        txCharacteristic = null
        rxCharacteristic = null
        _connectionState.value = ConnectionState.DISCONNECTED
    }

    /** 将命令入队并触发串行写出（写到 RX 特征值 6E400002，手机 -> 板子） */
    fun sendCommand(cmd: String) {
        if (_connectionState.value != ConnectionState.CONNECTED) {
            onScanError?.invoke("设备未连接，请先连接 WiFi-Deck")
            return
        }
        cmdQueue.offer(cmd)
        flushWriteQueue()
    }

    private fun flushWriteQueue() {
        if (writing) return
        val rx = rxCharacteristic ?: return
        val g = gatt ?: return
        val cmd = cmdQueue.poll() ?: return
        writing = true
        try {
            @SuppressLint("MissingPermission")
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                g.writeCharacteristic(
                    rx,
                    (cmd + "\n").toByteArray(Charsets.UTF_8),
                    BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
                )
            } else {
                @Suppress("DEPRECATION")
                rx.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
                @Suppress("DEPRECATION")
                rx.value = (cmd + "\n").toByteArray(Charsets.UTF_8)
                @Suppress("DEPRECATION")
                g.writeCharacteristic(rx)
            }
        } catch (e: Exception) {
            Log.e(TAG, "writeCharacteristic failed: ${e.message}")
            writing = false
            onScanError?.invoke("指令写入失败")
            return
        }
        // 留出 GATT 写入与固件 loop() 消费的时间，再发下一条
        scope.launch {
            delay(40)
            writing = false
            flushWriteQueue()
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
            _connectionState.value = ConnectionState.DISCONNECTED
            onScanError?.invoke("扫描失败 (code=$errorCode)")
        }
    }

    @SuppressLint("MissingPermission")
    private fun connect(device: BluetoothDevice) {
        _connectionState.value = ConnectionState.CONNECTING
        val ctx = appContext ?: return
        gatt = device.connectGatt(ctx, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
        // 15 秒连接超时
        connectTimeoutJob?.cancel()
        connectTimeoutJob = scope.launch {
            delay(15_000)
            if (_connectionState.value == ConnectionState.CONNECTING) {
                disconnect()
                onScanError?.invoke("连接超时")
            }
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    if (status == BluetoothGatt.GATT_SUCCESS) {
                        connectTimeoutJob?.cancel()
                        connectTimeoutJob = null
                        Log.i(TAG, "Connected, requesting MTU 247")
                        gatt.requestMtu(247)
                    } else {
                        Log.e(TAG, "Connected with error status=$status")
                        disconnect()
                    }
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    if (status != BluetoothGatt.GATT_SUCCESS) {
                        onScanError?.invoke("连接失败 (code=$status)")
                    }
                    Log.i(TAG, "Disconnected")
                    disconnect()
                }
            }
        }

        override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
            Log.i(TAG, "MTU changed to $mtu (status=$status)")
            @SuppressLint("MissingPermission")
            gatt.discoverServices()
        }

        @SuppressLint("MissingPermission")
        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                Log.e(TAG, "Service discovery failed: $status")
                return
            }
            val service = gatt.getService(SERVICE_UUID) ?: return
            txCharacteristic = service.getCharacteristic(CHAR_TX_UUID)
            rxCharacteristic = service.getCharacteristic(CHAR_RX_UUID)

            val tx = txCharacteristic ?: return
            gatt.setCharacteristicNotification(tx, true)
            val descriptor = tx.getDescriptor(CCCD_UUID)
            if (descriptor != null) {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    gatt.writeDescriptor(descriptor, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
                } else {
                    descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                    gatt.writeDescriptor(descriptor)
                }
            } else {
                _connectionState.value = ConnectionState.CONNECTED
                syncAfterConnect()
            }
        }

        @SuppressLint("MissingPermission")
        override fun onDescriptorWrite(
            gatt: BluetoothGatt,
            descriptor: BluetoothGattDescriptor,
            status: Int
        ) {
            if (status == BluetoothGatt.GATT_SUCCESS) {
                _connectionState.value = ConnectionState.CONNECTED
                Log.i(TAG, "Notify channel ready")
                syncAfterConnect()
            }
        }

        override fun onCharacteristicWrite(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int
        ) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                Log.e(TAG, "Write failed: $status")
            }
        }

        // Android 13+ (API 33+) 走这里
        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray
        ) {
            handleIncomingBytes(value)
        }

        // Android 8.0 ~ 12 (API 26 ~ 32) 走这里
        @Deprecated("Deprecated in API 33")
        @Suppress("DEPRECATION")
        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic
        ) {
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
        val chunk = value.toString(Charsets.UTF_8)
        synchronized(lineBuffer) {
            lineBuffer.append(chunk)
            var newlineIdx = lineBuffer.indexOf("\n")
            while (newlineIdx != -1) {
                val completeLine = lineBuffer.substring(0, newlineIdx).trim()
                lineBuffer.delete(0, newlineIdx + 1)
                if (completeLine.isNotEmpty()) {
                    onLine?.invoke(completeLine)
                }
                newlineIdx = lineBuffer.indexOf("\n")
            }
        }
    }
}
