// Protocol.swift — FDFT v2 binary protocol, Swift port of `Protocol.h`.
//
// Frame layout (all integers big-endian):
//
//     MAGIC    : 4 bytes   "FDFT"
//     VERSION  : 1 byte    = 2
//     KIND     : 1 byte
//     payload (depends on KIND)
//
// KIND = 1 (FILE)
//     nameLen  : UInt32 BE
//     name     : nameLen bytes (UTF-8)
//     size     :  Int64 BE
//     payload  : size bytes follow
//
// KIND = 2 (REGISTER_IP)
//     ipLen    : UInt8
//     ip       : ipLen bytes (ASCII)
//
// KIND = 3 (HASH) — optional trailer
//     algo     : UInt8   (= 1 for SHA-256)
//     digestLen: UInt8   (32 for SHA-256)
//     digest   : digestLen bytes
//
// Wire-compatible with the Kotlin (Android) and C++ (desktop) implementations
// bit-for-bit. The HASH trailer is appended only by native senders; the
// Android receiver stops reading after `size` body bytes and silently
// ignores the trailing HASH frame.
import Foundation

public enum FDFT {
    public static let magic: [UInt8] = [0x46, 0x44, 0x46, 0x54] // "FDFT"
    public static let version: UInt8 = 2

    public static let kindFile: UInt8 = 1
    public static let kindRegisterIP: UInt8 = 2
    public static let kindHash: UInt8 = 3

    public static let hashAlgoSHA256: UInt8 = 1
    public static let maxNameLen: Int = 4096
    public static let maxDigestLen: Int = 64
}

public struct FileHeader: Equatable {
    public let name: String
    public let size: Int64
    public init(name: String, size: Int64) {
        self.name = name
        self.size = size
    }
}

public enum ProtocolError: Error, Equatable {
    case eof
    case shortRead(expected: Int, actual: Int)
    case invalidMagic
    case unsupportedVersion(found: UInt8)
    case unknownKind(found: UInt8)
    case nameLengthOutOfRange(found: Int)
    case negativeSize
    case digestLengthOutOfRange(found: Int)
    case emptyIP
    case ioError(String)
}

// MARK: - Big-endian helpers

@usableFromInline
internal func storeU32BE(_ value: UInt32) -> [UInt8] {
    return [
        UInt8((value >> 24) & 0xFF),
        UInt8((value >> 16) & 0xFF),
        UInt8((value >>  8) & 0xFF),
        UInt8( value        & 0xFF),
    ]
}

@usableFromInline
internal func storeU64BE(_ value: UInt64) -> [UInt8] {
    var out: [UInt8] = []
    out.reserveCapacity(8)
    for i in (0..<8).reversed() {
        out.append(UInt8((value >> UInt64(i * 8)) & 0xFF))
    }
    return out
}

@usableFromInline
internal func loadU32BE(_ bytes: [UInt8]) -> UInt32 {
    return (UInt32(bytes[0]) << 24)
         | (UInt32(bytes[1]) << 16)
         | (UInt32(bytes[2]) <<  8)
         |  UInt32(bytes[3])
}

@usableFromInline
internal func loadU64BE(_ bytes: [UInt8]) -> UInt64 {
    var v: UInt64 = 0
    for b in bytes.prefix(8) {
        v = (v << 8) | UInt64(b)
    }
    return v
}

// MARK: - Reader / Writer abstractions
//
// Pulling bytes through a generic `Reader` (and pushing them through a
// `Writer`) lets us reuse the same protocol code against:
//   * an in-memory `[UInt8]` (used by the tests)
//   * a `Data` buffer
//   * a `NWConnection` send/recv loop
//   * a `FileHandle` (stdin / stdout)

public protocol Reader {
    // Reads up to `count` bytes into `buffer` starting at `offset`.
    // Returns the number of bytes actually read; 0 indicates clean EOF.
    // Throws on underlying I/O failure.
    mutating func read(into buffer: UnsafeMutableRawPointer,
                       count: Int) throws -> Int
}

public protocol Writer {
    // Writes exactly `count` bytes from `buffer`. Throws on short write
    // or underlying I/O failure.
    func write(_ buffer: UnsafeRawPointer, count: Int) throws
}

// Convenience: read exactly `n` bytes, throwing `.eof` / `.shortRead` on
// partial data.
public func readExact<R: Reader>(_ reader: inout R, _ n: Int) throws -> [UInt8] {
    if n == 0 { return [] }
    var buf = [UInt8](repeating: 0, count: n)
    var got = 0
    try buf.withUnsafeMutableBufferPointer { ptr in
        while got < n {
            let bytesRead = try reader.read(into: ptr.baseAddress! + got,
                                            count: n - got)
            if bytesRead == 0 { throw ProtocolError.eof }
            got += bytesRead
        }
    }
    return buf
}

public func writeAll<W: Writer>(_ writer: W, _ bytes: [UInt8]) throws {
    if bytes.isEmpty { return }
    try bytes.withUnsafeBufferPointer { ptr in
        try writer.write(ptr.baseAddress!, count: ptr.count)
    }
}

// MARK: - Encoders

