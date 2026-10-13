#pragma once
// Platform crash/termination hook. LogGuard records a best-effort marker in the
// ring header and then DEFERS to any previously installed handler — this is how
// it coexists with Crashpad/Breakpad: install Crashpad first, then lg_init().
#include <cstdint>

// The recorder passes its mapped header pointer (lg_file_header_t in
// ring_layout.h) as void* to keep this interface C-linkage and free of any
// layout dependency; the implementation casts after including the layout.

namespace lg::crash {

struct Config {
    // If true (default), after writing our marker invoke the previously
    // installed handler (e.g. Crashpad) instead of forcing SIG_DFL.
    bool chain_existing = true;
    // When no prior handler exists, re-raise with SIG_DFL (core dump/OS report).
    bool reraise_default = true;
};

// Recorder hands the crash layer its mapped header + monotonic clock once the
// mapping is established.
void set_header(void* h, int64_t (*now_ns)());

// Begin intercepting fatal faults. Safe to call once at init.
bool install(const Config& cfg);

} // namespace lg::crash
