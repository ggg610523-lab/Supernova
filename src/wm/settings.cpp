// The shell's Settings app. The settings that matter (palette, tablet mode, icon
// overlap, DND) are applied to the running shell immediately rather than written
// to a file for the next session.
//
// Desktop: a real managed win11wm client, so it gets the standard Fluent frame,
// caption buttons, snapping and edge resizing. The WM paints its content in the
// overlay instead of reading it back from a pixmap.
// Tablet: a full-screen iOS sheet, because mobile mode has no window chrome.
#include <manager.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

namespace wm {
namespace {

Color alpha(const Color& c, float a) { return Color{c.r, c.g, c.b, c.a * a}; }

enum SettingId {
    kSetLight = 1,
    kSetNight,
    kSetWallpaper,
    kSetWifi,
    kSetBluetooth,
    kSetAirplane,
    kSetBrightness,
    kSetTablet,
    kSetDnd,
    kSetOverlap,
    kSetLock,
    kSetScreenshot,
    kSetAboutShell,
    kSetDisplay,
    kSetSession,
    kSetPowered,
};

const char* const kSectionNames[4] = {"Appearance", "Network", "System", "About"};

std::string fitText(Text& text, const std::string& s, int px, int maxW) {
    if (maxW <= 0 || s.empty()) return std::string();
    if (text.measure(s, px) <= maxW) return s;
    std::string out = s;
    while (!out.empty() && text.measure(out + "\u2026", px) > maxW) {
        out.resize(out.size() - 1);
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80)
            out.resize(out.size() - 1);
    }
    return out + "\u2026";
}

std::string brightnessSubtitle(int value, int w, int h) {
    return std::to_string(value) + "% \u00B7 " + std::to_string(w) + " \u00D7 " +
           std::to_string(h);
}

std::string wallpaperSubtitle() {
    const std::string path = loadWallpaperPath();
    if (path.empty()) return "Bundled wallpaper";
    const size_t slash = path.find_last_of('/');
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    return "Current: " + name;
}

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void Manager::openSettings() {
    if (tabletMode) {
        if (settingsOpen) return;
    } else {
        openSettingsWindow();
        return;
    }
    // Settings outranks every other shell surface, including the tablet drawer
    // and the Launchpad, so they are all put away before it opens.
    closeOverlays();
    closeTabletDrawer();
    closeTabletMenu();
    closeTabletWidgetPicker();
    closeTabletDockPicker();
    if (tabletFolderOpen >= 0) closeTabletFolder();
    tabletEdit = false;
    settingsOpen = true;
    settingsAnim = 0.0;
    settingsHover = -1;
    settingsNavHover = -1;
    layoutSettings();
    grabPointer();
    grabShellKeyboard();
    dirty = true;
}

void Manager::openSettingsWindow() {
    if (settingsClient && settingsClient->alive) {
        if (settingsClient->minimized) restoreClient(settingsClient);
        focusClient(settingsClient, true);
        dirty = true;
        return;
    }
    closeOverlays();
    closeTabletDrawer();
    closeTabletMenu();
    closeTabletWidgetPicker();
    closeTabletDockPicker();
    if (tabletFolderOpen >= 0) closeTabletFolder();
    tabletEdit = false;

    const int frameW = std::min(860, screenW - 80);
    const int frameH = std::min(620, screenH - 100);
    const int clientW = frameW - 2 * metrics::kBorder;
    const int clientH = frameH - metrics::kCaptionH - metrics::kBorder;
    const int x = (screenW - frameW) / 2;
    const int y = (screenH - frameH) / 2;
#ifdef WIN11WM_WAYLAND
    Client* c = addInternalWindow("Settings", x, y, frameW, frameH);
    if (!c) return;
#else
    XSetWindowAttributes swa{};
    swa.background_pixel = BlackPixel(dpy, DefaultScreen(dpy));
    Window win = XCreateWindow(dpy, root, x, y, clientW, clientH, 0, CopyFromParent,
                               InputOutput, CopyFromParent, CWBackPixel, &swa);
    if (!win) return;
    XStoreName(dpy, win, "Settings");
    XClassHint hint{};
    hint.res_name = const_cast<char*>("systemsettings");
    hint.res_class = const_cast<char*>("Settings");
    XSetClassHint(dpy, win, &hint);
    XSizeHints normal{};
    normal.flags = PMinSize;
    normal.min_width = 620;
    normal.min_height = 380;
    XSetWMNormalHints(dpy, win, &normal);

    Client* c = add(win, false);
    if (!c) {
        XDestroyWindow(dpy, win);
        return;
    }
#endif
    c->internalSettings = true;
    settingsClient = c;
    c->title = "Settings";
    c->frame = Rect{x, y, frameW, frameH};
    c->restore = c->frame;
    c->drawFrame = c->frame;
    settleGeometry(c);
    settingsHover = -1;
    settingsNavHover = -1;
    // The WM owns this window, so map it through the same path as a client map
    // request. Waiting for a MapRequest is not reliable for a window we created
    // ourselves, and mapping it unredirected would leave a black rectangle on
    // top of the composited shell.
    mapClient(c);
#ifndef WIN11WM_WAYLAND
    XSelectInput(dpy, win, PropertyChangeMask | ButtonPressMask | ButtonReleaseMask |
                              PointerMotionMask | KeyPressMask);
#endif
    c->appear = 1.0;
    c->animMs = 0;
    c->animStart = 0.0;
    settleGeometry(c);
    focusClient(c, true);
    dirty = true;
}

void Manager::closeSettingsWindow() {
    Client* c = settingsClient;
    settingsClient = nullptr;
    if (!c) return;
#ifndef WIN11WM_WAYLAND
    const Window id = c->id;
    removeClient(c);
    if (id) XDestroyWindow(dpy, id);
#else
    removeClient(c);
#endif
    dirty = true;
}

bool Manager::launchShellApp(const std::string& exec) {
    if (exec == kSettingsExec) {
        openSettings();
        return true;
    }
    launchApp(exec);
    return false;
}

void Manager::setSettingsSection(int section) {
    section = std::clamp(section, 0, 2);
    if (section == settingsSection) return;
    settingsSection = section;
    settingsHover = -1;
    layoutSettings();
    dirty = true;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

void Manager::layoutSettings() {
    settingsRows.clear();
    settingsNavRects.clear();
    settingsSection = std::clamp(settingsSection, 0, 2);

    struct Spec {
        int id = 0;
        std::string title;
        std::string subtitle;
        bool toggle = false;
        bool on = false;
        bool enabled = true;
        bool info = false;
        bool header = false;
    };
    const SystemState& s = sysctl.state();
    const auto add = [](std::vector<Spec>& out, int id, std::string title,
                        std::string subtitle, bool toggle, bool on, bool enabled = true,
                        bool info = false) {
        out.push_back(Spec{id, std::move(title), std::move(subtitle), toggle, on, enabled,
                           info, false});
    };
    const auto addHeader = [](std::vector<Spec>& out, std::string title) {
        out.push_back(Spec{0, std::move(title), std::string(), false, false, true, false,
                           true});
    };
    const auto buildSection = [&](int section, std::vector<Spec>& out) {
        if (section == 0) {
            add(out, kSetLight, "Light mode", "Use the light Fluent palette", true,
                theme::isLight());
            add(out, kSetNight, "Night light", "Warm the screen in the evening", true,
                s.nightLight);
            add(out, kSetWallpaper, "Wallpaper", wallpaperSubtitle(), false, false);
        } else if (section == 1) {
            add(out, kSetTablet, "Tablet mode", "Switch to the touch-first home screen",
                true, tabletMode);
            add(out, kSetDnd, "Do not disturb", "Silence notifications", true,
                s.dndPresent ? s.dnd : dnd);
            add(out, kSetOverlap, "Prevent icon overlap", "Keep icons out of a widget's way",
                true, desktopNoOverlap);
            add(out, kSetLock, "Lock screen", "Lock the session now", false, false,
                s.lockPresent);
            add(out, kSetScreenshot, "Screenshot", "Capture the whole display", false,
                false, s.shotPresent);
        } else {
            add(out, kSetAboutShell, "Supernova Shell",
                "C++17 X11 compositing window manager", false, false, true, true);
            add(out, kSetDisplay, "Display",
                std::to_string(screenW) + " \u00D7 " + std::to_string(screenH), false,
                false, true, true);
            add(out, kSetSession, "Session",
                tabletMode ? "X11 \u00B7 tablet home" : "X11 \u00B7 desktop", false, false,
                true, true);
        }
    };

    if (tabletMode) {
        // iOS Settings: one full-screen grouped list, large title, Done button and
        // inset cards per section. The mobile skin deliberately has no sidebar --
        // every section is in one scrollable-looking column, as on an iPhone.
        settingsPanel = Rect{0, 0, screenW, screenH};
        settingsCloseRect = Rect{};
        const int contentW = std::min(720, screenW - 32);
        const int x = (screenW - contentW) / 2;
        settingsDoneRect = Rect{x + contentW - 76, 46, 76, 38};

        std::vector<Spec> all;
        for (int section = 0; section < 3; ++section) {
            addHeader(all, kSectionNames[section]);
            buildSection(section, all);
        }
        int rowCount = 0;
        for (const Spec& spec : all)
            if (!spec.header) ++rowCount;
        const int fixedH = 112 + 3 * 28 + 2 * 20 + 24;
        const int rowH = rowCount > 0
                             ? std::clamp((screenH - fixedH) / rowCount, 36, 48)
                             : 48;
        int y = 112;
        for (size_t i = 0; i < all.size(); ++i) {
            if (all[i].header) {
                if (i != 0) y += 20;
                settingsRows.push_back(SettingsRow{0, Rect{x, y, contentW, 26},
                                                   all[i].title, std::string(), false, false,
                                                   false, false, true});
                y += 28;
            } else {
                settingsRows.push_back(
                    SettingsRow{all[i].id, Rect{x, y, contentW, rowH}, all[i].title,
                                all[i].subtitle, all[i].toggle, all[i].on, all[i].enabled,
                                all[i].info, false});
                y += rowH;
            }
        }
    } else {
        // Fluent window: the client content is the panel. The caption and border
        // are drawn by the compositor as usual; this only lays out the sidebar
        // and rows inside it.
        std::vector<Spec> specs;
        buildSection(settingsSection, specs);
        if (settingsClient && settingsClient->alive) {
            const Rect& f = settingsClient->frame;
            settingsPanel = Rect{f.x + metrics::kBorder, f.y + settingsClient->captionH,
                                 f.w - 2 * metrics::kBorder,
                                 f.h - settingsClient->captionH - metrics::kBorder};
        } else {
            settingsPanel = Rect{80, 80, std::min(780, screenW - 160),
                                 std::min(560, screenH - 160)};
        }
        settingsDoneRect = Rect{};
        settingsCloseRect = Rect{};
        const int navX = settingsPanel.x + 18;
        const int navY = settingsPanel.y + 22;
        for (int i = 0; i < 3; ++i)
            settingsNavRects.push_back(Rect{navX, navY + i * 48, 174, 42});
        const int contentX = settingsPanel.x + 210;
        const int contentW = std::max(120, settingsPanel.w - 240);
        const int n = std::max<int>(1, int(specs.size()));
        const int rowH = std::clamp((settingsPanel.h - 64) / n, 44, 56);
        for (size_t i = 0; i < specs.size(); ++i) {
            const Rect r{contentX, settingsPanel.y + 64 + int(i) * rowH, contentW, rowH};
            settingsRows.push_back(
                SettingsRow{specs[i].id, r, specs[i].title, specs[i].subtitle,
                            specs[i].toggle, specs[i].on, specs[i].enabled, specs[i].info,
                            false});
        }
    }
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------

void Manager::updateSettingsHover(int x, int y) {
    int row = -1;
    for (size_t i = 0; i < settingsRows.size(); ++i) {
        if (settingsRows[i].enabled && !settingsRows[i].header &&
            settingsRows[i].rect.contains(x, y)) {
            row = int(i);
            break;
        }
    }
    int nav = -1;
    for (size_t i = 0; i < settingsNavRects.size(); ++i) {
        if (settingsNavRects[i].contains(x, y)) {
            nav = int(i);
            break;
        }
    }
    if (row != settingsHover || nav != settingsNavHover) {
        settingsHover = row;
        settingsNavHover = nav;
        dirty = true;
    }
}

bool Manager::handleSettingsPress(int x, int y, unsigned button) {
    if (button != Button1) return true;
    if (settingsDoneRect.w > 0 && settingsDoneRect.contains(x, y)) {
        closeOverlays();
        return true;
    }
    if (settingsCloseRect.w > 0 && settingsCloseRect.contains(x, y)) {
        closeOverlays();
        return true;
    }
    for (size_t i = 0; i < settingsNavRects.size(); ++i) {
        if (!settingsNavRects[i].contains(x, y)) continue;
        setSettingsSection(int(i));
        return true;
    }
    for (size_t i = 0; i < settingsRows.size(); ++i) {
        const SettingsRow& r = settingsRows[i];
        if (!r.enabled || !r.rect.contains(x, y)) continue;
        if (!r.info) activateSettingsRow(r.id);
        return true;
    }
    // A press on the desktop backdrop dismisses; the tablet sheet fills the
    // screen, so there is nowhere outside it to press.
    if (!tabletMode && !settingsPanel.contains(x, y)) closeOverlays();
    return true;
}

bool Manager::handleSettingsContentPress(int x, int y, unsigned button) {
    if (button != Button1) return true;
    for (size_t i = 0; i < settingsNavRects.size(); ++i) {
        if (!settingsNavRects[i].contains(x, y)) continue;
        setSettingsSection(int(i));
        return true;
    }
    for (size_t i = 0; i < settingsRows.size(); ++i) {
        const SettingsRow& r = settingsRows[i];
        if (!r.enabled || !r.rect.contains(x, y)) continue;
        if (!r.info) activateSettingsRow(r.id);
        return true;
    }
    return true;
}

void Manager::activateSettingsRow(int id) {
    const SystemState& s = sysctl.state();
    switch (id) {
        case kSetLight:
            setLightMode(!theme::isLight());
            break;
        case kSetNight:
            sysctl.setNightLight(!s.nightLight);
            break;
        case kSetWallpaper: {
            std::string dir = configPath("wallpaper");
            const size_t slash = dir.find_last_of('/');
            if (slash != std::string::npos) dir.resize(slash);
            if (dir.empty()) break;
            std::string quoted = "'";
            for (char ch : dir) {
                if (ch == '\'') quoted += "'\\''";
                else quoted += ch;
            }
            quoted += "'";
            launchApp("xdg-open " + quoted);
            break;
        }
        case kSetTablet:
            closeSettingsWindow();
            setTabletMode(!tabletMode);
            closeOverlays();
            return;
        case kSetDnd: {
            const bool next = !(s.dndPresent ? s.dnd : dnd);
            if (s.dndPresent) sysctl.setDnd(next);
            else dnd = next;
            if (next) {
                for (auto& cp : clients) cp->attentionPulse = 0.0;
            }
            break;
        }
        case kSetOverlap:
            desktopNoOverlap = !desktopNoOverlap;
            saveDesktopNoOverlap(desktopNoOverlap);
            if (tabletMode) layoutTabletHome();
            else layoutDesktopIcons();
            break;
        case kSetLock:
            sysctl.lockSession();
            closeOverlays();
            return;
        case kSetScreenshot:
            sysctl.screenshot();
            break;
        default:
            return;
    }
    // Rebuild the rows so the toggle knobs and the wallpaper subtitle follow the
    // change immediately.
    layoutSettings();
    dirty = true;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void Manager::drawSettings() {
    if (!settingsOpen && settingsAnim <= 0.001) return;
    const float a = float(clamp01(easeOutCubic(settingsAnim)));
    if (a <= 0.001f) return;
    const int slide = int(std::lround((1.0 - double(a)) * (tabletMode ? 26 : 14)));

    if (tabletMode) {
        // iOS Settings: a grouped list on the system background, with a large
        // title, a blue Done button, inset rounded cards and green switches.
        const bool light = theme::isLight();
        const Color bg = light ? rgb(0xF2F2F7) : rgb(0x000000);
        const Color card = light ? rgb(0xFFFFFF) : rgb(0x1C1C1E);
        const Color headerInk = light ? rgb(0x6E6E73) : rgb(0x8E8E93);
        const Color sep = light ? rgb(0x000000, 0.10f) : rgb(0xFFFFFF, 0.10f);
        const Color ink = light ? rgb(0x000000) : rgb(0xFFFFFF);
        const Color subInk = light ? rgb(0x6E6E73) : rgb(0x8E8E93);
        const Color iosBlue = rgb(0x007AFF);
        const Color iosGreen = rgb(0x34C759);
        const Color trackOff = light ? rgb(0xE9E9EA) : rgb(0x39393D);

        comp.drawRect(Rect{0, 0, screenW, screenH}, 0.f, bg, a);

        const int contentW = std::min(720, screenW - 32);
        const int x = (screenW - contentW) / 2;
        const TextTex title = text.get("Settings", 34, Weight::Bold);
        if (title.tex)
            comp.drawText(title, Rect{x, 40 + slide, title.w, title.h}, alpha(ink, a), a);

        const Rect done = Rect{settingsDoneRect.x, settingsDoneRect.y + slide,
                               settingsDoneRect.w, settingsDoneRect.h};
        if (settingsDoneRect.contains(pointerX, pointerY))
            comp.drawRect(done, 10.f, alpha(iosBlue, 0.12f), a);
        const TextTex doneTex = text.get("Done", 17, Weight::Regular);
        if (doneTex.tex)
            comp.drawText(doneTex,
                          Rect{done.x + (done.w - doneTex.w) / 2,
                               done.y + (done.h - doneTex.h) / 2, doneTex.w, doneTex.h},
                          alpha(iosBlue, a), a);

        // The card behind each section: one rounded surface from its first row
        // to its last, so the rows read as a grouped iOS list rather than cards.
        for (size_t i = 0; i < settingsRows.size(); ++i) {
            if (!settingsRows[i].header) continue;
            size_t j = i + 1;
            while (j < settingsRows.size() && !settingsRows[j].header) ++j;
            if (j == i + 1) continue;
            const Rect first = settingsRows[i + 1].rect;
            const Rect last = settingsRows[j - 1].rect;
            const Rect group{first.x, first.y + slide, first.w,
                             last.bottom() - first.y};
            comp.drawRect(group, 12.f, alpha(card, a), a);
        }

        for (size_t i = 0; i < settingsRows.size(); ++i) {
            const SettingsRow& r = settingsRows[i];
            Rect row = r.rect;
            row.y += slide;
            if (r.header) {
                std::string label = r.title;
                for (char& ch : label)
                    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                const TextTex h = text.get(label, 12, Weight::Medium);
                if (h.tex)
                    comp.drawText(h, Rect{row.x + 16, row.y + (row.h - h.h) / 2, h.w, h.h},
                                  alpha(headerInk, a), a);
                continue;
            }
            const bool hot = int(i) == settingsHover;
            if (hot && r.enabled) comp.drawRect(row, 0.f, alpha(iosBlue, 0.08f), a);

            // Hairline separator between rows in the same group.
            const bool lastInGroup = i + 1 >= settingsRows.size() ||
                                     settingsRows[i + 1].header;
            if (!lastInGroup)
                comp.drawRect(Rect{row.x + 16, row.bottom() - 1, row.w - 16, 1}, 0.f,
                              alpha(sep, a), a);

            const int textX = row.x + 16;
            const int switchW = r.toggle ? 51 : 0;
            const int textW = row.w - 32 - switchW - (r.toggle ? 12 : 26);
            const TextTex t = text.get(fitText(text, r.title, 16, textW), 16,
                                       Weight::Regular);
            const TextTex st = text.get(fitText(text, r.subtitle, 11, textW), 11,
                                        Weight::Regular);
            const int blockH = (t.tex ? t.h : 0) + (st.tex ? st.h + 2 : 0);
            const int top = row.y + (row.h - blockH) / 2;
            if (t.tex)
                comp.drawText(t, Rect{textX, top, t.w, t.h},
                              alpha(r.enabled ? ink : subInk, a), a);
            if (st.tex)
                comp.drawText(st, Rect{textX, top + t.h + 2, st.w, st.h},
                              alpha(subInk, a), a);

            if (r.toggle) {
                const int tw = 51, th = 31;
                const int tx = row.right() - 16 - tw;
                const int ty = row.y + (row.h - th) / 2;
                const Rect track{tx, ty, tw, th};
                comp.drawRect(track, float(th) * 0.5f,
                              r.on ? alpha(iosGreen, r.enabled ? 0.98f : 0.35f)
                                   : alpha(trackOff, r.enabled ? 1.f : 0.5f), a);
                const int knob = th - 4;
                const int kx = r.on ? track.right() - knob - 2 : track.x + 2;
                comp.drawRect(Rect{kx, track.y + 2, knob, knob}, float(knob) * 0.5f,
                              alpha(Color{1.f, 1.f, 1.f}, r.enabled ? 1.f : 0.6f), a);
            } else if (!r.info && r.enabled) {
                const TextTex chev = text.get("\u203A", 22, Weight::Regular);
                if (chev.tex)
                    comp.drawText(chev,
                                  Rect{row.right() - 22 - chev.w / 2,
                                       row.y + (row.h - chev.h) / 2, chev.w, chev.h},
                                  alpha(subInk, a), a);
            }
        }
        return;
    }

}

void Manager::drawSettingsContent(Client* c) {
    if (!c || !c->internalSettings || !c->alive) return;
    const Rect panel{c->frame.x + metrics::kBorder, c->frame.y + c->captionH,
                     c->frame.w - 2 * metrics::kBorder,
                     c->frame.h - c->captionH - metrics::kBorder};
    if (panel.w < 80 || panel.h < 80) return;
    if (settingsPanel.x != panel.x || settingsPanel.y != panel.y ||
        settingsPanel.w != panel.w || settingsPanel.h != panel.h) {
        settingsPanel = panel;
        layoutSettings();
    }

    const bool light = theme::isLight();
    const Color panelBg = light ? rgb(0xF2F2F7) : rgb(0x1C1C1E);
    const Color card = light ? rgb(0xFFFFFF) : rgb(0x2C2C2E);
    const Color sidebarSel = light ? rgb(0xE5E5EA) : rgb(0x3A3A3C);
    const Color sep = light ? rgb(0x000000, 0.12f) : rgb(0xFFFFFF, 0.12f);
    const Color ink = light ? rgb(0x000000) : rgb(0xFFFFFF);
    const Color subInk = light ? rgb(0x6E6E73) : rgb(0x8E8E93);
    const Color iosBlue = rgb(0x007AFF);
    const Color iosGreen = rgb(0x34C759);
    const Color trackOff = light ? rgb(0xE9E9EA) : rgb(0x39393D);

    comp.drawRect(panel, 0.f, panelBg, 1.f);
    comp.drawRect(Rect{panel.x + 198, panel.y + 12, 1, panel.h - 24}, 0.f,
                  alpha(sep, 0.7f), 1.f);

    for (size_t i = 0; i < settingsNavRects.size(); ++i) {
        const Rect& nav = settingsNavRects[i];
        const bool on = int(i) == settingsSection;
        const bool hot = int(i) == settingsNavHover;
        if (on || hot)
            comp.drawRect(nav, 10.f, on ? sidebarSel : alpha(ink, 0.05f), 1.f);
        const TextTex t = text.get(kSectionNames[i], 14,
                                   on ? Weight::Medium : Weight::Regular);
        if (!t.tex) continue;
        comp.drawText(t, Rect{nav.x + 16, nav.y + (nav.h - t.h) / 2, t.w, t.h},
                      on ? iosBlue : ink, 1.f);
    }

    std::string header = kSectionNames[settingsSection];
    for (char& ch : header)
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    const TextTex h = text.get(header, 12, Weight::Medium);
    if (h.tex)
        comp.drawText(h, Rect{panel.x + 210, panel.y + 18, h.w, h.h}, subInk, 1.f);

    if (!settingsRows.empty()) {
        const Rect first = settingsRows.front().rect;
        const Rect last = settingsRows.back().rect;
        const Rect group{first.x, first.y, first.w, last.bottom() - first.y};
        comp.drawRect(group, 12.f, card, 1.f);
    }

    for (size_t i = 0; i < settingsRows.size(); ++i) {
        const SettingsRow& r = settingsRows[i];
        const Rect& row = r.rect;
        const bool hot = int(i) == settingsHover;
        if (hot && r.enabled) comp.drawRect(row, 0.f, alpha(iosBlue, 0.08f), 1.f);
        if (i + 1 < settingsRows.size())
            comp.drawRect(Rect{row.x + 16, row.bottom() - 1, row.w - 16, 1}, 0.f, sep, 1.f);

        const int textX = row.x + 16;
        const int switchW = r.toggle ? 51 : 0;
        const int textW = row.w - 32 - switchW - (r.toggle ? 12 : 26);
        const TextTex t = text.get(fitText(text, r.title, 15, textW), 15, Weight::Regular);
        const TextTex st = text.get(fitText(text, r.subtitle, 11, textW), 11, Weight::Regular);
        const int blockH = (t.tex ? t.h : 0) + (st.tex ? st.h + 2 : 0);
        const int top = row.y + (row.h - blockH) / 2;
        if (t.tex)
            comp.drawText(t, Rect{textX, top, t.w, t.h}, r.enabled ? ink : subInk, 1.f);
        if (st.tex)
            comp.drawText(st, Rect{textX, top + t.h + 2, st.w, st.h}, subInk, 1.f);

        if (r.toggle) {
            const int tw = 51, th = 31;
            const int tx = row.right() - 16 - tw;
            const int ty = row.y + (row.h - th) / 2;
            const Rect track{tx, ty, tw, th};
            comp.drawRect(track, float(th) * 0.5f,
                          r.on ? alpha(iosGreen, r.enabled ? 0.98f : 0.35f)
                               : alpha(trackOff, r.enabled ? 1.f : 0.5f),
                          1.f);
            const int knob = th - 4;
            const int kx = r.on ? track.right() - knob - 2 : track.x + 2;
            comp.drawRect(Rect{kx, track.y + 2, knob, knob}, float(knob) * 0.5f,
                          alpha(Color{1.f, 1.f, 1.f}, r.enabled ? 1.f : 0.6f), 1.f);
        } else if (!r.info && r.enabled) {
            const TextTex chev = text.get("\u203A", 22, Weight::Regular);
            if (chev.tex)
                comp.drawText(chev,
                              Rect{row.right() - 22 - chev.w / 2,
                                   row.y + (row.h - chev.h) / 2, chev.w, chev.h},
                              subInk, 1.f);
        }
    }
}

}  // namespace wm
