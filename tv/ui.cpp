// Painting for the TV shell: the ten-foot launcher.
//
// The look is deliberately quiet, the way a living-room launcher is: a
// near-black field, one raised banner for the focused app, and a grid of cards
// under it. Colour belongs to app art; the shell itself uses white at a few
// alphas plus a single blue accent for the current section, so a wall of
// colourful icons never has to compete with its own chrome.
//
// Paint order, back to front: backdrop, grid rows, hero, rail, clock, hints.
// The hero and the rail are painted *over* the rows on purpose: the grid scrolls
// behind them, and their opaque fills are what clip a row that is sliding out.
#include "manager.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <string>

namespace tv {
namespace {

constexpr wm::Color kInk = wm::rgb(0xF3F6FA);
constexpr wm::Color kInkDim = wm::rgb(0xFFFFFF, 0.72f);
constexpr wm::Color kInkMute = wm::rgb(0xFFFFFF, 0.46f);
constexpr wm::Color kAccent = wm::rgb(0x8AB4F8);
constexpr wm::Color kField = wm::rgb(0x0A0B0E);

wm::Rect scaledAround(const wm::Rect& r, double scale) {
    const int w = int(std::lround(r.w * scale));
    const int h = int(std::lround(r.h * scale));
    return wm::Rect(r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, w, h);
}

float fade(double v) { return float(std::clamp(v, 0.0, 1.0)); }

// Truncates `s` until it fits `maxW`, ending in an ellipsis. Drops whole UTF-8
// codepoints, so a name with an accent is never cut mid-character.
std::string fit(wm::Text& text, const std::string& s, int px, wm::Weight weight, int maxW) {
    if (s.empty() || maxW <= 0) return std::string();
    if (text.measure(s, px, weight) <= maxW) return s;
    std::string out = s;
    while (!out.empty()) {
        out.pop_back();
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80)
            out.pop_back();
        if (text.measure(out + "\xE2\x80\xA6", px, weight) <= maxW)
            return out + "\xE2\x80\xA6";
    }
    return "\xE2\x80\xA6";
}

// A soft halo built from a few concentric rounded rects. Four stopped layers
// instead of a blur keeps the banding under the panel unnoticeable and costs
// four quads -- the kind of fill a Pi 3 can afford once per frame.
void softGlow(wm::Compositor& comp, const wm::Rect& r, float radius, const wm::Color& c,
              int spread, float opacity) {
    for (int i = 0; i < 4; ++i) {
        const int out = spread * (4 - i) / 4;
        wm::Color layer = c;
        layer.a = c.a * 0.10f * float(4 - i) / 4.0f;
        comp.drawRect(r.inflated(out), radius + float(out) * 0.7f, layer, opacity);
    }
}

const char* sectionTitle(Section s) {
    switch (s) {
        case Section::Home: return "Home";
        case Section::Apps: return "Apps";
        case Section::Recent: return "Recent";
        case Section::Settings: return "Settings";
    }
    return "Home";
}

const char* actionTitle(CardAction a) {
    switch (a) {
        case CardAction::RefreshApps: return "Refresh apps";
        case CardAction::QuitShell: return "Quit shell";
        case CardAction::None: break;
    }
    return "";
}

const char* actionBlurb(CardAction a) {
    switch (a) {
        case CardAction::RefreshApps: return "Rescan installed applications and their icons.";
        case CardAction::QuitShell: return "Stop the compositor and return to the console.";
        case CardAction::None: break;
    }
    return "";
}

}  // namespace

void Manager::drawBackground() {
    compositor_.drawRect(wm::Rect(0, 0, screenW_, screenH_), 0.0f, kField, 1.0f);
    // A cold halo behind the banner; it lifts the darkest part of the screen off
    // pure black without becoming a visible shape of its own. It is four
    // translucent layers spanning most of the screen, which is the single most
    // expensive thing in a launcher frame, so lean mode simply leaves it out.
    if (minimal_) return;
    const LauncherLayout L = launcherLayout();
    softGlow(compositor_, wm::Rect(L.contentX, L.topH, L.contentW, L.heroH),
             float(std::clamp(screenH_ / 40, 14, 24)), wm::rgb(0x1B3450),
             std::max(48, screenH_ / 9), 1.0f);
}

