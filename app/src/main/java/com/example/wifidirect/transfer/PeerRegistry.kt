package com.example.wifidirect.transfer

import java.net.InetAddress
import java.net.Socket
import java.util.concurrent.ConcurrentHashMap

/**
 * 组主侧维护「peer MAC → 客户端 IP」表，用于反向发送。
 *
 * 客户端连上组主后，会通过 [Constants.WIFI_REGISTER_PORT] 主动连过来并
 * 写一个 [WireProtocol.KIND_REGISTER_IP] 帧，告诉组主自己的 IP。
 * 组主侧调用 [record] 记下来，发送时用 [ipOf] 查目标。
 *
 * 客户端侧调用 [reportToOwner] 把自己 IP 上报给组主。
 */
object PeerRegistry {

    private val macToIp = ConcurrentHashMap<String, String>()

    /** 组主侧记录某 peer 上报的 IP。 */
    fun record(mac: String, ip: String) {
        macToIp[mac] = ip
    }

    /** 组主侧查询某 peer 的 IP；不存在则返回 null。 */
    fun ipOf(mac: String): String? = macToIp[mac]

    /** 测试 / 重置使用。 */
    fun clear() = macToIp.clear()

    /**
     * 客户端侧：把自己本机的 IP（在 Wi-Fi Direct 组里的 IP）上报给组主。
     * 阻塞调用，发送失败时仅打日志不抛出（上报失败只是无法支持组主反向发送，
     * 不应该阻断主流程）。
     */
    fun reportToOwner(ownerHost: String, myIp: String) {
        var socket: Socket? = null
        try {
            socket = Socket()
            socket.connect(
                java.net.InetSocketAddress(ownerHost, Constants.WIFI_REGISTER_PORT),
                Constants.WIFI_CONNECT_TIMEOUT_MS
            )
            WireProtocol.writeRegisterIp(socket.getOutputStream(), myIp)
            TransferBus.emit(TransferEvent.ClientRegisteredToOwner(myIp))
        } catch (e: Exception) {
            android.util.Log.w("PeerRegistry", "reportToOwner failed: ${e.message}")
        } finally {
            try { socket?.close() } catch (_: Exception) {}
        }
    }

    /**
     * 在 Wi-Fi Direct 组里，客户端的 IP 通常是 `192.168.49.x`（组主固定为 `.1`）。
     * 我们枚举本机网卡地址找出在该网段内的 IPv4 地址返回；找不到则返回 null。
     */
    fun localWifiDirectIp(): String? {
        return try {
            val ifaces = java.net.NetworkInterface.getNetworkInterfaces()?.toList().orEmpty()
            for (iface in ifaces) {
                if (!iface.isUp || iface.isLoopback) continue
                for (addr in iface.inetAddresses) {
                    if (!addr.isLoopbackAddress && addr is InetAddress && !addr.isLinkLocalAddress) {
                        val host = addr.hostAddress ?: continue
                        // p2p 接口通常含 "p2p" 字样，或地址在 192.168.49.0/24 网段
                        if (iface.name.contains("p2p", ignoreCase = true) ||
                            host.startsWith("192.168.49.")) {
                            return host
                        }
                    }
                }
            }
            // 兜底：返回任意一个非 loopback 的 IPv4
            for (iface in ifaces) {
                for (addr in iface.inetAddresses) {
                    if (!addr.isLoopbackAddress && addr is InetAddress && !addr.isLinkLocalAddress) {
                        return addr.hostAddress
                    }
                }
            }
            null
        } catch (e: Exception) {
            android.util.Log.w("PeerRegistry", "localWifiDirectIp failed: ${e.message}")
            null
        }
    }
}
