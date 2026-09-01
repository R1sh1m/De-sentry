# Design Comparison: This Implementation vs. the Consortium/Routing Design

This document compares the P2P database engine actually built in this repo
against a second, independently-developed De-Sentry design (docs:
`architecture.md`/`README.md`/routing/ledger specs, prototype scripts under
`scripts/dsync.*`) produced in parallel by a teammate for the same course
project. Both start from the same problem statement (`Project_Statement/project_statement.md`:
give each AI agent its own sandboxed database node so agents stop hammering
one shared instance) but land on different architectures. This doc records
what each design actually guarantees, which ideas from the second design
were adopted here (and how), which were deliberately not adopted (and why),
and what changed in this codebase as a result. It's written to stand
alongside `architecture.md`, not replace it.

## 1. The core architectural fork

The two designs answer "how do N nodes avoid write conflicts?" in opposite
ways.

**This implementation (CRDT / symmetric replication).** Every node can
accept a write to any document. Conflicts are resolved automatically and
deterministically by CRDT merge (LWW-Register / OR-Set / recursive
per-key-map, ordered by Hybrid Logical Clock) — see `architecture.md`'s
consistency-model section. There is no data ownership, no coordinator, and
no routing decision: a write lands on whichever node the application is
talking to, and P2P gossip/eager-broadcast propagates it to every other
node, which merges it into convergent state.

