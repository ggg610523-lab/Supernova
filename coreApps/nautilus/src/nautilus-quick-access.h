/*
 * nautilus-quick-access.h: pinned folders shown on the Home page.
 *
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* Returns a newly allocated list of #GFile, in display order. Free with
 * g_list_free_full (list, g_object_unref). */
GList *nautilus_quick_access_get_pins      (void);

gboolean nautilus_quick_access_is_pinned   (GFile *location);
void     nautilus_quick_access_pin         (GFile *location);
void     nautilus_quick_access_unpin       (GFile *location);

/* The shared settings object, for connecting to "changed::quick-access-pins". */
GSettings *nautilus_quick_access_settings  (void);

/* Icon name to use for a pinned folder. With @symbolic, the monochrome icon
 * used on the Home dashboard is returned; otherwise the full-colour icon used
 * by the navigation pane. */
const char *nautilus_quick_access_icon_name (GFile   *location,
                                             gboolean symbolic);

G_END_DECLS
