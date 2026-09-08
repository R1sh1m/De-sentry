# De-Sentry — Issue Inventory

Updated: 2026-09-08

This file records open issues, known limitations, and verification gaps found
in the repository or during the latest local validation. It does not claim
that every item is reproducible on every platform. Items marked **observed**
were reproduced in the current macOS worktree; items marked **documented**
come from `STATUS.md` or the project design notes.

## Current blockers and failures

| Priority | Status | Issue | Evidence / impact | Recommended next step |
|---|---|---|---|---|
| High | **Observed** | `quota_test` aborts instead of returning `kOutOfSpace`. | `tests/quota_test.cpp:156`; the test expects a write past the configured budget to fail, but the returned status has another code. | Trace quota accounting from `NodeEngine` through `StorageRouter` and the `kv` backend, then rerun the focused test. |
| High | **Observed** | `router_test` aborts on the same out-of-space contract. | `tests/router_test.cpp:167`; the `kv` backend does not satisfy the quota contract in the current build. | Fix the shared quota path or update the test only if the documented contract has intentionally changed. |
| High | **Observed** | `storage_test` can run indefinitely. | The binary ran for more than 11 minutes without completing and had to be terminated. | Reproduce with a timeout, identify the exact test case with progress logging or a debugger, and add a bounded regression test. |
| High | **Observed** | The full local C++ build previously failed compiling `ledger_v2_test`. | The test referenced `UnclaimedIntentsThrough` while the header exposed `UnclaimedIntentsBelow`; this may be fixed in the current generated build, but it should be checked from a clean build. | Clean-configure and rebuild from scratch; keep the API and test naming consistent. |
| Medium | **Observed** | Full C++ validation is not green on this machine. | Six tests passed, two failed, and one was terminated. | Do not treat the local tree as release-ready until all nine tests complete successfully. |

## Local changes not yet committed or pushed

The worktree contains seven unstaged local changes. They are not included in
the remote `main` branch:

- `app/package-lock.json`
- `app/src-tauri/src/ai.rs`
- `app/src/styles.css`
- `include/desentry/storage/wal.h`
- `src/common/config.cpp`
- `src/storage/wal.cpp`
- `src/supervisor/hardware_scan.cpp`

The frontend build passed locally. The C++ core compiled far enough to produce
the core library and executables, but the test suite did not finish cleanly.
The ONNX-enabled Rust path was not validated. These changes should be split
into focused commits and tested before pushing.

## High-risk correctness areas

### WAL and ledger

The local WAL changes affect malformed-tail detection, pruning, retained LSNs,
hash-chain rebuilding, and the next LSN after pruning. This is durable
storage behavior and needs focused tests for:

- torn final frames versus malformed complete frames;
- CRC failures with and without bytes after the damaged frame;
- pruning an unclaimed transit intent;
- pruning ordinary records while preserving original LSNs;
- reopening and appending after a sparse-LSN prune;
- `VerifyChain()` after every prune case.

The current implementation also stores `malformed_tail_` as mutable state and
reads it in a second lock section after `ReadAllLocked()`. Keep this state
consistent if WAL reads become concurrent or are refactored.

### Quota accounting

The quota contract currently fails in both the node-level and router-level
tests. This is a shared behavior boundary, so changing only one backend or
only one test risks inconsistent enforcement across engines.

### ONNX model path

The local change serializes ONNX session access with a mutex. The fallback
path builds, but the ONNX-enabled path has not been compiled or run with the
model. Validate concurrent inference, lock poisoning behavior, model loading,
and resource packaging before relying on semantic sizing.

## Documented engine limitations

These are known product limitations rather than newly discovered regressions:

- B+Tree writes use a coarse-grained mutex and serialize concurrent writers to
  one collection.
- Raw pages have no page-level checksums for torn writes during structural
  splits; the WAL protects documents, not every page write.
- The bump allocator does not reclaim deleted space; there is no vacuum until
  Phase 2.
- A `kv` document must fit in one 4 KiB page. Larger documents are rejected
  and must be chunked by callers.
- Keys are limited to 64 bytes.
- Multi-hop relay depends on gossip anti-entropy and is not optimized for
  large meshes.
- The tested scale target is 50 nodes on one LAN; larger deployments are
  neither designed nor tested.
- Admission token-bucket behavior has not been measured under load.
- Transit storage is not guaranteed during the failure-detection window before
  a dead peer is marked stale; anti-entropy repairs this when the peer returns.
- Optional SQLite, sqlite-vec, DuckDB, and LMDB backends are disabled unless
  their sources are already vendored.

## Verification gaps

The repository status documentation identifies these checks as incomplete:

- No MSI, DMG, or AppImage installer has been produced.
- The desktop node-creation wizard has not been driven end to end.
- The ONNX model has not been fetched, bundled, or loaded in a full build.
- The full 50-node soak test has not been run; only a smaller run was
  documented.
- Linux, macOS, and MSVC C++ builds have not all been validated in the
  documented verification pass.
- The vendored optional backends have not been exercised.
- `cluster_integration_test.py` has not been run in the documented pass.

## Security and operational follow-up

- Re-run the airplane-mode and removable-node integration tests after changes
  to process supervision, resources, keychain handling, or configuration.
- Verify that all peer-controlled values remain escaped in the frontend; do
  not reintroduce `innerHTML` for collection names, keys, or document data.
- Keep supervisors off the data path and loopback-only.
- Confirm recovery-key export and restore behavior before treating node
  provisioning as production-ready; there is no escrow or recovery path if
  the user's exported key is lost.
- Measure admission and gossip behavior under concurrent writes, peer churn,
  and delayed failure detection.

## Recommended order of work

1. Isolate and fix the `quota_test` and `router_test` failures.
2. Reproduce and fix the `storage_test` hang with a timeout and a focused
   regression test.
3. Add focused WAL tests for the local pruning and malformed-tail changes.
4. Perform a clean build and run all C++ tests.
5. Validate the ONNX path and the desktop node-creation flow.
6. Produce an installer and run the platform-specific verification matrix.
