#include "common/Log.hpp"

#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

namespace mc {
namespace {

std::atomic<bool> g_debug{false};
std::atomic<bool> g_color{true};
std::mutex g_mutex;
std::ofstream g_file;

const char* levelName(LogLevel level) {
    switch (level) {
    case LogLevel::Trace:
        return "TRACE";
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warn:
        return "WARN";
    case LogLevel::Error:
        return "ERROR";
    }
    return "INFO";
}

const char* levelColor(LogLevel level) {
    switch (level) {
    case LogLevel::Trace:
        return "\033[90m";
    case LogLevel::Debug:
        return "\033[36m";
    case LogLevel::Info:
        return "\033[32m";
    case LogLevel::Warn:
        return "\033[33m";
    case LogLevel::Error:
        return "\033[31m";
    }
    return "\033[0m";
}

}

void Log::init(bool debug, bool color) {
    g_debug.store(debug, std::memory_order_relaxed);
    g_color.store(color, std::memory_order_relaxed);
}

void Log::setFile(const std::string& path) {
    if (path.empty()) {
        return;
    }
    std::lock_guard lock(g_mutex);
    g_file.open(path, std::ios::trunc);
    if (!g_file) {
        g_file.clear();
    }
}

bool Log::enabled(LogLevel level) {
    if (level == LogLevel::Trace || level == LogLevel::Debug) {
        return g_debug.load(std::memory_order_relaxed);
    }
    return true;
}

void Log::write(LogLevel level, const std::string& message) {
    if (!enabled(level)) {
        return;
    }
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm{};
    localtime_r(&time, &tm);

    std::ostringstream stamp;
    stamp << std::put_time(&tm, "%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << millis.count();

    const auto tid = static_cast<unsigned long long>(std::hash<std::thread::id>{}(std::this_thread::get_id()));

    std::lock_guard lock(g_mutex);
    if (g_file.is_open()) {
        g_file << stamp.str() << ' ' << levelName(level) << " [" << (tid & 0xFFFFu) << "] " << message << '\n';
        g_file.flush();
    }
    if (g_color.load(std::memory_order_relaxed)) {
        std::cout << "\033[90m" << stamp.str() << "\033[0m "
                  << levelColor(level) << std::left << std::setw(5) << levelName(level) << "\033[0m "
                  << "\033[90m[" << (tid & 0xFFFFu) << "]\033[0m "
                  << message << '\n';
    } else {
        std::cout << stamp.str() << ' ' << levelName(level) << " [" << (tid & 0xFFFFu) << "] " << message << '\n';
    }
    std::cout.flush();
}

}
