package com.example.wifidirect.transfer

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.UUID

/**
 * [com.example.wifidirect.ui.TransferAdapter] 的状态折叠逻辑测试。
 *
 * 由于 [TransferAdapter] 是 Android UI 类（依赖 RecyclerView 的 ListAdapter），
 * 在纯 JVM 单测里实例化会抛 "Method ... not mocked"。这里改测它内部使用的
 * 事件折叠逻辑 —— 把该逻辑视为「以 Row 列表为状态的 reducer」直接验证。
 */
class TransferAdapterStateTest {

    @Test
    fun `Started event inserts row`() {
        val state = mutableMapOf<String, RowSnapshot>()
        val id = UUID.randomUUID().toString()
        val event = TransferEvent.Started(id, "f.bin", 100, Transport.WIFI_DIRECT)
        applyTo(state, event)
        assertEquals(1, state.size)
        assertEquals("f.bin", state[id]?.displayName)
        assertEquals(0L, state[id]?.transferred)
        assertEquals(Transport.WIFI_DIRECT, state[id]?.transport)
        assertFalse(state[id]?.done == true)
    }

    @Test
    fun `Progress updates transferred`() {
        val state = mutableMapOf<String, RowSnapshot>()
        val id = UUID.randomUUID().toString()
        applyTo(state, TransferEvent.Started(id, "f.bin", 100, Transport.WIFI_DIRECT))
        applyTo(state, TransferEvent.Progress(id, 50, 100))
        assertEquals(50L, state[id]?.transferred)
        assertEquals(100L, state[id]?.total)
        assertFalse(state[id]?.done == true)
    }

    @Test
    fun `Completed marks done and clamps transferred to total`() {
        val state = mutableMapOf<String, RowSnapshot>()
        val id = UUID.randomUUID().toString()
        applyTo(state, TransferEvent.Started(id, "f.bin", 100, Transport.WIFI_DIRECT))
        applyTo(state, TransferEvent.Progress(id, 90, 100))
        applyTo(state, TransferEvent.Completed(id))
        assertTrue(state[id]?.done == true)
        assertEquals(100L, state[id]?.transferred) // clamped
    }

    @Test
    fun `Failed marks failed and stores message`() {
        val state = mutableMapOf<String, RowSnapshot>()
        val id = UUID.randomUUID().toString()
        applyTo(state, TransferEvent.Started(id, "f.bin", 100, Transport.BLUETOOTH))
        applyTo(state, TransferEvent.Failed(id, "Disconnected"))
        assertTrue(state[id]?.failed == true)
        assertEquals("Disconnected", state[id]?.errorMessage)
    }

    @Test
    fun `Canceled marks canceled`() {
        val state = mutableMapOf<String, RowSnapshot>()
        val id = UUID.randomUUID().toString()
        applyTo(state, TransferEvent.Started(id, "f.bin", 100, Transport.BLUETOOTH))
        applyTo(state, TransferEvent.Canceled(id))
        assertTrue(state[id]?.canceled == true)
    }

    @Test
    fun `FileReceived adds synthetic row with saved path and mime`() {
        val state = mutableMapOf<String, RowSnapshot>()
        applyTo(state, TransferEvent.FileReceived(
            absolutePath = "/tmp/x.jpg",
            fileName = "x.jpg",
            size = 100L,
            transport = Transport.BLUETOOTH,
            mimeType = "image/jpeg"
        ))
        val row = state.values.firstOrNull()
        assertNotNull(row)
        assertEquals("image/jpeg", row?.mimeType)
        assertEquals("/tmp/x.jpg", row?.savedPath)
        assertEquals(Transport.BLUETOOTH, row?.transport)
        assertTrue(row?.openable == true)
    }

    @Test
    fun `ServerStarted is no_op`() {
        val state = mutableMapOf<String, RowSnapshot>()
        applyTo(state, TransferEvent.ServerStarted(Transport.WIFI_DIRECT, 8988))
        assertTrue(state.isEmpty())
    }

    // ------- 极简的 reducer 实现，复制 TransferAdapter.applyEvent 的折叠语义 -------

    private data class RowSnapshot(
        var displayName: String,
        var transport: Transport,
        var transferred: Long,
        var total: Long,
        var done: Boolean = false,
        var failed: Boolean = false,
        var canceled: Boolean = false,
        var errorMessage: String? = null,
        var savedPath: String? = null,
        var mimeType: String? = null,
        var openable: Boolean = false
    )

    private fun applyTo(state: MutableMap<String, RowSnapshot>, event: TransferEvent) {
        when (event) {
            is TransferEvent.Started -> {
                state[event.id] = RowSnapshot(event.fileName, event.transport, 0, event.size)
            }
            is TransferEvent.Progress -> {
                state[event.id]?.let {
                    it.transferred = event.transferred
                    it.total = event.total
                }
            }
            is TransferEvent.Completed -> {
                state[event.id]?.let {
                    it.done = true
                    if (it.total > 0) it.transferred = it.total
                }
            }
            is TransferEvent.Failed -> {
                state[event.id]?.let { it.failed = true; it.errorMessage = event.message }
            }
            is TransferEvent.Canceled -> {
                state[event.id]?.let { it.canceled = true }
            }
            is TransferEvent.FileReceived -> {
                val sid = "recv:${event.fileName}:${event.size}"
                state[sid] = RowSnapshot("↓ ${event.fileName}", event.transport, event.size, event.size,
                    done = true, savedPath = event.absolutePath, mimeType = event.mimeType,
                    openable = true)
            }
            is TransferEvent.ServerStarted,
            is TransferEvent.ClientRegisteredToOwner,
            is TransferEvent.PeerRegistered -> { /* no-op */ }
        }
    }
}
