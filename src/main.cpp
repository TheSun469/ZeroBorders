#include "gui/MainWindow.h"
#include "core/Log.h"

#include <QApplication>
#include <QIcon>
#include <QStyleFactory>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#pragma comment(lib, "ws2_32.lib")

namespace {

// RAII wrapper for Winsock init/cleanup.
struct WinsockGuard {
    bool ok = false;
    WinsockGuard() {
        WSADATA wsa{};
        ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
        if (!ok) {
            ZB_LOG_ERROR("WSAStartup failed");
        }
    }
    ~WinsockGuard() {
        if (ok) WSACleanup();
    }
};

} // namespace

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);

    WinsockGuard wsa;
    if (!wsa.ok) return 1;

    QApplication app(argc, argv);
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    QApplication::setApplicationName("ZeroBorders");
    QApplication::setOrganizationName("ZeroBorders");
    QApplication::setWindowIcon(QIcon(":/icons/app.png"));

    zb::MainWindow window;
    window.show();

    return QApplication::exec();
}
