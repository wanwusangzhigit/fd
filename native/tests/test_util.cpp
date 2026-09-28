// test_util.cpp — tests for Util.h helpers.
#include "Util.h"

#include <cassert>
#include <cstdio>
#include <string>

using namespace wfd::util;

static void TestHumanSize() {
    assert(HumanSize(0) == "0 B");
    assert(HumanSize(1) == "1 B");
    assert(HumanSize(1023) == "1023 B");
    assert(HumanSize(1024) == "1.0 KB");
    assert(HumanSize(1536) == "1.5 KB");
    assert(HumanSize(1048576) == "1.0 MB");
    assert(HumanSize(10485760) == "10.0 MB");
    assert(HumanSize(-1) == "?");
}

static void TestHumanEta() {
    assert(HumanEta(0) == "");
    assert(HumanEta(45) == "45s");
    assert(HumanEta(60) == "1m 0s");
    assert(HumanEta(90) == "1m 30s");
    assert(HumanEta(3600) == "1h 0m");
    assert(HumanEta(5400) == "1h 30m");
}

static void TestSanitizeFilename() {
    assert(SanitizeFilename("normal.txt") == "normal.txt");
    assert(SanitizeFilename("foo/bar") == "foo_bar");
    assert(SanitizeFilename("foo\\bar") == "foo_bar");
    assert(SanitizeFilename("a:b*c?d\"e<f>g|h") == "a_b_c_d_e_f_g_h");
    // Leading / trailing dots and spaces are trimmed.
    assert(SanitizeFilename("  hi  ") == "hi");
    assert(SanitizeFilename("..hi..") == "hi");
    // Control chars become underscores.
    assert(SanitizeFilename(std::string("a\0b", 3)) == "a_b");
}

static void TestParseHostPort() {
    auto p = ParseHostPort("192.168.1.5", 8988);
    assert(p.first == "192.168.1.5");
    assert(p.second == 8988);

    p = ParseHostPort("192.168.1.5:1234", 8988);
    assert(p.first == "192.168.1.5");
    assert(p.second == 1234);

    p = ParseHostPort("host.example.com:42", 0);
    assert(p.first == "host.example.com");
    assert(p.second == 42);

    // IPv6-ish bracketed: we don't fully support IPv6, but the parser
    // should still default gracefully.
    p = ParseHostPort("[::1]", 9999);
    assert(p.second == 9999);
}

int main() {
    TestHumanSize();
    TestHumanEta();
    TestSanitizeFilename();
    TestParseHostPort();
    std::printf("test_util: ALL OK\n");
    return 0;
}
