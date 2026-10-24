<p align="center">
  <img src="docs/assets/banner.svg" width="100%" alt="De-Sentry — decentralized peer-to-peer database engine">
</p>

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://img.shields.io/badge/De--Sentry-Decentralized%20Database%20Engine-2997ff?style=for-the-badge&logo=database&logoColor=white">
    <img alt="De-Sentry" src="https://img.shields.io/badge/De--Sentry-Decentralized%20Database%20Engine-2997ff?style=for-the-badge&logo=database&logoColor=white">
  </picture>
  <br>
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://img.shields.io/badge/C%2B%2B-17-blue?style=for-the-badge&logo=c%2B%2B&logoColor=white">
    <img alt="C++17" src="https://img.shields.io/badge/C%2B%2B-17-blue?style=for-the-badge&logo=c%2B%2B&logoColor=white">
  </picture>
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://img.shields.io/badge/P2P-Mesh%20Network-orange?style=for-the-badge">
    <img alt="P2P" src="https://img.shields.io/badge/P2P-Mesh%20Network-orange?style=for-the-badge">
  </picture>
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://img.shields.io/badge/CRDT-Conflict--Free-green?style=for-the-badge">
    <img alt="CRDT" src="https://img.shields.io/badge/CRDT-Conflict--Free-green?style=for-the-badge">
  </picture>
  <br>
  <em>🔥 Decentralized. Real-Time. Ultra-Slick.</em>
</p>

---

# 🔮 De-Sentry

> **The mesh is the artifact.** A decentralized, fully peer-to-peer database engine where every node is simultaneously a storage engine, a REST API server, and an encrypted P2P peer. No coordinators. No leaders. No single point of failure.

Built in **C++17** from first principles — the way real database engines are forged. A hand-written paged storage layer with WAL and disk-backed B+Tree (not a wrapper around SQLite/RocksDB/LMDB), a **CRDT document model** for conflict-free multi-writer replication, and a from-scratch encrypted P2P transport. The only third-party dependency is **OpenSSL** (Ed25519/X25519/AES-GCM).

**v2** introduces a **desktop control room**: a Tauri app that serves as the single point of access to the mesh. It discovers hardware, sizes and provisions nodes, supervises `desentryd` processes, and runs in the tray — keeping the mesh in sync in the background. You never hand-edit config files. You never launch a daemon manually. Everything is onboard and works **offline-first** — which is an acceptance test, not an aspiration.

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://img.shields.io/badge/Status-Building%20%26%20Running-brightgreen?style=for-the-badge">
    <img alt="Status" src="https://img.shields.io/badge/Status-Building%20%26%20Running-brightgreen?style=for-the-badge">
  </picture>
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://img.shields.io/badge/Tests-13/13%20C%2B%2B%20%E2%9C%93-brightgreen?style=for-the-badge">
    <img alt="Tests" src="https://img.shields.io/badge/Tests-13/13%20C%2B%2B%20%E2%9C%93-brightgreen?style=for-the-badge">
  </picture>
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://img.shields.io/badge/Integration-4%20Suites%20%E2%9C%93-brightgreen?style=for-the-badge">
    <img alt="Integration" src="https://img.shields.io/badge/Integration-4%20Suites%20%E2%9C%93-brightgreen?style=for-the-badge">
  </picture>
</p>

---

## Why De-Sentry?

Most databases assume a server. You connect to it; it owns the data; if it goes
away, so does your application. De-Sentry removes that assumption entirely.

| | Conventional client/server DB | Traditional distributed DB | **De-Sentry** |
|---|---|---|---|
| **Topology** | One primary; clients connect | Leader + followers, or shard coordinators | Flat peer-to-peer mesh; every node is equal |
| **Writes during a partition** | Unavailable (or failed over) | Quorum-dependent | Fully local, always accepted |
| **Conflict handling** | Locking / MVCC on one node | Consensus (Raft/Paxos) | CRDT join — mathematical convergence |
| **Coordinator** | The server | Leader or coordinator | **None, by design** |
| **Offline operation** | No | Usually no | Full read/write, syncs on reconnect |
| **Storage engine** | Vendor-supplied | Vendor-supplied | Hand-written pager, WAL, B+Tree |
| **Dependencies at runtime** | Driver + server | Many | **Just OpenSSL** |

### What that buys you

- **No single point of failure.** Kill any node; the mesh keeps serving.
- **No leader election.** Nothing to elect, nothing to stall on.
- **No lock contention across the network.** Concurrent writers merge, not block.
- **No cloud required.** A laptop in airplane mode is a fully functional node.
- **No opaque storage.** Every page, log record and index node is in this repo.

---

## Feature Highlights

<table>
<tr>
<td width="50%" valign="top">

### 🧠 Engine
- Hand-written **paged storage** (4 KiB slotted pages)
- **Write-ahead log** with CRC32 and crash recovery
- Disk-backed **B+Tree** with split/merge
- **Buffer pool** with a real eviction policy
- **Five native engines**: KV, columnar, time-series, vector, graph
- Schema validators, ACLs, quotas, retention

