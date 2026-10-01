#include "sysctl.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "apps.h"
#include "util.h"

namespace wm {
namespace {

// The probe. Everything is guarded so a missing tool, a kernel without that
// /sys node or a D-Bus with no players just means "absent", never an error and
// never a hang. Output is one key=value per line.
const char* const kProbeScript = R"SH(
has() { command -v "$1" >/dev/null 2>&1; }

for t in nmcli wpctl pactl amixer rfkill bluetoothctl playerctl gammastep \
         wlsunset loginctl import makoctl dunstctl; do
    has "$t" && printf 't=%s\n' "$t"
done

# --- radios -------------------------------------------------------------
if has rfkill; then
    for dev in wifi bluetooth; do
        info=$(rfkill list "$dev" 2>/dev/null) || info=
        [ -n "$info" ] || continue
        if [ "$dev" = wifi ]; then p=w; else p=b; fi
        printf '%sa=1\n' "$p"
        soft=$(printf '%s\n' "$info" | sed -n 's/^[[:space:]]*Soft blocked: //p')
        hard=$(printf '%s\n' "$info" | sed -n 's/^[[:space:]]*Hard blocked: //p')
        if [ "$hard" = yes ] || [ "$soft" = yes ]; then
            printf '%s=0\n' "$p"
        else
            printf '%s=1\n' "$p"
        fi
    done
fi

# --- wired link ---------------------------------------------------------
for i in /sys/class/net/*; do
    n=${i##*/}
    [ "$n" = lo ] && continue
    [ -d "$i/wireless" ] && continue
    [ -e "$i/carrier" ] || continue
    printf 'e=1\n'
    [ "$(cat "$i/carrier" 2>/dev/null)" = 1 ] && printf 'ec=1\n'
    break
done

# --- audio --------------------------------------------------------------
if has wpctl; then
    wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null | awk '
        { for (i = 1; i <= NF; i++) if ($i ~ /^[0-9]+(\.[0-9]+)?$/) { printf "av=%d\n", $i * 100; break }
          if (index($0, "MUTED")) print "am=1" }'
    printf 'ao=1\n'
elif has pactl; then
    pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null \
        | sed -n 's/.*[ \t]\([0-9]\+\)%.*/av=\1/p'
    pactl get-sink-mute @DEFAULT_SINK@ 2>/dev/null | sed -n 's/^Mute: /am=/p'
    printf 'ao=1\n'
elif has amixer; then
    amixer -D pulse sget Master 2>/dev/null | sed -n 's/.*\[\([0-9]\+\)%\].*/av=\1/p'
    printf 'ao=1\n'
fi

# --- backlight ----------------------------------------------------------
# Three backends, in descending order of fidelity: a writable sysfs node,
# `brightnessctl` (udev rules usually let the active user drive it), and
# `xrandr --brightness` as a software fallback when there is no panel node at
# all (desktops, external monitors). `bl=` is only emitted when we can read a
# real level; the xrandr path has no getter, so the WM tracks it itself.
bl_seen=0
for d in /sys/class/backlight/*; do
    [ -r "$d/brightness" ] || continue
    cur=$(cat "$d/brightness" 2>/dev/null) || continue
    max=$(cat "$d/max_brightness" 2>/dev/null) || continue
    [ "$max" -gt 0 ] 2>/dev/null || continue
    printf 'bl=%d\n' "$((cur * 100 / max))"
    bl_seen=1
    [ -w "$d/brightness" ] && printf 'blw=1\n'
    break
done
if has brightnessctl; then
    printf 'bt=1\n'
    if [ "$bl_seen" = 0 ]; then
        c=$(brightnessctl get 2>/dev/null); m=$(brightnessctl max 2>/dev/null)
        if [ -n "$c" ] && [ -n "$m" ] && [ "$m" -gt 0 ] 2>/dev/null; then
            printf 'bl=%d\n' "$((c * 100 / m))"
            bl_seen=1
        fi
    fi
fi
if [ "$bl_seen" = 0 ] && has xrandr; then
    xrout=$(xrandr --query 2>/dev/null | awk '/ connected/{print $1; exit}')
    if [ -n "$xrout" ]; then
        printf 'xr=1\n'
        printf 'xo=%s\n' "$xrout"
    fi
fi

# --- Do Not Disturb: the notification daemon's pause mode ---------------
if has dunstctl; then
    printf 'na=1\n'
    dunstctl is-paused 2>/dev/null | grep -qi true && printf 'np=1\n'
elif has makoctl; then
    printf 'na=1\n'
    makoctl mode 2>/dev/null | grep -qi 'do-not-disturb' && printf 'np=1\n'
fi

# --- media: MPRIS over D-Bus, so no playerctl is required ----------------
if has gdbus; then
    bus=${DBUS_SESSION_BUS_ADDRESS:-unix:path=/run/user/$(id -u)/bus}
    player=$(DBUS_SESSION_BUS_ADDRESS="$bus" gdbus call --session \
        --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
        --method org.freedesktop.DBus.ListNames 2>/dev/null \
        | tr ',' '\n' \
        | sed -n "s/.*'\(org\.mpris\.MediaPlayer2\.[^']*\)'.*/\1/p" | head -1)
    if [ -n "$player" ]; then
        printf 'mp=1\n'
        DBUS_SESSION_BUS_ADDRESS="$bus" gdbus call --session --dest "$player" \
            --object-path /org/mpris/MediaPlayer2/Player \
            --method org.freedesktop.DBus.Properties.Get \
            org.mpris.MediaPlayer2.Player PlaybackStatus 2>/dev/null \
            | grep -q Playing && printf 'mplay=1\n'
        meta=$(DBUS_SESSION_BUS_ADDRESS="$bus" gdbus call --session --dest "$player" \
            --object-path /org/mpris/MediaPlayer2/Player \
            --method org.freedesktop.DBus.Properties.Get \
            org.mpris.MediaPlayer2.Player Metadata 2>/dev/null)
        printf '%s\n' "$meta" | sed -n "s/.*'xesam:title': <'\([^']*\)'>.*/mt=\1/p" | head -1
        printf '%s\n' "$meta" | sed -n "s/.*'xesam:artist': <\['\([^']*\)'\].*/ma=\1/p" | head -1
    fi
fi

exit 0
)SH";

