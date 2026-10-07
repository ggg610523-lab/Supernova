#include "manager.h"
#include "xprop.h"

#include <X11/cursorfont.h>
#include <X11/keysym.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <unistd.h>

namespace wm {

// X errors are a fact of life for a WM: clients die between our last XSync and
// the next request. BadWindow/BadDrawable/BadMatch are expected races, so they
// are swallowed; anything else is logged so debugging stays possible.
int g_lastError = 0;
unsigned long g_errorCount = 0;

namespace {

// The three stacking passes restack() runs, in the order they are pushed.
enum class Pass { Background, Managed, Top };

int xErrorHandler(Display* dpy, XErrorEvent* ev) {
    g_lastError = ev->error_code;
    ++g_errorCount;
    switch (ev->error_code) {
        case BadWindow:
        case BadDrawable:
        case BadMatch:
        case BadColor:
        case BadAtom:
            return 0;
        default:
            break;
    }
    char text[128] = {0};
    XGetErrorText(dpy, ev->error_code, text, sizeof text - 1);
    log("X error: %s (request %d.%d, resource 0x%lx)", text, ev->request_code, ev->minor_code,
        ev->resourceid);
    return 0;
}

}  // namespace

Manager::~Manager() { shutdown(); }

void Manager::shutdown() {
    if (!dpy) return;
    ungrabPointer();
    XUngrabKeyboard(dpy, CurrentTime);
    // The glyph and icon caches own GL textures, so they have to go *before*
    // the compositor tears the context down: glDeleteTextures after
    // glXMakeCurrent(None) trips libepoxy's "no current context" assertion and
    // aborts the process on the way out.
    text.shutdown();
    icons.clear();
    comp.shutdown();
    // Release the compositing manager selection before the window that owns it
    // goes away, so clients do not keep talking to a dead compositor.
    if (A.netWmCm) XSetSelectionOwner(dpy, A.netWmCm, None, CurrentTime);
    if (wmCheckWin) {
        XDestroyWindow(dpy, wmCheckWin);
        wmCheckWin = 0;
    }
    for (Cursor& c : cursors) {
        if (c) {
            XFreeCursor(dpy, c);
            c = 0;
        }
    }
    XSync(dpy, False);
    XCloseDisplay(dpy);
    dpy = nullptr;
}

bool Manager::openDisplay(const std::string& name, std::string* error) {
    dpy = XOpenDisplay(name.empty() ? nullptr : name.c_str());
    if (!dpy) {
        *error = "cannot open the X display" +
                 (name.empty() ? std::string(" named by $DISPLAY") : " " + name);
        return false;
    }
    XSetErrorHandler(xErrorHandler);

    // All EWMH/ICCCM atoms are 0 until this runs. setupRootProperties(),
    // mapClient(), removeClient() and every atom comparison depend on it.
    A.init(dpy);

    screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);
    rootVisual = DefaultVisual(dpy, screen);
    rootDepth = DefaultDepth(dpy, screen);
    rootCmap = DefaultColormap(dpy, screen);
    screenW = DisplayWidth(dpy, screen);
    screenH = DisplayHeight(dpy, screen);

    int dummy = 0;
    compositeOk = XCompositeQueryExtension(dpy, &dummy, &dummy) != 0;
    damageOk = XDamageQueryExtension(dpy, &damageEventBase, &damageErrorBase) != 0;
    xfixesOk = XFixesQueryExtension(dpy, &dummy, &dummy) != 0;
    int major = 0, minor = 0;
    renderOk = XRenderQueryVersion(dpy, &major, &minor) != 0;

    if (!compositeOk) {
        *error = "the X server lacks the Composite extension";
        return false;
    }
    if (!damageOk) {
        *error = "the X server lacks the DAMAGE extension";
        return false;
    }
    return true;
}

bool Manager::claimWm(std::string* error) {
    g_lastError = 0;
    XSelectInput(dpy, root,
                 SubstructureRedirectMask | SubstructureNotifyMask | PropertyChangeMask |
                     StructureNotifyMask | KeyPressMask | KeyReleaseMask);
    XSync(dpy, False);
    if (g_lastError == BadAccess) {
        *error = "another window manager is already running on this display";
        return false;
    }
    return true;
}

void Manager::setupRootProperties() {
    // _NET_SUPPORTING_WM_CHECK: an EWMH compliant WM advertises itself with a
    // window it owns, so clients can recognise which WM is running.
    wmCheckWin = XCreateSimpleWindow(dpy, root, -100, -100, 1, 1, 0, 0, 0);
    XChangeProperty(dpy, root, A.netSupportingWmCheck, XA_WINDOW, 32, PropModeReplace,
                    reinterpret_cast<unsigned char*>(&wmCheckWin), 1);
    XChangeProperty(dpy, wmCheckWin, A.netSupportingWmCheck, XA_WINDOW, 32, PropModeReplace,
                    reinterpret_cast<unsigned char*>(&wmCheckWin), 1);
    const std::string name = "win11wm";
    XChangeProperty(dpy, wmCheckWin, A.netWmName, A.utf8String, 8, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(name.c_str()),
                    static_cast<int>(name.size()));

    // Claim the compositing manager selection. Toolkits query its owner to
    // decide whether an ARGB visual -- and with it their composited rendering
    // path -- is available; a compositor that skips this leaves GTK announcing
    // "no RGBA visual or compositor", falling back to a 24 bit visual and
    // half-initialising WebKit based apps (Electrobun, Tauri).
    char cmName[32];
    std::snprintf(cmName, sizeof cmName, "_NET_WM_CM_S%d", screen);
    A.netWmCm = XInternAtom(dpy, cmName, False);
    if (A.netWmCm) XSetSelectionOwner(dpy, A.netWmCm, wmCheckWin, CurrentTime);

    const Atom supported[] = {
        A.netSupported,          A.netSupportingWmCheck,  A.netClientList,
        A.netClientListStacking, A.netNumberOfDesktops,   A.netCurrentDesktop,
        A.netDesktopNames,       A.netDesktopGeometry,    A.netDesktopViewport,
        A.netWorkarea,           A.netActiveWindow,       A.netCloseWindow,
        A.netMoveresizeWindow,   A.netWmMoveresize,       A.netWmName,
        A.netWmIcon,             A.netWmPid,              A.netWmWindowType,
        A.netWmState,            A.netWmAllowedActions,   A.netWmStrut,
        A.netWmStrutPartial,     A.netFrameExtents,       A.netWmDesktop,
        A.netWmUserTime,         A.netRestackWindow,      A.netRequestFrameExtents,
        A.netShowingDesktop,     A.netWmOpacity,          A.netWmOpaqueRegion,
        A.netWmBypassCompositor, A.netStartupId,          A.netWmPing,
        A.typeNormal,            A.typeDesktop,           A.typeDock,
        A.typeToolbar,           A.typeMenu,              A.typeUtility,
        A.typeSplash,            A.typeDialog,            A.typeDropdownMenu,
        A.typePopupMenu,         A.typeTooltip,           A.typeNotification,
        A.stateModal,            A.stateSticky,           A.stateMaximizedVert,
        A.stateMaximizedHorz,    A.stateShaded,           A.stateSkipTaskbar,
        A.stateSkipPager,        A.stateHidden,           A.stateFullscreen,
        A.stateAbove,            A.stateBelow,            A.stateDemandsAttention,
        A.stateFocused,          A.actionMove,            A.actionResize,
        A.actionMinimize,        A.actionMaximizeHorz,    A.actionMaximizeVert,
        A.actionFullscreen,      A.actionClose,
    };
    XChangeProperty(dpy, root, A.netSupported, XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(supported),
                    static_cast<int>(sizeof(supported) / sizeof(supported[0])));

    const long one = 1, zero = 0;
    XChangeProperty(dpy, root, A.netNumberOfDesktops, XA_CARDINAL, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&one), 1);
    XChangeProperty(dpy, root, A.netCurrentDesktop, XA_CARDINAL, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&zero), 1);
    XChangeProperty(dpy, root, A.netShowingDesktop, XA_CARDINAL, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&zero), 1);
    const char desktopName[] = "Desktop";
    XChangeProperty(dpy, root, A.netDesktopNames, A.utf8String, 8, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(desktopName), sizeof(desktopName) - 1);
    const long geometry[2] = {screenW, screenH};
    XChangeProperty(dpy, root, A.netDesktopGeometry, XA_CARDINAL, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(geometry), 2);
    const long viewport[2] = {0, 0};
    XChangeProperty(dpy, root, A.netDesktopViewport, XA_CARDINAL, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(viewport), 2);
    updateClientList();
    updateWorkArea();
}

bool Manager::grabKeys(std::string* error) {
    // Global shortcuts. Windows itself uses Win for the Start menu, Win+arrows
    // for snapping, Win+Tab for Task View and Alt+Tab for the switcher.
    struct Binding {
        KeySym sym;
        unsigned mods;
    };
    const Binding bindings[] = {
        {XK_Super_L, 0},      {XK_Super_R, 0},
        {XK_d, Mod4Mask},     {XK_m, Mod4Mask},      {XK_r, Mod4Mask},
        {XK_Tab, Mod4Mask},   {XK_Left, Mod4Mask},   {XK_Right, Mod4Mask},
        {XK_Up, Mod4Mask},    {XK_Down, Mod4Mask},   {XK_Left, Mod4Mask | ShiftMask},
        {XK_Right, Mod4Mask | ShiftMask}, {XK_Up, Mod4Mask | ShiftMask},
        {XK_Down, Mod4Mask | ShiftMask},  {XK_Tab, Mod1Mask},
        {XK_Tab, Mod1Mask | ShiftMask},   {XK_F4, Mod1Mask},
        {XK_space, Mod4Mask},
        {XK_1, Mod4Mask}, {XK_2, Mod4Mask}, {XK_3, Mod4Mask}, {XK_4, Mod4Mask},
        {XK_5, Mod4Mask}, {XK_6, Mod4Mask}, {XK_7, Mod4Mask}, {XK_8, Mod4Mask},
        {XK_9, Mod4Mask},
    };
    // CapsLock and NumLock change the modifier state, so every binding is
    // grabbed with all four lock combinations.
    const unsigned locks[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};

    g_lastError = 0;
    for (const Binding& b : bindings) {
        const KeyCode code = XKeysymToKeycode(dpy, b.sym);
        if (!code) continue;
        for (unsigned lock : locks) {
            // owner_events = False on purpose: the WM *is* the client that owns
            // the grab, and with True the server delivers Super+Tab to whatever
            // window has the input focus instead, so shortcuts silently die as
            // soon as a window is focused.
            XGrabKey(dpy, code, b.mods | lock, root, False, GrabModeAsync, GrabModeAsync);
        }
    }
    // Alt+drag moves, Alt+right-drag resizes: the muscle memory Windows and
    // every other X11 desktop share. Same reasoning as above, and without it
    // the press is delivered to the client under the pointer and never starts a
    // drag.
    for (unsigned lock : locks) {
        const unsigned mask = Mod1Mask | lock;
        const unsigned events = ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
        XGrabButton(dpy, Button1, mask, root, False, events, GrabModeAsync, GrabModeAsync, None,
                    None);
        XGrabButton(dpy, Button3, mask, root, False, events, GrabModeAsync, GrabModeAsync, None,
                    None);
    }
    XSync(dpy, False);
    if (g_lastError == BadAccess && error) {
        *error = "some shortcuts are already grabbed by another client";
    }
    return true;
}

void Manager::setCursor(int which) {
    if (!dpy || !comp.overlay()) return;
    if (which < 0 || which > 6) which = 0;
    if (which == cursorShown && cursors[which]) return;
    if (!cursors[which]) {
        static const unsigned shapes[7] = {XC_left_ptr,       XC_sb_h_double_arrow,
                                           XC_sb_v_double_arrow, XC_top_left_corner,
                                           XC_top_right_corner,  XC_hand2,
                                           XC_xterm};
        cursors[which] = XCreateFontCursor(dpy, shapes[which]);
    }
    if (cursors[which]) {
        XDefineCursor(dpy, comp.overlay(), cursors[which]);
        cursorShown = which;
    }
}

void Manager::scanExistingWindows() {
    Window rootRet = 0, parentRet = 0, *children = nullptr;
    unsigned count = 0;
    if (!XQueryTree(dpy, root, &rootRet, &parentRet, &children, &count) || !children) return;
    for (unsigned i = 0; i < count; ++i) {
        if (children[i] == comp.overlay() || children[i] == wmCheckWin) continue;
        add(children[i], true);
    }
    XFree(children);
}

