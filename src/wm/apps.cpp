#include "apps.h"

#include "util.h"

#include <algorithm>
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

}  // namespace wm
