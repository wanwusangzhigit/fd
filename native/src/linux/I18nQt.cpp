// I18nQt.cpp — see I18nQt.h.
#include "I18nQt.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QSettings>

namespace wfd {
namespace linuxgui {

namespace {

QString IniPath() {
    const QString exe = QCoreApplication::applicationFilePath();
    if (exe.isEmpty()) return QString();
    return QFileInfo(exe).absolutePath() + QLatin1String("/wfd_lang.ini");
}

} // namespace

I18nQt& I18nQt::Instance() {
    static I18nQt instance;
    return instance;
}

QString I18nQt::Get(Str s) const {
    const int i = static_cast<int>(s);
    if (i < 0 || i >= static_cast<int>(Str::Count)) return QString();
    const char* src = (lang_ == Lang::Zh) ? kZh[i] : kEn[i];
    return QString::fromUtf8(src);
}

QString I18nQt::GetFmt(Str s, const QStringList& args) const {
    // The shared tables use printf placeholders (%ls / %d). Replace each with
    // the next argument manually rather than going through %1, so arguments
    // that themselves contain '%' cannot be mistaken for placeholders.
    const QString tpl = Get(s);
    QString out;
    out.reserve(tpl.size());
    int arg = 0;
    for (int i = 0; i < tpl.size(); ++i) {
        const QChar c = tpl.at(i);
        if (c == QLatin1Char('%') && i + 1 < tpl.size()) {
            const QChar n = tpl.at(i + 1);
            if (n == QLatin1Char('l') && i + 2 < tpl.size() &&
                tpl.at(i + 2) == QLatin1Char('s')) {
                out += (arg < args.size()) ? args.at(arg) : QString::fromLatin1("%ls");
                ++arg;
                i += 2;
                continue;
            }
            if (n == QLatin1Char('d')) {
                out += (arg < args.size()) ? args.at(arg) : QString::fromLatin1("%d");
                ++arg;
                i += 1;
                continue;
            }
        }
        out += c;
    }
    return out;
}

Lang I18nQt::SystemDefault() {
    return QLocale::system().language() == QLocale::Chinese ? Lang::Zh : Lang::En;
}

void I18nQt::LoadFromIni() {
    const QString path = IniPath();
    if (path.isEmpty()) { lang_ = SystemDefault(); return; }
    QSettings ini(path, QSettings::IniFormat);
    const QString v = ini.value(QStringLiteral("wfd/lang"), QString()).toString();
    if (v.compare(QLatin1String("zh"), Qt::CaseInsensitive) == 0) {
        lang_ = Lang::Zh;
    } else if (v.compare(QLatin1String("en"), Qt::CaseInsensitive) == 0) {
        lang_ = Lang::En;
    } else {
        lang_ = SystemDefault();
    }
}

void I18nQt::SaveToIni() const {
    const QString path = IniPath();
    if (path.isEmpty()) return;
    QSettings ini(path, QSettings::IniFormat);
    ini.setValue(QStringLiteral("wfd/lang"),
                 lang_ == Lang::Zh ? QLatin1String("zh") : QLatin1String("en"));
    ini.sync();
}

} // namespace linuxgui
} // namespace wfd
