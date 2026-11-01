// UiModel.cpp — see UiModel.h for the contract.
#include "UiModel.h"

#include <algorithm>

namespace wfd {
namespace ui {
namespace {

constexpr int64_t kSpeedGapResetMs = 1500;  // SpeedTracker.kt's window switch

int Percent(int64_t done, int64_t total) {
    if (total <= 0) return 0;
    int64_t pct = (done * 100) / total;
    if (pct < 0) return 0;
    if (pct > 100) return 100;
    return static_cast<int>(pct);
}

} // namespace

// ---------------------------------------------------------------------------
// SpeedTracker
// ---------------------------------------------------------------------------

SpeedTracker::Out SpeedTracker::Tick(TaskId id, int64_t done, int64_t total,
                                     int64_t nowMs) {
    auto itPrev = prev_.find(id);
    auto itLast = last_.find(id);
    const bool hasPrev = itPrev != prev_.end();
    const bool hasLast = itLast != last_.end();

    // Which baseline to measure from, exactly as SpeedTracker.kt's `when`:
    //   no samples        -> none (first tick reports 0 B/s)
    //   only one of them  -> that one
    //   last is stale     -> restart the window from `last`
    //   otherwise         -> `prev`, so the rate spans prev -> last
    Sample effectivePrev{};
    bool havePrev = false;
    if (hasPrev && hasLast) {
        if (nowMs - itLast->second.tMs > kSpeedGapResetMs) {
            effectivePrev = itLast->second;
            havePrev = true;
        } else {
            effectivePrev = itPrev->second;
            havePrev = true;
        }
    } else if (hasPrev) {
        effectivePrev = itPrev->second;
        havePrev = true;
    } else if (hasLast) {
        effectivePrev = itLast->second;
        havePrev = true;
    }
    if (!havePrev) effectivePrev = Sample{nowMs, done};

    const int64_t dtMs = std::max<int64_t>(nowMs - effectivePrev.tMs, 1);
    const int64_t db = std::max<int64_t>(done - effectivePrev.bytes, 0);
    const int64_t bytesPerSec = (db * 1000) / dtMs;

    prev_[id] = effectivePrev;
    last_[id] = Sample{nowMs, done};

    Out out;
    out.bytesPerSec = bytesPerSec;
    if (bytesPerSec > 0 && total > 0) {
        out.etaSec = std::max<int64_t>((total - done) / bytesPerSec, 0);
    }
    return out;
}

void SpeedTracker::Forget(TaskId id) {
    last_.erase(id);
    prev_.erase(id);
}

void SpeedTracker::Reset() {
    last_.clear();
    prev_.clear();
}

size_t SpeedTracker::TrackedCount() const {
    return last_.size() + prev_.size();
}

// ---------------------------------------------------------------------------
// UiModel — producers
// ---------------------------------------------------------------------------

UiModel::UiModel(Config cfg) : cfg_(cfg) {}

void UiModel::Post(UiEvent ev) {
    std::lock_guard<std::mutex> lk(m_);
    pendingCritical_.push_back(std::move(ev));
}

void UiModel::PostProgress(TaskId id, int64_t done, int64_t total) {
    std::lock_guard<std::mutex> lk(m_);
    pendingProgress_[id] = ProgressSample{done, total};
}

void UiModel::PostRecvProgress(Transport transport, uint64_t connId,
                               int64_t done, int64_t total) {
    std::lock_guard<std::mutex> lk(m_);
    pendingRecvProgress_[ConnKey{transport, connId}] = ProgressSample{done, total};
}

// ---------------------------------------------------------------------------
// UiModel — UI thread
// ---------------------------------------------------------------------------

TaskId UiModel::EnqueueSend(const TaskRow& seed, int64_t nowMs) {
    std::lock_guard<std::mutex> lk(m_);
    if (seed.peer.empty()) {
        AppendLog("send rejected: no peer selected", nowMs);
        return 0;
    }
    if (seed.sourcePath.empty() || seed.name.empty()) {
        AppendLog("send rejected: unreadable file", nowMs);
        return 0;
    }

    TaskRow row = seed;
    row.id = nextId_++;
    row.state = TaskState::Queued;
    row.done = 0;
    row.pct = 0;
    row.bytesPerSec = 0;
    row.etaSec = -1;
    row.cancelRequested = false;
    row.detail.clear();
    row.sha256.clear();
    row.startedMs = 0;
    row.endedMs = 0;
    if (row.total <= 0) row.total = 0;
    sends_.push_back(std::move(row));

    tokens_[sends_.back().id] = std::make_shared<std::atomic<bool>>(false);
    ++revision_;
    return sends_.back().id;
}

void UiModel::Pump(int64_t nowMs) {
    std::lock_guard<std::mutex> lk(m_);

    // Critical events first: a row must be terminal before progress folds in,
    // otherwise a queued Progress could transiently resurrect it.
    std::deque<UiEvent> events;
    events.swap(pendingCritical_);
    for (const auto& ev : events) ApplyCritical(ev, nowMs);

    FoldProgress(nowMs);
    RefreshPeers(nowMs);
    Trim();
}

Snapshot UiModel::Take() const {
    std::lock_guard<std::mutex> lk(m_);
    Snapshot s;
    s.peers = peers_;
    s.sends = sends_;
    s.recvs = recvs_;
    s.logs = logs_;
    s.tcpListening = tcpListening_;
    s.btListening = btListening_;
    s.beaconRunning = beaconRunning_;
    s.tcpPort = tcpPort_;
    s.receiveDir = receiveDir_;
    s.revision = revision_;
    return s;
}

bool UiModel::TryCancel(TaskId id) {
    std::lock_guard<std::mutex> lk(m_);
    TaskRow* row = FindTask(id);
    if (row == nullptr || IsTerminal(row->state)) return false;
    auto it = tokens_.find(id);
    if (it == tokens_.end()) return false;
    it->second->store(true, std::memory_order_relaxed);
    // The worker posts the terminal TaskCanceled, so the row only shows
    // "canceling" until then — one transition into a terminal state.
    row->cancelRequested = true;
    ++revision_;
    return true;
}

std::shared_ptr<std::atomic<bool>> UiModel::CancelToken(TaskId id) const {
    std::lock_guard<std::mutex> lk(m_);
    auto it = tokens_.find(id);
    return it == tokens_.end() ? nullptr : it->second;
}

uint64_t UiModel::revision() const {
    std::lock_guard<std::mutex> lk(m_);
    return revision_;
}

void UiModel::SetReceiveDir(std::string dir) {
    std::lock_guard<std::mutex> lk(m_);
    receiveDir_ = std::move(dir);
    ++revision_;
}

const std::string& UiModel::receiveDir() const {
    // Only read from the UI thread between Pump calls; the value never changes
    // from a producer thread, so a lock-free read is safe here.
    return receiveDir_;
}

// ---------------------------------------------------------------------------
// Folding
// ---------------------------------------------------------------------------

TaskRow* UiModel::FindTask(TaskId id) {
    for (auto& r : sends_) {
        if (r.id == id) return &r;
    }
    return nullptr;
}

RecvRow* UiModel::FindRecv(TaskId id) {
    for (auto& r : recvs_) {
        if (r.id == id) return &r;
    }
    return nullptr;
}

TaskId UiModel::EnsureRecvRow(Transport transport, uint64_t connId) {
    auto it = recvByConn_.find(ConnKey{transport, connId});
    if (it != recvByConn_.end()) return it->second;
    RecvRow row;
    row.id = nextRecvId_++;
    row.transport = transport;
    recvs_.push_back(std::move(row));
    recvByConn_[ConnKey{transport, connId}] = recvs_.back().id;
    return recvs_.back().id;
}

void UiModel::ApplyCritical(const UiEvent& ev, int64_t nowMs) {
    switch (ev.kind) {
    case UiEvent::Kind::TaskStarted: {
        TaskRow* row = FindTask(ev.id);
        if (row == nullptr || IsTerminal(row->state)) return;
        row->state = TaskState::Running;
        if (ev.size > 0) row->total = ev.size;
        if (row->startedMs == 0) row->startedMs = ev.atMs < 0 ? nowMs : ev.atMs;
        ++revision_;
        return;
    }
    case UiEvent::Kind::TaskCompleted: {
        TaskRow* row = FindTask(ev.id);
        if (row == nullptr || IsTerminal(row->state)) return;
        pendingProgress_.erase(ev.id);
        row->state = TaskState::Completed;
        if (row->total > 0) row->done = row->total;  // clamp
        row->pct = 100;
        row->endedMs = ev.atMs < 0 ? nowMs : ev.atMs;
        if (!ev.sha256.empty()) row->sha256 = ev.sha256;
        row->bytesPerSec = 0;
        row->etaSec = 0;
        speed_.Forget(ev.id);
        tokens_.erase(ev.id);
        ++revision_;
        return;
    }
    case UiEvent::Kind::TaskFailed:
    case UiEvent::Kind::TaskCanceled: {
        TaskRow* row = FindTask(ev.id);
        if (row == nullptr || IsTerminal(row->state)) return;
        pendingProgress_.erase(ev.id);
        row->state = ev.kind == UiEvent::Kind::TaskFailed ? TaskState::Failed
                                                          : TaskState::Canceled;
        row->detail = ev.detail;
        row->endedMs = ev.atMs < 0 ? nowMs : ev.atMs;
        row->bytesPerSec = 0;
        row->etaSec = -1;
        speed_.Forget(ev.id);
        tokens_.erase(ev.id);
        ++revision_;
        return;
    }
    case UiEvent::Kind::RecvStarted: {
        TaskId id = EnsureRecvRow(ev.transport, ev.connId);
        RecvRow* row = FindRecv(id);
        if (row == nullptr || IsTerminal(row->state)) return;
        row->peer = ev.key;
        row->name = ev.name;
        row->size = ev.size;
        row->state = TaskState::Running;
        row->startedMs = ev.atMs < 0 ? nowMs : ev.atMs;
        ++revision_;
        return;
    }
    case UiEvent::Kind::RecvCompleted:
    case UiEvent::Kind::RecvFailed: {
        TaskId id = EnsureRecvRow(ev.transport, ev.connId);
        RecvRow* row = FindRecv(id);
        if (row == nullptr || IsTerminal(row->state)) return;
        pendingRecvProgress_.erase(ConnKey{ev.transport, ev.connId});
        if (row->name.empty()) row->name = ev.name;
        if (row->peer.empty()) row->peer = ev.key;
        if (ev.size > 0) row->size = ev.size;
        row->savedPath = ev.path;
        row->error = ev.detail;
        row->sha256 = ev.sha256;
        row->integrityVerified = ev.integrityVerified;
        row->fileOnDisk = !ev.path.empty();
        const bool ok = ev.kind == UiEvent::Kind::RecvCompleted && ev.detail.empty();
        row->state = ok ? TaskState::Completed : TaskState::Failed;
        if (ok) {
            row->done = row->size;
            row->pct = 100;
        }
        row->endedMs = ev.atMs < 0 ? nowMs : ev.atMs;
        speed_.Forget(id);
        ++revision_;
        return;
    }
    case UiEvent::Kind::PeerSeen: {
        const int64_t stamp = ev.atMs < 0 ? nowMs : ev.atMs;
        for (auto& p : peers_) {
            if (p.transport == ev.transport && p.key == ev.key) {
                const bool changed = p.name != ev.name || p.stale ||
                                     p.lastSeenMs != stamp;
                p.name = ev.name;
                p.kind = ev.peerKind;
                p.lastSeenMs = stamp;
                p.stale = false;
                if (changed) ++revision_;
                return;
            }
        }
        PeerRow p;
        p.key = ev.key;
        p.name = ev.name;
        p.kind = ev.peerKind;
        p.transport = ev.transport;
        p.lastSeenMs = stamp;
        peers_.push_back(std::move(p));
        ++revision_;
        return;
    }
    case UiEvent::Kind::PeerGone: {
        for (auto& p : peers_) {
            if (p.transport == ev.transport && p.key == ev.key && !p.stale) {
                p.stale = true;
                ++revision_;
                return;
            }
        }
        return;
    }
    case UiEvent::Kind::TcpListenStarted:
        if (tcpListening_ && tcpPort_ == ev.port) return;  // idempotent
        tcpListening_ = true;
        tcpPort_ = ev.port;
        AppendLog("TCP listening on port " + std::to_string(ev.port), nowMs);
        ++revision_;
        return;
    case UiEvent::Kind::TcpListenStopped:
        if (!tcpListening_) return;
        tcpListening_ = false;
        tcpPort_ = 0;
        AppendLog("TCP listener stopped", nowMs);
        ++revision_;
        return;
    case UiEvent::Kind::TcpListenFailed:
        tcpListening_ = false;
        tcpPort_ = 0;
        AppendLog("TCP listen failed: " + ev.detail, nowMs);
        ++revision_;
        return;
    case UiEvent::Kind::BtListenStarted:
        if (btListening_) return;
        btListening_ = true;
        AppendLog("Bluetooth RFCOMM listening", nowMs);
        ++revision_;
        return;
    case UiEvent::Kind::BtListenStopped:
        if (!btListening_) return;
        btListening_ = false;
        AppendLog("Bluetooth listener stopped", nowMs);
        ++revision_;
        return;
    case UiEvent::Kind::BtListenFailed:
        btListening_ = false;
        AppendLog("Bluetooth listen failed: " + ev.detail, nowMs);
        ++revision_;
        return;
    case UiEvent::Kind::BeaconStarted:
        if (beaconRunning_) return;
        beaconRunning_ = true;
        ++revision_;
        return;
    case UiEvent::Kind::BeaconStopped:
        if (!beaconRunning_) return;
        beaconRunning_ = false;
        ++revision_;
        return;
    case UiEvent::Kind::Info:
        AppendLog(ev.detail, nowMs);
        ++revision_;
        return;
    }
}

void UiModel::FoldProgress(int64_t nowMs) {
    if (!pendingProgress_.empty()) {
        std::map<TaskId, ProgressSample> drained;
        drained.swap(pendingProgress_);
        for (const auto& kv : drained) {
            TaskRow* row = FindTask(kv.first);
            // Unknown id (already pruned) or terminal row: dropped, never
            // resurrected. This is what makes a Progress flood harmless.
            if (row == nullptr || IsTerminal(row->state)) continue;
            row->done = kv.second.done;
            if (kv.second.total > 0) row->total = kv.second.total;
            auto out = speed_.Tick(kv.first, row->done, row->total, nowMs);
            row->bytesPerSec = out.bytesPerSec;
            row->etaSec = out.etaSec;
            row->pct = Percent(row->done, row->total);
            if (row->state == TaskState::Queued) row->state = TaskState::Running;
            ++revision_;
        }
    }

    if (!pendingRecvProgress_.empty()) {
        std::map<ConnKey, ProgressSample> drained;
        drained.swap(pendingRecvProgress_);
        for (const auto& kv : drained) {
            auto it = recvByConn_.find(kv.first);
            if (it == recvByConn_.end()) continue;
            RecvRow* row = FindRecv(it->second);
            if (row == nullptr || IsTerminal(row->state)) continue;
            row->done = kv.second.done;
            if (kv.second.total > 0) row->size = kv.second.total;
            auto out = speed_.Tick(row->id, row->done, row->size, nowMs);
            row->bytesPerSec = out.bytesPerSec;
            row->etaSec = out.etaSec;
            row->pct = Percent(row->done, row->size);
            ++revision_;
        }
    }
}

void UiModel::RefreshPeers(int64_t nowMs) {
    if (cfg_.peerStaleMs <= 0 && cfg_.peerTtlMs <= 0) return;

    const size_t before = peers_.size();
    peers_.erase(std::remove_if(peers_.begin(), peers_.end(), [&](const PeerRow& p) {
        return cfg_.peerTtlMs > 0 && nowMs - p.lastSeenMs > cfg_.peerTtlMs;
    }), peers_.end());
    if (peers_.size() != before) ++revision_;

    for (auto& p : peers_) {
        // Staleness here is a timeout heuristic and can only *set* the flag:
        // an explicit PeerGone must not be cleared just because the last
        // beacon looked recent. Only a fresh PeerSeen revives a peer.
        if (cfg_.peerStaleMs <= 0 || p.stale) continue;
        if (nowMs - p.lastSeenMs > cfg_.peerStaleMs) {
            p.stale = true;
            ++revision_;
        }
    }
}

void UiModel::Trim() {
    auto prune = [](auto& rows, size_t max, auto isLive) {
        if (rows.size() <= max) return;
        size_t excess = rows.size() - max;
        // Scan from the oldest end and drop only finished rows, so a live
        // transfer can never be evicted by a burst of completions.
        for (auto it = rows.begin(); it != rows.end() && excess > 0;) {
            if (!isLive(*it)) { it = rows.erase(it); --excess; }
            else ++it;
        }
    };
    prune(sends_, cfg_.maxTaskRows,
          [](const TaskRow& r) { return !IsTerminal(r.state) || r.cancelRequested; });
    prune(recvs_, cfg_.maxRecvRows,
          [](const RecvRow& r) { return !IsTerminal(r.state); });

    if (logs_.size() > cfg_.maxLogRows) {
        logs_.erase(logs_.begin(),
                    logs_.begin() + static_cast<ptrdiff_t>(logs_.size() - cfg_.maxLogRows));
    }
}

void UiModel::AppendLog(std::string text, int64_t nowMs) {
    if (text.empty()) return;
    logs_.push_back(LogRow{nowMs, std::move(text)});
}

} // namespace ui
} // namespace wfd
