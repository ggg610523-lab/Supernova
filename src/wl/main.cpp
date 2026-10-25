// win11wm-wl -- the Windows 11 Fluent shell as a native Wayland compositor.
//
//   win11wm-wl [--headless] [--drm] [--socket NAME] [--stats] [--frames N]
//              [--exec CMD] [--tablet]
//
// By default it nests inside the Wayland session it was started from
// ($WAYLAND_DISPLAY), which is the quickest way to run and inspect the shell.
// Its own clients connect to the socket it advertises (printed at start-up, also
// exported as WAYLAND_DISPLAY for children launched with --exec).
#include "manager.h"

#include <csignal>
#include <cstdio>
#include <sys/wait.h>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <string>

namespace {

wm::Manager* g_manager = nullptr;

void onSignal(int) {
    if (g_manager) g_manager->stop();
}

// Reap launched apps. SIGCHLD used to be SIG_IGN, which the kernel turns into
// auto-reaping *and* which every child inherits: an app that ignores SIGCHLD
// cannot wait() for its own children (waitpid fails with ECHILD), and
// multi-process apps such as Firefox abort in their IPC/crash-helper setup.
// A handler keeps the compositor zombie-free without leaking the bad
// disposition into the apps it starts.
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
        "  --headless          render offscreen (no host compositor needed)\n"
        "  --drm               take over a TTY through the DRM/libinput backend\n"
        "  --socket NAME       Wayland socket name to advertise\n"
        "  --no-vsync          do not hint vsync (nesting still paces us)\n"
        "  --stats             draw the FPS/frame-time HUD\n"
        "  --perf              like --stats, plus PERF lines on stderr\n"
        "  --tablet            boot into the tablet/mobile home screen\n"
        "  -n, --frames N      render N frames then exit (self test)\n"
        "  --dump PATH         with --frames: write the last frame as a PPM\n"
        "  -e, --exec CMD      launch CMD once the compositor is up\n"
        "  -h, --help          this message\n"
        "\n"
        "Shortcuts: Win (Start), Win+Left/Right/Up/Down (snap/maximise),\n"
        "Win+Tab (Task View), Win+D (show desktop), Alt+Tab, Alt+F4, Alt+drag.\n"
        "\n"
        "Clients must be started with WAYLAND_DISPLAY pointed at the socket this\n"
        "process prints; X11 programs need XWayland, which this milestone does\n"
        "not host yet.\n",
        argv0);
}

}  // namespace

int main(int argc, char** argv) {
    wm::Options options;
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
            options.nested = true;
            continue;
        }
        if ((a == "--socket") && i + 1 < argc) {
            options.socket = argv[++i];
            continue;
        }
        if (a == "--no-vsync") {
            options.vsync = false;
            continue;
        }
        if (a == "--stats") {
            options.stats = true;
            continue;
        }
        if (a == "--perf") {
            options.stats = true;
            options.perfLog = true;
            continue;
        }
        if (a == "--tablet" || a == "--mobile") {
            options.tablet = true;
            continue;
        }
        if (a == "--dump" && i + 1 < argc) {
            options.dumpPath = argv[++i];
            continue;
        }
        if ((a == "-n" || a == "--frames") && i + 1 < argc) {
            options.frames = std::atoi(argv[++i]);
            if (options.frames < 0) options.frames = 0;
            continue;
        }
        if ((a == "-e" || a == "--exec") && i + 1 < argc) {
            options.launch.emplace_back(argv[++i]);
            continue;
        }
        std::fprintf(stderr, "win11wm-wl: unknown option '%s' (try --help)\n", a.c_str());
        return 2;
    }

    struct sigaction childAction {};
    childAction.sa_handler = onChildExit;
    childAction.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&childAction.sa_mask);
    sigaction(SIGCHLD, &childAction, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    wm::Manager manager;
    g_manager = &manager;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGHUP, onSignal);

    const int rc = manager.run(options);
    g_manager = nullptr;
    return rc;
}
