#pragma once
// Slot flag bits describing how to interpret Slot::payload.
#include <cstdint>

namespace lg::core {

enum SlotFlags : uint16_t {
    FLAG_PLAIN = 0,
    // payload holds raw uint64 return addresses; msg_len = n * sizeof(uint64_t).
    FLAG_STACK = 1u << 0,
    // payload is "<msg>\0<fields_json>"; msg_len = total bytes.
    FLAG_KV    = 1u << 1,
    // payload is a lazy::Encoded blob (fmt + tags + primitive values).
    FLAG_LAZY  = 1u << 2,
};

} // namespace lg::core