int Manager::run(const Options& options) {
    opts = &options;
    std::string error;
    if (!openDisplay(options.display, &error)) {
        log("%s", error.c_str());
        return 1;
    }
    if (!claimWm(&error)) {
        log("%s", error.c_str());
        return 1;
    }
    // Bundled assets: the wallpaper photo, Reversal icons (rasterised by
    // scripts/fetch-assets.sh) and the Roboto variable font. All three are
    // resolved before the compositor starts because it bakes the wallpaper as
    // part of init; each degrades gracefully when absent.
    assetDir = defaultAssetDir();
    comp.setVerbosePerf(options.perfLog);
    if (!comp.init(dpy, screen, screenW, screenH, options.vsync,
                   assetDir + "/wallpaper/wallpaper.png", &error)) {
        log("compositor: %s", error.c_str());
        return 1;
    }

    // Hatter (colourful app icons) is the default theme; assets/icons holds the
    // shell's own glyphs and the Control Centre look, assets/icons-hatter the
    // rasterised Hatter fallback for builds without librsvg.
    icons.init(assetDir + "/icons", assetDir + "/icons-hatter");
    text.init(assetDir + "/fonts");
    // The digital clock's display face, the same web-fetched Roboto as the UI font
    // opened at its heaviest cut. A miss is not fatal: displayText() falls back
    // to the UI font, so the clock still tells the time.
    clockText.initFromFile(assetDir + "/fonts/Roboto.ttf");

    std::string keyError;
    grabKeys(&keyError);
    if (!keyError.empty()) log("warning: %s", keyError.c_str());

    setupRootProperties();
    // The bar comes up at the thickness it was dragged to last time. Read before
    // the work area is published, since the work area is the screen minus the bar.
    metrics::taskbarH = loadTaskbarHeight();
    // Same for the palette: the shell comes up light or dark as the user left it.
    // Done before the first frame so nothing is ever painted in the wrong mode.
    theme::applyMode(loadThemeMode());
    updateWorkArea();
    apps = scanApps();
    pinned = loadPinned();
    recents = loadRecents();
    recentFiles = loadRecentFiles();
    launchpadOrder = loadLaunchpadOrder();
    // The desktop shows the session's real Desktop directory; layout is fixed, so
    // it is computed once here and reused by every frame and hit test. The cells
    // the user dragged icons into come back first, so the layout pass below puts
    // them straight where they were rather than in the default column.
    desktopItems = scanDesktop();
    desktopIconPlacement = loadDesktopIconPlacement();
    layoutDesktopIcons();
    initWidgets();
    scanExistingWindows();
    updateClientList();
    // Kick the first state probe now, off the critical path, so the Control
    // Centre is already populated the first time the clock is clicked.
    sysctl.init();

    // Kiosk / self-test aid: boot straight into the tablet home screen. There is
    // nothing to transition from at start up, so this skips the splash.
    if (const char* v = std::getenv("WIN11WM_TABLET")) {
        if (v[0] == '1') {
            tabletMode = true;
            tabletAnim = 1.0;
            applyTabletMode();
            log("tablet mode: starting on the iOS-like home screen");
        }
    }

    // Unified replacement for the old SDL prototype's "--embed prog": the single
    // compositor launches the requested programs and they show up as normal
    // managed clients (no second compositor, no reparent hack). Queued until
    // the first present() so fork()+GL cannot interleave: forking a
    // multithreaded GL process (epoxy dispatch, Mesa drivers) is what used to
    // abort the process with "Couldn't find current GLX or EGL context".
    for (const std::string& cmd : options.launch) {
        if (!cmd.empty()) pendingLaunches.push_back(cmd);
    }

    log("%s | %s | %s", comp.vendorName().c_str(), comp.rendererName().c_str(),
        comp.glVersion().c_str());
    log("texture_from_pixmap %s, vsync %s (swap interval %d), font \"%s\"",
        comp.hasTextureFromPixmap() ? "yes" : "NO", comp.vsyncActive() ? "on" : "off",
        comp.swapInterval(), text.family().c_str());
    log("canvas %dx%d, %zu managed window(s), %zu launcher entries", screenW, screenH,
        clients.size(), apps.size());
    log("desktop: %zu item(s) from %s", desktopItems.size(), desktopDir().c_str());
    if (!pinned.empty()) log("taskbar: %zu pinned launcher(s)", pinned.size());
    log("assets: %s", assetDir.c_str());

    lastTick = nowMs();
    dirty = true;
    loop();
    // The desktop as the user arranged it: cards and icon cells, written on the
    // way out so the next session opens on the same wallpaper layout. Mutations
    // save as they happen, so this is the copy that catches everything else.
    saveDesktopLayout();
    shutdown();
    return 0;
}

void Manager::loop() {
    const int fd = ConnectionNumber(dpy);
    while (running) {
        // Drain everything the server has for us before deciding to draw: one
        // frame per batch of events is what keeps dragging smooth.
        while (running && XPending(dpy) > 0) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            handleEvent(ev);
        }
        if (!running) break;

        tickAnimations(nowMs());
        if (opts->frames > 0) dirty = true;  // self test: render every iteration

        if (dirty) {
            render();
            comp.present();
            ++framesRendered;
            dirty = false;
            // Deferred --exec launches happen exactly once, right after the
            // first present(): the GL pipeline is fully live, and this stays
            // on the same single-threaded path as every other X call.
            if (!pendingLaunches.empty()) {
                for (const std::string& cmd : pendingLaunches) launchApp(cmd);
                pendingLaunches.clear();
            }
            if (opts->frames > 0 && framesRendered >= opts->frames) break;
            continue;
        }

        // Idle: hand the CPU back and wait for the next event. With vsync on,
        // SwapBuffers already paced the frame we just drew.
        XFlush(dpy);
        pollfd p{};
        p.fd = fd;
        p.events = POLLIN;
        // Widgets tick in the background: the clock on the wall second, the
        // battery every few seconds. Wake often enough to service them.
        int timeout = opts->stats ? 400 : -1;
        if (!widgets.empty() && (timeout < 0 || 250 < timeout)) timeout = 250;
        // A press on the home screen is waiting to turn into a long press, and the
        // hold is judged on a timer, so the loop has to wake often enough for it to
        // land on time instead of a whole poll interval late.
        if (tabletPressAt != 0) timeout = 16;
        poll(&p, 1, timeout);
        if (opts->stats) dirty = true;
    }
}

void Manager::tickAnimations(double now) {
    double dt = now - lastTick;
    lastTick = now;
    if (dt <= 0.0 || dt > 0.1) dt = 1.0 / 60.0;  // ignore stalls and the first frame
    const double dtMs = dt * 1000.0;

    // The home screen's long press is a timer, not an event, so it is serviced here.
    if (tabletAnim > 0.5) updateTabletLongPress();

    // The open folder eases in and out, and the sheet is laid out as it moves so
    // its cells are always where they are painted.
    if (tabletFolderOpen >= 0) {
        if (approach(tabletFolderAnim, 1.0, dtMs, 220.0)) dirty = true;
    } else if (tabletFolderAnim > 0.0) {
        if (approach(tabletFolderAnim, 0.0, dtMs, 180.0)) dirty = true;
        // The faded-out sheet's geometry goes only once it is invisible, so it is
        // still there to draw on the way down.
        if (tabletFolderAnim <= 0.0) {
            tabletFolderLast = -1;
            tabletFolderRects.clear();
            tabletFolderPanel = Rect{};
            tabletFolderPageCount = 1;
        }
    }

    for (auto& cp : clients) {
        Client* c = cp.get();
        const double p = c->animMs > 0 ? clamp01((now - c->animStart) / double(c->animMs)) : 1.0;
        const double eased = fluentEase(p);

        // Geometry either integrates through the springs (a continuous physical
        // trajectory, used by maximise/snap/restore/reflow and the release of a
        // move) or follows the legacy timed lerp that the launch placeholder and
        // the tablet zoom share. The two never run at the same time.
        if (c->geoSpring) {
            stepGeometry(c, dt);
        } else if (p < 1.0) {
            c->drawFrame = lerpRect(c->animFrom, c->frame, eased);
            dirty = true;
        } else {
            c->drawFrame = c->frame;
        }
        if (c->appear < 1.0) {
            c->appear = p >= 1.0 ? 1.0 : eased;
            dirty = true;
        }
        if (c->closing && c->vanish < 1.0) {
            c->vanish = p >= 1.0 ? 1.0 : eased;
            dirty = true;
        }
        // Minimise/restore runs linearly so it can be reversed mid flight. The
        // magic-lamp warp reads minFade directly, so this is its timeline.
        const double minTarget = c->minimized ? 1.0 : 0.0;
        if (c->minFade != minTarget) {
            // Tablet apps collapse into their home-screen icon (iOS 26) instead
            // of warping into a taskbar button, so they get their own duration.
            const double minMs = c->tabletApp ? double(metrics::kTabletMinMs)
                                              : double(metrics::kMinimizeMs);
            const double step = dtMs / minMs;
            c->minFade = c->minFade < minTarget ? std::min(minTarget, c->minFade + step)
                                                : std::max(minTarget, c->minFade - step);
            dirty = true;
        }
        if (c->attentionPulse > 0.0) {
            c->attentionPulse = std::max(0.0, c->attentionPulse - dtMs / 2000.0);
            dirty = true;
        }
    }

    // A close that never gets an answer is forced: the window is unmapped and
    // the client killed, which is the "end task" behaviour users expect.
    for (size_t i = clients.size(); i-- > 0;) {
        Client* c = clients[i].get();
        if (!c->closing || c->closeDeadline <= 0.0 || now <= c->closeDeadline) continue;
        if (c->id) XKillClient(dpy, c->id);
        removeClient(c);
        dirty = true;
    }

    // Minimised windows leave the screen once the animation has finished.
    for (auto& cp : clients) {
        Client* c = cp.get();
        if (c->minimized && c->mapped && !c->closing && c->minFade >= 1.0) unmapClient(c);
    }

    // Shell transitions (Start menu, Task View, Alt-Tab).
    const auto step = [&](double& v, double target, double ms) {
        if (v == target) return;
        const double d = dtMs / ms;
        v = target > v ? std::min(target, v + d) : std::max(target, v - d);
        dirty = true;
    };
    // The Launchpad is a spring, not a linear step: it overshoots a touch on the
    // way in and settles decisively on the way out (see stepStartSpring).
    stepStartSpring(dt, now);
    step(taskViewAnim, taskViewOpen ? 1.0 : 0.0, 170.0);
    step(altTabAnim, altTabOpen ? 1.0 : 0.0, 120.0);
    // Control Centre: a slightly longer ease so the grid reads as rising out
    // of the taskbar rather than simply appearing.
    step(ccAnim, ccOpen ? 1.0 : 0.0, 210.0);
    // Tablet / mobile mode: the splash fades in, the two shells are swapped
    // while it fully covers the screen, and it fades back out onto the new one.
    if (modeSwitching) {
        const double p = clamp01((now - modeSwitchStart) / double(metrics::kTabletSplashMs));
        if (p < 0.40) splashOpacity = p / 0.40;
        else if (p < 0.60) splashOpacity = 1.0;
        else splashOpacity = std::max(0.0, 1.0 - (p - 0.60) / 0.40);
        if (!modeSwapped && p >= 0.5) {
            modeSwapped = true;
            tabletMode = modeSwitchTarget;
            applyTabletMode();
        }
        if (p >= 1.0) {
            modeSwitching = false;
            tabletMode = modeSwitchTarget;
            splashOpacity = 0.0;
        }
        dirty = true;
    }
    // Cross-fades the home screen (and the taskbar out) around the swap point.
    step(tabletAnim, tabletMode ? 1.0 : 0.0, 300.0);
    // Rearrange mode jiggles the icons, so it needs a frame every tick.
    if (tabletEdit && tabletMode) dirty = true;
    // iPhone X home bar: a swipe that pauses on the way up opens the app
    // switcher. No motion event reports the pause (the finger is not moving),
    // so the dwell is detected here.
    if (tabletGesture && !tabletGestureSwipe && !tabletSwitcher &&
        tabletGestureStartY - tabletGestureCurY > 40 &&
        now - tabletGestureLastMove > 320.0) {
        openTabletSwitcher();
    }
    step(tabletSwitcherAnim, tabletSwitcher ? 1.0 : 0.0, 220.0);
    // Always drain an in-flight probe, even with the panel closed, so a probe
    // that was running when it closed cannot leak its pipe. Only a live panel
    // asks for the periodic refresh.
    if (sysctl.poll() && ccOpen) dirty = true;
    if (ccOpen) {
        // Keep the reading fresh so an external change (a keyboard volume key,
        // NetworkManager) shows up while the panel is visible.
        sysctl.requestRefresh(4.0);
        if (!ccControls.empty() && ccAnim < 1.0) layoutControlCenter();
    } else if (ccAnim <= 0.0 && !ccControls.empty()) {
        ccControls.clear();
    }

    // Desktop icons: glide toward the cells the grid wants whenever a widget has
    // reflowed the layout (added, removed, dragged or resized).
    animateDesktopIconReflow(dtMs);

    // Launchpad tiles: the same spring reflow, while a reorder drag (or the
    // settle after one) is on screen.
    if (startOpen) animateLaunchTileReflow(dtMs);

    // App launch placeholders (desktop icon growing into the app's window).
    tickLaunches(now, dtMs);

    // Everything else the shell shows: hover washes, selections, flyouts and the
    // snap preview all ease rather than toggle.
    tickFluidMotion(dtMs);

    // Widgets: repaint the analog clock when the wall second changes, and
    // re-probe the battery every few seconds. The tablet status bar shows the
    // clock too, so the second tick stays alive even without any widgets.
    if (!widgets.empty() || tabletMode) {
        const time_t sec = time(nullptr);
        if (sec != lastClockSecond) {
            lastClockSecond = sec;
            dirty = true;
        }
        if (now - lastBatteryProbe > 5000.0) {
            lastBatteryProbe = now;
            refreshBattery(false);
            refreshWeather(false);
        }
    }
    if (opts->stats) dirty = true;
}

