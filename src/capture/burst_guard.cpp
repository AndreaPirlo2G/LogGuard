#include "capture/burst_guard.hpp"

namespace lg {
namespace cap {

BurstDecision BurstGuard::summarize(const std::string& key) {
    BurstDecision d;
    d.emit_current = false; // this is a flush, not a new event
    auto it = state_.find(key);
    if (it != state_.end() && it->second.active) {
        d.pending_summary = it->second.suppressed;
        if (d.pending_summary > 0) d.summary_key = key;
        it->second.active = false;
        it->second.suppressed = 0;
    }
    return d;
}

BurstDecision BurstGuard::on_event(const std::string& key, uint64_t now_ns) {
    BurstState& st = state_[key];

    if (st.active && now_ns - st.first_seen_ns >= window_ns_) {
        // The previous run's window elapsed. Settle it: emit its pending
        // summary (if any), then open a fresh run for the current event.
        BurstDecision d = summarize(key);
        d.emit_current = true;
        // Reopen for the current event.
        BurstState& again = state_[key];
        again.active = true;
        again.first_seen_ns = now_ns;
        again.last_seen_ns = now_ns;
        again.suppressed = 0;
        return d;
    }

    if (!st.active) {
        // Start a fresh run; this first event always passes.
        st.active = true;
        st.first_seen_ns = now_ns;
        st.last_seen_ns = now_ns;
        st.suppressed = 0;
        return BurstDecision{true, 0, std::string()};
    }

    // Inside the suppression window: swallow the duplicate.
    st.last_seen_ns = now_ns;
    ++st.suppressed;
    ++total_suppressed_;
    return BurstDecision{false, 0, std::string()};
}

BurstDecision BurstGuard::drain(uint64_t now_ns) {
    for (auto& kv : state_) {
        BurstState& st = kv.second;
        if (st.active && st.suppressed > 0 &&
            now_ns - st.first_seen_ns >= window_ns_) {
            return summarize(kv.first);
        }
    }
    return BurstDecision{false, 0, std::string()};
}

} // namespace cap
} // namespace lg
