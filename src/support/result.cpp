#include "vortex/support/result.hpp"

namespace vortex::support {

const char* error_code_name(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Ok: return "Ok";
    case ErrorCode::SyntaxError: return "SyntaxError";
    case ErrorCode::AssembleError: return "AssembleError";
    case ErrorCode::VerifyError: return "VerifyError";
    case ErrorCode::DecodeError: return "DecodeError";
    case ErrorCode::RuntimeError: return "RuntimeError";
    case ErrorCode::Unimplemented: return "Unimplemented";
    case ErrorCode::InvalidArgument: return "InvalidArgument";
    case ErrorCode::OutOfMemory: return "OutOfMemory";
    case ErrorCode::InternalError: return "InternalError";
    }
    return "?";
}

std::string Diagnostic::format() const {
    std::string out = error_code_name(code);
    out += ": ";
    out += message;
    if (line != 0) {
        out += " (line " + std::to_string(line);
        if (column != 0) out += ", col " + std::to_string(column);
        out += ")";
    }
    return out;
}

}  // namespace vortex::support
