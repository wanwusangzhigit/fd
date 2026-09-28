// test_protocol.cpp — round-trip tests for the FDFT v2 binary protocol.
#include "Protocol.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

using namespace wfd::proto;

namespace {

// A minimal ring-buffer-backed pair of read/write callbacks so we can pump
// bytes from a writer into a reader without touching sockets.
struct Pipe {
    std::vector<uint8_t> buf;
    size_t readPos = 0;

    int write(const void* p, size_t n) {
        buf.insert(buf.end(),
                   static_cast<const uint8_t*>(p),
                   static_cast<const uint8_t*>(p) + n);
        return static_cast<int>(n);
    }
    int read(void* p, size_t n) {
        size_t avail = buf.size() - readPos;
        if (avail == 0) return 0; // EOF
        size_t give = std::min(avail, n);
        std::memcpy(p, buf.data() + readPos, give);
        readPos += give;
        return static_cast<int>(give);
    }
};

void TestRoundtripFile() {
    Pipe p;
    WriteFileHeader([&](const void* b, size_t n) { return p.write(b, n); },
                    "hello.txt", 5);
    const char* body = "hello";
    WriteAll([&](const void* b, size_t n) { return p.write(b, n); },
             body, 5);

    uint8_t kind = ReadFramePrefix([&](void* b, size_t n) { return p.read(b, n); });
    assert(kind == kKindFile);
    auto h = ReadFilePayload([&](void* b, size_t n) { return p.read(b, n); });
    assert(h.name == "hello.txt");
    assert(h.size == 5);
    char buf[5];
    ReadExact([&](void* b, size_t n) { return p.read(b, n); }, buf, 5);
    assert(std::memcmp(buf, "hello", 5) == 0);
}

void TestRoundtripRegisterIp() {
    Pipe p;
    WriteRegisterIp([&](const void* b, size_t n) { return p.write(b, n); },
                    "192.168.1.10");
    uint8_t kind = ReadFramePrefix([&](void* b, size_t n) { return p.read(b, n); });
    assert(kind == kKindRegisterIp);
    auto ip = ReadRegisterIpPayload([&](void* b, size_t n) { return p.read(b, n); });
    assert(ip == "192.168.1.10");
}

void TestBadMagic() {
    Pipe p;
    uint8_t bogus[6] = { 'X','X','X','X', kVersion, kKindFile };
    p.write(bogus, 6);
    bool threw = false;
    try {
        ReadFramePrefix([&](void* b, size_t n) { return p.read(b, n); });
    } catch (const std::exception&) {
        threw = true;
    }
    assert(threw);
}

void TestBadVersion() {
    Pipe p;
    uint8_t bad[6] = { 'F','D','F','T', 99, kKindFile };
    p.write(bad, 6);
    bool threw = false;
    try {
        ReadFramePrefix([&](void* b, size_t n) { return p.read(b, n); });
    } catch (const std::exception&) {
        threw = true;
    }
    assert(threw);
}

void TestBadKind() {
    Pipe p;
    uint8_t bad[6] = { 'F','D','F','T', kVersion, 99 };
    p.write(bad, 6);
    bool threw = false;
    try {
        ReadFramePrefix([&](void* b, size_t n) { return p.read(b, n); });
    } catch (const std::exception&) {
        threw = true;
    }
    assert(threw);
}

void TestEmptyName() {
    Pipe p;
    bool threw = false;
    try {
        WriteFileHeader([&](const void* b, size_t n) { return p.write(b, n); },
                        "", 0);
    } catch (const std::exception&) {
        threw = true;
    }
    assert(threw);
}

void TestTooLargeName() {
    Pipe p;
    bool threw = false;
    try {
        WriteFileHeader([&](const void* b, size_t n) { return p.write(b, n); },
                        std::string(kMaxNameLen + 1, 'x'), 0);
    } catch (const std::exception&) {
        threw = true;
    }
    assert(threw);
}

void TestEofOnReadExact() {
    Pipe p;
    uint8_t onlyThree[3] = {1, 2, 3};
    p.write(onlyThree, 3);
    bool threw = false;
    uint8_t out[8];
    try {
        ReadExact([&](void* b, size_t n) { return p.read(b, n); }, out, 8);
    } catch (const std::exception&) {
        threw = true;
    }
    assert(threw);
}

} // namespace

int main() {
    TestRoundtripFile();
    TestRoundtripRegisterIp();
    TestBadMagic();
    TestBadVersion();
    TestBadKind();
    TestEmptyName();
    TestTooLargeName();
    TestEofOnReadExact();
    std::printf("test_protocol: ALL OK\n");
    return 0;
}