void Manager::drawClient() {
    if (!active_ || !active_->tex.valid()) return;
    const wm::WindowTex& tex = active_->tex;
    const double sx = double(screenW_) / double(tex.w);
    const double sy = double(screenH_) / double(tex.h);
    const double scale = std::min(sx, sy);
    const int w = std::max(1, int(std::lround(tex.w * scale)));
    const int h = std::max(1, int(std::lround(tex.h * scale)));
    const int x = (screenW_ - w) / 2;
    const int y = (screenH_ - h) / 2;
    compositor_.drawTex(tex.tex, wm::Rect(x, y, w, h), 0.0f, wm::rgb(0xFFFFFF), 1.0f, true,
                        tex.alpha);
}

void Manager::drawHome() {
    const float opacity = fade(homeSpring_.value);
    if (opacity <= 0.001f) return;

    // Dim whatever is behind the launcher so the cards keep their contrast when
    // it fades in over a running app. With no app behind it there is nothing to
    // dim, so the full-screen fill is skipped.
    if (active_ && active_->mapped)
        compositor_.drawRect(wm::Rect(0, 0, screenW_, screenH_), 0.0f,
                             wm::rgb(0x000000, 0.62f), opacity);

    ensureIcons();
    drawRows(opacity);
    drawHero(opacity);
    drawRail(opacity);
    drawTopBar(opacity);
    drawHints(opacity);
}

