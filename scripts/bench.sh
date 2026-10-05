#!/usr/bin/env bash
#
# Benchmark win11wm on a private Xwayland display so it never disturbs the
# session the user is actually looking at.
#
#   ./scripts/bench.sh [frames] [extra win11wm args...]
#
# Runs the WM headless with --stats, captures the HUD numbers, and prints a
# one-line summary. Needs the binary built (./run.sh builds into build/).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/build/win11wm"
FRAMES="${1:-400}"
shift || true

DISP="${WIN11WM_BENCH_DISPLAY:-:77}"
GEOM="${WIN11WM_BENCH_GEOM:-1920x1080}"

[[ -x "$BIN" ]] || { echo "bench: $BIN not built (run ./run.sh)" >&2; exit 1; }

started_server=0
if ! xdpyinfo -display "$DISP" >/dev/null 2>&1; then
    # Xwayland is the only nested server on this box; -rootless needs no
    # geometry flag and inherits the host's resolution.
    Xwayland "$DISP" -rootless -nolisten tcp >/tmp/win11wm-bench-x.log 2>&1 &
    started_server=1
    for _ in $(seq 1 40); do
        xdpyinfo -display "$DISP" >/dev/null 2>&1 && break
        sleep 0.25
    done
fi
xdpyinfo -display "$DISP" >/dev/null 2>&1 || {
    echo "bench: could not start Xwayland on $DISP" >&2
    [ "$started_server" = 1 ] && kill %1 2>/dev/null
    exit 1
}

log=$(mktemp)
# DISPLAY is pinned and D-Bus is replaced with a *private* session bus. Both
# matter: single-instance D-Bus apps (nautilus, firefox, ...) would otherwise
# forward the request to the instance already running on the user's session and
# pop a window up over their real desktop instead of landing in this compositor.
# Stripping DBUS_SESSION_BUS_ADDRESS outright is not enough -- those apps then
# fall back to the well-known socket and find the user's instance anyway.
dbus_args=()
if command -v dbus-run-session >/dev/null 2>&1; then
    dbus_args=(dbus-run-session --)
fi

timeout 300 env -u XDG_RUNTIME_DIR DISPLAY="$DISP" \
    "${dbus_args[@]}" "$BIN" --perf --frames "$FRAMES" "$@" >"$log" 2>&1
rc=$?

canvas=$(grep -oE 'canvas [0-9]+x[0-9]+' "$log" | tail -1)
renderer=$(grep -oE '\| (Intel|AMD|NVIDIA|Apple|Mesa)[^|]*\|' "$log" | head -1 | tr -d '| ')
[ "$started_server" = 1 ] && kill %1 2>/dev/null

echo "=== ${canvas:-?} | ${renderer:-?}"
# Report the median window, so a one-off startup hitch does not skew the summary.
grep -oE 'PERF .*' "$log" | awk '
    { for (i = 1; i <= NF; i++) { split($i, kv, "="); v[kv[1]] = kv[2] }
      n++; fps[n] = v["fps"] + 0; cpu[n] = v["cpu"] + 0; gpu[n] = v["gpu"] + 0; dr[n] = v["drawsAvg"] + 0 }
    END {
      if (n == 0) { print "no PERF samples captured"; exit }
      asort(fps); asort(cpu); asort(gpu); asort(dr); m = int((n + 1) / 2);
      printf "windows sampled : %d\n", n;
      printf "median fps      : %.0f\n", fps[m];
      printf "median cpu ms   : %.2f\n", cpu[m];
      printf "median gpu ms   : %.2f\n", gpu[m];
      printf "median draws/fr : %.1f\n", dr[m];
    }'
echo "=== rc=$rc"
grep -vE '^\[win11wm\] (PERF|wallpaper|font|canvas|desktop|taskbar|assets|Intel|texture_)' "$log" | tail -8
rm -f "$log"