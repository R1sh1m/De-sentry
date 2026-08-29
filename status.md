---
title: De-Sentry — MVP Build Status
updated: 2026-08-29 (round 2: comparison + ledger/brain/client/dashboard)
---

# De-Sentry: MVP build status

This captures what was actually built, tested, and demonstrated for the
De-Sentry decentralized P2P DBMS course project, as a durable record for
future sessions/teammates. The full design rationale lives in
`architecture.md` in this project; this doc is the "what happened" companion
to that "why we designed it this way" doc.

## What it is

A fully peer-to-peer database engine in C++17 — no client-server split.
Every node is simultaneously a storage engine, a local REST API server, and
an encrypted P2P peer. Handles both structured (schema-validated) and
unstructured documents through one unified CRDT document model. Built from
scratch the way real engines (InnoDB/Postgres) are: hand-written paged
storage, WAL, disk-backed B+Tree — not a wrapper around SQLite/RocksDB/LMDB.
Only third-party dependency: OpenSSL (Ed25519/X25519/AES-256-GCM via EVP).

## Binding constraints set by the team

- Language: C/C++ (matching how production DBMS engines are actually built)
- Storage engine: custom, from scratch — no embedded KV library
- Distributed-systems algorithms (consistency model, consensus/CRDT choice,
  peer discovery, sharding approach): left to be designed as part of this
  build, not pre-chosen
- Deployment target for the course deadline: local multi-node simulation —
  multiple real OS processes on one host forming a genuine P2P network, not
  full multi-machine/container orchestration

## Architecture at a glance

- **Storage engine**: 4 KiB paged file I/O, LRU buffer pool, write-ahead log
  (logical redo records, CRC32, fsync-before-ack durability), slotted-page
  record layout, disk-backed B+Tree (order ~50, real leaf/internal splitting).
- **Data/consistency model**: every document is a `CrdtValue` tree with
  per-field Hybrid Logical Clock metadata. LWW-Register for scalars,
  OR-Set for arrays, recursive per-key LWW-map merge for objects — a proven
  join-semilattice (commutative, associative, idempotent), formally tested.
  JSON-Schema-subset validation is applied only at the API boundary, not as
  a separate storage representation, so structured and unstructured data
  share one model.
- **Network layer**: Ed25519 node identity, ephemeral X25519 ECDH handshake
  (forward secrecy), HKDF-SHA256 per-direction session keys, AES-256-GCM
  authenticated transport. UDP broadcast discovery + static bootstrap-peer
  fallback. Eager broadcast for low-latency propagation, gossip
  anti-entropy (digest exchange + delta push/pull) as the correctness
  backstop for full-mesh convergence.
- **API**: dependency-free HTTP/1.1 server (thread-per-connection, raw
  POSIX sockets) exposing `PUT/GET/DELETE /db/:collection/:key`,
  `GET /db/:collection`, schema get/set, `/_collections`, `/_peers`,
  `/_status`.
- **Crash recovery**: full WAL replay on `Open()`, always rebuilding a fresh
  B+Tree per collection touched by the WAL rather than trusting the
  catalog's possibly-unflushed root page id (see below — this was a real
  bug found and fixed during development).

Full rationale, diagrams, and the security-model table are in
`architecture.md`.

## Verification performed

- **Unit/integration test suites** (assert-based, no external framework,
  `ctest`-driven): `crdt_test`, `crypto_test`, `storage_test`,
  `network_test` — all 4 pass with **live assertions**. (`CMakeLists.txt`
  explicitly compiles test binaries with `-UNDEBUG` after diagnosing that
  the default `RelWithDebInfo` build type's `-DNDEBUG` was silently
  turning every `assert()` into a no-op — every test was "passing" without
  checking anything until this was fixed.)
- CRDT properties formally verified: idempotence, commutativity,
  associativity of merge; field-level delete-vs-concurrent-update race
  resolved correctly; OR-Set concurrent-add-survives-unseen-remove; binary
  codec round-trip.
