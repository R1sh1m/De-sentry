# De-Sentry — Issue Inventory

Updated: 2026-09-08

This is a working issue inventory, not a release claim. **Observed** means a
local test or build reproduced it. **Code-confirmed** means the behavior is
visible in the implementation but has not necessarily produced a user-facing
failure yet. **Risk** means the path is plausible and needs targeted
validation. **Gap** means required behavior has not been exercised.

## Executive assessment

The core engine, storage backends, CRDT layer, ledger, networking, supervisor,
and desktop app contain substantial implemented functionality, but the
repository is not release-ready. The current evidence is not one consistent
green baseline: different retained test runs show different failures, one
storage test previously ran for more than 11 minutes, and the packaged desktop
path has not been built or exercised.

The highest-priority work is:

1. Establish one reproducible clean-build test baseline.
2. Fix the quota contract and investigate the storage-test hang.
3. Prevent WAL/materialized-state divergence after failed writes.
4. Wire `buffer_pool_pages` through to the KV backend.
5. Validate packaged sidecar resolution and node-creation rollback.

## Claude Code execution contract

Use this file as an implementation queue. Do not attempt every item in one
run. Work in priority order and stop when a prerequisite fails.

For each task:

1. Read `AGENTS.md`, the issue's evidence paths, and the relevant tests.
2. Reproduce the issue with the smallest existing command before editing.
3. Make the smallest complete fix; do not rewrite unrelated subsystems.
4. Add or update an existing assert-based test when behavior changes.
5. Run the issue's acceptance commands and then the nearest broader suite.
6. Update this file: change the status, record the command and result, and
   leave unresolved findings unchanged.
7. Never claim a platform, installer, model, or integration path is verified
   unless its command actually ran successfully.

### Task queue

| ID | Task | Depends on | Acceptance command |
|---|---|---|---|
| `baseline` | Establish a clean macOS C++ baseline and isolate every failure. | None | `cmake -S . -B build-macos -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build-macos -j && ctest --test-dir build-macos --output-on-failure` |
| `quota` | Fix node/router quota accounting and status codes. | `baseline` | `./build-macos/quota_test && ./build-macos/router_test` |
| `storage-hang` | Find and bound the `storage_test` hang. | `baseline` | `./build-macos/storage_test` completes without timeout |
| `wal-recovery` | Make failed WAL/materialization writes recover consistently. | `baseline` | Focused regression test plus `./build-macos/storage_test` |
| `buffer-pool` | Thread `buffer_pool_pages` into the KV backend. | `baseline` | Configured values change the backend pool and the relevant test passes |
| `sidecar` | Align staging, Tauri bundling, and runtime sidecar lookup. | `mac-app` | `npm run tauri:build` and launch the produced app |
| `node-rollback` | Make node creation clean up ports/processes/artifacts on failure. | `mac-app` | Failure-injection tests and repeated create/cancel attempts |
| `path-allowlist` | Restrict `reveal_path` to approved canonical roots. | `mac-app` | Traversal, symlink, and outside-root tests |
| `coverage` | Make Docker and repository test commands run all suites. | `baseline` | Container and unified test command run all nine C++ suites |
| `ci` | Add CI for C++, app, Rust, and bounded integration smoke tests. | `coverage` | Pull-request workflow passes on a clean checkout |

When completing a task, use this record format in the issue entry:

```text
Status: Fixed / Reproduced / Blocked / Not reproducible
Changed: exact files
Reproduction: exact command
Acceptance: exact command and result
Remaining: what is still unverified
```

## Priority 0 — correctness blockers

