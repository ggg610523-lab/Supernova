// Everything the shell paints. All of it is expressed in screen pixels and
// handed to the compositor's shader as rects, so the same code produces the
// rounded corners, the Mica captions and the acrylic surfaces.
#include "manager.h"

#include <ctime>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "theme.h"

namespace wm {

namespace metrics {
// The one piece of shell geometry the user sets; Manager::updateTaskbarResize
// moves it while the bar's edge is being dragged.
int taskbarH = kTaskbarDefaultH;
}  // namespace metrics

// Layers render() can skip, for WIN11WM_LAYERS profiling. Each bit masks one
// stage of the paint order so a headless run can attribute frame cost.
enum : unsigned {
    kLayWallpaper = 1u << 0,  // wallpaper, desktop icons, widgets
    kLayClients = 1u << 1,    // managed client windows + launch placeholders
    kLaySnap = 1u << 2,       // snap preview
    kLayTaskbar = 1u << 3,    // taskbar
    kLayStart = 1u << 4,      // Start menu
    kLayTaskView = 1u << 5,   // task view
    kLayAltTab = 1u << 6,     // Alt-Tab switcher
    kLayContext = 1u << 7,    // context menu
    kLayTablet = 1u << 8,     // tablet chrome / switcher / splash
    kLayCC = 1u << 9,         // Control Centre
    kLayDialogs = 1u << 10,   // rename + confirm dialogs
    kLayAll = 0x7FFu,
};

namespace {

// Trim `s` with an ellipsis so it fits in `maxW` pixels.
std::string ellipsize(Text& text, const std::string& s, int px, int maxW) {
    if (maxW <= 0) return std::string();
    if (text.measure(s, px) <= maxW) return s;
    std::string out = s;
    while (!out.empty() && text.measure(out + "\u2026", px) > maxW) {
        out.resize(out.size() - 1);
        // Never split a multi-byte sequence.
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) {
            out.resize(out.size() - 1);
        }
    }
    return out + "\u2026";
}

Color tileTint(const std::string& name) {
    size_t h = 5381;
    for (unsigned char ch : name) h = h * 33 + ch;
    return theme::kTileTints[h % 6];
}

}  // namespace

void Manager::render() {
    layoutTaskbar();
    if (startOpen) layoutStartMenu();

    comp.beginFrame();
    // Benchmark hook (WIN11WM_LAYERS): a bitmask that skips whole layers, so a
    // headless run can attribute GPU time to a specific part of the shell.
    // Unset in normal use, where every layer is drawn.
    unsigned layers = ~0u;
    if (const char* v = std::getenv("WIN11WM_LAYERS")) layers = std::strtoul(v, nullptr, 0);
    const auto on = [&](unsigned bit) { return (layers & bit) != 0; };

    if (on(kLayWallpaper)) drawDesktop();

    // Bottom to top, exactly like the X stacking order, so the focused window
    // is painted last (and therefore on top).
    for (auto& cp : clients) {
        if (!on(kLayClients)) break;
        Client* c = cp.get();
        if (!c->mapped || c->unredirected) continue;
        // A minimising window stays on screen (warping into its taskbar icon)
        // until the magic-lamp animation has fully played, then it is unmapped
        // and skipped from here on.
        if (c->minimized && c->minFade >= 1.0) continue;
        // Re-binding the pixmap here (rather than per motion event) keeps a
        // resize down to one pixmap per frame.
        if (c->pixW <= 0 || !c->tex.valid()) ensurePixmap(c);
        drawClientSprite(c);
    }

    if (on(kLayClients)) drawLaunches();

    if (on(kLaySnap)) drawSnapPreview();
    if (on(kLayTaskbar) && tabletAnim < 0.999) drawTaskbar();
    if (on(kLayStart) && (startOpen || startAnim > 0.0)) drawStartMenu();
    if (on(kLayTaskView) && (taskViewOpen || taskViewAnim > 0.0)) drawTaskView();
    if (on(kLayAltTab) && (altTabOpen || altTabAnim > 0.0)) drawAltTab();
    if (on(kLayContext) && (contextOpen || contextAnim > 0.0)) drawContextMenu();
    // The iOS app switcher (swipe up and hold on the home bar) sits under the
    // status bar and the home indicator.
    if (on(kLayTablet) && (tabletSwitcher || tabletSwitcherAnim > 0.0))
        drawTabletSwitcher();
    // The tablet status bar and home indicator float above an open app, the way
    // iOS keeps them over the app that is running.
    if (on(kLayTablet) && tabletAnim > 0.001) drawTabletChrome();
    if (on(kLayCC) && (ccOpen || ccAnim > 0.0)) drawControlCenter();
    // The desktop's own dialogs are the last word on the screen while they are up,
    // so nothing behind them reads as actionable.
    if (on(kLayDialogs)) {
        drawDesktopRename();
        drawConfirmDelete();
    }
    // The mode transition splash owns the whole screen, so it is painted last.
    if (on(kLayTablet) && modeSwitching) drawTabletSplash();
    if (opts->stats) drawStats();
}

// The desktop grid lays out top-to-bottom and then starts a new column to the
// right, which is the order Windows fills its desktop in.
void Manager::layoutDesktopIcons() {
    desktopIconRects.assign(desktopItems.size(), Rect{});
    const int cellW = 92, cellH = 92, gapX = 6, gapY = 2;
    const int marginX = 10, marginY = 10;
    const int usableH = screenH - metrics::taskbarH - marginY;
    const int rows = std::max(1, (usableH + gapY) / (cellH + gapY));
    const auto cellAt = [&](int n) {
        const int col = n / rows, row = n % rows;
        return Rect{marginX + col * (cellW + gapX), marginY + row * (cellH + gapY), cellW, cellH};
    };
    // Icons the user dragged by hand keep the cell they were dropped in. They are
    // resolved first so the automatic grid below can step over them, the same way
    // it already steps over the widgets.
    const int maxX = std::max(0, screenW - cellW);
    const int maxY = std::max(0, screenH - metrics::taskbarH - cellH);
    std::vector<Rect> placed;
    std::vector<bool> isPlaced(desktopItems.size(), false);
    for (size_t i = 0; i < desktopItems.size(); ++i) {
        const auto it = desktopIconPlacement.find(desktopItems[i].path);
        if (it == desktopIconPlacement.end()) continue;
        // Clamp, so a spot saved for a different screen or taskbar height still
        // lands on screen rather than off in the margin.
        const int px = std::clamp(it->second.x, 0, maxX);
        const int py = std::clamp(it->second.y, 0, maxY);
        desktopIconRects[i] = Rect{px, py, cellW, cellH};
        placed.push_back(desktopIconRects[i]);
        isPlaced[i] = true;
    }
    // Walk the same top-to-bottom, left-to-right order, but step over any cell a
    // widget or a hand-placed icon occupies so the two never stack. Each icon
    // starts the scan at its own slot rather than after the previous icon's, which
    // is what keeps the rest of the grid still: dragging one icon away frees its
    // slot without tugging every icon behind it across the desktop.
    std::vector<bool> cellUsed;
    for (size_t i = 0; i < desktopItems.size(); ++i) {
        if (isPlaced[i]) continue;
        int n = int(i);
        Rect cell = cellAt(n);
        for (int guard = 0; guard < 4096; ++guard) {
            cell = cellAt(n);
            if (size_t(n) >= cellUsed.size()) cellUsed.resize(size_t(n) + 1, false);
            bool blocked = cellUsed[size_t(n)];
            if (!blocked)
                for (const Widget& w : widgets)
                    if (cell.inflated(8).intersects(w.rect)) {
                        blocked = true;
                        break;
                    }
            if (!blocked)
                for (const Rect& p : placed)
                    if (cell.inflated(4).intersects(p)) {
                        blocked = true;
                        break;
                    }
            if (!blocked) {
                cellUsed[size_t(n)] = true;
                break;
            }
            ++n;
        }
        desktopIconRects[i] = cell;
    }
    // Keep the *shown* cells in step with the targets we just computed. Icons
    // that already existed keep their current on-screen position (they are eased
    // across by animateDesktopIconReflow()); a brand new entry -- only possible
    // when the Desktop directory changes -- starts at its target so it does not
    // fly in from the origin.
    if (desktopIconDraw.size() != desktopIconRects.size()) {
        const size_t keep = std::min(desktopIconDraw.size(), desktopIconRects.size());
        desktopIconDraw.resize(desktopIconRects.size());
        for (size_t i = keep; i < desktopIconDraw.size(); ++i)
            desktopIconDraw[i] = desktopIconRects[i];
    }
}

// Every frame, spring each icon's shown cell toward the cell the grid wants it
// in. A spring rather than an exponential ease: icons settle with a hint of
// overshoot, retargeting mid-flight keeps the velocity they already had (so
// fast drags and widget reflows never stutter), and the grabbed icon is pinned
// exactly under the pointer. The whole system is critically-ish damped (~0.3 s)
// so it reads as motion without ever staying in the way.
void Manager::animateDesktopIconReflow(double dtMs) {
    if (desktopIconDraw.size() != desktopIconRects.size()) layoutDesktopIcons();
    const size_t n = desktopItems.size();

    // Keep the springs the same length as the item list -- matching either way,
    // so an entry appearing does not fly in from the origin and one disappearing
    // does not leave a stale spring to inherit later.
    if (desktopIconPos.size() != n) {
        const size_t keep = std::min(desktopIconPos.size(), n);
        desktopIconPos.resize(n);
        for (size_t i = keep; i < n; ++i) {
            const Rect& c = i < desktopIconDraw.size() ? desktopIconDraw[i] : desktopIconRects[i];
            desktopIconPos[i].reset(motion::Vec2{double(c.x), double(c.y)});
        }
    }

    const double dt = std::clamp(dtMs, 0.0, 100.0) / 1000.0;
    // A touch under critical (zeta < 1) is what gives the settle its life; the
    // frequency is close to the window geometry springs so the whole shell moves
    // to one rhythm.
    for (size_t i = 0; i < n && i < desktopIconRects.size(); ++i) {
        motion::Spring2& sp = desktopIconPos[i];
        // A playful profile: the dragged icon's neighbours part with a small
        // overshoot and every icon keeps its momentum when the target moves.
        sp.configure(3.0, motion::kPlayful.zeta);
        const Rect target = desktopIconRects[i];
        const motion::Vec2 tgt{double(target.x), double(target.y)};
        if (int(i) == dragDesktopIcon && desktopIconDragging) {
            // Held against the pointer: no lag, no spring, no velocity.
            sp.setValue(tgt);
            sp.setVelocity(motion::Vec2{});
        } else {
            sp.step(tgt, dt);
            // A spring settles asymptotically; snap the last fraction of a pixel
            // so the glide always terminates and never repaints forever.
            if (sp.settled(tgt, 0.4)) {
                sp.setValue(tgt);
                sp.setVelocity(motion::Vec2{});
            }
        }
        if (i >= desktopIconDraw.size()) continue;
        const motion::Vec2 v = sp.value();
        const Rect next{int(std::lround(v.x)), int(std::lround(v.y)), target.w, target.h};
        Rect& cur = desktopIconDraw[i];
        if (cur != next) {
            cur = next;
            dirty = true;
        }
    }

    // The lift: the carried icon rises quickly and sinks back a touch slower, so
    // grabbing and dropping both read on screen.
    desktopIconLift.setFrequency(4.2);
    desktopIconLift.zeta = 0.72;
    const double want = (dragDesktopIcon >= 0 && desktopIconDragging) ? 1.0 : 0.0;
    const bool moving = !desktopIconLift.settled(want, 1e-3);
    desktopIconLift.step(want, dt);
    if (moving) dirty = true;
}


