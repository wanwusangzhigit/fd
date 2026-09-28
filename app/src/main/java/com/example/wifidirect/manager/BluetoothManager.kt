package com.example.wifidirect.manager

import android.Manifest
import android.app.Activity
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothServerSocket
import android.bluetooth.BluetoothSocket
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.os.Build
import android.util.Log
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.annotation.RequiresPermission
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import java.io.IOException
import java.util.UUID

/**
 * Manages Bluetooth discovery, pairing, and the BluetoothSocket connection.
 *
 * Also runs a background "server" that listens for incoming RFCOMM connections
 * (needed for receiving files over Bluetooth).
 */
class BluetoothManager(private val context: Context) {

    companion object {
        private const val TAG = "BluetoothManager"
        const val PERMISSION_REQUEST_CODE = 1002

        // 服务标识与 [com.example.wifidirect.transfer.Constants.BLUETOOTH_APP_UUID] 保持一致。
        // 历史 bug：以前这里硬编码 UUID 与 App.getBluetoothUuid() 生成随机 UUID 同时存在；
        // 后者从未被调用且会被误用导致两端永不握手。现在统一从 Constants 取。
        val APP_UUID: UUID = com.example.wifidirect.transfer.Constants.BLUETOOTH_APP_UUID
        const val APP_NAME = com.example.wifidirect.transfer.Constants.BLUETOOTH_APP_NAME
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var receiver: BroadcastReceiver? = null
    private var discoveryReceiver: BroadcastReceiver? = null
    private var serverSocket: android.bluetooth.BluetoothServerSocket? = null
    private var runningServer = false

    private val systemBluetoothManager: android.bluetooth.BluetoothManager? =
        context.getSystemService(android.bluetooth.BluetoothManager::class.java)
    private val adapter: BluetoothAdapter? = systemBluetoothManager?.adapter

    private val _devices = MutableStateFlow<List<BluetoothDevice>>(emptyList())
    val discoveredDevices: StateFlow<List<BluetoothDevice>> = _devices

    private val _pairedDevices = MutableStateFlow<List<BluetoothDevice>>(emptyList())
    val pairedDevices: StateFlow<List<BluetoothDevice>> = _pairedDevices

    private val _isDiscovering = MutableStateFlow(false)
    val isDiscovering: StateFlow<Boolean> = _isDiscovering

    private val _statusMessage = MutableStateFlow("")
    val statusMessage: StateFlow<String> = _statusMessage

    private val _enabled = MutableStateFlow(false)
    val enabled: StateFlow<Boolean> = _enabled

    val isSupported: Boolean
        get() = adapter != null

    @get:RequiresPermission(Manifest.permission.BLUETOOTH_CONNECT)
    val adapterName: String
        get() = if (hasPermissions()) {
            try { adapter?.name ?: "Unknown" } catch (e: SecurityException) { "Unknown" }
        } else "Unknown"

    private val _incomingSocket = MutableStateFlow<BluetoothSocket?>(null)
    val incomingSocket: StateFlow<BluetoothSocket?> = _incomingSocket

    init {
        _enabled.value = adapter?.isEnabled == true
        registerStateReceiver()
    }

    /**
     * 启用蓝牙：发送一个 ACTION_REQUEST_ENABLE Intent 给系统，由 UI 通过
     * `ActivityResultContracts.StartActivityForResult` 处理回调。
     *
     * 旧版本里这里有一个 `enableIfPossible(activity)` 直接 `startActivityForResult`
     * 的同步版本，但调用方已经迁到 ActivityResult API 了，这里只保留 Intent 构造。
     */
    fun buildEnableIntent(): Intent = Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE)

