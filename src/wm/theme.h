// Windows 11 ("Fluent") design tokens: colours, metrics and motion
// durations. Everything the shell paints is derived from this one file, so the
// whole look can be re-skinned by editing a single place.
//
// Colours are float RGBA in linear-ish sRGB 0..1 space; the compositor emits
// premultiplied alpha, so an `a` of 0.06 really does mean "6% white wash".
//
// The Fluent palette ships in two modes and applyMode() swaps between them; the
// skins further down (Launchpad, the tablet home screen, Control Centre, the
// desktop widgets) are imitations of other systems and are mode-independent.
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
// The Fluent tokens below are ordinary inline variables rather than constants, so
// applyMode() can re-point the whole palette at light mode at runtime. Their
// initialisers are the *dark* palette, which is what the shell comes up in.
inline Color kAccent = rgb(0x4CC2FF);       // "spotlight" hover accent
inline Color kAccentDeep = rgb(0x0078D4);   // selection / fill accent

// --- window frame ----------------------------------------------------------
inline Color kCaption = rgb(0x202020);        // Mica-tinted, focused
inline Color kCaptionIdle = rgb(0x2B2B2B);    // unfocused
inline Color kBodyFallback = rgb(0x202020);   // before the 1st frame
inline Color kBorderFocus = rgb(0xFFFFFF, 0.14f);
inline Color kBorderIdle = rgb(0xFFFFFF, 0.08f);
inline Color kCaptionLine = rgb(0xFFFFFF, 0.06f);

// --- caption buttons (Windows 11 is 45x32, flush to the corner) -------------
inline Color kBtnHover = rgb(0xFFFFFF, 0.06f);
inline Color kBtnPress = rgb(0xFFFFFF, 0.10f);
// The close button keeps its red in both modes, so these two never move.
inline constexpr Color kCloseHover = rgb(0xC42B1C);
inline constexpr Color kClosePress = rgb(0xB22A1B);
inline Color kGlyph = rgb(0xFFFFFF, 0.95f);
inline Color kGlyphIdle = rgb(0xFFFFFF, 0.60f);

// --- text -----------------------------------------------------------------
inline Color kText = rgb(0xFFFFFF);
inline Color kTextIdle = rgb(0xFFFFFF, 0.62f);
inline Color kTextMuted = rgb(0xFFFFFF, 0.55f);
inline Color kTextDim = rgb(0xFFFFFF, 0.38f);

// --- shadows ---------------------------------------------------------------
inline Color kShadow = rgb(0x000000, 0.50f);
inline Color kShadowIdle = rgb(0x000000, 0.32f);

// --- shell (taskbar / Start / flyouts) -------------------------------------
// The taskbar background is the Windows 11 web clone's recipe verbatim: a
// translucent theme fill laid over a strongly saturated, blurred backdrop --
// `background: rgba(32,32,32,0.75); backdrop-filter: saturate(3) blur(20px)`.
// kTaskbarTint is the fill colour, kTaskbarTintOpacity its rgba alpha, and
// kTaskbarSaturate the backdrop-filter saturate() amount.
inline Color kTaskbarTint = rgb(0x202020);
inline float kTaskbarTintOpacity = 0.75f;
inline float kTaskbarSaturate = 3.0f;
inline Color kShellTint = rgb(0x2B2B2B, 0.90f);
inline Color kFlyoutTint = rgb(0x2B2B2B, 0.94f);
inline Color kShellBorder = rgb(0xFFFFFF, 0.09f);
inline Color kShellLine = rgb(0xFFFFFF, 0.06f);
inline Color kItemHover = rgb(0xFFFFFF, 0.06f);
inline Color kItemActive = rgb(0xFFFFFF, 0.10f);
inline Color kItemPress = rgb(0xFFFFFF, 0.04f);
inline Color kSearchBox = rgb(0xFFFFFF, 0.06f);
// The backplate under the focused app's taskbar button. Windows 11 lifts it a
// little above a plain hover wash and lets a trace of the accent through, which is
// what stops the active button reading as just "the one under the cursor".
inline Color kTaskActivePlate = rgb(0xFFFFFF, 0.115f);
// The accent bloom that lifts the taskbar button under the cursor -- and the
// focused app -- off the acrylic. Windows 11 keeps this neutral, but a trace of
// the accent spilling onto the bar is what stops the surface reading as flat
// paint and gives it the lit-glass look the whole shell is going for.
inline Color kTaskGlow = rgb(0x4CC2FF, 1.0f);
// The bold ring button parked at the left end of the taskbar. It has no job yet,
// so it is an empty ring rather than a glyph: a high-contrast neutral stroke, white
// on the dark bar and black on the light one, which applyMode() re-points so it
// tracks the rest of the shell. The hollow centre shows the taskbar's acrylic
// through it (draw.cpp redraws the surface inside the ring).
inline Color kCircleRing = rgb(0xFFFFFF, 0.95f);
// The wash behind a modal dialog, and the fill of a control sitting *on* a light
// flyout. These exist so the Fluent shell never has to borrow a colour from one of
// the imitated skins below: a near-black scrim over a white dialog is the one
// thing light mode cannot borrow from an iOS or macOS palette.
inline Color kScrim = rgb(0x0A0A0F, 0.96f);
inline Color kFieldFill = rgb(0x1C1C1E, 0.72f);

