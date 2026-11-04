// Desktop widgets: the iOS 26 "Liquid Glass" cards that live on the wallpaper
// alongside the desktop icons. The WM supports the iOS clock/battery/calendar/
// weather cards plus one of its own, a Smart Apps folder: a resizable panel that
// holds a grid of app icons the user can launch straight from the desktop.
// Every card can be dragged anywhere, resized from the corner, removed and added
// again.
#pragma once

#include <string>
#include <vector>

#include "util.h"

namespace wm {

enum class WidgetKind {
    Clock,     // analog clock, small square
    Battery,   // battery / charging state, medium
    Calendar,     // month grid with today marked, iOS 18 Calendar style
    Weather,      // current conditions + hourly strip, iOS 18 Weather style
    DigitalClock,  // huge HH:MM in the Roboto display face at its heaviest cut
    SmartApps,     // resizable large folder: a grid of live app icons
};

// One app inside a Smart Apps folder. A snapshot of the launcher -- name, icon
// name and WM class -- so the folder keeps working, and keeps its artwork, even
// when the .desktop file behind it is gone. exec non-empty means "run this",
// empty means "hand path to xdg-open".
struct WidgetApp {
    std::string name;
    std::string exec;
    std::string path;
    std::string icon;
    std::string wmClass;
    // Where the launcher went when the folder took it off the desktop, or empty
    // for an app that was never a desktop file (a pin, a recent, a hand-typed
    // entry). Removing the app from the folder moves it back out of the store.
    std::string stash;
};

struct Widget {
    WidgetKind kind = WidgetKind::Clock;
    Rect rect;  // absolute screen position and size
    // Which home screen page this card lives on, counted the way the tablet counts
    // them. Cards are desktop only -- tablet mode takes them off the screen -- so
    // this is always 0 for now and the paging around it is unused.
    int page = 0;

    // --- Smart Apps only ---------------------------------------------------
    // The folder's title, shown in the card's header band.
    std::string name;
    // The apps inside, in the user's order. Persisted with the card.
    std::vector<WidgetApp> apps;
    // The card's logical grid: 1..6 columns by 1..12 rows. The pixel rect follows
    // from these (a resize snaps to whole cells) so the icons stay on a regular
    // grid at every size. There is no paging: the rows simply grow until every app
    // the folder holds is on screen.
    int cols = 2;
    int rows = 2;
};

}  // namespace wm
