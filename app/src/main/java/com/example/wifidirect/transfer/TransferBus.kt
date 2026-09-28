package com.example.wifidirect.transfer

import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import java.util.concurrent.ConcurrentHashMap

/**
 * 进程级别的传输事件总线。
 *
 * 拆成两条流，避免旧实现里所有事件挤在同一个 `MutableSharedFlow(buffer=64, DROP_OLDEST)`
 * 中——那种实现下，几 MB 文件的 Progress 事件就能把缓冲塞满，进而把关键的
 * `Completed` / `Failed` 事件给丢掉，UI 卡在中间状态。
 *
 * - [critical]: Started/Completed/Failed/Canceled/FileReceived/ServerStarted 等
 *   状态变更事件，buffer=128、SUSPEND 永远不会丢。
 * - [progress]: Progress 流以 StateFlow 实现，每 transfer id 仅保留最新一条。
 *   UI 始终拿到最新进度即可，丢中间帧没问题。
 */
object TransferBus {

    private val _critical = MutableSharedFlow<TransferEvent>(
        replay = 0,
        extraBufferCapacity = 128,
        onBufferOverflow = BufferOverflow.SUSPEND
    )
    val critical: SharedFlow<TransferEvent> = _critical.asSharedFlow()

    private val _progress = MutableStateFlow<Map<String, TransferEvent.Progress>>(emptyMap())
    val progress: StateFlow<Map<String, TransferEvent.Progress>> = _progress.asStateFlow()

    /** 维护当前传输的取消句柄，按 transfer id 索引；UI 用 [cancelTransfer] 取消。 */
    private val cancelHandles = ConcurrentHashMap<String, () -> Unit>()

    fun emit(event: TransferEvent) {
        when (event) {
            is TransferEvent.Progress -> _progress.update { it + (event.id to event) }
            is TransferEvent.Completed -> finalize(event.id) { emitCritical(event) }
            is TransferEvent.Failed -> finalize(event.id) { emitCritical(event) }
            is TransferEvent.Canceled -> finalize(event.id) { emitCritical(event) }
            else -> emitCritical(event)
        }
    }

    private fun finalize(id: String, emitTerminal: () -> Unit) {
        _progress.update { it - id }
        cancelHandles.remove(id)
        emitTerminal()
    }

    private fun emitCritical(event: TransferEvent) {
        _critical.tryEmit(event)
    }

    /** 注册一个取消句柄；传输任务在循环里检查它返回的 lambda。 */
    fun registerCancelHandle(id: String, cancel: () -> Unit) {
        cancelHandles[id] = cancel
    }

    fun cancelTransfer(id: String): Boolean {
        val handle = cancelHandles.remove(id) ?: return false
        handle()
        return true
    }

    /** 供测试 / 重置使用。 */
    fun reset() {
        cancelHandles.clear()
        _progress.value = emptyMap()
    }
}