// Ambient motion. Everything the shell used to snap between states -- a hover
// wash, a selection highlight, a flyout, the caption buttons, the snap preview
// -- is eased here, so the entire OS moves with one continuous feel. Each value
// is delta-time paced and reversible mid-flight, which is what keeps it fluid
// even when the pointer darts around.
void Manager::tickFluidMotion(double dtMs) {
    if (dtMs <= 0.0) return;
    constexpr double kHoverMs = 120.0;   // per-item hover / selection fade
    // The running pill is the one thing on the bar that springs rather than fades,
    // so it gets its own, slightly slower budget.
    constexpr double kPillMs = 190.0;
    constexpr double kFlyoutMs = 140.0;  // dropdown open / close
    constexpr double kButtonMs = 90.0;   // caption-button hover (a touch snappier)

    // Ease every entry of a per-item fade toward 1 for the hot index and 0 for
    // the rest, growing or shrinking the vector to `n` entries.
    const auto fade = [&](std::vector<double>& v, size_t n, int hot) {
        if (v.size() != n) v.resize(n, 0.0);
        for (size_t i = 0; i < n; ++i) {
            if (approach(v[i], int(i) == hot ? 1.0 : 0.0, dtMs, kHoverMs)) dirty = true;
        }
    };

    // Taskbar: each app button, the Start button and the show-desktop sliver.
    fade(taskHover, taskItems.size(), hoverTaskIndex);
    if (approach(startHoverAnim, hoverStart ? 1.0 : 0.0, dtMs, kHoverMs)) dirty = true;
    if (approach(showDesktopHoverAnim, hoverShowDesktop ? 1.0 : 0.0, dtMs, kHoverMs))
        dirty = true;
    if (approach(clockHoverAnim, hoverClock ? 1.0 : 0.0, dtMs, kHoverMs)) dirty = true;
    if (approach(circleHoverAnim, hoverCircle ? 1.0 : 0.0, dtMs, kHoverMs)) dirty = true;
    // The ring-button motes keep drifting while the pointer is on the button, so
    // hold a repaint open until the hover (and its particles) have faded out.
    if (circleHoverAnim > 0.001) dirty = true;
    // The running pill springs between its three widths instead of snapping. It
    // tracks the same eased hover the icon lift does, so widening on hover and the
    // icon growing arrive together rather than in two separate jumps.
    if (taskPill.size() != taskItems.size()) {
        // Resize, not assign: an existing pill keeps the width it had, so opening
        // or closing a window does not make every other pill blink back to rest.
        taskPill.resize(taskItems.size(), 0.0);
        dirty = true;
    }
    for (size_t i = 0; i < taskItems.size(); ++i) {
        const TaskItem& it = taskItems[i];
        double target = 0.0;
        if (it.pin >= 0 && it.pin < int(pinned.size())) {
            // A pinned launcher with nothing running keeps no pill at all.
            if (Client* win = clientForPinned(pinned[size_t(it.pin)]))
                target = (win == focused && !win->minimized) ? 1.0 : 0.42;
        } else if (Client* c = it.client) {
            target = (c == focused && !c->minimized) ? 1.0 : 0.42;
        }
        if (target > 0.0 && taskHover[i] > target) target = taskHover[i];
        if (approach(taskPill[i], target, dtMs, kPillMs)) dirty = true;
    }
    // A button that has just appeared -- a new window opened, or an app pinned --
    // eases up out of the bar instead of blinking into place. New slots start at 0
    // and the vector grows with the bar, so only the new button animates.
    if (taskAppear.size() != taskItems.size()) {
        // The first layout fills the vector in at rest, so the bar does not pop
        // on start up. Every later growth is a genuinely new button, which
        // starts from 0 and eases in.
        taskAppear.resize(taskItems.size(), taskAppearPrimed ? 0.0 : 1.0);
        taskAppearPrimed = true;
        dirty = true;
    }
    for (size_t i = 0; i < taskAppear.size(); ++i) {
        if (approach(taskAppear[i], 1.0, dtMs, kPillMs)) dirty = true;
    }
    // The press pop: set to 1 when a button is clicked, and it relaxes back to
    // rest on its own. Snappier than a hover so the click reads as instant.
    if (taskPress.size() != taskItems.size()) {
        taskPress.resize(taskItems.size(), 0.0);
        dirty = true;
    }
    for (size_t i = 0; i < taskPress.size(); ++i) {
        if (approach(taskPress[i], 0.0, dtMs, 70.0)) dirty = true;
    }
    // Dock magnification, following Plank (PositionManager::update_draw_values):
    //
    //   radius = iconSize * (1 + peak)            Plank's ZoomIconSize
    //   p      = min(|x - cx|, radius) / radius   normalised distance to the icon
    //   u      = 1 - p^2                          Plank's zoom curve
    //   scale  = 1 + peak * u * gate              gate = Plank's zoom_in_progress
    //   push   = min(|x - cx|, radius) * peak * (1 - p/3) * gate
    //   shift  = pushed *away* from the pointer, so the row opens around it
    //
    // The curve has finite support -- it is exactly 1 at the radius -- so the row
    // truly comes to rest. The earlier Gaussian never reached 1 and left the whole
    // row subtly breathing under the cursor. The icons also *translate* apart,
    // which is the half of Plank's effect that makes the swell read as one smooth
    // wave instead of a lone icon popping out of a static row.
    {
        constexpr double kPeak = 0.28;   // peak growth at the hovered icon
        const double icon = double(metrics::taskIconSize());
        const double radius = std::max(1.0, icon * (1.0 + kPeak));
        const double dtSec = std::min(dtMs, 50.0) / 1000.0;

        // One spring for the whole field, exactly Plank's zoom_in_progress: it
        // eases in when the pointer reaches the bar and back out when it leaves,
        // so entering and leaving swells the row instead of snapping it.
        const double gateTarget = pointerOnTaskbar ? 1.0 : 0.0;
        dockZoomSpring.step(gateTarget, dtSec);
        if (!dockZoomSpring.settled(gateTarget)) dirty = true;
        const double gate = motion::clamp(dockZoomSpring.value, 0.0, 1.0);

        // Lay the field over the row: item 0 is the Start button, then the task
        // buttons. The distance is horizontal -- a dock row only cares how far
        // along it the pointer is, not how high up the bar it sits.
        const size_t n = taskItems.size();
        if (taskScale.size() != n) taskScale.resize(n, 1.0);
        if (taskShift.size() != n) taskShift.resize(n, 0);

        const auto place = [&](double cx, double& scale, int& shift) {
            const double delta = double(pointerX) - cx;
            const double d = std::min(std::abs(delta), radius);
            const double p = d / radius;
            const double u = 1.0 - p * p;               // 1 at the icon, 0 at radius
            const double ns = 1.0 + kPeak * u * gate;
            const double push = d * kPeak * (1.0 - p / 3.0) * gate;
            const int nsh = int(std::lround(
                delta > 0.0 ? -push : (delta < 0.0 ? push : 0.0)));
            if (std::abs(ns - scale) > 1e-4 || nsh != shift) dirty = true;
            scale = ns;
            shift = nsh;
        };

        place(startButtonRect.x + startButtonRect.w / 2.0, startScale, startShift);
        for (size_t i = 0; i < n; ++i) {
            place(taskItems[i].rect.x + taskItems[i].rect.w / 2.0, taskScale[i],
                  taskShift[i]);
        }
    }
    // The button being carried eases up out of the bar and settles back on drop.
    if (approach(pinDragLift, pinDragMoved ? 1.0 : 0.0, dtMs, kHoverMs)) dirty = true;

    // Launchpad tiles (only while the overlay is on screen) and its page dots.
    fade(appHover, startOpen ? appRects.size() : 0, startOpen ? hoverApp : -1);
    {
        const size_t n = startOpen ? appDotRects.size() : 0;
        if (dotHover.size() != n) dotHover.resize(n, 0.0);
        for (size_t i = 0; i < n; ++i) {
            if (approach(dotHover[i], int(i) == startHoverDot ? 1.0 : 0.0, dtMs, kHoverMs))
                dirty = true;
        }
    }

    // Context-menu items (the panel itself eases open/closed just below). The
    // ring menu hovers its grid cells, recent-file rows and power buttons in one
    // index space.
    const size_t hoverCount = contextRing
                                  ? contextRecents.size() + size_t(ringFilesShown) +
                                        ringPowerRects.size()
                                  : contextItems.size();
    fade(ctxHover, hoverCount, contextHover);

    // Desktop icons: the hover and the selection share one fade, so sliding the
    // selection from one icon to the next cross-fades instead of blinking.
    {
        const size_t n = desktopItems.size();
        if (desktopIconHover.size() != n) desktopIconHover.resize(n, 0.0);
        for (size_t i = 0; i < n; ++i) {
            const bool hot = int(i) == hoverDesktopIcon || int(i) == selectedDesktopIcon;
            if (approach(desktopIconHover[i], hot ? 1.0 : 0.0, dtMs, kHoverMs)) dirty = true;
        }
    }

    // Desktop widgets.
    fade(widgetHover, widgets.size(), hoverWidget);

    // Control Centre controls (only while the panel is on screen).
    fade(ccHoverFade, ccOpen ? ccControls.size() : 0, ccOpen ? ccHover : -1);

    // Tablet home / dock icons (only while the home screen is up). The vector spans
    // every icon the grid holds, not just the page on screen, because a hover index
    // is an icon's own index and the icons keep theirs across a page turn.
    fade(tabletIconHover,
         tabletMode ? tabletHome.size() + tabletDockRects.size() : 0,
         tabletMode ? tabletHover : -1);

    // The dock's spring system -- magnification, the rubber-band parting while an
    // icon is carried over the row, and the bounce of a drop or a launch. It is
    // the home screen's own motion, so it only runs while the home screen is up.
    if (tabletMode) updateTabletDockPhysics(dtMs);

    // The grid settling onto a page once a swipe has let go. The offset is in pages
    // rather than pixels so that a page caught halfway can be drawn halfway.
    if (tabletMode && !tabletPageSwipe && tabletHomePageCount > 0) {
        const double goal = double(tabletHomePage);
        if (std::abs(tabletPageOffset - goal) > 0.001) {
            tabletPageOffset +=
                (goal - tabletPageOffset) * std::min(1.0, dtMs / double(metrics::kTabletPageTurnMs));
            dirty = true;
        } else {
            tabletPageOffset = goal;
        }
    }

    // The Launchpad grid settling onto the page a swipe committed to. As with the
    // tablet home screen the offset is in pages, so the page the finger dropped
    // halfway keeps sliding the rest of the way instead of snapping. Programmatic
    // page changes set launchPageOffset to startPage themselves, so this only runs
    // for the tail of a swipe.
    if (startOpen && !launchSwipe) {
        const double goal = double(startPage);
        if (std::abs(launchPageOffset - goal) > 0.001) {
            launchPageOffset +=
                (goal - launchPageOffset) * std::min(1.0, dtMs / double(metrics::kLaunchPageTurnMs));
            dirty = true;
        } else {
            launchPageOffset = goal;
        }
    }

    // The quick actions sheet is modal too, so its rows light up under the pointer
    // even where a widget card is drawn behind the panel.
    if (tabletMenu) {
        int hot = -1;
        for (size_t i = 0; i < tabletMenuRows.size(); ++i)
            if (tabletMenuRows[i].inflated(2).contains(pointerX, pointerY)) hot = int(i);
        if (tabletMenuItem != hot) {
            tabletMenuItem = hot;
            dirty = true;
        }
    }

    // The dock picker is modal, so it is the thing under the pointer even where a
    // widget card is drawn behind it. -2 is the Done button, which lights up only
    // when there is something to commit.
    if (tabletDockPickerOpen) {
        int hot = -1;
        for (size_t i = 0; i < tabletDockPickerRects.size(); ++i)
            if (tabletDockPickerRects[i].inflated(6).contains(pointerX, pointerY))
                hot = int(i);
        if (tabletDockPickerDone.contains(pointerX, pointerY)) hot = -2;
        if (tabletDockPickerHover != hot) {
            tabletDockPickerHover = hot;
            dirty = true;
        }
    }

    // Caption buttons on every managed window.
    for (auto& cp : clients) {
        Client* c = cp.get();
        for (int b = 0; b < 3; ++b) {
            if (approach(c->hoverFade[b], c->hoverBtn == b ? 1.0 : 0.0, dtMs, kButtonMs))
                dirty = true;
        }
    }

    // The dropdown panel opens and closes with a short fade + slide. Its stale
    // contents are dropped only once the close animation has fully played out.
    // The ring menu is not a dropdown -- it pops out of its button the way Control
    // Centre pops out of the clock, so it runs that surface's 210ms instead of the
    // flyout's 140ms (see step(ccAnim, ...) in the tick). contextRing survives the
    // close animation, so this paces the way out as well as the way in.
    constexpr double kRingMenuMs = 210.0;  // Control Centre's open/close duration
    if (approach(contextAnim, contextOpen ? 1.0 : 0.0, dtMs,
                 contextRing ? kRingMenuMs : kFlyoutMs))
        dirty = true;
    if (!contextOpen && contextAnim <= 0.0 && (!contextItems.empty() || contextRing)) {
        contextItems.clear();
        contextClient = nullptr;
        contextWidget = -1;
        contextDesktop = -1;
        contextHover = -1;
        // The ring menu's cells go the same way, and its mode flag with them, so
        // the next flyout starts from a clean slate.
        contextRing = false;
        contextRecents.clear();
        ringRecentRects.clear();
        ringPowerRects.clear();
        ringGridH = 0;
    }

    // The snap preview catches the eye as it arms, then fades away when released.
    if (approach(snapPreviewAnim, snapZonePreview != kSnapNone ? 1.0 : 0.0, dtMs, kHoverMs))
        dirty = true;
}

