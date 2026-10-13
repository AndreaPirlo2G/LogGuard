#include "thread_id.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace lg::plat {

uint64_t current_tid() noexcept {
    static thread_local const uint64_t tid =
        static_cast<uint64_t>(GetCurrentThreadId());
    return tid;
}

} // namespace lg::plat
