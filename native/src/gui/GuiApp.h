// GuiApp.h — the Win32 front-end. Deliberately thin.
//
// Everything with behavior — event coalescing, terminal-state protection,
// rates, ETA, row caps, peer identity — lives in UiModel and is unit-tested on
// Linux (`native/tests/test_ui_model.cpp`). What is left here is window
// creation, UTF-8→UTF-16 for display, and copy. That trade is deliberate:
// wfd_gui.exe builds under MinGW but cannot be exercised in CI, so nothing
// worth trusting should live in this layer.
//
// Threading contract:
//   * transport callbacks fire on detached connection threads that can outlive
//     TcpListener::Stop(). They therefore capture `shared_ptr<UiModel>` by value
//     and NOTHING else — no HWND, no GuiApp*.
//   * the only cross-thread touch on the window is WakeUi(), which reads an
//     atomic HWND and posts a message carrying no data. LPARAM-carrying buffers
//     would leak once DestroyWindow starts discarding messages.
//   * a 250 ms timer re-syncs regardless, so a lost wake-up cannot wedge the UI.
#pragma once

#ifndef UNICODE
#define UNICODE 1
#endif
#include <windows.h>

#include <commctrl.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "BluetoothTransport.h"
#include "Discovery.h"
#include "TcpTransport.h"
#include "UiModel.h"

namespace wfd {
namespace gui {

// Safe from any thread, before creation and after destruction of the window.
// The message is a wake-up only — it carries no data, because DestroyWindow
// silently discards queued messages and a heap buffer in LPARAM would leak.
inline constexpr UINT kWakeMsg = WM_APP + 1;
void WakeUi();

class GuiApp {
public:
    explicit GuiApp(std::shared_ptr<ui::UiModel> model);
    ~GuiApp();

    GuiApp(const GuiApp&) = delete;
    GuiApp& operator=(const GuiApp&) = delete;

    bool Create(HINSTANCE instance, int showCmd, const std::string& receiveDir,
                int tcpPort);

    // Valid from Create() until the window is destroyed. The message loop needs
    // it for IsDialogMessageW, which drives Tab / arrow / mnemonic navigation.
    HWND hwnd() const { return hwnd_; }

    // Transfers list geometry. COL_PROGRESS is the column the custom-drawn
    // strip is painted into and the percentage text shares the cell. Public
    // because the column table, the row mirror and the custom-draw handler all
    // index it and must agree.
    enum TransferColumn {
        COL_DIR = 0,
        COL_STATE,
        COL_NAME,
        COL_PEER,
        COL_PROGRESS,
        COL_SIZE,
        COL_RATE,
        COL_ETA,
        COL_DETAIL,
        COL_TRANSFER_COUNT
    };

private:
    enum ControlId {
        IDC_BTN_FILES = 1001,
        IDC_BTN_SEND,
        IDC_BTN_CANCEL,
        IDC_BTN_DIR,
        IDC_CHK_TCP,
        IDC_CHK_BT,
        IDC_BTN_BTSCAN,
        IDC_LST_PEERS,
        IDC_LST_TRANSFERS,
        IDC_LST_LOG,
        IDC_STATUS,
    };
    enum : UINT_PTR { IDT_REFRESH = 1 };
    static constexpr int kBarHeight = 4;  // progress strip thickness

    // What the transfers list needs to know about a row beyond its state:
    // which model task it is, everything required to (re)launch or cancel it
    // without consulting any other index, and the text already rendered.
    struct TransferMirror {
        ui::TaskId    id = 0;
        ui::Transport transport = ui::Transport::Tcp;
        ui::TaskState state = ui::TaskState::Queued;
        int           pct = 0;
        bool          isSend = true;
        bool          cancellable = false;
        std::string   peer;
        std::string   sourcePath;
        std::string   name;
        std::wstring  cell[COL_TRANSFER_COUNT];
    };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wParam, LPARAM lParam);

    void BuildControls();
    void Layout(RECT client);
    void SyncModel(bool force);
    void FillPeers(const ui::Snapshot&);
    void FillTransfers(const ui::Snapshot&);
    void BuildTransferMirrors(const ui::Snapshot&,
                              std::vector<TransferMirror>& out);
    void WriteTransferRow(int row, const TransferMirror& m);
    void FillLog(const ui::Snapshot&);
    void UpdateChrome(const ui::Snapshot&);
    void RefreshCancelButton();
    void UpdateStatus(const ui::Snapshot&);
    void DrawProgressStrip(LPNMLVCUSTOMDRAW cd);
    void StartDiscoveryLoop();

    void PickAndQueue();
    void StartSelected();
    void CancelSelected();
    void ChooseReceiveDir();
    void ToggleTcp(bool enable);
    void ToggleBt(bool enable);
    void ScanBluetooth();
    void LaunchTask(const ui::TaskRow& row);

    int SelectedPeerIndex() const;
    int SelectedTransferIndex() const;

    std::shared_ptr<ui::UiModel> model_;
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND peers_ = nullptr;
    HWND transfers_ = nullptr;
    HWND log_ = nullptr;
    HWND status_ = nullptr;
    HWND btnFiles_ = nullptr;
    HWND btnSend_ = nullptr;
    HWND btnCancel_ = nullptr;
    HWND btnDir_ = nullptr;
    HWND chkTcp_ = nullptr;
    HWND chkBt_ = nullptr;
    HWND btnBtScan_ = nullptr;
    HFONT font_ = nullptr;
    HBRUSH barBack_ = nullptr;
    HBRUSH barFill_ = nullptr;

    std::string receiveDir_;
    int tcpPort_ = 0;
    uint64_t lastRevision_ = ~0ull;

    // Row-index mirrors, so custom draw and the action handlers can map a list
    // index back to model state without storing pointers in the control.
    std::vector<std::string> peerKeys_;
    std::vector<ui::Transport> peerTransports_;
    std::vector<TransferMirror> transferMirror_;

    tcp::TcpListener tcp_;
    bt::BluetoothListener bt_;
    // The discovery beacon is deliberately NOT a member: it is started and
    // stopped on its own worker thread, and a member would be destroyed with
    // GuiApp while that thread might still be using it.
    //
    // The two flags are shared_ptr so a detached scan/discovery thread keeps a
    // valid object even if it finishes after the window is gone.
    std::shared_ptr<std::atomic<bool>> shutdown_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> btScanning_ = std::make_shared<std::atomic<bool>>(false);
};

} // namespace gui
} // namespace wfd
