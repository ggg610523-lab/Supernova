#!/usr/bin/env bash
# start.sh - run win11wm inside a brand new X11 server.
#
# The new X server is a nested rootful Xwayland: it opens one window on the host
# display and serves a complete X11 screen inside it. win11wm then runs as the
# window manager of that private server, so the host session (KDE/Wayland here)
# is never touched and nothing is left behind when this script exits.
#
#   ./start.sh                      1280x800 window on the host display
#   ./start.sh --fullscreen         fill the whole host display
#   ./start.sh --size 1600x900 --client xterm
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
WM_BIN="$ROOT/build/win11wm"
ASSETS="$ROOT/assets"

HOST_DISPLAY="${DISPLAY:-}"
DISPLAY_NUM=""
SIZE=""
FULLSCREEN=0
OUTPUT_NAME=""
BUILD=auto
CLIENT=""
WM_ARGS=()
WAIT_FOR_WM=1

usage() {
    cat <<EOF
Usage: ${0##*/} [options] [-- extra win11wm args]

Starts a private X11 server and runs win11wm as its window manager.

Options:
  -d, --display N      X display number for the new server (default: auto from :9)
  -s, --size WxH       size of the new X screen (default: 1280x800)
  -f, --fullscreen     fill the host display completely (uses the host's full size)
  -o, --output NAME    output to use for fullscreen
      --host DISPLAY   host display to nest inside (default: \$DISPLAY)
      --client CMD     run CMD inside the new session once the WM is up
      --build          rebuild win11wm before starting
      --no-build       never rebuild
  -w, --no-wait        start the WM without waiting for it to take the screen
  -h, --help           this help

Everything is killed on Ctrl-C or when the script exits.
EOF
}

die() { printf '%s\n' "${0##*/}: $*" >&2; exit 1; }

while (($#)); do
    case "$1" in
        -d | --display)   DISPLAY_NUM="${2:-}"; shift 2 ;;
        -s | --size)      SIZE="${2:-}"; shift 2 ;;
        -f | --fullscreen) FULLSCREEN=1; shift ;;
        -o | --output)    OUTPUT_NAME="${2:-}"; shift 2 ;;
        --host)           HOST_DISPLAY="${2:-}"; shift 2 ;;
        --client)         CLIENT="${2:-}"; shift 2 ;;
        --build)          BUILD=yes; shift ;;
        --no-build)       BUILD=no; shift ;;
        -w | --no-wait)   WAIT_FOR_WM=0; shift ;;
        -h | --help)      usage; exit 0 ;;
        --)               shift; WM_ARGS+=("$@"); break ;;
        -*)               die "unknown option '$1' (try --help)" ;;
        *)                die "unexpected argument '$1' (try --help)" ;;
    esac
done

command -v Xwayland >/dev/null || die "Xwayland is not installed"
command -v xdpyinfo >/dev/null || die "xdpyinfo is not installed"

# ---------------------------------------------------------------- host display
[[ -n "$HOST_DISPLAY" ]] || die "no host display: pass --host :0 or export DISPLAY"
DISPLAY="$HOST_DISPLAY" xdpyinfo >/dev/null 2>&1 || die "cannot reach host display $HOST_DISPLAY"

