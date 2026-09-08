#!/usr/bin/env bash
# One-command developer bootstrap for De-Sentry on Linux and macOS.
#
# Checks prerequisites, builds the C++ engine, installs the app's
# dependencies, fetches the sizing model (once) and stages the desentryd
# sidecar -- everything after cloning that README.md used to list as five
# separate manual steps. Every step is idempotent: re-running this after a
# pull only rebuilds what changed.
#
# Run from the repository root:
#     ./scripts/setup.sh
#
# Afterwards either run a headless mesh:
#     ./scripts/run_cluster.sh 3
# or the desktop control room:
#     cd app && npm run tauri:dev
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

have() { command -v "$1" >/dev/null 2>&1; }
ver() { "$1" --version 2>/dev/null | head -n 1 || true; }

missing=()
need() {
  if have "$1"; then
    echo "  [ok] $1 $(ver "$1")"
  else
    echo "  [!!] $1 missing -- $2"
    missing+=("$1")
  fi
}

echo "1/5 prerequisites"
if [[ "$(uname)" == "Darwin" ]]; then
  need cmake "brew install cmake"
  need ninja "brew install ninja"
  need cargo "brew install rustup-init && rustup-init -y"
  need node "brew install node@22"
  need python3 "brew install python@3"
  if ! [[ -d "/opt/homebrew/opt/openssl@3/include/openssl" || -d "/usr/local/opt/openssl@3/include/openssl" ]]; then
    echo "  [!!] OpenSSL headers not found -- brew install openssl@3"
    missing+=("openssl-headers")
  else
    echo "  [ok] openssl-headers (Homebrew)"
  fi
else
  need cmake "sudo apt install cmake   (build-essential pkg-config libssl-dev)"
  need ninja "sudo apt install ninja-build"
  need g++ "sudo apt install build-essential"
  need cargo "curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh"
  need node "nodejs 18+ (https://nodejs.org) or your package manager"
  need python3 "sudo apt install python3"
  if ! pkg-config --exists openssl 2>/dev/null && ! [[ -f /usr/include/openssl/evp.h ]]; then
    echo "  [!!] OpenSSL headers not found -- sudo apt install libssl-dev"
    missing+=("openssl-headers")
  else
    echo "  [ok] openssl-headers"
  fi
fi

if [[ "${#missing[@]}" -gt 0 ]]; then
  echo "Missing tools: ${missing[*]}. Install them, then re-run ./scripts/setup.sh" >&2
  exit 1
fi

echo "2/5 engine (CMake configure + build)"
GENERATOR=()
if have ninja; then GENERATOR=(-G Ninja); fi
if ! cmake -S "$PROJECT_ROOT" -B "$PROJECT_ROOT/build" "${GENERATOR[@]}" -DCMAKE_BUILD_TYPE=RelWithDebInfo; then
  # A stale cache (different generator/toolchain than last time) fails
  # exactly like this; wiping it and retrying once is safe.
  echo "  configure failed -- clearing a possibly stale CMake cache and retrying once"
  rm -f "$PROJECT_ROOT/build/CMakeCache.txt"
  rm -rf "$PROJECT_ROOT/build/CMakeFiles"
  cmake -S "$PROJECT_ROOT" -B "$PROJECT_ROOT/build" "${GENERATOR[@]}" -DCMAKE_BUILD_TYPE=RelWithDebInfo
fi
cmake --build "$PROJECT_ROOT/build" -j

ENGINE_BIN=""
for candidate in "$PROJECT_ROOT/build/desentryd" "$PROJECT_ROOT/build/RelWithDebInfo/desentryd"; do
  if [[ -x "$candidate" ]]; then ENGINE_BIN="$candidate"; break; fi
done
[[ -n "$ENGINE_BIN" ]] || { echo "build finished but desentryd was not produced" >&2; exit 1; }
echo "  engine ready: $ENGINE_BIN ($(du -m "$ENGINE_BIN" | cut -f1) MiB)"

echo "3/5 app dependencies (npm install)"
(
  cd "$PROJECT_ROOT/app"
  npm install --no-audit --no-fund

  echo "4/5 sizing model (one-time download, skipped when present)"
  node scripts/fetch-model.mjs

  echo "5/5 stage desentryd as the Tauri sidecar"
  node scripts/stage-sidecar.mjs
)

echo ""
echo "Setup complete. Next:"
echo "  ./scripts/run_cluster.sh 3   # headless 3-node mesh (API 7701-7703)"
echo "  cd app && npm run tauri:dev  # desktop control room"
echo "  cd app && npm run tauri:build  # installer"