void Manager::drawHero(float opacity) {
    const LauncherLayout L = launcherLayout();
    const wm::Rect hero(L.contentX, L.topH, L.contentW, L.heroH);
    const int radius = std::clamp(screenH_ / 40, 14, 24);
    const int pad = std::clamp(L.heroH / 7, 18, 34);

    compositor_.drawRect(hero.inflated(1), float(radius + 1), wm::rgb(0xFFFFFF, 0.06f), opacity);
    // One flat banner in lean mode instead of a full-width blend.
    if (minimal_)
        compositor_.drawRect(hero, float(radius), wm::rgb(0x171C24), opacity);
    else
        compositor_.drawGradient(hero, float(radius), wm::rgb(0x1B222C), wm::rgb(0x11151B),
                                 opacity);

    const CardRef* card = focusedCard();
    const TvApp* app = nullptr;
    if (card && card->app >= 0 && card->app < int(apps_.size())) app = &apps_[size_t(card->app)];

    const wm::IconTex* icon = nullptr;
    if (app) icon = &app->icon;
    else if (card && card->action == CardAction::RefreshApps) icon = &iconRefresh_;
    else if (card && card->action == CardAction::QuitShell) icon = &iconQuit_;

    // A ghost of the app's own artwork on the right half: a wide banner needs
    // something there, and this says "this app" without another panel or a
    // downloaded backdrop image.
    if (icon && icon->valid()) {
        const int ghost = int(double(L.heroH) * 1.15);
        const int gx = hero.right() - ghost - L.heroH / 6;
        const int gy = hero.y + (L.heroH - ghost) / 2;
        compositor_.drawTex(icon->tex, wm::Rect(gx, gy, ghost, ghost), float(ghost) * 0.22f,
                            wm::rgb(0xFFFFFF), opacity * 0.07f, true, true);
    }

    const int tile = std::clamp(int(double(L.heroH) * 0.54), 56, 148);
    const wm::Rect tileR(hero.x + pad, hero.y + (L.heroH - tile) / 2, tile, tile);
    compositor_.drawRect(tileR, float(tile) * 0.24f, wm::rgb(0xFFFFFF, 0.055f), opacity);
    if (icon && icon->valid()) {
        const int inset = std::max(6, tile / 8);
        const int edge = tile - inset * 2;
        compositor_.drawTex(icon->tex, wm::Rect(tileR.x + inset, tileR.y + inset, edge, edge),
                            float(edge) * 0.20f, wm::rgb(0xFFFFFF), opacity, true, true);
    }

    const int titlePx = std::clamp(screenH_ / 26, 20, 40);
    const int subPx = std::clamp(screenH_ / 54, 12, 19);
    const int chipPx = std::clamp(screenH_ / 66, 10, 15);
    const int textX = tileR.right() + std::max(16, pad - 4);
    const int textW = std::max(120, hero.right() - pad - textX);

    std::string title;
    std::string blurb;
    if (app) {
        title = app->entry.name;
        blurb = app->entry.generic;
        if (blurb.empty()) blurb = "Application";
    } else if (card) {
        title = actionTitle(card->action);
        blurb = actionBlurb(card->action);
    }
    if (title.empty()) {
        title = section_ == Section::Recent ? "No recent apps" : "No apps found";
        blurb = section_ == Section::Recent
                    ? "Apps you open appear here for quick switching."
                    : "Install applications and pick Refresh apps in Settings.";
    }

    const bool hasAction = card && (app || card->action != CardAction::None);
    const char* verb = app ? "Open" : "Select";
    const int capH = std::clamp(chipPx * 2, 20, 28);
    const int gapTitle = std::max(4, titlePx / 6);
    const int gapSub = std::max(8, subPx / 2);

    title = fit(text_, title, titlePx, wm::Weight::Bold, textW);
    blurb = fit(text_, blurb, subPx, wm::Weight::Regular, textW);
    int th = 0;
    int sh = 0;
    text_.measure(title, titlePx, wm::Weight::Bold, &th);
    text_.measure(blurb, subPx, wm::Weight::Regular, &sh);

    const int blockH = th + gapTitle + sh + (hasAction ? gapSub + capH : 0);
    int ty = hero.y + std::max(pad / 2, (L.heroH - blockH) / 2);
    drawTextAt(title, titlePx, wm::Weight::Bold, kInk, textX, ty, opacity);
    ty += th + gapTitle;
    drawTextAt(blurb, subPx, wm::Weight::Regular, kInkDim, textX, ty, opacity);
    ty += sh + gapSub;
    if (hasAction) {
        const wm::Rect cap(textX, ty, capH, capH);
        drawKeycap("A", cap, opacity);
        int lh = 0;
        text_.measure(verb, chipPx, wm::Weight::Medium, &lh);
        drawTextAt(verb, chipPx, wm::Weight::Medium, kInk,
                   cap.right() + std::max(8, capH / 3), ty + (capH - lh) / 2, opacity);
    }

    // The banner's own count, so the section says how much is in it without a
    // second label floating at the bottom edge.
    std::string meta;
    if (section_ == Section::Settings) {
        meta = std::to_string(screenW_) + "x" + std::to_string(screenH_) + "  \xC2\xB7  " +
               std::to_string(controllers_.count()) + " controller" +
               (controllers_.count() == 1 ? "" : "s");
    } else if (section_ == Section::Recent) {
        meta = std::to_string(recentExecs_.size()) + " recent";
    } else {
        meta = std::to_string(apps_.size()) + " apps";
    }
    const int metaPx = std::clamp(screenH_ / 68, 10, 14);
    const wm::TextTex metaTex = text_.get(meta, metaPx, wm::Weight::Regular);
    if (metaTex.tex)
        compositor_.drawText(metaTex,
                             wm::Rect(hero.right() - pad - metaTex.w, hero.y + pad, metaTex.w,
                                      metaTex.h),
                             kInkMute, opacity * 0.85f);
}

