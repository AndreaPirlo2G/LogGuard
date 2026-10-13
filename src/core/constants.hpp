#pragma once
// Fixed on-disk capacities. Bumping any of these changes the binary layout and
// requires bumping the ring format version + migration handling; do not tune
// casually.
#include <cstddef>
#include <cstdint>

namespace lg::core {

constexpr uint32_t RING_MAGIC_NUMBER = 0x3153474Cu; // "LGS1"
constexpr char     RING_MAGIC_FILE[8] = {'L','G','R','I','N','G','0','2'};

constexpr std::size_t SLOT_SIZE    = 256;    // one slot == one cache-line-ish unit
constexpr std::size_t HEADER_SIZE  = 4096;   // one page
constexpr std::size_t CATEGORY_CAP = 16;     // including NUL in practice
constexpr std::size_t REQ_CAP      = 24;
constexpr std::size_t PAYLOAD_CAP  = 166;    // SLOT_SIZE - 90 byte slot header
constexpr std::size_t CRASH_FRAMES = 24;

// Default ring holds 2^18 slots (~64 MiB); fixed-size, circular overwrite.
constexpr std::uint64_t DEFAULT_SLOT_COUNT = 1ull << 18;

constexpr int MAX_RULES = 32;

} // namespace lg::core
