package io.github.xiangwen.wifideck.data

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.UUID

class BleReconnectTrackerTest {

    @Test
    fun markConnected_armsAutoReconnectAndSurvivesIntermediateRetrySteps() {
        val tracker = BleReconnectTracker()
        assertFalse(tracker.shouldAutoReconnect)
        assertEquals(ReconnectPhase.IDLE, tracker.uiState.phase)

        val initial = tracker.markConnected()
        assertTrue(tracker.shouldAutoReconnect)
        assertEquals(ReconnectPhase.IDLE, initial.phase)
        assertEquals(0, initial.attempt)
        assertNull(initial.lastError)

        // Simulates failed single connect() retry: shouldAutoReconnect remains armed
        val step1 = tracker.nextReconnectStep(status = 8, hasCachedDevice = true, errorMessage = "conn timeout")
        assertTrue(step1 is ReconnectStep.Retry)
        assertTrue(tracker.shouldAutoReconnect)

        val step2 = tracker.nextReconnectStep(status = 8, hasCachedDevice = true, errorMessage = "conn timeout 2")
        assertTrue(step2 is ReconnectStep.Retry)
        assertTrue(tracker.shouldAutoReconnect)

        // Subsequent successful handshake resets attempt counter and keeps session armed
        val reconnected = tracker.markConnected()
        assertTrue(tracker.shouldAutoReconnect)
        assertEquals(ReconnectPhase.IDLE, reconnected.phase)
        assertEquals(0, reconnected.attempt)
        assertNull(reconnected.lastError)
    }

    @Test
    fun directAttempts_transitionToScanFallback_thenExhaustAtMaxTotalAttempts_andRecoverOnRetry() {
        assertEquals(1, BleReconnectTracker.MAX_DIRECT_ATTEMPTS)
        assertEquals(5, BleReconnectTracker.MAX_TOTAL_ATTEMPTS)

        val tracker = BleReconnectTracker()
        tracker.markConnected()

        // Attempts 1..MAX_DIRECT_ATTEMPTS (1) -> RECONNECTING_DIRECT with DIRECT_BACKOFFS_MS
        for (idx in 1..BleReconnectTracker.MAX_DIRECT_ATTEMPTS) {
            val step = tracker.nextReconnectStep(status = 8, hasCachedDevice = true, errorMessage = "err $idx")
            assertEquals(
                ReconnectStep.Retry(
                    phase = ReconnectPhase.RECONNECTING_DIRECT,
                    attempt = idx,
                    maxAttempts = BleReconnectTracker.MAX_TOTAL_ATTEMPTS,
                    delayMs = BleReconnectTracker.DIRECT_BACKOFFS_MS[idx - 1],
                    useScan = false
                ),
                step
            )
            assertTrue(tracker.uiState.isReconnecting)
            assertFalse(tracker.uiState.isExhausted)
            assertEquals(ReconnectPhase.RECONNECTING_DIRECT, tracker.uiState.phase)
        }

        // Attempts 2..MAX_TOTAL_ATTEMPTS (5) -> RECONNECTING_SCAN with SCAN_BACKOFFS_MS
        for (idx in (BleReconnectTracker.MAX_DIRECT_ATTEMPTS + 1)..BleReconnectTracker.MAX_TOTAL_ATTEMPTS) {
            val step = tracker.nextReconnectStep(status = 8, hasCachedDevice = true, errorMessage = "scan $idx")
            val scanBackoffIdx = idx - BleReconnectTracker.MAX_DIRECT_ATTEMPTS - 1
            assertEquals(
                ReconnectStep.Retry(
                    phase = ReconnectPhase.RECONNECTING_SCAN,
                    attempt = idx,
                    maxAttempts = BleReconnectTracker.MAX_TOTAL_ATTEMPTS,
                    delayMs = BleReconnectTracker.SCAN_BACKOFFS_MS[scanBackoffIdx],
                    useScan = true
                ),
                step
            )
            assertTrue(tracker.uiState.isReconnecting)
            assertEquals(ReconnectPhase.RECONNECTING_SCAN, tracker.uiState.phase)
        }

        // Next failure after MAX_TOTAL_ATTEMPTS -> EXHAUSTED with error
        val exhaustedStep = tracker.nextReconnectStep(
            status = 8,
            hasCachedDevice = true,
            errorMessage = "最终重连失败"
        )
        assertTrue(exhaustedStep is ReconnectStep.Exhausted)
        val exhaustedState = (exhaustedStep as ReconnectStep.Exhausted).uiState
        assertEquals(ReconnectPhase.EXHAUSTED, exhaustedState.phase)
        assertEquals(BleReconnectTracker.MAX_TOTAL_ATTEMPTS, exhaustedState.attempt)
        assertEquals(BleReconnectTracker.MAX_TOTAL_ATTEMPTS, exhaustedState.maxAttempts)
        assertEquals("最终重连失败", exhaustedState.lastError)
        assertTrue(tracker.uiState.isExhausted)
        assertFalse(tracker.uiState.isReconnecting)
        assertFalse(tracker.shouldAutoReconnect)

        // Further calls while EXHAUSTED are ignored
        assertEquals(ReconnectStep.Ignore, tracker.nextReconnectStep(status = 8, hasCachedDevice = true))

        // Manual retry recovers from EXHAUSTED and restarts at attempt 1 direct
        val retried = tracker.onManualRetry(hasCachedDevice = true)
        assertEquals(ReconnectPhase.IDLE, retried.phase)
        assertEquals(0, retried.attempt)
        assertNull(retried.lastError)
        assertTrue(tracker.shouldAutoReconnect)

        val postRetryStep = tracker.nextReconnectStep(status = 8, hasCachedDevice = true)
        assertEquals(
            ReconnectStep.Retry(
                phase = ReconnectPhase.RECONNECTING_DIRECT,
                attempt = 1,
                maxAttempts = BleReconnectTracker.MAX_TOTAL_ATTEMPTS,
                delayMs = BleReconnectTracker.DIRECT_BACKOFFS_MS[0],
                useScan = false
            ),
            postRetryStep
        )
    }

