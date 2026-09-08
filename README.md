# De-Sentry

A decentralized, fully peer-to-peer database engine. No client-server split:
every node is simultaneously a storage engine, a REST API server for local
applications, and a P2P peer that gossips writes to every other node. There
is no coordinator, no leader, and no single point of failure by design.

Built in C++17 from first principles, the way real database engines are
built: a hand-written paged storage layer with a write-ahead log and a
disk-backed B+Tree (not a wrapper around SQLite/RocksDB/LMDB), a CRDT
document model for conflict-free multi-writer replication, and a from-scratch
encrypted P2P transport — no networking or database framework dependencies.
The only third-party dependency is OpenSSL (for Ed25519/X25519/AES-GCM).

v2 adds a desktop control room on top: a Tauri app that is the **single point
of access** to the mesh. It discovers hardware, sizes and provisions nodes,
supervises the `desentryd` processes, and runs in the tray keeping the mesh in
sync in the background. You never hand-edit `node.json` and never launch a
daemon yourself. Everything it needs is onboard — the app is fully functional
with no internet, which is an acceptance test
(`tests/integration/airplane_mode_test.py`), not an aspiration.

> **Course project** — validated at 3 nodes running locally in a full mesh.
> See [Project Statement](Project_Statement/project_statement.md) for the
> original brief and scope. Read [`STATUS.md`](STATUS.md) before trusting any
> claim here: the engine is built and tested, the desktop app is not yet
> compiled, and STATUS.md says exactly which is which.

---

## Tech Stack

| Layer | Technology | Role |
|---|---|---|
| **Core Engine** | C++17 | Storage engine (paged I/O, WAL, B+Tree), storage router, CRDT layer, ledger, P2P networking |
| **Crypto** | OpenSSL (EVP) | Ed25519 signing, X25519 ECDH, AES-256-GCM transport and at-rest, SHA-256 hash chains |
| **Desktop app** | Tauri v2 + TypeScript | Control room: node lifecycle, supervision, tray sync — no JS framework, one runtime dependency |
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

### Build from source

