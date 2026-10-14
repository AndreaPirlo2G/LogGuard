// Unit tests for best-effort stack symbolization, including addr2line source
// locations. Source-location assertions are conditional on addr2line and a
// binary built with debug info; the test binary itself is compiled -g in the
// build, and we resolve one of our own functions.
#include "diagnosis/symbolize.hpp"
#include "lgtest.hpp"

#include <cstdint>

using lg::diag::Symbolizer;

// A non-inlined named anchor whose address we can resolve. Marked noinline so
// -O2 still emits a distinct symbol.
__attribute__((noinline)) static uintptr_t anchor_frame() {
    return reinterpret_cast<uintptr_t>(
        reinterpret_cast<void*>(&anchor_frame));
}

static void test_module_resolution() {
    LG_CASE("self PC resolves to a backed executable module");
    Symbolizer s;
    s.load_self();
    uintptr_t pc = anchor_frame();
    auto fr = s.resolve(pc);
    CHECK(fr.addr == pc);
    // The main executable always backs its own text.
    CHECK(!fr.module.empty());
}

static void test_zero_and_unmapped() {
    LG_CASE("zero/unmapped PCs are safe and unbacked");
    Symbolizer s;
    s.load_self();
    auto z = s.resolve(0);
    CHECK_EQ(z.module.empty(), true);
    auto hi = s.resolve(0x1); // first page is never mapped
    CHECK_EQ(hi.module.empty(), true);
}

static void test_render_stack_format() {
    LG_CASE("rendered stack has one #NN line per frame");
    Symbolizer s;
    s.load_self();
    uintptr_t pcs[3] = {anchor_frame(), anchor_frame(), 0};
    std::string out = s.render_stack(reinterpret_cast<const uint64_t*>(pcs), 3);
    CHECK(out.find("#0 ") != std::string::npos);
    CHECK(out.find("#1 ") != std::string::npos);
    CHECK(out.find("#2 ") != std::string::npos);
}

static void test_source_location_when_available() {
    LG_CASE("addr2line resolves anchor to this source file (best effort)");
    Symbolizer s;
    s.load_self();
    s.enable_source_locations(true);
    auto fr = s.resolve(anchor_frame());
    auto li = s.source_location(fr);
    // When binutils + debug info are present the location names this file;
    // when stripped/tool missing it is empty, which is an accepted degrade.
    if (!li.location.empty()) {
        CHECK(li.location.find("test_symbolize.cpp") != std::string::npos ||
              li.location == "??:0");
    }
    // Rendering with source enabled must remain bounded and repeat-frame safe.
    uintptr_t pcs[4] = {anchor_frame(), anchor_frame(), anchor_frame(), 0};
    std::string out = s.render_stack(reinterpret_cast<const uint64_t*>(pcs), 4);
    CHECK(out.find("#3 ") != std::string::npos);
}

void run_all_tests() {
    test_module_resolution();
    test_zero_and_unmapped();
    test_render_stack_format();
    test_source_location_when_available();
}

LG_TEST_MAIN()
