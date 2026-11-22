package com.example.wifidirect.manager

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/** 纯 JVM 单测：对齐 native/src/Discovery.cpp 的 ParseBeacon 行为。 */
class BeaconParserTest {

    private fun frame(version: Int = 1, host: String, ips: String): Pair<ByteArray, Int> {
        // "WFDn"(4) + version(1) + host + '\0' + ips
        val bytes = mutableListOf<Byte>()
        bytes.addAll("WFDn".map { it.code.toByte() })
        bytes.add(version.toByte())
        bytes.addAll(host.toByteArray(Charsets.UTF_8).toList())
        bytes.add(0)
        bytes.addAll(ips.toByteArray(Charsets.UTF_8).toList())
        val arr = bytes.toByteArray()
        return arr to arr.size
    }

    @Test
    fun parsesNormalFrame() {
        val (buf, len) = frame(host = "desktop-pc", ips = "192.168.1.42")
        val peer = BeaconParser.parse(buf, len, nowMs = 1000)
        assertEquals("192.168.1.42", peer!!.ip)
        assertEquals("desktop-pc", peer.name)
        assertEquals(1000L, peer.lastSeenMs)
    }

    @Test
    fun takesFirstIpFromCsv() {
        val (buf, len) = frame(host = "pc", ips = "10.0.0.1,10.0.0.2")
        assertEquals("10.0.0.1", BeaconParser.parse(buf, len)!!.ip)
    }

    @Test
    fun parsesUtf8Hostname() {
        val (buf, len) = frame(host = "客厅电脑", ips = "192.168.1.7")
        assertEquals("客厅电脑", BeaconParser.parse(buf, len)!!.name)
    }

    @Test
    fun rejectsShortFrame() {
        assertNull(BeaconParser.parse(ByteArray(5), 5))
    }

    @Test
    fun rejectsWrongMagic() {
        val (buf, len) = frame(host = "pc", ips = "1.2.3.4")
        buf[0] = 'X'.code.toByte()
        assertNull(BeaconParser.parse(buf, len))
    }

    @Test
    fun rejectsWrongVersion() {
        val (buf, len) = frame(version = 2, host = "pc", ips = "1.2.3.4")
        assertNull(BeaconParser.parse(buf, len))
    }

    @Test
    fun rejectsMissingNul() {
        // 构造无 NUL 的帧
        val bytes = mutableListOf<Byte>()
        bytes.addAll("WFDn".map { it.code.toByte() })
        bytes.add(1)
        bytes.addAll("pc".map { it.code.toByte() })
        val arr = bytes.toByteArray()
        assertNull(BeaconParser.parse(arr, arr.size))
    }

    @Test
    fun emptyIpListYieldsEmptyIp() {
        val (buf, len) = frame(host = "pc", ips = "")
        val peer = BeaconParser.parse(buf, len)
        assertEquals("", peer!!.ip)
        assertEquals("pc", peer.name)
    }

    @Test
    fun toleratesTrailingBytesAfterIp() {
        val (buf, len) = frame(host = "pc", ips = "1.2.3.4")
        // 帧后有多余字节但 len 只读到真实帧长 —— 应按 len 解析
        val padded = ByteArray(len + 4) { if (it < len) buf[it] else 0x7f }
        assertEquals("1.2.3.4", BeaconParser.parse(padded, len)!!.ip)
    }
}
