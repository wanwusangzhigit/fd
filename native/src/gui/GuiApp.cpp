// GuiApp.cpp — Win32 implementation of the wfd desktop client.
//
// Nothing here is clever. Every decision with consequences (which event wins,
// what a terminal row may still be rewritten by, how a rate is sampled, when a
// row is pruned) is made in UiModel and covered by native/tests/test_ui_model.cpp.
#include "GuiApp.h"

#include "Platform.h"
#include "UiModel.h"
#include "Util.h"
#include "Win32Util.h"

#include <commctrl.h>

#ifndef CDRF_SUBITEMPASSTHRU
// Present in the MS SDK, missing from MinGW-w64's commctrl.h. Returned from the
// sub-item pre-paint stage to let the control draw the cell text and then call
// back at CDDS_ITEMPOSTPAINT | CDDS_SUBITEM, which is where the strip goes.
#define CDRF_SUBITEMPASSTHRU 0x00020001
#endif

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <thread>

#ifdef _MSC_VER
// Common Controls v6 has to be activated through a manifest. MinGW gets it from
// wfd_gui.rc; an MSVC build here has no .rc, so let the linker emit it. Guarded
// so MinGW does not end up with two conflicting manifests.
#pragma comment(linker, "\"/manifestdependency:type='win32' "                   \
                        "name='Microsoft.Windows.Common-Controls' "             \
                        "version='6.0.0.0' processorArchitecture='*' "          \
                        "publicKeyToken='6595b64144ccf1df' language='*'\"")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "gdi32.lib")
#endif

namespace wfd {
namespace gui {

namespace {

// The only window handle a non-UI thread may touch. Cleared in WM_DESTROY, so
// WakeUi() turns into a no-op and pending events simply stop being rendered.
std::atomic<HWND> g_mainWnd{nullptr};

constexpr const wchar_t* kWndClass = L"wfd_gui.main";

// Column layout for the transfers list; the enum itself is GuiApp::TransferColumn,
// shared with the mirror that stores the rendered text.
constexpr int kTransferCols = static_cast<int>(GuiApp::COL_TRANSFER_COUNT);

constexpr const wchar_t* kTransferHeaders[kTransferCols] = {
    L"Dir", L"State", L"File", L"Peer", L"Progress",
    L"Transferred", L"Rate", L"ETA", L"Detail",
};

// Un-scaled: InsertColumn runs them through S().
constexpr int kTransferWidths[kTransferCols] = {
    38, 52, 190, 140, 78, 120, 92, 78, 260,
};

// System-DPI-aware process: coordinates are physical pixels, so the fixed
// metrics below are scaled by the screen's logical DPI.
float DpiScale() {
    static float scale = [] {
        HDC dc = ::GetDC(nullptr);
        const int dpi = dc ? ::GetDeviceCaps(dc, LOGPIXELSX) : 96;
        if (dc) ::ReleaseDC(nullptr, dc);
        return dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
    }();
    return scale;
}

int S(int v) { return static_cast<int>(static_cast<float>(v) * DpiScale() + 0.5f); }

int64_t TickMs() { return static_cast<int64_t>(::GetTickCount64()); }

std::wstring Text(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = ::vsnwprintf(buf, 1024, fmt, ap);
    va_end(ap);
    if (n < 0) return std::wstring();
    buf[n < 1024 ? n : 1023] = L'\0';
    return std::wstring(buf);
}

std::wstring U8(const std::string& s) { return U8ToW(s); }

std::wstring Human(int64_t bytes) {
    return U8(util::HumanSize(bytes));
}

const wchar_t* StateText(ui::TaskState s) {
    switch (s) {
        case ui::TaskState::Queued:    return L"WAIT";
        case ui::TaskState::Running:   return L"RUN";
        case ui::TaskState::Completed: return L"OK";
        case ui::TaskState::Failed:    return L"FAIL";
        case ui::TaskState::Canceled:  return L"CNCL";
    }
    return L"";
}

const wchar_t* PeerKindText(ui::PeerKind k) {
    switch (k) {
        case ui::PeerKind::Lan: return L"LAN";
        case ui::PeerKind::BluetoothPaired: return L"BT paired";
        case ui::PeerKind::BluetoothInquiry: return L"BT found";
    }
    return L"";
}

std::wstring EtaText(int64_t etaSec) {
    if (etaSec < 0) return L"-";
    if (etaSec < 60) return Text(L"%llds", static_cast<long long>(etaSec));
    return Text(L"%lldm %02llds", static_cast<long long>(etaSec / 60),
                static_cast<long long>(etaSec % 60));
}

void InsertColumn(HWND list, int i, const wchar_t* text, int width) {
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.pszText = const_cast<wchar_t*>(text);
    col.cx = S(width);
    col.iSubItem = i;
    ::SendMessageW(list, LVM_INSERTCOLUMNW, i, reinterpret_cast<LPARAM>(&col));
}

int InsertRow(HWND list, int row, const std::wstring& first) {
    LVITEMW it{};
    it.mask = LVIF_TEXT;
    it.iItem = row;
    it.iSubItem = 0;
    it.pszText = const_cast<wchar_t*>(first.c_str());
    return static_cast<int>(
        ::SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&it)));
}

void SetCell(HWND list, int row, int col, const std::wstring& text) {
    LVITEMW it{};
    it.mask = LVIF_TEXT;
    it.iItem = row;
    it.iSubItem = col;
    it.pszText = const_cast<wchar_t*>(text.c_str());
    ::SendMessageW(list, LVM_SETITEMTEXTW, static_cast<WPARAM>(row),
                   reinterpret_cast<LPARAM>(&it));
}

int SelectedRow(HWND list) {
    return static_cast<int>(
        ::SendMessageW(list, LVM_GETNEXTITEM, static_cast<WPARAM>(-1),
                       static_cast<LPARAM>(LVIS_SELECTED)));
}

void SelectRow(HWND list, int row) {
    if (row < 0) return;
    LVITEMW it{};
    it.mask = LVIF_STATE;
    it.state = LVIS_SELECTED | LVIS_FOCUSED;
    it.stateMask = it.state;
    // LVM_SETITEMSTATE with LVIS_SELECTED scrolls the row into view itself.
    ::SendMessageW(list, LVM_SETITEMSTATE, static_cast<WPARAM>(row),
                   reinterpret_cast<LPARAM>(&it));
}

void ListBeginUpdate(HWND list) { ::SendMessageW(list, WM_SETREDRAW, FALSE, 0); }
void ListEndUpdate(HWND list) {
    ::SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    ::InvalidateRect(list, nullptr, TRUE);
}

HWND CreateButton(HWND parent, HINSTANCE inst, HFONT font, const wchar_t* label,
                  int id, DWORD extra = BS_PUSHBUTTON) {
    HWND b = ::CreateWindowExW(
        0, L"BUTTON", label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | extra, 0, 0,
        0, 0, parent, reinterpret_cast<HMENU>(static_cast<intptr_t>(id)), inst,
        nullptr);
    ::SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return b;
}

HWND CreateList(HWND parent, HINSTANCE inst, HFONT font, int id) {
    HWND list = ::CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS, 0,
        0, 0, 0, parent,
        reinterpret_cast<HMENU>(static_cast<intptr_t>(id)), inst, nullptr);
    ::SendMessageW(list, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    ::SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                   static_cast<LPARAM>(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES |
                                       LVS_EX_DOUBLEBUFFER));
    return list;
}

} // namespace

