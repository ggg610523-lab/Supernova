// The Smart Apps widget: a resizable large folder that lives on the desktop.
//
// It is a live launcher rather than a card: the apps inside are drawn as their
// real icons on one continuous grid, a single click launches one, a long press
// turns the card into an editable board (jiggle, reorder, minus badges to
// remove), and the corner grip snaps it to whole cells from 1x1 up to 6x6. There
// are no pages: the grid simply adds rows as the folder fills, so every app is
// always visible and one gesture -- resize -- is all it takes to make the card
// as compact or as roomy as the desktop allows.
//
// The card is one surface. Panel and contents are drawn from a single animated
// rectangle, so a resize glides (the cells and their icons scale with it) and a
// move keeps the icons welded to the sheet instead of letting them lag behind.
#include <manager.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace wm {
namespace {

constexpr int kSlop = 7;                // travel that turns a press into a drag
constexpr double kLongPressMs = 460.0;  // hold that opens the folder's edit mode

// The cell grid of one card: square cells on a cols x rows matrix. Recomputed
// from the widget alone, so hit-testing and painting can never disagree about
// where an icon is.
struct SmartGrid {
    Rect panel;
    Rect cells[metrics::kSmartMax * metrics::kSmartMaxRows];
    int cols = 0, rows = 0;
    int capacity = 0;
    int icon = 0;      // icon edge in px
    int labelPx = 0;   // label point size, 0 when the cells are too tight
};

int smartColumns(const Widget& w) {
    return clampi(w.cols, metrics::kSmartMin, metrics::kSmartMax);
}

// The rows a card actually needs: the user's own choice, raised if the apps
// would not all fit. This is what keeps the folder page-free -- a drop can grow
// it by a row instead of sending an app to a second page.
int smartRowsFor(const Widget& w, int cols) {
    if (cols < 1) return 1;
    const int needed = (int(w.apps.size()) + cols - 1) / cols;
    return std::min(std::max(clampi(w.rows, metrics::kSmartMin, metrics::kSmartMaxRows),
                             std::max(1, needed)),
                    metrics::kSmartMaxRows);
}

// `rect` is the geometry to lay out in: the card's own rect at rest, or the
// animating one while it grows. There is no title band: a large folder is a
// sheet of icons, and the few pixels a caption would cost go to the grid.
SmartGrid smartGridIn(const Widget& w, const Rect& rect) {
    SmartGrid g;
    g.panel = rect;
    g.cols = smartColumns(w);
    g.rows = smartRowsFor(w, g.cols);
    g.capacity = g.cols * g.rows;

    const int side = std::min(rect.w, rect.h);
    const int pad = std::clamp(side / 26, 6, 10);
    const int gx = rect.x + pad;
    const int gy = rect.y + pad;
    const int gw = std::max(g.cols, rect.w - 2 * pad);
    const int gh = std::max(g.rows, rect.h - 2 * pad);
    const int cellW = gw / g.cols;
    const int cellH = gh / g.rows;
    for (int i = 0; i < g.capacity; ++i) {
        g.cells[i] = Rect{gx + (i % g.cols) * cellW, gy + (i / g.cols) * cellH, cellW, cellH};
    }

    g.labelPx = (cellH >= 62 && cellW >= 50) ? 10 : 0;
    const int labelH = g.labelPx ? g.labelPx + 6 : 0;
    // The icons fill their cells: a folder should read as a dense grid of apps,
    // not a few small glyphs floating in glass. The ceiling keeps a single app in
    // a big card from becoming a billboard.
    g.icon = std::clamp(std::min(cellW - 8, cellH - labelH - 6), 18, 88);
    return g;
}

int smartSlotAt(const SmartGrid& g, int x, int y) {
    for (int i = 0; i < g.capacity; ++i)
        if (g.cells[i].contains(x, y)) return i;
    return -1;
}

// The icon box of a slot: a square centred in the cell, with the label hanging
// under it.
Rect smartIconRect(const SmartGrid& g, int slot) {
    if (slot < 0 || slot >= g.capacity) return {};
    const Rect& c = g.cells[slot];
    const int labelH = g.labelPx ? g.labelPx + 6 : 0;
    const int top = c.y + std::max(0, (c.h - (g.icon + labelH)) / 2);
    return Rect{c.x + (c.w - g.icon) / 2, top, g.icon, g.icon};
}

Rect smartBadgeRect(const SmartGrid& g, int slot) {
    const Rect icon = smartIconRect(g, slot);
    const int b = std::clamp(g.icon / 3, 14, 18);
    return Rect{icon.x - b / 3, icon.y - b / 3, b, b};
}

bool smartLight() { return theme::isLight(); }

Color smartInk() { return smartLight() ? theme::kSmartInkLight : theme::kSmartInkDark; }
Color smartSub() { return smartLight() ? theme::kSmartSubLight : theme::kSmartSubDark; }
Color smartHover() { return smartLight() ? theme::kSmartHotLight : theme::kSmartHotDark; }

// A string cut to fit `maxW`, with an ellipsis. The shell's own ellipsize() lives
// in draw.cpp's anonymous namespace, so this file carries its small copy.
std::string fitText(Text& text, const std::string& s, int px, int maxW) {
    if (s.empty() || maxW <= 0) return {};
    if (text.measure(s, px) <= maxW) return s;
    const std::string dots = "\u2026";
    std::string out;
    for (size_t i = 0; i < s.size();) {
        size_t j = i + size_t(std::max(1, utf8Len(static_cast<unsigned char>(s[i]))));
        if (j > s.size()) j = s.size();
        const std::string cand = s.substr(0, j);
        if (text.measure(cand + dots, px) > maxW) break;
        out = cand;
        i = j;
    }
    return out.empty() ? std::string() : out + dots;
}

std::string shellQuote(const std::string& path) {
    std::string q = "'";
    for (char c : path) {
        if (c == '\'') q += "'\\''";
        else q += c;
    }
    q += "'";
    return q;
}

// The tint for an app that ships no icon: the same hash-to-palette rule the
// desktop and the tablet grid use, so one app looks the same everywhere.
Color smartTint(const std::string& name) {
    size_t h = 5381;
    for (unsigned char ch : name) h = h * 33 + ch;
    return theme::kTileTints[h % 6];
}

}  // namespace

