package com.example.wifidirect.manager

import com.example.wifidirect.transfer.Constants

/**
 * 解析桌面 native 客户端广播的 LAN beacon 帧。
 *
 * wire 格式（与 native/src/Discovery.cpp 的 ParseBeacon 严格一致）：
 *   "WFDn"(4B) + version(1B) + hostname(UTF-8, NUL 结尾) + ipList(CSV, 不 NUL 结尾)
 *
 * 纯 JVM 逻辑，不依赖 Android，便于单测。
 */
data class LanPeer(
    val ip: String,
    val name: String,
    val lastSeenMs: Long = 0L,
)

object BeaconParser {

    /** 解析一帧 beacon，非法帧返回 null。 */
    fun parse(buf: ByteArray, len: Int, nowMs: Long = 0L): LanPeer? {
        if (len < 6) return null
        // magic "WFDn"
        if (buf[0] != 'W'.code.toByte() || buf[1] != 'F'.code.toByte() ||
            buf[2] != 'D'.code.toByte() || buf[3] != 'n'.code.toByte()) return null
        // version
        if (buf[4].toInt() and 0xFF != Constants.BEACON_VERSION) return null

        // hostname: 从 index 5 起找首个 NUL
        val payloadStart = 5
        var nul = -1
        for (i in payloadStart until len) {
            if (buf[i] == 0.toByte()) { nul = i; break }
        }
        if (nul < 0) return null
        val host = String(buf, payloadStart, nul - payloadStart, Charsets.UTF_8)

        // ipList: NUL 之后到帧尾，取首个逗号或结尾之前的 IP
        val ipStart = nul + 1
        if (ipStart >= len) {
            return LanPeer(ip = "", name = host, lastSeenMs = nowMs)
        }
        var ipEnd = len
        for (i in ipStart until len) {
            if (buf[i] == ','.code.toByte()) { ipEnd = i; break }
        }
        val ip = String(buf, ipStart, ipEnd - ipStart, Charsets.UTF_8)
        return LanPeer(ip = ip, name = host, lastSeenMs = nowMs)
    }
}
