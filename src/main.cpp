// win11wm -- a GPU accelerated, vsynced X11 compositor + window manager with a
// Windows 11 ("Fluent") shell.
//
//   win11wm [--display :N] [--stats] [--no-vsync] [--no-unredirect] [--frames N]
//
// It takes over the root window of a free display: every top level X11 window
// is redirected with XComposite and composited onto an overlay window through
// GLX_EXT_texture_from_pixmap, so clients keep running untouched -- which is
// what lets *any* X11 program work inside it.
#include "wm/manager.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

wm::Manager* g_manager = nullptr;

void onSignal(int) {
    // Async-signal-safe: the manager's poll() wakes with EINTR, notices the
    // flag and shuts down through its normal path.
    if (g_manager) g_manager->stop();
}

void usage(const char* argv0) {
    std::printf(
        "Usage: %s [options]\n"
        "\n"
        "  -d, --display :N    X display to manage (default: $DISPLAY)\n"
        "  --no-vsync          do not force a swap interval (benchmark / tearing)\n"
        "  --stats             show a live FPS, frame time and renderer HUD\n"
        "  --no-unredirect     keep fullscreen windows inside the compositor\n"
        "  -n, --frames N      render N frames then exit (self test)\n"
        "  -e, --exec CMD      launch CMD once the WM is up (repeatable);\n"
        "                      combines with --frames for scripted tests\n"
        "  -h, --help          this message\n"
        "\n"
        "Shortcuts\n"
        "  Win                     Start menu (type to search, Enter to launch)\n"
        "  Win+Left/Right/Up/Down  snap left / right / maximise / restore\n"
        "  Win+Shift+Left/Right    snap to a half without toggling\n"
        "  Win+Tab                 Task View\n"
        "  click the clock         Control Centre (iOS quick settings)\n"
        "  Control Centre          \"Tablet mode\" switches to the iOS-like\n"
        "                          home screen (dock, status bar, splash back)\n"
        "  Win+D / Win+M           show desktop\n"
        "  Win+1..9                activate the n-th taskbar window\n"
        "  drag the taskbar edge   resize the taskbar, as in Windows 10\n"
        "  Win+R                   Start menu (search focused)\n"
        "  Alt+Tab                 switcher (release Alt to switch)\n"
        "  Alt+F4                  close window\n"
        "  Alt+Space               window menu\n"
        "  Alt+drag                move a window anywhere\n"
        "  drag to an edge         snap preview, release to snap\n"
        "  right click a caption   window menu\n"
        "\n"
        "This window manager takes over the root window, so it needs a display\n"
        "without another window manager running (see README.md).\n",
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
        if ((a == "-d" || a == "--display") && i + 1 < argc) {
            options.display = argv[++i];
            continue;
        }
        if (a.rfind("--display=", 0) == 0) {
            options.display = a.substr(std::strlen("--display="));
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
        if (a == "--no-unredirect") {
            options.unredirect = false;
            continue;
        }
        if ((a == "-n" || a == "--frames") && i + 1 < argc) {
            options.frames = std::atoi(argv[++i]);
            if (options.frames < 0) options.frames = 0;
            continue;
        }
        if ((a == "-e" || a == "--exec") && i + 1 < argc) {
            // Kept for parity with the old SDL prototype's --embed flag:
            // --embed used to launch ONE client inside a separate SDL
            // compositor; here every app goes through the single GPU
            // compositor via launchApp() instead.
            options.launch.emplace_back(argv[++i]);
            continue;
        }
        std::fprintf(stderr, "win11wm: unknown option '%s' (try --help)\n", a.c_str());
        return 2;
    }

    // Launched applications are reaped automatically, and a client that dies
    // mid-write must not take us down with it.
    std::signal(SIGCHLD, SIG_IGN);
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
