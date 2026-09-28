// main.cpp — cross-platform CLI entry point for `wfd_transfer`.
//
// Mirrors what the Android app does, but driven by subcommands:
//
//   wfd_transfer send   --to <host[:port]>      <file> [<file> ...]
//   wfd_transfer send-bt --to <BT_ADDR>         <file> [<file> ...]
//   wfd_transfer recv   [--dir <dir>] [--port <p>]
//   wfd_transfer recv-bt [--dir <dir>]
//   wfd_transfer discover [--seconds <n>]
//   wfd_transfer beacon           (until Ctrl-C)
//   wfd_transfer list-bt
//
// All commands honor `--help`. Diagnostics go to stderr; the regular
// event stream goes to stdout so it's easy to scrape / redirect.
#include "BluetoothTransport.h"
#include "Discovery.h"
#include "Hash.h"
#include "Platform.h"
#include "Progress.h"
#include "TcpTransport.h"
#include "Util.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef WFD_WIN32
#  include <windows.h>
#endif

namespace {

std::atomic<bool> g_interrupted{false};
std::condition_variable g_cv;
std::mutex g_cvMutex;

void OnSignal(int) {
    g_interrupted.store(true);
    g_cv.notify_all();
}

void InstallSigintHandler() {
#ifdef WFD_WIN32
    auto handler = +[](DWORD ctrlType) -> BOOL {
        if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
            OnSignal(0);
            return TRUE;
        }
        return FALSE;
    };
    SetConsoleCtrlHandler(handler, TRUE);
#else
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
#endif
}

void WaitForInterrupt() {
    std::unique_lock<std::mutex> lk(g_cvMutex);
    g_cv.wait(lk, [] { return g_interrupted.load(); });
}

// --- argument parsing ----------------------------------------------------

struct Args {
    std::string command;
    std::string to;          // --to
    std::string dir;         // --dir
    int port = 0;            // --port (0 = default)
    int seconds = 8;         // --seconds
    bool includeHash = true; // --no-hash
    std::vector<std::string> files;
};

void PrintUsage() {
    std::fprintf(stderr,
        "wfd_transfer — cross-platform FDFT-compatible file transfer\n"
        "\n"
        "Usage:\n"
        "  wfd_transfer send      --to <host[:port]> <file> [<file> ...]\n"
        "                          [--no-hash]\n"
        "  wfd_transfer send-bt   --to <BT_ADDR>     <file> [<file> ...]\n"
        "                          [--no-hash]\n"
        "  wfd_transfer recv      [--dir <dir>] [--port <p>]\n"
        "  wfd_transfer recv-bt   [--dir <dir>]\n"
        "  wfd_transfer discover  [--seconds <n>]\n"
        "  wfd_transfer beacon\n"
        "  wfd_transfer list-bt\n"
        "  wfd_transfer hash <file> [<file> ...]\n"
        "\n"
        "Options:\n"
        "  --no-hash   skip SHA-256 trailer (compatible with Android peers)\n"
        "\n"
        "Defaults:\n"
        "  --port    %d (TCP file port)\n"
        "  --dir     %s\n"
        "  --seconds %d\n",
        wfd::tcp::kDefaultFilePort,
        wfd::platform::DefaultReceiveDir().c_str(),
        8);
}

bool ParseArgs(int argc, char** argv, Args& out) {
    if (argc < 2) return false;
    out.command = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* opt) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s requires a value\n", opt);
                std::exit(2);
            }
            return std::string(argv[++i]);
        };
        if (a == "--help" || a == "-h") { PrintUsage(); std::exit(0); }
        else if (a == "--to")     out.to = next("--to");
        else if (a == "--dir")    out.dir = next("--dir");
        else if (a == "--port")   out.port = std::atoi(next("--port").c_str());
        else if (a == "--seconds") out.seconds = std::atoi(next("--seconds").c_str());
        else if (a == "--no-hash") out.includeHash = false;
        else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "error: unknown option %s\n", a.c_str());
            return false;
        } else {
            out.files.push_back(a);
        }
    }
    return true;
}

// --- command implementations --------------------------------------------

