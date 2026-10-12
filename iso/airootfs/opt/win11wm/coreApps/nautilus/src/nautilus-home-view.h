/*
 * nautilus-home-view.h: Windows 11-style Home dashboard.
 *
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "nautilus-types.h"

#define NAUTILUS_TYPE_HOME_VIEW (nautilus_home_view_get_type ())

G_DECLARE_FINAL_TYPE (NautilusHomeView, nautilus_home_view, NAUTILUS, HOME_VIEW, AdwBin)

NautilusHomeView *nautilus_home_view_new      (NautilusWindowSlot *slot);
void              nautilus_home_view_refresh  (NautilusHomeView *self);
