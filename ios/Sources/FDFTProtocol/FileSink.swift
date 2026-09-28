// FileSink.swift — writes an incoming file body to disk safely.
//
// Mirrors `FileSink.h` on the C++ side: pick a unique filename under
// `receiveDir`, stream bytes off the receive callback, tee them into
// the optional SHA-256 hasher. Returns either the saved path or an
// error string.
import Foundation

public struct SinkOutcome: Equatable {
    public let ok: Bool
    public let path: String?
    public let error: String?
    public let bytesWritten: Int64
    public init(ok: Bool, path: String? = nil, error: String? = nil,
                bytesWritten: Int64 = 0) {
        self.ok = ok; self.path = path; self.error = error
        self.bytesWritten = bytesWritten
    }
}

public enum FileSinkUtil {
    /// Sanitize a filename the same way `util::SanitizeFilename` does on
    /// the C++ side: replace illegal chars with `_`, strip leading/trailing
    /// dots and spaces, cap at 240 chars.
    public static func sanitize(_ raw: String) -> String {
        let forbidden: Set<Character> = [
            "/", "\\", ":", "*", "?", "\"", "<", ">", "|",
        ]
        var cleaned = ""
        cleaned.reserveCapacity(raw.count)
        for c in raw {
            if forbidden.contains(c) || c.asciiValue.map({ $0 < 0x20 }) == .some(true) {
                cleaned.append("_")
            } else {
                cleaned.append(c)
            }
        }
        // Trim leading/trailing dots and spaces.
        while cleaned.first == "." || cleaned.first == " " {
            cleaned.removeFirst()
        }
        while cleaned.last == "." || cleaned.last == " " {
            cleaned.removeLast()
        }
        if cleaned.count > 240 { cleaned = String(cleaned.prefix(240)) }
        if cleaned.isEmpty { cleaned = "received.bin" }
        return cleaned
    }

    /// Pick a unique filename inside `dir` that doesn't already exist,
    /// appending " (1)", " (2)", ... before the extension.
    public static func resolveUnique(dir: String, name: String) -> String {
        let safe = sanitize(name)
        let base = (dir as NSString).appendingPathComponent(safe)
        if !FileManager.default.fileExists(atPath: base) { return base }
        let stem: String
        let ext: String
        if let dotIdx = safe.lastIndex(of: ".") {
            stem = String(safe[..<dotIdx])
            ext = String(safe[dotIdx...])
        } else {
            stem = safe
            ext = ""
        }
        for i in 1..<10_000 {
            let candidate = "\(stem) (\(i))\(ext)"
            let path = (dir as NSString).appendingPathComponent(candidate)
            if !FileManager.default.fileExists(atPath: path) { return path }
        }
        return base
    }
}

public final class FileSink {
    public let receiveDir: String
    public init(receiveDir: String) { self.receiveDir = receiveDir }

    public struct Outcome {
        public let path: String
        public let bytesWritten: Int64
    }

    /// Stream `size` bytes from `next`, writing them to a uniquely-named
    /// file under `receiveDir`. Calls `tee` with each chunk (used by
    /// callers to incrementally compute SHA-256).
    public func stream(
        from next: () async throws -> Data?,
        size: Int64,
        tee: ((Data) -> Void)? = nil
    ) async throws -> Outcome {
        try FileManager.default.createDirectory(atPath: receiveDir,
                                                withIntermediateDirectories: true)
        let path = FileSinkUtil.resolveUnique(dir: receiveDir,
                                              name: "incoming.bin")
        guard FileManager.default.createFile(atPath: path, contents: nil) else {
            throw ProtocolError.ioError("cannot create \(path)")
        }
        let handle = try FileHandle(forWritingTo: URL(fileURLWithPath: path))
        defer { try? handle.close() }

        var got: Int64 = 0
        while got < size {
            let chunk = try await next()
            guard let data = chunk, !data.isEmpty else {
                throw ProtocolError.eof
            }
            try handle.write(contentsOf: data)
            if let tee = tee { tee(data) }
            got += Int64(data.count)
        }
        try handle.close()
        return Outcome(path: path, bytesWritten: got)
    }

    /// Resolve a target path for `originalName` without writing anything;
    /// useful when callers want the header info first.
    public func resolveTarget(originalName: String) -> String {
        return FileSinkUtil.resolveUnique(dir: receiveDir, name: originalName)
    }
}
