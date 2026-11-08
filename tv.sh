#!/usr/bin/env bash
#
# Run tvshell, the 10-foot TV shell.
#
#   ./tv.sh                     nest inside the current Wayland session
#   ./tv.sh --fullscreen        fullscreen the nested window
#   ./tv.sh --pi                Raspberry Pi preset: DRM + fullscreen + minimal
#   ./tv.sh --headless          render offscreen (tests/CI)
#   ./tv.sh --drm               take over a TTY with DRM + libinput
#   ./tv.sh --minimal           reduced motion and fill rate
#   ./tv.sh --frames 60 --dump /tmp/tv.ppm
#   ./tv.sh -e nautilus         start an app with the shell
#
# Everything after the script name is handed to the shell. The binary is built
# from tv/ on first use; set TV_REBUILD=1 to force a rebuild.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="${TV_BUILD:-$ROOT/tv/build}"
BIN="$BUILD/tvshell"
DEPS="$ROOT/.deps/prefix"

say() { printf '\033[36m[tv]\033[0m %s\n' "$*" >&2; }
die() { printf '\033[31m[tv]\033[0m %s\n' "$*" >&2; exit 1; }

# --pi is a preset for the HDMI box: no session, so DRM owns the output, the
# shell runs fullscreen and the launcher drops its animations.
ARGS=()
for arg in "$@"; do
    case "$arg" in
        --pi) ARGS+=(--drm --fullscreen --minimal) ;;
        *)    ARGS+=("$arg") ;;
    esac
done
set -- ${ARGS[@]+"${ARGS[@]}"}

# The wlroots 0.18 build lives in .deps/prefix; use it when it is there.
if [[ -d "$DEPS/lib" ]]; then
    export LD_LIBRARY_PATH="$DEPS/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export PKG_CONFIG_PATH="$DEPS/lib/pkgconfig:$DEPS/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
fi

if [[ ! -x "$BIN" || -n "${TV_REBUILD:-}" ]]; then
    command -v cmake >/dev/null || die "cmake is required to build tvshell"
    pkg-config --exists wlroots-0.18 || die \
        "wlroots-0.18 not found; build it into .deps/prefix or install wlroots 0.18"
    say "building $BIN"
    cmake -S "$ROOT/tv" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$BUILD" -j"$(nproc 2>/dev/null || echo 4)"
fi

export TV_ASSET_DIR="${TV_ASSET_DIR:-$ROOT/assets}"

if [[ -z "${XDG_RUNTIME_DIR:-}" ]]; then
    export XDG_RUNTIME_DIR="/tmp/tvshell-runtime-$(id -u)"
    mkdir -p "$XDG_RUNTIME_DIR"
    chmod 700 "$XDG_RUNTIME_DIR"
fi

cd "$ROOT"
exec "$BIN" "$@"
