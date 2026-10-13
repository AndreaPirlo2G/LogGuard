#include "diagnosis/sequence_rule.hpp"

#include <cstring>

namespace lg::diag {

bool SeqStep::matches(uint16_t level, const char* cat, const char* msg) const {
    if (level < min_level) return false;
    if (!category.empty() && (cat == nullptr || category != std::string(cat)))
        return false;
    if (!substr.empty()) {
        std::string body = msg ? msg : "";
        if (body.find(substr) == std::string::npos) return false;
    }
    return true;
}

SequenceRule::SequenceRule(std::vector<SeqStep> steps, int64_t window_ns,
                           int64_t cooldown_ns)
    : steps_(std::move(steps)), window_ns_(window_ns),
      cooldown_ns_(cooldown_ns) {}

bool SequenceRule::observe(uint16_t level, const char* category,
                           const char* msg, int64_t now_ns) {
    if (steps_.empty()) return false;

    // Expire a partial chain if the inter-arrival window has elapsed.
    if (pos_ > 0 && window_ns_ > 0 && now_ns - chain_start_ns_ > window_ns_)
        reset();

    const SeqStep& expected = steps_[pos_];
    if (expected.matches(level, category, msg)) {
        if (pos_ == 0) chain_start_ns_ = now_ns;
        ++pos_;
        if (pos_ == steps_.size()) {
            pos_ = 0; // chain complete; arm for a fresh chain
            bool cooled = last_fire_ns_ == 0 ||
                          (now_ns - last_fire_ns_) > cooldown_ns_;
            chain_start_ns_ = 0;
            if (cooled) {
                last_fire_ns_ = now_ns;
                ++fires_;
                return true;
            }
        }
        return false;
    }

    // An unrelated event does not reset a chain, but an event matching step 0
    // re-roots the chain here (common when steps repeat before the tail).
    if (pos_ > 0 && steps_[0].matches(level, category, msg)) {
        pos_ = 1;
        chain_start_ns_ = now_ns;
    }
    return false;
}

} // namespace lg::diag
