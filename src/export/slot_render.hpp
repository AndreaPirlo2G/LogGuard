#pragma once
// Turn one committed ring Slot into the human/JSON representation written to an
// incident package. Kept separate from both the ring (storage) and the MCAP
// container (format) so rendering policy — how a stack trace is stringified,
// how structured fields are split from the message — can evolve in isolation.
#include "core/slot_format.hpp"

#include <cstdint>
#include <string>

namespace lg::diag {
class Symbolizer;
}

namespace lg::export_ {

struct RenderedSlot {
    std::string category;
    std::string request_id;
    std::string message;
    std::string fields_json; // empty when the slot carries no structured fields
    uint16_t level = 2;
};

// Render one slot. Stack slots are turned into a single "thread stack ..." line
// with raw hex PCs (symbolization is a separate offline step); KV slots split
// message and fields on the embedded NUL; lazy slots are formatted now; plain
// slots expose their raw payload.
RenderedSlot render_slot(const lg::core::Slot& s);

// Same as render_slot, but for stack slots appends best-effort resolved
// module+offset/nearest-symbol lines when a symbolizer is supplied (live export
// only; a recovering process cannot resolve a dead process's PCs).
RenderedSlot render_slot(const lg::core::Slot& s,
                         const lg::diag::Symbolizer* sym);

// Render the crashing-thread stack stored in the file header, if any.
// Returns an empty string when no crash frames are present.
std::string render_crash_stack(const lg::core::FileHeader& h);

// Render the synthetic crash marker line ("logguard crash marker: signal=..").
std::string render_crash_marker(const lg::core::FileHeader& h);

} // namespace lg::export_
