// HarmonyOS-inspired design tokens: colours, metrics and motion
// durations. Everything the shell paints is derived from this one file, so the
// whole look can be re-skinned by editing a single place.
//
// Colours are float RGBA in linear-ish sRGB 0..1 space; the compositor emits
// premultiplied alpha, so an `a` of 0.06 really does mean "6% white wash".
//
// The shell palette ships in two modes and applyMode() swaps between them; the
// skins further down (Launchpad, the tablet home screen, Control Centre, the
// desktop widgets) remain specialised and are mode-independent.
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

// --- system accent (HarmonyOS default blue) --------------------------------
// The default shell colour is tuned for HarmonyOS: a vivid blue with a cool,
// elegant dark navy base, rather than the Windows 11 default cyan/fluor palette.
inline Color kAccent = rgb(0x0A7DFF);
inline Color kAccentDeep = rgb(0x0057D9);

// --- window frame ----------------------------------------------------------
inline Color kCaption = rgb(0x121827);        // dark HarmonyOS shell base
inline Color kCaptionIdle = rgb(0x1A2235);    // unfocused
inline Color kBodyFallback = rgb(0x121827);   // before the 1st frame
inline Color kBorderFocus = rgb(0xFFFFFF, 0.12f);
inline Color kBorderIdle = rgb(0xFFFFFF, 0.08f);
inline Color kCaptionLine = rgb(0xFFFFFF, 0.05f);

// --- caption buttons -------------------------------------------------------
inline Color kBtnHover = rgb(0xFFFFFF, 0.06f);
inline Color kBtnPress = rgb(0xFFFFFF, 0.10f);
inline constexpr Color kCloseHover = rgb(0xFF5A5F);
inline constexpr Color kClosePress = rgb(0xE94D53);
inline Color kGlyph = rgb(0xFFFFFF, 0.95f);
inline Color kGlyphIdle = rgb(0xFFFFFF, 0.60f);

// --- text -----------------------------------------------------------------
inline Color kText = rgb(0xF5F7FF);
inline Color kTextIdle = rgb(0xFFFFFF, 0.62f);
inline Color kTextMuted = rgb(0xFFFFFF, 0.55f);
inline Color kTextDim = rgb(0xFFFFFF, 0.38f);

// --- shadows ---------------------------------------------------------------
inline Color kShadow = rgb(0x000000, 0.52f);
inline Color kShadowIdle = rgb(0x000000, 0.34f);

// --- shell (taskbar / Start / flyouts) -------------------------------------
inline Color kTaskbarTint = rgb(0x111827);
inline float kTaskbarTintOpacity = 0.80f;
inline float kTaskbarSaturate = 2.2f;
inline Color kShellTint = rgb(0x1A2235, 0.92f);
inline Color kFlyoutTint = rgb(0x1A2235, 0.94f);
inline Color kShellBorder = rgb(0xFFFFFF, 0.08f);
inline Color kShellLine = rgb(0xFFFFFF, 0.06f);
inline Color kItemHover = rgb(0xFFFFFF, 0.06f);
inline Color kItemActive = rgb(0xFFFFFF, 0.10f);
inline Color kItemPress = rgb(0xFFFFFF, 0.04f);
inline Color kSearchBox = rgb(0xFFFFFF, 0.06f);
inline Color kTaskActivePlate = rgb(0xFFFFFF, 0.10f);
inline Color kTaskGlow = rgb(0x0A7DFF, 1.0f);
inline Color kCircleRing = rgb(0xFFFFFF, 0.90f);
inline Color kScrim = rgb(0x0A0D18, 0.88f);
inline Color kFieldFill = rgb(0x141C2C, 0.74f);

// --- snap / drag previews --------------------------------------------------
inline Color kSnapFill = rgb(0x0A7DFF, 0.20f);
inline Color kSnapBorder = rgb(0x79B9FF, 0.82f);
inline Color kAccentRing = rgb(0x0A7DFF, 0.88f);

