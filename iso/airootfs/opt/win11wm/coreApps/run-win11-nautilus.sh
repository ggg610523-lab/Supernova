#!/bin/sh
# Run the modified (Windows 11-themed) Nautilus build on YOUR real desktop session.
#
# Usage:
#   ./run-win11-nautilus.sh            # opens the Home dashboard
#   ./run-win11-nautilus.sh /etc       # opens a specific folder
#
# Notes:
#  - The build uses the same app id (org.gnome.Nautilus) as the system Files,
#    so any running Nautilus is quit first to avoid handing off to the OLD UI.
#  - GSETTINGS_SCHEMA_DIR is required: the build's own schemas live in _build/data.

set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
SRC="$ROOT/nautilus"
BIN="$SRC/_build/src/nautilus"

if [ ! -x "$BIN" ]; then
  echo "error: $BIN not found. Build it first with ./build-nautilus.sh" >&2
  exit 1
fi

# Quit the system Files so our build isn't handed off to it.
nautilus -q 2>/dev/null || true
sleep 1

export GSETTINGS_SCHEMA_DIR="$SRC/_build/data"

exec "$BIN" "${1:-home:///}"
