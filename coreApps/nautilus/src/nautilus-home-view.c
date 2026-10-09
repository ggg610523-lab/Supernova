/*
 * nautilus-home-view.c: Windows 11-style Home dashboard.
 *
 * Shows a greeting with the current date and time, the user's pinned folders
 * ("Quick Access"), mounted drives with usage bars ("Storage") and the most
 * recently used files ("Recent Files").
 *
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nautilus-home-view.h"

#include <glib/gi18n.h>

#include "nautilus-file.h"
#include "nautilus-quick-access.h"
#include "nautilus-scheme.h"
#include "nautilus-window-slot.h"

#define MAX_RECENT_FILES 8
#define RECENT_FILES_ATTRIBUTES "standard::size,standard::type,standard::icon"

struct _NautilusHomeView
{
    AdwBin parent_instance;

    NautilusWindowSlot *slot;

    GtkWidget *quick_grid;
    GtkWidget *storage_section;
    GtkWidget *storage_box;
    GtkWidget *recent_section;
    GtkWidget *recent_box;

    GtkWidget *greeting_label;
    GtkWidget *time_label;

    guint clock_timeout_id;
};

G_DEFINE_TYPE (NautilusHomeView, nautilus_home_view, ADW_TYPE_BIN)

typedef struct
{
    char *name;
    GFile *root;
    guint64 total;
    guint64 free;
} DriveUsage;

static void
drive_usage_free (DriveUsage *usage)
{
    g_free (usage->name);
    g_clear_object (&usage->root);
    g_free (usage);
}

static void
open_location (NautilusHomeView *self,
               GFile            *location,
               GFile            *selection)
{
    g_autolist (NautilusFile) selection_files = NULL;

    if (location == NULL)
    {
        return;
    }

    if (selection != NULL)
    {
        selection_files = g_list_prepend (selection_files, nautilus_file_get (selection));
    }

    if (self->slot != NULL)
    {
        nautilus_window_slot_open_location_full (self->slot, location, selection_files);
    }
}

static void
on_location_clicked (GtkButton        *button,
                     NautilusHomeView *self)
{
    GFile *location = g_object_get_data (G_OBJECT (button), "location");
    GFile *selection = g_object_get_data (G_OBJECT (button), "selection");

    open_location (self, location, selection);
}

static void
set_button_location (GtkWidget        *button,
                     NautilusHomeView *self,
                     GFile            *location,
                     GFile            *selection)
{
    g_object_set_data_full (G_OBJECT (button), "location", g_object_ref (location), g_object_unref);
    if (selection != NULL)
    {
        g_object_set_data_full (G_OBJECT (button), "selection", g_object_ref (selection), g_object_unref);
    }
    g_signal_connect (button, "clicked", G_CALLBACK (on_location_clicked), self);
}

static char *
relative_time (GDateTime *modified)
{
    g_autoptr (GDateTime) now = g_date_time_new_now_local ();
    GTimeSpan span = g_date_time_difference (now, modified);
    gint64 minutes = span / G_TIME_SPAN_MINUTE;

    if (minutes < 1)
    {
        return g_strdup (_("Just now"));
    }
    if (minutes < 60)
    {
        /* Translators: %d is a number of minutes */
        return g_strdup_printf (ngettext ("%d minute ago", "%d minutes ago", minutes), (int) minutes);
    }

    gint64 hours = span / G_TIME_SPAN_HOUR;
    if (hours < 24)
    {
        /* Translators: %d is a number of hours */
        return g_strdup_printf (ngettext ("%d hour ago", "%d hours ago", hours), (int) hours);
    }

    gint64 days = span / G_TIME_SPAN_DAY;
    if (days < 7)
    {
        /* Translators: %d is a number of days */
        return g_strdup_printf (ngettext ("%d day ago", "%d days ago", days), (int) days);
    }

    return g_date_time_format (modified, "%b %e");
}

static char *
format_size (guint64 bytes)
{
    return g_format_size_full (bytes, G_FORMAT_SIZE_IEC_UNITS);
}

/* Build a square icon slot: a fixed-size box that centres a glyph of the given
 * pixel size. Using one helper everywhere keeps every glyph on the dashboard on
 * the same visual scale, whatever the aspect ratio of the underlying icon. */
