# De-Sentry — Decentralized AI Agent Database

> A high-performance, decentralized consortium database designed for multi-agent AI systems.

---

## Overview

When multiple autonomous AI agents simultaneously read and write to a shared central database, they cause **network congestion** and **data-locking bottlenecks**. De-Sentry eliminates this central point of failure by giving each agent its own independent, sandboxed database node. Together, these nodes form a cooperating **consortium** that maintains data integrity without funneling every operation through a single instance.

### The Three Core Pillars

| Pillar | Description |
|---|---|
| **Change Ledger** | Every mutation is recorded as a cryptographically hash-chained, append-only entry — a tamper-evident audit trail akin to Git commits |
| **Conflict Handling** | Node-owned primary data eliminates cross-node write conflicts by design; lightweight last-write-wins coordination handles the rare shared-state updates |
| **Brain Files** | Each node maintains a compact snapshot of its own state; nodes cache each other's brain files for fast network-wide awareness without expensive full queries |

---

## Tech Stack

| Layer | Technology | Role |
|---|---|---|
| **Core Engine** | C / C++ (C++17) | Database engine, WAL, hash-chaining, networking |
| **Python Layer** | Python 3.11+ via `pybind11` | Scripting, agent bindings, analytics |
| **Desktop UI** | Electron + React + TypeScript | Dashboard, node visualizer, ledger explorer |
| **Native Bridge** | `node-addon-api` (N-API) | C++ ↔ Node.js interop |
| **IPC** | JSON-RPC over stdin/stdout or local HTTP | Electron ↔ C++ / Python communication |
| **Build** | CMake + `node-gyp` | C++ compilation and addon packaging |

---

## Architecture at a Glance

```
 ┌─────────────────────────────────────────────────────────┐
 │                   Electron Dashboard                     │
 │            (React + TypeScript + node-addon-api)         │
 └─────────────────────┬───────────────────────────────────┘
                       │ IPC (JSON-RPC)
         ┌─────────────▼─────────────────┐
         │       C++ Node Engine          │
         │  ┌──────────┐  ┌───────────┐  │
         │  │  Local DB │  │ Change    │  │
         │  │ (SQLite/  │  │ Ledger    │  │
         │  │  custom)  │  │ (WAL+hash)│  │
         │  └──────────┘  └───────────┘  │
         │  ┌──────────┐  ┌───────────┐  │
         │  │Brain File │  │ Sync /    │  │
         │  │Generator  │  │Gossip I/O │  │
         │  └──────────┘  └───────────┘  │
         └───────────┬───────────────────┘
                     │ TCP / HTTP (Full Mesh)
         ┌───────────▼───────────────────┐
         │        Consortium Network      │
         │   Node A ──── Node B ──── Node C │
         │       \              /        │
         │        ──────────────         │
         └───────────────────────────────┘
```

---

## Project Scope

This is a **course project** validated at exactly **3 nodes** running locally in a full mesh.

### What this project solves
- Eliminating the single-point-of-failure central database for multi-agent workloads
- Providing tamper-evident audit trails via hash-chained WAL entries
- Fast network-wide state awareness through cached brain files

### Explicit Non-Goals (Course Scope)
- No true distributed mutual exclusion (no Raft/Paxos)
- No Byzantine fault tolerance
- No network partition / node-failure recovery
- No dynamic peer discovery for N > 3
- Not a blockchain (no global consensus among untrusted parties)

---

## Directory Structure

```
Database Project/
├── README.md                      ← You are here
├── ARCHITECTURE.md                ← Full system design spec
├── Project_Statement/             ← Original project brief
├── docs/
│   ├── node_design.md             ← Per-node internals
│   ├── sync_protocol.md           ← Brain file exchange protocol
│   ├── change_ledger.md           ← Hash-chain specification
│   └── api_spec.md                ← Inter-node API surface
├── src/
│   ├── core/                      ← C++ engine source
│   ├── python/                    ← Python bindings (pybind11)
│   └── ui/                        ← Electron + React frontend
├── Research_Docs/
│   └── Research Papers/           ← Annotated bibliography
└── tests/                         ← Unit + integration tests
```

---

## Getting Started (Local 3-Node Demo)

> Prerequisites: CMake ≥ 3.20, Node.js ≥ 20 LTS, Python 3.11+, a C++17 compiler

```bash
# 1. Clone the repo
git clone <repo-url>
cd "Database Project"

# 2. Build the C++ core
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel
cd ..

# 3. Install Python bindings
pip install -e ./src/python

# 4. Install and build Electron UI
cd src/ui
npm install
npm run build:addon   # compiles N-API addon
npm run dev           # launches Electron in dev mode

# 5. Launch all three nodes (separate terminals)
./build/desentry-node --id A --port 7001 --peers B:7002,C:7003
./build/desentry-node --id B --port 7002 --peers A:7001,C:7003
./build/desentry-node --id C --port 7003 --peers A:7001,B:7002
```

---

## Research Foundation

Key papers underpinning the design — full annotated bibliography in [`Research_Docs/Research Papers/index.md`](Research_Docs/Research%20Papers/index.md):

- **Dynamo** (DeCandia et al., SOSP 2007) — node-owned data + last-write-wins + gossip
- **Distributed Snapshots** (Chandy & Lamport, 1985) — brain file = consistent global snapshot
- **Raft** (Ongaro & Ousterhout, 2014) — consensus baseline we consciously simplify
- **CAP Theorem** (Brewer, 2000; Gilbert & Lynch, 2002) — our AP trade-off justification
- **CRDTs** (Preguiça, 2018) — theoretical basis for conflict-free node ownership

---

## Team

| Member | Node | Role |
|---|---|---|
| TBD | Node A | TBD |
| TBD | Node B | TBD |
| TBD | Node C | TBD |

---

*De-Sentry — Decentralized Sentinel for AI Agent Data*
