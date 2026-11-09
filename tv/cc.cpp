// The Control Centre: the desktop shell's iOS quick-settings panel, ported to
// the TV shell and driven by a d-pad instead of a pointer.
//
// The layout is the desktop's, module for module -- a connections tile, a media
// tile, brightness and volume pills, and a row of round controls -- because the
// geometry *is* what makes the panel read as Control Centre rather than as a
// list of switches. A module occupies whole cells of a 4x6 grid and every
// interactive part is registered as a CcControl, so focus movement is a scan of
// rectangles rather than per-widget hit testing.
//
//         0              1              2              3
//    0  [ connections 2x2          ] [ media 2x2               ]
//    1  [                           ] [                        ]
//    2  [ dnd 1x1 ] [ night 1x1 ] [ brightness 1x2 ] [ volume 1x2 ]
//    3  [ refresh 1x2              ] [  (sliders still)         ]
//    4  [ home ] [ apps ] [ recent ] [ settings ]
//    5  [ quit 2x1                ] [ status 2x1              ]
//
// The bottom two rows are the TV shell's own: the launcher's destinations as
// shortcuts, the two housekeeping actions, and a read-only status plate, since
// a TV has no taskbar, no desktop to hide and no session to lock.
//
// Everything that touches the outside world goes through wm::SystemControls,
// which probes asynchronously (see src/wm/sysctl.h); this file only reads the
// cached state and draws it, and greys out what the machine cannot do.
#include "manager.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <string>

namespace tv {
namespace {

enum CcId {
    kCcAirplane = 1,
    kCcWifi,
    kCcBluetooth,
    kCcWired,
    kCcMediaPrev,
    kCcMediaPlay,
    kCcMediaNext,
    kCcBrightness,
    kCcVolume,
    kCcDnd,
    kCcNight,
    kCcRefresh,
    kCcHome,
    kCcApps,
    kCcRecent,
    kCcSettings,
    kCcQuit,
};

// Truncates `s` until it fits `maxW`, ending in an ellipsis.
std::string ellipsize(wm::Text& text, const std::string& s, int px, int maxW) {
    if (maxW <= 0 || s.empty()) return std::string();
    if (text.measure(s, px) <= maxW) return s;
    std::string out = s;
    while (!out.empty() && text.measure(out + "\xE2\x80\xA6", px) > maxW) {
        out.pop_back();
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80)
            out.pop_back();
    }
    return out + "\xE2\x80\xA6";
}

}  // namespace

int Manager::ccScale() const {
    const int baseW = 2 * wm::metrics::kCcPad + wm::metrics::kCcCols * wm::metrics::kCcCell +
                      (wm::metrics::kCcCols - 1) * wm::metrics::kCcGap;
    const int baseH = 2 * wm::metrics::kCcPad + wm::metrics::kCcRows * wm::metrics::kCcCell +
                      (wm::metrics::kCcRows - 1) * wm::metrics::kCcGap;
    const int availW = std::max(160, screenW_ - std::max(48, screenW_ / 8));
    const int availH = std::max(160, screenH_ - std::max(48, screenH_ / 9));
    // The desktop never scales the panel past 100% (it hangs off a taskbar); a
    // TV has the whole screen for it, so it grows with the output.
    int scale = 150;
    scale = std::min(scale, availW * 100 / baseW);
    scale = std::min(scale, availH * 100 / baseH);
    return std::max(40, scale);
}

