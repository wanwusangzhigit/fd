package com.example.wifidirect.transfer

/**
 * 所有跨模块共享的常量集中在这里，避免 magic number 散落各处。
 *
 * 字段命名约定：<transport>_<purpose>_<unit>
 */
object Constants {

    /** TCP 端口（Wi-Fi Direct）— 文件数据通道 */
    const val WIFI_FILE_PORT = 8988

    /** TCP 端口（Wi-Fi Direct）— 客户端上报自身 IP 的信令通道（组主侧监听） */
    const val WIFI_REGISTER_PORT = 8989

    /** TCP 连接超时（ms） */
    const val WIFI_CONNECT_TIMEOUT_MS = 15_000

    /** Wi-Fi Direct `connect()` 调用后等待 `WIFI_P2P_CONNECTION_CHANGED_ACTION` 的最长时长 */
    const val WFD_CONNECT_TIMEOUT_MS: Long = 30_000L

    /** 蓝牙 RFCOMM 服务 UUID（两台机器必须一致） */
    val BLUETOOTH_APP_UUID: java.util.UUID =
        java.util.UUID.fromString("8e7a1c5a-2f44-4f7c-b76b-9b8f4b9f0001")

    /** 蓝牙 SDP 服务名 */
    const val BLUETOOTH_APP_NAME = "WiFiDirectFileTransfer_BT"

    /** 蓝牙 RFCOMM 通道单次发送的缓冲区大小（BT MTU 较小，用小缓冲更高效） */
    const val BUFFER_SIZE_BT = 16 * 1024

    /** Wi-Fi Direct / TCP 通道单次发送的缓冲区大小 */
    const val BUFFER_SIZE_TCP = 64 * 1024

    /** TCP 进度汇报的最小字节间隔 */
    const val PROGRESS_INTERVAL_BYTES_TCP = 256 * 1024

    /** 蓝牙进度汇报的最小字节间隔 */
    const val PROGRESS_INTERVAL_BYTES_BT = 64 * 1024

    /** 接收端写盘的缓冲区大小 */
    const val BUFFER_SIZE_RECEIVE = 64 * 1024

    /** 通知渠道 ID */
    const val NOTIFICATION_CHANNEL_ID = "file_transfer"

    /** 前台服务通知 ID */
    const val NOTIFICATION_ID = 7777

    /** 接收文件保存目录名（公共 Downloads / app-specific 外存下都使用这个名字） */
    const val RECEIVED_DIR_NAME = "WiFiDirectFileTransfer"
}
