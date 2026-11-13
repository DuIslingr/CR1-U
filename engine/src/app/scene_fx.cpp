#include "app/app.hpp"

namespace crapp {

// ---- Scene effects (ScnEffectRead / ScnEffectStart / ScnEffectProc) ----
// A dungeon's weather: call 2020 loads its set's textures into Tex2d slots 49..72 (the "(128)"
// copies are the battles'), call 2009 turns effect n on or off. Every frame each effect on draws
// its layers: 512 x 384 windows of a wrapping texture over the whole screen (tex2d_add at
// (-16, -32) in the iPhone's 480 x 320 space, which the iPad centres in its 512 x 384), one texel
// to a menu unit, moving by fixed steps each frame and by half the field's scroll the other way;
// some layers' alpha breathes (UpdateRandom: fade in to 255, hold a random while, fade out,
// hold). In battles (G+0x2c0 bit 5 instead of the field's bit 6) the weather uses the "(128)"
// copies, each in the slot after its own, with no field scroll. Effects 6..8 / 12..20 are the
// tower's Space backdrop (ScnEffProc_Space_Stop / Up / Down, variants 0..2): set 4's 512 x 1024
// paintings drawn screen-fixed at the back (z -1998), scrolling up or down, the later variants
// with their fog drifting both ways over the top 256 rows. Effect 1 (a blinking overlay from
// slot 47, which nothing found loads) isn't drawn.
namespace scenefx {
struct SetTexture { int set; const char* file; int slot; };
constexpr SetTexture kSetTextures[] = {
    {0, "d0101_snow", 61}, {0, "d0101_fog", 64}, {0, "d0117_fog", 51},
    {0, "d0101_snow(128)", 62}, {0, "d0101_fog(128)", 65}, {0, "d0117_fog(128)", 52},
    {1, "d0201_sun", 69}, {1, "d0209_fog", 53}, {1, "d0209_fog(128)", 54},
    {2, "d0307_fog", 49}, {2, "d0307_fog(128)", 50},
    {3, "d0401_rain01", 55}, {3, "d0401_rain02", 57}, {3, "d0401_fog", 59},
    {3, "d0401_rain01(128)", 56}, {3, "d0401_rain02(128)", 58}, {3, "d0401_fog(128)", 60},
    {4, "d0501-b00a00", 66}, {4, "d0507-b00a00", 67}, {4, "d0511-b00a00", 68}, {4, "d0507_fog", 71}, {4, "d0511_fog", 72},
    {5, "p0001_fog", 70},
};
// UpdateRandom's channels as each effect sets them up (marker = the effect that did)
struct ChannelInit { int inc, fadeIn, fadeOut, hold2, alpha; };
} // namespace scenefx

void loadSceneEffectSet(App& app, int set) {
    App::SceneFx& fx = app.fx;
    set = std::clamp(set, 0, 5);
    if (fx.set == set) return;
    for (cr::Texture& t : fx.tex) app.renderer.destroy(t);
    fx.set = set;
    for (const auto& st : scenefx::kSetTextures) {
        if (st.set != set) continue;
        cr::Image img;
        if (loadImage(app, std::string(st.file) + ".mct", img)) fx.tex[size_t(st.slot - 49)] = app.renderer.upload(img);
    }
}

void endSceneEffects(App& app) { // ScnEffectEnd (and the textures with it)
    for (cr::Texture& t : app.fx.tex) app.renderer.destroy(t);
    const uint32_t rng = app.fx.rng;
    app.fx = App::SceneFx();
    app.fx.rng = rng;
}

void setSceneEffect(App& app, bool on, int id) { // ScnEffectStart / ScnEffectStop
    if (id < 0 || id >= 21) return;
    App::SceneFx& fx = app.fx;
    if (on && !fx.on[size_t(id)]) { fx.timerA[size_t(id)] = fx.timerB[size_t(id)] = 0; }
    fx.on[size_t(id)] = on;
}

// The effect's channels, set up the first time it runs (its marker)
void initSceneChannels(App& app, int id) {
    using scenefx::ChannelInit;
    App::SceneFx& fx = app.fx;
    const int marker = id == 9 ? 2 : id; // effect 9 shares effect 2's setup
    std::vector<ChannelInit> init;
    int holdMin = 60, holdMax = 120;
    if (marker == 2) init = {{3, 40, 40, 60, 135}, {3, 40, 40, 60, 135}};
    else if (marker == 4) init = {{5, 40, 40, 30, 55}, {3, 40, 40, 30, 135}};
    else if (marker == 5) { init = {{5, 40, 40, 30, 55}, {1, 120, 120, 60, 135}, {1, 120, 120, 60, 135}}; holdMin = 120; holdMax = 240; }
    else return;
    bool any = false;
    for (size_t c = 0; c < init.size(); ++c) {
        App::SceneFx::Channel& ch = fx.ch[c];
        if (ch.marker == marker) continue;
        any = true;
        ch = App::SceneFx::Channel();
        ch.marker = marker;
        ch.inc = init[c].inc;
        ch.fadeIn = init[c].fadeIn;
        ch.fadeOut = init[c].fadeOut;
        ch.hold2 = init[c].hold2;
        ch.alpha = init[c].alpha;
    }
    if (any) { fx.holdMin = holdMin; fx.holdMax = holdMax; }
}

// UpdateRandom: channels 0 and 1 (2 keeps its starting alpha)
void updateSceneChannels(App& app) {
    App::SceneFx& fx = app.fx;
    for (int c = 0; c < 2; ++c) {
        App::SceneFx::Channel& ch = fx.ch[c];
        switch (ch.state) {
        case 0:
            ch.alpha = (ch.alpha + ch.inc) & 255;
            if (++ch.count == ch.fadeIn) {
                ch.count = 0;
                ch.state = 1;
                fx.rng = fx.rng * 1103515245u + 12345u;
                const int span = std::max(1, fx.holdMax - fx.holdMin);
                ch.holdLen = int((fx.rng >> 16) % uint32_t(span)) + fx.holdMin;
            }
            break;
        case 1: if (++ch.count == ch.holdLen) { ch.count = 0; ch.state = 2; } break;
        case 2:
            ch.alpha = (ch.alpha - ch.inc) & 255;
            if (++ch.count == ch.fadeOut) { ch.count = 0; ch.state = 3; }
            break;
        default: if (++ch.count == ch.hold2) { ch.count = 0; ch.state = 0; } break;
        }
    }
}

// One 20 Hz frame of every effect on: the channels, then the counters (as ScnEffectProc: drawn first)
// Effects 2..5, 9 and 11 run (and draw) only with the Options' Environment Effects on
// (__GetScreenEffectState, G+0x31dd4 - the option's +0x37); so do the tower's fogs
bool weatherEffect(int id) { return (id >= 2 && id <= 5) || id == 9 || id == 11; }

void stepSceneEffects(App& app) {
    App::SceneFx& fx = app.fx;
    for (int id = 0; id < 21; ++id) {
        if (!fx.on[size_t(id)]) continue;
        if (!app.environmentEffects && weatherEffect(id)) continue;
        int& a = fx.timerA[size_t(id)];
        int& b = fx.timerB[size_t(id)];
        switch (id) {
        case 1: a ^= 1; break; // the concentration lines: every other frame (G + 0x184)
        case 2: initSceneChannels(app, 2); updateSceneChannels(app); if (++a > 512) a = 0; break;
        case 3: if (++a > 400) a = 0; if (++b > 440) b = 0; break;
        case 4: initSceneChannels(app, 4); updateSceneChannels(app); if (++a > 200) a = 0; break;
        case 5: initSceneChannels(app, 5); updateSceneChannels(app); if (++a > 79) a = 0; if (++b >= 80) b = 0; break;
        case 9: initSceneChannels(app, 9); updateSceneChannels(app); if (++a > 120) a = 0; if (++b >= 181) b = 0; break;
        case 11: if (++a >= 241) a = 0; break;
        case 6: case 12: case 15: case 18: // Space_Stop
        case 7: case 13: case 16: case 19: // Space_Up
        case 8: case 14: case 17: case 20: { // Space_Down
            const int kind = id <= 8 ? id - 6 : (id - 12) % 3;
            if (kind == 1 && ++fx.spaceRise > 2000) fx.spaceRise = 0;
            if (kind == 2 && --fx.spaceRise > 2000) fx.spaceRise = 0;
            if (++fx.spaceDrift > 480) fx.spaceDrift = 0;
            break;
        }
        default: break;
        }
    }
}

// The Space backdrop's variant for effect id (0: d0501, 1: d0507, 2: d0511), -1 not a Space one
int spaceVariant(int id) {
    if (id >= 6 && id <= 8) return 0;
    if (id >= 12 && id <= 14) return 1;
    if (id >= 15 && id <= 20) return 2;
    return -1;
}


void drawSpaceBackdrop(App& app, bool battle) {
    const App::SceneFx& fx = app.fx;
    if (fx.set != 4) return;
    const float sx = battle ? 0.0f : app.scroll.offX * 0.5f;
    cr::Renderer& r = app.renderer;
    r.setOrigin(r.uiX());
    for (int id = 0; id < 21; ++id) {
        const int k = spaceVariant(id);
        if (k < 0 || !fx.on[size_t(id)]) continue;
        const float drift = float(fx.spaceDrift) * 1.066f;
        drawSceneLayer(app, 66 + k, -16, -32, 512, 384, 0, float(fx.spaceRise) * 1.024f, 255);
        if (k > 0 && app.environmentEffects) { // its fog, both ways
            drawSceneLayer(app, 70 + k, -16, -32, 512, 256, drift - sx, 0, 255);
            drawSceneLayer(app, 70 + k, -16, -32, 512, 256, -drift - sx, 0, 255);
        }
    }
}

// A layer: the texture's (u, v) window, wrapping, over menu rectangle (x, y, w, h) (tex2d units:
// 480 x 320 space, so +16 / +32 to the 512 x 384 menu units, doubled on screen); the screen's
// extra width (widescreen) gets more of the texture
void drawSceneLayer(App& app, int slot, float x, float y, float w, float h, float u, float v, int alpha) {
    const cr::Texture& t = app.fx.tex[size_t(std::clamp(slot - 49, 0, 23))];
    if (!t.id || alpha <= 0) return;
    cr::Renderer& r = app.renderer;
    const float extra = r.uiX() / 2; // menu units beyond the 512 at each side
    float mx = x + 16, my = y + 32;
    if (w >= 512) { mx -= extra; u -= extra; w += 2 * extra; }
    const float tw = float(t.width), th = float(t.height);
    const float a = float(alpha) / 255.0f;
    // Tiles of the texture's period: up to a 3 x 3 grid for a screen-sized window
    const float u0 = u - std::floor(u / tw) * tw, v0 = v - std::floor(v / th) * th;
    for (float dy = 0; dy < h;) {
        const float vv = std::fmod(v0 + dy, th), sh = std::min(th - vv, h - dy);
        for (float dx = 0; dx < w;) {
            const float uu = std::fmod(u0 + dx, tw), sw = std::min(tw - uu, w - dx);
            r.drawQuadUV(t, (mx + dx) * 2, (my + dy) * 2, sw * 2, sh * 2, uu, vv, sw, sh, 1, 1, 1, a);
            dx += sw;
        }
        dy += sh;
    }
}

// ScnEffectProc's draws, by effect number; sx, sy = half the field's scroll. In a battle the
// weather (effects 2..5, 9) draws the "(128)" copy in the next slot, and nothing scrolls
void drawSceneEffects(App& app, bool battle) {
    const App::SceneFx& fx = app.fx; // (effect 1 needs no set read)
    const float sx = battle ? 0.0f : app.scroll.offX * 0.5f, sy = battle ? 0.0f : app.scroll.offY * 0.5f;
    const int bo = battle ? 1 : 0; // the "(128)" copies' slot
    cr::Renderer& r = app.renderer;
    r.setOrigin(r.uiX());
    for (int id = 0; id < 21; ++id) {
        if (!fx.on[size_t(id)]) continue;
        if (!app.environmentEffects && weatherEffect(id)) continue;
        const float a = float(fx.timerA[size_t(id)]), b = float(fx.timerB[size_t(id)]);
        const int ch0 = fx.ch[0].alpha, ch1 = fx.ch[1].alpha, ch2 = fx.ch[2].alpha;
        switch (id) {
        case 1: { // syuutyuusen's concentration lines on alternate frames: four mirrored quarters
            // (_bkanTexUv: each tex2d (x - 16, y - 32) its own (u, v) and size, z -50)
            if (!(fx.timerA[1] & 1)) break;
            if (!app.speedLinesTex.id) {
                cr::Image img;
                if (loadImage(app, "syuutyuusen_hd.mct", img)) app.speedLinesTex = app.renderer.upload(img);
            }
            if (!app.speedLinesTex.id) break;
            static const int kUv[4][4] = {{0, 0, 256, 192}, {256, 0, -256, 192}, {256, 192, -256, -192}, {0, 192, 256, -192}};
            const float k = float(app.speedLinesTex.width) / 256.0f, extra = r.uiX(); // (widescreen: the outer halves stretched over the sides)
            for (const auto& q : kUv) // (a negative size flips the uv only)
                r.drawQuadUV(app.speedLinesTex, q[0] ? 512.0f : -extra, 2.0f * float(q[1]), 512 + extra, 384, q[0] * k, q[1] * k, q[2] * k,
                             q[3] * k);
            break;
        }
        case 2: // d0307_fog: two layers, breathing
            drawSceneLayer(app, 49 + bo, -16, -32, 512, 384, a - sx, 0.5f * a - sy, ch0);
            drawSceneLayer(app, 49 + bo, -16, -32, 512, 384, a + 128 - sx, 384 - 0.5f * a - sy, ch1);
            break;
        case 3: // d0117_fog: two layers drifting apart
            drawSceneLayer(app, 51 + bo, -16, -32, 512, 384, -0.64f * a - sx, 0.64f * a - sy, 255);
            drawSceneLayer(app, 51 + bo, -16, -32, 512, 384, 0.5818182f * b - sx, 0.5818182f * b - sy, 255);
            break;
        case 4: // rain: two sheets of streaks, then fog
            drawSceneLayer(app, 55 + bo, -16, -32, 512, 384, 0, -51.2f * a - sy, ch0);
            drawSceneLayer(app, 57 + bo, -16, -32, 512, 384, -13.251f * a - sx, -49.455f * a - sy, ch1);
            drawSceneLayer(app, 59 + bo, -16, -32, 512, 384, -1.28f * a - sx, 1.28f * a - sy, 255);
            break;
        case 5: // snow: d0101_fog, then two sheets of flakes
            drawSceneLayer(app, 64 + bo, -16, -32, 512, 384, -6.4f * a - sx, 3.2f * a - sy, ch0);
            drawSceneLayer(app, 61 + bo, -16, -32, 512, 384, -12.8f * b - sx, -12.8f * b - sy, ch1);
            drawSceneLayer(app, 61 + bo, -16, -32, 512, 384, 128 - 6.4f * b - sx, -6.4f * b - sy, ch2);
            break;
        case 9: // d0209_fog: two layers, breathing
            drawSceneLayer(app, 53 + bo, -16, -32, 512, 384, -2.1333333f * a - sx, -2.1333333f * a - sy, ch0);
            drawSceneLayer(app, 53 + bo, -16, -32, 512, 384, 128 - 1.4222222f * b - sx, -1.4222222f * b - sy, ch1);
            break;
        case 10: drawSceneLayer(app, 69, -16, -32, 512, 320, 0, 0, 255); break; // the sun's glare
        case 11: // p0001_fog, the puzzle rooms' clouds: (-16, 128) 512 x 256 on the iPad (1.0.1 HD); off the
            // iPad the band's top at tex2d y 64 - 2.2.0's _ScnEffectProc 0x63424, Android 1.1.1's 64 - c9a3a
            // (FUN_0003dd98; c9a3a = -oy / scale): the texture's dense bottom on the screen's
            drawSceneLayer(app, 70, -16, r.phoneLayout() ? 64 + androidEdgeY(app) : 128.0f, 512, 256, 2.1333333f * a - sx, 0, 255);
            break;
        default: break;
        }
    }
}

// ---- The field camera's scroll (FieldCamera::setScrollPlayer / setScroll2D / setScroll3D, ScrollAutoCtl) ----
// fldread_prc doubles each field's view on the iPad (the stage record's viewW / viewH and the
// painting's rect), so a 1024 x 1024 painting covers 2048 x 2048 screen pixels, two to a texel.
// FieldCamera::setCamera3D puts the 3D viewport (the view) at the screen's centre plus the
// offset. With the camera on the player (CameraPlayer, FieldScene::updateCamera) the offset
// centres the player's head (position + 7 + record +70) every frame, within the painting's rect;
// with it free (CameraFree) the scripts move it: FrmAdjCam slides to a shot's (x, y), FrmPanCam
// slides by twice its (dx, dy), FrmSetCam centres the view (SetScroll 0, 0: on the field it is a 3D
// camera op), Mapscroll centres an NPC or a view pixel.
bool paintedField(const App& app) {
    return app.mode == Mode::Maps && app.showBackground && app.background.id && app.haveStageCam;
}
bool scrollingField(const App& app) { return paintedField(app); }
// The stage view on the screen (FieldCamera::setCameraBack / setCamera3D's viewport): the record's
// view doubled (fldread_prc - the iPad, the Vita always, the iPhone on Retina), that many of the
// device's pixels - so on the 768-tall logical screen, x 768 / the device's height: the iPad's 768
// (x 1), the iPhone's Retina 640 (x 1.2), the Android release's 720 (x 16/15), the Vita's 544
// (the Vita copy as the game, its own layout: x 768 / 544)
bool vitaLayout(const App& app) { return app.vitaOnly && app.video.layout == 0; } // (the Vita copy as the game)
float fieldViewScale(const App& app) {
    if (vitaLayout(app)) return 768.0f / 544.0f;
    if (app.renderer.androidScreen()) return cr::Renderer::kAndroidScale;
    if (app.renderer.phoneLayout()) return cr::Renderer::kPhoneScale;
    return 1.0f;
}
float fieldViewW(const App& app) { return 2.0f * fieldViewScale(app) * float(app.stageCam.viewWidth > 0 ? app.stageCam.viewWidth : 1024); }
float fieldViewH(const App& app) { return 2.0f * fieldViewScale(app) * float(app.stageCam.viewHeight > 0 ? app.stageCam.viewHeight : 1024); }
// Painted Rooms: Fill (the port's): the view zoomed until the painting's rect covers the screen - 1
// in the original presentation, or where the rect already covers it
float fieldFillZoom(const App& app) {
    if (app.video.paintings != 1 || !app.haveStageCam) return 1;
    const float SW = app.renderer.screenW(), H = cr::Renderer::kLogicalH;
    const int* rc = app.stageCam.rect;
    const bool haveRect = rc[2] > 0 && rc[3] > 0;
    const float d = 2.0f * fieldViewScale(app); // (the rect in the record's view pixels: doubled, on this screen)
    const float rw = haveRect ? d * float(rc[2]) : fieldViewW(app), rh = haveRect ? d * float(rc[3]) : fieldViewH(app);
    return std::max(1.0f, std::max(SW / std::max(rw, 1.0f), H / std::max(rh, 1.0f)));
}

cr::Mat4 stageViewProjection(const App& app) {
    const cr::StageCamera& sc = app.stageCam;
    const float aspect = sc.viewHeight > 0 ? float(sc.viewWidth) / float(sc.viewHeight) : 1.0f;
    return cr::Mat4::perspective(sc.fovDegrees * 3.14159265f / 180.0f, aspect, 10.0f, 4000.0f) *
           cr::Mat4::lookAt({sc.eye[0], sc.eye[1], sc.eye[2]}, {sc.target[0], sc.target[1], sc.target[2]}, {0, 1, 0});
}

// Where world point p falls in the (doubled) view, in pixels from its top-left
bool fieldViewPoint(const App& app, const cr::Vec3& p, float& vx, float& vy) {
    const cr::Mat4 vp = stageViewProjection(app);
    const float* m = vp.m;
    const float w = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
    if (w <= 1e-4f) return false;
    vx = ((m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12]) / w * 0.5f + 0.5f) * fieldViewW(app);
    vy = (0.5f - (m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13]) / w * 0.5f) * fieldViewH(app);
    return true;
}

