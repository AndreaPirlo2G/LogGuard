// Unit tests for the structured slot query engine.
#include <cstring>
#include <string>
#include <vector>

#include "lgtest.hpp"
#include "core/constants.hpp"
#include "core/slot_format.hpp"
#include "export/query.hpp"

using lg::core::Slot;
using lg::core::RING_MAGIC_NUMBER;
using lg::export_::QuerySpec;
using lg::export_::parse_query;
using lg::export_::query;

namespace {

Slot make_slot(uint16_t level, const char* cat, const char* req,
               uint64_t mono, uint64_t tid, const char* msg) {
    Slot s{};
    s.magic = RING_MAGIC_NUMBER;
    s.level = level;
    s.mono_ns = mono;
    s.tid = tid;
    size_t cl = std::strlen(cat);
    if (cl > lg::core::CATEGORY_CAP) cl = lg::core::CATEGORY_CAP;
    std::memcpy(s.category, cat, cl);
    s.cat_len = static_cast<uint16_t>(cl);
    size_t rl = std::strlen(req);
    if (rl > lg::core::REQ_CAP) rl = lg::core::REQ_CAP;
    std::memcpy(s.req_id, req, rl);
    s.req_len = static_cast<uint16_t>(rl);
    size_t n = std::strlen(msg);
    if (n > lg::core::PAYLOAD_CAP - 1) n = lg::core::PAYLOAD_CAP - 1;
    std::memcpy(s.payload, msg, n);
    s.msg_len = static_cast<uint16_t>(n);
    return s;
}

std::vector<Slot> sample() {
    std::vector<Slot> v;
    v.push_back(make_slot(2, "net",      "r1", 100, 11, "connection ok"));
    v.push_back(make_slot(3, "net",      "r1", 200, 11, "retry scheduled"));
    v.push_back(make_slot(4, "planning", "r2", 300, 12, "timeout after 500ms"));
    v.push_back(make_slot(5, "planning", "r2", 400, 12, "fatal timeout abort"));
    return v;
}

} // namespace

static void test_no_filter_returns_all() {
    LG_CASE("empty spec returns every slot");
    auto v = sample();
    QuerySpec q;
    auto hits = query(v, q);
    CHECK_EQ(hits.size(), v.size());
}

static void test_min_level() {
    LG_CASE("min_level filters by severity");
    auto v = sample();
    QuerySpec q;
    q.min_level = 4;
    auto hits = query(v, q);
    CHECK_EQ(hits.size(), 2u); // ERROR + CRITICAL
    CHECK_EQ(hits[0].slot.level, 4);
    CHECK_EQ(hits[1].slot.level, 5);
}

static void test_category_and_request() {
    LG_CASE("category + request id correlation");
    auto v = sample();
    QuerySpec q;
    q.category = "planning";
    q.request_id = "r2";
    auto hits = query(v, q);
    CHECK_EQ(hits.size(), 2u);
    for (auto& h : hits)
        CHECK_STREQ(std::string(h.slot.category, h.slot.cat_len).c_str(),
                    "planning");
}

static void test_time_window() {
    LG_CASE("monotonic time window is inclusive");
    auto v = sample();
    QuerySpec q;
    q.from_ns = 200;
    q.to_ns = 300;
    auto hits = query(v, q);
    CHECK_EQ(hits.size(), 2u);
    CHECK_EQ(hits[0].slot.mono_ns, 200u);
    CHECK_EQ(hits[1].slot.mono_ns, 300u);
}

static void test_tid_and_text() {
    LG_CASE("tid and substring text filter");
    auto v = sample();
    QuerySpec q;
    q.tid = 12;
    q.contains = "timeout";
    auto hits = query(v, q);
    CHECK_EQ(hits.size(), 2u); // both planning timeouts on tid 12
}

static void test_all_tokens_and() {
    LG_CASE("multi-token text requires every token");
    auto v = sample();
    QuerySpec q;
    q.contains = "fatal timeout";
    q.match_all_tokens = true;
    auto hits = query(v, q);
    CHECK_EQ(hits.size(), 1u);
    CHECK_EQ(hits[0].slot.level, 5);
}

static void test_combined_and() {
    LG_CASE("combined clauses are ANDed");
    auto v = sample();
    QuerySpec q;
    q.min_level = 4;
    q.category = "planning";
    q.contains = "abort";
    auto hits = query(v, q);
    CHECK_EQ(hits.size(), 1u);
    CHECK_EQ(hits[0].slot.level, 5);
}

static void test_parse_expression() {
    LG_CASE("CLI expression parser");
    QuerySpec q;
    std::string err;
    CHECK(parse_query("level>=ERROR category=planning req=r2 text=timeout",
                      q, &err));
    CHECK_EQ(q.min_level, 4);
    CHECK_STREQ(q.category.c_str(), "planning");
    CHECK_STREQ(q.request_id.c_str(), "r2");
    CHECK_STREQ(q.contains.c_str(), "timeout");

    QuerySpec q2;
    CHECK(parse_query("cat=net tid=11 from=100 to=250", q2, nullptr));
    CHECK_STREQ(q2.category.c_str(), "net");
    CHECK_EQ(q2.tid, 11u);
    CHECK_EQ(q2.from_ns, 100u);
    CHECK_EQ(q2.to_ns, 250u);

    QuerySpec q3;
    CHECK(parse_query("retry scheduled", q3, nullptr)); // bare words -> AND text
    CHECK(q3.match_all_tokens);
    CHECK_STREQ(q3.contains.c_str(), "retry scheduled");

    QuerySpec q4;
    std::string err4;
    CHECK(!parse_query("level=BOGUS", q4, &err4));
    CHECK(!err4.empty());
}

void run_all_tests() {
    test_no_filter_returns_all();
    test_min_level();
    test_category_and_request();
    test_time_window();
    test_tid_and_text();
    test_all_tokens_and();
    test_combined_and();
    test_parse_expression();
}

LG_TEST_MAIN()
