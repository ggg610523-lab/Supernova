// A small, dependency-light PNG reader.
//
// The shell needs to turn icon files into GPU textures, and the icon theme we
// ship (Reversal) is a pile of SVG that scripts/fetch-assets.sh rasterises with
// rsvg-convert. That produces plain 8-bit PNGs, so this reader only has to cope
// with what non-interlaced PNG actually is: zlib-deflated scanlines with the
// five standard filters. It handles every colour type and bit depth the format
// allows, which also covers the PNGs that come with installed icon themes.
//
// Deliberately not libpng: one zlib stream and ~150 lines beats a build
// dependency that has to be present on every machine the WM runs on.
#pragma once

#include <string>
#include <vector>

namespace wm {

// Decodes `path` into straight (non-premultiplied) 8-bit RGBA. Returns false
// for anything it does not understand and leaves `rgba` empty.
bool loadPng(const std::string& path, std::vector<unsigned char>* rgba, int* outW,
             int* outH);

}  // namespace wm