// a .desktop launcher. A click selects, a double click opens (input.cpp).
// Everything sitting on the user's Desktop folder: a folder, a dropped file or
void Manager::drawDesktopIcons() {
    if (desktopIconRects.size() != desktopItems.size() ||
        desktopIconDraw.size() != desktopItems.size())
        layoutDesktopIcons();
    const double lift = std::clamp(desktopIconLift.value, 0.0, 1.4);

    const auto drawOne = [&](size_t i, bool lifted) {
        const DesktopItem& item = desktopItems[i];
        // Draw the eased cell, not the target: this is what makes the icons
        // glide while a widget or a carried icon reflows the grid.
        const Rect cell = desktopIconDraw[i];
        const bool selected = int(i) == selectedDesktopIcon;
        const bool hovered = int(i) == hoverDesktopIcon;
        // The lift only applies to the icon in hand.
        const double lg = lifted ? lift : 0.0;
        // Hover and selection share one eased amount, so the highlight fades in
        // and, when the selection moves on, cross-fades to the next icon.
        const double hl = i < desktopIconHover.size() ? desktopIconHover[i] : 0.0;
        // A grabbed icon swells slightly and casts a soft shadow, so it reads as
        // being held above the desktop rather than painted on it. A handful of
        // rounded layers with a quadratic falloff stands in for a real blur, the
        // same trick the taskbar uses for its soft top edge.
        const int grow = int(std::lround(48.0 * 0.10 * lg));
        const Rect icon{cell.x + (cell.w - (48 + grow)) / 2, cell.y + 8 - grow / 2,
                        48 + grow, 48 + grow};
        if (lg > 0.001) {
            constexpr int kLayers = 5;
            for (int s = kLayers; s >= 1; --s) {
                const float t = float(s) / float(kLayers);
                const float a = float(0.34 * lg) * (1.0f - t) * (1.0f - t);
                const int spread = int(std::lround(2.0 + 8.0 * t));
                comp.drawRect(Rect{icon.x - spread, icon.y - spread + int(std::lround(4.0 * lg)),
                                   icon.w + 2 * spread, icon.h + 2 * spread},
                              10.f + float(spread), theme::kShadow, a);
            }
        }
        if (hl > 0.001) {
            const Color fill =
                selected ? Color{theme::kAccent.r, theme::kAccent.g, theme::kAccent.b, 0.30f}
                         : theme::kItemHover;
            comp.drawRect(Rect{cell.x + 2, cell.y + 2, cell.w - 4, cell.h - 4}, 6.f, fill,
                          float(hl));
        }
        // The item's own icon first, then a generic one, then a letter tile.
        const char* fallback = item.isDir ? "folder" : "text-plain";
        if (!drawAppIcon(icon, item.icon, std::string(), 6.f, 1.0f) &&
            !drawAppIcon(icon, fallback, std::string(), 6.f, 1.0f)) {
            drawAppTile(icon, item.name, 8.f, tileTint(item.name), hovered);
        }
        const std::string label = ellipsize(text, item.name, 11, cell.w - 8);
        if (label.empty()) return;
        const TextTex t = text.get(label, 11, Weight::Regular);
        if (!t.tex) return;
        const int lx = cell.x + (cell.w - t.w) / 2;
        const int ly = icon.bottom() + 6;
        // Desktop labels sit on a photo, so a one pixel dark drop shadow keeps
        // them legible over a light patch of wallpaper.
        comp.drawText(t, Rect{lx + 1, ly + 1, t.w, t.h}, Color{0.f, 0.f, 0.f, 0.65f}, 1.0f);
        comp.drawText(t, Rect{lx, ly, t.w, t.h}, theme::kDesktopLabel, 1.0f);
    };

    // Draw everything but the carried icon first, then the carried icon on top of
    // the stack so it is never clipped by a neighbour it passes over.
    for (size_t i = 0; i < desktopItems.size() && i < desktopIconDraw.size(); ++i) {
        if (int(i) == dragDesktopIcon) continue;
        drawOne(i, false);
    }
    if (dragDesktopIcon >= 0 && size_t(dragDesktopIcon) < desktopIconDraw.size() &&
        size_t(dragDesktopIcon) < desktopItems.size())
        drawOne(size_t(dragDesktopIcon), true);
}

// ---------------------------------------------------------------------------
// Desktop folders: the rename field and the delete confirmation
// ---------------------------------------------------------------------------

// The name is typed into the icon's own label, so the entry never moves and the
// grid around it stays where it was. The field is deliberately plain -- a lit
// outline and a caret -- so it reads as "this text is editable" rather than as
// another window.
void Manager::drawDesktopRename() {
    const Rect field = desktopRenameRect();
    if (field.w <= 0 || field.h <= 0) return;
    comp.drawRect(field, 5.f, theme::kFieldFill, 0.95f);
    comp.drawRect(field, 5.f, theme::kAccentRing, 1.0f);
    const TextTex t = text.get(desktopRenameText, 11, Weight::Regular);
    if (t.tex) {
        const int tx = field.x + (field.w - t.w) / 2;
        const int ty = field.y + (field.h - t.h) / 2;
        comp.drawText(t, Rect{tx, ty, t.w, t.h}, theme::kText, 1.0f);
        // The caret blinks on the second, which is enough to read as a text field.
        if (std::fmod(nowMs(), 1000.0) < 500.0)
            comp.drawRect(Rect{tx + t.w + 1, field.y + 4, 1, field.h - 8}, 0.f, theme::kText,
                          1.0f);
    }
}

// Deleting a folder takes whatever was in it and cannot be undone, so the dialog
// says which folder, says what it costs, and makes backing out the obvious
// choice: Cancel sits first, on the left, under the pointer that got here.
void Manager::drawConfirmDelete() {
    if (!confirmDeleteOpen) return;
    const int panelW = std::min(420, screenW - 48);
    const int panelH = 196;
    const Rect panel{(screenW - panelW) / 2, (screenH - panelH) / 2, panelW, panelH};

    comp.drawRect(Rect{0, 0, screenW, screenH}, 0.f, theme::kScrim, 0.55f);
    comp.drawAcrylic(panel, float(metrics::kFlyoutRadius), theme::kFlyoutTint, 0.95f,
                     theme::kShellBorder, 1.0f);

    drawTextCentered("Delete this folder?", 17, Weight::Bold, theme::kText,
                     Rect{panel.x, panel.y + 26, panel.w, 24});
    drawTextCentered(ellipsize(text, confirmDeleteName, 14, panel.w - 48), 14, Weight::Regular,
                     theme::kTextMuted, Rect{panel.x, panel.y + 56, panel.w, 20});
    drawTextCentered("Everything inside it goes too, and this cannot be undone.", 12,
                     Weight::Regular, theme::kTextMuted, Rect{panel.x, panel.y + 82, panel.w, 20});

    struct Btn {
        Rect r;
        const char* label;
        bool danger;
    };
    const Btn btns[2] = {{confirmDeleteCancel, "Cancel", false},
                         {confirmDeleteOk, "Delete", true}};
    for (const Btn& b : btns) {
        comp.drawRect(b.r, 6.f, theme::kFieldFill, 0.92f);
        comp.drawRect(b.r, 6.f, b.danger ? theme::kAccent : theme::kShellBorder,
                      b.danger ? 0.70f : 1.0f);
        drawTextCentered(b.label, 13, Weight::Medium, theme::kText,
                         Rect{b.r.x, b.r.y + (b.r.h - 18) / 2, b.r.w, 18});
    }
}

// ---------------------------------------------------------------------------
// App launch animation (iOS style)
// ---------------------------------------------------------------------------
// iOS opens an app by growing the tapped icon into the app's window with a
// short, decelerating zoom while the icon cross-fades into the app. We do the
// same on the desktop: the icon's tile expands into the window the app will
// occupy, showing the app's own icon, so a double-click is answered instantly
// even when the process takes a moment to map its first window.
namespace {
constexpr double kLaunchMs = 380.0;          // icon -> placeholder grow
constexpr double kLaunchSettleMs = 240.0;    // retarget onto the real frame
constexpr double kLaunchFadeMs = 240.0;      // placeholder cross-fade once claimed
constexpr double kLaunchTimeoutMs = 8000.0;  // give up if nothing ever maps
}  // namespace

void Manager::beginLaunchAnim(const DesktopItem& item, const Rect& fromIcon) {
    LaunchAnim a;
    a.from = fromIcon;
    a.rect = fromIcon;
    a.icon = item.icon;
    a.name = item.name;
    a.start = nowMs();
    a.ms = kLaunchMs;
    // A generous, centred window the tile can grow into. placeNewClient centres
    // new windows too, so the real frame usually lands close by and the morph in
    // claimLaunch smooths out whatever difference remains.
    const int bottom = screenH - metrics::taskbarH;
    const int w = std::clamp(int(screenW * 0.60), metrics::kMinW,
                             std::max(metrics::kMinW, screenW - 120));
    const int h = std::clamp(int(bottom * 0.60), metrics::kMinH,
                             std::max(metrics::kMinH, bottom - 120));
    a.to = Rect{(screenW - w) / 2, (bottom - h) / 2, w, h};
    launches.push_back(std::move(a));
    dirty = true;
}

