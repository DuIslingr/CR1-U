#include "gfx/hud.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/text.hpp"
#include "gfx/font.hpp"

namespace cr {

namespace {

constexpr int kGlyph = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE; // 8 px
constexpr int kLineGap = 2;
constexpr int kPad = 4;
// TrueType sizes per unit of `scale`: CJK needs more room than the 8x8 cell
constexpr float kEm = 8.5f;
constexpr float kFontLine = 12.0f;

// Rasterises lines (white on transparent) with the software renderer.
bool rasterise(const std::vector<std::string>& utf8, Image& out) {
    std::vector<std::string> lines;
    for (const auto& l : utf8) lines.push_back(foldAscii(l));
    size_t cols = 1;
    for (const auto& l : lines) cols = std::max(cols, l.size());
    const int w = int(cols) * kGlyph + 2 * kPad;
    const int h = int(lines.size()) * (kGlyph + kLineGap) + 2 * kPad;

    SDL_Surface* surf = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    if (!surf) return false;
    SDL_Renderer* sr = SDL_CreateSoftwareRenderer(surf);
    if (!sr) {
        SDL_DestroySurface(surf);
        return false;
    }
    SDL_SetRenderDrawColor(sr, 0, 0, 0, 0);
    SDL_RenderClear(sr);
    SDL_SetRenderDrawColor(sr, 255, 255, 255, 255);
    for (size_t i = 0; i < lines.size(); ++i)
        SDL_RenderDebugText(sr, float(kPad), float(kPad + int(i) * (kGlyph + kLineGap)), lines[i].c_str());
    SDL_FlushRenderer(sr);

    out.width = w;
    out.height = h;
    out.rgba.resize(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        std::memcpy(&out.rgba[size_t(y) * w * 4], static_cast<const uint8_t*>(surf->pixels) + size_t(y) * surf->pitch,
                    size_t(w) * 4);
    SDL_DestroyRenderer(sr);
    SDL_DestroySurface(surf);
    return true;
}

} // namespace

void Hud::setFont(const Font* font) { font_ = font; }

// Re-rasterises when the text, size, font or window density changed. The 8x8 font is drawn
// at `scale`; TrueType text is rasterised at the window's density and drawn at 1 / density.
bool Hud::update(Renderer& r, const std::vector<std::string>& lines, float scale) {
    const bool ttf = font_ && font_->loaded();
    const float density = ttf ? std::max(1.0f, std::round(r.pixelScale() * 4.0f) / 4.0f) : 1.0f;
    const void* fontKey = ttf ? static_cast<const void*>(font_) : nullptr;
    const int generation = ttf ? font_->generation() : -1;
    if (tex_.id && lines == cached_ && scale == cachedScale_ && density == cachedDensity_ && fontKey == cachedFont_ &&
        generation == cachedGeneration_)
        return true;
    Image img;
    const bool ok = ttf ? font_->rasterise(lines, kEm * scale * density, kFontLine * scale * density,
                                           int(std::lround(kPad * scale * density)), img)
                        : rasterise(lines, img);
    if (!ok) return false;
    r.destroy(tex_);
    tex_ = r.upload(img);
    cached_ = lines;
    cachedScale_ = scale;
    cachedDensity_ = density;
    cachedFont_ = fontKey;
    cachedGeneration_ = generation;
    density_ = ttf ? density : 1.0f / scale;
    return true;
}

float Hud::measure(Renderer& r, const std::vector<std::string>& lines, float scale) {
    if (lines.empty() || !update(r, lines, scale)) return 0;
    return tex_.width / density_;
}

float Hud::lineStep(float scale) const {
    return font_ && font_->loaded() ? kFontLine * scale : float(kGlyph + kLineGap) * scale;
}

void Hud::draw(Renderer& r, const std::vector<std::string>& lines, float scale) {
    if (lines.empty() || !update(r, lines, scale)) return;
    const float w = tex_.width / density_, h = tex_.height / density_;
    r.fillRect(0, 0, w, h, 0, 0, 0, 0.55f); // backing so text reads over any model
    r.drawQuad(tex_, 0, 0, w, h);
}

void Hud::drawAt(Renderer& r, const std::vector<std::string>& lines, float x, float y, float scale, float boxW,
                 float boxH, float alpha) {
    if (lines.empty() || !update(r, lines, scale)) return;
    const float w = tex_.width / density_, h = tex_.height / density_;
    if (alpha > 0) r.fillRect(x - 12, y - 10, std::max(w, boxW) + 24, std::max(h, boxH) + 20, 0.02f, 0.03f, 0.08f, alpha);
    r.drawQuad(tex_, x, y, w, h, tint_[0], tint_[1], tint_[2], 1);
}

} // namespace cr