// FieldCamera::setScroll2D: centre view pixel (vx, vy), kept inside the painting's rect (a
// screen's half from its edges; a rect narrower than the screen stays centred); the offset
// moves `rate` of the way there
void scrollFieldTo(App& app, float vx, float vy, float rate) {
    const float V = fieldViewW(app), Vh = fieldViewH(app);
    const float fz = fieldFillZoom(app); // (Fill: less of the view on the screen)
    const float SW = app.renderer.screenW() / fz, H = cr::Renderer::kLogicalH / fz;
    const int* rc = app.stageCam.rect;
    const bool haveRect = rc[2] > 0 && rc[3] > 0;
    const float d = 2.0f * fieldViewScale(app); // (the rect in the record's view pixels: doubled, on this screen)
    const float rx = haveRect ? d * float(rc[0]) : 0, ry = haveRect ? d * float(rc[1]) : 0;
    const float rw = haveRect ? d * float(rc[2]) : V, rh = haveRect ? d * float(rc[3]) : Vh;
    auto axis = [](float v, float lo, float len, float screen) {
        return len <= screen ? lo + len / 2 : std::clamp(v, lo + screen / 2, lo + len - screen / 2);
    };
    const float tx = V / 2 - axis(vx, rx, rw, SW), ty = axis(vy, ry, rh, H) - Vh / 2;
    if (SDL_getenv("CR_SCROLL_DEBUG")) std::printf("scroll clamp: screen %.0f x %.0f, view %.0f x %.0f, rect %.0f %.0f %.0f %.0f\n", SW, H, V, Vh, rx, ry, rw, rh);
    app.scroll.offX += (tx - app.scroll.offX) * rate;
    app.scroll.offY += (ty - app.scroll.offY) * rate;
}

// FieldCamera::setScrollPlayer, every frame while the camera follows the player
void followPlayerScroll(App& app) {
    if (!scrollingField(app) || app.cam.free || !app.havePlayer) return;
    if (app.puzzle.stage >= 0 && app.puzzle.scroll.state == 1) return; // (ScrollCamera's: FieldCamera + 0 cleared, the view held where it was taken)
    app.scroll.scripted = false;
    const cr::Vec3 head = shownWalker(app).pos + cr::Vec3{0, 7.0f + float(app.stageCam.lookHeight), 0};
    float vx, vy;
    if (fieldViewPoint(app, head, vx, vy)) scrollFieldTo(app, vx, vy, 1.0f);
    if (SDL_getenv("CR_SCROLL_DEBUG"))
        std::printf("scroll: head view (%.0f, %.0f) of %.0f x %.0f, rect %d %d %d %d -> offset (%.0f, %.0f)\n", vx, vy,
                    fieldViewW(app), fieldViewH(app), app.stageCam.rect[0], app.stageCam.rect[1], app.stageCam.rect[2],
                    app.stageCam.rect[3], app.scroll.offX, app.scroll.offY);
}

// ScrollAutoCtl, once a 20 Hz step with the camera free: the slide
void stepFieldScroll(App& app) {
    App::FieldScroll& fs = app.scroll;
    if (!fs.sliding) return;
    if (!scrollingField(app) || !app.cam.free) { fs.sliding = false; return; }
    ++fs.t;
    const float k = fs.frames > 0 ? std::min(1.0f, float(fs.t) / float(fs.frames)) : 1.0f;
    const float x = fs.fromX + (fs.toX - fs.fromX) * k, y = fs.fromY + (fs.toY - fs.fromY) * k;
    fs.curX = x, fs.curY = y;
    fs.offX = fs.offY = 0;
    scrollFieldTo(app, fieldViewW(app) / 2 - x, fieldViewH(app) / 2 + y, 1.0f);
    if (fs.t >= fs.frames) fs.sliding = false;
}

// The scripts' shots on a painted field: jump (frames 0) or slide to offset (x, y)
// A script's move (npc_autoctl_prc), a frame: true once it has arrived
bool stepMove(App& app, App::Move& m) {
    ++m.t;
    const float t = float(m.t) / float(m.frames);
    const cr::Vec3 p{m.from.x + (m.to.x - m.from.x) * t, m.from.y + (m.to.y - m.from.y) * t, m.from.z + (m.to.z - m.from.z) * t};
    const float yaw = m.yawFrom + (m.yawTo - m.yawFrom) * t;
    setActor(app, m.npc, p, m.turn ? &yaw : nullptr, m.mode == 2);
    return m.t >= m.frames;
}
bool stepRescale(App& app, App::Rescale& r) { // (npc_autoctl_prc mode 3: the scale in a straight line)
    ++r.t;
    const float t = float(r.t) / float(r.frames);
    if (cr::FieldNpc* n = findNpc(app, r.npc)) {
        for (int k = 0; k < 3; ++k) n->axes[k] = r.from[k] + (r.to[k] - r.from[k]) * t;
        for (auto& d : app.npcDraws)
            if (d.id == r.npc) d.world = npcWorld(*n);
    }
    return r.t >= r.frames;
}
void stepScriptCam(App& app) { // (EvAutoCtl / CvAutoCtl: camera moves are straight lines)
    if (app.cam.t >= app.cam.frames) return;
    App::ScriptCam& c = app.cam;
    ++c.t;
    const float t = float(c.t) / float(c.frames);
    c.eye = c.eyeFrom + (c.eyeTo - c.eyeFrom) * t;
    c.target = c.tgtFrom + (c.tgtTo - c.tgtFrom) * t;
}

void scriptFieldScroll(App& app, float x, float y, int frames) {
    App::FieldScroll& fs = app.scroll;
    x *= fieldViewScale(app), y *= fieldViewScale(app); // (the device's pixels of the doubled view)
    if (frames <= 0) {
        fs.sliding = false;
        fs.curX = x, fs.curY = y, fs.scripted = true;
        fs.offX = fs.offY = 0;
        scrollFieldTo(app, fieldViewW(app) / 2 - x, fieldViewH(app) / 2 + y, 1.0f);
        return;
    }
    fs.fromX = fs.scripted ? fs.curX : fs.offX;
    fs.fromY = fs.scripted ? fs.curY : fs.offY;
    fs.scripted = true;
    fs.toX = x;
    fs.toY = y;
    fs.t = 0;
    fs.frames = frames;
    fs.sliding = true;
    stepFieldScroll(app); // (ScrollAutoCtl's first step: this frame's, after the script - see stepEvents)
}

// ---- Video settings ----

// Resolutions offered (up to 3840x2160); the window list leaves out what doesn't fit the display
constexpr int kResolutions[][2] = {{1024, 768}, {1152, 768}, {1280, 720}, {1280, 960}, {1366, 768}, {1440, 960}, {1600, 900}, {1600, 1200},
                                   {1920, 1080}, {1920, 1440}, {2560, 1440}, {2880, 2160}, {3200, 1800}, {3840, 2160}};
constexpr int kRenderScales[] = {50, 67, 75, 100, 125, 150, 200};

int keyValue(const char* text, const char* key, int fallback) {
    const char* at = SDL_strstr(text, key);
    return at ? SDL_atoi(at + SDL_strlen(key)) : fallback;
}

void loadVideo(App& app, const char* text) {
    App::Video& v = app.video;
    v.width = std::clamp(keyValue(text, "\nwidth=", v.width), 320, 3840);
    v.height = std::clamp(keyValue(text, "\nheight=", v.height), 240, 2160);
    v.display = std::clamp(keyValue(text, "\ndisplay=", v.display), 0, 2);
    v.widescreen = keyValue(text, "\nwidescreen=", v.widescreen) != 0;
    // painted_fill= 0 / 1 (Original / Fill); the old painted_rooms= (0 Original, 1 Whole, 2 Fill Width) and
    // zoom_paintings=: their Fill Width becomes Fill, Whole the original
    const int legacy = keyValue(text, "\npainted_rooms=", keyValue(text, "\nzoom_paintings=", 0) ? 2 : 0);
    v.paintings = std::clamp(keyValue(text, "\npainted_fill=", legacy == 2 ? 1 : 0), 0, 1);
    v.renderScale = std::clamp(keyValue(text, "\nrender_scale=", v.renderScale), 25, 400);
    v.msaa = std::clamp(keyValue(text, "\nmsaa=", v.msaa), 1, 8);
    v.vsync = keyValue(text, "\nvsync=", v.vsync) != 0;
    v.frameLimit = std::clamp(keyValue(text, "\nframe_limit=", v.frameLimit), 0, 1000);
    v.upscaler = std::clamp(keyValue(text, "\nupscaler=", v.upscaler), 0, kUpscalers - 1);
    v.upscaleMode = std::clamp(keyValue(text, "\nupscale_mode=", v.upscaleMode), 0, 4);
    v.sharpness = std::clamp(keyValue(text, "\nsharpness=", v.sharpness), 0, 100);
    v.hdTextures = keyValue(text, "\nhd_textures=", v.hdTextures) != 0;
    v.smoothModels = keyValue(text, "\nsmooth_models=", v.smoothModels);
    v.hdModels = keyValue(text, "\nhd_models=", v.hdModels) != 0;
    if (v.smoothModels != 2 && v.smoothModels != 3) v.smoothModels = 0;
    {
        const int a = keyValue(text, "\nanisotropy=", v.anisotropy);
        v.anisotropy = a >= 16 ? 16 : a >= 8 ? 8 : a >= 4 ? 4 : a >= 2 ? 2 : 1;
    }
    v.sharpTextures = keyValue(text, "\nsharp_textures=", v.sharpTextures) != 0;
    v.layout = std::clamp(keyValue(text, "\nlayout=", v.layout), 0, 3);
    app.showHud = keyValue(text, "\ncontrol_hints=", app.showHud) != 0;
    app.environmentEffects = keyValue(text, "\nenvironment_effects=", app.environmentEffects) != 0;
    app.lockOrientation = keyValue(text, "\nlock_orientation=", app.lockOrientation) != 0;
    app.lockLandscapeFlipped = keyValue(text, "\nlock_landscape_flipped=", app.lockLandscapeFlipped) != 0;
    if (const char* at = SDL_strstr(text, "\npad_confirm=")) app.padConfirmEast = SDL_strncmp(at + 13, "south", 5) != 0;
    app.fieldSpeed = std::clamp(keyValue(text, "\nfield_speed=", app.fieldSpeed), 1, 5);
    app.battleSpeed = std::clamp(keyValue(text, "\nbattle_speed=", app.battleSpeed), 1, 5);
}

// Keep the preferred landscape direction in the same settings file as the lock.
// SDL's hint constrains subsequent Android window updates; the Activity call
// applies the change immediately, without recreating the window or GL context.
void applyOrientation(App& app, bool captureDirection) {
    if (captureDirection && app.window) {
        const auto orientation = SDL_GetCurrentDisplayOrientation(SDL_GetDisplayForWindow(app.window));
        if (orientation == SDL_ORIENTATION_LANDSCAPE || orientation == SDL_ORIENTATION_LANDSCAPE_FLIPPED)
            app.lockLandscapeFlipped = orientation == SDL_ORIENTATION_LANDSCAPE_FLIPPED;
    }
    const char* hint = !app.lockOrientation ? "LandscapeLeft LandscapeRight" :
        app.lockLandscapeFlipped ? "LandscapeRight" : "LandscapeLeft";
    SDL_SetHint(SDL_HINT_ORIENTATIONS, hint);
#ifdef SDL_PLATFORM_ANDROID
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity) return;
    jclass cls = env->GetObjectClass(activity);
    jmethodID method = cls ? env->GetMethodID(cls, "setOrientationLock", "(ZZ)V") : nullptr;
    if (method) env->CallVoidMethod(activity, method, jboolean(app.lockOrientation), jboolean(app.lockLandscapeFlipped));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "Could not apply the screen orientation lock");
    }
    if (cls) env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
#endif
}

// Window size / fullscreen mode and the renderer's options from app.video
void applyVideo(App& app) {
    App::Video& v = app.video;
    cr::Renderer& r = app.renderer;
    r.setWidescreen(v.widescreen);
    r.setPhoneLayout(v.layout >= 2 || (v.layout == 0 && app.phoneData)); // 2.2.0's screens (3:2)
    r.setAndroidScreen(v.layout == 3); // (the Android release's: docs/ANDROID.md)
    r.setRenderScale(v.renderScale / 100.0f);
    r.setSamples(v.msaa);
    r.setVSync(v.vsync);
    cr::UpscaleSettings up;
    up.mode = kUpscaler[std::clamp(v.upscaler, 0, kUpscalers - 1)];
    up.ratio = kUpscaleModes[std::clamp(v.upscaleMode, 0, 4)].ratio;
    up.sharpness = v.sharpness / 100.0f;
    r.setUpscaler(up);
    cr::TextureFiltering tf;
    tf.anisotropy = v.anisotropy;
    tf.sharp = v.sharpTextures;
    // (an upscaler's scene is smaller: the mips its output would take - DLSS's and FSR's advice)
    tf.lodBias = up.mode != cr::Upscaler::Off ? -std::log2(up.ratio) : 0.0f;
    r.setFiltering(tf);
    if (!app.window) return;
#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_IOS)
    constexpr bool kPhone = true; // (a phone's or tablet's: the whole screen, the system's bars hidden - SDL's immersive mode)
#else
    constexpr bool kPhone = false;
#endif
    if (v.display == 0 && !kPhone) {
        SDL_SetWindowFullscreen(app.window, false);
        // The size in pixels: the window's in points, so divide by the display's density
        const float density = std::max(SDL_GetWindowPixelDensity(app.window), 0.25f);
        if (!SDL_SetWindowSize(app.window, int(v.width / density + 0.5f), int(v.height / density + 0.5f)))
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "window size %dx%d: %s", v.width, v.height, SDL_GetError());
    } else {
        SDL_DisplayMode mode{};
        const bool exact = v.display == 1 &&
                           SDL_GetClosestFullscreenDisplayMode(SDL_GetDisplayForWindow(app.window), v.width, v.height, 0.0f,
                                                               true, &mode);
        SDL_SetWindowFullscreenMode(app.window, exact ? &mode : nullptr); // nullptr: borderless at the desktop's size
        SDL_SetWindowFullscreen(app.window, true);
    }
    SDL_SyncWindow(app.window);
    applyOrientation(app);
    // The window manager applies the size when it gets to it: wait for it (up to a second) so the
    // next frame, and captures, have it
    for (int i = 0; i < 50 && v.display == 0; ++i) {
        SDL_PumpEvents();
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(app.window, &pw, &ph);
        if (std::abs(pw - v.width) <= 2 && std::abs(ph - v.height) <= 2) break;
        SDL_Delay(20);
    }
    SDL_PumpEvents();
    if (SDL_getenv("CR_VIDEO_DEBUG")) {
        int w = 0, h = 0, pw = 0, ph = 0;
        SDL_GetWindowSize(app.window, &w, &h);
        SDL_GetWindowSizeInPixels(app.window, &pw, &ph);
        std::fprintf(stderr, "video: asked %dx%d, window %dx%d points, %dx%d pixels, density %g, driver %s\n", v.width,
                     v.height, w, h, pw, ph, SDL_GetWindowPixelDensity(app.window), SDL_GetCurrentVideoDriver());
    }
}

void stepChoice(int& value, const int* choices, int count, int dir) {
    int i = 0;
    while (i < count - 1 && choices[i] < value) ++i;
    if (choices[i] != value && dir < 0) ++i; // between two: step from the next one down
    value = choices[std::clamp(i + dir, 0, count - 1)];
}

// adjustVideo's settings: 0 resolution, 1 display, 2 aspect, 3 painted rooms, 4 render scale,
// 5 MSAA, 6 vsync, 7 the original presentation, 8 anti-aliasing (MSAA / FSR Native AA / DLAA),
// 9 upscaler, 10 upscale mode, 11 sharpness, 12 HD textures, 13 smoothed models, 14 layout,
// 15 the frame rate, 16 anisotropic filtering, 17 texture filtering

// Anti-aliasing choices: Off, MSAA 2 / 4 / 8, FSR (Native AA), DLSS (DLAA)
int aaChoice(const App::Video& v) {
    if (v.upscaler) return 3 + v.upscaler;
    return v.msaa >= 8 ? 3 : v.msaa >= 4 ? 2 : v.msaa >= 2 ? 1 : 0;
}

std::string aaName(const App::Video& v) {
    if (v.upscaler == 1) return v.upscaleMode == 0 ? "FSR Native AA" : "FSR 3.1";
    if (v.upscaler == 2) return v.upscaleMode == 0 ? "DLAA" : "DLSS";
    if (v.upscaler == 3) return v.upscaleMode == 0 ? "MetalFX AA" : "MetalFX";
    return v.msaa <= 1 ? std::string("Off") : std::to_string(v.msaa) + "x MSAA";
}

void adjustVideo(App& app, int row, int dir) {
    App::Video& v = app.video;
    switch (row) {
    case 0: { // resolution
        int i = 0;
        const int n = int(sizeof kResolutions / sizeof kResolutions[0]);
        for (int k = 0; k < n; ++k)
            if (kResolutions[k][0] == v.width && kResolutions[k][1] == v.height) i = k;
        SDL_Rect usable{0, 0, 3840, 2160};
        if (app.window) SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(app.window), &usable);
        const float density = app.window ? std::max(SDL_GetWindowPixelDensity(app.window), 0.25f) : 1.0f;
        for (int step = 0; step < n; ++step) {
            i = (i + dir + n) % n;
            const bool fits = v.display != 0 || (kResolutions[i][0] / density <= usable.w && kResolutions[i][1] / density <= usable.h);
            if (fits) break;
        }
        v.width = kResolutions[i][0];
        v.height = kResolutions[i][1];
        break;
    }
    case 1: v.display = (v.display + dir + 3) % 3; break;
    case 2: v.widescreen = !v.widescreen; break;
    case 3: v.paintings = (v.paintings + dir + 2) % 2; break;
    case 4: stepChoice(v.renderScale, kRenderScales, int(sizeof kRenderScales / sizeof kRenderScales[0]), dir); break;
    case 5: {
        static const int kMsaa[] = {1, 2, 4, 8};
        stepChoice(v.msaa, kMsaa, 4, dir);
        v.msaa = std::min(v.msaa, app.renderer.maxSamples());
        break;
    }
    case 6: v.vsync = !v.vsync; break;
    case 7: v = App::Video(); break; // the original presentation
    case 8: { // MSAA up to what the GPU has, then the temporal upscalers at native resolution
        const int maxMsaa = app.renderer.maxSamples();
        int c = aaChoice(v);
        for (int step = 0; step < 6; ++step) {
            c = (c + dir + 6) % 6;
            const int samples = c >= 1 && c <= 3 ? 1 << c : 1;
            if (samples <= maxMsaa) break;
        }
        if (c >= 4) {
            v.upscaler = c - 3;
            v.upscaleMode = 0;
        } else {
            v.upscaler = 0;
            v.msaa = c ? 1 << c : 1;
        }
        break;
    }
    case 9:
        do v.upscaler = (v.upscaler + dir + kUpscalers) % kUpscalers;
        while (!upscalerHere(v.upscaler));
        break;
    case 10: v.upscaleMode = (v.upscaleMode + dir + 5) % 5; break;
    case 11: v.sharpness = std::clamp(v.sharpness + dir * 5, 0, 100); break;
    case 14: v.layout = (v.layout + dir + 4) % 4; break;
    case 15: { // the frame rate: 30, 60, 120, 144, unlimited
        static const int kLimits[] = {30, 60, 120, 144, 0};
        int i = 0;
        while (i < 4 && kLimits[i] != v.frameLimit) ++i;
        v.frameLimit = kLimits[(i + dir + 5) % 5];
        break;
    }
    case 16: { // anisotropic filtering: off, 2x .. 16x (up to the GPU's)
        static const int kAniso[] = {1, 2, 4, 8, 16};
        int i = 0;
        while (i < 4 && kAniso[i] != v.anisotropy) ++i;
        const int most = std::max(app.renderer.maxAnisotropy(), 1);
        for (int step = 0; step < 5; ++step) {
            i = (i + dir + 5) % 5;
            if (kAniso[i] <= std::max(most, 1) || kAniso[i] == 1) break;
        }
        v.anisotropy = kAniso[i];
        break;
    }
    case 17: v.sharpTextures = !v.sharpTextures; break;
    case 18: v.hdModels = !v.hdModels; break; // (what loads from now on)
    case 13: v.smoothModels = (v.smoothModels == 0 ? (dir > 0 ? 2 : 3) : v.smoothModels == 2 ? (dir > 0 ? 3 : 0) : (dir > 0 ? 0 : 2)); break;
    case 12: { // HD textures: what loads from now on (the next room); the UI atlas at once
        v.hdTextures = !v.hdTextures;
        cr::Image wi;
        if (app.rendererReady && loadImage(app, "window_hd.mct", wi, &app.androidWindow)) {
            app.renderer.destroy(app.windowTex);
            app.windowTex = app.renderer.upload(wi);
            noteWindowAtlas(app, wi);
        }
        break;
    }
    default: break;
    }
    applyVideo(app);
    saveMix(app);
}


