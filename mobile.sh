#!/usr/bin/env bash
#
# mobile.sh - run win11wm in tablet mode inside a phone-sized window.
#
# It nests a private X11 server in one host window (exactly as start.sh does) at
# a phone aspect ratio and boots win11wm straight into the tablet home screen, so
# the result looks and behaves like a mobile device rather than a desktop.
#
#   ./mobile.sh                 a 412x824 window (One UI phone aspect)
#   ./mobile.sh --size 390x844  a different size
#   ./mobile.sh --client xterm  also open a client inside the session
#
# Anything else before `--` is passed through to start.sh; everything after `--`
# goes to win11wm. Ctrl-C tears the whole thing down.
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

# One UI phones report about 412x915 in logical pixels; the height is trimmed 10%
# so the window fits comfortably on a 1080p host alongside decorations and a panel.
SIZE="${WIN11WM_MOBILE_SIZE:-412x824}"

START_ARGS=()
WM_ARGS=()
while (($#)); do
    case "$1" in
        -s | --size) SIZE="${2:-}"; shift 2 ;;
        -h | --help)
            sed -n '2,16p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'
            exit 0
            ;;
        --) shift; WM_ARGS+=("$@"); break ;;
        *) START_ARGS+=("$1"); shift ;;
    esac
done

# start.sh owns the Xwayland lifetime, display picking and cleanup; we only
# choose the geometry and ask win11wm to boot in tablet mode.
exec "$ROOT/start.sh" ${START_ARGS[@]+"${START_ARGS[@]}"} --size "$SIZE" \
    -- ${WM_ARGS[@]+"${WM_ARGS[@]}"} --tablet
