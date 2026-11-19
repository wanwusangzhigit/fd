// I18n.h — GUI-only bilingual (English / Simplified Chinese) strings.
//
// Covers ONLY static UI chrome: buttons, column headers, group boxes, window
// title, status bar, state/enum labels, direction tags and fixed detail words.
// Runtime data (log messages, ev.detail, transport errors) deliberately stays
// ASCII/UTF-8 as produced by UiModel and the transports — see GuiApp.cpp's
// FillLog and the error/detail paths. Nothing here changes wire bytes.
//
// Strings are stored as UTF-8 and converted through Win32Util::U8ToW at the
// Win32 boundary (the single UTF-8<->UTF-16 conversion point). That avoids
// wide-literal source-encoding differences between MinGW and MSVC.
#pragma once

#ifndef UNICODE
#define UNICODE 1
#endif
#include <windows.h>

#include <string>

namespace wfd {
namespace gui {

enum class Lang { En, Zh };

// One key per localized text point in GuiApp.cpp.
enum class Str {
    // Window / group boxes
    AppTitle,           // "wfd - file transfer"
    GroupPeers,
    GroupTransfers,
    GroupLog,
    // Buttons
    BtnPickFiles,
    BtnSendSelected,
    BtnCancel,
    BtnReceiveFolder,
    ChkTcpListen,
    ChkBtListen,
    BtnBtScan,
    // Transfer-table column headers
    ColDir, ColState, ColFile, ColPeer, ColProgress,
    ColTransferred, ColRate, ColEta, ColDetail,
    // Peers / Log column headers
    PeerName, PeerAddress, PeerKind, PeerSeen,
    LogTime, LogMessage,
    // Menu
    MenuLanguage, MenuChinese, MenuEnglish,
    // Status bar templates
    StatusConn,         // "TCP: %1   BT: %2   beacon: %3"
    StatusCounts,       // "peers: %1   running: %2"
    StatusReceive,      // "receive: %1"
    StatusOnPort,       // "on %1"
    StatusOn,           // "on"
    StatusOff,          // "off"
    // Window title template
    TitleTemplate,      // "wfd - %1 peers, %2 transfers, receive -> %3"
    // Task states
    StateWait, StateRun, StateOk, StateFail, StateCancel,
    // Peer kinds
    PeerLan, PeerBtPaired, PeerBtFound,
    // Peer staleness
    PeerStale, PeerLive,
    // Direction
    DirOut, DirIn,
    // Fixed detail words
    DetailSha256Prefix,     // "sha256 %1"
    DetailCanceling,        // "canceling... "
    DetailSha256Verified,   // "sha256 verified"
    DetailNoHashTrailer,    // "no hash trailer"

    Count
};

class I18n {
public:
    static I18n& Instance();

    Lang lang() const { return lang_; }
    void SetLang(Lang l) {
        if (lang_ == l) return;
        lang_ = l;
        for (bool& c : cached_) c = false;  // translations depend on language
    }

    // Returns a stable wide string (cached per key, valid for process life).
    const std::wstring& Get(Str s) const;

    // Convenience: Get(Str) as a raw pointer for the simple mapping helpers.
    const wchar_t* CStr(Str s) const { return Get(s).c_str(); }

    // Resolve the default language from the Windows UI language.
    static Lang SystemDefault();

    // Load/save the choice in <exe dir>/wfd_lang.ini ([wfd] lang=zh|en).
    void LoadFromIni();
    void SaveToIni() const;

private:
    I18n() = default;
    Lang lang_ = Lang::En;
    // One cache slot per key so two different Str's never invalidate each
    // other's c_str(). Filled lazily on first Get().
    mutable std::wstring cache_[static_cast<int>(Str::Count)];
    mutable bool cached_[static_cast<int>(Str::Count)] = {};
};

} // namespace gui
} // namespace wfd
