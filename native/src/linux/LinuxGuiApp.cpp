// LinuxGuiApp.cpp — Qt6 front-end. See LinuxGuiApp.h for the thread contract.
#include "LinuxGuiApp.h"

#include "I18nQt.h"
#include "Platform.h"
#include "TransferDelegate.h"
#include "Util.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QFileDialog>
#include <QHeaderView>
#include <QLocale>
#include <QMessageBox>
#include <QStyleOptionViewItem>
#include <QSplitter>
#include <QTableWidgetItem>
#include <QUrl>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWidget>
#include <QColor>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

namespace wfd {
namespace linuxgui {

namespace {

// Monotonic clock with a fixed epoch, so SpeedTracker sees small numbers
// (identical to tui.cpp:56-63).
const std::chrono::steady_clock::time_point g_epoch =
    std::chrono::steady_clock::now();

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - g_epoch).count();
}

QString Human(int64_t bytes) {
    return QString::fromStdString(util::HumanSize(bytes));
}

QString StateText(ui::TaskState s) {
    auto& i = I18nQt::Instance();
    switch (s) {
        case ui::TaskState::Queued:    return i.Get(Str::StateWait);
        case ui::TaskState::Running:   return i.Get(Str::StateRun);
        case ui::TaskState::Completed: return i.Get(Str::StateOk);
        case ui::TaskState::Failed:    return i.Get(Str::StateFail);
        case ui::TaskState::Canceled:  return i.Get(Str::StateCancel);
    }
    return QString();
}

int StateTint(ui::TaskState s) {
    switch (s) {
        case ui::TaskState::Completed: return 1;
        case ui::TaskState::Failed:    return 2;
        case ui::TaskState::Canceled:  return 3;
        default:                       return 0;
    }
}

QString PeerKindText(ui::PeerKind k) {
    auto& i = I18nQt::Instance();
    switch (k) {
        case ui::PeerKind::Lan:               return i.Get(Str::PeerLan);
        case ui::PeerKind::BluetoothPaired:   return i.Get(Str::PeerBtPaired);
        case ui::PeerKind::BluetoothInquiry:  return i.Get(Str::PeerBtFound);
    }
    return QString();
}

QString EtaText(int64_t etaSec) {
    if (etaSec < 0) return QStringLiteral("-");
    if (etaSec < 60) return QString::number(etaSec) + QLatin1String("s");
    return QString("%1m %2s").arg(etaSec / 60).arg(etaSec % 60, 2, 10, QChar('0'));
}

// Set a table item's text plus the role values the progress delegate reads.
void setCell(QTableWidget* table, int row, int col, const QString& text) {
    QTableWidgetItem* item = table->item(row, col);
    if (item == nullptr) {
        item = new QTableWidgetItem();
        table->setItem(row, col, item);
    }
    item->setText(text);
}

} // namespace

LinuxGuiApp::LinuxGuiApp(std::shared_ptr<ui::UiModel> model,
                         const std::string& receiveDir, int tcpPort,
                         QWidget* parent)
    : QMainWindow(parent), model_(std::move(model)),
      receiveDir_(receiveDir), tcpPort_(tcpPort) {
    buildUi();
    retranslate();

    model_->SetReceiveDir(receiveDir_);
    chkTcp_->setChecked(true);
    toggleTcp(true);
    runDiscovery();

    timer_ = new QTimer(this);
    timer_->setInterval(250);
    connect(timer_, &QTimer::timeout, this, &LinuxGuiApp::onTick);
    timer_->start();

    syncModel(true);
}

LinuxGuiApp::~LinuxGuiApp() {
    shutdown_->store(true);
    tcp_.Stop();
    bt_.Stop();
}