namespace {
// Edge bitmask (1 left, 2 right, 4 top, 8 bottom) -> cursor kind.
int edgeCursorKind(int edge) {
    if (edge == 0) return 0;
    if (edge == (1 | 2)) return 1;  // horizontal
    if (edge == (4 | 8)) return 2;  // vertical
    if (((edge & 1) && (edge & 4)) || ((edge & 2) && (edge & 8))) return 3;  // diagonal
    return 4;                                                               // anti-diagonal
}
}  // namespace

void Manager::handleEvent(XEvent& ev) {
    if (damageOk && ev.type == damageEventBase + XDamageNotify) {
        XDamageNotifyEvent* d = reinterpret_cast<XDamageNotifyEvent*>(&ev);
        onDamage(d->damage, d->drawable);
        return;
    }
    switch (ev.type) {
        case MapRequest:
            onMapRequest(ev.xmaprequest);
            break;
        case ConfigureRequest:
            onConfigureRequest(ev.xconfigurerequest);
            break;
        case ConfigureNotify:
            onConfigureNotify(ev.xconfigure);
            break;
        case MapNotify: {
            Client* c = find(ev.xmap.window);
            if (c) {
                c->mapped = true;
                c->needsRepaint = true;
                dirty = true;
            }
            break;
        }
        case UnmapNotify:
            onUnmapNotify(ev.xunmap);
            break;
        case DestroyNotify: {
            Client* c = find(ev.xdestroywindow.window);
            if (c) {
                c->alive = false;
                if (dragClient == c) cancelDrag();
                removeClient(c);
            }
            dirty = true;
            break;
        }
        case PropertyNotify:
            onPropertyNotify(ev.xproperty);
            break;
        case ClientMessage:
            onClientMessage(ev.xclient);
            break;
        case Expose:
            if (ev.xexpose.window == comp.overlay()) dirty = true;
            break;
        case EnterNotify:
        case LeaveNotify:
            onCrossing(ev.xcrossing);
            break;
        case MotionNotify:
            onMotion(ev.xmotion);
            break;
        case ButtonPress:
            onButtonPress(ev.xbutton);
            break;
        case ButtonRelease:
            onButtonRelease(ev.xbutton);
            break;
        case KeyPress:
            onKeyPress(ev.xkey);
            break;
        case KeyRelease: {
            // Skip auto-repeat: X sends press/release pairs with equal timestamps.
            if (XPending(dpy) > 0) {
                XEvent next;
                XPeekEvent(dpy, &next);
                if (next.type == KeyPress && next.xkey.time == ev.xkey.time &&
                    next.xkey.keycode == ev.xkey.keycode) {
                    break;
                }
            }
            const KeySym sym = XLookupKeysym(&ev.xkey, 0);
            if (altTabOpen && (sym == XK_Alt_L || sym == XK_Alt_R)) {
                // Windows commits the Alt-Tab choice when Alt is released.
                if (altTabIndex >= 0 && altTabIndex < int(altTabOrder.size())) {
                    Client* target = altTabOrder[size_t(altTabIndex)];
                    focusClient(target, true);
                    restoreClient(target);
                }
                closeOverlays();
            }
            break;
        }
        case MappingNotify:
            XRefreshKeyboardMapping(&ev.xmapping);
            break;
        default:
            break;
    }
}

// Recomputed on every pointer motion over our chrome: taskbar hover, caption
// button hover, resize cursors. Anything that changes marks the frame dirty.
void Manager::updateHoverStates(int px, int py) {
    bool changed = false;

    // Tablet / mobile mode has its own hover surfaces: the widgets, the home and
    // dock icons and the Control Centre panel. None of the desktop chrome --
    // taskbar, captions, resize edges, desktop icons -- exists here.
    if (tabletMode) {
        pointerOnTaskbar = false;
        const bool blocked = overlayOpen();
        const int newWidget = blocked ? -1 : widgetAt(px, py);
        if (newWidget != hoverWidget) {
            hoverWidget = newWidget;
            changed = true;
        }
        int newIcon = -1;
        if (!blocked) {
            for (size_t i = 0; i < tabletHomeRects.size() && i < tabletHome.size(); ++i) {
                if (tabletHomeRects[i].contains(px, py)) {
                    newIcon = int(i);
                    break;
                }
            }
            if (newIcon < 0) {
                for (size_t i = 0; i < tabletDockRects.size() && i < tabletDock.size(); ++i) {
                    if (tabletDockRects[i].contains(px, py)) {
                        newIcon = int(tabletHome.size() + i);
                        break;
                    }
                }
            }
        }
        if (newIcon != tabletHover) {
            tabletHover = newIcon;
            changed = true;
        }
        if (ccOpen) updateCcHover(px, py);
        const int wanted = (newWidget >= 0 || newIcon >= 0 || (ccOpen && ccHover >= 0)) ? 5 : 0;
        if (wanted != cursorShown) setCursor(wanted);
        if (changed) dirty = true;
        return;
    }

    const int taskbarTop = screenH - metrics::taskbarH;
    // A magnified icon grows up out of the bar, so the field must not collapse the
    // instant the cursor follows it off the top edge. The band reaches a little
    // above the bar; the per-button hover tests below still use the real rects, so
    // the extra band never lights a button the pointer is not actually on.
    const int dockReach = metrics::taskIconSize() / 2 + 8;
    const bool overTaskbar = py >= taskbarTop - dockReach;
    // The magnification field relaxes as soon as this goes false, including on a
    // LeaveNotify, which sends no motion event of its own.
    pointerOnTaskbar = overTaskbar;

    bool newStart = false, newShowDesktop = false, newClock = false, newCircle = false;
    int newTask = -1;
    if (overTaskbar) {
        newStart = startButtonRect.contains(px, py);
        if (!startOpen) {
            newCircle = circleButtonRect.contains(px, py);
            newShowDesktop = showDesktopRect.contains(px, py);
            newClock = clockRect.contains(px, py);
            for (size_t i = 0; i < taskItems.size(); ++i) {
                if (taskHitRect(i).contains(px, py)) {
                    newTask = int(i);
                    break;
                }
            }
        }
    }
    if (newStart != hoverStart || newShowDesktop != hoverShowDesktop ||
        newClock != hoverClock || newCircle != hoverCircle || newTask != hoverTaskIndex) {
        hoverStart = newStart;
        hoverShowDesktop = newShowDesktop;
        hoverClock = newClock;
        hoverCircle = newCircle;
        hoverTaskIndex = newTask;
        changed = true;
    }

    if (startOpen) {
        // While a page is being turned the tiles are sliding, so the rects
        // appRects holds (the page startPage names) are not where anything is
        // drawn: light nothing until the field has settled.
        const bool turning =
            launchSwipe || std::abs(launchPageOffset - double(startPage)) > 0.001;
        int newApp = -1;
        if (!turning) {
            for (size_t i = 0; i < appRects.size(); ++i) {
                if (appRects[i].contains(px, py)) {
                    newApp = int(i);
                    break;
                }
            }
        }
        if (newApp != hoverApp) {
            hoverApp = newApp;
            changed = true;
        }
        int newDot = -1;
        if (newApp < 0 && !turning) {
            for (size_t p = 0; p < appDotRects.size(); ++p) {
                if (appDotRects[p].contains(px, py)) {
                    newDot = int(p);
                    break;
                }
            }
        }
        if (newDot != startHoverDot) {
            startHoverDot = newDot;
            changed = true;
        }
    }

    if (contextOpen) {
        // Context-menu item hover, so the row under the pointer lights up (and,
        // with the eased ctxHover, fades back out as the pointer leaves it).
        const int newCtx = contextRowAt(px, py);
        if (newCtx != contextHover) {
            contextHover = newCtx;
            changed = true;
        }
    }

    if (ccOpen) {
        const int before = ccHover;
        updateCcHover(px, py);
        if (ccHover != before) changed = true;
    }

    int edge = 0;
    Client* edgeClient = nullptr;
    if (!overlayOpen()) edge = hitEdge(px, py, &edgeClient);
    if (edge != hoverEdge || edgeClient != hoverResizeClient) {
        hoverEdge = edge;
        hoverResizeClient = edgeClient;
        changed = true;
    }
    int wantedCursor = edge ? edgeCursorKind(edge) : (overTaskbar ? 5 : 0);
    // A launcher being carried is a horizontal drag, so it shows the horizontal
    // resize arrow wherever the pointer wanders to.
    if (pinDragMoved) wantedCursor = 1;
    // A Control Centre control is a button, so it gets the hand cursor.
    if (ccOpen && ccHover >= 0 && size_t(ccHover) < ccControls.size()) wantedCursor = 5;
    // Launchpad tiles and page dots are clickable too.
    if (startOpen && (hoverApp >= 0 || startHoverDot >= 0)) wantedCursor = 5;
    // The taskbar's edge resizes the bar, so it gets the up/down arrow. Last, because
    // it has to hold even with an overlay open, and because a drag in flight has to
    // keep its arrow once the pointer has wandered off the edge.
    if (taskbarResizeY >= 0 || taskbarGripAt(px, py)) wantedCursor = 2;
    if (wantedCursor != cursorShown) setCursor(wantedCursor);

    // Caption buttons: the top-most window whose frame covers the point wins.
    Client* owner = nullptr;
    int button = -1;
    if (!overlayOpen()) {
        for (size_t i = clients.size(); i-- > 0;) {
            Client* c = clients[i].get();
            if (!c->mapped || !c->managed || c->minimized || c->fullscreen) continue;
            if (!c->frame.contains(px, py)) continue;
            owner = c;
            button = hitCaptionButton(c, px, py);
            break;
        }
    }
    for (auto& cp : clients) {
        Client* c = cp.get();
        const int value = (c == owner) ? button : -1;
        if (c->hoverBtn != value) {
            c->hoverBtn = value;
            changed = true;
        }
    }

    // Desktop icons highlight only over bare wallpaper: an icon covered by a
    // window, the taskbar or an open flyout must never light up.
    int newDesktop = -1;
    if (!overlayOpen() && !overTaskbar && !dragClient && !clientAt(px, py)) {
        // Hit test the *shown* cells so a hover follows an icon mid-glide.
        const std::vector<Rect>& hit =
            desktopIconDraw.size() == desktopIconRects.size() ? desktopIconDraw : desktopIconRects;
        for (size_t i = 0; i < hit.size(); ++i) {
            if (hit[i].contains(px, py)) {
                newDesktop = int(i);
                break;
            }
        }
    }
    if (newDesktop != hoverDesktopIcon) {
        hoverDesktopIcon = newDesktop;
        changed = true;
    }

    // Widgets: a hand over the card, the diagonal resize arrow over its grip.
    int newWidget = -1;
    bool overGrip = false;
    if (!overlayOpen() && !overTaskbar && !dragClient && !clientAt(px, py)) {
        newWidget = widgetAt(px, py);
        if (newWidget >= 0) overGrip = widgetGripRect(widgets[newWidget]).contains(px, py);
    }
    if (newWidget != hoverWidget) {
        hoverWidget = newWidget;
        changed = true;
    }
    if (dragWidget >= 0)
        wantedCursor = widgetResizing ? 3 : 5;
    else if (newWidget >= 0)
        wantedCursor = overGrip ? 3 : 5;
    // A desktop icon reads as draggable under the pointer, and keeps the hand
    // while it is being carried.
    if (dragDesktopIcon >= 0 || newDesktop >= 0) wantedCursor = 5;
    if (wantedCursor != cursorShown) setCursor(wantedCursor);

    if (changed) dirty = true;
}

bool Manager::overlayOpen() const {
    // The desktop's folder dialogs count: while one is up the desktop underneath
    // must not light up under the pointer or offer resize edges, any more than the
    // wallpaper does behind the Control Centre.
    return startOpen || taskViewOpen || altTabOpen || contextOpen || ccOpen || tabletSwitcher ||
           confirmDeleteOpen || desktopRenameItem >= 0;
}

bool Manager::pointInOverlaySurface(int x, int y) const {
    if (startOpen && startRect.inflated(12).contains(x, y)) return true;
    if (taskViewOpen) return true;
    if (altTabOpen) return true;
    if (contextOpen && contextRect.contains(x, y)) return true;
    if (ccOpen && ccRect.inflated(12).contains(x, y)) return true;
    return false;
}

