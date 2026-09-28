package com.example.wifidirect.transfer

import android.content.Context
import android.util.Log
import com.example.wifidirect.transfer.TransferEvent.FileReceived
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.text.SimpleDateFormat
import java.util.ArrayList
import java.util.Date
import java.util.Locale

/**
 * 传输历史持久化：把已完成的接收 / 发送任务写到 `filesDir/transfer_history.json`。
 *
 * 设计选择：
 * - 不引入 Room / SQLite 数据库，因为这个数据量极小（典型几十条）、查询模式简单
 *   （顺序遍历即可）。一个 JSON 文件足够，少了一个抽象层。
 * - 持久化内容只针对「已完成」的事件（DONE / FAILED / CANCELED 不入档）。
 * - 所有公共方法都是线程安全的（内部 synchronized）。
 *
 * 表结构（JSON）：
 *
 *     [
 *       {
 *         "timestamp": "2024-09-25T16:32:11",
 *         "direction": "RECEIVING",  // or "SENDING"
 *         "transport": "WIFI_DIRECT", // or "BLUETOOTH"
 *         "fileName": "photo.jpg",
 *         "size": 1234567,
 *         "savedPath": "/storage/.../photo.jpg",  // 仅接收任务有
 *         "mimeType": "image/jpeg"                  // 仅接收任务有
 *       },
 *       ...
 *     ]
 *
 * 最多保留 [MAX_RECORDS] 条记录，超出后从最老的开始裁剪。
 */
class TransferHistory private constructor(private val storeFile: File) {

    private val lock = Any()

    fun all(): List<Record> = synchronized(lock) {
        if (!storeFile.exists()) return@synchronized emptyList()
        try {
            val text = storeFile.readText()
            if (text.isBlank()) return@synchronized emptyList()
            val arr = JSONArray(text)
            val list = ArrayList<Record>(arr.length())
            for (i in 0 until arr.length()) {
                runCatching { list += Record.fromJson(arr.getJSONObject(i)) }
            }
            list
        } catch (e: Exception) {
            Log.w(TAG, "读取历史失败: ${e.message}")
            emptyList()
        }
    }

    fun add(record: Record) = synchronized(lock) {
        val current = all().toMutableList()
        current.add(0, record)
        // 裁剪到 MAX_RECORDS 条
        while (current.size > MAX_RECORDS) current.removeAt(current.size - 1)
        save(current)
    }

    fun clear() = synchronized(lock) {
        try { storeFile.delete() } catch (e: Exception) {
            Log.w(TAG, "清空历史失败: ${e.message}")
        }
    }

    private fun save(records: List<Record>) {
        try {
            val arr = JSONArray()
            records.forEach { arr.put(it.toJson()) }
            storeFile.parentFile?.takeIf { !it.exists() }?.mkdirs()
            storeFile.writeText(arr.toString())
        } catch (e: Exception) {
            Log.w(TAG, "写入历史失败: ${e.message}")
        }
    }

    data class Record(
        val timestamp: String,
        val direction: Direction,
        val transport: Transport,
        val fileName: String,
        val size: Long,
        val savedPath: String?,
        val mimeType: String?
    ) {
        fun toJson(): JSONObject = JSONObject().apply {
            put("timestamp", timestamp)
            put("direction", direction.name)
            put("transport", transport.name)
            put("fileName", fileName)
            put("size", size)
            savedPath?.let { put("savedPath", it) }
            mimeType?.let { put("mimeType", it) }
        }

        companion object {
            fun fromJson(o: JSONObject): Record = Record(
                timestamp = o.optString("timestamp"),
                direction = Direction.valueOf(o.optString("direction", "RECEIVING")),
                transport = Transport.valueOf(o.optString("transport", "WIFI_DIRECT")),
                fileName = o.optString("fileName"),
                size = o.optLong("size", 0L),
                savedPath = o.optString("savedPath").ifBlank { null },
                mimeType = o.optString("mimeType").ifBlank { null }
            )
        }
    }

    enum class Direction { SENDING, RECEIVING }

    companion object {
        private const val TAG = "TransferHistory"
        private const val MAX_RECORDS = 200

        @Volatile private var instance: TransferHistory? = null

        /**
         * 初始化单例。应在 Application / Activity onCreate 里调用一次。
         * 之后调用 [get] 即可，无需传 context。
         */
        fun init(context: Context) {
            if (instance != null) return
            synchronized(this) {
                if (instance == null) {
                    instance = TransferHistory(File(context.filesDir, "transfer_history.json"))
                }
            }
        }

        /**
         * 获取单例。必须先调用 [init] 一次。
         * 设计为内部组件（Service / Connection）使用，避免它们持有 Context。
         */
        fun get(): TransferHistory = instance
            ?: throw IllegalStateException("TransferHistory.init() must be called first")

        /** 用于测试重置单例。 */
        internal fun resetForTest() { instance = null }

        private val isoFormat = SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss", Locale.ROOT)

        fun nowIso(): String = isoFormat.format(Date())
    }
}

/** 把 [FileReceived] 事件转化为历史记录的便利扩展。 */
fun FileReceived.toHistoryRecord(): TransferHistory.Record =
    TransferHistory.Record(
        timestamp = TransferHistory.nowIso(),
        direction = TransferHistory.Direction.RECEIVING,
        transport = transport,
        fileName = fileName,
        size = size,
        savedPath = absolutePath,
        mimeType = mimeType
    )
