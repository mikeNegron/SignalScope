#!/usr/bin/env bash
# bench_dsp.sh — run the DSP microbench and emit a self-describing
# JSON line capturing host/CPU/git metadata alongside the bench output.
#
# Usage:
#   ./scripts/bench_dsp.sh                     # 5000 iters (default)
#   ./scripts/bench_dsp.sh --iters=1000        # custom iter count via flag
#   BENCH_ITERS=10000 ./scripts/bench_dsp.sh   # custom iter count via env
#   ./scripts/bench_dsp.sh >> bench_log.jsonl  # append to a JSONL log
#
# Output schema (one line per invocation, JSONL-compatible):
#   {"ts","sha","dirty","host","cpu","pinning","governor","iters",
#    "real":{...}[,"complex":{...}]}
#
# The "real" (and "complex" when present) blocks are the bench binary's
# exact stdout, embedded as JSON sub-objects. Consume with:
#   jq -c '.' bench_log.jsonl     # pretty-print / validate
#   jq '.real."4096"' bench_log.jsonl   # extract a specific size
#
# Requirements:
#   - jq must be installed (apt install jq).
#   - The bench binary must be built (npm run test:backend).
#
# Optional optimizations (Linux):
#   - taskset:  pin to CPU 0 for stability.
#   - cpupower: requires root; sets governor to "performance" for the run.
# When unavailable, the script proceeds and reflects the skip in the output
# fields ("pinning":"skipped", "governor":"skipped").

set -euo pipefail

BENCH_BIN="./build/tests/bench_dsp_pipeline"
DEFAULT_ITERS=5000

# --- Argument / env parsing ---------------------------------------------------
ITERS="${BENCH_ITERS:-$DEFAULT_ITERS}"
for arg in "$@"; do
    case "$arg" in
        --iters=*)
            ITERS="${arg#--iters=}"
            ;;
        -h|--help)
            sed -n '2,28p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "Unknown argument: $arg" >&2
            echo "Usage: $0 [--iters=N]" >&2
            exit 2
            ;;
    esac
done

if ! [[ "$ITERS" =~ ^[0-9]+$ ]] || [ "$ITERS" -eq 0 ]; then
    echo "Error: iter count must be a positive integer (got: $ITERS)" >&2
    exit 2
fi

# --- Tool / binary checks -----------------------------------------------------
if ! command -v jq >/dev/null 2>&1; then
    echo "Error: jq is required but not installed." >&2
    echo "       Install with: sudo apt install jq" >&2
    exit 1
fi

if [ ! -x "$BENCH_BIN" ]; then
    echo "Error: bench binary not found or not executable at: $BENCH_BIN" >&2
    echo "       Build it with: npm run test:backend" >&2
    exit 1
fi

# --- Metadata capture ---------------------------------------------------------
TS="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

if SHA="$(git rev-parse HEAD 2>/dev/null)"; then
    if git diff --quiet 2>/dev/null; then
        DIRTY="clean"
    else
        DIRTY="dirty"
    fi
else
    SHA="unknown"
    DIRTY="unknown"
fi

HOST="$(hostname 2>/dev/null || echo unknown)"

# CPU model: try Linux lscpu, then macOS sysctl, then unknown.
CPU="unknown"
if command -v lscpu >/dev/null 2>&1; then
    # Grab the "Model name:" line; collapse whitespace.
    CPU_LINE="$(lscpu 2>/dev/null | grep -E '^Model name:' | head -n1 || true)"
    if [ -n "$CPU_LINE" ]; then
        CPU="$(echo "$CPU_LINE" | sed -E 's/^Model name:[[:space:]]*//' | tr -s ' ')"
    fi
elif command -v sysctl >/dev/null 2>&1; then
    CPU="$(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown)"
fi
[ -z "$CPU" ] && CPU="unknown"

# --- Optional pinning / governor ---------------------------------------------
PINNING="skipped"
RUN_PREFIX=()
if command -v taskset >/dev/null 2>&1; then
    RUN_PREFIX=(taskset -c 0)
    PINNING="taskset"
fi

GOVERNOR="skipped"
GOVERNOR_RESTORE=""
if [ "$(id -u)" -eq 0 ] && command -v cpupower >/dev/null 2>&1; then
    # Record the current governor on CPU 0 so we can restore it.
    if PREV_GOV="$(cpupower frequency-info -p 2>/dev/null | awk '/governor/ {print $3; exit}' | tr -d '"')"; then
        if cpupower frequency-set --governor performance >/dev/null 2>&1; then
            GOVERNOR="performance"
            GOVERNOR_RESTORE="$PREV_GOV"
        fi
    fi
fi

restore_governor() {
    if [ -n "$GOVERNOR_RESTORE" ] && command -v cpupower >/dev/null 2>&1; then
        cpupower frequency-set --governor "$GOVERNOR_RESTORE" >/dev/null 2>&1 || true
    fi
}
trap restore_governor EXIT

# --- Run the bench ------------------------------------------------------------
# Capture stdout (JSON); let stderr pass through so failures are visible.
BENCH_JSON="$("${RUN_PREFIX[@]}" "$BENCH_BIN" "$ITERS")"

# --- Compose the combined JSONL line -----------------------------------------
# Use jq to merge metadata with the bench's JSON output. The bench already
# includes "iters" and the per-size blocks ("real", optionally "complex");
# we add the metadata fields and re-order so metadata appears first.
echo "$BENCH_JSON" | jq -c \
    --arg ts "$TS" \
    --arg sha "$SHA" \
    --arg dirty "$DIRTY" \
    --arg host "$HOST" \
    --arg cpu "$CPU" \
    --arg pinning "$PINNING" \
    --arg governor "$GOVERNOR" \
    '{ts: $ts, sha: $sha, dirty: $dirty, host: $host, cpu: $cpu,
      pinning: $pinning, governor: $governor} + .'
