# WFDTransfer iOS / iPadOS

A universal SwiftUI app for iPhone and iPad that speaks the FDFT v2 binary
file-transfer protocol — wire-compatible with the Android app and the
native C++ desktop client.

## Layout

```
ios/
├── Package.swift                      Swift Package (library + CLI + tests)
├── project.yml                        XcodeGen manifest for the iOS app
├── Sources/
│   ├── FDFTProtocol/                  Cross-platform Swift library
│   │   ├── Protocol.swift             FDFT v2 binary framing (KIND=1/2/3)
│   │   ├── SHA256.swift               SHA-256 via CryptoKit / swift-crypto
│   │   ├── TCPTransport.swift         NWConnection-based send
│   │   ├── Discovery.swift            NWListener + Bonjour NetService
│   │   └── FileSink.swift             Receive-file writer
│   ├── WFDCLI/
│   │   └── main.swift                 CLI driver (cross-platform test target)
│   └── WFDTransferApp/                SwiftUI iOS app
│       ├── WFDTransferApp.swift       Entry point
│       ├── TransferModel.swift        Bridge between FDFTProtocol + SwiftUI
│       ├── ContentView.swift          Adaptive iPhone/iPad root
│       ├── ReceiveView.swift          Listener toggle + received list
│       ├── SendView.swift             File picker + peer picker + queue
│       ├── Info.plist
│       └── Assets.xcassets/
└── Tests/FDFTProtocolTests/           Protocol / Hash / FileSink unit tests
```

## Transports supported on iOS

| Channel | Used | Notes |
|---------|------|-------|
| **TCP LAN** (Network.framework) | ✅ | Same FDFT v2 framing as Android + desktop; compatible both ways |
| **Bonjour** (`NetService` + `NetServiceBrowser`) | ✅ | Service type `_wfd._tcp.`; desktop UDP beacon peers can also be detected by adding `dns_sd` to the C++ side |
| Bluetooth RFCOMM | ❌ | iOS forbids raw RFCOMM sockets |
| Wi-Fi Direct | ❌ | iOS does not expose Wi-Fi P2P APIs |
| MultipeerConnectivity | ❌ (not implemented) | iOS ↔ iOS-only; not needed for cross-platform use |

## Build (without Xcode — Linux / GitHub Actions)

The pure-Swift `FDFTProtocol` library builds on Linux and has unit tests
that run there:

```bash
cd ios
swift build           # build FDFTProtocol + wfd-cli
swift test            # run all unit tests
```

Requires Swift 5.9+ and `libcurl` / OpenSSL headers (swift-crypto pulls
them in transitively). CI runs this on Ubuntu.

## Build the iOS app (Xcode only)

You need a Mac with Xcode 15+ and XcodeGen:

```bash
brew install xcodegen
cd ios
xcodegen generate
open WFDTransfer.xcodeproj
# ⌘R to run on the iOS Simulator or a connected device
```

## Cross-platform interoperability

| Sender             | Receiver           | Transport | Verified |
|--------------------|--------------------|-----------|----------|
| Android (Kotlin)   | Native C++ desktop | TCP       | ✅ |
| Native C++ desktop | Android            | TCP       | ✅ |
| Native C++ desktop | Native C++ desktop | TCP       | ✅ |
| Native C++ desktop | iOS                | TCP       | ✅ (manual) |
| iOS                | Native C++ desktop | TCP       | ✅ (manual) |
| iOS                | Android            | TCP       | ✅ (manual) |
| Android            | iOS                | TCP       | ✅ (manual) |

When the sender is iOS / desktop and the receiver is iOS / desktop, an
optional SHA-256 trailer (`KIND=3`) is appended and verified. When the
receiver is Android, the trailer is silently ignored (the Android code
only reads `size` body bytes).
