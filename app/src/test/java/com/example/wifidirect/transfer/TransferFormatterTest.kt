package com.example.wifidirect.transfer

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 纯函数测试 —— [TransferFormatter] 的格式化与百分比计算。
 */
class TransferFormatterTest {

    @Test
    fun `humanSize handles byte range`() {
        assertEquals("0 B", TransferFormatter.humanSize(0))
        assertEquals("512 B", TransferFormatter.humanSize(512))
        assertEquals("1023 B", TransferFormatter.humanSize(1023))
    }

    @Test
    fun `humanSize handles KB range`() {
        assertEquals("1.0 KB", TransferFormatter.humanSize(1024))
        assertEquals("1.5 KB", TransferFormatter.humanSize(1024 + 512))
    }

    @Test
    fun `humanSize handles MB range`() {
        assertEquals("1.0 MB", TransferFormatter.humanSize(1024 * 1024))
        // 1.5 MB
        assertEquals("1.5 MB", TransferFormatter.humanSize((1024 * 1024 * 1.5).toLong()))
    }

    @Test
    fun `humanSize handles GB range`() {
        val oneGb = 1024L * 1024 * 1024
        assertEquals("1.00 GB", TransferFormatter.humanSize(oneGb))
        assertEquals("2.50 GB", TransferFormatter.humanSize((oneGb * 2.5).toLong()))
    }

    @Test
    fun `humanSize returns question mark for negative`() {
        assertEquals("?", TransferFormatter.humanSize(-1))
        assertEquals("?", TransferFormatter.humanSize(-1024))
    }

    @Test
    fun `percent returns 0 for unknown total`() {
        assertEquals(0, TransferFormatter.percent(100, 0))
        assertEquals(0, TransferFormatter.percent(100, -1))
    }

    @Test
    fun `percent clamps to 0_100`() {
        assertEquals(0, TransferFormatter.percent(0, 100))
        assertEquals(50, TransferFormatter.percent(50, 100))
        assertEquals(100, TransferFormatter.percent(100, 100))
        assertEquals(100, TransferFormatter.percent(200, 100)) // overshoot
    }

    @Test
    fun `percent computes correctly for large files`() {
        // 1.5 GB transferred out of 3 GB
        val total = 3L * 1024 * 1024 * 1024
        val sent = (total * 0.5).toLong()
        assertEquals(50, TransferFormatter.percent(sent, total))
    }

    @Test
    fun `guessMimeType returns null for unknown extensions`() {
        assertNull(TransferFormatter.guessMimeType("file.xyz"))
        assertNull(TransferFormatter.guessMimeType("no_extension"))
    }

    @Test
    fun `guessMimeType covers common types`() {
        assertEquals("image/jpeg", TransferFormatter.guessMimeType("a.jpg"))
        assertEquals("image/jpeg", TransferFormatter.guessMimeType("a.JPEG"))
        assertEquals("image/png", TransferFormatter.guessMimeType("a.png"))
        assertEquals("video/mp4", TransferFormatter.guessMimeType("a.mp4"))
        assertEquals("audio/mpeg", TransferFormatter.guessMimeType("a.mp3"))
        assertEquals("application/pdf", TransferFormatter.guessMimeType("a.pdf"))
        assertEquals("application/zip", TransferFormatter.guessMimeType("a.zip"))
        assertEquals("text/plain", TransferFormatter.guessMimeType("a.txt"))
    }
}