// --- snap / drag previews --------------------------------------------------
inline Color kSnapFill = rgb(0x4CC2FF, 0.22f);
inline Color kSnapBorder = rgb(0x9AD8FF, 0.80f);
inline Color kAccentRing = rgb(0x4CC2FF, 0.90f);

// --- task view / alt-tab cards --------------------------------------------
inline Color kCardTint = rgb(0x2B2B2B, 0.85f);
inline Color kDesktopBlur = rgb(0x000000, 0.45f);

// Which palette the Fluent tokens above currently hold.
enum class Mode { Dark, Light };

inline Mode mode = Mode::Dark;

inline bool isLight() { return mode == Mode::Light; }

// Re-points every Fluent token in one step. Windows 11's light mode is not the
// dark palette run backwards: Mica turns into a pale warm grey, the white washes
// that brighten dark surfaces become black washes that darken light ones (and at
// a much lower alpha, or every control turns into a smudge), the shadows lose
// most of their weight, and the accent darkens so it still reads as a fill
// against white.
//
// Only the Fluent tokens move. Launchpad, the tablet home screen and Control
// Centre are deliberate imitations of other systems' home screens, so they keep
// their own palettes whichever mode the Windows shell is in.
inline void applyMode(Mode m) {
    mode = m;
    if (m == Mode::Light) {
        kAccent = rgb(0x0078D4);
        kAccentDeep = rgb(0x005FB8);
        kCaption = rgb(0xF3F3F3);
        kCaptionIdle = rgb(0xFAFAFA);
        kBodyFallback = rgb(0xFFFFFF);
        kBorderFocus = rgb(0x000000, 0.0578f);
        kBorderIdle = rgb(0x000000, 0.0338f);
        kCaptionLine = rgb(0x000000, 0.0469f);
        kBtnHover = rgb(0x000000, 0.0373f);
        kBtnPress = rgb(0x000000, 0.0241f);
        kGlyph = rgb(0x1A1A1A, 0.95f);
        kGlyphIdle = rgb(0x1A1A1A, 0.60f);
        kText = rgb(0x1A1A1A);
        kTextIdle = rgb(0x000000, 0.62f);
        kTextMuted = rgb(0x000000, 0.55f);
        kTextDim = rgb(0x000000, 0.38f);
        kShadow = rgb(0x000000, 0.22f);
        kShadowIdle = rgb(0x000000, 0.13f);
        kTaskbarTint = rgb(0xF3F3F3);
        kTaskbarTintOpacity = 0.85f;
        kTaskbarSaturate = 3.0f;
        kShellTint = rgb(0xF3F3F3, 0.90f);
        kFlyoutTint = rgb(0xFFFFFF, 0.94f);
        kShellBorder = rgb(0x000000, 0.0578f);
        kShellLine = rgb(0x000000, 0.0469f);
        kItemHover = rgb(0x000000, 0.0373f);
        kItemActive = rgb(0x000000, 0.0595f);
        kItemPress = rgb(0x000000, 0.0235f);
        kSearchBox = rgb(0x000000, 0.0373f);
        kTaskActivePlate = rgb(0x000000, 0.0536f);
        kTaskGlow = rgb(0x0078D4, 1.0f);
        kCircleRing = rgb(0x000000, 0.90f);
        kScrim = rgb(0xF5F5F5, 0.96f);
        kFieldFill = rgb(0xEFEFEF, 0.92f);
        kSnapFill = rgb(0x0078D4, 0.16f);
        kSnapBorder = rgb(0x005FB8, 0.70f);
        kAccentRing = rgb(0x0078D4, 0.90f);
        kCardTint = rgb(0xFFFFFF, 0.85f);
        kDesktopBlur = rgb(0xFFFFFF, 0.40f);
        return;
    }
    kAccent = rgb(0x4CC2FF);
    kAccentDeep = rgb(0x0078D4);
    kCaption = rgb(0x202020);
    kCaptionIdle = rgb(0x2B2B2B);
    kBodyFallback = rgb(0x202020);
    kBorderFocus = rgb(0xFFFFFF, 0.14f);
    kBorderIdle = rgb(0xFFFFFF, 0.08f);
    kCaptionLine = rgb(0xFFFFFF, 0.06f);
    kBtnHover = rgb(0xFFFFFF, 0.06f);
    kBtnPress = rgb(0xFFFFFF, 0.10f);
    kGlyph = rgb(0xFFFFFF, 0.95f);
    kGlyphIdle = rgb(0xFFFFFF, 0.60f);
    kText = rgb(0xFFFFFF);
    kTextIdle = rgb(0xFFFFFF, 0.62f);
    kTextMuted = rgb(0xFFFFFF, 0.55f);
    kTextDim = rgb(0xFFFFFF, 0.38f);
    kShadow = rgb(0x000000, 0.50f);
    kShadowIdle = rgb(0x000000, 0.32f);
    kTaskbarTint = rgb(0x202020);
    kTaskbarTintOpacity = 0.75f;
    kTaskbarSaturate = 3.0f;
    kShellTint = rgb(0x2B2B2B, 0.90f);
    kFlyoutTint = rgb(0x2B2B2B, 0.94f);
    kShellBorder = rgb(0xFFFFFF, 0.09f);
    kShellLine = rgb(0xFFFFFF, 0.06f);
    kItemHover = rgb(0xFFFFFF, 0.06f);
    kItemActive = rgb(0xFFFFFF, 0.10f);
    kItemPress = rgb(0xFFFFFF, 0.04f);
    kSearchBox = rgb(0xFFFFFF, 0.06f);
    kTaskActivePlate = rgb(0xFFFFFF, 0.115f);
    kTaskGlow = rgb(0x4CC2FF, 1.0f);
    kCircleRing = rgb(0xFFFFFF, 0.95f);
    kScrim = rgb(0x0A0A0F, 0.96f);
    kFieldFill = rgb(0x1C1C1E, 0.72f);
    kSnapFill = rgb(0x4CC2FF, 0.22f);
    kSnapBorder = rgb(0x9AD8FF, 0.80f);
    kAccentRing = rgb(0x4CC2FF, 0.90f);
    kCardTint = rgb(0x2B2B2B, 0.85f);
    kDesktopBlur = rgb(0x000000, 0.45f);
}

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
inline constexpr Color kLaunchSearch = rgb(0xFFFFFF, 0.05f);   // search field wash
inline constexpr Color kLaunchSearchBorder = rgb(0xFFFFFF, 0.22f);  // its 1px hairline
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
inline constexpr Color kTabletFolderBack = rgb(0xFFFFFF, 0.16f);   // the squircle behind a folder's mini icons
inline constexpr Color kTabletFolderHover = rgb(0xFFFFFF, 0.30f);   // drop target: an icon over a folder
inline constexpr Color kTabletFolderSheet = rgb(0x1C1C22, 0.82f);   // the open folder's rounded sheet
// The dot under a dock / grid icon whose app is open. One colour for both states:
// the dock already says *which* apps are here, and the dot only has to say "this
// one is running".
inline constexpr Color kTabletRunDot = rgb(0xFFFFFF, 0.92f);
// A row that takes something away, so it is worded and coloured apart from the rest.
inline constexpr Color kTabletMenuDanger = rgb(0xFF6B6B);
inline constexpr Color kTabletBadgeFill = rgb(0x1B1B22, 0.94f);     // the dock's remove badge
inline constexpr Color kTabletBadgeTile = rgb(0xFFFFFF, 0.10f);     // the dock's "+" tile
inline constexpr Color kTabletBadgeGlyph = rgb(0xFFFFFF, 0.96f);
inline constexpr Color kTabletAccent = rgb(0x0A6CFF);               // HarmonyOS blue
inline constexpr Color kTabletPanelBorder = rgb(0xFFFFFF, 0.20f);
inline constexpr Color kTabletSplash = rgb(0x0A0A0F, 0.96f);
inline constexpr Color kTabletSplashLabel = rgb(0xFFFFFF);
inline constexpr Color kTabletSplashSub = rgb(0xFFFFFF, 0.55f);
inline constexpr Color kTabletSplashTrack = rgb(0xFFFFFF, 0.16f);
inline constexpr Color kTabletSplashFill = rgb(0xFFFFFF, 0.94f);

