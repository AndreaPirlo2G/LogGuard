#include "clock.hpp"

#include <ctime>

namespace lg::plat {

int64_t mono_now_ns() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

int64_t wall_now_ns() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return int64_t(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

} // namespace lg::plat
