package com.example.wifidirect.manager

import android.Manifest
import android.app.Activity
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.net.wifi.p2p.WifiP2pConfig
import android.net.wifi.p2p.WifiP2pDevice
import android.net.wifi.p2p.WifiP2pDeviceList
import android.net.wifi.p2p.WifiP2pInfo
import android.net.wifi.p2p.WifiP2pManager
import android.os.Build
import android.util.Log
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.annotation.RequiresPermission
import com.example.wifidirect.transfer.Constants
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch

/**
 * Manages WiFi Direct (Wi-Fi P2P) discovery, connection, and group ownership.
 */
class WiFiDirectManager(private val context: Context) {

    companion object {
        private const val TAG = "WiFiDirectManager"
        const val PERMISSION_REQUEST_CODE = 1001
    }

    private val manager: WifiP2pManager? =
        context.getSystemService(Context.WIFI_P2P_SERVICE) as? WifiP2pManager
    private var channel: WifiP2pManager.Channel? = null
    private var receiver: BroadcastReceiver? = null

    private val _peers = MutableStateFlow<List<WifiP2pDevice>>(emptyList())
    val peers: StateFlow<List<WifiP2pDevice>> = _peers

    private val _thisDevice = MutableStateFlow<WifiP2pDevice?>(null)
    val thisDevice: StateFlow<WifiP2pDevice?> = _thisDevice

    private val _connectionInfo = MutableStateFlow<WifiP2pInfo?>(null)
    val connectionInfo: StateFlow<WifiP2pInfo?> = _connectionInfo

    private val _isConnected = MutableStateFlow(false)
    val isConnected: StateFlow<Boolean> = _isConnected

    private val _statusMessage = MutableStateFlow("")
    val statusMessage: StateFlow<String> = _statusMessage

    /**
     * 我们最近调用过 `connect()` 但还没收到 `WIFI_P2P_CONNECTION_CHANGED_ACTION` 的设备地址。
     * 用于 [pendingConnectTimeout] 的超时清理 —— 旧版本没有这个清理，连接失败 / 对端不确认
     * 时状态会一直停在 "connecting"，UI 也无法重新发起连接。
     */
    private val _pendingConnect = MutableStateFlow<String?>(null)
    val pendingConnect: StateFlow<String?> = _pendingConnect

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var connectTimeoutJob: Job? = null

    init {
        channel = manager?.initialize(context, context.mainLooper, WifiP2pManager.ChannelListener {
            Log.w(TAG, "Wi-Fi P2P channel disconnected, retry")
            _statusMessage.value = "P2P channel disconnected"
        })
    }

    val isP2pSupported: Boolean
        get() = context.packageManager.hasSystemFeature(PackageManager.FEATURE_WIFI_DIRECT)

    fun initialize(activity: Activity): Boolean {
        if (!isP2pSupported) {
            _statusMessage.value = "Wi-Fi Direct not supported on this device"
            return false
        }

        if (!checkAndRequestPermissions(activity)) {
            return false
        }

        registerReceiver()
        _statusMessage.value = "Wi-Fi Direct initialized"
        return true
    }

    private fun registerReceiver() {
        if (receiver != null) return
        val filter = IntentFilter().apply {
            addAction(WifiP2pManager.WIFI_P2P_STATE_CHANGED_ACTION)
            addAction(WifiP2pManager.WIFI_P2P_PEERS_CHANGED_ACTION)
            addAction(WifiP2pManager.WIFI_P2P_CONNECTION_CHANGED_ACTION)
            addAction(WifiP2pManager.WIFI_P2P_THIS_DEVICE_CHANGED_ACTION)
        }
        receiver = object : BroadcastReceiver() {
            override fun onReceive(ctx: Context, intent: Intent) {
                when (intent.action) {
                    WifiP2pManager.WIFI_P2P_STATE_CHANGED_ACTION -> {
                        val state = intent.getIntExtra(
                            WifiP2pManager.EXTRA_WIFI_STATE, -1
                        )
                        val enabled = state == WifiP2pManager.WIFI_P2P_STATE_ENABLED
                        _statusMessage.value =
                            if (enabled) "Wi-Fi Direct enabled" else "Wi-Fi Direct disabled"
                    }

                    WifiP2pManager.WIFI_P2P_PEERS_CHANGED_ACTION -> {
                        @Suppress("MissingPermission")
                        manager?.let { requestPeers(it) }
                    }

                    WifiP2pManager.WIFI_P2P_CONNECTION_CHANGED_ACTION -> {
                        manager?.let { requestConnectionInfo(it) }
                    }

                    WifiP2pManager.WIFI_P2P_THIS_DEVICE_CHANGED_ACTION -> {
                        val device = androidx.core.content.IntentCompat.getParcelableExtra(
                            intent, WifiP2pManager.EXTRA_WIFI_P2P_DEVICE,
                            WifiP2pDevice::class.java
                        )
                        _thisDevice.value = device
                    }
                }
            }
        }
        ContextCompat.registerReceiver(
            context, receiver!!, filter, ContextCompat.RECEIVER_NOT_EXPORTED
        )
    }

    @Suppress("DEPRECATION")
    @RequiresPermission(anyOf = [
        Manifest.permission.ACCESS_FINE_LOCATION,
        Manifest.permission.NEARBY_WIFI_DEVICES
    ])
    private fun requestPeers(m: WifiP2pManager) {
        if (!hasPermissions()) return
        m.requestPeers(channel) { peersList: WifiP2pDeviceList ->
            _peers.value = peersList.deviceList.toList()
        }
    }

