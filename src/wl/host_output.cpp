#include "host_output.h"

#include "wlr.h"

#if __has_include(<backend/wayland.h>) && __has_include(<xdg-shell-client-protocol.h>)
#include <backend/wayland.h>
#include <xdg-shell-client-protocol.h>

bool wm_wl_output_set_fullscreen(struct wlr_output* output) {
    if (!output || !wlr_output_is_wl(output)) return false;
    struct wl_surface* surface = wlr_wl_output_get_surface(output);
    if (!surface) return false;
    auto* wlOutput = static_cast<struct wlr_wl_output*>(wl_surface_get_user_data(surface));
    if (!wlOutput || !wlOutput->xdg_toplevel) return false;
    xdg_toplevel_set_fullscreen(wlOutput->xdg_toplevel, nullptr);
    return true;
}

bool wm_wl_output_set_size(struct wlr_output* output, int w, int h) {
    if (!output || !wlr_output_is_wl(output) || w <= 0 || h <= 0) return false;
    struct wl_surface* surface = wlr_wl_output_get_surface(output);
    if (!surface) return false;
    auto* wlOutput = static_cast<struct wlr_wl_output*>(wl_surface_get_user_data(surface));
    if (!wlOutput || !wlOutput->xdg_toplevel) return false;
    // xdg-shell has no direct "set size"; equal min/max constraints make the
    // host resize the nested window to exactly the requested phone geometry.
    // KWin applies them on a configure, so commit the surface to request one.
    xdg_toplevel_set_min_size(wlOutput->xdg_toplevel, w, h);
    xdg_toplevel_set_max_size(wlOutput->xdg_toplevel, w, h);
    wl_surface_commit(surface);
    wl_display_flush(wl_proxy_get_display((struct wl_proxy*)surface));
    return true;
}
#else
bool wm_wl_output_set_fullscreen(struct wlr_output*) { return false; }
bool wm_wl_output_set_size(struct wlr_output*, int, int) { return false; }
#endif
