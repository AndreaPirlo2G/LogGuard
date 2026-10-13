#ifndef LOGGUARD_RING_LAYOUT_H
#define LOGGUARD_RING_LAYOUT_H
/*
 * Raw packed on-disk layout of the ring header/slots.
 *
 * This C-compatible header sits in the platform layer because the fatal
 * signal/VEH handler writes directly into FileHeader from an async context
 * and must not depend on higher C++ layers. core/slot_format.hpp wraps these
 * same definitions in the lg::core C++ namespace.
 */
#include <stdint.h>

#define LG_SLOT_SIZE    256u
#define LG_HEADER_SIZE  4096u
#define LG_CRASH_FRAMES 24u

#pragma pack(push, 1)
typedef struct lg_slot_t {
    uint32_t magic;
    uint32_t epoch;
    uint64_t ticket;
    int64_t  mono_ns;
    int64_t  wall_ns;
    uint64_t tid;
    uint16_t level;
    uint16_t flags;
    uint16_t msg_len;
    uint16_t cat_len;
    uint16_t req_len;
    char     category[16];
    char     req_id[24];
    char     payload[166];
} lg_slot_t;

typedef struct lg_hdr_s {
    char     magic[8];
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
    uint64_t next_ticket;
    uint32_t runtime_level;
    int32_t  crash_nframes;
    uint64_t crash_frames[LG_CRASH_FRAMES];
    uint64_t n_truncated;
    uint64_t n_snapshots;
    uint64_t n_stalls;
    uint8_t  reserved[LG_HEADER_SIZE - 68 - 4 - 4 - 8 * LG_CRASH_FRAMES - 24];
} lg_hdr_t;
#pragma pack(pop)

#endif /* LOGGUARD_RING_LAYOUT_H */
