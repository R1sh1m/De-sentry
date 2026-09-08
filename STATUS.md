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

> **Read this first — verification honesty.** The environment this v2 work was
> written in has **no CMake, no cargo/rustc, and no OpenSSL development
> headers**. Therefore **nothing C++ or Rust was compiled, linked, or run
> here**: no `ctest`, no integration test, no Tauri build, no live mesh. What
> *was* run is listed under [§3 Verification](#3-verification) and is
> deliberately separated into "executed" and "not executed". Treat every
> runtime claim below as *designed and statically checked*, not *observed*,
> unless §3 says it was executed.

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

### Executed here, and passing

| Check | Result |
| --- | --- |
| `g++ -std=c++17 -Wall -Wextra -fsyntax-only -Iinclude` on every file in `src/` and `apps/` | **48 pass / 1 fail** |
| The one failure | `src/security/crypto.cpp` — needs `<openssl/evp.h>`, absent from this machine. The file is **unmodified v1 code**; the failure is environmental, not a code defect |
| Same sweep over `tests/*.cpp` (with `-UNDEBUG`) | **9 / 9 pass** |
| `npx tsc --noEmit` in `app/` | exit 0 |
| `npx vite build` in `app/` | built: 71.63 kB JS, 19.61 kB CSS |
| `node app/tests/qr.check.mjs` | ALL CHECKS PASSED — the encoder's level-M format strings and 18-bit version words match the ISO/IEC 18004 published constants |
| `python -m py_compile` on the client, all 5 integration tests and the harness | all clean |
| `bash -n scripts/run_cluster.sh`, `scripts/stop_cluster.sh` | clean |

### Not executed here — and why

| Not run | Reason |
| --- | --- |
| `cmake` configure/build | CMake is not installed on this machine |
| `ctest` — **no C++ test was run** | requires a linked build, which requires CMake and OpenSSL |
| Any Python integration test | requires the `desentryd` binary |
| `cargo check` / `cargo build` — **no Rust was compiled** | cargo/rustc are not installed |
| `npm run tauri:build` — no installer was produced | needs cargo |
| Any live mesh, replication, GC or ACL behaviour | needs running binaries |

Consequences worth keeping in mind: the syntax sweep catches type and API
errors per translation unit but **not** link errors, ODR violations, or
anything about runtime behaviour. The Rust sidecar has had no compiler pass at
all — expect the first `cargo check` to surface borrow-checker and
API-version issues. Known Rust hazards were addressed pre-emptively
(`checked_sub` on `Instant`, `libc::kill` rather than a hand-declared extern,
never holding the node-map mutex across a restart wait), but "addressed
pre-emptively" is not "compiled".

**Do this first on a full toolchain**, in order:

1. `cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j`
2. `ctest --test-dir build --output-on-failure`
3. `cd app && cargo check --manifest-path src-tauri/Cargo.toml`
4. `./scripts/run_cluster.sh 3 --supervisor`, then the Python integration tests
5. `cd app && npm run tauri:build`

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

**v2:**

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
- Multi-hop relay past eager-broadcast's direct peers relies on gossip
  anti-entropy — correct, not the lowest-latency design for large meshes.

**v2 scope**

- Scale target is 50 nodes per user on a single LAN. Nothing in the protocol
  forbids more; nothing above that has been designed for or tested.
- Admission's token bucket is present and on, but has only ever been reasoned
  about, never measured under load.
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
