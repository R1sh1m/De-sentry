# AGENT.md

Some tools look for `AGENT.md`, others for `AGENTS.md`. This repository keeps
the real content in **[AGENTS.md](AGENTS.md)** — read that.

The 60-second version:

- **Zero fetched dependencies at runtime.** The mesh must work in airplane
  mode; `tests/integration/airplane_mode_test.py` proves it. No test
  framework, no JS framework, no runtime model download.
- **Extend the engine core, do not rewrite it.** New storage engines go behind
  `EngineBackend` in `src/storage/engines/`.
- **`Status` / `StatusOr`, never exceptions.** `ByteWriter` / `ByteReader` for
  every codec. Platform differences only in `platform.h` / `platform.cpp`.
- **Tests assert, and `-UNDEBUG` keeps those assertions live** in optimised
  builds. Do not drop that flag.
- **Supervisors are never on the data path, and there is no coordinator.**
  Kill every supervisor and the mesh must be functionally unchanged.

Build and test:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j
ctest --test-dir build --output-on-failure
./scripts/run_cluster.sh 3 --supervisor
cd app && npm run build          # tsc --noEmit + vite build
cd app && npm run tauri:build    # MSI / .dmg / AppImage
```

Companion docs: `DESIGN.md` (UI language), `STATUS.md` (what is built and what
is verified), `docs/architecture.md` (v1 engine), `docs/architecture-v2.md`
(supervisors, router, ledger v2, threat model).