void Manager::layoutControlCenter() {
    ccControls_.clear();
    const int scale = ccScale();
    const int cell = wm::metrics::kCcCell * scale / 100;
    const int gap = std::max(2, wm::metrics::kCcGap * scale / 100);
    const int pad = wm::metrics::kCcPad * scale / 100;
    const int cols = wm::metrics::kCcCols;
    const int rows = wm::metrics::kCcRows;

    const int w = 2 * pad + cols * cell + (cols - 1) * gap;
    const int h = 2 * pad + rows * cell + (rows - 1) * gap;
    ccRect_ = wm::Rect((screenW_ - w) / 2, (screenH_ - h) / 2, w, h);

    const auto cellRect = [&](int col, int row, int colspan, int rowspan) {
        return wm::Rect(ccRect_.x + pad + col * (cell + gap),
                        ccRect_.y + pad + row * (cell + gap),
                        colspan * cell + (colspan - 1) * gap,
                        rowspan * cell + (rowspan - 1) * gap);
    };
    // The circular button inside a module, centred in one of its quarters.
    const auto innerButton = [&](const wm::Rect& module, int col, int row) {
        const int qw = (module.w - gap) / 2;
        const int qh = (module.h - gap) / 2;
        const int d = std::min(wm::metrics::kCcButton * scale / 100, std::min(qw, qh));
        const int cx = module.x + col * (qw + gap) + qw / 2;
        const int cy = module.y + row * (qh + gap) + qh / 2;
        return wm::Rect(cx - d / 2, cy - d / 2, d, d);
    };
    const auto add = [&](const wm::Rect& r, int id, int slot = 0, bool vertical = false) {
        CcControl c;
        c.rect = r;
        c.id = id;
        c.slot = slot;
        c.vertical = vertical;
        ccControls_.push_back(c);
    };

    // --- connections 2x2: airplane, wifi, bluetooth, wired -----------------
    const wm::Rect conn = cellRect(0, 0, 2, 2);
    add(innerButton(conn, 0, 0), kCcAirplane, 0);
    add(innerButton(conn, 1, 0), kCcWifi, 1);
    add(innerButton(conn, 0, 1), kCcBluetooth, 2);
    add(innerButton(conn, 1, 1), kCcWired, 3);

    // --- media 2x2: title across the top, transport buttons below ----------
    const wm::Rect media = cellRect(2, 0, 2, 2);
    const int btn = std::min(wm::metrics::kCcButton * scale / 100, (media.w - 2 * gap) / 3);
    const int stripY = media.bottom() - gap - btn;
    for (int i = 0; i < 3; ++i) {
        const int cx = media.x + gap + i * ((media.w - 2 * gap) / 3) +
                       (media.w - 2 * gap) / 6;
        add(wm::Rect(cx - btn / 2, stripY, btn, btn), kCcMediaPrev + i, i);
    }

    // --- brightness and volume 1x2 pills ----------------------------------
    add(cellRect(2, 2, 1, 2), kCcBrightness, 0, true);
    add(cellRect(3, 2, 1, 2), kCcVolume, 0, true);

    // --- toggles and the TV rows ------------------------------------------
    add(cellRect(0, 2, 1, 1), kCcDnd);
    add(cellRect(1, 2, 1, 1), kCcNight);
    add(cellRect(0, 3, 2, 1), kCcRefresh);

    const int shortcuts[4] = {kCcHome, kCcApps, kCcRecent, kCcSettings};
    for (int i = 0; i < 4; ++i) add(cellRect(i, 4, 1, 1), shortcuts[i], i);

    add(cellRect(0, 5, 2, 1), kCcQuit);

    ccFocus_ = std::clamp(ccFocus_, 0, std::max(0, int(ccControls_.size()) - 1));
}

int Manager::ccControlIndex(int id, int slot) const {
    for (size_t i = 0; i < ccControls_.size(); ++i) {
        if (ccControls_[i].id == id && ccControls_[i].slot == slot) return int(i);
    }
    return -1;
}

wm::IconTex Manager::ccIcon(const std::string& name) {
    const auto it = ccIcons_.find(name);
    if (it != ccIcons_.end()) return it->second;
    const int px = std::clamp(ccScale() / 2, 16, 56);
    const wm::IconTex tex = icons_.get(name, px, wm::IconTheme::Shell);
    ccIcons_.emplace(name, tex);
    return tex;
}

void Manager::toggleControlCenter() {
    ccOpen_ = !ccOpen_;
    if (ccOpen_) {
        layoutControlCenter();
        ccFocus_ = 0;
        sysctl_.requestRefresh();
    }
    // The shell only polls the machine while the panel is open, so the timer
    // has to switch between its idle sleep and its polling cadence here.
    scheduleTick();
    dirty_ = true;
    requestFrame();
}