// access() only ever finds a tool that happens to sit in the working directory,
// so every action silently did nothing. Walk PATH the way execvp would.
bool toolInstalled(const char* name) {
    if (!name || !*name || strchr(name, '/')) return ::access(name, X_OK) == 0;
    const char* path = ::getenv("PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";
    const std::string needle(name);
    const char* p = path;
    while (*p) {
        const char* sep = strchr(p, ':');
        const size_t len = sep ? size_t(sep - p) : strlen(p);
        const std::string dir(p, len);
        // An empty PATH element means the working directory, per POSIX.
        const std::string full =
            (dir.empty() ? std::string(".") : dir) + "/" + needle;
        if (::access(full.c_str(), X_OK) == 0) return true;
        if (!sep) break;
        p = sep + 1;
    }
    return false;
}

// Runs a probe script in a child and returns the read end of its stdout.
int spawnScript(const char* script) {
    int fds[2];
    if (::pipe(fds) != 0) return -1;
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        ::close(fds[0]);
        ::dup2(fds[1], STDOUT_FILENO);
        ::close(fds[1]);
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }
        // New session: a helper that wedges can never take the WM with it.
        ::setsid();
        ::execl("/bin/sh", "sh", "-c", script, static_cast<char*>(nullptr));
        ::_exit(127);
    }
    ::close(fds[1]);
    // SIGCHLD is SIG_IGN in main(), so the child is reaped by the kernel; we
    // only need the pipe to tell us when its output is complete.
    const int flags = ::fcntl(fds[0], F_GETFL, 0);
    ::fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
    return fds[0];
}

std::string value(const std::string& line, const char* key) {
    const size_t klen = std::strlen(key);
    if (line.size() <= klen || line.compare(0, klen, key) != 0) return std::string();
    if (line[klen] != '=') return std::string();
    return line.substr(klen + 1);
}

}  // namespace

void SystemControls::init() {
    requestRefresh();
}

void SystemControls::run(const std::string& cmd) {
    if (cmd.empty()) return;
    launchApp(cmd);
}

void SystemControls::scheduleRefresh() {
    // Give the machine a moment to actually apply the write before we read the
    // state back, otherwise the slider would snap to its old value for a frame.
    reProbeAt_ = nowMs() + 250.0;
}

