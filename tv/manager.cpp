#include "manager.h"

#include "wlr.h"

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tv {
namespace {

double nowMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) * 1000.0 + double(ts.tv_nsec) / 1e6;
}

uint32_t nowMs32() { return uint32_t(nowMs()); }

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

bool dirExists(const std::string& path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string executableDir() {
    char buf[PATH_MAX] = {};
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    std::string path(buf, size_t(n));
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string{} : path.substr(0, slash);
}

void sendFrameDone(struct wlr_surface* surface, int, int, void* data) {
    if (surface) wlr_surface_send_frame_done(surface, static_cast<struct timespec*>(data));
}

void detachClientListeners(Client* c, struct wl_listener* skip) {
    for (struct wl_listener* l : {&c->surfaceCommit, &c->surfaceMap, &c->surfaceUnmap,
                                  &c->surfaceDestroy, &c->toplevelDestroy,
                                  &c->toplevelTitle, &c->toplevelAppId,
                                  &c->requestFullscreen, &c->requestClose}) {
        if (l != skip) wl_list_remove(&l->link);
        wl_list_init(&l->link);
    }
}

unsigned padKey(PadButton b) {
    switch (b) {
        case PadButton::Up: return KEY_UP;
        case PadButton::Down: return KEY_DOWN;
        case PadButton::Left: return KEY_LEFT;
        case PadButton::Right: return KEY_RIGHT;
        case PadButton::Select: return KEY_ENTER;
        case PadButton::Back: return KEY_ESC;
        case PadButton::Menu: return KEY_MENU;
        case PadButton::PageLeft: return KEY_PAGEUP;
        case PadButton::PageRight: return KEY_PAGEDOWN;
        case PadButton::Home: return KEY_HOMEPAGE;
    }
    return 0;
}

}  // namespace

Manager::~Manager() { shutdown(); }

int Manager::run(const Options& options) {
    opts_ = options;
    nested_ = !opts_.headless && !opts_.drm && std::getenv("WAYLAND_DISPLAY");
    if (!initWayland()) return 1;
    if (!initShell()) return 1;

    lastTick_ = nowMs();
    dirty_ = true;
    requestFrame();
    loop();
    shutdown();
    return 0;
}

