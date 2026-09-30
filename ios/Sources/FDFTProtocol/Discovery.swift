// Discovery.swift — Bonjour-based LAN discovery + NWListener TCP server.
//
// iOS cannot send raw UDP broadcast (only managed frameworks), so we use
// Bonjour (`NetService` for publishing + `NetServiceBrowser` for browsing)
// which is the idiomatic iOS / macOS way to find peers on the LAN.
//
// We publish a service of type `_wfd._tcp.` whose TXT record carries the
// hostname. Desktop peers using the C++ `Beacon` see our service via
// `dns_sd.h` if they want, or can simply keep using UDP broadcast.
//
// On Linux we stub these out — Network.framework isn't available.
import Foundation
#if canImport(Network)
import Network
#endif

public struct Peer: Equatable, Hashable {
    public let host: String
    public let ip: String
    public init(host: String, ip: String) { self.host = host; self.ip = ip }
}

public enum DiscoveryConstants {
    public static let serviceType = "_wfd._tcp."
    public static let serviceDomain = "" // default: browse local domains
    public static let beaconIntervalMs: Int = 1500
}

#if canImport(Network) && canImport(Foundation)

/// Publishes a Bonjour service so other peers can find us.
public final class Beacon: NSObject {
    private var service: NetService?
    private let port: Int32
    private let name: String

    public init(port: Int32 = 0) {
        self.port = port
        self.name = ProcessInfo.processInfo.localHost ?? "iOS-device"
        super.init()
    }

    public func start() {
        let svc = NetService(domain: "", type: DiscoveryConstants.serviceType,
                             name: name, port: port)
        var txtDict: [String: Data] = [:]
        if let host = ProcessInfo.processInfo.localHost {
            txtDict["host"] = host.data(using: .utf8)
        }
        svc.setTXTRecord(NetService.data(fromTXTRecord: txtDict))
        svc.publish()
        self.service = svc
    }

    public func stop() {
        service?.stop()
        service = nil
    }
}

/// Browses the LAN for `_wfd._tcp.` services and reports each one found.
public final class PeerBrowser: NSObject, NetServiceBrowserDelegate {
    private let browser = NetServiceBrowser()
    private var resolved: [Peer] = []
    private var services: [NetService] = []
    public var onChange: (([Peer]) -> Void)?

    public override init() {
        super.init()
        browser.delegate = self
    }

    public func start() {
        browser.searchForServices(ofType: DiscoveryConstants.serviceType,
                                  inDomain: DiscoveryConstants.serviceDomain)
    }

    public func stop() {
        browser.stop()
    }

    public func netServiceBrowser(_ browser: NetServiceBrowser,
                                  didFind service: NetService,
                                  moreComing: Bool) {
        services.append(service)
        service.delegate = self
        service.resolve(withTimeout: 4.0)
        if !moreComing { onChange?(resolved) }
    }

    public func netServiceBrowser(_ browser: NetServiceBrowser,
                                  didRemove service: NetService,
                                  moreComing: Bool) {
        services.removeAll { $0 === service }
        if !moreComing { onChange?(resolved) }
    }
}

extension PeerBrowser: NetServiceDelegate {
    public func netServiceDidResolveAddress(_ sender: NetService) {
        if let host = sender.hostName {
            let peer = Peer(host: sender.name, ip: host)
            if !resolved.contains(peer) { resolved.append(peer) }
            onChange?(resolved)
        }
    }

    public func netService(_ sender: NetService,
                           didNotResolve errorDict: [String : NSNumber]) {
        // Soft-fail; peer simply won't appear.
    }
}

// MARK: - TCP listener

/// Accepts inbound TCP connections and dispatches each as an FDFT file
/// receive. Mirrors `TcpListener` from the C++ side.
public final class TCPListener {
    #if canImport(Network)
    private var listener: NWListener?
    private let queue = DispatchQueue(label: "wfd.listener")
    public private(set) var port: Int = 0
    public private(set) var isRunning = false
    private let receiveDir: String
    private let onReceived: ReceivedCallback

    public init(receiveDir: String, onReceived: @escaping ReceivedCallback) {
        self.receiveDir = receiveDir
        self.onReceived = onReceived
    }

    public func start(port desired: UInt16 = 0) -> Bool {
        let parameters = NWParameters.tcp
        let listener: NWListener
        do {
            listener = try NWListener(using: parameters, on: NWEndpoint.Port(integerLiteral: desired))
        } catch {
            return false
        }
        listener.newConnectionHandler = { [weak self] conn in
            self?.handle(conn)
        }
        listener.stateUpdateHandler = { [weak self] state in
            if case .ready = state { self?.isRunning = true }
        }
        listener.start(queue: queue)
        self.listener = listener
        self.port = Int(desired)
        return true
    }

    public func stop() {
        listener?.cancel()
        listener = nil
        isRunning = false
    }

    private func handle(_ conn: NWConnection) {
        conn.start(queue: queue)
        let peerHost = conn.endpoint.debugDescription
        Task {
            do {
                try await self.receiveOne(conn: conn, peerHost: peerHost)
            } catch {
                self.onReceived(ReceivedFile(
                    peerHost: peerHost,
                    fileName: "",
                    fileSize: 0,
                    errorMessage: "\(error)"))
            }
        }
    }

