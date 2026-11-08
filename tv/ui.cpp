#include "manager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace tv {
namespace {

wm::Rect scaledAround(const wm::Rect& r, double scale) {
    const int w = int(std::lround(r.w * scale));
    const int h = int(std::lround(r.h * scale));
    return wm::Rect(r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, w, h);
}

float clampedOpacity(double v) {
    return float(std::clamp(v, 0.0, 1.0));
}

}  // namespace

void Manager::drawBackground() {
    compositor_.drawGradient(wm::Rect(0, 0, screenW_, screenH_), 0.0f,
                             wm::theme::rgb(0x0A1020), wm::theme::rgb(0x1A2A46), 1.0f);
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
    compositor_.drawTex(tex.tex, wm::Rect(x, y, w, h), 0.0f, wm::theme::rgb(0xFFFFFF),
                        1.0f, true, tex.alpha);
}

void Manager::drawHome() {
    const float opacity = clampedOpacity(homeSpring_.value);
    if (opacity <= 0.001f) return;

    compositor_.drawRect(wm::Rect(0, 0, screenW_, screenH_), 0.0f,
                         wm::theme::rgb(0x000000, 0.58f), opacity);
    drawTopBar(opacity);

    ensureIcons();

    const int columns = std::max(1, columns_);
    const int rows = std::max(1, rows_);
    const int per = columns * rows;
    const int page = currentPage();
    const int first = page * per;

    const int top = std::max(24, screenH_ / 18);
    const int headerH = std::max(62, screenH_ / 9);
    const int footerH = std::max(36, screenH_ / 18);
    const int contentTop = top + headerH;
    const int contentBottom = screenH_ - footerH;
    const int contentH = std::max(1, contentBottom - contentTop);

    const int gapX = std::max(12, screenW_ / 64);
    const int gapY = std::max(10, screenH_ / 72);
    const int gridW = std::min(screenW_ * 4 / 5, 1280);
    const int tileW = std::max(96, (gridW - gapX * (columns - 1)) / columns);
    const int tileH =
        std::max(112, std::min(tileW + screenH_ / 12, (contentH - gapY * (rows - 1)) / rows));
    const int usedW = tileW * columns + gapX * (columns - 1);
    const int usedH = tileH * rows + gapY * (rows - 1);
    const int gridX = (screenW_ - usedW) / 2;
    const int gridY = contentTop + std::max(0, (contentH - usedH) / 2);

    const int iconPx = std::max(48, std::min(tileW * 44 / 100, tileH * 42 / 100));

    for (int i = 0; i < per; ++i) {
        const int index = first + i;
        if (index >= int(apps_.size())) break;
        const int col = i % columns;
        const int row = i / columns;
        const double scale = index < int(tileScale_.size()) ? tileScale_[size_t(index)].value : 1.0;
        wm::Rect tile(gridX + col * (tileW + gapX), gridY + row * (tileH + gapY), tileW,
                      tileH);
        tile = scaledAround(tile, scale);

        const bool focused = index == focus_;
        if (focused) {
            compositor_.drawRect(tile, 20.0f, wm::theme::kAccent, opacity);
            const wm::Rect inner(tile.x + 2, tile.y + 2, tile.w - 4, tile.h - 4);
            compositor_.drawRect(inner, 18.0f, wm::theme::kCardTint, opacity);
        } else {
            compositor_.drawRect(tile, 18.0f, wm::theme::kCardTint, opacity * 0.82f);
        }

        TvApp& app = apps_[size_t(index)];
        if (app.icon.valid()) {
            const int ix = tile.x + (tile.w - iconPx) / 2;
            const int iy = tile.y + std::max(14, tileH / 8);
            compositor_.drawTex(app.icon.tex, wm::Rect(ix, iy, iconPx, iconPx), 0.0f,
                                wm::theme::rgb(0xFFFFFF), opacity, true, true);
        }

        const wm::Rect labelBox(tile.x + 8, tile.y + tileH - 54, tile.w - 16, 44);
        drawTextCentered(app.entry.name, std::max(13, screenH_ / 52), wm::Weight::Medium,
                         focused ? wm::theme::kText : wm::theme::kTextIdle, labelBox,
                         opacity);
    }

    const int pages = pageCount();
    if (pages > 1) {
        const int dot = std::max(6, screenH_ / 100);
        const int gap = dot * 2;
        const int total = pages * dot + (pages - 1) * gap;
        int x = (screenW_ - total) / 2;
        const int y = contentBottom - dot * 2;
        for (int p = 0; p < pages; ++p) {
            const wm::Color c = p == page ? wm::theme::kAccent : wm::theme::kTextDim;
            compositor_.drawRect(wm::Rect(x, y, dot, dot), float(dot) / 2.0f, c, opacity);
            x += dot + gap;
        }
    }

    drawHints(opacity);
}

void Manager::drawTopBar(float opacity) {
    const int top = std::max(24, screenH_ / 18);
    const int headerH = std::max(62, screenH_ / 9);
    compositor_.drawGradient(wm::Rect(0, 0, screenW_, top + headerH), 0.0f,
                             wm::theme::rgb(0x000000, 0.55f),
                             wm::theme::rgb(0x000000, 0.0f), opacity);

    drawTextAt("TV", std::max(24, screenH_ / 22), wm::Weight::Bold, wm::theme::kText,
               std::max(28, screenW_ / 45), top + 8, opacity);

    char clock[16] = {};
    char date[32] = {};
    const time_t t = time(nullptr);
    struct tm lt {};
    localtime_r(&t, &lt);
    std::strftime(clock, sizeof(clock), "%H:%M", &lt);
    std::strftime(date, sizeof(date), "%a %d %b", &lt);

    const int clockPx = std::max(20, screenH_ / 24);
    const wm::TextTex ct = text_.get(clock, clockPx, wm::Weight::Bold);
    const int right = std::max(28, screenW_ / 45);
    if (ct.tex)
        compositor_.drawText(ct, wm::Rect(screenW_ - right - ct.w, top + 2, ct.w, ct.h),
                             wm::theme::kText, opacity);
    const wm::TextTex dt = text_.get(date, std::max(12, screenH_ / 60), wm::Weight::Regular);
    if (dt.tex)
        compositor_.drawText(dt,
                             wm::Rect(screenW_ - right - dt.w, top + clockPx + 8, dt.w, dt.h),
                             wm::theme::kTextMuted, opacity);
}

void Manager::drawHints(float opacity) {
    const int h = std::max(36, screenH_ / 18);
    const int y = screenH_ - h;
    compositor_.drawRect(wm::Rect(0, y, screenW_, h), 0.0f,
                         wm::theme::rgb(0x000000, 0.32f), opacity);
    drawTextCentered("A  Select     B  Back     D-pad  Move     LB / RB  Page",
                     std::max(12, screenH_ / 60), wm::Weight::Regular,
                     wm::theme::kTextMuted, wm::Rect(0, y, screenW_, h), opacity);
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
