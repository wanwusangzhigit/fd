// TCPTransport.swift — TCP send / receive using Network.framework.
//
// On Apple platforms (iOS / iPadOS / macOS / tvOS) we use NWListener +
// NWConnection which abstracts IPv4/IPv6 and gives us TLS hand-offs if
// we ever need them. On Linux we don't have Network.framework, so the
// transport layer is stubbed out — the protocol layer still works.
import Foundation
#if canImport(Network)
import Network
#endif
#if canImport(os)
import os
#endif

public struct SendResult: Equatable, Sendable {
    public let ok: Bool
    public let error: String?
    public let bytesSent: Int64
    public let senderHash: String? // SHA-256 hex if a trailer was emitted
    public init(ok: Bool, error: String? = nil, bytesSent: Int64 = 0,
                senderHash: String? = nil) {
        self.ok = ok
        self.error = error
        self.bytesSent = bytesSent
        self.senderHash = senderHash
    }
}

public struct ReceivedFile: Equatable, Sendable {
    public let peerHost: String
    public let fileName: String
    public let fileSize: Int64
    public let savedPath: String?
    public let errorMessage: String?
    public let receivedHash: String?
    public let integrityVerified: Bool
    public init(peerHost: String, fileName: String, fileSize: Int64,
                savedPath: String? = nil, errorMessage: String? = nil,
                receivedHash: String? = nil, integrityVerified: Bool = false) {
        self.peerHost = peerHost
        self.fileName = fileName
        self.fileSize = fileSize
        self.savedPath = savedPath
        self.errorMessage = errorMessage
        self.receivedHash = receivedHash
        self.integrityVerified = integrityVerified
    }
}

public typealias ProgressCallback = (Int64, Int64) -> Void
public typealias ReceivedCallback = (ReceivedFile) -> Void

public enum TCPTransport {
    #if canImport(Network)
    /// Streams `filePath` to `host:port`. Optionally appends a SHA-256
    /// trailer (default true). Returns once the transfer completes.
    public static func send(
        host: String,
        port: Int,
        filePath: String,
        forcedName: String? = nil,
        includeHashTrailer: Bool = true,
        onProgress: ProgressCallback? = nil
    ) async -> SendResult {
        let url = URL(fileURLWithPath: filePath)
        let name = forcedName ?? url.lastPathComponent

        guard let attrs = try? FileManager.default.attributesOfItem(atPath: filePath),
              let size = (attrs[.size] as? NSNumber)?.int64Value else {
            return SendResult(ok: false, error: "cannot stat: \(filePath)")
        }
        guard let data = try? Data(contentsOf: url) else {
            return SendResult(ok: false, error: "cannot open: \(filePath)")
        }

        return await withCheckedContinuation { (cont: CheckedContinuation<SendResult, Never>) in
            let conn = NWConnection(host: NWEndpoint.Host(host),
                                    port: NWEndpoint.Port(integerLiteral: UInt16(port)),
                                    using: .tcp)
            let writer = ConnectionWriter(connection: conn)
            // Swift-6-safe one-shot guard. OSAllocatedUnfairLock is a
            // value-type lock whose withLock is async-safe (no
            // NSLock.lock()/unlock() from an async context, no captured
            // mutable `var didFinish` accessed concurrently).
            let didFinish = OSAllocatedUnfairLock(initialState: false)

            // Resume the continuation exactly once; subsequent calls
            // are no-ops. The atomic test-and-set inside a single
            // withLock scope proves to the Swift 6 compiler that the
            // shared flag is race-free.
            @Sendable
            func completeOnce(_ result: SendResult) {
                let firstCaller = didFinish.withLock { state -> Bool in
                    if state { return false }   // already claimed
                    state = true                // claim it now
                    return true
                }
                guard firstCaller else { return }
                cont.resume(returning: result)
                conn.cancel()
            }

            conn.stateUpdateHandler = { newState in
                switch newState {
                case .ready:
                    Task {
                        let result = await runSend(on: writer,
                                                   data: data,
                                                   size: size,
                                                   name: name,
                                                   includeHashTrailer: includeHashTrailer,
                                                   onProgress: onProgress)
                        completeOnce(result)
                    }
                case .failed(let err):
                    completeOnce(SendResult(ok: false, error: "\(err)"))
                case .cancelled:
                    completeOnce(SendResult(ok: false, error: "cancelled"))
                default:
                    break
                }
            }
            conn.start(queue: .global())
        }
    }

    private static func runSend(
        on writer: ConnectionWriter,
        data: Data,
        size: Int64,
        name: String,
        includeHashTrailer: Bool,
        onProgress: ProgressCallback?
    ) async -> SendResult {
        do {
            try writeFileHeader(writer, name: name, size: size)
        } catch {
            return SendResult(ok: false, error: "header: \(error)")
        }

        let stream = SHA256Stream()
        let chunkSize = 64 * 1024
        var sent: Int64 = 0
        while sent < size {
            let start = data.index(data.startIndex, offsetBy: Int(sent))
            let end = data.index(start, offsetBy: min(chunkSize, Int(size - sent)))
            let chunk = data.subdata(in: start..<end)
            do {
                try writer.writeSync(chunk)
            } catch {
                return SendResult(ok: false, error: "body: \(error)")
            }
            if includeHashTrailer { stream.update(chunk) }
            sent += Int64(chunk.count)
            if let cb = onProgress { cb(sent, size) }
        }

        var hashHex: String?
        if includeHashTrailer {
            let digest = stream.finish()
            do {
                try writeHashTrailer(writer, algo: FDFT.hashAlgoSHA256, digest: digest)
            } catch {
                return SendResult(ok: false, error: "trailer: \(error)")
            }
            hashHex = digest.map { String(format: "%02x", $0) }.joined()
        }

        return SendResult(ok: true, bytesSent: sent, senderHash: hashHex)
    }
    #endif
}

#if canImport(Network)
/// Adapter wrapping an `NWConnection` as a `Writer` with sync semantics.
final class ConnectionWriter: Writer, @unchecked Sendable {
    let connection: NWConnection
    init(connection: NWConnection) { self.connection = connection }

    func write(_ buffer: UnsafeRawPointer, count: Int) throws {
        let data = Data(bytes: buffer, count: count)
        try writeSync(data)
    }

    func writeSync(_ data: Data) throws {
        let semaphore = DispatchSemaphore(value: 0)
        var sendErr: NWError?
        connection.send(content: data, completion: .contentProcessed { err in
            sendErr = err
            semaphore.signal()
        })
        semaphore.wait()
        if let err = sendErr {
            throw NSError(domain: "wfd.send", code: 1,
                          userInfo: [NSLocalizedDescriptionKey: "\(err)"])
        }
    }
}
#endif