void WakeUi() {
    if (HWND h = g_mainWnd.load(std::memory_order_acquire)) {
        ::PostMessageW(h, kWakeMsg, 0, 0);
    }
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

GuiApp::GuiApp(std::shared_ptr<ui::UiModel> model) : model_(std::move(model)) {}

GuiApp::~GuiApp() {
    shutdown_->store(true);
    // The listeners are members, so their destructors stop the accept threads.
    // In-flight detached connection handlers keep the shared_ptr<UiModel> alive
    // on their own and simply post into a model nobody paints anymore.
    tcp_.Stop();
    bt_.Stop();
}

bool GuiApp::Create(HINSTANCE instance, int showCmd,
                    const std::string& receiveDir, int tcpPort) {
    instance_ = instance;
    receiveDir_ = receiveDir;
    tcpPort_ = tcpPort;

    WNDCLASSW wc{};
    wc.lpfnWndProc = &GuiApp::WndProc;
    wc.hInstance = instance_;
    wc.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kWndClass;
    if (!::RegisterClassW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    // WS_CLIPCHILDREN | WS_CLIPSIBLINGS: without them the custom-drawn progress
    // strips smear into neighbouring cells whenever something repaints.
    hwnd_ = ::CreateWindowExW(
        0, kWndClass, L"wfd - file transfer",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, CW_USEDEFAULT,
        CW_USEDEFAULT, S(1120), S(700), nullptr, nullptr, instance_, this);
    if (hwnd_ == nullptr) return false;

    ::ShowWindow(hwnd_, showCmd);
    ::UpdateWindow(hwnd_);
    return true;
}

LRESULT CALLBACK GuiApp::WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    GuiApp* self = reinterpret_cast<GuiApp*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        self = reinterpret_cast<GuiApp*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
        return DefWindowProcW(hwnd, msg, w, l);
    }
    if (self == nullptr) return DefWindowProcW(hwnd, msg, w, l);
    return self->Handle(msg, w, l);
}

// ---------------------------------------------------------------------------
// Message handling
// ---------------------------------------------------------------------------