void LinuxGuiApp::buildUi() {
    // ---- Menu bar ---------------------------------------------------------
    langMenu_ = menuBar()->addMenu(QString());
    auto* group = new QActionGroup(this);
    group->setExclusive(true);
    actEn_ = langMenu_->addAction(QString());
    actZh_ = langMenu_->addAction(QString());
    actEn_->setCheckable(true);
    actZh_->setCheckable(true);
    group->addAction(actEn_);
    group->addAction(actZh_);
    connect(actEn_, &QAction::triggered, this, &LinuxGuiApp::setEnglish);
    connect(actZh_, &QAction::triggered, this, &LinuxGuiApp::setChinese);

    // ---- Toolbar buttons --------------------------------------------------
    auto* bar = new QHBoxLayout();
    btnFiles_  = new QPushButton();
    btnSend_   = new QPushButton();
    btnCancel_ = new QPushButton();
    btnDir_    = new QPushButton();
    chkTcp_    = new QCheckBox();
    chkBt_     = new QCheckBox();
    btnBtScan_ = new QPushButton();
    bar->addWidget(btnFiles_);
    bar->addWidget(btnSend_);
    bar->addWidget(btnCancel_);
    bar->addWidget(btnDir_);
    bar->addWidget(chkTcp_);
    bar->addWidget(chkBt_);
    bar->addWidget(btnBtScan_);
    bar->addStretch(1);

    connect(btnFiles_, &QPushButton::clicked, this, &LinuxGuiApp::pickAndQueue);
    connect(btnSend_, &QPushButton::clicked, this, &LinuxGuiApp::startSelected);
    connect(btnCancel_, &QPushButton::clicked, this, &LinuxGuiApp::cancelSelected);
    connect(btnDir_, &QPushButton::clicked, this, &LinuxGuiApp::chooseReceiveDir);
    connect(chkTcp_, &QCheckBox::toggled, this, &LinuxGuiApp::toggleTcp);
    connect(chkBt_, &QCheckBox::toggled, this, &LinuxGuiApp::toggleBt);
    connect(btnBtScan_, &QPushButton::clicked, this, &LinuxGuiApp::scanBluetooth);

    // ---- Tables -----------------------------------------------------------
    peers_ = new QTableWidget(0, 4);
    peers_->setHorizontalHeaderLabels({QString(), QString(), QString(), QString()});
    peers_->horizontalHeader()->setStretchLastSection(true);
    peers_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    peers_->setSelectionBehavior(QAbstractItemView::SelectRows);
    peers_->setSelectionMode(QAbstractItemView::SingleSelection);

    transfers_ = new QTableWidget(0, COL_TRANSFER_COUNT);
    transfers_->setHorizontalHeaderLabels(QStringList(COL_TRANSFER_COUNT, QString()));
    transfers_->horizontalHeader()->setStretchLastSection(true);
    transfers_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    transfers_->setSelectionBehavior(QAbstractItemView::SelectRows);
    transfers_->setSelectionMode(QAbstractItemView::SingleSelection);
    progressDelegate_ = new TransferDelegate(this);
    transfers_->setItemDelegateForColumn(COL_PROGRESS, progressDelegate_);

    log_ = new QListWidget();

    connect(peers_, &QTableWidget::cellDoubleClicked,
            this, &LinuxGuiApp::onPeerDoubleClicked);
    connect(transfers_, &QTableWidget::cellDoubleClicked,
            this, &LinuxGuiApp::onTransferDoubleClicked);

    // ---- Layout -----------------------------------------------------------
    auto* topRow = new QWidget();
    topRow->setLayout(bar);

    auto* splitter = new QSplitter(Qt::Vertical);
    auto* upper = new QWidget();
    auto* upperLayout = new QHBoxLayout(upper);
    upperLayout->addWidget(peers_, 1);
    upperLayout->addWidget(transfers_, 3);
    splitter->addWidget(upper);
    splitter->addWidget(log_);
    splitter->setStretchFactor(0, 4);
    splitter->setStretchFactor(1, 1);

    auto* central = new QWidget();
    auto* root = new QVBoxLayout(central);
    root->addWidget(topRow);
    root->addWidget(splitter, 1);
    setCentralWidget(central);

    statusBar()->showMessage(QString());
    resize(1180, 720);

    btnCancel_->setEnabled(false);
}

