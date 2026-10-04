// BluetoothTransport.cpp — platform-specific RFCOMM implementation.
#include "BluetoothTransport.h"
#include "FileSink.h"
#include "Hash.h"
#include "Protocol.h"
#include "Platform.h"

#include <atomic>
#include <cstring>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>

#ifdef WFD_WIN32
// --- Windows Win32 Bluetooth -------------------------------------------------
#  include <winsock2.h>
#  include <ws2bth.h>
#  include <bthsdpdef.h>
#  include <bluetoothapis.h>
#  pragma comment(lib, "bthprops.lib")
#  pragma comment(lib, "ws2_32.lib")

namespace {
using BtSocket = SOCKET;
constexpr BtSocket kBtInvalid = INVALID_SOCKET;

// Convert a UTF-16 wide Bluetooth device name into a UTF-8 `std::string`.
// Uses WideCharToMultiByte rather than wcstombs_s because the latter has
// a 5-argument signature under MinGW that's easy to call incorrectly.
std::string WideToUtf8(const wchar_t* wide) {
    if (!wide) return {};
    int len = ::lstrlenW(wide);
    if (len == 0) return {};
    std::string out(static_cast<size_t>(len) * 4, '\0');
    int n = ::WideCharToMultiByte(CP_UTF8, 0, wide, len,
                                  out.data(), static_cast<int>(out.size()),
                                  nullptr, nullptr);
    if (n > 0) {
        out.resize(static_cast<size_t>(n));
        return out;
    }
    return {};
}

bool ParseUuid(const std::string& s, GUID* out) {
    // Accept canonical `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx`.
    std::string c;
    c.reserve(s.size());
    for (char ch : s) if (ch != '-') c.push_back(ch);
    if (c.size() != 32) return false;
    auto hx = [&](size_t i) -> int {
        char ch = c[i];
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    auto b = [&](size_t i) -> unsigned char {
        return static_cast<unsigned char>((hx(i) << 4) | hx(i + 1));
    };
    out->Data1 = (b(0) << 24) | (b(2) << 16) | (b(4) << 8) | b(6);
    out->Data2 = (b(8) << 8) | b(10);
    out->Data3 = (b(12) << 8) | b(14);
    for (int i = 0; i < 8; ++i) out->Data4[i] = b(16 + i * 2);
    return true;
}

std::string FormatBdaddr(const BTH_ADDR& a) {
    char buf[18];
    std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
        static_cast<int>((a >> 40) & 0xFF),
        static_cast<int>((a >> 32) & 0xFF),
        static_cast<int>((a >> 24) & 0xFF),
        static_cast<int>((a >> 16) & 0xFF),
        static_cast<int>((a >> 8) & 0xFF),
        static_cast<int>(a & 0xFF));
    return buf;
}

bool ParseBdaddr(const std::string& s, BTH_ADDR* out) {
    unsigned int b[6];
    if (std::sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x",
                    &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return false;
    BTH_ADDR a = 0;
    for (int i = 0; i < 6; ++i) a = (a << 8) | (b[i] & 0xFF);
    *out = a;
    return true;
}

} // namespace

#else
// --- Linux BlueZ ------------------------------------------------------------
#  include <bluetooth/bluetooth.h>
#  include <bluetooth/rfcomm.h>
#  include <bluetooth/hci.h>
#  include <bluetooth/hci_lib.h>
#  include <bluetooth/sdp.h>
#  include <bluetooth/sdp_lib.h>
#  include <unistd.h>

namespace {
using BtSocket = int;
constexpr BtSocket kBtInvalid = -1;

// RFCOMM raw sockets don't need UUID parsing — only SDP lookups do.
// We rely on channel iteration (1..30) to find the right service.

std::string FormatBdaddr(const bdaddr_t& a) {
    char buf[18];
    ba2str(&a, buf);
    return buf;
}

bool ParseBdaddr(const std::string& s, bdaddr_t* out) {
    return str2ba(s.c_str(), out) == 0;
}

} // namespace
#endif

namespace wfd {
namespace bt {

namespace {

constexpr size_t kBuffer = 16 * 1024;  // Bluetooth MTU is small.

// Receive-side plumbing now lives in FileSink.h / Protocol.h.

} // namespace

bool IsSupported() {
#if defined(WFD_HAVE_BLUEZ) || defined(WFD_WIN32)
    return true;
#else
    return false;
#endif
}

// -------- Discovery -------------------------------------------------------

std::vector<PeerDevice> EnumeratePairedDevices(size_t max) {
    std::vector<PeerDevice> result;
    (void)max;
#ifdef WFD_WIN32
    BLUETOOTH_DEVICE_SEARCH_PARAMS params{};
    params.dwSize = sizeof(params);
    params.fReturnAuthenticated = TRUE;
    params.fReturnRemembered = TRUE;
    params.fReturnConnected = TRUE;
    params.fReturnUnknown = FALSE;
    params.fIssueInquiry = FALSE;
    params.cTimeoutMultiplier = 0;
    BLUETOOTH_DEVICE_INFO info{};
    info.dwSize = sizeof(info);
    HANDLE h = BluetoothFindFirstDevice(&params, &info);
    if (h == nullptr) return result;
    do {
        PeerDevice p;
        p.address = FormatBdaddr(info.Address.ullLong);
        p.name = WideToUtf8(info.szName);
        result.push_back(std::move(p));
        if (max && result.size() >= max) break;
    } while (BluetoothFindNextDevice(h, &info));
    BluetoothFindDeviceClose(h);
#elif defined(WFD_HAVE_BLUEZ)
    // BlueZ exposes paired devices via the BlueZ D-Bus API; iterating them
    // from raw RFCOMM sockets is not directly possible. Fall back to active
    // inquiry results cached locally if any. For now we return an empty
    // list and rely on DiscoverDevices() for live discovery.
#endif
    return result;
}

int DiscoverDevices(std::function<void(const PeerDevice&)> onFound, int durationSec) {
    int count = 0;
#ifdef WFD_WIN32
    BLUETOOTH_DEVICE_SEARCH_PARAMS params{};
    params.dwSize = sizeof(params);
    params.fReturnAuthenticated = TRUE;
    params.fReturnRemembered = TRUE;
    params.fReturnUnknown = TRUE;
    params.fIssueInquiry = TRUE;
    params.cTimeoutMultiplier = static_cast<UCHAR>(std::max(1, std::min(durationSec, 48)));
    BLUETOOTH_DEVICE_INFO info{};
    info.dwSize = sizeof(info);
    HANDLE h = BluetoothFindFirstDevice(&params, &info);
    if (h == nullptr) return 0;
    do {
        PeerDevice p;
        p.address = FormatBdaddr(info.Address.ullLong);
        p.name = WideToUtf8(info.szName);
        onFound(p);
        ++count;
    } while (BluetoothFindNextDevice(h, &info));
    BluetoothFindDeviceClose(h);
#elif defined(WFD_HAVE_BLUEZ)
    int devId = hci_get_route(nullptr);
    int dd = hci_open_dev(devId);
    if (dd < 0) return 0;
    inquiry_info* info = nullptr;
    int flags = IREQ_CACHE_FLUSH;
    int maxRsp = 16, numRsp = hci_inquiry(devId, std::max(1, std::min(durationSec / 1, 12)),
                                          maxRsp, nullptr, &info, flags);
    close(dd);
    if (numRsp <= 0) return 0;
    for (int i = 0; i < numRsp; ++i) {
        PeerDevice p;
        p.address = FormatBdaddr(info[i].bdaddr);
        onFound(p);
        ++count;
    }
    free(info);
#endif
    return count;
}

// -------- Send ------------------------------------------------------------

SendResult SendFile(const std::string& peerAddress,
                    const std::string& filePath,
                    const std::string& forcedName,
                    std::function<void(int64_t, int64_t)> onProgress,
                    bool includeHashTrailer) {
    SendResult res;
#if defined(WFD_WIN32) || defined(WFD_HAVE_BLUEZ)
    int64_t size = platform::FileSize(filePath);
    if (size < 0) { res.error = "cannot stat: " + filePath; return res; }
    std::ifstream in(filePath, std::ios::binary);
    if (!in) { res.error = "cannot open: " + filePath; return res; }

    BtSocket sock = static_cast<BtSocket>(::socket(
#ifdef WFD_WIN32
        AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM
#else
        AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM
#endif
    ));
    if (sock == kBtInvalid) {
        res.error = "cannot create BT socket: " + platform::LastErrorString();
        return res;
    }

#ifdef WFD_WIN32
    GUID guid;
    if (!ParseUuid(kServiceUuid, &guid)) { res.error = "uuid parse"; return res; }
#else
    // uuid_t only used for SDP lookups; for raw RFCOMM we just iterate channels.
#endif

    int port = 0;
    bool connected = false;
#ifdef WFD_WIN32
    BTH_ADDR addr = 0;
    if (!ParseBdaddr(peerAddress, &addr)) { res.error = "bad address"; return res; }
    for (port = 1; port <= 30 && !connected; ++port) {
        SOCKADDR_BTH sa{};
        sa.addressFamily = AF_BTH;
        sa.btAddr = addr;
        sa.serviceClassId = guid;
        sa.port = port;
        if (::connect(sock, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0) {
            connected = true;
        }
    }
#else
    bdaddr_t addr{};
    if (!ParseBdaddr(peerAddress, &addr)) { res.error = "bad address"; return res; }
    for (port = 1; port <= 30 && !connected; ++port) {
        sockaddr_rc sa{};
        sa.rc_family = AF_BLUETOOTH;
        sa.rc_channel = static_cast<uint8_t>(port);
        bacpy(&sa.rc_bdaddr, &addr);
        if (::connect(sock, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0) {
            connected = true;
        }
    }
#endif
    if (!connected) {
        res.error = "cannot connect to " + peerAddress + " (no RFCOMM channel worked)";
#ifdef WFD_WIN32
        closesocket(sock);
#else
        close(sock);
#endif
        return res;
    }

    auto writeFn = [sock](const void* b, size_t n) -> int {
#ifdef WFD_WIN32
        return ::send(sock, static_cast<const char*>(b), static_cast<int>(n), 0);
#else
        return static_cast<int>(::write(sock, b, n));
#endif
    };
    try {
        proto::WriteFileHeader(writeFn, forcedName, size);
    } catch (const std::exception& e) {
        res.error = std::string("header: ") + e.what();
#ifdef WFD_WIN32
        closesocket(sock);
#else
        close(sock);
#endif
        return res;
    }
    hash::Sha256Stream h;
    std::vector<char> buf(kBuffer);
    int64_t sent = 0;
    while (sent < size) {
        size_t want = static_cast<size_t>(size - sent < static_cast<int64_t>(buf.size())
                                          ? size - sent : buf.size());
        in.read(buf.data(), want);
        auto got = static_cast<size_t>(in.gcount());
        if (got == 0) { res.error = "source EOF early"; break; }
        if (includeHashTrailer) h.Update(buf.data(), got);
        try { proto::WriteAll(writeFn, buf.data(), got); }
        catch (const std::exception& e) { res.error = std::string("body: ") + e.what(); break; }
        sent += static_cast<int64_t>(got);
        if (onProgress) onProgress(sent, size);
    }

    if (res.error.empty() && includeHashTrailer) {
        auto digest = h.Finish();
        try {
            proto::WriteHashTrailer(writeFn, proto::kHashAlgoSha256,
                                    digest.data(),
                                    static_cast<uint8_t>(digest.size()));
        } catch (const std::exception& e) {
            res.error = std::string("hash trailer: ") + e.what();
        }
        if (res.error.empty()) {
            char hex[65];
            static const char* kHex = "0123456789abcdef";
            for (size_t i = 0; i < digest.size(); ++i) {
                hex[i*2]   = kHex[digest[i] >> 4];
                hex[i*2+1] = kHex[digest[i] & 0x0F];
            }
            hex[64] = '\0';
            res.senderHash = hex;
        }
    }
#ifdef WFD_WIN32
    closesocket(sock);
#else
    close(sock);
#endif
    res.ok = res.error.empty();
    res.bytesSent = sent;
    return res;
#else
    res.error = "Bluetooth not compiled in";
    return res;
#endif
}

SendResult SendFile(const std::string& peerAddress,
                    const std::string& filePath,
                    const std::string& forcedName,
                    std::function<void(int64_t, int64_t)> onProgress) {
    return SendFile(peerAddress, filePath, forcedName, std::move(onProgress), true);
}

SendResult SendFile(const std::string& peerAddress,
                    const std::string& filePath,
                    std::function<void(int64_t, int64_t)> onProgress) {
    return SendFile(peerAddress, filePath, platform::Basename(filePath), std::move(onProgress), true);
}

// -------- Listener --------------------------------------------------------

BluetoothListener::~BluetoothListener() { Stop(); }

bool BluetoothListener::Start(const std::string& receiveDir, ReceivedCallback cb) {
#if defined(WFD_WIN32) || defined(WFD_HAVE_BLUEZ)
    if (running_.load()) return true;
    int fd = static_cast<int>(::socket(
#ifdef WFD_WIN32
        AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM
#else
        AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM
#endif
    ));
    if (fd < 0) return false;
    bool bound = false;
#ifndef WFD_WIN32
    bdaddr_t anyAddr{};
#endif
    for (int port = 1; port <= 30 && !bound; ++port) {
#ifdef WFD_WIN32
        SOCKADDR_BTH sa{};
        sa.addressFamily = AF_BTH;
        sa.btAddr = 0;
        sa.port = port;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0) bound = true;
#else
        sockaddr_rc sa{};
        sa.rc_family = AF_BLUETOOTH;
        bacpy(&sa.rc_bdaddr, &anyAddr);
        sa.rc_channel = static_cast<uint8_t>(port);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0) bound = true;
#endif
    }
    if (!bound) { platform::CloseSocket(fd); return false; }
    if (::listen(fd, 4) != 0) { platform::CloseSocket(fd); return false; }
    listenSock_ = platform::UniqueSocket{fd};
    running_ = true;
    stopRequested_ = false;
    thread_ = std::make_unique<ThreadHandle>();
    thread_->t = std::thread([this, fd, receiveDir, cb = std::move(cb)]() mutable {
        while (!stopRequested_.load()) {
#ifdef WFD_WIN32
            SOCKADDR_BTH peer{}; int len = sizeof(peer);
#else
            sockaddr_rc peer{}; socklen_t len = sizeof(peer);
#endif
            int cli = static_cast<int>(::accept(fd, reinterpret_cast<sockaddr*>(&peer), &len));
            if (cli < 0) {
                if (stopRequested_.load()) break;
                continue;
            }
            std::thread([cli, peer, receiveDir, cb] {
                platform::UniqueSocket cliSock{cli};
                std::string peerAddr =
#ifdef WFD_WIN32
                    FormatBdaddr(peer.btAddr);
#else
                    FormatBdaddr(peer.rc_bdaddr);
#endif
                ReceivedFile rf;
                rf.peerAddress = peerAddr;
                auto rawRead = [cli](void* b, size_t n) -> int {
#ifdef WFD_WIN32
                    return ::recv(cli, static_cast<char*>(b), static_cast<int>(n), 0);
#else
                    return static_cast<int>(::read(cli, b, n));
#endif
                };
                try {
                    uint8_t kind = proto::ReadFramePrefix(rawRead);
                    if (kind != proto::kKindFile) {
                        rf.errorMessage = "expected FILE frame";
                        cb(rf); return;
                    }
                    auto h = proto::ReadFilePayload(rawRead);
                    rf.fileName = h.name;
                    rf.fileSize = h.size;

                    // Stream body via FileSink, hashing as we go.
                    hash::Sha256Stream fileHash;
                    auto hashedRead = [&](void* b, size_t n) -> int {
                        int r = rawRead(b, n);
                        if (r > 0) fileHash.Update(b, static_cast<size_t>(r));
                        return r;
                    };
                    auto out = sink::StreamToFile(hashedRead, receiveDir,
                                                  h.name, h.size, nullptr);
                    if (!out.ok) {
                        rf.errorMessage = out.error;
                        cb(rf); return;
                    }
                    rf.savedPath = out.savedPath;

                    // Optional HASH trailer.
#ifdef WFD_WIN32
                    DWORD to = 300; // ms
                    ::setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO,
                                 reinterpret_cast<const char*>(&to), sizeof(to));
#else
                    timeval to{0, 300 * 1000};
                    ::setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO,
                                 reinterpret_cast<const char*>(&to), sizeof(to));
#endif
                    uint8_t peek[6];
                    int got = rawRead(peek, 6);
                    if (got == 6 &&
                        std::memcmp(peek, proto::kMagic, 4) == 0 &&
                        peek[4] == proto::kVersion &&
                        peek[5] == proto::kKindHash) {
                        uint8_t algo, digestLen;
                        if (rawRead(&algo, 1) == 1 &&
                            rawRead(&digestLen, 1) == 1 &&
                            digestLen > 0 && digestLen <= proto::kMaxDigestLen) {
                            std::vector<uint8_t> digest(digestLen);
                            if (rawRead(digest.data(), digestLen) == static_cast<int>(digestLen)) {
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
                    rf.errorMessage = std::string("recv: ") + e.what();
                }
                cb(rf);
            }).detach();
        }
    });
    return true;
#else
    return false; // stub
#endif
}

void BluetoothListener::Stop() {
    if (!running_.load()) return;
    stopRequested_ = true;
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

} // namespace bt
} // namespace wfd
