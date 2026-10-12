/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

#ifndef NAUTILUS_WL_BLUR_H
#define NAUTILUS_WL_BLUR_H

#include <gtk/gtk.h>

typedef struct _GdkSurface GdkSurface;

void nautilus_wl_blur_setup_display (GdkDisplay *display);
void nautilus_wl_blur_surface (GdkSurface *surface);

#endif /* NAUTILUS_WL_BLUR_H */