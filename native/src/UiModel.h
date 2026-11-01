// UiModel.h — platform-independent state machine shared by the interactive
// front-ends (`wfd_tui` on Linux, `wfd_gui` on Windows).
//
// It owns exactly the logic that is worth testing without a display:
//
//   * two event channels — critical events are queued and never dropped,
//     progress is coalesced per-task-id (latest wins). This mirrors the
//     Android `transfer/TransferBus.kt`, where a Progress flood used to push
//     Completed/Failed out of a shared bounded buffer and wedge the UI.
//   * terminal-state protection — once a task reaches Completed / Failed /
//     Canceled no later event rewrites it, and Completed clamps done to total
//     (`app/src/test/.../TransferAdapterStateTest.kt` pins the same semantics).
//   * rows are keyed by TaskId, never by filename. The old tui matched on
//     filename, so sending the same file twice made both rows share one
//     progress stream.
//   * rate / ETA sampling, ported from `transfer/SpeedTracker.kt`.
//
// Threading contract:
//   * `Post` / `PostProgress` may be called from any thread, any count. They
//     take the internal lock briefly, record, and return — no caller-supplied
//     callback ever runs while the lock is held.
//   * `Pump`, `Take`, `EnqueueSend`, `TryCancel` are for the single UI thread.
//   * Renderers never hold a pointer into model state: call `Pump(nowMs)` then
//     `Take()` and paint from the returned copy.
//
// `Pump` takes the clock as a parameter rather than reading it internally —
// that is what makes rate/ETA assertions deterministic in test_ui_model.
//
// Selection / focus deliberately live outside this class: ncurses and the
// Win32 ListView each own their own cursor.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace wfd {
namespace ui {

enum class Transport : uint8_t { Tcp = 0, Bluetooth = 1 };

enum class TaskState : uint8_t {
    Queued,
    Running,
    Completed,
    Failed,
    Canceled,
};

inline bool IsTerminal(TaskState s) {
    return s == TaskState::Completed || s == TaskState::Failed ||
           s == TaskState::Canceled;
}

enum class PeerKind : uint8_t {
    Lan,               // UDP beacon (discovery::Listen)
    BluetoothPaired,   // bt::EnumeratePairedDevices
    BluetoothInquiry,  // bt::DiscoverDevices
};

// 0 means "no task" and is never handed out. Ids are not reused.
using TaskId = uint64_t;

// Event timestamps are caller-supplied so tests can drive the clock.
// kNoTimestamp means "use the Pump() clock"; 0 is a legitimate clock value.
constexpr int64_t kNoTimestamp = -1;

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

struct UiEvent {
    enum class Kind : uint8_t {
        TaskStarted,
        TaskCompleted,
        TaskFailed,
        TaskCanceled,
        RecvStarted,     // a peer began sending us a file
        RecvCompleted,
        RecvFailed,
        PeerSeen,
        PeerGone,
        TcpListenStarted,
        TcpListenStopped,
        TcpListenFailed,
        BtListenStarted,
        BtListenStopped,
        BtListenFailed,
        BeaconStarted,
        BeaconStopped,
        Info,
    } kind = Kind::Info;

    TaskId    id = 0;
    Transport transport = Transport::Tcp;
    PeerKind  peerKind = PeerKind::Lan;

    // Correlation handle for inbound transfers. A listener callback cannot
    // know a model-assigned TaskId, so the transport stamps every connection
    // with a per-listener counter and echoes it in ReceivedFile. The UI thread
    // resolves (transport, connId) -> RecvRow inside Pump.
    uint64_t  connId = 0;

    std::string key;     // peer identity: IPv4 text or BT MAC — byte-exact, never localized
    std::string name;    // UTF-8 wire filename, or peer hostname
    std::string path;    // send: source path / recv: saved path
    std::string detail;  // error text
    std::string sha256;  // digest hex, mirroring SendResult.senderHash / ReceivedFile.receivedHash
    int64_t     size = 0;
    bool        integrityVerified = false;
    int         port = 0;
    int64_t     atMs = kNoTimestamp;  // < 0 means "stamp with Pump's nowMs"; 0 is a valid clock value
};

// ---------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------

struct PeerRow {
    std::string key;
    std::string name;
    PeerKind    kind = PeerKind::Lan;
    Transport   transport = Transport::Tcp;
    int64_t     lastSeenMs = 0;
    bool        stale = false;
};

struct TaskRow {
    TaskId    id = 0;
    Transport transport = Transport::Tcp;
    std::string name;        // filename shown to the user
    std::string sourcePath;  // local file being sent
    std::string peer;        // key of the destination peer
    int64_t total = 0;
    int64_t done = 0;
    int     pct = 0;
    int64_t bytesPerSec = 0;
    int64_t etaSec = -1;     // -1 = unknown
    TaskState state = TaskState::Queued;
    bool cancelRequested = false;
    std::string detail;      // error text on failure
    std::string sha256;      // digest we sent, when a trailer was emitted
    bool integrityVerified = false;
    int64_t startedMs = 0;
    int64_t endedMs = 0;
};

struct RecvRow {
    TaskId    id = 0;
    Transport transport = Transport::Tcp;
    std::string peer;
    std::string name;
    std::string savedPath;
    std::string error;
    std::string sha256;
    int64_t size = 0;
    int64_t done = 0;
    int     pct = 0;
    int64_t bytesPerSec = 0;
    int64_t etaSec = -1;
    TaskState state = TaskState::Running;
    // True when the bytes are on disk but the trailer did not match: the file
    // exists, it is just unverified. TcpTransport.cpp keeps savedPath in that
    // case, so the UI must not throw the filename away.
    bool integrityVerified = false;
    bool fileOnDisk = false;
    int64_t startedMs = 0;
    int64_t endedMs = 0;
};

struct LogRow {
    int64_t     atMs = 0;
    std::string text;
};

// Value copy handed to the painter. Safe to keep across a whole frame.
struct Snapshot {
    std::vector<PeerRow> peers;
    std::vector<TaskRow> sends;
    std::vector<RecvRow> recvs;
    std::vector<LogRow>  logs;
    bool tcpListening = false;
    bool btListening = false;
    bool beaconRunning = false;
    int  tcpPort = 0;
    std::string receiveDir;
    uint64_t revision = 0;
};

// ---------------------------------------------------------------------------
// Speed / ETA sampler — port of SpeedTracker.kt
// ---------------------------------------------------------------------------

class SpeedTracker {
public:
    struct Sample { int64_t tMs = 0; int64_t bytes = 0; };
    struct Out    { int64_t bytesPerSec = 0; int64_t etaSec = -1; };  // -1 = unknown

