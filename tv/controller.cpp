#include "controller.h"

extern "C" {
#include <wayland-server-core.h>
}

#include <linux/input.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

namespace tv {
namespace {

enum class Dir { Up, Down, Left, Right, None };

constexpr uint8_t kKeySource = 1;
constexpr uint8_t kHatSource = 2;
constexpr uint8_t kStickSource = 4;

PadButton buttonFor(Dir d) {
    switch (d) {
        case Dir::Up: return PadButton::Up;
        case Dir::Down: return PadButton::Down;
        case Dir::Left: return PadButton::Left;
        case Dir::Right: return PadButton::Right;
        default: return PadButton::Select;
    }
}

bool testBit(const unsigned long* bits, unsigned bit) {
    return (bits[bit / (8 * sizeof(unsigned long))] >>
            (bit % (8 * sizeof(unsigned long)))) &
           1ul;
}

bool hasEventBit(int fd, unsigned long ev, unsigned code) {
    unsigned long bits[(KEY_MAX + 8 * sizeof(unsigned long)) /
                       (8 * sizeof(unsigned long))] = {};
    if (ioctl(fd, EVIOCGBIT(ev, sizeof(bits)), bits) < 0) return false;
    return testBit(bits, code);
}

// A gamepad is recognised by the presence of a face/d-pad/guide button and at
// least one analog axis. This deliberately rejects ordinary keyboards and
// touchpads, which wlroots already feeds through the seat.
//
// BTN_MODE is what catches the TV remotes that present themselves as a joystick
// with only a hat and a guide button (several Bluetooth remotes do), while the
// PlayStation pads (DualShock 4, DualSense) always expose BTN_SOUTH and ABS_X.
bool looksLikeGamepad(int fd) {
    const bool face = hasEventBit(fd, EV_KEY, BTN_SOUTH) ||
                      hasEventBit(fd, EV_KEY, BTN_DPAD_UP) ||
                      hasEventBit(fd, EV_KEY, BTN_MODE);
    const bool axes = hasEventBit(fd, EV_ABS, ABS_X) ||
                      hasEventBit(fd, EV_ABS, ABS_HAT0X);
    return face && axes;
}

// The name a pad is reported by, so the log says "DualSense" rather than
// "Wireless Controller" and a Fire TV remote is recognisable at a glance.
std::string padLabel(unsigned short vendor, unsigned short product) {
    switch (vendor) {
        case 0x054C:  // Sony
            switch (product) {
                case 0x05C4:
                case 0x09CC:
                case 0x0BA0:
                    return "DualShock 4";
                case 0x0CE6:
                    return "DualSense";
                case 0x0DF2:
                    return "DualSense Edge";
                default:
                    return "PlayStation controller";
            }
        case 0x045E: return "Xbox controller";
        case 0x057E: return "Switch controller";
        case 0x0171: return "Amazon Fire TV remote";
        default: break;
    }
    return {};
}

}  // namespace

struct ControllerManager::Impl {
    struct Device {
        Impl* owner = nullptr;
        std::string path;
        std::string name;
        int fd = -1;
        struct wl_event_source* source = nullptr;
        bool dead = false;

        // Direction sources are OR-ed so the d-pad and the left stick can hold
        // the same direction without one releasing the other.
        std::array<uint8_t, 4> dirMask{};
        Dir repeatDir = Dir::None;
        double nextRepeat = 0.0;

        unsigned short vendor = 0;
        unsigned short product = 0;
        int absMinX = 0, absMaxX = 0, absCenterX = 0;
        int absMinY = 0, absMaxY = 0, absCenterY = 0;
        int stickX = 0, stickY = 0;
        int hatX = 0, hatY = 0;
    };

    struct wl_event_loop* loop = nullptr;
    Handler handler;
    std::vector<std::unique_ptr<Device>> devices;
    struct wl_event_source* rescan = nullptr;

    void emit(PadButton b, bool pressed) {
        if (handler) handler(PadEvent{b, pressed});
    }

    void setDir(Device& d, Dir dir, uint8_t source, bool pressed) {
        if (dir == Dir::None) return;
        const size_t i = static_cast<size_t>(dir);
        const bool was = d.dirMask[i] != 0;
        if (pressed) d.dirMask[i] |= source;
        else d.dirMask[i] &= uint8_t(~source);
        const bool now = d.dirMask[i] != 0;
        if (!was && now) {
            emit(buttonFor(dir), true);
            d.repeatDir = dir;
            d.nextRepeat = 0.0;  // pollRepeats fills in the first delay
        } else if (was && !now) {
            emit(buttonFor(dir), false);
            if (d.repeatDir == dir) d.repeatDir = Dir::None;
        }
    }

