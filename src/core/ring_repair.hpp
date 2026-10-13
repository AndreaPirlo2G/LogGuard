#pragma once
// Ring self-healing.
//
// A robot/vehicle power loss can interrupt the one-page header write, or a disk
// can hand back a file whose leading page no longer starts with "LGRING02".
// The naive reaction (treat bad magic as a fresh file and memset the header)
// discards a potentially salvageable black box. ring_repair instead inspects
// the slot arena first: published slots carry their own per-slot magic and are
// independently recognisable. If enough of them are intact we can reconstruct
// the ring geometry from the file size, recover the dominant epoch, export what
// survived, and only then reinitialise.
//
// Everything here is a pure function over a memory image so it is unit-testable
// without touching the filesystem or signal handlers.
#include "slot_format.hpp"

#include <cstdint>
#include <vector>

namespace lg {
namespace core {

enum class HeaderState {
    FreshZero,              // header page is all zero: never initialised
    Valid,                  // magic present and geometry matches the file
    GeometryMismatch,       // magic present but slot_count disagrees with size
    CorruptWithData,        // bad magic but the arena holds intact slots
    CorruptEmpty            // bad magic and no recognisable slots: safe reset
};

struct RepairReport {
    HeaderState state = HeaderState::FreshZero;

    uint64_t file_capacity = 0;   // usable slots implied by the file size
    uint64_t header_capacity = 0; // capacity the header claims (0 if unreadable)
    uint32_t recovered_epoch = 0; // dominant epoch among salvaged slots

    uint64_t salvageable = 0;     // intact slots belonging to recovered_epoch
    uint64_t torn = 0;            // published-looking but inconsistent slots
    bool header_readable = false; // magic page parses at all

    bool should_salvage() const { return salvageable > 0; }
    bool should_reset_header() const {
        return state == HeaderState::FreshZero ||
               state == HeaderState::CorruptWithData ||
               state == HeaderState::CorruptEmpty ||
               state == HeaderState::GeometryMismatch;
    }
};

// Largest power-of-two slot count that fits the file geometry. Returns 0 if the
// file is smaller than one header page plus a single slot.
uint64_t infer_capacity(uint64_t file_size);

// Classify a mapping. |base| points at the FileHeader page followed by the slot
// arena; |file_size| is the total mapped size. Never mutates the image.
RepairReport diagnose(const void* base, uint64_t file_size);

// Best-effort recovery of intact, current-epoch slots from an image whose
// header may be unreadable. Geometry and epoch are inferred from the arena;
// returned slots are ticket-ordered. Empty when nothing is salvageable.
std::vector<Slot> salvage_slots(const void* base, uint64_t file_size,
                                RepairReport* report = nullptr);

const char* to_string(HeaderState s);

} // namespace core
} // namespace lg
