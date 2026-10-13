// Unit tests for the retention/disk-quota policy (pure decision function) and
// the real filesystem enforce path.
#include <cstdio>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

#include "lgtest.hpp"
#include "export/retention.hpp"

using lg::export_::RetainedFile;
using lg::export_::RetentionPolicy;
using lg::export_::RetentionResult;
using lg::export_::apply_retention;
using lg::export_::enumerate_packages;
using lg::export_::enforce_retention;

namespace {

RetainedFile rf(const char* path, int64_t mtime, uint64_t size) {
    RetainedFile f;
    f.path = path;
    f.mtime_ns = mtime;
    f.size_bytes = size;
    return f;
}

std::vector<RetainedFile> sample() {
    // mtime ascending: oldest .. newest
    return {
        rf("/d/a.lgp", 1000, 100),
        rf("/d/b.lgp", 2000, 200),
        rf("/d/c.lgp", 3000, 300),
        rf("/d/d.lgp", 4000, 400),
        rf("/d/e.lgp", 5000, 500),
    };
}

bool deletes(const RetentionResult& r, const char* name) {
    std::string needle = std::string("/") + name;
    for (const auto& p : r.deleted)
        if (p.size() >= needle.size() &&
            p.compare(p.size() - needle.size(), needle.size(), needle) == 0)
            return true;
    return false;
}

void touch(const std::string& path, uint64_t bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    std::string chunk(bytes, 'x');
    f.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
}

} // namespace

static void test_no_bound_keeps_all() {
    LG_CASE("disabled bounds keep everything");
    auto files = sample();
    RetentionPolicy p{0, 0, 0};
    auto r = apply_retention(files, p, 9999);
    CHECK_EQ(r.deleted.size(), 0u);
    CHECK_EQ(r.files_kept, 5u);
}

static void test_max_files_keeps_newest() {
    LG_CASE("max_files keeps the newest N, deletes oldest");
    auto files = sample();
    RetentionPolicy p{3, 0, 0}; // keep 3 newest: c,d,e
    auto r = apply_retention(files, p, 9999);
    CHECK_EQ(r.deleted.size(), 2u);
    CHECK(deletes(r, "a.lgp"));
    CHECK(deletes(r, "b.lgp"));
    CHECK(!deletes(r, "c.lgp"));
    CHECK_EQ(r.files_kept, 3u);
}

static void test_total_bytes() {
    LG_CASE("max_total_bytes prunes until under quota");
    auto files = sample();
    RetentionPolicy p{0, 700, 0}; // newest-first: e500+d400=900>700 skip d,
                                  // c300 -> 800>700 skip, b200 -> 700 keep
    auto r = apply_retention(files, p, 9999);
    // e(500)+b(200)=700 kept; a,c,d deleted
    CHECK_EQ(r.bytes_kept, 700u);
    CHECK(deletes(r, "a.lgp"));
    CHECK(!deletes(r, "b.lgp"));
    CHECK(deletes(r, "c.lgp"));
    CHECK(deletes(r, "d.lgp"));
    CHECK(!deletes(r, "e.lgp"));
}

static void test_age_limit() {
    LG_CASE("max_age drops files older than the limit");
    auto files = sample();
    RetentionPolicy p{0, 0, 2500}; // now=5000: keep mtime >= 2500 -> c,d,e
    auto r = apply_retention(files, p, 5000);
    CHECK(deletes(r, "a.lgp")); // age 4000
    CHECK(deletes(r, "b.lgp")); // age 3000
    CHECK(!deletes(r, "c.lgp")); // age 2000, kept
    CHECK_EQ(r.files_kept, 3u);
}

static void test_combined_bounds() {
    LG_CASE("combined count + size + age bounds");
    auto files = sample();
    RetentionPolicy p{2, 800, 2500}; // newest 2 cap, age keeps c..e,
                                     // e500+d400=900>800 skip d, c300->800
    auto r = apply_retention(files, p, 5000);
    // kept e and c
    CHECK(!deletes(r, "e.lgp"));
    CHECK(!deletes(r, "c.lgp"));
    CHECK_EQ(r.files_kept, 2u);
    CHECK_EQ(r.bytes_kept, 800u);
}

static void test_empty_input() {
    LG_CASE("empty input yields empty result");
    RetentionPolicy p{1, 1, 1};
    auto r = apply_retention({}, p, 100);
    CHECK_EQ(r.deleted.size(), 0u);
    CHECK_EQ(r.files_kept, 0u);
    CHECK_EQ(r.bytes_freed, 0u);
}

static void test_filesystem_enforce() {
    LG_CASE("enumerate only lists .lgp and enforce actually removes");
    const std::string dir = "/tmp/lg_retention_test";
    mkdir(dir.c_str(), 0755);
    // clean
    enforce_retention(dir, RetentionPolicy{0, 0, 0}, 0);
    touch(dir + "/p1.lgp", 10);
    touch(dir + "/p2.lgp", 10);
    touch(dir + "/notes.txt", 10); // must be ignored

    bool found = false;
    auto files = enumerate_packages(dir, &found);
    CHECK(found);
    CHECK_EQ(files.size(), 2u);

    RetentionPolicy pol{1, 0, 0}; // keep one
    auto r = enforce_retention(dir, pol, 0);
    CHECK_EQ(r.deleted.size(), 1u);
    auto after = enumerate_packages(dir);
    CHECK_EQ(after.size(), 1u);
    // non-package file must survive
    struct stat st;
    CHECK(stat((dir + "/notes.txt").c_str(), &st) == 0);
}

static void test_missing_directory() {
    LG_CASE("missing directory enumerates empty without error");
    bool found = true;
    auto files = enumerate_packages("/tmp/lg_no_such_dir_xyz", &found);
    CHECK(!found);
    CHECK_EQ(files.size(), 0u);
}

void run_all_tests() {
    test_no_bound_keeps_all();
    test_max_files_keeps_newest();
    test_total_bytes();
    test_age_limit();
    test_combined_bounds();
    test_empty_input();
    test_filesystem_enforce();
    test_missing_directory();
}

LG_TEST_MAIN()
