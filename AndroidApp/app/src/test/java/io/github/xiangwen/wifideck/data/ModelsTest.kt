package io.github.xiangwen.wifideck.data

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ModelsTest {

    @Test
    fun bleMessageType_fromWireType_mapsAllKnownTypesAndReturnsNullForUnknown() {
        val expected = mapOf(
            "scan_begin" to BleMessageType.SCAN_BEGIN,
            "scan_end" to BleMessageType.SCAN_END,
            "sync_begin" to BleMessageType.SYNC_BEGIN,
            "sync_end" to BleMessageType.SYNC_END,
            "ap" to BleMessageType.AP,
            "st" to BleMessageType.STATUS,
            "cfg" to BleMessageType.CONFIG,
            "id" to BleMessageType.IDENTITY,
            "log" to BleMessageType.LOG
        )

        assertEquals(BleMessageType.entries.size, expected.size)
        expected.forEach { (wire, type) ->
            assertEquals(type, BleMessageType.fromWireType(wire))
        }
        assertNull(BleMessageType.fromWireType("unknown"))
        assertNull(BleMessageType.fromWireType(""))
    }

    @Test
    fun sniffMode_fromCode_mapsFirmwareEnumAndFallsBackToStop() {
        assertEquals(SniffMode.ALL, SniffMode.fromCode(0))
        assertEquals(SniffMode.BEACON, SniffMode.fromCode(1))
        assertEquals(SniffMode.PROBE, SniffMode.fromCode(2))
        assertEquals(SniffMode.DEAUTH, SniffMode.fromCode(3))
        assertEquals(SniffMode.EAPOL, SniffMode.fromCode(4))
        assertEquals(SniffMode.PWNAGOTCHI, SniffMode.fromCode(5))
        assertEquals(SniffMode.STOP, SniffMode.fromCode(6))

        assertEquals(SniffMode.STOP, SniffMode.fromCode(-1))
        assertEquals(SniffMode.STOP, SniffMode.fromCode(99))
        assertEquals("eapol", SniffMode.EAPOL.commandArg)
    }

    @Test
    fun sniffLogEntry_fromRaw_stripsKnownFramePrefixesAndPreservesRawText() {
        val mgmt = SniffLogEntry.fromRaw(1L, 1L, "[MGMT] Beacon SSID=Corp CH=6")
        assertEquals("Beacon SSID=Corp CH=6", mgmt.displayText)
        assertEquals("[MGMT] Beacon SSID=Corp CH=6", mgmt.text)

        val data = SniffLogEntry.fromRaw(2L, 2L, "[DATA] EAPOL M1/M4 11:22:33")
        assertEquals("EAPOL M1/M4 11:22:33", data.displayText)

        val ctrl = SniffLogEntry.fromRaw(3L, 3L, "[CTRL] RTS/CTS")
        assertEquals("RTS/CTS", ctrl.displayText)

        val ext = SniffLogEntry.fromRaw(4L, 4L, "[EXT] S1G Beacon")
        assertEquals("S1G Beacon", ext.displayText)

        val plain = SniffLogEntry.fromRaw(5L, 5L, "[INFO] Sniff started")
        assertEquals("[INFO] Sniff started", plain.displayText)
    }

    @Test
    fun sniffLogEntry_fromRaw_classifiesCategoriesAndHighlightsCorrectly() {
        val eapol = SniffLogEntry.fromRaw(1L, 1L, "[DATA] eapol handshake frame")
        assertEquals(SniffLogCategory.EAPOL, eapol.category)
        assertTrue(eapol.category.isHighlight)

        val deauth = SniffLogEntry.fromRaw(2L, 2L, "[MGMT] Deauth AA:BB -> CC:DD")
        assertEquals(SniffLogCategory.DEAUTH, deauth.category)
        assertTrue(deauth.category.isHighlight)

        val disassoc = SniffLogEntry.fromRaw(3L, 3L, "[MGMT] disassoc reason=8")
        assertEquals(SniffLogCategory.DEAUTH, disassoc.category)
        assertTrue(disassoc.category.isHighlight)

        val beacon = SniffLogEntry.fromRaw(4L, 4L, "[MGMT] Beacon HomeWiFi")
        assertEquals(SniffLogCategory.BEACON, beacon.category)
        assertFalse(beacon.category.isHighlight)

        val probe = SniffLogEntry.fromRaw(5L, 5L, "[MGMT] Probe Request")
        assertEquals(SniffLogCategory.PROBE, probe.category)
        assertFalse(probe.category.isHighlight)

        val other = SniffLogEntry.fromRaw(6L, 6L, "[CTRL] Ack")
        assertEquals(SniffLogCategory.FRAME, other.category)
        assertFalse(other.category.isHighlight)

        // Priority: EAPOL wins over Deauth/Beacon if both substrings appear
        val mixed = SniffLogEntry.fromRaw(7L, 7L, "[DATA] EAPOL near Beacon")
        assertEquals(SniffLogCategory.EAPOL, mixed.category)
    }

    @Test
    fun sniffLogEntry_fromRaw_formatsSlidingWindowSequenceNumbers() {
        val e1 = SniffLogEntry.fromRaw(10L, 1L, "[MGMT] Beacon A")
        val e2 = SniffLogEntry.fromRaw(11L, 42L, "[MGMT] Beacon B")
        val e3 = SniffLogEntry.fromRaw(500L, 500L, "[MGMT] Beacon C")
        val e4 = SniffLogEntry.fromRaw(1024L, 1024L, "[MGMT] Beacon D")

        assertEquals("001", e1.formattedSeq)
        assertEquals("042", e2.formattedSeq)
        assertEquals("500", e3.formattedSeq)
        assertEquals("1024", e4.formattedSeq)
        assertEquals(1024L, e4.id)
        assertEquals(1024L, e4.seq)
    }

    @Test
    fun apItemAndStatusAndConfigSyncHelpers_behaveAsExpected() {
        val ap24 = ApItem(index = 0, ssid = "Net24", bssid = "AA:BB:CC:DD:EE:01", channel = 13, rssi = -55)
        val ap5 = ApItem(index = 1, ssid = "Net5", bssid = "AA:BB:CC:DD:EE:02", channel = 36, rssi = -68)
        assertEquals("2.4G", ap24.band)
        assertEquals("5G", ap5.band)

        val idle = DeviceStatus()
        assertFalse(idle.attacking)
        assertFalse(idle.isBusy)

        val sniffingOnly = DeviceStatus(sniffing = true)
        assertFalse(sniffingOnly.attacking)
        assertTrue(sniffingOnly.isBusy)

        val jamming = DeviceStatus(signalJam = true)
        assertTrue(jamming.attacking)
        assertTrue(jamming.isBusy)

        assertFalse(ConfigSyncState(cfgReceived = false, idReceived = false).isSynced)
        assertFalse(ConfigSyncState(cfgReceived = true, idReceived = false).isSynced)
        assertFalse(ConfigSyncState(cfgReceived = false, idReceived = true).isSynced)
        assertTrue(ConfigSyncState(cfgReceived = true, idReceived = true).isSynced)
    }
}
