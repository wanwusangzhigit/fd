// Platform.h — minimal cross-platform abstraction layer for socket + fs APIs
//
// Wraps the few platform-specific calls we need so the rest of the codebase
// can be written against a single header without #ifdef branches scattered
// everywhere. Designed to compile cleanly on:
//   - Linux (glibc + BlueZ)
//   - Windows (Winsock2 + Win32 Bluetooth)
//
// Conventions:
//   * All `Open*` functions return negative on error (with errno/WSAGetLastError
//     preserved via LastError()), or a non-negative handle on success.
//   * RAII wrappers are provided as `UniqueSocket` etc. to avoid leaks.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace wfd {
namespace platform {

// ---------- Initialization / teardown -------------------------------------

// On Windows, must call WSAStartup once per process; on Linux it's a no-op.
// Idempotent; safe to call multiple times.
bool InitNetworking();
void ShutdownNetworking();

// Returns the OS-level last error code (errno or WSAGetLastError).
int LastError();
std::string LastErrorString();

// ---------- Time ----------------------------------------------------------

// Monotonic milliseconds since some unspecified epoch.
int64_t NowMs();

// ---------- Filesystem ----------------------------------------------------

// Reads the size of a regular file by path. Returns -1 on error.
int64_t FileSize(const std::string& path);

// Returns the basename component of a path (e.g. "/a/b/c.txt" -> "c.txt").
std::string Basename(const std::string& path);

// True iff the given path is a regular file that we can open for reading.
bool IsReadableFile(const std::string& path);

// Joins two path components with the platform separator.
std::string JoinPath(const std::string& a, const std::string& b);

// Returns the user's home directory (HOME on Unix, USERPROFILE on Windows).
std::string HomeDir();

// Returns a writable directory for received files. Defaults to
// `<home>/Downloads/wfd_received/` and is created if missing.
std::string DefaultReceiveDir();

// Creates and returns a fresh, uniquely-named directory suitable for
// temporary files. Caller owns the directory (cleans up by removing it
// and its contents — see `RemoveDir`). Returns empty on failure.
std::string TempDir();

// Recursively removes a directory and its contents. Returns false on error.
bool RemoveDir(const std::string& path);

// ---------- Socket handles -----------------------------------------------

// OS-level socket descriptor. We intentionally use `int` on both platforms
// (Windows' SOCKET is a UINT_PTR but small enough to fit in an int for our
// use cases). All wrappers below operate on Socket.
using Socket = int;

// Sentinel for an invalid socket.
constexpr Socket kInvalidSocket = -1;

// Closes a socket; safe to call on kInvalidSocket.
void CloseSocket(Socket s);

// RAII wrapper. Move-only.
class UniqueSocket {
public:
    UniqueSocket() = default;
    explicit UniqueSocket(Socket s) : s_(s) {}
    ~UniqueSocket() { CloseSocket(s_); }
    UniqueSocket(const UniqueSocket&) = delete;
    UniqueSocket& operator=(const UniqueSocket&) = delete;
    UniqueSocket(UniqueSocket&& o) noexcept : s_(o.s_) { o.s_ = kInvalidSocket; }
    UniqueSocket& operator=(UniqueSocket&& o) noexcept {
        if (this != &o) { CloseSocket(s_); s_ = o.s_; o.s_ = kInvalidSocket; }
        return *this;
    }
    Socket get() const { return s_; }
    Socket release() { Socket t = s_; s_ = kInvalidSocket; return t; }
    void reset(Socket s = kInvalidSocket) { CloseSocket(s_); s_ = s; }
    explicit operator bool() const { return s_ != kInvalidSocket; }
private:
    Socket s_ = kInvalidSocket;
};

// ---------- Console -------------------------------------------------------

// Returns true if stdin is a TTY (used to gate ANSI color output).
bool IsTerminal();

// Enables VT100 / ANSI escape processing on Windows 10+ terminals.
void EnableAnsiIfNeeded();

// Returns the terminal width in columns (defaults to 80 on failure).
int TerminalWidth();

} // namespace platform
} // namespace wfd