void LinuxGuiApp::retranslate() {
    auto& i = I18nQt::Instance();
    setWindowTitle(i.Get(Str::AppTitle));
    langMenu_->setTitle(i.Get(Str::MenuLanguage));
    actEn_->setText(i.Get(Str::MenuEnglish));
    actZh_->setText(i.Get(Str::MenuChinese));
    actEn_->setChecked(i.lang() == Lang::En);
    actZh_->setChecked(i.lang() == Lang::Zh);

    btnFiles_->setText(i.Get(Str::BtnPickFiles));
    btnSend_->setText(i.Get(Str::BtnSendSelected));
    btnCancel_->setText(i.Get(Str::BtnCancel));
    btnDir_->setText(i.Get(Str::BtnReceiveFolder));
    chkTcp_->setText(i.Get(Str::ChkTcpListen));
    chkBt_->setText(i.Get(Str::ChkBtListen));
    btnBtScan_->setText(i.Get(Str::BtnBtScan));

    peers_->setHorizontalHeaderLabels({
        i.Get(Str::PeerName), i.Get(Str::PeerAddress),
        i.Get(Str::PeerKind), i.Get(Str::PeerSeen)});
    const QStringList transferHeaders = {
        i.Get(Str::ColDir), i.Get(Str::ColState), i.Get(Str::ColFile),
        i.Get(Str::ColPeer), i.Get(Str::ColProgress), i.Get(Str::ColTransferred),
        i.Get(Str::ColRate), i.Get(Str::ColEta), i.Get(Str::ColDetail)};
    transfers_->setHorizontalHeaderLabels(transferHeaders);
}

void LinuxGuiApp::setEnglish() {
    I18nQt::Instance().SetLang(Lang::En);
    I18nQt::Instance().SaveToIni();
    retranslate();
    lastRevision_ = ~0ull;
    syncModel(true);
}

void LinuxGuiApp::setChinese() {
    I18nQt::Instance().SetLang(Lang::Zh);
    I18nQt::Instance().SaveToIni();
    retranslate();
    lastRevision_ = ~0ull;
    syncModel(true);
}

// ---------------------------------------------------------------------------
// Model -> view (GUI thread only, driven by the 250 ms timer)
// ---------------------------------------------------------------------------

void LinuxGuiApp::onTick() {
    syncModel(false);
}

void LinuxGuiApp::syncModel(bool force) {
    model_->Pump(NowMs());
    ui::Snapshot s = model_->Take();
    updateStatus(s);
    if (!force && s.revision == lastRevision_) return;
    lastRevision_ = s.revision;
    fillPeers(s);
    fillTransfers(s);
    fillLog(s);
    updateChrome(s);
}

void LinuxGuiApp::fillPeers(const ui::Snapshot& s) {
    int keep = -1;
    auto* current = peers_->currentItem();
    if (current != nullptr) keep = current->row();
    std::string keepKey = (keep >= 0 && keep < static_cast<int>(peerKeys_.size()))
                              ? peerKeys_[static_cast<size_t>(keep)] : std::string();

    peers_->setRowCount(static_cast<int>(s.peers.size()));
    peerKeys_.clear();
    peerTransports_.clear();
    int row = 0;
    for (const auto& p : s.peers) {
        setCell(peers_, row, 0, QString::fromStdString(p.name.empty() ? p.key : p.name));
        setCell(peers_, row, 1, QString::fromStdString(p.key));
        setCell(peers_, row, 2, PeerKindText(p.kind));
        setCell(peers_, row, 3, p.stale ? I18nQt::Instance().Get(Str::PeerStale)
                                       : I18nQt::Instance().Get(Str::PeerLive));
        peerKeys_.push_back(p.key);
        peerTransports_.push_back(p.transport);
        ++row;
    }
    if (!keepKey.empty()) {
        for (size_t k = 0; k < peerKeys_.size(); ++k) {
            if (peerKeys_[k] == keepKey) {
                peers_->setCurrentCell(static_cast<int>(k), 0);
                break;
            }
        }
    }
}

