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

> **Read this first — what has actually been run.** Both halves now build and
> run on Windows 11. The engine: MSYS2 UCRT64 GCC 16.2 + CMake 4.4.2 + Ninja +
> OpenSSL 3.6.4, `ctest` **9/9 green**, four Python integration suites passing
> against real `desentryd` processes. The desktop app: Rust 1.98.1 MSVC,
> `cargo check` clean, and the app **runs** — window up, supervisor sidecar
> spawned, hardware scan returning real volumes. Not yet done: **no installer
> has been produced** (`tauri build` has never run), the ONNX sizing path has
> never been compiled, and nothing has been built on Linux, macOS, or with
> MSVC for the C++ side. [§3](#3-verification) is the exact list.

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

### Executed 2026-09-08: first full-toolchain runs, on two machines

Windows box 1 -- MSYS2 UCRT64, g++ 16.1, CMake 4.4, OpenSSL 3.6, Python 3.13,
Node 26, Rust 1.98.1 windows-gnu. The v2 tree compiled, linked and ran;
everything below was executed on that toolchain against real binaries:

| Check | Result |
| --- | --- |
| `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j` | clean (after the fixes in SS4-round-3) |
| All 9 C++ suites, run as binaries | **all green**: crdt, crypto, storage (incl. crash-recovery replay), network (3-node convergence), router, acl, placement, ledger_v2, quota |
| `cluster_integration_test.py` (3 real nodes) | ALL PASSED (replication, convergence, ledger verify, peers, tombstones) |
| `transit_replay_test.py` (supervisor + 3 nodes, SIGKILL of one) | ALL 22 CHECKS PASSED (hold, intent, claim, reconverge, verify, quorum refusal, 403, doc survives) |
| `airplane_mode_test.py` | ALL 18 CHECKS PASSED (offline create/write/replicate/verify/restart, no non-LAN socket) |
| `usb_node_test.py` (kill, move data dir, restart) | ALL 18 CHECKS PASSED (identity travels, catch-up, verify) |
| `soak_test.py --nodes 8 --writes 60 --chaos 2` | ALL 9 CHECKS PASSED (converged in 0.2s, identical checksums, all chains verify) |
| `cd app && npm run typecheck`, `npm run build`, `npm run check:qr` | clean (71.63 kB JS, 19.61 kB CSS; QR encoder conformance passes) |
| Manual 3-node mesh (`run_cluster.ps1`) + curl + dashboard path | write-on-1/read-on-2-3, divergent writes converge, delete propagates |
| Rust: `cargo check` (default + `--no-default-features`), `cargo build`, `cargo test` | **clean, 28/28 tests pass** (after the SS4-round-4 fixes; keychain test hits the real Windows Credential Manager) |
| `npm run tauri:build` (WiX MSI, windows-gnu) | **`De-Sentry_2.0.0_x64_en-US.msi` (29.6 MiB) produced**; payload verified via the Installer API (app exe, desentryd sidecar, WebView2 loader, prototypes, ONNX model + runtime + tokenizer) |
| `soak_test.py --nodes 50 --writes 500 --chaos 8 --settle 180` | ALL 9 CHECKS PASSED (50 nodes up, 433/500 writes accepted, converged in 1.7s, 1 checksum everywhere, all chains verify) |
| Docker: `compose build` + 3-node cluster + `run tester` + `run unit-tests` | image builds on Linux, integration ALL PASSED, container unit suites green (network convergence needs the cluster stopped: 6 stacks oversubscribe the Docker VM's CPUs) |

Not run on box 1, and why:

| Not run | Reason |
| --- | --- |
| `cargo test` under MSVC / mobile cdylib link | the shipped `cdylib` links under MSVC (box 2 checks it below); on windows-gnu it exceeds GNU ld's export-ordinal ceiling (see SS4-round-4) |
| `tauri dev` interactive run | box 2 ran it (below); here only the bundled exe exists, unrunned headed |
| `ctest` meta-runner | binaries were run directly (a `ctest` invocation hung with no output while the same binaries pass standalone); box 2 ran `ctest` clean (below) |

Windows box 2 -- MSYS2 UCRT64 **GCC 16.2.0**, **CMake 4.4.2** with Ninja,
**OpenSSL 3.6.4**, Python 3.14, Node 22, Rust 1.98.1 MSVC:

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
| `cargo check --no-default-features` (Rust 1.98.1 MSVC) | **0 errors**, 2 dead-code warnings, both from the deliberately-excluded ONNX path |
| `tauri dev` debug build | compiled 408 crates in 1m 12s |
| The desktop app itself | window opens (1376×919, responding), spawns its supervisor sidecar on 7701/7801, and `/_supervisor/topology` returns a real hardware scan of the machine's volumes |
| `node app/tests/qr.check.mjs` | ALL CHECKS PASSED against the ISO/IEC 18004 published constants |
| `python -m py_compile` on the client, the harness and all 5 integration tests | clean |

### Not executed — and why

| Not run | Reason |
| --- | --- |
| `npm run tauri:build` — **no installer has been produced** | not attempted; only the dev build has run |
| The app's node-creation flow end to end | the window renders and the supervisor answers, but creating a node through the wizard has not been driven |
| The ONNX sizing path | built with `--no-default-features`; `ort` and the model have never been compiled or loaded |
| `cluster_integration_test.py` | wants a cluster started separately; the other four suites cover the same ground through the harness |
| `soak_test.py` at the full 50 nodes | run at 12; 50 was not attempted on this machine |
| Any build on Linux, macOS, or MSVC | only the MSYS2 UCRT64 toolchain was available |
| The vendored backends (SQLite / DuckDB / LMDB / sqlite-vec) | `OFF` by default; their sources are not vendored here |
| The ONNX sizing path | the model is a build-step download that was not run; the keyword fallback is what has been exercised |

The Rust survived first contact with a compiler far better than the C++ did:
one API error against eleven defects. The bugs the app build did surface were
all in its **configuration** rather than its code — and every one of them
would have stopped anyone building from a clean checkout (see §4).

**Next:**

1. `npm run tauri:build` — produce and install the MSI.
2. Drive the creation wizard: make a node through the app, not by hand.
3. `npm run fetch-model` and build with ONNX enabled.
4. The same `cmake` + `ctest` on Linux and macOS, and once under MSVC.

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

**v2, found by building the desktop app for the first time.** All three were
in the app's build configuration rather than its Rust, and each one stopped
the build before a line of code compiled — so anyone cloning the repo would
have hit them immediately:

- **The build hard-failed without the optional AI model.**
  `tauri.conf.json` bundles `resources/model/` unconditionally, and Tauri's
  build script aborts on a missing resource path. The app is designed to run
  without the model on a keyword fallback, so requiring a ~100 MB download to
  compile at all contradicted its own design. A committed
  `app/resources/model/README.md` now keeps the directory present, and
  `.gitignore` keeps the binaries out.
- **Both resource paths pointed at nothing.** Tauri resolves `bundle.resources`
  relative to `tauri.conf.json` — that is `src-tauri/` — while the assets live
  in `app/resources/`. Neither `resources/prototypes.json` nor
  `resources/model/` could ever have resolved; both needed `../`.
- **An invisible overlay covered the entire app.** The wizard sheet is
  appended to `<body>` at startup and hidden with the `hidden` attribute --
  but `hidden` acts only through the user-agent stylesheet's
  `[hidden] { display: none }`, and a user-agent declaration loses to *any*
  author declaration for the same property. `.sheet { display: grid }` won, so
  a full-window blurred scrim at `inset: 0; z-index: 40` was painted over
  everything and swallowed every click. The app started, drew the real UI
  underneath, and was completely unusable. `styles.css` now carries
  `[hidden] { display: none !important }` in its reset block, and
  `npm run check:css` fails the build if it is ever removed or weakened --
  the defect is invisible in review and total at runtime, which is exactly
  the combination worth a guard.
- **`TrayIcon::menu()` does not exist** in Tauri 2.11 (there is only
  `set_menu`), so the tray's status line did not compile. The status
  `MenuItem` is now kept in managed state and updated directly.

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

## 4b. Bugs found and fixed in the first full build+run (2026-09-08)

Missing, wrong, or untested code the first real compile/test cycle surfaced.
Each was fixed where the executable spec (tests, docs, integration) pointed,
and every fix below is covered by a now-green suite.

- **The entire `ledger/` subsystem was missing.** `node_engine.h`,
  `network_manager.h`, `supervisor.cpp` and `ledger_v2_test.cpp` included
  `ledger/{change_feed,transit_store,checkpoint}.h`, but no such files existed
  anywhere -- the first `cmake --build` failed on the includes. Implemented
  all three headers plus `src/ledger/*.cpp` (the build globs that directory
  already, so no CMake change was needed): `TransitStore`, `ChangeFeed`
  (long-poll with prune-aware `truncated`), and the quorum gate
  (`EvaluateCheckpoint`, `UnclaimedIntentsBelow`, `RunCheckpoint`).
- **Transit intent/claim pairs could never match.** The INTENT recorded
  `SHA-256(owner || key)` while both claim paths hashed different material,
  so every intent would have stayed "unclaimed" forever and the checkpoint
  gate would never pass. `ApplyClaimedTransit()` now records under the
  owner's own id and `RecordRemoteClaim()` recovers the real key from the
  held envelope (new `TransitStore::Lookup()`).
- **Transit row keys exceeded the 64-byte B+Tree key limit.** The
  `(owner, key_hash)` composite is 97 bytes as text and binary keys are
  unsafe (the B+Tree treats keys as C strings). The store was rebuilt as a
  sidecar append-log (`transit.log`, same framing as the cross-engine index)
  with an in-memory map -- which also keeps held bytes out of user backends,
  quotas and checksums. Found live: a holder logged "key exceeds 64 bytes"
  and held nothing.
- **`WriteAheadLog::Prune` vs its own test.** `ledger_v2_test` asserted a
  prune drops history below the checkpoint (`front().lsn >= checkpoint`),
  but the specified design -- docs, wal.h, and the integration test's
  "checkpointing rather than truncating" -- is transit-pair GC only. The
  test was rewritten to that contract (settled pairs dropped, PUTs and
  unclaimed intents survive, chain re-derives and re-verifies across a
  restart).
- **Mid-file corruption verified clean.** A flipped byte mid-file truncated
  replay (standard WAL semantics) and `VerifyChain()` blessed the prefix, so
  the tamper test failed. `ReadAllLocked()` now distinguishes a torn tail
  (short read: benign crash) from present-but-invalid bytes (corruption),
  and `VerifyChain()` fails on the latter. v1's storage test (which accepts
  either outcome) still passes.
- **Crash-recovery spin.** A restart after unflushed writes hung forever in
  `FindLeaf`: `roots.json` is written eagerly but dirty pages are dropped
  without a flush, so the stored root replayed as a zero page whose child is
  itself (found via gdb: 100% CPU in `FindLeaf`, plus a depth analysis of
  the zero-page layout). Two fixes: `StorageEngine::Open()` now calls the
  new `ResetForReplay()` (fresh trees; the fsync'd ledger is the source of
  truth -- exactly what SS4-round-1 prescribed) whenever the WAL is
  non-empty, and `FindLeaf` has a 256-level descent bound so no corrupt page
  can wedge the process again. A stale `roots_` after splits (never
  re-persisted) is moot under the reset.
- **ACL read denial leaked existence.** `GetDocument` returned `kAuthError`
  for non-readers while the suite (`ListDocuments` -> empty,
  `ReadableCollections` filtered, and the test itself) promises
  "nothing here". Now `kNotFound`, uniformly.
- **Quota tests wrote 4 KiB documents.** A 4096-byte payload plus CRDT
  framing does not fit a 4 KiB slotted page, so `kv` refused with
  `InvalidArgument` instead of the asserted `kOutOfSpace`. Payloads are now
  1 KiB (router + quota suites): the quota path is about aggregates, and the
  per-record page cap is a documented engine limit, not a quota bug.
- **`ts_rollup` did not know `timestamp_ms`.** The extractor accepted
  `ts`/`timestamp`/`time` but not the canonical schema field
  (`prototypes.json` requires `series`/`timestamp_ms`/`value`), so schema
  points bucketed by wall-clock and the rollup query came back empty.
- **Vector test corpus had exact duplicates.** The `(i%16, 7i%16)` pairs
  repeat every 16 vectors, so every queried vector had an equidistant twin
  and "nearest must be itself" was undefined. A per-vector nudge keeps the
  corpus distinct; the query remains an exact member.
- **`NodeConfig::Validate` accepted an unloadable default.** `engines=[kv]`
  with `default_engine=duckdb` passed validation and died later in the
  router. Membership is now checked at config-load time.
- **Unlimited-quota peers read as full.** Gossip reported `free_quota_mb=0`
  unconditionally (`RecordReport(..., 0)`), and the supervisor degraded any
  healthy peer reporting zero -- i.e. every default-config node. Gossip now
  reports ledger height only (`RecordLedgerHeight`), quota reports set a new
  `quota_reported` flag, and the supervisor only acts on a real report.
- **Cross-node ledger-tip equality is not a property.** Four integration
  tests waited for identical tips, but each node stamps its own HLC/origin,
  so equal tips are structurally impossible (verified by reading the append
  paths, not just observed). `harness.wait_converged` now compares
  per-collection checksums across data nodes -- the convergence the CRDT
  layer actually promises; supervisors (dataless by design) are excluded.
- **Staleness grace vs test timing.** Holds need the owner stale (3x gossip
  + 5s, ~6.2s at test intervals); two tests wrote 2s after the kill, so
  nobody held. Sleeps are now 8s with the derivation in the comment. The
  unit mesh test's fixed sleeps became poll-until-converged for the same
  reason (in-process stacks need longer than 1.5s on Windows; the live mesh
  converges the same scenario in 1.5s).
- **Windows-only breakage that never compiled before.** `secure_channel.cpp`
  defined handshakes with `int sockfd` against `dsn_socket_t` (`unsigned
  long long` on Windows) -- link failure; `disk_manager.cpp` used
  `struct stat` directly instead of the platform shim; `run_cluster.ps1`
  split config paths on spaces (usernames like "Rishi Misra") and indexed a
  one-element engine list down to its first character (`"kv"` -> `"k"`).
- **`.gitignore` swallowed the new sources.** A bare `ledger/` rule (meant
  for runtime data) matched `src/ledger/` and `include/desentry/ledger/`;
  anchored to `/ledger/` + `/transit/`.

## 4c. Bugs found in the first Rust compile + Tauri bundle (2026-09-08)

Toolchain: rustup `stable-x86_64-pc-windows-gnu` 1.98.1 (MSYS2 g++ as
linker), WiX 5 via `dotnet tool`, ONNX model via `npm run fetch-model`.
Same pattern as round 3: code that never compiled failed immediately, in
small, fixable ways.

- **`tray.menu()` does not exist in tauri 2.11.** `tray.rs` read the
  installed menu back to update the status line; the API only has
  `set_menu`. The status `MenuItem` (a cheap Arc handle) is now retained in
  a static at build time. All callers already use the concrete `Wry`
  handle, so the module's aspirational `<R: Runtime>` genericity went with
  it; `commands.rs` also dropped a now-unused `Manager` import.
- **`embed_all(&self)` vs a stateful ORT session.** `Session::run` needs
  `&mut`; the model already lives behind a `Mutex`, so the methods took
  `&mut self` and the two call sites lock mutably. Plus the `unused_mut` it
  hid.
- **`STOP_GRACE` unused on Windows.** The constant is only referenced inside
  `#[cfg(unix)]`; it is now gated `#[cfg(unix)]` instead of warned about.
- **Two no-onnx dead-code warnings** (`Prototype.text`, `dot`): scoped
  `cfg_attr` allows, matching the existing `label` precedent.
- **`tauri.conf.json` resources pointed at the wrong directory.**
  `resources/prototypes.json` and `resources/model/` resolve relative to
  `src-tauri/`, but the canonical layout (fetch-model, .gitignore, the
  sidecar's runtime lookup) is `app/resources/`. Now `../resources/...`.
- **The staged sidecar was never staged.** `cargo check` failed on the
  missing `binaries/desentryd-*.exe` until `npm run stage-sidecar` (which
  itself needed cargo's bin dir on `PATH` for its target-triple probe).
- **GNU ld cannot link the shipped `cdylib`** (`export ordinal too large`:
  ~139k auto-exported symbols over a 64k ordinal ceiling). Desktop
  verification built and tested with a temporarily narrowed
  `crate-type = ["rlib"]`, reverted immediately afterwards (verified clean
  `git diff`); the shipped `staticlib/cdylib/rlib` list is unchanged and
  links fine under MSVC, which has no such ceiling.
- **Test binaries died with `STATUS_ENTRYPOINT_NOT_FOUND` before main.**
  Traced with pefile to `TaskDialogIndirect` (rfd) missing from comctl32
  v5: MSVC's link auto-requests Common Controls v6, GNU ld does not.
  `build.rs` now links a windres-compiled manifest on windows-gnu only
  (`app.manifest` + `app.manifest.rc`); MSVC/mobile never see it. tauri's
  own default manifest covers real binaries on every toolchain, which is
  why only the test harnesses ever failed.
- **A stale WebView2Loader shadowed the good one.** The loader resolved to
  an unrelated copy under Windows Kits; the crate-built
  `target/debug/WebView2Loader.dll` belongs next to (or before, on PATH)
  test binaries. Cargo-test hygiene, not a code bug.

## 4d. Second merge round + model proof + live GUI (2026-09-08, later)

- **Merged origin/main twice more.** The remote added an independent minimal
  ledger sketch, sparse-LSN pruning, an ONNX session mutex and doc updates.
  Kept the verified full implementations; adopted their better ideas
  (sparse LSNs -- stable identities, no reuse, prune test updated;
  finer-grained session mutex; polled staleness in transit_replay;
  RootLooksValid instead of the reset; managed tray state) and re-greened
  everything after each merge.
- **Bootstrap identities never resolved.** Placeholder `bootstrap#host:port`
  table entries were never replaced on handshake (nothing propagated the
  proven id), so placement skipped those peers and id-keyed staleness polls
  never matched. `PeerTable::AdoptIdentity` (driven by the kPing node_id in
  `ProbeLoop`) retires placeholders; transit 22/22 and soak-50 re-passed.
- **Model proven twice.** `app/resources/model/` holds model.onnx (21.9 MiB
  int8 MiniLM), tokenizer.json and onnxruntime.dll. Direct ORT inference
  gives 384-dim embeddings with a working semantic signal, and a new
  `ai::tests::the_onnx_model_loads_and_sizes` test runs the app's exact
  load+embed+size path (method `"onnx"`, workload `"time-series"`).
  Rust suite is 29/29.
- **The GUI runs.** Release `de-sentry-app.exe` opens a responding
  "De-Sentry" window and supervises its sidecar: an app-owned supervisor
  `desentryd` on 7701/7801 whose `/_supervisor/topology` returns a real
  volume scan. MSI payload re-verified file-by-file (7 files, 80 MB).

## 4e. Packaging and easy-run work (2026-09-08, later)

- **`scripts/setup.ps1` / `setup.sh`: one-command bootstrap.** Checks
  prerequisites (with install hints, plus `-Install` via winget on
  Windows), configures and builds the engine, runs `npm install`,
  `fetch-model` and `stage-sidecar`, then prints next steps. Idempotent;
  recovers from a stale CMake cache (generator/toolchain switch) by
  wiping and retrying once. Verified by running it end to end.
- **`npm run tauri:dev` stages the sidecar first.** Previously dev ran
  against whatever sidecar was last staged (possibly weeks stale); the
  dev loop now rebuilds it every launch, like `tauri:build` always did.
  Verified: vite up on :5273, debug app compiled, linked and opened its
  window.
- **`.github/workflows/release.yml`: tagged releases build installers.**
  Engine gate (build + `ctest` on Windows/Linux/macOS), then per-OS
  `tauri-action` producing a draft Release (MSI/.dmg/AppImage/.deb).
  YAML-validated; runners untested (no CI run yet -- first `v*` tag
  exercises it).
- **README quickstart rewritten user-first:** installer download, then
  one-command setup, then manual steps.
- Orphan hygiene found during verification: killed leaked `desentryd`
  processes from earlier interrupted runs (stale `.pid` files had hidden
  them from `stop_cluster.ps1`) and duplicate GUI instances, keeping one
  release app + its supervisor.

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
- **Ledger chains are per-node, not shared.** Every node stamps its own HLC
  and origin signature, so two nodes holding identical documents report
  different tip hashes by construction. Cross-node tip equality is not a
  property of this design (integration tests assert checksum agreement, not
  tip agreement); a hash-set union across peers remains future work, and the
  gossip ledger exchange currently reports tips rather than merging them.
- **Record size caps are hard.** One `kv` record must fit a 4 KiB slotted
  page (~4088 bytes usable) and one key must fit 64 bytes; oversized writes
  are refused with `InvalidArgument`, not paged or chunked.
- **Restart replays the ledger into fresh `kv` trees when the WAL is
  non-empty.** A node whose WAL was deleted but whose `.dsf` files survive
  keeps serving from disk (empty WAL skips the reset); a node with a WAL
  rebuilds from it. Deleting `desentry.wal` while keeping the data files is
  operator error the engine does not defend against beyond that.
- Soak coverage on this machine is 8 nodes / 60 writes / 2 kills, not the
  50-node / 500-write / packet-loss default in `soak_test.py`.

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
