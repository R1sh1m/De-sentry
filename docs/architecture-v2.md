---
title: De-Sentry — Architecture v2
updated: 2026-09-07
supersedes: nothing
extends: docs/architecture.md
---

# De-Sentry v2 — Architecture

This document covers what v2 adds. `docs/architecture.md` still describes the
engine core — the CRDT document model, the HLC, the paged storage engine, the
secure channel — and none of that changed. v2 was built as an extension, not a
rewrite: every v1 file that still exists still does what it did, and a v1 data
directory opens unchanged.

What v2 adds, in the order it is described here:

| § | Addition | Where it lives |
| --- | --- | --- |
| 1 | The shape of the whole thing | — |
| 2 | The desktop app and its sidecar | `app/` |
| 3 | App-local supervisors | `include/desentry/supervisor/`, `src/supervisor/` |
| 4 | The polyglot storage router | `include/desentry/storage/router.h`, `src/storage/engines/` |
| 5 | Ledger v2: hash chain, transit, change feed | `include/desentry/ledger/`, `storage/wal.h` |
| 6 | Placement, admission control, ACLs | `net/placement.h`, `net/admission.h`, `storage/catalog.h` |
| 7 | Onboard workload sizing | `app/src-tauri/src/ai.rs`, `app/resources/` |
| 8 | Threat model | — |
| 9 | What v2 deliberately does not do | — |

---

## 1. The shape of the whole thing

```
   ┌──────────────────────────────────────────────────────────────┐
   │  De-Sentry.app  (Tauri v2)                                   │
   │                                                              │
   │   webview (TypeScript, no framework)                         │
   │      │  invoke()                    fetch() → 127.0.0.1      │
   │      ▼                                    │                  │
   │   Rust sidecar                            │                  │
   │      · spawns and supervises desentryd    │                  │
   │      · allocates ports, writes node.json  │                  │
   │      · OS keychain, recovery keys         │                  │
   │      · ONNX workload sizing               │                  │
   │      · tray, autostart, power, volumes    │                  │
   └──────────────┬────────────────────────────┼──────────────────┘
                  │ spawn                      │ REST (loopback)
                  ▼                            ▼
        ┌──────────────────┐        ┌──────────────────┐
        │ desentryd        │        │ desentryd        │   …
        │ (supervisor)     │        │ (data node)      │
        │ loopback only    │        │ 7701 / 7801      │
        │ holds no data    │        │                  │
        └────────┬─────────┘        └────────┬─────────┘
                 │ control plane             │ data plane
                 │ (discovery, GC,           │ (flat, leaderless
                 │  lifecycle, custody)      │  CRDT gossip)
                 ▼                           ▼
        ── never on the write path ──   ═══ peers on the LAN ═══
```

The two planes are the whole design. The control plane is app-local, bound to
loopback, and optional at runtime — if every supervisor on the network is down,
writes, reads, replication and convergence continue exactly as before; only
garbage collection and hardware discovery pause. The data plane is what v1 was
and still is: flat, leaderless, converging by CRDT merge.

---

## 2. The desktop app

`app/` is a Tauri v2 application: a TypeScript webview over a Rust sidecar.

**The app is the single point of access.** Nobody hand-edits `node.json` and
nobody runs `desentryd` from a terminal in normal use. That is a product
decision with an engineering consequence: the config file format has exactly
one authority (`NodeConfig` in `include/desentry/common/config.h`), and
`app/src-tauri/src/configgen.rs` writes the field names that header defines —
no translation layer, no camel-case conversion, nothing that could drift.

### 2.1 What the sidecar owns

| Concern | Module | Note |
| --- | --- | --- |
| Process supervision | `nodes.rs` | A node is not "started" until it answers `/_status`; a PID proves nothing. |
| Port allocation | `ports.rs` | API from 7701, P2P from 7801, discovery shared on 7901. Allocation *binds* to test, because reading a listener list races. |
| Config generation | `configgen.rs` | Atomic write-then-rename. Never writes a secret. |
| Key custody | `keychain.rs`, `recovery.rs` | Windows Credential Manager / macOS Keychain / Linux Secret Service. No escrow (§8.3). |
| Workload sizing | `ai.rs` | ONNX + a deterministic fallback that says it is one (§7). |
| Tray and background | `tray.rs` | Closing the window hides it; nodes keep syncing. |
| Power and volumes | `power.rs` | Battery throttles gossip; a volume change re-scans. |

### 2.2 Restart policy

