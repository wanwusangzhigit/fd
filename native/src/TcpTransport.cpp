// TcpTransport.cpp — TCP send / receive implementations.
#include "TcpTransport.h"
#include "FileSink.h"
#include "Hash.h"
#include "Protocol.h"
#include "Util.h"
#include "Progress.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>

#ifdef WFD_WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
   using socklen_t = int;
   using ssize_t = SSIZE_T;
#else
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace wfd {
namespace tcp {

namespace {

constexpr size_t kBuffer = 64 * 1024;
constexpr int kConnectTimeoutMs = 15000;

platform::UniqueSocket Connect(const std::string& host, int port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    auto portStr = std::to_string(port);
    int rc =
#ifdef WFD_WIN32
        getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res);
#else
        getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res);
#endif
    if (rc != 0 || res == nullptr) {
        return {};
    }
    std::unique_ptr<addrinfo, void(*)(addrinfo*)> guard(res, freeaddrinfo);
    for (auto* a = res; a; a = a->ai_next) {
        int fd = static_cast<int>(::socket(a->ai_family, a->ai_socktype, a->ai_protocol));
        if (fd < 0) continue;
        if (::connect(fd, a->ai_addr, static_cast<socklen_t>(a->ai_addrlen)) == 0) {
            return platform::UniqueSocket{fd};
        }
        platform::CloseSocket(fd);
    }
    return {};
}

// Receive-loop helpers are now provided by FileSink.h + Protocol.h.
// We keep only the Connect() helper here.

} // namespace

// ------- SendFile ---------------------------------------------------------

SendResult SendFile(const std::string& host, int port,
                     const std::string& filePath,
                     std::function<void(int64_t, int64_t)> onProgress) {
    return SendFile(host, port, filePath, platform::Basename(filePath),
                    std::move(onProgress), true);
}

SendResult SendFile(const std::string& host, int port,
                     const std::string& filePath,
                     const std::string& forcedName,
                     std::function<void(int64_t, int64_t)> onProgress,
                     bool includeHashTrailer) {
    SendResult res;
    int64_t size = platform::FileSize(filePath);
    if (size < 0) {
        res.error = "cannot stat: " + filePath;
        return res;
    }
    std::ifstream in(filePath, std::ios::binary);
    if (!in) { res.error = "cannot open: " + filePath; return res; }

    auto sock = Connect(host, port);
    if (!sock) {
        res.error = "cannot connect to " + host + ":" + std::to_string(port);
        return res;
    }

    auto writeFn = [fd = sock.get()](const void* b, size_t n) -> int {
        return ::send(fd, static_cast<const char*>(b), static_cast<int>(n), 0);
    };
    try {
        proto::WriteFileHeader(writeFn, forcedName, size);
    } catch (const std::exception& e) {
        res.error = std::string("header write: ") + e.what();
        return res;
    }

    // Body — hash incrementally as we stream.
    hash::Sha256Stream h;
    std::vector<char> buf(kBuffer);
    int64_t sent = 0;
    while (sent < size) {
        size_t want = static_cast<size_t>(size - sent < static_cast<int64_t>(buf.size())
                                          ? size - sent : buf.size());
        in.read(buf.data(), want);
        auto got = static_cast<size_t>(in.gcount());
        if (got == 0) { res.error = "source EOF early"; return res; }
        if (includeHashTrailer) h.Update(buf.data(), got);
        try {
            proto::WriteAll(writeFn, buf.data(), got);
        } catch (const std::exception& e) {
            res.error = std::string("body write: ") + e.what();
            return res;
        }
        sent += static_cast<int64_t>(got);
        if (onProgress) onProgress(sent, size);
    }

    if (includeHashTrailer) {
        auto digest = h.Finish();
        try {
            proto::WriteHashTrailer(writeFn, proto::kHashAlgoSha256,
                                    digest.data(),
                                    static_cast<uint8_t>(digest.size()));
        } catch (const std::exception& e) {
            res.error = std::string("hash trailer write: ") + e.what();
            return res;
        }
        char hex[65];
        static const char* kHex = "0123456789abcdef";
        for (size_t i = 0; i < digest.size(); ++i) {
            hex[i*2]   = kHex[digest[i] >> 4];
            hex[i*2+1] = kHex[digest[i] & 0x0F];
        }
        hex[64] = '\0';
        res.senderHash = hex;
    }

    res.ok = true;
    res.bytesSent = sent;
    return res;
}

SendResult SendFile(const std::string& host, int port,
                     const std::string& filePath,
                     const std::string& forcedName,
                     std::function<void(int64_t, int64_t)> onProgress) {
    return SendFile(host, port, filePath, forcedName, std::move(onProgress), true);
}

// ------- TcpListener ------------------------------------------------------

TcpListener::~TcpListener() { Stop(); }

bool TcpListener::Start(int port, const std::string& receiveDir, ReceivedCallback cb) {
    if (running_.load()) return true;
    int fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (fd < 0) return false;
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        platform::CloseSocket(fd);
        return false;
    }
    if (::listen(fd, 8) != 0) {
        platform::CloseSocket(fd);
        return false;
    }
    // Read back the bound port (in case caller passed 0).
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
        port_ = ntohs(addr.sin_port);
    } else {
        port_ = port;
    }
    listenSock_ = platform::UniqueSocket{fd};
    running_ = true;
    stopRequested_ = false;
    thread_ = std::make_unique<ThreadHandle>();
    thread_->t = std::thread([this, fd, receiveDir, cb = std::move(cb)]() mutable {
        Run(fd, receiveDir, std::move(cb));
    });
    return true;
}

