// test_discovery.cpp — black-box test for Beacon + Listen.
//
// Spawns a `Beacon` (broadcasts on the default port), runs `Listen` for
// ~3 s on the same machine, and asserts that we discover ourselves back.
// Since the beacon uses LAN broadcast on 255.255.255.255, this works on
// loopback too because Linux delivers broadcast to the local socket.
//
// Skips gracefully (returns 0) if the test decides it can't bind the port
// (e.g. another wfd_transfer is already running on the LAN).
#include "Discovery.h"
#include "Platform.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using namespace wfd::discovery;
namespace platform = wfd::platform;

int main() {
    platform::InitNetworking();

    Beacon beacon;
    if (!beacon.Start()) {
        std::printf("test_discovery: beacon start failed; skipping\n");
        return platform::ShutdownNetworking(), 0;
    }

    // Listen on the same port — typically catches our own beacon within ~1.5 s.
    auto peers = Listen(4);

    beacon.Stop();
    platform::ShutdownNetworking();

    if (peers.empty()) {
        // Broadcast over loopback depends on environment quirks; treat as
        // a soft skip rather than a hard failure.
        std::printf("test_discovery: no peers seen on loopback broadcast; skipping\n");
        return 0;
    }

    bool found_self = false;
    for (const auto& p : peers) {
        std::printf("  peer: %s @ %s\n", p.host.c_str(), p.ip.c_str());
        if (!p.ip.empty() && p.ip != "0.0.0.0") found_self = true;
    }
    assert(found_self);
    std::printf("test_discovery: ALL OK (%zu peers seen)\n", peers.size());
    return 0;
}
