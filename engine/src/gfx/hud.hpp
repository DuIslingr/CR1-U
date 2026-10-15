#pragma once
// On-screen text overlay. Text is rasterised into an RGBA image, then drawn as a textured
// quad: with a TrueType font when one is set (every language), else with SDL's built-in 8x8
// debug font (ASCII; accents folded) - no font assets needed, the same on every platform.

#include <string>
#include <vector>

#include "gfx/renderer.hpp"

namespace cr {

class Font;

class Hud {
public:
    // Draws `lines` at the top-left of the logical screen. Re-rasterises only on change.
    void draw(Renderer& r, const std::vector<std::string>& lines, float scale = 2.0f);
    // Draws `lines` with their top-left at (x, y) on a backing box at least boxW x boxH.
    void drawAt(Renderer& r, const std::vector<std::string>& lines, float x, float y, float scale, float boxW,
                float boxH, float alpha = 0.75f);
    // Font for the text (nullptr or not loaded: the 8x8 font). Takes effect on the next draw.
    void setFont(const Font* font);
    // Text colour (the rasterised glyphs are white): e.g. dark ink on the parchment window
    void setTint(float r, float g, float b) { tint_[0] = r, tint_[1] = g, tint_[2] = b; }
    void tint(float* out) const { out[0] = tint_[0], out[1] = tint_[1], out[2] = tint_[2]; }
    void destroy(Renderer& r) { r.destroy(tex_); }
    // The width `lines` take at `scale` (rasterising them), and the distance between lines
    float measure(Renderer& r, const std::vector<std::string>& lines, float scale);
    float lineStep(float scale) const;

private:
    bool update(Renderer& r, const std::vector<std::string>& lines, float scale);
    std::vector<std::string> cached_;
    float cachedScale_ = 0, cachedDensity_ = 0;
    float density_ = 1; // texture pixels per logical pixel
    const Font* font_ = nullptr;
    const void* cachedFont_ = nullptr;
    int cachedGeneration_ = -1;
    Texture tex_;
    float tint_[3] = {1, 1, 1};
};

} // namespace cr