// The window the launch spawned has finally mapped: hand it the tile so it can
// grow out of the placeholder instead of appearing on top of it.
void Manager::claimLaunch(Client* c) {
    if (!c || launches.empty()) return;
    const double now = nowMs();
    for (LaunchAnim& a : launches) {
        if (a.client || now - a.start > kLaunchTimeoutMs) continue;
        a.client = c;
        a.claimStart = now;
        c->fromLaunch = true;  // no scale-in: match the placeholder exactly
        // The placeholder could only *guess* at the window's size before the app
        // answered. Now the real frame is known, retarget the tile onto it and
        // put both the tile and the window on the very same timeline: they ease
        // from the same point to the same rect over the same 240ms, so the two
        // are exactly the same size throughout the cross-fade and there is no
        // jump when one hands over to the other.
        a.from = a.rect;
        a.to = c->frame;
        a.start = now;
        a.ms = kLaunchSettleMs;
        c->animFrom = a.rect;
        c->animStart = now;
        c->animMs = kLaunchSettleMs;
        dirty = true;
        return;
    }
}

void Manager::tickLaunches(double now, double dtMs) {
    if (launches.empty()) return;
    (void)dtMs;
    for (size_t i = launches.size(); i-- > 0;) {
        LaunchAnim& a = launches[i];
        // The tile keeps easing toward `to` even after a claim -- that is how it
        // arrives at exactly the window's rect while the two cross-fade.
        const double p = clamp01((now - a.start) / a.ms);
        // Same curve as the window's frame morph (fluentEase), so tile and window
        // are the same size at every instant, not just at the end.
        a.rect = lerpRect(a.from, a.to, fluentEase(p));
        if (a.client) {
            // The real window is on screen now: fade the placeholder away.
            if (now - a.claimStart >= kLaunchFadeMs) launches.erase(launches.begin() + i);
            else dirty = true;
            continue;
        }
        // A launch that never maps a window must not linger for ever. A tile that
        // has finished growing just waits quietly -- no repaint until the app
        // answers (claimLaunch restarts it) or it times out.
        if (now - a.start > kLaunchTimeoutMs) launches.erase(launches.begin() + i);
        else if (p < 1.0) dirty = true;
    }
}

// The placeholder tile: a Fluent glass frame with the app's icon in it, painted
// above the windows so it reads as "the app is opening".
void Manager::drawLaunches() {
    if (launches.empty()) return;
    const double now = nowMs();
    for (const LaunchAnim& a : launches) {
        double opacity = 1.0;
        if (a.client) opacity = clamp01(1.0 - (now - a.claimStart) / kLaunchFadeMs);
        if (opacity <= 0.01) continue;
        const Rect r = a.rect;
        if (r.w < 8 || r.h < 8) continue;
        const float rad = std::min(float(metrics::kRadius) + 6.f, std::min(r.w, r.h) * 0.22f);
        comp.drawAcrylic(r, rad, theme::kShellTint, 0.86f, theme::kShellBorder, float(opacity));
        // The app's own icon, sized to the tile: it starts exactly where the
        // desktop icon was and ends as a large app glyph, exactly like iOS.
        const int side = std::max(24, int(std::min(r.w, r.h) * 0.42));
        const Rect box{r.x + (r.w - side) / 2, r.y + (r.h - side) / 2 - side / 8, side, side};
        if (!drawAppIcon(box, a.icon, std::string(), rad * 0.6f, float(opacity)) &&
            !drawAppIcon(box, a.name, std::string(), rad * 0.6f, float(opacity))) {
            drawAppTile(box, a.name, rad * 0.6f, tileTint(a.name), false);
        }
        const std::string label = ellipsize(text, a.name, 15, std::max(40, r.w - 48));
        drawTextCentered(label, 15, Weight::Medium, theme::kText,
                         Rect{r.x, box.bottom() + 14, r.w, 24});
    }
}

void Manager::drawDesktop() {
    comp.drawWallpaper();
    // Tablet mode replaces the desktop grid with the iOS home screen, and takes the
    // widget cards off the screen with it.
    if (tabletAnim < 0.999) drawDesktopIcons();
    if (tabletAnim > 0.001) drawTabletHome();
    drawWidgets();
    // The dock picker is modal, so it goes over the home screen rather than under
    // it.
    if (tabletDockPickerOpen) drawTabletDockPickerView();
    if (tabletMenu) drawTabletMenuView();
}

void Manager::drawTextAt(const std::string& s, int px, Weight w, const Color& c, int x, int y) {
    if (s.empty()) return;
    const TextTex t = text.get(s, px, w);
    if (!t.tex) return;
    comp.drawText(t, Rect{x, y, t.w, t.h}, c, 1.0f);
}

void Manager::drawTextCentered(const std::string& s, int px, Weight w, const Color& c,
                               const Rect& area) {
    if (s.empty()) return;
    const TextTex t = text.get(s, px, w);
    if (!t.tex) return;
    comp.drawText(t, Rect{area.x + (area.w - t.w) / 2, area.y + (area.h - t.h) / 2, t.w, t.h}, c,
                  1.0f);
}

void Manager::drawTextRight(const std::string& s, int px, Weight w, const Color& c, int right,
                            int y) {
    if (s.empty()) return;
    const TextTex t = text.get(s, px, w);
    if (!t.tex) return;
    comp.drawText(t, Rect{right - t.w, y, t.w, t.h}, c, 1.0f);
}

// drawTextCentered() against a chosen Text instance, for content that is set in
// a face other than the UI font (the digital clock's display characters).
void Manager::drawTextCenteredIn(Text& t, const std::string& s, int px, Weight w, const Color& c,
                                 const Rect& area) {
    if (s.empty()) return;
    const TextTex tx = t.get(s, px, w);
    if (!tx.tex) return;
    comp.drawText(tx, Rect{area.x + (area.w - tx.w) / 2, area.y + (area.h - tx.h) / 2, tx.w, tx.h},
                  c, 1.0f);
}

int Manager::measureIn(Text& t, const std::string& s, int px, Weight w, int* outH) {
    if (s.empty()) {
        if (outH) *outH = 0;
        return 0;
    }
    return t.measure(s, px, w, outH);
}

Text& Manager::displayText() { return clockText.ready() ? clockText : text; }

// The Windows 11 Start logo. The bundled PNG (assets/icons/2048.png) is asked
// for by name so it wins over the vector source (2048.svg), which librsvg would
// otherwise prefer: the raster is the artwork that should show. The "2048"
// fallback keeps the SVG working when the PNG is absent, and four squares in a
// 2x2 grid are the last resort.
void Manager::drawStartGlyph(const Rect& box, const Color& c) {
    const Rect icon{box.x, box.y + (box.h - box.w) / 2, box.w, box.w};
    if (icon.w >= 8 &&
        drawAppIcon(icon, assetDir + "/icons/2048.png", "2048", 0.f, c.a))
        return;
    const int s = 8, gap = 2;
    const int x0 = box.x + (box.w - (2 * s + gap)) / 2;
    const int y0 = box.y + (box.h - (2 * s + gap)) / 2;
    comp.drawRect(Rect{x0, y0, s, s}, 1.5f, c);
    comp.drawRect(Rect{x0 + s + gap, y0, s, s}, 1.5f, c);
    comp.drawRect(Rect{x0, y0 + s + gap, s, s}, 1.5f, c);
    comp.drawRect(Rect{x0 + s + gap, y0 + s + gap, s, s}, 1.5f, c);
}

// A magnifier built from a ring of pixels plus a handle: no icon font needed
// and it stays crisp at 14px. Reversal's own `edit-find` glyph is preferred
// when the theme has been fetched.
void Manager::drawSearchGlyph(const Rect& box, const Color& c) {
    if (drawAppIcon(box, "edit-find", "edit-find", 2.f, c.a)) return;
    const double cx = box.x + box.w * 0.5 - 2.0;
    const double cy = box.y + box.h * 0.5 - 2.0;
    const double r = box.w * 0.30;
    for (int i = 0; i < 16; ++i) {
        const double a = i * (2.0 * M_PI / 16.0);
        comp.drawRect(Rect{int(cx + std::cos(a) * r), int(cy + std::sin(a) * r), 1, 1}, 0.f, c);
    }
    for (int i = 0; i < 4; ++i) {
        comp.drawRect(Rect{int(cx + r * 0.75) + i, int(cy + r * 0.75) + i, 1, 1}, 0.f, c);
    }
}

// A stand-in app icon: a coloured tile with the first letter, the same trick
// Windows uses for apps that ship without a usable icon.
void Manager::drawAppTile(const Rect& r, const std::string& name, float radius, const Color& tint,
                          bool hovered) {
    comp.drawRect(r, radius, tint, hovered ? 1.0f : 0.92f);
    const std::string letter = name.empty() ? "?" : name.substr(0, 1);
    const TextTex t = text.get(letter, int(r.h * 0.55), Weight::Bold);
    if (t.tex) {
        comp.drawText(t, Rect{r.x + (r.w - t.w) / 2, r.y + (r.h - t.h) / 2, t.w, t.h},
                      Color{1.f, 1.f, 1.f, 0.95f}, 1.0f);
    }
}

// Themed icon for a launcher, sized into `r`. Hatter's colourful app art is the
// default; the Control Centre asks for Shell (Reversal) so its monochrome glyphs
// stay put. Returns false when the theme has nothing for this entry so the caller
// can draw the letter tile instead.
bool Manager::drawAppIcon(const Rect& r, const std::string& iconName,
                          const std::string& wmClass, float radius, float opacity,
                          IconTheme theme) {
    // Rasterise at the size the icon is actually drawn, so an SVG source is
    // sampled 1:1 rather than downscaled from a fixed resolution.
    const IconTex t = icons.forApp(iconName, wmClass, std::max(r.w, r.h), theme);
    if (!t.valid()) return false;
    // Icons are square in every icon theme worth the name; keep the aspect
    // ratio anyway so a non-square icon is never stretched.
    int w = r.w, h = r.h;
    if (t.w > 0 && t.h > 0 && t.w != t.h) {
        if (t.w > t.h) {
            w = r.h * t.w / t.h;
            if (w > r.w) {
                h = r.w * t.h / t.w;
                w = r.w;
            }
        } else {
            h = r.w * t.h / t.w;
            if (h > r.h) {
                w = r.h * t.w / t.h;
                h = r.h;
            }
        }
    }
    const Rect dst{r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, w, h};
    comp.drawTex(t.tex, dst, radius, Color{1.f, 1.f, 1.f, 1.f}, opacity, true, true);
    return true;
}

// The window's own icon if it published one, otherwise a letter tile.
void Manager::drawWindowThumb(const WindowTex& tex, const Rect& dst, float radius, bool focused) {
    if (tex.valid()) {
        comp.drawTex(tex.tex, dst, radius, Color{1.f, 1.f, 1.f, 1.f}, 1.0f, true, tex.alpha);
        return;
    }
    comp.drawRect(dst, radius, focused ? theme::kItemActive : theme::kItemHover);
}

