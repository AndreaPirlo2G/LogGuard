#pragma once
// Session statistics aggregation.
//
// Folds the recovered/live slot window into a compact, human- and
// machine-readable summary: per-level and per-category counts, the record rate
// (peak and average), the time span, and how much the ring was forced to
// overwrite itself. This is pure over a slot list so it is shared by the live
// health path, the offline CLI and unit tests.
#include "core/slot_format.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace lg {
namespace diag {

struct SessionStats {
    static constexpr int kLevels = 6; // TRACE..CRITICAL

    uint64_t total = 0;
    std::array<uint64_t, kLevels> by_level{};
    std::unordered_map<std::string, uint64_t> by_category;

    // Monotonic-clock span of the observed window (ns). Zero when < 2 events.
    int64_t span_mono_ns = 0;
    int64_t first_mono_ns = 0;
    int64_t last_mono_ns = 0;

    // Peak records-per-second measured in 1-second buckets over mono time.
    uint64_t peak_per_second = 0;
    double avg_per_second = 0.0;

    // Slot flag tallies (see core/ring_flags.hpp).
    uint64_t stack_events = 0;
    uint64_t kv_events = 0;
    uint64_t lazy_events = 0;

    // Correlation breadth: distinct request ids seen (empty id excluded).
    uint64_t distinct_requests = 0;

    uint64_t count_level(int level) const {
        if (level < 0 || level >= kLevels) return 0;
        return by_level[static_cast<size_t>(level)];
    }
    uint64_t warn_or_worse() const {
        uint64_t n = 0;
        for (int l = 3; l < kLevels; ++l) n += by_level[static_cast<size_t>(l)];
        return n;
    }
    double warn_ratio() const {
        return total == 0 ? 0.0
                          : static_cast<double>(warn_or_worse()) /
                                static_cast<double>(total);
    }
};

// Fold one ticket-ordered (or unordered; sorting is handled internally when
// requested) slot window.
SessionStats aggregate(const std::vector<lg::core::Slot>& slots);

// Render the summary as aligned key/value lines for the CLI.
std::string render_stats(const SessionStats& s);

// Render as a compact JSON object (no external JSON dependency).
std::string render_stats_json(const SessionStats& s);

const char* level_name(int level);

} // namespace diag
} // namespace lg
