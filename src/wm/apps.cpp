#include "apps.h"

#include "theme.h"
#include "util.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace wm {
namespace {

std::string lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// Exec= lines may contain field codes (%f, %U, %i ...) which are meaningless
// without a file argument list; drop them and collapse the whitespace.
std::string stripFieldCodes(const std::string& exec) {
    std::string out;
    out.reserve(exec.size());
    for (size_t i = 0; i < exec.size(); ++i) {
        if (exec[i] == '%' && i + 1 < exec.size()) {
            const char c = exec[i + 1];
            if (c == '%') {
                out += '%';
                ++i;
                continue;
            }
            if (std::strchr("fFuUdDnNickvm", c)) {
                ++i;
                continue;
            }
        }
        out += exec[i];
    }
    std::string clean;
    bool space = false;
    for (char c : out) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            space = true;
            continue;
        }
        if (space && !clean.empty()) clean += ' ';
        space = false;
        clean += c;
    }
    return trim(clean);
}

struct DesktopEntry {
    AppEntry app;
    bool typeApplication = false;
    bool hidden = false;
    bool noDisplay = false;
};

// Reads one .desktop file. Returns false unless it is a usable Application
// entry (the same bar the Start menu applies), in which case `out` is filled.
bool parseDesktopFile(const std::string& path, AppEntry* out) {
    std::ifstream in(path);
    if (!in) return false;

    DesktopEntry e;
    bool inEntry = false;
    std::string line;
    std::string comment;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (t[0] == '[') {
            // Only the main group matters; [Desktop Action ...] groups carry
            // their own Exec and must not leak into the entry.
            inEntry = (t == "[Desktop Entry]");
            continue;
        }
        if (!inEntry) continue;
        const size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = t.substr(0, eq);
        // Skip localised keys such as Name[de].
        if (key.find('[') != std::string::npos) continue;
        const std::string val = trim(t.substr(eq + 1));

        if (key == "Name") e.app.name = val;
        else if (key == "Exec") e.app.exec = stripFieldCodes(val);
        else if (key == "GenericName") e.app.generic = val;
        else if (key == "Comment") comment = val;
        else if (key == "Icon") e.app.icon = val;
        else if (key == "Terminal") e.app.terminal = (val == "true");
        else if (key == "Type") e.typeApplication = (val == "Application");
        else if (key == "Hidden") e.hidden = (val == "true");
        else if (key == "NoDisplay") e.noDisplay = (val == "true");
        else if (key == "StartupWMClass") e.app.wmClass = lower(val);
    }

    if (!e.typeApplication || e.hidden || e.noDisplay) return false;
    if (e.app.name.empty() || e.app.exec.empty()) return false;

    if (!e.app.generic.empty()) e.app.generic += ' ';
    e.app.generic += comment;
    e.app.searchKey = lower(e.app.name + " " + e.app.generic);
    *out = std::move(e.app);
    return true;
}

void parseDesktop(const std::string& path, std::vector<AppEntry>* out,
                  std::set<std::string>* seen) {
    AppEntry app;
    if (!parseDesktopFile(path, &app)) return;
    const std::string dedup = lower(app.name);
    if (!seen->insert(dedup).second) return;
    out->push_back(std::move(app));
}

// A file's icon name, guessed from its extension. Only a guess: an icon theme
// that ships nothing for the name simply falls back to a letter tile.
std::string fileIcon(const std::string& name) {
    const size_t dot = name.rfind('.');
    const std::string ext = dot == std::string::npos ? std::string() : lower(name.substr(dot + 1));
    if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "gif" || ext == "webp" ||
        ext == "bmp" || ext == "svg") {
        return "image-x-generic";
    }
    if (ext == "pdf") return "application-pdf";
    if (ext == "sh" || ext == "bash" || ext == "run" || ext == "appimage" || ext == "exe") {
        return "application-x-executable";
    }
    if (ext == "mp4" || ext == "mkv" || ext == "webm" || ext == "avi" || ext == "mov") {
        return "video-x-generic";
    }
    if (ext == "mp3" || ext == "flac" || ext == "ogg" || ext == "wav") return "audio-x-generic";
    if (ext == "zip" || ext == "tar" || ext == "gz" || ext == "xz" || ext == "zst" ||
        ext == "7z" || ext == "rar") {
        return "package-x-generic";
    }
    return "text-plain";
}