static GtkWidget *
icon_slot_new (const char *icon_name,
               const char *css_class,
               int         slot_size,
               int         pixel_size)
{
    GtkWidget *slot = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *image = gtk_image_new_from_icon_name (icon_name);
    int pad = MAX (0, (slot_size - pixel_size) / 2);

    gtk_widget_add_css_class (slot, css_class);
    gtk_widget_set_size_request (slot, slot_size, slot_size);
    gtk_widget_set_valign (slot, GTK_ALIGN_CENTER);
    gtk_image_set_pixel_size (GTK_IMAGE (image), pixel_size);
    /* GtkBox packs a child at the start of its main axis and ignores the
     * child's halign, so equal leading/trailing margins are what actually
     * centre the glyph inside the square.  The glyph must not expand either:
     * an expanding child propagates its expand up to the slot, which then
     * stretches across the whole row (badge) or card. */
    gtk_widget_set_margin_start (image, pad);
    gtk_widget_set_margin_end (image, pad);
    gtk_widget_set_valign (image, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (slot), image);

    return slot;
}

static GtkWidget *
section_header_new (const char *label,
                    const char *icon_name,
                    int         count)
{
    GtkWidget *box;
    GtkWidget *badge;
    GtkWidget *title;

    box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);

    badge = icon_slot_new (icon_name, "home-section-badge", 20, 12);
    gtk_box_append (GTK_BOX (box), badge);

    title = gtk_label_new (label);
    gtk_widget_add_css_class (title, "home-section-title");
    gtk_label_set_xalign (GTK_LABEL (title), 0.0);
    gtk_box_append (GTK_BOX (box), title);

    if (count >= 0)
    {
        GtkWidget *count_label = gtk_label_new (NULL);
        g_autofree char *text = g_strdup_printf ("%d", count);

        gtk_label_set_text (GTK_LABEL (count_label), text);
        gtk_widget_add_css_class (count_label, "home-section-count");
        gtk_widget_set_valign (count_label, GTK_ALIGN_CENTER);
        gtk_box_append (GTK_BOX (box), count_label);
    }

    return box;
}

static const char *
icon_name_for_folder (GFile *location)
{
    return nautilus_quick_access_icon_name (location, TRUE);
}

static char *
folder_display_name (GFile *location)
{
    g_autofree char *path = g_file_get_path (location);

    if (path != NULL && g_strcmp0 (path, g_get_home_dir ()) == 0)
    {
        return g_strdup (_("Home"));
    }

    return g_file_get_basename (location);
}

static void
on_unpin_clicked (GtkButton        *button,
                  NautilusHomeView *self)
{
    GFile *location = g_object_get_data (G_OBJECT (button), "location");

    if (location != NULL)
    {
        nautilus_quick_access_unpin (location);
    }
}

static GtkWidget *
quick_access_card_new (NautilusHomeView *self,
                       GFile            *location)
{
    GtkWidget *container;
    GtkWidget *button;
    GtkWidget *box;
    GtkWidget *icon_box;
    GtkWidget *label;
    GtkWidget *unpin_button;
    g_autofree char *name = NULL;

    container = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class (container, "home-card");

    button = gtk_button_new ();
    gtk_widget_add_css_class (button, "home-card-main");
    gtk_widget_set_hexpand (button, TRUE);

    box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_halign (box, GTK_ALIGN_FILL);
    gtk_button_set_child (GTK_BUTTON (button), box);

    icon_box = icon_slot_new (icon_name_for_folder (location), "home-card-icon", 42, 22);
    gtk_box_append (GTK_BOX (box), icon_box);

    name = folder_display_name (location);
    label = gtk_label_new (name);
    gtk_widget_add_css_class (label, "home-card-label");
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars (GTK_LABEL (label), 1);
    gtk_widget_set_hexpand (label, TRUE);
    gtk_box_append (GTK_BOX (box), label);

    set_button_location (button, self, location, NULL);
    gtk_box_append (GTK_BOX (container), button);

    unpin_button = gtk_button_new_from_icon_name ("window-close-symbolic");
    gtk_widget_add_css_class (unpin_button, "home-card-unpin");
    gtk_widget_set_tooltip_text (unpin_button, _("Unpin from Quick Access"));
    gtk_widget_set_valign (unpin_button, GTK_ALIGN_CENTER);
    g_object_set_data_full (G_OBJECT (unpin_button), "location",
                            g_object_ref (location), g_object_unref);
    g_signal_connect (unpin_button, "clicked", G_CALLBACK (on_unpin_clicked), self);
    gtk_box_append (GTK_BOX (container), unpin_button);

    return container;
}