void Manager::moveCcFocus(int dx, int dy) {
    if (ccControls_.empty()) return;
    const CcControl& current = ccControls_[size_t(ccFocus_)];

    // A slider is its own axis: up/down changes the level instead of leaving
    // the control, which is how a remote user expects a volume pill to behave.
    if (current.vertical && dy != 0) {
        adjustCcControl(current.id, dy < 0 ? 5 : -5);
        return;
    }

    const int cx = current.rect.x + current.rect.w / 2;
    const int cy = current.rect.y + current.rect.h / 2;
    int best = -1;
    long bestScore = LONG_MAX;
    for (size_t i = 0; i < ccControls_.size(); ++i) {
        if (int(i) == ccFocus_) continue;
        const wm::Rect& r = ccControls_[i].rect;
        const int ox = r.x + r.w / 2;
        const int oy = r.y + r.h / 2;
        long primary = 0;
        long secondary = 0;
        if (dx > 0) {
            if (ox <= cx + 4) continue;
            primary = ox - cx;
            secondary = std::abs(oy - cy);
        } else if (dx < 0) {
            if (ox >= cx - 4) continue;
            primary = cx - ox;
            secondary = std::abs(oy - cy);
        } else if (dy > 0) {
            if (oy <= cy + 4) continue;
            primary = oy - cy;
            secondary = std::abs(ox - cx);
        } else if (dy < 0) {
            if (oy >= cy - 4) continue;
            primary = cy - oy;
            secondary = std::abs(ox - cx);
        } else {
            return;
        }
        const long score = primary * 4 + secondary * 3;
        if (score < bestScore) {
            bestScore = score;
            best = int(i);
        }
    }
    if (best < 0) return;
    ccFocus_ = best;
    dirty_ = true;
    requestFrame();
}

void Manager::adjustCcControl(int id, int delta) {
    const wm::SystemState& s = sysctl_.state();
    if (id == kCcBrightness) {
        if (!s.brightnessUsable()) return;
        sysctl_.setBrightness(std::clamp(s.brightness + delta, 0, 100));
    } else if (id == kCcVolume) {
        if (!s.audioPresent) return;
        sysctl_.setVolume(std::clamp((s.muted ? 0 : s.volume) + delta, 0, 100));
    } else {
        return;
    }
    dirty_ = true;
    requestFrame();
}

void Manager::activateCcFocus() {
    if (ccControls_.empty()) return;
    activateCcControl(ccControls_[size_t(ccFocus_)].id);
}

void Manager::activateCcControl(int id) {
    const wm::SystemState& s = sysctl_.state();
    switch (id) {
        case kCcAirplane: sysctl_.setAirplane(!s.airplane); break;
        case kCcWifi:
            if (s.wifiPresent) sysctl_.setWifi(!s.wifi);
            break;
        case kCcBluetooth:
            if (s.btPresent) sysctl_.setBluetooth(!s.bt);
            break;
        case kCcWired:  // read-only status, like iOS' cellular tile
            break;
        case kCcMediaPrev: sysctl_.mediaPrev(); break;
        case kCcMediaPlay: sysctl_.mediaPlayPause(); break;
        case kCcMediaNext: sysctl_.mediaNext(); break;
        case kCcBrightness:
            if (s.brightnessUsable()) sysctl_.setBrightness(s.brightness >= 100 ? 1 : s.brightness + 10);
            break;
        case kCcVolume:
            if (s.audioPresent) sysctl_.setMuted(!s.muted);
            break;
        case kCcDnd:
            if (s.dndPresent) sysctl_.setDnd(!s.dnd);
            break;
        case kCcNight: sysctl_.setNightLight(!s.nightLight); break;
        case kCcRefresh: reloadApps(); break;
        case kCcHome:
        case kCcApps:
        case kCcRecent:
        case kCcSettings: {
            const Section target = id == kCcHome     ? Section::Home
                                   : id == kCcApps   ? Section::Apps
                                   : id == kCcRecent ? Section::Recent
                                                     : Section::Settings;
            setHome(true);
            setSection(target);
            zone_ = Zone::Content;
            if (ccOpen_) toggleControlCenter();
            break;
        }
        case kCcQuit: stop(); break;
        default: break;
    }
    dirty_ = true;
    requestFrame();
}

