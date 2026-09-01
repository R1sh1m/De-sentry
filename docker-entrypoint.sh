#!/usr/bin/env bash
# Entrypoint for the De-Sentry container image.
#
# Renders a node.json config from environment variables (defaulting to the
# values in config/node.example.json) so a single image can run any node in a
# cluster. This is what makes docker-compose able to spin up N peers that
# already know how to reach each other: each container passes its siblings'
# "host:p2p_port" as DESENTRY_BOOTSTRAP_PEERS.
#
# Env vars (all optional):
#   DESENTRY_NODE_NAME          human-friendly label for logs
#   DESENTRY_DATA_DIR           where the engine stores its .dsf/.wal (default /data)
#   DESENTRY_API_BIND_ADDR      API bind address (default 0.0.0.0 in-container)
#   DESENTRY_API_PORT           API port (default 7701)
#   DESENTRY_P2P_BIND_ADDR      P2P bind address (default 0.0.0.0)
#   DESENTRY_P2P_PORT           P2P port (default 7801)
#   DESENTRY_DISCOVERY_ENABLED  "true"/"false" UDP LAN discovery (default false in cluster)
#   DESENTRY_DISCOVERY_PORT     UDP discovery port (default 7901)
#   DESENTRY_GOSSIP_INTERVAL_MS gossip anti-entropy interval (default 2000)
#   DESENTRY_BOOTSTRAP_PEERS    comma-separated "host:p2p_port" peers (default empty)
#   DESENTRY_BUFFER_POOL_PAGES  buffer pool size in 4KiB pages (default 1024)

set -euo pipefail

DATA_DIR="${DESENTRY_DATA_DIR:-/data}"
NODE_NAME="${DESENTRY_NODE_NAME:-desentry-node}"
API_BIND_ADDR="${DESENTRY_API_BIND_ADDR:-0.0.0.0}"
API_PORT="${DESENTRY_API_PORT:-7701}"
P2P_BIND_ADDR="${DESENTRY_P2P_BIND_ADDR:-0.0.0.0}"
P2P_PORT="${DESENTRY_P2P_PORT:-7801}"
DISCOVERY_ENABLED="${DESENTRY_DISCOVERY_ENABLED:-false}"
DISCOVERY_PORT="${DESENTRY_DISCOVERY_PORT:-7901}"
GOSSIP_INTERVAL_MS="${DESENTRY_GOSSIP_INTERVAL_MS:-2000}"
BUFFER_POOL_PAGES="${DESENTRY_BUFFER_POOL_PAGES:-1024}"

# Translate "true"/"false" strings into JSON booleans.
if [ "${DISCOVERY_ENABLED}" = "true" ] || [ "${DISCOVERY_ENABLED}" = "1" ]; then
  DISCOVERY_JSON="true"
else
  DISCOVERY_JSON="false"
fi

# Render the bootstrap_peers JSON array from a comma-separated list.
BOOTSTRAP_JSON="[]"
if [ -n "${DESENTRY_BOOTSTRAP_PEERS:-}" ]; then
  BOOTSTRAP_JSON="["
  IFS=',' read -ra PEERS <<< "${DESENTRY_BOOTSTRAP_PEERS}"
  for peer in "${PEERS[@]}"; do
    # Trim surrounding whitespace.
    peer="$(echo -n "$peer" | xargs)"
    [ -z "$peer" ] && continue
    BOOTSTRAP_JSON="${BOOTSTRAP_JSON}\"${peer}\","
  done
  BOOTSTRAP_JSON="${BOOTSTRAP_JSON%,}]"
fi

CONFIG_PATH="${DATA_DIR}/node.json"
mkdir -p "${DATA_DIR}"

cat > "${CONFIG_PATH}" <<EOF
{
  "node_name": "${NODE_NAME}",
  "data_dir": "${DATA_DIR}",
  "api_bind_addr": "${API_BIND_ADDR}",
  "api_port": ${API_PORT},
  "p2p_bind_addr": "${P2P_BIND_ADDR}",
  "p2p_port": ${P2P_PORT},
  "discovery_enabled": ${DISCOVERY_JSON},
  "discovery_port": ${DISCOVERY_PORT},
  "discovery_interval_ms": 2000,
  "bootstrap_peers": ${BOOTSTRAP_JSON},
  "gossip_interval_ms": ${GOSSIP_INTERVAL_MS},
  "buffer_pool_pages": ${BUFFER_POOL_PAGES}
}
EOF

echo "desentry: rendered config -> ${CONFIG_PATH}"
echo "desentry: node_name=${NODE_NAME} api=${API_BIND_ADDR}:${API_PORT} p2p=${P2P_BIND_ADDR}:${P2P_PORT} bootstrap=${BOOTSTRAP_JSON}"

exec "$@" --config "${CONFIG_PATH}"
