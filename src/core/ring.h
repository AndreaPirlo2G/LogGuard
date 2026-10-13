#pragma once
#include <cstdarg>
#include <cstdint>
#include <string>

namespace lg {

struct InitOpts {
    int  stall_watchdog_ms = 0;
    bool crash_chain_existing = true; // coexist with Crashpad/Breakpad
    bool crash_reraise_default = true;
    bool zstd_compression = false;    // requires dlopen("libzstd.so.1")
};

int ring_init(const char* ring_path, const InitOpts& opts, std::string* recovered_path);
void ring_shutdown();
void ring_logv(uint16_t level, const char* category, const char* fmt, va_list ap);
// Deferred-format path: stores fmt + captured values, renders at export time.
void ring_log_lazy_v(uint16_t level, const char* category, const char* fmt, va_list ap);

// Structured event: human message plus a raw JSON fields fragment
// (no enclosing braces, e.g. R"("user_id":42,"retry":3)").
void ring_event(uint16_t level, const char* category, const char* msg,
                const char* fields_json);

void ring_set_request(const char* id);
void ring_clear_request();
void ring_set_level(int level);
int  ring_get_level();
void ring_heartbeat();
void ring_trigger(const char* reason);

// Rule engine: a matching event within its time window fires a snapshot.
// category may be "" (any), needlestr may be "" (any); threshold <= 0 means
// "fire on the first match". Returns the rule id, or -1 if no space.
int  ring_add_rule(uint16_t min_level, const char* category, const char* substr,
                   int threshold, int window_ms, int cooldown_ms);
void ring_clear_rules();

struct Health {
    uint64_t truncated = 0, snapshots = 0, stalls = 0, triggers_dropped = 0;
    uint64_t rule_fires = 0;
};
Health ring_health();

int ring_export_readonly(const char* ring_path, std::string* out_path, int* out_count);

} // namespace lg