A crashed node is restarted with exponential backoff, and after five restarts
inside two minutes it is left stopped with the reason. A node that crashes on
startup — a corrupt data file, a port genuinely held by something else — would
otherwise be restarted forever, burning CPU and filling the log with one
repeated failure.

### 2.3 The webview

No UI framework, on purpose. The engine's whole discipline is zero fetched
dependencies; a control room whose UI layer is 200 lines of `createElement` is a
control room nobody has to audit. Live updates come from one long poll per node
over `GET /_changes?since=`, not from a timer.

`DESIGN.md` covers the visual system and the five screens.

---

## 3. App-local supervisors

A supervisor is an ordinary `desentryd` process started with
`"supervisor": true`. What makes it a supervisor is entirely what it does —
and, more importantly, what it is structurally prevented from doing:

* **It binds its API to loopback.** `NodeConfig::Validate()` rejects any other
  bind address for a supervisor, so this is enforced at config load rather than
  documented and hoped for.
* **The placement layer skips it** (`net/placement.cpp`), so no key is ever
  assigned to it. It holds no replicated data.
* **Gossip skips it**, so it is never a replication hop.
* **It is never elected, and nothing waits on it.**

### 3.1 Why this is not a coordinator

`docs/comparison.md` §2 records why a ROOT/coordinator node was deliberately
not adopted: it reintroduces a soft single point of failure on the write path,
and the CRDT model exists precisely so there does not have to be one.

A supervisor is not a coordinator because it is not on the write path at all.
The test for this is simple and worth stating: **kill every supervisor and the
mesh must be functionally unchanged.** Writes, reads, replication, convergence
and conflict resolution all continue. What stops is garbage collection,
hardware discovery, and the app's ability to show you a topology — control-plane
conveniences, not data-plane guarantees.

### 3.2 Responsibilities

1. **Hardware discovery** — a depth-limited folder scan for candidate data
   directories, removable-mount detection, and LAN peer enumeration. Read-only
   and side-effect free: a scan of the wrong directory tree is harmless.
2. **Fitness ranking** — `PeerFitness::Score()` over latency, success rate,
   ledger freshness and free quota, feeding gossip peer selection.
3. **Placement** — consistent hashing at RF=3 (§6.1).
4. **Node lifecycle** — `discovered → allocated → provisioned → running →
   degraded → reclaimed`, with invalid transitions rejected rather than
   silently accepted. `reclaimed` is terminal.
5. **Key custody** — recording *that* a recovery key was exported and when.
   Never the key.
6. **Quota enforcement** — marking a node degraded past 90% of budget, so the
   app can warn before writes start failing rather than after.
7. **Checkpoint and GC** — the quorum-gated ledger prune (§5.3).

---

## 4. The polyglot storage router

v1 had one physical shape for every collection: slotted pages indexed by a
B+Tree. That is the right general-purpose shape and it remains the default. But
a twenty-million-point sensor stream, a 384-dimension embedding table and a
parent/child object tree each pay a real cost for being stored as independent
key/value documents.

`StorageRouter` lets a collection be *bound* to a backend whose physical layout
matches its access pattern, without anything above the router knowing which
backend it got.

### 4.1 The two invariants that make this safe

1. **Every backend speaks CRDT bytes.** `Put`/`Get`/`Scan` move
   `CrdtValue::Encode()` blobs and `MergeRemote()` is implemented in terms of
   `CrdtValue::Merge` on all of them. Convergence therefore does not depend on
   which backend a peer happens to have bound a collection to — *two peers can
   disagree about the binding and still converge*, because the merge semantics
   live in the document type, not the storage layout.
2. **Every backend answers `Checksum()` the same way.** The hex digest is
   computed over the same sorted (key, top-HLC) fingerprint list, so `/_brain`
   parity between two peers is a statement about data, never about layout.

`tests/router_test.cpp` runs one identical six-part contract over every
from-scratch backend for exactly this reason. A per-engine test written to each
engine's strengths would pass while the promise quietly broke.

### 4.2 The engines

**From scratch, always compiled.** These reuse the existing DiskManager /
BufferPoolManager / WAL / HLC primitives rather than opening private file
formats behind the engine's back.

| Engine | Shape | Technique |
| --- | --- | --- |
| `kv` | general purpose | the v1 B+Tree over slotted pages |
| `columnar_lite` | scans and aggregates | front-coded keys, zigzag varints, byte-run RLE over paged segments |
| `ts_rollup` | time series | hour chunks with precomputed downsampled rollups at the front of each chunk, plus retention |
| `vector_hnsw_lite` | similarity | HNSW-lite (M=16, efSearch=64), exhaustive below 1000 vectors, optional int8 quantisation |
| `graph_adj` | graphs and objects | adjacency index derived from `parent`/`children`/`edges`, plus an object view |

