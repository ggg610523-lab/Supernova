/*
 * nautilus-home-directory.h: virtual directory backing the Home dashboard.
 *
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "nautilus-directory.h"

#define NAUTILUS_HOME_DIRECTORY_PROVIDER_NAME "home-directory-provider"

#define NAUTILUS_TYPE_HOME_DIRECTORY (nautilus_home_directory_get_type())

G_DECLARE_FINAL_TYPE (NautilusHomeDirectory, nautilus_home_directory, NAUTILUS, HOME_DIRECTORY, NautilusDirectory)

NautilusHomeDirectory *nautilus_home_directory_new (void);
