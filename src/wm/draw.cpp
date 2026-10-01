// Everything the shell paints. All of it is expressed in screen pixels and
// handed to the compositor's shader as rects, so the same code produces the
// rounded corners, the Mica captions and the acrylic surfaces.
#include "manager.h"

#include <ctime>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "theme.h"

namespace wm {
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
    drawDesktop();

    // Bottom to top, exactly like the X stacking order, so the focused window
    // is painted last (and therefore on top).
    for (auto& cp : clients) {
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

    // App launch placeholders sit above the windows but below the taskbar, so a
    // just-launched app reads as "opening" the instant it is double-clicked.
    drawLaunches();

    drawSnapPreview();
    drawTaskbar();
    if (startOpen || startAnim > 0.0) drawStartMenu();
    if (taskViewOpen || taskViewAnim > 0.0) drawTaskView();
    if (altTabOpen || altTabAnim > 0.0) drawAltTab();
    if (contextOpen || contextAnim > 0.0) drawContextMenu();
    if (ccOpen || ccAnim > 0.0) drawControlCenter();
    if (opts->stats) drawStats();
}

// The desktop grid lays out top-to-bottom and then starts a new column to the
// right, which is the order Windows fills its desktop in.
void Manager::layoutDesktopIcons() {
    desktopIconRects.clear();
    const int cellW = 92, cellH = 92, gapX = 6, gapY = 2;
    const int marginX = 10, marginY = 10;
    const int usableH = screenH - metrics::kTaskbarH - marginY;
    const int rows = std::max(1, (usableH + gapY) / (cellH + gapY));
    const auto cellAt = [&](int n) {
        const int col = n / rows, row = n % rows;
        return Rect{marginX + col * (cellW + gapX), marginY + row * (cellH + gapY), cellW, cellH};
    };
    // Walk the same top-to-bottom, left-to-right order, but step over any cell a
    // widget occupies so the icons reflow around the glass cards.
    int slot = 0;
    for (size_t i = 0; i < desktopItems.size(); ++i) {
        Rect cell;
        for (int guard = 0; guard < 4096; ++guard) {
            cell = cellAt(slot++);
            bool blocked = false;
            for (const Widget& w : widgets)
                if (cell.inflated(8).intersects(w.rect)) {
                    blocked = true;
                    break;
                }
            if (!blocked) break;
        }
        desktopIconRects.push_back(cell);
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

// Every frame, glide each icon's shown cell toward the cell the grid wants it
// in. Frame-rate independent exponential smoothing -- the same shape as macOS'
// critically damped reflow -- so a widget being dropped, resized or dragged over
// the grid slides the surrounding icons out of the way instead of teleporting
// them. Retargeting mid-flight is free: the icons simply head for the new cells,
// which is exactly what a widget being dragged across the desktop needs.
void Manager::animateDesktopIconReflow(double dtMs) {
    if (desktopIconDraw.size() != desktopIconRects.size()) layoutDesktopIcons();
    if (desktopIconDraw.empty()) return;
    // Time constant: ~110ms lands the glide in about a third of a second, quick
    // enough to feel responsive but slow enough to read as motion.
    const double tau = 110.0;
    const double k = 1.0 - std::exp(-std::max(0.0, dtMs) / tau);
    for (size_t i = 0; i < desktopIconDraw.size() && i < desktopIconRects.size(); ++i) {
        const Rect target = desktopIconRects[i];
        Rect& cur = desktopIconDraw[i];
        if (cur == target) continue;
        const auto ease = [k](int a, int b) { return int(std::lround(a + (b - a) * k)); };
        const Rect next{ease(cur.x, target.x), ease(cur.y, target.y), ease(cur.w, target.w),
                        ease(cur.h, target.h)};
        // Rounding can stall a sub-pixel move; finish the glide in that case so
        // the animation always terminates (and never repaints forever).
        cur = (next == cur) ? target : next;
        dirty = true;
    }
}

// Everything sitting on the user's Desktop folder: a folder, a dropped file or
// a .desktop launcher. A click selects, a double click opens (input.cpp).
void Manager::drawDesktopIcons() {
    if (desktopIconRects.size() != desktopItems.size() ||
        desktopIconDraw.size() != desktopItems.size())
        layoutDesktopIcons();
    for (size_t i = 0; i < desktopItems.size() && i < desktopIconDraw.size(); ++i) {
        const DesktopItem& item = desktopItems[i];
        // Draw the eased cell, not the target: this is what makes the icons
        // glide while a widget reflows the grid.
        const Rect cell = desktopIconDraw[i];
        const bool selected = int(i) == selectedDesktopIcon;
        const bool hovered = int(i) == hoverDesktopIcon;
        // Hover and selection share one eased amount, so the highlight fades in
        // and, when the selection moves on, cross-fades to the next icon.
        const double hl = i < desktopIconHover.size() ? desktopIconHover[i] : 0.0;
        if (hl > 0.001) {
            const Color fill =
                selected ? Color{theme::kAccent.r, theme::kAccent.g, theme::kAccent.b, 0.30f}
                         : theme::kItemHover;
            comp.drawRect(Rect{cell.x + 2, cell.y + 2, cell.w - 4, cell.h - 4}, 6.f, fill,
                          float(hl));
        }
        const Rect icon{cell.x + (cell.w - 48) / 2, cell.y + 8, 48, 48};
        // The item's own icon first, then a generic one, then a letter tile.
        const char* fallback = item.isDir ? "folder" : "text-plain";
        if (!drawAppIcon(icon, item.icon, std::string(), 6.f, 1.0f) &&
            !drawAppIcon(icon, fallback, std::string(), 6.f, 1.0f)) {
            drawAppTile(icon, item.name, 8.f, tileTint(item.name), hovered);
        }
        const std::string label = ellipsize(text, item.name, 11, cell.w - 8);
        if (label.empty()) continue;
        const TextTex t = text.get(label, 11, Weight::Regular);
        if (!t.tex) continue;
        const int lx = cell.x + (cell.w - t.w) / 2;
        const int ly = icon.bottom() + 6;
        // Desktop labels sit on a photo, so a one pixel dark drop shadow keeps
        // them legible over a light patch of wallpaper.
        comp.drawText(t, Rect{lx + 1, ly + 1, t.w, t.h}, Color{0.f, 0.f, 0.f, 0.65f}, 1.0f);
        comp.drawText(t, Rect{lx, ly, t.w, t.h}, theme::kText, 1.0f);
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
    const int bottom = screenH - metrics::kTaskbarH;
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
    drawDesktopIcons();
    drawWidgets();
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

// The Windows 11 Start logo: assets/icons/2048.svg when the asset pipeline has
// rendered it, otherwise four squares in a 2x2 grid.
void Manager::drawStartGlyph(const Rect& box, const Color& c) {
    const Rect icon{box.x, box.y + (box.h - box.w) / 2, box.w, box.w};
    if (icon.w >= 8 && drawAppIcon(icon, "2048", "2048", 0.f, c.a)) return;
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

// Reversal icon for a launcher, sized into `r`. Returns false when the theme
// has nothing for this entry so the caller can draw the letter tile instead.
bool Manager::drawAppIcon(const Rect& r, const std::string& iconName,
                          const std::string& wmClass, float radius, float opacity) {
    const IconTex t = icons.forApp(iconName, wmClass);
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
    double genie = 0.0;
    Rect genieIcon{};
    if (c->minFade > 0.0) {
        const double m = c->minFade;
        genie = m;
        genieIcon = Rect{screenW / 2 - 12, screenH - metrics::kTaskbarH, 24, 24};
        for (const TaskItem& it : taskItems) {
            if (it.client != c) continue;
            genieIcon = Rect{it.rect.x + (it.rect.w - 24) / 2, it.rect.y + (it.rect.h - 24) / 2,
                             24, 24};
            break;
        }
        opacity *= 1.0 - m * m;
    }
    if (c->vanish > 0.0) {
        opacity *= 1.0 - c->vanish;
        scale *= 1.0 - 0.06 * c->vanish;
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
    s.radius = (c->fullscreen || s.maximized) ? 0.f : float(metrics::kRadius);
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
    const int y = screenH - metrics::kTaskbarH;
    const int top = y + (metrics::kTaskbarH - metrics::kTaskIconH) / 2;
    const int gap = 4;
    const int w = metrics::kTaskIconW;
    const int h = metrics::kTaskIconH;

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
    const int total = count * w + (count > 1 ? (count - 1) * gap : 0);
    // Windows 11 centres the Start button together with the whole app group.
    int x = (screenW - (total + w + gap)) / 2;
    if (x < 8) x = 8;
    startButtonRect = Rect{x, top, w, h};
    x += w + gap;
    const int rightLimit = screenW - 150;  // the clock cluster owns the right side
    for (Client* c : visible) {
        if (x + w > rightLimit) break;
        taskItems.push_back(TaskItem{Rect{x, top, w, h}, c});
        x += w + gap;
    }
    clockRect = Rect{screenW - 140, y, 116, metrics::kTaskbarH};
    showDesktopRect = Rect{screenW - 14, y, 14, metrics::kTaskbarH};
}

void Manager::drawTaskbar() {
    const int y = screenH - metrics::kTaskbarH;
    const Rect bar{0, y, screenW, metrics::kTaskbarH};

    // Acrylic: the blurred wallpaper tinted towards the taskbar colour, plus the
    // one pixel line Windows 11 puts on the top edge.
    comp.drawAcrylic(bar, 0.f, theme::kTaskbarTint, 0.80f, theme::kShellLine, 1.0f);
    comp.drawRect(Rect{0, y, screenW, 1}, 0.f, theme::kShellBorder);

    if (startHoverAnim > 0.001)
        comp.drawRect(startButtonRect, 6.f, theme::kItemHover, float(startHoverAnim));
    drawStartGlyph(startButtonRect, theme::kGlyph);

    for (size_t i = 0; i < taskItems.size(); ++i) {
        const TaskItem& it = taskItems[i];
        Client* w = it.client;
        if (!w) continue;
        const bool active = (w == focused) && !w->minimized;
        const double hv = i < taskHover.size() ? taskHover[i] : 0.0;
        // The active wash is a base layer; the hover wash fades over it, so a
        // button lights up smoothly instead of popping.
        if (active) comp.drawRect(it.rect, 6.f, theme::kItemActive);
        if (hv > 0.001) comp.drawRect(it.rect, 6.f, theme::kItemHover, float(hv));
        const Rect iconArea{it.rect.x + (it.rect.w - 24) / 2, it.rect.y + (it.rect.h - 24) / 2,
                            24, 24};
        if (w->iconTex && w->iconW > 0) {
            comp.drawTex(w->iconTex, iconArea, 4.f, Color{1.f, 1.f, 1.f, 1.f}, 1.f, true, true);
        } else if (!drawAppIcon(iconArea, w->appName, w->appName, 5.f, 1.f)) {
            drawAppTile(iconArea, w->title, 5.f, tileTint(w->title), false);
        }
        // The running/active pill on the bottom edge of the button: it grows and
        // brightens as the button is hovered or becomes active.
        const int iw = std::max(1, int(std::lround(active ? 16.0 : lerp(6.0, 8.0, hv))));
        const Color ic = active ? theme::kAccent
                                : mixColor(theme::kTextDim, theme::kTextMuted, float(hv));
        comp.drawRect(Rect{it.rect.x + (it.rect.w - iw) / 2, bar.bottom() - 5, iw, 3}, 1.5f, ic);
    }

    time_t now = time(nullptr);
    struct tm lt {};
    localtime_r(&now, &lt);
    char hhmm[16] = {0};
    char dateStr[24] = {0};
    strftime(hhmm, sizeof hhmm, "%H:%M", &lt);
    strftime(dateStr, sizeof dateStr, "%d/%m/%Y", &lt);
    const int right = clockRect.right();
    drawTextRight(hhmm, 13, Weight::Regular, theme::kText, right, y + 9);
    drawTextRight(dateStr, 11, Weight::Regular, theme::kTextMuted, right, y + 27);

    if (showDesktopHoverAnim > 0.001)
        comp.drawRect(showDesktopRect, 2.f, theme::kItemHover, float(showDesktopHoverAnim));
    comp.drawRect(Rect{screenW - 3, y + 6, 2, metrics::kTaskbarH - 12}, 1.f, theme::kShellBorder);
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
    const int taskbarTop = screenH - metrics::kTaskbarH;
    startRect = Rect{0, 0, screenW, taskbarTop};

    // Top-centred search field, like macOS Launchpad.
    const int sw = std::min(400, screenW - 48);
    const int sh = 34;
    const int sy = std::max(18, taskbarTop / 14);
    searchRect = Rect{(screenW - sw) / 2, sy, sw, sh};

    // Grid: big squircles with a name under each, centred between the search
    // field and the page dots.
    const int icon = std::clamp(std::min(screenW, screenH) / 16, metrics::kLaunchIconMin,
                                metrics::kLaunchIcon);
    const int cellW = icon + icon / 2 + 24;
    const int cellH = icon + metrics::kLaunchLabelH + metrics::kLaunchRowGap;
    const int marginX = std::max(24, screenW / 12);
    const int cols = std::max(3, std::min(7, (screenW - 2 * marginX) / cellW));

    const int gridTop = searchRect.bottom() + 22;
    const int gridBottom = taskbarTop - metrics::kLaunchDotsH;
    const int rows = std::max(1, (gridBottom - gridTop) / cellH);
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
    const double eased = fluentEase(startAnim);
    if (eased <= 0.001) return;
    const float a = float(eased);

    // A frosted, semi-transparent glass over the desktop, the way macOS' Launchpad
    // reads: the blurred wallpaper supplies the blur, the black wash keeps it a
    // touch dark -- but the acrylic is deliberately NOT opaque. An opaque sample
    // of a dark wallpaper is what collapsed this to solid black; letting ~34% of
    // the real desktop through keeps it translucent instead.
    constexpr float kGlass = 0.66f;
    comp.drawAcrylic(startRect, 0.f, theme::kLaunchTint, 0.10f, Color{0.f, 0.f, 0.f, 0.f},
                     a * kGlass);
    comp.drawRect(startRect, 0.f, theme::kLaunchDim, a);

    // Top-centred search pill: magnifier on the left, then placeholder or query,
    // laid out left to right with a real gap. (The glyph used to span 34px from
    // x+12 while the text began at x+32, so the two overprinted each other, and a
    // long query ran straight off the end of the pill.)
    const int sh = searchRect.h;
    comp.drawRect(searchRect, float(sh) * 0.5f, theme::kLaunchSearch, a);
    const int glyph = 18;
    const int padX = 14;
    drawSearchGlyph(Rect{searchRect.x + padX, searchRect.y + (sh - glyph) / 2, glyph, glyph},
                    theme::kLaunchSearchText);
    {
        const std::string label = searchText.empty() ? std::string("Search") : searchText;
        const Color col = searchText.empty() ? theme::kLaunchSearchText : theme::kText;
        const int textX = searchRect.x + padX + glyph + 8;
        const int maxW = searchRect.right() - textX - 14;
        const TextTex t = text.get(ellipsize(text, label, 14, maxW), 14, Weight::Regular);
        if (t.tex) {
            comp.drawText(t, Rect{textX, searchRect.y + (sh - t.h) / 2, t.w, t.h}, col, a);
        }
    }

    // Icons. During the open they scale up about their own centres, which never
    // moves the hit rects (those stay the cells computed by layoutStartMenu).
    const float pop = 0.90f + 0.10f * float(eased);
    const int slot = std::clamp(std::min(screenW, screenH) / 16, metrics::kLaunchIconMin,
                                metrics::kLaunchIcon);
    for (size_t i = 0; i < appRects.size() && i < appFiltered.size(); ++i) {
        const Rect cell = appRects[i];
        const AppEntry& e = apps[appFiltered[startPageBase + i]];
        const int drawn = int(std::lround(slot * pop));
        const int ix = cell.x + (cell.w - drawn) / 2;
        const int iy = cell.y + (slot - drawn) / 2;
        const Rect box{ix, iy, drawn, drawn};
        const double hav = i < appHover.size() ? appHover[i] : 0.0;
        if (hav > 0.001) {
            const int pad = std::max(4, drawn / 8);
            comp.drawRect(Rect{ix - pad, iy - pad, drawn + 2 * pad, drawn + 2 * pad},
                          float(drawn) * 0.30f, theme::kLaunchHover, a * float(hav));
        }
        if (!drawAppIcon(box, e.icon, e.wmClass, float(drawn) * 0.24f, a)) {
            drawAppTile(box, e.name, float(drawn) * 0.24f, tileTint(e.name), false);
        }
        const TextTex t = text.get(ellipsize(text, e.name, 12, cell.w - 12), 12, Weight::Regular);
        if (!t.tex) continue;
        const int lx = cell.x + (cell.w - t.w) / 2;
        const int ly = cell.y + slot + 6;
        // A one pixel shadow keeps the white label legible over a light patch.
        comp.drawText(t, Rect{lx + 1, ly + 1, t.w, t.h}, theme::kLaunchLabelShadow, a);
        comp.drawText(t, Rect{lx, ly, t.w, t.h}, theme::kLaunchLabel, a);
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
        const int cx = d.x + d.w / 2, cy = d.y + d.h / 2;
        const Color col =
            mixColor(theme::kLaunchDot, theme::kLaunchDotActive, active ? 1.f : float(dv));
        comp.drawRect(Rect{cx - r, cy - r, 2 * r, 2 * r}, float(r), col, a);
    }
}

void Manager::drawContextMenu() {
    if (contextItems.empty()) return;
    const double eased = fluentEase(contextAnim);
    if (eased <= 0.001) return;
    const float a = float(eased);
    const int itemH = 32;
    const int pad = 6;
    // The flyout drops the last few pixels into place as it fades in, the way
    // Windows 11 menus do, and reverses cleanly when it is dismissed.
    const int dy = int(std::lround(-8.0 * (1.0 - eased)));
    const Rect panel{contextRect.x, contextRect.y + dy, contextRect.w,
                     int(contextItems.size()) * itemH + 2 * pad};
    comp.drawAcrylic(panel, float(metrics::kFlyoutRadius), theme::kFlyoutTint, 0.90f,
                     theme::kShellBorder, a);
    for (size_t i = 0; i < contextItems.size(); ++i) {
        const Rect item{panel.x + pad, panel.y + pad + int(i) * itemH, panel.w - 2 * pad, itemH};
        const double hv = i < ctxHover.size() ? ctxHover[i] : 0.0;
        if (hv > 0.001) comp.drawRect(item, 4.f, theme::kItemHover, float(hv) * a);
        const Color col = mixColor(theme::kTextIdle, theme::kText, float(hv));
        drawTextAt(contextItems[i], 13, Weight::Regular, col, item.x + 14,
                   item.y + (item.h - 18) / 2);
    }
}

void Manager::drawStats() {
    const Rect box{12, 12, 340, 104};
    comp.drawRect(box, 6.f, theme::kFlyoutTint, 0.93f);
    comp.drawRect(box, 6.f, theme::kShellBorder, 1.0f);
    char line[220];
    snprintf(line, sizeof line, "%d fps   frame %.2f ms   vsync %s (interval %d)",
             comp.sampledFps(), comp.lastFrameSeconds() * 1000.0,
             comp.vsyncActive() ? "on" : "off", comp.swapInterval());
    drawTextAt(line, 12, Weight::Regular, theme::kText, box.x + 12, box.y + 10);
    snprintf(line, sizeof line, "%s", comp.rendererName().c_str());
    drawTextAt(line, 11, Weight::Regular, theme::kTextMuted, box.x + 12, box.y + 32);
    snprintf(line, sizeof line, "%zu window(s) | texture_from_pixmap %s | font %s",
             clients.size(), comp.hasTextureFromPixmap() ? "yes" : "MISSING", text.family().c_str());
    drawTextAt(line, 11, Weight::Regular, theme::kTextMuted, box.x + 12, box.y + 52);
    snprintf(line, sizeof line, "drag: %s   focus: %s", dragClient ? (dragIsMove ? "move" : "resize") : "idle",
             focused ? focused->title.c_str() : "none");
    drawTextAt(line, 11, Weight::Regular, theme::kTextMuted, box.x + 12, box.y + 72);
    snprintf(line, sizeof line, "display %dx%d", screenW, screenH);
    drawTextAt(line, 11, Weight::Regular, theme::kTextMuted, box.x + 12, box.y + 88);
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
    const int marginX = 56, marginTop = 64, marginBottom = metrics::kTaskbarH + 44;
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