// ---- The Options screen ----
// Built from the original's pieces (__menuu_OptionPut: the "Options" plate, label bars, Check /
// On-Off / Japanese-English buttons, gauges, "n / m", Back), but organised by subject on three
// denser pages: rows 36 apart instead of 48, the controls at 85%, Back and the page number lower.
// The original's settings are all here (Instruction Manual, BGM, Sound Effects, Language,
// Environment Effects, Lock Screen Orientation, Return to Title / Reset Data), with their own
// labels in the game's language; ours are added beside them.

const char* const kOptionPageName[kOptionPages] = {"Game", "Sound", "Display", "Detail", "Graphics"};

std::string sysLabel(const App& app, int id, const char* fallback) {
    const std::string t = sysText(app, id);
    return t.empty() ? fallback : t;
}

void saveImportAnswer(App& app, bool yes) {
    const bool finished = app.saveImport.state == vitaimport::Session::Result && app.saveImport.succeeded;
    app.saveImport.answer(yes, app.saveDirOverride.empty() ? cr::saveDir() : app.saveDirOverride);
    if (finished) {
        app.optionsOpen = false;
        app.trackerArmed = false;
        bootTitle(app);
    }
}


std::vector<OptRow> optionRows(App& app, int page) {
    std::vector<OptRow> rows;
    auto gauge = [&](const std::string& label, int* v, int sound) {
        OptRow r;
        r.label = label;
        r.kind = OptControl::Gauge;
        r.gauge = v;
        r.value = *v;
        r.sound = sound; // hear the new level
        r.act = [&app, v](int dir) {
            *v = std::clamp(*v + dir * 5, 0, 100);
            applyMix(app);
            saveMix(app);
        };
        rows.push_back(r);
    };
    auto onOff = [&](const std::string& label, bool* flag, std::function<void()> after) {
        OptRow r;
        r.label = label;
        r.kind = OptControl::Pair;
        r.words[0] = 33; // On
        r.words[1] = 34; // Off
        r.selected = *flag ? 0 : 1;
        r.act = [&app, flag, after](int i) {
            *flag = i == 0;
            if (after) after();
            saveMix(app);
        };
        rows.push_back(r);
    };
    auto value = [&](const std::string& label, const std::string& text, std::function<void(int)> act) {
        OptRow r;
        r.label = label;
        r.kind = OptControl::Value;
        r.valueText = text;
        r.act = std::move(act);
        rows.push_back(r);
    };
    auto pair = [&](const std::string& label, const char* a, const char* b, int selected, std::function<void(int)> act) {
        OptRow r;
        r.label = label;
        r.kind = OptControl::Pair;
        r.texts[0] = a;
        r.texts[1] = b;
        r.selected = selected;
        r.act = std::move(act);
        rows.push_back(r);
    };
    App::Video& v = app.video;
    switch (page) {
    case 0: { // Game
        if (languageSheet(app)) { // 2.2.0 (__menuu_OptionPrc 0x6fdbc, state 5): one Language row, its arrows
            // through the eight languages (left: 0 -> 7), the name window_char's - __menuu_OptionPut
            // (0x70f4c): tex2d slot 0x32, 88 x 24 at u = language x 88
            value(sysLabel(app, 58082, "Language"), cr::kLanguageName[app.language], [&app](int dir) {
                for (int k = 1; k <= cr::kLanguages; ++k) { // (only those the data has)
                    const int l = ((app.language + (dir < 0 ? -1 : 1) * k) % cr::kLanguages + cr::kLanguages) % cr::kLanguages;
                    if (languageUsable(app, l)) {
                        app.language = l;
                        break;
                    }
                }
                applyLanguage(app);
                saveMix(app);
            });
            rows.back().valueTex = &app.charSheet;
            const float uv[4] = {88.0f * float(app.language), 0, 88, 24};
            std::memcpy(rows.back().valueUv, uv, sizeof uv);
        } else {
        OptRow lang;
        lang.label = sysLabel(app, 58082, "Language");
        lang.kind = OptControl::Pair;
        lang.words[0] = 37; // Japanese
        lang.words[1] = 38; // English
        lang.selected = app.language == 0 ? 0 : app.language == 1 ? 1 : -1;
        lang.act = [&app](int i) {
            if (!languageUsable(app, i)) return;
            app.language = i;
            applyLanguage(app);
            saveMix(app);
        };
        rows.push_back(lang);
        value("Text Language", cr::kLanguageName[app.language], [&app](int dir) {
            for (int k = 1; k <= cr::kLanguages; ++k) {
                const int l = ((app.language + (dir ? dir : 1) * k) % cr::kLanguages + cr::kLanguages) % cr::kLanguages;
                if (languageUsable(app, l)) {
                    app.language = l;
                    break;
                }
            }
            applyLanguage(app);
            saveMix(app);
        });
        }
        onOff(sysLabel(app, 58108, "Environment Effects"), &app.environmentEffects, nullptr);
        onOff(sysLabel(app, 58109, "Lock Screen Orientation"), &app.lockOrientation,
              [&app] { applyOrientation(app, app.lockOrientation); });
        onOff("Control Hints", &app.showHud, nullptr);
        rows.back().hint = "Show the control hints and status overlay. Off hides the WASD help; F3 is a shortcut.";
        value("Import Vita Saves", "Choose Folder", [&app](int dir) {
            if (dir == 0 && app.optionsFromTitle) app.saveImport.begin("cr1", app.window);
        });
        rows.back().enabled = app.optionsFromTitle;
        rows.back().hint = app.optionsFromTitle ? "Choose your decrypted Vita save export. Existing saves will be backed up."
                                               : "Import saves from the title screen's Options.";
        { // the Vita copy to take assets from (a folder dialog; Left / Right: none)
            std::string shown = "None";
            if (!app.vitaPath.empty()) {
                std::string p = app.vitaPath;
                while (p.size() > 1 && p.back() == '/') p.pop_back();
                shown = p.substr(p.rfind('/') + 1);
                if (shown.size() > 18) shown = shown.substr(0, 17) + "~";
            }
            if (app.vitaOnly) shown = "The game's data";
            else if (app.vitaPath != app.vitaLoaded) shown += " (next start)";
            else if (!app.vitaPath.empty() && !app.vita.isOpen()) shown += " (not found)";
            value("Vita Assets", shown, [&app](int dir) {
                if (app.vitaOnly) return; // (it is the whole game here)
                if (dir == 0) chooseVitaFolder(app);
                else if (!app.vitaPath.empty()) app.vitaPath.clear(), app.vitaNote.clear(), saveMix(app);
            });
            rows.back().enabled = !app.vitaOnly && !app.androidOnly;
            rows.back().hint = app.vitaOnly ? "Running from the Vita copy alone: " + app.vitaRoot
                               : !app.vitaNote.empty() ? app.vitaNote
                               : "Enter: your Vita copy's PCSG00497 folder - its textures, voices, music and scripts; Left / Right: none";
        }
        { // the Android copy to take assets from (a folder holding its main.*.obb and APK; Left / Right: none)
            std::string shown = "None";
            if (!app.androidPath.empty()) {
                std::string p = app.androidPath;
                while (p.size() > 1 && p.back() == '/') p.pop_back();
                shown = p.substr(p.rfind('/') + 1);
                if (shown.size() > 18) shown = shown.substr(0, 17) + "~";
            }
            if (app.androidOnly) shown = "The game's data";
            else if (app.androidPath != app.androidLoaded) shown += " (next start)";
            else if (!app.androidPath.empty() && !app.android.isOpen()) shown += " (not found)";
            value("Android Assets", shown, [&app](int dir) {
                if (app.androidOnly) return; // (it is the whole game here)
                if (dir == 0) chooseAndroidFolder(app);
                else if (!app.androidPath.empty()) app.androidPath.clear(), app.androidNote.clear(), saveMix(app);
            });
            rows.back().enabled = !app.vitaOnly && !app.androidOnly;
            rows.back().hint = app.androidOnly ? "Running from the Android release alone: " + app.android.path()
                               : !app.androidNote.empty() ? app.androidNote
                               : "Enter: a folder with your Android copy's main.*.obb and its APK - its textures and the art "
                                 "only it has; Left / Right: none";
        }
        const auto speed = [&](const char* label, int* x, const char* what) { // (Off, 2x .. 5x)
            value(label, *x > 1 ? std::to_string(*x) + "x" : std::string("Off"), [&app, x](int dir) {
                *x = (*x - 1 + (dir ? dir : 1) + 5) % 5 + 1;
                saveMix(app);
            });
            rows.back().hint = what;
        };
        speed("Field Speed", &app.fieldSpeed, "Walking the field 2 .. 5 times as fast - the scenes, talk and menus as they are");
        speed("Battle Speed", &app.battleSpeed, "The battles 2 .. 5 times as fast");
        OptRow swap; // the controller's ○ (east) / × (south): Off as on the Vita, ○ confirms
        swap.label = "Swap \u25cb and \u00d7";
        swap.kind = OptControl::Pair;
        swap.words[0] = 33; // On
        swap.words[1] = 34; // Off
        swap.selected = app.padConfirmEast ? 1 : 0;
        swap.act = [&app](int i) {
            app.padConfirmEast = i == 1;
            saveMix(app);
        };
        swap.hint = std::string(app.pad ? std::string("Controller: ") + SDL_GetGamepadName(app.pad) + ". " : "With a controller: ") +
                    (app.padConfirmEast ? "\u25cb (the right button) confirms, \u00d7 (the bottom one) goes back - as on the Vita"
                                        : "\u00d7 (the bottom button) confirms, \u25cb (the right one) goes back");
        rows.push_back(swap);
        OptRow manual; // the topics and their pages (MuScrList + MsgUty)
        manual.label = sysLabel(app, 58081, "Instruction Manual");
        manual.act = [&app](int) { openManual(app, 0); };
        rows.push_back(manual);
        OptRow title;
        if (app.optionsFromTitle) { // ModeOptionMenu(1): erasing the save data, asked twice
            title.label = sysLabel(app, 58122, "Reset Data");
            title.act = [&app](int) { app.resetAsk = 1, app.resetConfirm = 1; };
        } else {
            title.label = sysLabel(app, 58123, "Return to Title");
            title.enabled = app.mode == Mode::Maps && !app.booting;
            title.act = [&app](int) { app.resetAsk = 3, app.resetConfirm = 1; }; // OptionPrc 0xc: "return to the title?" (57060)
        }
        rows.push_back(title);
        break;
    }
    case 1: { // Sound
        gauge("Master Volume", &app.mix.master, int(se::kVolume));
        gauge(sysLabel(app, 58085, "BGM"), &app.mix.music, -1);
        gauge(sysLabel(app, 58086, "Sound Effects"), &app.mix.effects, int(se::kVolume));
        gauge("Voice Volume", &app.mix.voice, -1);
        char test[32];
        SDL_snprintf(test, sizeof test, "%03d", app.soundTestEntry);
        value("Sound Test", test, [&app](int dir) {
            const int n = std::max(1, app.sound.entries());
            if (dir == 0) app.sound.play(uint32_t(app.soundTestEntry));
            else app.soundTestEntry = ((app.soundTestEntry + dir) % n + n) % n;
        });
        rows.back().enterSounds = true;
        break;
    }
    case 2: { // Display
        static const char* const kDisplay[] = {"Window", "Fullscreen", "Borderless"};
        value("Resolution", v.display == 2 ? "Desktop" : std::to_string(v.width) + " x " + std::to_string(v.height),
              [&app](int dir) { adjustVideo(app, 0, dir ? dir : 1); });
        value("Display", kDisplay[v.display], [&app](int dir) { adjustVideo(app, 1, dir ? dir : 1); });
        pair("Aspect", "4:3", "Wide", v.widescreen ? 1 : 0, [&app](int i) {
            if (app.video.widescreen != (i == 1)) adjustVideo(app, 2, 1);
        });
        static const char* const kLayout[] = {"Auto", "iPad", "iPhone", "Android"};
        value("Layout", kLayout[std::clamp(v.layout, 0, 3)], [&app](int dir) { adjustVideo(app, 14, dir ? dir : 1); });
        rows.back().hint = "The screens' layout: the iPad release's (4:3), the iPhone 2.2.0's (3:2) or the Android release's (16:9); Auto follows the data";
        static const char* const kPaintings[] = {"Original", "Fill"};
        value("Painted Rooms", kPaintings[std::clamp(v.paintings, 0, 1)], [&app](int dir) { adjustVideo(app, 3, dir ? dir : 1); });
        onOff("VSync", &v.vsync, [&app] { applyVideo(app); });
        value("Frame Rate", v.frameLimit ? std::to_string(v.frameLimit) : std::string("Unlimited"),
              [&app](int dir) { adjustVideo(app, 15, dir ? dir : 1); });
        rows.back().hint = "Frames drawn a second at most; the game runs at its own 20 a second, shown moving between them";
        OptRow restore;
        restore.label = "Original Presentation";
        restore.hint = "Every display and graphics option back to the original's";
        restore.act = [&app](int) { adjustVideo(app, 7, 0); };
        rows.push_back(restore);
        break;
    }
    case 3: { // Detail: the textures and the models
        onOff("HD Textures", &v.hdTextures, nullptr);
        rows.back().act = [&app](int i) {
            if (app.video.hdTextures != (i == 0)) adjustVideo(app, 12, 1);
        };
        {
            const bool have = !app.hdDir.empty() && SDL_GetPathInfo(app.hdDir.c_str(), nullptr);
            rows.back().hint = have ? "waifu2x / Real-ESRGAN upscales from textures/hd (tools/hd_textures.py); from the next room on"
                                    : "None in textures/hd yet: --export-textures, then tools/hd_textures.py";
        }
        onOff("HD Models", &v.hdModels, nullptr);
        rows.back().act = [&app](int i) {
            if (app.video.hdModels != (i == 0)) adjustVideo(app, 18, 1);
        };
        {
            const bool have = !app.modelsDir.empty() && SDL_GetPathInfo(app.modelsDir.c_str(), nullptr);
            rows.back().hint = have ? "Replacement models (glTF) from models/hd, in place of the game's; from the next room on"
                                    : "None in models/hd yet: --export-models, edit in Blender, save as models/hd/<name>.glb";
        }
        static const char* const kSmooth[] = {"Off", "", "Smooth", "Smoother"};
        value("Smooth Models", kSmooth[v.smoothModels], [&app](int dir) { adjustVideo(app, 13, dir ? dir : 1); });
        rows.back().hint = v.smoothModels ? "Rounder models (4x / 9x triangles), hard edges kept; from the next room on"
                                          : "Rounds the models' silhouettes - characters, monsters, props, stages (PN triangles)";
        {
            const int most = std::max(app.renderer.maxAnisotropy(), 1);
            value("Anisotropic Filtering", v.anisotropy > 1 && most > 1 ? std::to_string(std::min(v.anisotropy, most)) + "x" : std::string("Off"),
                  [&app](int dir) { adjustVideo(app, 16, dir ? dir : 1); });
            rows.back().enabled = most > 1;
            rows.back().hint = most > 1 ? "Textures stay sharp on floors and walls seen at an angle" : "Not on this GPU";
        }
        pair("Texture Filtering", "Smooth", "Sharp", v.sharpTextures ? 1 : 0, [&app](int i) {
            if (app.video.sharpTextures != (i == 1)) adjustVideo(app, 17, 1);
        });
        rows.back().hint = "Sharp: the world's textures magnified by the nearest texel (square pixels up close)";
        break;
    }
    default: { // Graphics: what the Vulkan renderer adds
        const bool scalable = app.renderer.scalable();
        const std::string why = app.renderer.upscalerProblem(kUpscaler[std::clamp(v.upscaler, 0, kUpscalers - 1)]);
        auto unavailable = [&](OptRow& r) {
            if (v.upscaler && !why.empty()) r.hint = std::string(kUpscalerName[v.upscaler]) + " is off here: " + why;
        };
        value("Render Scale", scalable ? std::to_string(v.renderScale) + "%" : "100%", [&app](int dir) {
            if (app.renderer.scalable()) adjustVideo(app, 4, dir ? dir : 1);
        });
        rows.back().enabled = scalable;
        rows.back().hint = scalable ? "The frame at this size, then scaled to the window" : "Needs the Vulkan renderer";
        const std::string na = v.upscaler && !why.empty() ? " n/a" : ""; // chosen, but can't run here
        value("Anti-aliasing", scalable ? aaName(v) + na : "Off", [&app](int dir) {
            if (app.renderer.scalable()) adjustVideo(app, 8, dir ? dir : 1);
        });
        rows.back().enabled = scalable;
        rows.back().hint = !scalable ? "Needs the Vulkan renderer"
                           : v.upscaler ? "Temporal: jittered frames accumulated (MSAA is off meanwhile)"
                                        : "MSAA, or FSR Native AA / DLAA at full resolution";
        unavailable(rows.back());
        value("Upscaler", scalable ? kUpscalerName[v.upscaler] + na : "Off", [&app](int dir) {
            if (app.renderer.scalable()) adjustVideo(app, 9, dir ? dir : 1);
        });
        rows.back().enabled = scalable;
        rows.back().hint = "The scene renders smaller and is reconstructed; the menus stay sharp";
        unavailable(rows.back());
        value("Upscale Mode", v.upscaler == 2 && v.upscaleMode == 0 ? "DLAA" : kUpscaleModes[v.upscaleMode].name,
              [&app](int dir) {
                  if (app.video.upscaler) adjustVideo(app, 10, dir ? dir : 1);
              });
        rows.back().enabled = scalable && v.upscaler != 0;
        {
            const float ratio = kUpscaleModes[v.upscaleMode].ratio;
            char h[96];
            SDL_snprintf(h, sizeof h, "The scene at %d%% of the frame per side", int(100.0f / ratio + 0.5f));
            rows.back().hint = h;
        }
        value("Sharpness", std::to_string(v.sharpness) + "%", [&app](int dir) {
            if (app.video.upscaler == 1 || app.video.upscaler == 4) adjustVideo(app, 11, dir ? dir : 1);
        });
        rows.back().enabled = scalable && (v.upscaler == 1 || v.upscaler == 4);
        rows.back().hint = "FSR's sharpening pass (RCAS); DLSS has none";
        break;
    }
    }
    return rows;
}

// Menu units of row i: the top of its 24-tall bar; its control is centred on the bar
float optRowY(int i) { return kOptRowTop + kOptRowPitch * float(i); }
// A control's x (unscaled design units from kOptCtrlX) on screen
float optCtrlX(float x) { return kOptCtrlX + (x - kOptCtrlX) * kOptCtrlScale; }

void openOptions(App& app, int page) {
    const bool first = !app.optionsOpen;
    if (first) app.optionsFromTitle = app.titleOpen, app.resetAsk = 0;
    app.optionsOpen = true;
    app.optionsPage = (page + kOptionPages) % kOptionPages;
    app.optionsRow = 0;
    app.optionsScroll = 0;
    app.optionsAnim = 0;
    // __menuu_OptionSet (0x6fafc, 2.2.0 as 1.0.1): the first time - in any mode, the title's too - flag
    // 0x43 set and OptionPrc 0..2: SetFlag(3), MsgUty_WinOpen / SetMsg(0x23) - block 5's 35, 56035
    // "Option" in the manual's message window (no picture: _mwin[5]) - alone: no menu mask, the page
    // held back until the window has shut (then MuCom_MenuSetMask(1) and OptionPrc 3's slide-in).
    // (The title's flags are the saves', as the Progress Tracker's: allPairsFlags)
    // (and settings.ini's seen_flags: seen once, it stays seen across launches without a save - the
    // owner's choice, as Omega's port)
    if (first && !app.game.flag(0x43) && !seenFlag(app, 0x43) && !(app.titleOpen && allPairsFlags(app).flag(0x43))) {
        app.game.setFlag(0x43, true);
        markSeen(app, 0x43);
        openManual(app, kOptionIntroPage);
    }
}

// OptionPrc 0..2: "Option" up (openOptions), the page and its mask not yet
bool optionsAbout(const App& app) { return app.optionsOpen && app.manualOpen && app.manualIntro; }

