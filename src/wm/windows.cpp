// Per client bookkeeping: turning X windows into Clients, reading their
// properties, and the XComposite/DAMAGE plumbing that feeds the GPU.
//
// Nothing in here reparents anything. A managed window stays a child of the
// root; we only redirect it (so the server draws it off screen) and name its
// pixmap so the compositor can sample it as a texture.
#include "manager.h"
#include "xprop.h"

#include <X11/extensions/shape.h>
#include <X11/keysym.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace wm {

Client* Manager::find(Window w) {
    if (!w) return nullptr;
    for (auto& c : clients) {
        if (c->id == w) return c.get();
    }
    return nullptr;
}

Client* Manager::add(Window w, bool existing) {
    if (!dpy || w == 0 || w == root || w == comp.overlay() || w == wmCheckWin) return nullptr;
    if (find(w)) return nullptr;

    XWindowAttributes attr{};
    if (!XGetWindowAttributes(dpy, w, &attr)) return nullptr;
    if (attr.c_class == InputOnly) return nullptr;
    // Override-redirect windows are menus, tooltips, splash screens, drag icons
    // and so on. The server draws those directly on top of our overlay, which
    // is exactly what we want: we simply never touch them.
    if (attr.override_redirect) return nullptr;
    if (attr.depth == 0) return nullptr;

    auto holder = std::make_unique<Client>();
    Client* c = holder.get();
    c->id = w;
    c->visual = attr.visual;
    c->depth = attr.depth;
    c->hasAlpha = attr.depth == 32;
    clients.push_back(std::move(holder));

    int cw = attr.width, ch = attr.height;
    if (cw <= 0) cw = 640;
    if (ch <= 0) ch = 480;
    // The frame is the *requested* client size wrapped in our chrome; the
    // placement pass will decide where it ends up.
    c->restore = Rect{attr.x, attr.y, cw + 2 * metrics::kBorder,
                      ch + metrics::kCaptionH + metrics::kBorder};
    c->frame = c->restore;
    c->drawFrame = c->restore;

    readClassAndPid(c);
    readWindowType(c);
    readNormalHints(c);
    readStruts(c);
    readTitle(c);
    readIcon(c);
    if (!c->managed) {
        c->captionH = 0;
        c->frame = Rect{attr.x, attr.y, attr.width, attr.height};
        c->restore = c->frame;
        settleGeometry(c);
    }

    // Property changes (title, struts, ...) and clicks: clicks are how we do
    // click-to-focus for windows we cannot intercept keyboard focus on.
    XSelectInput(dpy, w, PropertyChangeMask | ButtonPressMask | ButtonReleaseMask);

    const bool viewable = attr.map_state == IsViewable;
    if (existing && c->managed && viewable) {
        mapClient(c);
        // Pre-existing windows appear immediately: no open animation.
        c->appear = 1.0;
        c->vanish = 0.0;
        c->animStart = 0.0;
        c->animMs = 0;
        settleGeometry(c);
        if (!focused && !c->isDock && !c->isDesktop) focusClient(c, false);
    } else if (existing && !c->managed) {
        c->mapped = viewable;
    }

    updateClientList();
    updateWorkArea();
    dirty = true;
    return c;
}

void Manager::removeClient(Client* c) {
    if (!c) return;
    auto it = std::find_if(clients.begin(), clients.end(),
                           [c](const std::unique_ptr<Client>& p) { return p.get() == c; });
    if (it == clients.end()) return;

    if (dragClient == c) {
        dragClient = nullptr;
        dragIsMove = false;
        ungrabPointer();
    }
    if (focused == c) focused = nullptr;
    if (contextClient == c) contextClient = nullptr;
    if (hoverResizeClient == c) hoverResizeClient = nullptr;
    altTabOrder.erase(std::remove(altTabOrder.begin(), altTabOrder.end(), c), altTabOrder.end());

    destroyPixmap(c);
    if (c->iconTex) comp.destroyTexture(c->iconTex);
    if (c->redirected && c->id) XCompositeUnredirectWindow(dpy, c->id, CompositeRedirectManual);

    clients.erase(it);
    updateClientList();
    updateWorkArea();
    if (!focused) focusNext(true);
    dirty = true;
}

