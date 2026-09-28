package com.example.wifidirect.ui

import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.recyclerview.widget.DiffUtil
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.ListAdapter
import androidx.recyclerview.widget.RecyclerView
import com.example.wifidirect.R
import com.example.wifidirect.transfer.ReceivedFileManager
import com.example.wifidirect.transfer.TransferFormatter
import com.example.wifidirect.transfer.TransferHistory

/**
 * 显示历史传输列表（接收 / 发送）。点击一行可以打开已接收的文件（如有 savedPath）。
 *
 * 这是简单实现 —— 用一个独立 Activity 而非 Fragment / BottomSheet，方便快速迭代。
 */
class HistoryActivity : AppCompatActivity() {

    private lateinit var adapter: HistoryAdapter

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        title = getString(R.string.history_title)
        setContentView(R.layout.activity_history)
        supportActionBar?.setDisplayHomeAsUpEnabled(true)

        val recyclerView = findViewById<androidx.recyclerview.widget.RecyclerView>(R.id.historyList)
        adapter = HistoryAdapter(::onItemClick)
        recyclerView.layoutManager = LinearLayoutManager(this)
        recyclerView.adapter = adapter

        loadHistory()
    }

    private fun loadHistory() {
        val records = TransferHistory.get().all()
        adapter.submitList(records)
        findViewById<TextView>(R.id.historyEmpty).visibility =
            if (records.isEmpty()) View.VISIBLE else View.GONE
    }

    private fun onItemClick(record: TransferHistory.Record) {
        val path = record.savedPath ?: return
        try {
            val intent = ReceivedFileManager(this).openFileIntent(path, record.mimeType)
            startActivity(intent)
        } catch (e: Exception) {
            android.widget.Toast.makeText(this, R.string.open_failed,
                android.widget.Toast.LENGTH_SHORT).show()
        }
    }

    override fun onOptionsItemSelected(item: android.view.MenuItem): Boolean {
        if (item.itemId == android.R.id.home) { finish(); return true }
        return super.onOptionsItemSelected(item)
    }

    // ------- Adapter -------

    private class HistoryAdapter(private val onClick: (TransferHistory.Record) -> Unit) :
        ListAdapter<TransferHistory.Record, HistoryAdapter.VH>(DIFF) {

        class VH(view: View) : RecyclerView.ViewHolder(view) {
            val title: TextView = view.findViewById(R.id.historyItemTitle)
            val subtitle: TextView = view.findViewById(R.id.historyItemSubtitle)
        }

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): VH {
            val v = LayoutInflater.from(parent.context).inflate(R.layout.item_history, parent, false)
            return VH(v)
        }

        override fun onBindViewHolder(holder: VH, position: Int) {
            val r = getItem(position)
            val ctx = holder.itemView.context
            val dirArrow = if (r.direction == TransferHistory.Direction.RECEIVING) "↓" else "↑"
            holder.title.text = "$dirArrow ${r.fileName}"
            val transportLabel = if (r.transport == com.example.wifidirect.transfer.Transport.WIFI_DIRECT)
                ctx.getString(R.string.transport_wfd) else ctx.getString(R.string.transport_bt)
            holder.subtitle.text = "${r.timestamp} · $transportLabel · ${TransferFormatter.humanSize(r.size)}"
            holder.itemView.setOnClickListener { onClick(r) }
        }

        companion object {
            val DIFF = object : DiffUtil.ItemCallback<TransferHistory.Record>() {
                override fun areItemsTheSame(o: TransferHistory.Record, n: TransferHistory.Record) =
                    o.timestamp == n.timestamp && o.fileName == n.fileName
                override fun areContentsTheSame(o: TransferHistory.Record, n: TransferHistory.Record) = o == n
            }
        }
    }
}
