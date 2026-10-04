// Platform.cpp — implementations of the cross-platform primitives.
#include "Platform.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef WFD_WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <shlobj.h>
#  pragma comment(lib, "ws2_32.lib")
   using ssize_t = SSIZE_T;
#else
#  include <unistd.h>
#  include <sys/ioctl.h>
#  include <termios.h>
#  include <pwd.h>
#  include <dirent.h>
#  include <time.h>
#  include <cstdio>
#endif

namespace wfd {
namespace platform {

namespace {
#ifdef WFD_WIN32
std::atomic<int> g_wsa_refcount{0};
#endif
}

bool InitNetworking() {
#ifdef WFD_WIN32
    if (g_wsa_refcount.fetch_add(1) == 0) {
        WSADATA wsa;
        if (int err = WSAStartup(MAKEWORD(2, 2), &wsa); err != 0) return false;
    }
    return true;
#else
    return true;
#endif
}

void ShutdownNetworking() {
#ifdef WFD_WIN32
    if (g_wsa_refcount.fetch_sub(1) == 1) {
        WSACleanup();
    }
#endif
}

int LastError() {
#ifdef WFD_WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

std::string LastErrorString() {
    int e = LastError();
    if (e == 0) return "no error";
#ifdef WFD_WIN32
    char buf[256];
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, (DWORD)e, 0, buf, sizeof(buf), nullptr);
    return std::string(buf, n) + " (WSAGetLastError=" + std::to_string(e) + ")";
#else
    return std::string(std::strerror(e)) + " (errno=" + std::to_string(e) + ")";
#endif
}

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

int64_t FileSize(const std::string& path) {
#ifdef WFD_WIN32
    struct _stati64 st;
    if (_stati64(path.c_str(), &st) != 0) return -1;
#else
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return -1;
#endif
    if ((st.st_mode & S_IFMT) != S_IFREG) return -1;
    return static_cast<int64_t>(st.st_size);
}

std::string Basename(const std::string& path) {
    auto pos = path.find_last_of("\\/");
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

bool IsReadableFile(const std::string& path) {
#ifdef WFD_WIN32
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || f == nullptr) return false;
#else
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
#endif
    std::fclose(f);
    return true;
}

std::string JoinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    // Modern Windows (Win2k+) accepts '/' as a path separator just like Unix.
    // We use forward slashes everywhere so the same code path works on
    // both platforms and avoids mixed-separator confusion under Wine.
    constexpr char sep = '/';
    if (a.back() == '/' || a.back() == '\\') return a + b;
    return a + std::string(1, sep) + b;
}

std::string HomeDir() {
#ifdef WFD_WIN32
    if (const char* p = std::getenv("USERPROFILE"); p && *p) return p;
    if (const char* p = std::getenv("HOMEDRIVE"); p) {
        if (const char* p2 = std::getenv("HOMEPATH"); p2) return std::string(p) + p2;
    }
    return "C:\\";
#else
    if (const char* p = std::getenv("HOME"); p && *p) return p;
    if (auto pw = getpwuid(getuid()); pw && pw->pw_dir) return pw->pw_dir;
    return "/tmp";
#endif
}

std::string DefaultReceiveDir() {
    std::string base = HomeDir();
#ifdef WFD_WIN32
    base = JoinPath(base, "Downloads");
#else
    base = JoinPath(base, "Downloads");
#endif
    std::string dir = JoinPath(base, "wfd_received");
    // Best-effort create
#ifdef WFD_WIN32
    CreateDirectoryA(dir.c_str(), nullptr);
#else
    mkdir(dir.c_str(), 0755);
#endif
    return dir;
}

std::string TempDir() {
    // Compose `<sys-tmp>/wfdXXXXXX` and create it via mkdtemp (POSIX) or
    // mkdir + random suffix (Windows).
    constexpr const char* kHex = "0123456789abcdef";
    auto rand_suffix = [](char* buf, int n) {
        static std::atomic<unsigned> seed{1};
        for (int i = 0; i < n; ++i) {
            unsigned r;
#ifdef WFD_WIN32
            r = static_cast<unsigned>(GetTickCount64() + i + seed.fetch_add(1));
#else
            r = static_cast<unsigned>(std::rand());
#endif
            buf[i] = kHex[r & 0xF];
        }
    };
#ifdef WFD_WIN32
    char tmpl[MAX_PATH];
    GetTempPathA(MAX_PATH, tmpl);
    std::string base = tmpl;
    base = JoinPath(base, "wfd");
    for (int attempt = 0; attempt < 8; ++attempt) {
        char suffix[9];
        rand_suffix(suffix, 8);
        suffix[8] = '\0';
        std::string candidate = base + std::string(suffix);
        if (CreateDirectoryA(candidate.c_str(), nullptr)) return candidate;
    }
    return JoinPath(base, "fallback");
#else
    char tmpl[] = "/tmp/wfdXXXXXX";
    if (char* d = mkdtemp(tmpl); d) return d;
    return "/tmp/wfd_fallback";
#endif
}

bool RemoveDir(const std::string& path) {
#ifdef WFD_WIN32
    // Recursive delete via SHFileOperation would be ideal; for tests we
    // assume shallow dirs.
    if (!RemoveDirectoryA(path.c_str())) return false;
    return true;
#else
    DIR* d = ::opendir(path.c_str());
    if (!d) return false;
    while (auto* ent = ::readdir(d)) {
        if (std::strcmp(ent->d_name, ".") == 0 ||
            std::strcmp(ent->d_name, "..") == 0) continue;
        std::string full = JoinPath(path, ent->d_name);
        struct stat st;
        if (::stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
            RemoveDir(full);
        else
            std::remove(full.c_str());
    }
    ::closedir(d);
    return ::rmdir(path.c_str()) == 0;
#endif
}

void CloseSocket(Socket s) {
    if (s == kInvalidSocket) return;
#ifdef WFD_WIN32
    ::closesocket(s);
#else
    ::close(s);
#endif
}

bool IsTerminal() {
#ifdef WFD_WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(fileno(stdout)) != 0;
#endif
}

void EnableAnsiIfNeeded() {
#ifdef WFD_WIN32
    // Enable VT100 for stdout on Windows 10+.
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(out, &mode)) {
        SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
#endif
}

int TerminalWidth() {
#ifdef WFD_WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        int w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        return w > 0 ? w : 80;
    }
    return 80;
#else
    struct winsize ws;
    if (ioctl(fileno(stdout), TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
    if (const char* p = std::getenv("COLUMNS"); p && *p) {
        int n = std::atoi(p);
        if (n > 0) return n;
    }
    return 80;
#endif
}

} // namespace platform
} // namespace wfd