// --- task view / alt-tab cards --------------------------------------------
inline Color kCardTint = rgb(0x1A2235, 0.86f);
inline Color kDesktopBlur = rgb(0x000000, 0.40f);

// Which palette the Fluent tokens above currently hold.
enum class Mode { Dark, Light };

inline Mode mode = Mode::Dark;

inline bool isLight() { return mode == Mode::Light; }

// Re-points every shell token in one step. This keeps the default HarmonyOS dark
// palette as the starting point, while the light mode remains a cool, airy
// variant rather than a Windows-style inversion.
inline void applyMode(Mode m) {
    mode = m;
    if (m == Mode::Light) {
        kAccent = rgb(0x0A7DFF);
        kAccentDeep = rgb(0x0057D9);
        kCaption = rgb(0xF5F7FB);
        kCaptionIdle = rgb(0xFFFFFF);
        kBodyFallback = rgb(0xFFFFFF);
        kBorderFocus = rgb(0x000000, 0.06f);
        kBorderIdle = rgb(0x000000, 0.04f);
        kCaptionLine = rgb(0x000000, 0.05f);
        kBtnHover = rgb(0x000000, 0.04f);
        kBtnPress = rgb(0x000000, 0.06f);
        kGlyph = rgb(0x111827, 0.90f);
        kGlyphIdle = rgb(0x111827, 0.60f);
        kText = rgb(0x111827);
        kTextIdle = rgb(0x111827, 0.62f);
        kTextMuted = rgb(0x111827, 0.55f);
        kTextDim = rgb(0x111827, 0.38f);
        kShadow = rgb(0x000000, 0.22f);
        kShadowIdle = rgb(0x000000, 0.12f);
        kTaskbarTint = rgb(0xF0F4FA);
        kTaskbarTintOpacity = 0.90f;
        kTaskbarSaturate = 1.8f;
        kShellTint = rgb(0xFFFFFF, 0.90f);
        kFlyoutTint = rgb(0xFFFFFF, 0.94f);
        kShellBorder = rgb(0x000000, 0.06f);
        kShellLine = rgb(0x000000, 0.05f);
        kItemHover = rgb(0x000000, 0.04f);
        kItemActive = rgb(0x000000, 0.06f);
        kItemPress = rgb(0x000000, 0.02f);
        kSearchBox = rgb(0x000000, 0.04f);
        kTaskActivePlate = rgb(0x000000, 0.05f);
        kTaskGlow = rgb(0x0A7DFF, 1.0f);
        kCircleRing = rgb(0x111827, 0.82f);
        kScrim = rgb(0xF2F5FA, 0.90f);
        kFieldFill = rgb(0xEEF3FB, 0.90f);
        kSnapFill = rgb(0x0A7DFF, 0.14f);
        kSnapBorder = rgb(0x0057D9, 0.70f);
        kAccentRing = rgb(0x0A7DFF, 0.88f);
        kCardTint = rgb(0xFFFFFF, 0.86f);
        kDesktopBlur = rgb(0xFFFFFF, 0.38f);
        return;
    }
    kAccent = rgb(0x0A7DFF);
    kAccentDeep = rgb(0x0057D9);
    kCaption = rgb(0x121827);
    kCaptionIdle = rgb(0x1A2235);
    kBodyFallback = rgb(0x121827);
    kBorderFocus = rgb(0xFFFFFF, 0.12f);
    kBorderIdle = rgb(0xFFFFFF, 0.08f);
    kCaptionLine = rgb(0xFFFFFF, 0.05f);
    kBtnHover = rgb(0xFFFFFF, 0.06f);
    kBtnPress = rgb(0xFFFFFF, 0.10f);
    kGlyph = rgb(0xFFFFFF, 0.95f);
    kGlyphIdle = rgb(0xFFFFFF, 0.60f);
    kText = rgb(0xF5F7FF);
    kTextIdle = rgb(0xFFFFFF, 0.62f);
    kTextMuted = rgb(0xFFFFFF, 0.55f);
    kTextDim = rgb(0xFFFFFF, 0.38f);
    kShadow = rgb(0x000000, 0.52f);
    kShadowIdle = rgb(0x000000, 0.34f);
    kTaskbarTint = rgb(0x111827);
    kTaskbarTintOpacity = 0.80f;
    kTaskbarSaturate = 2.2f;
    kShellTint = rgb(0x1A2235, 0.92f);
    kFlyoutTint = rgb(0x1A2235, 0.94f);
    kShellBorder = rgb(0xFFFFFF, 0.08f);
    kShellLine = rgb(0xFFFFFF, 0.06f);
    kItemHover = rgb(0xFFFFFF, 0.06f);
    kItemActive = rgb(0xFFFFFF, 0.10f);
    kItemPress = rgb(0xFFFFFF, 0.04f);
    kSearchBox = rgb(0xFFFFFF, 0.06f);
    kTaskActivePlate = rgb(0xFFFFFF, 0.10f);
    kTaskGlow = rgb(0x0A7DFF, 1.0f);
    kCircleRing = rgb(0xFFFFFF, 0.90f);
    kScrim = rgb(0x0A0D18, 0.88f);
    kFieldFill = rgb(0x141C2C, 0.74f);
    kSnapFill = rgb(0x0A7DFF, 0.20f);
    kSnapBorder = rgb(0x79B9FF, 0.82f);
    kAccentRing = rgb(0x0A7DFF, 0.88f);
    kCardTint = rgb(0x1A2235, 0.86f);
    kDesktopBlur = rgb(0x000000, 0.40f);
}

