# Contributing

Thanks for your interest. LogGuard is a reliability tool, so correctness and honesty about guarantees matter more than features.

## Development

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j
./tests/run_tests.sh
./build/bench_hotpath /tmp/lg-bench.ring
```

The test suite performs real process kills (SIGSEGV and SIGKILL) in a temp directory and verifies exported reports. It must pass before sending anything that touches `src/ring.cpp` or `src/mcap_writer.cpp`.

## Ground rules

- The hot path (`ring_logv`) may not allocate, take a lock, or make a syscall per record. Benchmarks accompany any change to it.
- Crash signal handlers stay async-signal-safe: no malloc, no locks, no compression, no `msync`.
- Any new persistence behavior comes with a failure-mode entry in the guarantee table (README / docs/security.md).
- MCAP output must stay readable by the official `mcap` CLI. If you have it installed, run `mcap info` and `mcap cat` on files produced by your change.
- No unsupported platform claims. If it only works on Linux, say so.

## Commit messages

Short imperative subject line, then what/why in the body when it isn't obvious. Performance claims include the number and how to reproduce it.
