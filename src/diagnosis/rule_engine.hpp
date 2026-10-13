#pragma once
// Thread-safe collection of trigger rules.
//
// The recorder hands every committed event to evaluate(); the engine owns
// matching, sliding-window counts and per-rule cooldown, and reports whether
// any rule fired so the recorder can enqueue a single snapshot. Firing policy
// (what a snapshot is, where it goes) stays in the recorder; the engine only
// decides *when*.
#include "diagnosis/rule.hpp"

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace lg::diag {

class RuleEngine {
public:
    using ClockFn = std::function<int64_t()>;

    static constexpr std::size_t kMaxRules = 32;

    explicit RuleEngine(ClockFn clock = {});

    // Add a rule. Returns its index, or -1 when the table is full.
    int add(const Rule& rule);

    // Remove all rules.
    void clear();

    std::size_t size() const;

    // Fold one event. Returns the number of rules that fired on this event
    // (normally 0 or 1). Thread-safe.
    int evaluate(uint16_t level, const char* category, const char* msg);

    // Number of rule fires since creation (diagnostics/health counters).
    uint64_t fire_count() const noexcept;

private:
    ClockFn clock_;
    mutable std::mutex mtx_;
    std::vector<Rule> rules_;
    uint64_t fires_ = 0;
};

} // namespace lg::diag
