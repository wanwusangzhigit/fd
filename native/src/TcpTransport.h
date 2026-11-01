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

// Progress of an inbound transfer, reported while it is still on the wire so a
// UI can show live receive progress instead of one row appearing at the end.
// `connId` is stable for the duration of one connection and is echoed in
// `ReceivedFile::connId`, which lets a front-end correlate its own row.
struct RecvUpdate {
    enum class Stage : uint8_t { Started, Progress };
    Stage stage = Stage::Started;
    uint64_t connId = 0;
    std::string peerHost;
    std::string fileName;   // set on Started; empty on Progress
    int64_t done = 0;
    int64_t total = 0;
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
    uint64_t connId = 0;
};

// Callback signature used by TcpListener.
using ReceivedCallback = std::function<void(const ReceivedFile&)>;
using RecvUpdateCallback = std::function<void(const RecvUpdate&)>;

// Stream a single file to `host:port`. `onProgress(transferred, total)` is
// invoked periodically (every ~256 KB). The 4-arg variant defaults
// `includeHashTrailer` to true so native-to-native transfers are verified.
SendResult SendFile(const std::string& host, int port,
                    const std::string& filePath,
                    std::function<void(int64_t, int64_t)> onProgress = {});

// Lowest-level variant. `includeHashTrailer` may be suppressed when talking to
// the Android app, which does not read a trailer. `cancelToken`, when given, is
// polled between chunks so a UI can abort a transfer in flight; the result then
// carries `error == "canceled"` and the byte count reached so far.
SendResult SendFile(const std::string& host, int port,
                    const std::string& filePath,
                    const std::string& forcedName,
                    std::function<void(int64_t, int64_t)> onProgress = {},
                    bool includeHashTrailer = true,
                    const std::atomic<bool>* cancelToken = nullptr);

// RAII TCP listener. Start() returns immediately; spawns an accept thread.
// Stop() joins the thread.
class TcpListener {
public:
    TcpListener() = default;
    ~TcpListener();
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    // Starts listening on `port`. Returns false on bind failure.
    // `onRecv` is optional; when supplied it reports Started / Progress for
    // every inbound connection before the terminal `cb` fires.
    bool Start(int port, const std::string& receiveDir, ReceivedCallback cb,
               RecvUpdateCallback onRecv = {});

    // Stops the accept thread (if any). Idempotent.
    //
    // Note: per-connection handlers are detached (an accept loop must not wait
    // on a slow peer), so `cb` may still fire briefly after Stop() returns.
    // Callbacks must therefore capture nothing that Stop() invalidates — hold
    // a shared_ptr to your state by value, never a raw handle.
    void Stop();

    bool IsRunning() const { return running_.load(); }
    int port() const { return port_; }

private:
    void Run(int listenFd, std::string receiveDir, ReceivedCallback cb,
             RecvUpdateCallback onRecv);
    platform::UniqueSocket listenSock_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    uint64_t nextConnId_ = 1;  // touched by the accept thread only
    int port_ = 0;
    struct ThreadHandle { std::thread t; };
    std::unique_ptr<ThreadHandle> thread_;
};

} // namespace tcp
} // namespace wfd
