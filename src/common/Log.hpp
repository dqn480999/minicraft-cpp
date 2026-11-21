#pragma once

#include <format>
#include <string>
#include <utility>

namespace mc {

enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error
};

class Log {
public:
    static void init(bool debug, bool color);
    static void setFile(const std::string& path);

    template <typename... Args>
    static void trace(std::format_string<Args...> fmt, Args&&... args) {
        write(LogLevel::Trace, std::format(fmt, std::forward<Args>(args)...));
    }

    template <typename... Args>
    static void debug(std::format_string<Args...> fmt, Args&&... args) {
        write(LogLevel::Debug, std::format(fmt, std::forward<Args>(args)...));
    }

    template <typename... Args>
    static void info(std::format_string<Args...> fmt, Args&&... args) {
        write(LogLevel::Info, std::format(fmt, std::forward<Args>(args)...));
    }

    template <typename... Args>
    static void warn(std::format_string<Args...> fmt, Args&&... args) {
        write(LogLevel::Warn, std::format(fmt, std::forward<Args>(args)...));
    }

    template <typename... Args>
    static void error(std::format_string<Args...> fmt, Args&&... args) {
        write(LogLevel::Error, std::format(fmt, std::forward<Args>(args)...));
    }

private:
    static void write(LogLevel level, const std::string& message);
    static bool enabled(LogLevel level);
};

}
