// TransferModel.swift — central observed object binding protocol layer to UI.
import Foundation
import SwiftUI
import FDFTProtocol

@MainActor
final class TransferModel: ObservableObject {
    @Published var peers: [Peer] = []
    @Published var sends: [SendRow] = []
    @Published var recvs: [RecvRow] = []
    @Published var listening: Bool = false
    @Published var receiveDir: String = {
        let docs = FileManager.default.urls(for: .documentDirectory,
                                            in: .userDomainMask).first!
        return docs.appendingPathComponent("wfd_received").path
    }()

    private var listener: TCPListener?
    private var browser: PeerBrowser?
    private let beacon = Beacon()

    struct SendRow: Identifiable, Equatable {
        let id = UUID()
        let name: String
        let peer: String
        var progress: Double
        var state: State
        var detail: String
        enum State { case pending, sending, done, failed }
    }

    struct RecvRow: Identifiable, Equatable {
        let id = UUID()
        let peer: String
        let name: String
        let size: Int64
        let path: String?
        let hash: String?
        let ok: Bool
    }

    func startListening() {
        guard !listening else { return }
        try? FileManager.default.createDirectory(
            atPath: receiveDir, withIntermediateDirectories: true)
        let listener = TCPListener(receiveDir: receiveDir) { [weak self] rf in
            Task { @MainActor in
                self?.recvs.insert(
                    RecvRow(peer: rf.peerHost,
                            name: rf.fileName,
                            size: rf.fileSize,
                            path: rf.savedPath,
                            hash: rf.receivedHash,
                            ok: rf.errorMessage == nil),
                    at: 0)
            }
        }
        if listener.start(port: 8988) {
            self.listener = listener
            self.listening = true
            beacon.start()
        }
    }

    func stopListening() {
        listener?.stop()
        listener = nil
        listening = false
        beacon.stop()
    }

    func startDiscovery() {
        let b = PeerBrowser()
        b.onChange = { [weak self] peers in
            Task { @MainActor in self?.peers = peers }
        }
        b.start()
        self.browser = b
    }

    func stopDiscovery() {
        browser?.stop()
        browser = nil
    }

    func send(_ fileURL: URL, to peer: Peer) {
        let row = SendRow(name: fileURL.lastPathComponent,
                          peer: peer.host,
                          progress: 0,
                          state: .pending,
                          detail: "")
        sends.append(row)
        let idx = sends.count - 1
        Task {
            let result = await TCPTransport.send(
                host: peer.ip,
                port: 8988,
                filePath: fileURL.path,
                forcedName: fileURL.lastPathComponent,
                includeHashTrailer: true) { transferred, total in
                    Task { @MainActor in
                        guard self.sends.indices.contains(idx) else { return }
                        let pct = total > 0 ? Double(transferred) / Double(total) : 0
                        self.sends[idx].progress = pct
                        self.sends[idx].state = .sending
                    }
                }
            await MainActor.run {
                guard self.sends.indices.contains(idx) else { return }
                self.sends[idx].state = result.ok ? .done : .failed
                self.sends[idx].detail = result.error ?? result.senderHash ?? ""
                if result.ok { self.sends[idx].progress = 1.0 }
            }
        }
    }
}
