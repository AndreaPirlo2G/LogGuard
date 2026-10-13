#include "thread_id.hpp"

#include <sys/syscall.h>
#include <unistd.h>

namespace lg::plat {

uint64_t current_tid() noexcept {
    static thread_local const uint64_t tid = static_cast<uint64_t>(syscall(SYS_gettid));
    return tid;
}

} // namespace lg::plat