int DoSend(const Args& args) {
    if (args.to.empty()) {
        std::fprintf(stderr, "error: --to is required\n");
        return 2;
    }
    if (args.files.empty()) {
        std::fprintf(stderr, "error: at least one file is required\n");
        return 2;
    }
    auto [host, port] = wfd::util::ParseHostPort(args.to, wfd::tcp::kDefaultFilePort);
    if (args.port > 0) port = args.port;

    int rc = 0;
    for (const auto& path : args.files) {
        if (!wfd::platform::IsReadableFile(path)) {
            std::fprintf(stderr, "error: cannot read %s\n", path.c_str());
            rc = 1;
            continue;
        }
        std::string name = wfd::platform::Basename(path);
        auto reporter = wfd::progress::MakeReporter(name);
        std::printf("Sending %s (%s) -> %s:%d  [%s]\n",
                    name.c_str(),
                    wfd::util::HumanSize(wfd::platform::FileSize(path)).c_str(),
                    host.c_str(), port,
                    args.includeHash ? "SHA-256" : "no-hash");
        auto res = wfd::tcp::SendFile(host, port, path, name, reporter, args.includeHash);
        if (res.ok) {
            std::printf("OK   %s  %s\n", name.c_str(),
                        wfd::util::HumanSize(res.bytesSent).c_str());
            if (!res.senderHash.empty())
                std::printf("     sha256: %s\n", res.senderHash.c_str());
        } else {
            std::fprintf(stderr, "FAIL %s  %s\n", name.c_str(), res.error.c_str());
            rc = 1;
        }
    }
    return rc;
}

int DoSendBt(const Args& args) {
    if (!wfd::bt::IsSupported()) {
        std::fprintf(stderr, "Bluetooth not compiled in on this build.\n");
        return 3;
    }
    if (args.to.empty()) {
        std::fprintf(stderr, "error: --to is required (Bluetooth MAC)\n");
        return 2;
    }
    if (args.files.empty()) {
        std::fprintf(stderr, "error: at least one file is required\n");
        return 2;
    }
    int rc = 0;
    for (const auto& path : args.files) {
        if (!wfd::platform::IsReadableFile(path)) {
            std::fprintf(stderr, "error: cannot read %s\n", path.c_str());
            rc = 1;
            continue;
        }
        std::string name = wfd::platform::Basename(path);
        auto reporter = wfd::progress::MakeReporter(name);
        std::printf("Sending %s over Bluetooth -> %s  [%s]\n", name.c_str(),
                    args.to.c_str(), args.includeHash ? "SHA-256" : "no-hash");
        auto res = wfd::bt::SendFile(args.to, path, name, reporter, args.includeHash);
        if (res.ok) {
            std::printf("OK   %s  %s\n", name.c_str(),
                        wfd::util::HumanSize(res.bytesSent).c_str());
            if (!res.senderHash.empty())
                std::printf("     sha256: %s\n", res.senderHash.c_str());
        } else {
            std::fprintf(stderr, "FAIL %s  %s\n", name.c_str(), res.error.c_str());
            rc = 1;
        }
    }
    return rc;
}

int DoRecv(const Args& args) {
    std::string dir = args.dir.empty() ? wfd::platform::DefaultReceiveDir() : args.dir;
    int port = args.port > 0 ? args.port : wfd::tcp::kDefaultFilePort;

    wfd::tcp::TcpListener listener;
    auto cb = [](const wfd::tcp::ReceivedFile& rf) {
        if (!rf.errorMessage.empty()) {
            std::fprintf(stderr, "[recv] FAIL from %s: %s\n",
                         rf.peerHost.c_str(), rf.errorMessage.c_str());
        } else {
            std::printf("[recv] OK  %s  %s  from %s  ->  %s\n",
                        rf.fileName.c_str(),
                        wfd::util::HumanSize(rf.fileSize).c_str(),
                        rf.peerHost.c_str(),
                        rf.savedPath.c_str());
            if (!rf.receivedHash.empty()) {
                std::printf("       sha256: %s  %s\n", rf.receivedHash.c_str(),
                            rf.integrityVerified ? "[verified]" : "[MISMATCH]");
            }
        }
        std::fflush(stdout);
    };
    if (!listener.Start(port, dir, cb)) {
        std::fprintf(stderr, "error: cannot listen on port %d\n", port);
        return 1;
    }
    std::printf("Receiving into %s on port %d (Ctrl-C to stop) ...\n",
                dir.c_str(), listener.port());
    InstallSigintHandler();
    WaitForInterrupt();
    listener.Stop();
    std::printf("\nStopped.\n");
    return 0;
}