    void updateStick(Device& d) {
        const int rangeX = d.absMaxX - d.absMinX;
        const int rangeY = d.absMaxY - d.absMinY;
        const double nx = rangeX > 0 ? double(d.stickX - d.absCenterX) / double(rangeX) : 0.0;
        const double ny = rangeY > 0 ? double(d.stickY - d.absCenterY) / double(rangeY) : 0.0;
        constexpr double kDead = 0.32;
        setDir(d, Dir::Left, kStickSource, nx < -kDead);
        setDir(d, Dir::Right, kStickSource, nx > kDead);
        setDir(d, Dir::Up, kStickSource, ny < -kDead);
        setDir(d, Dir::Down, kStickSource, ny > kDead);
    }

    void updateHat(Device& d) {
        setDir(d, Dir::Left, kHatSource, d.hatX < 0);
        setDir(d, Dir::Right, kHatSource, d.hatX > 0);
        setDir(d, Dir::Up, kHatSource, d.hatY < 0);
        setDir(d, Dir::Down, kHatSource, d.hatY > 0);
    }

    void handleKey(Device& d, unsigned code, int value) {
        if (value != 0 && value != 1 && value != 2) return;
        const bool pressed = value != 0;
        const bool repeat = value == 2;

        Dir dir = Dir::None;
        switch (code) {
            case BTN_DPAD_UP:
            case KEY_UP: dir = Dir::Up; break;
            case BTN_DPAD_DOWN:
            case KEY_DOWN: dir = Dir::Down; break;
            case BTN_DPAD_LEFT:
            case KEY_LEFT: dir = Dir::Left; break;
            case BTN_DPAD_RIGHT:
            case KEY_RIGHT: dir = Dir::Right; break;
            default: break;
        }
        if (dir != Dir::None) {
            if (repeat) {
                emit(buttonFor(dir), true);
                d.repeatDir = dir;
                d.nextRepeat = 0.0;
                return;
            }
            setDir(d, dir, kKeySource, pressed);
            return;
        }

        PadButton b;
        switch (code) {
            case BTN_SOUTH:
            case KEY_ENTER:
            case KEY_KPENTER:
#ifdef KEY_OK
            case KEY_OK:
#endif
                b = PadButton::Select;
                break;
            case BTN_EAST:
            case KEY_ESC:
            case KEY_BACK:
                b = PadButton::Back;
                break;
            case BTN_NORTH:
                b = PadButton::Menu;
                break;
            case BTN_WEST:
            case BTN_TL:
                b = PadButton::PageLeft;
                break;
            case BTN_TR:
                b = PadButton::PageRight;
                break;
            // DualShock 4 / DualSense: Options opens the section menu and
            // Share/Create steps back, which is what a TV remote's menu and
            // back buttons do.
            case BTN_START:
                b = PadButton::Menu;
                break;
            case BTN_SELECT:
                b = PadButton::Back;
                break;
            case BTN_MODE:
            case KEY_HOMEPAGE:
            case KEY_HOME:
                b = PadButton::Home;
                break;
            // TV remotes that report as a joystick rather than a keyboard.
            case KEY_MENU:
            case KEY_INFO:
                b = PadButton::Menu;
                break;
            case KEY_PAGEUP:
                b = PadButton::PageLeft;
                break;
            case KEY_PAGEDOWN:
                b = PadButton::PageRight;
                break;
            default:
                return;
        }
        if (repeat) return;
        emit(b, pressed);
    }

    void handle(Device& d, const struct input_event& ev) {
        switch (ev.type) {
            case EV_KEY:
                handleKey(d, ev.code, ev.value);
                break;
            case EV_ABS:
                if (ev.code == ABS_X) {
                    d.stickX = ev.value;
                    updateStick(d);
                } else if (ev.code == ABS_Y) {
                    d.stickY = ev.value;
                    updateStick(d);
                } else if (ev.code == ABS_HAT0X) {
                    d.hatX = ev.value;
                    updateHat(d);
                } else if (ev.code == ABS_HAT0Y) {
                    d.hatY = ev.value;
                    updateHat(d);
                }
                break;
            default:
                break;
        }
    }