public func writeFileHeader<W: Writer>(_ w: W, name: String, size: Int64) throws {
    let bytes = name.utf8
    if bytes.isEmpty || bytes.count > FDFT.maxNameLen {
        throw ProtocolError.nameLengthOutOfRange(found: bytes.count)
    }
    if size < 0 { throw ProtocolError.negativeSize }

    var prefix: [UInt8] = []
    prefix.reserveCapacity(6 + 4 + bytes.count + 8)
    prefix.append(contentsOf: FDFT.magic)
    prefix.append(FDFT.version)
    prefix.append(FDFT.kindFile)
    prefix.append(contentsOf: storeU32BE(UInt32(bytes.count)))
    prefix.append(contentsOf: bytes)
    prefix.append(contentsOf: storeU64BE(UInt64(size)))
    try writeAll(w, prefix)
}

public func writeRegisterIP<W: Writer>(_ w: W, ip: String) throws {
    let bytes = ip.utf8
    guard !bytes.isEmpty, bytes.count <= 255 else {
        throw ProtocolError.emptyIP
    }
    var prefix: [UInt8] = []
    prefix.reserveCapacity(6 + 1 + bytes.count)
    prefix.append(contentsOf: FDFT.magic)
    prefix.append(FDFT.version)
    prefix.append(FDFT.kindRegisterIP)
    prefix.append(UInt8(bytes.count))
    prefix.append(contentsOf: bytes)
    try writeAll(w, prefix)
}

public func writeHashTrailer<W: Writer>(
    _ w: W, algo: UInt8, digest: [UInt8]
) throws {
    if digest.isEmpty || digest.count > FDFT.maxDigestLen {
        throw ProtocolError.digestLengthOutOfRange(found: digest.count)
    }
    var prefix: [UInt8] = []
    prefix.reserveCapacity(6 + 1 + 1 + digest.count)
    prefix.append(contentsOf: FDFT.magic)
    prefix.append(FDFT.version)
    prefix.append(FDFT.kindHash)
    prefix.append(algo)
    prefix.append(UInt8(digest.count))
    prefix.append(contentsOf: digest)
    try writeAll(w, prefix)
}

// MARK: - Decoders

public func readFramePrefix<R: Reader>(_ reader: inout R) throws -> UInt8 {
    let header = try readExact(&reader, 6)
    if Array(header.prefix(4)) != FDFT.magic { throw ProtocolError.invalidMagic }
    if header[4] != FDFT.version {
        throw ProtocolError.unsupportedVersion(found: header[4])
    }
    let kind = header[5]
    if kind != FDFT.kindFile && kind != FDFT.kindRegisterIP && kind != FDFT.kindHash {
        throw ProtocolError.unknownKind(found: kind)
    }
    return kind
}

public func readFilePayload<R: Reader>(_ reader: inout R) throws -> FileHeader {
    let lenBytes = try readExact(&reader, 4)
    let nameLen = Int(loadU32BE(lenBytes))
    if nameLen == 0 || nameLen > FDFT.maxNameLen {
        throw ProtocolError.nameLengthOutOfRange(found: nameLen)
    }
    let nameBytes = try readExact(&reader, nameLen)
    let sizeBytes = try readExact(&reader, 8)
    let size = Int64(bitPattern: loadU64BE(sizeBytes))
    guard let name = String(bytes: nameBytes, encoding: .utf8) else {
        throw ProtocolError.nameLengthOutOfRange(found: nameLen)
    }
    return FileHeader(name: name, size: size)
}

public func readRegisterIPPayload<R: Reader>(_ reader: inout R) throws -> String {
    let lenByte = try readExact(&reader, 1)[0]
    if lenByte == 0 { throw ProtocolError.emptyIP }
    let bytes = try readExact(&reader, Int(lenByte))
    guard let ip = String(bytes: bytes, encoding: .ascii) else {
        throw ProtocolError.emptyIP
    }
    return ip
}

public func readHashPayload<R: Reader>(_ reader: inout R) throws -> (algo: UInt8, digest: [UInt8]) {
    let algo = try readExact(&reader, 1)[0]
    let len = try readExact(&reader, 1)[0]
    if len == 0 || Int(len) > FDFT.maxDigestLen {
        throw ProtocolError.digestLengthOutOfRange(found: Int(len))
    }
    let digest = try readExact(&reader, Int(len))
    return (algo, digest)
}

// MARK: - In-memory reader/writer (for tests + intermediate buffers)

public struct ByteBufferReader: Reader {
    public private(set) var bytes: [UInt8]
    public private(set) var pos: Int = 0

    public init(_ bytes: [UInt8]) { self.bytes = bytes }

    public mutating func read(into buffer: UnsafeMutableRawPointer,
                              count: Int) throws -> Int {
        if pos >= bytes.count { return 0 }
        let available = bytes.count - pos
        let give = min(available, count)
        bytes.withUnsafeBufferPointer { src -> Void in
            // memcpy returns the destination pointer; we don't need it,
            // but explicitly discard so the closure's return type is Void
            // (avoids "unused result" warning under -swift-runner strict mode).
            _ = memcpy(buffer, src.baseAddress! + pos, give)
        }
        pos += give
        return give
    }
}

public final class ByteBufferWriter: Writer {
    public private(set) var bytes: [UInt8] = []

    public init() {}
    public func write(_ buffer: UnsafeRawPointer, count: Int) throws {
        let src = buffer.assumingMemoryBound(to: UInt8.self)
        bytes.append(contentsOf: UnsafeBufferPointer(start: src, count: count))
    }
    /// Convenience helper used by tests + ad-hoc writers.
    public func append(_ bytes: [UInt8]) {
        self.bytes.append(contentsOf: bytes)
    }
}