// --- start menu letter tiles ----------------------------------------------
inline constexpr Color kTileTints[6] = {
    rgb(0x3A82F7), rgb(0x5E7CFF), rgb(0x1FB4A6),
    rgb(0x3DA5F5), rgb(0x6D6CF9), rgb(0x32B6E8),
};

// --- Control Centre (HarmonyOS-style quick settings) -----------------------
inline constexpr Color kCcBackdrop = rgb(0x000000, 0.38f);
inline constexpr Color kCcTile = rgb(0x1C2438, 0.72f);
inline constexpr Color kCcTileHover = rgb(0x2A3550, 0.82f);
inline constexpr Color kCcActive = rgb(0x0A7DFF);
inline constexpr Color kCcActiveGlyph = rgb(0xFFFFFF);
inline constexpr Color kCcGlyph = rgb(0xFFFFFF, 0.92f);
inline constexpr Color kCcGlyphOff = rgb(0xFFFFFF, 0.42f);
inline constexpr Color kCcDisabled = rgb(0xFFFFFF, 0.10f);
inline constexpr Color kCcControlOff = rgb(0xFFFFFF, 0.22f);
inline constexpr Color kCcControlHover = rgb(0xFFFFFF, 0.30f);
inline constexpr Color kCcSliderTrack = rgb(0xFFFFFF, 0.16f);
inline constexpr Color kCcSliderFill = rgb(0xFFFFFF, 0.92f);
inline constexpr Color kCcLabel = rgb(0xFFFFFF, 0.70f);

// --- Launchpad (macOS) ----------------------------------------------------
inline constexpr Color kLaunchTint = rgb(0x14141A, 1.0f);
inline constexpr Color kLaunchSearch = rgb(0xFFFFFF, 0.10f);
inline constexpr Color kLaunchSearchBorder = rgb(0xFFFFFF, 0.28f);
inline constexpr Color kLaunchSearchText = rgb(0xFFFFFF, 0.60f);
inline constexpr Color kLaunchLabel = rgb(0xFFFFFF, 0.95f);
inline constexpr Color kLaunchLabelShadow = rgb(0x000000, 0.60f);
inline constexpr Color kLaunchHover = rgb(0xFFFFFF, 0.10f);
inline constexpr Color kLaunchDot = rgb(0xFFFFFF, 0.32f);
inline constexpr Color kLaunchDotActive = rgb(0xFFFFFF, 0.95f);

