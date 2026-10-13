#pragma once
// Read path: collect the current epoch's slots into a stable, ticket-ordered
// snapshot. Pure wrt. the mapping — it never mutates slots, so it is safe to
// call from the offline exporter against a foreign (even live) ring.
#include "slot_format.hpp"

#include <cstring>
#include <vector>

namespace lg::core {

// Copy out all slots that belong to `epoch` and pass the integrity checks:
//  - magic must be the published value (acquire),
//  - the copied slot must still be consistent (no overwrite during copy),
//  - the slot's masked ticket must match its physical index.
inline std::vector<Slot> collect_epoch(const void* base, uint64_t slot_count,
                                       uint32_t epoch) {
    std::vector<Slot> out;
    out.reserve(slot_count < 1024 ? static_cast<size_t>(slot_count) : 1024);
    const uint64_t mask = slot_count - 1;
    for (uint64_t i = 0; i < slot_count; ++i) {
        const Slot* s = slot_at(base, i);
        if (__atomic_load_n(&s->magic, __ATOMIC_ACQUIRE) != RING_MAGIC_NUMBER)
            continue;
        Slot c;
        std::memcpy(&c, s, sizeof(c));
        if (c.magic != RING_MAGIC_NUMBER || c.epoch != epoch) continue;
        if ((c.ticket & mask) != i) continue;
        out.push_back(c);
    }
    // Simple sort; the ring is scanned on a background thread, not the hot path.
    for (size_t a = 1; a < out.size(); ++a) {
        Slot key = out[a];
        size_t b = a;
        while (b > 0 && out[b - 1].ticket > key.ticket) { out[b] = out[b - 1]; --b; }
        out[b] = key;
    }
    return out;
}

} // namespace lg::core
