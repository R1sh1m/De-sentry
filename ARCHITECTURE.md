# De-Sentry: System Architecture

> **Related docs**: [Node Design](docs/node_design.md) · [Sync Protocol](docs/sync_protocol.md) · [Change Ledger](docs/change_ledger.md) · [API Spec](docs/api_spec.md) · [Routing & Specialization](docs/routing_and_specialization.md)

## 1. Overview

De-Sentry is a decentralized database consortium designed for multi-agent AI systems. Rather than routing all reads and writes through a single shared instance, each agent owns a **sandboxed node** that holds its primary data independently. Nodes cooperate over a full-mesh network to maintain a consistent view of the entire consortium's state.

Each node is **type-specialized** — it declares a set of MIME types it is optimized to store and indexes data accordingly. A multi-factor **fitness ranking algorithm** determines which node is the best destination for any given piece of data. All routing decisions are permanently recorded in a dedicated **Routing Ledger** — a second hash-chained append-only log separate from the per-node Change Ledger.

The design deliberately trades *strong global consistency* for *high write availability and low coordination overhead* — the same trade-off made by Amazon Dynamo, and formally described by the CAP theorem. In the AP corner of CAP, we prioritize availability and partition tolerance, accepting eventual (not immediate) consistency for the shared brain-file layer.

A **3-level node hierarchy** (ROOT → SPECIALIST → GENERALIST) defines routing priority and coordinator responsibilities. When a target node is offline or at capacity, data **cascades** to the next-best node in the ranked list — never rejected, never silently evicted. Every cascade is recorded on the Routing Ledger and reversed (handed off) automatically once the target node reconnects.

---

## 2. High-Level Topology

### 2.1 Full-Mesh (3 Nodes)

```
        ┌──────────────────────────────────────────┐
        │              Consortium Network           │
        │                                           │
        │   ┌─────────┐         ┌─────────┐        │
        │   │  Node A  │◄───────►│  Node B  │       │
        │   │ (Agent 1)│         │ (Agent 2)│       │
        │   └────┬─────┘         └────┬─────┘       │
        │        │                    │              │
        │        │    ┌─────────┐    │              │
        │        └───►│  Node C  │◄──┘              │
        │             │ (Agent 3)│                  │
        │             └─────────┘                   │
        └──────────────────────────────────────────┘
```

Every node maintains a **bidirectional TCP connection** to every other node. With N=3 this yields 3 edges — a manageable constant. Messages exchanged:

| Message Type | Direction | Frequency |
|---|---|---|
| Brain file push | Any → Any | Periodic (configurable TTL) |
| Brain file pull | Any → Any | On-demand / cache-miss |
| Ledger entry notify | Originating node → peers | On every write |
| Health ping | Any → Any | Heartbeat interval |

### 2.2 Ownership Model

Each node is the **sole primary owner** of its own data partition. No other node may write to another node's primary data. This is the fundamental design decision that makes distributed locking unnecessary for the normal write path.

```
Node A owns:  /data/A/*
Node B owns:  /data/B/*
Node C owns:  /data/C/*

Brain files (read-only copies):
  Node A cache:  brain_B.json, brain_C.json
  Node B cache:  brain_A.json, brain_C.json
  Node C cache:  brain_A.json, brain_B.json
```

---

## 3. Two Ledgers

The system maintains two distinct hash-chained append-only logs:

| Ledger | Writer | Purpose |
|---|---|---|
| **Change Ledger** | Each node (locally) | Every data mutation on that node's primary store |
| **Routing Ledger** | ROOT node (replicated to all) | Every routing decision, cascade, handoff, type claim, and fitness ranking update |

The Routing Ledger is the **central registry** — the authoritative source of truth for which node owns which data type, and where every piece of data has been at every point in time. Full spec: [`docs/routing_and_specialization.md`](docs/routing_and_specialization.md).

---

## 4. Node Anatomy

Each node is a self-contained process with the following internal components. All nodes carry **two storage layers** — a type-specialized primary store and a generic transit store for temporarily holding foreign-type data while the correct owner node is offline:

