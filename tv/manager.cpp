#include "manager.h"

#include "host_output.h"
#include "wlr.h"

// Pointer input. The shared header covers the devices the shell already used;
// the cursor and the pointer events it forwards are pulled in here, without
// touching the desktop shell's own header.
extern "C" {
#include <wayland-client-protocol.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_pointer.h>
}

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
                                  &c->requestFullscreen}) {
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
    // A TV stick is the target of the DRM path, so it always gets the lean
    // profile: no spring animations and no per-frame work a Pi 3 cannot afford.
    minimal_ = opts_.minimal || opts_.drm;
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

    // A pointer gives the shell two things a d-pad cannot: the clock as a
    // Control Centre handle, and clicks on the panel's own controls. Apps get
    // no pointer focus -- a TV is remote-driven -- so the shell draws the
    // cursor itself.
    cursor_ = wlr_cursor_create();
    if (cursor_) {
        wlr_cursor_attach_output_layout(cursor_, outputLayout_);
        cursorMotion_.notify = [](struct wl_listener* l, void* data) {
            Manager* m = wl_container_of(l, m, cursorMotion_);
            auto* ev = static_cast<struct wlr_pointer_motion_event*>(data);
            if (!m->cursor_ || !ev) return;
            wlr_cursor_move(m->cursor_, &ev->pointer->base, ev->delta_x, ev->delta_y);
            m->onPointerMotion(m->cursor_->x, m->cursor_->y);
        };
        wl_signal_add(&cursor_->events.motion, &cursorMotion_);
        // A nested Wayland output reports the host pointer as an absolute
        // position in 0..1 output coordinates instead of as deltas, so it has
        // to be warped rather than moved.
        cursorMotionAbsolute_.notify = [](struct wl_listener* l, void* data) {
            Manager* m = wl_container_of(l, m, cursorMotionAbsolute_);
            auto* ev = static_cast<struct wlr_pointer_motion_absolute_event*>(data);
            if (!m->cursor_ || !ev) return;
            wlr_cursor_warp_absolute(m->cursor_, &ev->pointer->base, ev->x, ev->y);
            m->onPointerMotion(m->cursor_->x, m->cursor_->y);
        };
        wl_signal_add(&cursor_->events.motion_absolute, &cursorMotionAbsolute_);
        cursorButton_.notify = [](struct wl_listener* l, void* data) {
            Manager* m = wl_container_of(l, m, cursorButton_);
            auto* ev = static_cast<struct wlr_pointer_button_event*>(data);
            if (!ev) return;
            m->onPointerButton(ev->button,
                               ev->state == WL_POINTER_BUTTON_STATE_PRESSED);
        };
        wl_signal_add(&cursor_->events.button, &cursorButton_);
    }

    if (!wlr_backend_start(backend_)) {
        std::fprintf(stderr, "[tvshell] backend failed to start\n");
        return false;
    }

    if (opts_.headless) {
        if (!wlr_headless_add_output(backend_, opts_.width, opts_.height))
            std::fprintf(stderr, "[tvshell] cannot add a headless output\n");
    } else if (nested_) {
        struct wlr_output* out = wlr_wl_output_create(backend_);
        if (!out) {
            std::fprintf(stderr, "[tvshell] cannot create a nested output\n");
        } else {
            wlr_wl_output_set_title(out, "TV Shell");
            fullscreenPending_ = opts_.fullscreen;
            if (fullscreenPending_ && wm_wl_output_set_fullscreen(out)) {
                fullscreenPending_ = false;
                std::fprintf(stderr, "[tvshell] nested window fullscreen\n");
            }
        }
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

    section_ = Section::Home;
    zone_ = Zone::Content;
    railFocus_ = int(Section::Home);
    reloadApps();

    homeSpring_.configureIOS(0.32, 0.90);
    homeSpring_.value = 1.0;
    homeTarget_ = 1.0;

    focusScale_.configureIOS(0.22, 0.80);
    focusScale_.value = 1.0;

    gridScroll_.configureIOS(0.34, 0.86);
    gridScroll_.value = 0.0;

    ccSpring_.configureIOS(0.30, 0.90);
    ccSpring_.value = 0.0;

    controllers_.init(evloop_, [this](const PadEvent& event) { onController(event); });
    // The Control Centre reads the machine through SystemControls: radios,
    // audio, backlight and the media player are all probed off the frame path.
    sysctl_.init();
    sysctl_.requestRefresh();
    if (controllers_.count() == 0)
        std::fprintf(stderr,
                     "[tvshell] no controller found (check /dev/input permissions)\n");

    tickTimer_ = wl_event_loop_add_timer(
        evloop_,
        [](void* data) -> int {
            Manager* m = static_cast<Manager*>(data);
            const double now = nowMs();
            m->controllers_.pollRepeats(now);
            // A finished probe is drained here, off the render path, so the
            // panel never waits on a helper process.
            if (m->sysctl_.poll()) {
                m->dirty_ = true;
                m->requestFrame();
            }
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
            if (m->tickTimer_)
                wl_event_source_timer_update(m->tickTimer_, m->minimal_ ? 100 : 50);
            return 0;
        },
        this);
    if (tickTimer_) wl_event_source_timer_update(tickTimer_, minimal_ ? 100 : 50);

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

    // Detach the display before destroying it. Tearing the display down emits
    // the output's `destroy` signal, whose handler calls stop(); calling
    // wl_display_terminate() on a display that is already being destroyed
    // aborts inside libwayland.
    struct wl_display* display = display_;
    display_ = nullptr;
    evloop_ = nullptr;
    wl_display_destroy_clients(display);
    wl_display_destroy(display);
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
    // A nested output already carries the size its host window gave it. Asking
    // for a custom mode there is rejected and leaves the output's swapchain
    // unable to pass its test, which is why the nested window never mapped.
    if (output->current_mode) {
        wlr_output_state_set_mode(&state, output->current_mode);
    } else if (output->width <= 0 || output->height <= 0) {
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
    // A nested output's window is only mapped once its surface has a buffer,
    // so the first frame has to be committed explicitly here; no input or
    // timer event is guaranteed to arrive and ask for one.
    dirty_ = true;
    requestFrame();
}

void Manager::onOutputFrame() {
    // A nested output's host toplevel only exists once its surface has been
    // mapped, so a fullscreen request made at creation can arrive too early.
    // Retry on the first frame callbacks, by which point it is guaranteed.
    if (fullscreenPending_ && output_ && fullscreenAttempts_ < 60) {
        ++fullscreenAttempts_;
        if (wm_wl_output_set_fullscreen(output_)) {
            fullscreenPending_ = false;
            std::fprintf(stderr, "[tvshell] nested window fullscreen\n");
        }
    }
    renderFrameOnOutput();
}

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
    rebuildRows();
    if (ccOpen_) layoutControlCenter();
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
    compositor_.present();
    // present() is what flushes the last queued batch, so a dump taken before
    // it loses the final group of draw calls -- which is the footer's hint text,
    // the last thing painted every frame.
    if (!opts_.dump.empty() && opts_.frames > 0 && framesRendered_ + 1 >= opts_.frames)
        dumpFrame();
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

bool Manager::tick(double now) {
    double dt = (now - lastTick_) / 1000.0;
    lastTick_ = now;
    if (dt <= 0.0 || dt > 0.1) dt = 1.0 / 60.0;

    const double scaleTarget = (home_ && zone_ == Zone::Content) ? 1.0 : 0.0;
    const LauncherLayout layout = launcherLayout();
    const double step = double(layout.cardW + layout.gap);

    // Minimal mode writes the resting value instead of running the spring. A
    // remote press then costs exactly one frame, and the shell never redraws a
    // whole 720p screen just to glide a card into place.
    if (minimal_) {
        homeSpring_.value = homeTarget_;
        homeSpring_.velocity = 0.0;
        focusScale_.value = scaleTarget;
        focusScale_.velocity = 0.0;
        for (size_t i = 0; i < rowScroll_.size() && i < rows_.size(); ++i) {
            const int count = int(rows_[i].cards.size());
            const double maxScroll =
                std::max(0.0, double(count) * step - double(layout.contentW));
            const double target = (int(i) == row_ && zone_ == Zone::Content)
                                      ? std::clamp(double(col_) * step, 0.0, maxScroll)
                                      : std::clamp(rowScroll_[i].value, 0.0, maxScroll);
            rowScroll_[i].value = target;
            rowScroll_[i].velocity = 0.0;
        }
        gridScroll_.value = gridScrollTarget(layout);
        gridScroll_.velocity = 0.0;
        ccSpring_.value = ccOpen_ ? 1.0 : 0.0;
        ccSpring_.velocity = 0.0;
        return false;
    }

    bool moved = false;
    if (!homeSpring_.settled(homeTarget_, 0.001)) {
        homeSpring_.step(homeTarget_, dt);
        moved = true;
    } else {
        homeSpring_.value = homeTarget_;
    }

    // Only the card strip the controller is standing on grows; leaving the
    // content column (or the launcher) lets every card settle back to its rest
    // size. One spring is enough because at most one card is focused.
    if (!focusScale_.settled(scaleTarget, 0.001)) {
        focusScale_.step(scaleTarget, dt);
        moved = true;
    } else {
        focusScale_.value = scaleTarget;
    }

    // Each row keeps its focused card in view by scrolling a strip, the way a
    // TV home screen slides its row rather than paging it.
    for (size_t i = 0; i < rowScroll_.size() && i < rows_.size(); ++i) {
        const int count = int(rows_[i].cards.size());
        const double maxScroll =
            std::max(0.0, double(count) * step - double(layout.contentW));
        double target = 0.0;
        if (int(i) == row_ && zone_ == Zone::Content)
            target = std::clamp(double(col_) * step, 0.0, maxScroll);
        else
            target = std::clamp(rowScroll_[i].value, 0.0, maxScroll);
        if (!rowScroll_[i].settled(target, 0.5)) {
            rowScroll_[i].step(target, dt);
            moved = true;
        } else {
            rowScroll_[i].value = target;
        }
    }

    const double gridTarget = gridScrollTarget(layout);
    if (!gridScroll_.settled(gridTarget, 0.5)) {
        gridScroll_.step(gridTarget, dt);
        moved = true;
    } else {
        gridScroll_.value = gridTarget;
    }

    const double ccTarget = ccOpen_ ? 1.0 : 0.0;
    if (!ccSpring_.settled(ccTarget, 0.001)) {
        ccSpring_.step(ccTarget, dt);
        moved = true;
    } else {
        ccSpring_.value = ccTarget;
    }
    if (moved) dirty_ = true;
    return moved;
}

void Manager::render() {
    const bool appVisible = active_ && active_->mapped && active_->tex.valid();
    // A fullscreen app already covers the output; painting the backdrop under it
    // is pure fill rate, which is the first thing a Pi 3 runs out of.
    if (!appVisible) drawBackground();
    if (appVisible) drawClient();
    if (homeSpring_.value > 0.001) drawHome();
    if (ccOpen_ || ccSpring_.value > 0.001) drawControlCenter();
    drawCursor();
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
    // wlroots' output framebuffer is already top-down in GLES2 here (the same
    // order the desktop build's dump uses), so the rows are written as read.
    for (int y = 0; y < h; ++y) {
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
        compositor_.importBuffer(&c->surface->buffer->base, &c->tex);
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
    if (device->type == WLR_INPUT_DEVICE_KEYBOARD) {
        setupKeyboard(wlr_keyboard_from_input_device(device));
        return;
    }
    // A pointer only reaches the cursor once it is attached to it; without this
    // the shell never sees a motion or a button.
    if (device->type == WLR_INPUT_DEVICE_POINTER) {
        pointerPresent_ = true;
        if (cursor_) wlr_cursor_attach_input_device(cursor_, device);
    }
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

    if (ccOpen_) {
        if (!pressed) return;
        switch (sym) {
            case XKB_KEY_Left: moveCcFocus(-1, 0); return;
            case XKB_KEY_Right: moveCcFocus(1, 0); return;
            case XKB_KEY_Up: moveCcFocus(0, -1); return;
            case XKB_KEY_Down: moveCcFocus(0, 1); return;
            case XKB_KEY_Return:
            case XKB_KEY_KP_Enter: activateCcFocus(); return;
            case XKB_KEY_Escape:
            case XKB_KEY_BackSpace:
            case XKB_KEY_XF86Back:
            case XKB_KEY_Menu:
            case XKB_KEY_XF86MenuKB: toggleControlCenter(); return;
            case XKB_KEY_Home:
            case XKB_KEY_XF86HomePage:
                setHome(true);
                toggleControlCenter();
                return;
            default: return;
        }
    }

    // A Fire TV remote is an ordinary Bluetooth keyboard to Linux: the d-pad
    // arrives as arrow keys, select as Return, back as KEY_BACK (XKB's
    // XF86Back) and home as KEY_HOMEPAGE (XF86HomePage). Those have to drive
    // the launcher too, not just the desktop keys.
    if (pressed && (sym == XKB_KEY_Home || sym == XKB_KEY_XF86HomePage)) {
        setHome(true);
        setSection(Section::Home);
        zone_ = Zone::Content;
        return;
    }
    if (!home_ && active_ && pressed &&
        (sym == XKB_KEY_Escape || sym == XKB_KEY_BackSpace || sym == XKB_KEY_XF86Back)) {
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
            case XKB_KEY_KP_Enter: activateFocused(); break;
            case XKB_KEY_Escape:
            case XKB_KEY_BackSpace:
            case XKB_KEY_XF86Back: launcherBack(); break;
            // The Fire TV remote's menu button cycles the sections, like the
            // Menu/Options button on a pad.
            case XKB_KEY_Menu:
            case XKB_KEY_XF86MenuKB: toggleControlCenter(); break;
            case XKB_KEY_Page_Up: cycleSection(-1); break;
            case XKB_KEY_Page_Down: cycleSection(1); break;
            default: break;
        }
        return;
    }
    forwardKey(keycode, pressed);
}

void Manager::onController(const PadEvent& event) {
    if (ccOpen_) {
        if (!event.pressed) return;
        switch (event.button) {
            case PadButton::Up: moveCcFocus(0, -1); break;
            case PadButton::Down: moveCcFocus(0, 1); break;
            case PadButton::Left: moveCcFocus(-1, 0); break;
            case PadButton::Right: moveCcFocus(1, 0); break;
            case PadButton::Select: activateCcFocus(); break;
            case PadButton::Back:
            case PadButton::Menu: toggleControlCenter(); break;
            case PadButton::Home:
                setHome(true);
                toggleControlCenter();
                break;
            default: break;
        }
        return;
    }

    if (home_) {
        if (!event.pressed) return;
        switch (event.button) {
            case PadButton::Up: moveFocus(0, -1); break;
            case PadButton::Down: moveFocus(0, 1); break;
            case PadButton::Left: moveFocus(-1, 0); break;
            case PadButton::Right: moveFocus(1, 0); break;
            case PadButton::Select: activateFocused(); break;
            case PadButton::Back: launcherBack(); break;
            case PadButton::Home:
                setSection(Section::Home);
                zone_ = Zone::Content;
                break;
            case PadButton::Menu: toggleControlCenter(); break;
            case PadButton::PageLeft: cycleSection(-1); break;
            case PadButton::PageRight: cycleSection(1); break;
        }
        return;
    }

    if (event.button == PadButton::Home || event.button == PadButton::Back) {
        if (event.pressed) setHome(true);
        return;
    }
    // Menu is the quick-settings button everywhere, over an app as well.
    if (event.button == PadButton::Menu) {
        if (event.pressed) toggleControlCenter();
        return;
    }
    if (!active_) {
        if (event.pressed) setHome(true);
        return;
    }
    forwardKey(padKey(event.button), event.pressed);
}

void Manager::onPointerMotion(double x, double y) {
    ptrX_ = x;
    ptrY_ = y;
    pointerSeen_ = true;
    // The cursor is drawn by the shell, so every move needs a frame even when
    // nothing else changed: the previous position has to be erased.
    dirty_ = true;
    requestFrame();
}

void Manager::onPointerButton(uint32_t button, bool pressed) {
    if (!pressed || button != BTN_LEFT || !cursor_) return;
    const int x = int(std::lround(cursor_->x));
    const int y = int(std::lround(cursor_->y));

    // The clock is the Control Centre's handle, exactly as it is on the
    // desktop shell. Only while the launcher (and therefore the clock) is
    // actually on screen.
    if (home_ && homeSpring_.value > 0.5 && clockHitRect().contains(x, y)) {
        toggleControlCenter();
        return;
    }

    if (!ccOpen_) return;
    for (size_t i = 0; i < ccControls_.size(); ++i) {
        if (!ccControls_[i].rect.contains(x, y)) continue;
        ccFocus_ = int(i);
        activateCcControl(ccControls_[i].id);
        return;
    }
    // Clicking the panel's padding keeps it; clicking outside dismisses it.
    if (!ccRect_.contains(x, y)) toggleControlCenter();
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

Section Manager::section() const { return section_; }

LauncherLayout Manager::launcherLayout() const {
    LauncherLayout L;
    const int w = screenW_;
    const int h = screenH_;

    L.margin = std::clamp(w / 48, 20, 40);
    L.railW = std::clamp(w / 14, 72, 108);
    L.contentX = L.railW + L.margin;
    L.contentW = std::max(240, w - L.contentX - L.margin);
    L.topH = std::clamp(h / 11, 54, 88);
    L.footerH = std::max(26, h / 28);
    L.gap = std::clamp(w / 120, 10, 20);
    L.heroH = std::clamp(h * 21 / 100, 100, 232);
    L.captionH = std::clamp(h / 42, 20, 32);
    L.railItemH = std::clamp(h / 7, 58, 92);

    // The grid is the part of the screen that has to fill exactly: the hero and
    // the footer are fixed sizes, so the rows share whatever is left between
    // them. A row is `visibleRows` of that space, which is what makes a page of
    // rows land on the same baseline every time and never leave a half card
    // sliced by the hero or the footer.
    L.rowsTop = L.topH + L.heroH + std::max(10, h / 30);
    L.gridBottom = h - L.footerH;
    const int available = std::max(120, L.gridBottom - L.rowsTop);
    L.visibleRows = std::clamp(
        int(std::lround(double(available) / (double(h) * 0.19))), 2, 4);
    L.rowStride = (available + L.gap) / L.visibleRows;
    L.cardH = std::max(84, L.rowStride - L.captionH - 2 * L.gap);

    // Cards keep roughly a 1:1.2 portrait ratio; the column count follows from
    // that, then the width is stretched so a full row spans the content column
    // with no ragged gap at the right edge.
    L.iconsPerRow = std::clamp(
        int(std::lround(double(L.contentW) / (double(L.cardH) * 1.18))), 3, 10);
    L.cardW = (L.contentW - (L.iconsPerRow - 1) * L.gap) / L.iconsPerRow;
    return L;
}

double Manager::gridScrollTarget(const LauncherLayout& layout) const {
    if (rows_.empty()) return 0.0;
    const int pageRows = std::max(1, layout.visibleRows);
    const int maxPage = (int(rows_.size()) - 1) / pageRows;
    const int page = std::min(row_ / pageRows, maxPage);
    return double(page * pageRows * layout.rowStride);
}

void Manager::reloadApps() {
    std::vector<TvApp> fresh;
    for (wm::AppEntry& entry : wm::scanApps()) {
        if (entry.name.empty() || entry.exec.empty() || entry.exec == wm::kSettingsExec ||
            entry.terminal)
            continue;
        TvApp app;
        app.entry = std::move(entry);
        fresh.push_back(std::move(app));
    }
    apps_ = std::move(fresh);

    // Drop recents whose launcher has gone away since the last scan.
    recentExecs_.erase(
        std::remove_if(recentExecs_.begin(), recentExecs_.end(),
                       [this](const std::string& exec) {
                           return std::none_of(apps_.begin(), apps_.end(),
                                               [&exec](const TvApp& a) {
                                                   return a.entry.exec == exec;
                                               });
                       }),
        recentExecs_.end());
    rebuildRows();
}

void Manager::rebuildRows() {
    rows_.clear();
    const auto appIndices = [this]() {
        std::vector<int> indices;
        indices.reserve(apps_.size());
        for (size_t i = 0; i < apps_.size(); ++i) indices.push_back(int(i));
        return indices;
    };
    const auto recentIndices = [this]() {
        std::vector<int> indices;
        for (const std::string& exec : recentExecs_) {
            for (size_t i = 0; i < apps_.size(); ++i) {
                if (apps_[i].entry.exec == exec) {
                    indices.push_back(int(i));
                    break;
                }
            }
        }
        return indices;
    };
    const LauncherLayout layout = launcherLayout();
    const size_t cols = size_t(std::max(1, layout.iconsPerRow));

    // One long strip that the row slides along, the way a TV home screen
    // carousels its shelves.
    const auto addStrip = [this](const char* title, const std::vector<int>& indices) {
        if (indices.empty()) return;
        Row row;
        row.title = title;
        for (int index : indices) row.cards.push_back(CardRef{index, CardAction::None});
        rows_.push_back(std::move(row));
    };
    // A grid: the list is cut into rows that hold one screen of columns, so the
    // grid pages vertically while each row stays put. Continuation rows keep an
    // empty caption, which is the spacing between them.
    const auto addGrid = [this, cols](const char* title, const std::vector<int>& indices) {
        for (size_t start = 0; start < indices.size(); start += cols) {
            Row row;
            if (start == 0) row.title = title;
            const size_t end = std::min(indices.size(), start + cols);
            for (size_t i = start; i < end; ++i)
                row.cards.push_back(CardRef{indices[i], CardAction::None});
            rows_.push_back(std::move(row));
        }
    };

    switch (section_) {
        case Section::Home:
            // The way Android TV's home screen works when there is nothing to
            // resume: a Continue shelf if anything has been opened, then the
            // app grid itself, so the screen is never one lonely row.
            addStrip("Continue", recentIndices());
            addGrid("Your apps", appIndices());
            break;
        case Section::Apps:
            addGrid("All apps", appIndices());
            break;
        case Section::Recent:
            addGrid("Recently used", recentIndices());
            break;
        case Section::Settings: {
            Row row;
            row.title = "System";
            row.cards.push_back(CardRef{-1, CardAction::RefreshApps});
            row.cards.push_back(CardRef{-1, CardAction::QuitShell});
            rows_.push_back(std::move(row));
            break;
        }
    }

    rowScroll_.resize(rows_.size());
    for (auto& spring : rowScroll_) spring.configureIOS(0.30, 0.90);

    if (rows_.empty()) {
        row_ = 0;
        col_ = 0;
        zone_ = Zone::Rail;
    } else {
        row_ = std::clamp(row_, 0, int(rows_.size()) - 1);
        col_ = std::clamp(col_, 0, int(rows_[size_t(row_)].cards.size()) - 1);
    }
    dirty_ = true;
    requestFrame();
}

void Manager::setSection(Section s) {
    if (section_ == s && !rows_.empty()) {
        railFocus_ = int(s);
        return;
    }
    section_ = s;
    railFocus_ = int(s);
    row_ = 0;
    col_ = 0;
    rebuildRows();
    dirty_ = true;
    requestFrame();
}

void Manager::trackRecent(const std::string& exec) {
    if (exec.empty()) return;
    recentExecs_.erase(std::remove(recentExecs_.begin(), recentExecs_.end(), exec),
                       recentExecs_.end());
    recentExecs_.insert(recentExecs_.begin(), exec);
    if (recentExecs_.size() > 8) recentExecs_.resize(8);
    rebuildRows();
}

const CardRef* Manager::focusedCard() const {
    if (row_ < 0 || row_ >= int(rows_.size())) return nullptr;
    const Row& row = rows_[size_t(row_)];
    if (col_ < 0 || col_ >= int(row.cards.size())) return nullptr;
    return &row.cards[size_t(col_)];
}

int Manager::focusedApp() const {
    const CardRef* card = focusedCard();
    return card ? card->app : -1;
}

void Manager::moveRail(int dy) {
    const int next = std::clamp(railFocus_ + dy, 0, 3);
    if (next == railFocus_) return;
    railFocus_ = next;
    setSection(Section(railFocus_));
    dirty_ = true;
    requestFrame();
}

void Manager::moveContent(int dx, int dy) {
    if (rows_.empty()) {
        zone_ = Zone::Rail;
        dirty_ = true;
        requestFrame();
        return;
    }
    Row& row = rows_[size_t(row_)];
    const int count = int(row.cards.size());
    if (dx != 0) {
        if (count == 0) return;
        const int next = std::clamp(col_ + dx, 0, count - 1);
        if (next == col_) return;
        col_ = next;
    } else if (dy < 0) {
        if (row_ > 0) {
            --row_;
            col_ = std::max(0, std::min(col_, int(rows_[size_t(row_)].cards.size()) - 1));
        } else {
            // Up from the first row hands the focus back to the section rail,
            // which is where a TV remote expects to land.
            zone_ = Zone::Rail;
            railFocus_ = int(section_);
        }
    } else if (dy > 0) {
        if (row_ + 1 >= int(rows_.size())) return;
        ++row_;
        col_ = std::max(0, std::min(col_, int(rows_[size_t(row_)].cards.size()) - 1));
    } else {
        return;
    }
    dirty_ = true;
    requestFrame();
}

void Manager::moveFocus(int dx, int dy) {
    if (!home_) return;
    if (zone_ == Zone::Rail) {
        if (dx > 0) {
            zone_ = Zone::Content;
            row_ = 0;
            col_ = rows_.empty() ? 0 : std::min(col_, int(rows_[0].cards.size()) - 1);
            dirty_ = true;
            requestFrame();
            return;
        }
        if (dx < 0) return;
        moveRail(dy);
        return;
    }
    if (dx != 0)
        moveContent(dx, 0);
    else
        moveContent(0, dy);
}

void Manager::cycleSection(int delta) {
    const int next = std::clamp(railFocus_ + delta, 0, 3);
    if (next == railFocus_) return;
    railFocus_ = next;
    setSection(Section(railFocus_));
    zone_ = Zone::Rail;
    dirty_ = true;
    requestFrame();
}

void Manager::launcherBack() {
    if (!home_) return;
    if (zone_ == Zone::Content) {
        zone_ = Zone::Rail;
        railFocus_ = int(section_);
    } else if (section_ != Section::Home) {
        setSection(Section::Home);
        zone_ = Zone::Rail;
        return;
    } else {
        return;
    }
    dirty_ = true;
    requestFrame();
}

void Manager::activateFocused() {
    if (!home_) return;
    if (zone_ == Zone::Rail) {
        zone_ = Zone::Content;
        row_ = 0;
        col_ = rows_.empty() ? 0 : std::min(col_, int(rows_[0].cards.size()) - 1);
        dirty_ = true;
        requestFrame();
        return;
    }
    const CardRef* card = focusedCard();
    if (!card) return;
    switch (card->action) {
        case CardAction::RefreshApps: reloadApps(); return;
        case CardAction::QuitShell: stop(); return;
        case CardAction::None: break;
    }
    if (card->app < 0 || card->app >= int(apps_.size())) return;
    launchEntry(apps_[size_t(card->app)].entry);
}

void Manager::launchFocus() { activateFocused(); }

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
    trackRecent(entry.exec);
    if (Client* running = clientForApp(entry)) {
        active_ = running;
        setHome(false);
        focusClient(running);
        return;
    }
    wm::launchApp(entry.exec);
}

void Manager::ensureIcons() {
    const LauncherLayout layout = launcherLayout();
    const int px = std::max(48, std::min(layout.cardH * 55 / 100, layout.cardW));
    for (auto& app : apps_) {
        if (app.iconTried) continue;
        app.iconTried = true;
        app.icon = icons_.forApp(app.entry.icon, app.entry.wmClass, px,
                                 wm::IconTheme::Hatter);
    }
    if (chromeIconsTried_) return;
    chromeIconsTried_ = true;
    const int railPx = std::max(22, layout.railItemH / 3);
    railIcons_[0] = icons_.get("go-home", railPx);
    railIcons_[1] = icons_.get("view-grid", railPx);
    railIcons_[2] = icons_.get("lucide-play", railPx);
    railIcons_[3] = icons_.get("systemsettings", railPx);
    iconRefresh_ = icons_.get("view-grid", px);
    iconQuit_ = icons_.get("lucide-power", px);
}

}  // namespace tv
