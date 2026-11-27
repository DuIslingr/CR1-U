#include "gfx/menu_painter.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "core/text.hpp"
#include "game/stage_tables.hpp"
#include "gfx/font.hpp"
#include "gfx/font_outline.hpp"

namespace cr {

namespace {
constexpr float kScale = 2.0f; // menu units -> 1024x768 screen
} // namespace

void MenuPainter::release(Renderer& r) {
    for (auto& [s, c] : text_) {
        r.destroy(c.tex);
        r.destroy(c.edgeTex);
    }
    text_.clear();
    for (auto& [key, tex] : gradients_) r.destroy(tex);
    gradients_.clear();
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
    if (!r_ || w <= 0 || h <= 0) return;
    const float opacity = std::clamp(std::max(left[3], right[3]), 0.0f, 1.0f);
    if (opacity <= 0.001f) return;
    const auto byte = [](float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    // The colours and the alphas relative to the brighter end: a fade's alpha is the tint, so a fading
    // strip keeps one texture
    const std::array<uint8_t, 8> key = {byte(left[0]),  byte(left[1]),  byte(left[2]),
                                        byte(right[0]), byte(right[1]), byte(right[2]),
                                        byte(left[3] / opacity), byte(right[3] / opacity)};
    auto it = gradients_.find(key);
    if (it == gradients_.end()) {
        // One 1024-sample ramp, linearly filtered, drawn as one quad (metastigma's, CRO-U-metastigma-0.7.4
        // 33c4788): the strips this drew before - a few pixels each - left hairline seams between them
        if (gradients_.size() >= 256) { // (an animated colour would make a ramp a frame: start the cache over)
            for (auto& [k, tex] : gradients_) r_->destroy(tex);
            gradients_.clear();
        }
        constexpr int kRamp = 1024;
        Image img;
        img.width = kRamp;
        img.height = 1;
        img.rgba.resize(size_t(kRamp) * 4);
        for (int i = 0; i < kRamp; ++i) {
            const float t = float(i) / float(kRamp - 1);
            uint8_t* px = img.rgba.data() + size_t(i) * 4;
            for (int c = 0; c < 3; ++c) px[c] = byte((key[size_t(c)] * (1 - t) + key[size_t(c + 3)] * t) / 255.0f);
            px[3] = byte((key[6] * (1 - t) + key[7] * t) / 255.0f);
        }
        Texture tex = r_->upload(img);
        if (!tex.id) return;
        it = gradients_.emplace(key, tex).first;
    }
    r_->drawQuad(it->second, X(x) * kScale, Y(y) * kScale, w * K() * kScale, h * K() * kScale, 1, 1, 1, opacity);
}

void MenuPainter::slant(float x, float y, float w, float h, float lean, const float* left, const float* right) {
    if (!r_) return;
    const int strips = left[3] == right[3] && left[0] == right[0] && left[1] == right[1] && left[2] == right[2]
                           ? 1 : std::max(1, int(w * kScale / 4));
    for (int i = 0; i < strips; ++i) {
        const float t = (i + 0.5f) / strips;
        float c[4];
        for (int k = 0; k < 4; ++k) c[k] = left[k] + (right[k] - left[k]) * t;
        const float x0 = x + w * i / strips, x1 = x + w * (i + 1) / strips + (i + 1 < strips ? 0.01f : 0.0f);
        const float xy[8] = {X(x0 + lean) * kScale, Y(y) * kScale,     X(x1 + lean) * kScale, Y(y) * kScale,
                             X(x1) * kScale,        Y(y + h) * kScale, X(x0) * kScale,        Y(y + h) * kScale};
        r_->fillQuad(xy, c[0], c[1], c[2], c[3]);
    }
}

MenuPainter::Cached& MenuPainter::rasterise(const std::string& s, float size) {
    const float density = std::max(1.0f, std::round(r_->pixelScale() * 4.0f) / 4.0f);
    // The fixed edge is one menu unit wide: rasterised with that transparent border at the pixel
    // density, the glyph's alpha dilated into one antialiased outline (metastigma's, CRO-U 109c216)
    const int pad = cleanOutline_ && outline_ ? std::max(1, int(std::ceil(kScale * K() * density))) : 0;
    const std::string key = s + '\x01' + std::to_string(int(size * 10)) + '\x02' + std::to_string(pad);
    Cached& c = text_[key];
    const int generation = font_ ? font_->generation() : -2;
    if (c.tex.id && c.density == density && c.generation == generation && c.font == font_ && (pad == 0 || c.edgeTex.id))
        return c;
    r_->destroy(c.tex);
    r_->destroy(c.edgeTex);
    c = Cached();
    if (!font_ || !font_->loaded() || s.empty()) return c;
    Image img;
    const float em = size * kScale * density;
    if (!font_->rasterise({s}, em, em * 1.2f, pad, img)) return c;
    c.tex = r_->upload(img);
    c.pad = float(pad) / density;
    c.w = float(img.width - 2 * pad) / density;
    c.h = float(img.height - 2 * pad) / density;
    c.density = density;
    c.generation = generation;
    c.font = font_;
    if (pad > 0) {
        Image outline;
        if (makeGlyphOutline(img, pad, outline)) c.edgeTex = r_->upload(outline);
    }
    return c;
}

// Original mode: the eight copies a unit off in grey level k (drawfont_l 0x37a20: tex2d_add at x -1..+1, y -1..+1)
void MenuPainter::edge(const Texture& tex, float x, float y, float w, float h, float k, float alpha) {
    if (!outline_) return;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
            if (dx || dy) r_->drawQuad(tex, X(x + float(dx)) * kScale, Y(y + float(dy)) * kScale, w, h, k, k, k, alpha);
}

void MenuPainter::text(const std::string& s, float x, float y, bool dark, float alpha, float size) {
    if (!r_ || s.empty() || alpha <= 0.001f) return;
    Cached& c = rasterise(s, size * K());
    if (!c.tex.id) return;
    const float x0 = X(x) * kScale - c.pad, y0 = Y(y) * kScale - c.pad;
    const float w = c.w + 2 * c.pad, h = c.h + 2 * c.pad;
    const float k = dark ? 0.0f : 1.0f;
    if (outline_ && cleanOutline_ && c.edgeTex.id) r_->drawQuad(c.edgeTex, x0, y0, w, h, 1 - k, 1 - k, 1 - k, alpha);
    else if (!cleanOutline_) edge(c.tex, x, y, c.w, c.h, 1 - k, alpha);
    r_->drawQuad(c.tex, x0, y0, w, h, k, k, k, alpha);
}

void MenuPainter::textTint(const std::string& s, float x, float y, float r, float g, float b, float alpha, float size) {
    if (!r_ || s.empty() || alpha <= 0.001f) return;
    Cached& c = rasterise(s, size * K());
    if (!c.tex.id) return;
    const float x0 = X(x) * kScale - c.pad, y0 = Y(y) * kScale - c.pad;
    const float w = c.w + 2 * c.pad, h = c.h + 2 * c.pad;
    const float e = r + g + b > 0.01f ? 0.0f : 1.0f; // (a colour: edged black)
    if (outline_ && cleanOutline_ && c.edgeTex.id) r_->drawQuad(c.edgeTex, x0, y0, w, h, e, e, e, alpha);
    else if (!cleanOutline_) edge(c.tex, x, y, c.w, c.h, e, alpha);
    r_->drawQuad(c.tex, x0, y0, w, h, r, g, b, alpha);
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

void MenuPainter::rowBar(float x, float y, const std::string& label, float alpha, bool highlight, float shade, float cap) {
    const float a = std::min(alpha, cap);
    // Keyboard cursor: the cyan the game lights pressed controls with (tex2d colour 0xEBFF4B, A B G R:
    // R 0x4B, G 0xFF, B 0xEB - ComUty_CmdBack 0x4d4d0's pressed colour)
    const float r = highlight ? 0x4B / 255.0f : shade, g = highlight ? 1 : shade, b = highlight ? 0xEB / 255.0f : shade;
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
    const float r = highlight ? 0x4B / 255.0f : 1, b = highlight ? 0xEB / 255.0f : 1;
    piece(x, y + 4, 996, selected ? 932.0f : 900.0f, 16, 32, alpha, r, 1, b);
    for (int k = 0; k < 20; ++k) piece(x + 16 + 4 * k, y + 4, selected ? 120.0f : 76.0f, 188, 4, 32, alpha, r, 1, b);
    piece(x + 96, y + 4, selected ? 124.0f : 80.0f, 188, 16, 32, alpha, r, 1, b);
    if (word >= 0) {
        piece(x + 10, y + 8, float(word % 5) * 88, 512 + float(word / 5) * 24, 88, 24, alpha);
    } else if (!label.empty()) { // our labels: the game's font, centred on the face
        // (13, smaller to fit the face's 84: a long value - a fan translation's name - inside its button, CRO-U #19)
        const float wide = textWidth(label, 13), size = wide > 84 ? std::max(8.0f, std::floor(13.0f * 84 / wide)) : 13.0f;
        // light like the game's words; dark on the cyan cursor face
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

void MenuPainter::gauge(float x, float y, int value, float alpha, bool active, int fillAt) {
    value = std::clamp(value, 0, 100);
    picture(103, x, y, alpha);
    for (int k = 0; k < 24; ++k) picture(105, x + 20 + 8 * k, y, alpha);
    picture(104, x + 212, y, alpha);
    picture(108, x - 1, y - 13, alpha);
    picture(107, x + 193, y - 13, alpha);
    // The fill (0xFFFF0000: tex2d's A B G R - blue, as Omega's Part 8 4927.3 s shows) up to the knob at 235 + value x 2.03 (SideGuageInit; menus
    // place gauges at x 224, so it is x + 11 + ...)
    const float knob = x + 11 + std::floor(value * 2.03f);
    const float fill = (fillAt >= 0 ? x + 11 + float(fillAt) : knob) - (x + 14);
    // (the Vita's cursor gauge - _ComUty_SideGuageDraw's fourth argument from OptionPut - fills light cyan, 0xffffff80)
    const float colour[4] = {active ? 0x80 / 255.0f : 0, active ? 1.0f : 0, 1, alpha};
    if (fill > 0) {
        quad(x + 13, y + 6, 1, 2, colour, colour);
        quad(x + 14, y + 5, fill, 4, colour, colour);
    }
    const short* p = kMenuPicture[106]; // (the knob as it is: neither 2.2.0 nor the Vita lights it)
    piece(knob, y - 16, p[0], p[1], p[2], p[3], alpha);
}

void MenuPainter::backButton(float x, float y, int pressed, int bob, float alpha) {
    const float cyan[4] = {0x4B / 255.0f, 1, 0xEB / 255.0f, alpha}; // (tex2d's 0xEBFF4B is A B G R: cyan)
    const float white[4] = {1, 1, 1, alpha};
    // ComUty_CmdBack mode 2: the left arrow's box (24..51, faded in over 0..24), Back (56..176), the
    // right arrow's box (181..208, fading out to 232); the arrows bob inward (0..5). The part held
    // tints cyan (2.2.0 0x4d4d0: 1 Back, 2 the right arrow's box, 3 the left's - with their fades)
    const float* c = pressed == 1 ? cyan : white;
    const float* l = pressed == 3 ? cyan : white;
    const float* r = pressed == 2 ? cyan : white;
    const float clearL[4] = {l[0], l[1], l[2], 0}, clearR[4] = {r[0], r[1], r[2], 0};
    quad(x, y, 24, 24, clearL, l);
    quad(x + 24, y, 27, 24, l, l);
    quad(x + 56, y, 120, 24, c, c);
    quad(x + 181, y, 27, 24, r, r);
    quad(x + 208, y, 24, 24, r, clearR);
    piece(x + 24 + bob, y + 4, 336, 880, 16, 16, alpha);
    piece(x + 197 - bob, y + 4, 352, 880, 16, 16, alpha);
    piece(x + 80, y, 0, 224, 76, 24, alpha);
}

void MenuPainter::backBar(float x, float y, bool pressed, float alpha) {
    const float cyan[4] = {0x4B / 255.0f, 1, 0xEB / 255.0f, alpha}; // (tex2d's 0xEBFF4B is A B G R: cyan)
    const float white[4] = {1, 1, 1, alpha};
    const float* c = pressed ? cyan : white;
    const float clear[4] = {c[0], c[1], c[2], 0};
    quad(x, y, 24, 24, clear, c);
    quad(x + 24, y, 184, 24, c, c);
    quad(x + 208, y, 24, 24, c, clear);
    piece(x + 82, y, 0, 224, 76, 24, alpha);
}

void MenuPainter::imagePieceTo(const Texture& tex, float x, float y, float w, float h, float u, float v, float uw, float vh,
                               float alpha, float r, float g, float b) {
    if (!r_ || !tex.id || alpha <= 0.001f) return;
    r_->drawQuadUV(tex, X(x) * kScale, Y(y) * kScale, w * K() * kScale, h * K() * kScale, u, v, uw, vh, r, g, b, alpha);
}

void MenuPainter::backOkBar(float x, float y, int pressed, float alpha) {
    const float cyan[4] = {0x4B / 255.0f, 1, 0xEB / 255.0f, alpha}; // (tex2d's 0xEBFF4B is A B G R: cyan)
    const float white[4] = {1, 1, 1, alpha};
    const float* b = pressed == 1 ? cyan : white;
    const float* o = pressed == 5 ? cyan : white;
    const float clearB[4] = {b[0], b[1], b[2], 0}, clearO[4] = {o[0], o[1], o[2], 0};
    quad(x, y, 24, 24, clearB, b);
    quad(x + 24, y, 90, 24, b, b);
    quad(x + 118, y, 90, 24, o, o);
    quad(x + 208, y, 24, 24, o, clearO);
    piece(x + 36, y, 0, 224, 76, 24, alpha);
    piece(x + 126, y + 2, 76, 224, 64, 20, alpha);
}

void MenuPainter::digit(int d, float x, float y, float alpha) {
    if (d < 0) picture(66, x, y, alpha); // "/"
    else piece(x, y, float(d % 10) * 14, 0, 14, 14, alpha);
}

} // namespace cr