void LinuxGuiApp::fillTransfers(const ui::Snapshot& s) {
    // Build the mirror vector the same way the Win32 GUI does.
    std::vector<TransferMirror> next;
    next.reserve(s.sends.size() + s.recvs.size());

    for (const auto& r : s.sends) {
        TransferMirror m;
        m.id = r.id;
        m.state = r.state;
        m.pct = r.pct;
        m.isSend = true;
        m.cancellable = r.state == ui::TaskState::Running && !r.cancelRequested;
        m.savedPath = r.sourcePath;
        m.stateTint = StateTint(r.state);
        m.cell[COL_DIR] = I18nQt::Instance().Get(Str::DirOut);
        m.cell[COL_STATE] = StateText(r.state);
        m.cell[COL_NAME] = QString::fromStdString(r.name);
        m.cell[COL_PEER] = QString::fromStdString(r.peer);
        m.cell[COL_PROGRESS] = (r.state == ui::TaskState::Queued)
                                   ? QStringLiteral("-")
                                   : QString("%1%").arg(r.pct);
        m.cell[COL_SIZE] = QString("%1 / %2").arg(Human(r.done), Human(r.total));
        m.cell[COL_RATE] = (r.state == ui::TaskState::Running && r.bytesPerSec > 0)
                               ? Human(r.bytesPerSec) + QLatin1String("/s")
                               : QStringLiteral("-");
        m.cell[COL_ETA] = (r.state == ui::TaskState::Running)
                              ? EtaText(r.etaSec) : QStringLiteral("-");
        QString detail;
        if (!r.detail.empty()) detail = QString::fromStdString(r.detail);
        else if (r.state == ui::TaskState::Completed && !r.sha256.empty())
            detail = I18nQt::Instance().GetFmt(Str::DetailSha256Prefix,
                        {QString::fromStdString(r.sha256.substr(0, 16))});
        if (r.cancelRequested)
            detail = I18nQt::Instance().Get(Str::DetailCanceling) + detail;
        m.cell[COL_DETAIL] = detail;
        next.push_back(std::move(m));
    }

    for (const auto& r : s.recvs) {
        TransferMirror m;
        m.id = r.id;
        m.state = r.state;
        m.pct = r.pct;
        m.isSend = false;
        m.savedPath = r.savedPath;
        m.stateTint = StateTint(r.state);
        m.cell[COL_DIR] = I18nQt::Instance().Get(Str::DirIn);
        m.cell[COL_STATE] = StateText(r.state);
        m.cell[COL_NAME] = QString::fromStdString(r.name);
        m.cell[COL_PEER] = QString::fromStdString(r.peer);
        m.cell[COL_PROGRESS] = QString("%1%").arg(r.pct);
        m.cell[COL_SIZE] = QString("%1 / %2").arg(Human(r.done), Human(r.size));
        m.cell[COL_RATE] = (r.state == ui::TaskState::Running && r.bytesPerSec > 0)
                               ? Human(r.bytesPerSec) + QLatin1String("/s")
                               : QStringLiteral("-");
        m.cell[COL_ETA] = (r.state == ui::TaskState::Running)
                              ? EtaText(r.etaSec) : QStringLiteral("-");
        QString detail;
        if (!r.error.empty()) detail = QString::fromStdString(r.error);
        else if (r.integrityVerified) detail = I18nQt::Instance().Get(Str::DetailSha256Verified);
        else if (r.state == ui::TaskState::Completed) detail = I18nQt::Instance().Get(Str::DetailNoHashTrailer);
        else if (!r.savedPath.empty()) detail = QString::fromStdString(r.savedPath);
        m.cell[COL_DETAIL] = detail;
        next.push_back(std::move(m));
    }

    // In-place update when it's the same tasks in the same order (common case
    // during a transfer): keeps scroll position and selection stable.
    bool sameRows = next.size() == transferMirror_.size();
    if (sameRows) {
        for (size_t k = 0; k < next.size(); ++k) {
            if (next[k].id != transferMirror_[k].id) { sameRows = false; break; }
        }
    }

    ui::TaskId keepId = 0;
    int cur = selectedTransferRow();
    if (cur >= 0 && cur < static_cast<int>(transferMirror_.size()))
        keepId = transferMirror_[static_cast<size_t>(cur)].id;

    if (sameRows) {
        for (size_t k = 0; k < next.size(); ++k) {
            for (int c = 0; c < COL_TRANSFER_COUNT; ++c)
                setCell(transfers_, static_cast<int>(k), c, next[k].cell[c]);
            auto* item = transfers_->item(static_cast<int>(k), COL_PROGRESS);
            if (item != nullptr) {
                item->setData(static_cast<int>(RowRole::TaskId),
                              static_cast<qulonglong>(next[k].id));
                item->setData(static_cast<int>(RowRole::Pct), next[k].pct);
                item->setData(static_cast<int>(RowRole::StateTint), next[k].stateTint);
            }
        }
        transferMirror_.swap(next);
        refreshCancelButton();
        return;
    }

    transfers_->setRowCount(static_cast<int>(next.size()));
    transferMirror_.swap(next);
    for (size_t k = 0; k < transferMirror_.size(); ++k) {
        for (int c = 0; c < COL_TRANSFER_COUNT; ++c)
            setCell(transfers_, static_cast<int>(k), c, transferMirror_[k].cell[c]);
        auto* item = transfers_->item(static_cast<int>(k), COL_PROGRESS);
        if (item != nullptr) {
            item->setData(static_cast<int>(RowRole::TaskId),
                          static_cast<qulonglong>(transferMirror_[k].id));
            item->setData(static_cast<int>(RowRole::Pct), transferMirror_[k].pct);
            item->setData(static_cast<int>(RowRole::StateTint), transferMirror_[k].stateTint);
        }
    }
    if (keepId != 0) {
        for (size_t k = 0; k < transferMirror_.size(); ++k) {
            if (transferMirror_[k].id == keepId) {
                transfers_->setCurrentCell(static_cast<int>(k), 0);
                break;
            }
        }
    }
    refreshCancelButton();
}

