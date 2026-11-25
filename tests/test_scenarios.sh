#!/usr/bin/env bash
#
# test_scenarios.sh - automated scenario tests for the watchdog (PDF Section 7).
#
# Prints PASS/FAIL per check and exits non-zero if anything failed.
# Run from anywhere:   make test     or     bash tests/test_scenarios.sh
#
# Set SAVE_SAMPLE=1 to copy the log of the main run to docs/sample_watchdog.log.
#
# Expected children (configured in src/main.c):
#   alpha  forever     -> runs until stopped
#   beta   exit-ok 8   -> exits normally after 8 s, must NOT be restarted
#   gamma  crash 4     -> exits with code 1 every 4 s, restarted 3 times, then GIVE UP
#
set -u
cd "$(dirname "$0")/.." || exit 1

LOG=watchdog.log
OUT=$(mktemp)            # watchdog stdout/stderr, shown if a run misbehaves
SHUTDOWN_WAIT=10         # seconds; watchdog's own SIGKILL timeout is 5 s
pass=0
fail=0
WD=""

cleanup() {
    if [ -n "$WD" ] && kill -0 "$WD" 2>/dev/null; then
        kill -KILL "$WD" 2>/dev/null
    fi
    pkill -KILL -f '^\./worker ' 2>/dev/null
    rm -f "$OUT"
}
trap cleanup EXIT

ok()  { echo "PASS $1"; pass=$((pass + 1)); }
bad() { echo "FAIL $1"; fail=$((fail + 1)); }

# wait_for <regex> <timeout_s>: poll the log until the regex appears.
wait_for() {
    local end=$((SECONDS + $2))
    while [ "$SECONDS" -lt "$end" ]; do
        grep -Eq -- "$1" "$LOG" 2>/dev/null && return 0
        sleep 0.2
    done
    grep -Eq -- "$1" "$LOG" 2>/dev/null
}

# check <name> <regex> [timeout_s]: PASS if the regex shows up in the log in time.
check() {
    if wait_for "$2" "${3:-3}"; then ok "$1"; else bad "$1  (log has no /$2/)"; fi
}

# check_absent <name> <regex>: PASS if the regex is NOT in the log.
check_absent() {
    if grep -Eq -- "$2" "$LOG" 2>/dev/null; then bad "$1  (unexpected /$2/)"; else ok "$1"; fi
}

# Starts ./watchdog in the background with a fresh log; sets WD.
start_watchdog() {
    rm -f "$LOG"
    "$@" ./watchdog >"$OUT" 2>&1 &
    WD=$!
    if ! wait_for "WATCHDOG START" 3; then
        bad "watchdog did not start"
        cat "$OUT"
        exit 1
    fi
}

# Waits up to SHUTDOWN_WAIT seconds for the watchdog to exit.
wait_exit() {
    local end=$((SECONDS + SHUTDOWN_WAIT))
    while kill -0 "$WD" 2>/dev/null && [ "$SECONDS" -lt "$end" ]; do sleep 0.2; done
    if kill -0 "$WD" 2>/dev/null; then
        bad "$1: watchdog still running after ${SHUTDOWN_WAIT}s"
        kill -KILL "$WD" 2>/dev/null
    fi
    wait "$WD" 2>/dev/null
    WD=""
}

# PASS if no worker process is left anywhere.
check_no_workers() {
    sleep 0.3
    local left
    left=$(pgrep -f '^\./worker ')
    if [ -z "$left" ]; then ok "$1"; else bad "$1  (still running: $left)"; fi
}

# PASS if the watchdog has no zombie (Z) children right now.
check_no_zombies() {
    local z
    z=$(ps -o pid=,stat= --ppid "$WD" | awk '$2 ~ /^Z/ {print $1}')
    if [ -z "$z" ]; then ok "$1"; else bad "$1  (zombies: $z)"; fi
}

echo "== build"
make -s all || { echo "FAIL build"; exit 1; }
ok "build"

# ---------------------------------------------------------------- run 1
echo "== run 1: normal life cycle + SIGTERM shutdown"
start_watchdog env

# 1. Normal operation: all children are started.
check "1 normal operation: alpha started" "START alpha pid=[0-9]+"
check "1 normal operation: beta started"  "START beta pid=[0-9]+"
check "1 normal operation: gamma started" "START gamma pid=[0-9]+"

