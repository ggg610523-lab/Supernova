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
        if (!c->mapped || c->minimized || c->unredirected) continue;
        // Re-binding the pixmap here (rather than per motion event) keeps a
        // resize down to one pixmap per frame.
        if (c->pixW <= 0 || !c->tex.valid()) ensurePixmap(c);
        drawClientSprite(c);
    }

    drawSnapPreview();
    drawTaskbar();
    if (startOpen || startAnim > 0.0) drawStartMenu();
    if (taskViewOpen || taskViewAnim > 0.0) drawTaskView();
    if (altTabOpen || altTabAnim > 0.0) drawAltTab();
    if (contextOpen) drawContextMenu();
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
    for (size_t i = 0; i < desktopItems.size(); ++i) {
        const int col = int(i) / rows;
        const int row = int(i) % rows;
        desktopIconRects.push_back(Rect{marginX + col * (cellW + gapX),
                                        marginY + row * (cellH + gapY), cellW, cellH});
    }
}

// Everything sitting on the user's Desktop folder: a folder, a dropped file or
// a .desktop launcher. A click selects, a double click opens (input.cpp).
void Manager::drawDesktopIcons() {
    if (desktopIconRects.size() != desktopItems.size()) layoutDesktopIcons();
    for (size_t i = 0; i < desktopItems.size() && i < desktopIconRects.size(); ++i) {
        const DesktopItem& item = desktopItems[i];
        const Rect cell = desktopIconRects[i];
        const bool selected = int(i) == selectedDesktopIcon;
        const bool hovered = int(i) == hoverDesktopIcon;
        if (selected || hovered) {
            const Color fill =
                selected ? Color{theme::kAccent.r, theme::kAccent.g, theme::kAccent.b, 0.30f}
                         : theme::kItemHover;
            comp.drawRect(Rect{cell.x + 2, cell.y + 2, cell.w - 4, cell.h - 4}, 6.f, fill);
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

void Manager::drawDesktop() {
    comp.drawWallpaper();
    drawDesktopIcons();
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
        // Open: grow the last few percent while fading in (Fluent motion).
        const double t = fluentEase(c->appear);
        opacity *= 0.25 + 0.75 * t;
        scale = 0.93 + 0.07 * t;
    }
    if (c->minFade > 0.0) {
        // Minimise: shrink and slide towards the taskbar item. The client is
        // unmapped only once minFade reaches 1, so it stays live the whole way.
        const double m = c->minFade;
        opacity *= 1.0 - m;
        scale *= 1.0 - 0.10 * m;
        Rect target = f;
        for (const TaskItem& it : taskItems) {
            if (it.client != c) continue;
            target = Rect{it.rect.x, screenH - metrics::kTaskbarH, it.rect.w, it.rect.w};
            break;
        }
        f = lerpRect(f, target, m * m);
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
    s.minHover = c->hoverBtn == 0;
    s.maxHover = c->hoverBtn == 1;
    s.closeHover = c->hoverBtn == 2;
    s.minPress = c->pressBtn == 0;
    s.maxPress = c->pressBtn == 1;
    s.closePress = c->pressBtn == 2;
    comp.drawWindow(s);

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

    if (hoverStart) comp.drawRect(startButtonRect, 6.f, theme::kItemHover);
    drawStartGlyph(startButtonRect, theme::kGlyph);

    for (size_t i = 0; i < taskItems.size(); ++i) {
        const TaskItem& it = taskItems[i];
        Client* w = it.client;
        if (!w) continue;
        const bool active = (w == focused) && !w->minimized;
        const bool hovered = int(i) == hoverTaskIndex;
        if (active || hovered) {
            comp.drawRect(it.rect, 6.f, hovered ? theme::kItemHover : theme::kItemActive);
        }
        const Rect iconArea{it.rect.x + (it.rect.w - 24) / 2, it.rect.y + (it.rect.h - 24) / 2,
                            24, 24};
        if (w->iconTex && w->iconW > 0) {
            comp.drawTex(w->iconTex, iconArea, 4.f, Color{1.f, 1.f, 1.f, 1.f}, 1.f, true, true);
        } else if (!drawAppIcon(iconArea, w->appName, w->appName, 5.f, 1.f)) {
            drawAppTile(iconArea, w->title, 5.f, tileTint(w->title), false);
        }
        // The running/active pill on the bottom edge of the button.
        const int iw = active ? 16 : (hovered ? 8 : 6);
        const Color ic = active ? theme::kAccent : (hovered ? theme::kTextMuted : theme::kTextDim);
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

    if (hoverShowDesktop) comp.drawRect(showDesktopRect, 2.f, theme::kItemHover);
    comp.drawRect(Rect{screenW - 3, y + 6, 2, metrics::kTaskbarH - 12}, 1.f, theme::kShellBorder);
}

void Manager::drawSnapPreview() {
    if (snapZonePreview == kSnapNone) return;
    const Rect inner = snapGeometry(snapZonePreview).inflated(4);
    if (inner.empty()) return;
    comp.drawRect(inner, 8.f, theme::kSnapFill);
    // One pixel accent outline: four thin rects, no ring primitive needed.
    const Color b = theme::kSnapBorder;
    comp.drawRect(Rect{inner.x, inner.y, inner.w, 1}, 1.f, b);
    comp.drawRect(Rect{inner.x, inner.bottom() - 1, inner.w, 1}, 1.f, b);
    comp.drawRect(Rect{inner.x, inner.y, 1, inner.h}, 1.f, b);
    comp.drawRect(Rect{inner.right() - 1, inner.y, 1, inner.h}, 1.f, b);
}

void Manager::layoutStartMenu() {
    appFiltered.clear();
    appRects.clear();
    for (size_t i = 0; i < apps.size(); ++i) {
        if (!searchText.empty() && !containsFold(apps[i].searchKey, searchText)) continue;
        appFiltered.push_back(i);
    }
    const int w = std::min(screenW - 24, metrics::kStartW);
    const int h = std::min(screenH - metrics::kTaskbarH - 24, metrics::kStartH);
    startRect = Rect{(screenW - w) / 2, screenH - metrics::kTaskbarH - 12 - h, w, h};
    searchRect = Rect{startRect.x + 24, startRect.y + 24, w - 48, 36};

    const int cols = screenW < 560 ? 4 : 6;
    const int cellW = (w - 48) / cols;
    const int cellH = 70;
    const int gridTop = searchRect.bottom() + 22;
    const int maxRows = std::max(1, (startRect.bottom() - gridTop - 12) / cellH);
    const size_t maxTiles = size_t(cols) * size_t(maxRows);
    for (size_t i = 0; i < appFiltered.size() && i < maxTiles; ++i) {
        const int cx = int(i) % cols;
        const int cy = int(i) / cols;
        appRects.push_back(Rect{startRect.x + 24 + cx * cellW, gridTop + cy * cellH, cellW - 8,
                                cellH - 8});
    }
}

void Manager::drawStartMenu() {
    layoutStartMenu();
    const double eased = fluentEase(startAnim);
    if (eased <= 0.001) return;
    const float a = float(eased);
    const int rise = int(std::lround((1.0 - eased) * 24.0));
    const Rect panel{startRect.x, startRect.y + rise, startRect.w, startRect.h};

    comp.drawAcrylic(panel, float(metrics::kFlyoutRadius), theme::kFlyoutTint, 0.88f,
                     theme::kShellBorder, a);

    const Rect search{searchRect.x, searchRect.y + rise, searchRect.w, searchRect.h};
    comp.drawRect(search, 6.f, theme::kSearchBox, a);
    drawSearchGlyph(Rect{search.x + 10, search.y, 20, search.h}, theme::kTextMuted);
    const int textY = search.y + (search.h - 18) / 2;
    if (searchText.empty()) {
        drawTextAt("Search apps and settings", 13, Weight::Regular, theme::kTextDim,
                   search.x + 34, textY);
    } else {
        drawTextAt(searchText, 13, Weight::Regular, theme::kText, search.x + 34, textY);
    }

    drawTextAt(searchText.empty() ? "Pinned" : "Best match", 12, Weight::Bold, theme::kTextMuted,
               startRect.x + 24, searchRect.bottom() + 22 + rise);

    for (size_t i = 0; i < appRects.size() && i < appFiltered.size(); ++i) {
        Rect cell = appRects[i];
        cell.y += rise;
        if (cell.bottom() > panel.bottom()) break;
        const AppEntry& e = apps[appFiltered[i]];
        if (int(i) == hoverApp) comp.drawRect(cell, 5.f, theme::kItemHover, a);
        const Rect icon{cell.x + (cell.w - 36) / 2, cell.y + 4, 36, 36};
        if (!drawAppIcon(icon, e.icon, e.wmClass, 6.f, a)) {
            drawAppTile(icon, e.name, 7.f, tileTint(e.name), false);
        }
        drawTextCentered(ellipsize(text, e.name, 11, cell.w - 8), 11, Weight::Regular, theme::kText,
                         Rect{cell.x, icon.bottom() + 4, cell.w, 18});
    }
    if (appRects.empty()) {
        drawTextCentered("No apps match your search", 13, Weight::Regular, theme::kTextMuted,
                         Rect{startRect.x, startRect.y + rise + 110, startRect.w, 40});
    }
}

void Manager::drawContextMenu() {
    if (contextItems.empty()) return;
    const int itemH = 32;
    const int pad = 6;
    const Rect panel{contextRect.x, contextRect.y, contextRect.w,
                     int(contextItems.size()) * itemH + 2 * pad};
    comp.drawAcrylic(panel, float(metrics::kFlyoutRadius), theme::kFlyoutTint, 0.90f,
                     theme::kShellBorder);
    for (size_t i = 0; i < contextItems.size(); ++i) {
        const Rect item{panel.x + pad, panel.y + pad + int(i) * itemH, panel.w - 2 * pad, itemH};
        if (contextHover == int(i)) comp.drawRect(item, 4.f, theme::kItemHover);
        drawTextAt(contextItems[i], 13, Weight::Regular,
                   contextHover == int(i) ? theme::kText : theme::kTextIdle, item.x + 14,
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