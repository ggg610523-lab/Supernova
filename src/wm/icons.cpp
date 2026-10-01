#include "icons.h"
#include "png.h"
#include "util.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <set>
#include <sstream>
#include <sys/stat.h>

namespace wm {
namespace {

std::string lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::vector<std::string> splitPath(const std::string& p) {
    std::vector<std::string> v;
    std::stringstream ss(p);
    std::string t;
    while (std::getline(ss, t, ':')) {
        if (!t.empty()) v.push_back(t);
    }
    return v;
}

bool fileExists(const std::string& p) {
    struct stat st;
    return !p.empty() && stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool dirExists(const std::string& p) {
    struct stat st;
    return !p.empty() && stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// The freedesktop icon-name lookup rules, in the order they are normally tried.
// A desktop entry may say Icon=firefox-esr while the theme ships firefox.png, so
// a single exact match is not enough.
std::vector<std::string> iconCandidates(const std::string& name) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    auto add = [&out, &seen](std::string s) {
        if (s.empty() || s.find('/') != std::string::npos) return;
        if (!seen.insert(s).second) return;
        out.push_back(std::move(s));
    };
    add(name);
    add(lower(name));
    if (name.size() > 9 && name.compare(name.size() - 9, 9, "-symbolic") == 0)
        add(name.substr(0, name.size() - 9));
    // org.gnome.Nautilus / org_gnome_nautilus all reduce to flat names.
    std::string dashed = name;
    for (char& c : dashed) {
        if (c == '.' || c == '_') c = '-';
    }
    add(lower(dashed));
    if (name[0] == '.') add(name.substr(1));
    // Icon=firefox-esr should still find firefox.png, so also try the name with
    // its trailing component dropped.
    const size_t dash = dashed.rfind('-');
    if (dash != std::string::npos && dash > 0) add(lower(dashed.substr(0, dash)));
    // Last dash-separated component, which is what most themes actually ship.
    if (dash != std::string::npos && dash + 1 < dashed.size()) add(dashed.substr(dash + 1));
    return out;
}

// Subdirectory names inside a theme, best first. 48x48 downscaled is the usual
// sweet spot for shell chrome; the rest are fallbacks for partial themes.
const char* kSizeDirs[] = {"48x48",  "scalable", "32x32",   "64x64", "24x24",
                           "22x22",  "16x16",    "symbolic"};
const char* kContexts[] = {"apps",  "actions", "places", "categories", "devices",
                           "status", "mimetypes", "emblems", "panel", ""};

// <dir>/<size>/<context>/<name>.png and <dir>/<size>/<name>.png
std::string findInTheme(const std::string& dir, const std::string& name) {
    if (dir.empty() || !dirExists(dir)) return {};
    for (const char* size : kSizeDirs) {
        const std::string sizeDir = dir + "/" + size;
        for (const char* ctx : kContexts) {
            const std::string p = ctx[0] ? sizeDir + "/" + ctx + "/" + name + ".png"
                                         : sizeDir + "/" + name + ".png";
            if (fileExists(p)) return p;
        }
    }
    return {};
}

// Legacy flat directories: /usr/share/pixmaps, and our own assets/icons.
std::string findFlat(const std::string& dir, const std::string& name) {
    if (dir.empty()) return {};
    const std::string p = dir + "/" + name + ".png";
    return fileExists(p) ? p : std::string();
}

std::vector<std::string> readInherits(const std::string& themeDir) {
    std::vector<std::string> out;
    std::ifstream in(themeDir + "/index.theme");
    if (!in) return out;
    std::string line;
    bool inSection = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line[0] == '[') {
            inSection = line == "[Icon Theme]";
            continue;
        }
        if (!inSection) continue;
        if (line.compare(0, 9, "Inherits=") == 0) {
            std::stringstream ss(line.substr(9));
            std::string t;
            while (std::getline(ss, t, ',')) {
                t = lower(trim(t));
                if (!t.empty()) out.push_back(t);
            }
            break;
        }
    }
    return out;
}

std::string findInTree(const std::string& themeDir, const std::string& name, int depth) {
    if (depth > 8) return {};
    // Preferred themes first: the theme itself, then anything Inherits names.
    if (const std::string f = findInTheme(themeDir, name); !f.empty()) return f;
    for (const std::string& parent : readInherits(themeDir)) {
        if (parent == "hicolor") continue;
        const std::string base = themeDir + "/" + parent;
        if (const std::string f = findInTheme(base, name); !f.empty()) return f;
        if (const std::string f = findInTree(base, name, depth + 1); !f.empty()) return f;
    }
    return {};
}

// An icon search path root (/usr/share/icons) holds many themes, one per
// subdirectory. Collect them, putting the well-known complete ones first so a
// partial theme does not shadow a full one.
std::vector<std::string> themesUnder(const std::string& root) {
    std::vector<std::string> out;
    if (!dirExists(root)) return out;
    out.push_back(root);
    static const char* kPreferred[] = {"Adwaita", "breeze",   "Papirus", "hicolor",
                                       "gnome",   "oxygen",   "elementary", "Deepin"};
    for (const char* pref : kPreferred) {
        const std::string p = root + "/" + pref;
        if (dirExists(p) && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    }
    std::vector<std::string> rest;
    if (DIR* dir = opendir(root.c_str())) {
        while (dirent* ent = readdir(dir)) {
            if (ent->d_name[0] == '.') continue;
            const std::string p = root + "/" + ent->d_name;
            if (!dirExists(p)) continue;
            if (!fileExists(p + "/index.theme")) continue;  // not a theme
            if (std::find(out.begin(), out.end(), p) == out.end()) rest.push_back(p);
        }
        closedir(dir);
    }
    std::sort(rest.begin(), rest.end());
    out.insert(out.end(), rest.begin(), rest.end());
    return out;
}

}  // namespace

void IconStore::init(const std::string& assetDir) {
    assetDir_ = assetDir;
    themeRoots_.clear();
    if (const char* home = std::getenv("HOME")) {
        themeRoots_.push_back(std::string(home) + "/.icons");
        if (const char* xdg = std::getenv("XDG_DATA_HOME")) {
            themeRoots_.push_back(std::string(xdg) + "/icons");
        } else {
            themeRoots_.push_back(std::string(home) + "/.local/share/icons");
        }
    }
    for (const auto& d : splitPath(std::getenv("XDG_DATA_DIRS") ? std::getenv("XDG_DATA_DIRS")
                                                                : "/usr/local/share:/usr/share")) {
        themeRoots_.push_back(d + "/icons");
    }
    if (std::find(themeRoots_.begin(), themeRoots_.end(), std::string("/usr/share/icons")) ==
        themeRoots_.end()) {
        themeRoots_.push_back("/usr/share/icons");
    }
}

void IconStore::clear() {
    for (auto& kv : cache_) {
        if (kv.second.tex) glDeleteTextures(1, &kv.second.tex);
    }
    cache_.clear();
    missing_.clear();
}

std::string IconStore::resolve(const std::string& name) const {
    if (name.empty()) return {};
    // Absolute or relative path straight from the desktop entry.
    if (name.find('/') != std::string::npos) {
        // Only PNG can be decoded here, so try the exact name before appending.
        if (fileExists(name)) return name;
        if (fileExists(name + ".png")) return name + ".png";
        return {};
    }
    // Expand twice: the outer pass turns "org.gnome.Nautilus" into "nautilus",
    // and the inner pass applies the same rules to whatever that produced. Each
    // icon name is only ever looked up once (the result is memoised), so the
    // handful of extra stat calls per name is not worth optimising away.
    std::vector<std::string> names;
    std::set<std::string> seen;
    for (const std::string& step : iconCandidates(name)) {
        for (const std::string& cand : iconCandidates(step)) {
            if (seen.insert(cand).second) names.push_back(cand);
        }
    }
    for (const std::string& cand : names) {
        // 1. Bundled Reversal render (assets/icons/<name>.png).
        if (!assetDir_.empty()) {
            if (const std::string f = findFlat(assetDir_, cand); !f.empty()) return f;
        }
        // 2. Legacy pixmaps.
        for (const char* root : {"/usr/share/pixmaps", "/usr/local/share/pixmaps"}) {
            if (const std::string f = findFlat(root, cand); !f.empty()) return f;
        }
        // 3. XDG themes, then anything they inherit from.
        for (const auto& root : themeRoots_) {
            if (root.empty()) continue;
            for (const auto& theme : themesUnder(root)) {
                if (const std::string f = findInTree(theme, cand, 0); !f.empty()) return f;
            }
        }
    }
    return {};
}

IconTex IconStore::get(const std::string& nameOrPath) {
    if (nameOrPath.empty()) return {};
    const std::string key = lower(nameOrPath);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    if (missing_.count(key)) return {};

    const std::string path = resolve(nameOrPath);
    std::vector<unsigned char> rgba;
    int w = 0, h = 0;
    if (path.empty() || !loadPng(path, &rgba, &w, &h) || w <= 0 || h <= 0) {
        missing_.insert(key);
        return {};
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (!tex) {
        missing_.insert(key);
        return {};
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 rgba.data());
    // Assets are rendered at 128px but drawn into ~16-24px rectangles, so
    // minified hard. Without a mip chain that shimmers and drops thin strokes,
    // which is most of what a line icon is made of.
    glGenerateMipmap(GL_TEXTURE_2D);
    // Pick a single mip level and only bilinear-filter inside it. Trilinear
    // (LINEAR_MIPMAP_LINEAR) blends two levels for most on-screen sizes, and the
    // magnified smaller level is what softens a 128px asset drawn at 48px (or
    // 16px). Choosing the nearest level keeps the edges crisp, which is the
    // macOS look.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    IconTex t{tex, w, h};
    cache_[key] = t;
    return t;
}

IconTex IconStore::forApp(const std::string& iconName, const std::string& wmClass) {
    IconTex t;
    if (!iconName.empty()) t = get(iconName);
    if (!t.valid() && !wmClass.empty()) {
        t = get(wmClass);
        if (!t.valid()) t = get(lower(wmClass));
    }
    return t;
}

}  // namespace wm