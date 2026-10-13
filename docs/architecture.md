# Architecture

## Layers

```
Application
    │  LG_INFO / LG_ERROR macros (printf-style)
    ▼
facade (logguard.cpp) ── lifecycle, va_list forwarding
    │
    ▼
ring (ring.cpp) ─────── mmap file, lock-free slots, crash handler, recovery scan
    │
    ▼
mcap_writer ─────────── minimal standard-MCAP emitter (used only at export time)
    │
    ▼
crash-<ts>.lgp  ─────── read by lgtool, official mcap CLI, Foxglove Studio
```

The export path is the only place that allocates, compresses (future) or opens a
second file. Application threads never enter it while logging.

## Threading model

- **Producers:** every application thread writes directly into the mmap'd ring.
  A single global atomic ticket counter hands out slot positions; each thread
  writes a different slot, so there is no producer-side lock.
- **Background syncer (v0.1):** one thread issues `msync(MS_ASYNC)` once per
  second to bound the power-loss loss window. It never touches slot contents.
- **Export:** performed by `lg_init` recovery on the launching thread, or by
  `lgtool recover` out-of-process.

## Commit protocol

1. `ticket = next_ticket.fetch_add(1, relaxed)`; slot index = ticket mod N.
2. Fill a stack-local 256-byte `Slot` (message formatted into its payload).
3. Reset the destination magic to zero (release), copy the slot in, then
   release-store the magic. Magic is the single commit point.
4. No contiguous high-water mark is tracked. Readers decide record existence
   from `(magic, epoch, ticket mod N == physical index)` and tolerate in-flight
   holes instead of forcing fast threads to wait for slow ones.

## Crash recovery state machine

```
lg_init()
  ├─ ring file missing/blank  → initialize header, epoch=1, run
  ├─ header.clean == 1        → previous shutdown was clean, epoch++, run
  └─ header.clean == 0        → scan all slots with current epoch,
                                 validate (magic, epoch, ticket position),
                                 sort by ticket, export crash-<ts>.lgp,
                                 append crash marker if header.crashed,
                                 retire: epoch++, clean=1, run
```

`clean` is set to 1 only by `lg_shutdown()`. A process killed by any means
leaves it at zero, which is the sole crash-detection signal — no watchdog
thread or liveness protocol is needed.

### Why epoch is needed

The ring file is reused across processes. After recovery, slots from the dead
process physically remain until overwritten. Bumping `epoch` and tagging every
new slot makes those bytes unreadable as current records even if their magic
word survives.

### Signal handler contract

- Writes only: signal number, fault address, monotonic timestamp, `crashed=1`
  (release store) into the header page.
- Then restores `SIG_DFL` and re-raises, so core dumps, external crash
  reporters (Breakpad/Crashpad ordered after us) and shell exit status all
  behave exactly as without LogGuard.
- Never calls: malloc/free, printf, mutexes, zstd, msync (not
  async-signal-safe in POSIX).

## Export correctness details

- The scan freezes a logical view via the validation triple above; slots being
  overwritten concurrently simply fail validation and are skipped.
- Records are ordered by ticket, not by file position or wall clock — async
  completion order across threads is not causal order.
- Reports produced while the process is still alive get a full MCAP with
  trailing magic; crash-recovery files are written freshly by a live process
  (next launch or lgtool), so they are complete files too. A future "export
  from inside a dying process" path may emit summary-less MCAP, which remains
  legal and sequentially readable.
