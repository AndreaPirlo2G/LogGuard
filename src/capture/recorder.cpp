#include "capture/recorder.hpp"

#include "capture/recorder_registry.hpp"
#include "core/constants.hpp"
#include "core/ring_flags.hpp"
#include "core/slot_format.hpp"
#include "core/lazy_format.h"
#include "core/ring_repair.hpp"
#include "core/recovery.hpp"
#include "export/exporter.hpp"
#include "export/zstd_codec.h"
#include "platform/clock.hpp"
#include "platform/crash.h"
#include "platform/file_util.hpp"
#include "platform/mapping.h"
#include "platform/stack_walk.hpp"
#include "platform/thread_id.hpp"

#ifndef _WIN32
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

namespace lg::cap {

namespace {

using lg::core::Slot;
using lg::core::FileHeader;
namespace plat = lg::plat;
namespace export_ = lg::export_;
namespace diag = lg::diag;

constexpr uint32_t kSlotMagic = lg::core::RING_MAGIC_NUMBER;
constexpr size_t kHeaderSize = lg::core::HEADER_SIZE;
constexpr size_t kSlotSize = lg::core::SLOT_SIZE;
constexpr size_t kCategoryCap = lg::core::CATEGORY_CAP;
constexpr size_t kPayloadCap = lg::core::PAYLOAD_CAP;
constexpr uint16_t kFlagStack = lg::core::FLAG_STACK;
constexpr uint16_t kFlagKv = lg::core::FLAG_KV;
constexpr uint16_t kFlagLazy = lg::core::FLAG_LAZY;

// The single cooperative sampler used by the primary instance's watchdog.
plat::StackSampler& sampler() {
    static plat::StackSampler s;
    return s;
}

void retire_crashed_epoch(FileHeader* h) {
    __atomic_add_fetch(&h->epoch, 1u, __ATOMIC_RELEASE);
    h->crashed = 0;
    h->crash_nframes = 0;
    h->clean = 1;
}

} // namespace

// Persistence sink invoked inside each thread's sampling-signal context. The
// target Recorder is passed as `user` by the sampler. Runs on the sampled
// thread itself, so its stack pointer is valid; the commit is the same
// allocation-free, lock-free fixed-slot publish as the normal hot path.
void stack_persist_sink(uint64_t tid, const uint64_t* frames, std::size_t n,
                        void* user) {
    auto* self = static_cast<Recorder*>(user);
    if (!self || !self->base_ || n == 0) return;
    if (n > kPayloadCap / sizeof(uint64_t)) n = kPayloadCap / sizeof(uint64_t);
    auto* h = static_cast<FileHeader*>(self->base_);
    uint64_t ticket = __atomic_fetch_add(&h->next_ticket, 1ull, __ATOMIC_RELAXED);
    uint64_t idx = ticket & (self->slot_count_ - 1);
    auto* dst = reinterpret_cast<Slot*>(static_cast<char*>(self->base_) +
                                        kHeaderSize) + idx;
    Slot s;
    std::memset(&s, 0, sizeof(s));
    s.epoch = self->epoch_;
    s.ticket = ticket;
    s.mono_ns = plat::mono_now_ns();
    s.wall_ns = plat::wall_now_ns();
    s.tid = tid;
    s.level = 4;
    s.flags = kFlagStack;
    s.msg_len = static_cast<uint16_t>(n * sizeof(uint64_t));
    std::memcpy(s.category, "_stack", 6);
    s.cat_len = 6;
    std::memcpy(s.payload, frames, n * sizeof(uint64_t));
    __atomic_store_n(&dst->magic, 0u, __ATOMIC_RELEASE);
    std::memcpy(dst, &s, sizeof(s));
    __atomic_store_n(&dst->magic, kSlotMagic, __ATOMIC_RELEASE);
}

Recorder::Recorder() = default;

Recorder::~Recorder() {
    close();
}

// ---------------- process-global hooks (primary only) ----------------

void Recorder::install_primary_hooks(const lg::InitOpts& opts) {
    sampler().install(stack_persist_sink, this);
#ifndef _WIN32
    // SIGUSR1 cycles the primary instance's runtime level. A small trampoline
    // resolves the current primary at delivery time (it may be replaced).
    struct sigaction sc{};
    sc.sa_handler = [](int) {
        if (Recorder* p = registry_primary()) p->on_usr1();
    };
    sc.sa_flags = SA_RESTART;
    sigemptyset(&sc.sa_mask);
    sigaction(SIGUSR1, &sc, nullptr);
#endif
    auto* h = static_cast<FileHeader*>(base_);
    lg::crash::set_header(h, &lg::plat::mono_now_ns);
    lg::crash::Config ccfg;
    ccfg.chain_existing = opts.crash_chain_existing;
    ccfg.reraise_default = opts.crash_reraise_default;
    lg::crash::install(ccfg);
}

void Recorder::on_usr1() {
    int cur = level_.load(std::memory_order_relaxed);
    int next = (cur == 3) ? 2 : (cur == 2 ? 1 : 3); // WARN -> INFO -> DEBUG
    set_level(next);
}

void Recorder::on_promoted() {
    if (!base_) return;
    primary_ = true;
    // Point the process-global crash marker and the cooperative sampler at the
    // new header so a fault after the old primary closed cannot touch unmapped
    // memory. Signal dispositions themselves remain installed.
    lg::crash::set_header(base_, &lg::plat::mono_now_ns);
    sampler().install(stack_persist_sink, this);
    if (stall_ms_ > 0 && !watchdog_.joinable())
        watchdog_ = std::thread([this] { watchdog_loop(); });
}

// ---------------- background threads ----------------

export_::ExportOptions Recorder::make_options(const char* prefix,
                                              bool live) const {
    export_::ExportOptions eo;
    eo.zstd = zstd_;
    eo.prefix = prefix;
    eo.redactor = do_redact_ ? &redactor_ : nullptr;
    // Only a live process can resolve its own PCs; a recovered dead ring cannot.
    eo.symbolizer = (live && symbolize_) ? &symbolizer_ : nullptr;
    return eo;
}

void Recorder::enqueue_export(const char* prefix, int64_t cooldown_ns) {
    if (!base_) return;
    int64_t now = plat::mono_now_ns();
    int64_t last = last_export_.load(std::memory_order_relaxed);
    if (now - last < cooldown_ns) {
        triggers_dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    last_export_.store(now, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        tasks_.push_back({prefix});
    }
    cv_.notify_one();
}

void Recorder::enforce_retention() {
    if (!retention_.any_bound() || dir_.empty()) return;
    export_::RetentionResult rr =
        export_::enforce_retention(dir_, retention_, plat::wall_now_ns());
    if (!rr.deleted.empty()) {
        double mb = rr.bytes_freed / (1024.0 * 1024.0);
        std::fprintf(stderr,
                     "[logguard] retention pruned %zu package(s), freed %.2f MiB\n",
                     rr.deleted.size(), mb);
    }
}

void Recorder::worker_loop() {
    for (;;) {
        ExportTask task;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [&] { return !run_.load() || !tasks_.empty(); });
            if (!run_.load() && tasks_.empty()) return;
            task = tasks_.front();
            tasks_.pop_front();
        }
        auto* h = static_cast<FileHeader*>(base_);
        auto* slots = reinterpret_cast<Slot*>(static_cast<char*>(base_) +
                                              kHeaderSize);
        export_::ExportOptions eo = make_options(task.prefix.c_str(), true);
        export_::ExportResult r =
            export_::export_window(*h, slots, slot_count_, dir_, eo);
        if (r.count > 0) {
            __atomic_add_fetch(&h->n_snapshots, 1ull, __ATOMIC_RELEASE);
            std::fprintf(stderr, "[logguard] %s exported: %s (%d records)\n",
                         task.prefix.c_str(), r.path.c_str(), r.count);
            enforce_retention();
        }
    }
}

