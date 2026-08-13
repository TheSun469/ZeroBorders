#include "gui/MainWindow.h"
#include "core/Log.h"

#include <QApplication>
#include <QStyleFactory>
#include <QIcon>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <dbghelp.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <system_error>
#include <filesystem>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "dbghelp.lib")

namespace fs = std::filesystem;

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

// Returns the directory containing the running executable (UTF-8).
std::string getExeDirectory() {
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
}

std::string formatTimestamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

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

// Writes a crash report to today's log file. Kept minimal because the
// process state may be corrupted at this point.
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

void __cdecl signalHandler(int sig) {
    const char* name = "unknown";
    switch (sig) {
        case SIGABRT: name = "SIGABRT (abort)"; break;
        case SIGFPE:  name = "SIGFPE (floating-point)"; break;
        case SIGILL:  name = "SIGILL (illegal instruction)"; break;
        case SIGSEGV: name = "SIGSEGV (segmentation fault)"; break;
        case SIGTERM: name = "SIGTERM (terminate)"; break;
        default: break;
    }
    writeCrashReport(name, static_cast<DWORD>(sig), nullptr, nullptr);
    std::_Exit(1);
}

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

// Installs all crash handlers and enables file logging. Destruction restores
// the previous handlers (mainly for cleanliness; in practice the process is
// terminating anyway).
struct CrashGuard {
    LPTOP_LEVEL_EXCEPTION_FILTER prevFilter_ = nullptr;
    _invalid_parameter_handler prevInvalid_ = nullptr;
    _purecall_handler prevPure_ = nullptr;

    CrashGuard() {
        prevFilter_ = SetUnhandledExceptionFilter(unhandledExceptionFilter);
        std::signal(SIGABRT, signalHandler);
        std::signal(SIGFPE, signalHandler);
        std::signal(SIGILL, signalHandler);
        std::signal(SIGSEGV, signalHandler);
        std::signal(SIGTERM, signalHandler);
        prevInvalid_ = _set_invalid_parameter_handler(invalidParameterHandler);
        prevPure_ = _set_purecall_handler(pureCallHandler);
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
            writeCrashReport("Uncaught C++ Exception", 0, nullptr, nullptr);
            std::_Exit(1);
        });

        // Enable file logging to <exe-dir>/log.
        std::string logDir = getExeDirectory() + "\\log";
        zb::log::addFileSink(logDir);
    }

    ~CrashGuard() {
        // Flush all sinks before the process exits so the last log lines
        // are durable on disk.
        zb::log::removeAllSinks();
    }
};

} // namespace

// 单实例锁：用命名互斥量确保同一台电脑只运行一个 ZeroBorders 进程。
// 如果已有实例运行，尝试把已有窗口提到前台后退出。
bool checkSingleInstance() {
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
}

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);

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

    zb::MainWindow window;
    window.show();

    ZB_LOG_INFO("ZeroBorders started (PID={})", GetCurrentProcessId());

    int ret = QApplication::exec();

    ZB_LOG_INFO("ZeroBorders exiting with code {}", ret);
    return ret;
}
