#!/usr/bin/env bash
#
# Build (if needed) and run win11wm.
#
#   ./run.sh                 configure, build, run
#   ./run.sh --force-assets  re-download Reversal icons and MuternVF
#   ./run.sh --no-assets     never touch assets/
#   ./run.sh --debug         unoptimised build
#   ./run.sh --clean         wipe the build directory first
#   ./run.sh -- <args>       everything after -- goes to win11wm
#
# Assets are fetched once into ./assets (Reversal icons + MuternVF); win11wm
# finds them next to its own binary and degrades to letter tiles and a system
# font when they are missing.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="${WIN11WM_BUILD:-$ROOT/build}"
CONFIG=Release
FETCH=auto
CLEAN=0

say() { printf '\033[36m[run]\033[0m %s\n' "$*" >&2; }
die() { printf '\033[31m[run]\033[0m %s\n' "$*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --force-assets) FETCH=force; shift ;;
    --no-assets)    FETCH=no; shift ;;
    --debug)        CONFIG=Debug; shift ;;
    --clean)        CLEAN=1; shift ;;
    -h|--help)      sed -n '2,14p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'; exit 0 ;;
    --)             shift; break ;;
    *)              break ;;
  esac
done

command -v cmake >/dev/null || die "cmake is required"
command -v pkg-config >/dev/null || die "pkg-config is required"

# ------------------------------------------------------------------ build deps
if ! pkg-config --exists x11 xcomposite xdamage xfixes xrender epoxy freetype2 fontconfig zlib; then
  missing=()
  for m in x11 xcomposite xdamage xfixes xrender epoxy freetype2 fontconfig zlib; do
    pkg-config --exists "$m" || missing+=("$m")
  done
  die "missing build dependencies: ${missing[*]}"
fi

# ---------------------------------------------------------------------- assets
STAMP="$ROOT/assets/.stamp"
if [[ "$FETCH" != no ]]; then
  if [[ "$FETCH" == force || ! -f "$STAMP" ]]; then
    say "fetching bundled assets (Reversal icons, MuternVF)"
    [[ "$FETCH" == force ]] && "$ROOT/scripts/fetch-assets.sh" --force \
                             || "$ROOT/scripts/fetch-assets.sh"
  else
    say "assets present ($(find "$ROOT/assets/icons" -maxdepth 1 -name '*.png' 2>/dev/null | wc -l | tr -d ' ') icons)"
  fi
fi

# ------------------------------------------------------------------ build
[[ $CLEAN -eq 1 ]] && rm -rf "$BUILD"
say "configuring ($CONFIG)"
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE="$CONFIG" >/dev/null
say "building"
cmake --build "$BUILD" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)" >/dev/null

BIN="$BUILD/win11wm"
[[ -x "$BIN" ]] || die "build did not produce $BIN"

# ------------------------------------------------------------------- run
# The binary lives in the build directory, so it would look for assets in
# build/assets. Point it at the ones next to this script instead.
export WIN11WM_ASSETS="$ROOT/assets"
if [[ ! -d "$WIN11WM_ASSETS" ]]; then
  if [[ "$FETCH" == no ]]; then
    say "warning: no asset directory at $WIN11WM_ASSETS (--no-assets)"
  else
    die "no asset directory at $WIN11WM_ASSETS (run scripts/fetch-assets.sh)"
  fi
fi

if [[ -z "${DISPLAY:-}" ]]; then
  die "DISPLAY is not set; this WM needs an X server (it is not a Wayland compositor)"
fi
say "starting on DISPLAY=$DISPLAY (assets: $WIN11WM_ASSETS)"
exec "$BIN" "$@"