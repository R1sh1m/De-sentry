#!/usr/bin/env bash
# Launches a local N-node De-Sentry cluster for demo/manual testing:
# N separate `desentryd` processes, each with its own data directory,
# API port, and P2P port, bootstrap-configured to know about each other.
# This is the "local multi-node simulation" the project targets for a
# course demo -- real peer processes, real sockets, just all on one host.
#
# Usage: scripts/run_cluster.sh [num_nodes] [--supervisor] [--engines a,b]
#          num_nodes      how many data nodes (default: 3)
#          --supervisor   also start an app-local supervisor on 7700/7800
#          --engines      comma-separated engine list for every node
#                         (default: kv). Names must be compiled into the
#                         binary -- ask it with GET /_engines.
#
# The desktop app (app/) is the supported way to run nodes; this script is
# for engine work, where starting a mesh from a terminal and reading its
# logs directly is what you actually want.
#
# Each node's API is reachable at http://127.0.0.1:<7701 + i>
# Logs go to $RUN_DIR/node<i>.log
# Stop everything with scripts/stop_cluster.sh

set -euo pipefail

NUM_NODES=3
WITH_SUPERVISOR=0
ENGINES="kv"

while [ $# -gt 0 ]; do
  case "$1" in
    --supervisor) WITH_SUPERVISOR=1; shift ;;
    --engines) ENGINES="$2"; shift 2 ;;
    --engines=*) ENGINES="${1#*=}"; shift ;;
    -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
    *) NUM_NODES="$1"; shift ;;
  esac
done

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT_DIR/build/desentryd"
# Honour TMPDIR so this works where /tmp is not writable, and so two people on
# a shared machine do not collide.
RUN_DIR="${DESENTRY_RUN_DIR:-${TMPDIR:-/tmp}/desentry_cluster}"

if [ ! -x "$BIN" ]; then
  echo "error: $BIN not found. Build first:" >&2
  echo "  cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo" >&2
  echo "  cmake --build build -j" >&2
  exit 1
fi

# The engine list has to be JSON, and it is built once here rather than quoted
# into every heredoc below.
ENGINES_JSON="[$(echo "$ENGINES" | awk -F, '{for (i=1;i<=NF;i++) printf "%s\"%s\"", (i>1?",":""), $i}')]"
DEFAULT_ENGINE="$(echo "$ENGINES" | cut -d, -f1)"

"$ROOT_DIR/scripts/stop_cluster.sh" 2>/dev/null || true
mkdir -p "$RUN_DIR"
rm -f "$RUN_DIR"/*.pid

start_node() {
  local name="$1" api_port="$2" p2p_port="$3" supervisor="$4" bootstrap="$5"
  local data_dir="$RUN_DIR/$name/data"
  local config_path="$RUN_DIR/$name/node.json"
  mkdir -p "$data_dir"

  # A supervisor binds everything to loopback and does not advertise itself.
  # NodeConfig::Validate() enforces the API side of that and refuses to start
  # otherwise, so writing anything else here would only produce a node that
  # will not run.
  local p2p_bind="0.0.0.0"
  local discovery="true"
  if [ "$supervisor" = "true" ]; then
    p2p_bind="127.0.0.1"
    discovery="false"
  fi

  cat > "$config_path" <<EOF
{
  "node_name": "${name}",
  "data_dir": "${data_dir}",

  "api_bind_addr": "127.0.0.1",
  "api_port": ${api_port},
  "p2p_bind_addr": "${p2p_bind}",
  "p2p_port": ${p2p_port},

  "discovery_enabled": ${discovery},
  "discovery_port": 7901,
  "discovery_interval_ms": 2000,
  "bootstrap_peers": ${bootstrap},
  "gossip_interval_ms": 2000,
  "buffer_pool_pages": 1024,

  "supervisor": ${supervisor},
  "quota_mb": 0,
  "quota_split": {
    "db_pct": 60,
    "transit_store_pct": 15,
    "cache_hash_pct": 10,
    "ledger_pct": 10,
    "net_buffers_pct": 5
  },
  "engines": ${ENGINES_JSON},
  "default_engine": "${DEFAULT_ENGINE}",
  "replication_factor": 3,
  "transit_ttl_seconds": 604800,
  "retention_days": 0,

  "encrypt_at_rest": false,
  "keychain_ref": "",

  "max_peer_threads": 8,
  "peer_rate_limit_per_sec": 200,
  "peer_rate_burst": 400,
  "mdns_enabled": ${discovery},
  "advertise_hostname": ""
}
EOF

  nohup "$BIN" --config "$config_path" > "$RUN_DIR/${name}.log" 2>&1 &
  echo $! > "$RUN_DIR/${name}.pid"
  printf 'started %-12s api=http://127.0.0.1:%s  p2p=127.0.0.1:%s  pid=%s\n' \
    "$name" "$api_port" "$p2p_port" "$!"
}

# Every data node's bootstrap list is every other data node. The supervisor is
# never in it: it holds no data and is not a replication hop.
data_bootstrap() {
  local self="$1" list="["
  for ((j = 0; j < NUM_NODES; j++)); do
    if [ "$j" -ne "$self" ]; then
      list="${list}\"127.0.0.1:$((7801 + j))\","
    fi
  done
  echo "${list%,}]"
}

for ((i = 0; i < NUM_NODES; i++)); do
  start_node "node${i}" $((7701 + i)) $((7801 + i)) false "$(data_bootstrap "$i")"
done

if [ "$WITH_SUPERVISOR" = "1" ]; then
  # The supervisor still needs to reach the data nodes to collect tips for a
  # checkpoint, so it gets them as bootstrap peers even though no data routes
  # through it.
  sup_bootstrap="["
  for ((j = 0; j < NUM_NODES; j++)); do
    sup_bootstrap="${sup_bootstrap}\"127.0.0.1:$((7801 + j))\","
  done
  start_node "supervisor" 7700 7800 true "${sup_bootstrap%,}]"
fi

echo ""
echo "cluster of ${NUM_NODES} data node(s) is up (engines: ${ENGINES}). Try:"
echo "  ${ROOT_DIR}/build/desentry_cli --api 127.0.0.1:7701 put users u1 '{\"name\":\"Asha\"}'"
echo "  ${ROOT_DIR}/build/desentry_cli --api 127.0.0.1:7702 get users u1"
echo "  curl -s http://127.0.0.1:7701/_brain"
echo "  curl -s http://127.0.0.1:7701/_engines"
if [ "$WITH_SUPERVISOR" = "1" ]; then
  echo "  curl -s -X POST http://127.0.0.1:7700/_checkpoint    # quorum-gated GC"
  echo "  curl -s http://127.0.0.1:7700/_supervisor/topology"
fi
echo ""
echo "Dashboard: open tools/dashboard.html and point it at 127.0.0.1:7701"
echo "Logs:      ${RUN_DIR}/node<i>.log"
echo "Stop with: ${ROOT_DIR}/scripts/stop_cluster.sh"