// --- desktop widgets (iOS 18 full-colour) ---------------------------------
// iOS 18 widgets are opaque full-colour panels rather than glass: each card is a
// gradient with crisp white content on top. The clock is the monochrome one -- a
// black face with white ticks, numerals and hands -- and the battery panel takes
// its hue from the charge, so the colour of the card carries the reading.
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
inline constexpr Color kWidgetSecond = rgb(0xFF9F0A);         // iOS systemOrange

inline constexpr Color kWidgetPin = rgb(0xFFFFFF, 0.95f);
inline constexpr Color kWidgetRingTrack = rgb(0xFFFFFF, 0.28f);
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
// window have to read as the same size of app. 42 rather than Windows' 44 keeps
// the icons a touch closer together than the reference bar.
inline int taskButtonW() { return 42 * taskbarH / kTaskbarDefaultH; }
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
constexpr int kLaunchIcon = 80;      // app icon edge at 100% (the macOS Web size)
constexpr int kLaunchIconMin = 46;   // floor when the screen is small
constexpr int kLaunchCols = 4;       // the macOS Web launchpad is four across
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
// iOS caps its dock at four icons. HarmonyOS 6 raised its own from four columns
// to five, and a tablet row has width for more again, so this is a ceiling the
// screen usually gets to ignore: what the dock really holds is however many
// icons fit across it at the current size (see tabletDockCapacity()).
constexpr int kTabletDockMax = 6;
// The quick actions sheet: the menu a still long press opens on an icon. Sized as a
// share of the screen so it reads the same on a phone-sized panel and a desktop.
constexpr int kTabletMenuWidthPct = 42;
constexpr int kTabletMenuMinW = 288;
constexpr int kTabletMenuMaxW = 380;
constexpr int kTabletMenuPad = 10;
constexpr int kTabletMenuHeadH = 78;   // icon + name above the rows
constexpr int kTabletMenuRowH = 44;
// The running indicator: a small dot under an icon. The gap is the space a grid
// icon already leaves between itself and its label; a dock icon has no label and
// gets the same dot a little further down.
constexpr int kTabletRunDotSize = 5;
constexpr int kTabletRunDotGap = 1;