    @Suppress("DEPRECATION")
    private fun requestConnectionInfo(m: WifiP2pManager) {
        if (!hasPermissions()) return
        m.requestConnectionInfo(channel) { info: WifiP2pInfo ->
            _connectionInfo.value = info
            _isConnected.value = info.groupFormed && info.groupOwnerAddress != null
            // 连接结果到达 —— 取消超时计时与 pending 标记
            connectTimeoutJob?.cancel()
            connectTimeoutJob = null
            _pendingConnect.value = null
            if (info.groupFormed) {
                _statusMessage.value =
                    if (info.isGroupOwner) "Group owner at ${info.groupOwnerAddress}"
                    else "Connected as client to ${info.groupOwnerAddress}"
            }
        }
    }

    @RequiresPermission(anyOf = [
        Manifest.permission.ACCESS_FINE_LOCATION,
        Manifest.permission.NEARBY_WIFI_DEVICES
    ])
    fun discoverPeers() {
        if (!hasPermissions()) {
            _statusMessage.value = "Missing permissions for peer discovery"
            return
        }
        manager?.let { m ->
            m.discoverPeers(channel, object : WifiP2pManager.ActionListener {
                override fun onSuccess() {
                    _statusMessage.value = "Discovery started"
                }

                override fun onFailure(reason: Int) {
                    _statusMessage.value = "Discovery failed: ${reasonText(reason)}"
                }
            })
        } ?: run { _statusMessage.value = "Wi-Fi Direct unavailable" }
    }

    @RequiresPermission(anyOf = [
        Manifest.permission.ACCESS_FINE_LOCATION,
        Manifest.permission.NEARBY_WIFI_DEVICES
    ])
    fun connect(device: WifiP2pDevice) {
        if (!hasPermissions()) {
            _statusMessage.value = "Missing permissions for connection"
            return
        }
        manager?.let { m ->
            val config = WifiP2pConfig().apply {
                deviceAddress = device.deviceAddress
            }
            m.connect(channel, config, object : WifiP2pManager.ActionListener {
                override fun onSuccess() {
                    _statusMessage.value = "Connecting to ${device.deviceName}..."
                    _pendingConnect.value = device.deviceAddress
                    // 启动超时清理：超过阈值仍没收到 connectionInfo，就告知调用方放弃
                    connectTimeoutJob?.cancel()
                    connectTimeoutJob = scope.launch {
                        delay(Constants.WFD_CONNECT_TIMEOUT_MS)
                        if (_pendingConnect.value != null) {
                            _statusMessage.value = "Connect timed out"
                            _pendingConnect.value = null
                        }
                    }
                }

                override fun onFailure(reason: Int) {
                    _statusMessage.value = "Connect failed: ${reasonText(reason)}"
                    _pendingConnect.value = null
                }
            })
        }
    }

    fun disconnectFromGroup() {
        manager?.removeGroup(channel, object : WifiP2pManager.ActionListener {
            override fun onSuccess() {
                _statusMessage.value = "Disconnected from group"
            }

            override fun onFailure(reason: Int) {
                _statusMessage.value = "Disconnect failed: ${reasonText(reason)}"
            }
        })
    }

    fun cancelPeerDiscovery() {
        manager?.stopPeerDiscovery(channel, object : WifiP2pManager.ActionListener {
            override fun onSuccess() {}
            override fun onFailure(reason: Int) {}
        })
    }

    private fun reasonText(reason: Int): String = when (reason) {
        WifiP2pManager.P2P_UNSUPPORTED -> "P2P unsupported"
        WifiP2pManager.BUSY -> "framework busy"
        WifiP2pManager.ERROR -> "internal error"
        else -> "reason $reason"
    }

    private fun hasPermissions(): Boolean {
        val needed = permissionsForSdk()
        return needed.all {
            ContextCompat.checkSelfPermission(context, it) == PackageManager.PERMISSION_GRANTED
        }
    }

    private fun checkAndRequestPermissions(activity: Activity): Boolean {
        val needed = permissionsForSdk()
        val toRequest = needed.filter {
            ContextCompat.checkSelfPermission(activity, it) != PackageManager.PERMISSION_GRANTED
        }
        if (toRequest.isNotEmpty()) {
            ActivityCompat.requestPermissions(
                activity, toRequest.toTypedArray(), PERMISSION_REQUEST_CODE
            )
            return false
        }
        return true
    }

    private fun permissionsForSdk(): List<String> {
        val perms = mutableListOf(
            Manifest.permission.ACCESS_WIFI_STATE,
            Manifest.permission.CHANGE_WIFI_STATE
        )
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            perms += Manifest.permission.NEARBY_WIFI_DEVICES
        } else {
            perms += Manifest.permission.ACCESS_FINE_LOCATION
        }
        return perms
    }

    fun cleanup() {
        try {
            disconnectFromGroup()
            cancelPeerDiscovery()
        } catch (e: Exception) {
            Log.w(TAG, "cleanup error", e)
        }
        receiver?.let {
            try {
                context.unregisterReceiver(it)
            } catch (e: IllegalArgumentException) {
                Log.w(TAG, "receiver not registered", e)
            }
        }
        receiver = null
    }
}