void Manager::closeOverlays() {
    const bool was = overlayOpen();
    const bool wasCc = ccOpen;
    startOpen = taskViewOpen = altTabOpen = contextOpen = ccOpen = false;
    // The desktop's folder dialogs are overlays too and go the same way: anything
    // that would take the desktop away has answered them. They hand back the
    // grabs they took as they close.
    closeConfirmDelete();
    cancelDesktopRename();
    closeTabletSwitcher();
    altTabOrder.clear();
    altTabIndex = 0;
    searchText.clear();
    hoverApp = -1;
    startHoverDot = -1;
    startPage = 0;
    launchPageOffset = 0.0;
    launchSwipe = false;
    launchPressArmed = false;
    launchPressTile = -1;
    // A tile reorder ends with the launchpad: the drag state belongs to the
    // overlay, and the springs are re-synced from scratch the next time it opens.
    launchDragMoving = false;
    launchDragTile = -1;
    launchDragFrom = -1;
    launchTilePage = -1;
    launchTileTotal = 0;
    // The menu's items are kept until its close animation finishes (tickFluid
    // Motion drops them); clearing the hover lets the highlight fade out with it.
    contextHover = -1;
    ccHover = -1;
    if (wasCc) endCcDrag();
    if (was) {
        ungrabPointer();
        XUngrabKeyboard(dpy, CurrentTime);
    }
    dirty = true;
}

void Manager::updateClientList() {
    std::vector<Window> list;
    for (const auto& c : clients) {
        if (c->managed) list.push_back(c->id);
    }
    if (list.empty()) {
        XDeleteProperty(dpy, root, A.netClientList);
        XDeleteProperty(dpy, root, A.netClientListStacking);
        return;
    }
    // _NET_CLIENT_LIST wants map order and _NET_CLIENT_LIST_STACKING wants
    // bottom-to-top order; `clients` is already bottom-to-top, and pagers
    // tolerate the same list for both.
    XChangeProperty(dpy, root, A.netClientList, XA_WINDOW, 32, PropModeReplace,
                    reinterpret_cast<unsigned char*>(list.data()), int(list.size()));
    XChangeProperty(dpy, root, A.netClientListStacking, XA_WINDOW, 32, PropModeReplace,
                    reinterpret_cast<unsigned char*>(list.data()), int(list.size()));
}

Rect Manager::workArea() const {
    Rect wa{0, 0, screenW, screenH};
    if (screenH > metrics::taskbarH * 2) wa.h -= metrics::taskbarH;  // our taskbar
    for (const auto& c : clients) {
        if (!c->isDock || !c->mapped) continue;
        if (c->strut[2] > 0) {
            wa.y += int(c->strut[2]);
            wa.h -= int(c->strut[2]);
        }
        if (c->strut[3] > 0) wa.h -= int(c->strut[3]);
        if (c->strut[0] > 0) {
            wa.x += int(c->strut[0]);
            wa.w -= int(c->strut[0]);
        }
        if (c->strut[1] > 0) wa.w -= int(c->strut[1]);
    }
    if (wa.w < 1 || wa.h < 1) return Rect{0, 0, screenW, screenH};
    return wa;
}

void Manager::updateWorkArea() {
    const Rect wa = workArea();
    const long data[4] = {wa.x, wa.y, wa.w, wa.h};
    XChangeProperty(dpy, root, A.netWorkarea, XA_CARDINAL, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(data), 4);
}

// Where a snapped window lands. Windows 11 leaves a small gap between snapped
// halves and never lets a window cover the taskbar.
Rect Manager::snapGeometry(int zone) const {
    const Rect wa = workArea();
    const int g = metrics::kSnapGap;
    const int halfW = (wa.w - g) / 2;
    const int halfH = (wa.h - g) / 2;
    switch (zone) {
        case kSnapLeft:
            return Rect{wa.x, wa.y, halfW, wa.h};
        case kSnapRight:
            return Rect{wa.right() - halfW, wa.y, halfW, wa.h};
        case kSnapTop:
            return wa;
        case kSnapBottom:
            return Rect{wa.x, wa.y + halfH, wa.w, wa.h - halfH};
        case kSnapTopLeft:
            return Rect{wa.x, wa.y, halfW, halfH};
        case kSnapTopRight:
            return Rect{wa.right() - halfW, wa.y, halfW, halfH};
        case kSnapBottomLeft:
            return Rect{wa.x, wa.bottom() - halfH, halfW, halfH};
        case kSnapBottomRight:
            return Rect{wa.right() - halfW, wa.bottom() - halfH, halfW, halfH};
        default:
            return wa;
    }
}

Rect Manager::clientRect(const Client* c) const {
    const int capH = c->captionH;
    const int b = c->managed ? metrics::kBorder : 0;
    Rect r{c->frame.x + b, c->frame.y + capH, c->frame.w - 2 * b, c->frame.h - capH - b};
    if (r.w < 1) r.w = 1;
    if (r.h < 1) r.h = 1;
    return r;
}

// Pushes the frame geometry down to the actual X window: this is the only
// place a managed window is ever moved or resized.
void Manager::syncClientGeometry(Client* c) {
    if (!c || !c->alive || !c->id) return;
    if (c->managed) {
        // _NET_FRAME_EXTENTS tells toolkits how much decoration we add, which
        // is what makes GTK/Qt client side decorations line up with ours.
        const long extents[4] = {metrics::kBorder, metrics::kBorder, c->captionH,
                                 metrics::kBorder};
        XChangeProperty(dpy, c->id, A.netFrameExtents, XA_CARDINAL, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(extents), 4);
        XChangeProperty(dpy, c->id, A.gtkFrameExtents, XA_CARDINAL, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(extents), 4);
    }
    if (!c->mapped) return;
    const Rect r = clientRect(c);
    XMoveResizeWindow(dpy, c->id, r.x, r.y, unsigned(r.w), unsigned(r.h));
    c->needsRepaint = true;
    c->pixW = 0;  // force the pixmap to be re-named at the new size
    c->pixH = 0;
}

namespace {
// One shared (omega, zeta) for every window transition. zeta = 1 is critically
// damped -- the fastest arrival with no wobble -- and omega ~= 20 rad/s is the
// "standard" shell preset from the motion design. Using the same pair all over
// is what makes a maximise, a snap and a released drag feel like one material.
constexpr double kGeometryHz = 3.2;  // omega = 2 pi f ~= 20 rad/s
}  // namespace

// Prime the springs from the drawn frame and drop them onto `frame` at rest.
void Manager::settleGeometry(Client* c) {
    if (!c) return;
    // (f, zeta) rather than a raw stiffness: the whole geometry system moves at
    // one response frequency so a maximise, a snap and a released drag all read
    // as the same material.
    c->geo.configure(kGeometryHz, 1.0);
    c->geo.reset(motion::Box{double(c->frame.x), double(c->frame.y), double(c->frame.w),
                             double(c->frame.h)});
    c->drawFrame = c->frame;
    c->geoLive = true;
    c->geoSpring = false;
}

// One frame of the geometry system toward the current `frame`.
void Manager::stepGeometry(Client* c, double dtSec) {
    if (!c) return;
    if (!c->geoLive) settleGeometry(c);
    const Rect t = c->frame;
    const motion::Box g = c->geo.step(
        motion::Box{double(t.x), double(t.y), double(t.w), double(t.h)}, dtSec);
    const Rect next{int(std::lround(g.x)), int(std::lround(g.y)), int(std::lround(g.w)),
                    int(std::lround(g.h))};
    if (next != c->drawFrame) {
        c->drawFrame = next;
        dirty = true;
    }
}

// Applies `frame` (already updated). `animate` routes the change through the
// geometry springs, which keep their velocity so a retarget mid-flight bends the
// trajectory; otherwise the springs are snapped onto the new frame, which is
// what a drag under the pointer needs.
void Manager::applyFrame(Client* c, bool animate) {
    if (!c) return;
    if (!animate) {
        settleGeometry(c);
    } else if (!c->geoLive) {
        settleGeometry(c);
        c->geoSpring = true;
    } else {
        c->geoSpring = true;
    }
    // The open/close/vanish timelines still key off animMs; geometry does not,
    // so this only restarts those fades.
    c->animFrom = c->drawFrame;
    c->animStart = nowMs();
    c->animMs = metrics::kZoomMs;
    dirty = true;
}

void Manager::mapClient(Client* c) {
    if (!c || !c->alive) return;
    readDecorations(c);
    // Redirect before the first map so the client's very first frame goes into
    // an offscreen pixmap instead of straight to the screen.
    if (!c->redirected && !c->isDock && !c->isDesktop) {
        XCompositeRedirectWindow(dpy, c->id, CompositeRedirectManual);
        c->redirected = true;
    }
    // Passive observation of clicks: the client still receives them, we just
    // get a copy, which is how click-to-focus works without reparenting.
    // ButtonRelease is watched too so a caption button cannot get stuck when the
    // pointer is released over the window's own content.
    XSelectInput(dpy, c->id, PropertyChangeMask | ButtonPressMask | ButtonReleaseMask);

    applyFrame(c, false);
    // Map *before* forcing the geometry. syncClientGeometry() is a no-op for
    // unmapped windows, so the order below matters: mapping first means the
    // window is never visible at the position/size the client asked for, which
    // would otherwise cover our caption until the next layout pass.
    if (!c->mapped) {
        XMapWindow(dpy, c->id);
        c->mapped = true;
        c->namedPixmap = 0;
        c->pixW = c->pixH = 0;
    }
    syncClientGeometry(c);
    updateStateAtoms(c);
    c->appear = 0.0;
    c->animMs = metrics::kAnimMs;
    c->animStart = nowMs();
    c->animFrom = c->frame;
    c->needsRepaint = true;
    ensurePixmap(c);
    updateClientList();
    updateFullscreenRedirection();
    dirty = true;
}

void Manager::unmapClient(Client* c) {
    if (!c || !c->mapped) return;
    // Flip our flag first: the UnmapNotify the server sends back is then
    // recognised as ours and ignored.
    c->mapped = false;
    if (c->id) XUnmapWindow(dpy, c->id);
    destroyPixmap(c);
    if (dragClient == c) cancelDrag();
    c->hoverBtn = -1;
    dirty = true;
}

void Manager::raiseClient(Client* c) {
    if (!c || !c->managed) return;
    auto it = std::find_if(clients.begin(), clients.end(),
                           [c](const std::unique_ptr<Client>& p) { return p.get() == c; });
    if (it == clients.end()) return;
    if (it + 1 != clients.end()) {
        std::unique_ptr<Client> keep = std::move(*it);
        clients.erase(it);
        clients.push_back(std::move(keep));
    }
    restack();
    updateClientList();
    dirty = true;
}

// Our overlay is the bottom-most window of the display and the managed clients
// live above it, in `clients` order (bottom .. top). Anything we do not manage
// (menus, tooltips, splashes) keeps its own place in the stack, above us.
void Manager::restack() {
    if (!comp.overlay()) return;

    // XRestackWindows() takes the list *top first*: the first window ends up on
    // top of the group. (The obvious reading, bottom-first, is the trap -- it
    // puts the overlay above every client, so clicks land on the compositor
    // instead of the window under the pointer and mouse input dies session
    // wide.) We therefore build the list top-down, ending with the overlay,
    // which must stay at the very bottom: a click on a window's *content* has
    // to reach that window, while clicks on the chrome we draw around it (the
    // caption strip, the resize band) are not covered by the client and still
    // reach us.
    std::vector<Window> order;
    order.reserve(clients.size() + 2);
    // Fullscreen windows we handed back to the server, then everything we
    // decorate, then desktops; the overlay is appended last.
    for (const Pass pass : {Pass::Top, Pass::Managed, Pass::Background}) {
        for (const auto& c : clients) {
            if (!c->mapped) continue;
            if (pass == Pass::Background && !c->isDesktop) continue;
            if (pass == Pass::Managed && (c->isDesktop || c->isDock || !c->managed)) continue;
            if (pass == Pass::Top && !(c->unredirected && c->managed)) continue;
            if (pass == Pass::Managed && c->unredirected) continue;
            order.push_back(c->id);
        }
    }
    order.push_back(comp.overlay());
    if (order.size() == 1) {
        XLowerWindow(dpy, comp.overlay());
        return;
    }
    XRestackWindows(dpy, order.data(), int(order.size()));
}

void Manager::focusClient(Client* c, bool raise) {
    if (!c || !c->alive || !c->managed) return;
    if (raise) raiseClient(c);
    if (focused == c) return;
    if (focused) {
        focused->focused = false;
        updateStateAtoms(focused);
    }
    focused = c;
    c->focused = true;
    if (c->urgent) {
        c->urgent = false;
        c->attentionPulse = 0.0;
    }
    updateStateAtoms(c);

    // ICCCM: a client whose WM_HINTS `input` is False wants to be *told* that it
    // is focused (WM_TAKE_FOCUS) instead of being given the input focus.
    if (!c->inputHint && c->takeFocus) {
        XEvent ev{};
        ev.xclient.type = ClientMessage;
        ev.xclient.window = c->id;
        ev.xclient.message_type = A.wmProtocols;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = long(A.wmTakeFocus);
        ev.xclient.data.l[1] = CurrentTime;
        XSendEvent(dpy, c->id, False, NoEventMask, &ev);
    } else {
        XSetInputFocus(dpy, c->id, RevertToPointerRoot, CurrentTime);
    }
    const Window active = c->id;
    XChangeProperty(dpy, root, A.netActiveWindow, XA_WINDOW, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&active), 1);
    updateFullscreenRedirection();
    dirty = true;
}

