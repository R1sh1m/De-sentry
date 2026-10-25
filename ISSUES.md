# De-Sentry — Open Issue Inventory

Updated: 2026-10-25

This file lists only work that is still incomplete or insufficiently verified.
Completed fixes are recorded in `STATUS.md` and git history, not repeated here.

## Executive assessment

The core engine, storage backends, CRDT layer, ledger, networking, supervisor,
desktop app, CI workflow, and default test suites have passing validation.
Recent fixes also cover the Rust CI fallback, packaged sidecar naming, blue
accent styling, daemon version reporting, LAN adoption bootstrap wiring, and
false-positive convergence status for dead peer placeholders.

The remaining work is concentrated in release confidence, physical-device
validation, and deliberately unfinished product capabilities.

## Active queue

| ID | Priority | Remaining work | Current evidence | Definition of done |
|---|---|---|---|---|
| `cross-platform-release` | P0 | Complete fresh Linux and MSVC validation and launch-test the current macOS DMG. | macOS arm64 engine tests, frontend checks, Rust check, and unsigned DMG packaging pass. Linux evidence is container or historical; MSVC evidence is historical. The DMG has been built and checksum-verified but not launched. | Fresh builds, CTest, relevant integration smoke, and honest platform notes for Linux, macOS, and MSVC. Launch the packaged macOS app and verify the sidecar starts. |
| `installer-e2e` | P0 | Exercise packaged installation, sidecar startup, node creation, restart, upgrade, rollback, and uninstall. | A Windows MSI and macOS DMG have been produced, but a complete installer-to-node lifecycle has not been run from the current artifacts. | Install the current package, create/restart/delete a node, verify sidecar startup, test upgrade/uninstall, and record the results without secrets. |
| `invite-pairing-ui` | P1 | Validate the manual three-PC LAN flow through the desktop UI. | The host displays its LAN address, P2P port, and mesh code; LAN Mesh is preselected; local adoption now seeds loopback bootstrap peers; dead placeholders no longer count as synchronized. A local multi-process bootstrap test replicated a binary file. | Verify three physical PCs, firewall reachability, app-driven node creation/adoption, convergence, file reconstruction, and the native save dialog. QR scanning and fingerprint confirmation remain future UX work. |
| `recovery-key-ux` | P1 | Validate recovery-key export, persistence, restore, and the irreversible loss warning end to end. | The implementation intentionally has no escrow or alternate recovery path. The complete headed flow has not been exercised. | Force export, restart and restore a node, verify the recovery key unlocks it, and confirm the UI clearly warns that losing it is unrecoverable. |
| `secondary-indexes` | P1 | Implement or remove the advertised secondary-index capability. | Index names persist in catalog metadata and API responses, but no backend builds or queries them. | Ship one bounded indexed-query path with tests and quota accounting, or remove the option and update all user-facing documentation. |
| `ledger-digest-cache` | P2 | Replace remaining full-scan digest work with incremental, repairable summaries where profiling justifies it. | A digest cache exists, but parts of verification and summary work still scan ledger or storage state. | Benchmark a representative mesh, define the consistency invariant, add corruption/rebuild coverage, and keep a documented full-scan fallback. |
| `storage-reclamation` | P2 | Add safe space reclamation and page-integrity protection for long-lived collections. | Deleted space is not reclaimed, and raw structural pages lack page-level checksums. | Define the on-disk compatibility story, then add crash-safe vacuum and page-checksum tests before implementation. |
| `cross-engine-index` | P2 | Decide whether a SQLite-backed cross-engine index is needed. | The shipped implementation is an fsync'd append log; the SQLite variant remains future work. | Make an explicit performance and maintenance decision, then update architecture, configuration, and API documentation consistently. |
| `vendored-backends` | P2 | Validate optional SQLite, sqlite-vec, DuckDB, and LMDB adapters or explicitly mark them unsupported. | They remain opt-in and are not part of the default shipping build; checked-in-source builds are not currently validated. | If supported, provide approved sources/licenses and run router, API, and restart tests for each. Otherwise document them as unsupported optional adapters. |

## Release verification gaps

These are not known correctness failures, but they remain release blockers until
actually exercised:

- Headed desktop wizard E2E has not been automated.
- Packaged update/upgrade behavior has not been tested.
- Optional vendored backends are not covered by the default build.
- The ONNX path and fallback behavior need a fresh packaged-app verification.
- A physical three-PC LAN run, including firewall behavior, is still pending.

## Documented engine limitations

These are known design limits and remain unresolved:

- B+Tree writes use a coarse-grained mutex and serialize concurrent writers to
  one collection.
- Raw pages lack page-level checksums for torn structural writes; the WAL
  protects documents, not every page write.
- The bump allocator does not reclaim deleted space; vacuum is not implemented.
- A KV document must fit in one 4 KiB page; larger documents must be chunked by
  callers.
- Keys are limited to 64 bytes.
- Multi-hop relay depends on gossip anti-entropy and is not optimized for large
  meshes.
- The tested scale target is 50 nodes on one LAN; larger deployments are not
  designed or tested.
- Transit retention has a failure-detection window before a dead peer is marked
  stale; anti-entropy is the repair path when it returns.
- Ledger chains are per-node, not shared. Equal entry heights with different
  hashes are valid; checkpoint quorum refuses to prune on conflicting tips.
- Optional vendored backends are never downloaded automatically and are off by
  default.
- At-rest encryption requires the node's DEK; losing the exported recovery key
  is unrecoverable by design.
- `secondary_indexes` are metadata only until an indexed-query implementation is
  shipped.
- The cross-engine index currently has only the fsync'd append-log
  implementation.

## Validation policy

When closing an item, record the exact command, environment, result, and any
remaining platform or lifecycle coverage in `STATUS.md`. Do not mark a platform,
installer, model, or integration path verified from code inspection alone.