void Recorder::syncer_loop() {
    while (run_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
#ifndef _WIN32
        msync(base_, size_, MS_ASYNC);
#endif
    }
}

void Recorder::watchdog_loop() {
    auto grab = [&] {
        (void)sampler().sample_all_threads();
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    };
    while (run_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (stall_ms_ <= 0) continue;
        int64_t hb = heartbeat_.load(std::memory_order_relaxed);
        int64_t now = plat::mono_now_ns();
        if (hb == 0 || now - hb < stall_ms_ * 1000000LL) continue;
        int64_t last = last_stall_.exchange(now, std::memory_order_relaxed);
        if (now - last < 30LL * 1000000000LL) continue; // 30 s cooldown
        auto* h = static_cast<FileHeader*>(base_);
        __atomic_add_fetch(&h->n_stalls, 1ull, __ATOMIC_RELEASE);
        std::fprintf(stderr,
                     "[logguard] stall: no heartbeat for %d ms, dumping stacks\n",
                     stall_ms_);
        grab();
        enqueue_export("stall", 0);
    }
}

// ---------------- lifecycle ----------------

int Recorder::open(const RecorderConfig& cfg, const lg::InitOpts& opts,
                   std::string* recovered_path) {
    RecorderConfig c = cfg;
    std::string err;
    if (!c.normalize(&err)) {
        std::fprintf(stderr, "[logguard] bad config: %s\n", err.c_str());
        return -1;
    }

    ring_path_ = c.ring_path;
    dir_ = c.output_dir;
    stall_ms_ = c.stall_threshold_ms;
    do_redact_ = c.redact_on_export;
    level_.store(c.level);
    if (c.zstd_compression)
        zstd_ = export_::ZstdCodec::instance().available();
    symbolize_ = c.symbolize_stack;
    if (symbolize_) {
        symbolizer_.load_self();
        // Best-effort file:line resolution via addr2line at export time;
        // silently stays dladdr-only when binutils is unavailable.
        symbolizer_.enable_source_locations(true);
    }

    burst_enabled_ = c.burst_suppression;
    if (burst_enabled_) {
        std::lock_guard<std::mutex> lk(burst_mtx_);
        burst_ = std::unique_ptr<cap::BurstGuard>(
            new cap::BurstGuard(uint64_t(c.burst_window_ms) * 1000000ull));
    }

    retention_.max_files = c.retain_max_packages;
    retention_.max_total_bytes =
        c.retain_max_mb * 1024ull * 1024ull;
    retention_.max_age_ns =
        int64_t(c.retain_max_age_hours) * 3600ull * 1000000000ull;

    const uint64_t desired_slots = c.ring_slots;
    size_t desired = kHeaderSize + size_t(desired_slots) * kSlotSize;
    if (!plat::map_create(c.ring_path.c_str(), desired, mapping_)) {
        perror("map_create");
        return -1;
    }
    size_ = mapping_.size;
    base_ = mapping_.base;
    auto* h = static_cast<FileHeader*>(base_);
    auto* slots = reinterpret_cast<Slot*>(static_cast<char*>(base_) +
                                          kHeaderSize);

    slot_count_ = desired_slots;
    const bool magic_ok = std::memcmp(h->magic, "LGRING02", 8) == 0;
    if (!magic_ok) {
        // Bad/absent header magic. Before wiping, check whether the arena still
        // holds intact slots (power loss can tear the one-page header write):
        // salvage and export them so the last black-box window is not lost.
        core::RepairReport rep;
        std::vector<Slot> rescued =
            core::salvage_slots(base_, size_, &rep);
        if (rep.should_salvage() && !rescued.empty()) {
            export_::ExportOptions eo = make_options("repaired", false);
            export_::ExportResult r =
                export_::export_slots(std::move(rescued), dir_, eo);
            if (r.count > 0 && recovered_path) *recovered_path = r.path;
            std::fprintf(stderr,
                         "[logguard] repaired %s header; salvaged %d records "
                         "(epoch %u) -> %s\n",
                         core::to_string(rep.state), r.count,
                         rep.recovered_epoch, r.path.c_str());
            enforce_retention();
        }
        // Reinitialise a clean header sized to the current requested capacity.
        std::memset(h, 0, kHeaderSize);
        std::memcpy(h->magic, "LGRING02", 8);
        h->version = 2;
        h->slot_size = kSlotSize;
        h->slot_count = slot_count_;
#ifndef _WIN32
        h->pid = static_cast<int32_t>(getpid());
#endif
        h->epoch = 1;
        h->clean = 0;
        h->runtime_level = static_cast<uint16_t>(c.level);
    } else {
        slot_count_ = h->slot_count ? h->slot_count : desired_slots;
        if (h->clean == 0) {
            export_::ExportOptions eo = make_options("crash", false);
            export_::ExportResult r =
                export_::export_window(*h, slots, slot_count_, dir_, eo);
            if (r.count > 0 && recovered_path) *recovered_path = r.path;
            retire_crashed_epoch(h);
            std::fprintf(stderr, "[logguard] recovered %d records -> %s\n",
                         r.count, r.path.c_str());
            enforce_retention();
        }
        h->epoch += 1;
        h->clean = 0;
        h->crashed = 0;
#ifndef _WIN32
        h->pid = static_cast<int32_t>(getpid());
#endif
        level_.store(h->runtime_level ? h->runtime_level : c.level);
    }
    epoch_ = h->epoch;
    heartbeat_.store(plat::mono_now_ns(), std::memory_order_relaxed);
#ifndef _WIN32
    msync(h, kHeaderSize, MS_SYNC);
#endif

    // Register first; the first instance wins primary and owns signal hooks.
    int handle = registry_add(this);
    if (handle == kInvalidHandle) {
        std::fprintf(stderr, "[logguard] too many recorder instances (max %d)\n",
                     kMaxRecorders);
        plat::unmap(mapping_);
        base_ = nullptr;
        return -1;
    }
    primary_ = (handle == registry_primary_handle());
    if (primary_) install_primary_hooks(opts);

    run_ = true;
    worker_ = std::thread([this] { worker_loop(); });
    syncer_ = std::thread([this] { syncer_loop(); });
    if (stall_ms_ > 0 && primary_)
        watchdog_ = std::thread([this] { watchdog_loop(); });

    // Seed declarative rules plus the built-in ERROR trigger (10 s cooldown).
    for (const auto& rule : c.initial_rules) rules_.add(rule);
    add_rule(4 /*ERROR*/, "", "", 1, 1000, 10000);
    return 0;
}

