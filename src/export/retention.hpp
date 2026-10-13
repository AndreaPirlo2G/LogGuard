// Retention / disk-quota management for exported incident packages.
//
// A flight recorder runs for months and exports packages on every trigger; it
// must never fill the robot's disk. RetentionPolicy prunes the oldest exported
// packages once any bound is exceeded:
//   - keep at most max_files packages
//   - keep at most max_total_bytes on disk
//   - drop packages older than max_age_ns (0 = no age limit)
//
// The policy decision is a pure function over an injectable list of file
// descriptors (RetainedFile), so it is fully unit-testable without touching
// the filesystem; a thin filesystem helper enumerates a real directory.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lg {
namespace export_ {

// One package considered for retention. `mtime_ns` is the file modification
// time (wall clock); `size_bytes` its on-disk size.
struct RetainedFile {
    std::string path;
    int64_t mtime_ns = 0;
    uint64_t size_bytes = 0;
};

struct RetentionPolicy {
    // Keep the newest N packages; 0 disables the count bound.
    uint32_t max_files = 50;
    // Keep total package bytes at/below this value; 0 disables the size bound.
    uint64_t max_total_bytes = 64ull * 1024 * 1024; // 64 MiB default
    // Drop packages whose age (relative to `now_ns`) exceeds this; 0 disables.
    int64_t max_age_ns = 0;

    bool any_bound() const {
        return max_files > 0 || max_total_bytes > 0 || max_age_ns > 0;
    }
};

struct RetentionResult {
    std::vector<std::string> deleted; // paths the caller should unlink
    uint64_t bytes_freed = 0;
    uint64_t bytes_kept = 0;
    uint32_t files_kept = 0;

    bool changed() const { return !deleted.empty(); }
};

// Decide which files to prune. Input order does not matter; newest files (by
// mtime, ties broken by path for determinism) are retained first. The result
// only lists paths to delete — the caller owns the actual unlink.
RetentionResult apply_retention(const std::vector<RetainedFile>& files,
                                const RetentionPolicy& policy,
                                int64_t now_ns);

// Filesystem helper: enumerate regular files under `dir` whose name matches
// the logguard package pattern (suffix ".lgp"), sorted oldest-first. Returns
// an empty vector for a missing/unreadable directory. `*found` (if non-null)
// is set to whether the directory exists. Purely reads metadata; never deletes.
std::vector<RetainedFile> enumerate_packages(const std::string& dir,
                                             bool* found = nullptr);

// Enumerate, decide and actually unlink pruned packages. Returns the decision
// result (deleted contains the paths that were removed). Failures to unlink a
// listed file are ignored best-effort and the path is still reported.
RetentionResult enforce_retention(const std::string& dir,
                                  const RetentionPolicy& policy,
                                  int64_t now_ns);

} // namespace export_
} // namespace lg
