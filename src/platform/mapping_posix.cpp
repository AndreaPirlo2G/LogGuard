#include "mapping.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lg::plat {

bool map_create(const std::string& path, size_t size, Mapping& out) {
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0) return false;
    struct stat st{};
    if (::fstat(fd, &st) != 0) { ::close(fd); return false; }
    if (static_cast<size_t>(st.st_size) < size) {
        if (::ftruncate(fd, static_cast<off_t>(size)) != 0) { ::close(fd); return false; }
    }
    void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { ::close(fd); return false; }
    out = {p, size, fd, false};
    return true;
}

bool map_open_rw(const std::string& path, Mapping& out) {
    int fd = ::open(path.c_str(), O_RDWR);
    if (fd < 0) return false;
    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size <= 0) { ::close(fd); return false; }
    size_t size = static_cast<size_t>(st.st_size);
    void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { ::close(fd); return false; }
    out = {p, size, fd, false};
    return true;
}

bool map_open_ro(const std::string& path, Mapping& out) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size <= 0) { ::close(fd); return false; }
    size_t size = static_cast<size_t>(st.st_size);
    void* p = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { ::close(fd); return false; }
    out = {p, size, fd, true};
    return true;
}

void unmap(Mapping& m) {
    if (m.base && m.base != MAP_FAILED) ::munmap(m.base, m.size);
    if (m.handle >= 0) ::close(static_cast<int>(m.handle));
    m = {};
}

} // namespace lg::plat
