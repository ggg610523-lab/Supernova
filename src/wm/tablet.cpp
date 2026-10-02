// Tablet / mobile mode: an iOS-like home screen for the compositor.
//
// This is a third shell skin, next to the Windows 11 desktop and the macOS
// Launchpad. Entering it minimises the desktop windows so the home screen shows;
// from there a tapped app opens *inside* the mode, filling the band between the
// status bar and the home indicator. The app is inset rather than edge to edge
// on purpose: our overlay is the bottom-most window, so leaving those two strips
// free is what keeps the status bar (Control Centre) and the home indicator
// (back to the grid) tappable while an app is up.
//
// The layout is the real iPhone one: a 52px status bar with the clock on the
// left and the date on the right, a grid of squircle app icons with labels, a
// heavily translucent glass dock, and the iPhone X home indicator along the
// bottom edge.
//
// An "Edit" pill puts the home screen into rearrangement mode: the icons jiggle
// and can be dragged from cell to cell, and the wallpaper widgets are moved and
// resized exactly as they are on the desktop.
//
// Switching modes is not a hard cut: a full-screen splash fades in over the old
// shell, the two are swapped while it completely covers the screen, and it
// fades back out onto the new one. That is what makes the change read as a mode
// switch rather than a redraw.
#include "manager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>

#include "theme.h"

namespace wm {
namespace {

// Same idea as draw.cpp's ellipsize(), kept local so this file does not have to
// reach into another translation unit's anonymous namespace.
std::string tabletFit(Text& text, const std::string& s, int px, int maxW) {
    if (maxW <= 0 || s.empty()) return std::string();
    if (text.measure(s, px) <= maxW) return s;
    std::string out = s;
    while (!out.empty() && text.measure(out + "\u2026", px) > maxW) {
        out.resize(out.size() - 1);
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) {
            out.resize(out.size() - 1);
        }
    }
    return out + "\u2026";
}

// The same hash-coloured letter tile the desktop uses when an entry has no
// resolvable icon.
Color tabletTint(const std::string& name) {
    size_t h = 5381;
    for (unsigned char ch : name) h = h * 33 + ch;
    return theme::kTileTints[h % 6];
}

Color alpha(const Color& c, float a) { return Color{c.r, c.g, c.b, c.a * a}; }

}  // namespace

// ---------------------------------------------------------------------------
// Transition
// ---------------------------------------------------------------------------

// Starts the splash. The actual mode swap happens in tickAnimations() once the
// splash has fully covered the screen, so the user never sees the desktop
// become the home screen.
void Manager::setTabletMode(bool on) {
    if (modeSwitching || tabletMode == on) return;
    modeSwitching = true;
    modeSwapped = false;
    modeSwitchTarget = on;
    modeSwitchStart = nowMs();
    splashOpacity = 0.0;
    dirty = true;
}

// Applies the new mode. Runs while the splash is opaque, so everything here can
// be as abrupt as it likes.
void Manager::applyTabletMode() {
    closeOverlays();
    cancelDrag();
    endTabletIconDrag();
    if (dragWidget >= 0) endWidgetDrag();
    tabletHover = -1;
    if (tabletMode) {
        // Hide the desktop: minimise every visible window so it is neither
        // composited nor able to swallow a click, remembering which ones we
        // hid so the user's own minimised windows stay minimised.
        for (auto& cp : clients) {
            Client* c = cp.get();
            if (!c->managed || c->isDock || c->isDesktop || c->skipTaskbar) continue;
            if (c->closing || !c->alive || c->minimized || !c->mapped) continue;
            c->tabletHidden = true;
            minimizeClient(c, false);
        }
        buildTabletEntries();
        layoutTabletHome();
    } else {
        tabletEdit = false;
        for (auto& cp : clients) {
            Client* c = cp.get();
            // An app opened from the home screen becomes an ordinary window
            // again, decorations and all, wherever the desktop is concerned.
            if (c->tabletApp) endTabletApp(c);
            if (!c->tabletHidden) continue;
            c->tabletHidden = false;
            restoreClient(c);
        }
    }
    dirty = true;
}

// ---------------------------------------------------------------------------
// Entries
// ---------------------------------------------------------------------------

