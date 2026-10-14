#include "diagnosis/symbolize.hpp"

#include <cstdio>
#include <cstring>
#include <utility>

#ifndef _WIN32
#include <dlfcn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <sstream>

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
            // Tokenize: range perms offset dev inode pathname.
            // The pathname (if present) is everything left after the first
            // five whitespace-separated fields, stripped of surrounding
            // spaces.
            int field = 0;
            const char* p = line;
            while (field < 5 && *p) {
                while (*p == ' ' || *p == '\t') ++p;
                const char* tok = p;
                while (*p && *p != ' ' && *p != '\t' && *p != '\n') ++p;
                if (tok != p) ++field;
                while (*p == ' ' || *p == '\t') ++p;
            }
            if (*p && *p != '\n') {
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

void Symbolizer::enable_source_locations(bool on) {
    source_enabled_ = on;
#ifndef _WIN32
    if (on && addr2line_checked_ < 0) {
        // Probe once: `addr2line --version` succeeds when binutils is present.
        int rc = std::system("command -v addr2line >/dev/null 2>&1");
        addr2line_checked_ = (rc == 0) ? 1 : 0;
    }
#endif
}

void Symbolizer::resolve_module(const std::string& module,
                                const std::vector<uint64_t>& offsets) const {
#ifndef _WIN32
    if (addr2line_checked_ != 1 || offsets.empty()) return;
    // Build: addr2line -f -C -e <module> 0x.. 0x.. ...
    std::string cmd = "addr2line -f -C -e '";
    for (char c : module) {
        if (c == '\'') cmd += "'\\''";
        else cmd += c;
    }
    cmd += '\'';
    char num[32];
    for (uint64_t off : offsets) {
        std::snprintf(num, sizeof(num), " 0x%llx",
                      static_cast<unsigned long long>(off));
        cmd += num;
    }
    cmd += " 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) { addr2line_checked_ = 0; return; }
    char line[512];
    // addr2line emits two lines per address: function, then file:line.
    for (size_t i = 0; i < offsets.size(); ++i) {
        LineInfo li;
        if (std::fgets(line, sizeof(line), p)) {
            li.function = line;
            if (!li.function.empty() && li.function.back() == '\n')
                li.function.pop_back();
        }
        if (std::fgets(line, sizeof(line), p)) {
            li.location = line;
            if (!li.location.empty() && li.location.back() == '\n')
                li.location.pop_back();
        }
        line_cache_[{module, offsets[i]}] = li;
    }
    pclose(p);
#else
    (void)module;
    (void)offsets;
#endif
}

LineInfo Symbolizer::source_location(const ResolvedFrame& fr) const {
    LineInfo empty;
    if (!source_enabled_ || fr.module.empty()) return empty;
    auto key = std::make_pair(fr.module, fr.file_offset);
    auto it = line_cache_.find(key);
    if (it == line_cache_.end()) {
        // Lazy single-frame batch (one addr2line process per module).
        resolve_module(fr.module, {fr.file_offset});
        it = line_cache_.find(key);
    }
    if (it != line_cache_.end()) return it->second;
    return empty;
}

std::string Symbolizer::render_stack(const uint64_t* frames,
                                     std::size_t n) const {
    if (!loaded_) const_cast<Symbolizer*>(this)->load_self();
    // Resolve all PCs first so source locations can be fetched with one
    // addr2line process per module instead of one per frame.
    std::vector<ResolvedFrame> resolved(n);
    std::map<std::string, std::vector<uint64_t>> pending;
    for (std::size_t i = 0; i < n; ++i) {
        resolved[i] = resolve(frames[i]);
        if (source_enabled_ && !resolved[i].module.empty() &&
            modules_resolved_.find(resolved[i].module) ==
                modules_resolved_.end()) {
            pending[resolved[i].module].push_back(resolved[i].file_offset);
        }
    }
    for (auto& kv : pending) {
        // addr2line prints one line pair per requested address; dedup offsets
        // so the output count matches our request even with repeated frames.
        std::sort(kv.second.begin(), kv.second.end());
        kv.second.erase(std::unique(kv.second.begin(), kv.second.end()),
                        kv.second.end());
        resolve_module(kv.first, kv.second);
        modules_resolved_[kv.first] = true;
    }

    std::string out;
    char num[48];
    for (std::size_t i = 0; i < n; ++i) {
        const ResolvedFrame& fr = resolved[i];
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
        LineInfo li = source_location(fr);
        if (!li.location.empty() && li.location != "??:0") {
            out += " at ";
            if (!li.function.empty() && li.function != "??")
                out += li.function + " (";
            out += li.location;
            if (!li.function.empty() && li.function != "??")
                out += ')';
        }
        out += '\n';
    }
    return out;
}

} // namespace lg::diag