void Manager::focusNext(bool forward) {
    std::vector<Client*> candidates;
    for (auto& c : clients) {
        if (!c->managed || !c->mapped || c->minimized || !c->alive) continue;
        if (c->isDock || c->isDesktop || c->skipTaskbar) continue;
        candidates.push_back(c.get());
    }
    if (candidates.empty()) {
        if (focused) {
            focused->focused = false;
            updateStateAtoms(focused);
        }
        focused = nullptr;
        XSetInputFocus(dpy, PointerRoot, RevertToPointerRoot, CurrentTime);
        XDeleteProperty(dpy, root, A.netActiveWindow);
        dirty = true;
        return;
    }
    Client* next = forward ? candidates.back() : candidates.front();
    if (next == focused && candidates.size() > 1) next = candidates[candidates.size() - 2];
    focusClient(next, true);
}

Client* Manager::activeClient() const { return focused; }

int Manager::openWindows() const {
    int n = 0;
    for (const auto& c : clients) {
        if (c->managed && c->mapped && !c->minimized) ++n;
    }
    return n;
}

void Manager::closeClient(Client* c) {
    if (!c || !c->alive || c->closing) return;
    if (!windowHasAtom(dpy, c->id, A.wmProtocols, A.wmDelete)) {
        // No close protocol: the only way out is to kill the client.
        XKillClient(dpy, c->id);
        return;
    }
    XEvent ev{};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = c->id;
    ev.xclient.message_type = A.wmProtocols;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = long(A.wmDelete);
    ev.xclient.data.l[1] = CurrentTime;
    XSendEvent(dpy, c->id, False, NoEventMask, &ev);
    c->closing = true;
    c->animFrom = c->drawFrame;
    c->animStart = nowMs();
    // A tablet window zooms back into its icon on the way out, which reads
    // better with the slightly longer iOS 26 timing than the Fluent pop.
    c->animMs = c->tabletApp ? metrics::kTabletCloseMs : metrics::kAnimMs;
    c->closeDeadline = nowMs() + 2500.0;
    dirty = true;
}

void Manager::minimizeClient(Client* c, bool animate) {
    if (!c || !c->managed || !c->mapped || c->minimized) return;
    c->minimized = true;
    if (!animate) {
        c->minFade = 1.0;
        unmapClient(c);
    }
    updateStateAtoms(c);
    if (focused == c) {
        focused->focused = false;
        updateStateAtoms(focused);
        focused = nullptr;
        focusNext(true);
    }
    dirty = true;
}

void Manager::restoreClient(Client* c) {
    if (!c || !c->minimized) return;
    c->minimized = false;
    if (!c->mapped) {
        c->namedPixmap = 0;
        c->pixW = c->pixH = 0;
        XMapWindow(dpy, c->id);
        c->mapped = true;
        ensurePixmap(c);
    }
    updateStateAtoms(c);
    dirty = true;
}

void Manager::toggleMaximize(Client* c) {
    if (!c || !c->managed) return;
    const bool both = c->maximizedH && c->maximizedV;
    setMaximized(c, !both, !both);
}

void Manager::setMaximized(Client* c, bool horizontal, bool vertical) {
    if (!c || !c->managed) return;
    if (c->fullscreen) setFullscreen(c, false);
    if (horizontal == c->maximizedH && vertical == c->maximizedV && c->snapZone == kSnapNone) {
        return;
    }
    const bool wasManaged = c->maximizedH || c->maximizedV || c->snapZone != kSnapNone;
    if (!wasManaged) c->restore = c->frame;
    c->snapZone = kSnapNone;
    c->maximizedH = horizontal;
    c->maximizedV = vertical;
    if (!horizontal && !vertical) {
        c->frame = clampRect(c->restore, Rect{0, 0, screenW, screenH});
    } else {
        const Rect wa = workArea();
        Rect f = c->frame;
        if (horizontal) {
            f.x = wa.x;
            f.w = wa.w;
        }
        if (vertical) {
            f.y = wa.y;
            f.h = wa.h;
        }
        c->frame = f;
    }
    applyFrame(c, true);
    syncClientGeometry(c);
    updateStateAtoms(c);
    updateFullscreenRedirection();
    dirty = true;
}

void Manager::setFullscreen(Client* c, bool on) {
    if (!c || !c->managed || c->fullscreen == on) return;
    if (on) {
        if (!c->maximizedH && !c->maximizedV && c->snapZone == kSnapNone) c->restore = c->frame;
        c->fullscreen = true;
        c->captionH = 0;
        c->frame = Rect{0, 0, screenW, screenH};
    } else {
        c->fullscreen = false;
        // A client that asked to stay frameless keeps its own title bar when
        // it comes back out of fullscreen too.
        c->captionH = c->frameless ? 0 : metrics::kCaptionH;
        c->frame = clampRect(c->restore, Rect{0, 0, screenW, screenH});
    }
    applyFrame(c, true);
    syncClientGeometry(c);
    updateStateAtoms(c);
    updateFullscreenRedirection();
    dirty = true;
}

void Manager::toggleFullscreen(Client* c) {
    if (c) setFullscreen(c, !c->fullscreen);
}

void Manager::snapClient(Client* c, int zone) {
    if (!c || !c->managed) return;
    if (c->fullscreen) setFullscreen(c, false);
    if (zone == kSnapNone) {
        if (c->snapZone == kSnapNone && !c->maximizedH && !c->maximizedV) return;
        c->snapZone = kSnapNone;
        c->maximizedH = c->maximizedV = false;
        c->frame = clampRect(c->restore, Rect{0, 0, screenW, screenH});
    } else {
        const bool wasManaged = c->maximizedH || c->maximizedV || c->snapZone != kSnapNone;
        if (!wasManaged) c->restore = c->frame;
        if (zone == kSnapTop) {
            c->maximizedH = c->maximizedV = true;
            c->snapZone = kSnapNone;
        } else {
            c->maximizedH = c->maximizedV = false;
            c->snapZone = zone;
        }
        c->frame = snapGeometry(zone);
    }
    applyFrame(c, true);
    syncClientGeometry(c);
    updateStateAtoms(c);
    updateFullscreenRedirection();
    dirty = true;
}

// New windows are centred, then cascaded a little so a burst of them does not
// land in exactly the same place. Clients that asked for a position get it.
void Manager::placeNewClient(Client* c) {
    if (!c) return;
    const Rect wa = workArea();
    Rect want = c->restore;
    if (want.w < metrics::kMinW) want.w = metrics::kMinW;
    if (want.h < metrics::kMinH) want.h = metrics::kMinH;
    if (want.w > wa.w) want.w = wa.w;
    if (want.h > wa.h) want.h = wa.h;

    int x = want.x, y = want.y;
    if (!c->userPosition || x <= 0 || y <= 0) {
        static int cascade = 0;
        const int shift = (cascade % 6) * 28;
        x = wa.x + (wa.w - want.w) / 2 + shift - 70;
        y = wa.y + (wa.h - want.h) / 2 + shift - 70;
        ++cascade;
    }
    c->frame = clampRect(Rect{x, y, want.w, want.h}, Rect{0, 0, screenW, screenH});
    settleGeometry(c);
    c->restore = c->frame;
    updateStateAtoms(c);
}

void Manager::toggleShowDesktop() {
    showingDesktop = !showingDesktop;
    const long value = showingDesktop ? 1 : 0;
    XChangeProperty(dpy, root, A.netShowingDesktop, XA_CARDINAL, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&value), 1);
    for (auto& cp : clients) {
        Client* c = cp.get();
        // Minimised windows stay in the list: unmapping them must not delete
        // their taskbar button, or there is no way left to restore them.
        if (!c->managed || c->isDock || c->isDesktop || c->skipTaskbar) continue;
        if (c->closing || !c->alive) continue;
        if (showingDesktop) minimizeClient(c, true);
        else if (c->minimized) restoreClient(c);
    }
    dirty = true;
}

void Manager::activateTaskbarItem(Client* c) {
    if (!c) return;
    if (c->minimized) {
        restoreClient(c);
        focusClient(c, true);
    } else if (c->focused) {
        minimizeClient(c, true);
    } else {
        focusClient(c, true);
    }
}

// A pinned launcher is matched to its windows the way the tablet home screen
// matches an icon to the app it launched: the WM_CLASS instance against the
// entry's StartupWMClass, then the entry name against the window class or title.
Client* Manager::clientForPinned(const AppEntry& app) const {
    const auto low = [](std::string s) {
        for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        return s;
    };
    const std::string cls = low(app.wmClass);
    const std::string key = low(app.name);
    if (cls.empty() && key.empty()) return nullptr;
    // Top down: the topmost window of the app is the one a click should raise.
    for (size_t i = clients.size(); i-- > 0;) {
        Client* c = clients[i].get();
        if (!c->managed || !c->alive || c->closing || c->isDock || c->isDesktop) continue;
        if (c->skipTaskbar) continue;
        // appName is already lower-cased when the class was read.
        if (!cls.empty() && c->appName == cls) return c;
        if (!key.empty() && (c->appName == key || low(c->title) == key)) return c;
    }
    return nullptr;
}

bool Manager::isPinned(const AppEntry& app) const {
    return std::any_of(pinned.begin(), pinned.end(),
                       [&](const AppEntry& p) { return p.exec == app.exec; });
}

void Manager::pinApp(const AppEntry& app) {
    if (app.exec.empty() || isPinned(app)) return;
    pinned.push_back(app);
    savePinned(pinned);
    log("pinned \"%s\" to the taskbar", app.name.c_str());
    dirty = true;
}

void Manager::unpinApp(const std::string& exec) {
    const size_t before = pinned.size();
    pinned.erase(std::remove_if(pinned.begin(), pinned.end(),
                                [&](const AppEntry& p) { return p.exec == exec; }),
                 pinned.end());
    if (pinned.size() == before) return;
    savePinned(pinned);
    log("unpinned \"%s\" (%zu pin(s) left)", exec.c_str(), pinned.size());
    dirty = true;
}

// A pin launches its app only when there is nothing to focus, so a second click
// on a running app keeps the ordinary minimise/restore behaviour of a window
// button rather than starting a second copy.
void Manager::activatePinned(int index) {
    if (index < 0 || index >= int(pinned.size())) return;
    const AppEntry app = pinned[size_t(index)];
    if (Client* c = clientForPinned(app)) {
        activateTaskbarItem(c);
        return;
    }
    noteRecent(app.name, app.exec, app.icon, app.wmClass);
    launchApp(app.exec);
}

// One launch remembered. The newest first and deduplicated, so re-launching an
// app moves it to the top rather than adding a second row. The write is
// best-effort: a read-only config directory just makes the list session-only.
void Manager::noteRecent(const std::string& name, const std::string& exec,
                         const std::string& icon, const std::string& wmClass) {
    if (exec.empty()) return;
    recents.erase(std::remove_if(recents.begin(), recents.end(),
                                 [&](const AppEntry& r) { return r.exec == exec; }),
                  recents.end());
    AppEntry e;
    e.name = name.empty() ? exec : name;
    e.exec = exec;
    e.icon = icon;
    e.wmClass = wmClass;
    e.searchKey = e.name;
    for (char& c : e.searchKey) c = char(std::tolower(static_cast<unsigned char>(c)));
    recents.insert(recents.begin(), std::move(e));
    if (recents.size() > size_t(metrics::kRingMaxRecents)) recents.resize(metrics::kRingMaxRecents);
    saveRecents(recents);
}

// One file or folder the desktop opened, remembered the same way a launch is --
// newest first, deduplicated by path, best-effort write.
void Manager::noteRecentFile(const std::string& name, const std::string& path,
                             const std::string& icon, bool isDir) {
    if (path.empty()) return;
    recentFiles.erase(std::remove_if(recentFiles.begin(), recentFiles.end(),
                                     [&](const RecentFile& r) { return r.path == path; }),
                      recentFiles.end());
    RecentFile r;
    r.name = name.empty() ? path : name;
    r.path = path;
    r.icon = icon;
    r.isDir = isDir;
    recentFiles.insert(recentFiles.begin(), std::move(r));
    if (recentFiles.size() > size_t(metrics::kRingMaxRecents))
        recentFiles.resize(metrics::kRingMaxRecents);
    saveRecentFiles(recentFiles);
}

void Manager::activateTaskItem(int index) {
    if (index < 0 || index >= int(taskItems.size())) return;
    const TaskItem& it = taskItems[size_t(index)];
    if (it.pin >= 0) {
        activatePinned(it.pin);
        return;
    }
    activateTaskbarItem(it.client);
}

