// Theme.cpp — see Theme.h. Pure GDI/user32/comctl32; no new DLL imports.
#include "Theme.h"

#include <algorithm>
#include <cstdint>

namespace wfd {
namespace gui {

namespace {

// DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == ((DPI_AWARENESS_CONTEXT)-4).
// Defined locally so building against an older SDK (MinGW defaults to the
// Vista target in CMakeLists) still compiles.
constexpr intptr_t kPerMonitorV2 = -4;

HFONT MakeFont(unsigned dpi) {
    // Segoe UI is on every Windows Vista+; DEFAULT_CHARSET lets it fall back
    // per-script so Chinese filenames still render.
    HFONT h = ::CreateFontW(
        -::MulDiv(9, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL,
        0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (h != nullptr) return h;

    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        return ::CreateFontIndirectW(&ncm.lfMessageFont);
    }
    return static_cast<HFONT>(::GetStockObject(SYSTEM_FONT));
}

} // namespace

void EnablePerMonitorDpi() {
    using Fn = BOOL (WINAPI*)(void*);
    void* ctx = reinterpret_cast<void*>(kPerMonitorV2);
    HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
        auto fn = reinterpret_cast<Fn>(
            ::GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (fn != nullptr && fn(ctx) != FALSE) return;
    }
    // Old Windows (pre Win10 1703): process-wide system awareness is the best
    // available, and keeps all physical-pixel math consistent.
    ::SetProcessDPIAware();
}

unsigned WindowDpi(HWND hwnd) {
    HWND target = hwnd != nullptr ? hwnd : nullptr;
    HDC dc = ::GetDC(target);
    if (dc == nullptr) return 96;
    const int dpi = ::GetDeviceCaps(dc, LOGPIXELSX);
    ::ReleaseDC(target, dc);
    return dpi > 0 ? static_cast<unsigned>(dpi) : 96u;
}

int ScalePx(HWND hwnd, int value) {
    return static_cast<int>(
        static_cast<float>(value) * WindowDpi(hwnd) / 96.0f + 0.5f);
}

Theme::~Theme() {
    Release();
}

void Theme::Release() {
    if (font_ != nullptr) { ::DeleteObject(font_); font_ = nullptr; }
    if (barBack_ != nullptr) { ::DeleteObject(barBack_); barBack_ = nullptr; }
    if (barFill_ != nullptr) { ::DeleteObject(barFill_); barFill_ = nullptr; }
    if (barEdge_ != nullptr) { ::DeleteObject(barEdge_); barEdge_ = nullptr; }
}

void Theme::Rebuild(HWND hwnd) {
    Release();
    const unsigned dpi = WindowDpi(hwnd);
    font_ = MakeFont(dpi);

    barBack_ = ::CreateSolidBrush(RGB(230, 230, 230));
    barFill_ = ::CreateSolidBrush(RGB(0, 120, 215));
    barEdge_ = ::CreateSolidBrush(RGB(150, 150, 150));

    // Readable on both light and dark system themes where possible: system
    // gray for queued, accent blue for running, green/red for the outcomes.
    stateFg_[static_cast<int>(VisualState::Queued)] =
        ::GetSysColor(COLOR_GRAYTEXT);
    stateFg_[static_cast<int>(VisualState::Running)] = RGB(0, 102, 204);
    stateFg_[static_cast<int>(VisualState::Completed)] = RGB(0, 128, 0);
    stateFg_[static_cast<int>(VisualState::Failed)] = RGB(190, 0, 0);
    stateFg_[static_cast<int>(VisualState::Canceled)] = RGB(110, 110, 110);
}

void Theme::ApplyRowColors(LPNMLVCUSTOMDRAW cd, VisualState state, int row) const {
    // Keep the system selection highlight on the active row.
    if ((cd->nmcd.uItemState & CDIS_SELECTED) != 0) return;

    const int s = static_cast<int>(state);
    if (s >= 0 && s < 5) cd->clrText = stateFg_[s];

    // Alternating rows give the eye a track across a wide table; terminal
    // outcomes keep a faint tint so they read at a glance.
    if (state == VisualState::Completed) {
        cd->clrTextBk = RGB(235, 248, 235);
    } else if (state == VisualState::Failed) {
        cd->clrTextBk = RGB(252, 235, 235);
    } else {
        cd->clrTextBk = (row & 1) ? ::GetSysColor(COLOR_WINDOW)
                                  : RGB(245, 248, 252);
    }
}

void Theme::DrawProgress(LPNMLVCUSTOMDRAW cd, int pct, HWND hwnd) const {
    const HWND list = cd->nmcd.hdr.hwndFrom;
    RECT rc{};
    rc.left = static_cast<LONG>(cd->iSubItem);
    rc.top = LVIR_BOUNDS;
    if (::SendMessageW(list, LVM_GETSUBITEMRECT, cd->nmcd.dwItemSpec,
                       reinterpret_cast<LPARAM>(&rc)) == 0) {
        return;
    }
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;

    const int pad = ScalePx(hwnd, 4);
    const int barH = ScalePx(hwnd, 8);
    RECT bar = rc;
    bar.left += pad;
    bar.right -= pad;
    bar.top = rc.bottom - barH - ScalePx(hwnd, 2);
    bar.bottom = rc.bottom - ScalePx(hwnd, 2);
    if (bar.right <= bar.left || bar.bottom <= bar.top) return;

    ::FrameRect(cd->nmcd.hdc, &bar, barEdge_);

    RECT inner = bar;
    ::InflateRect(&inner, -1, -1);
    if (inner.right <= inner.left || inner.bottom <= inner.top) return;
    ::FillRect(cd->nmcd.hdc, &inner, barBack_);

    pct = std::max(0, std::min(100, pct));
    if (pct > 0) {
        RECT fill = inner;
        fill.right = fill.left + (inner.right - inner.left) * pct / 100;
        ::FillRect(cd->nmcd.hdc, &fill, barFill_);
    }
}

} // namespace gui
} // namespace wfd
