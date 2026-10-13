// Windows crash hook: Vectored Exception Handler + termination handlers.
// The VEH is added FIRST (so it runs before others) but does not consume the
// exception: after writing the marker it returns EXCEPTION_CONTINUE_SEARCH so
// Crashpad/WER still process it — the Windows analogue of POSIX chaining.
#include "crash.h"

#ifndef _WIN32
#error "crash_windows.cpp is Windows-only"
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "ring_layout.h"

namespace lg::crash {

namespace {
Config g_cfg{};
lg_hdr_t* g_hdr = nullptr;
int64_t (*g_now_ns)() = nullptr;
void* g_veh = nullptr;
void* g_terminate = nullptr;
void* g_unhandled = nullptr;

int64_t qpc_now_ns() {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c{}; QueryPerformanceCounter(&c);
    return int64_t(double(c.QuadPart) * 1e9 / double(freq.QuadPart));
}

LONG WINAPI veh(PEXCEPTION_POINTERS ep) {
    if (g_hdr) {
        g_hdr->sig = int(ep->ExceptionRecord->ExceptionCode);
        g_hdr->fault_addr = reinterpret_cast<uint64_t>(ep->ExceptionRecord->ExceptionAddress);
        g_hdr->crash_mono_ns = g_now_ns ? g_now_ns() : qpc_now_ns();
        // TODO(port): capture CONTEXT->Rip/Rsp stack walk via StackWalk64.
        __atomic_store_n(&g_hdr->crashed, 1u, __ATOMIC_RELEASE);
    }
    FlushViewOfFile(g_hdr, 0); // best-effort durability before continuing
    return EXCEPTION_CONTINUE_SEARCH; // let Crashpad/WER run
}
} // namespace

void set_header(void* hp, int64_t (*now_ns)()) {
    g_hdr = static_cast<lg_hdr_t*>(hp); g_now_ns = now_ns;
}

bool install(const Config& cfg) {
    g_cfg = cfg;
    g_veh = AddVectoredExceptionHandler(/*first*/ 1, &veh);
    // std::terminate / unhandled-filter registration is wired by the C++
    // wrapper in a follow-up; VEH alone covers access violations.
    return g_veh != nullptr;
}

} // namespace lg::crash
#endif
