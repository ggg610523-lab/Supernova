// Gamepad and TV-remote input for the TV shell.
//
// wlroots/libinput gives us keyboards and pointers, but not joysticks. A TV
// shell needs the d-pad, A/B, Guide and the shoulder buttons, so this reads the
// raw evdev event devices directly. Xbox pads, DualShock 4 and DualSense all
// speak the same Linux gamepad codes, so one mapping serves them; the device is
// named in the log ("DualSense", "DualShock 4") to make a pairing visible.
//
// A Fire TV remote is not a gamepad at all: it is a Bluetooth HID keyboard, so
// wlroots already delivers it through the seat and Manager::onKey maps its
// arrow/Return/Back/Home/Menu keys. Only remotes that report as a joystick
// (hat + guide button) come through here.
//
// The manager exposes only semantic events; no Linux input codes leak into the
// UI.
#pragma once

#include <cstddef>
#include <functional>
#include <memory>

struct wl_event_loop;

namespace tv {

enum class PadButton {
    Up,
    Down,
    Left,
    Right,
    Select,
    Back,
    Home,
    Menu,
    PageLeft,
    PageRight,
};

struct PadEvent {
    PadButton button;
    bool pressed;
};

class ControllerManager {
public:
    using Handler = std::function<void(const PadEvent&)>;

    ControllerManager();
    ~ControllerManager();
    ControllerManager(const ControllerManager&) = delete;
    ControllerManager& operator=(const ControllerManager&) = delete;

    // Starts scanning /dev/input/event* and listening for hotplug. The handler
    // runs on the compositor's event loop thread.
    void init(struct wl_event_loop* loop, Handler handler);
    void shutdown();

    // Advances the held-direction auto-repeat clock. Call from the shell's timer
    // so holding left/right keeps moving even when no other event arrives.
    void pollRepeats(double nowMs);

    // True while a direction is held, so the shell knows it has to keep the
    // repeat clock running. When nothing is held it can sleep.
    bool repeating() const;

    std::size_t count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace tv
