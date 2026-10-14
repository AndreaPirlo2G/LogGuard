# Changelog

All notable changes to this project are documented here.
The format is based on Keep a Changelog; versions follow semver (0.x = no API stability promise yet).

## [Unreleased]

### Added
- Packages now use **rolling chunks plus a trailing chunk time-index**. The
  writer (`export/mcap_writer.*`) flushes the active chunk once it reaches a
  size threshold (default 2 MiB, clampable to 4 KiB–64 MiB) and records, in a
  LogGuard-specific `CHUNK_INDEX` summary section (opcode `0x40`), each chunk's
  monotonic time span, absolute file offset, compressed/uncompressed sizes and
  compression flag. `McapVerifier::read_chunk_index` parses it defensively and
  `extract_range_file` answers a time-window query by binary-searching the
  index (`O(log n)` chunks) and inflating only intersecting chunks, instead of
  linearly walking/decompressing the whole package. Index-less packages
  (single-chunk files written before this change) transparently fall back to a
  full linear scan, so old `.lgp` files keep working. Older readers that don't
  know opcode `0x40` simply skip the section — the format stays forward/
  backward compatible.
- `lgtool range <file.lgp> [--from-ns N] [--to-ns N]` extracts log records
  whose monotonic timestamp falls in a window (open bounds allowed), reporting
  whether an indexed seek or the linear fallback was used and over how many
  chunks.
- A deterministic **mutation fuzz** target, `unit_mcap_fuzz`, seeds from real
  single- and multi-chunk packages (none and zstd) and applies thousands of
  byte flips, length-field corruptions, truncations and insertions, feeding
  every image to all parser entry points (verify, log/manifest extraction,
  chunk-index read, ranged seek) to prove they never crash or read out of
  bounds. It runs under AddressSanitizer/UBSan in CI-style checks.
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
- **Compile-time log gate** for zero-overhead release builds: define
  `LG_LOGGING_ENABLED=0` to erase every convenience macro (C++ `LG_INFO`, …,
  `LG_EVENT` and the new C `LG_C_LOG`/`LG_C_EVENT`) — argument construction
  included — while lifecycle calls and direct `lg_log()` stay available;
  `LG_COMPILE_MIN_LEVEL` (0 trace … 5 critical) / `LG_C_MIN_LEVEL` strip calls
  below a threshold. Exposed as the CMake options `LG_LOGGING_ENABLED` and
  `LG_COMPILE_MIN_LEVEL`, propagated `PUBLIC` to consumers.
- **Redaction tooling end-to-end**: `lgtool redact <in.lgp> <out.lgp>` rewrites
  an existing package with PII/secrets masked (optional `--family` allow-list,
  `--token` literals, `--zstd`), and `lgtool recover <ring> --redact` exports a
  crash package already masked. Every redacted manifest carries a per-family
  `redaction_report` (phone/email/id_card/bank_card/ipv4/secret/custom/total),
  and the C ABI gains `lg_recover_export_redacted`.
- **Source-level stack locations**: the export-time symbolizer now shells out
  to `addr2line -f -C` once per backing ELF (addresses batched, repeated
  frames de-duplicated) and renders `function (file.cpp:line)` after the
  module+offset and nearest symbol. It degrades silently to dladdr-only when
  binutils or debug info are unavailable.
- `lg_opts2_t` gains `symbolize_stack` so C/embedder users can opt into the
  `addr2line` source-location pass through the stable ABI; new `unit_symbolize`
  covers module resolution, zero/unmapped safety, `#NN` rendering and a
  best-effort self function → file:line assertion.
- `bench_hotpath` now measures per-call latency distributions and prints
  **p50/p99/max** in addition to the mean, with a `--matrix` sweep over message
  lengths × thread counts and `--iterations/--threads/--msg-len/--ring` flags.

### Fixed
- The chunk-index reader originally validated record bounds with pointer
  arithmetic (`body + declared_len > end`); a corrupted length near
  `UINT64_MAX` wrapped the pointer and escaped the check, allowing an
  out-of-bounds read. Found by the new mutation fuzz under AddressSanitizer;
  bounds are now compared as sizes against the remaining span, and ranged
  seeks re-validate every index offset against the actual buffer.
- Message records now carry the wall-clock time as `log_time` and the
  monotonic time as `publish_time` (previously both were the wall value),
  which is the clock the chunk index and time-window seeks are keyed on.

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
