// Failure-time environment snapshot.
//
// A crash is far easier to diagnose offline when the incident package records
// *what the machine looked like* at the moment it happened: was it out of
// memory, overloaded by load, starved of file descriptors, thrashing? Slots
// only carry application text; this module captures the surrounding host and
// process context once, at export time (never on the hot log path).
//
// All /proc parsing is split into pure functions over the raw file text, so it
// is fully unit-testable; capture_snapshot() merely reads the files and feeds
// them in. Anything unavailable (a missing /proc entry, a non-Linux host) is
// left at its sentinel and reported as such, rather than failing an export.
#pragma once

#include <cstdint>
#include <string>

namespace lg {
namespace diag {

// Values that could not be read stay at -1 (scalars) or 0 (counts), and the
// boolean `valid` flags distinguish a real zero from "not available".
struct SystemSnapshot {
    // ---- host (getloadavg / /proc/meminfo / /proc/uptime / /proc/stat) ----
    double load1 = -1.0, load5 = -1.0, load15 = -1.0;
    uint64_t mem_total_kb = 0;
    uint64_t mem_available_kb = 0;
    bool mem_available_valid = false;
    double uptime_seconds = -1.0;
    uint64_t cpu_total_jiffies = 0;  // cumulative across all CPUs
    uint64_t cpu_busy_jiffies = 0;   // total minus idle/iowait
    uint32_t process_count = 0;      // number of numeric entries in /proc
    bool process_count_valid = false;

    // ---- self (/proc/self/*) ----
    uint64_t self_rss_kb = 0;
    bool self_rss_valid = false;
    uint64_t self_user_ms = 0;
    uint64_t self_sys_ms = 0;
    bool self_cpu_valid = false;
    int self_thread_count = -1;     // /proc/self/status Threads
    int self_open_fds = -1;         // directory entries in /proc/self/fd
    bool self_open_fds_valid = false;

    // Render a compact JSON object fragment (no surrounding braces) suitable
    // for embedding inside the provenance manifest.
    std::string render_json() const;
};

// --- pure parsers: take the raw text of the corresponding /proc file ------
// parse_loadavg: "0.42 0.50 0.55 3/812 12345"
bool parse_loadavg(const std::string& text, double* l1, double* l5, double* l15);
// parse_meminfo: extracts MemTotal and MemAvailable in kB.
bool parse_meminfo(const std::string& text, uint64_t* total_kb,
                   uint64_t* available_kb);
// parse_uptime: "12345.67 6789.01"
bool parse_uptime(const std::string& text, double* uptime_seconds);
// parse_stat_cpu: the first ("cpu") aggregate line; emits total/busy jiffies.
bool parse_stat_cpu(const std::string& cpu_line, uint64_t* total,
                    uint64_t* busy);
// parse_statm: "size resident shared ..." (page units).
bool parse_statm_resident(const std::string& text, long page_size,
                          uint64_t* rss_kb);
// parse_usage_ticks: fields 14 (utime) and 15 (stime) in clock ticks.
bool parse_usage_ticks(const std::string& stat_text, long clock_ticks,
                       uint64_t* user_ms, uint64_t* sys_ms);
// parse_status_threads: the "Threads:" value in /proc/self/status.
bool parse_status_threads(const std::string& text, int* threads);

// Read live /proc and best-effort populate a snapshot. Never throws; fields
// that cannot be read remain at their sentinels.
SystemSnapshot capture_snapshot();

} // namespace diag
} // namespace lg