// One managed window: the Fluent frame plus the caption contents we draw
// ourselves (icon and centred title). All animation state is applied here so
// the X geometry stays a single source of truth.
void Manager::drawClientSprite(Client* c) {
    Rect f = c->drawFrame;
    if (f.empty()) return;

    double opacity = 1.0;
    double scale = 1.0;
    double genie = 0.0;
    Rect genieIcon{};
    // Corner radius override for the tablet zoom, or <0 to use the usual one.
    float radiusOverride = -1.f;

    // iOS 26 zoom transition -- tablet mode only. A tablet window is a rect that
    // morphs between the home-screen icon it was launched from and the full app
    // frame, its corners easing from the icon squircle (24% of its edge) to the
    // window radius as it grows. Opening decelerates out of the icon; closing
    // and the swipe-up home gesture accelerate back into it and fade, so the
    // icon underneath is revealed exactly where it left. This deliberately
    // bypasses the Fluent pop and the magic-lamp warp used on the desktop.
    const bool tabletZoom = c->tabletApp && c->tabletFromValid &&
                            (c->appear < 1.0 || c->vanish > 0.0 || c->minFade > 0.0);
    if (tabletZoom) {
        const bool opening = c->appear < 1.0;
        double p;
        if (opening) p = clamp01(c->appear);
        else if (c->closing && c->vanish > 0.0) p = clamp01(c->vanish);
        else p = clamp01(c->minFade);
        // 0 = collapsed into the icon, 1 = settled into the app frame.
        const double t = opening ? easeOutCubic(p) : 1.0 - p * p * p;
        f = lerpRect(c->tabletFrom, f, t);
        const float iconR = float(c->tabletFrom.w) * metrics::kTabletIconRadius;
        radiusOverride = iconR + (float(metrics::kRadius) - iconR) * float(t);
        opacity = opening ? 1.0 : t;
    } else {
        if (c->appear < 1.0) {
            // Open: grow the last few percent while fading in (Fluent motion). A
            // window born from a launch placeholder keeps its size so it is exactly
            // the same size as the tile it is cross-fading with -- only the opacity
            // does the transition.
            const double t = fluentEase(c->appear);
            opacity *= 0.25 + 0.75 * t;
            scale = c->fromLaunch ? 1.0 : 0.93 + 0.07 * t;
        }
        // Magic lamp: while minimising (or restoring) the GPU warps the content
        // into its taskbar icon, so the frame geometry is left alone and only the
        // opacity fades -- late, so the funnel stays visible on the way in.
        if (c->minFade > 0.0) {
            const double m = c->minFade;
            genie = m;
            // The funnel lands on the taskbar button's icon, whatever size that is.
            const int icon = metrics::taskIconSize();
            genieIcon = Rect{screenW / 2 - icon / 2, screenH - metrics::taskbarH, icon, icon};
            for (const TaskItem& it : taskItems) {
                if (it.client != c) continue;
                genieIcon = Rect{it.rect.x + (it.rect.w - icon) / 2,
                                 it.rect.y + (it.rect.h - icon) / 2, icon, icon};
                break;
            }
            opacity *= 1.0 - m * m;
        }
        if (c->vanish > 0.0) {
            opacity *= 1.0 - c->vanish;
            scale *= 1.0 - 0.06 * c->vanish;
        }
    }
    if (opacity <= 0.01) return;
    if (scale < 1.0) {
        const int dw = int(std::lround(f.w * (1.0 - scale)));
        const int dh = int(std::lround(f.h * (1.0 - scale)));
        f = Rect{f.x + dw / 2, f.y + dh / 2, f.w - dw, f.h - dh};
    }
    if (f.w < 8 || f.h < 8) return;

    WindowSprite s;
    s.tex = c->tex;
    s.frame = f;
    s.captionH = c->captionH;
    s.maximized = c->maximizedH && c->maximizedV;
    s.radius = radiusOverride >= 0.f ? radiusOverride
               : ((c->fullscreen || s.maximized) ? 0.f : float(metrics::kRadius));
    s.focused = (c == focused) && !c->closing;
    s.opacity = float(opacity);
    s.attention = float(c->attentionPulse) * 0.85f;
    s.minHover = float(c->hoverFade[0]);
    s.maxHover = float(c->hoverFade[1]);
    s.closeHover = float(c->hoverFade[2]);
    s.minPress = c->pressBtn == 0 ? 1.f : 0.f;
    s.maxPress = c->pressBtn == 1 ? 1.f : 0.f;
    s.closePress = c->pressBtn == 2 ? 1.f : 0.f;
    s.genie = float(genie);
    s.genieIcon = genieIcon;
    comp.drawWindow(s);

    // The warp carries the content; the caption chrome would only float in place.
    if (genie > 0.0) return;

    if (c->captionH <= 0 || f.w < 160) return;
    const float a = float(opacity);
    int textLeft = f.x + 12;
    if (c->iconTex && c->iconW > 0) {
        const Rect icon{f.x + 12, f.y + (c->captionH - 16) / 2, 16, 16};
        comp.drawTex(c->iconTex, icon, 3.f, Color{1.f, 1.f, 1.f, 1.f}, a, true, true);
        textLeft = icon.right() + 8;
    } else if (drawAppIcon(Rect{f.x + 12, f.y + (c->captionH - 16) / 2, 16, 16}, c->appName,
                           c->appName, 3.f, a)) {
        textLeft = f.x + 12 + 16 + 8;
    }
    // Windows 11 centres the title in the space between the icon and the
    // caption buttons, and ellipsizes rather than running into them.
    const int buttonsLeft = f.right() - 3 * metrics::kBtnW;
    const int avail = buttonsLeft - textLeft - 16;
    if (avail <= 24) return;
    const int px = 13;
    const Color col = s.focused ? theme::kText : theme::kTextIdle;
    const TextTex t = text.get(ellipsize(text, c->title, px, avail), px, Weight::Regular);
    if (!t.tex) return;
    const int centerY = f.y + c->captionH / 2;
    int tx = textLeft;
    if (t.w + 24 < avail) tx = textLeft + (avail - t.w) / 2;
    comp.drawText(t, Rect{tx, centerY - t.h / 2, t.w, t.h}, col, a);
}

void Manager::layoutTaskbar() {
    taskItems.clear();
    const int y = screenH - metrics::taskbarH;
    const int top = y + (metrics::taskbarH - metrics::taskButtonH()) / 2;
    const int gap = 0;
    const int w = metrics::taskButtonW();
    const int h = metrics::taskButtonH();

    std::vector<Client*> visible;
    for (auto& cp : clients) {
        Client* c = cp.get();
        // Deliberately *not* filtered on c->mapped: a minimised window has been
        // unmapped, but it must keep its taskbar button or there is no way to
        // bring it back.
        if (!c->managed || !c->alive || c->isDock || c->isDesktop || c->skipTaskbar) continue;
        if (c->closing) continue;
        visible.push_back(c);
    }
    const int count = int(visible.size());
    const int buttons = count + int(pinned.size());
    const int total = buttons * w + (buttons > 1 ? (buttons - 1) * gap : 0);
    // Windows 11 centres the Start button together with the whole app group.
    int x = (screenW - (total + w + gap)) / 2;
    if (x < 8) x = 8;
    startButtonRect = Rect{x, top, w, h};
    x += w + gap;
    const int rightLimit = screenW - 150;  // the clock cluster owns the right side
    // Pinned launchers come first, in the order they were pinned: the buttons
    // the user arranged stay where they put them while the window buttons after
    // them grow and shrink with what is running.
    for (size_t p = 0; p < pinned.size(); ++p) {
        if (x + w > rightLimit) break;
        TaskItem item;
        item.rect = Rect{x, top, w, h};
        item.pin = int(p);
        taskItems.push_back(item);
        x += w + gap;
    }
    for (Client* c : visible) {
        if (x + w > rightLimit) break;
        TaskItem item;
        item.rect = Rect{x, top, w, h};
        item.client = c;
        taskItems.push_back(item);
        x += w + gap;
    }
    // The bold ring button lives at the left of the bar, indented off the edge and
    // clear of the centred app group. It is decorative for now, so it is sized a
    // touch under an app icon and centred in the bar's thickness rather than in a
    // button cell.
    const int d = std::max(8, metrics::taskIconSize() * 4 / 5);
    const int indent = 16;
    circleButtonRect = Rect{indent, y + (metrics::taskbarH - d) / 2, d, d};

    clockRect = Rect{screenW - 140, y, 116, metrics::taskbarH};
    showDesktopRect = Rect{screenW - 14, y, 14, metrics::taskbarH};
}

// The on-screen box of a taskbar button's icon: the static cell slid by the dock's
// spread and grown upward by the zoom, unioned with the cell itself so the target
// never slips out from under the pointer while the row swells (Plank unions the
// item's draw region into its hover region for the same reason).
Rect Manager::taskHitRect(size_t index) const {
    if (index >= taskItems.size()) return Rect{};
    const Rect cell = taskItems[index].rect;
    const double scale = index < taskScale.size() ? taskScale[index] : 1.0;
    const int shift = index < taskShift.size() ? taskShift[index] : 0;
    const int side = std::max(4, int(std::lround(metrics::taskIconSize() * scale)));
    const int cx = cell.x + cell.w / 2 + shift;
    const int cy = cell.y + cell.h / 2;
    const Rect glyph{cx - side / 2, cy - side / 2, side, side};
    return cell.unionWith(glyph);
}

