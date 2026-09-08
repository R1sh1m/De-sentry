---
title: De-Sentry — working notes for agents and new contributors
updated: 2026-09-07
applies-to: whole repository
---

# AGENTS.md

Read this before changing anything. It is the short version of "how this
repository works": what the pieces are, where they live, how to build and test
them, and the conventions that are load-bearing rather than cosmetic.

The *why* lives in `docs/architecture.md` (v1 engine) and
`docs/architecture-v2.md` (supervisors, storage router, ledger v2, threat
model). The *what happened* lives in `STATUS.md`. The UI language lives in
`DESIGN.md`.

---

## 1. The one rule that shapes everything else

**Zero fetched dependencies at runtime.** A De-Sentry mesh must come up, form,
replicate, and serve on a laptop in airplane mode with no package manager, no
CDN, and no model download. `tests/integration/airplane_mode_test.py` is the
acceptance test for this and it is not decoration.

Consequences you will run into:

- No test framework. Tests are plain executables that `assert()`.
- No JS framework beyond the Tauri webview. `app/package.json` has exactly one
  runtime dependency (`@tauri-apps/api`); everything else — QR encoding,
  layout, rendering — is written out in `app/src/`.
- Vendored storage backends are **opt-in at configure time and never
  downloaded**. If you turn one on without placing its sources under
  `third_party/`, CMake fails loudly instead of fetching.
- The AI model is fetched by an explicit *build* step (`npm run fetch-model`)
  and bundled into the installer. It is never fetched at runtime, and there is
  a deterministic keyword fallback for when it is absent.

The second rule: **do not rewrite the engine core; extend it.** `src/storage`,
`src/crdt`, `src/net` and `src/ledger` are working v1 code with v2 additions
layered on. Add a backend behind `EngineBackend`; add a route in `routes.cpp`;
do not replace the B+Tree because a library would be easier.

---

## 2. Layout: engine vs app

```
include/desentry/**      public headers, one directory per subsystem
src/common/              Status/StatusOr, JSON, byte buffers, platform shims
src/storage/             pager, WAL, B+Tree, catalog, segment store, ROUTER
src/storage/engines/     one file per EngineBackend implementation
src/crdt/                CRDT document model + hybrid logical clocks
src/ledger/              hash-chained ledger, transit store, checkpoint, feed
src/net/                 transport, gossip, discovery, placement, admission
src/security/            Ed25519 / X25519 / AES-256-GCM over OpenSSL EVP
src/api/                 HTTP server + routes.cpp (the REST surface)
src/supervisor/          supervisor-only logic and its extra routes
src/engine/              NodeEngine: wires all of the above into one node
apps/desentry_node/      the desentryd binary
apps/desentry_cli/       thin HTTP client for demos and scripts

app/src/                 TypeScript frontend (no framework)
app/src-tauri/src/       Rust sidecar: process supervision, ports, keychain,
                         config generation, ONNX sizing, tray, notifications
app/resources/           prototypes.json, bundled model lands here
app/scripts/             icon generation, model fetch, sidecar staging

tests/*.cpp              C++ unit tests (one binary per file)
tests/integration/*.py   multi-process tests driven through the REST API
scripts/run_cluster.*    start a local mesh for engine work
tools/dashboard.html     dependency-free single-file debug dashboard
```

**Which layer does a change belong in?** If it must be true when the desktop
app is not running — replication, ACL enforcement, quota, GC — it belongs in
C++. If it is about *this machine* — which ports, which binary, which
keychain, what the user sees — it belongs in the app. A supervisor is app-local
policy that happens to be written in C++ because it needs engine types; it is
still never on the data path.

---

## 3. Build and test

Per-OS prerequisites (and the error → fix table for when one is missing) live
in `README.md` § Prerequisites / Troubleshooting. The short version: OpenSSL
**development headers** for the engine; Rust ≥ 1.77, Node ≥ 18 and — on Linux
— the webkit2gtk / appindicator / rsvg packages for the app. Python needs
nothing installed; `clients/python/requirements.txt` is empty on purpose, and
`app/scripts/requirements.txt` (Pillow) is only for regenerating icons.

