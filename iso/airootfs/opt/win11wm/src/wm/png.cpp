#include "png.h"

#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wm {
namespace {

constexpr unsigned char kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

uint32_t be32(const unsigned char* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

int channelsFor(int colorType) {
    switch (colorType) {
        case 0: return 1;  // greyscale
        case 2: return 3;  // truecolour
        case 3: return 1;  // palette index
        case 4: return 2;  // greyscale + alpha
        case 6: return 4;  // truecolour + alpha
        default: return 0;
    }
}

int paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = p > a ? p - a : a - p;
    const int pb = p > b ? p - b : b - p;
    const int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

// Reverses the per-scanline PNG filters in place. `prev` is the already
// reconstructed line above `cur`.
void unfilter(unsigned char* cur, unsigned char filter, const unsigned char* prev,
              size_t lineBytes, size_t bpp) {
    switch (filter) {
        case 0:
            break;
        case 1:
            for (size_t i = bpp; i < lineBytes; ++i) cur[i] += cur[i - bpp];
            break;
        case 2:
            if (prev) {
                for (size_t i = 0; i < lineBytes; ++i) cur[i] += prev[i];
            }
            break;
        case 3:
            for (size_t i = 0; i < lineBytes; ++i) {
                const int left = i >= bpp ? cur[i - bpp] : 0;
                const int up = prev ? prev[i] : 0;
                cur[i] += static_cast<unsigned char>((left + up) / 2);
            }
            break;
        case 4:
            for (size_t i = 0; i < lineBytes; ++i) {
                const int left = i >= bpp ? cur[i - bpp] : 0;
                const int up = prev ? prev[i] : 0;
                const int ul = (prev && i >= bpp) ? prev[i - bpp] : 0;
                cur[i] += static_cast<unsigned char>(paeth(left, up, ul));
            }
            break;
        default:
            break;
    }
}

// Reads the `depth`-bit sample `index` of a packed scanline.
unsigned sampleAt(const unsigned char* line, size_t index, int depth) {
    switch (depth) {
        case 8:
            return line[index];
        case 16:
            return line[index * 2];  // take the high byte of the 16-bit sample
        case 4:
            return (line[index / 2] >> (index % 2 ? 0 : 4)) & 0x0F;
        case 2:
            return (line[index / 4] >> (6 - 2 * (index % 4))) & 0x03;
        case 1:
        default:
            return (line[index / 8] >> (7 - (index % 8))) & 0x01;
    }
}

}  // namespace

bool loadPng(const std::string& path, std::vector<unsigned char>* rgba, int* outW, int* outH) {
    if (!rgba) return false;
    rgba->clear();
    if (outW) *outW = 0;
    if (outH) *outH = 0;

    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 8 + 12) {
        std::fclose(f);
        return false;
    }
    std::vector<unsigned char> file(static_cast<size_t>(size), 0);
    const size_t got = std::fread(file.data(), 1, file.size(), f);
    std::fclose(f);
    if (got != file.size()) return false;
    if (std::memcmp(file.data(), kSignature, 8) != 0) return false;

    uint32_t width = 0, height = 0;
    int depth = 0, colorType = 0, interlace = 0;
    std::vector<unsigned char> palette;
    std::vector<unsigned char> paletteAlpha;
    std::vector<unsigned char> compressed;
    bool sawHeader = false;

    size_t pos = 8;
    while (pos + 8 <= file.size()) {
        const uint32_t length = be32(file.data() + pos);
        const char* type = reinterpret_cast<const char*>(file.data() + pos + 4);
        const size_t dataAt = pos + 8;
        if (dataAt + length + 4 > file.size()) return false;  // truncated
        const unsigned char* data = file.data() + dataAt;

        if (std::strncmp(type, "IHDR", 4) == 0) {
            if (length < 13) return false;
            width = be32(data);
            height = be32(data + 4);
            depth = data[8];
            colorType = data[9];
            interlace = data[12];
            sawHeader = true;
        } else if (std::strncmp(type, "PLTE", 4) == 0) {
            palette.assign(data, data + length);
        } else if (std::strncmp(type, "tRNS", 4) == 0) {
            paletteAlpha.assign(data, data + length);
        } else if (std::strncmp(type, "IDAT", 4) == 0) {
            compressed.insert(compressed.end(), data, data + length);
        } else if (std::strncmp(type, "IEND", 4) == 0) {
            break;
        }
        pos = dataAt + length + 4;  // skip the CRC
    }