void SystemControls::requestRefresh(double minInterval) {
    if (fd_ >= 0) return;
    if (reProbeAt_ > 0.0) return;  // a write-triggered refresh is already queued
    const double now = nowMs();
    if (now - lastProbeAt_ < minInterval * 1000.0) return;
    spawnProbe();
}

void SystemControls::spawnProbe() {
    const int fd = spawnScript(kProbeScript);
    if (fd < 0) return;
    fd_ = fd;
    buf_.clear();
    lastProbeAt_ = nowMs();
}

bool SystemControls::poll() {
    if (fd_ < 0) {
        if (reProbeAt_ > 0.0 && nowMs() >= reProbeAt_) {
            reProbeAt_ = 0.0;
            spawnProbe();
        }
        return false;
    }
    bool done = false;
    for (;;) {
        char chunk[512];
        const ssize_t n = ::read(fd_, chunk, sizeof chunk);
        if (n > 0) {
            buf_.append(chunk, size_t(n));
            continue;
        }
        if (n == 0) {
            done = true;  // writer closed: the probe finished
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            done = true;
        }
        break;
    }
    if (!done) return false;
    ::close(fd_);
    fd_ = -1;
    const std::string blob = buf_;
    buf_.clear();
    parse(blob);
    return true;
}

void SystemControls::parse(const std::string& blob) {
    // Presence and tool flags describe the machine as of *this* probe, so they
    // start clear: carrying the previous answer forward would leave a
    // disconnected adapter or an unplugged player stuck on screen forever.
    SystemState next;
    next.probed = true;
    // Night Light is the one thing no helper can read back, so it is our own
    // memory of what we last asked for and has to survive a re-probe.
    next.nightLight = s_.nightLight;
    std::string title, artist;

    size_t pos = 0;
    while (pos < blob.size()) {
        size_t end = blob.find('\n', pos);
        if (end == std::string::npos) end = blob.size();
        const std::string line = blob.substr(pos, end - pos);
        pos = end + 1;
        if (line.empty()) continue;

        if (!value(line, "t").empty()) {
            const std::string t = value(line, "t");
            if (t == "gammastep" || t == "wlsunset") next.nightPresent = true;
            if (t == "import" || t == "scrot" || t == "maim") next.shotPresent = true;
            if (t == "loginctl") next.lockPresent = true;
            continue;
        }
        if (!value(line, "wa").empty()) { next.wifiPresent = true; continue; }
        if (!value(line, "w").empty()) { next.wifi = value(line, "w") == "1"; continue; }
        if (!value(line, "ba").empty()) { next.btPresent = true; continue; }
        if (!value(line, "b").empty()) { next.bt = value(line, "b") == "1"; continue; }
        if (!value(line, "e").empty()) { next.wiredPresent = true; continue; }
        if (!value(line, "ec").empty()) { next.wired = value(line, "ec") == "1"; continue; }
        if (!value(line, "ao").empty()) { next.audioPresent = true; continue; }
        if (!value(line, "av").empty()) {
            next.volume = clampi(std::atoi(value(line, "av").c_str()), 0, 100);
            continue;
        }
        if (!value(line, "am").empty()) { next.muted = value(line, "am") == "1"; continue; }
        if (!value(line, "bl").empty()) {
            next.backlightPresent = true;
            next.brightness = clampi(std::atoi(value(line, "bl").c_str()), 0, 100);
            continue;
        }
        if (!value(line, "blw").empty()) { next.backlightWritable = true; continue; }
        if (!value(line, "bt").empty()) { next.brightnessctlPresent = true; continue; }
        if (!value(line, "xr").empty()) {
            // No panel node we can read: dim in software and remember the level
            // ourselves, since xrandr has no brightness getter.
            next.backlightPresent = true;
            next.xrandrPresent = true;
            next.brightness = s_.brightness;
            continue;
        }
        if (!value(line, "xo").empty()) { next.xrandrOutput = value(line, "xo"); continue; }
        if (!value(line, "na").empty()) { next.dndPresent = true; continue; }
        if (!value(line, "np").empty()) { next.dnd = value(line, "np") == "1"; continue; }
        if (!value(line, "mp").empty()) { next.mediaPresent = true; continue; }
        if (!value(line, "mplay").empty()) { next.playing = value(line, "mplay") == "1"; continue; }
        if (!value(line, "mt").empty()) { title = value(line, "mt"); continue; }
        if (!value(line, "ma").empty()) { artist = value(line, "ma"); continue; }
    }
    next.mediaTitle = title;
    next.mediaArtist = artist;
    // iOS parity: airplane mode is "no radios, no wired link".
    next.airplane = !next.wifi && !next.bt && !next.wired;

    // `probed` is deliberately not part of the comparison: the first probe must
    // be committed even when every value happens to match the defaults.
    if (s_.probed && next == s_) return;  // nothing moved: leave the frame alone
    s_ = next;
}

