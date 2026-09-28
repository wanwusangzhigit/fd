// tui.cpp — ncurses interactive front-end for the wfd_transfer CLI.
//
// Built only when libncursesw / ncurses is found (Linux; PDCurses port on
// Windows is possible but not wired up yet). Invoke via:
//
//   wfd_tui [--dir <receive-dir>] [--port <tcp-port>]
//
// Layout (top-down):
//   [Peers]      discovered peers, pick one with Enter to send.
//   [Send Queue] files dragged onto the command line, with progress bars.
//   [Recv Log]   received-file rows that arrived over TCP during this session.
//   [Hotkeys]    bottom hint line.
//
// The UI is deliberately simple: a single thread drives ncurses via
// `getch()`; background TCP listeners and sends run on their own threads
// and post updates onto a mutex-protected event queue that the UI thread
// drains every 100 ms via `timeout(100)`.
#include "BluetoothTransport.h"
#include "Discovery.h"
#include "Platform.h"
#include "Progress.h"
#include "TcpTransport.h"
#include "Util.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <ncurses.h>

namespace {

// ---- Event queue: producers (listener / sender threads), consumer (UI) ----

struct UiEvent {
    enum class Kind {
        PeerDiscovered,
        SendStarted,
        SendProgress,
        SendCompleted,
        SendFailed,
        RecvOk,
        RecvFail,
        Info,
    } kind = Kind::Info;

