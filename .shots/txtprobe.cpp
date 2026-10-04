// Replicates src/wm/text.cpp layout()/rasterise() math offline to check whether
// the rendered ink of a string fits the texture width the code allocates.
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H
#include <cstdio>
#include <cstdint>
#include <string>

static constexpr int kLoadFlags = FT_LOAD_DEFAULT | FT_LOAD_FORCE_AUTOHINT;
static constexpr int kRenderFlags =
    FT_LOAD_RENDER | FT_LOAD_FORCE_AUTOHINT | FT_LOAD_TARGET_NORMAL;

static int utf8Len(unsigned char lead) {
    if ((lead & 0x80u) == 0) return 1;
    if ((lead & 0xE0u) == 0xC0u) return 2;
    if ((lead & 0xF0u) == 0xE0u) return 3;
    if ((lead & 0xF8u) == 0xF0u) return 4;
    return 1;
}
static uint32_t nextCp(const std::string& s, size_t* i) {
    const size_t n = s.size();
    unsigned char lead = (unsigned char)s[*i];
    int len = utf8Len(lead);
    if (len <= 1 || *i + (size_t)len > n) { *i += 1; return lead; }
    uint32_t cp = (uint32_t)lead & (0xFFu >> len);
    for (int k = 1; k < len; ++k)
        cp = (cp << 6) | ((uint32_t)(unsigned char)s[*i + k] & 0x3Fu);
    *i += len;
    return cp;
}

static void probe(FT_Face face, const std::string& s, int px, bool synth) {
    FT_Set_Pixel_Sizes(face, 0, (FT_UInt)px);
    int pen = 0;
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = nextCp(s, &i);
        if (FT_Load_Char(face, cp, kLoadFlags) != 0) continue;
        if (synth) FT_GlyphSlot_Embolden(face->glyph);
        pen += (int)(face->glyph->advance.x >> 6);
    }
    const int width = pen > 0 ? pen : 0;
    const int height = (int)(face->size->metrics.height >> 6);
    const int baseline = (int)(face->size->metrics.ascender >> 6);

    int minx = 1 << 30, maxx = -(1 << 30);
    int miny = 1 << 30, maxy = -(1 << 30);
    pen = 0;
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = nextCp(s, &i);
        if (FT_Load_Char(face, cp, kRenderFlags) != 0) continue;
        if (synth) FT_GlyphSlot_Embolden(face->glyph);
        const FT_Bitmap& bm = face->glyph->bitmap;
        const int gx = pen + face->glyph->bitmap_left;
        const int gy = baseline - face->glyph->bitmap_top;
        if (bm.rows && bm.width) {
            if (gx < minx) minx = gx;
            if (gx + (int)bm.width > maxx) maxx = gx + (int)bm.width;
            if (gy < miny) miny = gy;
            if (gy + (int)bm.rows > maxy) maxy = gy + (int)bm.rows;
        }
        pen += (int)(face->glyph->advance.x >> 6);
    }
    const bool clipL = minx < 0;
    const bool clipR = maxx > width;
    const bool clipT = miny < 0;
    const bool clipB = maxy > height;
    std::printf("%-28s px=%2d synth=%d width=%3d height=%2d ink x[%d..%d] y[%d..%d]  %s%s%s%s\n",
                s.c_str(), px, synth, width, height, minx, maxx, miny, maxy,
                clipL ? "CLIP-LEFT " : "", clipR ? "CLIP-RIGHT " : "",
                clipT ? "CLIP-TOP " : "", clipB ? "CLIP-BOTTOM " : "");
}

int main() {
    FT_Library lib;
    if (FT_Init_FreeType(&lib) != 0) { std::printf("no lib\n"); return 1; }
    FT_Face face = nullptr;
    const char* path = "/home/mango/Desktop/Supernova/assets/fonts/MuternVF.ttf";
    if (FT_New_Face(lib, path, 0, &face) != 0) { std::printf("no face %s\n", path); return 1; }
    for (const char* s : {"12:30", "Battery", "MetaTrader 5", "Add/Remove Soft…",
                          "Task view", "Delete this folder?", "Sun 04 Oct"}) {
        for (int synth = 0; synth <= 1; ++synth) probe(face, s, 16, synth);
    }
    FT_Done_Face(face);
    FT_Done_FreeType(lib);
    return 0;
}
