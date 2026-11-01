// tui.cpp — ncurses interactive front-end for the wfd_transfer CLI.
//
// Built only when libncursesw / ncurses is found (Linux). The Windows desktop
// client is `wfd_gui`; PDCurses is deliberately not wired up. Invoke via:
//
//   wfd_tui [--dir <receive-dir>] [--port <tcp-port>]
//
// Layout (top-down):
//   [Peers]      discovered peers, pick one with Enter to send.
//   [Send Queue] files queued this session, with progress bars.
//   [Recv Log]   files that arrived over TCP during this session.
//   [Hotkeys]    bottom hint line.
//
// All of the interesting logic — event coalescing, terminal-state protection,
// rates, ETA, row caps — lives in UiModel and is unit-tested on Linux. What is
// left here is ncurses plumbing: a single thread drives `getch()`, background
// listeners and senders post events into the model, and every frame the UI
// thread pumps the model and paints a value-copy snapshot.
#include "Discovery.h"
#include "Platform.h"
#include "TcpTransport.h"
#include "UiModel.h"
#include "Util.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <ncurses.h>

namespace {

using wfd::ui::Config;
using wfd::ui::LogRow;
using wfd::ui::PeerRow;
using wfd::ui::RecvRow;
using wfd::ui::Snapshot;
using wfd::ui::TaskId;
using wfd::ui::TaskRow;
using wfd::ui::TaskState;
using wfd::ui::Transport;
using wfd::ui::UiEvent;
using wfd::ui::IsTerminal;
using wfd::ui::UiModel;

std::atomic<bool> g_quit{false};

// Monotonic clock with an arbitrary zero, so SpeedTracker sees small numbers.
const std::chrono::steady_clock::time_point g_epoch =
    std::chrono::steady_clock::now();

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - g_epoch).count();
}

// ---- Background producers -------------------------------------------------

