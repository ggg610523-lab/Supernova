// Desktop widgets. Manager methods that own the iOS 18 style full-colour cards on
// the wallpaper: battery probing, placement, drag/resize, hit-testing and drawing.
#include "manager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <dirent.h>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

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

// Calendar helpers. day count is the only calendar maths the card needs, and it
// is kept here rather than pulling in a date library for one month grid.
int daysInMonth(int year, int mon /*0-11*/) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (mon == 1 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
    return kDays[mon];
}

// Weather card reading. The shell never touches the network: the iOS 18 panel is
// drawn from ~/.config/win11wm/weather when the user has written one, and from a
// pleasant default when they have not, so the card is never blank and the shell
// never blocks on a fetch.
struct WeatherReading {
    std::string city;
    int temp = 0;
    std::string condition;
    int high = 0;
    int low = 0;
    std::vector<std::pair<std::string, int>> hourly;  // label, temperature
};

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

// Collapses the many condition names into the four glyphs the card can draw:
// clear, partly cloudy, overcast, and wet. Everything unmatched reads as grey
// sky, which is the safest thing to show when the text is unrecognised.
enum class SkyGlyph { Clear, Partly, Overcast, Wet };

SkyGlyph skyGlyphFor(const std::string& condition) {
    const std::string s = lower(condition);
    const auto has = [&s](const char* w) { return s.find(w) != std::string::npos; };
    if (has("rain") || has("drizzle") || has("shower") || has("thunder") || has("storm") ||
        has("snow") || has("sleet") || has("hail"))
        return SkyGlyph::Wet;
    if (has("part") || has("mostly sunny") || has("few") || has("scattered") || has("breaks"))
        return SkyGlyph::Partly;
    if (has("clear") || has("sun") || has("fair")) return SkyGlyph::Clear;
    return SkyGlyph::Overcast;
}

WeatherReading readWeatherConfig() {
    WeatherReading r;
    r.city = "Cupertino";
    r.temp = 72;
    r.condition = "Partly Cloudy";
    r.high = 78;
    r.low = 64;
    r.hourly = {{"Now", 72}, {"1PM", 74}, {"2PM", 76}, {"3PM", 77}, {"4PM", 76}};

    std::ifstream f(configPath("weather"));
    std::string line;
    while (f && std::getline(f, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq));
        const std::string val = trim(line.substr(eq + 1));
        if (key == "city") r.city = val;
        else if (key == "condition") r.condition = val;
        else if (key == "temp") r.temp = std::atoi(val.c_str());
        else if (key == "high") r.high = std::atoi(val.c_str());
        else if (key == "low") r.low = std::atoi(val.c_str());
        else if (key == "hourly") {
            r.hourly.clear();
            size_t i = 0;
            while (i < val.size()) {
                const size_t comma = val.find(',', i);
                const size_t end = comma == std::string::npos ? val.size() : comma;
                const std::string item = trim(val.substr(i, end - i));
                const size_t colon = item.rfind(':');
                if (colon != std::string::npos) {
                    const std::string lab = trim(item.substr(0, colon));
                    const int tp = std::atoi(trim(item.substr(colon + 1)).c_str());
                    if (!lab.empty()) r.hourly.push_back({lab, tp});
                }
                if (comma == std::string::npos) break;
                i = comma + 1;
            }
        }
    }
    if (r.hourly.size() > 6) r.hourly.resize(6);
    if (r.hourly.empty()) r.hourly.push_back({"Now", r.temp});
    if (r.city.empty()) r.city = "My Location";
    return r;
}

// The sky gradient: a clear blue by day, a night blue after dark, and a muted
// grey-blue under cloud or rain -- the same shift the iOS Weather panel makes.
void weatherPalette(SkyGlyph sky, bool night, Color& top, Color& bottom) {
    if (night) {
        top = theme::kWidgetWeatherNightTop;
        bottom = theme::kWidgetWeatherNightBottom;
        return;
    }
    if (sky == SkyGlyph::Clear) {
        top = theme::kWidgetWeatherClearTop;
        bottom = theme::kWidgetWeatherClearBottom;
    } else if (sky == SkyGlyph::Wet) {
        top = theme::kWidgetWeatherRainTop;
        bottom = theme::kWidgetWeatherRainBottom;
    } else {
        top = theme::kWidgetWeatherCloudTop;
        bottom = theme::kWidgetWeatherCloudBottom;
    }
}

