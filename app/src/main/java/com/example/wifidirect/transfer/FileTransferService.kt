package com.example.wifidirect.transfer

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.Uri
import android.os.Build
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.lifecycle.LifecycleService
import androidx.lifecycle.lifecycleScope
import com.example.wifidirect.App
import com.example.wifidirect.R
import com.example.wifidirect.ui.MainActivity
import com.example.wifidirect.util.streamTo
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.onEach
import kotlinx.coroutines.launch
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.BindException
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicBoolean

/**
 * 前台服务，负责：
 * 1. 持续运行的 TCP 文件接收 server（[Constants.WIFI_FILE_PORT]）—— 不论组主还是
 *    客户端都启动，因为同一时刻谁是 sender 谁是 receiver 由用户操作决定；
 * 2. 持续运行的 IP 上报接收 server（[Constants.WIFI_REGISTER_PORT]）—— 仅组主
 *    侧需要（客户端侧启动它没有坏处，只是没人会上报给它）；
 * 3. 通过 [ACTION_SEND_WIFI] 启动的对外 TCP 文件发送；
 * 4. 维护一个随传输进度更新的前台通知。
 *
 * 与旧实现相比关键修复：
 * - 服务端 `accept()` 循环不再因为单次异常而退出；
 * - 进度通知随事件实时刷新；
 * - 使用 Android 15 的 `FOREGROUND_SERVICE_TYPE_FILE_SHARING`（在低版本上回退到
 *   `dataSync`），与 manifest 中声明的类型匹配；
 * - 支持取消传输（按 transfer id 调用 [TransferBus.cancelTransfer]）。
 */
class FileTransferService : LifecycleService() {

    companion object {
        private const val TAG = "FileTransferService"

        const val ACTION_SEND_WIFI = "com.example.wifidirect.SEND_WIFI"
        const val ACTION_START_SERVER = "com.example.wifidirect.START_SERVER"

        const val EXTRA_HOST = "extra_host"
        const val EXTRA_PORT = "extra_port"
        const val EXTRA_FILE_URI = "extra_file_uri"
        const val EXTRA_FILE_NAME = "extra_file_name"
        const val EXTRA_FILE_SIZE = "extra_file_size"
        const val EXTRA_TRANSFER_ID = "extra_transfer_id"

        /** 兼容旧代码的入口：转发到 [TransferBus.emit]。 */
        fun emit(event: TransferEvent) = TransferBus.emit(event)
    }

    private val ioScope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val fileServerRunning = AtomicBoolean(false)
    private val registerServerRunning = AtomicBoolean(false)

    /** 维护发送任务的 Job 用于取消。 */
    private val sendJobs = ConcurrentHashMap<String, Job>()

    private val receivedFileManager by lazy { ReceivedFileManager(this) }

    override fun onCreate() {
        super.onCreate()
        ensureNotificationChannel()
        startForegroundCompat(getString(R.string.notif_ready))
        observeProgressForNotification()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        super.onStartCommand(intent, flags, startId)
        intent ?: return START_STICKY
        when (intent.action) {
            ACTION_START_SERVER -> {
                startFileServer()
                startRegisterServer()
            }
            ACTION_SEND_WIFI -> handleSendIntent(intent)
        }
        return START_STICKY
    }

    override fun onDestroy() {
        fileServerRunning.set(false)
        registerServerRunning.set(false)
        ioScope.cancel()
        super.onDestroy()
    }

    // ----- TCP file server --------------------------------------------------

    private fun startFileServer() {
        if (!fileServerRunning.compareAndSet(false, true)) return
        ioScope.launch {
            var server: ServerSocket? = null
            try {
                server = ServerSocket(Constants.WIFI_FILE_PORT)
                Log.i(TAG, "TCP file server listening on :${Constants.WIFI_FILE_PORT}")
                emit(TransferEvent.ServerStarted(Transport.WIFI_DIRECT, Constants.WIFI_FILE_PORT))
                while (fileServerRunning.get()) {
                    val client = try {
                        server.accept()
                    } catch (e: IOException) {
                        if (fileServerRunning.get()) Log.w(TAG, "accept failed", e)
                        continue // 关键修复：单次 accept 异常不退出循环
                    }
                    if (client == null) continue
                    launch { handleIncomingTcp(client) }
                }
            } catch (e: BindException) {
                Log.e(TAG, "无法绑定 ${Constants.WIFI_FILE_PORT}，可能已被占用", e)
                emit(TransferEvent.Failed("server-file", "Port ${Constants.WIFI_FILE_PORT} in use"))
            } catch (e: Exception) {
                Log.e(TAG, "file server crashed", e)
            } finally {
                try { server?.close() } catch (_: IOException) {}
                fileServerRunning.set(false)
            }
        }
    }