| Status | Issue | Evidence | Impact | Next action |
|---|---|---|---|---|
| **Observed** | The C++ test suite is not consistently green. | A local macOS run failed `quota_test` and `router_test` on `kOutOfSpace`, then `storage_test` ran for over 11 minutes and was terminated. A retained test log also records failures in `acl_test`, `ledger_v2_test`, `quota_test`, and `router_test` (`build/Testing/Temporary/LastTest.log.tmp2771d`). | There is no authoritative passing baseline for the current tree. | Clean-configure from scratch, run each failing binary independently with a timeout, and record one reproducible baseline. |
| **Observed** | The KV quota contract does not reliably return `kOutOfSpace`. | `tests/quota_test.cpp:156` and `tests/router_test.cpp:167` abort when a write past budget returns another status. | API callers cannot distinguish capacity exhaustion from a generic failure; writes may stop before the intended quota boundary. | Trace node-level and backend-level accounting, including page allocation and metadata overhead, then add focused quota cases. |
| **Observed** | `storage_test` can run indefinitely. | `build/storage_test` consumed CPU for more than 11 minutes without completing and required termination. | CI or release validation can hang indefinitely. | Run test cases individually or add progress labels; use a debugger/sample to identify the loop and add a regression test with a bounded timeout. |
| **Fixed and pushed** | Ledger checkpoint helper name and bound semantics were inconsistent with its test. | The test uses inclusive `UnclaimedIntentsThrough`; the implementation had exposed exclusive `UnclaimedIntentsBelow`. | The ledger test could not compile from a clean tracked checkout. | Fixed in `cdaf10d`; keep the inclusive contract covered. |
| **Fixed and pushed** | WAL pruning removed ordinary PUT history instead of only settled transit pairs. | `ledger_v2_test` caught the mismatch; the focused ledger suite now passes. | Checkpoint GC could remove audit history and materialized-write evidence. | Keep pruning limited to settled transit records and run the full suite after WAL changes. |

## Priority 1 — code-confirmed correctness defects

### WAL and materialized state can diverge after a failed write

**Status: Code-confirmed — `src/storage/storage_engine.cpp:132-157`.**

`StorageEngine::PutRaw()` appends and flushes a WAL record before calling
`router_->Put()`. If the backend write fails, the error is returned but the
WAL record remains. On restart, replay retries the write and skips an
unreplayable record while allowing recovery to continue (`PutRaw` replay path
around `src/storage/storage_engine.cpp:68-100`). This can leave the ledger
claiming a write that is absent from materialized storage.

**Fix direction:** make failed materialization explicit in the recovery model:
either reserve/validate backend capacity before appending, add a compensating
record, or make startup fail closed when a durable PUT cannot be replayed.
Add a test that forces backend failure after WAL append, restarts, and checks
ledger/materialized-state consistency.

### `buffer_pool_pages` is accepted but ignored by the KV backend

**Status: Code-confirmed — `src/common/config.cpp:77`,
`apps/desentry_node/main.cpp:113`, `src/engine/node_engine.cpp:28-32`,
`src/storage/storage_engine.cpp:17-45`, `src/storage/engines/kv_bplus.cpp:14-25`.**

The setting is parsed and passed through the daemon and desktop config
generator, but `StorageEngine::Open()` does not forward it into router/backend
options. `KvBPlusBackend::Open()` always creates a 1024-page buffer pool.

**Impact:** the user-visible sizing control has no effect for the default
engine, and memory use does not follow generated node configuration.

**Fix direction:** add the option to the storage/router/backend option chain,
construct the KV pool from it, validate zero and very small values, and test
that two configurations create measurably different pool capacities.

### Packaged sidecar naming and lookup do not agree

**Status: Risk requiring packaging reproduction.**

`app/scripts/stage-sidecar.mjs:3-9,75-80` stages
`desentryd-<target-triple>`, while `app/src-tauri/tauri.conf.json:57`
declares `binaries/desentryd`. The Rust resolver in
`app/src-tauri/src/lib.rs:60-80` searches for an unsuffixed binary beside the
executable or directly under resources. No installer has been produced, so
this is not yet a reproduced failure.

**Impact:** a packaged app may start without being able to launch its sidecar.

**Fix direction:** choose one target-triple naming convention, make staging,
Tauri `externalBin`, and runtime resolution use the same convention, then run
`npm run tauri:build` and launch the produced artifact.

### Node creation has partial-failure cleanup gaps

**Status: Code-confirmed risk — `app/src-tauri/src/commands.rs:253-315`,
`app/src-tauri/src/appstate.rs:80-95,110-175`.**