static GtkWidget *
storage_row_new (NautilusHomeView *self,
                 const char       *name,
                 const char       *icon_name,
                 GFile            *location,
                 guint64           total,
                 guint64           free_space)
{
    GtkWidget *button;
    GtkWidget *box;
    GtkWidget *top;
    GtkWidget *icon_box;
    GtkWidget *labels;
    GtkWidget *name_label;
    GtkWidget *detail_label;
    GtkWidget *percent_label;
    GtkWidget *bar;
    GtkWidget *fill;
    double fraction = 0.0;

    button = gtk_button_new ();
    gtk_widget_add_css_class (button, "home-storage-item");
    gtk_widget_set_halign (button, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand (button, TRUE);

    box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
    gtk_button_set_child (GTK_BUTTON (button), box);

    top = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_append (GTK_BOX (box), top);

    icon_box = icon_slot_new (icon_name, "home-storage-icon", 32, 18);
    gtk_box_append (GTK_BOX (top), icon_box);

    labels = gtk_box_new (GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_hexpand (labels, TRUE);
    gtk_widget_set_valign (labels, GTK_ALIGN_CENTER);
    name_label = gtk_label_new (name);
    gtk_widget_add_css_class (name_label, "home-storage-name");
    gtk_label_set_xalign (GTK_LABEL (name_label), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (name_label), PANGO_ELLIPSIZE_END);
    gtk_box_append (GTK_BOX (labels), name_label);

    detail_label = gtk_label_new (NULL);
    gtk_widget_add_css_class (detail_label, "home-storage-detail");
    gtk_label_set_xalign (GTK_LABEL (detail_label), 0.0);
    {
        g_autofree char *used = format_size (total - free_space);
        g_autofree char *whole = format_size (total);
        /* Translators: %s is used space, %s is total space */
        g_autofree char *text = g_strdup_printf (_("%s of %s"), used, whole);
        gtk_label_set_text (GTK_LABEL (detail_label), text);
    }
    gtk_box_append (GTK_BOX (labels), detail_label);
    gtk_box_append (GTK_BOX (top), labels);

    percent_label = gtk_label_new (NULL);
    gtk_widget_add_css_class (percent_label, "home-storage-percent");
    gtk_widget_set_valign (percent_label, GTK_ALIGN_CENTER);
    {
        char *text = g_strdup_printf ("%d%%", (int) (total > 0 ? (100.0 * (total - free_space) / total) : 0));
        gtk_label_set_text (GTK_LABEL (percent_label), text);
        g_free (text);
    }
    gtk_box_append (GTK_BOX (top), percent_label);

    bar = gtk_level_bar_new_for_interval (0, 100);
    fill = bar;
    gtk_widget_add_css_class (fill, "home-storage-bar");
    if (total > 0)
    {
        fraction = 100.0 * (total - free_space) / total;
    }
    gtk_level_bar_set_value (GTK_LEVEL_BAR (bar), fraction);
    if (fraction > 90.0)
    {
        gtk_widget_add_css_class (fill, "critical");
    }
    else if (fraction > 70.0)
    {
        gtk_widget_add_css_class (fill, "warning");
    }
    gtk_box_append (GTK_BOX (box), bar);

    set_button_location (button, self, location, NULL);

    return button;
}

static GtkWidget *
recent_row_new (NautilusHomeView *self,
                GtkRecentInfo    *info)
{
    GtkWidget *button;
    GtkWidget *box;
    GtkWidget *icon;
    GtkWidget *labels;
    GtkWidget *name_label;
    GtkWidget *detail_label;
    GtkWidget *size_label;
    GIcon *gicon;
    const char *name;
    char *detail = NULL;

    button = gtk_button_new ();
    gtk_widget_add_css_class (button, "home-recent-item");
    gtk_widget_set_halign (button, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand (button, TRUE);

    box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_button_set_child (GTK_BUTTON (button), box);

    gicon = gtk_recent_info_get_gicon (info);
    /* Fixed-size slot (32px) so every row's icon lines up and stays on the same
     * scale as the Storage rows, regardless of the source icon's aspect ratio. */
    {
        GtkWidget *icon_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class (icon_box, "home-recent-icon");
        gtk_widget_set_size_request (icon_box, 32, 32);
        gtk_widget_set_valign (icon_box, GTK_ALIGN_CENTER);
        if (gicon != NULL)
        {
            icon = gtk_image_new_from_gicon (gicon);
            g_object_unref (gicon);
        }
        else
        {
            icon = gtk_image_new_from_icon_name ("text-x-generic");
        }
        gtk_image_set_pixel_size (GTK_IMAGE (icon), 20);
        /* Equal margins centre the glyph: GtkBox packs children at the start
         * of its main axis and ignores halign. */
        gtk_widget_set_margin_start (icon, 6);
        gtk_widget_set_margin_end (icon, 6);
        gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
        gtk_box_append (GTK_BOX (icon_box), icon);
        gtk_box_append (GTK_BOX (box), icon_box);
    }

    labels = gtk_box_new (GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_hexpand (labels, TRUE);
    gtk_widget_set_valign (labels, GTK_ALIGN_CENTER);
    gtk_widget_set_overflow (labels, GTK_OVERFLOW_HIDDEN);

    name = gtk_recent_info_get_display_name (info);
    name_label = gtk_label_new (name != NULL ? name : gtk_recent_info_get_uri (info));
    gtk_widget_add_css_class (name_label, "home-recent-name");
    gtk_label_set_xalign (GTK_LABEL (name_label), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (name_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars (GTK_LABEL (name_label), 1);
    gtk_box_append (GTK_BOX (labels), name_label);

    {
        GDateTime *modified = gtk_recent_info_get_modified (info);
        g_autofree char *relative = modified != NULL ? relative_time (modified) : NULL;
        g_autofree char *uri = g_strdup (gtk_recent_info_get_uri (info));
        g_autoptr (GFile) location = g_file_new_for_uri (uri);
        g_autoptr (GFile) parent = g_file_get_parent (location);
        g_autofree char *parent_name = parent != NULL ? g_file_get_parse_name (parent) : NULL;

        if (parent_name != NULL)
        {
            /* Translators: %s is the parent folder, %s is a relative time */
            detail = g_strdup_printf (_("%s · %s"), parent_name, relative != NULL ? relative : "");
        }
        else
        {
            detail = g_strdup (relative != NULL ? relative : "");
        }
    }
    detail_label = gtk_label_new (detail);
    gtk_widget_add_css_class (detail_label, "home-recent-detail");
    gtk_label_set_xalign (GTK_LABEL (detail_label), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (detail_label), PANGO_ELLIPSIZE_END);
    gtk_box_append (GTK_BOX (labels), detail_label);
    gtk_box_append (GTK_BOX (box), labels);
    g_free (detail);

    {
        g_autofree char *uri = g_strdup (gtk_recent_info_get_uri (info));
        g_autoptr (GFile) location = g_file_new_for_uri (uri);
        g_autoptr (GFileInfo) file_info = g_file_query_info (location, RECENT_FILES_ATTRIBUTES,
                                                             G_FILE_QUERY_INFO_NONE, NULL, NULL);
        if (file_info != NULL &&
            g_file_info_has_attribute (file_info, G_FILE_ATTRIBUTE_STANDARD_SIZE))
        {
            g_autofree char *size = format_size (g_file_info_get_size (file_info));
            size_label = gtk_label_new (size);
            gtk_widget_add_css_class (size_label, "home-recent-size");
            gtk_widget_set_valign (size_label, GTK_ALIGN_CENTER);
            gtk_box_append (GTK_BOX (box), size_label);
        }
    }

    /* Open the containing folder and select the file, matching the reference
     * file manager's behaviour. */
    {
        g_autofree char *uri = g_strdup (gtk_recent_info_get_uri (info));
        g_autoptr (GFile) location = g_file_new_for_uri (uri);
        g_autoptr (GFile) parent = g_file_get_parent (location);

        if (parent != NULL && g_file_is_native (location))
        {
            set_button_location (button, self, parent, location);
        }
        else
        {
            set_button_location (button, self, location, NULL);
        }
    }

    return button;
}

static char *
greeting_text (void)
{
    g_autoptr (GDateTime) now = g_date_time_new_now_local ();
    int hour = g_date_time_get_hour (now);

    if (hour < 6)
    {
        return g_strdup (_("Good night"));
    }
    if (hour < 12)
    {
        return g_strdup (_("Good morning"));
    }
    if (hour < 17)
    {
        return g_strdup (_("Good afternoon"));
    }
    return g_strdup (_("Good evening"));
}

static char *
date_text (void)
{
    g_autoptr (GDateTime) now = g_date_time_new_now_local ();
    g_autofree char *head = g_date_time_format (now, "%A, %B");

    return g_strdup_printf ("%s %d, %d", head,
                            g_date_time_get_day_of_month (now),
                            g_date_time_get_year (now));
}

static void
update_clock (NautilusHomeView *self)
{
    g_autoptr (GDateTime) now = g_date_time_new_now_local ();
    g_autofree char *time = g_date_time_format (now, "%H:%M");
    g_autofree char *greeting = greeting_text ();

    if (self->time_label != NULL)
    {
        gtk_label_set_text (GTK_LABEL (self->time_label), time);
    }
    if (self->greeting_label != NULL)
    {
        gtk_label_set_text (GTK_LABEL (self->greeting_label), greeting);
    }

    return;
}

static gboolean
on_clock_timeout (gpointer user_data)
{
    update_clock (NAUTILUS_HOME_VIEW (user_data));
    return G_SOURCE_CONTINUE;
}

static void
clear_box (GtkWidget *container)
{
    GtkWidget *child;

    while ((child = gtk_widget_get_first_child (container)) != NULL)
    {
        gtk_widget_unparent (child);
    }
}

static void
refresh_quick_access (NautilusHomeView *self)
{
    g_autolist (GFile) pins = nautilus_quick_access_get_pins ();
    guint n = 0;

    clear_box (self->quick_grid);

    for (GList *l = pins; l != NULL; l = l->next)
    {
        GFile *location = l->data;
        GtkWidget *card = quick_access_card_new (self, location);

        gtk_grid_attach (GTK_GRID (self->quick_grid), card, n % 2, n / 2, 1, 1);
        n++;
    }

    gtk_widget_set_visible (self->quick_grid, n > 0);
}

static void
on_pins_changed (GSettings *settings,
                 gchar     *key,
                 gpointer   user_data)
{
    refresh_quick_access (NAUTILUS_HOME_VIEW (user_data));
}

static GList *
collect_drive_usage (void)
{
    GVolumeMonitor *monitor = g_volume_monitor_get ();
    GList *drives = g_volume_monitor_get_connected_drives (monitor);
    GList *result = NULL;

    for (GList *l = drives; l != NULL; l = l->next)
    {
        GDrive *drive = l->data;
        GList *volumes = g_drive_get_volumes (drive);
        gboolean found = FALSE;

        for (GList *v = volumes; v != NULL && !found; v = v->next)
        {
            GVolume *volume = v->data;
            GMount *mount = g_volume_get_mount (volume);

            if (mount != NULL)
            {
                g_autoptr (GFile) root = g_mount_get_root (mount);

                if (root != NULL && g_file_is_native (root))
                {
                    g_autoptr (GFileInfo) info = g_file_query_filesystem_info (
                        root, "filesystem::size,filesystem::free", NULL, NULL);

                    if (info != NULL &&
                        g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE))
                    {
                        DriveUsage *usage = g_new0 (DriveUsage, 1);
                        usage->name = g_strdup (g_drive_get_name (drive));
                        usage->root = g_object_ref (root);
                        usage->total = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
                        usage->free = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);

                        if (usage->total > 0)
                        {
                            result = g_list_append (result, usage);
                            found = TRUE;
                        }
                        else
                        {
                            drive_usage_free (usage);
                        }
                    }
                }

                g_object_unref (mount);
            }

            g_object_unref (volume);
        }

        g_list_free (volumes);
        g_object_unref (drive);
    }

    /* On systems without a reported drive (e.g. containers), fall back to the
     * plain mounts so the Storage section still shows something useful. */
    if (result == NULL)
    {
        GList *mounts = g_volume_monitor_get_mounts (monitor);

        for (GList *l = mounts; l != NULL; l = l->next)
        {
            GMount *mount = l->data;
            g_autoptr (GFile) root = NULL;
            g_autoptr (GFileInfo) info = NULL;

            if (g_mount_is_shadowed (mount))
            {
                g_object_unref (mount);
                continue;
            }

            root = g_mount_get_root (mount);

            if (root == NULL || !g_file_is_native (root))
            {
                g_object_unref (mount);
                continue;
            }

            info = g_file_query_filesystem_info (root, "filesystem::size,filesystem::free", NULL, NULL);

            if (info != NULL &&
                g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE))
            {
                guint64 total = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);

                if (total > 0)
                {
                    DriveUsage *usage = g_new0 (DriveUsage, 1);
                    g_autofree char *mount_name = g_mount_get_name (mount);

                    usage->name = g_steal_pointer (&mount_name);
                    usage->root = g_object_ref (root);
                    usage->total = total;
                    usage->free = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);

                    result = g_list_append (result, usage);
                }
            }

            g_object_unref (mount);
        }

        g_list_free (mounts);
    }

    /* As a last resort, always show the filesystem the running system lives on
     * so the Storage section is never empty. */
    if (result == NULL)
    {
        g_autoptr (GFile) root = g_file_new_for_path ("/");
        g_autoptr (GFileInfo) info = g_file_query_filesystem_info (
            root, "filesystem::size,filesystem::free", NULL, NULL);

        if (info != NULL &&
            g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE))
        {
            DriveUsage *usage = g_new0 (DriveUsage, 1);

            usage->name = g_strdup (_("Filesystem"));
            usage->root = g_object_ref (root);
            usage->total = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
            usage->free = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
            result = g_list_append (result, usage);
        }
    }

    g_list_free (drives);
    g_object_unref (monitor);

    return result;
}

