package com.example.wifidirect.ui

import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.ProgressBar
import android.widget.TextView
import androidx.recyclerview.widget.DiffUtil
import androidx.recyclerview.widget.ListAdapter
import androidx.recyclerview.widget.RecyclerView
import com.example.wifidirect.R
import com.example.wifidirect.transfer.TransferEvent
import com.example.wifidirect.transfer.TransferFormatter
import com.example.wifidirect.transfer.Transport

/**
 * 显示活跃传输的列表。修复要点：
 *
 * - 新增「取消」按钮 —— 调用 [onCancelTransferClicked]，调用方据此取消传输。
 * - 已完成的「已接收」行点击打开 —— [onOpenReceivedClicked]。
 * - DiffUtil 的 `areContentsTheSame` 现在真正比较内容（旧版只比较 id）。
 * - 处理了 [TransferEvent.Canceled]（旧版编译期就漏了）。
 */
class TransferAdapter(
    private val onOpenReceivedClicked: (path: String, mimeType: String?) -> Unit,
    private val onCancelTransferClicked: (transferId: String) -> Unit
) : ListAdapter<TransferAdapter.Row, TransferAdapter.VH>(DIFF) {

    data class Row(
        val id: String,
        val displayName: String,
        val transport: Transport,
        val transferred: Long,
        val total: Long,
        val done: Boolean,
        val failed: Boolean,
        val canceled: Boolean,
        val errorMessage: String?,
        /** 仅对「已接收」行有效 —— 表示落盘路径或 content-uri 字符串。 */
        val savedPath: String? = null,
        /** 仅对「已接收」行有效 —— mimeType 用于 ACTION_VIEW。 */
        val mimeType: String? = null,
        /** 行是否点击会打开已接收文件。 */
        val openable: Boolean = false,
        /** UI 计算的瞬时速率（字节/秒）；任务未开始或已结束时为 0。 */
        val speedBytesPerSec: Long = 0L,
        /** UI 计算的剩余时间（秒）；任务未开始或已完成时为 null。 */
        val etaSeconds: Long? = null
    )

    class VH(view: View) : RecyclerView.ViewHolder(view) {
        val name: TextView = view.findViewById(R.id.transferName)
        val meta: TextView = view.findViewById(R.id.transferMeta)
        val progress: ProgressBar = view.findViewById(R.id.transferProgress)
        val percent: TextView = view.findViewById(R.id.transferPercent)
        val cancelBtn: Button? = view.findViewById(R.id.transferCancel)
        val icon: android.widget.ImageView? = view.findViewById(R.id.transferIcon)
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): VH {
        val v = LayoutInflater.from(parent.context).inflate(R.layout.item_transfer, parent, false)
        return VH(v)
    }

    override fun onBindViewHolder(holder: VH, position: Int) {
        val row = getItem(position)
        val ctx = holder.itemView.context
        val transportLabel = when (row.transport) {
            Transport.WIFI_DIRECT -> ctx.getString(R.string.transport_wfd)
            Transport.BLUETOOTH -> ctx.getString(R.string.transport_bt)
        }
        holder.name.text = "${row.displayName}  •  $transportLabel"
        holder.progress.isIndeterminate = row.total <= 0 && !row.done && !row.failed && !row.canceled
        holder.progress.max = 100
        holder.progress.progress = TransferFormatter.percent(row.transferred, row.total)

        // 文件类型图标
        holder.icon?.setImageResource(iconResFor(row.mimeType, row.displayName))

        val percentLabel = when {
            row.failed -> ctx.getString(R.string.status_failed)
            row.canceled -> ctx.getString(R.string.status_canceled)
            row.done -> ctx.getString(R.string.status_done)
            row.total > 0 -> "${TransferFormatter.percent(row.transferred, row.total)}%"
            else -> TransferFormatter.humanSize(row.transferred)
        }
        holder.percent.text = percentLabel

        val sizeInfo = if (row.total > 0)
            "${TransferFormatter.humanSize(row.transferred)} / ${TransferFormatter.humanSize(row.total)}"
        else TransferFormatter.humanSize(row.transferred)
        val status = when {
            row.canceled -> ctx.getString(R.string.status_canceled)
            row.failed -> row.errorMessage ?: ctx.getString(R.string.status_failed)
            row.done -> ctx.getString(R.string.status_completed)
            else -> {
                // 进行中：附加速率 + ETA
                val speed = if (row.speedBytesPerSec > 0)
                    TransferFormatter.humanSize(row.speedBytesPerSec) + "/s" else "—"
                val eta = row.etaSeconds?.let { formatEta(ctx, it) } ?: ""
                "${ctx.getString(R.string.status_transferring)} · $speed $eta".trim()
            }
        }
        holder.meta.text = "$sizeInfo — $status"

        // 取消按钮：仅在进行中的传输可见
        val cancelBtn = holder.cancelBtn
        if (cancelBtn != null) {
            cancelBtn.visibility = if (row.done || row.failed || row.canceled) View.GONE else View.VISIBLE
            cancelBtn.setOnClickListener { onCancelTransferClicked(row.id) }
        }
        // 点击打开：仅已接收且可解析路径时
        if (row.openable && !row.savedPath.isNullOrEmpty()) {
            holder.itemView.setOnClickListener {
                onOpenReceivedClicked(row.savedPath, row.mimeType)
            }
        } else {
            holder.itemView.setOnClickListener(null)
        }
    }

    companion object {
        val DIFF = object : DiffUtil.ItemCallback<Row>() {
            override fun areItemsTheSame(o: Row, n: Row) = o.id == n.id
            override fun areContentsTheSame(o: Row, n: Row) = o == n
        }

        /** 把秒数格式化成 `1d 2h 3m` / `5m 30s` / `45s` 这样的紧凑字符串。 */
        private fun formatEta(ctx: android.content.Context, seconds: Long): String {
            if (seconds < 1) return ""
            val s = seconds % 60
            val m = (seconds / 60) % 60
            val h = (seconds / 3600) % 24
            val d = seconds / 86400
            return when {
                d > 0 -> "· ${d}d ${h}h"
                h > 0 -> "· ${h}h ${m}m"
                m > 0 -> "· ${m}m ${s}s"
                else -> "· ${s}s"
            }
        }

        /** 根据 mimeType 与扩展名选择图标资源。当前统一用通用图标，
         *  后续可按 mime 分支出 image/audio/video/document 等专属图标。 */
        @androidx.annotation.DrawableRes
        private fun iconResFor(mimeType: String?, fileName: String): Int {
            val ext = fileName.substringAfterLast('.', "").lowercase()
            val mt = (mimeType ?: TransferFormatter.guessMimeType(fileName) ?: "").lowercase()
            return when {
                mt.startsWith("image/") || ext in listOf("jpg", "jpeg", "png", "gif", "webp", "bmp")
                    -> com.example.wifidirect.R.drawable.ic_file_image
                mt.startsWith("video/") || ext in listOf("mp4", "mkv", "avi", "mov", "webm")
                    -> com.example.wifidirect.R.drawable.ic_file_video
                mt.startsWith("audio/") || ext in listOf("mp3", "m4a", "wav", "flac", "ogg")
                    -> com.example.wifidirect.R.drawable.ic_file_audio
                mt == "application/pdf" || ext == "pdf"
                    -> com.example.wifidirect.R.drawable.ic_file_pdf
                mt == "application/zip" || ext == "zip"
                    -> com.example.wifidirect.R.drawable.ic_file_archive
                else -> com.example.wifidirect.R.drawable.ic_file_generic
            }
        }
    }

    /** 由 UI 计算速率后调用，把速率 / ETA 直接更新到对应行。 */
    fun setSpeed(id: String, bytesPerSec: Long, etaSeconds: Long?) {
        val current = currentList.toMutableList()
        val idx = current.indexOfFirst { it.id == id }
        if (idx < 0) return
        current[idx] = current[idx].copy(speedBytesPerSec = bytesPerSec, etaSeconds = etaSeconds)
        submitList(current)
    }

    /** 滑动删除：从列表移除指定位置的行。 */
    fun removeAt(position: Int) {
        if (position < 0 || position >= currentList.size) return
        val current = currentList.toMutableList()
        val row = current[position]
        current.removeAt(position)
        submitList(current)
        // 清掉速率采样器里对应的状态，避免泄漏
        com.example.wifidirect.transfer.SpeedTracker.forget(row.id)
    }

    /** 把事件折叠进当前列表。 */
    fun applyEvent(event: TransferEvent) {
        val current = currentList.toMutableList()
        when (event) {
            is TransferEvent.Started -> {
                current.removeAll { it.id == event.id }
                current.add(0, Row(
                    id = event.id,
                    displayName = event.fileName,
                    transport = event.transport,
                    transferred = 0,
                    total = event.size,
                    done = false,
                    failed = false,
                    canceled = false,
                    errorMessage = null
                ))
            }
            is TransferEvent.Progress -> {
                val idx = current.indexOfFirst { it.id == event.id }
                if (idx >= 0) {
                    val r = current[idx]
                    current[idx] = r.copy(transferred = event.transferred, total = event.total)
                }
            }
            is TransferEvent.Completed -> {
                val idx = current.indexOfFirst { it.id == event.id }
                if (idx >= 0) {
                    val r = current[idx]
                    current[idx] = r.copy(done = true,
                        transferred = if (r.total > 0) r.total else r.transferred)
                }
            }
            is TransferEvent.Failed -> {
                val idx = current.indexOfFirst { it.id == event.id }
                if (idx >= 0) {
                    current[idx] = current[idx].copy(failed = true, errorMessage = event.message)
                }
            }
            is TransferEvent.Canceled -> {
                val idx = current.indexOfFirst { it.id == event.id }
                if (idx >= 0) {
                    current[idx] = current[idx].copy(canceled = true)
                }
            }
            is TransferEvent.FileReceived -> {
                val syntheticId = "recv:${event.fileName}:${event.size}"
                current.removeAll { it.id == syntheticId }
                current.add(0, Row(
                    id = syntheticId,
                    displayName = "↓ ${event.fileName}",
                    transport = event.transport,
                    transferred = event.size,
                    total = event.size,
                    done = true,
                    failed = false,
                    canceled = false,
                    errorMessage = null,
                    savedPath = event.absolutePath,
                    mimeType = event.mimeType,
                    openable = true
                ))
            }
            is TransferEvent.ServerStarted,
            is TransferEvent.ClientRegisteredToOwner,
            is TransferEvent.PeerRegistered -> { /* no-op for the list */ }
        }
        submitList(current)
    }
}
