// The iOS Control Centre: a 4 column x 6 row grid of dark glass tiles that
// drops out of the taskbar clock.
//
// The layout is the real one. A module occupies whole grid cells and its inner
// controls are 54pt circles (or, for brightness and volume, a full-height pill
// with a fill level), which is what makes the panel read as Control Centre
// rather than as a list of switches:
//
//        0              1              2              3
//   0  [ connections 2x2          ] [ media 2x2               ]
//   1  [                           ] [                        ]
//   2  [ dnd 1x1 ] [ night 1x1 ] [ brightness 1x2 ] [ volume 1x2 ]
//   3  [ show desktop 1x2         ] [  (sliders still)         ]
//   4  [ lock ] [ screenshot ] [ files ] [ terminal ]
//   5  [ tablet mode 2x1        ] [ light mode 2x1           ]
//
// The last row is this shell's own two switches rather than anything iOS ships:
// the desktop <-> tablet hand-off, and the Fluent shell's light/dark palette.
// Everything that needs the outside world goes through SystemControls, which
// probes asynchronously; this file only reads the cached state and draws.
#include "manager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "theme.h"

namespace wm {
namespace {

// Interactive parts of the panel, in draw order. The values are internal; the
// hit test only ever compares them.
enum CcId {
    kCcNone = 0,
    // connections tile (2x2)
    kCcAirplane,
    kCcWifi,
    kCcBluetooth,
    kCcWired,
    // media tile (2x2)
    kCcMediaPrev,
    kCcMediaPlay,
    kCcMediaNext,
    // sliders (1x2 each)
    kCcBrightness,
    kCcVolume,
    // 1x1 toggles
    kCcDnd,
    kCcNight,
    kCcShowDesktop,
    // the bottom row: two wide plates, each this shell's own switch
    kCcTablet,
    kCcLight,
    // 1x2
    kCcLock,
    kCcScreenshot,
    // launcher row
    kCcLauncherFirst,
    kCcLauncherCount = 2,
};

// `ellipsize` lives in draw.cpp's anonymous namespace; this is the same idea for
// the Control Centre's own strings.
std::string ccEllipsize(Text& text, const std::string& s, int px, int maxW) {
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

}  // namespace

void Manager::setLightMode(bool on) {
    const theme::Mode want = on ? theme::Mode::Light : theme::Mode::Dark;
    if (theme::mode == want) return;
    theme::applyMode(want);
    saveThemeMode(want);
    // Repaint everything, not just the panel: window captions, the taskbar and any
    // open flyout all read the palette, and only a full frame shows that.
    dirty = true;
}

int Manager::ccControlIndex(int id, int slot) const {
    for (size_t i = 0; i < ccControls.size(); ++i) {
        if (ccControls[i].id == id && ccControls[i].slot == slot) return int(i);
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

int Manager::ccScale() const {
    const int baseW = 2 * metrics::kCcPad + metrics::kCcCols * metrics::kCcCell +
                      (metrics::kCcCols - 1) * metrics::kCcGap;
    const int baseH = 2 * metrics::kCcPad + metrics::kCcRows * metrics::kCcCell +
                      (metrics::kCcRows - 1) * metrics::kCcGap;
    // Leave room for the taskbar plus the drop shadow above it.
    const int availH = screenH - metrics::taskbarH - 24;
    const int availW = screenW - 24;
    int scale = metrics::kCcMaxScale;
    if (availW * 100 / baseW < scale) scale = availW * 100 / baseW;
    if (availH * 100 / baseH < scale) scale = availH * 100 / baseH;
    return std::max(40, scale);
}

// Places every module and records each clickable part in ccControls.
void Manager::layoutControlCenter() {
    ccControls.clear();
    const int scale = ccScale();
    const int cell = metrics::kCcCell * scale / 100;
    const int gap = std::max(2, metrics::kCcGap * scale / 100);
    const int pad = metrics::kCcPad * scale / 100;
    const int cols = metrics::kCcCols;
    const int rows = metrics::kCcRows;

    const int w = 2 * pad + cols * cell + (cols - 1) * gap;
    const int h = 2 * pad + rows * cell + (rows - 1) * gap;

    if (tabletMode) {
        // Tablet mode has no taskbar clock to hang off, so Control Centre drops
        // out of the top-right corner instead, the way it does on iOS (swipe
        // down from the top-right). It zooms out of that corner.
        const int margin = std::max(10, screenW / 64);
        int x = screenW - w - margin;
        if (x < 4) x = 4;
        int y = std::max(margin, metrics::kTabletStatusH - 10);
        if (y + h > screenH - 4) y = std::max(4, screenH - 4 - h);
        ccRect = Rect{x, y, w, h};
        ccAnchorX = clampi(screenW - margin, ccRect.x, ccRect.right());
        ccAnchorY = clampi(metrics::kTabletStatusH, ccRect.y, ccRect.bottom());
    } else {
        // Anchored above the clock, nudged left so it is not hard against the edge.
        int x = clockRect.right() - w - 4;
        if (x + w > screenW - 4) x = screenW - w - 4;
        if (x < 4) x = 4;
        int y = screenH - metrics::taskbarH - 12 - h;
        if (y < 4) y = 4;
        ccRect = Rect{x, y, w, h};
        // The panel zooms out of the clock, so remember the point under it,
        // clamped to the panel so the origin is always inside the rectangle.
        ccAnchorX = clampi(clockRect.x + clockRect.w / 2, ccRect.x, ccRect.right());
        ccAnchorY = clampi(clockRect.y + clockRect.h / 2, ccRect.y, ccRect.bottom());
    }

    // A module spanning `span` cells, positioned at grid column/row.
    const auto cellRect = [&](int col, int row, int colspan, int rowspan) {
        return Rect{ccRect.x + pad + col * (cell + gap), ccRect.y + pad + row * (cell + gap),
                    colspan * cell + (colspan - 1) * gap, rowspan * cell + (rowspan - 1) * gap};
    };
    // The circular button inside a module, centred in one of its quarters.
    const auto innerButton = [&](const Rect& module, int col, int row, int colspan, int rowspan) {
        const int qw = (module.w - gap) / 2;
        const int qh = (module.h - gap) / 2;
        const int d = std::min(metrics::kCcButton * scale / 100, std::min(qw, qh));
        const int cx = module.x + col * (qw + gap) + qw / 2;
        const int cy = module.y + row * (qh + gap) + qh / 2;
        return Rect{cx - d / 2, cy - d / 2, d, d};
    };
    const auto add = [&](const Rect& r, int id, int slot = 0, bool vertical = false) {
        CcControl c;
        c.rect = r;
        c.id = id;
        c.slot = slot;
        c.vertical = vertical;
        ccControls.push_back(c);
        return c;
    };

    // --- connections 2x2: airplane, wifi, bluetooth, wired -----------------
    const Rect conn = cellRect(0, 0, 2, 2);
    add(innerButton(conn, 0, 0, 1, 1), kCcAirplane, 0);
    add(innerButton(conn, 1, 0, 1, 1), kCcWifi, 1);
    add(innerButton(conn, 0, 1, 1, 1), kCcBluetooth, 2);
    add(innerButton(conn, 1, 1, 1, 1), kCcWired, 3);

    // --- media 2x2: title across the top, three transport buttons below ----
    const Rect media = cellRect(2, 0, 2, 2);
    const int btn = std::min(metrics::kCcButton * scale / 100, (media.w - 2 * gap) / 3);
    const int stripY = media.bottom() - gap - btn;
    for (int i = 0; i < 3; ++i) {
        const int cx = media.x + gap + i * ((media.w - 2 * gap) / 3) +
                       (media.w - 2 * gap) / 6;
        add(Rect{cx - btn / 2, stripY, btn, btn}, kCcMediaPrev + i, i);
    }

    // --- brightness and volume 1x2 pills ----------------------------------
    add(cellRect(2, 2, 1, 2), kCcBrightness, 0, true);
    add(cellRect(3, 2, 1, 2), kCcVolume, 0, true);

    // --- the 1x1 toggles and the bottom row -------------------------------
    add(cellRect(0, 2, 1, 1), kCcDnd);
    add(cellRect(1, 2, 1, 1), kCcNight);
    add(cellRect(0, 3, 2, 1), kCcShowDesktop);
    add(cellRect(0, 4, 1, 1), kCcLock);
    add(cellRect(1, 4, 1, 1), kCcScreenshot);
    // The bottom row is two half-width plates: the desktop <-> tablet / mobile mode
    // switch, and the Fluent shell's light/dark palette.
    add(cellRect(0, 5, 2, 1), kCcTablet);
    add(cellRect(2, 5, 2, 1), kCcLight);

    // --- launchers share the bottom row, which leaves exactly two slots ----
    for (int i = 0; i < kCcLauncherCount; ++i) {
        const Rect r = cellRect(2 + i, 4, 1, 1);
        add(r, kCcLauncherFirst + i, i);
    }
}

void Manager::updateCcHover(int px, int py) {
    int found = -1;
    for (size_t i = 0; i < ccControls.size(); ++i) {
        if (ccControls[i].rect.contains(px, py)) {
            found = int(i);
            break;
        }
    }
    if (found != ccHover) {
        ccHover = found;
        dirty = true;
    }
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------

void Manager::handleControlCenterPress(int x, int y, unsigned button) {
    if (button != Button1) {
        closeOverlays();
        return;
    }
    for (size_t i = 0; i < ccControls.size(); ++i) {
        const CcControl& c = ccControls[i];
        if (!c.rect.contains(x, y)) continue;

        // A press inside the panel is consumed, whatever it lands on.
        if (c.id == kCcBrightness || c.id == kCcVolume) {
            const SystemState& s = sysctl.state();
            const bool usable = c.id == kCcVolume ? s.audioPresent : s.brightnessUsable();
            if (!usable) return;
            ccDrag = int(i);
            // Jump to the pressed level, then track the pointer.
            ccDragValue = c.vertical
                              ? clampi((c.rect.bottom() - y) * 100 / std::max(1, c.rect.h), 0, 100)
                              : clampi((x - c.rect.x) * 100 / std::max(1, c.rect.w), 0, 100);
            if (c.id == kCcVolume) sysctl.setVolume(ccDragValue);
            else sysctl.setBrightness(ccDragValue);
            grabPointer();
            dirty = true;
            return;
        }
        activateCcControl(c.id);
        return;
    }
    // Clicking the panel's padding should not dismiss it.
    if (ccRect.contains(x, y)) return;
    closeOverlays();
}

void Manager::updateCcDrag(int x, int y) {
    if (ccDrag < 0 || size_t(ccDrag) >= ccControls.size()) return;
    const CcControl& c = ccControls[size_t(ccDrag)];
    const int value = c.vertical
                          ? clampi((c.rect.bottom() - y) * 100 / std::max(1, c.rect.h), 0, 100)
                          : clampi((x - c.rect.x) * 100 / std::max(1, c.rect.w), 0, 100);
    if (value == ccDragValue) return;
    ccDragValue = value;
    if (c.id == kCcVolume) sysctl.setVolume(value);
    else sysctl.setBrightness(value);
    dirty = true;
}

void Manager::endCcDrag() {
    if (ccDrag < 0) return;
    ccDrag = -1;
    ungrabPointer();
    dirty = true;
}

void Manager::launchCcApp(int index) {
    if (index < 0 || index >= int(ccLaunchers.size())) return;
    launchApp(ccLaunchers[size_t(index)]);
}

void Manager::activateCcControl(int id) {
    const SystemState& s = sysctl.state();
    switch (id) {
        case kCcAirplane: sysctl.setAirplane(!s.airplane); break;
        case kCcWifi:
            if (s.wifiPresent) sysctl.setWifi(!s.wifi);
            break;
        case kCcBluetooth:
            if (s.btPresent) sysctl.setBluetooth(!s.bt);
            break;
        case kCcWired:  // read-only status, like iOS' cellular tile
            break;
        case kCcMediaPrev: sysctl.mediaPrev(); break;
        case kCcMediaPlay: sysctl.mediaPlayPause(); break;
        case kCcMediaNext: sysctl.mediaNext(); break;
        case kCcBrightness:
            if (s.brightnessUsable()) {
                sysctl.setBrightness(s.brightness >= 100 ? 1 : s.brightness + 10);
            }
            break;
        case kCcVolume:
            if (s.audioPresent) sysctl.setMuted(!s.muted);
            break;
        case kCcDnd: {
            // Prefer the real notification daemon; when there is none, fall
            // back to suppressing our own urgency pulses so the switch still
            // does something observable.
            const bool next = !(s.dndPresent ? s.dnd : dnd);
            if (s.dndPresent) sysctl.setDnd(next);
            else dnd = next;
            if (next) {
                for (auto& cp : clients) cp->attentionPulse = 0.0;
            }
            break;
        }
        case kCcNight: sysctl.setNightLight(!s.nightLight); break;
        case kCcShowDesktop: toggleShowDesktop(); break;
        case kCcTablet: setTabletMode(!tabletMode); break;
        case kCcLight: setLightMode(!theme::isLight()); break;
        case kCcLock: sysctl.lockSession(); break;
        case kCcScreenshot: sysctl.screenshot(); break;
        default:
            if (id >= kCcLauncherFirst && id < kCcLauncherFirst + kCcLauncherCount) {
                launchCcApp(id - kCcLauncherFirst);
            }
            break;
    }
    dirty = true;
}

void Manager::toggleControlCenter() {
    if (ccOpen) {
        closeOverlays();
        return;
    }
    if (contextOpen || startOpen || taskViewOpen || altTabOpen) closeOverlays();
    ccOpen = true;
    ccHover = -1;
    ccDrag = -1;
    // Resolve the launcher row once per open: it comes from the .desktop scan,
    // which is far too slow to run while a frame is being painted.
    ccLaunchers.clear();
    struct Want {
        const char* fallback;  // used when no .desktop entry matched
        const char* wmClass;
        const char* nameMatch;  // lower-case substring of Name= or WM_CLASS
    };
    static const Want kWants[kCcLauncherCount] = {
        {"nautilus", "nautilus", "files"},
        {"xterm", "konsole", "terminal"},
    };
    for (const Want& want : kWants) {
        std::string found;
        for (const AppEntry& e : apps) {
            const bool classHit = !e.wmClass.empty() && e.wmClass == want.wmClass;
            const bool nameHit = containsFold(e.searchKey, want.nameMatch);
            if (classHit || nameHit) {
                found = e.exec;
                break;
            }
        }
        if (found.empty()) found = want.fallback;
        ccLaunchers.push_back(found);
    }
    layoutControlCenter();
    sysctl.requestRefresh();
    grabPointer();
    XGrabKeyboard(dpy, root, False, GrabModeAsync, GrabModeAsync, CurrentTime);
    dirty = true;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void Manager::drawControlCenter() {
    const double eased = fluentEase(ccAnim);
    if (eased <= 0.001) return;
    const float a = float(eased);
    if (ccRect.empty()) layoutControlCenter();

    // The panel grows out of the clock rather than simply appearing: every rect
    // is scaled about the point under the pointer, and the whole thing fades in
    // with it. Hit testing still uses the unscaled layout in ccControls, so a
    // click lands correctly even mid-animation.
    const float zoom = 0.86f + 0.14f * float(eased);
    const float ax = float(ccAnchorX);
    const float ay = float(ccAnchorY);
    const auto grow = [&](const Rect& r) {
        const float cx = float(r.x + r.w / 2), cy = float(r.y + r.h / 2);
        const float w = float(r.w) * zoom, h = float(r.h) * zoom;
        const Rect out{int(std::lround(cx - w / 2 + (ax - cx) * (1.f - zoom))),
                        int(std::lround(cy - h / 2 + (ay - cy) * (1.f - zoom))),
                        int(std::lround(w)), int(std::lround(h))};
        return out;
    };

    // iOS blurs and darkens whatever is behind the panel.
    comp.drawRect(Rect{0, 0, screenW, screenH}, 0.f, theme::kCcBackdrop, a);
    // The panel itself is the taskbar's acrylic verbatim -- same tint, alpha,
    // border and saturate() amount (draw.cpp's drawTaskbar) -- so the flyout is
    // visibly the same pane of glass the bar is, just rounded and floating.
    comp.drawAcrylic(grow(ccRect), float(metrics::kCcPanelRadius) * zoom, theme::kTaskbarTint,
                     theme::kTaskbarTintOpacity, theme::kShellLine, a, theme::kTaskbarSaturate);

    const int scale = ccScale();
    const int cell = metrics::kCcCell * scale / 100;
    const int gap = std::max(2, metrics::kCcGap * scale / 100);
    const SystemState& s = sysctl.state();

    const auto cellRect = [&](int col, int row, int colspan, int rowspan) {
        return Rect{ccRect.x + metrics::kCcPad * scale / 100 + col * (cell + gap),
                    ccRect.y + metrics::kCcPad * scale / 100 + row * (cell + gap),
                    colspan * cell + (colspan - 1) * gap, rowspan * cell + (rowspan - 1) * gap};
    };
    const auto controlRect = [&](int id, int slot) -> Rect {
        for (const CcControl& c : ccControls) {
            if (c.id == id && c.slot == slot) return c.rect;
        }
        return Rect();
    };
    // The eased 0..1 hover amount of a control, so tiles light up and fade back
    // smoothly instead of snapping.
    const auto hoverAmt = [&](int id, int slot) -> float {
        for (size_t i = 0; i < ccControls.size(); ++i) {
            if (ccControls[i].id == id && ccControls[i].slot == slot)
                return i < ccHoverFade.size() ? float(ccHoverFade[i]) : 0.0f;
        }
        return 0.0f;
    };
    // A rounded module plate (the 2x2 tiles, the 1x2 tile and the sliders).
    const auto plateC = [&](const Rect& r, const Color& fill, int radiusPx) {
        const Rect g = grow(r);
        comp.drawRect(g, float(radiusPx) * zoom, fill, a);
    };
    const auto plate = [&](const Rect& r, float hot) {
        plateC(r, mixColor(theme::kCcTile, theme::kCcTileHover, hot),
               metrics::kCcTileRadius * scale / 100);
    };
    // A circular control. For a 1x1 module the circle *is* the whole plate; for
    // the connectivity tile it is one of the four inner spots. Drawing a rounded
    // square underneath a circle is what made the corners peek out, so there is
    // deliberately no separate tile here.
    const auto circle = [&](const Rect& r, const Color& fill) {
        const Rect g = grow(r);
        comp.drawRect(g, float(std::min(g.w, g.h)) * 0.5f, fill, a);
    };
    const auto controlFill = [&](bool on, bool enabled, float hot) {
        if (on) return theme::kCcActive;
        // Light mode: every round control takes the plate fill, so the launchers
        // and the 1x1 toggles share one background with the wide plates (tablet
        // mode, light mode, show desktop) instead of a white wash sitting beside
        // a dark tile. Dark mode keeps its two-tone look exactly as it was.
        if (theme::isLight())
            return mixColor(theme::kCcTile, theme::kCcTileHover, hot);
        if (!enabled) return theme::kCcDisabled;
        return mixColor(theme::kCcControlOff, theme::kCcControlHover, hot);
    };
    const auto glyph = [&](const Rect& r, const char* icon, bool on, bool enabled) {
        const float opacity = !enabled ? theme::kCcGlyphOff.a
                                       : (on ? theme::kCcActiveGlyph.a : theme::kCcGlyph.a);
        const int d = int(r.w * 0.50);
        const Rect box = grow(Rect{r.x + (r.w - d) / 2, r.y + (r.h - d) / 2, d, d});
        if (drawAppIcon(box, icon, icon, 0.f, opacity, IconTheme::Shell)) return;
        // No icon in the theme: a filled dot still communicates state.
        const int dot = std::max(3, int(box.w * 0.4));
        comp.drawRect(Rect{box.x + (box.w - dot) / 2, box.y + (box.h - dot) / 2, dot, dot},
                      float(dot) * 0.5f, enabled && on ? theme::kCcActiveGlyph : theme::kCcGlyph,
                      a * opacity);
    };

    // --- connections ------------------------------------------------------
    {
        const Rect m = cellRect(0, 0, 2, 2);
        plate(m, std::max({hoverAmt(kCcAirplane, 0), hoverAmt(kCcWifi, 1),
                           hoverAmt(kCcBluetooth, 2), hoverAmt(kCcWired, 3)}));
        const struct {
            int id;
            int slot;
            const char* icon;
            bool on;
            bool enabled;
        } rows[] = {
            {kCcAirplane, 0, "lucide-plane", s.airplane, s.wifiPresent || s.btPresent},
            {kCcWifi, 1, "lucide-wifi", s.wifi, s.wifiPresent},
            {kCcBluetooth, 2, "lucide-bluetooth", s.bt, s.btPresent},
            {kCcWired, 3, "lucide-cable", s.wired, s.wiredPresent},
        };
        for (const auto& r : rows) {
            const Rect b = controlRect(r.id, r.slot);
            circle(b, controlFill(r.on, r.enabled, hoverAmt(r.id, r.slot)));
            glyph(b, r.icon, r.on, r.enabled);
        }
    }

    // --- media ------------------------------------------------------------
    {
        const Rect m = cellRect(2, 0, 2, 2);
        plate(m, hoverAmt(kCcMediaPlay, 1));
        // Title and artist live in the space above the transport strip, not
        // vertically centred over the whole tile, so they never collide with
        // the buttons.
        const int btn = std::min(metrics::kCcButton * scale / 100, (m.w - 2 * gap) / 3);
        const int stripTop = m.bottom() - gap - btn - gap;
        const Rect area{m.x + gap, m.y + gap, m.w - 2 * gap, std::max(0, stripTop - m.y - gap)};
        const bool has = s.mediaPresent;
        const std::string line = !has ? std::string("No media")
                                      : (s.mediaTitle.empty() ? std::string("Not playing")
                                                              : s.mediaTitle);
        const int titlePx = std::max(10, int(m.h * 0.17));
        const int artistPx = std::max(9, int(m.h * 0.13));
        drawTextCentered(ccEllipsize(text, line, titlePx, area.w), titlePx, Weight::Medium,
                         theme::kCcLabel, grow(Rect{area.x, area.y, area.w, area.h * 3 / 5}));
        if (has && !s.mediaArtist.empty()) {
            const std::string who = ccEllipsize(text, s.mediaArtist, artistPx, area.w);
            drawTextCentered(who, artistPx, Weight::Regular, theme::kCcGlyphOff,
                             grow(Rect{area.x, area.y + area.h * 3 / 5, area.w, area.h * 2 / 5}));
        }
        const char* prev = "lucide-skip-back";
        const char* play = s.playing ? "lucide-pause" : "lucide-play";
        const char* next = "lucide-skip-forward";
        glyph(controlRect(kCcMediaPrev, 0), prev, false, has);
        glyph(controlRect(kCcMediaPlay, 1), play, false, has);
        glyph(controlRect(kCcMediaNext, 2), next, false, has);
    }

    // --- brightness / volume pills ---------------------------------------
    // iOS anchors the glyph to the bottom of the pill and grows the fill from
    // there. The fill is the whole pill clipped to its own height, so it follows
    // the pill's rounded corners at every level instead of being a second,
    // slightly different shape.
    const auto slider = [&](int id, const char* icon, int value, bool enabled) {
        const Rect m = controlRect(id, 0);
        if (m.empty()) return;
        const int radius = metrics::kCcSliderRadius * scale / 100;
        plateC(m, mixColor(theme::kCcTile, theme::kCcTileHover, hoverAmt(id, 0)), radius);
        const int level = std::max(0, std::min(100, value));
        const int fillH = m.h * level / 100;
        const Color fill = enabled ? theme::kCcSliderFill : Color{1.f, 1.f, 1.f, 0.26f};
        // One clipped draw of the whole pill, so the fill follows the pill's
        // rounded corners at every height and never double-blends into a seam.
        const int fillTop = m.bottom() - fillH;
        const int clipY = int(std::lround(ay + (float(fillTop) - ay) * zoom));
        if (fillH > 0) {
            // A full pill is drawn unclipped so its rounded top is exact; any
            // partial level gets a straight top edge at the clip line.
            const int top = level >= 100 ? Compositor::kNoClip : clipY;
            comp.drawRect(grow(m), float(radius) * zoom, fill, a, top);
        }
        const int d = int(m.w * 0.44);
        const int inset = std::max(2, int(d * 0.18));
        const Rect box = grow(Rect{m.x + (m.w - d) / 2, m.bottom() - d - inset, d, d});
        if (enabled) {
            // The glyph inverts once the white fill reaches it, which is exactly
            // how the icon reads in iOS.
            const std::string dark = std::string(icon) + "-dark";
            const bool reached = clipY <= box.y + box.h / 2;
            const char* chosen = reached ? dark.c_str() : icon;
            if (drawAppIcon(box, chosen, chosen, 0.f, 1.0f, IconTheme::Shell)) return;
        }
        const int dot = std::max(3, box.w / 3);
        comp.drawRect(Rect{box.x + (box.w - dot) / 2, box.y + (box.h - dot) / 2, dot, dot},
                      float(dot) * 0.5f, enabled ? theme::kCcGlyph : theme::kCcGlyphOff, a);
    };
    slider(kCcBrightness, "lucide-sun",
           ccDrag == ccControlIndex(kCcBrightness, 0) ? ccDragValue : s.brightness,
           s.brightnessUsable());
    slider(kCcVolume, "lucide-volume-2",
           ccDrag == ccControlIndex(kCcVolume, 0) ? ccDragValue : (s.muted ? 0 : s.volume),
           s.audioPresent);

    // --- 1x1 circles and the 1x2 tile ------------------------------------
    {
        const Rect m = cellRect(0, 2, 1, 1);
        const bool on = s.dndPresent ? s.dnd : dnd;
        circle(m, controlFill(on, true, hoverAmt(kCcDnd, 0)));
        glyph(m, "lucide-bell-off", on, true);
    }
    {
        const Rect m = cellRect(1, 2, 1, 1);
        circle(m, controlFill(s.nightLight, s.nightPresent, hoverAmt(kCcNight, 0)));
        glyph(m, "lucide-moon", s.nightLight, s.nightPresent);
    }
    {
        // "Show desktop" is the 2-wide module iOS gives to Screen Mirroring: a
        // rounded plate that turns systemBlue while the desktop is showing.
        const Rect m = cellRect(0, 3, 2, 1);
        const bool on = showingDesktop;
        plateC(m, on ? theme::kCcActive
                     : mixColor(theme::kCcTile, theme::kCcTileHover, hoverAmt(kCcShowDesktop, 0)),
               metrics::kCcTileRadius * scale / 100);
        const int d = int(m.h * 0.34);
        // Five pixels further left than the grid gap puts it, so the row does not
        // read as pushed into its tile. The label follows the icon, box.right()
        // moves with it.
        const Rect box = grow(Rect{m.x + gap - 5, m.y + (m.h - d) / 2, d, d});
        drawAppIcon(box, "lucide-monitor", "lucide-monitor", 0.f, 1.0f, IconTheme::Shell);
        drawTextAt("Show desktop", int(m.h * 0.20), Weight::Medium, theme::kCcLabel,
                   box.right() + gap, m.y + (m.h - int(m.h * 0.20) * 3 / 2) / 2);
    }
    {
        const Rect m = cellRect(0, 4, 1, 1);
        circle(m, controlFill(false, s.lockPresent, hoverAmt(kCcLock, 0)));
        glyph(m, "lucide-lock", false, s.lockPresent);
    }
    {
        const Rect m = cellRect(1, 4, 1, 1);
        circle(m, controlFill(false, s.shotPresent, hoverAmt(kCcScreenshot, 0)));
        glyph(m, "lucide-camera", false, s.shotPresent);
    }

    // --- launcher row -----------------------------------------------------
    static const char* const kLauncherIcons[kCcLauncherCount] = {"lucide-folder", "lucide-terminal"};
    for (int i = 0; i < kCcLauncherCount; ++i) {
        const Rect m = cellRect(2 + i, 4, 1, 1);
        const bool have = i < int(ccLaunchers.size()) && !ccLaunchers[size_t(i)].empty();
        circle(m, controlFill(false, have, hoverAmt(kCcLauncherFirst + i, i)));
        if (!have) continue;
        const int d = int(m.h * 0.46);
        const Rect box = grow(Rect{m.x + (m.w - d) / 2, m.y + (m.h - d) / 2, d, d});
        if (!drawAppIcon(box, kLauncherIcons[i], kLauncherIcons[i], 0.f, a, IconTheme::Shell)) {
            drawAppTile(box, "?", float(box.w) * 0.30f, theme::kCcActive, false);
        }
    }

// --- tablet / mobile mode, and light mode ----------------------------------
    // Two wide plates sharing the bottom row. Both turn systemBlue while they are
    // on, the way an iOS toggle reads. Both glyphs are Lucide, like every other
    // icon on this surface, so nothing here is drawn by hand any more.
    {
        const auto switchPlate = [&](const Rect& m, int id, bool on) {
            plateC(m, on ? theme::kCcActive
                         : mixColor(theme::kCcTile, theme::kCcTileHover, hoverAmt(id, 0)),
                    metrics::kCcTileRadius * scale / 100);
            const int d = int(m.h * 0.34);
            const Rect box = grow(Rect{m.x + gap, m.y + (m.h - d) / 2, d, d});
            return box;
        };
        // A switch glyph, in the same two whites the round controls use.
        const auto switchGlyph = [&](const Rect& box, const char* icon, bool on) {
            drawAppIcon(box, icon, icon, 0.f,
                        on ? theme::kCcActiveGlyph.a : theme::kCcGlyph.a, IconTheme::Shell);
        };

        {
            const Rect m = cellRect(0, 5, 2, 1);
            const bool on = tabletMode;
            const Rect box = switchPlate(m, kCcTablet, on);
            switchGlyph(box, "lucide-tablet", on);
            const std::string label = "Tablet mode";
            const int px = int(m.h * 0.20);
            drawTextAt(label, px, Weight::Medium, theme::kCcLabel, box.right() + gap,
                       m.y + (m.h - px * 3 / 2) / 2);
        }
        {
            const Rect m = cellRect(2, 5, 2, 1);
            const bool on = theme::isLight();
            const Rect box = switchPlate(m, kCcLight, on);
            // Both halves of the toggle are Lucide, and the glyph names the mode the
            // shell is actually in -- moon while it is dark, sun once it is light --
            // rather than always advertising the one you would switch to.
            switchGlyph(box, on ? "lucide-sun" : "lucide-moon", on);
            const std::string label = "Light mode";
            const int px = int(m.h * 0.20);
            drawTextAt(label, px, Weight::Medium, theme::kCcLabel, box.right() + gap,
                       m.y + (m.h - px * 3 / 2) / 2);
        }
    }
}

}  // namespace wm