LRESULT GuiApp::Handle(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        BuildControls();
        ::SetTimer(hwnd_, IDT_REFRESH, 250, nullptr);
        // Listen from the start, like the tui does: a transfer client that
        // waits for the user to tick a box before it can receive anything is a
        // surprise the first time a peer tries to send. UpdateChrome then keeps
        // the checkbox honest about whether the bind actually succeeded.
        model_->SetReceiveDir(receiveDir_);
        ::SendMessageW(chkTcp_, BM_SETCHECK, BST_CHECKED, 0);
        ToggleTcp(true);
        StartDiscoveryLoop();
        SyncModel(true);
        return 0;
    }
    case WM_SIZE: {
        RECT rc{};
        ::GetClientRect(hwnd_, &rc);
        Layout(rc);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize.x = S(760);
        mmi->ptMinTrackSize.y = S(460);
        return 0;
    }
    case kWakeMsg:
        SyncModel(false);
        return 0;
    case WM_TIMER:
        if (wParam == IDT_REFRESH) SyncModel(false);
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);
        if (code == BN_CLICKED || code == 0) {
            switch (id) {
            case IDC_BTN_FILES:  PickAndQueue(); return 0;
            case IDC_BTN_SEND:   StartSelected(); return 0;
            case IDC_BTN_CANCEL: CancelSelected(); return 0;
            case IDC_BTN_DIR:    ChooseReceiveDir(); return 0;
            case IDC_BTN_BTSCAN: ScanBluetooth(); return 0;
            case IDC_CHK_TCP:
                ToggleTcp(::SendMessageW(chkTcp_, BM_GETCHECK, 0, 0) == BST_CHECKED);
                return 0;
            case IDC_CHK_BT:
                ToggleBt(::SendMessageW(chkBt_, BM_GETCHECK, 0, 0) == BST_CHECKED);
                return 0;
            default: break;
            }
        }
        return 0;
    }
    case WM_NOTIFY: {
        auto* hdr = reinterpret_cast<NMHDR*>(lParam);
        if (hdr->hwndFrom == transfers_ && hdr->code == NM_CUSTOMDRAW) {
            auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
            switch (cd->nmcd.dwDrawStage) {
            case CDDS_PREPAINT:
                return CDRF_NOTIFYITEMDRAW;
            case CDDS_ITEMPREPAINT:
                return CDRF_NOTIFYSUBITEMDRAW;
            case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
                return CDRF_SUBITEMPASSTHRU;   // we want the post-paint pass
            case CDDS_ITEMPOSTPAINT | CDDS_SUBITEM:
                if (cd->iSubItem == COL_PROGRESS) DrawProgressStrip(cd);
                return CDRF_DODEFAULT;
            default:
                return CDRF_DODEFAULT;
            }
        }
        if (hdr->hwndFrom == transfers_ && hdr->code == LVN_ITEMCHANGED) {
            // A click on another row must re-enable Cancel even when the model
            // did not change, and this path cannot call SyncModel: rebuilding
            // the list would re-notify and recurse.
            RefreshCancelButton();
        }
        if (hdr->hwndFrom == transfers_ &&
            (hdr->code == NM_DBLCLK || hdr->code == LVN_ITEMACTIVATE)) {
            StartSelected();
            return 0;
        }
        if (hdr->hwndFrom == peers_ && hdr->code == NM_DBLCLK) {
            PickAndQueue();
            return 0;
        }
        return 0;
    }
    case WM_DESTROY: {
        // Order matters: stop the timer, tell the background loops to exit,
        // unblock the accept loops, only then make WakeUi() inert.
        ::KillTimer(hwnd_, IDT_REFRESH);
        shutdown_->store(true);
        tcp_.Stop();
        bt_.Stop();
        // The discovery thread owns its Beacon and stops it once it sees
        // shutdown_; nothing here may touch it.
        g_mainWnd.store(nullptr, std::memory_order_release);
        ::PostQuitMessage(0);
        return 0;
    }
    default:
        return ::DefWindowProcW(hwnd_, msg, wParam, lParam);
    }
}

// ---------------------------------------------------------------------------
// Controls and layout
// ---------------------------------------------------------------------------

void GuiApp::BuildControls() {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        font_ = ::CreateFontIndirectW(&ncm.lfMessageFont);
    }
    if (font_ == nullptr) font_ = static_cast<HFONT>(GetStockObject(SYSTEM_FONT));

    barBack_ = ::CreateSolidBrush(RGB(222, 222, 222));
    barFill_ = ::CreateSolidBrush(::GetSysColor(COLOR_HIGHLIGHT));

    btnFiles_  = CreateButton(hwnd_, instance_, font_, L"&Pick files...", IDC_BTN_FILES);
    btnSend_   = CreateButton(hwnd_, instance_, font_, L"&Send selected", IDC_BTN_SEND);
    btnCancel_ = CreateButton(hwnd_, instance_, font_, L"Ca&ncel", IDC_BTN_CANCEL);
    btnDir_    = CreateButton(hwnd_, instance_, font_, L"Receive &folder...", IDC_BTN_DIR);
    chkTcp_    = CreateButton(hwnd_, instance_, font_, L"&TCP listen", IDC_CHK_TCP,
                              BS_AUTOCHECKBOX);
    chkBt_     = CreateButton(hwnd_, instance_, font_, L"&Bluetooth listen",
                              IDC_CHK_BT, BS_AUTOCHECKBOX);
    btnBtScan_ = CreateButton(hwnd_, instance_, font_, L"Bluetooth &scan", IDC_BTN_BTSCAN);

    peers_ = CreateList(hwnd_, instance_, font_, IDC_LST_PEERS);
    InsertColumn(peers_, 0, L"Name", 170);
    InsertColumn(peers_, 1, L"Address", 150);
    InsertColumn(peers_, 2, L"Kind", 80);
    InsertColumn(peers_, 3, L"Seen", 70);

    transfers_ = CreateList(hwnd_, instance_, font_, IDC_LST_TRANSFERS);
    for (int i = 0; i < COL_TRANSFER_COUNT; ++i) {
        InsertColumn(transfers_, i, kTransferHeaders[i], kTransferWidths[i]);
    }

    log_ = CreateList(hwnd_, instance_, font_, IDC_LST_LOG);
    InsertColumn(log_, 0, L"Time", 80);
    InsertColumn(log_, 1, L"Message", 760);

    status_ = ::CreateWindowExW(0, STATUSCLASSNAMEW, L"",
                                WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0,
                                0, hwnd_,
                                reinterpret_cast<HMENU>(
                                    static_cast<intptr_t>(IDC_STATUS)),
                                instance_, nullptr);
    ::SendMessageW(status_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);

    // The window handle is live from here on: hand it to the atomic so worker
    // threads can wake us.
    g_mainWnd.store(hwnd_, std::memory_order_release);
}

