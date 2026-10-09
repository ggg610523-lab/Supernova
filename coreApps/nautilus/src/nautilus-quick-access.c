/*
 * nautilus-quick-access.c: pinned folders shown on the Home page.
 *
 * The pinned folders are stored in the "quick-access-pins" GSettings key as an
 * ordered list of URIs. They show up both on the Home dashboard and in the
 * navigation pane. On first use the list is seeded with Desktop, Downloads and
 * Documents.
 *
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nautilus-quick-access.h"

#include <gtk/gtk.h>

#define QUICK_ACCESS_PINS_KEY         "quick-access-pins"
#define QUICK_ACCESS_INITIALIZED_KEY  "quick-access-pins-initialized"

static GSettings *settings = NULL;

GSettings *
nautilus_quick_access_settings (void)
{
    if (settings == NULL)
    {
        settings = g_settings_new ("org.gnome.nautilus.preferences");
    }

    return settings;
}

static const GUserDirectory default_folder_dirs[] =
{
    G_USER_DIRECTORY_DESKTOP,
    G_USER_DIRECTORY_DOWNLOAD,
    G_USER_DIRECTORY_DOCUMENTS,
};

static void
ensure_defaults (void)
{
    GSettings *s = nautilus_quick_access_settings ();
    GPtrArray *uris;

    if (g_settings_get_boolean (s, QUICK_ACCESS_INITIALIZED_KEY))
    {
        return;
    }

    uris = g_ptr_array_new_with_free_func (g_free);

    /* The standard user folders, in the order they are pinned by default. */
    for (gsize i = 0; i < G_N_ELEMENTS (default_folder_dirs); i++)
    {
        const char *path = g_get_user_special_dir (default_folder_dirs[i]);

        /* Disabled or missing directories point at $HOME. */
        if (path == NULL || g_strcmp0 (path, g_get_home_dir ()) == 0)
        {
            continue;
        }

        g_autoptr (GFile) file = g_file_new_for_path (path);

        if (g_file_query_exists (file, NULL))
        {
            g_autofree char *uri = g_file_get_uri (file);

            g_ptr_array_add (uris, g_steal_pointer (&uri));
        }
    }

    g_ptr_array_add (uris, NULL);

    g_settings_set_strv (s, QUICK_ACCESS_PINS_KEY, (const gchar * const *) uris->pdata);
    g_settings_set_boolean (s, QUICK_ACCESS_INITIALIZED_KEY, TRUE);

    g_ptr_array_unref (uris);
}

GList *
nautilus_quick_access_get_pins (void)
{
    GSettings *s = nautilus_quick_access_settings ();
    g_auto (GStrv) strv = NULL;
    GList *pins = NULL;

    ensure_defaults ();

    strv = g_settings_get_strv (s, QUICK_ACCESS_PINS_KEY);

    if (strv == NULL)
    {
        return NULL;
    }

    for (gsize i = 0; strv[i] != NULL; i++)
    {
        g_autoptr (GFile) file = g_file_new_for_uri (strv[i]);

        pins = g_list_append (pins, g_object_ref (file));
    }

    return pins;
}

const char *
nautilus_quick_access_icon_name (GFile   *location,
                                 gboolean symbolic)
{
    static const struct
    {
        GUserDirectory dir;
        const char *symbolic_icon;
        const char *full_icon;
    } map[] =
    {
        { G_USER_DIRECTORY_DESKTOP,   "user-desktop-symbolic",    "user-desktop"    },
        { G_USER_DIRECTORY_DOCUMENTS, "folder-documents-symbolic", "folder-documents" },
        { G_USER_DIRECTORY_DOWNLOAD,  "folder-download-symbolic",  "folder-download"  },
        { G_USER_DIRECTORY_MUSIC,     "folder-music-symbolic",     "folder-music"     },
        { G_USER_DIRECTORY_PICTURES,  "folder-pictures-symbolic",  "folder-pictures"  },
        { G_USER_DIRECTORY_VIDEOS,    "folder-videos-symbolic",    "folder-videos"    },
    };
    g_autofree char *path = NULL;

    g_return_val_if_fail (G_IS_FILE (location), symbolic ? "folder-symbolic" : "folder");

    path = g_file_get_path (location);

    if (path == NULL)
    {
        return symbolic ? "folder-symbolic" : "folder";
    }

    for (gsize i = 0; i < G_N_ELEMENTS (map); i++)
    {
        const char *dir = g_get_user_special_dir (map[i].dir);

        if (dir != NULL && g_strcmp0 (dir, path) == 0)
        {
            return symbolic ? map[i].symbolic_icon : map[i].full_icon;
        }
    }

    if (g_strcmp0 (path, g_get_home_dir ()) == 0)
    {
        return symbolic ? "user-home-symbolic" : "user-home";
    }

    return symbolic ? "folder-symbolic" : "folder";
}

gboolean
nautilus_quick_access_is_pinned (GFile *location)
{
    g_autofree char *uri = NULL;
    g_auto (GStrv) strv = NULL;
    gboolean pinned = FALSE;

    g_return_val_if_fail (G_IS_FILE (location), FALSE);

    uri = g_file_get_uri (location);
    strv = g_settings_get_strv (nautilus_quick_access_settings (), QUICK_ACCESS_PINS_KEY);

    if (strv != NULL)
    {
        pinned = g_strv_contains ((const gchar * const *) strv, uri);
    }

    return pinned;
}

static void
set_pins (GPtrArray *pins)
{
    g_ptr_array_add (pins, NULL);
    g_settings_set_strv (nautilus_quick_access_settings (), QUICK_ACCESS_PINS_KEY,
                         (const gchar * const *) pins->pdata);
}

void
nautilus_quick_access_pin (GFile *location)
{
    g_autofree char *uri = NULL;
    g_auto (GStrv) strv = NULL;
    GPtrArray *pins;

    g_return_if_fail (G_IS_FILE (location));

    if (nautilus_quick_access_is_pinned (location))
    {
        return;
    }

    ensure_defaults ();

    uri = g_file_get_uri (location);
    strv = g_settings_get_strv (nautilus_quick_access_settings (), QUICK_ACCESS_PINS_KEY);
    pins = g_ptr_array_new_with_free_func (g_free);

    if (strv != NULL)
    {
        for (gsize i = 0; strv[i] != NULL; i++)
        {
            g_ptr_array_add (pins, g_strdup (strv[i]));
        }
    }
    g_ptr_array_add (pins, g_steal_pointer (&uri));

    set_pins (pins);
    g_ptr_array_unref (pins);
}

void
nautilus_quick_access_unpin (GFile *location)
{
    g_autofree char *uri = NULL;
    g_auto (GStrv) strv = NULL;
    GPtrArray *pins;

    g_return_if_fail (G_IS_FILE (location));

    uri = g_file_get_uri (location);
    strv = g_settings_get_strv (nautilus_quick_access_settings (), QUICK_ACCESS_PINS_KEY);
    pins = g_ptr_array_new_with_free_func (g_free);

    if (strv != NULL)
    {
        for (gsize i = 0; strv[i] != NULL; i++)
        {
            if (g_strcmp0 (strv[i], uri) != 0)
            {
                g_ptr_array_add (pins, g_strdup (strv[i]));
            }
        }
    }

    set_pins (pins);
    g_ptr_array_unref (pins);
}
