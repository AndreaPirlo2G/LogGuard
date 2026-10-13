#pragma once
// Cross-event sequence rules.
//
// A threshold rule answers "how many times did X happen". Many real faults are
// *chains*: e.g. "queue drained -> reconnect started -> reconnect timed out ->
// gave up", or "auth refresh failed, then an outbound call used a stale token".
// A SequenceRule fires only when an ordered list of steps is observed within a
// sliding time window. Missing or out-of-order steps quietly reset the matcher.
//
// The matcher is pure logic: callers pass an explicit timestamp so it is unit
// testable without threads or sleeps.
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace lg::diag {

struct SeqStep {
    uint16_t min_level = 0; // event level must be >= this
    std::string category;   // "" matches any category
    std::string substr;     // "" matches any message; otherwise substring

    SeqStep() = default;
    SeqStep(uint16_t lvl, std::string cat, std::string needle)
        : min_level(lvl), category(std::move(cat)),
          substr(std::move(needle)) {}

    bool matches(uint16_t level, const char* cat, const char* msg) const;
};

class SequenceRule {
public:
    SequenceRule() = default;
    SequenceRule(std::vector<SeqStep> steps, int64_t window_ns,
                 int64_t cooldown_ns);

    // Feed one event at monotonic time now_ns. Returns true exactly once when
    // the whole ordered chain completes inside the window (and the cooldown has
    // elapsed since the previous completion).
    bool observe(uint16_t level, const char* category, const char* msg,
                 int64_t now_ns);

    void reset() { pos_ = 0; chain_start_ns_ = 0; }
    std::size_t size() const { return steps_.size(); }
    int fire_count() const { return fires_; }

private:
    std::vector<SeqStep> steps_;
    int64_t window_ns_ = 0;
    int64_t cooldown_ns_ = 0;
    std::size_t pos_ = 0;          // index of the next expected step
    int64_t chain_start_ns_ = 0;   // timestamp of the first matched step
    int64_t last_fire_ns_ = 0;
    int fires_ = 0;
};

} // namespace lg::diag