# NOTE: awk must drain the whole stream. Closing the pipe early (e.g. with
# `exit`) would SIGPIPE xdpyinfo and, under `set -o pipefail -e`, abort the
# script the moment the result is assigned to a variable.
host_size() { DISPLAY="$HOST_DISPLAY" xdpyinfo 2>/dev/null |
    awk '/dimensions:/ && !found { width = $2; found = 1 }
         END { if (found) print width }'; }

# ------------------------------------------------------------------ arguments
xwayland_args=()
if ((FULLSCREEN)); then
    SIZE="${SIZE:-$(host_size)}"
    xwayland_args+=(-fullscreen)
    if [[ -n "$OUTPUT_NAME" ]]; then xwayland_args+=(-output "$OUTPUT_NAME"); fi
fi
SIZE="${SIZE:-1280x800}"
[[ "$SIZE" =~ ^[0-9]+x[0-9]+$ ]] || die "invalid --size '$SIZE' (expected WxH)"
xwayland_args+=(-geometry "$SIZE")

# ------------------------------------------------------------- free display #
socket() { [[ -e "/tmp/.X11-unix/X$1" ]]; }
in_use() {
    DISPLAY=":$1" xdpyinfo >/dev/null 2>&1
}
pick_display() {
    local n
    for n in $(seq 9 99); do
        socket "$n" && continue
        in_use "$n" && continue
        printf '%s' "$n"
        return 0
    done
    return 1
}

DISPLAY_NUM="${DISPLAY_NUM:-$(pick_display)}" ||
    die "no free display number between :9 and :99"
[[ "$DISPLAY_NUM" =~ ^[0-9]+$ ]] || die "invalid --display '$DISPLAY_NUM'"
socket "$DISPLAY_NUM" && die "display :$DISPLAY_NUM is already in use"
in_use "$DISPLAY_NUM" && die "display :$DISPLAY_NUM is already serving"

# ---------------------------------------------------------------------- build
if [[ "$BUILD" == yes ]]; then
    [[ -d "$ROOT/build" ]] || die "no build directory; run: cmake -S . -B build"
    printf 'building win11wm ... '
    cmake --build "$ROOT/build" -j"$(nproc 2>/dev/null || echo 4)" >/dev/null ||
        die "build failed"
    echo done
elif [[ "$BUILD" == auto && ! -x "$WM_BIN" ]]; then
    [[ -d "$ROOT/build" ]] || die "win11wm is not built; run: cmake -S . -B build && cmake --build build"
    printf 'building win11wm ... '
    cmake --build "$ROOT/build" -j"$(nproc 2>/dev/null || echo 4)" >/dev/null ||
        die "build failed"
    echo done
fi
[[ -x "$WM_BIN" ]] || die "win11wm not found at $WM_BIN (try --build)"
[[ -d "$ASSETS" ]] || die "assets missing at $ASSETS; run: scripts/fetch-assets.sh"

# ----------------------------------------------------------------- lifecycle
XWAYLAND_PID=""
WM_PID=""
XLOG="$(mktemp -t win11wm-xwayland.XXXXXX)"

cleanup() {
    trap - EXIT INT TERM
    [[ -n "$WM_PID" ]] && kill "$WM_PID" 2>/dev/null || true
    [[ -n "$XWAYLAND_PID" ]] && kill "$XWAYLAND_PID" 2>/dev/null || true
    wait 2>/dev/null || true
    rm -f "$XLOG"
}
trap cleanup EXIT INT TERM

start_server() {
    # Xwayland is chatty on stderr (xkbcomp warnings); keep it in a log so a
    # real failure can still be shown.
    DISPLAY="$HOST_DISPLAY" Xwayland ":$DISPLAY_NUM" -noreset "${xwayland_args[@]}" \
        2>"$XLOG" &
    XWAYLAND_PID=$!
}

xlog() { sed 's/^/  | /' "$XLOG"; }

wait_for_server() {
    local i
    for i in $(seq 1 100); do
        if ! kill -0 "$XWAYLAND_PID" 2>/dev/null; then
            return 1
        fi
        DISPLAY=":$DISPLAY_NUM" xdpyinfo >/dev/null 2>&1 && return 0
        sleep 0.1
    done
    return 1
}

start_server
if ! wait_for_server; then
    if ((FULLSCREEN)); then
        printf '%s\n' "${0##*/}: fullscreen not available, falling back to a window" >&2
        xlog >&2
        # Retry without -fullscreen/-output but keep the requested geometry.
        kill "$XWAYLAND_PID" 2>/dev/null || true
        wait "$XWAYLAND_PID" 2>/dev/null || true
        xwayland_args=(-noreset -geometry "$SIZE")
        XWAYLAND_PID=""
        start_server
        if ! wait_for_server; then
            printf '%s\n' "${0##*/}: the X server failed to start:" >&2
            xlog >&2
            die "cannot start X server on :$DISPLAY_NUM"
        fi
    else
        printf '%s\n' "${0##*/}: the X server failed to start:" >&2
        xlog >&2
        die "cannot start X server on :$DISPLAY_NUM"
    fi
fi

nestsize() { DISPLAY=":$DISPLAY_NUM" xdpyinfo 2>/dev/null |
    awk '/dimensions:/ && !found { width = $2; found = 1 }
         END { if (found) print width }'; }

# ----------------------------------------------------------------- start wm #
# Clients must talk X11 to this private server. A Wayland session in the parent
# environment would otherwise win: toolkits (kitty, GTK, Qt) probe
# $WAYLAND_DISPLAY first and the app would open on the host desktop instead of
# in here. Xwayland itself keeps the variable -- it needs it to reach the host
# compositor -- so only the WM (and everything it spawns) is scrubbed.
env -u WAYLAND_DISPLAY -u XDG_SESSION_TYPE \
    DISPLAY=":$DISPLAY_NUM" WIN11WM_ASSETS="$ASSETS" "$WM_BIN" "${WM_ARGS[@]}" &
WM_PID=$!

if ((WAIT_FOR_WM)); then
    # Give win11wm time to claim the screen and fail loudly if it cannot.
    for i in $(seq 1 30); do
        kill -0 "$WM_PID" 2>/dev/null || break
        sleep 0.1
    done
    kill -0 "$WM_PID" 2>/dev/null ||
        die "win11wm exited immediately; run it by hand on :$DISPLAY_NUM to see why"
fi

printf 'win11wm running on :%s (%s) inside %s\n' \
    "$DISPLAY_NUM" "$(nestsize)" "$HOST_DISPLAY"
if ((FULLSCREEN)); then
    printf 'mode: fullscreen (Ctrl-C to stop)\n'
else
    printf 'mode: window %s (Ctrl-C to stop)\n' "$SIZE"
fi

if [[ -n "$CLIENT" ]]; then
    sleep 1
    # Same scrub as the WM: --client apps are X11 clients of this server too.
    env -u WAYLAND_DISPLAY -u XDG_SESSION_TYPE DISPLAY=":$DISPLAY_NUM" \
        setsid bash -c "$CLIENT" >/dev/null 2>&1 &
fi

# Keep the script alive while the X server runs; both die together on Ctrl-C.
wait "$XWAYLAND_PID"