std::vector<std::string> dataDirs() {
    std::vector<std::string> dirs;
    if (const char* home = std::getenv("HOME")) {
        dirs.push_back(std::string(home) + "/.local/share/applications");
    }
    if (const char* xdg = std::getenv("XDG_DATA_DIRS")) {
        std::stringstream ss(xdg);
        std::string part;
        while (std::getline(ss, part, ':')) {
            if (!part.empty()) dirs.push_back(part + "/applications");
        }
    }
    dirs.emplace_back("/usr/local/share/applications");
    dirs.emplace_back("/usr/share/applications");
    return dirs;
}

}  // namespace

bool containsFold(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    return lower(haystack).find(lower(needle)) != std::string::npos;
}

std::string desktopDir() {
    const char* home = std::getenv("HOME");
    if (!home) return {};
    // user-dirs.dirs wins when it exists: a session that has been localised into
    // another language keeps its folders wherever that file says they are.
    std::ifstream in(std::string(home) + "/.config/user-dirs.dirs");
    std::string line;
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.rfind("XDG_DESKTOP_DIR", 0) != 0) continue;
        const size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string v = trim(t.substr(eq + 1));
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
            v = v.substr(1, v.size() - 2);
        }
        // The file stores $HOME/Desktop rather than an absolute path.
        if (v.rfind("$HOME", 0) == 0) v = std::string(home) + v.substr(5);
        if (!v.empty()) return v;
    }
    return std::string(home) + "/Desktop";
}

std::vector<DesktopItem> scanDesktop() {
    std::vector<DesktopItem> items;
    const std::string dir = desktopDir();
    if (dir.empty()) return items;
    DIR* d = opendir(dir.c_str());
    if (!d) return items;

    std::vector<std::string> names;
    while (dirent* ent = readdir(d)) {
        const char* n = ent->d_name;
        // "." and ".." are hidden by definition; hidden files are not shown,
        // which is what every desktop does.
        if (n[0] == '.') continue;
        names.emplace_back(n);
    }
    closedir(d);
    std::sort(names.begin(), names.end());

    for (const std::string& n : names) {
        const std::string path = dir + "/" + n;
        struct stat st {};
        if (::stat(path.c_str(), &st) != 0) continue;
        DesktopItem item;
        item.path = path;
        item.isDir = S_ISDIR(st.st_mode) != 0;

        if (item.isDir) {
            item.name = n;
            item.icon = "folder";
            items.push_back(std::move(item));
            continue;
        }
        // An installer's launcher: show the entry's own name and icon, and run
        // its Exec rather than handing the file to a viewer.
        const size_t len = n.size();
        if (len > 8 && n.compare(len - 8, 8, ".desktop") == 0) {
            AppEntry app;
            if (parseDesktopFile(path, &app)) {
                item.name = app.name.empty() ? n.substr(0, len - 8) : app.name;
                item.icon = app.icon;
                item.exec = app.exec;
                item.isDesktopEntry = true;
                items.push_back(std::move(item));
                continue;
            }
        }
        item.name = n;
        item.icon = fileIcon(n);
        items.push_back(std::move(item));
    }
    return items;
}

std::string tabletFolderDir() {
    const std::string d = desktopDir();
    return d.empty() ? std::string{} : d + "/tablet";
}

// The folder is a pure convenience: whether it exists or not, the tablet home
// screen reads it. Making it on first use just means the user can see where a
// launcher goes without having to guess.
bool ensureTabletFolder() {
    const std::string dir = tabletFolderDir();
    if (dir.empty()) return false;
    struct stat st {};
    if (::stat(dir.c_str(), &st) == 0) return S_ISDIR(st.st_mode) != 0;
    return ::mkdir(dir.c_str(), 0755) == 0;
}

