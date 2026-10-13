#include "crash.h"
#include "ring_layout.h"

#include <execinfo.h>
#include <initializer_list>
#include <pthread.h>
#include <signal.h>

namespace lg::crash {

namespace {

Config g_cfg{};
lg_hdr_t* g_hdr = nullptr;
int64_t (*g_now_ns)() = nullptr;

bool has_prior_handler(const struct sigaction& prev) {
    if (prev.sa_flags & SA_SIGINFO) return prev.sa_sigaction != nullptr;
    return prev.sa_handler != SIG_DFL && prev.sa_handler != SIG_IGN;
}

void handler(int sig, siginfo_t* si, void*) {
    if (g_hdr) {
        static volatile int reentry = 0;
        g_hdr->sig = sig;
        g_hdr->fault_addr = reinterpret_cast<uint64_t>(si ? si->si_addr : nullptr);
        g_hdr->crash_mono_ns = g_now_ns ? g_now_ns() : 0;
        // backtrace() is not formally async-signal-safe (may touch the loader
        // lock); guard against re-entry, treat frames as best-effort.
        if (!__sync_lock_test_and_set(&reentry, 1)) {
            void* frames[LG_CRASH_FRAMES];
            int nf = backtrace(frames, LG_CRASH_FRAMES);
            for (int i = 0; i < nf; ++i) g_hdr->crash_frames[i] = uint64_t(frames[i]);
            g_hdr->crash_nframes = nf;
        }
        __atomic_store_n(&g_hdr->crashed, 1u, __ATOMIC_RELEASE);
    }

    struct sigaction prev{};
    if (g_cfg.chain_existing && sigaction(sig, nullptr, &prev) == 0 &&
        has_prior_handler(prev)) {
        // Hand off to the previous owner (Crashpad/Breakpad/host app).
        if ((prev.sa_flags & SA_SIGINFO)) {
            if (prev.sa_sigaction) prev.sa_sigaction(sig, si, nullptr);
        } else if (prev.sa_handler) {
            prev.sa_handler(sig);
        }
        return;
    }
    if (g_cfg.reraise_default) {
        struct sigaction dfl{};
        dfl.sa_handler = SIG_DFL;
        sigemptyset(&dfl.sa_mask);
        sigaction(sig, &dfl, nullptr);
        pthread_kill(pthread_self(), sig);
    }
}

} // namespace

void set_header(void* hp, int64_t (*now_ns)()) {
    g_hdr = static_cast<lg_hdr_t*>(hp);
    g_now_ns = now_ns;
}

bool install(const Config& cfg) {
    g_cfg = cfg;

    stack_t ss{};
    static char alt[32768];
    ss.ss_sp = alt;
    ss.ss_size = sizeof(alt);
    sigaltstack(&ss, nullptr);

    struct sigaction sa{};
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int s : {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE})
        if (sigaction(s, &sa, nullptr) != 0) return false;
    return true;
}

} // namespace lg::crash
