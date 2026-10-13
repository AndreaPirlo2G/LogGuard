// Ring self-healing tests: torn/zero/corrupt headers with an intact arena.
#include <cstdint>
#include <cstring>
#include <vector>

#include "lgtest.hpp"
#include "core/ring_repair.hpp"
#include "core/slot_format.hpp"
#include "core/recovery.hpp"

using namespace lg::core;

namespace {

constexpr uint64_t kCap = 64;
constexpr uint64_t kFileSize = HEADER_SIZE + kCap * SLOT_SIZE;

struct Image {
    std::vector<uint8_t> mem;
    void* base() { return mem.data(); }
};

Image fresh_image() {
    Image img;
    img.mem.assign(kFileSize, 0);
    return img;
}

Slot* slot_of(void* base, uint64_t i) { return slot_at(base, i); }

void publish(Slot& s, uint64_t ticket, uint32_t epoch) {
    std::memset(&s, 0, sizeof(s));
    s.ticket = ticket;
    s.epoch = epoch;
    s.level = 2;
    s.msg_len = 3;
    __atomic_store_n(&s.magic, RING_MAGIC_NUMBER, __ATOMIC_RELEASE);
}

void init_valid_header(void* base) {
    FileHeader& h = *header_at(base);
    initialize_header(h, kCap, 100, 2);
}

void test_infer_capacity() {
    LG_CASE("capacity inferred as power of two from file size");
    CHECK_EQ(infer_capacity(0), (uint64_t)0);
    CHECK_EQ(infer_capacity(HEADER_SIZE), (uint64_t)0);
    CHECK_EQ(infer_capacity(HEADER_SIZE + SLOT_SIZE), (uint64_t)1);
    CHECK_EQ(infer_capacity(kFileSize), kCap);
    CHECK_EQ(infer_capacity(HEADER_SIZE + SLOT_SIZE * 63), (uint64_t)32);
}

void test_zero_header_is_fresh() {
    LG_CASE("all-zero header classified fresh");
    auto img = fresh_image();
    auto rep = diagnose(img.base(), kFileSize);
    CHECK(rep.state == HeaderState::FreshZero);
    CHECK(!rep.should_salvage());
}

void test_valid_header() {
    LG_CASE("matching header classified valid");
    auto img = fresh_image();
    init_valid_header(img.base());
    auto rep = diagnose(img.base(), kFileSize);
    CHECK(rep.state == HeaderState::Valid);
    CHECK(rep.header_readable);
}

void test_geometry_mismatch() {
    LG_CASE("header slot_count disagreeing with file is geometry mismatch");
    auto img = fresh_image();
    init_valid_header(img.base());
    header_at(img.base())->slot_count = 128; // file only holds 64
    auto rep = diagnose(img.base(), kFileSize);
    CHECK(rep.state == HeaderState::GeometryMismatch);
}

void test_corrupt_empty() {
    LG_CASE("nonzero garbage header, no slots -> corrupt empty");
    auto img = fresh_image();
    FileHeader& h = *header_at(img.base());
    std::memset(&h, 0xAB, 64); // nonzero but not the magic
    auto rep = diagnose(img.base(), kFileSize);
    CHECK(rep.state == HeaderState::CorruptEmpty);
    CHECK(!rep.should_salvage());
    CHECK(rep.should_reset_header());
}

void test_salvage_torn_header_with_data() {
    LG_CASE("torn header but intact slots are salvaged");
    auto img = fresh_image();
    // Write real slots first (simulating a live ring), then wreck the header.
    for (uint64_t i = 0; i < 12; ++i)
        publish(*slot_of(img.base(), i), i, /*epoch*/ 3);
    FileHeader& h = *header_at(img.base());
    std::memset(&h, 0x5A, 128); // destroy magic + geometry
    RepairReport rep;
    auto got = salvage_slots(img.base(), kFileSize, &rep);
    CHECK(rep.state == HeaderState::CorruptWithData);
    CHECK(rep.should_salvage());
    CHECK_EQ(rep.salvageable, (uint64_t)12);
    CHECK_EQ(rep.recovered_epoch, (uint32_t)3);
    CHECK_EQ(got.size(), (size_t)12);
    // ticket ordered
    for (size_t i = 1; i < got.size(); ++i)
        CHECK(got[i - 1].ticket <= got[i].ticket);
}

void test_salvage_picks_dominant_epoch() {
    LG_CASE("dominant (newest) epoch wins during salvage");
    auto img = fresh_image();
    // A handful of stale epoch-1 stragglers, many epoch-4 records. Tickets
    // must map to the physical index (ticket & 63 == i) to be consistent.
    publish(*slot_of(img.base(), 0), 0, 1);
    publish(*slot_of(img.base(), 1), 1, 1);
    for (uint64_t i = 2; i < 20; ++i)
        publish(*slot_of(img.base(), i), /*ticket=*/i, /*epoch*/ 4);
    FileHeader& h = *header_at(img.base());
    std::memset(&h, 0x33, 40);
    RepairReport rep;
    auto got = salvage_slots(img.base(), kFileSize, &rep);
    CHECK_EQ(rep.recovered_epoch, (uint32_t)4);
    CHECK_EQ(got.size(), (size_t)18);
    for (auto& s : got) CHECK_EQ(s.epoch, (uint32_t)4);
}

void test_torn_slots_excluded() {
    LG_CASE("published-but-inconsistent slots are counted torn, excluded");
    auto img = fresh_image();
    for (uint64_t i = 0; i < 8; ++i)
        publish(*slot_of(img.base(), i), i, 2);
    // Corrupt one slot's length after publish.
    slot_of(img.base(), 3)->msg_len = PAYLOAD_CAP + 5;
    FileHeader& h = *header_at(img.base());
    std::memset(&h, 0x77, 32);
    RepairReport rep;
    auto got = salvage_slots(img.base(), kFileSize, &rep);
    CHECK_EQ(rep.torn, (uint64_t)1);
    CHECK_EQ(got.size(), (size_t)7);
}

void test_null_and_too_small() {
    LG_CASE("null base and undersized files handled");
    auto rep = diagnose(nullptr, kFileSize);
    CHECK(!rep.should_salvage());
    CHECK_EQ(infer_capacity(kFileSize), kCap);
    auto small = fresh_image();
    auto got = salvage_slots(small.base(), HEADER_SIZE, nullptr);
    CHECK(got.empty());
}

} // namespace

void run_all_tests() {
    test_infer_capacity();
    test_zero_header_is_fresh();
    test_valid_header();
    test_geometry_mismatch();
    test_corrupt_empty();
    test_salvage_torn_header_with_data();
    test_salvage_picks_dominant_epoch();
    test_torn_slots_excluded();
    test_null_and_too_small();
}

LG_TEST_MAIN()
