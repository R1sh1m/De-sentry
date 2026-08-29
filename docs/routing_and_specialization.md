# De-Sentry: Node Specialization, Routing Ledger & Hierarchy Design

> **Related docs**: [Node Capability Score (NCS)](node_capability_score.md) — hardware benchmarking spec that feeds into the fitness formula below.

## 1. Overview

This document specifies the **type-aware routing system** for De-Sentry. It introduces:

- A **Routing Ledger** — a second append-only, hash-chained log maintained by the coordinator node and replicated to all peers. It is the authoritative record of every routing decision, cascade event, and handoff in the system.
- A **Node Fitness Ranking Algorithm** — a multi-factor score that determines which node should own or hold a given piece of data.
- A **Data Classification Pipeline** — how incoming data is typed and matched against node profiles.
- A **3-Level Node Hierarchy** — structural roles for nodes with defined responsibilities and routing priority.
- **Cascade Rules** — what happens when a target node is offline or full. Data is **never rejected and never evicted**; everything is accounted for on the routing ledger.

---

## 2. Two Ledgers, Two Responsibilities

The system maintains two separate hash-chained logs:

| Ledger | Maintained By | Tracks |
|---|---|---|
| **Change Ledger** (existing) | Each node, locally | Every mutation to that node's primary data |
| **Routing Ledger** (new) | Coordinator node (replicated to all) | Every routing decision, cascade, handoff, and node registration |

The Routing Ledger is the **central registry**. It is the single source of truth for:
- Which node owns which data types
- What data is in transit, where, and why
- The full history of every cascade and handoff event

Both ledgers use the same hash-chain mechanism (SHA-256, append-only). The Routing Ledger lives on the coordinator node's disk and is distributed as part of the brain file exchange so all peers hold a read-only replica.

---

## 3. The 3-Level Node Hierarchy

Every node in the consortium is assigned exactly one **hierarchy level**. There are three levels, and the total depth of the hierarchy is capped at 3.

```
Level 0  ──  ROOT (Coordinator)
              │
              ├── Level 1  ──  SPECIALIST Node(s)
              │
              └── Level 2  ──  GENERALIST Node(s)
```

### Level 0 — ROOT (Coordinator)

- **Exactly one node** holds the ROOT role at any time (statically assigned in config for course scope).
- Maintains and writes to the **Routing Ledger**.
- Holds its own type specialization in addition to coordinator duties.
- All nodes hold a read-only replica of the Routing Ledger (received via brain file sync).
- In the 3-node demo: **Node A = ROOT**.

> **Acknowledged limitation**: Static ROOT assignment reintroduces a soft single point of failure for the routing registry. A production system would use a Raft-based leader election to promote a Level-1 node to ROOT if the current ROOT fails.

### Level 1 — SPECIALIST

- Declares **primary ownership** of one or more MIME type groups.
- The **first-choice target** for any data matching its declared types.
- Maintains a type-optimized primary storage engine (see §7).
- Can hold transit data for other nodes (transit store).
- In the 3-node demo: **Node B = SPECIALIST** (e.g., CSV/XML/tabular data).

### Level 2 — GENERALIST

- Declares **partial capability** across multiple types (no single primary specialization).
- The **second- or third-choice target** for routing decisions.
- Acts as the consortium's relay and overflow absorber.
- Has a larger transit store quota than specialists by default.
- In the 3-node demo: **Node C = GENERALIST**.

### Hierarchy Rules

1. A node's level is declared in `node_profile.toml` and registered on the Routing Ledger at startup via a `NODE_REGISTER` entry.
2. The hierarchy has **at most 3 levels (0, 1, 2)**. No sub-hierarchies or nested levels.
3. For routing decisions, the hierarchy determines **tie-breaking order** when fitness scores are equal: Level 0 > Level 1 > Level 2.
4. The ROOT node can reassign hierarchy levels by appending a `LEVEL_CHANGE` entry to the Routing Ledger — no node can self-promote above its configured level without coordinator approval.

