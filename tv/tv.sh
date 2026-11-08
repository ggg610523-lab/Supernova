#!/usr/bin/env bash
#
# Build and run tvshell, the 10-foot TV compositor for a Raspberry Pi 3.
#
#   ./tv/tv.sh                     nest inside the current Wayland session
#   ./tv/tv.sh --headless          render offscreen (for tests/CI)
#   ./tv/tv.sh --drm               take over a TTY with DRM + libinput
#   ./tv/tv.sh --frames 120 --dump /tmp/tv.ppm
#   ./tv/tv.sh --launch nautilus   start an app with the shell
#
# Controller input is read from /dev/input/event*. If no controller is found,
# make sure your user can read those nodes (usually the "input" group).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TV_ROOT="$ROOT/tv"
BUILD="${TV_BUILD:-$TV_ROOT/build}"
BIN="$BUILD/tvshell"
DEPS="$ROOT/.deps/prefix"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

say() { printf '\033[36m[tv]\033[0m %s\n' "$*" >&2; }
die() { printf '\033[31m[tv]\033[0m %s\n' "$*" >&2; exit 1; }

if [[ -d "$DEPS/lib" ]]; then
    export LD_LIBRARY_PATH="$DEPS/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export PKG_CONFIG_PATH="$DEPS/lib/pkgconfig:$DEPS/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
fi

if [[ "${1:-}" == -h || "${1:-}" == --help ]]; then
    awk 'NR > 1 { if (/^#/) { sub(/^# ?/, ""); print; next } exit }' "${BASH_SOURCE[0]}"
    exit 0
fi

if [[ ! -x "$BIN" ]]; then
    command -v cmake >/dev/null || die "cmake is required to build tvshell"
    pkg-config --exists wlroots-0.18 || die \
        "wlroots-0.18 not found; build it into .deps/prefix or install wlroots 0.18"
    say "building $BIN"
    cmake -S "$TV_ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$BUILD" -j"$JOBS"
fi

export TV_ASSET_DIR="${TV_ASSET_DIR:-$ROOT/assets}"

if [[ -z "${XDG_RUNTIME_DIR:-}" ]]; then
    export XDG_RUNTIME_DIR="/tmp/tvshell-runtime-$(id -u)"
    mkdir -p "$XDG_RUNTIME_DIR"
    chmod 700 "$XDG_RUNTIME_DIR"
fi

if [[ -z "${WAYLAND_DISPLAY:-}" && "${TV_DRM:-0}" != 1 ]]; then
    if [[ ! -r /dev/dri/card0 ]]; then
        say "no WAYLAND_DISPLAY and no readable /dev/dri/card0; use --headless for a test"
    fi
fi

if ! compgen -G "/dev/input/event*" >/dev/null 2>&1; then
    say "no /dev/input/event* nodes found; controller input will be unavailable"
elif [[ ! -r "$(compgen -G '/dev/input/event*' | head -n1)" ]]; then
    say "input devices are not readable; add your user to the input group for controllers"
fi

cd "$ROOT"
exec "$BIN" "$@"
