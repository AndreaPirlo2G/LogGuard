#include "capture/recorder_config.hpp"

#include <algorithm>
#include <string>

#include "diagnosis/rule_engine.hpp"
#include "platform/file_util.hpp"

namespace lg::cap {

namespace {

// Smallest power of two >= v, clamped into [lo, hi].
uint32_t round_up_pow2_clamped(uint32_t v, uint32_t lo, uint32_t hi) {
    uint32_t p = 1;
    while (p < v && p < hi) p <<= 1;
    if (p < lo) p = lo;
    if (p > hi) p = hi;
    return p;
}

} // namespace

bool RecorderConfig::normalize(std::string* error) {
    if (ring_path.empty()) {
        if (error) *error = "ring_path must not be empty";
        return false;
    }

    static constexpr uint32_t kMinSlots = 1024u;
    static constexpr uint32_t kMaxSlots = 1u << 26; // ~17 GiB worst case cap
    ring_slots = round_up_pow2_clamped(ring_slots, kMinSlots, kMaxSlots);

    if (level > 5) level = 5;

    if (stall_threshold_ms < 0) stall_threshold_ms = 0;
    if (stall_threshold_ms > 24 * 60 * 60 * 1000)
        stall_threshold_ms = 24 * 60 * 60 * 1000;

    if (burst_suppression) {
        if (burst_window_ms < 1) burst_window_ms = 1;
        if (burst_window_ms > 60 * 1000) burst_window_ms = 60 * 1000;
    }

    // Retention: at least keep one package; clamp the age bound to a year.
    if (retain_max_packages == 0) retain_max_packages = 1;
    if (retain_max_packages > 100000) retain_max_packages = 100000;
    if (retain_max_mb > 1ull << 30) retain_max_mb = 1ull << 30;
    if (retain_max_age_hours < 0) retain_max_age_hours = 0;
    if (retain_max_age_hours > 24 * 365) retain_max_age_hours = 24 * 365;

    if (output_dir.empty()) output_dir = lg::plat::parent_dir(ring_path);

    // Keep the rule table bounded; extras are ignored deterministically rather
    // than silently blowing the fixed rule-array at runtime.
    if (initial_rules.size() > lg::diag::RuleEngine::kMaxRules)
        initial_rules.resize(lg::diag::RuleEngine::kMaxRules);

    return true;
}

RecorderConfig RecorderConfig::for_path(const std::string& path) {
    RecorderConfig c;
    c.ring_path = path;
    c.normalize();
    return c;
}

} // namespace lg::cap
