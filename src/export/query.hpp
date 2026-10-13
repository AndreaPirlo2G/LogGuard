// Structured query over committed ring slots.
//
// Recovery produces a flat list of slots; on-site diagnosis needs to slice
// that list: "all ERROR+ from the 'planning' category for request 0x7f in the
// last 2 seconds whose message mentions 'timeout'". QuerySpec encodes such a
// filter with pure, side-effect-free AND semantics so it can be unit-tested
// without a file and reused by lgtool and the C API.
//
// The filter runs over the storage-level core::Slot (which carries timestamp,
// tid and the fixed category/request fields); rendering to text is a separate
// step, so time/id predicates never depend on JSON formatting.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/slot_format.hpp"

namespace lg {
namespace export_ {

struct QuerySpec {
    // Inclusive monotonic-time window in nanoseconds. from_ns/to_ns==0
    // disables that bound.
    uint64_t from_ns = 0;
    uint64_t to_ns = 0;

    // Minimum severity (0..5); records below are excluded. -1 disables.
    int min_level = -1;

    // Exact-category match when non-empty.
    std::string category;

    // Exact request-id correlation when non-empty.
    std::string request_id;

    // Filter on a specific thread when non-zero.
    uint64_t tid = 0;

    // Case-sensitive substring match against the rendered text (message for
    // plain slots, plus rendered stack/kv/lazy text). Empty disables. When
    // match_all_tokens is true this is split on whitespace and every token
    // must appear (AND). No regex engine is used.
    std::string contains;
    bool match_all_tokens = false;

    bool enabled() const {
        return from_ns != 0 || to_ns != 0 || min_level >= 0 ||
               !category.empty() || !request_id.empty() || tid != 0 ||
               !contains.empty();
    }
};

struct QueryHit {
    size_t index; // index into the input collection
    lg::core::Slot slot;
};

// Return true if a single slot satisfies every active clause.
bool matches(const lg::core::Slot& s, const QuerySpec& q);

// Apply the spec to a collection, preserving input order.
std::vector<QueryHit> query(const std::vector<lg::core::Slot>& slots,
                            const QuerySpec& q);

// Parse a compact expression used by the CLI:
//   "level>=ERROR category=planning req=0x7f tid=12345 text=timeout"
// A bare word is treated as a text= substring. Unknown keys are ignored.
// Returns false only on a malformed level clause.
bool parse_query(const std::string& expr, QuerySpec& out,
                 std::string* error = nullptr);

// Single-line rendering of one hit (uses the shared slot renderer).
std::string render_hit(const QueryHit& h);

} // namespace export_
} // namespace lg
