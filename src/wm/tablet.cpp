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
// A long press puts the home screen into rearrangement mode: the icons jiggle
// and can be dragged from cell to cell. The wallpaper widgets are desktop only
// and are taken off the screen while this mode is up.
//
// Switching modes is not a hard cut: a full-screen splash fades in over the old
// shell, the two are swapped while it completely covers the screen, and it
// fades back out onto the new one. That is what makes the change read as a mode
// switch rather than a redraw.
#include "manager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "theme.h"

namespace wm {
namespace {

// strcasecmp without allocating. The running check asks this question of every icon
// on screen, against every open window, on every frame -- lowercasing into a fresh
// std::string each time is the wrong shape for a question asked that often.
bool sameFold(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty() || a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

// What each row of the quick actions sheet does. Held as ids rather than matched on
// the wording, so the sheet and the action that runs on release cannot drift apart.
enum TabletMenuAct { kMenuOpen = 0, kMenuAddDock, kMenuRemoveDock, kMenuRemoveHome };

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

constexpr float kTabletPi = 3.14159265358979323846f;

}  // namespace

// ---------------------------------------------------------------------------
// Transition
// ---------------------------------------------------------------------------

// Starts the splash. The actual mode swap happens in tickAnimations() once the
// splash has fully covered the screen, so the user never sees the desktop
// become the home screen.
void Manager::setTabletMode(bool on) {
    if (modeSwitching || tabletMode == on) return;
    // The splash swallows input until it finishes, so a drag in flight would never
    // see its release. Call it off here rather than lose the reorder silently.
    if (pinDrag >= 0) endPinDrag(false);
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
    closeTabletMenu();
    endTabletIconDrag();
    if (dragWidget >= 0) endWidgetDrag();
    tabletHover = -1;
    if (tabletMode) {
        // The widget cards stay: the home screen shows them, only folded onto the
        // phone and re-paged. They are laid out after the grid, below, once the
        // page count is known.
        closeTabletWidgetPicker();
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
        // The desktop's cards are sized for a monitor and would swamp the phone,
        // so they are stashed and the home screen gets a set of its own. The grid
        // is laid out once to measure the strip and the dock, then again after the
        // cards are placed so each page can make room for its own widgets.
        suspendWidgets();
        buildTabletEntries();
        layoutTabletHome();
        initTabletWidgets();
        layoutTabletHome();
        snapTabletIconSprings();
    } else {
        tabletEdit = false;
        closeTabletWidgetPicker();
        // The app drawer belongs to the tablet shell, so it goes with it.
        tabletDrawer = false;
        tabletDrawerAnim = 0.0;
        tabletDrawerDragging = false;
        tabletDrawerPullArmed = false;
        tabletDrawerSpring.value = 0.0;
        tabletDrawerSpring.velocity = 0.0;
        tabletDrawerSwipe = false;
        tabletDrawerArmed = false;
        tabletDrawerPressIndex = -1;
        // The phone's own arrangement is kept for next time before the desktop's
        // cards, stashed on entry, come back in their place.
        saveTabletWidgets(widgets);
        restoreWidgets();
        // Leaving the mode abandons any half-turned page and keeps the page the user
        // was on, so the home screen comes back the way they left it.
        tabletPageSwipe = false;
        tabletPageOffset = double(tabletHomePage);
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
// The home screen as the user left it. Reading and writing it lives with the rest
// of the shell's saved settings in apps.cpp; these two are the bridge between
// that plain data and the live members.
bool Manager::loadTabletLayout() {
    const TabletLayout layout = loadTabletLayoutFile();
    if (layout.dock.empty() && layout.home.empty()) return false;
    tabletDock = layout.dock;
    tabletHome = layout.home;
    return true;
}

void Manager::saveTabletLayout() {
    TabletLayout layout;
    layout.dock = tabletDock;
    layout.home = tabletHome;
    saveTabletLayoutFile(layout);
}

void Manager::buildTabletEntries() {
    tabletHome.clear();
    tabletDock.clear();
    tabletHomePage = 0;
    tabletHomePageCount = 1;
    tabletHomeFirst = 0;
    tabletPageOffset = 0.0;
    tabletPageSpring.value = 0.0;
    tabletPageSpring.velocity = 0.0;
    tabletPageSwipe = false;
    tabletPageFirst.clear();
    tabletPageTop.clear();
    tabletPageCapacity.clear();

    // The arrangement the user left behind wins over the one this session's
    // Desktop directory would suggest. Nothing is written here on purpose: until
    // the home screen is actually rearranged, a newly installed launcher still
    // shows up on the next run.
    if (loadTabletLayout()) {
        tabletEntriesBuilt = true;
        return;
    }

    // The home screen is fed solely from the desktop's "tablet" folder: a
    // curated directory of .desktop launchers, and nothing else counts. The
    // folder is created on first use so there is always somewhere a launcher
    // can be dropped.
    ensureTabletFolder();
    std::vector<TabletEntry> found;
    for (const DesktopItem& d : scanTabletFolder()) {
        TabletEntry e;
        e.name = d.name;
        e.icon = d.icon;
        e.exec = d.exec;
        e.path = d.path;
        found.push_back(e);
    }

    // The dock holds its own copies of apps, and iOS is strict about this: what is
    // in the dock is *not* also on the grid, so nothing appears twice on one screen.
    // The grid keeps at least one icon, so a session with only a handful of
    // launchers still has something to rearrange.
    const size_t dockCount =
        std::min<size_t>(metrics::kTabletDockFill, found.empty() ? 0 : found.size() - 1);
    tabletDock.assign(found.begin(), found.begin() + long(dockCount));
    for (size_t i = dockCount; i < found.size(); ++i) {
        TabletItem item;
        item.app = found[i];
        tabletHome.push_back(std::move(item));
    }
    tabletEntriesBuilt = true;
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void Manager::layoutTabletHome() {
    if (!tabletEntriesBuilt) buildTabletEntries();
    // Every cell carries a stable id so the reflow springs can follow an icon
    // through a reorder; a freshly built or spliced item has none yet.
    for (TabletItem& item : tabletHome)
        if (item.uid == 0) item.uid = tabletNextUid++;
    tabletHomeRects.clear();
    tabletDockRects.clear();

    tabletStatusRect = Rect{0, 0, screenW, metrics::kTabletStatusH};
    tabletHomeBarRect = Rect{(screenW - metrics::kTabletHomeBarW) / 2,
                             screenH - 14, metrics::kTabletHomeBarW, metrics::kTabletHomeBarH};
    // --- dock: a glass pill of squircle app icons, above the home bar -------
    // The dock is edited where it stands, the way HarmonyOS does it: while the
    // home screen is in rearrange mode every icon grows a remove badge. New apps
    // get in through the long press menu's "Add to Dock" instead of a tile that
    // would grow the pill. The row here is the physics' raw material: the base
    // slot each icon rests in, which the per-frame spring step deflects, magnifies
    // and bounces into the live tabletDockRects that everything reads.
    const int di = std::clamp(std::min(screenW, screenH) / 11, 48, metrics::kTabletDockIcon);
    tabletDockIconSize = di;
    tabletDockRemoveRects.clear();
    const size_t slots = tabletDock.size();
    if (slots > 0) {
        const int gap = std::max(14, di / 2);
        const int padX = std::max(16, di / 3);
        const int padY = std::max(12, di / 4);
        const int contentW = int(slots) * di + (int(slots) - 1) * gap;
        const int dockW = std::min(screenW - 32, contentW + 2 * padX);
        const int dockH = di + 2 * padY;
        const int dockX = (screenW - dockW) / 2;
        const int dockY = screenH - 30 - dockH;
        tabletDockRect = Rect{dockX, dockY, dockW, dockH};
        const int badge = std::max(20, di / 3);
        int x = dockX + (dockW - contentW) / 2;
        tabletDockBaseRects.clear();
        tabletDockScale.assign(slots, 1.0);
        tabletDockDrift.assign(slots, 0.0);
        tabletDockDriftV.assign(slots, 0.0);
        tabletDockWobble.assign(slots, 1.0);
        tabletDockWobbleAge.assign(slots, -1.0);
        tabletDockRects.clear();
        for (size_t i = 0; i < slots; ++i) {
            const Rect box{x, dockY + padY, di, di};
            tabletDockBaseRects.push_back(box);
            tabletDockRects.push_back(box);
            // The badge rides the icon's top-right corner and hangs half outside
            // it, which is what makes it big enough to hit with a fingertip.
            if (tabletEdit)
                tabletDockRemoveRects.push_back(
                    Rect{box.right() - badge / 2, box.y - badge / 2, badge, badge});
            x += di + gap;
        }
    } else {
        tabletDockRect = Rect{};
    }

    // --- home grid: squircle + label cells, top aligned, as many pages of them
    // as the apps need --------------------------------------------------
    const int icon = std::clamp(std::min(screenW, screenH) / 11, metrics::kTabletIconMin, 84);
    tabletIconSize = icon;
    const int cellW = icon + icon / 2 + 24;
    const int cellH = icon + 34;
    const int marginX = std::max(18, screenW / 18);
    const int cols = std::max(3, (screenW - 2 * marginX) / cellW);
    const int topBase = tabletStatusRect.bottom() + 24;
    const int gridBottom = tabletDockRect.empty() ? screenH - 46 : tabletDockRect.y - 18;
    const int dotsH = metrics::kTabletPageDotsH;

    // A widget takes the top of the page it lives on, and only that page. The
    // icons of every other page keep the whole area, which is the Android home
    // screen rule: cards and apps share a page, never the whole screen at once.
    auto widgetBottomOn = [&](int page) {
        int top = topBase;
        for (const Widget& w : widgets) {
            if (w.page != page) continue;
            top = std::max(top, w.rect.bottom() + metrics::kTabletWidgetGap);
        }
        return top;
    };
    auto hasWidgetOn = [&](int page) {
        for (const Widget& w : widgets)
            if (w.page == page) return true;
        return false;
    };
    int maxWidgetPage = 0;
    for (const Widget& w : widgets) maxWidgetPage = std::max(maxWidgetPage, w.page);
    maxWidgetPage = std::min(maxWidgetPage, 16);

    // Pack the apps page by page. A page whose cards leave no room holds none,
    // which is how a widget-only page exists at all; the apps carry on to the
    // next page instead of being painted underneath a card.
    auto packPages = [&](bool reserveDots) {
        tabletPageFirst.clear();
        tabletPageTop.clear();
        tabletPageCapacity.clear();
        const int reserved = reserveDots ? dotsH : 0;
        const int count = int(tabletHome.size());
        int first = 0;
        int page = 0;
        do {
            const int top = widgetBottomOn(page);
            const int rows = (gridBottom - reserved - top) / cellH;
            int capacity = std::max(0, rows) * cols;
            if (capacity == 0 && !hasWidgetOn(page)) capacity = cols;
            tabletPageFirst.push_back(first);
            tabletPageTop.push_back(top);
            tabletPageCapacity.push_back(capacity);
            first = std::min(count, first + capacity);
            ++page;
        } while (page < 64 && (first < count || page <= maxWidgetPage));
    };
    packPages(false);
    if (tabletPageFirst.size() > 1) packPages(true);
    tabletHomePageCount = std::max<int>(1, int(tabletPageFirst.size()));

    tabletGridCols = cols;
    tabletGridCellW = cellW;
    tabletGridCellH = cellH;
    tabletGridX = (screenW - cols * cellW) / 2;
    // A page slides by its own width plus a gap, which is what a fingertip
    // travelling across the screen is taken to mean.
    tabletGridStride = cols * cellW + cellW / 2;

    tabletHomePage = std::clamp(tabletHomePage, 0, tabletHomePageCount - 1);
    tabletHomeFirst = tabletPageFirst[size_t(tabletHomePage)];
    tabletHomePerPage = tabletPageCapacity[size_t(tabletHomePage)];
    tabletGridTop = tabletPageTop[size_t(tabletHomePage)];
    // A card saved for a page that no longer exists comes back on the last one.
    for (Widget& w : widgets) w.page = std::clamp(w.page, 0, tabletHomePageCount - 1);

    // Only the page on screen is laid out; the others are computed where they are
    // drawn, and only when they are actually coming into view.
    for (int k = 0; k < tabletHomePerPage; ++k) {
        if (tabletHomeFirst + k >= int(tabletHome.size())) break;
        tabletHomeRects.push_back(tabletGridCell(k));
    }

    // The page dots, centred in the strip between the grid and the dock.
    tabletPageDots.clear();
    if (tabletHomePageCount > 1) {
        const int d = 8, gap = 14;
        const int totalW = tabletHomePageCount * gap - (gap - d);
        const int dx0 = (screenW - totalW) / 2;
        const int dy = gridBottom - dotsH + (dotsH - d) / 2;
        for (int i = 0; i < tabletHomePageCount; ++i)
            tabletPageDots.push_back(Rect{dx0 + i * gap, dy, d, d});
    }

    // The rearrange-mode "+": a round button in the top-right corner, where the
    // home screen keeps the way to put a widget on the page. It only exists while
    // the screen is being rearranged.
    tabletWidgetAddRect = Rect{};
    if (tabletEdit) {
        const int s = std::max(32, tabletIconSize * 3 / 5);
        const int m = 10;
        tabletWidgetAddRect = Rect{screenW - m - s, tabletStatusRect.bottom() + m, s, s};
    }

    if (tabletFolderOpen >= 0) layoutTabletFolder();
}

// Where icon k of a page sits. The grid geometry is kept rather than recomputed so
// that a page which is not on screen can still be drawn in the right place while it
// slides in.
Rect Manager::tabletGridCell(int k) const {
    const int col = k % tabletGridCols;
    const int row = k / tabletGridCols;
    return Rect{tabletGridX + col * tabletGridCellW, tabletGridTop + row * tabletGridCellH,
                tabletGridCellW, tabletGridCellH};
}

// ---------------------------------------------------------------------------
// Grid reflow springs: the icons glide aside instead of snapping
// ---------------------------------------------------------------------------
// Every icon's cell is a spring keyed by the item's stable uid, so a rearrange
// is a physical reflow rather than a jump. This is the home screen's answer to
// the desktop grid's animateDesktopIconReflow(): while an icon is carried over a
// cell, the icons between its old slot and the new one are eased one cell over,
// and they settle back with the shell's playful spring once the drop is made.

// Drop every visible icon's spring straight onto its cell. Used when the grid is
// rebuilt or a page turns, where easing in from the previous page's positions
// would read as a slide rather than a page.
void Manager::snapTabletIconSprings() {
    for (int k = 0; k < int(tabletHomeRects.size()); ++k) {
        const int index = tabletHomeFirst + k;
        if (index >= int(tabletHome.size())) break;
        const int uid = tabletHome[size_t(index)].uid;
        if (uid == 0) continue;
        const Rect& c = tabletHomeRects[size_t(k)];
        motion::Spring2 sp;
        sp.configure(motion::kPlayful.frequency, motion::kPlayful.zeta);
        sp.reset(motion::Vec2{double(c.x), double(c.y)});
        tabletIconSpring[uid] = sp;
    }
}

void Manager::animateTabletIconReflow(double dtMs) {
    if (!tabletMode || tabletHome.empty() || tabletHomeRects.empty()) return;
    const double dt = dtMs / 1000.0;
    const bool dragging = tabletDragIcon >= 0 || tabletDragFromDock || tabletDragFromFolder;
    const int from = tabletDragIcon;
    const int to = tabletDragTarget;
    // Only a plain grid drag parts the row: a card lifted from the dock or a
    // folder has no slot to make room for, and a drop on a folder or the dock is
    // not a place on the grid either.
    const bool gridDrag = dragging && from >= 0 && to >= 0 && !tabletDragFromDock &&
                          !tabletDragFromFolder;
    const int slots = int(tabletHomeRects.size());
    bool moving = false;
    for (int k = 0; k < slots; ++k) {
        const int index = tabletHomeFirst + k;
        if (index >= int(tabletHome.size())) break;
        const int uid = tabletHome[size_t(index)].uid;
        if (uid == 0) continue;
        // The carried icon rides the pointer, not its cell, so it has no spring
        // target until it is dropped.
        if (gridDrag && index == from) continue;
        int disp = k;
        if (gridDrag) {
            if (from < to && index > from && index <= to) disp = k - 1;
            else if (from > to && index >= to && index < from) disp = k + 1;
        }
        disp = clampi(disp, 0, slots - 1);
        const Rect& target = tabletHomeRects[size_t(disp)];
        const motion::Vec2 goal{double(target.x), double(target.y)};
        auto it = tabletIconSpring.find(uid);
        if (it == tabletIconSpring.end()) {
            motion::Spring2 sp;
            sp.configure(motion::kPlayful.frequency, motion::kPlayful.zeta);
            sp.reset(goal);
            tabletIconSpring[uid] = sp;
            continue;
        }
        it->second.step(goal, dt);
        if (!it->second.settled(goal, 0.35)) moving = true;
    }
    if (moving) dirty = true;
}

// Turning to a page: the grid is laid out for it, and an open folder is closed
// because the sheet it belongs to is on the page being left behind.
void Manager::setTabletHomePage(int page) {
    const int clamped = std::clamp(page, 0, tabletHomePageCount - 1);
    if (clamped == tabletHomePage) return;
    tabletHomePage = clamped;
    if (tabletFolderOpen >= 0) closeTabletFolder();
    tabletHover = -1;
    tabletDragTarget = -1;
    layoutTabletHome();
    snapTabletIconSprings();
    dirty = true;
}

// The open folder: a rounded sheet holding its apps nine at a time in a 3x3, the
// same nine-per-page grid the closed folder icon shows. The sheet is inset from
// the status bar and the home indicator so both stay tappable underneath.
void Manager::layoutTabletFolder() {
    tabletFolderRects.clear();
    if (tabletFolderOpen < 0 || tabletFolderOpen >= int(tabletHome.size())) return;
    const TabletFolder& f = tabletHome[size_t(tabletFolderOpen)].folder;

    tabletFolderPageCount =
        std::max(1, (int(f.apps.size()) + metrics::kTabletFolderPerPage - 1) /
                    metrics::kTabletFolderPerPage);
    tabletFolderPage = std::clamp(tabletFolderPage, 0, tabletFolderPageCount - 1);

    const int sheetW = std::min(screenW - 32, std::max(360, screenW * 78 / 100));
    const int mini = std::clamp((sheetW - 2 * 24) / 3, 44, metrics::kTabletFolderMiniMax);
    const int sheetH = 24 + mini + 34 + 24 + metrics::kTabletFolderPerPage / 3 *
                                              (mini + mini / 3 + 10) +
                       34;
    const int sheetX = (screenW - sheetW) / 2;
    const int sheetY = std::max(metrics::kTabletStatusH + 12, (screenH - sheetH) / 2);
    tabletFolderPanel = Rect{sheetX, sheetY, sheetW, sheetH};

    // The 3x3 block, centred in the sheet under the folder's name.
    const int gap = mini / 3;
    const int step = mini + gap;
    const int blockW = 3 * mini + 2 * gap;
    const int blockX = sheetX + (sheetW - blockW) / 2;
    const int blockY = sheetY + 24 + mini + 26;
    const int first = tabletFolderPage * metrics::kTabletFolderPerPage;
    for (int i = 0; i < metrics::kTabletFolderPerPage; ++i) {
        const int index = first + i;
        if (index >= int(f.apps.size())) break;
        const Rect cell{blockX + (i % 3) * step, blockY + (i / 3) * (step + 10), mini, mini};
        tabletFolderRects.push_back(cell);
    }
}

void Manager::openTabletFolder(int index) {
    if (index < 0 || index >= int(tabletHome.size())) return;
    if (!tabletHome[size_t(index)].isFolder) return;
    tabletFolderOpen = index;
    tabletFolderLast = index;
    tabletFolderPage = 0;
    tabletFolderAnim = 0.0;
    tabletHover = -1;
    layoutTabletFolder();
    dirty = true;
}

void Manager::closeTabletFolder() {
    if (tabletFolderOpen < 0) return;
    // The sheet is left in place and drawn fading out; tickAnimations() clears it
    // once the animation is done. Dropping it here would cut the close off.
    tabletFolderLast = tabletFolderOpen;
    tabletFolderOpen = -1;
    tabletFolderPage = 0;
    dirty = true;
}

// iOS suggests a name from what it thinks is inside; there is no category
// database to consult here, so this is the longest word its apps have in common,
// falling back to "Folder" when they share nothing at all.
std::string Manager::suggestFolderName(const std::vector<TabletEntry>& apps) {
    if (apps.empty()) return "Folder";
    // Only a word every app carries can describe the group, so the first word is
    // tried alone and then with one more word appended.
    for (size_t words = 1; words <= 2; ++words) {
        std::string shared;
        bool first = true;
        for (const TabletEntry& a : apps) {
            std::string lower;
            lower.reserve(a.name.size());
            for (char ch : a.name)
                lower.push_back(char(std::tolower(static_cast<unsigned char>(ch))));
            // Split on spaces and keep only alphanumeric words: "Files" and
            // "Text Editor" must not agree on the word "editor" by accident alone.
            std::vector<std::string> words_in;
            std::string cur;
            for (char ch : lower) {
                if (std::isalnum(static_cast<unsigned char>(ch))) {
                    cur.push_back(ch);
                } else if (!cur.empty()) {
                    words_in.push_back(cur);
                    cur.clear();
                }
            }
            if (!cur.empty()) words_in.push_back(cur);
            // Words that only describe the app rather than the group.
            static const char* kNoise[] = {"app", "application", "the", "of", "and", "for"};
            for (std::string& w : words_in) {
                bool noise = false;
                for (const char* n : kNoise)
                    if (w == n) noise = true;
                if (noise) w.clear();
            }
            std::string candidate;
            for (size_t i = 0; i < words_in.size() && i < words; ++i) {
                if (words_in[i].empty()) continue;
                if (!candidate.empty()) candidate.push_back(' ');
                candidate += words_in[i];
            }
            if (candidate.empty()) {
                shared.clear();
                break;
            }
            if (first) {
                shared = candidate;
                first = false;
            } else if (candidate != shared) {
                shared.clear();
                break;
            }
        }
        if (!shared.empty()) {
            // Title case, the way a folder name is shown.
            std::string out;
            bool up = true;
            for (char ch : shared) {
                out.push_back(up ? char(std::toupper(static_cast<unsigned char>(ch))) : ch);
                up = (ch == ' ');
            }
            return out;
        }
    }
    return "Folder";
}

// ---------------------------------------------------------------------------
// The dock as a place you can put apps in and take them out again
// ---------------------------------------------------------------------------

// How many icons the dock row has room for: whatever fits across the screen at the
// current icon size, up to the ceiling. The row is the real limit, so a narrow
// screen gets a shorter dock rather than an overflowing one.
int Manager::tabletDockCapacity() const {
    const int di = tabletDockIconSize > 0 ? tabletDockIconSize : metrics::kTabletDockIcon;
    const int gap = std::max(14, di / 2);
    const int padX = std::max(16, di / 3);
    // The same 32px of screen margin the dock pill is laid out inside.
    const int room = screenW - 32 - 2 * padX;
    const int cols = (room + gap) / (di + gap);
    return std::clamp(cols, 1, metrics::kTabletDockMax);
}

// The picker sheet: a panel of app tiles, twelve to a page, with the page dots and
// the Done button that commits the selection.
void Manager::layoutTabletDockPicker() {
    tabletDockPickerRects.clear();
    tabletDockPickerDots.clear();
    if (!tabletDockPickerOpen) return;

    constexpr int cols = metrics::kTabletDockPickerCols;
    constexpr int per = metrics::kTabletDockPickerPerPage;
    tabletDockPickerPageCount =
        std::max(1, (int(tabletDockPickerApps.size()) + per - 1) / per);
    tabletDockPickerPage = std::clamp(tabletDockPickerPage, 0, tabletDockPickerPageCount - 1);

    const int panelW = std::min(screenW - 32, std::max(420, screenW * 72 / 100));
    const int pad = 20;
    const int tileW = (panelW - 2 * pad) / cols;
    const int mini = std::clamp(tileW - 30, 40, metrics::kTabletFolderMiniMax);
    const int rows = (per + cols - 1) / cols;
    const int titleH = 44;
    const int panelH = titleH + rows * (mini + 28) + 64;
    const int panelX = (screenW - panelW) / 2;
    const int panelY = std::max(metrics::kTabletStatusH + 12, (screenH - panelH) / 2);
    tabletDockPickerPanel = Rect{panelX, panelY, panelW, panelH};

    const int first = tabletDockPickerPage * per;
    for (int i = 0; i < per; ++i) {
        const int index = first + i;
        if (index >= int(tabletDockPickerApps.size())) break;
        const int col = i % cols, row = i / cols;
        const int tx = panelX + pad + col * tileW + (tileW - mini) / 2;
        const int ty = panelY + titleH + row * (mini + 28);
        tabletDockPickerRects.push_back(Rect{tx, ty, mini, mini});
    }

    // Done sits bottom right, where HarmonyOS puts it.
    const int doneW = 96, doneH = 40;
    tabletDockPickerDone = Rect{panelX + panelW - pad - doneW, panelY + panelH - 16 - doneH,
                                 doneW, doneH};

    // Page dots, centred under the grid and only when there is more than one page.
    if (tabletDockPickerPageCount > 1) {
        const int d = 8, gap = 12;
        const int totalW = tabletDockPickerPageCount * d + (tabletDockPickerPageCount - 1) * (gap - d);
        int x = panelX + (panelW - totalW) / 2;
        const int y = tabletDockPickerDone.y - 6 - d;
        for (int i = 0; i < tabletDockPickerPageCount; ++i) {
            tabletDockPickerDots.push_back(Rect{x, y, d, d});
            x += gap;
        }
    }
}

void Manager::openTabletDockPicker() {
    if (tabletDockPickerOpen) return;
    tabletDockPickerOpen = true;
    tabletDockPickerPage = 0;
    tabletDockPickerHover = -1;
    tabletDockPickerPress = -1;
    tabletDockPickerDonePress = false;
    // Every launcher on the machine that is not in the dock yet, which is what the
    // HarmonyOS picker lists: the installed apps, not the ones already on a page.
    tabletDockPickerApps.clear();
    for (const AppEntry& a : apps) {
        TabletEntry e;
        e.name = a.name;
        e.icon = a.icon;
        e.wmClass = a.wmClass;
        e.exec = a.exec;
        if (e.name.empty()) continue;
        bool known = false;
        for (const TabletEntry& d : tabletDock)
            if (sameTabletApp(d, e)) known = true;
        if (known) continue;
        for (const TabletEntry& d : tabletDockPickerApps)
            if (sameTabletApp(d, e)) known = true;
        if (known) continue;
        tabletDockPickerApps.push_back(std::move(e));
    }
    // A list of everything can only be in one order, and this is the one.
    std::sort(tabletDockPickerApps.begin(), tabletDockPickerApps.end(),
              [](const TabletEntry& a, const TabletEntry& b) {
                  std::string la, lb;
                  for (char c : a.name) la.push_back(char(std::tolower((unsigned char)c)));
                  for (char c : b.name) lb.push_back(char(std::tolower((unsigned char)c)));
                  return la == lb ? a.name < b.name : la < lb;
              });
    tabletDockPickerSel.assign(tabletDockPickerApps.size(), 0);
    layoutTabletDockPicker();
    dirty = true;
}

void Manager::closeTabletDockPicker() {
    if (!tabletDockPickerOpen) return;
    tabletDockPickerOpen = false;
    tabletDockPickerApps.clear();
    tabletDockPickerSel.clear();
    tabletDockPickerRects.clear();
    tabletDockPickerDots.clear();
    tabletDockPickerHover = -1;
    tabletDockPickerPress = -1;
    tabletDockPickerDonePress = false;
    dirty = true;
}

// Done: every ticked app joins the dock, up to what the row has room for.
void Manager::addTabletDockApps() {
    int room = tabletDockCapacity() - int(tabletDock.size());
    bool changed = false;
    for (size_t i = 0; i < tabletDockPickerSel.size() && room > 0; ++i) {
        if (!tabletDockPickerSel[i]) continue;
        TabletEntry e = tabletDockPickerApps[i];
        // An app on the grid or in a folder is moved into the dock rather than
        // copied into it, which is what dragging one there does too.
        removeTabletAppFromHome(tabletHome, e);
        tabletDock.push_back(std::move(e));
        --room;
        changed = true;
    }
    if (changed) {
        saveTabletLayout();
        layoutTabletHome();
    }
    closeTabletDockPicker();
}

// A remove badge: the app leaves the dock. It is not lost, though -- if it is
// nowhere else on the home screen it goes back on the grid, which is where a dock
// app came from in the first place.
void Manager::removeTabletDockApp(int index) {
    if (index < 0 || index >= int(tabletDock.size())) return;
    const TabletEntry gone = tabletDock[size_t(index)];
    tabletDock.erase(tabletDock.begin() + index);

    bool onGrid = false;
    for (const TabletItem& item : tabletHome) {
        if (!item.isFolder && sameTabletApp(item.app, gone)) onGrid = true;
        if (!item.isFolder) continue;
        for (const TabletEntry& e : item.folder.apps)
            if (sameTabletApp(e, gone)) onGrid = true;
    }
    if (!onGrid) {
        TabletItem item;
        item.app = gone;
        tabletHome.push_back(std::move(item));
    }
    saveTabletLayout();
    layoutTabletHome();
    dirty = true;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

// The device outline used by the Control Centre tile: a rounded phone drawn from
// four hairlines, so no icon asset is needed.
// ---------------------------------------------------------------------------
// Quick actions: what a long press that holds still opens
// ---------------------------------------------------------------------------

// The rows, in order. One function builds them for both the sheet and the action, so
// "row 2" means the same thing in both places.
//
// Folders are deliberately not here: holding a folder has always been how it gets
// renamed, and taking that away to offer a menu instead would be a worse trade. Its
// apps come out by being dragged out, which dissolves it once the last one leaves.
void Manager::tabletMenuItems(std::vector<std::string>& labels, std::vector<char>& danger,
                              std::vector<int>& acts) const {
    labels.clear();
    danger.clear();
    acts.clear();
    const TabletEntry* app = nullptr;
    if (tabletMenuDock >= 0 && tabletMenuDock < int(tabletDock.size())) {
        app = &tabletDock[size_t(tabletMenuDock)];
    } else if (tabletMenuHome >= 0 && tabletMenuHome < int(tabletHome.size()) &&
               !tabletHome[size_t(tabletMenuHome)].isFolder) {
        app = &tabletHome[size_t(tabletMenuHome)].app;
    }
    if (!app) return;

    const auto add = [&](const char* label, TabletMenuAct act, bool harmful) {
        labels.push_back(label);
        acts.push_back(int(act));
        danger.push_back(harmful ? 1 : 0);
    };
    add("Open", kMenuOpen, false);
    if (tabletMenuDock >= 0) {
        // Not marked harmful: the app goes back to the grid, it is not thrown away.
        add("Remove from Dock", kMenuRemoveDock, false);
    } else if (int(tabletDock.size()) < tabletDockCapacity()) {
        add("Add to Dock", kMenuAddDock, false);
    }
    // The app is still installed after this. It is only the icon that goes, which is
    // why it is the one row worded as a loss and coloured apart from the rest.
    add("Remove from Home Screen", kMenuRemoveHome, true);
}

void Manager::openTabletMenu(int dockIndex, int homeIndex) {
    tabletMenuDock = dockIndex;
    tabletMenuHome = homeIndex;
    tabletMenu = true;
    tabletMenuItem = -1;
    layoutTabletMenu();
    dirty = true;
}

void Manager::closeTabletMenu() {
    tabletMenu = false;
    tabletMenuItem = -1;
    tabletMenuDock = -1;
    tabletMenuHome = -1;
    tabletMenuRows.clear();
    tabletMenuLabels.clear();
    tabletMenuDanger.clear();
    dirty = true;
}

void Manager::layoutTabletMenu() {
    tabletMenuRows.clear();
    tabletMenuLabels.clear();
    tabletMenuDanger.clear();
    if (!tabletMenu) return;

    std::vector<int> acts;
    tabletMenuItems(tabletMenuLabels, tabletMenuDanger, acts);
    if (tabletMenuLabels.empty()) {  // nothing to act on: do not show an empty sheet
        tabletMenu = false;
        return;
    }

    const int panelW = std::min(
        screenW - 32, std::clamp(screenW * metrics::kTabletMenuWidthPct / 100,
                                 metrics::kTabletMenuMinW, metrics::kTabletMenuMaxW));
    const int pad = metrics::kTabletMenuPad;
    const int panelH = metrics::kTabletMenuHeadH +
                       int(tabletMenuLabels.size()) * metrics::kTabletMenuRowH + 2 * pad;
    const int panelX = (screenW - panelW) / 2;
    const int panelY =
        std::max(metrics::kTabletStatusH + 12, (screenH - panelH) / 2);
    tabletMenuPanel = Rect{panelX, panelY, panelW, panelH};

    for (size_t i = 0; i < tabletMenuLabels.size(); ++i) {
        tabletMenuRows.push_back(
            Rect{panelX + pad,
                 panelY + metrics::kTabletMenuHeadH + int(i) * metrics::kTabletMenuRowH,
                 panelW - 2 * pad, metrics::kTabletMenuRowH});
    }
}

void Manager::drawTabletMenuView() {
    if (!tabletMenu) return;
    const float a = 1.0f;
    // The icon and name the sheet is about, up top.
    TabletEntry app;
    if (tabletMenuDock >= 0 && tabletMenuDock < int(tabletDock.size()))
        app = tabletDock[size_t(tabletMenuDock)];
    else if (tabletMenuHome >= 0 && tabletMenuHome < int(tabletHome.size()))
        app = tabletHome[size_t(tabletMenuHome)].app;

    // A sheet over the home screen rather than a popup hanging off the icon: it stays
    // the same size and place whichever icon it was opened from, which is what makes
    // it usable with one finger.
    const float radius = float(std::min(tabletMenuPanel.w, tabletMenuPanel.h)) * 0.12f;
    comp.drawAcrylic(tabletMenuPanel, radius, theme::kTabletFolderSheet, 0.97f,
                     theme::kTabletPanelBorder, a);

    const int icon = std::min<int>(metrics::kTabletMenuHeadH - 20, 58);
    const int iy = tabletMenuPanel.y + (metrics::kTabletMenuHeadH - icon) / 2;
    const Rect box{tabletMenuPanel.x + metrics::kTabletMenuPad, iy, icon, icon};
    const float ir = float(icon) * metrics::kTabletIconRadius;
    if (!drawAppIcon(box, app.icon, app.wmClass, ir, a))
        drawAppTile(box, app.name, ir, tabletTint(app.name), false);

    const int nameX = box.right() + 14;
    const int nameMax = tabletMenuPanel.right() - metrics::kTabletMenuPad - nameX;
    const TextTex name =
        text.get(tabletFit(text, app.name, 17, nameMax), 17, Weight::Medium);
    if (name.tex)
        comp.drawText(name,
                      Rect{nameX, iy + icon / 2 - name.h / 2, name.w, name.h},
                      alpha(theme::kTabletLabel, a), 1.0f);

    for (size_t i = 0; i < tabletMenuRows.size(); ++i) {
        const Rect row = tabletMenuRows[i];
        if (int(i) == tabletMenuItem)
            comp.drawRect(row, 10.f, theme::kTabletIconHover, a * 0.9f);
        const Color c =
            tabletMenuDanger[i] ? theme::kTabletMenuDanger : theme::kTabletLabel;
        const TextTex t = text.get(tabletFit(text, tabletMenuLabels[i], 15, row.w - 24), 15,
                                   Weight::Regular);
        if (!t.tex) continue;
        comp.drawText(t, Rect{row.x + 14, row.y + (row.h - t.h) / 2, t.w, t.h},
                      alpha(c, a), 1.0f);
    }
}

bool Manager::runTabletMenuAction(int row) {
    if (row < 0 || row >= int(tabletMenuLabels.size())) return false;
    std::vector<std::string> labels;
    std::vector<char> danger;
    std::vector<int> acts;
    tabletMenuItems(labels, danger, acts);
    if (row >= int(acts.size())) return false;

    const TabletMenuAct act = TabletMenuAct(acts[size_t(row)]);
    const int dock = tabletMenuDock;
    const int home = tabletMenuHome;
    TabletEntry app;
    if (dock >= 0 && dock < int(tabletDock.size()))
        app = tabletDock[size_t(dock)];
    else if (home >= 0 && home < int(tabletHome.size()))
        app = tabletHome[size_t(home)].app;

    // Put it away first: every action below re-reads the layout, and leaving the
    // sheet up while it changes would leave its rows pointing at the wrong things.
    closeTabletMenu();

    switch (act) {
        case kMenuOpen:
            openTabletEntry(app);
            return true;
        case kMenuAddDock: {
            if (int(tabletDock.size()) >= tabletDockCapacity()) break;
            // An app is shown once, so adding it here takes it off the grid.
            if (home >= 0 && home < int(tabletHome.size()))
                tabletHome.erase(tabletHome.begin() + home);
            tabletDock.push_back(app);
            break;
        }
        case kMenuRemoveDock: {
            if (dock < 0 || dock >= int(tabletDock.size())) break;
            tabletDock.erase(tabletDock.begin() + dock);
            // Back to the grid, unless it is already somewhere else on the screen.
            bool onGrid = false;
            for (const TabletItem& item : tabletHome) {
                if (!item.isFolder) {
                    if (sameTabletApp(item.app, app)) onGrid = true;
                    continue;
                }
                for (const TabletEntry& e : item.folder.apps)
                    if (sameTabletApp(e, app)) onGrid = true;
            }
            if (!onGrid) {
                TabletItem item;
                item.app = app;
                tabletHome.push_back(std::move(item));
            }
            break;
        }
        case kMenuRemoveHome:
            if (home >= 0 && home < int(tabletHome.size()))
                tabletHome.erase(tabletHome.begin() + home);
            break;
    }

    layoutTabletHome();
    saveTabletLayout();
    dirty = true;
    return true;
}

// The small round badge the dock carries while rearranging: the remove "x" in an
// icon's corner, and the row's "+" tile. Drawn as a filled circle with a glyph so
// it needs no icon asset and stays legible at any dock size.
void Manager::drawTabletDockBadge(const Rect& box, const char* glyph, float a, bool tile) {
    if (tile) {
        comp.drawRect(box, float(box.w) * metrics::kTabletIconRadius, theme::kTabletBadgeTile, a);
        comp.drawRect(box.inflated(-2), float(box.w) * metrics::kTabletIconRadius,
                      theme::kTabletPanelBorder, a * 0.5f);
    } else {
        comp.drawRect(box, float(std::min(box.w, box.h)) * 0.5f, theme::kTabletBadgeFill, a);
    }
    drawTextCentered(glyph, tile ? 30 : 14, Weight::Bold, alpha(theme::kTabletBadgeGlyph, a),
                     box);
}

// The app picker: everything installed that is not in the dock yet, twelve to a
// page, a tick on the ones to add, and Done to commit them.
void Manager::drawTabletDockPickerView() {
    if (!tabletDockPickerOpen) return;
    const float a = 1.0f;
    const Rect& panel = tabletDockPickerPanel;
    const float radius = float(std::min(panel.w, panel.h)) * 0.10f;
    comp.drawAcrylic(panel, radius, theme::kTabletFolderSheet, 0.96f,
                     theme::kTabletPanelBorder, a);

    drawTextAt("Add to Dock", 18, Weight::Medium, alpha(theme::kTabletLabel, a), panel.x + 20,
               panel.y + 16);
    const int room = tabletDockCapacity() - int(tabletDock.size());
    if (tabletDockPickerApps.empty()) {
        drawTextAt("No other apps found", 13, Weight::Regular,
                   alpha(theme::kTabletStatusSub, a), panel.x + 20, panel.y + 40);
    }
    if (room <= 0) {
        // Nothing can be added until something comes out, so say so rather than
        // letting every tick quietly do nothing.
        drawTextAt("Dock is full", 13, Weight::Regular, alpha(theme::kTabletStatusSub, a),
                   panel.x + 20, panel.y + 40);
    }

    for (size_t i = 0; i < tabletDockPickerRects.size(); ++i) {
        const TabletEntry& e = tabletDockPickerApps[i];
        const Rect box = tabletDockPickerRects[i];
        const float ir = float(box.w) * metrics::kTabletIconRadius;
        if (int(i) == tabletDockPickerHover)
            comp.drawRect(box.inflated(5), ir + 4.f, theme::kTabletIconHover, a * 0.9f);
        if (!drawAppIcon(box, e.icon, e.wmClass, ir, a))
            drawAppTile(box, e.name, ir, tabletTint(e.name), false);

        // The tick. A filled dot when chosen and a ring when not, rather than a
        // glyph, so it needs no font to render.
        const int dot = std::max(14, box.w / 4);
        const Rect mark{box.right() - dot / 2, box.bottom() - dot / 2, dot, dot};
        const bool on = i < tabletDockPickerSel.size() && tabletDockPickerSel[i];
        if (on) {
            comp.drawRect(mark, float(dot) * 0.5f, theme::kTabletAccent, a);
            comp.drawRect(Rect{mark.x + 1, mark.y + 1, dot - 2, dot - 2}, float(dot) * 0.5f,
                          theme::kTabletBadgeGlyph, a);
        } else {
            comp.drawRect(mark, float(dot) * 0.5f, theme::kTabletFolderSheet, a);
            comp.drawRect(mark.inflated(-2), float(dot) * 0.5f, theme::kTabletFolderHover, a);
        }

        // The name under the tile, elided the same way the grid labels are.
        const int labelPx = std::max(10, box.w / 6);
        const std::string label = tabletFit(text, e.name, labelPx, box.w + 18);
        const TextTex t = text.get(label, labelPx, Weight::Regular);
        if (t.tex) {
            const Rect lr{box.x + box.w / 2 - t.w / 2, box.bottom() + 5, t.w, t.h};
            comp.drawText(t, Rect{lr.x + 1, lr.y + 1, t.w, t.h},
                          alpha(theme::kTabletLabelShadow, a), 1.0f);
            comp.drawText(t, lr, alpha(theme::kTabletLabel, a), 1.0f);
        }
    }

    // Page dots, and the highlight on the page being shown.
    for (size_t i = 0; i < tabletDockPickerDots.size(); ++i) {
        const bool on = int(i) == tabletDockPickerPage;
        comp.drawRect(tabletDockPickerDots[i], float(tabletDockPickerDots[i].w) * 0.5f,
                      on ? theme::kTabletLabel : theme::kTabletFolderHover, a);
    }

    // Done, lit only when there is something to commit.
    bool any = false;
    for (char sel : tabletDockPickerSel)
        if (sel) any = true;
    const bool live = any && room > 0;
    const Rect done = tabletDockPickerDone;
    if (int(-2) == tabletDockPickerHover && live)
        comp.drawRect(done.inflated(2), float(done.h) * 0.5f, theme::kTabletIconHover, a);
    comp.drawRect(done, float(done.h) * 0.5f,
                  live ? theme::kTabletAccent : theme::kTabletBadgeTile,
                  live ? a : a * 0.8f);
    drawTextCentered("Done", 15, Weight::Medium,
                     alpha(live ? theme::kTabletBadgeGlyph : theme::kTabletStatusSub, a), done);
}

// ---------------------------------------------------------------------------
// Adding and removing widgets (rearrange mode's "+")
// ---------------------------------------------------------------------------
// Rearrange mode grows a "+" in the top-right corner. It opens a short sheet
// listing every widget kind the shell can draw, ticked for the ones already on
// the page being shown, so a card is added or taken off by tapping its row. This
// is the home screen's own way to put a card on a page -- the desktop uses the
// wallpaper context menu, which a phone has not got.

namespace {
const char* widgetKindName(WidgetKind k) {
    switch (k) {
        case WidgetKind::Clock: return "Clock";
        case WidgetKind::Battery: return "Battery";
        case WidgetKind::Calendar: return "Calendar";
        case WidgetKind::Weather: return "Weather";
        case WidgetKind::DigitalClock: return "Digital clock";
    }
    return "Widget";
}
const WidgetKind kWidgetKinds[] = {WidgetKind::Clock, WidgetKind::Battery,
                                   WidgetKind::Calendar, WidgetKind::Weather,
                                   WidgetKind::DigitalClock};
constexpr int kWidgetKindCount = int(sizeof(kWidgetKinds) / sizeof(kWidgetKinds[0]));
}  // namespace

void Manager::openTabletWidgetPicker() {
    if (tabletWidgetPicker) return;
    tabletWidgetPicker = true;
    tabletWidgetPickerHover = -1;
    tabletWidgetPickerPress = -1;
    tabletWidgetPickerDonePress = false;
    layoutTabletWidgetPicker();
    dirty = true;
}

void Manager::closeTabletWidgetPicker() {
    if (!tabletWidgetPicker) return;
    tabletWidgetPicker = false;
    tabletWidgetPickerRows.clear();
    tabletWidgetPickerHover = -1;
    tabletWidgetPickerPress = -1;
    tabletWidgetPickerDonePress = false;
    dirty = true;
}

void Manager::layoutTabletWidgetPicker() {
    tabletWidgetPickerRows.clear();
    if (!tabletWidgetPicker) return;
    const int titleH = 44;
    const int rowH = 52;
    const int panelW = std::min(screenW - 32, std::max(340, screenW * 74 / 100));
    const int panelH = titleH + kWidgetKindCount * rowH + 64;
    const int panelX = (screenW - panelW) / 2;
    const int panelY = std::max(metrics::kTabletStatusH + 12, (screenH - panelH) / 2);
    tabletWidgetPickerPanel = Rect{panelX, panelY, panelW, panelH};
    for (int i = 0; i < kWidgetKindCount; ++i)
        tabletWidgetPickerRows.push_back(
            Rect{panelX + 12, panelY + titleH + i * rowH, panelW - 24, rowH - 8});
    const int doneW = 96, doneH = 40;
    tabletWidgetPickerDone = Rect{panelX + panelW - 20 - doneW, panelY + panelH - 16 - doneH,
                                  doneW, doneH};
}

void Manager::toggleTabletWidget(WidgetKind kind) {
    // Ticked means it is on the page being shown; tapping takes that one card
    // off. Unticked adds a fresh card onto this page.
    const int page = tabletHomePage;
    for (size_t i = 0; i < widgets.size(); ++i) {
        if (widgets[i].kind == kind && widgets[i].page == page) {
            removeWidget(int(i));
            dirty = true;
            return;
        }
    }
    addWidget(kind);
    dirty = true;
}

bool Manager::handleTabletWidgetPickerPress(int x, int y, unsigned button) {
    if (!tabletWidgetPicker) return false;
    if (button != Button1) return true;
    if (!tabletWidgetPickerPanel.contains(x, y)) {
        closeTabletWidgetPicker();
        return true;
    }
    if (tabletWidgetPickerDone.contains(x, y)) {
        tabletWidgetPickerDonePress = true;
        return true;
    }
    for (size_t i = 0; i < tabletWidgetPickerRows.size(); ++i) {
        if (tabletWidgetPickerRows[i].inflated(2).contains(x, y)) {
            tabletWidgetPickerPress = int(i);
            return true;
        }
    }
    return true;
}

void Manager::handleTabletWidgetPickerRelease(int x, int y) {
    if (!tabletWidgetPicker) return;
    const int row = tabletWidgetPickerPress;
    const bool onDone = tabletWidgetPickerDonePress;
    tabletWidgetPickerPress = -1;
    tabletWidgetPickerDonePress = false;
    if (onDone && tabletWidgetPickerDone.contains(x, y)) {
        closeTabletWidgetPicker();
        return;
    }
    if (row >= 0 && row < kWidgetKindCount && row < int(tabletWidgetPickerRows.size()) &&
        tabletWidgetPickerRows[size_t(row)].inflated(2).contains(x, y)) {
        toggleTabletWidget(kWidgetKinds[row]);
        layoutTabletWidgetPicker();
    }
    dirty = true;
}

void Manager::updateTabletWidgetPickerHover(int x, int y) {
    if (!tabletWidgetPicker) return;
    const int was = tabletWidgetPickerHover;
    tabletWidgetPickerHover = -1;
    if (tabletWidgetPickerDone.contains(x, y)) tabletWidgetPickerHover = -2;
    for (size_t i = 0; i < tabletWidgetPickerRows.size(); ++i)
        if (tabletWidgetPickerRows[i].inflated(2).contains(x, y))
            tabletWidgetPickerHover = int(i);
    if (was != tabletWidgetPickerHover) dirty = true;
}

void Manager::drawTabletWidgetPickerView() {
    if (!tabletWidgetPicker) return;
    const float a = 1.0f;
    const Rect& panel = tabletWidgetPickerPanel;
    const float radius = float(std::min(panel.w, panel.h)) * 0.10f;
    comp.drawAcrylic(panel, radius, theme::kTabletFolderSheet, 0.96f,
                     theme::kTabletPanelBorder, a);
    drawTextAt("Widgets", 18, Weight::Medium, alpha(theme::kTabletLabel, a), panel.x + 20,
               panel.y + 16);

    for (int i = 0; i < kWidgetKindCount && i < int(tabletWidgetPickerRows.size()); ++i) {
        const Rect& row = tabletWidgetPickerRows[size_t(i)];
        const WidgetKind kind = kWidgetKinds[i];
        bool on = false;
        for (const Widget& w : widgets)
            if (w.kind == kind && w.page == tabletHomePage) on = true;
        if (i == tabletWidgetPickerHover)
            comp.drawRect(row.inflated(2), 12.f, theme::kTabletIconHover, a * 0.9f);
        drawTextAt(widgetKindName(kind), 15, Weight::Regular, alpha(theme::kTabletLabel, a),
                   row.x + 12, row.y + (row.h - 18) / 2);
        // The tick: a filled disc when the card is up, a ring when it is not,
        // matching the dock picker's marks so the two read as the same control.
        const int dot = 22;
        const Rect mark{row.right() - dot - 12, row.y + (row.h - dot) / 2, dot, dot};
        if (on) {
            comp.drawRect(mark, float(dot) * 0.5f, theme::kTabletAccent, a);
            comp.drawRect(mark.inflated(-3), float(dot) * 0.5f, theme::kTabletBadgeGlyph, a);
        } else {
            comp.drawRect(mark, float(dot) * 0.5f, theme::kTabletFolderSheet, a);
            comp.drawRect(mark.inflated(-2), float(dot) * 0.5f, theme::kTabletFolderHover, a);
        }
    }

    const Rect done = tabletWidgetPickerDone;
    if (tabletWidgetPickerHover == -2)
        comp.drawRect(done.inflated(2), float(done.h) * 0.5f, theme::kTabletIconHover, a);
    comp.drawRect(done, float(done.h) * 0.5f, theme::kTabletAccent, a);
    drawTextCentered("Done", 15, Weight::Medium, alpha(theme::kTabletBadgeGlyph, a), done);
}

// One page of the grid, slid sideways by dx. Icons carry their absolute index
// through the whole file, so a page is a window onto tabletHome rather than a
// list of its own.
void Manager::drawTabletGridPage(int page, int dx, float a, double wiggle) {
    if (page < 0 || page >= tabletHomePageCount) return;
    const int icon = tabletIconSize;
    const float radius = float(icon) * metrics::kTabletIconRadius;
    const int first = page * tabletHomePerPage;
    for (int k = 0; k < tabletHomePerPage; ++k) {
        const int index = first + k;
        if (index >= int(tabletHome.size())) break;
        const TabletItem& item = tabletHome[size_t(index)];
        const bool lifting = tabletDragIcon == index && !tabletDragFromDock &&
                             !tabletDragFromFolder;
        Rect cell = tabletGridCell(k);
        cell.x += dx;
        // The page on screen is drawn from the reflow springs so it glides; a page
        // sliding in or out (dx != 0) is drawn from its own static cells.
        if (dx == 0 && !lifting) {
            auto sit = tabletIconSpring.find(item.uid);
            if (sit != tabletIconSpring.end()) {
                const motion::Vec2 v = sit->second.value();
                cell = Rect{int(std::lround(v.x)), int(std::lround(v.y)), tabletGridCellW,
                            tabletGridCellH};
            }
        }
        if (lifting) cell = tabletDragRect;

        // Where a lifted icon would land, painted under the icons of that page.
        if (index == tabletDragTarget &&
            (tabletDragIcon >= 0 || tabletDragFromDock || tabletDragFromFolder))
            comp.drawRect(cell.inflated(-6), radius + 6.f, theme::kTabletIconHover, a * 0.9f);

        int jig = 0;
        if (tabletEdit && !lifting)
            jig = int(std::lround(std::sin(wiggle + double(index) * 0.7) * 3.0));
        const Rect box{cell.x + (cell.w - icon) / 2 + jig, cell.y, icon, icon};
        const double hv = size_t(index) < tabletIconHover.size() ? tabletIconHover[index] : 0.0;
        if (hv > 0.001 && !lifting)
            comp.drawRect(box.inflated(4), radius + 3.f, theme::kTabletIconHover, a * float(hv));
        if (lifting) comp.drawRect(box.inflated(6), radius + 5.f, theme::kTabletIconHover, a);

        const std::string label = item.isFolder ? item.folder.name : item.app.name;
        if (item.isFolder) {
            // The folder's own squircle, with up to nine of its apps as mini icons
            // inside it, exactly the way iOS draws one.
            comp.drawRect(box, radius, theme::kTabletFolderBack, a);
            const int pad = std::max(4, icon / 10);
            const int inner = icon - 2 * pad;
            const int gap = std::max(2, inner / 16);
            const int cellSz = (inner - 2 * gap) / 3;
            const float miniR = float(cellSz) * metrics::kTabletIconRadius;
            const int shown =
                std::min<int>(metrics::kTabletFolderPerPage, int(item.folder.apps.size()));
            for (int m = 0; m < shown; ++m) {
                const Rect miniBox{pad + box.x + (m % 3) * (cellSz + gap),
                                   pad + box.y + (m / 3) * (cellSz + gap), cellSz, cellSz};
                const TabletEntry& app = item.folder.apps[size_t(m)];
                if (!drawAppIcon(miniBox, app.icon, app.wmClass, miniR, a))
                    drawAppTile(miniBox, app.name, miniR, tabletTint(app.name), false);
            }
        } else if (!drawAppIcon(box, item.app.icon, item.app.wmClass, radius, a)) {
            drawAppTile(box, item.app.name, radius, tabletTint(item.app.name), false);
        }
        // The drop ring, for either kind of cell a folder can be made on.
        if (index == tabletDragOverFolder || index == tabletDragOverApp)
            comp.drawRect(box.inflated(3), radius + 2.f, theme::kTabletFolderHover, a);
        // Open right now. A folder stands for several apps, so it never claims one
        // of them is running; a lifted icon keeps its dot underneath itself.
        if (!item.isFolder && !lifting && tabletEntryRunning(item.app))
            drawTabletRunDot(box, jig, a);
        const TextTex t = text.get(tabletFit(text, label, 12, cell.w - 8), 12, Weight::Regular);
        if (!t.tex) continue;
        const int lx = cell.x + (cell.w - t.w) / 2 + jig;
        const int ly = box.bottom() + 6;
        comp.drawText(t, Rect{lx + 1, ly + 1, t.w, t.h}, alpha(theme::kTabletLabelShadow, a),
                      1.0f);
        comp.drawText(t, Rect{lx, ly, t.w, t.h}, alpha(theme::kTabletLabel, a), 1.0f);
    }
}

void Manager::drawTabletHome() {
    if (tabletAnim <= 0.001) return;
    const float a = float(clamp01(easeOutCubic(tabletAnim)));
    // The rect list holds only the cells that fit, so "not laid out yet" is the
    // test -- comparing sizes would re-lay out every frame.
    if ((tabletHomeRects.empty() && !tabletHome.empty()) ||
        (tabletDockRects.empty() && !tabletDock.empty()))
        layoutTabletHome();

    // The jiggle: a slow sine per icon, out of phase, so edit mode reads at a
    // glance without any rotation primitive.
    const double wiggle = nowMs() / 150.0;

    // The grid turns sideways like a page of icons: while one page is moving, the
    // page it is leaving and the one coming in are both drawn, sliding with it.
    {
        const double off = tabletPageOffset;
        const int stride = std::max(1, tabletGridStride);
        const int base = int(std::floor(off));
        const double frac = off - double(base);
        const int whole = int(std::lround(-frac * stride));
        if (tabletPageSwipe || std::abs(frac) > 0.001) {
            drawTabletGridPage(base, whole, a, wiggle);
            const int entering = frac > 0.0 ? base + 1 : base - 1;
            const int shift = int(std::lround((frac > 0.0 ? 1.0 - frac : 1.0 + frac) * stride));
            drawTabletGridPage(entering, whole + shift, a, wiggle);
        } else {
            drawTabletGridPage(base, 0, a, wiggle);
        }
    }

    // --- dock ---------------------------------------------------------------
    // The page dots, in the strip between the grid and the dock: which page this is
    // out of how many, and a tap on one turns straight to it.
    for (size_t i = 0; i < tabletPageDots.size(); ++i) {
        const bool on = int(i) == tabletHomePage;
        comp.drawRect(tabletPageDots[i], float(tabletPageDots[i].w) * 0.5f,
                      on ? theme::kTabletLabel : theme::kTabletFolderHover, a);
    }

    // The app drawer, when it is up: a full-screen sheet over the home screen but
    // under the dock, so the dock floats above it the way One UI's does.
    if (tabletDrawerAnim > 0.001)
        drawTabletDrawer(float(clamp01(tabletDrawerAnim)));

    // The dock stays up while rearranging, so an icon can be dragged out of the
    // grid into it or lifted back out again.
    if (!tabletDockRect.empty()) {
        const float dr = float(std::min(tabletDockRect.h, 44)) * 0.62f;
        comp.drawAcrylic(tabletDockRect, dr, theme::kTabletDockGlass, 0.55f,
                         tabletDragOverDock ? theme::kTabletDockHover
                                            : theme::kTabletDockBorder,
                         a);
        const int dockIcon = tabletDockIconSize;
        const float dockRadius = float(dockIcon) * metrics::kTabletIconRadius;
        for (size_t i = 0; i < tabletDockRects.size() && i < tabletDock.size(); ++i) {
            const bool dragging = int(i) == tabletDragDockIndex && tabletDragFromDock;
            Rect box = tabletDockRects[i];
            if (dragging) {
                box = tabletDragRect;
                box.w = box.h = dockIcon;
            } else {
                // The live, magnified box. The wobble rides the same factor so a
                // bounce reads as the icon itself springing rather than sliding.
                const double sc =
                    i < tabletDockScale.size() ? tabletDockScale[i] * tabletDockWobble[i] : 1.0;
                if (sc > 1.001) {
                    const int nw = int(std::lround(double(box.w) * sc));
                    const int nh = int(std::lround(double(box.h) * sc));
                    box = Rect{box.x + box.w / 2 - nw / 2, box.y + box.h / 2 - nh / 2, nw, nh};
                }
            }
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
            if (!dragging && tabletEntryRunning(tabletDock[i]))
                drawTabletRunDot(box, 0, a);
            // The remove badge, in the icon's top-right corner, only while the home
            // screen is in rearrange mode.
            if (tabletEdit && i < tabletDockRemoveRects.size())
                drawTabletDockBadge(tabletDockRemoveRects[i], "x", a, false);
        }
    }

    // The rearrange-mode "+" in the top-right corner: the home screen's own way
    // to put a widget on the page. A plus is drawn from two bars rather than set
    // in a font, so it needs nothing loaded to render.
    if (tabletEdit && !tabletWidgetAddRect.empty()) {
        const Rect& b = tabletWidgetAddRect;
        const float r = float(b.w) * 0.5f;
        comp.drawRect(b, r, theme::kTabletBadgeTile, a);
        comp.drawRect(b.inflated(-2), r, theme::kTabletPanelBorder, a * 0.5f);
        const int arm = std::max(4, b.w / 4);
        const int len = b.w / 2;
        comp.drawRect(Rect{b.x + (b.w - len) / 2, b.y + b.h / 2 - arm / 2, len, arm}, arm * 0.5f,
                      theme::kTabletLabel, a);
        comp.drawRect(Rect{b.x + b.w / 2 - arm / 2, b.y + (b.h - len) / 2, arm, len}, arm * 0.5f,
                      theme::kTabletLabel, a);
    }

    // One UI sets the home screen back while an app grows out of its icon (and
    // again while it collapses back), so the expanding window reads as rising off
    // the wallpaper rather than simply covering it. The scrim tracks the same
    // curve the window itself morphs on (see drawClientSprite), and it stays up
    // while the app is settled so the strips the app does not cover -- the status
    // bar and the home bar -- do not flash at the end of the open.
    {
        double dim = 0.0;
        for (const auto& cp : clients) {
            const Client* c = cp.get();
            if (!c->tabletApp || !c->tabletFromValid) continue;
            double t;
            if (c->appear < 1.0) t = easeOutCubic(c->appear);
            else if (c->closing && c->vanish > 0.0) t = 1.0 - c->vanish * c->vanish * c->vanish;
            else if (c->minFade > 0.0) t = 1.0 - c->minFade * c->minFade * c->minFade;
            else t = 1.0;
            dim = std::max(dim, t);
        }
        if (dim > 0.001)
            comp.drawRect(Rect{0, 0, screenW, screenH}, 0.f, theme::kTabletSplash,
                          float(0.30 * dim));
    }

    // An open folder, then the rename field over it: both belong to the home
    // screen rather than to the app layer, so they are painted from here.
    drawTabletFolderView();
    drawTabletRename();
}

// The clock/date and the home indicator, painted after the windows so they float
// over an open app exactly the way iOS' status bar and gesture bar do.

// An open folder: the screen dims, and the folder's icon zooms out into a rounded
// sheet holding its apps nine at a time. The zoom is anchored on the icon it came
// from, so it reads as that folder opening rather than a panel arriving.
void Manager::drawTabletFolderView() {
    // While one is fading out the index has already been cleared, so the last one
    // is used to keep drawing it until the animation finishes.
    const int index = tabletFolderOpen >= 0 ? tabletFolderOpen : tabletFolderLast;
    if (index < 0 || tabletFolderAnim <= 0.001) return;
    if (index >= int(tabletHome.size())) return;
    const TabletFolder& f = tabletHome[size_t(index)].folder;
    const float a = float(clamp01(easeOutCubic(tabletFolderAnim)));
    comp.drawRect(Rect{0, 0, screenW, screenH}, 0.f, theme::kTabletSplash, 0.55f * a);

    const Rect panel = tabletFolderPanel;
    if (panel.empty()) return;
    comp.drawAcrylic(panel, 34.f, theme::kTabletFolderSheet, 0.90f, theme::kShellBorder, a);

    // The name, doubling as the page label once a folder has more than one page.
    const std::string title =
        f.name.empty() ? std::string("Folder") : f.name;
    drawTextCentered(title, 19, Weight::Bold, alpha(theme::kText, a),
                     Rect{panel.x, panel.y + 20, panel.w, 30});

    const float miniR = float(metrics::kTabletIconRadius);
    for (size_t i = 0; i < tabletFolderRects.size() && i < f.apps.size(); ++i) {
        Rect box = tabletFolderRects[i];
        const bool dragging = tabletDragFromFolder && int(i) == tabletDragInside;
        if (dragging) box = tabletDragRect;
        const TabletEntry& app = f.apps[i];
        const float r = float(box.w) * miniR;
        if (dragging) comp.drawRect(box.inflated(5), r + 4.f, theme::kTabletIconHover, a);
        if (!drawAppIcon(box, app.icon, app.wmClass, r, a))
            drawAppTile(box, app.name, r, tabletTint(app.name), false);
    }

    // Page dots, when the folder holds more than the nine one page shows.
    if (tabletFolderPageCount > 1) {
        const int dot = 6, gap = 14;
        const int totalW = tabletFolderPageCount * gap - (gap - dot);
        int dx = panel.x + (panel.w - totalW) / 2;
        const int dy = panel.bottom() - 26;
        for (int p = 0; p < tabletFolderPageCount; ++p) {
            const bool on = p == tabletFolderPage;
            comp.drawRect(Rect{dx, dy, dot, dot}, dot * 0.5f,
                          on ? theme::kText : theme::kTextMuted, a * (on ? 0.95f : 0.5f));
            dx += gap;
        }
    }
}

// The rename field. Drawn over everything, with a blinking caret, because it is a
// modal moment: the user is typing a name and nothing else should compete.
void Manager::drawTabletRename() {
    if (tabletRenameItem < 0 || tabletRenameItem >= int(tabletHome.size())) return;
    if (!tabletHome[size_t(tabletRenameItem)].isFolder) return;
    const float a = float(clamp01(easeOutCubic(tabletAnim)));
    comp.drawRect(Rect{0, 0, screenW, screenH}, 0.f, theme::kTabletSplash, 0.62f * a);

    const int fieldW = std::min(screenW - 64, 420);
    const int fieldH = 48;
    const Rect field{(screenW - fieldW) / 2, screenH / 2 - fieldH / 2, fieldW, fieldH};
    comp.drawAcrylic(field, 12.f, theme::kCcTile, 0.94f, theme::kAccentRing, a);
    const TextTex t = text.get(tabletRenameText, 17, Weight::Medium);
    if (t.tex) {
        const int tx = field.x + 18;
        comp.drawText(t, Rect{tx, field.y + (fieldH - t.h) / 2, t.w, t.h},
                      alpha(theme::kText, a), 1.0f);
        // The caret blinks on the second, which is enough to read as a text field.
        if (std::fmod(nowMs(), 1000.0) < 500.0) {
            comp.drawRect(Rect{tx + t.w + 2, field.y + 10, 2, fieldH - 20}, 1.f,
                          alpha(theme::kText, a), 1.0f);
        }
    }
    drawTextCentered("Return to save, Escape to cancel", 12, Weight::Regular,
                     alpha(theme::kTextMuted, a), Rect{0, field.bottom() + 18, screenW, 20});
}

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
    // The bundled PNG Start logo wins over 2048.svg, exactly as on the taskbar;
    // the plain name keeps the SVG as a fallback when the PNG is missing.
    if (!drawAppIcon(box, assetDir + "/icons/2048.png", "2048", radius, a)) {
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
    settleGeometry(c);
    // The iOS 26 zoom transition anchors on the icon the app was launched from.
    // `animFrom == frame` keeps drawFrame still; drawClientSprite() morphs the
    // sprite between this rect and the frame on its own.
    // The One UI / Android 12 splash anchors on the entry the app came from: its
    // squircle is what is painted over the shape while it grows, so the icon the
    // user pressed is the icon that turns into the app.
    if (const TabletEntry* e = tabletEntryFor(c)) {
        c->tabletIcon = e->icon;
        c->tabletIconClass = e->wmClass;
    } else {
        c->tabletIcon.clear();
        c->tabletIconClass = c->appName;
    }
    c->tabletFrom = tabletIconRectFor(c);
    c->tabletFromValid = true;
    c->animFrom = c->frame;
    c->animStart = nowMs();
    c->animMs = metrics::kTabletOpenMs;
    syncClientGeometry(c);
    c->needsRepaint = true;
    dirty = true;
}

// Does this window belong to this launcher? The WM_CLASS instance against the
// entry's StartupWMClass, then the entry name against the window class or title.
bool Manager::tabletEntryMatchesClient(const TabletEntry& e, const Client* c) const {
    if (!c) return false;
    if (sameFold(e.wmClass, c->appName)) return true;
    if (sameFold(e.name, c->appName)) return true;
    if (sameFold(e.name, c->title)) return true;
    return false;
}

// Whether this home screen entry has a window open. A dock that cannot say this is
// just a row of pictures, and it is the one thing both the dock and the grid can
// answer cheaply for every icon on screen.
bool Manager::tabletEntryRunning(const TabletEntry& e) const {
    for (const auto& cp : clients) {
        const Client* c = cp.get();
        if (!c->managed || !c->alive || c->closing || c->isDock || c->isDesktop) continue;
        if (c->skipTaskbar) continue;
        if (tabletEntryMatchesClient(e, c)) return true;
    }
    return false;
}

// The dot, centred under an icon. It goes through the same helper the icons do so it
// travels with them when the page slides or the icon wiggles.
void Manager::drawTabletRunDot(const Rect& box, int jiggle, float a) {
    const int d = metrics::kTabletRunDotSize;
    const Rect dot{box.x + (box.w - d) / 2 + jiggle, box.bottom() + metrics::kTabletRunDotGap,
                   d, d};
    comp.drawRect(dot, float(d) * 0.5f, alpha(theme::kTabletRunDot, a), 1.0f);
}

// The launcher entry a window belongs to, by the same rule the zoom anchor uses:
// the grid (or an entry inside a folder on it), then the dock. Null when nothing
// matches, which is the case the splash answers with the window's letter tile.
const TabletEntry* Manager::tabletEntryFor(const Client* c) const {
    if (!c) return nullptr;
    const auto matches = [&](const TabletEntry& e) { return tabletEntryMatchesClient(e, c); };
    for (const TabletItem& item : tabletHome) {
        if (!item.isFolder) {
            if (matches(item.app)) return &item.app;
            continue;
        }
        for (const TabletEntry& app : item.folder.apps)
            if (matches(app)) return &app;
    }
    for (const TabletEntry& e : tabletDock)
        if (matches(e)) return &e;
    return nullptr;
}

// The home-screen (or dock) icon a window belongs to, matched by the rule above.
// With no match the zoom runs out of a squircle just above the home indicator, so
// every tablet window still gets the transition instead of popping in.
Rect Manager::tabletIconRectFor(const Client* c) const {
    const auto matches = [&](const TabletEntry& e) { return tabletEntryMatchesClient(e, c); };
    const int icon = std::max(metrics::kTabletIconMin, tabletIconSize);
    for (size_t i = 0; i < tabletHomeRects.size() && i < tabletHome.size(); ++i) {
        const TabletItem& item = tabletHome[i];
        const Rect& cell = tabletHomeRects[i];
        const Rect iconRect{cell.x + (cell.w - icon) / 2, cell.y, icon, icon};
        if (!item.isFolder) {
            if (matches(item.app)) return iconRect;
            continue;
        }
        // An app that has been put in a folder flies out of that folder's icon,
        // which is the last place the user saw it, exactly as iOS does it.
        for (const TabletEntry& app : item.folder.apps)
            if (matches(app)) return iconRect;
    }
    for (size_t i = 0; i < tabletDockRects.size() && i < tabletDock.size(); ++i) {
        if (matches(tabletDock[i])) return tabletDockRects[i];
    }
    return Rect{screenW / 2 - icon / 2,
                screenH - metrics::kTabletHomeBarZone - icon - 8, icon, icon};
}

// Back to an ordinary decorated window for the desktop.
void Manager::endTabletApp(Client* c) {
    if (!c || !c->tabletApp) return;
    c->tabletApp = false;
    c->tabletFromValid = false;
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
    settleGeometry(c);
    c->animMs = 0;
    syncClientGeometry(c);
    updateStateAtoms(c);
    dirty = true;
}

// Swipe-up equivalent: everything open collapses back to the grid.
void Manager::tabletGoHome() {
    closeTabletMenu();
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
        noteRecent(e.name, e.exec, e.icon, e.wmClass);
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
    // A rename is modal: it owns every press until it is committed or abandoned.
    if (tabletRenameItem >= 0) {
        closeTabletFolder();
        tabletEdit = true;
        tabletRenameItem = -1;
        tabletRenameText.clear();
        // Abandoning a rename lands in rearrange mode, where the dock shows its
        // badges and its "+", so its row is laid out again.
        layoutTabletHome();
        dirty = true;
        return true;
    }
    // The quick actions sheet is modal while it is up: a press on a row arms it, and
    // a press anywhere else just puts it away, the way a sheet is dismissed.
    if (tabletMenu) {
        if (button != Button1) return true;
        tabletMenuItem = -1;
        for (size_t i = 0; i < tabletMenuRows.size(); ++i)
            if (tabletMenuRows[i].inflated(2).contains(x, y)) tabletMenuItem = int(i);
        return true;
    }
    // The dock picker is modal for as long as it is up, so it takes the press even
    // where a press would otherwise land on the home screen behind it.
    if (tabletDockPickerOpen) {
        if (button != Button1) return true;
        if (!tabletDockPickerPanel.contains(x, y)) {
            // A tap outside dismisses it, and nothing is added.
            closeTabletDockPicker();
            return true;
        }
        if (tabletDockPickerDone.contains(x, y)) {
            tabletDockPickerDonePress = true;
            return true;
        }
        for (size_t i = 0; i < tabletDockPickerRects.size(); ++i) {
            if (tabletDockPickerRects[i].inflated(6).contains(x, y)) {
                tabletDockPickerPress = int(i);
                return true;
            }
        }
        for (size_t i = 0; i < tabletDockPickerDots.size(); ++i) {
            if (tabletDockPickerDots[i].inflated(8).contains(x, y)) {
                tabletDockPickerPage = int(i);
                layoutTabletDockPicker();
                return true;
            }
        }
        return true;
    }
    // The widget sheet is modal for as long as it is up, exactly like the dock's.
    if (tabletWidgetPicker) return handleTabletWidgetPickerPress(x, y, button);
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
    // The app drawer is modal like the switcher: its grid owns the pointer while it
    // is up, with the status bar still reaching Control Centre.
    if (tabletDrawer) {
        if (button != Button1) return true;
        if (tabletStatusRect.contains(x, y)) {
            toggleControlCenter();
            return true;
        }
        return handleTabletDrawerPress(x, y, button);
    }
    if (button != Button1) return true;  // swallow right/middle clicks

    // The page dots turn straight to a page. They are there whether or not the
    // screen is being rearranged, the way they are on a tablet's home screen.
    if (tabletHomePageCount > 1) {
        for (size_t i = 0; i < tabletPageDots.size(); ++i) {
            if (!tabletPageDots[i].inflated(10).contains(x, y)) continue;
            setTabletHomePage(int(i));
            return true;
        }
    }

    // While rearranging, the dock carries a small target of its own: a badge on
    // each icon, and holding a badge takes that app out of the dock. They are
    // tested before the icons so that holding a badge picks up nothing.
    if (tabletEdit) {
        for (size_t i = 0; i < tabletDockRemoveRects.size(); ++i) {
            if (!tabletDockRemoveRects[i].contains(x, y)) continue;
            tabletDockPressRemove = int(i);
            dirty = true;
            return true;
        }
        // The "+" opens the widget sheet, which is where a card is put on the
        // page or taken off it.
        if (!tabletWidgetAddRect.empty() && tabletWidgetAddRect.contains(x, y)) {
            openTabletWidgetPicker();
            return true;
        }
    }

    // An open folder is modal too: taps land in the sheet, and a tap outside it
    // closes the folder rather than reaching the home screen behind.
    if (tabletFolderOpen >= 0) {
        if (tabletFolderPanel.contains(x, y)) {
            for (size_t i = 0; i < tabletFolderRects.size(); ++i) {
                if (!tabletFolderRects[i].inflated(6).contains(x, y)) continue;
                // Remember the press so a dwell can lift it out of the folder.
                tabletPressAt = nowMs();
                tabletPressPos = Point{x, y};
                tabletPressItem = -1;
                tabletPressDock = -1;
                tabletFolderPressCell = int(i);
                return true;
            }
            // The dots page the folder rather than launching anything.
            if (tabletFolderPageCount > 1) {
                const int dot = 6, gap = 14;
                const int totalW = tabletFolderPageCount * gap - (gap - dot);
                const int dx0 = tabletFolderPanel.x + (tabletFolderPanel.w - totalW) / 2;
                const Rect dots{dx0 - 6, tabletFolderPanel.bottom() - 32,
                                totalW + 12, 18};
                if (dots.contains(x, y)) {
                    const int page = std::clamp((x - dx0) / gap, 0, tabletFolderPageCount - 1);
                    if (page != tabletFolderPage) {
                        tabletFolderPage = page;
                        layoutTabletFolder();
                        dirty = true;
                    }
                    return true;
                }
            }
            tabletPressAt = 0;  // a sheet tap is not a tap on the home screen
            return true;
        }
        tabletPressAt = 0;
        closeTabletFolder();
        return true;
    }

    // A widget card is grabbable like the desktop's, but only where no icon or
    // dock slot is over it: the cards sit behind the icons on the home screen, so
    // an overlap belongs to the icon.
    if (!tabletEdit) {
        bool overLaunch = false;
        for (size_t i = 0; i < tabletDockRects.size() && i < tabletDock.size(); ++i)
            if (tabletDockRects[i].contains(x, y)) overLaunch = true;
        for (const Rect& r : tabletHomeRects)
            if (r.contains(x, y)) overLaunch = true;
        if (!overLaunch && handleWidgetPress(x, y, time)) return true;
    }

    // Where the press landed, kept so tickAnimations() can turn it into a long
    // press: hold an icon and it lifts, hold the background and the screen starts
    // jiggling. This is the gesture that makes rearranging discoverable.
    tabletPressAt = nowMs();
    tabletPressPos = Point{x, y};
    tabletLongPressFired = false;
    tabletPressItem = -1;
    tabletPressDock = -1;
    tabletFolderPressCell = -1;
    // A press on bare wallpaper is where a sideways page turn begins; one on an
    // icon is a drag or a launch, and the dot strip turns pages by being tapped.
    tabletSwipeStartX = x;
    for (size_t i = 0; i < tabletDockRects.size() && i < tabletDock.size(); ++i) {
        if (tabletDockRects[i].contains(x, y)) {
            tabletPressDock = int(i);
            break;
        }
    }
    if (tabletPressDock < 0) {
        for (size_t i = 0; i < tabletHomeRects.size(); ++i) {
            if (!tabletHomeRects[i].contains(x, y)) continue;
            tabletPressItem = tabletHomeFirst + int(i);
            break;
        }
    }

    // Already rearranging: a press is the start of a drag, or nothing at all.
    if (tabletEdit) {
        handleTabletIconPress(x, y);
        return true;
    }

    // The status bar is the Control Centre affordance: there is no taskbar clock
    // to click while the home screen is up.
    if (tabletStatusRect.contains(x, y)) {
        tabletPressAt = 0;
        toggleControlCenter();
        return true;
    }
    // The bottom strip is the iPhone X home bar: a tap goes home, but the real
    // gesture is a swipe, so hand it to the gesture tracker.
    if (y >= screenH - metrics::kTabletHomeBarZone) {
        tabletPressAt = 0;
        return handleTabletGesturePress(x, y);
    }
    // A tap on an icon is left open: nothing happens until the button comes back
    // up, so a press held still can still turn into a drag or a long press. That
    // is the whole reason rearranging works without a button to go looking for.
    return true;
}

void Manager::handleTabletRelease(int x, int y, unsigned button, Time time) {
    (void)time;
    // The drawer is modal and its own grid resolves the release: a tap launches,
    // a swipe pages, a swipe down puts it away.
    if (tabletDrawer) {
        handleTabletDrawerRelease(x, y, button);
        return;
    }
    // The sheet acts on release, and only on the row the press started on, so
    // sliding off a row before letting go does nothing.
    if (tabletMenu) {
        if (button != Button1) return;
        const int row = tabletMenuItem;
        tabletMenuItem = -1;
        if (row >= 0 && row < int(tabletMenuRows.size()) &&
            tabletMenuRows[size_t(row)].contains(x, y))
            runTabletMenuAction(row);
        dirty = true;
        return;
    }
    // What the press was on, kept before the state is cleared.
    const int item = tabletPressItem;
    const int dock = tabletPressDock;
    const int cell = tabletFolderPressCell;
    // Whether this really was a press that landed on the home screen: the status
    // bar and the home indicator act on the way down and arm nothing.
    const bool armed = tabletPressAt != 0;
    const bool dwelled = tabletLongPressFired;
    const bool dragging =
        tabletDragIcon >= 0 || tabletDragFromDock || tabletDragFromFolder;

    tabletPressAt = 0;
    tabletLongPressFired = false;
    tabletPressItem = -1;
    tabletPressDock = -1;
    tabletFolderPressCell = -1;

    // A page turn, rather than a tap: the grid follows the finger and then settles
    // on whichever page it was let go nearest to.
    if (tabletPageSwipe) {
        tabletPageSwipe = false;
        const int dx = x - tabletSwipeStartX;
        const int stride = std::max(1, tabletGridStride);
        int page = tabletPageSwipeFrom;
        if (dx <= -stride / 3) page = tabletPageSwipeFrom + 1;
        else if (dx >= stride / 3) page = tabletPageSwipeFrom - 1;
        setTabletHomePage(page);
        return;
    }

    // A swipe up on the wallpaper opens the One UI app drawer. It is the same
    // armed press as everything else -- what sets it apart is that it travelled
    // upwards and never landed on an icon, a folder or the dock.
    if (armed && !dwelled && !dragging && !tabletEdit && item < 0 && dock < 0 && cell < 0) {
        const int upDx = x - tabletPressPos.x;
        const int upDy = tabletPressPos.y - y;  // upwards is positive
        if (upDy > 70 && upDy > std::abs(upDx) * 2) {
            openTabletDrawer();
            return;
        }
    }

    // A long press that never travelled was not a drag. The icon it lifted goes back
    // where it came from and the quick actions open for it, which is how the sheet is
    // reached without taking the drag away from anyone who means it: the two are told
    // apart by whether the finger moved, not by a second gesture.
    if (dwelled && dragging && armed && !tabletFolderOpen &&
        std::abs(x - tabletPressPos.x) <= 10 && std::abs(y - tabletPressPos.y) <= 10) {
        const int fromGrid = tabletDragIcon;
        const int fromDock = tabletDragFromDock ? tabletDragDockIndex : -1;
        const bool gridApp = fromGrid >= 0 && fromGrid < int(tabletHome.size()) &&
                             !tabletHome[size_t(fromGrid)].isFolder;
        if (gridApp || (fromDock >= 0 && fromDock < int(tabletDock.size()))) {
            cancelTabletIconDrag();
            openTabletMenu(fromDock, gridApp ? fromGrid : -1);
        } else {
            endTabletIconDrag();  // a folder, or something not worth a menu
        }
        return;
    }

    // The dock's own targets, resolved before anything else so a stray flag can
    // never outlive the press that set it.
    if (tabletDockPickerOpen) {
        const int tile = tabletDockPickerPress;
        const bool onDone = tabletDockPickerDonePress;
        tabletDockPickerPress = -1;
        tabletDockPickerDonePress = false;
        if (button == Button1 && !dwelled && !dragging) {
            if (onDone && tabletDockPickerDone.contains(x, y)) {
                addTabletDockApps();
            } else if (tile >= 0 && tile < int(tabletDockPickerRects.size()) &&
                       tabletDockPickerRects[size_t(tile)].inflated(6).contains(x, y)) {
                // A tap ticks or unticks; it never launches, the picker is a
                // chooser and not a launcher.
                tabletDockPickerSel[size_t(tile)] = tabletDockPickerSel[size_t(tile)] ? 0 : 1;
                dirty = true;
            }
        }
        return;
    }
    if (tabletWidgetPicker) {
        handleTabletWidgetPickerRelease(x, y);
        return;
    }
    if (tabletDockPressRemove >= 0) {
        const int index = tabletDockPressRemove;
        tabletDockPressRemove = -1;
        if (button == Button1 && !dwelled && !dragging && index < int(tabletDockRemoveRects.size()) &&
            tabletDockRemoveRects[size_t(index)].contains(x, y))
            removeTabletDockApp(index);
        return;
    }

    // A lift or a dwell has already done the work; letting go must not also launch.
    if (button != Button1 || dwelled || dragging) return;

    if (cell >= 0) {
        if (tabletFolderOpen < 0 || tabletFolderOpen >= int(tabletHome.size())) return;
        const TabletFolder& f = tabletHome[size_t(tabletFolderOpen)].folder;
        const int index = tabletFolderPage * metrics::kTabletFolderPerPage + cell;
        if (index >= 0 && index < int(f.apps.size())) openTabletEntry(f.apps[size_t(index)]);
        return;
    }
    if (dock >= 0) {
        if (dock < int(tabletDock.size())) {
            openTabletEntry(tabletDock[size_t(dock)]);
            bounceTabletDockIcon(dock);  // a launch pops the icon out a little
        }
        return;
    }
    if (item >= 0) {
        if (item >= int(tabletHome.size())) return;
        const TabletItem& slot = tabletHome[size_t(item)];
        // A folder is the one cell that is not an app itself: it opens instead.
        if (slot.isFolder) openTabletFolder(item);
        else openTabletEntry(slot.app);
        return;
    }
    // A tap on bare wallpaper ends the jiggle, which with Escape is the only way
    // out; a drag that ended out here went through the
    // drop path instead and never gets here.
    if (tabletEdit && armed) {
        tabletEdit = false;
        tabletHover = -1;
        layoutTabletHome();
        dirty = true;
    }
    (void)x;
    (void)y;
}

// Turns a press that has been held still into a long press. Called from the frame
// tick, since it is a timer rather than an event.
void Manager::updateTabletLongPress() {
    if (tabletRenameItem >= 0 || tabletSwitcher || ccOpen || overlayOpen()) return;
    if (tabletLongPressFired || tabletDragIcon >= 0 || tabletGesture) return;
    if (tabletPressAt == 0) return;
    if (nowMs() - tabletPressAt < metrics::kTabletLongPressMs) return;
    // Only a press that has not travelled counts: a drag is already a drag.
    // A finger that has travelled is dragging, not holding. The threshold sits
    // above the direct-drag slop so a press that starts to move always lifts the
    // icon (onMotion) before the hold is called off here.
    if (std::abs(tabletPressPos.x - pointerX) > 14 ||
        std::abs(tabletPressPos.y - pointerY) > 14) {
        tabletPressAt = 0;
        return;
    }
    // A finger that has started turning a page is not holding anything.
    if (tabletPageSwipe) {
        tabletPressAt = 0;
        return;
    }
    tabletLongPressFired = true;
    tabletEdit = true;
    tabletHover = -1;
    // The dock grows a "+" tile and a badge on every icon while rearranging, so
    // its row is laid out again before anything is lifted out of it.
    layoutTabletHome();
    if (tabletFolderOpen >= 0) {
        // Inside a folder: holding one of its apps lifts it, ready to be dragged
        // out onto the home screen.
        if (tabletFolderPressCell >= 0) {
            beginTabletFolderDrag(tabletFolderPressCell, tabletPressPos.x,
                                  tabletPressPos.y);
            dirty = true;
            return;
        }
        // Holding the sheet itself opens it for renaming.
        if (tabletFolderOpen < int(tabletHome.size()) &&
            tabletFolderPanel.contains(tabletPressPos.x, tabletPressPos.y)) {
            beginTabletRename(tabletFolderOpen);
            return;
        }
    }
    if (tabletPressDock >= 0) {
        beginTabletDockDrag(tabletPressDock, tabletPressPos.x, tabletPressPos.y);
        dirty = true;
        return;
    }
    if (tabletPressItem >= 0) {
        // Holding a folder names it, which is where iOS puts renaming. Holding a
        // plain app lifts it instead. A folder is still moved by dragging it in
        // edit mode, so nothing is lost by not lifting it here.
        const TabletItem& item = tabletHome[size_t(tabletPressItem)];
        if (item.isFolder) {
            beginTabletRename(tabletPressItem);
            return;
        }
        beginTabletIconDrag(tabletPressItem, tabletPressPos.x, tabletPressPos.y);
        dirty = true;
        return;
    }
    // The background: nothing to lift, the jiggle is the whole point of the hold.
    dirty = true;
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
    if (!tabletGestureSwipe && dy > 230 && !tabletSwitcher && !tabletDrawer) openTabletSwitcher();
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
    // The drawer is the home screen's own sheet: a swipe up on the home bar puts
    // it away rather than going home, so the two gestures do not fight.
    if (tabletDrawer) {
        closeTabletDrawer();
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

// ---------------------------------------------------------------------------
// The One UI app drawer
// ---------------------------------------------------------------------------
// A full-screen "Apps" screen swiped up from the wallpaper: every tablet app --
// grid, folder members and dock alike -- gathered into one alphabetical grid,
// with the One UI search pill above it and the page dots below. It is modal, so
// its grid owns the pointer while it is up, and it rises from the bottom edge.

// Case-insensitive, so the drawer sorts the way a reader expects regardless of
// how the .desktop files captialised their names.
static bool tabletNameLess(const std::string& a, const std::string& b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const unsigned char ca = static_cast<unsigned char>(std::tolower(
            static_cast<unsigned char>(a[i])));
        const unsigned char cb = static_cast<unsigned char>(std::tolower(
            static_cast<unsigned char>(b[i])));
        if (ca != cb) return ca < cb;
    }
    return a.size() < b.size();
}

// A drawer entry matches when every whitespace-separated word of the query is
// found somewhere in the launcher's metadata. That makes "web fire" find Firefox
// even though the words are not adjacent, while an empty query matches everything.
static bool tabletDrawerEntryMatches(const TabletEntry& e, const std::string& query) {
    if (query.empty()) return true;
    const std::string hay = e.name + " " + e.wmClass + " " + e.exec + " " + e.path;
    size_t i = 0;
    while (i < query.size()) {
        while (i < query.size() && std::isspace(static_cast<unsigned char>(query[i]))) ++i;
        const size_t start = i;
        while (i < query.size() && !std::isspace(static_cast<unsigned char>(query[i]))) ++i;
        if (i == start) continue;
        if (!containsFold(hay, query.substr(start, i - start))) return false;
    }
    return true;
}

void Manager::buildTabletDrawerApps() {
    if (!tabletEntriesBuilt) buildTabletEntries();
    tabletDrawerApps.clear();
    tabletDrawerApps.reserve(tabletHome.size() + tabletDock.size());
    for (const TabletEntry& e : tabletDock) tabletDrawerApps.push_back(e);
    for (const TabletItem& item : tabletHome) {
        if (item.isFolder)
            for (const TabletEntry& e : item.folder.apps) tabletDrawerApps.push_back(e);
        else
            tabletDrawerApps.push_back(item.app);
    }
    // One UI lists the drawer alphabetically, and so does everyone who reads it.
    std::stable_sort(tabletDrawerApps.begin(), tabletDrawerApps.end(),
                     [](const TabletEntry& a, const TabletEntry& b) {
                         return tabletNameLess(a.name, b.name);
                     });
    updateTabletDrawerMatches();
    tabletDrawerPage = 0;
}

void Manager::updateTabletDrawerMatches() {
    tabletDrawerMatches.clear();
    tabletDrawerMatches.reserve(tabletDrawerApps.size());
    for (size_t i = 0; i < tabletDrawerApps.size(); ++i)
        if (tabletDrawerEntryMatches(tabletDrawerApps[i], tabletDrawerSearchText))
            tabletDrawerMatches.push_back(int(i));
}

void Manager::setTabletDrawerSearch(const std::string& query) {
    if (query == tabletDrawerSearchText) return;
    tabletDrawerSearchText = query;
    tabletDrawerSearchFocused = true;
    tabletDrawerHover = -1;
    updateTabletDrawerMatches();
    tabletDrawerPage = 0;
    tabletDrawerOffset = 0.0;
    layoutTabletDrawer();
    dirty = true;
}

void Manager::clearTabletDrawerSearch() {
    if (tabletDrawerSearchText.empty()) return;
    tabletDrawerSearchText.clear();
    tabletDrawerSearchFocused = true;
    tabletDrawerHover = -1;
    updateTabletDrawerMatches();
    tabletDrawerPage = 0;
    tabletDrawerOffset = 0.0;
    layoutTabletDrawer();
    dirty = true;
}

Rect Manager::tabletDrawerCell(int k) const {
    const int col = k % tabletDrawerCols;
    const int row = k / tabletDrawerCols;
    return Rect{tabletDrawerGridX + col * tabletDrawerCellW,
                tabletDrawerGridTop + row * tabletDrawerCellH,
                tabletDrawerCellW, tabletDrawerCellH};
}

void Manager::layoutTabletDrawer() {
    if (tabletDrawerApps.empty()) buildTabletDrawerApps();
    tabletDrawerRects.clear();
    tabletDrawerDots.clear();

    // The search pill spans the width the way the reference does, and the grid is
    // inset under it; both sit inside the status bar and the dock.
    const int marginX = std::max(20, screenW / 16);
    const int searchW = std::min(screenW - 2 * marginX, 760);
    const int searchY = tabletStatusRect.bottom() + std::max(14, screenH / 22);
    tabletDrawerSearchRect = Rect{(screenW - searchW) / 2, searchY, searchW,
                                  metrics::kTabletSearchH};
    // The clear target is a generous circle inside the pill rather than the whole
    // trailing corner, so it is easy to hit on a touchscreen without stealing
    // presses meant for the rest of the field.
    const int clearD = std::max(28, tabletDrawerSearchRect.h - 14);
    tabletDrawerClearRect =
        Rect{tabletDrawerSearchRect.right() - clearD - 8,
             tabletDrawerSearchRect.y + (tabletDrawerSearchRect.h - clearD) / 2, clearD, clearD};

    // A denser grid than the home screen's: smaller icons, more columns.
    const int icon = std::clamp(std::min(screenW, screenH) / 14, metrics::kTabletIconMin, 72);
    tabletDrawerIconSize = icon;
    const int cellW = icon + icon / 2 + 16;
    const int cellH = icon + 30;
    const int cols = std::max(3, (screenW - 2 * marginX) / cellW);
    tabletDrawerCols = cols;
    tabletDrawerCellW = cellW;
    tabletDrawerCellH = cellH;
    tabletDrawerGridX = (screenW - cols * cellW) / 2;
    tabletDrawerGridTop = tabletDrawerSearchRect.bottom() + std::max(18, screenH / 26);
    const int gridBottom = tabletDockRect.empty() ? screenH - 44 : tabletDockRect.y - 14;

    auto rowsFor = [&](int reserved) {
        return std::max(1, (gridBottom - reserved - tabletDrawerGridTop) / cellH);
    };
    const int visible = int(tabletDrawerMatches.size());
    int rows = rowsFor(0);
    const int dotsH = visible > cols * rows ? metrics::kTabletPageDotsH : 0;
    if (dotsH) rows = rowsFor(dotsH);

    tabletDrawerPerPage = cols * rows;
    tabletDrawerPageCount =
        std::max(1, (visible + tabletDrawerPerPage - 1) / tabletDrawerPerPage);
    tabletDrawerPage = std::clamp(tabletDrawerPage, 0, tabletDrawerPageCount - 1);
    tabletDrawerFirst = tabletDrawerPage * tabletDrawerPerPage;
    tabletDrawerStride = cols * cellW + cellW / 2;

    for (int k = 0; k < tabletDrawerPerPage; ++k) {
        if (tabletDrawerFirst + k >= visible) break;
        tabletDrawerRects.push_back(tabletDrawerCell(k));
    }

    if (tabletDrawerPageCount > 1) {
        const int d = 8, gap = 14;
        const int totalW = tabletDrawerPageCount * gap - (gap - d);
        const int dx0 = (screenW - totalW) / 2;
        const int dy = gridBottom - dotsH + (dotsH - d) / 2;
        for (int i = 0; i < tabletDrawerPageCount; ++i)
            tabletDrawerDots.push_back(Rect{dx0 + i * gap, dy, d, d});
    }
}

int Manager::tabletDrawerHitTest(int x, int y) const {
    for (size_t i = 0; i < tabletDrawerRects.size(); ++i)
        if (tabletDrawerRects[i].contains(x, y)) return tabletDrawerFirst + int(i);
    return -1;
}

void Manager::setTabletDrawerPage(int page) {
    const int clamped = std::clamp(page, 0, tabletDrawerPageCount - 1);
    if (clamped == tabletDrawerPage) return;
    tabletDrawerPage = clamped;
    layoutTabletDrawer();
    dirty = true;
}

void Manager::openTabletDrawer() {
    if (tabletDrawer) return;
    // Every open starts on a clean query: a stale search narrowing the grid the
    // next time the drawer comes up would look like missing apps.
    tabletDrawerSearchText.clear();
    tabletDrawerSearchFocused = true;
    buildTabletDrawerApps();
    tabletDrawerPage = 0;
    tabletDrawerOffset = 0.0;
    tabletDrawerSwipe = false;
    tabletDrawerArmed = false;
    tabletDrawerPressIndex = -1;
    tabletDrawerHover = -1;
    tabletDrawerSearchPress = false;
    tabletDrawerClearPress = false;
    tabletDrawer = true;
    layoutTabletDrawer();
    tabletHover = -1;
    dirty = true;
}

void Manager::closeTabletDrawer() {
    if (!tabletDrawer) return;
    tabletDrawer = false;
    tabletDrawerSwipe = false;
    tabletDrawerArmed = false;
    tabletDrawerPressIndex = -1;
    tabletDrawerHover = -1;
    tabletDrawerSearchPress = false;
    tabletDrawerClearPress = false;
    tabletDrawerSearchFocused = false;
    tabletDrawerSearchText.clear();
    tabletDrawerOffset = 0.0;
    dirty = true;
}

bool Manager::handleTabletDrawerPress(int x, int y, unsigned button) {
    if (button != Button1) return true;
    tabletDrawerDownX = x;
    tabletDrawerDownY = y;
    tabletDrawerArmed = true;
    tabletDrawerPressIndex = -1;
    tabletDrawerSearchPress = false;
    tabletDrawerClearPress = false;
    // A press in the search pill focuses it; the trailing clear disc is tested
    // first so tapping it never reads as a press on the field itself.
    if (!tabletDrawerSearchText.empty() &&
        tabletDrawerClearRect.inflated(6).contains(x, y)) {
        tabletDrawerClearPress = true;
        tabletDrawerSearchFocused = true;
    } else if (tabletDrawerSearchRect.contains(x, y)) {
        tabletDrawerSearchPress = true;
        tabletDrawerSearchFocused = true;
    } else {
        tabletDrawerPressIndex = tabletDrawerHitTest(x, y);
        tabletDrawerSearchFocused = false;
    }
    tabletDrawerHover = tabletDrawerHitTest(x, y);
    dirty = true;
    return true;
}

void Manager::updateTabletDrawerMotion(int x, int y) {
    const int hover = tabletDrawerHitTest(x, y);
    if (hover != tabletDrawerHover) {
        tabletDrawerHover = hover;
        dirty = true;
    }
    if (!tabletDrawerArmed) return;
    // The pill handles its own press: dragging off it is not a page turn.
    if (tabletDrawerSearchPress || tabletDrawerClearPress) return;
    const int dx = x - tabletDrawerDownX;
    const int dy = y - tabletDrawerDownY;
    // A sideways drag off a bare part of the sheet turns the page, the same slop
    // the home screen uses so brushing the sheet does not flick the grid.
    if (tabletDrawerPressIndex < 0 &&
        std::abs(dx) > metrics::kTabletSwipeSlop && std::abs(dx) > std::abs(dy)) {
        if (!tabletDrawerSwipe) {
            tabletDrawerSwipe = true;
            tabletDrawerSwipeFrom = tabletDrawerPage;
        }
    }
    if (tabletDrawerSwipe) {
        const double stride = double(std::max(1, tabletDrawerStride));
        tabletDrawerOffset = std::clamp(double(tabletDrawerSwipeFrom) - double(dx) / stride,
                                        -0.35, double(tabletDrawerPageCount - 1) + 0.35);
        dirty = true;
    }
}

void Manager::handleTabletDrawerRelease(int x, int y, unsigned button) {
    if (button != Button1) {
        tabletDrawerArmed = false;
        tabletDrawerPressIndex = -1;
        tabletDrawerSearchPress = false;
        tabletDrawerClearPress = false;
        return;
    }
    const int dx = x - tabletDrawerDownX;
    const int dy = y - tabletDrawerDownY;
    const int index = tabletDrawerPressIndex;
    const bool clearPressed = tabletDrawerClearPress;
    const bool searchPressed = tabletDrawerSearchPress;
    tabletDrawerArmed = false;
    tabletDrawerPressIndex = -1;
    tabletDrawerSearchPress = false;
    tabletDrawerClearPress = false;

    if (clearPressed) {
        if (tabletDrawerClearRect.inflated(6).contains(x, y) && !tabletDrawerSearchText.empty())
            clearTabletDrawerSearch();
        dirty = true;
        return;
    }
    if (searchPressed) {
        // A tap on the pill just leaves the field focused and the caret blinking.
        if (std::abs(dx) < 12 && std::abs(dy) < 12) tabletDrawerSearchFocused = true;
        dirty = true;
        return;
    }
    if (tabletDrawerSwipe) {
        tabletDrawerSwipe = false;
        const int stride = std::max(1, tabletDrawerStride);
        int page = tabletDrawerSwipeFrom;
        if (dx <= -stride / 3) page = tabletDrawerSwipeFrom + 1;
        else if (dx >= stride / 3) page = tabletDrawerSwipeFrom - 1;
        setTabletDrawerPage(page);
        return;
    }
    // A swipe down puts the drawer away, the way it came up.
    if (dy > 90 && std::abs(dy) > std::abs(dx)) {
        closeTabletDrawer();
        return;
    }
    // A tap opens an app and lets the drawer fall away behind it. The slot is an
    // index into the filtered list, so it is mapped back through the matches.
    if (index >= 0 && index < int(tabletDrawerMatches.size()) &&
        std::abs(dx) < 12 && std::abs(dy) < 12) {
        const TabletEntry e = tabletDrawerApps[size_t(tabletDrawerMatches[size_t(index)])];
        closeTabletDrawer();
        openTabletEntry(e);
    }
    dirty = true;
}

// The drawer's search glyph: a magnifier, drawn from primitives so it needs no
// icon theme. It mirrors the reference pill's leading icon.
static void drawTabletSearchGlyph(Compositor& comp, int cx, int cy, float s, const Color& ink,
                                  float a) {
    const float r = s * 0.30f;
    const float thick = std::max(2.f, s * 0.10f);
    // A ring with the gap at the lower-right, where the handle meets it.
    comp.drawArc(cx, cy, r, thick, 2.55f, 8.28f, ink, a);
    comp.drawRectRotated(int(std::lround(cx + r * 0.86f)), int(std::lround(cy + r * 0.86f)),
                         int(std::max(2.f, thick)), int(std::lround(r * 1.15f)), -0.785f,
                         thick * 0.5f, ink, a);
}

// The clear affordance inside the search pill: two strokes crossed at 45 degrees,
// drawn rather than typed so it stays crisp at any pill height.
static void drawTabletClearGlyph(Compositor& comp, int cx, int cy, float s, const Color& ink,
                                 float a) {
    const float thick = std::max(2.f, s * 0.11f);
    const float len = s * 0.72f;
    comp.drawRectRotated(cx, cy, int(std::lround(thick)), int(std::lround(len)), 0.785f,
                         thick * 0.5f, ink, a);
    comp.drawRectRotated(cx, cy, int(std::lround(thick)), int(std::lround(len)), -0.785f,
                         thick * 0.5f, ink, a);
}

void Manager::drawTabletDrawer(float a) {
    if (a <= 0.001f) return;
    // The content rises up from the bottom edge as it opens: what the sheet does
    // at 0 is off the bottom, at 1 it is where the grid is drawn.
    const int rise = int(std::lround((1.0 - double(a)) * double(screenH) * 0.18));

    // A dim, frosted backdrop. One UI sits the drawer over a blurred, darkened
    // wallpaper, so the home icons behind it read as tone rather than as shapes.
    // A heavier wash at the top makes the white search pill pop and fades the
    // wallpaper's busiest area out behind the grid.
    comp.drawAcrylic(Rect{0, 0, screenW, screenH}, 0.f, theme::kTabletSplash, 0.78f,
                     theme::kTabletSplash, a);
    comp.drawGradient(Rect{0, 0, screenW, std::max(1, screenH * 2 / 5)}, 0.f,
                      theme::kTabletDrawerScrimTop,
                      Color{theme::kTabletDrawerScrimTop.r, theme::kTabletDrawerScrimTop.g,
                            theme::kTabletDrawerScrimTop.b, 0.0f},
                      a);
    comp.drawGradient(Rect{0, screenH * 3 / 5, screenW, screenH - screenH * 3 / 5}, 0.f,
                      Color{theme::kTabletDrawerScrimBottom.r, theme::kTabletDrawerScrimBottom.g,
                            theme::kTabletDrawerScrimBottom.b, 0.0f},
                      theme::kTabletDrawerScrimBottom, a);

    // --- search pill --------------------------------------------------------
    {
        Rect sr = tabletDrawerSearchRect;
        sr.y += rise;
        const float radius = float(sr.h) * 0.5f;
        // Soft elevation: two offset washes under the pill read as a shadow at a
        // distance, which is all the drawer needs to lift the field off the grid.
        comp.drawRect(Rect{sr.x - 2, sr.y + 3, sr.w + 4, sr.h + 3}, radius + 2.f,
                      theme::kTabletSearchShadow, 0.6f * a);
        comp.drawRect(Rect{sr.x - 1, sr.y + 1, sr.w + 2, sr.h + 2}, radius + 1.f,
                      theme::kTabletSearchShadow, 0.9f * a);
        // A live query gets a thin accent ring, so the field says it is filtering
        // without a second search screen.
        if (!tabletDrawerSearchText.empty())
            comp.drawRect(sr.inflated(3), radius + 3.f, theme::kTabletAccent, 0.90f * a);
        comp.drawRect(sr.inflated(2), radius + 2.f, theme::kTabletSearchBorder, a);
        comp.drawGradient(sr, radius, theme::kTabletSearch, theme::kTabletSearchBottom, a);

        const float glyph = float(sr.h);
        const int gx = sr.x + sr.h / 2 + 2;
        const int gy = sr.y + sr.h / 2;
        drawTabletSearchGlyph(comp, gx, gy, glyph, theme::kTabletSearchIcon, a);

        const int textPx = std::max(14, int(sr.h * 0.36f));
        const int textX = sr.x + sr.h + 8;
        const bool clearShown = !tabletDrawerSearchText.empty();
        const int trailing = clearShown ? sr.h + 12 : 116;
        const int textMax = std::max(20, sr.right() - trailing - textX);
        const std::string shown =
            tabletDrawerSearchText.empty() ? std::string("Search apps") : tabletDrawerSearchText;
        const TextTex t = text.get(tabletFit(text, shown, textPx,
                                             tabletDrawerSearchText.empty() ? textMax
                                                                            : textMax - 6),
                                   textPx, tabletDrawerSearchText.empty() ? Weight::Regular
                                                                          : Weight::Medium);
        if (t.tex) {
            const Rect tr{textX, gy - t.h / 2, t.w, t.h};
            comp.drawText(t, tr,
                          alpha(tabletDrawerSearchText.empty() ? theme::kTabletSearchHint
                                                               : theme::kTabletSearchInk,
                                a),
                          a);
            // A steady caret at the end of the typed text, with a slow blink so
            // the field looks alive without ever distracting from the results.
            if (clearShown && tabletDrawerSearchFocused &&
                (int64_t(nowMs() / 520) & 1) == 0) {
                const int cx = std::min(tr.x + t.w + 2, textX + textMax - 2);
                comp.drawRect(Rect{cx, gy - textPx / 2, std::max(2, textPx / 12), textPx},
                              float(std::max(2, textPx / 12)) * 0.5f, theme::kTabletSearchInk,
                              0.72f * a);
            }
        }

        const int tR = sr.y + sr.h / 2;
        if (clearShown) {
            const Rect cb = tabletDrawerClearRect;
            const Rect cr{cb.x, cb.y + rise, cb.w, cb.h};
            comp.drawRect(cr, float(cr.w) * 0.5f, theme::kTabletSearchClear, a);
            drawTabletClearGlyph(comp, cr.x + cr.w / 2, cr.y + cr.h / 2, float(cr.w) * 0.34f,
                                 theme::kTabletSearchIcon, a);
        } else {
            // The trailing pair from the reference: a microphone and a lens.
            const int micX = sr.right() - 74;
            const float micW = glyph * 0.10f, micH = glyph * 0.23f;
            comp.drawRect(Rect{int(micX - micW / 2), int(tR - micH / 2), int(micW), int(micH)},
                          micW * 0.5f, theme::kTabletSearchIcon, 0.9f * a);
            comp.drawArc(micX, int(tR + micH * 0.10f), micW * 0.92f, std::max(2.f, micW * 0.22f),
                         0.25f, 2.89f, theme::kTabletSearchIcon, a);
            const int lensX = sr.right() - 20;
            const float lr = glyph * 0.14f;
            comp.drawArc(lensX, tR, lr, std::max(2.f, glyph * 0.05f), 0.f,
                         2.f * kTabletPi - 0.001f, theme::kTabletSearchIcon, a);
            comp.drawRect(Rect{int(lensX), int(tR), int(lr * 0.22f), int(lr * 0.22f)},
                          lr * 0.11f, theme::kTabletSearchIcon, a);
        }
    }

    // --- the grid, slid sideways by the page offset -------------------------
    const int icon = tabletDrawerIconSize;
    const float radius = float(icon) * metrics::kTabletIconRadius;
    const double off = tabletDrawerOffset;
    const int stride = std::max(1, tabletDrawerStride);
    const int base = int(std::floor(off));
    const double frac = off - double(base);
    const int whole = int(std::lround(-frac * stride));
    const int visible = int(tabletDrawerMatches.size());
    auto page = [&](int p, int dx) {
        if (p < 0 || p >= tabletDrawerPageCount) return;
        const int first = p * tabletDrawerPerPage;
        for (int k = 0; k < tabletDrawerPerPage; ++k) {
            const int index = first + k;
            if (index >= visible) break;
            Rect cell = tabletDrawerCell(k);
            cell.x += dx;
            cell.y += rise;
            const Rect box{cell.x + (cell.w - icon) / 2, cell.y, icon, icon};
            const TabletEntry& e = tabletDrawerApps[size_t(tabletDrawerMatches[size_t(index)])];
            if (index == tabletDrawerHover)
                comp.drawRect(box.inflated(8), radius + 7.f, theme::kTabletIconHover,
                              0.70f * a);
            if (!drawAppIcon(box, e.icon, e.wmClass, radius, a))
                drawAppTile(box, e.name, radius, tabletTint(e.name), false);
            if (tabletEntryRunning(e)) drawTabletRunDot(box, 0, a);
            const TextTex t =
                text.get(tabletFit(text, e.name, metrics::kTabletAppLabelPx, cell.w - 6),
                         metrics::kTabletAppLabelPx, Weight::Regular);
            if (!t.tex) continue;
            const Rect tr{cell.x + (cell.w - t.w) / 2, box.bottom() + 6, t.w, t.h};
            comp.drawText(t, Rect{tr.x + 1, tr.y + 1, t.w, t.h},
                          alpha(theme::kTabletLabelShadow, a), 1.0f);
            comp.drawText(t, tr, alpha(theme::kTabletLabel, a), a);
        }
    };
    if (tabletDrawerSwipe || std::abs(frac) > 0.001) {
        page(base, whole);
        const int entering = frac > 0.0 ? base + 1 : base - 1;
        const int shift = int(std::lround((frac > 0.0 ? 1.0 - frac : 1.0 + frac) * stride));
        page(entering, whole + shift);
    } else {
        page(base, 0);
    }

    // --- empty state --------------------------------------------------------
    if (tabletDrawerMatches.empty()) {
        const int cx = screenW / 2;
        const int cy = std::min(screenH - rise - 120, tabletDrawerGridTop + rise + 92);
        drawTabletSearchGlyph(comp, cx, cy, float(std::max(72, screenW / 12)),
                              alpha(theme::kTabletStatusSub, 0.55f), a);
        drawTextCentered("No apps found", 20, Weight::Medium,
                         alpha(theme::kTabletLabel, a),
                         Rect{0, cy + 54, screenW, 30});
        drawTextCentered("Try a different name", 14, Weight::Regular,
                         alpha(theme::kTabletStatusSub, a),
                         Rect{0, cy + 84, screenW, 24});
    }

    // --- page dots ----------------------------------------------------------
    for (size_t i = 0; i < tabletDrawerDots.size(); ++i) {
        Rect d = tabletDrawerDots[i];
        d.y += rise;
        const bool on = int(i) == tabletDrawerPage;
        if (on) {
            const Rect pill{d.x - 5, d.y, d.w + 10, d.h};
            comp.drawRect(pill, float(d.h) * 0.5f, alpha(theme::kTabletLabel, a), a);
        } else {
            d = d.inflated(-1);
            comp.drawRect(d, float(d.w) * 0.5f, theme::kTabletFolderHover, a);
        }
    }
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
    for (size_t i = 0; i < tabletDockRects.size() && i < tabletDock.size(); ++i) {
        if (!tabletDockRects[i].contains(x, y)) continue;
        beginTabletDockDrag(int(i), x, y);
        return true;
    }
    for (size_t i = 0; i < tabletHomeRects.size() && i < tabletHome.size(); ++i) {
        if (!tabletHomeRects[i].contains(x, y)) continue;
        beginTabletIconDrag(tabletHomeFirst + int(i), x, y);
        return true;
    }
    return false;
}

void Manager::beginTabletIconDrag(int index, int x, int y) {
    if (index < 0 || index >= int(tabletHome.size())) return;
    // The index is absolute across the whole tabletHome list; the rect it grabs is
    // its cell on the page currently shown.
    const int local = index - tabletHomeFirst;
    if (local < 0 || local >= int(tabletHomeRects.size())) return;
    tabletDragIcon = index;
    tabletDragFromDock = false;
    tabletDragFromFolder = false;
    tabletDragTarget = index;
    tabletDragDockIndex = -1;
    tabletDragOverFolder = -1;
    tabletDragOverDock = false;
    tabletDragInside = -1;
    tabletDragUid = tabletHome[size_t(index)].uid;
    tabletDragRect = tabletHomeRects[size_t(local)];
    tabletDragGrab = Point{x - tabletDragRect.x, y - tabletDragRect.y};
    grabPointer();
    dirty = true;
}

void Manager::beginTabletDockDrag(int index, int x, int y) {
    if (index < 0 || index >= int(tabletDock.size())) return;
    tabletDragIcon = -1;          // a dock icon has no grid cell of its own
    tabletDragDockIndex = index;  // but the release needs to know which slot it was
    tabletDragFromDock = true;
    tabletDragFromFolder = false;
    tabletDragTarget = -1;
    tabletDragOverFolder = -1;
    tabletDragOverDock = false;
    tabletDragInside = -1;
    tabletDragUid = 0;
    tabletDragRect = index < int(tabletDockRects.size()) ? tabletDockRects[size_t(index)]
                                                         : Rect{x - 16, y - 16, 32, 32};
    tabletDragGrab = Point{x - tabletDragRect.x, y - tabletDragRect.y};
    grabPointer();
    dirty = true;
}

void Manager::beginTabletFolderDrag(int cell, int x, int y) {
    if (tabletFolderOpen < 0 || tabletFolderOpen >= int(tabletHome.size())) return;
    const TabletFolder& f = tabletHome[size_t(tabletFolderOpen)].folder;
    const int index = tabletFolderPage * metrics::kTabletFolderPerPage + cell;
    if (cell < 0 || cell >= int(tabletFolderRects.size()) || index >= int(f.apps.size())) return;
    tabletDragIcon = -1;
    tabletDragDockIndex = -1;
    tabletDragFromDock = false;
    tabletDragFromFolder = true;
    tabletDragTarget = -1;
    tabletDragOverFolder = -1;
    tabletDragOverDock = false;
    tabletDragInside = cell;
    tabletDragUid = 0;
    tabletDragRect = tabletFolderRects[size_t(cell)];
    tabletDragGrab = Point{x - tabletDragRect.x, y - tabletDragRect.y};
    grabPointer();
    dirty = true;
}

void Manager::beginTabletRename(int index) {
    if (index < 0 || index >= int(tabletHome.size())) return;
    if (!tabletHome[size_t(index)].isFolder) return;
    tabletRenameItem = index;
    tabletRenameText = tabletHome[size_t(index)].folder.name;
    if (tabletDragIcon >= 0 || tabletDragFromDock || tabletDragFromFolder) endTabletIconDrag();
    dirty = true;
}

void Manager::updateTabletIconDrag(int x, int y) {
    if (tabletDragIcon < 0 && !tabletDragFromDock && !tabletDragFromFolder) return;
    tabletDragRect.x = x - tabletDragGrab.x;
    tabletDragRect.y = y - tabletDragGrab.y;

    // Holding a lifted icon against either edge turns the page after a beat, which
    // is how an icon is carried to a part of the grid that is not on screen. The
    // beat has to be waited out again before it turns another page, or one sweep of
    // the finger would run through all of them.
    {
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

    // What the icon is over decides what letting go will mean, so all of them are
    // recomputed every move and the release just reads them.
    tabletDragTarget = -1;
    tabletDragOverFolder = -1;
    tabletDragOverApp = -1;
    tabletDragOverDock = false;
    tabletDragDockSlot = -1;
    tabletDragInside = -1;

    // Inside an open folder: one of its own cells, if the sheet is under the icon.
    if (tabletFolderOpen >= 0 && !tabletFolderPanel.empty() &&
        tabletFolderPanel.contains(x, y)) {
        for (size_t i = 0; i < tabletFolderRects.size(); ++i) {
            if (!tabletFolderRects[i].inflated(8).contains(x, y)) continue;
            tabletDragInside = int(i);
            break;
        }
    }

    if (tabletDragInside >= 0) {
        dirty = true;
        return;  // a cell inside the folder wins: nowhere else to go
    }

    // The dock, which is a drop target from anywhere on the home screen. The row
    // parts to make room for a carried icon, so a pointer can be sitting in the
    // open gap between two icons -- that is still a place to drop, just the slot
    // the gap makes. The icons are tested first with a finger-sized target; in
    // the gap (or past the last icon) the slot is where the pointer sits among
    // the icons, which is exactly the place the rubber band opens a gap for.
    if (!tabletDockRect.empty() && tabletDockRect.inflated(12).contains(x, y)) {
        int slot = -1;
        for (size_t i = 0; i < tabletDockRects.size(); ++i) {
            if (!tabletDockRects[i].inflated(8).contains(x, y)) continue;
            slot = int(i);
            break;
        }
        if (slot < 0) {
            for (size_t i = 0; i < tabletDockRects.size(); ++i) {
                if (x >= tabletDockRects[i].right()) continue;
                slot = int(i);
                break;
            }
            if (slot < 0) slot = int(tabletDockRects.size());
        }
        tabletDragOverDock = true;
        tabletDragDockSlot = slot;
        dirty = true;
        return;
    }

    // A cell in the grid. A folder being moved is never merged into anything, so
    // for a folder this is only ever a reorder target.
    const bool movingFolder =
        tabletDragIcon >= 0 && tabletDragIcon < int(tabletHome.size()) &&
        tabletHome[size_t(tabletDragIcon)].isFolder;
    for (size_t i = 0; i < tabletHomeRects.size(); ++i) {
        if (!tabletHomeRects[i].contains(x, y)) continue;
        const int index = tabletHomeFirst + int(i);
        if (index >= int(tabletHome.size())) break;
        if (movingFolder) {
            tabletDragTarget = index;
        } else if (tabletHome[size_t(index)].isFolder) {
            tabletDragOverFolder = index;  // the app joins this folder
        } else {
            tabletDragOverApp = index;     // two apps in a cell make a folder
        }
        break;
    }
    if (tabletDragOverFolder >= 0 || tabletDragOverApp >= 0) {
        dirty = true;
        return;
    }

    // Otherwise the nearest cell, which is where a plain reorder would land.
    int best = -1;
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
    tabletDragTarget = best;
    dirty = true;
}


// The long press turned out not to be a drag after all: put the icon back exactly
// where it came from and touch nothing else. Deliberately does not go through
// endTabletIconDrag(), whose job is to resolve a drop -- running that here would
// read the release position as a drop target and could, on a long press that never
// moved, resolve to an icon that is not the one being held.
void Manager::cancelTabletIconDrag() {
    tabletDragIcon = -1;
    tabletDragDockIndex = -1;
    tabletDragFromDock = false;
    tabletDragFromFolder = false;
    tabletDragTarget = -1;
    tabletDragOverFolder = -1;
    tabletDragOverApp = -1;
    tabletDragOverDock = false;
    tabletDragDockSlot = -1;
    tabletDragInside = -1;
    tabletDragUid = 0;
    ungrabPointer();
    layoutTabletHome();
    dirty = true;
}

void Manager::endTabletIconDrag() {
    const bool dragging = tabletDragIcon >= 0 || tabletDragFromDock || tabletDragFromFolder;
    if (!dragging) return;

    // What is being moved has to be settled before anything is taken out of its
    // list, so the release reads the layout the user was looking at as it was.
    // Every target is captured here, because the fields are cleared immediately
    // afterwards and the resolution below runs on the copies.
    const int fromGrid = tabletDragIcon;
    const int fromDock = tabletDragDockIndex;
    const int overFolder = tabletDragOverFolder;
    const int overApp = tabletDragOverApp;
    const bool overDock = tabletDragOverDock;
    const int inside = tabletDragInside;
    const int overCell = tabletDragTarget;
    const int dockSlot = tabletDragDockSlot;
    const bool fromFolder = tabletDragFromFolder;
    const int openFolder = tabletFolderOpen;
    // The carried icon's identity and where it was let go, so its spring can pick
    // up from the pointer and glide the last cell home.
    const int dragUid = tabletDragUid;
    const Rect dropRect = tabletDragRect;

    const bool stayPut =
        (overDock && fromDock >= 0) ||                       // a dock icon on the dock
        (overApp == fromGrid && fromGrid >= 0) ||            // an icon on itself
        (overFolder == fromGrid && fromGrid >= 0);
    if (stayPut) {
        tabletDragIcon = -1;
        tabletDragDockIndex = -1;
        tabletDragFromDock = false;
        tabletDragFromFolder = false;
        tabletDragTarget = -1;
        tabletDragOverFolder = -1;
        tabletDragOverApp = -1;
        tabletDragOverDock = false;
        tabletDragDockSlot = -1;
        tabletDragInside = -1;
        ungrabPointer();
        layoutTabletHome();
        dirty = true;
        return;
    }

    TabletEntry carried;
    bool have = false;
    if (fromFolder && openFolder >= 0 && openFolder < int(tabletHome.size()) && inside >= 0) {
        const TabletFolder& f = tabletHome[size_t(openFolder)].folder;
        const int index = tabletFolderPage * metrics::kTabletFolderPerPage + inside;
        if (index >= 0 && index < int(f.apps.size())) {
            carried = f.apps[size_t(index)];
            have = true;
        }
    } else if (fromGrid >= 0 && fromGrid < int(tabletHome.size()) &&
               !tabletHome[size_t(fromGrid)].isFolder) {
        carried = tabletHome[size_t(fromGrid)].app;
        have = true;
    } else if (fromDock >= 0 && fromDock < int(tabletDock.size())) {
        carried = tabletDock[size_t(fromDock)];
        have = true;
    }

    tabletDragIcon = -1;
    tabletDragDockIndex = -1;
    tabletDragFromDock = false;
    tabletDragFromFolder = false;
    tabletDragTarget = -1;
    tabletDragOverFolder = -1;
    tabletDragOverApp = -1;
    tabletDragOverDock = false;
    tabletDragDockSlot = -1;
    tabletDragInside = -1;
    ungrabPointer();

    if (!have) {
        // A folder icon was lifted. Folders are moved rather than carried, so the
        // only thing that can happen to one is a plain reorder of the grid.
        if (fromGrid >= 0 && overCell >= 0 && overCell != fromGrid &&
            fromGrid < int(tabletHome.size()) && overCell < int(tabletHome.size())) {
            TabletItem moved = tabletHome[size_t(fromGrid)];
            tabletHome.erase(tabletHome.begin() + fromGrid);
            tabletHome.insert(tabletHome.begin() + overCell, std::move(moved));
        }
        layoutTabletHome();
        dirty = true;
        return;
    }

    // Lift the app out of wherever it came from.
    //
    // Dragging an app out of a folder is the same removal as every other one, so it
    // gets the same rule: a folder left with less than two apps stops being a folder
    // and its remainder takes its place on the grid. It used to wait for the folder to
    // empty instead, which left a folder holding a single icon -- something a home
    // screen has no way to get rid of, since dragging that one app out dissolves it
    // and a folder of one is what you are left holding.
    //
    // A dissolved folder takes its cell with it, so every cell after it moves up by
    // one; that is what `unpacked` below corrects the drop position for.
    int unpacked = 0;
    int dockDrop = -1;  // the dock slot a drop landed on, for its bounce
    if (fromFolder && openFolder >= 0 && openFolder < int(tabletHome.size())) {
        const size_t before = tabletHome.size();
        removeTabletAppFromHome(tabletHome, carried);
        // The folder is still a folder only if two or more apps were left in it, and
        // then the sheet stays open on it. Anything else -- the remainder spliced in
        // its place, or nothing at all -- means the folder is gone and so is the
        // sheet, which would otherwise sit open over a folder that is not there.
        const bool stillFolder =
            openFolder < int(tabletHome.size()) && tabletHome[size_t(openFolder)].isFolder;
        if (!stillFolder) {
            closeTabletFolder();
            // Only a folder that left nothing behind takes its cell with it.
            if (tabletHome.size() < before) unpacked = 1;
        }
    } else if (fromDock >= 0 && fromDock < int(tabletDock.size())) {
        tabletDock.erase(tabletDock.begin() + fromDock);
    } else if (fromGrid >= 0 && fromGrid < int(tabletHome.size())) {
        tabletHome.erase(tabletHome.begin() + fromGrid);
    }

    // Lifting the app out of the grid shifts every later cell up by one, and so does
    // a folder that dissolved above, so the cell the icon was dropped on has to be
    // counted again before it is used.
    const auto shifted = [&](int idx) {
        int at = idx;
        if (fromDock < 0 && fromGrid >= 0 && fromGrid < at) --at;
        if (unpacked && openFolder < at) --at;
        return at;
    };

    // --- where does it go? iOS' order: into a folder, onto the dock, back into an
    // open folder, onto another app (which makes a new folder), else a plain slot.
    const int joinFolder = shifted(overFolder);
    const int joinApp = shifted(overApp);
    int popFolder = -1;
    if (joinFolder >= 0 && joinFolder < int(tabletHome.size())) {
        tabletHome[size_t(joinFolder)].folder.apps.push_back(carried);
        popFolder = joinFolder;
    } else if (joinApp >= 0 && joinApp < int(tabletHome.size())) {
        // Two apps in one cell: they become a folder, named for what is in it.
        TabletItem made;
        made.isFolder = true;
        made.folder.apps.push_back(tabletHome[size_t(joinApp)].app);
        made.folder.apps.push_back(carried);
        made.folder.name = suggestFolderName(made.folder.apps);
        tabletHome[size_t(joinApp)] = std::move(made);
        popFolder = joinApp;
    } else if (overDock) {
        // Into the slot it was dropped on, so the dock reorders rather than always
        // appending. The slot was counted before the app left the dock.
        int at = std::clamp(dockSlot >= 0 && fromDock < dockSlot ? dockSlot - 1 : dockSlot, 0,
                            int(tabletDock.size()));
        tabletDock.insert(tabletDock.begin() + at, carried);
        // What the row holds is what fits across the screen, so that is the cap a
        // drop is held to rather than a fixed number.
        if (int(tabletDock.size()) > tabletDockCapacity())
            tabletDock.erase(tabletDock.end() - tabletDockCapacity());
        dockDrop = at;
    } else if (inside >= 0 && openFolder >= 0 && openFolder < int(tabletHome.size())) {
        // Back into the folder it came from, which is a reorder inside it.
        TabletItem& slot = tabletHome[size_t(openFolder)];
        if (!slot.isFolder) {
            TabletItem made;
            made.isFolder = true;
            made.folder.apps.push_back(carried);
            made.folder.name = suggestFolderName(made.folder.apps);
            tabletHome[size_t(openFolder)] = std::move(made);
        } else {
            const int at = std::clamp(tabletFolderPage * metrics::kTabletFolderPerPage + inside,
                                      0, int(slot.folder.apps.size()));
            slot.folder.apps.insert(slot.folder.apps.begin() + at, carried);
        }
    } else if (overCell >= 0) {
        // A plain slot. Appending past the end is how the grid grows a new row.
        const int at = std::clamp(shifted(overCell), 0, int(tabletHome.size()));
        TabletItem item;
        item.app = carried;
        item.uid = dragUid;  // the same icon, so its reflow spring keeps following it
        tabletHome.insert(tabletHome.begin() + at, std::move(item));
    }

    tabletDragEdge = 0;
    layoutTabletHome();
    // The carried icon was painted under the pointer; its spring resumes from
    // there so the drop settles onto the cell instead of popping onto it.
    if (dragUid != 0) {
        motion::Spring2& sp = tabletIconSpring[dragUid];
        sp.configure(motion::kPlayful.frequency, motion::kPlayful.zeta);
        sp.reset(motion::Vec2{double(dropRect.x), double(dropRect.y)});
    }
    // A dock drop bounced the icon it landed on; the layout just re-based the row,
    // so the pop is applied to the fresh slot.
    if (dockDrop >= 0 && dockDrop < int(tabletDock.size())) bounceTabletDockIcon(dockDrop);
    // Opening the folder is what makes it a place to put apps in rather than just
    // an icon that appeared, and it is where iOS leaves you as well.
    if (popFolder >= 0) openTabletFolder(popFolder);
    // A lift that carried nothing changed nothing, so only a real drop is written
    // out. Everything else here -- reordering, folder creation, joining, emptying,
    // a dock move -- goes through this one place.
    if (!carried.name.empty()) saveTabletLayout();
    dirty = true;
}

// ---------------------------------------------------------------------------
// Dock physics: the magnification, the rubber band, the bounce
// ---------------------------------------------------------------------------
// The dock is a small spring system rather than a static row. Magnification is
// the shell's usual approach() so a sweep of the finger reads as icons swelling,
// but the horizontal parting runs on a damped spring -- icons give way with a
// tug and settle with a small overshoot, which is what makes a dock feel sprung
// instead of a tray of faithfully-spaced slots. A drop or a launch adds one pop:
// a decaying sinusoid riding the magnification of that single icon.

namespace {

// One semi-implicit Euler step of a damped spring. freq is the natural frequency
// in hertz, zeta the damping ratio: 1.0 is critically damped, a touch under is
// the "springy" settle a dock should have.
inline void dockSpring(double& v, double& vel, double target, double dtMs, double freq,
                       double zeta = 0.6) {
    const double dt = std::min(dtMs, 50.0) / 1000.0;
    const double w = 2.0 * 3.14159265358979323846 * std::max(freq, 0.5);
    const double acc = w * w * (target - v) - 2.0 * zeta * w * vel;
    vel += acc * dt;
    v += vel * dt;
}

}  // namespace

void Manager::bounceTabletDockIcon(int index) {
    if (index < 0 || index >= int(tabletDockWobbleAge.size())) return;
    tabletDockWobbleAge[size_t(index)] = 0.0;
    tabletDockWobble[size_t(index)] = 1.0 + 0.20;  // pop out, then wobble home
    dirty = true;
}

void Manager::updateTabletDockPhysics(double dtMs) {
    const size_t n = tabletDock.size();
    if (n == 0 || tabletDockRect.empty() || tabletDockBaseRects.size() != n ||
        tabletDockRects.size() != n || tabletDockScale.size() != n ||
        tabletDockDrift.size() != n || tabletDockDriftV.size() != n ||
        tabletDockWobble.size() != n || tabletDockWobbleAge.size() != n)
        return;
    bool moved = false;

    // --- magnification: swell under the pointer, sag back when it leaves -----
    // The hit area is the pill plus a finger's width of grace, so resting a finger
    // on the row swells it, and lifting it lets the icons deflate in place.
    const bool overDock =
        tabletDockRect.inflated(std::max(24, tabletDockIconSize)).contains(pointerX, pointerY);
    const double focus = double(pointerX);
    const double sigma = double(std::max(40, tabletDockIconSize)) * 1.05;
    for (size_t i = 0; i < n; ++i) {
        double want = 1.0;
        if (overDock) {
            const double dist = focus - double(tabletDockBaseRects[i].x + tabletDockBaseRects[i].w / 2);
            want += 0.45 * std::exp(-dist * dist / (2.0 * sigma * sigma));
        }
        if (approach(tabletDockScale[i], want, dtMs, 90.0)) moved = true;
    }

    // --- the bounce: a decaying sinusoid over the natural scale -------------
    // An icon that was dropped on or launched from the dock gets one pop. The age
    // runs its own clock so the wobble stays glued to the drop; the moment it is
    // back down to plain magnification the icon goes idle and stops asking to
    // repaint.
    for (size_t i = 0; i < n; ++i) {
        if (tabletDockWobbleAge[i] < 0.0) continue;
        tabletDockWobbleAge[i] += dtMs;
        const double age = tabletDockWobbleAge[i];
        const double wobble = 1.0 + 0.20 * std::exp(-age / 130.0) * std::cos(age * 0.021);
        if (age > 420.0 || std::abs(wobble - 1.0) < 0.004) {
            if (tabletDockWobble[i] != 1.0) moved = true;
            tabletDockWobble[i] = 1.0;
            tabletDockWobbleAge[i] = -1.0;
            continue;
        }
        if (std::abs(wobble - tabletDockWobble[i]) > 0.0005) moved = true;
        tabletDockWobble[i] = wobble;
    }

    // The rubber band: this frame's target picture is the row laid out against
    // itself at its magnified widths -- with a phantom slot dropped in where the
    // carried icon is headed -- and then recentred on the pill so the row never
    // walks off an edge. Each icon prowls from its base slot toward that picture
    // on the dock's own spring; the drop slot's icon count is stable while the
    // row parts, because the phantom claims that place in the row.
    const double gap = double(std::max(14, tabletDockIconSize / 2));
    const int phIndex =
        tabletDragOverDock ? (tabletDragDockSlot >= 0 ? tabletDragDockSlot : int(n)) : -1;
    const int items = int(n) + (phIndex >= 0 ? 1 : 0);

    std::vector<double> w(size_t(items), double(tabletDockBaseRects[0].w));
    std::vector<double> c(size_t(items), 0.0);
    for (size_t i = 0; i < n; ++i) w[i] = double(tabletDockBaseRects[i].w) * tabletDockScale[i];

    // The anchor: the phantom rides the finger, clamped so the whole row stays on
    // the pill; otherwise the icon under the finger keeps its own base slot and
    // the swell happens in place.
    int A = phIndex;
    double aGo = 0.0;
    if (phIndex >= 0) {
        aGo = double(pointerX);
        const double half = w[size_t(phIndex)] / 2;
        aGo = std::clamp(aGo, double(tabletDockRect.x) + half + 2.0,
                         double(tabletDockRect.right()) - half - 2.0);
    } else {
        long best = 0;
        long bestD = (long)1 << 62;
        for (size_t i = 0; i < n; ++i) {
            const long dx = long(tabletDockBaseRects[i].x + tabletDockBaseRects[i].w / 2) - long(pointerX);
            if (dx * dx < bestD) {
                bestD = dx * dx;
                best = long(i);
            }
        }
        A = int(best);
        aGo = double(tabletDockBaseRects[size_t(A)].x + tabletDockBaseRects[size_t(A)].w / 2);
    }
    c[size_t(A)] = aGo;
    for (int i = A - 1; i >= 0; --i)
        c[size_t(i)] = c[size_t(i + 1)] - (w[size_t(i)] / 2.0 + gap + w[size_t(i + 1)] / 2.0);
    for (int i = A + 1; i < items; ++i)
        c[size_t(i)] = c[size_t(i - 1)] + (w[size_t(i)] / 2.0 + gap + w[size_t(i - 1)] / 2.0);

    // Recentre the real icons on the pill's own centre so magnification never drags
    // the row off it. With the phantom the finger is the anchor, so the row centres
    // itself gently while the parting still does its job.
    double mean = 0.0;
    for (size_t i = 0; i < n; ++i) mean += c[i];
    mean /= double(n);
    double base = 0.0;
    for (size_t i = 0; i < n; ++i) base += double(tabletDockBaseRects[i].x + tabletDockBaseRects[i].w / 2);
    base /= double(n);
    if (phIndex < 0) {
        const double shift = base - mean;
        if (std::abs(shift) > 0.01) {
            for (size_t i = 0; i < n; ++i) c[i] += shift;
            moved = true;
        }
    } else if (std::abs(mean - base) > 4.0) {
        const double shift = (base - mean) * std::min(1.0, dtMs / 60.0);
        for (size_t i = 0; i < n; ++i) c[i] += shift;
        moved = true;
    }

    // Spring each icon toward its target deflection from its base slot. When the
    // row is parked and still, drift and velocity run to zero and painting stops.
    for (size_t i = 0; i < n; ++i) {
        const double target = c[i] - double(tabletDockBaseRects[i].x + tabletDockBaseRects[i].w / 2);
        dockSpring(tabletDockDrift[i], tabletDockDriftV[i], target, dtMs, 4.0);
        if (std::abs(tabletDockDrift[i] - target) > 0.03 ||
            std::abs(tabletDockDriftV[i]) > 0.05)
            moved = true;
    }

    // Rebuild the live rects, and the edit-mode remove badges that ride them, from
    // the drifted positions so hit testing and drawing agree with the visuals.
    for (size_t i = 0; i < n; ++i) {
        const Rect& b = tabletDockBaseRects[i];
        const double cx = double(b.x + b.w / 2) + tabletDockDrift[i];
        tabletDockRects[i] = Rect{int(std::lround(cx)) - b.w / 2, b.y, b.w, b.h};
    }
    if (tabletEdit) {
        const int badge = std::max(20, tabletDockIconSize / 3);
        tabletDockRemoveRects.clear();
        for (size_t i = 0; i < n; ++i) {
            const Rect& b = tabletDockRects[i];
            tabletDockRemoveRects.push_back(
                Rect{b.right() - badge / 2, b.y - badge / 2, badge, badge});
        }
    }

    if (moved) dirty = true;
}

}  // namespace wm