// The menu behind a right click on a pinned taskbar button or a Launchpad tile:
// one item that pins or unpins, whichever applies to what was clicked.
void Manager::openPinMenu(int appIndex, int pinIndex, int x, int y) {
    const AppEntry* app = nullptr;
    if (pinIndex >= 0 && pinIndex < int(pinned.size())) app = &pinned[size_t(pinIndex)];
    else if (appIndex >= 0 && appIndex < int(apps.size())) app = &apps[size_t(appIndex)];
    if (!app) return;
    // Reached from the Launchpad as well as from the taskbar, so drop whatever
    // flyout is up first: the pointer and keyboard must end up grabbed once, not
    // stacked, which is the same dance openStartMenu does the other way round.
    if (overlayOpen()) closeOverlays();
    contextClient = nullptr;
    contextWidget = -1;
    contextDesktop = -1;
    contextPin = pinIndex;
    contextApp = appIndex;
    contextItems.clear();
    contextItems.push_back(isPinned(*app) ? "Unpin from taskbar" : "Pin to taskbar");

    const int itemH = 32;
    const int width = 200;
    const int height = int(contextItems.size()) * itemH + 12;
    int mx = x, my = y - 6;
    const int bottom = screenH - metrics::taskbarH;
    if (my + height > bottom) my = bottom - height;
    if (mx + width > screenW) mx = screenW - width - 4;
    if (mx < 4) mx = 4;
    if (my < 4) my = 4;
    contextRect = Rect{mx, my, width, height};
    contextOpen = true;
    contextHover = -1;
    startOpen = false;
    grabPointer();
    XGrabKeyboard(dpy, root, False, GrabModeAsync, GrabModeAsync, CurrentTime);
    dirty = true;
}

// The circle button's flyout. It reuses the context flyout wholesale -- the same
// open/close animation, pointer grab, hover wipe and Escape/outside dismiss -- but
// rebuilds the contents as a greeting, a grid of the recent apps and a row of
// power actions, so pressing the ring is a quick way back to whatever was opened
// last and a way out of the session.
void Manager::openRingMenu() {
    if (overlayOpen()) closeOverlays();
    contextClient = nullptr;
    contextWidget = -1;
    contextDesktop = -1;
    contextPin = -1;
    contextApp = -1;
    contextRing = true;
    contextRecents = recents;
    contextFiles = recentFiles;
    // The ring drives its own rows/cells, so the plain item list stays empty.
    contextItems.clear();
    layoutRingMenu();
    contextOpen = true;
    contextHover = -1;
    startOpen = false;
    grabPointer();
    XGrabKeyboard(dpy, root, False, GrabModeAsync, GrabModeAsync, CurrentTime);
    dirty = true;
}

// Places the panel and every cell in it from the current recents, once, when the
// menu opens. drawContextMenu and contextRowAt both read these rects, so what is
// painted and what is clickable can never drift apart.
void Manager::layoutRingMenu() {
    const int pad = 8;
    const int cols = metrics::kRingCols;
    const int cell = metrics::kRingCell;
    const int width = cols * cell + 2 * pad;

    const int n = int(contextRecents.size());
    const int rows = (n + cols - 1) / cols;              // 0 when there are none
    // With no recents the grid keeps one cell's worth of height for its message,
    // so the panel does not collapse to a header-only strip.
    ringGridH = (rows == 0 ? 1 : rows) * cell;
    const int dividerH = 9;                              // hairline + breathing room

    // The recent files hide entirely when there are none, so a user whose shell
    // has opened nothing yet sees the menu exactly as it always looked. Shown
    // files cap out at a single screenful of rows underneath the app grid.
    ringFilesShown = std::min(int(contextFiles.size()), int(metrics::kRingMaxFiles));
    const bool hasFiles = ringFilesShown > 0;
    // The files' caption plus its rows (one file per row) is a section of its
    // own, kept apart from the app grid by the same hairline that splits the
    // power row off.
    const int ringFilesH =
        hasFiles ? metrics::kRingFilesLabelH + ringFilesShown * metrics::kRingFileRowH : 0;

    const int height = metrics::kRingHeaderH + ringGridH + dividerH + ringFilesH +
                       dividerH + metrics::kRingPowerH + pad;

    // Anchored above the bar at the ring's left edge, so it grows out of the
    // button that opened it and never covers the taskbar.
    const int bottom = screenH - metrics::taskbarH;
    int mx = std::max(6, circleButtonRect.x);
    int my = bottom - height - 10;
    if (my < 6) my = 6;
    if (mx + width > screenW) mx = screenW - width - 6;
    contextRect = Rect{mx, my, width, height};

    const int gx = mx + pad;
    const int gy = my + metrics::kRingHeaderH;
    ringRecentRects.clear();
    for (int i = 0; i < n; ++i) {
        const int c = i % cols, r = i / cols;
        ringRecentRects.push_back(Rect{gx + c * cell, gy + r * cell, cell, cell});
    }

    // Recent files: a caption and one row per file. The label sits on the next
    // hairline and reads as a group with the rows under it, exactly as the
    // greeting does above the app grid.
    ringFileRects.clear();
    if (hasFiles) {
        const int fy = gy + ringGridH + dividerH;
        ringFilesLabelTop = fy;
        const int ry = fy + metrics::kRingFilesLabelH;
        for (int i = 0; i < ringFilesShown; ++i)
            ringFileRects.push_back(Rect{gx, ry + i * metrics::kRingFileRowH, width - 2 * pad,
                                         metrics::kRingFileRowH});
    }

    const int py = gy + ringGridH + dividerH + ringFilesH + dividerH;
    const int pcw = (width - 2 * pad) / metrics::kRingPowerCount;
    ringPowerRects.clear();
    for (int i = 0; i < metrics::kRingPowerCount; ++i) {
        ringPowerRects.push_back(Rect{gx + i * pcw, py, pcw, metrics::kRingPowerH});
    }
}

// The index of the cell under a point: 0..recents-1 for the app grid, then the
// recent-file rows, then the power buttons, or -1. An ordinary flyout keeps its
// single-column rows instead. One definition, so the hover wipe and the press
// handler can never disagree.
int Manager::contextRowAt(int x, int y) const {
    if (!contextOpen || !contextRect.contains(x, y)) return -1;
    if (contextRing) {
        for (size_t i = 0; i < ringRecentRects.size(); ++i) {
            if (ringRecentRects[i].contains(x, y)) return int(i);
        }
        const size_t base = ringRecentRects.size();
        for (size_t i = 0; i < ringFileRects.size(); ++i) {
            if (ringFileRects[i].contains(x, y)) return int(base + i);
        }
        for (size_t i = 0; i < ringPowerRects.size(); ++i) {
            const size_t pb = base + ringFileRects.size();
            if (ringPowerRects[i].contains(x, y)) return int(pb + i);
        }
        return -1;
    }
    const int top = contextRect.y + 6;
    if (y < top) return -1;
    const int idx = (y - top) / 32;
    if (idx < 0 || idx >= int(contextItems.size())) return -1;
    return idx;
}

// The four power actions, in the order layoutRingMenu lays them out and
// drawContextMenu paints them. `$USER` is expanded by the shell launchApp runs.
void Manager::runRingPower(int index) {
    switch (index) {
        case 0: launchApp("loginctl suspend"); break;
        case 1: launchApp("loginctl terminate-user \"$USER\""); break;
        case 2: launchApp("loginctl reboot"); break;
        case 3: launchApp("loginctl poweroff"); break;
        default: break;
    }
}

// ------------------------------------------------------------------ reordering
// A press on a pinned button arms a drag instead of launching straight away: the
// button only lifts once the pointer has travelled far enough, so an ordinary
// click is still an ordinary click. From then on the pointer owns the drag and the
// button under it is the one being carried.
void Manager::beginPinDrag(int index, int x) {
    if (index < 0 || index >= int(pinned.size())) return;
    // The bar's edge owns the pointer while it is being dragged.
    if (taskbarResizeY >= 0) return;
    pinDrag = index;
    pinDragPressX = x;
    pinDragMoved = false;
    pinDragOrder = pinned;
}

void Manager::updatePinDrag(int x) {
    if (pinDrag < 0 || pinDrag >= int(pinned.size())) return;
    if (!pinDragMoved) {
        const int dx = x - pinDragPressX;
        if (dx > -metrics::kPinDragSlop && dx < metrics::kPinDragSlop) return;
        pinDragMoved = true;
        dirty = true;
    }
    // Where the pin wants to land: the slot the pointer is in, counted over the
    // *other* buttons. Skipping the dragged one is what keeps it from shoving
    // itself along by its own centre as it is carried past a neighbour.
    const int current = pinDrag;
    int slot = 0;
    for (const TaskItem& it : taskItems) {
        if (it.pin < 0 || it.pin == current) continue;
        if (x > it.rect.x + it.rect.w / 2) ++slot;
        else break;
    }
    if (slot == current) return;
    const AppEntry carried = pinned[size_t(current)];
    pinned.erase(pinned.begin() + current);
    pinned.insert(pinned.begin() + slot, carried);
    pinDrag = slot;
    dirty = true;
}

void Manager::endPinDrag(bool commit) {
    if (pinDrag < 0) return;
    const int index = pinDrag;
    const bool moved = pinDragMoved;
    // A drag that never travelled, or one Escape called off, leaves the pins
    // exactly as the press found them; a real drag is what gets written out.
    if (!commit || !moved) pinned = pinDragOrder;
    else savePinned(pinned);
    pinDrag = -1;
    pinDragMoved = false;
    pinDragOrder.clear();
    // A press that turned out to be a click is the click the old code did on the
    // press itself, just answered one event later.
    if (commit && !moved) activatePinned(index);
    dirty = true;
}

// The grab strip along the top of the taskbar. It reaches a few pixels above the
// bar as well as into it: the bar's buttons are centred, so the space between the
// top edge and the first button is empty, and the desktop just above the bar is
// empty too because maximised windows stop at the bar.
bool Manager::taskbarGripAt(int x, int y) const {
    if (tabletMode || x < 0 || x >= screenW) return false;
    const int top = screenH - metrics::taskbarH;
    return y >= top - metrics::kTaskbarOuterGrip && y < top + metrics::taskbarInnerGrip();
}

void Manager::beginTaskbarResize(int y) {
    // One pointer drag at a time: a resize in flight keeps the button until it is
    // released, so a second grab cannot restart the bar from a different origin.
    if (taskbarResizeY >= 0 || pinDrag >= 0) return;
    taskbarResizeY = y;
    taskbarResizeH = metrics::taskbarH;
}

void Manager::updateTaskbarResize(int y) {
    if (taskbarResizeY < 0) return;
    // Dragging the edge upwards grows the bar, the same way dragging a window's
    // bottom edge upwards grows its height. Computed from the thickness at the
    // press, so the bar tracks the pointer exactly instead of drifting by the
    // rounding of each step.
    const int want = taskbarResizeH + (taskbarResizeY - y);
    const int next = std::max(metrics::kTaskbarMinH,
                              std::min(metrics::kTaskbarMaxH, want));
    if (next == metrics::taskbarH) return;
    metrics::taskbarH = next;
    layoutTaskbar();
    // Buttons and icon boxes are derived from the thickness, so the whole bar
    // reflows from this one value.
    reflowWorkAreaWindows();
    updateWorkArea();
    dirty = true;
}

void Manager::endTaskbarResize() {
    if (taskbarResizeY < 0) return;
    taskbarResizeY = -1;
    saveTaskbarHeight(metrics::taskbarH);
    dirty = true;
}

// A maximised or snapped window is sized from the work area, which the bar is part
// of, so a bar that changed thickness leaves them floating. setMaximized() cannot
// do this job: it returns early when the flags already match, which is exactly the
// case here.
void Manager::reflowWorkAreaWindows() {
    const Rect wa = workArea();
    for (auto& cp : clients) {
        Client* c = cp.get();
        if (!c->managed || !c->alive || c->fullscreen || c->closing) continue;
        if (c->snapZone != kSnapNone) {
            c->frame = snapGeometry(c->snapZone);
        } else if (c->maximizedH || c->maximizedV) {
            Rect f = c->frame;
            if (c->maximizedH) {
                f.x = wa.x;
                f.w = wa.w;
            }
            if (c->maximizedV) {
                f.y = wa.y;
                f.h = wa.h;
            }
            c->frame = f;
        } else {
            continue;  // an ordinary window keeps the size the user gave it
        }
        applyFrame(c, true);
        syncClientGeometry(c);
        updateStateAtoms(c);
    }
}

// Wheel over the taskbar: move the focus to the next/previous button, which is
// what every other desktop does and what makes a crowded taskbar usable. Pinned
// launchers have no window of their own to focus, so they are skipped.
void Manager::cycleTaskbar(bool backward) {
    std::vector<Client*> buttons;
    for (const TaskItem& it : taskItems) {
        if (it.client) buttons.push_back(it.client);
    }
    const int n = int(buttons.size());
    if (n == 0) return;
    int idx = -1;
    for (int i = 0; i < n; ++i) {
        if (buttons[size_t(i)] == focused) {
            idx = i;
            break;
        }
    }
    const int next = backward ? (idx <= 0 ? n - 1 : idx - 1) : (idx + 1) % n;
    if (idx >= 0 && next == idx) return;  // a single button stays put
    Client* target = buttons[size_t(next)];
    if (!target) return;
    restoreClient(target);
    focusClient(target, true);
}

