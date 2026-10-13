#include "lgtest.hpp"

#include "diagnosis/sequence_rule.hpp"
#include "diagnosis/symbolize.hpp"

using namespace lg::diag;

static void test_ordered_chain_fires_once() {
    LG_CASE("ordered chain fires once on completion");
    std::vector<SeqStep> steps = {
        SeqStep(2, "net", "reconnect start"),
        SeqStep(2, "net", "reconnect timeout"),
        SeqStep(3, "net", "gave up"),
    };
    SequenceRule r(steps, 5'000'000'000LL, 1'000'000'000LL);

    int64_t t = 1'000'000'000LL;
    CHECK(!r.observe(2, "net", "reconnect start", t));
    t += 10;
    CHECK(!r.observe(2, "other", "noise event", t)); // unrelated, keeps chain
    t += 10;
    CHECK(!r.observe(2, "net", "reconnect timeout", t));
    t += 10;
    CHECK(r.observe(3, "net", "gave up", t));
    CHECK_EQ(r.fire_count(), 1);

    // Immediate identical completion is suppressed by cooldown.
    CHECK(!r.observe(2, "net", "reconnect start", t));
    CHECK(!r.observe(2, "net", "reconnect timeout", t));
    CHECK(!r.observe(3, "net", "gave up", t));
    CHECK_EQ(r.fire_count(), 1);
}

static void test_out_of_order_resets() {
    LG_CASE("tail before head does not fire");
    std::vector<SeqStep> steps = {SeqStep(2, "", "a"), SeqStep(2, "", "b")};
    SequenceRule r(steps, 5'000'000'000LL, 0);
    int64_t t = 0;
    CHECK(!r.observe(2, "x", "b", t));
    // Seeing "a" re-roots a chain; a following "b" completes it.
    CHECK(!r.observe(2, "x", "a", t));
    CHECK(r.observe(2, "x", "b", t));
}

static void test_window_expiry() {
    LG_CASE("chain slower than window does not fire");
    std::vector<SeqStep> steps = {SeqStep(2, "", "a"), SeqStep(2, "", "b")};
    SequenceRule r(steps, 1'000'000'000LL, 0); // 1 s window
    CHECK(!r.observe(2, "x", "a", 0));
    // 2 s later: partial chain expired, "b" alone is not a completion.
    CHECK(!r.observe(2, "x", "b", 2'000'000'000LL));
    CHECK_EQ(r.fire_count(), 0);
}

static void test_level_gate() {
    LG_CASE("step level gate enforced");
    std::vector<SeqStep> steps = {SeqStep(4, "", "boom"), SeqStep(4, "", "boom2")};
    SequenceRule r(steps, 5'000'000'000LL, 0);
    CHECK(!r.observe(2, "x", "boom", 0)); // too low, no match
    CHECK(!r.observe(4, "x", "boom", 0));
    CHECK(r.observe(4, "x", "boom2", 0));
}

static void test_symbolizer_self() {
    LG_CASE("symbolizer loads self map and renders without crashing");
    Symbolizer s;
    s.load_self();
    // Resolve an address inside this very function; on Linux it should map to
    // an executable file-backed region (the test binary).
    uint64_t here = reinterpret_cast<uint64_t>(
        reinterpret_cast<void*>(&test_symbolizer_self));
    ResolvedFrame fr = s.resolve(here);
    CHECK(fr.addr == here);
    // Module may be empty in exotic sandboxes; only assert format robustness.
    std::string line = s.render_stack(&here, 1);
    CHECK(!line.empty());

    // An obviously bogus address must not crash and yields no module.
    ResolvedFrame bad = s.resolve(0x1234);
    CHECK(bad.module.empty());
}

void run_all_tests() {
    test_ordered_chain_fires_once();
    test_out_of_order_resets();
    test_window_expiry();
    test_level_gate();
    test_symbolizer_self();
}

LG_TEST_MAIN()
