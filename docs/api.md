# REST API Reference

Every De-Sentry node exposes a lightweight, dependency-free HTTP/1.1 REST interface on its local port (default: data nodes `7701`, `7702`, `7703`; supervisor `7700`).

---

## Data Plane Endpoints

All data plane endpoints are peer-to-peer. Writes are processed locally, broadcast to gossip peers, merged via conflict-free replicated data types (CRDTs), and recorded in an Ed25519-signed SHA-256 hash-chained ledger.

### Document Storage & CRUD

| Method | Endpoint | Description |
|---|---|---|
| `PUT` | `/db/:collection/:key` | Upsert document (JSON body). Merged via LWW-ORSet CRDT |
| `GET` | `/db/:collection/:key` | Fetch current convergent state of a document |
| `DELETE` | `/db/:collection/:key` | Mark document as deleted (tombstone with HLC timestamp) |
| `GET` | `/db/:collection` | Scan or list documents within a collection |

### Collections, Schemas & ACLs

| Method | Endpoint | Description |
|---|---|---|
| `GET` | `/_collections` | List all collections registered on the local node |
| `GET` | `/_collection/:collection` | Inspect collection metadata (storage engine, ACL, placement) |
| `PUT` | `/_collection/:collection/engine` | Bind or switch collection storage engine (`kv`, `ts_rollup`, etc.) |
| `PUT` | `/_collection/:collection/acl` | Set access control: `{owner_node, private, readers[], parent}` |
| `PUT` | `/_collection/:collection/placement` | Configure replication factor and node placement policy |
| `PUT` | `/_schema/:collection` | Set JSON Schema validation rules |
| `GET` | `/_schema/:collection` | Retrieve active schema validation definition |

### Node Identity, Peer Mesh & Topology

| Method | Endpoint | Description |
|---|---|---|
| `GET` | `/_status` | Node identity, public key, uptime, peer count, at-rest encryption posture |
| `GET` | `/_ping` | Storage-independent process liveness probe; returns `{ "ok": true }` |
| `GET` | `/_peers` | List discovered, authenticated, and disconnected P2P peers |
| `GET` | `/_brain` | Compact health snapshot: signed ledger tip, collection checksums, peers |
| `GET` | `/_placement/:collection/:key` | Consistent-hash ring replica subset and displaced owner status |
| `GET` | `/_quota` | Quota allocation and per-engine disk usage metrics |
| `GET` | `/_changes?since=` | Long-polling change feed (returns `truncated` on gap) |

### Cryptographic Ledger & Audit

| Method | Endpoint | Description |
|---|---|---|
| `GET` | `/_ledger/tip` | Current SHA-256 hash-chain tip and Ed25519 signature |
| `GET` | `/_ledger/entries?from=&to=` | Paginated audit ledger entries for verification or replay |
| `POST` | `/_ledger/verify` | Execute full cryptographic verification of local ledger chain |
| `POST` | `/_verify` | Compare and verify remote peer ledger tip against local tip |
| `POST` | `/_checkpoint` | Quorum-gated garbage collection of ledger entries prior to checkpoint |

### Liveness and detail endpoints

The desktop app uses `GET /_ping` as the authoritative process liveness check.
It does not read storage, catalogs, quotas, or checksum state, so a running
node remains reachable while a storage-heavy endpoint is busy or temporarily
blocked on a lock. The app treats `/_status`, `/_brain`, and `/_quota` as
best-effort detail snapshots; failure of one of those endpoints does not by
itself mark the node offline.

`GET /_ping` returns HTTP `200` with a minimal JSON body:

```json
{ "ok": true }
```

Peer entries include `handshake_proven` and `suspicion`. Discovery records and
bootstrap placeholders can appear in `/_peers` before an authenticated
connection exists, or after that address has gone dead. A peer counts as live
for desktop convergence only when `handshake_proven` is `true` and
`suspicion` is `healthy`; a non-empty peer list alone does not mean that
replication is available.

### Transit Store (Offline Handoff)

| Method | Endpoint | Description |
|---|---|---|
| `GET` | `/_transit` | Inspect pending handoff records held for temporarily offline peers |
| `POST` | `/_transit/claim` | Claim pending handoffs when owner node reconnects |
| `POST` | `/_transit/expire` | Trigger TTL sweep on expired transit documents |

### Specialized Engines

| Method | Endpoint | Description |
|---|---|---|
| `POST` | `/_search/vector/:collection` | Execute approximate k-NN nearest-neighbor search |
| `GET` | `/_ts/:collection/rollups` | Aggregate time-series metric rollups across window intervals |
| `GET` | `/_graph/:collection/:key` | Query vertex adjacency list and edge relations |
| `GET` | `/_engines` | List compiled storage engines available in this binary |

---

## Supervisor Plane Endpoints (`/_supervisor/*`)

Supervisor nodes run loopback-only and manage local host hardware discovery, process supervision, and key management. **Supervisors are never on the data path.**

| Method | Endpoint | Description |
|---|---|---|
| `GET` | `/_supervisor/scan` | Hardware volume and storage layout discovery |
| `GET` | `/_supervisor/mounts` | Active filesystem mount points and fitness score |
| `GET` | `/_supervisor/inspect` | Process supervision metrics and daemon status |
| `GET` | `/_supervisor/nodes` | List managed `desentryd` instances and states |
| `POST` | `/_supervisor/nodes/transition` | Transition node state (`discovered`, `provisioned`, `running`, etc.) |
| `POST` | `/_supervisor/recovery-key-exported` | Mark that node disaster recovery key was secured |
| `GET` | `/_supervisor/manifest` | Cluster configuration manifest |
| `GET` | `/_supervisor/topology` | Mesh connection graph and link latencies |
| `POST` | `/_supervisor/pairing` | Generate or verify QR peer pairing tokens |
