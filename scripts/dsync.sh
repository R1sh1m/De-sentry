#!/usr/bin/env bash
# dsync.sh - Mac/Linux wrapper for dsync.py
# Usage: ./scripts/dsync.sh <command> [args] [--force]
# Requires: Python 3.9+ on PATH

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 "$SCRIPT_DIR/dsync.py" "$@"