    Out Tick(TaskId id, int64_t done, int64_t total, int64_t nowMs);
    void Forget(TaskId id);
    void Reset();
    size_t TrackedCount() const;

private:
    std::map<TaskId, Sample> last_, prev_;
};

// ---------------------------------------------------------------------------
// UiModel
// ---------------------------------------------------------------------------

struct Config {
    size_t  maxLogRows = 50;
    size_t  maxTaskRows = 200;
    size_t  maxRecvRows = 200;
    // Peers seen longer than peerStaleMs ago are flagged `stale` (kept visible).
    // peerTtlMs > 0 additionally evicts them. 0 disables each behavior — the
    // tui sets both to 0 so peers never vanish, which is its current behavior.
    int64_t peerStaleMs = 8000;
    int64_t peerTtlMs = 0;
};

class UiModel {
public:
    explicit UiModel(Config cfg = {});

    // ---- producers: any thread -------------------------------------------

    // Critical event. Never dropped, never coalesced.
    void Post(UiEvent ev);

    // Progress is latest-wins per id: a flood cannot starve a terminal event,
    // and an id that already reached a terminal state is ignored outright.
    void PostProgress(TaskId id, int64_t done, int64_t total);

    // Same coalescing, keyed by the transport's connection handle instead of a
    // model TaskId (inbound rows have no id the producer could know).
    void PostRecvProgress(Transport transport, uint64_t connId,
                          int64_t done, int64_t total);

    // ---- UI thread -------------------------------------------------------

    // Inserts a Queued row and returns its id, or 0 when `seed` is not
    // sendable (empty peer / empty path). On rejection an Info row is posted
    // so both front-ends surface the same reason.
    TaskId EnqueueSend(const TaskRow& seed, int64_t nowMs);

    // Folds queued events + coalesced progress into the rows, refreshes
    // rates/ETA, expires peers, trims to the configured caps.
    void Pump(int64_t nowMs);

    Snapshot Take() const;

    // Signals the worker's cancel token and marks the row as "canceling"; the
    // worker itself posts the terminal TaskCanceled, so a task has exactly one
    // transition into a terminal state. Returns false for unknown / already
    // terminal ids.
    bool TryCancel(TaskId id);

    // Token handed to the sender thread; `SendFile` polls it between chunks.
    std::shared_ptr<std::atomic<bool>> CancelToken(TaskId id) const;

    uint64_t revision() const;

    // ---- configuration mirrors (UI thread) --------------------------------
    void SetReceiveDir(std::string dir);
    const std::string& receiveDir() const;

private:
    struct ProgressSample { int64_t done = 0; int64_t total = 0; };
    using ConnKey = std::pair<Transport, uint64_t>;

    void ApplyCritical(const UiEvent& ev, int64_t nowMs);
    void FoldProgress(int64_t nowMs);
    void RefreshPeers(int64_t nowMs);
    void Trim();
    void AppendLog(std::string text, int64_t nowMs);
    TaskRow* FindTask(TaskId id);
    RecvRow* FindRecv(TaskId id);
    TaskId EnsureRecvRow(Transport transport, uint64_t connId);

    Config cfg_;
    mutable std::mutex m_;

    std::vector<PeerRow>  peers_;
    std::vector<TaskRow>  sends_;
    std::vector<RecvRow>  recvs_;
    std::vector<LogRow>   logs_;

    std::deque<UiEvent>              pendingCritical_;
    std::map<TaskId, ProgressSample> pendingProgress_;
    std::map<ConnKey, ProgressSample> pendingRecvProgress_;
    std::map<ConnKey, TaskId>        recvByConn_;
    std::map<TaskId, std::shared_ptr<std::atomic<bool>>> tokens_;

    SpeedTracker speed_;
    TaskId       nextId_ = 1;
    TaskId       nextRecvId_ = 1ull << 32;  // disjoint space from sends
    uint64_t     revision_ = 0;

    bool tcpListening_ = false;
    bool btListening_ = false;
    bool beaconRunning_ = false;
    int  tcpPort_ = 0;
    std::string receiveDir_;
};

} // namespace ui
} // namespace wfd
