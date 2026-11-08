// Painting for the TV shell: the Android TV style launcher.
//
// Layout, from back to front: backdrop, hero preview of the focused card,
// horizontally scrolling rows of cards, then the section rail. The rail is
// painted *over* the rows on purpose -- a row's strip slides underneath it as
// the focus moves, and the rail's scrim is what hides the cards that have
// scrolled off the left of the content area. The top bar and the hint strip
// close the frame.
#include "manager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace tv {
namespace {

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

std::vector<std::string> wrap(wm::Text& text, const std::string& s, int px, wm::Weight weight,
                              int maxW, int maxLines) {
    std::vector<std::string> lines;
    std::string line;
    size_t i = 0;
    while (i < s.size() && int(lines.size()) < maxLines) {
        size_t j = i;
        while (j < s.size() && s[j] != ' ') ++j;
        const std::string word = s.substr(i, j - i);
        i = j + 1;
        const std::string candidate = line.empty() ? word : line + " " + word;
        if (line.empty() || text.measure(candidate, px, weight) <= maxW) {
            line = candidate;
        } else {
            lines.push_back(line);
            line = word;
        }
    }
    if (!line.empty() && int(lines.size()) < maxLines) lines.push_back(line);
    if (!lines.empty()) lines.back() = fit(text, lines.back(), px, weight, maxW);
    return lines;
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
        case CardAction::RefreshApps: return "Rescan the installed applications and their icons.";
        case CardAction::QuitShell: return "Stop the compositor and return to the console.";
        case CardAction::None: break;
    }
    return "";
}

}  // namespace

void Manager::drawBackground() {
    compositor_.drawGradient(wm::Rect(0, 0, screenW_, screenH_), 0.0f,
                             wm::rgb(0x060A12), wm::rgb(0x13223A), 1.0f);
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
    compositor_.drawTex(tex.tex, wm::Rect(x, y, w, h), 0.0f, wm::rgb(0xFFFFFF), 1.0f,
                        true, tex.alpha);
}

void Manager::drawHome() {
    const float opacity = fade(homeSpring_.value);
    if (opacity <= 0.001f) return;

    // Dim whatever is behind the launcher so the cards keep their contrast when
    // it fades in over a running app.
    compositor_.drawRect(wm::Rect(0, 0, screenW_, screenH_), 0.0f,
                         wm::rgb(0x000000, 0.62f), opacity);

    ensureIcons();
    drawHero(opacity);
    drawRows(opacity);
    drawRail(opacity);
    drawTopBar(opacity);
    drawHints(opacity);
}

void Manager::drawHero(float opacity) {
    const LauncherLayout L = launcherLayout();
    const wm::Rect hero(L.contentX, L.topH + std::max(8, screenH_ / 90), L.contentW, L.heroH);

    compositor_.drawGradient(hero, 24.0f, wm::rgb(0x173650), wm::rgb(0x0A1524),
                             opacity);

    const CardRef* card = focusedCard();
    const TvApp* app = nullptr;
    if (card && card->app >= 0 && card->app < int(apps_.size())) app = &apps_[size_t(card->app)];

    const int pad = std::max(18, L.heroH / 8);
    const int iconEdge = std::max(56, int(L.heroH * 0.44));
    const int iconX = hero.x + pad;
    const int iconY = hero.y + (hero.h - iconEdge) / 2;

    const wm::IconTex* icon = nullptr;
    if (app) icon = &app->icon;
    else if (card && card->action == CardAction::RefreshApps) icon = &iconRefresh_;
    else if (card && card->action == CardAction::QuitShell) icon = &iconQuit_;

    if (icon && icon->valid()) {
        compositor_.drawRect(wm::Rect(iconX - 8, iconY - 8, iconEdge + 16, iconEdge + 16),
                             float(iconEdge / 5), wm::rgb(0xFFFFFF, 0.06f), opacity);
        compositor_.drawTex(icon->tex, wm::Rect(iconX, iconY, iconEdge, iconEdge), 0.0f,
                            wm::rgb(0xFFFFFF), opacity, true, true);
    }

    const int textX = iconX + iconEdge + pad;
    const int textW = std::max(120, hero.right() - textX - pad);
    const int titlePx = std::clamp(screenH_ / 15, 26, 58);
    const int subPx = std::clamp(screenH_ / 38, 14, 26);
    const int labelPx = std::clamp(screenH_ / 56, 11, 18);

    // The destination name, in the quiet overline style TV launchers use.
    drawTextAt(sectionTitle(section_), labelPx, wm::Weight::Bold,
               wm::rgb(0x8FD4FF), textX, hero.y + pad, opacity);

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

    const int titleY = hero.y + pad + labelPx + std::max(6, screenH_ / 120);
    drawTextAt(fit(text_, title, titlePx, wm::Weight::Bold, textW), titlePx,
               wm::Weight::Bold, wm::theme::kText, textX, titleY, opacity);

    int lineY = titleY + titlePx + std::max(8, screenH_ / 100);
    for (const std::string& line :
         wrap(text_, blurb, subPx, wm::Weight::Regular, textW, 2)) {
        drawTextAt(line, subPx, wm::Weight::Regular, wm::theme::kTextIdle, textX, lineY,
                   opacity);
        lineY += subPx + std::max(2, subPx / 4);
    }

    // The action prompt: whichever button the controller should use next.
    if (!title.empty() && card && (app || card->action != CardAction::None)) {
        const bool isAction = !app;
        const int pillH = labelPx * 2 + 10;
        const int pillW = isAction ? text_.measure("A  Select", labelPx, wm::Weight::Bold) +
                                         labelPx * 2
                                   : text_.measure("A  Open", labelPx, wm::Weight::Bold) +
                                         labelPx * 2;
        const wm::Rect pill(textX, std::min(hero.bottom() - pillH - pad, lineY + 4), pillW,
                            pillH);
        compositor_.drawRect(pill, float(pillH) / 2, wm::rgb(0x4CC2FF, 0.20f), opacity);
        drawTextCentered(isAction ? "A  Select" : "A  Open", labelPx, wm::Weight::Bold,
                         wm::rgb(0xD6EEFF), pill, opacity);
    }
}

