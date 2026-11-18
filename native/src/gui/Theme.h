// Theme.h — visual layer for the Win32 GUI.
//
// Owns every decision that is purely about looks: font, palette, row colors,
// the progress strip, and DPI. Kept out of GuiApp on purpose — GuiApp carries
// the event/transport/thread contract and should stay reviewable against the
// UiModel state machine. Nothing here touches UiModel or any transport.
//
// Only GDI/user32/comctl32 are used (already linked into wfd_gui), so the
// static single-file distribution and the CI DLL allowlist are unaffected.
#pragma once

#ifndef UNICODE
#define UNICODE 1
#endif
#include <windows.h>
#include <commctrl.h>

namespace wfd {
namespace gui {

// Row color role, ordered to match ui::TaskState (Queued..Canceled). GuiApp
// maps a TaskState to one of these without Theme needing to know UiModel.
enum class VisualState : int {
    Queued = 0,
    Running,
    Completed,
    Failed,
    Canceled,
};

// Opt the process into Per-Monitor DPI awareness v2 before any window exists.
// Resolves SetProcessDpiAwarenessContext dynamically (Win10+) and falls back
// to SetProcessDPIAware on older systems. user32 is already linked.
void EnablePerMonitorDpi();

// Effective DPI of a window (or the primary monitor when hwnd is null).
// GetDeviceCaps works on every supported Windows version and, after a
// WM_DPICHANGED, reflects the monitor the window moved to.
unsigned WindowDpi(HWND hwnd);

// Scale a logical pixel value to a DPI.
int ScalePx(HWND hwnd, int value);

class Theme {
public:
    Theme() = default;
    ~Theme();

    Theme(const Theme&) = delete;
    Theme& operator=(const Theme&) = delete;

    // (Re)create font and brushes for the given window's DPI. Called once from
    // WM_CREATE and again from WM_DPICHANGED.
    void Rebuild(HWND hwnd);

    HFONT Font() const { return font_; }

    // ITEMPREPAINT: color the row by state and alternate the background.
    // Selected rows are left alone so the system highlight stays visible.
    void ApplyRowColors(LPNMLVCUSTOMDRAW cd, VisualState state, int row) const;

    // ITEMPOSTPAINT | SUBITEM: draw a bordered two-tone progress strip.
    void DrawProgress(LPNMLVCUSTOMDRAW cd, int pct, HWND hwnd) const;

private:
    void Release();

    HFONT    font_ = nullptr;
    HBRUSH   barBack_ = nullptr;
    HBRUSH   barFill_ = nullptr;
    HBRUSH   barEdge_ = nullptr;
    COLORREF stateFg_[5] = {};
};

} // namespace gui
} // namespace wfd
