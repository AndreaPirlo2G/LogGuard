#pragma once
// Epoch bookkeeping across process restarts. A crash leaves the header with
// clean==0; the next init (or the offline CLI) detects that, exports the final
// window, then retires the epoch so it is never exported twice.
#include "slot_format.hpp"

#include <cstring>

namespace lg::core {

inline constexpr char RING_FILE_MAGIC[8] = {'L','G','R','I','N','G','0','2'};

inline bool has_valid_magic(const FileHeader& h) {
    return std::memcmp(h.magic, RING_FILE_MAGIC, 8) == 0;
}

inline bool needs_recovery(const FileHeader& h) {
    return has_valid_magic(h) && h.clean == 0;
}

// Initialize a brand-new header (mapping was zero-filled).
inline void initialize_header(FileHeader& h, uint64_t slot_count, int pid,
                               int default_level) {
    std::memset(&h, 0, sizeof(h));
    std::memcpy(h.magic, RING_FILE_MAGIC, 8);
    h.version = 2;
    h.slot_size = SLOT_SIZE;
    h.slot_count = slot_count;
    h.pid = static_cast<int32_t>(pid);
    h.epoch = 1;
    h.clean = 0;
    h.runtime_level = static_cast<uint32_t>(default_level);
}

// Retire a crashed session after its window has been exported: bump epoch and
// clear crash state so the same records cannot be exported again.
inline void retire_crashed_epoch(FileHeader& h) {
    __atomic_add_fetch(&h.epoch, 1u, __ATOMIC_RELEASE);
    h.crashed = 0;
    h.crash_nframes = 0;
    h.clean = 1;
}

// Begin a new live session on an existing file.
inline void begin_session(FileHeader& h, int pid) {
    h.epoch += 1;
    h.clean = 0;
    h.crashed = 0;
    h.pid = static_cast<int32_t>(pid);
}

} // namespace lg::core
