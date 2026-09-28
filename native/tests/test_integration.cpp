// test_integration.cpp — end-to-end TCP transfer test.
//
// Boots a `tcp::TcpListener` on an ephemeral port, sends a synthetic file
// through `tcp::SendFile`, and verifies:
//
//   * the saved file exists and matches the source byte-for-byte;
//   * the receiver computed the same SHA-256 as the sender;
//   * `integrityVerified == true` on the ReceivedFile.
//
// Then re-runs the same scenario with `includeHashTrailer=false` to ensure
// the receiver gracefully handles the older Android-compatible mode.
#include "Hash.h"
#include "Platform.h"
#include "TcpTransport.h"
#include "Util.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <condition_variable>
#include <string>
#include <thread>
#include <vector>

using namespace wfd;

namespace {

std::string TempDir() {
    return wfd::platform::TempDir();
}

std::vector<uint8_t> ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(in),
             std::istreambuf_iterator<char>() };
}

void WriteFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

struct RecvCapture {
    std::mutex m;
    std::condition_variable cv;
    bool got = false;
    tcp::ReceivedFile rf;
};

void RunOneTransfer(bool includeHash, int64_t size) {
    std::string srcDir = TempDir();
    std::string dstDir = TempDir();

    std::vector<uint8_t> payload(size);
    // Pattern that's deterministic but varied.
    for (int64_t i = 0; i < size; ++i)
        payload[static_cast<size_t>(i)] = static_cast<uint8_t>((i * 31 + 7) & 0xFF);
    std::string srcPath = platform::JoinPath(srcDir, "source.bin");
    WriteFile(srcPath, payload);
    std::string expectedHash = hash::Sha256Hex(payload.data(), payload.size());

    RecvCapture cap;
    tcp::TcpListener listener;
    bool started = listener.Start(0, dstDir, [&](const tcp::ReceivedFile& rf) {
        std::lock_guard<std::mutex> lk(cap.m);
        cap.rf = rf;
        cap.got = true;
        cap.cv.notify_all();
    });
    assert(started);
    int port = listener.port();

    auto res = tcp::SendFile("127.0.0.1", port, srcPath, "source.bin",
                             nullptr, includeHash);
    assert(res.ok);
    assert(res.bytesSent == size);

    if (includeHash) {
        assert(!res.senderHash.empty());
        assert(res.senderHash == expectedHash);
    } else {
        assert(res.senderHash.empty());
    }

    // Wait up to 5 s for the receiver callback.
    {
        std::unique_lock<std::mutex> lk(cap.m);
        cap.cv.wait_for(lk, std::chrono::seconds(5), [&] { return cap.got; });
    }
    assert(cap.got);
    assert(cap.rf.errorMessage.empty());
    assert(cap.rf.fileName == "source.bin");
    assert(cap.rf.fileSize == size);

    auto got = ReadFile(cap.rf.savedPath);
    assert(got.size() == static_cast<size_t>(size));
    assert(got == payload);

    if (includeHash) {
        assert(cap.rf.integrityVerified);
        assert(cap.rf.receivedHash == expectedHash);
    } else {
        assert(cap.rf.receivedHash.empty());
    }

    listener.Stop();
}

void TestSmallWithHash()      { RunOneTransfer(true,  16); }
void TestSmallWithoutHash()   { RunOneTransfer(false, 16); }
void TestMediumWithHash()     { RunOneTransfer(true,  256 * 1024); }
void TestEmptyFile()          { RunOneTransfer(true,  0); }

} // namespace

int main() {
    platform::InitNetworking();
    TestSmallWithHash();
    TestSmallWithoutHash();
    TestMediumWithHash();
    TestEmptyFile();
    platform::ShutdownNetworking();
    std::printf("test_integration: ALL OK\n");
    return 0;
}
