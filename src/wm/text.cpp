#include "text.h"

#include "util.h"

#include <epoxy/gl.h>

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_SYNTHESIS_H

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace wm {
namespace {

// First family fontconfig can actually resolve wins. An installed MuternVF
// (bundled or system wide) is preferred; otherwise these humanist sans faces
// keep the Windows 11 proportions.
const char* const kFamilies[] = {
    "MuternVF", "Segoe UI Variable Display", "Segoe UI", "Noto Sans", "DejaVu Sans",
    "Adwaita Sans", "Liberation Sans", "FreeSans", "sans-serif",
};

constexpr size_t kMaxEntries = 900;

// Grid-fit glyphs and keep the *measure* and *render* passes on identical load
// flags, so the hinted advances used by layout() are the ones rasterise() draws
// and the texture is never a pixel narrow. Forcing the autohinter also means any
// face fontconfig resolves -- including the fallbacks used when MuternVF is
// absent -- lands stems on whole pixels instead of scaling freely.
constexpr int kLoadFlags = FT_LOAD_DEFAULT | FT_LOAD_FORCE_AUTOHINT;
constexpr int kRenderFlags = FT_LOAD_RENDER | FT_LOAD_FORCE_AUTOHINT | FT_LOAD_TARGET_NORMAL;

int weightToFc(Weight w) {
    switch (w) {
        case Weight::Bold: return FC_WEIGHT_BOLD;
        case Weight::Medium: return FC_WEIGHT_MEDIUM;
        default: return FC_WEIGHT_REGULAR;
    }
}

bool resolveFamily(const char* family, int fcWeight, std::string* file, int* index) {
    FcPattern* pat = FcNameParse(reinterpret_cast<const FcChar8*>(family));
    if (!pat) return false;
    FcPatternDel(pat, FC_WEIGHT);
    FcPatternAddInteger(pat, FC_WEIGHT, fcWeight);
    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult result = FcResultNoMatch;
    FcPattern* match = FcFontMatch(nullptr, pat, &result);
    FcPatternDestroy(pat);
    if (!match) return false;

    FcChar8* path = nullptr;
    int idx = 0;
    const bool ok = result != FcResultNoMatch &&
                    FcPatternGetString(match, FC_FILE, 0, &path) == FcResultMatch &&
                    path != nullptr &&
                    FcPatternGetInteger(match, FC_INDEX, 0, &idx) == FcResultMatch;
    if (ok) {
        *file = reinterpret_cast<const char*>(path);
        *index = idx;
    }
    FcPatternDestroy(match);
    return ok;
}

// Decode the codepoint at `*i` and advance `*i` past it.
uint32_t nextCodepoint(const std::string& s, size_t* i) {
    const size_t n = s.size();
    const unsigned char lead = static_cast<unsigned char>(s[*i]);
    const int len = utf8Len(lead);
    if (len <= 1 || *i + static_cast<size_t>(len) > n) {
        *i += 1;
        return lead;
    }
    uint32_t cp = uint32_t(lead) & (0xFFu >> len);
    for (int k = 1; k < len; ++k) {
        cp = (cp << 6) | (uint32_t(static_cast<unsigned char>(s[*i + static_cast<size_t>(k)])) & 0x3Fu);
    }
    *i += static_cast<size_t>(len);
    return cp;
}

// ---------------------------------------------------------------------------
// Bundled font: MuternVF, a variable font. One file gives us Regular / Medium /
// Bold by moving the wght axis, which is exactly the three weights the Fluent
// shell uses, and it keeps the assets to a single file.
// ---------------------------------------------------------------------------
bool loadVariableFont(FT_Library lib, const std::string& path, void* faces[3],
                      bool hasOpsz[3], double opszMin[3], double opszMax[3]) {
    const int weights[3] = {400, 500, 700};
    for (int i = 0; i < 3; ++i) {
        FT_Face face = nullptr;
        if (FT_New_Face(lib, path.c_str(), 0, &face) != 0 || !face) return false;
        FT_MM_Var* mm = nullptr;
        if (FT_Get_MM_Var(face, &mm) != 0 || !mm) {
            FT_Done_Face(face);
            return false;
        }
        // FT_Set_Var_Design_Coordinates() takes plain 16.16 design values, one
        // per axis; the ranges come from the fvar table the face carries.
        bool ok = false;
        if (mm->num_axis > 0 && mm->num_axis <= 16) {
            FT_Fixed coords[16];
            for (FT_UInt a = 0; a < mm->num_axis; ++a) coords[a] = mm->axis[a].def;
            for (FT_UInt a = 0; a < mm->num_axis; ++a) {
                if (mm->axis[a].tag != FT_MAKE_TAG('w', 'g', 'h', 't')) continue;
                const double lo = double(mm->axis[a].minimum) / 65536.0;
                const double hi = double(mm->axis[a].maximum) / 65536.0;
                const double want = double(weights[i]);
                const double clamped = want < lo ? lo : (want > hi ? hi : want);
                coords[a] = FT_Fixed(clamped * 65536.0);
                ok = true;
            }
            if (ok && FT_Set_Var_Design_Coordinates(face, mm->num_axis, coords) != 0) ok = false;
            // MuternVF also has an optical size axis, which is what makes it
            // legible at caption sizes. Remember its range; faceFor() drives it
            // from the requested pixel size.
            for (FT_UInt a = 0; a < mm->num_axis; ++a) {
                if (mm->axis[a].tag != FT_MAKE_TAG('o', 'p', 's', 'z')) continue;
                opszMin[i] = double(mm->axis[a].minimum) / 65536.0;
                opszMax[i] = double(mm->axis[a].maximum) / 65536.0;
                hasOpsz[i] = true;
            }
        }
        FT_Done_MM_Var(lib, mm);
        if (!ok) {
            FT_Done_Face(face);
            return false;
        }
        faces[i] = face;
    }
    return true;
}

// The bundled face opened as an ordinary static font. MuternVF ships as a
// variable font upstream, but a plain build of the same face carries no fvar
// table, and then loadVariableFont() above cannot drive a wght axis at all. In
// that case open the one file once per weight so the whole shell still renders in
// MuternVF (Bold is synthesized from it -- see faceFor()).
bool loadBundledStatic(FT_Library lib, const std::string& path, void* faces[3]) {
    for (int i = 0; i < 3; ++i) {
        FT_Face face = nullptr;
        if (FT_New_Face(lib, path.c_str(), 0, &face) != 0 || !face) return false;
        faces[i] = face;
    }
    return true;
}

// Static per-weight instances shipped next to the variable font; used when the
// FreeType we link against cannot set variation coordinates.
bool loadStaticFont(FT_Library lib, const std::string& dir, void* faces[3]) {
    static const char* files[3] = {"MuternVF-TextRegular.ttf", "MuternVF-TextMedium.ttf",
                                   "MuternVF-TextBold.ttf"};
    for (int i = 0; i < 3; ++i) {
        FT_Face face = nullptr;
        if (FT_New_Face(lib, (dir + "/" + files[i]).c_str(), 0, &face) != 0 || !face) return false;
        faces[i] = face;
    }
    return true;
}

}  // namespace

int utf8Len(unsigned char lead) {
    if ((lead & 0x80u) == 0) return 1;
    if ((lead & 0xE0u) == 0xC0u) return 2;
    if ((lead & 0xF0u) == 0xE0u) return 3;
    if ((lead & 0xF8u) == 0xF0u) return 4;
    return 1;
}

Text::~Text() { shutdown(); }

bool Text::init(const std::string& fontDir) {
    if (ready_ || attempted_) return ready_;
    attempted_ = true;

    if (!FcInit()) {
        log("fontconfig: init failed");
        return false;
    }
    FT_Library lib = nullptr;
    if (FT_Init_FreeType(&lib) != 0) {
        log("FreeType: init failed");
        return false;
    }
    lib_ = lib;

    // faces_[0] = Regular, [1] = Medium, [2] = Bold. Medium and Bold are
    // optional: faceFor() falls back to Regular when a file is missing.

    // 1. The bundled MuternVF variable font.
    if (!fontDir.empty()) {
        const std::string variable = fontDir + "/MuternVF.ttf";
        if (loadVariableFont(lib, variable, faces_, hasOpsz_, opszMin_, opszMax_)) {
            family_ = "MuternVF";
            bundled_ = true;
            for (int i = 0; i < 3; ++i) facePx_[i] = 0;
            ready_ = true;
            log("font: MuternVF (variable, bundled)");
            return true;
        }
        // 1b. The same file as a plain static face (no variable axes). This is
        // what actually ships today, so without it the whole shell silently fell
        // through to fontconfig and rendered in whatever face that resolved to.
        if (loadBundledStatic(lib, variable, faces_)) {
            family_ = "MuternVF";
            bundled_ = true;
            synthBold_ = true;
            for (int i = 0; i < 3; ++i) facePx_[i] = 0;
            ready_ = true;
            log("font: MuternVF (static, bundled; bold synthesized)");
            return true;
        }
        // 2. Static Text instances from the same family.
        if (loadStaticFont(lib, fontDir, faces_)) {
            family_ = "MuternVF";
            bundled_ = true;
            for (int i = 0; i < 3; ++i) facePx_[i] = 0;
            ready_ = true;
            log("font: MuternVF (static Text instances, bundled)");
            return true;
        }
        log("warning: bundled font not usable in %s; falling back to system faces",
            fontDir.c_str());
    }

    // 3. fontconfig. An installed MuternVF is preferred; otherwise the first
    // family that resolves wins. On Linux "Segoe UI" is normally absent, so we
    // degrade through humanist sans faces whose metrics are close enough to keep
    // the Windows 11 proportions.
    for (int i = 0; i < 3; ++i) {
        const Weight w = i == 0 ? Weight::Regular : (i == 1 ? Weight::Medium : Weight::Bold);
        std::string file;
        int index = 0;
        bool found = false;
        for (const char* fam : kFamilies) {
            if (resolveFamily(fam, weightToFc(w), &file, &index)) {
                if (family_.empty()) family_ = fam;
                found = true;
                break;
            }
        }
        if (!found) continue;
        FT_Face face = nullptr;
        if (FT_New_Face(lib, file.c_str(), index, &face) != 0 || !face) continue;
        faces_[i] = face;
        facePx_[i] = 0;
    }
    if (!faces_[0]) {
        log("no usable UI font found; the shell will render without text");
        return false;
    }
    ready_ = true;
    log("font: \"%s\"%s", family_.c_str(), bundled_ ? " (bundled)" : "");
    return true;
}

void Text::shutdown() {
    for (auto& kv : cache_) {
        if (kv.second.tex.tex) {
            GLuint t = static_cast<GLuint>(kv.second.tex.tex);
            glDeleteTextures(1, &t);
        }
    }
    cache_.clear();
    order_.clear();
    synthBold_ = false;
    for (void*& f : faces_) {
        if (f) FT_Done_Face(static_cast<FT_Face>(f));
        f = nullptr;
        facePx_[0] = facePx_[1] = facePx_[2] = 0;
        hasOpsz_[0] = hasOpsz_[1] = hasOpsz_[2] = false;
    }
    if (lib_) {
        FT_Done_FreeType(static_cast<FT_Library>(lib_));
        lib_ = nullptr;
    }
    ready_ = false;
}

void* Text::faceFor(int px, Weight w) {
    int idx = w == Weight::Bold ? 2 : (w == Weight::Medium ? 1 : 0);
    if (!faces_[idx]) idx = 0;
    FT_Face face = static_cast<FT_Face>(faces_[idx]);
    if (!face) return nullptr;
    if (facePx_[idx] != px) {
        if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(px)) != 0) return nullptr;
        facePx_[idx] = px;
        // Optical size follows the pixel size, which is the whole point of the
        // axis: 13px caption text wants the "text" cut, not the display one.
        if (hasOpsz_[idx]) {
            FT_MM_Var* mm = nullptr;
            if (FT_Get_MM_Var(face, &mm) == 0 && mm && mm->num_axis > 0 && mm->num_axis <= 16) {
                FT_Fixed coords[16];
                for (FT_UInt a = 0; a < mm->num_axis; ++a) coords[a] = mm->axis[a].def;
                for (FT_UInt a = 0; a < mm->num_axis; ++a) {
                    if (mm->axis[a].tag != FT_MAKE_TAG('o', 'p', 's', 'z')) continue;
                    const double want = std::min(std::max(double(px), opszMin_[idx]),
                                                 opszMax_[idx]);
                    coords[a] = FT_Fixed(want * 65536.0);
                }
                FT_Set_Var_Design_Coordinates(face, mm->num_axis, coords);
                FT_Done_MM_Var(static_cast<FT_Library>(lib_), mm);
            }
        }
    }
    return face;
}

