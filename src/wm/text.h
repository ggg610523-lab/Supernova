// Text for the shell chrome: title bars, taskbar clock, Start menu, Alt-Tab.
//
// We rasterise with FreeType and hand the coverage bitmap straight to OpenGL as
// an RGBA texture (RGB = white, A = coverage) so the one shader used for every
// primitive can tint text with `color.a`. Strings are cached because the shell
// re-draws the same handful of labels every frame at 60Hz.
//
// The GL texture objects are only touched from the thread that owns the
// context, which for us is the single WM thread that also runs the event loop.
#pragma once

#include <deque>
#include <string>
#include <unordered_map>

namespace wm {

struct TextTex {
    unsigned int tex = 0;
    int w = 0;
    int h = 0;
};

enum class Weight { Regular, Medium, Bold };

class Text {
public:
    ~Text();
    Text() = default;
    Text(const Text&) = delete;
    Text& operator=(const Text&) = delete;

    // fontconfig + FreeType only: no GL context needed yet. `fontDir` is the
    // directory the bundled UI font lives in (see scripts/fetch-assets.sh);
    // when it holds MuternVF.ttf that variable font is used directly, with the
    // static Text instances and fontconfig as fallbacks.
    bool init(const std::string& fontDir = std::string());
    // Loads one specific file for every weight slot. The digital clock's display
    // face is a single heavy weight, not a family with Regular/Medium/Bold, so all
    // three slots open the same cut and the widget can ask for Bold and still get
    // the display weight it wants. False leaves the instance unusable, which the
    // caller falls back from.
    bool initFromFile(const std::string& path);
    bool ready() const { return ready_; }
    void shutdown();

    // Rasterise `utf8` at `px` pixels and return its GL texture (cached).
    TextTex get(const std::string& utf8, int px, Weight w = Weight::Regular);
    // Width in pixels without touching GL. `outH` receives the line height.
    int measure(const std::string& utf8, int px, Weight w = Weight::Regular,
                int* outH = nullptr);
    int lineHeight(int px);
    const std::string& family() const { return family_; }

private:
    struct Key {
        std::string text;
        int px = 0;
        int weight = 0;
        bool operator==(const Key& o) const {
            return px == o.px && weight == o.weight && text == o.text;
        }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const {
            return std::hash<std::string>()(k.text) ^ (size_t(k.px) * 2654435761u) ^
                   (size_t(k.weight) << 7);
        }
    };
    struct Entry {
        TextTex tex;
        Key key;
    };

    void* faceFor(int px, Weight w);          // FT_Face (size already set)
    void layout(const std::string& utf8, void* face, int* outW, int* outH);
    void rasterise(const std::string& utf8, void* face, TextTex* out, int width, int height);
    void evictLocked();

    void* lib_ = nullptr;
    void* faces_[3] = {nullptr, nullptr, nullptr};  // Regular, Medium, Bold
    int facePx_[3] = {0, 0, 0};
    // Optical size axis of a variable font (MuternVF); absent for static faces.
    bool hasOpsz_[3] = {false, false, false};
    double opszMin_[3] = {0.0, 0.0, 0.0};
    double opszMax_[3] = {0.0, 0.0, 0.0};
    std::string family_;
    bool bundled_ = false;
    // A static bundled face has no real Bold; the Bold slot is thickened with
    // FT_GlyphSlot_Embolden so headings keep their emphasis in the one family.
    bool synthBold_ = false;
    bool ready_ = false;
    bool attempted_ = false;
    std::unordered_map<Key, Entry, KeyHash> cache_;
    std::deque<Key> order_;
};

// UTF-8 helper: number of bytes in the codepoint starting at `i`.
int utf8Len(unsigned char lead);

}  // namespace wm
