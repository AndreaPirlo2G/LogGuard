# LogGuard

[![build](https://img.shields.io/badge/build-passing-brightgreen)](.github/workflows/ci.yml)
[![platform](https://img.shields.io/badge/platform-Linux-lightgrey)](#platform-support)
[![license](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
[![standard](https://img.shields.io/badge/container-MCAP-success)](https://mcap.dev/)

**A crash-safe flight recorder for C++ applications.**

Your logs are written to a fixed-size, memory-mapped ring buffer. They never
grow your disk. When the process crashes, is killed, hangs, or you trigger a
snapshot manually, LogGuard exports the surrounding time window — including
all-thread stack traces — as a single self-contained file in standard
[MCAP](https://mcap.dev/) format. No daemon, no network, no lock-in: open the
result in [Foxglove Studio](https://foxglove.dev/) or any MCAP reader.

> Status: **v0.4**, Linux implemented & tested; Windows port has a
> defined platform boundary (stub included). v0.4 adds true **multi-instance**
> recorders behind stable C ABI handles, an **ordered event-sequence rule**
> engine, optional stack **symbolization** at export, **privacy redaction**, a
> defensive **MCAP verifier** (`lgtool verify`, fuzz-hardened), ring health
> **auditing** (`lgtool audit`), a compile-time **zero-cost log cutoff**,
> rolling chunks with a **time index for O(log n) windowed extraction**
> (`lgtool range`), and deterministic parser **mutation fuzzing**.
> See the [roadmap](docs/roadmap.md).

## Why

| Tool | What it gives you | Gap |
|---|---|---|
| spdlog / quill | Fast sinks | On `SIGSEGV`, whatever was buffered is gone |
| Tracy | Real-time profiling | Built for frame analysis, not incident packaging |
| LTTng/perf | Kernel tracing | Separate toolchain, heavyweight for app teams |
| Crashpad | Minidump at crash | No structured log window, no event context |
| Sentry breadcrumbs | Pre-crash window | Requires a networked service; unusable on edge/robot/air-gapped devices |

LogGuard is the offline, embeddable middle ground: **the last 60 seconds
before a crash, as one file, with zero network calls.**

## 30-second demo

```cpp
#include <logguard/logguard.h>

int main() {
    lg_init("app.ring");                    // fixed-size ring, crash-safe
    LG_INFO("orders", "request %d received", 42);
    // ... NULL dereference, abort, kill -9, power loss ...
}
```

```bash
$ ./crash_demo app.ring
Segmentation fault (core dumped)
$ ./crash_demo app.ring                      # next launch auto-recovers
[logguard] crash report available: ./crash-1791817982.lgp (33 records)
$ lgtool cat crash-*.lgp | tail -3
... "level":"CRITICAL","category":"_logguard",
    "msg":"logguard crash marker: signal=11 fault_addr=0x0 ..."
... "level":"CRITICAL","category":"_stack",
    "msg":"crashing thread stack frames=6 557b07d5975d ..."
```

The same window opens in Foxglove Studio — LogGuard writes standard MCAP,
not a private format.

## Features (v0.2)

**Capture**
- **Crash-safe ring** — fixed-size mmap file, lock-free single-instruction
  commit; records survive `SIGSEGV`, `SIGABRT` and `SIGKILL`.
- **Crashing-thread stack trace** — captured from an alternate signal stack,
  stored as raw return addresses (symbolize offline with `addr2line`).
- **Stall watchdog** — when the event loop stops calling `lg_heartbeat()`,
  every thread's stack is dumped and a report is exported. Catches deadlocks
  and livelocks that never produce a crash.
- **Live tail** — `lgtool tail app.ring` reads a *running* process's ring
  directly; inspect the black box without waiting for a failure.

**Diagnosis**
- **Structured events** — `LG_EVENT` attaches typed key/value fields to a
  record; fields survive into MCAP and are queryable, not just printf text.
- **Rule engine** — fire a snapshot on an error *pattern*, a *rate* threshold
  ("3 payment failures in 2 s"), category, or level, each with its own
  cooldown. This is the trigger-based flight recorder, not just crash capture.
- **Request correlation** — thread-local `request_id`, auto-attached to every
  record; follow one request across threads with one query.
- **Error fingerprints** — variable tokens (ids, numbers, addresses, quoted
  strings) are normalized into stable signatures, so the same bug at order
  9003 and 9004 is one fingerprint, not two.
- **Cross-version diff** — `lgtool diff baseline candidate` reports added,
  removed, and frequency-regressed signatures with a non-zero exit code.
  Wire it into CI to block a PR that introduces a new error path.

**Output & ops**
- **Standard MCAP output** — schema/channel records, self-describing, opens in
  Foxglove Studio and any MCAP reader; no private binary format.
- **Indexed chunks** — a package is written as rolling chunks with a trailing
  time index, so a time-window query (`lgtool range`) binary-searches the index
  and inflates only the chunks that overlap, instead of scanning the whole
  file. Old single-chunk packages still work via an automatic linear fallback.
- **CLI** — `recover`, `info`, `cat`, `query`, `report`, `fingerprint`,
  `diff`, `tail`, `scan`, `range`, `verify`, `manifest`, `audit`, `prune`.
- **Self-contained HTML report** — one file to attach straight to a ticket.
- **Runtime log-level changes** — `SIGUSR1` cycles level on a live process.
- **Bounded resources, hard isolation** — fixed file, capped memory, no
  exceptions escape into business code, no threads after `lg_shutdown`.

## Structured events & rules

```cpp
// Capture a window when "payment failed" recurs 3 times within 2 seconds.
lg_add_rule(LG_LEVEL_ERROR, "payment", "failed",
            /*threshold*/ 3, /*window_ms*/ 2000, /*cooldown_ms*/ 30000);

LG_EVENT(LG_LEVEL_ERROR, "payment", "payment failed at gateway",
         logguard::Fields().num("order", 9003)
                           .str("gateway", "stripe")
                           .num("amount_ms", 290)
                           .flag("retryable"));
// -> {"...","msg":"payment failed at gateway",
//     "fields":{"order":9003,"gateway":"stripe","amount_ms":290,"retryable":true}}
// third occurrence within 2 s exports rule-<ts>.lgp
```

## CLI

```bash
lgtool recover <ring>                    # export an unclean ring
lgtool info    <file.lgp>                # counts, levels, categories, span
lgtool cat     <file.lgp>                # records as JSON lines
lgtool query   <file.lgp> 'request_id":"tx-3'   # substring filter
lgtool range   <file.lgp> --from-ns A --to-ns B  # monotonic window, index seek
lgtool redact  <file.lgp> <out.lgp>       # rewrite with PII/secrets masked
lgtool report  <file.lgp> report.html    # self-contained HTML report
lgtool tail    <ring>                    # live view of a running process
lgtool fingerprint <file.lgp|file.fp>    # error signatures + counts
lgtool diff <baseline> <candidate>       # new/removed/regressed; exit 1 on regression
lgtool verify <file.lgp>                 # defensive container check; exit 1 if corrupt
lgtool manifest <file.lgp>               # provenance + failure-time system snapshot
lgtool audit  <ring>                     # ring health: torn slots, fill, epochs
lgtool prune  <dir> --files 50 --mb 64   # enforce retention (--dry-run supported)
```

CI gate:

```bash
lgtool fingerprint main.lgp > base.fp
lgtool fingerprint pr.lgp   > cand.fp
lgtool diff base.fp cand.fp     # non-zero exit blocks the PR
```

## API

```c
int  lg_init_opts(const char* ring_path, const LgOpts* opts);
void lg_shutdown(void);

void LG_INFO(category, fmt, ...);        /* also DEBUG/WARN/ERROR/CRITICAL */
void lg_trigger_snapshot(const char* reason);

void lg_event(int level, const char* category, const char* msg,
              const char* fields_json); /* or use LG_EVENT + Fields builder */
int  lg_add_rule(int min_level, const char* category, const char* substr,
                 int threshold, int window_ms, int cooldown_ms);
void lg_clear_rules(void);

void lg_set_request(const char* id);     /* or RAII: ScopedRequest */
void lg_clear_request(void);

void lg_set_level(int level);            /* LG_LEVEL_DEBUG..CRITICAL */
int  lg_get_level(void);
void lg_heartbeat(void);                 /* feed the stall watchdog */

void lg_health(LgHealth* out);           /* truncated / snapshots / stalls */
```

## Fault model — what is guaranteed, what is not

| Event | Records retained? | How |
|---|---|---|
| `SIGSEGV` / `SIGABRT` / `SIGBUS` / `SIGFPE` | Yes + crash marker + crashing-thread stack | signal handler, async-signal-safe |
| `SIGKILL` (`kill -9`) | Yes | committed slots already live in the page cache |
| Deadlock / event-loop stall | Yes + **all-thread** stacks | independent watchdog thread |
| Power loss / OS freeze | Records already flushed by the kernel; last few writes may be lost | mmap, no explicit fsync per record |
| Hardware corruption | Out of scope |

## Performance

Hot path (write + submit), measured on a cloud VM, single writer, default
256-byte slots:

| Configuration | Cost per call |
|---|---|
| Current v0.1 (foreground `vsnprintf`) | ~1.8 µs |

The ring commit itself is one fetch-add + one memcpy + a release store.
Essentially all of the measured cost is foreground formatting. Deferred
formatting (store args, format on the export thread) is the v0.2 target,
along with p50/p99 reporting under contention. Reproduce with
`benchmarks/bench_hotpath`.

## Building

Requires C11/C++17, pthread. **No hard third-party dependencies** — zstd is
loaded at runtime via `dlopen` when present and compression is enabled, and
the library silently falls back to uncompressed chunks otherwise.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Targets: the `logguard` library (static/shared), `lgtool`, and examples
`crash_demo`, `basic`, `c_abi`. Install with `cmake --install build`.

Options: `-DLOGGUARD_ENABLE_ZSTD=ON|OFF`, `-DLOGGUARD_BUILD_TOOLS/EXAMPLES/TESTS`.

Shipping a release build that must compile logging out entirely? Set
`-DLG_LOGGING_ENABLED=OFF` to erase every convenience macro (arguments
included), or `-DLG_COMPILE_MIN_LEVEL=4` to keep warnings and errors only;
the same macros are available per-TU (`LG_LOGGING_ENABLED`,
`LG_COMPILE_MIN_LEVEL`, C ABI `LG_C_LOG`/`LG_C_EVENT`).

## Layered design

```
include/logguard/      public C++ facade (logguard.h), stable C ABI (c_api.h),
                       compile-time cutoff (config.h)
src/platform/          OS boundary, no upward deps: clock, thread id, file
                       utils, mmap mapping, crash handlers, stack sampler
src/core/              on-disk layout, constants, lock-free ring primitives,
                       JSON escaping, lazy encode, epoch recovery, ring audit
src/diagnosis/         frequency + ordered-sequence rules, fingerprints,
                       signature diff, stack symbolization
src/export/            slot rendering, MCAP writer, zstd (dlopen) codec,
                       privacy redactor, defensive MCAP verifier, exporter
src/capture/           Recorder orchestration: config, instance registry,
                       watchdog, snapshots, multi-instance lifetime
src/capi/              stable C ABI wrapping the C++ layers
tools/                 lgtool CLI
```

Dependencies flow strictly downward: `capture → {core, diagnosis, export,
platform}` and every layer may use `platform`; nothing in `platform` or `core`
reaches up. The first recorder created in a process becomes the **primary**
instance that owns the (process-global) crash handler, SIGUSR1 level channel and
all-thread stack sampler; additional instances are independent ring writers
(per-module black boxes). If the primary is destroyed first, crash targeting is
re-bound to the next instance. See [Architecture](docs/ARCHITECTURE.md).

## Multi-instance (stable C ABI)

Several independent black boxes can coexist in one process:

```c
#include <logguard/c_api.h>
lg_opts2_t opts = {0};
opts.ring_slots = 1u << 18;
int h1 = lg_open("/var/log/net.ring", &opts);
int h2 = lg_open("/var/log/gpu.ring", &opts);
lg_log_to(h2, LG_LEVEL_WARN, "gpu", "gpu stall");
lg_close(h1); lg_close(h2);
```

The singleton calls (`lg_init`/`lg_log`) remain and bind to the default
instance, so existing single-ring users are unchanged. See `examples/c_multi.c`.

## Stable C ABI

Non-C++ consumers link against `<logguard/c_api.h>` — usable from C, C#
(P/Invoke), Python (cffi/ctypes), Rust, or vendor SDKs. Symbols are
versioned; existing signatures never change within a major release. See
`examples/c_abi.c`.

## Platform support

- Linux x86-64: implemented and tested (POSIX signals, sigaltstack, procfs
  stack walk, mmap). Crash handlers **chain** with previously installed
  handlers (Crashpad/Breakpad): LogGuard records its marker, then invokes the
  prior handler so minidump delivery is preserved.
- Windows: platform boundary defined (`*_windows.cpp` stubs), VEH +
  `CreateFileMapping` port is in progress; see [roadmap](docs/roadmap.md).
- Stack frames are raw addresses. Build with `-fno-omit-frame-pointer` and
  keep symbols (or a separate `.debug`) for `addr2line` resolution.

## Documentation

- [Architecture](docs/architecture.md)
- [Ring binary format](docs/ring-format.md)
- [MCAP mapping](docs/mcap-mapping.md)
- [Security & privacy model](docs/security.md)
- [Roadmap](docs/roadmap.md)
- [Contributing](CONTRIBUTING.md)

## License

[MIT](LICENSE).
