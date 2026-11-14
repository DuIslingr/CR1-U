// Chaos Rings HD reimplementation — entry point.
//
// Bring-up stage: an asset browser over the original dataHD files, which exercises
// the whole pipeline (data discovery, LZW, textures, models, GL, input) on every
// platform before any game logic exists.
//
// Controls:
//   Tab              switch between texture and model browsing
//   Left / Right     previous / next asset (or tap left/right half in texture mode)
//   PgUp / PgDn      jump 10 assets           --model <name>  start at that model
//   Mouse drag       orbit the model          Mouse wheel   zoom
//   U                toggle model up-axis (Y-up / Z-up)
//   F                toggle smooth texture filtering (original: nearest)
//   N / B            next / previous animation (includes the rest pose)
//   Space            confirm on the field; pause / resume in the model viewer
//   - / =            slower / faster
//   W A S D          walk the party leader on a field (Shift: run)
//   P / O            player on/off / next party character (f001-f008)
//   Esc              quit

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include "app/app.hpp"

namespace crapp {

// Video settings from the command line (after settings.ini): --original, --resolution WxH,
// --windowed / --fullscreen / --borderless, --widescreen, --fill-paintings, --render-scale %,
// --msaa n, --no-vsync
void videoArgs(App& app, int argc, char** argv) {
    App::Video& v = app.video;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const char* next = i + 1 < argc ? argv[i + 1] : nullptr;
        if (SDL_strcmp(a, "--original") == 0) v = App::Video();
        else if (SDL_strcmp(a, "--resolution") == 0 && next) {
            int w = 0, h = 0;
            if (SDL_sscanf(next, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 240) {
                v.width = std::min(w, 3840);
                v.height = std::min(h, 2160);
            }
        } else if (SDL_strcmp(a, "--windowed") == 0) v.display = 0;
        else if (SDL_strcmp(a, "--fullscreen") == 0) v.display = 1;
        else if (SDL_strcmp(a, "--borderless") == 0) v.display = 2;
        else if (SDL_strcmp(a, "--widescreen") == 0) v.widescreen = true;
        else if (SDL_strcmp(a, "--layout") == 0 && i + 1 < argc) // --layout auto | ipad | iphone | android
            v.layout = SDL_strcmp(argv[i + 1], "android") == 0 ? 3 : SDL_strcmp(argv[i + 1], "iphone") == 0 ? 2
                     : SDL_strcmp(argv[i + 1], "ipad") == 0 ? 1 : 0;
        else if (SDL_strcmp(a, "--zoom-paintings") == 0 || SDL_strcmp(a, "--fill-paintings") == 0) v.paintings = 1;
        else if (SDL_strcmp(a, "--render-scale") == 0 && next) v.renderScale = std::clamp(SDL_atoi(next), 25, 400);
        else if (SDL_strcmp(a, "--msaa") == 0 && next) v.msaa = std::clamp(SDL_atoi(next), 1, 8);
        else if (SDL_strcmp(a, "--no-vsync") == 0) v.vsync = false;
        else if (SDL_strcmp(a, "--fps") == 0 && next) v.frameLimit = std::clamp(SDL_atoi(next), 0, 1000); // 0: unlimited
        else if (SDL_strcmp(a, "--upscaler") == 0 && next) // off | fsr | fsr4 | dlss | metalfx
            v.upscaler = SDL_strcmp(next, "fsr") == 0 || SDL_strcmp(next, "fsr3") == 0 ? 1 : SDL_strcmp(next, "dlss") == 0 ? 2
                       : SDL_strcmp(next, "metalfx") == 0 ? 3 : SDL_strcmp(next, "fsr4") == 0 ? 4 : 0;
        else if (SDL_strcmp(a, "--upscale-mode") == 0 && next) { // native | quality | balanced | performance | ultra
            static const char* const kModes[] = {"native", "quality", "balanced", "performance", "ultra"};
            for (int k = 0; k < 5; ++k)
                if (SDL_strcmp(next, kModes[k]) == 0) v.upscaleMode = k;
        } else if (SDL_strcmp(a, "--sharpness") == 0 && next) v.sharpness = std::clamp(SDL_atoi(next), 0, 100);
        else if (SDL_strcmp(a, "--hd-textures") == 0) v.hdTextures = true;
        else if (SDL_strcmp(a, "--smooth-models") == 0 && next) {
            const int l = SDL_atoi(next);
            v.smoothModels = l == 2 || l == 3 ? l : 0;
        }
    }
}

// ---- A controller (the Vita's buttons) ----
// A screen that takes the cursor keys (else, on the field, the stick walks and the arrows would be
// the viewer's own keys)
bool padMenus(const App& app) {
    return app.optionsOpen || app.manualOpen || app.scenariosOpen || app.saveScreen.open || app.charSelectOpen || app.campOpen ||
           app.lastBattle.open || (app.shop.open && app.shop.phase != App::Shop::Talk) || app.resumePrompt ||
           (app.mode == Mode::Maps && (app.titleOpen || app.battle.active || app.worldChoiceOpen || app.choiceFlag >= 0)) ||
           app.puzzle.pkState == 1;
}
bool padField(const App& app) { // the field itself, nothing over it
    return app.mode == Mode::Maps && app.havePlayer && !app.titleOpen && !app.booting && !app.runner.running() &&
           app.dialogue.empty() && !app.battle.active && !app.itemWindowOpen && !padMenus(app);
}
// The menu a touch goes down on (the mouse-down's order)
int pressScreen(const App& app) {
    if (app.saveScreen.open) return kPressSave;
    if (app.charSelectOpen) return kPressCharSelect;
    if (app.campOpen) return kPressCamp;
    if (app.lastBattle.open) return kPressLastBattle;
    if (app.worldChoiceOpen) return kPressWorld;
    if (app.shop.open && app.shop.phase != App::Shop::Talk) return kPressShop;
    if (app.mode == Mode::Maps && app.battle.active) return kPressBattle;
    if (app.resumePrompt) return kPressResume;
    if (app.mode == Mode::Maps && app.titleOpen) return kPressTitle;
    return kPressNone;
}
// A battle's touch held for its menus (the command pages, lists, targets, the escape question:
// __menub_mainPrc's _pad_chk_trg_up_box0 - the lift); a talk and the one-tap phases (results, victory,
// defeat) take it as it goes down
bool battleMenuPress(const App& app) { return !battletalk::on(app) && app.battle.phase == App::Battle::Command; }
// The touch lifting on screen `screen`: its click there (the clicks test their boxes, so a touch slid
// off the control it went down on does nothing - _pad_chk_updw_box's trg)
void pressRelease(App& app, int screen, float px, float py) {
    switch (screen) {
    case kPressSave: saveScreenClick(app, px, py); break;
    case kPressCharSelect: charSelectClick(app, px, py); break;
    case kPressLastBattle: lastBattleClick(app, px, py); break;
    case kPressWorld: worldSelectClick(app, px, py); break;
    case kPressShop: shopClick(app, px, py); break;
    case kPressBattle: battleClick(app, px, py); break;
    case kPressResume: resumeClick(app, px, py); break;
    case kPressTitle: titleClick(app, px, py); break;
    case kPressChoice: { // an event's Yes / No plates (0x88 x 0x28): __menuu_EventPrc's ComUty_YesNoChoiceProc trg
        float lx, ly, mx, my;
        app.renderer.toLogical(px, py, lx, ly);
        menuPoint(app, lx, ly, mx, my);
        if (mx >= 336 && mx < 472 && my >= 104 && my < 144) answerChoice(app, true);
        else if (mx >= 336 && mx < 472 && my >= 160 && my < 200) answerChoice(app, false);
        break;
    }
    case kPressHistory: // its Back: __menuu_HistoryPrc's ComUty_BackButtonProc(1) trg
        if (historyBackAt(app, px, py)) { menuSound(app, se::kBack); closeScenarios(app); }
        break;
    case kPressOptions: optionsClick(app, px, py); break;
    case kPressMap: closeMapScreen(app); break; // (__menuu_MapPrc 1: the whole screen's box)
    default: break;
    }
}
// _ComUty_NumCountProc (2.2.0 0x4a040): a touch held on a count's < or > steps it as it goes down, then -
// its counter past 0x14 - one a frame; let go, it starts over (the shop's count, the Camp's seeds)
void pressCountStep(App& app, float dt) {
    int side = 0;
    bool shop = false;
    if (app.shop.open && app.shop.phase == App::Shop::Count) {
        shop = true;
        side = shopPressIn(app, true, 336, 60, 36, 31) ? -1 : shopPressIn(app, true, 436, 60, 36, 31) ? 1 : 0;
    } else if (app.campOpen && app.camp.page == App::Camp::Items && app.camp.phase == App::Camp::Ask && !app.camp.list.empty()) {
        const int item = app.camp.list[size_t(std::clamp(app.camp.listCursor, 0, int(app.camp.list.size()) - 1))];
        if (item >= 3 && item <= 6)
            side = campPressIn(app, wideRight(app), 336, 60, 36, 31) ? -1 : campPressIn(app, wideRight(app), 436, 60, 36, 31) ? 1 : 0;
    }
    const SDL_Keycode k = side < 0 ? SDLK_LEFT : SDLK_RIGHT;
    auto stepIt = [&] { if (shop) shopKey(app, k); else campKey(app, k); };
    if (side != app.countHeldSide) { // (held now: a step at once - its counter 0 -> 1)
        app.countHeldSide = side, app.countHeldFrames = 0, app.countClock = 0;
        if (side) stepIt();
        return;
    }
    if (!side) return;
    for (app.countClock += dt; app.countClock >= 1.0f / kFramesPerSecond; app.countClock -= 1.0f / kFramesPerSecond)
        if (++app.countHeldFrames >= 20) stepIt(); // (the counter past 0x14: one a frame)
}
void padKey(App& app, SDL_Keycode key) { // the button as its key, through the same handling
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = key;
    e.key.down = true;
    const bool keep = app.padKeyNow;
    app.padKeyNow = true; // (its Esc never quits)
    SDL_AppEvent(&app, &e);
    app.padKeyNow = keep;
}
// The title's "touch to start" in front, nothing over it
bool titleTapFront(const App& app) {
    return app.mode == Mode::Maps && app.titleOpen && app.titleStage == 0 && !app.resumePrompt && !app.resumeNotice &&
           !app.optionsOpen && !app.manualOpen && !app.scenariosOpen && !app.saveScreen.open && !app.charSelectOpen;
}
void padButton(App& app, int b) {
    const bool east = b == SDL_GAMEPAD_BUTTON_EAST, south = b == SDL_GAMEPAD_BUTTON_SOUTH;
    const bool confirm = app.padConfirmEast ? east : south, cancel = app.padConfirmEast ? south : east;
    const bool puzzle = app.puzzle.stage >= 0 && fieldMenuWanted(app) && !fieldSceneRunning(app);
    if (app.vitaLogo.phase) { if (confirm && app.vitaLogo.phase == 2) app.vitaLogo.skip = true; return; }
    if (fieldBarButton(app, b, confirm, cancel)) return;
    const bool face = confirm || cancel || b == SDL_GAMEPAD_BUTTON_WEST || b == SDL_GAMEPAD_BUTTON_NORTH || b == SDL_GAMEPAD_BUTTON_START;
    // a defeat (the Vita's _btl_main 0x513: FUN_8106c116 of □ × ○ △ START) ends on any of them
    if (face && app.mode == Mode::Maps && app.battle.active && app.battle.phase == App::Battle::Lost) {
        app.battle.lostTap = true;
        return;
    }
    // the Vita's title (__menuu_TitlePrc 1: FUN_8100876a, 0xf008 - □ △ ○ × START) starts on any of them
    if (face && titleTapFront(app)) {
        padKey(app, SDLK_RETURN);
        return;
    }
    switch (b) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: if (padMenus(app)) padKey(app, SDLK_UP); return;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: if (padMenus(app)) padKey(app, SDLK_DOWN); return;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: if (padMenus(app)) padKey(app, SDLK_LEFT); return;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: if (padMenus(app)) padKey(app, SDLK_RIGHT); return;
    case SDL_GAMEPAD_BUTTON_NORTH: // △: the menu
        if (app.campOpen) { // (the Vita's Camp reads no △ - __menuu_CampMainPrc 5: ○ and ×; × closes it there)
            if (!fieldBarPad(app)) app.campOpen = false, menuSound(app, se::kCampClose);
        }
        else if (padField(app) && fieldBarPad(app)) { // (the Vita's △, button 0x1000: the bar, as the corner button)
            if (fieldMenuWanted(app) && !fieldSceneRunning(app) && app.fieldMenu.fade > 0) toggleFieldMenu(app);
        } else if (padField(app)) padKey(app, SDLK_K);
        return;
    case SDL_GAMEPAD_BUTTON_WEST: // □: text skipped; in a puzzle room, the ability
        if (padField(app) && puzzle) padKey(app, SDLK_T);
        else if (app.mode == Mode::Maps && app.runner.running()) holdSkip(app, kSkipPad, true); // (up: SDL_EVENT_GAMEPAD_BUTTON_UP)
        return;
    // L / R: a page back / on (Options' arrows), the Camp's and the shop's arrows and counts (ComUty_BackButtonProc
    // mode 2, _ComUty_NumCountProc: 0x100 / 0x200); the field leaves them be (the viewer's map stepping)
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: padKey(app, SDLK_PAGEUP); return;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: padKey(app, SDLK_PAGEDOWN); return;
    case SDL_GAMEPAD_BUTTON_START: // START: a puzzle restarted; elsewhere the Options
        if (puzzle) padKey(app, SDLK_F5);
        else if (padField(app) || app.optionsOpen) padKey(app, SDLK_V);
        return;
    case SDL_GAMEPAD_BUTTON_BACK: // SELECT: a puzzle's help; on the field the corner bar
        if (puzzle) padKey(app, SDLK_F6);
        else if (padField(app)) padKey(app, SDLK_R);
        return;
    default: break;
    }
    if (confirm) padKey(app, SDLK_RETURN);
    else if (cancel && (padMenus(app) || app.mapScreen.open || app.staffRoll.open)) padKey(app, SDLK_ESCAPE); // (not on the field: Esc quits)
}
bool padLive(App& app) {
    if (!app.pad) return false;
    const float lx = float(SDL_GetGamepadAxis(app.pad, SDL_GAMEPAD_AXIS_LEFTX)) / 32767.0f;
    const float ly = float(SDL_GetGamepadAxis(app.pad, SDL_GAMEPAD_AXIS_LEFTY)) / 32767.0f;
    bool held = std::sqrt(lx * lx + ly * ly) >= 0.25f; // (the dead zone)
    for (auto button : {SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT})
        held = held || SDL_GetGamepadButton(app.pad, button);
    return app.padHeldOver.live(held);
}
// ---- A puzzle room's touch from a controller ----
// The puzzles are touch play (the Vita release's too: a PS TV puts a pointer on the screen for them).
// Here the right stick moves a pointer in a puzzle room and R3 held is the finger on the glass: a
// press, its slide and its release go through as the mouse's, so every touch the room takes - a
// block lifted and swiped, the ability's button, the panel, the field's pad - works the same.
bool stickPointerWanted(const App& app) {
    return app.pad && app.mode == Mode::Maps && app.puzzle.stage >= 0 && fieldMenuWanted(app) && !fieldSceneRunning(app) &&
           !app.fieldMenu.open;
}
void stickPointerEvent(App& app, Uint32 type) {
    App::StickPointer& p = app.stickPointer;
    SDL_Event e{};
    e.type = type;
    if (type == SDL_EVENT_MOUSE_MOTION) e.motion.x = p.x, e.motion.y = p.y, e.motion.state = p.down ? SDL_BUTTON_LMASK : 0;
    else e.button.x = p.x, e.button.y = p.y, e.button.button = SDL_BUTTON_LEFT, e.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
    SDL_AppEvent(&app, &e);
}
void stickPointerRelease(App& app) { // the room left or covered: a finger still down lifts, the pointer goes
    App::StickPointer& p = app.stickPointer;
    if (p.down) p.down = false, stickPointerEvent(app, SDL_EVENT_MOUSE_BUTTON_UP);
    p.shown = false;
}
void stickPointerButton(App& app, bool down) { // R3
    App::StickPointer& p = app.stickPointer;
    if (down) {
        if (!stickPointerWanted(app) || p.down) return;
        if (!p.shown || p.stage != app.puzzle.stage) {
            int ww = 0, wh = 0;
            SDL_GetWindowSize(app.window, &ww, &wh);
            if (p.stage != app.puzzle.stage) p.x = float(ww) / 2, p.y = float(wh) / 2;
            p.stage = app.puzzle.stage, p.shown = true;
        }
        p.down = true;
        stickPointerEvent(app, SDL_EVENT_MOUSE_BUTTON_DOWN);
    } else if (p.down) {
        p.down = false;
        stickPointerEvent(app, SDL_EVENT_MOUSE_BUTTON_UP);
    }
}
// Each frame: the stick moves the pointer (a dead zone of a quarter; all the way over, the window's
// height a second, gentler near the middle), the move sent as the mouse's
void stickPointerStep(App& app, float dt) {
    App::StickPointer& p = app.stickPointer;
    if (!stickPointerWanted(app)) {
        if (p.shown || p.down) stickPointerRelease(app);
        return;
    }
    const float sx = float(SDL_GetGamepadAxis(app.pad, SDL_GAMEPAD_AXIS_RIGHTX)) / 32767.0f;
    const float sy = float(SDL_GetGamepadAxis(app.pad, SDL_GAMEPAD_AXIS_RIGHTY)) / 32767.0f;
    const float mag = std::sqrt(sx * sx + sy * sy);
    if (mag < 0.25f) return;
    int ww = 0, wh = 0;
    SDL_GetWindowSize(app.window, &ww, &wh);
    if (p.stage != app.puzzle.stage) p.x = float(ww) / 2, p.y = float(wh) / 2, p.stage = app.puzzle.stage;
    p.shown = true;
    const float k = (mag - 0.25f) / 0.75f, speed = float(wh) * std::min(1.0f, k) * std::min(1.0f, k) * std::min(dt, 0.1f) / mag;
    p.x = std::clamp(p.x + sx * speed, 0.0f, float(std::max(ww - 1, 0)));
    p.y = std::clamp(p.y + sy * speed, 0.0f, float(std::max(wh - 1, 0)));
    stickPointerEvent(app, SDL_EVENT_MOUSE_MOTION);
}
// The pointer: a small cyan cross, ringed dark, a size of its own whatever the screen
void drawStickPointer(App& app) {
    const App::StickPointer& p = app.stickPointer;
    if (!p.shown || !stickPointerWanted(app)) return;
    cr::Renderer& r = app.renderer;
    int ww = 0, wh = 0;
    SDL_GetWindowSize(app.window, &ww, &wh);
    float lx, ly;
    r.toLogical(p.x * (ww > 0 ? float(r.frameWidth()) / float(ww) : 1.0f), p.y * (wh > 0 ? float(r.frameHeight()) / float(wh) : 1.0f), lx, ly);
    r.setOrigin(0);
    const float a = p.down ? 1.0f : 0.85f, arm = p.down ? 13.0f : 16.0f;
    r.fillRect(lx - arm - 2, ly - 4, 2 * arm + 4, 8, 0.02f, 0.06f, 0.1f, 0.8f * a);
    r.fillRect(lx - 4, ly - arm - 2, 8, 2 * arm + 4, 0.02f, 0.06f, 0.1f, 0.8f * a);
    r.fillRect(lx - arm, ly - 2, 2 * arm, 4, 0.3f, 0.95f, 1.0f, a);
    r.fillRect(lx - 2, ly - arm, 4, 2 * arm, 0.3f, 0.95f, 1.0f, a);
}

