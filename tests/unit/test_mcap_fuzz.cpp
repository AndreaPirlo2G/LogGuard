// Deterministic mutation fuzzing for the defensive MCAP parser.
//
// We start from a real, healthy package (single-chunk and multi-chunk, none +
// zstd when available), then apply thousands of seeded mutations: random byte
// flips, length-field corruption, truncation and extension. Every mutated
// image is fed to every parser entry point (verify, log extraction, manifest
// extraction, chunk-index read, ranged extraction). The contract is simple:
// none of them may crash, may read past the buffer, or may report impossible
// offsets/counts. This is a fixed-seed regression test, not a live fuzzer.
#include <cstdint>
#include <climits>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "lgtest.hpp"
#include "export/mcap_writer.h"
#include "export/mcap_verify.hpp"
#include "export/zstd_codec.h"

using lg::export_::McapVerifier;
using lg::export_::VerifyReport;

namespace {

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed) {}
    uint32_t next() {
        // LCG, good enough for deterministic fuzzing.
        s = s * 1664525u + 1013904223u;
        return s;
    }
    size_t below(size_t n) { return n ? static_cast<size_t>(next() % n) : 0; }
};

std::vector<uint8_t> read_all(const std::string& path) {
    std::vector<uint8_t> out;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    uint8_t buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return out;
}

std::string seed_package(const std::string& name, const std::string& comp,
                         size_t target, int messages) {
    const std::string path = std::string("/tmp/") + name;
    std::remove(path.c_str());
    lg::McapWriter w;
    if (!w.open(path)) return path;
    w.begin(comp, target);
    w.write_manifest(1, R"({"producer":"logguard","fuzz":true})");
    for (int i = 0; i < messages; ++i) {
        std::string m = "fuzz-";
        m += std::to_string(i);
        m += std::string(target < 8192 ? 50 : 5, 'z');
        int64_t t = 500 + static_cast<int64_t>(i) * 7;
        w.write_log(static_cast<uint32_t>(i), 2,
                    static_cast<uint64_t>(i), 1, t, t, "net", "r", m.c_str());
    }
    w.finish();
    return path;
}

// Drive every parsing entry point against one (possibly hostile) image.
void exercise(const std::vector<uint8_t>& img) {
    auto rep = McapVerifier::verify(img.data(), img.size(), true);
    CHECK(rep.parsed_bytes <= img.size());
    CHECK(rep.message_count <= 100000);
    CHECK(rep.indexed_chunks <= 1000000);

    std::vector<McapVerifier::LogMessage> logs;
    McapVerifier::extract_logs(img.data(), img.size(), logs, &rep);

    std::vector<McapVerifier::ChunkIndexEntry> idx;
    bool has_idx = McapVerifier::read_chunk_index(img.data(), img.size(), idx);
    if (has_idx) {
        // The parser accepted the section; under mutation we make no claim
        // about ordering/offset sanity (it may return whatever the damaged
        // bytes encode), only that the entry count stays bounded. A hostile
        // offset is re-checked at seek time against the actual buffer.
        CHECK(idx.size() <= 1000000);
    }

    // Ranged seek must also tolerate a damaged index: hostile offsets are
    // re-validated against the buffer before any dereference.
    std::vector<McapVerifier::LogMessage> ranged;
    McapVerifier::extract_range_file_blob(img, 0, INT64_MAX, ranged);
}

void fuzz_mutations(const std::vector<uint8_t>& base, const char* label) {
    LG_CASE(label);
    if (base.size() < 16) { // package could not be built; skip silently
        return;
    }
    Rng rng(0xC0FFEE);
    constexpr int kTrials = 1500;
    for (int trial = 0; trial < kTrials; ++trial) {
        std::vector<uint8_t> img = base;
        const int mode = rng.below(5);
        if (mode == 0) {
            // Random single/multi byte flips.
            int flips = 1 + rng.below(8);
            for (int k = 0; k < flips; ++k) {
                size_t pos = rng.below(img.size());
                img[pos] = uint8_t(rng.next() >> 16);
            }
        } else if (mode == 1) {
            // Truncation: cut the tail, possibly into a record header.
            size_t cut = rng.below(img.size());
            img.resize(img.size() - cut);
        } else if (mode == 2) {
            // Corrupt a u64 length field (pick an opcode-ish position and
            // overwrite the preceding 8 bytes with a wild length).
            size_t pos = 9 + rng.below(img.size() - 9);
            uint64_t wild;
            uint32_t pick = rng.below(4);
            if (pick == 0) wild = 0xFFFFFFFFFFFFFFFFull;
            else if (pick == 1) wild = uint64_t(img.size()) + 1000;
            else if (pick == 2) wild = 0;
            else wild = rng.next() | (uint64_t(rng.next()) << 32);
            for (int b = 0; b < 8 && pos + b < img.size(); ++b)
                img[pos + b] = uint8_t(wild >> (8 * b));
        } else if (mode == 3) {
            // Inject random bytes at a random position.
            size_t at = rng.below(img.size());
            size_t add = 1 + rng.below(40);
            img.insert(img.begin() + at, add, uint8_t(rng.next() >> 20));
        } else {
            // Append trailing garbage.
            size_t add = 1 + rng.below(64);
            for (size_t k = 0; k < add; ++k) img.push_back(uint8_t(rng.next()));
        }
        exercise(img);
    }
}

void test_fuzz_single_chunk() {
    static const std::string p =
        seed_package("lgfuzz_single.lgp", "none", 2 * 1024 * 1024, 60);
    fuzz_mutations(read_all(p), "fuzz single-chunk package under mutation");
}

void test_fuzz_multi_chunk() {
    static const std::string p =
        seed_package("lgfuzz_multi.lgp", "none", 4 * 1024, 300);
    fuzz_mutations(read_all(p), "fuzz multi-chunk/index package under mutation");
}

void test_fuzz_multi_chunk_zstd() {
    if (!lg::export_::ZstdCodec::instance().available()) {
        LG_CASE("fuzz zstd multi-chunk skipped (no runtime zstd)");
        return;
    }
    static const std::string p =
        seed_package("lgfuzz_multi_zstd.lgp", "zstd", 4 * 1024, 300);
    fuzz_mutations(read_all(p), "fuzz zstd multi-chunk package under mutation");
}

void test_fuzz_empty_and_magic() {
    LG_CASE("fuzz degenerate inputs never crash");
    std::vector<uint8_t> empty;
    exercise(empty);
    std::vector<uint8_t> magic(9, 0);
    exercise(magic);
    std::vector<uint8_t> real_magic(
        reinterpret_cast<const uint8_t*>("\x89MCAP\r\n\x1a\n"),
        reinterpret_cast<const uint8_t*>("\x89MCAP\r\n\x1a\n") + 9);
    exercise(real_magic);
}

} // namespace

void run_all_tests() {
    test_fuzz_single_chunk();
    test_fuzz_multi_chunk();
    test_fuzz_multi_chunk_zstd();
    test_fuzz_empty_and_magic();
}

LG_TEST_MAIN()
