package com.example.wifidirect.util

/**
 * 把任意字符串变成安全的文件名：
 * - 把平台禁止的字符（路径分隔符、控制字符、`<>:"|?*`）替换成下划线
 * - 移除首尾的点和空格（Windows / Android 都不允许）
 * - 限制长度（255 是 ext4/fat32 上限，留一些余量）
 */
fun sanitizeFilename(raw: String): String {
    val forbidden = setOf('/', '\\', ':', '*', '?', '"', '<', '>', '|', '\u0000')
    val cleaned = raw.map { c -> if (c in forbidden || c.code < 0x20) '_' else c }.joinToString("")
    // Windows / Android 不允许文件名以点或空格开头 / 结尾，两端都要清。
    return cleaned.trim().trimStart('.', ' ').trimEnd('.', ' ').let {
        if (it.length > 240) it.substring(0, 240) else it
    }
}