</td>
<td width="50%" valign="top">

### 🌐 Mesh
- **CRDT** documents over **hybrid logical clocks**
- **Gossip anti-entropy** with content fingerprints
- **Consistent-hash** placement, RF = 3
- **Transit store** holds writes for offline owners
- **Hash-chained, Ed25519-signed** audit ledger
- Ed25519 / X25519 / AES-256-GCM transport

</td>
</tr>
<tr>
<td width="50%" valign="top">

### 🖥️ Control Room
- **Tauri v2** desktop app — no JS framework, only Tauri packages
- Hardware discovery → node sizing → provisioning
- Tree ⇄ Mesh canvas with live topology
- Tray supervision and background sync
- **Offline AI sizing** (MiniLM + ONNX) with keyword fallback
- QR pairing, recovery keys, no escrow

</td>
<td width="50%" valign="top">

### 🧪 Verifiability
- 13 C++ assert-based suites (no framework)
- Integration tests against **real processes**
- **Airplane-mode** acceptance test
- 50-node chaos soak
- Ledger verification over HTTP
- `STATUS.md` records what was *not* run

</td>
</tr>
</table>

---

## Tech Stack

| Layer | Technology | Role |
|---|---|---|
| **Core Engine** | C++17 | Storage engine (paged I/O, WAL, B+Tree), storage router, CRDT layer, ledger, P2P networking |
| **Crypto** | OpenSSL (EVP) | Ed25519 signing, X25519 ECDH, AES-256-GCM transport and at-rest, SHA-256 hash chains |
| **Desktop app** | Tauri v2 + TypeScript | Control room: node lifecycle, supervision, tray sync — no JS framework, only Tauri packages |
| **Sidecar** | Rust | Port allocation, config generation, process supervision, OS keychain, notifications |
| **Onboard AI** | ONNX Runtime + all-MiniLM-L6-v2 | Description → engine sizing, offline, with a deterministic keyword fallback |
| **Python Client** | Python 3 (stdlib only) | Zero-dependency HTTP client for AI agent bindings |
| **Dashboard** | Single static HTML file | Browser-based node status viewer (no build step, no framework) |
| **Build** | CMake ≥ 3.16, cargo, npm/bun | C++ engine, Rust sidecar, frontend + installers |
| **Containers** | Docker + Docker Compose | Containerized 3-node cluster with integration tests |

---

## Prerequisites

Nothing here is needed to *use* a running node — the REST API and the Python
client have no dependencies at all. This is what it takes to build.

### What each part needs

| To build | You need |
|---|---|
| **Engine** (`desentryd`, `desentry_cli`, tests) | CMake ≥ 3.16, a C++17 compiler, **OpenSSL development headers** (not just the runtime library), a threads library |
| **Desktop app** (`app/`) | the above, plus Rust ≥ 1.77 (MSVC toolchain on Windows), Node ≥ 18 or bun, a webview + tray stack (Linux only, see below) |
| **Python client / integration tests** | Python ≥ 3.8. **No packages** — everything is stdlib. `clients/python/requirements.txt` exists and is deliberately empty |
| **Regenerating the app icons** (rare) | `python -m pip install -r app/scripts/requirements.txt` (Pillow). The icons are committed, so you can skip this |

OpenSSL is the one that catches people out: distributions ship the runtime
library and the headers as **separate packages**, and CMake needs the headers.
`libssl.so` being present is not enough.

### Ubuntu / Debian (22.04+)

```bash
# Engine
sudo apt update
sudo apt install -y build-essential cmake pkg-config libssl-dev git python3

# Desktop app (Tauri v2 needs a webview, a tray backend, and an SVG rasteriser)
sudo apt install -y libwebkit2gtk-4.1-dev libgtk-3-dev libsoup-3.0-dev \
                    libayatana-appindicator3-dev librsvg2-dev libxdo-dev \
                    patchelf curl wget file
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh   # Rust
# Node ≥ 18 via your distro, nvm, or NodeSource
```

On Linux the app stores node keys in the **Secret Service**, so a keyring
daemon (`gnome-keyring` or `kwalletmanager`) must be running at runtime or key
custody will fail with a D-Bus error. Headless machines: run `dbus-run-session`
or build the engine only.

### macOS 14+

```bash
xcode-select --install                      # C/C++ toolchain
brew install cmake openssl@3 node rust      # or rustup, or MacPorts

# Homebrew's OpenSSL is keg-only, so CMake will not find it by itself:
export OPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
```

Export `OPENSSL_ROOT_DIR` in your shell profile, or pass it per-configure with
`-DOPENSSL_ROOT_DIR=...`. Without it you get
`Could NOT find OpenSSL` even though `brew list` shows it installed.

### Windows 11

Install, in this order:

1. **Visual Studio 2022 Build Tools** with the *Desktop development with C++*
   workload (this is the C++ compiler, the Windows SDK, and the MSVC linker).
2. **CMake ≥ 3.16** — <https://cmake.org/download/>, or `winget install Kitware.CMake`.
3. **OpenSSL**, one of:
   - *vcpkg (recommended)*
     ```powershell
     git clone https://github.com/microsoft/vcpkg C:\vcpkg
     C:\vcpkg\bootstrap-vcpkg.bat
     C:\vcpkg\vcpkg install openssl:x64-windows
     ```
     then configure with
     `-DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake`
   - *or* the Win64 OpenSSL installer (**not** the "Light" build — it has no
     headers), then `setx OPENSSL_ROOT_DIR "C:\Program Files\OpenSSL-Win64"`.
4. **Rust (MSVC toolchain)** — <https://rustup.rs>, then
   `rustup default stable-x86_64-pc-windows-msvc`. The GNU toolchain will not
   link against an MSVC-built OpenSSL.
5. **Node ≥ 18** — `winget install OpenJS.NodeJS.LTS`.
6. **WebView2** — already present on Windows 11; on older builds install the
   Evergreen Runtime.

MSYS2/MinGW works for the engine if you `pacman -S mingw-w64-x86_64-openssl
mingw-w64-x86_64-cmake`, but the desktop app expects the MSVC toolchain.

### One thing that is *not* a prerequisite

Nothing in this project downloads a dependency at build or run time by
itself. The vendored storage backends are off by default and CMake **fails**
rather than fetching them; `npm run fetch-model` is the single explicit,
opt-in network step, and the app runs without it on a deterministic keyword
fallback. If a build ever tries to reach the network unprompted, that is a
bug — please report it.

---

## Quickstart

### Just run it (no toolchain)

Download the installer for your OS from the
[Releases page](https://github.com/R1sh1m/De-sentry/releases) (Windows MSI,
macOS .dmg, Linux AppImage/.deb -- published automatically for every `v*`
tag by `.github/workflows/release.yml`). Install, open De-Sentry, and create
your first node in the wizard. Everything -- engine, sidecar, sizing model --
is inside the installer; the app works with no internet.

### One-command setup from source

After cloning, a single script checks prerequisites, builds the engine,
installs app dependencies, fetches the sizing model once, and stages the
sidecar. Re-run it after any pull; every step is idempotent:

```powershell
.\scripts\setup.ps1        # Windows (add -Install to try winget for missing tools)
```

```bash
./scripts/setup.sh          # Linux / macOS
```

Then either a headless mesh or the desktop app:

```powershell
.\scripts\run_cluster.ps1 -Nodes 3   # API on 7701-7703; stop with .\scripts\stop_cluster.ps1
cd app; npm run tauri:dev            # desktop control room (dev)
```

### Build from source (manual steps)

