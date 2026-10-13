// Hot-path microbenchmark: average cost of one LG_INFO call.
// Usage: bench_hotpath [ring_path] [iterations] [threads]
#include <logguard/logguard.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    const char* ring = argc > 1 ? argv[1] : "/tmp/lg-bench.ring";
    long total = argc > 2 ? std::atol(argv[2]) : 3'000'000;
    int threads = argc > 3 ? std::atoi(argv[3]) : 1;
    if (total <= 0) total = 3'000'000;
    if (threads <= 0) threads = 1;

    if (lg_init(ring) != 0) return 1;

    auto run = [&](long n) {
        for (long i = 0; i < n; ++i)
            LG_INFO("bench", "request_id=%ld latency_ms=%d status=ok", i % 1024, int(i % 30));
    };

    constexpr long WARM = 100'000;
    run(WARM / (threads > 1 ? threads : 1));

    auto t0 = std::chrono::steady_clock::now();
    if (threads == 1) {
        run(total);
    } else {
        std::vector<std::thread> pool;
        long per = total / threads;
        for (int t = 0; t < threads; ++t) pool.emplace_back(run, per);
        for (auto& th : pool) th.join();
        total = per * threads;
    }
    auto t1 = std::chrono::steady_clock::now();

    double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / total;
    double mops = total / std::chrono::duration<double>(t1 - t0).count() / 1e6;
    printf("threads=%d records=%ld avg=%.1f ns/call throughput=%.2f Mops/s\n",
           threads, total, ns, mops);

    lg_shutdown();
    return 0;
}
