// Desktop widgets: the iOS 26 "Liquid Glass" cards that live on the wallpaper
// alongside the desktop icons. The WM supports two kinds, both of which the
// user can drag anywhere, resize from the corner, remove, and add again.
#pragma once

#include "util.h"

namespace wm {

enum class WidgetKind {
    Clock,    // analog clock, small square
    Battery,  // battery / charging state, medium
};

struct Widget {
    WidgetKind kind = WidgetKind::Clock;
    Rect rect;  // absolute screen position and size
};

}  // namespace wm