void GuiApp::Layout(RECT c) {
    const int m = S(8);
    const int bh = S(26);
    int x = m;
    const int y = m;

    struct Item { HWND hwnd; int w; };
    const Item items[] = {
        {btnFiles_, 110}, {btnSend_, 130}, {btnCancel_, 80}, {btnDir_, 150},
        {chkTcp_, 110}, {chkBt_, 150}, {btnBtScan_, 120},
    };
    for (const auto& it : items) {
        const int w = S(it.w);
        ::MoveWindow(it.hwnd, x, y, w, bh, TRUE);
        x += w + m / 2;
    }

    const int statusH = S(22);
    const int logH = S(140);
    const int top = y + bh + m;
    const int listH = c.bottom - top - logH - statusH - m * 3;
    const int peersW = std::max(S(260), static_cast<int>(c.right) * 27 / 100);

    if (listH > S(60)) {
        ::MoveWindow(peers_, m, top, peersW, listH, TRUE);
        ::MoveWindow(transfers_, m + peersW + m / 2, top,
                     c.right - peersW - m * 2 - m / 2, listH, TRUE);
        ::MoveWindow(log_, m, top + listH + m / 2, c.right - m * 2, logH, TRUE);
    }
    ::MoveWindow(status_, 0, c.bottom - statusH, c.right, statusH, TRUE);
}

// ---------------------------------------------------------------------------
// Model -> view
// ---------------------------------------------------------------------------

void GuiApp::SyncModel(bool force) {
    model_->Pump(TickMs());
    ui::Snapshot snap = model_->Take();
    UpdateStatus(snap);
    if (!force && snap.revision == lastRevision_) return;
    lastRevision_ = snap.revision;
    FillPeers(snap);
    FillTransfers(snap);
    FillLog(snap);
    UpdateChrome(snap);
}

void GuiApp::FillPeers(const ui::Snapshot& s) {
    const int row = SelectedRow(peers_);
    std::string keep =
        (row >= 0 && static_cast<size_t>(row) < peerKeys_.size())
            ? peerKeys_[static_cast<size_t>(row)] : std::string();

    ListBeginUpdate(peers_);
    ::SendMessageW(peers_, LVM_DELETEALLITEMS, 0, 0);
    peerKeys_.clear();
    peerTransports_.clear();

    int i = 0;
    for (const auto& p : s.peers) {
        InsertRow(peers_, i, U8(p.name.empty() ? p.key : p.name));
        SetCell(peers_, i, 1, U8(p.key));
        SetCell(peers_, i, 2, PeerKindText(p.kind));
        SetCell(peers_, i, 3, p.stale ? L"stale" : L"live");
        peerKeys_.push_back(p.key);
        peerTransports_.push_back(p.transport);
        ++i;
    }
    ListEndUpdate(peers_);

    for (size_t k = 0; k < peerKeys_.size(); ++k) {
        if (!keep.empty() && peerKeys_[k] == keep) { SelectRow(peers_, static_cast<int>(k)); break; }
    }
}

