#include "file_util.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>

namespace lg::plat {

std::string parent_dir(const std::string& path) {
    size_t pos = path.find_last_of("\\/");
    return pos == std::string::npos ? std::string(".") : path.substr(0, pos);
}

std::string join_path(const std::string& dir, const std::string& name) {
    if (dir.empty() || dir == ".") return name;
    char last = dir.back();
    if (last == '\\' || last == '/') return dir + name;
    return dir + "\\" + name;
}

bool ensure_dir(const std::string& path) {
    if (path.empty() || path == ".") return true;
    DWORD attr = GetFileAttributesA(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES)
        return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        char c = path[i];
        cur.push_back(c);
        if (c == '\\' || c == '/' || i + 1 == path.size()) {
            if (cur.size() <= 3) continue; // drive prefix like C:\
            CreateDirectoryA(cur.c_str(), nullptr);
        }
    }
    attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::string package_path(const std::string& dir, const char* prefix,
                         int64_t wall_ns) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s-%lld.lgp",
                  prefix ? prefix : "incident",
                  static_cast<long long>(wall_ns));
    return join_path(dir, buf);
}

} // namespace lg::plat