// How full the dock is on a session that has never edited it. This is the size a
// tablet ships with, and it is deliberately not the ceiling: a user who wants more
// puts them there, rather than arriving to find their apps already in the dock.
constexpr int kTabletDockFill = 4;
// The dock's app picker: a grid of twelve to a page, with page dots under it.
constexpr int kTabletDockPickerCols = 4;
constexpr int kTabletDockPickerPerPage = 12;
// Turning a page of the home screen. A sideways drag on the wallpaper has to pass
// the same slop as any other press before it counts, so an accidental brush does
// not flick the grid; holding a lifted icon against a screen edge turns the page
// after a beat, so a drag can be carried to icons that are not on screen.
constexpr int kTabletSwipeSlop = 22;
constexpr int kTabletPageDotsH = 26;   // the strip the page dots sit in
constexpr int kTabletPageTurnMs = 240; // how long a page takes to settle
constexpr int kTabletDragEdgePx = 30;  // edge band a lifted icon turns the page in
constexpr int kTabletDragEdgeMs = 380;
constexpr int kTabletLongPressMs = 450;  // hold this long to rearrange
// A folder shows nine apps at a time in a 3x3, both in the closed icon and in the
// open sheet, exactly as iOS lays one out.
constexpr int kTabletFolderPerPage = 9;
constexpr int kTabletFolderMiniMax = 72;
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