std::vector<DesktopItem> scanTabletFolder() {
    std::vector<DesktopItem> items;
    const std::string dir = tabletFolderDir();
    if (dir.empty()) return items;
    DIR* d = opendir(dir.c_str());
    if (!d) return items;

    std::vector<std::string> names;
    while (dirent* ent = readdir(d)) {
        if (ent->d_name[0] == '.') continue;
        names.emplace_back(ent->d_name);
    }
    closedir(d);
    std::sort(names.begin(), names.end());

    for (const std::string& n : names) {
        // Only .desktop launchers are recognised: the folder is the curated list
        // the home screen shows, so a stray file or another folder belongs to
        // the desktop, not to the grid.
        const size_t len = n.size();
        if (len <= 8 || n.compare(len - 8, 8, ".desktop") != 0) continue;
        const std::string path = dir + "/" + n;
        AppEntry app;
        if (!parseDesktopFile(path, &app)) continue;
        DesktopItem item;
        item.path = path;
        item.isDesktopEntry = true;
        item.name = app.name.empty() ? n.substr(0, len - 8) : app.name;
        item.icon = app.icon;
        item.exec = app.exec;
        items.push_back(std::move(item));
    }
    return items;
}

bool usableDesktopName(const std::string& name) {
    if (name.empty() || name == "." || name == "..") return false;
    if (name.find('/') != std::string::npos) return false;
    // scanDesktop() hides dotfiles, so a hidden entry would be created and then
    // never be seen or opened again.
    if (name[0] == '.') return false;
    for (const char ch : name)
        if (static_cast<unsigned char>(ch) < 0x20) return false;
    return true;
}

namespace {

bool pathExists(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0;
}

// Depth-first removal, so a folder goes together with everything inside it. Links
// are unlinked rather than followed, so a symlink on the desktop cannot lead the
// delete out of the desktop and into whatever it points at.
bool removeTree(const std::string& path) {
    struct stat st {};
    if (::lstat(path.c_str(), &st) != 0) return false;
    if (S_ISDIR(st.st_mode)) {
        if (DIR* d = opendir(path.c_str())) {
            while (dirent* e = readdir(d)) {
                const char* n = e->d_name;
                if (n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0'))) continue;
                removeTree(path + "/" + n);
            }
            closedir(d);
        }
        return ::rmdir(path.c_str()) == 0;
    }
    return ::unlink(path.c_str()) == 0;
}

}  // namespace

bool createDesktopFolder(const std::string& base, std::string* made) {
    const std::string dir = desktopDir();
    if (dir.empty() || !usableDesktopName(base)) return false;
    // "New Folder", then "New Folder 2" and up. A second folder should never fail
    // just because the first is still there, so the number is part of the name
    // rather than a collision the user has to clear.
    for (int n = 1; n < 1000; ++n) {
        const std::string name = n == 1 ? base : base + " " + std::to_string(n);
        if (pathExists(dir + "/" + name)) continue;
        if (::mkdir((dir + "/" + name).c_str(), 0755) == 0) {
            if (made) *made = name;
            return true;
        }
        // Something else took the name between the check and the mkdir. Anything
        // else -- a read-only desktop, a full disk -- is not worth retrying.
        if (errno != EEXIST) return false;
    }
    return false;
}

bool renameDesktopEntry(const std::string& path, const std::string& name) {
    const std::string dir = desktopDir();
    if (dir.empty() || path.empty() || !usableDesktopName(name)) return false;
    const std::string to = dir + "/" + name;
    if (to == path) return true;  // renamed to what it already is
    // Refuse to land on top of something else rather than replace it.
    if (pathExists(to)) return false;
    return ::rename(path.c_str(), to.c_str()) == 0;
}

bool deleteDesktopEntry(const std::string& path) {
    if (path.empty()) return false;
    return removeTree(path);
}

std::vector<AppEntry> scanApps() {
    std::vector<AppEntry> apps;
    std::set<std::string> seen;
    for (const std::string& dir : dataDirs()) {
        DIR* d = opendir(dir.c_str());
        if (!d) continue;
        std::vector<std::string> names;
        while (dirent* ent = readdir(d)) {
            const char* n = ent->d_name;
            const size_t len = std::strlen(n);
            if (len > 8 && std::strcmp(n + len - 8, ".desktop") == 0) names.emplace_back(n);
        }
        closedir(d);
        // Deterministic order: readdir order is filesystem dependent.
        std::sort(names.begin(), names.end());
        for (const std::string& n : names) parseDesktop(dir + "/" + n, &apps, &seen);
    }
    std::sort(apps.begin(), apps.end(),
              [](const AppEntry& a, const AppEntry& b) { return a.name < b.name; });
    return apps;
}

void launchApp(const std::string& exec) {
    if (exec.empty()) return;
    const pid_t pid = fork();
    if (pid < 0) {
        log("fork failed while launching \"%s\"", exec.c_str());
        return;
    }
    if (pid == 0) {
        // New session: the app never sees the WM's terminal signals and can
        // never become a zombie we would have to reap.
        setsid();
        execl("/bin/sh", "sh", "-c", exec.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    log("launched: %s", exec.c_str());
}

// ---------------------------------------------------------------- taskbar pins
namespace {

// One pin per line, fields tab separated in the order of AppEntry: name, exec,
// icon, wmClass, terminal. Tabs are the only thing a .desktop string could
// contain that would break the line format, so they are squeezed out on write.
std::string pinField(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    }
    return out;
}

std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> out;
    size_t a = 0;
    while (true) {
        const size_t tab = line.find('\t', a);
        if (tab == std::string::npos) {
            out.push_back(line.substr(a));
            return out;
        }
        out.push_back(line.substr(a, tab - a));
        a = tab + 1;
    }
}

// mkdir -p: $HOME/.config is not guaranteed to exist on a fresh account, and a
// single mkdir() cannot create a whole chain of missing parents.
void makeDirs(const std::string& path) {
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i < path.size() && path[i] != '/') continue;
        const std::string dir = path.substr(0, i);
        if (dir.empty() || isDirectory(dir)) continue;
        ::mkdir(dir.c_str(), 0700);
    }
}

