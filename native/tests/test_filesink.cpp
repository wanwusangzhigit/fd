// test_filesink.cpp — verifies sink::StreamToFile and
// sink::ResolveUniquePath for correctness and uniqueness.
#include "FileSink.h"
#include "Platform.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace wfd;
using namespace wfd::sink;

namespace {

std::string TempDir() {
    return wfd::platform::TempDir();
}

std::vector<uint8_t> ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(in),
             std::istreambuf_iterator<char>() };
}

void WriteFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

void TestStreamToFileBasic() {
    std::string dir = TempDir();
    std::vector<uint8_t> payload(2048);
    for (size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<uint8_t>((i * 17 + 3) & 0xFF);

    size_t cursor = 0;
    auto readFn = [&](void* b, size_t n) -> int {
        if (cursor >= payload.size()) return 0;
        size_t give = std::min(n, payload.size() - cursor);
        std::memcpy(b, payload.data() + cursor, give);
        cursor += give;
        return static_cast<int>(give);
    };
    auto out = StreamToFile(readFn, dir, "data.bin",
                            static_cast<int64_t>(payload.size()));
    assert(out.ok);
    assert(out.bytesWritten == static_cast<int64_t>(payload.size()));
    auto got = ReadFile(out.savedPath);
    assert(got == payload);
}

void TestStreamToFileEofMidway() {
    std::string dir = TempDir();
    // The readFn signals EOF immediately.
    auto readFn = [](void*, size_t) -> int { return 0; };
    auto out = StreamToFile(readFn, dir, "empty.bin", 100);
    assert(!out.ok);
    assert(!out.error.empty());
}

void TestResolveUniquePathCollision() {
    std::string dir = TempDir();
    // Create an existing file.
    std::string p1 = ResolveUniquePath(dir, "x.txt");
    WriteFile(p1, { 'h', 'i' });
    // Now resolution should pick a new name.
    std::string p2 = ResolveUniquePath(dir, "x.txt");
    assert(p2 != p1);
    // And again — different again.
    WriteFile(p2, { 'y', 'o' });
    std::string p3 = ResolveUniquePath(dir, "x.txt");
    assert(p3 != p1);
    assert(p3 != p2);
}

void TestResolveUniquePathSanitizes() {
    std::string dir = TempDir();
    std::string p = ResolveUniquePath(dir, "a/b:c?*.txt");
    assert(p.find('/') == std::string::npos || p.find("/") >= dir.size());
    assert(p.find("\\") == std::string::npos || p.find("\\") >= dir.size());
}

} // namespace

int main() {
    TestStreamToFileBasic();
    TestStreamToFileEofMidway();
    TestResolveUniquePathCollision();
    TestResolveUniquePathSanitizes();
    std::printf("test_filesink: ALL OK\n");
    return 0;
}
