// test_hash.cpp — SHA-256 known-answer tests.
//
// Reference vectors from NIST FIPS 180-4 examples and the well-known
// SHA-256 playground values. We exercise both the one-shot helper and
// the incremental stream interface (with multiple Update() calls of
// varying sizes — important for the padding path).
#include "Hash.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace wfd::hash;

namespace {

#define CHECK_EQ(actual, expected) \
    do { \
        std::string a = (actual); \
        std::string e = (expected); \
        if (a != e) { \
            std::fprintf(stderr, "FAIL\n  expected: %s\n  actual:   %s\n", \
                         e.c_str(), a.c_str()); \
            std::exit(1); \
        } \
    } while (0)

void TestKnownAnswers() {
    // Empty input.
    const char* empty = "";
    CHECK_EQ(Sha256Hex(empty, 0),
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    // Single 'a'.
    const char* one = "a";
    CHECK_EQ(Sha256Hex(one, 1),
        "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb");

    // "abc" — FIPS 180-4 example B.1.
    const char* abc = "abc";
    CHECK_EQ(Sha256Hex(abc, 3),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    // "message digest".
    const char* md = "message digest";
    CHECK_EQ(Sha256Hex(md, std::strlen(md)),
        "f7846f55cf23e14eebeab5b4e1550cad5b509e3348fbc4efa3a1413d393cb650");

    // 56-byte input (just under a single block boundary).
    std::string s56(56, 'a');
    CHECK_EQ(Sha256Hex(s56.data(), s56.size()),
        "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");

    // 64-byte input (exactly one block + padding-only second block).
    std::string s64(64, 'a');
    CHECK_EQ(Sha256Hex(s64.data(), s64.size()),
        "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");

    // 1000000 'a' (heavy stress test of incremental hashing).
    Sha256Stream strm;
    std::string chunk(1000, 'a');
    for (int i = 0; i < 1000; ++i) strm.Update(chunk.data(), chunk.size());
    CHECK_EQ(strm.FinishHex(),
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

void TestIncrementalChunkingMatchesOneShot() {
    std::string data(12345, '\0');
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<char>(i & 0xFF);

    std::string oneShot = Sha256Hex(data.data(), data.size());

    Sha256Stream s;
    for (size_t off = 0; off < data.size(); off += 7)
        s.Update(data.data() + off, std::min<size_t>(7, data.size() - off));
    std::string streamed = s.FinishHex();

    assert(oneShot == streamed);
}

} // namespace

int main() {
    TestKnownAnswers();
    TestIncrementalChunkingMatchesOneShot();
    std::printf("test_hash: ALL OK\n");
    return 0;
}
