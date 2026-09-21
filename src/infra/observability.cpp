#include "vortex/infra/observability.hpp"

namespace vortex::infra {

void ReplayRecorder::record_event(uint32_t kind, uint64_t payload) {
    log_.emplace_back(kind, payload);
}

Result<void> JitDumpWriter::open(const std::string& path) {
    (void)path;
    return support::unimplemented("jitdump writer");
}

Result<void> JitDumpWriter::record_code_load(
    uint64_t addr, size_t size, const std::string& symbol,
    const std::string& source_line_map) {
    (void)addr;
    (void)size;
    (void)symbol;
    (void)source_line_map;
    return support::unimplemented("jitdump writer");
}

Result<void> JitDumpWriter::close() {
    return support::unimplemented("jitdump writer");
}

Result<void> GdbJitInterface::register_code(const void* code,
                                            size_t size,
                                            const char* symbol) {
    (void)code;
    (void)size;
    (void)symbol;
    return support::unimplemented("GDB JIT interface");
}

Result<void> GdbJitInterface::unregister_code(const void* code) {
    (void)code;
    return support::unimplemented("GDB JIT interface");
}

}  // namespace vortex::infra