void Manager::drawControlCenter() {
    const double eased = std::clamp(ccSpring_.value, 0.0, 1.0);
    if (eased <= 0.001) return;
    const float a = float(eased);
    if (ccControls_.empty()) return;

    const int scale = ccScale();
    const int cell = wm::metrics::kCcCell * scale / 100;
    const int gap = std::max(2, wm::metrics::kCcGap * scale / 100);
    const int pad = wm::metrics::kCcPad * scale / 100;
    const int panelRadius = wm::metrics::kCcPanelRadius * scale / 100;
    const int tileRadius = wm::metrics::kCcTileRadius * scale / 100;
    const int sliderRadius = wm::metrics::kCcSliderRadius * scale / 100;
    // The panel slides up those last few pixels as it fades in. Hit testing uses
    // the settled layout, so a press lands correctly even mid-animation.
    const int slide = int(std::lround((1.0 - eased) * 16.0));

    const wm::Rect panel(ccRect_.x, ccRect_.y + slide, ccRect_.w, ccRect_.h);
    // iOS blurs and darkens whatever is behind the panel.
    compositor_.drawRect(wm::Rect(0, 0, screenW_, screenH_), 0.0f, wm::theme::kCcBackdrop, a);
    const wm::Color panelTop = wm::rgb(0x191A1F);
    const wm::Color panelBottom = wm::rgb(0x0E0E12);
    compositor_.drawRect(panel.inflated(1), float(panelRadius + 1),
                         wm::rgb(0xFFFFFF, 0.07f), a);
    compositor_.drawGradient(panel, float(panelRadius), panelTop, panelBottom, a);
    // The panel's own colour at a given height. The focus ring is drawn by
    // painting the ring white and then punching this colour back out of its
    // middle: a translucent control fill would let a white ring show through,
    // which is exactly the wash that made the focused circle read as a blob.
    const auto panelAt = [&](int y) {
        const double t = panel.h > 1 ? double(y - panel.y) / double(panel.h) : 0.0;
        return wm::mixColor(panelTop, panelBottom, float(std::clamp(t, 0.0, 1.0)));
    };

    const auto moved = [&](const wm::Rect& r) {
        return wm::Rect(r.x, r.y + slide, r.w, r.h);
    };
    const auto cellRect = [&](int col, int row, int colspan, int rowspan) {
        return moved(wm::Rect(ccRect_.x + pad + col * (cell + gap),
                              ccRect_.y + pad + row * (cell + gap),
                              colspan * cell + (colspan - 1) * gap,
                              rowspan * cell + (rowspan - 1) * gap));
    };
    const auto rectFor = [&](int id) -> wm::Rect {
        for (const CcControl& c : ccControls_)
            if (c.id == id) return moved(c.rect);
        return wm::Rect();
    };
    const int focusId = ccControls_[size_t(ccFocus_)].id;
    const auto hot = [&](int id) { return focusId == id; };
    // A 2px white outline, painted before the control's own fill so the fill
    // sits on top of it and only the stroke stays visible.
    const auto ring = [&](const wm::Rect& r, float radius) {
        compositor_.drawRect(r.inflated(3), radius + 3.0f, wm::rgb(0xFFFFFF, 0.92f), a);
        compositor_.drawRect(r.inflated(1), radius + 1.0f, panelAt(r.y + r.h / 2), a);
    };
    const auto plate = [&](const wm::Rect& r, int radius) {
        compositor_.drawRect(r.inflated(1), float(radius + 1), wm::rgb(0xFFFFFF, 0.05f), a);
        compositor_.drawRect(r, float(radius), wm::theme::kCcTile, a);
    };
    const auto circle = [&](const wm::Rect& r, const wm::Color& fill) {
        compositor_.drawRect(r, float(std::min(r.w, r.h)) * 0.5f, fill, a);
    };
    const auto controlFill = [](bool on, bool enabled) {
        if (on) return wm::theme::kCcActive;
        if (!enabled) return wm::theme::kCcDisabled;
        return wm::theme::kCcControlOff;
    };
    const auto glyph = [&](const wm::Rect& r, const char* name, bool on, bool enabled) {
        const int d = int(r.w * 0.5);
        const wm::Rect box(r.x + (r.w - d) / 2, r.y + (r.h - d) / 2, d, d);
        const wm::IconTex tex = ccIcon(name);
        const float opacity = enabled ? (on ? 1.0f : 0.88f) : 0.30f;
        if (tex.valid()) {
            compositor_.drawTex(tex.tex, box, 0.0f,
                                on ? wm::theme::kCcActiveGlyph : wm::theme::kCcGlyph,
                                a * opacity, true, true);
            return;
        }
        // No icon in the theme: a filled dot still communicates state.
        const int dot = std::max(3, box.w / 3);
        compositor_.drawRect(
            wm::Rect(box.x + (box.w - dot) / 2, box.y + (box.h - dot) / 2, dot, dot),
            float(dot) * 0.5f,
            enabled ? wm::theme::kCcGlyph : wm::theme::kCcGlyphOff, a * opacity);
    };

    const wm::SystemState& s = sysctl_.state();

    // --- connections ------------------------------------------------------
    {
        plate(cellRect(0, 0, 2, 2), tileRadius);
        const struct {
            int id;
            const char* icon;
            bool on;
            bool enabled;
        } rows[] = {
            {kCcAirplane, "lucide-plane", s.airplane, s.wifiPresent || s.btPresent},
            {kCcWifi, "lucide-wifi", s.wifi, s.wifiPresent},
            {kCcBluetooth, "lucide-bluetooth", s.bt, s.btPresent},
            {kCcWired, "lucide-cable", s.wired, s.wiredPresent},
        };
        for (const auto& row : rows) {
            const wm::Rect b = rectFor(row.id);
            if (hot(row.id)) ring(b, float(std::min(b.w, b.h)) * 0.5f);
            circle(b, controlFill(row.on, row.enabled));
            glyph(b, row.icon, row.on, row.enabled);
        }
    }

    // --- media ------------------------------------------------------------
    {
        const wm::Rect m = cellRect(2, 0, 2, 2);
        plate(m, tileRadius);
        const wm::Rect first = rectFor(kCcMediaPrev);
        const int stripTop = first.y;
        const wm::Rect area(m.x + gap, m.y + gap, m.w - 2 * gap,
                            std::max(0, stripTop - m.y - 2 * gap));
        const std::string line = !s.mediaPresent
                                     ? std::string("No media")
                                     : (s.mediaTitle.empty() ? std::string("Not playing")
                                                             : s.mediaTitle);
        const int titlePx = std::max(10, int(m.h * 0.13));
        const int artistPx = std::max(9, int(m.h * 0.10));
        drawTextCentered(ellipsize(text_, line, titlePx, area.w), titlePx, wm::Weight::Medium,
                         wm::theme::kCcLabel,
                         wm::Rect(area.x, area.y, area.w, area.h * 3 / 5), a);
        if (s.mediaPresent && !s.mediaArtist.empty()) {
            const std::string who = ellipsize(text_, s.mediaArtist, artistPx, area.w);
            drawTextCentered(who, artistPx, wm::Weight::Regular, wm::theme::kCcGlyphOff,
                             wm::Rect(area.x, area.y + area.h * 3 / 5, area.w, area.h * 2 / 5),
                             a);
        }
        const char* prev = "lucide-skip-back";
        const char* play = s.playing ? "lucide-pause" : "lucide-play";
        const char* next = "lucide-skip-forward";
        const int ids[3] = {kCcMediaPrev, kCcMediaPlay, kCcMediaNext};
        const char* names[3] = {prev, play, next};
        for (int i = 0; i < 3; ++i) {
            const wm::Rect b = rectFor(ids[i]);
            if (hot(ids[i])) ring(b, float(std::min(b.w, b.h)) * 0.5f);
            glyph(b, names[i], false, s.mediaPresent);
        }
    }

    // --- brightness / volume pills ---------------------------------------
    // The glyph sits at the bottom of the pill and the fill grows from there,
    // and inverts (the -dark cut) once the white has reached it.
    const auto slider = [&](int id, const char* icon, int value, bool enabled) {
        const wm::Rect m = rectFor(id);
        if (m.empty()) return;
        if (hot(id)) ring(m, float(sliderRadius));
        plate(m, sliderRadius);
        const int level = std::clamp(value, 0, 100);
        const int fillH = m.h * level / 100;
        if (fillH > 0) {
            const wm::Color fill =
                enabled ? wm::theme::kCcSliderFill : wm::rgb(0xFFFFFF, 0.26f);
            compositor_.drawRect(m, float(sliderRadius), fill, a, m.bottom() - fillH);
        }
        const int d = int(m.w * 0.44);
        const int inset = std::max(2, int(d * 0.18));
        const wm::Rect box(m.x + (m.w - d) / 2, m.bottom() - d - inset, d, d);
        const bool reached = enabled && (m.bottom() - fillH) <= box.y + box.h / 2;
        const wm::IconTex tex = ccIcon(icon);
        if (tex.valid()) {
            // The glyph inverts once the white fill reaches it, which is how
            // iOS reads: white type on the empty part, dark on the filled part.
            compositor_.drawTex(tex.tex, box, 0.0f,
                                reached ? wm::rgb(0x111114) : wm::rgb(0xFFFFFF),
                                a * (enabled ? 1.0f : 0.30f), true, true);
            return;
        }
        const int dot = std::max(3, box.w / 3);
        compositor_.drawRect(
            wm::Rect(box.x + (box.w - dot) / 2, box.y + (box.h - dot) / 2, dot, dot),
            float(dot) * 0.5f, enabled ? wm::theme::kCcGlyph : wm::theme::kCcGlyphOff,
            a * (enabled ? 1.0f : 0.30f));
    };
    slider(kCcBrightness, "lucide-sun", s.brightness, s.brightnessUsable());
    slider(kCcVolume, "lucide-volume-2", s.muted ? 0 : s.volume, s.audioPresent);

    // --- dnd / night ------------------------------------------------------
    {
        const wm::Rect m = rectFor(kCcDnd);
        const bool on = s.dndPresent ? s.dnd : false;
        if (hot(kCcDnd)) ring(m, float(std::min(m.w, m.h)) * 0.5f);
        circle(m, controlFill(on, s.dndPresent));
        glyph(m, "lucide-bell-off", on, s.dndPresent);
    }
    {
        const wm::Rect m = rectFor(kCcNight);
        if (hot(kCcNight)) ring(m, float(std::min(m.w, m.h)) * 0.5f);
        circle(m, controlFill(s.nightLight, s.nightPresent));
        glyph(m, "lucide-moon", s.nightLight, s.nightPresent);
    }

    // --- refresh apps (2 wide) -------------------------------------------
    {
        const wm::Rect m = rectFor(kCcRefresh);
        if (hot(kCcRefresh)) ring(m, float(tileRadius));
        plate(m, tileRadius);
        const int d = int(m.h * 0.36);
        const wm::Rect box(m.x + gap, m.y + (m.h - d) / 2, d, d);
        const wm::IconTex tex = ccIcon("view-grid");
        if (tex.valid())
            compositor_.drawTex(tex.tex, box, 0.0f, wm::theme::kCcGlyph, a, true, true);
        const int px = int(m.h * 0.20);
        drawTextAt("Refresh apps", px, wm::Weight::Medium, wm::theme::kCcLabel,
                   box.right() + gap, m.y + (m.h - px * 3 / 2) / 2, a);
    }

    // --- launcher shortcuts (1x1 circles) --------------------------------
    {
        struct Shortcut {
            int id;
            const char* icon;
        };
        const Shortcut items[4] = {
            {kCcHome, "go-home"},
            {kCcApps, "view-grid"},
            {kCcRecent, "lucide-play"},
            {kCcSettings, "systemsettings"},
        };
        for (const Shortcut& item : items) {
            const wm::Rect m = rectFor(item.id);
            if (hot(item.id)) ring(m, float(std::min(m.w, m.h)) * 0.5f);
            circle(m, wm::theme::kCcControlOff);
            glyph(m, item.icon, false, true);
        }
    }

    // --- quit shell (2 wide) ---------------------------------------------
    {
        const wm::Rect m = rectFor(kCcQuit);
        if (hot(kCcQuit)) ring(m, float(tileRadius));
        plate(m, tileRadius);
        const int d = int(m.h * 0.36);
        const wm::Rect box(m.x + gap, m.y + (m.h - d) / 2, d, d);
        const wm::IconTex tex = ccIcon("lucide-power");
        if (tex.valid())
            compositor_.drawTex(tex.tex, box, 0.0f, wm::theme::kCcGlyph, a, true, true);
        const int px = int(m.h * 0.20);
        drawTextAt("Quit shell", px, wm::Weight::Medium, wm::theme::kCcLabel,
                   box.right() + gap, m.y + (m.h - px * 3 / 2) / 2, a);
    }

    // --- status plate: read-only, so focus never lands on it --------------
    {
        const wm::Rect m = cellRect(2, 5, 2, 1);
        plate(m, tileRadius);
        const int px = int(m.h * 0.18);
        const std::string line = std::to_string(screenW_) + "x" + std::to_string(screenH_) +
                                 "  \xC2\xB7  " + std::to_string(controllers_.count()) +
                                 (controllers_.count() == 1 ? " pad" : " pads");
        drawTextAt(ellipsize(text_, line, px, m.w - gap * 2), px, wm::Weight::Regular,
                   wm::theme::kCcGlyph, m.x + gap, m.y + (m.h - px * 3 / 2) / 2, a);
    }
}

}  // namespace tv