void TcpListener::Stop() {
    if (!running_.load()) return;
    stopRequested_ = true;
    // Shutdown before close so accept() reliably unblocks on Linux.
    int fd = listenSock_.get();
    if (fd >= 0) {
#ifdef WFD_WIN32
        ::shutdown(fd, SD_BOTH);
#else
        ::shutdown(fd, SHUT_RDWR);
#endif
    }
    listenSock_.reset();
    if (thread_ && thread_->t.joinable()) thread_->t.join();
    thread_.reset();
    running_ = false;
}

void TcpListener::Run(int listenFd, std::string receiveDir, ReceivedCallback cb) {
    while (!stopRequested_.load()) {
        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        int cli = static_cast<int>(::accept(listenFd, reinterpret_cast<sockaddr*>(&peer), &len));
        if (cli < 0) {
            if (stopRequested_.load()) break;
            continue; // soft error, keep looping
        }
        // Detach handler into its own thread so accept loop keeps going.
        std::thread([cli, peer, receiveDir, cb] {
            platform::UniqueSocket cliSock{cli};
            char ipBuf[INET_ADDRSTRLEN] = "?";
            inet_ntop(AF_INET, &peer.sin_addr, ipBuf, sizeof(ipBuf));
            std::string peerHost = ipBuf;

            ReceivedFile rf;
            rf.peerHost = peerHost;
            try {
                // First, read the frame prefix (6 bytes) + FILE payload header.
                // We buffer those bytes so we can replay them into proto::ReadExact
                // (the helpers expect to read everything from the read callback).
                auto rawRead = [cli](void* b, size_t n) -> int {
                    return ::recv(cli, static_cast<char*>(b), static_cast<int>(n), 0);
                };
                uint8_t kind = proto::ReadFramePrefix(rawRead);
                if (kind != proto::kKindFile) {
                    rf.errorMessage = "expected FILE frame, got kind=" + std::to_string(kind);
                    cb(rf);
                    return;
                }
                auto h = proto::ReadFilePayload(rawRead);
                rf.fileName = h.name;
                rf.fileSize = h.size;

                // Stream body to disk through FileSink, hashing as we go.
                hash::Sha256Stream fileHash;
                auto hashedRead = [&](void* b, size_t n) -> int {
                    int r = ::recv(cli, static_cast<char*>(b), static_cast<int>(n), 0);
                    if (r > 0) fileHash.Update(b, static_cast<size_t>(r));
                    return r;
                };
                auto out = sink::StreamToFile(hashedRead, receiveDir,
                                              h.name, h.size, nullptr);
                if (!out.ok) {
                    rf.errorMessage = out.error;
                    cb(rf);
                    return;
                }
                rf.savedPath = out.savedPath;

                // Optional HASH trailer — native-to-native only.
                // Peek up to 6 bytes; if it doesn't look like a frame prefix,
                // push back into the kernel via MSG_PEEK is impossible after
                // consume, so we use a short SO_RCVTIMEO and treat EOF /
                // timeout as "no trailer".
                timeval tv{0, 0};
                tv.tv_sec = 0; tv.tv_usec = 300 * 1000; // 300 ms
                ::setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO,
                             reinterpret_cast<const char*>(&tv), sizeof(tv));
                uint8_t peek[6];
                int got = ::recv(cli, reinterpret_cast<char*>(peek), 6, 0);
                if (got == 6 &&
                    std::memcmp(peek, proto::kMagic, 4) == 0 &&
                    peek[4] == proto::kVersion &&
                    peek[5] == proto::kKindHash) {
                    uint8_t algo, digestLen;
                    if (::recv(cli, reinterpret_cast<char*>(&algo), 1, MSG_WAITALL) == 1 &&
                        ::recv(cli, reinterpret_cast<char*>(&digestLen), 1, MSG_WAITALL) == 1 &&
                        digestLen > 0 && digestLen <= proto::kMaxDigestLen) {
                        std::vector<uint8_t> digest(digestLen);
                        if (::recv(cli, reinterpret_cast<char*>(digest.data()),
                                   digestLen, MSG_WAITALL) == digestLen) {
                            auto computed = fileHash.Finish();
                            rf.integrityVerified =
                                !std::memcmp(computed.data(), digest.data(),
                                             std::min(computed.size(), digest.size()));
                            if (!rf.integrityVerified) {
                                rf.errorMessage = "SHA-256 mismatch";
                            }
                            char hex[65];
                            static const char* kHex = "0123456789abcdef";
                            for (size_t i = 0; i < digest.size(); ++i) {
                                hex[i*2]   = kHex[digest[i] >> 4];
                                hex[i*2+1] = kHex[digest[i] & 0x0F];
                            }
                            hex[64] = '\0';
                            rf.receivedHash = hex;
                        }
                    }
                }
            } catch (const std::exception& e) {
                rf.errorMessage = std::string("recv frame: ") + e.what();
            }
            cb(rf);
        }).detach();
    }
}

} // namespace tcp
} // namespace wfd
