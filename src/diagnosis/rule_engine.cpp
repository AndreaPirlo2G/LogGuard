#include "diagnosis/rule_engine.hpp"

#include "platform/clock.hpp"

namespace lg::diag {

RuleEngine::RuleEngine(ClockFn clock)
    : clock_(clock ? std::move(clock)
                   : [] { return lg::plat::mono_now_ns(); }) {}

int RuleEngine::add(const Rule& rule) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (rules_.size() >= kMaxRules) return -1;
    rules_.push_back(rule);
    return static_cast<int>(rules_.size()) - 1;
}

void RuleEngine::clear() {
    std::lock_guard<std::mutex> lk(mtx_);
    rules_.clear();
}

std::size_t RuleEngine::size() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return rules_.size();
}

int RuleEngine::evaluate(uint16_t level, const char* category, const char* msg) {
    const int64_t now = clock_();
    int fired = 0;
    std::lock_guard<std::mutex> lk(mtx_);
    for (Rule& r : rules_) {
        if (r.observe(level, category, msg, now)) ++fired;
    }
    fires_ += static_cast<uint64_t>(fired);
    return fired;
}

uint64_t RuleEngine::fire_count() const noexcept {
    // fires_ is only mutated under mtx_ in evaluate(); read under the same lock
    // for a consistent snapshot.
    std::lock_guard<std::mutex> lk(mtx_);
    return fires_;
}

} // namespace lg::diag