// --- tablet / mobile mode (iOS home screen) -------------------------------
inline constexpr Color kTabletStatusText = rgb(0xFFFFFF);
inline constexpr Color kTabletStatusSub = rgb(0xFFFFFF, 0.78f);
inline constexpr Color kTabletLabel = rgb(0xFFFFFF);
inline constexpr Color kTabletLabelShadow = rgb(0x000000, 0.55f);
inline constexpr Color kTabletIconHover = rgb(0xFFFFFF, 0.14f);
inline constexpr Color kTabletDockGlass = rgb(0x16161C, 0.30f);
inline constexpr Color kTabletDockBorder = rgb(0xFFFFFF, 0.22f);
inline constexpr Color kTabletDockHover = rgb(0xFFFFFF, 0.12f);
inline constexpr Color kTabletHomeBar = rgb(0xFFFFFF, 0.92f);
inline constexpr Color kTabletHomeBarShadow = rgb(0x000000, 0.28f);
inline constexpr Color kTabletFolderBack = rgb(0xFFFFFF, 0.16f);
inline constexpr Color kTabletFolderHover = rgb(0xFFFFFF, 0.30f);
inline constexpr Color kTabletFolderSheet = rgb(0x1C1C22, 0.82f);
inline constexpr Color kTabletRunDot = rgb(0xFFFFFF, 0.92f);
inline constexpr Color kTabletMenuDanger = rgb(0xFF6B6B);
inline constexpr Color kTabletBadgeFill = rgb(0x1B1B22, 0.94f);
inline constexpr Color kTabletBadgeTile = rgb(0xFFFFFF, 0.10f);
inline constexpr Color kTabletBadgeGlyph = rgb(0xFFFFFF, 0.96f);
inline constexpr Color kTabletAccent = rgb(0x0A7DFF);
inline constexpr Color kTabletPanelBorder = rgb(0xFFFFFF, 0.20f);
inline constexpr Color kTabletSplash = rgb(0x0A0A0F, 0.96f);
inline constexpr Color kTabletSplashLabel = rgb(0xFFFFFF);
inline constexpr Color kTabletSplashSub = rgb(0xFFFFFF, 0.55f);
inline constexpr Color kTabletSplashTrack = rgb(0xFFFFFF, 0.16f);
inline constexpr Color kTabletSplashFill = rgb(0xFFFFFF, 0.94f);

// --- desktop widgets (iOS 18 full-colour) ---------------------------------
inline constexpr Color kWidgetClockTop = rgb(0x1B1B1D);
inline constexpr Color kWidgetClockBottom = rgb(0x000000);
inline constexpr Color kWidgetBatteryGoodTop = rgb(0x22B573);
inline constexpr Color kWidgetBatteryGoodBottom = rgb(0x0E8A63);
inline constexpr Color kWidgetBatteryWarnTop = rgb(0xF0A21C);
inline constexpr Color kWidgetBatteryWarnBottom = rgb(0xC0700A);
inline constexpr Color kWidgetBatteryLowTop = rgb(0xEF4D4D);
inline constexpr Color kWidgetBatteryLowBottom = rgb(0xB0242A);
inline constexpr Color kWidgetBatteryNoneTop = rgb(0x6C7480);
inline constexpr Color kWidgetBatteryNoneBottom = rgb(0x424955);
inline constexpr Color kWidgetHover = rgb(0xFFFFFF, 0.12f);
inline constexpr Color kWidgetLabel = rgb(0xFFFFFF, 0.98f);
inline constexpr Color kWidgetSub = rgb(0xFFFFFF, 0.72f);
inline constexpr Color kWidgetTick = rgb(0xFFFFFF, 0.92f);
inline constexpr Color kWidgetTickMinor = rgb(0xFFFFFF, 0.40f);
inline constexpr Color kWidgetHand = rgb(0xFFFFFF, 1.0f);
inline constexpr Color kWidgetSecond = rgb(0xFF9F0A);

