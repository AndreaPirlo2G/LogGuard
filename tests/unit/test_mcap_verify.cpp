// Defensive MCAP verifier tests: feed it malformed/truncated/hostile bytes and
// ensure it reports failure without crashing or allocating absurd amounts.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "lgtest.hpp"
#include "export/mcap_verify.hpp"
#include "export/mcap_writer.h"
#include "export/zstd_codec.h"

using lg::export_::McapVerifier;
using lg::export_::VerifyReport;

namespace {

constexpr uint8_t kMagic[9] = {0x89, 'M', 'C', 'A', 'P', 0x0D, 0x0A, 0x1A, 0x0A};

// Build a minimal but valid package the real writer would produce.
std::vector<uint8_t> make_valid_mcap() {
    const std::string path = "/tmp/lgverify_valid.lgp";
    remove(path.c_str());
    {
        lg::McapWriter w;
        if (!w.open(path)) return {};
        w.begin("none");
        w.write_log(1, 2, 1, 100, 1, 1, "net", "req-1", "hello");
        w.finish();
    }
    std::vector<uint8_t> bytes;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return {};
    uint8_t tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0)
        bytes.insert(bytes.end(), tmp, tmp + n);
    fclose(f);
    return bytes;
}

VerifyReport verify(const std::vector<uint8_t>& b) {
    const uint8_t* p = b.empty() ? reinterpret_cast<const uint8_t*>("") : b.data();
    return McapVerifier::verify(p, b.size(), /*allow_trailing*/ true);
}

void test_empty_rejected() {
    LG_CASE("empty buffer rejected");
    std::vector<uint8_t> b;
    auto r = verify(b);
    CHECK(!r.ok);
    CHECK(r.has_error());
}

void test_short_garbage() {
    LG_CASE("short garbage rejected");
    std::vector<uint8_t> b = {1, 2, 3, 4};
    CHECK(!verify(b).ok);
}

void test_magic_only() {
    LG_CASE("magic only not ok without footer");
    std::vector<uint8_t> b(kMagic, kMagic + 9);
    auto r = verify(b);
    CHECK(!r.ok);
    CHECK(!r.has_error());
}

void test_valid_accepted() {
    LG_CASE("valid file accepted");
    auto b = make_valid_mcap();
    if (b.empty()) { CHECK(!"could not build valid mcap"); return; }
    auto r = verify(b);
    if (!r.ok) fprintf(stderr, "%s\n", r.render().c_str());
    CHECK(r.ok);
    CHECK(r.has_footer);
    CHECK(r.channel_count >= 1);
    CHECK(r.message_count >= 1);
}

void test_truncated() {
    LG_CASE("truncated middle never reported ok");
    auto b = make_valid_mcap();
    if (b.empty()) { CHECK(!"fixture"); return; }
    b.resize(b.size() / 2);
    auto r = verify(b);
    CHECK(!r.ok);
    CHECK(r.parsed_bytes <= b.size());
}

void test_zero_injection_bounded() {
    LG_CASE("zero byte injection stays bounded");
    auto b = make_valid_mcap();
    if (b.empty()) { CHECK(!"fixture"); return; }
    for (size_t i = 8; i + 9 < b.size(); i += 37) b[i] = 0x00;
    auto r = verify(b);
    CHECK(r.parsed_bytes <= b.size());
}

void test_huge_declared_length() {
    LG_CASE("huge declared length rejected, no allocation");
    std::vector<uint8_t> b(kMagic, kMagic + 9);
    b.push_back(0x05); // Message
    uint64_t huge = 1ull << 60;
    for (int i = 0; i < 8; ++i) b.push_back(uint8_t(huge >> (8 * i)));
    b.push_back(0xFF);
    auto r = verify(b);
    CHECK(!r.ok);
    CHECK(r.has_error());
}

void test_random_bytes_no_crash() {
    LG_CASE("random bytes after magic never crash");
    uint32_t seed = 0x12345678;
    for (int trial = 0; trial < 200; ++trial) {
        std::vector<uint8_t> b(kMagic, kMagic + 9);
        seed = seed * 1664525u + 1013904223u;
        size_t n = 1 + (seed % 128);
        for (size_t i = 0; i < n; ++i) {
            seed = seed * 1664525u + 1013904223u;
            b.push_back(uint8_t(seed >> 16));
        }
        auto r = verify(b);
        CHECK(r.parsed_bytes <= b.size());
    }
}

