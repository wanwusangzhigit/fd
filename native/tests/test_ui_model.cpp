// test_ui_model.cpp — behavior of the shared front-end state machine.
//
// This is the only place the interactive clients' interesting logic can be
// exercised: wfd_gui.exe builds under MinGW but can never be run in CI, so
// everything worth trusting lives here and runs on Linux.
//
// Determinism comes from UiModel::Pump taking the clock as a parameter —
// no wall-clock sleeps anywhere in the rate/ETA assertions.
#undef NDEBUG  // CMake's Release adds -DNDEBUG, which would make these no-ops
#include <cassert>

#include "Platform.h"
#include "TcpTransport.h"
#include "UiModel.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace wfd;
using ui::Config;
using ui::PeerKind;
using ui::TaskId;
using ui::TaskRow;
using ui::TaskState;
using ui::Transport;
using ui::UiEvent;
using ui::UiModel;

namespace {

// --- helpers --------------------------------------------------------------

TaskRow MakeSeed(const std::string& name = "f.bin",
                 const std::string& peer = "192.168.1.5") {
    TaskRow r;
    r.name = name;
    r.sourcePath = "/tmp/" + name;
    r.peer = peer;
    r.total = 100;
    return r;
}

UiEvent Ev(UiEvent::Kind kind, TaskId id) {
    UiEvent ev;
    ev.kind = kind;
    ev.id = id;
    return ev;
}

const TaskRow* FindSend(const ui::Snapshot& s, TaskId id) {
    for (const auto& r : s.sends) {
        if (r.id == id) return &r;
    }
    return nullptr;
}

const ui::RecvRow* FindRecv(const ui::Snapshot& s, TaskId id) {
    for (const auto& r : s.recvs) {
        if (r.id == id) return &r;
    }
    return nullptr;
}

// --- 1. task identity -----------------------------------------------------
// The bug this replaces: tui.cpp matched rows by filename, so sending the same
// file twice let both rows share one progress stream and one of them hung.
void TestTaskIdentity() {
    UiModel m;
    TaskId a = m.EnqueueSend(MakeSeed(), 1000);
    TaskId b = m.EnqueueSend(MakeSeed(), 1000);
    assert(a != 0 && b != 0 && a != b);

    m.Post(Ev(UiEvent::Kind::TaskStarted, a));
    m.Post(Ev(UiEvent::Kind::TaskStarted, b));
    m.PostProgress(b, 70, 100);
    m.Post(Ev(UiEvent::Kind::TaskCompleted, a));
    m.Pump(2000);

    auto s = m.Take();
    assert(s.sends.size() == 2);
    const TaskRow* ra = FindSend(s, a);
    const TaskRow* rb = FindSend(s, b);
    assert(ra && ra->state == TaskState::Completed && ra->pct == 100);
    assert(rb && rb->state == TaskState::Running);
    assert(rb->done == 70 && rb->pct == 70);
}

// --- 2. terminal rows are not rewritten, and Completed clamps --------------
void TestTerminalGuard() {
    UiModel m;
    TaskId id = m.EnqueueSend(MakeSeed(), 0);
    m.Post(Ev(UiEvent::Kind::TaskStarted, id));
    m.PostProgress(id, 50, 100);
    m.Pump(1000);
    assert(m.Take().sends.front().done == 50);

    m.Post(Ev(UiEvent::Kind::TaskCompleted, id));
    m.PostProgress(id, 60, 100);   // late progress after terminal
    m.Pump(2000);

    auto row = m.Take().sends.front();
    assert(row.state == TaskState::Completed);
    assert(row.done == 100);       // clamped to total, not 60
    assert(row.pct == 100);

    // A failure arriving after completion must not reopen the row.
    UiEvent lateFail = Ev(UiEvent::Kind::TaskFailed, id);
    lateFail.detail = "too late";
    m.Post(lateFail);
    m.Pump(3000);
    assert(m.Take().sends.front().state == TaskState::Completed);
}

// --- 3. a progress flood cannot starve a critical event --------------------
void TestProgressCannotStarve() {
    UiModel m;
    TaskRow seed = MakeSeed();
    seed.total = 10000;
    TaskId id = m.EnqueueSend(seed, 0);
    m.Post(Ev(UiEvent::Kind::TaskStarted, id));
    for (int i = 1; i <= 10000; ++i) m.PostProgress(id, i, 10000);
    m.Post(Ev(UiEvent::Kind::TaskCompleted, id));
    m.Pump(5000);

    auto s = m.Take();
    assert(s.sends.size() == 1);
    assert(s.sends.front().state == TaskState::Completed);
    assert(s.sends.front().done == 10000);
}

// --- 4. unknown ids create nothing ----------------------------------------
void TestUnknownIdsIgnored() {
    UiModel m;
    m.PostProgress(9999, 10, 100);
    UiEvent done = Ev(UiEvent::Kind::TaskCompleted, 9998);
    m.Post(done);
    m.Post(Ev(UiEvent::Kind::TaskStarted, 9997));
    m.Pump(1000);
    auto s = m.Take();
    assert(s.sends.empty() && s.recvs.empty());
}

// --- 5. peer identity is (transport, key) ---------------------------------
void TestPeerIdentity() {
    UiModel m;
    UiEvent lan;
    lan.kind = UiEvent::Kind::PeerSeen;
    lan.transport = Transport::Tcp;
    lan.peerKind = PeerKind::Lan;
    lan.key = "192.168.1.5";
    lan.name = "desk-1";
    lan.atMs = 100;
    UiEvent bt = lan;
    bt.transport = Transport::Bluetooth;
    bt.peerKind = PeerKind::BluetoothPaired;
    bt.name = "phone-1";
    m.Post(lan);
    m.Post(bt);
    m.Pump(200);
    assert(m.Take().peers.size() == 2);  // same string, different transport

    UiEvent renamed = lan;
    renamed.name = "desk-1-renamed";
    renamed.atMs = 300;
    m.Post(renamed);
    m.Pump(400);
    auto s = m.Take();
    assert(s.peers.size() == 2);         // hostname change updates, not adds
    const ui::PeerRow* lanRow = nullptr;
    for (const auto& p : s.peers) {
        if (p.transport == Transport::Tcp) lanRow = &p;
    }
    assert(lanRow && lanRow->name == "desk-1-renamed" && !lanRow->stale);

    UiEvent gone;
    gone.kind = UiEvent::Kind::PeerGone;
    gone.transport = Transport::Tcp;
    gone.key = "192.168.1.5";
    m.Post(gone);
    m.Pump(500);
    for (const auto& p : m.Take().peers) {
        if (p.transport == Transport::Tcp) assert(p.stale);
    }
}

// --- 6. staleness and TTL --------------------------------------------------
void TestPeerExpiry() {
    Config cfg;
    cfg.peerStaleMs = 1000;
    cfg.peerTtlMs = 2000;
    UiModel m(cfg);

    UiEvent seen;
    seen.kind = UiEvent::Kind::PeerSeen;
    seen.key = "10.0.0.1";
    seen.name = "a";
    seen.atMs = 0;
    m.Post(seen);
    m.Pump(1500);
    auto s = m.Take();
    assert(s.peers.size() == 1 && s.peers.front().stale);

    m.Pump(2500);
    assert(m.Take().peers.empty());  // TTL evicts

    // tui's configuration must never lose a peer.
    Config keep;
    keep.peerStaleMs = 0;
    keep.peerTtlMs = 0;
    UiModel m2(keep);
    UiEvent seen2 = seen;
    m2.Post(seen2);
    m2.Pump(10'000'000);
    auto s2 = m2.Take();
    assert(s2.peers.size() == 1 && !s2.peers.front().stale);
}

// --- 7. rate / ETA matches SpeedTracker.kt --------------------------------
void TestSpeedAndEta() {
    ui::SpeedTracker st;
    auto r0 = st.Tick(1, 0, 1000, 0);
    assert(r0.bytesPerSec == 0 && r0.etaSec == -1);      // first sample

    auto r1 = st.Tick(1, 1000, 1000, 1000);
    assert(r1.bytesPerSec == 1000);
    assert(r1.etaSec == 0);

    // Window is prev->last, so 3000 B over 2000 ms = 1500 B/s.
    auto r2 = st.Tick(1, 3000, 6000, 2000);
    assert(r2.bytesPerSec == 1500);
    assert(r2.etaSec == 2);

    // A gap longer than 1500 ms restarts the window from `last`.
    ui::SpeedTracker st2;
    st2.Tick(2, 0, 10000, 0);
    st2.Tick(2, 1000, 10000, 1000);
    auto r3 = st2.Tick(2, 3000, 10000, 3000);
    assert(r3.bytesPerSec == 1000);   // (3000-1000) / (3000-1000)

    // Identical timestamps must not divide by zero.
    ui::SpeedTracker st3;
    st3.Tick(3, 100, 1000, 500);
    auto r4 = st3.Tick(3, 200, 1000, 500);
    assert(r4.bytesPerSec >= 0);

    // total <= 0 leaves ETA unknown; a done regression floors the delta.
    ui::SpeedTracker st4;
    st4.Tick(4, 500, 0, 0);
    auto r5 = st4.Tick(4, 600, 0, 1000);
    assert(r5.etaSec == -1);
    st4.Tick(4, 900, 1000, 2000);
    auto r6 = st4.Tick(4, 100, 1000, 3000);
    assert(r6.bytesPerSec == 0);
}

// --- 8. terminal events release sampler state -----------------------------
void TestSpeedCleanupOnTerminal() {
    ui::SpeedTracker st;
    st.Tick(7, 100, 1000, 0);
    st.Tick(7, 200, 1000, 100);
    assert(st.TrackedCount() == 2);  // one task: a {prev} and a {last} sample
    st.Forget(7);
    assert(st.TrackedCount() == 0);
}

// --- 9. row caps keep live work -------------------------------------------
void TestCapsKeepRunningRows() {
    Config cfg;
    cfg.maxTaskRows = 10;
    cfg.maxLogRows = 5;
    UiModel m(cfg);

    TaskId live = 0;
    for (int i = 0; i < 200; ++i) {
        TaskId id = m.EnqueueSend(MakeSeed("f" + std::to_string(i) + ".bin"), 0);
        m.Post(Ev(UiEvent::Kind::TaskStarted, id));
        if (i == 199) {
            live = id;  // newest stays Running
        } else {
            m.Post(Ev(UiEvent::Kind::TaskCompleted, id));
        }
    }
    for (int i = 0; i < 300; ++i) {
        UiEvent info;
        info.kind = UiEvent::Kind::Info;
        info.detail = "line " + std::to_string(i);
        m.Post(info);
    }
    m.Pump(1000);

    auto s = m.Take();
    assert(s.sends.size() == 10);
    assert(s.logs.size() == 5);
    assert(FindSend(s, live) != nullptr);   // the Running row survived pruning
    assert(FindSend(s, live)->state == TaskState::Running);
}

// --- 10. concurrent producers ---------------------------------------------
void TestConcurrentProducers() {
    UiModel m;
    std::vector<TaskId> ids;
    for (int i = 0; i < 64; ++i) ids.push_back(m.EnqueueSend(MakeSeed("c" + std::to_string(i)), 0));

    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t) {
        workers.emplace_back([&] {
            while (!start.load()) std::this_thread::yield();
            for (int i = 0; i < 1000; ++i) {
                TaskId id = ids[static_cast<size_t>(i) % ids.size()];
                m.PostProgress(id, i, 1000);
                if (i % 500 == 0) {
                    UiEvent info;
                    info.kind = UiEvent::Kind::Info;
                    info.detail = "t" + std::to_string(i);
                    m.Post(info);
                }
            }
        });
    }
    start.store(true);
    for (auto& w : workers) w.join();

