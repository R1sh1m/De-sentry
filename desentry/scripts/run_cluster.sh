#!/usr/bin/env bash
# Launches a local N-node De-Sentry cluster for demo/manual testing:
# N separate `desentryd` processes, each with its own data directory,
# API port, and P2P port, bootstrap-configured to know about each other.
# This is the "local multi-node simulation" the project targets for a
# course demo -- real peer processes, real sockets, just all on one host.
#
# Usage: scripts/run_cluster.sh [num_nodes]   (default: 3)
#
# Each node's API is reachable at http://127.0.0.1:<7701 + i>
# Logs go to /tmp/desentry_cluster/node<i>.log
# Stop everything with scripts/stop_cluster.sh

set -euo pipefail

NUM_NODES="${1:-3}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT_DIR/build/desentryd"
RUN_DIR="/tmp/desentry_cluster"

if [ ! -x "$BIN" ]; then
  echo "error: $BIN not found -- build first (see README.md: cmake + make in build/)" >&2
  exit 1
fi

mkdir -p "$RUN_DIR"
rm -f "$RUN_DIR"/*.pid

# Build the bootstrap peer list every node uses to find every other node.
BOOTSTRAP_JSON="["
for ((i = 0; i < NUM_NODES; i++)); do
  p2p_port=$((7801 + i))
  BOOTSTRAP_JSON="${BOOTSTRAP_JSON}\"127.0.0.1:${p2p_port}\","
done
BOOTSTRAP_JSON="${BOOTSTRAP_JSON%,}]"

for ((i = 0; i < NUM_NODES; i++)); do
  api_port=$((7701 + i))
  p2p_port=$((7801 + i))
  disc_port=$((7901))  # shared discovery port -- all nodes also try UDP broadcast
  data_dir="$RUN_DIR/node${i}/data"
  config_path="$RUN_DIR/node${i}/node.json"
  mkdir -p "$data_dir"

  # Each node's own bootstrap list excludes itself.
  node_bootstrap="["
  for ((j = 0; j < NUM_NODES; j++)); do
    if [ "$j" -ne "$i" ]; then
      node_bootstrap="${node_bootstrap}\"127.0.0.1:$((7801 + j))\","
    fi
  done
  node_bootstrap="${node_bootstrap%,}]"

  cat > "$config_path" <<EOF
{
  "node_name": "node${i}",
  "data_dir": "${data_dir}",
  "api_bind_addr": "127.0.0.1",
  "api_port": ${api_port},
  "p2p_bind_addr": "0.0.0.0",
  "p2p_port": ${p2p_port},
  "discovery_enabled": true,
  "discovery_port": ${disc_port},
  "discovery_interval_ms": 2000,
  "bootstrap_peers": ${node_bootstrap},
  "gossip_interval_ms": 2000,
  "buffer_pool_pages": 1024
}
EOF

  "$BIN" --config "$config_path" > "$RUN_DIR/node${i}.log" 2>&1 &
  echo $! > "$RUN_DIR/node${i}.pid"
  echo "started node${i}: api=http://127.0.0.1:${api_port}  p2p=127.0.0.1:${p2p_port}  pid=$!"
done

echo ""
echo "cluster of ${NUM_NODES} node(s) is up. Try:"
echo "  ${ROOT_DIR}/build/desentry_cli --api 127.0.0.1:7701 put users u1 '{\"name\":\"Asha\"}'"
echo "  ${ROOT_DIR}/build/desentry_cli --api 127.0.0.1:7702 get users u1"
echo "Stop with: ${ROOT_DIR}/scripts/stop_cluster.sh"