**The consortium/routing design (ownership partitioning + coordination).**
Each node **owns** an exclusive data partition; no other node ever writes
to it. Conflicts are avoided by construction rather than resolved after the
fact. A **fitness-ranking algorithm** (`original_design/routing_and_specialization.md`,
`original_design/node_capability_score.md` in the teammate's package) decides which
node *should* own a given piece of data based on declared MIME-type
capability, current load, historical write performance, and a full
hardware benchmark (disk/CPU/memory/network). A **Routing Ledger** — a
second hash-chained log, written by a designated coordinator (ROOT) node —
records every routing decision, cascade, and handoff. Rare shared-state
writes go through that coordinator with last-write-wins-by-physical-clock
resolution.

Both are legitimate, well-reasoned answers to the same problem, and both
documents are explicit and honest about their own trade-offs (the
consortium design's own `architecture.md` Sec 9 lists "no distributed
mutual exclusion," "reintroduces a soft dependency on one node," and
"clock drift... acknowledged" as stated limitations). The comparison below
is about which trade-offs are the right ones for this project, not about
either design being sloppy.

## 2. Why this implementation keeps the CRDT model as the core

Three concrete guarantees the consortium design's own docs list as
*explicit, acknowledged gaps* are things the CRDT model in this repo
already solves, not roadmap items:

| Gap the consortium design names as acknowledged/out-of-scope | Status in this implementation |
|---|---|
| "No distributed mutual exclusion... last-write-wins by timestamp is used instead. Clock drift is acknowledged." (their `architecture.md` Sec 9.1) | Every write is CRDT-merged with Hybrid Logical Clocks, not physical-clock LWW. HLC gives causal ordering across nodes even under clock skew (`crdt/hlc.h`; `NodeEngine::MergeRemote` calls `clock_->Observe()` on every remote timestamp specifically to preserve this). Field-level delete-vs-concurrent-update races and OR-Set concurrent-add-vs-remove races are both formally tested (`tests/crdt_test.cpp`), not just asserted in prose. |
| "This reintroduces a soft dependency on one node [the ROOT/coordinator]... ROOT failure [needs] a Raft-based leader election." (their `original_design/routing_and_specialization.md` Sec 3, "Acknowledged limitation") | There is no coordinator node in this implementation, for any write path — shared-state or otherwise. Every node is symmetric. Nothing to elect, nothing that becomes unavailable if one node goes down. |
| "Who writes Routing Ledger during coordinator downtime? Ledger writes pause (acknowledged limitation)." (their `original_design/routing_and_specialization.md` Sec 13, Open Questions) | Not applicable — there's no routing decision requiring a single writer to serialize. |
| "No network partition or node-failure handling... behavior during a dropped connection or offline node is not implemented." (`Project_Statement/project_statement.md` Sec on Assumptions/Limitations) | Handled by design, not left open: eager broadcast tolerates individual send failures per-peer (one peer being down doesn't block delivery to the others), and gossip anti-entropy is the correctness backstop that catches up any peer that was offline when a write happened, once it reconnects — proven in `tests/network_test.cpp`'s three-node convergence test, not just asserted. |

The reason this isn't "our design is just better" one-upmanship: the
consortium design's ownership model exists specifically to make the CRDT
problem *disappear* ("since each node would contain entirely unique files,
cross-node conflicts are non-existent" — `Project_Statement/project_statement.md`). That's a
reasonable simplification if building real conflict resolution is out of
budget. This project already had a working, tested CRDT layer before this
comparison was done, so the trade-off that motivated ownership partitioning
doesn't apply here — adding ownership + a coordinator on top would
subtract guarantees (introduce a soft single point of failure, weaken
consistency to physical-clock LWW for the coordinator's namespace) without
buying anything back, since multi-writer conflicts are already handled
correctly. **Decision: not adopted.** The routing/fitness-ranking/hierarchy
system (Secs 3–8 of their `original_design/routing_and_specialization.md`) is a
sophisticated piece of design work, and the right home for it is as an
optional data-*placement* layer on top of the existing replication model
(see Sec 5, "Deferred, not rejected," below) — not a replacement for
conflict resolution.

## 3. Ideas adopted from the consortium design

Three ideas were genuinely complementary — they don't conflict with the
symmetric-replication model, they make it stronger — and have been
implemented in this codebase as a direct result of this comparison.

### 3.1 Hash-chained tamper-evident ledger

The consortium design's Change Ledger (`original_design/change_ledger.md`) is a clean
idea independent of the ownership question: every mutation gets a SHA-256
`entry_hash` chained from the previous entry's hash (`prev_hash`), so any
post-hoc alteration, reordering, or deletion of a past entry is detectable
by recomputing the chain — a Git-commit-style audit trail. This repo's WAL
(`storage/wal.h`) already had CRC32-per-record framing for torn-write
detection, but that only protects against a crash mid-write, not
after-the-fact tampering with an already-committed file. The hash chain is
a genuinely different, complementary guarantee.

**Adopted, with one closed gap.** `storage/wal.h`/`wal.cpp` now compute a
SHA-256 `entry_hash` per record chained from `prev_hash`, with a genesis
record chaining from 32 zero bytes — same design as their spec. New:
`WriteAheadLog::VerifyChain()` does a full independent re-verification
(not a cached "trust the tip" shortcut), and `WriteAheadLog::Tip()` exposes
the current chain head. Exposed over REST as `GET /_ledger/tip`,
`GET /_ledger/entries?from=&to=`, and `POST /_ledger/verify` (see
`README.md`'s endpoint table). Covered by a new test,
`TestWalHashChain` in `tests/storage_test.cpp`, which checks genesis
linkage, chain continuity across a process restart, and that tampering
with an already-committed on-disk record is caught.

Their own `original_design/change_ledger.md` Sec 8 names an acknowledged gap: *"No
entry signing... a node could regenerate a full chain from scratch and
substitute it. Production fix: sign each entry with the node's private key
(Ed25519)."* This repo already has an Ed25519 identity per node
(`net/identity.h`, used for the P2P handshake) — so rather than defer
that fix, `NodeEngine::SignLedgerTip()` signs the current chain tip
(`entry_id` + `entry_hash`) with the node's persistent Ed25519 key today,
exposed as the `signature` field on `GET /_ledger/tip`. A hash chain alone
proves internal self-consistency; the signature additionally proves *this
specific node's identity* attests to *this exact* tip, closing the gap
their design explicitly deferred.

### 3.2 Brain file → `GET /_brain`

The "brain file" concept (their `original_design/node_design.md` Sec 4, `README.md`'s
"Three Core Pillars") — a compact snapshot a node publishes so peers (or,
notably for this project's stated audience, an *AI agent* orchestrating
several nodes) get situational awareness without expensive per-node
queries — is a good idea independent of the ownership question. This
implementation already had `GET /_status` (identity, uptime, counts) and an
internal digest mechanism used for gossip anti-entropy, but nothing
brain-file-shaped: a single call giving collection-level detail plus the
ledger tip.

**Adopted.** `GET /_brain` (see `src/api/routes.cpp`) returns node_id,
generation timestamp, the signed ledger tip, a per-collection summary
(`NodeEngine::Summarize()`: live document count + a deterministic SHA-256
checksum over every live key's HLC timestamp, sorted — so two converged
nodes produce an identical checksum for the same collection, a cheap way
to answer "have these two nodes converged?" without diffing the actual
documents), and the known-peer list. This is deliberately a pull-on-demand
endpoint rather than their push/cache/TTL brain-file exchange protocol
(`original_design/sync_protocol.md`) — this implementation's gossip layer already
handles active peer-to-peer state exchange (`net/gossip.h`); `GET /_brain`
adds the missing "one call, human- or agent-readable snapshot" surface on
top of that, without duplicating a second sync mechanism alongside gossip.

### 3.3 Python client for the AI-agent audience

Both `Project_Statement/project_statement.md` and the consortium `README.md` frame the
primary consumer as AI agent code, and the consortium design's answer is a
native `pybind11` binding (`original_design/node_design.md` Sec 7, `original_design/api_spec.md`
Sec 5's Electron/N-API bridge) requiring a compiled extension per platform.
This implementation already exposes a full local REST API for exactly this
purpose (`architecture.md` Sec 3.1: every peer serves both a P2P protocol
and a local application-facing API) — a native binding would be a second,
redundant way to reach the same functionality, with a real cost (a
build/compile step per agent machine/OS, and a native-ABI crash surface a
pure-HTTP client doesn't have).

**Adopted in the lower-cost form that fits this project's existing
surface.** `clients/python/desentry_client.py` — a zero-dependency,
pure-stdlib (`urllib`, `json`) client over the existing REST API. Covers
document CRUD, schema, `/_brain`, and the ledger endpoints, plus a small
`Consortium` convenience class for fanning a call out across several
nodes' clients at once (e.g. "do all nodes' ledger tips currently
verify?"). No native compile step, works unmodified against a node
running on another machine (a native in-process binding structurally
cannot), and was smoke-tested end-to-end against a live `desentryd`
process as part of this change.

## 4. A tool, not a feature: `tools/dashboard.html`

The consortium design's Electron + React dashboard (`README.md`'s tech
stack, `original_design/api_spec.md` Sec 5) is a legitimate answer to "operators want
to see the ledger and node topology," but it's a large dependency (Electron
+ React + TypeScript + `node-addon-api` + a native-addon build step) for
what this project needs at MVP stage. `tools/dashboard.html` is a single
dependency-free static HTML file (no build step, no npm) that polls
`/_status`, `/_brain`, `/_peers`, and `/_ledger/entries` over plain
`fetch()` and renders node status, the signed ledger tip, collection
checksums, known peers, and a live ledger-entry table, with a one-click
"Verify ledger" action against `POST /_ledger/verify`. It needed one small,
independently useful engine change to work from a static file: the HTTP
server didn't send CORS headers, so a page opened as `file://` (or served
from a different origin) couldn't call it from a browser at all —
`http_server.cpp` now sends `Access-Control-Allow-Origin: *` (the API is
loopback-only by default and carries no session auth to leak, so this is
safe) and answers CORS preflight `OPTIONS` requests, which also unblocks
*any* other browser-based tool against this API in the future, not just
this one page.

## 5. Ideas deliberately not adopted, and why

- **Ownership partitioning + coordinator + fitness-ranking routing**
  (their `original_design/routing_and_specialization.md`, `original_design/node_capability_score.md`).
  Covered in Sec 2 above — the CRDT model already solves the problem this
  subsystem exists to avoid, more generally (any node can write anything,
  not just its own partition) and without the soft coordinator dependency
  their own docs flag as a limitation.
- **3-level node hierarchy (ROOT/SPECIALIST/GENERALIST) with a statically
  assigned coordinator.** Same reasoning — this implementation has no
  concept of a distinguished node; every peer is equal, which is what "no
  client-server model" (this project's original stated requirement) means
  taken literally. A hierarchy with a ROOT is a soft client-server model
  wearing P2P clothing.
- **Physical-clock last-write-wins for shared state** (their
  `original_design/sync_protocol.md` Sec 4.2, explicitly flagged by them as having a
  clock-drift correctness risk). Superseded by HLC-ordered CRDT merge,
  which this implementation already had.
- **Type-specialized storage engines per node** (`blob_columnar`,
  `columnar`, `text_fts`, `time_series`, etc. — their
  `original_design/routing_and_specialization.md` Sec 11). Not adopted for the MVP:
  this implementation's single generic paged storage engine (page manager +
  buffer pool + B+Tree, `architecture.md` Sec 5) already handles both
  structured and unstructured documents uniformly (Sec 4.1: JSON-Schema
  validation is applied at the API boundary, not baked into a separate
  storage representation), and specialized engines are a genuine
  performance optimization, not a correctness requirement — reasonable
  future work, not something the MVP needs to claim.
- **Hardware capability benchmarking (NCS)**
  (`original_design/node_capability_score.md`) — only meaningful as an input to the
  fitness-ranking routing system above; not adopted for the same reason
  that system wasn't.

## 6. Deferred, not rejected: where type-aware placement could still fit

The routing/fitness-ranking work is not wasted design effort even though
it wasn't adopted as a replacement for conflict resolution — it's a
legitimate answer to a *different* question this implementation doesn't
currently address at all: **where should a large binary blob physically
live**, as opposed to *how do concurrent writes to the same key converge*.
Today, every document (JSON or otherwise) replicates to every node that
gossips with it; there's no notion of "this node prefers to hold
image/jpeg data." A future iteration could layer an opt-in placement hint
(a node declares MIME-type affinity in its config; the API layer prefers
storing large blobs locally on an affine node and replicates a reference
rather than the full bytes to non-affine peers) *on top of* the existing
CRDT replication model, rather than instead of it — CRDT merge would still
own correctness for the reference/metadata document; the fitness-ranking
math from `original_design/node_capability_score.md` would decide placement, not
conflict resolution. Stated here as a roadmap item, not silently dropped.

## 7. Net effect on this codebase

Concretely, this comparison produced: a hash-chained, Ed25519-signed audit
ledger (`storage/wal.h`/`.cpp`, `engine/node_engine.h`/`.cpp`, new
`tests/storage_test.cpp` coverage); three new REST endpoints
(`/_ledger/tip`, `/_ledger/entries`, `/_ledger/verify`) plus `/_brain`;
CORS support in the HTTP server; a zero-dependency Python client
(`clients/python/desentry_client.py`); and a dependency-free HTML ops
dashboard (`tools/dashboard.html`). All of it was chosen because it made
an existing, already-correct design more auditable and more usable, not
because it was present in the other design — the parts of that design that
would have weakened this implementation's actual guarantees (a soft
coordinator dependency, physical-clock LWW) were identified and explicitly
left out, with the reasoning recorded above rather than silently ignored.
