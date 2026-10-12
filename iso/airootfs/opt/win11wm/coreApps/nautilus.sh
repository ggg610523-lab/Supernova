#!/usr/bin/env bash
# nautilus.sh - open the Windows 11-themed Nautilus build inside win11wm.
#
# Starts win11wm in a private nested X server (see start.sh) and launches the
# modified Nautilus as its client. Nautilus is forced onto X11: a Wayland
# session in the parent environment would otherwise win and it would open on
# the host desktop instead of inside the compositor.
#
#   ./nautilus.sh                    1280x800 window, opens Home
#   ./nautilus.sh /etc               opens a specific folder
#   ./nautilus.sh --fullscreen       fill the whole host display
#   ./nautilus.sh -s 1600x900        request a specific size
#
# The build is located through these variables (both overridable):
#   WIN11WM_NAUTILUS_BIN         path to the nautilus binary
#   WIN11WM_NAUTILUS_SCHEMA_DIR  dir with that build's gschemas.compiled
#
# Any other arguments (e.g. -d/-o/--build) are forwarded to start.sh.
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
START_SH="$ROOT/../start.sh"
[[ -x "$START_SH" ]] || { echo "nautilus.sh: start.sh not found at $START_SH" >&2; exit 1; }

DEFAULT_SRC="$ROOT/nautilus"
NAUTILUS_BIN="${WIN11WM_NAUTILUS_BIN:-$DEFAULT_SRC/_build/src/nautilus}"
SCHEMA_DIR="${WIN11WM_NAUTILUS_SCHEMA_DIR:-$DEFAULT_SRC/_build/data}"

[[ -x "$NAUTILUS_BIN" ]] || {
    echo "nautilus.sh: nautilus binary not found at $NAUTILUS_BIN" >&2
    echo "  set WIN11WM_NAUTILUS_BIN to your build" >&2
    exit 1
}
[[ -f "$SCHEMA_DIR/gschemas.compiled" ]] || {
    echo "nautilus.sh: no gschemas.compiled in $SCHEMA_DIR" >&2
    echo "  set WIN11WM_NAUTILUS_SCHEMA_DIR to your build's data dir" >&2
    exit 1
}

SIZE="${WIN11WM_SIZE:-1280x800}"
FULLSCREEN=0
WM_ARGS=()
TARGET=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        -f | --fullscreen) FULLSCREEN=1; shift ;;
        -s | --size)       SIZE="${2:-}"; shift 2 ;;
        -h | --help)
            sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'
            exit 0
            ;;
        --) shift; TARGET="${1:-}"; break ;;
        -*) WM_ARGS+=("$1"); shift ;;
        *)  TARGET="$1"; shift ;;
    esac
done

# -fullscreen sizes itself to the host, so only pass --size in windowed mode.
if ((FULLSCREEN)); then
    WM_ARGS+=(--fullscreen)
else
    WM_ARGS+=(--size "$SIZE")
fi

TARGET="${TARGET:-home:///}"

# Nautilus is a single-instance D-Bus app sharing the host session bus; quit any
# instance already registered so this build is not handed off to the old UI.
GSETTINGS_SCHEMA_DIR="$SCHEMA_DIR" "$NAUTILUS_BIN" -q 2>/dev/null || true

# Build a shell-safe client command line for start.sh's --client hook. GDK_BACKEND
# forces X11 onto the private server; the schema dir is needed for this build.
client="env GDK_BACKEND=x11 GSETTINGS_SCHEMA_DIR=$(printf '%q' "$SCHEMA_DIR") $(printf '%q' "$NAUTILUS_BIN") $(printf '%q' "$TARGET")"

exec "$START_SH" "${WM_ARGS[@]}" --client "$client"