### Engine (C++17, OpenSSL is the only required dependency)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Optional vendored backends (sources must already be in `third_party/`):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DDESENTRY_WITH_SQLITE=ON -DDESENTRY_WITH_SQLITE_VEC=ON \
  -DDESENTRY_WITH_DUCKDB=ON -DDESENTRY_WITH_LMDB=ON
```

`DESENTRY_WITH_SQLITE_VEC=ON` requires `DESENTRY_WITH_SQLITE=ON`. Ask a built
binary what it actually has: `curl -s http://127.0.0.1:7701/_engines`.

Windows: the same commands work; the binary lands in
`build/RelWithDebInfo/desentryd.exe` under the Visual Studio generator and in
`build/desentryd.exe` under Ninja. The cluster scripts check both.

### A local mesh

```bash
./scripts/run_cluster.sh 3 --supervisor --engines kv,ts_rollup
./scripts/stop_cluster.sh
```

```powershell
.\scripts\run_cluster.ps1 -Nodes 3 -Supervisor -Engines kv,ts_rollup
.\scripts\stop_cluster.ps1
```

Ports: data node *i* gets API `7701+i` and P2P `7801+i`; a supervisor gets
`7700`/`7800`; discovery is `7901` for everyone. Logs go to the run directory
the script prints.

### Integration tests

```bash
python3 tests/integration/transit_replay_test.py
python3 tests/integration/airplane_mode_test.py
python3 tests/integration/usb_node_test.py
python3 tests/integration/soak_test.py --nodes 50 --chaos 8 --settle 180

./scripts/run_cluster.sh 3        # cluster_integration_test wants one already up
python3 tests/integration/cluster_integration_test.py
```

They spawn real `desentryd` processes through `tests/integration/harness.py`
and talk to them over HTTP with the standard library only — nothing to `pip
install`, which is why `clients/python/requirements.txt` is empty. Build
first; they locate the binary the same way the cluster scripts do.

### Desktop app

```bash
cd app
npm install            # dev-time only; the shipped app fetches nothing
npm run typecheck      # tsc --noEmit
npm run check:qr       # QR encoder vs the ISO published constants
npm run check:css      # the [hidden] reset that keeps hidden elements hidden
npm run build          # typecheck + vite build
npm run tauri:dev      # dev shell (expects desentryd on PATH or staged)
npm run fetch-model    # one-time: pull MiniLM into app/resources/
npm run stage-sidecar  # copy versioned desentryd next to the Tauri bundler
npm run tauri:build    # stage sidecar + produce MSI / .dmg / AppImage
```

`bun` works in place of `npm` for every one of these if you prefer it; nothing
in the tree depends on which one you use.

---

## 4. Conventions that are load-bearing

**`Status` / `StatusOr`, never exceptions.** `include/desentry/common/status.h`.
Every fallible call returns one; check it. `StatusOr<T>` holds the value —
`std::move(x.value())` to take ownership out of it. There are no `throw`
statements in engine code and adding one will surprise callers that have no
handler anywhere up the stack.

**`ByteWriter` / `ByteReader` for every codec.**
`include/desentry/common/byte_buffer.h`. All on-disk and on-wire encoding goes
through them: fixed little-endian integers, length-prefixed bytes, bounds
checked on read. Do not `memcpy` a struct and do not use `std::ostream`. A
reader that runs past the end returns a failed `Status`; it does not read
adjacent memory.

**Tests assert, and assertions must stay live.** Test binaries are compiled
with `-UNDEBUG` (`/UNDEBUG` on MSVC) *after* the build type's `-DNDEBUG`, in
`CMakeLists.txt`. Without that, a RelWithDebInfo build compiles every `assert`
into nothing and the whole suite passes while checking nothing. If you add a
test target by hand, carry that flag over.

**One test file, one binary, one `add_test`.** `tests/*_test.cpp` is globbed;
adding a file is enough. Tests must not need a network, a fixture directory
that already exists, or a specific `/tmp` — use the portable temp-directory
helper the existing tests use.