void LinuxGuiApp::fillLog(const ui::Snapshot& s) {
    log_->clear();
    for (const auto& l : s.logs) {
        const int64_t sec = l.atMs / 1000 % 86400;
        log_->addItem(QString("%1:%2:%3  %4")
                          .arg(sec / 3600, 2, 10, QChar('0'))
                          .arg(sec / 60 % 60, 2, 10, QChar('0'))
                          .arg(sec % 60, 2, 10, QChar('0'))
                          .arg(QString::fromStdString(l.text)));
    }
    if (log_->count() > 0) log_->scrollToBottom();
}

void LinuxGuiApp::updateChrome(const ui::Snapshot& s) {
    setWindowTitle(I18nQt::Instance().GetFmt(Str::TitleTemplate, {
        QString::number(static_cast<int>(s.peers.size())),
        QString::number(static_cast<int>(s.sends.size() + s.recvs.size())),
        QString::fromStdString(s.receiveDir)}));
    refreshCancelButton();
}

void LinuxGuiApp::updateStatus(const ui::Snapshot& s) {
    size_t running = 0;
    for (const auto& r : s.sends)
        if (r.state == ui::TaskState::Running) ++running;
    for (const auto& r : s.recvs)
        if (r.state == ui::TaskState::Running) ++running;

    auto& i = I18nQt::Instance();
    const QString onPort = i.GetFmt(Str::StatusOnPort, {QString::number(s.tcpPort)});
    const QString on = i.Get(Str::StatusOn);
    const QString off = i.Get(Str::StatusOff);
    const QString conn = i.GetFmt(Str::StatusConn, {
        s.tcpListening ? onPort : off,
        s.btListening ? on : off,
        s.beaconRunning ? on : off});
    const QString counts = i.GetFmt(Str::StatusCounts, {
        QString::number(static_cast<int>(s.peers.size())),
        QString::number(static_cast<int>(running))});
    const QString dest = i.GetFmt(Str::StatusReceive, {
        QString::fromStdString(s.receiveDir)});
    statusBar()->showMessage(QString("%1    |    %2    |    %3")
                                 .arg(conn, counts, dest));
}

void LinuxGuiApp::refreshCancelButton() {
    const int idx = selectedTransferRow();
    const bool can = idx >= 0 &&
                     transferMirror_[static_cast<size_t>(idx)].cancellable;
    btnCancel_->setEnabled(can);
}

int LinuxGuiApp::selectedPeerRow() const {
    auto* item = peers_->currentItem();
    return item == nullptr ? -1 : item->row();
}

int LinuxGuiApp::selectedTransferRow() const {
    auto* item = transfers_->currentItem();
    return item == nullptr ? -1 : item->row();
}

// ---------------------------------------------------------------------------
// Actions (GUI thread)
// ---------------------------------------------------------------------------

void LinuxGuiApp::pickAndQueue() {
    const int peerIdx = selectedPeerRow();
    std::string peer;
    ui::Transport transport = ui::Transport::Tcp;
    if (peerIdx >= 0 && peerIdx < static_cast<int>(peerKeys_.size())) {
        peer = peerKeys_[static_cast<size_t>(peerIdx)];
        transport = peerTransports_[static_cast<size_t>(peerIdx)];
    }

    const QStringList paths = QFileDialog::getOpenFileNames(
        this, I18nQt::Instance().Get(Str::BtnPickFiles), QString());
    if (paths.isEmpty()) return;

    for (const QString& p : paths) {
        const std::string path = p.toStdString();
        ui::TaskRow seed;
        seed.name = platform::Basename(path);
        seed.sourcePath = path;
        seed.peer = peer;                 // empty peer is rejected by the model
        seed.transport = transport;
        seed.total = platform::FileSize(path);
        model_->EnqueueSend(seed, NowMs());
    }
    syncModel(true);
}

