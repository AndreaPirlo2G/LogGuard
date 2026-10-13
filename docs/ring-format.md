# Ring file format (v1)

One process owns one ring file. Default path is user-supplied; default size is
64 MiB. File mode is 0600. All integers are little-endian, all structures are
packed to 1 byte.

## Layout

```
offset 0                 4096
┌──────────────────────────────────────┐
│ FileHeader (one page)                 │
├──────────────────────────────────────┤
│ Slot[0]        ┐                       │
│ Slot[1]        │ slot_count × 256 B    │
│ ...            │ (default 262144)      │
│ Slot[N-1]      ┘                       │
└──────────────────────────────────────┘
```

The file is created with `ftruncate` and mapped `MAP_SHARED`; `MAP_POPULATE` is
a future option for latency-sensitive setups.

## FileHeader

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 8 | magic | ASCII `LGRING01` |
| 8 | 4 | version | 1 |
| 12 | 4 | slot_size | 256 |
| 16 | 8 | slot_count | power of two |
| 24 | 4 | pid | owning process at creation/last start |
| 28 | 4 | epoch | generation counter, bumped per start/recovery |
| 32 | 4 | clean | 1 after `lg_shutdown()`; crash detection reads this |
| 36 | 4 | crashed | set to 1 by the signal handler |
| 40 | 4 | sig | fatal signal number |
| 44 | 8 | fault_addr | `siginfo_t.si_addr` |
| 52 | 8 | crash_mono_ns | CLOCK_MONOTONIC at crash |
| 60 | 8 | next_ticket | global atomic ticket counter |

## Slot (256 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic (`LGS1`; commit point) |
| 4 | 4 | epoch |
| 8 | 8 | ticket |
| 16 | 8 | mono_ns (CLOCK_MONOTONIC) |
| 24 | 8 | wall_ns (CLOCK_REALTIME) |
| 32 | 8 | tid (Linux gettid) |
| 40 | 2 | level (0–5) |
| 42 | 2 | flags (reserved) |
| 44 | 2 | msg_len |
| 46 | 2 | cat_len |
| 48 | 16 | category (truncated, no NUL guarantee) |
| 64 | 192 | payload: message text (truncated, no NUL guarantee) |

Length-prefixed fields mean readers never depend on NUL termination.

## Design trade-offs

- **Fixed slot size** trades long-message fidelity for a genuinely simple,
  allocation-free hot path and O(1) overwrite. Messages over 191 bytes are
  truncated; route full text to a conventional sink too. A future string-block
  area can relax this without changing the commit protocol.
- **One global ticket counter** is a contended atomic but not a lock; on
  realistic multi-socket workloads it is far cheaper than per-call formatting.
  Per-thread ticket shards are a v2 optimization if benchmarks demand it.
- **No checksums in the ring.** The kernel and filesystem own durability of
  mapped pages; adding checksums per slot would cost hot-path cycles for a
  corruption mode the OS already covers. Exported MCAP files carry the
  container's own integrity mechanisms.
