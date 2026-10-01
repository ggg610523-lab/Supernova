#include "manager.h"
#include "xprop.h"

#include <X11/cursorfont.h>
#include <X11/keysym.h>

#include <algorithm>
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
    text.shutdown();
    icons.clear();
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
    // scripts/fetch-assets.sh) and the MuternVF variable font. All three are
    // resolved before the compositor starts because it bakes the wallpaper as
    // part of init; each degrades gracefully when absent.
    assetDir = defaultAssetDir();
    if (!comp.init(dpy, screen, screenW, screenH, options.vsync,
                   assetDir + "/wallpaper/wallpaper.png", &error)) {
        log("compositor: %s", error.c_str());
        return 1;
    }

    icons.init(assetDir + "/icons");
    text.init(assetDir + "/fonts");

    std::string keyError;
    grabKeys(&keyError);
    if (!keyError.empty()) log("warning: %s", keyError.c_str());

    setupRootProperties();
    updateWorkArea();
    apps = scanApps();
    // The desktop shows the session's real Desktop directory; layout is fixed, so
    // it is computed once here and reused by every frame and hit test.
    desktopItems = scanDesktop();
    layoutDesktopIcons();
    initWidgets();
    scanExistingWindows();
    updateClientList();
    // Kick the first state probe now, off the critical path, so the Control
    // Centre is already populated the first time the clock is clicked.
    sysctl.init();

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
    log("assets: %s", assetDir.c_str());

    lastTick = nowMs();
    dirty = true;
    loop();
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
        poll(&p, 1, timeout);
        if (opts->stats) dirty = true;
    }
}

