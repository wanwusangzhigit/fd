// TcpTransport.h — TCP send (client) and receive (server) entry points.
//
// Mirrors the wire protocol exactly as the Android `FileTransferService`:
// - Default file port: 8988 (Constants.WIFI_FILE_PORT)
// - Server runs forever accepting concurrent connections.
// - Each accepted connection delivers exactly one file using FDFT v2.
//
// Two ways to use:
//   1) Low-level helpers: `SendFile(host, port, path)` and
//      `ListenForever(port, onReceived)`.
//   2) RAII listener: `TcpListener listener; listener.Start(port, cb); ... listener.Stop();`
#pragma once

#include "Platform.h"
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace wfd {
namespace tcp {

constexpr int kDefaultFilePort = 8988;
constexpr int kDefaultRegisterPort = 8989;

// Outcome of SendFile.
struct SendResult {
    bool ok = false;
    std::string error;             // populated iff !ok
    int64_t bytesSent = 0;
    std::string senderHash;        // SHA-256 hex if a trailer was emitted
};

// Callback invoked by ListenForever for each successfully accepted peer.
//   peerHost — IP address of the connected peer.
//   fileName / fileSize / payloadPath — frame info + path of the saved file
//   errorMessage — non-empty if reception failed midway (file may be partial).
struct ReceivedFile {
    std::string peerHost;
    std::string fileName;
    int64_t fileSize = 0;
    std::string savedPath;
    std::string errorMessage; // empty on success
    std::string receivedHash; // SHA-256 hex from trailer, if any
    bool integrityVerified = false;
};

// Callback signature used by TcpListener.
using ReceivedCallback = std::function<void(const ReceivedFile&)>;

// Stream a single file to `host:port`. `onProgress(transferred, total)` is
// invoked periodically (every ~256 KB). The 4-arg variant defaults
// `includeHashTrailer` to true so native-to-native transfers are verified.
SendResult SendFile(const std::string& host, int port,
                    const std::string& filePath,
                    std::function<void(int64_t, int64_t)> onProgress = {});

// Convenience: takes filename override (otherwise derived from path).
SendResult SendFile(const std::string& host, int port,
                    const std::string& filePath,
                    const std::string& forcedName,
                    std::function<void(int64_t, int64_t)> onProgress);

// Lowest-level variant: lets caller suppress the optional SHA-256 trailer
// (e.g. when talking to the Android app which doesn't expect it).
SendResult SendFile(const std::string& host, int port,
                    const std::string& filePath,
                    const std::string& forcedName,
                    std::function<void(int64_t, int64_t)> onProgress,
                    bool includeHashTrailer);

// RAII TCP listener. Start() returns immediately; spawns an accept thread.
// Stop() joins the thread.
class TcpListener {
public:
    TcpListener() = default;
    ~TcpListener();
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    // Starts listening on `port`. Returns false on bind failure.
    bool Start(int port, const std::string& receiveDir, ReceivedCallback cb);

    // Stops the accept thread (if any). Idempotent.
    void Stop();

    bool IsRunning() const { return running_.load(); }
    int port() const { return port_; }

private:
    void Run(int listenFd, std::string receiveDir, ReceivedCallback cb);
    platform::UniqueSocket listenSock_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    int port_ = 0;
    struct ThreadHandle { std::thread t; };
    std::unique_ptr<ThreadHandle> thread_;
};

} // namespace tcp
} // namespace wfd
