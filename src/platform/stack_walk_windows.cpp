#include "platform/stack_walk.hpp"

// Windows implementation (planned v0.5):
//   capture_self     -> RtlCaptureStackBackTrace
//   cooperative sink -> thread suspension + StackWalk64 (DbgHelp), guarded by
//                       the loader lock caveats, or a Vectored Continue handler
//                       driven sampler.
// The interface is fixed so core code compiles on Windows today; the capture
// itself is reported as unavailable until implemented.

namespace lg::plat {

std::size_t capture_self(uint64_t*, std::size_t, std::size_t) noexcept {
    return 0;
}

StackSampler::~StackSampler() = default;

bool StackSampler::install(Sink, void*) noexcept {
    return false; // not yet functional on Windows
}

std::size_t StackSampler::sample_all_threads() noexcept {
    return 0;
}

} // namespace lg::plat
