#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKDIR=/tmp/archiso-build
OUTDIR="$ROOT/out"
PROFILE="$ROOT/iso"

echo "=== Cleaning work dir ==="
sudo rm -rf "$WORKDIR" "$OUTDIR"
mkdir -p "$OUTDIR"

echo "=== Building ISO ==="
sudo mkarchiso -v -w "$WORKDIR" -o "$OUTDIR" "$PROFILE"

echo "=== Done ==="
ls -lh "$OUTDIR"/*.iso 2>/dev/null || true