    uint64_t prevRev = 0;
    for (int i = 0; i < 50; ++i) {
        m.Pump(1000 + i);
        const uint64_t rev = m.revision();
        assert(rev >= prevRev);   // monotone under concurrent producers
        prevRev = rev;
    }
    auto s = m.Take();
    assert(s.sends.size() == ids.size());
    for (const auto& r : s.sends) assert(r.state != TaskState::Completed);
}

// --- 11. listener state mirror --------------------------------------------
void TestListenerMirror() {
    UiModel m;
    UiEvent started;
    started.kind = UiEvent::Kind::TcpListenStarted;
    started.port = 8988;
    m.Post(started);
    m.Post(started);   // double start is idempotent
    m.Pump(1000);
    auto s = m.Take();
    assert(s.tcpListening && s.tcpPort == 8988);
    assert(s.logs.size() == 1);

    UiEvent failed;
    failed.kind = UiEvent::Kind::TcpListenFailed;
    failed.detail = "address in use";
    m.Post(failed);
    m.Pump(2000);
    auto s2 = m.Take();
    assert(!s2.tcpListening && s2.tcpPort == 0);
    assert(s2.logs.back().text.find("address in use") != std::string::npos);

    UiEvent btOn;
    btOn.kind = UiEvent::Kind::BtListenStarted;
    m.Post(btOn);
    UiEvent beacon;
    beacon.kind = UiEvent::Kind::BeaconStarted;
    m.Post(beacon);
    m.Pump(3000);
    auto s3 = m.Take();
    assert(s3.btListening && s3.beaconRunning && !s3.tcpListening);
}

