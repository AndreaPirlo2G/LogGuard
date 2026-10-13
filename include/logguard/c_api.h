#ifndef LOGGUARD_C_API_H
#define LOGGUARD_C_API_H

/*
 * LogGuard stable C ABI.
 *
 * Integration entry point for C, C# (P/Invoke), Python (cffi/ctypes), Rust
 * (extern "C") and vendor SDKs. ABI stability is guaranteed within a major
 * version: existing signatures never change; new functions are added with
 * versioned suffixes. Strings are NUL-terminated UTF-8 and stay owned by the
 * caller unless documented otherwise.
 *
 * Levels: 0 TRACE, 1 DEBUG, 2 INFO, 3 WARN, 4 ERROR, 5 CRITICAL.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOGGUARD_VERSION_MAJOR 0
#define LOGGUARD_VERSION_MINOR 4
#define LOGGUARD_VERSION_PATCH 0

typedef struct {
    int stall_watchdog_ms; /* 0 = disabled */
    int zstd_compression;  /* 1 = compress if libzstd loads at runtime, else fall back */
} lg_opts_t;

typedef struct {
    uint64_t truncated_msgs;
    uint64_t snapshots;
    uint64_t stalls;
    uint64_t triggers_dropped;
    uint64_t rule_fires;
} lg_health_t;

/* Lifecycle. Returns 0 on success, non-zero on failure. */
int  lg_init(const char* ring_path);
int  lg_init_opts(const char* ring_path, const lg_opts_t* opts);
void lg_shutdown(void);

/* Logging: printf-style text log and structured event. */
void lg_log(uint16_t level, const char* category, const char* fmt, ...);
/* Deferred-formatting variant: the format string and primitive arguments are
 * captured on the hot path (no vsnprintf there); the message is rendered later
 * by the export worker. %s arguments must point at strings that stay alive
 * until export (use it for constant/format-driven logs). */
void lg_log_lazy(uint16_t level, const char* category, const char* fmt, ...);
void lg_event(uint16_t level, const char* category,
              const char* msg, const char* fields_json);

/* Rules: threshold hits of matching records within window_ms trigger a
 * snapshot; cooldown_ms debounces. Returns rule id, -1 on error. */
int  lg_add_rule(int min_level, const char* category, const char* substr,
                 int threshold, int window_ms, int cooldown_ms);
void lg_clear_rules(void);

/* Context / level / watchdog. */
void lg_set_request(const char* id);
void lg_clear_request(void);
void lg_set_level(int level);
int  lg_get_level(void);
void lg_heartbeat(void);

/* Manual snapshot. */
void lg_trigger_snapshot(const char* reason);

void lg_health(lg_health_t* out);

/* Offline recovery from an arbitrary process. */
int lg_recover_export(const char* ring_path);

const char* lg_version(void);

/* ---- Multi-instance API (added in 0.4) ----
 * Open an independent recorder on its own ring file and return a small
 * non-negative handle, or -1 when the instance table is full (max 8). The
 * first opened handle is the "primary": it owns the process-global crash
 * handler and runtime signals; later handles are ordinary ring writers with
 * their own level/rules/export worker. lg_init*() simply opens the default
 * instance (handle 0 slot when free). */
typedef struct {
    int stall_watchdog_ms;
    int zstd_compression;
    int redact_on_export; /* mask PII/secrets in exported packages */
    uint32_t ring_slots;  /* rounded up to a power of two; 0 -> default 262144 */
    const char* output_dir; /* NULL -> alongside the ring file */
    /* Disk retention of exported .lgp packages; 0 means "use the default",
       retain_max_age_hours 0 disables the age bound. */
    uint32_t retain_max_packages;
    uint64_t retain_max_mb;
    int      retain_max_age_hours;
} lg_opts2_t;

int  lg_open(const char* ring_path, const lg_opts2_t* opts);
void lg_close(int handle);

/* Route the handle-less lg_log/... macros on the calling thread to `handle`. */
void lg_use(int handle);

void lg_log_to(int handle, uint16_t level, const char* category,
               const char* fmt, ...);
void lg_event_to(int handle, uint16_t level, const char* category,
                 const char* msg, const char* fields_json);
void lg_set_level_to(int handle, int level);
int  lg_get_level_to(int handle);
int  lg_add_rule_to(int handle, int min_level, const char* category,
                    const char* substr, int threshold, int window_ms,
                    int cooldown_ms);
void lg_health_to(int handle, lg_health_t* out);
void lg_trigger_to(int handle, const char* reason);
void lg_heartbeat_to(int handle);

/* Cross-event sequence rules on a handle. A chain is an array of steps (level,
 * category, message-substring); observing all steps in order within window_ms
 * fires a snapshot once per cooldown_ms. Returns the sequence id, or -1. Pass
 * NULL/"" for category or substr to wildcard that field on a step. */
typedef struct {
    int min_level;
    const char* category;
    const char* substr;
} lg_seq_step_t;
int lg_add_sequence_to(int handle, const lg_seq_step_t* steps, int nsteps,
                       int window_ms, int cooldown_ms);
void lg_clear_sequences_to(int handle);

#ifdef __cplusplus
}

/* Short aliases for the C++ facade. The C ABI header is the single source of
 * truth for ABI-facing structs/functions. */
using LgOpts = lg_opts_t;
using LgHealth = lg_health_t;
#endif

#endif /* LOGGUARD_C_API_H */
