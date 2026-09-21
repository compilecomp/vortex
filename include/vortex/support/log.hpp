// Structured logging used by the runtime, compiler tiers and the `vx` tool.
#pragma once

#include <cstdint>
#include <string_view>

namespace vortex::support {

enum class LogLevel : uint8_t {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Off = 5,
};

class Logger {
public:
    static Logger& instance();

    void set_level(LogLevel level) noexcept { level_ = level; }
    [[nodiscard]] LogLevel level() const noexcept { return level_; }

    void log(LogLevel level, std::string_view component, std::string_view message);

private:
    LogLevel level_ = LogLevel::Warn;
};

inline void trace(std::string_view component, std::string_view msg) {
    Logger::instance().log(LogLevel::Trace, component, msg);
}
inline void debug(std::string_view component, std::string_view msg) {
    Logger::instance().log(LogLevel::Debug, component, msg);
}
inline void info(std::string_view component, std::string_view msg) {
    Logger::instance().log(LogLevel::Info, component, msg);
}
inline void warn(std::string_view component, std::string_view msg) {
    Logger::instance().log(LogLevel::Warn, component, msg);
}
inline void error(std::string_view component, std::string_view msg) {
    Logger::instance().log(LogLevel::Error, component, msg);
}

}  // namespace vortex::support