    std::string name;
    std::string peer;
    int64_t a = 0;     // transferred or size
    int64_t b = 0;     // total
    std::string extra; // saved path / error
};

std::mutex g_evtMutex;
std::queue<UiEvent> g_evtQueue;

void PostEvent(UiEvent e) {
    std::lock_guard<std::mutex> lk(g_evtMutex);
    g_evtQueue.push(std::move(e));
}

bool DrainEvent(UiEvent& out) {
    std::lock_guard<std::mutex> lk(g_evtMutex);
    if (g_evtQueue.empty()) return false;
    out = std::move(g_evtQueue.front());
    g_evtQueue.pop();
    return true;
}

// ---- Application state ----------------------------------------------------

struct PeerRow {
    std::string host; // hostname
    std::string ip;
};

struct SendRow {
    std::string name;
    std::string peer;
    int64_t total = 0;
    int64_t done = 0;
    int pct = 0;
    enum State { Pending, Sending, Done, Failed } state = Pending;
    std::string detail;
};

struct RecvRow {
    std::string peer;
    std::string name;
    int64_t size = 0;
    std::string path;
    std::string err;
    bool ok = false;
};

struct AppState {
    std::vector<PeerRow> peers;
    std::vector<SendRow> sends;
    std::vector<RecvRow> recvs;
    std::vector<std::string> logs;
    int selected = 0;
    int focus = 0; // 0=peers, 1=sends, 2=recvs
    bool quit = false;
};

// Pretty-printers ----------------------------------------------------------

void RenderBar(int row, int col, int width, int pct) {
    if (width < 2) return;
    pct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    int filled = (width - 2) * pct / 100;
    mvaddch(row, col, '[');
    for (int i = 0; i < width - 2; ++i) {
        addch(i < filled ? '#' : ' ');
    }
    addch(']');
}

const char* StateTag(SendRow::State s) {
    switch (s) {
        case SendRow::Pending: return "WAIT";
        case SendRow::Sending: return "SEND";
        case SendRow::Done:    return "OK  ";
        case SendRow::Failed:  return "FAIL";
    }
    return "    ";
}

// Background tasks ---------------------------------------------------------

std::atomic<bool> g_state_quit{false};

void RunDiscovery() {
    using namespace wfd::discovery;
    Beacon beacon;
    beacon.Start();
    while (!g_state_quit.load()) {
        // Run a single short listen cycle so we keep picking up new arrivals.
        auto peers = Listen(3);
        for (auto& p : peers) {
            UiEvent e;
            e.kind = UiEvent::Kind::PeerDiscovered;
            e.name = p.host;
            e.peer = p.ip;
            PostEvent(std::move(e));
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    beacon.Stop();
}

void RunTcpListener(int port, const std::string& dir) {
    using namespace wfd::tcp;
    TcpListener listener;
    listener.Start(port, dir, [](const ReceivedFile& rf) {
        UiEvent e;
        if (!rf.errorMessage.empty()) {
            e.kind = UiEvent::Kind::RecvFail;
            e.peer = rf.peerHost;
            e.extra = rf.errorMessage;
        } else {
            e.kind = UiEvent::Kind::RecvOk;
            e.peer = rf.peerHost;
            e.name = rf.fileName;
            e.a = rf.fileSize;
            e.extra = rf.savedPath;
        }
        PostEvent(std::move(e));
    });
    while (!g_state_quit.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    listener.Stop();
}

void StartSend(SendRow& row) {
    std::thread([row]() mutable {
        UiEvent started;
        started.kind = UiEvent::Kind::SendStarted;
        started.name = row.name;
        started.peer = row.peer;
        PostEvent(started);

        auto onProgress = [row](int64_t done, int64_t total) mutable {
            UiEvent e;
            e.kind = UiEvent::Kind::SendProgress;
            e.name = row.name;
            e.a = done;
            e.b = total;
            PostEvent(std::move(e));
        };

        std::string path = row.detail; // we stash source path in `detail`
        auto res = wfd::tcp::SendFile(row.peer, wfd::tcp::kDefaultFilePort,
                                      path, row.name, onProgress);
        UiEvent done;
        done.name = row.name;
        done.peer = row.peer;
        if (res.ok) {
            done.kind = UiEvent::Kind::SendCompleted;
            done.a = res.bytesSent;
        } else {
            done.kind = UiEvent::Kind::SendFailed;
            done.extra = res.error;
        }
        PostEvent(done);
    }).detach();
}

// Apply event ---------------------------------------------------------------

void ApplyEvent(AppState& s, const UiEvent& e) {
    switch (e.kind) {
    case UiEvent::Kind::PeerDiscovered: {
        for (auto& p : s.peers) {
            if (p.ip == e.peer) { p.host = e.name; return; }
        }
        s.peers.push_back({e.name, e.peer});
        break;
    }
    case UiEvent::Kind::SendStarted: {
        for (auto& r : s.sends) {
            if (r.name == e.name && r.peer == e.peer) r.state = SendRow::Sending;
        }
        break;
    }
    case UiEvent::Kind::SendProgress: {
        for (auto& r : s.sends) {
            if (r.name == e.name && r.state == SendRow::Sending) {
                r.done = e.a;
                r.total = e.b > 0 ? e.b : r.total;
                r.pct = r.total > 0 ? static_cast<int>(r.done * 100 / r.total) : 0;
            }
        }
        break;
    }
    case UiEvent::Kind::SendCompleted: {
        for (auto& r : s.sends) {
            if (r.name == e.name && r.state == SendRow::Sending) {
                r.state = SendRow::Done;
                r.done = r.total;
                r.pct = 100;
            }
        }
        break;
    }
    case UiEvent::Kind::SendFailed: {
        for (auto& r : s.sends) {
            if (r.name == e.name && r.state == SendRow::Sending) {
                r.state = SendRow::Failed;
                r.detail = e.extra;
            }
        }
        break;
    }
    case UiEvent::Kind::RecvOk:
        s.recvs.push_back({e.peer, e.name, e.a, e.extra, "", true});
        break;
    case UiEvent::Kind::RecvFail:
        s.recvs.push_back({e.peer, "", 0, "", e.extra, false});
        break;
    case UiEvent::Kind::Info:
        s.logs.push_back(e.extra);
        if (s.logs.size() > 50) s.logs.erase(s.logs.begin());
        break;
    }
}

// Drawing -------------------------------------------------------------------

void DrawHeader(const AppState& s) {
    attron(A_REVERSE);
    for (int c = 0; c < COLS; ++c) mvaddch(0, c, ' ');
    mvprintw(0, 0, " wfd_tui  —  peers:%zu  sends:%zu  recvs:%zu ",
             s.peers.size(), s.sends.size(), s.recvs.size());
    attroff(A_REVERSE);
}

void DrawPeers(const AppState& s, int top, int height) {
    mvprintw(top, 0, "Peers (TAB to focus, 'r' to refresh, Enter to send):");
    for (size_t i = 0; i < s.peers.size() && static_cast<int>(i) < height - 1; ++i) {
        const auto& p = s.peers[i];
        bool sel = s.focus == 0 && static_cast<int>(i) == s.selected;
        if (sel) attron(A_REVERSE);
        mvprintw(top + 1 + static_cast<int>(i), 0, " %-24s  %s",
                 p.host.c_str(), p.ip.c_str());
        if (sel) attroff(A_REVERSE);
        clrtoeol();
    }
}

void DrawSends(const AppState& s, int top, int height) {
    mvprintw(top, 0, "Send queue:");
    for (size_t i = 0; i < s.sends.size() && static_cast<int>(i) < height - 1; ++i) {
        const auto& r = s.sends[i];
        int row = top + 1 + static_cast<int>(i);
        mvprintw(row, 0, " %s ", StateTag(r.state));
        printw("%-20.20s -> %-15.15s  ", r.name.c_str(), r.peer.c_str());
        int col = getcurx(stdscr);
        RenderBar(row, col, 20, r.pct);
        printw("  %3d%%  %s/%s",
               r.pct,
               wfd::util::HumanSize(r.done).c_str(),
               wfd::util::HumanSize(r.total).c_str());
        clrtoeol();
    }
}

void DrawRecvs(const AppState& s, int top, int height) {
    mvprintw(top, 0, "Received:");
    int slots = height - 1;
    int start = static_cast<int>(s.recvs.size()) > slots
                ? static_cast<int>(s.recvs.size()) - slots : 0;
    for (int i = 0; i + start < static_cast<int>(s.recvs.size()) && i < slots; ++i) {
        const auto& r = s.recvs[i + start];
        int row = top + 1 + i;
        if (r.ok) {
            mvprintw(row, 0, " OK   %-18.18s  %-9s  from %s  ->  %s",
                     r.name.c_str(),
                     wfd::util::HumanSize(r.size).c_str(),
                     r.peer.c_str(),
                     r.path.c_str());
        } else {
            mvprintw(row, 0, " FAIL from %s: %s",
                     r.peer.c_str(), r.err.c_str());
        }
        clrtoeol();
    }
}

void DrawHotkeys(int row) {
    mvprintw(row, 0,
             "[TAB] focus  [r] refresh  [s] send-file  [Enter] send-to-selected  [q] quit");
    clrtoeol();
}

// Input handling ------------------------------------------------------------

std::string PromptLine(int row, const char* prompt) {
    echo();
    curs_set(1);
    char buf[1024] = {0};
    mvprintw(row, 0, "%s", prompt);
    clrtoeol();
    getnstr(buf, static_cast<int>(sizeof(buf) - 1));
    noecho();
    curs_set(0);
    return std::string(buf);
}

} // namespace

int main(int argc, char** argv) {
    std::string dir = wfd::platform::DefaultReceiveDir();
    int port = wfd::tcp::kDefaultFilePort;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--dir" && i + 1 < argc) dir = argv[++i];
        else if (a == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
    }

    wfd::platform::InitNetworking();
    initscr();
    noecho();
    cbreak();
    curs_set(0);
    keypad(stdscr, TRUE);
    timeout(100);

    AppState s;

    std::thread discovery_thread(RunDiscovery);
    std::thread tcp_thread(RunTcpListener, port, dir);

    while (!s.quit) {
        erase();
        DrawHeader(s);
        int peers_h = std::max(4, LINES / 4);
        int sends_h = std::max(4, LINES / 4);
        int recvs_h = LINES - peers_h - sends_h - 3 /*separators+hotkeys*/ - 1;
        if (recvs_h < 3) recvs_h = 3;
        DrawPeers(s, 1, peers_h);
        DrawSends(s, 1 + peers_h, sends_h);
        DrawRecvs(s, 1 + peers_h + sends_h, recvs_h);
        DrawHotkeys(LINES - 1);
        refresh();

        int ch = getch();
        if (ch == ERR) {
            // drain events
            UiEvent e;
            while (DrainEvent(e)) ApplyEvent(s, e);
            continue;
        }
        UiEvent e;
        while (DrainEvent(e)) ApplyEvent(s, e);

        switch (ch) {
        case 'q':
            s.quit = true;
            g_state_quit.store(true);
            break;
        case '\t':
            s.focus = (s.focus + 1) % 3;
            s.selected = 0;
            break;
        case 'r':
            // Discovery thread loops continuously so nothing fancy needed.
            break;
        case KEY_UP:
            if (s.selected > 0) --s.selected;
            break;
        case KEY_DOWN: {
            int maxSel = 0;
            if (s.focus == 0) maxSel = static_cast<int>(s.peers.size()) - 1;
            if (s.selected < maxSel) ++s.selected;
            break;
        }
        case 's': {
            std::string path = PromptLine(LINES - 2, "Path of file to send: ");
            if (!path.empty() && wfd::platform::IsReadableFile(path)) {
                SendRow r;
                r.name = wfd::platform::Basename(path);
                r.detail = path; // stash source path here
                r.peer = s.peers.empty() ? "" : s.peers[0].ip;
                s.sends.push_back(std::move(r));
            } else if (!path.empty()) {
                s.logs.push_back("cannot read: " + path);
            }
            break;
        }
        case '\n':
        case KEY_ENTER: {
            if (s.focus == 0 && !s.peers.empty() &&
                s.selected < static_cast<int>(s.peers.size())) {
                std::string path = PromptLine(LINES - 2, "Path of file to send: ");
                if (!path.empty() && wfd::platform::IsReadableFile(path)) {
                    SendRow r;
                    r.name = wfd::platform::Basename(path);
                    r.detail = path;
                    r.peer = s.peers[s.selected].ip;
                    s.sends.push_back(std::move(r));
                    StartSend(s.sends.back());
                }
            } else if (s.focus == 1 && !s.sends.empty()) {
                // Restart pending/failed send to selected peer.
                if (!s.peers.empty()) {
                    s.sends.back().peer = s.peers[0].ip;
                    StartSend(s.sends.back());
                }
            }
            break;
        }
        default: break;
        }
    }

    g_state_quit.store(true);
    endwin();
    discovery_thread.join();
    tcp_thread.join();
    wfd::platform::ShutdownNetworking();
    return 0;
}