void Manager::readClassAndPid(Client* c) {
    XClassHint hint{};
    if (XGetClassHint(dpy, c->id, &hint)) {
        if (hint.res_name) {
            c->appName = hint.res_name;
            std::transform(c->appName.begin(), c->appName.end(), c->appName.begin(),
                           [](unsigned char ch) { return char(std::tolower(ch)); });
            XFree(hint.res_name);
        }
        if (hint.res_class) XFree(hint.res_class);
    }
    unsigned long pid = 0;
    if (cardProperty(dpy, c->id, A.netWmPid, &pid)) c->pid = pid_t(pid);
}

void Manager::readTitle(Client* c) {
    std::string title;
    if (atomName(dpy, c->id, A.netWmName, &title)) {
        c->title = title;
    } else if (atomName(dpy, c->id, A.wmName, &title)) {
        c->title = title;
    } else {
        c->title = c->appName.empty() ? "Window" : c->appName;
    }
    if (c->title.empty()) c->title = "Window";
}

void Manager::readWindowType(Client* c) {
    c->isDock = c->isDesktop = c->isToolbar = c->isMenu = false;
    c->isDialog = c->isSplash = c->isModal = false;

    Atom type = None;
    int format = 0;
    unsigned long count = 0, after = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(dpy, c->id, A.netWmWindowType, 0, 32, False, XA_ATOM, &type, &format,
                           &count, &after, &data) == Success &&
        data && format == 32) {
        const Atom* atoms = reinterpret_cast<const Atom*>(data);
        for (unsigned long i = 0; i < count; ++i) {
            const Atom a = atoms[i];
            if (a == A.typeDock) c->isDock = true;
            else if (a == A.typeDesktop) c->isDesktop = true;
            else if (a == A.typeToolbar) c->isToolbar = true;
            else if (a == A.typeMenu || a == A.typeDropdownMenu || a == A.typePopupMenu)
                c->isMenu = true;
            else if (a == A.typeDialog || a == A.typeUtility || a == A.typeNotification)
                c->isDialog = true;
            else if (a == A.typeSplash) c->isSplash = true;
        }
    }
    if (data) XFree(data);

    c->isModal = windowHasAtom(dpy, c->id, A.netWmState, A.stateModal);
    c->urgent = windowHasAtom(dpy, c->id, A.netWmState, A.stateDemandsAttention);
    const bool skipTaskbar = windowHasAtom(dpy, c->id, A.netWmState, A.stateSkipTaskbar);
    const bool skipPager = windowHasAtom(dpy, c->id, A.netWmState, A.stateSkipPager);

    // Docks and desktop windows belong to the session, not to us: we let the
    // server draw them and only honour their struts. Splash screens and menus
    // are not decorated either.
    c->managed = !c->isDock && !c->isDesktop && !c->isMenu && !c->isSplash;
    c->captionH = c->managed ? metrics::kCaptionH : 0;
    // Force a re-read: readDecorations() owns the frameless decision, and the
    // caption height above just reset it.
    c->frameless = false;
    readDecorations(c);
    c->skipTaskbar = skipTaskbar || c->isDock || c->isDesktop;
    c->skipPager = skipPager || c->skipTaskbar;
    c->fullscreen = windowHasAtom(dpy, c->id, A.netWmState, A.stateFullscreen);
    c->maximizedH = windowHasAtom(dpy, c->id, A.netWmState, A.stateMaximizedHorz);
    c->maximizedV = windowHasAtom(dpy, c->id, A.netWmState, A.stateMaximizedVert);
}

