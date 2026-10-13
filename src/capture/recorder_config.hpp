#pragma once
// Recorder configuration: the single, validated knob surface a product uses to
// integrate logguard. Everything here has a safe default, so an embedder can
// lg_init(path) with zero config and later opt into capacity, compression,
// privacy and watchdog behaviour. The runtime hot path never reads this struct
// (it copies the few scalars it needs into atomics at init), so config can be
// freely extended without touching concurrency-sensitive code.
#include <cstdint>
#include <string>
#include <vector>

#include "diagnosis/rule.hpp"

namespace lg::cap {

struct RecorderConfig {
    // Path to the fixed-size flight-ring file (the black box tape).
    std::string ring_path;

    // Ring capacity. Rounded up to a power of two >= 1024 slots. The file is
    // preallocated and never grows past this.
    uint32_t ring_slots = 1u << 18; // 262144 slots * 256B = 64 MiB

    // Initial runtime verbosity (0=TRACE 1=DEBUG 2=INFO 3=WARN 4=ERROR
    // 5=CRITICAL). Mutable at runtime via SIGUSR1 / set_level.
    uint16_t level = 2;

    // Compress the incident package data chunk with zstd when the runtime
    // library is available; silently falls back to uncompressed if not.
    bool zstd_compression = false;

    // Hang watchdog: when no lg_heartbeat() arrives for this many ms, capture
    // all thread stacks and export. 0 disables the watchdog.
    int stall_threshold_ms = 0;

    // Where incident packages are written. Empty -> alongside the ring file.
    std::string output_dir;

    // Privacy: run exported text through the redaction engine (see
    // export/redaction.hpp) before serialization.
    bool redact_on_export = false;

    // Best-effort symbolization of live watchdog/sampler stack slots (module +
    // file offset, nearest dynamic symbol). Off by default; crash-time PCs are
    // always kept raw and resolved offline with addr2line.
    bool symbolize_stack = false;

    // Log-storm suppression: when the same (category, message) key repeats
    // inside burst_window_ms, collapse the run into the first event plus a
    // later "previous message repeated N times" summary, so a runaway loop
    // cannot evict useful records. Applies only to plain log events, never to
    // crash markers or captured stacks. Off by default (zero hot-path cost).
    bool burst_suppression = false;
    int burst_window_ms = 100;

    // Disk retention for exported packages: after each successful export the
    // output directory is pruned so a long-running recorder never fills the
    // robot's disk. max_packages keeps the newest N; max_package_mb bounds
    // their total size; max_age_hours drops older packages (0 = no age limit).
    uint32_t retain_max_packages = 50;
    uint64_t retain_max_mb = 64;
    int retain_max_age_hours = 0;

    // Declarative triggers installed at startup (in addition to the built-in
    // ERROR rule). Empty is fine.
    std::vector<diag::Rule> initial_rules;

    // ---- validation / normalization ----
    // Clamp/round fields into the legal range and fill derived defaults
    // (output_dir). Returns false and sets `error` only for unusable config
    // (e.g. empty ring path).
    bool normalize(std::string* error = nullptr);

    // A ready-to-use default for the given ring path.
    static RecorderConfig for_path(const std::string& path);
};

} // namespace lg::cap