// --- 12. hash mismatch keeps the file, marks it unverified -----------------
// TcpTransport leaves savedPath populated when the trailer disagrees; the old
// tui threw the filename away on any RecvFail.
void TestRecvHashMismatch() {
    UiModel m;
    UiEvent start;
    start.kind = UiEvent::Kind::RecvStarted;
    start.connId = 42;
    start.transport = Transport::Tcp;
    start.key = "10.0.0.9";
    start.name = "photo.jpg";
    start.size = 4096;
    m.Post(start);
    m.PostRecvProgress(Transport::Tcp, 42, 2048, 4096);
    m.Pump(1000);
    TaskId recvId = 0;
    {
        auto s = m.Take();
        assert(s.recvs.size() == 1);
        assert(s.recvs.front().state == TaskState::Running);
        assert(s.recvs.front().done == 2048 && s.recvs.front().pct == 50);
        recvId = s.recvs.front().id;
    }

    UiEvent done;
    done.kind = UiEvent::Kind::RecvFailed;
    done.connId = 42;
    done.transport = Transport::Tcp;
    done.name = "photo.jpg";
    done.path = "/tmp/photo.jpg";
    done.detail = "SHA-256 mismatch";
    done.sha256 = std::string(64, 'a');
    done.size = 4096;
    m.Post(done);
    m.PostRecvProgress(Transport::Tcp, 42, 4096, 4096);  // late, must be dropped
    m.Pump(2000);

    auto s = m.Take();
    assert(s.recvs.size() == 1);
    const ui::RecvRow* found = FindRecv(s, recvId);
    assert(found != nullptr && found->id == recvId);
    assert(found->state == TaskState::Failed);
    assert(found->fileOnDisk && found->savedPath == "/tmp/photo.jpg");
    assert(!found->integrityVerified);
    assert(found->done == 2048);  // unchanged by the post-terminal progress
}

