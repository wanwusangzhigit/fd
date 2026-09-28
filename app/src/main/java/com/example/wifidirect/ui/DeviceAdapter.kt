package com.example.wifidirect.ui

import android.bluetooth.BluetoothDevice
import android.net.wifi.p2p.WifiP2pDevice
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.TextView
import androidx.recyclerview.widget.DiffUtil
import androidx.recyclerview.widget.ListAdapter
import androidx.recyclerview.widget.RecyclerView
import com.example.wifidirect.R
import com.example.wifidirect.manager.safeName

/**
 * 统一的设备适配器，可同时显示 Wi-Fi Direct peers 和 Bluetooth discovered devices。
 *
 * 关键修复（相比旧版）：
 * - [DeviceItem] 从嵌套类提到顶层 —— 旧版 Kotlin 解析顺序导致 `ListAdapter<DeviceItem, VH>`
 *   在主构造里无法解析到 `DeviceItem`。
 * - [DIFF] 的 `areContentsTheSame` 真正比较名称 / 地址 / 状态，而不是简单地返回 id 相等
 *   （旧版永远不重绘，所以连接状态变化用户看不到）。
 */
sealed class DeviceItem {
    abstract val stableId: String
    abstract fun contentIdentity(): String

    data class Wifi(val device: WifiP2pDevice) : DeviceItem() {
        override val stableId: String get() = "wifi:${device.deviceAddress}"
        override fun contentIdentity(): String =
            "wifi|${device.deviceAddress}|${device.deviceName}|${device.status}"
    }
    data class Bt(val device: BluetoothDevice) : DeviceItem() {
        override val stableId: String get() = "bt:${device.address}"
        override fun contentIdentity(): String =
            "bt|${device.address}|${device.safeName()}|${try { device.bondState } catch(_: SecurityException) { -1 }}"
    }
}

class DeviceAdapter(
    private val onClick: (DeviceItem) -> Unit
) : ListAdapter<DeviceItem, DeviceAdapter.VH>(DIFF) {

    class VH(view: View) : RecyclerView.ViewHolder(view) {
        val name: TextView = view.findViewById(R.id.deviceName)
        val detail: TextView = view.findViewById(R.id.deviceDetail)
        val transport: TextView = view.findViewById(R.id.deviceTransport)
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): VH {
        val v = LayoutInflater.from(parent.context).inflate(R.layout.item_device, parent, false)
        return VH(v)
    }

    override fun onBindViewHolder(holder: VH, position: Int) {
        val item = getItem(position)
        val ctx = holder.itemView.context
        when (item) {
            is DeviceItem.Wifi -> {
                val d = item.device
                holder.name.text = d.deviceName.ifEmpty { ctx.getString(R.string.unnamed_device) }
                holder.detail.text = d.deviceAddress
                holder.transport.text = ctx.getString(R.string.transport_wfd)
            }
            is DeviceItem.Bt -> {
                val d = item.device
                holder.name.text = d.safeName().ifEmpty { d.address }
                holder.detail.text = d.address
                holder.transport.text = ctx.getString(R.string.transport_bt)
            }
        }
        holder.itemView.setOnClickListener { onClick(item) }
    }

    companion object {
        val DIFF = object : DiffUtil.ItemCallback<DeviceItem>() {
            override fun areItemsTheSame(o: DeviceItem, n: DeviceItem) = o.stableId == n.stableId
            override fun areContentsTheSame(o: DeviceItem, n: DeviceItem) =
                o.contentIdentity() == n.contentIdentity()
        }
    }
}