void Manager::readDecorations(Client* c) {
    if (!c) return;
    struct PropMwmHints {
        unsigned long flags;
        unsigned long functions;
        unsigned long decorations;
        long input_mode;
        unsigned long status;
    };
    constexpr unsigned long kHintsDecorations = 1L << 1;
    constexpr unsigned long kDecorAll = 1L << 0;
    constexpr unsigned long kDecorTitle = 1L << 3;

    PropMwmHints hints{};
    Atom type = None;
    int format = 0;
    unsigned long count = 0, after = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(dpy, c->id, A.motifHints, 0, 5, False, A.motifHints, &type, &format,
                           &count, &after, &data) == Success &&
        data && format == 32 && count >= 3) {
        hints.flags = *reinterpret_cast<unsigned long*>(data);
        hints.functions = *reinterpret_cast<unsigned long*>(data + sizeof(long));
        hints.decorations = *reinterpret_cast<unsigned long*>(data + 2 * sizeof(long));
    }
    if (data) XFree(data);

    // Only an explicit request counts. MWM_DECOR_ALL means "give me everything",
    // so a window is frameless when it asks for decorations and clears both the
    // catch-all and the title bar. This is what GTK does for undecorated
    // windows (Electron, Tauri and Electrobun apps all draw their own title bar).
    const bool wantsNone =
        (hints.flags & kHintsDecorations) && (hints.decorations & (kDecorAll | kDecorTitle)) == 0;
    const bool frameless = wantsNone && c->managed;

    if (frameless == c->frameless) return;
    c->frameless = frameless;
    if (!c->fullscreen) c->captionH = frameless ? 0 : metrics::kCaptionH;
    if (c->managed && c->mapped) {
        applyFrame(c, true);
        syncClientGeometry(c);
    }
    dirty = true;
}

void Manager::readNormalHints(Client* c) {
    XSizeHints hints{};
    long supplied = 0;
    if (!XGetWMNormalHints(dpy, c->id, &hints, &supplied)) return;
    if (hints.flags & PMinSize) {
        c->minW = hints.min_width > 1 ? hints.min_width : 1;
        c->minH = hints.min_height > 1 ? hints.min_height : 1;
    }
    if (hints.flags & PMaxSize) {
        c->maxW = hints.max_width;
        c->maxH = hints.max_height;
    }
    // Only honour an explicit position request: most clients create their
    // window at 0,0 and expect the WM to decide where it lives.
    if ((hints.flags & (PPosition | USPosition)) && (hints.x > 0 || hints.y > 0)) {
        c->userPosition = true;
        c->restore.x = hints.x;
        c->restore.y = hints.y;
    }
    // A client that says "input = False" wants WM_TAKE_FOCUS instead of the
    // input focus itself (think on-screen keyboards and input methods).
    if (XWMHints* wm = XGetWMHints(dpy, c->id)) {
        if (wm->flags & InputHint) c->inputHint = wm->input != False;
        XFree(wm);
    }
    Atom* protos = nullptr;
    int n = 0;
    if (XGetWMProtocols(dpy, c->id, &protos, &n)) {
        for (int i = 0; protos && i < n; ++i) {
            if (protos[i] == A.wmTakeFocus) c->takeFocus = true;
        }
        XFree(protos);  // XGetWMProtocols allocates with Xmalloc
    }
}

void Manager::readStruts(Client* c) {
    c->strut[0] = c->strut[1] = c->strut[2] = c->strut[3] = 0;
    for (const Atom prop : {A.netWmStrutPartial, A.netWmStrut}) {
        Atom type = None;
        int format = 0;
        unsigned long count = 0, after = 0;
        unsigned char* data = nullptr;
        if (XGetWindowProperty(dpy, c->id, prop, 0, 12, False, XA_CARDINAL, &type, &format,
                               &count, &after, &data) != Success) {
            continue;
        }
        if (data && format == 32 && count >= 4) {
            const unsigned long* v = reinterpret_cast<const unsigned long*>(data);
            for (int i = 0; i < 4; ++i) c->strut[i] = long(v[i]);
            if (data) XFree(data);
            return;
        }
        if (data) XFree(data);
    }
}

