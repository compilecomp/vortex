#include "vortex/infra/snapshot.hpp"

namespace vortex::infra {

support::Result<void> SnapshotWriter::add_section(SnapshotSection kind,
                                                  std::vector<uint8_t> payload) {
    total_bytes_ += payload.size();
    sections_.emplace_back(kind, std::move(payload));
    header_.section_count = static_cast<uint32_t>(sections_.size());
    return support::ok();
}

support::Result<std::vector<uint8_t>> SnapshotWriter::finalize() {
    return support::unimplemented("snapshot serialization");
}

support::Result<SnapshotReader> SnapshotReader::open(const std::string& path) {
    (void)path;
    return support::unimplemented("snapshot reader");
}

support::Result<size_t> SnapshotReader::section_count() const {
    return support::unimplemented("snapshot reader");
}

support::Result<std::string> AotPipeline::emit_shared_library(
    const PgoProfileInput& input, const std::string& output_path) {
    (void)input;
    (void)output_path;
    return support::unimplemented("PGO-AOT pipeline");
}

}  // namespace vortex::infra