void RunDiscovery(std::shared_ptr<UiModel> model) {
    using namespace wfd::discovery;
    Beacon beacon;
    beacon.Start();
    UiEvent up;
    up.kind = UiEvent::Kind::BeaconStarted;
    model->Post(std::move(up));
    while (!g_quit.load()) {
        auto peers = Listen(3);
        for (auto& p : peers) {
            UiEvent ev;
            ev.kind = UiEvent::Kind::PeerSeen;
            ev.transport = Transport::Tcp;
            ev.peerKind = wfd::ui::PeerKind::Lan;
            ev.key = p.ip;
            ev.name = p.host;
            ev.atMs = NowMs();
            model->Post(std::move(ev));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    beacon.Stop();
}

void RunTcpListener(std::shared_ptr<UiModel> model, int port,
                    const std::string& dir) {
    using namespace wfd::tcp;
    TcpListener listener;
    auto started = listener.Start(
        port, dir,
        [model](const ReceivedFile& rf) {
            UiEvent ev;
            ev.kind = rf.errorMessage.empty() ? UiEvent::Kind::RecvCompleted
                                              : UiEvent::Kind::RecvFailed;
            ev.transport = Transport::Tcp;
            ev.connId = rf.connId;
            ev.key = rf.peerHost;
            ev.name = rf.fileName;
            ev.path = rf.savedPath;
            ev.detail = rf.errorMessage;
            ev.sha256 = rf.receivedHash;
            ev.size = rf.fileSize;
            ev.integrityVerified = rf.integrityVerified;
            model->Post(std::move(ev));
        },
        [model](const RecvUpdate& u) {
            if (u.stage == RecvUpdate::Stage::Started) {
                UiEvent ev;
                ev.kind = UiEvent::Kind::RecvStarted;
                ev.transport = Transport::Tcp;
                ev.connId = u.connId;
                ev.key = u.peerHost;
                ev.name = u.fileName;
                ev.size = u.total;
                model->Post(std::move(ev));
            } else {
                model->PostRecvProgress(Transport::Tcp, u.connId, u.done,
                                        u.total);
            }
        });
    if (!started) {
        UiEvent ev;
        ev.kind = UiEvent::Kind::TcpListenFailed;
        ev.port = port;
        ev.detail = "cannot bind port " + std::to_string(port);
        model->Post(std::move(ev));
        return;
    }
    UiEvent ev;
    ev.kind = UiEvent::Kind::TcpListenStarted;
    ev.port = listener.port();
    model->Post(std::move(ev));

    while (!g_quit.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    listener.Stop();
}

// One detached thread per transfer. It captures the model by value only: the
// listener's per-connection handlers outlive Stop(), so nothing here may hold
// a reference to UI state.
void StartSend(std::shared_ptr<UiModel> model, TaskId id, std::string peer,
               std::string sourcePath, std::string name,
               std::shared_ptr<std::atomic<bool>> token) {
    std::thread([model, id, peer = std::move(peer),
                 sourcePath = std::move(sourcePath), name = std::move(name),
                 token = std::move(token)] {
        auto onProgress = [model, id](int64_t done, int64_t total) {
            model->PostProgress(id, done, total);
        };

        auto res = wfd::tcp::SendFile(peer, wfd::tcp::kDefaultFilePort,
                                     sourcePath, name, onProgress, true,
                                     token.get());
        UiEvent done;
        done.id = id;
        done.name = name;
        if (res.ok) {
            done.kind = UiEvent::Kind::TaskCompleted;
            done.size = res.bytesSent;
            done.sha256 = res.senderHash;
        } else if (res.error == "canceled") {
            done.kind = UiEvent::Kind::TaskCanceled;
        } else {
            done.kind = UiEvent::Kind::TaskFailed;
            done.detail = res.error;
        }
        model->Post(std::move(done));
    }).detach();
}

// ---- Drawing --------------------------------------------------------------

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

const char* StateTag(TaskState s) {
    switch (s) {
        case TaskState::Queued:    return "WAIT";
        case TaskState::Running:   return "SEND";
        case TaskState::Completed: return "OK  ";
        case TaskState::Failed:    return "FAIL";
        case TaskState::Canceled:  return "CNCL";
    }
    return "    ";
}

void DrawHeader(const Snapshot& s) {
    attron(A_REVERSE);
    for (int c = 0; c < COLS; ++c) mvaddch(0, c, ' ');
    const std::string listen = s.tcpListening
        ? "on :" + std::to_string(s.tcpPort)
        : std::string("off");
    mvprintw(0, 0,
             " wfd_tui  —  peers:%zu  sends:%zu  recvs:%zu  tcp:%s  recv-> %s ",
             s.peers.size(), s.sends.size(), s.recvs.size(), listen.c_str(),
             s.receiveDir.c_str());
    attroff(A_REVERSE);
}

// Draws `rows` of `label`, highlighting the selected one when `focused`.
template <class Row, class Painter>
void DrawPane(const char* label, int top, int height, const std::vector<Row>& rows,
              bool focused, int selected, Painter paint) {
    if (focused) attron(A_BOLD);
    mvprintw(top, 0, "%s", label);
    if (focused) attroff(A_BOLD);
    clrtoeol();
    const int slots = height - 1;
    // Keep the selected row in view when the pane overflows.
    int first = 0;
    if (focused && selected >= slots) first = selected - slots + 1;
    for (int i = first; i < static_cast<int>(rows.size()) && i - first < slots; ++i) {
        const int row = top + 1 + (i - first);
        if (focused && i == selected) attron(A_REVERSE);
        paint(row, rows[static_cast<size_t>(i)]);
        if (focused && i == selected) attroff(A_REVERSE);
        clrtoeol();
    }
}

void PaintPeer(int row, const PeerRow& p) {
    mvprintw(row, 0, " %-24s  %s%s", p.name.empty() ? "?" : p.name.c_str(),
             p.key.c_str(), p.stale ? "  (stale)" : "");
}

void PaintSend(int row, const TaskRow& r) {
    mvprintw(row, 0, " %s ", StateTag(r.state));
    printw("%-20.20s -> %-15.15s  ", r.name.c_str(), r.peer.c_str());
    const int col = getcurx(stdscr);
    RenderBar(row, col, 20, r.pct);
    printw("  %3d%%  %s/%s", r.pct, wfd::util::HumanSize(r.done).c_str(),
           wfd::util::HumanSize(r.total).c_str());
    if (r.state == TaskState::Running && r.bytesPerSec > 0) {
        printw("  %s/s", wfd::util::HumanSize(r.bytesPerSec).c_str());
        if (r.etaSec >= 0) printw("  eta %llds", static_cast<long long>(r.etaSec));
    }
    if (r.cancelRequested && r.state == TaskState::Running) printw("  canceling…");
    if (r.state == TaskState::Failed && !r.detail.empty()) {
        printw("  %s", r.detail.c_str());
    }
}

void PaintRecv(int row, const RecvRow& r) {
    mvprintw(row, 0, " %s %-18.18s  %s  from %s",
             StateTag(r.state), r.name.c_str(),
             wfd::util::HumanSize(r.size).c_str(), r.peer.c_str());
    if (r.state == TaskState::Running) {
        const int col = getcurx(stdscr);
        RenderBar(row, col, 14, r.pct);
        printw("  %3d%%", r.pct);
        if (r.bytesPerSec > 0) printw("  %s/s", wfd::util::HumanSize(r.bytesPerSec).c_str());
    } else if (!r.error.empty()) {
        printw("  ->  %s", r.error.c_str());
    } else {
        printw("  ->  %s%s", r.savedPath.c_str(),
               r.integrityVerified ? "  [sha256 verified]" : "");
    }
}

void PaintLog(int row, const LogRow& l) {
    mvprintw(row, 0, " %s", l.text.c_str());
}

void DrawHotkeys(int row) {
    mvprintw(row, 0,
             "[TAB] focus  [UP/DN] select  [s] queue-file  [Enter] send/retry  "
             "[x] cancel  [q] quit");
    clrtoeol();
}

void DrawLogs(const Snapshot& s, int top, int height) {
    const int slots = std::max(1, height - 1);
    mvprintw(top, 0, "Log:");
    clrtoeol();
    int first = std::max(0, static_cast<int>(s.logs.size()) - slots);
    for (int i = first; i < static_cast<int>(s.logs.size()) && i - first < slots; ++i) {
        PaintLog(top + 1 + (i - first), s.logs[static_cast<size_t>(i)]);
    }
}

std::string PromptLine(int row, const char* prompt) {
    // The main loop runs with timeout(100) so the screen keeps updating while
    // nothing is typed, and getnstr inherits it: any pause longer than 100 ms
    // mid-path would return a truncated string. Prompts are modal, so make the
    // read blocking and restore the tick afterwards.
    timeout(-1);
    echo();
    curs_set(1);
    char buf[1024] = {0};
    mvprintw(row, 0, "%s", prompt);
    clrtoeol();
    refresh();
    getnstr(buf, static_cast<int>(sizeof(buf) - 1));
    noecho();
    curs_set(0);
    timeout(100);
    return std::string(buf);
}

struct Cursor {
    int focus = 0;                          // 0=peers, 1=sends, 2=recvs
    std::array<int, 3> sel{};               // one cursor per pane, remembered
};

// Validates and creates a Queued row. An empty `peer` is rejected by the model,
// which logs why — the tui used to fall back silently to peers[0] and ship a
// file to someone the user never picked.
TaskId MakeTask(const std::shared_ptr<UiModel>& model, const std::string& path,
                const std::string& peer) {
    if (!wfd::platform::IsReadableFile(path)) {
        UiEvent ev;
        ev.kind = UiEvent::Kind::Info;
        ev.detail = "cannot read: " + path;
        model->Post(std::move(ev));
        return 0;
    }
    int64_t size = 0;
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (in) size = static_cast<int64_t>(in.tellg());
    }
    TaskRow seed;
    seed.name = wfd::platform::Basename(path);
    seed.sourcePath = path;
    seed.peer = peer;
    seed.transport = Transport::Tcp;
    seed.total = size;
    return model->EnqueueSend(seed, NowMs());
}

void Launch(const std::shared_ptr<UiModel>& model, TaskId id,
            const std::string& peer, const std::string& sourcePath,
            const std::string& name) {
    if (id == 0) return;
    // TaskStarted is posted here, on the UI thread, rather than by the worker:
    // the row must read Sending before the next key press is handled, or a
    // second Enter on the same row would start a second thread for one task.
    UiEvent started;
    started.kind = UiEvent::Kind::TaskStarted;
    started.id = id;
    started.name = name;
    started.path = sourcePath;
    started.atMs = NowMs();
    model->Post(std::move(started));
    model->Pump(NowMs());
    StartSend(model, id, peer, sourcePath, name, model->CancelToken(id));
}

// The peer the send panes should target: the Peers-pane cursor, which stays
// valid while focus is elsewhere so `s` and Enter agree on one destination.
std::string SelectedPeer(const Snapshot& s, const Cursor& cur) {
    if (cur.sel[0] < 0 || cur.sel[0] >= static_cast<int>(s.peers.size())) return {};
    return s.peers[static_cast<size_t>(cur.sel[0])].key;
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

    // The tui has always kept every peer it ever saw, so expiry is off.
    Config cfg;
    cfg.maxLogRows = 50;
    cfg.peerStaleMs = 0;
    cfg.peerTtlMs = 0;
    auto model = std::make_shared<UiModel>(cfg);
    model->SetReceiveDir(dir);

    wfd::platform::InitNetworking();
    initscr();
    noecho();
    cbreak();
    curs_set(0);
    keypad(stdscr, TRUE);
    timeout(100);

    std::thread discovery_thread(RunDiscovery, model);
    std::thread tcp_thread(RunTcpListener, model, port, dir);

    Cursor cur;
    Snapshot snap = model->Take();
    bool quit = false;
    while (!quit) {
        model->Pump(NowMs());
        snap = model->Take();

        erase();
        DrawHeader(snap);
        const int peers_h = std::max(4, LINES / 4);
        const int sends_h = std::max(4, LINES / 4);
        int recvs_h = LINES - peers_h - sends_h - 4;
        if (recvs_h < 3) recvs_h = 3;
        const int logs_h = std::max(2, LINES - 1 - (1 + peers_h + sends_h + recvs_h));
        int& sel = cur.sel[cur.focus];
        DrawPane("Peers (TAB to focus, Enter to send):", 1, peers_h, snap.peers,
                 cur.focus == 0, cur.sel[0], PaintPeer);
        DrawPane("Send queue:", 1 + peers_h, sends_h, snap.sends,
                 cur.focus == 1, cur.sel[1], PaintSend);
        DrawPane("Received:", 1 + peers_h + sends_h, recvs_h, snap.recvs,
                 cur.focus == 2, cur.sel[2], PaintRecv);
        DrawLogs(snap, 1 + peers_h + sends_h + recvs_h, logs_h);
        DrawHotkeys(LINES - 1);
        refresh();

        const int ch = getch();
        if (ch == ERR) continue;  // nothing typed; the next frame pumps again

        const int rowCount = cur.focus == 0 ? static_cast<int>(snap.peers.size())
                           : cur.focus == 1 ? static_cast<int>(snap.sends.size())
                                            : static_cast<int>(snap.recvs.size());
        switch (ch) {
        case 'q':
            quit = true;
            g_quit.store(true);
            break;
        case '\t':
            cur.focus = (cur.focus + 1) % 3;
            break;
        case KEY_UP:
            if (sel > 0) --sel;
            break;
        case KEY_DOWN:
            if (sel + 1 < rowCount) ++sel;
            break;
        case 'x': {
            // Only the send pane has something a token can actually stop.
            if (cur.focus == 1 && sel < static_cast<int>(snap.sends.size())) {
                const TaskRow& r = snap.sends[static_cast<size_t>(sel)];
                if (!model->TryCancel(r.id)) {
                    UiEvent ev;
                    ev.kind = UiEvent::Kind::Info;
                    ev.detail = "nothing to cancel: " + r.name;
                    model->Post(std::move(ev));
                }
            }
            break;
        }
        case 's': {
            const std::string path = PromptLine(LINES - 2, "Path of file to queue: ");
            if (path.empty()) break;
            MakeTask(model, path, SelectedPeer(snap, cur));  // stays WAIT until Enter
            break;
        }
        case '\n':
        case KEY_ENTER: {
            if (cur.focus == 0 && sel < static_cast<int>(snap.peers.size())) {
                const std::string path =
                    PromptLine(LINES - 2, "Path of file to send: ");
                if (path.empty()) break;
                const std::string peer = snap.peers[static_cast<size_t>(sel)].key;
                const TaskId id = MakeTask(model, path, peer);
                Launch(model, id, peer, path, wfd::platform::Basename(path));
            } else if (cur.focus == 1 && sel < static_cast<int>(snap.sends.size())) {
                const TaskRow row = snap.sends[static_cast<size_t>(sel)];
                if (row.state == TaskState::Queued) {
                    Launch(model, row.id, row.peer, row.sourcePath, row.name);
                } else if (IsTerminal(row.state)) {
                    // Retry the row the cursor is actually on, not whichever
                    // happens to be newest.
                    const TaskId id = MakeTask(model, row.sourcePath, row.peer);
                    Launch(model, id, row.peer, row.sourcePath, row.name);
                }
            }
            break;
        }
        default: break;
        }
        // Rows can disappear under the cursor (caps, expiry); keep it in range.
        for (int& v : cur.sel) v = std::max(0, v);
    }

    g_quit.store(true);
    endwin();
    discovery_thread.join();
    tcp_thread.join();
    wfd::platform::ShutdownNetworking();
    return 0;
}
