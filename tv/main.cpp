// tvshell -- a controller-first TV compositor for the Raspberry Pi 3.
//
//   tvshell [--headless] [--drm] [--socket NAME] [--width W] [--height H]
//           [--frames N] [--dump PATH] [-e CMD]
//
// With WAYLAND_DISPLAY set it nests inside the current Wayland session, which
// is the quick way to develop. On the Pi it is normally started from a TTY with
// --drm, taking over the HDMI output directly.
#include "manager.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/wait.h>

namespace {

tv::Manager* g_manager = nullptr;

void onSignal(int) {
    if (g_manager) g_manager->stop();
}

void onChildExit(int) {
    const int saved = errno;
    while (waitpid(-1, nullptr, WNOHANG) > 0) {
    }
    errno = saved;
}

void usage(const char* argv0) {
    std::printf(
        "Usage: %s [options]\n"
        "\n"
        "  --headless          render offscreen (no display needed)\n"
        "  --drm               take over a TTY through DRM + libinput\n"
        "  --nested            force nesting in the current Wayland session\n"
        "  -f, --fullscreen    fullscreen the nested window\n"
        "  --minimal           reduce motion and fill rate (Pi 3; on with --drm)\n"
        "  --socket NAME       Wayland socket name to advertise\n"
        "  --width W           output width for headless/nested (default 1280)\n"
        "  --height H          output height (default 720)\n"
        "  --frames N          render N frames then exit (self test)\n"
        "  --dump PATH         write the last frame as a PPM\n"
        "  --no-vsync          do not hint vsync\n"
        "  -e, --exec CMD      launch CMD once the compositor is up\n"
        "  -h, --help          this message\n"
        "\n"
        "Controller: d-pad/left stick navigate, A select, B back, Guide home,\n"
        "LB/RB page, Menu opens the Control Centre. Xbox, DualShock 4 and\n"
        "DualSense are all read\n"
        "directly from /dev/input. A Fire TV remote works over Bluetooth: its\n"
        "d-pad/select/back/home/menu arrive as a keyboard, which is handled too.\n"
        "Keyboard arrows/Enter/Escape/Home also work.\n"
        "\n"
        "On the Pi, run ./tv.sh --drm from a VT. The user needs read access to\n"
        "/dev/input/event* (normally the 'input' group) for the controller.\n",
        argv0);
}

}  // namespace

int main(int argc, char** argv) {
    tv::Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        }
        if (a == "--headless") {
            options.headless = true;
            continue;
        }
        if (a == "--drm") {
            options.drm = true;
            continue;
        }
        if (a == "--nested") {
            options.drm = false;
            options.headless = false;
            continue;
        }
        if (a == "--no-vsync") {
            options.noVsync = true;
            continue;
        }
        if (a == "-f" || a == "--fullscreen") {
            options.fullscreen = true;
            continue;
        }
        if (a == "--minimal") {
            options.minimal = true;
            continue;
        }
        if ((a == "--socket") && i + 1 < argc) {
            options.socket = argv[++i];
            continue;
        }
        if ((a == "--width") && i + 1 < argc) {
            options.width = std::atoi(argv[++i]);
            if (options.width < 320) options.width = 320;
            continue;
        }
        if ((a == "--height") && i + 1 < argc) {
            options.height = std::atoi(argv[++i]);
            if (options.height < 240) options.height = 240;
            continue;
        }
        if ((a == "--frames") && i + 1 < argc) {
            options.frames = std::atoi(argv[++i]);
            if (options.frames < 0) options.frames = 0;
            continue;
        }
        if ((a == "--dump") && i + 1 < argc) {
            options.dump = argv[++i];
            continue;
        }
        if ((a == "-e" || a == "--exec") && i + 1 < argc) {
            options.launch.emplace_back(argv[++i]);
            continue;
        }
        std::fprintf(stderr, "tvshell: unknown option '%s' (try --help)\n", a.c_str());
        return 2;
    }

    struct sigaction childAction {};
    childAction.sa_handler = onChildExit;
    sigemptyset(&childAction.sa_mask);
    childAction.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &childAction, nullptr);

    struct sigaction signalAction {};
    signalAction.sa_handler = onSignal;
    sigemptyset(&signalAction.sa_mask);
    signalAction.sa_flags = SA_RESTART;
    sigaction(SIGINT, &signalAction, nullptr);
    sigaction(SIGTERM, &signalAction, nullptr);

    tv::Manager manager;
    g_manager = &manager;
    const int rc = manager.run(options);
    g_manager = nullptr;
    return rc;
}
