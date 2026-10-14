// Hot-path microbenchmark.
//
// Single run:
//   bench_hotpath [ring_path] [iterations] [threads] [msg_len]
// reports avg / p50 / p99 per-call latency and throughput.
//
// Matrix mode (default when invoked as "matrix" or with --matrix):
//   sweeps message lengths {48, 200, 1024} x threads {1, 2, 4} and prints a
//   fixed-width table. Each LG_INFO call is timed around the public macro,
//   which itself performs the lock-free ticket + fixed memcpy only (the ring
//   slot payload is 166 bytes, so longer messages exercise printf formatting
//   inside the slot renderer bounds, not extra copies).
#include <logguard/logguard.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Sample {
    long calls = 0;
    double total_ns = 0;
    double p50 = 0, p99 = 0, max_ns = 0;
    double mops = 0;
};

long percentile(std::vector<uint64_t>& v, double q) {
    if (v.empty()) return 0;
    size_t idx = static_cast<size_t>(q * (v.size() - 1));
    std::nth_element(v.begin(), v.begin() + idx, v.end());
    return static_cast<long>(v[idx]);
}

// Build a printf format whose rendered message is roughly `msg_len` bytes.
// The hot-path macro stores the format + primitive args; the rendered length
// affects slot packing and (for the lazy path) the export worker, but here we
// measure the call itself regardless.
const char* format_for_len(int msg_len) {
    if (msg_len <= 48)
        return "request_id=%ld latency_ms=%d status=ok";
    if (msg_len <= 200)
        return "request_id=%ld latency_ms=%d status=ok "
               "route=/api/v2/orders/batch/confirm user=%ld region=cn-east-1";
    return "request_id=%ld latency_ms=%d status=ok "
           "route=/api/v2/orders/batch/confirm user=%ld region=cn-east-1 "
           "trace=0123456789abcdef0123456789abcdef0123456789abcdef0123456789ab"
           "cdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789ab";
}

Sample run_once(long total, int threads, int msg_len) {
    const char* fmt = format_for_len(msg_len);
    constexpr long WARM = 50'000;
    auto emit = [&](long n) {
        for (long i = 0; i < n; ++i)
            LG_INFO("bench", fmt, i % 1024, int(i % 30), i);
    };
    emit(WARM / (threads > 1 ? threads : 1));

    std::vector<uint64_t> samples;
    samples.reserve(static_cast<size_t>(total));
    std::mutex mtx;

    auto worker = [&](long n) {
        std::vector<uint64_t> local;
        local.reserve(static_cast<size_t>(n));
        for (long i = 0; i < n; ++i) {
            auto t0 = std::chrono::steady_clock::now();
            LG_INFO("bench", fmt, i % 1024, int(i % 30), i);
            auto t1 = std::chrono::steady_clock::now();
            local.push_back(
                std::chrono::duration<uint64_t, std::nano>(t1 - t0).count());
        }
        std::lock_guard<std::mutex> g(mtx);
        samples.insert(samples.end(), local.begin(), local.end());
    };

    auto wall0 = std::chrono::steady_clock::now();
    if (threads == 1) {
        worker(total);
    } else {
        std::vector<std::thread> pool;
        long per = total / threads;
        for (int t = 0; t < threads; ++t) pool.emplace_back(worker, per);
        for (auto& th : pool) th.join();
        total = per * threads;
    }
    auto wall1 = std::chrono::steady_clock::now();

    Sample s;
    s.calls = total;
    uint64_t sum = 0, mx = 0;
    for (uint64_t x : samples) { sum += x; if (x > mx) mx = x; }
    s.total_ns = samples.empty() ? 0 : double(sum) / samples.size();
    s.p50 = percentile(samples, 0.50);
    s.p99 = percentile(samples, 0.99);
    s.max_ns = static_cast<double>(mx);
    double secs = std::chrono::duration<double>(wall1 - wall0).count();
    s.mops = secs > 0 ? total / secs / 1e6 : 0;
    return s;
}

void print_header() {
    std::printf("%-8s %-8s %10s %10s %10s %10s %12s\n",
                "msg_len", "threads", "avg_ns", "p50_ns", "p99_ns",
                "max_ns", "Mops/s");
}

} // namespace

int main(int argc, char** argv) {
    const char* ring = "/tmp/lg-bench.ring";
    long total = 1'000'000;
    bool matrix = false;
    int threads = 1, msg_len = 200;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--matrix") matrix = true;
        else if (a == "--iterations" && i + 1 < argc) total = std::atol(argv[++i]);
        else if (a == "--threads" && i + 1 < argc) threads = std::atoi(argv[++i]);
        else if (a == "--msg-len" && i + 1 < argc) msg_len = std::atoi(argv[++i]);
        else if (a == "--ring" && i + 1 < argc) ring = argv[++i];
        else if (i == 1 && a.find('-') == std::string::npos) ring = argv[i];
    }
    if (total <= 0) total = 1'000'000;

    if (lg_init(ring) != 0) return 1;

    if (matrix) {
        const int lens[] = {48, 200, 1024};
        const int thrs[] = {1, 2, 4};
        print_header();
        for (int len : lens) {
            for (int th : thrs) {
                Sample s = run_once(total, th, len);
                std::printf("%-8d %-8d %10.1f %10.0f %10.0f %10.0f %12.2f\n",
                            len, th, s.total_ns, s.p50, s.p99, s.max_ns,
                            s.mops);
            }
        }
    } else {
        Sample s = run_once(total, threads, msg_len);
        std::printf("msg_len=%d threads=%d records=%ld avg=%.1f ns/call "
                    "p50=%.0f p99=%.0f max=%.0f throughput=%.2f Mops/s\n",
                    msg_len, threads, total, s.total_ns, s.p50, s.p99,
                    s.max_ns, s.mops);
    }

    lg_shutdown();
    return 0;
}
