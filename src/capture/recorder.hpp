#pragma once
// Recorder: one independently-configured flight-recorder instance bound to one
// ring file. This is the real implementation of the hot path, lifecycle,
// background export worker, hang watchdog and rule evaluation. The historical
// ring_* C-style functions and the C ABI are thin facades over a process-wide
// default instance; embedders that need several independent black boxes (e.g.
// one per sub-system or plugin) open explicit Recorder handles instead.
//
// Layering: capture owns orchestration and may use core/platform/export/diagnosis;
// those lower layers never call back into capture.
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "capture/burst_guard.hpp"
#include "capture/recorder_config.hpp"
#include "core/ring.h"
#include "diagnosis/rule_engine.hpp"
#include "diagnosis/sequence_rule.hpp"
#include "diagnosis/symbolize.hpp"
#include "export/exporter.hpp"
#include "export/redaction.hpp"
#include "export/retention.hpp"
#include "platform/mapping.h"

namespace lg::cap {

class Recorder {
public:
    Recorder();
    ~Recorder();

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // Map/create the ring, recover any unclean previous session, install
    // process-global crash/signal hooks when this is the primary instance and
    // start the background worker/syncer/watchdog. On recovery, *recovered_path
    // (if non-null) receives the exported package path. Returns 0 on success.
    int open(const RecorderConfig& cfg, const lg::InitOpts& opts,
             std::string* recovered_path);
    void close();

    bool is_open() const { return base_ != nullptr; }
    bool is_primary() const { return primary_; }
    const std::string& dir() const { return dir_; }

    // Re-bind the process-global crash header, cooperative sampler and watchdog
    // to this instance after it is promoted to primary (the previous primary
    // closed). Signal dispositions are per-process, so exactly one live
    // recorder may own them at a time.
    void on_promoted();

    // ---- hot path ----
    void logv(uint16_t level, const char* category, const char* fmt, va_list ap);
    void log_lazy_v(uint16_t level, const char* category, const char* fmt,
                    va_list ap);
    void event(uint16_t level, const char* category, const char* msg,
               const char* fields_json);
    void set_request(const char* id); // ambient per-thread correlation id
    void clear_request();

    // ---- controls ----
    void set_level(int level);
    int  get_level() const;
    void heartbeat();
    void trigger(const char* reason);

    int  add_rule(uint16_t min_level, const char* category, const char* substr,
                  int threshold, int window_ms, int cooldown_ms);
    void clear_rules();

    // Add an ordered cross-event chain rule. The matcher is fed every event;
    // observing all steps in order within window_ms fires once (debounced by
    // cooldown_ms). Returns the sequence id, or -1 when the table is full.
    int add_sequence_rule(std::vector<diag::SeqStep> steps, int window_ms,
                          int cooldown_ms);
    void clear_sequence_rules();

    lg::Health health() const;

    // Offline recovery of a ring not owned by a live instance.
    static int export_readonly(const char* ring_path, bool redact,
                               std::string* out_path, int* out_count);

private:
    struct ExportTask {
        std::string prefix;
    };

    void worker_loop();
    void syncer_loop();
    void watchdog_loop();
    void enqueue_export(const char* prefix, int64_t cooldown_ns);
    void evaluate_rules(uint16_t level, const char* cat, const char* msg);
    void commit_slot(uint16_t level, const char* category, const char* msg,
                     const char* fields, bool apply_rules);
    // Unconditional writer used by the public path after burst gating and by
    // internally generated lines (crash markers, stacks, burst summaries).
    void write_slot_core(uint16_t level, const char* category, const char* msg,
                         const char* fields, bool apply_rules);
    void persist_stack(uint64_t tid, const uint64_t* frames, std::size_t n);
    void on_usr1();
    lg::export_::ExportOptions make_options(const char* prefix,
                                            bool live) const;

    // Process-global cooperative hooks are owned by exactly one primary.
    void install_primary_hooks(const lg::InitOpts& opts);

    // ---- mapped ring ----
    lg::plat::Mapping mapping_;
    void* base_ = nullptr;
    size_t size_ = 0;
    uint64_t slot_count_ = 0;
    uint32_t epoch_ = 0;
    std::string dir_;
    std::string ring_path_;

    // ---- config ----
    bool zstd_ = false;
    bool do_redact_ = false;
    bool symbolize_ = false;
    int stall_ms_ = 0;
    bool primary_ = false;
    lg::export_::Redactor redactor_;
    lg::diag::Symbolizer symbolizer_;

    // ---- background ----
    std::atomic<bool> run_{false};
    std::thread syncer_;
    std::thread worker_;
    std::thread watchdog_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<ExportTask> tasks_;
    std::atomic<int64_t> last_export_{0};
    std::atomic<int64_t> last_stall_{0};
    std::atomic<uint64_t> triggers_dropped_{0};

    // ---- runtime ----
    std::atomic<int> level_{2};
    std::atomic<int64_t> heartbeat_{0};
    lg::diag::RuleEngine rules_;
    std::mutex seq_mtx_;
    std::vector<lg::diag::SequenceRule> sequences_;

    // ---- log-storm suppression (opt-in) ----
    bool burst_enabled_ = false;
    std::mutex burst_mtx_;
    std::unique_ptr<lg::cap::BurstGuard> burst_;
    std::atomic<uint64_t> burst_suppressed_{0};

    // ---- disk retention for exported packages ----
    lg::export_::RetentionPolicy retention_;
    void enforce_retention();

    friend void stack_persist_sink(uint64_t, const uint64_t*, std::size_t, void*);
};

} // namespace lg::cap
