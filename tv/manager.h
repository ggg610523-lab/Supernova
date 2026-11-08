// The TV shell's compositor and policy.
//
// This is a native wlroots compositor, like the desktop Wayland build, but its
// policy is a ten-foot living-room UI rather than a desktop: apps launch
// fullscreen, a controller drives the launcher, and there is no taskbar,
// wallpaper or window chrome. The GLES2 renderer, text rasteriser, icon store
// and .desktop scanner are the same components the desktop shell uses.
#pragma once

extern "C" {
#include <wayland-server-core.h>
}

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "apps.h"
#include "compositor.h"
#include "controller.h"
#include "icons.h"
#include "motion.h"
#include "text.h"
#include "theme.h"
#include "util.h"

struct wl_display;
struct wlr_allocator;
struct wlr_backend;
struct wlr_input_device;
struct wlr_keyboard;
struct wlr_output;
struct wlr_output_layout;
struct wlr_presentation;
struct wlr_renderer;
struct wlr_seat;
struct wlr_surface;
struct wlr_xdg_shell;
struct wlr_xdg_surface;
struct wlr_xdg_toplevel;

namespace tv {

struct Options {
    bool headless = false;
    bool drm = false;
    bool noVsync = false;
    int width = 1280;
    int height = 720;
    int frames = 0;  // self-test: render N frames then exit
    std::string socket;
    std::string dump;
    std::vector<std::string> launch;
};

// A fullscreen app managed by the TV shell. Only the root xdg surface is drawn;
// popups/subsurfaces are a later milestone.
struct Client {
    class Manager* manager = nullptr;
    struct wl_listener* firing = nullptr;
    struct wlr_xdg_toplevel* toplevel = nullptr;
    struct wlr_xdg_surface* xdg = nullptr;
    struct wlr_surface* surface = nullptr;
    wm::WindowTex tex;
    bool mapped = false;
    bool configured = false;
    bool fullscreen = false;
    bool alive = true;
    std::string title;
    std::string appId;

    struct wl_listener surfaceCommit {};
    struct wl_listener surfaceMap {};
    struct wl_listener surfaceUnmap {};
    struct wl_listener surfaceDestroy {};
    struct wl_listener toplevelDestroy {};
    struct wl_listener toplevelTitle {};
    struct wl_listener toplevelAppId {};
    struct wl_listener requestFullscreen {};
    struct wl_listener requestClose {};
};

struct TvApp {
    wm::AppEntry entry;
    wm::IconTex icon;
    bool iconTried = false;
    std::string label;
};

class Manager {
public:
    Manager() = default;
    ~Manager();
    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;

    int run(const Options& options);
    void stop();

private:
    friend struct Client;

    bool initWayland();
    bool initShell();
    void shutdown();
    void loop();

    // ---- backend/output
    void onNewOutput(struct wlr_output* output);
    void onOutputFrame();
    void onOutputDestroy();
    void syncOutputSize();
    void requestFrame();
    void renderFrameOnOutput();
    void render();
    void tick(double now);
    void dumpFrame();

    // ---- xdg-shell
    void onNewToplevel(struct wlr_xdg_toplevel* toplevel);
    void onMap(Client* c);
    void onUnmap(Client* c);
    void onCommit(Client* c);
    void onDestroy(Client* c);
    void focusClient(Client* c);
    void clearClientFocus();

    // ---- input
    void onNewInput(struct wlr_input_device* device);
    void setupKeyboard(struct wlr_keyboard* keyboard);
    void onKey(uint32_t keycode, uint32_t modifiers, bool pressed);
    void onController(const PadEvent& event);
    void forwardKey(unsigned evdevCode, bool pressed);

    // ---- TV policy
    void setHome(bool home);
    void toggleHome();
    void moveFocus(int dx, int dy);
    void changePage(int delta);
    void selectFocus();
    void launchFocus();
    void launchEntry(const wm::AppEntry& entry);
    Client* clientForApp(const wm::AppEntry& entry) const;
    int perPage() const { return columns_ * rows_; }
    int pageCount() const;
    int currentPage() const;
    void layoutApps();
    void ensureIcons();

    // ---- painting
    void drawBackground();
    void drawClient();
    void drawHome();
    void drawTopBar(float opacity);
    void drawHints(float opacity);
    void drawTextAt(const std::string& text, int px, wm::Weight weight,
                    const wm::Color& color, int x, int y, float opacity);
    void drawTextCentered(const std::string& text, int px, wm::Weight weight,
                          const wm::Color& color, const wm::Rect& box, float opacity);

    Options opts_;
    struct wl_display* display_ = nullptr;
    struct wl_event_loop* evloop_ = nullptr;
    struct wlr_backend* backend_ = nullptr;
    struct wlr_renderer* renderer_ = nullptr;
    struct wlr_allocator* allocator_ = nullptr;
    struct wlr_output* output_ = nullptr;
    struct wlr_output_layout* outputLayout_ = nullptr;
    struct wlr_seat* seat_ = nullptr;
    struct wlr_keyboard* keyboard_ = nullptr;
    struct wlr_xdg_shell* xdgShell_ = nullptr;
    struct wlr_presentation* presentation_ = nullptr;

    struct wl_listener newOutput_ {};
    struct wl_listener newInput_ {};
    struct wl_listener newToplevel_ {};
    struct wl_listener outputFrame_ {};
    struct wl_listener outputDestroy_ {};
    struct wl_listener outputRequestState_ {};
    struct wl_listener keyboardKey_ {};
    struct wl_listener keyboardMods_ {};

    wm::Compositor compositor_;
    wm::Text text_;
    wm::IconStore icons_;
    ControllerManager controllers_;

    std::vector<std::unique_ptr<Client>> clients_;
    Client* active_ = nullptr;

    std::vector<TvApp> apps_;
    std::vector<wm::motion::Spring> tileScale_;
    int focus_ = 0;
    int columns_ = 5;
    int rows_ = 2;

    bool home_ = true;
    wm::motion::Spring homeSpring_;
    double homeTarget_ = 1.0;

    bool nested_ = false;
    bool running_ = false;
    bool dirty_ = true;
    int framesRendered_ = 0;
    double lastTick_ = 0.0;
    int lastClockMinute_ = -1;
    int screenW_ = 1280;
    int screenH_ = 720;
    double frameBackoffUntil_ = 0.0;
    struct wl_event_source* tickTimer_ = nullptr;

    std::string assetDir_;
    std::string socketName_;
};

}  // namespace tv
