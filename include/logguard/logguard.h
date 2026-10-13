#pragma once
// LogGuard — public C++ API (Linux, v0.2)
#include <cstdint>
#include <cstdio>
#include <string>

#define LG_LEVEL_TRACE    0
#define LG_LEVEL_DEBUG    1
#define LG_LEVEL_INFO     2
#define LG_LEVEL_WARN     3
#define LG_LEVEL_ERROR    4
#define LG_LEVEL_CRITICAL 5

// The stable C ABI (functions, LgOpts, LgHealth) is the single source of truth.
#include "c_api.h"
#include "config.h"

#ifdef __cplusplus
namespace logguard {
// RAII request context:
//   { logguard::ScopedRequest _r(req_id); LG_INFO(...); }
struct ScopedRequest {
    explicit ScopedRequest(const char* id) { lg_set_request(id); }
    ~ScopedRequest() { lg_clear_request(); }
    ScopedRequest(const ScopedRequest&) = delete;
    ScopedRequest& operator=(const ScopedRequest&) = delete;
};

// Builds the raw JSON fields fragment for lg_event:
//   logguard::Fields f; f.num("user_id", 42).str("route", "/api/x").flag("retry");
//   lg_event(LG_LEVEL_WARN, "auth", "login failed", f.json().c_str());
class Fields {
public:
    Fields& str(const char* k, const char* v) {
        add_key(k); buf_ += '"'; esc(v ? v : ""); buf_ += '"'; return *this;
    }
    Fields& num(const char* k, long long v) { add_key(k); add_num(v); return *this; }
    Fields& u64(const char* k, unsigned long long v) { add_key(k); add_unum(v); return *this; }
    Fields& flag(const char* k, bool on = true) { add_key(k); buf_ += on ? "true" : "false"; return *this; }
    const std::string& json() const { return buf_; }
private:
    std::string buf_;
    void add_key(const char* k) { if (!buf_.empty()) buf_ += ','; buf_ += '"'; esc(k ? k : ""); buf_ += "\":"; }
    void add_num(long long v) {
        char b[32]; std::snprintf(b, sizeof(b), "%lld", v); buf_ += b;
    }
    void add_unum(unsigned long long v) {
        char b[32]; std::snprintf(b, sizeof(b), "%llu", v); buf_ += b;
    }
    void esc(const char* s) {
        for (; *s; ++s) {
            if (*s == '"' || *s == '\\') buf_ += '\\';
            buf_ += *s;
        }
    }
};
// RAII multi-instance recorder. Each handle owns an independent ring file; the
// first constructed handle in a process is the primary (owns crash/signals).
// Call use() to route the handle-less LG_INFO(...) macros on this thread to it.
//   {
//     logguard::RecorderHandle r2("/var/log/plugin.lgp", nullptr);
//     r2.use();
//     LG_INFO("plugin", "hello from the second black box");
//   }
struct RecorderHandle {
    int handle;
    explicit RecorderHandle(const char* ring_path, const lg_opts2_t* opts = nullptr)
        : handle(lg_open(ring_path, opts)) {}
    ~RecorderHandle() { if (handle >= 0) lg_close(handle); }
    RecorderHandle(const RecorderHandle&) = delete;
    RecorderHandle& operator=(const RecorderHandle&) = delete;

    bool ok() const { return handle >= 0; }
    void use() const { if (handle >= 0) lg_use(handle); }
    void set_level(int lvl) const { lg_set_level_to(handle, lvl); }
    int  level() const { return lg_get_level_to(handle); }
    void trigger(const char* reason) const { lg_trigger_to(handle, reason); }
    void heartbeat() const { lg_heartbeat_to(handle); }
};
} // namespace logguard

//   LG_EVENT(LG_LEVEL_WARN, "auth", "login failed",
//            logguard::Fields().num("user", 7).str("ip", "10.0.0.1"));
#define LG_EVENT(lvl, cat, msg, fields_expr)                                      \
    do { if ((lvl) >= lg_get_level()) {                                           \
        ::logguard::Fields _lgf_ = (fields_expr);                                 \
        lg_event((lvl), (cat), (msg), _lgf_.json().c_str());                      \
    } } while (0)
#endif

#define LG_LOG_AT(lvl, cat, ...)                                                \
    LG_IF_ENABLED(lvl, if ((lvl) >= lg_get_level())                            \
                          lg_log((lvl), (cat), __VA_ARGS__);)
// Deferred-formatting variant: formatting happens on the export worker.
// Numeric/pointer args only (%s must reference a string valid until export).
#define LG_LOG_LAZY(lvl, cat, ...)                                             \
    LG_IF_ENABLED(lvl, if ((lvl) >= lg_get_level())                            \
                          lg_log_lazy((lvl), (cat), __VA_ARGS__);)
#define LG_INFO_LAZY(cat, ...) LG_LOG_LAZY(LG_LEVEL_INFO, (cat), __VA_ARGS__)
#define LG_TRACE(cat, ...)    LG_LOG_AT(LG_LEVEL_TRACE,    (cat), __VA_ARGS__)
#define LG_DEBUG(cat, ...)    LG_LOG_AT(LG_LEVEL_DEBUG,    (cat), __VA_ARGS__)
#define LG_INFO(cat, ...)     LG_LOG_AT(LG_LEVEL_INFO,     (cat), __VA_ARGS__)
#define LG_WARN(cat, ...)     LG_LOG_AT(LG_LEVEL_WARN,     (cat), __VA_ARGS__)
#define LG_ERROR(cat, ...)    LG_LOG_AT(LG_LEVEL_ERROR,    (cat), __VA_ARGS__)
#define LG_CRITICAL(cat, ...) LG_LOG_AT(LG_LEVEL_CRITICAL, (cat), __VA_ARGS__)
