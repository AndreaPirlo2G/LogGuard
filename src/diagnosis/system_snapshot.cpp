#include "diagnosis/system_snapshot.hpp"

#include <dirent.h>
#include <stdio.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

namespace lg {
namespace diag {

namespace {

std::string slurp(const char* path) {
    std::ifstream f(path);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Trim surrounding whitespace.
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

} // namespace

bool parse_loadavg(const std::string& text, double* l1, double* l5,
                   double* l15) {
    std::istringstream ss(text);
    double a = -1, b = -1, c = -1;
    if (ss >> a >> b >> c) {
        if (l1) *l1 = a;
        if (l5) *l5 = b;
        if (l15) *l15 = c;
        return true;
    }
    return false;
}

bool parse_meminfo(const std::string& text, uint64_t* total_kb,
                   uint64_t* available_kb) {
    bool got_total = false, got_avail = false;
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        // Lines look like: "MemTotal:       16384000 kB"
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = trim(line.substr(0, colon));
        std::istringstream vs(line.substr(colon + 1));
        uint64_t v = 0;
        if (!(vs >> v)) continue;
        if (key == "MemTotal") {
            if (total_kb) *total_kb = v;
            got_total = true;
        } else if (key == "MemAvailable") {
            if (available_kb) *available_kb = v;
            got_avail = true;
        }
    }
    // MemAvailable exists since kernel 3.14; absence is tolerated.
    return got_total;
}

bool parse_uptime(const std::string& text, double* uptime_seconds) {
    std::istringstream ss(text);
    double up = -1;
    if (ss >> up) {
        if (uptime_seconds) *uptime_seconds = up;
        return true;
    }
    return false;
}

bool parse_stat_cpu(const std::string& cpu_line, uint64_t* total,
                    uint64_t* busy) {
    std::istringstream ss(cpu_line);
    std::string label;
    ss >> label;
    if (label.rfind("cpu", 0) != 0) return false;
    // Aggregate fields: user nice system idle iowait irq softirq steal ...
    uint64_t v;
    uint64_t sum = 0, idle = 0;
    int idx = 0;
    while (ss >> v) {
        sum += v;
        // idle is field index 3, iowait index 4 (after the "cpu" label).
        if (idx == 3 || idx == 4) idle += v;
        ++idx;
    }
    if (idx < 4) return false;
    if (total) *total = sum;
    if (busy) *busy = sum - idle;
    return true;
}

bool parse_statm_resident(const std::string& text, long page_size,
                          uint64_t* rss_kb) {
    std::istringstream ss(text);
    uint64_t size_pages = 0, resident_pages = 0;
    if (!(ss >> size_pages)) return false;
    if (!(ss >> resident_pages)) return false;
    if (page_size <= 0) page_size = 4096;
    if (rss_kb) *rss_kb = resident_pages * uint64_t(page_size) / 1024ull;
    return true;
}

bool parse_usage_ticks(const std::string& stat_text, long clock_ticks,
                       uint64_t* user_ms, uint64_t* sys_ms) {
    // /proc/self/stat: the comm field is parenthesised and may itself contain
    // spaces or parentheses, so split from the *last* ')'. Everything after
    // it starts with field 3 (state); utime is field 14, stime field 15, i.e.
    // token indices 11 and 12 in the post-comm token stream (0-based).
    size_t rp = stat_text.rfind(')');
    if (rp == std::string::npos) return false;
    std::istringstream ss(stat_text.substr(rp + 1));
    std::string tok;
    uint64_t utime = 0, stime = 0;
    for (int i = 0; i < 13 && ss >> tok; ++i) {
        if (i == 11) utime = std::strtoull(tok.c_str(), nullptr, 10);
        if (i == 12) stime = std::strtoull(tok.c_str(), nullptr, 10);
    }
    if (clock_ticks <= 0) clock_ticks = 100;
    if (user_ms) *user_ms = utime * 1000ull / uint64_t(clock_ticks);
    if (sys_ms) *sys_ms = stime * 1000ull / uint64_t(clock_ticks);
    return true;
}

bool parse_status_threads(const std::string& text, int* threads) {
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        if (trim(line.substr(0, colon)) != "Threads") continue;
        std::istringstream vs(line.substr(colon + 1));
        int n = -1;
        if (vs >> n) {
            if (threads) *threads = n;
            return true;
        }
    }
    return false;
}

namespace {

int count_dir_entries(const char* path, bool numeric_only) {
    DIR* d = opendir(path);
    if (!d) return -1;
    int n = 0;
    while (dirent* e = readdir(d)) {
        if (!numeric_only) {
            ++n;
            continue;
        }
        bool digits = e->d_name[0] != '\0';
        for (char c : std::string(e->d_name)) {
            if (!std::isdigit((unsigned char)c)) { digits = false; break; }
        }
        if (digits) ++n;
    }
    closedir(d);
    return n;
}

void append_kv(std::string& out, bool& first, const char* key,
               const std::string& value) {
    if (!first) out += ',';
    first = false;
    out += '"';
    out += key;
    out += "\":";
    out += value;
}

std::string fmt_double(double v) {
    if (v < 0) return "null";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

} // namespace

std::string SystemSnapshot::render_json() const {
    std::string o;
    bool first = true;
    append_kv(o, first, "load_avg_1m", fmt_double(load1));
    append_kv(o, first, "load_avg_5m", fmt_double(load5));
    append_kv(o, first, "load_avg_15m", fmt_double(load15));
    append_kv(o, first, "mem_total_kb", std::to_string(mem_total_kb));
    append_kv(o, first, "mem_available_kb",
              mem_available_valid ? std::to_string(mem_available_kb)
                                  : std::string("null"));
    append_kv(o, first, "uptime_seconds", fmt_double(uptime_seconds));
    append_kv(o, first, "cpu_total_jiffies",
              std::to_string(cpu_total_jiffies));
    append_kv(o, first, "cpu_busy_jiffies", std::to_string(cpu_busy_jiffies));
    if (cpu_total_jiffies > 0) {
        double pct = 100.0 * double(cpu_busy_jiffies) /
                     double(cpu_total_jiffies);
        append_kv(o, first, "cpu_busy_pct", fmt_double(pct));
    }
    append_kv(o, first, "process_count",
              process_count_valid ? std::to_string(process_count)
                                  : std::string("null"));
    append_kv(o, first, "self_rss_kb",
              self_rss_valid ? std::to_string(self_rss_kb)
                             : std::string("null"));
    append_kv(o, first, "self_user_ms",
              self_cpu_valid ? std::to_string(self_user_ms)
                             : std::string("null"));
    append_kv(o, first, "self_sys_ms",
              self_cpu_valid ? std::to_string(self_sys_ms)
                             : std::string("null"));
    append_kv(o, first, "self_threads",
              self_thread_count >= 0 ? std::to_string(self_thread_count)
                                    : std::string("null"));
    append_kv(o, first, "self_open_fds",
              self_open_fds_valid ? std::to_string(self_open_fds)
                                  : std::string("null"));
    return o;
}

SystemSnapshot capture_snapshot() {
    SystemSnapshot s;

    double la[3] = {-1, -1, -1};
    if (getloadavg(la, 3) == 3) {
        s.load1 = la[0];
        s.load5 = la[1];
        s.load15 = la[2];
    }

    std::string mem = slurp("/proc/meminfo");
    if (!mem.empty()) {
        uint64_t total = 0, avail = 0;
        if (parse_meminfo(mem, &total, &avail)) {
            s.mem_total_kb = total;
            s.mem_available_kb = avail;
            s.mem_available_valid = true;
        }
    }

    std::string up = slurp("/proc/uptime");
    if (!up.empty()) parse_uptime(up, &s.uptime_seconds);

    // Aggregate cpu line only (first line of /proc/stat).
    std::string stat = slurp("/proc/stat");
    if (!stat.empty()) {
        std::string cpu_line;
        std::istringstream(stat) >> std::ws;
        std::getline(std::istringstream(stat), cpu_line);
        parse_stat_cpu(cpu_line, &s.cpu_total_jiffies, &s.cpu_busy_jiffies);
    }

    int procs = count_dir_entries("/proc", /*numeric_only*/ true);
    if (procs >= 0) {
        s.process_count = uint32_t(procs);
        s.process_count_valid = true;
    }

    long page = sysconf(_SC_PAGESIZE);
    std::string statm = slurp("/proc/self/statm");
    if (!statm.empty()) {
        uint64_t rss = 0;
        if (parse_statm_resident(statm, page, &rss)) {
            s.self_rss_kb = rss;
            s.self_rss_valid = true;
        }
    }

    long ticks = sysconf(_SC_CLK_TCK);
    std::string selfstat = slurp("/proc/self/stat");
    if (!selfstat.empty()) {
        uint64_t ums = 0, sms = 0;
        if (parse_usage_ticks(selfstat, ticks, &ums, &sms)) {
            s.self_user_ms = ums;
            s.self_sys_ms = sms;
            s.self_cpu_valid = true;
        }
    }

    std::string status = slurp("/proc/self/status");
    if (!status.empty()) parse_status_threads(status, &s.self_thread_count);

    // /proc/self/fd contains ".", "..", and one entry per open descriptor.
    int fds = count_dir_entries("/proc/self/fd", /*numeric_only*/ false);
    if (fds >= 0) {
        s.self_open_fds = fds - 2; // exclude . and ..
        if (s.self_open_fds < 0) s.self_open_fds = 0;
        s.self_open_fds_valid = true;
    }

    return s;
}

} // namespace diag
} // namespace lg