void Manager::drawRows(float opacity) {
    const LauncherLayout L = launcherLayout();
    const int captionPx = std::clamp(screenH_ / 46, 14, 24);
    const int labelPx = std::clamp(screenH_ / 54, 12, 20);
    const int step = L.cardW + L.gap;

    for (size_t r = 0; r < rows_.size(); ++r) {
        const Row& row = rows_[r];
        const int rowY = L.rowsTop + int(r) * L.rowStride;
        const bool rowFocused = zone_ == Zone::Content && int(r) == row_;

        const int captionY = rowY + (L.captionH - captionPx) / 2 - captionPx / 6;
        if (rowFocused) {
            compositor_.drawRect(
                wm::Rect(L.contentX - std::max(10, L.gap), captionY + captionPx / 6,
                         std::max(3, L.gap / 3), captionPx),
                float(std::max(3, L.gap / 3)) / 2, wm::theme::kAccent, opacity);
        }
        drawTextAt(fit(text_, row.title, captionPx, wm::Weight::Bold, L.contentW), captionPx,
                   wm::Weight::Bold, rowFocused ? wm::theme::kText : wm::theme::kTextMuted,
                   L.contentX, captionY, opacity);

        const double scroll = r < rowScroll_.size() ? rowScroll_[r].value : 0.0;
        const int cardY = rowY + L.captionH;
        for (size_t c = 0; c < row.cards.size(); ++c) {
            const int x = L.contentX - int(std::lround(scroll)) + int(c) * step;
            // Only fully visible cards are painted. A card that is halfway past
            // either edge of the content area would otherwise spill over the
            // rail or the screen edge, and there is no scissor in this pipeline.
            if (x < L.contentX - 1) continue;
            if (x + L.cardW > L.contentX + L.contentW + 1) continue;

            const bool focused = rowFocused && int(c) == col_;
            wm::Rect card(x, cardY, L.cardW, L.cardH);
            if (focused) card = scaledAround(card, 1.0 + 0.05 * focusScale_.value);

            if (focused) {
                compositor_.drawRect(card.inflated(3), 19.0f, wm::theme::kAccent, opacity);
                compositor_.drawRect(card, 16.0f, wm::rgb(0x22344C), opacity);
            } else {
                compositor_.drawRect(card, 16.0f, wm::rgb(0xFFFFFF, 0.055f), opacity);
            }

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

            const int labelH = std::max(24, L.cardH / 6);
            const int iconPx = std::max(32, std::min(L.cardH - labelH - L.cardH / 4,
                                                     int(L.cardW * 0.62)));
            if (cardIcon && cardIcon->valid()) {
                const int ix = card.x + (card.w - iconPx) / 2;
                const int iy = card.y + std::max(8, (L.cardH - labelH - iconPx) / 2);
                compositor_.drawTex(cardIcon->tex, wm::Rect(ix, iy, iconPx, iconPx), 0.0f,
                                    wm::rgb(0xFFFFFF), opacity, true, true);
            }

            const wm::Rect labelBox(card.x + 10, card.bottom() - labelH - 6, card.w - 20,
                                    labelH);
            drawTextCentered(fit(text_, label, labelPx, wm::Weight::Medium, labelBox.w), labelPx,
                             wm::Weight::Medium,
                             focused ? wm::theme::kText : wm::theme::kTextIdle, labelBox,
                             opacity);
        }
    }
}

