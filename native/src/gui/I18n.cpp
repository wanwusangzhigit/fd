// I18n.cpp — see I18n.h. UTF-8 tables converted via Win32Util::U8ToW.
#include "I18n.h"
#include "Win32Util.h"

#include <array>
#include <cwchar>

namespace wfd {
namespace gui {

namespace {

// Parallel UTF-8 tables, indexed by static_cast<int>(Str). Keep in the same
// order as the Str enum; kCount guards against drift.
constexpr const char* kEn[] = {
    "wfd - file transfer",
    "Peers", "Transfers", "Log",
    "&Pick files...", "&Send selected", "Ca&ncel", "Receive &folder...",
    "&TCP listen", "&Bluetooth listen", "Bluetooth &scan",
    "Dir", "State", "File", "Peer", "Progress", "Transferred", "Rate", "ETA", "Detail",
    "Name", "Address", "Kind", "Seen",
    "Time", "Message",
    "Language", "中文", "English",
    "TCP: %ls   BT: %ls   beacon: %ls",
    "peers: %d   running: %d",
    "receive: %ls",
    "on %d", "on", "off",
    "wfd - %d peers, %d transfers, receive -> %ls",
    "WAIT", "RUN", "OK", "FAIL", "CNCL",
    "LAN", "BT paired", "BT found",
    "stale", "live",
    "out", "in",
    "sha256 %ls", "canceling... ", "sha256 verified", "no hash trailer",
};

constexpr const char* kZh[] = {
    "wfd - 文件传输",
    "设备", "传输列表", "日志",
    "选择文件(&P)...", "发送所选(&S)", "取消(&N)", "接收文件夹(&F)...",
    "TCP 监听(&T)", "蓝牙监听(&B)", "蓝牙扫描(&S)",
    "方向", "状态", "文件", "对端", "进度", "已传输", "速率", "剩余", "详情",
    "名称", "地址", "类型", "最近",
    "时间", "消息",
    "语言", "中文", "English",
    "TCP：%ls   BT：%ls   广播：%ls",
    "设备：%d   传输中：%d",
    "接收目录：%ls",
    "端口 %d", "开启", "关闭",
    "wfd - %d 个设备，%d 个传输，接收 -> %ls",
    "等待", "传输中", "完成", "失败", "已取消",
    "局域网", "蓝牙已配对", "蓝牙发现",
    "过期", "在线",
    "发送", "接收",
    "sha256 %ls", "取消中... ", "sha256 已校验", "无校验 trailer",
};

static_assert(std::size(kEn) == static_cast<size_t>(Str::Count),
              "kEn table size must match Str::Count");
static_assert(std::size(kZh) == static_cast<size_t>(Str::Count),
              "kZh table size must match Str::Count");

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
