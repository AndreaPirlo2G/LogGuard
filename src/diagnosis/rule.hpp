#pragma once
// Declarative trigger rule.
//
// A rule matches an event when its level is at/above `min_level`, the event
// category equals `category` (when non-empty) and its message contains
// `substr` (when non-empty). Matched events are counted inside a sliding
// `window_ns`; once the count reaches `threshold` the rule fires, then stays
// silent for `cooldown_ns` to avoid snapshot storms.
#include <cstdint>
#include <string>

namespace lg::diag {

struct Rule {
    uint16_t min_level = 4;                 // inclusive
    std::string category;                   // empty matches any
    std::string substr;                     // empty matches any
    int threshold = 1;
    int64_t window_ns   = 1'000'000'000LL;  // counting window
    int64_t cooldown_ns = 10'000'000'000LL; // silence after a fire

    // Mutable evaluation state.
    int count = 0;
    int64_t first_ns = 0;
    int64_t last_fire_ns = 0;

    // Pure predicate: does this rule match the event's static attributes?
    bool matches(uint16_t level, const char* cat, const char* msg) const
        noexcept;

    // Fold one event at time `now_ns`; returns true exactly when this event
    // crosses the threshold and the cooldown has elapsed (and arms a fire).
    bool observe(uint16_t level, const char* cat, const char* msg,
                 int64_t now_ns) noexcept;

    // Reset the sliding window/count state (used by tests and reconfiguration).
    void reset() noexcept { count = 0; first_ns = 0; last_fire_ns = 0; }
};

} // namespace lg::diag