void SystemControls::setVolume(int percent) {
    if (!s_.audioPresent) return;
    s_.volume = clampi(percent, 0, 100);
    // A dragged slider un-mutes, exactly like iOS and every desktop mixer.
    s_.muted = false;
    char cmd[128];
    std::snprintf(cmd, sizeof cmd, "wpctl set-volume @DEFAULT_AUDIO_SINK@ %d%%", s_.volume);
    if (toolInstalled("wpctl")) {
        run(cmd);
    } else if (toolInstalled("pactl")) {
        std::snprintf(cmd, sizeof cmd, "pactl set-sink-volume @DEFAULT_SINK@ %d%%", s_.volume);
        run(cmd);
        run("pactl set-sink-mute @DEFAULT_SINK@ 0");
    } else if (toolInstalled("amixer")) {
        std::snprintf(cmd, sizeof cmd, "amixer -D pulse sset Master %d%%", s_.volume);
        run(cmd);
    }
    scheduleRefresh();
}

void SystemControls::setMuted(bool on) {
    if (!s_.audioPresent) return;
    s_.muted = on;
    if (toolInstalled("wpctl")) {
        run(std::string("wpctl set-mute @DEFAULT_AUDIO_SINK@ ") + (on ? "1" : "0"));
    } else if (toolInstalled("pactl")) {
        run(std::string("pactl set-sink-mute @DEFAULT_SINK@ ") + (on ? "1" : "0"));
    } else if (toolInstalled("amixer")) {
        run(std::string("amixer -D pulse sset Master ") + (on ? "mute" : "unmute"));
    }
    scheduleRefresh();
}

void SystemControls::setBrightness(int percent) {
    if (!s_.backlightPresent) return;
    // Never let the slider black the panel out completely; iOS stops just above
    // zero too, and a fully dark screen is impossible to undo by touch.
    s_.brightness = clampi(percent, 1, 100);
    char cmd[256];
    if (s_.backlightWritable) {
        // The sysfs node is 0..max_brightness, not 0..100, so the shell rescales.
        std::snprintf(cmd, sizeof cmd,
                      "f=$(echo /sys/class/backlight/*/brightness); "
                      "m=$(echo /sys/class/backlight/*/max_brightness); "
                      "printf '%%s' $(( %d * $(cat $m) / 100 )) > $f",
                      s_.brightness);
        run(cmd);
    } else if (s_.brightnessctlPresent) {
        std::snprintf(cmd, sizeof cmd, "brightnessctl set %d%%", s_.brightness);
        run(cmd);
    } else if (s_.xrandrPresent && !s_.xrandrOutput.empty()) {
        // Software dimming: map 1..100 onto 0.25..1.0 gamma so the bottom of the
        // slider is visibly dim without ever going fully black.
        const double f = 0.25 + 0.75 * double(s_.brightness) / 100.0;
        std::snprintf(cmd, sizeof cmd, "xrandr --output %s --brightness %.2f",
                      s_.xrandrOutput.c_str(), f);
        run(cmd);
    } else {
        return;
    }
    scheduleRefresh();
}

void SystemControls::setWifi(bool on) {
    if (!s_.wifiPresent) return;
    s_.wifi = on;
    if (toolInstalled("nmcli")) {
        run(std::string("nmcli radio wifi ") + (on ? "on" : "off"));
    } else if (toolInstalled("rfkill")) {
        run(std::string(on ? "rfkill unblock wifi" : "rfkill block wifi"));
    }
    scheduleRefresh();
}