void Manager::readIcon(Client* c) {
    Atom type = None;
    int format = 0;
    unsigned long count = 0, after = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(dpy, c->id, A.netWmIcon, 0, 0x1FFFFF, False, XA_CARDINAL, &type,
                           &format, &count, &after, &data) != Success ||
        !data || format != 32 || count < 2) {
        if (data) XFree(data);
        return;
    }
    const unsigned long* v = reinterpret_cast<const unsigned long*>(data);
    // _NET_WM_ICON is a packed list of (width, height, ARGB pixels...). Take the
    // largest icon that is still a sensible size for a 16px caption glyph.
    unsigned long pos = 0;
    int bestW = 0, bestH = 0;
    std::vector<unsigned long> best;
    while (pos + 2 <= count) {
        const int w = int(v[pos]);
        const int h = int(v[pos + 1]);
        if (w <= 0 || h <= 0 || pos + 2 + static_cast<unsigned long>(w) * h > count) break;
        if (w > bestW && w <= 128) {
            bestW = w;
            bestH = h;
            best.assign(v + pos + 2, v + pos + 2 + static_cast<unsigned long>(w) * h);
        }
        pos += 2 + static_cast<unsigned long>(w) * h;
    }
    XFree(data);
    if (bestW <= 0 || best.empty()) return;

    std::vector<unsigned char> rgba(static_cast<size_t>(bestW) * bestH * 4u);
    for (size_t i = 0; i < best.size(); ++i) {
        const unsigned long p = best[i];
        rgba[i * 4 + 0] = static_cast<unsigned char>((p >> 16) & 0xFF);  // R
        rgba[i * 4 + 1] = static_cast<unsigned char>((p >> 8) & 0xFF);   // G
        rgba[i * 4 + 2] = static_cast<unsigned char>(p & 0xFF);          // B
        rgba[i * 4 + 3] = static_cast<unsigned char>((p >> 24) & 0xFF);  // A
    }
    comp.uploadTexture(&c->iconTex, rgba.data(), bestW, bestH);
    c->iconW = bestW;
    c->iconH = bestH;
}

void Manager::readAlpha(Client* c) {
    // A 32 bit visual means the client may draw with per pixel alpha.
    c->hasAlpha = c->depth == 32;
}

void Manager::readOpacity(Client* c) {
    // _NET_WM_OPACITY: a client asking for translucency must never be handed
    // back to the server unredirected, or it would suddenly be opaque.
    unsigned long value = 0;
    if (cardProperty(dpy, c->id, A.netWmOpacity, &value) && value < 0xFFFFFFFFul) {
        c->hasAlpha = true;
    }
}

void Manager::updateStateAtoms(Client* c) {
    if (!c || !c->id || !c->alive || !c->managed) return;
    Atom states[8];
    int n = 0;
    if (c->maximizedV) states[n++] = A.stateMaximizedVert;
    if (c->maximizedH) states[n++] = A.stateMaximizedHorz;
    if (c->fullscreen) states[n++] = A.stateFullscreen;
    if (c->minimized) states[n++] = A.stateHidden;
    if (c->focused) states[n++] = A.stateFocused;
    if (c->urgent) states[n++] = A.stateDemandsAttention;
    if (n) {
        XChangeProperty(dpy, c->id, A.netWmState, XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(states), n);
    } else {
        XDeleteProperty(dpy, c->id, A.netWmState);
    }
}