    @Test
    fun gatt133OrMissingCachedDevice_fallsBackDirectlyToScan_andUserDisconnectDisarms() {
        // Case 1: GATT 133 triggers immediate scan fallback on attempt 1 and sticks for subsequent retries
        val tracker133 = BleReconnectTracker()
        tracker133.markConnected()

        val gatt133Step = tracker133.nextReconnectStep(
            status = BleReconnectTracker.GATT_ERROR_133,
            hasCachedDevice = true,
            errorMessage = "gatt 133"
        )
        assertEquals(
            ReconnectStep.Retry(
                phase = ReconnectPhase.RECONNECTING_SCAN,
                attempt = 1,
                maxAttempts = BleReconnectTracker.MAX_TOTAL_ATTEMPTS,
                delayMs = BleReconnectTracker.SCAN_BACKOFFS_MS[0],
                useScan = true
            ),
            gatt133Step
        )

        val followUpStep = tracker133.nextReconnectStep(status = 8, hasCachedDevice = true)
        assertEquals(
            ReconnectStep.Retry(
                phase = ReconnectPhase.RECONNECTING_SCAN,
                attempt = 2,
                maxAttempts = BleReconnectTracker.MAX_TOTAL_ATTEMPTS,
                delayMs = BleReconnectTracker.SCAN_BACKOFFS_MS[1],
                useScan = true
            ),
            followUpStep
        )
        assertEquals(ReconnectPhase.RECONNECTING_SCAN, tracker133.uiState.phase)

        // Case 2: Missing cached device falls back directly to scan phase
        val trackerNoCache = BleReconnectTracker()
        trackerNoCache.markConnected()
        val noCacheStep = trackerNoCache.nextReconnectStep(status = 0, hasCachedDevice = false)
        assertEquals(
            ReconnectStep.Retry(
                phase = ReconnectPhase.RECONNECTING_SCAN,
                attempt = 1,
                maxAttempts = BleReconnectTracker.MAX_TOTAL_ATTEMPTS,
                delayMs = BleReconnectTracker.SCAN_BACKOFFS_MS[0],
                useScan = true
            ),
            noCacheStep
        )

        // Case 3: onUserDisconnect() disarms auto-reconnect cleanly
        val disconnectedState = trackerNoCache.onUserDisconnect()
        assertFalse(trackerNoCache.shouldAutoReconnect)
        assertEquals(ReconnectPhase.IDLE, disconnectedState.phase)
        assertEquals(0, disconnectedState.attempt)
        assertNull(disconnectedState.lastError)
        assertEquals(ReconnectStep.Ignore, trackerNoCache.nextReconnectStep(status = 8, hasCachedDevice = true))
    }

