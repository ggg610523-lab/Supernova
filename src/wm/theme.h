// Windows 11 ("Fluent", dark) design tokens: colours, metrics and motion
// durations. Everything the shell paints is derived from this one file, so the
// whole look can be re-skinned by editing a single place.
//
// Colours are float RGBA in linear-ish sRGB 0..1 space; the compositor emits
// premultiplied alpha, so an `a` of 0.06 really does mean "6% white wash".
#pragma once

#include <cstdint>

namespace wm {

struct Color {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
};

// 0xRRGGBB (+ optional alpha).
inline constexpr Color rgb(unsigned hex, float alpha = 1.0f) {
    return Color{float((hex >> 16) & 0xFFu) / 255.0f,
                 float((hex >> 8) & 0xFFu) / 255.0f,
                 float(hex & 0xFFu) / 255.0f, alpha};
}

namespace theme {

// --- system accent (Windows 11 default blue) --------------------------------
inline constexpr Color kAccent = rgb(0x4CC2FF);       // "spotlight" hover accent
inline constexpr Color kAccentDeep = rgb(0x0078D4);   // selection / fill accent

// --- window frame ----------------------------------------------------------
inline constexpr Color kCaption = rgb(0x202020);        // Mica-tinted, focused
inline constexpr Color kCaptionIdle = rgb(0x2B2B2B);    // unfocused
inline constexpr Color kBodyFallback = rgb(0x202020);   // before the 1st frame
inline constexpr Color kBorderFocus = rgb(0xFFFFFF, 0.14f);
inline constexpr Color kBorderIdle = rgb(0xFFFFFF, 0.08f);
inline constexpr Color kCaptionLine = rgb(0xFFFFFF, 0.06f);

// --- caption buttons (Windows 11 is 45x32, flush to the corner) -------------
inline constexpr Color kBtnHover = rgb(0xFFFFFF, 0.06f);
inline constexpr Color kBtnPress = rgb(0xFFFFFF, 0.10f);
inline constexpr Color kCloseHover = rgb(0xC42B1C);
inline constexpr Color kClosePress = rgb(0xB22A1B);
inline constexpr Color kGlyph = rgb(0xFFFFFF, 0.95f);
inline constexpr Color kGlyphIdle = rgb(0xFFFFFF, 0.60f);

// --- text -----------------------------------------------------------------
inline constexpr Color kText = rgb(0xFFFFFF);
inline constexpr Color kTextIdle = rgb(0xFFFFFF, 0.62f);
inline constexpr Color kTextMuted = rgb(0xFFFFFF, 0.55f);
inline constexpr Color kTextDim = rgb(0xFFFFFF, 0.38f);

// --- shadows ---------------------------------------------------------------
inline constexpr Color kShadow = rgb(0x000000, 0.50f);
inline constexpr Color kShadowIdle = rgb(0x000000, 0.32f);

// --- shell (taskbar / Start / flyouts) -------------------------------------
inline constexpr Color kTaskbarTint = rgb(0x1F1F1F, 0.78f);
inline constexpr Color kShellTint = rgb(0x2B2B2B, 0.90f);
inline constexpr Color kFlyoutTint = rgb(0x2B2B2B, 0.94f);
inline constexpr Color kShellBorder = rgb(0xFFFFFF, 0.09f);
inline constexpr Color kShellLine = rgb(0xFFFFFF, 0.06f);
inline constexpr Color kItemHover = rgb(0xFFFFFF, 0.06f);
inline constexpr Color kItemActive = rgb(0xFFFFFF, 0.10f);
inline constexpr Color kItemPress = rgb(0xFFFFFF, 0.04f);
inline constexpr Color kSearchBox = rgb(0xFFFFFF, 0.06f);

// --- snap / drag previews --------------------------------------------------
inline constexpr Color kSnapFill = rgb(0x4CC2FF, 0.22f);
inline constexpr Color kSnapBorder = rgb(0x9AD8FF, 0.80f);
inline constexpr Color kAccentRing = rgb(0x4CC2FF, 0.90f);

// --- task view / alt-tab cards --------------------------------------------
inline constexpr Color kCardTint = rgb(0x2B2B2B, 0.85f);
inline constexpr Color kDesktopBlur = rgb(0x000000, 0.45f);

// --- start menu letter tiles ----------------------------------------------
inline constexpr Color kTileTints[6] = {
    rgb(0x3A7BD5), rgb(0x7A5AF8), rgb(0xD5523A),
    rgb(0x2FA37C), rgb(0xC99A1F), rgb(0x3E8FB0),
};

}  // namespace theme

namespace metrics {

constexpr int kCaptionH = 32;      // Windows 11 caption height at 100%
constexpr int kBtnW = 46;          // 45 + 1 rounding, flush to the corner
constexpr int kBorder = 1;         // visible hairline frame
constexpr int kRadius = 8;         // window corner radius
constexpr int kResizeEdge = 8;     // invisible resize band outside the frame
constexpr int kShadowPad = 30;     // quad margin the drop shadow spreads into
constexpr int kTaskbarH = 48;
constexpr int kTaskIconW = 44;
constexpr int kTaskIconH = 40;
constexpr int kSnapGap = 4;        // gap Win11 leaves around snapped halves
constexpr int kSnapZonePx = 24;    // distance from an edge that arms a snap
constexpr int kAnimMs = 150;       // open/close/minimise duration
constexpr int kZoomMs = 170;       // maximise / restore duration
constexpr int kMinW = 160;
constexpr int kMinH = 90;
constexpr int kStartW = 620;
constexpr int kStartH = 620;
constexpr int kFlyoutRadius = 8;

}  // namespace metrics

}  // namespace wm