void test_extract_logs() {
    LG_CASE("extract logs from valid package");
    auto b = make_valid_mcap();
    if (b.empty()) { CHECK(!"fixture"); return; }
    std::vector<McapVerifier::LogMessage> msgs;
    bool ok = McapVerifier::extract_logs(b.data(), b.size(), msgs);
    CHECK(ok);
    bool found = false;
    for (auto& m : msgs)
        if (m.json.find("hello") != std::string::npos) found = true;
    CHECK(found);
}

void test_manifest_channel() {
    LG_CASE("manifest message counted separately, not as a log");
    const std::string path = "/tmp/lgverify_manifest.lgp";
    remove(path.c_str());
    {
        lg::McapWriter w;
        if (!w.open(path)) { CHECK(!"open"); return; }
        w.begin("none");
        w.write_manifest(1,
            R"({"producer":"logguard","version":"0.4.0","events":2})");
        w.write_log(1, 2, 1, 100, 1, 1, "net", "r1", "one");
        w.write_log(2, 4, 2, 200, 2, 2, "net", "r2", "two");
        w.finish();
    }
    auto r = McapVerifier::verify_file(path);
    CHECK(r.ok);
    CHECK_EQ(r.message_count, 2u);   // only /logguard/logs
    CHECK_EQ(r.manifest_count, 1u);  // provenance manifest
    // Two channels registered (logs + manifest), maybe more schemas.
    CHECK(r.channel_count >= 2);

    std::vector<uint8_t> bytes;
    {
        FILE* fp = std::fopen(path.c_str(), "rb");
        std::fseek(fp, 0, SEEK_END);
        long n = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        bytes.resize(size_t(n));
        size_t rd = std::fread(bytes.data(), 1, bytes.size(), fp);
        std::fclose(fp);
        CHECK_EQ(rd, bytes.size());
    }
    std::vector<McapVerifier::LogMessage> msgs;
    CHECK(McapVerifier::extract_logs(bytes.data(), bytes.size(), msgs));
    CHECK_EQ(msgs.size(), 2u); // manifest must not leak into log extraction

    LG_CASE("manifest payload extracted verbatim via extract_manifest_file");
    std::string manifest_json;
    CHECK(McapVerifier::extract_manifest_file(path, manifest_json));
    CHECK(manifest_json.find("\"producer\":\"logguard\"") !=
          std::string::npos);
    CHECK(manifest_json.find("\"events\":2") != std::string::npos);

    // Same extraction must work through a zstd-compressed chunk, where the
    // manifest sits inside the inflated body rather than the top-level stream.
    if (lg::export_::ZstdCodec::instance().available()) {
        LG_CASE("manifest extracted from a zstd-compressed chunk");
        const std::string zpath = "/tmp/lgverify_manifest_zstd.lgp";
        remove(zpath.c_str());
        {
            lg::McapWriter zw;
            if (zw.open(zpath)) {
                zw.begin("zstd");
                zw.write_manifest(7,
                    R"({"producer":"logguard","system":{"load_avg_1m":1.5}})");
                zw.write_log(1, 2, 1, 100, 1, 1, "net", "r1", "one");
                zw.finish();
                std::string zjson;
                CHECK(McapVerifier::extract_manifest_file(zpath, zjson));
                CHECK(zjson.find("load_avg_1m") != std::string::npos);
            }
        }
    }
}

void test_nonexistent_file() {
    LG_CASE("nonexistent file reported as error");
    auto r = McapVerifier::verify_file("/tmp/lg_no_such_file_xyz.lgp");
    CHECK(!r.ok);
    CHECK(r.has_error());
}

} // namespace

void run_all_tests() {
    test_empty_rejected();
    test_short_garbage();
    test_magic_only();
    test_valid_accepted();
    test_truncated();
    test_zero_injection_bounded();
    test_huge_declared_length();
    test_random_bytes_no_crash();
    test_extract_logs();
    test_manifest_channel();
    test_nonexistent_file();
}

LG_TEST_MAIN()
