// Desktop widgets: the iOS 26 "Liquid Glass" cards that live on the wallpaper
// alongside the desktop icons. The WM supports two kinds, both of which the
// user can drag anywhere, resize from the corner, remove, and add again.
#pragma once

#include "util.h"

namespace wm {

enum class WidgetKind {
    Clock,     // analog clock, small square
    Battery,   // battery / charging state, medium
    Calendar,     // month grid with today marked, iOS 18 Calendar style
    Weather,      // current conditions + hourly strip, iOS 18 Weather style
    DigitalClock,  // huge HH:MM in the Poppins ExtraBold display face
};

struct Widget {
    WidgetKind kind = WidgetKind::Clock;
    Rect rect;  // absolute screen position and size
    // Which home screen page this card lives on, counted the way the tablet counts
    // them. Cards are desktop only -- tablet mode takes them off the screen -- so
    // this is always 0 for now and the paging around it is unused.
    int page = 0;
};

}  // namespace wm
