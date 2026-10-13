// LogGuard burst suppression.
//
// A runaway loop can emit the same message thousands of times per second and
// evict genuinely useful records from the ring. BurstGuard deduplicates a
// consecutive run of identical keys inside a suppression window: the first
// event passes, the rest are counted, and when the window elapses (or a later
// copy arrives after the window) a single summary carrying the suppressed
// count is released, rendered by the caller as e.g.
// "previous message repeated N times".
//
// The clock is injected so the policy is fully unit-testable without sleeping.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace lg {
namespace cap {

// Result of feeding one event to the guard.
struct BurstDecision {
    // True when the current event itself should be appended to the ring.
    bool emit_current = true;
    // Number of earlier copies suppressed before this event whose summary has
    // now come due. When >0 the caller should append one summary line (keyed by
    // summary_key) before appending the current event.
    uint64_t pending_summary = 0;
    std::string summary_key;
};

// Per-key suppression state.
struct BurstState {
    uint64_t first_seen_ns = 0;   // timestamp of the run's first (emitted) event
    uint64_t last_seen_ns = 0;    // most recent event in the run
    uint64_t suppressed = 0;      // swallowed copies in the current run
    bool active = false;          // a run is open (an event was emitted)
};

class BurstGuard {
public:
    explicit BurstGuard(uint64_t window_ns = kDefaultWindowNs)
        : window_ns_(window_ns) {}

    // Feed one event identified by `key` (typically category + '\x1f' + msg).
    BurstDecision on_event(const std::string& key, uint64_t now_ns);

    // Flush one open run whose window has elapsed. Returns a decision whose
    // emit_current is false but whose pending_summary>0 for an expired run.
    // Call repeatedly until pending_summary==0 to drain all pending summaries;
    // intended for a periodic worker.
    BurstDecision drain(uint64_t now_ns);

    // Number of keys currently tracked.
    size_t tracked_keys() const { return state_.size(); }

    // Total suppressed events since creation (observability counter).
    uint64_t total_suppressed() const { return total_suppressed_; }

    void set_window_ns(uint64_t ns) { window_ns_ = ns ? ns : 1; }
    uint64_t window_ns() const { return window_ns_; }

    // Drop all state (e.g. on ring reset).
    void clear() { state_.clear(); }

    static constexpr uint64_t kDefaultWindowNs = 100ull * 1000000ull; // 100 ms

private:
    // Build a summary decision for an open run of `key` and mark it closed.
    BurstDecision summarize(const std::string& key);

    uint64_t window_ns_;
    std::unordered_map<std::string, BurstState> state_;
    uint64_t total_suppressed_ = 0;
};

} // namespace cap
} // namespace lg
