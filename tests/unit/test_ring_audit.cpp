// Ring health audit tests: build synthetic ring images and verify the scanner
// classifies intact, torn, foreign-epoch and empty slots correctly.
#include <cstdint>
#include <cstring>
#include <vector>

#include "lgtest.hpp"
#include "core/ring_audit.hpp"
#include "core/slot_format.hpp"
#include "core/recovery.hpp"

using namespace lg::core;

namespace {

constexpr uint64_t kCap = 64;

struct RingImage {
    std::vector<uint8_t> mem;
    void* base() { return mem.data(); }
};

RingImage make_image() {
    RingImage img;
    img.mem.assign(HEADER_SIZE + kCap * SLOT_SIZE, 0);
    FileHeader& h = *header_at(img.base());
    initialize_header(h, kCap, /*pid*/ 1234, /*level*/ 2);
    return img;
}

Slot* slot_of(void* base, uint64_t i) { return slot_at(base, i); }

void publish(Slot& s, uint64_t ticket, uint32_t epoch) {
    std::memset(&s, 0, sizeof(s));
    s.ticket = ticket;
    s.epoch = epoch;
    s.level = 2;
    s.msg_len = 5;
    s.cat_len = 0;
    s.req_len = 0;
    __atomic_store_n(&s.magic, RING_MAGIC_NUMBER, __ATOMIC_RELEASE);
}

void test_all_empty_is_healthy() {
    LG_CASE("fresh ring: all empty, healthy");
    auto img = make_image();
    auto a = audit_ring(img.base(), kCap);
    CHECK(a.header_ok);
    CHECK_EQ(a.empty, kCap);
    CHECK_EQ(a.valid, (uint64_t)0);
    CHECK(a.healthy());
}

void test_valid_slots_counted() {
    LG_CASE("published current-epoch slots are valid");
    auto img = make_image();
    uint32_t ep = header_at(img.base())->epoch;
    for (uint64_t i = 0; i < 10; ++i)
        publish(*slot_of(img.base(), i), /*ticket=*/i, ep);
    auto a = audit_ring(img.base(), kCap);
    CHECK_EQ(a.valid, (uint64_t)10);
    CHECK_EQ(a.empty, kCap - 10);
    CHECK_EQ(a.min_ticket, (uint64_t)0);
    CHECK_EQ(a.max_ticket, (uint64_t)9);
    CHECK_EQ(a.distinct_tickets, (uint64_t)10);
    CHECK(a.healthy());
}

void test_wrapped_tickets_match_index() {
    LG_CASE("wrapped tickets still match their physical index");
    auto img = make_image();
    uint32_t ep = header_at(img.base())->epoch;
    // ticket 64 maps to physical index 0 in a 64-slot ring.
    publish(*slot_of(img.base(), 0), /*ticket=*/64, ep);
    publish(*slot_of(img.base(), 1), /*ticket=*/65, ep);
    auto a = audit_ring(img.base(), kCap);
    CHECK_EQ(a.valid, (uint64_t)2);
    CHECK_EQ(a.ticket_mismatch, (uint64_t)0);
}

void test_ticket_index_mismatch_flagged() {
    LG_CASE("magic good but ticket/index mismatch is flagged");
    auto img = make_image();
    uint32_t ep = header_at(img.base())->epoch;
    publish(*slot_of(img.base(), 0), /*ticket=*/7, ep); // 7 & 63 != 0
    auto a = audit_ring(img.base(), kCap);
    CHECK_EQ(a.ticket_mismatch, (uint64_t)1);
    CHECK_EQ(a.valid, (uint64_t)0);
    CHECK(!a.healthy());
}

void test_foreign_epoch_separated() {
    LG_CASE("intact slots from a prior epoch are foreign, not valid");
    auto img = make_image();
    uint32_t ep = header_at(img.base())->epoch;
    publish(*slot_of(img.base(), 0), 0, ep - 1);
    auto a = audit_ring(img.base(), kCap);
    CHECK_EQ(a.foreign_epoch, (uint64_t)1);
    CHECK_EQ(a.valid, (uint64_t)0);
}

void test_torn_lengths_flagged() {
    LG_CASE("impossible lengths classify a published slot as torn");
    auto img = make_image();
    uint32_t ep = header_at(img.base())->epoch;
    Slot& s = *slot_of(img.base(), 3);
    publish(s, 3, ep);
    s.msg_len = PAYLOAD_CAP + 10; // corrupt after publish
    auto a = audit_ring(img.base(), kCap);
    CHECK_EQ(a.torn, (uint64_t)1);
    CHECK_EQ(a.valid, (uint64_t)0);
}

void test_garbage_magic_is_torn() {
    LG_CASE("nonzero garbage magic is treated as torn, not empty");
    auto img = make_image();
    Slot& s = *slot_of(img.base(), 5);
    __atomic_store_n(&s.magic, 0xDEADBEEF, __ATOMIC_RELEASE);
    auto a = audit_ring(img.base(), kCap);
    CHECK_EQ(a.torn, (uint64_t)1);
    CHECK_EQ(a.empty, kCap - 1);
}

void test_null_and_zero_capacity() {
    LG_CASE("null base / zero capacity handled");
    auto a = audit_ring(nullptr, kCap);
    CHECK_EQ(a.capacity, (uint64_t)0);
    CHECK(!a.header_ok);
    auto b = audit_ring(nullptr, 0);
    CHECK_EQ(b.fill_ratio(), 0.0);
}

void test_bad_header_detected() {
    LG_CASE("corrupt file magic marks header not ok");
    auto img = make_image();
    FileHeader& h = *header_at(img.base());
    h.magic[0] = 'X';
    auto a = audit_ring(img.base(), kCap);
    CHECK(!a.header_ok);
}

} // namespace

void run_all_tests() {
    test_all_empty_is_healthy();
    test_valid_slots_counted();
    test_wrapped_tickets_match_index();
    test_ticket_index_mismatch_flagged();
    test_foreign_epoch_separated();
    test_torn_lengths_flagged();
    test_garbage_magic_is_torn();
    test_null_and_zero_capacity();
    test_bad_header_detected();
}

LG_TEST_MAIN()