// The page's scroll kept in range - and, `follow`, moved to show the cursor's row
void optionsScrollFix(App& app, int n, bool follow) {
    const int most = std::max(0, n - kOptVisible);
    if (follow && app.optionsRow >= 0 && app.optionsRow < n) {
        if (app.optionsRow < app.optionsScroll) app.optionsScroll = app.optionsRow;
        if (app.optionsRow >= app.optionsScroll + kOptVisible) app.optionsScroll = app.optionsRow - kOptVisible + 1;
    }
    app.optionsScroll = std::clamp(app.optionsScroll, 0, most);
}
// The scroll bar's track (menu units) and the thumb on it
void optionsBar(int n, int scroll, float& top, float& height, float& thumbY, float& thumbH) {
    top = kOptRowTop - 2, height = kOptRowPitch * float(kOptVisible - 1) + 28;
    thumbH = std::max(height * float(kOptVisible) / float(std::max(n, 1)), 24.0f);
    const int most = std::max(1, n - kOptVisible);
    thumbY = top + (height - thumbH) * float(scroll) / float(most);
}
// A point on the bar (menu y): the scroll that puts the thumb's middle there; the cursor kept on screen
void optionsBarTo(App& app, float my) {
    const int n = int(optionRows(app, app.optionsPage).size());
    float top, height, ty, th;
    optionsBar(n, app.optionsScroll, top, height, ty, th);
    const float f = std::clamp((my - top - th / 2) / std::max(height - th, 1.0f), 0.0f, 1.0f);
    app.optionsScroll = int(std::lround(f * float(std::max(0, n - kOptVisible))));
    if (app.optionsRow != kOptionsBackRow)
        app.optionsRow = std::clamp(app.optionsRow, app.optionsScroll, std::min(n, app.optionsScroll + kOptVisible) - 1);
}
// The mouse wheel / a drag over the rows: `rows` rows on; the cursor kept on screen
void optionsScrollBy(App& app, int rows) {
    const int n = int(optionRows(app, app.optionsPage).size());
    app.optionsScroll += rows;
    optionsScrollFix(app, n, false);
    if (app.optionsRow != kOptionsBackRow && n > 0)
        app.optionsRow = std::clamp(app.optionsRow, app.optionsScroll, std::min(n, app.optionsScroll + kOptVisible) - 1);
}
// Text in lines no wider than `width` (at most `most`), by words
std::vector<std::string> wrapText(cr::MenuPainter& m, const std::string& s, float width, float size, int most) {
    std::vector<std::string> lines(1);
    size_t at = 0;
    while (at < s.size()) {
        size_t end = s.find(' ', at);
        if (end == std::string::npos) end = s.size();
        const std::string word = s.substr(at, end - at);
        const std::string tried = lines.back().empty() ? word : lines.back() + " " + word;
        if (!lines.back().empty() && m.textWidth(tried, size) > width) {
            if (int(lines.size()) == most) {
                lines.back() += "...";
                return lines;
            }
            lines.push_back(word);
        } else {
            lines.back() = tried;
        }
        at = end + 1;
    }
    return lines;
}

// A tap's menu units under the same offset
void phoneMenuPoint(const App& app, float& mx, float& my) {
    if (app.renderer.phoneLayout()) mx -= 16, my -= 32;
}
// Widescreen (the iPad layout): the origins of a screen's left- and right-hand groups - kept to
// the screen's edges, as the later widescreen release does (and the shop and the battle here),
// the rest centred (uiX). The iPhone layout's one 480-wide screen doesn't spread
// (inside the display's safe area: off a camera hole)
float wideLeft(const App& app) {
    const cr::Renderer& r = app.renderer;
    return r.phoneLayout() ? r.uiX() : std::min(r.uiX(), r.safeLeft());
}
float wideRight(const App& app) {
    const cr::Renderer& r = app.renderer;
    return r.phoneLayout() ? r.uiX() : std::max(r.uiX(), 2 * r.uiX() - r.safeRight());
}
// The Android layout: how far the phone's own edges lie outside the centred 480 x 320 screen, in
// tex2d units - Android 1.1.1's gac +0x3d14c / +0x3d150 (the centring offset in pixels) over +0x3d154
// (the 2D scale). The pieces 1.1.1 puts on the phone's edges add or subtract it (_menub_mainPut: the
// battle's badge, rows and banner); 0 in the iPhone, iPad and Vita layouts
float androidEdgeX(const App& app) {
    const cr::Renderer& r = app.renderer;
    if (!r.androidScreen()) return 0;
    // tex2d x 0 is layout x uiX + 32; the screen's left edge is layout x w/2 - w/2 / scale
    return (r.uiX() + 32 - r.screenW() * 0.5f * (1 - 1 / r.layoutScale())) / 2;
}
float androidEdgeY(const App& app) {
    const cr::Renderer& r = app.renderer;
    return r.androidScreen() ? (64 - r.layoutTop()) / 2 : 0.0f; // (tex2d y 0 is layout y 64)
}
// A logical point in menu units of the group drawn from `origin`
void menuPointAt(const App& app, float lx, float ly, float origin, float& mx, float& my) {
    mx = (lx - origin) / 2, my = ly / 2;
    phoneMenuPoint(app, mx, my);
}
void menuPointFit(const App& app, float lx, float ly, float& mx, float& my) {
    mx = (lx - app.renderer.uiX()) / 2, my = ly / 2;
    if (app.renderer.phoneLayout()) mx = (mx - kPhoneFitX) / kPhoneFitK, my = (my - kPhoneFitY) / kPhoneFitK;
}
// A logical point (toLogical's) in a MuCom menu's units
void menuPoint(const App& app, float lx, float ly, float& mx, float& my) {
    mx = (lx - app.renderer.uiX()) / 2, my = ly / 2;
    phoneMenuPoint(app, mx, my);
}

void drawOptions(App& app, float, float) {
    cr::Renderer& r = app.renderer;
    cr::MenuPainter& m = app.menu;
    if (optionsAbout(app)) return; // (OptionPrc 0..2: the message window alone - drawManual)
    m.begin(r, app.windowTex, &app.font);
    const PhoneFit phoneFit(app, m); // the iPhone layout: its screen's menu coordinates
    // Behind the menu: the scene under the menu mask (drawFrame's drawMenuMask)
    r.setOrigin(r.uiX());
    // The windows slide in: rows from the left, controls from the right (WinDispOn -240 / +240)
    const float t = std::min(1.0f, app.optionsAnim / 8.0f), ease = 1 - (1 - t) * (1 - t);
    const float rowsX = -240 * (1 - ease), ctrlX = 240 * (1 - ease), a = t;
    m.picture(102, 8, 4); // "Options"
    const std::vector<OptRow> rows = optionRows(app, app.optionsPage);
    const int n = int(rows.size()), shown = std::min(n, kOptVisible);
    optionsScrollFix(app, n, false);
    { // (the port's: a darker panel under the page's name, the rows and the help line - readable
      // over any scene - and the help's lines)
        const float shade[4] = {0, 0, 0, 0.5f * a}, fade[4] = {0, 0, 0, 0.2f * a};
        const float nameW = m.textWidth(kOptionPageName[app.optionsPage], 16);
        m.quad(228, 10, nameW + 16, 24, shade, shade);
        m.quad(12, kOptRowTop - 8, 250, optRowY(shown) + 42 - kOptRowTop, shade, shade);
        m.quad(262, kOptRowTop - 8, 238, optRowY(shown) + 42 - kOptRowTop, shade, fade);
    }
    m.text(kOptionPageName[app.optionsPage], 236, 14, false, a, 16);
    for (int i = app.optionsScroll; i < n && i < app.optionsScroll + kOptVisible; ++i) {
        const OptRow& row = rows[size_t(i)];
        const float y = optRowY(i - app.optionsScroll), mid = y + 12;
        const bool cursor = app.optionsRow == i;
        const float dim = row.enabled ? a : a * 0.45f;
        m.rowBar(20 + rowsX, y, row.label, dim, cursor);
        // The control in the original's design units (buttons 8 above the bar, gauges 4 below),
        // scaled about (kOptCtrlX, the bar's middle)
        m.setScale(kOptCtrlX + ctrlX, mid, kOptCtrlScale);
        const float cx = kOptCtrlX + ctrlX;
        switch (row.kind) {
        case OptControl::Check: m.button(cx + 120, y - 8, 23, "", false, dim, cursor && row.enabled); break;
        case OptControl::Gauge: m.gauge(cx, y + 4, row.value, a, cursor); break;
        case OptControl::Pair:
            for (int k = 0; k < 2; ++k)
                m.button(cx + 120 * k, y - 8, row.words[k], row.texts[k], row.selected == k, a, false);
            break;
        case OptControl::Value: // our picker: the value on a button with the Back arrows either side
            m.button(cx + 60, y - 8, -1, row.valueTex ? std::string() : row.valueText, false, dim, cursor && row.enabled);
            if (row.valueTex) // (where the button's word goes)
                m.imagePiece(*row.valueTex, cx + 70, y, row.valueUv[0], row.valueUv[1], row.valueUv[2], row.valueUv[3], dim);
            m.piece(cx + 36, y + 4, 336, 880, 16, 16, dim);
            m.piece(cx + 180, y + 4, 352, 880, 16, 16, dim);
            break;
        }
        m.setScale(0, 0, 1);
        if (cursor && row.kind == OptControl::Pair) m.text(">", kOptCtrlX + ctrlX - 12, y + 4, false, a);
    }
    if (n > kOptVisible) { // the scroll bar: a dark track, the thumb in the buttons' teal
        float top, height, ty, th;
        optionsBar(n, app.optionsScroll, top, height, ty, th);
        const float track[4] = {0.05f, 0.07f, 0.09f, 0.75f * a}, trackEdge[4] = {0.22f, 0.25f, 0.28f, 0.75f * a};
        const float lit[4] = {0.55f, 0.92f, 0.92f, a}, deep[4] = {0.16f, 0.5f, 0.55f, a};
        m.quad(kOptBarX - 1, top - 1, kOptBarW + 2, height + 2, trackEdge, trackEdge);
        m.quad(kOptBarX, top, kOptBarW, height, track, track);
        m.quad(kOptBarX + 1, ty + 1, kOptBarW - 2, th - 2, lit, deep);
    }
    // "n / total" from the game's 14 x 14 digits, and Back
    m.digit(app.optionsPage + 1, 20, kOptPageY, a);
    m.digit(-1, 34, kOptPageY, a);
    m.digit(kOptionPages, 48, kOptPageY, a);
    m.backButton(192, kOptBackY, app.optionsRow == kOptionsBackRow, app.optionsBob, a);
    if (app.optionsRow >= 0 && app.optionsRow < n && !rows[size_t(app.optionsRow)].hint.empty()) { // (under the rows shown, clear of Back)
        const std::vector<std::string> lines = wrapText(m, rows[size_t(app.optionsRow)].hint, 470, 11, 2);
        for (size_t l = 0; l < lines.size(); ++l) m.text(lines[l], 20, optRowY(shown) + 2 + 13 * float(l), false, a, 11);
    }
    if (app.showHud) // (our key hints: off in a Release build, F3)
        m.text("Up/Down  Left/Right  Enter  Q/E or PgUp/PgDn: page  Esc: back", 20, 372, false, a * 0.8f, 9);
    if (app.saveImport.active()) {
        r.setOrigin(0);
        r.fillRect(0, 0, r.screenW(), cr::Renderer::kLogicalH, 0, 0, 0, 0.75f);
        r.setOrigin(r.uiX());
        m.picture(12, 0, 220, 1);
        std::string message = app.saveImport.state == vitaimport::Session::Picking
            ? "Choose the folder containing your decrypted Vita saves." : app.saveImport.message;
        std::replace(message.begin(), message.end(), '\n', ' ');
        const auto lines = wrapText(m, message, 455, 13, 8);
        for (size_t l = 0; l < lines.size(); ++l) m.text(lines[l], 24, 234 + 16 * float(l), false, 1, 13);
        if (app.saveImport.state == vitaimport::Session::Confirm) {
            m.plate(336, 104, 0, 0, app.saveImport.focus == 0, 1, false);
            m.plate(336, 160, 1, 1, app.saveImport.focus == 1, 1, false);
        } else if (app.saveImport.state == vitaimport::Session::Result) {
            m.plate(336, 104, -1, -1, true, 1, false, "OK");
        }
    }
    if (app.resetAsk) { // OptionPrc 9 / 10: the message board (57078, then 57079), Yes / No at (336, 104 / 160)
        std::vector<std::vector<std::string>> pages;
        app.systemText.message(app.resetAsk == 1 ? 57078 : app.resetAsk == 2 ? 57079 : 57060, app.language, nullptr, pages, nullptr);
        r.setOrigin(0); // (the rows' window goes while it asks: WinDispOff(5))
        r.fillRect(0, 0, r.screenW(), cr::Renderer::kLogicalH, 0, 0, 0, 0.75f);
        r.setOrigin(r.uiX());
        m.picture(12, 0, 220, 1);
        if (!pages.empty())
            for (size_t l = 0; l < pages[0].size(); ++l) m.text(pages[0][l], 24, 234 + 16.0f * float(l), false, 1, 14);
        m.plate(336, 104, 0, 0, app.resetConfirm == 0, 1, false);
        m.plate(336, 160, 1, 1, app.resetConfirm == 1, 1, false);
    }
    r.setOrigin(r.uiX());
}

// One 20 Hz step: the slide-in and the Back arrows' bob (0..5 and back)
void stepOptions(App& app) {
    app.saveImport.poll();
    takeVitaFolder(app);
    takeAndroidFolder(app);
    if (!app.optionsOpen) return;
    if (optionsAbout(app)) { app.optionsAnim = 0; return; } // (the page slides in once "Option" has shut)
    ++app.optionsAnim;
    app.optionsBob += app.optionsBobDir;
    if (app.optionsBob >= 5 || app.optionsBob <= 0) app.optionsBobDir = -app.optionsBobDir;
}

// OptionPrc 10, Yes to both questions: prty_init_all, removeSaveData, removeContData - every pair's
// save and the resume point go; Yes to the first asks the second (sound 2), No leaves it (2)
void resetAnswer(App& app, bool yes) {
    if (app.resetAsk == 3) { // Return to Title (OptionPrc 0xd / 0x18 / 0x19): Yes - SetFade black over 500 ms and
        // MuCom_PreReturnTitle (the music off), 0xc + 1 frames, then MuCom_ReturnTitle (stepEvents); No - the page again
        app.resetAsk = 0;
        menuSound(app, se::kChoose);
        if (!yes) return;
        playBgm(app, 1u << 27);
        app.fadeGrey = 0, app.fadeFrom = 0, app.fadeTo = 1, app.fadeFrames = 10, app.fadeT = 0;
        app.titleReturnIn = 13;
        return;
    }
    if (yes && app.resetAsk == 1) {
        app.resetAsk = 2, app.resetConfirm = 1;
        menuSound(app, se::kChoose);
        return;
    }
    app.resetAsk = 0;
    if (!yes) { menuSound(app, se::kChoose); return; }
    menuSound(app, 13); // (0x9e on Omega's numbering)
    for (int pair = 1; pair <= 4; ++pair) SDL_RemovePath(slotPath(app, pair).c_str());
    SDL_RemovePath(savePath(app, "resume.dat").c_str());
    app.game = cr::GameState();
    app.playSeconds = 0; // (prty_init_all: the play time is in the block it clears)
}

// Keys: Up/Down rows (then Back), Left/Right change, Enter activates, Q/E PgUp/PgDn pages
void optionsKey(App& app, SDL_Keycode key) {
    if (app.saveImport.active()) {
        if (app.saveImport.state == vitaimport::Session::Picking) {
            if (key == SDLK_ESCAPE || key == SDLK_BACKSPACE) app.saveImport.cancel();
        } else if (key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_UP || key == SDLK_DOWN) {
            app.saveImport.focus ^= 1;
        } else if (key == SDLK_ESCAPE || key == SDLK_BACKSPACE) saveImportAnswer(app, false);
        else if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE)
            saveImportAnswer(app, app.saveImport.focus == 0);
        return;
    }

    if (app.resetAsk) { // Reset Data's question: Yes / No
        switch (key) {
        case SDLK_UP: case SDLK_DOWN: case SDLK_LEFT: case SDLK_RIGHT: case SDLK_W: case SDLK_S: case SDLK_A: case SDLK_D:
            app.resetConfirm ^= 1, menuSound(app, se::kCursor);
            break;
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: case SDLK_E: resetAnswer(app, app.resetConfirm == 0); break;
        case SDLK_ESCAPE: case SDLK_BACKSPACE: resetAnswer(app, false); break;
        default: break;
        }
        return;
    }
    std::vector<OptRow> rows = optionRows(app, app.optionsPage);
    const int n = int(rows.size());
    int& row = app.optionsRow;
    auto at = [&]() -> OptRow* { return row >= 0 && row < n ? &rows[size_t(row)] : nullptr; };
    switch (key) {
    case SDLK_ESCAPE: case SDLK_BACKSPACE: app.optionsOpen = false; menuSound(app, se::kBack); return;
    case SDLK_UP: case SDLK_W: row = row == kOptionsBackRow ? n - 1 : row > 0 ? row - 1 : kOptionsBackRow; break;
    case SDLK_DOWN: case SDLK_S: row = row == kOptionsBackRow ? 0 : row + 1 < n ? row + 1 : kOptionsBackRow; break;
    case SDLK_HOME: row = 0; break;
    case SDLK_END: row = n - 1; break;
    // the original's page arrows sit on its Back bar, and OptionPrc plays the Back sound for them
    case SDLK_Q: case SDLK_PAGEUP: openOptions(app, app.optionsPage - 1); menuSound(app, se::kBack); return;
    case SDLK_E: case SDLK_PAGEDOWN: case SDLK_TAB: openOptions(app, app.optionsPage + 1); menuSound(app, se::kBack); return;
    case SDLK_LEFT: case SDLK_RIGHT: case SDLK_A: case SDLK_D: {
        OptRow* r = at();
        if (!r || !r->enabled || !r->act || r->kind == OptControl::Check) return;
        const int dir = (key == SDLK_RIGHT || key == SDLK_D) ? 1 : -1;
        if (r->kind == OptControl::Pair) r->act(dir > 0 ? 1 : 0);
        else r->act(dir);
        if (r->sound >= 0) menuSound(app, uint32_t(r->sound));
        return;
    }
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: {
        if (row == kOptionsBackRow) { app.optionsOpen = false; menuSound(app, se::kBack); return; }
        OptRow* r = at();
        if (!r || !r->enabled || !r->act) return;
        if (r->kind == OptControl::Pair) r->act(r->selected == 0 ? 1 : 0);
        else r->act(0);
        if (r->sound >= 0 && !r->enterSounds) menuSound(app, uint32_t(r->sound));
        return;
    }
    default: return;
    }
    optionsScrollFix(app, n, true);
    menuSound(app, se::kCursor);
}

// Which part of Options' < Back > bar a point (menu units) is on: -1 the left arrow, 0 Back, 1 the
// right arrow, 3 the bar between them, 2 off the bar (__ComUty_BackButtonProc 0x49e64, type 2: Back
// at 0xf8 (0x78 wide), the arrows at 0xc0 and 0x175 (0x33 wide) - __menuu_OptionPrc 0x6fdbc's -1 / -3 / -2)
int optionsBackPart(float mx, float my) {
    if (my < kOptBackY - 4 || my >= kOptBackY + 30 || mx < 0xc0 || mx >= 0x175 + 0x33) return 2;
    if (mx < 0xc0 + 0x33) return -1;
    if (mx >= 0x175) return 1;
    return mx >= 0xf8 && mx < 0xf8 + 0x78 ? 0 : 3;
}

