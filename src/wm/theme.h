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

// Blend two colours component-wise; t=0 yields `a`, t=1 yields `b`. Used to
// cross-fade hover highlights, labels and status colours as they animate.
inline constexpr Color mixColor(const Color& a, const Color& b, float t) {
    return Color{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                 a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

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

// --- Control Centre (iOS) -------------------------------------------------
// A deliberately different skin from the Fluent flyouts above: Control Centre is
// a grid of dark glass tiles on a heavily blurred backdrop, and the "on" colour
// of a control is the system fill (iOS blue), not the Windows accent.
inline constexpr Color kCcBackdrop = rgb(0x000000, 0.38f);
inline constexpr Color kCcTile = rgb(0x1C1C1E, 0.72f);
inline constexpr Color kCcTileHover = rgb(0x2C2C2E, 0.82f);
inline constexpr Color kCcTileBorder = rgb(0xFFFFFF, 0.08f);
inline constexpr Color kCcActive = rgb(0x0A84FF);   // iOS systemBlue
inline constexpr Color kCcActiveGlyph = rgb(0xFFFFFF);
inline constexpr Color kCcGlyph = rgb(0xFFFFFF, 0.92f);
inline constexpr Color kCcGlyphOff = rgb(0xFFFFFF, 0.42f);  // disabled control
inline constexpr Color kCcDisabled = rgb(0xFFFFFF, 0.10f);
inline constexpr Color kCcControlOff = rgb(0xFFFFFF, 0.22f);    // circular control, off
inline constexpr Color kCcControlHover = rgb(0xFFFFFF, 0.30f);  // ... and hovered
inline constexpr Color kCcSliderTrack = rgb(0xFFFFFF, 0.16f);
inline constexpr Color kCcSliderFill = rgb(0xFFFFFF, 0.92f);
inline constexpr Color kCcLabel = rgb(0xFFFFFF, 0.70f);

// --- Launchpad (macOS) ----------------------------------------------------
// A full-screen blurred wallpaper with large squircles, a top search pill and
// page dots. Deliberately its own skin: white labels on the photo, no Fluent
// panel chrome.
inline constexpr Color kLaunchTint = rgb(0x14141A, 1.0f);      // acrylic tint over the blur
inline constexpr Color kLaunchDim = rgb(0x000000, 0.16f);      // extra darkening wash
inline constexpr Color kLaunchSearch = rgb(0xFFFFFF, 0.16f);   // search field pill
inline constexpr Color kLaunchSearchText = rgb(0xFFFFFF, 0.60f);
inline constexpr Color kLaunchLabel = rgb(0xFFFFFF, 0.95f);
inline constexpr Color kLaunchLabelShadow = rgb(0x000000, 0.60f);
inline constexpr Color kLaunchHover = rgb(0xFFFFFF, 0.10f);
inline constexpr Color kLaunchDot = rgb(0xFFFFFF, 0.32f);
inline constexpr Color kLaunchDotActive = rgb(0xFFFFFF, 0.95f);

// --- tablet / mobile mode (iOS home screen) -------------------------------
// A third skin: the iPhone/iPad home screen. White labels and a status bar sit
// straight on the (blurred) wallpaper, the dock is a heavily translucent glass
// pill, and the only chrome is the black home indicator bar.
inline constexpr Color kTabletStatusText = rgb(0xFFFFFF);
inline constexpr Color kTabletStatusSub = rgb(0xFFFFFF, 0.78f);
inline constexpr Color kTabletLabel = rgb(0xFFFFFF);
inline constexpr Color kTabletLabelShadow = rgb(0x000000, 0.55f);
inline constexpr Color kTabletIconHover = rgb(0xFFFFFF, 0.14f);
inline constexpr Color kTabletDockGlass = rgb(0x16161C, 0.30f);   // acrylic tint over the blur
inline constexpr Color kTabletDockBorder = rgb(0xFFFFFF, 0.22f);
inline constexpr Color kTabletDockHover = rgb(0xFFFFFF, 0.12f);
inline constexpr Color kTabletHomeBar = rgb(0xFFFFFF, 0.92f);
inline constexpr Color kTabletHomeBarShadow = rgb(0x000000, 0.28f);
inline constexpr Color kTabletEditPill = rgb(0x14141A, 0.55f);
inline constexpr Color kTabletEditPillBorder = rgb(0xFFFFFF, 0.22f);
inline constexpr Color kTabletEditPillHover = rgb(0xFFFFFF, 0.14f);
inline constexpr Color kTabletSplash = rgb(0x0A0A0F, 0.96f);
inline constexpr Color kTabletSplashLabel = rgb(0xFFFFFF);
inline constexpr Color kTabletSplashSub = rgb(0xFFFFFF, 0.55f);
inline constexpr Color kTabletSplashTrack = rgb(0xFFFFFF, 0.16f);
inline constexpr Color kTabletSplashFill = rgb(0xFFFFFF, 0.94f);

// --- desktop widgets (iOS 26 "Liquid Glass") ------------------------------
// Squircles of dark glass sitting directly on the wallpaper. The clock dial and
// the battery ring stay white/orange/status-tinted, the way iOS tints widget
// content against a translucent card.
inline constexpr Color kWidgetGlass = rgb(0x141418, 0.44f);   // acrylic tint over the blur
inline constexpr Color kWidgetBorder = rgb(0xFFFFFF, 0.16f);  // glass hairline
inline constexpr Color kWidgetHover = rgb(0xFFFFFF, 0.10f);
inline constexpr Color kWidgetLabel = rgb(0xFFFFFF, 0.88f);
inline constexpr Color kWidgetSub = rgb(0xFFFFFF, 0.55f);
inline constexpr Color kWidgetTick = rgb(0xFFFFFF, 0.90f);
inline constexpr Color kWidgetTickMinor = rgb(0xFFFFFF, 0.34f);
inline constexpr Color kWidgetHand = rgb(0xFFFFFF, 1.0f);
inline constexpr Color kWidgetSecond = rgb(0xFF9F0A);         // iOS systemOrange

inline constexpr Color kWidgetPin = rgb(0xFFFFFF, 0.95f);
inline constexpr Color kWidgetRingTrack = rgb(0xFFFFFF, 0.16f);
inline constexpr Color kWidgetGrip = rgb(0xFFFFFF, 0.28f);
inline constexpr Color kWidgetGreen = rgb(0x30D158);          // iOS systemGreen
inline constexpr Color kWidgetYellow = rgb(0xFFD60A);         // iOS systemYellow
inline constexpr Color kWidgetRed = rgb(0xFF453A);            // iOS systemRed

}  // namespace theme

namespace metrics {

constexpr int kCaptionH = 32;      // Windows 11 caption height at 100%
constexpr int kBtnW = 46;          // 45 + 1 rounding, flush to the corner
constexpr int kBorder = 1;         // visible hairline frame
constexpr int kRadius = 8;         // window corner radius
constexpr int kResizeEdge = 8;     // invisible resize band outside the frame
constexpr int kShadowPad = 30;     // quad margin the drop shadow spreads into
// The taskbar's thickness is the one piece of shell geometry the user sets. As in
// Windows 10 you grab the bar's top edge and drag it, and it stays where you left
// it; Windows 11 dropped that and offers only a small/default toggle. 48 is the
// Windows 11 height at 100%, and the baseline the derived sizes below are written
// against, so at kTaskbarDefaultH they come out at exactly 44/40/32.
constexpr int kTaskbarDefaultH = 48;
// 32 is Windows' own small taskbar height, and the floor here too: the two-line
// clock alone is 29px, so a thinner bar could not hold it.
constexpr int kTaskbarMinH = 32;
constexpr int kTaskbarMaxH = 96;
extern int taskbarH;               // live thickness; saved by saveTaskbarHeight

// Buttons and icons are sized off the thickness, so a thicker bar grows its
// contents instead of stranding a 32px icon in the middle of it. One icon box
// serves every button, whatever the button is: a pinned launcher and a running
// window have to read as the same size of app.
inline int taskButtonW() { return 44 * taskbarH / kTaskbarDefaultH; }
inline int taskButtonH() { return 40 * taskbarH / kTaskbarDefaultH; }
inline int taskIconSize() { return 32 * taskbarH / kTaskbarDefaultH; }
inline int taskIconDragSize() { return 34 * taskbarH / kTaskbarDefaultH; }

constexpr int kPinDragSlop = 6;    // travel that turns a press into a drag

// The strip of bar above the buttons, which is empty because they are centred in
// it. It doubles as the grab area inside the bar, and it shrinks with the bar, so
// the grip can never overlap a button however thin the bar gets.
inline int taskbarInnerGrip() { return (taskbarH - taskButtonH()) / 2; }

// Desktop pixels above the bar that also belong to its edge. Maximised windows stop
// at the top of the bar, so this band is empty desktop and grabbing it cannot
// steal a press meant for a window.
constexpr int kTaskbarOuterGrip = 4;
constexpr int kSnapGap = 4;        // gap Win11 leaves around snapped halves
constexpr int kSnapZonePx = 24;    // distance from an edge that arms a snap
constexpr int kAnimMs = 150;       // open/close/minimise duration
constexpr int kZoomMs = 170;       // maximise / restore duration
constexpr int kMinimizeMs = 240;   // magic-lamp minimise / restore duration
constexpr int kMinW = 160;
constexpr int kMinH = 90;
constexpr int kStartW = 620;
constexpr int kStartH = 620;
constexpr int kFlyoutRadius = 8;

// --- Control Centre (iOS 18: 4 columns x 6 rows) --------------------------
// The reference layout is a 4x5 grid of circular spots with 15pt gutters, 18pt
// tile corners and 15pt tile padding; the inner buttons are 54pt circles. We
// spend one extra row on the Tablet-mode toggle, which is this shell's own
// control rather than one iOS ships.
constexpr int kCcCols = 4;
constexpr int kCcRows = 6;
constexpr int kCcCell = 76;       // one grid cell
constexpr int kCcGap = 15;        // grid gap
constexpr int kCcPad = 16;        // panel padding
constexpr int kCcTileRadius = 18;
constexpr int kCcButton = 64;     // circular inner button diameter
constexpr int kCcSliderRadius = 22;  // brightness / volume pill corners
constexpr int kCcPanelRadius = 22;
constexpr int kCcMaxScale = 100;  // percent; shrinks to fit small screens

// --- Launchpad (macOS) ----------------------------------------------------
constexpr int kLaunchIcon = 64;      // app icon edge at 100%
constexpr int kLaunchIconMin = 46;   // floor when the screen is small
constexpr int kLaunchRowGap = 20;    // vertical gap between icon rows
constexpr int kLaunchLabelH = 18;    // label strip under each icon
constexpr int kLaunchDotsH = 34;     // page-dot strip at the bottom

// --- tablet / mobile mode (iOS home screen) -------------------------------
// The status bar is 44pt on an iPhone X (52 here to leave room for the Dynamic
// Island), the home indicator is the 134x5pt gesture bar, and the dock is a
// full-width glass pill holding up to five squircles.
constexpr int kTabletStatusH = 52;
constexpr int kTabletIcon = 68;         // app icon edge at 100%
constexpr int kTabletIconMin = 48;
constexpr int kTabletCellW = 106;       // icon + label cell
constexpr int kTabletCellH = 108;
constexpr int kTabletDockIcon = 62;
constexpr int kTabletHomeBarW = 144;    // the iPhone X home indicator
constexpr int kTabletHomeBarH = 5;
// The band along the bottom edge kept clear of app windows, so a tap on the
// home indicator reaches the shell instead of the app covering the screen.
constexpr int kTabletHomeBarZone = 30;
constexpr int kTabletSplashMs = 1100;   // full splash transition
// The corner factor of a home-screen squircle, shared by the drawn icons and
// the iOS 26 zoom transition so a window's corners match the icon it grows from.
constexpr float kTabletIconRadius = 0.24f;
// Tablet-only app motion (iOS 26): the window zooms out of its home-screen icon
// when it opens, and collapses back into it when it closes or is sent home.
// Slightly slower than the Fluent pop because more distance is being covered.
constexpr int kTabletOpenMs = 340;   // icon -> full app frame
constexpr int kTabletCloseMs = 300;  // app frame -> icon, on close
constexpr int kTabletMinMs = 300;    // app frame -> icon, on swipe-up home

// --- desktop widgets (iOS 26) ---------------------------------------------
constexpr int kWidgetRadius = 34;      // squircle corner at the small size
constexpr int kWidgetMin = 110;        // floor when the user shrinks one
constexpr int kWidgetGrip = 22;        // bottom-right resize handle (hit box)
constexpr int kWidgetClock = 168;      // small square widget edge
constexpr int kWidgetBatteryW = 344;   // medium widget
constexpr int kWidgetBatteryH = 168;
constexpr int kWidgetPad = 16;         // widget origin inset

}  // namespace metrics

}  // namespace wm
