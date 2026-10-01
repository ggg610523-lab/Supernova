// Desktop widgets. Manager methods that own the iOS 26 style glass cards on the
// wallpaper: battery probing, placement, drag/resize, hit-testing and drawing.
#include "manager.h"

#include <algorithm>
#include <cmath>
#include <dirent.h>
#include <fstream>
#include <string>

namespace wm {
namespace {

constexpr float kPi = 3.14159265358979323846f;

std::string readText(const std::string& path) {
    std::ifstream f(path);
    std::string line;
    if (f && std::getline(f, line)) return line;
    return {};
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    return s.substr(i);
}

struct BatteryReading {
    bool present = false;
    int percent = -1;
    bool charging = false;
    bool full = false;
};

// Reads /sys/class/power_supply directly -- no fork, no polling helper.
BatteryReading probeBattery() {
    BatteryReading r;
    DIR* d = opendir("/sys/class/power_supply");
    if (!d) return r;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        const std::string base = std::string("/sys/class/power_supply/") + e->d_name;
        if (trim(readText(base + "/type")) != "Battery") continue;
        const std::string cap = trim(readText(base + "/capacity"));
        if (cap.empty()) continue;
        r.present = true;
        r.percent = std::clamp(std::atoi(cap.c_str()), 0, 100);
        const std::string status = trim(readText(base + "/status"));
        r.charging = status == "Charging" || status == "Full";
        r.full = status == "Full";
        break;
    }
    closedir(d);
    return r;
}

// A wedge of a ring, tiled from small tangential capsules so the shared
// axis-aligned rect primitive can render a smooth arc.
void arc(Compositor& comp, int cx, int cy, float radius, float thick, float a0, float a1,
         const Color& c, float opacity) {
    const float span = a1 - a0;
    if (std::fabs(span) < 0.01f || radius <= 0.f) return;
    const int n = std::max(3, int(std::ceil(std::fabs(span) / (2.f * kPi) * 128.f)));
    const float step = span / n;
    for (int i = 0; i < n; ++i) {
        const float tm = a0 + step * (i + 0.5f);
        const int px = int(std::lround(cx + std::sin(tm) * radius));
        const int py = int(std::lround(cy - std::cos(tm) * radius));
        const float seg = radius * std::fabs(step) * 1.7f + thick * 0.5f;
        comp.drawRectRotated(px, py, int(std::lround(thick)), int(std::lround(seg)),
                             tm + kPi * 0.5f, thick * 0.5f, c, opacity);
    }
}

// A rounded bar placed along the radial direction at angle `a` (clockwise from
// 12 o'clock), centred `rAt` from the centre.
void radialRect(Compositor& comp, int cx, int cy, float a, float rAt, float thick, float len,
                const Color& c, float opacity) {
    const int px = int(std::lround(cx + std::sin(a) * rAt));
    const int py = int(std::lround(cy - std::cos(a) * rAt));
    comp.drawRectRotated(px, py, std::max(1, int(std::lround(thick))), std::max(1, int(std::lround(len))),
                         a, thick * 0.5f, c, opacity);
}

// A clock hand from `tail` behind the centre to `len` in front of it.
void hand(Compositor& comp, int cx, int cy, float a, float len, float thick, float tail,
          const Color& c) {
    const float total = len + tail;
    const float mid = (len - tail) * 0.5f;
    radialRect(comp, cx, cy, a, mid, thick, total, c, 1.0f);
}

}  // namespace

Rect Manager::widgetGripRect(const Widget& w) const {
    const int g = std::min(metrics::kWidgetGrip, std::min(w.rect.w, w.rect.h) / 2);
    return Rect{w.rect.right() - g, w.rect.bottom() - g, g, g};
}

int Manager::widgetAt(int x, int y) const {
    for (size_t i = widgets.size(); i-- > 0;)
        if (widgets[i].rect.contains(x, y)) return int(i);
    return -1;
}

void Manager::refreshBattery(bool force) {
    const BatteryReading b = probeBattery();
    const int p = b.present ? b.percent : -1;
    if (force || p != batteryPercent || b.charging != batteryCharging || b.full != batteryFull) {
        batteryPercent = p;
        batteryCharging = b.charging;
        batteryFull = b.full;
        dirty = true;
    }
}

void Manager::initWidgets() {
    widgets.clear();
    addWidget(WidgetKind::Clock);
    addWidget(WidgetKind::Battery);
    refreshBattery(true);
}