static void
refresh_storage (NautilusHomeView *self)
{
    GList *usages;
    guint n = 0;

    clear_box (self->storage_box);

    usages = collect_drive_usage ();

    for (GList *l = usages; l != NULL; l = l->next)
    {
        DriveUsage *usage = l->data;
        GtkWidget *row = storage_row_new (self, usage->name, "drive-harddisk-symbolic",
                                          usage->root, usage->total, usage->free);

        gtk_box_append (GTK_BOX (self->storage_box), row);
        n++;
    }

    g_list_free_full (usages, (GDestroyNotify) drive_usage_free);

    gtk_widget_set_visible (self->storage_section, n > 0);
}

static gint
compare_recent (gconstpointer a,
                gconstpointer b)
{
    GtkRecentInfo *info_a = (GtkRecentInfo *) a;
    GtkRecentInfo *info_b = (GtkRecentInfo *) b;

    /* Most recent first. */
    return g_date_time_compare (gtk_recent_info_get_modified (info_b),
                                gtk_recent_info_get_modified (info_a));
}

static void
refresh_recent (NautilusHomeView *self)
{
    GtkRecentManager *manager;
    GList *items;
    guint n = 0;

    clear_box (self->recent_box);

    manager = gtk_recent_manager_get_default ();
    items = gtk_recent_manager_get_items (manager);
    items = g_list_sort (items, compare_recent);

    for (GList *l = items; l != NULL && n < MAX_RECENT_FILES; l = l->next)
    {
        GtkRecentInfo *info = l->data;
        GtkWidget *row;

        if (!gtk_recent_info_is_local (info))
        {
            continue;
        }

        row = recent_row_new (self, info);
        gtk_box_append (GTK_BOX (self->recent_box), row);
        n++;
    }

    g_list_free_full (items, (GDestroyNotify) gtk_recent_info_unref);

    gtk_widget_set_visible (self->recent_section, n > 0);
}

