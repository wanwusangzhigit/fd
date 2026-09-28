// Util.h — small standalone helpers shared by transport & CLI layers.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wfd {
namespace util {

// Same constants as Android `Constants.kt`.
constexpr int kWifiFilePort = 8988;
constexpr int kWifiRegisterPort = 8989;
constexpr const char* kServiceUuid = "8e7a1c5a-2f44-4f7c-b76b-9b8f4b9f0001";
constexpr const char* kServiceName = "WiFiDirectFileTransfer_BT";

// Human-readable byte count ("512 B", "1.5 KB", "42.0 MB", "2.50 GB").
std::string HumanSize(int64_t bytes);

// Per-second rate ("2.5 MB/s", "800 KB/s").
std::string HumanRate(int64_t bytesPerSec);

// ETA seconds -> compact "1m 30s" / "45s" / "1h 5m".
std::string HumanEta(int64_t seconds);

// Mirrors `util.Sanitize.kt` (`sanitizeFilename`).
std::string SanitizeFilename(const std::string& raw);

// Split `host[:port]` -> (host, port). Defaults to `defaultPort`.
std::pair<std::string, int> ParseHostPort(const std::string& s, int defaultPort);

// Returns the local IPv4 address of the preferred outbound interface as a
// string ("192.168.1.5"), or empty on failure.
std::string LocalIpv4();

// Returns terminal width using `Platform::TerminalWidth()`, but cached.
int TerminalWidth();

} // namespace util
} // namespace wfd