void Manager::addWidget(WidgetKind kind) {
    const int w = kind == WidgetKind::Clock ? metrics::kWidgetClock : metrics::kWidgetBatteryW;
    const int h = kind == WidgetKind::Clock ? metrics::kWidgetClock : metrics::kWidgetBatteryH;
    const int bottom = screenH - metrics::kTaskbarH;
    int x = screenW - w - metrics::kWidgetPad;
    int y = metrics::kWidgetPad;
    // Cascade down/left past the widgets already on screen.
    for (int guard = 0; guard < 64; ++guard) {
        bool clash = false;
        for (const Widget& o : widgets) {
            if (Rect{x, y, w, h}.inflated(12).intersects(o.rect)) {
                y = o.rect.bottom() + 16;
                if (y + h > bottom - metrics::kWidgetPad) {
                    y = metrics::kWidgetPad;
                    x = o.rect.x - w - 16;
                }
                clash = true;
                break;
            }
        }
        if (!clash) break;
    }
    x = std::clamp(x, metrics::kWidgetPad, std::max(metrics::kWidgetPad, screenW - w - metrics::kWidgetPad));
    y = std::clamp(y, metrics::kWidgetPad, std::max(metrics::kWidgetPad, bottom - h - metrics::kWidgetPad));
    widgets.push_back(Widget{kind, Rect{x, y, w, h}});
    if (kind == WidgetKind::Battery) refreshBattery(true);
    layoutDesktopIcons();
    dirty = true;
}

void Manager::removeWidget(int index) {
    if (index < 0 || index >= int(widgets.size())) return;
    widgets.erase(widgets.begin() + index);
    if (dragWidget == index) dragWidget = -1;
    hoverWidget = -1;
    layoutDesktopIcons();
    dirty = true;
}

bool Manager::handleWidgetPress(int x, int y, Time time) {
    const int i = widgetAt(x, y);
    if (i < 0) return false;
    const bool resize = widgetGripRect(widgets[i]).contains(x, y);
    beginWidgetDrag(i, x, y, resize);
    selectedDesktopIcon = -1;
    return true;
}

void Manager::beginWidgetDrag(int index, int x, int y, bool resize) {
    if (index < 0 || index >= int(widgets.size())) return;
    dragWidget = index;
    widgetResizing = resize;
    widgetGrab = Point{x - widgets[index].rect.x, y - widgets[index].rect.y};
    hoverWidget = index;
    grabPointer();
    dirty = true;
}

void Manager::updateWidgetDrag(int x, int y) {
    if (dragWidget < 0 || dragWidget >= int(widgets.size())) return;
    Widget& w = widgets[dragWidget];
    const int bottom = screenH - metrics::kTaskbarH;
    if (!widgetResizing) {
        w.rect.x = std::clamp(x - widgetGrab.x, 0, std::max(0, screenW - w.rect.w));
        w.rect.y = std::clamp(y - widgetGrab.y, 0, std::max(0, bottom - w.rect.h));
    } else {
        w.rect.w = std::clamp(x - w.rect.x, metrics::kWidgetMin, std::max(metrics::kWidgetMin, screenW - w.rect.x));
        w.rect.h = std::clamp(y - w.rect.y, metrics::kWidgetMin, std::max(metrics::kWidgetMin, bottom - w.rect.y));
    }
    // Recompute the icon grid on every motion step so the icons slide out of the
    // widget's way live, the way macOS reflows around a dragged item.
    layoutDesktopIcons();
    dirty = true;
}

void Manager::endWidgetDrag() {
    if (dragWidget < 0) return;
    dragWidget = -1;
    widgetResizing = false;
    ungrabPointer();
    layoutDesktopIcons();
    dirty = true;
}

void Manager::drawWidgets() {
    for (size_t i = 0; i < widgets.size(); ++i) {
        const Widget& w = widgets[i];
        const bool dragging = int(i) == dragWidget;
        const bool hot = int(i) == hoverWidget || dragging;
        // The hover wash fades in, and stays lit while the card is being dragged.
        const double hv = dragging ? 1.0 : (i < widgetHover.size() ? widgetHover[i] : 0.0);
        const float radius = std::min(float(metrics::kWidgetRadius),
                                      std::min(w.rect.w, w.rect.h) * 0.28f);
        comp.drawAcrylic(w.rect, radius, theme::kWidgetGlass, 0.62f, theme::kWidgetBorder);
        if (hv > 0.001) comp.drawRect(w.rect, radius, theme::kWidgetHover, float(hv));
        if (w.kind == WidgetKind::Clock)
            drawClockWidget(w);
        else
            drawBatteryWidget(w);
        if (hot) {  // diagonal resize grip
            const Rect g = widgetGripRect(w);
            for (int k = 0; k < 3; ++k) {
                const int d = (2 - k) * 7;
                comp.drawRect(Rect{g.right() - 5 - d, g.bottom() - 5 - d, 4, 4}, 1.f, theme::kWidgetGrip);
            }
        }
    }
}

