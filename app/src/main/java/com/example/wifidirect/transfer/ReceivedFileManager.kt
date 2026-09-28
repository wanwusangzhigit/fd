package com.example.wifidirect.transfer

import android.content.ContentValues
import android.content.Context
import android.media.MediaScannerConnection
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import android.util.Log
import com.example.wifidirect.util.sanitizeFilename
import java.io.File
import java.io.IOException

/**
 * 统一处理"接收到的文件应该写到哪儿 / 写完后怎么让用户在系统文件管理器看到"。
 *
 * 按用户的明确选择采用如下策略：
 *
 * - **Android 10 (API 29) 及以上**：写入公共 `Downloads/WiFiDirectFileTransfer/`
 *   通过 [MediaStore.Downloads] —— 用户可在系统「文件」/「下载」直接看到，
 *   无需任何运行时权限。
 * - **Android 9 (API 28) 及以下**：写入 app-specific 外存下的同名子目录，
 *   写完后通过 [MediaScannerConnection.scanFile] 让 MediaStore 索引它。
 *
 * 同时提供一个「打开」入口（[openFileIntent]），供 UI 用 FileProvider 启动
 * ACTION_VIEW。
 */
class ReceivedFileManager(private val context: Context) {

    companion object {
        private const val TAG = "ReceivedFileManager"
    }

    /**
     * 一次接收会话：调用方先创建 [Sink]，写完后 [commit]（或失败时 [rollback]）。
     * 内部先写到 `.part` 文件 / pending 行，避免半截文件被索引。
     */
    abstract class Sink {
        abstract val output: java.io.OutputStream

        /** 提交并触发 MediaStore 扫描。返回最终对外可见的路径（可能是 content uri 字符串）。 */
        fun commit(): String = doCommit()

        /** 提交失败时清理临时文件。 */
        fun rollback() = doRollback()

        protected abstract fun doCommit(): String
        protected abstract fun doRollback(): String
    }

    /**
     * 创建一个用于接收 [displayName] 的临时 [Sink]。
     */
    fun begin(displayName: String): Sink {
        val safeName = sanitizeFilename(displayName).ifEmpty { "received.bin" }
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            MediaStoreSink(context, safeName)
        } else {
            LegacySink(context, safeName)
        }
    }

    /**
     * 构造一个用于 ACTION_VIEW 的 [android.content.Intent]，打开已接收的文件。
     * 调用方需要给 intent 加 `FLAG_GRANT_READ_URI_PERMISSION`。
     */
    fun openFileIntent(pathOrUri: String, mimeType: String?): android.content.Intent {
        val uri: Uri = if (pathOrUri.startsWith("content://")) {
            Uri.parse(pathOrUri)
        } else {
            androidx.core.content.FileProvider.getUriForFile(
                context,
                "${context.packageName}.fileprovider",
                File(pathOrUri)
            )
        }
        return android.content.Intent(android.content.Intent.ACTION_VIEW).apply {
            setDataAndType(uri, mimeType ?: "*/*")
            addFlags(android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
    }

    // --- Android 10+ via MediaStore.Downloads -------------------------------

    @androidx.annotation.RequiresApi(Build.VERSION_CODES.Q)
    private class MediaStoreSink(private val context: Context, private val name: String) : Sink() {

        private val pendingName = "$name.part"
        private val cr = context.contentResolver
        private val collection = MediaStore.Downloads.EXTERNAL_CONTENT_URI
        private var itemUri: Uri? = null

        override val output: java.io.OutputStream = run {
            val values = ContentValues().apply {
                put(MediaStore.Downloads.DISPLAY_NAME, pendingName)
                put(MediaStore.Downloads.RELATIVE_PATH,
                    Environment.DIRECTORY_DOWNLOADS + "/" + Constants.RECEIVED_DIR_NAME)
                put(MediaStore.Downloads.IS_PENDING, 1)
            }
            val uri = cr.insert(collection, values)
                ?: throw IOException("MediaStore.insert returned null for $pendingName")
            itemUri = uri
            cr.openOutputStream(uri, "w")
                ?: throw IOException("MediaStore.openOutputStream returned null for $uri")
        }

        override fun doCommit(): String {
            try {
                output.flush()
                output.close()
            } catch (e: IOException) {
                Log.w(TAG, "flush failed on commit", e)
            }
            val uri = itemUri ?: return ""
            val values = ContentValues().apply {
                put(MediaStore.Downloads.DISPLAY_NAME, name)
                put(MediaStore.Downloads.IS_PENDING, 0)
            }
            try {
                cr.update(uri, values, null, null)
            } catch (e: Exception) {
                Log.w(TAG, "rename via update failed", e)
            }
            return uri.toString()
        }

        override fun doRollback(): String {
            try { output.close() } catch (_: IOException) {}
            itemUri?.let { uri ->
                try { cr.delete(uri, null, null) } catch (e: Exception) {
                    Log.w(TAG, "rollback delete failed", e)
                }
            }
            return ""
        }
    }

    // --- Android 9 and below ------------------------------------------------

    private class LegacySink(private val context: Context, private val name: String) : Sink() {

        private val dir = File(
            context.getExternalFilesDir(Environment.DIRECTORY_DOWNLOADS)
                ?: context.filesDir,
            Constants.RECEIVED_DIR_NAME
        ).apply { if (!exists()) mkdirs() }

        private val tmp = File(dir, "$name.part")
        private var committedFile: File? = null

        override val output: java.io.OutputStream = java.io.FileOutputStream(tmp)

        override fun doCommit(): String {
            try { output.flush(); output.close() } catch (e: IOException) {
                Log.w(TAG, "flush on commit failed", e)
            }
            // 同名文件已存在时改用 `name (1).ext` 规避冲突
            val dot = name.lastIndexOf('.')
            val (base, ext) = if (dot > 0) name.substring(0, dot) to name.substring(dot)
                              else name to ""
            var target = File(dir, name)
            var i = 1
            while (target.exists()) {
                target = File(dir, "$base ($i)$ext")
                i++
            }
            if (!tmp.renameTo(target)) {
                tmp.copyTo(target, overwrite = true)
                tmp.delete()
            }
            committedFile = target
            // 触发 MediaStore 索引让用户在文件管理器中可见
            MediaScannerConnection.scanFile(context, arrayOf(target.absolutePath), null, null)
            return target.absolutePath
        }

        override fun doRollback(): String {
            try { output.close() } catch (_: IOException) {}
            tmp.delete()
            return ""
        }
    }
}
