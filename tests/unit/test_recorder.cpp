// Unit tests for multi-instance capture: two Recorders on distinct ring files
// must be independently configurable and must not cross-contain events.
#include "lgtest.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "capture/recorder.hpp"
#include "capture/recorder_config.hpp"
#include "capture/recorder_registry.hpp"
#include "core/constants.hpp"
#include "core/slot_format.hpp"
#include "platform/mapping.h"

using namespace lg;
using lg::cap::Recorder;
using lg::cap::RecorderConfig;

namespace {

std::string tmp_ring(const char* name) {
    std::string p = "/tmp/";
    p += name;
    std::remove(p.c_str());
    return p;
}

// Count valid slots belonging to the current epoch in a ring file.
int count_slots(const std::string& path) {
    plat::Mapping m;
    if (!plat::map_open_rw(path, m)) return -1;
    auto* h = static_cast<const lg::core::FileHeader*>(m.base);
    auto* slots = reinterpret_cast<const lg::core::Slot*>(
        static_cast<const char*>(m.base) + lg::core::HEADER_SIZE);
    int n = 0;
    for (uint64_t i = 0; i < h->slot_count; ++i) {
        const auto& s = slots[i];
        if (__atomic_load_n(&s.magic, __ATOMIC_ACQUIRE) !=
            lg::core::RING_MAGIC_NUMBER)
            continue;
        if (s.epoch == h->epoch && (s.ticket & (h->slot_count - 1)) == i) ++n;
    }
    plat::unmap(m);
    return n;
}

} // namespace

static void test_two_independent_recorders() {
    LG_CASE("two recorders keep independent rings and primary flag");
    std::string pa = tmp_ring("lg_multi_a.ring");
    std::string pb = tmp_ring("lg_multi_b.ring");

    Recorder a, b;
    RecorderConfig ca = RecorderConfig::for_path(pa);
    ca.ring_slots = 2048;
    ca.level = 2; // INFO
    RecorderConfig cb = RecorderConfig::for_path(pb);
    cb.ring_slots = 2048;
    cb.level = 4; // ERROR: INFO/DEBUG are filtered by the embedder

    InitOpts opts;
    CHECK_EQ(a.open(ca, opts, nullptr), 0);
    CHECK_EQ(b.open(cb, opts, nullptr), 0);
    CHECK(a.is_primary());
    CHECK(!b.is_primary());

    // Route the handle-less logging on this thread at A.
    lg::cap::registry_set_request("req-A");
    for (int i = 0; i < 10; ++i) a.event(2, "svcA", "alpha message", nullptr);
    lg::cap::registry_set_request("req-B");
    for (int i = 0; i < 5; ++i) b.event(4, "svcB", "beta error", nullptr);

    // The hot path commits regardless; filtering is the embedder's policy via
    // level. Here we assert events land in the correct file only.
    a.close();
    b.close();

    CHECK_EQ(count_slots(pa), 10);
    CHECK_EQ(count_slots(pb), 5);
}

static void test_registry_handles() {
    LG_CASE("registry assigns stable handles and promotes on remove");
    std::string pa = tmp_ring("lg_reg_a.ring");
    std::string pb = tmp_ring("lg_reg_b.ring");
    Recorder a, b;
    CHECK_EQ(a.open(RecorderConfig::for_path(pa), InitOpts{}, nullptr), 0);
    CHECK_EQ(b.open(RecorderConfig::for_path(pb), InitOpts{}, nullptr), 0);

    int ha = -1, hb = -1;
    for (int i = 0; i < lg::cap::kMaxRecorders; ++i) {
        if (lg::cap::registry_get(i) == &a) ha = i;
        if (lg::cap::registry_get(i) == &b) hb = i;
    }
    CHECK(ha >= 0 && hb >= 0 && ha != hb);
    CHECK_EQ(lg::cap::registry_primary_handle(), ha);

    a.close(); // primary removed -> b promoted
    CHECK_EQ(lg::cap::registry_primary(), &b);
    b.close();
    CHECK_EQ(lg::cap::registry_primary_handle(), lg::cap::kInvalidHandle);
}

static void test_bad_config_rejected() {
    LG_CASE("open with empty ring path fails cleanly");
    Recorder r;
    RecorderConfig c; // empty path
    CHECK_EQ(r.open(c, InitOpts{}, nullptr), -1);
}

static void test_burst_suppression_collapses_storm() {
    LG_CASE("burst-enabled recorder collapses repeated identical logs");
    std::string p = tmp_ring("lg_burst.ring");
    RecorderConfig cfg = RecorderConfig::for_path(p);
    cfg.ring_slots = 4096;
    cfg.burst_suppression = true;
    cfg.burst_window_ms = 20;

    Recorder a, b; // a becomes primary, b (burst-enabled) is non-primary
    CHECK_EQ(a.open(RecorderConfig::for_path(tmp_ring("lg_burst_p.ring")),
                    InitOpts{}, nullptr), 0);
    CHECK_EQ(b.open(cfg, InitOpts{}, nullptr), 0);

    for (int i = 0; i < 50; ++i) b.event(2, "noisy", "repeated timeout", nullptr);
    // All 50 share one suppression window (well under 20 ms), so only the
    // first is committed; the other 49 are swallowed.
    b.close();
    a.close();

    CHECK_EQ(count_slots(p), 1);
}

// Count *.lgp regular files in a directory.
static int count_packages(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) return -1;
    int n = 0;
    while (dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (name.size() < 4 ||
            name.compare(name.size() - 4, 4, ".lgp") != 0)
            continue;
        struct stat st;
        if (stat((dir + "/" + name).c_str(), &st) == 0 && S_ISREG(st.st_mode))
            ++n;
    }
    closedir(d);
    return n;
}

static void test_export_prunes_to_retention_policy() {
    LG_CASE("recorder prunes output dir to retention policy after export");
    std::string ring = tmp_ring("lg_retain.ring");
    std::string dir = "/tmp/lg_retain_dir";
    // Start from a clean output directory.
    DIR* d = opendir(dir.c_str());
    if (d) {
        closedir(d);
        (void)std::system(("rm -rf " + dir).c_str());
    }
    mkdir(dir.c_str(), 0755);

    RecorderConfig cfg = RecorderConfig::for_path(ring);
    cfg.ring_slots = 4096;
    cfg.output_dir = dir;
    cfg.retain_max_packages = 2;
    cfg.retain_max_mb = 1024; // only the count bound is exercised here
    cfg.retain_max_age_hours = 0;

    // Seed four old packages with descending mtimes so newest-first pruning
    // has a deterministic order.
    for (int i = 0; i < 4; ++i) {
        std::string f = dir + "/old-" + std::to_string(i) + ".lgp";
        FILE* fp = std::fopen(f.c_str(), "wb");
        CHECK(fp != nullptr);
        if (fp) { std::fputc('x', fp); std::fclose(fp); }
    }
    CHECK_EQ(count_packages(dir), 4);

    Recorder r;
    CHECK_EQ(r.open(cfg, InitOpts{}, nullptr), 0);
    r.event(2, "demo", "retention integration event", nullptr);
    r.trigger("retain-test"); // cooldown-free first snapshot
    r.close();                 // drains export worker + retention prune

    // One fresh package plus at most one survivor = bounded by max_files.
    int left = count_packages(dir);
    CHECK(left >= 1);
    CHECK(left <= 2);
}

void run_all_tests() {
    test_two_independent_recorders();
    test_registry_handles();
    test_bad_config_rejected();
    test_burst_suppression_collapses_storm();
    test_export_prunes_to_retention_policy();
}

LG_TEST_MAIN()
