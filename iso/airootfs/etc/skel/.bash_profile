# win11wm live session: on the first VT, start the graphical session.
# Loop so a crashed session restarts instead of letting systemd's start
# limiter kill the autologin getty for good.
if [ -z "${DISPLAY:-}" ] && [ "$(tty)" = "/dev/tty1" ]; then
    while true; do
        startx
        sleep 2
    done
fi