Node creation allocates ports, starts a node, writes configuration, and stores
recovery material across multiple fallible steps. Errors from keychain writes
or later config persistence can return without stopping the already-started
node or releasing the port reservation.

**Impact:** failed creation can leak processes, ports, temporary state, or
keychain entries and make subsequent creation attempts fail mysteriously.

**Fix direction:** use one rollback path that stops the child, releases
reservations, removes only artifacts created by this attempt, and reports the
original failure. Add failure-injection tests for each step.

## Priority 1 — quota and storage risks

### Per-engine quota allocation rounds down

**Status: Code-confirmed risk — `src/storage/router.cpp:246-265`.**

The router divides the data-plane budget evenly among configured engines, then
converts each share to whole MiB. Small quotas or many engines can lose a
significant portion to rounding, while the node-level guard uses a separate
calculation.

**Impact:** configured quota, backend quota, and reported free space can
disagree; small test quotas may fail for metadata reasons before the intended
limit.

**Fix direction:** define the accounting unit once, distribute remainders
deterministically, and test one through five configured engines at small and
large quotas.

### WAL malformed-tail state needs focused coverage

**Status: Local unpushed change; partially validated.**

The local changes add `malformed_tail_` tracking and alter prune/LSN behavior
in `include/desentry/storage/wal.h` and `src/storage/wal.cpp`. The focused
ledger test passes after the pruning correction, but the full storage suite
does not complete.

Required cases:

- truncated final frame versus a complete frame with a bad CRC;
- malformed frame with bytes after it;
- verification after restart;
- sparse original LSNs after pruning;
- append and checkpoint behavior after a prune;
- concurrent reads while verification observes malformed state.

Do not push these changes as production-ready until these cases pass.

## Priority 1 — security and isolation review

### `reveal_path` accepts arbitrary existing paths

**Status: Code-confirmed risk — `app/src-tauri/src/commands.rs:511-532`.**

The command accepts a path from the frontend and passes an existing path to
the platform opener without restricting it to sidecar-known paths or an
approved application directory.

**Impact:** if an untrusted webview path ever gains command invocation
capability, it could open arbitrary local files or directories. This is also a
UX boundary issue because the command is broader than its apparent purpose.

**Fix direction:** accept an internal node/file identifier instead of a raw
path, resolve it against an allowlisted root, canonicalize it, and reject
paths outside that root. Add traversal and symlink tests.

### Recovery-key loss is unrecoverable

**Status: Documented product constraint.**

There is no escrow or recovery path other than the user's exported recovery
key. The creation flow has not been driven end to end.

**Fix direction:** verify forced export, clear confirmation, persistence, and
restore behavior in the desktop flow; make the irreversible consequence
unambiguous without introducing escrow.

## Priority 2 — test and release-system gaps

### Docker does not run all C++ tests

**Status: Code-confirmed — `Dockerfile:59-62`,
`docker-compose.yml:94-105`.**

CMake registers nine C++ test binaries, but the container image copies and
runs only `crdt_test`, `crypto_test`, `storage_test`, and `network_test`.
ACL, placement, ledger, quota, and router regressions can pass the documented
container check unnoticed.

**Fix direction:** copy/run all test binaries or invoke `ctest` in the image,
and fail the compose test service on any failure.

### Python integration tests are not part of one test command

**Status: Verification gap — `CMakeLists.txt:167-186`,
`STATUS.md:88-96`.**

Replication, transit replay, airplane mode, removable-node, and soak tests
are manual Python commands rather than CTest targets or a single test script.

**Impact:** a green CTest result does not validate the distributed system.

**Fix direction:** add a documented test entry point that builds, starts the
required mesh, runs the Python suites, and cleans up; preserve the ability to
run each suite independently.

### There is no CI workflow

**Status: Code-confirmed gap.**

No GitHub Actions workflow or equivalent repository CI was found.

**Impact:** pull requests do not automatically run C++ builds/tests, frontend
checks, Rust checks, integration suites, or packaging checks.

**Fix direction:** add offline-safe CI jobs for CMake/CTest, `npm run build`
and checks, `cargo check --no-default-features`, and a bounded integration
smoke test. Add a separate opt-in job for model/package validation.