void Manager::drawTaskbar() {
    const int y = screenH - metrics::taskbarH;
    const Rect bar{0, y, screenW, metrics::taskbarH};

    // A soft shadow above the bar, so the acrylic reads as a pane floating over
    // the desktop rather than paint stuck to the bottom edge. Stacked one pixel
    // rows with a quadratic falloff are enough to read as a blurred shadow, so the
    // shader stays untouched.
    constexpr int kShadowRows = 14;
    for (int i = 1; i <= kShadowRows; ++i) {
        const float t = float(i) / float(kShadowRows);
        const float a = 0.34f * (1.0f - t) * (1.0f - t);
        comp.drawRect(Rect{0, y - i, screenW, 1}, 0.f, theme::kShadow, a);
    }

    // The Windows 11 web clone's taskbar background: a translucent theme fill over
    // a strongly saturated, blurred backdrop. The compositor samples the
    // pre-blurred wallpaper, so tinting the blur by `tintAmount` at full surface
    // alpha reproduces `rgba(tint, opacity)` over `backdrop-filter` exactly, and
    // the desktop behind the bar no longer shows through sharp.
    comp.drawAcrylic(bar, 0.f, theme::kTaskbarTint, theme::kTaskbarTintOpacity,
                     theme::kShellLine, 1.0f, theme::kTaskbarSaturate);
    comp.drawRect(Rect{0, y, screenW, 1}, 0.f, theme::kShellBorder);

    // Glass sheen: the first few rows fade from the bright hairline down into the
    // acrylic, so the surface reads as lit from above instead of evenly tinted.
    // Light mode takes a much weaker sheen, or it just washes the pale bar out.
    {
        const float sheen = theme::isLight() ? 0.30f : 1.0f;
        for (int i = 1; i <= 5; ++i) {
            const float a = 0.05f * sheen * (1.0f - float(i) / 6.0f);
            comp.drawRect(Rect{0, y + i, screenW, 1}, 0.f, Color{1.f, 1.f, 1.f, 1.f}, a);
        }
    }

    // The bold ring button at the left end of the bar: white on the dark bar, black
    // on the light one, straight from applyMode(), so it tracks the mode like
    // everything else. Draw a solid disc, then redraw the taskbar's own acrylic
    // over the middle to punch the hole. Both passes are opaque, so the inner
    // acrylic reproduces the surface it is sitting on exactly, leaving only the
    // ring -- no ring primitive needed. It has no action yet; its hover already
    // glows, and a press would live beside Start's.
    if (circleButtonRect.w > 0) {
        const float rad = float(circleButtonRect.w) * 0.5f;
        const int thick = std::max(4, circleButtonRect.w / 5);  // bold stroke
        // Hover: a soft white halo behind the ring, the same stacked rounded
        // rects the app buttons use, so the button reads as lit the moment the
        // pointer reaches it. It eases in and out with circleHoverAnim.
        const Color white{1.f, 1.f, 1.f, 1.f};
        const int ccx = circleButtonRect.x + circleButtonRect.w / 2;
        const int ccy = circleButtonRect.y + circleButtonRect.h / 2;
        if (circleHoverAnim > 0.001) {
            constexpr int kGlowLayers = 5;
            for (int k = kGlowLayers; k >= 1; --k) {
                const float t = float(k) / float(kGlowLayers);
                const float a = 0.14f * float(circleHoverAnim) * (1.0f - t) * (1.0f - t);
                const int spread = int(std::lround(1.0 + 11.0 * t));
                // Clipped to the bar's top edge, so the halo never washes over
                // the desktop above the taskbar.
                comp.drawRect(circleButtonRect.inflated(spread), rad + float(spread), white, a,
                              y);
            }
        }
        // Motes: a ring of tiny white particles that drift outward from the rim
        // and fade as they go, so the glow feels charged rather than painted. No
        // state is stored -- each mote's position comes from the clock and its
        // own index -- and the whole ring is scaled by the hover amount, so it
        // appears with the glow and disappears with it. The golden ratio spreads
        // the phases so the motes never settle into a visible pattern.
        if (circleHoverAnim > 0.001) {
            const double tt = nowMs() / 1000.0;
            constexpr int kMotes = 12;
            constexpr double kGolden = 0.6180339887498949;
            for (int i = 0; i < kMotes; ++i) {
                const double seed = double(i) * kGolden;
                const double period = 1.5 + 1.2 * seed;                 // 1.5 .. 2.7 s
                const double phase = std::fmod(tt / period + seed, 1.0);
                // Distance grows from just outside the rim outward.
                const double orbit = double(rad) + 1.0 + phase * (double(rad) * 1.6 + 9.0);
                const double ang = double(i) * (2.0 * M_PI / double(kMotes)) +
                                   tt * (0.7 + 0.6 * seed);
                const int mx = ccx + int(std::lround(std::cos(ang) * orbit));
                const int my = ccy + int(std::lround(std::sin(ang) * orbit));
                const float fade = float(std::sin(phase * M_PI));        // 0..1..0
                const int size = std::max(2, int(std::lround(double(rad) * 0.24 *
                                                             (1.0 - 0.45 * phase))));
                const float a = 0.9f * fade * float(circleHoverAnim);
                if (a <= 0.01f) continue;
                // `y` is the taskbar's top edge: every mote is clipped to the
                // bar, so a mote never drifts up onto the desktop.
                comp.drawRect(Rect{mx - size / 2, my - size / 2, size, size},
                              float(size) * 0.5f, white, a, y);
                // A fainter, larger copy underneath reads as the mote's own glow.
                comp.drawRect(Rect{mx - size, my - size, size * 2, size * 2},
                              float(size), white, a * 0.25f, y);
            }
        }
        comp.drawRect(circleButtonRect, rad, theme::kCircleRing);
        // The ring itself picks up the white a little as it lights, so the glow
        // is not only a halo around an unchanged circle.
        if (circleHoverAnim > 0.001)
            comp.drawRect(circleButtonRect, rad, white,
                          float(circleHoverAnim) * 0.30f);
        comp.drawAcrylic(circleButtonRect.inflated(-thick),
                         std::max(0.f, rad - float(thick)), theme::kTaskbarTint,
                         theme::kTaskbarTintOpacity, theme::kShellLine, 1.0f,
                         theme::kTaskbarSaturate);
    }

    // A soft accent bloom behind a button. Concentric rounded rects of the same
    // hue at falling alpha stack into a halo that reads as a blur without needing
    // a blur pass: the inner layers overlap and accumulate, the outer ones thin
    // out to nothing.
    const auto bloom = [&](const Rect& r, double amount, float radius) {
        if (amount <= 0.01) return;
        constexpr int kLayers = 4;
        for (int k = 0; k < kLayers; ++k) {
            const float a = 0.08f * float(amount) * (1.0f - float(k) / float(kLayers));
            comp.drawRect(r.inflated(2 + k), radius + float(k), theme::kTaskGlow, a);
        }
    };

    // Windows 11 lifts and slightly enlarges the icon under the cursor rather than
    // only washing the button, and that motion is most of what makes the bar feel
    // alive. A focused app keeps a little of the lift so it stays findable.
    const auto lifted = [](double hv) { return fluentEase(hv); };

    {
        const double g = lifted(startHoverAnim);
        // Sized like every app icon, not the whole button: a launcher drawn at
        // button width (44px) read as a bigger, louder neighbour than the 32px app
        // icons beside it. The magnification field scales it, so the launcher is
        // simply item 0 of the row.
        const int baseBox = std::max(8, int(std::lround(metrics::taskIconSize())));
        const int box = std::max(8, int(std::lround(baseBox * startScale)));
        const int up = (box - baseBox) / 2;
        const int cx = startButtonRect.x + startButtonRect.w / 2 + startShift;
        const int cy = startButtonRect.y + startButtonRect.h / 2;
        const Rect icon{cx - box / 2, cy - box / 2 - int(std::lround(g)) - up, box, box};
        drawStartGlyph(icon, theme::kGlyph);
    }

    for (size_t i = 0; i < taskItems.size(); ++i) {
        const TaskItem& it = taskItems[i];
        Client* w = it.client;
        const double hv = i < taskHover.size() ? taskHover[i] : 0.0;
        const double g = lifted(hv);
        // A button just added eases in; a clicked one pops. Both are read here so
        // the plate, the icon and the pill all animate as one object.
        const double appear = i < taskAppear.size() ? taskAppear[i] : 1.0;
        const double press = i < taskPress.size() ? taskPress[i] : 0.0;
        // The dock's slide for this button, shared by its icon, plate and pill so
        // they move as one object.
        const int shift = i < taskShift.size() ? taskShift[i] : 0;

        // A pin lights up when the window it stands for is the focused one; a
        // window button lights up when it is focused itself.
        bool active = false;
        bool running = false;
        if (it.pin >= 0 && it.pin < int(pinned.size())) {
            const AppEntry& app = pinned[size_t(it.pin)];
            if (Client* win = clientForPinned(app)) {
                running = true;
                active = (win == focused) && !win->minimized;
            }
            // The carried button grows by a couple of pixels and keeps a stronger
            // wash while it is lifted, so the eye can follow it across the bar.
            const bool liftedPin = pinDragMoved && it.pin == pinDrag;
            // The hovered icon grows, and the focused one keeps a fraction of the
            // growth so the active app is readable at a glance.
            const double mag = i < taskScale.size() ? taskScale[i] : 1.0;
            // Hover is only a vertical rise now; the size comes from the
            // magnification field, so the two no longer stack into one big zoom.
            const double restGrow = (active ? 1.05 : 1.0) *
                                    (0.82 + 0.18 * appear) * (1.0 - 0.07 * press);
            const int restSide =
                std::max(4, int(std::lround(metrics::taskIconSize() * restGrow)));
            int side = std::max(4, int(std::lround(restSide * mag)));
            // The dock grows an icon upward out of the bar rather than about its
            // middle, so a magnified glyph reads as rising off the shelf.
            const int up = (side - restSide) / 2;
            Rect area{it.rect.x + (it.rect.w - side) / 2 + shift,
                      it.rect.y + (it.rect.h - side) / 2 - int(std::lround(g)) - up, side, side};
            if (liftedPin) {
                // Carried: eases up out of the bar, so the eye can follow it across.
                side = side + int(std::lround(pinDragLift * (metrics::taskIconDragSize() -
                                                             side)));
                area = Rect{it.rect.x + (it.rect.w - side) / 2 + shift,
                            it.rect.y + (it.rect.h - side) / 2 - int(std::lround(pinDragLift * 2)),
                            side, side};
            }
            // Only the focused app gets a backplate; hovering leaves the bar bare
            // and lets the enlarged icon do the talking.
            bloom(it.rect.moved(shift, 0), (active ? 0.45 : 0.0) * appear, 4.f);
            if (active) {
                comp.drawRect(Rect{it.rect.x + 2 + shift, it.rect.y + 2, it.rect.w - 4,
                                   it.rect.h - 4},
                              4.f, theme::kTaskActivePlate, float(appear));
            }
            if (!drawAppIcon(area, app.icon, app.wmClass, 5.f, float(appear))) {
                drawAppTile(area, app.name, 5.f, tileTint(app.name), false);
            }
        } else {
            if (!w) continue;
            active = (w == focused) && !w->minimized;
            running = true;
            const double mag = i < taskScale.size() ? taskScale[i] : 1.0;
            const double restGrow = (active ? 1.05 : 1.0) *
                                    (0.82 + 0.18 * appear) * (1.0 - 0.07 * press);
            const int restSide =
                std::max(4, int(std::lround(metrics::taskIconSize() * restGrow)));
            const int side = std::max(4, int(std::lround(restSide * mag)));
            const int up = (side - restSide) / 2;
            const Rect iconArea{it.rect.x + (it.rect.w - side) / 2 + shift,
                                it.rect.y + (it.rect.h - side) / 2 - int(std::lround(g)) - up,
                                side, side};
            // The same accent bloom as a pinned button, then the active plate. No
            // hover wash: the icon's own growth is the whole hover response.
            bloom(it.rect.moved(shift, 0), (active ? 0.45 : 0.0) * appear, 4.f);
            if (active) {
                comp.drawRect(Rect{it.rect.x + 2 + shift, it.rect.y + 2, it.rect.w - 4,
                                   it.rect.h - 4},
                              4.f, theme::kTaskActivePlate, float(appear));
            }
            if (w->iconTex && w->iconW > 0) {
                comp.drawTex(w->iconTex, iconArea, 4.f, Color{1.f, 1.f, 1.f, 1.f},
                             float(appear), true, true);
            } else if (!drawAppIcon(iconArea, w->appName, w->appName, 5.f, float(appear))) {
                drawAppTile(iconArea, w->title, 5.f, tileTint(w->title), false);
            }
        }

        // The running/active pill on the bottom edge of the button. Its width is
        // animated (see tickFluidMotion), so it breathes between the short "running
        // but not focused" bar and the long focused one. A pinned launcher that is
        // not running shows no pill at all, the way Windows 11 does.
        const double pw = i < taskPill.size() ? taskPill[i] : 0.0;
        if (pw > 0.01 && appear > 0.01) {
            const int wide = 18, narrow = 7;
            const int iw = std::max(2, int(std::lround(narrow + (wide - narrow) * pw)));
            const int ph = active ? 4 : 3;
            const Color ic = active ? theme::kAccent
                                    : mixColor(theme::kTextDim, theme::kTextMuted, float(g));
            const Rect pill{it.rect.x + (it.rect.w - iw) / 2 + shift,
                            bar.bottom() - 6 - int(std::lround(press * 1.5)), iw, ph};
            // A short, faint copy underneath is enough to read as a glow, which is
            // what makes the focused app's indicator look lit rather than painted.
            if (active) comp.drawRect(pill.inflated(2), float(ph) * 0.5f + 2.f, ic, 0.22f);
            comp.drawRect(pill, float(ph) * 0.5f, ic, float(appear));
        }
        (void)running;
    }

    time_t now = time(nullptr);
    struct tm lt {};
    localtime_r(&now, &lt);
    char hhmm[16] = {0};
    char dateStr[24] = {0};
    strftime(hhmm, sizeof hhmm, "%H:%M", &lt);
    strftime(dateStr, sizeof dateStr, "%d/%m/%Y", &lt);
    const int right = clockRect.right();
    // The clock is a two-line stack, centred in the bar rather than hung off fixed
    // offsets, so it stays in the middle when the bar is dragged thicker or thinner.
    // 29 is the stack's own height: 13px time, 5px gap, 11px date.
    const int top = y + (metrics::taskbarH - 29) / 2;
    // The whole cluster takes a hover wash, the way the tray does in Windows 11, so
    // the click target looks like it exists before it is pressed.
    if (clockHoverAnim > 0.001) {
        comp.drawRect(Rect{clockRect.x + 2, y + 4, clockRect.w - 4, metrics::taskbarH - 8},
                      4.f, theme::kItemHover, float(clockHoverAnim));
    }
    drawTextRight(hhmm, 13, Weight::Regular, theme::kText, right, top);
    drawTextRight(dateStr, 11, Weight::Regular, theme::kTextMuted, right, top + 18);

    if (showDesktopHoverAnim > 0.001)
        comp.drawRect(showDesktopRect, 2.f, theme::kItemHover, float(showDesktopHoverAnim));
    comp.drawRect(Rect{screenW - 3, y + 6, 2, metrics::taskbarH - 12}, 1.f, theme::kShellBorder);
}

