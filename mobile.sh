#!/usr/bin/env bash
#
# mobile.sh - run win11wm in tablet mode inside a phone-sized window.
#
# The default path uses the private Xwayland server from start.sh, which gives
# the phone geometry exactly. On a Wayland host, --wayland uses win11wm-wl
# instead; the host compositor may override that nested window's size.
#
#   ./mobile.sh                 a 412x824 window (One UI phone aspect)
#   ./mobile.sh --size 390x844  a different size
#   ./mobile.sh --client kitty  also open a client inside the session
#   ./mobile.sh --wayland       force the Wayland compositor
#   ./mobile.sh --x11           force the Xwayland fallback
#   ./mobile.sh --build         rebuild win11wm-wl before starting
#   ./mobile.sh --lock          boot behind the Android-style lock screen
#   ./mobile.sh --login         boot on the Android-style sign-in screen
#   ./mobile.sh --no-auth       TEST ONLY: bypass PAM on lock/login
#
# Anything else before `--` is passed through to the backend runner; everything
# after `--` goes to win11wm. Ctrl-C tears the whole thing down.
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

# One UI phones report about 412x915 in logical pixels; the height is trimmed 10%
# so the window fits comfortably on a 1080p host alongside decorations and a panel.
SIZE="${WIN11WM_MOBILE_SIZE:-412x824}"
MODE="${WIN11WM_MOBILE_BACKEND:-auto}"
BUILD=auto
CLIENT=""
PASS_ARGS=()
WM_ARGS=()

usage() {
    awk 'NR > 1 { if (/^#/) { sub(/^# ?/, ""); print; next } exit }' "${BASH_SOURCE[0]}"
}

while (($#)); do
    case "$1" in
        -s | --size) SIZE="${2:-}"; shift 2 ;;
        -c | --client) CLIENT="${2:-}"; shift 2 ;;
        --wayland) MODE=wayland; shift ;;
        --x11) MODE=x11; shift ;;
        --lock | --login | --no-auth) WM_ARGS+=("$1"); shift ;;
        --build) BUILD=yes; shift ;;
        -h | --help) usage; exit 0 ;;
        --) shift; WM_ARGS+=("$@"); break ;;
        *) PASS_ARGS+=("$1"); shift ;;
    esac
done

[[ "$SIZE" =~ ^[0-9]+x[0-9]+$ ]] || {
    echo "mobile.sh: invalid size '$SIZE' (expected WxH)" >&2
    exit 2
}

if [[ "$MODE" == auto ]]; then
    if [[ -n "${DISPLAY:-}" ]]; then MODE=x11
    elif [[ -n "${WAYLAND_DISPLAY:-}" ]]; then MODE=wayland
    else
        echo "mobile.sh: no DISPLAY or WAYLAND_DISPLAY; cannot start a nested session" >&2
        exit 2
    fi
fi

if [[ "$MODE" == wayland ]]; then
    [[ -n "${WAYLAND_DISPLAY:-}" ]] || {
        echo "mobile.sh: no WAYLAND_DISPLAY; run inside a Wayland session or use --x11" >&2
        exit 2
    }
    if [[ "$BUILD" == yes ]]; then
        cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release
        cmake --build "$ROOT/build" --target win11wm-wl -j"$(nproc 2>/dev/null || echo 4)"
    fi
    args=(--tablet --size "$SIZE")
    [[ -n "$CLIENT" ]] && args+=(--exec "$CLIENT")
    args+=(${PASS_ARGS[@]+"${PASS_ARGS[@]}"} ${WM_ARGS[@]+"${WM_ARGS[@]}"})
    exec "$ROOT/run-wl.sh" "${args[@]}"
fi

# X11 fallback: start.sh owns the private Xwayland server, display picking and
# cleanup; we only choose the geometry and ask win11wm to boot in tablet mode.
start_args=(${PASS_ARGS[@]+"${PASS_ARGS[@]}"})
[[ -n "$CLIENT" ]] && start_args+=(--client "$CLIENT")
exec "$ROOT/start.sh" "${start_args[@]}" --size "$SIZE" \
    -- ${WM_ARGS[@]+"${WM_ARGS[@]}"} --tablet
