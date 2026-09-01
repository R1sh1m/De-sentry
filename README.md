# De-Sentry

A decentralized, fully peer-to-peer database engine. No client-server split:
every node is simultaneously a storage engine, a REST API server for local
applications, and a P2P peer that gossips writes to every other node. There
is no coordinator, no leader, and no single point of failure by design.

Built in C++17 from first principles, the way real database engines are
built: a hand-written paged storage layer with a write-ahead log and a
disk-backed B+Tree (not a wrapper around SQLite/RocksDB/LMDB), a CRDT
document model for conflict-free multi-writer replication, and a from-scratch
encrypted P2P transport — no networking or database framework dependencies.
The only third-party dependency is OpenSSL (for Ed25519/X25519/AES-GCM).

> **Course project** — validated at 3 nodes running locally in a full mesh.
> See [Project Statement](Project_Statement/project_statement.md) for the
> original brief and scope.

---

## Tech Stack

| Layer | Technology | Role |
|---|---|---|
| **Core Engine** | C++17 | Storage engine (paged I/O, WAL, B+Tree), CRDT layer, P2P networking |
| **Crypto** | OpenSSL (EVP) | Ed25519 signing, X25519 ECDH, AES-256-GCM transport, SHA-256 hash chains |
| **Python Client** | Python 3 (stdlib only) | Zero-dependency HTTP client for AI agent bindings |
| **Dashboard** | Single static HTML file | Browser-based node status viewer (no build step, no framework) |
| **Build** | CMake ≥ 3.16 | C++ compilation |
| **Containers** | Docker + Docker Compose | Containerized 3-node cluster with integration tests |

---

## Quickstart

### Build from source

> Prerequisites: CMake ≥ 3.16, a C++17 compiler (GCC/Clang), OpenSSL dev headers

```bash
# Build
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ..
make -j"$(nproc)"

# Run tests (4 suites: crdt, crypto, storage, network)
ctest --output-on-failure
```

### Run a 3-node cluster

```bash
cd ..
./scripts/run_cluster.sh 3

# Write on node 0, read from nodes 1 and 2:
curl -s -X PUT http://127.0.0.1:7701/db/users/u1 -d '{"name":"Asha","role":"admin"}'
sleep 1
curl -s http://127.0.0.1:7702/db/users/u1   # -> {"name":"Asha","role":"admin"}
curl -s http://127.0.0.1:7703/db/users/u1   # -> {"name":"Asha","role":"admin"}

./scripts/stop_cluster.sh
```

### Or use Docker Compose

```bash
docker compose up --build          # build image, start 3 nodes + run integration tests
docker compose up node-a node-b node-c   # just the cluster, for manual play
docker compose run tester          # re-run integration tests against a running cluster
```

### CLI

```bash
./build/desentry_cli --api 127.0.0.1:7701 put users u1 '{"name":"Asha"}'
./build/desentry_cli --api 127.0.0.1:7702 get users u1
./build/desentry_cli --api 127.0.0.1:7701 status
```

### Python client

```python
from clients.python.desentry_client import DesentryClient
node = DesentryClient("http://127.0.0.1:7701")
node.put("users", "u1", {"name": "Asha"})
print(node.get("users", "u1"))
print(node.verify_ledger())   # {"verified": true, "entries_checked": N}
```

### Dashboard

Open `tools/dashboard.html` directly in a browser (no build step, no server)
and point it at a running node's API address to see live node status, the
signed ledger tip, per-collection checksums, known peers, and a scrolling
ledger-entry table with one-click ledger verification.

---

## REST API

Every node serves this API on its local HTTP port (default 7701/7702/7703):

| Method | Path | Purpose |
|--------|------|---------|
| PUT | `/db/:collection/:key` | Upsert a document (JSON body) |
| GET | `/db/:collection/:key` | Fetch a document |
| DELETE | `/db/:collection/:key` | Delete a document (tombstone) |
| GET | `/db/:collection` | List/scan a collection |
| PUT | `/_schema/:collection` | Set a JSON-Schema-subset validator |
| GET | `/_schema/:collection` | Read a collection's schema |
| GET | `/_collections` | List known collections |
| GET | `/_peers` | List known P2P peers |
| GET | `/_status` | Node identity, uptime, peer count |
| GET | `/_brain` | Compact snapshot: signed ledger tip, per-collection checksums, peers |
| GET | `/_ledger/tip` | Current hash-chain tip + Ed25519 signature |
| GET | `/_ledger/entries?from=&to=` | Bounded page of ledger entries for replay/audit |
| POST | `/_ledger/verify` | Full hash-chain re-verification |

Every write is broadcast to connected peers and merged via CRDT so all nodes
converge without coordination. Every write is also appended to a SHA-256
hash-chained, Ed25519-signed audit ledger — a tamper-evident history of every
mutation.

---

## Architecture

