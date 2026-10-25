#!/usr/bin/env bash
# Stops every node started by run_cluster.sh (SIGTERM -> clean checkpointed shutdown).
set -euo pipefail
# Honour TMPDIR so this works where /tmp is not writable, and two people on
# a shared machine do not collide.
RUN_DIR="${DESENTRY_RUN_DIR:-${TMPDIR:-/tmp}/desentry_cluster}"

if [ ! -d "$RUN_DIR" ]; then
  echo "no cluster running (no $RUN_DIR)"
  exit 0
fi

for pid_file in "$RUN_DIR"/*.pid; do
  [ -f "$pid_file" ] || continue
  pid="$(cat "$pid_file")"
  if kill -0 "$pid" 2>/dev/null; then
    kill -TERM "$pid"
    echo "stopped pid $pid ($(basename "$pid_file"))"
  fi
  rm -f "$pid_file"
done
