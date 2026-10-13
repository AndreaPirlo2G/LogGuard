#include "core/ring_repair.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_map>

#include "core/constants.hpp"
#include "core/recovery.hpp"

namespace lg {
namespace core {

namespace {

bool is_power_of_two(uint64_t v) { return v && (v & (v - 1)) == 0; }

// A copied slot is internally consistent (lengths inside their caps, level in
// range) and its ticket maps to the physical index under the ring mask.
bool slot_consistent(const Slot& s, uint64_t index, uint64_t mask) {
    if (s.msg_len > PAYLOAD_CAP) return false;
    if (s.cat_len > CATEGORY_CAP) return false;
    if (s.req_len > REQ_CAP) return false;
    if (s.level > 5) return false;
    if ((s.ticket & mask) != index) return false;
    return true;
}

bool header_page_all_zero(const FileHeader& h) {
    const auto* p = reinterpret_cast<const uint8_t*>(&h);
    for (size_t i = 0; i < HEADER_SIZE; ++i)
        if (p[i] != 0) return false;
    return true;
}

} // namespace

uint64_t infer_capacity(uint64_t file_size) {
    if (file_size < HEADER_SIZE + SLOT_SIZE) return 0;
    uint64_t slots = (file_size - HEADER_SIZE) / SLOT_SIZE;
    // Ring indexing requires a power-of-two capacity. Round down.
    uint64_t cap = 1;
    while ((cap << 1) <= slots && cap < (1ull << 40)) cap <<= 1;
    return cap;
}

const char* to_string(HeaderState s) {
    switch (s) {
    case HeaderState::FreshZero: return "fresh-zero";
    case HeaderState::Valid: return "valid";
    case HeaderState::GeometryMismatch: return "geometry-mismatch";
    case HeaderState::CorruptWithData: return "corrupt-with-data";
    case HeaderState::CorruptEmpty: return "corrupt-empty";
    }
    return "unknown";
}

RepairReport diagnose(const void* base, uint64_t file_size) {
    RepairReport rep;
    rep.file_capacity = infer_capacity(file_size);
    if (base == nullptr || rep.file_capacity == 0) {
        rep.state = HeaderState::CorruptEmpty;
        return rep;
    }

    const FileHeader& h = *header_at(base);
    const bool magic_ok =
        std::memcmp(h.magic, RING_FILE_MAGIC, sizeof(RING_FILE_MAGIC)) == 0;

    if (magic_ok) {
        rep.header_readable = true;
        rep.header_capacity = h.slot_count;
        if (h.slot_count == rep.file_capacity && is_power_of_two(h.slot_count)) {
            rep.state = HeaderState::Valid;
        } else {
            rep.state = HeaderState::GeometryMismatch;
        }
        return rep;
    }

    // Bad magic. Distinguish a never-initialised file from a torn/corrupt one
    // by looking at the arena: any slot carrying the per-slot published magic
    // means data existed. Also check whether the header page is merely zero.
    if (header_page_all_zero(h)) {
        rep.state = HeaderState::FreshZero;
        return rep;
    }

    uint64_t intact = 0;
    const uint64_t mask = rep.file_capacity - 1;
    for (uint64_t i = 0; i < rep.file_capacity; ++i) {
        const Slot* s = slot_at(base, i);
        uint32_t magic = __atomic_load_n(&s->magic, __ATOMIC_ACQUIRE);
        if (magic == RING_MAGIC_NUMBER) {
            Slot c;
            std::memcpy(&c, s, sizeof(c));
            if (c.magic == RING_MAGIC_NUMBER && slot_consistent(c, i, mask))
                ++intact;
        }
    }
    rep.state = intact > 0 ? HeaderState::CorruptWithData
                           : HeaderState::CorruptEmpty;
    return rep;
}

std::vector<Slot> salvage_slots(const void* base, uint64_t file_size,
                                RepairReport* report) {
    RepairReport local;
    RepairReport& rep = report ? *report : local;
    rep = diagnose(base, file_size);

    std::vector<Slot> out;
    if (base == nullptr || rep.file_capacity == 0) return out;

    const uint64_t mask = rep.file_capacity - 1;

    // Pass 1: gather consistent published slots, tally epochs and torn slots.
    std::unordered_map<uint32_t, uint64_t> epoch_counts;
    struct Item {
        Slot slot;
        uint32_t epoch;
    };
    std::vector<Item> items;
    items.reserve(256);

    for (uint64_t i = 0; i < rep.file_capacity; ++i) {
        const Slot* s = slot_at(base, i);
        uint32_t magic = __atomic_load_n(&s->magic, __ATOMIC_ACQUIRE);
        if (magic == 0) continue;
        Slot c;
        std::memcpy(&c, s, sizeof(c));
        if (c.magic != RING_MAGIC_NUMBER || !slot_consistent(c, i, mask)) {
            if (magic != 0) ++rep.torn;
            continue;
        }
        items.push_back({c, c.epoch});
        ++epoch_counts[c.epoch];
    }

    if (items.empty()) return out;

    // The dominant epoch is almost certainly the last live session: a ring that
    // wrapped keeps the newest epoch's slots; a retired older epoch leaves at
    // most a handful of stragglers. Ties go to the higher epoch number.
    uint32_t best_epoch = 0;
    uint64_t best_count = 0;
    for (const auto& kv : epoch_counts) {
        if (kv.second > best_count ||
            (kv.second == best_count && kv.first > best_epoch)) {
            best_count = kv.second;
            best_epoch = kv.first;
        }
    }
    rep.recovered_epoch = best_epoch;
    rep.salvageable = best_count;

    for (const auto& it : items)
        if (it.epoch == best_epoch) out.push_back(it.slot);

    std::sort(out.begin(), out.end(),
              [](const Slot& a, const Slot& b) { return a.ticket < b.ticket; });
    return out;
}

} // namespace core
} // namespace lg
