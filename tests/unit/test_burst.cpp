// Unit tests for lg::cap::BurstGuard (log-storm suppression).
#include "capture/burst_guard.hpp"
#include "lgtest.hpp"

using lg::cap::BurstGuard;
using lg::cap::BurstDecision;

static void test_first_event_passes() {
    BurstGuard g(100);
    BurstDecision d = g.on_event("net\\x1ftimeout", 0);
    CHECK(d.emit_current);
    CHECK_EQ(d.pending_summary, 0u);
}

static void test_duplicates_within_window_suppressed() {
    BurstGuard g(100);
    CHECK(g.on_event("k", 0).emit_current);
    for (uint64_t t = 1; t < 100; ++t) {
        BurstDecision d = g.on_event("k", t);
        CHECK(!d.emit_current);
    }
    CHECK_EQ(g.total_suppressed(), 99u);
}

static void test_window_elapse_emits_summary_then_current() {
    BurstGuard g(100);
    CHECK(g.on_event("k", 0).emit_current);
    CHECK(!g.on_event("k", 10).emit_current);
    CHECK(!g.on_event("k", 20).emit_current);
    // t=100: window [0,100) elapsed; settle 2 suppressed, current passes.
    BurstDecision d = g.on_event("k", 100);
    CHECK(d.emit_current);
    CHECK_EQ(d.pending_summary, 2u);
    CHECK_STREQ(d.summary_key.c_str(), "k");
}

static void test_no_summary_when_nothing_suppressed() {
    BurstGuard g(100);
    CHECK(g.on_event("k", 0).emit_current);
    // Next copy arrives exactly at/after window with none in between.
    BurstDecision d = g.on_event("k", 100);
    CHECK(d.emit_current);
    CHECK_EQ(d.pending_summary, 0u);
}

static void test_distinct_keys_independent() {
    BurstGuard g(100);
    CHECK(g.on_event("a", 0).emit_current);
    CHECK(g.on_event("b", 0).emit_current);
    CHECK(!g.on_event("a", 10).emit_current);
    CHECK(!g.on_event("b", 20).emit_current);
    CHECK_EQ(g.tracked_keys(), 2u);
    // a settles first.
    BurstDecision da = g.on_event("a", 100);
    CHECK(da.emit_current);
    CHECK_EQ(da.pending_summary, 1u);
    // b still inside its window relative to its first at 0; at t=100 it also elapses.
    BurstDecision db = g.on_event("b", 100);
    CHECK(db.emit_current);
    CHECK_EQ(db.pending_summary, 1u);
}

static void test_drain_flushes_expired_run() {
    BurstGuard g(100);
    CHECK(g.on_event("k", 0).emit_current);
    CHECK(!g.on_event("k", 10).emit_current);
    CHECK(!g.on_event("k", 20).emit_current);

    BurstDecision before = g.drain(50);
    CHECK(!before.emit_current);
    CHECK_EQ(before.pending_summary, 0u);

    BurstDecision after = g.drain(100);
    CHECK(!after.emit_current);       // drain is not a new event
    CHECK_EQ(after.pending_summary, 2u);
    CHECK_STREQ(after.summary_key.c_str(), "k");

    // After draining, once more is empty.
    BurstDecision empty = g.drain(200);
    CHECK_EQ(empty.pending_summary, 0u);
}

static void test_new_run_after_settlement_counts_again() {
    BurstGuard g(100);
    g.on_event("k", 0);
    g.on_event("k", 10);
    BurstDecision d = g.on_event("k", 100); // settles 1, new run begins
    CHECK(d.emit_current);
    CHECK_EQ(d.pending_summary, 1u);
    // Duplicates of the new run get suppressed from t=100.
    CHECK(!g.on_event("k", 110).emit_current);
    CHECK(!g.on_event("k", 120).emit_current);
    BurstDecision d2 = g.on_event("k", 200);
    CHECK(d2.emit_current);
    CHECK_EQ(d2.pending_summary, 2u);
}

static void test_clear_resets() {
    BurstGuard g(100);
    g.on_event("k", 0);
    g.on_event("k", 10);
    g.clear();
    CHECK_EQ(g.tracked_keys(), 0u);
    // After clear, event passes as a fresh run.
    BurstDecision d = g.on_event("k", 10);
    CHECK(d.emit_current);
    CHECK_EQ(d.pending_summary, 0u);
}

void run_all_tests() {
    LG_CASE("first event passes");
    test_first_event_passes();
    LG_CASE("duplicates within window suppressed");
    test_duplicates_within_window_suppressed();
    LG_CASE("window elapse emits summary then current");
    test_window_elapse_emits_summary_then_current();
    LG_CASE("no summary when nothing suppressed");
    test_no_summary_when_nothing_suppressed();
    LG_CASE("distinct keys independent");
    test_distinct_keys_independent();
    LG_CASE("drain flushes expired run");
    test_drain_flushes_expired_run();
    LG_CASE("new run after settlement counts again");
    test_new_run_after_settlement_counts_again();
    LG_CASE("clear resets state");
    test_clear_resets();
}

LG_TEST_MAIN()
