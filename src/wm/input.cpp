// All the things that come back from the X server: requests from clients we
// have to answer, damage notifications, and the user's pointer and keyboard.
#include "manager.h"

#include <X11/keysym.h>

#include <algorithm>
#include <cstring>

namespace wm {
namespace {

// _NET_WM_MOVERESIZE direction -> our resize edge bitmask.
int moveResizeEdge(int direction) {
    switch (direction) {
        case 0: return 1 | 4;   // top left
        case 1: return 4;       // top
        case 2: return 2 | 4;   // top right
        case 3: return 2;       // right
        case 4: return 2 | 8;   // bottom right
        case 5: return 8;       // bottom
        case 6: return 1 | 8;   // bottom left
        case 7: return 1;       // left
        default: return 0;
    }
}

}  // namespace

void Manager::onMapRequest(XMapRequestEvent& ev) {
    Client* c = find(ev.window);
    const bool known = c != nullptr;
    if (!c) c = add(ev.window, false);
    if (!c) {
        // Nothing we manage (should not happen): let the server do its thing.
        XMapWindow(dpy, ev.window);
        return;
    }
    if (!c->managed) {
        // Docks and desktop windows are the session's business.
        XMapWindow(dpy, ev.window);
        c->mapped = true;
        dirty = true;
        return;
    }
    if (c->closing) return;
    if (!known) placeNewClient(c);
    mapClient(c);
    // An app opened from the tablet home screen stays in tablet mode: it fills
    // the display between the status bar and the home indicator.
    if (tabletMode) {
        makeTabletApp(c);
        focusClient(c, true);
        return;
    }
    // If the user launched this from a desktop icon, grow the window out of that
    // placeholder rather than letting it pop in on top of it.
    if (!known) claimLaunch(c);
    focusClient(c, true);
}

void Manager::onConfigureRequest(XConfigureRequestEvent& ev) {
    Client* c = find(ev.window);
    const unsigned mask = unsigned(ev.value_mask);
    if (!c || !c->managed) {
        XWindowChanges wc{};
        wc.x = ev.x;
        wc.y = ev.y;
        wc.width = ev.width;
        wc.height = ev.height;
        wc.border_width = ev.border_width;
        wc.sibling = ev.above;
        wc.stack_mode = ev.detail;
        XConfigureWindow(dpy, ev.window, mask, &wc);
        if (c) {
            c->frame = Rect{ev.x, ev.y, ev.width, ev.height};
            c->drawFrame = c->frame;
            if (c->isDock) {
                readStruts(c);
                updateWorkArea();
            }
            dirty = true;
        }
        return;
    }
    if (c->closing) return;
    if (mask & CWStackMode) restack();

    // Size requests become client-size requests: the frame grows by the chrome.
    const bool wantsSize = (mask & (CWWidth | CWHeight)) != 0;
    const bool wantsPos = (mask & (CWX | CWY)) != 0;
    if (wantsSize) {
        const Rect cur = clientRect(c);
        int cw = (mask & CWWidth) ? ev.width : cur.w;
        int ch = (mask & CWHeight) ? ev.height : cur.h;
        if (cw < c->minW) cw = c->minW;
        if (ch < c->minH) ch = c->minH;
        if (c->maxW > 0 && cw > c->maxW) cw = c->maxW;
        if (c->maxH > 0 && ch > c->maxH) ch = c->maxH;
        // A maximised, snapped or fullscreen window keeps the geometry we gave it.
        const bool locked =
            c->fullscreen || c->tabletApp || (c->maximizedH && c->maximizedV) ||
            c->snapZone != kSnapNone;
        if (!locked) {
            c->frame.w = cw + 2 * metrics::kBorder;
            c->frame.h = ch + c->captionH + metrics::kBorder;
            // A window must stay reachable: without this a client that asks for
            // a size wider than the screen pushes its right edge (and with it the
            // close button) off the display, where nobody can click it.
            c->frame = clampRect(c->frame, Rect{0, 0, screenW, screenH});
            c->drawFrame = c->frame;
            c->animMs = 0;
            syncClientGeometry(c);
        }
    }
    if (wantsPos && !c->mapped) {
        // Only honoured before the window is first placed.
        if (mask & CWX) c->frame.x = ev.x;
        if (mask & CWY) c->frame.y = ev.y;
        c->drawFrame = c->frame;
    }
    dirty = true;
}

void Manager::onConfigureNotify(XConfigureEvent& ev) {
    Client* c = find(ev.window);
    if (!c || !c->mapped || ev.window != c->id) return;
    const Rect expected = clientRect(c);
    if (ev.width == expected.w && ev.height == expected.h) return;

    // The window changed size behind our back: a toolkit resize, or a client
    // that sets its own geometry.
    if (c->pixW != ev.width || c->pixH != ev.height) {
        c->pixW = c->pixH = 0;
        ensurePixmap(c);
    }
    const bool weDriveIt = c->fullscreen || c->tabletApp || (c->maximizedH && c->maximizedV) ||
                           c->snapZone != kSnapNone || (dragClient == c && !dragIsMove);
    if (!weDriveIt && c->managed) {
        c->frame.w = ev.width + 2 * metrics::kBorder;
        c->frame.h = ev.height + c->captionH + metrics::kBorder;
        c->frame = clampRect(c->frame, Rect{0, 0, screenW, screenH});
        c->drawFrame = c->frame;
        c->animMs = 0;
        dirty = true;
    }
}

void Manager::onPropertyNotify(XPropertyEvent& ev) {
    Client* c = find(ev.window);
    if (!c) return;
    const Atom a = ev.atom;
    if (a == A.netWmName || a == A.wmName) {
        readTitle(c);
        dirty = true;
    } else if (a == A.netWmIcon) {
        readIcon(c);
        dirty = true;
    } else if (a == A.netWmStrut || a == A.netWmStrutPartial) {
        readStruts(c);
        updateWorkArea();
        dirty = true;
    } else if (a == A.netWmWindowType) {
        readWindowType(c);
        dirty = true;
    } else if (a == A.motifHints) {
        readDecorations(c);
    } else if (a == A.wmNormalHints) {
        readNormalHints(c);
    } else if (a == A.netWmOpacity) {
        readOpacity(c);
    }
    // _NET_WM_STATE is deliberately ignored here: clients are supposed to *ask*
    // for state changes with a ClientMessage, and reacting to our own writes
    // would make us fight the toolkit.
}

void Manager::onClientMessage(XClientMessageEvent& ev) {
    Client* c = find(ev.window);
    if (!c) return;

    if (ev.message_type == A.wmChangeState) {
        if (ev.data.l[0] == IconicState) minimizeClient(c, true);
        return;
    }
    if (ev.message_type == A.netCloseWindow) {
        closeClient(c);
        return;
    }
    if (ev.message_type == A.netActiveWindow) {
        restoreClient(c);
        focusClient(c, true);
        return;
    }
    if (ev.message_type == A.netRequestFrameExtents) {
        syncClientGeometry(c);
        return;
    }
    // A client side decorated window asking us to move or resize it: this is the
    // path that makes GTK/Qt/Electron header bars draggable in here.
    if (ev.message_type == A.netWmMoveresize || ev.message_type == A.netMoveresizeWindow) {
        // _NET_MOVERESIZE_WINDOW carries the direction in data.l[2] as well but
        // is the one modern toolkits use first; both are handled identically.
        const int x = int(ev.data.l[0]);
        const int y = int(ev.data.l[1]);
        const int dir = int(ev.data.l[2]);
        if (dir == 8) beginMove(c, x, y);
        else if (dir >= 0 && dir <= 7) beginResize(c, moveResizeEdge(dir), x, y);
        else if (dir == 9) toggleMaximize(c);
        else if (dir == 10) closeClient(c);
        else if (dir == 11) minimizeClient(c, true);
        return;
    }
    if (ev.message_type == A.netWmState) {
        const long action = ev.data.l[0];  // 0 remove, 1 add, 2 toggle
        const Atom props[2] = {Atom(ev.data.l[1]), Atom(ev.data.l[2])};
        for (Atom prop : props) {
            if (!prop) continue;
            if (prop == A.stateFullscreen) {
                setFullscreen(c, action == 0 ? false : (action == 1 ? true : !c->fullscreen));
            } else if (prop == A.stateMaximizedHorz || prop == A.stateMaximizedVert) {
                const bool h = prop == A.stateMaximizedHorz ? action != 0 : c->maximizedH;
                const bool v = prop == A.stateMaximizedVert ? action != 0 : c->maximizedV;
                setMaximized(c, h, v);
            } else if (prop == A.stateHidden) {
                if (action != 0) minimizeClient(c, true);
            } else if (prop == A.stateDemandsAttention) {
                c->urgent = action != 0;
                c->attentionPulse = c->urgent ? 1.0 : 0.0;
                updateStateAtoms(c);
                dirty = true;
            } else if (prop == A.stateSkipTaskbar) {
                c->skipTaskbar = action != 0;
                dirty = true;
            }
            // _NET_WM_STATE_ABOVE/BELOW are accepted, but stacking always stays
            // under our control.
        }
    }
}

void Manager::onDamage(Damage damage, Window drawable) {
    // Always subtract, even for windows we no longer track, or the server keeps
    // reporting the same damage forever.
    XDamageSubtract(dpy, damage, None, None);
    Client* c = find(drawable);
    if (!c) return;
    if (c->usingCopy) refreshCopy(c);
    c->needsRepaint = true;
    dirty = true;
}

void Manager::onUnmapNotify(XUnmapEvent& ev) {
    Client* c = find(ev.window);
    if (!c) return;
    if (!c->mapped) return;  // our own unmap (minimise, unredirection)
    // A client withdrew its window: stop compositing it but keep the Client so
    // it can come back (ICCCM "Withdrawn" state).
    c->mapped = false;
    destroyPixmap(c);
    if (dragClient == c) cancelDrag();
    if (focused == c) {
        focused = nullptr;
        focusNext(true);
    }
    dirty = true;
}

void Manager::onCrossing(XCrossingEvent& ev) {
    if (ev.window != comp.overlay()) return;
    if (ev.type == EnterNotify) {
        updateHoverStates(ev.x, ev.y);
        return;
    }
// Pointer left our chrome: clear every hover so nothing looks stuck.
if (hoverStart || hoverShowDesktop || hoverClock || hoverTaskIndex >= 0 ||
        tabletHover >= 0) {
        hoverStart = hoverShowDesktop = hoverClock = false;
        hoverTaskIndex = -1;
        tabletHover = -1;
        dirty = true;
    }
    // A caption button pressed and then abandoned (pointer slid off the shell,
    // Alt pressed, ...) must not stay latched.
    for (auto& cp : clients) {
        if (cp->pressBtn < 0) continue;
        cp->pressBtn = -1;
        dirty = true;
    }
}

namespace {
// Opt-in event tracing: WIN11WM_TRACE_INPUT=1 dumps every button and motion
// event with root coordinates, which is the only practical way to work out why
// a synthetic click did or did not land on the chrome.
bool traceInput() {
    static const bool on = [] {
        const char* v = std::getenv("WIN11WM_TRACE_INPUT");
        return v && v[0] == '1';
    }();
    return on;
}

const char* buttonName(unsigned b) {
    switch (b) {
        case Button1: return "L";
        case Button2: return "M";
        case Button3: return "R";
        case Button4: return "wheel-up";
        case Button5: return "wheel-down";
        default: return "?";
    }
}
}  // namespace

void Manager::onButtonPress(XButtonEvent& ev) {
    const int x = ev.x_root, y = ev.y_root;
    if (traceInput()) {
        log("press %s at (%d,%d) on 0x%lx mods 0x%x", buttonName(ev.button), x, y,
            static_cast<unsigned long>(ev.window), static_cast<unsigned>(ev.state));
    }

    // The mode transition splash swallows input while it plays so a stray click
    // cannot land on a shell that is about to be replaced.
    if (modeSwitching) return;
    if (tabletMode) {
        // The app switcher holds the pointer while it is up, so everything is
        // a card interaction, whatever window the event names.
        if (tabletSwitcher) {
            handleTabletSwitcherPress(x, y, ev.button);
            return;
        }
        // A press on an open tablet app is the app's: we only observe it for
        // focus and raise. Clicks on the home screen, the status bar or the home
        // indicator reach the overlay (the app is deliberately inset from both)
        // and are ours.
        if (ev.window != comp.overlay()) {
            Client* c = find(ev.window);
            if (c) handleClientPress(c, x, y, ev.button);
            return;
        }
        handleTabletPress(x, y, ev.button, ev.time);
        return;
    }

    // Alt+drag: the passive grab on the root delivers this even when the press
    // landed on a client window, which is what makes "Alt+drag to move" work
    // for every client, client side decorations included.
    if (ev.state & Mod1Mask) {
        Client* c = find(ev.window);
        if (!c) c = clientAt(x, y);
        if (c && c->managed) {
            if (ev.button == Button1) beginMove(c, x, y);
            else if (ev.button == Button3) beginResize(c, 2 | 8, x, y);
            return;
        }
    }

    // While one of our overlays owns the pointer, everything is ours.
    if (overlayOpen()) {
        handleOverlayPress(x, y, ev.button, ev.time);
        return;
    }
    // Wheel over the shell: the taskbar cycles its buttons. Clients still get
    // their own copy of Button4/5, we simply never selected wheel events on
    // them, so scrolling inside a window keeps working untouched.
    if (ev.button == Button4 || ev.button == Button5) {
        if (y >= screenH - metrics::taskbarH) cycleTaskbar(ev.button == Button5);
        return;
    }

    // Our overlay is the bottom-most window, so a click that lands on chrome we
    // draw (a caption that another window happens to overlap, a resize band
    // under a neighbour) is delivered to the window above it. Claim it back
    // before the client swallows it.
    if (handleChromePress(x, y, ev.button, ev.time)) return;

    // Clicks observed on a client window: focus and raise only. The client gets
    // its own copy of the event and never notices we were watching.
    if (ev.window != comp.overlay()) {
        Client* c = find(ev.window);
        if (c) handleClientPress(c, x, y, ev.button);
        return;
    }
    handleOverlayPress(x, y, ev.button, ev.time);
}

void Manager::onButtonRelease(XButtonEvent& ev) {
    const int x = ev.x_root, y = ev.y_root;
    if (traceInput()) {
        log("release %s at (%d,%d) on 0x%lx", buttonName(ev.button), x, y,
            static_cast<unsigned long>(ev.window));
    }
    if (tabletGesture) {
        endTabletGesture();
        return;
    }
    if (tabletSwitchDrag >= 0) {
        endTabletSwitchDrag();
        return;
    }
    if (tabletDragIcon >= 0) {
        endTabletIconDrag();
        return;
    }
    if (taskbarResizeY >= 0) {
        endTaskbarResize();
        return;
    }
    if (dragClient) {
        endDrag(x, y);
        return;
    }
    if (dragWidget >= 0) {
        endWidgetDrag();
        return;
    }
    // Dropping a pin: the reorder is committed, unless the press never became a
    // drag, in which case this release is the click the press stood for.
    if (pinDrag >= 0) {
        if (ev.button == Button1) endPinDrag(true);
        return;
    }
    // A Control Centre slider keeps tracking until the button comes up, so the
    // release does not have to land on the pill.
    if (ccDrag >= 0) {
        updateCcDrag(x, y);
        endCcDrag();
        return;
    }
    // A caption button only fires if the release lands on the same button.
    for (auto& cp : clients) {
        Client* c = cp.get();
        if (c->pressBtn < 0) continue;
        const int btn = c->pressBtn;
        c->pressBtn = -1;
        if (hitCaptionButton(c, x, y) == btn) {
            if (btn == 0) minimizeClient(c, true);
            else if (btn == 1) toggleMaximize(c);
            else if (btn == 2) closeClient(c);
        }
        dirty = true;
        break;
    }
}

void Manager::onMotion(XMotionEvent& ev) {
    const int x = ev.x_root, y = ev.y_root;
    if (traceInput() && (dragClient || ev.state)) {
        log("motion to (%d,%d) on 0x%lx mods 0x%x%s", x, y, static_cast<unsigned long>(ev.window),
            static_cast<unsigned>(ev.state), dragClient ? " [dragging]" : "");
    }
    if (tabletMode) {
        // An in-flight drag -- a bottom-edge gesture, a switcher card, a home
        // icon, a widget, a Control Centre slider -- keeps tracking; everything
        // else is tablet hover.
        if (tabletGesture) {
            updateTabletGesture(x, y);
            return;
        }
        if (tabletSwitchDrag >= 0) {
            updateTabletSwitchDrag(x, y);
            return;
        }
        if (tabletDragIcon >= 0) {
            updateTabletIconDrag(x, y);
            return;
        }
        if (dragWidget >= 0) {
            updateWidgetDrag(x, y);
            return;
        }
        if (ccDrag >= 0) {
            updateCcDrag(x, y);
            return;
        }
        updateHoverStates(x, y);
        return;
    }
    if (taskbarResizeY >= 0) {
        // The bar's edge tracks the pointer anywhere on screen, up or down, the way
        // a dragged window edge does: the pointer does not have to stay on the bar.
        updateTaskbarResize(y);
        return;
    }
    if (dragClient) {
        updateDrag(x, y);
        return;
    }
    // A pin being carried keeps tracking wherever the pointer goes: the reorder
    // is horizontal, so wandering off the taskbar must not drop it.
    if (pinDrag >= 0) {
        updatePinDrag(x);
        updateHoverStates(x, y);
        return;
    }
    if (dragWidget >= 0) {
        updateWidgetDrag(x, y);
        return;
    }
    if (ccDrag >= 0) {
        updateCcDrag(x, y);
        return;
    }
    if (ev.window == comp.overlay() || overlayOpen()) updateHoverStates(x, y);
}

void Manager::onKeyPress(XKeyEvent& ev) {
    const unsigned mods = ev.state & (ShiftMask | ControlMask | Mod1Mask | Mod4Mask);
    const KeySym sym = XLookupKeysym(&ev, 0);

    // The tablet home screen has no keyboard shortcuts; Escape still leaves
    // rearrange mode or dismisses Control Centre when it is open.
    if (tabletMode) {
        if (sym == XK_Escape) {
            if (tabletSwitcher) {
                closeTabletSwitcher();
                tabletGoHome();
            } else if (tabletEdit) {
                tabletEdit = false;
            } else {
                closeOverlays();
            }
            dirty = true;
        }
        return;
    }

    // Escape abandons a launcher reorder and puts the pins back as they were,
    // which is also what a lost button release leaves behind.
    if (pinDrag >= 0 && sym == XK_Escape) {
        endPinDrag(false);
        return;
    }

    if (ccOpen) {
        // Any key press that is not a Control Centre interaction dismisses it.
        if (sym == XK_Escape) {
            closeOverlays();
            return;
        }
    }
    if (startOpen) {
        if (sym == XK_Escape) {
            closeOverlays();
            return;
        }
        if (sym == XK_Return || sym == XK_KP_Enter) {
            if (!appFiltered.empty() && appFiltered.front() < apps.size()) {
                launchApp(apps[appFiltered.front()].exec);
            }
            closeOverlays();
            return;
        }
        // Super+<key> closes the menu and runs the shortcut, like Windows.
        if (mods & Mod4Mask) {
            runShortcut(sym, mods);
            return;
        }
        // Left/Right flip Launchpad pages.
        if (sym == XK_Left || sym == XK_Right) {
            const int next = startPage + (sym == XK_Right ? 1 : -1);
            if (next >= 0 && next < startPageCount) {
                startPage = next;
                layoutStartMenu();
                dirty = true;
            }
            return;
        }
        if (sym == XK_BackSpace) {
            if (!searchText.empty()) {
                searchText.pop_back();
                startPage = 0;
                layoutStartMenu();
                dirty = true;
            }
            return;
        }
        char buf[16] = {0};
        const int n = XLookupString(&ev, buf, sizeof buf - 1, nullptr, nullptr);
        bool printable = n > 0;
        for (int i = 0; i < n && printable; ++i) {
            const unsigned char ch = static_cast<unsigned char>(buf[i]);
            if (ch < 0x20 || ch == 0x7F) printable = false;
        }
        if (printable) {
            searchText.append(buf, size_t(n));
            startPage = 0;
            layoutStartMenu();
            dirty = true;
        }
        return;
    }
    if (altTabOpen) {
        if (sym == XK_Tab && !altTabOrder.empty()) {
            const int step = (mods & ShiftMask) ? -1 : 1;
            altTabIndex = (altTabIndex + step + int(altTabOrder.size())) % int(altTabOrder.size());
            dirty = true;
            return;
        }
        if (sym == XK_Escape) {
            closeOverlays();
            return;
        }
        if (sym == XK_Return || sym == XK_KP_Enter) {
            if (altTabIndex >= 0 && altTabIndex < int(altTabOrder.size())) {
                Client* target = altTabOrder[size_t(altTabIndex)];
                focusClient(target, true);
                restoreClient(target);
            }
            closeOverlays();
            return;
        }
        return;
    }
    if (taskViewOpen) {
        if (sym == XK_Escape) {
            closeOverlays();
            return;
        }
        if (mods & Mod4Mask) runShortcut(sym, mods);
        return;
    }
    if (contextOpen) {
        if (sym == XK_Escape) closeOverlays();
        return;
    }
    runShortcut(sym, mods);
}

void Manager::applyContextAction(int index) {
    // The pin menu, opened either on a pinned taskbar button or on a Launchpad
    // tile: a single item that pins or unpins what was clicked.
    if (contextPin >= 0 || contextApp >= 0) {
        AppEntry app;
        if (contextPin >= 0 && contextPin < int(pinned.size())) app = pinned[size_t(contextPin)];
        else if (contextApp >= 0 && contextApp < int(apps.size())) app = apps[size_t(contextApp)];
        else return;
        if (index != 0) return;
        if (isPinned(app)) unpinApp(app.exec);
        else pinApp(app);
        return;
    }
    // The desktop menu: an optional "Remove Widget" when opened on a card, then
    // the two add entries.
    if (!contextClient) {
        int first = 0;
        if (contextWidget >= 0) {
            if (index == 0) {
                removeWidget(contextWidget);
                return;
            }
            first = 1;
        }
        if (index == first) addWidget(WidgetKind::Clock);
        else if (index == first + 1) addWidget(WidgetKind::Battery);
        return;
    }
    Client* c = contextClient;
    switch (index) {
        case 0: toggleMaximize(c); break;
        case 1: minimizeClient(c, true); break;
        case 2: toggleFullscreen(c); break;
        case 3: snapClient(c, kSnapLeft); break;
        case 4: snapClient(c, kSnapRight); break;
        case 5: closeClient(c); break;
        default: break;
    }
}

void Manager::openAltTab(bool backward) {
    altTabOrder.clear();
    if (focused) altTabOrder.push_back(focused);
    for (size_t i = clients.size(); i-- > 0;) {
        Client* c = clients[i].get();
        if (!c->managed || !c->alive || c->isDock || c->isDesktop || c->skipTaskbar) continue;
        if (c == focused) continue;
        altTabOrder.push_back(c);
    }
    if (altTabOrder.size() < 2) {
        altTabOrder.clear();
        return;
    }
    altTabOpen = true;
    altTabIndex = backward ? int(altTabOrder.size()) - 1 : 1;
    grabPointer();
    XGrabKeyboard(dpy, root, False, GrabModeAsync, GrabModeAsync, CurrentTime);
    dirty = true;
}

void Manager::runShortcut(KeySym sym, unsigned mods) {
    Client* c = focused;
    const bool super = (mods & Mod4Mask) != 0;
    const bool alt = (mods & Mod1Mask) != 0;
    const bool shift = (mods & ShiftMask) != 0;

    if (alt && sym == XK_Tab) {
        openAltTab(shift);
        return;
    }
    if (alt && sym == XK_F4) {
        if (c) closeClient(c);
        return;
    }
    if (alt && sym == XK_space) {
        if (c) showContextMenu(c, c->frame.x + 12, c->frame.y + c->captionH + 12);
        return;
    }
    if (!super) return;

    if (sym == XK_Tab) {
        toggleTaskView();
        return;
    }
    if (sym == XK_d || sym == XK_m) {
        toggleShowDesktop();
        return;
    }
    if (sym == XK_r) {
        openStartMenu();
        return;
    }
    if (sym == XK_space) {
        if (c) showContextMenu(c, c->frame.x + 12, c->frame.y + c->captionH + 12);
        return;
    }
    // Win+1..9 address the taskbar buttons, so they work with nothing focused at
    // all -- which is exactly when a pinned launcher still has to be launchable.
    if (sym >= XK_1 && sym <= XK_9) {
        activateTaskItem(int(sym - XK_1));
        return;
    }
    if (!c) return;
    switch (sym) {
        case XK_Left:
            // Snap to the left half; pressing it again restores the window.
            if (shift) snapClient(c, kSnapLeft);
            else snapClient(c, c->snapZone == kSnapLeft ? kSnapNone : kSnapLeft);
            break;
        case XK_Right:
            if (shift) snapClient(c, kSnapRight);
            else snapClient(c, c->snapZone == kSnapRight ? kSnapNone : kSnapRight);
            break;
        case XK_Up:
            setMaximized(c, true, true);
            break;
        case XK_Down:
            if (c->maximizedH || c->maximizedV || c->snapZone != kSnapNone) snapClient(c, kSnapNone);
            else minimizeClient(c, true);
            break;
    }
}

void Manager::handleClientPress(Client* c, int x, int y, unsigned button) {
    (void)x;
    (void)y;
    if (!c || !c->managed) return;
    if (overlayOpen()) {
        closeOverlays();
        return;
    }
    if (button == Button1 || button == Button3) {
        // Click to focus and raise. Mid-drag we are not told about clicks that
        // land on the client, so this is the whole focus story for clients.
        focusClient(c, true);
    }
}

void Manager::handleTaskbarPress(int x, int y, unsigned button) {
    // A pin is being carried, so the button that started it is still down: ignore
    // anything else that lands on the bar until that one is let go.
    if (pinDrag >= 0) return;
    if (startButtonRect.contains(x, y)) {
        if (startOpen) closeOverlays();
        else openStartMenu();
        return;
    }
    if (showDesktopRect.contains(x, y)) {
        toggleShowDesktop();
        return;
    }
    if (clockRect.contains(x, y)) {
        toggleControlCenter();
        return;
    }
    for (size_t i = 0; i < taskItems.size(); ++i) {
        if (!taskItems[i].rect.contains(x, y)) continue;
        // A pinned button right clicks into its own pin/unpin menu, so its
        // button is never the same target as its window button.
        if (taskItems[i].pin >= 0) {
            if (button == Button3) openPinMenu(-1, taskItems[i].pin, x, y - 8);
            // A left press on a pin arms a reorder drag; the launch (or the focus)
            // happens on the release, and only if the pointer never travelled.
            else beginPinDrag(taskItems[i].pin, x);
            return;
        }
        Client* c = taskItems[i].client;
        if (button == Button3 && c) {
            focusClient(c, true);
            showContextMenu(c, x, y - 8);
            return;
        }
        activateTaskbarItem(c);
        return;
    }
}

Client* Manager::clientAt(int x, int y) const {
    for (size_t i = clients.size(); i-- > 0;) {
        Client* c = clients[i].get();
        if (!c->managed || !c->mapped || c->minimized || c->unredirected) continue;
        if (c->frame.contains(x, y)) return c;
    }
    return nullptr;
}

int Manager::hitCaptionButton(const Client* c, int x, int y) const {
    if (!c || !c->managed || c->captionH <= 0 || !c->mapped || c->minimized) return -1;
    const Rect caption{c->frame.x, c->frame.y, c->frame.w, c->captionH};
    if (!caption.contains(x, y)) return -1;
    // Cells are counted from the right edge, exactly like the shader does.
    const int fromRight = c->frame.right() - x;
    if (fromRight <= metrics::kBtnW) return 2;                  // close
    if (fromRight <= 2 * metrics::kBtnW) return 1;              // maximise
    if (fromRight <= 3 * metrics::kBtnW) return 0;              // minimise
    return -1;
}

int Manager::hitTitleBar(Client* c, int x, int y) const {
    if (!c || !c->managed || c->captionH <= 0 || !c->mapped || c->minimized) return 0;
    const Rect caption{c->frame.x, c->frame.y, c->frame.w, c->captionH};
    return caption.contains(x, y) ? 1 : 0;
}

// The invisible resize band around a frame. Maximised windows are not
// resizable, and a snapped window unsnaps as soon as you drag its edge.
int Manager::hitEdge(int x, int y, Client** out) const {
    for (size_t i = clients.size(); i-- > 0;) {
        Client* c = clients[i].get();
        if (!c->managed || !c->mapped || c->minimized || c->fullscreen) continue;
        if (c->maximizedH && c->maximizedV) continue;
        const Rect band = c->frame.inflated(metrics::kResizeEdge);
        if (!band.contains(x, y)) continue;
        if (c->frame.contains(x, y)) continue;  // inside the frame is not an edge
        int edge = 0;
        if (x < c->frame.x) edge |= 1;
        if (x >= c->frame.right()) edge |= 2;
        if (y < c->frame.y) edge |= 4;
        if (y >= c->frame.bottom()) edge |= 8;
        if (!edge) continue;
        if (out) *out = c;
        return edge;
    }
    return 0;
}

int Manager::snapZoneFor(int x, int y) const {
    const int zone = metrics::kSnapZonePx;
    const bool left = x <= zone;
    const bool right = x >= screenW - zone - 1;
    const bool top = y <= zone;
    const bool bottom = y >= screenH - metrics::taskbarH - zone - 1;
    if (left && top) return kSnapTopLeft;
    if (right && top) return kSnapTopRight;
    if (left && bottom) return kSnapBottomLeft;
    if (right && bottom) return kSnapBottomRight;
    if (left) return kSnapLeft;
    if (right) return kSnapRight;
    if (top) return kSnapTop;
    if (bottom) return kSnapBottom;
    return kSnapNone;
}

Client* Manager::chromeAt(int x, int y) const {
    for (size_t i = clients.size(); i-- > 0;) {
        Client* c = clients[i].get();
        if (!c->managed || !c->mapped || c->minimized || c->unredirected) continue;
        if (c->captionH <= 0) continue;
        if (!c->frame.contains(x, y)) continue;
        // Content belongs to the client; only the caption strip is ours.
        if (clientRect(c).contains(x, y)) return nullptr;
        return c;
    }
    return nullptr;
}

// All the interactive chrome of the shell, in one place: the taskbar strip, the
// invisible resize bands, and the captions we draw in the overlay. Both the
// "the click reached the overlay" path and the "the click reached a client that
// was sitting on top of our chrome" path funnel through here.
bool Manager::handleChromePress(int x, int y, unsigned button, Time time) {
    // The taskbar's edge is grabbed before anything else on the bar, because the
    // grip reaches above the bar as well as into it. It is checked ahead of the
    // taskbar strip below for the same reason.
    if (button == Button1 && taskbarGripAt(x, y)) {
        beginTaskbarResize(y);
        return true;
    }
    if (y >= screenH - metrics::taskbarH) {
        handleTaskbarPress(x, y, button);
        return true;
    }

    // The resize bands live just outside the frame, in the overlay.
    Client* edgeClient = nullptr;
    const int edge = hitEdge(x, y, &edgeClient);
    if (edge && edgeClient) {
        focusClient(edgeClient, true);
        if (button == Button1) beginResize(edgeClient, edge, x, y);
        return true;
    }

    Client* c = chromeAt(x, y);
    if (!c) {
        if (traceInput()) log("  chromeAt(%d,%d) -> none", x, y);
        return false;
    }
    const int btn = hitCaptionButton(c, x, y);
    if (traceInput()) {
        log("  chromeAt(%d,%d) -> 0x%lx frame=(%d,%d %dx%d) capH=%d button=%d edge=%d",
            x, y, static_cast<unsigned long>(c->id), c->frame.x, c->frame.y, c->frame.w,
            c->frame.h, c->captionH, btn, edge);
    }
    focusClient(c, true);
    if (button == Button3) {
        showContextMenu(c, x, y);
        return true;
    }
    if (button != Button1) return true;

    if (btn >= 0) {
        c->pressBtn = btn;
        c->hoverBtn = btn;
        dirty = true;
        return true;
    }
    if (hitTitleBar(c, x, y)) {
        const bool doubleClick =
            lastClickClient == c && time >= lastClickTime && time - lastClickTime < 400;
        lastClickClient = c;
        lastClickTime = time;
        if (doubleClick) {
            lastClickClient = nullptr;
            toggleMaximize(c);
            return true;
        }
        beginMove(c, x, y);
    }
    return true;
}

void Manager::handleOverlayPress(int x, int y, unsigned button, Time time) {
    // Control Centre stays open when a control is used, exactly as iOS does;
    // only a press outside the panel dismisses it.
    if (ccOpen) {
        handleControlCenterPress(x, y, button);
        return;
    }
    if (contextOpen) {
        if (button == Button1 && contextRect.contains(x, y)) {
            const int idx = (y - contextRect.y - 6) / 32;
            if (idx >= 0 && idx < int(contextItems.size())) applyContextAction(idx);
        }
        closeOverlays();
        return;
    }
    if (startOpen) {
        // The wheel flips Launchpad pages.
        if (button == Button4 || button == Button5) {
            const int next = startPage + (button == Button5 ? 1 : -1);
            if (next >= 0 && next < startPageCount) {
                startPage = next;
                layoutStartMenu();
                dirty = true;
            }
            return;
        }
        if (button == Button1 || button == Button3) {
            // Tiles answer both buttons: left starts the app, right offers to
            // pin it. Either way the Launchpad gives way to what comes next.
            for (size_t i = 0; i < appRects.size(); ++i) {
                if (!appRects[i].contains(x, y)) continue;
                const size_t gi = startPageBase + i;
                if (gi < appFiltered.size() && appFiltered[gi] < apps.size()) {
                    const size_t ai = appFiltered[gi];
                    if (button == Button3) openPinMenu(int(ai), -1, x, y);
                    else {
                        launchApp(apps[ai].exec);
                        closeOverlays();
                    }
                } else {
                    closeOverlays();
                }
                return;
            }
            if (button != Button1) {
                closeOverlays();
                return;
            }
            for (size_t p = 0; p < appDotRects.size(); ++p) {
                if (!appDotRects[p].contains(x, y)) continue;
                startPage = int(p);
                layoutStartMenu();
                dirty = true;
                return;
            }
            if (searchRect.contains(x, y)) return;  // focus stays in the field
        }
        // Anywhere else (the empty backdrop) dismisses, like macOS.
        closeOverlays();
        return;
    }
    if (altTabOpen) {
        if (button == Button1 && altTabIndex >= 0 && altTabIndex < int(altTabOrder.size())) {
            Client* target = altTabOrder[size_t(altTabIndex)];
            focusClient(target, true);
            restoreClient(target);
        }
        closeOverlays();
        return;
    }
    if (taskViewOpen) {
        if (button == Button1) {
            for (size_t i = 0; i < taskViewRects.size(); ++i) {
                if (!taskViewRects[i].contains(x, y)) continue;
                if (i < altTabOrder.size()) {
                    Client* target = altTabOrder[i];
                    restoreClient(target);
                    focusClient(target, true);
                }
                break;
            }
        }
        closeOverlays();
        return;
    }

    // Desktop widgets sit above the wallpaper icons: a left press grabs one to
    // move or resize, a right press opens the add/remove menu anywhere on the
    // desktop (above the taskbar).
    if (button == Button1 && handleWidgetPress(x, y, time)) return;
    if (button == Button3 && y < screenH - metrics::taskbarH) {
        openDesktopMenu(x, y);
        return;
    }

    // The desktop icons are painted on the wallpaper, below every window and
    // below our own chrome, so this is the only path that reaches them.
    if (button == Button1 && handleDesktopPress(x, y, time)) return;
    handleChromePress(x, y, button, time);
}

// A desktop icon opens on a single click, the way a launcher should; a click on
// bare wallpaper clears the selection.
bool Manager::handleDesktopPress(int x, int y, Time time) {
    (void)time;
    // Test what the user actually sees: mid-reflow the shown cells are the
    // eased positions, not the settled grid targets.
    const std::vector<Rect>& hit =
        desktopIconDraw.size() == desktopIconRects.size() ? desktopIconDraw : desktopIconRects;
    for (size_t i = 0; i < hit.size(); ++i) {
        if (!hit[i].contains(x, y)) continue;
        const int index = int(i);
        selectedDesktopIcon = index;
        hoverDesktopIcon = index;
        if (index < int(desktopItems.size())) {
            // Grow the placeholder out of the icon's own glyph, not its cell, so
            // the animation starts exactly where the user clicked.
            const Rect cell = hit[i];
            const Rect fromIcon{cell.x + (cell.w - 48) / 2, cell.y + 8, 48, 48};
            openDesktopItem(desktopItems[size_t(index)], fromIcon);
        }
        dirty = true;
        return true;
    }
    if (selectedDesktopIcon >= 0) {
        selectedDesktopIcon = -1;
        dirty = true;
    }
    return false;
}

// Opens one desktop item: a launcher runs its Exec, anything else is handed to
// the session's default handler (xdg-open), so folders open in the file manager
// and files in whatever the desktop is configured to use.
void Manager::openDesktopItem(const DesktopItem& item, const Rect& fromIcon) {
    // iOS-style launch: the icon starts growing into a window tile right away, so
    // the double-click feels answered before the process has even been forked.
    beginLaunchAnim(item, fromIcon);
    if (!item.exec.empty()) {
        launchApp(item.exec);
        return;
    }
    if (item.path.empty()) return;
    // Single quote the path for the shell launchApp runs it through.
    std::string quoted = "'";
    for (char ch : item.path) {
        if (ch == '\'') quoted += "'\\''";
        else quoted += ch;
    }
    quoted += "'";
    launchApp("xdg-open " + quoted);
}

// Drag starts. The pointer grab we take here is what makes the drag stay
// smooth: every motion event comes to us (nothing else can steal it) and we
// only ever push one asynchronous move request per event.
void Manager::beginMove(Client* c, int x, int y) {
    if (!c || !c->managed || c->minimized) return;
    // Dragging a maximised or snapped window restores it first, exactly like
    // Windows 11: the window pops back to its old size under the cursor.
    if (c->maximizedH || c->maximizedV || c->snapZone != kSnapNone) {
        const Rect restore = clampRect(c->restore, Rect{0, 0, screenW, screenH});
        const double frac =
            c->frame.w > 0 ? double(x - c->frame.x) / double(c->frame.w) : 0.5;
        c->maximizedH = false;
        c->maximizedV = false;
        c->snapZone = kSnapNone;
        c->frame = restore;
        c->frame.x = x - int(frac * double(restore.w));
        c->frame.y = y - metrics::kCaptionH / 2;
        c->drawFrame = c->frame;
        c->animMs = 0;
        syncClientGeometry(c);
        updateStateAtoms(c);
        updateFullscreenRedirection();
    }
    dragClient = c;
    dragIsMove = true;
    dragEdge = 0;
    dragGrab = Point{x - c->frame.x, y - c->frame.y};
    dragFrameStart = c->frame;
    snapZonePreview = kSnapNone;
    raiseClient(c);
    XGrabPointer(dpy, comp.overlay(), False,
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync,
                 GrabModeAsync, None, None, CurrentTime);
    dirty = true;
}

void Manager::beginResize(Client* c, int edge, int x, int y) {
    if (!c || !c->managed || edge == 0) return;
    if (c->maximizedH && c->maximizedV) return;
    if (c->snapZone != kSnapNone) {
        // Resizing a snapped window first returns it to free floating.
        c->snapZone = kSnapNone;
        updateStateAtoms(c);
    }
    dragClient = c;
    dragIsMove = false;
    dragEdge = edge;
    dragFrameStart = c->frame;
    raiseClient(c);
    XGrabPointer(dpy, comp.overlay(), False,
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync,
                 GrabModeAsync, None, None, CurrentTime);
    dirty = true;
}

void Manager::updateDrag(int x, int y) {
    Client* c = dragClient;
    if (!c || !c->alive) return;

    if (dragIsMove) {
        // Pure translation: the frame moves, the client size never changes, so
        // this costs one XMoveWindow and a re-draw. Nothing else.
        c->frame.x = x - dragGrab.x;
        c->frame.y = y - dragGrab.y;
        // Keep a strip of the caption on screen, like Windows does.
        if (c->frame.y < 0) c->frame.y = 0;
        if (c->frame.x > screenW - 90) c->frame.x = screenW - 90;
        if (c->frame.x + c->frame.w < 90) c->frame.x = 90 - c->frame.w;
        syncClientGeometry(c);
        const int zone = snapZoneFor(x, y);
        if (zone != snapZonePreview) {
            snapZonePreview = zone;
            dirty = true;
        }
    } else {
        int x1 = dragFrameStart.x, y1 = dragFrameStart.y;
        int x2 = dragFrameStart.right(), y2 = dragFrameStart.bottom();
        if (dragEdge & 1) x1 = x;
        if (dragEdge & 2) x2 = x;
        if (dragEdge & 4) y1 = y;
        if (dragEdge & 8) y2 = y;
        // Respect the client's own minimum size.
        const int minW = c->minW + 2 * metrics::kBorder;
        const int minH = c->minH + c->captionH + metrics::kBorder;
        if (x2 - x1 < minW) {
            if (dragEdge & 1) x1 = x2 - minW;
            else x2 = x1 + minW;
        }
        if (y2 - y1 < minH) {
            if (dragEdge & 4) y1 = y2 - minH;
            else y2 = y1 + minH;
        }
        c->frame = Rect{x1, y1, x2 - x1, y2 - y1};
        // syncClientGeometry sees a size change and invalidates the pixmap; the
        // next frame re-binds it, so a resize costs one pixmap per frame rather
        // than one per motion event.
        syncClientGeometry(c);
    }
    dirty = true;
}

void Manager::cancelDrag() {
    if (!dragClient) return;
    dragClient = nullptr;
    dragIsMove = false;
    dragEdge = 0;
    snapZonePreview = kSnapNone;
    XUngrabPointer(dpy, CurrentTime);
    setCursor(0);
    dirty = true;
}

void Manager::endDrag(int x, int y) {
    Client* c = dragClient;
    if (!c) return;
    const bool wasMove = dragIsMove;
    dragClient = nullptr;
    dragIsMove = false;
    dragEdge = 0;
    XUngrabPointer(dpy, CurrentTime);
    setCursor(0);
    dirty = true;
    if (!c->alive) {
        snapZonePreview = kSnapNone;
        return;
    }

    if (wasMove) {
        const int zone = snapZoneFor(x, y);
        snapZonePreview = kSnapNone;
        if (zone != kSnapNone) {
            snapClient(c, zone);
            return;
        }
        c->frame = clampRect(c->frame, Rect{0, 0, screenW, screenH});
        c->drawFrame = c->frame;
        c->animMs = 0;
        syncClientGeometry(c);
        updateStateAtoms(c);
    } else {
        // Bind the pixmap for the final size.
        c->pixW = c->pixH = 0;
        ensurePixmap(c);
        updateStateAtoms(c);
    }
}

}  // namespace wm