- Network layer verified over real sockets: full authenticated-encryption
  handshake, TCP transport over real loopback (including clean failure on
  an unreachable peer), and a real 3-node mesh (3 real `NodeEngine` +
  `NetworkManager` instances) converging correctly under **concurrent
  divergent writes to the same document**, plus gossip-only propagation
  for a write that only reaches one node directly.
- **Live end-to-end demo with compiled binaries as separate OS processes**
  (not just unit tests): started 3 real `desentryd` processes via
  `scripts/run_cluster.sh 3`, each on its own API/P2P port with its own
  data directory. Confirmed via `curl`:
  - `/_status` and `/_peers` on each node showing independent node
    identities and correct peer discovery (UDP + bootstrap).
  - A `PUT /db/users/u1` on node0's REST API replicated over the real
    encrypted P2P network and was independently readable via `GET` on
    node1's and node2's own REST APIs within ~1 second — the core
    "no client-server, every peer is both server and client" requirement,
    demonstrated live rather than just tested in isolation.

## Bugs found and fixed during development (worth remembering)

- **Crash-recovery bug**: `Catalog::Save()` wrote the B+Tree root page id
  immediately (outside buffer-pool/WAL durability), but the actual root
  page was never guaranteed flushed. A restart could read back a
  zero-filled page misread as a corrupt B+Tree node → segfault. Fixed by
  always rebuilding a fresh B+Tree per collection from WAL replay on
  `Open()`, rather than trusting the catalog's stored root page id.
- **Silently-disabled test assertions**: see `-UNDEBUG` note above — a real
  test (`TestDeleteVsUpdateRace`) then failed for a genuine, documented
  reason (two independent, un-synchronized `HybridLogicalClock` instances
  offer no ordering guarantee without an `Observe()` call — not an engine
  bug, a test-construction issue, fixed and explained in a code comment).
- Deliberately **not** re-broadcasting merged remote writes in
  `NodeEngine::MergeRemote()`, to avoid broadcast storms in a mesh with
  more than 2 nodes — gossip anti-entropy is the documented correctness
  backstop for multi-hop convergence in this MVP.

## How to build and run

See `README.md` in the delivered source archive for full instructions.
Short version:

```bash
mkdir build && cd build && cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo .. && make -j
ctest --output-on-failure
cd .. && ./scripts/run_cluster.sh 3   # then curl the REST API on ports 7701-7703
./scripts/stop_cluster.sh
```

## Known MVP-scope limitations (stated plainly, not glossed over)

- No page-level checksums to detect/repair a page torn mid-write during a
  B+Tree structural split (WAL protects documents, not yet raw page
  integrity at that granularity).
- Coarse-grained mutex on the B+Tree rather than latch-crabbing —
  documented tradeoff for MVP simplicity.
- Multi-hop relay beyond eager-broadcast's direct peers relies on gossip
  anti-entropy rather than flooding — correct but not the lowest-latency
  possible design for very large meshes.
- Deployment scope is local multi-node simulation (real processes, one
  host) per the course deadline decision, not multi-machine/container
  orchestration — the P2P protocol itself has no such restriction, this is
  a deployment-scope choice, not an architectural one.

## Round 2: comparison against a teammate's independent design, and what was merged in

A teammate produced a second, independently-developed De-Sentry design in
parallel (docs: `ARCHITECTURE.md`/`README.md`/`project_statement.md`, plus
`docs/routing_and_specialization.md`, `docs/node_capability_score.md`,
`docs/change_ledger.md`, `docs/sync_protocol.md`, `docs/node_design.md`,
`docs/api_spec.md`). Full comparison is `comparison.md` in this project;
summary of the outcome:

