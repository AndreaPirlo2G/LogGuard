#include <logguard/logguard.h>
#include "ring.h"

#include "capture/recorder.hpp"
#include "capture/recorder_config.hpp"
#include "capture/recorder_registry.hpp"
#include "diagnosis/sequence_rule.hpp"

#include <vector>

#include <array>
#include <cstdarg>
#include <cstdio>

extern "C" {

using lg::cap::Recorder;
using lg::cap::RecorderConfig;
using lg::cap::kMaxRecorders;

int lg_init(const char* ring_path) {
    lg::InitOpts opts;
    std::string recovered;
    int rc = lg::ring_init(ring_path, opts, &recovered);
    if (rc == 0 && !recovered.empty())
        fprintf(stderr, "[logguard] crash report available: %s\n", recovered.c_str());
    return rc;
}

int lg_init_opts(const char* ring_path, const LgOpts* opts) {
    lg::InitOpts o;
    if (opts) {
        o.stall_watchdog_ms = opts->stall_watchdog_ms;
        o.zstd_compression = opts->zstd_compression != 0;
    }
    std::string recovered;
    int rc = lg::ring_init(ring_path, o, &recovered);
    if (rc == 0 && !recovered.empty())
        fprintf(stderr, "[logguard] crash report available: %s\n", recovered.c_str());
    return rc;
}

void lg_shutdown() { lg::ring_shutdown(); }

void lg_log(uint16_t level, const char* category, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    lg::ring_logv(level, category, fmt, ap);
    va_end(ap);
}

void lg_log_lazy(uint16_t level, const char* category, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    lg::ring_log_lazy_v(level, category, fmt, ap);
    va_end(ap);
}

void lg_event(uint16_t level, const char* category, const char* msg,
              const char* fields_json) {
    lg::ring_event(level, category, msg, fields_json);
}

int lg_add_rule(int min_level, const char* category, const char* substr,
                int threshold, int window_ms, int cooldown_ms) {
    return lg::ring_add_rule(static_cast<uint16_t>(min_level), category, substr,
                             threshold, window_ms, cooldown_ms);
}
void lg_clear_rules() { lg::ring_clear_rules(); }

void lg_set_request(const char* id) { lg::ring_set_request(id); }
void lg_clear_request() { lg::ring_clear_request(); }
void lg_set_level(int level) { lg::ring_set_level(level); }
int  lg_get_level() { return lg::ring_get_level(); }
void lg_heartbeat() { lg::ring_heartbeat(); }

void lg_trigger_snapshot(const char* reason) { lg::ring_trigger(reason); }

void lg_health(LgHealth* out) {
    if (!out) return;
    auto h = lg::ring_health();
    out->truncated_msgs = h.truncated;
    out->snapshots = h.snapshots;
    out->stalls = h.stalls;
    out->triggers_dropped = h.triggers_dropped;
    out->rule_fires = h.rule_fires;
}

int lg_recover_export(const char* ring_path) {
    std::string path;
    int count = 0;
    return lg::ring_export_readonly(ring_path, &path, &count);
}

int lg_recover_export_redacted(const char* ring_path) {
    std::string path;
    int count = 0;
    return Recorder::export_readonly(ring_path, /*redact=*/true,
                                     &path, &count);
}

const char* lg_version() { return "0.4.0"; }

// ---------------- multi-instance (0.4) ----------------

namespace {
// External handle -> owned Recorder. Handles are the registry slot ids.
std::array<Recorder*, kMaxRecorders>& owned() {
    static std::array<Recorder*, kMaxRecorders> a{};
    return a;
}
} // namespace

int lg_open(const char* ring_path, const lg_opts2_t* opts) {
    if (!ring_path) return -1;
    auto* r = new Recorder();
    RecorderConfig cfg = RecorderConfig::for_path(ring_path);
    lg::InitOpts io;
    if (opts) {
        io.stall_watchdog_ms = opts->stall_watchdog_ms;
        io.zstd_compression = opts->zstd_compression != 0;
        cfg.stall_threshold_ms = opts->stall_watchdog_ms;
        cfg.zstd_compression = opts->zstd_compression != 0;
        cfg.redact_on_export = opts->redact_on_export != 0;
        if (opts->ring_slots) cfg.ring_slots = opts->ring_slots;
        if (opts->output_dir) cfg.output_dir = opts->output_dir;
        if (opts->retain_max_packages)
            cfg.retain_max_packages = opts->retain_max_packages;
        if (opts->retain_max_mb) cfg.retain_max_mb = opts->retain_max_mb;
        cfg.retain_max_age_hours = opts->retain_max_age_hours;
        cfg.symbolize_stack = opts->symbolize_stack != 0;
    }
    std::string recovered;
    if (r->open(cfg, io, &recovered) != 0) {
        delete r;
        return -1;
    }
    // Recover the registry slot assigned to this instance so callers get a
    // stable handle that matches the internal table.
    for (int i = 0; i < kMaxRecorders; ++i) {
        if (lg::cap::registry_get(i) == r) {
            owned()[static_cast<size_t>(i)] = r;
            if (!recovered.empty())
                fprintf(stderr, "[logguard] handle %d recovered: %s\n", i,
                        recovered.c_str());
            return i;
        }
    }
    r->close();
    delete r;
    return -1;
}

void lg_close(int handle) {
    if (handle < 0 || handle >= kMaxRecorders) return;
    auto* r = owned()[static_cast<size_t>(handle)];
    if (!r) return;
    r->close();
    delete r;
    owned()[static_cast<size_t>(handle)] = nullptr;
}

void lg_use(int handle) { lg::cap::registry_set_current(handle); }

void lg_log_to(int handle, uint16_t level, const char* category,
               const char* fmt, ...) {
    if (handle < 0 || handle >= kMaxRecorders) return;
    Recorder* r = owned()[static_cast<size_t>(handle)];
    if (!r) return;
    va_list ap;
    va_start(ap, fmt);
    r->logv(level, category, fmt, ap);
    va_end(ap);
}

void lg_event_to(int handle, uint16_t level, const char* category,
                 const char* msg, const char* fields_json) {
    if (handle < 0 || handle >= kMaxRecorders) return;
    if (Recorder* r = owned()[static_cast<size_t>(handle)])
        r->event(level, category, msg, fields_json);
}

void lg_set_level_to(int handle, int level) {
    if (handle >= 0 && handle < kMaxRecorders)
        if (Recorder* r = owned()[static_cast<size_t>(handle)])
            r->set_level(level);
}

int lg_get_level_to(int handle) {
    if (handle >= 0 && handle < kMaxRecorders)
        if (Recorder* r = owned()[static_cast<size_t>(handle)])
            return r->get_level();
    return 2;
}

int lg_add_rule_to(int handle, int min_level, const char* category,
                   const char* substr, int threshold, int window_ms,
                   int cooldown_ms) {
    if (handle < 0 || handle >= kMaxRecorders) return -1;
    Recorder* r = owned()[static_cast<size_t>(handle)];
    return r ? r->add_rule(static_cast<uint16_t>(min_level), category, substr,
                           threshold, window_ms, cooldown_ms)
             : -1;
}

void lg_health_to(int handle, lg_health_t* out) {
    if (!out) return;
    lg::Health z;
    if (handle >= 0 && handle < kMaxRecorders)
        if (Recorder* r = owned()[static_cast<size_t>(handle)]) z = r->health();
    out->truncated_msgs = z.truncated;
    out->snapshots = z.snapshots;
    out->stalls = z.stalls;
    out->triggers_dropped = z.triggers_dropped;
    out->rule_fires = z.rule_fires;
}

void lg_trigger_to(int handle, const char* reason) {
    if (handle >= 0 && handle < kMaxRecorders)
        if (Recorder* r = owned()[static_cast<size_t>(handle)])
            r->trigger(reason);
}

void lg_heartbeat_to(int handle) {
    if (handle >= 0 && handle < kMaxRecorders)
        if (Recorder* r = owned()[static_cast<size_t>(handle)]) r->heartbeat();
}

int lg_add_sequence_to(int handle, const lg_seq_step_t* steps, int nsteps,
                       int window_ms, int cooldown_ms) {
    if (handle < 0 || handle >= kMaxRecorders || !steps || nsteps < 2)
        return -1;
    Recorder* r = owned()[static_cast<size_t>(handle)];
    if (!r) return -1;
    std::vector<lg::diag::SeqStep> v;
    v.reserve(static_cast<size_t>(nsteps));
    for (int i = 0; i < nsteps; ++i) {
        v.emplace_back(static_cast<uint16_t>(steps[i].min_level),
                       steps[i].category ? steps[i].category : "",
                       steps[i].substr ? steps[i].substr : "");
    }
    return r->add_sequence_rule(std::move(v), window_ms, cooldown_ms);
}

void lg_clear_sequences_to(int handle) {
    if (handle >= 0 && handle < kMaxRecorders)
        if (Recorder* r = owned()[static_cast<size_t>(handle)])
            r->clear_sequence_rules();
}

} // extern "C"
