# Changelog

All notable changes to this project are documented here.
The format is based on Keep a Changelog; versions follow semver (0.x = no API stability promise yet).

## [Unreleased]

### Added
- Every package manifest now embeds a **failure-time environment snapshot**
  (`diagnosis/system_snapshot.*`) under `"system"`: load averages, total /
  available memory, uptime, aggregate CPU busy ratio, host process count, and
  the recorder's own RSS, user/sys CPU time, thread count and open file
  descriptors. This records whether a crash correlated with OOM, overload or
  FD exhaustion. All `/proc` parsing is a set of pure, unit-tested functions;
  capture runs once at export time, never on the hot log path, and degrades to
  `null` fields when a metric is unavailable.
- `lgtool manifest <file.lgp>` prints the package provenance manifest
  (including the new `system` snapshot) as indented JSON. Extraction works for
  both uncompressed and zstd-compressed chunks and is covered by new
  `McapVerifier::extract_manifest_file` tests.
- **Disk retention for exported packages** (`export/retention.*`) keeps a
  long-running recorder from filling the robot's disk: after every successful
  export the output directory is pruned to a newest-first policy bounded by
  package count, total size and optional age. Configured through
  `RecorderConfig.retain_max_packages` (default 50), `retain_max_mb`
  (default 64) and `retain_max_age_hours` (0 = no age limit), and through the
  C ABI `lg_opts2_t.retain_*` fields. Standalone tooling:
  `lgtool prune <dir> [--files N] [--mb N] [--age-hours H] [--dry-run]`.
  Only `*.lgp` files are ever considered; all other files are left untouched.
- Every exported `.lgp` now carries a self-describing **provenance manifest**
  on a dedicated `/logguard/manifest` MCAP channel (`McapWriter::write_manifest`):
  producer/version, export trigger and timestamp, event count, compression,
  redaction/symbolization flags, crash flag and observed monotonic time span.
  `lgtool verify` counts it separately (`manifest=`) and never treats it as a
  log message; it does not leak into `lgtool cat/query`.
- **Structured query** over slots (`export/query.*`) with AND-combined
  predicates: monotonic time window, minimum level, exact category, request-id,
  thread id and multi-token substring. Exposed as `lgtool scan <ring>
  [level>=WARN] [cat=C] [req=R] [tid=T] [text=..]` for on-site filtering of a
  live/killed ring without exporting first.
- `lgtool audit` now also renders a current-epoch **session summary**:
  per-level/category breakdown, distinct request-id count, stack/KV/lazy
  event tallies, time span and peak/average event rate
  (`diagnosis/session_stats.*`).
- **Log-storm suppression** (`capture/burst_guard.*`, opt-in via
  `RecorderConfig.burst_suppression`/`burst_window_ms`): a runaway loop
  repeating the same `(category, message)` is collapsed into the first event
  plus a later `previous message repeated N times` summary, so it cannot evict
  useful records. Crash markers, captured stacks and structured KV events are
  never suppressed. Default off = zero hot-path cost.

## [0.4.0] - 2026-10-13

Multi-instance core, hardened diagnosis, and crash-safe parsing.

### Architecture
- Eliminated the former god file `ring.cpp` (695 lines): its behaviour moved to
  a real `capture::Recorder` orchestrator plus an instance registry; `ring.cpp`
  is now a thin compatibility shim bound to the default instance.
- Strictly downward dependencies across `platform → core → diagnosis/export →
  capture → capi`; see `docs/ARCHITECTURE.md`.
- First recorder opened becomes the process **primary** (owns the crash
  handler, SIGUSR1 channel and all-thread sampler); destroying it first
  re-binds crash targeting to the next live recorder.

### Added
- **Multi-instance recorders** behind stable C ABI handles: `lg_open`,
  `lg_log_to`, `lg_add_rule_to`, `lg_add_sequence_to`, etc.; singleton
  `lg_init/lg_log` remain unchanged. C++ RAII wrapper and `examples/c_multi.c`.
- **Ordered event-sequence rules** (`diagnosis/sequence_rule.*`): detect a
  chain of steps in order within a window, fire once per cooldown.
- **Stack symbolization** at export (`diagnosis/symbolize.*`): module+offset
  via `/proc/self/maps` and `dladdr`, export-only (never on the crash path).
- **Privacy redaction** (`export/redaction.*`): regex-free masking of phone,
  email, ID/bank card, IPv4 and secret assignments; wired into export behind a
  config/ABI flag with per-kind counters.
- **Defensive MCAP verifier** (`export/mcap_verify.*`): bounds-checked,
  allocation-capped walk of attacker-controlled bytes (fuzz target); CLI gains
  `lgtool verify`.
