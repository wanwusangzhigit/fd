// swift-tools-version:5.9
//
// WFDTransfer — Swift implementation of the FDFT v2 binary protocol
// for iOS / iPadOS / macOS / Linux.
//
// The `FDFTProtocol` library is platform-agnostic Swift that compiles
// on every Swift target. It uses:
//
//   * `Foundation` for file I/O and UUID handling
//   * `CryptoKit` (Apple platforms) or `Crypto` (swift-crypto on Linux)
//     for SHA-256
//   * `Network` (Apple platforms) for TCP via NWListener / NWConnection
//
// On Linux the TCP / Bonjour paths are stubbed out — the library still
// builds so the protocol tests run there.
import PackageDescription

let package = Package(
    name: "WFDTransfer",
    platforms: [
        .iOS(.v17),
        .macOS(.v14),
    ],
    products: [
        .library(name: "FDFTProtocol", targets: ["FDFTProtocol"]),
        .executable(name: "wfd-cli", targets: ["WFDCLI"]),
    ],
    dependencies: [
        // swift-crypto gives us a CryptoKit-compatible API on Linux so the
        // protocol layer hashes identically on every platform.
        .package(url: "https://github.com/apple/swift-crypto.git", from: "3.0.0"),
    ],
    targets: [
        .target(
            name: "FDFTProtocol",
            dependencies: [
                .product(name: "Crypto", package: "swift-crypto"),
            ]
        ),
        .executableTarget(
            name: "WFDCLI",
            dependencies: ["FDFTProtocol"]
        ),
        .testTarget(
            name: "FDFTProtocolTests",
            dependencies: ["FDFTProtocol"]
        ),
    ]
)
