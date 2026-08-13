#pragma once

#include <chrono>
#include <cstdio>
#include <format>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace zb::log {

enum class Level { Debug, Info, Warn, Error };

using SinkCallback = std::function<void(Level lvl, std::string_view line)>;

inline std::mutex& sinkMutex() {
    static std::mutex m;
    return m;
}
inline std::vector<SinkCallback>& sinks() {
    static std::vector<SinkCallback> s;
    return s;
}

inline void addSink(SinkCallback cb) {
    std::lock_guard<std::mutex> lk(sinkMutex());
    sinks().push_back(std::move(cb));
}

inline void removeAllSinks() {
    std::lock_guard<std::mutex> lk(sinkMutex());
    sinks().clear();
}

// File logging: append every log line to a per-day file under logDir.
// The directory is created if missing. Safe to call once at startup.
// Declared here, implemented in Log.cpp (non-inline to keep std::ofstream
// out of the header).
void addFileSink(const std::string& logDir);

// Build the per-day log file name: zb_YYYYMMDD.log
std::string getLogFileName();

// Low-level crash writer: appends a raw byte sequence to today's log file
// without going through the sink machinery. Used by the crash handler so we
// can record a stack trace even when the process state is corrupt.
void writeToCrashLog(const std::string& logDir, std::string_view text);

// Returns the log directory currently configured by addFileSink(). Returns
// an empty string if file logging was never enabled. Used by the crash
// handler to locate the destination file without touching sink state.
const std::string& activeLogDir();

inline void write(Level lvl, std::string_view msg) {
    const char* tag = "DBG";
    switch (lvl) {
        case Level::Debug: tag = "DBG"; break;
        case Level::Info:  tag = "INF"; break;
        case Level::Warn:  tag = "WRN"; break;
        case Level::Error: tag = "ERR"; break;
    }

    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto t = system_clock::to_time_t(now);
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);

    std::string line = std::format("[{}:{:03d}] [{}] {}\n", buf, ms.count(), tag, msg);
    std::fputs(line.c_str(), stdout);
    std::fflush(stdout);
#ifdef _WIN32
    OutputDebugStringA(line.c_str());
#endif

    // Dispatch to GUI sinks (copy list under lock to avoid deadlock if a
    // sink adds/removes another sink).
    std::vector<SinkCallback> snapshot;
    {
        std::lock_guard<std::mutex> lk(sinkMutex());
        snapshot = sinks();
    }
    for (auto& s : snapshot) {
        if (s) s(lvl, line);
    }
}

} // namespace zb::log

#define ZB_LOG_DEBUG(...) ::zb::log::write(::zb::log::Level::Debug, std::format(__VA_ARGS__))
#define ZB_LOG_INFO(...)  ::zb::log::write(::zb::log::Level::Info,  std::format(__VA_ARGS__))
#define ZB_LOG_WARN(...)  ::zb::log::write(::zb::log::Level::Warn,  std::format(__VA_ARGS__))
#define ZB_LOG_ERROR(...) ::zb::log::write(::zb::log::Level::Error, std::format(__VA_ARGS__))