    @Test
    fun connectTimeoutOrForceScanFallback_switchesToScanAndProgressesScanBackoffMonotonically() {
        // Simulates logcat 0x0108 disconnect -> 1 fast direct connect -> timeout -> immediate scan fallback
        val tracker = BleReconnectTracker()
        tracker.markConnected()

        val attempt1 = tracker.nextReconnectStep(status = 8, hasCachedDevice = true, errorMessage = "disconnect 8")
        assertEquals(
            ReconnectStep.Retry(
                phase = ReconnectPhase.RECONNECTING_DIRECT,
                attempt = 1,
                maxAttempts = 5,
                delayMs = 800L,
                useScan = false
            ),
            attempt1
        )

        val attempt2 = tracker.nextReconnectStep(
            status = BleReconnectTracker.GATT_ERROR_CONNECT_TIMEOUT,
            hasCachedDevice = true,
            errorMessage = "direct timeout",
            forceScanFallback = true
        )
        assertEquals(
            ReconnectStep.Retry(
                phase = ReconnectPhase.RECONNECTING_SCAN,
                attempt = 2,
                maxAttempts = 5,
                delayMs = 1000L,
                useScan = true
            ),
            attempt2
        )

        // Even with custom maxDirectAttempts=3, GATT_ERROR_CONNECT_TIMEOUT forces scan from scan backoff[0]
        val customTracker = BleReconnectTracker(
            maxTotalAttempts = 5,
            maxDirectAttempts = 3,
            directBackoffsMs = longArrayOf(500L, 1000L, 1500L),
            scanBackoffsMs = longArrayOf(1200L, 2400L)
        )
        customTracker.markConnected()

        val customAttempt1 = customTracker.nextReconnectStep(status = 8, hasCachedDevice = true)
        assertEquals(false, (customAttempt1 as ReconnectStep.Retry).useScan)
        assertEquals(500L, customAttempt1.delayMs)

        val customAttempt2 = customTracker.nextReconnectStep(
            status = BleReconnectTracker.GATT_ERROR_CONNECT_TIMEOUT,
            hasCachedDevice = true
        )
        assertEquals(true, (customAttempt2 as ReconnectStep.Retry).useScan)
        assertEquals(1200L, customAttempt2.delayMs)

        // Next failure stays on scan and clamps gracefully to scanBackoffsMs.last()
        val customAttempt3 = customTracker.nextReconnectStep(status = 22, hasCachedDevice = true)
        assertEquals(2400L, (customAttempt3 as ReconnectStep.Retry).delayMs)
        val customAttempt4 = customTracker.nextReconnectStep(status = -1, hasCachedDevice = true)
        assertEquals(2400L, (customAttempt4 as ReconnectStep.Retry).delayMs)

        // Manual retry direct failure with forceScanFallback=true must not repeat another blind direct connect
        tracker.exhaustNow("exhausted")
        tracker.onManualRetry(hasCachedDevice = true)
        val postManualDirectFailure = tracker.nextReconnectStep(
            status = 22,
            hasCachedDevice = true,
            forceScanFallback = true
        )
        assertEquals(
            ReconnectStep.Retry(
                phase = ReconnectPhase.RECONNECTING_SCAN,
                attempt = 1,
                maxAttempts = 5,
                delayMs = BleReconnectTracker.SCAN_BACKOFFS_MS[0],
                useScan = true
            ),
            postManualDirectFailure
        )
    }

    @Test
    fun timeoutPolicyAndEdgeConfigurations_keepBoundsAndGuardEmptyBackoffs() {
        assertEquals(4_500L, BleReconnectTracker.DIRECT_CONNECT_TIMEOUT_MS)
        assertEquals(8_000L, BleReconnectTracker.SCAN_CONNECT_TIMEOUT_MS)
        assertEquals(10_000L, BleReconnectTracker.GATT_HANDSHAKE_TIMEOUT_MS)
        assertEquals(10_000L, BleReconnectTracker.SCAN_TIMEOUT_MS)

        assertEquals(
            BleReconnectTracker.DIRECT_CONNECT_TIMEOUT_MS,
            BleReconnectTracker.physicalConnectTimeoutMs(fromScan = false)
        )
        assertEquals(
            BleReconnectTracker.SCAN_CONNECT_TIMEOUT_MS,
            BleReconnectTracker.physicalConnectTimeoutMs(fromScan = true)
        )
        assertTrue(BleReconnectTracker.DIRECT_CONNECT_TIMEOUT_MS < BleReconnectTracker.SCAN_CONNECT_TIMEOUT_MS)
        assertTrue(BleReconnectTracker.SCAN_CONNECT_TIMEOUT_MS < BleReconnectTracker.GATT_HANDSHAKE_TIMEOUT_MS)

        // First scan starts after 1 direct backoff + 1 short direct connect timeout + 1st scan backoff (< 7s total)
        val firstScanDelayMs = BleReconnectTracker.DIRECT_BACKOFFS_MS[0] +
            BleReconnectTracker.DIRECT_CONNECT_TIMEOUT_MS +
            BleReconnectTracker.SCAN_BACKOFFS_MS[0]
        assertTrue("Expected scan fallback in <7s, got ${firstScanDelayMs}ms", firstScanDelayMs < 7_000L)

        // Zero direct attempts + empty backoff arrays must safely fall back to scan with 0ms delay
        val edgeTracker = BleReconnectTracker(
            maxTotalAttempts = 2,
            maxDirectAttempts = 0,
            directBackoffsMs = longArrayOf(),
            scanBackoffsMs = longArrayOf()
        )
        edgeTracker.markConnected()
        val s1 = edgeTracker.nextReconnectStep(status = 8, hasCachedDevice = true)
        assertEquals(
            ReconnectStep.Retry(
                phase = ReconnectPhase.RECONNECTING_SCAN,
                attempt = 1,
                maxAttempts = 2,
                delayMs = 0L,
                useScan = true
            ),
            s1
        )
        val manualNoCache = edgeTracker.onManualRetry(hasCachedDevice = false)
        assertFalse(edgeTracker.shouldAutoReconnect)
        assertEquals(ReconnectPhase.IDLE, manualNoCache.phase)
    }

