// I18nQt.h — Qt-side adapter over the shared Strings.h tables.
//
// The bilingual UTF-8 data (Str enum + kEn/kZh) lives in gui/Strings.h and is
// shared with the Win32 front-end. This adapter only maps it into Qt types:
// QString from UTF-8, QLocale for the system default, and an ini file next to
// the executable for persistence (same convention as the Win32 build).
#pragma once

#include <QString>
#include <QStringList>

#include "Strings.h"   // Lang / Str / kEn / kZh

namespace wfd {
namespace linuxgui {

// Re-export the shared declarations so linuxgui code can write them directly.
using wfd::gui::Str;
using wfd::gui::Lang;
using wfd::gui::kEn;
using wfd::gui::kZh;

class I18nQt {
public:
    static I18nQt& Instance();

    Lang lang() const { return lang_; }
    void SetLang(Lang l) { lang_ = l; }

    // Translate a key into the active language as UTF-8 -> QString.
    QString Get(Str s) const;

    // Translate a template whose placeholders are %ls / %d (printf-style,
    // from the shared tables) into a QString with %1/%2... filled by `args`.
    QString GetFmt(Str s, const QStringList& args) const;

    static Lang SystemDefault();
    void LoadFromIni();
    void SaveToIni() const;

private:
    I18nQt() = default;
    Lang lang_ = Lang::En;
};

} // namespace linuxgui
} // namespace wfd
