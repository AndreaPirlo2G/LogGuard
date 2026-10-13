// Unit tests for the failure-time environment snapshot parsers. All parsing
// is exercised against canned /proc text so the tests are deterministic and
// host-independent; capture_snapshot() is only smoke-tested for sane output.
#include "lgtest.hpp"

#include <cstdio>
#include <string>

#include "diagnosis/system_snapshot.hpp"

using lg::diag::SystemSnapshot;
using lg::diag::parse_loadavg;
using lg::diag::parse_meminfo;
using lg::diag::parse_uptime;
using lg::diag::parse_stat_cpu;
using lg::diag::parse_statm_resident;
using lg::diag::parse_usage_ticks;
using lg::diag::parse_status_threads;
using lg::diag::capture_snapshot;

static void test_loadavg() {
    LG_CASE("loadavg parses three averages");
    double a = 0, b = 0, c = 0;
    CHECK(parse_loadavg("0.42 0.50 0.55 3/812 12345\n", &a, &b, &c));
    CHECK(a > 0.41 && a < 0.43);
    CHECK(b > 0.49 && b < 0.51);
    CHECK(c > 0.54 && c < 0.56);

    LG_CASE("loadavg rejects malformed text");
    CHECK(!parse_loadavg("not a loadavg", &a, &b, &c));
}

static void test_meminfo() {
    LG_CASE("meminfo extracts total and available kilobytes");
    const char* text =
        "MemTotal:       16384000 kB\n"
        "MemFree:         1000000 kB\n"
        "MemAvailable:    8000000 kB\n"
        "Buffers:          200000 kB\n";
    uint64_t total = 0, avail = 0;
    CHECK(parse_meminfo(text, &total, &avail));
    CHECK_EQ(total, 16384000u);
    CHECK_EQ(avail, 8000000u);

    LG_CASE("meminfo tolerates missing MemAvailable (old kernels)");
    avail = 123;
    CHECK(parse_meminfo("MemTotal:        2048000 kB\n", &total, &avail));
    CHECK_EQ(total, 2048000u);

    LG_CASE("meminfo without MemTotal is a failure");
    CHECK(!parse_meminfo("Buffers: 100 kB\n", &total, &avail));
}

static void test_uptime() {
    LG_CASE("uptime parses seconds");
    double up = -1;
    CHECK(parse_uptime("12345.67 6789.01\n", &up));
    CHECK(up > 12345.6 && up < 12345.8);
    CHECK(!parse_uptime("garbage", &up));
}

static void test_stat_cpu() {
    LG_CASE("cpu aggregate line splits busy from idle/iowait");
    // user nice system idle iowait irq softirq steal guest guest_nice
    //   10   2     3   100    10    1      2      1     0      0
    uint64_t total = 0, busy = 0;
    CHECK(parse_stat_cpu("cpu  10 2 3 100 10 1 2 1 0 0", &total, &busy));
    CHECK_EQ(total, 129u);
    CHECK_EQ(busy, 19u); // 129 - (idle 100 + iowait 10)

    LG_CASE("non-aggregate line is rejected");
    CHECK(!parse_stat_cpu("intr 1234", &total, &busy));
}

static void test_statm() {
    LG_CASE("resident pages convert to kilobytes");
    uint64_t rss = 0;
    // 1024 resident pages * 4096 B = 4 MiB = 4096 KiB
    CHECK(parse_statm_resident("2048 1024 512 100 0 500 0\n", 4096, &rss));
    CHECK_EQ(rss, 4096u);
}

static void test_proc_self_stat() {
    LG_CASE("utime/stime parsed even when comm contains spaces/parens");
    // Construct a tricky comm: "(my (weird) proc)" — fields before it are
    // pid only; split from the last ')'. Post-comm: state + 10 fields then
    // utime, stime. Build enough tokens: state='R', fields 4..13 = 10 tokens,
    // then utime=500 stime=300.
    std::string text =
        "1234 (my (weird) proc) R 1 2 3 0 -1 0 0 0 0 0 500 300";
    uint64_t ums = 0, sms = 0;
    CHECK(parse_usage_ticks(text, 100, &ums, &sms));
    CHECK_EQ(ums, 5000u); // 500 ticks / 100 Hz * 1000 ms
    CHECK_EQ(sms, 3000u);

    LG_CASE("stat without closing paren rejected");
    CHECK(!parse_usage_ticks("1234 (truncated", 100, &ums, &sms));
}

static void test_status_threads() {
    LG_CASE("Threads value extracted from status");
    int n = -1;
    CHECK(parse_status_threads("Name:\tproc\nState:\tR (running)\n"
                               "Threads:\t7\n", &n));
    CHECK_EQ(n, 7);
    CHECK(!parse_status_threads("Name: proc\n", &n));
}

static void test_render_json() {
    LG_CASE("render_json reports unavailable fields as null");
    SystemSnapshot empty;
    std::string j = empty.render_json();
    CHECK(j.find("\"load_avg_1m\":null") != std::string::npos);
    CHECK(j.find("\"mem_available_kb\":null") != std::string::npos);
    CHECK(j.find("\"self_open_fds\":null") != std::string::npos);

    LG_CASE("render_json emits values when present");
    SystemSnapshot s;
    s.load1 = 1.5;
    s.mem_total_kb = 1024;
    s.mem_available_valid = true;
    s.mem_available_kb = 512;
    s.cpu_total_jiffies = 200;
    s.cpu_busy_jiffies = 50;
    std::string js = s.render_json();
    CHECK(js.find("\"load_avg_1m\":1.50") != std::string::npos);
    CHECK(js.find("\"mem_total_kb\":1024") != std::string::npos);
    CHECK(js.find("\"mem_available_kb\":512") != std::string::npos);
    CHECK(js.find("\"cpu_busy_pct\":25.00") != std::string::npos);
}

static void test_live_capture() {
    LG_CASE("live capture on a Linux host populates host fields");
    SystemSnapshot s = capture_snapshot();
    // This test binary certainly has an RSS and at least one thread/fd.
    CHECK(s.self_rss_valid);
    CHECK(s.self_rss_kb > 0);
    CHECK(s.self_thread_count >= 1);
    CHECK(s.self_cpu_valid);
}

void run_all_tests() {
    test_loadavg();
    test_meminfo();
    test_uptime();
    test_stat_cpu();
    test_statm();
    test_proc_self_stat();
    test_status_threads();
    test_render_json();
    test_live_capture();
}

LG_TEST_MAIN()
