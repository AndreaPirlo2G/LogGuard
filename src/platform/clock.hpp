#pragma once
// Monotonic and wall-clock timestamps. Monotonic time is used for all internal
// ordering, windows and cooldowns; wall time is only for human-readable output.
#include <cstdint>

namespace lg::plat {

// Nanoseconds since an unspecified epoch; immune to wall-clock adjustments.
int64_t mono_now_ns() noexcept;

// Nanoseconds since the Unix epoch (CLOCK_REALTIME).
int64_t wall_now_ns() noexcept;

// Convenience conversions.
constexpr int64_t ms_to_ns(int64_t ms) { return ms * 1'000'000LL; }
constexpr int64_t sec_to_ns(int64_t s) { return s * 1'000'000'000LL; }

} // namespace lg::plat
