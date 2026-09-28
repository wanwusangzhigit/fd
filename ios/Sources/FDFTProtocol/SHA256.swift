// SHA256.swift — thin SHA-256 wrapper.
//
// Uses CryptoKit on Apple platforms and swift-crypto elsewhere. Both
// APIs are intentionally CryptoKit-shaped, so we expose one type that
// works as either.
import Foundation
#if canImport(CryptoKit)
import CryptoKit
public typealias SHA256Digest = CryptoKit.SHA256
#else
import Crypto
public typealias SHA256Digest = Crypto.SHA256
#endif

public enum HashUtil {
    public static func sha256(_ data: Data) -> [UInt8] {
        return Array(SHA256Digest.hash(data))
    }

    public static func sha256(_ bytes: [UInt8]) -> [UInt8] {
        return Array(SHA256Digest.hash(bytes))
    }

    public static func sha256Hex(_ data: Data) -> String {
        return sha256(data).map { String(format: "%02x", $0) }.joined()
    }
}

/// Incremental SHA-256 streaming wrapper, used while sending / receiving
/// file bodies so we can compute the digest in lockstep with the bytes
/// flowing over the wire.
public final class SHA256Stream {
    private var hasher: SHA256Digest

    public init() { hasher = SHA256Digest() }

    public func update(_ data: Data) {
        hasher.update(data: data)
    }

    public func update(_ bytes: [UInt8]) {
        hasher.update(data: bytes.withUnsafeBufferPointer { Data(buffer: $0) })
    }

    public func update(_ bytes: UnsafeRawPointer, count: Int) {
        let bufPtr = bytes.assumingMemoryBound(to: UInt8.self)
        hasher.update(data: Data(bytes: bufPtr, count: count))
    }

    public func finish() -> [UInt8] {
        return Array(hasher.finalize())
    }

    public func finishHex() -> String {
        return finish().map { String(format: "%02x", $0) }.joined()
    }
}