void Manager::drawSnapPreview() {
    if (snapZonePreview == kSnapNone || snapPreviewAnim <= 0.001) return;
    const Rect inner = snapGeometry(snapZonePreview).inflated(4);
    if (inner.empty()) return;
    // The preview eases in as the zone arms and fades out once it is released.
    const float a = float(fluentEase(snapPreviewAnim));
    comp.drawRect(inner, 8.f, theme::kSnapFill, a);
    // One pixel accent outline: four thin rects, no ring primitive needed.
    const Color b = theme::kSnapBorder;
    comp.drawRect(Rect{inner.x, inner.y, inner.w, 1}, 1.f, b, a);
    comp.drawRect(Rect{inner.x, inner.bottom() - 1, inner.w, 1}, 1.f, b, a);
    comp.drawRect(Rect{inner.x, inner.y, 1, inner.h}, 1.f, b, a);
    comp.drawRect(Rect{inner.right() - 1, inner.y, 1, inner.h}, 1.f, b, a);
}

void Manager::layoutStartMenu() {
    appFiltered.clear();
    appRects.clear();
    appDotRects.clear();
    startPageBase = 0;
    startPageSize = 0;
    for (size_t i = 0; i < apps.size(); ++i) {
        if (!searchText.empty() && !containsFold(apps[i].searchKey, searchText)) continue;
        appFiltered.push_back(i);
    }

    // The launcher owns everything above the taskbar; the taskbar stays put so
    // its Start button can still toggle it.
    const int taskbarTop = screenH - metrics::taskbarH;
    startRect = Rect{0, 0, screenW, taskbarTop};

    // Top-centred search field, like the macOS Web launchpad: a rounded pill
    // sitting in a ~100px band, with the query centred until the user starts
    // typing.
    const int sw = std::min(260, screenW - 48);
    const int sh = 34;
    const int sy = std::clamp(screenH / 22, 18, 40);
    searchRect = Rect{(screenW - sw) / 2, sy, sw, sh};

    // A plain four-column grid, the way the macOS Web launchpad lays its apps
    // out: 90% of the screen wide, 25% per cell, big icons with a name below.
    const int cols = metrics::kLaunchCols;
    const int cellW = std::max(1, screenW * 90 / 100 / cols);
    const int icon = std::clamp(std::min(cellW - 32, screenH / 8), metrics::kLaunchIconMin,
                                metrics::kLaunchIcon);
    const int cellH = icon + metrics::kLaunchLabelH + metrics::kLaunchRowGap;

    const int gridTop = searchRect.bottom() + 18;
    const int gridBottom = taskbarTop - metrics::kLaunchDotsH;
    // Four rows to a page, the macOS Web grid; a short screen takes fewer.
    const int rows = std::clamp((gridBottom - gridTop) / cellH, 1, 4);
    startPageSize = size_t(cols) * size_t(rows);

    const size_t total = appFiltered.size();
    startPageCount = std::max(1, int((total + startPageSize - 1) / startPageSize));
    if (startPage >= startPageCount) startPage = startPageCount - 1;
    if (startPage < 0) startPage = 0;
    startPageBase = size_t(startPage) * startPageSize;

    // Centre the grid block; a short last page stays top-aligned inside it,
    // exactly like Launchpad.
    const int gridW = cols * cellW;
    const int gridH = rows * cellH;
    const int gx = (screenW - gridW) / 2;
    const int gy = gridTop + ((gridBottom - gridTop) - gridH) / 2;
    // Kept so drawStartMenu can lay out any page while the field slides, not just
    // the page this layout produced rects for.
    launchCols = cols;
    launchCellW = cellW;
    launchCellH = cellH;
    launchGridX = gx;
    launchGridTop = gy;
    const size_t base = startPageBase;
    const size_t onPage = total > base ? std::min(startPageSize, total - base) : 0;
    for (size_t i = 0; i < onPage; ++i) {
        const int cx = int(i) % cols;
        const int cy = int(i) / cols;
        appRects.push_back(Rect{gx + cx * cellW, gy + cy * cellH, cellW, cellH});
    }

    // Page dots, only with more than one page and while not searching (a search
    // is always a single short list).
    if (startPageCount > 1 && searchText.empty()) {
        const int gap = 18;
        const int span = (startPageCount - 1) * gap;
        const int dx = (screenW - span) / 2;
        const int dy = taskbarTop - metrics::kLaunchDotsH / 2 - 2;
        for (int p = 0; p < startPageCount; ++p) {
            appDotRects.push_back(Rect{dx + p * gap - 7, dy - 7, 14, 14});
        }
    }
}