void
nautilus_home_view_refresh (NautilusHomeView *self)
{
    g_return_if_fail (NAUTILUS_IS_HOME_VIEW (self));

    update_clock (self);
    refresh_quick_access (self);
    refresh_storage (self);
    refresh_recent (self);
}

static void
nautilus_home_view_dispose (GObject *object)
{
    NautilusHomeView *self = NAUTILUS_HOME_VIEW (object);

    g_signal_handlers_disconnect_by_func (nautilus_quick_access_settings (),
                                          G_CALLBACK (on_pins_changed), self);
    g_clear_handle_id (&self->clock_timeout_id, g_source_remove);
    self->slot = NULL;

    G_OBJECT_CLASS (nautilus_home_view_parent_class)->dispose (object);
}

static void
nautilus_home_view_class_init (NautilusHomeViewClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->dispose = nautilus_home_view_dispose;
}

static void
nautilus_home_view_init (NautilusHomeView *self)
{
    GtkWidget *scrolled;
    GtkWidget *clamp;
    GtkWidget *content;
    GtkWidget *header;
    GtkWidget *title_box;
    GtkWidget *date_label;
    GtkWidget *quick_section;
    GtkWidget *recent_section;

    gtk_widget_add_css_class (GTK_WIDGET (self), "nautilus-home-view");

    scrolled = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_hexpand (scrolled, TRUE);
    gtk_widget_set_vexpand (scrolled, TRUE);

    clamp = adw_clamp_new ();
    adw_clamp_set_maximum_size (ADW_CLAMP (clamp), 880);
    adw_clamp_set_tightening_threshold (ADW_CLAMP (clamp), 700);

    content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 28);
    gtk_widget_add_css_class (content, "home-content");
    adw_clamp_set_child (ADW_CLAMP (clamp), content);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), clamp);

    /* Hero: greeting + date, with the clock on the right. */
    header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 20);
    title_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand (title_box, TRUE);

    self->greeting_label = gtk_label_new (NULL);
    gtk_widget_add_css_class (self->greeting_label, "home-greeting");
    gtk_label_set_xalign (GTK_LABEL (self->greeting_label), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (self->greeting_label), PANGO_ELLIPSIZE_END);
    gtk_box_append (GTK_BOX (title_box), self->greeting_label);

    date_label = gtk_label_new (NULL);
    {
        g_autofree char *text = date_text ();
        gtk_label_set_text (GTK_LABEL (date_label), text);
    }
    gtk_widget_add_css_class (date_label, "home-date");
    gtk_label_set_xalign (GTK_LABEL (date_label), 0.0);
    gtk_box_append (GTK_BOX (title_box), date_label);
    gtk_box_append (GTK_BOX (header), title_box);

    self->time_label = gtk_label_new (NULL);
    gtk_widget_add_css_class (self->time_label, "home-clock");
    gtk_widget_set_valign (self->time_label, GTK_ALIGN_START);
    gtk_box_append (GTK_BOX (header), self->time_label);
    gtk_box_append (GTK_BOX (content), header);

    /* Quick Access. */
    quick_section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
    gtk_box_append (GTK_BOX (quick_section),
                    section_header_new (_("Quick Access"), "view-grid-symbolic", -1));
    self->quick_grid = gtk_grid_new ();
    gtk_grid_set_column_spacing (GTK_GRID (self->quick_grid), 8);
    gtk_grid_set_row_spacing (GTK_GRID (self->quick_grid), 8);
    gtk_grid_set_column_homogeneous (GTK_GRID (self->quick_grid), TRUE);
    gtk_box_append (GTK_BOX (quick_section), self->quick_grid);
    gtk_box_append (GTK_BOX (content), quick_section);

    /* Storage. */
    self->storage_section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
    gtk_box_append (GTK_BOX (self->storage_section),
                    section_header_new (_("Storage"), "drive-harddisk-symbolic", -1));
    self->storage_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_append (GTK_BOX (self->storage_section), self->storage_box);
    gtk_box_append (GTK_BOX (content), self->storage_section);

    /* Recent Files. */
    recent_section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
    gtk_box_append (GTK_BOX (recent_section),
                    section_header_new (_("Recent Files"), "document-open-recent-symbolic", -1));
    self->recent_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_append (GTK_BOX (recent_section), self->recent_box);
    self->recent_section = recent_section;
    gtk_box_append (GTK_BOX (content), recent_section);

    adw_bin_set_child (ADW_BIN (self), scrolled);

    g_signal_connect (nautilus_quick_access_settings (),
                      "changed::quick-access-pins",
                      G_CALLBACK (on_pins_changed), self);

    self->clock_timeout_id = g_timeout_add_seconds (30, on_clock_timeout, self);
}

NautilusHomeView *
nautilus_home_view_new (NautilusWindowSlot *slot)
{
    NautilusHomeView *self;

    self = g_object_new (NAUTILUS_TYPE_HOME_VIEW, NULL);
    self->slot = slot;

    return self;
}
