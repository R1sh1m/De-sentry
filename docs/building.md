# Building & Installing De-Sentry

This guide covers building the De-Sentry C++17 engine, the Tauri v2 desktop control room, running integration tests, and configuring optional vendored storage engines.

---

## Prerequisites Overview

Zero dependencies are fetched at runtime. Only OpenSSL development headers are required for the C++ engine.

| Component | Prerequisites |
|---|---|
| **Engine** (`desentryd`, `desentry_cli`, tests) | CMake ≥ 3.16, C++17 compiler, **OpenSSL development headers** (not just runtime), threads library |
| **Desktop App** (`app/`) | The above, plus Rust ≥ 1.77 (MSVC on Windows), Node ≥ 18 (or bun), webview + tray stack (Linux only) |
| **Python Client & Tests** | Python ≥ 3.8 (standard library only; `clients/python/requirements.txt` is intentionally empty) |
| **Icon Generation** (optional) | Python with Pillow (`app/scripts/requirements.txt`). Built icons are committed in git. |

> **Important**: Linux distributions ship the runtime library (`libssl.so`) and C/C++ development headers (`openssl/evp.h`) in separate packages. CMake requires the development package.

---

## Platform Setup

### Ubuntu / Debian (22.04+)

```bash
# Engine dependencies
sudo apt update
sudo apt install -y build-essential cmake pkg-config libssl-dev git python3

# Desktop app dependencies (Tauri v2 webview, tray backend, SVG rasterizer)
sudo apt install -y libwebkit2gtk-4.1-dev libgtk-3-dev libsoup-3.0-dev \
                    libayatana-appindicator3-dev librsvg2-dev libxdo-dev \
                    patchelf curl wget file

# Rust toolchain
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh

# Node.js ≥ 18 (via distribution packages, nvm, or NodeSource)
```

> **Linux Keyring Note**: The desktop app stores node keys in the **Secret Service**. A keyring daemon (`gnome-keyring` or `kwalletmanager`) must be active at runtime. On headless systems, execute under `dbus-run-session` or build only the engine.

---

### macOS 14+ (Apple Silicon & Intel)

```bash
# 1. Install toolchains
xcode-select --install
brew install cmake openssl@3 node rust python

# 2. Configure OpenSSL paths (Homebrew OpenSSL is keg-only)
export OPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
export PATH="$(brew --prefix openssl@3)/bin:$PATH"
```

To configure and build the C++ engine:

```bash
cmake -S . -B build-macos \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DDESENTRY_BUILD_TESTS=ON
cmake --build build-macos --parallel
ctest --test-dir build-macos --output-on-failure
```

---

### Windows 11

Install in order:

1. **Visual Studio 2022 Build Tools**: Select *Desktop development with C++* (includes MSVC compiler, Windows SDK, linker).
2. **CMake ≥ 3.16**: `winget install Kitware.CMake` or download from [cmake.org](https://cmake.org/download/).
3. **OpenSSL**:
   - *Via vcpkg (recommended)*:
     ```powershell
     git clone https://github.com/microsoft/vcpkg C:\vcpkg
     C:\vcpkg\bootstrap-vcpkg.bat
     C:\vcpkg\vcpkg install openssl:x64-windows
     ```
     Configure with: `-DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake`
   - *Via Win64 OpenSSL installer*: Use the full installer (not the "Light" version, which omits headers), then set:
     `setx OPENSSL_ROOT_DIR "C:\Program Files\OpenSSL-Win64"`
4. **Rust (MSVC toolchain)**: Install via [rustup.rs](https://rustup.rs), then run:
   `rustup default stable-x86_64-pc-windows-msvc`
5. **Node.js ≥ 18**: `winget install OpenJS.NodeJS.LTS`
6. **WebView2**: Built into Windows 11.

---

## One-Command Source Setup

The repository provides automated setup scripts that verify prerequisites, build the engine, install frontend packages, bundle the sizing model, and stage sidecars:

```powershell
# Windows PowerShell (use -Install to attempt winget installation for missing prerequisites)
.\scripts\setup.ps1
```

```bash
# Linux / macOS
./scripts/setup.sh
```

---

## Manual Engine Build

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ..
cmake --build . -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

# Run all 13 C++ assert-based test suites
ctest --output-on-failure
```

### Desktop app fallback build

The desktop app's optional ONNX sizing model is enabled by default. Environments
that cannot link the native ONNX runtime can still compile and test the
deterministic keyword fallback:

```bash
cd app/src-tauri
mkdir -p binaries
touch binaries/desentryd-$(rustc -vV | sed -n 's/^host: //p')
cargo check --no-default-features
cargo test --no-default-features
```

Password verifier salts are generated directly from the operating system
entropy source through the existing `getrandom` dependency, so the fallback
build does not depend on an optional `rand_core` feature being enabled by a
transitive dependency.

### Optional Storage Backends

De-Sentry includes 5 native zero-dependency engines: KV, columnar, time-series, vector HNSW, and graph. Additional storage engines are opt-in, must be placed in `third_party/`, and are **never downloaded automatically**:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DDESENTRY_WITH_SQLITE=ON \
  -DDESENTRY_WITH_SQLITE_VEC=ON \
  -DDESENTRY_WITH_DUCKDB=ON \
  -DDESENTRY_WITH_LMDB=ON
```

Query what engines a running node has enabled:
```bash
curl -s http://127.0.0.1:7701/_engines
```

---

## Desktop Control Room Build

The desktop application is built with Tauri v2 and TypeScript (no frontend framework).

```bash
cd app
npm install
npm run fetch-model      # One-time build step: bundles MiniLM sizing model into app/resources/
npm run build            # Typecheck (tsc) + Vite bundle
npm run stage-sidecar    # Stages versioned desentryd from build/
npm run tauri:dev        # Launch development shell
npm run tauri:build      # Generate production installer (MSI / .dmg / AppImage)
```

### Keyword-Only Sizing Fallback (Without ONNX)

If your environment cannot compile `ort` (ONNX runtime bindings), build without the ONNX feature:

```bash
npm run tauri:build -- -- --no-default-features
npm run tauri:dev   -- -- --no-default-features
```

The app will size collections using the deterministic keyword fallback and reflect that status cleanly in the UI.

---

## Running Local Clusters

Start a multi-node mesh for development and testing:

```bash
# Linux / macOS: 3 data nodes + supervisor
./scripts/run_cluster.sh 3 --supervisor --engines kv,ts_rollup  # NOTE: quote the engine list if it contains commas, e.g. "kv,ts_rollup,vector_hnsw_lite"
./scripts/stop_cluster.sh
```

```powershell
# Windows PowerShell
.\scripts\run_cluster.ps1 -Nodes 3 -Supervisor -Engines "kv,ts_rollup"  # NOTE: quote the engine list if it contains commas
.\scripts\stop_cluster.ps1
```

> **Important**: The `-Engines` argument accepts a **comma-separated list** of engine names. On Windows PowerShell, **always quote the list** (e.g. `"kv,ts_rollup,vector_hnsw_lite"`). Unquoted lists are split incorrectly and cause a config error (`unknown default_engine: kv ts_rollup ...`).

Default port scheme:
- Supervisor: API `7700`, P2P `7800`
- Node *i*: API `7701+i`, P2P `7801+i`
- Discovery UDP: `7901`

---

## Testing

```bash
# 1. C++ unit tests
ctest --test-dir build --output-on-failure

# 2. Integration tests (starts real desentryd processes via Python stdlib)
python3 tests/integration/transit_replay_test.py
python3 tests/integration/airplane_mode_test.py
python3 tests/integration/usb_node_test.py
python3 tests/integration/soak_test.py --nodes 50 --chaos 8 --settle 180

# 3. Cluster integration against a running cluster
./scripts/run_cluster.sh 3
python3 tests/integration/cluster_integration_test.py
```
