#!/bin/bash
# Launch gtopt via run_gtopt with an automatic stall-watchdog.
#
# When the SDDPWorkPool monitor logs "no progress for N intervals" with N >= 2
# the watchdog snapshots all gtopt thread stacks via gdb -p and writes them to
# /tmp/gtopt_stall_<timestamp>.txt for offline analysis.  It also captures one
# follow-up snapshot 60 s later, then continues (does NOT kill gtopt).
#
# Requires kernel.yama.ptrace_scope=0 for live gdb attach without sudo:
#   sudo sysctl -w kernel.yama.ptrace_scope=0
#
# Usage:  bash run_with_stall_watchdog.sh <case-dir>
# Default case-dir: gtopt_iplp_plain
set -euo pipefail

CASE="${1:-gtopt_iplp_plain}"
LOG_DIR="$(pwd)/output/logs"
mkdir -p "$LOG_DIR"

# Snapshot existing gtopt_*.log files so the watchdog only tails the NEW one
# that this run produces.
existing=$(ls -1 "$LOG_DIR"/gtopt_*.log 2>/dev/null | sort -u || true)

# Start run_gtopt in the foreground; the watchdog runs in the background and
# follows whichever new gtopt_*.log appears.
(
  # Wait up to 60 s for a new gtopt_*.log to appear.
  new_log=""
  for _ in $(seq 1 600); do
    sleep 0.1
    current=$(ls -1 "$LOG_DIR"/gtopt_*.log 2>/dev/null | sort -u || true)
    new_log=$(comm -13 <(echo "$existing") <(echo "$current") | head -n1)
    if [[ -n "$new_log" ]]; then
      break
    fi
  done
  if [[ -z "$new_log" ]]; then
    echo "[watchdog] no new gtopt log appeared within 60s — exiting" >&2
    exit 0
  fi
  echo "[watchdog] tailing $new_log" >&2

  last_dump_at=0
  # Tail the log; trigger a stack snapshot the first time we see "no progress
  # for >= 2 intervals" and at most once every 5 min thereafter.
  tail -F -n 0 "$new_log" 2>/dev/null \
    | grep --line-buffered -E 'no progress for [0-9]+ interval' \
    | while read -r line; do
      now=$(date +%s)
      if (( now - last_dump_at < 300 )); then
        continue
      fi
      pid=$(pgrep -x gtopt | head -n1 || true)
      if [[ -z "$pid" ]]; then
        continue
      fi
      ts=$(date +%Y%m%d-%H%M%S)
      out="/tmp/gtopt_stall_${ts}.txt"
      echo "[watchdog] STALL detected at $(date -Is): $line" | tee -a "$out"
      echo "[watchdog] dumping thread stacks for pid=$pid -> $out" >&2
      timeout 60 gdb -p "$pid" -batch -nx \
        -ex 'set pagination off' \
        -ex 'set print frame-arguments none' \
        -ex 'set confirm off' \
        -ex 'thread apply all bt 50' \
        -ex 'detach' -ex 'quit' \
        >> "$out" 2>&1 || \
        echo "[watchdog] gdb failed (ptrace_scope?)" >> "$out"
      last_dump_at=$now
    done
) &
watchdog_pid=$!
trap 'kill $watchdog_pid 2>/dev/null || true' EXIT

# Run the actual job.  Foreground; user sees the TUI.
run_gtopt "$CASE"
