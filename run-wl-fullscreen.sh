#!/usr/bin/env bash
#
# Run win11wm-wl nested and ask the host compositor to fullscreen its output.
# Everything after the script name is passed through to ./run-wl.sh.
#
#   ./run-wl-fullscreen.sh
#   ./run-wl-fullscreen.sh --exec kitty
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$ROOT/run-wl.sh" --fullscreen "$@"