**Vendored OSS backbone, opt-in.** SQLite (public domain), DuckDB (MIT), LMDB
(OpenLDAP), sqlite-vec (Apache-2.0/MIT). Compiled in only when their sources
are present under `third_party/`, gated behind `DESENTRY_WITH_*`. CMake
**hard-fails** if an option is enabled without the sources — it never
downloads. A tree with no `third_party/` still builds and runs completely; that
is the zero-fetched-dependencies requirement, and it is why the from-scratch
family is not a demo.

Licence exclusions, deliberate: no Xapian (GPL), no TDengine (AGPL, whose
network clause would reach a peer-to-peer daemon), and no
ClickHouse/QuestDB — those are servers, and a server dependency would break the
"one desktop app, no daemon to install" property the product is built around.

`GET /_engines` reports what this binary actually has, which build option would
add the rest, and which are active on this node. The binary is the authority,
not a table in a document.

### 4.3 Budget

The node's quota is split by `QuotaSplit` (db 60 / transit 15 / cache 10 /
ledger 10 / net buffers 5 by default). The data-plane share is then divided
**evenly across active engines**, so a node with five engines cannot use five
times its quota.

One exception, and it matters: **a replication merge is never refused for
quota.** `engine_common.h` grants merges a 5% overdraft. A node that rejected a
peer's merge because it was full would stop converging permanently — that is a
correctness failure, not backpressure.

---

## 5. Ledger v2

v1's WAL was a durability log. v2 evolves it into a hash-chained,
signature-carrying CRDT ledger, without changing what it does for durability.

### 5.1 The entry

```
  entry {
    op          PUT | DEL | CHECKPOINT | TRANSIT_INTENT | TRANSIT_CLAIMED
    key_hash    SHA-256(collection || 0x00 || key)
    hlc         hybrid logical clock reading
    prev_hash ──┐ chain: entry_hash = H(prev_hash || fields)
    entry_hash ─┘
    origin      node_id + Ed25519 signature
  }
```

The `0x00` separator is not decoration: without it `("ab","c")` and `("a","bc")`
hash alike, and two different documents would share a ledger identity.

All nodes converge on the *set* of hashes. The raw bytes for a document live
wherever the document lives; the ledger says what happened, not what it said.

**v1 → v2 migration.** A v1 WAL is re-derived into a v2 chain on first open,
and the pre-migration tip is recorded in `ledger_migration.json` so the
discontinuity is auditable rather than silent.

### 5.2 Transit: writes for a node that is not there

```
   write for C, while C is offline
        │
        ▼
   replicas store bytes in the _transit TTL collection
        │  and append TRANSIT_INTENT to the ledger
        ▼
   C returns, replays GET /_ledger/entries?from&to
        │  pulls the bytes it is missing
        │  applies them (ordinary CRDT merge)
        ▼
   C appends TRANSIT_CLAIMED
        │
        ▼
   a supervisor checkpoints and collects the settled pair
```

`_transit` is never listed as a collection by any API. It is bookkeeping.

### 5.3 The checkpoint gate

Checkpointing is the one operation in the system that **deletes history**, so
it is gated twice:

1. **Quorum.** `2f+1` matching, self-verified tips with `f = (rf-1)/2` — at
   RF=3 that is all three replicas. Unanimity among the odd core rather than a
   bare majority, because the cost of waiting is retained garbage and the cost
   of being wrong is deleted history.
2. **No unclaimed intents.** Every `TRANSIT_INTENT` at or below the checkpoint
   must have a matching `TRANSIT_CLAIMED`. Pruning an unclaimed intent would
   throw away a write an offline node has not collected — data loss dressed up
   as garbage collection.

Three refusal cases are distinguished, and this distinction is the point:

* **A conflicting tip** (same height, different hash) **aborts entirely.** Two
  nodes have different histories, and pruning would destroy the evidence needed
  to work out why.
* **A lagging peer is not a conflict.** A node at entry 80 when the tip is 100
  has simply not caught up. Treating that as a conflict would mean a mesh with
  any gossip latency could never checkpoint.
* **Falling short of quorum lowers the checkpoint to a safe prefix** rather
  than stalling.

