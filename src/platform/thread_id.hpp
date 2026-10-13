#pragma once
// Stable OS thread identifier. Distinct from pthread_t/std::thread::id: this is
// the value the kernel and /proc use, which is what we want in incident reports.
#include <cstdint>

namespace lg::plat {

// Cached per thread after the first call (the id never changes within a thread).
uint64_t current_tid() noexcept;

} // namespace lg::plat
