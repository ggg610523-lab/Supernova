#!/usr/bin/env bash
#
# Build (if needed) and run win11wm-wl, the native Wayland compositor build of
# the Windows 11 shell. The X11 window manager is still ./run.sh.
#
#   ./run-wl.sh                 nest inside the Wayland session you are in
#   ./run-wl.sh --headless      render offscreen (no session needed)
#   ./run-wl.sh --drm           take over a TTY with DRM + libinput
#   ./run-wl.sh --socket seat0  advertise a fixed socket name
#   ./run-wl.sh --help          every option win11wm-wl takes
#   ./run-wl.sh -- <args>       everything after -- is passed through
#
# Clients connect to the socket printed at start-up (it is also exported as
# WAYLAND_DISPLAY for --exec children), e.g. from another terminal:
#
#   WAYLAND_DISPLAY=wayland-1 kitty
#
# wlroots is looked up through pkg-config. When it was built locally it lives in
# .deps/prefix, which this script puts on PKG_CONFIG_PATH and LD_LIBRARY_PATH so
# nothing has to be installed system wide.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="${WIN11WM_BUILD:-$ROOT/build}"
BIN="$BUILD/win11wm-wl"
DEPS="$ROOT/.deps/prefix"
JOBS="$(nproc 2>/dev/null || echo 4)"

say() { printf '\033[36m[run-wl]\033[0m %s\n' "$*" >&2; }
die() { printf '\033[31m[run-wl]\033[0m %s\n' "$*" >&2; exit 1; }

if [[ -d "$DEPS/lib" ]]; then
    export LD_LIBRARY_PATH="$DEPS/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export PKG_CONFIG_PATH="$DEPS/lib/pkgconfig:$DEPS/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
fi

if [[ "${1:-}" == -h || "${1:-}" == --help ]]; then
    sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'
    exit 0
fi
[[ "${1:-}" == -- ]] && shift

if [[ ! -x "$BIN" ]]; then
    command -v cmake >/dev/null || die "cmake is required to build win11wm-wl"
    pkg-config --exists wlroots-0.18 || die \
        "wlroots-0.18 not found; build it into a prefix and point PKG_CONFIG_PATH at it
     (see the Wayland notes in README.md), or install wlroots 0.18"
    say "building $BIN"
    cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$BUILD" --target win11wm-wl -j"$JOBS"
fi

# The shell loads its icons, fonts and wallpaper relative to the working
# directory, and the nested backend needs the session it was started from.
cd "$ROOT"
exec "$BIN" "$@"
