// Session statistics aggregation tests.
#include <cstdint>
#include <cstring>
#include <vector>

#include "lgtest.hpp"
#include "diagnosis/session_stats.hpp"
#include "core/ring_flags.hpp"

using namespace lg::diag;

namespace {

using lg::core::FLAG_KV;
using lg::core::FLAG_LAZY;
using lg::core::FLAG_STACK;

lg::core::Slot make_slot(int level, const char* cat, const char* req,
                         int64_t mono, uint8_t flags = 0) {
    lg::core::Slot s;
    std::memset(&s, 0, sizeof(s));
    s.level = static_cast<uint16_t>(level);
    s.mono_ns = mono;
    s.flags = flags;
    if (cat) {
        size_t n = std::strlen(cat);
        std::memcpy(s.category, cat, n);
        s.cat_len = static_cast<uint16_t>(n);
    }
    if (req) {
        size_t n = std::strlen(req);
        std::memcpy(s.req_id, req, n);
        s.req_len = static_cast<uint16_t>(n);
    }
    return s;
}

void test_counts() {
    LG_CASE("total and per-level/category counts");
    std::vector<lg::core::Slot> v;
    v.push_back(make_slot(2, "net", "r1", 10));
    v.push_back(make_slot(3, "net", "r2", 20));
    v.push_back(make_slot(4, "gpu", "r1", 30));
    v.push_back(make_slot(5, "gpu", "", 40));
    auto s = aggregate(v);
    CHECK_EQ(s.total, (uint64_t)4);
    CHECK_EQ(s.count_level(2), (uint64_t)1);
    CHECK_EQ(s.count_level(3), (uint64_t)1);
    CHECK_EQ(s.count_level(4), (uint64_t)1);
    CHECK_EQ(s.count_level(5), (uint64_t)1);
    CHECK_EQ(s.by_category.at("net"), (uint64_t)2);
    CHECK_EQ(s.by_category.at("gpu"), (uint64_t)2);
    CHECK_EQ(s.warn_or_worse(), (uint64_t)3);
}

void test_request_breadth() {
    LG_CASE("distinct requests counted, empty excluded");
    std::vector<lg::core::Slot> v;
    v.push_back(make_slot(2, "a", "tx-1", 1));
    v.push_back(make_slot(2, "a", "tx-1", 2));
    v.push_back(make_slot(2, "a", "tx-2", 3));
    v.push_back(make_slot(2, "a", nullptr, 4));
    auto s = aggregate(v);
    CHECK_EQ(s.distinct_requests, (uint64_t)2);
}

void test_flag_tallies() {
    LG_CASE("stack/kv/lazy event tallies");
    std::vector<lg::core::Slot> v;
    v.push_back(make_slot(3, "a", 0, 1, FLAG_STACK));
    v.push_back(make_slot(2, "a", 0, 2, FLAG_KV));
    v.push_back(make_slot(2, "a", 0, 3, FLAG_LAZY));
    v.push_back(make_slot(2, "a", 0, 4, FLAG_KV | FLAG_LAZY));
    auto s = aggregate(v);
    CHECK_EQ(s.stack_events, (uint64_t)1);
    CHECK_EQ(s.kv_events, (uint64_t)2);
    CHECK_EQ(s.lazy_events, (uint64_t)2);
}

void test_span_and_rate() {
    LG_CASE("span and peak/avg rate over a two-second window");
    std::vector<lg::core::Slot> v;
    // 10 events in the first second, 2 in the second.
    for (int i = 0; i < 10; ++i)
        v.push_back(make_slot(2, "a", 0, i * 100'000'000LL));
    for (int i = 0; i < 2; ++i)
        v.push_back(make_slot(2, "a", 0,
                              1'000'000'000LL + i * 100'000'000LL));
    auto s = aggregate(v);
    CHECK_EQ(s.span_mono_ns, 1'100'000'000LL);
    CHECK_EQ(s.peak_per_second, (uint64_t)10);
    CHECK(s.avg_per_second > 9.0 && s.avg_per_second < 12.5);
}

void test_empty_and_single() {
    LG_CASE("empty and single-event windows");
    auto e = aggregate({});
    CHECK_EQ(e.total, (uint64_t)0);
    CHECK_EQ(e.span_mono_ns, (int64_t)0);
    std::vector<lg::core::Slot> one{make_slot(2, "a", 0, 999)};
    auto o = aggregate(one);
    CHECK_EQ(o.total, (uint64_t)1);
    CHECK_EQ(o.span_mono_ns, (int64_t)0);
}

void test_unsorted_input() {
    LG_CASE("out-of-order input sorted internally before timing");
    std::vector<lg::core::Slot> v;
    v.push_back(make_slot(4, "late", 0, 500));
    v.push_back(make_slot(2, "early", 0, 100));
    v.push_back(make_slot(3, "mid", 0, 300));
    auto s = aggregate(v);
    CHECK_EQ(s.first_mono_ns, (int64_t)100);
    CHECK_EQ(s.last_mono_ns, (int64_t)500);
    CHECK_EQ(s.span_mono_ns, (int64_t)400);
}

void test_render_contains() {
    LG_CASE("text and json rendering include key fields");
    std::vector<lg::core::Slot> v;
    v.push_back(make_slot(4, "net", "r1", 1));
    v.push_back(make_slot(2, "net", "r2", 2));
    auto s = aggregate(v);
    std::string txt = render_stats(s);
    CHECK(txt.find("records:") != std::string::npos);
    CHECK(txt.find("net=2") != std::string::npos);
    std::string js = render_stats_json(s);
    CHECK(js.find("\"total\":2") != std::string::npos);
    CHECK(js.find("\"net\":2") != std::string::npos);
}

} // namespace

void run_all_tests() {
    test_counts();
    test_request_breadth();
    test_flag_tallies();
    test_span_and_rate();
    test_empty_and_single();
    test_unsorted_input();
    test_render_contains();
}

LG_TEST_MAIN()
