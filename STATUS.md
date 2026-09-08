---
title: De-Sentry — build status
version: 2.0
updated: 2026-09-07
---

# De-Sentry v2: build status

What exists, what was verified, **what was not verified**, and what is
knowingly unfinished. The design rationale lives in `docs/architecture.md`
(v1 engine) and `docs/architecture-v2.md` (supervisors, storage router,
ledger v2, threat model); `AGENTS.md` is the how-to-work-here companion.

> **Read this first — what has actually been run.** The engine now builds and
> runs: on Windows 11 with MSYS2 UCRT64 GCC 16.2, CMake 4.4.2 + Ninja and
> OpenSSL 3.6.4, `ctest` is **9/9 green** and four Python integration suites
> pass against real `desentryd` processes. The **Rust sidecar and the Tauri
> app have still never been compiled** (no cargo on that machine), and nothing
> has been built on Linux, macOS, or MSVC. [§3](#3-verification) lists
> exactly what was executed and what was not; treat anything not listed there
> as designed, not observed.

---

## 1. What is built

### v1 engine (unchanged foundation, still current)

A fully peer-to-peer C++17 database engine — no client-server split. Every
node is simultaneously a storage engine, a local REST API server, and an
encrypted P2P peer. Hand-written paged storage, WAL, disk-backed B+Tree.
CRDT document model over hybrid logical clocks, so every node can write
anything and ordering stays causally correct under clock skew. OpenSSL is the
only required third-party dependency (Ed25519 / X25519 / AES-256-GCM via EVP).

### v2 additions

| Area | What was added | Where |
| --- | --- | --- |
| **Desktop app** | Tauri v2 control room: sidebar tree, Tree ⇄ Mesh canvas, inspector, file explorer, 5-step creation wizard, pairing QR, tray background mode | `app/src/` |
| **Sidecar** | Rust process supervision: port allocation (API 770x, P2P 780x, discovery 7901), config generation, log capture, crash restart, autostart, notifications, power/battery awareness | `app/src-tauri/src/` |
| **Supervisors** | `supervisor: true` nodes — loopback-only API, never elected, never a replication hop. Hardware discovery, fitness ranking, placement, key custody, quota enforcement, node lifecycle | `src/supervisor/`, `include/desentry/supervisor/supervisor.h` |
| **Storage router** | Abstract `EngineBackend` + per-collection engine selection, quota enforcement and `Checksum` on every backend | `src/storage/router.cpp`, `include/desentry/storage/router.h` |
| **Engines (from scratch)** | `kv_bplus`, `columnar_lite`, `ts_rollup`, `vector_hnsw_lite`, `graph_adj` — always compiled, no external code | `src/storage/engines/` |
| **Engines (vendored, opt-in)** | SQLite, sqlite-vec, DuckDB, LMDB — `OFF` by default, CMake **fails** rather than downloads if enabled without sources present | `src/storage/engines/*_backend.cpp` |
| **Ledger v2** | Hash-only entries `{op, key_hash, HLC, prev_hash → entry_hash, Ed25519 sig}`; transit store with TTL; quorum-gated checkpoint GC; `GET /_changes?since=` long-poll | `src/ledger/` |
| **Placement** | Consistent hashing, 160 virtual nodes per peer, RF = 3 | `src/net/placement.cpp` |
| **Security** | Per-collection ACLs enforced at PUT/GET/DELETE **and** in the gossip byte filter; per-partition AES-GCM at rest keyed from the OS keychain; password-KDF unlock for removable nodes | `src/api/routes.cpp`, `src/net/gossip.cpp`, `app/src-tauri/src/keychain.rs` |
| **AI sizing** | all-MiniLM-L6-v2 (Apache-2.0) via an ONNX Runtime sidecar: description → embedding → cosine against 7 prototypes → engine + confidence. Confidence < 0.35 forces the manual picker. Deterministic keyword fallback when ONNX is unavailable. Decision + confidence recorded in the node manifest | `app/src-tauri/src/ai.rs`, `app/resources/prototypes.json` |
| **Recovery** | Crockford-base32 recovery keys, QR + print export, **forced** before a node can be created. No escrow | `app/src-tauri/src/recovery.rs`, `app/src/util/qr.ts` |

Approximate size: ~20.7k lines C++/headers, ~5.3k TypeScript, ~3.7k Rust,
~1.6k Python (tests + client).

### Explicit non-goals, honoured

- **No coordinator / ROOT election.** The data plane is flat P2P. Rationale in
  `docs/comparison.md` §2.
- **No GPL/AGPL/server dependencies.** No Xapian, no TDengine, no
  ClickHouse/QuestDB.
- **No key escrow.** The user's exported recovery key is the only path back.
- **No per-peer rate-limit bypass.** Token-bucket admission is in
  `src/net/admission.cpp`; it must stay on before any non-LAN deployment
  (`docs/architecture.md` §8).
- **The app is the single point of access.** Nothing asks a user to edit
  `node.json` or launch `desentryd`. `scripts/run_cluster.*` is the one
  deliberate exception, for engine development.

---

## 2. Test suites in the tree

C++ (`ctest`, one binary per file, assertions kept live by `-UNDEBUG`):

| Suite | Covers |
| --- | --- |
| `crdt_test` | merge idempotence/commutativity/associativity, delete-vs-update race, OR-Set, codec round-trip |
| `crypto_test` | Ed25519, X25519, AES-256-GCM, handshake |
| `storage_test` | pager, WAL, B+Tree, catalog, hash chain across restart, tamper detection |
| `network_test` | secure channel, TCP transport over loopback, 3-node convergence under divergent writes, gossip-only propagation |
| `router_test` | engine selection, per-engine round-trip, `Checksum` stability, columnar segment codec |
| `acl_test` | private-collection reads/writes, reader list, inheritance, gossip byte filter |
| `placement_test` | ring determinism, RF-3 replica sets, rebalance movement bounds |
| `ledger_v2_test` | entry chaining, signature verification, checkpoint quorum gate, conflicting-tip abort, lagging-peer-is-not-a-conflict |
| `quota_test` | per-engine `quota_mb` enforcement and the quota split |

Python integration (real `desentryd` processes over HTTP, stdlib only):

| Test | Covers |
| --- | --- |
| `cluster_integration_test.py` | 3-node mesh, replication, peers, brain |
| `transit_replay_test.py` | transit store TTL, replay to a returning node |
| `airplane_mode_test.py` | full lifecycle with no network egress available |
| `usb_node_test.py` | removable node: password-KDF unlock, detach/attach |
| `soak_test.py` | 50 nodes on one LAN, with nodes killed and restarted mid-write (`--nodes`, `--chaos`, `--settle`) |

---

## 3. Verification

Toolchain actually used: **Windows 11**, MSYS2 UCRT64 **GCC 16.2.0**,
**CMake 4.4.2** with Ninja, **OpenSSL 3.6.4**, Python 3.14, Node 22.

### Executed, and passing

| Check | Result |
| --- | --- |
| `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build` | clean: `desentryd.exe`, `desentry_cli.exe`, 9 test binaries |
| `ctest --test-dir build` | **9 / 9 passed**, three consecutive runs |
| `network_test` specifically | 8 / 8 consecutive runs, after the gossip fix below |
| `python tests/integration/transit_replay_test.py` | ALL 22 CHECKS PASSED |
| `python tests/integration/airplane_mode_test.py` | ALL 18 CHECKS PASSED |
| `python tests/integration/usb_node_test.py` | ALL 18 CHECKS PASSED |
| `python tests/integration/soak_test.py --nodes 12 --writes 150 --chaos 3` | ALL 9 CHECKS PASSED — 12 real processes, 3 killed mid-write, all converged, every hash chain verified |
| A live single node (`desentryd --config node.json`) | identity generated, 5 engines loaded, P2P listener + UDP discovery + REST API up; PUT/GET/scan, `_collection/.../engine` binding to `ts_rollup`, `_collection/.../acl`, `_quota`, `_brain`, `_ledger/verify` all correct; **data and the signed ledger survived a restart** |
| `npx tsc --noEmit` and `npx vite build` in `app/` | clean: 71.63 kB JS, 19.61 kB CSS |
| `node app/tests/qr.check.mjs` | ALL CHECKS PASSED against the ISO/IEC 18004 published constants |
| `python -m py_compile` on the client, the harness and all 5 integration tests | clean |

### Not executed — and why

| Not run | Reason |
| --- | --- |
| `cargo check` / `cargo build` — **no Rust has ever been compiled** | cargo/rustc are not installed on the machine used |
| `npm run tauri:build` — no installer has been produced | needs cargo |
| The desktop app end to end | same |
| `cluster_integration_test.py` | wants a cluster started separately; the other four suites cover the same ground through the harness |
| `soak_test.py` at the full 50 nodes | run at 12; 50 was not attempted on this machine |
| Any build on Linux, macOS, or MSVC | only the MSYS2 UCRT64 toolchain was available |
| The vendored backends (SQLite / DuckDB / LMDB / sqlite-vec) | `OFF` by default; their sources are not vendored here |
| The ONNX sizing path | the model is a build-step download that was not run; the keyword fallback is what has been exercised |

The Rust sidecar is therefore the largest unverified surface in the tree.
Known hazards in it were handled deliberately (`checked_sub` on `Instant`,
`libc::kill` rather than a hand-declared extern, never holding the node-map
mutex across a restart wait) — but as the C++ side just demonstrated, code
that has never been compiled has never been checked. Expect the first
`cargo check` to find things.

**Next, on a machine with the full toolchain:**

1. `cd app && cargo check --manifest-path src-tauri/Cargo.toml`
2. `npm run tauri:build`
3. The same `cmake` + `ctest` run on Linux and macOS, and once under MSVC.

---

## 4. Bugs found and fixed (worth remembering)

**v1, still relevant:**

- **Crash-recovery bug.** `Catalog::Save()` wrote the B+Tree root page id
  outside buffer-pool/WAL durability, while the root page itself was not
  guaranteed flushed. A restart could read a zero-filled page as a corrupt
  B+Tree node and segfault. Fixed by rebuilding a fresh B+Tree per collection
  from WAL replay on `Open()` instead of trusting the stored root page id.
- **Silently-disabled test assertions.** `RelWithDebInfo` defines `NDEBUG`,
  which compiled every `assert()` — i.e. every actual check — into nothing.
  The whole suite "passed" while verifying nothing. Fixed with `-UNDEBUG` on
  test targets only. This is the single most important line in this file:
  **if you add a test target by hand, carry that flag.**
- **Merged remote writes are deliberately not re-broadcast** in
  `NodeEngine::MergeRemote()`, to avoid broadcast storms past 2 nodes; gossip
  anti-entropy is the documented correctness backstop for multi-hop.

**v2, found by actually building and running it on Windows:**

Every one of these was invisible to a syntax-only check. They are listed
first because they are the argument for compiling and running code rather
than reading it.

- **`secure_channel.cpp` did not link on Windows.** The header had been ported
  to `dsn_socket_t`; the definitions still said `int sockfd`. On Linux those
  are the same type and it linked silently. On Windows `SOCKET` is 64-bit
  unsigned, so four functions simply did not exist at link time.
- **Recovery hung at 100% CPU on a zero-filled page.** `roots.json` names each
  collection's B+Tree root, and it is written separately from the page it
  names — so a crash before the flush leaves an id pointing at nothing. A read
  past the end of the data file returns zeros, zeros parse as "internal node,
  no keys, child 0", and the descent walks to page 0 and stays there forever.
  This is v1's catalog-root bug reintroduced through a different door. Fixed
  in two places: `IndexFor()` validates a persisted root before trusting it,
  and `FindLeaf()` is bounded so no corrupt page can spin a node forever.
- **Gossip could never repair a divergence.** The digest compared each key's
  top HLC timestamp only. After merging a peer's write, a document's freshest
  field can come *from that peer* — so both sides report the same timestamp
  while only one of them holds the merged result, neither offers anything,
  and the divergence is permanent. It reproduced about one run in three.
  `DigestEntry` now carries a content fingerprint and peers exchange when the
  bytes differ, not only when a clock is newer.
- **The transit store was unreachable code.** `PlacementPolicy::Rebuild()`
  drops absent peers from the ring, so `Place()` never named one, so
  `HoldForUnreachableOwners()` — which looked for unreachable nodes *in the
  plan* — never found any. A `PlacementPlan` now also reports
  `displaced_owners`: who would have held this key if everyone were up.
- **...and then the transit write was refused for an over-long key.** The
  storage key was `node_id + "/" + hex(sha256)` = 97 bytes against the
  engine's 64-byte limit. The refusal surfaced only as a log line on a path
  nobody was watching. Both halves are now truncated, with the full values
  kept inside the envelope and checked on read.
- **`default_engine` could name an engine the node never loads.**
  `NodeConfig::Validate()` checked that the name was *known*, not that this
  node had it — leaving every unbound collection unroutable at first write.
- **A refused read returned `AuthError` where the design says `NotFound`.**
  `docs/architecture-v2.md` §6.3 and the test agreed; the implementation was
  the odd one out. Distinguishing "you may not read this" from "this does not
  exist" tells a stranger the collection is there.
- **`ts_rollup` silently bucketed by write time** for any point whose field
  was named `timestamp_ms` rather than `ts`/`timestamp`/`time` — a wrong
  answer that looks like a working one. The explicit-unit spellings are now
  accepted.
- **`quota_split` was silently ignored** by both cluster scripts and the Rust
  sidecar: they wrote `db_pct`, `transit_store_pct`, … while the parser reads
  `db`, `transit_store`, …. The node booted, dropped the block, and ran on
  defaults that also sum to 100 — so nothing complained.
- **Four tests asserted things the design does not promise**, and were
  corrected rather than the engine bent to fit: documents larger than a 4 KiB
  page (`router_test`, `quota_test`), a prune dropping PUT entries rather
  than settled transit pairs (`ledger_v2_test`), an exclusive reading of an
  inclusive bound — since renamed `UnclaimedIntentsThrough` — and 40 vectors
  generated by a period-16 formula in 16 dimensions, three of which were
  byte-identical while the test asserted a unique nearest neighbour.
- **The integration harness waited for every node's ledger tip to match.**
  Each node's ledger is its own append-ordered, self-signed chain, so nodes
  that receive the same writes in a different order have different tips *by
  construction*. It now compares the per-collection checksums from `/_brain`,
  which is what a replication test actually cares about.
- **`network_test` slept fixed intervals** and failed on a busy machine. It
  now waits on the condition with a generous bound.

**v2, found while writing it:**

- **`graph_adj` edge ownership.** An edge was reachable from both endpoints'
  adjacency records with no single owner, so a delete could leave a half-edge
  that survived merge. Ownership is now assigned deterministically to the
  lexicographically smaller endpoint.
- **`sqlite_vec` rowid resume.** Reopening a partition restarted rowid
  allocation and could collide with existing vectors; resume now reads the
  stored maximum.
- **QR format-info bit collision.** The second format-info copy wrote eight
  modules up the left of the bottom-left finder; the split is at **seven** —
  module `(size-8, 8)` is the always-dark module and carries no format bit.
  Found by checking both copies against the eight published level-M strings.
- **Explorer paging skipped a key.** Paging appended a space to the last key
  to get an exclusive lower bound, but the engine's scan bound is *inclusive*;
  replaced with ask-from-`last.key` and drop the repeated first row.
- **Dashboard XSS.** `tools/dashboard.html` v1 interpolated collection names
  and document keys — both peer-controlled — straight into `innerHTML`. Now
  escaped.
- **Windows portability in tests.** `network_test.cpp` used `<sys/socket.h>`,
  `socketpair()` and `close()` directly. `SocketPair` was added to
  `platform.h`/`platform.cpp` and the test ported; platform code stays
  confined to the shim.

---

## 5. Known limits (stated plainly)

**Engine**

- Coarse-grained mutex on the B+Tree rather than latch-crabbing. Correct, but
  it serialises concurrent writers to one collection.
- No page-level checksums for a page torn mid-write during a structural split.
  WAL protects documents, not raw page integrity at that granularity.
- The bump allocator does not reclaim space: **no vacuum until Phase 2.**
  Delete-heavy workloads grow the file until the partition is rebuilt.
- **A document must fit in one 4 KiB page.** The kv backend stores a document
  in a slotted page and refuses anything larger with `InvalidArgument`. There
  are no overflow pages, so a large blob has to be chunked by the caller.
- **Keys are limited to 64 bytes**, which is why the transit store's own key
  had to be truncated to fit.
- Multi-hop relay past eager-broadcast's direct peers relies on gossip
  anti-entropy — correct, not the lowest-latency design for large meshes.

**v2 scope**

- Scale target is 50 nodes per user on a single LAN. Nothing in the protocol
  forbids more; nothing above that has been designed for or tested.
- Admission's token bucket is present and on, but has only ever been reasoned
  about, never measured under load.
- Bytes are held for an absent owner **at write time**, and only once that
  peer is already known stale (`gossip_interval * 3 + 5s`). A write during the
  window between a node dying and the mesh noticing is replicated to the peers
  that are up and is not held for the absent one; it reaches that node through
  ordinary anti-entropy when it returns, not through the transit store. Using
  a failed send as direct evidence of unreachability would close the window
  and is the obvious next improvement.
- The AI sizing model is bundled by a **build** step (`npm run fetch-model`),
  not shipped in the git tree. A tree without it still works — the keyword
  fallback runs and labels itself honestly — but the semantic path is absent
  until that step runs.
- Vendored backends are unbuilt in this tree by default. `GET /_engines` is
  the only trustworthy answer to "what does this binary actually have".
- Installers (MSI / .dmg / AppImage) are configured but have never been
  produced here.

---

## 6. History: round 2 (v1)

A teammate's independent design proposed partition ownership routed through a
coordinator ("ROOT") plus a second routing ledger. Their own docs noted this
reintroduces a soft single point of failure and relies on physical-clock LWW
for shared state. The CRDT/HLC model already solves both more generally, so
the ownership/routing/coordinator subsystem was **not** adopted — reasoning
recorded in `docs/comparison.md` §2 rather than silently dropped.

Adopted from it, because it was complementary rather than competing: the
hash-chained Ed25519-signed audit ledger (which v2's ledger grew out of), the
`/_brain` whole-node snapshot endpoint, the zero-dependency Python client, and
`tools/dashboard.html`. The type-aware data-*placement* idea was flagged there
as future work — and v2's storage router is where it landed.

---

## 7. Documents

| File | Contents |
| --- | --- |
| `AGENTS.md` / `AGENT.md` | how to build, test and work in this repo; load-bearing conventions |
| `DESIGN.md` | the control room's design language: tokens, components, screens |
| `docs/architecture.md` | v1 engine design, security model, §8 threat notes |
| `docs/architecture-v2.md` | supervisors, storage router, ledger v2, placement/ACLs, AI sizing, threat model |
| `docs/comparison.md` | the two-design comparison and what was adopted |
| `docs/apple-reference.md` | the unmodified design-language source `DESIGN.md` derives from |
| `README.md` | project overview and quick start |