// A condition glyph built from primitives, so the card needs no icon theme and
// stays crisp at any widget size. `s` is the glyph's nominal diameter.
void weatherGlyph(Compositor& comp, int cx, int cy, float s, SkyGlyph sky, const Color& ink,
                  const Color& sun) {
    auto circle = [&](float x, float y, float r, const Color& c) {
        comp.drawRect(Rect{int(std::lround(x - r)), int(std::lround(y - r)),
                           int(std::lround(2 * r)), int(std::lround(2 * r))},
                      r, c);
    };
    auto cloud = [&](float x, float y, float cs) {
        circle(x - cs * 0.22f, y - cs * 0.06f, cs * 0.30f, ink);
        circle(x + cs * 0.20f, y - cs * 0.02f, cs * 0.24f, ink);
        comp.drawRect(Rect{int(std::lround(x - cs * 0.55f)), int(std::lround(y + cs * 0.06f)),
                           int(std::lround(cs * 1.10f)), int(std::lround(cs * 0.34f))},
                      cs * 0.17f, ink);
    };

    if (sky == SkyGlyph::Clear) {
        circle(cx, cy, s * 0.30f, sun);
        for (int i = 0; i < 8; ++i) {
            const float a = i * float(kPi) / 4.f;
            const int px = int(std::lround(cx + std::sin(a) * s * 0.44f));
            const int py = int(std::lround(cy - std::cos(a) * s * 0.44f));
            comp.drawRectRotated(px, py, int(std::max(2.f, s * 0.055f)), int(std::max(3.f, s * 0.16f)),
                                 a, s * 0.03f, sun, 1.f);
        }
    } else if (sky == SkyGlyph::Partly) {
        circle(cx + s * 0.17f, cy - s * 0.20f, s * 0.19f, sun);
        cloud(cx - s * 0.04f, cy + s * 0.10f, s * 0.86f);
    } else if (sky == SkyGlyph::Wet) {
        cloud(cx, cy - s * 0.10f, s * 0.92f);
        for (int i = -1; i <= 1; ++i)
            comp.drawRectRotated(int(std::lround(cx + i * s * 0.26f)), int(std::lround(cy + s * 0.34f)),
                                 int(std::max(2.f, s * 0.05f)), int(std::lround(s * 0.22f)), 0.35f,
                                 s * 0.025f, ink, 1.f);
    } else {
        cloud(cx, cy + s * 0.02f, s * 0.98f);
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

// A vertical two-stop gradient rounded panel, the base of an iOS 18 widget.
void gradientPanel(Compositor& comp, const Rect& r, float radius, const Color& top,
                   const Color& bottom) {
    comp.drawGradient(r, radius, top, bottom, 1.0f);
}

}  // namespace

Rect Manager::widgetGripRect(const Widget& w) const {
    const int g = std::min(metrics::kWidgetGrip, std::min(w.rect.w, w.rect.h) / 2);
    return Rect{w.rect.right() - g, w.rect.bottom() - g, g, g};
}

// Whether a card is on the page being shown. A card whose own page has gone past
// the end of the grid -- because apps were taken off the home screen and there are
// fewer pages now -- shows on the last page rather than disappearing, and goes back
// to its own page if the grid ever grows that far again.
bool Manager::widgetOnPage(const Widget& w) const {
    if (!tabletMode) return true;
    return std::min(w.page, std::max(0, tabletHomePageCount - 1)) == tabletHomePage;
}

int Manager::widgetAt(int x, int y) const {
    for (size_t i = widgets.size(); i-- > 0;) {
        // On a tablet a card belongs to one page, so a card that is not on the page
        // being shown is not there at all: it is not drawn, and it must not swallow
        // a press meant for the icon underneath it either.
        if (!widgetOnPage(widgets[i])) continue;
        if (widgets[i].rect.contains(x, y)) return int(i);
    }
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
    addWidget(WidgetKind::Calendar);
    addWidget(WidgetKind::Weather);
    addWidget(WidgetKind::DigitalClock);
    refreshBattery(true);
    refreshWeather(true);
}

// Reads ~/.config/win11wm/weather (or the built-in default) and folds the time
// of day in, so the panel darkens after sunset. Runs on the same slow timer as
// the battery: the file changes rarely and the card must not reread it per frame.
void Manager::refreshWeather(bool force) {
    const WeatherReading r = readWeatherConfig();
    time_t t = time(nullptr);
    struct tm lt {};
    localtime_r(&t, &lt);
    const bool night = lt.tm_hour < 6 || lt.tm_hour >= 19;
    if (force || r.city != weatherCity || r.temp != weatherTemp || r.condition != weatherCondition ||
        r.high != weatherHigh || r.low != weatherLow || r.hourly != weatherHourly ||
        night != weatherNight) {
        weatherCity = r.city;
        weatherTemp = r.temp;
        weatherCondition = r.condition;
        weatherHigh = r.high;
        weatherLow = r.low;
        weatherHourly = r.hourly;
        weatherNight = night;
        dirty = true;
    }
}

// Tablet mode has nowhere to put the cards -- its grid is a page of app icons, not
// wallpaper furniture -- so they are taken off the desktop rather than left sitting
// behind the home screen where a press would grab an invisible one. Held aside
// instead of dropped: leaving the mode has to hand back the desktop the user
// arranged, cards included, rather than an empty wallpaper.
void Manager::suspendWidgets() {
    widgetStash = std::move(widgets);
    widgets.clear();
    dragWidget = -1;
    widgetResizing = false;
    hoverWidget = -1;
    contextWidget = -1;
    layoutDesktopIcons();
}

// Puts the cards back where they were. Nothing about them changed while they were
// away, so this is a move rather than a rebuild and the icon grid they had been
// reflowing around returns with them.
void Manager::restoreWidgets() {
    widgets = std::move(widgetStash);
    widgetStash.clear();
    hoverWidget = -1;
    layoutDesktopIcons();
}

void Manager::addWidget(WidgetKind kind) {
    int w = metrics::kWidgetClock, h = metrics::kWidgetClock;
    switch (kind) {
        case WidgetKind::Clock: break;
        case WidgetKind::Battery: w = metrics::kWidgetBatteryW; h = metrics::kWidgetBatteryH; break;
        case WidgetKind::Calendar:
            w = metrics::kWidgetCalendarW;
            h = metrics::kWidgetCalendarH;
            break;
        case WidgetKind::Weather: w = metrics::kWidgetWeatherW; h = metrics::kWidgetWeatherH; break;
        case WidgetKind::DigitalClock:
            w = metrics::kWidgetDigitalW;
            h = metrics::kWidgetDigitalH;
            break;
    }
    const int bottom = screenH - metrics::taskbarH;
    int x = screenW - w - metrics::kWidgetPad;
    int y = metrics::kWidgetPad;
    // Cascade down/left past the cards already on screen. Only the ones sharing this
    // page count as in the way: a card on another page is not on screen to collide
    // with, and the tablet's pages are meant to be laid out independently.
    const int page = tabletMode ? tabletHomePage : 0;
    for (int guard = 0; guard < 64; ++guard) {
        bool clash = false;
        for (const Widget& o : widgets) {
            if (o.page != page) continue;
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
    widgets.push_back(Widget{kind, Rect{x, y, w, h}, page});
    if (kind == WidgetKind::Battery) refreshBattery(true);
    else if (kind == WidgetKind::Weather) refreshWeather(true);
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
    const int bottom = screenH - metrics::taskbarH;

    // Holding a card against a screen edge turns the page, exactly as it does for a
    // lifted icon, so a card can be moved to a page it is not on. The edge state is
    // shared with the icon drag on purpose: only one of the two is ever in flight,
    // and the icon drag clears it when it ends.
    if (tabletMode && !widgetResizing) {
        const int band = metrics::kTabletDragEdgePx;
        int edge = 0;
        if (x < band) edge = -1;
        else if (x > screenW - band) edge = 1;
        if (edge != tabletDragEdge) {
            tabletDragEdge = edge;
            tabletDragEdgeAt = nowMs();
        } else if (edge != 0 && nowMs() - tabletDragEdgeAt > metrics::kTabletDragEdgeMs) {
            const int want = tabletHomePage + edge;
            if (want >= 0 && want < tabletHomePageCount && want != tabletHomePage) {
                setTabletHomePage(want);
                tabletDragEdgeAt = nowMs();
            }
        }
    }
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
    // A card dropped on a page belongs to that page, which is how one is moved from
    // one page to another. On the desktop there are no pages and the field is left
    // alone.
    if (tabletMode && !widgetResizing && dragWidget < int(widgets.size()))
        widgets[size_t(dragWidget)].page = tabletHomePage;
    dragWidget = -1;
    widgetResizing = false;
    tabletDragEdge = 0;
    ungrabPointer();
    layoutDesktopIcons();
    dirty = true;
}

void Manager::drawWidgets() {
    for (size_t i = 0; i < widgets.size(); ++i) {
        const Widget& w = widgets[i];
        // Each card is shown on its own page only, which is what keeps a page of
        // icons free of the cards that belong to the next one over. The card being
        // dragged stays drawn whichever page it has reached, or carrying it to the
        // next page would make it vanish under the finger.
        if (!widgetOnPage(w) && int(i) != dragWidget) continue;
        const bool dragging = int(i) == dragWidget;
        const bool hot = int(i) == hoverWidget || dragging;
        // The hover wash fades in, and stays lit while the card is being dragged.
        const double hv = dragging ? 1.0 : (i < widgetHover.size() ? widgetHover[i] : 0.0);
        const float radius = std::min(float(metrics::kWidgetRadius),
                                      std::min(w.rect.w, w.rect.h) * 0.28f);
        // An iOS 18 widget is a full-colour panel, not glass: a vivid two-stop
        // gradient with crisp white content sitting on it. The clock wears a fixed
        // blue-violet; the battery panel takes the colour of its reading, so the
        // card itself says whether the charge is healthy, low, or absent.
        Color top = theme::kWidgetClockTop, bottom = theme::kWidgetClockBottom;
        if (w.kind == WidgetKind::Battery) {
            if (batteryPercent < 0) {
                top = theme::kWidgetBatteryNoneTop;
                bottom = theme::kWidgetBatteryNoneBottom;
            } else if (batteryCharging || batteryFull || batteryPercent > 40) {
                top = theme::kWidgetBatteryGoodTop;
                bottom = theme::kWidgetBatteryGoodBottom;
            } else if (batteryPercent > 20) {
                top = theme::kWidgetBatteryWarnTop;
                bottom = theme::kWidgetBatteryWarnBottom;
            } else {
                top = theme::kWidgetBatteryLowTop;
                bottom = theme::kWidgetBatteryLowBottom;
            }
        } else if (w.kind == WidgetKind::Calendar) {
            top = theme::kWidgetCalendarTop;
            bottom = theme::kWidgetCalendarBottom;
        } else if (w.kind == WidgetKind::Weather) {
            weatherPalette(skyGlyphFor(weatherCondition), weatherNight, top, bottom);
        }
        // The digital clock is bare type on the wallpaper: no card, and no hover
        // wash either -- a panel appearing under the pointer would be exactly the
        // background it is meant not to have. It stays draggable and resizable; the
        // corner grip is what shows when it is hot.
        const bool bare = w.kind == WidgetKind::DigitalClock;
        if (!bare) {
            gradientPanel(comp, w.rect, radius, top, bottom);
            if (hv > 0.001) comp.drawRect(w.rect, radius, theme::kWidgetHover, float(hv));
        }
        switch (w.kind) {
            case WidgetKind::Clock: drawClockWidget(w); break;
            case WidgetKind::Battery: drawBatteryWidget(w); break;
            case WidgetKind::Calendar: drawCalendarWidget(w); break;
            case WidgetKind::Weather: drawWeatherWidget(w); break;
            case WidgetKind::DigitalClock: drawDigitalClock(w); break;
        }
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

    // Black on white: the second hand and hub are white like the rest, thin enough
    // to stay distinguishable from the minute hand without borrowing a colour.
    hand(comp, cx, cy, hourAng, R * 0.46f, std::max(3.f, R * 0.074f), R * 0.08f, theme::kWidgetHand);
    hand(comp, cx, cy, minAng, R * 0.66f, std::max(2.f, R * 0.052f), R * 0.10f, theme::kWidgetHand);
    hand(comp, cx, cy, secAng, R * 0.74f, std::max(1.f, R * 0.018f), R * 0.16f, theme::kWidgetHand);
    comp.drawRect(Rect{cx - 3, cy - 3, 6, 6}, 3.f, theme::kWidgetHand);
}

void Manager::drawBatteryWidget(const Widget& w) {
    const int pad = metrics::kWidgetPad;
    const bool has = batteryPercent >= 0;
    const int pct = has ? std::clamp(batteryPercent, 0, 100) : 0;
    // The panel's own gradient already carries the charge colour, so the ring and
    // its contents stay white: a green ring on a green card would read as mud.
    const Color ink = Color{1.f, 1.f, 1.f, 1.f};

    const int ringD = std::max(48, std::min(w.rect.h - 2 * pad - 8, int(w.rect.w * 0.42f)));
    const int rcx = w.rect.x + pad + ringD / 2;
    const int rcy = w.rect.y + w.rect.h / 2;
    const float radius = ringD * 0.5f - 10.f;
    const float thick = std::max(7.f, ringD * 0.13f);

    comp.drawArc(rcx, rcy, radius, thick, 0.f, 2.f * kPi, theme::kWidgetRingTrack, 1.f);
    if (has && pct > 0)
        comp.drawArc(rcx, rcy, radius, thick, 0.f, 2.f * kPi * pct / 100.f, ink, 1.f);

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
            if (fw > 0) comp.drawRect(Rect{tx + 3, py + 3, fw, pillH - 6}, 4.f, ink);
        }
        comp.drawRect(Rect{tx + pillW + 2, py + pillH / 2 - 5, 5, 10}, 2.f, theme::kWidgetRingTrack);
    }
}

// The iOS 18 Calendar widget: a white card, the month and year, a Sunday-first
// weekday header, and a six-week grid with today ringed in systemRed. Leading
// and trailing days of the neighbouring months are drawn faintly so the block
// reads as a whole month rather than a ragged patch.
void Manager::drawCalendarWidget(const Widget& w) {
    const int pad = metrics::kWidgetPad;
    const int gx = w.rect.x + pad;
    const int gw = std::max(1, w.rect.w - 2 * pad);

    time_t t = time(nullptr);
    struct tm lt {};
    localtime_r(&t, &lt);
    const int year = lt.tm_year + 1900;
    const int mon = lt.tm_mon;
    const int today = lt.tm_mday;

    static const char* kMonths[] = {"January",  "February", "March",    "April",
                                    "May",      "June",     "July",     "August",
                                    "September", "October",  "November", "December"};
    drawTextCentered(std::string(kMonths[mon]) + " " + std::to_string(year), 18, Weight::Medium,
                     theme::kWidgetCalText, Rect{gx, w.rect.y + pad - 3, gw, 24});

    static const char* kWeek[] = {"S", "M", "T", "W", "T", "F", "S"};
    const int cellW = std::max(1, gw / 7);
    const int gridX = gx + (gw - cellW * 7) / 2;
    const int weekY = w.rect.y + pad + 24;
    for (int c = 0; c < 7; ++c)
        drawTextCentered(kWeek[c], 11, Weight::Medium, theme::kWidgetCalMuted,
                         Rect{gridX + c * cellW, weekY, cellW, 14});

    const int divY = weekY + 17;
    comp.drawRect(Rect{gx, divY, gw, 1}, 0.f, theme::kWidgetCalLine);

    struct tm first {};
    first.tm_year = year - 1900;
    first.tm_mon = mon;
    first.tm_mday = 1;
    mktime(&first);
    const int lead = first.tm_wday;  // 0 = Sunday
    const int dim = daysInMonth(year, mon);
    const int prevDim = daysInMonth(year, (mon + 11) % 12);

    const int gridY = divY + 5;
    const int rowH = std::max(1, (w.rect.bottom() - pad - gridY) / 6);
    for (int i = 0; i < 42; ++i) {
        const int col = i % 7;
        const int rowi = i / 7;
        const int dayNo = i - lead + 1;
        const bool other = dayNo < 1 || dayNo > dim;
        const int num = dayNo < 1 ? prevDim + dayNo : (dayNo > dim ? dayNo - dim : dayNo);

        const int cx = gridX + col * cellW + cellW / 2;
        const int cy = gridY + rowi * rowH + rowH / 2;
        const bool isToday = !other && num == today;
        const int r = std::min(cellW, rowH) / 2 - 2;
        if (isToday && r > 3)
            comp.drawRect(Rect{cx - r, cy - r, 2 * r, 2 * r}, float(r), theme::kWidgetCalAccent);

        const Color ink = isToday ? theme::kWidgetCalAccentInk
                                  : (other ? theme::kWidgetCalMuted : theme::kWidgetCalText);
        drawTextCentered(std::to_string(num), std::max(10, std::min(14, rowH - 8)),
                         other ? Weight::Regular : Weight::Medium, ink,
                         Rect{cx - cellW / 2, cy - rowH / 2, cellW, rowH});
    }
}

// The iOS 18 Weather widget: a sky gradient carrying the city, a very large
// temperature, the condition, the day's high and low, a condition glyph, and a
// row of hourly readings along the bottom.
void Manager::drawWeatherWidget(const Widget& w) {
    const int pad = metrics::kWidgetPad;
    const int x0 = w.rect.x + pad;
    const int gw = std::max(1, w.rect.w - 2 * pad);

    drawTextAt(weatherCity, 15, Weight::Medium, theme::kWidgetWeatherInk, x0, w.rect.y + pad - 4);

    char temp[16];
    std::snprintf(temp, sizeof temp, "%d\u00b0", weatherTemp);
    const int bigY = w.rect.y + pad + 10;
    drawTextAt(temp, 40, Weight::Medium, theme::kWidgetWeatherInk, x0 - 2, bigY);

    drawTextAt(weatherCondition, 14, Weight::Regular, theme::kWidgetWeatherSub, x0, bigY + 46);

    char hl[32];
    std::snprintf(hl, sizeof hl, "H:%d\u00b0  L:%d\u00b0", weatherHigh, weatherLow);
    drawTextAt(hl, 13, Weight::Regular, theme::kWidgetWeatherFaint, x0, bigY + 64);

    weatherGlyph(comp, w.rect.right() - pad - 34, w.rect.y + pad + 38, 56.f,
                 skyGlyphFor(weatherCondition), theme::kWidgetWeatherInk, theme::kWidgetWeatherSun);

    const int n = int(weatherHourly.size());
    if (n > 0) {
        const int stripH = 38;
        const int stripY = w.rect.bottom() - 14 - stripH;
        const int cellW = std::max(1, gw / n);
        for (int i = 0; i < n; ++i) {
            const int cx = x0 + i * cellW + cellW / 2;
            drawTextCentered(weatherHourly[size_t(i)].first, 11, Weight::Medium,
                             theme::kWidgetWeatherSub, Rect{cx - cellW / 2, stripY, cellW, 14});
            drawTextCentered(std::to_string(weatherHourly[size_t(i)].second) + "\u00b0", 14,
                             Weight::Medium, theme::kWidgetWeatherInk,
                             Rect{cx - cellW / 2, stripY + 20, cellW, 20});
        }
    }
}

// A bedside-style digital clock: no card, just the time set huge in the Poppins
// ExtraBold display face with the date beneath it in the UI font. The point size
// is chosen to fill the widget's footprint -- as large as the width and the clock
// band both allow -- so resizing it scales the digits rather than leaving them at
// a fixed size.
void Manager::drawDigitalClock(const Widget& w) {
    const int pad = metrics::kWidgetPad;
    const int gx = w.rect.x + pad;
    const int gw = std::max(1, w.rect.w - 2 * pad);

    time_t t = time(nullptr);
    struct tm lt {};
    localtime_r(&t, &lt);
    char hhmm[16];
    std::snprintf(hhmm, sizeof hhmm, "%02d:%02d", lt.tm_hour, lt.tm_min);

    Text& display = displayText();

    // The time owns the space above the date line. Start the face at the band's
    // own height and shrink it until both the rendered height and the advance
    // width fit, which lands on the largest size the widget can carry.
    const int topPad = 10;
    const int band = std::max(24, w.rect.h - topPad - pad - 26);  // reserve the date line
    int px = band;
    int mh = 0;
    const int mw = measureIn(display, hhmm, px, Weight::Bold, &mh);
    if (mh > band || mw > gw) {
        const double s = std::min(double(band) / std::max(1, mh), double(gw) / std::max(1, mw));
        px = std::max(18, int(px * s));
    }
    const Rect timeArea{gx, w.rect.y + topPad, gw, band};
    const Rect dateArea{gx, w.rect.y + topPad + band + 4, gw, 22};

    char dateStr[64];
    strftime(dateStr, sizeof dateStr, "%A, %d %B", &lt);

    drawTextCenteredIn(display, hhmm, px, Weight::Bold, theme::kWidgetDigitalInk, timeArea);
    drawTextCentered(dateStr, 15, Weight::Medium, theme::kWidgetDigitalDim, dateArea);
}

}  // namespace wm
