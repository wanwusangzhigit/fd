package com.example.wifidirect.transfer

import android.bluetooth.BluetoothSocket
import android.content.ContentResolver
import android.net.Uri
import android.util.Log
import com.example.wifidirect.util.streamTo
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import java.io.IOException
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap

/**
 * 抽象「一个蓝牙 RFCOMM 连接（作为发送端）」的全部状态：socket + 接收方主机名 +
 * 一个独立的协程 scope（与 BluetoothManager 的接收 scope 分开，避免互相影响）。
 *
 * 设计原则：
 * - 调用方只关心 [sendFile] 与 [close]；
 * - 内部所有错误都通过 [TransferBus] 汇报，UI 总能看到状态；
 * - socket 由本类独占持有，杜绝之前 Activity / BluetoothManager / Service 多处关闭造成的双重释放。
 */
class BluetoothSenderConnection(
    private val socket: BluetoothSocket,
    private val scope: CoroutineScope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
) {
    private val sendJobs = ConcurrentHashMap<String, Job>()

    fun sendFile(
        uri: Uri,
        name: String,
        size: Long,
        contentResolver: ContentResolver,
        id: String = UUID.randomUUID().toString()
    ): Job {
        TransferBus.emit(TransferEvent.Started(id, name, size, Transport.BLUETOOTH))
        val job = scope.launch {
            var input: java.io.InputStream? = null
            try {
                input = contentResolver.openInputStream(uri)
                    ?: throw IOException("Cannot open $uri")
                val out = socket.outputStream
                WireProtocol.writeFileHeader(out, name, size)
                input.streamTo(
                    destination = out,
                    expectedBytes = if (size > 0) size else Long.MAX_VALUE,
                    bufferSize = Constants.BUFFER_SIZE_BT,
                    progressIntervalBytes = Constants.PROGRESS_INTERVAL_BYTES_BT.toLong(),
                    onProgress = { transferred -> TransferBus.emit(TransferEvent.Progress(id, transferred, size)) },
                    isCancelled = { sendJobs[id]?.isActive != true }
                )
                out.flush()
                TransferBus.emit(TransferEvent.Completed(id))
            } catch (e: Exception) {
                Log.e(TAG, "BT send failed", e)
                val evt = if (e is kotlinx.coroutines.CancellationException)
                    TransferEvent.Canceled(id)
                else TransferEvent.Failed(id, e.message ?: "Bluetooth send failed")
                TransferBus.emit(evt)
            } finally {
                sendJobs.remove(id)
                try { input?.close() } catch (_: IOException) {}
            }
        }
        sendJobs[id] = job
        TransferBus.registerCancelHandle(id) { job.cancel() }
        return job
    }

    fun cancel(id: String) {
        sendJobs.remove(id)?.cancel()
        TransferBus.cancelTransfer(id)
    }

    fun close() {
        sendJobs.values.forEach { it.cancel() }
        sendJobs.clear()
        try { socket.close() } catch (e: IOException) { Log.w(TAG, "close socket", e) }
        scope.cancel()
    }

    companion object {
        private const val TAG = "BluetoothSenderConn"
    }
}

/**
 * 蓝牙接收端：在一个独立的协程 scope 里循环 `readFramePrefix()` 接收文件。
 * 与旧实现不同的是 socket 关闭权交给调用方（BluetoothManager）。
 */
class BluetoothReceiverConnection(
    private val socket: BluetoothSocket,
    private val receivedFileManager: ReceivedFileManager,
    private val scope: CoroutineScope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
) : AutoCloseable {

    private val _closed = MutableStateFlow(false)
    val closed: StateFlow<Boolean> = _closed.asStateFlow()

    fun start(): Job = scope.launch {
        try {
            val din = java.io.DataInputStream(socket.inputStream)
            while (!_closed.value) {
                val prefix = try { WireProtocol.readFramePrefix(din) }
                              catch (e: IOException) { break } // 对端关闭
                if (prefix.kind != WireProtocol.KIND_FILE) {
                    Log.w(TAG, "Skip non-FILE frame on BT connection: kind=${prefix.kind}")
                    continue
                }
                val header = WireProtocol.readFilePayload(din)
                val rid = UUID.randomUUID().toString()
                TransferBus.emit(TransferEvent.Started(rid, header.name, header.size, Transport.BLUETOOTH))
                val sink = receivedFileManager.begin(header.name)
                try {
                    sink.output.use { out ->
                        din.streamTo(
                            destination = out,
                            expectedBytes = header.size,
                            bufferSize = Constants.BUFFER_SIZE_RECEIVE,
                            progressIntervalBytes = Constants.PROGRESS_INTERVAL_BYTES_BT.toLong(),
                            onProgress = { transferred ->
                                TransferBus.emit(TransferEvent.Progress(rid, transferred, header.size))
                            },
                            isCancelled = { _closed.value }
                        )
                    }
                    val finalPath = sink.commit()
                    TransferBus.emit(TransferEvent.Completed(rid))
                    val fileReceived = TransferEvent.FileReceived(
                        absolutePath = finalPath,
                        fileName = header.name,
                        size = header.size,
                        transport = Transport.BLUETOOTH,
                        mimeType = TransferFormatter.guessMimeType(header.name)
                    )
                    TransferBus.emit(fileReceived)
                    try {
                        TransferHistory.get().add(fileReceived.toHistoryRecord())
                    } catch (e: IllegalStateException) {
                        Log.w(TAG, "TransferHistory 未初始化，跳过写入", e)
                    }
                } catch (e: Exception) {
                    Log.e(TAG, "BT receive file failed", e)
                    try { sink.rollback() } catch (_: Exception) {}
                    TransferBus.emit(TransferEvent.Failed(rid, e.message ?: "Receive failed"))
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "BT receive loop ended: ${e.message}")
        } finally {
            _closed.value = true
        }
    }

    override fun close() {
        _closed.value = true
        try { socket.close() } catch (e: IOException) { Log.w(TAG, "close", e) }
        scope.cancel()
    }

    companion object {
        private const val TAG = "BluetoothReceiverConn"
    }
}
