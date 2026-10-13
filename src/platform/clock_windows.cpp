#include "clock.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <ctime>

namespace lg::plat {

int64_t mono_now_ns() noexcept {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return static_cast<int64_t>(double(c.QuadPart) * 1e9 / double(freq.QuadPart));
}

int64_t wall_now_ns() noexcept {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    // 100-ns intervals since 1601-01-01 -> ns since 1970-01-01
    return static_cast<int64_t>(u.QuadPart) * 10 - 11644473600000000000LL;
}

} // namespace lg::plat