// Write via a sibling temp file and rename over the target, so a crash halfway
// through leaves the previous file intact instead of a truncated one. Creates the
// config directory if it is missing.
bool writeFileAtomicImpl(const std::string& path, const std::string& text) {
    const size_t slash = path.rfind('/');
    if (slash != std::string::npos) makeDirs(path.substr(0, slash));
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        out << text;
        if (!out) return false;
    }
    return ::rename(tmp.c_str(), path.c_str()) == 0;
}

// $XDG_CONFIG_HOME/win11wm/<leaf>, else $HOME/.config/win11wm/<leaf>. Empty when
// neither variable names an absolute path, which is the only way these settings
// cannot be stored at all.
std::string configFile(const char* leaf) {
    std::string base;
    if (const char* env = std::getenv("XDG_CONFIG_HOME")) {
        if (env[0] == '/') base = env;
    }
    if (base.empty()) {
        if (const char* home = std::getenv("HOME")) {
            if (home[0] == '/') base = std::string(home) + "/.config";
        }
    }
    if (base.empty()) return {};
    return base + "/win11wm/" + leaf;
}

}  // namespace

std::string configPath(const char* leaf) { return configFile(leaf); }

bool writeFileAtomic(const std::string& path, const std::string& text) {
    return writeFileAtomicImpl(path, text);
}

namespace {

// Tabs and newlines are the field and record separators in the saved layout, so
// they cannot survive inside a name or a path. Same treatment the taskbar pins
// get, for the same reason.
std::string layoutField(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    }
    return out;
}

// The saved form of one app: a full snapshot of the entry, so a folder keeps
// working -- and keeps its icon -- even if the .desktop file it came from is
// gone. exec non-empty means "run this", empty means "hand path to xdg-open".
std::string layoutRecord(const char* kind, const TabletEntry& e) {
    std::ostringstream t;
    t << kind << '\t' << layoutField(e.name) << '\t' << layoutField(e.exec) << '\t'
      << layoutField(e.path) << '\t' << layoutField(e.icon) << '\t'
      << layoutField(e.wmClass) << '\n';
    return t.str();
}

TabletEntry layoutEntry(const std::vector<std::string>& f) {
    TabletEntry e;
    if (f.size() > 1) e.name = f[1];
    if (f.size() > 2) e.exec = f[2];
    if (f.size() > 3) e.path = f[3];
    if (f.size() > 4) e.icon = f[4];
    if (f.size() > 5) e.wmClass = f[5];
    return e;
}

std::vector<std::string> splitFields(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == '\t') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}
}  // namespace

bool sameTabletApp(const TabletEntry& a, const TabletEntry& b) {
    if (!a.exec.empty() || !b.exec.empty()) return a.exec == b.exec && a.name == b.name;
    return a.path == b.path && a.name == b.name;
}