bool Manager::initWayland() {
    display_ = wl_display_create();
    if (!display_) {
        std::fprintf(stderr, "[tvshell] cannot create a Wayland display\n");
        return false;
    }
    evloop_ = wl_display_get_event_loop(display_);

    if (opts_.headless) {
        backend_ = wlr_headless_backend_create(evloop_);
        nested_ = false;
    } else if (opts_.drm) {
        backend_ = wlr_backend_autocreate(evloop_, nullptr);
        nested_ = false;
    } else if (std::getenv("WAYLAND_DISPLAY")) {
        backend_ = wlr_wl_backend_create(evloop_, nullptr);
        nested_ = true;
    } else {
        backend_ = wlr_backend_autocreate(evloop_, nullptr);
        nested_ = false;
    }
    if (!backend_) {
        std::fprintf(stderr, "[tvshell] cannot create a wlroots backend\n");
        return false;
    }

    renderer_ = wlr_renderer_autocreate(backend_);
    if (!renderer_) {
        std::fprintf(stderr, "[tvshell] cannot create a renderer\n");
        return false;
    }
    allocator_ = wlr_allocator_autocreate(backend_, renderer_);
    if (!allocator_) {
        std::fprintf(stderr, "[tvshell] cannot create an allocator\n");
        return false;
    }
    wlr_renderer_init_wl_display(renderer_, display_);

    wlr_compositor_create(display_, 6, renderer_);
    wlr_subcompositor_create(display_);
    wlr_data_device_manager_create(display_);
    wlr_primary_selection_v1_device_manager_create(display_);
    outputLayout_ = wlr_output_layout_create(display_);
    wlr_xdg_output_manager_v1_create(display_, outputLayout_);
    presentation_ = wlr_presentation_create(display_, backend_);

    seat_ = wlr_seat_create(display_, "seat0");
    wlr_seat_set_capabilities(seat_, WL_SEAT_CAPABILITY_POINTER);
    xdgShell_ = wlr_xdg_shell_create(display_, 3);

    newOutput_.notify = [](struct wl_listener* l, void* data) {
        Manager* m = wl_container_of(l, m, newOutput_);
        m->onNewOutput(static_cast<struct wlr_output*>(data));
    };
    wl_signal_add(&backend_->events.new_output, &newOutput_);

    newInput_.notify = [](struct wl_listener* l, void* data) {
        Manager* m = wl_container_of(l, m, newInput_);
        m->onNewInput(static_cast<struct wlr_input_device*>(data));
    };
    wl_signal_add(&backend_->events.new_input, &newInput_);

    newToplevel_.notify = [](struct wl_listener* l, void* data) {
        Manager* m = wl_container_of(l, m, newToplevel_);
        m->onNewToplevel(static_cast<struct wlr_xdg_toplevel*>(data));
    };
    wl_signal_add(&xdgShell_->events.new_toplevel, &newToplevel_);

    if (!wlr_backend_start(backend_)) {
        std::fprintf(stderr, "[tvshell] backend failed to start\n");
        return false;
    }

    if (opts_.headless) {
        if (!wlr_headless_add_output(backend_, opts_.width, opts_.height))
            std::fprintf(stderr, "[tvshell] cannot add a headless output\n");
    } else if (nested_) {
        if (!wlr_wl_output_create(backend_))
            std::fprintf(stderr, "[tvshell] cannot create a nested output\n");
    }

    const char* socket = nullptr;
    if (!opts_.socket.empty()) {
        if (wl_display_add_socket(display_, opts_.socket.c_str()) != 0) {
            std::fprintf(stderr, "[tvshell] cannot open socket '%s'\n", opts_.socket.c_str());
            return false;
        }
        socketName_ = opts_.socket;
        socket = socketName_.c_str();
    } else {
        socket = wl_display_add_socket_auto(display_);
        if (socket) socketName_ = socket;
    }
    if (!socket) {
        std::fprintf(stderr, "[tvshell] cannot open a Wayland socket\n");
        return false;
    }
    setenv("WAYLAND_DISPLAY", socket, 1);
    std::fprintf(stderr, "[tvshell] Wayland socket: %s (%s)\n", socket,
                 nested_ ? "nested" : (opts_.headless ? "headless" : "drm"));
    return true;
}

bool Manager::initShell() {
    const char* envAssets = std::getenv("TV_ASSET_DIR");
    if (envAssets && dirExists(envAssets)) {
        assetDir_ = envAssets;
    } else {
        const std::string exe = executableDir();
        const std::string candidate = exe + "/../../assets";
        assetDir_ = dirExists(candidate) ? candidate : "assets";
    }
    std::fprintf(stderr, "[tvshell] assets: %s\n", assetDir_.c_str());

    text_.init(assetDir_ + "/fonts");
    icons_.init(assetDir_ + "/icons", assetDir_ + "/icons-hatter");

    apps_ = wm::scanApps();
    apps_.erase(std::remove_if(apps_.begin(), apps_.end(),
                               [](const wm::AppEntry& a) {
                                   return a.name.empty() || a.exec.empty() ||
                                          a.exec == wm::kSettingsExec || a.terminal;
                               }),
                apps_.end());
    layoutApps();

    homeSpring_.configureIOS(0.32, 0.90);
    homeSpring_.value = 1.0;
    homeTarget_ = 1.0;

    tileScale_.resize(apps_.size());
    for (auto& s : tileScale_) {
        s.configureIOS(0.22, 0.80);
        s.value = 1.0;
    }

    controllers_.init(evloop_, [this](const PadEvent& event) { onController(event); });
    if (controllers_.count() == 0)
        std::fprintf(stderr,
                     "[tvshell] no controller found (check /dev/input permissions)\n");

    tickTimer_ = wl_event_loop_add_timer(
        evloop_,
        [](void* data) -> int {
            Manager* m = static_cast<Manager*>(data);
            const double now = nowMs();
            m->controllers_.pollRepeats(now);
            if (m->home_ || !m->active_) {
                const time_t t = time(nullptr);
                struct tm lt {};
                localtime_r(&t, &lt);
                if (lt.tm_min != m->lastClockMinute_) {
                    m->lastClockMinute_ = lt.tm_min;
                    m->dirty_ = true;
                    m->requestFrame();
                }
            }
            if (m->tickTimer_) wl_event_source_timer_update(m->tickTimer_, 50);
            return 0;
        },
        this);
    if (tickTimer_) wl_event_source_timer_update(tickTimer_, 50);

    for (const std::string& cmd : opts_.launch) wm::launchApp(cmd);
    return true;
}