void LinuxGuiApp::startSelected() {
    const int idx = selectedTransferRow();
    if (idx < 0 || idx >= static_cast<int>(transferMirror_.size())) return;
    const TransferMirror m = transferMirror_[static_cast<size_t>(idx)];
    if (!m.isSend) return;

    ui::TaskId id = m.id;
    if (m.state == ui::TaskState::Queued) {
        // launch the row the model already has
    } else if (ui::IsTerminal(m.state)) {
        ui::TaskRow seed;
        seed.name = QString(m.cell[COL_NAME]).toStdString();
        seed.sourcePath = m.savedPath;
        seed.peer = QString(m.cell[COL_PEER]).toStdString();
        seed.total = platform::FileSize(m.savedPath);
        id = model_->EnqueueSend(seed, NowMs());
        if (id == 0) { syncModel(true); return; }
    } else {
        return;  // already on the wire; use Cancel
    }

    ui::Snapshot s = model_->Take();
    for (const auto& r : s.sends) {
        if (r.id == id) {
            if (r.state == ui::TaskState::Queued) launchTask(r);
            break;
        }
    }
    syncModel(true);
}

void LinuxGuiApp::cancelSelected() {
    const int idx = selectedTransferRow();
    if (idx < 0 || idx >= static_cast<int>(transferMirror_.size())) return;
    const ui::TaskId id = transferMirror_[static_cast<size_t>(idx)].id;
    if (!model_->TryCancel(id)) {
        ui::UiEvent ev;
        ev.kind = ui::UiEvent::Kind::Info;
        ev.detail = "nothing to cancel (already finished, or an inbound row)";
        model_->Post(std::move(ev));
    } else {
        ui::UiEvent ev;
        ev.kind = ui::UiEvent::Kind::Info;
        ev.detail = "cancel requested; the transfer stops at the next chunk";
        model_->Post(std::move(ev));
    }
    syncModel(true);
}

void LinuxGuiApp::chooseReceiveDir() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, I18nQt::Instance().Get(Str::BtnReceiveFolder),
        QString::fromStdString(receiveDir_));
    if (dir.isEmpty()) return;
    const std::string d = dir.toStdString();
    if (d == receiveDir_) return;
    receiveDir_ = d;
    model_->SetReceiveDir(d);

    ui::UiEvent ev;
    ev.kind = ui::UiEvent::Kind::Info;
    ev.detail = "receive folder -> " + d;
    model_->Post(std::move(ev));

    const bool tcpWas = chkTcp_->isChecked();
    const bool btWas = chkBt_->isChecked();
    if (tcpWas) toggleTcp(false);
    if (btWas) toggleBt(false);
    if (tcpWas) toggleTcp(true);
    if (btWas) toggleBt(true);
    syncModel(true);
}