// The home screen and the dock are built once, so rearranging the icons holds
// for the rest of the session instead of being undone by the next layout pass.
void Manager::buildTabletEntries() {
    tabletHome.clear();
    tabletDock.clear();

    // The home screen is fed from the session's Desktop directory, which is the
    // curated list of launchers the user actually has; the full .desktop scan is
    // the fallback when the Desktop is empty.
    for (const DesktopItem& d : desktopItems) {
        TabletEntry e;
        e.name = d.name;
        e.icon = d.icon;
        e.exec = d.exec;
        e.path = d.path;
        tabletHome.push_back(e);
    }
    if (tabletHome.empty()) {
        for (const AppEntry& a : apps) {
            if (tabletHome.size() >= 24) break;
            TabletEntry e;
            e.name = a.name;
            e.icon = a.icon;
            e.wmClass = a.wmClass;
            e.exec = a.exec;
            tabletHome.push_back(e);
        }
    }
    // The dock is a separate snapshot of the first five entries, so moving a
    // home icon never quietly reorders the dock.
    const size_t dockCount = std::min<size_t>(tabletHome.size(), 5);
    tabletDock.assign(tabletHome.begin(), tabletHome.begin() + long(dockCount));
    tabletEntriesBuilt = true;
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void Manager::layoutTabletHome() {
    if (!tabletEntriesBuilt) buildTabletEntries();
    tabletHomeRects.clear();
    tabletDockRects.clear();

    tabletStatusRect = Rect{0, 0, screenW, metrics::kTabletStatusH};
    tabletHomeBarRect = Rect{(screenW - metrics::kTabletHomeBarW) / 2,
                             screenH - 14, metrics::kTabletHomeBarW, metrics::kTabletHomeBarH};
    // The Edit / Done pill sits to the right of the first icon row, clear of it.
    const int pillW = 78, pillH = 30;
    tabletEditRect = Rect{screenW - pillW - std::max(12, screenW / 48),
                          tabletStatusRect.bottom() + 6, pillW, pillH};

    // --- dock: a glass pill of up to five squircles, above the home bar -----
    const int n = int(tabletDock.size());
    const int di = std::clamp(std::min(screenW, screenH) / 11, 48, metrics::kTabletDockIcon);
    tabletDockIconSize = di;
    if (n > 0) {
        const int gap = std::max(14, di / 2);
        const int padX = std::max(16, di / 3);
        const int padY = std::max(12, di / 4);
        const int contentW = n * di + (n - 1) * gap;
        const int dockW = std::min(screenW - 32, contentW + 2 * padX);
        const int dockH = di + 2 * padY;
        const int dockX = (screenW - dockW) / 2;
        const int dockY = screenH - 30 - dockH;
        tabletDockRect = Rect{dockX, dockY, dockW, dockH};
        int x = dockX + (dockW - contentW) / 2;
        for (int i = 0; i < n; ++i) {
            tabletDockRects.push_back(Rect{x, dockY + padY, di, di});
            x += di + gap;
        }
    } else {
        tabletDockRect = Rect{};
    }

    // --- home grid: squircle + label cells, top aligned --------------------
    const int icon = std::clamp(std::min(screenW, screenH) / 11, metrics::kTabletIconMin, 84);
    tabletIconSize = icon;
    const int cellW = icon + icon / 2 + 24;
    const int cellH = icon + 34;
    const int marginX = std::max(18, screenW / 18);
    const int cols = std::max(3, (screenW - 2 * marginX) / cellW);
    const int top = tabletStatusRect.bottom() + 24 + pillH;
    const int bottom = tabletDockRect.empty() ? screenH - 46 : tabletDockRect.y - 18;
    const int rows = std::max(1, (bottom - top) / cellH);
    const size_t per = size_t(cols) * size_t(rows);
    const size_t visible = std::min(tabletHome.size(), per);

    const int gridW = cols * cellW;
    const int gx = (screenW - gridW) / 2;
    for (size_t i = 0; i < visible; ++i) {
        const int col = int(i) % cols;
        const int row = int(i) / cols;
        tabletHomeRects.push_back(Rect{gx + col * cellW, top + row * cellH, cellW, cellH});
    }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

// The device outline used by the Control Centre tile: a rounded phone drawn from
// four hairlines, so no icon asset is needed.
void Manager::drawTabletGlyph(const Rect& box, const Color& c, float opacity) {
    const int w = std::max(10, int(box.w * 0.46f));
    const int h = std::max(14, int(box.h * 0.86f));
    const Rect body{box.x + (box.w - w) / 2, box.y + (box.h - h) / 2, w, h};
    const int t = std::max(1, w / 8);
    const float r = float(t) * 0.5f;
    comp.drawRect(Rect{body.x, body.y, t, body.h}, r, c, opacity);
    comp.drawRect(Rect{body.right() - t, body.y, t, body.h}, r, c, opacity);
    comp.drawRect(Rect{body.x, body.y, body.w, t}, r, c, opacity);
    comp.drawRect(Rect{body.x, body.bottom() - t, body.w, t}, r, c, opacity);
    comp.drawRect(Rect{body.x + body.w / 3, body.bottom() - 2 * t, body.w / 3, t}, r,
                  c, opacity * 0.7f);
}

void Manager::drawTabletHome() {
    if (tabletAnim <= 0.001) return;
    const float a = float(clamp01(easeOutCubic(tabletAnim)));
    // The rect list holds only the cells that fit, so "not laid out yet" is the
    // test -- comparing sizes would re-lay out every frame.
    if ((tabletHomeRects.empty() && !tabletHome.empty()) ||
        (tabletDockRects.empty() && !tabletDock.empty()))
        layoutTabletHome();

    const int icon = tabletIconSize;
    const float radius = float(icon) * 0.24f;
    // The jiggle: a slow sine per icon, out of phase, so edit mode reads at a
    // glance without any rotation primitive.
    const double wiggle = nowMs() / 150.0;

    // Where the dragged icon would land, painted under the icons.
    if (tabletDragIcon >= 0 && tabletDragTarget >= 0 &&
        tabletDragTarget < int(tabletHomeRects.size())) {
        const Rect cell = tabletHomeRects[size_t(tabletDragTarget)].inflated(-6);
        comp.drawRect(cell, radius + 6.f, theme::kTabletIconHover, a * 0.9f);
    }

    // --- home screen icons --------------------------------------------------
    for (size_t i = 0; i < tabletHomeRects.size() && i < tabletHome.size(); ++i) {
        const bool dragging = int(i) == tabletDragIcon;
        const Rect cell = dragging ? tabletDragRect : tabletHomeRects[i];
        int jig = 0;
        if (tabletEdit && !dragging)
            jig = int(std::lround(std::sin(wiggle + double(i) * 0.7) * 3.0));
        const Rect box{cell.x + (cell.w - icon) / 2 + jig, cell.y, icon, icon};
        const double hv = i < tabletIconHover.size() ? tabletIconHover[i] : 0.0;
        if (hv > 0.001 && !dragging)
            comp.drawRect(box.inflated(4), radius + 3.f, theme::kTabletIconHover,
                          a * float(hv));
        if (dragging) comp.drawRect(box.inflated(6), radius + 5.f, theme::kTabletIconHover, a);
        if (!drawAppIcon(box, tabletHome[i].icon, tabletHome[i].wmClass, radius, a)) {
            drawAppTile(box, tabletHome[i].name, radius, tabletTint(tabletHome[i].name),
                        false);
        }
        const TextTex t = text.get(
            tabletFit(text, tabletHome[i].name, 12, cell.w - 8), 12, Weight::Regular);
        if (!t.tex) continue;
        const int lx = cell.x + (cell.w - t.w) / 2 + jig;
        const int ly = box.bottom() + 6;
        comp.drawText(t, Rect{lx + 1, ly + 1, t.w, t.h}, alpha(theme::kTabletLabelShadow, a),
                      1.0f);
        comp.drawText(t, Rect{lx, ly, t.w, t.h}, alpha(theme::kTabletLabel, a), 1.0f);
    }

    // --- dock (hidden while rearranging, like iOS) --------------------------
    if (!tabletEdit && !tabletDockRect.empty()) {
        const float dr = float(std::min(tabletDockRect.h, 44)) * 0.62f;
        comp.drawAcrylic(tabletDockRect, dr, theme::kTabletDockGlass, 0.55f,
                         theme::kTabletDockBorder, a);
        const int dockIcon = tabletDockIconSize;
        const float dockRadius = float(dockIcon) * 0.24f;
        for (size_t i = 0; i < tabletDockRects.size() && i < tabletDock.size(); ++i) {
            const Rect box = tabletDockRects[i];
            const size_t hoverIndex = tabletHome.size() + i;
            const double hv =
                hoverIndex < tabletIconHover.size() ? tabletIconHover[hoverIndex] : 0.0;
            if (hv > 0.001)
                comp.drawRect(box.inflated(4), dockRadius + 3.f, theme::kTabletDockHover,
                              a * float(hv));
            if (!drawAppIcon(box, tabletDock[i].icon, tabletDock[i].wmClass, dockRadius, a)) {
                drawAppTile(box, tabletDock[i].name, dockRadius,
                            tabletTint(tabletDock[i].name), false);
            }
        }
    }

    // --- Edit / Done pill ---------------------------------------------------
    comp.drawAcrylic(tabletEditRect, float(tabletEditRect.h) * 0.5f, theme::kTabletEditPill,
                     0.5f, theme::kTabletEditPillBorder, a);
    drawTextCentered(tabletEdit ? "Done" : "Edit", 13, Weight::Medium,
                     alpha(theme::kTabletLabel, a), tabletEditRect);
}

// The clock/date and the home indicator, painted after the windows so they float
// over an open app exactly the way iOS' status bar and gesture bar do.
void Manager::drawTabletChrome() {
    if (tabletAnim <= 0.001) return;
    const float a = float(clamp01(easeOutCubic(tabletAnim)));

    time_t nowT = time(nullptr);
    struct tm lt {};
    localtime_r(&nowT, &lt);
    char hhmm[16] = {0};
    char dateStr[32] = {0};
    strftime(hhmm, sizeof hhmm, "%H:%M", &lt);
    strftime(dateStr, sizeof dateStr, "%a %d %b", &lt);

    const int sideX = std::max(24, screenW / 16);
    const int ty = tabletStatusRect.y + (tabletStatusRect.h - 20) / 2;
    drawTextAt(hhmm, 16, Weight::Bold, alpha(theme::kTabletStatusText, a), sideX, ty);
    drawTextRight(dateStr, 13, Weight::Regular, alpha(theme::kTabletStatusSub, a),
                  screenW - sideX, ty + 2);

    // The home indicator. While a swipe from the bottom edge is running it rides
    // the finger and stretches, which is how the iPhone X bar behaves as it
    // turns into the app-switcher gesture.
    Rect bar = tabletHomeBarRect;
    float barA = a;
    if (tabletGesture) {
        const int lift = std::max(0, screenH - 14 - tabletGestureCurY);
        bar.y = clampi(tabletGestureCurY - metrics::kTabletHomeBarH / 2,
                       tabletStatusRect.bottom(), screenH - 14);
        const int stretch = bar.w + int(70.0 * clamp01(double(lift) / 220.0));
        bar.w = stretch;
        bar.x = (screenW - stretch) / 2;
        barA *= 0.55f + 0.45f * float(1.0 - clamp01(double(lift) / 300.0));
    }
    // A soft shadow keeps the white bar legible over a light app.
    comp.drawRect(bar.inflated(1), float(metrics::kTabletHomeBarH + 2) * 0.5f,
                  Color{0.f, 0.f, 0.f, 0.28f * barA}, barA);
    comp.drawRect(bar, float(metrics::kTabletHomeBarH) * 0.5f, alpha(theme::kTabletHomeBar, barA));
}

void Manager::drawTabletSplash() {
    if (splashOpacity <= 0.001) return;
    const double p = clamp01((nowMs() - modeSwitchStart) / double(metrics::kTabletSplashMs));
    const float a = float(splashOpacity);
    comp.drawRect(Rect{0, 0, screenW, screenH}, 0.f, theme::kTabletSplash, a);

    const int cx = screenW / 2;
    const int base = std::clamp(std::min(screenW, screenH) / 8, 72, 120);
    const float pop = 0.88f + 0.12f * float(easeOutCubic(clamp01(p / 0.4)));
    const int d = int(std::lround(base * pop));
    const Rect box{cx - d / 2, screenH / 2 - d / 2 - 46, d, d};
    const float radius = float(d) * 0.22f;
    if (!drawAppIcon(box, "2048", "2048", radius, a)) {
        drawAppTile(box, modeSwitchTarget ? "T" : "D", radius, theme::kAccentDeep, false);
    }

    const std::string title = modeSwitchTarget ? "Tablet Mode" : "Desktop Mode";
    const std::string sub = modeSwitchTarget ? "Touch-friendly home screen"
                                             : "Back to the desktop";
    drawTextCentered(title, 22, Weight::Bold, alpha(theme::kTabletSplashLabel, a),
                     Rect{0, box.bottom() + 34, screenW, 32});
    drawTextCentered(sub, 13, Weight::Regular, alpha(theme::kTabletSplashSub, a),
                     Rect{0, box.bottom() + 66, screenW, 22});

    const int tw = 190;
    const int th = 4;
    const Rect track{cx - tw / 2, box.bottom() + 108, tw, th};
    comp.drawRect(track, th * 0.5f, alpha(theme::kTabletSplashTrack, a));
    const int fill = int(tw * clamp01(p));
    if (fill > 0)
        comp.drawRect(Rect{track.x, track.y, fill, th}, th * 0.5f,
                      alpha(theme::kTabletSplashFill, a));
}

// ---------------------------------------------------------------------------
// Tablet apps
// ---------------------------------------------------------------------------

// An app opened from the home screen keeps the mode and fills the display
// between the status bar and the home indicator. Leaving those strips free is
// what keeps them tappable: our overlay sits below the client window, so pixels
// the app covers would send the click to the app instead of to us.
void Manager::makeTabletApp(Client* c) {
    if (!c || !c->managed || c->isDock || c->isDesktop) return;
    c->tabletApp = true;
    c->tabletHidden = false;
    c->captionH = 0;
    const int top = metrics::kTabletStatusH;
    const int h = std::max(metrics::kMinH, screenH - top - metrics::kTabletHomeBarZone);
    c->frame = Rect{0, top, screenW, h};
    c->drawFrame = c->frame;
    c->animFrom = c->frame;
    c->animStart = nowMs();
    c->animMs = 0;
    syncClientGeometry(c);
    c->needsRepaint = true;
    dirty = true;
}

// Back to an ordinary decorated window for the desktop.
void Manager::endTabletApp(Client* c) {
    if (!c || !c->tabletApp) return;
    c->tabletApp = false;
    c->captionH = c->frameless ? 0 : metrics::kCaptionH;
    const Rect wa = workArea();
    Rect want = c->restore;
    if (want.w < metrics::kMinW || want.h < metrics::kMinH) {
        want.w = std::max(metrics::kMinW, screenW * 3 / 5);
        want.h = std::max(metrics::kMinH, screenH * 3 / 5);
        want.x = (screenW - want.w) / 2;
        want.y = (screenH - want.h) / 2;
    }
    c->frame = clampRect(want, wa);
    c->drawFrame = c->frame;
    c->animMs = 0;
    syncClientGeometry(c);
    updateStateAtoms(c);
    dirty = true;
}

// Swipe-up equivalent: everything open collapses back to the grid.
void Manager::tabletGoHome() {
    for (auto& cp : clients) {
        Client* c = cp.get();
        if (!c->managed || c->isDock || c->isDesktop || c->skipTaskbar) continue;
        if (c->closing || !c->alive || c->minimized || !c->mapped) continue;
        minimizeClient(c, true);
    }
    tabletEdit = false;
    dirty = true;
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------

void Manager::openTabletEntry(const TabletEntry& e) {
    if (!e.exec.empty()) {
        launchApp(e.exec);
    } else if (!e.path.empty()) {
        std::string quoted = "'";
        for (char ch : e.path) {
            if (ch == '\'') quoted += "'\\''";
            else quoted += ch;
        }
        quoted += "'";
        launchApp("xdg-open " + quoted);
    }
    dirty = true;
}

bool Manager::handleTabletPress(int x, int y, unsigned button, Time time) {
    if (button == Button4 || button == Button5) return true;  // no scroll surfaces here
    // The switcher owns the pointer while it is up.
    if (tabletSwitcher) return handleTabletSwitcherPress(x, y, button);
    // Control Centre is the one desktop flyout that stays live in tablet mode.
    if (ccOpen) {
        handleControlCenterPress(x, y, button);
        return true;
    }
    if (overlayOpen()) {
        handleOverlayPress(x, y, button, time);
        return true;
    }
    if (button != Button1) return true;  // swallow right/middle clicks

    // Widgets stay usable -- and rearrangeable -- exactly as on the desktop.
    if (handleWidgetPress(x, y, time)) return true;

    if (tabletEditRect.contains(x, y)) {
        tabletEdit = !tabletEdit;
        tabletHover = -1;
        dirty = true;
        return true;
    }
    if (tabletEdit) {
        handleTabletIconPress(x, y);  // drag, or nothing at all
        return true;
    }

    for (size_t i = 0; i < tabletDockRects.size() && i < tabletDock.size(); ++i) {
        if (!tabletDockRects[i].contains(x, y)) continue;
        openTabletEntry(tabletDock[i]);
        return true;
    }
    for (size_t i = 0; i < tabletHomeRects.size() && i < tabletHome.size(); ++i) {
        if (!tabletHomeRects[i].contains(x, y)) continue;
        openTabletEntry(tabletHome[i]);
        return true;
    }
    // The status bar is the Control Centre affordance: there is no taskbar clock
    // to click while the home screen is up.
    if (tabletStatusRect.contains(x, y)) {
        toggleControlCenter();
        return true;
    }
    // The bottom strip is the iPhone X home bar: a tap goes home, but the real
    // gesture is a swipe, so hand it to the gesture tracker.
    if (y >= screenH - metrics::kTabletHomeBarZone) return handleTabletGesturePress(x, y);
    return true;
}

// ---------------------------------------------------------------------------
// The iPhone X home-bar gesture
// ---------------------------------------------------------------------------

// Every app the home screen has opened, bottom of the stack first.
std::vector<Client*> Manager::tabletAppList() const {
    std::vector<Client*> list;
    for (const auto& cp : clients) {
        Client* c = cp.get();
        if (!c->tabletApp || !c->alive || c->closing) continue;
        if (c->isDock || c->isDesktop) continue;
        list.push_back(c);
    }
    return list;
}

bool Manager::handleTabletGesturePress(int x, int y) {
    tabletGesture = true;
    tabletGestureSwipe = false;
    tabletGestureStartX = tabletGestureCurX = x;
    tabletGestureStartY = tabletGestureCurY = y;
    tabletGestureLastMove = nowMs();
    grabPointer();
    dirty = true;
    return true;
}

void Manager::updateTabletGesture(int x, int y) {
    if (!tabletGesture) return;
    const int dx = x - tabletGestureStartX;
    const int dy = tabletGestureStartY - y;  // upwards is positive
    tabletGestureCurX = x;
    tabletGestureCurY = y;
    if (std::abs(dx) > 4 || std::abs(dy) > 4) tabletGestureLastMove = nowMs();
    // A mostly-sideways drag along the bar steps between apps (Apple: "switch
    // between apps -- swipe left or right on home bar").
    if (std::abs(dx) > std::abs(dy) && std::abs(dx) > 30) {
        tabletGestureSwipe = true;
    } else if (std::abs(dy) > std::abs(dx)) {
        tabletGestureSwipe = false;
    }
    // A clear overshoot opens the switcher immediately; a pause further up is
    // picked up by the dwell check in tickAnimations().
    if (!tabletGestureSwipe && dy > 230 && !tabletSwitcher) openTabletSwitcher();
    dirty = true;
}

void Manager::endTabletGesture() {
    if (!tabletGesture) return;
    const int dx = tabletGestureCurX - tabletGestureStartX;
    const int dy = tabletGestureStartY - tabletGestureCurY;
    tabletGesture = false;
    ungrabPointer();
    if (tabletSwitcher) {  // the dwell already opened it
        dirty = true;
        return;
    }
    if (tabletGestureSwipe && std::abs(dx) > 60) {
        switchTabletApp(dx > 0 ? -1 : 1);  // drag right reveals the app on the left
    } else if (dy > 30) {
        tabletGoHome();
    }
    dirty = true;
}

void Manager::switchTabletApp(int dir) {
    std::vector<Client*> list = tabletAppList();
    const int n = int(list.size());
    if (n <= 0) return;
    int idx = 0;
    for (int i = 0; i < n; ++i) {
        if (list[size_t(i)] == focused) idx = i;
    }
    const int next = ((idx + dir) % n + n) % n;
    Client* target = list[size_t(next)];
    restoreClient(target);
    focusClient(target, true);
    dirty = true;
}

// ---------------------------------------------------------------------------
// App switcher
// ---------------------------------------------------------------------------

void Manager::openTabletSwitcher() {
    if (tabletSwitcher) return;
    std::vector<Client*> list = tabletAppList();
    if (list.empty()) return;  // nothing open: the bar gesture just goes home
    tabletSwitcher = true;
    tabletSwitcherAnim = 0.0;
    tabletSwitchOrder = std::move(list);
    tabletSwitchDrag = -1;
    tabletHover = -1;
    layoutTabletSwitcher();
    // Hold the pointer while the switcher is up: the grab delivers every press
    // to us, so a card can never be swallowed by the app behind it.
    grabPointer();
    dirty = true;
}

void Manager::closeTabletSwitcher() {
    if (!tabletSwitcher && tabletSwitchOrder.empty()) return;
    tabletSwitcher = false;
    tabletSwitchDrag = -1;
    tabletSwitchTravel = 0;
    tabletSwitchOrder.clear();
    tabletSwitchRects.clear();
    ungrabPointer();  // now that the flag is down, this really releases it
    dirty = true;
}

// The cards fan out of the focused one, overlapping like the real switcher.
void Manager::layoutTabletSwitcher() {
    tabletSwitchRects.clear();
    const int n = int(tabletSwitchOrder.size());
    if (n <= 0) return;
    int focus = 0;
    for (int i = 0; i < n; ++i) {
        if (tabletSwitchOrder[size_t(i)] == focused) focus = i;
    }
    const int cardW = std::clamp(screenW * 46 / 100, 220, 620);
    const int cardH = std::clamp(screenH * 58 / 100, 200, 700);
    const int step = cardW * 62 / 100;
    const int cy = std::max(metrics::kTabletStatusH + 16, (screenH - cardH) / 2 - 16);
    for (int i = 0; i < n; ++i) {
        const int cx = screenW / 2 + (i - focus) * step;
        tabletSwitchRects.push_back(Rect{cx - cardW / 2, cy, cardW, cardH});
    }
}

void Manager::drawTabletSwitcher() {
    if (!tabletSwitcher && tabletSwitcherAnim <= 0.001) return;
    const float a = float(clamp01(easeOutCubic(tabletSwitcherAnim)));
    comp.drawRect(Rect{0, 0, screenW, screenH}, 0.f, theme::kTabletSplash, 0.72f * a);

    for (size_t i = 0; i < tabletSwitchRects.size() && i < tabletSwitchOrder.size(); ++i) {
        Client* c = tabletSwitchOrder[i];
        if (!c) continue;
        Rect card = tabletSwitchRects[i];
        if (int(i) == tabletSwitchDrag) card = tabletSwitchDragRect;
        const bool hot = c == focused;
        comp.drawAcrylic(card, 30.f, theme::kCcTile, 0.86f,
                         hot ? theme::kAccentRing : theme::kShellBorder, a);
        const Rect thumb{card.x + 10, card.y + 10, card.w - 20, card.h - 54};
        if (c->tex.valid()) {
            comp.drawTex(c->tex.tex, thumb, 22.f, Color{1.f, 1.f, 1.f, 1.f}, a, true,
                         c->tex.alpha);
        } else {
            drawAppTile(thumb, c->title, 22.f, tabletTint(c->title), false);
        }
        const std::string label = tabletFit(text, c->title, 14, card.w - 32);
        drawTextCentered(label, 14, Weight::Medium, alpha(theme::kText, a),
                         Rect{card.x + 16, card.bottom() - 42, card.w - 32, 30});
    }
    if (tabletSwitchOrder.empty())
        drawTextCentered("No open apps", 15, Weight::Regular,
                         alpha(theme::kTabletSplashSub, a),
                         Rect{0, 0, screenW, screenH});
}

bool Manager::handleTabletSwitcherPress(int x, int y, unsigned button) {
    if (button != Button1) {  // right/middle click dismisses, like a tap outside
        closeTabletSwitcher();
        tabletGoHome();
        return true;
    }
    // The home bar still works: swipe/tap it to dismiss and go home.
    if (y >= screenH - metrics::kTabletHomeBarZone) {
        closeTabletSwitcher();
        tabletGoHome();
        return true;
    }
    // Topmost card under the pointer wins (the fan overlaps by design).
    for (size_t i = tabletSwitchRects.size(); i-- > 0;) {
        if (!tabletSwitchRects[i].inflated(10).contains(x, y)) continue;
        tabletSwitchDrag = int(i);
        tabletSwitchFromY = y;
        tabletSwitchTravel = 0;
        tabletSwitchDragRect = tabletSwitchRects[i];
        grabPointer();
        dirty = true;
        return true;
    }
    closeTabletSwitcher();
    tabletGoHome();
    return true;
}

void Manager::updateTabletSwitchDrag(int x, int y) {
    (void)x;
    if (tabletSwitchDrag < 0 || tabletSwitchDrag >= int(tabletSwitchRects.size())) return;
    tabletSwitchTravel = std::max(0, tabletSwitchFromY - y);
    tabletSwitchDragRect = tabletSwitchRects[size_t(tabletSwitchDrag)];
    tabletSwitchDragRect.y -= tabletSwitchTravel;
    dirty = true;
}

void Manager::endTabletSwitchDrag() {
    if (tabletSwitchDrag < 0) return;
    const int index = tabletSwitchDrag;
    const int travel = tabletSwitchTravel;
    tabletSwitchDrag = -1;
    tabletSwitchTravel = 0;
    ungrabPointer();
    if (index >= int(tabletSwitchOrder.size())) {
        layoutTabletSwitcher();
        dirty = true;
        return;
    }
    Client* c = tabletSwitchOrder[size_t(index)];
    if (travel > 110) {
        // Swiped off the top: quit the app, the way iOS closes it from here.
        if (c) closeClient(c);
        tabletSwitchOrder.erase(tabletSwitchOrder.begin() + index);
        if (tabletSwitchOrder.empty()) {
            closeTabletSwitcher();
            tabletGoHome();
            return;
        }
        layoutTabletSwitcher();
    } else if (travel < 12) {
        // A tap opens the app and leaves the switcher.
        if (c) {
            restoreClient(c);
            focusClient(c, true);
        }
        closeTabletSwitcher();
    }
    dirty = true;
}

bool Manager::handleTabletIconPress(int x, int y) {
    for (size_t i = 0; i < tabletHomeRects.size() && i < tabletHome.size(); ++i) {
        if (!tabletHomeRects[i].contains(x, y)) continue;
        beginTabletIconDrag(int(i), x, y);
        return true;
    }
    return false;
}

void Manager::beginTabletIconDrag(int index, int x, int y) {
    if (index < 0 || index >= int(tabletHomeRects.size())) return;
    tabletDragIcon = index;
    tabletDragTarget = index;
    tabletDragRect = tabletHomeRects[size_t(index)];
    tabletDragGrab = Point{x - tabletDragRect.x, y - tabletDragRect.y};
    grabPointer();
    dirty = true;
}

void Manager::updateTabletIconDrag(int x, int y) {
    if (tabletDragIcon < 0) return;
    tabletDragRect.x = x - tabletDragGrab.x;
    tabletDragRect.y = y - tabletDragGrab.y;
    // Snap to the cell whose centre is nearest the pointer.
    int best = tabletDragIcon;
    long bestDist = -1;
    for (size_t i = 0; i < tabletHomeRects.size(); ++i) {
        const Rect& c = tabletHomeRects[i];
        const long dx = (c.x + c.w / 2) - x;
        const long dy = (c.y + c.h / 2) - y;
        const long d = dx * dx + dy * dy;
        if (bestDist < 0 || d < bestDist) {
            bestDist = d;
            best = int(i);
        }
    }
    if (best != tabletDragTarget) tabletDragTarget = best;
    dirty = true;
}

void Manager::endTabletIconDrag() {
    if (tabletDragIcon < 0) return;
    const int from = tabletDragIcon;
    const int to = tabletDragTarget;
    tabletDragIcon = -1;
    tabletDragTarget = -1;
    ungrabPointer();
    // Reordering the list is what the drop means; the labelled layout then
    // re-flows around it on the next frame.
    if (to >= 0 && to != from && from < int(tabletHome.size()) && to < int(tabletHome.size())) {
        TabletEntry moved = tabletHome[size_t(from)];
        tabletHome.erase(tabletHome.begin() + from);
        tabletHome.insert(tabletHome.begin() + to, std::move(moved));
    }
    layoutTabletHome();
    dirty = true;
}

}  // namespace wm
