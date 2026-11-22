// LinuxGuiApp.h — Qt6 desktop front-end for the wfd file-transfer client.
//
// Deliberately thin, like its Win32 sibling: every decision with consequences
// (event coalescing, terminal-state protection, rate/ETA, row caps) lives in
// UiModel and is unit-tested. This class only owns widgets, layout, and the
// Qt-side rendering of a UiModel snapshot.
//
// Threading contract (same as Win GUI / tui):
//   * transport callbacks fire on detached threads and capture ONLY
//     shared_ptr<UiModel> by value — never a QWidget or a LinuxGuiApp*.
//   * the GUI thread advances the model on a 250 ms QTimer (Pump -> Take),
//     which is also the lost-wake-up fallback. No cross-thread widget calls.
#pragma once

#include <QMainWindow>
#include <QMenuBar>
#include <QStatusBar>
#include <QTableWidget>
#include <QListWidget>
#include <QPushButton>
#include <QCheckBox>
#include <QTimer>
#include <QActionGroup>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "BluetoothTransport.h"
#include "Discovery.h"
#include "TcpTransport.h"
#include "UiModel.h"

class QCloseEvent;

namespace wfd {
namespace linuxgui {

class TransferDelegate;

class LinuxGuiApp : public QMainWindow {
    Q_OBJECT
public:
    explicit LinuxGuiApp(std::shared_ptr<ui::UiModel> model,
                         const std::string& receiveDir, int tcpPort,
                         QWidget* parent = nullptr);
    ~LinuxGuiApp() override;

    enum TransferColumn {
        COL_DIR = 0, COL_STATE, COL_NAME, COL_PEER, COL_PROGRESS,
        COL_SIZE, COL_RATE, COL_ETA, COL_DETAIL, COL_TRANSFER_COUNT
    };

private slots:
    void onTick();
    void pickAndQueue();
    void startSelected();
    void cancelSelected();
    void chooseReceiveDir();
    void toggleTcp(bool enable);
    void toggleBt(bool enable);
    void scanBluetooth();
    void onPeerDoubleClicked(int row, int col);
    void onTransferDoubleClicked(int row, int col);
    void setEnglish();
    void setChinese();

private:
    void buildUi();
    void retranslate();

    void syncModel(bool force);
    void fillPeers(const ui::Snapshot& s);
    void fillTransfers(const ui::Snapshot& s);
    void fillLog(const ui::Snapshot& s);
    void updateChrome(const ui::Snapshot& s);
    void updateStatus(const ui::Snapshot& s);
    void refreshCancelButton();

    void launchTask(const ui::TaskRow& row);
    void startSend(const ui::TaskRow& row);
    void runDiscovery();
    void runTcpListener();
    void closeEvent(QCloseEvent* event) override;

    int selectedPeerRow() const;
    int selectedTransferRow() const;

    struct TransferMirror {
        ui::TaskId id = 0;
        ui::TaskState state = ui::TaskState::Queued;
        int pct = 0;
        bool isSend = true;
        bool cancellable = false;
        std::string savedPath;
        QString cell[COL_TRANSFER_COUNT];
        int stateTint = 0;
    };

    std::shared_ptr<ui::UiModel> model_;
    std::string receiveDir_;
    int tcpPort_;

    QMenu* langMenu_ = nullptr;
    QAction* actEn_ = nullptr;
    QAction* actZh_ = nullptr;
    QPushButton* btnFiles_ = nullptr;
    QPushButton* btnSend_ = nullptr;
    QPushButton* btnCancel_ = nullptr;
    QPushButton* btnDir_ = nullptr;
    QPushButton* btnBtScan_ = nullptr;
    QCheckBox* chkTcp_ = nullptr;
    QCheckBox* chkBt_ = nullptr;
    QTableWidget* peers_ = nullptr;
    QTableWidget* transfers_ = nullptr;
    QListWidget* log_ = nullptr;
    QTimer* timer_ = nullptr;
    TransferDelegate* progressDelegate_ = nullptr;

    std::vector<std::string> peerKeys_;
    std::vector<ui::Transport> peerTransports_;
    std::vector<TransferMirror> transferMirror_;

    tcp::TcpListener tcp_;
    bt::BluetoothListener bt_;
    std::shared_ptr<std::atomic<bool>> shutdown_ =
        std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> btScanning_ =
        std::make_shared<std::atomic<bool>>(false);

    uint64_t lastRevision_ = ~0ull;
};

} // namespace linuxgui
} // namespace wfd
