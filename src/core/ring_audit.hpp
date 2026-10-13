#pragma once
// Ring health audit: scan a ring file/mapping offline and quantify how much of
// it is intact. This is the diagnostic counterpart to the hot-path reader:
// collect_epoch assumes a well-behaved live writer, while audit is meant to run
// against a file that may have been torn by power loss, truncated, or corrupted
// on disk. It never mutates the mapping and is allocation-bounded.
#include "slot_format.hpp"

#include <cstdint>

namespace lg {
namespace core {

struct RingAudit {
    uint64_t capacity = 0;        // declared slot count
    uint64_t valid = 0;           // slots that pass every integrity check
    uint64_t empty = 0;           // never-written slots (magic == 0)
    uint64_t torn = 0;            // partially published / inconsistent slots
    uint64_t foreign_epoch = 0;   // intact slots from a different epoch
    uint64_t ticket_mismatch = 0; // magic good but ticket does not match index
    uint32_t header_epoch = 0;
    bool header_ok = false;
    bool crashed = false;

    // Ticket continuity of the current epoch (0/0 if fewer than 2 slots).
    uint64_t min_ticket = 0;
    uint64_t max_ticket = 0;
    uint64_t distinct_tickets = 0;

    bool healthy() const { return torn == 0 && ticket_mismatch == 0; }

    // Fraction of the ring currently holding valid current-epoch records.
    double fill_ratio() const {
        return capacity == 0 ? 0.0
                             : static_cast<double>(valid) /
                                   static_cast<double>(capacity);
    }
};

// Audit a raw mapping. |base| must point at the FileHeader page followed by
// the slot arena, exactly as the platform mapping lays it out.
RingAudit audit_ring(const void* base, uint64_t capacity);

} // namespace core
} // namespace lg
