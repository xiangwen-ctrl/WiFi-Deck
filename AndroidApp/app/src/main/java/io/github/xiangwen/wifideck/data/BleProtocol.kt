package io.github.xiangwen.wifideck.data

import kotlinx.coroutines.flow.StateFlow
import java.io.ByteArrayOutputStream

/** BLE 传输层抽象：生产环境由 [BleManager] 实现，通过 [io.github.xiangwen.wifideck.ui.MainViewModel.Factory] 注入，JVM 单测可注入替身 */
interface BleTransport {
    val connectionState: StateFlow<BleManager.ConnectionState>
    val reconnectState: StateFlow<ReconnectUiState>
    val mtu: StateFlow<Int>
    var onLine: ((String) -> Unit)?
    var onScanError: ((String) -> Unit)?

    fun sendCommand(cmd: String): Boolean
    fun sendEmergencyStop(): Boolean
    fun sendTargetedCommand(baseCmd: String, indices: Collection<Int>): Boolean
    fun retryReconnect(): Boolean = false
    fun disconnect(userInitiated: Boolean = true)
    fun onBluetoothAdapterOff(): Boolean
    fun onBluetoothAdapterOn()
}

/**
 * 纯 Kotlin BLE 上行字节流分帧器：
 * 按字节边界累积直到遇见 `0x0A` (`\n`)，剥离末尾 `0x0D` (`\r`) 后再整行 UTF-8 解码，
 * 避免跨 Notify 分片的多字节中文乱码，并在单行超出 [maxLineBytes] 后丢弃至下一个 `\n` 再恢复解析。
 */
internal class BleLineFramer(
    private val maxLineBytes: Int = DEFAULT_MAX_LINE_BYTES
) {
    companion object {
        const val DEFAULT_MAX_LINE_BYTES = 4096
    }

    private val buffer = ByteArrayOutputStream(512)
    private var oversizeDrop = false

    fun reset() {
        buffer.reset()
        oversizeDrop = false
    }

    fun feed(chunk: ByteArray, onDropWarning: ((String) -> Unit)? = null): List<String> {
        if (chunk.isEmpty()) return emptyList()
        val completedLines = mutableListOf<String>()
        var cursor = 0
        while (cursor < chunk.size) {
            var nlPos = -1
            for (i in cursor until chunk.size) {
                if (chunk[i] == 0x0A.toByte()) {
                    nlPos = i
                    break
                }
            }
            if (oversizeDrop) {
                if (nlPos == -1) {
                    break
                }
                oversizeDrop = false
                buffer.reset()
                cursor = nlPos + 1
                continue
            }

            if (nlPos != -1) {
                val sliceLen = nlPos - cursor
                if (buffer.size() + sliceLen > maxLineBytes) {
                    onDropWarning?.invoke("RX line exceeded ${maxLineBytes}B, dropping oversized frame")
                    buffer.reset()
                } else {
                    if (sliceLen > 0) {
                        buffer.write(chunk, cursor, sliceLen)
                    }
                    val rawBytes = buffer.toByteArray()
                    buffer.reset()
                    var validEnd = rawBytes.size
                    while (validEnd > 0 && rawBytes[validEnd - 1] == 0x0D.toByte()) {
                        validEnd--
                    }
                    if (validEnd > 0) {
                        val decoded = String(rawBytes, 0, validEnd, Charsets.UTF_8).trim()
                        if (decoded.isNotEmpty()) {
                            completedLines.add(decoded)
                        }
                    }
                }
                cursor = nlPos + 1
            } else {
                val remaining = chunk.size - cursor
                if (buffer.size() + remaining > maxLineBytes) {
                    onDropWarning?.invoke("RX buffer exceeded ${maxLineBytes}B without newline, dropping until next LF")
                    buffer.reset()
                    oversizeDrop = true
                } else {
                    buffer.write(chunk, cursor, remaining)
                }
                break
            }
        }
        return completedLines
    }
}

