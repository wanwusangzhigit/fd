// Progress.cpp — TTY-aware progress line.
#include "Progress.h"
#include "Platform.h"

#include <cstdio>
#include <cstring>
#include <sstream>

namespace wfd {
namespace progress {

namespace {

void PrintBar(const std::string& name, int64_t transferred, int64_t total) {
    using namespace wfd::util;
    int width = TerminalWidth();
    if (width < 30) width = 30;
    int pct = total > 0 ? static_cast<int>(transferred * 100 / total) : 0;
    pct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    std::string sizeInfo = HumanSize(transferred) + " / " + HumanSize(total);
    // Build the bar.
    int barSlots = std::min<int>(30, width / 4);
    if (barSlots < 5) barSlots = 5;
    int filled = static_cast<int>(barSlots * pct / 100);
    std::string bar = "[";
    for (int i = 0; i < barSlots; ++i) bar.push_back(i < filled ? '#' : ' ');
    bar += "]";
    // Truncate filename if needed.
    std::string nameStr = name;
    if (nameStr.size() > 24) nameStr = nameStr.substr(0, 21) + "...";
    char line[256];
    std::snprintf(line, sizeof(line), "%-24s %s %3d%% · %s",
                   nameStr.c_str(), bar.c_str(), pct, sizeInfo.c_str());
    std::fputs("\r", stdout);
    std::fputs(line, stdout);
    int lineLen = static_cast<int>(std::strlen(line));
    if (lineLen < width - 1) {
        for (int i = lineLen; i < width - 1; ++i) std::fputc(' ', stdout);
    }
    std::fflush(stdout);
}

} // namespace

std::function<void(int64_t, int64_t)> MakeReporter(const std::string& fileName) {
    if (!platform::IsTerminal()) {
        return [](int64_t, int64_t) {}; // no-op
    }
    return [fileName](int64_t transferred, int64_t total) {
        PrintBar(fileName, transferred, total);
        if (transferred >= total) {
            std::fputc('\n', stdout);
            std::fflush(stdout);
        }
    };
}

} // namespace progress
} // namespace wfd