void Manager::shutdown() {
    if (!display_) return;
    controllers_.shutdown();
    if (tickTimer_) {
        wl_event_source_remove(tickTimer_);
        tickTimer_ = nullptr;
    }
    for (auto& c : clients_) compositor_.releaseWindowTex(&c->tex);
    clients_.clear();
    active_ = nullptr;
    text_.shutdown();
    icons_.clear();
    compositor_.shutdown();
    wl_display_destroy_clients(display_);
    wl_display_destroy(display_);
    display_ = nullptr;
    evloop_ = nullptr;
}

void Manager::loop() {
    if (!display_) return;
    running_ = true;
    wl_display_run(display_);
}

void Manager::stop() {
    running_ = false;
    if (display_) wl_display_terminate(display_);
}

void Manager::onNewOutput(struct wlr_output* output) {
    if (!output) return;
    if (!output_) output_ = output;

    if (!wlr_output_init_render(output, allocator_, renderer_)) {
        std::fprintf(stderr, "[tvshell] cannot init output render\n");
        return;
    }

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    if (output->current_mode) {
        wlr_output_state_set_mode(&state, output->current_mode);
    } else {
        wlr_output_state_set_custom_mode(&state, opts_.width, opts_.height, 60000);
    }
    if (!wlr_output_commit_state(output, &state))
        std::fprintf(stderr, "[tvshell] output commit failed\n");
    wlr_output_state_finish(&state);

    wlr_output_create_global(output, display_);
    wlr_output_layout_add_auto(outputLayout_, output);

    outputFrame_.notify = [](struct wl_listener* l, void*) {
        Manager* m = wl_container_of(l, m, outputFrame_);
        m->onOutputFrame();
    };
    wl_signal_add(&output->events.frame, &outputFrame_);

    outputDestroy_.notify = [](struct wl_listener* l, void*) {
        Manager* m = wl_container_of(l, m, outputDestroy_);
        m->onOutputDestroy();
    };
    wl_signal_add(&output->events.destroy, &outputDestroy_);

    outputRequestState_.notify = [](struct wl_listener* l, void* data) {
        Manager* m = wl_container_of(l, m, outputRequestState_);
        auto* ev = static_cast<struct wlr_output_event_request_state*>(data);
        if (!ev || !ev->output || !ev->state) return;
        if (!wlr_output_commit_state(ev->output, ev->state)) return;
        m->syncOutputSize();
        m->dirty_ = true;
        m->requestFrame();
    };
    wl_signal_add(&output->events.request_state, &outputRequestState_);

    syncOutputSize();
    std::fprintf(stderr, "[tvshell] output %s: %dx%d\n", output->name, output->width,
                 output->height);
}

void Manager::onOutputFrame() { renderFrameOnOutput(); }

void Manager::onOutputDestroy() {
    output_ = nullptr;
    stop();
}

void Manager::syncOutputSize() {
    if (!output_) return;
    const int w = output_->width;
    const int h = output_->height;
    if (w <= 1 || h <= 1) return;
    if (w == screenW_ && h == screenH_ && compositor_.ready()) return;
    screenW_ = w;
    screenH_ = h;
    if (!compositor_.ready()) {
        std::string error;
        compositor_.init(renderer_, w, h, std::string(), &error);
    } else {
        compositor_.resize(w, h);
    }
    layoutApps();
    dirty_ = true;
}

void Manager::requestFrame() {
    dirty_ = true;
    if (!output_ || !output_->enabled) return;
    if (nowMs() < frameBackoffUntil_) return;
    wlr_output_schedule_frame(output_);
}