// A click at frame pixel (px, py): menu units, then the control there
void optionsClick(App& app, float px, float py) {
    float lx, ly;
    app.renderer.toLogical(px, py, lx, ly);
    float mx, my;
    menuPointFit(app, lx, ly, mx, my);
    if (app.saveImport.active()) {
        if (app.saveImport.state == vitaimport::Session::Result) {
            if (mx >= 336 && mx < 472 && my >= 104 && my < 144) saveImportAnswer(app, false);
        } else if (app.saveImport.state == vitaimport::Session::Confirm && mx >= 336 && mx < 472) {
            if (my >= 104 && my < 144) saveImportAnswer(app, true);
            else if (my >= 160 && my < 200) saveImportAnswer(app, false);
        }
        return;
    }

    if (app.resetAsk) { // the Yes / No plates (0x88 x 0x28)
        if (mx >= 336 && mx < 472 && my >= 104 && my < 144) resetAnswer(app, true);
        else if (mx >= 336 && mx < 472 && my >= 160 && my < 200) resetAnswer(app, false);
        return;
    }
    // ComUty_CmdBack 2's three parts: the left arrow the page before, Back closes, the right arrow the
    // page after (the whole bar used to close Options)
    if (const int part = optionsBackPart(mx, my); part != 2) {
        if (part == -1 || part == 1) { openOptions(app, app.optionsPage + part); menuSound(app, se::kBack); }
        else if (part == 0) { app.optionsOpen = false; menuSound(app, se::kBack); }
        return;
    }
    if (my >= kOptPageY - 4 && my < kOptPageY + 18 && mx >= 16 && mx < 66) { // "n / m": the next page
        openOptions(app, app.optionsPage + 1);
        menuSound(app, se::kBack);
        return;
    }
    std::vector<OptRow> rows = optionRows(app, app.optionsPage);
    if (int(rows.size()) > kOptVisible && mx >= kOptBarX - 8 && mx < kOptBarX + kOptBarW + 8) { // the scroll bar: there, and held
        float top, height, ty, th;
        optionsBar(int(rows.size()), app.optionsScroll, top, height, ty, th);
        if (my >= top - 4 && my < top + height + 4) {
            optionsBarTo(app, my);
            app.optionsBarDrag = true;
            return;
        }
    }
    for (int i = app.optionsScroll; i < int(rows.size()) && i < app.optionsScroll + kOptVisible; ++i) {
        OptRow& r = rows[size_t(i)];
        const float y = optRowY(i - app.optionsScroll);
        if (my < y - 4 || my >= y + 28) continue;
        app.optionsRow = i;
        if (!r.enabled || !r.act) return;
        auto inX = [&](float x0, float x1) { return mx >= optCtrlX(x0) && mx < optCtrlX(x1); };
        int dir = 2; // the control hit: -1 / 0 / +1, or 2 for the row's label (just moves the cursor)
        switch (r.kind) {
        case OptControl::Check: if (inX(344, 456)) dir = 0; break;
        case OptControl::Pair:
            if (inX(224, 336)) dir = 0;
            else if (inX(344, 456)) dir = 1;
            break;
        case OptControl::Gauge: { // set the value where the track was hit (knob at x + 11 + v x 2.03)
            if (!inX(224, 444) || !r.gauge) return;
            const float design = kOptCtrlX + (mx - kOptCtrlX) / kOptCtrlScale; // back to design units
            *r.gauge = std::clamp(int(std::lround((design - 235) / 2.03f)), 0, 100);
            applyMix(app);
            saveMix(app);
            if (r.sound >= 0) menuSound(app, uint32_t(r.sound));
            return;
        }
        case OptControl::Value:
            if (inX(260, 276)) dir = -1;
            else if (inX(404, 420)) dir = 1;
            else if (inX(284, 396)) dir = 0;
            break;
        }
        if (dir == 2) { menuSound(app, se::kCursor); return; }
        r.act(dir);
        if (r.sound >= 0 && !(dir == 0 && r.enterSounds)) menuSound(app, uint32_t(r.sound));
        return;
    }
}

// Uty_TexDisp_Proc, a frame: 0 fades in over g7 frames (255 / g7 a frame), 1 holds until the
// counter passes g8, 2 fades out (255 / g9 a frame) until it reaches g7, 3 clears flag 0 and ends
void stepTexDisp(App& app) {
    App::TexDisp& t = app.texDisp;
    if (!t.on) return;
    const int in = t.g[7], hold = t.g[8], out = t.g[9];
    switch (t.state) {
    case 0:
        t.alpha = in > 0 ? 255 / in * t.counter : 0;
        if (t.counter >= in) t.alpha = 255, t.counter = 0, t.state = 1;
        break;
    case 1:
        if (hold < t.counter) t.counter = 0, t.state = 2;
        break;
    case 2:
        t.alpha = out > 0 ? std::max(0, 255 / out * (out - t.counter)) : 0;
        if (t.counter >= in) t.alpha = 0, t.counter = 0, t.state = 3;
        break;
    case 3:
        app.game.setFlag(0, false);
        t = App::TexDisp();
        return;
    default: break;
    }
    ++t.counter;
}

// The Vita's logo opcodes (FUN_81031854 / FUN_81031bee): fade in over `in` frames, hold `hold`
// (cut short by a tap or confirm), fade out over `out`, then 10 frames of black before the script
// goes on. Its pictures are full screens at their sheets' top-left, drawn to fit: the Vita's sqex /
// mve / mclogo 960 x 640 of 1024 x 1024 (centred on (480, 320); the iPad's own copies 1024 x 768,
// centred on (512, 384)), the warning (picture 254) 960 x 544, the whole of its sheet
void stepVitaLogo(App& app) {
    App::VitaLogo& l = app.vitaLogo;
    if (!l.phase) return;
    ++l.frame;
    if (l.phase == 1 && l.frame >= std::max(1, l.in)) l.phase = 2, l.frame = 0;
    else if (l.phase == 2 && (l.frame >= l.hold || l.skip)) l.phase = 3, l.frame = 0;
    else if (l.phase == 3 && l.frame >= std::max(1, l.out)) l.phase = 4, l.frame = 0;
    else if (l.phase == 4 && l.frame >= 10) {
        l.phase = 0;
        for (cr::Texture& t : l.tex) app.renderer.destroy(t);
    }
}
void drawVitaLogo(App& app) {
    const App::VitaLogo& l = app.vitaLogo;
    cr::Renderer& r = app.renderer;
    r.setOrigin(0);
    r.fillRect(0, 0, r.screenW(), cr::Renderer::kLogicalH, 0, 0, 0, 1);
    const cr::Texture& t = l.tex[std::clamp(l.slot, 0, 1)];
    const float a = l.phase == 1 ? float(l.frame) / float(std::max(1, l.in)) : l.phase == 2 ? 1.0f
                    : l.phase == 3 ? 1.0f - float(l.frame) / float(std::max(1, l.out)) : 0.0f;
    if (t.id && a > 0) {
        // the picture in its sheet, from the top-left: the warning all of it; a logo the Vita's 15/16 x
        // 10/16 of the sheet's width (960 x 640 of 1024), the iPad's all of its width by 3/4 (1024 x 768)
        const int s = std::clamp(l.slot, 0, 1);
        const float sw = float(t.width);
        const float pw = l.warning[s] ? sw : l.fromVita[s] ? sw * 15 / 16 : sw;
        const float ph = l.warning[s] ? float(t.height) : l.fromVita[s] ? sw * 10 / 16 : sw * 3 / 4;
        const float k = std::min(r.screenW() / pw, float(cr::Renderer::kLogicalH) / ph), w = pw * k, h = ph * k;
        r.drawQuadUV(t, (r.screenW() - w) / 2, (cr::Renderer::kLogicalH - h) / 2, w, h, 0, 0, pw, ph, 1, 1, 1, a);
    }
    r.setOrigin(r.uiX());
}

// Uty_TexDisp_Put: a 480 x 96 piece (w, h) is a strip: (u, v) x2 of the slot's picture, 1024 x 192
// at tex2d (2x - 32, 320 - h) of the HD 2x frame - the screen's (2x, 288); anything else the whole
// picture over the screen (tex2d (-32, -64), 1024 x 768)
void drawTexDisp(App& app) {
    const App::TexDisp& t = app.texDisp;
    if (!t.on || t.alpha <= 0 || t.g[0] < 0 || t.g[0] >= 16) return;
    const cr::Texture& tex = app.portraits[t.g[0]].tex;
    if (!tex.id) return;
    cr::Renderer& r = app.renderer;
    const float a = float(t.alpha) / 255.0f, ox = r.uiX();
    r.setOrigin(0);
    const float k = float(tex.width) / 1024.0f; // the HD sheet's pixels (1024 wide)
    if (vitaLayout(app)) { // the Vita's: a script picture (Tex2d slot kind 3, tex2d_add at scale 1) is (2x, 2y - 60) of
        // its 960 x 544 screen, 2w x 2h of the sheet from (2u, 2v) - the screen centred on ours, its pixels 768 / 544 of it
        const float s = 768.0f / 544.0f, w = 2.0f * float(t.g[5]), h = 2.0f * float(t.g[6]);
        const float x = 2.0f * float(t.g[1]) - 480.0f, y = 2.0f * float(t.g[2]) - 60.0f - 272.0f;
        r.drawQuadUV(tex, r.screenW() / 2 + x * s, cr::Renderer::kLogicalH / 2.0f + y * s, w * s, h * s, 2.0f * float(t.g[3]) * k,
                     2.0f * float(t.g[4]) * k, w * k, h * k, 1, 1, 1, a);
    } else if (t.g[5] == 480 && t.g[6] == 96)
        r.drawQuadUV(tex, ox + 2.0f * float(t.g[1]), 288, 1024, 192, 2.0f * float(t.g[3]) * k, 2.0f * float(t.g[4]) * k, 1024 * k,
                     192 * k, 1, 1, 1, a);
    else r.drawQuadUV(tex, ox, 0, 1024, 768, 0, 0, 1024 * k, 768 * k, 1, 1, 1, a);
    r.setOrigin(ox);
}

// MsgUty_TexDispProc, a frame: 1 the picture read, 2 each page turn (flag 4075) the next page (its
// frames from the start) until the pages run out - or flag 3 clears - then 3 deletes it. The frame
// steps every 20 frames, the last one holds 40, then the page's frames run again
void stepTutoTex(App& app) {
    App::TutoTex& t = app.tutoTex;
    if (!t.on) return;
    auto finish = [&] { app.renderer.destroy(t.tex); t = App::TutoTex(); };
    if (t.state == 1) t.state = 2;
    else if (t.state == 2) {
        if (app.game.flag(4075)) {
            app.game.setFlag(4075, false);
            t.timer = 20, t.frame = 0;
            if (++t.page >= t.pages) { finish(); return; }
            app.tutoPage = std::max(app.tutoPage, t.page);
            if (const char* path = SDL_getenv("CR_TUTO_SHOT"); path && t.page == 0) { // a look (tests)
                static bool once = false;
                if (!once) { once = true; t.timer = 20; }
            }
        }
        if (!app.game.flag(3)) { app.game.setFlag(4075, false); finish(); return; }
    }
    stepTutoFrames(t);
}

void stepTutoFrames(App::TutoTex& t) { // (the frame timer alone: the manual's picture too)
    if (--t.timer < 0 && t.page >= 0 && t.page < 4) {
        const int n = t.frames[t.page];
        t.timer = ++t.frame == n - 1 ? 40 : 20;
        if (t.frame >= n) t.frame = 0;
    }
}

// MsgUty_TexDispDraw: while flag 3 is set, the page's frame (frame x 384, row x 256: 384 x 256 of
// the 2048-wide sheet) at tex2d (2x, 2y) of the HD frame - the screen's (2x + 32, 2y + 64)
void drawTutoTex(App& app) {
    if (app.game.flag(3)) drawTutoPicture(app, app.tutoTex);
}

void drawTutoPicture(App& app, const App::TutoTex& t) {
    if (!t.on || !t.tex.id || t.page < 0 || t.page >= 4 || t.frames[t.page] < 0) return;
    cr::Renderer& r = app.renderer;
    const float ox = r.uiX(), k = float(t.tex.width) / 2048.0f;
    r.setOrigin(0);
    r.drawQuadUV(t.tex, ox + 2 * t.x + 32, 2 * t.y + 64, 384, 256, 384.0f * float(t.frame) * k, 256.0f * float(t.row[t.page]) * k,
                 384 * k, 256 * k);
    r.setOrigin(ox);
}

// Uty_DrawController_Put: the cross key (1864, 1584, 144 x 144 of window_hd's pixels) at (64, 384)
// and its centre (1864, 1448, 96 x 96) at (88, 408) of the HD frame - the screen's (x + 32, y + 64)
// - until the script clears flag 0. 2.2.0's draws too (FUN_00025ab8): tex2d_add(0x30, 32, 192, (928,
// 796) 96 x 96) - the grey ring - and (48, 208, (944, 724) 64 x 64) - the black button - of its
// sheet, window_hd's at twice that: the screen's (96, 448) and (128, 480)
void drawPadArt(App& app) {
    if (!app.padArt) return;
    if (!app.game.flag(0)) { app.padArt = false; return; }
    if (!app.windowTex.id) return;
    cr::Renderer& r = app.renderer;
    cr::MenuPainter& m = app.menu;
    m.begin(r, app.windowTex, &app.font);
    r.setOrigin(r.uiX());
    if (r.phoneLayout()) {
        m.pieceTo(48, 224, 96, 96, 928, 796, 96, 96, 1); // menu units: logical / 2
        m.pieceTo(64, 240, 64, 64, 944, 724, 64, 64, 1);
        return;
    }
    m.pieceTo((64 + 32) / 2.0f, (384 + 64) / 2.0f, 72, 72, 932, 792, 72, 72, 1); // menu units: logical / 2
    m.pieceTo((88 + 32) / 2.0f, (408 + 64) / 2.0f, 48, 48, 932, 724, 48, 48, 1);
}

// Uty_DrawExclamationMark_Put: the screen dimmed (black 0xc0) and the "!" (468, 440, 44 x 48) at
// tex2d (220, 180), until the script clears flag 0
void drawExclamation(App& app) {
    if (!app.exclamation) return;
    if (!app.game.flag(0)) { app.exclamation = false; return; }
    cr::Renderer& r = app.renderer;
    r.setOrigin(0);
    r.fillRect(0, 0, r.screenW(), cr::Renderer::kLogicalH, 0, 0, 0, float(0xc0) / 255.0f);
    r.setOrigin(r.uiX());
    if (!app.windowTex.id) return;
    app.menu.begin(r, app.windowTex, &app.font);
    app.menu.piece(220 + 16, 180 + 32, 468, 440, 44, 48, 1);
}

float screenFadeAlpha(const App& app) {
    if (app.fadeFrames <= 0) return 0;
    const float t = std::min(1.0f, float(app.fadeT) / float(app.fadeFrames));
    return app.fadeFrom + (app.fadeTo - app.fadeFrom) * t;
}

// The 2D is one queue sorted by draw order: the menu mask, Tex2d's portraits, the window texture
// (slot 0x30), the front fade (Screen, op 16: SetupMask 0x12250's appearance, 189), the message
// text, the window texture while NowLoading (NowLoadStart 0x7a93c: 500). The front fade: once a
// frame, over the portraits and the message panel, under the text (drawFrame decides where - the
// message's lines and name call it first)
void drawFrontFade(App& app) {
    if (app.mode != Mode::Maps || app.frontFadeDrawnAt == app.frameNo) return;
    app.frontFadeDrawnAt = app.frameNo;
    const float a = screenFadeAlpha(app);
    if (a <= 0.001f) return;
    cr::Renderer& r = app.renderer;
    const float keep = r.origin();
    r.setOrigin(0);
    r.fillRect(0, 0, r.screenW(), float(cr::Renderer::kLogicalH), app.fadeGrey, app.fadeGrey, app.fadeGrey, a);
    r.setOrigin(keep);
    app.order2d.frontFade = ++app.order2d.seq;
}

// One 20 Hz frame of the running script and of the things it set going.
// Voiced builds: a page's clips start when the page is shown (PrcMsg reaching code 0x1e), one
// after another; a new page's line replaces one still playing.
void updateVoices(App& app) {
    if (app.dialogue.empty()) {
        app.voiceQueue.clear(); // a closed/replaced scene cannot restart its remaining clips
        return;
    }
    if (!app.sound.hasVoices()) return;
    const int page = int(std::min(app.dialoguePage, app.dialogue.size() - 1));
    if (app.dialogueSerial != app.voicedSerial || page != app.voicedPage) {
        app.voicedSerial = app.dialogueSerial;
        app.voicedPage = page;
        app.voiceQueue.clear();
        app.sound.stopLine(); // an unvoiced page replaces the previous page too
        if (page < int(app.dialogueVoices.size()) && !app.dialogueVoices[page].empty()) {
            const auto& clips = app.dialogueVoices[page];
            app.sound.playLine(clips[0]);
            ++app.voicesPlayed;
            app.voiceQueue.assign(clips.begin() + 1, clips.end());
        }
    }
    if (!app.voiceQueue.empty() && !app.sound.linePlaying()) {
        app.sound.playLine(app.voiceQueue.front());
        ++app.voicesPlayed;
        app.voiceQueue.erase(app.voiceQueue.begin());
    }
}

// Pieces of window_hd.mct (2048 x 2048), measured from its alpha
extern const float kWinMessagePanel[4] = {0, 1408, 960, 192}; // grey message window
extern const float kWinSmallPanel[4] = {0, 1600, 512, 96}; // dark small window
// The message windows (the reference playthrough: speech with portraits up sits on the torn
// parchment, its name on the blue streak painted into it; other speech on the grey band, the
// name on a small dark tab). The skip button and the page's "next" diamond beside them.
constexpr float kWinParchment[4] = {1024, 248, 1024, 280};
// _PutMsg's pieces (2.2.0 0x2bf84: tex2d_add u, v, w, h on the 1024 sheet, doubled on window_hd):
// the skip button (336, 848) 32 x 32; the "next" mark - a down arrow over a bar - (512, 36) 20 x 20,
// olive on the parchment (style 2), (532, 36) white on style 0x20's band
constexpr float kWinSkip[4] = {672, 1696, 64, 64};
constexpr float kWinNext[2][4] = {{1024, 72, 40, 40}, {1064, 72, 40, 40}};
constexpr float kWinBadge[4] = {936, 880, 88, 96};         // the "!" bubble over an NPC in reach (menuf_mainPut's FollowWindow type 0: tex2d (468, 440) 44 x 48)

// A message page in the game's message window, across the whole screen's width: the parchment
// while portraits are up, else the grey band; `next`: the page waits for a tap (the diamond)
// The page's lines at (x, y): as one block, or - when a line is centred (0x17 flips a line; the
// windowless style 0x10 centres them all unless flipped) - one by one, the centred ones about the
// layout's middle (CmdFont: tex2d x 240)
void drawMessageLines(App& app, const std::vector<std::string>& page, float x, float y, float scale, bool centreAll) {
    drawFrontFade(app); // (the text over the front fade)
    app.order2d.text = ++app.order2d.seq;
    cr::Renderer& r = app.renderer;
    bool any = centreAll;
    for (const std::string& l : page) any |= !l.empty() && l[0] == '\x01';
    const std::vector<std::string> shown = typedPage(page, app.textLimit); // typing: centred on the whole line
    // (x, y) is _CmdFont's (2.2.0 0x2bdf0): the glyphs' box - the Hud's rasterised text has a
    // margin round it
    const float pad = cr::Hud::pad(scale);
    if (!any) {
        app.dialogueHud.drawAt(r, shown, x - pad, y - pad, scale, 0, 0, 0.0f);
        return;
    }
    const float step = app.dialogueHud.lineStep(scale);
    for (size_t k = 0; k < page.size() && k < app.lineHud.size(); ++k) {
        const bool flip = !page[k].empty() && page[k][0] == '\x01';
        const std::string text = flip ? page[k].substr(1) : page[k];
        cr::Hud& h = app.lineHud[k];
        float tint[3];
        app.dialogueHud.tint(tint);
        h.setTint(tint[0], tint[1], tint[2]);
        const float w = h.measure(r, {text}, scale);
        const float lx = flip != centreAll ? cr::Renderer::kLogicalW / 2 - w / 2 : x - pad;
        if (k >= shown.size()) break; // (not typed yet)
        const std::string& part = shown[k];
        const std::string drawn = flip && !part.empty() ? part.substr(1) : part;
        if (!drawn.empty()) h.drawAt(r, {drawn}, lx, y - pad + step * float(k), scale, 0, 0, 0.0f);
    }
}

// The Android build's message strip for the screen's width (tex2d_setup_tex: window_cutin_480 ..
// _1366, the first at least as wide). Width is in layout units, so the separate parchment
// also works with Android art in the iPad / iPhone layouts. Wider screens stretch the largest.
int androidCutinWidth(float wv) {
    static const int kWidths[] = {480, 800, 854, 960, 1024, 1280, 1366};
    const int* w = std::find_if(std::begin(kWidths), std::end(kWidths), [&](int k) { return int(wv) <= k; });
    return w == std::end(kWidths) ? 0 : *w;
}

const cr::Texture* androidCutin(App& app, float width) {
    const int w = androidCutinWidth(std::min(width, 1366.0f));
    if (w == 0 || !app.android.isOpen()) return nullptr;
    const std::string name = "window_cutin_" + std::to_string(w) + ".mct";
    if (name != app.cutinWideName) {
        app.renderer.destroy(app.cutinWideTex);
        cr::Image img;
        if (loadImage(app, name, img)) app.cutinWideTex = app.renderer.upload(img);
        app.cutinWideName = name;
    }
    return app.cutinWideTex.id ? &app.cutinWideTex : nullptr;
}

