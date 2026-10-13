#include "diagnosis/symbolize.hpp"

#include <cstdio>
#include <cstring>
#include <utility>

#ifndef _WIN32
#include <dlfcn.h>
#endif

namespace lg::diag {

Symbolizer::Symbolizer() = default;

void Symbolizer::load_self() {
    regions_.clear();
#ifndef _WIN32
    FILE* f = std::fopen("/proc/self/maps", "re");
    if (f) {
        char line[1024];
        while (std::fgets(line, sizeof(line), f)) {
            unsigned long start = 0, end = 0, off = 0;
            char perms[8] = {0};
            // Path (if any) starts at column 73 in practice; parse permissively.
            if (std::sscanf(line, "%lx-%lx %7s %lx", &start, &end, perms,
                            &off) < 3)
                continue;
            Region r;
            r.start = start;
            r.end = end;
            r.file_off = off;
            r.exec = perms[0] && std::strchr(perms, 'x') != nullptr;
            const char* p = std::strchr(line, ' ');
            for (int i = 0; i < 5 && p; ++i) {
                while (*p == ' ') ++p;
                p = std::strchr(p, ' ');
            }
            if (p) {
                while (*p == ' ' || *p == '\t') ++p;
                const char* nl = std::strchr(p, '\n');
                r.path.assign(p, nl ? static_cast<size_t>(nl - p)
                                    : std::strlen(p));
            }
            // Backing file mappings only (skip [heap], [stack], anonymous).
            const bool file_backed =
                !r.path.empty() && r.path[0] != '[' && r.path[0] == '/';
            if (file_backed) regions_.push_back(std::move(r));
        }
        std::fclose(f);
    }
#endif
    loaded_ = true;
}

ResolvedFrame Symbolizer::resolve(uint64_t pc) const {
    ResolvedFrame fr;
    fr.addr = pc;
    for (const Region& r : regions_) {
        if (pc >= r.start && pc < r.end) {
            fr.module = r.path;
            // ELF file offset = mapping file offset + (pc - map start).
            fr.file_offset = r.file_off + (pc - r.start);
            break;
        }
    }
#ifndef _WIN32
    Dl_info info;
    if (dladdr(reinterpret_cast<void*>(static_cast<uintptr_t>(pc)), &info) &&
        info.dli_sname) {
        fr.nearest = info.dli_sname;
    }
#endif
    return fr;
}

std::string Symbolizer::render_stack(const uint64_t* frames,
                                     std::size_t n) const {
    if (!loaded_) const_cast<Symbolizer*>(this)->load_self();
    std::string out;
    char num[32];
    for (std::size_t i = 0; i < n; ++i) {
        ResolvedFrame fr = resolve(frames[i]);
        std::snprintf(num, sizeof(num), "#%-2zu 0x%016llx", i,
                      static_cast<unsigned long long>(fr.addr));
        out += num;
        if (!fr.module.empty()) {
            out += " in ";
            out += fr.module;
            std::snprintf(num, sizeof(num), "+0x%llx",
                          static_cast<unsigned long long>(fr.file_offset));
            out += num;
        }
        if (!fr.nearest.empty()) {
            out += " [";
            out += fr.nearest;
            out += ']';
        }
        out += '\n';
    }
    return out;
}

} // namespace lg::diag
