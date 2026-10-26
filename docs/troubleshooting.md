# Troubleshooting Guide

This guide details common build, linking, runtime, and dependency errors encountered across supported platforms, along with their verified solutions.

---

## Error → Resolution Matrix

Every entry below represents a configuration or environment issue rather than an engine defect.

| Error Message or Symptom | Root Cause | Verified Fix |
|---|---|---|
| `Could NOT find OpenSSL (missing: OPENSSL_CRYPTO_LIBRARY OPENSSL_INCLUDE_DIR)` | CMake cannot locate OpenSSL library or headers | **Linux**: `sudo apt install libssl-dev`<br>**macOS**: `export OPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"`<br>**Windows**: Provide vcpkg toolchain file or set `OPENSSL_ROOT_DIR` environment variable |
| `fatal error: openssl/evp.h: No such file or directory` | OpenSSL runtime binary is installed, but C/C++ development headers are missing | Install the `-dev` / `-devel` distribution package (e.g. `libssl-dev`), not just the runtime package |
| `LNK1181: cannot open input file 'libcrypto.lib'` | MSVC found OpenSSL headers but cannot locate the matching MSVC import library | Use the vcpkg `openssl:x64-windows` package and pass `-DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake` |
| `'cmake' is not recognized` / `command not found` | CMake is not installed or not in current shell `PATH` | Install CMake and reopen terminal to refresh path variables |
| `error: linker 'cc' not found` | C toolchain is not installed | **Linux**: `sudo apt install build-essential`<br>**macOS**: `xcode-select --install` |
| `Package webkit2gtk-4.1 was not found` · `failed to run custom build command for 'soup3-sys'` | Missing Linux webview runtime and development packages for Tauri v2 | Install Linux packages: `sudo apt install libwebkit2gtk-4.1-dev libgtk-3-dev libsoup-3.0-dev librsvg2-dev` |
| `failed to run custom build command for 'libappindicator-sys'` | Missing Linux system tray development library | Install tray backend: `sudo apt install libayatana-appindicator3-dev` |
| Rust build fails on `onig_sys` | `tokenizers` crate requires a native C compiler | Ensure MSVC or GCC/Clang is installed and available on `PATH` |
| `ort` fails to build, or `libonnxruntime` is missing | ONNX Runtime binding cannot compile or link in current environment | Run `npm run fetch-model` first, or compile without ONNX: `npm run tauri:build -- -- --no-default-features` (note double `--`) |
| `error: unexpected argument '--no-default-features' found` | Flag was parsed by Tauri CLI instead of forwarded to Cargo | Add the second `--` separator: `npm run tauri:dev -- -- --no-default-features` |
| `resource path 'resources\model' doesn't exist` | Directory `app/resources/model/` was removed or not initialized | Restore `app/resources/model/README.md` from git, or run `npm run fetch-model` |
| `ModuleNotFoundError: No module named 'clients'` | Python client imported as package instead of added to module path | Run `export PYTHONPATH="$PWD/clients/python"` (PowerShell: `$env:PYTHONPATH="$PWD\clients\python"`), then `from desentry_client import DesentryClient` |
| `curl: (22) The requested URL returned error: 400` or invalid JSON syntax in PowerShell | PowerShell strips unescaped double quotes inside single-quoted `-d` arguments | Use the Python SDK (`desentry_client`), use `Invoke-RestMethod` with `ConvertTo-Json`, or escape inner quotes: `curl -X PUT ... -d '{\"key\":\"value\"}'` |
| `ModuleNotFoundError: No module named 'PIL'` | Missing Pillow library for icon generation | Run `python -m pip install -r app/scripts/requirements.txt` (only needed if regenerating icons; prebuilt icons are committed) |
| `CMake Error … DESENTRY_WITH_SQLITE=ON but third_party/…/sqlite3.c is missing` | **Intended behavior**: zero dependencies are fetched over the network | Place source code into `third_party/` per `third_party/README.md`, or leave the flag `OFF` |
| `could not find desentryd. Build it first:` / `error: …/build/desentryd not found` | C++ engine has not been built before running tests or packaging app | Build the engine first. Scripts look in `build/`, `build/RelWithDebInfo/`, `build/Release/`, `build/Debug/`. Integration tests also accept `DESENTRY_ENGINE=/path/to/desentryd` |
| Node starts, then fails with `Address already in use` | A previously launched cluster is still holding ports 770x/780x | Stop background nodes: `./scripts/stop_cluster.sh` (or `.\scripts\stop_cluster.ps1`) |
| Desktop app reports a keychain / D-Bus error on Linux | No Secret Service provider is available in the desktop session | Launch `gnome-keyring` or KWallet. On headless servers, wrap execution with `dbus-run-session` |
| All tests pass suspiciously fast after a manual build tweak | Assertions were stripped out by `NDEBUG` | Test executables require `-UNDEBUG` (`/UNDEBUG` on MSVC) so `assert()` remains active |

---

## Verifying Test & Platform Status

To distinguish whether an error is a local environment issue or an unverified code path, consult [`STATUS.md`](../STATUS.md). It explicitly distinguishes between:
- What has been compiled and verified green with real processes.
- What has not yet been executed in CI or on specific platforms.