    private fun handleIncomingTcp(socket: Socket) {
        socket.use { s ->
            val transferId = UUID.randomUUID().toString()
            // 重要：先读帧头拿到真实文件名，再创建 sink。
            // 旧版本在这里先 begin("") 创建占位 sink 又 rollback，对 MediaStore 路径
            // 会产生一条瞬时 .part 表项再被删除 —— 不必要的副作用且会让用户在文件
            // 管理器里看到瞬时闪现的条目。
            val din = java.io.DataInputStream(s.getInputStream())
            val prefix = try { WireProtocol.readFramePrefix(din) }
                         catch (e: Exception) {
                Log.w(TAG, "invalid frame prefix: ${e.message}")
                return
            }
            if (prefix.kind != WireProtocol.KIND_FILE) {
                Log.w(TAG, "skip non-FILE frame on file port: kind=${prefix.kind}")
                return
            }
            val header = try { WireProtocol.readFilePayload(din) }
                         catch (e: Exception) {
                Log.w(TAG, "invalid FILE payload: ${e.message}")
                return
            }
            emit(TransferEvent.Started(transferId, header.name, header.size, Transport.WIFI_DIRECT))

            val sink = receivedFileManager.begin(header.name)
            try {
                sink.output.use { out ->
                    din.streamTo(
                        destination = out,
                        expectedBytes = header.size,
                        bufferSize = Constants.BUFFER_SIZE_RECEIVE,
                        progressIntervalBytes = Constants.PROGRESS_INTERVAL_BYTES_TCP.toLong(),
                        onProgress = { transferred ->
                            emit(TransferEvent.Progress(transferId, transferred, header.size))
                        },
                        isCancelled = { false }
                    )
                }
                val finalPath = sink.commit()
                emit(TransferEvent.Completed(transferId))
                val fileReceived = TransferEvent.FileReceived(
                    absolutePath = finalPath,
                    fileName = header.name,
                    size = header.size,
                    transport = Transport.WIFI_DIRECT,
                    mimeType = TransferFormatter.guessMimeType(header.name)
                )
                emit(fileReceived)
                TransferHistory.get().add(fileReceived.toHistoryRecord())
            } catch (e: Exception) {
                Log.e(TAG, "receive stream failed", e)
                try { sink.rollback() } catch (_: Exception) {}
                emit(TransferEvent.Failed(transferId, e.message ?: "Receive failed"))
            }
        }
    }

    // ----- TCP register server (client→owner IP reporting) -----------------

    private fun startRegisterServer() {
        if (!registerServerRunning.compareAndSet(false, true)) return
        ioScope.launch {
            var server: ServerSocket? = null
            try {
                server = ServerSocket(Constants.WIFI_REGISTER_PORT)
                Log.i(TAG, "TCP register server listening on :${Constants.WIFI_REGISTER_PORT}")
                while (registerServerRunning.get()) {
                    val client = try { server.accept() } catch (e: IOException) {
                        if (registerServerRunning.get()) Log.w(TAG, "register accept failed", e)
                        continue
                    }
                    if (client == null) continue
                    launch { handleIncomingRegister(client) }
                }
            } catch (e: BindException) {
                Log.w(TAG, "无法绑定 register 端口 ${Constants.WIFI_REGISTER_PORT}", e)
            } catch (e: Exception) {
                Log.e(TAG, "register server crashed", e)
            } finally {
                try { server?.close() } catch (_: IOException) {}
                registerServerRunning.set(false)
            }
        }
    }

    private fun handleIncomingRegister(socket: Socket) {
        socket.use { s ->
            try {
                val din = java.io.DataInputStream(s.getInputStream())
                val prefix = WireProtocol.readFramePrefix(din)
                require(prefix.kind == WireProtocol.KIND_REGISTER_IP) {
                    "Expected REGISTER_IP frame, got kind=${prefix.kind}"
                }
                val payload = WireProtocol.readRegisterIpPayload(din)
                val remoteSocket = s.inetAddress?.hostAddress ?: "?"
                Log.i(TAG, "Peer reported ip=${payload.ip} from socket=$remoteSocket")
                PeerRegistry.record(remoteSocket, payload.ip)
                emit(TransferEvent.PeerRegistered(payload.ip))
            } catch (e: Exception) {
                Log.w(TAG, "register frame parse failed", e)
            }
        }
    }

    // ----- Send -------------------------------------------------------------

    private fun handleSendIntent(intent: Intent) {
        val host = intent.getStringExtra(EXTRA_HOST) ?: return
        val port = intent.getIntExtra(EXTRA_PORT, Constants.WIFI_FILE_PORT)
        val uri = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            intent.getParcelableExtra(EXTRA_FILE_URI, Uri::class.java)
        } else {
            @Suppress("DEPRECATION") intent.getParcelableExtra(EXTRA_FILE_URI)
        } ?: return
        val name = intent.getStringExtra(EXTRA_FILE_NAME) ?: "file"
        val size = intent.getLongExtra(EXTRA_FILE_SIZE, -1L)
        val id = intent.getStringExtra(EXTRA_TRANSFER_ID) ?: UUID.randomUUID().toString()