int Text::lineHeight(int px) {
    void* f = faceFor(px, Weight::Regular);
    if (!f) return px + 4;
    return static_cast<int>(static_cast<FT_Face>(f)->size->metrics.height >> 6);
}

// Metrics only (no rendering): the pen position after the last glyph is the
// string width, and the face's `height` is the line height.
void Text::layout(const std::string& s, void* facePtr, int* outW, int* outH) {
    FT_Face face = static_cast<FT_Face>(facePtr);
    int pen = 0;
    for (size_t i = 0; i < s.size();) {
        const uint32_t cp = nextCodepoint(s, &i);
        // Same flags as the render pass so measurement and drawing agree on the
        // hinted advances and the texture is never one pixel short.
        if (FT_Load_Char(face, cp, kLoadFlags) != 0) continue;
        // A static bundled face has no real Bold: thicken its outline so the
        // measured advance matches what rasterise() draws.
        if (synthBold_ && facePtr == faces_[2]) FT_GlyphSlot_Embolden(face->glyph);
        pen += static_cast<int>(face->glyph->advance.x >> 6);
    }
    *outW = pen > 0 ? pen : 0;
    *outH = static_cast<int>(face->size->metrics.height >> 6);
}


// Render `s` into a tightly packed RGBA bitmap (white + coverage in alpha) and
// hand it to GL. One texture per cached string, alpha-blended by the shader.
void Text::rasterise(const std::string& s, void* facePtr, TextTex* out, int width, int height) {
    FT_Face face = static_cast<FT_Face>(facePtr);
    std::vector<unsigned char> buf(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0);
    const int baseline = static_cast<int>(face->size->metrics.ascender >> 6);
    int pen = 0;

    for (size_t i = 0; i < s.size();) {
        const uint32_t cp = nextCodepoint(s, &i);
        if (FT_Load_Char(face, cp, kRenderFlags) != 0) continue;
        // Match layout(): synthesize the missing Bold from the static face.
        if (synthBold_ && facePtr == faces_[2]) FT_GlyphSlot_Embolden(face->glyph);
        const FT_Bitmap& bm = face->glyph->bitmap;
        const int gx = pen + face->glyph->bitmap_left;
        const int gy = baseline - face->glyph->bitmap_top;
        const int rows = static_cast<int>(bm.rows);
        const int cols = static_cast<int>(bm.width);

        for (int row = 0; row < rows; ++row) {
            const int py = gy + row;
            if (py < 0 || py >= height) continue;
            const unsigned char* src = bm.buffer + static_cast<ptrdiff_t>(row) * bm.pitch;
            for (int col = 0; col < cols; ++col) {
                unsigned char cov = 0;
                if (bm.pixel_mode == FT_PIXEL_MODE_GRAY) {
                    cov = src[col];
                } else if (bm.pixel_mode == FT_PIXEL_MODE_MONO) {
                    cov = (src[col >> 3] & (0x80u >> (col & 7))) ? 255 : 0;
                } else {
                    continue;  // colour/LCD bitmaps are not used for chrome
                }
                if (!cov) continue;
                const int px = gx + col;
                if (px < 0 || px >= width) continue;
                const size_t o = (static_cast<size_t>(py) * static_cast<size_t>(width) +
                                  static_cast<size_t>(px)) * 4u;
                buf[o + 0] = 255;
                buf[o + 1] = 255;
                buf[o + 2] = 255;
                // Max-blend so overlapping glyph boxes never darken each other.
                if (cov > buf[o + 3]) buf[o + 3] = cov;
            }
        }
        pen += static_cast<int>(face->glyph->advance.x >> 6);
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (!tex) return;
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    out->tex = tex;
    out->w = width;
    out->h = height;
}

void Text::evictLocked() {
    while (!order_.empty() && cache_.size() >= kMaxEntries) {
        const Key key = order_.front();
        order_.pop_front();
        auto it = cache_.find(key);
        if (it == cache_.end()) continue;
        if (it->second.tex.tex) {
            GLuint t = static_cast<GLuint>(it->second.tex.tex);
            glDeleteTextures(1, &t);
        }
        cache_.erase(it);
    }
}

TextTex Text::get(const std::string& s, int px, Weight w) {
    if (s.empty() || px <= 0) return TextTex{};
    const Key key{s, px, static_cast<int>(w)};
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second.tex;

    void* face = faceFor(px, w);
    if (!face) return TextTex{};

    int ww = 0, hh = 0;
    layout(s, face, &ww, &hh);
    if (ww <= 0 || hh <= 0) return TextTex{};

    TextTex out{};
    rasterise(s, face, &out, ww, hh);
    if (!out.tex) return out;

    if (cache_.size() >= kMaxEntries) evictLocked();
    cache_.emplace(key, Entry{out, key});
    order_.push_back(key);
    return out;
}

int Text::measure(const std::string& s, int px, Weight w, int* outH) {
    if (s.empty() || px <= 0) {
        if (outH) *outH = lineHeight(px);
        return 0;
    }
    auto it = cache_.find(Key{s, px, static_cast<int>(w)});
    if (it != cache_.end()) {
        if (outH) *outH = it->second.tex.h;
        return it->second.tex.w;
    }
    void* face = faceFor(px, w);
    if (!face) {
        if (outH) *outH = px + 4;
        return 0;
    }
    int ww = 0, hh = 0;
    layout(s, face, &ww, &hh);
    if (outH) *outH = hh;
    return ww;
}

}  // namespace wm
