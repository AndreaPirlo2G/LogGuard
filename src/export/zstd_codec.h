#pragma once
// Optional zstd compression loaded at runtime via dlopen. Zero hard
// dependency: if libzstd is unavailable (embedded targets, minimal images),
// LogGuard still writes valid uncompressed MCAP. The reader does not care.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lg::export_ {

class ZstdCodec {
public:
    // Finds libzstd.so.1 (or common sonames); available() stays false if absent.
    static ZstdCodec& instance();
    bool available() const { return handle_ != nullptr; }

    // Returns compressed bytes, or an empty vector on failure (caller falls
    // back to storing the data uncompressed).
    std::vector<uint8_t> compress(const uint8_t* data, size_t n, int level = 3);

    // Decompress exactly `expected` bytes; empty on failure/unavailable.
    std::vector<uint8_t> decompress(const uint8_t* data, size_t n, size_t expected);
private:
    ZstdCodec();
    ~ZstdCodec();
    void* handle_ = nullptr;
    unsigned long long (*cc_bound_)(unsigned long long) = nullptr;
    size_t (*cc_)(void*, size_t, const void*, size_t, int) = nullptr;
    size_t (*dc_)(void*, size_t, const void*, size_t) = nullptr;
    unsigned (*is_error_)(size_t) = nullptr;
};

} // namespace lg::export_
