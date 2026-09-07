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

v2 adds a desktop control room on top: a Tauri app that is the **single point
of access** to the mesh. It discovers hardware, sizes and provisions nodes,
supervises the `desentryd` processes, and runs in the tray keeping the mesh in
sync in the background. You never hand-edit `node.json` and never launch a
daemon yourself. Everything it needs is onboard — the app is fully functional
with no internet, which is an acceptance test
(`tests/integration/airplane_mode_test.py`), not an aspiration.

> **Course project** — validated at 3 nodes running locally in a full mesh.
> See [Project Statement](Project_Statement/project_statement.md) for the
> original brief and scope. Read [`STATUS.md`](STATUS.md) before trusting any
> claim here: it separates what was executed and observed from what was only
> designed and statically checked.

---

## Tech Stack

| Layer | Technology | Role |
|---|---|---|
| **Core Engine** | C++17 | Storage engine (paged I/O, WAL, B+Tree), storage router, CRDT layer, ledger, P2P networking |
| **Crypto** | OpenSSL (EVP) | Ed25519 signing, X25519 ECDH, AES-256-GCM transport and at-rest, SHA-256 hash chains |
| **Desktop app** | Tauri v2 + TypeScript | Control room: node lifecycle, supervision, tray sync — no JS framework, one runtime dependency |
| **Sidecar** | Rust | Port allocation, config generation, process supervision, OS keychain, notifications |
| **Onboard AI** | ONNX Runtime + all-MiniLM-L6-v2 | Description → engine sizing, offline, with a deterministic keyword fallback |
| **Python Client** | Python 3 (stdlib only) | Zero-dependency HTTP client for AI agent bindings |
| **Dashboard** | Single static HTML file | Browser-based node status viewer (no build step, no framework) |
| **Build** | CMake ≥ 3.16, cargo, npm/bun | C++ engine, Rust sidecar, frontend + installers |
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

# Run tests (9 suites: crdt, crypto, storage, network, router, acl,
#            placement, ledger_v2, quota)
ctest --output-on-failure
```

Optional vendored storage backends are **off by default and never
downloaded** — place their sources under `third_party/` and turn them on
explicitly (`-DDESENTRY_WITH_SQLITE=ON`, `…_SQLITE_VEC`, `…_DUCKDB`,
`…_LMDB`). A running node will tell you what it actually has:
`curl -s http://127.0.0.1:7701/_engines`.

### Build the desktop app

> Prerequisites: the above, plus Rust/cargo and Node ≥ 18 (or bun)

```bash
cd app
npm install
npm run fetch-model      # one-time: bundle the sizing model (build step, not runtime)
npm run build            # typecheck + frontend bundle
npm run tauri:dev        # run the control room
npm run tauri:build      # MSI / .dmg / AppImage with versioned desentryd sidecars
```

### Run a 3-node cluster

The desktop app is the supported way to run nodes. These scripts exist for
engine work, where starting a mesh from a terminal and reading its logs
directly is what you actually want. (Windows: `.\scripts\run_cluster.ps1
-Nodes 3 -Supervisor`.)

