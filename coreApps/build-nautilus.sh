#!/bin/sh
# Reproducible build environment for GNOME Nautilus on this Manjaro box.
#
# System has glib 2.88.3, so main/52.alpha (needs glib >= 2.89.0) cannot build.
# We build Nautilus 50.3.1 (needs glib >= 2.84.0) from the git worktree
# "../nautilus-50.3.1".
#
# Workarounds applied (no root available):
#   1. meson + ninja + blueprint-compiler installed into ./.build-venv (pip)
#   2. gdbus-codegen / glib-mkenums / glib-genmarshal / gresource extracted from
#      the "glib2-devel" package into ~/.local/gdbusroot, wrapped in ~/.local/bin
#   3. pkg-config overrides in ~/.local/pc-override point gio-2.0.pc/glib-2.0.pc
#      at those wrappers
#   4. disabled optional features whose libs/tooling are absent:
#      -Dselinux=disabled (no libselinux) and -Dintrospection=false (no g-ir-scanner)

set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
SRC="$ROOT/nautilus"

export PATH="$ROOT/.build-venv/bin:$PATH"
export PKG_CONFIG_PATH="$HOME/.local/pc-override"

echo "==> configuring"
if [ -d "$SRC/_build" ]; then
  meson setup --reconfigure "$SRC/_build" "$SRC" -Dselinux=disabled -Dintrospection=false
else
  meson setup "$SRC/_build" "$SRC" -Dselinux=disabled -Dintrospection=false
fi

echo "==> compiling"
ninja -C "$SRC/_build"

echo "==> testing"
meson test -C "$SRC/_build" --print-errorlogs

echo "==> done: $SRC/_build/src/nautilus"