void Recorder::close() {
    if (!base_) return;
    run_ = false;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (syncer_.joinable()) syncer_.join();
    if (watchdog_.joinable()) watchdog_.join();

    auto* h = static_cast<FileHeader*>(base_);
    h->clean = 1;
    h->runtime_level = static_cast<uint16_t>(level_.load());
#ifndef _WIN32
    msync(base_, size_, MS_SYNC);
#endif
    // Detach from the registry before unmapping so a stale signal cannot touch
    // freed memory. If this was primary, registry_remove promotes another live
    // instance; re-bind the process hooks to it, otherwise clear the crash
    // header so a later fault cannot write into this unmapped region.
    bool was_primary = primary_;
    for (int i = 0; i < kMaxRecorders; ++i) {
        if (registry_get(i) == this) {
            registry_remove(i);
            break;
        }
    }
    if (was_primary) {
        if (Recorder* next = registry_primary())
            next->on_promoted();
        else
            lg::crash::set_header(nullptr, nullptr);
    }
    primary_ = false;
    plat::unmap(mapping_);
    base_ = nullptr;
}

// ---------------- rules ----------------

void Recorder::evaluate_rules(uint16_t lvl, const char* cat, const char* msg) {
    bool fired = rules_.evaluate(lvl, cat, msg) > 0;
    if (!fired) {
        std::lock_guard<std::mutex> lk(seq_mtx_);
        int64_t now = plat::mono_now_ns();
        for (auto& sq : sequences_) {
            if (sq.observe(lvl, cat, msg, now)) {
                fired = true;
                break;
            }
        }
    }
    if (fired) enqueue_export("rule", 10LL * 1000000000LL);
}