void Manager::drawStartMenu() {
    layoutStartMenu();
    // Master progress, straight from the spring. macOS Web plays the whole
    // launchpad in with a single transition: everything fades from 0 to 1 while
    // scaling from 1.2 down to 1, so every element is drawn scaled about the
    // centre of the field rather than growing out of its own source.
    const double f = clamp01(startAnim);
    if (f <= 0.001) return;
    const float a = float(f);
    const double scale = 1.0 + 0.2 * (1.0 - f);
    const double ccx = screenW * 0.5;
    const double ccy = startRect.h * 0.5;
    const auto sx = [&](int v) { return int(std::lround(ccx + (v - ccx) * scale)); };
    const auto sy = [&](int v) { return int(std::lround(ccy + (v - ccy) * scale)); };
    const auto scaled = [&](const Rect& r) {
        return Rect{sx(r.x), sy(r.y), int(std::lround(r.w * scale)),
                    int(std::lround(r.h * scale))};
    };

    // Backdrop: the macOS Web launchpad effect verbatim -- `backdrop-filter:
    // blur(25px)` over a fully transparent surface. The acrylic runs at zero
    // tint, no border and saturation 1, so all that is left is the wallpaper
    // blurred behind the grid with nothing dimming it down.
    comp.drawAcrylic(startRect, 0.f, theme::kLaunchTint, 0.0f, Color{0.f, 0.f, 0.f, 0.f}, a,
                     1.0f);

    // Search field: a small bordered pill. The placeholder is centred until the
    // user types, then the query is left-aligned after the magnifier, exactly as
    // the CSS `input:focus { text-align: left }` does.
    const Rect search = scaled(searchRect);
    // A fully rounded pill: the hairline is painted as a filled pill and the wash
    // is laid back over it inset by the stroke, so the corners stay perfectly
    // round instead of the four straight edges of the old box.
    const float pillR = search.h * 0.5f;
    const float stroke = std::max(1.f, std::round(search.h * 0.09f));
    comp.drawRect(search, pillR, theme::kLaunchSearchBorder, a);
    comp.drawRect(search.inflated(-int(std::lround(stroke))), pillR - stroke,
                  theme::kLaunchSearch, a);
    const int glyph = 15;
    const int padX = 10;
    drawSearchGlyph(Rect{search.x + padX, search.y + (search.h - glyph) / 2, glyph, glyph},
                    theme::kLaunchSearchText);
    {
        const std::string label = searchText.empty() ? std::string("Search") : searchText;
        const Color col = searchText.empty() ? theme::kLaunchSearchText : theme::kLaunchLabel;
        const int maxW = search.w - 2 * padX - glyph - 10;
        const TextTex t = text.get(ellipsize(text, label, 13, maxW), 13, Weight::Regular);
        if (t.tex) {
            const int tx = searchText.empty() ? search.x + (search.w - t.w) / 2
                                              : search.x + padX + glyph + 8;
            comp.drawText(t, Rect{tx, search.y + (search.h - t.h) / 2, t.w, t.h}, col, a);
        }
    }

    // The app grid. It is drawn from launchPageOffset rather than straight from
    // startPage so that a swipe drags the field with the pointer and then settles:
    // while a page is moving, the one leaving and the one arriving are both on
    // screen, sliding together, exactly as the tablet home screen turns a page.
    const int stride = std::max(1, launchCellW * launchCols);
    const double off = launchPageOffset;
    const int basePage = int(std::floor(off));
    const double frac = off - double(basePage);
    const int whole = int(std::lround(-frac * stride));
    const auto drawPage = [&](int page, int dx) {
        if (page < 0 || page >= startPageCount) return;
        const size_t base = size_t(page) * startPageSize;
        const size_t total = appFiltered.size();
        const size_t onPage = total > base ? std::min(startPageSize, total - base) : 0;
        for (size_t i = 0; i < onPage; ++i) {
            const int cx = int(i) % launchCols;
            const int cy = int(i) / launchCols;
            const Rect raw{launchGridX + cx * launchCellW, launchGridTop + cy * launchCellH,
                           launchCellW, launchCellH};
            const AppEntry& e = apps[appFiltered[base + i]];
            const int icon = std::clamp(std::min(raw.w - 32, screenH / 8),
                                        metrics::kLaunchIconMin, metrics::kLaunchIcon);
            const Rect box = scaled(Rect{raw.x + (raw.w - icon) / 2, raw.y, icon, icon});

            const double hav = i < appHover.size() ? appHover[i] : 0.0;
            if (hav > 0.001) {
                const int pad = std::max(4, box.w / 8);
                comp.drawRect(Rect{box.x - pad + dx, box.y - pad, box.w + 2 * pad, box.h + 2 * pad},
                              float(box.w) * 0.30f, theme::kLaunchHover, a * float(hav));
            }
            const Rect ibox{box.x + dx, box.y, box.w, box.h};
            if (!drawAppIcon(ibox, e.icon, e.wmClass, float(ibox.w) * 0.24f, a)) {
                drawAppTile(ibox, e.name, float(ibox.w) * 0.24f, tileTint(e.name), false);
            }
            const TextTex t =
                text.get(ellipsize(text, e.name, 15, raw.w - 12), 15, Weight::Regular);
            if (!t.tex) continue;
            const Rect label = scaled(
                Rect{raw.x + (raw.w - t.w) / 2, raw.y + icon + 6, t.w, t.h});
            // A one pixel shadow keeps the white label legible over a light patch.
            comp.drawText(t, Rect{label.x + dx + 1, label.y + 1, label.w, label.h},
                          theme::kLaunchLabelShadow, a);
            comp.drawText(t, Rect{label.x + dx, label.y, label.w, label.h},
                          theme::kLaunchLabel, a);
        }
    };
    if (launchSwipe || std::abs(frac) > 0.001) {
        // The field is one connected strip: the page under the finger follows
        // it exactly, and the next page always sits one stride further on, so
        // the two sets of icons keep a constant separation while they slide --
        // a rigid turn, which is what makes the release glide into the next
        // page without any re-passing. (An arriving offset of (1-frac)*stride,
        // as here, walks the incoming set through the middle of the field and
        // out the other side of a half-drag, so the two pages crossed.)
        drawPage(basePage, whole);
        drawPage(frac > 0.0 ? basePage + 1 : basePage - 1, whole + stride);
    } else {
        drawPage(basePage, 0);
    }

    if (appFiltered.empty()) {
        drawTextCentered("No apps match \u201c" + searchText + "\u201d", 15, Weight::Regular,
                         theme::kLaunchSearchText,
                         Rect{0, searchRect.bottom() + 40, screenW, 40});
    }

    // Page dots: the current dot and any hovered dot grow and brighten smoothly.
    for (size_t p = 0; p < appDotRects.size(); ++p) {
        const bool active = int(p) == startPage;
        const double dv = p < dotHover.size() ? dotHover[p] : 0.0;
        const int r = std::max(1, int(std::lround(active ? 4.0 : lerp(3.0, 4.0, dv))));
        const Rect& d = appDotRects[p];
        const int cx = sx(d.x + d.w / 2), cy = sy(d.y + d.h / 2);
        const Color col =
            mixColor(theme::kLaunchDot, theme::kLaunchDotActive, active ? 1.f : float(dv));
        comp.drawRect(Rect{cx - r, cy - r, 2 * r, 2 * r}, float(r), col, a);
    }
}

void Manager::drawContextMenu() {
    // A ring menu with no rows at all (no recents) still shows its greeting.
    if (contextItems.empty() && !contextRing) return;
    const double eased = fluentEase(contextAnim);
    if (eased <= 0.001) return;
    const float a = float(eased);
    const int itemH = 32;
    const int pad = contextRing ? 8 : 6;
    // The ring menu pops out of the ring button exactly the way Control Centre pops
    // out of the clock: a 0.86 -> 1.0 zoom about the button under the pointer, with
    // the whole panel fading in as it grows, reversed cleanly on dismissal. Rects
    // scale and glyphs keep their size, which is what drawControlCenter does too.
    // The plain context flyout keeps its own drop-and-settle, so its zoom is 1.0
    // and grow() is the identity.
    const float zoom = contextRing ? 0.86f + 0.14f * float(eased) : 1.0f;
    const float ax = float(circleButtonRect.x + circleButtonRect.w / 2);
    const float ay = float(circleButtonRect.y + circleButtonRect.h / 2);
    const auto grow = [&](const Rect& r) {
        if (zoom >= 0.999f) return r;
        const float cx = float(r.x + r.w / 2), cy = float(r.y + r.h / 2);
        const float w = float(r.w) * zoom, h = float(r.h) * zoom;
        return Rect{int(std::lround(cx - w / 2 + (ax - cx) * (1.f - zoom))),
                    int(std::lround(cy - h / 2 + (ay - cy) * (1.f - zoom))),
                    int(std::lround(w)), int(std::lround(h))};
    };
    // A hairline must stay one pixel tall however far the panel is zoomed out, so
    // it rides grow() for its position and width only.
    const auto growHairline = [&](const Rect& r) {
        const Rect g = grow(r);
        return Rect{g.x, g.y, g.w, 1};
    };
    // The flyout drops the last few pixels into place as it fades in, the way
    // Windows 11 menus do, and reverses cleanly when it is dismissed. The ring menu
    // zooms instead of sliding, so it takes no dy.
    const int dy = contextRing ? 0 : int(std::lround(-8.0 * (1.0 - eased)));
    // The ring menu's height is already settled by layoutRingMenu (it depends on
    // how many rows the recent grid needs), so the panel reads it back rather
    // than recomputing it from the item list.
    const Rect panel = grow(Rect{contextRect.x, contextRect.y + dy, contextRect.w,
                                 contextRing ? contextRect.h
                                             : int(contextItems.size()) * itemH + 2 * pad});
    if (contextRing) {
        // The ring menu wears the taskbar's frosted surface -- the same tint,
        // tint amount, saturate() and hairline -- so the two read as one
        // material. A soft shadow lifts it off the desktop. The corner rides the
        // zoom, the way drawControlCenter scales its panel radius.
        const float radius = float(metrics::kFlyoutRadius) * zoom;
        for (int i = 8; i >= 1; --i) {
            const float t = float(i) / 8.0f;
            comp.drawRect(panel.inflated(i), radius + float(i), theme::kShadow,
                          0.22f * a * (1.0f - t) * (1.0f - t));
        }
        comp.drawAcrylic(panel, radius, theme::kTaskbarTint, theme::kTaskbarTintOpacity,
                         theme::kShellLine, a, theme::kTaskbarSaturate);
    } else {
        comp.drawAcrylic(panel, float(metrics::kFlyoutRadius), theme::kFlyoutTint, 0.90f,
                         theme::kShellBorder, a);
    }

    int top = panel.y + pad;
    if (contextRing) {
        // The greeting reads the clock at draw time, so it is right for however
        // long the shell has been up, not just for the moment it started. Medium,
        // not Bold: the bundled font has no real bold face and the synthesized
        // one smears at small sizes, which is what made this text look blurred.
        time_t now = time(nullptr);
        struct tm lt {};
        localtime_r(&now, &lt);
        const int hour = lt.tm_hour;
        const char* greet = hour < 5    ? "Good night"
                            : hour < 12 ? "Good morning"
                            : hour < 17 ? "Good afternoon"
                            : hour < 22 ? "Good evening"
                                        : "Good night";
        drawTextAt(ellipsize(text, greet, 18, panel.w - 32), 18, Weight::Medium, theme::kText,
                   panel.x + 16, panel.y + 14);
        char dateStr[48] = {0};
        strftime(dateStr, sizeof dateStr, "%A, %d %B", &lt);
        drawTextAt(ellipsize(text, dateStr, 12, panel.w - 32), 12, Weight::Regular,
                   theme::kTextMuted, panel.x + 16, panel.y + 44);
        // A hairline separates the greeting from the list, so the rows read as a
        // separate group rather than as part of the header.
        comp.drawRect(Rect{panel.x + 10, panel.y + metrics::kRingHeaderH - 1, panel.w - 20, 1},
                      0.f, theme::kShellBorder, a * 0.8f);
        top = panel.y + metrics::kRingHeaderH;
    }

    if (contextRing) {
        // Recent apps are a grid of bare icons -- the same squares the taskbar and
        // Launchpad use -- rather than a list of names, so the menu is scanned at a
        // glance. Every cell was placed by layoutRingMenu. When there are no
        // recents yet the empty grid gets its own short note.
        if (contextRecents.empty()) {
            drawTextCentered("No recent apps", 13, Weight::Regular, theme::kTextDim,
                             Rect{panel.x, top, panel.w, ringGridH});
        }
        for (size_t i = 0; i < contextRecents.size() && i < ringRecentRects.size(); ++i) {
const Rect cell = grow(ringRecentRects[i]);
            const double hv = i < ctxHover.size() ? ctxHover[i] : 0.0;
            if (hv > 0.001)
                comp.drawRect(cell.inflated(-10), 12.f, theme::kItemHover, float(hv) * a);
            const AppEntry& app = contextRecents[i];
            const int icon = metrics::kRingIcon;
            const Rect ibox{cell.x + (cell.w - icon) / 2, cell.y + (cell.h - icon) / 2,
                            icon, icon};
            if (!drawAppIcon(ibox, app.icon, app.wmClass, float(icon) * 0.22f, float(a)))
                drawAppTile(ibox, app.name, float(icon) * 0.22f, tileTint(app.name), false);
        }

        // A hairline separates the grid from the power row, so the two read as
        // distinct groups the way the greeting does from the grid.
        if (!ringPowerRects.empty()) {
            // contextRect, not the grown panel: grow() needs the unscaled span, and
            // for the ring the two differ by exactly that zoom.
            comp.drawRect(growHairline(Rect{contextRect.x + 10, ringPowerRects[0].y - 5,
                                            contextRect.w - 20, 1}),
                          0.f, theme::kShellBorder, a * 0.8f);
        }

        // The power row, in the order runRingPower() switches on. The glyphs are
        // Lucide, the same set the Control Centre is drawn against, so the two
        // surfaces agree about what "sleep" and "restart" look like.
        static const char* const kPowerGlyph[metrics::kRingPowerCount] = {
            "lucide-moon", "lucide-log-out", "lucide-rotate-cw", "lucide-power"};
        static const char* const kPowerLabel[metrics::kRingPowerCount] = {
            "Sleep", "Log out", "Restart", "Power"};
        for (size_t i = 0; i < ringPowerRects.size() && i < metrics::kRingPowerCount; ++i) {
const Rect cell = grow(ringPowerRects[i]);
            const size_t idx = contextRecents.size() + i;
            const double hv = idx < ctxHover.size() ? ctxHover[idx] : 0.0;
            if (hv > 0.001)
                comp.drawRect(cell.inflated(-4), 8.f, theme::kItemHover, float(hv) * a);
            const int glyph = std::max(16, metrics::kRingIcon - 22);
            const Rect ibox{cell.x + (cell.w - glyph) / 2, cell.y + 9, glyph, glyph};
            if (!drawAppIcon(ibox, kPowerGlyph[i], kPowerGlyph[i], float(glyph) * 0.22f,
                             float(a), IconTheme::Shell))
                drawAppTile(ibox, kPowerLabel[i], float(glyph) * 0.22f,
                            tileTint(kPowerLabel[i]), false);
            drawTextCentered(kPowerLabel[i], 12, Weight::Regular,
                             mixColor(theme::kTextMuted, theme::kText, float(hv)),
                             Rect{cell.x, ibox.bottom() + 3, cell.w, 15});
        }
        return;
    }

    for (size_t i = 0; i < contextItems.size(); ++i) {
        const Rect item{panel.x + pad, top + int(i) * itemH, panel.w - 2 * pad, itemH};
        const double hv = i < ctxHover.size() ? ctxHover[i] : 0.0;
        if (hv > 0.001) comp.drawRect(item, 4.f, theme::kItemHover, float(hv) * a);
        const Color col = mixColor(theme::kTextIdle, theme::kText, float(hv));
        drawTextAt(contextItems[i], 13, Weight::Regular, col, item.x + 14,
                   item.y + (item.h - 18) / 2);
    }
}

