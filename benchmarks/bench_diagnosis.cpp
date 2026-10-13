// Microbenchmarks for the offline/pure-logic engines: frequency rules,
// fingerprint normalisation, privacy redaction, ring audit and MCAP
// verification. These quantify the cost of the analysis path, complementing
// bench_hotpath which measures the logging hot path.
//
// Usage: bench_diagnosis [iterations]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "diagnosis/rule.hpp"
#include "diagnosis/rule_engine.hpp"
#include "diagnosis/fingerprint.hpp"
#include "diagnosis/sequence_rule.hpp"
#include "export/redaction.hpp"
#include "core/ring_audit.hpp"
#include "core/slot_format.hpp"
#include "core/recovery.hpp"
#include "export/mcap_verify.hpp"
#include "export/mcap_writer.h"

namespace {

using namespace lg::core;

double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

template <typename Fn>
double time_ns(long iters, Fn&& fn) {
    constexpr long WARM = 10000;
    for (long i = 0; i < WARM; ++i) fn(i);
    double t0 = now_s();
    for (long i = 0; i < iters; ++i) fn(i);
    return (now_s() - t0) * 1e9 / static_cast<double>(iters);
}

std::vector<uint8_t> build_package(uint32_t messages) {
    const std::string path = "/tmp/lg_bench_diag.lgp";
    remove(path.c_str());
    {
        lg::McapWriter w;
        if (!w.open(path)) return {};
        w.begin("none");
        char buf[128];
        for (uint32_t i = 0; i < messages; ++i) {
            std::snprintf(buf, sizeof(buf), "req-%u latency_ms=%d",
                          i % 1024, int(i % 40));
            w.write_log(i, i % 6, i, 100, i, i, "net", "r", buf);
        }
        w.finish();
    }
    std::vector<uint8_t> bytes;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    uint8_t tmp[8192];
    size_t n;
    while ((n = std::fread(tmp, 1, sizeof(tmp), f)) > 0)
        bytes.insert(bytes.end(), tmp, tmp + n);
    std::fclose(f);
    return bytes;
}

} // namespace

int main(int argc, char** argv) {
    long iters = argc > 1 ? std::atol(argv[1]) : 1'000'000;
    if (iters <= 0) iters = 1'000'000;

    // Frequency rule evaluate (hot-path-adjacent analysis).
    {
        lg::diag::RuleEngine eng;
        lg::diag::Rule r;
        r.min_level = 3;
        r.category = "net";
        r.substr = "timeout";
        r.threshold = 5;
        r.window_ns = 1'000'000'000ull;
        r.cooldown_ns = 10'000'000'000ull;
        eng.add(r);
        double ns = time_ns(iters, [&](long i) {
            eng.evaluate(i % 6, "net", i % 7 == 0 ? "timeout" : "ok");
        });
        printf("rule evaluate         %8.1f ns/call\n", ns);
    }

    // Ordered sequence step matching.
    {
        std::vector<lg::diag::SeqStep> steps = {
            {3, "net", "retry"}, {4, "net", "circuit open"}};
        lg::diag::SequenceRule seq(steps, 1'000'000'000ull, 10'000'000'000ull);
        const char* msgs[3] = {"ok", "retry", "circuit open"};
        long clock = 0;
        double ns = time_ns(iters, [&](long i) {
            seq.observe(3, "net", msgs[i % 3], clock += 1'000'000);
        });
        printf("sequence observe      %8.1f ns/call\n", ns);
    }

    // Fingerprint normalisation of a noisy message.
    {
        std::string m = "user 13812345678 failed id 0x7ffd1234 attempt #97";
        double ns = time_ns(iters, [&](long) {
            volatile std::string x = lg::diag::normalize_message(m);
            (void)x;
        });
        printf("fingerprint normalize %8.1f ns/call\n", ns);
    }

    // Privacy redaction on a message carrying phone/email/IP.
    {
        lg::export_::Redactor r;
        std::string base =
            "call 13812345678 or a@b.com from 192.168.1.10 id 110101199003077651";
        double ns = time_ns(iters / 4 + 1, [&](long) {
            std::string s = base;
            r.apply(s);
        });
        printf("redaction apply       %8.1f ns/call\n", ns);
    }

    // Ring audit of an in-memory ring (scanned capacity).
    {
        constexpr uint64_t cap = 4096;
        std::vector<uint8_t> mem(HEADER_SIZE + cap * SLOT_SIZE, 0);
        FileHeader& h = *header_at(mem.data());
        initialize_header(h, cap, 1, 2);
        for (uint64_t i = 0; i < 2048; ++i) {
            Slot* s = slot_at(mem.data(), i);
            std::memset(s, 0, sizeof(*s));
            s->ticket = i;
            s->epoch = h.epoch;
            s->level = 2;
            __atomic_store_n(&s->magic, RING_MAGIC_NUMBER, __ATOMIC_RELEASE);
        }
        double t0 = now_s();
        constexpr long runs = 2000;
        for (long i = 0; i < runs; ++i) {
            auto a = lg::core::audit_ring(mem.data(), cap);
            if (a.valid != 2048) { std::printf("audit wrong\n"); break; }
        }
        double ns = (now_s() - t0) * 1e9 / runs;
        printf("ring audit (4096)     %8.1f ns/scan (%.1f ns/slot)\n", ns,
               ns / cap);
    }

    // MCAP verify of a generated package.
    {
        auto pkg = build_package(2000);
        if (!pkg.empty()) {
            double t0 = now_s();
            constexpr long runs = 2000;
            for (long i = 0; i < runs; ++i) {
                auto rep = lg::export_::McapVerifier::verify(pkg.data(),
                                                              pkg.size(), true);
                if (!rep.ok) { std::printf("verify wrong\n"); break; }
            }
            double ns = (now_s() - t0) * 1e9 / runs;
            printf("mcap verify (2000 msgs) %7.1f ns/file (%.0f bytes)\n", ns,
                   (double)pkg.size());
        }
    }

    return 0;
}
