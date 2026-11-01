// Win32Util.h — the only place this app converts between UTF-8 and UTF-16.
//
// Everything inside the app (protocol names, hashes, peer keys, file paths as
// we hand them to wfd_core) is UTF-8 `std::string`. Wide strings exist only
// for Win32 window text, list-view items and dialog results.
//
// `MultiByteToWideChar`/`WideCharToMultiByte` are used instead of `wcstombs`
// because the latter depends on the process locale, which a GUI process inherits
// as the ANSI codepage — that mangles Chinese file names. `MB_ERR_INVALID_CHARS`
// is deliberately not set: bad input degrades to U+FFFD rather than failing the
// whole transfer.
//
// Never route these through protocol-visible bytes: the wire `forcedName` is
// written verbatim and capped at 4096 bytes, and IPv4 text / BT MAC / SHA-256
// hex are used as map keys — a round-trip through UTF-16 would silently break
// both interop and de-duplication.
#pragma once

#include <string>
#include <vector>

struct HWND__;

namespace wfd {
namespace gui {

std::wstring U8ToW(const std::string& s);
std::string WToU8(const wchar_t* w);

// Multi-select file picker. Returns UTF-8 absolute paths, empty if canceled.
std::vector<std::string> PickFiles(HWND__* owner);

// Folder picker seeded with `initial`. Returns a UTF-8 path, empty if canceled.
std::string PickFolder(HWND__* owner, const std::string& initial);

} // namespace gui
} // namespace wfd
