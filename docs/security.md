# Security & privacy model

## The ring file holds unredacted data

The ring stores exactly what applications log, in plaintext, for fast
overwrite. It is created with mode **0600** in a caller-chosen directory.

Treat that directory with the same sensitivity as the process's memory:

- Put it behind application-private storage (`/var/lib/<app>/`), not world-
  readable temp dirs for production deployments.
- Do not log secrets, tokens or full PII payloads into LogGuard macros, the
  same way you would not log them anywhere else.
- Device images and log bundles should exclude or wipe the ring unless
  incident collection is intended.

A redaction/filtering layer at export time is on the roadmap; until then,
redaction is the application's responsibility.

## Report files

- Exported `.lgp` files inherit the umask of the exporting process. Operators
  should collect them with the same care as core dumps.
- The crash marker records signal number and fault address only — never
  register contents or stack memory.

## Reader attack surface

LogGuard will need a hardened parser once it implements anything beyond the
minimal MCAP writer output. Treat any externally supplied `.lgp` as untrusted:

- The preferred long-term position is to rely on the official MCAP library's
  hardened reader rather than maintain a second parser; `lgtool cat` in v0.1
  is a minimal scanner for files LogGuard itself produced, not a general
  untrusted-input parser. Do not open arbitrary MCAP files with it yet.

## Crash handler coexistence

The handler restores `SIG_DFL` and re-raises after writing its marker. It is
designed to compose with Breakpad/Crashpad: install LogGuard **first**, so the
later handler chains as the outer reporter. Documented integration examples
for both are planned; test the ordering in your own process before shipping.

## Supply chain

- v0.1 has zero third-party build/runtime dependencies (C++17 + pthreads +
  libc). This is a deliberate property; additions require justification.
- MCAP library adoption (for chunking/indexing) will be vendored at a pinned
  version with its license and CVE posture recorded here.
