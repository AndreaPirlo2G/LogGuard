// Backward-compatible recorder facade.
//
// The historical ring_* entry points used to be a 500+ line god unit holding
// the single global recorder. The real implementation now lives in the capture
// layer as lg::cap::Recorder (one instance per ring file) with a small handle
// registry; this file is intentionally a thin shim that owns the process-wide
// *default* instance and forwards the legacy zero-handle API to it. New code
// that needs multiple black boxes should open Recorder handles directly (see
// capture/recorder.hpp) or use the handle-based C ABI.
#include "ring.h"

#include "capture/recorder.hpp"
#include "capture/recorder_config.hpp"
#include "capture/recorder_registry.hpp"

#include <cstdio>

namespace lg {

using lg::cap::Recorder;
using lg::cap::RecorderConfig;

namespace {
// The default instance addressed by the legacy LG_INFO macros and the simple
// C ABI. Owned here for the process lifetime.
Recorder* g_default = nullptr;
} // namespace

int ring_init(const char* ring_path, const InitOpts& opts,
              std::string* recovered_path) {
    if (g_default) g_default->close();
    if (!g_default) g_default = new Recorder();

    RecorderConfig cfg = RecorderConfig::for_path(ring_path ? ring_path : "");
    cfg.stall_threshold_ms = opts.stall_watchdog_ms;
    cfg.zstd_compression = opts.zstd_compression;
    int rc = g_default->open(cfg, opts, recovered_path);
    if (rc != 0) {
        delete g_default;
        g_default = nullptr;
    }
    return rc;
}

void ring_shutdown() {
    if (!g_default) return;
    g_default->close();
    delete g_default;
    g_default = nullptr;
}

void ring_logv(uint16_t level, const char* category, const char* fmt,
               va_list ap) {
    if (Recorder* r = lg::cap::registry_current()) r->logv(level, category, fmt, ap);
}

void ring_log_lazy_v(uint16_t level, const char* category, const char* fmt,
                     va_list ap) {
    if (Recorder* r = lg::cap::registry_current())
        r->log_lazy_v(level, category, fmt, ap);
}

void ring_event(uint16_t level, const char* category, const char* msg,
                const char* fields_json) {
    if (Recorder* r = lg::cap::registry_current())
        r->event(level, category, msg, fields_json);
}

void ring_set_request(const char* id) { lg::cap::registry_set_request(id); }
void ring_clear_request() { lg::cap::registry_clear_request(); }

void ring_set_level(int level) {
    if (Recorder* r = lg::cap::registry_current()) r->set_level(level);
}

int ring_get_level() {
    Recorder* r = lg::cap::registry_current();
    return r ? r->get_level() : 2;
}

void ring_heartbeat() {
    if (Recorder* r = lg::cap::registry_current()) r->heartbeat();
}

void ring_trigger(const char* reason) {
    if (Recorder* r = lg::cap::registry_current()) r->trigger(reason);
}

int ring_add_rule(uint16_t min_level, const char* category, const char* substr,
                  int threshold, int window_ms, int cooldown_ms) {
    Recorder* r = lg::cap::registry_current();
    return r ? r->add_rule(min_level, category, substr, threshold, window_ms,
                           cooldown_ms)
             : -1;
}

void ring_clear_rules() {
    if (Recorder* r = lg::cap::registry_current()) r->clear_rules();
}

Health ring_health() {
    Health h;
    if (Recorder* r = lg::cap::registry_current()) h = r->health();
    return h;
}

int ring_export_readonly(const char* ring_path, std::string* out_path,
                         int* out_count) {
    return Recorder::export_readonly(ring_path, false, out_path, out_count);
}

} // namespace lg
