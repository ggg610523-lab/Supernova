// iOS-style lock and sign-in screen, shared by the X11 and Wayland shells.
//
// The same surface serves three jobs:
//   * a session lock screen (Win+L, Control Centre, logind's LockedHint);
//   * an iOS-style sign-in screen (`--login`) with a user picker;
//   * the tablet/mobile variant, which uses the same touch-first layout as a
//     phone (large clock, lock controls, swipe-up passcode layer and keypad).
//
// Authentication is PAM. The screen never sees a password hash, and it does not
// try to manage sessions itself: unlocking a logind-locked session calls
// `loginctl unlock-session`, which is the same contract greeters use.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "util.h"

namespace wm {

class Compositor;
class Text;
class IconStore;

class LockScreen {
public:
    enum class Mode { Lock, Login };

    LockScreen();
    ~LockScreen();
    LockScreen(const LockScreen&) = delete;
    LockScreen& operator=(const LockScreen&) = delete;

    // `assetDir` is the shell's asset root. The current user is picked up from
    // the passwd database; a login screen also enumerates local users.
    // `noAuth` is test-only and bypasses PAM.
    void init(const std::string& assetDir, bool noAuth = false);
    void shutdown();

    // Show the lock screen, or the sign-in screen with its user picker.
    void start(Mode mode);
    void stop();
    bool active() const;

    // Call once per shell animation tick. Returns true when the screen changed
    // and the compositor should repaint. While idle it also watches logind's
    // LockedHint, so `loginctl lock-session` from the Control Centre works.
    bool tick(double nowMs);

    // Returns true when the key changed the screen. `sym` is an XKB/X11 keysym;
    // `typed` is the UTF-8 produced by the key, if any.
    bool handleKey(uint32_t sym, const std::string& typed, bool pressed);
    bool handleText(const std::string& utf8);
    void pointerMove(int x, int y);
    bool pointerButton(int x, int y, unsigned button, bool pressed);
    // A controller's d-pad/left stick. Returns true when it changed the screen.
    bool controllerNav(int dx, int dy, bool activate, bool back);

    void draw(Compositor& comp, Text& text, IconStore& icons, int screenW, int screenH,
              bool tablet);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Ask logind to lock this session. The shell's logind watcher then brings the
// lock surface up; calling this directly also shows it immediately.
void requestSessionLock();

}  // namespace wm
