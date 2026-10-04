// Icon resolution for the shell: Start menu tiles, taskbar buttons, window
// captions and the search box.
//
// The default theme is Reversal (SVG), which scripts/fetch-assets.sh downloads
// and rasterises into assets/icons as PNGs. When librsvg is available an SVG
// *source* is preferred over the raster: it is rendered at a resolution that
// stays sharp at any on-screen size, so the bundled PNGs, the Reversal checkout
// the asset script keeps in the cache, and the desktop's own scalable themes can
// all feed the same crisp path. Without librsvg the raster PNGs are used as
// before. On top of that we fall back to the freedesktop icon theme search path,
// so the icons that come with the desktop's own themes (and any absolute path a
// .desktop file happens to name) still work.
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

class IconStore {
public:
    // `assetDir` holds the rasterised Reversal PNGs (flat, `<name>.png`). It may
    // be empty: everything then comes from the system icon themes.
    void init(const std::string& assetDir);
    // Deletes every uploaded texture; call from shutdown().
    void clear();

    // Requires the GL context to be current. `px` is the edge the icon will be
    // drawn at: an SVG is rasterised at (a bucket at least that large) so it is
    // sampled 1:1 instead of being filtered down from a fixed resolution, which
    // is what made icons look soft.
    IconTex get(const std::string& nameOrPath, int px);
    // A desktop entry's Icon= name, then its StartupWMClass as a fallback.
    IconTex forApp(const std::string& iconName, const std::string& wmClass, int px);

    // Resolves an icon name to a readable PNG path, or "" if there is none.
    // Pure filesystem work: no GL needed, safe to call for diagnostics.
    std::string resolve(const std::string& name) const;
    // Resolves an icon name to an SVG source, or "" if there is none. The bundled
    // assets are checked first (this project's own coloured glyphs), then the
    // Reversal checkout whose rasterised PNGs ship in assets/icons, then the
    // installed icon themes' scalable directories. Pure filesystem work.
    std::string resolveSvg(const std::string& name);

    size_t decodedCount() const { return cache_.size(); }
    size_t missingCount() const { return missing_.size(); }

private:
    std::string assetDir_;
    std::vector<std::string> themeRoots_;
    std::unordered_map<std::string, IconTex> cache_;
    std::unordered_set<std::string> missing_;  // names with no usable source
    // basename (lower-case, no ".svg") -> best SVG path in the Reversal checkout.
    // Built once, lazily, the first time an SVG is wanted.
    std::unordered_map<std::string, std::string> svgIndex_;
    bool svgIndexBuilt_ = false;
    // Per-name resolution memo: which SVG a name maps to, so an animated icon is
    // not re-resolved through the filesystem every frame.
    std::unordered_map<std::string, std::string> svgPath_;
    std::unordered_set<std::string> svgNone_;
    void ensureSvgIndex();
};

}  // namespace wm