// Icon resolution for the shell: Start menu tiles, taskbar buttons, window
// captions and the search box.
//
// The default theme for app icons is Hatter (SVG), the colourful rounded-square
// icon set scripts/fetch-assets.sh downloads and rasterises into
// assets/icons-hatter as PNGs; Hatter's scalable app art is what the shell shows
// for a program. Reversal stays the shell's own monochrome glyph set (drawn from
// assets/icons) and backs the Control Centre. When librsvg is available an SVG
// *source* is preferred over the raster: it is rendered at a resolution that
// stays sharp at any on-screen size, so the bundled PNGs, the Hatter and Reversal
// checkouts the asset script keeps in the cache, and the desktop's own scalable
// themes can all feed the same crisp path. Without librsvg the raster PNGs are
// used as before. On top of that we fall back to the freedesktop icon theme
// search path, so the icons that come with the desktop's own themes (and any
// absolute path a .desktop file happens to name) still work.
//
// Lookups are lazy and cached: an icon is decoded at most once, on the thread
// that owns the GL context, the first time it is actually painted.
#pragma once

#include <epoxy/gl.h>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wm {

class Compositor;

// A decoded icon, ready for Compositor::drawTex().
struct IconTex {
    GLuint tex = 0;
    int w = 0;
    int h = 0;
    bool valid() const { return tex != 0 && w > 0 && h > 0; }
};

// Which bundled theme an icon comes from. Hatter is the colourful, rounded-square
// application icon theme the shell shows for anything that stands for a program;
// Shell is the monochrome Reversal glyph set the shell chrome and the Control
// Centre were drawn against. App art is where colour belongs, so Hatter is the
// default; the Start-button artwork (asked for by path) and the Control Centre
// opt back into Shell so their hand-drawn look is never overridden.
enum class IconTheme { Hatter, Shell };

class IconStore {
public:
    // `assetDir` holds the shell's own rasterised icons (Reversal and this
    // project's glyphs, flat `<name>.png`); `hatterDir` holds the rasterised
    // Hatter app icons, used when librsvg cannot render the checkouts directly.
    // Either may be empty: everything then comes from the system icon themes.
    void init(const std::string& assetDir, const std::string& hatterDir = std::string());
    // Deletes every uploaded texture; call from shutdown().
    void clear();

    // Requires the GL context to be current. `px` is the edge the icon will be
    // drawn at: an SVG is rasterised at (a bucket at least that large) so it is
    // sampled 1:1 instead of being filtered down from a fixed resolution, which
    // is what made icons look soft.
    IconTex get(const std::string& nameOrPath, int px, IconTheme theme = IconTheme::Hatter);
    // A desktop entry's Icon= name, then its StartupWMClass as a fallback.
    IconTex forApp(const std::string& iconName, const std::string& wmClass, int px,
                   IconTheme theme = IconTheme::Hatter);

    // Resolves an icon name to a readable PNG path, or "" if there is none.
    // Pure filesystem work: no GL needed, safe to call for diagnostics.
    std::string resolve(const std::string& name,
                        IconTheme theme = IconTheme::Hatter) const;
    // Resolves an icon name to an SVG source, or "" if there is none. For Hatter
    // the Hatter checkout comes first (its colourful app art), then the bundled
    // assets (this project's own coloured glyphs), then the Reversal checkout
    // whose rasterised PNGs ship in assets/icons, then the installed icon themes'
    // scalable directories. Pure filesystem work.
    std::string resolveSvg(const std::string& name, IconTheme theme = IconTheme::Hatter);

    size_t decodedCount() const { return cache_.size(); }
    size_t missingCount() const { return missing_.size(); }

private:
    std::string assetDir_;
    std::string hatterDir_;
    std::vector<std::string> themeRoots_;
    std::unordered_map<std::string, IconTex> cache_;
    std::unordered_set<std::string> missing_;  // "<h|s>:<name>" with no source
    // basename (lower-case, no ".svg") -> best SVG path in the Reversal checkout.
    // Built once, lazily, the first time an SVG is wanted.
    std::unordered_map<std::string, std::string> svgIndex_;
    bool svgIndexBuilt_ = false;
    // The same index for the Hatter checkout's default theme, so its colourful
    // app icons resolve at any size instead of only through the bundled PNGs.
    std::unordered_map<std::string, std::string> hatterIndex_;
    bool hatterIndexBuilt_ = false;
    // Per-name resolution memo: which SVG a name maps to, so an animated icon is
    // not re-resolved through the filesystem every frame. The key is prefixed
    // with the theme so the same name can mean two different glyphs.
    std::unordered_map<std::string, std::string> svgPath_;
    std::unordered_set<std::string> svgNone_;
    void ensureSvgIndex();
    void ensureHatterIndex();
};

}  // namespace wm