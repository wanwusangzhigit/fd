// FileSink.h — writes an incoming file body to disk safely.
//
// Both `tcp::TcpListener` and `bt::BluetoothListener` end up doing the same
// dance: pick a unique filename in the receive dir, open it, stream bytes
// off the socket while reporting progress, and report either the saved
// path or an error message back to the caller. FileSink factors that out
// so neither transport has to reinvent it.
//
// `ReadFn` is any callable `int(void*, size_t)` returning bytes read or < 0
// on error, identical to the one passed to `proto::ReadExact`.
#pragma once

#include "Platform.h"
#include <functional>
#include <string>

namespace wfd {
namespace sink {

struct WriteOutcome {
    bool ok = false;
    std::string savedPath;  // populated on success
    std::string error;      // populated on failure
    int64_t bytesWritten = 0;
};

// Streams `size` bytes from `readFn` into a file under `receiveDir`.
// `originalName` is sanitized and uniquified if it already exists.
// `onProgress(transferred, total)` is called every chunk (may be null).
//
// `readFn` is invoked through `proto::ReadExact` so partial reads are
// accumulated automatically; an EOF or read error surfaces in `outcome.error`.
template <class ReadFn>
WriteOutcome StreamToFile(ReadFn readFn,
                          const std::string& receiveDir,
                          const std::string& originalName,
                          int64_t size,
                          std::function<void(int64_t, int64_t)> onProgress = {});

// Resolves `originalName` to a writable, non-colliding path inside
// `receiveDir`. Sanitizes illegal characters and appends " (1)", " (2)", …
// as needed. Returns empty string on failure.
std::string ResolveUniquePath(const std::string& receiveDir,
                              const std::string& originalName);

} // namespace sink
} // namespace wfd

// ===== Inline template definition ==========================================

#include "Protocol.h"
#include "Util.h"

#include <cstdio>
#include <fstream>
#include <vector>

namespace wfd {
namespace sink {

inline std::string ResolveUniquePath(const std::string& receiveDir,
                                     const std::string& originalName) {
    std::string safe = util::SanitizeFilename(originalName);
    if (safe.empty()) safe = "received.bin";

    std::string base = platform::JoinPath(receiveDir, safe);
    if (!platform::IsReadableFile(base)) return base;

    std::string stem = safe, ext;
    auto dot = safe.find_last_of('.');
    if (dot != std::string::npos) {
        stem = safe.substr(0, dot);
        ext = safe.substr(dot);
    }
    for (int i = 1; i < 10000; ++i) {
        std::string candidate = stem + " (" + std::to_string(i) + ")" + ext;
        base = platform::JoinPath(receiveDir, candidate);
        if (!platform::IsReadableFile(base)) return base;
    }
    return {}; // give up
}

template <class ReadFn>
WriteOutcome StreamToFile(ReadFn readFn,
                          const std::string& receiveDir,
                          const std::string& originalName,
                          int64_t size,
                          std::function<void(int64_t, int64_t)> onProgress) {
    WriteOutcome out;
    if (size < 0) { out.error = "negative size"; return out; }

    std::string path = ResolveUniquePath(receiveDir, originalName);
    if (path.empty()) { out.error = "could not resolve output path"; return out; }

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { out.error = "cannot open output file: " + path; return out; }

    constexpr size_t kBuffer = 64 * 1024;
    std::vector<char> buf(kBuffer);
    int64_t got = 0;
    while (got < size) {
        size_t want = static_cast<size_t>(
            size - got < static_cast<int64_t>(buf.size()) ? size - got : buf.size());
        try {
            proto::ReadExact(readFn, buf.data(), want);
        } catch (const std::exception& e) {
            f.close();
            std::remove(path.c_str());
            out.error = std::string("recv: ") + e.what();
            return out;
        }
        f.write(buf.data(), want);
        got += static_cast<int64_t>(want);
        if (onProgress) onProgress(got, size);
    }
    f.flush();
    f.close();
    out.ok = true;
    out.savedPath = path;
    out.bytesWritten = got;
    return out;
}

} // namespace sink
} // namespace wfd
