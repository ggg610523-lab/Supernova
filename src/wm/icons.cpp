#include "icons.h"
#include "png.h"
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

#ifdef WIN11WM_HAVE_RSVG
#include <cairo.h>
// rsvg.h pulls in the cairo rendering entry points itself; including
// rsvg-cairo.h directly is deprecated.
#include <librsvg/rsvg.h>
#endif

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

// The flat SVG variant: our own assets/icons/<name>.svg.
std::string findFlatSvg(const std::string& dir, const std::string& name) {
    if (dir.empty()) return {};
    const std::string p = dir + "/" + name + ".svg";
    return fileExists(p) ? p : std::string();
}

// <dir>/<size>/<context>/<name>.svg and <dir>/<size>/<name>.svg -- the vector
// counterpart of findInTheme, so a theme that ships only scalable art still
// resolves.
std::string findSvgInTheme(const std::string& dir, const std::string& name) {
    if (dir.empty() || !dirExists(dir)) return {};
    for (const char* size : kSizeDirs) {
        const std::string sizeDir = dir + "/" + size;
        for (const char* ctx : kContexts) {
            const std::string p = ctx[0] ? sizeDir + "/" + ctx + "/" + name + ".svg"
                                         : sizeDir + "/" + name + ".svg";
            if (fileExists(p)) return p;
        }
    }
    return {};
}

std::vector<std::string> readInherits(const std::string& themeDir);

std::string findSvgInTree(const std::string& themeDir, const std::string& name, int depth) {
    if (depth > 8) return {};
    if (const std::string f = findSvgInTheme(themeDir, name); !f.empty()) return f;
    for (const std::string& parent : readInherits(themeDir)) {
        if (parent == "hicolor") continue;
        const std::string base = themeDir + "/" + parent;
        if (const std::string f = findSvgInTheme(base, name); !f.empty()) return f;
        if (const std::string f = findSvgInTree(base, name, depth + 1); !f.empty()) return f;
    }
    return {};
}

// The Reversal checkout scripts/fetch-assets.sh keeps in the cache. Its SVGs are
// the source the bundled PNGs were rasterised from, so reading them directly is
// what lets an icon stay sharp at any size.
std::string reversalRoot() {
    std::string base;
    if (const char* xdg = std::getenv("XDG_CACHE_HOME")) {
        if (xdg[0] == '/') base = xdg;
    }
    if (base.empty()) {
        if (const char* home = std::getenv("HOME")) {
            if (home[0] == '/') base = std::string(home) + "/.cache";
        }
    }
    if (base.empty()) return {};
    return base + "/win11wm/reversal";
}

// Scores a candidate SVG path: a scalable one beats a fixed-size one, the theme's
// own src/ beats a mirror, and a shorter path (fewer nested copies) wins ties.
int svgScore(const std::string& p) {
    int s = 0;
    if (p.find("/scalable/") != std::string::npos) s += 1000;
    if (p.find("/src/") != std::string::npos) s += 100;
    s -= int(p.size() / 16);
    return s;
}

void indexSvgs(const std::string& dir, std::unordered_map<std::string, std::string>* out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* ent = readdir(d)) {
        const char* n = ent->d_name;
        if (n[0] == '.') continue;
        const std::string p = dir + "/" + n;
        struct stat st {};
        if (::stat(p.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            indexSvgs(p, out);
            continue;
        }
        if (!S_ISREG(st.st_mode)) continue;
        const size_t len = std::strlen(n);
        if (len <= 4 || std::strcmp(n + len - 4, ".svg") != 0) continue;
        const std::string key = lower(std::string(n, len - 4));
        auto it = out->find(key);
        if (it == out->end()) {
            out->emplace(key, p);
            continue;
        }
        if (svgScore(p) > svgScore(it->second)) it->second = p;
    }
    closedir(d);
}

// The freedesktop name rules applied twice (org.gnome.Nautilus -> nautilus), the
// same expansion IconStore::resolve() does for PNGs.
std::vector<std::string> expandedCandidates(const std::string& name) {
    std::vector<std::string> names;
    std::set<std::string> seen;
    for (const std::string& step : iconCandidates(name)) {
        for (const std::string& cand : iconCandidates(step)) {
            if (seen.insert(cand).second) names.push_back(cand);
        }
    }
    return names;
}

