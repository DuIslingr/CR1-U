#include "gfx/menu_painter.hpp"

#include <algorithm>
#include <cmath>

#include "core/text.hpp"
#include "game/stage_tables.hpp"
#include "gfx/font.hpp"

namespace cr {

namespace {
constexpr float kScale = 2.0f; // menu units -> 1024x768 screen
} // namespace

void MenuPainter::release(Renderer& r) {
    for (auto& [s, c] : text_) r.destroy(c.tex);
    text_.clear();
    r.destroy(white_);
}

void MenuPainter::piece(float x, float y, float u, float v, float w, float h, float alpha, float r, float g, float b) {
    if (!r_ || !sheet_ || !sheet_->id || alpha <= 0.001f) return;
    r_->drawQuadUV(*sheet_, X(x) * kScale, Y(y) * kScale, w * K() * kScale, h * K() * kScale, u * kScale, v * kScale,
                   w * kScale, h * kScale, r, g, b, alpha);
}

void MenuPainter::pieceTo(float x, float y, float w, float h, float u, float v, float uw, float vh, float alpha, float r,
                          float g, float b) {
    if (!r_ || !sheet_ || !sheet_->id || alpha <= 0.001f) return;
    r_->drawQuadUV(*sheet_, X(x) * kScale, Y(y) * kScale, w * K() * kScale, h * K() * kScale, u * kScale, v * kScale,
                   uw * kScale, vh * kScale, r, g, b, alpha);
}

void MenuPainter::image(const Texture& tex, float x, float y, float w, float h, float alpha) {
    if (!r_ || !tex.id || alpha <= 0.001f) return;
    r_->drawQuad(tex, X(x) * kScale, Y(y) * kScale, w * K() * kScale, h * K() * kScale, 1, 1, 1, alpha);
}

void MenuPainter::imagePiece(const Texture& tex, float x, float y, float u, float v, float w, float h, float alpha) {
    if (!r_ || !tex.id || alpha <= 0.001f) return;
    r_->drawQuadUV(tex, X(x) * kScale, Y(y) * kScale, w * K() * kScale, h * K() * kScale, u, v, w, h, 1, 1, 1, alpha);
}

void MenuPainter::picture(int pict, float x, float y, float alpha) {
    if (pict < 0 || pict >= 156) return;
    const short* p = kMenuPicture[pict];
    piece(x, y, p[0], p[1], p[2], p[3], alpha);
}

void MenuPainter::stretch(int pict, float x, float y, float w, float h, float alpha) {
    if (pict < 0 || pict >= 156 || !r_ || !sheet_ || !sheet_->id || alpha <= 0.001f) return;
    const short* p = kMenuPicture[pict];
    r_->drawQuadUV(*sheet_, X(x) * kScale, Y(y) * kScale, w * K() * kScale, h * K() * kScale, p[0] * kScale, p[1] * kScale,
                   p[2] * kScale, p[3] * kScale, 1, 1, 1, alpha);
}

void MenuPainter::quad(float x, float y, float w, float h, const float* left, const float* right) {
    if (!r_) return;
    if (!white_.id) {
        Image img;
        img.width = 2;
        img.height = 1;
        img.rgba = {255, 255, 255, 255, 255, 255, 255, 255};
        white_ = r_->upload(img);
    }
    // A horizontal gradient as narrow strips (the 2D path has no per-vertex colour)
    const int strips = left[3] == right[3] && left[0] == right[0] ? 1 : std::max(1, int(w * kScale / 4));
    for (int i = 0; i < strips; ++i) {
        const float t = (i + 0.5f) / strips;
        float c[4];
        for (int k = 0; k < 4; ++k) c[k] = left[k] + (right[k] - left[k]) * t;
        r_->fillRect(X(x + w * i / strips) * kScale, Y(y) * kScale, w / strips * K() * kScale + 0.01f, h * K() * kScale,
                     c[0], c[1], c[2], c[3]);
    }
}

MenuPainter::Cached& MenuPainter::rasterise(const std::string& s, float size) {
    const std::string key = s + '\x01' + std::to_string(int(size * 10));
    Cached& c = text_[key];
    const float density = std::max(1.0f, std::round(r_->pixelScale() * 4.0f) / 4.0f);
    const int generation = font_ ? font_->generation() : -2;
    if (c.tex.id && c.density == density && c.generation == generation) return c;
    r_->destroy(c.tex);
    c = Cached();
    if (!font_ || !font_->loaded() || s.empty()) return c;
    Image img;
    const float em = size * kScale * density;
    if (!font_->rasterise({s}, em, em * 1.2f, 0, img)) return c;
    c.tex = r_->upload(img);
    c.w = img.width / density;
    c.h = img.height / density;
    c.density = density;
    c.generation = generation;
    return c;
}

void MenuPainter::text(const std::string& s, float x, float y, bool dark, float alpha, float size) {
    if (!r_ || s.empty() || alpha <= 0.001f) return;
    Cached& c = rasterise(s, size * K());
    if (!c.tex.id) return;
    const float k = dark ? 0.0f : 1.0f;
    r_->drawQuad(c.tex, X(x) * kScale, Y(y) * kScale, c.w, c.h, k, k, k, alpha);
}

void MenuPainter::textTint(const std::string& s, float x, float y, float r, float g, float b, float alpha, float size) {
    if (!r_ || s.empty() || alpha <= 0.001f) return;
    Cached& c = rasterise(s, size * K());
    if (!c.tex.id) return;
    r_->drawQuad(c.tex, X(x) * kScale, Y(y) * kScale, c.w, c.h, r, g, b, alpha);
}

void MenuPainter::textCentred(const std::string& s, float x, float y, bool dark, float alpha, float size) {
    if (!r_ || s.empty()) return;
    const Cached& c = rasterise(s, size * K());
    text(s, x, y - c.h / kScale / K() / 2, dark, alpha, size);
}

float MenuPainter::textWidth(const std::string& s, float size) {
    if (!r_ || s.empty()) return 0;
    return rasterise(s, size * K()).w / kScale / K();
}

void MenuPainter::rowBar(float x, float y, const std::string& label, float alpha, bool highlight) {
    const float a = std::min(alpha, 179.0f / 255.0f);
    // Keyboard cursor: the lime the game lights pressed controls with (0xEBFF4B)
    const float r = highlight ? 0xEB / 255.0f : 1, g = 1, b = highlight ? 0x4B / 255.0f : 1;
    const float solid[4] = {r, g, b, a}, clear[4] = {r, g, b, 0};
    quad(x, y, 100, 24, solid, solid);
    quad(x + 100, y, 123, 24, solid, clear);
    textCentred(label, x + 8, y + 12, true, alpha);
}

void MenuPainter::button(float x, float y, int word, const std::string& label, bool selected, float alpha, bool highlight) {
    // Base: left cap, 14 slices, right cap
    piece(x + 4, y, 0, 184, 24, 40, alpha);
    for (int k = 0; k < 14; ++k) piece(x + 28 + 4 * k, y, 28, 184, 4, 40, alpha);
    piece(x + 84, y, 28, 184, 24, 40, alpha);
    // Face: normal or selected strip
    const float r = highlight ? 0xEB / 255.0f : 1, b = highlight ? 0x4B / 255.0f : 1;
    piece(x, y + 4, 996, selected ? 932.0f : 900.0f, 16, 32, alpha, r, 1, b);
    for (int k = 0; k < 20; ++k) piece(x + 16 + 4 * k, y + 4, selected ? 120.0f : 76.0f, 188, 4, 32, alpha, r, 1, b);
    piece(x + 96, y + 4, selected ? 124.0f : 80.0f, 188, 16, 32, alpha, r, 1, b);
    if (word >= 0) {
        piece(x + 10, y + 8, float(word % 5) * 88, 512 + float(word / 5) * 24, 88, 24, alpha);
    } else if (!label.empty()) { // our labels: the game's font, centred on the face
        const float size = textWidth(label, 13) > 84 ? 11.0f : 13.0f;
        // light like the game's words; dark on the lime cursor face
        textCentred(label, x + 56 - textWidth(label, size) / 2, y + 20, highlight, alpha, size);
    }
}

void MenuPainter::plate(float x, float y, int icon, int word, bool selected, float alpha, bool dimmed,
                        const std::string& text) {
    const float a = dimmed ? alpha * 0.45f : alpha;
    piece(x, y, 0, 184, 24, 40, a);
    for (int k = 0; k < 21; ++k) piece(x + 24 + 4 * k, y, 28, 184, 4, 40, a);
    piece(x + 108, y, 28, 184, 24, 40, a);
    piece(x + 28, y, selected ? 96.0f : 52.0f, 184, 24, 40, a);
    for (int k = 0; k < 17; ++k) piece(x + 52 + 4 * k, y, selected ? 120.0f : 76.0f, 184, 4, 40, a);
    piece(x + 120, y, selected ? 124.0f : 80.0f, 184, 16, 40, a);
    if (icon >= 0) piece(x + 8, y + 4, float(icon % 16) * 32, 376 + float(icon / 16) * 32, 32, 32, a);
    if (word >= 0) piece(x + 40, y + 8, float(word % 5) * 88, 512 + float(word / 5) * 24, 88, 24, a);
    else if (!text.empty()) textCentred(text, x + 40 + (88 - textWidth(text, 15)) / 2, y + 20, true, a, 15);
}

void MenuPainter::gauge(float x, float y, int value, float alpha, bool highlight) {
    value = std::clamp(value, 0, 100);
    picture(103, x, y, alpha);
    for (int k = 0; k < 24; ++k) picture(105, x + 20 + 8 * k, y, alpha);
    picture(104, x + 212, y, alpha);
    picture(108, x - 1, y - 13, alpha);
    picture(107, x + 193, y - 13, alpha);
    // The fill (red, 0xFFFF0000) up to the knob at 235 + value x 2.03 (SideGuageInit; menus
    // place gauges at x 224, so it is x + 11 + ...)
    const float knob = x + 11 + std::floor(value * 2.03f);
    const float fill = knob - (x + 14);
    const float red[4] = {1, 0, 0, alpha};
    if (fill > 0) {
        quad(x + 13, y + 6, 1, 2, red, red);
        quad(x + 14, y + 5, fill, 4, red, red);
    }
    const float r = highlight ? 0xEB / 255.0f : 1, b = highlight ? 0x4B / 255.0f : 1;
    const short* p = kMenuPicture[106];
    piece(knob, y - 16, p[0], p[1], p[2], p[3], alpha, r, 1, b);
}

void MenuPainter::backButton(float x, float y, bool pressed, int bob, float alpha) {
    const float lime[4] = {0xEB / 255.0f, 1, 0x4B / 255.0f, alpha};
    const float white[4] = {1, 1, 1, alpha};
    const float* c = pressed ? lime : white;
    const float clear[4] = {c[0], c[1], c[2], 0};
    quad(x + 24, y, 32, 24, clear, c);
    quad(x + 56, y, 120, 24, c, c);
    quad(x + 176, y, 32, 24, c, clear);
    piece(x + 4 + bob, y + 4, 336, 880, 16, 16, alpha);
    piece(x + 197 - bob, y + 4, 352, 880, 16, 16, alpha);
    piece(x + 80, y, 0, 224, 76, 24, alpha);
}

void MenuPainter::backBar(float x, float y, bool pressed, float alpha) {
    const float lime[4] = {0xEB / 255.0f, 1, 0x4B / 255.0f, alpha};
    const float white[4] = {1, 1, 1, alpha};
    const float* c = pressed ? lime : white;
    const float clear[4] = {c[0], c[1], c[2], 0};
    quad(x, y, 24, 24, clear, c);
    quad(x + 24, y, 184, 24, c, c);
    quad(x + 208, y, 24, 24, c, clear);
    piece(x + 82, y, 0, 224, 76, 24, alpha);
}

void MenuPainter::digit(int d, float x, float y, float alpha) {
    if (d < 0) picture(66, x, y, alpha); // "/"
    else piece(x, y, float(d % 10) * 14, 0, 14, 14, alpha);
}

} // namespace cr
