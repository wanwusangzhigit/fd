// Discovery.h — UDP broadcast beacon so two native clients can find each
// other on the same LAN without typing host names manually.
//
// Beacon packet payload:
//   ASCII "WFDn" + 1-byte version + UTF-8 hostname + '\0' + IP list (CSV)
// Sent every BEACON_INTERVAL_MS ms to the LAN broadcast on port DISCOVERY_PORT.
//
// Two public entry points:
//   * `Beacon::Start()` runs a background thread.
//   * `Listen(durationSec)` blocks for up to `durationSec` collecting beacons.
#pragma once

#include "Platform.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace wfd {
namespace discovery {

constexpr int kBeaconPort = 18998;
constexpr int kBeaconIntervalMs = 1500;

struct Peer {
    std::string ip;
    std::string host;
    int64_t lastSeenMs = 0;
};

// Background broadcaster announcing our presence on the LAN.
class Beacon {
public:
    Beacon() = default;
    ~Beacon();
    bool Start();
    void Stop();
private:
    void Run(int fd);
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    struct ThreadHandle { std::thread t; };
    std::unique_ptr<ThreadHandle> thread_;
};

// Block for up to `durationSec` listening for beacons. Returns all peers
// seen (most recently updated first).
std::vector<Peer> Listen(int durationSec);

} // namespace discovery
} // namespace wfd
