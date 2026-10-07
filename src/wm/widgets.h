// Desktop widgets: the iOS 26 "Liquid Glass" cards that live on the wallpaper
// alongside the desktop icons. The WM supports multiple kinds, all of which the
// user can drag anywhere, resize from the corner, remove, and add again.
//
// SmartFolder: A rounded box container for organizing apps in a grid layout
// directly accessible from the desktop, with drag-and-drop support for adding
// apps and direct launch from the folder without opening a separate window.
#pragma once

#include "util.h"
#include <string>
#include <vector>

namespace wm {

enum class WidgetKind {
    Clock,        // analog clock, small square
    Battery,      // battery / charging state, medium
    Calendar,     // month grid with today marked, iOS 18 Calendar style
    Weather,      // current conditions + hourly strip, iOS 18 Weather style
    DigitalClock, // huge HH:MM in the Poppins ExtraBold display face
    SmartFolder,  // app grid in a rounded box container (2x3, 3x3, or 4x3 grid)
};

// One app reference in a SmartFolder: a snapshot entry like tablet entries
// so the app stays accessible even if the .desktop file is moved or removed.
struct FolderAppEntry {
    std::string name;    // "Firefox"
    std::string icon;    // icon name for lookup
    std::string wmClass; // StartupWMClass for window matching
    std::string exec;    // command to run, or path to open via xdg-open
    std::string path;    // file/folder path when not a launcher
};

// SmartFolder state: a collection of app shortcuts organized in a grid
// within a draggable, resizable rounded container on the desktop.
struct SmartFolderData {
    std::string name;                      // "Work Apps", "Utilities", etc.
    std::vector<FolderAppEntry> apps;      // grid of shortcuts (1-9 apps)
    int gridCols = 3;                      // columns: 2, 3, or 4
    bool labelVisible = true;              // show app names below icons
    bool lockedArrangement = false;        // prevent accidental rearrange
};

struct Widget {
    WidgetKind kind = WidgetKind::Clock;
    Rect rect;  // absolute screen position and size
    // Which home screen page this card lives on, counted the way the tablet counts
    // them. Cards are desktop only -- tablet mode takes them off the screen -- so
    // this is always 0 for now and the paging around it is unused.
    int page = 0;
    // SmartFolder-specific data (nullptr for other widget types)
    SmartFolderData* folder = nullptr;
};

}  // namespace wm
