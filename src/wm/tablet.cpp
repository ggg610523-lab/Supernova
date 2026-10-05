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
        // The cards are desktop furniture, so they come off the desktop with it.
        suspendWidgets();
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
    tabletPageSwipe = false;

    // The arrangement the user left behind wins over the one this session's
    // Desktop directory would suggest. Nothing is written here on purpose: until
    // the home screen is actually rearranged, a newly installed launcher still
    // shows up on the next run.
    if (loadTabletLayout()) {
        tabletEntriesBuilt = true;
        return;
    }

    // The home screen is fed from the session's Desktop directory, which is the
    // curated list of launchers the user actually has; the full .desktop scan is
    // the fallback when the Desktop is empty.
    std::vector<TabletEntry> found;
    for (const DesktopItem& d : desktopItems) {
        TabletEntry e;
        e.name = d.name;
        e.icon = d.icon;
        e.exec = d.exec;
        e.path = d.path;
        found.push_back(e);
    }
    if (found.empty()) {
        for (const AppEntry& a : apps) {
            if (found.size() >= 24) break;
            TabletEntry e;
            e.name = a.name;
            e.icon = a.icon;
            e.wmClass = a.wmClass;
            e.exec = a.exec;
            found.push_back(e);
        }
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
    tabletHomeRects.clear();
    tabletDockRects.clear();

    tabletStatusRect = Rect{0, 0, screenW, metrics::kTabletStatusH};
    tabletHomeBarRect = Rect{(screenW - metrics::kTabletHomeBarW) / 2,
                             screenH - 14, metrics::kTabletHomeBarW, metrics::kTabletHomeBarH};
    // --- dock: a glass pill of squircle app icons, above the home bar -------
    // The dock is edited where it stands, the way HarmonyOS does it: while the
    // home screen is in rearrange mode every icon grows a remove badge and the
    // row grows a "+" tile that opens the app picker. Both are part of the row's
    // content while rearranging, so they widen the pill instead of hanging off it.
    const int di = std::clamp(std::min(screenW, screenH) / 11, 48, metrics::kTabletDockIcon);
    tabletDockIconSize = di;
    tabletDockAddRect = Rect{};
    tabletDockRemoveRects.clear();
    const int extras = tabletEdit ? 1 : 0;
    const int slots = int(tabletDock.size()) + extras;
    if (slots > 0) {
        const int gap = std::max(14, di / 2);
        const int padX = std::max(16, di / 3);
        const int padY = std::max(12, di / 4);
        const int contentW = slots * di + (slots - 1) * gap;
        const int dockW = std::min(screenW - 32, contentW + 2 * padX);
        const int dockH = di + 2 * padY;
        const int dockX = (screenW - dockW) / 2;
        const int dockY = screenH - 30 - dockH;
        tabletDockRect = Rect{dockX, dockY, dockW, dockH};
        const int badge = std::max(20, di / 3);
        int x = dockX + (dockW - contentW) / 2;
        for (int i = 0; i < int(tabletDock.size()); ++i) {
            const Rect box{x, dockY + padY, di, di};
            tabletDockRects.push_back(box);
            // The badge rides the icon's top-right corner and hangs half outside
            // it, which is what makes it big enough to hit with a fingertip.
            if (tabletEdit)
                tabletDockRemoveRects.push_back(
                    Rect{box.right() - badge / 2, box.y - badge / 2, badge, badge});
            x += di + gap;
        }
        if (tabletEdit) tabletDockAddRect = Rect{x, dockY + padY, di, di};
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
    const int top = tabletStatusRect.bottom() + 24;
    const int gridBottom = tabletDockRect.empty() ? screenH - 46 : tabletDockRect.y - 18;

    // How many rows a page has is what decides how many pages there are, and the
    // dots need a strip of their own, which takes a row off the last page. So the
    // rows are worked out twice: once to find out whether there is more than one
    // page at all, and again with the dots' strip reserved if there is.
    auto rowsFor = [gridBottom, top, cellH](int reserved) {
        return std::max(1, (gridBottom - reserved - top) / cellH);
    };
    int rows = rowsFor(0);
    const int dotsH = int(tabletHome.size()) > cols * rows ? metrics::kTabletPageDotsH : 0;
    if (dotsH) rows = rowsFor(dotsH);

    tabletGridCols = cols;
    tabletGridCellW = cellW;
    tabletGridCellH = cellH;
    tabletGridTop = top;
    tabletGridX = (screenW - cols * cellW) / 2;
    tabletHomePerPage = cols * rows;
    tabletHomePageCount =
        std::max(1, (int(tabletHome.size()) + tabletHomePerPage - 1) / tabletHomePerPage);
    tabletHomePage = std::clamp(tabletHomePage, 0, tabletHomePageCount - 1);
    tabletHomeFirst = tabletHomePage * tabletHomePerPage;
    // A page slides by its own width plus a gap, which is what a fingertip
    // travelling across the screen is taken to mean.
    tabletGridStride = cols * cellW + cellW / 2;

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
            Rect box = tabletDockRects[i];
            const bool dragging = int(i) == tabletDragDockIndex && tabletDragFromDock;
            if (dragging) {
                box = tabletDragRect;
                box.w = box.h = dockIcon;
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
        // The row's "+" tile: how an app that is not on the grid gets into the dock.
        if (tabletEdit && !tabletDockAddRect.empty())
            drawTabletDockBadge(tabletDockAddRect, "+", a, true);
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

    // The page dots turn straight to a page. They are there whether or not the
    // screen is being rearranged, the way they are on a tablet's home screen.
    if (tabletHomePageCount > 1) {
        for (size_t i = 0; i < tabletPageDots.size(); ++i) {
            if (!tabletPageDots[i].inflated(10).contains(x, y)) continue;
            setTabletHomePage(int(i));
            return true;
        }
    }

    // While rearranging, the dock carries two small targets of its own: a badge on
    // every icon takes it out of the dock, and the row's "+" opens the picker.
    // They are tested before the icons so that holding a badge picks up nothing.
    if (tabletEdit) {
        for (size_t i = 0; i < tabletDockRemoveRects.size(); ++i) {
            if (!tabletDockRemoveRects[i].contains(x, y)) continue;
            tabletDockPressRemove = int(i);
            dirty = true;
            return true;
        }
        if (!tabletDockAddRect.empty() && tabletDockAddRect.contains(x, y)) {
            tabletDockPressAdd = true;
            dirty = true;
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
    if (tabletDockPressRemove >= 0) {
        const int index = tabletDockPressRemove;
        tabletDockPressRemove = -1;
        if (button == Button1 && !dwelled && !dragging && index < int(tabletDockRemoveRects.size()) &&
            tabletDockRemoveRects[size_t(index)].contains(x, y))
            removeTabletDockApp(index);
        return;
    }
    if (tabletDockPressAdd) {
        tabletDockPressAdd = false;
        if (button == Button1 && !dwelled && !dragging && tabletDockAddRect.contains(x, y))
            openTabletDockPicker();
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
        if (dock < int(tabletDock.size())) openTabletEntry(tabletDock[size_t(dock)]);
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
    if (std::abs(tabletPressPos.x - pointerX) > 6 ||
        std::abs(tabletPressPos.y - pointerY) > 6) {
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
    for (size_t i = 0; i < tabletDockRects.size() && i < tabletDock.size(); ++i) {
        if (!tabletDockRects[i].contains(x, y)) continue;
        beginTabletDockDrag(int(i), x, y);
        return true;
    }
    for (size_t i = 0; i < tabletHomeRects.size() && i < tabletHome.size(); ++i) {
        if (!tabletHomeRects[i].contains(x, y)) continue;
        beginTabletIconDrag(int(i), x, y);
        return true;
    }
    return false;
}

void Manager::beginTabletIconDrag(int index, int x, int y) {
    if (index < 0 || index >= int(tabletHomeRects.size())) return;
    if (index >= int(tabletHome.size())) return;
    tabletDragIcon = index;
    tabletDragFromDock = false;
    tabletDragFromFolder = false;
    tabletDragTarget = index;
    tabletDragDockIndex = -1;
    tabletDragOverFolder = -1;
    tabletDragOverDock = false;
    tabletDragInside = -1;
    tabletDragRect = tabletHomeRects[size_t(index)];
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

    // The dock, which is a drop target from anywhere on the home screen.
    for (size_t i = 0; i < tabletDockRects.size(); ++i) {
        if (!tabletDockRects[i].inflated(8).contains(x, y)) continue;
        tabletDragOverDock = true;
        tabletDragDockSlot = int(i);
        break;
    }
    if (tabletDragOverDock) {
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
        tabletHome.insert(tabletHome.begin() + at, std::move(item));
    }

    tabletDragEdge = 0;
    layoutTabletHome();
    // Opening the folder is what makes it a place to put apps in rather than just
    // an icon that appeared, and it is where iOS leaves you as well.
    if (popFolder >= 0) openTabletFolder(popFolder);
    // A lift that carried nothing changed nothing, so only a real drop is written
    // out. Everything else here -- reordering, folder creation, joining, emptying,
    // a dock move -- goes through this one place.
    if (!carried.name.empty()) saveTabletLayout();
    dirty = true;
}

}  // namespace wm