void Manager::drawRows(float opacity) {
    const LauncherLayout L = launcherLayout();
    const int captionPx = std::clamp(screenH_ / 52, 13, 20);
    const int labelPx = std::clamp(screenH_ / 64, 10, 15);
    const int step = L.cardW + L.gap;
    const int labelH = labelPx + 8;
    const int radius = std::clamp(L.cardH / 11, 8, 20);
    const int scroll = int(std::lround(gridScroll_.value));

    for (size_t r = 0; r < rows_.size(); ++r) {
        const Row& row = rows_[r];
        const int rowY = L.rowsTop + int(r) * L.rowStride - scroll;
        // A page only ever moves by whole rows, so a row is either fully inside
        // the grid or fully outside it.
        if (rowY + L.rowStride - L.gap <= L.rowsTop - 2) continue;
        if (rowY >= L.gridBottom - 2) continue;

        const bool rowFocused = zone_ == Zone::Content && int(r) == row_;
        if (!row.title.empty()) {
            drawTextAt(fit(text_, row.title, captionPx, wm::Weight::Bold, L.contentW),
                       captionPx, wm::Weight::Bold, rowFocused ? kInk : kInkMute, L.contentX,
                       rowY + std::max(0, (L.captionH - captionPx) / 2 - 2), opacity);
        }

        const double strip = r < rowScroll_.size() ? rowScroll_[r].value : 0.0;
        const int cardY = rowY + L.captionH;
        for (size_t c = 0; c < row.cards.size(); ++c) {
            const int x = L.contentX - int(std::lround(strip)) + int(c) * step;
            // Cards that have slid under the rail are dropped; a card leaving the
            // right edge can be partial, which is how a TV row says "more".
            if (x < L.contentX - 1) continue;
            if (x > screenW_ - 8) continue;

            const bool focused = rowFocused && int(c) == col_;
            wm::Rect card(x, cardY, L.cardW, L.cardH);
            if (focused) card = scaledAround(card, 1.0 + 0.03 * focusScale_.value);

            const CardRef& ref = row.cards[c];
            const TvApp* rowApp = nullptr;
            if (ref.app >= 0 && ref.app < int(apps_.size())) rowApp = &apps_[size_t(ref.app)];
            const wm::IconTex* cardIcon = nullptr;
            std::string label;
            if (rowApp) {
                cardIcon = &rowApp->icon;
                label = rowApp->entry.name;
            } else if (ref.action == CardAction::RefreshApps) {
                cardIcon = &iconRefresh_;
                label = actionTitle(ref.action);
            } else if (ref.action == CardAction::QuitShell) {
                cardIcon = &iconQuit_;
                label = actionTitle(ref.action);
            }

            if (focused) {
                compositor_.drawRect(card.inflated(3), float(radius + 3),
                                     wm::rgb(0x8AB4F8, 0.20f), opacity);
                compositor_.drawRect(card.inflated(2), float(radius + 2),
                                     wm::rgb(0xFFFFFF, 0.94f), opacity);
            } else if (!minimal_) {
                // A hairline edge on every card; lean mode skips the quad and
                // lets the card's own fill sit straight on the field.
                compositor_.drawRect(card.inflated(1), float(radius + 1),
                                     wm::rgb(0xFFFFFF, 0.07f), opacity);
            }
            compositor_.drawRect(card, float(radius),
                                 focused ? wm::rgb(0x252C36) : wm::rgb(0x161A20), opacity);

            const int iconPx = cardIconPx();
            if (cardIcon && cardIcon->valid()) {
                const int ix = card.x + (card.w - iconPx) / 2;
                const int iy = card.y + std::max(8, (L.cardH - labelH - iconPx) / 2);
                compositor_.drawTex(cardIcon->tex, wm::Rect(ix, iy, iconPx, iconPx),
                                    float(iconPx) * 0.22f, wm::rgb(0xFFFFFF), opacity, true, true);
            }

            const wm::Rect labelBox(card.x + 8, card.bottom() - labelH - 8, card.w - 16, labelH);
            drawTextCentered(fit(text_, label, labelPx, wm::Weight::Medium, labelBox.w), labelPx,
                             wm::Weight::Medium, focused ? kInk : kInkDim, labelBox, opacity);
        }
    }
}