// The phone parchment (window_hd's (1024, 240, 960, 256): the iPhone's and the Vita's, the name streak
// painted into its top-left corner) split as Android keeps it - the streak as a piece of its own (392 x 80,
// as Android's (1024, 240)) and the paper without it - so a wider screen can have the paper scaled to it and
// the streak where the iPhone puts it. The streak is told from the paper by colour (blue / green over the
// paper's beige); under it the torn edge runs on between its ends with the tear's detail from the paper
// beside it, mirrored, and the paper's own tone column by column
bool splitParchment(App& app) {
    if (app.parchSplitFor == app.windowTex.id) return app.parchPaperTex.id && app.parchStreakTex.id;
    app.parchSplitFor = app.windowTex.id;
    app.renderer.destroy(app.parchPaperTex);
    app.renderer.destroy(app.parchStreakTex);
    cr::Image wi;
    if (!loadImage(app, "window_hd.mct", wi) || wi.rgba.empty()) return false;
    const int lw = wi.logicalW > 0 ? wi.logicalW : wi.width;
    const float k = float(wi.width) / float(std::max(lw, 1));
    auto P = [&](float u) { return int(std::lround(u * k)); };
    const int W = P(960), H = P(256), ox = P(1024), oy = P(240), BX = P(392), BY = P(80);
    if (W < 16 || ox + W > wi.width || oy + H > wi.height) return false;
    std::vector<float> px(size_t(W) * size_t(H) * 4);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W * 4; ++x) px[size_t(y) * size_t(W) * 4 + size_t(x)] = wi.rgba[(size_t(oy + y) * size_t(wi.width) + size_t(ox)) * 4 + size_t(x)];
    auto at = [&](std::vector<float>& v, int x, int y) { return &v[(size_t(y) * size_t(W) + size_t(x)) * 4]; };
    std::vector<float> m(size_t(W) * size_t(H), 0.0f);
    int s0 = W, s1 = -1;
    for (int y = 0; y < BY; ++y)
        for (int x = 0; x < BX; ++x) {
            const float* c = at(px, x, y);
            const float v = c[3] > 8 ? std::clamp((std::max(c[1], c[2]) - c[0] - 6) / 40.0f, 0.0f, 1.0f) : 0.0f;
            m[size_t(y) * size_t(W) + size_t(x)] = v;
            if (v > 0.3f) s0 = std::min(s0, x), s1 = std::max(s1, x);
        }
    if (s1 < 0) return false;
    std::vector<float> edgeY(static_cast<size_t>(W), static_cast<float>(H));
    for (int x = 0; x < W; ++x)
        for (int y = 0; y < H; ++y)
            if (at(px, x, y)[3] > 128) { edgeY[size_t(x)] = float(y); break; }
    const int L = 0, R = std::min(s1 + 3, W - 1), span = std::min(P(150), W - R - 2); // (from the piece's left edge)
    if (span < 10) return false;
    float refMean = 0;
    for (int i = 1; i <= span; ++i) refMean += edgeY[size_t(R + i)];
    refMean /= float(span);
    auto mirror = [](int i, int n) { const int t = i % (2 * n); return t < n ? t : 2 * n - 1 - t; };
    // the paper's broad tone per column (well inside it), smoothed across columns
    std::vector<float> band(size_t(W) * 3, 0.0f), tone(size_t(W) * 3, 0.0f);
    const int b0 = std::min(P(150), H - 2), b1 = std::min(P(250), H);
    for (int x = 0; x < W; ++x)
        for (int y = b0; y < b1; ++y)
            for (int c = 0; c < 3; ++c) band[size_t(x) * 3 + size_t(c)] += at(px, x, y)[c] / float(b1 - b0);
    const int kw = std::max(1, P(20));
    for (int x = 0; x < W; ++x)
        for (int c = 0; c < 3; ++c) {
            float sum = 0;
            for (int d = -kw; d <= kw; ++d) sum += band[size_t(std::clamp(x + d, 0, W - 1)) * 3 + size_t(c)];
            tone[size_t(x) * 3 + size_t(c)] = sum / float(2 * kw + 1);
        }
    std::vector<float> paper = px;
    const float taper = float(std::max(1, P(24))), deep = float(P(60)), blendLen = float(std::max(1, P(30)));
    for (int x = L; x < R; ++x) {
        const float w = std::min({1.0f, float(x - L + 1) / taper, float(R - x) / taper});
        const int c = R + 1 + mirror(x - L, span);
        // (Android's strip runs its torn top level under the streak, below the paper beside it and rising
        // where the tail ends - Part 1 66 s: about 45 px lower under the streak than past it; the piece's own
        // edge drops away to the left, under the portrait, so it is not followed there)
        const float drop = float(P(34)) * std::min(1.0f, float(R - x) / taper); // (level, rising where the tail ends)
        const float ex = edgeY[size_t(R)] + drop + w * (edgeY[size_t(c)] - refMean);
        for (int y = 0; y < H; ++y) {
            const float d = float(y) - ex;
            float* o = at(paper, x, y);
            if (d < -2) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
            const int yc = int(std::lround(edgeY[size_t(c)] + d));
            if (yc >= H) break;
            if (yc < 0) continue;
            const float* rc = at(px, c, yc);
            const float t = d < deep ? 0.0f : std::min(1.0f, (d - deep) / blendLen); // (back into the piece's own paper deeper down)
            for (int ch = 0; ch < 3; ++ch) {
                const float v = std::clamp(rc[ch] + tone[size_t(x) * 3 + size_t(ch)] - tone[size_t(c) * 3 + size_t(ch)], 0.0f, 255.0f);
                o[ch] = v * (1 - t) + at(px, x, y)[ch] * t;
            }
            o[3] = d < deep ? rc[3] : 255.0f;
        }
    }
    app.parchTop = refMean / k;
    cr::Image pi, si;
    pi.width = W, pi.height = H, pi.rgba.resize(size_t(W) * size_t(H) * 4);
    for (size_t i = 0; i < pi.rgba.size(); ++i) pi.rgba[i] = uint8_t(std::lround(std::clamp(paper[i], 0.0f, 255.0f)));
    si.width = BX, si.height = BY, si.rgba.resize(size_t(BX) * size_t(BY) * 4);
    for (int y = 0; y < BY; ++y)
        for (int x = 0; x < BX; ++x) {
            const float* c = at(px, x, y);
            uint8_t* o = &si.rgba[(size_t(y) * size_t(BX) + size_t(x)) * 4];
            o[0] = uint8_t(c[0]), o[1] = uint8_t(c[1]), o[2] = uint8_t(c[2]);
            o[3] = uint8_t(std::lround(c[3] * m[size_t(y) * size_t(W) + size_t(x)]));
        }
    if (k != 1.0f) pi.logicalW = 960, pi.logicalH = 256, si.logicalW = 392, si.logicalH = 80;
    app.parchPaperTex = app.renderer.upload(pi);
    app.parchStreakTex = app.renderer.upload(si);
    return app.parchPaperTex.id && app.parchStreakTex.id;
}

bool drawAndroidMessagePanel(App& app, float x, float y, float w, float h, float nameX, float nameY) {
    const cr::Texture* strip = androidCutin(app, w);
    if (!strip) return false;
    app.renderer.drawQuadUV(*strip, x, y, w, h, 0, 0, std::min(w, 1366.0f), h);
    static const float kWinNamePhone[4] = {1024, 240, 392, 80};
    drawWinPiece(app, kWinNamePhone, nameX, nameY, 392, 80);
    return true;
}

// _PutMsg's "next" mark (2.2.0 0x2bf84), steady (no blink), by the window's style: the parchment's
// (2) olive at tex2d (452, 296), the others' white - style 4 at y 100, a tutorial band's (8) 84
// below its top, 0x10 at 296, and 0x20 (alone) at 296; none otherwise (the grey panel 0x40, the bare
// text 0x80 / 0x200 without those bits). The iPad's (1.0.1) sits 48 above the bottom, 88 in from the right. Android
// adds its edges (ox, and oy but for styles 4 and 8). `right` / `bottom`: the safe area's edges in
// logical units; `rectY` the style's rect y (tex2d)
void drawMessageMark(App& app, int style, int rectY, float right, float bottom) {
    const cr::Renderer& r = app.renderer;
    const float ex = 2 * androidEdgeX(app), ey = 2 * androidEdgeY(app);
    const float x = right - (r.phoneLayout() ? 56.0f : 88.0f) + ex;
    float y;
    if (style & 2) y = bottom - 48 + ey;
    else if (style & 4) y = 2.0f * 100 + 64;
    else if (style & 8) y = 2.0f * float(rectY + 84) + 64;
    else if (style & 0x30) y = bottom - 48 + ey;
    else return;
    drawWinPiece(app, kWinNext[style & 2 ? 0 : 1], x, y, 40, 40);
}

// _PutMsg's skip button: tex2d (444, 0) on the iPhone - 36 in from the safe area's right, at its
// top (Android: + ox, - oy) - drawn while a scene plays
void drawMessageSkip(App& app, float right, float top) {
    if (!app.runner.running()) return;
    drawWinPiece(app, kWinSkip, right - 72 + 2 * androidEdgeX(app), top - 2 * androidEdgeY(app), 64, 64);
}

void drawMessageBox(App& app, const std::vector<std::string>& page, const std::string& speaker, bool next,
                    const std::string& pageNo) {
    cr::Renderer& r = app.renderer;
    const float W = cr::Renderer::kLogicalW;
    // The screen's edges in the 1024 layout's units - in the iPhone layout (2.2.0) the safe area
    // tex2d (0, 0)..(480, 320), which its corners keep to (the skip button at (444, 0), the "next"
    // mark at y 296)
    const bool phone = r.phoneLayout();
    const float H = phone ? 704.0f : float(cr::Renderer::kLogicalH), top = phone ? 64.0f : 0.0f;
    const float left = phone ? 32.0f : -r.uiX(), width = phone ? 960.0f : r.screenW();
    bool portraits = false;
    for (const App::Portrait& p : app.portraits) portraits = portraits || (p.tex.id && p.alpha > 0.5f);
    // PrcWin / PutWin: the window fades in and out (its alpha); PutMsg draws the text, the name and
    // the mark only while it is up (state 3) - a window never opened by a script draws as is
    const int ws = app.msgWin.state;
    const bool fading = ws == 1 || ws == 2 || ws == 4 || ws == 5, textOn = !fading;
    const float wa = fading ? app.msgWin.alpha : 1.0f;
    if (!app.windowTex.id) { // no window art: a plain box
        app.dialogueHud.setTint(1, 1, 1);
        app.dialogueHud.drawAt(r, typedPage(page, app.textLimit), 60, H - 210, 2.0f, W - 120, 170);
        return;
    }
    // __EvtOpen's style picks the window (PutWin) and the text's place (PutMsg): its rect from
    // 0xd3d30 (tex2d x, y and the text's y below it; logical = 2 tex2d + (32, 64))
    const int style = app.msgStyle;
    const int base = style & 2 ? 0 : style & 4 ? 2 : style & 8 ? 4 : style & 0x10 ? 6 : style & 0x20 ? 8 : style & 0x40 ? 10 : 0;
    static const short kRect[12][2] = {{176, 60}, {176, 60}, {8, 8},  {224, 8}, {16, 16}, {108, 16},
                                       {0, 44},   {0, 0},    {0, 264}, {0, 0},  {8, 16},  {216, 16}};
    const short* rect = kRect[base | (app.msgLines == 1)];
    const float right = left + width;
    const float ex = 2 * androidEdgeX(app); // (Android: how far the phone's edges lie outside the centred screen)
    if (base != 0 || style & 0x80 || style & 0x200) {
        const float wy = 2.0f * rect[0] + 64, ty = 2.0f * (rect[0] + rect[1]) + 64;
        float tx = 2.0f * 36 + 32;
        bool outline = false;
        if (style & 0x80 || style & 0x210) { // no window: the text alone (0x10: outlined in black)
            outline = (style & 0x10) != 0;
        } else if (style & 0x20) { // a black band across the bottom (tex2d y 252..352), the name on its edge
            // (draw_vista; Android 1.1.1 0x7d678: x - ox / scale - 1 .. 480 + ox / scale + 1, the phone's whole width)
            r.fillRect(left - ex, 2.0f * 252 + 64, width + 2 * ex, 768 - (2.0f * 252 + 64), 0, 0, 0, wa);
            if (!speaker.empty() && textOn) { // PutMsg: drawfont_l at tex2d (16, 236), just above the band
                // draw_vista2 (2.2.0 0x1c640): the grey streak under the name - window (612, 14)
                // 140 x 22 at tex2d (0, 238) (the HD binary's iPad at (-16, 238))
                static const float kWinNameStreak[4] = {1224, 28, 280, 44};
                drawWinPiece(app, kWinNameStreak, 2.0f * (phone ? 0 : -16) + 32, 2.0f * 238 + 64, 280, 44, wa);
                const float nx = 2.0f * 16 + 32, ny = 2.0f * 236 + 64, namePad = cr::Hud::pad(2.2f); // (the glyphs' left)
                drawFrontFade(app); // (the name: text, over the front fade)
                app.nameHud.setTint(0, 0, 0); // (an outline, so it reads over a bright scene too)
                for (int k = 0; k < 4; ++k) app.nameHud.drawAt(r, {speaker}, nx - namePad + (k & 1 ? 2 : -2), ny + (k & 2 ? 2 : -2), 2.2f, 0, 0, 0.0f);
                app.nameHud.setTint(1, 1, 1);
                app.nameHud.drawAt(r, {speaker}, nx - namePad, ny, 2.2f, 0, 0, 0.0f);
            }
        } else if (style & 8) { // a tutorial's band: dark, its edges soft (8 / 88 / 8 tex2d), the text further left
            // (draw_abeblack: 0xc0 at the window's alpha; Android 1.1.1 0x7d48c: x - ox / scale .. the
            // phone's right edge - its whole width)
            const float a = 0.75f * wa, y1 = wy + 16, y2 = wy + 192, bl = left - ex, bw = width + 2 * ex;
            for (int k = 0; k < 8; ++k) { // the 2D path has no per-vertex colour: the edges in strips
                const float t = (float(k) + 0.5f) / 8.0f;
                r.fillRect(bl, wy + 2.0f * float(k), bw, 2.0f, 0, 0, 0, a * t);
                r.fillRect(bl, y2 + 2.0f * float(k), bw, 2.0f, 0, 0, 0, a * (1 - t));
            }
            r.fillRect(bl, y1, bw, y2 - y1, 0, 0, 0, a);
            if (app.language == 1) tx = 2.0f * 8 + 32;
        } else if (style & 0x40) { // the grey panel (960 x 192 at its rect)
            drawWinPiece(app, kWinMessagePanel, 32, wy, 960, 192, wa);
            if (!speaker.empty() && textOn) {
                drawWinPiece(app, kWinSmallPanel, 32, wy - 36, 300, 40);
                drawFrontFade(app); // (the name: text, over the front fade)
                app.nameHud.setTint(1, 1, 1);
                app.nameHud.drawAt(r, {speaker}, 62, wy - 34, 1.9f, 0, 0, 0.0f);
            }
        }
        const bool centreAll = (style & 0x10) != 0;
        if (outline && textOn) {
            app.dialogueHud.setTint(0, 0, 0);
            for (int k = 0; k < 8; ++k) {
                static const float d[8][2] = {{-2, -2}, {0, -2}, {2, -2}, {-2, 0}, {2, 0}, {-2, 2}, {0, 2}, {2, 2}};
                drawMessageLines(app, page, tx + d[k][0], ty + d[k][1], 2.2f, centreAll);
            }
        }
        app.dialogueHud.setTint(1, 1, 1);
        if (textOn) drawMessageLines(app, page, tx, ty, 2.2f, centreAll);
        (void)pageNo; // (PutMsg draws no page count)
        if (next && textOn) drawMessageMark(app, style, rect[0], right, H);
        drawMessageSkip(app, right, top);
        return;
    }
    (void)portraits;
    float textY;
    float textX = left + 68;
    if (phone) { // 2.2.0 (FUN_0001c940, the Retina path): the parchment's 480 x 128 - window_hd
        // (1024, 240, 960, 256) - at tex2d (0, y + 16); the name (drawfont_text) at (24, 206), the
        // lines at (36, y + text y) - no +18 - (the port's font offsets as for the iPad)
        static const float kWinParchmentPhone[4] = {1024, 240, 960, 256};
        const short* pr = kRect[app.msgLines == 1];
        const float stripW = r.screenW() / r.layoutScale();
        const float stripY = 2.0f * pr[0] + 20 + 64;
        const float stripBottom = r.layoutTop() + cr::Renderer::kLogicalH / r.layoutScale();
        // Android's atlas needs its separate full-width strip and overlaid name streak even
        // when the selected layout is iPhone (including Auto), rather than Android.
        float parchK = 1, parchX = 32;
        const float parchY = 2.0f * (pr[0] + 16) + 64;
        if (!((app.androidWindow || r.androidScreen()) && !fading &&
              drawAndroidMessagePanel(app, (W - stripW) / 2, stripY, stripW, stripBottom - stripY,
                                      32, 2.0f * (pr[0] + 16) + 64))) {
            // (a screen wider than the iPhone's: the Vita stretches the piece across it, dragging the
            // streak. The port scales the paper alike on both axes, to the screen's width from its
            // left edge - no distortion, the bottom reaching the screen's - and keeps the streak where
            // the iPhone has it, as Android's separate strip and streak do)
            const float x0 = std::min(32.0f, std::floor((W - stripW) / 2)), x1 = std::max(992.0f, std::ceil((W + stripW) / 2));
            const float K = (x1 - x0) / 960.0f;
            if (K > 1.001f && splitParchment(app)) {
                const float ptop = app.parchTop * (K - 1); // (scaled about its top edge: the edge stays where the streak expects it)
                app.renderer.drawQuadUV(app.parchPaperTex, x0, parchY - ptop, 960 * K, 256 * K, 0, 0, 960, 256, 1, 1, 1, wa);
                app.renderer.drawQuadUV(app.parchStreakTex, 32, parchY, 392, 80, 0, 0, 392, 80, 1, 1, 1, wa);
            } else {
                parchK = K, parchX = x0;
                drawWinPiece(app, kWinParchmentPhone, x0, parchY, 960 * parchK, 256 * parchK, wa);
            }
        }
        if (!speaker.empty() && textOn) {
            drawFrontFade(app); // (the name: text, over the front fade)
            app.nameHud.setTint(1, 1, 1);
            const float nx = parchX + (2.0f * 24 + 32 - 32) * parchK, ny = parchY + (2.0f * 206 + 64 - parchY) * parchK;
            app.nameHud.drawAt(r, {speaker}, nx - cr::Hud::pad(2.0f), ny - 14, 2.0f, 0, 0, 0.0f);
        }
        app.dialogueHud.setTint(0.12f, 0.10f, 0.08f);
        textX = 2.0f * 36 + 32;
        textY = 2.0f * (pr[0] + pr[1]) + 64;
    } else if (true) { // the parchment (style 2, and the default)
        if (!app.androidWindow || fading || !drawAndroidMessagePanel(app, left, H - 280, width, 280, left, H - 280))
            drawWinPiece(app, kWinParchment, left, H - 280, width, 280, wa);
        if (!speaker.empty() && textOn) {
            drawFrontFade(app); // (the name: text, over the front fade)
            app.nameHud.setTint(1, 1, 1);
            app.nameHud.drawAt(r, {speaker}, left + 44 - cr::Hud::pad(2.0f), H - 274, 2.0f, 0, 0, 0.0f); // on the streak (piece y 14..52)
        }
        app.dialogueHud.setTint(0.12f, 0.10f, 0.08f); // ink
        textY = H - 196;
    } else {
        drawWinPiece(app, kWinMessagePanel, left, H - 170, width, 170);
        if (!speaker.empty()) {
            drawWinPiece(app, kWinSmallPanel, left, H - 206, 300 - left, 40);
            drawFrontFade(app); // (the name: text, over the front fade)
            app.nameHud.setTint(1, 1, 1);
            app.nameHud.drawAt(r, {speaker}, left + 30, H - 204, 1.9f, 0, 0, 0.0f);
        }
        app.dialogueHud.setTint(1, 1, 1);
        textY = H - 140;
    }
    if (textOn) drawMessageLines(app, page, textX, textY, 2.2f, false); // text and name from the screen's left
    if (next && textOn) drawMessageMark(app, style, 0, right, H); // (steady: PutMsg doesn't blink it)
    drawMessageSkip(app, right, top); // a scene is playing
}

void drawWinPiece(App& app, const float* piece, float x, float y, float w, float h, float alpha) {
    app.renderer.drawQuadUV(app.windowTex, x, y, w, h, piece[0], piece[1], piece[2], piece[3], 1, 1, 1, alpha);
}