An unverified replica is not a vote. A node reporting the right hash without
having verified its own chain is asserting agreement it has not checked;
counting it would let a corrupt ledger authorise deleting everyone's history.

### 5.4 The change feed

`GET /_changes?since=` is a long poll over the ledger tip, mirroring OrbitDB's
update events. The app holds one per node; a write anywhere in the mesh that
reaches this node's ledger returns the poll within its round trip, with no
polling timer at all.

`truncated: true` means the caller's cursor predates a prune. Clients must drop
the cursor and re-read rather than pretending continuity across a gap that is
really there — the app does, `desentry_client.follow()` does, and
`tools/dashboard.html` does.

---

## 6. Placement, admission, ACLs

### 6.1 Placement

Consistent hashing with 160 virtual nodes per peer, RF=3. `HashToRing` is the
first eight bytes of a SHA-256 read big-endian — fixed, so every node on every
platform computes the same replica set for the same key.

A shard key co-locates: with one set, the hash input is the document's value
for that field, so every document sharing a tenant or a device lands on the
same replicas.

Excluded from the ring, each with the exclusion *recorded* so the UI can
explain it: supervisors, reclaimed nodes, degraded nodes, nodes not seen within
30 seconds, and bootstrap placeholders.

Under-replication is reported, not hidden. Two nodes cannot hold three copies,
and `PlacementPlan::under_replicated` says so rather than pretending RF=3 was
achieved.

### 6.2 Admission control

`docs/architecture.md` §8 flagged per-peer rate limiting as required before any
non-loopback deployment. v2 implements it rather than leaving it a note:

* **Token bucket per peer**, checked *before* decoding — a limiter that runs
  after parsing is a limiter that can be exhausted by malformed input.
* **Message-ID dedup**, FIFO-evicting at 8192 entries. IDs include random
  bytes, so they are unique across restarts.
* **Bounded worker pool** with drop-on-overflow and a counter, so a fifty-node
  fan-out cannot spawn fifty detached threads per write.

### 6.3 Per-collection ACLs

`CollectionAcl { owner_node, is_private, readers[], parent }` on
`CollectionMeta`. A collection with no ACL is shared with the mesh — that is the
CRDT default and making it implicit is deliberate. Private means: reads for the
owner plus named readers, writes for the owner alone.

Enforced in **three** places, because any one of them alone would be a hole:

1. `routes.cpp` on PUT/GET/DELETE.
2. `NodeEngine`'s CRUD path and `ReadableCollections`, so a refused read is
   `NotFound` rather than `Forbidden` — an error that distinguishes "you may
   not read this" from "this does not exist" tells the stranger the collection
   is there.
3. **The gossip byte filter.** A peer that is not a reader receives hashes, not
   bytes. An ACL enforced only at the API while replication shipped the
   documents anyway would be theatre.

Inheritance walks `parent` to a depth of 16 and **fails closed** on a cycle.
Malformed metadata must never be a permission.

---

## 7. Onboard workload sizing

```
   description
       │  all-MiniLM-L6-v2, 384-dim, 256 word-piece truncate, Apache-2.0
       ▼
   embedding ──cosine──► 7 prototype embeddings
       │                 (sql, nosql-doc, time-series, vector,
       │                  graph, semi-structured, oops-rdbms)
       ▼
   softmax ──► top-1 + confidence
       │
       ▼
   NodeSpec { engines[], quota_split, shard_key, RF,
              secondary_indexes, retention_days }
   + draft collections + PUT /_schema payloads, for a human to confirm
```

Everything runs on this machine. The model is bundled in the installer, the
ONNX Runtime is loaded from app resources, and no request leaves the process.

Three deliberate properties:

**Prototypes are embedded at load time, with the same model.** Checked-in
vectors would be faster and would become silently meaningless the first time
the model was replaced or re-quantised, with nothing to report it.

**Confidence is a margin, not a similarity.** Cosine between short English
sentences sits in a narrow band, so reporting one as confidence would read 0.7
for a coin flip. The seven scores go through a softmax at T=0.05 and the
reported number is the winner's share: 1/7 when the description says nothing,
approaching 1 when one shape clearly wins. **Below 0.35 the wizard stops
proposing and makes the user pick engines.**

**The fallback is honest.** If the model or runtime cannot load, a
deterministic keyword heuristic runs — whole-word matched, so "paragraph" does
not score "graph" — and the decision reports `method: "keyword"` with the
reason. The wizard shows it. A fallback presented as a model result would be
worse than no model.

