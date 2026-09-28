package com.example.wifidirect.transfer

/**
 * 传输发生的通道。
 */
enum class Transport { WIFI_DIRECT, BLUETOOTH }

/**
 * 描述文件传输过程中可能出现的所有事件。UI 通过 [FileTransferService.events]
 * 订阅并以 [TransferAdapter.applyEvent] 折叠成当前可见的列表。
 *
 * 每个 transfer 都有一个稳定的 [id]（UUID），同一 id 的事件按时间顺序到达，
 * 状态机如下：
 *
 *     Started ──► Progress* ──► (Completed | Failed | Canceled)
 *
 * 任何终态后该 id 不会再收到事件。
 */
sealed class TransferEvent {

    data class Started(
        val id: String,
        val fileName: String,
        val size: Long,
        val transport: Transport
    ) : TransferEvent()

    data class Progress(
        val id: String,
        val transferred: Long,
        val total: Long
    ) : TransferEvent()

    data class Completed(val id: String) : TransferEvent()

    data class Failed(val id: String, val message: String) : TransferEvent()

    data class Canceled(val id: String) : TransferEvent()

    /**
     * 一份文件已成功接收并落盘。携带 [transport] 以便 UI 区分来源（旧版本
     * 硬编码为 WIFI_DIRECT，蓝牙收到的文件也被错标为 Wi-Fi Direct）。
     */
    data class FileReceived(
        val absolutePath: String,
        val fileName: String,
        val size: Long,
        val transport: Transport,
        val mimeType: String?
    ) : TransferEvent()

    data class ServerStarted(val transport: Transport, val port: Int) : TransferEvent()

    /** 客户端成功把自己的 IP 上报给了组主（用于组主→客户端反向发送）。 */
    data class ClientRegisteredToOwner(val ip: String) : TransferEvent()

    /** 组主侧收到了某客户端上报的 IP（用于组主→客户端反向发送）。 */
    data class PeerRegistered(val ip: String) : TransferEvent()
}

/**
 * 小型纯函数集合，便于 UI 显示传输大小 / 百分比。设计成无状态可单测。
 */
object TransferFormatter {

    fun humanSize(bytes: Long): String = when {
        bytes < 0 -> "?"
        bytes < 1024 -> "$bytes B"
        bytes < 1024 * 1024 -> String.format(java.util.Locale.ROOT, "%.1f KB", bytes / 1024.0)
        bytes < 1024L * 1024 * 1024 -> String.format(java.util.Locale.ROOT, "%.1f MB", bytes / (1024.0 * 1024))
        else -> String.format(java.util.Locale.ROOT, "%.2f GB", bytes / (1024.0 * 1024 * 1024))
    }

    fun percent(transferred: Long, total: Long): Int {
        if (total <= 0) return 0
        return ((transferred * 100) / total).toInt().coerceIn(0, 100)
    }

    /** 简单的 MIME 猜测，用于「打开文件」时启动正确的应用。 */
    fun guessMimeType(name: String): String? {
        val lower = name.substringAfterLast('.', "").lowercase()
        return when (lower) {
            "jpg", "jpeg" -> "image/jpeg"
            "png" -> "image/png"
            "gif" -> "image/gif"
            "webp" -> "image/webp"
            "bmp" -> "image/bmp"
            "mp4" -> "video/mp4"
            "mkv" -> "video/x-matroska"
            "avi" -> "video/x-msvideo"
            "mov" -> "video/quicktime"
            "mp3" -> "audio/mpeg"
            "m4a" -> "audio/mp4"
            "wav" -> "audio/wav"
            "flac" -> "audio/flac"
            "ogg" -> "audio/ogg"
            "pdf" -> "application/pdf"
            "txt" -> "text/plain"
            "zip" -> "application/zip"
            "doc" -> "application/msword"
            "docx" -> "application/vnd.openxmlformats-officedocument.wordprocessingml.document"
            "xls" -> "application/vnd.ms-excel"
            "xlsx" -> "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"
            "ppt" -> "application/vnd.ms-powerpoint"
            "pptx" -> "application/vnd.openxmlformats-officedocument.presentationml.presentation"
            else -> null
        }
    }
}
