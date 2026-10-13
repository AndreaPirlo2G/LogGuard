#include "core/ring_audit.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_set>

#include "core/recovery.hpp"

namespace lg {
namespace core {

namespace {

// A slot is "torn" when its magic says published but the payload metadata is
// internally impossible (lengths exceed their caps). Such a slot results from
// an interrupted multi-byte write caught between the body store and the release
// of magic; the reader must skip it rather than trust the lengths.
bool lengths_plausible(const Slot& s) {
    if (s.msg_len > PAYLOAD_CAP) return false;
    if (s.cat_len > CATEGORY_CAP) return false;
    if (s.req_len > REQ_CAP) return false;
    if (s.level > 5) return false;
    // msg_len must cover the advertised category/request prefixes. Payload
    // layout is [cat\0?][req\0?][message]; the header lengths describe it.
    return true;
}

} // namespace

RingAudit audit_ring(const void* base, uint64_t capacity) {
    RingAudit a;
    a.capacity = capacity;
    if (base == nullptr || capacity == 0) {
        a.capacity = 0;
        return a;
    }

    const FileHeader& h = *header_at(base);
    a.header_ok = std::memcmp(h.magic, RING_FILE_MAGIC, 8) == 0;
    a.header_epoch = h.epoch;
    a.crashed = h.crashed != 0;
    // Trust the file's own declared capacity if it is sane, else the caller's.
    if (a.header_ok && h.slot_count > 0 && h.slot_count <= capacity)
        a.capacity = h.slot_count;

    const uint64_t mask = a.capacity - 1;
    bool have_tickets = false;
    std::unordered_set<uint64_t> tickets;
    tickets.reserve(256);

    for (uint64_t i = 0; i < a.capacity; ++i) {
        const Slot* s = slot_at(base, i);
        uint32_t magic = __atomic_load_n(&s->magic, __ATOMIC_ACQUIRE);
        if (magic == 0) {
            ++a.empty;
            continue;
        }
        if (magic != RING_MAGIC_NUMBER) {
            ++a.torn; // garbage magic that is neither 0 nor published value
            continue;
        }
        // Copy defensively; an overwrite during copy would flip fields.
        Slot c;
        std::memcpy(&c, s, sizeof(c));
        if (c.magic != RING_MAGIC_NUMBER) {
            ++a.torn;
            continue;
        }
        if ((c.ticket & mask) != i) {
            ++a.ticket_mismatch;
            continue;
        }
        if (!lengths_plausible(c)) {
            ++a.torn;
            continue;
        }
        if (c.epoch != a.header_epoch) {
            ++a.foreign_epoch;
            continue;
        }
        ++a.valid;
        tickets.insert(c.ticket);
        if (!have_tickets) {
            a.min_ticket = a.max_ticket = c.ticket;
            have_tickets = true;
        } else {
            a.min_ticket = std::min(a.min_ticket, c.ticket);
            a.max_ticket = std::max(a.max_ticket, c.ticket);
        }
    }

    a.distinct_tickets = tickets.size();
    return a;
}

} // namespace core
} // namespace lg