void LinuxGuiApp::toggleTcp(bool enable) {
    if (!enable) {
        if (!tcp_.IsRunning()) return;
        tcp_.Stop();
        ui::UiEvent ev;
        ev.kind = ui::UiEvent::Kind::TcpListenStopped;
        model_->Post(std::move(ev));
        syncModel(true);
        return;
    }
    if (tcp_.IsRunning()) return;

    auto model = model_;
    const bool ok = tcp_.Start(
        tcpPort_, receiveDir_,
        [model](const tcp::ReceivedFile& rf) {
            ui::UiEvent ev;
            ev.kind = rf.errorMessage.empty() ? ui::UiEvent::Kind::RecvCompleted
                                              : ui::UiEvent::Kind::RecvFailed;
            ev.transport = ui::Transport::Tcp;
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
        [model](const tcp::RecvUpdate& u) {
            if (u.stage == tcp::RecvUpdate::Stage::Started) {
                ui::UiEvent ev;
                ev.kind = ui::UiEvent::Kind::RecvStarted;
                ev.transport = ui::Transport::Tcp;
                ev.connId = u.connId;
                ev.key = u.peerHost;
                ev.name = u.fileName;
                ev.size = u.total;
                model->Post(std::move(ev));
            } else {
                model->PostRecvProgress(ui::Transport::Tcp, u.connId, u.done, u.total);
            }
        });

    ui::UiEvent ev;
    if (ok) {
        ev.kind = ui::UiEvent::Kind::TcpListenStarted;
        ev.port = tcp_.port();
    } else {
        ev.kind = ui::UiEvent::Kind::TcpListenFailed;
        ev.port = tcpPort_;
        ev.detail = "cannot bind TCP port " + std::to_string(tcpPort_) +
                    " (already in use, or blocked)";
    }
    model_->Post(std::move(ev));
    syncModel(true);
}

void LinuxGuiApp::toggleBt(bool enable) {
    if (!enable) {
        if (!bt_.IsRunning()) return;
        bt_.Stop();
        ui::UiEvent ev;
        ev.kind = ui::UiEvent::Kind::BtListenStopped;
        model_->Post(std::move(ev));
        syncModel(true);
        return;
    }
    if (bt_.IsRunning()) return;
    if (!bt::IsSupported()) {
        ui::UiEvent ev;
        ev.kind = ui::UiEvent::Kind::BtListenFailed;
        ev.detail = "Bluetooth not supported (BlueZ dev headers missing?)";
        model_->Post(std::move(ev));
        syncModel(true);
        return;
    }

    auto model = model_;
    const bool ok = bt_.Start(
        receiveDir_,
        [model](const bt::ReceivedFile& rf) {
            ui::UiEvent ev;
            ev.kind = rf.errorMessage.empty() ? ui::UiEvent::Kind::RecvCompleted
                                              : ui::UiEvent::Kind::RecvFailed;
            ev.transport = ui::Transport::Bluetooth;
            ev.connId = rf.connId;
            ev.key = rf.peerAddress;
            ev.name = rf.fileName;
            ev.path = rf.savedPath;
            ev.detail = rf.errorMessage;
            ev.sha256 = rf.receivedHash;
            ev.size = rf.fileSize;
            ev.integrityVerified = rf.integrityVerified;
            model->Post(std::move(ev));
        },
        [model](const bt::RecvUpdate& u) {
            if (u.stage == bt::RecvUpdate::Stage::Started) {
                ui::UiEvent ev;
                ev.kind = ui::UiEvent::Kind::RecvStarted;
                ev.transport = ui::Transport::Bluetooth;
                ev.connId = u.connId;
                ev.key = u.peerAddress;
                ev.name = u.fileName;
                ev.size = u.total;
                model->Post(std::move(ev));
            } else {
                model->PostRecvProgress(ui::Transport::Bluetooth, u.connId,
                                        u.done, u.total);
            }
        });

    ui::UiEvent ev;
    ev.kind = ok ? ui::UiEvent::Kind::BtListenStarted
                 : ui::UiEvent::Kind::BtListenFailed;
    if (!ok) ev.detail = "cannot register the RFCOMM service (no adapter?)";
    model_->Post(std::move(ev));
    syncModel(true);
}

void LinuxGuiApp::scanBluetooth() {
    if (btScanning_->exchange(true)) return;
    auto model = model_;
    auto flag = btScanning_;
    auto shutdown = shutdown_;
    std::thread([model, flag, shutdown] {
        auto post = [model](ui::PeerKind kind, const bt::PeerDevice& d) {
            ui::UiEvent ev;
            ev.kind = ui::UiEvent::Kind::PeerSeen;
            ev.transport = ui::Transport::Bluetooth;
            ev.peerKind = kind;
            ev.key = d.address;   // byte-exact MAC, it is a map key
            ev.name = d.name.empty() ? d.address : d.name;
            model->Post(std::move(ev));
        };
        for (const auto& d : bt::EnumeratePairedDevices(0)) {
            if (shutdown->load()) return;
            post(ui::PeerKind::BluetoothPaired, d);
        }
        ui::UiEvent info;
        info.kind = ui::UiEvent::Kind::Info;
        info.detail = "Bluetooth inquiry: about 12 s...";
        model->Post(std::move(info));
        bt::DiscoverDevices([&post](const bt::PeerDevice& d) {
            post(ui::PeerKind::BluetoothInquiry, d);
        }, 12);
        ui::UiEvent finished;
        finished.kind = ui::UiEvent::Kind::Info;
        finished.detail = "Bluetooth inquiry finished";
        model->Post(std::move(finished));
        flag->store(false);
    }).detach();
}

// ---------------------------------------------------------------------------
// Background producers (detached threads — capture only the model)
// ---------------------------------------------------------------------------

void LinuxGuiApp::launchTask(const ui::TaskRow& row) {
    auto model = model_;
    ui::UiEvent started;
    started.kind = ui::UiEvent::Kind::TaskStarted;
    started.id = row.id;
    started.name = row.name;
    started.path = row.sourcePath;
    started.size = row.total;
    started.atMs = NowMs();
    model->Post(std::move(started));
    model->Pump(NowMs());
    startSend(row);
}

void LinuxGuiApp::startSend(const ui::TaskRow& row) {
    auto model = model_;
    const ui::TaskId id = row.id;
    const std::string peer = row.peer;
    const std::string path = row.sourcePath;
    const std::string name = row.name;
    const ui::Transport transport = row.transport;
    auto token = model->CancelToken(id);

    std::thread([model, id, peer, path, name, transport, token] {
        auto onProgress = [model, id](int64_t done, int64_t total) {
            model->PostProgress(id, done, total);
        };
        const std::atomic<bool>* rawToken = token ? token.get() : nullptr;
        bool ok = false;
        std::string error, sha;
        if (transport == ui::Transport::Bluetooth) {
            auto res = bt::SendFile(peer, path, name, onProgress, true, rawToken);
            ok = res.ok; error = res.error; sha = res.senderHash;
        } else {
            auto res = tcp::SendFile(peer, tcp::kDefaultFilePort, path, name,
                                     onProgress, true, rawToken);
            ok = res.ok; error = res.error; sha = res.senderHash;
        }
        ui::UiEvent done;
        done.id = id;
        done.name = name;
        if (ok) {
            done.kind = ui::UiEvent::Kind::TaskCompleted;
            done.sha256 = sha;
        } else if (error == "canceled") {
            done.kind = ui::UiEvent::Kind::TaskCanceled;
        } else {
            done.kind = ui::UiEvent::Kind::TaskFailed;
            done.detail = error;
        }
        model->Post(std::move(done));
    }).detach();
}

void LinuxGuiApp::runDiscovery() {
    auto model = model_;
    auto shutdown = shutdown_;
    std::thread([model, shutdown] {
        discovery::Beacon beacon;
        if (beacon.Start()) {
            ui::UiEvent ev;
            ev.kind = ui::UiEvent::Kind::BeaconStarted;
            model->Post(std::move(ev));
        }
        while (!shutdown->load()) {
            for (const auto& p : discovery::Listen(3)) {
                if (shutdown->load()) break;
                ui::UiEvent ev;
                ev.kind = ui::UiEvent::Kind::PeerSeen;
                ev.transport = ui::Transport::Tcp;
                ev.peerKind = ui::PeerKind::Lan;
                ev.key = p.ip;     // byte-exact IPv4 text, it is a map key
                ev.name = p.host;
                ev.atMs = NowMs();
                model->Post(std::move(ev));
            }
        }
        beacon.Stop();
    }).detach();
}

void LinuxGuiApp::runTcpListener() {
    // TCP listening is managed by the member listener via toggleTcp(); this
    // helper is kept for symmetry with the TUI and is intentionally unused.
}

void LinuxGuiApp::onPeerDoubleClicked(int row, int /*col*/) {
    if (row < 0 || row >= static_cast<int>(peerKeys_.size())) return;
    pickAndQueue();
}

void LinuxGuiApp::onTransferDoubleClicked(int row, int /*col*/) {
    if (row < 0 || row >= static_cast<int>(transferMirror_.size())) return;
    const TransferMirror& m = transferMirror_[static_cast<size_t>(row)];
    if (m.isSend) {
        startSelected();
    } else if (m.state == ui::TaskState::Completed && !m.savedPath.empty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(
            QString::fromStdString(m.savedPath)));
    }
}

void LinuxGuiApp::closeEvent(QCloseEvent* event) {
    shutdown_->store(true);
    tcp_.Stop();
    bt_.Stop();
    QMainWindow::closeEvent(event);
}

} // namespace linuxgui
} // namespace wfd