int Recorder::add_rule(uint16_t min_level, const char* category,
                       const char* substr, int threshold, int window_ms,
                       int cooldown_ms) {
    diag::Rule r;
    r.min_level = min_level;
    r.category = category ? category : "";
    r.substr = substr ? substr : "";
    r.threshold = threshold;
    r.window_ns = int64_t(window_ms > 0 ? window_ms : 1000) * 1'000'000LL;
    r.cooldown_ns = int64_t(cooldown_ms > 0 ? cooldown_ms : 10000) * 1'000'000LL;
    return rules_.add(r);
}

void Recorder::clear_rules() { rules_.clear(); }

int Recorder::add_sequence_rule(std::vector<diag::SeqStep> steps, int window_ms,
                                int cooldown_ms) {
    if (steps.size() < 2) return -1;
    std::lock_guard<std::mutex> lk(seq_mtx_);
    if (sequences_.size() >= 16) return -1;
    int64_t win = int64_t(window_ms > 0 ? window_ms : 5000) * 1'000'000LL;
    int64_t cool = int64_t(cooldown_ms > 0 ? cooldown_ms : 10000) * 1'000'000LL;
    sequences_.emplace_back(std::move(steps), win, cool);
    return static_cast<int>(sequences_.size()) - 1;
}

void Recorder::clear_sequence_rules() {
    std::lock_guard<std::mutex> lk(seq_mtx_);
    sequences_.clear();
}

