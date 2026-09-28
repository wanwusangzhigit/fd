// Protocol.h — FDFT v2 binary protocol matching WireProtocol.kt bit-for-bit,
// plus an optional native-only KIND=3 HASH trailer for integrity verification.
//
// Frame layout (all integers big-endian):
//
//     MAGIC    : 4 bytes   "FDFT"
//     VERSION  : 1 byte    = 2
//     KIND     : 1 byte
//     payload (depends on KIND)
//
// KIND = 1 (FILE)
//     nameLen  : uint32 BE
//     name     : nameLen bytes (UTF-8)
//     size     : int64  BE
//     payload  : size bytes follow
//
// KIND = 2 (REGISTER_IP)
//     ipLen    : uint8
//     ip       : ipLen bytes (ASCII)
//
// KIND = 3 (HASH) — native-only trailer, optional.
//     algo     : uint8   (= 1 for SHA-256)
//     digestLen: uint8   (32 for SHA-256)
//     digest   : digestLen bytes
//
// The Kotlin reference is `app/src/main/java/com/example/wifidirect/transfer/WireProtocol.kt`.
// Anything written by the Android side parses here, and vice versa.
// Android readers ignore the trailing HASH frame (they don't read past
// `size` body bytes), so native-to-Android transfers remain compatible.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wfd {
namespace proto {

constexpr char kMagic[5] = "FDFT"; // 4 bytes + NUL kept for convenience
constexpr uint8_t kVersion = 2;
constexpr uint8_t kKindFile = 1;
constexpr uint8_t kKindRegisterIp = 2;
constexpr uint8_t kKindHash = 3;        // native-only trailer
constexpr uint8_t kHashAlgoSha256 = 1;
constexpr size_t kMaxNameLen = 4096;
constexpr size_t kMaxDigestLen = 64;

// RAII-free, blocking-only helpers. They throw std::runtime_error on EOF or
// protocol violation so callers can propagate with `try { ... } catch`.

struct FileHeader {
    std::string name;
    int64_t size = 0;
};

// Reads exactly `n` bytes from a `Read`-callback. Throws on EOF or read error.
// `Read` is any callable `int(void*, size_t)` returning bytes read or < 0 on error.
template <class Read>
void ReadExact(Read readFn, void* buf, size_t n);

// Writes exactly `n` bytes via `WriteFn`; throws on short write or error.
template <class WriteFn>
void WriteAll(WriteFn writeFn, const void* buf, size_t n);

// Writes a FILE frame header. Caller must follow up with exactly `size` bytes
// of file content.
template <class WriteFn>
void WriteFileHeader(WriteFn writeFn, const std::string& name, int64_t size);

// Writes a REGISTER_IP frame.
template <class WriteFn>
void WriteRegisterIp(WriteFn writeFn, const std::string& ip);

// Writes a HASH trailer (KIND=3). Used after a FILE body to let the
// receiver verify integrity end-to-end.
template <class WriteFn>
void WriteHashTrailer(WriteFn writeFn, uint8_t algo,
                      const uint8_t* digest, uint8_t digestLen);

// Reads and validates magic+version+kind; returns kind. Throws on mismatch.
template <class Read>
uint8_t ReadFramePrefix(Read readFn);

// Reads FILE payload following a prefix with KIND == kKindFile.
template <class Read>
FileHeader ReadFilePayload(Read readFn);

// Reads REGISTER_IP payload following a prefix with KIND == kKindRegisterIp.
template <class Read>
std::string ReadRegisterIpPayload(Read readFn);

// Reads HASH payload following a prefix with KIND == kKindHash.
// Returns {algo, digest_bytes}.
template <class Read>
std::pair<uint8_t, std::vector<uint8_t>> ReadHashPayload(Read readFn);

} // namespace proto
} // namespace wfd

// ===== Inline template definitions ==========================================

#include <cstring>
#include <stdexcept>

