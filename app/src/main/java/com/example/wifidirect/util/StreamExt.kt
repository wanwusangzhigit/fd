package com.example.wifidirect.util

import com.example.wifidirect.transfer.Constants
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.io.InterruptedIOException

/**
 * 把 [this] 流按 [expectedBytes] 字节流式拷贝到 [destination]，每传输
 * [progressIntervalBytes] 字节回调一次 [onProgress]（参数是累计字节数）。
 *
 * 抽出来的目的是让 TCP 服务端、蓝牙发送端、接收端共用同一份实现，
 * 而不是每个地方各自维护一份近似的循环（旧代码就是这样）。
 *
 * 行为约定：
 * - 不会关闭任何一个流（调用方负责）。
 * - 遇到 EOF 但没读够 [expectedBytes] 字节时抛 [IOException]，避免静默写坏文件。
 * - 通过 [isCancelled] 提供协作式取消：返回 true 时立即抛 [InterruptedIOException]。
 * - 内部按块读取；缓冲区大小由 [bufferSize] 决定。
 */
@Throws(IOException::class)
fun InputStream.streamTo(
    destination: OutputStream,
    expectedBytes: Long,
    onProgress: (transferred: Long) -> Unit = {},
    progressIntervalBytes: Long = Constants.PROGRESS_INTERVAL_BYTES_TCP.toLong(),
    bufferSize: Int = Constants.BUFFER_SIZE_TCP,
    isCancelled: () -> Boolean = { false }
): Long {
    require(expectedBytes >= 0) { "expectedBytes must be >= 0" }
    require(bufferSize > 0) { "bufferSize must be > 0" }
    val buffer = ByteArray(bufferSize)
    var transferred = 0L
    var lastReport = 0L
    while (transferred < expectedBytes) {
        if (isCancelled()) throw InterruptedIOException("Transfer cancelled")
        val toRead = minOf(bufferSize.toLong(), expectedBytes - transferred).toInt()
        val read = read(buffer, 0, toRead)
        if (read <= 0) {
            throw IOException("Unexpected EOF after $transferred/$expectedBytes bytes")
        }
        destination.write(buffer, 0, read)
        destination.flush()
        transferred += read
        if (transferred - lastReport >= progressIntervalBytes || transferred == expectedBytes) {
            onProgress(transferred)
            lastReport = transferred
        }
    }
    return transferred
}
