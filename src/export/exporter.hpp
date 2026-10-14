#pragma once
// Snapshot exporter: collect the currently-valid ring window, render each slot
// and serialize it to a self-contained MCAP (.lgp) incident package.
//
// Used by the recorder's worker (live snapshots) and by offline recovery
// (startup export of an unclean ring). It depends only on the POD slot layout,
// the renderer and the MCAP writer — never on the recorder's global state — so
// it is trivially unit-testable and reusable by third-party tooling.
#include "core/slot_format.hpp"
#include "export/redaction.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace lg::diag {
class Symbolizer;
}

namespace lg::export_ {

struct ExportResult {
    std::string path;
    int count = 0;
    bool ok = false;
    int redacted_hits = 0; // number of sensitive tokens masked, if a redactor ran
    // Per-family breakdown (all zero when no redactor ran). Mirrored into the
    // package manifest as "redaction_report".
    RedactionStats redaction;
};

struct ExportOptions {
    bool zstd = false;     // compress the data chunk when libzstd is present
    const char* prefix = "snapshot";
    // When non-null, rendered text is passed through this privacy redactor
    // before serialization, so the on-disk package carries no PII/secrets.
    const Redactor* redactor = nullptr;
    // When non-null (live export), stack slots are best-effort symbolized.
    const lg::diag::Symbolizer* symbolizer = nullptr;
};

// Gather the slots belonging to epoch `epoch` from a contiguous slot array of
// `slot_count` fixed slots, returning them copied and ordered by ticket.
std::vector<lg::core::Slot>
collect_window(const lg::core::Slot* slots, uint64_t slot_count, uint32_t epoch);

// Write a package at "<dir>/<prefix>-<wall_ns>.lgp". The header supplies crash
// marker/stack state and the target epoch; `slots` is the raw ring array.
ExportResult export_window(const lg::core::FileHeader& header,
                           const lg::core::Slot* slots, uint64_t slot_count,
                           const std::string& dir, const ExportOptions& opts);

// Export an already collected, ticket-ordered slot list. Used by ring
// self-healing, where the header page is unreadable and only arena slots
// survived; consequently no crash marker/stack is appended.
ExportResult export_slots(std::vector<lg::core::Slot> recs,
                          const std::string& dir, const ExportOptions& opts);

} // namespace lg::export_
