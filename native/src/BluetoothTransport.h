// BluetoothTransport.h — Bluetooth RFCOMM send / receive.
//
// Two platform implementations behind a uniform facade:
//
//   Linux:   socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM)
//            Requires libbluetooth-dev + paired device. Uses BlueZ RFCOMM
//            directly (no D-Bus needed for raw RFCOMM).
//
//   Windows: socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM)
//            Requires the device to be paired and the SDP record registered
//            via BluetoothRegisterForAuthenticationEx etc. We register an
//            SDP entry on listen.
//
// Both endpoints MUST use the same UUID as Constants.BLUETOOTH_APP_UUID
// in the Kotlin reference: `8e7a1c5a-2f44-4f7c-b76b-9b8f4b9f0001`.
#pragma once

#include "Platform.h"
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace wfd {
namespace bt {

// Same as Constants.BLUETOOTH_APP_UUID.
constexpr const char* kServiceUuid = "8e7a1c5a-2f44-4f7c-b76b-9b8f4b9f0001";
constexpr const char* kServiceName = "WiFiDirectFileTransfer_BT";

// A Bluetooth MAC address, formatted as `XX:XX:XX:XX:XX:XX` on both
// platforms. Empty if unknown.
struct PeerDevice {
    std::string address;  // ASCII, colon-separated
    std::string name;     // friendly name, may be empty
};

// Outcome of Send.
struct SendResult {
    bool ok = false;
    std::string error;
    int64_t bytesSent = 0;
    std::string senderHash;
};

struct ReceivedFile {
    std::string peerAddress;
    std::string fileName;
    int64_t fileSize = 0;
    std::string savedPath;
    std::string errorMessage;
    std::string receivedHash;
    bool integrityVerified = false;
};

// Callback signature for listener / discovery.
using ReceivedCallback = std::function<void(const ReceivedFile&)>;

// -------- Discovery ------------------------------------------------------

// Enumerate paired devices whose address appears in the OS cache.
// Returns at most `max` entries (use 0 for unlimited).
std::vector<PeerDevice> EnumeratePairedDevices(size_t max = 0);

// Active Bluetooth inquiry (typically 10–12 s). Calls `onFound` for each
// newly discovered device; returns total count. Blocking.
int DiscoverDevices(std::function<void(const PeerDevice&)> onFound, int durationSec = 12);

// -------- Send -----------------------------------------------------------

// Streams `filePath` to `peerAddress` over RFCOMM.
SendResult SendFile(const std::string& peerAddress,
                    const std::string& filePath,
                    const std::string& forcedName,
                    std::function<void(int64_t, int64_t)> onProgress = {});

// Convenience: basename derived from filePath.
SendResult SendFile(const std::string& peerAddress,
                    const std::string& filePath,
                    std::function<void(int64_t, int64_t)> onProgress = {});

// Lowest-level variant: lets caller suppress the optional SHA-256 trailer
// (e.g. when talking to the Android app which doesn't expect it).
SendResult SendFile(const std::string& peerAddress,
                    const std::string& filePath,
                    const std::string& forcedName,
                    std::function<void(int64_t, int64_t)> onProgress,
                    bool includeHashTrailer);

// -------- Receive (RFCOMM server) ---------------------------------------

// RAII Bluetooth listener. Mirrors `tcp::TcpListener` semantics.
class BluetoothListener {
public:
    BluetoothListener() = default;
    ~BluetoothListener();
    BluetoothListener(const BluetoothListener&) = delete;
    BluetoothListener& operator=(const BluetoothListener&) = delete;

    // Starts listening for incoming RFCOMM connections from any peer.
    // Returns false if Bluetooth is unsupported or registration fails.
    bool Start(const std::string& receiveDir, ReceivedCallback cb);
    void Stop();
    bool IsRunning() const { return running_.load(); }

private:
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    platform::UniqueSocket listenSock_;
    struct ThreadHandle { std::thread t; };
    std::unique_ptr<ThreadHandle> thread_;
};

// Whether the build actually has Bluetooth support compiled in.
// Returns false on platforms where we ship a stub (e.g. no BlueZ dev headers).
bool IsSupported();

} // namespace bt
} // namespace wfd
