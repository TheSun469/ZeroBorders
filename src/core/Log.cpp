#include "Log.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace fs = std::filesystem;

namespace zb::log {

namespace {

std::mutex& fileMutex() {
    static std::mutex m;
    return m;
}

// Stores the active log directory so the crash handler can write a final
// record without relying on sink state (which may be corrupted by the time
// an access violation is caught).
std::string& logDirStorage() {
    static std::string dir;
    return dir;
}

#ifdef _WIN32
std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                  static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), len);
    return out;
}
#endif

fs::path pathFromUtf8(const std::string& utf8) {
#ifdef _WIN32
    return fs::path(utf8ToWide(utf8));
#else
    return fs::path(utf8);
#endif
}

} // namespace

std::string getLogFileName() {
    using namespace std::chrono;
    auto now = system_clock::now();
    std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "zb_%04d%02d%02d.log",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

void addFileSink(const std::string& logDir) {
    std::error_code ec;
    fs::path dirPath = pathFromUtf8(logDir);
    fs::create_directories(dirPath, ec);
    if (ec) {
        // Fall back to stdout-only logging; record the failure there.
        ZB_LOG_ERROR("Failed to create log directory '{}': {}", logDir, ec.message());
        return;
    }

    // Remember the directory for the crash handler.
    logDirStorage() = logDir;

    auto logFilePath = dirPath / getLogFileName();

    // Write a header so the file is created immediately (and to confirm the
    // path is writable before we start relying on it).
    {
        std::lock_guard<std::mutex> lk(fileMutex());
#ifdef _WIN32
        std::ofstream hdr(logFilePath.wstring(), std::ios::app | std::ios::binary);
#else
        std::ofstream hdr(logFilePath.string(), std::ios::app | std::ios::binary);
#endif
        if (hdr.is_open()) {
            hdr << "=== ZeroBorders log session started ===\n";
        }
    }

    // Capture the path by value so the sink survives directory changes.
    auto logPathW = logFilePath.wstring();
    auto logPathU8 = logFilePath.string();

    addSink([logPathW, logPathU8](Level, std::string_view line) {
        std::lock_guard<std::mutex> lk(fileMutex());
#ifdef _WIN32
        std::ofstream file(logPathW, std::ios::app | std::ios::binary);
#else
        std::ofstream file(logPathU8, std::ios::app | std::ios::binary);
#endif
        if (file.is_open()) {
            file.write(line.data(), static_cast<std::streamsize>(line.size()));
            file.flush();
        }
    });

    ZB_LOG_INFO("File logging enabled: {}", logPathU8);
}

void writeToCrashLog(const std::string& logDir, std::string_view text) {
    if (logDir.empty()) return;
    fs::path logFilePath = pathFromUtf8(logDir) / getLogFileName();
    // Best-effort: no exception handling needed here (called from crash
    // handler where exceptions are suppressed), but std::ofstream itself
    // won't throw on open failure by default.
    std::lock_guard<std::mutex> lk(fileMutex());
#ifdef _WIN32
    std::ofstream file(logFilePath.wstring(), std::ios::app | std::ios::binary);
#else
    std::ofstream file(logFilePath.string(), std::ios::app | std::ios::binary);
#endif
    if (file.is_open()) {
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        file.flush();
    }
}

const std::string& activeLogDir() {
    return logDirStorage();
}

} // namespace zb::log
