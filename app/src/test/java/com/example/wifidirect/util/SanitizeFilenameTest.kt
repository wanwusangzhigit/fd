package com.example.wifidirect.util

import org.junit.Assert.assertEquals
import org.junit.Test

class SanitizeFilenameTest {

    @Test
    fun `replaces forbidden characters`() {
        assertEquals("a_b_c", sanitizeFilename("a/b\\c"))
        assertEquals("a_b", sanitizeFilename("a:b"))
        assertEquals("__", sanitizeFilename("<>"))  // 每个字符单独替换
    }

    @Test
    fun `trims leading trailing dots and whitespace`() {
        assertEquals("a", sanitizeFilename(" a. "))
        assertEquals("a", sanitizeFilename(".a."))
    }

    @Test
    fun `limits length`() {
        val long = "a".repeat(500)
        val result = sanitizeFilename(long)
        assertEquals(240, result.length)
    }

    @Test
    fun `empty input yields empty string`() {
        assertEquals("", sanitizeFilename(""))
    }

    @Test
    fun `unicode preserved`() {
        assertEquals("测试.txt", sanitizeFilename("测试.txt"))
    }
}