void Manager::renderFrameOnOutput() {
    if (!output_ || !output_->enabled) return;
    syncOutputSize();
    const double now = nowMs();
    const bool animating = tick(now);
    if (!dirty_ && !animating && opts_.frames <= 0) return;
    if (now < frameBackoffUntil_) return;

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    struct wlr_render_pass* pass =
        wlr_output_begin_render_pass(output_, &state, nullptr, nullptr);
    if (!pass) {
        wlr_output_state_finish(&state);
        frameBackoffUntil_ = now + 16.0;
        return;
    }

    compositor_.beginFrame();
    render();
    if (!opts_.dump.empty() && opts_.frames > 0 && framesRendered_ + 1 >= opts_.frames)
        dumpFrame();
    compositor_.present();
    wlr_render_pass_submit(pass);

    if (!wlr_output_commit_state(output_, &state)) {
        wlr_output_state_finish(&state);
        frameBackoffUntil_ = now + 16.0;
        return;
    }
    wlr_output_state_finish(&state);

    ++framesRendered_;
    dirty_ = false;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    for (auto& c : clients_) {
        if (!c->mapped) continue;
        if (c->xdg) wlr_xdg_surface_for_each_surface(c->xdg, sendFrameDone, &ts);
        else if (c->surface) wlr_surface_send_frame_done(c->surface, &ts);
    }

    if (opts_.frames > 0 && framesRendered_ >= opts_.frames) {
        stop();
        return;
    }
    // A nested output receives its next frame event from the host after this
    // commit. Headless/DRM outputs need an explicit request to keep an animation
    // running.
    if ((animating || opts_.frames > 0) && !nested_)
        wlr_output_schedule_frame(output_);
}

void Manager::tick(double now) {
    double dt = (now - lastTick_) / 1000.0;
    lastTick_ = now;
    if (dt <= 0.0 || dt > 0.1) dt = 1.0 / 60.0;

    bool moved = false;
    if (!homeSpring_.settled(homeTarget_, 0.001)) {
        homeSpring_.step(homeTarget_, dt);
        moved = true;
    } else {
        homeSpring_.value = homeTarget_;
    }

    for (size_t i = 0; i < tileScale_.size(); ++i) {
        const double target = (home_ && int(i) == focus_) ? 1.07 : 1.0;
        if (!tileScale_[i].settled(target, 0.001)) {
            tileScale_[i].step(target, dt);
            moved = true;
        } else {
            tileScale_[i].value = target;
        }
    }
    if (moved) dirty_ = true;
}

void Manager::render() {
    drawBackground();
    if (active_ && active_->mapped && active_->tex.valid()) drawClient();
    if (homeSpring_.value > 0.001) drawHome();
}

