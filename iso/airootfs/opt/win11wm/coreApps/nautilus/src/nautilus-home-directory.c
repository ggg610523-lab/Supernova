/*
 * nautilus-home-directory.c: virtual directory backing the Home dashboard.
 *
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nautilus-home-directory.h"

#include <glib/gi18n.h>

#include "nautilus-file-utilities.h"
#include "nautilus-internal-place-file.h"
#include "nautilus-scheme.h"

struct _NautilusHomeDirectory
{
    NautilusDirectory parent_instance;
};

G_DEFINE_TYPE_WITH_CODE (NautilusHomeDirectory, nautilus_home_directory, NAUTILUS_TYPE_DIRECTORY,
                         nautilus_ensure_extension_points ();
                         /* It looks like you’re implementing an extension point.
                          * Did you modify nautilus_ensure_extension_builtins() accordingly?
                          *
                          * • Yes
                          * • Doing it right now
                          */
                         g_io_extension_point_implement (NAUTILUS_DIRECTORY_PROVIDER_EXTENSION_POINT_NAME,
                                                         g_define_type_id,
                                                         NAUTILUS_HOME_DIRECTORY_PROVIDER_NAME,
                                                         0));

/* The dashboard is not backed by a real filesystem directory: it never contains
 * any files. We only need the directory to resolve and load cleanly so that the
 * files view can host the dashboard widget instead of an item list. */

static void
real_call_when_ready (NautilusDirectory         *directory,
                      NautilusFileAttributes     file_attributes,
                      gboolean                   wait_for_file_list,
                      NautilusDirectoryCallback  callback,
                      gpointer                   callback_data)
{
    if (callback != NULL)
    {
        (*callback) (directory, NULL, callback_data);
    }
}

static void
real_cancel_callback (NautilusDirectory         *directory,
                      NautilusDirectoryCallback  callback,
                      gpointer                   callback_data)
{
    /* There is never a pending callback. */
}

static void
real_file_monitor_add (NautilusDirectory         *directory,
                       gconstpointer              client,
                       gboolean                   monitor_hidden_files,
                       NautilusFileAttributes     file_attributes,
                       NautilusDirectoryCallback  callback,
                       gpointer                   callback_data)
{
    if (callback != NULL)
    {
        (*callback) (directory, NULL, callback_data);
    }
}

static void
real_file_monitor_remove (NautilusDirectory *directory,
                          gconstpointer      client)
{
    /* No monitor state is kept. */
}

static void
real_force_reload (NautilusDirectory *directory)
{
    /* Nothing to reload. */
}

static GList *
real_get_file_list (NautilusDirectory *directory)
{
    return NULL;
}

static gboolean
real_are_all_files_seen (NautilusDirectory *directory)
{
    /* The dashboard is always fully loaded. */
    return TRUE;
}

static gboolean
real_is_not_empty (NautilusDirectory *directory)
{
    return FALSE;
}

static gboolean
real_is_editable (NautilusDirectory *directory)
{
    return FALSE;
}

static gboolean
real_handles_location (GFile *location)
{
    return g_file_has_uri_scheme (location, SCHEME_HOME);
}

static NautilusFile *
real_new_as_file (NautilusDirectory *directory)
{
    return g_object_new (NAUTILUS_TYPE_INTERNAL_PLACE_FILE, "directory", directory, NULL);
}

static void
nautilus_home_directory_class_init (NautilusHomeDirectoryClass *klass)
{
    NautilusDirectoryClass *directory_class = NAUTILUS_DIRECTORY_CLASS (klass);

    directory_class->are_all_files_seen = real_are_all_files_seen;
    directory_class->call_when_ready = real_call_when_ready;
    directory_class->cancel_callback = real_cancel_callback;
    directory_class->file_monitor_add = real_file_monitor_add;
    directory_class->file_monitor_remove = real_file_monitor_remove;
    directory_class->force_reload = real_force_reload;
    directory_class->get_file_list = real_get_file_list;
    directory_class->handles_location = real_handles_location;
    directory_class->is_editable = real_is_editable;
    directory_class->is_not_empty = real_is_not_empty;
    directory_class->new_as_file = real_new_as_file;
}

static void
nautilus_home_directory_init (NautilusHomeDirectory *self)
{
}

NautilusHomeDirectory *
nautilus_home_directory_new (void)
{
    g_autoptr (GFile) location = g_file_new_for_uri (SCHEME_HOME ":///");

    return g_object_new (NAUTILUS_TYPE_HOME_DIRECTORY,
                         "location", location,
                         NULL);
}
