#pragma once
// Deferred formatting encoder/renderer.
//
// Hot path stores a truncated printf-style format string plus up to MAX_ARGS
// captured values (no formatting on the producing thread); the human string is
// rendered later on the export/tail path. Supported conversions: %d/%lld,
// %u/%llu, %x/%p, %s. This is the v0.2 low-overhead path (LG_LOG_F); the
// classic printf macros still format synchronously.
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace lg::lazy {

constexpr uint8_t TAG_I64 = 1, TAG_U64 = 2, TAG_STR = 3;
constexpr int     MAX_ARGS = 6;
constexpr int     FMT_CAP = 96;

struct Encoded {
    char    fmt[FMT_CAP];
    uint8_t tags[MAX_ARGS];
    uint64_t vals[MAX_ARGS]; // STR stores a pointer into the (short-lived) arg
    int n = 0;
};

// Encode from a va_list whose arguments are known by scanning the conversions.
// String pointers MUST remain valid until render() (caller stores them in the
// slot's inline region immediately after).
inline void encode(Encoded& e, const char* fmt, va_list ap) {
    e = Encoded{}; // value-init: zeroes arrays, n=0, no class-memaccess warning
    size_t fl = std::strlen(fmt);
    if (fl > FMT_CAP - 1) fl = FMT_CAP - 1;
    std::memcpy(e.fmt, fmt, fl);
    for (size_t i = 0; i < fl && e.n < MAX_ARGS; ++i) {
        if (fmt[i] != '%' || i + 1 >= fl) continue;
        char c = fmt[++i];
        while (c && std::strchr("-+ #0.hljztL*", c) && i + 1 < fl) c = fmt[++i];
        switch (c) {
            case 'd': case 'i':
                e.tags[e.n] = TAG_I64;
                e.vals[e.n] = static_cast<uint64_t>(va_arg(ap, long long));
                ++e.n; break;
            case 'u':
                e.tags[e.n] = TAG_U64;
                e.vals[e.n] = va_arg(ap, unsigned long long);
                ++e.n; break;
            case 'x': case 'X': case 'p':
                e.tags[e.n] = TAG_U64;
                e.vals[e.n] = va_arg(ap, unsigned long long);
                ++e.n; break;
            case 's':
                e.tags[e.n] = TAG_STR;
                e.vals[e.n] = reinterpret_cast<uint64_t>(va_arg(ap, const char*));
                ++e.n; break;
            case '%': break;
            default: break;
        }
    }
}

// Render into `out` using small fixed per-conversion buffers.
inline void render(const Encoded& e, std::string& out) {
    int ai = 0;
    char num[40];
    for (size_t i = 0; i < std::strlen(e.fmt);) {
        if (e.fmt[i] != '%') { out.push_back(e.fmt[i++]); continue; }
        // Copy the whole conversion spec verbatim, then substitute value.
        size_t start = i++;
        while (i < std::strlen(e.fmt) && std::strchr("-+ #0.hljztL*", e.fmt[i])) ++i;
        char conv = (i < std::strlen(e.fmt)) ? e.fmt[i] : '\0';
        char spec[24];
        size_t sl = std::min(sizeof(spec) - 1, i - start + 1);
        std::memcpy(spec, e.fmt + start, sl); spec[sl] = '\0';
        if (conv == '%') { out.push_back('%'); ++i; continue; }
        if (ai >= e.n) { out += spec; if (conv) ++i; continue; }
        uint8_t tag = e.tags[ai]; uint64_t v = e.vals[ai]; ++ai;
        if (tag == TAG_STR) {
            const char* s = reinterpret_cast<const char*>(v);
            // Use plain %s with width/flags honored by snprintf.
            std::snprintf(num, sizeof(num), spec, s ? s : "(null)");
            out += num;
        } else if (tag == TAG_I64) {
            std::snprintf(num, sizeof(num), spec, static_cast<long long>(v));
            out += num;
        } else {
            if (conv == 'p') std::snprintf(num, sizeof(num), "%p",
                                           reinterpret_cast<void*>(v));
            else if (conv == 'x' || conv == 'X')
                std::snprintf(num, sizeof(num), spec, v);
            else std::snprintf(num, sizeof(num), spec, v);
            out += num;
        }
        if (conv) ++i;
    }
}

} // namespace lg::lazy