int DoRecvBt(const Args& args) {
    if (!wfd::bt::IsSupported()) {
        std::fprintf(stderr, "Bluetooth not compiled in on this build.\n");
        return 3;
    }
    std::string dir = args.dir.empty() ? wfd::platform::DefaultReceiveDir() : args.dir;
    wfd::bt::BluetoothListener listener;
    auto cb = [](const wfd::bt::ReceivedFile& rf) {
        if (!rf.errorMessage.empty()) {
            std::fprintf(stderr, "[bt-recv] FAIL from %s: %s\n",
                         rf.peerAddress.c_str(), rf.errorMessage.c_str());
        } else {
            std::printf("[bt-recv] OK  %s  %s  from %s  ->  %s\n",
                        rf.fileName.c_str(),
                        wfd::util::HumanSize(rf.fileSize).c_str(),
                        rf.peerAddress.c_str(),
                        rf.savedPath.c_str());
            if (!rf.receivedHash.empty()) {
                std::printf("          sha256: %s  %s\n", rf.receivedHash.c_str(),
                            rf.integrityVerified ? "[verified]" : "[MISMATCH]");
            }
        }
        std::fflush(stdout);
    };
    if (!listener.Start(dir, cb)) {
        std::fprintf(stderr, "error: cannot start Bluetooth listener\n");
        return 1;
    }
    std::printf("Receiving over Bluetooth into %s (Ctrl-C to stop) ...\n", dir.c_str());
    InstallSigintHandler();
    WaitForInterrupt();
    listener.Stop();
    std::printf("\nStopped.\n");
    return 0;
}

int DoDiscover(const Args& args) {
    int secs = args.seconds > 0 ? args.seconds : 8;
    std::printf("Listening for beacons (%d s) ...\n", secs);
    auto peers = wfd::discovery::Listen(secs);
    if (peers.empty()) {
        std::printf("No peers found.\n");
        return 1;
    }
    std::printf("%-24s  %s\n", "HOST", "IP");
    std::printf("%-24s  %s\n", "------------------------", "---------------");
    for (const auto& p : peers) {
        std::printf("%-24s  %s\n", p.host.c_str(), p.ip.c_str());
    }
    return 0;
}

int DoBeacon() {
    wfd::discovery::Beacon b;
    if (!b.Start()) {
        std::fprintf(stderr, "error: cannot start beacon\n");
        return 1;
    }
    std::printf("Beaconing on port %d (Ctrl-C to stop) ...\n",
                wfd::discovery::kBeaconPort);
    InstallSigintHandler();
    WaitForInterrupt();
    b.Stop();
    std::printf("\nStopped.\n");
    return 0;
}

int DoListBt() {
    if (!wfd::bt::IsSupported()) {
        std::fprintf(stderr, "Bluetooth not compiled in on this build.\n");
        return 3;
    }
    auto devices = wfd::bt::EnumeratePairedDevices();
    if (devices.empty()) {
        std::printf("No paired devices.\n");
        return 1;
    }
    std::printf("%-18s  %s\n", "ADDRESS", "NAME");
    std::printf("%-18s  %s\n", "------------------", "----------------------------");
    for (const auto& d : devices) {
        std::printf("%-18s  %s\n", d.address.c_str(), d.name.c_str());
    }
    return 0;
}

int DoHash(const Args& args) {
    if (args.files.empty()) {
        std::fprintf(stderr, "error: at least one file is required\n");
        return 2;
    }
    int rc = 0;
    for (const auto& path : args.files) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "error: cannot open %s\n", path.c_str());
            rc = 1;
            continue;
        }
        wfd::hash::Sha256Stream h;
        char buf[64 * 1024];
        while (in) {
            in.read(buf, sizeof(buf));
            auto got = static_cast<size_t>(in.gcount());
            if (got > 0) h.Update(buf, got);
        }
        std::printf("%s  %s\n", h.FinishHex().c_str(), path.c_str());
    }
    return rc;
}

} // namespace

int main(int argc, char** argv) {
    Args args;
    if (!ParseArgs(argc, argv, args)) {
        PrintUsage();
        return 2;
    }
    wfd::platform::EnableAnsiIfNeeded();
    wfd::platform::InitNetworking();

    int rc;
    if (args.command == "send")          rc = DoSend(args);
    else if (args.command == "send-bt")  rc = DoSendBt(args);
    else if (args.command == "recv")     rc = DoRecv(args);
    else if (args.command == "recv-bt")  rc = DoRecvBt(args);
    else if (args.command == "discover") rc = DoDiscover(args);
    else if (args.command == "beacon")   rc = DoBeacon();
    else if (args.command == "list-bt")  rc = DoListBt();
    else if (args.command == "hash")     rc = DoHash(args);
    else {
        PrintUsage();
        rc = 2;
    }

    wfd::platform::ShutdownNetworking();
    return rc;
}
