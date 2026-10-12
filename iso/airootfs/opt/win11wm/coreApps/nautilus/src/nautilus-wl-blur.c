/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Windows 11-style Mica backdrop for Nautilus on KDE Plasma.
 *
 * GTK4/libadwaita cannot blur the desktop behind a window by itself, so the
 * window chrome only fades it out (see the translucent --win11-* colours in
 * style.css). Plasma exposes background blur to clients through the
 * ext-background-effect-v1 Wayland protocol (KWin >= 6.1); when running under
 * such a compositor we send a full-surface blur region so the desktop shows
 * through blurred, like the Win11-gtk-theme Mica previews.
 */

#include <config.h>

#include <gtk/gtk.h>

#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#include <wayland-client.h>
#endif
#ifdef GDK_WINDOWING_X11
#include <gdk/x11/gdkx.h>
#include <X11/Xatom.h>
#endif

#include "nautilus-wl-blur.h"

#ifdef GDK_WINDOWING_WAYLAND

#include "ext-background-effect-client.h"

typedef struct
{
    GdkDisplay *display;
    struct wl_display *wl_display;
    struct wl_compositor *wl_compositor;
    struct ext_background_effect_manager_v1 *manager;
} NautilusWlBlurState;

static NautilusWlBlurState blur_state;

static void
registry_handle_global (gpointer             data,
                        struct wl_registry *registry,
                        guint32             name,
                        const char         *interface,
                        guint32             version)
{
    NautilusWlBlurState *state = data;

    if (state->manager == NULL &&
        g_strcmp0 (interface, "ext_background_effect_manager_v1") == 0)
    {
        state->manager = wl_registry_bind (registry,
                                           name,
                                           &ext_background_effect_manager_v1_interface,
                                           1);
        g_message ("nautilus-wl-blur: bound ext_background_effect_manager_v1");
    }
}

static const struct wl_registry_listener registry_listener = {
    registry_handle_global,
    NULL,
};

static void
nblur_ensure_manager (GdkDisplay *display)
{
    if (blur_state.wl_display != NULL || !GDK_IS_WAYLAND_DISPLAY (display))
    {
        return;
    }

    blur_state.display = display;
    blur_state.wl_display = gdk_wayland_display_get_wl_display (display);
    blur_state.wl_compositor = gdk_wayland_display_get_wl_compositor (display);

    if (blur_state.wl_display == NULL)
    {
        return;
    }

    struct wl_registry *registry = wl_display_get_registry (blur_state.wl_display);
    wl_registry_add_listener (registry, &registry_listener, &blur_state);

    /* The compositor delivers the global list asynchronously; dispatch once
     * here so the manager is bound before the first window maps. */
    wl_display_roundtrip (blur_state.wl_display);
}

static gboolean
nblur_surface_is_wayland (GdkSurface *surface)
{
    return GDK_IS_WAYLAND_SURFACE (surface);
}

static void
nblur_apply_wayland (GdkSurface *surface)
{
    static GHashTable *applied = NULL;

    if (surface == NULL || !nblur_surface_is_wayland (surface))
    {
        return;
    }

    nblur_ensure_manager (gdk_surface_get_display (surface));

    if (blur_state.manager == NULL || blur_state.wl_compositor == NULL)
    {
        g_message ("nautilus-wl-blur: cannot apply (manager=%p compositor=%p)",
                   (void *) blur_state.manager, (void *) blur_state.wl_compositor);
        return;
    }

    if (applied == NULL)
    {
        applied = g_hash_table_new (g_direct_hash, g_direct_equal);
    }
    if (g_hash_table_contains (applied, surface))
    {
        return;
    }

    struct wl_surface *wl_surface = gdk_wayland_surface_get_wl_surface (surface);
    if (wl_surface == NULL)
    {
        return;
    }

    struct ext_background_effect_surface_v1 *effect =
        ext_background_effect_manager_v1_get_background_effect (blur_state.manager,
                                                                wl_surface);
    struct wl_region *region = wl_compositor_create_region (blur_state.wl_compositor);
    wl_region_add (region, 0, 0, 0x7fffffff, 0x7fffffff);
    ext_background_effect_surface_v1_set_blur_region (effect, region);
    wl_region_destroy (region);
    g_message ("nautilus-wl-blur: applied blur region to surface");

    g_hash_table_add (applied, surface);
}

#endif /* GDK_WINDOWING_WAYLAND */

#ifdef GDK_WINDOWING_X11
/* X11 fallback: an X11 client cannot speak the Wayland background-effect
 * protocol, so ask the compositor for a blurred backdrop the cross-desktop way:
 * _KDE_NET_WM_BLUR_BEHIND_REGION (read by KWin and by win11wm). The value is a
 * CARDINAL rect list; a single box larger than any screen covers the window. */
static void
nblur_apply_x11 (GdkSurface *surface)
{
    static GHashTable *applied = NULL;

    if (surface == NULL || !GDK_IS_X11_SURFACE (surface))
    {
        return;
    }

    if (applied == NULL)
    {
        applied = g_hash_table_new (g_direct_hash, g_direct_equal);
    }
    if (g_hash_table_contains (applied, surface))
    {
        return;
    }

    Window xid = gdk_x11_surface_get_xid (surface);
    GdkDisplay *display = gdk_surface_get_display (surface);
    if (xid == 0 || display == NULL)
    {
        return;
    }

    Display *xdisplay = gdk_x11_display_get_xdisplay (display);
    Atom atom = gdk_x11_get_xatom_by_name_for_display (display,
                                                       "_KDE_NET_WM_BLUR_BEHIND_REGION");
    unsigned long region[4] = { 0, 0, 0x7fffffffUL, 0x7fffffffUL };
    XChangeProperty (xdisplay, xid, atom, XA_CARDINAL, 32, PropModeReplace,
                     (unsigned char *) region, 4);
    XFlush (xdisplay);
    g_message ("nautilus-wl-blur: set _KDE_NET_WM_BLUR_BEHIND_REGION on X11 window");

    g_hash_table_add (applied, surface);
}
#endif /* GDK_WINDOWING_X11 */

void
nautilus_wl_blur_setup_display (GdkDisplay *display)
{
#ifdef GDK_WINDOWING_WAYLAND
    if (display != NULL && GDK_IS_WAYLAND_DISPLAY (display))
    {
        nblur_ensure_manager (display);
    }
#endif
}

void
nautilus_wl_blur_surface (GdkSurface *surface)
{
    if (surface == NULL)
    {
        return;
    }
#ifdef GDK_WINDOWING_X11
    if (GDK_IS_X11_SURFACE (surface))
    {
        nblur_apply_x11 (surface);
        return;
    }
#endif
#ifdef GDK_WINDOWING_WAYLAND
    nblur_apply_wayland (surface);
#endif
}