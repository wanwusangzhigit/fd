#include "Win32Util.h"

#ifndef UNICODE
#define UNICODE 1
#endif
#include <windows.h>

#include <commdlg.h>
#include <objbase.h>
#include <shlobj.h>

#include <cstring>

namespace wfd {
namespace gui {

std::wstring U8ToW(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                       static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                          &out[0], n);
    return out;
}

std::string WToU8(const wchar_t* w) {
    if (w == nullptr || *w == L'\0') return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr,
                                        nullptr);
    if (n <= 1) return std::string();
    std::string out(static_cast<size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], n, nullptr, nullptr);
    return out;
}

std::vector<std::string> PickFiles(HWND__* owner) {
    std::vector<std::string> result;

    // OFN_ALLOWMULTISELECT returns "<dir>\0name1\0name2\0\0" when more than one
    // file is chosen, or a single "<dir>\<file>\0\0" otherwise. 64 KB of buffer
    // covers a few hundred long names; OFN_EXPLORER is implicit in modern comctl.
    std::wstring buf(32768, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = reinterpret_cast<HWND>(owner);
    ofn.lpstrFilter = L"All Files\0*.*\0";
    ofn.lpstrFile = &buf[0];
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST |
                OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!::GetOpenFileNameW(&ofn)) return result;  // canceled or buffer too small

    // ofn.lpstrFile still points at `buf`, whose contents were modified in place.
    const wchar_t* p = buf.c_str();
    std::wstring first(p);
    p += first.size() + 1;
    if (*p == L'\0') {
        result.push_back(WToU8(first.c_str()));  // single selection: full path
        return result;
    }
    while (*p != L'\0') {
        std::wstring name(p);
        p += name.size() + 1;
        std::wstring full = first;
        if (!full.empty() && full.back() != L'\\') full += L'\\';
        full += name;
        result.push_back(WToU8(full.c_str()));
    }
    return result;
}

namespace {

std::wstring g_initialDir;

INT CALLBACK BrowseProc(HWND hwnd, UINT msg, LPARAM, LPARAM) {
    if (msg == BFFM_INITIALIZED && !g_initialDir.empty()) {
        ::SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE,
                       reinterpret_cast<LPARAM>(g_initialDir.c_str()));
    }
    return 0;
}

} // namespace

std::string PickFolder(HWND__* owner, const std::string& initial) {
    g_initialDir = U8ToW(initial);
    BROWSEINFOW bi{};
    bi.hwndOwner = reinterpret_cast<HWND>(owner);
    bi.lpszTitle = L"Choose the receive directory";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
    bi.lpfn = initial.empty() ? nullptr : &BrowseProc;

    LPITEMIDLIST pidl = ::SHBrowseForFolderW(&bi);
    if (pidl == nullptr) return std::string();
    wchar_t path[MAX_PATH * 8] = {0};
    const bool got = ::SHGetPathFromIDListW(pidl, path) != FALSE;
    ::CoTaskMemFree(pidl);
    return got ? WToU8(path) : std::string();
}

} // namespace gui
} // namespace wfd