---

## 4. Node Fitness Ranking Algorithm

Every node has a computed **fitness score** for each data type it can handle. This score determines routing priority.

### 4.1 Fitness Score Formula

```
fitness(node N, type T) =
    w_cap  × capability_score(N, T) × ncs_multiplier(N, T)  // scaled by hardware reality
  + w_load × (1 − load_ratio(N))          // how much capacity does N have left?
  + w_perf × effective_performance(N, T)  // NCS-predicted until empirical data exists
  + w_avail × availability_score(N)       // how reliably online has N been?
```

See [`node_capability_score.md`](node_capability_score.md) for the full definition of `ncs_multiplier` and `effective_performance`. In brief:
- **`ncs_multiplier(N, T)`**: scales a node's declared capability by its actual hardware fitness for type T (e.g., a node claiming image specialization but running on HDD gets a multiplier < 0.5).
- **`effective_performance(N, T)`**: blends NCS-predicted performance with empirical write latency history as write count grows — new nodes are not penalized for having no history.

Default weights (tunable per deployment):

| Weight | Default | Description |
|---|---|---|
| `w_cap` | **0.45** | Highest weight — specialization is the primary routing signal |
| `w_load` | **0.25** | Second — don't overload a nearly-full node |
| `w_perf` | **0.20** | Third — prefer nodes with proven fast handling of this type |
| `w_avail` | **0.10** | Least — recent uptime history as a tiebreaker |

All component scores are normalized to `[0.0, 1.0]`. Final fitness is therefore in `[0.0, 1.0]`.

### 4.2 Component Score Definitions

#### `capability_score(N, T)`

Declared in `node_profile.toml`. The node explicitly rates its own capability per MIME type group:

```toml
[capabilities]
"image/*"       = 1.00   # Primary specialization
"video/mp4"     = 0.60   # Partial capability
"application/*" = 0.10   # Generic fallback only
"*/*"           = 0.05   # Catch-all — last resort
```

Nodes that declare `*/*` capability with a low score can accept any data type as a last resort, providing the "never reject" guarantee.

#### `load_ratio(N)`

```
load_ratio(N) = (primary_used_bytes + transit_used_bytes) / total_quota_bytes
```

Read from the node's brain file (updated on every brain file refresh). A node at 90% capacity has `load_ratio = 0.90`, contributing `0.25 × (1 − 0.90) = 0.025` to fitness — heavily deprioritized.

#### `performance_score(N, T)`

A rolling exponential moving average (EMA) of write latency for type T on node N, normalized against the network-wide best latency for that type:

```
performance_score(N, T) = best_latency_for_T / ema_latency(N, T)
```

If node N has never handled type T, it gets a neutral score of `0.50`.