void removeTabletAppFromHome(std::vector<TabletItem>& home, const TabletEntry& app) {
    for (size_t i = 0; i < home.size();) {
        TabletItem& item = home[i];
        if (!item.isFolder) {
            if (sameTabletApp(item.app, app)) {
                home.erase(home.begin() + long(i));
                continue;
            }
            ++i;
            continue;
        }
        TabletFolder& f = item.folder;
        f.apps.erase(std::remove_if(f.apps.begin(), f.apps.end(),
                                    [&](const TabletEntry& a) { return sameTabletApp(a, app); }),
                     f.apps.end());
        if (f.apps.size() >= 2) {
            ++i;
            continue;
        }
        // A folder with too little left in it stops being a folder: its remainder
        // takes its place on the grid as plain icons.
        const std::vector<TabletEntry> rest = f.apps;
        home.erase(home.begin() + long(i));
        for (const TabletEntry& a : rest) {
            TabletItem plain;
            plain.app = a;
            home.insert(home.begin() + long(i), std::move(plain));
            ++i;
        }
    }
}

TabletLayout loadTabletLayoutFile() {
    TabletLayout layout;
    const std::string path = configFile("tablet-layout");
    if (path.empty()) return layout;
    std::ifstream in(path);
    if (!in) return layout;

    // A folder is a header followed by its apps, so it is read into `pending` and
    // flushed when the next header -- or the end of the file -- turns up.
    TabletItem pending;
    bool inFolder = false;
    auto flush = [&] {
        if (!inFolder) return;
        // Two apps is what makes a folder a folder; anything less goes back on the
        // grid as plain icons rather than being kept as a folder of one.
        if (pending.folder.apps.size() >= 2) {
            layout.home.push_back(pending);
        } else {
            for (const TabletEntry& e : pending.folder.apps) {
                TabletItem item;
                item.app = e;
                layout.home.push_back(std::move(item));
            }
        }
        pending = TabletItem{};
        inFolder = false;
    };

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const std::vector<std::string> f = splitFields(line);
        const std::string& kind = f[0];
        if (kind == "folder" && f.size() > 1) {
            flush();
            pending.isFolder = true;
            pending.folder.name = f[1];
            inFolder = true;
        } else if (kind == "in" && inFolder) {
            pending.folder.apps.push_back(layoutEntry(f));
        } else if (kind == "dock" && f.size() > 3) {
            flush();
            if (int(layout.dock.size()) < metrics::kTabletDockMax)
                layout.dock.push_back(layoutEntry(f));
        } else if (kind == "app" && f.size() > 3) {
            flush();
            TabletItem item;
            item.app = layoutEntry(f);
            layout.home.push_back(std::move(item));
        }
    }
    flush();

    // An app in both places would show twice on one screen, which is exactly what
    // the dock is there to prevent, so a repeat is taken off the grid by the same
    // rule that moves an app into the dock at runtime.
    for (const TabletEntry& d : layout.dock) removeTabletAppFromHome(layout.home, d);

    return layout;
}

bool saveTabletLayoutFile(const TabletLayout& layout) {
    const std::string path = configFile("tablet-layout");
    if (path.empty()) {
        log("cannot save the tablet layout: no XDG_CONFIG_HOME or HOME");
        return false;
    }
    std::ostringstream text;
    text << "# win11wm tablet home screen\n";
    for (const TabletEntry& e : layout.dock) text << layoutRecord("dock", e);
    for (const TabletItem& item : layout.home) {
        if (item.isFolder) {
            text << "folder\t" << layoutField(item.folder.name) << '\n';
            for (const TabletEntry& e : item.folder.apps) text << layoutRecord("in", e);
        } else {
            text << layoutRecord("app", item.app);
        }
    }
    if (!writeFileAtomicImpl(path, text.str())) {
        log("cannot write the tablet layout to %s", path.c_str());
        return false;
    }
    log("saved the tablet layout (%zu dock, %zu home) to %s", layout.dock.size(),
        layout.home.size(), path.c_str());
    return true;
}

std::string pinnedPath() { return configFile("pinned"); }