// Build the row list plus its rendered text. Kept apart from the ListView call
// sequence so the "same rows?" decision below has something to compare.
void GuiApp::BuildTransferMirrors(const ui::Snapshot& s,
                                  std::vector<TransferMirror>& out) {
    out.clear();
    out.reserve(s.sends.size() + s.recvs.size());

    for (const auto& r : s.sends) {
        TransferMirror m;
        m.id = r.id;
        m.transport = r.transport;
        m.state = r.state;
        m.pct = r.pct;
        m.isSend = true;
        m.cancellable = r.state == ui::TaskState::Running && !r.cancelRequested;
        m.peer = r.peer;
        m.sourcePath = r.sourcePath;
        m.name = r.name;
        m.cell[COL_DIR] = L"out";
        m.cell[COL_STATE] = StateText(r.state);
        m.cell[COL_NAME] = U8(r.name);
        m.cell[COL_PEER] = U8(r.peer);
        m.cell[COL_PROGRESS] =
            r.state == ui::TaskState::Queued ? L"-" : Text(L"%d%%", r.pct);
        m.cell[COL_SIZE] = Text(L"%s / %s", Human(r.done).c_str(),
                                Human(r.total).c_str());
        m.cell[COL_RATE] =
            r.state == ui::TaskState::Running && r.bytesPerSec > 0
                ? Text(L"%s/s", Human(r.bytesPerSec).c_str()) : L"-";
        m.cell[COL_ETA] =
            r.state == ui::TaskState::Running ? EtaText(r.etaSec) : L"-";
        std::wstring detail;
        if (!r.detail.empty()) detail = U8(r.detail);
        else if (r.state == ui::TaskState::Completed && !r.sha256.empty())
            detail = Text(L"sha256 %s", U8(r.sha256.substr(0, 16)).c_str());
        if (r.cancelRequested) detail = L"canceling... " + detail;
        m.cell[COL_DETAIL] = detail;
        out.push_back(std::move(m));
    }

    for (const auto& r : s.recvs) {
        TransferMirror m;
        m.id = r.id;
        m.transport = r.transport;
        m.state = r.state;
        m.pct = r.pct;
        m.isSend = false;
        m.peer = r.peer;
        m.name = r.name;
        m.cell[COL_DIR] = L"in";
        m.cell[COL_STATE] = StateText(r.state);
        m.cell[COL_NAME] = U8(r.name);
        m.cell[COL_PEER] = U8(r.peer);
        m.cell[COL_PROGRESS] = Text(L"%d%%", r.pct);
        m.cell[COL_SIZE] = Text(L"%s / %s", Human(r.done).c_str(),
                                Human(r.size).c_str());
        m.cell[COL_RATE] =
            r.state == ui::TaskState::Running && r.bytesPerSec > 0
                ? Text(L"%s/s", Human(r.bytesPerSec).c_str()) : L"-";
        m.cell[COL_ETA] =
            r.state == ui::TaskState::Running ? EtaText(r.etaSec) : L"-";
        std::wstring detail;
        if (!r.error.empty()) detail = U8(r.error);
        else if (r.integrityVerified) detail = L"sha256 verified";
        else if (r.state == ui::TaskState::Completed) detail = L"no hash trailer";
        else if (!r.savedPath.empty()) detail = U8(r.savedPath);
        m.cell[COL_DETAIL] = detail;
        out.push_back(std::move(m));
    }
}

void GuiApp::WriteTransferRow(int row, const TransferMirror& m) {
    // Column 0 holds the direction tag, which never changes for a given task;
    // InsertRow owns it and this covers the rest.
    for (int c = COL_STATE; c < COL_TRANSFER_COUNT; ++c) {
        SetCell(transfers_, row, c, m.cell[c]);
    }
}

void GuiApp::FillTransfers(const ui::Snapshot& s) {
    std::vector<TransferMirror> next;
    BuildTransferMirrors(s, next);

    // In-place when the rows are the same tasks in the same order, which is the
    // common case while a transfer is running. Rebuilding (LVM_DELETEALLITEMS)
    // every frame would reset the scroll position under the user's cursor.
    bool sameRows = next.size() == transferMirror_.size();
    if (sameRows) {
        for (size_t k = 0; k < next.size(); ++k) {
            if (next[k].id != transferMirror_[k].id) { sameRows = false; break; }
        }
    }

    if (sameRows) {
        for (size_t k = 0; k < next.size(); ++k) {
            const bool barMoved = next[k].pct != transferMirror_[k].pct;
            WriteTransferRow(static_cast<int>(k), next[k]);
            // Text changes repaint themselves; the custom-drawn strip lives in
            // the row background, so it needs an explicit nudge.
            if (barMoved) {
                ::SendMessageW(transfers_, LVM_UPDATE, static_cast<WPARAM>(k), 0);
            }
        }
        transferMirror_.swap(next);
        return;
    }

    const int prev = SelectedTransferIndex();
    const ui::TaskId keep =
        prev >= 0 ? transferMirror_[static_cast<size_t>(prev)].id : 0;

    ListBeginUpdate(transfers_);
    ::SendMessageW(transfers_, LVM_DELETEALLITEMS, 0, 0);
    transferMirror_.swap(next);
    for (size_t k = 0; k < transferMirror_.size(); ++k) {
        InsertRow(transfers_, static_cast<int>(k), transferMirror_[k].cell[COL_DIR]);
        WriteTransferRow(static_cast<int>(k), transferMirror_[k]);
    }
    ListEndUpdate(transfers_);

    for (size_t k = 0; k < transferMirror_.size(); ++k) {
        if (keep != 0 && transferMirror_[k].id == keep) {
            SelectRow(transfers_, static_cast<int>(k));
            break;
        }
    }
}

void GuiApp::FillLog(const ui::Snapshot& s) {
    ListBeginUpdate(log_);
    ::SendMessageW(log_, LVM_DELETEALLITEMS, 0, 0);
    int i = 0;
    for (const auto& l : s.logs) {
        const int64_t sec = l.atMs / 1000 % 86400;
        InsertRow(log_, i, Text(L"%02lld:%02lld:%02lld",
                                static_cast<long long>(sec / 3600),
                                static_cast<long long>(sec / 60 % 60),
                                static_cast<long long>(sec % 60)));
        SetCell(log_, i, 1, U8(l.text));
        ++i;
    }
    const int last = static_cast<int>(s.logs.size()) - 1;
    if (last >= 0) {
        // Follow the tail without selecting the row: a selection marker in the
        // log pane would suggest it means something it does not.
        ::SendMessageW(log_, LVM_ENSUREVISIBLE, static_cast<WPARAM>(last), FALSE);
    }
    ListEndUpdate(log_);
}