void Manager::tickAnimations(double now) {
    double dt = now - lastTick;
    lastTick = now;
    if (dt <= 0.0 || dt > 0.1) dt = 1.0 / 60.0;  // ignore stalls and the first frame
    const double dtMs = dt * 1000.0;

    for (auto& cp : clients) {
        Client* c = cp.get();
        const double p = c->animMs > 0 ? clamp01((now - c->animStart) / double(c->animMs)) : 1.0;
        const double eased = fluentEase(p);

        if (p < 1.0) {
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
        // Minimise/restore runs linearly so it can be reversed mid flight.
        const double minTarget = c->minimized ? 1.0 : 0.0;
        if (c->minFade != minTarget) {
            const double step = dtMs / double(metrics::kAnimMs);
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
    step(startAnim, startOpen ? 1.0 : 0.0, 170.0);
    step(taskViewAnim, taskViewOpen ? 1.0 : 0.0, 170.0);
    step(altTabAnim, altTabOpen ? 1.0 : 0.0, 120.0);
    // Control Centre: a slightly longer ease so the grid reads as rising out
    // of the taskbar rather than simply appearing.
    step(ccAnim, ccOpen ? 1.0 : 0.0, 210.0);
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

    // Widgets: repaint the analog clock when the wall second changes, and
    // re-probe the battery every few seconds.
    if (!widgets.empty()) {
        const time_t sec = time(nullptr);
        if (sec != lastClockSecond) {
            lastClockSecond = sec;
            dirty = true;
        }
        if (now - lastBatteryProbe > 5000.0) {
            lastBatteryProbe = now;
            refreshBattery(false);
        }
    }
    if (opts->stats) dirty = true;
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
    const int taskbarTop = screenH - metrics::kTaskbarH;
    const bool overTaskbar = py >= taskbarTop;

    bool newStart = false, newShowDesktop = false, newClock = false;
    int newTask = -1;
    if (overTaskbar) {
        newStart = startButtonRect.contains(px, py);
        if (!startOpen) {
            newShowDesktop = showDesktopRect.contains(px, py);
            newClock = clockRect.contains(px, py);
            for (size_t i = 0; i < taskItems.size(); ++i) {
                if (taskItems[i].rect.contains(px, py)) {
                    newTask = int(i);
                    break;
                }
            }
        }
    }
    if (newStart != hoverStart || newShowDesktop != hoverShowDesktop ||
        newClock != hoverClock || newTask != hoverTaskIndex) {
        hoverStart = newStart;
        hoverShowDesktop = newShowDesktop;
        hoverClock = newClock;
        hoverTaskIndex = newTask;
        changed = true;
    }

    if (startOpen) {
        int newApp = -1;
        for (size_t i = 0; i < appRects.size(); ++i) {
            if (appRects[i].contains(px, py)) {
                newApp = int(i);
                break;
            }
        }
        if (newApp != hoverApp) {
            hoverApp = newApp;
            changed = true;
        }
        int newDot = -1;
        if (newApp < 0) {
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
    // A Control Centre control is a button, so it gets the hand cursor.
    if (ccOpen && ccHover >= 0 && size_t(ccHover) < ccControls.size()) wantedCursor = 5;
    // Launchpad tiles and page dots are clickable too.
    if (startOpen && (hoverApp >= 0 || startHoverDot >= 0)) wantedCursor = 5;
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
    if (wantedCursor != cursorShown) setCursor(wantedCursor);

    if (changed) dirty = true;
}

bool Manager::overlayOpen() const {
    return startOpen || taskViewOpen || altTabOpen || contextOpen || ccOpen;
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
    altTabOrder.clear();
    altTabIndex = 0;
    searchText.clear();
    hoverApp = -1;
    startHoverDot = -1;
    startPage = 0;
    contextClient = nullptr;
    contextWidget = -1;
    contextItems.clear();
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
    if (screenH > metrics::kTaskbarH * 2) wa.h -= metrics::kTaskbarH;  // our taskbar
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

// Applies `frame` (already updated) either instantly -- dragging must never be
// animated -- or through the Fluent geometry animation.
void Manager::applyFrame(Client* c, bool animate) {
    if (!c) return;
    if (animate) {
        c->animFrom = c->drawFrame.empty() ? c->frame : c->drawFrame;
        c->animStart = nowMs();
        c->animMs = metrics::kZoomMs;
    } else {
        c->drawFrame = c->frame;
        c->animMs = metrics::kZoomMs;
        c->animStart = nowMs() - double(metrics::kZoomMs);
    }
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
    c->animMs = metrics::kAnimMs;
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
    c->drawFrame = c->frame;
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

// Wheel over the taskbar: move the focus to the next/previous button, which is
// what every other desktop does and what makes a crowded taskbar usable.
void Manager::cycleTaskbar(bool backward) {
    if (taskItems.empty()) return;
    Client* start = focused;
    int idx = -1;
    for (size_t i = 0; i < taskItems.size(); ++i) {
        if (taskItems[i].client == start) {
            idx = int(i);
            break;
        }
    }
    const int n = int(taskItems.size());
    int next = backward ? (idx <= 0 ? n - 1 : idx - 1) : (idx + 1) % n;
    if (idx >= 0 && next == idx) next = idx;  // a single button stays put
    Client* target = taskItems[size_t(next)].client;
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
    // An in-flight drag owns the pointer until it finishes.
    if (dragClient || dragWidget >= 0) return;
    XUngrabPointer(dpy, CurrentTime);
}

void Manager::openStartMenu() {
    if (contextOpen || taskViewOpen) closeOverlays();
    startOpen = true;
    searchText.clear();
    hoverApp = -1;
    startHoverDot = -1;
    startPage = 0;
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
    const int bottom = screenH - metrics::kTaskbarH;
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

// The desktop's own context menu, reusing the client menu plumbing. On a widget
// it offers "Remove Widget"; on bare wallpaper (or below a widget entry) the two
// add entries.
void Manager::openDesktopMenu(int x, int y) {
    contextClient = nullptr;
    contextWidget = widgetAt(x, y);
    contextItems.clear();
    if (contextWidget >= 0) contextItems.push_back("Remove Widget");
    contextItems.push_back("Add Clock Widget");
    contextItems.push_back("Add Battery Widget");

    const int itemH = 32;
    const int width = 210;
    const int height = int(contextItems.size()) * itemH + 12;
    int mx = x, my = y - 6;
    const int bottom = screenH - metrics::kTaskbarH;
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

// @@MANAGER_CPP_END@@

}  // namespace wm