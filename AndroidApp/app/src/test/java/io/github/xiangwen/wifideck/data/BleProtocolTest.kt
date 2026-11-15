package io.github.xiangwen.wifideck.data

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class BleProtocolTest {

    @Test
    fun framer_handlesLfAndCrLfFragmentsIncludingSplitCrLf() {
        val framer = BleLineFramer()

        val chunk1 = "{\"t\":\"st\",\"ch\":1}\n{\"t\":\"cfg\",\"ap\":1}\r".toByteArray(Charsets.UTF_8)
        val chunk2 = "\n\r\n   \n{\"t\":\"id\",\"ssid\":\"Test\"}\r\n".toByteArray(Charsets.UTF_8)

        val out1 = framer.feed(chunk1)
        assertEquals(listOf("{\"t\":\"st\",\"ch\":1}"), out1)

        val out2 = framer.feed(chunk2)
        assertEquals(
            listOf(
                "{\"t\":\"cfg\",\"ap\":1}",
                "{\"t\":\"id\",\"ssid\":\"Test\"}"
            ),
            out2
        )
    }

    @Test
    fun framer_reassemblesMultiByteUtf8ChineseAcrossNotifyChunksWithoutCorruption() {
        val framer = BleLineFramer()
        val expected = "{\"t\":\"ap\",\"i\":0,\"s\":\"中文测试WiFi📶\",\"b\":\"AA:BB:CC:DD:EE:FF\",\"c\":36,\"r\":-45}"
        val rawPacket = (expected + "\n").toByteArray(Charsets.UTF_8)

        // Split into 2-byte Notify slices so 3-byte CJK and 4-byte emoji UTF-8 sequences span packets
        val collected = mutableListOf<String>()
        var idx = 0
        while (idx < rawPacket.size) {
            val end = (idx + 2).coerceAtMost(rawPacket.size)
            collected.addAll(framer.feed(rawPacket.copyOfRange(idx, end)))
            idx = end
        }

        assertEquals(listOf(expected), collected)
        assertTrue(collected.single().none { it == '\uFFFD' })
    }

    @Test
    fun framer_preservesJsonEscapesSplitAcrossChunks() {
        val framer = BleLineFramer()
        val expected = "{\"t\":\"log\",\"m\":\"SSID=\\\"A\\\\B\\u001bC\\\"\"}"
        val bytes = (expected + "\r\n").toByteArray(Charsets.UTF_8)
        val mid = bytes.size / 2

        assertEquals(emptyList<String>(), framer.feed(bytes.copyOfRange(0, mid)))
        assertEquals(listOf(expected), framer.feed(bytes.copyOfRange(mid, bytes.size)))
    }

    @Test
    fun framer_acceptsExact4096ByteLineAndRecoversAfterOversizeDropWithAndWithoutLf() {
        val framer = BleLineFramer(maxLineBytes = 4096)
        val warnings = mutableListOf<String>()

        // Exact 4096B line is accepted
        val exactLine = "a".repeat(4096)
        assertEquals(
            listOf(exactLine),
            framer.feed((exactLine + "\n").toByteArray(Charsets.UTF_8)) { warnings.add(it) }
        )
        assertTrue(warnings.isEmpty())

        // Case 1: >4096B across chunks without LF enters oversizeDrop, then recovers on next LF
        val part1 = "x".repeat(3000).toByteArray(Charsets.UTF_8)
        val part2 = "y".repeat(1500).toByteArray(Charsets.UTF_8)
        val tailAndNext = ("z".repeat(100) + "\n{\"t\":\"st\",\"ch\":6}\n").toByteArray(Charsets.UTF_8)

        assertEquals(emptyList<String>(), framer.feed(part1) { warnings.add(it) })
        assertEquals(emptyList<String>(), framer.feed(part2) { warnings.add(it) })
        assertEquals(1, warnings.size)

        val recovered1 = framer.feed(tailAndNext) { warnings.add(it) }
        assertEquals(listOf("{\"t\":\"st\",\"ch\":6}"), recovered1)

        // Case 2: Single chunk containing >4096B line terminated by LF drops only that line and keeps next
        val singleOversizeChunk = ("o".repeat(4097) + "\n{\"t\":\"st\",\"ch\":11}\n").toByteArray(Charsets.UTF_8)
        val recovered2 = framer.feed(singleOversizeChunk) { warnings.add(it) }
        assertEquals(listOf("{\"t\":\"st\",\"ch\":11}"), recovered2)
        assertEquals(2, warnings.size)
    }

    @Test
    fun framer_resetClearsHalfFrameAndOversizeDropState() {
        val framer = BleLineFramer(maxLineBytes = 16)
        assertEquals(emptyList<String>(), framer.feed("012345678901234567".toByteArray(Charsets.UTF_8)))

        framer.reset()
        assertEquals(
            listOf("{\"t\":\"st\"}"),
            framer.feed("{\"t\":\"st\"}\n".toByteArray(Charsets.UTF_8))
        )
    }

    @Test
    fun commandCodec_computesEffectivePayloadLimitsAcrossMtuValues() {
        assertEquals(20, BleCommandCodec.maxCommandPayloadBytes(10))
        assertEquals(20, BleCommandCodec.maxCommandPayloadBytes(23))
        assertEquals(21, BleCommandCodec.maxCommandPayloadBytes(24))
        assertEquals(219, BleCommandCodec.maxCommandPayloadBytes(222))
        assertEquals(219, BleCommandCodec.maxCommandPayloadBytes(247))
        assertEquals(219, BleCommandCodec.maxCommandPayloadBytes(512))
    }

    @Test
    fun commandCodec_sanitizesCrLfInjectionAndDetectsUrgentStops() {
        val empty = BleCommandCodec.prepareCommand(" \r\n\t ", mtu = 23)
        assertEquals(BleCommandCodec.PreparedCommand.Empty, empty)

        val sanitized = BleCommandCodec.prepareCommand("  set led on\r\n ", mtu = 23)
        assertEquals(
            BleCommandCodec.PreparedCommand.Ready("set led on", urgent = false, wireBytesLength = 10),
            sanitized
        )

        val stop1 = BleCommandCodec.prepareCommand("STOP\r\n", mtu = 23)
        val stop2 = BleCommandCodec.prepareCommand("stop all", mtu = 23)
        val stop3 = BleCommandCodec.prepareCommand("Stop Deauther", mtu = 23)
        assertTrue(stop1 is BleCommandCodec.PreparedCommand.Ready && stop1.urgent)
        assertTrue(stop2 is BleCommandCodec.PreparedCommand.Ready && stop2.urgent)
        assertTrue(stop3 is BleCommandCodec.PreparedCommand.Ready && stop3.urgent)
    }

    @Test
    fun commandCodec_enforces20ByteAnd219ByteUtf8Boundaries() {
        // At default MTU=23, limit is 20B
        val exactly20Ascii = "a".repeat(20)
        val twentyOneAscii = "a".repeat(21)
        assertTrue(BleCommandCodec.prepareCommand(exactly20Ascii, mtu = 23) is BleCommandCodec.PreparedCommand.Ready)
        val rejectedAt23 = BleCommandCodec.prepareCommand(twentyOneAscii, mtu = 23)
        assertTrue(rejectedAt23 is BleCommandCodec.PreparedCommand.Rejected)
        assertTrue((rejectedAt23 as BleCommandCodec.PreparedCommand.Rejected).reason.contains("20B"))

        // Multi-byte UTF-8 check at MTU=23: "set ap_ssid 中测" = 12 + 6 = 18B (OK); + "试" = 21B (Rejected)
        assertTrue(BleCommandCodec.prepareCommand("set ap_ssid 中测", mtu = 23) is BleCommandCodec.PreparedCommand.Ready)
        assertTrue(BleCommandCodec.prepareCommand("set ap_ssid 中测试", mtu = 23) is BleCommandCodec.PreparedCommand.Rejected)

        // At MTU=247 (or 512), upper limit is 219B
        val exactly219Ascii = "b".repeat(219)
        val twoTwentyAscii = "b".repeat(220)
        assertTrue(BleCommandCodec.prepareCommand(exactly219Ascii, mtu = 247) is BleCommandCodec.PreparedCommand.Ready)
        val rejectedAt247 = BleCommandCodec.prepareCommand(twoTwentyAscii, mtu = 247)
        assertTrue(rejectedAt247 is BleCommandCodec.PreparedCommand.Rejected)
        assertTrue((rejectedAt247 as BleCommandCodec.PreparedCommand.Rejected).reason.contains("219B"))

        // 73 Chinese characters = 219 UTF-8 bytes (Ready); + "x" = 220B (Rejected)
        val exactly219Utf8 = "中".repeat(73)
        val twoTwentyUtf8 = exactly219Utf8 + "x"
        assertTrue(BleCommandCodec.prepareCommand(exactly219Utf8, mtu = 512) is BleCommandCodec.PreparedCommand.Ready)
        assertTrue(BleCommandCodec.prepareCommand(twoTwentyUtf8, mtu = 512) is BleCommandCodec.PreparedCommand.Rejected)
    }

    @Test
    fun commandCodec_prepareTargetedCommand_validatesDeduplicatesSortsAndChecksMtu() {
        assertTrue(BleCommandCodec.prepareTargetedCommand("start deauther", emptyList(), mtu = 247) is BleCommandCodec.PreparedCommand.Rejected)
        assertTrue(BleCommandCodec.prepareTargetedCommand("start deauther", listOf(1, -2), mtu = 247) is BleCommandCodec.PreparedCommand.Rejected)

        val valid = BleCommandCodec.prepareTargetedCommand("start deauther\r\n", listOf(5, 1, 5, 3), mtu = 247)
        assertEquals(
            BleCommandCodec.PreparedCommand.Ready("start deauther 1,3,5", urgent = false, wireBytesLength = 20),
            valid
        )

        // At MTU=23 (20B max), "start deauther 1,3,5" is 20B (Ready); "start deauther 1,3,10" is 21B (Rejected)
        assertTrue(BleCommandCodec.prepareTargetedCommand("start deauther", listOf(1, 3, 5), mtu = 23) is BleCommandCodec.PreparedCommand.Ready)
        assertTrue(BleCommandCodec.prepareTargetedCommand("start deauther", listOf(1, 3, 10), mtu = 23) is BleCommandCodec.PreparedCommand.Rejected)
    }
}
