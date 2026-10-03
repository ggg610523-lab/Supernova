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
    // Which home screen page this card lives on, counted the way the tablet counts
    // them. The desktop has no pages, so the field only means anything in tablet
    // mode -- where each card belongs to one page and is shown only on that one.
    // A card keeps its page across a restart of the shell for the same reason the
    // apps do: it is where the user put it.
    int page = 0;
};

}  // namespace wm