namespace wfd {
namespace proto {

namespace detail {

inline void StoreU16BE(uint8_t* p, uint16_t v) {
    p[0] = (v >> 8) & 0xFF;
    p[1] = v & 0xFF;
}
inline void StoreU32BE(uint8_t* p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8) & 0xFF;
    p[3] = v & 0xFF;
}
inline void StoreU64BE(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = (v >> (8 * (7 - i))) & 0xFF;
}
inline uint16_t LoadU16BE(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
inline uint32_t LoadU32BE(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
           (static_cast<uint32_t>(p[3]));
}
inline uint64_t LoadU64BE(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

} // namespace detail

template <class Read>
void ReadExact(Read readFn, void* buf, size_t n) {
    auto* p = static_cast<uint8_t*>(buf);
    size_t got = 0;
    while (got < n) {
        int r = readFn(p + got, n - got);
        if (r == 0) throw std::runtime_error("EOF");
        if (r < 0)  throw std::runtime_error("read error");
        got += static_cast<size_t>(r);
    }
}

template <class WriteFn>
void WriteAll(WriteFn writeFn, const void* buf, size_t n) {
    const auto* p = static_cast<const uint8_t*>(buf);
    size_t sent = 0;
    while (sent < n) {
        int w = writeFn(p + sent, n - sent);
        if (w < 0) throw std::runtime_error("write error");
        if (w == 0) throw std::runtime_error("zero write");
        sent += static_cast<size_t>(w);
    }
}

template <class WriteFn>
void WriteFileHeader(WriteFn writeFn, const std::string& name, int64_t size) {
    if (name.empty() || name.size() > kMaxNameLen)
        throw std::runtime_error("filename length out of range");
    if (size < 0) throw std::runtime_error("size must be >= 0");
    uint8_t prefix[6] = {
        'F', 'D', 'F', 'T',
        kVersion, kKindFile
    };
    WriteAll(writeFn, prefix, sizeof(prefix));
    uint8_t lenBuf[4];
    detail::StoreU32BE(lenBuf, static_cast<uint32_t>(name.size()));
    WriteAll(writeFn, lenBuf, 4);
    WriteAll(writeFn, name.data(), name.size());
    uint8_t sizeBuf[8];
    detail::StoreU64BE(sizeBuf, static_cast<uint64_t>(size));
    WriteAll(writeFn, sizeBuf, 8);
}

template <class WriteFn>
void WriteRegisterIp(WriteFn writeFn, const std::string& ip) {
    if (ip.empty() || ip.size() > 255)
        throw std::runtime_error("ip length out of range");
    uint8_t prefix[6] = { 'F', 'D', 'F', 'T', kVersion, kKindRegisterIp };
    WriteAll(writeFn, prefix, sizeof(prefix));
    uint8_t len = static_cast<uint8_t>(ip.size());
    WriteAll(writeFn, &len, 1);
    WriteAll(writeFn, ip.data(), ip.size());
}

template <class Read>
uint8_t ReadFramePrefix(Read readFn) {
    uint8_t buf[6];
    ReadExact(readFn, buf, 6);
    if (std::memcmp(buf, kMagic, 4) != 0)
        throw std::runtime_error("invalid magic");
    uint8_t version = buf[4];
    if (version != kVersion)
        throw std::runtime_error("unsupported version");
    uint8_t kind = buf[5];
    if (kind != kKindFile && kind != kKindRegisterIp && kind != kKindHash)
        throw std::runtime_error("unknown kind");
    return kind;
}

template <class Read>
FileHeader ReadFilePayload(Read readFn) {
    uint8_t lenBuf[4];
    ReadExact(readFn, lenBuf, 4);
    uint32_t nameLen = detail::LoadU32BE(lenBuf);
    if (nameLen == 0 || nameLen > kMaxNameLen)
        throw std::runtime_error("name length out of range");
    std::string name(nameLen, '\0');
    ReadExact(readFn, name.data(), nameLen);
    uint8_t sizeBuf[8];
    ReadExact(readFn, sizeBuf, 8);
    int64_t size = static_cast<int64_t>(detail::LoadU64BE(sizeBuf));
    if (size < 0) throw std::runtime_error("size must be >= 0");
    return { std::move(name), size };
}

template <class Read>
std::string ReadRegisterIpPayload(Read readFn) {
    uint8_t len;
    ReadExact(readFn, &len, 1);
    if (len == 0) throw std::runtime_error("empty ip");
    std::string ip(len, '\0');
    ReadExact(readFn, ip.data(), len);
    return ip;
}

template <class WriteFn>
void WriteHashTrailer(WriteFn writeFn, uint8_t algo,
                      const uint8_t* digest, uint8_t digestLen) {
    if (digestLen == 0 || digestLen > kMaxDigestLen)
        throw std::runtime_error("digest length out of range");
    uint8_t prefix[6] = { 'F', 'D', 'F', 'T', kVersion, kKindHash };
    WriteAll(writeFn, prefix, sizeof(prefix));
    WriteAll(writeFn, &algo, 1);
    WriteAll(writeFn, &digestLen, 1);
    WriteAll(writeFn, digest, digestLen);
}

template <class Read>
std::pair<uint8_t, std::vector<uint8_t>> ReadHashPayload(Read readFn) {
    uint8_t algo, len;
    ReadExact(readFn, &algo, 1);
    ReadExact(readFn, &len, 1);
    if (len == 0 || len > kMaxDigestLen)
        throw std::runtime_error("digest length out of range");
    std::vector<uint8_t> digest(len);
    ReadExact(readFn, digest.data(), len);
    return { algo, std::move(digest) };
}

} // namespace proto
} // namespace wfd