void Manager::drawRail(float opacity) {
    const LauncherLayout L = launcherLayout();
    // A real panel rather than a vignette: it is the one edge in the design,
    // and it is what hides a row that has scrolled off to the left.
    compositor_.drawRect(wm::Rect(0, 0, L.railW, screenH_), 0.0f, wm::rgb(0x0C0E12), opacity);
    compositor_.drawRect(wm::Rect(L.railW - 1, 0, 1, screenH_), 0.0f, wm::rgb(0xFFFFFF, 0.05f),
                         opacity);

    // The head of the rail stays empty: the mark lives in the Home row below.
    // Loading is still done here so the Home slot has its texture by the time
    // the nav loop reaches it.
    ensureLogo();

    const int itemH = L.railItemH;
    const int iconEdge = std::clamp(itemH / 3, 20, 30);
    const int labelPx = std::clamp(screenH_ / 74, 10, 13);
    const int itemW = std::max(48, L.railW - 18);
    const int x = (L.railW - itemW) / 2;

    for (int i = 0; i < 4; ++i) {
        const wm::Rect item(x, L.topH + i * itemH, itemW, itemH - 8);
        const bool selected = i == int(section_);
        const bool focused = zone_ == Zone::Rail && i == railFocus_;

        if (focused) {
            compositor_.drawRect(item, 14.0f, wm::rgb(0x8AB4F8, 0.17f), opacity);
            compositor_.drawRect(wm::Rect(0, item.y + (item.h - 22) / 2, 3, 22), 1.5f, kAccent,
                                 opacity);
        } else if (selected) {
            compositor_.drawRect(item, 14.0f, wm::rgb(0xFFFFFF, 0.075f), opacity);
        }

        const wm::Color tone = selected ? kInk : kInkMute;
        const wm::Rect iconR(item.x + (item.w - iconEdge) / 2, item.y + 10, iconEdge, iconEdge);
        // Home wears the shell's own mark -- untinted, since the logo's colour is
        // the point of it; the rest keep their theme glyphs in the rail's tone.
        if (i == 0 && logo_.valid())
            compositor_.drawTex(logo_.tex, iconR, 0.0f, wm::rgb(0xFFFFFF), opacity, true, true);
        else if (railIcons_[i].valid())
            compositor_.drawTex(railIcons_[i].tex, iconR, 0.0f, tone, opacity, true, true);

        const wm::Rect labelBox(item.x, item.y + 10 + iconEdge, item.w, labelPx + 10);
        drawTextCentered(fit(text_, sectionTitle(Section(i)), labelPx, wm::Weight::Medium,
                             labelBox.w),
                         labelPx, wm::Weight::Medium, tone, labelBox, opacity);
    }
}

wm::Rect Manager::clockTextRect() {
    const LauncherLayout L = launcherLayout();
    char clock[16] = {};
    char date[32] = {};
    const time_t t = time(nullptr);
    struct tm lt {};
    localtime_r(&t, &lt);
    std::strftime(clock, sizeof(clock), "%H:%M", &lt);
    std::strftime(date, sizeof(date), "%a %d %b", &lt);

    const int clockPx = std::clamp(screenH_ / 32, 18, 30);
    const int datePx = std::clamp(clockPx * 2 / 5, 10, 14);
    int ch = 0;
    int dh = 0;
    const int cw = text_.measure(clock, clockPx, wm::Weight::Bold, &ch);
    const int dw = text_.measure(date, datePx, wm::Weight::Regular, &dh);
    const int w = std::max(cw, dw);
    const int h = ch + 2 + dh;
    const int rightEdge = screenW_ - std::max(24, screenW_ / 44);
    return wm::Rect(rightEdge - w, std::max(6, (L.topH - h) / 2), w, h);
}

wm::Rect Manager::clockHitRect() {
    wm::Rect r = clockTextRect();
    // A ten-foot UI wants a generous target: the clock is small type in a corner
    // and a TV pointer is not a mouse.
    const int pad = std::max(12, screenH_ / 40);
    r = r.inflated(pad);
    if (r.x < 0) {
        r.w += r.x;
        r.x = 0;
    }
    if (r.y < 0) {
        r.h += r.y;
        r.y = 0;
    }
    if (r.right() > screenW_) r.w = screenW_ - r.x;
    return r;
}

