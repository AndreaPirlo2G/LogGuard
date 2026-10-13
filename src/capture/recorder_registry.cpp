#include "capture/recorder_registry.hpp"

#include <array>
#include <cstdio>
#include <mutex>

namespace lg::cap {

namespace {

struct Table {
    std::mutex mtx;
    std::array<Recorder*, kMaxRecorders> slot{};
    int primary = kInvalidHandle;
};

Table& table() {
    static Table t;
    return t;
}

thread_local int t_current = 0; // default slot; falls back to primary if empty
thread_local char t_request[kRequestCap] = {0};

} // namespace

int registry_add(Recorder* r) {
    Table& t = table();
    std::lock_guard<std::mutex> lk(t.mtx);
    int handle = kInvalidHandle;
    for (int i = 0; i < kMaxRecorders; ++i) {
        if (!t.slot[static_cast<size_t>(i)]) {
            handle = i;
            t.slot[static_cast<size_t>(i)] = r;
            break;
        }
    }
    if (handle == kInvalidHandle) return kInvalidHandle;
    if (t.primary == kInvalidHandle) t.primary = handle;
    return handle;
}

void registry_remove(int handle) {
    if (handle < 0 || handle >= kMaxRecorders) return;
    Table& t = table();
    std::lock_guard<std::mutex> lk(t.mtx);
    t.slot[static_cast<size_t>(handle)] = nullptr;
    if (t.primary == handle) {
        // Promote another live instance, if any, so crash hooks keep a target.
        t.primary = kInvalidHandle;
        for (int i = 0; i < kMaxRecorders; ++i) {
            if (t.slot[static_cast<size_t>(i)]) {
                t.primary = i;
                break;
            }
        }
    }
}

Recorder* registry_get(int handle) {
    if (handle < 0 || handle >= kMaxRecorders) return nullptr;
    Table& t = table();
    std::lock_guard<std::mutex> lk(t.mtx);
    return t.slot[static_cast<size_t>(handle)];
}

int registry_primary_handle() {
    Table& t = table();
    std::lock_guard<std::mutex> lk(t.mtx);
    return t.primary;
}

Recorder* registry_primary() {
    int h = registry_primary_handle();
    return h == kInvalidHandle ? nullptr : registry_get(h);
}

void registry_set_current(int handle) {
    if (handle >= 0 && handle < kMaxRecorders) t_current = handle;
}

int registry_current_handle() {
    return t_current;
}

Recorder* registry_current() {
    Recorder* r = registry_get(t_current);
    if (r) return r;
    return registry_primary();
}

void registry_set_request(const char* id) {
    if (id) {
        std::snprintf(t_request, sizeof(t_request), "%s", id);
    } else {
        t_request[0] = '\0';
    }
}

const char* registry_request() {
    return t_request;
}

void registry_clear_request() {
    t_request[0] = '\0';
}

} // namespace lg::cap