```bash
cd ..
./scripts/run_cluster.sh 3 --supervisor --engines kv,ts_rollup

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
| GET | `/_engines` | Which storage engines this binary actually has |
| GET | `/_collection/:collection` | Collection metadata: engine, ACL, placement |
| PUT | `/_collection/:collection/engine` | Choose the storage engine for a collection |
| PUT | `/_collection/:collection/acl` | Set `{owner_node, private, readers[], parent}` |
| PUT | `/_collection/:collection/placement` | Replication factor and placement policy |
| GET | `/_quota` | Quota split and per-engine usage |
| GET | `/_changes?since=` | Long-poll change feed (signals `truncated` on a gap) |
| GET | `/_transit` · POST `/_transit/claim` · POST `/_transit/expire` | Transit store: pending handoffs, claims, TTL sweep |
| POST | `/_checkpoint` | Quorum-gated GC below the checkpoint |
| POST | `/_verify` | Verify a peer's tip against ours (the quorum vote) |
| GET | `/_placement/:collection/:key` | Which nodes own a key, and why |
| POST | `/_search/vector/:collection` | k-NN over a vector collection |
| GET | `/_ts/:collection/rollups` | Time-series rollups |
| GET | `/_graph/:collection/:key` | Adjacency for a graph node |

Supervisor-only nodes serve a further `/_supervisor/*` surface — `scan`,
`mounts`, `inspect`, `nodes` (+ per-node `transition`, `recovery-key-exported`),
`manifest`, `pass`, `topology`, `pairing`. It is bound to loopback and is the
app's private control channel; **no data is ever routed through it.**

Every write is broadcast to connected peers and merged via CRDT so all nodes
converge without coordination. Every write is also appended to a SHA-256
hash-chained, Ed25519-signed audit ledger — a tamper-evident history of every
mutation. Reads, writes and gossip are all filtered by the collection's ACL:
a non-reader receives hashes, never bytes.

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

v2 keeps that shape and adds a second plane above it: app-local **supervisors**
(`supervisor: true`, loopback-only, never elected, never a replication hop)
that handle hardware discovery, fitness ranking, placement, key custody, quota
and node lifecycle. The data plane stays flat P2P — kill every supervisor and
the mesh is functionally unchanged.

For the full design document — goals/non-goals, prior-art comparison, CRDT
consistency model, storage engine internals, network protocol, security model,
hash-chained audit ledger, and roadmap — see
[`docs/architecture.md`](docs/architecture.md). For the v2 layer —
supervisors, the storage router, ledger v2, ACLs and the threat model — see
[`docs/architecture-v2.md`](docs/architecture-v2.md).

For a comparison against an alternate consortium/routing design and what was
adopted from it, see [`docs/comparison.md`](docs/comparison.md).

---

## Directory Structure

```
De-Sentry/
├── README.md                       ← You are here
├── AGENTS.md / AGENT.md            ← How to build, test and work in this repo
├── DESIGN.md                       ← Control-room design language (tokens, components, screens)
├── STATUS.md                       ← What is built, what was verified, what was not
├── CMakeLists.txt                  ← Build system
├── Dockerfile                      ← Container image build
├── docker-compose.yml              ← 3-node cluster + integration tests
├── docker-entrypoint.sh            ← Container entrypoint (renders config from env)
│
├── include/desentry/               ← Public headers, organized by layer
│   └── (common, storage, crdt, ledger, security, net, engine, api, supervisor)
├── src/                            ← Implementation (mirrors include/)
│   ├── api/                        ← HTTP/1.1 REST server + routes
│   ├── common/                     ← Status/StatusOr, JSON, byte buffers, platform shim
│   ├── crdt/                       ← CRDT types + HLC
│   ├── engine/                     ← Node engine (ties everything together)
│   ├── ledger/                     ← Hash-chained ledger, transit store, checkpoint, feed
│   ├── net/                        ← P2P networking, gossip, discovery, placement, admission
│   ├── security/                   ← Crypto (Ed25519, X25519, AES-GCM)
│   ├── storage/                    ← Disk manager, buffer pool, WAL, B+Tree, router
│   │   └── engines/                ← One file per EngineBackend (5 from scratch + 4 opt-in)
│   └── supervisor/                 ← Supervisor-only logic, hardware scan, extra routes
├── apps/
│   ├── desentry_node/              ← desentryd daemon entrypoint
│   └── desentry_cli/               ← Thin CLI client
│
├── app/                            ← Desktop control room (Tauri v2)
│   ├── src/                        ← TypeScript frontend (no framework)
│   ├── src-tauri/src/              ← Rust sidecar: ports, config, supervision, keychain, AI
│   ├── resources/                  ← Engine prototypes; the sizing model lands here
│   └── scripts/                    ← Icon generation, model fetch, sidecar staging
│
├── tests/                          ← Assert-based C++ suites (9 binaries, one per file)
│   └── integration/                ← Python: cluster, transit replay, airplane mode, USB, 50-node soak
├── clients/python/                 ← Zero-dependency Python client
├── tools/dashboard.html            ← Browser dashboard (no build step)
├── config/                         ← Example node config (JSON)
│
├── docs/
│   ├── architecture.md             ← Full v1 system design spec
│   ├── architecture-v2.md          ← Supervisors, router, ledger v2, threat model
│   ├── comparison.md               ← Comparison against alternate design
│   ├── apple-reference.md          ← Unmodified design-language source for DESIGN.md
│   └── original_design/            ← Original consortium/routing design docs (historical)
├── scripts/
│   ├── run_cluster.sh / .ps1       ← Launch a local N-node cluster
│   ├── stop_cluster.sh / .ps1      ← Stop the cluster
│   ├── dsync.py / .ps1 / .sh       ← Git collaboration tooling
│   └── README.md                   ← Collaboration workflow guide
│
├── Project_Statement/              ← Original course project brief
├── Research_Docs/                  ← Annotated bibliography and research
├── third_party/                    ← Opt-in vendored backends (never auto-downloaded)
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

**v1** was validated end-to-end with real compiled binaries: four test suites
passing with live assertions, and a 3-node mesh of real OS processes
replicating over real sockets on one host.

**v2** — the desktop app, supervisors, storage router, ledger v2, ACLs and the
onboard sizing model — was written in an environment with no CMake, no cargo
and no OpenSSL headers, so **none of it has been compiled, linked or run**.
The frontend typechecks and builds, the QR encoder passes conformance checks
against the ISO published constants, and every C++ source and test passes a
`-fsyntax-only` sweep — but that is not the same as a passing `ctest`.

[`STATUS.md`](STATUS.md) says exactly what was executed and what was not, plus
the bugs found, the known limits, and the order to build things in on a full
toolchain. Read it before quoting any status from this page.

---

*De-Sentry — Decentralized Sentinel for AI Agent Data*