```
┌────────────────────────────────────────────────────────────────┐
│                        Node Engine (C++)                        │
│                                                                │
│  ┌──────────────────┐   ┌──────────────┐   ┌───────────────┐  │
│  │  PRIMARY STORE   │   │ Change Ledger│   │  Brain File   │  │
│  │  (type-optimized)│   │ (WAL, hash-  │   │  Generator    │  │
│  │  e.g. columnar,  │   │  chained)    │   │  + Cache      │  │
│  │  blob, FTS...    │   │              │   │               │  │
│  └──────┬───────────┘   └──────┬───────┘   └──────┬────────┘  │
│         │                      │                   │           │
│  ┌──────────────────┐          │                   │           │
│  │  TRANSIT STORE   │          │                   │           │
│  │  (generic blob)  │          │                   │           │
│  │  Holds foreign-  │          │                   │           │
│  │  type data for   │          │                   │           │
│  │  offline peers   │          │                   │           │
│  └──────┬───────────┘          │                   │           │
│         │                      │                   │           │
│  ┌──────▼───────────────────────▼───────────────────▼────────┐ │
│  │  Data Classification Pipeline + Cascade Decision Engine    │ │
│  │  (MIME sniff → fitness ranking → route / cascade / hold)   │ │
│  └───────────────────────────┬───────────────────────────────┘ │
│                              │                                 │
│  ┌───────────────────────────▼───────────────────────────────┐ │
│  │              Network I/O Layer                             │ │
│  │    (TCP listener + connection pool to 2 peers)            │ │
│  └───────────────────────────┬───────────────────────────────┘ │
└──────────────────────────────┼─────────────────────────────────┘
                               │
              ┌────────────────▼────────────────┐
              │   IPC Bridge (N-API / stdin)     │
              │   Python bindings (pybind11)     │
              └─────────────────────────────────┘
```

---

## 4. The Change Ledger (Hash-Chained WAL)

Every mutation to a node's local database produces an immutable **ledger entry** appended to the node's Write-Ahead Log.

### 4.1 Entry Structure

```
┌────────────────────────────────────────────┐
│              Ledger Entry                  │
├────────────────────────────────────────────┤
│ entry_id      : uint64  (monotonic counter)│
│ node_id       : string  ("A" | "B" | "C") │
│ timestamp_us  : int64   (Unix µs, local)   │
│ operation     : enum    {INSERT, UPDATE,   │
│                          DELETE, SCHEMA}   │
│ table         : string                     │
│ key           : bytes                      │
│ payload       : bytes   (serialized delta) │
│ prev_hash     : bytes[32] (SHA-256)        │
│ entry_hash    : bytes[32] (SHA-256)        │
└────────────────────────────────────────────┘
```

### 4.2 Hash Chain Invariant

```
entry_hash[n] = SHA-256(
    entry_id[n]    ||
    node_id[n]     ||
    timestamp_us[n]||
    operation[n]   ||
    payload[n]     ||
    prev_hash[n]         ← prev_hash[n] == entry_hash[n-1]
)
```

The genesis entry (n=0) uses `prev_hash = 0x00...00` (32 zero bytes).

This structure means:
- **Tamper detection**: altering any field in entry `n` invalidates `entry_hash[n]` and every subsequent hash.
- **Append-only**: entries cannot be removed without breaking the chain.
- **Verifiability**: any peer can replay and recompute the entire chain from the genesis entry.

> ⚠️ **Limitation (acknowledged)**: SHA-256 integrity only detects accidental or post-hoc tampering. It does not protect against a malicious node re-signing a fabricated chain from scratch. A production system would require signed entries with a PKI.

---

## 5. Brain Files

A **brain file** is a compact JSON snapshot of a node's current state. It is the primary mechanism by which nodes gain awareness of the rest of the consortium without executing expensive cross-node queries.

### 5.1 Schema

```json
{
  "node_id": "A",
  "schema_version": 1,
  "generated_at_us": 1735000000000000,
  "generation": 42,
  "ttl_seconds": 30,
  "ledger_tip": {
    "entry_id": 1034,
    "entry_hash": "a3f9...c12b"
  },
  "table_summaries": [
    {
      "table": "agents",
      "row_count": 512,
      "last_modified_us": 1734999990000000,
      "checksum": "d4e2...8f01"
    }
  ],
  "custom_metadata": {}
}
```

### 5.2 Lifecycle

```
[Write occurs on Node A]
        │
        ▼
[Change Ledger appended]
        │
        ▼  (async, throttled)
[Brain File Generator runs]
        │
        ▼
[brain_A.json regenerated + generation++ ]
        │
        ├──► pushed to Node B cache
        └──► pushed to Node C cache

[Node B / C read agent data summary]
        │
        ▼
[Check local cache: is brain_A.json fresh? (now < generated_at + ttl)]
        │
   Yes──┤──► serve from cache (no network hop)
        │
   No───┤──► pull fresh brain_A.json from Node A
```

### 5.3 Conflict Resolution for Brain Files

