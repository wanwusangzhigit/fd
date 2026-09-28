// Util.cpp — small standalone helpers.
#include "Util.h"
#include "Platform.h"

#include <cstdio>
#include <cstring>
#include <set>

#ifdef WFD_WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <iphlpapi.h>
#  pragma comment(lib, "iphlpapi.lib")
#else
#  include <arpa/inet.h>
#  include <ifaddrs.h>
#  include <net/if.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <unistd.h>
#endif

namespace wfd {
namespace util {

namespace {
std::set<char> ForbiddenFsChars() {
    std::set<char> f;
    for (char c : {'/', '\\', ':', '*', '?', '"', '<', '>', '|', '\0'})
        f.insert(c);
    return f;
}
}

std::string HumanSize(int64_t bytes) {
    if (bytes < 0) return "?";
    if (bytes < 1024) return std::to_string(bytes) + " B";
    char buf[64];
    if (bytes < 1024LL * 1024)
        std::snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    else if (bytes < 1024LL * 1024 * 1024)
        std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024));
    else
        std::snprintf(buf, sizeof(buf), "%.2f GB", bytes / (1024.0 * 1024 * 1024));
    return buf;
}

std::string HumanRate(int64_t bytesPerSec) {
    if (bytesPerSec <= 0) return "-";
    return HumanSize(bytesPerSec) + "/s";
}

std::string HumanEta(int64_t seconds) {
    if (seconds < 1) return "";
    int s = static_cast<int>(seconds % 60);
    int m = static_cast<int>((seconds / 60) % 60);
    int h = static_cast<int>((seconds / 3600) % 24);
    int d = static_cast<int>(seconds / 86400);
    char buf[64];
    if (d > 0) std::snprintf(buf, sizeof(buf), "%dd %dh", d, h);
    else if (h > 0) std::snprintf(buf, sizeof(buf), "%dh %dm", h, m);
    else if (m > 0) std::snprintf(buf, sizeof(buf), "%dm %ds", m, s);
    else            std::snprintf(buf, sizeof(buf), "%ds", s);
    return buf;
}

std::string SanitizeFilename(const std::string& raw) {
    static const auto forbidden = ForbiddenFsChars();
    std::string cleaned;
    cleaned.reserve(raw.size());
    for (char c : raw) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' || c == '\0') {
            cleaned.push_back('_');
        } else if (static_cast<unsigned char>(c) < 0x20) {
            cleaned.push_back('_');
        } else {
            cleaned.push_back(c);
        }
    }
    // Trim leading/trailing dots and spaces.
    size_t start = 0, end = cleaned.size();
    while (start < end && (cleaned[start] == '.' || cleaned[start] == ' ')) ++start;
    while (end > start && (cleaned[end - 1] == '.' || cleaned[end - 1] == ' ')) --end;
    cleaned = cleaned.substr(start, end - start);
    if (cleaned.size() > 240) cleaned.resize(240);
    return cleaned;
}

std::pair<std::string, int> ParseHostPort(const std::string& s, int defaultPort) {
    auto colon = s.rfind(':');
    if (colon != std::string::npos) {
        bool allDigits = true;
        for (size_t i = colon + 1; i < s.size(); ++i) {
            if (s[i] < '0' || s[i] > '9') { allDigits = false; break; }
        }
        if (allDigits && colon + 1 < s.size()) {
            try {
                int port = std::stoi(s.substr(colon + 1));
                return { s.substr(0, colon), port };
            } catch (...) {}
        }
    }
    return { s, defaultPort };
}

std::string LocalIpv4() {
#ifdef WFD_WIN32
    char host[256];
    if (gethostname(host, sizeof(host)) != 0) return "";
    addrinfo hints{}; hints.ai_family = AF_INET;
    addrinfo* res = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0) return "";
    std::string addr;
    for (auto* a = res; a; a = a->ai_next) {
        char buf[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(a->ai_addr)->sin_addr,
                      buf, sizeof(buf))) {
            addr = buf; break;
        }
    }
    freeaddrinfo(res);
    return addr;
#else
    ifaddrs* ifas = nullptr;
    if (getifaddrs(&ifas) != 0) return "";
    std::string result;
    for (auto* ifa = ifas; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;
        char host[NI_MAXHOST];
        if (getnameinfo(ifa->ifa_addr, sizeof(sockaddr_in), host, sizeof(host),
                        nullptr, 0, NI_NUMERICHOST) == 0) {
            result = host; break;
        }
    }
    freeifaddrs(ifas);
    return result;
#endif
}

int TerminalWidth() { return platform::TerminalWidth(); }

} // namespace util
} // namespace wfd