    private fun registerStateReceiver() {
        if (receiver != null) return
        val filter = IntentFilter().apply {
            addAction(BluetoothAdapter.ACTION_STATE_CHANGED)
            addAction(BluetoothDevice.ACTION_FOUND)
            addAction(BluetoothAdapter.ACTION_DISCOVERY_STARTED)
            addAction(BluetoothAdapter.ACTION_DISCOVERY_FINISHED)
            addAction(BluetoothDevice.ACTION_BOND_STATE_CHANGED)
        }
        receiver = object : BroadcastReceiver() {
            override fun onReceive(ctx: Context, intent: Intent) {
                when (intent.action) {
                    BluetoothAdapter.ACTION_STATE_CHANGED -> {
                        val state = intent.getIntExtra(
                            BluetoothAdapter.EXTRA_STATE, BluetoothAdapter.ERROR
                        )
                        _enabled.value = state == BluetoothAdapter.STATE_ON
                    }

                    BluetoothAdapter.ACTION_DISCOVERY_STARTED -> {
                        _isDiscovering.value = true
                        _statusMessage.value = "Bluetooth discovery started"
                    }

                    BluetoothAdapter.ACTION_DISCOVERY_FINISHED -> {
                        _isDiscovering.value = false
                        _statusMessage.value = "Bluetooth discovery finished"
                    }

                    BluetoothDevice.ACTION_FOUND -> {
                        val device = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                            intent.getParcelableExtra(
                                BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java
                            )
                        } else {
                            @Suppress("DEPRECATION")
                            intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE)
                        }
                        device?.let {
                            val current = _devices.value.toMutableList()
                            if (current.none { d -> d.address == it.address }) {
                                current.add(it)
                                _devices.value = current
                            }
                        }
                    }

                    BluetoothDevice.ACTION_BOND_STATE_CHANGED -> {
                        refreshPaired()
                    }
                }
            }
        }
        ContextCompat.registerReceiver(
            context, receiver!!, filter, ContextCompat.RECEIVER_NOT_EXPORTED
        )
    }

    fun startDiscovery(activity: Activity) {
        if (!hasPermissions()) {
            requestPermissions(activity)
            return
        }
        val a = adapter ?: run {
            _statusMessage.value = "Bluetooth unavailable"
            return
        }
        if (!a.isEnabled) {
            _statusMessage.value = "Enable Bluetooth first"
            return
        }
        refreshPaired()
        try {
            // cancel any ongoing discovery first
            a.cancelDiscovery()
        } catch (e: SecurityException) {
            Log.w(TAG, "cancel discovery denied", e)
        }
        try {
            val ok = a.startDiscovery()
            _statusMessage.value = if (ok) "Discovery started" else "Discovery failed to start"
        } catch (e: SecurityException) {
            _statusMessage.value = "Discovery denied: ${e.message}"
        }
    }

    @Suppress("DEPRECATION")
    private fun refreshPaired() {
        if (!hasPermissions()) return
        try {
            val bonded = adapter?.bondedDevices?.toList() ?: emptyList()
            _pairedDevices.value = bonded
        } catch (e: SecurityException) {
            Log.w(TAG, "Cannot read bonded devices", e)
        }
    }

    fun connectToDevice(device: BluetoothDevice, onResult: (BluetoothSocket?) -> Unit) {
        scope.launch {
            if (!hasPermissions()) {
                _statusMessage.value = "Missing Bluetooth permissions"
                return@launch
            }
            try {
                adapter?.cancelDiscovery()
                val socket: BluetoothSocket? = device.createRfcommSocketToServiceRecord(APP_UUID)
                try {
                    socket?.connect()
                    _statusMessage.value = "Bluetooth connected to ${device.safeName()}"
                    onResult(socket)
                } catch (e: IOException) {
                    Log.w(TAG, "connect() failed", e)
                    try { socket?.close() } catch (_: IOException) {}
                    _statusMessage.value = "Bluetooth connect failed: ${e.message}"
                    onResult(null)
                }
            } catch (e: SecurityException) {
                Log.w(TAG, "create socket denied", e)
                _statusMessage.value = "Cannot create Bluetooth socket"
                onResult(null)
            }
        }
    }

    /**
     * Start an RFCOMM server that listens for incoming connections continuously.
     * Each accepted connection is handed to [onAccepted] asynchronously (this
     * method spawns its own coroutine per connection so the accept loop is not
     * blocked — the old implementation called `stopServer()` after the first
     * accept, forcing the user to re-tap "Listen" for every transfer).
     */
    fun startServer(onAccepted: (BluetoothSocket) -> Unit) {
        if (runningServer) return
        runningServer = true
        scope.launch {
            if (!hasPermissions()) {
                _statusMessage.value = "Cannot start BT server: permissions missing"
                runningServer = false
                return@launch
            }
            var localServer: BluetoothServerSocket? = null
            try {
                localServer = adapter?.listenUsingRfcommWithServiceRecord(APP_NAME, APP_UUID)
            } catch (e: IOException) {
                Log.w(TAG, "listen failed", e)
                _statusMessage.value = "Cannot listen on Bluetooth: ${e.message}"
                runningServer = false
                return@launch
            } catch (e: SecurityException) {
                Log.w(TAG, "listen denied", e)
                _statusMessage.value = "Cannot listen on Bluetooth: ${e.message}"
                runningServer = false
                return@launch
            }
            serverSocket = localServer
            _statusMessage.value = "Bluetooth server listening..."
            while (runningServer) {
                val accepted = try {
                    localServer?.accept()
                } catch (e: IOException) {
                    if (runningServer) {
                        // 单次 accept 异常不应该让整个服务退出。短暂 sleep 后继续。
                        Log.w(TAG, "accept failed (will retry)", e)
                        kotlinx.coroutines.delay(500); continue
                    }
                    null
                } catch (e: SecurityException) {
                    Log.w(TAG, "accept denied", e); null
                }
                if (accepted == null) continue
                _statusMessage.value =
                    "Incoming Bluetooth connection from ${accepted.remoteDevice?.safeName()}"
                _incomingSocket.value = accepted
                // 把对端连接交给调用方处理，循环立刻继续接受下一个连接。
                scope.launch { onAccepted(accepted) }
            }
        }
    }

    fun stopServer() {
        runningServer = false
        try { serverSocket?.close() } catch (e: IOException) { Log.w(TAG, "server close", e) }
        serverSocket = null
        _incomingSocket.value = null   // 清空之前忘记清的状态
    }

    fun clearDiscovered() {
        _devices.value = emptyList()
    }

    private fun hasPermissions(): Boolean {
        val needed = permissionsForSdk()
        return needed.all {
            ContextCompat.checkSelfPermission(context, it) == PackageManager.PERMISSION_GRANTED
        }
    }

    private fun requestPermissions(activity: Activity) {
        ActivityCompat.requestPermissions(
            activity, permissionsForSdk().toTypedArray(), PERMISSION_REQUEST_CODE
        )
    }

    private fun permissionsForSdk(): List<String> = when {
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.S ->
            listOf(
                Manifest.permission.BLUETOOTH_SCAN,
                Manifest.permission.BLUETOOTH_CONNECT,
                Manifest.permission.BLUETOOTH_ADVERTISE
            )
        else ->
            listOf(
                Manifest.permission.BLUETOOTH,
                Manifest.permission.BLUETOOTH_ADMIN
            )
    }

    fun cleanup() {
        stopServer()
        receiver?.let {
            try { context.unregisterReceiver(it) } catch (e: IllegalArgumentException) {
                Log.w(TAG, "receiver not registered", e)
            }
        }
        receiver = null
        _incomingSocket.value = null   // 修复旧版本不在此清空的遗漏
        try { adapter?.cancelDiscovery() } catch (e: SecurityException) {
            Log.w(TAG, "cancel discovery denied", e)
        }
        scope.cancel()
    }
}

fun BluetoothDevice.safeName(): String {
    return try {
        name ?: "Unknown"
    } catch (e: SecurityException) {
        "Unknown"
    }
}
