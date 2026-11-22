// Strings.h — the single source of truth for GUI-facing bilingual text.
//
// Pure data: no Windows/Qt headers, no conversion logic. Both front-ends
// consume the same Str keys and the same UTF-8 kEn/kZh tables:
//   * Win32  gui/I18n.cpp  -> wchar_t via U8ToW
//   * Qt6    linux/I18nQt.cpp -> QString via QString::fromUtf8
// Keep the two tables in exactly the same order as the Str enum.
#pragma once

#include <iterator>   // std::size

namespace wfd {
namespace gui {

enum class Lang { En, Zh };

// One key per localized text point in the two front-ends.
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
    // Status bar templates (use %ls/%d format specifiers)
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

// Parallel UTF-8 tables, indexed by static_cast<int>(Str).
inline constexpr const char* kEn[] = {
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

inline constexpr const char* kZh[] = {
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

} // namespace gui
} // namespace wfd
