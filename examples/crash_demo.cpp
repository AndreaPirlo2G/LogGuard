// Demo modes:
//   crash_demo <ring> crash [iterations]      segfault after N iterations
//   crash_demo <ring> kill                    log forever (kill -9 externally)
//   crash_demo <ring> stall <watchdog_ms>     event loop hangs; watchdog dumps stacks
//   crash_demo <ring> snapshot                manual snapshot after 20 iterations
//   crash_demo <ring> rules                   structured events + rate rule
//                                             (3 payment failures in 2 s -> capture)
#include <logguard/logguard.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <unistd.h>

static void worker_thread() {
    for (int i = 0;; ++i) {
        logguard::ScopedRequest r(("worker-" + std::to_string(i % 7)).c_str());
        LG_DEBUG("worker", "idle tick %d", i);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

int main(int argc, char** argv) {
    const char* ring = argc > 1 ? argv[1] : "demo.ring";
    const char* mode = argc > 2 ? argv[2] : "crash";
    long iterations = argc > 3 ? std::atol(argv[3]) : 50;

    LgOpts opts{};
    opts.zstd_compression = 1; // compress when libzstd loads, else fall back
    if (std::strcmp(mode, "stall") == 0)
        opts.stall_watchdog_ms = argc > 3 ? std::atoi(argv[3]) : 2000;
    if (lg_init_opts(ring, &opts) != 0) return 1;

    std::thread t(worker_thread);
    t.detach();

    if (std::strcmp(mode, "stall") == 0) {
        for (int i = 0; i < 3; ++i) {
            LG_INFO("demo", "event loop iteration %d", i);
            lg_heartbeat();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        LG_WARN("demo", "event loop now blocking for 10 seconds (deadlock simulation)");
        std::this_thread::sleep_for(std::chrono::seconds(10)); // no heartbeat -> stall
        lg_shutdown();
        return 0;
    }

    if (std::strcmp(mode, "rules") == 0) {
        // Capture when "payment failed" recurs 3 times within 2 seconds.
        int rid = lg_add_rule(LG_LEVEL_ERROR, "payment", "failed", 3, 2000, 30000);
        LG_INFO("demo", "installed rate-based rule id=%d", rid);
        for (int i = 0; i < 8; ++i) {
            char req[24];
            std::snprintf(req, sizeof(req), "tx-%d", i);
            logguard::ScopedRequest r(req);
            if (i >= 3 && i <= 5) {
                LG_EVENT(LG_LEVEL_ERROR, "payment", "payment failed at gateway",
                         logguard::Fields().num("order", 9000 + i)
                                           .str("gateway", "stripe")
                                           .num("amount_ms", 200 + i * 30)
                                           .flag("retryable"));
            } else {
                LG_INFO("payment", "order %d captured", i);
            }
            usleep(300000);
        }
        std::this_thread::sleep_for(std::chrono::seconds(1)); // let worker export
        lg_shutdown();
        return 0;
    }

    if (std::strcmp(mode, "snapshot") == 0) {
        for (long i = 0; i < iterations; ++i) {
            char req[32];
            std::snprintf(req, sizeof(req), "req-%ld", i / 5);
            logguard::ScopedRequest r(req);
            LG_INFO("orders", "item=%ld amount=%d status=ok", i, rand() % 500);
            if (i == 20) lg_trigger_snapshot("operator requested diagnostics");
            usleep(2000);
        }
        lg_shutdown();
        return 0;
    }

    std::srand(static_cast<unsigned>(std::time(nullptr)));
    for (long i = 0;; ++i) {
        char req[32];
        std::snprintf(req, sizeof(req), "req-%ld", i / 5);
        logguard::ScopedRequest r(req);
        LG_INFO("demo", "item=%ld latency_ms=%d status=ok", i, 1 + rand() % 20);
        if (std::strcmp(mode, "crash") == 0 && i == iterations - 1) {
            LG_CRITICAL("demo", "about to dereference null pointer");
            volatile int* p = nullptr;
            *p = 42;
        }
        usleep(5000 + rand() % 15000);
    }
}
