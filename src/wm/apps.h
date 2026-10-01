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

}  // namespace wm