> Prerequisites: see [above](#prerequisites) — CMake ≥ 3.16, a C++17 compiler,
> and OpenSSL **development headers**.

```bash
# Build
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ..
make -j"$(nproc)"

# Run tests (9 suites: crdt, crypto, storage, network, router, acl,
#            placement, ledger_v2, quota)
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
npm run tauri:build -- --no-default-features   # keyword heuristic instead
```

The app then sizes collections with the deterministic keyword fallback and
says so in the UI rather than presenting a fallback as a model result.

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
| `ort` fails to build, or `libonnxruntime` is missing | The ONNX runtime is not available in your environment | Either `npm run fetch-model`, or build without it: `npm run tauri:build -- --no-default-features` |
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
| GET | `/_status` | Node identity, uptime, peer count |
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
| GET | `/_transit` · POST `/_transit/claim` · POST `/_transit/expire` | Transit store: pending handoffs, claims, TTL sweep |
| POST | `/_checkpoint` | Quorum-gated GC below the checkpoint |
| POST | `/_verify` | Verify a peer's tip against ours (the quorum vote) |
| GET | `/_placement/:collection/:key` | Which nodes own a key, and why |
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
├── README.md                       ← You are here
├── AGENTS.md / AGENT.md            ← How to build, test and work in this repo
├── DESIGN.md                       ← Control-room design language (tokens, components, screens)
├── STATUS.md                       ← What is built, what was verified, what was not
├── CMakeLists.txt                  ← Build system
├── Dockerfile                      ← Container image build
├── docker-compose.yml              ← 3-node cluster + integration tests
├── docker-entrypoint.sh            ← Container entrypoint (renders config from env)
│
├── include/desentry/               ← Public headers, organized by layer
│   └── (common, storage, crdt, ledger, security, net, engine, api, supervisor)
├── src/                            ← Implementation (mirrors include/)
│   ├── api/                        ← HTTP/1.1 REST server + routes
│   ├── common/                     ← Status/StatusOr, JSON, byte buffers, platform shim
│   ├── crdt/                       ← CRDT types + HLC
│   ├── engine/                     ← Node engine (ties everything together)
│   ├── ledger/                     ← Hash-chained ledger, transit store, checkpoint, feed
│   ├── net/                        ← P2P networking, gossip, discovery, placement, admission
│   ├── security/                   ← Crypto (Ed25519, X25519, AES-GCM)
│   ├── storage/                    ← Disk manager, buffer pool, WAL, B+Tree, router
│   │   └── engines/                ← One file per EngineBackend (5 from scratch + 4 opt-in)
│   └── supervisor/                 ← Supervisor-only logic, hardware scan, extra routes
├── apps/
│   ├── desentry_node/              ← desentryd daemon entrypoint
│   └── desentry_cli/               ← Thin CLI client
│
├── app/                            ← Desktop control room (Tauri v2)
│   ├── src/                        ← TypeScript frontend (no framework)
│   ├── src-tauri/src/              ← Rust sidecar: ports, config, supervision, keychain, AI
│   ├── resources/                  ← Engine prototypes; the sizing model lands here
│   └── scripts/                    ← Icon generation, model fetch, sidecar staging
│       └── requirements.txt        ← Pillow, only for regenerating icons
│
├── tests/                          ← Assert-based C++ suites (9 binaries, one per file)
│   └── integration/                ← Python: cluster, transit replay, airplane mode, USB, 50-node soak
├── clients/python/                 ← Zero-dependency Python client (+ an empty requirements.txt)
├── tools/dashboard.html            ← Browser dashboard (no build step)
├── config/                         ← Example node config (JSON)
│
├── docs/
│   ├── architecture.md             ← Full v1 system design spec
│   ├── architecture-v2.md          ← Supervisors, router, ledger v2, threat model
│   ├── comparison.md               ← Comparison against alternate design
│   ├── apple-reference.md          ← Unmodified design-language source for DESIGN.md
│   └── original_design/            ← Original consortium/routing design docs (historical)
├── scripts/
│   ├── run_cluster.sh / .ps1       ← Launch a local N-node cluster
│   ├── stop_cluster.sh / .ps1      ← Stop the cluster
│   ├── dsync.py / .ps1 / .sh       ← Git collaboration tooling
│   └── README.md                   ← Collaboration workflow guide
│
├── Project_Statement/              ← Original course project brief
├── Research_Docs/                  ← Annotated bibliography and research
├── third_party/                    ← Opt-in vendored backends (never auto-downloaded)
└── .gitignore
```

---

## Research Foundation

Key papers underpinning the design — full annotated bibliography in
[`Research_Docs/`](Research_Docs/):

- **Dynamo** (DeCandia et al., SOSP 2007) — node-owned data + last-write-wins + gossip
- **Distributed Snapshots** (Chandy & Lamport, 1985) — brain file = consistent global snapshot
- **Raft** (Ongaro & Ousterhout, 2014) — consensus baseline we consciously simplify
- **CAP Theorem** (Brewer, 2000; Gilbert & Lynch, 2002) — our AP trade-off justification
- **CRDTs** (Preguiça, 2018) — theoretical basis for conflict-free node ownership

---

## Status

**The engine builds and runs.** On Windows 11 (MSYS2 UCRT64 GCC 16.2, CMake
4.4.2 + Ninja, OpenSSL 3.6.4): `ctest` is 9/9 green, and the transit-replay,
airplane-mode, USB-node and soak integration suites all pass against real
`desentryd` processes. A single node has been run end to end — writes, reads,
scans, engine binding, ACLs, quota, a verified signed ledger, and everything
intact across a restart.

**The desktop app has not been built.** The Rust sidecar has never been
compiled — no cargo on the machine used — so no installer exists and the
control room has not been run. The frontend typechecks and bundles, and the QR
encoder passes conformance checks against the ISO published constants, but
that is the whole of it.

Nothing here has been built on Linux, macOS, or with MSVC.

[`STATUS.md`](STATUS.md) says exactly what was executed and what was not, plus
the bugs found, the known limits, and the order to build things in on a full
toolchain. Read it before quoting any status from this page.

---

*De-Sentry — Decentralized Sentinel for AI Agent Data*