void Manager::drawStats() {
    const Rect box{12, 12, 340, 128};
    comp.drawRect(box, 6.f, theme::kFlyoutTint, 0.93f);
    comp.drawRect(box, 6.f, theme::kShellBorder, 1.0f);
    char line[220];
    snprintf(line, sizeof line, "%d fps   frame %.2f ms   vsync %s (interval %d)",
             comp.sampledFps(), comp.lastFrameSeconds() * 1000.0,
             comp.vsyncActive() ? "on" : "off", comp.swapInterval());
    drawTextAt(line, 12, Weight::Regular, theme::kText, box.x + 12, box.y + 10);
    if (comp.hasTimerQuery()) {
        snprintf(line, sizeof line, "cpu %.2f ms   gpu %.2f ms",
                 comp.cpuFrameMs(), comp.gpuFrameMs());
    } else {
        snprintf(line, sizeof line, "cpu %.2f ms   gpu n/a", comp.cpuFrameMs());
    }
    drawTextAt(line, 11, Weight::Regular, theme::kTextMuted, box.x + 12, box.y + 32);
    snprintf(line, sizeof line, "%ld draw call(s)/frame   %s", comp.drawCallsLastFrame(),
             comp.rendererName().c_str());
    drawTextAt(line, 11, Weight::Regular, theme::kTextMuted, box.x + 12, box.y + 52);
    snprintf(line, sizeof line, "draws fill=%ld acrylic=%ld win=%ld tex=%ld grad=%ld arc=%ld",
             comp.drawsByMode()[0], comp.drawsByMode()[1], comp.drawsByMode()[2],
             comp.drawsByMode()[3], comp.drawsByMode()[4], comp.drawsByMode()[5]);
    drawTextAt(line, 11, Weight::Regular, theme::kTextMuted, box.x + 12, box.y + 72);
    snprintf(line, sizeof line, "%zu window(s) | texture_from_pixmap %s | font %s",
             clients.size(), comp.hasTextureFromPixmap() ? "yes" : "MISSING", text.family().c_str());
    drawTextAt(line, 11, Weight::Regular, theme::kTextMuted, box.x + 12, box.y + 92);
    snprintf(line, sizeof line, "drag: %s   focus: %s", dragClient ? (dragIsMove ? "move" : "resize") : "idle",
             focused ? focused->title.c_str() : "none");
    drawTextAt(line, 11, Weight::Regular, theme::kTextMuted, box.x + 12, box.y + 112);
}

// The Alt-Tab switcher: a row of live window thumbnails with the selection
// centred, exactly like Windows 11.
void Manager::drawAltTab() {
    const double eased = fluentEase(altTabAnim);
    if (eased <= 0.001) return;
    const float a = float(eased);

    // The list can change while the switcher is open (a window may die).
    altTabOrder.erase(std::remove_if(altTabOrder.begin(), altTabOrder.end(),
                                     [this](Client* c) { return find(c->id) != c; }),
                      altTabOrder.end());
    const int n = int(altTabOrder.size());
    if (n <= 0) return;
    if (altTabIndex >= n) altTabIndex = n - 1;
    if (altTabIndex < 0) altTabIndex = 0;

    const int shown = std::min(n, 5);
    const int cardW = 200, cardH = 148, gap = 10;
    const int totalW = shown * cardW + (shown - 1) * gap;
    const int x0 = (screenW - totalW) / 2;
    const int y0 = (screenH - cardH) / 2 - 24;

    comp.drawRect(Rect{0, 0, screenW, screenH}, 0.f, theme::kDesktopBlur, 0.55f * a);

    int start = clampi(altTabIndex - shown / 2, 0, n - shown);
    for (int i = 0; i < shown; ++i) {
        const int idx = start + i;
        Client* c = altTabOrder[size_t(idx)];
        const Rect card{x0 + i * (cardW + gap), y0, cardW, cardH};
        const bool selected = idx == altTabIndex;
        comp.drawAcrylic(card, 8.f, theme::kCardTint, 0.86f,
                         selected ? theme::kAccentRing : theme::kShellBorder, a);
        const Rect thumb{card.x + 8, card.y + 8, cardW - 16, cardH - 44};
        if (c->tex.valid()) {
            comp.drawTex(c->tex.tex, thumb, 4.f, Color{1.f, 1.f, 1.f, 1.f}, a, true, c->tex.alpha);
        } else {
            drawAppTile(thumb, c->title, 4.f, tileTint(c->title), false);
        }
        drawTextCentered(ellipsize(text, c->title, 12, cardW - 24), 12, Weight::Regular,
                         selected ? theme::kText : theme::kTextIdle,
                         Rect{card.x + 10, card.y + cardH - 30, cardW - 20, 22});
    }
    drawTextCentered("Release Alt to switch", 12, Weight::Regular, theme::kTextMuted,
                     Rect{0, y0 + cardH + 16, screenW, 20});
}

// Task View (Super+Tab): a grid of live thumbnails of every open window.
void Manager::drawTaskView() {
    const double eased = fluentEase(taskViewAnim);
    if (eased <= 0.001) return;
    const float a = float(eased);
    comp.drawRect(Rect{0, 0, screenW, screenH}, 0.f, theme::kDesktopBlur, 0.62f * a);

    altTabOrder.clear();
    if (focused) altTabOrder.push_back(focused);
    for (size_t i = clients.size(); i-- > 0;) {
        Client* c = clients[i].get();
        if (!c->managed || !c->alive || c->isDock || c->isDesktop || c->skipTaskbar) continue;
        if (c == focused) continue;
        altTabOrder.push_back(c);
    }
    taskViewRects.clear();
    if (altTabOrder.empty()) {
        drawTextCentered("No open windows", 16, Weight::Regular, theme::kTextMuted,
                         Rect{0, 0, screenW, screenH});
        return;
    }

    const int n = int(altTabOrder.size());
    const int cols = n <= 1 ? 1 : (n <= 4 ? 2 : 3);
    const int rows = (n + cols - 1) / cols;
    const int gap = 24;
    const int marginX = 56, marginTop = 64, marginBottom = metrics::taskbarH + 44;
    const int availW = screenW - 2 * marginX;
    const int availH = screenH - marginTop - marginBottom;
    const int cellW = std::max(80, (availW - (cols - 1) * gap) / cols);
    const int cellH = std::max(70, (availH - (rows - 1) * gap) / rows);

    for (int i = 0; i < n; ++i) {
        const int cx = i % cols, cy = i / cols;
        const Rect cell{marginX + cx * (cellW + gap), marginTop + cy * (cellH + gap), cellW,
                        cellH};
        Client* c = altTabOrder[size_t(i)];
        taskViewRects.push_back(cell);
        const bool hovered = taskViewHover == i;
        comp.drawAcrylic(cell, 8.f, theme::kCardTint, hovered ? 0.80f : 0.87f,
                         hovered ? theme::kAccentRing : theme::kShellBorder, a);
        const Rect thumb{cell.x + 8, cell.y + 8, cell.w - 16, cell.h - 44};
        if (c->tex.valid()) {
            comp.drawTex(c->tex.tex, thumb, 4.f, Color{1.f, 1.f, 1.f, 1.f}, a, true, c->tex.alpha);
        } else {
            drawAppTile(thumb, c->title, 4.f, tileTint(c->title), false);
        }
        drawTextCentered(ellipsize(text, c->title, 13, cell.w - 28), 13, Weight::Regular,
                         hovered ? theme::kText : theme::kTextIdle,
                         Rect{cell.x + 14, cell.y + cell.h - 34, cell.w - 28, 26});
    }
    drawTextCentered("Task view", 20, Weight::Medium, theme::kText, Rect{0, 18, screenW, 34});
}

}  // namespace wm