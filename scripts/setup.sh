#!/usr/bin/env bash
# One-command developer bootstrap for De-Sentry on Linux and macOS.
#
# Checks prerequisites, builds the C++ engine, installs the app's
# dependencies, fetches the sizing model (once) and stages the desentryd
# sidecar -- everything after cloning that README.md used to list as five
# separate manual steps. Every step is idempotent: re-running this after a
# pull only rebuilds what changed.
#
# Node.js and Python 3 are auto-downloaded into .local/ when missing from
# PATH. A self-contained Python virtual environment is created in .venv/.
#
# Run from the repository root:
#     ./scripts/setup.sh
#
# Afterwards either run a headless mesh:
#     ./scripts/run_cluster.sh 3
# or the desktop control room:
#     cd app && npm run tauri:dev
set -euo pipefail
# Load Cargo environment if rustup was installed previously
if [ -f "$HOME/.cargo/env" ]; then
  . "$HOME/.cargo/env"
fi

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

have() { command -v "$1" >/dev/null 2>&1; }
ver() { "$1" --version 2>/dev/null | head -n 1 || true; }

# ── Local tool directory (gitignored) ──────────────────────────────────────
LOCAL_DIR="$PROJECT_ROOT/.local"
NODE_VERSION="22.11.0"

# ── Auto-download Node.js if not on PATH ───────────────────────────────────
ensure_node() {
  # If Node already downloaded, just add to PATH
  if [ -d "$LOCAL_DIR/node" ]; then
    export PATH="$LOCAL_DIR/node/bin:$PATH"
    echo "  [ok] node $(node -v) (already present)"
    return
  fi
  if have node; then return; fi
  echo "[setup] Node.js not found on PATH -- downloading v${NODE_VERSION} to $LOCAL_DIR/node"
  mkdir -p "$LOCAL_DIR"
  local arch platform url ext
  arch="$(uname -m)"
  [[ "$arch" == "arm64" || "$arch" == "aarch64" ]] && arch="arm64" || arch="x64"
  platform="$(uname)"
  case "$platform" in
    Darwin) url="https://nodejs.org/dist/v${NODE_VERSION}/node-v${NODE_VERSION}-darwin-${arch}.tar.gz"; ext=".gz" ;;
    Linux)  url="https://nodejs.org/dist/v${NODE_VERSION}/node-v${NODE_VERSION}-linux-${arch}.tar.xz"; ext=".xz" ;;
    *)      echo "[setup] Unsupported platform for auto-downloading Node.js: $platform" >&2; return 1 ;;
  esac
  echo "  Downloading from $url"
  curl -sSfL -o "$LOCAL_DIR/node.tar${ext}" "$url"
  if [[ "$platform" == "Linux" ]]; then
    tar -xJf "$LOCAL_DIR/node.tar${ext}" -C "$LOCAL_DIR"
  else
    tar -xzf "$LOCAL_DIR/node.tar${ext}" -C "$LOCAL_DIR"
  fi
  rm -f "$LOCAL_DIR/node.tar${ext}"
  local node_dir
  node_dir=$(find "$LOCAL_DIR" -maxdepth 1 -type d -name "node-v${NODE_VERSION}-*" | head -1)
  if [[ -z "$node_dir" ]]; then
    echo "[setup] Failed to extract Node.js archive" >&2; return 1
  fi
  mv "$node_dir" "$LOCAL_DIR/node"
  export PATH="$LOCAL_DIR/node/bin:$PATH"
  echo "  [ok] node $(node -v)"
}

