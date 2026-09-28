// SendView.swift — picks a file and sends to a chosen peer.
import SwiftUI
import UniformTypeIdentifiers

struct SendView: View {
    @EnvironmentObject var model: TransferModel
    @State private var showPicker = false
    @State private var pickedURL: URL?

    var body: some View {
        List {
            Section("Peers") {
                if model.peers.isEmpty {
                    Button("Discover…") { model.startDiscovery() }
                }
                ForEach(model.peers, id: \.self) { peer in
                    Button {
                        guard let url = pickedURL else { return }
                        model.send(url, to: peer)
                    } label: {
                        VStack(alignment: .leading) {
                            Text(peer.host).font(.body)
                            Text(peer.ip).font(.caption.monospaced()).foregroundStyle(.secondary)
                        }
                    }
                }
            }

            Section {
                Button {
                    showPicker = true
                } label: {
                    if let url = pickedURL {
                        Label(url.lastPathComponent, systemImage: "doc")
                    } else {
                        Label("Pick a file…", systemImage: "doc")
                    }
                }
            }

            Section("Queue") {
                if model.sends.isEmpty {
                    Text("Nothing queued.").foregroundStyle(.secondary)
                }
                ForEach(model.sends) { row in
                    VStack(alignment: .leading, spacing: 4) {
                        HStack {
                            Image(systemName: icon(for: row.state))
                                .foregroundStyle(color(for: row.state))
                            Text(row.name)
                            Spacer()
                            Text("\(Int(row.progress * 100))%")
                                .font(.caption.monospaced())
                        }
                        ProgressView(value: row.progress)
                            .progressViewStyle(.linear)
                        Text("to \(row.peer)")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                }
            }
        }
        .navigationTitle("Send")
        .fileImporter(isPresented: $showPicker,
                      allowedContentTypes: [.data, .item],
                      allowsMultipleSelection: false) { result in
            if let urls = try? result.get(), let url = urls.first {
                pickedURL = url
            }
        }
    }

    private func icon(for s: TransferModel.SendRow.State) -> String {
        switch s {
        case .pending: return "clock"
        case .sending: return "paperplane.fill"
        case .done: return "checkmark.circle.fill"
        case .failed: return "xmark.octagon.fill"
        }
    }

    private func color(for s: TransferModel.SendRow.State) -> Color {
        switch s {
        case .pending: return .gray
        case .sending: return .blue
        case .done: return .green
        case .failed: return .red
        }
    }
}
