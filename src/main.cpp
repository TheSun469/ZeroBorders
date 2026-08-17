#include "gui/MainWindow.h"
#include "core/Log.h"

#include <QApplication>
#include <QBuffer>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QStyleFactory>
#include <QIcon>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <dbghelp.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "dbghelp.lib")
#else
#include <execinfo.h>  // backtrace
#include <unistd.h>    // readlink, getpid
#include <dlfcn.h>     // dladdr
#endif

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <system_error>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
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
#else
// Linux 不需要 Winsock 初始化。
struct WinsockGuard {
    bool ok = true;
};
#endif

// Returns the directory containing the running executable (UTF-8).
std::string getExeDirectory() {
#ifdef _WIN32
    wchar_t path[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return ".";
    std::wstring wpath(path, len);
    // Strip the executable file name, keep the directory.
    size_t slash = wpath.find_last_of(L"\\/");
    if (slash != std::wstring::npos) wpath.resize(slash);
    // Convert to UTF-8.
    int n = WideCharToMultiByte(CP_UTF8, 0, wpath.data(),
                                static_cast<int>(wpath.size()),
                                nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wpath.data(),
                        static_cast<int>(wpath.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
#else
    char path[4096];
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len <= 0) return ".";
    path[len] = '\0';
    std::string spath(path);
    size_t slash = spath.find_last_of('/');
    if (slash != std::string::npos) spath.resize(slash);
    return spath;
#endif
}

std::string formatTimestamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

#ifdef _WIN32
// Format a stack backtrace using StackWalk64 + SymFromAddr when symbols are
// available. Returns a multi-line string. Best-effort: if dbghelp cannot
// resolve a frame, the raw address is recorded.
std::string captureBacktrace(CONTEXT* ctx, HANDLE thread) {
    std::string out;
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_INCLUDE_32BIT_MODULES |
                  SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);

    HANDLE proc = GetCurrentProcess();
    SymInitialize(proc, nullptr, TRUE);

    STACKFRAME64 sf{};
    sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrStack.Mode = AddrModeFlat;
    sf.AddrFrame.Mode = AddrModeFlat;
#if defined(_M_X64) || defined(__x86_64__)
    sf.AddrPC.Offset = ctx->Rip;
    sf.AddrStack.Offset = ctx->Rsp;
    sf.AddrFrame.Offset = ctx->Rbp;
#else
    sf.AddrPC.Offset = ctx->Eip;
    sf.AddrStack.Offset = ctx->Esp;
    sf.AddrFrame.Offset = ctx->Ebp;
#endif

    const int kMaxFrames = 32;
    char symbolBuffer[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;

    for (int i = 0; i < kMaxFrames; ++i) {
        if (!StackWalk64(
#if defined(_M_X64) || defined(__x86_64__)
                IMAGE_FILE_MACHINE_AMD64,
#else
                IMAGE_FILE_MACHINE_I386,
#endif
                proc, thread, &sf, ctx, nullptr,
                SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
            break;
        }
        if (sf.AddrPC.Offset == 0) break;

        char line[512];
        DWORD64 addr = sf.AddrPC.Offset;
        DWORD64 disp = 0;
        bool hasSym = SymFromAddr(proc, addr, &disp, sym) != FALSE;

        IMAGEHLP_MODULE64 mod{};
        mod.SizeOfStruct = sizeof(mod);
        const char* modName = "?";
        if (SymGetModuleInfo64(proc, sf.AddrPC.Offset, &mod)) {
            modName = mod.ModuleName;
        }

        if (hasSym) {
            std::snprintf(line, sizeof(line),
                          "  #%02d %s!%s+0x%llx [0x%llx]\n",
                          i, modName, sym->Name,
                          static_cast<unsigned long long>(disp),
                          static_cast<unsigned long long>(addr));
        } else {
            std::snprintf(line, sizeof(line),
                          "  #%02d %s!0x%llx\n",
                          i, modName,
                          static_cast<unsigned long long>(addr));
        }
        out += line;
    }
    SymCleanup(proc);
    return out;
}
#else
// Linux: 使用 execinfo.h 的 backtrace()/backtrace_symbols()。
std::string captureBacktrace() {
    void* buffer[32];
    int n = backtrace(buffer, 32);
    char** symbols = backtrace_symbols(buffer, n);
    std::string out;
    if (symbols) {
        for (int i = 0; i < n; ++i) {
            out += "  #";
            out += std::to_string(i);
            out += " ";
            out += symbols[i];
            out += "\n";
        }
        free(symbols);
    }
    return out;
}
#endif

// Writes a crash report to today's log file. Kept minimal because the
// process state may be corrupted at this point.
#ifdef _WIN32
void writeCrashReport(const char* title, DWORD code, void* address,
                      CONTEXT* ctx) {
    std::string report;
    report += "\n";
    report += "========================================\n";
    report += "!!! APPLICATION CRASH !!!\n";
    report += "========================================\n";
    char header[512];
    std::snprintf(header, sizeof(header),
                  "Time: %s\nTitle: %s\nException code: 0x%08lX\nAddress: 0x%p\nThread ID: %lu\n",
                  formatTimestamp().c_str(), title, code, address,
                  GetCurrentThreadId());
    report += header;
    report += "Backtrace:\n";
    if (ctx) {
        HANDLE thread = GetCurrentThread();
        report += captureBacktrace(ctx, thread);
    } else {
        report += "  (no context available)\n";
    }
    report += "========================================\n";

    zb::log::writeToCrashLog(zb::log::activeLogDir(), report);

    // Also emit to OutputDebugString so it shows up in a debugger.
    OutputDebugStringA(report.c_str());
}
#else
void writeCrashReport(const char* title, int sig) {
    std::string report;
    report += "\n";
    report += "========================================\n";
    report += "!!! APPLICATION CRASH !!!\n";
    report += "========================================\n";
    char header[512];
    std::snprintf(header, sizeof(header),
                  "Time: %s\nTitle: %s\nSignal: %d\nPID: %d\n",
                  formatTimestamp().c_str(), title, sig, getpid());
    report += header;
    report += "Backtrace:\n";
    report += captureBacktrace();
    report += "========================================\n";

    zb::log::writeToCrashLog(zb::log::activeLogDir(), report);

    // Linux: 输出到 stderr。
    fputs(report.c_str(), stderr);
}
#endif

#ifdef _WIN32
LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* ep) {
    if (ep) {
        writeCrashReport("Unhandled Exception",
                         ep->ExceptionRecord->ExceptionCode,
                         ep->ExceptionRecord->ExceptionAddress,
                         ep->ContextRecord);
    } else {
        writeCrashReport("Unhandled Exception", 0, nullptr, nullptr);
    }
    // Let the default handler terminate so the OS error reporting still runs.
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

void signalHandler(int sig) {
    const char* name = "unknown";
    switch (sig) {
        case SIGABRT: name = "SIGABRT (abort)"; break;
        case SIGFPE:  name = "SIGFPE (floating-point)"; break;
        case SIGILL:  name = "SIGILL (illegal instruction)"; break;
        case SIGSEGV: name = "SIGSEGV (segmentation fault)"; break;
        case SIGTERM: name = "SIGTERM (terminate)"; break;
        default: break;
    }
#ifdef _WIN32
    writeCrashReport(name, static_cast<DWORD>(sig), nullptr, nullptr);
#else
    writeCrashReport(name, sig);
#endif
    std::_Exit(1);
}

#ifdef _WIN32
void __cdecl invalidParameterHandler(const wchar_t* /*expression*/,
                                     const wchar_t* /*function*/,
                                     const wchar_t* /*file*/,
                                     unsigned int /*line*/,
                                     uintptr_t /*reserved*/) {
    writeCrashReport("CRT Invalid Parameter", 0, nullptr, nullptr);
    std::_Exit(1);
}

void __cdecl pureCallHandler() {
    writeCrashReport("Pure Virtual Function Call", 0, nullptr, nullptr);
    std::_Exit(1);
}
#endif

// Installs all crash handlers and enables file logging. Destruction restores
// the previous handlers (mainly for cleanliness; in practice the process is
// terminating anyway).
struct CrashGuard {
#ifdef _WIN32
    LPTOP_LEVEL_EXCEPTION_FILTER prevFilter_ = nullptr;
    _invalid_parameter_handler prevInvalid_ = nullptr;
    _purecall_handler prevPure_ = nullptr;
#endif

    CrashGuard() {
#ifdef _WIN32
        prevFilter_ = SetUnhandledExceptionFilter(unhandledExceptionFilter);
        prevInvalid_ = _set_invalid_parameter_handler(invalidParameterHandler);
        prevPure_ = _set_purecall_handler(pureCallHandler);
#endif
        std::signal(SIGABRT, signalHandler);
        std::signal(SIGFPE, signalHandler);
        std::signal(SIGILL, signalHandler);
        std::signal(SIGSEGV, signalHandler);
        std::signal(SIGTERM, signalHandler);
        // Catch C++ uncaught exceptions as a last resort.
        std::set_terminate([]() {
            try {
                std::rethrow_exception(std::current_exception());
            } catch (const std::exception& e) {
                std::string msg = "Uncaught C++ exception: ";
                msg += e.what();
                msg += "\n";
                zb::log::writeToCrashLog(zb::log::activeLogDir(), msg);
            } catch (...) {
                zb::log::writeToCrashLog(zb::log::activeLogDir(),
                                         "Uncaught C++ exception (unknown type)\n");
            }
#ifdef _WIN32
            writeCrashReport("Uncaught C++ Exception", 0, nullptr, nullptr);
#else
            writeCrashReport("Uncaught C++ Exception", 0);
#endif
            std::_Exit(1);
        });

        // Enable file logging to <exe-dir>/log.
        std::string logDir = getExeDirectory();
#ifdef _WIN32
        logDir += "\\log";
#else
        logDir += "/log";
#endif
        zb::log::addFileSink(logDir);
    }

    ~CrashGuard() {
        // Flush all sinks before the process exits so the last log lines
        // are durable on disk.
        zb::log::removeAllSinks();
    }
};

// 运行时绘制一个向下的 chevron 箭头，返回 PNG data URI 供 QSS 使用。
// 避免使用 SVG（项目未链接 Qt SVG 模块）。
static QString makeChevronArrowUri() {
    QPixmap pix(12, 12);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(QColor("#605e5c"), 1.6);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    QPainterPath path;
    path.moveTo(3, 4.5);
    path.lineTo(6, 8);
    path.lineTo(9, 4.5);
    p.drawPath(path);
    p.end();
    QByteArray ba;
    QBuffer buf(&ba);
    buf.open(QIODevice::WriteOnly);
    pix.save(&buf, "PNG");
    return QStringLiteral("data:image/png;base64,") + QString::fromLatin1(ba.toBase64());
}

// Word 2019 浅色风格全局样式表
QString wordLightStyleSheet(const QString& arrowUri) {
    return QStringLiteral(R"(
        QWidget {
            color: #323130;
            font-family: "Segoe UI", "Microsoft YaHei UI", "Microsoft YaHei", sans-serif;
            font-size: 9pt;
        }
        QMainWindow, QDialog {
            background-color: #f3f2f1;
        }

        /* ---- GroupBox: 白色卡片 ---- */
        QGroupBox {
            background-color: #ffffff;
            border: 1px solid #e1dfdd;
            border-radius: 4px;
            margin-top: 10px;
            padding-top: 8px;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            subcontrol-position: top left;
            left: 10px;
            top: 2px;
            padding: 0 4px;
            color: #3b3a39;
            font-weight: bold;
        }

        /* ---- 按钮 ---- */
        QPushButton {
            background-color: #ffffff;
            border: 1px solid #c8c6c4;
            border-radius: 4px;
            padding: 4px 14px;
            min-height: 22px;
            color: #323130;
        }
        QPushButton:hover {
            background-color: #f3f2f1;
            border-color: #0078d4;
        }
        QPushButton:pressed {
            background-color: #e1dfdd;
            border-color: #0078d4;
        }
        QPushButton:disabled {
            color: #a19f9d;
            background-color: #f3f2f1;
            border-color: #e1dfdd;
        }
        QPushButton#startBtn_ {
            background-color: #0078d4;
            border: 1px solid #0078d4;
            color: #ffffff;
            font-weight: bold;
        }
        QPushButton#startBtn_:hover {
            background-color: #106ebe;
        }
        QPushButton#startBtn_:pressed {
            background-color: #005a9e;
        }

        /* ---- 输入框 / 下拉框 ---- */
        QLineEdit, QComboBox {
            background-color: #ffffff;
            border: 1px solid #c8c6c4;
            border-radius: 4px;
            padding: 3px 6px;
            selection-background-color: #0078d4;
            selection-color: #ffffff;
        }
        QLineEdit:hover, QComboBox:hover {
            border-color: #0078d4;
        }
        QLineEdit:focus, QComboBox:focus {
            border-color: #0078d4;
        }
        QLineEdit:disabled, QComboBox:disabled {
            background-color: #f3f2f1;
            color: #a19f9d;
        }
        QComboBox::drop-down {
            border: none;
            width: 20px;
        }
        QComboBox::down-arrow {
            image: url(%1);
            width: 12px;
            height: 12px;
        }
        QComboBox::down-arrow:on {
            top: 1px;
        }
        QComboBox QAbstractItemView {
            background-color: #ffffff;
            border: 1px solid #e1dfdd;
            selection-background-color: #cfe4f5;
            selection-color: #323130;
            outline: none;
        }

        /* ---- 表格 ---- */
        QTableView {
            background-color: #ffffff;
            border: 1px solid #e1dfdd;
            border-radius: 3px;
            gridline-color: #f3f2f1;
            selection-background-color: #cfe4f5;
            selection-color: #323130;
            alternate-background-color: #faf9f8;
        }
        QTableView::item {
            padding: 2px 4px;
        }
        QTableView::item:selected {
            background-color: #cfe4f5;
        }
        QHeaderView::section {
            background-color: #f3f2f1;
            color: #3b3a39;
            border: none;
            border-right: 1px solid #e1dfdd;
            border-bottom: 1px solid #e1dfdd;
            padding: 4px 6px;
            font-weight: bold;
        }
        QHeaderView::section:hover {
            background-color: #e1dfdd;
        }

        /* ---- 复选框 ---- */
        QCheckBox {
            spacing: 6px;
        }
        QCheckBox::indicator {
            width: 14px;
            height: 14px;
            border: 1px solid #c8c6c4;
            border-radius: 3px;
            background-color: #ffffff;
        }
        QCheckBox::indicator:hover {
            border-color: #0078d4;
        }
        QCheckBox::indicator:checked {
            background-color: #0078d4;
            border-color: #0078d4;
            image: none;
        }

        /* ---- 进度条 ---- */
        QProgressBar {
            background-color: #f3f2f1;
            border: 1px solid #e1dfdd;
            border-radius: 3px;
            text-align: center;
            color: #323130;
            height: 16px;
        }
        QProgressBar::chunk {
            background-color: #0078d4;
            border-radius: 2px;
        }

        /* ---- 日志文本 ---- */
        QPlainTextEdit, QTextEdit {
            background-color: #ffffff;
            border: 1px solid #e1dfdd;
            border-radius: 3px;
            selection-background-color: #cfe4f5;
        }

        /* ---- 标签 ---- */
        QLabel {
            background: transparent;
        }

        /* ---- 分割器 ---- */
        QSplitter::handle {
            background-color: #e1dfdd;
        }
        QSplitter::handle:hover {
            background-color: #0078d4;
        }
        QSplitter::handle:horizontal {
            width: 3px;
        }
        QSplitter::handle:vertical {
            height: 3px;
        }

        /* ---- 滚动条 ---- */
        QScrollBar:vertical {
            background: transparent;
            width: 12px;
            margin: 0;
        }
        QScrollBar::handle:vertical {
            background: #c8c6c4;
            min-height: 30px;
            border-radius: 4px;
            margin: 2px;
        }
        QScrollBar::handle:vertical:hover {
            background: #0078d4;
        }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
            height: 0;
        }
        QScrollBar:horizontal {
            background: transparent;
            height: 12px;
            margin: 0;
        }
        QScrollBar::handle:horizontal {
            background: #c8c6c4;
            min-width: 30px;
            border-radius: 4px;
            margin: 2px;
        }
        QScrollBar::handle:horizontal:hover {
            background: #0078d4;
        }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {
            width: 0;
        }

        /* ---- 菜单 ---- */
        QMenu {
            background-color: #ffffff;
            border: 1px solid #e1dfdd;
            border-radius: 4px;
            padding: 4px 0;
        }
        QMenu::item {
            padding: 6px 24px;
            border-radius: 0;
        }
        QMenu::item:selected {
            background-color: #cfe4f5;
        }
        QMenu::separator {
            height: 1px;
            background: #e1dfdd;
            margin: 4px 8px;
        }

        /* ---- 工具提示 ---- */
        QToolTip {
            background-color: #ffffff;
            color: #323130;
            border: 1px solid #e1dfdd;
            border-radius: 3px;
            padding: 4px 6px;
        }

        /* ---- 工具按钮（折叠箭头） ---- */
        QToolButton {
            background: transparent;
            border: none;
            border-radius: 3px;
            padding: 2px;
        }
        QToolButton:hover {
            background-color: #f3f2f1;
        }
        QToolButton:pressed {
            background-color: #e1dfdd;
        }
    )").arg(arrowUri);
}

} // namespace

