#include "platform/stack_walk.hpp"

#include <dirent.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

namespace lg::plat {

namespace {

// Realtime signal reserved for cooperative stack sampling. Picked from the
// upper end to avoid colliding with user handlers on low realtime signals.
int sample_sig() {
    // SIGRTMIN is a runtime libc call (not constexpr), offset 5 keeps clear of
    // low realtime signals commonly used by runtimes.
    static const int sig = SIGRTMIN + 5;
    return sig;
}

StackSampler::Sink g_sink = nullptr;
void* g_sink_user = nullptr;

void sample_handler(int) {
    if (!g_sink) return;
    uint64_t frames[kMaxStackFrames];
    std::size_t n = capture_self(frames, kMaxStackFrames, 1);
    uint64_t tid = static_cast<uint64_t>(::syscall(SYS_gettid));
    g_sink(tid, frames, n, g_sink_user);
}

} // namespace

std::size_t capture_self(uint64_t* out, std::size_t capacity,
                         std::size_t skip) noexcept {
    void* raw[kMaxStackFrames];
    int n = ::backtrace(raw, static_cast<int>(capacity + skip));
    if (n <= static_cast<int>(skip)) return 0;
    n -= static_cast<int>(skip);
    if (static_cast<std::size_t>(n) > capacity) n = static_cast<int>(capacity);
    for (int i = 0; i < n; ++i)
        out[i] = reinterpret_cast<uint64_t>(raw[i + static_cast<int>(skip)]);
    return static_cast<std::size_t>(n);
}

StackSampler::~StackSampler() {
    if (!installed_) return;
    struct sigaction sa{};
    sa.sa_handler = SIG_DFL;
    ::sigemptyset(&sa.sa_mask);
    ::sigaction(sample_sig(), &sa, nullptr);
}

bool StackSampler::install(Sink sink, void* user) noexcept {
    g_sink = sink;
    g_sink_user = user;
    struct sigaction sa{};
    sa.sa_handler = sample_handler;
    sa.sa_flags = SA_RESTART | SA_ONSTACK;
    ::sigemptyset(&sa.sa_mask);
    if (::sigaction(sample_sig(), &sa, nullptr) != 0) return false;
    installed_ = true;
    return true;
}

std::size_t StackSampler::sample_all_threads() noexcept {
    if (!installed_) return 0;

    uint64_t tids[256];
    std::size_t ntid = 0;
    DIR* d = ::opendir("/proc/self/task");
    if (d) {
        struct dirent* e;
        while ((e = ::readdir(d)) != nullptr && ntid < 256) {
            if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
            tids[ntid++] = static_cast<uint64_t>(std::strtoull(e->d_name, nullptr, 10));
        }
        ::closedir(d);
    }
    if (ntid == 0) return 0;

    // Each interrupted thread runs sample_handler -> recorder sink, which
    // performs the actual lock-free ring write. Brief settle window; threads
    // that miss the signal are simply absent from the snapshot (acceptable for
    // a diagnostic best-effort, and ticket order keeps results consistent).
    for (std::size_t i = 0; i < ntid; ++i)
        ::syscall(SYS_tgkill, ::getpid(), static_cast<pid_t>(tids[i]), sample_sig());
    ::usleep(20 * 1000);
    return ntid;
}

} // namespace lg::plat
