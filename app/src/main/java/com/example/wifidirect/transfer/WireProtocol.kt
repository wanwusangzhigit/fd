package com.example.wifidirect.transfer

import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream

/**
 * 二进制长度前缀协议（v2），取代旧的 `FILE <name> <size>` 文本协议。
 *
 * 旧的文本协议最大的问题：用空格作为分隔符，文件名包含空格时接收端解析
 * 出错（且解析出错只会静默 break，不会反馈到 UI）。
 *
 * 新协议帧格式（所有多字节整数都是 big-endian）：
 *
 *     MAGIC    : 4 bytes  "FDFT"
 *     VERSION  : 1 byte   = 2
 *     KIND     : 1 byte
 *     payload (KIND 决定)：
 *
 * KIND = 1（FILE 文件传输）
 *     nameLen  : 4 bytes (uint32)
 *     name     : nameLen bytes (UTF-8)
 *     size     : 8 bytes (int64)
 *     后续读取 size 字节的文件载荷
 *
 * KIND = 2（REGISTER_IP 客户端上报 IP）
 *     ipLen    : 1 byte
 *     ip       : ipLen bytes (ASCII)
 *
 * 协议版本号变更时请同步更新 [VERSION]。
 */
object WireProtocol {

    const val MAGIC = "FDFT"
    const val VERSION: Byte = 2

    /** 文件传输帧 */
    const val KIND_FILE: Byte = 1

    /** 客户端上报自身 IP 帧（用于组主反向发送） */
    const val KIND_REGISTER_IP: Byte = 2

    /** 最大允许的文件名长度（防止恶意端发送超大长度导致 OOM） */
    private const val MAX_NAME_LEN = 4096

    data class FileHeader(val name: String, val size: Long)
    data class RegisterIp(val ip: String)

    /**
     * 写文件传输头。调用方接着写 [FileHeader.size] 字节的文件内容。
     */
    @Throws(IOException::class)
    fun writeFileHeader(out: OutputStream, name: String, size: Long) {
        val dos = if (out is DataOutputStream) out else DataOutputStream(out)
        val nameBytes = name.toByteArray(Charsets.UTF_8)
        require(nameBytes.size in 1..MAX_NAME_LEN) {
            "Filename length out of range: ${nameBytes.size}"
        }
        require(size >= 0) { "File size must be >= 0" }
        dos.write(MAGIC.toByteArray(Charsets.US_ASCII))
        dos.writeByte(VERSION.toInt())
        dos.writeByte(KIND_FILE.toInt())
        dos.writeInt(nameBytes.size)
        dos.write(nameBytes)
        dos.writeLong(size)
        dos.flush()
    }

    /**
     * 写客户端上报 IP 帧。组主侧会在 [WIFI_REGISTER_PORT] 收到这个帧。
     */
    @Throws(IOException::class)
    fun writeRegisterIp(out: OutputStream, ip: String) {
        val dos = if (out is DataOutputStream) out else DataOutputStream(out)
        val ipBytes = ip.toByteArray(Charsets.US_ASCII)
        require(ipBytes.size in 1..255) { "IP length out of range: ${ipBytes.size}" }
        dos.write(MAGIC.toByteArray(Charsets.US_ASCII))
        dos.writeByte(VERSION.toInt())
        dos.writeByte(KIND_REGISTER_IP.toInt())
        dos.writeByte(ipBytes.size)
        dos.write(ipBytes)
        dos.flush()
    }

    /**
     * 读下一帧的前缀（magic + version + kind）。返回 [DataInputStream] 之后已经
     * 指向 payload 起始位置；调用方根据 [readFramePrefix] 返回的 kind 选择
     * 用 [readFilePayload] 或 [readRegisterIpPayload] 继续读。
     */
    data class FramePrefix(val version: Byte, val kind: Byte)

    @Throws(IOException::class)
    fun readFramePrefix(input: InputStream): FramePrefix {
        val dis = if (input is DataInputStream) input else DataInputStream(input)
        val magic = ByteArray(MAGIC.length)
        dis.readFully(magic)
        val magicStr = String(magic, Charsets.US_ASCII)
        require(magicStr == MAGIC) { "Invalid magic: \"$magicStr\"" }
        val version = dis.readByte()
        require(version == VERSION) { "Unsupported protocol version: $version" }
        val kind = dis.readByte()
        require(kind == KIND_FILE || kind == KIND_REGISTER_IP) {
            "Unknown frame kind: $kind"
        }
        return FramePrefix(version, kind)
    }

    @Throws(IOException::class)
    fun readFilePayload(input: InputStream): FileHeader {
        val dis = if (input is DataInputStream) input else DataInputStream(input)
        val nameLen = dis.readInt()
        require(nameLen in 1..MAX_NAME_LEN) { "Filename length out of range: $nameLen" }
        val nameBytes = ByteArray(nameLen)
        dis.readFully(nameBytes)
        val size = dis.readLong()
        require(size >= 0) { "File size must be >= 0" }
        return FileHeader(String(nameBytes, Charsets.UTF_8), size)
    }

    @Throws(IOException::class)
    fun readRegisterIpPayload(input: InputStream): RegisterIp {
        val dis = if (input is DataInputStream) input else DataInputStream(input)
        val ipLen = dis.readUnsignedByte()
        require(ipLen in 1..255) { "IP length out of range: $ipLen" }
        val ipBytes = ByteArray(ipLen)
        dis.readFully(ipBytes)
        return RegisterIp(String(ipBytes, Charsets.US_ASCII))
    }
}
