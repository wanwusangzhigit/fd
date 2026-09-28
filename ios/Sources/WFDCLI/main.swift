// wfd-cli — minimal CLI front-end for the FDFTProtocol library.
//
// Useful for two purposes:
//   * Verifying that the Swift package builds and runs on Linux via
//     `swift run wfd-cli --help`. CI does this on every PR.
//   * Driving the protocol from a shell during local debugging on macOS.
//
// iOS / iPadOS UI is in `Sources/WFDTransferApp/` — built only by Xcode
// against the iOS SDK; not part of this executable.
import Foundation
import FDFTProtocol

@main
struct WFDCLI {
    static func main() async {
        let args = CommandLine.arguments
        guard args.count > 1 else { printUsage(); exit(EXIT_FAILURE) }

        switch args[1] {
        case "hash":
            guard args.count > 2 else { printUsage(); exit(EXIT_FAILURE) }
            for path in args.dropFirst(2) {
                guard let data = try? Data(contentsOf: URL(fileURLWithPath: path)) else {
                    FileHandle.standardError.write(Data("error: cannot read \(path)\n".utf8))
                    continue
                }
                print("\(HashUtil.sha256Hex(data))  \(path)")
            }
        case "version":
            print("wfd-cli 1.0.0")
        case "help", "-h", "--help":
            printUsage()
        default:
            print("unknown command: \(args[1])")
            printUsage()
            exit(EXIT_FAILURE)
        }
    }

    static func printUsage() {
        let txt = """
        wfd-cli — FDFT protocol utility

        Usage:
          wfd-cli hash <file> [<file> ...]    Compute SHA-256 (same hex format as sha256sum)
          wfd-cli version                     Print version
          wfd-cli help                        Show this help
        """
        print(txt)
    }
}
