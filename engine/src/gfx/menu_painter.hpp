#pragma once
// The original game's menu pieces (ComUty_* / MuCom*), drawn as the binary draws them: in the
// menus' 512x384 space, doubled onto the 1024x768 screen, from window_hd.mct (Tex2d slot 44,
// its coordinates doubled too) and plain colour (slot 45). Text uses the font the game's text
// uses (gfx/font.hpp) at its size: 14 menu units.

#include <map>
#include <string>

#include "gfx/renderer.hpp"

namespace cr {

class Font;

class MenuPainter {
public:
    void begin(Renderer& r, const Texture& sheet, const Font* font) {
        r_ = &r;
        sheet_ = &sheet;
        font_ = font;
    }
    void release(Renderer& r); // the cached text
    // Everything drawn after this is scaled by k about (x, y) (our denser Options pages draw
    // the game's controls at 85%); k = 1 turns it off
    void setScale(float x, float y, float k) { sx_ = x; sy_ = y; k_ = k; }
    // Moves what's drawn by (dx, dy) menu units (the iPhone layout's corner offsets)
    void setOffset(float dx, float dy) { dx_ = dx; dy_ = dy; }
    // Outside all that: the whole menu scaled by k and moved to (x, y) (fitting the iPad's 512 x 384
    // menus into the iPhone layout's screen)
    void setFit(float x, float y, float k) { fx_ = x; fy_ = y; fk_ = k; }
    // Cuts what's drawn after this to menu rows y0..y1 (MuScrList_Draw's masks over a scrolled
    // list); clearClip ends it
    void setClip(float y0, float y1) {
        if (r_) r_->setClipY(Y(y0) * 2.0f, Y(y1) * 2.0f);
    }
    void clearClip() {
        if (r_) r_->clearClipY();
    }
    // drawfont_l (0x37a20) / drawfont_center (0x3867c): every string drawn 8 times a unit off (+-1, +-1)
    // in the opposite colour - white text edged black, black text edged white - then itself. The
    // battle banner's (0x3783c, drawfont_btl's twin) and drawfont_text (messages) draw it plain:
    // setOutline(false) around those
    void setOutline(bool on) { outline_ = on; }

    // A piece of window_hd at (x, y): its sheet rectangle in menu units (u, v, w, h)
    void piece(float x, float y, float u, float v, float w, float h, float alpha = 1, float r = 1, float g = 1, float b = 1);
    // ... drawn w x h (tex2d_add's own size: the battle HUD's stretched strips)
    void pieceTo(float x, float y, float w, float h, float u, float v, float uw, float vh, float alpha = 1, float r = 1,
                 float g = 1, float b = 1);
    // A whole texture (another MuTex sheet) drawn w x h at (x, y)
    void image(const Texture& tex, float x, float y, float w, float h, float alpha = 1);
    // ... the texture's (u, v, uw, vh) drawn w x h menu units (a sheet with more pixels than units)
    void imagePieceTo(const Texture& tex, float x, float y, float w, float h, float u, float v, float uw, float vh,
                      float alpha = 1, float r = 1, float g = 1, float b = 1);
    // A w x h piece of another texture, in its own (logical) pixels, at (x, y)
    void imagePiece(const Texture& tex, float x, float y, float u, float v, float w, float h, float alpha = 1);
    // Sheet-0 picture `pict` (kMenuPicture) at (x, y) (MuComPictDraw)
    void picture(int pict, float x, float y, float alpha = 1);
    // ... stretched to w x h
    void stretch(int pict, float x, float y, float w, float h, float alpha = 1);
    // Plain colour quad, colours per left / right edge (tex2d_add4 on slot 45)
    void quad(float x, float y, float w, float h, const float* left, const float* right);
    // The same as a parallelogram: the top edge `lean` to the right of the bottom (the battle bars, tex2d_add4)
    void slant(float x, float y, float w, float h, float lean, const float* left, const float* right);
    // Text with its top-left at (x, y), 14 units high; dark = black (style 1), else white
    void text(const std::string& s, float x, float y, bool dark, float alpha = 1, float size = 14);
    float textWidth(const std::string& s, float size = 14);
    // ... in a colour (MuComFontPlateDisp's colours: the plates' yellow, grey)
    void textTint(const std::string& s, float x, float y, float r, float g, float b, float alpha = 1, float size = 14);
    // Text whose line box is centred on y (labels in bars and on buttons: this font's box is
    // taller than the iOS system font's, so a fixed top offset would sit low)
    void textCentred(const std::string& s, float x, float y, bool dark, float alpha = 1, float size = 14);

    // ComUty_OptCategoryDraw: a 24-tall white bar (100 solid, 123 fading out, alpha up to
    // 0xB3) with the label in black at (x + 8, y + 5). `highlight` tints the bar (keyboard
    // cursor - an addition: the original is touch-only)
    void rowBar(float x, float y, const std::string& label, float alpha, bool highlight);
    // ComUty_CmdBottom: a button at (x, y), 104 x 40 base, 112 x 32 face; the label is word
    // picture `word` (5 x 88 x 24 grid at (0, 512)) or, when word < 0, `text`
    void button(float x, float y, int word, const std::string& text, bool selected, float alpha, bool highlight);
    // __ComUty_CmdBottom (ComUty_ChoicePlateDraw, the title menu): a 132-wide plate at (x, y),
    // a 32 x 32 icon (kanIconNoTbl - 1; 16 per row from (0, 376)) and word picture `word`
    // (no word picture for it: `text` instead)
    void plate(float x, float y, int icon, int word, bool selected, float alpha, bool dimmed,
               const std::string& text = std::string());
    // ComUty_SideGuageDraw: the track at (x, y), the fill and knob for `value` 0..100
    void gauge(float x, float y, int value, float alpha, bool highlight);
    // ComUty_CmdBack type 2: the Back bar at (x, y), `pressed` lights it, `bob` 0..5 moves the arrows
    void backButton(float x, float y, bool pressed, int bob, float alpha);
    // ComUty_CmdBack type 1 (the shop's): a plain 232-wide bar, white fading in over 24 at each end,
    // "Back" at x + 82
    void backBar(float x, float y, bool pressed, float alpha);
    // ComUty_CmdBack type 3: Back (x .. x + 114, faded in over 24, the word at + 36) and OK (x + 118 .. x + 208,
    // fading out to 232, its word (sheet (76, 224) 64 x 20) at (+126, +2)); `pressed` 1 lights Back, 5 OK
    void backOkBar(float x, float y, int pressed, float alpha);
    // The 14 x 14 digits of sheet 0 (0 at (0, 0), then every 14 units) and "/" (picture 66)
    void digit(int d, float x, float y, float alpha = 1);

private:
    float sx_ = 0, sy_ = 0, k_ = 1, dx_ = 0, dy_ = 0, fx_ = 0, fy_ = 0, fk_ = 1;
    bool outline_ = true;
    void edge(const Texture& tex, float x, float y, float w, float h, float k, float alpha);
    float X(float x) const { return fx_ + (sx_ + (x - sx_) * k_ + dx_) * fk_; }
    float Y(float y) const { return fy_ + (sy_ + (y - sy_) * k_ + dy_) * fk_; }
    float K() const { return k_ * fk_; }
    Renderer* r_ = nullptr;
    const Texture* sheet_ = nullptr;
    const Font* font_ = nullptr;
    struct Cached {
        Texture tex;
        float w = 0, h = 0, density = 0; // logical size, texture pixels per logical pixel
        int generation = -1;
    };
    std::map<std::string, Cached> text_;
    Texture white_;
    Cached& rasterise(const std::string& s, float size);
};

} // namespace cr