// "[E]" over the examinable NPC in reach (the original's FollowWindow bubble). `vp` maps the
// world to the 3D rectangle (rx, ry, rw, rh) of the logical screen.
void drawPrompt(App& app, const cr::Mat4& vp, float rx, float ry, float rw, float rh) {
    app.promptX = app.promptY = -1;
    if (app.runner.running() || !app.dialogue.empty() || app.itemWindowOpen) return;
    const cr::FieldNpc* n = interactTarget(app);
    if (!n) return;
    const float k = n->model == 0 || n->model == 1 ? 1.0f : 0.0f;
    // FollowWindow::update: the bubble's point is the box's top, pos + offset + (0, sizeY, 0)
    const float p[3] = {n->pos[0] + k * n->offset[0] + n->offset[0], n->pos[1] + (k + 1) * n->offset[1] + n->box[1],
                        n->pos[2] + k * n->offset[2] + n->offset[2]};
    const float* m = vp.m;
    const float cw = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
    if (cw <= 0) return;
    const float sx = rx + ((m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12]) / cw * 0.5f + 0.5f) * rw;
    const float sy = ry + (0.5f - (m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13]) / cw * 0.5f) * rh;
    app.promptX = sx, app.promptY = sy; // (a tap within 30 of it examines: touchPadDown, as FollowWindow::update's 900)
    if (app.windowTex.id) drawWinPiece(app, kWinBadge, sx - 48, sy - 44, 88, 96); // the piece (468, 440) 44 x 48 at (x - 24, y - 22)
    else app.promptHud.drawAt(app.renderer, {"[E]"}, sx - 24, sy - 16, 2.0f, 0, 0, 0.8f);
}

void selectAction(App& app, int delta) {
    LoadedModel& lm = app.model;
    const int n = int(lm.actions.size()) + 1; // + rest pose
    lm.action = wrap(lm.action + 1 + delta, size_t(n)) - 1;
    lm.time = 0;
    updateTitle(app);
}

void drawCheckerboard(cr::Renderer& r) {
    const int W = cr::Renderer::kLogicalW, H = cr::Renderer::kLogicalH;
    for (int y = 0; y < H; y += 32)
        for (int x = 0; x < W; x += 32) {
            const float c = ((x + y) / 32) % 2 ? 0.25f : 0.18f;
            r.fillRect(float(x), float(y), 32, 32, c, c, c, 1);
        }
}

// FieldCamera::setCamera3D for a script's camera in a room without a painting: the stage view's
// viewport (the view doubled as fldread_prc does on the iPad - viewW x viewH screen pixels -
// centred on the 1024 x 768 screen, moved by the quake) with aspect viewW / viewH in whole
// numbers and the stage's FOV: a 1024 view is a 2048 x 2048 frame of which the screen shows the
// middle. That frustum, mapped onto the whole screen (wider screens see past its sides)
// FieldCamera::setCamera3D's near and far planes (2.2.0 0x413ac, 1.0.1 0x3c494 the same), from the live
// eye and target and the map: near = 1 + an offset - eye y / 10 above 200, else the eye's distance / 10
// when it is over 100 and eye and target differ in height by over 25 (not on maps 45, 86, 87), else 10;
// none on maps 19, 28, 62 and 13, 29, 46, 63, 88, 102, 106, 113; 500 on map 42 for the second pair
// (_prty + 0xc18 = 1) - far 2000, 1000 on maps 86 and 87
void fieldDepthRange(const App& app, const cr::Vec3& eye, const cr::Vec3& target, float& zNear, float& zFar) {
    const int map = app.mapNumber;
    auto in = [map](std::initializer_list<int> l) { return std::find(l.begin(), l.end(), map) != l.end(); };
    float off = 10;
    if (eye.y > 200) off = eye.y / 10;
    else {
        const cr::Vec3 d = eye - target;
        const float dist = std::sqrt(cr::dot(d, d));
        if (dist > 100 && std::fabs(eye.y - target.y) > 25 && !in({45, 86, 87})) off = dist / 10;
    }
    if (in({19, 28, 62})) off = 0;
    if (fieldRoute(app) == 1 && map == 42) off = 500;
    if (in({13, 29, 46, 63, 88, 102, 106, 113})) off = 0;
    zNear = 1 + off;
    zFar = in({86, 87}) ? 1000.0f : 2000.0f;
    if (zNear >= zFar * 0.5f) zNear = zFar * 0.5f; // (a guard: the original has none)
}

cr::Mat4 scriptCameraView(const App& app, const cr::Vec3& eye, const cr::Vec3& target) {
    const cr::StageCamera& sc = app.stageCam;
    const float SW = app.renderer.screenW(), H = cr::Renderer::kLogicalH;
    const float fz = fieldFillZoom(app);
    const float w = fieldViewW(app) * fz, h = fieldViewH(app) * fz;
    // (setCamera3D casts the corner's sum to int: a corner left of the screen moves a pixel on a
    // positive fraction - the shake's whole pixels taken against the corner, the layout's own
    // centring left as it is)
    const float cx0 = (cr::Renderer::kLogicalW - w) / 2, cy0 = (H - h) / 2;
    const float rx = app.renderer.uiX() + cx0 + (std::trunc(cx0 + app.fieldQuake.off[0]) - std::trunc(cx0));
    const float ry = cy0 + (std::trunc(cy0 + app.fieldQuake.off[1]) - std::trunc(cy0));
    cr::Mat4 toScreen;
    toScreen.m[0] = w / SW, toScreen.m[5] = h / H;
    toScreen.m[12] = (2 * rx + w) / SW - 1, toScreen.m[13] = 1 - (2 * ry + h) / H;
    const float whole = float(std::max(1, sc.viewWidth / std::max(1, sc.viewHeight)));
    float zNear, zFar;
    fieldDepthRange(app, eye, target, zNear, zFar);
    return toScreen * cr::Mat4::perspective(sc.fovDegrees * 3.14159265f / 180.0f, whole, zNear, zFar) *
           cr::Mat4::lookAt(eye, target, {0, 1, 0});
}

// btl_lose (flag 0xffd set by the defeat, btl_main 0x4b1): an ordinary battle lost - back to the
// pair's Ark (area 7 / 13 / 19 / 25, room 103, or 111 once flag 1881 (route 2) / 2281 (route 3)
// is set) at (-140, -1, 25) facing 180, a fifth of the Oz gone (the same in 1.0.1 and 2.2.0)
void loseToArk(App& app) {
    endBattle(app, false);
    const int pair = std::clamp(app.playerChar / 2, 0, 3);
    cr::WarpRequest w;
    w.valid = true;
    w.area = 7 + 6 * pair;
    w.map = (pair == 2 && app.game.flag(1881)) || (pair == 3 && app.game.flag(2281)) ? 111 : 103;
    w.pos[0] = -140, w.pos[1] = -1, w.pos[2] = 25;
    w.angleDegrees = 180;
    cr::addOz(app.game, -(app.game.oz * 20 / 100));
    performWarp(app, w, -1);
    // _btl_lose (2.2.0 0xb6f8) stores the load flags as 0x27: bit 1 runs the area's script 0
    // (fldread_evt_init2), bit 3 clear - no room entry script (EvrStart 5)
    app.pendingEntry = App::Entry::Area;
    app.lastEvent = "lost: back to the Ark";
}

// The menu asking for the mask, and its shape (as their Set / Prc / Clr calls have it): the camp,
// options, the shop, the World Select, the last battle's pairs and a battle's results the slant
// (1), the save screen and the tutorials' pictures the whole screen (0; the save screen at once,
// MuCom_MenuOneFrSetMask). -1: none
int menuMaskWanted(const App& app, bool& atOnce) {
    atOnce = false;
    if (app.mode != Mode::Maps) return -1;
    // (__menuu_SaveLoadSet sets the mask in mode 1 alone - the title's, MuCom_MenuOneFrSetMask - 2.2.0 and 1.0.1
    // alike: saving from the field (modes 2..4) leaves the scene as it is. Part 01 921 / 923 s: the hall's top
    // 50.0 / 50.1 before and under "Save data?")
    if (app.saveScreen.open) {
        atOnce = true;
        return app.saveScreen.load ? 0 : -1;
    }
    if (app.tutoTex.on) return 0;
    if (app.campOpen) return 1;
    if ((app.optionsOpen && !optionsAbout(app)) || app.shop.open || app.worldChoiceOpen || app.lastBattle.open) return 1;
    if (app.battle.active && app.battle.phase == App::Battle::Results && app.battle.result.stage != 3) return 1; // (MbResultPrc 0 .. 10)
    return -1;
}

int menuMaskAlpha(const App& app) {
    const App::MenuMask& m = app.menuMask;
    return m.from + (m.to - m.from) * std::min(m.t, m.ms) / std::max(1, m.ms);
}

// A frame: a menu opening fades the mask in (0 -> 0xa0 over 400 ms, or at once), one closing
// fades it out from where it is; a change of shape between menus keeps it
void stepMenuMask(App& app) {
    App::MenuMask& m = app.menuMask;
    bool atOnce = false;
    const int want = menuMaskWanted(app, atOnce);
    if (want >= 0 && (m.want < 0 || (atOnce && want != m.type))) m.from = 0, m.to = 0xa0, m.t = 0, m.ms = atOnce ? 1 : 400;
    else if (want < 0 && m.want >= 0) m.from = menuMaskAlpha(app), m.to = 0, m.t = 0, m.ms = 400;
    else m.t = std::min(m.t + 50, m.ms);
    if (want >= 0) m.type = want;
    m.want = want;
}

void settleMenuMask(App& app) { // (captures: the menu's mask as it stands once in)
    stepMenuMask(app);
    app.menuMask.t = app.menuMask.ms;
}

// PutMask's quad (tex2d-space +-512 x +-384 at z -1: over the 3D, under the 2D); widescreen
// carries the shapes out to the screen's sides
void drawMenuMask(App& app) {
    const int alpha = menuMaskAlpha(app);
    if (alpha <= 0) return;
    cr::Renderer& r = app.renderer;
    r.setOrigin(0);
    const float a = float(alpha) / 255.0f, SW = r.screenW(), H = cr::Renderer::kLogicalH, x0 = r.uiX();
    switch (app.menuMask.type) {
    case 1: { const float q[8] = {0, 0, x0 + 427, 0, x0 + 747, H, 0, H}; r.fillQuad(q, 0, 0, 0, a); break; }
    case 2: { const float q[8] = {x0 + 597, 0, SW, 0, SW, H, x0 + 277, H}; r.fillQuad(q, 0, 0, 0, a); break; }
    case 3: // the bottom's corners at 0x10 at most: bands down the screen
        for (int k = 0; k < 16; ++k) {
            const float t = (float(k) + 0.5f) / 16, bottom = std::min(a, 16.0f / 255.0f);
            r.fillRect(0, H * float(k) / 16, SW, H / 16 + 1, 0, 0, 0, a + (bottom - a) * t);
        }
        break;
    default: r.fillRect(0, 0, SW, H, 0, 0, 0, a); break;
    }
}