        val job = ioScope.launch {
            sendOverTcp(id, host, port, uri, name, size)
        }
        sendJobs[id] = job
        TransferBus.registerCancelHandle(id) { job.cancel() }
    }

    private suspend fun sendOverTcp(id: String, host: String, port: Int, uri: Uri, name: String, size: Long) {
        emit(TransferEvent.Started(id, name, size, Transport.WIFI_DIRECT))
        var socket: Socket? = null
        var input: InputStream? = null
        try {
            socket = Socket()
            socket.connect(InetSocketAddress(host, port), Constants.WIFI_CONNECT_TIMEOUT_MS)
            socket.tcpNoDelay = true
            input = contentResolver.openInputStream(uri)
                ?: throw IOException("Cannot open file at $uri")

            val out = socket.getOutputStream()
            WireProtocol.writeFileHeader(out, name, size)
            input.streamTo(
                destination = out,
                expectedBytes = size,
                bufferSize = Constants.BUFFER_SIZE_TCP,
                progressIntervalBytes = Constants.PROGRESS_INTERVAL_BYTES_TCP.toLong(),
                onProgress = { transferred -> emit(TransferEvent.Progress(id, transferred, size)) },
                isCancelled = { sendJobs[id]?.isActive != true }
            )
            out.flush()
            emit(TransferEvent.Completed(id))
        } catch (e: Exception) {
            Log.e(TAG, "sendOverTcp failed", e)
            val message = if (e is kotlinx.coroutines.CancellationException) "Canceled" else (e.message ?: "Send failed")
            emit(if (e is kotlinx.coroutines.CancellationException) TransferEvent.Canceled(id)
                 else TransferEvent.Failed(id, message))
        } finally {
            sendJobs.remove(id)
            try { input?.close() } catch (_: IOException) {}
            try { socket?.close() } catch (_: IOException) {}
        }
    }

    /** 外部（MainActivity）取消某次发送。 */
    fun cancelSend(id: String) {
        sendJobs.remove(id)?.cancel()
        TransferBus.cancelTransfer(id)
    }

    // ----- Progress-driven foreground notification --------------------------

    private fun observeProgressForNotification() {
        // 计算「正在进行的所有 transfer 的平均进度」作为通知进度
        lifecycleScope.launch {
            var lastReportedPercent = -1
            var activeIds: Set<String> = emptySet()
            TransferBus.progress.combine(TransferBus.critical) { progMap, _ -> progMap }
                .map { progMap ->
                    activeIds = progMap.keys
                    if (progMap.isEmpty()) 0 to ""
                    else {
                        val avg = progMap.values.map { p ->
                            TransferFormatter.percent(p.transferred, p.total)
                        }.average().toInt()
                        val name = progMap.values.firstOrNull()?.id ?: ""
                        avg to name
                    }
                }
                .distinctUntilChanged()
                .collect { (percent, _) ->
                    if (percent == lastReportedPercent && activeIds.isEmpty()) return@collect
                    lastReportedPercent = percent
                    val text = if (activeIds.isEmpty()) getString(R.string.notif_ready)
                               else getString(R.string.notif_transferring, percent)
                    notify(buildNotification(text, percent))
                }
        }
    }

    // ----- Foreground notification plumbing ---------------------------------

    private fun ensureNotificationChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (nm.getNotificationChannel(Constants.NOTIFICATION_CHANNEL_ID) == null) {
            val channel = NotificationChannel(
                Constants.NOTIFICATION_CHANNEL_ID,
                getString(R.string.notif_channel_name),
                NotificationManager.IMPORTANCE_LOW
            ).apply { description = getString(R.string.notif_channel_desc) }
            nm.createNotificationChannel(channel)
        }
    }

    private fun startForegroundCompat(text: String) {
        val notif = buildNotification(text, 0)
        // FOREGROUND_SERVICE_TYPE_FILE_SHARING 是 Android 16 (API 36) 引入的；
        // 当前 compileSdk=35 还没有这个常量，所以暂用 dataSync（Android 15 上仍可用）。
        // 等 SDK 36 稳定后可换成 FOREGROUND_SERVICE_TYPE_FILE_SHARING。
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(Constants.NOTIFICATION_ID, notif,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)
        } else {
            startForeground(Constants.NOTIFICATION_ID, notif)
        }
    }

    private fun notify(notification: Notification) {
        try {
            NotificationManagerCompat.from(this).notify(Constants.NOTIFICATION_ID, notification)
        } catch (e: SecurityException) {
            // POST_NOTIFICATIONS 权限被拒：前台通知本身仍生效，只是无法更新外观
            Log.w(TAG, "notify failed: ${e.message}")
        }
    }

    private fun buildNotification(text: String, percent: Int): Notification {
        val openIntent = Intent(this, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP
        }
        val pi = PendingIntent.getActivity(
            this, 0, openIntent,
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )
        return NotificationCompat.Builder(this, Constants.NOTIFICATION_CHANNEL_ID)
            .setSmallIcon(android.R.drawable.stat_sys_upload)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(text)
            .setContentIntent(pi)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setProgress(100, percent, percent <= 0 && percent != 0)
            .build()
    }
}
