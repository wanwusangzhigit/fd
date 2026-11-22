// I18n.cpp — see I18n.h. UTF-8 tables (Strings.h) converted via U8ToW.
#include "I18n.h"
#include "Strings.h"
#include "Win32Util.h"

#include <cwchar>

namespace wfd {
namespace gui {

namespace {

std::wstring IniPath() {
    wchar_t exe[MAX_PATH] = {0};
    DWORD n = ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir;
    if (n > 0 && n < MAX_PATH) {
        std::wstring full(exe, n);
        size_t slash = full.find_last_of(L"\\/");
        dir = (slash == std::wstring::npos) ? std::wstring()
                                            : full.substr(0, slash + 1);
    }
    return dir + L"wfd_lang.ini";
}

} // namespace

I18n& I18n::Instance() {
    static I18n instance;
    return instance;
}

const std::wstring& I18n::Get(Str s) const {
    const int i = static_cast<int>(s);
    if (i < 0 || i >= static_cast<int>(Str::Count)) {
        static const std::wstring empty;
        return empty;
    }
    if (!cached_[i]) {
        const char* src = (lang_ == Lang::Zh) ? kZh[i] : kEn[i];
        cache_[i] = U8ToW(src);
        cached_[i] = true;
    }
    return cache_[i];
}

Lang I18n::SystemDefault() {
    LANGID id = ::GetUserDefaultUILanguage();
    return PRIMARYLANGID(id) == LANG_CHINESE ? Lang::Zh : Lang::En;
}

void I18n::LoadFromIni() {
    const std::wstring path = IniPath();
    if (path.empty()) { lang_ = SystemDefault(); return; }
    wchar_t buf[16] = {0};
    ::GetPrivateProfileStringW(L"wfd", L"lang", L"", buf, 16, path.c_str());
    if (buf[0] == L'z' || buf[0] == L'Z') {
        lang_ = Lang::Zh;
    } else if (buf[0] == L'e' || buf[0] == L'E') {
        lang_ = Lang::En;
    } else {
        lang_ = SystemDefault();
    }
    // Translations depend on language; invalidate all cache slots.
    for (int i = 0; i < static_cast<int>(Str::Count); ++i) cached_[i] = false;
}

void I18n::SaveToIni() const {
    const std::wstring path = IniPath();
    if (path.empty()) return;
    ::WritePrivateProfileStringW(L"wfd", L"lang",
                                 lang_ == Lang::Zh ? L"zh" : L"en",
                                 path.c_str());
}

} // namespace gui
} // namespace wfd
