// Windows port of the file-backed mapping. Built only on WIN32.
// Notes for the port: FILE_FLAG_DELETE_ON_CLOSE is intentionally not used (the
// ring must survive a killed process and be re-opened on the next launch); the
// file is created with a security descriptor restricting access to the user.
#include "mapping.h"

#ifndef _WIN32
#error "mapping_windows.cpp is Windows-only"
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace lg::plat {

bool map_create(const std::string& path, size_t size, Mapping& out) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER need{}; need.QuadPart = static_cast<LONGLONG>(size);
    LARGE_INTEGER cur{};
    if (!GetFileSizeEx(h, &cur) || cur.QuadPart < need.QuadPart) {
        if (!SetFilePointerEx(h, need, nullptr, FILE_BEGIN) || !SetEndOfFile(h)) {
            CloseHandle(h); return false;
        }
    }
    HANDLE m = CreateFileMappingA(h, nullptr, PAGE_READWRITE,
                                  static_cast<DWORD>(size >> 32),
                                  static_cast<DWORD>(size & 0xffffffff), nullptr);
    if (!m) { CloseHandle(h); return false; }
    void* p = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, size);
    CloseHandle(m);
    if (!p) { CloseHandle(h); return false; }
    out = {p, size, reinterpret_cast<intptr_t>(h), false};
    return true;
}

bool map_open_rw(const std::string& path, Mapping& out) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return false; }
    size_t size = static_cast<size_t>(sz.QuadPart);
    HANDLE m = CreateFileMappingA(h, nullptr, PAGE_READWRITE,
                                  DWORD(size >> 32), DWORD(size & 0xffffffff), nullptr);
    if (!m) { CloseHandle(h); return false; }
    void* p = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, size);
    CloseHandle(m);
    if (!p) { CloseHandle(h); return false; }
    out = {p, size, reinterpret_cast<intptr_t>(h), false};
    return true;
}

bool map_open_ro(const std::string& path, Mapping& out) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return false; }
    size_t size = static_cast<size_t>(sz.QuadPart);
    HANDLE m = CreateFileMappingA(h, nullptr, PAGE_READONLY,
                                  static_cast<DWORD>(size >> 32),
                                  static_cast<DWORD>(size & 0xffffffff), nullptr);
    if (!m) { CloseHandle(h); return false; }
    void* p = MapViewOfFile(m, FILE_MAP_READ, 0, 0, size);
    CloseHandle(m);
    if (!p) { CloseHandle(h); return false; }
    out = {p, size, reinterpret_cast<intptr_t>(h), true};
    return true;
}

void unmap(Mapping& m) {
    if (m.base) UnmapViewOfFile(m.base);
    if (m.handle) CloseHandle(reinterpret_cast<HANDLE>(m.handle));
    m = {};
}

} // namespace lg::plat
#endif