Brain files are **not** subject to multi-node concurrent write conflicts — each node is the sole generator of its own brain file. However, two nodes may have cached different generations of the same brain file.

**Resolution rule**: A reader always prefers the brain file with the highest `generation` counter. If two cached copies have the same generation but different hashes (should not happen in practice), the copy that was received from the originating node takes precedence.

---

## 6. Sync Protocol

Full specification in [`docs/sync_protocol.md`](docs/sync_protocol.md). Summary:

1. **Periodic push**: every `BRAIN_SYNC_INTERVAL_S` seconds, each node pushes its own fresh brain file to both peers.
2. **Cache-miss pull**: if a node needs a peer's brain file and the cached copy is expired (`now > generated_at + ttl`), it issues a synchronous pull.
3. **Ledger tail notification**: after committing a change ledger entry, the originating node sends a lightweight `LEDGER_NOTIFY` message to peers containing only `{ node_id, entry_id, entry_hash }`. Peers store this to know the originating node's ledger tip without receiving the full payload.
4. **Integrity check**: any node can request a full ledger replay from any peer and recompute hashes to verify integrity.

---

## 7. Coordination: Shared-State Updates

For the rare case where coordination across nodes is required (e.g., updating a shared registry or global schema change), the system uses a **lightweight coordinator** approach:

- **Coordinator**: Node A is designated coordinator (configurable at startup).
- **Serialization**: the coordinator is the sole writer to the shared-state namespace; other nodes submit change requests to the coordinator via HTTP.
- **Acknowledged Limitation**: This reintroduces a soft dependency on one node (Node A). A production system would replace this with Raft or Multi-Paxos for proper distributed consensus. We accept this trade-off for course scope.

---

## 8. Layered Tech Stack

```
┌──────────────────────────────────────────────────────────┐
│  Electron UI  (React + TypeScript)                        │
│  - Node topology visualizer                               │
│  - Live ledger explorer (hash chain browser)              │
│  - Brain file dashboard                                   │
│  - Write simulator / test console                         │
└────────────────────────┬─────────────────────────────────┘
                         │ N-API (node-addon-api)
┌────────────────────────▼─────────────────────────────────┐
│  C++ Core Engine  (C++17)                                 │
│  - Local DB layer (SQLite or custom B-Tree)               │
│  - Change Ledger (append-only log + SHA-256 chaining)     │
│  - Brain File generator + cache manager                   │
│  - TCP networking (libuv or Boost.Asio)                   │
│  - JSON serialization (nlohmann/json or RapidJSON)        │
└────────────────────────┬─────────────────────────────────┘
                         │ pybind11
┌────────────────────────▼─────────────────────────────────┐
│  Python Bindings                                          │
│  - Agent API (read/write from Python agent code)          │
│  - Analytics / brain file query helpers                   │
│  - CLI tools for ledger verification                      │
└──────────────────────────────────────────────────────────┘
```

---

## 9. Assumptions and Limitations

| # | Limitation | Rationale |
|---|---|---|
| 1 | **No distributed mutual exclusion** | Raft/Paxos is research-level complexity; last-write-wins by timestamp is used instead. Clock drift is acknowledged. |
| 2 | **Not a blockchain** | No global consensus among untrusted parties. Hash chaining provides tamper-evidence, not Byzantine fault tolerance. |
| 3 | **No partition handling** | All 3 nodes assumed reachable during sync. No retry/reconciliation logic. |
| 4 | **Fixed 3-node topology** | No dynamic peer discovery; full-mesh at N=3 only. |
| 5 | **No adversarial trust model** | Nodes are assumed honest. Hashing detects accidental corruption, not malicious re-signing. |

---

## 10. Key Academic References

| Paper | Maps to |
|---|---|
| Chandy & Lamport (1985) *Distributed Snapshots* | Brain file = consistent distributed snapshot |
| DeCandia et al. (2007) *Dynamo* | Full architecture analogy (node ownership, LWW, gossip) |
| Brewer (2000) *CAP Theorem* + Gilbert & Lynch (2002) | Our AP trade-off |
| Ongaro & Ousterhout (2014) *Raft* | The consensus mechanism we consciously omit |
| Preguiça (2018) *CRDTs Overview* | Theoretical basis for conflict-free node ownership |
| Demers et al. (1987) *Epidemic Algorithms* | Brain file gossip exchange pattern |
| Lamport (1978) *Time, Clocks* | Timestamp ordering in LWW conflict resolution |

Full annotated bibliography: [`Research_Docs/Research Papers/index.md`](Research_Docs/Research%20Papers/index.md)
