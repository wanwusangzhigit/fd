// Discovery.cpp — UDP broadcast beacon transmitter + receiver.
//
// Two peers on the same LAN can find each other without typing IPs:
//
//   * `Beacon::Start()` broadcasts a small packet every `kBeaconIntervalMs`
//     milliseconds from a background thread.
//   * `Listen(durationSec)` binds a UDP socket to `kBeaconPort`, collects
//     unique beacons for `durationSec` seconds, then returns the peers
//     sorted by most-recently-seen.
//
// Wire format (all bytes ASCII / UTF-8, network-byte-order not needed):
//
//     "WFDn"          4 bytes magic
//     VERSION         1 byte  (= 1)
//     hostname        N bytes, NUL-terminated UTF-8
//     ipList          M bytes, comma-separated IPv4 list, NOT NUL-terminated
//                     (the rest of the UDP packet payload is the list).
//
// We keep the format intentionally trivial to keep Android-side
// implementation cheap if we ever want to interop.
#include "Discovery.h"
#include "Platform.h"
#include "Util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <map>
#include <memory>
#include <thread>
#include <vector>

#ifdef WFD_WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  pragma comment(lib, "ws2_32.lib")
   using ssize_t = SSIZE_T;
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace wfd {
namespace discovery {

namespace {

constexpr char kBeaconMagic[5] = "WFDn";
constexpr uint8_t kBeaconVersion = 1;
constexpr size_t kMaxPacket = 1024;

// Build the beacon payload: "WFDn" + version + hostname + '\0' + csv_ips
std::string BuildBeaconPayload() {
    std::string p;
    p.append(kBeaconMagic, 4);
    p.push_back(static_cast<char>(kBeaconVersion));

    char host[256] = {0};
    if (gethostname(host, sizeof(host) - 1) != 0) {
        std::strncpy(host, "unknown", sizeof(host) - 1);
    }
    p.append(host);
    p.push_back('\0');

    std::string ip = util::LocalIpv4();
    if (ip.empty()) ip = "0.0.0.0";
    p.append(ip);
    return p;
}

bool ParseBeacon(const char* buf, size_t len, Peer& out) {
    if (len < 6) return false;
    if (std::memcmp(buf, kBeaconMagic, 4) != 0) return false;
    if (static_cast<uint8_t>(buf[4]) != kBeaconVersion) return false;

    const char* p = buf + 5;
    const char* end = buf + len;
    // hostname is NUL-terminated
    const char* nul = static_cast<const char*>(std::memchr(p, '\0', end - p));
    if (!nul) return false;
    out.host.assign(p, nul - p);

    const char* ipStart = nul + 1;
    if (ipStart >= end) {
        out.ip.clear();
    } else {
        // Take the first IP from the CSV (there might be several).
        const char* comma = static_cast<const char*>(
            std::memchr(ipStart, ',', end - ipStart));
        size_t ipLen = comma ? static_cast<size_t>(comma - ipStart)
                             : static_cast<size_t>(end - ipStart);
        out.ip.assign(ipStart, ipLen);
    }
    out.lastSeenMs = platform::NowMs();
    return true;
}

} // namespace

// ---------- Beacon ---------------------------------------------------------

Beacon::~Beacon() { Stop(); }

bool Beacon::Start() {
    if (running_.load()) return true;
    if (!platform::InitNetworking()) return false;

    int fd = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (fd < 0) return false;

    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST,
                 reinterpret_cast<const char*>(&yes), sizeof(yes));
#ifdef SO_REUSEADDR
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&yes), sizeof(yes));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        platform::CloseSocket(fd);
        return false;
    }

    running_ = true;
    stop_ = false;
    thread_ = std::make_unique<ThreadHandle>();
    thread_->t = std::thread([this, fd] { Run(fd); });
    return true;
}

void Beacon::Stop() {
    if (!running_.load()) return;
    stop_ = true;
    if (thread_ && thread_->t.joinable()) thread_->t.join();
    thread_.reset();
    running_ = false;
}

void Beacon::Run(int fd) {
    std::string payload = BuildBeaconPayload();
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(static_cast<uint16_t>(kBeaconPort));
    dst.sin_addr.s_addr = inet_addr("255.255.255.255");

    while (!stop_.load()) {
        // Refresh payload every ~10s in case the IP changes (DHCP, etc.).
        static int64_t lastRefresh = 0;
        int64_t now = platform::NowMs();
        if (now - lastRefresh > 10000 || payload.empty()) {
            std::string p = BuildBeaconPayload();
            if (p.size() <= kMaxPacket) payload = std::move(p);
            lastRefresh = now;
        }
        int rc = ::sendto(fd, payload.data(),
                          static_cast<int>(payload.size()), 0,
                          reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        (void)rc;

        // Sleep in 100 ms slices so Stop is responsive.
        for (int slept = 0; slept < kBeaconIntervalMs && !stop_.load(); slept += 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    platform::CloseSocket(fd);
    platform::ShutdownNetworking();
}

// ---------- Listen ---------------------------------------------------------

std::vector<Peer> Listen(int durationSec) {
    std::vector<Peer> result;
    if (!platform::InitNetworking()) return result;

    int fd = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (fd < 0) return result;

    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(kBeaconPort));
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        platform::CloseSocket(fd);
        return result;
    }

    int64_t deadline = platform::NowMs() + durationSec * 1000LL;
    std::map<std::string, Peer> seen; // key = ip

    while (true) {
        int64_t remaining = deadline - platform::NowMs();
        if (remaining <= 0) break;
        timeval tv{};
        tv.tv_sec = static_cast<long>(std::min<int64_t>(remaining, 1000) / 1000);
        tv.tv_usec = 0;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&tv), sizeof(tv));

        char buf[kMaxPacket];
        sockaddr_in src{};
        socklen_t slen = sizeof(src);
        ssize_t n = ::recvfrom(fd, buf, sizeof(buf), 0,
                               reinterpret_cast<sockaddr*>(&src), &slen);
        if (n <= 0) continue;
        Peer p;
        if (!ParseBeacon(buf, static_cast<size_t>(n), p)) continue;
        if (p.ip.empty()) continue;
        seen[p.ip] = p;
    }

    platform::CloseSocket(fd);
    platform::ShutdownNetworking();

    result.reserve(seen.size());
    for (auto& [_, peer] : seen) result.push_back(std::move(peer));
    std::sort(result.begin(), result.end(),
              [](const Peer& a, const Peer& b) {
                  return a.lastSeenMs > b.lastSeenMs;
              });
    return result;
}

} // namespace discovery
} // namespace wfd
