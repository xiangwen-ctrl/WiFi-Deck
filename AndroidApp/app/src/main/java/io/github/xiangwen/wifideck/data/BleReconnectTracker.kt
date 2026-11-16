package io.github.xiangwen.wifideck.data

import java.util.UUID

enum class ReconnectPhase {
    IDLE,
    RECONNECTING_DIRECT,
    RECONNECTING_SCAN,
    EXHAUSTED
}

data class ReconnectUiState(
    val phase: ReconnectPhase = ReconnectPhase.IDLE,
    val attempt: Int = 0,
    val maxAttempts: Int = BleReconnectTracker.MAX_TOTAL_ATTEMPTS,
    val lastError: String? = null
) {
    val isReconnecting: Boolean
        get() = phase == ReconnectPhase.RECONNECTING_DIRECT || phase == ReconnectPhase.RECONNECTING_SCAN

    val isExhausted: Boolean
        get() = phase == ReconnectPhase.EXHAUSTED
}

sealed class ReconnectStep {
    data object Ignore : ReconnectStep()
    data class Retry(
        val phase: ReconnectPhase,
        val attempt: Int,
        val maxAttempts: Int,
        val delayMs: Long,
        val useScan: Boolean
    ) : ReconnectStep()

    data class Exhausted(val uiState: ReconnectUiState) : ReconnectStep()
}

/**
 * 纯 Kotlin BLE 会话级重连状态机与 #12 蓝牙开关竞态追踪器。
 * 将物理连接握手成功标志与会话级自动重连资格解耦，避免单次重连失败后死锁。
 */