// ---------------- hot path ----------------

void Recorder::commit_slot(uint16_t lvl, const char* category, const char* msg,
                           const char* fields, bool apply_rules) {
    // Log-storm suppression is opt-in and applies only to ordinary user text
    // logs: structured KV events, crash markers and captured stacks always
    // pass through untouched.
    if (burst_enabled_ && fields == nullptr && apply_rules) {
        std::string key;
        {
            const char* c = category ? category : "";
            const char* m = msg ? msg : "";
            key.reserve(std::strlen(c) + std::strlen(m) + 1);
            key.append(c);
            key.push_back('\x1f');
            key.append(m);
        }
        cap::BurstDecision d;
        {
            std::lock_guard<std::mutex> lk(burst_mtx_);
            d = burst_->on_event(key, plat::mono_now_ns());
        }
        // First emit the collapsed-run summary if one came due.
        if (d.pending_summary > 0) {
            char line[96];
            std::snprintf(line, sizeof(line),
                          "previous message repeated %llu times",
                          (unsigned long long)d.pending_summary);
            write_slot_core(lvl, category ? category : "_logguard",
                            line, nullptr, false);
        }
        if (!d.emit_current) return; // duplicate swallowed inside the window
    }
    write_slot_core(lvl, category, msg, fields, apply_rules);
}