void SystemControls::setBluetooth(bool on) {
    if (!s_.btPresent) return;
    s_.bt = on;
    if (toolInstalled("bluetoothctl")) {
        run(std::string("bluetoothctl power ") + (on ? "on" : "off"));
    } else if (toolInstalled("rfkill")) {
        run(std::string(on ? "rfkill unblock bluetooth" : "rfkill block bluetooth"));
    }
    scheduleRefresh();
}

void SystemControls::setAirplane(bool on) {
    if (!s_.wifiPresent && !s_.btPresent) return;
    if (on) {
        s_.wifi = false;
        s_.bt = false;
        if (toolInstalled("nmcli")) {
            run("nmcli radio all off");
        } else {
            if (s_.wifiPresent) run("rfkill block wifi");
            if (s_.btPresent) run("rfkill block bluetooth");
        }
    } else {
        s_.wifi = s_.wifiPresent;
        s_.bt = s_.btPresent;
        if (toolInstalled("nmcli")) {
            run("nmcli radio all on");
        } else {
            if (s_.wifiPresent) run("rfkill unblock wifi");
            if (s_.btPresent) run("rfkill unblock bluetooth");
        }
    }
    scheduleRefresh();
}

void SystemControls::setDnd(bool on) {
    if (!s_.dndPresent) return;
    s_.dnd = on;
    if (toolInstalled("dunstctl")) {
        run(std::string("dunstctl set-paused ") + (on ? "true" : "false"));
    } else if (toolInstalled("makoctl")) {
        run(on ? "makoctl mode -a do-not-disturb" : "makoctl mode -r do-not-disturb");
    }
    scheduleRefresh();
}

void SystemControls::setNightLight(bool on) {
    if (!s_.nightPresent) return;
    s_.nightLight = on;
    if (toolInstalled("gammastep")) {
        run(on ? "gammastep -O 1.0" : "gammastep -O 0.0");
    } else if (toolInstalled("wlsunset")) {
        run(on ? "wlsunset -T 4500 -t 3500" : "wlsunset -x");
    }
}

// Transport is spoken over MPRIS so it works wherever a player is, without
// needing playerctl installed. Playerctl stays the fallback for setups whose
// players do not implement MPRIS.
void SystemControls::mediaAction(const char* mprisMethod, const char* playerctlArg) {
    if (!s_.mediaPresent) return;
    if (toolInstalled("gdbus")) {
        run(std::string("gdbus call --session --dest org.freedesktop.DBus ") +
            "--object-path /org/freedesktop/DBus "
            "--method org.freedesktop.DBus.ListNames | tr ',' '\\n' "
            "| sed -n \"s/.*'\\(org\\.mpris\\.MediaPlayer2\\.[^']*\\)'.*/\\1/p\" | head -1 | "
            "while read p; do gdbus call --session --dest \"$p\" "
            "--object-path /org/mpris/MediaPlayer2/Player "
            "--method org.mpris.MediaPlayer2.Player." + mprisMethod + " >/dev/null 2>&1; done");
    } else if (toolInstalled("playerctl")) {
        run(std::string("playerctl ") + playerctlArg);
    } else {
        return;
    }
    // Optimistic toggle so the play/pause glyph flips immediately; the re-probe
    // corrects it if the player refused.
    s_.playing = !s_.playing;
    scheduleRefresh();
}

void SystemControls::mediaPrev() { mediaAction("Previous", "previous"); }

void SystemControls::mediaNext() { mediaAction("Next", "next"); }

void SystemControls::mediaPlayPause() { mediaAction("PlayPause", "play-pause"); }

void SystemControls::lockSession() {
    if (!s_.lockPresent) return;
    run("loginctl lock-session");
}

void SystemControls::screenshot() {
    if (!s_.shotPresent) return;
    if (toolInstalled("import")) {
        run("d=$(xdg-user-dir PICTURES 2>/dev/null || echo $HOME); "
            "mkdir -p \"$d/Screenshots\"; "
            "import -window root \"$d/Screenshots/shot-$(date +%H%M%S).png\"");
    } else if (toolInstalled("scrot")) {
        run("d=$(xdg-user-dir PICTURES 2>/dev/null || echo $HOME); "
            "mkdir -p \"$d/Screenshots\"; "
            "scrot \"$d/Screenshots/shot-$(date +%H%M%S).png\"");
    }
}

}  // namespace wm