int loadTaskbarHeight() {
    const std::string path = configFile("taskbar-height");
    if (path.empty()) return metrics::kTaskbarDefaultH;
    std::ifstream in(path);
    int height = 0;
    // Unreadable, unparsable or out-of-range -- including a value left by a build
    // with different limits -- falls back to the default. A bad setting must never
    // be the reason the WM comes up wrong.
    if (!(in >> height) || height < metrics::kTaskbarMinH || height > metrics::kTaskbarMaxH) {
        return metrics::kTaskbarDefaultH;
    }
    return height;
}

void saveTaskbarHeight(int height) {
    if (height < metrics::kTaskbarMinH || height > metrics::kTaskbarMaxH) return;
    const std::string path = configFile("taskbar-height");
    if (path.empty()) {
        log("cannot save taskbar height: no XDG_CONFIG_HOME or HOME");
        return;
    }
    if (!writeFileAtomicImpl(path, std::to_string(height) + "\n")) {
        log("cannot write taskbar height to %s", path.c_str());
        return;
    }
    log("saved taskbar height %d to %s", height, path.c_str());
}

theme::Mode loadThemeMode() {
    const std::string path = configFile("theme-mode");
    if (path.empty()) return theme::Mode::Dark;
    std::ifstream in(path);
    std::string mode;
    if (!(in >> mode)) return theme::Mode::Dark;
    // A file from a future build, or one the user has edited, must not stop the
    // shell coming up -- an unrecognised palette is simply not a palette.
    if (mode == "light") return theme::Mode::Light;
    return theme::Mode::Dark;
}

void saveThemeMode(theme::Mode m) {
    const std::string path = configFile("theme-mode");
    if (path.empty()) {
        log("cannot save theme mode: no XDG_CONFIG_HOME or HOME");
        return;
    }
    const std::string word = (m == theme::Mode::Light) ? "light\n" : "dark\n";
    if (!writeFileAtomicImpl(path, word)) {
        log("cannot write theme mode to %s", path.c_str());
        return;
    }
    log("saved theme mode %s to %s", word.c_str(), path.c_str());
}

std::vector<AppEntry> loadPinned() {
    PinnedList pins;
    const std::string path = pinnedPath();
    if (path.empty()) return pins;
    std::ifstream in(path);
    if (!in) return pins;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const std::vector<std::string> f = splitTabs(line);
        // name and exec are the only fields a pin cannot work without; the rest
        // are the icon and the class used to recognise the app's windows.
        if (f.size() < 2 || f[0].empty() || f[1].empty()) continue;
        AppEntry e;
        e.name = f[0];
        e.exec = f[1];
        if (f.size() > 2) e.icon = f[2];
        if (f.size() > 3) e.wmClass = f[3];
        if (f.size() > 4) e.terminal = f[4] == "1";
        e.searchKey = lower(e.name);
        // The same launcher twice: a hand-edited file, or a write from an older
        // WM that did not know about pinning. First line wins.
        const bool dup = std::any_of(pins.begin(), pins.end(), [&](const AppEntry& p) {
            return p.exec == e.exec;
        });
        if (dup) continue;
        pins.push_back(e);
    }
    return pins;
}

void savePinned(const PinnedList& pins) {
    const std::string path = pinnedPath();
    if (path.empty()) {
        log("cannot save taskbar pins: no XDG_CONFIG_HOME or HOME");
        return;
    }
    std::ostringstream text;
    for (const AppEntry& e : pins) {
        text << pinField(e.name) << '\t' << pinField(e.exec) << '\t' << pinField(e.icon) << '\t'
             << pinField(e.wmClass) << '\t' << (e.terminal ? '1' : '0') << '\n';
    }
    if (!writeFileAtomicImpl(path, text.str())) {
        log("cannot write taskbar pins to %s", path.c_str());
        return;
    }
    log("saved %zu taskbar pin(s) to %s", pins.size(), path.c_str());
}

std::string recentsPath() { return configFile("recents"); }

std::vector<AppEntry> loadRecents() {
    PinnedList recents;
    const std::string path = recentsPath();
    if (path.empty()) return recents;
    std::ifstream in(path);
    if (!in) return recents;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const std::vector<std::string> f = splitTabs(line);
        // A recent record is the launcher snapshot, so name and exec are the
        // only two fields it cannot work without.
        if (f.size() < 2 || f[0].empty() || f[1].empty()) continue;
        AppEntry e;
        e.name = f[0];
        e.exec = f[1];
        if (f.size() > 2) e.icon = f[2];
        if (f.size() > 3) e.wmClass = f[3];
        e.searchKey = lower(e.name);
        // The same launcher twice: a hand-edited file, or a write from an older
        // WM. First line wins, exactly as the pin file does.
        const bool dup = std::any_of(recents.begin(), recents.end(), [&](const AppEntry& r) {
            return r.exec == e.exec;
        });
        if (dup) continue;
        recents.push_back(e);
    }
    return recents;
}