```
                    ┌──────────────────────────────────────────────────────┐
                    │                   desentryd (one peer)               │
                    │                                                      │
   local apps      │  ┌────────────┐       ┌────────────────────────────┐ │
   (curl, your  ───┼─▶│  API Layer │       │       Engine Core          │ │
   app's HTTP      │  │ HTTP/1.1   │◀─────▶│ (storage + CRDT + catalog) │ │
   client)         │  │ REST + WS  │       └────────────┬───────────────┘ │
                    │  └────────────┘                    │                 │
                    │                                    ▼                 │
                    │                     ┌──────────────────────────┐    │
                    │                     │     Storage Engine        │    │
                    │                     │ DiskMgr / BufferPool /    │    │
                    │                     │ WAL / B+Tree / Catalog    │    │
                    │                     │     (local *.dsf file)    │    │
                    │                     └──────────────────────────┘    │
                    │                                    ▲                 │
                    │  ┌─────────────────────────────────┘                 │
                    │  │                                                   │
                    │  ▼                                                   │
                    │ ┌────────────────────────────────────────────────┐  │
                    │ │               Network Layer                     │  │
                    │ │ Identity (Ed25519+X25519) · Secure channel       │  │
                    │ │ TCP wire protocol · UDP LAN discovery           │  │
                    │ │ Gossip / anti-entropy replicator                │  │
                    │ └────────────────────────────────────────────────┘  │
                    └────────────────────────┬─────────────────────────────┘
                                              │  TCP (encrypted, signed frames)
                                              ▼
                                  other peers, symmetric, same shape
```

For the full design document — goals/non-goals, prior-art comparison, CRDT
consistency model, storage engine internals, network protocol, security model,
hash-chained audit ledger, and roadmap — see
[`docs/architecture.md`](docs/architecture.md).

For a comparison against an alternate consortium/routing design and what was
adopted from it, see [`docs/comparison.md`](docs/comparison.md).

---

## Directory Structure

```
De-Sentry/
├── README.md                       ← You are here
├── CMakeLists.txt                  ← Build system
├── Dockerfile                      ← Container image build
├── docker-compose.yml              ← 3-node cluster + integration tests
├── docker-entrypoint.sh            ← Container entrypoint (renders config from env)
│
├── include/desentry/               ← Public headers, organized by layer
│   └── (common, storage, crdt, security, net, engine, api)
├── src/                            ← Implementation (mirrors include/)
│   ├── api/                        ← HTTP/1.1 REST server
│   ├── common/                     ← Shared utilities
│   ├── crdt/                       ← CRDT types + HLC
│   ├── engine/                     ← Node engine (ties everything together)
│   ├── net/                        ← P2P networking, gossip, discovery
│   ├── security/                   ← Crypto (Ed25519, X25519, AES-GCM)
│   └── storage/                    ← Disk manager, buffer pool, WAL, B+Tree
├── apps/
│   ├── desentry_node/              ← desentryd daemon entrypoint
│   └── desentry_cli/               ← Thin CLI client
├── tests/                          ← Assert-based test suites (crdt, crypto, storage, network)
│   └── integration/                ← Python integration test for Docker cluster
├── clients/python/                 ← Zero-dependency Python client
├── tools/dashboard.html            ← Browser dashboard (no build step)
├── config/                         ← Example node config (JSON)
│
├── docs/
│   ├── architecture.md             ← Full system design spec
│   ├── comparison.md               ← Comparison against alternate design
│   └── original_design/            ← Original consortium/routing design docs (historical)
├── scripts/
│   ├── run_cluster.sh / .ps1       ← Launch a local N-node cluster
│   ├── stop_cluster.sh / .ps1      ← Stop the cluster
│   ├── dsync.py / .ps1 / .sh       ← Git collaboration tooling
│   └── README.md                   ← Collaboration workflow guide
│
├── Project_Statement/              ← Original course project brief
├── Research_Docs/                  ← Annotated bibliography and research
├── status.md                       ← MVP build status log
└── .gitignore
```

---

## Research Foundation

Key papers underpinning the design — full annotated bibliography in
[`Research_Docs/`](Research_Docs/):

- **Dynamo** (DeCandia et al., SOSP 2007) — node-owned data + last-write-wins + gossip
- **Distributed Snapshots** (Chandy & Lamport, 1985) — brain file = consistent global snapshot
- **Raft** (Ongaro & Ousterhout, 2014) — consensus baseline we consciously simplify
- **CAP Theorem** (Brewer, 2000; Gilbert & Lynch, 2002) — our AP trade-off justification
- **CRDTs** (Preguiça, 2018) — theoretical basis for conflict-free node ownership

---

## Status

MVP scope, built for a course project deadline as a local multi-node
simulation (real OS processes, real sockets, one host). All four test suites
pass with live assertions, and the full stack has been verified end-to-end
with real compiled binaries. See [`status.md`](status.md) for the full build
log, bugs found, and known limitations.

---

*De-Sentry — Decentralized Sentinel for AI Agent Data*
