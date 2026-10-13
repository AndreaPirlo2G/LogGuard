#pragma once
// Best-effort native stack symbolization.
//
// The ring stores raw PCs (from backtrace()/the sampling signal). For an
// incident report to be useful those PCs need module-relative offsets and,
// when the dynamic symbol table exposes them, nearest symbol names. This runs
// ONLY on the export worker (never inside a fatal signal handler), so it may
// read /proc/self/maps and call dladdr.
//
// Output is deliberately offline-resolvable even when local symbols are
// stripped: each frame is rendered as
//   0x7f.. in /bin/app+0x1a2f [main+0x44]
// and the module+offset can be fed to `addr2line -e /bin/app -f -C 0x1a2f`.
#include <cstdint>
#include <string>
#include <vector>

namespace lg::diag {

struct ResolvedFrame {
    uint64_t addr = 0;
    std::string module;       // file backing the mapping, "" if anonymous
    uint64_t file_offset = 0; // addr translated to an ELF file offset
    std::string nearest;      // nearest dynamic symbol, "" if unavailable
};

class Symbolizer {
public:
    Symbolizer();

    // Read the current process memory map once (cheap snapshot).
    void load_self();

    // Resolve one PC. Safe to call with a zero/unmapped address.
    ResolvedFrame resolve(uint64_t pc) const;

    // Render a whole captured stack (array of raw PCs) into multi-line text
    // with a fixed "#NN" prefix per frame.
    std::string render_stack(const uint64_t* frames, std::size_t n) const;

private:
    struct Region {
        uint64_t start = 0;
        uint64_t end = 0;
        uint64_t file_off = 0;
        std::string path;
        bool exec = false;
    };
    std::vector<Region> regions_;
    bool loaded_ = false;
};

} // namespace lg::diag