> Prerequisites: see [above](#prerequisites) — CMake ≥ 3.16, a C++17 compiler,
> and OpenSSL **development headers**.

```bash
# Build
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ..
make -j"$(nproc)"

# Run tests (13 suites: acl, at_rest, crdt, crypto, ledger_v2, liveness,
#            network, placement, quota, retention, router, storage, transit)
ctest --output-on-failure
```

Optional vendored storage backends are **off by default and never
downloaded** — place their sources under `third_party/` and turn them on
explicitly (`-DDESENTRY_WITH_SQLITE=ON`, `…_SQLITE_VEC`, `…_DUCKDB`,
`…_LMDB`). A running node will tell you what it actually has:
`curl -s http://127.0.0.1:7701/_engines`.

### Build the desktop app

> Prerequisites: the above, plus Rust ≥ 1.77, Node ≥ 18 (or bun), and on Linux
> the webview/tray packages listed in [Prerequisites](#prerequisites).

```bash
cd app
npm install
npm run fetch-model      # one-time: bundle the sizing model (build step, not runtime)
npm run build            # typecheck + frontend bundle
npm run tauri:dev        # run the control room
npm run tauri:build      # MSI / .dmg / AppImage with versioned desentryd sidecars
```

The engine must be built first — `npm run tauri:build` stages `desentryd` from
`build/` as a versioned sidecar and will stop if it is not there.

If `ort` (the ONNX runtime binding) will not build in your environment, the
sizing model is separable:

```bash
# `--` twice: npm strips the first, and the Tauri CLI forwards what follows
# the second to cargo. `tauri build --no-default-features` is rejected by the
# CLI's own argument parser.
npm run tauri:build -- -- --no-default-features   # keyword heuristic instead
npm run tauri:dev   -- -- --no-default-features
```

The app then sizes collections with the deterministic keyword fallback and
reflects it in the UI rather than presenting a fallback as a model result.

### macOS: exact build and run steps

These steps are for macOS 14 or newer. Run them from the repository root in a
new Terminal session:

```bash
# 1. Install the native and development prerequisites.
xcode-select --install
brew install cmake openssl@3 node rust python
export OPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
export PATH="$(brew --prefix openssl@3)/bin:$PATH"

# 2. Configure and build the C++ engine and tests.
cmake -S . -B build-macos \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DDESENTRY_BUILD_TESTS=ON
cmake --build build-macos --parallel

# 3. Run the C++ tests. If one test hangs, stop it and record the failure.
ctest --test-dir build-macos --output-on-failure

# 4. Install frontend dependencies and run the browser-side checks.
cd app
npm install
npm run typecheck
npm run check:qr
npm run check:css
npm run build

# 5. Stage the matching ARM macOS sidecar and launch the Tauri dev shell.
npm run stage-sidecar
npm run tauri:dev
```

On Apple Silicon, the staged daemon is named
`desentryd-aarch64-apple-darwin`. The Tauri shell must open, start its
supervisor sidecar, and return a hardware-volume scan. Stop it with `Ctrl-C`.

To exercise the optional semantic sizing path, fetch the model explicitly and
then run the app again:

```bash
npm run fetch-model
npm run tauri:dev
```

If the model is unavailable or ONNX cannot compile, use the documented
keyword-only fallback:

```bash
npm run tauri:dev -- -- --no-default-features
```

To produce the macOS installer after the development shell works:

```bash
npm run tauri:build
```

The installer is not considered verified until the generated `.app` and
`.dmg` launch, the sidecar starts, and the node-creation wizard completes
successfully. See [`ISSUES.md`](ISSUES.md) for the complete acceptance
checklist and unresolved release risks.

### Run a 3-node cluster

The desktop app is the supported way to run nodes. These scripts exist for
engine work, where starting a mesh from a terminal and reading its logs
directly is what you actually want. (Windows: `.\scripts\run_cluster.ps1
-Nodes 3 -Supervisor`.)

```bash
cd ..
./scripts/run_cluster.sh 3 --supervisor --engines kv,ts_rollup

# Write on node 0, read from nodes 1 and 2:
curl -s -X PUT http://127.0.0.1:7701/db/users/u1 -d '{"name":"Asha","role":"admin"}'
sleep 1
curl -s http://127.0.0.1:7702/db/users/u1   # -> {"name":"Asha","role":"admin"}
curl -s http://127.0.0.1:7703/db/users/u1   # -> {"name":"Asha","role":"admin"}

./scripts/stop_cluster.sh
```

### Or use Docker Compose

```bash
docker compose up --build          # build image, start 3 nodes + run integration tests
docker compose up node-a node-b node-c   # just the cluster, for manual play
docker compose run tester          # re-run integration tests against a running cluster
```

### CLI

```bash
./build/desentry_cli --api 127.0.0.1:7701 put users u1 '{"name":"Asha"}'
./build/desentry_cli --api 127.0.0.1:7702 get users u1
./build/desentry_cli --api 127.0.0.1:7701 status
```

### Python client

Python ≥ 3.8, and **nothing to install** — the client is stdlib-only. There is
a `requirements.txt` next to it so `pip install -r` works and tells you that
plainly:

```bash
python -m pip install -r clients/python/requirements.txt   # a no-op, by design
```

It is a single module, not an installed package, so put it on the path rather
than importing it as `clients.python.desentry_client` (there are no
`__init__.py` files — that import raises `ModuleNotFoundError`):

```bash
export PYTHONPATH="$PWD/clients/python"      # Windows: $env:PYTHONPATH = "$PWD\clients\python"
```

```python
from desentry_client import DesentryClient

node = DesentryClient("http://127.0.0.1:7701")
node.put("users", "u1", {"name": "Asha"})
print(node.get("users", "u1"))
print(node.verify_ledger())   # {"verified": true, "entries_checked": N}
```

Or, without touching the environment:

```python
import sys; sys.path.insert(0, "clients/python")
from desentry_client import DesentryClient
```

### Integration tests

Also stdlib-only. Build the engine first — these start real `desentryd`
processes and find the binary the same way the cluster scripts do.

```bash
# Start their own cluster and tear it down again:
python3 tests/integration/transit_replay_test.py
python3 tests/integration/airplane_mode_test.py
python3 tests/integration/usb_node_test.py
python3 tests/integration/soak_test.py --nodes 50 --chaos 8 --settle 180

# Runs against an already-running cluster (the Docker Compose `tester` job):
./scripts/run_cluster.sh 3
python3 tests/integration/cluster_integration_test.py
```

### Dashboard

Open `tools/dashboard.html` directly in a browser (no build step, no server)
and point it at a running node's API address to see live node status, the
signed ledger tip, per-collection checksums, known peers, and a scrolling
ledger-entry table with one-click ledger verification.

---

## Troubleshooting

Every one of these is a missing prerequisite, not a bug in the tree.

| What you see | What it means | Fix |
|---|---|---|
| `Could NOT find OpenSSL (missing: OPENSSL_CRYPTO_LIBRARY OPENSSL_INCLUDE_DIR)` | CMake cannot find OpenSSL | Linux: `apt install libssl-dev`. macOS: `export OPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"`. Windows: vcpkg toolchain file, or set `OPENSSL_ROOT_DIR` |
| `fatal error: openssl/evp.h: No such file or directory` | The runtime library is installed, the **headers** are not | Install the `-dev` / `-devel` package, not just the library |
| `LNK1181: cannot open input file 'libcrypto.lib'` | MSVC found headers but no import library, or a MinGW build of OpenSSL | Use the vcpkg `openssl:x64-windows` package and pass `-DCMAKE_TOOLCHAIN_FILE=…\vcpkg.cmake` |
| `'cmake' is not recognized` / `command not found` | CMake is not installed or not on PATH | Install it, and reopen the shell so PATH is picked up |
| `error: linker 'cc' not found` | No C toolchain | `apt install build-essential`, or `xcode-select --install` |
| `Package webkit2gtk-4.1 was not found` · `failed to run custom build command for 'soup3-sys'` | Tauri's Linux webview dependencies are missing | Install the desktop-app packages in [Prerequisites](#prerequisites) |
| `failed to run custom build command for 'libappindicator-sys'` | No tray backend | `apt install libayatana-appindicator3-dev` |
| Rust build fails on `onig_sys` | `tokenizers` builds a small C library | Install a C compiler (`build-essential` / Xcode CLT / MSVC) |
| `ort` fails to build, or `libonnxruntime` is missing | The ONNX runtime is not available in your environment | Either `npm run fetch-model`, or build without it: `npm run tauri:build -- -- --no-default-features` (two `--`; see above) |
| `error: unexpected argument '--no-default-features' found` | The flag reached the Tauri CLI instead of cargo | Add the second `--`: `npm run tauri:dev -- -- --no-default-features` |
| `resource path 'resources\model' doesn't exist` | `app/resources/model/` is missing — it is a build-step download | It only needs to *exist*; the committed `app/resources/model/README.md` keeps it there. Restore that file, or run `npm run fetch-model` |
| `ModuleNotFoundError: No module named 'clients'` | The Python client is a module, not an installed package | `export PYTHONPATH="$PWD/clients/python"` and `from desentry_client import …` |
| `ModuleNotFoundError: No module named 'PIL'` | Only `app/scripts/make-icons.py` needs Pillow | `python -m pip install -r app/scripts/requirements.txt` — or skip it; the icons are committed |
| `CMake Error … DESENTRY_WITH_SQLITE=ON but third_party/…/sqlite3.c is missing` | **Working as intended.** The build never downloads | Vendor the sources per `third_party/README.md`, or leave the option `OFF` |
| `could not find desentryd. Build it first:` / `error: …/build/desentryd not found` | The engine is not built, or sits in a build directory these do not check | Build it. They look in `build/`, `build/RelWithDebInfo/`, `build/Release/`, `build/Debug/`; the Python tests also honour `DESENTRY_ENGINE=/path/to/desentryd` |
| Node starts, then `Address already in use` | Another mesh is still running on 770x/780x | `./scripts/stop_cluster.sh` (or `.\scripts\stop_cluster.ps1`) |
| App reports a keychain / D-Bus error on Linux | No Secret Service is running | Start `gnome-keyring` or KWallet; on headless boxes use `dbus-run-session` |
| Every test passes suspiciously fast after a manual build tweak | `assert()` was compiled out by `NDEBUG` | Test targets need `-UNDEBUG` (`/UNDEBUG` on MSVC) — see `CMakeLists.txt` |

If you hit something not on this list, `STATUS.md` records what has and has
not actually been executed, which is usually the fastest way to tell a
missing prerequisite from an unverified code path.

---

## REST API

Every node serves this API on its local HTTP port (default 7701/7702/7703):

| Method | Path | Purpose |
|--------|------|---------|
| PUT | `/db/:collection/:key` | Upsert a document (JSON body) |
| GET | `/db/:collection/:key` | Fetch a document |
| DELETE | `/db/:collection/:key` | Delete a document (tombstone) |
| GET | `/db/:collection` | List/scan a collection |
| PUT | `/_schema/:collection` | Set a JSON-Schema-subset validator |
| GET | `/_schema/:collection` | Read a collection's schema |
| GET | `/_collections` | List known collections |
| GET | `/_peers` | List known P2P peers |
| GET | `/_status` | Node identity, uptime, peer count, at-rest posture (`encrypt_at_rest`, `at_rest_sealed`) |
| GET | `/_brain` | Compact snapshot: signed ledger tip, per-collection checksums, peers |
| GET | `/_ledger/tip` | Current hash-chain tip + Ed25519 signature |
| GET | `/_ledger/entries?from=&to=` | Bounded page of ledger entries for replay/audit |
| POST | `/_ledger/verify` | Full hash-chain re-verification |
| GET | `/_engines` | Which storage engines this binary actually has |
| GET | `/_collection/:collection` | Collection metadata: engine, ACL, placement |
| PUT | `/_collection/:collection/engine` | Choose the storage engine for a collection |
| PUT | `/_collection/:collection/acl` | Set `{owner_node, private, readers[], parent}` |
| PUT | `/_collection/:collection/placement` | Replication factor and placement policy |
| GET | `/_quota` | Quota split and per-engine usage |
| GET | `/_changes?since=` | Long-poll change feed (signals `truncated` on a gap) |
| GET | `/_transit` · POST `/_transit/claim` · POST `/_transit/expire` | Transit store: pending handoffs, claims, TTL sweep, `load_corrupt` flag |
| POST | `/_checkpoint` | Quorum-gated GC below the checkpoint |
| POST | `/_verify` | Verify a peer's tip against ours (the quorum vote) |
| GET | `/_placement/:collection/:key` | Ring RF subset + `displaced_owners` for a key; actual holders converge to all reachable nodes (see `/_brain` checksums) |
| POST | `/_search/vector/:collection` | k-NN over a vector collection |
| GET | `/_ts/:collection/rollups` | Time-series rollups |
| GET | `/_graph/:collection/:key` | Adjacency for a graph node |

Supervisor-only nodes serve a further `/_supervisor/*` surface — `scan`,
`mounts`, `inspect`, `nodes` (+ per-node `transition`, `recovery-key-exported`),
`manifest`, `pass`, `topology`, `pairing`. It is bound to loopback and is the
app's private control channel; **no data is ever routed through it.**

Every write is broadcast to connected peers and merged via CRDT so all nodes
converge without coordination. Every write is also appended to a SHA-256
hash-chained, Ed25519-signed audit ledger — a tamper-evident history of every
mutation. Reads, writes and gossip are all filtered by the collection's ACL:
a non-reader receives hashes, never bytes.

---

## Architecture

```
                    ┌──────────────────────────────────────────────────────┐
                    │                   desentryd (one peer)               │
                    │                                                      │
   local apps      │  ┌────────────┐       ┌────────────────────────────┐ │
   (curl, your  ───┼─▶│  API Layer │       │       Engine Core          │ │
   app's HTTP      │  │ HTTP/1.1   │◀─────▶│ (storage + CRDT + catalog) │ │
   client)         │  │ REST + WS  │       └────────────┬───────────────┘ │
                    │  └────────────┘                    │                 │
                    │                                    ▼                 │
                    │                     ┌──────────────────────────┐    │
                    │                     │     Storage Engine        │    │
                    │                     │ DiskMgr / BufferPool /    │    │
                    │                     │ WAL / B+Tree / Catalog    │    │
                    │                     │     (local *.dsf file)    │    │
                    │                     └──────────────────────────┘    │
                    │                                    ▲                 │
                    │  ┌─────────────────────────────────┘                 │
                    │  │                                                   │
                    │  ▼                                                   │
                    │ ┌────────────────────────────────────────────────┐  │
                    │ │               Network Layer                     │  │
                    │ │ Identity (Ed25519+X25519) · Secure channel       │  │
                    │ │ TCP wire protocol · UDP LAN discovery           │  │
                    │ │ Gossip / anti-entropy replicator                │  │
                    │ └────────────────────────────────────────────────┘  │
                    └────────────────────────┬─────────────────────────────┘
                                              │  TCP (encrypted, signed frames)
                                              ▼
                                  other peers, symmetric, same shape
```

v2 keeps that shape and adds a second plane above it: app-local **supervisors**
(`supervisor: true`, loopback-only, never elected, never a replication hop)
that handle hardware discovery, fitness ranking, placement, key custody, quota
and node lifecycle. The data plane stays flat P2P — kill every supervisor and
the mesh is functionally unchanged.

For the full design document — goals/non-goals, prior-art comparison, CRDT
consistency model, storage engine internals, network protocol, security model,
hash-chained audit ledger, and roadmap — see
[`docs/architecture.md`](docs/architecture.md). For the v2 layer —
supervisors, the storage router, ledger v2, ACLs and the threat model — see
[`docs/architecture-v2.md`](docs/architecture-v2.md).

For a comparison against an alternate consortium/routing design and what was
adopted from it, see [`docs/comparison.md`](docs/comparison.md).

---

## Directory Structure

```
De-Sentry/
├── README.md                          ← You are here
├── AGENTS.md                          ← How to build, test and work in this repo
├── DESIGN.md                          ← Control-room design language (tokens, components, screens)
├── STATUS.md                          ← What is built, what was verified, what was not
├── ISSUES.md                          ← Known issues and the active work queue
├── CMakeLists.txt                     ← Build system
├── Dockerfile                         ← Container image build
├── docker-compose.yml                 ← 3-node cluster + integration tests
├── docker-entrypoint.sh               ← Container entrypoint (renders config from env)
│
├── include/desentry/                  ← Public headers, organized by layer
│   └── (common, storage, crdt, ledger, security, net, engine, api, supervisor)
├── src/                               ← Implementation (mirrors include/)
│   ├── api/                           ← HTTP/1.1 REST server + routes
│   ├── common/                        ← Status/StatusOr, JSON, byte buffers, platform shim
│   ├── crdt/                          ← CRDT types + HLC
│   ├── engine/                        ← Node engine (ties everything together)
│   ├── ledger/                        ← Hash-chained ledger, transit store, checkpoint, feed
│   ├── net/                           ← P2P networking, gossip, discovery, placement, admission
│   ├── security/                      ← Crypto (Ed25519, X25519, AES-GCM)
│   ├── storage/                       ← Disk manager, buffer pool, WAL, B+Tree, router
│   │   └── engines/                   ← One file per EngineBackend (5 from scratch + 4 opt-in)
│   └── supervisor/                    ← Supervisor-only logic, hardware scan, extra routes
├── apps/
│   ├── desentry_node/                 ← desentryd daemon entrypoint
│   └── desentry_cli/                  ← Thin CLI client
│
├── app/                               ← Desktop control room (Tauri v2)
│   ├── src/                           ← TypeScript frontend (no framework)
│   ├── src-tauri/src/                 ← Rust sidecar: ports, config, supervision, keychain, AI
│   ├── resources/                     ← Engine prototypes; the sizing model lands here
│   └── scripts/                       ← Icon generation, model fetch, sidecar staging
│       └── requirements.txt           ← Pillow, only for regenerating icons
│
├── tests/                             ← Assert-based C++ suites (one binary per file)
│   └── integration/                   ← Python: cluster, transit replay, airplane mode, USB, 50-node soak
├── clients/python/                    ← Zero-dependency Python client (+ an empty requirements.txt)
├── tools/dashboard.html               ← Browser dashboard (no build step)
├── config/                            ← Example node config (JSON)
│
├── docs/
│   ├── LITERATURE.md                  ← Research foundation & annotated bibliography
│   ├── architecture.md                ← Full v1 system design spec
│   ├── architecture-v2.md             ← Supervisors, router, ledger v2, threat model
│   ├── architecture-v3.md             ← Forward-looking design notes
│   ├── comparison.md                  ← Comparison against alternate design
│   ├── project_statement.md           ← Original project brief and scope
│   ├── apple-reference.md             ← Design-language source for DESIGN.md
│   └── original_design/               ← Original consortium/routing design docs (historical)
├── scripts/
│   ├── run_cluster.sh / .ps1          ← Launch a local N-node cluster
│   ├── stop_cluster.sh / .ps1         ← Stop the cluster
│   ├── dsync.py / .ps1 / .sh          ← Git collaboration tooling
│   └── README.md                      ← Collaboration workflow guide
│
├── third_party/                       ← Opt-in vendored backends (never auto-downloaded)
└── .gitignore
```

---

## Research Foundation

De-Sentry is built on decades of foundational research in distributed systems,
database theory, and peer-to-peer architectures. The full annotated bibliography
is in [`docs/LITERATURE.md`](docs/LITERATURE.md).

### What influenced the design

| Area | Key Works | How we adopted it |
|---|---|---|
| **Consistent hashing** | Dynamo (DeCandia et al., SOSP 2007), Chord (Stoica et al., SIGCOMM 2001) | 160 virtual nodes per peer, RF=3 replica sets, ring stability |
| **CRDT convergence** | Preguiça (2018), Shapiro et al. (SSS 2011) | Field-level LWW + OR-Set over HLC timestamps; mathematical convergence without coordination |
| **Gossip anti-entropy** | Demers et al. (PODC 1987), Jelasity et al. (TOCS 2007) | Digest exchange with content fingerprints; repair when bytes differ, not just when clocks advance |
| **Hybrid logical clocks** | Lamport (CACM 1978), Kulkarni et al. (OPODIS 2014) | Causality tracking without global synchronization; H-3/M-3 encoding pass |
| **Write-ahead logging** | Gray & Reuter (1993), Mohan et al. (ARIES, TODS 1992) | WAL with CRC32 validation, redo recovery, torn-tail vs corruption distinction |
| **B+Tree indexing** | Comer (ACM Computing Surveys, 1979) | Disk-backed B+Tree with buffer pool and eviction policy |
| **NoSQL categories** | Han et al. (IEEE ICPCA 2011), Sadalage & Fowler (2012) | Polyglot persistence: KV, columnar, time-series, vector, graph — all behind one router |
| **Vector search** | Malkov & Yashunin (IEEE TPAMI 2020) | HNSW approximate nearest neighbors in `vector_hnsw_lite` |
| **Time-series rollups** | Jensen et al. (IEEE TKDE 2017) | Bucket aggregations with configurable window sizes |
| **Secure transport** | Rescorla (2000); RFC 8446 / 7748 / 8032 | X25519 ECDH + AES-256-GCM authenticated encryption |
| **Audit trails** | Schneier & Kelsey (ACM TOCS 1999), Crosby & Wallach (USENIX Security 2009) | Hash-chained ledger with Ed25519 signatures on every entry |
| **Distributed file systems** | GFS (SOSP 2003), HDFS (IEEE MSST 2010) | Transit store chunking, holder selection, heartbeat-based liveness |

### What we consciously avoided

- **Raft** (Ongaro & Ousterhout, USENIX ATC 2014) — requires a leader; violates the "no coordinator" thesis
- **Paxos** (Lamport, 1998) — complexity, leader requirement, performance overhead
- **Tendermint/BFT** (Buchman, 2016) — reserved for v3 adversarial settings; currently out of scope

### Classic database theory, actually implemented

The systems literature describes a stable set of abstractions. De-Sentry builds
them rather than importing them, so each one is inspectable:

| Textbook concept | Concrete implementation |
|---|---|
| Disk manager, slotted pages, buffer pool | `src/storage/disk_manager.cpp`, `slotted_page.cpp`, `buffer_pool_manager.cpp` |
| Write-ahead logging, ARIES-style redo | `src/storage/wal.cpp` (CRC32, torn-tail vs corruption distinction) |
| B+Tree indexing, split/merge | `src/storage/bplus_tree.cpp` |
| Relational-model constraints | Schema validators via `PUT /_schema/:collection` |
| Query translation and heuristic optimization | `src/api/routes.cpp` scan/filter paths, `src/storage/router.cpp` |
| Deferred/immediate-update recovery | `StorageEngine::Open()` replay; stored roots never trusted |
| Replica placement and replication | Consistent-hash ring (RF=3, 160 vnodes) in `src/net/placement.cpp` + gossip in `src/net/gossip.cpp` — placement, not row fragmentation |
| NoSQL families (KV, document, column, graph) | `src/storage/engines/` + a vector and time-series engine |
| Distributed catalog management | `src/storage/catalog.cpp`, `GET /_brain` |
| CAP / BASE trade-off | AP by design; CRDT convergence instead of consensus |

The full concept-to-code map, chapter-level reading list, and bibliography live
in [`docs/LITERATURE.md`](docs/LITERATURE.md). Deeper per-paper annotations from
the research phase are preserved in
[`research/Research Papers/index.md`](research/Research%20Papers/index.md).

---

## Status

**Verified on 2026-10-22** (Windows 11, MSYS2 UCRT64 GCC 16.2, CMake + MinGW
Makefiles, OpenSSL 3.6): `ctest` is **13/13 green**, and all four integration
suites passed against real `desentryd` processes — airplane mode (18/18
checks), transit replay (22/22), USB node (18/18) and an 8-node chaos soak
(16/16, converged in 0.4 s). A single node has been run end to end — writes,
reads, scans, engine binding, ACLs, quota, a verified signed ledger, and
everything intact across a restart.

**The desktop app builds and runs.** `npm run build` (typecheck + Vite),
`check:css`, `check:qr` and `check:a11y` are green, and `cargo check` is clean
(Rust 1.98.1 MSVC). Earlier dev runs opened the app, started the supervisor
sidecar and reported a real hardware scan of the machine; the ONNX sizing path
is compiled and has a passing model-quality test, with a deterministic keyword
fallback for the model-absent case. A Windows MSI has been produced and its
payload inspected, but the installed-app E2E path (create → restart → delete →
upgrade) has not been walked. Linux, macOS and MSVC engine builds remain open.

[`STATUS.md`](STATUS.md) says exactly what was executed and what was not, plus
the bugs found, the known limits, and the order to build things in on a full
toolchain. Read it before quoting any status from this page.

---

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://img.shields.io/badge/De-Sentry-v2.0.0-2997ff?style=for-the-badge&logo=database&logoColor=white">
    <img alt="De-Sentry v2.0.0" src="https://img.shields.io/badge/De-Sentry-v2.0.0-2997ff?style=for-the-badge&logo=database&logoColor=white">
  </picture>
  <br>
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://img.shields.io/badge/Engine-C%2B%2B17%20%7C%20OpenSSL-2997ff?style=for-the-badge">
    <img alt="Engine" src="https://img.shields.io/badge/Engine-C%2B%2B17%20%7C%20OpenSSL-2997ff?style=for-the-badge">
  </picture>
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://img.shields.io/badge/UI-Tauri%20%2B%20TypeScript-2997ff?style=for-the-badge">
    <img alt="UI" src="https://img.shields.io/badge/UI-Tauri%20%2B%20TypeScript-2997ff?style=for-the-badge">
  </picture>
  <br>
  <em>🚀 Built from first principles. No frameworks. No fetched dependencies.</em>
</p>

---

*De-Sentry — Decentralized Sentinel for AI Agent Data*
