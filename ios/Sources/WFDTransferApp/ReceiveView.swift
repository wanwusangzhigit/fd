// ReceiveView.swift — toggles the TCP listener and shows incoming files.
import SwiftUI

struct ReceiveView: View {
    @EnvironmentObject var model: TransferModel

    var body: some View {
        List {
            Section {
                Toggle("Listening", isOn: $model.listening)
                    .onChange(of: model.listening) { _, on in
                        if on { model.startListening() } else { model.stopListening() }
                    }
                LabeledContent("Save to") {
                    Text(model.receiveDir)
                        .font(.caption.monospaced())
                        .lineLimit(2)
                        .truncationMode(.middle)
                }
            }

            Section("Discovered peers") {
                if model.peers.isEmpty {
                    Text("Tap Discover below…").foregroundStyle(.secondary)
                }
                ForEach(model.peers, id: \.self) { peer in
                    VStack(alignment: .leading) {
                        Text(peer.host).font(.body)
                        Text(peer.ip).font(.caption.monospaced()).foregroundStyle(.secondary)
                    }
                }
            }

            Section {
                Button("Discover peers") { model.startDiscovery() }
                Button("Stop discovery", role: .destructive) { model.stopDiscovery() }
            }

            Section("Received") {
                if model.recvs.isEmpty {
                    Text("No files received yet.").foregroundStyle(.secondary)
                }
                ForEach(model.recvs) { r in
                    VStack(alignment: .leading, spacing: 4) {
                        HStack {
                            Image(systemName: r.ok ? "checkmark.circle.fill" : "xmark.octagon.fill")
                                .foregroundStyle(r.ok ? .green : .red)
                            Text(r.name).font(.body)
                            Spacer()
                            Text(human(r.size)).font(.caption).foregroundStyle(.secondary)
                        }
                        if let h = r.hash {
                            Text("sha256: \(h.prefix(16))…")
                                .font(.caption2.monospaced())
                                .foregroundStyle(.secondary)
                        }
                        if let p = r.path {
                            Text(p).font(.caption2.monospaced())
                                .foregroundStyle(.secondary)
                                .lineLimit(1).truncationMode(.middle)
                        }
                    }
                }
            }
        }
        .navigationTitle("Receive")
    }

    private func human(_ bytes: Int64) -> String {
        if bytes < 1024 { return "\(bytes) B" }
        if bytes < 1024 * 1024 { return String(format: "%.1f KB", Double(bytes) / 1024) }
        if bytes < 1024 * 1024 * 1024 { return String(format: "%.1f MB", Double(bytes) / 1024 / 1024) }
        return String(format: "%.2f GB", Double(bytes) / 1024 / 1024 / 1024)
    }
}
