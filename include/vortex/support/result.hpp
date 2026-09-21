// Result<T> — error handling across the compiler stack. C++26-oriented code;
// std::expected is the underlying carrier.
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace vortex::support {

enum class ErrorCode : uint8_t {
    Ok = 0,
    SyntaxError,
    AssembleError,
    VerifyError,
    DecodeError,
    RuntimeError,
    Unimplemented,
    InvalidArgument,
    OutOfMemory,
    InternalError,
};

const char* error_code_name(ErrorCode code) noexcept;

/// A diagnostic with a human-readable message and optional source location.
struct Diagnostic {
    ErrorCode code = ErrorCode::InternalError;
    std::string message;
    uint32_t line = 0;
    uint32_t column = 0;

    Diagnostic() = default;
    Diagnostic(ErrorCode c, std::string msg, uint32_t ln = 0, uint32_t col = 0)
        : code(c), message(std::move(msg)), line(ln), column(col) {}

    std::string format() const;
};

template <typename T>
using Result = std::expected<T, Diagnostic>;

inline Result<void> ok() { return Result<void>{}; }

/// Returns an unexpected diagnostic convertible into any Result<T> — call sites
/// simply `return fail(...);` regardless of their T.
inline std::unexpected<Diagnostic> fail(ErrorCode code, std::string message,
                                        uint32_t line = 0, uint32_t col = 0) {
    return std::unexpected(Diagnostic(code, std::move(message), line, col));
}

/// Marker for contract-stubbed subsystems (see docs/roadmap.md).
inline std::unexpected<Diagnostic> unimplemented(std::string_view what) {
    return fail(ErrorCode::Unimplemented,
                std::string(what) + ": contract-stubbed (see docs/roadmap.md)");
}

}  // namespace vortex::support