#ifdef WIN11WM_HAVE_RSVG
void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

// Reversal ships monochrome art with a fixed #363636; the shell is dark, so the
// same recolour the theme's installer does is applied before rendering.
bool isReversalPath(const std::string& p) {
    return p.find("/win11wm/reversal/") != std::string::npos;
}

// The render sizes an icon is available at. A draw size is rounded *up* to the
// next rung, so the texture is always sampled at 1:1 or mildly minified and never
// magnified; the ladder bounds how many rasterisations an icon can ever have, so
// an animated icon cannot thrash the cache. Ratio <= 1.5 keeps minification from
// visibly softening an edge.
int iconBucket(int px) {
    static constexpr int kLadder[] = {16, 24, 32, 48, 64, 96, 128, 192, 256};
    for (int b : kLadder) {
        if (px <= b) return b;
    }
    return 256;
}

// Renders `path` into `px` square of straight (non-premultiplied) RGBA, matching
// what loadPng() returns.
bool renderSvg(const std::string& path, bool recolour, int px,
               std::vector<unsigned char>* rgba, int* outW, int* outH) {
    if (px <= 0) return false;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::string data;
    char buf[8192];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof buf, f)) > 0) data.append(buf, got);
    std::fclose(f);
    if (data.empty()) return false;

    if (recolour) {
        replaceAll(data, "#363636", "#dedede");
        replaceAll(data, "#3b3b3b", "#dedede");
    }

    GError* err = nullptr;
    RsvgHandle* handle = rsvg_handle_new_from_data(
        reinterpret_cast<const guint8*>(data.data()), gsize(data.size()), &err);
    if (!handle) {
        if (err) g_error_free(err);
        return false;
    }
    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
    cairo_t* cr = cairo_create(surf);
    const RsvgRectangle viewport{0.0, 0.0, double(px), double(px)};
    const bool ok = rsvg_handle_render_document(handle, cr, &viewport, &err) != FALSE;
    cairo_destroy(cr);
    cairo_surface_flush(surf);
    if (!ok) {
        if (err) g_error_free(err);
        cairo_surface_destroy(surf);
        g_object_unref(handle);
        return false;
    }

    // cairo stores premultiplied BGRA; the shader wants straight RGBA (it applies
    // the alpha itself), so each pixel is un-premultiplied on the way out.
    const unsigned char* src = cairo_image_surface_get_data(surf);
    const int stride = cairo_image_surface_get_stride(surf);
    rgba->assign(size_t(px) * size_t(px) * 4u, 0);
    for (int y = 0; y < px; ++y) {
        const unsigned char* line = src + size_t(y) * size_t(stride);
        unsigned char* out = rgba->data() + size_t(y) * size_t(px) * 4u;
        for (int x = 0; x < px; ++x) {
            const unsigned char b = line[x * 4 + 0];
            const unsigned char g = line[x * 4 + 1];
            const unsigned char r = line[x * 4 + 2];
            const unsigned char a = line[x * 4 + 3];
            if (a == 0) {
                out[x * 4 + 0] = out[x * 4 + 1] = out[x * 4 + 2] = out[x * 4 + 3] = 0;
            } else if (a == 255) {
                out[x * 4 + 0] = r;
                out[x * 4 + 1] = g;
                out[x * 4 + 2] = b;
                out[x * 4 + 3] = 255;
            } else {
                out[x * 4 + 0] = (unsigned char)std::min(255, int(r) * 255 / a);
                out[x * 4 + 1] = (unsigned char)std::min(255, int(g) * 255 / a);
                out[x * 4 + 2] = (unsigned char)std::min(255, int(b) * 255 / a);
                out[x * 4 + 3] = a;
            }
        }
    }
    cairo_surface_destroy(surf);
    g_object_unref(handle);
    if (outW) *outW = px;
    if (outH) *outH = px;
    return true;
}
#endif  // WIN11WM_HAVE_RSVG

