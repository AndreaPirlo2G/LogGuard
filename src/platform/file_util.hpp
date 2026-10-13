#pragma once
// Small filesystem helpers shared by the recorder and the offline exporter.
// Kept here so no higher layer issues direct path syscalls.
#include <cstdint>
#include <string>

namespace lg::plat {

// Directory portion of a path ("a/b/c.ring" -> "a/b"); "." when no separator.
std::string parent_dir(const std::string& path);

// Join a directory and a file name with the platform separator.
std::string join_path(const std::string& dir, const std::string& name);

// Recursively create a directory and its parents. Returns true if the
// directory exists afterwards (already present counts as success).
bool ensure_dir(const std::string& path);

// Build an incident-package path: "<dir>/<prefix>-<wall_ns>.lgp".
std::string package_path(const std::string& dir, const char* prefix,
                         int64_t wall_ns);

} // namespace lg::plat