// --- 13. recv rows are scoped by (transport, connId) -----------------------
// Two listeners both start their connId counters at 1, so the counter alone
// is not an identity.
void TestRecvConnIdScoping() {
    UiModel m;
    auto start = [](Transport t, int64_t size, const char* name) {
        UiEvent ev;
        ev.kind = UiEvent::Kind::RecvStarted;
        ev.transport = t;
        ev.connId = 1;
        ev.key = "peer";
        ev.name = name;
        ev.size = size;
        return ev;
    };
    m.Post(start(Transport::Tcp, 100, "tcp.bin"));
    m.Post(start(Transport::Bluetooth, 200, "bt.bin"));
    m.PostRecvProgress(Transport::Bluetooth, 1, 50, 200);
    m.Pump(100);

    auto s = m.Take();
    assert(s.recvs.size() == 2);
    TaskId tcpId = 0;
    TaskId btId = 0;
    for (const auto& r : s.recvs) {
        if (r.transport == Transport::Tcp) { tcpId = r.id; assert(r.size == 100); }
        else { btId = r.id; assert(r.size == 200 && r.done == 50 && r.pct == 25); }
    }
    assert(tcpId != 0 && btId != 0 && tcpId != btId);

    // Completing only the Bluetooth connection must leave the TCP row Running.
    UiEvent done;
    done.kind = UiEvent::Kind::RecvCompleted;
    done.transport = Transport::Bluetooth;
    done.connId = 1;
    done.size = 200;
    m.Post(done);
    m.Pump(200);
    s = m.Take();
    const ui::RecvRow* bt = FindRecv(s, btId);
    const ui::RecvRow* tcp = FindRecv(s, tcpId);
    assert(bt && bt->state == TaskState::Completed);
    assert(tcp && tcp->state == TaskState::Running);
}

// --- 14. snapshots are isolated copies ------------------------------------
void TestSnapshotIsolation() {
    UiModel m;
    TaskId id = m.EnqueueSend(MakeSeed(), 0);
    m.Pump(100);
    ui::Snapshot frozen = m.Take();
    const uint64_t frozenRev = frozen.revision;

    m.Post(Ev(UiEvent::Kind::TaskStarted, id));
    m.PostProgress(id, 99, 100);
    m.Pump(200);

    assert(frozen.sends.front().state == TaskState::Queued);
    assert(frozen.sends.front().done == 0);
    assert(frozen.revision == frozenRev);
    assert(m.Take().sends.front().state == TaskState::Running);
}

