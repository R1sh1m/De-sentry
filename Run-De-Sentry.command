#!/bin/bash

set -e

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_DIR="$PROJECT_DIR/app"

if ! command -v npm >/dev/null 2>&1; then
  echo "npm is not installed or is not available in PATH."
  read -r -p "Press Return to close..."
  exit 1
fi

cd "$APP_DIR"
echo "Starting De-Sentry..."
echo "Keep this window open while using the app."
echo
exec npm run tauri:dev