**Platform code lives in `platform.h`/`platform.cpp`, nowhere else.** No
`<unistd.h>`, no `<sys/socket.h>`, no `socketpair()`, no `close()` in a test or
a subsystem. If you need a POSIX facility on Windows, add it to the shim (as
`SocketPair` was) and call the shim.

**The app is the single point of access.** Nothing in `app/` should ever tell
a user to edit `node.json` or run `desentryd` by hand, and the Rust side owns
config generation (`configgen.rs`) so hand-editing is never necessary.
`scripts/run_cluster.*` is the deliberate exception, for engine work.

**Supervisors are never on the data path.** `supervisor: true` implies a
loopback-only API bind, no discovery advertisement, no election, no
replication hop. The test for a change here is: kill every supervisor and the
mesh must be functionally unchanged.

**No coordinator, ever.** The data plane is flat P2P — see
`docs/comparison.md` §2 for why this was chosen over a ROOT/coordinator
design. A change that introduces an elected leader on the data path is a
change to the project's thesis, not a refactor.

**Licences are a constraint, not a preference.** No GPL (rules out Xapian), no
AGPL (rules out TDengine), no server-process dependencies (rules out
ClickHouse/QuestDB). Anything vendored must be permissive and must sit under
`third_party/` with its licence.

### Frontend conventions

- No framework, no bundled runtime dependency beyond `@tauri-apps/api`.
- Never `innerHTML` with anything a peer or a user typed — collection names and
  document keys arrive from the mesh. Use the DOM helpers in `util/dom.ts`, or
  escape. (v1's `tools/dashboard.html` got this wrong; it now escapes.)
- **`[hidden] { display: none !important }` must stay in the `styles.css`
  reset.** The `hidden` attribute works only through the user-agent
  stylesheet, which loses to any author rule that sets `display` -- so without
  it, hiding an element does nothing. `npm run check:css` enforces this.
- Design tokens come from `app/src/tokens.css` and are documented in
  `DESIGN.md`. Only the status hues (converged / lagging / offline /
  supervisor) are allowed to carry meaning through colour.
- The app degrades honestly in a browser: `isTauri()` is false, sidecar calls
  raise `NoSidecarError`, and the UI says so instead of pretending.

### Rust sidecar conventions

- Never hold the node-map mutex across a wait (process start, restart backoff,
  HTTP round trip). Clone what you need and drop the guard.
- Node lifecycle is `discovered → allocated → provisioned → running →
  degraded → reclaimed`; transitions go through `nodes.rs` so the tray, the
  notifications and the UI all see the same state.
- Anything that touches a secret goes through `keychain.rs` (Windows
  Credential Manager / macOS Keychain / Linux Secret Service). There is no
  escrow and no recovery path other than the user's exported recovery key.

---

## 5. Where to look first

| You want to change | Start at |
| --- | --- |
| A new storage engine | `include/desentry/storage/router.h`, then `src/storage/engines/` |
| The REST surface | `src/api/routes.cpp` (data) or `src/supervisor/supervisor_routes.cpp` |
| Replication / gossip | `src/net/gossip.cpp`, `src/net/placement.cpp` |
| GC and checkpoints | `src/ledger/checkpoint.cpp` (quorum gate lives here) |
| Access control | `Catalog::CollectionMeta`, enforced in `routes.cpp` + the gossip byte filter |
| What the user sees | `app/src/views/`, tokens in `app/src/tokens.css` |
| Process supervision | `app/src-tauri/src/nodes.rs`, `ports.rs`, `http.rs` |
| Engine sizing from a description | `app/src-tauri/src/ai.rs` + `app/resources/prototypes.json` |

---

## 6. Before you call something done

1. `ctest --test-dir build --output-on-failure` is green.
2. The relevant integration test ran against real processes.
3. `cd app && npm run build` is clean (it typechecks first).
4. If you touched anything a peer sends: it is bounds-checked on read.
5. If you touched anything user-facing: it is escaped on render.
6. `STATUS.md` says what you verified **and what you did not**. An unrun test
   is never reported as a passing one.
