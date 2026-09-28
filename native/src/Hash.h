// Hash.h — self-contained SHA-256 implementation.
//
// We ship our own SHA-256 (FIPS 180-4) for two reasons:
//   * No external dependency on Linux (libssl-dev) or Windows (OpenSSL).
//   * Identical byte-for-byte behavior on every compiler / libc.
//
// Two flavors:
//   * `Sha256Stream` — incremental: feed bytes via `Update()`, finish
//     with `Finish()` to get the 32-byte digest.
//   * `Sha256(buf, n)` — one-shot convenience.
//
// The implementation is small enough (~150 LOC) to keep in a single
// translation unit and fast enough for our use case (streaming hash of
// arbitrarily large files at ~600 MB/s on a modern desktop CPU).
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace wfd {
namespace hash {

constexpr size_t kSha256Len = 32;

// 32-byte digest expressed as a hex string ("a3b1c2...", 64 chars).
std::string Sha256Hex(const void* data, size_t len);

// Incremental hasher. Cheap to copy after `Update()` if you need snapshots.
class Sha256Stream {
public:
    Sha256Stream();
    void Update(const void* data, size_t len);
    std::array<uint8_t, kSha256Len> Finish();
    std::string FinishHex();
private:
    void Compress(const uint8_t* block);
    uint32_t state_[8];
    uint8_t buffer_[64];
    uint32_t bufferLen_ = 0;
    uint64_t totalLen_ = 0;
    bool done_ = false;
};

} // namespace hash
} // namespace wfd