// --- calendar widget (iOS 18 Calendar) ------------------------------------
inline constexpr Color kWidgetCalendarTop = rgb(0xFFFFFF);
inline constexpr Color kWidgetCalendarBottom = rgb(0xF4F4F7);
inline constexpr Color kWidgetCalText = rgb(0x1C1C1E);
inline constexpr Color kWidgetCalMuted = rgb(0x9A9AA0);
inline constexpr Color kWidgetCalWeekend = rgb(0xB0B0B8);
inline constexpr Color kWidgetCalAccent = rgb(0xFF3B30);
inline constexpr Color kWidgetCalAccentInk = rgb(0xFFFFFF);
inline constexpr Color kWidgetCalLine = rgb(0x1C1C1E, 0.10f);

// --- weather widget (iOS 18 Weather) --------------------------------------
inline constexpr Color kWidgetWeatherClearTop = rgb(0x54A9E8);
inline constexpr Color kWidgetWeatherClearBottom = rgb(0x1E6FB4);
inline constexpr Color kWidgetWeatherCloudTop = rgb(0x7E93A8);
inline constexpr Color kWidgetWeatherCloudBottom = rgb(0x4A5C70);
inline constexpr Color kWidgetWeatherRainTop = rgb(0x5A6B7E);
inline constexpr Color kWidgetWeatherRainBottom = rgb(0x333F4D);
inline constexpr Color kWidgetWeatherNightTop = rgb(0x2A3550);
inline constexpr Color kWidgetWeatherNightBottom = rgb(0x121A2E);
inline constexpr Color kWidgetWeatherInk = rgb(0xFFFFFF);
inline constexpr Color kWidgetWeatherSub = rgb(0xFFFFFF, 0.78f);
inline constexpr Color kWidgetWeatherFaint = rgb(0xFFFFFF, 0.55f);
inline constexpr Color kWidgetWeatherPane = rgb(0xFFFFFF, 0.14f);
inline constexpr Color kWidgetWeatherSun = rgb(0xFFD60A);

// --- digital clock widget -------------------------------------------------
inline constexpr Color kWidgetDigitalInk = rgb(0xFFFFFF);
inline constexpr Color kWidgetDigitalDim = rgb(0xFFFFFF, 0.55f);

inline constexpr Color kWidgetPin = rgb(0xFFFFFF, 0.95f);
inline constexpr Color kWidgetRingTrack = rgb(0xFFFFFF, 0.28f);
inline constexpr Color kWidgetGrip = rgb(0xFFFFFF, 0.28f);
inline constexpr Color kWidgetGreen = rgb(0x30D158);
inline constexpr Color kWidgetYellow = rgb(0xFFD60A);
inline constexpr Color kWidgetRed = rgb(0xFF453A);

}  // namespace theme

