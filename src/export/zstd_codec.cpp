#include "zstd_codec.h"

#include <dlfcn.h>

namespace lg::export_ {

ZstdCodec& ZstdCodec::instance() {
    static ZstdCodec z;
    return z;
}

ZstdCodec::ZstdCodec() {
    static const char* names[] = {
        "libzstd.so.1", "libzstd.so", "libzstd.1.dylib", nullptr};
    for (int i = 0; names[i] && !handle_; ++i)
        handle_ = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!handle_) return;
    cc_bound_ = reinterpret_cast<unsigned long long (*)(unsigned long long)>(
        dlsym(handle_, "ZSTD_compressBound"));
    cc_ = reinterpret_cast<size_t (*)(void*, size_t, const void*, size_t, int)>(
        dlsym(handle_, "ZSTD_compress"));
    dc_ = reinterpret_cast<size_t (*)(void*, size_t, const void*, size_t)>(
        dlsym(handle_, "ZSTD_decompress"));
    is_error_ = reinterpret_cast<unsigned (*)(size_t)>(
        dlsym(handle_, "ZSTD_isError"));
    if (!cc_bound_ || !cc_ || !dc_) { dlclose(handle_); handle_ = nullptr; }
}

ZstdCodec::~ZstdCodec() { if (handle_) dlclose(handle_); }

std::vector<uint8_t> ZstdCodec::compress(const uint8_t* data, size_t n, int level) {
    std::vector<uint8_t> out;
    if (!handle_) return out;
    unsigned long long cap = cc_bound_(n);
    out.resize(size_t(cap));
    size_t r = cc_(out.data(), out.size(), data, n, level);
    // ZSTD_isError: error codes have the high bit set in the stable ABI.
    if (r & (size_t(1) << (sizeof(size_t) * 8 - 1))) { out.clear(); return out; }
    out.resize(r);
    return out;
}

std::vector<uint8_t> ZstdCodec::decompress(const uint8_t* data, size_t n, size_t expected) {
    std::vector<uint8_t> out;
    if (!handle_ || !dc_ || expected == 0 || expected > (64ull << 20)) return out;
    out.resize(expected);
    size_t r = dc_(out.data(), out.size(), data, n);
    bool err = is_error_ ? is_error_(r)
                         : bool(r & (size_t(1) << (sizeof(size_t) * 8 - 1)));
    if (err || r != expected) out.clear();
    return out;
}

} // namespace lg::export_
