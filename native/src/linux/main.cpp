// main.cpp — Qt6 desktop client entry point.
#include "I18nQt.h"
#include "LinuxGuiApp.h"
#include "Platform.h"
#include "TcpTransport.h"
#include "UiModel.h"

#include <QApplication>

#include <cstdlib>
#include <filesystem>
#include <string>

int main(int argc, char** argv) {
    std::string dir = wfd::platform::DefaultReceiveDir();
    int port = wfd::tcp::kDefaultFilePort;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--dir" && i + 1 < argc) {
            dir = argv[++i];
        } else if (a == "--port" && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        }
    }

    // Best-effort: DefaultReceiveDir() creates itself, but a --dir passed by
    // the user may not exist. Creating it avoids a confusing "recv failed"
    // for every inbound file. Ignore errors (e.g. read-only path) — the
    // transfer layer will still report them.
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    wfd::platform::InitNetworking();

    // Resolve language before the window is built (ini wins, else system UI).
    wfd::linuxgui::I18nQt::Instance().LoadFromIni();

    QApplication app(argc, argv);

    wfd::ui::Config cfg;
    cfg.maxLogRows = 200;
    cfg.peerStaleMs = 8000;
    cfg.peerTtlMs = 0;   // stale peers stay visible
    auto model = std::make_shared<wfd::ui::UiModel>(cfg);

    wfd::linuxgui::LinuxGuiApp window(model, dir, port);
    window.show();

    int rc = app.exec();
    wfd::platform::ShutdownNetworking();
    return rc;
}
