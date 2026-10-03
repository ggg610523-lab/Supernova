// The Start menu's application list.
//
// Instead of hard coding a launcher table we read the freedesktop .desktop
// entries that are already installed, which is what makes the Start menu able
// to start (almost) any X11 program on the machine.
#pragma once

#include <string>
#include <vector>

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

// The thickness the user dragged the taskbar to, from the same config directory
// as the pins. A missing, unreadable or nonsense value falls back to the Windows
// 11 default rather than failing: a bad setting must never stop the WM starting.
int loadTaskbarHeight();

// Writes the taskbar's thickness, so the bar is still the size the user left it
// next time. Called once, when the drag that changed it ends.
void saveTaskbarHeight(int height);

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

}  // namespace wm
