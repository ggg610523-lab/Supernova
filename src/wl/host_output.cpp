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
#else
bool wm_wl_output_set_fullscreen(struct wlr_output*) { return false; }
#endif