void Manager::drawClockWidget(const Widget& w) {
    const int cx = w.rect.x + w.rect.w / 2;
    const int cy = w.rect.y + w.rect.h / 2;
    const float R = std::min(w.rect.w, w.rect.h) * 0.5f;
    const float pad = R * 0.12f;

    time_t t = time(nullptr);
    struct tm lt {};
    localtime_r(&t, &lt);
    const float hourAng = (lt.tm_hour % 12 + lt.tm_min / 60.f + lt.tm_sec / 3600.f) * kPi / 6.f;
    const float minAng = (lt.tm_min + lt.tm_sec / 60.f) * kPi / 30.f;
    const float secAng = lt.tm_sec * kPi / 30.f;

    for (int i = 0; i < 60; ++i) {
        const float a = i * kPi / 30.f;
        const bool major = (i % 5 == 0);
        const float len = major ? R * 0.10f : R * 0.05f;
        const float th = major ? std::max(2.f, R * 0.026f) : std::max(1.f, R * 0.012f);
        radialRect(comp, cx, cy, a, R - pad - len * 0.5f, th, len,
                   major ? theme::kWidgetTick : theme::kWidgetTickMinor, major ? 1.f : 0.9f);
    }

    const float nr = R - pad - R * 0.24f;
    const int np = std::max(9, int(R * 0.15f));
    auto numeral = [&](const char* s, int dx, int dy) {
        drawTextCentered(s, np, Weight::Medium, theme::kWidgetSub,
                         Rect{cx + dx - int(R * 0.25f), cy + dy - int(R * 0.14f), int(R * 0.5f),
                              int(R * 0.28f)});
    };
    numeral("12", 0, int(-nr));
    numeral("3", int(nr), 0);
    numeral("6", 0, int(nr));
    numeral("9", int(-nr), 0);

    hand(comp, cx, cy, hourAng, R * 0.46f, std::max(3.f, R * 0.074f), R * 0.08f, theme::kWidgetHand);
    hand(comp, cx, cy, minAng, R * 0.66f, std::max(2.f, R * 0.052f), R * 0.10f, theme::kWidgetHand);
    hand(comp, cx, cy, secAng, R * 0.74f, std::max(1.5f, R * 0.022f), R * 0.16f, theme::kWidgetSecond);
    comp.drawRect(Rect{cx - 3, cy - 3, 6, 6}, 3.f, theme::kWidgetSecond);
}

void Manager::drawBatteryWidget(const Widget& w) {
    const int pad = metrics::kWidgetPad;
    const bool has = batteryPercent >= 0;
    const int pct = has ? std::clamp(batteryPercent, 0, 100) : 0;
    const Color col = !has ? theme::kWidgetSub
                           : (batteryCharging || batteryFull) ? theme::kWidgetGreen
                           : pct <= 20 ? theme::kWidgetRed
                           : pct <= 40 ? theme::kWidgetYellow
                                       : theme::kWidgetGreen;

    const int ringD = std::max(48, std::min(w.rect.h - 2 * pad - 8, int(w.rect.w * 0.42f)));
    const int rcx = w.rect.x + pad + ringD / 2;
    const int rcy = w.rect.y + w.rect.h / 2;
    const float radius = ringD * 0.5f - 10.f;
    const float thick = std::max(7.f, ringD * 0.13f);

    arc(comp, rcx, rcy, radius, thick, 0.f, 2.f * kPi, theme::kWidgetRingTrack, 1.f);
    if (has && pct > 0)
        arc(comp, rcx, rcy, radius, thick, 0.f, 2.f * kPi * pct / 100.f, col, 1.f);

    char buf[16];
    if (has)
        std::snprintf(buf, sizeof buf, "%d%%", pct);
    else
        std::snprintf(buf, sizeof buf, "--");
    drawTextCentered(buf, std::max(15, ringD / 4), Weight::Bold, theme::kWidgetLabel,
                     Rect{rcx - ringD / 2, rcy - ringD / 4, ringD, ringD / 2});

    const int tx = rcx + ringD / 2 + pad;
    drawTextAt("Battery", 17, Weight::Medium, theme::kWidgetLabel, tx, w.rect.y + pad + 4);
    const char* st = !has ? "No battery"
                          : batteryFull ? "Charged"
                          : batteryCharging ? "Charging"
                                            : "On battery";
    drawTextAt(st, 14, Weight::Regular, theme::kWidgetSub, tx, w.rect.y + pad + 30);

    const int pillW = std::min(96, w.rect.right() - pad - tx - 14);
    const int pillH = 26;
    if (pillW > 20) {
        const int py = w.rect.bottom() - pad - pillH;
        comp.drawRect(Rect{tx, py, pillW, pillH}, 6.f, theme::kWidgetRingTrack);
        if (has && pct > 0) {
            const int fw = int((pillW - 6) * pct / 100.f);
            if (fw > 0) comp.drawRect(Rect{tx + 3, py + 3, fw, pillH - 6}, 4.f, col);
        }
        comp.drawRect(Rect{tx + pillW + 2, py + pillH / 2 - 5, 5, 10}, 2.f, theme::kWidgetRingTrack);
    }
}

}  // namespace wm