void Manager::drawTopBar(float opacity) {
    char clock[16] = {};
    char date[32] = {};
    const time_t t = time(nullptr);
    struct tm lt {};
    localtime_r(&t, &lt);
    std::strftime(clock, sizeof(clock), "%H:%M", &lt);
    std::strftime(date, sizeof(date), "%a %d %b", &lt);

    const int clockPx = std::clamp(screenH_ / 32, 18, 30);
    const int datePx = std::clamp(clockPx * 2 / 5, 10, 14);
    const wm::TextTex ct = text_.get(clock, clockPx, wm::Weight::Bold);
    const wm::TextTex dt = text_.get(date, datePx, wm::Weight::Regular);
    // Drawn inside the block clockTextRect() reports, so the click target is
    // exactly the time the user sees.
    const wm::Rect block = clockTextRect();
    const int rightEdge = block.right();
    const int top = block.y;

    if (ct.tex)
        compositor_.drawText(ct, wm::Rect(rightEdge - ct.w, top, ct.w, ct.h),
                             wm::rgb(0xFFFFFF, 0.96f), opacity);
    if (dt.tex)
        compositor_.drawText(dt, wm::Rect(rightEdge - dt.w, top + ct.h + 2, dt.w, dt.h),
                             kInkMute, opacity);
}

void Manager::drawHints(float opacity) {
    const LauncherLayout L = launcherLayout();
    const int px = std::clamp(screenH_ / 66, 10, 14);

    std::string hints;
    if (ccOpen_)
        hints = "D-pad  Move      A  Select      B  Close";
    else if (zone_ == Zone::Rail)
        hints = "D-pad  Sections      Right / A  Browse      LB / RB  Tabs";
    else if (rows_.empty())
        hints = "B  Sections      LB / RB  Tabs";
    else if (section_ == Section::Settings)
        hints = "D-pad  Move      A  Select      B  Back";
    else
        hints = "D-pad  Move      A  Open      B  Back      LB / RB  Tabs";

    drawTextCentered(hints, px, wm::Weight::Regular, kInkMute,
                     wm::Rect(0, screenH_ - L.footerH, screenW_, L.footerH), opacity);
}

void Manager::drawCursor() {
    if (!pointerPresent_ || !pointerSeen_) return;
    // Over a fullscreen app there is nothing for the shell to point at, so the
    // dot stays hidden -- a cursor parked on top of a film is worse than none.
    // With the panel open it comes back, since its controls can be clicked
    // from over a running app too.
    if (homeSpring_.value <= 0.001 && ccSpring_.value <= 0.001 && !ccOpen_) return;
    const int x = int(std::lround(ptrX_));
    const int y = int(std::lround(ptrY_));
    // The shell's own pointer: a dot with a dark edge, drawn with two quads.
    // There is no cursor theme here -- a TV shell has nothing to point at but
    // the clock and the Control Centre.
    const int d = std::clamp(screenH_ / 80, 12, 24);
    compositor_.drawRect(wm::Rect(x - d / 2 - 1, y - d / 2 - 1, d + 2, d + 2),
                         float(d + 2) * 0.5f, wm::rgb(0x000000, 0.45f), 1.0f);
    compositor_.drawRect(wm::Rect(x - d / 2, y - d / 2, d, d), float(d) * 0.5f,
                         wm::rgb(0xFFFFFF), 0.94f);
}

void Manager::drawKeycap(const char* key, const wm::Rect& box, float opacity) {
    compositor_.drawRect(box, float(box.h) * 0.3f, wm::rgb(0xFFFFFF, 0.15f), opacity);
    const int px = std::max(8, int(double(box.h) * 0.5));
    drawTextCentered(key, px, wm::Weight::Bold, kInk, box, opacity);
}

void Manager::drawTextAt(const std::string& text, int px, wm::Weight weight,
                         const wm::Color& color, int x, int y, float opacity) {
    if (text.empty()) return;
    const wm::TextTex t = text_.get(text, px, weight);
    if (!t.tex) return;
    compositor_.drawText(t, wm::Rect(x, y, t.w, t.h), color, opacity);
}

void Manager::drawTextCentered(const std::string& text, int px, wm::Weight weight,
                               const wm::Color& color, const wm::Rect& box, float opacity) {
    if (text.empty()) return;
    const wm::TextTex t = text_.get(text, px, weight);
    if (!t.tex) return;
    const int x = box.x + (box.w - t.w) / 2;
    const int y = box.y + (box.h - t.h) / 2;
    compositor_.drawText(t, wm::Rect(x, y, t.w, t.h), color, opacity);
}

}  // namespace tv