void saveRecents(const PinnedList& recents) {
    const std::string path = recentsPath();
    if (path.empty()) {
        log("cannot save recent apps: no XDG_CONFIG_HOME or HOME");
        return;
    }
    std::ostringstream text;
    for (const AppEntry& e : recents) {
        text << pinField(e.name) << '\t' << pinField(e.exec) << '\t' << pinField(e.icon) << '\t'
             << pinField(e.wmClass) << '\n';
    }
    if (!writeFileAtomicImpl(path, text.str())) {
        log("cannot write recent apps to %s", path.c_str());
        return;
    }
    log("saved %zu recent app(s) to %s", recents.size(), path.c_str());
}

std::string recentFilesPath() { return configFile("recent-files"); }

RecentFileList loadRecentFiles() {
    RecentFileList files;
    const std::string path = recentFilesPath();
    if (path.empty()) return files;
    std::ifstream in(path);
    if (!in) return files;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const std::vector<std::string> f = splitTabs(line);
        // A recent file cannot work without either a name or a path.
        if (f.size() < 2 || (f[0].empty() && f[1].empty())) continue;
        RecentFile r;
        r.name = f[0];
        r.path = f[1];
        if (f.size() > 2) r.icon = f[2];
        if (f.size() > 3) r.isDir = f[3] == "1";
        // The same file twice: a hand-edited file, or a write from an older WM.
        bool dup = false;
        for (const RecentFile& o : files) {
            if (o.path == r.path) {
                dup = true;
                break;
            }
        }
        if (dup) continue;
        files.push_back(std::move(r));
    }
    return files;
}

void saveRecentFiles(const RecentFileList& files) {
    const std::string path = recentFilesPath();
    if (path.empty()) {
        log("cannot save recent files: no XDG_CONFIG_HOME or HOME");
        return;
    }
    std::ostringstream text;
    for (const RecentFile& r : files) {
        text << pinField(r.name) << '\t' << pinField(r.path) << '\t' << pinField(r.icon)
             << '\t' << (r.isDir ? '1' : '0') << '\n';
    }
    if (!writeFileAtomicImpl(path, text.str())) {
        log("cannot write recent files to %s", path.c_str());
        return;
    }
    log("saved %zu recent file(s) to %s", files.size(), path.c_str());
}

std::string launchpadOrderPath() { return configFile("launchpad-order"); }

std::vector<std::string> loadLaunchpadOrder() {
    std::vector<std::string> order;
    const std::string path = launchpadOrderPath();
    if (path.empty()) return order;
    std::ifstream in(path);
    if (!in) return order;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        order.push_back(line);
    }
    return order;
}

void saveLaunchpadOrder(const std::vector<std::string>& order) {
    const std::string path = launchpadOrderPath();
    if (path.empty()) {
        log("cannot save the Launchpad order: no XDG_CONFIG_HOME or HOME");
        return;
    }
    std::ostringstream text;
    for (const std::string& exec : order) {
        for (char ch : exec) {
            if (ch == '\t' || ch == '\n' || ch == '\r') continue;
            text << ch;
        }
        text << '\n';
    }
    if (!writeFileAtomicImpl(path, text.str())) {
        log("cannot write the Launchpad order to %s", path.c_str());
        return;
    }
    log("saved %zu app(s) to the Launchpad order %s", order.size(), path.c_str());
}

// ---------------------------------------------------------------------------
// Desktop arrangement: the widget cards, and the cells the icons were dragged to
// ---------------------------------------------------------------------------

