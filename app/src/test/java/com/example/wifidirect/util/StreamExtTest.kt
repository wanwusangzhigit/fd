package com.example.wifidirect.util

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertThrows
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.io.InterruptedIOException

class StreamExtTest {

    @Test
    fun `streamTo copies all bytes and reports progress`() {
        val data = ByteArray(100_000) { (it and 0xFF).toByte() }
        val src = ByteArrayInputStream(data)
        val dst = ByteArrayOutputStream()
        val reports = mutableListOf<Long>()

        src.streamTo(
            destination = dst,
            expectedBytes = data.size.toLong(),
            progressIntervalBytes = 10_000L,
            onProgress = { reports += it }
        )

        assertEquals(data.size, dst.size())
        assertEquals(data.toList(), dst.toByteArray().toList())
        // 至少报告过一次（最后一次必然是 transferred == expectedBytes）
        assertEquals(data.size.toLong(), reports.last())
    }

    @Test
    fun `streamTo throws when source ends early`() {
        val src = ByteArray(100).inputStream()
        val dst = ByteArrayOutputStream()
        val ex = assertThrows(IOException::class.java) {
            src.streamTo(dst, expectedBytes = 1_000L)
        }
        assertEquals(true, ex.message?.contains("Unexpected EOF"))
    }

    @Test
    fun `streamTo aborts via isCancelled`() {
        val data = ByteArray(10_000)
        val src = ByteArrayInputStream(data)
        val dst = ByteArrayOutputStream()
        var ticks = 0
        val ex = assertThrows(InterruptedIOException::class.java) {
            src.streamTo(
                destination = dst,
                expectedBytes = data.size.toLong(),
                bufferSize = 1, // 让每次 read 仅读 1 字节，进度回调才能多次触发
                onProgress = { ticks++ },
                progressIntervalBytes = 1L,
                isCancelled = { ticks >= 3 }
            )
        }
        assertEquals("Transfer cancelled", ex.message)
        // 拷贝一定没完整
        assertNotEquals(data.size, dst.size())
    }
}
