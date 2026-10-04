set -u
N=$1; OUT=$2; shift 2
DISPLAY=:1 Xwayland :$N -noreset -geometry 1280x800 >/home/mango/Desktop/Supernova/.shots/xw$N.log 2>&1 &
XPID=$!
for i in $(seq 1 100); do DISPLAY=:$N xdpyinfo >/dev/null 2>&1 && break; sleep 0.1; done
env -u WAYLAND_DISPLAY DISPLAY=:$N WIN11WM_ASSETS=/home/mango/Desktop/Supernova/assets "$@" >/home/mango/Desktop/Supernova/.shots/wm$N.log 2>&1 &
WM=$!
sleep 4
DISPLAY=:$N import -window root /home/mango/Desktop/Supernova/.shots/$OUT 2>/home/mango/Desktop/Supernova/.shots/imp$N.log && echo "captured $OUT" || echo "capture failed $N"
kill $WM 2>/dev/null; sleep 0.4; kill $XPID 2>/dev/null; wait 2>/dev/null