### Test discovery depends on top-level globs

**Status: Code-confirmed risk — `CMakeLists.txt:167-186`.**

Tests are discovered from `tests/*_test.cpp` and only exist when
`DESENTRY_BUILD_TESTS` is enabled. This is convenient but can hide missing
registration or make a production build appear healthy without a smoke test.

**Fix direction:** keep the glob if desired, but add a configure-time summary
and a no-tests smoke target that starts the daemon and checks its health
endpoint.

## Priority 2 — product and platform verification gaps

The following are documented but not yet verified in the current project
status:

- No MSI, DMG, or AppImage has been produced.
- The desktop node-creation wizard has not been driven end to end.
- The ONNX-enabled Rust path has not been compiled and the model has not been
  loaded.
- The full 50-node soak test has not been run; only a smaller run was
  documented.
- Linux, macOS, and MSVC C++ builds have not all been validated in the
  documented verification pass.
- Optional SQLite, sqlite-vec, DuckDB, and LMDB backends have not been
  exercised.
- `cluster_integration_test.py` has not been run in the documented pass.
- Admission token-bucket behavior has not been measured under load.
- Installer update/upgrade behavior has not been tested.

## macOS bring-up and release procedure

This is the exact order for achieving a working De-Sentry build on macOS.
Run commands from the repository root unless the command begins with `cd`.
Do not skip ahead to the Tauri installer until the engine and sidecar checks
are green.

### 1. Install prerequisites

```bash
xcode-select --install
brew install cmake openssl@3 node rust python
export OPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
export PATH="$(brew --prefix openssl@3)/bin:$PATH"
```

Confirm the toolchain:

```bash
clang++ --version
cmake --version
openssl version
node --version
npm --version
rustc --version
cargo --version
python3 --version
```

If Homebrew is not installed, install it from
`https://brew.sh/`, then repeat the package command. Do not install Python
packages for the repository's standard integration tests; they intentionally
use the standard library.

### 2. Configure and build the engine

```bash
export OPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
cmake -S . -B build-macos \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DDESENTRY_BUILD_TESTS=ON
cmake --build build-macos --parallel
```

Optional SQLite, sqlite-vec, DuckDB, and LMDB backends remain disabled unless
their sources already exist under `third_party/`. Never make the build fetch
them automatically.

### 3. Run the C++ validation

Run the complete suite with a bounded shell loop so one hanging binary does
not block the entire run:

```bash
for test in acl_test crdt_test crypto_test ledger_v2_test network_test \
            placement_test quota_test router_test storage_test; do
  echo "=== $test ==="
  perl -e 'alarm 120; exec @ARGV' "./build-macos/$test" || exit $?
done
```

Then run CTest for the canonical report:

```bash
ctest --test-dir build-macos --output-on-failure
```

If a test fails, do not run the full release procedure. Record the exact
binary, assertion, and command under the corresponding issue. If `perl` is
unavailable, use a macOS-compatible process timeout or run the binary in a
separate Terminal and terminate it explicitly.

### 4. Run the local mesh and integration tests

```bash
cmake --build build-macos --parallel
./scripts/run_cluster.sh 3 --supervisor --engines kv,ts_rollup
```

In another terminal, run the standard-library integration suites:

```bash
python3 tests/integration/transit_replay_test.py
python3 tests/integration/airplane_mode_test.py
python3 tests/integration/usb_node_test.py
python3 tests/integration/soak_test.py --nodes 12 --writes 150 --chaos 3
```

Run the optional cluster suite only when its pre-start requirement is met:

```bash
python3 tests/integration/cluster_integration_test.py
```

Always clean up the mesh:

```bash
./scripts/stop_cluster.sh
```

### 5. Build and validate the frontend

```bash
cd app
npm install
npm run typecheck
npm run check:qr
npm run check:css
npm run build
cd ..
```

The app must remain dependency-free at runtime. Do not add a frontend
framework or runtime package to solve an issue.

### 6. Build the macOS desktop app without ONNX first

Use the fallback path to validate the Tauri shell and sidecar independently
of the model:

```bash
cd app
npm run stage-sidecar
npm run tauri:dev -- -- --no-default-features
```

