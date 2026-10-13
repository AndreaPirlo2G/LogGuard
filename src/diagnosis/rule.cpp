#include "diagnosis/rule.hpp"

#include <cstring>
#include <string_view>

namespace lg::diag {

bool Rule::matches(uint16_t level, const char* cat, const char* msg) const
    noexcept {
    if (level < min_level) return false;
    if (!category.empty()) {
        const char* c = cat ? cat : "";
        if (category != c) return false;
    }
    if (!substr.empty()) {
        if (!msg || !std::strstr(msg, substr.c_str())) return false;
    }
    return true;
}

bool Rule::observe(uint16_t level, const char* cat, const char* msg,
                   int64_t now_ns) noexcept {
    if (!matches(level, cat, msg)) return false;

    if (count == 0 || now_ns - first_ns > window_ns) {
        first_ns = now_ns;
        count = 0;
    }
    ++count;

    const int need = threshold > 0 ? threshold : 1;
    // last_fire_ns == 0 means "never fired": the cooldown must not suppress the
    // very first fire even when the injected/mono clock is a small value.
    const bool cooled = (last_fire_ns == 0) ||
                        (now_ns - last_fire_ns > cooldown_ns);
    if (count >= need && cooled) {
        last_fire_ns = now_ns;
        count = 0;
        return true; // fire
    }
    return false;
}

} // namespace lg::diag
