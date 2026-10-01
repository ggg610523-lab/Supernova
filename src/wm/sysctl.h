// Quick-settings backends for the Control Centre.
//
// Two rules shape this file:
//
//   1. The render loop must never wait on a helper process. Reading state
//      therefore runs as a forked child that writes "key=value" lines into a
//      pipe; the WM drains that pipe from its idle path (SystemControls::poll)
//      and parses the result. Writes are fire-and-forget, the same detached
//      fork launchApp() already uses.
//
//   2. Nothing is faked. Every control knows whether the tool that backs it is
//      actually installed and whether the current user may write to it; the
//      Control Centre greys out what it cannot do instead of showing a switch
//      that lies.
#pragma once

#include <string>

namespace wm {

struct SystemState {
    // Availability: the device / tool is present at all.
    bool wifiPresent = false;
    bool btPresent = false;
    bool wiredPresent = false;
    bool audioPresent = false;
    bool backlightPresent = false;
    bool backlightWritable = false;   // /sys/class/backlight/.../brightness is writable
    bool brightnessctlPresent = false;  // `brightnessctl` can drive the panel
    bool xrandrPresent = false;       // software dimming fallback (no /sys backlight)
    std::string xrandrOutput;         // connected output to dim
    bool mediaPresent = false;
    bool nightPresent = false;
    bool shotPresent = false;
    bool lockPresent = false;
    bool dndPresent = false;    // a notification daemon we can pause

    // Live values.
    bool wifi = false;          // radio enabled (unblocked)
    bool bt = false;
    bool wired = false;         // carrier is up
    bool airplane = false;      // iOS parity: no radios and no wired link
    bool muted = false;
    int volume = 0;             // 0..100
    int brightness = 100;       // 0..100 (starts full so the xrandr path is sane)
    bool playing = false;
    bool nightLight = false;    // tracked by us: the helper has no getter
    bool dnd = false;           // notification daemon paused (Do Not Disturb)
    std::string mediaTitle;
    std::string mediaArtist;

    // True once a probe has come back at least once.
    bool probed = false;

    // A brightness slider is usable when *any* backend can drive the panel:
    // a writable sysfs node, `brightnessctl`, or `xrandr --brightness`.
    bool brightnessUsable() const {
        return backlightPresent &&
               (backlightWritable || brightnessctlPresent || xrandrPresent);
    }

    // Lets poll() skip a state commit (and a repaint) when a probe changed
    // nothing. `probed` is excluded on purpose: the first probe must always be
    // committed, otherwise the panel would never learn that probing works.
    bool operator==(const SystemState& o) const {
        return wifiPresent == o.wifiPresent && btPresent == o.btPresent &&
               wiredPresent == o.wiredPresent && audioPresent == o.audioPresent &&
               backlightPresent == o.backlightPresent &&
               backlightWritable == o.backlightWritable &&
               brightnessctlPresent == o.brightnessctlPresent &&
               xrandrPresent == o.xrandrPresent && xrandrOutput == o.xrandrOutput &&
               mediaPresent == o.mediaPresent &&
               nightPresent == o.nightPresent && shotPresent == o.shotPresent &&
               lockPresent == o.lockPresent && dndPresent == o.dndPresent &&
               wifi == o.wifi && bt == o.bt &&
               wired == o.wired && airplane == o.airplane && muted == o.muted &&
               volume == o.volume && brightness == o.brightness && playing == o.playing &&
               nightLight == o.nightLight && dnd == o.dnd && mediaTitle == o.mediaTitle &&
               mediaArtist == o.mediaArtist;
    }
    bool operator!=(const SystemState& o) const { return !(*this == o); }
};

class SystemControls {
public:
    void init();

    // Kicks a probe off, unless one is already running or the last one landed
    // less than `minInterval` ago.
    void requestRefresh(double minInterval = 0.0);
    // Drains the probe pipe. Returns true when the state changed (so the
    // caller can mark the frame dirty).
    bool poll();
    bool probeRunning() const { return fd_ >= 0; }

    const SystemState& state() const { return s_; }

    // Actions. Each one updates the cached value immediately (so the control
    // animates without waiting for the next probe) and schedules a re-probe.
    void setVolume(int percent);
    void setBrightness(int percent);
    void setWifi(bool on);
    void setBluetooth(bool on);
    void setAirplane(bool on);
    void setNightLight(bool on);
    void setMuted(bool on);
    void setDnd(bool on);
    void mediaPrev();
    void mediaNext();
    void mediaPlayPause();
    void lockSession();
    void screenshot();

private:
    void spawnProbe();
    void parse(const std::string& blob);
    void run(const std::string& cmd);
    // One MPRIS transport call, with playerctl as the fallback.
    void mediaAction(const char* mprisMethod, const char* playerctlArg);
    // Re-probe once the machine has had a moment to apply a write.
    void scheduleRefresh();

    SystemState s_;
    int fd_ = -1;              // read end of the in-flight probe, -1 when idle
    std::string buf_;          // partial output of that probe
    double reProbeAt_ = 0.0;   // monotonic ms; 0 = no re-probe pending
    double lastProbeAt_ = -1e9;
};

}  // namespace wm