    if (!sawHeader || width == 0 || height == 0 || width > 8192 || height > 8192) return false;
    if (interlace != 0) return false;  // Adam7 is not produced by any rasteriser
    const int channels = channelsFor(colorType);
    if (channels == 0) return false;
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) return false;
    if ((depth < 8) && (colorType == 2 || colorType == 4 || colorType == 6)) return false;
    if (colorType == 3 && palette.empty()) return false;
    if (compressed.empty()) return false;

    const size_t lineBytes = (size_t(width) * size_t(channels) * size_t(depth) + 7u) / 8u;
    const size_t bpp = size_t(std::max(1, channels * depth / 8));
    const size_t stride = lineBytes + 1;  // + filter byte

    std::vector<unsigned char> raw(stride * size_t(height));
    uLongf destLen = static_cast<uLongf>(raw.size());
    if (uncompress(raw.data(), &destLen, compressed.data(),
                   static_cast<uLong>(compressed.size())) != Z_OK ||
        destLen != raw.size()) {
        return false;
    }

    std::vector<unsigned char> lines(lineBytes * size_t(height));
    for (uint32_t y = 0; y < height; ++y) {
        unsigned char* dst = lines.data() + size_t(y) * lineBytes;
        const unsigned char* src = raw.data() + size_t(y) * stride;
        const unsigned char filter = src[0];
        if (filter > 4) return false;
        std::memcpy(dst, src + 1, lineBytes);
        // The filter byte lives in the raw stream, not in the copied scanline.
        unfilter(dst, filter, y > 0 ? lines.data() + size_t(y - 1) * lineBytes : nullptr,
                 lineBytes, bpp);
    }

    rgba->assign(size_t(width) * size_t(height) * 4u, 0);
    const int maxSample = (1 << depth) - 1;
    for (uint32_t y = 0; y < height; ++y) {
        const unsigned char* line = lines.data() + size_t(y) * lineBytes;
        unsigned char* out = rgba->data() + size_t(y) * size_t(width) * 4u;
        for (uint32_t x = 0; x < width; ++x) {
            unsigned char r = 0, g = 0, b = 0, a = 255;
            if (colorType == 3) {
                const unsigned idx = sampleAt(line, x, depth);
                if (size_t(idx) * 3 + 2 < palette.size()) {
                    r = palette[idx * 3 + 0];
                    g = palette[idx * 3 + 1];
                    b = palette[idx * 3 + 2];
                }
                a = idx < paletteAlpha.size() ? paletteAlpha[idx] : 255;
            } else if (colorType == 0 || colorType == 4) {
                const unsigned v = sampleAt(line, size_t(x) * size_t(channels), depth);
                const unsigned vv = depth >= 8 ? v : (v * 255u) / unsigned(maxSample);
                r = g = b = static_cast<unsigned char>(vv);
                if (colorType == 4) {
                    a = static_cast<unsigned char>(sampleAt(line, size_t(x) * 2 + 1, depth));
                }
            } else {
                const size_t base = size_t(x) * size_t(channels);
                r = static_cast<unsigned char>(sampleAt(line, base + 0, depth));
                g = static_cast<unsigned char>(sampleAt(line, base + 1, depth));
                b = static_cast<unsigned char>(sampleAt(line, base + 2, depth));
                if (colorType == 6) {
                    a = static_cast<unsigned char>(sampleAt(line, base + 3, depth));
                }
            }
            out[x * 4 + 0] = r;
            out[x * 4 + 1] = g;
            out[x * 4 + 2] = b;
            out[x * 4 + 3] = a;
        }
    }
    if (outW) *outW = int(width);
    if (outH) *outH = int(height);
    return true;
}

}  // namespace wm