class BleReconnectTracker(
    private val maxTotalAttempts: Int = MAX_TOTAL_ATTEMPTS,
    private val maxDirectAttempts: Int = MAX_DIRECT_ATTEMPTS,
    private val directBackoffsMs: LongArray = DIRECT_BACKOFFS_MS,
    private val scanBackoffsMs: LongArray = SCAN_BACKOFFS_MS
) {
    companion object {
        const val MAX_TOTAL_ATTEMPTS = 5
        const val MAX_DIRECT_ATTEMPTS = 1
        const val GATT_ERROR_133 = 133
        const val GATT_ERROR_CONNECT_TIMEOUT = -2
        const val DIRECT_CONNECT_TIMEOUT_MS = 4_500L
        const val SCAN_CONNECT_TIMEOUT_MS = 8_000L
        const val GATT_HANDSHAKE_TIMEOUT_MS = 10_000L
        const val SCAN_TIMEOUT_MS = 10_000L
        val DIRECT_BACKOFFS_MS = longArrayOf(800L)
        val SCAN_BACKOFFS_MS = longArrayOf(1000L, 2000L, 3500L, 5000L)

        fun physicalConnectTimeoutMs(fromScan: Boolean): Long {
            return if (fromScan) SCAN_CONNECT_TIMEOUT_MS else DIRECT_CONNECT_TIMEOUT_MS
        }
    }

    private val lock = Any()

    @Volatile
    var shouldAutoReconnect: Boolean = false
        private set

    @Volatile
    var uiState: ReconnectUiState = ReconnectUiState(maxAttempts = maxTotalAttempts)
        private set

    private var attempt: Int = 0
    private var directAttempt: Int = 0
    private var scanAttempt: Int = 0
    private var preferScanFallback: Boolean = false
    private var btOffSessionLatched: Boolean = false

    fun markConnected(): ReconnectUiState = synchronized(lock) {
        shouldAutoReconnect = true
        attempt = 0
        directAttempt = 0
        scanAttempt = 0
        preferScanFallback = false
        btOffSessionLatched = false
        uiState = ReconnectUiState(
            phase = ReconnectPhase.IDLE,
            attempt = 0,
            maxAttempts = maxTotalAttempts,
            lastError = null
        )
        uiState
    }

    fun onUserDisconnect(): ReconnectUiState = synchronized(lock) {
        clearAllLocked(clearLatch = true)
        uiState
    }

    fun onSessionAbandoned(): ReconnectUiState = synchronized(lock) {
        clearAllLocked(clearLatch = true)
        uiState
    }

    fun onManualRetry(hasCachedDevice: Boolean): ReconnectUiState = synchronized(lock) {
        shouldAutoReconnect = hasCachedDevice
        attempt = 0
        directAttempt = 0
        scanAttempt = 0
        preferScanFallback = !hasCachedDevice
        btOffSessionLatched = false
        uiState = ReconnectUiState(
            phase = ReconnectPhase.IDLE,
            attempt = 0,
            maxAttempts = maxTotalAttempts,
            lastError = null
        )
        uiState
    }

    fun nextReconnectStep(
        status: Int,
        hasCachedDevice: Boolean,
        errorMessage: String? = null,
        forceScanFallback: Boolean = false
    ): ReconnectStep = synchronized(lock) {
        if (!shouldAutoReconnect) {
            return ReconnectStep.Ignore
        }
        if (attempt >= maxTotalAttempts) {
            shouldAutoReconnect = false
            preferScanFallback = false
            val exhaustedState = ReconnectUiState(
                phase = ReconnectPhase.EXHAUSTED,
                attempt = attempt,
                maxAttempts = maxTotalAttempts,
                lastError = errorMessage ?: "自动重连失败（已尝试 $maxTotalAttempts 次），请点击重试"
            )
            uiState = exhaustedState
            return ReconnectStep.Exhausted(exhaustedState)
        }

        attempt += 1
        if (status == GATT_ERROR_133 ||
            status == GATT_ERROR_CONNECT_TIMEOUT ||
            forceScanFallback ||
            !hasCachedDevice
        ) {
            preferScanFallback = true
        }
        val useScan = preferScanFallback || !hasCachedDevice || attempt > maxDirectAttempts
        val phase = if (useScan) ReconnectPhase.RECONNECTING_SCAN else ReconnectPhase.RECONNECTING_DIRECT
        val delayMs = if (useScan) {
            scanAttempt += 1
            if (scanBackoffsMs.isEmpty()) {
                0L
            } else {
                val scanIdx = (scanAttempt - 1).coerceIn(0, scanBackoffsMs.lastIndex)
                scanBackoffsMs[scanIdx]
            }
        } else {
            directAttempt += 1
            if (directBackoffsMs.isEmpty()) {
                0L
            } else {
                val directIdx = (directAttempt - 1).coerceIn(0, directBackoffsMs.lastIndex)
                directBackoffsMs[directIdx]
            }
        }

        uiState = ReconnectUiState(
            phase = phase,
            attempt = attempt,
            maxAttempts = maxTotalAttempts,
            lastError = errorMessage
        )
        return ReconnectStep.Retry(
            phase = phase,
            attempt = attempt,
            maxAttempts = maxTotalAttempts,
            delayMs = delayMs,
            useScan = useScan
        )
    }

    fun exhaustNow(errorMessage: String): ReconnectUiState = synchronized(lock) {
        shouldAutoReconnect = false
        preferScanFallback = false
        uiState = ReconnectUiState(
            phase = ReconnectPhase.EXHAUSTED,
            attempt = attempt,
            maxAttempts = maxTotalAttempts,
            lastError = errorMessage
        )
        uiState
    }

    /**
     * #12 竞态兜底：若 STATE_DISCONNECTED 在 ACTION_STATE_CHANGED 广播前先到达且适配器已关闭，
     * 先锁存当前是否处于连接/扫描/重连活动会话，供随后到达的 onBluetoothAdapterOff() 消费。
     */
    fun onDisconnectWhileBtOff(wasActiveSession: Boolean) = synchronized(lock) {
        if (wasActiveSession || uiState.isReconnecting || shouldAutoReconnect) {
            btOffSessionLatched = true
        }
        clearAllLocked(clearLatch = false)
    }

    fun wasSessionActiveForBtOff(currentConnectionActive: Boolean = false): Boolean = synchronized(lock) {
        currentConnectionActive || uiState.isReconnecting || btOffSessionLatched
    }

    fun onBluetoothAdapterOff(currentConnectionActive: Boolean): Boolean = synchronized(lock) {
        val wasActive = currentConnectionActive || uiState.isReconnecting || btOffSessionLatched
        clearAllLocked(clearLatch = true)
        wasActive
    }

    fun onBluetoothAdapterOn(): ReconnectUiState = synchronized(lock) {
        btOffSessionLatched = false
        if (uiState.isExhausted) {
            uiState = ReconnectUiState(
                phase = ReconnectPhase.IDLE,
                attempt = 0,
                maxAttempts = maxTotalAttempts,
                lastError = null
            )
        }
        uiState
    }

    private fun clearAllLocked(clearLatch: Boolean) {
        shouldAutoReconnect = false
        attempt = 0
        directAttempt = 0
        scanAttempt = 0
        preferScanFallback = false
        if (clearLatch) {
            btOffSessionLatched = false
        }
        uiState = ReconnectUiState(
            phase = ReconnectPhase.IDLE,
            attempt = 0,
            maxAttempts = maxTotalAttempts,
            lastError = null
        )
    }
}

/**
 * 纯 Kotlin BLE 写确认序号追踪器：
 * 1) 队列上限对齐固件 CMD_RING_SIZE=16（15 个可用槽位）；
 * 2) 严格校对当前在途写入序号与特征 UUID，隔离超时后延迟到达的陈旧 onCharacteristicWrite 回调。
 */
class WriteAckTracker {
    companion object {
        const val MAX_CMD_QUEUE_SIZE = 15
    }

    private val lock = Any()
    private var seqCounter = 0L
    private var activeSeq = 0L

    fun nextSeq(): Long = synchronized(lock) {
        seqCounter += 1L
        activeSeq = seqCounter
        activeSeq
    }

    fun disarm(seq: Long) = synchronized(lock) {
        if (activeSeq == seq) {
            activeSeq = 0L
        }
    }

    fun reset() = synchronized(lock) {
        activeSeq = 0L
    }

    fun currentActiveSeq(): Long = synchronized(lock) { activeSeq }

    fun matches(activeSeq: Long, callbackUuid: UUID?, expectedUuid: UUID): Boolean = synchronized(lock) {
        activeSeq > 0L && this.activeSeq == activeSeq && callbackUuid == expectedUuid
    }
}