// 单实例锁：用命名互斥量确保同一台电脑只运行一个 ZeroBorders 进程。
// 如果已有实例运行，尝试把已有窗口提到前台后退出。
bool checkSingleInstance() {
#ifdef _WIN32
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"ZeroBorders_SingleInstance_Mutex");
    if (hMutex == nullptr) return false;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hMutex);
        // 尝试找到已运行的主窗口并激活
        HWND hwnd = FindWindowW(nullptr, L"零界 ZeroBorders - 局域网键鼠共享");
        if (hwnd) {
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
        }
        return false;
    }
    return true;
#else
    // Linux: 跳过单实例检查。可使用 flock 锁文件或 QLocalServer/QLocalSocket 实现。
    return true;
#endif
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    // 单实例检查：仅 Release 模式启用。Debug 模式允许启动多个实例
    // 方便开发时同时运行两个程序测试双机通信。
    // MSVC: Debug 不定义 NDEBUG，Release 定义 NDEBUG。
#ifdef NDEBUG
    if (!checkSingleInstance()) {
        return 0;
    }
#endif

    // Install crash handlers and enable file logging as early as possible.
    CrashGuard crashGuard;

    WinsockGuard wsa;
    if (!wsa.ok) return 1;

    QApplication app(argc, argv);
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    QApplication::setApplicationName("ZeroBorders");
    QApplication::setOrganizationName("ZeroBorders");
    QApplication::setWindowIcon(QIcon(":/icons/app.png"));
    QString arrowUri = makeChevronArrowUri();
    app.setStyleSheet(wordLightStyleSheet(arrowUri));

    zb::MainWindow window;
    window.show();

#ifdef _WIN32
    ZB_LOG_INFO("ZeroBorders started (PID={})", GetCurrentProcessId());
#else
    ZB_LOG_INFO("ZeroBorders started (PID={})", getpid());
#endif

    int ret = QApplication::exec();

    ZB_LOG_INFO("ZeroBorders exiting with code {}", ret);
    return ret;
}
