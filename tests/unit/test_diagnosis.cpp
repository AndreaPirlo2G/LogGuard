// Unit tests for the pure-logic modules: rule engine, fingerprint normalization
// and signature diff. These need no ring/filesystem and use an injected clock
// for deterministic sliding-window behaviour.
#include "lgtest.hpp"

#include "diagnosis/rule_engine.hpp"
#include "diagnosis/fingerprint.hpp"
#include "diagnosis/signature_diff.hpp"
#include "core/json_util.hpp"

using namespace lg::diag;
namespace json = lg::core::json;

static void test_rule_threshold_and_cooldown() {
    LG_CASE("rule fires at threshold then cools down");
    int64_t now = 1000;
    RuleEngine eng([&] { return now; });

    Rule r;
    r.min_level = 4;
    r.category = "payment";
    r.substr = "failed";
    r.threshold = 3;
    r.window_ns = 1'000;
    r.cooldown_ns = 500;
    CHECK_EQ(eng.add(r), 0);

    // Below threshold: no fire.
    CHECK_EQ(eng.evaluate(4, "payment", "failed once"), 0);
    CHECK_EQ(eng.evaluate(4, "payment", "failed twice"), 0);
    // Third within window -> fires.
    CHECK_EQ(eng.evaluate(4, "payment", "failed thrice"), 1);
    // Immediately after, still inside cooldown -> suppressed.
    CHECK_EQ(eng.evaluate(4, "payment", "failed again"), 0);
    // After both cooldown and the counting window elapse, the counter resets
    // and three more observations fire again.
    now += 1100;
    CHECK_EQ(eng.evaluate(4, "payment", "failed a"), 0);
    CHECK_EQ(eng.evaluate(4, "payment", "failed b"), 0);
    CHECK_EQ(eng.evaluate(4, "payment", "failed c"), 1);
    CHECK_EQ(eng.fire_count(), 2u);
}

static void test_rule_level_and_category_filter() {
    LG_CASE("rule respects level and category");
    RuleEngine eng([] { return 1; });
    Rule r;
    r.min_level = 4;
    r.category = "db";
    r.threshold = 1;
    eng.add(r);

    CHECK_EQ(eng.evaluate(3, "db", "warn only"), 0);        // level too low
    CHECK_EQ(eng.evaluate(4, "auth", "db-ish? no"), 0);    // wrong category
    CHECK_EQ(eng.evaluate(4, "db", "connection lost"), 1); // match
}

static void test_rule_window_resets() {
    LG_CASE("rule count resets outside the window");
    int64_t now = 0;
    RuleEngine eng([&] { return now; });
    Rule r;
    r.min_level = 4;
    r.threshold = 2;
    r.window_ns = 100;
    r.cooldown_ns = 0;
    eng.add(r);

    CHECK_EQ(eng.evaluate(4, "", "x"), 0);
    now = 200; // window elapsed; first observation ages out
    CHECK_EQ(eng.evaluate(4, "", "x"), 0);
    CHECK_EQ(eng.evaluate(4, "", "x"), 1);
}

static void test_fingerprint_normalization() {
    LG_CASE("fingerprint normalizes variable tokens");
    CHECK_STREQ(normalize_message("order 9003 failed"),
                "order <n> failed");
    CHECK_STREQ(normalize_message("order 9004 failed"),
                "order <n> failed");
    CHECK_STREQ(normalize_message("fault at 0x7f9a1b3c offset"),
                "fault at <hex> offset");
    CHECK_STREQ(normalize_message("user \"alice\" logged in"),
                "user <str> logged in");
    CHECK_STREQ(normalize_message("latency 12.5 ms"),
                "latency <n> ms");
    // Same signature despite different numbers -> alias intended.
    CHECK(normalize_message("retry 3 times") ==
          normalize_message("retry 9 times"));
}

static void test_add_observation_filters() {
    LG_CASE("add_observation ignores internals and low levels");
    SignatureSet set;
    CHECK(add_observation(set, "ERROR", "payment", "boom 1"));
    CHECK(!add_observation(set, "INFO", "payment", "fine 2"));
    CHECK(!add_observation(set, "ERROR", "_stack", "internal"));
    CHECK(!add_observation(set, "NOTALEVEL", "x", "y"));
    CHECK_EQ(set.size(), 1u);
}

static void test_diff_added_changed_removed() {
    LG_CASE("diff detects added/removed/changed and regression flag");
    SignatureSet base;
    SignatureSet cand;
    add_observation(base, "WARN", "db", "slow <n> ms"); // count 1
    add_observation(base, "ERROR", "pay", "card declined <n>");
    add_observation(base, "ERROR", "pay", "card declined <n>"); // count 2
    add_observation(cand, "WARN", "db", "slow <n> ms");
    for (int i = 0; i < 9; ++i)
        add_observation(cand, "ERROR", "pay", "card declined <n>");
    add_observation(cand, "ERROR", "auth", "null token <n>");

    DiffReport rep = compare(base, cand);
    CHECK_EQ(rep.added, 1);    // auth null token is new
    CHECK_EQ(rep.removed, 0);  // db WARN present in both at the same count
    CHECK_EQ(rep.changed, 1);  // pay card declined 2 -> 9
    CHECK(rep.has_regression());
}

static void test_diff_no_regression_clean() {
    LG_CASE("identical profiles are not a regression");
    SignatureSet a, b;
    add_observation(a, "ERROR", "x", "err <n>");
    add_observation(b, "ERROR", "x", "err 42");
    DiffReport rep = compare(a, b);
    CHECK_EQ(rep.has_regression(), false);
}

static void test_serialize_roundtrip() {
    LG_CASE("signature set serializes and deserializes");
    SignatureSet set;
    add_observation(set, "ERROR", "pay", "failed order <n>");
    add_observation(set, "ERROR", "pay", "failed order 9"); // same sig
    add_observation(set, "WARN", "db", "pool wait <n> ms");
    std::string text = serialize(set);
    SignatureSet back = deserialize(text);
    CHECK_EQ(back.size(), set.size());
    CHECK_EQ(serialize(back), text);
}

static void test_json_helpers() {
    LG_CASE("json escaping and kv emitters");
    std::string out;
    json::write_escaped(out, "a\"b\nc");
    CHECK_STREQ(out, "\"a\\\"b\\nc\"");

    std::string kv = "{";
    bool first = true;
    json::write_kv_string(kv, "name", "bob", first);
    json::write_kv_int(kv, "n", 7, first);
    json::write_kv_bool(kv, "ok", true, first);
    kv += "}";
    CHECK_STREQ(kv, "{\"name\":\"bob\",\"n\":7,\"ok\":true}");
}

void run_all_tests() {
    test_rule_threshold_and_cooldown();
    test_rule_level_and_category_filter();
    test_rule_window_resets();
    test_fingerprint_normalization();
    test_add_observation_filters();
    test_diff_added_changed_removed();
    test_diff_no_regression_clean();
    test_serialize_roundtrip();
    test_json_helpers();
}

LG_TEST_MAIN()