// --- 15. cancellation ------------------------------------------------------
void TestCancellation() {
    UiModel m;
    TaskId id = m.EnqueueSend(MakeSeed(), 0);
    m.Post(Ev(UiEvent::Kind::TaskStarted, id));
    m.Pump(1000);

    auto token = m.CancelToken(id);
    assert(token && !token->load());
    assert(m.TryCancel(id));
    assert(token->load());                 // worker observes this between chunks

    auto s = m.Take();
    assert(s.sends.front().cancelRequested);
    assert(s.sends.front().state == TaskState::Running);  // still one transition pending

    UiEvent canceled = Ev(UiEvent::Kind::TaskCanceled, id);
    canceled.detail = "canceled";
    m.Post(canceled);
    m.Pump(2000);
    assert(m.Take().sends.front().state == TaskState::Canceled);

    assert(!m.TryCancel(id));        // already terminal
    assert(!m.TryCancel(123456));    // unknown
}

// --- 16. real transport driven entirely through the model ------------------
void TestEndToEndThroughModel() {
    std::string srcDir = platform::TempDir();
    std::string dstDir = platform::TempDir();
    constexpr int64_t kSize = 1024 * 1024;
    {
        std::ofstream out(platform::JoinPath(srcDir, "e2e.bin"), std::ios::binary);
        std::string blob(static_cast<size_t>(kSize), 'x');
        out.write(blob.data(), static_cast<std::streamsize>(blob.size()));
    }

    auto model = std::make_shared<UiModel>();
    std::atomic<bool> terminal{false};
    std::atomic<int> recvProgress{0};

    tcp::TcpListener listener;
    bool ok = listener.Start(
        0, dstDir,
        [model, &terminal](const tcp::ReceivedFile& rf) {
            // Mirrors the GUI's hard rule: capture the model by value only.
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
            model->Post(ev);
            terminal.store(true);
        },
        [&recvProgress, model](const tcp::RecvUpdate& u) {
            if (u.stage == tcp::RecvUpdate::Stage::Started) {
                UiEvent ev;
                ev.kind = UiEvent::Kind::RecvStarted;
                ev.transport = Transport::Tcp;
                ev.connId = u.connId;
                ev.key = u.peerHost;
                ev.name = u.fileName;
                ev.size = u.total;
                model->Post(ev);
            } else {
                ++recvProgress;
                model->PostRecvProgress(Transport::Tcp, u.connId, u.done, u.total);
            }
        });
    assert(ok);

    std::atomic<bool> cancelRequest{false};
    std::thread sender([&] {
        model->Post(Ev(UiEvent::Kind::TaskStarted, 0));  // unknown id: ignored
        auto res = tcp::SendFile("127.0.0.1", listener.port(),
                                 platform::JoinPath(srcDir, "e2e.bin"), "e2e.bin",
                                 [&](int64_t done, int64_t total) {
                                     model->PostProgress(0, done, total);
                                     // Pace the sender so the transfer stays in
                                     // flight long enough to be observed.
                                     std::this_thread::sleep_for(
                                         std::chrono::milliseconds(2));
                                 },
                                 true, &cancelRequest);
        assert(res.ok);
    });

    // Pump while both sides are in flight: this is what exercises live receive
    // progress, which did not exist before (StreamToFile got nullptr).
    int sawRunningRecv = 0;
    for (int i = 0; i < 400 && !terminal.load(); ++i) {
        model->Pump(static_cast<int64_t>(i) * 10);
        for (const auto& r : model->Take().recvs) {
            if (r.state == TaskState::Running && r.done > 0) ++sawRunningRecv;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    sender.join();
    for (int i = 0; i < 40 && !terminal.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    model->Pump(100000);
    listener.Stop();

    assert(recvProgress.load() > 0);  // the listener emits progress
    assert(sawRunningRecv > 0);       // and the model folds it while Running

    auto s = model->Take();
    assert(s.recvs.size() == 1);
    const ui::RecvRow& r = s.recvs.front();
    assert(r.state == TaskState::Completed);
    assert(r.integrityVerified);
    assert(r.name == "e2e.bin");
    assert(r.fileOnDisk && r.size == kSize);

    platform::RemoveDir(srcDir);
    platform::RemoveDir(dstDir);
}

} // namespace

int main() {
    platform::InitNetworking();
    TestTaskIdentity();
    TestTerminalGuard();
    TestProgressCannotStarve();
    TestUnknownIdsIgnored();
    TestPeerIdentity();
    TestPeerExpiry();
    TestSpeedAndEta();
    TestSpeedCleanupOnTerminal();
    TestCapsKeepRunningRows();
    TestConcurrentProducers();
    TestListenerMirror();
    TestRecvHashMismatch();
    TestRecvConnIdScoping();
    TestSnapshotIsolation();
    TestCancellation();
    TestEndToEndThroughModel();
    platform::ShutdownNetworking();
    std::printf("test_ui_model: ALL OK (16 groups)\n");
    return 0;
}
