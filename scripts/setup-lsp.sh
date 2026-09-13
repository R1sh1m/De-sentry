#!/usr/bin/env bash
# Regenerate per-machine LSP artifacts for De-Sentry on Linux and macOS.
#
# Writes only gitignored files (see .gitignore "Local LSP overrides"):
#   1. <root>/compile_commands.json -- symlink to the newest
#      build*/compile_commands.json, so clangd auto-discovers it in every
#      editor (VSCode, Neovim, Zed, Emacs). Falls back to a copy if symlinks
#      are unavailable.
#   2. .vscode/lsp.local.json -- seeded from lsp.local.json.example on first
#      run, never overwritten afterwards (your machine paths stay yours).
#
# Idempotent: re-run after `cmake -S . -B build`, after switching build dirs
# (build/ vs build-macos), or after pulling new shared LSP configs.
#
# Run from the repository root:
#     ./scripts/setup-lsp.sh
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

# Prefer the documented default build dir; otherwise take the newest
# compile_commands.json under any build*/ directory (multi-config generators
# nest it one level deeper, e.g. build/RelWithDebInfo/).
CANDIDATE=""
if [[ -f "build/compile_commands.json" ]]; then
  CANDIDATE="build/compile_commands.json"
else
  # shellcheck disable=SC2012
  CANDIDATE="$(ls -t build*/compile_commands.json build*/*/compile_commands.json 2>/dev/null | head -n 1 || true)"
fi

if [[ -z "$CANDIDATE" ]]; then
  echo "[lsp] no compile_commands.json found under build*/."
  echo "[lsp] configure first: cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo"
  echo "[lsp] (CMAKE_EXPORT_COMPILE_COMMANDS is already ON in CMakeLists.txt)"
else
  if ln -sfn "$CANDIDATE" compile_commands.json 2>/dev/null; then
    echo "[lsp] linked compile_commands.json -> $CANDIDATE"
  else
    cp -f "$CANDIDATE" compile_commands.json
    echo "[lsp] copied compile_commands.json from $CANDIDATE (symlink unavailable;"
    echo "[lsp] re-run this script after reconfiguring so it does not go stale)"
  fi
fi

mkdir -p .vscode
if [[ -f ".vscode/lsp.local.json" ]]; then
  echo "[lsp] .vscode/lsp.local.json already exists -- leaving your overrides alone"
else
  cp .vscode/lsp.local.json.example .vscode/lsp.local.json
  echo "[lsp] seeded .vscode/lsp.local.json from example (gitignored)"
fi

echo "[lsp] toolchain probe (informational only):"
for tool in clangd rust-analyzer python3 node cargo cmake; do
  if command -v "$tool" >/dev/null 2>&1; then
    echo "  [ok] $tool $("$tool" --version 2>/dev/null | head -n 1 || true)"
  else
    echo "  [--] $tool not on PATH"
  fi
done
echo "[lsp] done. Shared configs (.clangd, pyrightconfig.json,"
echo "[lsp] .vscode/settings.json) are committed; local files stay gitignored."