void Recorder::write_slot_core(uint16_t lvl, const char* category,
                               const char* msg, const char* fields,
                               bool apply_rules) {
    if (!base_) return;
    auto* h = static_cast<FileHeader*>(base_);
    uint64_t ticket = __atomic_fetch_add(&h->next_ticket, 1ull, __ATOMIC_RELAXED);
    uint64_t idx = ticket & (slot_count_ - 1);
    auto* dst = reinterpret_cast<Slot*>(static_cast<char*>(base_) +
                                        kHeaderSize) + idx;

    Slot s;
    std::memset(&s, 0, sizeof(s));
    s.epoch = epoch_;
    s.ticket = ticket;
    s.mono_ns = plat::mono_now_ns();
    s.wall_ns = plat::wall_now_ns();
    s.tid = plat::current_tid();
    s.level = lvl;
    if (category) {
        size_t cl = std::strlen(category);
        if (cl > kCategoryCap) cl = kCategoryCap;
        std::memcpy(s.category, category, cl);
        s.cat_len = static_cast<uint16_t>(cl);
    }
    const char* req = registry_request();
    size_t rl = req ? std::strlen(req) : 0;
    if (rl > kRequestCap) rl = kRequestCap;
    std::memcpy(s.req_id, req, rl);
    s.req_len = static_cast<uint16_t>(rl);

    size_t cap = kPayloadCap - 1;
    size_t total = 0;
    if (fields && *fields) {
        size_t ml = msg ? std::strlen(msg) : 0;
        size_t fl = std::strlen(fields);
        if (ml + 1 + fl > cap) {
            if (fl <= cap) {
                while (ml > 0 && ml + 1 + fl > cap) --ml;
            } else {
                ml = 0; fl = cap;
            }
            __atomic_add_fetch(&h->n_truncated, 1ull, __ATOMIC_RELAXED);
        }
        if (msg) std::memcpy(s.payload, msg, ml);
        s.payload[ml] = '\0';
        std::memcpy(s.payload + ml + 1, fields, fl);
        total = ml + 1 + fl;
        s.flags = kFlagKv;
    } else {
        size_t n = msg ? std::strlen(msg) : 0;
        if (n > cap) {
            n = cap;
            __atomic_add_fetch(&h->n_truncated, 1ull, __ATOMIC_RELAXED);
        }
        if (msg) std::memcpy(s.payload, msg, n);
        total = n;
    }
    s.msg_len = static_cast<uint16_t>(total);

    __atomic_store_n(&dst->magic, 0u, __ATOMIC_RELEASE);
    std::memcpy(dst, &s, sizeof(s));
    __atomic_store_n(&dst->magic, kSlotMagic, __ATOMIC_RELEASE);

    if (lvl >= 5) enqueue_export("snapshot", 5LL * 1000000000LL);
    else if (apply_rules) evaluate_rules(lvl, category, fields ? nullptr : msg);
}

void Recorder::logv(uint16_t lvl, const char* category, const char* fmt,
                    va_list ap) {
    if (!base_) return;
    char buf[kPayloadCap];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0) buf[0] = '\0';
    commit_slot(lvl, category, n < 0 ? "" : buf, nullptr, true);
}

void Recorder::event(uint16_t lvl, const char* category, const char* msg,
                     const char* fields_json) {
    if (!base_) return;
    commit_slot(lvl, category, msg ? msg : "", fields_json, true);
}

void Recorder::log_lazy_v(uint16_t lvl, const char* category, const char* fmt,
                          va_list ap) {
    if (!base_) return;
    lg::lazy::Encoded enc;
    lg::lazy::encode(enc, fmt, ap);
    auto* h = static_cast<FileHeader*>(base_);
    uint64_t ticket = __atomic_fetch_add(&h->next_ticket, 1ull, __ATOMIC_RELAXED);
    uint64_t idx = ticket & (slot_count_ - 1);
    auto* dst = reinterpret_cast<Slot*>(static_cast<char*>(base_) +
                                        kHeaderSize) + idx;
    Slot s;
    std::memset(&s, 0, sizeof(s));
    s.epoch = epoch_;
    s.ticket = ticket;
    s.mono_ns = plat::mono_now_ns();
    s.wall_ns = plat::wall_now_ns();
    s.tid = plat::current_tid();
    s.level = lvl;
    if (category) {
        size_t cl = std::strlen(category);
        if (cl > kCategoryCap) cl = kCategoryCap;
        std::memcpy(s.category, category, cl);
        s.cat_len = uint16_t(cl);
    }
    const char* req = registry_request();
    size_t rl = req ? std::strlen(req) : 0;
    if (rl > kRequestCap) rl = kRequestCap;
    std::memcpy(s.req_id, req, rl);
    s.req_len = uint16_t(rl);
    size_t n = sizeof(enc);
    if (n > kPayloadCap) n = kPayloadCap;
    std::memcpy(s.payload, &enc, n);
    s.msg_len = uint16_t(n);
    s.flags = kFlagLazy;
    __atomic_store_n(&dst->magic, 0u, __ATOMIC_RELEASE);
    std::memcpy(dst, &s, sizeof(s));
    __atomic_store_n(&dst->magic, kSlotMagic, __ATOMIC_RELEASE);
    if (lvl >= 5) enqueue_export("snapshot", 5LL * 1000000000LL);
    else evaluate_rules(lvl, category, nullptr);
}

