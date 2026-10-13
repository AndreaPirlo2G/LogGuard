#pragma once
// Thin cross-OS shared-memory/file mapping abstraction.
// The ring buffer lives in a memory-mapped file; isolating the mapping here
// keeps the core recorder portable (POSIX shm_open/mmap vs Win32 CreateFileMapping).
#include <cstddef>
#include <cstdint>
#include <string>

namespace lg::plat {

struct Mapping {
    void*    base = nullptr;
    size_t   size = 0;
    intptr_t handle = -1; // fd on POSIX, HANDLE on Windows
    bool     read_only = false;
};

// Create (or open) a file-backed shared mapping of exactly `size` bytes.
// If the file is new it is grown to `size`; an existing file must be >= size.
bool map_create(const std::string& path, size_t size, Mapping& out);

// Open an existing mapping read/write without resizing (offline recover needs
// to retire the crashed epoch).
bool map_open_rw(const std::string& path, Mapping& out);

// Open an existing mapping read-only (used by `lgtool tail`).

void unmap(Mapping& m);

} // namespace lg::plat