/** 纯 Kotlin 下行指令校验、换行注入清洗与 ATT MTU 负载边界计算 */
internal object BleCommandCodec {
    const val MAX_CMD_BYTES = 219
    const val MIN_MTU_PAYLOAD_BYTES = 20

    sealed class PreparedCommand {
        data object Empty : PreparedCommand()
        data class Ready(
            val command: String,
            val urgent: Boolean,
            val wireBytesLength: Int
        ) : PreparedCommand()

        data class Rejected(val reason: String) : PreparedCommand()
    }

    /** 返回指定 ATT MTU 下单次写入允许的最大指令 UTF-8 字节数（始终受 (MTU - 3) 约束且介于 20..219B） */
    fun maxCommandPayloadBytes(mtu: Int): Int {
        return (mtu - 3)
            .coerceAtLeast(MIN_MTU_PAYLOAD_BYTES)
            .coerceAtMost(MAX_CMD_BYTES)
    }

    fun sanitizeCommand(raw: String): String {
        return raw.replace("\r", "").replace("\n", "").trim()
    }

    fun isEmergencyStopCommand(trimmedCmd: String): Boolean {
        return trimmedCmd.equals("stop", ignoreCase = true) ||
            trimmedCmd.equals("stop all", ignoreCase = true) ||
            trimmedCmd.equals("stop deauther", ignoreCase = true)
    }

    fun prepareCommand(rawCmd: String, mtu: Int): PreparedCommand {
        val normalized = sanitizeCommand(rawCmd)
        if (normalized.isEmpty()) return PreparedCommand.Empty
        val wireBytesLen = normalized.toByteArray(Charsets.UTF_8).size
        if (isEmergencyStopCommand(normalized)) {
            return PreparedCommand.Ready(
                command = normalized,
                urgent = true,
                wireBytesLength = wireBytesLen
            )
        }
        val maxBytes = maxCommandPayloadBytes(mtu)
        if (wireBytesLen > MAX_CMD_BYTES) {
            return PreparedCommand.Rejected("指令过长 (${wireBytesLen}B > ${MAX_CMD_BYTES}B)，已拦截")
        }
        if (wireBytesLen > maxBytes) {
            return PreparedCommand.Rejected(
                "当前蓝牙 MTU($mtu) 负载上限 ${maxBytes}B，无法完整发送 ${wireBytesLen}B 指令"
            )
        }
        return PreparedCommand.Ready(
            command = normalized,
            urgent = false,
            wireBytesLength = wireBytesLen
        )
    }

    fun prepareTargetedCommand(
        baseCmd: String,
        indices: Collection<Int>,
        mtu: Int
    ): PreparedCommand {
        val prefix = sanitizeCommand(baseCmd)
        if (prefix.isEmpty()) return PreparedCommand.Empty
        if (indices.isEmpty()) {
            return PreparedCommand.Rejected("请先在扫描页选择至少一个目标")
        }
        if (indices.any { it < 0 }) {
            return PreparedCommand.Rejected("存在非法目标索引，已拦截")
        }
        val sorted = indices.distinct().sorted()
        val fullCmd = "$prefix ${sorted.joinToString(",")}"
        val wireBytesLen = fullCmd.toByteArray(Charsets.UTF_8).size
        val maxBytes = maxCommandPayloadBytes(mtu)
        if (wireBytesLen > MAX_CMD_BYTES) {
            return PreparedCommand.Rejected("指令过长 (${wireBytesLen}B > ${MAX_CMD_BYTES}B)，已拦截")
        }
        if (wireBytesLen > maxBytes) {
            return PreparedCommand.Rejected(
                "当前蓝牙 MTU($mtu) 负载上限 ${maxBytes}B，无法完整发送 ${wireBytesLen}B 目标指令（请减少勾选数量或全选）"
            )
        }
        return PreparedCommand.Ready(
            command = fullCmd,
            urgent = false,
            wireBytesLength = wireBytesLen
        )
    }
}