- **Ring health audit** (`core/ring_audit.*`) + `lgtool audit`: torn-slot,
  foreign-epoch, ticket-mismatch and fill reporting.
- **Compile-time zero-cost log cutoff** (`include/logguard/config.h`).
- `RecorderConfig` validation with power-of-two capacity rounding.
- **Ring self-healing** (`core/ring_repair.*`): a torn/corrupt one-page header
  no longer wipes the black box. The arena is scanned first, intact slots are
  grouped by dominant epoch, salvaged to a `repaired-*.lgp` package, and only
  then is the header reinitialised. Pure logic, fully unit-tested; covered by
  an end-to-end power-loss case.
- **Benchmark suite** (`-DLOGGUARD_BUILD_BENCH=ON`): `bench_hotpath` (per-call
  logging cost, single/multi thread) and `bench_diagnosis` (rule/sequence
  evaluation, fingerprint normalisation, redaction, ring audit, MCAP verify).

### Tests
- 6 unit binaries, ~330 assertions; 22 end-to-end shell cases covering crash,
  SIGKILL, watchdog, recovery, zstd, multi-instance isolation, verify, audit.

## [0.3.0] - 2026-10-12

Productization: layered architecture, the five "production tickets", CMake,
stable C ABI.

### Architecture
- Reorganized into explicit layers: `platform/` (file mapping + crash
  handlers), `core/` (ring, rules, watchdog, lazy format), `export/` (MCAP +
  codec), `capi/` (stable C ABI). OS-specific code sits behind interfaces.
- CMake build with options for tools/examples/tests/zstd and install rules.

### Added
- **Optional zstd compression**: libzstd loaded at runtime via `dlopen`; no
  hard dependency, automatic fallback to uncompressed chunks.
- **Deferred (lazy) formatting**: macro path stores the format string,
  per-argument type tags and values; rendering happens on the export worker,
  taking printf cost off the hot path.
- **Stable C ABI** (`logguard/c_api.h`), versioned, usable from C/C#/Python/
  Rust; `examples/c_abi.c` demonstrates it.
- **Crash-handler chaining**: POSIX handlers save and forward to any
  previously installed handler, so Crashpad/Breakpad minidumps still work.
- **Windows platform boundary**: `mapping_windows.cpp` / `crash_windows.cpp`
  with `CreateFileMapping`/VEH design (stub port, not yet functional).

## [0.2.0] - 2026-10-12

From "crash logger" to a diagnosis framework: structured events, trigger rules,
error fingerprints and cross-version comparison.

### Added
- Structured events: `lg_event` / `LG_EVENT` with a `Fields` builder; typed
  key/value fields are stored in the slot and emitted as a MCAP `fields` object.
- Rule engine: snapshot on error pattern + rate threshold within a time window,
  per-rule cooldown (`lg_add_rule` / `lg_clear_rules`); default ERROR rule.
- Error fingerprints: messages normalized by replacing ids/numbers/hex/quoted
  strings with placeholders, producing stable signatures (`lgtool fingerprint`).
- `lgtool diff`: added / removed / frequency-regressed signatures between a
  baseline and candidate, with non-zero exit code for CI gating.
- `lgtool tail`: live read-only view of a running process's ring.
- `Health.rule_fires` counter.

### Changed
- MCAP record schema gains a `fields` object property.
- Slot layout gains `FLAG_KV` (msg NUL-terminated, followed by raw JSON fields).

## [0.1.0] - 2026-10-12

First vertical slice. The goal of this release is proving one end-to-end claim:
**the pre-crash window survives process death and can be exported to a standard MCAP file.**

### Added
- Crash-safe ring: fixed-size 64 MiB mmap'd file, 256-byte fixed slots, lock-free producers.
- Hot path: atomic ticket claim + release-store commit, no allocation/lock/syscall per record.
- POSIX crash handler (SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGFPE) writing an async-signal-safe marker.
- Epoch-based generation tagging so stale slot bytes are never read as new records.
- Automatic recovery on `lg_init()` plus offline `lgtool recover`.
- Minimal standards-compliant MCAP writer (Header/Schema/Channel/Messages/DataEnd/Footer), uncompressed.
- `lgtool cat` for dumping records from any MCAP file with the LogGuard channel.
- Crash demo program and shell-based recovery test suite (`tests/run_tests.sh`).
- Hot-path microbenchmark (`benchmarks/bench_hotpath`).

### Not included (yet)
Windows, compression/chunked output, trigger engine, correlation indexes, diff, redaction, C ABI.
