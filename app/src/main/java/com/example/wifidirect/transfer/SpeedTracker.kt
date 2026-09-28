package com.example.wifidirect.transfer

import java.util.concurrent.ConcurrentHashMap

/**
 * 基于 transfer id 的瞬时速率采样器。
 *
 * 给定一组 (id, transferred, total) 的事件序列，对每个 id 维护最近两个采样点
 * 计算 bytes/sec，并把剩余时间换算成 ETA 秒数。在 UI 端使用，避免在传输层引入
 * 与显示相关的逻辑。
 *
 * 对每个 id 只保留最多两个采样点（旧的丢弃），内存占用与 id 数线性相关。
 */
object SpeedTracker {

    data class Sample(val timestamp: Long, val transferred: Long)
    data class Decorated(val bytesPerSec: Long, val etaSeconds: Long?)

    private val lastSamples = ConcurrentHashMap<String, Sample>()
    private val prevSamples = ConcurrentHashMap<String, Sample>()

    /**
     * 记录一次新采样，返回基于「上一次到本次」的瞬时速率与 ETA。
     * 首次采样时返回 0 / null。
     */
    fun tick(id: String, transferred: Long, total: Long): Decorated {
        val now = System.currentTimeMillis()
        val prev = prevSamples[id]
        val last = lastSamples[id]
        // 用「prev→last」窗口计算速率；若 last 距今已超过 1s 则用它替代 prev
        val effectivePrev = when {
            prev == null && last == null -> null
            last == null -> prev
            prev == null -> last
            (now - (last?.timestamp ?: now)) > 1500 -> last
            else -> prev
        }
        val baseline = effectivePrev ?: Sample(now, transferred)
        val dtMs = (now - baseline.timestamp).coerceAtLeast(1)
        val db = (transferred - baseline.transferred).coerceAtLeast(0)
        val bytesPerSec = (db * 1000) / dtMs
        // 维护采样窗
        prevSamples[id] = baseline
        lastSamples[id] = Sample(now, transferred)
        // ETA
        val etaSeconds = if (bytesPerSec <= 0 || total <= 0) null
                         else ((total - transferred) / bytesPerSec)
        return Decorated(bytesPerSec, etaSeconds)
    }

    /** 任务结束（终态）时清理。 */
    fun forget(id: String) {
        lastSamples.remove(id)
        prevSamples.remove(id)
    }

    /** 测试用：清空全部状态。 */
    fun reset() {
        lastSamples.clear()
        prevSamples.clear()
    }
}
