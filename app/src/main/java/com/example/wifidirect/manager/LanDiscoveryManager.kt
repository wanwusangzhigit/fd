package com.example.wifidirect.manager

import android.util.Log
import com.example.wifidirect.transfer.Constants
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetSocketAddress
import java.net.SocketTimeoutException

/**
 * 监听桌面 native 客户端广播的 UDP beacon，暴露同一 LAN 上的对端列表。
 *
 * 只收不发——Android 端不广播自己的 beacon，因此两台 Android 之间不会
 * 通过这里互相发现；被发现的都是正在广播的 native（Windows/Linux）客户端。
 */
class LanDiscoveryManager {

    companion object {
        private const val TAG = "LanDiscovery"
    }

    private val _peers = MutableStateFlow<List<LanPeer>>(emptyList())
    val peers: StateFlow<List<LanPeer>> = _peers.asStateFlow()

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var job: Job? = null
    private var socket: DatagramSocket? = null

    fun start() {
        if (job != null) return
        job = scope.launch {
            var sock: DatagramSocket? = null
            try {
                // 必须先 reuseAddress 再 bind，否则与同机其它监听者（含桌面
                // beacon）抢占端口时 bind 失败。桌面侧也开了 SO_REUSEADDR。
                sock = DatagramSocket(null).apply {
                    reuseAddress = true
                    soTimeout = 1000
                    bind(InetSocketAddress("0.0.0.0", Constants.DISCOVERY_PORT))
                }
                socket = sock
                Log.i(TAG, "LAN discovery listening on :${Constants.DISCOVERY_PORT}")

                val buf = ByteArray(1024)
                while (isActive) {
                    val packet = DatagramPacket(buf, buf.size)
                    try {
                        sock.receive(packet)
                    } catch (_: SocketTimeoutException) {
                        // 每秒醒来一次，顺手清理过期 peer
                        prune()
                        continue
                    }
                    val peer = BeaconParser.parse(
                        packet.data, packet.length, System.currentTimeMillis())
                        ?: continue
                    if (peer.ip.isEmpty()) continue
                    merge(peer)
                    prune()
                }
            } catch (e: Exception) {
                Log.w(TAG, "LAN discovery stopped: ${e.message}")
            } finally {
                sock?.close()
                if (socket === sock) socket = null
            }
        }
    }

    fun stop() {
        job?.cancel()
        job = null
        try { socket?.close() } catch (_: Exception) {}
        socket = null
    }

    fun cleanup() {
        stop()
        scope.cancel()
    }

    /** 以 ip 为 key 合并：已存在则更新时间与名称，否则新增。 */
    private fun merge(peer: LanPeer) {
        val now = peer.lastSeenMs
        val updated = _peers.value.toMutableList()
        val idx = updated.indexOfFirst { it.ip == peer.ip }
        if (idx >= 0) {
            updated[idx] = updated[idx].copy(name = peer.name, lastSeenMs = now)
        } else {
            updated += peer
        }
        _peers.value = updated.sortedByDescending { it.lastSeenMs }
    }

    /** 剔除超过 TTL 未再收到 beacon 的 peer。 */
    private fun prune() {
        val now = System.currentTimeMillis()
        val kept = _peers.value.filter { now - it.lastSeenMs <= Constants.LAN_PEER_TTL_MS }
        if (kept.size != _peers.value.size) _peers.value = kept
    }
}
