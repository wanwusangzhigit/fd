package com.example.wifidirect.ui

import android.app.Activity
import android.bluetooth.BluetoothDevice
import android.content.Intent
import android.net.Uri
import android.net.wifi.p2p.WifiP2pDevice
import android.net.wifi.p2p.WifiP2pInfo
import android.os.Build
import android.os.Bundle
import android.provider.OpenableColumns
import android.util.Log
import android.view.View
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.example.wifidirect.App
import com.example.wifidirect.R
import com.example.wifidirect.databinding.ActivityMainBinding
import com.example.wifidirect.manager.BluetoothManager
import com.example.wifidirect.manager.WiFiDirectManager
import com.example.wifidirect.manager.safeName
import com.example.wifidirect.transfer.BluetoothReceiverConnection
import com.example.wifidirect.transfer.BluetoothSenderConnection
import com.example.wifidirect.transfer.Constants
import com.example.wifidirect.transfer.FileTransferService
import com.example.wifidirect.transfer.PeerRegistry
import com.example.wifidirect.transfer.ReceivedFileManager
import com.example.wifidirect.transfer.TransferBus
import com.example.wifidirect.transfer.TransferEvent
import com.example.wifidirect.transfer.TransferFormatter
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.filterNotNull
import kotlinx.coroutines.launch

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private lateinit var wifi: WiFiDirectManager
    private lateinit var bt: BluetoothManager
    private lateinit var receivedFileManager: ReceivedFileManager

    private val deviceAdapter = DeviceAdapter(::onDeviceClicked)
    private val transferAdapter = TransferAdapter(::onOpenReceivedClicked, ::onCancelTransferClicked)

    /** 一份待发送文件。配合 SavedState 旋转屏后保留。 */
    data class QueuedFile(val uri: Uri, val displayName: String, val size: Long)
    private val pendingFiles = mutableListOf<QueuedFile>()

    /** 维护每个 transfer id 的瞬时速率采样器。 */
    private val speedTracker = com.example.wifidirect.transfer.SpeedTracker

    /** 当前选中的待发送文件列表。配合 [SavedState] 旋转后保留。多文件选择通过
     *  [pickFiles] 实现；用户每次点「Pick」会追加而不是替换。 */
    // 已在上面声明 pendingFiles

    /** 组主侧发起反向发送时缓存的「待连接的 peer MAC + 待发文件」，等待 IP 上报后发送。 */
    private var pendingOwnerSendMac: String? = null
    private var pendingOwnerSendFiles: List<QueuedFile> = emptyList()

    /** 客户端→组主方向的 pending：用户在尚未连上组主时点了 peer，连上后自动 flush。 */
    private var pendingClientSendFiles: List<QueuedFile> = emptyList()

    /** 已连接的蓝牙发送端 socket 连接（独占持有，杜绝多处 close）。 */
    private var bluetoothSenderConnection: BluetoothSenderConnection? = null

    /** 已连接的蓝牙接收端 socket（保持引用避免被 GC）。 */
    private var bluetoothReceiverConnection: BluetoothReceiverConnection? = null

    /** 标志：是否已经启动过蓝牙 RFCOMM 服务端（旧代码每接收一次就停止）。 */
    private var bluetoothServerStarted = false

    // ----- 文件选择 ---------------------------------------------------------

    private val pickFiles = registerForActivityResult(
        ActivityResultContracts.GetMultipleContents()
    ) { uris: List<Uri> ->
        if (uris.isEmpty()) {
            toast(getString(R.string.no_file_selected))
            return@registerForActivityResult
        }
        val added = mutableListOf<QueuedFile>()
        for (uri in uris) {
            try {
                // 持久化读权限：旋转 / Activity 被回收后 Service 端 openInputStream 仍能成功
                contentResolver.takePersistableUriPermission(
                    uri,
                    Intent.FLAG_GRANT_READ_URI_PERMISSION
                )
            } catch (e: SecurityException) {
                Log.w(TAG, "takePersistableUriPermission failed for $uri: ${e.message}")
            }
            val (name, size) = queryFileMeta(uri)
            added += QueuedFile(uri, name, size)
        }
        if (added.isEmpty()) {
            toast(getString(R.string.no_file_selected))
            return@registerForActivityResult
        }
        pendingFiles += added
        refreshSelectedFileLabel()
        toast(resources.getQuantityString(
            R.plurals.toast_files_selected, added.size, added.size
        ))
    }

    /** 启用蓝牙走 ActivityResult API（取代旧的 startActivityForResult + onActivityResult）。 */
    private val enableBtLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        if (result.resultCode == Activity.RESULT_OK) {
            toast(getString(R.string.bt_enabled))
        } else {
            toast(getString(R.string.bt_still_disabled))
        }
    }

    /** 申请蓝牙权限走 ActivityResult API（替代 requestPermissions + onRequestPermissionsResult）。 */
    private val requestBtPermission = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { result ->
        val denied = result.any { !it.value }
        if (denied) toast(getString(R.string.bt_perm_denied))
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        wifi = WiFiDirectManager(this)
        bt = BluetoothManager(this)
        receivedFileManager = ReceivedFileManager(this)
        // 初始化传输历史单例（接收事件会调用 TransferHistory.get()）
        com.example.wifidirect.transfer.TransferHistory.init(applicationContext)

        setupRecyclerViews()
        setupButtons(savedInstanceState)
        setupSwipeRefresh()
        observeManagers()
        observeTransferEvents()
        restoreState(savedInstanceState)

        ContextCompat.startForegroundService(this,
            Intent(this, FileTransferService::class.java).apply {
                action = FileTransferService.ACTION_START_SERVER
            })
        // 自动启动蓝牙服务监听（不需要用户手动点击）
        autoStartBluetoothServer()
    }

    private fun setupRecyclerViews() {
        binding.deviceList.layoutManager = LinearLayoutManager(this)
        binding.deviceList.adapter = deviceAdapter

        binding.transferList.layoutManager = LinearLayoutManager(this)
        binding.transferList.adapter = transferAdapter

        // 给传输列表加滑动删除：仅允许已完成 / 失败 / 取消的行被滑走。
        val swipeCallback = object : androidx.recyclerview.widget.ItemTouchHelper.SimpleCallback(
            0,
            androidx.recyclerview.widget.ItemTouchHelper.LEFT or androidx.recyclerview.widget.ItemTouchHelper.RIGHT
        ) {
            override fun onMove(
                rv: RecyclerView,
                vh: RecyclerView.ViewHolder,
                target: RecyclerView.ViewHolder
            ): Boolean = false

            override fun getSwipeDirs(
                rv: RecyclerView,
                vh: RecyclerView.ViewHolder
            ): Int {
                // 通过 adapter 取出行，仅在终态时返回允许滑动的方向
                val pos = vh.bindingAdapterPosition
                if (pos < 0) return 0
                val row = transferAdapter.currentList.getOrNull(pos) ?: return 0
                return if (row.done || row.failed || row.canceled) super.getSwipeDirs(rv, vh) else 0
            }

            override fun onSwiped(viewHolder: RecyclerView.ViewHolder, direction: Int) {
                val pos = viewHolder.bindingAdapterPosition
                if (pos < 0) return
                transferAdapter.removeAt(pos)
            }
        }
        androidx.recyclerview.widget.ItemTouchHelper(swipeCallback)
            .attachToRecyclerView(binding.transferList)
    }

    private fun setupButtons(savedInstanceState: Bundle?) {
        // 旧逻辑：「Discover Wi-Fi Direct」时如果 initialize 失败反而又调用了 discoverPeers，
        // 语义完全反了。这里改成「初始化失败 → return」。
        binding.btnDiscoverWifi.setOnClickListener {
            if (!wifi.isP2pSupported) {
                toast(getString(R.string.wifi_p2p_unsupported)); return@setOnClickListener
            }
            wifi.initialize(this)        // 会自行申请权限并注册 receiver
            @Suppress("MissingPermission")  // 内部已 hasPermissions() 检查
            wifi.discoverPeers()
        }

        binding.btnDiscoverBt.setOnClickListener {
            if (!bt.isSupported) {
                toast(getString(R.string.bt_unsupported)); return@setOnClickListener
            }
            // 旧逻辑：未开启蓝牙时点了按钮先异步弹系统对话框，紧接着同步 startDiscovery 立刻失败。
            // 这里改成：若蓝牙未开启则只触发启用流程，等收到 enabled=true 再调用 startDiscovery。
            if (!bt.enabled.value) {
                val enable = Intent(android.bluetooth.BluetoothAdapter.ACTION_REQUEST_ENABLE)
                enableBtLauncher.launch(enable)
                // 启用成功后通过 enabled.collectLatest 自动启动发现
                return@setOnClickListener
            }
            ensureBtPermissionThenScan()
        }

        binding.btnStartBtServer.setOnClickListener {
            ensureBtServerStarted()
        }

        binding.btnStopWifi.setOnClickListener {
            wifi.disconnectFromGroup()
            // 重置所有 pending 发送
            pendingClientSendFiles = emptyList()
            pendingOwnerSendFiles = emptyList()
            pendingOwnerSendMac = null
        }

        binding.btnPickFile.setOnClickListener {
            // GetMultipleContents 由系统 picker 实现，无需任何存储权限。
            pickFiles.launch("*/*")
        }

        binding.btnHistory.setOnClickListener {
            startActivity(Intent(this, HistoryActivity::class.java))
        }
    }

    private fun setupSwipeRefresh() {
        binding.swipeRefresh.setOnRefreshListener {
            // 同时触发 Wi-Fi Direct + 蓝牙发现
            if (wifi.isP2pSupported && wifi.initialize(this)) {
                @Suppress("MissingPermission")  // 内部已检查
                wifi.discoverPeers()
            }
            if (bt.isSupported && bt.enabled.value && hasBluetoothPermission()) {
                bt.clearDiscovered()
                bt.startDiscovery(this)
            }
            // 给一点延迟隐藏刷新动画
            binding.swipeRefresh.postDelayed({
                binding.swipeRefresh.isRefreshing = false
            }, 1500)
        }
    }

    private fun observeManagers() {
        // Wi-Fi Direct peers
        lifecycleScope.launch {
            wifi.peers.collectLatest { peers ->
                refreshDeviceList(peers = peers, bluetooth = bt.discoveredDevices.value)
            }
        }
        // Bluetooth discovered + paired
        lifecycleScope.launch {
            bt.discoveredDevices.collectLatest { devices ->
                refreshDeviceList(peers = wifi.peers.value, bluetooth = devices)
            }
        }
        lifecycleScope.launch {
            bt.pairedDevices.collectLatest { devices ->
                refreshDeviceList(peers = wifi.peers.value, bluetooth = devices)
            }
        }
        // 启用蓝牙后自动扫描
        lifecycleScope.launch {
            bt.enabled.collectLatest { enabled ->
                if (enabled) {
                    binding.statusBt.text = getString(R.string.bt_enabled_status)
                    // 启用成功后立即扫描一次，并启动服务监听
                    ensureBtServerStarted()
                    if (hasBluetoothPermission()) bt.startDiscovery(this@MainActivity)
                } else {
                    binding.statusBt.text = getString(R.string.bt_disabled_status)
                }
            }
        }
        // Wi-Fi Direct 状态消息
        lifecycleScope.launch {
            wifi.statusMessage.collectLatest { msg ->
                if (msg.isNotEmpty()) binding.statusWifi.text = "Wi-Fi Direct: $msg"
            }
        }
        // 蓝牙状态消息
        lifecycleScope.launch {
            bt.statusMessage.collectLatest { msg ->
                if (msg.isNotEmpty()) binding.statusBt.text = "Bluetooth: $msg"
            }
        }
        // Wi-Fi 连接状态变化 —— 自动上报 IP / 自动 flush 待发送
        lifecycleScope.launch {
            wifi.connectionInfo.collectLatest { info: WifiP2pInfo? ->
                if (info != null && info.groupFormed) {
                    if (info.isGroupOwner) {
                        binding.statusWifi.text = getString(R.string.wfd_group_owner, info.groupOwnerAddress)
                        // 组主不需要上报；只清空 pending（如果有客户端方向的待发送走下面）
                    } else {
                        binding.statusWifi.text = getString(R.string.wfd_client, info.groupOwnerAddress)
                        // 客户端：把自己的 IP 上报给组主（用于组主反向发送）
                        val host = info.groupOwnerAddress?.hostAddress
                        val myIp = PeerRegistry.localWifiDirectIp()
                        if (host != null && myIp != null) {
                            PeerRegistry.reportToOwner(host, myIp)
                        }
                        // 客户端方向：如果用户在连接成功前已选好文件等待，现在 flush
                        if (pendingClientSendFiles.isNotEmpty()) {
                            val h = info.groupOwnerAddress?.hostAddress
                            if (h != null) {
                                launchWifiSendBatch(h, pendingClientSendFiles)
                                pendingClientSendFiles = emptyList()
                            }
                        }
                    }
                    binding.btnStopWifi.visibility = View.VISIBLE
                } else {
                    binding.btnStopWifi.visibility = View.GONE
                }
            }
        }
        // 组主侧收到客户端上报 IP 时，如果有 pending 反向发送，立即触发
        lifecycleScope.launch {
            TransferBus.critical.collect { event ->
                if (event is TransferEvent.PeerRegistered) {
                    if (pendingOwnerSendMac != null && pendingOwnerSendFiles.isNotEmpty()) {
                        launchWifiSendBatch(event.ip, pendingOwnerSendFiles)
                        pendingOwnerSendFiles = emptyList()
                        pendingOwnerSendMac = null
                    }
                }
            }
        }
    }

    private fun observeTransferEvents() {
        lifecycleScope.launch {
            TransferBus.critical.collect { event ->
                transferAdapter.applyEvent(event)
                if (event is TransferEvent.FileReceived) {
                    toast(getString(R.string.toast_received, event.fileName))
                }
            }
        }
        lifecycleScope.launch {
            TransferBus.progress.collect { progMap ->
                progMap.forEach { (id, ev) ->
                    val decorated = speedTracker.tick(id, ev.transferred, ev.total)
                    transferAdapter.applyEvent(ev.copy(
                        transferred = ev.transferred,
                        total = ev.total
                    ))
                    // 把速率作为「家饰」单独设置进适配器
                    transferAdapter.setSpeed(id, decorated.bytesPerSec, decorated.etaSeconds)
                }
            }
        }
    }

    private fun refreshDeviceList(
        peers: List<WifiP2pDevice>,
        bluetooth: List<BluetoothDevice>
    ) {
        val items = mutableListOf<DeviceItem>()
        peers.forEach { items += DeviceItem.Wifi(it) }
        bluetooth.forEach { items += DeviceItem.Bt(it) }
        deviceAdapter.submitList(items)
        binding.emptyView.visibility = if (items.isEmpty()) View.VISIBLE else View.GONE
    }

    // ----- 设备点击：选择发送通道 -------------------------------------------

    private fun onDeviceClicked(item: DeviceItem) {
        if (pendingFiles.isEmpty()) {
            toast(getString(R.string.pick_file_first))
            return
        }
        when (item) {
            is DeviceItem.Wifi -> onWifiDeviceClicked(item, pendingFiles.toList())
            is DeviceItem.Bt -> onBtDeviceClicked(item.device, pendingFiles.toList())
        }
    }

    private fun onWifiDeviceClicked(item: DeviceItem.Wifi, files: List<QueuedFile>) {
        val info = wifi.connectionInfo.value
        when {
            info != null && info.groupFormed && !info.isGroupOwner -> {
                val host = info.groupOwnerAddress?.hostAddress
                if (host == null) { toast(getString(R.string.no_owner_addr)); return }
                launchWifiSendBatch(host, files)
            }
            info != null && info.groupFormed && info.isGroupOwner -> {
                val peerIp = PeerRegistry.ipOf(item.device.deviceAddress)
                if (peerIp != null) {
                    launchWifiSendBatch(peerIp, files)
                } else {
                    pendingOwnerSendMac = item.device.deviceAddress
                    pendingOwnerSendFiles = files
                    toast(getString(R.string.waiting_peer_ip))
                }
            }
            else -> {
                toast(getString(R.string.connecting_to, item.device.deviceName))
                @Suppress("MissingPermission")  // 内部已 hasPermissions() 检查
                wifi.connect(item.device)
                pendingClientSendFiles = files
            }
        }
    }

    private fun onBtDeviceClicked(device: BluetoothDevice, files: List<QueuedFile>) {
        if (!hasBluetoothPermission()) {
            requestBtPermission.launch(btPermissionArray())
            return
        }
        toast(getString(R.string.connecting_bt, device.safeName()))
        bluetoothSenderConnection?.close()
        bt.connectToDevice(device) { socket ->
            if (socket == null) {
                runOnUiThreadSafely { toast(getString(R.string.bt_connect_failed)) }
            } else {
                val conn = BluetoothSenderConnection(socket)
                bluetoothSenderConnection = conn
                // 在同一 socket 上依次发送多个文件；BluetoothSenderConnection 内部
                // 用独立的 Job 维护每个 transfer，但 socket 共享，发送顺序天然串行。
                files.forEach { f -> conn.sendFile(f.uri, f.displayName, f.size, contentResolver) }
                runOnUiThreadSafely { clearPendingFiles() }
            }
        }
    }

    /** 批量通过 Wi-Fi Direct 发送：每文件独立 startService → 多 TCP 连接并行。 */
    private fun launchWifiSendBatch(host: String, files: List<QueuedFile>) {
        files.forEach { f -> launchWifiSend(host, f.uri, f.displayName, f.size) }
        clearPendingFiles()
        if (files.size > 1) {
            toast(resources.getQuantityString(
                R.plurals.toast_sending_via_wfd_batch, files.size, files.size
            ))
        } else if (files.size == 1) {
            toast(getString(R.string.toast_sending_via_wfd, files.first().displayName))
        }
    }

    /** 发送单个文件到指定 host（通过 Service 启动一次 TCP 发送）。 */
    private fun launchWifiSend(host: String, uri: Uri, name: String, size: Long) {
        val intent = Intent(this, FileTransferService::class.java).apply {
            action = FileTransferService.ACTION_SEND_WIFI
            putExtra(FileTransferService.EXTRA_HOST, host)
            putExtra(FileTransferService.EXTRA_PORT, Constants.WIFI_FILE_PORT)
            putExtra(FileTransferService.EXTRA_FILE_URI, uri)
            putExtra(FileTransferService.EXTRA_FILE_NAME, name)
            putExtra(FileTransferService.EXTRA_FILE_SIZE, size)
        }
        ContextCompat.startForegroundService(this, intent)
    }

    // ----- 已接收文件：打开 -------------------------------------------------

    private fun onOpenReceivedClicked(path: String, mimeType: String?) {
        try {
            val intent = receivedFileManager.openFileIntent(path, mimeType)
            startActivity(intent)
        } catch (e: Exception) {
            Log.w(TAG, "open received file failed", e)
            toast(getString(R.string.open_failed))
        }
    }

    private fun onCancelTransferClicked(id: String) {
        val ok = TransferBus.cancelTransfer(id)
        if (!ok) {
            // 可能是发送侧持有取消句柄
            startService(Intent(this, FileTransferService::class.java).apply {
                // 目前 service 没暴露专门的取消 action；UI 仅靠 TransferBus 取消句柄即可
            })
        }
        if (!ok) toast(getString(R.string.cancel_no_handle))
    }

    // ----- 蓝牙服务端 -------------------------------------------------------

    private fun autoStartBluetoothServer() {
        if (bluetoothServerStarted) return
        if (!bt.isSupported || !bt.enabled.value) return
        if (!hasBluetoothPermission()) return
        ensureBtServerStarted()
    }

    private fun ensureBtServerStarted() {
        if (bluetoothServerStarted) return
        if (!hasBluetoothPermission()) {
            requestBtPermission.launch(btPermissionArray())
            return
        }
        bluetoothServerStarted = true
        bt.startServer { socket ->
            // 用新的接收端抽象：在一个独立的 scope 里循环接收多个文件
            val conn = BluetoothReceiverConnection(socket, receivedFileManager)
            bluetoothReceiverConnection = conn
            conn.start()
        }
        toast(getString(R.string.bt_listening))
    }

    private fun btPermissionArray(): Array<String> =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) arrayOf(
            android.Manifest.permission.BLUETOOTH_SCAN,
            android.Manifest.permission.BLUETOOTH_CONNECT,
            android.Manifest.permission.BLUETOOTH_ADVERTISE
        ) else arrayOf(
            android.Manifest.permission.BLUETOOTH,
            android.Manifest.permission.BLUETOOTH_ADMIN
        )

    private fun ensureBtPermissionThenScan() {
        if (!hasBluetoothPermission()) {
            requestBtPermission.launch(btPermissionArray())
        } else {
            bt.clearDiscovered()
            bt.startDiscovery(this)
        }
    }

    // ----- 状态保存 ---------------------------------------------------------

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        // 把 pendingFiles 序列化成三个并行数组（Uri 字符串 / 名字 / 大小）
        if (pendingFiles.isNotEmpty()) {
            outState.putStringArrayList(KEY_SEND_URIS,
                ArrayList(pendingFiles.map { it.uri.toString() }))
            outState.putStringArrayList(KEY_SEND_NAMES,
                ArrayList(pendingFiles.map { it.displayName }))
            outState.putLongArray(KEY_SEND_SIZES,
                pendingFiles.map { it.size }.toLongArray())
        }
        outState.putBoolean(KEY_BT_SERVER_STARTED, bluetoothServerStarted)
    }

    private fun restoreState(savedInstanceState: Bundle?) {
        savedInstanceState ?: return
        val uris = savedInstanceState.getStringArrayList(KEY_SEND_URIS)
        val names = savedInstanceState.getStringArrayList(KEY_SEND_NAMES)
        val sizes = savedInstanceState.getLongArray(KEY_SEND_SIZES)
        if (uris != null && names != null && sizes != null && uris.size == names.size) {
            pendingFiles.clear()
            for (i in uris.indices) {
                pendingFiles += QueuedFile(
                    uri = Uri.parse(uris[i]),
                    displayName = names[i],
                    size = sizes.getOrElse(i) { -1L }
                )
            }
            refreshSelectedFileLabel()
        }
        bluetoothServerStarted = savedInstanceState.getBoolean(KEY_BT_SERVER_STARTED, false)
    }

    // ----- 辅助 ---------------------------------------------------------------

    private fun refreshSelectedFileLabel() {
        if (pendingFiles.isEmpty()) {
            binding.selectedFile.text = ""
            binding.sendHint.visibility = View.GONE
            return
        }
        val totalBytes = pendingFiles.sumOf { it.size }
        val namesPreview = pendingFiles.joinToString(", ") { it.displayName }
                                  .let { if (it.length > 60) it.substring(0, 57) + "…" else it }
        binding.selectedFile.text = resources.getQuantityString(
            R.plurals.selected_files_label,
            pendingFiles.size,
            pendingFiles.size,
            TransferFormatter.humanSize(totalBytes),
            namesPreview
        )
        binding.sendHint.visibility = View.VISIBLE
    }

    private fun clearPendingFiles() {
        pendingFiles.clear()
        refreshSelectedFileLabel()
    }

    // ----- 文件选择 END -----------------------------------------------------

    private fun queryFileMeta(uri: Uri): Pair<String, Long> {
        var name: String? = null
        var size = -1L
        contentResolver.query(uri, null, null, null, null)?.use { cursor ->
            val nameIdx = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
            val sizeIdx = cursor.getColumnIndex(OpenableColumns.SIZE)
            if (cursor.moveToFirst()) {
                if (nameIdx >= 0) name = cursor.getString(nameIdx)
                if (sizeIdx >= 0 && !cursor.isNull(sizeIdx)) size = cursor.getLong(sizeIdx)
            }
        }
        if (name.isNullOrBlank()) name = uri.lastPathSegment ?: "file"
        return name!! to size
    }

    private fun hasBluetoothPermission(): Boolean =
        btPermissionArray().all {
            ContextCompat.checkSelfPermission(this, it) ==
                android.content.pm.PackageManager.PERMISSION_GRANTED
        }

    private fun runOnUiThreadSafely(block: () -> Unit) {
        // 旧代码从 IO scope 直接 runOnUiThread 操作 binding，Activity destroy 后会崩。
        if (!isFinishing && !isDestroyed) runOnUiThread { if (!isFinishing && !isDestroyed) block() }
    }

    private fun toast(msg: String) = Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()

    override fun onDestroy() {
        super.onDestroy()
        try { bluetoothSenderConnection?.close() } catch (e: Exception) {
            Log.w(TAG, "close BT sender conn", e)
        }
        try { bluetoothReceiverConnection?.close() } catch (e: Exception) {
            Log.w(TAG, "close BT receiver conn", e)
        }
        bt.cleanup()
        wifi.cleanup()
    }

    companion object {
        private const val TAG = "MainActivity"
        private const val KEY_SEND_URIS = "pending_send_uris"
        private const val KEY_SEND_NAMES = "pending_send_names"
        private const val KEY_SEND_SIZES = "pending_send_sizes"
        private const val KEY_BT_SERVER_STARTED = "bt_server_started"
    }
}