void GuiApp::RefreshCancelButton() {
    const int idx = SelectedTransferIndex();
    const bool can = idx >= 0 &&
                     transferMirror_[static_cast<size_t>(idx)].cancellable;
    ::EnableWindow(btnCancel_, can);
}

void GuiApp::UpdateChrome(const ui::Snapshot& s) {
    // BM_SETCHECK does not generate BN_CLICKED, so this cannot re-enter Toggle*.
    ::SendMessageW(chkTcp_, BM_SETCHECK,
                   s.tcpListening ? BST_CHECKED : BST_UNCHECKED, 0);
    ::SendMessageW(chkBt_, BM_SETCHECK,
                   s.btListening ? BST_CHECKED : BST_UNCHECKED, 0);
    RefreshCancelButton();
    const std::wstring title = Text(
        L"wfd - %d peers, %d transfers, receive -> %s",
        static_cast<int>(s.peers.size()),
        static_cast<int>(s.sends.size() + s.recvs.size()),
        U8(s.receiveDir).c_str());
    ::SetWindowTextW(hwnd_, title.c_str());
}

void GuiApp::UpdateStatus(const ui::Snapshot& s) {
    size_t running = 0;
    for (const auto& r : s.sends)
        if (r.state == ui::TaskState::Running) ++running;
    for (const auto& r : s.recvs)
        if (r.state == ui::TaskState::Running) ++running;

    // Counts go through int, not %zu: this string is formatted by vsnwprintf,
    // which on the msvcrt/UCRT side does not reliably implement the z modifier.
    const std::wstring txt = Text(
        L"TCP listen: %s   Bluetooth listen: %s   beacon: %s   peers: %d   "
        L"running: %d   receive folder: %s",
        s.tcpListening ? Text(L"on port %d", s.tcpPort).c_str() : L"off",
        s.btListening ? L"on" : L"off",
        s.beaconRunning ? L"broadcasting" : L"stopped",
        static_cast<int>(s.peers.size()), static_cast<int>(running),
        U8(s.receiveDir).c_str());
    ::SendMessageW(status_, SB_SETTEXTW, 0,
                   reinterpret_cast<LPARAM>(txt.c_str()));
}

