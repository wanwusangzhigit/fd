// ContentView.swift — adaptive SwiftUI root view.
//
// Two-tab NavigationStack:
//   * Receive — toggles the TCP listener and shows incoming files.
//   * Send — picks a file via `.fileImporter`, browses for peers,
//            and shows the send queue.
//
// On iPad (regular size class) we use a NavigationSplitView with the
// two panes side-by-side; on iPhone we use a TabView.
import SwiftUI

struct ContentView: View {
    @EnvironmentObject var model: TransferModel
    @Environment(\.horizontalSizeClass) var sizeClass

    var body: some View {
        if sizeClass == .regular {
            NavigationSplitView {
                List(selection: .constant(0)) {
                    NavigationLink("Receive", value: 0).badge(model.recvs.count)
                    NavigationLink("Send", value: 1).badge(model.sends.count)
                }
                .navigationTitle("WFDTransfer")
            } detail: {
                TabView(selection: .constant(0)) {
                    ReceiveView().tabItem { Label("Receive", systemImage: "tray.and.arrow.down") }
                    SendView().tabItem { Label("Send", systemImage: "paperplane") }
                }
            }
        } else {
            TabView {
                ReceiveView()
                    .tabItem { Label("Receive", systemImage: "tray.and.arrow.down") }
                SendView()
                    .tabItem { Label("Send", systemImage: "paperplane") }
            }
        }
    }
}