// Fullscreen windows that cannot benefit from compositing are handed back to
// the X server ("unredirected"), which removes us from their present path
// entirely -- the reason games and video players stay smooth in here.
void Manager::updateFullscreenRedirection() {
    if (!opts || !opts->unredirect) return;
    Client* top = nullptr;
    for (size_t i = clients.size(); i-- > 0;) {
        Client* c = clients[i].get();
        if (!c->mapped || c->minimized || c->isDock || c->isDesktop) continue;
        top = c;
        break;
    }
    for (auto& cp : clients) {
        Client* c = cp.get();
        const bool want = (c == top) && c->fullscreen && !c->hasAlpha && c->mapped && c->alive;
        if (want == c->unredirected) continue;
        if (want) {
            destroyPixmap(c);
            XCompositeUnredirectWindow(dpy, c->id, CompositeRedirectManual);
            c->redirected = false;
            c->unredirected = true;
            // Keep it just above the overlay, below menus and docks.
            XWindowChanges wc{};
            wc.sibling = comp.overlay();
            wc.stack_mode = Above;
            XConfigureWindow(dpy, c->id, CWSibling | CWStackMode, &wc);
            log("unredirected fullscreen window 0x%lx (%s)", c->id, c->title.c_str());
        } else {
            XCompositeRedirectWindow(dpy, c->id, CompositeRedirectManual);
            c->redirected = true;
            c->unredirected = false;
            c->namedPixmap = 0;
            c->pixW = c->pixH = 0;
        }
        dirty = true;
    }
    restack();
}

void Manager::grabPointer() {
    if (!comp.overlay()) return;
    g_lastError = 0;
    // owner_events = False: while a flyout (Start, Task View, Alt-Tab, the
    // window menu) is open the shell must see every press itself, not the
    // window the pointer happens to be over.
    XGrabPointer(dpy, comp.overlay(), False,
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync,
                 GrabModeAsync, None, None, CurrentTime);
}

void Manager::ungrabPointer() {
    if (!dpy || !comp.overlay()) return;
    // An in-flight drag owns the pointer until it finishes, and the app
    // switcher keeps it until it closes so every click on a card is ours and
    // never leaks through to the apps sitting behind it.
    if (dragClient || dragWidget >= 0 || tabletSwitcher) return;
    XUngrabPointer(dpy, CurrentTime);
}

namespace {
// Launchpad transition presets. Enter is critically damped at a moderate
// frequency, so the field arrives without the overshoot that would push the
// 1.2 -> 1 scale back under 1; exit is faster still, so leaving reads as crisp.
// Because the spring keeps its value and velocity across the retarget, an open
// reversed mid-flight bends its trajectory instead of restarting it.
constexpr double kStartEnterHz = 2.7;     // omega ~= 17 rad/s
constexpr double kStartEnterZeta = 1.0;   // critically damped
constexpr double kStartExitHz = 4.1;      // omega ~= 26 rad/s
constexpr double kStartExitZeta = 0.95;   // no visible overshoot
}  // namespace

// One frame of the Launchpad progress. The whole field is drawn straight from
// this value -- a fade plus a scale about the centre -- so there is no trajectory
// ring to keep any more.
void Manager::stepStartSpring(double dtSec, double now) {
    (void)now;
    const double target = startOpen ? 1.0 : 0.0;
    if (target != startTargetOpen) {
        startTargetOpen = target;
        const bool enter = target > 0.5;
        startSpring.setFrequency(enter ? kStartEnterHz : kStartExitHz);
        startSpring.zeta = enter ? kStartEnterZeta : kStartExitZeta;
    }
    startSpring.step(target, dtSec);
    // Terminate exactly at rest: a spring that never quite arrives keeps the
    // shell repainting forever, which is a battery leak rather than a transition.
    if (startSpring.settled(target)) {
        startSpring.value = target;
        startSpring.velocity = 0.0;
    }
    startAnim = startSpring.value;
    if (startSpring.value != target) dirty = true;
}

void Manager::openStartMenu() {
    if (contextOpen || taskViewOpen) closeOverlays();
    startOpen = true;
    searchText.clear();
    hoverApp = -1;
    startHoverDot = -1;
    startPage = 0;
    launchPageOffset = 0.0;
    launchSwipe = false;
    launchPressArmed = false;
    launchPressTile = -1;
    // A fresh launchpad has no carried tile and its springs are rebuilt for the
    // page it opens on.
    launchDragMoving = false;
    launchDragTile = -1;
    launchDragFrom = -1;
    launchTilePage = -1;
    launchTileTotal = 0;
    layoutStartMenu();
    grabPointer();
    XGrabKeyboard(dpy, root, False, GrabModeAsync, GrabModeAsync, CurrentTime);
    dirty = true;
}

void Manager::toggleTaskView() {
    if (taskViewOpen) {
        closeOverlays();
        return;
    }
    if (contextOpen || startOpen) closeOverlays();
    taskViewOpen = true;
    taskViewHover = -1;
    grabPointer();
    XGrabKeyboard(dpy, root, False, GrabModeAsync, GrabModeAsync, CurrentTime);
    dirty = true;
}

// The window menu (Win11 reaches it with Alt+Space, or right-clicking the
// caption). Item order is fixed so input.cpp can switch on the index.
void Manager::showContextMenu(Client* c, int x, int y) {
    if (!c) return;
    contextClient = c;
    contextWidget = -1;
    contextDesktop = -1;
    contextPin = -1;
    contextApp = -1;
    contextItems.clear();
    contextItems.push_back((c->maximizedH && c->maximizedV) ? "Restore" : "Maximize");
    contextItems.push_back("Minimize");
    contextItems.push_back(c->fullscreen ? "Exit full screen" : "Full screen");
    contextItems.push_back("Snap left");
    contextItems.push_back("Snap right");
    contextItems.push_back("Close");

    const int itemH = 32;
    const int width = 200;
    const int height = int(contextItems.size()) * itemH + 12;
    int mx = x, my = y - 6;
    const int bottom = screenH - metrics::taskbarH;
    if (my + height > bottom) my = bottom - height;
    if (mx + width > screenW) mx = screenW - width - 4;
    if (mx < 4) mx = 4;
    if (my < 4) my = 4;
    contextRect = Rect{mx, my, width, height};
    contextOpen = true;
    contextHover = -1;
    startOpen = false;
    grabPointer();
    XGrabKeyboard(dpy, root, False, GrabModeAsync, GrabModeAsync, CurrentTime);
    dirty = true;
}

// The desktop's own context menu, reusing the client menu plumbing. Its shape
// follows what it was opened on: a card offers to be removed, a folder offers to
// be renamed or deleted, and the create and widget entries are always there.
// applyContextAction() walks the rows in this same order.
void Manager::openDesktopMenu(int x, int y) {
    contextClient = nullptr;
    contextWidget = widgetAt(x, y);
    contextDesktop = desktopItemAt(x, y);
    contextPin = -1;
    contextApp = -1;
    contextItems.clear();
    if (contextWidget >= 0) contextItems.push_back("Remove Widget");
    // Only a folder can be renamed or deleted as a folder. A plain file is the
    // user's, and this menu does not manage files.
    if (contextDesktop >= 0 && contextDesktop < int(desktopItems.size()) &&
        desktopItems[size_t(contextDesktop)].isDir) {
        contextItems.push_back("Rename Folder");
        contextItems.push_back("Delete Folder");
    }
    contextItems.push_back("New Folder");
    contextItems.push_back("Add Clock Widget");
    contextItems.push_back("Add Battery Widget");
    contextItems.push_back("Add Calendar Widget");
    contextItems.push_back("Add Weather Widget");
    contextItems.push_back("Add Digital Clock Widget");

    const int itemH = 32;
    const int width = 210;
    const int height = int(contextItems.size()) * itemH + 12;
    int mx = x, my = y - 6;
    const int bottom = screenH - metrics::taskbarH;
    if (my + height > bottom) my = bottom - height;
    if (mx + width > screenW) mx = screenW - width - 4;
    if (mx < 4) mx = 4;
    if (my < 4) my = 4;
    contextRect = Rect{mx, my, width, height};
    contextOpen = true;
    contextHover = -1;
    startOpen = false;
    grabPointer();
    XGrabKeyboard(dpy, root, False, GrabModeAsync, GrabModeAsync, CurrentTime);
    dirty = true;
}

// ---------------------------------------------------------------------------
// Desktop folders
// ---------------------------------------------------------------------------

namespace {

// What a typed folder name actually is. Leading and trailing spaces are a habit,
// not an intention, and a name that is only spaces would make an entry the user
// could never click on again.
std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

}  // namespace

// The desktop directory changed under us -- the shell just created, renamed or
// deleted an entry, or something outside the shell did. Rebuilding around what is
// really there is what keeps the icons from claiming a folder that is gone or
// hiding one that just appeared.
void Manager::refreshDesktop(const std::string& selectPath) {
    desktopItems = scanDesktop();
    layoutDesktopIcons();
    selectedDesktopIcon = -1;
    hoverDesktopIcon = -1;
    // The grid is sorted by name, so an entry that survived a rename has a
    // different index than it had. Find it again by what it is, not by where it
    // was.
    if (!selectPath.empty()) {
        for (size_t i = 0; i < desktopItems.size(); ++i) {
            if (desktopItems[i].path != selectPath) continue;
            selectedDesktopIcon = int(i);
            hoverDesktopIcon = int(i);
            break;
        }
    }
    dirty = true;
}

// Renaming puts a field over the icon's own label instead of opening a dialog, so
// the entry is named where the user is already looking. The keyboard is held for
// the duration -- the same capture the Start menu's search box uses -- so a name
// can be typed without leaving the desktop for anything.
void Manager::beginDesktopRename(int index) {
    if (index < 0 || index >= int(desktopItems.size())) return;
    if (!desktopItems[size_t(index)].isDir) return;
    desktopRenameItem = index;
    desktopRenameText = desktopItems[size_t(index)].name;
    grabPointer();
    XGrabKeyboard(dpy, root, False, GrabModeAsync, GrabModeAsync, CurrentTime);
    dirty = true;
}

void Manager::cancelDesktopRename() {
    if (desktopRenameItem < 0) return;
    desktopRenameItem = -1;
    desktopRenameText.clear();
    ungrabPointer();
    XUngrabKeyboard(dpy, CurrentTime);
    dirty = true;
}

// The field sits exactly where the icon's own label is drawn, a little wider and
// tall enough to type into, so naming an entry does not move it.
Rect Manager::desktopRenameRect() const {
    if (desktopRenameItem < 0) return {};
    const size_t i = size_t(desktopRenameItem);
    const std::vector<Rect>& hit =
        desktopIconDraw.size() == desktopIconRects.size() ? desktopIconDraw : desktopIconRects;
    if (i >= hit.size()) return {};
    const Rect cell = hit[i];
    const int w = std::max(88, cell.w + 12);
    return Rect{cell.x + (cell.w - w) / 2, cell.y + 8 + 48 + 3, w, 24};
}

void Manager::commitDesktopRename() {
    const int index = desktopRenameItem;
    const std::string wanted = trimmed(desktopRenameText);
    std::string path;
    if (index >= 0 && index < int(desktopItems.size())) path = desktopItems[size_t(index)].path;
    // Hand the keyboard and pointer back before touching the disk: whatever the
    // rename turns out to be, the desktop is interactive again by the time we
    // find out.
    cancelDesktopRename();
    if (path.empty() || wanted.empty()) return;
    // A name that is unusable or already taken is refused rather than forced, so
    // the entry simply keeps the name it had.
    if (!renameDesktopEntry(path, wanted)) return;
    refreshDesktop(path);
}

// A folder can hold files, so deleting one from the desktop takes whatever was in
// it. That is the only irreversible thing this menu does, which is why it arrives
// through a dialog that names the folder and offers a way out.
void Manager::openConfirmDelete(const std::string& path, const std::string& name) {
    confirmDeletePath = path;
    confirmDeleteName = name;
    confirmDeleteOpen = true;

    const int panelW = std::min(420, screenW - 48);
    const int panelH = 196;
    const Rect panel{(screenW - panelW) / 2, (screenH - panelH) / 2, panelW, panelH};
    const int btnW = std::min(112, (panelW - 54) / 2);
    const int btnH = 36;
    const int by = panel.bottom() - 24 - btnH;
    const int left = panel.x + (panelW - (2 * btnW + 12)) / 2;
    confirmDeleteCancel = Rect{left, by, btnW, btnH};
    confirmDeleteOk = Rect{left + btnW + 12, by, btnW, btnH};

    grabPointer();
    XGrabKeyboard(dpy, root, False, GrabModeAsync, GrabModeAsync, CurrentTime);
    dirty = true;
}

void Manager::closeConfirmDelete() {
    if (!confirmDeleteOpen) return;
    confirmDeleteOpen = false;
    confirmDeletePath.clear();
    confirmDeleteName.clear();
    ungrabPointer();
    XUngrabKeyboard(dpy, CurrentTime);
    dirty = true;
}

void Manager::commitConfirmDelete() {
    const std::string path = confirmDeletePath;
    // Let go first: the delete can take a moment on a large folder, and the
    // desktop must not be frozen behind a dialog that has already been answered.
    closeConfirmDelete();
    if (path.empty()) return;
    if (deleteDesktopEntry(path)) refreshDesktop();
}

// @@MANAGER_CPP_END@@

}  // namespace wm