# ── Auto-download Python 3 if not on PATH ──────────────────────────────────
ensure_python() {
  if have python3; then return; fi
  echo "[setup] Python 3 not found on PATH -- downloading Miniconda to $LOCAL_DIR/miniconda"
  mkdir -p "$LOCAL_DIR"
  local arch platform url
  arch="$(uname -m)"
  platform="$(uname)"
  if [[ "$arch" == "arm64" || "$arch" == "aarch64" ]]; then
    [[ "$platform" == "Linux" ]] && arch="aarch64" || arch="arm64"
  else
    arch="x86_64"
  fi
  case "$platform" in
    Darwin) url="https://repo.anaconda.com/miniconda/Miniconda3-latest-MacOSX-${arch}.sh" ;;
    Linux)  url="https://repo.anaconda.com/miniconda/Miniconda3-latest-Linux-${arch}.sh" ;;
    *)      echo "[setup] Unsupported platform for auto-downloading Python: $platform" >&2; return 1 ;;
  esac
  echo "  Downloading from $url"
  curl -sSfL -o "$LOCAL_DIR/miniconda.sh" "$url"
  bash "$LOCAL_DIR/miniconda.sh" -b -p "$LOCAL_DIR/miniconda"
  rm -f "$LOCAL_DIR/miniconda.sh"
  export PATH="$LOCAL_DIR/miniconda/bin:$PATH"
  echo "  [ok] python3 $(python3 --version)"
}

missing=()
need() {
  if have "$1"; then
    echo "  [ok] $1 $(ver "$1")"
  else
    echo "  [!!] $1 missing -- $2"
    missing+=("$1")
  fi
}

echo "1/6 prerequisites"
if [[ "$(uname)" == "Darwin" ]]; then
  need cmake "brew install cmake"
  need ninja "brew install ninja"
  need cargo "brew install rustup-init && rustup-init -y"
  need node "auto-downloads into .local/node"
  need python3 "auto-downloads Miniconda into .local/miniconda"
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
  need node "auto-downloads into .local/node"
  need python3 "auto-downloads Miniconda into .local/miniconda"
  if ! pkg-config --exists openssl 2>/dev/null && ! [[ -f /usr/include/openssl/evp.h ]]; then
    echo "  [!!] OpenSSL headers not found -- sudo apt install libssl-dev"
    missing+=("openssl-headers")
  else
    echo "  [ok] openssl-headers"
  fi
fi

# ── Attempt auto-download for node and python3 ─────────────────────────────
if [[ " ${missing[*]+"${missing[*]}"} " =~ " node " ]]; then
  ensure_node || echo "  [!!] node auto-download failed -- install manually and re-run" >&2
fi
if [[ " ${missing[*]+"${missing[*]}"} " =~ " python3 " ]]; then
  ensure_python || echo "  [!!] python3 auto-download failed -- install manually and re-run" >&2
fi

# Re-check: remove any tool that is now available from the missing list
new_missing=()
for tool in "${missing[@]}"; do
  if have "$tool"; then
    echo "  [ok] $tool $(ver "$tool")"
  else
    new_missing+=("$tool")
  fi
done
missing=("${new_missing[@]+"${new_missing[@]}"}")

if [[ "${#missing[@]}" -gt 0 ]]; then
  echo "Missing tools: ${missing[*]}. Install them, then re-run ./scripts/setup.sh" >&2
  exit 1
fi

echo "2/6 Python virtual environment"
VENV_DIR="$PROJECT_ROOT/.venv"
if [[ ! -d "$VENV_DIR" ]]; then
  python3 -m venv "$VENV_DIR"
fi
# shellcheck disable=SC1091
source "$VENV_DIR/bin/activate"
echo "  venv: $(which python3) ($(python3 --version))"

for req_file in "$PROJECT_ROOT/clients/python/requirements.txt" \
                "$PROJECT_ROOT/app/scripts/requirements.txt"; do
  if [[ -f "$req_file" ]]; then
    pip install -q -r "$req_file"
  fi
done
deactivate 2>/dev/null || true

echo "3/6 engine (CMake configure + build)"
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

echo "4/6 app dependencies (npm install)"
(
  cd "$PROJECT_ROOT/app"
  npm install --no-audit --no-fund

  echo "5/6 sizing model (one-time download, skipped when present)"
  node scripts/fetch-model.mjs

  echo "6/6 stage desentryd as the Tauri sidecar"
  node scripts/stage-sidecar.mjs
)

echo ""
echo "Setup complete. Next:"
echo "  ./scripts/run_cluster.sh 3   # headless 3-node mesh (API 7701-7703)"
echo "  cd app && npm run tauri:dev  # desktop control room"
echo "  cd app && npm run tauri:build  # installer"