# 2. Normal termination: beta exits with code 0 and is not restarted.
check        "2 normal termination: beta exit logged" "EXIT beta.*normal" 12
check_absent "2 normal termination: beta not restarted" "RESTART beta"

# 3. Unexpected termination: SIGKILL alpha -> CRASH + RESTART.
ALPHA=$(pgrep -P "$WD" -f '^\./worker alpha ')
if [ -n "$ALPHA" ]; then
    kill -KILL "$ALPHA"
    check "3 unexpected termination: crash logged" "CRASH alpha.*signal 9" 3
    check "3 unexpected termination: restarted"    "RESTART alpha attempt 1" 3
    NEW_ALPHA=$(sleep 0.5; pgrep -P "$WD" -f '^\./worker alpha ')
    if [ -n "$NEW_ALPHA" ] && [ "$NEW_ALPHA" != "$ALPHA" ]; then
        ok "3 unexpected termination: new alpha pid=$NEW_ALPHA"
    else
        bad "3 unexpected termination: no new alpha process"
    fi
else
    bad "3 unexpected termination: alpha pid not found"
fi

# 4. Restart limit: gamma crashes every 4 s -> 3 restarts -> GIVE UP.
check "4 restart limit: gamma gave up" "GIVE UP gamma" 25
sleep 5   # longer than gamma's crash period: no 4th restart may follow
n=$(grep -c "RESTART gamma" "$LOG")
if [ "$n" -eq 3 ]; then ok "4 restart limit: exactly 3 restarts"; else bad "4 restart limit: $n restarts (want 3)"; fi
check_no_zombies "4 no zombie children while running"

# 5. Status report on SIGUSR1; the watchdog keeps running.
kill -USR1 "$WD"
check "5 status report: header" "STATUS REPORT" 3
check "5 status report: alpha RUNNING"  "alpha +pid=[0-9]+ +state=RUNNING" 3
check "5 status report: gamma GAVE_UP"  "gamma +pid=[0-9]+ +state=GAVE_UP" 3
if kill -0 "$WD" 2>/dev/null; then ok "5 watchdog still alive after SIGUSR1"; else bad "5 watchdog died on SIGUSR1"; fi

# 6. Graceful shutdown on SIGTERM.
kill -TERM "$WD"
wait_exit "6 graceful shutdown"
check        "6 graceful shutdown: requested" "SHUTDOWN requested"
check        "6 graceful shutdown: exit"      "WATCHDOG EXIT"
check_absent "6 graceful shutdown: no SIGKILL needed" "SHUTDOWN timeout"
if awk '/SHUTDOWN requested/ {s = 1} s && /RESTART/ {found = 1} END {exit !found}' "$LOG"; then
    bad "6 graceful shutdown: restart after shutdown request"
else
    ok "6 graceful shutdown: no restarts after shutdown request"
fi
check_no_workers "6 graceful shutdown: no children left"

if [ "${SAVE_SAMPLE:-0}" = "1" ]; then
    mkdir -p docs && cp "$LOG" docs/sample_watchdog.log
    echo "     (saved run 1 log to docs/sample_watchdog.log)"
fi

# ---------------------------------------------------------------- run 2
echo "== run 2: SIGINT (same path as Ctrl+C)"
start_watchdog env
check "7 SIGINT: children started" "START gamma" 3
kill -INT "$WD"
wait_exit "7 SIGINT"
check "7 SIGINT: shutdown requested" "SHUTDOWN requested"
check "7 SIGINT: watchdog exit"      "WATCHDOG EXIT"
check_no_workers "7 SIGINT: no children left"

# ---------------------------------------------------------------- run 3
echo "== run 3: stubborn workers ignore SIGTERM -> SIGKILL after timeout"
start_watchdog env WORKER_STUBBORN=1
check "8 stubborn: children started" "START alpha" 3
kill -TERM "$WD"
wait_exit "8 stubborn"
check "8 stubborn: SIGKILL after timeout" "SHUTDOWN timeout.*SIGKILL"
check "8 stubborn: watchdog exit"         "WATCHDOG EXIT"
check_no_workers "8 stubborn: no children left"

echo
echo "passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
