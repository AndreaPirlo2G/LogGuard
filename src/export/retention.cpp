#include "export/retention.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace lg {
namespace export_ {

namespace {

bool ends_with(const std::string& s, const char* suffix) {
    size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

} // namespace

RetentionResult apply_retention(const std::vector<RetainedFile>& files,
                                const RetentionPolicy& policy,
                                int64_t now_ns) {
    RetentionResult result;

    // Newest first; tie on path for deterministic behaviour.
    std::vector<const RetainedFile*> order;
    order.reserve(files.size());
    for (const auto& f : files) order.push_back(&f);
    std::sort(order.begin(), order.end(),
              [](const RetainedFile* a, const RetainedFile* b) {
                  if (a->mtime_ns != b->mtime_ns)
                      return a->mtime_ns > b->mtime_ns;
                  return a->path < b->path;
              });

    uint64_t kept_bytes = 0;
    uint32_t kept_count = 0;
    // Walk newest -> oldest, keeping while every bound allows it.
    std::vector<const RetainedFile*> keep;
    for (const RetainedFile* f : order) {
        if (policy.max_files > 0 && kept_count >= policy.max_files) break;
        if (policy.max_age_ns > 0 &&
            now_ns - f->mtime_ns > policy.max_age_ns)
            break; // this and everything older is dropped
        if (policy.max_total_bytes > 0 &&
            kept_bytes + f->size_bytes > policy.max_total_bytes) {
            // Skip this file but continue: a later (older) small file might
            // still fit and retaining it is strictly better for diagnostics.
            continue;
        }
        keep.push_back(f);
        kept_bytes += f->size_bytes;
        ++kept_count;
    }

    // Build a kept set; everything not kept is deleted.
    std::vector<uint8_t> is_kept(files.size(), 0);
    for (const RetainedFile* k : keep)
        for (size_t i = 0; i < files.size(); ++i)
            if (&files[i] == k) { is_kept[i] = 1; break; }

    for (size_t i = 0; i < files.size(); ++i) {
        if (is_kept[i]) continue;
        result.deleted.push_back(files[i].path);
        result.bytes_freed += files[i].size_bytes;
    }
    result.bytes_kept = kept_bytes;
    result.files_kept = kept_count;
    return result;
}

std::vector<RetainedFile> enumerate_packages(const std::string& dir,
                                             bool* found) {
    std::vector<RetainedFile> out;
    DIR* d = opendir(dir.c_str());
    if (!d) {
        if (found) *found = false;
        return out;
    }
    if (found) *found = true;

    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        if (!ends_with(e->d_name, ".lgp")) continue;
        std::string full = dir;
        full.push_back('/');
        full += e->d_name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        RetainedFile rf;
        rf.path = std::move(full);
        rf.mtime_ns = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL +
                      st.st_mtim.tv_nsec;
        rf.size_bytes = static_cast<uint64_t>(st.st_size);
        out.push_back(std::move(rf));
    }
    closedir(d);
    return out;
}

RetentionResult enforce_retention(const std::string& dir,
                                  const RetentionPolicy& policy,
                                  int64_t now_ns) {
    std::vector<RetainedFile> files = enumerate_packages(dir);
    RetentionResult decision = apply_retention(files, policy, now_ns);
    for (const std::string& p : decision.deleted) {
        if (std::remove(p.c_str()) != 0) {
            // Best effort: the path is still reported as pruned; a later run
            // will see and retry it.
        }
    }
    return decision;
}

} // namespace export_
} // namespace lg
