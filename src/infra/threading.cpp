#include "vortex/infra/threading.hpp"

namespace vortex::infra {

WorkStealingPool::WorkStealingPool(uint32_t os_threads)
    : os_threads_(os_threads) {}

support::Result<uint64_t> WorkStealingPool::spawn(GreenThread thread) {
    threads_.push_back(thread);
    ++scheduled_;
    return thread.id;
}

support::Result<void> WorkStealingPool::yield(uint64_t thread_id) {
    (void)thread_id;
    return support::unimplemented("M:N scheduler yield");
}

support::Result<void> HandshakeManager::request(SuspensionKind kind,
                                                uint64_t thread_id) {
    (void)kind;
    (void)thread_id;
    return support::unimplemented("handshake suspension");
}

support::Result<void> HandshakeManager::acknowledge(uint64_t thread_id) {
    (void)thread_id;
    return support::unimplemented("handshake suspension");
}

support::Result<void> GuardPageSuspender::arm(void* stack_base,
                                              size_t stack_size) {
    (void)stack_base;
    (void)stack_size;
    return support::unimplemented("guard-page suspension");
}

support::Result<void> GuardPageSuspender::disarm(void* stack_base,
                                                 size_t stack_size) {
    (void)stack_base;
    (void)stack_size;
    return support::unimplemented("guard-page suspension");
}

support::Result<void*> SafepointPollingPage::reserve() {
    return support::unimplemented("safepoint polling page");
}

support::Result<void> SafepointPollingPage::arm() {
    return support::unimplemented("safepoint polling page");
}

support::Result<void> SafepointPollingPage::disarm() {
    return support::unimplemented("safepoint polling page");
}

}  // namespace vortex::infra
