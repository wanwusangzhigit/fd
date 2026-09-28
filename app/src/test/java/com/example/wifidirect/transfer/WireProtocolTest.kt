package com.example.wifidirect.transfer

import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.IOException

/**
 * [WireProtocol] round-trip 测试：写到流再读回来应当得到等价的对象。
 *
 * 这一组测试覆盖：
 * - 正常路径（普通文件名、中文文件名、大文件名）
 * - 错误路径（magic 不符、版本号不一致、长度越界等应当抛出）
 */
class WireProtocolTest {

    @Test
    fun `writeFileHeader then readFilePayload round trips ASCII`() {
        val out = ByteArrayOutputStream()
        WireProtocol.writeFileHeader(out, "photo.jpg", 1024 * 1024)
        val input = ByteArrayInputStream(out.toByteArray())

        val prefix = WireProtocol.readFramePrefix(input)
        assertEquals(WireProtocol.VERSION, prefix.version)
        assertEquals(WireProtocol.KIND_FILE, prefix.kind)

        val header = WireProtocol.readFilePayload(input)
        assertEquals("photo.jpg", header.name)
        assertEquals(1024L * 1024, header.size)
    }

    @Test
    fun `writeFileHeader then readFilePayload round trips Chinese filename`() {
        val out = ByteArrayOutputStream()
        WireProtocol.writeFileHeader(out, "测试 文件 (1).mp4", 9527L)
        val input = ByteArrayInputStream(out.toByteArray())

        WireProtocol.readFramePrefix(input)
        val header = WireProtocol.readFilePayload(input)
        assertEquals("测试 文件 (1).mp4", header.name)
        assertEquals(9527L, header.size)
    }

    @Test
    fun `writeRegisterIp then readRegisterIpPayload round trips`() {
        val out = ByteArrayOutputStream()
        WireProtocol.writeRegisterIp(out, "192.168.49.23")
        val input = ByteArrayInputStream(out.toByteArray())

        val prefix = WireProtocol.readFramePrefix(input)
        assertEquals(WireProtocol.KIND_REGISTER_IP, prefix.kind)
        val payload = WireProtocol.readRegisterIpPayload(input)
        assertEquals("192.168.49.23", payload.ip)
    }

    @Test
    fun `readFramePrefix rejects bad magic`() {
        val garbage = "XXXX".toByteArray() + byteArrayOf(2, 1, 0, 0, 0, 0, 0, 0, 0)
        val ex = assertThrows(IllegalArgumentException::class.java) {
            WireProtocol.readFramePrefix(ByteArrayInputStream(garbage))
        }
        assertTrue(ex.message?.contains("Invalid magic") == true)
    }

    @Test
    fun `readFramePrefix rejects unsupported version`() {
        val bytes = mutableListOf<Byte>()
        bytes.addAll("FDFT".toByteArray().toList())
        bytes.add(99)  // unsupported version
        bytes.add(WireProtocol.KIND_FILE)
        val ex = assertThrows(IllegalArgumentException::class.java) {
            WireProtocol.readFramePrefix(ByteArrayInputStream(bytes.toByteArray()))
        }
        assertTrue(ex.message?.contains("Unsupported protocol version") == true)
    }

    @Test
    fun `readFilePayload rejects absurd name length`() {
        val bytes = mutableListOf<Byte>()
        bytes.addAll("FDFT".toByteArray().toList())
        bytes.add(WireProtocol.VERSION)
        bytes.add(WireProtocol.KIND_FILE)
        // 写一个超出 MAX_NAME_LEN 的 nameLen（10 MB）
        bytes.addAll(listOf(0, 0xFF, 0xFF, 0xFF).map { it.toByte() })
        val ex = assertThrows(IllegalArgumentException::class.java) {
            WireProtocol.readFramePrefix(ByteArrayInputStream(bytes.toByteArray()))
            WireProtocol.readFilePayload(ByteArrayInputStream(bytes.toByteArray()))
        }
        // 异常信号：任何一种合法的拒绝都行
        assertTrue(ex.message?.isNotBlank() == true)
    }

    @Test
    fun `writeFileHeader rejects empty name`() {
        val out = ByteArrayOutputStream()
        assertThrows(IllegalArgumentException::class.java) {
            WireProtocol.writeFileHeader(out, "", 10)
        }
    }

    @Test
    fun `writeFileHeader rejects negative size`() {
        val out = ByteArrayOutputStream()
        assertThrows(IllegalArgumentException::class.java) {
            WireProtocol.writeFileHeader(out, "ok", -1)
        }
    }

    @Test
    fun `writeRegisterIp rejects too long ip`() {
        val out = ByteArrayOutputStream()
        val hugeIp = "x".repeat(300)
        assertThrows(IllegalArgumentException::class.java) {
            WireProtocol.writeRegisterIp(out, hugeIp)
        }
    }
}
