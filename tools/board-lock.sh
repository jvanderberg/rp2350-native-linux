#!/usr/bin/env bash
# One board, several sessions: take turns.
#   board-lock.sh acquire OWNER [WAIT_S]   waits (default 3600 s) for the lock
#   board-lock.sh release OWNER
#   board-lock.sh status
# The lock is a directory (mkdir is atomic); a lock older than 45 minutes is
# considered abandoned and is taken over. Hold it only for a test session
# (load + run + read results), not while building.
set -euo pipefail
L=/tmp/rp2350-board.lock
cmd=${1:-status}; owner=${2:-}
age() { echo $(( $(date +%s) - $(stat -f %m "$L" 2>/dev/null || stat -c %Y "$L") )); }
case "$cmd" in
  acquire)
    [ -n "$owner" ] || { echo "usage: $0 acquire OWNER [WAIT_S]" >&2; exit 2; }
    deadline=$(( $(date +%s) + ${3:-3600} ))
    until mkdir "$L" 2>/dev/null; do
      if [ -d "$L" ] && [ "$(age)" -gt 2700 ]; then
        echo "board-lock: taking over a stale lock from $(cat "$L/owner" 2>/dev/null)" >&2
        rm -rf "$L"; continue
      fi
      [ "$(date +%s)" -lt "$deadline" ] || { echo "board-lock: timed out; held by $(cat "$L/owner" 2>/dev/null)" >&2; exit 1; }
      sleep 15
    done
    echo "$owner" > "$L/owner"; echo "board-lock: acquired by $owner";;
  release)
    if [ "$(cat "$L/owner" 2>/dev/null)" = "$owner" ]; then rm -rf "$L"; echo "board-lock: released"; else echo "board-lock: not held by $owner" >&2; exit 1; fi;;
  status)
    if [ -d "$L" ]; then echo "held by $(cat "$L/owner" 2>/dev/null) for $(age) s"; else echo free; fi;;
esac