    private func receiveOne(conn: NWConnection, peerHost: String) async throws {
        // Async versions of the protocol helpers — read frames directly
        // off the NWConnection rather than through the sync `Reader`
        // protocol (which would need a semaphore bridge that risks
        // deadlock when the connection's queue is the same one we're
        // blocking).
        let kind = try await readFramePrefixAsync(conn)
        guard kind == FDFT.kindFile else {
            throw ProtocolError.unknownKind(found: kind)
        }
        let header = try await readFilePayloadAsync(conn)
        let sink = FileSink(receiveDir: receiveDir)
        let stream = SHA256Stream()
        let outcome = try await sink.stream(
            from: { [weak conn] -> Data? in
                guard let conn = conn else { return nil }
                // NWConnection.receive on a closed connection throws; treat
                // any error as clean EOF so the sink stops cleanly.
                let chunk: Data
                do {
                    chunk = try await conn.receive(minimum: 1, maximum: 64 * 1024)
                } catch {
                    return nil
                }
                return chunk.isEmpty ? nil : chunk
            },
            size: header.size,
            tee: { stream.update($0) })

        var receivedHash: String?
        var integrityVerified = false
        // Optional HASH trailer — best-effort.
        let peekOpt: Data? = try? await conn.receive(minimum: 6, maximum: 6)
        if let peek = peekOpt, peek.count == 6 {
            let prefix = Array(peek)
            if Array(prefix.prefix(4)) == FDFT.magic,
               prefix[4] == FDFT.version,
               prefix[5] == FDFT.kindHash {
                let restOpt: Data? = try? await conn.receive(minimum: 2, maximum: 2)
                if let rest = restOpt, rest.count == 2 {
                    let digestLen = Int(rest[1])
                    if digestLen > 0 && digestLen <= FDFT.maxDigestLen {
                        let dOpt: Data? = try? await conn.receive(
                            minimum: digestLen, maximum: digestLen)
                        if let d = dOpt, d.count == digestLen {
                            let digest = Array(d)
                            let computed = stream.finish()
                            integrityVerified = (digest == computed)
                            receivedHash = digest.map {
                                String(format: "%02x", $0)
                            }.joined()
                        }
                    }
                }
            }
        }
        onReceived(ReceivedFile(
            peerHost: peerHost,
            fileName: header.name,
            fileSize: header.size,
            savedPath: outcome.path,
            errorMessage: (receivedHash != nil && !integrityVerified)
                          ? "SHA-256 mismatch" : nil,
            receivedHash: receivedHash,
            integrityVerified: integrityVerified))
    }
    #else
    public init(receiveDir: String, onReceived: @escaping ReceivedCallback) {}
    public func start(port: UInt16 = 0) -> Bool { false }
    public func stop() {}
    #endif
}

#if canImport(Network)
// MARK: - Async NWConnection read helpers
//
// These mirror the synchronous `readFramePrefix` / `readFilePayload` in
// Protocol.swift but pull bytes via NWConnection's native async
// `receive(minimum:maximum:)` so we never need to bridge across queue
// boundaries with semaphores.

private func receiveExact(_ conn: NWConnection,
                          _ n: Int) async throws -> Data {
    var collected = Data(capacity: n)
    while collected.count < n {
        let chunk = try await conn.receive(minimum: 1, maximum: n - collected.count)
        if chunk.isEmpty { throw ProtocolError.eof }
        collected.append(chunk)
    }
    return collected
}

private func readFramePrefixAsync(_ conn: NWConnection) async throws -> UInt8 {
    let header = try await receiveExact(conn, 6)
    let bytes = Array(header)
    if Array(bytes.prefix(4)) != FDFT.magic { throw ProtocolError.invalidMagic }
    if bytes[4] != FDFT.version {
        throw ProtocolError.unsupportedVersion(found: bytes[4])
    }
    let kind = bytes[5]
    if kind != FDFT.kindFile && kind != FDFT.kindRegisterIP && kind != FDFT.kindHash {
        throw ProtocolError.unknownKind(found: kind)
    }
    return kind
}

private func readFilePayloadAsync(_ conn: NWConnection) async throws -> FileHeader {
    let lenBytes = try await receiveExact(conn, 4)
    let nameLen = Int(loadU32BE(Array(lenBytes)))
    if nameLen == 0 || nameLen > FDFT.maxNameLen {
        throw ProtocolError.nameLengthOutOfRange(found: nameLen)
    }
    let nameData = try await receiveExact(conn, nameLen)
    let sizeBytes = try await receiveExact(conn, 8)
    let size = Int64(bitPattern: loadU64BE(Array(sizeBytes)))
    guard let name = String(data: nameData, encoding: .utf8) else {
        throw ProtocolError.nameLengthOutOfRange(found: nameLen)
    }
    return FileHeader(name: name, size: size)
}
#endif

extension ProcessInfo {
    /// Best-effort local hostname for Bonjour publishing.
    var localHost: String? {
        // `Host.current()` is unavailable on iOS; fall back to the device
        // name which iOS exposes via `ProcessInfo.processInfo.hostName`
        // (typically "iPhone" / "iPad" / "<user>'s iPhone").
        return ProcessInfo.processInfo.hostName
    }
}
#endif
