// WFDTransferApp.swift — entry point for the iOS / iPadOS app.
//
// Universal app: same code path drives iPhone and iPad layouts. The
// `ContentView` adjusts its layout based on `horizontalSizeClass` and
// `idiom` so iPad gets a side-by-side layout while iPhone gets stacked.
import SwiftUI

@main
struct WFDTransferApp: App {
    @StateObject private var model = TransferModel()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(model)
        }
    }
}
