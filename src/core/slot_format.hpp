#pragma once
// Packed on-disk binary layout shared by writer, reader and the platform crash
// handler (a fatal handler writes directly into FileHeader). Keep POD and
// fixed-size; static_asserts below lock the dimensions.
#include "constants.hpp"

#include <cstddef>
#include <cstdint>

namespace lg::core {

#pragma pack(push, 1)

struct Slot {
    uint32_t magic;                                   // 0  RING_MAGIC_NUMBER when published
    uint32_t epoch;                                   // 4
    uint64_t ticket;                                  // 8  global sequence
    int64_t  mono_ns;                                 // 16
    int64_t  wall_ns;                                 // 24
    uint64_t tid;                                     // 32
    uint16_t level;                                   // 40
    uint16_t flags;                                   // 42
    uint16_t msg_len;                                 // 44
    uint16_t cat_len;                                 // 46
    uint16_t req_len;                                 // 48
    char     category[CATEGORY_CAP];                  // 50
    char     req_id[REQ_CAP];                         // 66
    char     payload[PAYLOAD_CAP];                    // 90..256
};

struct FileHeader {
    char     magic[8];          // RING_MAGIC_FILE
    uint32_t version;
    uint32_t slot_size;
    uint64_t slot_count;
    int32_t  pid;
    uint32_t epoch;
    uint32_t clean;
    uint32_t crashed;
    int32_t  sig;
    uint64_t fault_addr;
    int64_t  crash_mono_ns;
    uint64_t next_ticket;       // offset 68
    // v2 extensions
    uint32_t runtime_level;
    int32_t  crash_nframes;
    uint64_t crash_frames[CRASH_FRAMES];
    uint64_t n_truncated;
    uint64_t n_snapshots;
    uint64_t n_stalls;
    uint8_t  reserved[HEADER_SIZE - 68 - 4 - 4 - 8 * CRASH_FRAMES - 24];
};

#pragma pack(pop)

static_assert(sizeof(Slot) == SLOT_SIZE, "slot must stay 256 bytes");
static_assert(sizeof(FileHeader) == HEADER_SIZE, "header must stay one page");

inline Slot* slot_at(void* base, uint64_t index) {
    return reinterpret_cast<Slot*>(static_cast<char*>(base) + HEADER_SIZE) + index;
}
inline const Slot* slot_at(const void* base, uint64_t index) {
    return reinterpret_cast<const Slot*>(static_cast<const char*>(base) + HEADER_SIZE) + index;
}
inline FileHeader* header_at(void* base) {
    return reinterpret_cast<FileHeader*>(base);
}
inline const FileHeader* header_at(const void* base) {
    return reinterpret_cast<const FileHeader*>(base);
}

} // namespace lg::core
