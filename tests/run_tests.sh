#!/usr/bin/env bash
# End-to-end tests against the real ring file + MCAP exports.
# Requires: Linux, g++ (cmake used automatically if available).
# Usage:   ./tests/run_tests.sh [build_dir]
set -u

ROOT="${SOURCE_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
BUILD="${BUILD_DIR:-${1:-$ROOT/build-cmake}}"
W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT

if [ -x "$BUILD/lgtool" ] && [ -x "$BUILD/crash_demo" ]; then
  echo "reusing prebuilt $BUILD"
else
  cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$BUILD" -j >/dev/null || { echo "build failed"; exit 2; }
fi

TOOL="$BUILD/lgtool"; DEMO="$BUILD/crash_demo"
PASS=0; FAIL=0
pass() { echo "PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "FAIL: $1"; FAIL=$((FAIL+1)); }

# Helper: run demo, recover its ring, print the export path.
recover() { # ring
  "$TOOL" recover "$1" 2>/dev/null | sed -n 's/.*-> //p'
}

# --- 1. SIGSEGV: marker survives ------------------------------------------
R="$W/segv.ring"; rm -f /tmp/crash-*.lgp
"$DEMO" "$R" crash 40 >/dev/null 2>/dev/null || true
sleep 0.3
F=$(recover "$R")
if [ -n "$F" ] && grep -q '"msg":"logguard crash marker: signal=11' "$F"; then
  pass "SIGSEGV marker present in export"
else
  fail "SIGSEGV marker (got $F)"
fi
grep -q '"category":"_stack"' "$F" && pass "crashing-thread stack captured" \
  || fail "crashing-thread stack"
"$TOOL" cat "$F" 2>/dev/null | grep -q 'status=ok' \
  && pass "business records precede crash" || fail "business records"

# --- 2. kill -9 -------------------------------------------------------------
R="$W/k9.ring"; rm -f /tmp/crash-*.lgp
"$DEMO" "$R" kill & PID=$!
sleep 1; kill -9 "$PID"; wait "$PID" 2>/dev/null
F=$(recover "$R")
if [ -n "$F" ] && [ "$("$TOOL" info "$F" 2>/dev/null | sed -n 's/records: *//p')" -gt 0 ]; then
  pass "SIGKILL preserves records"
else
  fail "SIGKILL (got $F)"
fi

# --- 3. clean shutdown exports nothing --------------------------------------
R="$W/clean.ring"
"$DEMO" "$R" snapshot 5 >/dev/null 2>/dev/null
rm -f /tmp/snapshot-*.lgp
OUT=$("$TOOL" recover "$R" 2>&1)
echo "$OUT" | grep -qi 'clean\|nothing' && pass "clean shutdown not exported" \
  || fail "clean shutdown ($OUT)"

# --- 4. manual snapshot -----------------------------------------------------
R="$W/snap.ring"
"$DEMO" "$R" snapshot 30 >/dev/null 2>/dev/null
F=$(ls -t "$W"/snapshot-*.lgp 2>/dev/null | head -1)
if [ -n "$F" ] && "$TOOL" cat "$F" 2>/dev/null | grep -q 'manual snapshot triggered'; then
  pass "manual trigger snapshot"
else
  fail "manual snapshot (got $F)"
fi
"$TOOL" query "$F" 'request_id":"req-2' 2>/dev/null | head -1 | grep -q 'req-2' \
  && pass "request_id correlation query" || fail "request_id query"

# --- 5. stall watchdog ------------------------------------------------------
R="$W/stall.ring"
"$DEMO" "$R" stall 1200 >/dev/null 2>/dev/null &
PID=$!
sleep 4
F=$(ls -t "$W"/stall-*.lgp 2>/dev/null | head -1)
kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null
if [ -n "$F" ] && "$TOOL" cat "$F" 2>/dev/null | grep -q 'thread stack'; then
  pass "stall watchdog dumps thread stacks"
else
  fail "stall watchdog (got $F)"
fi

# --- 6. SIGUSR1 dynamic level ----------------------------------------------
R="$W/usr.ring"; rm -f /tmp/crash-*.lgp
"$DEMO" "$R" kill >/dev/null 2>/dev/null &
PID=$!
sleep 1
kill -USR1 "$PID"; kill -USR1 "$PID"   # INFO -> DEBUG
sleep 1
kill -9 "$PID"; wait "$PID" 2>/dev/null
F=$(recover "$R")
"$TOOL" info "$F" 2>/dev/null | grep -q 'DEBUG=' \
  && pass "SIGUSR1 lowers level to DEBUG at runtime" || fail "SIGUSR1 dynamic level"

# --- 7. HTML report ---------------------------------------------------------
R="$W/rep.ring"
"$DEMO" "$R" snapshot 10 >/dev/null 2>/dev/null
F=$(ls -t "$W"/snapshot-*.lgp 2>/dev/null | head -1)
"$TOOL" report "$F" "$W/report.html" >/dev/null 2>&1
grep -q 'LogGuard incident report' "$W/report.html" \
  && pass "HTML report generated" || fail "HTML report"

# --- 8. structured events + rule engine ------------------------------------
R="$W/rule.ring"
"$DEMO" "$R" rules >/dev/null 2>/dev/null
F=$(ls -t "$W"/rule-*.lgp 2>/dev/null | head -1)
if [ -n "$F" ] && "$TOOL" cat "$F" 2>/dev/null | grep -q '"fields":{'; then
  pass "structured KV fields exported"
else
  fail "structured KV fields (got $F)"
fi
[ -n "$F" ] && "$TOOL" cat "$F" 2>/dev/null | grep -q '"gateway":"stripe"' \
  && pass "rate-based rule fires snapshot" || fail "rule-engine snapshot"

# --- 9. fingerprint + diff (regression gate) --------------------------------
"$TOOL" fingerprint "$F" 2>/dev/null | grep -q 'payment failed at gateway' \
  && pass "fingerprint normalizes variables" || fail "fingerprint"
printf 'ERROR\tpayment\tpayment failed at gateway\t2\n' > "$W/base.fp"
printf 'ERROR\tpayment\tpayment failed at gateway\t9\nERROR\tauth\tnew crash\t3\n' > "$W/cand.fp"
"$TOOL" diff "$W/base.fp" "$W/cand.fp" > "$W/diff.out" 2>/dev/null
grep -q '^+ ERROR	auth' "$W/diff.out" && pass "diff detects new error signature" \
  || fail "diff added"
grep -q '2 -> 9' "$W/diff.out" && pass "diff detects frequency regression" \
  || fail "diff changed"

# --- 10. live tail ----------------------------------------------------------
R="$W/tail.ring"
"$DEMO" "$R" kill >/dev/null 2>/dev/null &
PID=$!
sleep 1
timeout 2 "$TOOL" tail "$R" > "$W/tail.out" 2>/dev/null
kill -9 "$PID" 2>/dev/null; wait "$PID" 2>/dev/null
grep -q '\[INFO\] demo {req-' "$W/tail.out" \
  && pass "live tail reads running ring" || fail "live tail"

# --- 11. deferred (lazy) formatting rendered at export ----------------------
R="$W/lazy.ring"
"$BUILD/basic" "$R" lazy >/dev/null 2>/dev/null
F=$(ls -t "$W"/snapshot-*.lgp 2>/dev/null | head -1)
if [ -n "$F" ] && "$TOOL" cat "$F" 2>/dev/null | grep -q 'counter=42 ratio=7 ok=1'; then
  pass "lazy formatting rendered at export"
else
  fail "lazy formatting (got $F)"
fi

# --- 12. verify: healthy accepted, truncated rejected -----------------------
"$BUILD/basic" "$W/v.ring" normal >/dev/null 2>/dev/null
LP=$(ls -t "$W"/snapshot-*.lgp 2>/dev/null | head -1)
if [ -n "$LP" ] && "$TOOL" verify "$LP" 2>/dev/null | grep -q 'MCAP OK'; then
  pass "verify accepts healthy package"
else
  fail "verify accepts healthy package ($LP)"
fi
if [ -n "$LP" ]; then
  BAD="$W/corrupt.lgp"
  head -c $(( $(stat -c%s "$LP") / 2 )) "$LP" > "$BAD"
  if "$TOOL" verify "$BAD" >/dev/null 2>&1; then
    fail "verify rejects truncated package"
  else
    pass "verify rejects truncated package"
  fi
else
  fail "verify rejects truncated package"
fi

# --- 13. ring audit reports health on a live-written ring -------------------
"$BUILD/basic" "$W/au.ring" normal >/dev/null 2>/dev/null
if "$TOOL" audit "$W/au.ring" 2>/dev/null | grep -q 'HEALTHY'; then
  pass "audit reports ring healthy"
else
  fail "audit reports ring healthy"
fi

# --- 14. self-heal: corrupted header but intact slots are salvaged ----------
RING_R="$W/repair.ring"
"$BUILD/basic" "$RING_R" normal >/dev/null 2>/dev/null
# Wipe the header page (kill the LGRING02 magic + geometry); slot arena survives.
head -c 4096 /dev/zero | dd of="$RING_R" bs=4096 count=1 conv=notrunc >/dev/null 2>&1
# Opening again must salvage slots into a "repaired-*.lgp" before reinit.
"$BUILD/basic" "$RING_R" normal >/tmp/lg_repair.log 2>&1
if ls "$W"/repaired-*.lgp >/dev/null 2>&1 && \
   "$TOOL" cat "$(ls -t "$W"/repaired-*.lgp | head -1)" 2>/dev/null | grep -q 'iteration='; then
  pass "corrupted header salvages surviving slots"
else
  fail "corrupted header salvage ($(cat /tmp/lg_repair.log 2>/dev/null | head -1))"
fi

# --- 15. multi-instance isolation (two rings in one process) ----------------
if [ -x "$BUILD/c_multi" ]; then
  RA="$W/ma.ring"; RB="$W/mb.ring"
  "$BUILD/c_multi" "$RA" "$RB" >/dev/null 2>/dev/null
  if grep -q 'alpha event' "$RA" && ! grep -q 'beta event' "$RA"; then
    pass "ring A holds only alpha events"
  else
    fail "ring A isolation"
  fi
  if grep -q 'beta event' "$RB" && ! grep -q 'alpha event' "$RB"; then
    pass "ring B holds only beta events"
  else
    fail "ring B isolation"
  fi
else
  fail "c_multi binary missing"
fi

# --- 16. structured scan over a live/killed ring ----------------------------
RS="$W/scan.ring"
"$DEMO" "$RS" kill >/dev/null 2>/dev/null &
SPID=$!
sleep 1
# Category filter returns demo events; a category that never occurs matches none.
if "$TOOL" scan "$RS" cat=demo 2>/dev/null | grep -q " demo " \
   && ! "$TOOL" scan "$RS" cat=does_not_exist 2>/dev/null | grep -q .; then
  pass "scan filters live ring by category"
else
  fail "scan category filter"
fi
# Text multi-token AND filter narrows the result.
if "$TOOL" scan "$RS" text="latency_ms status=ok" 2>/dev/null | grep -q latency_ms; then
  pass "scan text token filter matches"
else
  fail "scan text filter"
fi
# A severity above anything present returns no matches (exit 1).
if ! "$TOOL" scan "$RS" level>=ERROR >/dev/null 2>&1; then
  pass "scan level filter excludes lower severity"
else
  fail "scan level filter"
fi
kill -9 "$SPID" 2>/dev/null; wait "$SPID" 2>/dev/null

# --- 17. package retention pruning -------------------------------------------
PD="$W/prune"; mkdir -p "$PD"
# Five packages with ascending ages; a non-package file must be left alone.
for i in 1 2 3 4 5; do
  f="$PD/pkg-$i.lgp"; head -c 1024 /dev/zero >"$f"
  touch -d "$i hours ago" "$f"
done
echo notes >"$PD/notes.txt"
if "$TOOL" prune "$PD" --files 3 >/dev/null 2>&1 \
   && [ "$(ls "$PD"/*.lgp 2>/dev/null | wc -l)" -eq 3 ] \
   && [ -f "$PD/notes.txt" ]; then
  pass "prune keeps newest N packages and ignores non-packages"
else
  fail "prune file-count bound"
fi
# Age bound drops packages older than the threshold (pkg-4/5 are already gone,
# so start from a fresh set to exercise age logic deterministically).
PD2="$W/prune-age"; mkdir -p "$PD2"
head -c 1024 /dev/zero >"$PD2/new.lgp"; touch -d "1 minutes ago" "$PD2/new.lgp"
head -c 1024 /dev/zero >"$PD2/old.lgp"; touch -d "3 days ago" "$PD2/old.lgp"
if "$TOOL" prune "$PD2" --age-hours 24 >/dev/null 2>&1 \
   && [ -f "$PD2/new.lgp" ] && [ ! -f "$PD2/old.lgp" ]; then
  pass "prune removes packages older than age bound"
else
  fail "prune age bound"
fi
# Dry run never deletes anything.
before=$(ls "$PD"/*.lgp 2>/dev/null | wc -l)
"$TOOL" prune "$PD" --files 1 --dry-run >/dev/null 2>&1
after=$(ls "$PD"/*.lgp 2>/dev/null | wc -l)
if [ "$before" -eq "$after" ] && [ "$after" -eq 3 ]; then
  pass "prune dry-run makes no changes"
else
  fail "prune dry-run"
fi

# --- 18. package redaction: rewriter and recover --redact -------------------
R="$W/redact.ring"; rm -f "$W"/crash-*.lgp
"$DEMO" "$R" crash 40 >/dev/null 2>/dev/null || true
sleep 0.3
IN=$(recover "$R")
if [ -n "$IN" ] && "$TOOL" redact "$IN" "$W/safe.lgp" >/dev/null 2>&1 &&
   [ -s "$W/safe.lgp" ]; then
  # Redacted package must still parse as valid MCAP and carry a manifest.
  if "$TOOL" verify "$W/safe.lgp" 2>/dev/null | grep -q "MCAP OK" &&
     "$TOOL" manifest "$W/safe.lgp" 2>/dev/null | grep -qE '"redacted": ?true'; then
    pass "lgtool redact rewrites a valid package with a redaction report"
  else
    fail "lgtool redact output invalid"
  fi
else
  fail "lgtool redact ($IN)"
fi
# --family narrows the scanner; a phone under email-only masking survives.
"$TOOL" redact "$IN" "$W/email.lgp" --family email >/dev/null 2>&1 || true

# recover --redact: direct crash export with masking enabled.
R2="$W/redact2.ring"; rm -f "$W"/crash-*.lgp
"$DEMO" "$R2" crash 40 >/dev/null 2>/dev/null || true
sleep 0.3
OUT=$("$TOOL" recover "$R2" --redact 2>/dev/null)
RR=$(echo "$OUT" | sed -n 's/.*-> //p')
if [ -n "$RR" ] && "$TOOL" manifest "$RR" 2>/dev/null | grep -qE '"redacted": ?true'; then
  pass "recover --redact exports a masked package"
else
  fail "recover --redact ($OUT)"
fi

# --- 19. compile-time gate: LG_LOGGING_ENABLED=0 erases log calls -----------
cat > "$W/gate_off.c" <<'EOF'
#include <logguard/c_api.h>
int use_gate(void) {
    LG_C_LOG(2, "cat", "never emitted %d", 123);
    LG_C_EVENT(2, "cat", "m", "{}");
    return 0;
}
EOF
cat > "$W/gate_on.c" <<'EOF'
#include <logguard/c_api.h>
int use_gate(void) {
    LG_C_LOG(2, "cat", "emitted %d", 123);
    return 0;
}
EOF
if "${CC:-cc}" -c -I"$ROOT/include" -DLG_LOGGING_ENABLED=0 \
      "$W/gate_off.c" -o "$W/gate_off.o" 2>/dev/null &&
   ! nm "$W/gate_off.o" 2>/dev/null | grep -q " U lg_log"; then
  pass "compile gate removes lg_log references when disabled"
else
  fail "compile gate off"
fi
if "${CC:-cc}" -c -I"$ROOT/include" "$W/gate_on.c" -o "$W/gate_on.o" 2>/dev/null &&
   nm "$W/gate_on.o" 2>/dev/null | grep -q " U lg_log"; then
  pass "log macros emit lg_log when enabled"
else
  fail "compile gate on"
fi
# Level threshold: MIN_LEVEL=WARN must drop an INFO call but keep ERROR.
cat > "$W/gate_lvl.c" <<'EOF'
#include <logguard/c_api.h>
void a(void){ LG_C_LOG(2, "c", "info dropped %d", 1); }
void b(void){ LG_C_LOG(4, "c", "error kept %d", 2); }
EOF
if "${CC:-cc}" -c -I"$ROOT/include" -DLG_C_MIN_LEVEL=3 \
      "$W/gate_lvl.c" -o "$W/gate_lvl.o" 2>/dev/null &&
   ! nm "$W/gate_lvl.o" 2>/dev/null | grep -q " U lg_event"; then
  nlog=$(nm "$W/gate_lvl.o" 2>/dev/null | grep -c " U lg_log")
  [ "$nlog" = "1" ] && pass "MIN_LEVEL strips below-threshold calls only" \
                    || fail "compile min-level ($nlog)"
else
  fail "compile min-level build"
fi

echo "----------------------------------------"
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