**Core fork**: their design avoids write conflicts by giving each node
exclusive ownership of a data partition, routed by a fitness-ranking
algorithm (hardware benchmarking + declared MIME-type capability + load +
availability) through a designated coordinator ("ROOT") node that also
maintains a second hash-chained "Routing Ledger." Their own docs are
explicit that this reintroduces a soft single point of failure ("Ledger
writes pause" if the coordinator is down) and uses physical-clock
last-write-wins for the rare shared-state path (acknowledged clock-drift
risk). This build's CRDT/HLC model already solves both of those problems
more generally — every node can write anything, ordering is causally
correct under clock skew, no coordinator exists to fail — so the ownership
+ routing + coordinator subsystem was **not adopted**, with the reasoning
recorded in `comparison.md` §2 rather than silently ignored.

**What *was* adopted**, because it was genuinely complementary rather than
competing with the CRDT model:

1. **Hash-chained, Ed25519-signed audit ledger.** Their Change Ledger
   design (SHA-256 chain, `prev_hash`/`entry_hash`, genesis = zero bytes)
   is a clean idea independent of the ownership question. Implemented on
   top of the existing WAL (`storage/wal.h`/`.cpp`): every record now
   chains from the previous one, `WriteAheadLog::VerifyChain()` does a
   full independent re-verification, and the chain tip is additionally
   **signed with the node's existing Ed25519 identity key**
   (`NodeEngine::SignLedgerTip()`) — closing a gap their own docs flagged
   as future work ("no entry signing... production fix: sign each entry
   with Ed25519"). New REST endpoints: `GET /_ledger/tip`,
   `GET /_ledger/entries?from=&to=`, `POST /_ledger/verify`. New test
   coverage: `TestWalHashChain` in `tests/storage_test.cpp` (genesis
   linkage, chain continuity across a restart, tamper detection on an
   already-committed record).
2. **Brain-file-style status endpoint.** `GET /_brain` — a single-call,
   compact snapshot (signed ledger tip, per-collection live document count
   + a deterministic checksum, known peers), matching their "brain file"
   concept's intent (fast whole-node awareness for an agent or operator)
   without duplicating their separate push/cache/TTL sync protocol
   alongside the existing gossip layer.
3. **Zero-dependency Python client**, `clients/python/desentry_client.py`
   — covers their stated audience (AI agent code) more cheaply than a
   native pybind11 binding would: pure stdlib (`urllib`/`json`), no compile
   step, works against a node on any host since it's just HTTP. Includes a
   `Consortium` helper for checking several nodes at once (e.g. "do all
   ledger tips verify?"). Smoke-tested end-to-end against a live node.
4. **`tools/dashboard.html`** — a dependency-free single HTML file (no
   Electron/React/build step) giving the operator-facing visibility their
   design's Electron dashboard aimed for: live status, signed ledger tip,
   per-collection checksums, peers, a scrolling ledger table, one-click
   ledger verification. Required one small, independently useful engine
   change to work as a static page calling the API from the browser: the
   HTTP server now sends CORS headers and answers preflight `OPTIONS`
   (`src/api/http_server.cpp`) — safe because the API is loopback-only by
   default with no session auth to leak.

All four items, plus the full reasoning for what was deliberately left
out (ownership partitioning, the 3-level node hierarchy, physical-clock
LWW, type-specialized storage engines, hardware capability benchmarking),
are written up in `comparison.md`. The type-aware data-*placement* idea
(as opposed to conflict resolution) is flagged there as legitimate future
work, not rejected outright — see its final section.

All 4 test suites (`crdt_test`, `crypto_test`, `storage_test` — now
including the new hash-chain test, `network_test`) still pass 100% with
live assertions after these changes, and every new endpoint was verified
against a live running `desentryd` process, not just unit-tested.

## Deliverables

- Full C++17 source tree (buildable with CMake, zero fetched dependencies
  beyond OpenSSL), delivered to the team as `desentry-mvp.tar.gz`
  (round 2: now includes the audit ledger, `/_brain`, the Python client,
  and the dashboard).
- `architecture.md` (this project) — full design document, updated with
  the ledger/brain-file section (§7.6).
- `comparison.md` (this project) — the design comparison against the
  teammate's consortium/routing design and what was adopted from it.
- This status doc.