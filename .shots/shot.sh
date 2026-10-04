set -u
N=21
SIZE=1280x800
DISPLAY=:1 Xwayland :$N -noreset -geometry $SIZE >/home/mango/Desktop/Supernova/.shots/xw.log 2>&1 &
XPID=$!
ok=0
for i in $(seq 1 100); do DISPLAY=:$N xdpyinfo >/dev/null 2>&1 && { ok=1; break; }; sleep 0.1; done
if [ $ok -ne 1 ]; then echo "server failed"; sed 's/^/X | /' /home/mango/Desktop/Supernova/.shots/xw.log; kill $XPID 2>/dev/null; exit 1; fi
env -u WAYLAND_DISPLAY DISPLAY=:$N WIN11WM_ASSETS=/home/mango/Desktop/Supernova/assets /home/mango/Desktop/Supernova/build/win11wm --no-vsync >/home/mango/Desktop/Supernova/.shots/wm.log 2>&1 &
WM=$!
sleep 3
if kill -0 $WM 2>/dev/null; then echo "WM alive"; else echo "WM DIED"; fi
DISPLAY=:$N import -window root /home/mango/Desktop/Supernova/.shots/desktop.png 2>/home/mango/Desktop/Supernova/.shots/imp.log && echo "captured" || { echo "capture failed"; cat /home/mango/Desktop/Supernova/.shots/imp.log; }
kill $WM 2>/dev/null; sleep 0.4; kill $XPID 2>/dev/null; wait 2>/dev/null
echo '--- wm.log ---'; sed 's/^/WM | /' /home/mango/Desktop/Supernova/.shots/wm.log | head -50