Rect Manager::smartDrawnRect(const Widget& w, int index) const {
    if (smartPanelWidget != index) return w.rect;
    const motion::Vec2 p = smartPanelPos.value();
    const motion::Vec2 s = smartPanelSize.value();
    return Rect{int(std::lround(p.x)), int(std::lround(p.y)),
                std::max(1, int(std::lround(s.x))), std::max(1, int(std::lround(s.y)))};
}

// Starts (or re-bases) the card's growth animation from `from`. Called whenever
// the resting rect changes, so the sheet eases from what the user was looking at
// rather than from a rect it never had.
void Manager::seedSmartPanel(int index, const Rect& from) {
    smartPanelWidget = index;
    smartPanelPos.reset(motion::Vec2{double(from.x), double(from.y)});
    smartPanelSize.reset(motion::Vec2{double(from.w), double(from.h)});
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void Manager::drawSmartApps(const Widget& w, int index) {
    // One rectangle drives everything: panel, cells, icons and labels. A resize
    // therefore scales the whole card as a unit instead of snapping the sheet
    // around icons that are already there.
    Widget anim = w;
    anim.rect = smartDrawnRect(w, index);
    const Rect rect = anim.rect;
    const SmartGrid g = smartGridIn(anim, rect);
    const float radius = std::clamp(std::min(rect.w, rect.h) * 0.13f, 12.f, 26.f);
    const bool editing = smartEdit == index;
    const bool carried = dragWidget == index;
    const double now = nowMs();

    // A soft shadow, so the sheet reads as sitting on the wallpaper rather than
    // being painted into it. Two rounded layers stand in for a real blur; a small
    // card does not need more.
    for (int k = 2; k >= 1; --k) {
        const int spread = 2 + 4 * k;
        comp.drawRect(Rect{rect.x - spread, rect.y - spread + 2 + k, rect.w + 2 * spread,
                           rect.h + 2 * spread},
                      radius + float(spread), theme::kShadow, 0.05f);
    }
    comp.drawAcrylic(rect, radius, smartLight() ? theme::kSmartGlassLight : theme::kSmartGlassDark,
                     smartLight() ? 0.90f : 0.84f,
                     smartLight() ? theme::kSmartBorderLight : theme::kSmartBorderDark,
                     carried ? 0.98f : 0.94f, smartLight() ? 1.15f : 1.35f);

    const int hoverSlot =
        (hoverWidget == index || carried) ? smartSlotAt(g, pointerX, pointerY) : -1;

    for (int slot = 0; slot < g.capacity; ++slot) {
        const int ai = slot;
        if (ai >= int(w.apps.size())) break;
        // An app that is still on its way in is not drawn in the grid yet: the
        // ghost flying into the cell is the only copy, so the hand-off at the end
        // is invisible rather than a second icon popping in beside it.
        bool incoming = false;
        for (const SmartFly& f : smartFlies) {
            if (f.widget == index && f.appIndex == ai && now - f.start < f.ms) incoming = true;
        }
        if (incoming) continue;
        const Rect cell = g.cells[slot];
        const WidgetApp& app = w.apps[size_t(ai)];
        // Edit mode shakes the whole board a pixel or two, the way every mobile
        // launcher signals that the icons can now be moved.
        int jx = 0, jy = 0;
        if (editing) {
            jx = int(std::lround(std::sin(now / 95.0 + slot * 1.1) * 1.6));
            jy = int(std::lround(std::cos(now / 110.0 + slot * 1.3) * 1.3));
        }
        Rect icon = smartIconRect(g, slot).moved(jx, jy);
        if (slot == hoverSlot && !editing && !(smartDragMoving && ai == smartDragFrom))
            comp.drawRect(icon.inflated(4), float(icon.w) * 0.28f, smartHover(), 1.0f);

        if (smartDragMoving && ai == smartDragFrom) continue;  // drawn under the pointer
        const float ir = float(icon.w) * 0.235f;
        if (!drawAppIcon(icon, app.icon, app.wmClass, ir, 1.0f))
            drawAppTile(icon, app.name, ir, smartTint(app.name), false);

        if (g.labelPx) {
            const std::string label = fitText(text, app.name, g.labelPx, cell.w - 4);
            const int lx = cell.x + (cell.w - text.measure(label, g.labelPx)) / 2;
            drawTextAt(label, g.labelPx, Weight::Medium, smartInk(), lx + jx,
                       icon.bottom() + 3 + jy);
        }

        if (editing) {
            const Rect badge = smartBadgeRect(g, slot).moved(jx, jy);
            comp.drawRect(badge, float(badge.w) * 0.5f, theme::kSmartBadge, 0.92f);
            comp.drawRect(badge, float(badge.w) * 0.5f,
                          smartLight() ? theme::kSmartBorderLight : theme::kSmartBorderDark, 1.0f);
            comp.drawRect(Rect{badge.x + badge.w / 4, badge.y + badge.h / 2 - 1, badge.w / 2, 2},
                          1.0f, theme::kText, 1.0f);
        }
    }

    // A brand new card with nothing in it says how to fill it instead of showing
    // an empty sheet.
    if (w.apps.empty()) {
        drawTextCentered("Drag apps here", 11, Weight::Regular, smartSub(),
                         Rect{rect.x, rect.y + rect.h / 2 - 9, rect.w, 18});
    }
}

// The carried icon and the rename field sit above every card, so they are drawn
// once at the end of the desktop surface.
void Manager::drawSmartOverlay() {
    // The card that just took something settles with a brief accent ring, so the
    // add reads as an acceptance rather than as an icon simply appearing.
    if (smartAcceptWidget >= 0 && smartAcceptWidget < int(widgets.size())) {
        const double p = clamp01((nowMs() - smartAcceptAt) / 420.0);
        if (p < 1.0) {
            const Widget& w = widgets[size_t(smartAcceptWidget)];
            const Rect r = smartDrawnRect(w, smartAcceptWidget);
            const float radius = std::clamp(std::min(r.w, r.h) * 0.13f, 12.f, 26.f);
            const int grow = int(std::lround(2 + 7 * p));
            comp.drawRect(r.inflated(grow), radius + float(grow), theme::kAccentRing,
                          float(0.45 * (1.0 - p) * (1.0 - p)));
        }
    }

    // The accommodation itself: the launcher that left the desktop travels into
    // the cell it now occupies, shrinking from the grid icon to the folder icon.
    for (const SmartFly& f : smartFlies) {
        const double p = clamp01((nowMs() - f.start) / f.ms);
        if (p >= 1.0) continue;
        const Rect box = lerpRect(f.from, f.to, easeOutCubic(p));
        const float fade = p < 0.72 ? 1.0f : float(1.0 - (p - 0.72) / 0.28);
        for (int k = 3; k >= 1; --k) {
            const int spread = 2 + 3 * k;
            comp.drawRect(box.inflated(spread).moved(0, 2 + k), float(box.w) * 0.26f + spread,
                          theme::kShadow, 0.05f * fade);
        }
        const float ir = float(box.w) * 0.235f;
        if (!drawAppIcon(box, f.icon, f.wmClass, ir, fade))
            drawAppTile(box, f.name, ir, smartTint(f.name), false);
    }

    if (smartDragMoving && dragWidget >= 0 && dragWidget < int(widgets.size())) {
        const Widget& w = widgets[size_t(dragWidget)];
        if (smartDragFrom >= 0 && smartDragFrom < int(w.apps.size())) {
            const WidgetApp& app = w.apps[size_t(smartDragFrom)];
            // Carried clear of the card, the drop removes the app instead of
            // reordering it. A quiet chip under the icon says so before release.
            const Rect panel = smartDrawnRect(w, dragWidget);
            const bool outside = !panel.inflated(10).contains(pointerX, pointerY);
            if (outside) {
                const int chipW = 84, chipH = 24;
                const Rect chip{pointerX - chipW / 2, smartDragPos.y + smartDragH + 10, chipW, chipH};
                comp.drawRect(chip, float(chipH) * 0.5f, theme::kItemActive, 0.92f);
                drawTextCentered("Remove", 11, Weight::Medium, theme::kTextMuted, chip);
            }
            const int edge = std::max(28, smartDragW);
            const Rect box{smartDragPos.x, smartDragPos.y, edge, edge};
            for (int k = 3; k >= 1; --k) {
                const int spread = 2 + 3 * k;
                comp.drawRect(box.inflated(spread).moved(0, 2 + k), float(edge) * 0.26f + spread,
                              theme::kShadow, 0.05f);
            }
            const float ir = float(edge) * 0.235f;
            if (!drawAppIcon(box, app.icon, app.wmClass, ir, 1.0f))
                drawAppTile(box, app.name, ir, smartTint(app.name), false);
        }
    }
    drawSmartRename();
}

// ---------------------------------------------------------------------------
// Press, drag, release
// ---------------------------------------------------------------------------

void Manager::beginSmartPress(int index, int x, int y, bool resize) {
    if (index < 0 || index >= int(widgets.size())) return;
    Widget& w = widgets[index];
    dragWidget = index;
    widgetResizing = resize;
    widgetGrab = Point{x - w.rect.x, y - w.rect.y};
    hoverWidget = index;

    smartPressWidget = index;
    smartPressPos = Point{x, y};
    smartPressAt = nowMs();
    smartPressMoved = false;
    smartLongFired = false;
    smartEditEntered = false;
    smartDragFrom = -1;
    smartDragSlot = -1;
    smartDragMoving = false;

    const SmartGrid g = smartGridIn(w, smartDrawnRect(w, index));
    smartPressSlot = resize ? -1 : smartSlotAt(g, x, y);

    // Tapping another card's folder leaves the one being edited, the way tapping
    // outside it would.
    if (smartEdit >= 0 && smartEdit != index) {
        smartEdit = -1;
        dirty = true;
    }

    // In edit mode the minus badge removes an app on the press, which is what
    // makes the gesture feel immediate; nothing else is armed for that cell.
    if (!resize && smartPressSlot >= 0 && smartEdit == index &&
        smartPressSlot < int(w.apps.size())) {
        if (smartBadgeRect(g, smartPressSlot).inflated(2).contains(x, y)) {
            const int appIndex = smartPressSlot;
            smartPressSlot = -2;  // consumed: the release does nothing
            smartAppsRemove(index, appIndex);
            return;
        }
    }
    grabPointer();
    dirty = true;
}

// Handles the smart-card half of a drag. Returns false when the gesture is an
// ordinary move of the whole card, which the caller's generic path performs.
bool Manager::smartAppsDrag(int x, int y) {
    if (dragWidget < 0 || dragWidget >= int(widgets.size())) return false;
    Widget& w = widgets[size_t(dragWidget)];
    if (w.kind != WidgetKind::SmartApps) return false;

    if (widgetResizing) {
        const int availW = std::max(metrics::kSmartCell, screenW - w.rect.x);
        const int availH = std::max(metrics::kSmartCell, screenH - metrics::taskbarH - w.rect.y);
        const int maxCols =
            std::clamp(availW / metrics::kSmartCell, metrics::kSmartMin, metrics::kSmartMax);
        const int maxRows =
            std::clamp(availH / metrics::kSmartCell, metrics::kSmartMin, metrics::kSmartMax);
        const int cols = std::clamp(int(std::lround(double(x - w.rect.x) / metrics::kSmartCell)),
                                    metrics::kSmartMin, maxCols);
        int rows = std::clamp(int(std::lround(double(y - w.rect.y) / metrics::kSmartCell)),
                              metrics::kSmartMin, maxRows);
        // The card can never be shrunk past its own contents: the apps keep their
        // cells and the grid simply grows a row instead of hiding anything.
        const int needed = (int(w.apps.size()) + cols - 1) / cols;
        rows = std::max(rows, std::min(maxRows, std::max(metrics::kSmartMin, needed)));
        if (cols != w.cols || rows != w.rows) {
            // Keep the animation continuous: when a new size is committed, the
            // sheet glides from the rectangle it is currently drawn at.
            if (smartPanelWidget != dragWidget) seedSmartPanel(dragWidget, smartDrawnRect(w, dragWidget));
            w.cols = cols;
            w.rows = rows;
            w.rect.w = cols * metrics::kSmartCell;
            w.rect.h = rows * metrics::kSmartCell;
            layoutDesktopIcons();
            dirty = true;
        }
        return true;
    }

    const int dx = x - smartPressPos.x;
    const int dy = y - smartPressPos.y;
    if (!smartPressMoved && dx * dx + dy * dy < kSlop * kSlop) {
        // Held still long enough: the folder opens for editing. The press stays
        // armed so the release cannot also count as a launch.
        if (!smartLongFired && smartPressSlot >= 0 && !widgetResizing) {
            if (nowMs() - smartPressAt >= kLongPressMs) {
                smartLongFired = true;
                smartEditEntered = true;
                smartEdit = dragWidget;
                dirty = true;
            }
        }
        return true;
    }
    smartPressMoved = true;

    if (smartEdit == dragWidget && smartPressSlot >= 0) {
        // Reordering: the app lifts out of its cell and the release drops it in
        // whichever cell is under the pointer.
        const SmartGrid g = smartGridIn(w, smartDrawnRect(w, dragWidget));
        if (smartDragFrom < 0) {
            if (smartPressSlot >= int(w.apps.size())) return true;
            smartDragFrom = smartPressSlot;
            smartDragW = g.icon;
            smartDragH = g.icon;
        }
        smartDragMoving = true;
        smartDragPos = Point{x - smartDragW / 2, y - smartDragH / 2};
        const int slot = smartSlotAt(g, x, y);
        smartDragSlot = slot >= 0 && slot < int(w.apps.size()) ? slot : -1;
        dirty = true;
        return true;
    }
    // A press that started on an icon but is not in edit mode moves the whole
    // folder: there is no reorder outside edit mode, so nothing is lost by
    // treating the sheet as the handle.
    return false;
}

void Manager::smartAppsRelease() {
    const int index = dragWidget;
    if (index < 0 || index >= int(widgets.size())) {
        dragWidget = -1;
        widgetResizing = false;
        return;
    }
    Widget& w = widgets[size_t(index)];
    const SmartGrid g = smartGridIn(w, smartDrawnRect(w, index));
    bool changed = false;
    // A resize changes the saved grid even though no app moved, so it has to be
    // written out on release just as a reorder is.
    const bool resized = widgetResizing;
    // A move is a layout change too: the cell the card now sits in is saved with
    // it, exactly as for the other cards.
    const bool moved = smartPressMoved && !smartDragMoving;
    // Releasing close to the left edge parks the card there, flush with the
    // desktop's own margin. Small cards belong to the column of apps, not to a
    // floating spot in the middle of the wallpaper.
    if (moved && w.rect.x <= 32) {
        w.rect.x = metrics::kWidgetPad;
        seedSmartPanel(index, smartDrawnRect(w, index));
    }

    if (!widgetResizing && !smartPressMoved && !smartEditEntered) {
        if (smartPressSlot >= 0 && smartPressSlot < int(w.apps.size()) &&
            smartEdit != index) {
            const int ai = smartPressSlot;
            const WidgetApp app = w.apps[size_t(ai)];
            if (app.exec == kSettingsExec) {
                openSettings();
            } else if (!app.exec.empty()) {
                noteRecent(app.name, app.exec, app.icon, app.wmClass);
                launchApp(app.exec);
            } else if (!app.path.empty()) {
                launchApp("xdg-open " + shellQuote(app.path));
            }
        }
    } else if (smartDragMoving && smartDragFrom >= 0) {
        // Letting go clear of the card takes the app out of it -- the Android
        // gesture: the icon leaves the folder and lands back on the desktop. A
        // pixel of slack keeps a sloppy drop on the edge from doing it by accident.
        const bool outside = !g.panel.inflated(10).contains(pointerX, pointerY);
        if (outside) {
            const int appIndex = smartDragFrom;
            smartDragFrom = -1;
            smartDragMoving = false;
            dragWidget = -1;
            widgetResizing = false;
            tabletDragEdge = 0;
            smartPressWidget = -1;
            smartPressSlot = -1;
            smartPressMoved = false;
            smartLongFired = false;
            smartEditEntered = false;
            smartDragSlot = -1;
            ungrabPointer();
            smartAppsRemove(index, appIndex);
            layoutDesktopIcons();
            dirty = true;
            return;
        }
        int to = smartDragSlot;
        if (to < 0 || to >= int(w.apps.size())) to = smartDragFrom;
        if (to >= 0 && to != smartDragFrom && smartDragFrom < int(w.apps.size())) {
            WidgetApp app = std::move(w.apps[size_t(smartDragFrom)]);
            w.apps.erase(w.apps.begin() + smartDragFrom);
            w.apps.insert(w.apps.begin() + to, std::move(app));
            changed = true;
        }
    }
    (void)g;

    smartPressWidget = -1;
    smartPressSlot = -1;
    smartPressMoved = false;
    smartLongFired = false;
    smartEditEntered = false;
    smartDragFrom = -1;
    smartDragSlot = -1;
    smartDragMoving = false;
    dragWidget = -1;
    widgetResizing = false;
    tabletDragEdge = 0;
    ungrabPointer();
    layoutDesktopIcons();
    if (changed || resized || moved) saveDesktopLayout();
    dirty = true;
}

// One frame of the folder's own motion: the long-press dwell, the jiggle that
// keeps edit mode alive, the accommodation fly-in, and the growth of the sheet.
void Manager::tickSmartApps(double now, double dtMs) {
    (void)now;
    const double t = nowMs();

    for (size_t i = smartFlies.size(); i-- > 0;) {
        if (t - smartFlies[i].start >= smartFlies[i].ms)
            smartFlies.erase(smartFlies.begin() + long(i));
        else
            dirty = true;
    }
    if (smartAcceptWidget >= 0) {
        if (t - smartAcceptAt >= 420.0) smartAcceptWidget = -1;
        else dirty = true;
    }

    if (dragWidget >= 0 && dragWidget < int(widgets.size()) &&
        widgets[size_t(dragWidget)].kind == WidgetKind::SmartApps && !smartPressMoved &&
        smartPressSlot >= 0 && !smartLongFired && !widgetResizing &&
        t - smartPressAt >= kLongPressMs) {
        smartLongFired = true;
        smartEditEntered = true;
        smartEdit = dragWidget;
        dirty = true;
    }
    if (smartEdit >= 0) {
        // The board's jiggle is a continuous animation, so the frame is dirty for
        // as long as edit mode is up.
        if (smartEdit >= int(widgets.size()) ||
            widgets[size_t(smartEdit)].kind != WidgetKind::SmartApps)
            smartEdit = -1;
        else
            dirty = true;
    }

    if (smartPanelWidget >= 0 && (smartPanelWidget >= int(widgets.size()) ||
                                  widgets[size_t(smartPanelWidget)].kind != WidgetKind::SmartApps))
        smartPanelWidget = -1;
    if (smartPanelWidget < 0) return;

    const Widget& w = widgets[size_t(smartPanelWidget)];
    const motion::Vec2 goalPos{double(w.rect.x), double(w.rect.y)};
    const motion::Vec2 goalSize{double(w.rect.w), double(w.rect.h)};
    // While the card is being carried it must stay welded to the pointer: the
    // panel and everything on it move as one, with no easing to lag behind.
    const bool moving = dragWidget == smartPanelWidget && !widgetResizing && !smartDragMoving;
    if (moving) {
        smartPanelPos.setValue(goalPos);
        smartPanelPos.setVelocity(motion::Vec2{});
        smartPanelSize.setValue(goalSize);
        smartPanelSize.setVelocity(motion::Vec2{});
        return;
    }
    const double dt = std::clamp(dtMs, 0.0, 100.0) / 1000.0;
    smartPanelPos.configure(6.2, 0.88);
    smartPanelSize.configure(6.2, 0.88);
    smartPanelPos.step(goalPos, dt);
    smartPanelSize.step(goalSize, dt);
    if (smartPanelPos.settled(goalPos, 0.35)) {
        smartPanelPos.setValue(goalPos);
        smartPanelPos.setVelocity(motion::Vec2{});
    }
    if (smartPanelSize.settled(goalSize, 0.35)) {
        smartPanelSize.setValue(goalSize);
        smartPanelSize.setVelocity(motion::Vec2{});
    }
    const Rect cur = smartDrawnRect(w, smartPanelWidget);
    if (cur != w.rect) dirty = true;
}

// Takes one app out of the folder. A launcher the folder had taken off the
// desktop goes back there, so removing is always reversible; the board closes the
// gap with the same spring the rest of the shell moves on.
bool Manager::smartAppsRemove(int index, int appIndex) {
    if (index < 0 || index >= int(widgets.size())) return false;
    Widget& w = widgets[size_t(index)];
    if (w.kind != WidgetKind::SmartApps) return false;
    if (appIndex < 0 || appIndex >= int(w.apps.size())) return false;
    const WidgetApp taken = w.apps[size_t(appIndex)];
    w.apps.erase(w.apps.begin() + appIndex);
    const bool restored = !taken.stash.empty() && restoreStashedEntry(taken.stash);
    if (restored) refreshDesktop();
    dirty = true;
    saveDesktopLayout();
    return true;
}

// ---------------------------------------------------------------------------
// Adding apps and desktop folders
// ---------------------------------------------------------------------------

int Manager::smartAppsDropTarget(int x, int y) const {
    const int i = widgetAt(x, y);
    if (i < 0) return -1;
    if (widgets[size_t(i)].kind != WidgetKind::SmartApps) return -1;
    // The corner grip is a resize handle, not a drop target.
    if (widgetGripRect(widgets[size_t(i)]).contains(x, y)) return -1;
    return i;
}

namespace {

// One app, if the folder does not already hold it and still has a free cell.
bool addOne(Widget& w, const std::string& name, const std::string& exec,
            const std::string& path, const std::string& icon, const std::string& wmClass,
            int capacity) {
    if (exec.empty() && path.empty()) return false;
    for (const WidgetApp& a : w.apps) {
        if (a.exec != exec || a.name != name) continue;
        if (exec.empty() && a.path != path) continue;
        return false;
    }
    if (int(w.apps.size()) >= capacity) return false;
    w.apps.push_back(WidgetApp{name, exec, path, icon, wmClass, std::string()});
    return true;
}

}  // namespace

// The most cells a card can show without leaving the screen: its own column
// count, capped by the rows that fit under it and by the grid's own limit.
static int smartCapacityFrom(const Widget& w, int screenBottom) {
    const int cols = clampi(w.cols, metrics::kSmartMin, metrics::kSmartMax);
    const int fitRows = std::max(metrics::kSmartMin,
                                 (screenBottom - w.rect.y) / metrics::kSmartCell);
    const int rows = std::min(fitRows, metrics::kSmartMaxRows);
    return cols * rows;
}

// The app a point lands on inside a card, or -1. What the context menu needs to
// offer "Remove App" only when it was opened on an icon.
int Manager::smartAppsAppAt(int index, int x, int y) const {
    if (index < 0 || index >= int(widgets.size())) return -1;
    const Widget& w = widgets[size_t(index)];
    if (w.kind != WidgetKind::SmartApps) return -1;
    const SmartGrid g = smartGridIn(w, smartDrawnRect(w, index));
    const int slot = smartSlotAt(g, x, y);
    if (slot < 0 || slot >= int(w.apps.size())) return -1;
    return slot;
}

bool Manager::smartAppsTake(int index, const DesktopItem& item, const Rect& fromIcon) {
    if (index < 0 || index >= int(widgets.size())) return false;
    Widget& w = widgets[size_t(index)];
    if (w.kind != WidgetKind::SmartApps) return false;

    const int bottom = screenH - metrics::taskbarH;
    const int capacity = smartCapacityFrom(w, bottom);
    if (int(w.apps.size()) >= capacity) return false;

    // The sheet starts growing from the rectangle it is drawn at right now.
    if (smartPanelWidget != index) seedSmartPanel(index, smartDrawnRect(w, index));

    std::string flyIcon = item.icon;
    std::string flyClass;
    bool changed = false;
    // Which cells were just filled, so each new app can fly into its own one.
    std::vector<int> newApps;
    std::vector<std::string> newIcons;
    std::vector<std::string> newNames;

    if (item.isDir && !item.path.empty()) {
        // A folder leaves the desktop whole and is read back out of the store, so
        // the card holds files that still exist. An empty one is refused: there
        // would be nothing to show and the folder would vanish for no reason.
        if (scanDesktopFolder(item.path).empty()) return false;
        const std::string stored = stashDesktopEntry(item.path);
        if (stored.empty()) return false;
        for (const DesktopItem& inside : scanDesktopFolder(stored)) {
            if (int(w.apps.size()) >= capacity) break;
            if (!addOne(w, inside.name, inside.exec, inside.path, inside.icon, std::string(),
                        capacity))
                continue;
            w.apps.back().stash = inside.path;
            newApps.push_back(int(w.apps.size()) - 1);
            newIcons.push_back(w.apps.back().icon);
            newNames.push_back(w.apps.back().name);
            changed = true;
        }
        if (!changed) return false;
        if (w.name.empty() || w.name == "Smart Apps") w.name = item.name;
        flyIcon = "folder";
    } else {
        if (!addOne(w, item.name, item.exec, item.path, item.icon, std::string(), capacity))
            return false;
        // A launcher moves off the desktop into the store, so from now on it is
        // reachable through the folder alone -- that is what taking an app into a
        // folder means. A plain file simply joins the grid where it is.
        if (!item.path.empty()) {
            const std::string stored = stashDesktopEntry(item.path);
            if (!stored.empty()) {
                w.apps.back().path = stored;
                w.apps.back().stash = stored;
            }
        }
        flyClass = w.apps.back().wmClass;
        newApps.push_back(int(w.apps.size()) - 1);
        newIcons.push_back(w.apps.back().icon);
        newNames.push_back(w.apps.back().name);
        changed = true;
    }
    if (!changed) return false;

    // The card grows a row rather than hiding the new app behind a page that no
    // longer exists. It is then pulled back inside the screen if it has grown
    // past the bottom edge.
    const int cols = clampi(w.cols, metrics::kSmartMin, metrics::kSmartMax);
    const int fitRows = std::max(metrics::kSmartMin, (bottom - w.rect.y) / metrics::kSmartCell);
    const int needed = (int(w.apps.size()) + cols - 1) / cols;
    w.rows = std::clamp(std::max(w.rows, needed), metrics::kSmartMin,
                        std::min(fitRows, metrics::kSmartMaxRows));
    w.rect.h = w.rows * metrics::kSmartCell;
    if (w.rect.bottom() > bottom) w.rect.y = std::max(0, bottom - w.rect.h);

    // The accommodation: the desktop's icon flies into the cell it went to. A
    // folder spills its apps out one after another, so the hand-over reads as
    // the folder emptying itself into the grid rather than as a cut.
    const SmartGrid after = smartGridIn(w, w.rect);
    const Rect start = fromIcon.empty()
                           ? Rect{w.rect.x + w.rect.w / 2 - 24, w.rect.y + w.rect.h / 2 - 24,
                                  48, 48}
                           : fromIcon;
    const double base = nowMs();
    smartFlies.clear();  // an older flight for this card would fight the new one
    for (size_t k = 0; k < newApps.size(); ++k) {
        SmartFly fly;
        fly.from = start;
        fly.to = smartIconRect(after, newApps[k]);
        fly.icon = k < newIcons.size() ? newIcons[k] : flyIcon;
        fly.wmClass = flyClass;
        fly.name = k < newNames.size() ? newNames[k] : item.name;
        fly.widget = index;
        fly.appIndex = newApps[k];
        fly.start = base + 45.0 * double(k);
        fly.ms = 340.0;
        smartFlies.push_back(fly);
    }
    if (smartFlies.size() > 12) smartFlies.erase(smartFlies.begin(),
                                                 smartFlies.end() - 12);
    smartAcceptWidget = index;
    smartAcceptAt = base;

    layoutDesktopIcons();
    refreshDesktop();  // the entry has left the desktop
    dirty = true;
    saveDesktopLayout();
    return true;
}

// ---------------------------------------------------------------------------
// Renaming
// ---------------------------------------------------------------------------

void Manager::beginSmartRename(int index) {
    if (index < 0 || index >= int(widgets.size())) return;
    if (widgets[size_t(index)].kind != WidgetKind::SmartApps) return;
    smartRenameWidget = index;
    smartRenameText = widgets[size_t(index)].name.empty() ? std::string("Smart Apps")
                                                          : widgets[size_t(index)].name;
    grabPointer();
    grabShellKeyboard();
    dirty = true;
}

void Manager::cancelSmartRename() {
    if (smartRenameWidget < 0) return;
    smartRenameWidget = -1;
    smartRenameText.clear();
    ungrabPointer();
    ungrabShellKeyboard();
    dirty = true;
}

void Manager::commitSmartRename() {
    const int index = smartRenameWidget;
    std::string name = smartRenameText;
    // Leading and trailing spaces are a typing habit, not part of the name.
    while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.erase(0, 1);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
    const bool valid = index >= 0 && index < int(widgets.size()) &&
                       widgets[size_t(index)].kind == WidgetKind::SmartApps;
    cancelSmartRename();
    if (!valid || name.empty()) return;
    widgets[size_t(index)].name = name;
    saveDesktopLayout();
    dirty = true;
}

// The field a rename is typed into: a band across the top of the card, drawn only
// while it is being typed into. The card itself carries no title.
Rect Manager::smartRenameRect() const {
    if (smartRenameWidget < 0 || smartRenameWidget >= int(widgets.size())) return {};
    const Widget& w = widgets[size_t(smartRenameWidget)];
    if (w.kind != WidgetKind::SmartApps) return {};
    const Rect r = smartDrawnRect(w, smartRenameWidget);
    const int h = 26;
    return Rect{r.x + 6, r.y + 6, std::max(1, r.w - 12), h};
}

void Manager::drawSmartRename() {
    if (smartRenameWidget < 0 || smartRenameWidget >= int(widgets.size())) return;
    Widget& w = widgets[size_t(smartRenameWidget)];
    if (w.kind != WidgetKind::SmartApps) return;
    const Rect field = smartRenameRect();
    if (field.empty()) return;
    const float radius = std::clamp(std::min(w.rect.w, w.rect.h) * 0.13f, 12.f, 26.f);

    comp.drawAcrylic(field, std::min(radius, 12.f),
                     smartLight() ? theme::kSmartGlassLight : theme::kSmartGlassDark,
                     smartLight() ? 0.96f : 0.92f, theme::kAccentRing, 1.0f);
    const TextTex t = text.get(smartRenameText, 12, Weight::Bold);
    if (!t.tex) return;
    const int tx = field.x + (field.w - t.w) / 2;
    comp.drawText(t, Rect{tx, field.y + (field.h - t.h) / 2, t.w, t.h}, smartInk(), 1.0f);
    if (std::fmod(nowMs(), 1000.0) < 500.0)
        comp.drawRect(Rect{tx + t.w + 2, field.y + 5, 1, field.h - 10}, 0.f, smartInk(), 1.0f);
}

}  // namespace wm
