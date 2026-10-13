# Roadmap

The ordering principle: one claim must be rock solid before the next is
marketed. Version numbers are indicative.

## v0.1 — vertical slice (current)

- [x] mmap crash-safe ring, lock-free fixed-slot producers
- [x] POSIX fatal signal handler with async-signal-safe marker
- [x] Automatic and offline crash recovery
- [x] Standard MCAP reports (uncompressed) + minimal CLI
- [x] Recovery test suite (real SIGSEGV / SIGKILL) and hot-path benchmark

## v0.2 — diagnosis framework (done)

- [x] Structured event channel (`lg_event` / `LG_EVENT` + Fields builder)
- [x] Rules engine: pattern + frequency threshold + window + cooldown
- [x] Error fingerprints (number/hex/quoted-string normalization)
- [x] Baseline-vs-candidate `lgtool diff` with CI exit code
- [x] request_id thread context + `lgtool query`
- [x] `lgtool tail` live ring inspection
- [x] Self-contained HTML report export
- [x] Health counters API
- [x] Manual + CRITICAL trigger with cooldown/merge

## v0.3 — production tickets (done on Linux)

- [x] Layered architecture behind a platform interface
- [x] CMake build + install rules
- [x] Optional chunked zstd output via runtime-loaded libzstd (dlopen)
- [x] Deferred (lazy) formatting off the hot path
- [x] Stable C ABI (`logguard/c_api.h`) for C / C# / FFI consumers
- [x] Crash-handler chaining for Crashpad/Breakpad coexistence (POSIX)
- [x] Stall watchdog with all-thread stack capture
- [x] SIGUSR1 runtime level change
- [ ] Compile-out gate (`LG_ENABLE`) for zero-overhead release builds
- [ ] MCAP message-index section for indexed readers
- [ ] Export-time redaction hooks and redaction-report attachment
- [ ] Hardened reader bounds; fuzz the parser path
- [ ] p50/p99 benchmarks across message lengths and thread counts

## v0.4 — platforms and ecosystem

- [ ] Windows backend: finish `CreateFileMapping` + VEH behind the existing
      `mapping_windows.cpp` / `crash_windows.cpp` boundary
- [ ] macOS (mach exception / `pthread_mach_threads_np` stacks)
- [ ] Symbolicated stack traces (`addr2line`/DbgHelp integration or docs)
- [ ] OpenTelemetry context propagation alignment
- [ ] GitHub Action: diff PR runs against baseline and comment on new errors
- [ ] vcpkg / conan / brew packaging, SBOM

## Explicitly not planned for the core

- APM / log upload SaaS / network beaconing — offline-first is a core property.
- General-purpose database or query language inside the report format.
- Qt desktop GUI — Foxglove interoperability plus HTML reports cover this.
- AI root-cause claims — evidence-grade output only; AI may consume reports
  externally, never as a runtime dependency.