void Manager::dumpFrame() {
    const int w = screenW_;
    const int h = screenH_;
    if (w <= 0 || h <= 0) return;
    std::vector<unsigned char> pixels(size_t(w) * size_t(h) * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    FILE* f = std::fopen(opts_.dump.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; --y) {
        const unsigned char* row = pixels.data() + size_t(y) * size_t(w) * 4;
        for (int x = 0; x < w; ++x) {
            const unsigned char rgb[3] = {row[x * 4 + 0], row[x * 4 + 1], row[x * 4 + 2]};
            std::fwrite(rgb, 1, 3, f);
        }
    }
    std::fclose(f);
}

void Manager::onNewToplevel(struct wlr_xdg_toplevel* toplevel) {
    if (!toplevel || !toplevel->base || !toplevel->base->surface) return;
    auto holder = std::make_unique<Client>();
    Client* c = holder.get();
    c->manager = this;
    c->toplevel = toplevel;
    c->xdg = toplevel->base;
    c->surface = toplevel->base->surface;
    c->title = toplevel->title ? toplevel->title : "App";
    c->appId = toplevel->app_id ? toplevel->app_id : "";
    clients_.push_back(std::move(holder));

    c->surfaceCommit.notify = [](struct wl_listener* l, void*) {
        Client* cl = wl_container_of(l, cl, surfaceCommit);
        if (cl && cl->alive && cl->manager) cl->manager->onCommit(cl);
    };
    wl_signal_add(&c->surface->events.commit, &c->surfaceCommit);

    c->surfaceMap.notify = [](struct wl_listener* l, void*) {
        Client* cl = wl_container_of(l, cl, surfaceMap);
        if (cl && cl->alive && cl->manager) cl->manager->onMap(cl);
    };
    wl_signal_add(&c->surface->events.map, &c->surfaceMap);

    c->surfaceUnmap.notify = [](struct wl_listener* l, void*) {
        Client* cl = wl_container_of(l, cl, surfaceUnmap);
        if (cl && cl->alive && cl->manager) cl->manager->onUnmap(cl);
    };
    wl_signal_add(&c->surface->events.unmap, &c->surfaceUnmap);

    c->surfaceDestroy.notify = [](struct wl_listener* l, void*) {
        Client* cl = wl_container_of(l, cl, surfaceDestroy);
        if (cl && cl->manager) {
            cl->firing = l;
            cl->manager->onDestroy(cl);
        }
    };
    wl_signal_add(&c->surface->events.destroy, &c->surfaceDestroy);

    c->toplevelDestroy.notify = [](struct wl_listener* l, void*) {
        Client* cl = wl_container_of(l, cl, toplevelDestroy);
        if (cl && cl->manager) {
            cl->firing = l;
            cl->manager->onDestroy(cl);
        }
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

    c->requestFullscreen.notify = [](struct wl_listener* l, void*) {
        Client* cl = wl_container_of(l, cl, requestFullscreen);
        if (cl && cl->toplevel) cl->fullscreen = cl->toplevel->requested.fullscreen;
    };
    wl_signal_add(&toplevel->events.request_fullscreen, &c->requestFullscreen);

    c->requestClose.notify = [](struct wl_listener*, void*) {};
    wl_signal_add(&toplevel->events.request_close, &c->requestClose);

    dirty_ = true;
    requestFrame();
}

void Manager::onMap(Client* c) {
    if (!c) return;
    c->mapped = true;
    if (c->toplevel) {
        c->fullscreen = true;
        wlr_xdg_toplevel_set_fullscreen(c->toplevel, true);
    }
    active_ = c;
    home_ = false;
    homeTarget_ = 0.0;
    focusClient(c);
    dirty_ = true;
    requestFrame();
}

void Manager::onUnmap(Client* c) {
    if (!c) return;
    c->mapped = false;
    compositor_.releaseWindowTex(&c->tex);
    if (active_ == c) {
        active_ = nullptr;
        for (auto it = clients_.rbegin(); it != clients_.rend(); ++it) {
            if (it->get()->mapped && it->get() != c) {
                active_ = it->get();
                break;
            }
        }
        if (active_) {
            home_ = false;
            homeTarget_ = 0.0;
            focusClient(active_);
        } else {
            home_ = true;
            homeTarget_ = 1.0;
            clearClientFocus();
        }
    }
    dirty_ = true;
    requestFrame();
}

void Manager::onCommit(Client* c) {
    if (!c || !c->xdg || !c->xdg->initialized) return;
    if (!c->configured) {
        c->configured = true;
        if (c->toplevel) wlr_xdg_toplevel_set_fullscreen(c->toplevel, true);
        dirty_ = true;
        requestFrame();
        return;
    }
    if (c->surface && c->surface->buffer)
        compositor_.importBuffer(c->surface->buffer, &c->tex);
    dirty_ = true;
    requestFrame();
}

void Manager::onDestroy(Client* c) {
    if (!c || !c->alive) return;
    c->alive = false;
    // The listener currently dispatching has already been unlinked by wlroots;
    // leave its link alone and unlink the rest.
    detachClientListeners(c, c->firing);
    if (active_ == c) active_ = nullptr;
    compositor_.releaseWindowTex(&c->tex);

    const auto it = std::find_if(clients_.begin(), clients_.end(),
                                 [c](const std::unique_ptr<Client>& p) { return p.get() == c; });
    if (it != clients_.end()) clients_.erase(it);

    if (!active_) {
        for (auto rit = clients_.rbegin(); rit != clients_.rend(); ++rit) {
            if (rit->get()->mapped) {
                active_ = rit->get();
                break;
            }
        }
        if (active_) {
            home_ = false;
            homeTarget_ = 0.0;
            focusClient(active_);
        } else {
            home_ = true;
            homeTarget_ = 1.0;
            clearClientFocus();
        }
    }
    dirty_ = true;
    requestFrame();
}

void Manager::clearClientFocus() {
    if (seat_) wlr_seat_keyboard_clear_focus(seat_);
}

void Manager::focusClient(Client* c) {
    if (!c || !c->mapped) return;
    if (active_ && active_ != c && active_->toplevel)
        wlr_xdg_toplevel_set_activated(active_->toplevel, false);
    active_ = c;
    if (c->toplevel) wlr_xdg_toplevel_set_activated(c->toplevel, true);
    if (seat_ && c->surface && !home_)
        wlr_seat_keyboard_notify_enter(seat_, c->surface, nullptr, 0,
                                       keyboard_ ? &keyboard_->modifiers : nullptr);
    dirty_ = true;
    requestFrame();
}

void Manager::onNewInput(struct wlr_input_device* device) {
    if (!device) return;
    if (device->type == WLR_INPUT_DEVICE_KEYBOARD)
        setupKeyboard(wlr_keyboard_from_input_device(device));
}

void Manager::setupKeyboard(struct wlr_keyboard* keyboard) {
    if (!keyboard) return;
    struct xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_rule_names rules = {};
    struct xkb_keymap* keymap =
        xkb_keymap_new_from_names(ctx, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (keymap) {
        wlr_keyboard_set_keymap(keyboard, keymap);
        xkb_keymap_unref(keymap);
    }
    if (ctx) xkb_context_unref(ctx);
    wlr_keyboard_set_repeat_info(keyboard, 25, 600);
    keyboard_ = keyboard;

    keyboardKey_.notify = [](struct wl_listener* l, void* data) {
        Manager* m = wl_container_of(l, m, keyboardKey_);
        auto* ev = static_cast<struct wlr_keyboard_key_event*>(data);
        if (!m->keyboard_ || !ev) return;
        m->onKey(ev->keycode, 0, ev->state == WL_KEYBOARD_KEY_STATE_PRESSED);
    };
    wl_signal_add(&keyboard->events.key, &keyboardKey_);

    keyboardMods_.notify = [](struct wl_listener* l, void*) {
        Manager* m = wl_container_of(l, m, keyboardMods_);
        if (m->seat_ && m->keyboard_ && m->active_ && !m->home_)
            wlr_seat_keyboard_notify_modifiers(m->seat_, &m->keyboard_->modifiers);
    };
    wl_signal_add(&keyboard->events.modifiers, &keyboardMods_);

    wlr_seat_set_keyboard(seat_, keyboard);
    wlr_seat_set_capabilities(seat_, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
}

void Manager::forwardKey(unsigned evdevCode, bool pressed) {
    if (!seat_ || !keyboard_ || !active_ || !active_->mapped || home_) return;
    wlr_seat_set_keyboard(seat_, keyboard_);
    wlr_seat_keyboard_notify_key(seat_, nowMs32(), evdevCode,
                                 pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
                                         : WL_KEYBOARD_KEY_STATE_RELEASED);
}

void Manager::onKey(uint32_t keycode, uint32_t, bool pressed) {
    if (!keyboard_) return;
    const xkb_keysym_t sym = xkb_state_key_get_one_sym(keyboard_->xkb_state, keycode + 8);

    if (pressed && (sym == XKB_KEY_Home || sym == XKB_KEY_HOMEPAGE)) {
        setHome(true);
        return;
    }
    if (!home_ && active_ && pressed &&
        (sym == XKB_KEY_Escape || sym == XKB_KEY_BackSpace)) {
        setHome(true);
        return;
    }

    if (home_) {
        if (!pressed) return;
        switch (sym) {
            case XKB_KEY_Left: moveFocus(-1, 0); break;
            case XKB_KEY_Right: moveFocus(1, 0); break;
            case XKB_KEY_Up: moveFocus(0, -1); break;
            case XKB_KEY_Down: moveFocus(0, 1); break;
            case XKB_KEY_Return:
            case XKB_KEY_KP_Enter: selectFocus(); break;
            case XKB_KEY_Page_Up: changePage(-1); break;
            case XKB_KEY_Page_Down: changePage(1); break;
            default: break;
        }
        return;
    }
    forwardKey(keycode, pressed);
}

void Manager::onController(const PadEvent& event) {
    if (home_) {
        if (!event.pressed) return;
        switch (event.button) {
            case PadButton::Up: moveFocus(0, -1); break;
            case PadButton::Down: moveFocus(0, 1); break;
            case PadButton::Left: moveFocus(-1, 0); break;
            case PadButton::Right: moveFocus(1, 0); break;
            case PadButton::Select: selectFocus(); break;
            case PadButton::Back: break;
            case PadButton::Home: break;
            case PadButton::Menu: break;
            case PadButton::PageLeft: changePage(-1); break;
            case PadButton::PageRight: changePage(1); break;
        }
        return;
    }

    if (event.button == PadButton::Home || event.button == PadButton::Menu ||
        event.button == PadButton::Back) {
        if (event.pressed) setHome(true);
        return;
    }
    if (!active_) {
        if (event.pressed) setHome(true);
        return;
    }
    forwardKey(padKey(event.button), event.pressed);
}

void Manager::setHome(bool home) {
    if (home_ == home) return;
    home_ = home;
    homeTarget_ = home ? 1.0 : 0.0;
    if (home) clearClientFocus();
    else if (active_) focusClient(active_);
    dirty_ = true;
    requestFrame();
}

void Manager::toggleHome() { setHome(!home_); }

int Manager::pageCount() const {
    const int per = std::max(1, perPage());
    return std::max(1, (int(apps_.size()) + per - 1) / per);
}

int Manager::currentPage() const {
    if (apps_.empty()) return 0;
    return std::clamp(focus_ / std::max(1, perPage()), 0, pageCount() - 1);
}

void Manager::layoutApps() {
    columns_ = screenW_ >= 1600 ? 6 : 5;
    rows_ = screenH_ >= 900 ? 3 : 2;
    if (!apps_.empty() && columns_ > int(apps_.size()))
        columns_ = std::max(1, int(apps_.size()));
    tileScale_.resize(apps_.size());
    for (auto& s : tileScale_) {
        if (s.value == 0.0) {
            s.configureIOS(0.22, 0.80);
            s.value = 1.0;
        }
    }
    if (!apps_.empty()) focus_ = std::clamp(focus_, 0, int(apps_.size()) - 1);
    else focus_ = 0;
}

void Manager::moveFocus(int dx, int dy) {
    if (apps_.empty()) return;
    const int per = std::max(1, perPage());
    const int page = currentPage();
    const int start = page * per;
    const int local = std::clamp(focus_ - start, 0, per - 1);
    int col = local % columns_;
    int row = local / columns_;
    col = std::clamp(col + dx, 0, columns_ - 1);
    row = std::clamp(row + dy, 0, rows_ - 1);
    int next = start + row * columns_ + col;
    if (next >= int(apps_.size())) next = int(apps_.size()) - 1;
    if (next < start) next = start;
    focus_ = next;
    dirty_ = true;
    requestFrame();
}

void Manager::changePage(int delta) {
    if (apps_.empty()) return;
    const int page = std::clamp(currentPage() + delta, 0, pageCount() - 1);
    focus_ = std::clamp(page * std::max(1, perPage()), 0, int(apps_.size()) - 1);
    dirty_ = true;
    requestFrame();
}

void Manager::selectFocus() {
    if (apps_.empty()) return;
    launchFocus();
}

void Manager::launchFocus() {
    if (focus_ < 0 || focus_ >= int(apps_.size())) return;
    launchEntry(apps_[size_t(focus_)].entry);
}

Client* Manager::clientForApp(const wm::AppEntry& entry) const {
    const std::string wm = lower(entry.wmClass);
    const std::string name = lower(entry.name);
    for (const auto& c : clients_) {
        if (!c->mapped) continue;
        const std::string id = lower(c->appId);
        const std::string title = lower(c->title);
        if ((!wm.empty() && (id == wm || id.find(wm) != std::string::npos)) ||
            (!name.empty() && (id.find(name) != std::string::npos ||
                               title.find(name) != std::string::npos)))
            return c.get();
    }
    return nullptr;
}

void Manager::launchEntry(const wm::AppEntry& entry) {
    if (entry.exec.empty()) return;
    if (Client* running = clientForApp(entry)) {
        active_ = running;
        setHome(false);
        focusClient(running);
        return;
    }
    wm::launchApp(entry.exec);
}

void Manager::ensureIcons() {
    const int px = std::max(48, std::min(screenW_, screenH_) / 10);
    for (auto& app : apps_) {
        if (app.iconTried) continue;
        app.iconTried = true;
        app.icon = icons_.forApp(app.entry.icon, app.entry.wmClass, px,
                                 wm::IconTheme::Hatter);
    }
}

}  // namespace tv