void GuiApp::DrawProgressStrip(LPNMLVCUSTOMDRAW cd) {
    const size_t row = static_cast<size_t>(cd->nmcd.dwItemSpec);
    if (row >= transferMirror_.size()) return;

    RECT rc{};
    rc.left = static_cast<LONG>(cd->iSubItem);
    rc.top = LVIR_BOUNDS;
    if (!::SendMessageW(transfers_, LVM_GETSUBITEMRECT, cd->nmcd.dwItemSpec,
                        reinterpret_cast<LPARAM>(&rc))) {
        return;
    }
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;

    rc.top = rc.bottom - S(kBarHeight);
    ::FillRect(cd->nmcd.hdc, &rc, barBack_);
    const int w = (rc.right - rc.left) * transferMirror_[row].pct / 100;
    if (w > 0) {
        RECT fill = rc;
        fill.right = fill.left + w;
        ::FillRect(cd->nmcd.hdc, &fill, barFill_);
    }
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

int GuiApp::SelectedPeerIndex() const {
    const int row = SelectedRow(peers_);
    if (row < 0 || static_cast<size_t>(row) >= peerKeys_.size()) return -1;
    return row;
}

int GuiApp::SelectedTransferIndex() const {
    const int row = SelectedRow(transfers_);
    if (row < 0 || static_cast<size_t>(row) >= transferMirror_.size()) return -1;
    return row;
}

void GuiApp::PickAndQueue() {
    const int peerIdx = SelectedPeerIndex();
    std::string peer;
    ui::Transport transport = ui::Transport::Tcp;
    if (peerIdx >= 0) {
        peer = peerKeys_[static_cast<size_t>(peerIdx)];
        transport = peerTransports_[static_cast<size_t>(peerIdx)];
    }

    std::vector<std::string> paths = PickFiles(hwnd_);
    if (paths.empty()) return;

    for (const auto& p : paths) {
        ui::TaskRow seed;
        seed.name = platform::Basename(p);
        seed.sourcePath = p;
        seed.peer = peer;
        seed.transport = transport;
        seed.total = platform::FileSize(p);
        // An empty `peer` is rejected by the model, which logs the reason. No
        // silent fallback to "whoever happens to be first in the list".
        model_->EnqueueSend(seed, TickMs());
    }
    WakeUi();
    SyncModel(true);
}

void GuiApp::StartSelected() {
    const int idx = SelectedTransferIndex();
    if (idx < 0) return;
    const TransferMirror m = transferMirror_[static_cast<size_t>(idx)];
    if (!m.isSend) return;  // inbound rows cannot be (re)started here

    ui::TaskId id = m.id;
    if (m.state == ui::TaskState::Queued) {
        // Launch the row the model already has.
    } else if (ui::IsTerminal(m.state)) {
        // Retry as a fresh task so the previous outcome stays visible.
        ui::TaskRow seed;
        seed.name = m.name;
        seed.sourcePath = m.sourcePath;
        seed.peer = m.peer;
        seed.transport = m.transport;
        seed.total = platform::FileSize(m.sourcePath);
        id = model_->EnqueueSend(seed, TickMs());
        if (id == 0) { SyncModel(true); return; }
    } else {
        return;  // already on the wire; use Cancel
    }

    ui::Snapshot s = model_->Take();
    for (const auto& r : s.sends) {
        if (r.id == id) {
            if (r.state == ui::TaskState::Queued) LaunchTask(r);
            break;
        }
    }
    SyncModel(true);
}

void GuiApp::LaunchTask(const ui::TaskRow& row) {
    auto model = model_;
    const ui::TaskId id = row.id;
    const std::string peer = row.peer;
    const std::string path = row.sourcePath;
    const std::string name = row.name;
    const ui::Transport transport = row.transport;
    const int64_t total = row.total;

    // TaskStarted is posted here, on the UI thread, before the worker starts:
    // the row has to read Running before the next click is processed, or two
    // clicks would put one TaskId on the wire twice.
    ui::UiEvent started;
    started.kind = ui::UiEvent::Kind::TaskStarted;
    started.id = id;
    started.name = name;
    started.path = path;
    started.size = total;
    started.atMs = TickMs();
    model->Post(std::move(started));
    model->Pump(TickMs());

    auto token = model->CancelToken(id);
    std::thread([model, id, peer, path, name, transport, token] {
        auto onProgress = [model, id](int64_t done, int64_t total) {
            model->PostProgress(id, done, total);
            WakeUi();
        };

        const std::atomic<bool>* rawToken = token ? token.get() : nullptr;
        std::string error;
        bool ok = false;
        std::string sha;
        if (transport == ui::Transport::Bluetooth) {
            auto res = bt::SendFile(peer, path, name, onProgress, true, rawToken);
            ok = res.ok;
            error = res.error;
            sha = res.senderHash;
        } else {
            auto res = tcp::SendFile(peer, tcp::kDefaultFilePort, path, name,
                                     onProgress, true, rawToken);
            ok = res.ok;
            error = res.error;
            sha = res.senderHash;
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
        WakeUi();
    }).detach();
}

void GuiApp::CancelSelected() {
    const int idx = SelectedTransferIndex();
    if (idx < 0) return;
    const ui::TaskId id = transferMirror_[static_cast<size_t>(idx)].id;
    ui::UiEvent ev;
    if (!model_->TryCancel(id)) {
        ev.kind = ui::UiEvent::Kind::Info;
        ev.detail = "nothing to cancel (already finished, or an inbound row)";
        model_->Post(std::move(ev));
    } else {
        ev.kind = ui::UiEvent::Kind::Info;
        ev.detail = "cancel requested; the transfer stops at the next chunk";
        model_->Post(std::move(ev));
    }
    SyncModel(true);
}

void GuiApp::ChooseReceiveDir() {
    std::string dir = PickFolder(hwnd_, receiveDir_);
    if (dir.empty() || dir == receiveDir_) return;
    receiveDir_ = dir;
    model_->SetReceiveDir(dir);

    ui::UiEvent ev;
    ev.kind = ui::UiEvent::Kind::Info;
    ev.detail = "receive folder -> " + dir;
    model_->Post(std::move(ev));

    // Rebind the listeners so new files land in the new folder.
    const bool tcpWas = ::SendMessageW(chkTcp_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool btWas = ::SendMessageW(chkBt_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (tcpWas) ToggleTcp(false);
    if (btWas) ToggleBt(false);
    if (tcpWas) ToggleTcp(true);
    if (btWas) ToggleBt(true);
    SyncModel(true);
}

void GuiApp::ToggleTcp(bool enable) {
    if (!enable) {
        if (!tcp_.IsRunning()) return;
        tcp_.Stop();
        ui::UiEvent ev;
        ev.kind = ui::UiEvent::Kind::TcpListenStopped;
        model_->Post(std::move(ev));
        SyncModel(true);
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
            WakeUi();
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
                model->PostRecvProgress(ui::Transport::Tcp, u.connId, u.done,
                                       u.total);
            }
            WakeUi();
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
    SyncModel(true);
}

void GuiApp::ToggleBt(bool enable) {
    if (!enable) {
        if (!bt_.IsRunning()) return;
        bt_.Stop();
        ui::UiEvent ev;
        ev.kind = ui::UiEvent::Kind::BtListenStopped;
        model_->Post(std::move(ev));
        SyncModel(true);
        return;
    }
    if (bt_.IsRunning()) return;
    if (!bt::IsSupported()) {
        ui::UiEvent ev;
        ev.kind = ui::UiEvent::Kind::BtListenFailed;
        ev.detail = "this build has no Bluetooth support";
        model_->Post(std::move(ev));
        SyncModel(true);
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
            WakeUi();
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
            WakeUi();
        });

    ui::UiEvent ev;
    ev.kind = ok ? ui::UiEvent::Kind::BtListenStarted
                 : ui::UiEvent::Kind::BtListenFailed;
    if (!ok) ev.detail = "cannot register the RFCOMM service (no adapter?)";
    model_->Post(std::move(ev));
    SyncModel(true);
}

void GuiApp::ScanBluetooth() {
    if (btScanning_->exchange(true)) return;  // one inquiry at a time

    auto model = model_;
    auto flag = btScanning_;
    auto shutdown = shutdown_;
    std::thread([model, flag, shutdown] {
        auto post = [model](ui::PeerKind kind, const bt::PeerDevice& d) {
            ui::UiEvent ev;
            ev.kind = ui::UiEvent::Kind::PeerSeen;
            ev.transport = ui::Transport::Bluetooth;
            ev.peerKind = kind;
            ev.key = d.address;   // MAC text: byte-exact, it is a map key
            ev.name = d.name.empty() ? d.address : d.name;
            model->Post(std::move(ev));
            WakeUi();
        };

        for (const auto& d : bt::EnumeratePairedDevices(0)) {
            if (shutdown->load()) return;
            post(ui::PeerKind::BluetoothPaired, d);
        }

        ui::UiEvent info;
        info.kind = ui::UiEvent::Kind::Info;
        info.detail = "Bluetooth inquiry: about 12 s...";
        model->Post(std::move(info));
        WakeUi();

        bt::DiscoverDevices([&post](const bt::PeerDevice& d) {
            post(ui::PeerKind::BluetoothInquiry, d);
        }, 12);

        ui::UiEvent finished;
        finished.kind = ui::UiEvent::Kind::Info;
        finished.detail = "Bluetooth inquiry finished";
        model->Post(std::move(finished));
        flag->store(false);
        WakeUi();
    }).detach();
    // The scan is intentionally not waited for on shutdown: DiscoverDevices
    // blocks ~12 s and cannot be canceled, and it only ever touches the
    // shared_ptr<UiModel> it captured, which stays valid on its own.
}

void GuiApp::StartDiscoveryLoop() {
    auto model = model_;
    auto shutdown = shutdown_;
    std::thread([model, shutdown] {
        discovery::Beacon beacon;
        if (beacon.Start()) {
            ui::UiEvent ev;
            ev.kind = ui::UiEvent::Kind::BeaconStarted;
            model->Post(std::move(ev));
            WakeUi();
        }
        while (!shutdown->load()) {
            for (const auto& p : discovery::Listen(3)) {
                if (shutdown->load()) break;
                ui::UiEvent ev;
                ev.kind = ui::UiEvent::Kind::PeerSeen;
                ev.transport = ui::Transport::Tcp;
                ev.peerKind = ui::PeerKind::Lan;
                ev.key = p.ip;     // IPv4 text: byte-exact map key
                ev.name = p.host;
                ev.atMs = TickMs();
                model->Post(std::move(ev));
            }
            WakeUi();
        }
        beacon.Stop();
        // `beacon` lives on this thread on purpose: GuiApp's own beacon_ member
        // would be destroyed with the window while this loop is still running.
    }).detach();
}

} // namespace gui
} // namespace wfd

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int APIENTRY WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCmd) {
    using wfd::gui::GuiApp;
    using wfd::gui::U8ToW;

    std::string dir = wfd::platform::DefaultReceiveDir();
    int port = wfd::tcp::kDefaultFilePort;

    int argc = 0;
    if (LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) {
            const std::string a = wfd::gui::WToU8(argv[i]);
            if (a == "--dir" && i + 1 < argc) dir = wfd::gui::WToU8(argv[++i]);
            else if (a == "--port" && i + 1 < argc)
                port = std::atoi(wfd::gui::WToU8(argv[++i]).c_str());
        }
        ::LocalFree(argv);
    }

    wfd::platform::InitNetworking();  // refcounted; the only call site here
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ::SetProcessDPIAware();

    // The manifest selects comctl32 v6; this registers the window classes.
    // Without it, CreateWindowExW("SysListView32") fails with
    // ERROR_CANNOT_FIND_WND_CLASS.
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    if (!::InitCommonControlsEx(&icc)) {
        ::CoUninitialize();
        return 1;
    }

    wfd::ui::Config cfg;
    cfg.maxLogRows = 200;
    cfg.peerStaleMs = 8000;
    cfg.peerTtlMs = 0;   // stale peers stay visible; the Kind column says so
    auto model = std::make_shared<wfd::ui::UiModel>(cfg);

    {
        GuiApp app(model);
        if (!app.Create(instance, showCmd, dir, port)) return 1;

        MSG msg{};
        while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
            // IsDialogMessage gives Tab/arrow/mnemonic navigation to the child
            // controls without needing a real dialog resource.
            if (::IsDialogMessageW(app.hwnd(), &msg)) continue;
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }

    ::CoUninitialize();
    wfd::platform::ShutdownNetworking();
    return 0;
}
