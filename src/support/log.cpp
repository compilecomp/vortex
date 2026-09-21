#include "vortex/support/log.hpp"

#include <cstdio>
#include <mutex>

namespace vortex::support {

namespace {
const char* level_name(LogLevel l) noexcept {
    switch (l) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warn: return "WARN";
    case LogLevel::Error: return "ERROR";
    case LogLevel::Off: return "OFF";
    }
    return "?";
}
}  // namespace

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::log(LogLevel level, std::string_view component,
                 std::string_view message) {
    if (level < level_) return;
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    std::FILE* out = level >= LogLevel::Warn ? stderr : stdout;
    std::fprintf(out, "[%s] %.*s: %.*s\n", level_name(level),
                 static_cast<int>(component.size()), component.data(),
                 static_cast<int>(message.size()), message.data());
}

}  // namespace vortex::support