Updated every time a write completes (the write latency is recorded in the Routing Ledger's `WRITE_COMPLETE` entry).

#### `availability_score(N)`

```
availability_score(N) = successful_pings_last_24h / total_pings_last_24h
```

Read from each node's own heartbeat log. A node that was offline for 6 of the last 24 hours scores `0.75`.

### 4.3 Ranking Output

The coordinator maintains a **ranked node list** per MIME type group, recomputed whenever:
- A brain file update changes a node's load or availability, OR
- A write completes and updates a performance score.

The ranked list is stored in the Routing Ledger as a `RANKING_UPDATE` entry and replicated to all nodes in the next brain file push.

```
Ranked list for "image/jpeg" (example):
  1. Node A  →  fitness = 0.88  (Level 0, specialist)
  2. Node C  →  fitness = 0.31  (Level 2, generalist)
  3. Node B  →  fitness = 0.09  (Level 1, specialist — wrong type)
```

---

## 5. Data Classification Pipeline

When data arrives at any node (from an agent or via cascade), it goes through a multi-stage classification pipeline before being routed.

```
Incoming data (bytes + optional hints from agent)
        │
        ▼
┌───────────────────────────┐
│  Stage 1: Header Sniff    │  Read first 512 bytes
│  (libmagic / custom C++)  │  Determine MIME type from magic bytes
└───────────┬───────────────┘
            │
            ▼
┌───────────────────────────┐
│  Stage 2: Extension Hint  │  If filename present, check extension map
│                           │  Extension agreement → confidence +0.3
│                           │  Extension disagreement → flag for review
└───────────┬───────────────┘
            │
            ▼
┌───────────────────────────┐
│  Stage 3: Agent Hint      │  Agent may declare mime_type in metadata
│                           │  Validated against Stage 1 result
│                           │  Agreement → confidence +0.2
└───────────┬───────────────┘
            │
            ▼
┌───────────────────────────┐
│  Stage 4: Confidence      │  Final MIME type + confidence score [0,1]
│  Aggregation              │  If confidence < 0.5 → classify as */*
└───────────┬───────────────┘
            │
            ▼
┌───────────────────────────┐
│  Stage 5: Routing Query   │  Fetch ranked node list for this MIME type
│                           │  from local Routing Ledger replica
└───────────┬───────────────┘
            │
            ▼
      [Cascade Decision Engine — see §6]
```

### Classification Entry on Routing Ledger

Every classification result is appended to the Routing Ledger:

```json
{
  "type": "DATA_CLASSIFY",
  "data_id": "uuid-v4",
  "detected_mime": "image/jpeg",
  "confidence": 0.92,
  "extension_hint": ".jpg",
  "agent_hint": "image/jpeg",
  "size_bytes": 2097152,
  "routed_to": "A",
  "routing_reason": "primary_owner_available"
}
```

This means every piece of data in the system has a permanent routing history, traceable from first arrival.

---

## 6. Cascade Decision Engine

This is the core routing state machine. It runs on the **receiving node** (the node the agent is directly connected to) and consults the local Routing Ledger replica.

### 6.1 Decision Flow

```
classify(data) → mime_type, confidence
ranked_nodes = routing_ledger.get_ranked_list(mime_type)

FOR each node in ranked_nodes (ordered by fitness, descending):

    IF node is reachable AND node.transit_available():
        IF node == this_node:
            write to this_node.primary_store  (if this_node is owner)
            OR write to this_node.transit_store (if this_node is holding on behalf)
        ELSE:
            forward to node via ROUTE_WRITE message
        ENDIF
        log ROUTE_DECISION on Routing Ledger
        RETURN success

    ELSE IF node is offline:
        log SKIP_NODE(reason=offline) on Routing Ledger
        CONTINUE to next node in ranked list

    ELSE IF node.transit_store is full:
        log SKIP_NODE(reason=transit_full) on Routing Ledger
        CONTINUE to next node in ranked list

END FOR

// All ranked nodes exhausted — Emergency path
→ Expand transit quota on current node (TRANSIT_QUOTA_EXPAND entry)
→ Write to local transit store regardless
→ Log EMERGENCY_STORE on Routing Ledger
→ Broadcast QUOTA_ALERT to all peers on next heartbeat
```

> **Invariant**: The loop above will always find a node because every node declares a `*/*` catch-all capability with a non-zero (if very low) score. A node at 100% quota triggers an `EMERGENCY_EXPAND` before writing — it never silently drops data.

### 6.2 Cascade Depth

Each cascade adds one level of depth. The routing ledger entry tracks this:

| `cascade_depth` | Meaning | Routing Ledger Event |
|---|---|---|
| 0 | Written directly to primary owner | `ROUTE_DIRECT` |
| 1 | Written to first-choice alternate (primary owner offline/full) | `CASCADE_STORE` depth=1 |
| 2 | Written to second-choice alternate (both previous nodes unavailable) | `CASCADE_STORE` depth=2 |
| ≥3 | Emergency path — all ranked nodes exhausted | `EMERGENCY_STORE` |

The maximum cascade depth in the 3-node system is naturally 2 (the third node is always the fallback).

---

## 7. Routing Ledger: Entry Types

The Routing Ledger uses the **same hash-chain mechanism** as the Change Ledger (SHA-256, append-only, `prev_hash` links). The coordinator node is the sole **writer**; all other nodes hold **verified read-only replicas**.

### Entry Type Catalog

```
NODE_REGISTER      — Node joins consortium, declares level + capabilities
NODE_DEREGISTER    — Node formally leaves (graceful shutdown)
LEVEL_CHANGE       — Coordinator reassigns a node's hierarchy level
TYPE_CLAIM         — Node claims primary ownership of a MIME type group
TYPE_RELEASE       — Node relinquishes type ownership
RANKING_UPDATE     — Updated fitness scores for a MIME type's node ranking
DATA_CLASSIFY      — Classification result for a data write
ROUTE_DIRECT       — Data written directly to primary owner (cascade_depth=0)
CASCADE_STORE      — Data placed in transit on alternate node; includes:
                     { data_id, held_by, target_owner, cascade_depth, reason }
EMERGENCY_STORE    — All ranked nodes exhausted; stored locally with quota expand
TRANSIT_QUOTA_EXPAND — Node's transit quota expanded (reason + new limit recorded)
QUOTA_ALERT        — Broadcast: node approaching or at capacity
HANDOFF_INIT       — Target node came online; initiating drain of transit data
HANDOFF_ACK        — Target node confirmed receipt of a specific data_id
HANDOFF_COMPLETE   — All transit data for a target node has been successfully drained
WRITE_COMPLETE     — Write latency recorded (feeds performance_score updates)
COORDINATOR_ELECT  — Coordinator role transferred (future: Raft-based)
```

### CASCADE_STORE Entry (Full Schema)

```json
{
  "entry_type": "CASCADE_STORE",
  "entry_id": 2041,
  "timestamp_us": 1735000000000000,
  "data_id": "f7a3c2d1-...",
  "mime_type": "image/jpeg",
  "size_bytes": 2097152,
  "original_target_node": "A",
  "held_by_node": "C",
  "cascade_depth": 1,
  "reason": "target_node_offline",
  "skipped_nodes": [
    { "node_id": "A", "reason": "offline" },
    { "node_id": "B", "reason": "transit_full" }
  ],
  "expires_never": true,
  "prev_hash": "...",
  "entry_hash": "..."
}
```

`expires_never: true` enforces the no-eviction guarantee at the ledger level. Any code path that would evict a transit entry MUST first check the Routing Ledger for a `CASCADE_STORE` entry — if one exists, eviction is **blocked**.

---

## 8. Transit Store and Handoff Protocol

### 8.1 Transit Store Schema (per node)

```sql
CREATE TABLE transit_store (
    data_id          TEXT PRIMARY KEY,
    target_owner     TEXT NOT NULL,      -- which node should ultimately own this
    held_by          TEXT NOT NULL,      -- this node's ID
    cascade_depth    INTEGER NOT NULL,
    routing_ledger_entry_id INTEGER NOT NULL,  -- FK to CASCADE_STORE entry
    mime_type        TEXT NOT NULL,
    size_bytes       INTEGER NOT NULL,
    raw_data         BLOB NOT NULL,
    received_at_us   INTEGER NOT NULL,
    status           TEXT NOT NULL CHECK(status IN ('HELD','HANDOFF_IN_PROGRESS','COMPLETE'))
);

-- Quota tracking
CREATE TABLE transit_quota (
    node_id          TEXT PRIMARY KEY,
    used_bytes       INTEGER NOT NULL DEFAULT 0,
    quota_bytes      INTEGER NOT NULL,
    emergency_expanded INTEGER NOT NULL DEFAULT 0  -- flag: normal=0, emergency=1
);
```

### 8.2 Handoff Protocol (Target Node Returns Online)

```
Node C (holds transit data for Node A)
        │
        │  [Node A heartbeat received — A is back online]
        ▼
1. C queries: SELECT * FROM transit_store WHERE target_owner = 'A' AND status = 'HELD'

2. C appends HANDOFF_INIT to Routing Ledger:
   { initiating_node: 'C', target_node: 'A', data_count: 42, total_bytes: 87654321 }

3. FOR each transit entry:
   a. C sends HANDOFF_SEND message to A:
      { data_id, mime_type, raw_data, routing_ledger_entry_id }
   
   b. A writes to its primary store (via Change Ledger INSERT)
   
   c. A sends HANDOFF_ACK to C:
      { data_id, change_ledger_entry_id }
   
   d. C appends HANDOFF_ACK to Routing Ledger (echoes A's confirmation)
   
   e. C updates transit_store: SET status = 'COMPLETE' WHERE data_id = ?
   
   f. C deletes from transit_store (only after HANDOFF_ACK confirmed on ledger)
   
   g. C updates transit_quota: used_bytes -= size_bytes

4. After all entries drained:
   C appends HANDOFF_COMPLETE to Routing Ledger:
   { initiating_node: 'C', target_node: 'A', data_count: 42, duration_us: ... }
```

> **Invariant**: Transit entries are only deleted AFTER a `HANDOFF_ACK` is recorded on the Routing Ledger. If C crashes mid-handoff, on restart it can replay the Routing Ledger, find entries with `status = 'HANDOFF_IN_PROGRESS'` without a corresponding `HANDOFF_ACK`, and retry the handoff.

---

## 9. Node Profile (Extended `node_profile.toml`)

```toml
[node]
id          = "B"
level       = 1               # 0=ROOT, 1=SPECIALIST, 2=GENERALIST
display_name = "CSV & Tabular Specialist"

[capabilities]
# MIME type group → capability_score (0.0–1.0)
"text/csv"            = 1.00   # Primary specialization
"text/xml"            = 0.95
"application/json"    = 0.80
"application/x-ndjson"= 0.75
"text/plain"          = 0.40
"application/*"       = 0.15
"*/*"                 = 0.05   # Catch-all: always accept, very low priority

[storage_engine]
primary = "columnar"           # columnar | blob | text_fts | generic
options.compression = "lz4"
options.schema_inference = true
options.column_index = true

[transit]
quota_mb        = 512          # Normal transit quota
emergency_expand = true        # Allow quota expansion (never reject)
emergency_cap_mb = 2048        # Hard ceiling on emergency expansion

[ranking_weights]
# Override default weights for this node's self-assessment
w_cap   = 0.45
w_load  = 0.25
w_perf  = 0.20
w_avail = 0.10
```

---

## 10. Brain File Extensions

The brain file gains two new sections to expose routing-relevant state to peers:

```json
{
  "node_id": "B",
  "specialization": {
    "level": 1,
    "capabilities": {
      "text/csv":         1.00,
      "text/xml":         0.95,
      "application/json": 0.80,
      "*/*":              0.05
    },
    "storage_engine": "columnar"
  },
  "transit_store": {
    "quota_bytes":      536870912,
    "used_bytes":       89478485,
    "load_ratio":       0.167,
    "emergency_active": false,
    "held_for_nodes": {
      "A": { "record_count": 42, "total_bytes": 10485760, "oldest_held_us": 1735000000000000 },
      "C": { "record_count":  0, "total_bytes": 0 }
    }
  },
  "routing_ledger_tip": {
    "entry_id": 304,
    "entry_hash": "c9f2a3..."
  }
}
```

The `routing_ledger_tip` lets every node verify its replica of the Routing Ledger is current, analogous to how `ledger_tip` works for the Change Ledger.

---

## 11. Storage Engines by Specialization

Each node loads a type-specialized storage engine in addition to the always-present generic transit backend.

| Engine | Specialization | Key Optimizations |
|---|---|---|
| `blob_columnar` | Images, video, audio | Content-addressable store (SHA-256 dedup), EXIF/ID3 metadata index, thumbnail sidecar, chunked storage for large files |
| `columnar` | CSV, XML, tabular, JSON | Schema inference on ingest, columnar layout (Apache Arrow-compatible), column-level LZ4/Zstd compression, SQL-like query via SQLite virtual tables |
| `text_fts` | Plain text, Markdown, HTML, logs | Inverted trigram index (full-text search), BM25 ranking, Zstd compression, line-addressable chunking |
| `dom_tree` | XML, JSON, YAML, TOML | DOM tree storage (msgpack-serialized), XPath/JSONPath index, schema version tracking, structural diff storage |
| `time_series` | Metrics, logs with timestamps | Time-partitioned append-only files, delta encoding, downsampling summaries, configurable retention |
| `generic` | `*/*` (fallback, transit) | Raw blob + opaque metadata, no parsing, SQLite-backed, used by ALL nodes for transit store |

The `generic` engine is always active and serves the transit store regardless of the node's primary engine.

---

## 12. Worked Example: Full Routing Trace (3-Node Demo)

**Scenario**: Agent writes a 2 MB JPEG. Node A (image specialist, ROOT) is offline. Node B (CSV specialist) is at 95% transit capacity. Node C (generalist) is online with 40% capacity.

```
Step 1: Agent writes to Node B (its local connection point)
        │
        ▼
Step 2: Classification pipeline
        Detected MIME: image/jpeg (confidence: 0.97)
        Routing Ledger entry: DATA_CLASSIFY
        │
        ▼
Step 3: Fetch ranked list for image/jpeg from Routing Ledger replica
        1. Node A  fitness=0.88  ← primary owner, image specialist
        2. Node C  fitness=0.31  ← generalist, partial capability
        3. Node B  fitness=0.09  ← CSV specialist, wrong type
        │
        ▼
Step 4: Try Node A (rank 1)
        Heartbeat check: A is OFFLINE
        Routing Ledger entry: SKIP_NODE { node_id:'A', reason:'offline' }
        │
        ▼
Step 5: Try Node C (rank 2)
        Heartbeat check: C is ONLINE ✓
        Transit capacity: 40% used — has space ✓
        │
        ▼
Step 6: Forward data to Node C
        Node C writes to transit_store:
          { data_id: 'f7a3...', target_owner: 'A', held_by: 'C', cascade_depth: 1 }
        │
        ▼
Step 7: Routing Ledger entries (written by coordinator B, relayed to C):
        CASCADE_STORE {
          data_id: 'f7a3...',
          original_target: 'A',
          held_by: 'C',
          cascade_depth: 1,
          reason: 'target_offline',
          skipped: [{ node:'A', reason:'offline' }],
          expires_never: true
        }
        │
        ▼
Step 8: Node A comes back online (4 hours later)
        C detects A's heartbeat
        │
        ▼
Step 9: Handoff protocol executes (§8.2)
        Routing Ledger entries:
          HANDOFF_INIT { C → A, count: 1 }
          HANDOFF_ACK  { data_id: 'f7a3...', change_ledger_entry: 2819 }
          HANDOFF_COMPLETE { C → A, duration_us: 380000 }
        │
        ▼
Step 10: Transit entry deleted from C after ACK confirmed
         Data now lives in A's primary store, indexed as image/jpeg
         Full routing history permanently on Routing Ledger: provable end-to-end
```

---

## 13. Open Questions (Deferred to Implementation Phase)

| Question | Current Answer | Future Consideration |
|---|---|---|
| Who writes Routing Ledger during coordinator downtime? | Ledger writes pause (acknowledged limitation) | Promote Level-1 node via election |
| How are performance scores initialized for new nodes? | `0.50` neutral score | Benchmark write during `NODE_REGISTER` |
| What if two nodes declare conflicting TYPE_CLAIMs for the same MIME? | First registered wins; second gets `0.80 × declared_score` | Negotiation protocol |
| Is `emergency_expand` truly unbounded below `emergency_cap_mb`? | Bounded by `emergency_cap_mb` in TOML | Disk space monitoring hook |
| How does a Level-2 node get promoted to Level-1? | Manual `LEVEL_CHANGE` by coordinator | Could be automated based on sustained high capability scores |