namespace metrics {

constexpr int kCaptionH = 32;
constexpr int kBtnW = 46;
constexpr int kBorder = 1;
constexpr int kRadius = 10;
constexpr int kResizeEdge = 8;
constexpr int kShadowPad = 30;
constexpr int kTaskbarDefaultH = 48;
constexpr int kTaskbarMinH = 32;
constexpr int kTaskbarMaxH = 96;
extern int taskbarH;

inline int taskButtonW() { return 42 * taskbarH / kTaskbarDefaultH; }
inline int taskButtonH() { return 40 * taskbarH / kTaskbarDefaultH; }
inline int taskIconSize() { return 32 * taskbarH / kTaskbarDefaultH; }
inline int taskIconDragSize() { return 34 * taskbarH / kTaskbarDefaultH; }

constexpr int kPinDragSlop = 6;
inline int taskbarInnerGrip() { return (taskbarH - taskButtonH()) / 2; }

constexpr int kTaskbarOuterGrip = 4;
constexpr int kSnapGap = 4;
constexpr int kSnapZonePx = 24;
constexpr int kAnimMs = 150;
constexpr int kZoomMs = 170;
constexpr int kMinimizeMs = 240;
constexpr int kMinW = 160;
constexpr int kMinH = 90;
constexpr int kStartW = 620;
constexpr int kStartH = 620;
constexpr int kFlyoutRadius = 14;

constexpr int kCcCols = 4;
constexpr int kCcRows = 6;
constexpr int kCcCell = 76;
constexpr int kCcGap = 15;
constexpr int kCcPad = 16;
constexpr int kCcTileRadius = 18;
constexpr int kCcButton = 64;
constexpr int kCcSliderRadius = 22;
constexpr int kCcPanelRadius = 22;
constexpr int kCcMaxScale = 100;

constexpr int kLaunchIcon = 80;
constexpr int kLaunchIconMin = 46;
constexpr int kLaunchCols = 4;
constexpr int kLaunchRowGap = 78;
constexpr int kLaunchLabelH = 18;
constexpr int kLaunchDotsH = 34;
constexpr int kLaunchSwipeSlop = 22;

constexpr int kRingHeaderH = 72;
constexpr int kRingCols = 4;
constexpr int kRingCell = 78;
constexpr int kRingIcon = 48;
constexpr int kRingPowerH = 74;
constexpr int kRingMaxRecents = 8;
constexpr int kRingPowerCount = 4;

constexpr int kTabletStatusH = 52;
constexpr int kTabletIcon = 68;
constexpr int kTabletIconMin = 48;
constexpr int kTabletCellW = 106;
constexpr int kTabletCellH = 108;
constexpr int kTabletDockIcon = 62;
constexpr int kTabletDockMax = 6;
constexpr int kTabletMenuWidthPct = 42;
constexpr int kTabletMenuMinW = 288;
constexpr int kTabletMenuMaxW = 380;
constexpr int kTabletMenuPad = 10;
constexpr int kTabletMenuHeadH = 78;
constexpr int kTabletMenuRowH = 44;
constexpr int kTabletRunDotSize = 5;
constexpr int kTabletRunDotGap = 1;
constexpr int kTabletDockFill = 4;
constexpr int kTabletDockPickerCols = 4;
constexpr int kTabletDockPickerPerPage = 12;
constexpr int kTabletSwipeSlop = 22;
constexpr int kTabletPageDotsH = 26;
constexpr int kTabletPageTurnMs = 240;
constexpr int kTabletDragEdgePx = 30;
constexpr int kTabletDragEdgeMs = 380;
constexpr int kTabletLongPressMs = 450;
constexpr int kTabletFolderPerPage = 9;
constexpr int kTabletFolderMiniMax = 72;
constexpr int kTabletHomeBarW = 144;
constexpr int kTabletHomeBarH = 5;
constexpr int kTabletHomeBarZone = 30;
constexpr int kTabletSplashMs = 1100;
constexpr float kTabletIconRadius = 0.24f;
constexpr int kTabletOpenMs = 340;
constexpr int kTabletCloseMs = 300;
constexpr int kTabletMinMs = 300;

constexpr int kWidgetRadius = 34;
constexpr int kWidgetMin = 110;
constexpr int kWidgetGrip = 22;
constexpr int kWidgetClock = 168;
constexpr int kWidgetBatteryW = 344;
constexpr int kWidgetBatteryH = 168;
constexpr int kWidgetCalendarW = 344;
constexpr int kWidgetCalendarH = 232;
constexpr int kWidgetWeatherW = 344;
constexpr int kWidgetWeatherH = 168;
constexpr int kWidgetDigitalW = 344;
constexpr int kWidgetDigitalH = 168;
constexpr int kWidgetPad = 16;

}  // namespace metrics

}  // namespace wm
