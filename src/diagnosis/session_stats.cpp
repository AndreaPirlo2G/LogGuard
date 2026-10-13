#include "diagnosis/session_stats.hpp"

#include "core/ring_flags.hpp"

#include <algorithm>
#include <cstdio>
#include <unordered_set>

namespace lg {
namespace diag {

using lg::core::Slot;

namespace {

constexpr const char* kLevelNames[SessionStats::kLevels] = {
    "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "CRITICAL"};

// Pull the fixed-width category field; Slot stores it NUL/space padded.
std::string slot_category(const Slot& s) {
    if (s.cat_len == 0) return std::string();
    uint32_t n = std::min<uint32_t>(s.cat_len,
                                    static_cast<uint32_t>(sizeof(s.category)));
    return std::string(reinterpret_cast<const char*>(s.category), n);
}

std::string slot_request(const Slot& s) {
    if (s.req_len == 0) return std::string();
    uint32_t n = std::min<uint32_t>(s.req_len,
                                    static_cast<uint32_t>(sizeof(s.req_id)));
    return std::string(reinterpret_cast<const char*>(s.req_id), n);
}

} // namespace

const char* level_name(int level) {
    if (level < 0 || level >= SessionStats::kLevels) return "?";
    return kLevelNames[level];
}

SessionStats aggregate(const std::vector<Slot>& slots) {
    SessionStats st;
    if (slots.empty()) return st;

    // Work on a mono-sorted copy so span and rate buckets are meaningful even
    // if callers hand us slots gathered out of order.
    std::vector<const Slot*> ordered;
    ordered.reserve(slots.size());
    for (const Slot& s : slots) ordered.push_back(&s);
    std::sort(ordered.begin(), ordered.end(),
              [](const Slot* a, const Slot* b) {
                  if (a->mono_ns != b->mono_ns) return a->mono_ns < b->mono_ns;
                  return a->ticket < b->ticket;
              });

    st.first_mono_ns = ordered.front()->mono_ns;
    st.last_mono_ns = ordered.back()->mono_ns;
    st.span_mono_ns = std::max<int64_t>(0, st.last_mono_ns - st.first_mono_ns);

    std::unordered_set<std::string> requests;

    // 1-second peak bucket keyed relative to the first event.
    std::unordered_map<int64_t, uint64_t> per_second;
    per_second.reserve(64);

    for (const Slot* sp : ordered) {
        const Slot& s = *sp;
        ++st.total;
        if (s.level < SessionStats::kLevels)
            ++st.by_level[static_cast<size_t>(s.level)];
        std::string cat = slot_category(s);
        if (!cat.empty()) ++st.by_category[cat];

        if (s.flags & lg::core::FLAG_STACK) ++st.stack_events;
        if (s.flags & lg::core::FLAG_KV) ++st.kv_events;
        if (s.flags & lg::core::FLAG_LAZY) ++st.lazy_events;

        std::string req = slot_request(s);
        if (!req.empty()) requests.insert(req);

        int64_t sec = (s.mono_ns - st.first_mono_ns) / 1'000'000'000LL;
        uint64_t v = ++per_second[sec];
        if (v > st.peak_per_second) st.peak_per_second = v;
    }

    st.distinct_requests = requests.size();
    if (st.span_mono_ns > 0)
        st.avg_per_second =
            static_cast<double>(st.total) /
            (static_cast<double>(st.span_mono_ns) / 1e9);
    return st;
}

std::string render_stats(const SessionStats& s) {
    std::string o;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "records:      %llu\n",
                  (unsigned long long)s.total);
    o += buf;
    o += "levels:";
    for (int l = 0; l < SessionStats::kLevels; ++l) {
        std::snprintf(buf, sizeof(buf), " %s=%llu", kLevelNames[l],
                      (unsigned long long)s.by_level[l]);
        o += buf;
    }
    o += '\n';

    // Deterministic category ordering (sorted) for stable CLI output.
    std::vector<std::pair<std::string, uint64_t>> cats(s.by_category.begin(),
                                                       s.by_category.end());
    std::sort(cats.begin(), cats.end(),
              [](const auto& a, const auto& b) {
                  if (a.second != b.second) return a.second > b.second;
                  return a.first < b.first;
              });
    o += "categories:";
    for (const auto& kv : cats) {
        o += ' ';
        o += kv.first;
        o += '=';
        o += std::to_string(kv.second);
    }
    o += '\n';

    std::snprintf(buf, sizeof(buf),
                  "span_ms:      %.2f\n"
                  "rate:         avg=%.1f/s peak=%llu/s\n"
                  "warn+:        %llu (%.1f%%)\n"
                  "events:       stack=%llu kv=%llu lazy=%llu\n"
                  "requests:     %llu distinct\n",
                  s.span_mono_ns / 1e6, s.avg_per_second,
                  (unsigned long long)s.peak_per_second,
                  (unsigned long long)s.warn_or_worse(),
                  s.warn_ratio() * 100.0, (unsigned long long)s.stack_events,
                  (unsigned long long)s.kv_events,
                  (unsigned long long)s.lazy_events,
                  (unsigned long long)s.distinct_requests);
    o += buf;
    return o;
}

std::string render_stats_json(const SessionStats& s) {
    std::string o = "{";
    o += "\"total\":" + std::to_string(s.total) + ",\"levels\":{";
    for (int l = 0; l < SessionStats::kLevels; ++l) {
        if (l) o += ',';
        o += '"';
        o += kLevelNames[l];
        o += "\":" + std::to_string(s.by_level[l]);
    }
    o += "},\"categories\":{";
    bool first = true;
    for (const auto& kv : s.by_category) {
        if (!first) o += ',';
        first = false;
        o += '"' + kv.first + "\":" + std::to_string(kv.second);
    }
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "},\"span_ns\":%lld,\"avg_per_sec\":%.2f,"
                  "\"peak_per_sec\":%llu,\"warn_or_worse\":%llu,"
                  "\"stack\":%llu,\"kv\":%llu,\"lazy\":%llu,"
                  "\"distinct_requests\":%llu}",
                  (long long)s.span_mono_ns, s.avg_per_second,
                  (unsigned long long)s.peak_per_second,
                  (unsigned long long)s.warn_or_worse(),
                  (unsigned long long)s.stack_events,
                  (unsigned long long)s.kv_events,
                  (unsigned long long)s.lazy_events,
                  (unsigned long long)s.distinct_requests);
    o += buf;
    return o;
}

} // namespace diag
} // namespace lg