// Uploads straight RGBA as a GL texture. `mipmaps` is used only by the raster
// fallback: a mip chain averages straight-alpha texels and blurs an edge, so an
// SVG -- which is already rasterised at the draw size -- is uploaded flat and
// sampled 1:1 instead.
IconTex uploadIcon(const std::vector<unsigned char>& rgba, int w, int h, bool mipmaps) {
    if (rgba.empty() || w <= 0 || h <= 0) return {};
    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (!tex) return {};
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    mipmaps ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (mipmaps) glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    return IconTex{tex, w, h};
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
    const std::vector<std::string> names = expandedCandidates(name);
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

std::string IconStore::resolveSvg(const std::string& name) {
    if (name.empty()) return {};
    // An absolute path from a .desktop entry: only a real .svg is worth taking,
    // everything else is left to the raster path.
    if (name.find('/') != std::string::npos) {
        const size_t len = name.size();
        if (len > 4 && name.compare(len - 4, 4, ".svg") == 0 && fileExists(name)) return name;
        return {};
    }
    const std::vector<std::string> names = expandedCandidates(name);

    // 1. The project's own SVGs (assets/icons/<name>.svg), already coloured.
    for (const std::string& cand : names) {
        if (const std::string f = findFlatSvg(assetDir_, cand); !f.empty()) return f;
    }
    // 2. The Reversal checkout: the vector source of the bundled PNGs.
    ensureSvgIndex();
    for (const std::string& cand : names) {
        auto it = svgIndex_.find(lower(cand));
        if (it != svgIndex_.end()) return it->second;
    }
    // 3. Installed icon themes' scalable directories.
    for (const std::string& cand : names) {
        for (const auto& root : themeRoots_) {
            if (root.empty()) continue;
            for (const auto& theme : themesUnder(root)) {
                if (const std::string f = findSvgInTree(theme, cand, 0); !f.empty()) return f;
            }
        }
    }
    return {};
}

void IconStore::ensureSvgIndex() {
    if (svgIndexBuilt_) return;
    svgIndexBuilt_ = true;
    const std::string root = reversalRoot();
    if (root.empty() || !dirExists(root)) return;
    indexSvgs(root, &svgIndex_);
}

IconTex IconStore::get(const std::string& nameOrPath, int px) {
    if (nameOrPath.empty()) return {};
    const std::string base = lower(nameOrPath);

#ifdef WIN11WM_HAVE_RSVG
    // A vector source beats the raster: it is rasterised at the size it will be
    // drawn, so it is sampled 1:1 and stays sharp instead of being filtered down
    // from one fixed resolution. The name -> SVG lookup is memoised because an
    // animated icon asks again every frame while its size changes.
    auto svg = svgPath_.find(base);
    if (svg == svgPath_.end() && !svgNone_.count(base)) {
        std::string path = resolveSvg(nameOrPath);
        if (path.empty()) {
            svgNone_.insert(base);
        } else {
            svg = svgPath_.emplace(base, std::move(path)).first;
        }
    }
    if (svg != svgPath_.end()) {
        const int bucket = iconBucket(px);
        const std::string key = base + "@" + std::to_string(bucket);
        auto it = cache_.find(key);
        if (it != cache_.end()) return it->second;
        std::vector<unsigned char> rgba;
        int w = 0, h = 0;
        if (renderSvg(svg->second, isReversalPath(svg->second), bucket, &rgba, &w, &h)) {
            const IconTex t = uploadIcon(rgba, w, h, false);
            if (t.valid()) {
                cache_[key] = t;
                return t;
            }
        }
        // A malformed SVG falls through to the raster path rather than failing.
    }
#else
    (void)px;
#endif

    auto it = cache_.find(base);
    if (it != cache_.end()) return it->second;
    if (missing_.count(base)) return {};

    const std::string path = resolve(nameOrPath);
    std::vector<unsigned char> rgba;
    int w = 0, h = 0;
    if (path.empty() || !loadPng(path, &rgba, &w, &h) || w <= 0 || h <= 0) {
        missing_.insert(base);
        return {};
    }
    const IconTex t = uploadIcon(rgba, w, h, true);
    if (!t.valid()) {
        missing_.insert(base);
        return {};
    }
    cache_[base] = t;
    return t;
}

IconTex IconStore::forApp(const std::string& iconName, const std::string& wmClass, int px) {
    IconTex t;
    if (!iconName.empty()) t = get(iconName, px);
    if (!t.valid() && !wmClass.empty()) {
        t = get(wmClass, px);
        if (!t.valid()) t = get(lower(wmClass), px);
    }
    return t;
}

}  // namespace wm