// ---------------- controls ----------------

void Recorder::set_request(const char* id) { registry_set_request(id); }
void Recorder::clear_request() { registry_clear_request(); }

void Recorder::set_level(int lvl) {
    level_.store(lvl, std::memory_order_relaxed);
    if (base_) {
        auto* h = static_cast<FileHeader*>(base_);
        h->runtime_level = static_cast<uint16_t>(lvl);
    }
}

int Recorder::get_level() const {
    return level_.load(std::memory_order_relaxed);
}

void Recorder::heartbeat() {
    heartbeat_.store(plat::mono_now_ns(), std::memory_order_relaxed);
}

void Recorder::trigger(const char* reason) {
    if (!base_) return;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "manual snapshot triggered: %s",
                  reason ? reason : "(none)");
    // A literal WARN marker that bypasses rule evaluation.
    commit_slot(3 /*WARN*/, "_logguard", buf, nullptr, false);
    enqueue_export("snapshot", 5LL * 1000000000LL);
}

lg::Health Recorder::health() const {
    lg::Health r;
    if (base_) {
        auto* h = static_cast<const FileHeader*>(base_);
        r.truncated = __atomic_load_n(&h->n_truncated, __ATOMIC_RELAXED);
        r.snapshots = __atomic_load_n(&h->n_snapshots, __ATOMIC_RELAXED);
        r.stalls = __atomic_load_n(&h->n_stalls, __ATOMIC_RELAXED);
    }
    r.triggers_dropped = triggers_dropped_.load();
    r.rule_fires = rules_.fire_count();
    return r;
}

// ---------------- offline recovery ----------------

int Recorder::export_readonly(const char* ring_path, bool redact,
                              std::string* out_path, int* out_count) {
    plat::Mapping mapping;
    if (!plat::map_open_rw(ring_path, mapping)) {
        perror("open ring");
        return -1;
    }
    void* base = mapping.base;
    auto* h = static_cast<FileHeader*>(base);
    auto* slots = reinterpret_cast<Slot*>(static_cast<char*>(base) +
                                          kHeaderSize);

    int rc;
    if (std::memcmp(h->magic, "LGRING02", 8) != 0) {
        std::fprintf(stderr, "not a logguard v2 ring file\n");
        rc = -1;
    } else if (h->clean) {
        std::printf("previous session shut down cleanly; nothing to recover\n");
        rc = 1;
    } else {
        export_::Redactor red;
        export_::ExportOptions eo;
        eo.zstd = false;
        eo.prefix = "crash";
        eo.redactor = redact ? &red : nullptr;
        export_::ExportResult r = export_::export_window(
            *h, slots, h->slot_count, plat::parent_dir(ring_path), eo);
        retire_crashed_epoch(h);
#ifndef _WIN32
        msync(base, kHeaderSize, MS_SYNC);
#endif
        if (out_path) *out_path = r.path;
        if (out_count) *out_count = r.count;
        std::printf("recovered %d records -> %s\n", r.count, r.path.c_str());
        rc = 0;
    }
    plat::unmap(mapping);
    return rc;
}

} // namespace lg::cap
