// The Wayland compositor core: backend, outputs, xdg-shell policy and the frame
// loop. The shell drawing itself is the shared draw.cpp/tablet.cpp/widgets.cpp
// code, unchanged; this file supplies the platform it runs on.
#include "manager.h"

#include "wlr.h"
#include <xkbcommon/xkbcommon.h>

#include <linux/input-event-codes.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "apps.h"
#include "png.h"

namespace wm {
namespace {

Window nextWindowId = 1;

// X11 modifier bits, so the shared shortcut table keeps its spelling.
constexpr unsigned kShiftMask = 1u << 0;
constexpr unsigned kLockMask = 1u << 1;
constexpr unsigned kControlMask = 1u << 2;
constexpr unsigned kMod1Mask = 1u << 3;
constexpr unsigned kMod4Mask = 1u << 6;

unsigned xModifiers(uint32_t wlrMods) {
    unsigned mods = 0;
    if (wlrMods & WLR_MODIFIER_SHIFT) mods |= kShiftMask;
    if (wlrMods & WLR_MODIFIER_CAPS) mods |= kLockMask;
    if (wlrMods & WLR_MODIFIER_CTRL) mods |= kControlMask;
    if (wlrMods & WLR_MODIFIER_ALT) mods |= kMod1Mask;
    if (wlrMods & WLR_MODIFIER_LOGO) mods |= kMod4Mask;
    return mods;
}

}  // namespace

Manager::~Manager() { shutdown(); }

Client::~Client() {
    if (!listenersAdded) return;
    wl_list_remove(&surfaceCommit.link);
    wl_list_remove(&surfaceDestroy.link);
    wl_list_remove(&surfaceMap.link);
    wl_list_remove(&surfaceUnmap.link);
    wl_list_remove(&toplevelDestroy.link);
    wl_list_remove(&toplevelRequestMove.link);
    wl_list_remove(&toplevelRequestResize.link);
    wl_list_remove(&toplevelRequestMaximize.link);
    wl_list_remove(&toplevelRequestFullscreen.link);
    wl_list_remove(&toplevelRequestMinimize.link);
    wl_list_remove(&toplevelTitle.link);
    wl_list_remove(&toplevelAppId.link);
    wl_list_remove(&toplevelClose.link);
    wl_list_remove(&toplevelShowMenu.link);
}

void Manager::removeClient(Client* c) {
    if (!c) return;
    size_t index = clients.size();
    for (size_t i = 0; i < clients.size(); ++i) {
        if (clients[i].get() == c) {
            index = i;
            break;
        }
    }
    if (index == clients.size()) return;
    if (focused == c) focused = nullptr;
    if (dragClient == c) {
        dragClient = nullptr;
        dragIsMove = false;
        dragEdge = 0;
    }
    if (settingsClient == c) settingsClient = nullptr;
    if (contextClient == c) contextClient = nullptr;
    destroyPixmap(c);
    clients.erase(clients.begin() + static_cast<ptrdiff_t>(index));
    altTabOrder.erase(std::remove(altTabOrder.begin(), altTabOrder.end(), c),
                      altTabOrder.end());
    tabletSwitchOrder.erase(std::remove(tabletSwitchOrder.begin(), tabletSwitchOrder.end(), c),
                            tabletSwitchOrder.end());
    dirty = true;
    if (!focused) focusNext(true);
}

Client* Manager::addInternalWindow(const std::string& title, int x, int y, int frameW,
                                   int frameH) {
    auto holder = std::make_unique<Client>();
    Client* c = holder.get();
    c->manager = this;
    c->id = nextWindowId++;
    c->alive = true;
    c->managed = true;
    c->mapped = true;
    c->internalSettings = true;
    c->title = title;
    c->appName = "win11wm-settings";
    c->captionH = metrics::kCaptionH;
    c->frame = Rect{x, y, frameW, frameH};
    c->restore = c->frame;
    c->drawFrame = c->frame;
    c->appear = 1.0;
    c->animMs = 0;
    clients.push_back(std::move(holder));
    settleGeometry(c);
    raiseClient(c);
    return c;
}

void Manager::stop() {
    running = false;
    if (wlDisplay) wl_display_terminate(wlDisplay);
}

void Manager::requestFrame() {
    dirty = true;
    if (!output) return;
    // While the swapchain is starved there is no point asking for a frame: the
    // render pass would fail again. The tick timer retries once the backoff has
    // expired, so nothing is lost.
    if (nowMs() < frameBackoffUntil) return;
    wlr_output_schedule_frame(output);
}

void Manager::shutdown() {
    if (tickTimer) {
        wl_event_source_remove(tickTimer);
        tickTimer = nullptr;
    }
    if (wlDisplay) {
        wl_display_destroy_clients(wlDisplay);
        wl_display_destroy(wlDisplay);
        wlDisplay = nullptr;
    }
    running = false;
}

// --------------------------------------------------------------- client table
Client* Manager::find(Window id) {
    if (!id) return nullptr;
    for (auto& c : clients) {
        if (c->id == id) return c.get();
    }
    return nullptr;
}

void Manager::updateClientList() {}
void Manager::updateWorkArea() {}
void Manager::scanExistingWindows() {}

void Manager::setCursor(int which) {
    if (!cursor || !cursorManager) return;
    static const char* const names[7] = {"left_ptr", "text", "sb_h_double_arrow",
                                         "sb_v_double_arrow", "fd_diagonal",
                                         "bd_diagonal", "hand2"};
    if (which < 0 || which > 6) which = 0;
    if (cursorShown == which) return;
    cursorShown = which;
    wlr_cursor_set_xcursor(cursor, cursorManager, names[which]);
}

void Manager::grabPointer() {}
void Manager::ungrabPointer() {}
void Manager::updateFullscreenRedirection() {}

void Manager::updateStateAtoms(Client* c) {
    (void)c;
}

// --------------------------------------------------------------- geometry
Rect Manager::clientRect(const Client* c) const {
    const int capH = c->captionH > 0 ? c->captionH : 0;
    return Rect{c->frame.x + metrics::kBorder, c->frame.y + capH,
                c->frame.w - 2 * metrics::kBorder, c->frame.h - capH - metrics::kBorder};
}

void Manager::syncClientGeometry(Client* c) {
    if (!c || !c->alive || !c->toplevel) return;
    if (c->mapped || c->configured) {
        const Rect r = clientRect(c);
        const int cw = std::max(1, r.w);
        const int ch = std::max(1, r.h);
        if (cw != c->pendingW || ch != c->pendingH) {
            c->pendingW = cw;
            c->pendingH = ch;
            wlr_xdg_toplevel_set_size(c->toplevel, cw, ch);
        }
    }
    if (!c->mapped) return;
    c->needsRepaint = true;
    c->pixW = 0;
    c->pixH = 0;
}

void Manager::ensurePixmap(Client* c) {
    if (!c || !c->surface) return;
    if (c->tex.valid() && c->pixW == c->tex.w && c->pixH == c->tex.h) return;
    struct wlr_buffer* buffer = &c->surface->buffer->base;
    if (!buffer) return;
    if (!comp.importBuffer(buffer, &c->tex)) {
        c->pixW = c->pixH = 0;
        return;
    }
    c->pixW = c->tex.w;
    c->pixH = c->tex.h;
    c->bufferW = c->tex.w;
    c->bufferH = c->tex.h;
    c->hasAlpha = c->tex.alpha;
    c->needsRepaint = false;
    if (std::getenv("WIN11WM_TEXDEBUG")) {
        log("TEXDBG %s buf=%dx%d tex=%dx%d surf=%dx%d scale=%.2f alpha=%d",
            c->appName.c_str(), c->surface->buffer->base.width,
            c->surface->buffer->base.height, c->tex.w, c->tex.h,
            c->surface->current.width, c->surface->current.height,
            (double)c->surface->current.scale, c->tex.alpha ? 1 : 0);
    }
}

void Manager::destroyPixmap(Client* c) {
    if (!c) return;
    comp.releaseWindowTex(&c->tex);
    c->pixW = c->pixH = 0;
}

// --------------------------------------------------------------- policy
void Manager::mapClient(Client* c) {
    if (!c || !c->managed || c->mapped) return;
    c->mapped = true;
    c->alive = true;
    c->closing = false;
    c->vanish = 0.0;
    c->appear = std::max(c->appear, 0.0);
    c->animFrom = c->frame;
    c->animStart = nowMs();
    c->animMs = metrics::kZoomMs;
    // A client needs wl_surface.enter to know which output it is on; browsers in
    // particular use it to pick their scale and refresh rate.
    if (output && c->surface) wlr_surface_send_enter(c->surface, output);
    syncClientGeometry(c);
    raiseClient(c);
    focusClient(c, false);
    dirty = true;
    requestFrame();
}

void Manager::unmapClient(Client* c) {
    if (!c) return;
    c->mapped = false;
    destroyPixmap(c);
    if (focused == c) {
        c->focused = false;
        focused = nullptr;
        focusNext(true);
    }
    dirty = true;
    requestFrame();
}

void Manager::raiseClient(Client* c) {
    if (!c) return;
    if (!clients.empty() && clients.back().get() == c) return;
    for (size_t i = 0; i < clients.size(); ++i) {
        if (clients[i].get() != c) continue;
        auto holder = std::move(clients[i]);
        clients.erase(clients.begin() + static_cast<ptrdiff_t>(i));
        clients.push_back(std::move(holder));
        break;
    }
    restack();
    dirty = true;
}

void Manager::restack() {
    // Wayland stacking is simply the order of the client list: painting walks it
    // bottom to top and hit testing walks it top to bottom, so there is nothing
    // else to push to the server.
    dirty = true;
}

void Manager::focusClient(Client* c, bool raise) {
    if (!c || !c->mapped) return;
    if (raise) raiseClient(c);
    if (focused == c) return;
    if (focused) focused->focused = false;
    focused = c;
    c->focused = true;
    if (c->toplevel) wlr_xdg_toplevel_set_activated(c->toplevel, true);
    if (seat && c->surface) {
        wlr_seat_keyboard_notify_enter(seat, c->surface, nullptr, 0, nullptr);
    }
    dirty = true;
    requestFrame();
}

void Manager::focusNext(bool forward) {
    if (clients.empty()) {
        if (seat) wlr_seat_keyboard_clear_focus(seat);
        focused = nullptr;
        return;
    }
    std::vector<Client*> order;
    for (auto& c : clients) {
        if (c->mapped && !c->minimized && c->managed) order.push_back(c.get());
    }
    if (order.empty()) {
        focused = nullptr;
        return;
    }
    int idx = -1;
    for (size_t i = 0; i < order.size(); ++i) {
        if (order[i] == focused) idx = int(i);
    }
    if (idx < 0) idx = int(order.size()) - 1;
    const int n = int(order.size());
    idx = forward ? (idx + 1) % n : (idx - 1 + n) % n;
    focusClient(order[size_t(idx)], true);
}

Client* Manager::activeClient() const { return focused; }

int Manager::openWindows() const {
    int n = 0;
    for (auto& c : clients) {
        if (c->mapped && !c->minimized && c->managed) ++n;
    }
    return n;
}

void Manager::closeClient(Client* c) {
    if (!c || !c->alive || c->closing) return;
    if (c->internalSettings) {
        closeSettingsWindow();
        return;
    }
    if (c->toplevel) wlr_xdg_toplevel_send_close(c->toplevel);
    c->closing = true;
    c->animFrom = c->drawFrame;
    c->animStart = nowMs();
    c->animMs = c->tabletApp ? metrics::kTabletCloseMs : metrics::kAnimMs;
    c->closeDeadline = nowMs() + 2500.0;
    dirty = true;
}

void Manager::applyFrame(Client* c, bool animate) {
    if (!c) return;
    if (!animate) settleGeometry(c);
    else if (!c->geoLive) {
        settleGeometry(c);
        c->geoSpring = true;
    } else {
        c->geoSpring = true;
    }
    c->animFrom = c->drawFrame;
    c->animStart = nowMs();
    c->animMs = metrics::kZoomMs;
    syncClientGeometry(c);
    dirty = true;
}

void Manager::restoreClient(Client* c) {
    if (!c || !c->minimized) return;
    c->minimized = false;
    c->pixW = c->pixH = 0;
    c->mapped = true;
    ensurePixmap(c);
    dirty = true;
}

// --------------------------------------------------------------- xdg shell
void Manager::onNewToplevel(struct wlr_xdg_toplevel* toplevel) {
    struct wlr_xdg_surface* xs = toplevel->base;
    if (!xs || !xs->surface) return;
    if (find(Window(reinterpret_cast<uintptr_t>(xs)))) return;

    auto holder = std::make_unique<Client>();
    Client* c = holder.get();
    c->id = nextWindowId++;
    c->alive = true;
    c->managed = true;
    c->xdg = xs;
    c->toplevel = toplevel;
    c->surface = xs->surface;
    c->appId = toplevel->app_id ? toplevel->app_id : "";
    c->title = toplevel->title && toplevel->title[0] ? toplevel->title : "Window";
    c->appName = c->appId;
    std::transform(c->appName.begin(), c->appName.end(), c->appName.begin(),
                   [](unsigned char ch) { return char(std::tolower(ch)); });
    c->captionH = metrics::kCaptionH;
    c->minW = 1;
    c->minH = 1;
    c->pendingW = 800;
    c->pendingH = 600;
    c->restore = Rect{0, 0, 800 + 2 * metrics::kBorder,
                     600 + metrics::kCaptionH + metrics::kBorder};
    c->frame = c->restore;
    c->drawFrame = c->restore;
    c->hasAlpha = true;
    clients.push_back(std::move(holder));

    // Listen to the surface and toplevel for the lifetime of the client. The
    // listeners recover the Client with wl_container_of and then reach the
    // compositor through Client::manager.
    c->manager = this;
    c->surfaceCommit.notify = [](struct wl_listener* l, void*) {
        Client* cl = wl_container_of(l, cl, surfaceCommit);
        if (cl && cl->manager) cl->manager->onSurfaceCommit(cl);
    };
    wl_signal_add(&xs->surface->events.commit, &c->surfaceCommit);

    c->surfaceDestroy.notify = [](struct wl_listener* l, void*) {
        Client* cl = wl_container_of(l, cl, surfaceDestroy);
        if (cl && cl->manager) cl->manager->onToplevelDestroy(cl);
    };
    wl_signal_add(&xs->surface->events.destroy, &c->surfaceDestroy);

    c->surfaceMap.notify = [](struct wl_listener* l, void*) {
        Client* cl = wl_container_of(l, cl, surfaceMap);
        if (cl && cl->manager) cl->manager->mapClient(cl);
    };
    wl_signal_add(&xs->surface->events.map, &c->surfaceMap);

    c->surfaceUnmap.notify = [](struct wl_listener* l, void*) {
        Client* cl = wl_container_of(l, cl, surfaceUnmap);
        if (cl && cl->manager) cl->manager->unmapClient(cl);
    };
    wl_signal_add(&xs->surface->events.unmap, &c->surfaceUnmap);

    c->toplevelDestroy.notify = [](struct wl_listener* l, void*) {
        Client* cl = wl_container_of(l, cl, toplevelDestroy);
        if (cl && cl->manager) cl->manager->onToplevelDestroy(cl);
    };
    wl_signal_add(&toplevel->events.destroy, &c->toplevelDestroy);

    c->toplevelTitle.notify = [](struct wl_listener* l, void*) {
        Client* cl = wl_container_of(l, cl, toplevelTitle);
        if (cl && cl->toplevel && cl->toplevel->title) cl->title = cl->toplevel->title;
    };
    wl_signal_add(&toplevel->events.set_title, &c->toplevelTitle);

    c->toplevelAppId.notify = [](struct wl_listener* l, void*) {
        Client* cl = wl_container_of(l, cl, toplevelAppId);
        if (cl && cl->toplevel && cl->toplevel->app_id) cl->appId = cl->toplevel->app_id;
    };
    wl_signal_add(&toplevel->events.set_app_id, &c->toplevelAppId);

    c->toplevelRequestMove.notify = [](struct wl_listener* l, void*) {
        Client* cl = wl_container_of(l, cl, toplevelRequestMove);
        if (cl && cl->manager) cl->manager->beginInteractiveMove(cl);
    };
    wl_signal_add(&toplevel->events.request_move, &c->toplevelRequestMove);

    c->toplevelRequestResize.notify = [](struct wl_listener* l, void* data) {
        Client* cl = wl_container_of(l, cl, toplevelRequestResize);
        auto* ev = static_cast<struct wlr_xdg_toplevel_resize_event*>(data);
        if (!cl || !cl->manager) return;
        int edge = 0;
        if (ev->edges & WLR_EDGE_LEFT) edge |= 1;
        if (ev->edges & WLR_EDGE_RIGHT) edge |= 2;
        if (ev->edges & WLR_EDGE_TOP) edge |= 4;
        if (ev->edges & WLR_EDGE_BOTTOM) edge |= 8;
        if (edge) cl->manager->beginResize(cl, edge, int(cl->manager->pointerX),
                                           int(cl->manager->pointerY));
    };
    wl_signal_add(&toplevel->events.request_resize, &c->toplevelRequestResize);

    c->toplevelRequestMaximize.notify = [](struct wl_listener* l, void*) {
        Client* cl = wl_container_of(l, cl, toplevelRequestMaximize);
        if (cl && cl->manager && cl->toplevel)
            cl->manager->setMaximized(cl, cl->toplevel->requested.maximized,
                                      cl->toplevel->requested.maximized);
    };
    wl_signal_add(&toplevel->events.request_maximize, &c->toplevelRequestMaximize);

    c->toplevelRequestFullscreen.notify = [](struct wl_listener* l, void*) {
        Client* cl = wl_container_of(l, cl, toplevelRequestFullscreen);
        if (cl && cl->manager && cl->toplevel)
            cl->manager->setFullscreen(cl, cl->toplevel->requested.fullscreen);
    };
    wl_signal_add(&toplevel->events.request_fullscreen, &c->toplevelRequestFullscreen);

    c->toplevelRequestMinimize.notify = [](struct wl_listener* l, void*) {
        Client* cl = wl_container_of(l, cl, toplevelRequestMinimize);
        if (cl && cl->manager) cl->manager->minimizeClient(cl, true);
    };
    wl_signal_add(&toplevel->events.request_minimize, &c->toplevelRequestMinimize);

    c->toplevelShowMenu.notify = [](struct wl_listener* l, void* data) {
        Client* cl = wl_container_of(l, cl, toplevelShowMenu);
        auto* ev = static_cast<struct wlr_xdg_toplevel_show_window_menu_event*>(data);
        if (cl && cl->manager) cl->manager->showContextMenu(cl, ev->x, ev->y);
    };
    wl_signal_add(&toplevel->events.request_show_window_menu, &c->toplevelShowMenu);
    c->listenersAdded = true;

    // The first configure has to wait for the client's initial commit: xdg-shell
    // only accepts one once the surface is initialized, and a configure before
    // that leaves some clients waiting forever for a size they can use.
    placeNewClient(c);
    dirty = true;
}

void Manager::onSurfaceCommit(Client* c) {
    if (!c || !c->surface || !c->xdg || !c->xdg->initialized) return;

    if (!c->configured) {
        // The surface is initialized now, so it can take its first configure:
        // the size the Fluent frame leaves for the client's content.
        c->configured = true;
        const Rect r = clientRect(c);
        c->pendingW = std::max(1, r.w);
        c->pendingH = std::max(1, r.h);
        wlr_xdg_toplevel_set_size(c->toplevel, c->pendingW, c->pendingH);
        dirty = true;
        requestFrame();
        return;
    }

    // Re-import on every commit: the surface points at a new buffer, and the
    // one we were sampling is the client's to recycle.
    c->pixW = c->pixH = 0;
    ensurePixmap(c);
    if (!c->mapped) mapClient(c);
    dirty = true;
    requestFrame();
}

// wlr_surface_for_each_surface reports each surface of a tree (the toplevel
// itself first, then its subsurfaces and popups) with its offset from the root,
// in surface-local pixels. The toplevel is already on screen through the window
// shader, so only the children are blitted here.
struct SurfacePass {
    Manager* mgr = nullptr;
    struct wlr_surface* root = nullptr;
    Rect frame{};
    Rect content{};
    float radius = 0.f;
    float opacity = 1.f;
    bool maximized = false;
};

void Manager::surfaceDrawIterator(struct wlr_surface* surface, int sx, int sy, void* data) {
    auto* pass = static_cast<SurfacePass*>(data);
    if (!surface || surface == pass->root || !surface->buffer) return;
    if (surface->current.width <= 0 || surface->current.height <= 0) return;
    if (std::getenv("WIN11WM_TEXDEBUG"))
        log("SUBDBG surf=%dx%d at %d,%d scale=%d buf=%dx%d", surface->current.width,
            surface->current.height, sx, sy, surface->current.scale,
            surface->buffer ? surface->buffer->base.width : 0,
            surface->buffer ? surface->buffer->base.height : 0);
    WindowTex t;
    if (!pass->mgr->comp.importBuffer(&surface->buffer->base, &t)) return;
    const Rect dst{pass->content.x + sx, pass->content.y + sy,
                   surface->current.width, surface->current.height};
    pass->mgr->comp.drawWindowContent(t, dst, pass->frame, pass->content, pass->radius,
                                      pass->maximized, pass->opacity);
    pass->mgr->comp.releaseWindowTex(&t);
}

void Manager::drawClientSubsurfaces(Client* c, const Rect& frame, const Rect& content,
                                   float radius, bool maximized, float opacity) {
    if (!c || !c->surface || !c->mapped || opacity <= 0.01f) return;
    if (content.w <= 0 || content.h <= 0) return;
    SurfacePass pass;
    pass.mgr = this;
    pass.root = c->surface;
    pass.frame = frame;
    pass.content = content;
    pass.radius = radius;
    pass.opacity = opacity;
    pass.maximized = maximized;
    if (std::getenv("WIN11WM_TEXDEBUG")) {
        log("SUBPASS %s frame=%d,%d %dx%d content=%d,%d %dx%d toplevel=%dx%d pending=%dx%d",
            c->appName.c_str(), frame.x, frame.y, frame.w, frame.h, content.x, content.y,
            content.w, content.h, c->surface->current.width, c->surface->current.height,
            c->pendingW, c->pendingH);
    }
    wlr_surface_for_each_surface(c->surface, surfaceDrawIterator, &pass);
}

void Manager::onToplevelDestroy(Client* c) {
    if (!c) return;
    c->alive = false;
    removeClient(c);
    dirty = true;
    requestFrame();
}

void Manager::onNewPopup(struct wlr_xdg_popup* popup) {
    if (!popup) return;
    popup->base->data = this;
    // Popups are part of their parent's surface tree; the renderer walks the
    // parent's buffer for now, and input goes to the popup grab that xdg-shell
    // installs itself.
    (void)popup;
}

void Manager::beginInteractiveMove(Client* c) {
    if (!c) return;
    beginMove(c, pointerX, pointerY);
}

void Manager::onRequestMove(Client*, struct wlr_seat_pointer_request_set_cursor_event*) {}

// --------------------------------------------------------------- output
namespace {
// Reads the output framebuffer back into a binary PPM. The GL framebuffer's
// first row is the top of the screen (that is the orientation the output pass
// renders in), so no flip is needed.
void dumpFrameToFile(const std::string& path, int w, int h) {
    std::vector<unsigned char> rgba(size_t(w) * size_t(h) * 4u);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (size_t i = 0; i < rgba.size(); i += 4) {
        std::fputc(rgba[i], f);
        std::fputc(rgba[i + 1], f);
        std::fputc(rgba[i + 2], f);
    }
    std::fclose(f);
    log("wrote %s (%dx%d)", path.c_str(), w, h);
}
}  // namespace

void Manager::renderFrameOnOutput() {
    static int dbgAttempts, dbgFails, dbgCommitFails; static double dbgLast;
    ++dbgAttempts;
    if (nowMs() - dbgLast > 1000.0) {
        dbgLast = nowMs();
        log("DBG frame attempts=%d fails=%d commitfail=%d swap=%p starve=%d backoff=%.0f",
            dbgAttempts, dbgFails, dbgCommitFails, (void*)(output ? output->swapchain : nullptr),
            frameStarveCount, frameBackoffUntil - nowMs());
        dbgAttempts = 0; dbgFails = 0; dbgCommitFails = 0;
    }
    if (!output || !output->enabled) return;
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    struct wlr_render_pass* pass = wlr_output_begin_render_pass(output, &state, nullptr, nullptr);
    if (!pass) {
        wlr_output_state_finish(&state);
        // The host compositor has not released an output buffer yet (it is
        // still compositing the previous one, or our window is not visible).
        // Back off for a frame, exponentially, so a starved swapchain costs a
        // handful of attempts instead of a flood of failed render passes.
        ++dbgFails;
        ++frameStarveCount;
        const int shift = std::min(frameStarveCount, 4);
        frameBackoffUntil = nowMs() + double(16 << shift);
        return;
    }
    frameStarveCount = 0;
    tickAnimations(nowMs());
    render();
    comp.present();
    if (!opts->dumpPath.empty() && opts->frames > 0 && framesRendered + 1 >= opts->frames)
        dumpFrameToFile(opts->dumpPath, screenW, screenH);
    wlr_render_pass_submit(pass);
    if (!wlr_output_commit_state(output, &state)) {
        ++dbgCommitFails;
        // Nothing was presented, so do not count it as a frame and let the
        // backoff throttle the retry.
        wlr_output_state_finish(&state);
        frameBackoffUntil = nowMs() + 16.0;
        return;
    }
    wlr_output_state_finish(&state);

    // A client only draws again once its frame callback has fired, so every
    // surface that was on screen this frame has to be told the frame is done.
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    for (auto& cp : clients) {
        if (cp->mapped && cp->surface) wlr_surface_send_frame_done(cp->surface, &now);
    }

    ++framesRendered;
    dirty = false;

    if (!pendingLaunches.empty()) {
        for (const std::string& cmd : pendingLaunches) launchApp(cmd);
        pendingLaunches.clear();
    }
    if (opts && opts->frames > 0 && framesRendered >= opts->frames) stop();
}

// --------------------------------------------------------------- run
int Manager::run(const Options& options) {
    opts = &options;
    nested = !options.headless && !options.drm;
    wlr_log_init(std::getenv("WIN11WM_DEBUG") ? WLR_DEBUG : WLR_ERROR, nullptr);

    wlDisplay = wl_display_create();
    if (!wlDisplay) {
        log("cannot create the Wayland display");
        return 1;
    }
    struct wl_event_loop* evloop = wl_display_get_event_loop(wlDisplay);

    // Backend: nested inside the host Wayland server by default (that is what a
    // development run uses), headless for scripted frame dumps, and the
    // session/DRM autodetection otherwise.
    if (options.headless) {
        backend = wlr_headless_backend_create(evloop);
        nested = false;
    } else if (options.drm) {
        backend = wlr_backend_autocreate(evloop, nullptr);
        nested = false;
    } else if (std::getenv("WAYLAND_DISPLAY")) {
        backend = wlr_wl_backend_create(evloop, nullptr);
        nested = true;
    } else {
        backend = wlr_backend_autocreate(evloop, nullptr);
        nested = false;
    }
    if (!backend) {
        log("cannot create a wlroots backend");
        return 1;
    }

    renderer = wlr_renderer_autocreate(backend);
    if (!renderer) {
        log("cannot create a renderer (no GPU and no software renderer allowed)");
        return 1;
    }
    allocator = wlr_allocator_autocreate(backend, renderer);
    if (!allocator) {
        log("cannot create an allocator");
        return 1;
    }
    wlr_renderer_init_wl_display(renderer, wlDisplay);

    wlr_compositor_create(wlDisplay, 6, renderer);
    wlr_subcompositor_create(wlDisplay);
    wlr_data_device_manager_create(wlDisplay);
    outputLayout = wlr_output_layout_create(wlDisplay);
    seat = wlr_seat_create(wlDisplay, "seat0");
    xdgShell = wlr_xdg_shell_create(wlDisplay, 3);

    struct wlr_xdg_decoration_manager_v1* deco =
        wlr_xdg_decoration_manager_v1_create(wlDisplay);
    if (deco) {
        decorationNew.notify = [](struct wl_listener* l, void* data) {
            Manager* m = wl_container_of(l, m, decorationNew);
            auto* d = static_cast<struct wlr_xdg_toplevel_decoration_v1*>(data);
            // We draw the Fluent frame ourselves, in every case.
            wlr_xdg_toplevel_decoration_v1_set_mode(
                d, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        };
        wl_signal_add(&deco->events.new_toplevel_decoration, &decorationNew);
    }

    cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(cursor, outputLayout);
    cursorManager = wlr_xcursor_manager_create(nullptr, 24);
    wlr_xcursor_manager_load(cursorManager, 1);
    setCursor(0);

    newOutput.notify = [](struct wl_listener* l, void* data) {
        Manager* m = wl_container_of(l, m, newOutput);
        auto* o = static_cast<struct wlr_output*>(data);
        if (!m->output) {
            m->output = o;
            m->screenW = o->width;
            m->screenH = o->height;
        }
        if (!wlr_output_init_render(o, m->allocator, m->renderer)) return;
        struct wlr_output_state st;
        wlr_output_state_init(&st);
        wlr_output_state_set_enabled(&st, true);
        if (o->current_mode) wlr_output_state_set_mode(&st, o->current_mode);
        else if (o->width <= 0 || o->height <= 0)
            wlr_output_state_set_custom_mode(&st, 1280, 800, 60000);
        if (!wlr_output_commit_state(o, &st)) {
            wlr_log(WLR_ERROR, "output commit failed");
        }
        wlr_output_state_finish(&st);
        wlr_output_create_global(o, m->wlDisplay);
        wlr_output_layout_add_auto(m->outputLayout, o);
        m->outputFrame.notify = [](struct wl_listener* l, void*) {
            Manager* mm = wl_container_of(l, mm, outputFrame);
            mm->renderFrameOnOutput();
        };
        wl_signal_add(&o->events.frame, &m->outputFrame);
        m->outputDestroy.notify = [](struct wl_listener* l, void*) {
            Manager* mm = wl_container_of(l, mm, outputDestroy);
            mm->output = nullptr;
        };
        wl_signal_add(&o->events.destroy, &m->outputDestroy);
        log("output %s: %dx%d", o->name, o->width, o->height);
    };
    wl_signal_add(&backend->events.new_output, &newOutput);

    newInput.notify = [](struct wl_listener* l, void* data) {
        Manager* m = wl_container_of(l, m, newInput);
        auto* dev = static_cast<struct wlr_input_device*>(data);
        if (dev->type == WLR_INPUT_DEVICE_KEYBOARD) {
            struct wlr_keyboard* kb = wlr_keyboard_from_input_device(dev);
            struct xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
            struct xkb_rule_names rules = {};
            struct xkb_keymap* km =
                xkb_keymap_new_from_names(ctx, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
            wlr_keyboard_set_keymap(kb, km);
            xkb_keymap_unref(km);
            xkb_context_unref(ctx);
            wlr_keyboard_set_repeat_info(kb, 25, 600);
            m->keyboard = kb;
            m->keyboardKey.notify = [](struct wl_listener* kl, void* kdata) {
                Manager* mm = wl_container_of(kl, mm, keyboardKey);
                auto* ke = static_cast<struct wlr_keyboard_key_event*>(kdata);
                if (!mm->keyboard) return;
                mm->keyMods = xModifiers(wlr_keyboard_get_modifiers(mm->keyboard));
                mm->onKey(ke->keycode, mm->keyMods,
                          ke->state == WL_KEYBOARD_KEY_STATE_PRESSED);
            };
            wl_signal_add(&kb->events.key, &m->keyboardKey);
            m->keyboardMods.notify = [](struct wl_listener* kl, void*) {
                Manager* mm = wl_container_of(kl, mm, keyboardMods);
                if (mm->keyboard)
                    mm->onKeyboardModifiers(xModifiers(wlr_keyboard_get_modifiers(mm->keyboard)));
            };
            wl_signal_add(&kb->events.modifiers, &m->keyboardMods);
            wlr_seat_set_keyboard(m->seat, kb);
        }
        // Pointer, touch and tablet devices only reach the cursor once they are
        // attached to it; without this the shell never sees a motion or a button.
        if (dev->type == WLR_INPUT_DEVICE_POINTER || dev->type == WLR_INPUT_DEVICE_TOUCH ||
            dev->type == WLR_INPUT_DEVICE_TABLET) {
            wlr_cursor_attach_input_device(m->cursor, dev);
        }
        uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
        if (m->keyboard) caps |= WL_SEAT_CAPABILITY_KEYBOARD;
        wlr_seat_set_capabilities(m->seat, caps);
    };
    wl_signal_add(&backend->events.new_input, &newInput);

    // wlroots 0.18 announces a surface before its role is known, so
    // `new_surface` cannot tell a toplevel from a popup. `new_toplevel` is the
    // event that carries the xdg_toplevel, and is what a window is built from.
    newXdgSurface.notify = [](struct wl_listener* l, void* data) {
        Manager* m = wl_container_of(l, m, newXdgSurface);
        m->onNewToplevel(static_cast<struct wlr_xdg_toplevel*>(data));
    };
    wl_signal_add(&xdgShell->events.new_toplevel, &newXdgSurface);

    newPopup.notify = [](struct wl_listener* l, void* data) {
        Manager* m = wl_container_of(l, m, newPopup);
        m->onNewPopup(static_cast<struct wlr_xdg_popup*>(data));
    };
    wl_signal_add(&xdgShell->events.new_popup, &newPopup);

    cursorMotion.notify = [](struct wl_listener* l, void* data) {
        Manager* m = wl_container_of(l, m, cursorMotion);
        auto* ev = static_cast<struct wlr_pointer_motion_event*>(data);
        wlr_cursor_move(m->cursor, &ev->pointer->base, ev->delta_x, ev->delta_y);
        m->onPointerMotion(m->cursor->x, m->cursor->y);
    };
    wl_signal_add(&cursor->events.motion, &cursorMotion);

    // The nested Wayland backend reports the host pointer as a position in
    // 0..1 output coordinates instead of deltas, so it has to be warped.
    cursorMotionAbsolute.notify = [](struct wl_listener* l, void* data) {
        Manager* m = wl_container_of(l, m, cursorMotionAbsolute);
        auto* ev = static_cast<struct wlr_pointer_motion_absolute_event*>(data);
        wlr_cursor_warp_absolute(m->cursor, &ev->pointer->base, ev->x, ev->y);
        m->onPointerMotion(m->cursor->x, m->cursor->y);
    };
    wl_signal_add(&cursor->events.motion_absolute, &cursorMotionAbsolute);

    cursorButton.notify = [](struct wl_listener* l, void* data) {
        Manager* m = wl_container_of(l, m, cursorButton);
        auto* ev = static_cast<struct wlr_pointer_button_event*>(data);
        m->onPointerButton(ev->button, ev->state == WL_POINTER_BUTTON_STATE_PRESSED,
                           ev->time_msec);
    };
    wl_signal_add(&cursor->events.button, &cursorButton);

    cursorAxis.notify = [](struct wl_listener* l, void* data) {
        Manager* m = wl_container_of(l, m, cursorAxis);
        auto* ev = static_cast<struct wlr_pointer_axis_event*>(data);
        m->onPointerAxis(ev->delta);
    };
    wl_signal_add(&cursor->events.axis, &cursorAxis);

    if (!wlr_backend_start(backend)) {
        log("the wlroots backend failed to start");
        return 1;
    }

    if (options.headless) {
        struct wlr_output* o = wlr_headless_add_output(backend, 1280, 800);
        if (!o) log("cannot add a headless output");
    } else if (nested) {
        struct wlr_output* o = wlr_wl_output_create(backend);
        if (!o) log("cannot create a nested Wayland output");
    }

    // Now that an output exists, size the shell from it.
    if (!output) {
    }
    if (output) {
        screenW = output->width;
        screenH = output->height;
    }
    if (screenW <= 0) screenW = 1280;
    if (screenH <= 0) screenH = 800;

    assetDir = defaultAssetDir();
    log("assets: %s", assetDir.c_str());
    icons.init(assetDir + "/icons", assetDir + "/icons-hatter");
    text.init(assetDir + "/fonts");
    clockText.initFromFile(assetDir + "/fonts/Roboto.ttf");

    std::string error;
    if (!comp.init(renderer, screenW, screenH,
                   assetDir + "/wallpaper/wallpaper.png", &error)) {
        log("compositor: %s", error.c_str());
    }
    comp.setVerbosePerf(options.perfLog);

    metrics::taskbarH = loadTaskbarHeight();
    theme::applyMode(loadThemeMode());
    updateWorkArea();
    apps = scanApps();
    pinned = loadPinned();
    recents = loadRecents();
    recentFiles = loadRecentFiles();
    launchpadOrder = loadLaunchpadOrder();
    desktopItems = scanDesktop();
    desktopIconPlacement = loadDesktopIconPlacement();
    desktopNoOverlap = loadDesktopNoOverlap();
    layoutDesktopIcons();
    initWidgets();
    updateClientList();
    sysctl.init();

    if (options.tablet || (std::getenv("WIN11WM_TABLET") &&
                           std::getenv("WIN11WM_TABLET")[0] == '1')) {
        tabletMode = true;
        tabletAnim = 1.0;
        applyTabletMode();
    }
    if (std::getenv("WIN11WM_OPEN_SETTINGS") &&
        std::getenv("WIN11WM_OPEN_SETTINGS")[0] == '1')
        openSettings();

    for (const std::string& cmd : options.launch) {
        if (!cmd.empty()) pendingLaunches.push_back(cmd);
    }

    // `--socket NAME` pins the name so a launcher script can point clients at a
    // known display; without it libwayland picks the first free wayland-N.
    const char* socket = nullptr;
    if (!options.socket.empty()) {
        if (wl_display_add_socket(wlDisplay, options.socket.c_str()) == 0)
            socket = options.socket.c_str();
        else
            log("socket \"%s\" is taken, falling back to an automatic name",
                options.socket.c_str());
    }
    if (!socket) socket = wl_display_add_socket_auto(wlDisplay);
    if (!socket) {
        log("cannot open a Wayland socket");
        return 1;
    }
    setenv("WAYLAND_DISPLAY", socket, 1);
    log("Wayland socket: %s (%s)", socket, nested ? "nested" : "headless");

    // Animation tick: 60Hz while anything is moving, and a slower heartbeat for
    // the clock/battery widgets when the shell is idle.
    tickTimer = wl_event_loop_add_timer(
        evloop,
        [](void* data) -> int {
            Manager* m = static_cast<Manager*>(data);
            const double now = nowMs();
            // The clock and the battery cards want a repaint even when nothing
            // is moving, the same heartbeat the X11 loop's poll timeout gave them.
            if (now - m->lastIdleRefresh >= 250.0) {
                m->lastIdleRefresh = now;
                m->dirty = true;
            }
            m->tickAnimations(now);
            if (m->dirty || (m->opts && m->opts->frames > 0)) m->requestFrame();
            return 16;
        },
        this);
    wl_event_source_timer_update(tickTimer, 16);

    lastTick = nowMs();
    dirty = true;
    requestFrame();

    log("%s | %s | %s", comp.vendorName().c_str(), comp.rendererName().c_str(),
        comp.glVersion().c_str());
    log("canvas %dx%d", screenW, screenH);

    loop();

    saveDesktopLayout();
    shutdown();
    return 0;
}

void Manager::loop() {
    if (!wlDisplay) return;
    running = true;
    wl_display_run(wlDisplay);
}

// === PORTED SHELL ===
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
        if (c->toplevel) wlr_xdg_toplevel_send_close(c->toplevel);
        c->closing = false;
        c->alive = false;
        unmapClient(c);
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
    step(settingsAnim, settingsOpen ? 1.0 : 0.0, 180.0);
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
    // The app drawer rises and falls, and its grid settles onto a page once a
    // swipe has let go -- the same page-in-pages offset the home grid uses.
    step(tabletDrawerAnim, tabletDrawer ? 1.0 : 0.0, 300.0);
    if (tabletDrawer && !tabletDrawerSwipe && tabletDrawerPageCount > 0) {
        const double goal = double(tabletDrawerPage);
        if (std::abs(tabletDrawerOffset - goal) > 0.001) {
            tabletDrawerOffset += (goal - tabletDrawerOffset) *
                                  std::min(1.0, dtMs / double(metrics::kTabletPageTurnMs));
            dirty = true;
        } else {
            tabletDrawerOffset = goal;
        }
    }
    // Rearrange mode jiggles the icons, so it needs a frame every tick.
    if (tabletEdit && tabletMode) dirty = true;
    // iPhone X home bar: a swipe that pauses on the way up opens the app
    // switcher. No motion event reports the pause (the finger is not moving),
    // so the dwell is detected here.
    if (tabletGesture && !tabletGestureSwipe && !tabletSwitcher && !tabletDrawer &&
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

    // Tablet home icons: the same reflow, so an icon carried across the grid
    // makes its neighbours part and settle rather than snapping around it.
    if (tabletMode) animateTabletIconReflow(dtMs);

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
    return startOpen || taskViewOpen || altTabOpen || contextOpen || ccOpen || settingsOpen ||
           tabletSwitcher || confirmDeleteOpen || desktopRenameItem >= 0;
}

bool Manager::pointInOverlaySurface(int x, int y) const {
    if (startOpen && startRect.inflated(12).contains(x, y)) return true;
    if (taskViewOpen) return true;
    if (altTabOpen) return true;
    if (contextOpen && contextRect.contains(x, y)) return true;
    if (ccOpen && ccRect.inflated(12).contains(x, y)) return true;
    if (settingsOpen && settingsPanel.contains(x, y)) return true;
    return false;
}

void Manager::closeOverlays() {
    const bool was = overlayOpen();
    const bool wasCc = ccOpen;
    startOpen = taskViewOpen = altTabOpen = contextOpen = ccOpen = false;
    settingsOpen = false;
    settingsHover = -1;
    settingsNavHover = -1;
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
        ungrabShellKeyboard();
    }
    dirty = true;
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

namespace {
// One shared (omega, zeta) for every window transition, matching the X11 build.
constexpr double kGeometryHz = 3.2;
}  // namespace

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
// button rather than starting a second copy. A cold launch grows the launch
// placeholder out of the pin's own button, exactly as a desktop icon does, so
// the app's window seems to open the moment the button is clicked.
void Manager::activatePinned(int index) {
    if (index < 0 || index >= int(pinned.size())) return;
    const AppEntry app = pinned[size_t(index)];
    if (Client* c = clientForPinned(app)) {
        activateTaskbarItem(c);
        return;
    }
    noteRecent(app.name, app.exec, app.icon, app.wmClass);
    Rect from{};
    for (size_t i = 0; i < taskItems.size(); ++i) {
        if (taskItems[i].pin == index) {
            from = taskHitRect(i);
            break;
        }
    }
    if (from.w < 8 || from.h < 8) {
        const int side = metrics::taskIconSize();
        from = Rect{screenW / 2 - side / 2, screenH - metrics::taskbarH, side, side};
    }
    DesktopItem item;
    item.name = app.name.empty() ? app.exec : app.name;
    item.icon = app.icon;
    item.exec = app.exec;
    item.path = app.exec;
    beginLaunchAnim(item, from);
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
    grabShellKeyboard();
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
    grabShellKeyboard();
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
        if (it.client) {
            buttons.push_back(it.client);
            continue;
        }
        // A pinned launcher stands in for its running window, which no longer has
        // a button of its own, so it takes part in the wheel walk here.
        if (it.pin >= 0 && it.pin < int(pinned.size())) {
            if (Client* c = clientForPinned(pinned[size_t(it.pin)])) buttons.push_back(c);
        }
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
    grabShellKeyboard();
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
    grabShellKeyboard();
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
    grabShellKeyboard();
    dirty = true;
}

// The desktop's own context menu, reusing the client menu plumbing. Its shape
// follows what it was opened on: a card offers to be removed, a folder offers to
// be renamed or deleted, and the create, widget and no-overlap entries are
// always there. applyContextAction() walks the rows in this same order.
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
    // The desktop no-overlap guard, shown as the state it is in so the row both
    // reports and flips it.
    contextItems.push_back(desktopNoOverlap ? "Prevent Overlap: On" : "Prevent Overlap: Off");

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
    grabShellKeyboard();
    dirty = true;
}

// Flips the desktop no-overlap option. The desktop is re-laid at once so the
// change is visible while the menu is still open -- turning it off lets a
// hand-placed folder that had been nudged aside return to the cell it was left
// in; turning it on folds the icons that are currently under a widget away.
void Manager::toggleDesktopNoOverlap() {
    desktopNoOverlap = !desktopNoOverlap;
    saveDesktopNoOverlap(desktopNoOverlap);
    layoutDesktopIcons();
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
    grabShellKeyboard();
    dirty = true;
}

void Manager::cancelDesktopRename() {
    if (desktopRenameItem < 0) return;
    desktopRenameItem = -1;
    desktopRenameText.clear();
    ungrabPointer();
    ungrabShellKeyboard();
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
    grabShellKeyboard();
    dirty = true;
}

void Manager::closeConfirmDelete() {
    if (!confirmDeleteOpen) return;
    confirmDeleteOpen = false;
    confirmDeletePath.clear();
    confirmDeleteName.clear();
    ungrabPointer();
    ungrabShellKeyboard();
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



}  // namespace wm