void Manager::drawRail(float opacity) {
    const LauncherLayout L = launcherLayout();
    // The scrim is also the clip: rows slide underneath the rail rather than
    // over it.
    compositor_.drawGradient(wm::Rect(0, 0, L.railW + std::max(10, L.railW / 8), screenH_),
                             0.0f, wm::rgb(0x05080F, 0.94f),
                             wm::rgb(0x05080F, 0.62f), opacity);

    const int count = 4;
    const int itemH = L.railItemH;
    const int total = count * itemH;
    const int y0 = std::max(L.topH, (screenH_ - L.footerH - total) / 2);
    const int iconEdge = std::max(20, std::min(itemH / 3, 34));
    const int labelPx = std::clamp(screenH_ / 62, 11, 17);
    const int pad = std::max(6, L.railW / 16);

    for (int i = 0; i < count; ++i) {
        const wm::Rect item(pad, y0 + i * itemH, L.railW - pad * 2, itemH - pad);
        const bool selected = i == int(section_);
        const bool focused = zone_ == Zone::Rail && i == railFocus_;

        if (focused) {
            compositor_.drawRect(item, 14.0f, wm::rgb(0x4CC2FF, 0.20f), opacity);
        } else if (selected) {
            compositor_.drawRect(item, 14.0f, wm::rgb(0xFFFFFF, 0.06f), opacity);
        }

        if (selected) {
            const int barW = std::max(3, pad / 2);
            const int barH = std::max(16, itemH / 2);
            compositor_.drawRect(wm::Rect(pad - barW - 2, item.y + (item.h - barH) / 2, barW,
                                          barH),
                                 float(barW) / 2,
                                 focused ? wm::theme::kAccent : wm::theme::kTextDim, opacity);
        }

        const wm::IconTex& icon = railIcons_[i];
        const int labelH = labelPx + 6;
        const int contentH = iconEdge + labelH;
        const int top = item.y + (item.h - contentH) / 2;
        if (icon.valid()) {
            const int ix = item.x + (item.w - iconEdge) / 2;
            compositor_.drawTex(icon.tex, wm::Rect(ix, top, iconEdge, iconEdge), 0.0f,
                                wm::rgb(0xFFFFFF), opacity, true, true);
        }
        const wm::Rect labelBox(item.x, top + iconEdge, item.w, labelH);
        drawTextCentered(fit(text_, sectionTitle(Section(i)), labelPx, wm::Weight::Bold,
                             labelBox.w),
                         labelPx, wm::Weight::Bold,
                         selected ? wm::theme::kText : wm::theme::kTextMuted, labelBox,
                         opacity);
    }
}

void Manager::drawTopBar(float opacity) {
    const LauncherLayout L = launcherLayout();
    compositor_.drawGradient(wm::Rect(0, 0, screenW_, L.topH), 0.0f,
                             wm::rgb(0x000000, 0.40f),
                             wm::rgb(0x000000, 0.0f), opacity);

    const int wordPx = std::clamp(screenH_ / 30, 18, 34);
    drawTextAt("tv", wordPx, wm::Weight::Bold, wm::theme::kText,
               std::max(20, L.railW / 6), std::max(10, L.topH / 2 - wordPx / 2), opacity);
    compositor_.drawRect(wm::Rect(std::max(20, L.railW / 6) - 4, std::max(10, L.topH / 2 - wordPx / 2) + wordPx / 4, 4, 4), 2.0f, wm::theme::kAccent, opacity);

    char clock[16] = {};
    char date[32] = {};
    const time_t t = time(nullptr);
    struct tm lt {};
    localtime_r(&t, &lt);
    std::strftime(clock, sizeof(clock), "%H:%M", &lt);
    std::strftime(date, sizeof(date), "%a %d %b", &lt);

    const int clockPx = std::clamp(screenH_ / 26, 18, 34);
    const wm::TextTex ct = text_.get(clock, clockPx, wm::Weight::Bold);
    const int right = std::max(24, screenW_ / 40);
    const int top = std::max(8, L.topH / 2 - clockPx);
    if (ct.tex)
        compositor_.drawText(ct, wm::Rect(screenW_ - right - ct.w, top, ct.w, ct.h),
                             wm::theme::kText, opacity);
    const wm::TextTex dt = text_.get(date, std::max(11, clockPx / 2), wm::Weight::Regular);
    if (dt.tex)
        compositor_.drawText(dt, wm::Rect(screenW_ - right - dt.w, top + clockPx + 2, dt.w, dt.h),
                             wm::theme::kTextMuted, opacity);
}

void Manager::drawHints(float opacity) {
    const LauncherLayout L = launcherLayout();
    const int y = screenH_ - L.footerH;
    compositor_.drawRect(wm::Rect(0, y, screenW_, L.footerH), 0.0f,
                         wm::rgb(0x000000, 0.42f), opacity);

    std::string hints;
    if (zone_ == Zone::Rail)
        hints = "D-pad  Sections     Right / A  Browse     LB / RB  Tabs";
    else if (rows_.empty())
        hints = "Nothing here     B  Sections     LB / RB  Tabs";
    else if (section_ == Section::Settings)
        hints = "D-pad  Move     A  Select     B  Back     LB / RB  Tabs";
    else
        hints = "D-pad  Move     A  Open     B  Back     Up  Sections     LB / RB  Tabs";
    drawTextCentered(hints, std::clamp(screenH_ / 64, 11, 18), wm::Weight::Regular,
                     wm::theme::kTextMuted, wm::Rect(0, y, screenW_, L.footerH), opacity);
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
