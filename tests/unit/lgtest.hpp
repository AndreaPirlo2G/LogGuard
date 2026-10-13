#pragma once
// Tiny zero-dependency unit-test harness.
//
// logguard intentionally avoids pulling a test framework into its supply
// chain; this ~40-line harness gives us CHECK/CHECK_EQ with a counted pass/fail
// summary, enough to lock the behaviour of the pure-logic modules (rules,
// fingerprints, diff, JSON, lazy formatting). It compiles standalone.
#include <cstdio>
#include <cstring>
#include <string>

namespace lgtest {

struct State {
    int checks = 0;
    int failures = 0;
    std::string current_case;
};

inline State& state() {
    static State s;
    return s;
}

inline void begin_case(const char* name) {
    state().current_case = name;
}

inline void record(bool ok, const char* expr, const char* file, int line) {
    ++state().checks;
    if (!ok) {
        ++state().failures;
        std::printf("  FAIL [%s] %s @%s:%d\n",
                    state().current_case.c_str(), expr, file, line);
    }
}

inline int finish() {
    std::printf("\n%d checks, %d failures\n",
                state().checks, state().failures);
    return state().failures == 0 ? 0 : 1;
}

} // namespace lgtest

#define LG_CASE(name) lgtest::begin_case(name)

#define CHECK(cond) \
    lgtest::record(static_cast<bool>(cond), #cond, __FILE__, __LINE__)

#define CHECK_EQ(a, b) lgtest::record(((a) == (b)), #a " == " #b, __FILE__, __LINE__)

#define CHECK_STREQ(a, b)                                                      \
    lgtest::record(std::string(a) == std::string(b),                           \
                   #a " == " #b, __FILE__, __LINE__)

#define LG_TEST_MAIN()                                                         \
    int main() {                                                               \
        ::run_all_tests();                                                     \
        return lgtest::finish();                                               \
    }