namespace {

// The saved word for a card kind. These words are the file format, so they are
// written out in full rather than as the enum's numbers: a build that reordered
// WidgetKind must not be able to move the user's cards.
const char* widgetKindWord(WidgetKind k) {
    switch (k) {
        case WidgetKind::Clock: return "clock";
        case WidgetKind::Battery: return "battery";
        case WidgetKind::Calendar: return "calendar";
        case WidgetKind::Weather: return "weather";
        case WidgetKind::DigitalClock: return "digital";
    }
    return "clock";
}

bool widgetKindFromWord(const std::string& s, WidgetKind* out) {
    if (s == "clock") *out = WidgetKind::Clock;
    else if (s == "battery") *out = WidgetKind::Battery;
    else if (s == "calendar") *out = WidgetKind::Calendar;
    else if (s == "weather") *out = WidgetKind::Weather;
    else if (s == "digital") *out = WidgetKind::DigitalClock;
    else return false;
    return true;
}

// A number read back from a file the user may have edited by hand. Anything that
// is not a plain integer reads as absent rather than as zero, so one broken line
// drops out instead of parking a card in the top-left corner.
bool fileInt(const std::string& s, int* out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0') return false;
    *out = int(v);
    return true;
}

}  // namespace

std::optional<std::vector<Widget>> loadWidgets() {
    const std::string path = configFile("widgets");
    if (path.empty()) return std::nullopt;
    std::ifstream in(path);
    if (!in) return std::nullopt;

    std::vector<Widget> out;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const std::vector<std::string> f = splitFields(line);
        if (f.size() < 6) continue;
        WidgetKind kind;
        int x = 0, y = 0, w = 0, h = 0, page = 0;
        if (!widgetKindFromWord(f[0], &kind)) continue;
        if (!fileInt(f[1], &x) || !fileInt(f[2], &y) || !fileInt(f[3], &w) ||
            !fileInt(f[4], &h))
            continue;
        if (w <= 0 || h <= 0) continue;
        Widget widget;
        widget.kind = kind;
        widget.rect = Rect{x, y, w, h};
        widget.page = (fileInt(f[5], &page) && page > 0) ? page : 0;
        out.push_back(widget);
    }
    // A file that holds no recognisable card reads as no file at all: the default
    // set is a better desktop than an empty one, and a missing file is how the
    // very first session reads anyway.
    if (out.empty()) return std::nullopt;
    return out;
}

bool saveWidgets(const std::vector<Widget>& widgets) {
    const std::string path = configFile("widgets");
    if (path.empty()) {
        log("cannot save the desktop widgets: no XDG_CONFIG_HOME or HOME");
        return false;
    }
    std::ostringstream text;
    text << "# win11wm desktop widgets: kind, x, y, w, h, page\n";
    for (const Widget& w : widgets) {
        text << widgetKindWord(w.kind) << '\t' << w.rect.x << '\t' << w.rect.y << '\t'
             << w.rect.w << '\t' << w.rect.h << '\t' << w.page << '\n';
    }
    if (!writeFileAtomicImpl(path, text.str())) {
        log("cannot write the desktop widgets to %s", path.c_str());
        return false;
    }
    log("saved %zu desktop widget(s) to %s", widgets.size(), path.c_str());
    return true;
}

std::map<std::string, Point> loadDesktopIconPlacement() {
    std::map<std::string, Point> out;
    const std::string path = configFile("desktop-icons");
    if (path.empty()) return out;
    std::ifstream in(path);
    if (!in) return out;

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const std::vector<std::string> f = splitFields(line);
        // The path is the record: without it there is no icon to match, and
        // without both coordinates there is no cell to go to.
        if (f.size() < 3 || f[0].empty()) continue;
        int x = 0, y = 0;
        if (!fileInt(f[1], &x) || !fileInt(f[2], &y)) continue;
        out[f[0]] = Point{x, y};
    }
    return out;
}

bool saveDesktopIconPlacement(const std::map<std::string, Point>& placement) {
    const std::string path = configFile("desktop-icons");
    if (path.empty()) {
        log("cannot save the desktop icon positions: no XDG_CONFIG_HOME or HOME");
        return false;
    }
    std::ostringstream text;
    text << "# win11wm desktop icons: path, x, y\n";
    for (const auto& entry : placement) {
        text << layoutField(entry.first) << '\t' << entry.second.x << '\t'
             << entry.second.y << '\n';
    }
    if (!writeFileAtomicImpl(path, text.str())) {
        log("cannot write the desktop icon positions to %s", path.c_str());
        return false;
    }
    log("saved %zu desktop icon position(s) to %s", placement.size(), path.c_str());
    return true;
}

}  // namespace wm