    @Test
    fun btOffRace_latchesDisconnectBeforeBroadcast_andAdapterOnClearsExhausted() {
        val tracker = BleReconnectTracker()
        tracker.markConnected()

        // Connection drops to DISCONNECTED while BT is already off, before ACTION_STATE_CHANGED broadcast
        tracker.onDisconnectWhileBtOff(wasActiveSession = true)
        assertFalse(tracker.shouldAutoReconnect)
        assertEquals(ReconnectPhase.IDLE, tracker.uiState.phase)
        assertTrue(tracker.wasSessionActiveForBtOff(currentConnectionActive = false))

        // Subsequent onBluetoothAdapterOff(false) still returns true and consumes the latch
        assertTrue(tracker.onBluetoothAdapterOff(currentConnectionActive = false))
        assertFalse(tracker.wasSessionActiveForBtOff(currentConnectionActive = false))
        assertFalse(tracker.onBluetoothAdapterOff(currentConnectionActive = false))

        // Inactive session does not falsely trigger BT-off reconnect latch
        tracker.onDisconnectWhileBtOff(wasActiveSession = false)
        assertFalse(tracker.onBluetoothAdapterOff(currentConnectionActive = false))

        // Adapter ON clears EXHAUSTED state back to IDLE
        val exhausted = tracker.exhaustNow("timeout")
        assertTrue(exhausted.isExhausted)
        val afterBtOn = tracker.onBluetoothAdapterOn()
        assertEquals(ReconnectPhase.IDLE, afterBtOn.phase)
        assertNull(afterBtOn.lastError)
    }

    @Test
    fun writeAckTracker_enforcesQueueBoundAndIsolatesStaleCallbacks() {
        assertEquals(15, WriteAckTracker.MAX_CMD_QUEUE_SIZE)
        assertTrue(WriteAckTracker.MAX_CMD_QUEUE_SIZE <= 15)

        val ackTracker = WriteAckTracker()
        val rxUuid = BleManager.CHAR_RX_UUID
        val txUuid = BleManager.CHAR_TX_UUID
        val randomUuid = UUID.randomUUID()

        assertEquals(0L, ackTracker.currentActiveSeq())
        assertFalse(ackTracker.matches(0L, rxUuid, rxUuid))

        val seq1 = ackTracker.nextSeq()
        val seq2 = ackTracker.nextSeq()
        val seq3 = ackTracker.nextSeq()
        assertEquals(1L, seq1)
        assertEquals(2L, seq2)
        assertEquals(3L, seq3)
        assertEquals(seq3, ackTracker.currentActiveSeq())

        // Stale seq1/seq2 callbacks must NOT match active seq3
        assertFalse(ackTracker.matches(seq1, rxUuid, rxUuid))
        assertFalse(ackTracker.matches(seq2, rxUuid, rxUuid))

        // Wrong or null characteristic UUID must NOT match active seq3
        assertFalse(ackTracker.matches(seq3, txUuid, rxUuid))
        assertFalse(ackTracker.matches(seq3, randomUuid, rxUuid))
        assertFalse(ackTracker.matches(seq3, null, rxUuid))

        // Exact active seq + expected UUID matches
        assertTrue(ackTracker.matches(seq3, rxUuid, rxUuid))

        // Disarming a stale seq does not disturb active seq3
        ackTracker.disarm(seq1)
        assertEquals(seq3, ackTracker.currentActiveSeq())
        assertTrue(ackTracker.matches(seq3, rxUuid, rxUuid))

        // Disarming active seq clears matching
        ackTracker.disarm(seq3)
        assertEquals(0L, ackTracker.currentActiveSeq())
        assertFalse(ackTracker.matches(seq3, rxUuid, rxUuid))

        // Reset clears active seq without breaking monotonic sequence counter
        val seq4 = ackTracker.nextSeq()
        assertEquals(4L, seq4)
        ackTracker.reset()
        assertEquals(0L, ackTracker.currentActiveSeq())
        assertFalse(ackTracker.matches(seq4, rxUuid, rxUuid))
        assertEquals(5L, ackTracker.nextSeq())
    }
}
