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
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace lg::diag {

struct ResolvedFrame {
    uint64_t addr = 0;
    std::string module;       // file backing the mapping, "" if anonymous
    uint64_t file_offset = 0; // addr translated to an ELF file offset
    std::string nearest;      // nearest dynamic symbol, "" if unavailable
};

// Source location from `addr2line -f -C -e <module> <offset>` (export-time
// only). Empty fields mean the tool was unavailable or the binary is stripped
// without debug info.
struct LineInfo {
    std::string function; // demangled function name, "??" if unknown
    std::string location; // "file.cpp:42" or "??:0"
};

class Symbolizer {
public:
    Symbolizer();

    // Read the current process memory map once (cheap snapshot).
    void load_self();

    // Resolve one PC. Safe to call with a zero/unmapped address.
    ResolvedFrame resolve(uint64_t pc) const;

    // Enable resolution to source file/line via the external addr2line tool.
    // Modules are resolved lazily and one addr2line process handles a whole
    // module's offsets at once, so a 64-frame stack still costs ~1 process
    // per backing ELF. No-op (returns false) if addr2line is not on PATH.
    void enable_source_locations(bool on);

    // Best-effort source location for a previously resolved frame. Requires
    // enable_source_locations(true); otherwise fields stay empty.
    LineInfo source_location(const ResolvedFrame& fr) const;

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
    // Batch-resolve all requested offsets of one module with a single
    // addr2line invocation; populates line_cache_ on success.
    void resolve_module(const std::string& module,
                        const std::vector<uint64_t>& offsets) const;

    std::vector<Region> regions_;
    bool loaded_ = false;
    bool source_enabled_ = false;
    mutable int addr2line_checked_ = -1; // -1 unknown, 0/1 available
    // (module, file_offset) -> source location
    mutable std::map<std::pair<std::string, uint64_t>, LineInfo> line_cache_;
    // modules already batch-resolved (even when the answer is all "??")
    mutable std::unordered_map<std::string, bool> modules_resolved_;
};

} // namespace lg::diag
