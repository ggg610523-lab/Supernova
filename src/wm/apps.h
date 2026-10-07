// The Start menu's application list.
//
// Instead of hard coding a launcher table we read the freedesktop .desktop
// entries that are already installed, which is what makes the Start menu able
// to start (almost) any X11 program on the machine.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "theme.h"
#include "util.h"
#include "widgets.h"

namespace wm {

struct AppEntry {
    std::string name;       // "Files"
    std::string exec;       // "nautilus %U"  (field codes pre-stripped)
    std::string generic;    // GenericName + Comment, folded into the search key
    std::string icon;       // icon *name* from the entry (not resolved to a file)
    std::string wmClass;    // StartupWMClass, used to match running windows
    bool terminal = false;
    std::string searchKey;  // lower-case "name generic"
};

// Scans XDG_DATA_DIRS + ~/.local/share for applications/*.desktop.
std::vector<AppEntry> scanApps();

// The launchers the user pinned to the taskbar, in the order they were pinned.
// An entry is a snapshot of the AppEntry, so a pin keeps working (and keeps its
// icon) even when the .desktop file it came from is gone.
using PinnedList = std::vector<AppEntry>;

// $XDG_CONFIG_HOME/win11wm/<leaf>, else $HOME/.config/win11wm/<leaf>. Empty when
// neither variable names an absolute path, which is the only way these settings
// cannot be stored at all. Every saved preference goes through here.
std::string configPath(const char* leaf);

// Writes text to path via a sibling temp file and rename, creating the config
// directory when it is missing, so a crash halfway through leaves the previous
// file intact rather than a truncated one. False if it could not be written.
bool writeFileAtomic(const std::string& path, const std::string& text);

// Where the pins live: $XDG_CONFIG_HOME/win11wm/pinned, else
// $HOME/.config/win11wm/pinned. Empty when neither can be determined.
std::string pinnedPath();

// Reads the pin file back. Missing or unreadable file = no pins, never an error.
PinnedList loadPinned();

// Writes the pin list, one entry per line, replacing the file atomically.
void savePinned(const PinnedList& pins);

// Where the recently launched apps live, in the same config directory as the
// pins. Empty when neither XDG_CONFIG_HOME nor HOME names an absolute path.
std::string recentsPath();

// Reads the recent-app list back, newest first. Missing or unreadable = empty,
// never an error: a first run simply has nothing to show.
PinnedList loadRecents();

// Writes the recent-app list, newest first, replacing the file atomically.
void saveRecents(const PinnedList& recents);

// The thickness the user dragged the taskbar to, from the same config directory
// as the pins. A missing, unreadable or nonsense value falls back to the Windows
// 11 default rather than failing: a bad setting must never stop the WM starting.
int loadTaskbarHeight();

// Writes the taskbar's thickness, so the bar is still the size the user left it
// next time. Called once, when the drag that changed it ends.
void saveTaskbarHeight(int height);

// Which Fluent palette the shell should come up in, from the same config
// directory as the pins. Anything missing or unrecognised means dark, which is
// both Windows 11's default and this shell's.
theme::Mode loadThemeMode();

// Writes the palette choice, so the desktop is still light or still dark next
// time. Called once, when the Control Centre toggle is pressed.
void saveThemeMode(theme::Mode m);

// One thing the user's desktop should show: a folder, a plain file, or a
// .desktop launcher dropped there by an installer.
struct DesktopItem {
    std::string name;   // "Projects", "Firefox" -- what gets painted
    std::string path;   // absolute path, used to open it
    std::string icon;   // icon *name* (never a file path), best effort
    std::string exec;   // non-empty for a launcher: run this instead of path
    bool isDir = false;
    bool isDesktopEntry = false;
};

// The session's desktop directory: $XDG_DESKTOP_DIR from user-dirs.dirs, else
// $HOME/Desktop. Empty when neither can be determined.
std::string desktopDir();

// The desktop's contents, hidden files excluded, sorted by name. A .desktop
// file is parsed the same way the Start menu parses one, so its Name, Icon and
// Exec are what the desktop shows and runs.
std::vector<DesktopItem> scanDesktop();

// The tablet folder: a dedicated directory (the desktop's own "tablet" folder)
// whose .desktop files are the ONLY apps the tablet home screen recognises.
// Created on first use, so there is always somewhere to drop a launcher.
std::string tabletFolderDir();
bool ensureTabletFolder();

// The tablet folder's contents: only .desktop launchers, in name order. Anything
// that is not a launcher is ignored -- the tablet home screen is curated, not
// scraped.
std::vector<DesktopItem> scanTabletFolder();

// Whether a name may be used for a desktop entry. Empty is not a name, neither are
// "." and "..", and a name holding a path separator would put the entry somewhere
// else entirely. A leading dot is refused because scanDesktop() skips hidden files,
// so a hidden entry could be created and then never be seen or opened again.
bool usableDesktopName(const std::string& name);

// Creates a folder on the desktop called `base`, or the first free "base N" for
// N = 2, 3, ... so making a second one never fails behind the first. Writes the
// name it settled on to *made. False if the desktop directory is unusable or
// nothing could be created -- never fatal, the desktop simply stays as it was.
bool createDesktopFolder(const std::string& base, std::string* made);

// Renames one desktop entry to `name`, keeping it in the same directory. False if
// the name is unusable or already belongs to something else, which leaves the
// original untouched.
bool renameDesktopEntry(const std::string& path, const std::string& name);

// Deletes one desktop entry: a file outright, a folder together with everything
// inside it. This is the one call here that is not undoable, so the shell only
// reaches it after the user has confirmed.
bool deleteDesktopEntry(const std::string& path);

// fork + setsid + sh -c: fully detached, so a crashed app never takes the WM
// with it and the WM never has to reap anything it does not want to.
void launchApp(const std::string& exec);

// ASCII case-insensitive substring search (used by the Start menu search box).
bool containsFold(const std::string& haystack, const std::string& needle);

// One launchable thing on the tablet home screen: the same shape as a desktop
// item, kept as a snapshot so the arrangement survives even if the .desktop file
// behind it is later moved or removed.
struct TabletEntry {
    std::string name;
    std::string icon;
    std::string wmClass;
    std::string exec;  // non-empty: run this
    std::string path;  // otherwise hand the path to xdg-open
};

// A folder is a grid cell that holds other apps, the way iOS and Android do it:
// it sits in the grid beside plain icons and shows its first nine apps as a 3x3
// mini grid inside its own squircle.
struct TabletFolder {
    std::string name;  // suggested from the apps, editable by the user
    std::vector<TabletEntry> apps;
};

// One cell of the home grid: either one app or one folder, never both.
struct TabletItem {
    bool isFolder = false;
    TabletEntry app;
    TabletFolder folder;
};

// The home screen as the user left it.
struct TabletLayout {
    std::vector<TabletEntry> dock;
    std::vector<TabletItem> home;
};

// Whether two entries are the same launcher, however each was found. The desktop
// items and the .desktop scan describe the same apps with different metadata, so
// the exec -- or the path for a non-launcher -- decides, and the name has to agree.
bool sameTabletApp(const TabletEntry& a, const TabletEntry& b);

// Takes one app out of the home screen wherever it appears, as a plain grid icon
// or inside a folder, and dissolves any folder left with fewer than two apps in
// it. This is the rule that keeps a single screen from showing the same app twice,
// whether an app arrives there by being dragged, picked in the dock's app picker,
// or read back from a file somebody edited by hand.
void removeTabletAppFromHome(std::vector<TabletItem>& home, const TabletEntry& app);

// Reads the saved arrangement back from the config directory. An empty layout
// means there is nothing usable to read -- no file yet, or one that did not parse
// -- and the caller is expected to build the default home screen instead.
TabletLayout loadTabletLayoutFile();

// Writes the arrangement, replacing the file atomically. False if it could not be
// stored, which is never fatal: the session simply will not be remembered.
bool saveTabletLayoutFile(const TabletLayout& layout);

// --- desktop arrangement (the widget cards and the hand-placed icons) --------

// The cards as the user left them -- kind, rect and page -- from the same config
// directory as the pins. nullopt when there is no file to read, or none holding a
// single recognisable card, which is the caller's cue to lay out the default set.
// A kind this build does not know is skipped rather than trusted, so a file from
// a newer build cannot park a mystery card on the wallpaper.
std::optional<std::vector<Widget>> loadWidgets();

// Writes the card list, replacing the file atomically. False if it could not be
// stored, which is never fatal: the arrangement simply will not be remembered.
bool saveWidgets(const std::vector<Widget>& widgets);

// The cells the user dragged desktop icons into, keyed by the entry's absolute
// path. A missing file is an empty map, never an error: the first session simply
// lays the grid out itself, and an entry that has since been deleted or renamed
// is a key nothing matches, so it is ignored rather than being an error.
std::map<std::string, Point> loadDesktopIconPlacement();

// Writes those cells, so the next session opens with the icons where they were
// left. Same atomic write, same "false is never fatal" rule as everything here.
bool saveDesktopIconPlacement(const std::map<std::string, Point>& placement);

}  // namespace wm