// The heart of the "any X11 program" promise: name the window's pixmap and hand
// it to GLX as a texture. No client involvement, no reparenting, no copying.
void Manager::ensurePixmap(Client* c) {
    if (!c || !c->alive || !c->mapped || !c->redirected || c->unredirected) return;
    const Rect r = clientRect(c);
    if (r.w <= 0 || r.h <= 0) return;
    if (c->namedPixmap && c->tex.valid() && c->pixW == r.w && c->pixH == r.h) return;

    destroyPixmap(c);

    // The window must be viewable and redirected: if the picture is gone the
    // server answers with an error and a *garbage* pixmap id, and feeding that
    // to GLX takes the whole compositor down. Ask, verify, and retry later.
    g_lastError = 0;
    Pixmap windowPixmap = XCompositeNameWindowPixmap(dpy, c->id);
    XSync(dpy, False);
    if (g_lastError != 0 || !windowPixmap) {
        if (c->needsRepaint) dirty = true;
        return;  // not viewable yet; we will be called again
    }
    c->namedPixmap = windowPixmap;
    c->pixW = r.w;
    c->pixH = r.h;

    GLuint tex = comp.bindPixmap(windowPixmap, XVisualIDFromVisual(c->visual), c->depth, r.w,
                                 r.h, c->hasAlpha);
    if (!tex) {
        // The window's visual has no matching GLX config (some 8/16 bit or
        // exotic visuals). XRender it into a pixmap of our own visual instead.
        c->copyPixmap = XCreatePixmap(dpy, root, unsigned(r.w), unsigned(r.h),
                                      unsigned(rootDepth));
        if (!renderOk || !c->copyPixmap || !refreshCopy(c)) {
            destroyPixmap(c);
            return;
        }
        c->usingCopy = true;
        tex = comp.bindPixmap(c->copyPixmap, XVisualIDFromVisual(rootVisual), rootDepth, r.w,
                              r.h, false);
        if (!tex) {
            destroyPixmap(c);
            return;
        }
    }
    c->tex = WindowTex{tex, r.w, r.h, c->hasAlpha && !c->usingCopy};

    // Damage: tell me when this window's pixels change and I only re-draw then.
    if (damageOk && !c->damage) {
        c->damage = XDamageCreate(dpy, c->id, XDamageReportNonEmpty);
    }
    c->needsRepaint = true;
    dirty = true;
}

bool Manager::refreshCopy(Client* c) {
    if (!c || !c->namedPixmap || !c->copyPixmap) return false;
    XRenderPictFormat* srcFormat = XRenderFindVisualFormat(dpy, c->visual);
    XRenderPictFormat* dstFormat = XRenderFindVisualFormat(dpy, rootVisual);
    if (!srcFormat || !dstFormat) return false;
    if (!c->srcPic) {
        c->srcPic = XRenderCreatePicture(dpy, c->namedPixmap, srcFormat, 0, nullptr);
    }
    if (!c->copyDstPic) {
        c->copyDstPic = XRenderCreatePicture(dpy, c->copyPixmap, dstFormat, 0, nullptr);
    }
    if (!c->srcPic || !c->copyDstPic) return false;
    const int w = c->pixW > 0 ? c->pixW : 1;
    const int h = c->pixH > 0 ? c->pixH : 1;
    XRenderComposite(dpy, PictOpSrc, c->srcPic, None, c->copyDstPic, 0, 0, 0, 0, 0, 0, w, h);
    return true;
}

void Manager::destroyPixmap(Client* c) {
    if (!c) return;
    if (c->damage) {
        XDamageDestroy(dpy, c->damage);
        c->damage = 0;
    }
    if (c->tex.tex) {
        comp.releasePixmap(c->tex.tex);
        c->tex = WindowTex{};
    }
    if (c->srcPic) {
        XRenderFreePicture(dpy, c->srcPic);
        c->srcPic = 0;
    }
    if (c->copyDstPic) {
        XRenderFreePicture(dpy, c->copyDstPic);
        c->copyDstPic = 0;
    }
    if (c->copyPixmap) {
        XFreePixmap(dpy, c->copyPixmap);
        c->copyPixmap = 0;
    }
    // namedPixmap came from XCompositeNameWindowPixmap(), so the Composite
    // extension owns it: freeing it here leaves the server handing the same id
    // out again, which is how you get RenderBadPicture/BadPixmap a few frames
    // later. Just forget it; the extension drops it when the window goes away.
    c->namedPixmap = 0;
    c->usingCopy = false;
    c->pixW = c->pixH = 0;
}

}  // namespace wm