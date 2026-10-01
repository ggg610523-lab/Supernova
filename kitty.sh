#!/usr/bin/env bash
# kitty.sh - open kitty inside win11wm.
#
# Starts win11wm in a private nested X server (see start.sh) and launches kitty
# as its client. kitty is forced onto X11: a Wayland session in the parent
# environment would otherwise win and kitty would open on the host desktop
# instead of inside the compositor.
#
#   ./kitty.sh                   1280x800 window on the host display
#   ./kitty.sh --fullscreen      fill the whole host display, kitty maximized
#   ./kitty.sh -s 1600x900       request a specific size
#   ./kitty.sh -- --title hi     everything after -- goes to kitty
#
# --fullscreen also asks win11wm to maximize kitty (`kitty --start-as=maximized`)
# so the terminal fills the work area instead of sitting in a default-sized
# window; the taskbar stays visible. Pass your own --start-as=... after -- to
# override that.
#
# Any other arguments (e.g. -d/-o/--build) are forwarded to start.sh.
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
[[ -x "$ROOT/start.sh" ]] || { echo "kitty.sh: start.sh not found next to this script" >&2; exit 1; }
command -v kitty >/dev/null || { echo "kitty.sh: kitty is not installed" >&2; exit 1; }

SIZE="${WIN11WM_SIZE:-1280x800}"
FULLSCREEN=0
WM_ARGS=()
KITTY_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        -f | --fullscreen) FULLSCREEN=1; shift ;;
        -s | --size)       SIZE="${2:-}"; shift 2 ;;
        -h | --help)
            sed -n '2,14p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'
            exit 0
            ;;
        --) shift; KITTY_ARGS=("$@"); break ;;
        *)  WM_ARGS+=("$1"); shift ;;
    esac
done

# -fullscreen sizes itself to the host, so only pass --size in windowed mode.
if ((FULLSCREEN)); then
    WM_ARGS+=(--fullscreen)
    # Maximize the terminal inside win11wm. A user-supplied --start-as wins.
    start_as=""
    for arg in "${KITTY_ARGS[@]}"; do
        [[ "$arg" == --start-as* ]] && start_as=1
    done
    [[ -n "$start_as" ]] || KITTY_ARGS+=(--start-as=maximized)
else
    WM_ARGS+=(--size "$SIZE")
fi

# Build a shell-safe kitty command line for start.sh's --client hook.
client="kitty"
for arg in "${KITTY_ARGS[@]}"; do
    client+=" $(printf '%q' "$arg")"
done

exec "$ROOT/start.sh" "${WM_ARGS[@]}" --client "$client"