Verify that the window opens, the supervisor starts, the hardware scan
returns volumes, and the UI reports fallback sizing honestly. Stop the app
with `Ctrl-C` and return to the repository root.

### 7. Validate the ONNX path explicitly

The model is an explicit build-time download, not a runtime dependency:

```bash
cd app
npm run fetch-model
npm run tauri:dev
```

Confirm model resources exist before building:

```bash
find resources/model -maxdepth 2 -type f -print
test -f resources/prototypes.json
```

Exercise at least two concurrent sizing requests and verify that a missing
model uses the deterministic fallback instead of pretending to use ONNX.

### 8. Produce and inspect the macOS installer

```bash
cd app
npm run tauri:build
```

The result should include a `.app` and a `.dmg` under `app/src-tauri/target/`.
Install or open the `.app`, create a node through the wizard, verify the
supervisor sidecar, then inspect the packaged sidecar name and runtime logs.
If staging and runtime lookup disagree, stop and fix task `sidecar`.

### 9. macOS completion checklist

Do not mark macOS complete until all of these are true:

- `cmake --build build-macos --parallel` passes.
- All nine C++ test binaries finish within their timeout.
- `ctest --test-dir build-macos --output-on-failure` passes.
- Transit, airplane-mode, USB, and bounded soak tests pass.
- Frontend typecheck, QR check, CSS check, and build pass.
- Tauri fallback mode launches and starts the sidecar.
- ONNX mode loads the model, or the limitation is explicitly recorded.
- The node-creation wizard succeeds end to end.
- A `.dmg` is produced and the packaged app starts.
- `ISSUES.md` records commands that were not run; none are implied green.

## Documented engine limitations

These are known design limits, not automatically bugs:

- B+Tree writes use a coarse-grained mutex and serialize concurrent writers to
  one collection.
- Raw pages have no page-level checksums for torn writes during structural
  splits; the WAL protects documents, not every page write.
- The bump allocator does not reclaim deleted space; there is no vacuum until
  Phase 2.
- A KV document must fit in one 4 KiB page; larger documents must be chunked
  by callers.
- Keys are limited to 64 bytes.
- Multi-hop relay depends on gossip anti-entropy and is not optimized for
  large meshes.
- The tested scale target is 50 nodes on one LAN; larger deployments are not
  designed or tested.
- Transit retention has a failure-detection window before a dead peer is
  marked stale; anti-entropy is the repair path when it returns.
- Optional vendored backends are never downloaded automatically and are off by
  default.

## Requirements/documentation contradictions

`Project_Statement/project_statement.md:45-51` says node-failure and network
partition handling is not implemented, while `docs/comparison.md:59-61`
claims eager broadcast plus gossip anti-entropy handles offline peers. The
implementation and tests support parts of the newer claim, but the project
statement has not been reconciled.

Resolve the documentation by stating the exact guarantee: for example,
temporary peer loss and eventual anti-entropy recovery are supported, while
formal partition tolerance, bounded convergence time, and arbitrary failure
patterns are not guaranteed.

## Current local worktree

Before this execution pass, the branch was aligned with `origin/main` and the
following local changes were pending:

```text
app/src-tauri/src/ai.rs
include/desentry/storage/wal.h
src/storage/wal.cpp
```

The ONNX change serializes access to the inference session but the ONNX path
has not been built or run. The WAL changes affect durable recovery, pruning,
verification, and LSN behavior. Committing these files does not mean those
paths are fully verified; the task queue and acceptance commands above remain
authoritative.

## Recommended execution order

1. Reproduce the C++ failures independently from a clean build and diagnose
   the storage-test hang.
2. Fix quota accounting and add deterministic small-quota tests.
3. Resolve WAL/materialized-state failure semantics and finish WAL regression
   coverage.
4. Wire and test `buffer_pool_pages`.
5. Fix node-creation rollback and constrain `reveal_path`.
6. Align sidecar staging, Tauri bundling, and runtime resolution; build and
   launch an installer.
7. Expand Docker/CTest coverage and add CI.
8. Reconcile the project statement with the actual partition/failure
   guarantees.