// The scroll list in focus (MuScrList: the only menus whose held direction repeats on the Vita -
// FUN_81008500(6, 3) from its pad handler FUN_8105880a): its cursor and length; false for anything else
bool padListFocus(const App& app, int& cur, int& n) {
    const App::Battle& b = app.battle;
    if (app.mode == Mode::Maps && b.active) {
        if (b.menu != App::Battle::Genes && b.menu != App::Battle::Items) return false;
        cur = b.pick, n = int(b.picks.size());
        return true;
    }
    if (app.campOpen) { // (the Items list, the equipment slots and lists, the plates' entries, the Status lists)
        if (app.camp.notice || !campScrollable(app)) return false;
        cur = campFocus(app), n = campScrollCount(app);
        return true;
    }
    if (app.shop.open && app.shop.phase == App::Shop::List) { cur = app.shop.listCursor, n = int(app.shop.list.size()); return true; }
    if (app.lastBattle.open && app.lastBattle.state == 3) { cur = app.lastBattle.cursor, n = int(app.lastBattle.pairs.size()); return true; }
    if (app.mode == Mode::Maps && app.worldChoiceOpen) {
        const App::WorldSelect& ws = app.worldSelect;
        if (ws.state == 1) { cur = ws.cursor, n = int(ws.worlds.size()); return true; }
        if (ws.state == 6) { cur = ws.levelCursor, n = ws.levels; return true; }
        return false;
    }
    if (app.manualOpen && app.manualMsg.state == 0) { cur = app.manualCursor, n = int(manualTopics(app).size()); return true; }
    return false;
}