The decision, its confidence and every prototype score are written into the
node's `manifest.json`, so a node that travels on a USB stick carries the
reason it is shaped the way it is.

---

## 8. Threat model

### 8.1 What is defended

| Threat | Defence |
| --- | --- |
| A peer forging another's writes | Ed25519 identity, verified in the handshake; `Requestor` is never taken from a caller-settable header |
| Reading data in flight | Ephemeral X25519 → HKDF → AES-256-GCM per session |
| Rewriting history | Hash chain + per-entry origin signatures; `POST /_verify` re-derives the whole chain |
| One node deleting shared history | Quorum-gated checkpoint (§5.3) |
| Reading a private collection | Per-collection ACL at API *and* gossip byte filter (§6.3) |
| Flooding a peer | Token bucket before decode, dedup, bounded worker pool (§6.2) |
| Reading a stolen disk or USB stick | Per-partition AES-GCM at rest; key in the OS keychain, or a password-KDF for removable nodes |
| Reading a stolen `identity.key` | Owner-only permissions: `0600` on POSIX, a real protected DACL on Windows |

### 8.2 What is not defended

Stated plainly, because a threat model that claims everything is a threat model
nobody checks:

* **A malicious peer inside the mesh.** Anyone holding a valid identity that a
  user has paired can write to shared collections and will converge with
  everyone else. Pairing is the trust boundary; there is no reputation system
  and no Byzantine agreement.
* **Traffic analysis on the LAN.** Sizes and timings are visible even though
  contents are not.
* **A compromised machine.** If an attacker has the user's account, they have
  the keychain, and the at-rest encryption is moot.
* **Denial of service by a paired peer.** Rate limiting bounds the damage per
  peer; it does not stop a peer you trusted from being noisy.

### 8.3 No escrow

A node's at-rest key exists in exactly two places: the OS keychain on the
machine that created it, and wherever the user put the recovery key when the
wizard showed it — once. Nothing in the app, in the supervisor's registry, or
on the node's disk can reproduce it. The supervisor records only *that* an
export happened and when.

This is a deliberate and costly choice. A user who loses both copies loses the
data. The alternative — a copy the app can reach — is a copy an attacker who
reaches the app can reach, which would make the encryption decorative. The
wizard therefore refuses to finish until the key has actually been exported,
and says why on the screen rather than in a footnote.

Removable nodes get no keychain entry at all: a stick that unlocks from the
keychain of the machine that made it is a stick that cannot be read anywhere
else, which defeats the point of putting a node on a stick.

---

## 9. What v2 deliberately does not do

* **No coordinator, no election, no consensus on the data path.** §3.1.
* **No escrow.** §8.3.
* **No per-peer rate-limit bypass.** There is no trusted-peer exemption; the
  bucket applies to everyone, because the peer most able to flood you is the
  one you already paired with.
* **No engine that is not in the binary.** The engine picker reads
  `GET /_engines`; an option that fails on selection is worse than one that is
  not offered.
* **No network at run time.** The one build step that fetches anything
  (`npm run fetch-model`) is a build step, and what it fetches is bundled into
  the installer. `tests/integration/airplane_mode_test.py` asserts that no node
  process opens a connection outside the machine or LAN.
* **No UI framework, no charting library, no QR library.** The QR encoder in
  `app/src/util/qr.ts` is ISO/IEC 18004 byte mode implemented directly, checked
  against the standard's published format and version bit strings in
  `app/tests/qr.check.mjs`.

---

## 10. Where things live

```
include/desentry/
  common/     platform.h ← the ONLY file allowed to #ifdef _WIN32
              config.h   ← the ONE authority on node.json's shape
  storage/    router.h, catalog.h, wal.h, segment_store.h
              engines/   kv_bplus, columnar_lite, ts_rollup,
                         vector_hnsw_lite, graph_adj, vendored_backends
  ledger/     transit_store.h, checkpoint.h, change_feed.h
  net/        placement.h, admission.h, peer.h, gossip.h, …
  supervisor/ supervisor.h
  engine/     node_engine.h
  api/        routes.h, http_server.h

app/
  src/        webview: api.ts, bridge.ts, state.ts, views/, util/
  src-tauri/  sidecar: nodes.rs, ports.rs, configgen.rs, ai.rs,
              keychain.rs, recovery.rs, tray.rs, power.rs, commands.rs
  resources/  prototypes.json (checked in), model/ (fetched)

tests/          C++ suites, run by ctest
tests/integration/  Python acceptance tests, run by hand
```