    static int onReadable(int fd, uint32_t, void* data) {
        Device* d = static_cast<Device*>(data);
        if (!d || d->dead) return 0;
        struct input_event evs[32];
        for (;;) {
            const ssize_t n = read(fd, evs, sizeof(evs));
            if (n < 0) {
                if (errno == EAGAIN || errno == EINTR) return 0;
                d->dead = true;
                if (d->source) {
                    wl_event_source_remove(d->source);
                    d->source = nullptr;
                }
                close(d->fd);
                d->fd = -1;
                return 0;
            }
            if (n == 0) return 0;
            const ssize_t count = n / ssize_t(sizeof(struct input_event));
            for (ssize_t i = 0; i < count; ++i) d->owner->handle(*d, evs[i]);
        }
    }

    static int onRescan(void* data) {
        Impl* impl = static_cast<Impl*>(data);
        impl->scan();
        if (impl->rescan)
            wl_event_source_timer_update(impl->rescan, 2000);
        return 0;
    }

    void addDevice(const std::string& path) {
        for (const auto& d : devices)
            if (!d->dead && d->path == path) return;

        const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) return;
        if (!looksLikeGamepad(fd)) {
            close(fd);
            return;
        }

        auto dev = std::make_unique<Device>();
        dev->owner = this;
        dev->path = path;
        dev->fd = fd;
        char name[128] = {};
        if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0) dev->name = name;
        struct input_id id {};
        const bool haveId = ioctl(fd, EVIOCGID, &id) >= 0;
        if (haveId) {
            dev->vendor = id.vendor;
            dev->product = id.product;
        }

        struct input_absinfo info {};
        if (ioctl(fd, EVIOCGABS(ABS_X), &info) >= 0) {
            dev->absMinX = info.minimum;
            dev->absMaxX = info.maximum;
            dev->absCenterX = (info.minimum + info.maximum) / 2;
            dev->stickX = dev->absCenterX;
        }
        if (ioctl(fd, EVIOCGABS(ABS_Y), &info) >= 0) {
            dev->absMinY = info.minimum;
            dev->absMaxY = info.maximum;
            dev->absCenterY = (info.minimum + info.maximum) / 2;
            dev->stickY = dev->absCenterY;
        }

        dev->source = wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE, onReadable, dev.get());
        if (!dev->source) {
            close(fd);
            return;
        }
        std::string label = padLabel(dev->vendor, dev->product);
        if (label.empty()) label = dev->name;
        if (label.empty()) label = "gamepad";
        std::fprintf(stderr, "[tvshell] controller: %s (%s)\n", label.c_str(), path.c_str());
        devices.push_back(std::move(dev));
    }

    void scan() {
        for (int i = 0; i < 64; ++i) {
            char path[64];
            std::snprintf(path, sizeof(path), "/dev/input/event%d", i);
            addDevice(path);
        }
        devices.erase(
            std::remove_if(devices.begin(), devices.end(),
                           [](const std::unique_ptr<Device>& d) { return d->dead; }),
            devices.end());
    }

    void pollRepeats(double now) {
        for (auto& d : devices) {
            if (d->dead || d->repeatDir == Dir::None) continue;
            if (d->nextRepeat <= 0.0) {
                d->nextRepeat = now + 400.0;
                continue;
            }
            if (now < d->nextRepeat) continue;
            emit(buttonFor(d->repeatDir), true);
            d->nextRepeat = now + 110.0;
        }
    }
};

ControllerManager::ControllerManager() : impl_(std::make_unique<Impl>()) {}
ControllerManager::~ControllerManager() { shutdown(); }

void ControllerManager::init(struct wl_event_loop* loop, Handler handler) {
    shutdown();
    impl_->loop = loop;
    impl_->handler = std::move(handler);
    impl_->scan();
    impl_->rescan = wl_event_loop_add_timer(loop, Impl::onRescan, impl_.get());
    if (impl_->rescan) wl_event_source_timer_update(impl_->rescan, 2000);
}

void ControllerManager::shutdown() {
    if (!impl_) return;
    if (impl_->rescan) {
        wl_event_source_remove(impl_->rescan);
        impl_->rescan = nullptr;
    }
    for (auto& d : impl_->devices) {
        if (d->source) {
            wl_event_source_remove(d->source);
            d->source = nullptr;
        }
        if (d->fd >= 0) close(d->fd);
        d->fd = -1;
    }
    impl_->devices.clear();
    impl_->handler = nullptr;
}

void ControllerManager::pollRepeats(double nowMs) {
    if (impl_) impl_->pollRepeats(nowMs);
}

std::size_t ControllerManager::count() const {
    return impl_ ? impl_->devices.size() : 0;
}

}  // namespace tv