void padStep(App& app, float dt) {
    // The Vita's field bar open (__menuf_mainPrc): the stick moves its cursor as the D-pad does, a step
    // a push (the reader, FUN_8106bafc, turns an axis past 0.5 into a D-pad press - its trg bits, +0x30;
    // the bar has no repeat)
    if (app.pad && fieldBarPad(app) && app.fieldMenu.open && !padMenus(app)) {
        const float bx = float(SDL_GetGamepadAxis(app.pad, SDL_GAMEPAD_AXIS_LEFTX)) / 32767.0f;
        const float by = float(SDL_GetGamepadAxis(app.pad, SDL_GAMEPAD_AXIS_LEFTY)) / 32767.0f;
        int dir = -1;
        if (std::max(std::fabs(bx), std::fabs(by)) > 0.5f)
            dir = std::fabs(by) >= std::fabs(bx) ? (by < 0 ? 0 : 1) : (bx < 0 ? 2 : 3);
        static const int kBarDir[4] = {SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT,
                                       SDL_GAMEPAD_BUTTON_DPAD_RIGHT};
        if (dir != app.padHeld) {
            app.padHeld = dir, app.padHeldFor = 0;
            if (dir >= 0) fieldBarButton(app, kBarDir[dir], false, false);
        }
        return;
    }
    if (!padLive(app) || !padMenus(app)) { app.padHeld = -1, app.padCountDir = 0; return; }
    // A count (the shop's, the Camp's seeds - _ComUty_NumCountProc, FUN_8105199e): L / R held change it
    // a step a press, then - its counter past 0x14 - one a frame (left / right the same here)
    {
        bool count = app.shop.open && app.shop.phase == App::Shop::Count;
        if (app.campOpen && app.camp.page == App::Camp::Items && app.camp.phase == App::Camp::Ask && !app.camp.list.empty()) {
            const int item = app.camp.list[size_t(std::clamp(app.camp.listCursor, 0, int(app.camp.list.size()) - 1))];
            count = item >= 3 && item <= 6;
        }
        int cd = 0;
        if (count) {
            if (SDL_GetGamepadButton(app.pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) || SDL_GetGamepadButton(app.pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) cd = -1;
            else if (SDL_GetGamepadButton(app.pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) || SDL_GetGamepadButton(app.pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) cd = 1;
        }
        if (cd != app.padCountDir) app.padCountDir = cd, app.padCountFor = 0; // (the press itself came as its button)
        else if (cd) {
            constexpr float kFrame = 1.0f / kFramesPerSecond;
            const float before = app.padCountFor;
            app.padCountFor += dt;
            if (app.padCountFor >= 20 * kFrame && std::floor((app.padCountFor - 20 * kFrame) / kFrame) != std::floor((before - 20 * kFrame) / kFrame))
                padKey(app, cd < 0 ? SDLK_LEFT : SDLK_RIGHT);
        }
    }
    const float lx = float(SDL_GetGamepadAxis(app.pad, SDL_GAMEPAD_AXIS_LEFTX)) / 32767.0f;
    const float ly = float(SDL_GetGamepadAxis(app.pad, SDL_GAMEPAD_AXIS_LEFTY)) / 32767.0f;
    int dir = -1;
    if (SDL_GetGamepadButton(app.pad, SDL_GAMEPAD_BUTTON_DPAD_UP)) dir = 0;
    else if (SDL_GetGamepadButton(app.pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) dir = 1;
    else if (SDL_GetGamepadButton(app.pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) dir = 2;
    else if (SDL_GetGamepadButton(app.pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) dir = 3;
    const bool fromPad = dir >= 0;
    if (!fromPad && std::max(std::fabs(lx), std::fabs(ly)) > 0.5f) // (the Vita's reader, FUN_8106bafc: past half, a direction)
        dir = std::fabs(ly) >= std::fabs(lx) ? (ly < 0 ? 0 : 1) : (lx < 0 ? 2 : 3);
    static const SDL_Keycode kDir[4] = {SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT};
    if (dir != app.padHeld) {
        app.padHeld = dir, app.padHeldFor = 0;
        if (dir >= 0 && !fromPad) padKey(app, kDir[dir]); // (the D-pad's first press came as its button event)
        return;
    }
    if (dir < 0) return;
    // Held: only a scroll list's up / down repeat (FUN_81008500(6, 3): after 6 frames, then every 3 -
    // 0.30 / 0.15 s), and a repeat stops at the list's end (FUN_8105880a: only a fresh press wraps)
    int cur = 0, n = 0;
    if (dir > 1 || !padListFocus(app, cur, n)) return;
    const float before = app.padHeldFor;
    app.padHeldFor += dt;
    if (app.padHeldFor >= 0.30f && std::floor((app.padHeldFor - 0.30f) / 0.15f) != std::floor((before - 0.30f) / 0.15f) &&
        (dir == 0 ? cur > 0 : cur < n - 1))
        padKey(app, kDir[dir]);
}

// Called only on the game thread. Lifecycle-thread notifications merely set inputCancel.
void consumeInputCancel(App& app) {
    if (SDL_GetCurrentThreadID() != app.gameThread || !app.inputCancel.consume()) return;
    app.dragging = app.optionsBarDrag = false;
    app.campPointer = App::CampPointer();
    app.manualPointer = App::ManualPointer();
    app.optionsGauge = App::OptGauge();
    app.press = App::Press();
    app.touchPad = App::TouchPad();
    app.tapPending = false;
    clearSkip(app);
    app.padHeld = -1, app.padHeldFor = 0;
    app.padHeldOver.cancel();
    app.pointerCancelled = true;
    app.puzzle.tapX = app.puzzle.tapY = -1;
    app.puzzle.tap = App::Puzzle::Tap(); // (CheckTap: the touch gone)
    app.fingers = App::Fingers();
    if (app.puzzle.pkState == 1 && app.puzzle.pkFromX > -1e8f) app.puzzle.pkPointerCancelled = true, app.puzzle.pkHeld = false;
}

// SDL's mobile callbacks may arrive off-thread. Rebuild GL objects only on
// the game thread, before another input event can upload or draw anything.
bool consumeGraphicsReset(App& app) {
    if (SDL_GetCurrentThreadID() != app.gameThread || !app.graphicsReset.exchange(false)) return true;
    if (app.renderer.restoreAfterContextLoss()) return true;
    SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Unable to restore the OpenGL context");
    return false;
}

} // namespace crapp

using namespace crapp;

// The game as the setup (platform/setup.hpp) knows it: the first start's on a PC, a Vita dump copied
// into vita/ on any start
static cr::SetupGame crSetupGame() {
    cr::SetupGame setup;
    setup.title = "Chaos Rings", setup.port = "CR1:U", setup.vitaId = "PCSG00497", setup.trilogyDir = "CR1";
    setup.trilogyAddon = "DIESEL1000ADDCON"; // (the Prequel Trilogy keeps the game's data in its add-on)
    setup.obbHint = "main.100.com.square_enix.chaosrings.googleplay.obb";
    setup.package = "com.square_enix.chaosrings.googleplay";
    setup.iosOk = [](const std::string& p) {
        const char* args[] = {"chaosrings", "--data", p.c_str()};
        return !cr::findDataDir(3, const_cast<char**>(args)).empty();
    };
    setup.androidOk = [](const std::string& p) {
        cr::ObbArchive probe;
        return findAndroid(p, probe, nullptr);
    };
    setup.vitaOk = [](const std::string& p) {
        for (const char* rel : {"CR1/resources/cr1.psarc", "resources/cr1.psarc", "cr1.psarc"})
            if (SDL_GetPathInfo((p + rel).c_str(), nullptr)) return true;
        return false;
    };
    return setup;
}

SDL_AppResult SDL_AppInit(void** state, int argc, char** argv) {
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetAppMetadata("Chaos Rings HD (reimplementation)", "0.1", "local.preservation.chaosrings");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_Init: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    auto* app = new App;
    *state = app;

    // Your copy, found by itself (platform/paths.hpp): ios/, android/ and vita/ in the game folder
    // (beside the program, the .app or the AppImage; on Android its folder on the shared storage) -
    // the iOS data the game, the others its add-ons (each switched off in Options), else the Vita
    // copy alone, else the Android release alone
    app->dataDir = cr::findDataDir(argc, argv);
    const std::string beside = [] {
        const char* b = SDL_GetBasePath();
        std::string s = b ? b : "";
        if (!s.empty() && s.back() != '/') s += '/';
        return s;
    }();
    auto isDir = [](const std::string& p) {
        SDL_PathInfo info;
        return SDL_GetPathInfo(p.c_str(), &info) && info.type == SDL_PATHTYPE_DIRECTORY;
    };
    // a release folder that opens: vita/ (or the folders below it), android/ ...
    auto found = [](const char* kind, const std::function<bool(const std::string&)>& opens) {
        for (const std::string& r : cr::releaseFolders(kind))
            if (opens(r)) return r;
        return std::string();
    };
    auto vitaThere = [](const std::string& p) {
        for (const char* rel : {"CR1/resources/cr1.psarc", "resources/cr1.psarc", "cr1.psarc"})
            if (SDL_GetPathInfo((p + rel).c_str(), nullptr)) return true;
        return false;
    };
    auto androidThere = [](const std::string& p) {
        cr::ObbArchive probe;
        return findAndroid(p, probe, nullptr);
    };
    app->phoneData = SDL_GetPathInfo((app->dataDir + "voiceevnt.bin").c_str(), nullptr); // 2.2.0's data
    // --vita <PCSG00497 folder | cr1.psarc> (or CR_VITA, else settings.ini's vita=; --vita none:
    // without): the Vita release's assets. With no iOS data, the Vita copy alone - also given as
    // --data - is the game (vitaOnly)
    std::string vitaArg;
    bool vitaGiven = false;
    for (int i = 1; i + 1 < argc; ++i)
        if (SDL_strcmp(argv[i], "--vita") == 0) vitaArg = argv[i + 1], vitaGiven = true;
    if (!vitaGiven)
        if (const char* env = SDL_getenv("CR_VITA")) vitaArg = env, vitaGiven = true;
    if (!vitaGiven) vitaArg = storedVitaPath();
    app->vitaPath = vitaArg == "none" ? std::string() : vitaArg; // (kept for settings.ini even if it fails to open)
    std::string vitaBeside; // (found in vita/: used, not written to settings.ini)
    if (!vitaGiven && app->vitaPath.empty()) { // (a Vita dump copied into vita/ - a phone's way -, its files encrypted: decrypted into place first)
        bool quiet = false; // (a capture's, a test's: logged, no messages)
        for (int i = 1; i < argc; ++i)
            for (const char* f : {"--capture", "--selftest", "--explore", "--export-textures", "--export-models"})
                if (SDL_strcmp(argv[i], f) == 0) quiet = true;
        for (const std::string& place : cr::gameFolders()) {
            cr::SetupResult got;
            if (cr::installDroppedVita(crSetupGame(), place, got, quiet)) break;
        }
    }
    if (!vitaGiven && app->vitaPath.empty()) vitaBeside = found("vita", vitaThere);
    if (app->dataDir.empty()) {
        std::string dataArg;
        for (int i = 1; i + 1 < argc; ++i)
            if (SDL_strcmp(argv[i], "--data") == 0) dataArg = argv[i + 1];
        for (const std::string& p : {dataArg, app->vitaPath, vitaBeside}) {
            if (p.empty()) continue;
            app->vitaOnly = true;
            if (openVita(*app, p)) break;
            app->vitaOnly = false;
        }
        if (app->vitaOnly) SDL_Log("data: the Vita copy alone (%s)", app->vitaRoot.c_str());
        // else the Android release alone (--data / --android / CR_ANDROID / settings.ini's android=
        // / android/ beside the build: a folder with its main.*.obb and its APK, or the OBB)
        if (!app->vitaOnly) {
            std::vector<std::string> tries = {dataArg};
            for (int i = 1; i + 1 < argc; ++i)
                if (SDL_strcmp(argv[i], "--android") == 0) tries.push_back(argv[i + 1]);
            if (const char* env = SDL_getenv("CR_ANDROID")) tries.push_back(env);
            tries.push_back(storedSetting("android="));
            tries.push_back(found("android", androidThere));
            for (const std::string& p : tries) {
                if (p.empty() || p == "none") continue;
                app->androidOnly = true;
                if (openAndroid(*app, p)) break;
                app->androidOnly = false;
            }
            if (app->androidOnly) {
                app->phoneData = true; // (2.2.0's data: voiceevnt.bin and the iPhone layout's art)
                SDL_Log("data: the Android release alone (%s)", app->android.path().c_str());
            }
        }
    } else if (!app->vitaPath.empty())
        openVita(*app, app->vitaPath);
    else if (!vitaBeside.empty()) {
        openVita(*app, vitaBeside);
        app->vitaPath.clear(); // (openVita takes it as the setting)
    }
    if (app->vitaOnly && app->vitaPath == app->vitaLoaded && app->vitaLoaded == vitaBeside) app->vitaPath.clear();
    // --android <folder | main.*.obb> (or CR_ANDROID, else settings.ini's android=; none: without):
    // the Android release's expansion file, its table from the APK beside it
    {
        std::string a;
        bool given = false;
        for (int i = 1; i + 1 < argc; ++i)
            if (SDL_strcmp(argv[i], "--android") == 0) a = argv[i + 1], given = true;
        if (!given)
            if (const char* env = SDL_getenv("CR_ANDROID")) a = env, given = true;
        if (!given) a = storedSetting("android=");
        if (!app->androidOnly) app->androidPath = a == "none" ? std::string() : a;
        if (!app->androidPath.empty() && !app->vitaOnly && !app->androidOnly) openAndroid(*app, app->androidPath);
        else if (!given && a.empty() && !app->vitaOnly && !app->androidOnly) // (found in android/: used, not written to settings.ini)
            if (const std::string f = found("android", androidThere); !f.empty() && openAndroid(*app, f)) app->androidPath.clear();
    }
    if (const char* sd = SDL_getenv("CR_SAVE_DIR")) { // save.dat / resume.dat elsewhere (tests)
        app->saveDirOverride = sd;
        if (!app->saveDirOverride.empty() && app->saveDirOverride.back() != '/') app->saveDirOverride += '/';
    }
    // the HD textures: textures/hd/ in the game folder (CR_HD_TEXTURES elsewhere); the older
    // hd-textures/ beside the program or in the profile folder while there is no textures/hd/
    bool hdBundled = false;
    if (const char* hd = SDL_getenv("CR_HD_TEXTURES")) app->hdDir = hd;
    else if (isDir(cr::texturesFolder("hd"))) app->hdDir = cr::texturesFolder("hd"), hdBundled = true;
    else if (!beside.empty() && isDir(beside + "hd-textures")) app->hdDir = beside + "hd-textures", hdBundled = true;
    else if (isDir(cr::saveDir() + "hd-textures")) app->hdDir = cr::saveDir() + "hd-textures";
    else app->hdDir = cr::texturesFolder("hd");
    if (!app->hdDir.empty() && app->hdDir.back() != '/') app->hdDir += '/';
    // the replacement models: models/hd/ in the game folder (CR_HD_MODELS elsewhere)
    if (const char* hm = SDL_getenv("CR_HD_MODELS")) app->modelsDir = hm;
    else app->modelsDir = cr::modelsFolder("hd");
    if (!app->modelsDir.empty() && app->modelsDir.back() != '/') app->modelsDir += '/';
    if (app->dataDir.empty()) {
        const std::vector<std::string> g = cr::gameFolders();
#ifdef SDL_PLATFORM_ANDROID
        if (!g.empty()) // (the folders made, to be seen over USB)
            for (const char* f : {"ios", "android", "vita"}) SDL_CreateDirectory((g.front() + f).c_str());
#endif
#if !defined(SDL_PLATFORM_IOS)
        // The first start without a copy: where is it? (platform/setup.hpp - what's chosen copied into
        // the game folder's ios/, android/, vita/: found as on any start - a phone's too)
        static bool setupAsked = false;
        bool plain = true; // (not a test's or a tool's run)
        for (int i = 1; i < argc; ++i)
            for (const char* f : {"--capture", "--selftest", "--explore", "--export-textures", "--export-models", "--data", "--vita", "--android"})
                if (SDL_strcmp(argv[i], f) == 0) plain = false;
        if (!setupAsked && (plain || cr::setupScripted()) && !SDL_getenv("CR_DATA")) {
            setupAsked = true;
            const cr::SetupGame setup = crSetupGame();
            cr::SetupResult got;
#ifdef SDL_PLATFORM_ANDROID
            const std::string fallback; // (a phone's game folder is always its own: no other)
#else
            const std::string fallback = cr::saveDir();
#endif
            if (!cr::runSetup(setup, g.empty() ? cr::saveDir() : g.front(), fallback, got)) return SDL_APP_SUCCESS; // (Quit)
            SDL_Log("setup: iOS %s, Android %s, Vita %s", got.ios.c_str(), got.android.c_str(), got.vita.c_str());
            if (cr::setupScripted()) { // (a test's: the setup alone - whether a start finds the copy now)
                const char* noArgs[] = {argv[0]};
                const std::string d = cr::findDataDir(1, const_cast<char**>(noArgs));
                const bool found = !d.empty() || !cr::releaseFolders("android").empty() || !cr::releaseFolders("vita").empty();
                SDL_Log("setup: the game: %s", found ? (d.empty() ? "android/ or vita/" : d.c_str()) : "(none)");
                return found ? SDL_APP_SUCCESS : SDL_APP_FAILURE;
            }
            delete app;
            *state = nullptr;
            return SDL_AppInit(state, argc, argv); // (found now, as on any start)
        }
#endif
        std::string msg = "Could not find your copy of the game.\n\nPut any one of these in the folder here:\n" +
                          (g.empty() ? std::string("(no folder)") : g.front()) +
                          "\n\n  ios/      the iPad's ChaosRingsHD.app (or the iPhone's ChaosRings.app, or the unpacked .ipa)"
                          "\n  android/  the Android release's main.100.com.square_enix.chaosrings.googleplay.obb and its APK"
                          "\n  vita/     the Vita's PCSG00497 folder, or a NoNpDrm dump's folder (the Prequel Trilogy's with its"
                          "\n            addcont/ beside its app/: decrypted here on the next start)\n\nAlso looked in:";
        for (size_t k = 1; k < g.size(); ++k) msg += "\n" + g[k];
        msg += "\n\nThe README's \"Your copy\" has the details.";
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Chaos Rings", msg.c_str(), nullptr);
        return SDL_APP_FAILURE;
    }

    // --renderer vulkan | gl (or CR_RENDERER): Vulkan by default, OpenGL if Vulkan can't start
    cr::Backend want = cr::Backend::Vulkan;
    const char* choice = SDL_getenv("CR_RENDERER");
    for (int i = 1; i + 1 < argc; ++i)
        if (SDL_strcmp(argv[i], "--renderer") == 0) choice = argv[i + 1];
    if (choice && (SDL_strcasecmp(choice, "gl") == 0 || SDL_strcasecmp(choice, "opengl") == 0)) want = cr::Backend::OpenGL;
    cr::useBundledVulkan(); // (macOS: the .app's own loader)
    for (cr::Backend b : {want, cr::Backend::OpenGL}) {
        const uint64_t flags = cr::Renderer::windowFlags(b);
        if (!flags) continue; // not built in
        app->window = SDL_CreateWindow("Chaos Rings", 1024, 768,
                                       SDL_WindowFlags(flags) | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY
#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_IOS)
                                           | SDL_WINDOW_FULLSCREEN // (the system's bars hidden from the start)
#endif
                                       );
        if (!app->window) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateWindow: %s", SDL_GetError());
            continue;
        }
#ifdef CR_HAVE_VULKAN
        { // FSR 4's files (AMD's DLLs, Linux's ffx-vk-linux): fsr4/ in the game folder, beside the executable, CR_FSR4_DIR
            std::vector<std::string> dirs;
            if (const char* env = SDL_getenv("CR_FSR4_DIR"); env && *env) dirs.push_back(env);
            for (const std::string& g : cr::gameFolders()) dirs.push_back(g + (g.empty() || g.back() == '/' || g.back() == '\\' ? "" : "/") + "fsr4/");
            if (const char* base = SDL_GetBasePath()) dirs.push_back(std::string(base) + "fsr4/");
            cr::setFsr4Dirs(dirs);
        }
#endif
        { // the Vulkan pipelines compiled in earlier runs: the profile's, as settings.ini is kept - not a
          // capture's, a test's, an explore's or an export's; CR_PIPELINE_CACHE names another file, "" none
            bool kept = true;
            for (int i = 1; i < argc; ++i)
                for (const char* f : {"--capture", "--selftest", "--explore", "--export-textures", "--export-models"})
                    if (SDL_strcmp(argv[i], f) == 0) kept = false;
            if (const char* pc = SDL_getenv("CR_PIPELINE_CACHE")) cr::setPipelineCacheFile(pc);
            else cr::setPipelineCacheFile(kept ? userDir() + "vulkan-pipelines.bin" : std::string());
        }
        if (app->renderer.init(app->window, b)) {
            app->rendererReady = true;
            break;
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s renderer failed to start", cr::backendName(b));
        SDL_DestroyWindow(app->window);
        app->window = nullptr;
        if (b == cr::Backend::OpenGL) break;
    }
    if (!app->rendererReady) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "no renderer could start");
        return SDL_APP_FAILURE;
    }
    if (!app->clock.start()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "lifecycle clock: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    app->gameThread = SDL_GetCurrentThreadID();
    app->inputCancel.start(SDL_GetWindowID(app->window));
    SDL_Log("renderer: %s", app->renderer.description().c_str());
    // Captures and tests start from the original presentation; the command line can change it
    videoArgs(*app, argc, argv);
    applyVideo(*app);
    SDL_Log("data: %s", app->dataDir.c_str());

    if (app->vitaOnly) // the archive's files, by their iOS names (.gxt as .mct)
        for (const std::string& n : app->vita.names()) {
            const std::string name = n.size() > 4 && n.compare(n.size() - 4, 4, ".gxt") == 0 ? n.substr(0, n.size() - 4) + ".mct" : n;
            collectAssets(app, nullptr, name.c_str());
        }
    else if (app->androidOnly) // its files, as they are named
        for (const std::string& n : app->android.names()) collectAssets(app, nullptr, n.c_str());
    else SDL_EnumerateDirectory(app->dataDir.c_str(), collectAssets, app);
    app->systemText.load(app->dataDir);
    // The game's text in every language: a system font (CR_FONT overrides), else the 8x8 font
    cr::loadSystemFonts(app->fontLatin, app->fontCjk, app->fontCjkBold);
    if (app->fontCjk.loaded()) SDL_Log("font: %s", app->fontCjk.path().c_str());
    else SDL_Log("no CJK font found (set CR_FONT): Japanese, Chinese and Korean text unavailable");
    if (app->fontLatin.loaded()) SDL_Log("latin font: %s", app->fontLatin.path().c_str());
    else SDL_Log("no Arial / Liberation Sans found (set CR_LATIN_FONT): Latin text in the CJK font, wider than the original's");
    if (app->fontCjkBold.loaded()) SDL_Log("bold font: %s", app->fontCjkBold.path().c_str());
    app->font.assign(app->fontLatin.loaded() ? app->fontLatin : app->fontCjk); // (applyLanguage picks the language's)
    // (the options menu is the engine's own, column-aligned for the 8x8 font)
    for (cr::Hud* h : {&app->dialogueHud, &app->promptHud, &app->itemHud, &app->choiceHud, &app->nameHud,
                       &app->pageHud, &app->battleHud})
        h->setFont(&app->font);
    for (cr::Hud& h : app->lineHud) h.setFont(&app->font);
    // --language n (or CR_LANGUAGE): 0 Japanese, 1 English, ... 7 Korean (cr::kLanguageName)
    int languageArg = -1;
    if (const char* env = SDL_getenv("CR_LANGUAGE")) languageArg = SDL_atoi(env);
    for (int i = 1; i + 1 < argc; ++i)
        if (SDL_strcmp(argv[i], "--language") == 0) languageArg = SDL_atoi(argv[i + 1]);
    if (languageArg >= 0) app->language = languageArg;
    applyLanguage(*app);
    const bool hdTexturesAtFirstUpload = app->video.hdTextures; // (the UI atlas', reloaded after loadMix if it changes)
    {
        cr::Image wi;
        if (loadImage(*app, "window_hd.mct", wi, &app->androidWindow)) app->windowTex = app->renderer.upload(wi), noteWindowAtlas(*app, wi);
    }
    std::sort(app->textures.begin(), app->textures.end());
    std::sort(app->models.begin(), app->models.end());
    std::sort(app->maps.begin(), app->maps.end());
    SDL_Log("%zu textures, %zu models", app->textures.size(), app->models.size());

    for (int i = 1; i < argc; ++i)
        if (SDL_strcmp(argv[i], "--no-player") == 0) app->showPlayer = false;
    for (int i = 1; i < argc; ++i)
        if (SDL_strcmp(argv[i], "--no-props") == 0) app->showProps = false;
    for (int i = 1; i + 1 < argc; ++i)
        if (SDL_strcmp(argv[i], "--message") == 0) app->captureMessage = SDL_atoi(argv[i + 1]);
    for (int i = 1; i < argc; ++i)
        if (SDL_strcmp(argv[i], "--volume-menu") == 0) app->captureOptions = true;
        else if (SDL_strcmp(argv[i], "--video-menu") == 0) app->captureOptions = true, app->optionsPage = 2;
    for (int i = 1; i + 1 < argc; ++i)
        if (SDL_strcmp(argv[i], "--options-page") == 0) app->captureOptions = true, app->optionsPage = SDL_atoi(argv[i + 1]) - 1;
    for (int i = 1; i < argc; ++i)
        if (SDL_strcmp(argv[i], "--new-game") == 0) app->newGame = true;
        else if (SDL_strcmp(argv[i], "--camp") == 0) app->captureMenu = 1;
        else if (SDL_strcmp(argv[i], "--char-select") == 0) app->captureMenu = 4;
        else if (SDL_strcmp(argv[i], "--world-select") == 0) app->captureMenu = 8;
        else if (SDL_strcmp(argv[i], "--last-battle") == 0) app->captureMenu = 9;
        else if (SDL_strcmp(argv[i], "--scenarios") == 0) app->captureMenu = 5;
        else if (SDL_strcmp(argv[i], "--manual") == 0) app->captureMenu = 6;
        else if (SDL_strcmp(argv[i], "--title") == 0) app->captureTitle = true;
    for (int i = 1; i < argc; ++i) {
        if (SDL_strcmp(argv[i], "--no-encounters") == 0) app->noEncounters = true;
        if (i + 1 < argc && SDL_strcmp(argv[i], "--field-speed") == 0) app->fieldSpeed = std::clamp(SDL_atoi(argv[i + 1]), 1, 5);
        if (i + 1 < argc && SDL_strcmp(argv[i], "--battle-speed") == 0) app->battleSpeed = std::clamp(SDL_atoi(argv[i + 1]), 1, 5);
        if (SDL_strcmp(argv[i], "--no-environment-effects") == 0) app->environmentEffects = false;
    }
    if (!app->battleData.load(app->dataDir)) SDL_Log("no battle data in sys.evt: battles unavailable");
    if (!app->battleActions.load(app->dataDir)) SDL_Log("no battle actions in sys.evt");
    { // the voiced releases' battle-voice table (sys.evt section +0x40, 13 speakers x 1000 s16)
        std::vector<uint8_t> f;
        if (cr::readFile(app->dataDir + "sys.evt", f) && f.size() >= 0x80) {
            auto u32 = [&](size_t p) { return uint32_t(f[p]) | uint32_t(f[p + 1]) << 8 | uint32_t(f[p + 2]) << 16 | uint32_t(f[p + 3]) << 24; };
            const uint32_t at = u32(0x40);
            if (at && at < f.size()) {
                uint32_t end = uint32_t(f.size());
                for (size_t h = 0; h < 0x80; h += 4)
                    if (const uint32_t o = u32(h); o > at && o < end) end = o;
                for (uint32_t q = at; q + 2 <= end; q += 2) app->battleVoiceTable.push_back(int16_t(f[q] | f[q + 1] << 8));
                SDL_Log("battle voices: %zu situations", app->battleVoiceTable.size());
            }
        }
    }
    if (!app->itemTable.load(app->dataDir)) SDL_Log("no item table in sys.evt: shops unavailable");
    for (int i = 1; i + 1 < argc; ++i)
        if (SDL_strcmp(argv[i], "--frames") == 0) app->captureFrames = SDL_atoi(argv[i + 1]);
        else if (SDL_strcmp(argv[i], "--settle-fps") == 0) app->captureSettleFps = std::clamp(SDL_atoi(argv[i + 1]), 0, 1000);
        else if (SDL_strcmp(argv[i], "--settle") == 0) app->captureSettle = std::clamp(SDL_atoi(argv[i + 1]), 0, 1000);
        else if (SDL_strcmp(argv[i], "--event") == 0) app->captureEvent = SDL_atoi(argv[i + 1]);
        else if (SDL_strcmp(argv[i], "--area") == 0) app->captureArea = SDL_atoi(argv[i + 1]);
        else if (SDL_strcmp(argv[i], "--set-flags") == 0)
            for (const char* p = argv[i + 1]; *p;) {
                app->captureFlags.push_back(SDL_atoi(p));
                while (*p && *p != ',') ++p;
                if (*p == ',') ++p;
            }
        else if (SDL_strcmp(argv[i], "--title-new") == 0) app->captureTitlePair = std::clamp(SDL_atoi(argv[i + 1]), 1, 2);
        else if (SDL_strcmp(argv[i], "--talk-portrait") == 0) app->captureTalk = SDL_atoi(argv[i + 1]);
        else if (SDL_strcmp(argv[i], "--save-screen") == 0)
            app->captureMenu = SDL_strcmp(argv[i + 1], "load") == 0 ? 3 : SDL_strcmp(argv[i + 1], "clear") == 0 ? 7 : 2;
        else if (SDL_strcmp(argv[i], "--shop") == 0) app->captureShop = argv[i + 1];
        else if (SDL_strcmp(argv[i], "--battle-view") == 0) app->captureBattleView = argv[i + 1];
        else if (SDL_strcmp(argv[i], "--camp-view") == 0) app->captureCampView = argv[i + 1];
        else if (SDL_strcmp(argv[i], "--field-menu") == 0) app->captureFieldMenu = argv[i + 1];
        else if (SDL_strcmp(argv[i], "--touch-pad") == 0) app->captureTouchPad = argv[i + 1];
        else if (SDL_strcmp(argv[i], "--scene-fx") == 0 && i + 2 < argc) {
            app->captureSceneFx[0] = SDL_atoi(argv[i + 1]);
            app->captureSceneFx[1] = SDL_atoi(argv[i + 2]);
            if (i + 3 < argc && argv[i + 3][0] >= '0' && argv[i + 3][0] <= '9') app->captureSceneFx[2] = SDL_atoi(argv[i + 3]);
        }
        else if (SDL_strcmp(argv[i], "--settle-moving") == 0) {
            app->captureSettle = std::clamp(SDL_atoi(argv[i + 1]), 0, 1000);
            app->captureSettleMoving = true;
        }
    for (int i = 1; i + 2 < argc; ++i)
        if (SDL_strcmp(argv[i], "--battle") == 0) {
            app->captureBattle[0] = SDL_atoi(argv[i + 1]);
            app->captureBattle[1] = SDL_atoi(argv[i + 2]);
        }
    for (int i = 1; i + 2 < argc; ++i)
        if (SDL_strcmp(argv[i], "--at") == 0) {
            app->captureAt[0] = float(SDL_atof(argv[i + 1]));
            app->captureAt[1] = float(SDL_atof(argv[i + 2]));
        }
    for (int i = 1; i < argc; ++i)
        if (SDL_strcmp(argv[i], "--win-battles") == 0) app->captureWinBattles = true;
    for (int i = 1; i < argc; ++i)
        if (SDL_strcmp(argv[i], "--models") == 0) app->mode = Mode::Models;
        else if (SDL_strcmp(argv[i], "--maps") == 0) app->mode = Mode::Maps;
    if (SDL_getenv("CR_OP_TRACE")) { // (debugging: every script instruction and native call)
        cr::setEventTrace([](size_t pc, int op) { std::printf("  op %#zx: %d\n", pc, op); });
        cr::setNativeTrace([](int id, const int32_t* a) {
            std::printf("  call %d (%d %d %d %d %d %d %d %d %d %d %d)\n", id, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10]);
        });
    }
    for (int i = 1; i < argc; ++i) {
        if (SDL_strcmp(argv[i], "--capture") == 0 && i + 2 < argc) {
            SDL_SetLogPriorities(SDL_LOG_PRIORITY_WARN);
            return runCapture(*app, argv[i + 1], argv[i + 2]) ? SDL_APP_FAILURE : SDL_APP_SUCCESS;
        }
        if (SDL_strcmp(argv[i], "--explore") == 0) {
            SDL_SetLogPriorities(SDL_LOG_PRIORITY_WARN);
            return runExplore(*app, i + 1 < argc ? SDL_atoi(argv[i + 1]) : 200) ? SDL_APP_FAILURE : SDL_APP_SUCCESS;
        }
        if (SDL_strcmp(argv[i], "--export-models") == 0) { // [dir]: models/export/ in the game folder without one
            SDL_SetLogPriorities(SDL_LOG_PRIORITY_WARN);
            const char* dir = i + 1 < argc && argv[i + 1][0] != '-' ? argv[i + 1] : "";
            return runExportModels(*app, dir) ? SDL_APP_FAILURE : SDL_APP_SUCCESS;
        }
        if (SDL_strcmp(argv[i], "--export-textures") == 0) { // [dir]: textures/export/ in the game folder without one
            SDL_SetLogPriorities(SDL_LOG_PRIORITY_WARN);
            const char* dir = i + 1 < argc && argv[i + 1][0] != '-' ? argv[i + 1] : "";
            return runExportTextures(*app, dir) ? SDL_APP_FAILURE : SDL_APP_SUCCESS;
        }
        if (SDL_strcmp(argv[i], "--print-msgs") == 0 && i + 2 < argc) { // --print-msgs area language: every event message (tools)
            // decoded - its pages (" | " between lines, " || " between pages) and what MessageInfo says
            cr::EventFile ef;
            if (!ef.load(app->dataDir, SDL_atoi(argv[i + 1]))) return SDL_APP_FAILURE;
            const int language = SDL_atoi(argv[i + 2]);
            for (int id = 1000; id < 51000; ++id) {
                std::vector<std::vector<std::string>> pages;
                cr::MessageInfo mi;
                if (!ef.message(id, language, nullptr, pages, &mi)) continue;
                std::string all;
                for (size_t p = 0; p < pages.size(); ++p)
                    for (size_t l = 0; l < pages[p].size(); ++l) all += (p && !l ? " || " : l ? " | " : "") + pages[p][l];
                std::printf("%d\t%s\tname=%d\tvoices=", id, all.c_str(), mi.nameId);
                for (const auto& v : mi.voices) std::printf("%d@%d,", v.clip, v.page);
                std::printf("\tkept=");
                for (const auto& k : mi.kept) std::printf("%d:%d:%d,", k.page, k.chars, k.instant ? 1 : 0);
                std::printf("\tpauses=");
                for (const auto& pz : mi.pauses) std::printf("%d@%d:%d,", pz.frames, pz.page, pz.at);
                std::printf("\n");
            }
            return SDL_APP_SUCCESS;
        }
        if (SDL_strcmp(argv[i], "--print-sysmsg") == 0) { // --print-sysmsg id...: system messages' text (tools)
            for (int k = i + 1; k < argc; ++k) std::printf("%s: %s\n", argv[k], sysText(*app, SDL_atoi(argv[k])).c_str());
            return SDL_APP_SUCCESS;
        }
        if (SDL_strcmp(argv[i], "--selftest") == 0) {
            SDL_SetLogPriorities(SDL_LOG_PRIORITY_WARN); // keep per-title logging quiet
            const int rc = runSelfTest(*app);
            return rc ? SDL_APP_FAILURE : SDL_APP_SUCCESS;
        }
    }
    for (int i = 1; i + 1 < argc; ++i) { // --map <name>: start on that field (e.g. d0108)
        if (SDL_strcmp(argv[i], "--map") != 0) continue;
        for (size_t m = 0; m < app->maps.size(); ++m)
            if (app->maps[m].rfind(argv[i + 1], 0) == 0) {
                app->mapIndex = int(m);
                app->mode = Mode::Maps;
                break;
            }
    }
    for (int i = 1; i + 1 < argc; ++i) {
        if (SDL_strcmp(argv[i], "--model") != 0) continue;
        const std::string want = argv[i + 1];
        for (size_t m = 0; m < app->models.size(); ++m) {
            if (app->models[m].rfind(want, 0) == 0) {
                app->modelIndex = int(m);
                app->mode = Mode::Models;
                break;
            }
        }
    }
    if (hdBundled) app->video.hdTextures = true; // (the build's own: on, unless settings.ini turns it off)
    if (!startSound(*app)) SDL_Log("sound effects unavailable");
    else SDL_Log("%d sound effects", app->sound.entries());
    if (!app->music.init(app->dataDir)) SDL_Log("music unavailable");
    vitaAudio(*app);
    loadMix(*app);
    for (int i = 1; i < argc; ++i) // (over the stored setting; captures read it earlier)
        if (SDL_strcmp(argv[i], "--no-environment-effects") == 0) app->environmentEffects = false;
    videoArgs(*app, argc, argv); // the command line wins over settings.ini
    for (int i = 1; i + 1 < argc; ++i) { // (--field-speed / --battle-speed n: over the stored setting, not saved)
        if (SDL_strcmp(argv[i], "--field-speed") == 0) app->fieldSpeed = std::clamp(SDL_atoi(argv[i + 1]), 1, 5);
        if (SDL_strcmp(argv[i], "--battle-speed") == 0) app->battleSpeed = std::clamp(SDL_atoi(argv[i + 1]), 1, 5);
    }
    applyVideo(*app);
    // The UI atlas was uploaded before settings.ini restored hd_textures (hdBundled, loadMix): when
    // that changed it, again, else the HD replacement stayed out until HD Textures was toggled
    if (app->video.hdTextures != hdTexturesAtFirstUpload) {
        cr::Image wi;
        if (loadImage(*app, "window_hd.mct", wi, &app->androidWindow)) {
            app->renderer.destroy(app->windowTex);
            app->windowTex = app->renderer.upload(wi);
            noteWindowAtlas(*app, wi);
        }
    }
    if (languageArg >= 0) { // the command line wins over settings.ini
        app->language = languageArg;
        applyLanguage(*app);
    }
    showTexture(*app, 0);
    setMode(*app, app->newGame ? Mode::Maps : app->mode);
    if (app->newGame) {
        startNewGame(*app);
    } else { // the game boots to its title unless a viewer mode was asked for
        bool viewer = false;
        for (int i = 1; i < argc; ++i)
            for (const char* f : {"--viewer", "--map", "--model", "--models", "--maps"})
                viewer = viewer || SDL_strcmp(argv[i], f) == 0;
        app->viewer = viewer;
        if (!viewer) bootTitle(*app);
    }

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* state, SDL_Event* event) {
    auto& app = *static_cast<App*>(state);
    if (event->type == SDL_EVENT_RENDER_DEVICE_RESET) {
        if (event->render.windowID == SDL_GetWindowID(app.window)) app.graphicsReset.store(true);
        return SDL_APP_CONTINUE;
    }
    if (!consumeGraphicsReset(app)) return SDL_APP_FAILURE;
    if (app.inputCancel.note(*event)) return SDL_APP_CONTINUE;
    consumeInputCancel(app);
    switch (event->type) {
    case SDL_EVENT_QUIT:
        return SDL_APP_SUCCESS;
    case SDL_EVENT_GAMEPAD_ADDED:
        if (!app.pad) {
            app.pad = SDL_OpenGamepad(event->gdevice.which);
            if (app.pad) SDL_Log("controller: %s", SDL_GetGamepadName(app.pad));
        }
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (app.pad && SDL_GetGamepadID(app.pad) == event->gdevice.which) {
            holdSkip(app, kSkipPad, false);
            SDL_CloseGamepad(app.pad), app.pad = nullptr;
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        app.pointerLast = false;
        if (app.pad && SDL_GetGamepadID(app.pad) == event->gbutton.which) {
            if (event->gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) stickPointerButton(app, true); // R3: a puzzle room's finger
            else padButton(app, event->gbutton.button);
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_UP: // the square let go: the skip stops
        if (app.pad && SDL_GetGamepadID(app.pad) == event->gbutton.which && event->gbutton.button == SDL_GAMEPAD_BUTTON_WEST)
            holdSkip(app, kSkipPad, false);
        if (app.pad && SDL_GetGamepadID(app.pad) == event->gbutton.which && event->gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_STICK)
            stickPointerButton(app, false);
        break;
    case SDL_EVENT_KEY_UP:
        if (event->key.key == SDLK_X) holdSkip(app, kSkipKey, false);
        break;
    case SDL_EVENT_KEY_DOWN:
        app.pointerLast = false;
        // X holds the skip corner wherever a scene runs - a battle's talk too, whose key handler
        // takes every key before the switch below (seen in metastigma's fork, 8a3aa5d)
        if (event->key.key == SDLK_X && app.mode == Mode::Maps && app.runner.running()) holdSkip(app, kSkipKey, true);
        if (app.vitaLogo.phase) { // the Vita's logos: confirm cuts the hold short
            const SDL_Keycode k = event->key.key;
            if (app.vitaLogo.phase == 2 && (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE || k == SDLK_E || k == SDLK_ESCAPE))
                app.vitaLogo.skip = true;
            break;
        }
        if (event->key.key == SDLK_F3) { app.showHud = !app.showHud; saveMix(app); break; } // the saved Control Hints setting
        if (event->key.key == SDLK_V && !app.manualOpen && !app.scenariosOpen) { // the Options screen
            if (app.optionsOpen) app.optionsOpen = false;
            else openOptions(app, 0);
            break;
        }
        if (app.manualOpen) {
            manualKey(app, event->key.key);
            break;
        }
        if (app.optionsOpen) {
            optionsKey(app, event->key.key);
            break;
        }
        if (app.scenariosOpen) {
            scenariosKey(app, event->key.key);
            break;
        }
        if (app.saveScreen.open) {
            saveScreenKey(app, event->key.key);
            break;
        }
        if (app.charSelectOpen) {
            charSelectKey(app, event->key.key);
            break;
        }
        if (app.campOpen) {
            campKey(app, event->key.key);
            break;
        }
        if (app.lastBattle.open) {
            lastBattleKey(app, event->key.key);
            break;
        }
        if (app.shop.open && app.shop.phase != App::Shop::Talk) {
            shopKey(app, event->key.key);
            break;
        }
        if (app.resumePrompt == 1) {
            switch (event->key.key) {
            case SDLK_UP: case SDLK_DOWN: case SDLK_W: case SDLK_S: case SDLK_LEFT: case SDLK_RIGHT: case SDLK_A: case SDLK_D:
                app.resumeCursor ^= 1; menuSound(app, se::kCursor); break;
            case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: case SDLK_E: answerResume(app, app.resumeCursor == 0); break;
            case SDLK_ESCAPE: case SDLK_BACKSPACE: answerResume(app, false, true); break;
            default: break;
            }
            break;
        }
        if (app.staffRoll.open) { // the end card waits for a tap - or ○ (the Vita's __menuu_StaffRollPrc: 0x2000 alone)
            switch (event->key.key) {
            case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: case SDLK_E:
                staffRollTap(app);
                break;
            default: break;
            }
            break;
        }
        if (app.mapScreen.open) { // any of these closes it (the original: a tap anywhere)
            switch (event->key.key) {
            case SDLK_ESCAPE: case SDLK_BACKSPACE: case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: case SDLK_E:
                closeMapScreen(app);
                break;
            default: break;
            }
            break;
        }
        if (app.puzzle.stage >= 0 && fieldMenuWanted(app) && !fieldSceneRunning(app) &&
            (event->key.key == SDLK_T || event->key.key == SDLK_F5 || event->key.key == SDLK_F6)) {
            if (event->key.key == SDLK_T) puzzle::abilityKey(app); // the ability, straight ahead
            else puzzle::ask(app, event->key.key == SDLK_F5 ? 0 : 1);                  // the pad's two buttons
            break;
        }
        if (app.puzzle.pkState == 1) { // a block held: the cross key sends it (up = row - 1, as a swipe up)
            int dir = 0;
            switch (event->key.key) {
            case SDLK_UP: case SDLK_W: dir = 1; break;
            case SDLK_DOWN: case SDLK_S: dir = 2; break;
            case SDLK_LEFT: case SDLK_A: dir = 3; break;
            case SDLK_RIGHT: case SDLK_D: dir = 4; break;
            default: break;
            }
            if (dir) { puzzle::pkSend(app, dir); break; }
        }
        if ((event->key.key == SDLK_R || event->key.key == SDLK_J) && fieldMenuWanted(app) && !fieldSceneRunning(app)) {
            if (event->key.key == SDLK_R) toggleFieldMenu(app); // the corner button
            else if (fieldButtonShown(app, 3)) { // the encounter switch (skill 170 known), bar open or not
                const bool was = app.fieldMenu.open;
                app.fieldMenu.open = true;
                pressFieldButton(app, 3);
                app.fieldMenu.open = was;
            }
            break;
        }
        if (event->key.key == SDLK_K && app.mode == Mode::Maps && app.havePlayer && !app.titleOpen && !app.booting &&
            !app.runner.running() && app.dialogue.empty() && !app.battle.active && !app.itemWindowOpen) {
            openCamp(app); // the Camp menu
            menuSound(app, se::kCampOpen);
            break;
        }
        if (app.mode == Mode::Maps && app.titleOpen) {
            if (!titleKey(app, event->key.key) && !app.padKeyNow) return SDL_APP_SUCCESS; // (Esc quits - the keyboard's, not the pad's ×)
            break;
        }
        if (app.mode == Mode::Maps && app.battle.active) {
            battleKey(app, event->key.key);
            return SDL_APP_CONTINUE;
        }
        if (app.mode == Mode::Maps && app.worldChoiceOpen) { // the World Select
            worldSelectKey(app, event->key.key);
            return SDL_APP_CONTINUE;
        }
        if (app.mode == Mode::Maps && app.choiceFlag >= 0) { // yes / no
            switch (event->key.key) {
            case SDLK_UP: case SDLK_DOWN: case SDLK_LEFT: case SDLK_RIGHT: case SDLK_W: case SDLK_S:
                app.choiceCursor ^= 1;
                menuSound(app, se::kCursor);
                return SDL_APP_CONTINUE;
            case SDLK_ESCAPE: case SDLK_BACKSPACE: answerChoice(app, false, true); return SDL_APP_CONTINUE;
            default: break;
            }
        }
        switch (event->key.key) {
        case SDLK_ESCAPE: case SDLK_AC_BACK: if (app.padKeyNow) break; return SDL_APP_SUCCESS; // (the pad's × never quits)
        // (the viewer's stepping: a game's field leaves them alone - the pad's L / R send PgUp / PgDn,
        // and the arrow keys or L / R on the field changed the map, ten at a time)
        case SDLK_RIGHT: if (app.viewer) step(app, 1); break;
        case SDLK_LEFT: if (app.viewer) step(app, -1); break;
        case SDLK_PAGEDOWN: if (app.viewer) step(app, 10); break;
        case SDLK_PAGEUP: if (app.viewer) step(app, -10); break;
        case SDLK_TAB:
            if (!app.viewer) break;
            setMode(app, app.mode == Mode::Textures ? Mode::Models
                         : app.mode == Mode::Models ? Mode::Maps : Mode::Textures);
            break;
        case SDLK_C: if (app.mode == Mode::Maps) { app.showCollision = !app.showCollision; updateTitle(app); } break;
        case SDLK_M:
            if (app.mode == Mode::Maps && !app.stageCams.empty()) {
                app.stageCamIndex = (app.stageCamIndex + 1) % int(app.stageCams.size());
                app.stageCam = app.stageCams[app.stageCamIndex];
                app.mapNumber = app.stageCam.mapNumber;
                app.area = app.stageCam.area;
                enterMap(app);
                updateTitle(app);
            }
            break;
        case SDLK_P: if (app.mode == Mode::Maps) { app.showPlayer = !app.showPlayer; placePlayer(app); } break;
        case SDLK_E: case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
            if (app.mode != Mode::Maps) {
                if (event->key.key == SDLK_SPACE && app.mode == Mode::Models) {
                    app.playing = !app.playing;
                    updateTitle(app);
                }
                break;
            }
            if (app.choiceFlag >= 0) {
                answerChoice(app, app.choiceCursor == 0);
            } else if (!app.dialogue.empty()) { // next page (or the rest of this one)
                tapDialogue(app);
            } else if (app.runner.running()) {
                app.tapPending = true; // touch_up_chk: e.g. closing the item window
            } else {
                interact(app);
            }
            break;
        case SDLK_O:
            if (app.mode == Mode::Maps) {
                app.playerChar = (app.playerChar + 1) % int(SDL_arraysize(kPartyModels));
                const cr::FieldWalker keep = app.walker;
                const bool had = app.havePlayer;
                placePlayer(app);
                if (had && app.havePlayer) app.walker.pos = keep.pos, app.walker.heading = keep.heading;
                updateTitle(app);
            }
            break;
        case SDLK_U: app.zUp = !app.zUp; updateTitle(app); break;
        case SDLK_F: app.smoothFilter = !app.smoothFilter; updateTitle(app); break;
        case SDLK_X: break; // (held at the top of SDL_EVENT_KEY_DOWN; up: SDL_EVENT_KEY_UP)
        case SDLK_N:
            if (app.mode == Mode::Models) selectAction(app, 1);
            else if (app.mode == Mode::Maps) { app.showNpcs = !app.showNpcs; buildNpcDraws(app); updateTitle(app); }
            break;
        case SDLK_B: // previous animation in model mode, background view in map mode
            if (app.mode == Mode::Models) selectAction(app, -1);
            else if (app.mode == Mode::Maps) { app.showBackground = !app.showBackground; updateTitle(app); }
            break;
        case SDLK_MINUS: app.speed = std::max(0.125f, app.speed * 0.5f); updateTitle(app); break;
        case SDLK_EQUALS: app.speed = std::min(8.0f, app.speed * 2.0f); updateTitle(app); break;
        default: break;
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        app.pointerLast = true;
        app.pointerCancelled = false;
        if (event->button.button == SDL_BUTTON_LEFT) { // (ScrollCamera: touchesBegan's tapCount 2 - the double tap, a touch's or a click's)
            if (event->button.which != SDL_TOUCH_MOUSEID && !app.fingers.mouseHeld) app.fingers.mouseHeld = true, ++app.fingers.held;
            if (event->button.clicks >= 2) {
                int ww = 0, wh = 0;
                SDL_GetWindowSize(app.window, &ww, &wh);
                app.fingers.doubleTap = true;
                app.fingers.x = event->button.x * (ww > 0 ? float(app.renderer.frameWidth()) / ww : 1);
                app.fingers.y = event->button.y * (wh > 0 ? float(app.renderer.frameHeight()) / wh : 1);
            }
        }
        if (app.puzzle.pkPointerCancelled && app.puzzle.pkState == 1) {
            int ww = 0, wh = 0;
            SDL_GetWindowSize(app.window, &ww, &wh);
            float lx, ly;
            app.renderer.toLogical(event->button.x * (ww > 0 ? float(app.renderer.frameWidth()) / ww : 1),
                                   event->button.y * (wh > 0 ? float(app.renderer.frameHeight()) / wh : 1), lx, ly);
            logicalToTex2d(app, lx, ly, app.puzzle.pkFromX, app.puzzle.pkFromY); // (the detector's new start)
            app.puzzle.pkAtX = app.puzzle.pkFromX, app.puzzle.pkAtY = app.puzzle.pkFromY, app.puzzle.pkHeld = true;
            app.puzzle.pkPointerCancelled = false;
            break; // re-grab the held block without spending ESP or sending it anywhere
        }
        if (app.vitaLogo.phase) { // (a tap: the same)
            if (app.vitaLogo.phase == 2) app.vitaLogo.skip = true;
            break;
        }
        if (app.manualOpen || app.scenariosOpen) {
            int ww = 0, wh = 0;
            SDL_GetWindowSize(app.window, &ww, &wh);
            const float kx = ww > 0 ? float(app.renderer.frameWidth()) / float(ww) : 1.0f;
            const float ky = wh > 0 ? float(app.renderer.frameHeight()) / float(wh) : 1.0f;
            if (app.manualOpen) manualPointerDown(app, event->button.x * kx, event->button.y * ky); // (MuScrList_Proc: on the lift)
            else {
                // its Back: held, closing as the touch lifts there (pressRelease); elsewhere a drag to scroll
                if (historyBackAt(app, event->button.x * kx, event->button.y * ky))
                    app.press = App::Press{true, kPressHistory, event->button.x * kx, event->button.y * ky};
                else app.dragging = true;
            }
            break;
        }
        if (app.optionsOpen) { // window points -> the frame's pixels
            int ww = 0, wh = 0;
            SDL_GetWindowSize(app.window, &ww, &wh);
            const float kx = ww > 0 ? float(app.renderer.frameWidth()) / float(ww) : 1.0f;
            const float ky = wh > 0 ? float(app.renderer.frameHeight()) / float(wh) : 1.0f;
            // (2.2.0's buttons act as the touch lifts on them, lit while held - _pad_chk_updw_box; page 2's On / Off and a
            // gauge take the press: optionsPointerDown)
            const float px = event->button.x * kx, py = event->button.y * ky;
            if (optionsPointerDown(app, px, py)) app.press = App::Press{true, kPressOptions, px, py};
            break;
        }
        if (app.saveScreen.open || app.campOpen || app.resumePrompt || app.charSelectOpen || app.worldChoiceOpen ||
            (app.shop.open && app.shop.phase != App::Shop::Talk) || (app.mode == Mode::Maps && app.titleOpen) ||
            (app.mode == Mode::Maps && app.battle.active)) {
            int ww = 0, wh = 0;
            SDL_GetWindowSize(app.window, &ww, &wh);
            const float kx = ww > 0 ? float(app.renderer.frameWidth()) / float(ww) : 1.0f;
            const float ky = wh > 0 ? float(app.renderer.frameHeight()) / float(wh) : 1.0f;
            const float px = event->button.x * kx, py = event->button.y * ky;
            // (2.2.0's menus act on the touch lifting inside a control - _pad_chk_updw_box's trg,
            // _pad_chk_trg_up_box0 - and light it while held: the press is kept, the click runs on the
            // release. A battle's talk and its one-tap phases and the Camp's pointer take the press itself)
            const int screen = pressScreen(app);
            if (app.campOpen) campPointerDown(app, px, py);
            else if (screen == kPressBattle && !battleMenuPress(app)) battleClick(app, px, py);
            else if (screen) {
                app.press = App::Press{true, screen, px, py};
                if (screen == kPressBattle) battlePressDown(app, px, py); // (where it went down: drags, swipes)
            }
            break;
        }
        if (app.staffRoll.open) { // _pad_chk_updw_box(-16, -32, 512, 384): the end card's tap
            staffRollTap(app);
            break;
        }
        if (app.mapScreen.open) { // _pad_chk_updw_box(0, 0, 480, 320) trg: a touch lifting anywhere closes it (pressRelease)
            int ww = 0, wh = 0;
            SDL_GetWindowSize(app.window, &ww, &wh);
            app.press = App::Press{true, kPressMap, event->button.x * (ww > 0 ? float(app.renderer.frameWidth()) / ww : 1.0f),
                                   event->button.y * (wh > 0 ? float(app.renderer.frameHeight()) / wh : 1.0f)};
            break;
        }
        if (app.mode == Mode::Maps) { // the field menu's corner button and bar
            int ww = 0, wh = 0;
            SDL_GetWindowSize(app.window, &ww, &wh);
            const float kx = ww > 0 ? float(app.renderer.frameWidth()) / float(ww) : 1.0f;
            const float ky = wh > 0 ? float(app.renderer.frameHeight()) / float(wh) : 1.0f;
            if (fieldMenuClick(app, event->button.x * kx, event->button.y * ky)) break;
            if (puzzle::click(app, event->button.x * kx, event->button.y * ky)) break; // a tap on a block
            if (touchPadDown(app, event->button.x * kx, event->button.y * ky)) break; // the pad, the "!" button
        }
        if (app.mode == Mode::Maps && app.runner.running() && app.windowTex.id) {
            // A scene: the skip button (top right) fast-forwards it; a tap elsewhere turns the page
            int ww = 0, wh = 0;
            SDL_GetWindowSize(app.window, &ww, &wh);
            const float kx = ww > 0 ? float(app.renderer.frameWidth()) / float(ww) : 1.0f;
            const float ky = wh > 0 ? float(app.renderer.frameHeight()) / float(wh) : 1.0f;
            float lx, ly;
            app.renderer.toLogical(event->button.x * kx, event->button.y * ky, lx, ly);
            if (skipCorner(app, lx, ly)) {
                holdSkip(app, kSkipPointer, true);
                break;
            }
            if (app.choiceFlag >= 0) { // the Yes / No plates: held, answered as the touch lifts (pressRelease)
                app.press = App::Press{true, kPressChoice, event->button.x * kx, event->button.y * ky};
                break;
            }
            if (app.choiceFlag < 0 && !app.dialogue.empty()) {
                tapDialogue(app);
                break;
            }
            if (app.choiceFlag < 0) app.tapPending = true;
        }
        if (app.mode == Mode::Textures) {
            // Touch also arrives as mouse events by default, so this covers Android
            int w = 0;
            SDL_GetWindowSize(app.window, &w, nullptr);
            step(app, event->button.x < w / 2 ? -1 : 1);
        } else {
            app.dragging = true;
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event->button.button == SDL_BUTTON_LEFT && app.fingers.mouseHeld)
            app.fingers.mouseHeld = false, app.fingers.held = std::max(0, app.fingers.held - 1);
        holdSkip(app, kSkipPointer, false); // the finger off the skip corner
        if (app.pointerCancelled) { // cancellation is not a click or a puzzle release
            app.press = App::Press(), app.manualPointer = App::ManualPointer(), app.optionsGauge = App::OptGauge();
            break;
        }
        if (app.manualPointer.down) { // the manual's list: a tap as it lifts, else the drag's end
            int ww = 0, wh = 0;
            SDL_GetWindowSize(app.window, &ww, &wh);
            manualPointerUp(app, event->button.x * (ww > 0 ? float(app.renderer.frameWidth()) / ww : 1.0f),
                            event->button.y * (wh > 0 ? float(app.renderer.frameHeight()) / wh : 1.0f));
            break;
        }
        if (app.press.down) { // a menu's touch lifting: the control it lifts on (if the screen is still the one)
            int ww = 0, wh = 0;
            SDL_GetWindowSize(app.window, &ww, &wh);
            const float px = event->button.x * (ww > 0 ? float(app.renderer.frameWidth()) / ww : 1.0f);
            const float py = event->button.y * (wh > 0 ? float(app.renderer.frameHeight()) / wh : 1.0f);
            const App::Press pr = app.press;
            app.press = App::Press();
            const bool same = pr.screen == kPressChoice ? app.mode == Mode::Maps && app.choiceFlag >= 0
                              : pr.screen == kPressHistory ? app.scenariosOpen
                              : pr.screen == kPressOptions ? app.optionsOpen && !app.manualOpen
                              : pr.screen == kPressMap ? app.mapScreen.open : pressScreen(app) == pr.screen;
            if (same) pressRelease(app, pr.screen, px, py);
            break;
        }
        if (app.campPointer.down) {
            int ww = 0, wh = 0;
            SDL_GetWindowSize(app.window, &ww, &wh);
            campPointerUp(app, event->button.x * (ww > 0 ? float(app.renderer.frameWidth()) / ww : 1.0f),
                          event->button.y * (wh > 0 ? float(app.renderer.frameHeight()) / wh : 1.0f));
            break;
        }
        app.dragging = false;
        app.optionsBarDrag = false;
        touchPadUp(app);
        if (app.mode == Mode::Maps && app.puzzle.stage >= 0) {
            int ww = 0, wh = 0;
            SDL_GetWindowSize(app.window, &ww, &wh);
            const float kx = ww > 0 ? float(app.renderer.frameWidth()) / float(ww) : 1.0f;
            const float ky = wh > 0 ? float(app.renderer.frameHeight()) / float(wh) : 1.0f;
            puzzle::release(app, event->button.x * kx, event->button.y * ky);
        }
        break;
    case SDL_EVENT_FINGER_DOWN: // (the touches as such: ScrollCamera counts them; their mouse events do the rest)
        ++app.fingers.held, ++app.fingers.fresh;
        app.fingers.x = event->tfinger.x * float(app.renderer.frameWidth());
        app.fingers.y = event->tfinger.y * float(app.renderer.frameHeight());
        break;
    case SDL_EVENT_FINGER_UP:
        app.fingers.held = std::max(0, app.fingers.held - 1);
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (app.press.down) { // (the held control follows the finger)
            int ww = 0, wh = 0;
            SDL_GetWindowSize(app.window, &ww, &wh);
            app.press.px = event->motion.x * (ww > 0 ? float(app.renderer.frameWidth()) / ww : 1.0f);
            app.press.py = event->motion.y * (wh > 0 ? float(app.renderer.frameHeight()) / wh : 1.0f);
            if (app.press.screen == kPressOptions && app.optionsOpen) optionsPointerMove(app, app.press.px, app.press.py); // (a gauge held)
        }
        if (app.mode == Mode::Maps && app.puzzle.pkHeld && (event->motion.state & SDL_BUTTON_LMASK)) {
            int ww = 0, wh = 0; // the held block's touch: the detector follows it
            SDL_GetWindowSize(app.window, &ww, &wh);
            puzzle::drag(app, event->motion.x * (ww > 0 ? float(app.renderer.frameWidth()) / float(ww) : 1.0f),
                         event->motion.y * (wh > 0 ? float(app.renderer.frameHeight()) / float(wh) : 1.0f));
        }
        if (app.mode == Mode::Maps && app.runner.running() && (event->motion.state & SDL_BUTTON_LMASK)) {
            // (pad_chk_circle tests where the held touch is now: one slid off the corner stops the
            // skip, one slid onto it starts it)
            int ww = 0, wh = 0;
            SDL_GetWindowSize(app.window, &ww, &wh);
            float lx, ly;
            app.renderer.toLogical(event->motion.x * (ww > 0 ? float(app.renderer.frameWidth()) / float(ww) : 1.0f),
                                   event->motion.y * (wh > 0 ? float(app.renderer.frameHeight()) / float(wh) : 1.0f), lx, ly);
            holdSkip(app, kSkipPointer, skipCorner(app, lx, ly));
        }
        if (app.manualPointer.down) {
            int ww = 0, wh = 0;
            SDL_GetWindowSize(app.window, &ww, &wh);
            manualPointerMove(app, event->motion.x * (ww > 0 ? float(app.renderer.frameWidth()) / ww : 1.0f),
                              event->motion.y * (wh > 0 ? float(app.renderer.frameHeight()) / wh : 1.0f));
            break;
        }
        if (app.campPointer.down) {
            int ww = 0, wh = 0;
            SDL_GetWindowSize(app.window, &ww, &wh);
            campPointerMove(app, event->motion.x * (ww > 0 ? float(app.renderer.frameWidth()) / ww : 1.0f),
                            event->motion.y * (wh > 0 ? float(app.renderer.frameHeight()) / wh : 1.0f));
            break;
        }
        if (app.optionsOpen && app.optionsBarDrag) { // the scroll bar's thumb follows
            int ww = 0, wh = 0;
            SDL_GetWindowSize(app.window, &ww, &wh);
            float lx, ly, mx, my;
            app.renderer.toLogical(event->motion.x * (ww > 0 ? float(app.renderer.frameWidth()) / float(ww) : 1.0f),
                                   event->motion.y * (wh > 0 ? float(app.renderer.frameHeight()) / float(wh) : 1.0f), lx, ly);
            menuPointFit(app, lx, ly, mx, my);
            optionsBarTo(app, my);
            break;
        }
        if (app.touchPad.down) {
            int ww = 0, wh = 0;
            SDL_GetWindowSize(app.window, &ww, &wh);
            const float kx = ww > 0 ? float(app.renderer.frameWidth()) / float(ww) : 1.0f;
            const float ky = wh > 0 ? float(app.renderer.frameHeight()) / float(wh) : 1.0f;
            touchPadMove(app, event->motion.x * kx, event->motion.y * ky);
        } else if (app.dragging && app.scenariosOpen) { // the chart follows the finger
            int wh = 0;
            SDL_GetWindowSize(app.window, nullptr, &wh);
            scenariosScrollBy(app, event->motion.yrel * (wh > 0 ? 768.0f / float(wh) : 1.0f));
            scenariosScrollSideways(app, event->motion.xrel * (wh > 0 ? 384.0f / float(wh) : 0.5f)); // (tex2d)
        } else if (app.dragging && app.mode != Mode::Textures) {
            app.yaw -= event->motion.xrel * 0.01f;
            app.pitch = std::clamp(app.pitch + event->motion.yrel * 0.01f, -1.5f, 1.5f);
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if (app.optionsOpen && !app.resetAsk) { optionsScrollBy(app, event->wheel.y > 0 ? -1 : event->wheel.y < 0 ? 1 : 0); break; }
        if (app.campOpen) { campScrollBy(app, event->wheel.y > 0 ? -1 : event->wheel.y < 0 ? 1 : 0); break; }
        if (app.scenariosOpen) { scenariosScrollBy(app, event->wheel.y * 96); break; }
        if (app.manualOpen) { manualKey(app, event->wheel.y > 0 ? SDLK_UP : SDLK_DOWN); break; }
        app.zoom = std::clamp(app.zoom * (event->wheel.y > 0 ? 0.9f : 1.1f), 0.1f, 10.0f);
        break;
    default:
        break;
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* state) {
    if (!consumeGraphicsReset(*static_cast<App*>(state))) return SDL_APP_FAILURE;
    consumeInputCancel(*static_cast<App*>(state));
    static_cast<App*>(state)->sound.update();
    App& app = *static_cast<App*>(state);
    const float activeDt = app.clock.step();
    padStep(app, activeDt);
    pressCountStep(app, activeDt);
    stickPointerStep(app, activeDt);
    app.music.update(activeDt);
    // the frame rate's limit: each frame no sooner than its slot (vsync may hold it longer)
    if (app.video.frameLimit > 0) {
        static Uint64 next = 0;
        const Uint64 period = SDL_NS_PER_SECOND / Uint64(app.video.frameLimit), t = SDL_GetTicksNS();
        if (next > t && next - t <= period) SDL_DelayPrecise(next - t);
        next = std::max(next, t) + period; // (after a stall: from now, no catching up)
    }
    drawFrame(app);
    if (SDL_getenv("CR_FPS_LOG")) { // (debugging) the frames drawn a second, every two seconds
        static Uint64 since = SDL_GetTicksNS();
        static int frames = 0;
        ++frames;
        if (const Uint64 now = SDL_GetTicksNS(); now - since >= 2 * SDL_NS_PER_SECOND) {
            std::printf("fps: %.1f (limit %d)\n", double(frames) * 1e9 / double(now - since), app.video.frameLimit);
            std::fflush(stdout);
            frames = 0, since = now;
        }
    }
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* state, SDL_AppResult) {
    auto* app = static_cast<App*>(state);
    if (!app) return;
    app->clock.stop();
    app->inputCancel.stop();
    // (the Progress Tracker: nothing new is written here - the original's ContData_Save, on
    // leaving, writes only the snapshot a scene's end or an encounter took (_ContData_Save_set's
    // pending mark 0x3f6), never the live state, so a game left mid-cutscene resumes at the last
    // scene's end; ours writes those snapshots as they are taken, the Vita's way)
    if (app->rendererReady) {
        app->renderer.destroy(app->currentTex);
        unloadModel(*app);
        app->sound.shutdown();
        unloadLoaded(*app, app->player);
        for (auto& [name, lm] : app->npcModels) unloadLoaded(*app, lm);
        for (auto& [id, lm] : app->effectModels) unloadLoaded(*app, lm);
        app->hud.destroy(app->renderer);
        app->dialogueHud.destroy(app->renderer);
        app->optionsHud.destroy(app->renderer);
        app->renderer.destroy(app->windowTex);
        app->promptHud.destroy(app->renderer);
        app->itemHud.destroy(app->renderer);
        app->renderer.shutdown();
    }
    if (app->window) SDL_DestroyWindow(app->window);
    delete app;
}