void drawFrame(App& app, bool present) {
    if (app.battle.active && app.battle.exit) { // the battle's way out, between frames
        const int e = app.battle.exit;
        app.battle.exit = 0;
        if (e == 1) endBattle(app, false);
        else if (e == 4) endBattle(app, true);
        else if (e == 3) loseToArk(app);
        else bootTitle(app);
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(app.window, &w, &h);
    auto& r = app.renderer;
    { // the display's safe area, in the window's pixels (CR_SAFE_INSETS "l,t,r,b": tests)
        float l = 0, t = 0, rr = 0, b = 0;
        int ww = 0, wh = 0;
        SDL_Rect safe;
        if (const char* e = SDL_getenv("CR_SAFE_INSETS"); e && std::sscanf(e, "%f,%f,%f,%f", &l, &t, &rr, &b) == 4) {
        } else if (SDL_GetWindowSafeArea(app.window, &safe) && SDL_GetWindowSize(app.window, &ww, &wh) && ww > 0 && wh > 0) {
            const float sx = float(w) / float(ww), sy = float(h) / float(wh);
            l = float(safe.x) * sx, t = float(safe.y) * sy, rr = float(ww - safe.x - safe.w) * sx, b = float(wh - safe.y - safe.h) * sy;
        }
        r.setSafeInsets(l, t, rr, b);
    }
    r.beginFrame(w, h);
    // the examine prompt ("!" over what can be checked): placed through the scene's camera, drawn
    // with the UI once the scene is done - not upscaled with it (FSR / DLSS / MetalFX)
    struct {
        bool on = false;
        cr::Mat4 vp;
        float x = 0, y = 0, w = 0, h = 0;
    } prompt;
    bool fieldHiddenDrawn = false; // (the field pass left the field out itself: no black over it)
    ++app.frameNo;
    auto promptLater = [&](const cr::Mat4& vp, float x, float y, float pw, float ph) { prompt.on = true, prompt.vp = vp, prompt.x = x, prompt.y = y, prompt.w = pw, prompt.h = ph; };
    // W: the game's 2D layout (1024); SW: the whole logical screen (wider in widescreen)
    const float W = cr::Renderer::kLogicalW, H = cr::Renderer::kLogicalH, SW = r.screenW();
    const Uint64 now = app.clock.ticksNS();
    const float realDt = std::min(app.lastTicks && now >= app.lastTicks ? float(double(now - app.lastTicks) * 1e-9) : 0.0f, 0.1f);
    app.lastTicks = now;
    if (app.mode == Mode::Maps && !app.booting && !app.titleOpen && (app.havePlayer || app.battle.active))
        app.playSeconds += realDt;
    // Field Speed / Battle Speed: the battles, and the field while the player walks it, faster
    const float dt = realDt * float(app.mode == Mode::Maps && app.battle.active && !app.optionsOpen && !app.manualOpen ? app.battleSpeed
                                         : padField(app)                                                           ? app.fieldSpeed
                                                                                                                   : 1);
    app.drawFrames = dt * kFramesPerSecond;

    if (app.mode == Mode::Textures) {
        drawCheckerboard(r);
        if (app.currentTex.id) {
            const float tw = float(app.currentTex.width), th = float(app.currentTex.height);
            const float s = std::min({SW / tw, H / th, 1.0f});
            r.drawQuad(app.currentTex, (SW - tw * s) / 2, (H - th * s) / 2, tw * s, th * s);
        }
    } else if (app.mode == Mode::Maps && app.battle.active) {
        drawBattle(app, dt);
    } else if (app.mode == Mode::Maps && app.booting) {
        // The title: no field; the boot script runs at 20 Hz by itself (it shows its pictures
        // through Tex2d, drawn with the 2D below)
        r.fillRect(0, 0, SW, H, 0, 0, 0, 1);
        for (app.bootClock += dt; app.bootClock >= 1.0f / kFramesPerSecond; app.bootClock -= 1.0f / kFramesPerSecond) {
            stepEvents(app);
        }
    } else if (app.mode == Mode::Maps && app.showBackground) {
        // The field as the game composes it: background, 3D through the stage camera, foreground.
        // The background is the scene rendered from that camera, so both share one rectangle: the
        // stage view. Originally (FieldCamera::setCamera3D) that is the doubled view centred on the
        // screen and moved by the scroll; the options can show the whole painting instead, at the
        // screen's height (wider screens get bars at the sides) or its width (top and bottom cut).
        r.fillRect(0, 0, SW, H, 0, 0, 0, 1);
        // A room without a painting is fully 3D: there a script's free camera films the whole screen
        // (CameraPlayer leaves the live eye / target as the last shot set them, until the map is
        // loaded again - FieldCamera keeps them; only fldread_prc copies the stage record back)
        const bool scriptCam = app.cam.set && !app.background.id;
        cr::Vec3 eye, tgt;
        cr::Mat4 mvp;
        const bool have3D = app.haveStageCam || scriptCam;
        if (have3D) {
            const cr::StageCamera& sc = app.stageCam;
            cr::Vec3 camEye, camTarget; // (the script's camera as this frame shows it)
            shownCamera(app, camEye, camTarget);
            eye = scriptCam ? camEye : cr::Vec3{sc.eye[0], sc.eye[1], sc.eye[2]};
            tgt = scriptCam ? camTarget : cr::Vec3{sc.target[0], sc.target[1], sc.target[2]};
            const float aspect = scriptCam ? SW / H : sc.viewHeight > 0 ? float(sc.viewWidth) / float(sc.viewHeight) : 1.0f;
            const float fov = app.haveStageCam ? sc.fovDegrees : 60.0f;
            float zNear, zFar; // (FieldCamera::setCamera3D's, the script's camera and the stage's alike)
            fieldDepthRange(app, eye, tgt, zNear, zFar);
            app.nearPlane = zNear, app.farPlane = zFar;
            mvp = cr::Mat4::perspective(fov * 3.14159265f / 180.0f, aspect, zNear, zFar) * cr::Mat4::lookAt(eye, tgt, {0, 1, 0});
            if (scriptCam && app.haveStageCam && sc.viewWidth > 0 && sc.viewHeight > 0) mvp = scriptCameraView(app, eye, tgt);
            r.setCamera(zNear, zFar, fov * 3.14159265f / 180.0f);
            animateModel(app, dt);
            animateNpcs(app, dt);
            updatePlayer(app, dt, std::atan2(eye.x - tgt.x, eye.z - tgt.z));
        }
        // The stage view on screen (vx, vy, vw, vh)
        float vx, vy, vw, vh;
        if (scrollingField(app)) {
            followPlayerScroll(app);
            const float fz = fieldFillZoom(app);
            vw = fieldViewW(app) * fz;
            vh = fieldViewH(app) * fz;
            float sx, sy;
            shownScroll(app, sx, sy);
            vx = (SW - vw) / 2 + sx * fz;
            vy = (H - vh) / 2 - sy * fz;
        } else {
            vw = vh = std::min(SW, H);
            vx = (SW - vw) / 2;
            vy = (H - vh) / 2;
        }
        // FieldCamera::setCameraBack / setCamera3D: the viewport moves by the quake's offset
        // (whole pixels: the corner's sum is cast to int - toward zero, so a corner left of the
        // screen moves a pixel on any positive fraction, as the original's 1-pixel tremors do)
        vx += std::trunc(vx + app.fieldQuake.off[0]) - std::trunc(vx), vy += std::trunc(vy + app.fieldQuake.off[1]) - std::trunc(vy);
        // Layers are drawn at native pixel size, centred in the stage view (viewW x viewH:
        // 1024 for most fields, 512 for d0108/d0117/d0208/d0219). 512x1024 backgrounds such
        // as d0501/d0507/d0511 occupy the middle strip x 256..768
        const float viewSize = app.haveStageCam && app.stageCam.viewWidth > 0 ? float(app.stageCam.viewWidth) : 1024.0f;
        auto drawLayer = [&](const cr::Texture& t) {
            const float k = vw / viewSize, w = t.width * k, h = t.height * k;
            r.drawQuad(t, vx + (vw - w) / 2, vy + (vh - h) / 2, w, h);
        };
        // Screen's toggle (gac + 0x3a08e in 2.2.0): drawBack (0x1d68c's caller) leaves out the painting
        // and mdl_put every model (the effects' too); npc_shodow's shadows and the scene layers are
        // drawn all the same
        const bool hidden = app.fieldHidden;
        fieldHiddenDrawn = hidden;
        if (app.background.id && !hidden) drawLayer(app.background);
        drawSpaceBackdrop(app, false); // the tower's scrolling backdrop, behind the 3D
        r.setOrigin(0);
        if (have3D) {
            if (scriptCam) {
                const bool whole = app.haveStageCam && app.stageCam.viewWidth > 0; // (scriptCameraView places the view, quake and all)
                r.begin3DRect(whole ? 0 : std::trunc(app.fieldQuake.off[0]), whole ? 0 : std::trunc(app.fieldQuake.off[1]), SW, H);
            }
            else r.begin3DRect(vx, vy, vw, vh);
            // Field props are occlusion geometry: all their materials are untextured / solid-alpha
            // and black-diffuse, placed over pillars and door frames of the painting. They only
            // write depth, so the player and NPCs disappear behind them
            if (!hidden) {
                r.setColorWrite(false);
                if (app.showProps) drawLoaded(app, app.model, mvp);
                r.setColorWrite(true);
                // mdl_put (2.2.0 0x14a70): slot 0 (the alpha model) drawn at once, then every other slot
                // into the draw queue (FUN_000948a0) in slot order - the player's (1, gac + 0x83c) before
                // the NPCs' -: a blended NPC with depth writes no longer hides the player behind it
                if (app.havePlayer && playerShown(app))
                    drawLoaded(app, app.player, mvp * playerWorld(shownWalker(app)), nullptr, shownPlayerAlpha(app), -1.0f, app.playerFaded),
                        modelDrawn(app);
                drawNpcs(app, mvp);
            }
            drawFieldShadows(app, mvp); // (draw order 990: after the models)
            if (app.puzzle.stage >= 0) puzzle::drawFx(app, mvp);
            if (!hidden) drawEffects(app, app.fieldEffects, mvp, &eye, &tgt);
            if (app.showCollision && app.collision.vao) {
                cr::Material colMat; // translucent so the painted floor shows through
                colMat.culling = 0;
                colMat.properties = 8;
                colMat.blendSrc = 4;
                colMat.blendDst = 5;
                colMat.diffuse[3] = 0.45f;
                colMat.ambient[0] = colMat.ambient[1] = colMat.ambient[2] = 0;
                r.drawMesh(app.collision, nullptr, mvp, colMat);
                if (app.triggerMesh.vao) r.drawMesh(app.triggerMesh, nullptr, mvp, colMat);
            }
            r.end3D();
            if (scriptCam) promptLater(mvp, 0, 0, SW, H);
            else promptLater(mvp, vx, vy, vw, vh);
        }
        if (app.foreground.id && !hidden) drawLayer(app.foreground);
        drawSceneEffects(app);
        r.setOrigin(0);
    } else { // Models and Maps share the 3D path
        animateModel(app, dt);
        if (app.mode == Mode::Maps) {
            animateNpcs(app, dt);
            updatePlayer(app, dt, app.yaw);
        }

        r.fillRect(0, 0, SW, H, 0.22f, 0.24f, 0.28f, 1);
        const LoadedModel& m = app.model;
        const float dist = m.radius * 2.6f * app.zoom;
        // Orbit in Y-up world space; Z-up models are rotated into it
        const cr::Mat4 world = app.zUp ? cr::Mat4::zUpToYUp() : cr::Mat4{};
        const cr::Vec3 c = app.zUp ? cr::Vec3{m.center.x, m.center.z, -m.center.y} : m.center;
        const cr::Vec3 eye{c.x + dist * std::cos(app.pitch) * std::sin(app.yaw), c.y + dist * std::sin(app.pitch),
                           c.z + dist * std::cos(app.pitch) * std::cos(app.yaw)};
        const cr::Mat4 view = cr::Mat4::lookAt(eye, c, {0, 1, 0});
        const cr::Mat4 proj = cr::Mat4::perspective(0.8f, r.aspect(), m.radius * 0.01f, m.radius * 20.0f);
        r.setCamera(m.radius * 0.01f, m.radius * 20.0f, 0.8f);
        const cr::Mat4 mvp = proj * view * world;
        r.begin3D();
        if (app.mode == Mode::Maps) {
            cr::Material occluder; // field props: shown as translucent grey in the 3D view
            occluder.culling = 0;
            occluder.properties = 8;
            occluder.blendSrc = 4;
            occluder.blendDst = 5;
            occluder.ambient[0] = occluder.ambient[1] = occluder.ambient[2] = 0.35f;
            occluder.diffuse[0] = occluder.diffuse[1] = occluder.diffuse[2] = 0;
            occluder.diffuse[3] = 0.5f;
            drawLoaded(app, m, mvp, &occluder);
            drawNpcs(app, mvp);
        } else {
            drawLoaded(app, m, mvp);
        }
        if (app.mode == Mode::Maps && app.havePlayer && playerShown(app))
            drawLoaded(app, app.player, mvp * playerWorld(shownWalker(app)), nullptr, shownPlayerAlpha(app), -1.0f, app.playerFaded), modelDrawn(app);
        if (app.mode == Mode::Maps) drawFieldShadows(app, mvp);
        if (app.mode == Mode::Maps && app.puzzle.stage >= 0) puzzle::drawFx(app, mvp);
        if (app.mode == Mode::Maps) drawEffects(app, app.fieldEffects, mvp, &eye, &c);
        if (app.mode == Mode::Maps && app.collision.vao && app.showCollision) {
            cr::Material colMat;          // opaque, untextured, double-sided, depth test + write
            colMat.culling = 0;
            colMat.properties = 8 | 16;
            colMat.ambient[0] = colMat.ambient[1] = colMat.ambient[2] = 0;
            r.drawMesh(app.collision, nullptr, mvp, colMat);
            if (app.triggerMesh.vao) r.drawMesh(app.triggerMesh, nullptr, mvp, colMat);
        }
        r.end3D();
        if (app.mode == Mode::Maps) promptLater(mvp, 0, 0, SW, H);
    }
    // On-screen status: window title bars are unreliable (Wayland) or absent (Android)
    std::vector<std::string> lines{app.status};
    if (app.mode == Mode::Maps)
        lines.insert(lines.end(), {"Left/Right: map   PgUp/PgDn: jump 10   B: background   C: collision   F: smooth filter",
                                   "M: other map using this collision   drag/wheel: camera   Tab: textures",
                                   "WASD: walk (Shift: run)   E/Enter: examine   P: player   O: party member   N: NPCs   V: volume"});
    else if (app.mode == Mode::Models)
        lines.insert(lines.end(), {"Left/Right: model   PgUp/PgDn: jump 10   N/B: animation   Space: pause",
                                   "-/=: speed   U: up-axis   F: smooth filter   drag/wheel: camera   Tab: maps"});
    else
        lines.push_back("Left/Right texture  Tab models");
    r.endScene(); // the scene is complete: upscaled to the frame (when on); the UI over it at full size
    if (prompt.on) {
        r.setOrigin(0);
        drawPrompt(app, prompt.vp, prompt.x, prompt.y, prompt.w, prompt.h);
    }
    if (app.mode == Mode::Maps) {
        if (app.fieldHidden && !app.booting && !fieldHiddenDrawn) r.fillRect(0, 0, SW, H, 0, 0, 0, 1); // Screen's toggle: the field not drawn (drawBack / mdl_put skipped)
        // (Screen's fade (189): drawFrontFade, over the portraits and the message window, under the text)
        for (app.menuMask.clock += dt; app.menuMask.clock >= 1.0f / kFramesPerSecond; app.menuMask.clock -= 1.0f / kFramesPerSecond)
            stepMenuMask(app);
        drawMenuMask(app);
    }
    r.setOrigin(r.uiX()); // the game's 2D from here on: its 1024 x 768 layout, centred
    app.order2d = App::Order2D();
    // A message window this frame (the calls of drawMessageBox below): the front fade waits for its
    // text - else it goes over the portraits at once
    const bool heldNow = app.heldPage && !(!app.runner.running() || !app.dialogue.empty() || app.lastPage.empty());
    const bool messageNow = app.mode == Mode::Maps && (!app.dialogue.empty() || (app.choiceFlag >= 0 && !app.lastPage.empty()) ||
                                                       (app.choiceFlag < 0 && (heldNow || (app.msgWin.state != 0 && app.msgWin.state != 6))));
    if (app.mode == Mode::Maps) {
        if (!app.dialogue.empty()) { // 0x1c: 1 = the left speaks, 2 = the right; the other side dims
            const size_t pi = std::min(app.dialoguePage, app.dialogue.size() - 1);
            if (pi < app.dialogueInfo.size() && app.dialogueInfo[pi].side)
                for (App::Portrait& p : app.portraits)
                    if (p.side) p.shade = (p.side < 0) == (app.dialogueInfo[pi].side == 1) ? 1.0f : 128.0f / 255.0f;
        }
        for (const App::Portrait& p : app.portraits) // Tex2d: portraits and title cards (over a battle in its talks)
            if (p.tex.id && p.alpha > 0.001f && (!app.battle.active || battletalk::on(app))) {
                if (p.flags & 0x8000) { // a full-screen card (tex2d_img_prc): the iPad's sheets hold a
                    // 1024 x 768 picture for the screen (tex2d (-32, -64)), 2.2.0's and the Vita's a 960 x
                    // 640 one for tex2d (0, 0) 480 x 320. Scaled to cover the screen - the whole width in
                    // widescreen -, centred and cut at the sides or top and bottom (the iPhone layout's
                    // screen: (32, 64) 960 x 640); the slide moves it
                    const bool phone = r.phoneLayout(), small = app.phoneData || app.vitaOnly;
                    const float pw = small ? 960.0f : 1024.0f, ph = small ? 640.0f : 768.0f;
                    const float vw = phone ? 960.0f : SW, vh = phone ? 640.0f : float(H), top = phone ? 64.0f : 0.0f;
                    const float k = phone ? vh / ph : std::max(vw / pw, vh / ph); // (the iPhone's: its height)
                    r.drawQuadUV(p.tex, 512 - pw * k / 2 + p.x, top + (vh - ph * k) / 2 + p.y, pw * k, ph * k, 0, 0, pw, ph,
                                 p.shade, p.shade, p.shade, p.alpha);
                    app.order2d.portrait = ++app.order2d.seq;
                    continue;
                }
                const float w = float(p.tex.width) * p.scale, h = float(p.tex.height) * p.scale; // zooms about its centre
                // Widescreen: side portraits keep to the screen's edges (as the later widescreen
                // release does), centre images to the middle
                const float edge = p.side < 0 ? wideLeft(app) - r.uiX() : p.side > 0 ? wideRight(app) - r.uiX() : 0.0f; // (inside the safe area)
                const float x = edge + p.x - (w - float(p.tex.width)) / 2, y = p.y - (h - float(p.tex.height)) / 2;
                r.drawQuadUV(p.tex, x, y, w, h, p.side == 1 ? float(p.tex.width) : 0, 0,
                             p.side == 1 ? -float(p.tex.width) : float(p.tex.width), float(p.tex.height), p.shade,
                             p.shade, p.shade, p.alpha); // right-hand portraits are mirrored (slot +0xe)
                app.order2d.portrait = ++app.order2d.seq;
            }
        if (app.itemWindowOpen) { // Uty_GetItem
            std::vector<std::vector<std::string>> name;
            const std::string item = itemName(app, app.itemWindowItem);
            if (app.windowTex.id) {
                drawWinPiece(app, kWinSmallPanel, W / 2 - 256, H / 2 - 60, 512, 120);
                app.itemHud.drawAt(r, {"Obtained", "  " + item}, W / 2 - 210, H / 2 - 30, 2.0f, 0, 0, 0.0f);
            } else {
                app.itemHud.drawAt(r, {"Obtained:", "  " + item, "", "          [E]"}, W / 2 - 180, H / 2 - 60, 2.0f, 360, 100);
            }
        }
        if (!messageNow) drawFrontFade(app);
    }
    if (fieldMenuWanted(app)) { // the corner button and its bar
        for (app.fieldMenu.clock += dt; app.fieldMenu.clock >= 1.0f / kFramesPerSecond; app.fieldMenu.clock -= 1.0f / kFramesPerSecond)
            stepFieldMenu(app), stepPuzzleHud(app), stepTouchPad(app);
        drawFieldMenu(app);
        drawTouchPad(app);
        r.setOrigin(r.uiX());
    } else
        app.fieldMenu.fade = 0, app.fieldMenu.clock = 0, touchPadUp(app);
    if (app.mode == Mode::Maps && app.puzzleBanner.on) { // Uty_PuzzleStart_Put, over the scene that runs it
        drawPuzzleBanner(app);
        r.setOrigin(r.uiX());
    }
    if (app.mode == Mode::Maps && app.texDisp.on) drawTexDisp(app);
    if (app.mode == Mode::Maps && app.vitaLogo.phase) drawVitaLogo(app);
    if (app.mode == Mode::Maps && app.tutoTex.on) drawTutoTex(app);
    if (app.mode == Mode::Maps && app.exclamation) drawExclamation(app);
    if (app.mode == Mode::Maps && app.padArt) drawPadArt(app);
    if (app.mode == Mode::Maps && app.battle.active) drawBattleHud(app, W, H);
    if (app.mode == Mode::Maps && app.titleOpen && !app.optionsOpen && !app.saveScreen.open && !app.resumePrompt &&
        !app.charSelectOpen && !app.scenariosOpen && !app.manualOpen)
        drawTitleMenu(app);
    if (app.optionsOpen) {
        for (app.optionsClock += dt; app.optionsClock >= 1.0f / kFramesPerSecond; app.optionsClock -= 1.0f / kFramesPerSecond)
            stepOptions(app);
        if (!app.campOpen && !app.saveScreen.open) drawOptions(app, W, H); // (else above those, below)
    }
    if (!app.hideHud && app.showHud) app.hud.draw(r, lines);
    if (app.mode == Mode::Maps && app.worldChoiceOpen) { // Util 1: the World Select
        for (app.worldSelect.clock += dt; app.worldSelect.clock >= 1.0f / kFramesPerSecond && app.worldChoiceOpen;
             app.worldSelect.clock -= 1.0f / kFramesPerSecond)
            stepWorldSelect(app);
        drawWorldSelect(app);
        r.setOrigin(r.uiX());
    }
    if (app.mode == Mode::Maps && app.choiceFlag >= 0) { // Util 3: Yes / No beside the question
        if (app.dialogue.empty() && !app.lastPage.empty()) drawMessageBox(app, app.lastPage, "", false, "");
        // __menuu_EventPut: ComUty_YesNoChoiceDraw - the plates at (336, 104) and (336, 160) of
        // window 0, which slides in from 360 to the right
        const float t = std::min(1.0f, float(app.choiceAnim) / 4.0f), dx = 360 * (1 - t) * (1 - t);
        if (app.windowTex.id) {
            app.menu.begin(r, app.windowTex, &app.font);
            r.setOrigin(r.uiX());
            const PhoneMenu phoneMenu(app, app.menu);
            // (__menuu_EventPrc 0x76e24 clears the lit plate (DAT_000d36f0) every frame and
            // ComUty_YesNoChoiceProc sets it only while a finger is down on one: touch play shows both
            // dark; keys and pads keep the cursor's lit. The Vita's EventPrc (0x81052a0a) keeps its
            // DAT_81121cc8 between frames: a cursor, lit after a tap too)
            const bool lit = !app.pointerLast || vitaLayout(app);
            app.menu.plate(336 + dx, 104, 0, 0, lit && app.choiceCursor == 0, 1, false);
            app.menu.plate(336 + dx, 160, 1, 1, lit && app.choiceCursor == 1, 1, false);
        } else {
            const float cx = W - 260, cy = H - 360;
            r.fillRect(cx, cy, 200, 110, 0.05f, 0.08f, 0.2f, 0.85f);
            app.choiceHud.drawAt(r, {std::string(app.choiceCursor == 0 ? "> " : "  ") + "Yes",
                                     std::string(app.choiceCursor == 1 ? "> " : "  ") + "No"},
                                 cx + 36, cy + 24, 2.0f, 0, 0, 0.0f);
        }
    }
    if (app.heldPage && (!app.runner.running() || !app.dialogue.empty() || app.lastPage.empty())) app.heldPage = false;
    if (app.mode == Mode::Maps && app.dialogue.empty() && app.heldPage && app.choiceFlag < 0) { // (a page that ended itself)
        std::string speaker;
        std::vector<std::vector<std::string>> name;
        if (app.lastPageInfo.nameId >= 0 && app.systemText.message(app.lastPageInfo.nameId, app.language, nullptr, name) &&
            !name.empty() && !name[0].empty())
            speaker = name[0][0];
        drawMessageBox(app, app.lastPage, speaker, false, "");
    }
    if (app.mode == Mode::Maps && !app.dialogue.empty()) {
        const size_t pi = std::min(app.dialoguePage, app.dialogue.size() - 1);
        std::string speaker; // PrcMsg 0x15: the name plate
        if (pi < app.dialogueInfo.size() && app.dialogueInfo[pi].nameId >= 0) {
            std::vector<std::vector<std::string>> name;
            if (app.systemText.message(app.dialogueInfo[pi].nameId, app.language, nullptr, name) && !name.empty() &&
                !name[0].empty())
                speaker = name[0][0];
        }
        const cr::MessageInfo* info = pageInfo(app);
        if (app.typeOn) app.textLimit = app.dialogueTyped;
        drawMessageBox(app, app.dialogue[pi], speaker, pageTyped(app) && !(info && info->closeAfter >= 0), std::string());
        app.textLimit = 1 << 30;
    } else if (app.mode == Mode::Maps && !app.heldPage && app.choiceFlag < 0 && app.msgWin.state != 0 && app.msgWin.state != 6) {
        // The window alone (PutWin draws it in every state but 6): fading in before its text, up
        // without any (the opening's black band stands 600 frames before its first line), or fading out
        drawMessageBox(app, {}, "", false, "");
    }
    drawFrontFade(app); // (189: over the message window, when no text came)
    r.setOrigin(r.uiX());
    if (app.nowLoading.on && app.windowTex.id) { // NowLoadProc (draw order 500): 8 frames of (820 + 24k, 96, 24 x 24) at tex2d (464, 320)
        app.menu.begin(r, app.windowTex, &app.font);
        r.setOrigin(r.uiX());
        const bool phone = app.renderer.phoneLayout(); // 2.2.0: (448, 288), the iPhone screen's corner
        app.menu.piece((phone ? 448 : 464) + 16, (phone ? 288 : 320) + 32, 820 + 24.0f * float(app.nowLoading.frame % 8), 96, 24, 24, 1);
    }
    // The menus over everything else (Options draws above them)
    if (app.charSelectOpen) {
        ++app.charSelectAnim;
        drawCharSelect(app);
    }
    if (app.scenariosOpen) {
        ++app.scenariosAnim;
        drawScenarios(app);
    }
    if (app.campOpen && !app.optionsOpen) {
        for (app.campClock += dt; app.campClock >= 1.0f / kFramesPerSecond && app.campOpen; app.campClock -= 1.0f / kFramesPerSecond)
            stepCamp(app);
        if (app.campOpen) drawCamp(app);
    }
    if (app.mapScreen.open) {
        for (app.mapScreen.clock += dt; app.mapScreen.clock >= 1.0f / kFramesPerSecond; app.mapScreen.clock -= 1.0f / kFramesPerSecond)
            stepMapScreen(app);
        drawMapScreen(app);
    }
    if (app.lastBattle.open) {
        for (app.lastBattle.clock += dt; app.lastBattle.clock >= 1.0f / kFramesPerSecond; app.lastBattle.clock -= 1.0f / kFramesPerSecond)
            stepLastBattle(app);
        drawLastBattle(app);
    }
    if (app.staffRoll.open) {
        const bool* keys = SDL_GetKeyboardState(nullptr); // the corner button held: Space / Enter, or the mouse there
        float mx = 0, my = 0;
        const bool held = SDL_GetMouseState(&mx, &my) & SDL_BUTTON_LMASK;
        bool corner = false;
        if (held) {
            int ww = 0, wh = 0;
            SDL_GetWindowSize(app.window, &ww, &wh);
            float lx, ly, tx, ty;
            r.toLogical(mx * (ww > 0 ? float(r.frameWidth()) / float(ww) : 1.0f), my * (wh > 0 ? float(r.frameHeight()) / float(wh) : 1.0f), lx, ly);
            logicalToTex2d(app, lx, ly, tx, ty);
            corner = app.staffRoll.phone ? tx >= 444 && tx < 476 && ty >= 0 && ty < 32 : tx >= 460 && tx < 492 && ty >= -32 && ty < 0;
        }
        app.staffRoll.fast = keys[SDL_SCANCODE_SPACE] || keys[SDL_SCANCODE_RETURN] || corner;
        for (app.staffRoll.clock += dt; app.staffRoll.clock >= 1.0f / kFramesPerSecond; app.staffRoll.clock -= 1.0f / kFramesPerSecond)
            stepStaffRoll(app);
        drawStaffRoll(app);
    }
    if (app.shop.open) {
        for (app.shopClock += dt; app.shopClock >= 1.0f / kFramesPerSecond; app.shopClock -= 1.0f / kFramesPerSecond)
            stepShop(app);
        if (app.shop.open) drawShop(app);
    }
    if (app.saveScreen.open) {
        for (app.saveClock += dt; app.saveClock >= 1.0f / kFramesPerSecond; app.saveClock -= 1.0f / kFramesPerSecond)
            stepSaveScreen(app);
        if (app.saveScreen.open) drawSaveScreen(app);
    }
    if (app.resumePrompt && !app.optionsOpen) drawResumePrompt(app);
    if (app.optionsOpen && (app.campOpen || app.saveScreen.open)) drawOptions(app, W, H);
    if (app.manualOpen) {
        for (app.manualMsg.clock += dt; app.manualMsg.clock >= 1.0f / kFramesPerSecond && app.manualOpen;
             app.manualMsg.clock -= 1.0f / kFramesPerSecond)
            stepManual(app);
        ++app.manualAnim;
        if (app.manualOpen) drawManual(app);
    }
    stepTrophies(app, realDt); // (the system's notification: over everything, at its own pace)
    drawTrophyNotice(app);
    drawStickPointer(app); // (the port's: a controller's touch in a puzzle room)
    r.setOrigin(0);
    r.endFrame(present);
}

void setMode(App& app, Mode mode) {
    app.mode = mode;
    if (mode == Mode::Models) showModel(app, app.modelIndex);
    if (mode == Mode::Maps) showMap(app, app.mapIndex);
    updateTitle(app);
}

void step(App& app, int delta) {
    if (app.mode == Mode::Textures) showTexture(app, app.texIndex + delta);
    else if (app.mode == Mode::Models) showModel(app, app.modelIndex + delta);
    else showMap(app, app.mapIndex + delta);
}

} // namespace crapp
