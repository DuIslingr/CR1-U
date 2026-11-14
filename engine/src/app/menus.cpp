#include "app/app.hpp"

namespace crapp {

// ---- Saves (SaveData_Save / ContData_Save, the Camp menu, __menuu_SaveLoad*) ----

std::string savePath(const App& app, const char* file) {
    return (app.saveDirOverride.empty() ? cr::saveDir() : app.saveDirOverride) + file;
}

cr::SaveData captureSave(const App& app) {
    cr::SaveData s;
    s.area = app.area;
    s.map = app.mapNumber;
    s.pos[0] = app.walker.pos.x;
    s.pos[1] = app.walker.pos.y;
    s.pos[2] = app.walker.pos.z;
    s.yaw = app.walker.heading;
    if (app.area == 1) { // (_SaveData_Save 71813-71825: in the puzzle rooms the field's load point, not where he stands)
        s.pos[0] = app.loadPoint[0], s.pos[1] = app.loadPoint[1], s.pos[2] = app.loadPoint[2];
        s.yaw = app.loadPoint[3] * 3.14159265f / 180.0f;
    }
    s.leader = app.playerChar;
    s.playSeconds = app.playSeconds;
    s.savedAt = int64_t(std::time(nullptr));
    s.game = app.game;
    s.game.frmScene = s.game.frmRecord = 0;
    cr::changeRoute(s.game, &app.itemTable, s.route()); // (the leader's pair's own items in view)
    return s;
}


// Back on the field the save was made on, as arriving there by warp (the room's entry scripts
// run as they would)
bool applySave(App& app, const cr::SaveData& s) {
    if (s.map < 0 || s.map >= cr::kNumMaps) return false;
    bootTitle(app); // everything closed and torn down
    app.trackerArmed = false; // (SaveData_Load_init 71984: a load or a resume starts it again)
    app.runner.stop();
    app.booting = false;
    app.titleOpen = false;
    app.game = s.game;
    cr::giveDataItems(app.game); // (2.2.0's data holds them from its making: a port save from before may not)
    // (ContData_memcpy / Load_SaveData: once "About Progress Tracker" was shown this launch, the state
    // taken up keeps flag 0x44)
    if (app.interruptShown) app.game.setFlag(0x44, true);
    // (_SaveData_Load_init 71981: SetFlag(0xff7) on every load - each area's entry 0 then clears it and
    // 4076 (the encounter switch's hold) and aborts its arrival, e.g. j001_d01 0x4063)
    app.game.setFlag(4087, true);
    app.treasureBoxes.clear();
    app.entryArea = -1;
    app.playSeconds = s.playSeconds;
    app.playerChar = std::clamp(s.leader, 0, 7);
    cr::changeRoute(app.game, &app.itemTable, app.playerChar / 2);
    app.mapNumber = -1; // so the warp loads the field even if it's the one showing
    cr::WarpRequest w;
    w.valid = true;
    w.area = s.area;
    w.map = s.map;
    std::memcpy(w.pos, s.pos, sizeof w.pos);
    w.angleDegrees = s.yaw * 180.0f / 3.14159265f;
    if (!performWarp(app, w, -1)) return false;
    app.walker.pos = {s.pos[0], s.pos[1], s.pos[2]};
    app.walker.heading = s.yaw;
    return true;
}

// The pair's faces (pictures 29..36: the men across the top row, the women below, a pair per
// column; characters 0 Escher 1 Musiea 2 Eluca 3 Zhamo 4 Olgar 5 Vahti 6 Ayuta 7 Mana)
// tex2d slot 0x32 (FUN_00011fb4: window_char, the _hd sheet 2048 x 1024 for 1024 x 512 tex2d units):
// 2.2.0's languages' names (88 x 24 at u = language x 88, v 0) and the face boards' names (MuTex table
// 3 at 0xbbd30: 92 x 20 at u = language x 92, v = 32 + row x 20). 1.0.1's data has no such sheet
const cr::Texture* charSheet(App& app) {
    if (!app.charSheetTried && app.rendererReady) {
        app.charSheetTried = true;
        cr::Image img;
        if (loadImage(app, "window_char.mct", img) && img.width >= 1024 && img.height > 0) {
            img.logicalW = 1024, img.logicalH = 1024 * img.height / img.width;
            app.charSheet = app.renderer.upload(img);
        }
    }
    return app.charSheet.id ? &app.charSheet : nullptr;
}
// 2.2.0's Options and boards: the data's languages and its window_char sheet
bool languageSheet(App& app) { return app.systemText.hasLanguage(2) && charSheet(app); }

int facePicture(int character) {
    static const int kFace[8] = {29, 30, 32, 31, 33, 34, 35, 36};
    return kFace[std::clamp(character, 0, 7)];
}

std::string playTimeText(double seconds) {
    const long t = long(seconds);
    char b[32];
    SDL_snprintf(b, sizeof b, "%ld:%02ld:%02ld", t / 3600, t / 60 % 60, t % 60);
    return b;
}

std::string dateText(int64_t when) {
    const std::time_t t = std::time_t(when);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char b[48];
    std::strftime(b, sizeof b, "%Y/%m/%d  %H:%M", &tm);
    return b;
}

// The per-pair saves of the earlier releases (save1.dat .. save4.dat): read once, to fold into
// data.dat, and left as they are
std::string slotPath(const App& app, int pair) {
    const std::string name = "save" + std::to_string(std::clamp(pair, 1, 4)) + ".dat";
    return savePath(app, name.c_str());
}

// The one save, as 2.2.0's data.dat (SaveData_Save 0x725d0): the four pairs' records and bags, the
// shared Oz, bag and flags, each pair's own place and "saved" mark (cr::SaveData::places)
std::string dataPath(const App& app) { return savePath(app, "data.dat"); }

namespace {
// Per-pair saves (`files`: paths) folded into one, oldest first, each newer one's pair taking its
// own records, bag and place from its file (cr::foldPairSave)
bool foldPairSaves(const App& app, const std::vector<std::string>& files, cr::SaveData& d) {
    std::vector<cr::SaveData> slots;
    for (const std::string& f : files) {
        cr::SaveData s;
        if (cr::readSave(f, s)) slots.push_back(std::move(s));
    }
    if (slots.empty()) return false;
    std::stable_sort(slots.begin(), slots.end(), [](const cr::SaveData& a, const cr::SaveData& b) { return a.savedAt < b.savedAt; });
    d = slots[0];
    d.places = {};
    d.places[size_t(d.route())] = d.placeInPlay();
    for (size_t i = 1; i < slots.size(); ++i) cr::foldPairSave(d, slots[i], &app.itemTable);
    d.wholeGame = true;
    return true;
}
} // namespace

// Reads data.dat. Without one, the earlier releases' per-pair saves are folded into it once (the
// files themselves are kept): the newest is the base, each pair's own records, bag and place from
// its own file, the flags added together
bool readData(const App& app, cr::SaveData& d) {
    if (cr::readSave(dataPath(app), d)) {
        d.wholeGame = true;
        return true;
    }
    std::vector<std::string> files;
    for (int pair = 1; pair <= 4; ++pair) files.push_back(slotPath(app, pair));
    if (!foldPairSaves(app, files, d)) return false;
    cr::writeSave(dataPath(app), d);
    return true;
}

// A Vita import's save1 .. save4 and resume.dat (the per-pair files of one Vita save): data.dat made
// anew from the files it wrote; one already there is kept beside them as data-before-import-<time>.dat
bool foldVitaImport(App& app, const std::vector<std::string>& written) {
    std::vector<std::string> files;
    for (const std::string& name : written)
        if (name.size() == 9 && name.compare(0, 4, "save") == 0 && name.compare(5, 4, ".dat") == 0) files.push_back(savePath(app, name.c_str()));
    cr::SaveData active; // (all stamped the same second: the Vita's pair in play - its resume.dat's - folded last)
    if (std::find(written.begin(), written.end(), "resume.dat") != written.end() && cr::readSave(savePath(app, "resume.dat"), active)) {
        const std::string last = slotPath(app, active.route() + 1);
        const auto it = std::find(files.begin(), files.end(), last);
        if (it != files.end()) std::rotate(it, it + 1, files.end());
    }
    cr::SaveData d;
    if (!foldPairSaves(app, files, d)) return false;
    if (SDL_GetPathInfo(dataPath(app).c_str(), nullptr)) {
        const std::string keep = savePath(app, ("data-before-import-" + std::to_string(int64_t(std::time(nullptr))) + ".dat").c_str());
        if (!SDL_CopyFile(dataPath(app).c_str(), keep.c_str())) return false;
    }
    return cr::writeSave(dataPath(app), d);
}

// SaveData_Save: the whole game into data.dat with the pair in play's place and "saved" mark (the
// other pairs' places kept)
bool saveGame(App& app, bool perfect) {
    cr::SaveData s = captureSave(app);
    cr::SaveData old;
    if (readData(app, old)) s.places = old.places;
    // (SaveLoadPrc 8, 73621-73623: mode 4's _SaveData_PerfectSaveInit (71526) first - every pair's place its Ark,
    // area 7 / 13 / 19 / 25, room 97 / 97 / 105 / 105, at (0, 7.5, 15) facing 0 - the saved marks kept; then the
    // pair in play's own place, as always)
    if (perfect)
        for (int r = 0; r < 4; ++r) {
            cr::SaveData::Place& p = s.places[size_t(r)];
            p.area = 7 + 6 * r, p.map = r < 2 ? 97 : 105;
            p.pos[0] = 0, p.pos[1] = 7.5f, p.pos[2] = 15;
            p.yaw = 0;
        }
    s.places[size_t(s.route())] = s.placeInPlay();
    s.game.vars[9] = 0; // (71848: the saved copy's var 9, +0x30b28, written 0)
    if (!cr::writeSave(dataPath(app), s)) return false;
    SDL_RemovePath(savePath(app, "resume.dat").c_str()); // (_removeContData: a save ends the resume point)
    return true;
}

int currentPair(const App& app) { return app.playerChar / 2 + 1; }

// The title's plates, by what they are (0 the last battle: every pair cleared; 1 Load: a save;
// 2 New), drawn at (328, 136 + 48 x that number) - New alone sits at 232
std::vector<int> savePlates(const App& app) {
    const App::SaveScreen& ss = app.saveScreen;
    if (ss.allClear) return {0, 1, 2};
    if (!ss.haveSave) return {2};
    return {1, 2};
}

void openSaveScreen(App& app, bool load) {
    App::SaveScreen& ss = app.saveScreen;
    ss = App::SaveScreen();
    ss.open = true;
    ss.load = load;
    // the one save as the pair would load it (SaveData_Load_chk: its "saved" mark, +0xc14 + p;
    // Set_Prtyno + SaveData_Load_init: its own place and bag)
    const int pair = std::clamp((load ? app.chosenPair : currentPair(app)) - 1, 0, 3);
    cr::SaveData d;
    ss.haveSave = readData(app, d) && d.places[size_t(pair)].has;
    if (ss.haveSave) {
        d.takePlace(pair);
        cr::changeRoute(d.game, &app.itemTable, pair);
        ss.data = d;
    }
    if (load) { // __menuu_SaveLoadSet, the title's: the emblems and plates from the save's flags
        const cr::GameState sys = allPairsFlags(app);
        const int p = pair;
        const int emblem[3] = {31 + 2 * p, 32 + 2 * p, 39};
        for (int k = 0; k < 3; ++k)
            if (sys.flag(emblem[k])) ss.emblems |= 1u << k;
        ss.allClear = sys.flag(32) && sys.flag(34) && sys.flag(36) && sys.flag(38);
        ss.cleared = sys.flag(40 + p);
    }
    if (load && !ss.haveSave) { // no save of the pair: the boards are still drawn (__menuu_SaveLoadSet ->
        // ComUty_CharInfoInit, 0x4a714) from the party records as the title holds them - prty_init's, level 1
        ss.data = cr::SaveData();
        ss.data.game.party = cr::newParty();
    }
    if (!load) { // save: straight to "Save data?"
        ss.phase = 1;
        ss.pending = 1;
        ss.message = 57012;
    }
}

void closeSaveScreen(App& app) { app.saveScreen.open = false; }

// Util 6's save menu: SaveLoadClr with its title mark set (SaveLoadPrc 0xb / 0xc) - MuCom_PreReturnTitle
// (__bgmgo(0x8000000): the music out over 20 frames, the scene's effects ended), SetFade black, and
// MuCom_ReturnTitle: the field torn down and the boot script from the start
void saveScreenToTitle(App& app) {
    app.saveScreen.open = false;
    playBgm(app, 1u << 27);
    bootTitle(app);
}
// SaveLoadPrc 8 -> No with the title mark: message 0x3c, "return to the title without saving?" (0xc: its Yes
// the title, its No back to "Save data?"); the menu never closes into the field
void saveScreenNoToTitle(App& app) {
    App::SaveScreen& ss = app.saveScreen;
    ss.askLeave = !ss.askLeave;
    ss.message = ss.askLeave ? 57060 : 57012;
    ss.confirm = 0;
}

// Yes in the confirmation: do it
void saveScreenYes(App& app) {
    App::SaveScreen& ss = app.saveScreen;
    if (ss.toTitle && ss.askLeave) { saveScreenToTitle(app); return; } // leaving unsaved (SaveLoadPrc 0xc -> Yes)
    if (ss.pending == 1) {
        if (saveGame(app, ss.perfect)) {
            ss.data = captureSave(app);
            ss.haveSave = true;
            ss.message = 57082; // Save complete.
            menuSound(app, se::kSaved);
        } else {
            ss.message = -1; // couldn't write
            menuSound(app, se::kBuzzer);
        }
    } else if (ss.pending == 2) {
        const cr::SaveData s = ss.data;
        ss.open = false;
        const bool loaded = applySave(app, s);
        // applySave tears down the old session, including this menu. Rebuild only
        // its result panel, without reviving the pending load confirmation.
        ss.load = true;
        ss.haveSave = true;
        ss.data = s;
        ss.kind = 4;
        if (loaded) {
            ss.open = true;
            ss.message = 57083; // Load complete.
            menuSound(app, se::kLoaded);
        } else {
            ss.open = true;
            ss.message = -1;
            menuSound(app, se::kBuzzer);
        }
    } else if (ss.pending == 3) { // New: the boot script carries on with var 1 = the pair
        menuSound(app, se::kLoaded); // SaveLoadPrc plays it for any Yes in load mode
        if (ss.kind == 1 || ss.kind == 3) { // (SaveLoadPrc 4, 73551-73559: any Yes but a plain new game shows
            ss.message = 57083;             // 0x53 for 20 frames - state 5 - before it goes on)
            ss.phase = 2, ss.timer = 0, ss.startAfter = true;
            return;
        }
        saveScreenStart(app);
        return;
    }
    ss.phase = 2;
    ss.timer = 0;
}

// The title's choice taken up (SaveLoadClr): var 1 for the boot script, or a new game started
void saveScreenStart(App& app) {
    App::SaveScreen& ss = app.saveScreen;
    {
        ss.open = false;
        if (ss.kind == 1) { // Set_Prtyno(0): the last battle is the first pair's, on the one save
            app.chosenPair = 1;
            cr::SaveData d;
            if (readData(app, d)) {
                cr::GameState g = d.game;
                cr::changeRoute(g, &app.itemTable, 0);
                for (int f : {0, 4089}) g.setFlag(f, app.game.flag(f)); // the boot script's own flags
                for (const auto& [k, v] : app.game.vars) g.vars[k] = v; // and the variables it has set
                if (app.interruptShown) g.setFlag(0x44, true); // (TitleClr's _Load_SaveData 72137-72153, every path)
                app.game = std::move(g);
            }
        }
        if (app.booting && app.runner.running()) {
            app.game.vars[1] = ss.kind; // SaveLoadClr: SetVar(1, the choice) - the boot script branches on it
        } else {
            app.titleOpen = false;
            app.runner.stop();
            startNewGame(app);
        }
    }
}

void saveScreenChoose(App& app, int i) { // the title's plates (savePlates): the last battle, Load, New
    App::SaveScreen& ss = app.saveScreen;
    const std::vector<int> plates = savePlates(app);
    if (i < 0 || i >= int(plates.size())) return;
    ss.cursor = i;
    // (SaveLoadPrc 73517: Load with no save for this pair - all four cleared shows its plate - SE 2, nothing more)
    if (plates[size_t(i)] == 1 && !ss.haveSave) { menuSound(app, se::kChoose); return; }
    ss.phase = 1;
    ss.confirm = 0;
    ss.lines[0] = ss.lines[1] = ss.lines[2] = 0;
    switch (plates[size_t(i)]) {
    case 0: ss.kind = 1, ss.pending = 3, ss.message = 57056; break; // Proceed to the last battle?
    case 1: ss.kind = 4, ss.pending = 2, ss.message = 57013; break; // Load data?
    default:
        ss.pending = 3;
        if (ss.cleared) ss.kind = 3, ss.message = 57086, ss.lines[0] = 57087, ss.lines[1] = 57088; // with your clear data?
        else ss.kind = 2, ss.message = 57057; // Begin new game?
        break;
    }
    menuSound(app, se::kChoose);
}

// ComUty_GameTimeAndPlayTimeDraw: the save's date (picture 125 over a line; the month 126 + m,
// the day, 139, the year; the time with the colon 75) on a dark board at (324, 12), and under it
// __ComUty_PlayingTimeDraw at (324, 84): picture 126, the hours (to 999) and minutes
// ComUty_PictAnimDraw out and back (PictAnimInit's last 1): its count goes up to ticks x frames and
// back down to 0, one a draw; the piece is count / ticks steps to the right
int pictAnimFrame(int draws, int ticks, int frames) {
    const int top = ticks * frames, k = draws % (2 * top);
    return (k <= top ? k : 2 * top - k) / ticks;
}

// The menu font's size (_MuComFontFromIdRegist 0x52684: 14, but 13 when FUN_000373e4 finds the
// language outside 0, 5, 6, 7 - English, French, German, Spanish)
float menuFontSize(const App& app) { return cjkTextLanguage(app) ? 14.0f : 13.0f; }

void drawSaveDate(App& app, cr::MenuPainter& m, const cr::SaveData& d, float a, float dx = 0) { // dx: the window's slide
    (void)app;
    const time_t when = time_t(d.savedAt);
    struct tm tm {};
#ifdef _WIN32
    localtime_s(&tm, &when);
#else
    localtime_r(&when, &tm);
#endif
    const float dark[4] = {0, 0, 0, 0.5f * a}, white[4] = {1, 1, 1, a};
    m.quad(324 + dx, 12, 148, 64, dark, dark);
    m.picture(125, 372 + dx, 12, a);
    m.quad(325 + dx, 28, 146, 1, white, white);
    m.picture(126 + std::clamp(tm.tm_mon + 1, 1, 12), 336 + dx, 38, a);
    drawShopNumber(m, tm.tm_mday, 378 + dx, 38, 2, 12, a);
    m.picture(139, 400 + dx, 38, a);
    drawShopNumber(m, tm.tm_year + 1900, 410 + dx, 38, 4, 12, a);
    const int digit[4] = {tm.tm_hour / 10, tm.tm_hour % 10, tm.tm_min / 10, tm.tm_min % 10};
    static const float kAt[4] = {0, 12, 36, 48};
    for (int i = 0; i < 4; ++i)
        if (i || tm.tm_hour >= 10) drawShopNumber(m, digit[i], 368 + dx + kAt[i], 58, 1, 12, a);
    m.picture(75, 392 + dx, 58, a);
    const int secs = int(std::max(0.0, d.playSeconds));
    const int hours = std::min(999, secs / 3600), mins = std::min(59, secs / 60 % 60);
    m.quad(324 + dx, 84, 148, 40, dark, dark);
    m.picture(126, 344 + dx, 88, a);
    m.quad(325 + dx, 100, 146, 1, white, white);
    const int pd[5] = {hours / 100, hours % 100 / 10, hours % 10, mins / 10, mins % 10};
    static const float kPlay[5] = {32, 44, 56, 80, 92};
    for (int i = 0; i < 5; ++i)
        if (i || pd[i]) drawShopNumber(m, pd[i], 324 + dx + kPlay[i], 84 + 22, 1, 12, a);
    m.picture(75, 324 + dx + 68, 84 + 22, a);
}

void drawSaveScreen(App& app) {
    App::SaveScreen& ss = app.saveScreen;
    cr::Renderer& r = app.renderer;
    cr::MenuPainter& m = app.menu;
    m.begin(r, app.windowTex, &app.font);
    const PhoneMenu phoneMenu(app, m); // the iPhone layout: its screen's menu coordinates
    r.setOrigin(0);
    const bool art = ss.load && app.charSelectArt.id;
    if (art) r.fillRect(0, 0, r.screenW(), cr::Renderer::kLogicalH, 0, 0, 0, 1);
    r.setOrigin(r.uiX());
    // On the title (__menuu_SaveLoadPut 0x75e04, mode 1) the pair's Character Select art behind it all:
    // piece 16 of texture 0x11 (0x12 with flag 4092, the second sheet) at tex2d (-32, -64), z -10, in
    // white - under the menu mask (__menuu_SaveLoadSet's ChgBackMaskDrawOrder(1) / MuCom_MenuOneFrSetMask:
    // SetFade_back black at 0xa0), so 0x5f of it shows
    if (art) drawCharArt(app, 1, false, 1 - 0xA0 / 255.0f); // (else the menu mask, drawFrame's)
    r.setOrigin(r.uiX());
    const float a = std::min(1.0f, ss.anim / 8.0f);
    // Widescreen: the face boards, emblems to the screen's left; the date, the plates to its right;
    // Back and the message centred (as the shop). (No heading: SaveLoadPut draws none, 2.2.0 and 1.0.1
    // alike - the port had the Camp's)
    r.setOrigin(wideLeft(app));
    // The slot (__menuu_SaveLoadPut): the pair's face boards (ComUty_FaceBoardDraw, window 2) at (0, 24)
    // and (0, 112) - the save's on the title, the party's when saving -, on the title the save's date
    // and play time (ComUty_GameTimeAndPlayTimeDraw) top right and the pair's emblems (pictures
    // 111..113 at (16 + 72k, 224)). ComUty_CharInfoInit (0x4a714): the records 2 x pair + k, as many as
    // MuCom_ChkPartyNum (0x51ee0) gives - one while flag 0xfed + route (4077 + route) holds the second
    // back; with no save the party as the title holds it (openSaveScreen)
    const cr::GameState* shown = ss.load ? &ss.data.game : &app.game;
    const int route = ss.load ? std::clamp(app.chosenPair - 1, 0, 3) : std::clamp(app.playerChar / 2, 0, 3);
    for (int k = 0; k < (ss.perfect ? 0 : shown->flag(4077 + route) ? 1 : 2); ++k) { // (73769: none in mode 4)
        const int ch = 2 * route + k;
        int hp, hpMax, mp, mpMax;
        memberHpMp(app, ch, hp, hpMax, mp, mpMax, shown);
        drawFaceBoard(app, m, 0, 24 + 88.0f * float(k), ch, hp, hpMax, mp, mpMax, a, shown);
    }
    for (int k = 0; k < 3; ++k)
        if (ss.emblems & (1u << k)) m.picture(111 + k, 16 + 72.0f * float(k), 224, a);
    r.setOrigin(wideRight(app));
    if (ss.load && ss.haveSave) drawSaveDate(app, m, ss.data, a);
    if (ss.phase == 0) { // the plates at (328, 136 + 48 x which): the last battle, Load, New
        const std::vector<int> plates = savePlates(app);
        static const int kWord[3] = {4, 7, 6};
        for (size_t i = 0; i < plates.size(); ++i)
            m.plate(328, 136 + 48.0f * float(plates[i]), plateIcon(kWord[plates[i]]), kWord[plates[i]],
                    (ss.cursor == int(i) && menuCursorLit(app)) || pressIn(app, kPressSave, wideRight(app), 328, 136 + 48.0f * float(plates[i]), 136, 40),
                    a, false); // (window 3; the icon is the word's: ComUty_ChoicePlateDraw; lit while held - SaveLoadPrc 2's DAT_000d34a0)
        r.setOrigin(r.uiX());
        const float backY = r.phoneLayout() ? 286 : 344;
        m.backBar(192, backY, pressIn(app, kPressSave, r.uiX(), 192, backY, 232, 24), a); // (SaveLoadPut: BackButtonDraw mode 1, no arrows; lit while held - DAT_000d34a4)
    } else if (r.phoneLayout()) { // the original's (SaveLoadPut, the same in 2.2.0): the message
        // board (ComUty_MsgBoardDraw, picture 12) at (0, 220), lines from (24, 234) - MuComFontDisp's +1: 235; Yes / No the
        // plates at window 6 + (336, 104) / (336, 160). The board's mark (picture 13 at (458, 298), out and
        // back: SaveLoadSet's ComUty_PictAnimInit(d357c, 0x30, 1, 4, ..)) and its lines in the menu font
        m.picture(12, 0, 220, a);
        const short* mark = cr::kMenuPicture[13];
        m.piece(458, 298, float(mark[0] + 20 * pictAnimFrame(ss.anim, 1, 4)), mark[1], mark[2], mark[3], a);
        const std::string msg = ss.message > 0 ? sysText(app, ss.message) : std::string("The save couldn't be written.");
        m.text(msg.empty() ? "?" : msg, 24, 235, false, a, menuFontSize(app));
        int extra = 0;
        for (int l : ss.lines)
            if (l > 0) m.text(sysText(app, l), 24, 235 + 16.0f * float(++extra), false, a, menuFontSize(app));
        if (ss.phase == 1) { // (__ComUty_YesNoChoiceProc: a held plate takes the cursor)
            const bool hy = pressIn(app, kPressSave, r.uiX(), 336, 104, 136, 40), hn = pressIn(app, kPressSave, r.uiX(), 336, 160, 136, 40);
            m.plate(336, 104, 0, 0, hy || (!hn && ss.confirm == 0), a, false);
            m.plate(336, 160, 1, 1, hn || (!hy && ss.confirm == 1), a, false);
        }
    } else { // the message board, Yes / No under it while asking
        r.setOrigin(r.uiX());
        const float mx = 96, my = 232;
        drawWinPiece(app, kWinSmallPanel, mx * 2, my * 2, 320 * 2, 48 * 2);
        const std::string msg = ss.message > 0 ? sysText(app, ss.message) : std::string("The save couldn't be written.");
        int extra = 0;
        for (int l : ss.lines) extra += l > 0;
        const float ty = my + 24 - 9.0f * float(extra);
        m.textCentred(msg.empty() ? "?" : msg, mx + 20, ty, false, a, 15);
        for (int l = 0; l < extra; ++l) m.textCentred(sysText(app, ss.lines[l]), mx + 20, ty + 18.0f * float(l + 1), false, a, 13);
        if (ss.phase == 1) { // (a held one takes the cursor, as the iPhone layout's plates)
            const bool hy = pressIn(app, kPressSave, r.uiX(), 128, 288, 112, 48), hn = pressIn(app, kPressSave, r.uiX(), 272, 288, 112, 48);
            const bool yes = hy || (!hn && ss.confirm == 0), no = hn || (!hy && ss.confirm == 1);
            m.button(128, 296, 0, "", yes, a, yes); // Yes
            m.button(272, 296, 1, "", no, a, no);   // No
        }
    }
    r.setOrigin(wideLeft(app));
    if (keyHints(app)) // (our key hints)
        m.text(ss.phase == 1 ? "Left/Right  Enter  Esc: no" : ss.phase == 0 ? "Up/Down  Enter  Esc: back" : "", 20, 372, false,
           a * 0.8f, 9);
    r.setOrigin(r.uiX());
}

// One 20 Hz step: the fade-in, and the "done" message closing itself
void stepSaveScreen(App& app) {
    App::SaveScreen& ss = app.saveScreen;
    if (!ss.open) return;
    ++ss.anim;
    if (ss.phase == 2 && ++ss.timer >= 20) { // SaveLoadPrc 4 / 8: 0x14 frames of "Save complete." or the load's message
        if (ss.startAfter) {
            ss.startAfter = false;
            saveScreenStart(app);
            return;
        }
        ss.open = false;
        if (ss.load && ss.message > 0) app.titleOpen = false; // loaded: on with the game
        if (ss.toTitle) saveScreenToTitle(app); // (SaveLoadPrc 0xb: PreReturnTitle, then SaveLoadClr's ReturnTitle)
    }
}

void saveScreenKey(App& app, SDL_Keycode key) {
    App::SaveScreen& ss = app.saveScreen;
    const bool back = key == SDLK_ESCAPE || key == SDLK_BACKSPACE || key == SDLK_AC_BACK;
    const bool enter = key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE || key == SDLK_E;
    if (ss.phase == 2) {
        if (enter || back) ss.timer = 1000;
        return;
    }
    if (ss.phase == 1) {
        // (the Vita's SaveLoadPrc 4 (62580): the plates stacked at 104 / 160, moved with up / down - FUN_81008704;
        // video 04 458.5 s; left / right kept for keyboards)
        if (key == SDLK_LEFT || key == SDLK_A || key == SDLK_RIGHT || key == SDLK_D || key == SDLK_UP || key == SDLK_W ||
            key == SDLK_DOWN || key == SDLK_S) { ss.confirm ^= 1; menuSound(app, se::kCursor); }
        else if (enter && ss.confirm == 0) saveScreenYes(app);
        else if (enter || back) { // No
            // (SaveLoadPrc 4 / 8 play 2 themselves - ComUty_YesNoChoiceProc(.., 0); 0xc's "without saving?"
            // (.., 1): the Vita's × 3)
            menuSound(app, back && ss.toTitle && ss.askLeave ? se::kBack : se::kChoose);
            if (ss.toTitle) saveScreenNoToTitle(app);
            else if (ss.load && ss.kind == 3) { // then a plain new game? (57089..57091)
                ss.kind = 2;
                ss.message = 57089, ss.lines[0] = 57090, ss.lines[1] = 57091;
                ss.confirm = 0;
            } else if (ss.load) ss.phase = 0;
            else closeSaveScreen(app);
        }
        return;
    }
    const int n = int(savePlates(app).size());
    if (key == SDLK_UP || key == SDLK_W) { ss.cursor = (ss.cursor + n - 1) % n; menuSound(app, se::kCursor); }
    else if (key == SDLK_DOWN || key == SDLK_S) { ss.cursor = (ss.cursor + 1) % n; menuSound(app, se::kCursor); }
    else if (enter) saveScreenChoose(app, ss.cursor);
    else if (back) {
        if (ss.load && app.booting) app.game.vars[1] = 0; // back to Character Select
        closeSaveScreen(app);
        menuSound(app, se::kBack);
    }
}

void saveScreenClick(App& app, float px, float py) {
    App::SaveScreen& ss = app.saveScreen;
    float lx, ly;
    app.renderer.toLogical(px, py, lx, ly);
    float mx, my, rx;
    menuPoint(app, lx, ly, mx, my);
    menuPointAt(app, lx, ly, wideRight(app), rx, my); // (widescreen: the plates on the right)
    if (ss.phase == 2) { ss.timer = 1000; return; }
    if (ss.phase == 1) {
        if (app.renderer.phoneLayout()) { // the plates (0x88 x 0x28)
            if (mx >= 336 && mx < 472 && my >= 104 && my < 144) { ss.confirm = 0; saveScreenYes(app); }
            else if (mx >= 336 && mx < 472 && my >= 160 && my < 200) saveScreenKey(app, SDLK_ESCAPE);
            return;
        }
        if (my >= 288 && my < 336 && mx >= 128 && mx < 240) { ss.confirm = 0; saveScreenYes(app); }
        else if (my >= 288 && my < 336 && mx >= 272 && mx < 384) saveScreenKey(app, SDLK_ESCAPE);
        return;
    }
    const std::vector<int> plates = savePlates(app);
    for (size_t i = 0; i < plates.size(); ++i) { // _pad_chk_updw_box(328, 136 + 48k, 136, 40)
        const float py0 = 136 + 48.0f * float(plates[i]);
        if (rx >= 328 && rx < 464 && my >= py0 && my < py0 + 40) { saveScreenChoose(app, int(i)); return; }
    }
    const float backY = app.renderer.phoneLayout() ? 286 : 344; // 2.2.0's Back: (192, 286)
    // (_pad_chk_updw_box(0xc0, 0x11e, 0x100, 0x22): 256 x 34 - wider and taller than the bar drawn)
    if (my >= backY && my < backY + 0x22 && mx >= 0xc0 && mx < 0xc0 + 0x100) saveScreenKey(app, SDLK_ESCAPE);
}

// ---- The shop (Util 2: __menuu_ShopPrc / __menuu_ShopPut) ----
// Laid out as ShopPut draws it (its windows, stMenuf_Shop_Rect, all sit at 0, 0), in the menus'
// 512 x 384 units:
//   "Shop" (picture 97) at (8, 4); the Oz bar (152) at (316, 0), nine digits from x 362.
//   The shopkeeper (c101_L01_hd, 512 x 512 texels at half scale) at (-32, 128).
//   Talk / Buy / Sell plates at (328, 144 + 48k), the categories at (328, 96 + 48k), the Back
//   bar (ComUty_CmdBack type 1) at (192, 288).
//   The section labels: "Buy" at (12, 40) over line 15 at (0, 56), "> Weapons" at (40, 58) over
//   line 16 at (24, 72).
//   The list's rows (ComUty_ShopItemPlate: picture 8, 9 for the cursor's, 256 x 48) at x 192
//   from y 92, 48 apart, four shown: icon at +12 / +8, name at +56 / +12, price at +158 / +30
//   (six digits 10 apart), "/OZ" (101) at +226 / +34. Above them the HELP board with the item's
//   help text, beside them the scroll bar; for equipment ◀ ▶ beside Back pick the character.
//   The character's board (ComUty_StateBoardDraw at 12, 92) for equipment: face (144 on) at
//   (4, 60), name box (24, 92), LV box (24, 124), HP / MP / NEXT rows at y 148 / 168 / 188 with
//   their bars, EXP and the stats from y 208, 20 apart, values from x 92; the highlighted item's
//   change beside each (▲ / ▼, the amount in small digits).
//   How many: PRICE (98) at (312, 40) and the total from x 362, the count between < and >
//   buttons at y 60, Yes / No at (336, 104 / 160), the question on the message board (picture
//   12, 480 x 96) at (0, 220), its text from (24, 234).
// Widescreen keeps the left-hand group (heading, labels, shopkeeper, board) to the screen's left
// and the right-hand one (Oz, plates, list, Yes / No) to its right, as the later widescreen
// release does; Back and the message board stay centred.
namespace shopui {
constexpr int kMainWords[3] = {26, 27, 28};          // Talk, Buy, Sell
constexpr int kMainIcons[3] = {20, 22, 23};          // kanIconNoTbl - 1
constexpr int kCategoryWords[4] = {8, 29, 30, 31};   // Items, Weapons, Armor, Jewel
constexpr int kCategoryIcons[4] = {8, 24, 25, 26};
constexpr float kRowX = 192, kRowY = 92, kRowH = 48;
constexpr int kRows = 4;                              // rows shown
constexpr float kBackX = 192, kBackY = 288;
// MuScrList_Draw mode 0 (0xbc1f0): the focus line at 150, the rows cut to 76..280, the gauge at (456, 72)
constexpr float kFocusY = 150, kClipTop = 76, kClipBottom = 280;
constexpr uint32_t kSoundEquip = 21; // ShopPrc, Yes to "Equip it?"
// The board's stat rows (the delta table at 0xc8af0, from the board's y): HP, MP, Attack, Defense, Speed, Luck
constexpr float kStatRowY[6] = {56, 76, 136, 156, 176, 196};
} // namespace shopui

int shopRoute(const App& app) { return std::clamp(currentPair(app) - 1, 0, 3); }
// The character the board shows: the pair's first or second
int shopCharacter(const App& app) { return shopRoute(app) * 2 + std::clamp(app.shop.member, 0, 1); }
bool shopEquipment(const App& app) { return app.shop.category >= 1 && app.shop.category <= 3; }

int ownedCount(const App& app, int item) {
    auto it = app.game.items.find(item);
    return it == app.game.items.end() ? 0 : it->second;
}

// _GetItmCnt (59977) as 2.2.0 counts gear: its bag keeps what is worn (the equipment slots name it), so
// the count is the port's bag - the spares - and what the item's pair wears: weapons / armor 201..424
// the pair (n - 201) / 56's, jewels the current pair's
int heldCount(const App& app, int item) {
    int n = ownedCount(app, item);
    if (item < 201 || item > 438) return n;
    const int route = item <= 424 ? (item - 201) / 56 : std::clamp(app.playerChar / 2, 0, 3);
    for (int c : {2 * route, 2 * route + 1}) {
        const cr::PartyMember& pm = app.game.party[size_t(c)];
        n += int(std::count(std::begin(pm.equip), std::end(pm.equip), item));
    }
    return n;
}
// ... less the jewel a held-back second member wears (MuCom_ChkPartyNum 1: __Common_ListNextReady
// 66592-66596, MuComMakeItemData leaving it out)
int pairHeldCount(const App& app, int item) {
    const int route = std::clamp(app.playerChar / 2, 0, 3);
    const bool heldBack = app.game.flag(4077 + route);
    return heldCount(app, item) - (heldBack && item >= 425 && item <= 438 && app.game.party[size_t(2 * route + 1)].equip[cr::kJewel] == item);
}

// The Camp's jewel rules (the pair's members by camp.member): FUN_00055e90 (54057-54076) - a jewel held fewer
// than twice that the partner wears is refused (and drawn grey, 55553-55560); FUN_00055768 (53612-53640) - the
// jewel slot is refused with no jewel at all, or just one and the partner wearing a jewel
bool campJewelRefused(const App& app, int item) {
    if (item < 425 || item > 438 || heldCount(app, item) >= 2) return false;
    const int route = std::clamp(app.playerChar / 2, 0, 3);
    return app.game.party[size_t(2 * route + (app.camp.member == 0 ? 1 : 0))].equip[cr::kJewel] == item;
}
bool campJewelSlotRefused(const App& app) {
    int n = 0;
    for (int j = 425; j <= 438; ++j) n += heldCount(app, j);
    const int route = std::clamp(app.playerChar / 2, 0, 3);
    return n < 1 || (n == 1 && app.game.party[size_t(2 * route + (app.camp.member == 0 ? 1 : 0))].equip[cr::kJewel] != 0);
}

// The price of one in the current list (half when selling)
int shopUnitPrice(const App& app, int item) {
    const cr::ItemParam* p = app.itemTable.item(item);
    if (!p) return 0;
    if (app.shop.selling) return cr::sellPrice(*p);
    const int pr = shopRoute(app) * 2; // 173 Sexy Pose on either member: 10% off
    const bool pose = cr::hasSkill(app.game, pr, 173) || cr::hasSkill(app.game, pr + 1, 173);
    const int price = cr::buyPrice(*p);
    return pose ? price - price / 10 : price; // (__menuu_ShopPrc 6: the tenth rounded down comes off)
}

// _MuCom_IsShopNewState (0x51ac4): the buy list's "NEW" (picture 0x73) - flag 0xffb set and the shop past
// level 2: a consumable whose 0xbc150 level is the shop's or higher (the seeds 3..6 always: the loop's
// own exit after the first row), gear whose (n - 201) % 14 + 1 is
void plateText(const App& app, cr::MenuPainter& m, const std::string& s, float x, float y, int style, float a);
bool shopNewState(const App& app, int item) {
    static constexpr short kNew[15][2] = {{1, 2},  {21, 2}, {28, 2}, {2, 3},  {22, 3}, {29, 3}, {32, 3}, {33, 4},
                                          {34, 4}, {35, 4}, {36, 5}, {37, 5}, {38, 5}, {30, 6}, {31, 6}};
    const int level = app.shop.level;
    if (!app.game.flag(0xffb) || level < 3) return false;
    if (item >= 1 && item <= 80) {
        for (const auto& e : kNew) {
            if (e[0] == item && level <= e[1]) return true;
            if (item >= 3 && item <= 6) return true;
        }
        return false;
    }
    return item >= 201 && item <= 438 && level <= (item - 201) % 14 + 1;
}

int shopMaxCount(const App& app, int item) {
    if (app.shop.selling) return std::min(ownedCount(app, item), cr::kMaxItemCount);
    return cr::maxPurchase(shopUnitPrice(app, item), app.game.oz, heldCount(app, item)); // (65963: 99 - GetItmCnt)
}

void refreshShopList(App& app) {
    App::Shop& s = app.shop;
    s.list = s.selling ? cr::sellableItems(app.game, app.itemTable, shopCharacter(app), s.category)
                       : cr::shopStock(app.game, app.itemTable, shopCharacter(app), s.category, s.level);
    s.listCursor = std::clamp(s.listCursor, 0, std::max(0, int(s.list.size()) - 1));
    s.listTop = std::clamp(s.listTop, std::max(0, s.listCursor - shopui::kRows + 1), s.listCursor);
}

// _ComUty_CharInfoInit (0x4a714, 46342-46378): the pair's boards built afresh - and MP over the most
// brought down to it (MuCom_SetNowFldMp). It runs as the Camp and the shop open, after an item used in
// the Camp, an equipment change (__ChangeEquipItem, Remove too) and the shop's gear lists and Equip
void capPairMp(App& app) {
    const int route = std::clamp(app.playerChar / 2, 0, 3);
    for (int c : {2 * route, 2 * route + 1}) {
        cr::PartyMember& pm = app.game.party[size_t(c)];
        const int most = cr::memberStat(app.game, c, cr::kMp, app.itemTable);
        if (pm.mp > most) pm.mp = most;
    }
}

void openShop(App& app) {
    App::Shop& s = app.shop;
    cr::Texture keeper = s.keeper;
    const int category = s.category; // (the Vita's DAT_81114530: set only by a pick, 67112 - kept from visit to visit)
    s = App::Shop();
    s.keeper = keeper;
    s.category = category;
    s.open = true;
    s.level = cr::shopLevel(app.game, shopRoute(app));
    s.member = std::clamp(app.playerChar - shopRoute(app) * 2, 0, 1);
    s.bgmBack = app.game.vars[0]; // ShopSet: var 0 kept, the shop's music (_bgmgo(0x13))
    capPairMp(app); // (65675)
    playBgm(app, 0x13);
    if (!s.keeper.id) {
        cr::Image img;
        if (loadImage(app, "c101_L01_hd.mct", img)) s.keeper = app.renderer.upload(img);
    }
}

void closeShop(App& app) {
    if (app.shop.open && !app.shop.noBgmRecovery) playBgm(app, uint32_t(app.shop.bgmBack)); // ShopClr
    app.shop.open = false;
    app.renderer.destroy(app.shop.keeper);
}

// Talk: the windows go, the area's EvrStart 4 plays (it clears flag 4090 when done) while the
// script that opened the shop waits parked; then back to the plates
void startShopTalk(App& app) {
    const uint32_t at = app.events.returnEntry(4);
    if (!at) { menuSound(app, se::kBuzzer); return; }
    app.shop.phase = App::Shop::Talk;
    app.shop.timer = 0;
}

// The shop's list (ShopPrc -> FUN_0006a9d8 -> MuScrList_Proc, layout 0): the held touch a frame at a time (shopClick's
// right-hand units), its rows (scrlist::); the focus row follows them
void shopListPoint(const App& app, float px, float py, float& rx, float& my) {
    float lx, ly;
    app.renderer.toLogical(px, py, lx, ly);
    const float uiX = app.renderer.uiX(), rightX = app.renderer.phoneLayout() ? uiX : 2 * uiX;
    float cx = (lx - uiX) / 2;
    rx = (lx - rightX) / 2, my = ly / 2;
    phoneMenuPoint(app, cx, my);
    if (app.renderer.phoneLayout()) rx -= 16;
}
void stepShopList(App& app) {
    App::Shop& s = app.shop;
    const int n = int(s.list.size());
    scrlist::ready(s.scroll, 1, s.listCursor);
    if (s.phase == App::Shop::List && app.press.down && app.press.screen == kPressShop) {
        float rx, my;
        shopListPoint(app, app.press.px, app.press.py, rx, my);
        const double t = s.scroll.lastT + 1.0 / 20; // (a frame's way: the touch sampled a game frame at a time)
        if (s.scroll.hold == ScrList::None) scrlist::press(s.scroll, rx, my, n, t);
        else scrlist::move(s.scroll, rx, my, n, t);
    } else if (s.phase != App::Shop::List) s.scroll.hold = ScrList::None;
    const bool sound = scrlist::frame(s.scroll, 1, s.listCursor, n);
    s.listTop = s.listCursor; // (kept equal to the focus)
    if (sound && s.phase == App::Shop::List) menuSound(app, se::kListFocus); // (MuScrList_Draw: __effgo(1))
}

void stepShop(App& app) {
    App::Shop& s = app.shop;
    if (!s.open) return;
    ++s.anim;
    ++s.mark;
    s.bob += s.bobDir;
    if (s.bob >= 5 || s.bob <= 0) s.bobDir = -s.bobDir;
    stepShopList(app); // (FUN_0006a9d8: MuScrList_Proc)
    if (s.phase == App::Shop::Talk) {
        // (ShopPrc 3, 65811-65830: 0 -> 1 with a count of 4, run down past 0 - the talk on the 7th frame after the tap)
        if (s.timer >= 0 && ++s.timer > 6) {
            s.timer = -1;
            app.game.setFlag(4090, true);
            app.runner.swap(app.parkedRunner);
            if (!app.runner.startAt(app.events, app.events.returnEntry(4), 4, app.game, app.area, app.mapNumber)) {
                app.runner.swap(app.parkedRunner);
                s.phase = App::Shop::Main;
            } else {
                s.talkRunning = true;
                app.runnerWhat = "shop talk";
            }
        } else if (s.timer < 0 && s.backIn > 0) { // (3 -> 4: a count of 4 run down past 0 before the windows come)
            if (--s.backIn == 0) s.phase = App::Shop::Main, s.anim = 0;
        } else if (s.timer < 0 && !app.runner.running()) { // done: the shop's script again
            app.runner.swap(app.parkedRunner);
            s.talkRunning = false;
            app.game.setFlag(4090, false);
            s.backIn = 7;
        }
    }
}

void shopChooseMain(App& app, int i) {
    App::Shop& s = app.shop;
    s.cursor = i;
    menuSound(app, se::kChoose);
    if (i == 0) { startShopTalk(app); return; }
    s.selling = i == 2;
    s.phase = App::Shop::Category;
    s.anim = 0;
}

// __menuu_ShopPrc 4 / 5's 2 -> 3 (FUN_0006a968: ComUty_CharInfoInit, window 9): Buy's Weapons / Armor /
// Jewel and Sell's all but Items ask whose list first - the pair's plates; the list's Back returns there
bool shopAsksMember(const App& app) { return app.shop.category != 0; }
void shopChooseCategory(App& app, int c) {
    App::Shop& s = app.shop;
    // (ShopPrc's sale 2, 66123-66131: FUN_0006ae80 sounds 4 for an empty Items / Jewel, and __CheckItemList
    // keeps the category menu up; Weapons and Armor always open)
    if (s.selling && (c == 0 || c == 3) && cr::sellableItems(app.game, app.itemTable, shopCharacter(app), c).empty()) {
        menuSound(app, se::kBuzzer);
        return;
    }
    capPairMp(app); // (65891 / 66500: the gear lists' boards built afresh)
    s.category = c;
    s.listCursor = s.listTop = 0;
    s.scroll.key = -1; // (a new list: settled on its focus)
    menuSound(app, se::kChoose);
    s.anim = 0;
    if (shopAsksMember(app)) {
        s.member = 0;
        s.phase = App::Shop::Member;
        return;
    }
    refreshShopList(app);
    s.phase = App::Shop::List;
}
void shopChooseMember(App& app, int k) {
    App::Shop& s = app.shop;
    s.member = std::clamp(k, 0, campMembers(app) - 1);
    s.listCursor = s.listTop = 0;
    s.scroll.key = -1;
    refreshShopList(app);
    menuSound(app, se::kChoose);
    s.phase = App::Shop::List;
    s.anim = 0;
}

void shopChooseItem(App& app, int index) {
    App::Shop& s = app.shop;
    if (index < 0 || index >= int(s.list.size())) return;
    s.listCursor = index;
    const int item = s.list[size_t(index)];
    if (shopMaxCount(app, item) < 1) { menuSound(app, se::kBuzzer); return; } // can't afford / 99 already
    menuSound(app, se::kChoose);
    s.count = 1;
    s.confirm = 0;
    s.phase = App::Shop::Count;
    s.anim = 0;
    s.message = s.selling ? 57018 : 57017; // How many [item]s would you like to buy / sell?
}

// ◀ / ▶ in an equipment list: the pair's other character, and their list
// (__Common_ListReady 66817-66826: list mode 2 - < Back > and the swipe - only with both members there)
bool shopTwoMembers(const App& app) { return !app.game.flag(4077 + shopRoute(app)); }
void shopSwitchMember(App& app) {
    App::Shop& s = app.shop;
    if (!shopTwoMembers(app)) return;
    s.member ^= 1;
    s.listCursor = s.listTop = 0;
    refreshShopList(app);
    menuSound(app, se::kPage); // the Back bar's < > arrows
}

void shopBackToList(App& app) {
    App::Shop& s = app.shop;
    s.phase = App::Shop::List;
    s.message = 0;
    s.anim = 0;
    refreshShopList(app);
}

// Yes: pay, take the items (or the reverse); bought equipment the character can wear: "Equip [item]?"
void shopPay(App& app) {
    App::Shop& s = app.shop;
    const int item = s.list[size_t(s.listCursor)];
    const int total = shopUnitPrice(app, item) * s.count;
    if (s.selling) {
        int& have = app.game.items[item];
        have = std::max(0, have - s.count);
        cr::addOz(app.game, total);
    } else {
        app.game.items[item] = std::min(cr::kMaxItemCount, ownedCount(app, item) + s.count);
        cr::addOz(app.game, -total);
        // (the Vita's __menuu_ShopPrc 7: FUN_8106ad28(0x2f) - G+0x34e3d bit 7 - then the trophy check, 0x8105c11a)
        setTrophyBit(app, cr::trophies::kBought);
        checkTrophies(app);
    }
    menuSound(app, se::kPaid);
    if (s.selling) { // (ShopPrc 66173-66177: __CheckItemList - Items or Jewels with nothing left to sell go back to the
                     // categories; the focus on the sold item again, or the top once it's gone - SetListPosFromItemNo)
        refreshShopList(app);
        if ((s.category == 0 || s.category == 3) && s.list.empty()) {
            s.phase = App::Shop::Category, s.message = 0, s.anim = 0;
            return;
        }
        const auto at = std::find(s.list.begin(), s.list.end(), item);
        s.listCursor = s.listTop = at == s.list.end() ? 0 : int(at - s.list.begin());
    }
    const cr::ItemParam* p = app.itemTable.item(item);
    // (ShopPrc 7, 65990: asked after any equipment bought - DAT_000d30c8 is the category - one worn already too)
    if (!s.selling && p && cr::equipSlot(p->kind) >= 0 && (p->owners >> shopCharacter(app) & 1)) {
        s.phase = App::Shop::EquipAsk;
        s.message = 57076; // Equip [item]?
        s.confirm = 0;
        return;
    }
    shopBackToList(app);
}

// Yes to "Equip it?": worn by the board's character, what they wore goes back in the bag, and the pair's
// HP and MP refilled (ShopPrc 0xd, 66076: pcparam_hpmp_recovery, as Camp's __ChangeEquipItem)
void shopEquip(App& app) {
    App::Shop& s = app.shop;
    const int item = s.list[size_t(s.listCursor)];
    const cr::ItemParam* p = app.itemTable.item(item);
    if (p && cr::equipSlot(p->kind) >= 0 && ownedCount(app, item) > 0) {
        int& slot = app.game.party[size_t(shopCharacter(app))].equip[cr::equipSlot(p->kind)];
        if (slot > 0) app.game.items[slot] = std::min(cr::kMaxItemCount, ownedCount(app, slot) + 1);
        --app.game.items[item];
        slot = item;
        menuSound(app, shopui::kSoundEquip);
        ViewerHost(app).recoverPair(); // (anywhere: not the Camp's home-map test)
        capPairMp(app);                // (66077)
    }
    shopBackToList(app);
}

void shopBack(App& app) {
    App::Shop& s = app.shop;
    menuSound(app, s.selling && s.phase == App::Shop::Count ? se::kChoose : se::kBack); // (ShopPrc: a sale's No is SE 2, a purchase's 3)
    switch (s.phase) {
    case App::Shop::Main: closeShop(app); break;
    case App::Shop::Category: s.phase = App::Shop::Main; s.anim = 0; break;
    case App::Shop::Member: s.phase = App::Shop::Category; s.anim = 0; break;
    case App::Shop::List: s.phase = shopAsksMember(app) ? App::Shop::Member : App::Shop::Category; s.anim = 0; break;
    case App::Shop::Count: case App::Shop::EquipAsk: shopBackToList(app); break;
    default: break;
    }
}

std::string shopMessage(const App& app) {
    const App::Shop& s = app.shop;
    if (s.message <= 0 || s.list.empty()) return std::string();
    const int item = s.list[size_t(s.listCursor)];
    std::vector<std::vector<std::string>> pages;
    if (!app.systemText.message(
            s.message, app.language, nullptr, pages, nullptr,
            [&](int n) { return n == 9 ? itemName(app, item) : std::string(); }) ||
        pages.empty() || pages[0].empty())
        return std::string();
    return pages[0][0];
}

// ComUty_PictNumDraw: `digits` places `step` apart, right-aligned (left-aligned with `left`), in
// digit style `style` (picture 20 + style: 0 white, 2 outlined, 4 red / 5 cyan small)
void drawShopNumber(cr::MenuPainter& m, int value, float x, float y, int digits, float step, float alpha, int style,
                    bool left) {
    std::string t = std::to_string(std::abs(value));
    if (int(t.size()) > digits) t.assign(size_t(std::max(digits, 0)), '9'); // (ComUty_PictNumDraw 46630-46634: more digits than places - every place a 9)
    const int n = int(t.size());
    const short* p = cr::kMenuPicture[20 + style];
    for (int k = 0; k < n; ++k)
        m.piece(x + float((left ? 0 : digits - n) + k) * step, y, p[0] + float(t[size_t(k)] - '0') * p[2], p[1], p[2], p[3],
                alpha);
}

// __ComUty_SubColorBar: a 140-wide bar, `type` 0 the black track, 1 HP red -> orange, 2 MP green
// -> orange, 3 experience blue -> lilac (0xc8800), filled to value / most
void drawShopBar(cr::MenuPainter& m, float x, float y, float value, float most, int type, float alpha) {
    static const float kColour[4][2][3] = {{{0, 0, 0}, {0, 0, 0}},
                                           {{0xdd / 255.f, 0x02 / 255.f, 0}, {1, 0xc3 / 255.f, 0x73 / 255.f}},
                                           {{0x07 / 255.f, 0xaf / 255.f, 0x73 / 255.f}, {1, 0xc3 / 255.f, 0x73 / 255.f}},
                                           {{0x02 / 255.f, 0x84 / 255.f, 0xf4 / 255.f}, {0xdc / 255.f, 0xcb / 255.f, 1}}};
    const float f = most > 0 ? std::clamp(value / most, 0.0f, 1.0f) : 0.0f;
    if (f <= 0) return;
    const float* a = kColour[type][0];
    const float* b = kColour[type][1];
    const float l[4] = {a[0], a[1], a[2], alpha};
    const float rr[4] = {a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, a[2] + (b[2] - a[2]) * f, alpha};
    m.quad(x + 4, y + 1, 140 * f, 3, l, rr);
}

// ComUty_StateBoardDraw at (bx, by) for character c; `item` (if > 0) shows what wearing it changes
void drawShopBoard(App& app, float bx, float by, int c, int item, float a, bool face = true, int count = 0) {
    using namespace shopui;
    cr::MenuPainter& m = app.menu;
    const cr::PartyMember& pm = app.game.party[size_t(c)];
    const float white[4] = {1, 1, 1, a};
    m.quad(bx + 12, by, 143, 16, white, white); // the name
    m.textCentred(sysText(app, 58029 + c), bx + 12 + (143 - m.textWidth(sysText(app, 58029 + c), 14)) / 2, by + 8, true, a,
                  14);
    if (face) m.picture(144 + (facePicture(c) - 29), bx - 8, by - 32, a); // (StateBoardDraw's last argument: 0 on Status)
    m.quad(bx + 12, by + 32, 143, 15, white, white); // LV
    m.picture(82, bx + 50, by + 36, a);
    drawShopNumber(m, pm.level, bx + 80, by + 33, 3, 12, a, 2, true);
    const int hpMax = cr::memberStat(app.game, c, cr::kHp, app.itemTable), mpMax = cr::memberStat(app.game, c, cr::kMp, app.itemTable);
    const int hp = pm.hp < 0 ? hpMax : pm.hp, mp = pm.mp < 0 ? mpMax : pm.mp;
    for (int k = 0; k < 2; ++k) { // __ComUty_FaceBoardHpMp
        const float x = bx - 1, y = by + 56 + 20 * k;
        m.picture(k ? 63 : 62, x, y, a);
        drawShopNumber(m, k ? mp : hp, x + 22, y - 4, 5, 12, a);
        m.picture(k ? 65 : 66, x + 82, y - 4, a);
        drawShopNumber(m, k ? mpMax : hpMax, x + 94, y - 4, 5, 12, a);
        drawShopBar(m, x + 16, y + 9, 1, 1, 0, a);
        drawShopBar(m, x + 15, y + 8, float(k ? mp : hp), float(k ? mpMax : hpMax), k ? 2 : 1, a);
    }
    { // __ComUty_FaceBoardNextExp
        const float x = bx - 2, y = by + 96;
        const int from = cr::expForLevel(pm.level), to = cr::expForLevel(pm.level + 1);
        m.picture(64, x, y, a);
        drawShopNumber(m, std::max(0, to - pm.exp), x + 82, y - 4, 6, 12, a);
        drawShopBar(m, x + 16, y + 9, 1, 1, 0, a);
        drawShopBar(m, x + 15, y + 8, float(pm.exp - from), float(std::max(1, to - from)), 3, a);
    }
    // EXP and the four stats: label boxes (83..87) at x + 12, their underline (88) at x + 64
    const int values[5] = {pm.exp, cr::memberStat(app.game, c, cr::kAttack, app.itemTable), cr::memberStat(app.game, c, cr::kDefense, app.itemTable),
                           cr::memberStat(app.game, c, cr::kSpeed, app.itemTable), cr::memberStat(app.game, c, cr::kLuck, app.itemTable)};
    for (int k = 0; k < 5; ++k) {
        const float y = by + 116 + 20 * k;
        m.picture(83 + k, bx + 12, y, a);
        m.picture(88, bx + 64, y + 11, a);
        if (k == 0) drawShopNumber(m, values[0], bx + 68, y - 2, 7, 12, a);
        else drawShopNumber(m, values[k], bx + 80, y - 2, 4, 12, a);
    }
    if (item <= 0) return;
    const cr::ItemParam* p = app.itemTable.item(item);
    // (StateBoardDraw(.., item, count): a seed's count is its stat's change (47783-47800) - "+N" and the up arrow
    // on the result board; Remove (200) is a kind-8 item of no stats against the worn jewel (47744-47760))
    const bool seed = item >= 3 && item <= 6 && count > 0, remove = item == campui::kRemove;
    const cr::ItemParam* jewel = remove ? app.itemTable.item(app.game.party[size_t(c)].equip[cr::kJewel]) : nullptr;
    if (!seed && !jewel && (!p || cr::equipSlot(p->kind) < 0)) return;
    for (int st = 0; st < 6; ++st) { // the change: ▲ / ▼ at x + 158, sign and amount from x + 128 for the stats
        const int d = seed ? (st == cr::kAttack + (item - 3) ? count : 0)
                    : jewel ? -jewel->stat[st]
                            : cr::memberStat(app.game, c, cr::Stat(st), app.itemTable, item) - cr::memberStat(app.game, c, cr::Stat(st), app.itemTable);
        if (d == 0) continue;
        const float y = by + kStatRowY[st];
        m.picture(d > 0 ? 93 : 94, bx + 158, y, a);
        if (st < 2) continue;
        const int ad = std::abs(d);
        m.picture(d > 0 ? 89 : 92, bx + 128 + (ad >= 100 ? 0 : ad >= 10 ? 7 : 14), y, a);
        drawShopNumber(m, ad, bx + 135, y, 3, 7, a, d > 0 ? 5 : 4);
    }
}

// ComUty_NumCountDraw (0x4ad98) at (348, 68): white < and > buttons fading out at the ends around the
// box (black at 0xb3) and the number in two big digits always (picture 26 on: 28 x 28; PictNumDraw of
// count / 10 and count % 10, 1..99); both arrows always drawn whole. `dx`: the window's slide
// A side held by a touch (its struct's +5 / +4, set by _ComUty_NumCountProc) turns its box's gradient
// 0xEBFF4B - cyan
void drawNumCount(cr::MenuPainter& m, int count, int = 99, float dx = 0, float a = 1, bool minusHeld = false, bool plusHeld = false) {
    const float on[4] = {1, 1, 1, a * 0xB3 / 255.0f}, off[4] = {1, 1, 1, 0}, dark[4] = {0, 0, 0, a * 0xB3 / 255.0f};
    const float cyan[4] = {0x4B / 255.0f, 1, 0xEB / 255.0f, a * 0xB3 / 255.0f}, cyanOff[4] = {0x4B / 255.0f, 1, 0xEB / 255.0f, 0};
    const float* mOn = minusHeld ? cyan : on;
    const float* mOff = minusHeld ? cyanOff : off;
    const float* pOn = plusHeld ? cyan : on;
    const float* pOff = plusHeld ? cyanOff : off;
    m.quad(336 + dx, 60, 15, 31, mOff, mOn);
    m.quad(351 + dx, 60, 20, 31, mOn, mOn);
    m.quad(372 + dx, 60, 63, 31, dark, dark);
    m.quad(436 + dx, 60, 19, 31, pOn, pOn);
    m.quad(455 + dx, 60, 16, 31, pOn, pOff);
    m.picture(99, 348 + dx, 68, a);
    m.picture(100, 448 + dx, 68, a);
    const short* big = cr::kMenuPicture[26];
    const int n = std::clamp(count, 1, 99);
    m.piece(376 + dx, 62, big[0] + float(n / 10) * 28, big[1], 28, 28, a);
    m.piece(404 + dx, 62, big[0] + float(n % 10) * 28, big[1], 28, 28, a);
}

// A touch held on the shop (shopClick's units: the centre group, or the right-hand one) inside a box
bool shopPressIn(const App& app, bool right, float x, float y, float w, float h) {
    if (!app.press.down || app.press.screen != kPressShop) return false;
    float lx, ly;
    app.renderer.toLogical(app.press.px, app.press.py, lx, ly);
    const float uiX = app.renderer.uiX(), rightX = app.renderer.phoneLayout() ? uiX : 2 * uiX;
    float cx = (lx - uiX) / 2, rx = (lx - rightX) / 2, my = ly / 2;
    phoneMenuPoint(app, cx, my);
    if (app.renderer.phoneLayout()) rx -= 16;
    const float px = right ? rx : cx;
    return px >= x && px < x + w && my >= y && my < y + h;
}

void drawShop(App& app) {
    using namespace shopui;
    App::Shop& s = app.shop;
    cr::Renderer& r = app.renderer;
    cr::MenuPainter& m = app.menu;
    m.begin(r, app.windowTex, &app.font);
    const PhoneMenu phoneMenu(app, m); // the iPhone layout: its screen's menu coordinates
    if (s.phase == App::Shop::Talk) { // the shopkeeper's scene: ShopPrc 2 takes windows 1 - 5 off, "Shop" (window 0)
        r.setOrigin(wideLeft(app));      // stays over it
        m.picture(97, 8, 4, 1);
        r.setOrigin(r.uiX());
        return;
    }
    const float a = std::min(1.0f, float(s.anim + 1) / 6.0f), slide = (1 - a) * (1 - a);
    const float kFont = menuFontSize(app); // (the labels: MuComFontFromIdRegist's size)
    const float leftEdge = wideLeft(app), centre = r.uiX(), rightEdge = wideRight(app); // origins (the screen's edges inside the safe area)
    const bool list = s.phase == App::Shop::List;
    const bool asking = s.phase == App::Shop::Count || s.phase == App::Shop::EquipAsk;
    const bool board = list && shopEquipment(app);
    const int item = (list || asking) && !s.list.empty() ? s.list[size_t(s.listCursor)] : 0;

    // Left: the shopkeeper (or the character's board), the heading and the section labels
    r.setOrigin(leftEdge);
    // window 3 (FUN_0006a1e8, the shop's Put): tex 10's picture 3, 256 x 256 at (0, 64) of the phone's screen
    // (+ (16, 32) in menu units); the iPad layout keeps 1.0.1's place
    if (s.keeper.id && !list && !board) {
        const bool phone = r.phoneLayout();
        r.drawQuad(s.keeper, ((phone ? 16 : -32) - 240 * slide) * 2, (phone ? 32 + 64 : 128) * 2, 256 * 2, 256 * 2, 1, 1, 1, 1);
    }
    if (board) drawShopBoard(app, 12 - 200 * slide, 92, shopCharacter(app), item, a); // (FUN_0006a6c0 66395-66421: the focused item, sold or bought)
    m.picture(97, 8, 4, 1); // "Shop"
    if (s.phase == App::Shop::Main) { // window 2: font 0 (7, 0x3a) "Shopping" at (12, 40) over picture 15
        m.textCentred(sysText(app, 58058), 12 - 140 * slide, 47, false, a, kFont);
        m.picture(15, -140 * slide, 56, a);
    } else { // window 6: "Buy" / "Sell" (7, 0x5a / 0x5b); window 7: the category (7, 0xbd118's 99, 73, 74, 75)
        m.textCentred(sysText(app, s.selling ? 58091 : 58090), 12, 47, false, 1, kFont);
        m.picture(15, 0, 56, 1);
        if (list || asking) {
            static const int kCategory[4] = {58099, 58073, 58074, 58075}; // >Items, >Weapons, >Armor, >Jewel
            m.textCentred(sysText(app, kCategory[std::clamp(s.category, 0, 3)]), 40, 65, false, 1, kFont);
            m.picture(16, 24, 72, 1);
        }
    }

    // Right: Oz, the plates, the list, the count and Yes / No
    r.setOrigin(rightEdge);
    if (list && !s.list.empty()) drawHelpWindow(app, m, item, a); // HELP: the cursor's item (ComUty_HelpWindowDraw)
    m.picture(152, 316, 0, 1); // the Oz bar
    drawShopNumber(m, app.game.oz, 362, 0, 9, 12, 1);
    if (s.phase == App::Shop::Main)
        for (int i = 0; i < 3; ++i)
            m.plate(328 + 240 * slide, 144 + 48.0f * float(i), kMainIcons[i], kMainWords[i],
                    (s.cursor == i && menuCursorLit(app)) || shopPressIn(app, true, 328, 144 + 48.0f * float(i), 136, 40), a, false);
    if (s.phase == App::Shop::Category)
        for (int i = 0; i < 4; ++i)
            m.plate(328 + 240 * slide, 96 + 48.0f * float(i), kCategoryIcons[i], kCategoryWords[i],
                    (s.category == i && menuCursorLit(app)) || shopPressIn(app, true, 328, 96 + 48.0f * float(i), 136, 40), a, false); // (FUN_0006af18: held)
    if (s.phase == App::Shop::Member) // FUN_0006a634: window 9, the pair's plates at (328, 176 + 56k) (FUN_0004f234)
        for (int k = 0; k < campMembers(app); ++k) {
            const int word = 12 + campCharacter(app, k);
            m.plate(328 + 240 * slide, 176 + 56.0f * float(k), plateIcon(word), word,
                    (s.member == k && menuCursorLit(app)) || shopPressIn(app, true, 328, 176 + 56.0f * float(k), 136, 40), a, false); // (FUN_0006a860: held)
        }
    if (list) { // MuScrList_Draw mode 0: rows from the focus line at 150 less the scroll, cut to 76..280
        const int n = int(s.list.size());
        const float scroll = s.scroll.key == 1 ? float(s.scroll.shown()) : kRowH * float(s.listCursor);
        const int focus = s.scroll.key == 1 ? scrlist::focusOf(s.scroll, n) : s.listCursor; // (__MuScrList_GetFocusPos)
        m.setClip(kClipTop, kClipBottom);
        for (int index = 0; index < n; ++index) {
            const int it = s.list[size_t(index)];
            const float x = kRowX + 160 * slide, y = kFocusY + kRowH * float(index) - scroll;
            if (y + kRowH <= kClipTop || y >= kClipBottom) continue;
            // (ShopPut's rows, 55665-55695: the name yellow (style 3) for what the board's character wears,
            // grey (2) when it costs more than the purse or 99 are held; the plate and icon as they are)
            int style = 0;
            if (!s.selling) {
                const cr::PartyMember& pm = app.game.party[size_t(shopCharacter(app))];
                if (std::find(std::begin(pm.equip), std::end(pm.equip), it) != std::end(pm.equip)) style = 3;
                if (shopUnitPrice(app, it) > app.game.oz || heldCount(app, it) > 98) style = 2;
            } else // (ShopPut 9, 55706-55738: selling, yellow for what either member wears; no grey)
                for (int g = 0; g < campMembers(app); ++g) {
                    const cr::PartyMember& pm = app.game.party[size_t(campCharacter(app, g))];
                    if (std::find(std::begin(pm.equip), std::end(pm.equip), it) != std::end(pm.equip)) style = 3;
                }
            const float ra = a;
            m.picture(index == focus ? 9 : 8, x, y, ra);
            if (const cr::ItemParam* p = app.itemTable.item(it); p && p->icon >= 0)
                m.piece(x + 12, y + 8, float(p->icon % 16) * 32, 248 + float(p->icon / 16) * 32, 32, 32, ra);
            plateText(app, m, itemName(app, it), x + 56, y + 12, style, ra);
            if (!s.selling && shopNewState(app, it)) m.picture(0x73, x + 8, y + 4, ra); // (ComPlate: 0x73 at (x + 8, y + 4))
            drawShopNumber(m, shopUnitPrice(app, it), x + 158, y + 30, 6, 10, ra);
            m.picture(101, x + 226, y + 34, ra);
        }
        if (n > 2) { // the jump pills: down over the first rows, up under the last (0x8c / 0x8d at x 280)
            if (focus < 2) m.picture(0x8c, 280 + 160 * slide, 112 - scroll, a);
            if (focus >= n - 2) m.picture(0x8d, 280 + 160 * slide, 208 + kRowH * float(n - 1) - scroll, a);
        }
        m.clearClip();
        drawCampScrollBar(m, scroll, n, a); // (ComUty_GuageBarDraw at (456, 72), 208 long)
    }
    if (asking && item > 0) {
        if (s.phase == App::Shop::Count) {
            m.picture(98, 312, 40, 1); // PRICE and the total
            drawShopNumber(m, shopUnitPrice(app, item) * s.count, 362, 40, 9, 12, 1);
            m.picture(73, 362, 49, 1);
            drawNumCount(m, s.count, shopMaxCount(app, item), 0, 1, shopPressIn(app, true, 336, 60, 36, 31),
                         shopPressIn(app, true, 436, 60, 36, 31)); // (two digits, the box at 0xb3: ComUty_NumCountDraw)
        }
        // (a held plate takes the cursor: __ComUty_YesNoChoiceProc)
        const bool hy = shopPressIn(app, true, 336, 104, 136, 40), hn = shopPressIn(app, true, 336, 160, 136, 40);
        m.plate(336, 104, 0, 0, hy || (!hn && s.confirm == 0), 1, false); // Yes
        m.plate(336, 160, 1, 1, hn || (!hy && s.confirm == 1), 1, false); // No
    }

    // Centre: Back (with ◀ ▶ for equipment lists) and the message board
    r.setOrigin(centre);
    if (asking && item > 0) { // ComUty_MsgBoardDraw (window 0x10, (0, 220)): its mark, the lines at (24, 234 + MuComFontDisp's 1), 16 apart, one style
        m.picture(12, 0, 220, 1);
        const short* mark = cr::kMenuPicture[13];
        m.piece(458, 298, float(mark[0] + 20 * pictAnimFrame(s.mark, 1, 4)), mark[1], mark[2], mark[3], 1);
        m.text(shopMessage(app), 24, 235, false, 1, kFont);
        std::vector<std::vector<std::string>> have;
        if (s.phase == App::Shop::Count &&
            // (__Common_ListNextReady 66590-66597: GetItemCnt - worn gear counted - less the held-back member's jewel)
            app.systemText.message(57046, app.language, [&](int) { return pairHeldCount(app, item); }, have) && !have.empty() &&
            !have[0].empty())
            m.text(have[0][0], 24, 251, false, 1, kFont);
    } else if (list && shopEquipment(app) && shopTwoMembers(app)) {
        // ComUty_CmdBack 2: < Back > - the part held lit (1 Back, 2 right, 3 left: the boxes shopClick tests)
        m.backButton(kBackX, kBackY, shopPressIn(app, false, kBackX + 51, kBackY, 130, 24) ? 1 : shopPressIn(app, false, 0x175, kBackY, 0x33, 24) ? 2
                                     : shopPressIn(app, false, 0xc0, kBackY, 0x33, 24) ? 3 : 0, s.bob, a);
    } else {
        m.backBar(kBackX, kBackY, shopPressIn(app, false, kBackX, kBackY, 232, 24), a); // (lit while held: DAT_000d307c)
    }
    const char* help = s.phase == App::Shop::Count     ? "Left/Right: 1  Shift: 10  Up/Down: Yes / No  Enter  Esc"
                       : s.phase == App::Shop::EquipAsk ? "Up/Down  Enter  Esc: no"
                       : list && shopEquipment(app)     ? "Up/Down  Left/Right: character  Enter  Esc: back"
                                                        : "Up/Down  Enter  Esc: back";
    if (keyHints(app)) // (our key hints)
        m.text(help, 20, 372, false, a * 0.8f, 9);
    r.setOrigin(r.uiX());
}

void shopKey(App& app, SDL_Keycode key) {
    App::Shop& s = app.shop;
    const bool back = key == SDLK_ESCAPE || key == SDLK_BACKSPACE || key == SDLK_AC_BACK;
    const bool enter = key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE || key == SDLK_E;
    const bool up = key == SDLK_UP || key == SDLK_W, down = key == SDLK_DOWN || key == SDLK_S;
    // (the Vita: L / R - PgUp / PgDn - switch the list's member (the list's Back bar in mode 2:
    // __Common_ListReady's SetIdInfo(8 / 9, .., 2), MuScrList_Proc's -3 / -2) and change the count
    // (_ComUty_NumCountProc, FUN_8105199e); left / right too)
    const bool lr = s.phase == App::Shop::List || s.phase == App::Shop::Count;
    const bool left = key == SDLK_LEFT || key == SDLK_A || (lr && key == SDLK_PAGEUP),
               right = key == SDLK_RIGHT || key == SDLK_D || (lr && key == SDLK_PAGEDOWN);
    switch (s.phase) {
    case App::Shop::Main:
        if (up || down) { s.cursor = (s.cursor + (up ? 2 : 1)) % 3; menuSound(app, se::kCursor); }
        else if (enter) shopChooseMain(app, s.cursor);
        else if (back) shopBack(app);
        break;
    case App::Shop::Category:
        if (up || down) { s.category = (s.category + (up ? 3 : 1)) % 4; menuSound(app, se::kCursor); }
        else if (enter) shopChooseCategory(app, s.category);
        else if (back) shopBack(app);
        break;
    case App::Shop::Member:
        if ((up || down || left || right) && campMembers(app) > 1) { s.member ^= 1; menuSound(app, se::kCursor); }
        else if (enter) shopChooseMember(app, s.member);
        else if (back) shopBack(app);
        break;
    case App::Shop::List: {
        const int n = int(s.list.size());
        if ((up || down) && n > 0) {
            s.listCursor = (s.listCursor + (up ? n - 1 : 1)) % n;
            s.listTop = s.listCursor; // (the focus row on the line)
            menuSound(app, se::kListFocus);
        } else if ((left || right) && shopEquipment(app)) shopSwitchMember(app);
        else if (enter) { // (the Vita's: with the rows at rest)
            if (scrlist::rests(s.scroll, 1)) shopChooseItem(app, s.listCursor);
        }
        else if (back) shopBack(app);
        break;
    }
    case App::Shop::Count: {
        const int most = shopMaxCount(app, s.list[size_t(s.listCursor)]);
        const int step = (SDL_GetModState() & SDL_KMOD_SHIFT) ? 10 : 1;
        if (left || right) {
            const int was = s.count;
            s.count = std::clamp(s.count + (right ? step : -step), 1, std::max(1, most));
            menuSound(app, s.count != was ? se::kChoose : se::kBuzzer); // ComUty_NumCountProc
        } else if (up || down) { s.confirm ^= 1; menuSound(app, se::kCursor); }
        else if (enter && s.confirm == 0) shopPay(app);
        else if (enter || back) shopBack(app);
        break;
    }
    case App::Shop::EquipAsk:
        if (up || down || left || right) { s.confirm ^= 1; menuSound(app, se::kCursor); }
        else if (enter && s.confirm == 0) shopEquip(app);
        else if (enter || back) shopBack(app);
        break;
    default: break;
    }
}

void shopClick(App& app, float px, float py) {
    App::Shop& s = app.shop;
    float lx, ly;
    app.renderer.toLogical(px, py, lx, ly);
    const float uiX = app.renderer.uiX(), rightX = app.renderer.phoneLayout() ? uiX : 2 * uiX;
    float cx = (lx - uiX) / 2, rx = (lx - rightX) / 2, my = ly / 2; // centre / right-hand groups
    phoneMenuPoint(app, cx, my);
    if (app.renderer.phoneLayout()) rx -= 16;
    using shopui::kBackX;
    using shopui::kBackY;
    // (_pad_chk_updw_box(0xc0, 0x11e, 0x100, 0x22): 192..448 x 286..320 - wider and taller than the bar drawn)
    const bool onBack = my >= 0x11e && my < 0x11e + 0x22 && cx >= 0xc0 && cx < 0xc0 + 0x100;
    auto plateAt = [&](float x0, float y0, float step, int n) {
        for (int i = 0; i < n; ++i)
            if (rx >= x0 && rx < x0 + 136 && my >= y0 + step * i && my < y0 + 40 + step * i) return i;
        return -1;
    };
    switch (s.phase) {
    case App::Shop::Main:
        if (const int i = plateAt(328, 144, 48, 3); i >= 0) shopChooseMain(app, i);
        else if (onBack) shopBack(app);
        break;
    case App::Shop::Category:
        if (const int i = plateAt(328, 96, 48, 4); i >= 0) shopChooseCategory(app, i);
        else if (onBack) shopBack(app);
        break;
    case App::Shop::Member:
        if (const int i = plateAt(328, 176, 56, campMembers(app)); i >= 0) shopChooseMember(app, i);
        else if (onBack) shopBack(app);
        break;
    case App::Shop::List: {
        using namespace shopui;
        const bool arrows = shopEquipment(app) && shopTwoMembers(app) && my >= kBackY && my < kBackY + 24;
        const int n = int(s.list.size());
        // MuScrList_Proc (scrlist::): a drag's or the gauge's lift moves only the rows; a tap over / under the line a row
        // on, the pills' boxes to the other end, the line's row chosen once the rows rest
        scrlist::ready(s.scroll, 1, s.listCursor);
        float sx, sy;
        shopListPoint(app, px, py, sx, sy);
        if (!scrlist::lift(s.scroll, sx, sy, n, s.scroll.lastT + 1.0 / 20)) {
            if (scrlist::follow(s.scroll, s.listCursor, n)) menuSound(app, se::kListFocus);
            s.listTop = s.listCursor;
            break;
        }
        if (const int r = scrlist::tap(s.scroll, sx, sy, n); r) {
            if (r == 2) scrlist::follow(s.scroll, s.listCursor, n), shopChooseItem(app, s.listCursor);
            else if (r == 1 && scrlist::follow(s.scroll, s.listCursor, n)) menuSound(app, se::kListFocus);
            s.listTop = s.listCursor;
        } else if (arrows && ((cx >= 0xc0 && cx < 0xf3) || (cx >= 0x175 && cx < 0x1a8)))
            shopSwitchMember(app);
        else if (onBack) shopBack(app);
        break;
    }
    case App::Shop::Count:
        // (the count's < >: stepped while held - pressCountStep, _ComUty_NumCountProc -, not on the lift)
        if (my >= 60 && my < 91 && ((rx >= 336 && rx < 372) || (rx >= 436 && rx < 472))) {}
        else if (const int i = plateAt(336, 104, 56, 2); i == 0) shopPay(app);
        else if (i == 1) shopBack(app);
        break;
    case App::Shop::EquipAsk:
        if (const int i = plateAt(336, 104, 56, 2); i == 0) shopEquip(app);
        else if (i == 1) shopBack(app);
        break;
    default: break;
    }
}

// ---- The Camp menu (__menuu_CampMain*, __MuCamp_NextProc and its pages) ----
// Laid out as CampMainPut draws it (stMenuf_CampMain_Rect: every window at 0, 0), in the menus'
// 512 x 384 units:
//   "Camp" (picture 81) at (8, 4); the clock and the purse (ComUty_BattryAndRealTimeAndOzDraw);
//   the pair's boards (ComUty_FaceBoardDraw) at (0, 24 + 88k); the plates Items, Equip, Genes,
//   Status, Options (0xc8600) at (328, 48 + 48k); Back; the message board (picture 12, (0, 220)).
// On a page the boards slide out left, the plates right; the page's title (font 0 at (12, 40) over
// picture 15 at (0, 56)) and section (font 2 at (40, 58), picture 16 at (24, 72)) come in, the
// pair's name plates at (328, 176 + 56k) and the state board (ComUty_StateBoardDraw) at (12, 92).
//   Items: the bag by category (make_item_list: Standard 1-60, Event 61-80, Weapons, Armor,
//     Jewel, Gene Plates; ◀ ▶ skips empty ones) with counts (ComUty_CampItemPlate); the seeds
//     (3-6) and the MP items (24-27) are used on a member (use_item: a seed point each, up to
//     5000 a stat; MP +100 / +250 / +500 / full), the seeds by the count asked (57077).
//   Equip: a member, then weapon / armor / jewel and what the bag holds for it, the board
//     showing what it changes; wearing one puts the old back in the bag.
//   Status: a member's board (at (308, 68)).
// Genes wait for the gene plates. Save is the port's own (the original saves from the field's
// SAVE button and save points).

bool keyHints(const App& app) { return app.showHud && !app.renderer.phoneLayout(); }
int plateIcon(int word) { return word >= 0 && word < 37 ? campui::kIconOfWord[word] - 1 : -1; }
int campRoute(const App& app) { return std::clamp(app.playerChar / 2, 0, 3); }
// ComUty_CharInfoInit (0x4a714): the boards are the route's pair (records 2 x route + k);
// MuCom_ChkPartyNum (0x51ee0): one while flag 0xfed + route holds the second back
int campCharacter(const App& app, int member) { return campRoute(app) * 2 + std::clamp(member, 0, 1); }
int campMembers(const App& app) { return app.game.flag(4077 + campRoute(app)) ? 1 : 2; }

// ComUty_BattryAndRealTimeAndOzDraw: the clock (TIME, 71, at (363, 8); the digits at +47 / +59 /
// +83 / +95, the colon (75) blinking at +71; the line (74) under it) and the purse (OZ, 72, at
// (326, 24); nine digits from (362, 26); the line at (362, 36)) and the battery (__ComUty_BattryDraw at
// (335, 9): picture 0x75 full, +1 under 51%, +2 under 18% - the lower kept from 18 up to 23 when it came from
// under 18 -, +3 / +4 / +5 the same charging, +6 charged; SDL's power info, looked at once a second; a
// machine without a battery as charged)
int batteryPicture() {
    static Uint64 at = 0;
    static int picture = 0x75 + 6;
    static bool low = false; // (_gbMiddleFlg)
    if (at && SDL_GetTicks() - at < 1000) return picture;
    at = SDL_GetTicks();
    int pct = -1;
    const SDL_PowerState st = SDL_GetPowerInfo(nullptr, &pct);
    const int rest = pct < 0 ? 100 : pct;
    const bool charging = st == SDL_POWERSTATE_CHARGING;
    if (st == SDL_POWERSTATE_NO_BATTERY || st == SDL_POWERSTATE_UNKNOWN || st == SDL_POWERSTATE_ERROR) return picture = 0x75 + 6;
    if (rest >= 51) picture = 0x75 + (st == SDL_POWERSTATE_CHARGED ? 6 : charging ? 3 : 0);
    else if (rest >= 23) picture = 0x75 + (charging ? 4 : 1), low = false;
    else if (rest < 18) picture = 0x75 + (charging ? 5 : 2), low = true;
    else picture = 0x75 + (low ? (charging ? 5 : 2) : (charging ? 4 : 1));
    return picture;
}

void drawClockAndOz(App& app, cr::MenuPainter& m, float a) {
    m.picture(batteryPicture(), 335, 9, a);
    const time_t now = time(nullptr);
    struct tm tm {};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    m.picture(71, 363, 8, a);
    const int digit[4] = {tm.tm_hour / 10, tm.tm_hour % 10, tm.tm_min / 10, tm.tm_min % 10};
    static const float kAt[4] = {0, 12, 36, 48};
    for (int i = 0; i < 4; ++i)
        if (i || digit[0]) drawShopNumber(m, digit[i], 363 + 47 + kAt[i], 6, 1, 12, a);
    // (__ComUty_RealTimeDraw 0x4bcf0: the colon while its own count, one a frame, is under 20 of 40 - a second
    // on, a second off at 20 Hz)
    if (SDL_GetTicks() / 1000 % 2 == 0) m.picture(75, 363 + 71, 6, a);
    m.picture(74, 363 + 36, 16, a);
    m.picture(72, 326, 24, a);
    drawShopNumber(m, app.game.oz, 362, 26, 9, 12, a);
    m.picture(74, 362, 36, a);
}

// ComUty_FaceBoardDraw at (x, y): the face (29 + n), the board (28) and the name (37 + n and the
// word 53 + n, English) at y + 24, LV (61) and the level at (24 / 48, 86), HP and MP
// (__ComUty_FaceBoardHpMp, style 2) at (96, 50 / 70), NEXT at (96, 90)
void drawFaceBoard(App& app, cr::MenuPainter& m, float x, float y, int c, int hp, int hpMax, int mp, int mpMax, float a,
                   const cr::GameState* gs) {
    const cr::PartyMember& pm = (gs ? *gs : app.game).party[size_t(std::clamp(c, 0, 7))];
    const int face = facePicture(c) - 29;
    m.picture(28, x + 8, y + 24, a);
    m.picture(37 + face, x + 136, y + 24, a);
    // __ComUty_FaceBoardBackDraw (2.2.0 0x4a86c, 1.0.1 0x191e8): the name - row 8 (Shea) for face 7 under
    // MuCom_GetManaSheaCheck (flag 0x996 on the fourth pair, or flag 0x997); 2.2.0: window_char's (MuTex
    // table 3, face + 9 x language: 0xbb330 / 0xbb350), 1.0.1: sheet 0's pictures 45 / 53 + face, 0x99 / 0x9a
    const bool shea = face == 7 && ((app.game.flag(0x996) && campRoute(app) == 3) || app.game.flag(0x997));
    if (languageSheet(app))
        m.imagePiece(app.charSheet, x + 96, y + 24, 92.0f * float(std::clamp(app.language, 0, 7)), 32.0f + 20.0f * float(shea ? 8 : face),
                     92, 20, a);
    else if (shea) m.picture(app.language == 1 ? 0x99 : 0x9a, x + 96, y + 24, a);
    else m.picture((app.language == 0 ? 45 : 53) + face, x + 96, y + 24, a);
    m.picture(29 + face, x, y, a);
    m.picture(61, x + 24, y + 86, a);
    // (__ComUty_FaceBoardFaceDraw: PictNumDraw(x + 0x30, y + 0x56, level, 3 places, 12, style 1) right-aligned -
    // Part 01 923 s: "LV      1", the digit at the field's right)
    drawShopNumber(m, pm.level, x + 48, y + 86, 3, 12, a, 1);
    for (int k = 0; k < 2; ++k) {
        const float hx = x + 96, hy = y + 50 + 20.0f * float(k);
        m.picture(k ? 63 : 62, hx, hy, a);
        drawShopNumber(m, k ? mp : hp, hx + 22, hy - 4, 5, 12, a, 2);
        m.picture(65, hx + 82, hy - 4, a);
        drawShopNumber(m, k ? mpMax : hpMax, hx + 94, hy - 4, 5, 12, a, 2);
        drawShopBar(m, hx + 16, hy + 9, 1, 1, 0, a);
        drawShopBar(m, hx + 15, hy + 8, float(k ? mp : hp), float(k ? mpMax : hpMax), k ? 2 : 1, a);
    }
    const float nx = x + 96, ny = y + 90;
    const int from = cr::expForLevel(pm.level), to = cr::expForLevel(pm.level + 1);
    m.picture(64, nx, ny, a);
    drawShopNumber(m, pm.level >= cr::kMaxLevel ? 0 : std::max(0, to - pm.exp), nx + 82, ny - 4, 6, 12, a, 2);
    drawShopBar(m, nx + 16, ny + 9, 1, 1, 0, a);
    drawShopBar(m, nx + 15, ny + 8, float(pm.exp - from), float(std::max(1, to - from)), 3, a);
}

// The member's HP (full outside battles) and MP as the boards show them
void memberHpMp(const App& app, int c, int& hp, int& hpMax, int& mp, int& mpMax, const cr::GameState* gs) {
    const cr::GameState& g = gs ? *gs : app.game;
    const cr::PartyMember& pm = g.party[size_t(c)];
    hpMax = cr::memberStat(g, c, cr::kHp, app.itemTable);
    mpMax = cr::memberStat(g, c, cr::kMp, app.itemTable);
    hp = hpMax;
    mp = pm.mp < 0 ? mpMax : pm.mp; // (the stored number, above the most until it next changes)
}

// MuComMakeItemHaveData (0x51f1c): what the bag holds of a category - weapons / armor (kinds 6 / 7)
// the first member's, or the second's unless held back (flag 0xfed + route). With the second held
// back, MuComMakeItemData leaves the jewel they wear out. 2.2.0's bag counts what is worn: the
// lists and counts take pairHeldCount - the port's spares and what the pair wears - so worn gear is
// listed (yellow, and refused in Equip) as there

std::vector<int> campItems(const App& app, int category) {
    std::vector<int> out;
    int from = 1, to = 60;
    const int kind = campui::kCategoryKind[std::clamp(category, 0, campui::kCategories - 1)];
    switch (kind) {
    case 3: from = 61, to = 80; break;
    case 6: case 7: from = 201, to = 424; break;
    case 8: from = 425, to = 438; break;
    case 10: from = 479, to = 500; break;
    default: break;
    }
    const int route = campRoute(app), bits = (1 << (2 * route)) | (campMembers(app) > 1 ? 2 << (2 * route) : 0);
    for (int n = from; n <= to; ++n) {
        const int count = pairHeldCount(app, n); // (make_item_list: GetItmCnt, worn gear in it)
        if (count <= 0) continue;
        const cr::ItemParam* p = app.itemTable.item(n);
        if (category == 2 && (!p || p->kind != cr::kKindWeapon || !(p->owners & bits))) continue;
        if (category == 3 && (!p || p->kind != cr::kKindArmor || !(p->owners & bits))) continue;
        out.push_back(n);
    }
    return out;
}

// FUN_00055768 (0x55768) for the empty jewel slot (0x1da): 2.2.0 sums the bag's jewels 425..438 - worn
// ones counted - and takes 2 or more, or 1 the other member doesn't wear; the port's bag holds only the
// unworn, so: any jewel in it
bool campFreeJewel(const App& app) {
    for (int n = 425; n <= 438; ++n)
        if (ownedCount(app, n) > 0) return true;
    return false;
}
// MuCom_GetCommonSkill (0x517e8): an ability 0x59..0x71 or a skill 0x8b..0xba known that a plate in the
// bag holds (GetPlateNumFromSkill: the table at 0xc78b0, plates 0x1df..0x1f4)
bool campCommonSkill(const App& app) {
    for (int p = cr::kFirstPlate; p < cr::kFirstPlate + cr::kPlates; ++p) {
        if (ownedCount(app, p) <= 0) continue;
        for (int e : cr::plateContents(p))
            if (((e >= 0x59 && e <= 0x71) || (e >= 0x8b && e <= 0xba)) && ownedCount(app, e) > 0) return true;
    }
    return false;
}

// The plates in the bag, 479.. in id order: the Genes page's list (MuComMakeGenePlate(10, 1))
std::vector<int> campGeneList(const App& app) {
    std::vector<int> out;
    for (int p = cr::kFirstPlate; p < cr::kFirstPlate + cr::kPlates; ++p)
        if (ownedCount(app, p) > 0) out.push_back(p);
    return out;
}
// A plate's known entries (MuComMakeItemData(plate, 0xd): its table's entries the bag counts)
std::vector<int> campKnownEntries(const App& app, int plate) {
    std::vector<int> out;
    if (!cr::isPlate(plate)) return out;
    for (int e : cr::plateContents(plate))
        if (e > 0 && ownedCount(app, e) > 0) out.push_back(e);
    return out;
}
bool campWears(const App& app, int character, int plate) { // MuScrList_IsNowEquip (a plate)
    const cr::PartyMember& pm = app.game.party[size_t(character)];
    return plate > 0 && std::find(std::begin(pm.genes), std::end(pm.genes), plate) != std::end(pm.genes);
}
// MuScrList_GetChoiceListNo's refusal for a plate row (kinds 6 / 0xb): none of its entries known
bool campPlateLocked(const App& app, int item) { return cr::isPlate(item) && cr::plateLearned(app.game, item) == 0; }

// What can go in member c's slot (EquipProc 0xb / 0xf / 0x13 / 0x17 -> __MuCampScrListComInit 0x675e8):
// weapons / armor / jewels (MuComMakeItemData(c, 6 / 7 / 8): the bag's of the kind that c may wear - the
// absent member's worn jewel isn't in the port's bag), the Human Genes slot the bag's 479..480 (kind 0xb), a plate slot
// the bag's 481..500 with a known entry (MuComMakeGenePlate(0xc, ..)). "Remove" (item 200) comes last for
// a jewel or a plate slot that is worn (their flag: the slot empty); __ChangeEquipItem (0x677fc) wears what
// is chosen whoever else wears it
std::vector<int> campEquipList(const App& app, int c, int slot) {
    std::vector<int> out;
    const cr::PartyMember& pm = app.game.party[size_t(c)];
    if (slot >= 3) {
        const int from = slot == 3 ? cr::kFirstPlate : cr::kFirstPlate + 2, to = slot == 3 ? cr::kFirstPlate + 2 : cr::kFirstPlate + cr::kPlates;
        for (int p = from; p < to; ++p) {
            if (ownedCount(app, p) <= 0) continue;
            if (slot > 3 && cr::plateLearned(app.game, p) == 0) continue;
            out.push_back(p);
        }
        if (slot > 3 && pm.genes[slot - 3] > 0) out.push_back(campui::kRemove);
        return out;
    }
    const int kind = slot == cr::kWeapon ? cr::kKindWeapon : slot == cr::kArmor ? cr::kKindArmor : cr::kKindJewel;
    for (int n = 201; n <= 438; ++n) {
        const cr::ItemParam* p = app.itemTable.item(n);
        if (!p || p->kind != kind || pairHeldCount(app, n) <= 0 || !(p->owners & (1 << c))) continue; // (worn ones too, refused)
        out.push_back(n);
    }
    if (slot == cr::kJewel && pm.equip[cr::kJewel] > 0) out.push_back(campui::kRemove);
    return out;
}

// MuCom_CheckExitItem (0x52b8c) = use_item(0, 1) (FUN_0005eeac): the Emergency Exit works while the
// route's exit flag (0x43e, 0x5ce, 0x75e, 0x8ee: 1086 + 400 x route) is clear
bool campExitUsable(const App& app) { return !app.game.flag(1086 + 400 * campRoute(app)); }
// __ChkCampItem (FUN_00056508; its table 0xbc440): the Emergency Exit (1) while it works, the seeds
// 3..6 and the MP items 24..27
bool campUsable(const App& app, int item) {
    if (item == 1) return campExitUsable(app);
    return (item >= 3 && item <= 6) || (item >= 24 && item <= 27);
}

[[maybe_unused]] void campMessage(App& app, const std::string& text) {
    app.camp.message = text;
    app.camp.messageFrames = text.empty() ? 0 : 90;
}

// A page's windows (0 out .. 1 in). The windows' moves - ComUty_WinDispOn / Off - are linear over four
// frames. A page's windows come after the main page's have gone: CampMain 6 takes the main page's off, 7
// and 8 set the page up, and the page's first state puts its own on - the Items list (CampMainPrc 0's 1:
// its 0 waits a frame) three frames after the main page's went, the Genes list (2's 2, after its
// pictures load: 0 FUN_000677a8, 1 MuCom_IsRead) four; Equip's and Status's member plates (window 8)
// come in with CampMain 6 itself. Back: the page's windows go first (campPageOutFrames)
float campEase(const App& app) {
    const App::Camp& c = app.camp;
    if (c.pageOut) return 1 - std::min(1.0f, float(c.pageOut - 1) / 4.0f);
    const int on = c.page == App::Camp::Items ? 3 : c.page == App::Camp::Genes ? 4 : 0;
    return std::clamp(float(c.anim - on) / 4.0f, 0.0f, 1.0f);
}
// Back from a page: the frame the main page's windows go on, counted from the page's going off - the Items
// list's 0xf - 0x11 (WinDispOff, five frames' wait, done), Equip's and Status's the same, then CampMain 9 a
// frame on; the Genes list's 7 (no wait)
int campPageOutFrames(const App& app) { return app.camp.page == App::Camp::Genes ? 3 : 7; }
// The Items page between its states (CampMainPrc 0): the state left takes its windows off (from its frame, or the
// next where it is the Yes / No's 7 or the board's 10), the next puts its own on a frame after - the Yes / No
// (5) after the list (2) or the member choice (4), the list (1) after the Yes / No's No (7) or the board (10),
// the board (8) on the same frame as the Yes / No go
float campPhaseEase(const App& app, App::Camp::Phase ph) {
    const App::Camp& c = app.camp;
    const bool fromAsk = c.prevPhase == App::Camp::Ask || c.prevPhase == App::Camp::Result;
    const int offAt = fromAsk ? 1 : 0;
    const int onAt = c.prevPhase == App::Camp::Ask && c.phase == App::Camp::Result ? 1 : offAt + 1;
    if (ph == c.phase) return c.phaseT >= 100 ? 1.0f : std::clamp(float(c.phaseT - onAt) / 4.0f, 0.0f, 1.0f);
    if (ph == c.prevPhase && c.phaseT < 100) return 1 - std::clamp(float(c.phaseT - offAt) / 4.0f, 0.0f, 1.0f);
    return 0;
}
void campTrackPhase(App::Camp& c) {
    if (c.page != c.shownPage) c.shownPage = c.page, c.shownPhase = c.prevPhase = c.phase, c.phaseT = 100;
    else if (c.phase != c.shownPhase) c.prevPhase = c.shownPhase, c.shownPhase = c.phase, c.phaseT = 0;
}

constexpr App::Camp::Role RTitle = App::Camp::RTitle, RSection = App::Camp::RSection, RBoard = App::Camp::RBoard,
                          RGene = App::Camp::RGene, RList = App::Camp::RList, RPlates = App::Camp::RPlates,
                          RFaces = App::Camp::RFaces, RBar = App::Camp::RBar;
// A state change's windows (App::Camp::Slide): the view left kept to draw it going; its moves set by the caller
// (the original's frames, counted from the input's: 0 the state that took it, 1 the next, ...); input waits
// until `wait` (the states that only count or wait on a window's move)
App::Camp::Slide& campSlide(App& app, int wait, bool ghost = true) {
    App::Camp& c = app.camp;
    App::Camp::Slide& s = c.slide;
    s = App::Camp::Slide();
    s.t = 0, s.wait = wait, s.was = ghost;
    if (ghost) {
        App::Camp::View& v = s.view;
        v.page = c.page, v.phase = c.phase, v.member = c.member, v.slot = c.slot, v.statusView = c.statusView;
        v.category = c.category, v.plate = c.plate, v.listCursor = c.listCursor, v.list = c.list;
        v.scroll = c.scroll, v.swipe = c.swipe;
    }
    c.swipe = 0; // (the new list's: MuScrList_DispOn / DispActive clear d1fa4)
    return s;
}
void campSwapView(App::Camp& c, App::Camp::View& v) { // (to draw the view left, and back)
    std::swap(c.page, v.page), std::swap(c.phase, v.phase), std::swap(c.member, v.member), std::swap(c.slot, v.slot);
    std::swap(c.statusView, v.statusView), std::swap(c.category, v.category), std::swap(c.plate, v.plate);
    std::swap(c.listCursor, v.listCursor), std::swap(c.list, v.list), std::swap(c.scroll, v.scroll);
    std::swap(c.swipe, v.swipe);
}
bool campSliding(const App& app) { return app.camp.slide.t < app.camp.slide.wait; }

float campListX(const App& app) {
    // (the Items and Genes lists' window comes in from 480 to the right - MuScrList_Start(.., 0): WinDispOn(.., 0x1e0))
    const App::Camp::Page p = app.camp.page;
    return campui::kRowX + (p == App::Camp::Items || p == App::Camp::Genes ? 480 * (1 - campEase(app)) : 0);
}
bool campSlotList(const App& app) { return app.camp.page == App::Camp::Equip && app.camp.phase == App::Camp::Slot; }
bool campStatusList(const App& app) { // the Status details' equipment or skill list
    return app.camp.page == App::Camp::Status && app.camp.phase == App::Camp::Slot && app.camp.statusView != 0;
}
bool campScrollable(const App& app) {
    return campSlotList(app) || campStatusList(app) ||
           ((app.camp.page == App::Camp::Items || app.camp.page == App::Camp::Equip || app.camp.page == App::Camp::Genes) &&
            (app.camp.phase == App::Camp::List || app.camp.phase == App::Camp::Entries));
}
int campScrollCount(const App& app) { return campSlotList(app) ? campui::kEquipSlots : int(app.camp.list.size()); }
// ---- MuScrList's rows and touch (2.2.0 MuScrList_Proc 0x569b0, 54610-55160; layout 0 of the table at 0xbc1e0) ----
// The rows sit at 150 + 48 k less the offset; a touch on the drag area moves them pixel for pixel, its lift snaps them
// to a row (20 past one is the next) and a flick runs one to three rows on; the gauge's box sets them where the finger
// is; a tap over or under the line moves them a row, on the line chooses; the pills jump to the other end
namespace scrlist {
namespace {
int most(int n) { return kRow * std::max(0, n - 1); } // (iVar6: 48 (n - 1) - the last row on the line)
// ComUty_PadChkMove's box (448, 68, 32, 208) - inside, not on its edge - only with 3 rows or more (54778-54800); held,
// the rows go to 48 n (y - 76) / 208 (bc228 / bc224) and on to the next row as it lifts; under 5 the top
bool onGauge(float x, float y) { return x > 448 && x < 480 && y > 68 && y < 276; }
void gauge(ScrList& s, float y, int n) {
    const int v = kRow * n * int(std::trunc(y - 76)) / 208;
    if (v < 5) s.off = s.target = 0;
    else s.off = v, s.target = v - v % kRow + kRow;
}
// The lift after a drag (case 1 / 4, 54985-55020): the way added; a flick (the last touch move's way, points per
// touchesMoved - 1.0's x 0.001 against 0.01 / 0.02 / 0.15) runs 1 / 2 / 3 rows more 10 a frame, none from an end pulled
// on; the rows' target the row within 20, else the next down the list
void commit(ScrList& s, int n, int dyO, float vel) {
    if ((s.off <= 0 && dyO < 0) || (s.off >= most(n) && float(dyO) > float(kRow))) vel = 0;
    const int now = s.off + s.drag;
    s.drag = 0;
    if (now > 0 && now < most(n) && vel != 0) {
        const float a = std::fabs(vel);
        if (a >= 10) s.momentum = kRow * (a >= 150 ? 3 : a >= 20 ? 2 : 1);
        s.momDown = vel > 0;
    }
    s.off = now;
    const int r = now % kRow;
    s.target = std::abs(r) < kSnap ? now - r : now + kRow - r;
}
// No touch on the rows (case 0, 57688-57790): 8 a frame toward the target; past an end back to it
void ease(ScrList& s, int n) {
    if (s.momentum != 0) return;
    if (s.off > most(n)) {
        s.off -= kEase;
        if (s.off <= most(n)) s.off = s.target = most(n);
    } else if (s.off >= 0) {
        if (s.target > s.off) s.off = std::min(s.target, s.off + kEase);
        else if (s.target < s.off) s.off = std::max(s.target, s.off - kEase);
    } else {
        s.off += kEase;
        if (s.off >= 0) s.off = s.target = 0;
    }
}
// A flick's frame (57800-57878): 10 on; spent, or past an end, the target the next row on from there
void flick(ScrList& s, int n) {
    if (s.momentum <= 0) return;
    s.momentum -= kFlick;
    s.off += s.momDown ? kFlick : -kFlick;
    if (s.off >= 0 && s.off <= most(n) && s.momentum >= 0) return;
    s.target = s.off - s.off % kRow + (s.momDown ? kRow : -kRow);
    s.momentum = 0;
}
} // namespace

int focusOf(const ScrList& s, int n) {
    const int v = s.shown(); // (d1f4c + d1f50)
    if (v <= 0) return 0;
    if (v >= most(n)) return std::max(0, n - 1);
    return (v + kRow - kSnap) / kRow;
}
bool atRest(const ScrList& s) { return s.momentum <= 0 && s.off == s.target; }
bool rests(const ScrList& s, int key) { return s.key != key || (s.hold == ScrList::None && atRest(s)); }
void settle(ScrList& s, int row) {
    s.off = s.target = kRow * row;
    s.drag = 0, s.momentum = 0;
}
void jump(ScrList& s, int row) { // (the top: one row above it, eased back - 53709-53712)
    s.off = s.target = row ? kRow * row : -kRow;
    s.byKey = false;
}
bool follow(ScrList& s, int& cursor, int n) {
    if (s.byKey) return false;
    const int f = std::clamp(focusOf(s, n), 0, std::max(0, n - 1));
    const bool sound = f != s.focus && s.quiet == 0; // (MuScrList_Draw 55408: __effgo(1) unless d1ee0)
    cursor = s.focus = f;
    return sound;
}
bool ready(ScrList& s, int key, int cursor) {
    if (s.key == key) return false;
    const int quiet = s.quiet;
    s = ScrList();
    s.key = key, s.quiet = quiet, s.focus = cursor;
    settle(s, cursor);
    return true;
}
bool frame(ScrList& s, int key, int& cursor, int n) {
    if (s.quiet > 0) --s.quiet; // (54660)
    if (ready(s, key, cursor)) return false;
    if (s.hold == ScrList::Gauge) return follow(s, cursor, n); // (PadChkMove held: MuScrList_Proc returns there)
    if (s.hold == ScrList::None && cursor != s.focus) {
        // the keys / pad (and the code's moves): the rows 8 a frame after the focus, at once from further than a row; a
        // wrap past an end (the Vita's FUN_8105880a, 64836-64870: a fresh press on the first row up, the last down) lands a
        // row beyond the other end and eases back, as the pills' jumps
        s.byKey = true;
        s.drag = 0, s.momentum = 0;
        s.target = kRow * cursor;
        if (n > 2 && s.focus == 0 && cursor == n - 1) s.off = kRow * n;
        else if (n > 2 && s.focus == n - 1 && cursor == 0) s.off = -kRow;
        else if (std::abs(s.off - s.target) > kRow) s.off = s.target;
        s.focus = cursor;
    }
    if (s.hold == ScrList::None) ease(s, n);
    flick(s, n);
    if (n < 3) s.off = std::clamp(s.off, -kRow, kRow * n); // (5787c-578a4)
    return follow(s, cursor, n);
}
void press(ScrList& s, float x, float y, int n, double t) {
    s.hold = ScrList::None, s.drag = 0; // (a new touch: one lost on the way is dropped)
    if (n >= 3 && s.drag == 0 && onGauge(x, y)) { // (the gauge first: 54778)
        s.hold = ScrList::Gauge, s.byKey = false;
        gauge(s, y, n);
        return;
    }
    if (x > 0 && x < 456 && y > 76 && y < 284) { // MuCom_PadChkScroll(0, 76, 456, 208) (bc22c): its start
        s.hold = ScrList::Rows, s.byKey = false, s.moved = false;
        s.startY = s.lastY = y, s.lastT = t, s.vel = 0;
    }
}
void move(ScrList& s, float x, float y, int n, double t) {
    if (s.hold == ScrList::Gauge) {
        if (onGauge(x, y)) gauge(s, y, n);
        return;
    }
    if (s.hold != ScrList::Rows) return;
    // touchesMoved (2.2.0 0x3b30, 594-608): the way since the last move, up positive (x 0.001 there) - here over a 60th
    // of a second at least, as the iPhone's touches come
    if (y != s.lastY && t - s.lastT >= 1.0 / 60) {
        s.vel = float((s.lastY - y) / ((t - s.lastT) * 60));
        s.lastY = y, s.lastT = t;
    }
    if (std::fabs(y - s.startY) > 6) s.moved = true; // (the port's slop for a tap: the original takes any move)
    // case 3 (55045-55090): the rows follow the finger; pulled toward an end from within three rows of it (the offset
    // the drag started from) by a half / third / quarter, past it a fifth / sixth, not from two rows beyond
    const int dyO = int(std::trunc(s.startY - y)), o = -s.off, last = most(n);
    if (o <= -4 * kRow || dyO >= 0) {
        if (o >= 4 * kRow - last || dyO <= 0) s.drag = dyO;
        else if (const int k = (o + last + 4 * kRow) / kRow; k >= 2) s.drag = -std::min(kRow, int(std::trunc(float(-dyO) / float(8 - k))));
    } else if (const int k = (o - 4 * kRow) / kRow; k < -1)
        s.drag = -std::max(-kRow, int(std::trunc(float(-dyO) / float(k + 8))));
}
bool lift(ScrList& s, float x, float y, int n, double t) {
    if (s.hold == ScrList::Gauge) { // (PadChkMove's 2 as it lifts: set once more)
        if (onGauge(x, y)) gauge(s, y, n);
        s.hold = ScrList::None;
        return false;
    }
    if (s.hold != ScrList::Rows) { // (a press not seen - down and up within a frame: the gauge set there, else a tap)
        if (n >= 3 && s.drag == 0 && onGauge(x, y)) {
            s.byKey = false;
            gauge(s, y, n);
            return false;
        }
        return true;
    }
    move(s, x, y, n, t);
    s.hold = ScrList::None;
    if (!s.moved) {
        s.drag = 0;
        return true;
    }
    commit(s, n, int(std::trunc(s.startY - y)), s.vel);
    return false;
}
int tap(ScrList& s, float x, float y, int n, float rowX) {
    auto box = [&](float by, float bh) { return x >= rowX && x <= rowX + 256 && y >= by && y <= by + bh; }; // (_pad_chk_updw_box)
    const int f = focusOf(s, n);
    s.byKey = false;
    // the pills' boxes (54880-54910, with 3 rows or more): over the first row on the line the end, under the last the top
    if (n >= 3 && f == 0 && box(76, 74)) { jump(s, n); return 1; }
    if (n >= 3 && f == n - 1 && box(198, 74)) { jump(s, 0); return 1; }
    // over / under the line (54950-54970: bc24c / bc25c): the rows 32 (2/3 of a row) on, the lift's snap a row
    if (box(76, 74)) { s.drag = -kRow * 2 / 3; commit(s, n, 0, 0); return 1; }
    if (box(198, 74)) { s.drag = kRow * 2 / 3; commit(s, n, 0, 0); return 1; }
    // the line (bc1f0: (192, 150, 256, 48), 55140-55160): chosen with the rows at rest (+0x50 == +0x58, no flick)
    if (box(150, 48)) {
        if (!atRest(s)) return 3;
        s.quiet = 4; // (__ScrListSelectSePlay: d1ee0)
        return 2;
    }
    return 0;
}
} // namespace scrlist

// MuScrList (mode 0): the rows move under a fixed line - the focus row is the one on it. Keys and the wheel move the
// focus a row (the rows ease after it: scrlist::frame); the touch moves the rows and the focus follows them
void campScrollBy(App& app, int rows) {
    if (!campScrollable(app)) return;
    auto& c = app.camp;
    int& cursor = campSlotList(app) ? c.slot : c.listCursor;
    const int n = campScrollCount(app), before = cursor;
    cursor = std::clamp(cursor + rows, 0, std::max(0, n - 1));
    int& top = campSlotList(app) ? c.slotTop : c.listTop; // (kept for the Status lists' memory)
    top = cursor;
    if (before != cursor) menuSound(app, se::kListFocus); // (MuScrList_Draw: __effgo(1) as the focus row changes)
}
int campFocus(const App& app) { return campSlotList(app) ? app.camp.slot : app.camp.listCursor; }
// Which list is up: a new one (a page, a phase, a category, a member, a Status view) starts settled
int campScrollKey(const App& app) {
    const App::Camp& c = app.camp;
    return int(c.page) * 4096 + int(c.phase) * 256 + c.statusView * 64 + c.category * 8 + c.member;
}
// The rows' offset now (scrlist::): 48 x the focus row once settled; a list not yet stepped settled on its focus
float campScrollPx(const App& app) {
    return app.camp.scroll.key == campScrollKey(app) ? float(app.camp.scroll.shown()) : campui::kRowH * float(campFocus(app));
}
int& campCursor(App& app) { return campSlotList(app) ? app.camp.slot : app.camp.listCursor; }
// The focus row brought by the rows (a drag, a flick, the gauge, a tap): SE 1 (MuScrList_Draw), the Status lists' memory
void campFollowed(App& app, bool sound) {
    if (campScrollable(app)) (campSlotList(app) ? app.camp.slotTop : app.camp.listTop) = campFocus(app);
    if (sound) menuSound(app, se::kListFocus);
}
void stepCampScroll(App& app) {
    campFollowed(app, scrlist::frame(app.camp.scroll, campScrollKey(app), campCursor(app), campScrollCount(app)));
}

int campEntries(const App& app) { return app.renderer.phoneLayout() ? campui::kEntries - 1 : campui::kEntries; }
float campBackY(const App& app) { return app.renderer.phoneLayout() ? campui::kPhoneBackY : campui::kBackY; }
void campEquipRefill(App& app);
void openCamp(App& app) {
    App::Camp& c = app.camp;
    app.campOpen = true;
    app.campPointer = App::CampPointer();
    auto art = std::move(c.plateArt); // the plates' pictures stay loaded
    auto portraits = std::move(c.statusArt);
    c = App::Camp();
    c.plateArt = std::move(art);
    c.statusArt = std::move(portraits);
    c.cursor = 0; // (the pad's: the Vita's CampMainSet leaves DAT_81113c48, which the pages' member choices reset to 0 -
                  // Items lit as it opens, video 16 1137.5 s; 2.2.0 iOS has no key cursor, DAT_000d2e10 = -1)
    campEquipRefill(app); // (CampMainSet: opened on a home map, the pair healed - pcparam_hpmp_recovery)
    capPairMp(app);       // (63012: ComUty_CharInfoInit)
    // __menuu_CampMainSet (0x635e8): the first time an ability 0x59..0x71 or a skill 0x8b..0xba is in the
    // bag (flag 0x38), message 56010 ("Gene Plates") comes before the Camp's windows (CampMain 0-2, then 3)
    if (!app.game.flag(0x38))
        for (int item = 0x59; item < 0xbb; ++item)
            if ((item < 0x72 || item >= 0x8b) && ownedCount(app, item) > 0) {
                app.game.setFlag(0x38, true);
                campNotice(app, 56010);
                break;
            }
}

// The Camp's once-only messages (MsgUty_WinOpen / SetMsg, block 5): the field's message window
// over the Camp, its windows and the mask down until the message is read
void campNotice(App& app, int id) {
    app.camp.notice = id;
    menuSound(app, se::kWinOpen); // (_MsgUty_WinOpen 76973: SE 6; its close 7, 77165)
    showMessages(app, {id});
}

// The Status details' lists (CampMainPrc 3's state 2): the seven the member wears
// (__MuCampScrListEquipInit(.., 0xc, 0): an empty jewel slot 0x1da, empty plate slots 0x1dc..),
// and every known entry of the four plates worn, each once
std::vector<int> campStatusItems(const App& app, int view) {
    std::vector<int> out;
    const cr::PartyMember& pm = app.game.party[size_t(campCharacter(app, app.camp.member))];
    if (view == 1) {
        for (int slot = 0; slot < campui::kEquipSlots; ++slot) {
            const int worn = slot < 3 ? pm.equip[slot] : pm.genes[slot - 3];
            out.push_back(worn > 0 ? worn : slot == 2 ? 0x1da : slot >= 3 ? 0x1db + (slot - 3) : 0);
        }
    } else if (view == 2) {
        for (int g : pm.genes) {
            if (g <= 0) continue;
            for (int e : cr::plateContents(g))
                if (e > 0 && ownedCount(app, e) > 0 && std::find(out.begin(), out.end(), e) == out.end()) out.push_back(e);
        }
    }
    return out;
}
// -2 (the right arrow, a swipe to the left) board -> equipment -> skills -> board; -3 the other way
// (SE 5 either way; the lists keep where they were)
// CampMainPrc 3's 6 / 9 / 0xc (the frame after the arrow or swipe): the view's windows out toward the arrow - the
// board (10) to 240, a list (0xe / 0xd) to 480 with its line (0x10 / 0xf) to -140 -, and the next frame its 7 / 10 /
// 4 put the new view's on from the other side (d301c: where it came from); input again the frame after (5 / 8 / 0xb)
void campStatusTurn(App& app, int dir) {
    App::Camp& c = app.camp;
    menuSound(app, se::kPage);
    App::Camp::Slide& s = campSlide(app, 3);
    const float way = dir > 0 ? -1.0f : 1.0f; // (-2, the right arrow or a swipe to the left: out to the left)
    if (c.statusView == 0) s.out[RBoard] = {1, 240 * way};
    else s.out[RList] = {1, 480 * way}, s.out[RSection] = {1, -140};
    c.viewTop[c.statusView] = c.listCursor;
    c.statusView = (c.statusView + dir + 3) % 3;
    c.list = campStatusItems(app, c.statusView);
    c.listCursor = c.listTop = std::clamp(c.viewTop[c.statusView], 0, std::max(0, int(c.list.size()) - 1));
    if (c.statusView == 0) s.in[RBoard] = {2, -240 * way};
    else s.in[RList] = {2, -480 * way}, s.in[RSection] = {2, -140};
}

void campRefreshList(App& app) {
    App::Camp& c = app.camp;
    if (c.phase == App::Camp::Entries) c.list = campKnownEntries(app, c.plate);
    else if (c.page == App::Camp::Items) c.list = campItems(app, c.category);
    else if (c.page == App::Camp::Equip) c.list = campEquipList(app, campCharacter(app, c.member), c.slot);
    else if (c.page == App::Camp::Genes) c.list = campGeneList(app);
    c.listCursor = std::clamp(c.listCursor, 0, std::max(0, int(c.list.size()) - 1));
    c.listTop = std::clamp(c.listTop, std::max(0, c.listCursor - campui::kRows + 1), c.listCursor);
}

// Items: the next category that holds anything, `dir` from the current (the current one too if 0)
// (__MuCampScrListItemInit 0x67154: its six, skipping the empty ones)
void campNextCategory(App& app, int dir) {
    App::Camp& c = app.camp;
    const int n = campui::kCategories;
    for (int k = dir ? 1 : 0; k <= n; ++k) {
        const int cat = ((c.category + dir * k) % n + n) % n;
        if (!campItems(app, cat).empty() || k == n) {
            c.category = cat;
            break;
        }
    }
    c.listCursor = c.listTop = 0;
    campRefreshList(app);
}
// CampMainPrc 0's 2 on -3 / -2 (63408-63428): the list out the arrow's way and the next category's (ItemInit's
// other window, 0xe / 0xf) in from the other side that frame (MuScrList_DispActive: -3 out to 480, in from -480;
// -2 the other way), the category's line swapping windows 0x10 / 0x11 from -140 (FUN_00066dac(.., 0)); SE 5
void campTurnCategory(App& app, int dir) {
    App::Camp::Slide& s = campSlide(app, 1);
    const float way = dir > 0 ? -1.0f : 1.0f;
    s.out[RList] = {0, 480 * way}, s.out[RSection] = {0, -140};
    s.in[RList] = {0, -480 * way}, s.in[RSection] = {0, -140};
    campNextCategory(app, dir);
    menuSound(app, se::kPage);
}

// CampMainPrc 8's state 1 (63391-63400): __MuCampScrListItemInit(0) - the list made again from the top, its
// category kept (the next one holding anything when it's empty); a use, a No and a Back all come here
void campItemsAgain(App& app) {
    App::Camp& c = app.camp;
    c.phase = App::Camp::List;
    c.listCursor = c.listTop = 0;
    campRefreshList(app);
    if (c.list.empty()) campNextCategory(app, 0);
}

void campOpenEntries(App& app, int plate) {
    App::Camp& c = app.camp;
    c.plate = plate;
    c.plateCursor = c.listCursor, c.plateTop = c.listTop;
    c.phase = App::Camp::Entries;
    c.listCursor = c.listTop = 0;
    campRefreshList(app);
}
void campCloseEntries(App& app) {
    App::Camp& c = app.camp;
    c.plate = 0;
    c.phase = App::Camp::List;
    c.listCursor = c.plateCursor, c.listTop = c.plateTop;
    campRefreshList(app);
}

void campOpenPage(App& app, int i, bool sound) {
    App::Camp& c = app.camp;
    c.cursor = i;
    if (i < 5 && !campui::kReady[i]) {
        menuSound(app, se::kBuzzer);
        return;
    }
    if (sound) menuSound(app, se::kChoose);
    c.anim = 0;
    c.member = 0;
    c.statusView = 0;
    c.lastSlot = 0;
    c.slide = App::Camp::Slide();
    c.swipe = 0;
    c.headIn = false;
    c.message.clear();
    switch (i) {
    case 0:
        c.page = App::Camp::Items;
        c.phase = App::Camp::List;
        c.category = 0;
        campNextCategory(app, 0);
        break;
    case 1: c.page = App::Camp::Equip; c.phase = App::Camp::Member; break;
    case 2: // Genes: the plates in the bag, the board showing the cursor's
        c.page = App::Camp::Genes;
        c.phase = App::Camp::List;
        c.listCursor = c.listTop = 0;
        campRefreshList(app);
        break;
    case 3: c.page = App::Camp::Status; c.phase = App::Camp::Member; break;
    case 4: openOptions(app, 0), c.optionsUp = true; break;
    default: app.campOpen = false; openSaveScreen(app, false); break; // Save
    }
}

// The Camp busy: a once-only message up, the Emergency Exit under way, the page's windows going
// (or Options on its way, or a state change's windows still before the state that takes input - campSliding)
bool campBusy(const App& app) {
    return app.camp.notice != 0 || app.camp.phase == App::Camp::Exit || app.camp.pageOut != 0 || app.camp.toOptions != 0 ||
           campSliding(app);
}

void campBack(App& app) {
    App::Camp& c = app.camp;
    if (campBusy(app) || c.leaving) return;
    // (no Back bar during the Yes / No - window 4 is off, CampMainPrc 4 -> 5 -, but the Vita's × answers No
    // there: __ComUty_YesNoChoiceProc's 1, then CampMainPrc 6's No - SE 2 and the list again)
    if (c.page == App::Camp::Items && c.phase == App::Camp::Ask) { c.confirm = 1; campChoose(app); return; }
    if (c.page == App::Camp::Items && c.phase == App::Camp::Result) { campChoose(app); return; } // (any tap: CampMainPrc 9)
    if (c.page != App::Camp::Main) menuSound(app, se::kBack); // (the main page's Back - its box or the Vita's × - 9 alone)
    c.message.clear();
    switch (c.page) {
    case App::Camp::Main: // (CampMain 5 -> 0xd: every window off, the Camp closed four frames on - 0xe, 0x10;
        // the Back box's SE 9 - its only one, as for the Vita's ×)
        c.leaving = 1, menuSound(app, se::kCampClose);
        return;
    case App::Camp::Items:
        if (c.phase == App::Camp::Member) campItemsAgain(app); // (CampMainPrc 4's -1: the list again, from the top)
        else c.pageOut = 1;
        return;
    case App::Camp::Equip:
        if (c.phase == App::Camp::Entries) { // 0x1d's -1: the plate list again - that frame ">Genes" registered again
            // (window 0x10 off and on: in from -140), the entries out to 480, the plate list back from -480
            App::Camp::Slide& s = campSlide(app, 1);
            s.out[RList] = {0, 480}, s.in[RList] = {0, -480}, s.in[RSection] = {0, -140};
            campCloseEntries(app);
            return;
        }
        if (c.phase == App::Camp::List) { // 0xc / 0x10 / 0x14 / 0x19's -1: the list out to 480 and the slot list back
            // from -480 that frame; the next (0xe / 0x12 / 0x16 / 0x1b) takes the line off - and a plate slot's puts the
            // state board back from -240 and the gene board off to -240
            App::Camp::Slide& s = campSlide(app, 2);
            s.out[RList] = {0, 480}, s.in[RList] = {0, -480}, s.out[RSection] = {1, -140};
            if (c.slot >= 3) s.out[RGene] = {1, -240}, s.in[RBoard] = {1, -240};
            c.phase = App::Camp::Slot;
            return;
        }
        if (c.phase == App::Camp::Slot && !c.direct) { // 4's -1 -> 5 the next frame: __MuEquip_CategoryChoiceEnd (the
            // title to -140, the state board to -240, the slot list to 480) and the list's Back bar off (0xd, 0xc); 6
            // counts 4 .. -1, then the face boards (from -240), Back and the members' plates (from 360) on; 0 / 1 the choice
            App::Camp::Slide& s = campSlide(app, 8);
            s.out[RTitle] = {1, -140}, s.out[RBoard] = {1, -240}, s.out[RList] = {1, 480}, s.out[RBar] = {1, 0};
            s.in[RFaces] = {6, -240}, s.in[RBar] = {6, 0}, s.in[RPlates] = {6, 360};
            c.phase = App::Camp::Member;
            return;
        }
        c.pageOut = 1;
        return;
    case App::Camp::Status:
        if (c.phase == App::Camp::Slot) { // Slot = the details: any view's -1 is the member choice. CampMainPrc 3's 6 / 9 /
            // 0xc the next frame: the board out to 240 or the list to 480 with its line, and __MuState_ReturnCharChoice -
            // the title to -140, the portrait (0xb) to -240, the Back bar off and on again at once (its arrows gone), the
            // face boards and the members' plates in from -240 / 360; 0 / 1 the choice
            App::Camp::Slide& s = campSlide(app, 3);
            if (c.statusView == 0) s.out[RBoard] = {1, 240};
            else s.out[RList] = {1, 480}, s.out[RSection] = {1, -140};
            s.out[RTitle] = {1, -140}, s.out[RGene] = {1, -240}, s.out[RBar] = {1, 0, true};
            s.in[RBar] = {1, 0}, s.in[RFaces] = {1, -240}, s.in[RPlates] = {1, 360};
            c.phase = App::Camp::Member;
            c.statusView = 0;
            return;
        }
        c.pageOut = 1;
        return;
    case App::Camp::Genes:
        if (c.phase == App::Camp::Entries) { // Genes 5 -> 6 the next frame: ">Gene Details" off, the entries out to 480,
            // the plate list back from -480; 3 the frame after
            App::Camp::Slide& s = campSlide(app, 2);
            s.out[RSection] = {1, -140}, s.out[RList] = {1, 480}, s.in[RList] = {1, -480};
            campCloseEntries(app);
            return;
        }
        c.pageOut = 1;
        return;
    }
}

// Items: use the chosen item on member c (use_item, FUN_0005eeac); false when it does nothing: a seed's
// stat bonus at 5000 already (each seed used in turn: a stat that fills up part way stops it there),
// an MP item on full MP
bool campUseItem(App& app, int character, int item, int count) {
    cr::PartyMember& pm = app.game.party[size_t(character)];
    if (ownedCount(app, item) < count || count < 1) return false;
    if (item >= 3 && item <= 6) {
        int& points = pm.seeds[item - 3];
        count = std::min(count, cr::seedRoom(pm, cr::Stat(cr::kAttack + item - 3)));
        if (count <= 0) return false;
        points += count;
    } else if (item >= 24 && item <= 27) {
        const int most = cr::memberStat(app.game, character, cr::kMp, app.itemTable);
        // (FUN_0005eddc 59852-59886: refused only at exactly the most; above it, added and then clamped)
        const int mp = pm.mp < 0 ? most : pm.mp;
        if (mp == most) return false;
        static const int kGain[3] = {100, 250, 500};
        pm.mp = item == 27 ? most : std::min(most, mp + kGain[item - 24]);
        count = 1;
    } else
        return false;
    if ((app.game.items[item] -= count) <= 0) app.game.items.erase(item);
    return true;
}

// CampMainPrc 5: how many seeds the count goes to - MuCom_GetUsedItemLimitNum or the bag's, the fewer,
// kept within 1..999
int campSeedLimit(const App& app, int character, int item) {
    const cr::PartyMember& pm = app.game.party[size_t(character)];
    return std::clamp(std::min(ownedCount(app, item), cr::seedRoom(pm, cr::Stat(cr::kAttack + item - 3))), 1, 999);
}

// __menuu_CampMainSet: on the home maps 0x61..0x71 and 0x53 (DAT_000d2e30) the Camp's opening and
// whatever __ChangeEquipItem wears refill the pair's HP and MP (pcparam_hpmp_recovery)
void campEquipRefill(App& app) {
    if ((app.mapNumber >= 0x61 && app.mapNumber <= 0x71) || app.mapNumber == 0x53) ViewerHost(app).recoverPair();
}

void campChoose(App& app) {
    App::Camp& c = app.camp;
    if (campBusy(app) || c.leaving) return;
    const int item = c.list.empty() ? -1 : c.list[size_t(c.listCursor)];
    switch (c.page) {
    case App::Camp::Main:
        if (c.cursor == 0 && !app.game.flag(0x39)) { // CampMain 5: the first Items tap (flag 0x39) - the windows
            menuSound(app, se::kChoose);              // go, message 56016, then the Items page (CampMain 2 -> 6)
            app.game.setFlag(0x39, true);
            c.anim = 0, c.mainGone = true; // (windows 1 - 5 off that frame, "Camp" (0) left on, the mask cleared)
            campNotice(app, 56016);
            return;
        }
        if (c.cursor == 4 && campui::kReady[4]) { // Options: CampMain 6 takes every window off the next frame ("Camp" too),
            menuSound(app, se::kChoose);          // 7 opens Options the frame after (FUN_00071430(2))
            c.toOptions = 1;
            return;
        }
        campOpenPage(app, c.cursor);
        return;
    case App::Camp::Items:
        if (c.phase == App::Camp::List) {
            if (item <= 0 || !campUsable(app, item)) { menuSound(app, se::kBuzzer); return; }
            menuSound(app, se::kChoose);
            if (item == 1) { // the Emergency Exit: straight to its Yes / No (CampMainPrc 2 -> 5, text 57014)
                c.phase = App::Camp::Ask;
                c.count = 1, c.confirm = 0;
            } else c.phase = App::Camp::Member;
        } else if (c.phase == App::Camp::Member) { // (CampMainPrc 4 -> 5: no member refused here)
            menuSound(app, se::kChoose);
            c.phase = App::Camp::Ask;
            c.count = 1;
            c.confirm = 0;
        } else if (c.phase == App::Camp::Ask) {
            if (c.confirm != 0) { // No: back to the list (CampMainPrc 7 -> 1), SE 2
                menuSound(app, se::kChoose);
                campItemsAgain(app);
                return;
            }
            if (item == 1) { // Yes to the Emergency Exit: DAT_000d2f18 / d2e32, del_item, SE 2, state 7 -> 0xf
                if (!campExitUsable(app) || ownedCount(app, 1) <= 0) { menuSound(app, se::kBuzzer); return; }
                if (--app.game.items[1] <= 0) app.game.items.erase(1);
                menuSound(app, se::kChoose);
                c.phase = App::Camp::Exit;
                c.exitT = 0;
                return;
            }
            if (!campUseItem(app, campCharacter(app, c.member), item, c.count)) { menuSound(app, se::kBuzzer); return; } // (SE 4, the Yes / No up)
            capPairMp(app); // (63558)
            menuSound(app, campui::kSoundUse); // 0x14, then the board (window 0x12) until a tap (CampMainPrc 8, 9)
            c.resultItem = item;
            c.resultCount = item >= 3 && item <= 6 ? c.count : 0;
            c.phase = App::Camp::Result;
        } else if (c.phase == App::Camp::Result) { // the tap (SE 2): the list again (10 -> 1)
            menuSound(app, se::kChoose);
            campItemsAgain(app); // (video 08 1479.8 s: back on Emergency Exit after a seed at row 2)
        }
        return;
    case App::Camp::Equip: {
        cr::PartyMember& pm = app.game.party[size_t(campCharacter(app, c.member))];
        if (c.phase == App::Camp::Member) {
            // EquipProc 1: the face boards (2) to -240, the members' plates (8) to 360, Back (4) off; 3 counts 4 .. -1
            // and __MuEquip_CategoryChoiceInit puts the title (9, from -140), the state board (10, from -240) and the
            // slot list (0xe, from 480, its Back bar with it) on five frames on; 4 takes input the frame after
            menuSound(app, se::kChoose);
            App::Camp::Slide& s = campSlide(app, 6);
            s.out[RFaces] = {0, -240}, s.out[RPlates] = {0, 360}, s.out[RBar] = {0, 0};
            s.in[RTitle] = {5, -140}, s.in[RBoard] = {5, -240}, s.in[RList] = {5, 480}, s.in[RBar] = {5, 0};
            c.phase = App::Camp::Slot;
            c.slot = 0, c.slotTop = 0, c.lastSlot = 0; // (d2fac = 0)
        } else if (c.phase == App::Camp::Slot) {
            if (c.slot == cr::kJewel && campJewelSlotRefused(app)) { menuSound(app, se::kBuzzer); return; } // (kind 4: -4)
            c.listCursor = c.listTop = 0;
            campRefreshList(app);
            if (c.list.empty()) { menuSound(app, se::kBuzzer); return; } // nothing to wear
            // (__MuCampScrListComInit 64925-64958: MuScrList_SetListPosFromItemNo(the worn one) - on it, not the top)
            const int worn = c.slot >= 3 ? pm.genes[size_t(c.slot - 3)] : pm.equip[c.slot];
            if (worn > 0)
                if (auto at = std::find(c.list.begin(), c.list.end(), worn); at != c.list.end())
                    c.listCursor = c.listTop = int(at - c.list.begin());
            menuSound(app, se::kChoose);
            // EquipProc 4: the slot list out to -480; 0xb / 0xf / 0x13 / 0x17 the next frame: the line (0x10) from -140,
            // the list (0xf) from 480 - a plate slot's 0x17 takes the state board (10) off to -240 and its 0x18, the
            // pictures read, puts the gene board (0xb) on from -240 -; input from the next state on
            App::Camp::Slide& s = campSlide(app, c.slot >= 3 ? 3 : 2);
            s.out[RList] = {0, -480}, s.in[RSection] = {1, -140}, s.in[RList] = {1, 480};
            if (c.slot >= 3) s.out[RBoard] = {1, -240}, s.in[RGene] = {2, -240};
            c.lastSlot = c.slot; // (d2fac)
            c.phase = App::Camp::List;
        } else if (c.phase == App::Camp::List && !c.list.empty() && c.slot >= 3) { // a gene plate
            if (item == campui::kRemove) { // EquipProc 0x19: Remove acts at once (__ChangeEquipItem 200) - that frame
                // the plate list out to 480, ">Genes" off, the slot list in from 480, the state board back from -240, the
                // gene board off to -240
                App::Camp::Slide& s = campSlide(app, 1);
                s.out[RList] = {0, 480}, s.out[RSection] = {0, -140}, s.out[RGene] = {0, -240};
                s.in[RList] = {0, 480}, s.in[RBoard] = {0, -240};
                pm.genes[c.slot - 3] = 0;
                menuSound(app, campui::kSoundEquip);
                capPairMp(app); // (65036)
                c.phase = App::Camp::Slot;
            } else if (campPlateLocked(app, item)) menuSound(app, se::kBuzzer);
            else { // SE 2, then the plate's known entries (0x1a) with the board in picture mode: the plate list out to
                // -480 that frame; 0x1a the next: ">Genes" off, the entries (0x13) in from 480; 0x1c: ">Gene Details"
                menuSound(app, se::kChoose);
                App::Camp::Slide& s = campSlide(app, 3);
                s.out[RList] = {0, -480}, s.out[RSection] = {1, -140}, s.in[RList] = {1, 480}, s.in[RSection] = {2, -140};
                campOpenEntries(app, item);
            }
        } else if (c.phase == App::Camp::Entries) { // 0x1d: OK (-5) - none while the member wears it
            if (campWears(app, campCharacter(app, c.member), c.plate)) { menuSound(app, se::kBuzzer); return; }
            // 0x1f the next frame: ">Gene Details", the gene board (to -240) and the entries (to -480) off; 0x1e counts
            // 4 .. -1, then the state board (from -240) and the slot list (from 480) on; 4 the frame after
            App::Camp::Slide& s = campSlide(app, 7);
            s.out[RSection] = {1, -140}, s.out[RGene] = {1, -240}, s.out[RList] = {1, -480};
            s.in[RBoard] = {6, -240}, s.in[RList] = {6, 480};
            pm.genes[c.slot - 3] = c.plate;
            menuSound(app, campui::kSoundEquip);
            campEquipRefill(app);
            capPairMp(app); // (65036)
            c.plate = 0;
            c.phase = App::Camp::Slot;
        } else if (c.phase == App::Camp::List && !c.list.empty()) {
            // (kind 5: what the member wears already is refused, MuScrList_IsNowEquip -4)
            if (item != campui::kRemove && pm.equip[c.slot] == item) { menuSound(app, se::kBuzzer); return; }
            if (campJewelRefused(app, item)) { menuSound(app, se::kBuzzer); return; } // (the partner's only one)
            // (0xc / 0x10 / 0x14: the list out to -480; 0xd / 0x11 / 0x15 the next frame: the line off, the slot list
            // made again and in from 480; 4 the frame after)
            App::Camp::Slide& s = campSlide(app, 2);
            s.out[RList] = {0, -480}, s.out[RSection] = {1, -140}, s.in[RList] = {1, 480};
            int& worn = pm.equip[c.slot];
            const int wear = item == campui::kRemove ? 0 : item;
            if (worn > 0) app.game.items[worn] = std::min(cr::kMaxItemCount, ownedCount(app, worn) + 1); // (the port's bag
            if (wear > 0 && ownedCount(app, wear) <= 0) {                                                // holds what isn't worn;
                for (int g = 0; g < 2; ++g) {                                                             // with no spare, the one the
                    int& other = app.game.party[size_t(campCharacter(app, g))].equip[c.slot];             // partner wears changes hands -
                    if (&other != &worn && other == wear) { other = 0; app.game.items[wear] = 1; break; } // __ChangeEquipItem: whoever
                }                                                                                         // else wears it)
            }
            if (wear > 0 && --app.game.items[wear] <= 0) app.game.items.erase(wear);
            worn = wear;
            menuSound(app, campui::kSoundEquip);
            if (wear > 0) campEquipRefill(app);
            capPairMp(app); // (__ChangeEquipItem -> ComUty_CharInfoInit, 65036)
            c.phase = App::Camp::Slot;
        }
        return;
    }
    case App::Camp::Status:
        if (c.phase == App::Camp::Member) { // CampMainPrc 3's 2: the board first, the lists made
            menuSound(app, se::kChoose);
            // (2, the next frame: the face boards to -240, the members' plates to 360, the Back bar off and on again at
            // once - its arrows -, the title from -140; 3, the portraits read: the portrait (0xb) from -240; 4: the board
            // (10) from 240; 5 takes input)
            App::Camp::Slide& s = campSlide(app, 4);
            s.out[RFaces] = {1, -240}, s.out[RPlates] = {1, 360}, s.out[RBar] = {1, 0, true};
            s.in[RBar] = {1, 0}, s.in[RTitle] = {1, -140}, s.in[RGene] = {2, -240}, s.in[RBoard] = {3, 240};
            c.phase = App::Camp::Slot;
            c.statusView = 0;
            c.viewTop[0] = c.viewTop[1] = c.viewTop[2] = 0;
            c.list.clear();
            c.listCursor = c.listTop = 0;
        }
        return;
    case App::Camp::Genes: // browsing: a plate opens its known entries (Genes 3 -> 4), a plate with none is refused (kind 0xb)
        if (c.phase == App::Camp::List && !c.list.empty()) {
            if (campPlateLocked(app, item)) { menuSound(app, se::kBuzzer); return; }
            menuSound(app, se::kChoose);
            // (Genes 4 the next frame: the plate list out to -480, ">Gene Details" from -140, the entries from 480)
            App::Camp::Slide& s = campSlide(app, 2);
            s.out[RList] = {1, -480}, s.in[RSection] = {1, -140}, s.in[RList] = {1, 480};
            campOpenEntries(app, item);
        } else if (c.phase == App::Camp::Entries) menuSound(app, se::kChoose); // (the list's own SE; Back alone leaves it)
        return;
    }
}

// CampMain 5 -> 0xd -> 0xe -> 0x10 (64160-64205): 0xd, the next frame, takes every window off and clears the mask
// (MuCom_MenuClrMask); 0xe counts 4 .. -1, then 0x10 (SetMenuStat(6, 4)) closes the Camp eight frames after the
// tap. `leaving` is 1 + the frames since
void stepCampLeaving(App& app) {
    App::Camp& c = app.camp;
    if (!c.leaving || ++c.leaving <= 8) return;
    c.leaving = 0;
    c.anim = 0;
    app.campOpen = false;
}

// The Emergency Exit's frames after Yes: CampMainPrc 7 takes the Yes / No and its board off (the list went
// when the Exit was chosen), 0xf - 0x12 wait; CampMain 9 puts the clock and purse (window 1) on and 0xd, the
// next frame, takes every window off and clears the mask. A window taken off while it is coming on is whole
// the next frame (ClrWin: state 4 -> 5): the clock flashes in whole at 11, then fades with the "Camp" heading,
// gone at 15. 0xe four frames on fades to white over 500 ms with sound 0x1f and bgmgo(0x8000000), and 12
// frames later the Camp closes (0xf, 0x10) - __menuu_CampMainClr (0x63750): pcparam_hpmp_recovery, then the
// route's home (area 7 + 6 x route, map 0x61 - 0x69 for route 2 once flag 0x759 is set, route 3 once 0x8e9
// is) at (0, 7.5, 15) facing 0, load flags 0x26: the area's script 0 runs (InitFieldScene) and no room script
constexpr int kCampExitHide = 10, kCampExitGone = 15, kCampExitFade = 16, kCampExitWarp = 30;
void campExitWarp(App& app) {
    App::Camp& c = app.camp;
    app.campOpen = false;
    app.campPointer = App::CampPointer();
    c.phase = App::Camp::List;
    c.page = App::Camp::Main;
    c.exitT = 0;
    ViewerHost(app).recoverPair();
    const int route = campRoute(app);
    cr::WarpRequest w;
    w.valid = true;
    w.area = 7 + 6 * route;
    w.map = (route == 2 && app.game.flag(0x759)) || (route == 3 && app.game.flag(0x8e9)) ? 0x69 : 0x61;
    w.pos[0] = 0, w.pos[1] = 7.5f, w.pos[2] = 15;
    w.angleDegrees = 0;
    performWarp(app, w, -1); // (its field fades in from the white)
    app.lastEvent = "Emergency Exit: back home";
}

// A Camp frame (20 a second): the windows' moves, a once-only message read, the Emergency Exit's frames
void stepCamp(App& app) {
    App::Camp& c = app.camp;
    if (!app.campOpen) return;
    if (c.optionsUp && !app.optionsOpen) { // back from Options: CampMain 9 puts "Camp", the boards, the plates, Back and
        c.optionsUp = false;               // the clock on again (64131-64150) - their four frames' moves from here
        c.anim = -1, c.barIn = c.headIn = true;
    }
    ++c.anim; // (a game frame: the windows' moves)
    campTrackPhase(c);
    if (c.phaseT < 100) ++c.phaseT;
    if (c.slide.t < 100) ++c.slide.t;
    if (!app.campPointer.sideways) // (a list let go: MuScrList_Proc brings it back 10 a frame)
        c.swipe = c.swipe > 0 ? std::max(0.0f, c.swipe - 10) : std::min(0.0f, c.swipe + 10);
    if (c.leaving) { stepCampLeaving(app); return; } // (Back on the main page: the windows going)
    if (c.toOptions && ++c.toOptions - 1 >= 2) { // (CampMain 7, two frames after the tap: Options up)
        c.toOptions = 0;
        campOpenPage(app, 4, false);
        return;
    }
    if (c.pageOut && ++c.pageOut - 1 >= campPageOutFrames(app)) { // the page's windows gone: the main page's on (CampMain 9)
        c.pageOut = 0;
        c.barIn = c.page == App::Camp::Items;
        c.page = App::Camp::Main;
        c.phase = App::Camp::List;
        c.direct = false;
        c.mainGone = false;
        c.slide = App::Camp::Slide();
        c.anim = 0;
        campTrackPhase(c);
    }
    stepCampScroll(app);
    ++c.mark;
    if (c.notice && app.dialogue.empty()) { // the message closed: on to what was asked (CampMain 2)
        const int id = c.notice;
        c.notice = 0;
        menuSound(app, se::kWinClose); // (_MsgUty_WinClose 77165)
        c.anim = 0;
        if (id == 56016) campOpenPage(app, 0, false);
    }
    if (c.phase != App::Camp::Exit) return;
    ++c.exitT;
    if (c.exitT == kCampExitFade) {
        app.fadeGrey = 1, app.fadeFrom = 0, app.fadeTo = 1, app.fadeFrames = 10, app.fadeT = 0; // SetFade(white, 0, 255, 500 ms)
        menuSound(app, 0x1f);
        playBgm(app, 0x8000000u);
    }
    if (c.exitT >= kCampExitWarp) campExitWarp(app);
}
// EquipProc 4 on -3 / -2 -> 7, the next frame: __MuEquip_CategoryChoiceEnd - the title to -140, the state board and
// the slot list out the arrow's way (-3: to 240 / 480, -2: to -240 / -480); 8 waits for the list's move to start,
// 9 (frame 4) takes the other member and CategoryChoiceInit puts them on from the other side (the board from
// -240 / 240, the list from -480 / 480 - MuScrList_Start(.., d2fbc == -3)), the list on the slot last chosen on the
// page (d2fac, FUN_00055c6c) or the first; 10 waits on its move, 4 takes input the frame after
void campSwitchMember(App& app, int dir) {
    App::Camp& c = app.camp;
    App::Camp::Slide& s = campSlide(app, 6);
    const float way = dir > 0 ? -1.0f : 1.0f;
    s.out[RTitle] = {1, -140}, s.out[RBoard] = {1, 240 * way}, s.out[RList] = {1, 480 * way};
    s.in[RTitle] = {4, -140}, s.in[RBoard] = {4, -240 * way}, s.in[RList] = {4, -480 * way};
    c.member ^= 1;
    c.slot = c.slotTop = std::clamp(c.lastSlot, 0, campui::kEquipSlots - 1);
}
void campKey(App& app, SDL_Keycode key) {
    App::Camp& c = app.camp;
    if (c.leaving || c.pageOut || c.toOptions || campSliding(app)) return; // (the windows going; a state's wait)
    const bool back = key == SDLK_ESCAPE || key == SDLK_BACKSPACE || key == SDLK_AC_BACK || (key == SDLK_K && c.page == App::Camp::Main);
    const bool enter = key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE || key == SDLK_E;
    const bool up = key == SDLK_UP || key == SDLK_W, down = key == SDLK_DOWN || key == SDLK_S;
    // (the Vita's L / R - PgUp / PgDn - are the Back bar's arrows where it has them (ComUty_BackButtonProc
    // mode 2: 0x100 -> -3, 0x200 -> -2): the Items categories, the Equip member, the Status views; and
    // the seeds' count (_ComUty_NumCountProc, FUN_8105199e: L / R); left / right as well)
    const bool lr = campArrows(app) || (c.page == App::Camp::Items && c.phase == App::Camp::Ask);
    const bool left = key == SDLK_LEFT || key == SDLK_A || (lr && key == SDLK_PAGEUP),
               right = key == SDLK_RIGHT || key == SDLK_D || (lr && key == SDLK_PAGEDOWN);
    if (c.notice) { if (enter || back) tapDialogue(app); return; } // (the message's page turned, as a tap)
    if (c.phase == App::Camp::Exit) return;
    if (c.phase == App::Camp::Result) { if (enter || back) campChoose(app); return; } // (pad_chk_updw_box: the whole screen)
    if (back) { campBack(app); return; }
    if (enter) { // (the Vita's MuScrList_Proc, 65420-65426: ○ on a list taken only with its rows at rest)
        if (!campScrollable(app) || scrlist::rests(c.scroll, campScrollKey(app))) campChoose(app);
        return;
    }
    const int step = up ? -1 : down ? 1 : 0;
    if (c.page == App::Camp::Status && c.phase == App::Camp::Slot) { // the details: the arrows turn, the lists scroll
        if (left || right) campStatusTurn(app, right ? 1 : -1);
        else if (step && c.statusView != 0) campScrollBy(app, step);
        return;
    }
    if (c.page == App::Camp::Equip && c.phase == App::Camp::Slot && (left || right)) { // EquipProc 4 -> 7..10: the
        if (campMembers(app) > 1) { menuSound(app, se::kPage); campSwitchMember(app, right ? 1 : -1); } // other member's
        return;                                                                                         // slots (arrows with two)
    }
    switch (c.page) {
    case App::Camp::Main:
        if (step) { c.cursor = (c.cursor + step + campEntries(app)) % campEntries(app); menuSound(app, se::kCursor); }
        return;
    case App::Camp::Items:
    case App::Camp::Equip:
    case App::Camp::Genes:
        if (c.phase == App::Camp::List || c.phase == App::Camp::Entries) {
            const int n = int(c.list.size());
            if (step && n > 0) {
                c.listCursor = (c.listCursor + step + n) % n;
                c.listTop = std::clamp(c.listTop, std::max(0, c.listCursor - campui::kRows + 1), c.listCursor);
                menuSound(app, n > 1 ? se::kListFocus : se::kCursor); // (MuScrList_Draw: __effgo(1) as the focus row changes)
            } else if ((left || right) && c.page == App::Camp::Items && c.phase == App::Camp::List)
                campTurnCategory(app, left ? -1 : 1);
            return;
        }
        if (c.phase == App::Camp::Ask) {
            const int item = c.list.empty() ? 0 : c.list[size_t(c.listCursor)];
            if ((left || right) && item >= 3 && item <= 6) {
                const int was = c.count;
                c.count = std::clamp(c.count + (right ? 1 : -1), 1, std::max(1, campSeedLimit(app, campCharacter(app, c.member), item)));
                menuSound(app, c.count != was ? se::kChoose : se::kBuzzer);
            } else if (step) { c.confirm ^= 1; menuSound(app, se::kCursor); }
            return;
        }
        if (c.phase == App::Camp::Slot && c.page == App::Camp::Equip) {
            if (step) {
                c.slot = (c.slot + step + campui::kEquipSlots) % campui::kEquipSlots;
                if (c.slot < c.slotTop) c.slotTop = c.slot;
                if (c.slot >= c.slotTop + campui::kRows) c.slotTop = c.slot - campui::kRows + 1;
                menuSound(app, se::kCursor);
            }
            return;
        }
        [[fallthrough]];
    case App::Camp::Status:
        if (c.phase == App::Camp::Member && (step || left || right)) { if (campMembers(app) > 1) c.member ^= 1; menuSound(app, se::kCursor); }
        return;
    }
}

// ComUty_BackButtonProc's mode for the Back bar: 2 (the < > arrows: -3 at 0xc0, Back at 0xf8, -2 at
// 0x175, each 0x22 high at 0x11e) on the Items list (MuScrList's, SetIdInfo(10, .., 2)), the
// equipment slots with two members and the Status details (DAT_000d2e0c = 2); none during the Items
// Yes / No, its board and the Emergency Exit; else 1
bool campArrows(const App& app) {
    const App::Camp& c = app.camp;
    return (c.page == App::Camp::Items && c.phase == App::Camp::List) ||
           (c.page == App::Camp::Equip && c.phase == App::Camp::Slot && campMembers(app) > 1) ||
           (c.page == App::Camp::Status && c.phase == App::Camp::Slot);
}
// ComUty_BackButtonProc mode 3 (Back and OK, OK -5): a plate's entries in Equip while the member doesn't
// wear it (EquipProc 0x1a: SetIdInfo(7, 0x13, ..); Part 01 1195 / 1210 s "Back | OK")
bool campOkBar(const App& app) {
    const App::Camp& c = app.camp;
    return c.page == App::Camp::Equip && c.phase == App::Camp::Entries && !campWears(app, campCharacter(app, c.member), c.plate);
}
bool campBackShown(const App& app) {
    const App::Camp& c = app.camp;
    return !(c.page == App::Camp::Items && (c.phase == App::Camp::Ask || c.phase == App::Camp::Result || c.phase == App::Camp::Exit));
}

void campClick(App& app, float px, float py) {
    using namespace campui;
    App::Camp& c = app.camp;
    if (c.notice) { tapDialogue(app); return; }
    if (c.phase == App::Camp::Exit) return;
    if (c.phase == App::Camp::Result) { campChoose(app); return; } // (a tap anywhere)
    if (c.leaving || c.pageOut || c.toOptions || campSliding(app)) return;
    float lx, ly;
    app.renderer.toLogical(px, py, lx, ly);
    // (widescreen: Back centred, the section's box on the left, the rest on the right - drawCamp)
    float cx, my, lx0, rx;
    menuPointAt(app, lx, ly, app.renderer.uiX(), cx, my);
    menuPointAt(app, lx, ly, wideLeft(app), lx0, my);
    menuPointAt(app, lx, ly, wideRight(app), rx, my);
    float mx = rx;
    auto in = [&](float x, float y, float w, float h) { return mx >= x && mx < x + w && my >= y && my < y + h; };
    auto inAt = [&](float at, float x, float y, float w, float h) { return at >= x && at < x + w && my >= y && my < y + h; };
    if (campBackShown(app)) {
        if (campOkBar(app)) {
            if (inAt(cx, 0xc0, campBackY(app), 0x72, 34)) campBack(app);
            else if (inAt(cx, 0x137, campBackY(app), 0x72, 34)) campChoose(app); // (-5: __ChangeEquipItem)
            if (inAt(cx, 0xc0, campBackY(app), 0xe8, 34)) return;
        } else if (!campArrows(app) && inAt(cx, kBackX, campBackY(app), 256, 34)) { campBack(app); return; }
        if (campArrows(app)) {
            if (inAt(cx, 0xc0, campBackY(app), 0x33, 34)) { campKey(app, SDLK_LEFT); return; }
            if (inAt(cx, 0xf8, campBackY(app), 0x78, 34)) { campBack(app); return; }
            if (inAt(cx, 0x175, campBackY(app), 0x33, 34)) { campKey(app, SDLK_RIGHT); return; }
        }
    }
    switch (c.page) {
    case App::Camp::Main:
        for (int i = 0; i < campEntries(app); ++i)
            if (in(kPlateX + 360 * (1 - campEase(app)), kPlateY + kPlateStep * float(i), 136, 40)) { c.cursor = i; campChoose(app); return; }
        // CampMain 5: a face board's box (8, 0x30 + 0x58k, 0xfc x 0x50) - SE 2, then Equip with that member's
        // slots at once (CampMain 7's 5: EquipProc from 3); Back there leaves the page (EquipProc 6 -> 0x22)
        for (int k = 0; k < campMembers(app); ++k)
            if (inAt(lx0, 8, 48 + 88.0f * float(k), 252, 80)) {
                campOpenPage(app, 1);
                c.member = k, c.phase = App::Camp::Slot, c.slot = 0, c.slotTop = 0, c.direct = true;
                // (EquipProc from 3: its count 4 .. -1, then CategoryChoiceInit - the title, the state board and the
                // slot list on six frames after CampMain 6; window 4, Back, stays)
                            App::Camp::Slide& s = campSlide(app, 7, false);
                s.in[RTitle] = {6, -140}, s.in[RBoard] = {6, -240}, s.in[RList] = {6, 480};
                return;
            }
        return;
    default: break;
    }
    if (c.phase == App::Camp::Member) { // __MuCommon_CharChoiceProc: a member's plate (0x148, 0xb0 + 0x38k) or face board (8, 0x30 + 0x58k)
        for (int k = 0; k < campMembers(app); ++k)
            if (in(kPlateX + 360 * (1 - campEase(app)), kMemberY + kMemberStep * float(k), 136, 40) ||
                inAt(lx0, 8, 48 + 88.0f * float(k), 252, 80)) { c.member = k; campChoose(app); return; }
        return;
    }
    if (c.page == App::Camp::Status && c.phase == App::Camp::Slot && c.statusView == 0) return; // (the board: arrows and swipes)
    if (campScrollable(app)) {
        // A tap's lift on the rows (scrlist::tap - MuScrList_Proc): over / under the line a row on, the line's row
        // chosen (the Status lists and a plate's entries in Equip choose nothing), the pills' boxes to the other end.
        // (The gauge, arrows and all, is a held control: campPointerDown.) The Items category's label takes no tap:
        // CampMainPrc 0's state 2 runs only MuScrList_Proc (63400-63445) - the arrows and swipes turn it
        scrlist::ready(c.scroll, campScrollKey(app), campFocus(app));
        const int r = scrlist::tap(c.scroll, mx, my, campScrollCount(app));
        if (r == 2 && c.page != App::Camp::Status && !(c.page == App::Camp::Equip && c.phase == App::Camp::Entries)) campChoose(app);
        else if (r == 1) campFollowed(app, scrlist::follow(c.scroll, campCursor(app), campScrollCount(app)));
        if (r) return;
    }
    if (c.phase == App::Camp::Ask) {
        const int item = c.list.empty() ? 0 : c.list[size_t(c.listCursor)];
        if ((in(336, 60, 36, 31) || in(436, 60, 36, 31)) && item >= 3 && item <= 6) {} // (stepped while held: pressCountStep)
        else if (in(336, 104, 136, 40)) { c.confirm = 0; campChoose(app); }
        else if (in(336, 160, 136, 40)) { c.confirm = 1; campChoose(app); }
    }
}

void campPointerPoint(const App& app, float px, float py, float& mx, float& my) {
    float lx, ly;
    app.renderer.toLogical(px, py, lx, ly);
    menuPointAt(app, lx, ly, wideRight(app), mx, my);
}
// A touch held inside a Camp box (menu units of the group drawn from `origin`): 2.2.0 lights the control
// while _pad_chk_updw_box finds the finger on it (not once it drags a list)
bool campPressIn(const App& app, float origin, float x, float y, float w, float h) {
    const auto& p = app.campPointer;
    if (!p.down || p.dragging) return false;
    float lx, ly, mx, my;
    app.renderer.toLogical(p.curPx, p.curPy, lx, ly);
    menuPointAt(app, lx, ly, origin, mx, my);
    return mx >= x && mx < x + w && my >= y && my < y + h;
}
bool campSwipeBoard(const App& app, const App::CampPointer& p);
void campPointerDown(App& app, float px, float py) {
    auto& p = app.campPointer;
    p = App::CampPointer();
    p.down = true, p.page = app.camp.page, p.phase = app.camp.phase;
    p.px = px, p.py = py;
    p.curPx = px, p.curPy = py;
    campPointerPoint(app, px, py, p.startX, p.startY);
    p.lastY = p.startY;
    // MuScrList_Proc: the gauge's box, else the drag area (0, 76, 456, 208) - the boards' side too (widescreen: all left
    // of the gauge) - its touch (scrlist::press)
    if (campScrollable(app) && !campSwipeBoard(app, p)) {
        App::Camp& c = app.camp;
        scrlist::ready(c.scroll, campScrollKey(app), campFocus(app));
        scrlist::press(c.scroll, std::max(p.startX, 1.0f), p.startY, campScrollCount(app), app.inputTime);
        p.bar = c.scroll.hold == ScrList::Gauge;
        campFollowed(app, scrlist::follow(c.scroll, campCursor(app), campScrollCount(app)));
    }
}
// The lists MuScrList_SetSwipeMove marks (d1f9c): the Items list (__MuCampScrListItemInit, 64719), the equipment
// slots with two members (__MuCampScrListEquipInit kind 4, 65096) and the Status details' two lists (kind 0xc and
// the skills', 63873); their -3 / -2 turn what the Back bar's arrows turn
bool campSwipeList(const App& app) {
    const App::Camp& c = app.camp;
    return (c.page == App::Camp::Items && c.phase == App::Camp::List) ||
           (c.page == App::Camp::Equip && c.phase == App::Camp::Slot && campMembers(app) > 1) ||
           (c.page == App::Camp::Status && c.phase == App::Camp::Slot && c.statusView != 0);
}
// The Status board's view (CampMainPrc 3's 5: ComUty_GetSwipeX over (0, 0x4c, 0x1c8 x 0xd0))
bool campSwipeBoard(const App& app, const App::CampPointer& p) {
    return app.camp.page == App::Camp::Status && app.camp.phase == App::Camp::Slot && app.camp.statusView == 0 && !p.bar &&
           p.startX >= 0 && p.startX < 0x1c8 && p.startY >= 0x4c && p.startY < 0x4c + 0xd0;
}
void campPointerMove(App& app, float px, float py) {
    auto& p = app.campPointer;
    if (!p.down) return;
    if (!app.campOpen || app.camp.page != p.page || app.camp.phase != p.phase) { p = App::CampPointer(); return; }
    float mx, my;
    campPointerPoint(app, px, py, mx, my);
    p.curPx = px, p.curPy = py;
    const bool list = campScrollable(app);
    if (std::fabs(mx - p.startX) > 6 || std::fabs(my - p.startY) > 6) p.dragging = true;
    const float dx = mx - p.startX, dy = my - p.startY;
    ScrList& sl = app.camp.scroll;
    if (list && p.bar) {
        scrlist::move(sl, std::max(mx, 1.0f), my, campScrollCount(app), app.inputTime); // (ComUty_PadChkMove held)
        campFollowed(app, scrlist::follow(sl, campCursor(app), campScrollCount(app)));
    } else if (campSwipeBoard(app, p)) {
        app.camp.swipe = dx; // (the board drawn at d304c + 0x134: the finger's way, 64350)
    } else if (list && sl.hold == ScrList::Rows) {
        // MuScrList_Proc (54700-54760): a drag past 5 up or down scrolls (d1f9e); past 10 sideways first, on a list that
        // swipes, the list follows the finger less those 10 (d1f9d, d1fa4) and scrolls no more
        if (!p.sideways && !p.upright) {
            if (std::fabs(dy) > 5) p.upright = true;
            else if (campSwipeList(app) && std::fabs(dx) > 10) p.sideways = true, sl.hold = ScrList::None, sl.drag = 0;
        }
        if (p.sideways) app.camp.swipe = dx > 0 ? dx - 10 : dx + 10;
        else { // (case 3: the rows with the finger, pixel for pixel)
            scrlist::move(sl, std::max(mx, 1.0f), my, campScrollCount(app), app.inputTime);
            campFollowed(app, scrlist::follow(sl, campCursor(app), campScrollCount(app)));
        }
    }
    p.lastY = my;
}
void campPointerUp(App& app, float px, float py) {
    if (!app.campPointer.down) return;
    campPointerMove(app, px, py);
    const auto p = app.campPointer;
    const bool board = app.campOpen && p.down && app.camp.page == p.page && app.camp.phase == p.phase && campSwipeBoard(app, p);
    app.campPointer = App::CampPointer();
    if (board && p.dragging) {
        // ComUty_GetSwipeX: let go more than 30 to the left -2, to the right -3 (SE 5); else the board back at once
        float mx, my;
        campPointerPoint(app, px, py, mx, my);
        const float dx = p.startX - mx;
        if (std::fabs(dx) > 30 && std::fabs(dx) > std::fabs(my - p.startY)) campKey(app, dx > 0 ? SDLK_RIGHT : SDLK_LEFT);
        app.camp.swipe = 0;
        return;
    }
    if (p.sideways) { // MuScrList_Proc's release (54680-54700): d1fa4 past 30 is -3, past -30 -2 (SE 5); else it eases back
        if (app.campOpen && app.camp.page == p.page && app.camp.phase == p.phase && std::fabs(app.camp.swipe) > 30)
            campKey(app, app.camp.swipe > 0 ? SDLK_LEFT : SDLK_RIGHT);
        return;
    }
    const bool same = p.down && app.campOpen && app.camp.page == p.page && app.camp.phase == p.phase;
    ScrList& sl = app.camp.scroll;
    if (same && campScrollable(app) && sl.hold != ScrList::None) { // (the list's touch lifting: MuScrList_Proc case 1 / 4)
        float mx, my;
        campPointerPoint(app, px, py, mx, my);
        const bool tap = scrlist::lift(sl, std::max(mx, 1.0f), my, campScrollCount(app), app.inputTime);
        campFollowed(app, scrlist::follow(sl, campCursor(app), campScrollCount(app)));
        if (!tap || p.dragging) return;
    } else sl.hold = ScrList::None;
    if (same && !p.dragging && !p.bar) campClick(app, px, py);
}

// A plate's name at the ComPlate's (x, y) (ComUty_ComPlate_Base: + (0x38, 0xc)): MuComFontPlateDisp (0x5242c,
// 51012-51032) draws it a unit lower (y + 1), in its colours - 0 white, 2 grey (0x808080), 3 yellow -, the
// plate and the icon staying as they are; at the size ComUty_ComPlate_ChangeStr -> _MuComFontChangeTex
// (51095-51110) registers it: 13, 14 in Japanese, Chinese, Korean
void plateText(const App& app, cr::MenuPainter& m, const std::string& s, float x, float y, int style, float a) {
    const float size = menuFontSize(app);
    if (style == 2) m.textTint(s, x, y + 1, 0x80 / 255.0f, 0x80 / 255.0f, 0x80 / 255.0f, a, size);
    else if (style == 3) m.textTint(s, x, y + 1, 1, 1, 0, a, size);
    else m.text(s, x, y + 1, false, a, size);
}

// A list row (ComUty_ComPlate_Base): plate 8 / 9, the icon at (12, 8), the name at (56, 12) in the style's
// colour; an item count (ComUty_PictNumDraw(x + 0xde, y + 0x1e, 2 places, 10, style 0 - white)
void drawCampRow(App& app, cr::MenuPainter& m, float x, float y, int item, int count, bool lit, float a, int style,
                 const std::string& label) {
    m.picture(lit ? 9 : 8, x, y, a);
    if (const cr::ItemParam* p = app.itemTable.item(item); p && p->icon >= 0)
        m.piece(x + 12, y + 8, float(p->icon % 16) * 32, 248 + float(p->icon / 16) * 32, 32, 32, a);
    plateText(app, m, item > 0 ? itemName(app, item) : std::string("-"), x + 56, y + 12, style, a);
    if (!label.empty()) m.text(label, x + 56, y + 30, false, a * 0.8f, 11);
    if (count > 0) drawShopNumber(m, count, x + 222, y + 30, 2, 10, a, 0);
}

// A plate's entry in the Camp (MuScrList type 7: ComUty_ComPlate_Equip_GeneAbility): the base plate,
// then for a gene "MP/" (picture 0x1b at (188, 34)) and its cost in three white digits from (218, 30) -
// halved while the member has skill 0x84; a skill shows no cost (Part 01 1210 s "Analyze MP/ 2")
void drawGeneAbilityRow(App& app, cr::MenuPainter& m, float x, float y, int it, int ch, bool lit, float a) {
    drawCampRow(app, m, x, y, it, 0, lit, a);
    if (cr::isSkill(it)) return;
    const cr::BattleActionDef* act = app.battleActions.get(cr::geneAction(it));
    if (!act) return;
    const int cost = ch >= 0 && ch < cr::kCharacters && cr::hasSkill(app.game, ch, 132) ? act->mp / 2 : act->mp;
    m.picture(27, x + 188, y + 34, a);
    drawShopNumber(m, cost, x + 218, y + 30, 3, 10, a, 0);
}

// A gene plate's row (ComUty_ComPlate_Equip_GenePlate): plate 8 / 9, its icon and name - yellow (3)
// for one worn, grey (2) while none of it is known - then six pips at (167 + 14k, 30), the unknown ones
// first and dim (10, known 11), or "MASTER" (picture 96 at (176, 24)) once all six are known
// (Part 01 1190 s: Human Warrior two gold, the rest dark)
void drawGeneRow(App& app, cr::MenuPainter& m, float x, float y, int plate, bool lit, bool worn, float a) {
    m.picture(lit ? 9 : 8, x, y, a);
    if (const cr::ItemParam* p = app.itemTable.item(plate); p && p->icon >= 0)
        m.piece(x + 12, y + 8, float(p->icon % 16) * 32, 248 + float(p->icon / 16) * 32, 32, 32, a);
    const int known = cr::plateLearned(app.game, plate);
    constexpr int total = 6;
    plateText(app, m, itemName(app, plate), x + 56, y + 12, known == 0 ? 2 : worn ? 3 : 0, a);
    if (known >= total) m.picture(96, x + 176, y + 24, a);
    else
        for (int k = 0; k < total; ++k) m.picture(k < total - known ? 10 : 11, x + 251 - 14 * float(total) + 14 * float(k), y + 30, a);
}

// ComUty_GuageBarDraw (0x4dd88; 456, 72, offset, 2, n, 48, 208): the track (window sheet (144, 184) 16 x 16,
// (144, 200) 16 x 8 down to the end (144, 208) at +200), its arrows (0x4c at the top, 0x4d at +200) and the
// knob - top (160, 196) 16 x 12, middle (160, 208) 16 x 4, end (160, 212) 16 x 8 - max(24, 368 / n) long (the
// whole 192 under three rows), moved (196 - L) x offset / ((n - 2) x 48) down from +12. Drawn for any list
void drawCampScrollBar(cr::MenuPainter& m, float scroll, int n, float a) {
    using namespace campui;
    const float x = kGaugeX, y = kGaugeY, h = kGaugeH;
    m.piece(x, y, 144, 184, 16, 16, a);
    for (int i = 0; i < int(h - 24) / 8; ++i) m.piece(x, y + 16 + 8 * float(i), 144, 200, 16, 8, a);
    m.piece(x, y + h - 8, 144, 208, 16, 16, a);
    m.picture(0x4c, x, y, a);
    m.picture(0x4d, x, y + h - 8, a);
    float len = 192, at = 0;
    if (n >= 3) {
        len = std::max(24.0f, std::floor((h - 24) * 2 / float(n)));
        at = std::clamp(std::floor((196 - len) * scroll / (float(n - 2) * kRowH)), 0.0f, h - 24 - len + 10);
    }
    const float top = y + 12 + at;
    m.piece(x, top, 160, 196, 16, 12, a);
    if (len > 20) m.pieceTo(x, top + 12, 16, len - 20, 160, 208, 16, 4, a);
    m.piece(x, top + len - 8, 160, 212, 16, 8, a);
}

// ComUty_GeneBoardDraw (0x4bfac) at (x, y): the plate's name (FUN_0004a4dc, ComUty_StateBoardChrName: a white
// 163 x 16 bar, the name centred on it in the dark style) and MuComRectDraw's white 163 x 16 under "SKILL n / 6"
// - both solid, no panel behind the board - then EITHER (bit 1, while the plate list is browsed) the names of
// its known entries, up to six, each centred in 128 from (x + 12, y + 75) 18 apart, OR (bit 2, a plate chosen)
// its picture (gene_m*c.mct) at (x + 12, y + 68). Part 01 1190 / 1195 s: "Human Warrior SKILL 2 / 6" with
// "Trompe-L'oeil", "Physical Block"; "Deer Genes SKILL 1 / 6" with the plate's picture
void drawGeneBoard(App& app, cr::MenuPainter& m, float x, float y, int plate, float a, int mode = 1) {
    if (!cr::isPlate(plate)) return;
    const float white[4] = {1, 1, 1, a};
    m.quad(x, y, 163, 16, white, white);
    const std::string name = itemName(app, plate);
    m.textCentred(name, x + (163 - m.textWidth(name, 14)) / 2, y + 8, true, a, 14);
    m.quad(x, y + 24, 163, 16, white, white);
    m.picture(95, x + 24, y + 27, a);
    drawShopNumber(m, cr::plateLearned(app.game, plate), x + 90, y + 24, 1, 12, a, 2);
    m.picture(65, x + 102, y + 24, a);
    drawShopNumber(m, 6, x + 114, y + 24, 1, 12, a, 2);
    static const char* const kArt[cr::kPlates] = {"gene_m001c.mct", "gene_m002c.mct", "gene_m201c.mct", "gene_m202c.mct",
                                                  "gene_m203c.mct", "gene_m204c.mct", "gene_m205c.mct", "gene_m206c.mct",
                                                  "gene_m207c.mct", "gene_m208c.mct", "gene_m209c.mct", "gene_m210c.mct",
                                                  "gene_m211c.mct", "gene_m212c.mct", "gene_m213c.mct", "gene_m214c.mct",
                                                  "gene_m215c.mct", "gene_m216c.mct", "gene_m217c.mct", "gene_m218c.mct",
                                                  "gene_m219c.mct", "gene_m220c.mct"};
    cr::Texture& art = app.camp.plateArt[size_t(plate - cr::kFirstPlate)];
    if (!art.id) {
        cr::Image img;
        if (loadImage(app, kArt[plate - cr::kFirstPlate], img)) art = app.renderer.upload(img);
    }
    if (mode == 2) {
        if (art.id) { // the picture, within 128 x 128 (the iPhone sheet's size)
            const float w = float(art.width), h = float(art.height), k = std::min(1.0f, std::min(128 / std::max(w, 1.0f), 128 / std::max(h, 1.0f)));
            m.image(art, x + 12, y + 68, w * k, h * k, a);
        }
        return;
    }
    float ly = y + 75;
    int shown = 0;
    for (int e : cr::plateContents(plate)) {
        if (e <= 0 || ownedCount(app, e) <= 0 || shown >= 6) continue;
        const std::string entry = itemName(app, e);
        m.text(entry, x + 12 + (128 - m.textWidth(entry, menuFontSize(app))) / 2, ly, false, a, menuFontSize(app)); // (ComUty_GeneAbilityStrChange 46306: 13 / 14)
        ly += 18;
        ++shown;
    }
}

// ---- The staff roll (Util 7: ModeStaffRollMenu(var 0), __menuu_StaffRollSet / Prc / Put) ----
// EndBack0<var 0 + 1>_hd.mct (sheet 1) behind it all: its picture 10 (1024 x 1790 at (0, y): the
// HD build draws that slot 1:1 on the 1024 x 768 screen) scrolls up 0.222 a frame (2.22 fast) to
// y -1022; over its left a black panel (tex2d (x - 16, -32), 280 x 384) carries the credits -
// system text block 9's 336 lines, 21 at a time, 40 apart from y 800, x 32, rising 2.826 a frame
// (10 steps a frame while the corner button (460, -32, 32 x 32: sheet 0 picture 114) is held),
// each past y -40 taking the next line at 800 (a line of < 3 characters is a gap). Once all have
// gone and the picture is at the bottom: 100 frames (the text and the button fade at 5 left),
// the panel slides out (3 a frame, 9 fast) to -280, the end card shows (sheet 1 picture 11 -
// mode 2: 12 - at (0, 328): the sheet's strips below the picture), and after 4 frames a tap (sound
// 2) fades it all out and the menu ends. Sheet 1 (MuTex_GetTexInfo 1, 0xca490): 8 shorts a picture.
int staffLines(const App& app) { // the scripts': 2.2.0's and the Vita's (voiced: sys.evt's +0x40) or 1.0.1's
    // (Android 1.1.1 takes entries 0..0x1ab: 0x6aabe cmp r7, #427 - its own text's)
    if (app.androidOnly) return staffroll::kAndroidLines;
    return app.phoneData || !app.battleVoiceTable.empty() ? staffroll::kPhoneLines : staffroll::kLines;
}
// Android 1.1.1's staff roll (InitStaffRollMenu's Open / Prc / Put, 0x6a5d0..0x6ae20) measures the phone's
// screen, oy (edgeY) beyond the 320 above and below: the picture drawn oy higher, scrolled to
// -(895 - (320 + 2 oy)) at that over 4600 a frame; the lines from 320 + oy, recycled past -20 - oy,
// (340 + 2 oy) / 20 of them; the panel from -oy down past the bottom; the fast button at y -oy. 0 on
// the iPhone, so 2.2.0's numbers
float staffPicEnd(const App::StaffRoll& st) { return st.phone ? staffroll::kPhonePicEnd - 2 * st.edgeY : staffroll::kPicEnd; }
int staffShown(const App::StaffRoll& st) { return st.phone ? std::clamp(int((340 + 2 * st.edgeY) / 20), 1, 32) : 21; }

void staffWin(App& app, int w, bool on) { app.staffRoll.winTo[w] = on ? 1.0f : 0.0f; }

void openStaffRoll(App& app, int mode) {
    App::StaffRoll& st = app.staffRoll;
    app.renderer.destroy(st.tex);
    st = App::StaffRoll();
    st.open = true;
    st.mode = std::clamp(mode, 0, 2);
    st.phone = app.renderer.phoneLayout();
    st.edgeY = st.phone ? androidEdgeY(app) : 0;
    // (Android 1.1.1 0x6aa40..0x6aa68: ((screen h / scale) x 20 / 320 + 315) x 20 / 4600 - 1.457 on a
    // 3:2 screen, as the screen's height asks; 2.2.0 1.9217 on the iPhone, 2.826 on the iPad)
    st.lineStep = !st.phone ? staffroll::kLine
                  : app.renderer.androidScreen() ? ((320 + 2 * st.edgeY) * 20 / 320 + 315) * 20 / 4600.0f
                                                 : staffroll::kPhoneLine;
    const int shown = staffShown(st);
    st.next = shown;
    for (int i = 0; i < shown; ++i)
        st.lines[i] = st.phone ? App::StaffRoll::Line{true, 320.0f + st.edgeY + 20.0f * float(i), i} : App::StaffRoll::Line{true, 800.0f + 40.0f * float(i), i};
    cr::Image img;
    static const char* const kBack[3] = {"EndBack01", "EndBack02", "EndBack03"}; // _hd (iPad), else the iPhone's
    if (loadImage(app, std::string(kBack[st.mode]) + "_hd.mct", img) || loadImage(app, std::string(kBack[st.mode]) + ".mct", img))
        st.tex = app.renderer.upload(img);
    staffWin(app, 1, true);
}

std::string staffLine(const App& app, int entry) { return sysText(app, 60000 + entry); } // font block 9

void stepStaffRoll(App& app) {
    using namespace staffroll;
    App::StaffRoll& st = app.staffRoll;
    if (!st.open) return;
    for (int w = 0; w < 5; ++w) // the windows fade 4 frames
        st.win[w] = st.win[w] < st.winTo[w] ? std::min(st.winTo[w], st.win[w] + 0.25f) : std::max(st.winTo[w], st.win[w] - 0.25f);
    switch (st.state) {
    case 0: st.state = 1; break; // the texture read
    case 1:
        staffWin(app, 0, true), staffWin(app, 3, true), staffWin(app, 2, true);
        st.state = 2;
        break;
    case 2:
        if (st.countdown < 1) {
            const int total = staffLines(app), shown = staffShown(st);
            if (st.next <= total) {
                const int steps = st.fast ? 10 : 1;
                for (int i = 0; i < shown; ++i) {
                    auto& l = st.lines[i];
                    for (int k = 0; k < steps; ++k) {
                        l.y -= st.lineStep;
                        if (l.y > (st.phone ? -20 - st.edgeY : -40)) continue;
                        const int e = st.next++;
                        l.y = st.phone ? 320 + st.edgeY : 800;
                        l.on = e < total && staffLine(app, e).size() >= 3;
                        l.entry = e;
                    }
                }
            }
            const float end = staffPicEnd(st);
            const float phonePic = (-kPhonePicEnd + 2 * st.edgeY) / 4600.0f; // (Android: [0xb8e94] / 4600; 0.125 at oy 0)
            st.picY = std::max(end, st.picY - (st.phone ? (st.fast ? 10 * phonePic : phonePic) : (st.fast ? kPicFast : kPic)));
            if (st.next > total && st.picY == end) st.countdown = 100;
        } else {
            if (st.countdown == 5) staffWin(app, 3, false), staffWin(app, 2, false);
            if (--st.countdown < 1) st.state = 4;
        }
        break;
    case 4:
        st.panel -= st.fast ? 9.0f : 3.0f;
        if (st.panel < kPanelEnd) st.state = 5;
        break;
    case 5: staffWin(app, 4, true); st.timer = 4, st.state = 6; break;
    case 6: if (st.timer-- < 0) st.state = 7; break;
    case 7: break; // a tap (staffRollTap)
    case 8: staffWin(app, 0, false), staffWin(app, 1, false); st.timer = 4, st.state = 9; break;
    case 9: if (st.timer-- < 0) st.state = 3; break;
    case 3: // __mc_SetMenuStat(0xc, 4): the script goes on
        app.renderer.destroy(st.tex);
        st = App::StaffRoll();
        break;
    default: break;
    }
}

void staffRollTap(App& app) {
    App::StaffRoll& st = app.staffRoll;
    if (!st.open || st.state != 7) return;
    menuSound(app, 2);
    st.state = 8;
}

void drawStaffRoll(App& app) {
    const App::StaffRoll& st = app.staffRoll;
    if (!st.open) return;
    cr::Renderer& r = app.renderer;
    const float W = r.screenW(), H = cr::Renderer::kLogicalH, ox = r.uiX();
    r.setOrigin(0);
    r.fillRect(0, 0, W, H, 0, 0, 0, 1);
    if (st.phone) { // 2.2.0: the picture (512 x 895) at tex2d (0, y); the panel (x, 0) 280 x 320
        r.setOrigin(ox);
        if (st.tex.id && st.win[0] > 0) {
            const float k = float(st.tex.width) / 1024.0f;
            r.drawQuadUV(st.tex, 32, 64 + 2 * (st.picY - st.edgeY), 1024, 1790, 0, 0, 1024 * k, 1790 * k, 1, 1, 1, st.win[0]);
        }
        if (st.win[1] > 0) r.fillRect(32 + 2 * st.panel, 64 - 2 * st.edgeY, 560, 640 + 4 * st.edgeY, 0, 0, 0, st.win[1]);
        r.setOrigin(0);
    } else {
    if (st.tex.id && st.win[0] > 0) { // picture 10: (0, 0, 1024, 1790) of the 1024 x 2048 sheet
        const float k = float(st.tex.width) / 1024.0f;
        r.drawQuadUV(st.tex, ox, st.picY, 1024, 1790, 0, 0, 1024 * k, 1790 * k, 1, 1, 1, st.win[0]);
    }
    if (st.win[1] > 0) r.fillRect(ox + 2 * st.panel, 0, 560, H, 0, 0, 0, st.win[1]); // tex2d (x - 16, -32) 280 x 384
    }
    cr::MenuPainter& m = app.menu;
    if (app.windowTex.id) m.begin(r, app.windowTex, &app.font);
    r.setOrigin(ox);
    if (st.win[3] > 0)
        for (const auto& l : st.lines) {
            if (!l.on) continue;
            if (st.phone) { if (l.y > -20 - st.edgeY && l.y < 320 + st.edgeY) m.text(staffLine(app, l.entry), 16 + 16, l.y + 32, false, st.win[3], menuFontSize(app)); } // (StaffRollSet 73990: 13 / 14)
            else if (l.y > -40 && l.y < 800) m.text(staffLine(app, l.entry), 16, l.y / 2, false, st.win[3], menuFontSize(app));
        }
    if (st.win[2] > 0 && app.windowTex.id) // the fast button (2.2.0: (444, 0))
        m.picture(114, (st.phone ? 444 : 460) + 16, (st.phone ? -st.edgeY : -32) + 32, st.win[2]);
    if (st.tex.id && st.win[4] > 0) { // the end card: picture 11 (0, 1936, 1024, 112) / 12 (0, 1824, 1024, 114)
        const float k = float(st.tex.width) / 1024.0f;
        r.setOrigin(0);
        const float ex = st.phone ? ox + 32 : ox, ey = st.phone ? 2.0f * 144 + 64 : 328; // 2.2.0: (0, 144)
        if (st.mode == 2) r.drawQuadUV(st.tex, ex, ey - 1, 1024, 114, 0, 1824 * k, 1024 * k, 114 * k, 1, 1, 1, st.win[4]);
        else r.drawQuadUV(st.tex, ex, ey, 1024, 112, 0, 1936 * k, 1024 * k, 112 * k, 1, 1, 1, st.win[4]);
    }
    r.setOrigin(ox);
}

// ---- The last battle's pair choice (Util 8; __menuu_LastBtSet / Prc / Put / Clr) ----
// Flag 0 clear: StartLastBtMenu - the rounds start over (no menu). Else, round `count` (0..3):
// everyone's HP / MP restored (pcparam_hpmp_all_recovery), window 0 the header (sheet 0 picture
// 109 at (8, 6)), 1 the round's title (font block 7 entry 0x68 + count) over a rule (picture 15 at
// (0, 56)), 2 the guardian's picture (hi / mizu / kaze / mu.mct, sheet 1 picture 5: 256 x 256 at
// (0, 64)) - 1 and 2 slide in from 140 / 240 to the left - and the pairs not yet used as plates.
// The plates: the leaders' names (MuScrList_ItemRegist(0x1e + 2p) registers block 7 entry id - 1). One chosen: the windows go, that pair is the party
// (Set_Prtyno) and its camp menu opens; once it closes, the question (block 6 entry 0x13 + count,
// var 0 = the leader's name, MuStr_GetCharNameId(2p) = block 7 entry 2p + 0x1d) and Yes / No: No
// back to the plates, Yes closes - count + 1, the pair's bit set, var 0 = the pair; after the
// fourth, count and the bits clear and so does flag 1.
namespace lastbt {
// The pairs: MuScrList rows (list layout 0, as the World Select's and the shop's) of
// ComUty_ComPlate_LastBt - plate 8 / 9 (lit), icon 4 at (12, 8), the leader's name at (56, 12)
constexpr float kRowX = 192, kRowY = 92, kRowStep = 48, kRowW = 256, kRowH = 44; // menu units
}

// MuScrList_ItemRegist(0x1e + 2p) registers block 7 entry id - 1: the pair's leader
std::string lastBattlePlate(const App& app, int p) { return sysText(app, 58029 + 2 * p); }

void lbWin(App& app, int w, bool on) { app.lastBattle.winTo[w] = on ? 1.0f : 0.0f; }

void openLastBattle(App& app) {
    App::LastBattle& lb = app.lastBattle;
    app.renderer.destroy(lb.tex);
    lb.open = true, lb.state = 0, lb.timer = 0, lb.cursor = 0, lb.chosen = -1, lb.confirm = 0;
    for (float& w : lb.win) w = 0;
    for (float& w : lb.winTo) w = 0;
    cr::Image img;
    static const char* const kGuardian[4] = {"hi.mct", "mizu.mct", "kaze.mct", "mu.mct"};
    if (loadImage(app, kGuardian[std::clamp(lb.count, 0, 3)], img)) lb.tex = app.renderer.upload(img);
    for (size_t c = 0; c < app.game.party.size(); ++c) { // pcparam_hpmp_all_recovery (29898-29932): MP as a number
        app.game.party[c].hp = -1;
        app.game.party[c].mp = cr::memberStat(app.game, int(c), cr::kMp, app.itemTable);
    }
}

void lastBattleList(App& app, bool windows) { // __LastBt_CharChoiceInit
    App::LastBattle& lb = app.lastBattle;
    lb.pairs.clear();
    for (int p = 0; p < 4; ++p)
        if (!(lb.mask & (1 << p))) lb.pairs.push_back(p);
    lb.cursor = 0;
    if (windows) lbWin(app, 0, true), lbWin(app, 1, true), lbWin(app, 2, true);
    lbWin(app, 3, true); // the plates
    lb.state = 3;
}

void lastBattleChoose(App& app, int i) {
    App::LastBattle& lb = app.lastBattle;
    if (lb.state != 3 || i < 0 || i >= int(lb.pairs.size())) return;
    menuSound(app, se::kChoose);
    lb.chosen = lb.pairs[size_t(i)];
    lb.state = 4;
}

void lastBattleAnswer(App& app, bool yes, bool cancelled) {
    App::LastBattle& lb = app.lastBattle;
    if (lb.state != 10) return;
    menuSound(app, cancelled ? se::kBack : se::kChoose); // (LastBtPrc's ComUty_YesNoChoiceProc(.., 1): the Vita's × 3)
    if (yes) lb.state = 11;
    else lbWin(app, 5, false), lbWin(app, 6, false), lb.state = 2;
}

void stepLastBattle(App& app) {
    App::LastBattle& lb = app.lastBattle;
    if (!lb.open) return;
    for (int w = 0; w < 7; ++w)
        lb.win[w] = lb.win[w] < lb.winTo[w] ? std::min(lb.winTo[w], lb.win[w] + 0.25f) : std::max(lb.winTo[w], lb.win[w] - 0.25f);
    switch (lb.state) {
    case 0: lb.state = 1; break;
    case 1: lastBattleList(app, true); break;
    case 2: lastBattleList(app, false); break;
    case 3: break; // a plate (lastBattleChoose)
    case 4:
        for (int w = 0; w < 4; ++w) lbWin(app, w, false);
        lb.state = 5, lb.timer = 4;
        break;
    case 5: if (lb.timer-- < 0) lb.state = 6; break;
    case 6: // Set_Prtyno, ModeCampMenu(1), SetCampMainMenu
        app.playerChar = 2 * lb.chosen;
        cr::changeRoute(app.game, &app.itemTable, lb.chosen); // (Set_Prtyno: the pair's bag)
        app.playerByReadMdl = true; // (Set_Prtyno: read_mdl)
        openCamp(app);
        lb.state = 7;
        break;
    case 7: if (!app.campOpen) lb.state = 8; break;
    case 8: lb.state = 9; break;
    case 9:
        for (int w : {0, 1, 2, 5, 6}) lbWin(app, w, true);
        lb.confirm = 0;
        lb.state = 10;
        break;
    case 10: break; // Yes / No (lastBattleAnswer)
    case 11: case 12:
        for (int w = 0; w < 7; ++w) lbWin(app, w, false);
        lb.state = 13, lb.timer = 4;
        break;
    case 13: if (lb.timer-- < 0) lb.state = 14; break;
    case 14: { // __mc_SetMenuStat(9, 4) - __menuu_LastBtClr
        app.renderer.destroy(lb.tex);
        lb.open = false;
        ++lb.count;
        lb.mask |= 1 << lb.chosen;
        app.game.vars[0] = lb.chosen;
        if (lb.count >= 4) {
            lb.count = lb.mask = 0;
            app.game.setFlag(1, false);
        }
        break;
    }
    default: break;
    }
}

void drawLastBattle(App& app) {
    App::LastBattle& lb = app.lastBattle;
    if (!lb.open || app.campOpen || !app.windowTex.id) return;
    cr::Renderer& r = app.renderer;
    cr::MenuPainter& m = app.menu;
    m.begin(r, app.windowTex, &app.font);
    // __menuu_LastBtPut: its windows sit at (0, 0) (as the camp's), so these are menu units: the
    // header (picture 109) at (8, 6), the round's title (FontDisp (12, 40)) over the rule (15 at
    // (0, 56)), the guardian at (0, 64), the rows, the board (ComUty_MsgBoardDraw: picture 12 at
    // (0, 220)) and Yes / No. The iPhone layout: all of it at its screen's offset (16, 32)
    const PhoneMenu phoneMenu(app, m);
    // Widescreen: the heading, rule and picture to the screen's left, the rows and Yes / No to its
    // right, the question centred
    r.setOrigin(wideLeft(app));
    auto ease = [](float t) { return 1 - (1 - t) * (1 - t); };
    if (lb.win[0] > 0) m.picture(109, 8, 6, lb.win[0]);
    if (lb.win[1] > 0) {
        const float dx = -140 * (1 - ease(lb.win[1]));
        m.picture(15, dx, 56, lb.win[1]);
        m.textCentred(sysText(app, 58000 + 0x68 + lb.count), dx + 12, 47, false, lb.win[1], menuFontSize(app)); // (LastBtSet 72185: 13 / 14)
    }
    if (lb.win[2] > 0 && lb.tex.id) { // sheet 1 picture 5: (0, 0, 256, 256) at (0, 64)
        const float dx = -240 * (1 - ease(lb.win[2]));
        m.imagePiece(lb.tex, dx, 64, 0, 0, 256, 256, lb.win[2]);
    }
    r.setOrigin(wideRight(app));
    if (lb.win[3] > 0)
        for (size_t i = 0; i < lb.pairs.size(); ++i) {
            using namespace lastbt;
            const float a = lb.win[3], x = kRowX + 200 * (1 - ease(a)), y = kRowY + kRowStep * float(i);
            m.picture(lb.cursor == int(i) ? 9 : 8, x, y, a);
            m.piece(x + 12, y + 8, float(4 % 16) * 32, 248 + float(4 / 16) * 32, 32, 32, a); // icon 4
            plateText(app, m, lastBattlePlate(app, lb.pairs[i]), x + 56, y + 12, 0, a);
        }
    r.setOrigin(r.uiX());
    if (lb.win[5] > 0) { // the question (ComUty_MsgBoardDraw at y 220, sliding up from 160 below)
        const float dy = 160 * (1 - ease(lb.win[5]));
        m.picture(12, 0, 220 + dy, lb.win[5]);
        m.text(sysText(app, 57000 + 0x13 + lb.count, sysText(app, 58029 + 2 * std::max(0, lb.chosen))), 24, 235 + dy,
               false, lb.win[5], 15);
    }
    if (lb.win[6] > 0) { // Yes / No (from 360 to the right)
        const float dx = 360 * (1 - ease(lb.win[6]));
        r.setOrigin(wideRight(app));
        // (__menuu_LastBtPrc's ComUty_YesNoChoiceProc: a held plate takes the cursor)
        const bool hy = pressIn(app, kPressLastBattle, wideRight(app), 336, 104, 136, 40), hn = pressIn(app, kPressLastBattle, wideRight(app), 336, 160, 136, 40);
        m.plate(336 + dx, 104, 0, 0, hy || (!hn && lb.confirm == 0), lb.win[6], false);
        m.plate(336 + dx, 160, 1, 1, hn || (!hy && lb.confirm == 1), lb.win[6], false);
    }
    r.setOrigin(r.uiX());
}
void lastBattleKey(App& app, SDL_Keycode key) {
    App::LastBattle& lb = app.lastBattle;
    if (lb.state == 3) {
        const int n = int(lb.pairs.size());
        if (n <= 0) return;
        if (key == SDLK_UP || key == SDLK_W) lb.cursor = (lb.cursor + n - 1) % n, menuSound(app, se::kListFocus);
        else if (key == SDLK_DOWN || key == SDLK_S) lb.cursor = (lb.cursor + 1) % n, menuSound(app, se::kListFocus);
        else if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE || key == SDLK_E) lastBattleChoose(app, lb.cursor);
    } else if (lb.state == 10) {
        if (key == SDLK_UP || key == SDLK_DOWN || key == SDLK_W || key == SDLK_S) lb.confirm ^= 1, menuSound(app, se::kCursor);
        else if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE || key == SDLK_E) lastBattleAnswer(app, lb.confirm == 0);
        else if (key == SDLK_ESCAPE || key == SDLK_BACKSPACE) lastBattleAnswer(app, false, true);
    }
}

void lastBattleClick(App& app, float px, float py) {
    App::LastBattle& lb = app.lastBattle;
    float lx, ly;
    app.renderer.toLogical(px, py, lx, ly);
    float mx, my;
    menuPointAt(app, lx, ly, wideRight(app), mx, my); // menu units (widescreen: the plates on the right)
    auto in = [&](float x, float y, float w, float h) { return mx >= x && mx < x + w && my >= y && my < y + h; };
    if (lb.state == 3) {
        for (size_t i = 0; i < lb.pairs.size(); ++i)
            if (in(lastbt::kRowX, lastbt::kRowY + lastbt::kRowStep * float(i), lastbt::kRowW, lastbt::kRowH)) {
                lb.cursor = int(i);
                lastBattleChoose(app, int(i));
            }
    } else if (lb.state == 10) { // the boxes (0x88 x 0x28)
        if (in(336, 104, 136, 40)) lastBattleAnswer(app, true);
        else if (in(336, 160, 136, 40)) lastBattleAnswer(app, false);
    }
}

// ---- The World Select (Util 1: __menuu_WldChoiceSet / Prc / Put / Clr) ----
//
// Windows (all at (0, 0), sliding in from an offset): 0 the title (picture 0 at (8, 4)), 1 the
// gate's picture (MAP_0n.mct, sheet 1 picture 1: 256 x 256 at (0, 64)), 2 the rule (picture 15
// at (0, 56)) under "World Select" (block 7 entry 0 at (12, 40)), 3 the rule (16 at (24, 72))
// under ">Level Select" (entry 1 at (40, 58)), 6 / 7 the lists, 8 Yes / No, 9 / 10 the message
// board at y 220. The gates (list 0, block 7 entries 2..7) follow the pair's order (0xca7d0),
// offered when their flags are set (0xca810 + k, the sixth 0xca820; none: the first alone); a
// plate (ComUty_ComPlate_Wld) is picture 8 (9 chosen), icon 0x3d at (12, 8), the name at (56,
// 12) and, for gates 0..4, four pips at (195 + 14k, 30) - picture 11 for each of the gate's
// flags set (0xc9bb0), else 10. A gate whose flag 0xca7f0 is set first asks "Skip the first
// half?" (block 6 entry 0x40); then the level bands (list 1: entries 8.., 2 + the pair's flags
// 0xca830 set); then "Is this okay?" (entry 0x12) with the gate and band. The sixth gate has no
// bands. Clr: Back sets flag 0; else var 0 = the gate, var 1 = the band, var 2 = the pair,
// flag 1 = the skip (not for gates 4 and 5).
namespace wldsel {
constexpr uint8_t kOrder[4][6] = {{0, 1, 2, 3, 4, 5}, {1, 2, 0, 3, 4, 5}, {2, 0, 1, 3, 4, 5}, {3, 2, 1, 0, 4, 5}};
constexpr short kAskFlag[4][4] = {{1122, 1142, 1162, 1182}, {1522, 1542, 1562, 1582}, {1922, 1942, 1962, 1982},
                                  {2322, 2342, 2362, 2382}};
constexpr short kGateFlag[4] = {1076, 1476, 1876, 2276}; // + gate (0..4)
constexpr short kSixthFlag[4] = {1104, 1504, 1904, 2304};
constexpr short kLevelFlag[4][8] = {{1123, 1143, 1163, 1183, 31, 31, 32, 32}, {1523, 1543, 1563, 1583, 33, 33, 34, 34},
                                    {1923, 1943, 1963, 1983, 35, 35, 36, 36}, {2323, 2343, 2363, 2383, 37, 37, 38, 38}};
constexpr short kPipFlag[5][4][4] = { // [gate][pip][pair]
    {{1122, 1522, 1922, 2322}, {1123, 1523, 1923, 2323}, {1052, 1452, 1852, 2252}, {1053, 1453, 1853, 2253}},
    {{1142, 1542, 1942, 2342}, {1143, 1543, 1943, 2343}, {1054, 1454, 1854, 2254}, {1055, 1455, 1855, 2255}},
    {{1162, 1562, 1962, 2362}, {1163, 1563, 1963, 2363}, {1056, 1456, 1856, 2256}, {1057, 1457, 1857, 2257}},
    {{1182, 1582, 1982, 2382}, {1183, 1583, 1983, 2383}, {1058, 1458, 1858, 2258}, {1059, 1459, 1859, 2259}},
    {{1205, 1605, 2005, 2405}, {1206, 1606, 2006, 2406}, {1207, 1607, 2007, 2407}, {1060, 1460, 1860, 2260}}};
constexpr float kListX = 192, kListY = 92, kListStep = 48; // list layout 0, as the shop's rows
constexpr int kRows = 4;
constexpr float kBackX = 208, kBackY = 320;

void on(App& app, int w, float x = 0, float y = 0) {
    App::WorldSelect& ws = app.worldSelect;
    ws.winTo[w] = 1, ws.fromX[w] = x, ws.fromY[w] = y;
}
void off(App& app, int w, float x = 0, float y = 0) {
    App::WorldSelect& ws = app.worldSelect;
    ws.winTo[w] = 0, ws.fromX[w] = x, ws.fromY[w] = y;
}
// __MuWldChoice_LvlToYesNoChange: the lists' windows go, Yes / No and the board come
void toYesNo(App& app, bool ask) {
    App::WorldSelect& ws = app.worldSelect;
    if (!ask) off(app, 2, 0, -140), off(app, 3, 0, -140);
    off(app, 4), off(app, 5);
    on(app, 8, 140, 0), on(app, 9, 0, 160), on(app, 10, 0, 160);
    ws.asking = ask;
    ws.confirm = 0;
}
// MuScrList_Draw's scroll bar (ComUty_GuageBarDraw) beside the rows while some are out of sight,
// as the shop's: track, the arrows (76 / 77), the knob (106)
void levelList(App& app, float from, bool fresh = false) {
    App::WorldSelect& ws = app.worldSelect;
    on(app, 3, -140, 0);
    on(app, 7, from, 0);
    if (fresh) ws.levelCursor = ws.levelTop = 0, ws.levelScroll.key = -1; // (FUN_00050934: the list built anew, focus at the top)
    ws.levelCursor = std::clamp(ws.levelCursor, 0, ws.levels - 1);
    ws.state = 6;
}
} // namespace wldsel

void openWorldSelect(App& app) {
    using namespace wldsel;
    App::WorldSelect& ws = app.worldSelect;
    for (cr::Texture& t : ws.maps) app.renderer.destroy(t);
    ws = App::WorldSelect();
    ws.pair = std::clamp(currentPair(app) - 1, 0, 3);
    unsigned mask = 0;
    for (int k = 0; k < 5; ++k)
        if (app.game.flag(kGateFlag[ws.pair] + k)) mask |= 1u << k;
    if (app.game.flag(kSixthFlag[ws.pair])) mask |= 0x20;
    if (!mask) mask = 1;
    for (int k = 0; k < 6; ++k)
        if (mask & (1u << kOrder[ws.pair][k])) ws.worlds.push_back(kOrder[ws.pair][k]);
    for (int k = 0; k < 8; ++k) ws.levels += app.game.flag(kLevelFlag[ws.pair][k]);
    ws.world = ws.worlds.empty() ? 0 : ws.worlds[0];
    for (int k = 0; k < 6; ++k) {
        cr::Image img;
        if (loadImage(app, "MAP_0" + std::to_string(k + 1) + ".mct", img)) ws.maps[k] = app.renderer.upload(img);
    }
    on(app, 0), on(app, 1), on(app, 2, -140, 0), on(app, 6, -480, 0), on(app, 4), on(app, 5);
    ws.state = 1;
    app.worldChoiceOpen = true;
    app.worldCursor = 0;
}

void closeWorldSelect(App& app) { // __menuu_WldChoiceClr
    App::WorldSelect& ws = app.worldSelect;
    for (cr::Texture& t : ws.maps) app.renderer.destroy(t);
    app.worldChoiceOpen = false;
    // (49265-49283: flags 0 / 1 only ever set here - a cancel sets 0, a skip 1, worlds 4 and 5 clear 1 -
    // and the band takes var 1's value, d1b48, whatever the world)
    if (ws.cancelled) { app.game.setFlag(0, true); return; }
    app.game.vars[0] = ws.world;
    app.game.vars[1] = ws.level; // (d1b48: 0 from the open's memset, -1 after a Back out of a band list - the last
                                 // gate, with no band list of its own, keeps it)
    app.game.vars[2] = ws.pair;
    if (ws.skip) app.game.setFlag(1, true);
    if (ws.world == 4 || ws.world == 5) app.game.setFlag(1, false);
    app.game.bands[size_t(std::clamp(ws.pair, 0, 3))] = app.game.vars[1]; // (AppCtl +0x308d0 + route)
    app.treasureReroll = true; // MuCom_BitSet(0x40000000): the next area loaded draws its boxes again
}

void worldSelectChoose(App& app, int pos) { // state 1: a gate, or Back (-1)
    using namespace wldsel;
    App::WorldSelect& ws = app.worldSelect;
    if (ws.state != 1) return;
    if (pos < 0) { // Back: everything goes
        menuSound(app, se::kBack);
        off(app, 6, 480, 0);
        for (int w = 0; w < 11; ++w)
            if (w != 6) off(app, w);
        ws.cancelled = true;
        ws.state = 9, ws.timer = 4;
        return;
    }
    if (pos >= int(ws.worlds.size())) return;
    menuSound(app, se::kChoose);
    ws.world = ws.worlds[size_t(pos)];
    off(app, 6, -480, 0);
    if (ws.world == 5) {
        toYesNo(app, false);
        ws.state = 7, ws.timer = 4;
    } else if (ws.world < 4 && app.game.flag(kAskFlag[ws.pair][ws.world])) {
        toYesNo(app, true);
        ws.state = 3, ws.timer = 4;
    } else
        levelList(app, 480, true); // (WldChoicePrc 49398)
}

void worldSelectLevel(App& app, int n) { // state 6: a band, or Back (-1)
    using namespace wldsel;
    App::WorldSelect& ws = app.worldSelect;
    if (ws.state != 6) return;
    if (n < 0) {
        menuSound(app, se::kBack);
        off(app, 3, 0, -140), off(app, 7, 480, 0), on(app, 6, -480, 0);
        ws.level = -1; // (WldChoicePrc 6, 49440-49447: d1b48 = MuScrList_GetChoiceListNo's -1)
        ws.skip = false;
        ws.state = 1;
        return;
    }
    if (n >= ws.levels) return;
    menuSound(app, se::kChoose);
    ws.level = n;
    toYesNo(app, false);
    off(app, 7, -480, 0);
    ws.state = 7, ws.timer = 4;
}

void worldSelectAnswer(App& app, bool yes, bool cancelled) { // states 4 (skip?) and 8 (okay?)
    using namespace wldsel;
    App::WorldSelect& ws = app.worldSelect;
    if (ws.state != 4 && ws.state != 8) return;
    menuSound(app, cancelled ? se::kBack : se::kChoose); // (WldChoicePrc's ComUty_YesNoChoiceProc(.., 1): the Vita's × 3)
    if (ws.state == 4) { // ComUty_YesNoChoiceProc: 2 the first box (Yes)
        off(app, 8, -360, 0), off(app, 9, 0, 160), off(app, 10, 0, 160);
        on(app, 4), on(app, 5);
        ws.skip = yes;
        levelList(app, 480, true); // (49424)
        return;
    }
    if (!yes) {
        off(app, 8, 140, 0), off(app, 9, 0, 160), off(app, 10, 0, 160);
        on(app, 2, -140, 0), on(app, 4), on(app, 5);
        if (ws.world == 5) {
            on(app, 6, -480, 0);
            ws.state = 1;
        } else
            levelList(app, -480);
        return;
    }
    off(app, 0), off(app, 1), off(app, 8), off(app, 9, 0, 160), off(app, 10, 0, 160);
    ws.state = 9, ws.timer = 4;
}

// A point in the lists' units (tex2d: menuPointAt takes off the phone layout's 16, 32 - drawWorldSelect's ownPlace)
void worldListPoint(const App& app, float px, float py, float& x, float& y) {
    float lx, ly;
    app.renderer.toLogical(px, py, lx, ly);
    menuPointAt(app, lx, ly, wideRight(app), x, y);
}
void stepWorldSelect(App& app) {
    App::WorldSelect& ws = app.worldSelect;
    if (!app.worldChoiceOpen) return;
    for (int w = 0; w < 11; ++w)
        ws.win[w] = ws.win[w] < ws.winTo[w] ? std::min(ws.winTo[w], ws.win[w] + 0.25f) : std::max(ws.winTo[w], ws.win[w] - 0.25f);
    ++ws.mark;
    // (WldChoicePrc 2 / 6: MuScrList_Proc on the list up - the held touch a frame at a time, worldSelectClick's units)
    for (int k = 0; k < 2; ++k) {
        ScrList& sl = k ? ws.levelScroll : ws.scroll;
        int& cursor = k ? ws.levelCursor : ws.cursor;
        const int n = k ? ws.levels : int(ws.worlds.size());
        const bool up = ws.state == (k ? 6 : 1);
        scrlist::ready(sl, 1, cursor);
        if (up && app.press.down && app.press.screen == kPressWorld) {
            float x, y;
            worldListPoint(app, app.press.px, app.press.py, x, y);
            const double t = sl.lastT + 1.0 / 20;
            if (sl.hold == ScrList::None) scrlist::press(sl, x, y, n, t);
            else scrlist::move(sl, x, y, n, t);
        } else if (!up) sl.hold = ScrList::None;
        if (scrlist::frame(sl, 1, cursor, n) && up) menuSound(app, se::kListFocus); // (MuScrList_Draw: __effgo(1))
        (k ? ws.levelTop : ws.top) = cursor;
    }
    switch (ws.state) {
    // (asm 0x50094 / 0x502ac: the count at 1 or more moves on at once, then counts down - with 4, one frame)
    case 3: if (ws.timer-- >= 1) ws.state = 4; break;
    case 7: if (ws.timer-- >= 1) ws.state = 8; break;
    case 9: if (--ws.timer < 0) ws.state = 10; break;
    case 10: closeWorldSelect(app); break;
    default: break;
    }
    if (ws.state == 1 && !ws.worlds.empty()) ws.world = ws.worlds[size_t(std::clamp(ws.cursor, 0, int(ws.worlds.size()) - 1))];
}

void drawWorldSelect(App& app) {
    using namespace wldsel;
    App::WorldSelect& ws = app.worldSelect;
    if (!app.worldChoiceOpen || !app.windowTex.id) return;
    cr::Renderer& r = app.renderer;
    cr::MenuPainter& m = app.menu;
    m.begin(r, app.windowTex, &app.font);
    // WldChoicePut's windows sit at (0, 0): menu units, 1.0.1's records - the title (picture 0) at
    // (8, 4), the gate's picture at (0, 64), the rules (15 / 16) at (0, 56) / (24, 72) under
    // FontDisp (12, 40) / (40, 58), the board (ComUty_MsgBoardDraw: picture 12) at (0, 220). The
    // iPhone layout: all of it at its screen's offset (16, 32) (o), the lists and Yes / No too;
    // Back is placed as tex2d + (16, 32) already
    const bool phone = r.phoneLayout();
    auto ownPlace = [&](bool own) { m.setOffset(own && phone ? 16.0f : 0.0f, own && phone ? 32.0f : 0.0f); };
    const float ox = phone ? 16.0f : 0.0f, oy = phone ? 32.0f : 0.0f;
    // Widescreen: the title, rules and the gate's picture to the screen's left, the lists (with
    // their Back) and Yes / No to its right, the board centred
    r.setOrigin(wideLeft(app));
    auto ease = [](float t) { return 1 - (1 - t) * (1 - t); };
    auto dx = [&](int w) { return ws.fromX[w] * (1 - ease(ws.win[w])); };
    auto dy = [&](int w) { return ws.fromY[w] * (1 - ease(ws.win[w])); };
    if (ws.win[1] > 0 && ws.maps[std::clamp(ws.world, 0, 5)].id) // the gate's picture
        m.imagePiece(ws.maps[std::clamp(ws.world, 0, 5)], dx(1) + ox, dy(1) + 64 + oy, 0, 0, 256, 256, ws.win[1]);
    if (ws.win[0] > 0) m.picture(0, dx(0) + 8 + ox, dy(0) + 4 + oy, ws.win[0]);
    if (ws.win[2] > 0) { // (the labels as the camp's: FontDisp's y the line box's top)
        m.picture(15, dx(2) + ox, dy(2) + 56 + oy, ws.win[2]);
        m.textCentred(sysText(app, 58000), dx(2) + 12 + ox, dy(2) + 47 + oy, false, ws.win[2], menuFontSize(app)); // (WldChoiceSet 49230: MuComFont* 13 / 14 - video 01 929 s)
    }
    if (ws.win[3] > 0) {
        m.picture(16, dx(3) + 24 + ox, dy(3) + 72 + oy, ws.win[3]);
        m.textCentred(sysText(app, 58001), dx(3) + 40 + ox, dy(3) + 65 + oy, false, ws.win[3], menuFontSize(app));
    }
    auto plate = [&](float x, float y, bool chosen, const std::string& name, int pips, float a) {
        m.picture(chosen ? 9 : 8, x, y, a);
        plateText(app, m, name, x + 56, y + 12, 0, a);
        if (pips >= 0)
            for (int k = 0; k < 4; ++k) m.picture(pips & (1 << k) ? 11 : 10, x + 195 + 14 * float(k), y + 30, a);
    };
    r.setOrigin(wideRight(app));
    // __menuu_WldChoicePut (0x4054c) draws its lists through MuScrList_Draw - mode 0 of the table at 0xbc1f0, as the
    // Camp's and the shop's: the focus row on the line at 150, the rows moving under it, cut to 76..280, the
    // pills (0x8c / 0x8d at x 280) and ComUty_GuageBarDraw's gauge at (456, 72)
    auto scrollList = [&](float sx, int n, const ScrList& sl, int cursor, auto&& row, float a) {
        const float scroll = sl.key == 1 ? float(sl.shown()) : kListStep * float(cursor);
        const int focus = sl.key == 1 ? scrlist::focusOf(sl, n) : cursor; // (__MuScrList_GetFocusPos)
        m.setClip(campui::kListTop, campui::kListBottom);
        for (int i = 0; i < n; ++i) {
            const float y = campui::kFocusY + kListStep * float(i) - scroll;
            if (y + kListStep > campui::kListTop && y < campui::kListBottom) row(i, sx, y, i == focus);
        }
        if (n > 2) {
            if (focus < 2) m.picture(0x8c, sx + 88, 112 - scroll, a);
            if (focus >= n - 2) m.picture(0x8d, sx + 88, 208 + kListStep * float(n - 1) - scroll, a);
        }
        m.clearClip();
        m.setOffset((phone ? 16.0f : 0.0f) + sx - kListX, phone ? 32.0f : 0.0f);
        drawCampScrollBar(m, scroll, n, a);
        ownPlace(true);
    };
    // (MuScrList_Proc's BackButtonProc: the Back bar lit while held - the box worldSelectClick tests)
    const bool backHeld = pressIn(app, kPressWorld, wideRight(app), kBackX - (phone ? 16 : 0), kBackY - (phone ? 32 : 0), 232, 24);
    if (ws.win[6] > 0) { // the gates
        ownPlace(true);
        const int n = int(ws.worlds.size());
        scrollList(kListX + dx(6), n, ws.scroll, ws.cursor, [&](int i, float x, float y, bool lit) {
            const int g = ws.worlds[size_t(i)];
            int pips = -1;
            if (g < 5) {
                pips = 0;
                for (int k = 0; k < 4; ++k) pips |= app.game.flag(kPipFlag[g][k][ws.pair]) ? 1 << k : 0;
            }
            plate(x, y, ws.state == 1 && lit, sysText(app, 58002 + g), pips, ws.win[6]);
            m.piece(x + 12, y + 8, float(0x3d % 16) * 32, 248 + float(0x3d / 16) * 32, 32, 32, ws.win[6]); // icon 0x3d
        }, ws.win[6]);
        ownPlace(false);
        m.backBar(kBackX + dx(6), kBackY, ws.state == 1 && backHeld, ws.win[6]);
    }
    if (ws.win[7] > 0) { // the level bands
        ownPlace(true);
        scrollList(kListX + dx(7), ws.levels, ws.levelScroll, ws.levelCursor, [&](int i, float x, float y, bool lit) {
            plate(x, y, ws.state == 6 && lit, sysText(app, 58008 + i), -1, ws.win[7]);
            m.piece(x + 12, y + 8, float(0x3d % 16) * 32, 248 + float(0x3d / 16) * 32, 32, 32, ws.win[7]);
        }, ws.win[7]);
        ownPlace(false);
        m.backBar(kBackX + dx(7), kBackY, ws.state == 6 && backHeld, ws.win[7]);
    }
    r.setOrigin(r.uiX());
    if (ws.win[9] > 0) { // the board (ComUty_MsgBoardDraw at y 220, from 160 below)
        // (ComUty_MsgBoardDraw: its mark - WldChoiceSet's PictAnimInit(d1b74, 0x30, 1, 4, ..) - and the lines in
        // the menu font)
        const float a = ws.win[9], y = 220 + oy + dy(9);
        m.picture(12, ox, y, a);
        const short* mark = cr::kMenuPicture[13];
        m.piece(458 + ox, y + 78, float(mark[0] + 20 * pictAnimFrame(ws.mark, 1, 4)), mark[1], mark[2], mark[3], a);
        std::vector<std::string> lines;
        if (ws.asking) lines = {sysText(app, 57000 + 0x40)};
        else if (ws.world == 5) lines = {sysText(app, 58002 + ws.world), sysText(app, 58018)};
        else lines = {sysText(app, 58002 + ws.world), sysText(app, 58008 + ws.level), sysText(app, 58018)};
        for (size_t k = 0; k < lines.size(); ++k) m.text(lines[k], 24 + ox, y + 15 + 16 * float(k), false, a, menuFontSize(app));
    }
    r.setOrigin(wideRight(app));
    if (ws.win[8] > 0) { // Yes / No
        ownPlace(true);
        // (FUN_00050864's ComUty_YesNoChoiceProc: a held plate takes the cursor)
        const bool hy = pressIn(app, kPressWorld, wideRight(app), 336, 104, 136, 40), hn = pressIn(app, kPressWorld, wideRight(app), 336, 160, 136, 40);
        m.plate(336 + dx(8), 104, 0, 0, hy || (!hn && ws.confirm == 0), ws.win[8], false);
        m.plate(336 + dx(8), 160, 1, 1, hn || (!hy && ws.confirm == 1), ws.win[8], false);
    }
    ownPlace(false);
    r.setOrigin(r.uiX());
}

void worldSelectKey(App& app, SDL_Keycode key) {
    App::WorldSelect& ws = app.worldSelect;
    const bool up = key == SDLK_UP || key == SDLK_W, down = key == SDLK_DOWN || key == SDLK_S;
    const bool enter = key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE || key == SDLK_E;
    const bool back = key == SDLK_ESCAPE || key == SDLK_BACKSPACE || key == SDLK_AC_BACK;
    if (ws.state == 1) {
        const int n = int(ws.worlds.size());
        if ((up || down) && n > 0) ws.cursor = (ws.cursor + (up ? n - 1 : 1)) % n, menuSound(app, se::kListFocus);
        else if (enter) { if (scrlist::rests(ws.scroll, 1)) worldSelectChoose(app, ws.cursor); } // (the Vita's: at rest)
        else if (back) worldSelectChoose(app, -1);
    } else if (ws.state == 6) {
        if (up || down) ws.levelCursor = (ws.levelCursor + (up ? ws.levels - 1 : 1)) % ws.levels, menuSound(app, se::kListFocus);
        else if (enter) { if (scrlist::rests(ws.levelScroll, 1)) worldSelectLevel(app, ws.levelCursor); }
        else if (back) worldSelectLevel(app, -1);
    } else if (ws.state == 4 || ws.state == 8) {
        if (up || down || key == SDLK_LEFT || key == SDLK_RIGHT) ws.confirm ^= 1, menuSound(app, se::kCursor);
        else if (enter) worldSelectAnswer(app, ws.confirm == 0);
        else if (back) worldSelectAnswer(app, false, true);
    }
}

void worldSelectClick(App& app, float px, float py) {
    using namespace wldsel;
    App::WorldSelect& ws = app.worldSelect;
    float lx, ly;
    app.renderer.toLogical(px, py, lx, ly);
    float mx, my;
    menuPointAt(app, lx, ly, wideRight(app), mx, my); // (widescreen: the lists and Yes / No on the right)
    auto in = [&](float x, float y, float w, float h) { return mx >= x && mx < x + w && my >= y && my < y + h; };
    if (ws.state == 1 || ws.state == 6) {
        const bool phone = app.renderer.phoneLayout(); // (Back: tex2d + (16, 32) already, see drawWorldSelect)
        if (in(kBackX - (phone ? 16 : 0), kBackY - (phone ? 32 : 0), 232, 24)) {
            if (ws.state == 1) worldSelectChoose(app, -1);
            else worldSelectLevel(app, -1);
            return;
        }
        // MuScrList_Proc (scrlist::): a drag's or the gauge's lift moves only the rows; a tap over / under the line a row
        // on, the pills' boxes to the other end, the line's row chosen once the rows rest
        const int n = ws.state == 1 ? int(ws.worlds.size()) : ws.levels;
        int& focus = ws.state == 1 ? ws.cursor : ws.levelCursor;
        ScrList& sl = ws.state == 1 ? ws.scroll : ws.levelScroll;
        scrlist::ready(sl, 1, focus);
        float x, y;
        worldListPoint(app, px, py, x, y);
        const bool tapped = scrlist::lift(sl, x, y, n, sl.lastT + 1.0 / 20);
        const int r = tapped ? scrlist::tap(sl, x, y, n, kListX) : 1;
        if (scrlist::follow(sl, focus, n)) menuSound(app, se::kListFocus);
        if (r == 2) {
            if (ws.state == 1) worldSelectChoose(app, focus);
            else worldSelectLevel(app, focus);
        }
        if (r) return;
    } else if (ws.state == 4 || ws.state == 8) {
        if (in(336, 104, 136, 40)) worldSelectAnswer(app, true);
        else if (in(336, 160, 136, 40)) worldSelectAnswer(app, false);
    }
}

// ComUty_HelpWindowDraw (0x49d2c): picture 0x11 (the 336 x 68 striped panel) at (144, 8), the "HELP" tag (0x12)
// at (144, -4), up to three lines at x 156 from y 32 / 26 / 18 (one / two / three lines: 0xbb298's 24 / 18 / 10,
// + 8), 16 apart, MuComFontDisp style 1 (dark). Part 01 1190 s: "Gene Plate used to record / Human Warrior
// abilities" on the striped panel over the Camp's top right
void drawHelpWindow(App& app, cr::MenuPainter& m, int item, float a, float dx, float dy) {
    if (item <= 0) return;
    m.picture(17, 144 + dx, 8 + dy, a);
    m.picture(18, 144 + dx, -4 + dy, a);
    std::vector<std::vector<std::string>> help;
    if (!app.systemText.message(cr::itemHelpId(item), app.language, nullptr, help) || help.empty()) return;
    const int n = std::min(3, int(help[0].size()));
    static const int kTop[4] = {0, 24, 18, 10};
    // (MuComFontDisp adds a unit to y; the lines are set at 14 in every language: _MuComFontHelpStrSet 0x52734 `mov lr, #14`.
    // The Vita's (0x8104c5a6-0x8104c5be) sets 13 for English, French and German - its layout here, as Part 03 1220 s shows)
    const float size = vitaLayout(app) && app.language >= 1 && app.language <= 3 ? 13.0f : 14.0f;
    for (int i = 0; i < n; ++i) m.text(help[0][size_t(i)], 156 + dx, float(kTop[n] + 8 + 16 * i + 1) + dy, true, a, size);
}

void drawCamp(App& app) {
    using namespace campui;
    App::Camp& c = app.camp;
    // A once-only message (the field's window): 56010 before any window is on (CampMainSet's 0 - 2, then 3); 56016 over
    // "Camp" (window 0) with the main page's others going (CampMain 5 takes 1 - 5 off, not 0). The Emergency Exit past
    // CampMain 0xd: no windows
    if ((c.notice && !c.mainGone) || (c.phase == App::Camp::Exit && c.exitT >= kCampExitGone)) return;
    cr::Renderer& r = app.renderer;
    cr::MenuPainter& m = app.menu;
    m.begin(r, app.windowTex, &app.font); // (the scene under the menu mask: drawFrame's)
    r.setOrigin(r.uiX());
    const PhoneMenu phoneMenu(app, m);
    // Widescreen: the heading, the boards and the section labels to the screen's left, the plates,
    // lists, Yes / No, the clock and Oz to its right (as the shop); Back and the message centred
    const auto left = [&] { r.setOrigin(wideLeft(app)); };
    const auto right = [&] { r.setOrigin(wideRight(app)); };
    const auto centre = [&] { r.setOrigin(r.uiX()); };
    left();
    campTrackPhase(c);
    const float ease = campEase(app);
    const bool main = c.page == App::Camp::Main;
    const bool items = c.page == App::Camp::Items;
    // Every window off the frame after: Back on the main page (CampMain 0xd), Options (CampMain 6's 4); and the first
    // Items tap's 56016 that frame, "Camp" left on (CampMain 5)
    const int offT = c.leaving ? c.leaving - 1 : c.toOptions ? c.toOptions - 1 : -1;
    const CampWin allOff = offT >= 0 ? campWinOff(offT, 1, 1) : CampWin{1, 0};
    const CampWin mainOff = main && c.mainGone ? campWinOff(c.anim, 0, 1) : CampWin{1, 0};
    const float leaveA = 1 - std::min(allOff.a, mainOff.a), leaveK = std::max(allOff.dx, mainOff.dx);
    // "Camp" (window 0): CampMain 0xd takes it off with the rest at the Emergency Exit's end (kCampExitHide), whole
    // the frame after, then 3/4 .. gone; back from Options CampMain 9 puts it on again
    const float exitOff = c.phase == App::Camp::Exit ? std::clamp(float(c.exitT - kCampExitHide - 1) / 4.0f, 0.0f, 1.0f) : 0.0f;
    m.picture(81, 8, 4, allOff.a * (1 - exitOff) * (c.headIn && main ? ease : 1.0f)); // "Camp"
    // The main screen's pieces slide away on a page (CampMain 6), before the page's own come (campEase), and stay
    // away while the page's go on Back (and from the start after 56016 took them)
    // (a window going off is still whole the frame after - ClrWin's state 5 - while its place moves a quarter)
    float out, outA;
    if (main) out = std::max(1 - ease, leaveK), outA = std::max(1 - ease, leaveA);
    else if (c.pageOut || c.mainGone) out = outA = 1;
    else out = std::min(1.0f, float(c.anim) / 4.0f), outA = std::clamp(float(c.anim - 1) / 4.0f, 0.0f, 1.0f);
    // The face boards (window 2): the main screen's and the Items member choice's (CampMainPrc 0's 3); Equip's and
    // Status's (they keep the main screen's) are their pages' below
    if (main || items) {
        const float faceK = main ? out : std::min(out, 1 - std::min(ease, campPhaseEase(app, App::Camp::Member)));
        const float faceA = main ? outA : faceK == out ? outA : faceK;
        for (int k = 0; k < campMembers(app); ++k) {
            const int ch = campCharacter(app, k);
            int hp, hpMax, mp, mpMax;
            memberHpMp(app, ch, hp, hpMax, mp, mpMax);
            drawFaceBoard(app, m, -240 * faceK, 24 + 88.0f * float(k), ch, hp, hpMax, mp, mpMax, 1 - faceA);
        }
    }
    right();
    // (a face board held: CampMain 5 sets d2e10 = 5 and CampMainPut lights the Equip plate for it, 64378)
    bool faceHeld = false;
    for (int k = 0; main && k < campMembers(app); ++k) faceHeld |= campPressIn(app, wideLeft(app), 8, 48 + 88.0f * float(k), 252, 80);
    for (int i = 0; i < campEntries(app); ++i) {
        const bool save = i == 5;
        // (lit: the keys' cursor - touch play none -, or a touch holding it: __MuCamp_NextProc 5's DAT_000d2e10)
        const bool held = main && (campPressIn(app, wideRight(app), kPlateX, kPlateY + kPlateStep * float(i), 136, 40) || (i == 1 && faceHeld));
        m.plate(kPlateX + 360 * out, kPlateY + kPlateStep * float(i), save ? 6 : plateIcon(kWords[i]), save ? -1 : kWords[i],
                (main && c.cursor == i && menuCursorLit(app)) || held, 1 - outA, !save && !kReady[i], save ? "Save" : "");
    }
    left();
    // A state change's windows (campSlide): the view left drawn first with its windows going (`ghost`), then the
    // current one with its coming. W: a window's move in the pass; P: within the page's own coming and going too
    // (campEase, from `base`)
    App::Camp::Slide& sl = c.slide;
    const bool sliding = sl.t < 100;
    bool ghost = false, ghostList = false;
    const bool curList = campScrollable(app);
    auto W = [&](App::Camp::Role role) -> CampWin {
        if (!sliding) return ghost ? CampWin{0, 0} : CampWin{1, 0};
        const App::Camp::Move& mv = ghost ? sl.out[size_t(role)] : sl.in[size_t(role)];
        if (mv.at < 0) return ghost ? CampWin{0, 0} : CampWin{1, 0};
        if (!ghost) return campWinOn(sl.t, mv.at, mv.dx);
        if (mv.cut) return sl.t >= mv.at ? CampWin{0, 0} : CampWin{1, 0};
        return campWinOff(sl.t, mv.at, mv.dx);
    };
    auto P = [&](App::Camp::Role role, float base) {
        const CampWin w = W(role);
        return CampWin{std::min(ease, w.a), base * (1 - ease) + w.dx};
    };
    auto pass = [&] {
        const int ch = campCharacter(app, c.member);
        const int item = c.list.empty() ? 0 : c.list[size_t(std::clamp(c.listCursor, 0, int(c.list.size()) - 1))];
        auto faces = [&](CampWin w) {
            if (w.a <= 0) return;
            left();
            for (int k = 0; k < campMembers(app); ++k) {
                const int fc = campCharacter(app, k);
                int hp, hpMax, mp, mpMax;
                memberHpMp(app, fc, hp, hpMax, mp, mpMax);
                drawFaceBoard(app, m, w.dx, 24 + 88.0f * float(k), fc, hp, hpMax, mp, mpMax, w.a);
            }
        };
        // Equip's and Status's face boards: their member choice's (EquipProc 1 / CampMainPrc 3's 2 take them off, 6 /
        // __MuState_ReturnCharChoice put them on); a face board's tap to Equip takes them with the main page's
        if (!main && !items) {
            if (c.phase == App::Camp::Member) faces(W(RFaces));
            else if (!ghost && c.direct) faces({1 - outA, -240 * out});
        }
        auto memberPlates = [&](bool lit, CampWin w) { // (FUN_00066a5c: window 8, word 12 + record at (0x148, 0xb0 + 0x38k))
            if (w.a <= 0) return;
            right();
            for (int k = 0; k < campMembers(app); ++k) {
                const int word = 12 + campCharacter(app, k);
                // (window 8: WinDispOn(8, 0x168) - in from 360 to the right, as the main plates go out)
                // (__MuCommon_CharChoiceProc: the plate - or the member's face board - held lights it)
                const bool held = lit && (campPressIn(app, wideRight(app), kPlateX, kMemberY + kMemberStep * float(k), 136, 40) ||
                                          campPressIn(app, wideLeft(app), 8, 48 + 88.0f * float(k), 252, 80));
                m.plate(kPlateX + w.dx, kMemberY + kMemberStep * float(k), plateIcon(word), word,
                        (lit && c.member == k && menuCursorLit(app)) || held, w.a, false);
            }
            left();
        };
        auto helpBoard = [&](int it, float e) { // HELP: the item's text across the top (MuScrList's: gone with its list)
            if (it <= 0 || e <= 0 || ghost) return;
            right();
            drawHelpWindow(app, m, it, e, 480 * (1 - e)); // (MuScrList's: in from 480 to the right with the list)
            left();
        };
        // MuScrList_Draw (0x57b28, mode 0 of the table at 0xbc1f0): row k at (x, 150 + 48k - offset), cut to y
        // 76..280, the row on the line lit; with more than two rows the down pill (0x8c) at (280, 112 - offset)
        // while the focus is one of the first two and the up pill (0x8d) under the last row while it is one of
        // the last two (taps there jump to the other end); then the gauge (window 0xc: it stays from one list to
        // the next). Part 01 1182-1210 s: the lit row on the line in the middle, the pill over the first row
        const float scroll = campScrollPx(app);
        auto scrollList = [&](float x, float e, int n, auto&& row) {
            if (e <= 0) return;
            const int focus = std::clamp(int((scroll + 28) / kRowH), 0, std::max(0, n - 1)); // (__MuScrList_GetFocusPos)
            m.setClip(kListTop, kListBottom);
            for (int k = 0; k < n; ++k) {
                const float y = kFocusY + kRowH * float(k) - scroll;
                if (y + kRowH > kListTop && y < kListBottom) row(k, x, y, k == focus);
            }
            if (n > 2) { // (MuScrList_Draw 55930-55950: at 0x118 on the list's window - the swipe's d1fa4 moves only the rows)
                if (focus < 2) m.picture(0x8c, x - c.swipe + 88, 112 - scroll, e);
                if (focus >= n - 2) m.picture(0x8d, x - c.swipe + 88, 208 + kRowH * float(n - 1) - scroll, e);
            }
            m.clearClip();
            const float gauge = ghost ? (curList ? 0.0f : e) : ghostList ? ease : e;
            if (gauge > 0) drawCampScrollBar(m, scroll, n, gauge);
        };
        switch (c.page) {
        case App::Camp::Items: {
            // A state's windows (campPhaseEase): the one left going, the current one coming
            auto itemsPhase = [&](App::Camp::Phase ph, float e, bool current) {
                if (e <= 0) return;
                if (ph == App::Camp::List) {
                    // (MuScrList_Start / DispActive: WinDispOn / Off(.., 0x1e0) - in from 480 to the right and out to it;
                    // a category's turn (campTurnCategory) moves it too, and a swipe holds it beside the finger)
                    const CampWin lw = W(RList);
                    const float le = std::min(e, lw.a);
                    if (le <= 0) return;
                    if (current && !c.pageOut) helpBoard(item, le); // (MuScrList's HELP: gone at once when the list goes)
                    right();
                    const float lx = kRowX + 480 * (1 - e) + lw.dx + c.swipe;
                    if (c.list.empty()) m.text("Nothing here.", lx + 56, kFocusY + 16, false, le, 15);
                    // MuScrList_Draw type 10 (__ChkCampItem, FUN_00056508): white for what can be used here (its table:
                    // 1 while MuCom_CheckExitItem, 3..6, 24..27), yellow for what either member wears, else grey; no
                    // count for a plate
                    scrollList(lx, le, int(c.list.size()), [&](int k, float x, float y, bool lit) {
                        const int it = c.list[size_t(k)];
                        bool worn = false;
                        for (int g = 0; g < campMembers(app); ++g) {
                            const cr::PartyMember& pm = app.game.party[size_t(campCharacter(app, g))];
                            worn |= std::find(std::begin(pm.equip), std::end(pm.equip), it) != std::end(pm.equip) || campWears(app, campCharacter(app, g), it);
                        }
                        // (55769-55772: one member in the party - the jewel member 1 wears off the count)
                        drawCampRow(app, m, x, y, it, cr::isPlate(it) ? 0 : pairHeldCount(app, it), lit, le, worn ? 3 : campUsable(app, it) ? 0 : 2);
                    });
                    left();
                } else if (ph == App::Camp::Member) memberPlates(current, {e, 360 * (1 - e)}); // (the face boards above)
                else if (ph == App::Camp::Result) // window 0x12: StateBoardDraw(12, 92, .., item), in from -140
                    drawShopBoard(app, 12 - 140 * (1 - e), 92, ch, c.resultItem, e, true, c.resultCount);
                else if (ph == App::Camp::Ask) { // (the Yes / No alone: windows 10 and 0xb, the count on 10)
                    const bool seed = item >= 3 && item <= 6;
                    right();
                    // (window 10: WinDispOn(10, 0x168) - in from 360 to the right; 0xb: WinDispOnTypeY(0xb, 0xa0) - up
                    // from 160 below)
                    const float dx = 360 * (1 - e), dy = 160 * (1 - e);
                    const float wr = wideRight(app);
                    if (seed) drawNumCount(m, c.count, campSeedLimit(app, ch, item), dx, e, current && campPressIn(app, wr, 336, 60, 36, 31),
                                           current && campPressIn(app, wr, 436, 60, 36, 31));
                    // (a held plate takes the cursor: __ComUty_YesNoChoiceProc)
                    const bool hy = current && campPressIn(app, wr, 336, 104, 136, 40), hn = current && campPressIn(app, wr, 336, 160, 136, 40);
                    m.plate(336 + dx, 104, plateIcon(0), 0, hy || (!hn && current && c.confirm == 0), e, false); // Yes
                    m.plate(336 + dx, 160, plateIcon(1), 1, hn || (!hy && current && c.confirm == 1), e, false); // No
                    centre();
                    // ComUty_MsgBoardDraw(0, 0xdc, .., 0, font 3): picture 12, the mark (picture 13, PictAnimInit(1 tick, 4
                    // frames, 20 px, out and back)) at (458, 298), the line at (24, 234) in the menu font
                    m.picture(12, 0, 220 + dy, e);
                    const short* mark = cr::kMenuPicture[13];
                    m.piece(458, 298 + dy, float(mark[0] + 20 * pictAnimFrame(c.mark, 1, 4)), mark[1], mark[2], mark[3], e);
                    if (current) m.text(sysText(app, seed ? 57077 : 57014, itemName(app, item)), 24, 235 + dy, false, e, menuFontSize(app));
                    left();
                }
            };
            if (ghost) itemsPhase(App::Camp::List, ease, false); // (the category left)
            else {
                if (c.prevPhase != c.phase && c.phaseT < 100) itemsPhase(c.prevPhase, campPhaseEase(app, c.prevPhase), false);
                itemsPhase(c.phase, std::min(ease, campPhaseEase(app, c.phase)), true);
            }
            break;
        }
        case App::Camp::Equip: {
            // (CategoryChoiceInit / End: the title from / to -140, the state board (10) -240, the lists 480 - campSlide)
            const cr::PartyMember& pm = app.game.party[size_t(ch)];
            const CampWin lw = P(RList, 480);
            const float lx = kRowX + lw.dx + c.swipe;
            if (c.phase == App::Camp::Member) memberPlates(!ghost, P(RPlates, 360)); // (the face boards; the state board comes with the slots)
            else if (c.phase == App::Camp::Slot) {
                const CampWin bw = P(RBoard, -240);
                if (bw.a > 0) drawShopBoard(app, 12 + bw.dx, 92, ch, 0, bw.a);
                right();
                // __MuCampScrListEquipInit (0x67a2c): a scrolling list of the seven - weapon, armor, jewel (else item
                // 0x1da, "Jewel"), the Human Genes and three plates (else 0x1db.., their names) - what is worn yellow
                // (__MuScrList_ScrListEquip_Main 0x579cc); the empty jewel slot white while a jewel is free for it
                // (FUN_00055768) and the empty plate slots while MuCom_GetCommonSkill finds a plate to wear, else grey
                const int jewelTone = campFreeJewel(app) ? 0 : 2, plateTone = campCommonSkill(app) ? 0 : 2;
                scrollList(lx, lw.a, kEquipSlots, [&](int slot, float x, float y, bool lit) {
                    const int worn = slot < 3 ? pm.equip[slot] : pm.genes[slot - 3];
                    const int shown = worn > 0 ? worn : slot == 2 ? 0x1da : slot >= 3 ? 0x1db + (slot - 3) : 0;
                    drawCampRow(app, m, x, y, shown, 0, lit, lw.a, worn > 0 ? 3 : shown == 0x1da ? jewelTone : slot >= 4 ? plateTone : 0);
                });
                left();
            } else if (c.slot >= 3 && c.phase == App::Camp::Entries) { // EquipProc 0x1c: the chosen plate's picture, its
                // known entries listed, Back | OK unless the member wears it
                const CampWin gw = P(RGene, -240);
                if (gw.a > 0) drawGeneBoard(app, m, 14 + gw.dx, 92, c.plate, gw.a, 2);
                right();
                scrollList(lx, lw.a, int(c.list.size()), [&](int k, float x, float y, bool lit) {
                    drawGeneAbilityRow(app, m, x, y, c.list[size_t(k)], ch, lit, lw.a);
                });
                helpBoard(item, lw.a);
                left();
            } else if (c.slot >= 3) { // a gene plate for the slot: its board (the cursor's known entries) beside the list
                const CampWin gw = P(RGene, -240);
                if (gw.a > 0) drawGeneBoard(app, m, 14 + gw.dx, 92, cr::isPlate(item) ? item : pm.genes[c.slot - 3], gw.a, 1);
                right();
                // (MuScrList_Draw type 6: yellow for a plate this member wears, grey while none of it is known)
                scrollList(lx, lw.a, int(c.list.size()), [&](int k, float x, float y, bool lit) {
                    const int it = c.list[size_t(k)];
                    if (cr::isPlate(it)) drawGeneRow(app, m, x, y, it, lit, campWears(app, ch, it), lw.a);
                    else drawCampRow(app, m, x, y, it, 0, lit, lw.a);
                });
                helpBoard(cr::isPlate(item) ? item : 0, lw.a);
            } else {
                const CampWin bw = P(RBoard, -240);
                if (bw.a > 0) drawShopBoard(app, 12 + bw.dx, 92, ch, item > 0 ? item : 0, bw.a);
                right();
                // (MuScrList_Draw type 5: yellow for what the member wears, no count)
                scrollList(lx, lw.a, int(c.list.size()), [&](int k, float x, float y, bool lit) {
                    const int it = c.list[size_t(k)];
                    const bool worn = it > 0 && std::find(std::begin(pm.equip), std::end(pm.equip), it) != std::end(pm.equip);
                    drawCampRow(app, m, x, y, it, 0, lit, lw.a, campJewelRefused(app, it) ? 2 : worn ? 3 : 0);
                });
                helpBoard(item, lw.a);
            }
            break;
        }
        case App::Camp::Genes: {
            // (Genes 2: the title, the gene board (0xb) from -140 and the plate list from 480; 7 the other way)
            const CampWin gw = P(RGene, -140), lw = P(RList, 480);
            const float lx = kRowX + lw.dx + c.swipe;
            if (c.phase == App::Camp::Entries) { // Genes 4-5: the chosen plate's picture and its known entries
                if (gw.a > 0) drawGeneBoard(app, m, 14 + gw.dx, 92, c.plate, gw.a, 2);
                right();
                scrollList(lx, lw.a, int(c.list.size()), [&](int k, float x, float y, bool lit) {
                    drawGeneAbilityRow(app, m, x, y, c.list[size_t(k)], ch, lit, lw.a);
                });
                helpBoard(item, lw.a);
                left();
                break;
            }
            if (gw.a > 0) drawGeneBoard(app, m, 14 + gw.dx, 92, item, gw.a, 1); // (browsing: the cursor's known entries by name)
            right();
            if (c.list.empty() && lw.a > 0) m.text("No gene plates.", lx + 56, kFocusY + 16, false, lw.a, 15);
            // (MuScrList_Draw type 0xb: yellow for a plate either member wears; its HELP the plate's own text)
            scrollList(lx, lw.a, int(c.list.size()), [&](int k, float x, float y, bool lit) {
                const int it = c.list[size_t(k)];
                bool worn = false;
                for (int g = 0; g < campMembers(app); ++g) worn |= campWears(app, campCharacter(app, g), it);
                drawGeneRow(app, m, x, y, it, lit, worn, lw.a);
            });
            helpBoard(item, lw.a);
            break;
        }
        case App::Camp::Status:
            if (c.phase == App::Camp::Member) memberPlates(!ghost, P(RPlates, 360)); // (the face boards; no state board)
            else { // CampMainPut case 3: the record's portrait (CampMainPrc 3's 2: j00n_L01.mct, the pair's two into
                // slots 8 / 9; picture 4, 256 x 256, at (0, 64)), then its state board at (308, 68), the seven it wears
                // or its plates' known entries; the face boards and the members' plates are off. The portrait (0xb)
                // comes from -240, the board (10) from 240 (campSlide)
                static const char* const kFaceImage[cr::kCharacters] = {"j001_L01.mct", "j002_L01.mct", "j003_L01.mct", "j004_L01.mct",
                                                                        "j005_L01.mct", "j006_L01.mct", "j007_L01.mct", "j008_L01.mct"};
                cr::Texture& art = c.statusArt[size_t(std::clamp(ch, 0, cr::kCharacters - 1))];
                if (!art.id) {
                    cr::Image img;
                    std::string name = kFaceImage[std::clamp(ch, 0, cr::kCharacters - 1)];
                    if (loadImage(app, name.substr(0, 8) + "_hd.mct", img) || loadImage(app, name, img)) art = app.renderer.upload(img);
                }
                left();
                const CampWin pw = P(RGene, -240);
                if (art.id && pw.a > 0) {
                    const float k = art.width >= 512 ? 2.0f : 1.0f;
                    m.imagePieceTo(art, pw.dx, 64, 256, 256, 0, 0, 256 * k, 256 * k, pw.a);
                }
                right();
                if (c.statusView == 0) { // (CampMainPut 3: no face, 0x668ac; at d304c + 0x134 - a finger's swipe moves it)
                    const CampWin bw = P(RBoard, 240);
                    if (bw.a > 0) drawShopBoard(app, 308 + bw.dx + c.swipe, 68, ch, 0, bw.a, false);
                } else {
                    const CampWin lw = P(RList, 480);
                    const float lx = kRowX + lw.dx + c.swipe;
                    scrollList(lx, lw.a, int(c.list.size()), [&](int k, float x, float y, bool lit) {
                        const int it = c.list[size_t(k)];
                        if (c.statusView == 2) drawGeneAbilityRow(app, m, x, y, it, ch, lit, lw.a);
                        else drawCampRow(app, m, x, y, it, 0, lit, lw.a);
                    });
                    if (c.list.empty() && lw.a > 0) m.text("-", lx + 120, kFocusY + 16, false, lw.a, 15);
                }
                left();
            }
            break;
        default: break;
        }
        centre();
        // ComUty_BackButtonDraw (after the page's own pieces: over the Status page's portrait, CampMainPut's end): the
        // arrows (mode 2) where the page turns (campArrows), Back | OK on a plate's entries, else the plain bar; none
        // during the Items Yes / No, its board and the Emergency Exit. (Window 4: CampMain 6 takes it off for Items -
        // the list brings its own and takes it with it, and its member choice puts window 4 on with the plates
        // (CampMainPrc 0's 3, off again in 4: 63451-63458) -; Equip, Genes and Status keep the main page's)
        const float barA = std::min(W(RBar).a, main    ? std::min(1 - leaveA, c.barIn ? ease : 1.0f)
                                               : items ? std::min(ease, std::max(campPhaseEase(app, App::Camp::List),
                                                                                 campPhaseEase(app, App::Camp::Member)))
                                                       : 1.0f);
        if (campBackShown(app) && barA > 0) { // (lit while a touch holds it - the boxes campClick tests: BackButtonProc's held)
            const float ux = app.renderer.uiX(), by = campBackY(app);
            const bool live = !ghost;
            if (campOkBar(app))
                m.backOkBar(kBackX, by, !live ? 0 : campPressIn(app, ux, 0xc0, by, 0x72, 34) ? 1 : campPressIn(app, ux, 0x137, by, 0x72, 34) ? 5 : 0, barA);
            else if (campArrows(app))
                m.backButton(kBackX, by, !live ? 0 : campPressIn(app, ux, 0xf8, by, 0x78, 34) ? 1 : campPressIn(app, ux, 0x175, by, 0x33, 34) ? 2
                                         : campPressIn(app, ux, 0xc0, by, 0x33, 34) ? 3 : 0, 0, barA);
            else m.backBar(kBackX, by, live && campPressIn(app, ux, kBackX, by, 256, 34), barA);
        } else if (items && !ghost && !campBackShown(app)) { // (the member choice's going as the Yes / No come)
            const float mb = std::min(ease, campPhaseEase(app, App::Camp::Member));
            if (mb > 0) m.backBar(kBackX, campBackY(app), false, mb);
        }
        // The title and section lines over everything else of the page (the section's line runs over the HELP
        // window's foot, its ">" over the state board's face), both in the menu font (MuComFontDisp of slots 0 / 2)
        left();
        // The Items page's title and section (windows 9, 0x10 / 0x11) are its list's: off from the member
        // choice to the list again (CampMainPrc 0's 2 -> 3 .. 10 -> 1). Equip's and Status's come with the member
        // (__MuEquip_CategoryChoiceInit, CampMainPrc 3's 2): none on their member choice
        const float itemsTitle = items ? std::min(ease, campPhaseEase(app, App::Camp::List)) : 0.0f;
        if (!main && (items ? itemsTitle > 0 : c.phase != App::Camp::Member)) {
            static const int kTitle[5] = {0, 58070, 58059, 58087, 58077}; // (0x46, __MuEquip_CategoryChoiceInit's 0x3b, 0x57, 0x4d)
            const float pa = items ? itemsTitle : ease;
            const CampWin tw = W(RTitle);
            const float ta = std::min(pa, tw.a), tx = -140 * (1 - pa) + tw.dx;
            if (ta > 0) {
                m.picture(15, tx, 56, ta);
                m.textCentred(sysText(app, kTitle[c.page]), 12 + tx, 47, false, ta, menuFontSize(app));
            }
            std::string section;
            if (c.page == App::Camp::Items) section = sysText(app, kCategoryLabel[c.category]);
            // EquipProc's window 0x10 (font slot 2): the Equip lists' ">Weapons" / ">Armor" / ">Jewel" (cases 0xb /
            // 0xf / 0x13: 0x49..0x4b) and a gene slot's ">Genes" (0x17: 0x4c), a plate's entries' ">Gene Details"
            // (0x1c, and the Genes page's 4: 0x4e); none on the member choice and the slot list
            // (__MuEquip_CategoryChoiceInit: the title and the name only) nor while the Genes page browses (its
            // 2: the title alone). Part 01 1190 / 1195 s: "Equip >Genes", "Equip >Gene Details"
            else if (c.page == App::Camp::Equip || c.page == App::Camp::Genes) {
                if (c.phase == App::Camp::Entries) section = sysText(app, 58078);
                else if (c.page == App::Camp::Equip && c.phase != App::Camp::Member && c.phase != App::Camp::Slot)
                    section = sysText(app, c.slot < 3 ? 58073 + c.slot : 58076);
            } else if (c.phase == App::Camp::Slot && c.statusView) section = sysText(app, 58099 + c.statusView); // (windows 0x10 / 0xf: 0x64, 0x65)
            if (!section.empty() && section[0] != '>') section = ">" + section;
            const CampWin sw = W(RSection);
            const float sa = std::min(pa, sw.a), sx = -140 * (1 - pa) + sw.dx;
            if (!section.empty() && sa > 0) { // (the Status page's only on its lists)
                m.picture(16, 24 + sx, 72, sa);
                m.textCentred(section, 40 + sx, 65, false, sa, menuFontSize(app));
            }
            // (No arrows beside the Items section: CampMainPut 0 draws windows 0x10 / 0x11 - the category's
            // line and the one it changes from - and the page turns on the Back bar's arrows)
        }
    };
    if (sliding && sl.was) {
        campSwapView(c, sl.view);
        ghost = true;
        ghostList = campScrollable(app);
        pass();
        ghost = false;
        campSwapView(c, sl.view);
    }
    pass();
    if (items && !c.pageOut && !c.mainGone && ease <= 0) { // (the main page's Back going)
        centre();
        m.backBar(kBackX, campBackY(app), false, 1 - outA);
    }
    // Window 1 (BattryAndRealTimeAndOzDraw, FUN_0004bf3c) is the main page's: CampMain's 6 takes it off for any
    // page and its 9 puts it back with the main page (Part 01 1180 s: TIME / OZ on the main page, none from 1182 s)
    right();
    if (main || (!c.pageOut && outA < 1)) drawClockAndOz(app, m, 1 - outA);
    // (the Emergency Exit's end: CampMain 9 puts it on, 0xd at once takes it off - whole at 11, then fading)
    if (c.phase == App::Camp::Exit && c.exitT > kCampExitHide && c.exitT < kCampExitGone)
        drawClockAndOz(app, m, 1 - float(c.exitT - kCampExitHide - 1) / 4.0f);
    centre();
    if (c.messageFrames > 0) --c.messageFrames;
    if (c.messageFrames > 0 && !c.message.empty()) {
        m.picture(12, 0, 220, 1);
        m.text(c.message, 24, 235, false, 1, menuFontSize(app));
    }
    left();
    const char* help = main ? "Up/Down  Enter  Esc or K: back"
                       : c.page == App::Camp::Items && c.phase == App::Camp::List ? "Up/Down  Left/Right: category  Enter  Esc: back"
                       : c.phase == App::Camp::Ask ? "Left/Right: how many  Up/Down  Enter"
                       : c.phase == App::Camp::Result ? "Enter: on"
                       : c.page == App::Camp::Status && c.phase == App::Camp::Slot ? "Left/Right: status, equipment, skills  Esc: back"
                       : c.page == App::Camp::Equip && c.phase == App::Camp::Slot && campMembers(app) > 1
                           ? "Up/Down  Left/Right: the other member  Enter  Esc: back"
                           : "Up/Down  Enter  Esc: back";
    if (keyHints(app)) // (our key hints)
        m.text(help, 20, 372, false, 0.8f, 9);
    centre();
}

// ---- The Progress Tracker (ContData_Save): the field kept when the game is left ----
bool trackerField(const App& app) { // (G+0x388 bit 6 set, bit 3 clear: on the field)
    return !app.booting && !app.titleOpen && app.havePlayer && !app.battle.active && app.mode == Mode::Maps;
}
void writeResume(App& app) {
    if (!app.persist || !trackerField(app)) return;
    cr::SaveData s = captureSave(app);
    s.game.vars[9] = 0; // (ContData_Save_set: +0x3174c, var 9, written 0 - as data.dat's)
    cr::writeSave(savePath(app, "resume.dat"), s);
}
// The Vita's (FORMATS "The Vita's code"): saved as you go, not only when the game is left - every
// script's end on the field (__EvtReturn / __EvtAbort: _ContData_Save_set_fld, then the save
// opcode 0x45) but the first after a load or a new game, which arms it, and before an encounter
// (fldencount_encount: _ContData_Save_set)
void trackerSceneEnd(App& app) {
    if (!trackerField(app)) return;
    if (!app.trackerArmed) { app.trackerArmed = true; return; }
    writeResume(app);
}

// ---- Trophies (game/trophies.hpp; docs/ENGINE.md "Trophies") ----
// CR1's only achievements are the Vita's (2.2.0 and Android have none): on whatever data is the game -
// the owner's choice, as every port's achievements. The unlocks follow the Vita's code (FUN_8106ae3a and its callers); the notification is the
// one the reference playthrough shows (the emulator's - on the Vita the system draws its own)
bool trophiesOn(const App& app) { return app.trophy.live; }

// FUN_8106ae3a: every trophy whose condition holds unlocked, kept at once (the system's record), its
// notification queued
void checkTrophies(App& app) {
    if (!trophiesOn(app)) return;
    App::Trophies& t = app.trophy;
    const std::vector<int> got = cr::trophies::check(app.game, t.flags);
    if (got.empty()) return;
    for (int n : got) {
        t.queue.push_back(n);
        t.earnedLog.push_back(n);
        SDL_Log("trophy %d unlocked", n);
    }
    saveMix(app);
}

// FUN_8106ad28: a bit of the system data (the shop's purchase, 0x2f), kept
void setTrophyBit(App& app, int bit) {
    if (!trophiesOn(app) || app.trophy.flags.has(bit)) return;
    app.trophy.flags.set(bit);
    saveMix(app);
}

// The trophy pack beside the Vita copy (sce_sys/trophy/NPWR08182_00/TROPHY.TRP): names, grades, pictures
void loadTrophyPack(App& app) {
    App::Trophies& t = app.trophy;
    if (t.packTried) return;
    t.packTried = true;
    for (const std::string& base : {app.vitaRoot + "/..", app.vitaPath, app.vitaRoot, app.vitaPath + "/.."}) {
        if (base.size() <= 3) continue;
        if (cr::trophies::readPack(base + "/sce_sys/trophy/NPWR08182_00/TROPHY.TRP", t.info)) {
            t.packRead = true;
            break;
        }
    }
    for (int n = 0; n < cr::trophies::kCount; ++n) {
        cr::Image img;
        if (cr::trophies::decodeIcon(t.info[size_t(n)], img)) t.icons[size_t(n)] = app.renderer.upload(img);
    }
}
std::string trophyName(const App& app, int n) {
    const App::Trophies& t = app.trophy;
    return t.packRead && !t.info[size_t(n)].name.empty() ? t.info[size_t(n)].name : cr::trophies::builtinName(n);
}
char trophyGrade(const App& app, int n) { return app.trophy.packRead ? app.trophy.info[size_t(n)].grade : cr::trophies::builtinGrade(n); }

// One at a time, in the order unlocked
void stepTrophies(App& app, float dt) {
    App::Trophies& t = app.trophy;
    if (!t.notice.on && !t.queue.empty()) {
        loadTrophyPack(app);
        t.notice.start(t.queue.front());
        t.queue.erase(t.queue.begin());
    }
    if (!t.hold) t.notice.step(dt);
}

// The notification (Part 1 1850.8 s, 1280 x 720): a light grey box (190, 188, 193) at 0.94 with rounded
// corners, x 720..1251, y 26..107 - right-aligned 29 from the screen's edge -, the trophy's picture
// 64 x 64 at (+15, +8), "(Bronze) <name>" and "You have earned a trophy!" in dark grey (54, 52, 56)
// from x +106, their glyphs at y +16..+40 and +46..+65 (the name cut at the box's edge); sliding in
// from the right edge and out again. Sized by the visible screen's height
void drawTrophyNotice(App& app) {
    const cr::trophies::Notice& nt = app.trophy.notice;
    const float shown = nt.shown();
    if (!nt.on || shown <= 0) return;
    cr::Renderer& r = app.renderer;
    r.setOrigin(0);
    const float R = r.layoutRight(); // (the visible screen's right edge and height, in the layout's units)
    const float T = r.layoutTop(), H = 768.0f - 2 * T, s = H / 720.0f;
    const float w = 530 * s, h = 82 * s, gap = 29 * s;
    const float x = R - gap - w + (1 - shown) * (gap + w), y = T + 26 * s;
    if (x >= R) return;
    const float g[4] = {190 / 255.0f, 188 / 255.0f, 193 / 255.0f, 0.94f}; // (translucent: (182, 181, 186) over the dark snow, (192, 182, 178) over Part 1 3075 s orange)
    const float rad = 10 * s;
    static const float kInset[5] = {1.0f, 0.55f, 0.3f, 0.13f, 0.03f}; // a quarter circle's inset, rows of rad / 5
    r.fillRect(x, y + rad, w, h - 2 * rad, g[0], g[1], g[2], g[3]);
    for (int k = 0; k < 5; ++k) {
        const float in = rad * kInset[k], dy = rad * float(k) / 5;
        r.fillRect(x + in, y + dy, w - 2 * in, rad / 5 + 0.01f, g[0], g[1], g[2], g[3]);
        r.fillRect(x + in, y + h - dy - rad / 5, w - 2 * in, rad / 5 + 0.01f, g[0], g[1], g[2], g[3]);
    }
    const int n = nt.id;
    if (n >= 0 && n < cr::trophies::kCount && app.trophy.icons[size_t(n)].id)
        r.drawQuad(app.trophy.icons[size_t(n)], x + 15 * s, y + 8 * s, 64 * s, 64 * s);
    // (Vita3K's notification, not the game's text: the CJK font in every language, as it was measured against Part 01)
    const cr::Font& font = app.fontCjk.loaded() ? app.fontCjk : app.font;
    if (!font.loaded() || n < 0 || n >= cr::trophies::kCount) return;
    cr::MenuPainter& m = app.menu;
    m.begin(r, app.windowTex, &font);
    const float ink = 54 / 255.0f;
    const float line1 = 24 * s, line2 = 21 * s; // (the glyph boxes: the name's 24 rows with its brackets, the second line's cap 15)
    std::string head = std::string(cr::trophies::gradeWord(trophyGrade(app, n))) + " " + trophyName(app, n);
    const float room = std::min(R, x + w - 8 * s) - (x + 106 * s);
    while (!head.empty() && m.textWidth(head, line1 / 2) * 2 > room) { // (cut at the box's edge, a character at a time)
        size_t cut = head.size() - 1;
        while (cut > 0 && (uint8_t(head[cut]) & 0xc0) == 0x80) --cut;
        head.erase(cut);
    }
    m.textTint(head, (x + 106 * s) / 2, (y + 8.6f * s) / 2, ink, ink, ink, 1, line1 / 2);
    m.textTint("You have earned a trophy!", (x + 106 * s) / 2, (y + 38.4f * s) / 2, ink, ink, ink, 1, line2 / 2);
}

// ---- The title's Progress Tracker offer (__menuu_TitlePrc 0xf..0x18, TitlePut windows 4..7) ----
// Start with a resume point (TitlePrc 4: MuCom_ChackInterruptData): first, while flag 0x44 is clear
// and the message hasn't been shown this launch (DAT_000d3370, outside TitleSet's memset), flags 0x44
// and 3 set and "About Progress Tracker" (MsgUty_WinOpen / SetMsg(0x24): 56036) read through
// (0xf..0x12); then (0x13, ContData_memcpy: the interrupted game in play) its pair's face boards come
// in from x -240 at (0, 24 + 88k) (window 6), its date and play time from +360 (window 5), Yes / No
// from +360 at (336, 126) / (336, 176) (window 4, ComUty_YesNoChoicePosProc(.., 0x150, 0x7e, 0x150,
// 0xb0)) and the message board up from +160 at y 220 with 57092 / 57093 (window 7). An answer sends
// the four windows out (0x15); Yes then stops the music and fades to black over 500 ms (0x16:
// __bgmgo(0x8000000), SetFade 10 frames) before the load, No waits 4 and Start goes on. The resume
// point is used up either way
void openResumePrompt(App& app) { // TitlePrc 0x13
    app.resumePrompt = 1;
    app.resumeCursor = 0;
    app.resumeWin = 0;
    app.resumeMark = 0;
}

void offerResume(App& app) {
    if (app.resumePrompt || app.resumeNotice || !app.persist) return;
    if (!cr::readSave(savePath(app, "resume.dat"), app.resumeData)) return;
    cr::SaveData d; // (an earlier release's: one pair's, folded into the one save as its newest)
    if (!app.resumeData.wholeGame && readData(app, d)) cr::foldPairSave(d, app.resumeData, &app.itemTable), app.resumeData = d;
    if (!allPairsFlags(app).flag(0x44) && !app.game.flag(0x44) && !app.interruptShown) { // 0xf
        app.game.setFlag(0x44, true);
        app.interruptShown = true;
        app.game.setFlag(3, true); // 0x10: SetFlag(3), MsgUty_WinOpen(0x24)
        app.dialogue.clear();
        menuSound(app, se::kWinOpen); // (_MsgUty_WinOpen 76973)
        showMessages(app, {kManualFirstPage + 0x24});
        app.resumeNotice = 1;
        return;
    }
    openResumePrompt(app);
}

void answerResume(App& app, bool yes, bool cancelled) {
    if (app.resumePrompt != 1) return;
    app.resumePrompt = 2; // 0x15: the windows go
    app.resumeYes = yes;
    // (cont.dat stays: only SaveData_Save and Reset Data remove it - _removeContData's callers 71862 / 70698 -
    // and Uty_LoadData reads it again at every title, so the offer comes back until the next save)
    menuSound(app, cancelled ? se::kBack : se::kChoose); // Yes and No alike (ComUty_YesNoChoicePosProc(.., 1); the Vita's × 3)
    if (!yes) { app.resumeIn = 6; return; } // 0x16: 4, counted below zero (0x17), then Start
    // Yes (0x16): the music off, black over 500 ms (10 frames), ten counted below zero, then the load
    playBgm(app, 1u << 27);
    app.fadeGrey = 0, app.fadeFrom = 0, app.fadeTo = 1, app.fadeFrames = 10, app.fadeT = 0;
    app.resumeIn = 12;
}

// The boxes 136 x 40 at (336, 126) - Yes - and (336, 176) - No (widescreen: on the screen's right, as drawn)
void resumeClick(App& app, float px, float py) {
    if (app.resumePrompt != 1) return;
    float lx, ly, mx, my;
    app.renderer.toLogical(px, py, lx, ly);
    menuPointAt(app, lx, ly, wideRight(app), mx, my);
    for (int k = 0; k < 2; ++k)
        if (mx >= 336 && mx < 336 + 136 && my >= 126 + 50 * k && my < 126 + 50 * k + 40) {
            app.resumeCursor = k;
            answerResume(app, k == 0);
            return;
        }
}

// One 20 Hz frame of the offer: the message read through (0x11: flag 3 clear, MsgUty_WinClose), the
// windows' slide (4 frames), the board's mark, the answer's countdown
void stepResume(App& app) {
    if (app.resumeNotice == 1 && app.dialogue.empty()) {
        menuSound(app, se::kWinClose); // (_MsgUty_WinClose 77165)
        app.game.setFlag(3, false);
        app.resumeNotice = 0;
        openResumePrompt(app);
    }
    if (app.resumePrompt == 1) app.resumeWin = std::min(1.0f, app.resumeWin + 0.25f);
    else if (app.resumePrompt == 2) app.resumeWin = std::max(0.0f, app.resumeWin - 0.25f);
    if (app.resumePrompt) ++app.resumeMark;
    if (app.resumeIn > 0 && --app.resumeIn == 0) {
        app.resumePrompt = 0;
        if (!app.resumeYes) titleStart(app);
        else if (!applySave(app, app.resumeData)) { menuSound(app, se::kBuzzer); titleStart(app); }
    }
}

void drawResumePrompt(App& app) {
    cr::Renderer& r = app.renderer;
    cr::MenuPainter& m = app.menu;
    m.begin(r, app.windowTex, &app.font);
    const PhoneMenu phoneMenu(app, m); // the iPhone layout: its screen's menu coordinates
    const float w = app.resumeWin, e = 1 - (1 - w) * (1 - w), a = w, out = 1 - e;
    // Window 6: the interrupted game's pair (ComUty_CharInfoInit: one while flag 0xfed + route holds
    // the second back), as its state has them
    const cr::GameState& g = app.resumeData.game;
    const int route = app.resumeData.route();
    r.setOrigin(wideLeft(app));
    for (int k = 0; k < (g.flag(4077 + route) ? 1 : 2); ++k) {
        const int ch = 2 * route + k;
        int hp, hpMax, mp, mpMax;
        memberHpMp(app, ch, hp, hpMax, mp, mpMax, &g);
        drawFaceBoard(app, m, -240 * out, 24 + 88.0f * float(k), ch, hp, hpMax, mp, mpMax, a, &g);
    }
    r.setOrigin(wideRight(app));
    drawSaveDate(app, m, app.resumeData, a, 360 * out);                    // window 5
    // (TitlePrc's ComUty_YesNoChoicePosProc: a held plate takes the cursor)
    const bool hy = pressIn(app, kPressResume, wideRight(app), 336, 126, 136, 40), hn = pressIn(app, kPressResume, wideRight(app), 336, 176, 136, 40);
    m.plate(336 + 360 * out, 126, 0, 0, hy || (!hn && app.resumeCursor == 0), a, false); // window 4: Yes
    m.plate(336 + 360 * out, 176, 1, 1, hn || (!hy && app.resumeCursor == 1), a, false); // No
    r.setOrigin(r.uiX());
    const float by = 220 + 160 * out; // window 7: ComUty_MsgBoardDraw, its mark out and back, the lines once still
    m.picture(12, 0, by, a);
    const short* mark = cr::kMenuPicture[13];
    m.piece(458, by + 78, float(mark[0] + 20 * pictAnimFrame(app.resumeMark, 1, 4)), mark[1], mark[2], mark[3], a);
    if (w >= 1) {
        m.text(sysText(app, 57092), 24, by + 15, false, a, menuFontSize(app));
        m.text(sysText(app, 57093), 24, by + 31, false, a, menuFontSize(app));
    }
}

// prty_init_new(pair - 1, mode) (0x6b82c), on the state the title runs on (TitleClr's Load_SaveData:
// data.dat, or prty_init_all's without one): a new game for the pair. Its flags 1001 + 400p ..
// 1400 + 400p are cleared, its two members start over from their records (0xcb3f0: level 1, their
// weapon, armor and jewel, the Human Genes, their own ability and skill), its bag (+0x580 + 0x100p:
// its own items and its members' gear) is emptied and holds the starting items (those and 0xce700:
// 3 Emergency Exits, 2 Chocolates). The rest of the save goes on: the Oz, the shared bag (kinds 4, 5,
// 9, 10: genes, skills, the manual, plates), the other pairs, the flags outside the range. Mode 3
// (var 1 = 3, "with your clear data", offered with the pair's clear flag 40 + p) also keeps each
// member's stat bonuses (seeds and level-up skills' points: +0x0c HP, +0x16 MP, +0x1c..+0x28),
// equipment and gene plates, and the pair's bag less its kind 3 and 9 items (the story's)
void newPairParty(App& app, int pair, bool clearData) {
    const int p = std::clamp(pair, 1, 4) - 1;
    cr::SaveData data;
    const bool haveData = readData(app, data);
    const bool ng = haveData && clearData && data.game.flag(40 + p);
    cr::GameState g = haveData ? data.game : app.game;
    if (!haveData) for (int f = 45; f <= 48; ++f) g.setFlag(f, false); // (prty_init_all: none of Uty_LoadData's cont.dat flags)
    if (haveData) {
        for (int f : {0, 4089}) g.setFlag(f, app.game.flag(f)); // the boot script's own
        // (and its variables: prty_init_new leaves the work alone - the vars the boot script has set
        // this session, the pair 0, the choice 1 and the rest, stay over the save's)
        for (const auto& [k, v] : app.game.vars) g.vars[k] = v;
        // (the pair's opened boxes, as its field flags: the areas of its story, 2 + 6p .. 7 + 6p)
        g.openedBoxes.erase(std::remove_if(g.openedBoxes.begin(), g.openedBoxes.end(),
                                           [p](int b) { return b / 1000 >= 2 + 6 * p && b / 1000 <= 7 + 6 * p; }),
                            g.openedBoxes.end());
        app.treasureBoxes.clear();
    }
    cr::changeRoute(g, &app.itemTable, p); // (Set_Prtyno: the pair's bag in view)
    for (int f = 1001 + 400 * p; f <= 1400 + 400 * p; ++f) g.setFlag(f, false);
    // ResetFieldFlag_NewGame: the pair's four blocks of field flags (3000 + 4b .. 3003 + 4b), then
    // 3300..3363
    static const int kFieldBlocks[4][4] = {{0, 5, 10, 15}, {1, 6, 11, 12}, {2, 7, 8, 13}, {3, 4, 9, 14}};
    for (int b : kFieldBlocks[p])
        for (int f = 3000 + 4 * b; f < 3004 + 4 * b; ++f) g.setFlag(f, false);
    for (int f = 3300; f < 3364; ++f) g.setFlag(f, false);
    const cr::Party fresh = cr::newParty();
    for (int k = 0; k < 2; ++k) {
        const size_t c = size_t(2 * p + k);
        cr::PartyMember m = fresh[c];
        if (ng) {
            const cr::PartyMember& old = g.party[c];
            std::copy(std::begin(old.seeds), std::end(old.seeds), m.seeds);
            std::copy(std::begin(old.boosts), std::end(old.boosts), m.boosts);
            // (the original's bonus fields hold the levels' growth too - pcparam_change_lv adds it
            // there - so it stays with the bonuses as the level goes back to 1)
            for (int st = 0; st < cr::kStats; ++st) m.boosts[st] += cr::levelGrowth(old.level, cr::Stat(st));
            std::copy(std::begin(old.equip), std::end(old.equip), m.equip);
            std::copy(std::begin(old.genes), std::end(old.genes), m.genes);
        }
        m.hp = m.mp = -1;
        g.party[c] = m;
    }
    // The pair's bag: mode 3 keeps it less kinds 3 and 9 (del_item over its slots); otherwise it is
    // emptied (memset) - its own items and its members' gear. Abilities and skills 81..88 / 131..138
    // are flags outside the cleared range, and the shared bag is not the pair's: both stay
    for (auto it = g.items.begin(); it != g.items.end();) {
        const cr::ItemHome h = cr::itemHome(&app.itemTable, it->first);
        const cr::ItemParam* ip = app.itemTable.item(it->first);
        const bool own = h == cr::ItemHome::Route || (h == cr::ItemHome::Gear && (it->first - 201) / 56 == p);
        if (own && (ng ? ip && (ip->kind == 3 || ip->kind == 9) : true)) it = g.items.erase(it);
        else ++it;
    }
    if (ng) cr::ensureStartingGenes(g);
    else cr::giveStartingItems(g);
    if (!haveData) g.walkExp = {}, g.noEncounters = {};
    if (app.interruptShown) g.setFlag(0x44, true); // (TitleClr's Load_SaveData: the Progress Tracker's message seen)
    cr::giveDataItems(g); // (_Load_SaveData: no data -> prty_init_all)
    for (int c : {2 * p, 2 * p + 1}) g.party[size_t(c)].mp = cr::memberStat(g, c, cr::kMp, app.itemTable); // (prty_init_new: MP full, as a number)
    app.game = std::move(g);
    app.playerChar = 2 * p;
}

// The flags the title and its menus run on (the Scenarios chart, the second Character Select
// sheet, the Load's emblems): the one save's (TitleClr's Load_SaveData), the game's own while one
// is on, and the prompts seen outside a save
cr::GameState allPairsFlags(const App& app) {
    cr::GameState g;
    auto take = [&](const cr::GameState& s) {
        for (size_t i = 0; i < g.flags.size() && i < s.flags.size(); ++i) g.flags[i] |= s.flags[i];
    };
    cr::SaveData d;
    if (readData(app, d)) take(d.game);
    else if (cr::readSave(savePath(app, "resume.dat"), d)) take(d.game); // (a resume point alone)
    if (!app.booting) take(app.game);
    for (int f : app.seenFlags) g.setFlag(f, true); // (the prompts seen outside a save)
    return g;
}

// ---- Scenarios (__menuu_HistorySet / HistoryPut, flow_hd.mct) ----
// A column per pair (x -2 + 256k), ten chapters each in the order that pair meets the gates, and
// the finale across the bottom. All numbers are the binary's tables: kHistoryPiece the sheet
// rectangles (gkastHistoryMct), kHistoryEntry {x, y, kind (0 banner, 1 tall, 2 wide), picture}
// (0xc8c60), kHistoryState {reached, cleared} flags (0xc8ef0), kHistoryNext {before, this}
// (0xc9110: "next" when the one before is done and this isn't begun), kHistoryPoints {flag,
// points} (0xc9260: the Achievements %).
constexpr short kHistoryPiece[38][4] = {
    {0, 934, 432, 80}, {856, 0, 72, 24}, {928, 0, 72, 24}, {840, 24, 24, 72}, {864, 24, 24, 72},
    {588, 472, 300, 84}, {588, 560, 300, 84}, {588, 648, 300, 84}, {588, 736, 300, 84}, {588, 824, 300, 84},
    {588, 912, 300, 84}, {528, 160, 264, 80}, {528, 320, 264, 152}, {0, 776, 26, 152}, {204, 776, 168, 152},
    {546, 776, 26, 152}, {28, 776, 80, 152}, {0, 0, 264, 80}, {0, 80, 264, 80}, {0, 160, 264, 80},
    {0, 240, 264, 80}, {528, 0, 264, 80}, {264, 0, 264, 80}, {264, 80, 264, 80}, {264, 160, 264, 80},
    {264, 240, 264, 80}, {528, 80, 264, 80}, {0, 320, 264, 152}, {264, 320, 264, 152}, {0, 472, 588, 148},
    {0, 624, 588, 148}, {832, 0, 18, 20}, {792, 0, 40, 48}, {528, 240, 136, 80}, {792, 320, 136, 152},
    {792, 128, 192, 152}, {720, 280, 304, 40}, {976, 464, 48, 56}};
// 2.2.0's flow_hd.mct (its table at 0xbac00, in tex2d: these doubled) is the same sheet but for the
// finale's "?" bar: wider caps 13 / 15 and a narrower body 16
constexpr short kPhoneDataHistoryPiece[3][4] = {{0, 776, 144, 152}, {432, 776, 144, 152}, {144, 776, 72, 152}};
constexpr short kHistoryEntry[41][4] = {
    {-2, 96, 0, 17}, {-2, 192, 0, 18}, {-2, 288, 0, 25}, {-2, 384, 0, 19}, {-2, 480, 0, 20}, {-2, 576, 0, 23},
    {-2, 672, 1, 27}, {-2, 840, 0, 21}, {-2, 936, 1, 28}, {-2, 1272, 0, 26},
    {254, 96, 0, 18}, {254, 192, 0, 19}, {254, 288, 0, 24}, {254, 384, 0, 17}, {254, 480, 0, 20}, {254, 576, 0, 22},
    {254, 672, 1, 27}, {254, 840, 0, 21}, {254, 936, 1, 28}, {254, 1272, 0, 26},
    {510, 96, 0, 20}, {510, 192, 0, 19}, {510, 288, 0, 22}, {510, 384, 0, 18}, {510, 480, 0, 17}, {510, 576, 0, 25},
    {510, 672, 1, 27}, {510, 840, 0, 21}, {510, 936, 1, 28}, {510, 1272, 0, 26},
    {766, 96, 0, 19}, {766, 192, 0, 17}, {766, 288, 0, 23}, {766, 384, 0, 18}, {766, 480, 0, 20}, {766, 576, 0, 24},
    {766, 672, 1, 27}, {766, 840, 0, 21}, {766, 936, 1, 28}, {766, 1272, 0, 26},
    {-2, 1104, 2, 29}};
constexpr short kHistoryState[41][2] = {
    {1092, 1093}, {1094, 1095}, {1096, 1097}, {1098, 1099}, {1100, 1101}, {1101, 1102}, {1103, 1104}, {1109, 1207}, {1207, 1207}, {-1, -1},
    {1492, 1493}, {1494, 1495}, {1496, 1497}, {1498, 1499}, {1500, 1501}, {1501, 1502}, {1503, 1504}, {1509, 1607}, {1607, 1607}, {-1, -1},
    {2292, 2293}, {2294, 2295}, {2296, 2297}, {2298, 2299}, {2300, 2301}, {2301, 2302}, {2303, 2304}, {2309, 2407}, {2407, 2407}, {-1, -1},
    {1892, 1893}, {1894, 1895}, {1896, 1897}, {1898, 1899}, {1900, 1901}, {1901, 1902}, {1903, 1904}, {1909, 2007}, {2007, 2007}, {-1, -1},
    {-1, -1}};
constexpr short kHistoryNext[41][2] = {
    {-1, -1}, {1093, 1094}, {1095, 1096}, {1097, 1098}, {1099, 1100}, {1101, 1101}, {1102, 1103}, {1104, 1109}, {1107, 1207}, {-1, -1},
    {-1, -1}, {1493, 1494}, {1495, 1496}, {1497, 1498}, {1499, 1500}, {1501, 1501}, {1502, 1503}, {1504, 1509}, {1507, 1607}, {-1, -1},
    {-1, -1}, {2293, 2294}, {2295, 2296}, {2297, 2298}, {2299, 2300}, {2301, 2301}, {2302, 2303}, {2304, 2309}, {2307, 2407}, {-1, -1},
    {-1, -1}, {1993, 1894}, {1995, 1896}, {1897, 1898}, {1899, 1900}, {1901, 1901}, {1902, 1903}, {1904, 1909}, {1907, 2007}, {-1, -1},
    {-1, -1}};
constexpr short kHistoryPoints[41][2] = {
    {1093, 3}, {1095, 3}, {1097, 2}, {1099, 3}, {1101, 3}, {1102, 2}, {1104, 3}, {1207, 3}, {32, 0}, {1061, 2},
    {1493, 3}, {1495, 3}, {1497, 2}, {1499, 3}, {1501, 3}, {1502, 2}, {1504, 3}, {1607, 3}, {34, 0}, {1461, 2},
    {2293, 3}, {2295, 3}, {2297, 2}, {2299, 3}, {2301, 3}, {2302, 2}, {2304, 3}, {2407, 3}, {38, 0}, {2261, 2},
    {1893, 3}, {1895, 3}, {1897, 2}, {1899, 3}, {1901, 3}, {1902, 2}, {1904, 3}, {2007, 3}, {36, 0}, {1861, 2},
    {39, 12}};
constexpr short kHistoryCleared[4] = {32, 34, 38, 36}; // each pair's story done; 39: all of them
constexpr float kHistoryTop = 32, kHistoryBottom = 634;  // between the black bands
// 2.2.0's (__menuu_HistoryPut, 0xbe3d0 in its binary; tex2d): the same chart at half scale, the
// columns 152 apart and scrolling sideways too (0..-120); y from the scroll + 62; black bands
// (0, 0, 480, 48) and (0, 285, 480, 35); the heading (8, 4), the clock and Oz, the arrows
// (218, 92) / (218, 276) / (0, 174) / (469, 174), "Achievements" (4, 296) and its digits from
// (66, 288), 24 apart; Back
constexpr short kPhoneHistoryEntry[41][2] = {
    {6, 48}, {6, 96}, {6, 144}, {6, 192}, {6, 240}, {6, 288}, {6, 336}, {6, 420}, {6, 468}, {6, 636},
    {158, 48}, {158, 96}, {158, 144}, {158, 192}, {158, 240}, {158, 288}, {158, 336}, {158, 420}, {158, 468}, {158, 636},
    {310, 48}, {310, 96}, {310, 144}, {310, 192}, {310, 240}, {310, 288}, {310, 336}, {310, 420}, {310, 468}, {310, 636},
    {462, 48}, {462, 96}, {462, 144}, {462, 192}, {462, 240}, {462, 288}, {462, 336}, {462, 420}, {462, 468}, {462, 636},
    {6, 552}};

void openScenarios(App& app) {
    app.scenariosOpen = true;
    app.scenariosScroll = 0;
    app.scenariosHScroll = 0;
    app.scenariosAnim = app.scenariosOut = app.scenariosBob = 0, app.scenariosBobDir = 1;
    app.scenariosFlags = allPairsFlags(app);
    if (!app.scenariosSheet.id) {
        cr::Image img;
        if (loadImage(app, "flow_hd.mct", img)) app.scenariosSheet = app.renderer.upload(img);
    }
}

void closeScenarios(App& app) {
    app.scenariosOpen = false;
    app.scenariosOut = 0;
    app.renderer.destroy(app.scenariosSheet);
}
// Back (__menuu_HistoryPrc 1, 77757-77779): __effgo(3), the window off (WinDispOff); 2 counts 4 down past 0, 3 a frame
// on leaves (__mc_SetMenuStat(0xe, 4)) - gone 7 frames after the tap
void scenariosBack(App& app) {
    if (!app.scenariosOpen || app.scenariosOut) return;
    menuSound(app, se::kBack);
    app.scenariosOut = 1;
}
// A frame (20 Hz): the window's moves, the arrows' bob (HistorySet 77553-77563: ComUty_PictMoveInit(.., 1, dx, dy, 4, ..)
// for each - ComUty_PictMoveDraw 46240-46256 moves the count a unit a draw, 0 up to 4 and back), Back's wait
void stepScenarios(App& app) {
    if (!app.scenariosOpen) return;
    ++app.scenariosAnim;
    app.scenariosBob += app.scenariosBobDir;
    if (app.scenariosBob >= 4 || app.scenariosBob <= 0) app.scenariosBobDir = -app.scenariosBobDir;
    if (app.scenariosOut && ++app.scenariosOut > 7) closeScenarios(app);
}

// 0 locked, 1 next, 2 reached, 3 cleared (HistorySet)
int scenarioState(const cr::GameState& g, int i) {
    auto f = [&](int n) { return n >= 0 && g.flag(n); };
    if (i == 40) { // the finale: all four stories done, cleared with 39
        if (g.flag(39)) return 3;
        for (short c : kHistoryCleared)
            if (!g.flag(c)) return 0;
        return 2;
    }
    if (i % 10 == 9) { // the bonus chapters (HistorySet 0x7bb5c..0x7bbc0, 0xbe7b0 {39, 10 flags}): cleared by the
        // last flag, reached with 39 and the nine before it
        static constexpr short kFirst[4] = {1052, 1452, 2252, 1852};
        const int b = kFirst[i / 10];
        if (g.flag(b + 9)) return 3;
        if (!g.flag(39)) return 0;
        for (int n = b; n < b + 9; ++n)
            if (!g.flag(n)) return 0;
        return 2;
    }
    int st = f(kHistoryState[i][1]) ? 3 : f(kHistoryState[i][0]) ? 2 : 0;
    if (f(kHistoryNext[i][0]) && !f(kHistoryNext[i][1])) st = 1;
    return st;
}

// HistorySet (77639-77645): before flag 39 the bonus row has no way-in marks (+0xd37d8 = 1) and the chart
// stops above it, -407 (tex2d; -453 with 39)
bool scenarioNoMark(const cr::GameState& g, int i) { return i < 40 && i % 10 == 9 && !g.flag(39); }
float scenarioLowest(const App& app) {
    const bool all = app.scenariosFlags.flag(39);
    return app.renderer.phoneLayout() ? 2.0f * (all ? -453 : -407) : kHistoryBottom - (1272 + 40 + 80) - 16 + (all ? 0 : 92);
}

int scenarioPercent(const cr::GameState& g) {
    int p = 0;
    for (const auto& e : kHistoryPoints)
        if (g.flag(e[0])) p += e[1];
    return std::min(p, 100);
}

// The History's Back at a touch (the frame's pixels): __menuu_HistoryPrc's ComUty_BackButtonProc(1) - (192, 286,
// 256 x 34) in the iPhone layout; the iPad layout's bar at (272, 330)
bool historyBackAt(const App& app, float px, float py) {
    float lx, ly, mx, my;
    app.renderer.toLogical(px, py, lx, ly);
    menuPoint(app, lx, ly, mx, my);
    const bool phone = app.renderer.phoneLayout();
    const float x = phone ? 192 : 272, y = phone ? 286 : 330, w = phone ? 256 : 232, h = phone ? 34 : 24;
    return mx >= x && mx < x + w && my >= y && my < y + h;
}
// Where a drag may start (HistoryPrc 1: ComUty_PadPhotoProc(.., 0, 0x3e, 0x1e0, 0xdf) - tex2d (0, 62)-(480, 285) in the
// iPhone layout; the iPad layout's chart between its black bands)
bool historyDragAt(const App& app, float px, float py) {
    float lx, ly, mx, my;
    app.renderer.toLogical(px, py, lx, ly);
    menuPoint(app, lx, ly, mx, my);
    if (app.renderer.phoneLayout()) return mx >= 0 && mx < 480 && my >= 62 && my < 62 + 223;
    return ly >= kHistoryTop && ly < kHistoryBottom;
}

void drawScenarios(App& app) {
    cr::Renderer& r = app.renderer;
    const cr::Texture& t = app.scenariosSheet;
    r.setOrigin(0);
    r.fillRect(0, 0, r.screenW(), cr::Renderer::kLogicalH, 0, 0, 0, 1);
    r.setOrigin(r.uiX());
    if (!t.id) return;
    // (HistoryPrc 0: WinDispOn the frame after HistorySet - in over 4 frames; Back's WinDispOff: whole a frame, then out)
    const float a = app.scenariosOut ? campWinOff(app.scenariosOut - 1, 0, 0).a : campWinOn(app.scenariosAnim, 1, 0).a;
    const float bob = float(app.scenariosBob); // (the arrows: up -1, down +1, left -1, right +1 x the count, tex2d)
    auto rect = [&](int n) -> const short* {
        if (app.phoneData && (n == 13 || n == 15 || n == 16)) return kPhoneDataHistoryPiece[n == 13 ? 0 : n == 15 ? 1 : 2];
        return kHistoryPiece[n];
    };
    auto piece = [&](int n, float x, float y, float alpha = -1) {
        const short* p = rect(n);
        r.drawQuadUV(t, x, y, p[2], p[3], p[0], p[1], p[2], p[3], 1, 1, 1, alpha < 0 ? a : alpha);
    };
    // The finale's long "?" (not yet open): cap 13, body 16 repeated up to the mark 14 at mid, body
    // again up to cap 15 at right
    auto bar = [&](float x, float y, float mid, float right) {
        piece(13, x, y);
        auto body = [&](float from, float to) {
            const short* m = rect(16);
            for (float px = from; px < to; px += m[2]) {
                const float w = std::min(float(m[2]), to - px);
                r.drawQuadUV(t, px, y, w, m[3], m[0], m[1], w, m[3], 1, 1, 1, a);
            }
        };
        body(x + rect(13)[2], mid);
        piece(14, mid, y);
        body(mid + rect(14)[2], right);
        piece(15, right, y);
    };
    const cr::GameState& g = app.scenariosFlags;
    const bool laterPairs = g.flag(32) || g.flag(34); // +0x20: the third and fourth pairs shown
    if (r.phoneLayout()) { // 2.2.0's iPhone chart (kPhoneHistoryEntry), tex2d -> layout
        auto at = [&](int n, float tx, float ty, float alpha = -1) { piece(n, 2 * tx + 32, 2 * ty + 64, alpha); };
        const float hs = app.scenariosHScroll, vs = app.scenariosScroll / 2;
        // (z: the entries -8 under the pairs' strips -4 and the way-in locks -4; the bands -3)
        std::vector<std::array<float, 3>> over;
        for (int i = 0; i < 41; ++i) {
            const short* e = kHistoryEntry[i];
            const float tx = hs + kPhoneHistoryEntry[i][0], ty = vs + 62 + kPhoneHistoryEntry[i][1];
            const float x = 2 * tx + 32, y = 2 * ty + 64;
            const float h = e[2] == 0 ? 80.0f : e[2] == 1 ? 152.0f : 148.0f;
            if (y > 2 * 285 + 64 || y + h < 2 * 48 + 64) continue;
            const int st = scenarioState(g, i);
            if (e[2] == 2) { // the finale: its two halves (29 / 30), or the "?" across all four columns
                if (st >= 2) piece(29, x, y), piece(30, x + 588, y);
                else bar(x, y, x + 504, x + 1032);
                static constexpr short kShut[4] = {60, 212, 365, 516}, kOpen[4] = {67, 219, 371, 523};
                for (int k = 0; k < 4; ++k)
                    if (st == 0 && !g.flag(kHistoryCleared[k])) at(32, tx - 6 + kShut[k], ty - 17);
                    else at(31, tx - 6 + kOpen[k], ty - 10);
            } else {
                piece(st <= 1 ? (e[2] == 1 ? 12 : 11) : e[3], x, y);
                if (scenarioNoMark(g, i)) {
                } else if (st != 0) at(31, tx + 61, ty - 10);
                else if (ty - 17 < 91) over.push_back({31, tx + 61, ty - 10}); // (by the strips: the open one)
                else if (ty - 17 < 280) over.push_back({32, tx + 54, ty - 17});
                else at(32, tx + 54, ty - 17);
            }
            if (st == 3) at(e[2] == 0 ? 33 : e[2] == 1 ? 34 : 35, tx + (e[2] == 2 ? 244 : 66), ty);
        }
        for (const auto& o : over) at(int(o[0]), o[1], o[2]);
        // (0, 48)-(480, 64): an unseen quad at z -4 (tex2d_add4, alpha 0) that keeps the chart out of the
        // gap above the strips
        r.fillRect(32, 2 * 48 + 64, 960, 32, 0, 0, 0, 1);
        for (int k = 0; k < 4; ++k) at(k >= 2 && !laterPairs ? 7 + k : 5 + k, hs + 150.0f * float(k), 58);
        r.fillRect(32, 64, 960, 96, 0, 0, 0, 1);               // (0, 0, 480, 48)
        r.fillRect(32, 2 * 285 + 64, 960, 70, 0, 0, 0, 1);      // (0, 285, 480, 35)
        at(0, 8, 4);
        if (vs < 0) at(1, 218, 92 - bob);
        if (2 * vs > scenarioLowest(app)) at(2, 218, 276 + bob);
        if (hs < 0) at(3, 0 - bob, 174);
        if (hs > -120) at(4, 469 + bob, 174);
        at(36, 4, 296);
        const int pct = scenarioPercent(g);
        const int digits[3] = {pct / 100, pct / 10 % 10, pct % 10};
        for (int k = 0; k < 3; ++k) {
            if ((k == 0 && pct < 100) || (k == 1 && pct < 10)) continue;
            const short* d = kHistoryPiece[37];
            r.drawQuadUV(t, 2 * (66 + 24.0f * float(k)) + 32, 2 * 288 + 64, d[2], d[3], d[0], d[1] + d[3] * float(digits[k]), d[2], d[3],
                         1, 1, 1, a);
        }
        cr::MenuPainter& m = app.menu;
        m.begin(r, app.windowTex, &app.font);
        const PhoneMenu phoneMenu(app, m);
        drawClockAndOz(app, m, a);
        m.backBar(192, 286, app.press.down && app.press.screen == kPressHistory && historyBackAt(app, app.press.px, app.press.py), a); // (HistoryPut: BackButtonDraw2 mode 1; lit while held - DAT_000d37b8)
        return;
    }
    // (z: the entries -8 under the pairs' strips -4; the way-in locks -4 over them while above y 688;
    // the bands -3)
    std::vector<std::array<float, 3>> over;
    const float sy = app.scenariosScroll;
    for (int i = 0; i < 41; ++i) {
        const short* e = kHistoryEntry[i];
        const float x = e[0] - 32, y = e[1] + 40 + sy;
        const float h = e[2] == 0 ? 80.0f : e[2] == 1 ? 152.0f : 148.0f;
        if (y > kHistoryBottom || y + h < kHistoryTop) continue;
        const int st = scenarioState(g, i);
        if (e[2] == 2) { // the wide finale: its two halves (29 / 30, past the screen's sides), or the long "?"
            if (st >= 2) { // (kept to the 1024 layout: wider screens show past it)
                auto half = [&](int n, float hx) {
                    const short* p = rect(n);
                    const float l = std::max(hx, 0.0f), rr = std::min(hx + p[2], 1024.0f);
                    if (rr > l) r.drawQuadUV(t, l, y, rr - l, p[3], p[0] + (l - hx), p[1], rr - l, p[3], 1, 1, 1, a);
                };
                half(29, x - 72), half(30, x + 516);
            } else bar(x + 4, y, x + 430, x + 998);
            for (int k = 0; k < 4; ++k) // the four stories leading in: open or shut
                if (st == 0 && !g.flag(kHistoryCleared[k])) piece(32, x + 108 + 256.0f * float(k), y - 34);
                else piece(31, x + 121 + 256.0f * float(k), y - 20);
        } else {
            piece(st <= 1 ? (e[2] == 1 ? 12 : 11) : e[3], x, y);
            if (scenarioNoMark(g, i)) {
            } else if (st != 0) piece(31, x + 122, y - 20); // the way in: open / shut (the open one by the strips)
            else if (y - 34 < 97) over.push_back({31, x + 122, y - 20});
            else if (y - 34 < 688) over.push_back({32, x + 108, y - 34});
            else piece(32, x + 108, y - 34);
        }
        if (st == 3) piece(e[2] == 0 ? 33 : e[2] == 1 ? 34 : 35, x + (e[2] == 2 ? 416 : 128), y); // cleared: the star
    }
    for (int k = 0; k < 4; ++k) // the pairs' strips over their columns (5..8; 9 / 10 not yet open)
        piece(k >= 2 && !laterPairs ? 7 + k : 5 + k, -54 + 256.0f * float(k), kHistoryTop);
    for (const auto& o : over) piece(int(o[0]), o[1], o[2]);
    // The black bands, the heading, the arrows while there's more, Achievements %
    r.fillRect(-32, -64, 1088, 96, 0, 0, 0, 1);
    r.fillRect(-32, kHistoryBottom, 1088, 768 - kHistoryBottom, 0, 0, 0, 1);
    piece(0, 8, -12); // ("Scenarios": the binary's -28 clips the word on this sheet)
    if (sy < 0) piece(1, 436, 100 - 2 * bob);
    if (sy > kHistoryBottom - (1272 + 40 + 80)) piece(2, 436, 616 + 2 * bob);
    piece(36, 8, 656);
    const int pct = scenarioPercent(g);
    const int digits[3] = {pct / 100, pct / 10 % 10, pct % 10};
    for (int k = 0; k < 3; ++k) {
        if (k == 0 && pct < 100) continue;
        if (k == 1 && pct < 10) continue;
        const short* d = kHistoryPiece[37];
        r.drawQuadUV(t, 132 + 48.0f * float(k), 640, d[2], d[3], d[0], d[1] + d[3] * float(digits[k]), d[2], d[3], 1, 1, 1, a);
    }
    cr::MenuPainter& m = app.menu;
    m.begin(r, app.windowTex, &app.font);
    m.backBar(272, 330, app.press.down && app.press.screen == kPressHistory && historyBackAt(app, app.press.px, app.press.py), a);
    if (keyHints(app)) // (our key hints)
        m.text("Up/Down: scroll  Esc: back", 300, 372, false, a * 0.8f, 9);
}

void scenariosScrollBy(App& app, float d) {
    const float lowest = scenarioLowest(app);
    app.scenariosScroll = std::clamp(app.scenariosScroll + d, lowest, 0.0f);
}
void scenariosScrollSideways(App& app, float d) { // the iPhone layout's chart (tex2d)
    if (app.renderer.phoneLayout()) app.scenariosHScroll = std::clamp(app.scenariosHScroll + d, -120.0f, 0.0f);
}

void scenariosKey(App& app, SDL_Keycode key) {
    if (app.scenariosOut) return; // (HistoryPrc 2 / 3: going)
    // (a controller pans in padStep, held; its confirm doesn't close - the Vita's Back takes x or a touch, BackButtonProc 1)
    if (app.padKeyNow && (key == SDLK_UP || key == SDLK_DOWN || key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_RETURN)) return;
    switch (key) {
    case SDLK_UP: case SDLK_W: scenariosScrollBy(app, 96); break;
    case SDLK_DOWN: case SDLK_S: scenariosScrollBy(app, -96); break;
    case SDLK_PAGEUP: scenariosScrollBy(app, 480); break;
    case SDLK_PAGEDOWN: scenariosScrollBy(app, -480); break;
    case SDLK_LEFT: case SDLK_A: scenariosScrollSideways(app, 40); break;
    case SDLK_RIGHT: case SDLK_D: scenariosScrollSideways(app, -40); break;
    case SDLK_ESCAPE: case SDLK_BACKSPACE: case SDLK_AC_BACK: case SDLK_RETURN: scenariosBack(app); break;
    default: break;
    }
}

// ---- The Instruction Manual (Options; MuScrList + MsgUty: the topics, then the page) ----
// __menuu_OptionPrc (0x6fdbc) 0xf..0x16, 2.2.0 and 1.0.1 alike: the topics are the manual items the
// bag holds (MuComMakeItemHaveData(0, 9) - make_item_list kind 9, 439..478, given as the story goes),
// by name; each one's page is MsgUty_SetMsg's block 5 entry item - 439 (0x12: item - 0x1b7 while it
// is below 0x1b7 + 0x28), system message 56000 + (item - 439). (The port had listed 56000..56036 as
// fixed topics with 56007 first and the page in a side panel.) 56035 "Option" is OptionSet's, through
// the message window: openOptions

std::vector<int> manualTopics(const App& app) { // the items
    std::vector<int> items;
    for (int item = kManualFirstItem; item <= kManualLastItem; ++item)
        if (ownedCount(app, item) > 0) items.push_back(item);
    for (int item : {501, 502}) // (make_item_list kind 9, 60556-60570: then 0x1f5 / 0x1f6 - "Option", "Progress Tracker")
        if (ownedCount(app, item) > 0) items.push_back(item);
    return items;
}

std::vector<std::string> manualPage(const App& app, int id) { // the lines after the title
    std::vector<std::vector<std::string>> pages;
    std::vector<std::string> lines;
    if (!app.systemText.message(id, app.language, nullptr, pages)) return lines;
    for (const auto& pg : pages)
        for (const std::string& l : pg) lines.push_back(l);
    while (!lines.empty() && lines.front().empty()) lines.erase(lines.begin());
    if (!lines.empty()) lines.erase(lines.begin()); // the title
    while (!lines.empty() && lines.front().empty()) lines.erase(lines.begin());
    return lines;
}

// A topic's page (OptionPrc 0x12..0x16): SetFlag(3), MsgUty_WinOpen (0x7b1c4: _SetWin style 8 - the
// tutorial band - at _mwin[4] (tex2d y 16, the picture below it) for an id in MsgUty_SetMsg's table,
// else _mwin[5] (y 108); the port's msgLines 2 / 1 pick those rects - SE 6) and MsgUty_SetMsg
// (FUN_0007b304: block 5's entry), paged by tap (PrcMsg). Its table (0xbe100, {id, picture}: {12, 8},
// {13, 9}, {18, 10}, {10, 11}, {19, 13}) gives five of them a picture (MsgUty_TexDispInit: the rows at
// 0xbe280 {slot 3, pages, (row, frames) x 4, x 144, y 160}; pictures 8..11 and 13 the battle_01..03,
// gene_01 and battle_04 sheets, Uty_TutoTexDisp's): it shows from the first page turn, a page of the
// picture a page of text, until its pages run out or the message ends. The last page out, the window
// closes (MsgUty_WinClose: ClrWin, SE 7) and once gone the list is back at that item (0x16 -> 0xf:
// MuScrList_SetListPosFromItemNo)
namespace manualpic {
struct Row { int id; const char* sheet; int pages; int row[4], frames[4]; };
constexpr Row kRows[5] = {
    {12, "battle_01_hd.mct", 4, {0, 1, 2, 3}, {1, 4, 4, 1}}, {13, "battle_02_hd.mct", 4, {0, 1, 2, 3}, {1, 1, 2, 1}},
    {18, "battle_03_hd.mct", 3, {0, 1, 2, -1}, {3, 1, 1, -1}}, {10, "gene_01_hd.mct", 3, {0, 1, 2, -1}, {1, 2, 1, -1}},
    {19, "battle_04_hd.mct", 2, {0, 1, -1, -1}, {1, 1, -1, -1}}};
constexpr float kX = 144, kY = 160;
const Row* find(int id) {
    for (const Row& r : kRows)
        if (kManualFirstPage + r.id == id) return &r;
    return nullptr;
}
} // namespace manualpic

void manualShow(App& app, int id) {
    App::ManualMsg& mm = app.manualMsg;
    app.renderer.destroy(mm.pic.tex);
    mm.pic = App::TutoTex();
    mm.id = id;
    mm.pages.clear();
    if (!app.systemText.message(id, app.language, nullptr, mm.pages) || mm.pages.empty())
        mm.pages = {{"[message " + std::to_string(id) + "]"}};
    mm.page = 0, mm.typed = 0;
    mm.win = App::MsgWin();
    mm.win.state = 1;
    menuSound(app, se::kWinOpen);
    const manualpic::Row* row = manualpic::find(id);
    mm.picture = row != nullptr;
    if (row) {
        App::TutoTex& t = mm.pic;
        cr::Image img;
        std::string name = row->sheet;
        if (loadImage(app, name, img) || loadImage(app, name.replace(name.find("_hd"), 3, ""), img)) t.tex = app.renderer.upload(img);
        t.on = true, t.state = 2, t.pages = row->pages;
        for (int k = 0; k < 4; ++k) t.row[k] = row->row[k], t.frames[k] = row->frames[k];
        t.x = manualpic::kX, t.y = manualpic::kY;
    }
    mm.state = 2;
}

void manualCloseMessage(App& app) { // the message done: ClrFlag(3) - the picture goes - and the window closes
    App::ManualMsg& mm = app.manualMsg;
    app.renderer.destroy(mm.pic.tex);
    mm.pic = App::TutoTex();
    mm.win.state = 4;
    menuSound(app, se::kWinClose);
    mm.state = 3;
}

int manualPageChars(const App& app) {
    const App::ManualMsg& mm = app.manualMsg;
    return mm.page < mm.pages.size() ? pageChars(mm.pages[mm.page]) : 0;
}

// A tap on the page (PrcMsg): one still typing comes out whole, one out turns - the picture to its
// next page (flag 4075), gone past its last - and the last page out ends the message
void manualTap(App& app) {
    App::ManualMsg& mm = app.manualMsg;
    if (mm.state != 2 || mm.win.state != 3) return;
    if (mm.typed < manualPageChars(app)) { mm.typed = manualPageChars(app); return; }
    mm.typed = 0;
    if (mm.pic.on) {
        mm.pic.timer = 20, mm.pic.frame = 0;
        if (++mm.pic.page >= mm.pic.pages) { app.renderer.destroy(mm.pic.tex); mm.pic = App::TutoTex(); }
    }
    if (++mm.page >= mm.pages.size()) manualCloseMessage(app);
}

void openManual(App& app, int intro) { // intro: a page shown before any topic (OptionSet's 56035), 0 none
    app.manualOpen = true;
    app.manualIntro = intro;
    app.manualCursor = 0;
    app.manualScroll = ScrList();
    app.manualAnim = app.manualFrames = app.manualOut = 0;
    app.manualPointer = App::ManualPointer();
    App::ManualMsg& mm = app.manualMsg;
    app.renderer.destroy(mm.pic.tex);
    mm = App::ManualMsg();
    if (intro) manualShow(app, intro); // (OptionPrc 0..2: the same window, then Options)
}

void manualChoose(App& app) { // OptionPrc 0x10: a topic chosen - the list's windows go, 4 frames on its page opens
    const std::vector<int> topics = manualTopics(app);
    if (app.manualCursor < 0 || app.manualCursor >= int(topics.size()) || app.manualOut) return;
    menuSound(app, se::kChoose); // (__ScrListSelectSePlay)
    app.manualPointer = App::ManualPointer();
    App::ManualMsg& mm = app.manualMsg;
    const int item = topics[size_t(app.manualCursor)]; // (OptionPrc 0x12: item - 0x1b7 below 0x1b7 + 0x28, else - 0x1d2)
    mm.id = kManualFirstPage + item - (item <= kManualLastItem ? kManualFirstItem : 0x1d2);
    mm.state = 1, mm.count = 4;
}

// OptionPrc 0x10's Back (MuScrList_Proc's ComUty_BackButtonProc(1), SE 3) -> 0x11: the gauge and Back (windows 7, 8)
// and the list (MuScrList_DispActive(0, 0): out to 480) go off, and state 3 - the frame after - puts Options' "Options"
// and Back (0, 5) on, 4 its two halves (1, 2); the manual is gone once its windows are
void manualBack(App& app) {
    if (!app.manualOpen || app.manualOut || app.manualMsg.state != 0 || app.manualIntro) return;
    menuSound(app, se::kBack);
    app.manualOut = 1;
    app.manualPointer = App::ManualPointer();
    if (app.optionsOpen) app.optionsAnim = -1;
}

// A frame (20 Hz): 0x12's wait, PrcWin and PrcMsg's typing, the picture's frames, the window closing; the list's rows
// stepping 8 toward the focus row (MuScrList_Proc), its windows' moves
void stepManual(App& app) {
    App::ManualMsg& mm = app.manualMsg;
    ++app.manualAnim, ++app.manualFrames;
    if (scrlist::frame(app.manualScroll, 1, app.manualCursor, int(manualTopics(app).size())) && app.manualMsg.state == 0)
        menuSound(app, se::kListFocus); // (MuScrList_Draw: __effgo(1) as the focus row changes)
    if (app.manualOut && ++app.manualOut > 5) { // (WinDispOff: whole a frame, then 4 frames to gone)
        app.manualOpen = false, app.manualOut = 0;
        return;
    }
    switch (mm.state) {
    case 1:
        if (--mm.count < 0) manualShow(app, mm.id);
        break;
    case 2:
        stepMsgWin(mm.win);
        if (mm.win.state == 3) mm.typed = app.typeOn ? std::min(mm.typed + 1, manualPageChars(app)) : manualPageChars(app);
        if (mm.pic.on) stepTutoFrames(mm.pic);
        break;
    case 3:
        stepMsgWin(mm.win);
        if (mm.win.state == 6) {
            mm.state = 0;
            if (app.manualIntro) { app.manualOpen = false; break; } // (the intro: on to Options)
            app.manualAnim = 0; // (0x16 -> 0xf: MuScrList_Start - the list's windows anew, at the item)
        }
        break;
    default: break;
    }
}

// The list's windows (0 out .. 1 in): MuScrList_Start (OptionPrc 0xf, the frame after the Manual was chosen in
// state 5 or the page's window had shut) puts the list (window 6, its HELP with it) on from 480 to the right and the
// gauge (7) and Back (8) on where they are; a topic chosen (0x10) or Back (0x11) takes them off that frame
void manualWindows(const App& app, CampWin& list, CampWin& bar) {
    const App::ManualMsg& mm = app.manualMsg;
    list = campWinOn(app.manualAnim, 1, 480), bar = campWinOn(app.manualAnim, 1, 0);
    if (app.manualOut) list = campWinOff(app.manualOut - 1, 0, 480), bar = campWinOff(app.manualOut - 1, 0, 0);
    else if (mm.state == 1) list = campWinOff(4 - mm.count, 0, 480), bar = campWinOff(4 - mm.count, 0, 0);
    else if (mm.state >= 2) list.a = bar.a = 0;
}

bool manualPressIn(const App& app, float origin, float x, float y, float w, float h) { // (Back lit while held)
    const App::ManualPointer& p = app.manualPointer;
    if (!p.down || p.dragging) return false;
    float lx, ly, mx, my;
    app.renderer.toLogical(p.curPx, p.curPy, lx, ly);
    menuPointAt(app, lx, ly, origin, mx, my);
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

// OptionPrc 0xf: MuScrList_Init(7, 8) - layout 0 (Init zeroes it), the Camp's - and SetIdInfo(10, 6, 0, 0, 1, 1):
// type 10's rows (ComUty_ComPlate_Base: plate 8 / 9, the item's icon, its name white - FUN_00056508's own for the
// manual items, 0x1b7..0x1d9 and 0x1f5 / 0x1f6 - and no count, MuScrList_Draw 55748-55790), the HELP board with
// the focused topic's item text (argument 5: ComUty_HelpWindowProc(0, item - 1) - 52438 "Basic Controls" ...;
// HelpWindowDraw 55922) and the one-part Back bar (argument 6: ComUty_BackButtonDraw type 1). Over the menu mask,
// Options' windows gone (FUN_00071444; drawOptions)
void drawManual(App& app) {
    using namespace campui;
    cr::Renderer& r = app.renderer;
    cr::MenuPainter& m = app.menu;
    const App::ManualMsg& mm = app.manualMsg;
    m.begin(r, app.windowTex, &app.font);
    r.setOrigin(r.uiX());
    if (!app.manualIntro) {
        const PhoneMenu phoneMenu(app, m);
        CampWin list, bar;
        manualWindows(app, list, bar);
        const std::vector<int> topics = manualTopics(app);
        const int n = int(topics.size());
        const ScrList& sl = app.manualScroll;
        const float scroll = sl.key == 1 ? float(sl.shown()) : kRowH * float(app.manualCursor);
        const int focus = std::clamp(sl.key == 1 ? scrlist::focusOf(sl, n) : app.manualCursor, 0, std::max(0, n - 1)); // (__MuScrList_GetFocusPos)
        r.setOrigin(wideRight(app)); // (widescreen: the list, its HELP and gauge to the right, as the Camp's)
        if (list.a > 0 && n > 0) {
            drawHelpWindow(app, m, topics[size_t(focus)], list.a, list.dx);
            const float x = kRowX + list.dx;
            m.setClip(kListTop, kListBottom); // (MuScrList_Draw mode 0: row k at (192, 150 + 48k - offset), cut to 76..280)
            for (int k = 0; k < n; ++k) {
                const float y = kFocusY + kRowH * float(k) - scroll;
                if (y + kRowH > kListTop && y < kListBottom) drawCampRow(app, m, x, y, topics[size_t(k)], 0, k == focus, list.a, 0);
            }
            if (n > 2) { // (the pills: taps jump to the other end)
                if (focus < 2) m.picture(0x8c, x + 88, 112 - scroll, list.a);
                if (focus >= n - 2) m.picture(0x8d, x + 88, 208 + kRowH * float(n - 1) - scroll, list.a);
            }
            m.clearClip();
        }
        if (bar.a > 0) drawCampScrollBar(m, scroll, n, bar.a);
        r.setOrigin(r.uiX());
        if (bar.a > 0) {
            m.backBar(kBackX, campBackY(app), manualPressIn(app, r.uiX(), kBackX, campBackY(app), 256, 34), bar.a);
            if (app.showHud) m.text("Up/Down  Enter: read  Esc: back", 20, 372, false, bar.a * 0.8f, 9); // (our key hints)
        }
    }
    if (mm.state < 2) return;
    // The message in the standard window (drawMessageBox with the manual's window, style and rect)
    const int keepStyle = app.msgStyle, keepLines = app.msgLines, keepLimit = app.textLimit;
    const App::MsgWin keepWin = app.msgWin;
    app.msgStyle = 8, app.msgLines = mm.picture ? 2 : 1, app.msgWin = mm.win;
    const bool text = mm.state == 2 && mm.page < mm.pages.size();
    app.textLimit = app.typeOn ? mm.typed : 1 << 30;
    drawMessageBox(app, text ? mm.pages[mm.page] : std::vector<std::string>(), "", text && mm.typed >= manualPageChars(app), "");
    app.msgStyle = keepStyle, app.msgLines = keepLines, app.msgWin = keepWin, app.textLimit = keepLimit;
    if (mm.state == 2) drawTutoPicture(app, mm.pic); // (MsgUty_TexDispDraw: while flag 3 is set)
    r.setOrigin(r.uiX());
}

// MuScrList (layout 0): a key or the wheel moves the focus a row, the rows easing after it (scrlist::frame)
void manualScrollBy(App& app, int rows, bool wrap) { // (wrap: a key / the pad)
    const int n = int(manualTopics(app).size()), before = app.manualCursor;
    // (the Vita's FUN_8105880a: up on the first row the last, down on the last the first - a fresh press; the pad's held
    // repeat stops at the ends, padStep)
    if (wrap && n > 0 && std::abs(rows) == 1 && (before + rows < 0 || before + rows >= n)) app.manualCursor = rows < 0 ? n - 1 : 0;
    else app.manualCursor = std::clamp(app.manualCursor + rows, 0, std::max(0, n - 1));
    if (before != app.manualCursor) menuSound(app, se::kListFocus); // (MuScrList_Draw: __effgo(1) as the focus row changes)
}

void manualKey(App& app, SDL_Keycode key) {
    App::ManualMsg& mm = app.manualMsg;
    const bool confirm = key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE || key == SDLK_E;
    const bool back = key == SDLK_ESCAPE || key == SDLK_BACKSPACE || key == SDLK_AC_BACK;
    if (mm.state == 2) { // the page: a tap turns it (keys: confirm; back closes it at once - the port's)
        if (confirm) manualTap(app);
        else if (back && mm.win.state == 3) manualCloseMessage(app);
        return;
    }
    if (mm.state != 0 || app.manualOut) return; // (the list going, or the window closing)
    if (confirm) { // (the Vita's MuScrList_Proc, 65420-65426: ○ taken only with the rows at rest)
        if (scrlist::rests(app.manualScroll, 1)) manualChoose(app);
        return;
    }
    switch (key) {
    case SDLK_UP: case SDLK_W: manualScrollBy(app, -1, true); break;
    case SDLK_DOWN: case SDLK_S: manualScrollBy(app, 1, true); break;
    case SDLK_PAGEUP: case SDLK_PAGEDOWN: { // (L / R: the port's - a list flicked a view on)
        const int before = app.manualCursor, n = int(manualTopics(app).size());
        app.manualCursor = std::clamp(before + (key == SDLK_PAGEUP ? -campui::kRows : campui::kRows), 0, std::max(0, n - 1));
        if (app.manualCursor != before) menuSound(app, se::kPage);
        break;
    }
    case SDLK_ESCAPE: case SDLK_BACKSPACE: case SDLK_AC_BACK: manualBack(app); break;
    default: break;
    }
}

// A touch lifting (frame pixels), not a drag (MuScrList_Proc): Back (lit while held) closes; on the list a tap over /
// under the line moves it a row, on the line (pad_chk_updw_box(192, 150, 256, 48)) chooses at rest; the pills' boxes
// jump to the other end (scrlist::tap)
void manualClick(App& app, float px, float py) {
    using namespace campui;
    App::ManualMsg& mm = app.manualMsg;
    if (mm.state == 2) { manualTap(app); return; } // (anywhere: PrcMsg's tap)
    if (mm.state != 0 || app.manualIntro || app.manualOut) return;
    float lx, ly, mx, my;
    app.renderer.toLogical(px, py, lx, ly);
    menuPointAt(app, lx, ly, app.renderer.uiX(), mx, my);
    if (mx >= kBackX && mx < kBackX + 256 && my >= campBackY(app) && my < campBackY(app) + 34) { manualBack(app); return; }
    menuPointAt(app, lx, ly, wideRight(app), mx, my);
    // a tap on the rows (scrlist::tap - MuScrList_Proc): over / under the line a row on, the line's row chosen once the
    // rows rest, the pills' boxes to the other end (the gauge is a held control: manualPointerDown)
    const int n = int(manualTopics(app).size());
    scrlist::ready(app.manualScroll, 1, app.manualCursor);
    const int r = scrlist::tap(app.manualScroll, mx, my, n);
    if (r == 2) manualChoose(app);
    else if (r == 1 && scrlist::follow(app.manualScroll, app.manualCursor, n)) menuSound(app, se::kListFocus);
}

// The list's touch (the Camp's model: campPointerDown / Move / Up). On the page a tap turns it as it goes down
void manualPointerDown(App& app, float px, float py) {
    using namespace campui;
    App::ManualPointer& p = app.manualPointer;
    p = App::ManualPointer();
    const App::ManualMsg& mm = app.manualMsg;
    if (mm.state == 2) { manualTap(app); return; }
    if (mm.state != 0 || app.manualIntro || app.manualOut) return;
    p.down = true;
    p.curPx = px, p.curPy = py;
    float lx, ly;
    app.renderer.toLogical(px, py, lx, ly);
    menuPointAt(app, lx, ly, wideRight(app), p.startX, p.startY);
    p.lastY = p.startY;
    // MuScrList_Proc: the gauge's box, else the drag area (scrlist::press)
    const int n = int(manualTopics(app).size());
    scrlist::ready(app.manualScroll, 1, app.manualCursor);
    scrlist::press(app.manualScroll, std::max(p.startX, 1.0f), p.startY, n, app.inputTime);
    p.bar = app.manualScroll.hold == ScrList::Gauge;
    if (scrlist::follow(app.manualScroll, app.manualCursor, n)) menuSound(app, se::kListFocus);
}
void manualPointerMove(App& app, float px, float py) {
    using namespace campui;
    App::ManualPointer& p = app.manualPointer;
    if (!p.down) return;
    if (!app.manualOpen || app.manualMsg.state != 0 || app.manualOut) {
        p = App::ManualPointer();
        app.manualScroll.hold = ScrList::None, app.manualScroll.drag = 0;
        return;
    }
    p.curPx = px, p.curPy = py;
    float lx, ly, mx, my;
    app.renderer.toLogical(px, py, lx, ly);
    menuPointAt(app, lx, ly, wideRight(app), mx, my);
    if (std::fabs(mx - p.startX) > 6 || std::fabs(my - p.startY) > 6) p.dragging = true;
    // (MuScrList_Proc case 3: the rows with the finger, pixel for pixel; ComUty_PadChkMove: the gauge held)
    const int n = int(manualTopics(app).size());
    scrlist::move(app.manualScroll, std::max(mx, 1.0f), my, n, app.inputTime);
    if (scrlist::follow(app.manualScroll, app.manualCursor, n)) menuSound(app, se::kListFocus);
    p.lastY = my;
}
void manualPointerUp(App& app, float px, float py) {
    using namespace campui;
    if (!app.manualPointer.down) return;
    manualPointerMove(app, px, py);
    const App::ManualPointer p = app.manualPointer;
    app.manualPointer = App::ManualPointer();
    if (!p.down) return;
    float lx, ly, mx, my;
    app.renderer.toLogical(px, py, lx, ly);
    menuPointAt(app, lx, ly, wideRight(app), mx, my);
    const int n = int(manualTopics(app).size());
    const bool tap = scrlist::lift(app.manualScroll, std::max(mx, 1.0f), my, n, app.inputTime); // (case 1 / 4)
    if (scrlist::follow(app.manualScroll, app.manualCursor, n)) menuSound(app, se::kListFocus);
    if (tap && !p.dragging && !p.bar) manualClick(app, px, py);
}

// ---- Character Select (Util 4: __menuu_StartPut) ----
// The chara1 sheet's art (the first two pairs and "Character Select"), the two plates at
// (56 + 232k, 232) - word pictures 12 Escher and 14 Eluca - and Back. The second sheet (16 Olgar,
// 18 Ayuta) is its mode 2, for when the first stories are cleared: not offered yet.
constexpr int kCharSelectWords[2][2] = {{12, 14}, {16, 18}}; // Escher / Eluca, Olgar / Ayuta

bool laterPairsOpen(const App& app) { // a first story finished (flag 32 or 34): chara2's pairs too
    const cr::GameState g = allPairsFlags(app);
    return g.flag(32) || g.flag(34);
}

void openCharSelect(App& app) {
    app.charSelectOpen = true;
    app.charSelectAnim = 0;
    // (__menuu_StartPrc 1, 72633-72640: the second sheet when flag 4092 is set - StartClr's for Olgar /
    // Ayuta; the boot script clears it from the title (j001_t99 0x2ed5), not on Back from Load / New)
    app.charSelectPage = app.game.flag(4092) && laterPairsOpen(app) ? 1 : 0;
    app.renderer.destroy(app.charSelectArt);
    cr::Image img;
    if (loadImage(app, app.charSelectPage ? "chara2_hd.mct" : "chara1_hd.mct", img)) app.charSelectArt = app.renderer.upload(img);
}

void charSelectTurn(App& app) { // the other sheet, once it's open
    if (!laterPairsOpen(app)) { menuSound(app, se::kBuzzer); return; }
    app.charSelectPage ^= 1;
    app.renderer.destroy(app.charSelectArt);
    cr::Image img;
    if (loadImage(app, app.charSelectPage ? "chara2_hd.mct" : "chara1_hd.mct", img)) app.charSelectArt = app.renderer.upload(img);
    menuSound(app, se::kPage);
}

void closeCharSelect(App& app, int pair) { // 0: back to the title
    app.charSelectOpen = false;
    app.game.vars[0] = pair; // __menuu_StartClr: SetVar(0, the pair 1..4, or 0 back); flag 4092 the second sheet
    app.game.setFlag(4092, pair >= 3);
    if (pair) app.chosenPair = pair;
    else app.renderer.destroy(app.charSelectArt); // (a pair: the save screen shows it too - showMap frees it)
}

// __menuu_StartPut's art (piece 16): 1.0.1's sheet 1024 x 768 + 164 for the iPad, 2.2.0's 960 x 640 +
// 168 (480 x 320 + 84 in tex2d) for the iPhone. Either sheet goes to either layout: scaled to cover
// the screen (the whole of it in widescreen), cut at the sides or top and bottom. `banner`: the
// "Character Select" one (piece 18) over its top edge; `grey` darkens it. Drawn from origin uiX
void drawCharArt(App& app, float alpha, bool banner, float grey) {
    cr::Renderer& r = app.renderer;
    if (!app.charSelectArt.id) return;
    const bool phoneSheet = app.phoneData || app.vitaOnly; // (the Vita's sheet is laid out as 2.2.0's)
    const float sw = phoneSheet ? 960.0f : 1024.0f, sh = phoneSheet ? 640.0f : 768.0f;
    const float vw = r.phoneLayout() ? 960.0f : r.screenW(), vh = r.phoneLayout() ? 640.0f : 768.0f;
    const float x0 = r.phoneLayout() ? 32.0f : -r.uiX(), y0 = r.phoneLayout() ? 64.0f : 0.0f;
    const float k = std::max(vw / sw, vh / sh), w = sw * k, ax = x0 + (vw - w) / 2, ay = y0 + (vh - sh * k) / 2;
    r.drawQuadUV(app.charSelectArt, ax, ay, w, sh * k, 0, 0, sw, sh, grey, grey, grey, alpha);
    const float bh = phoneSheet ? 168.0f : 164.0f;
    if (banner) r.drawQuadUV(app.charSelectArt, ax, y0, w, bh * k, 0, sh, sw, bh, grey, grey, grey, alpha);
}

void drawCharSelect(App& app) {
    cr::Renderer& r = app.renderer;
    cr::MenuPainter& m = app.menu;
    m.begin(r, app.windowTex, &app.font);
    const PhoneMenu phoneMenu(app, m); // the iPhone layout: its screen's menu coordinates
    r.setOrigin(0);
    r.fillRect(0, 0, r.screenW(), cr::Renderer::kLogicalH, 0, 0, 0, 1);
    r.setOrigin(r.uiX());
    const float t = std::min(1.0f, app.charSelectAnim / 10.0f);
    drawCharArt(app, t, true); // __menuu_StartPut: the art (piece 16, z -5), the banner (piece 18, z -4)
    for (int k = 0; k < 2; ++k) // (lit: the keys' cursor, or a touch holding it - StartPrc 2's DAT_000d3348)
        m.plate(56 + 232.0f * float(k), 232, plateIcon(kCharSelectWords[app.charSelectPage][k]), // (the word's icon)
                kCharSelectWords[app.charSelectPage][k],
                (app.charSelectCursor == k && menuCursorLit(app)) || pressIn(app, kPressCharSelect, r.uiX(), 272.0f * float(k), 0, 208, 278),
                t, false);
    // (the Back held lit - StartPrc 2's DAT_000d334c, minus BackButtonPosSetXYProc's answer: 1 Back, 2 the right
    // arrow, 3 the left - the part held: charSelectBackPart)
    const int held = app.press.down && app.press.screen == kPressCharSelect ? charSelectBackPart(app, app.press.px, app.press.py) : 0;
    if (laterPairsOpen(app)) m.backButton(124, 288, held, 0, t); // ComUty_BackButtonPosSetXYDraw(124, 288): its arrows
    else m.backBar(124, 288, held == 1, t);                      // (mode 2) once the later pairs are open (flags 0x20 / 0x22)
    if (keyHints(app)) // (our key hints)
    m.text(laterPairsOpen(app) ? "Left/Right  Q: other pairs  Enter  Esc: back" : "Left/Right  Enter  Esc: back", 20, 372,
           false, t * 0.8f, 9);
}

void charSelectKey(App& app, SDL_Keycode key) {
    switch (key) {
    case SDLK_LEFT: case SDLK_A: case SDLK_RIGHT: case SDLK_D: case SDLK_UP: case SDLK_DOWN:
        app.charSelectCursor ^= 1;
        menuSound(app, se::kCursor);
        break;
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: case SDLK_E:
        menuSound(app, se::kChoose);
        closeCharSelect(app, app.charSelectPage * 2 + app.charSelectCursor + 1);
        break;
    case SDLK_Q: case SDLK_TAB: case SDLK_PAGEUP: case SDLK_PAGEDOWN: charSelectTurn(app); break;
    case SDLK_ESCAPE: case SDLK_BACKSPACE: case SDLK_AC_BACK: menuSound(app, se::kBack); closeCharSelect(app, 0); break;
    default: break;
    }
}

// The Back's part at a touch (the frame's pixels): ComUty_BackButtonPosSetXYProc (0x4a018: __ComUty_BackButtonProc
// with x -68) - mode 1 the bar's box (124, 286, 256 x 34) is 1; mode 2 (the later pairs open) Back (180, 286,
// 120 x 34) 1, the right arrow's (305, 286, 51 x 34) 2, the left's (124, 286, 51 x 34) 3; else 0
int charSelectBackPart(const App& app, float px, float py) {
    float lx, ly, mx, my;
    app.renderer.toLogical(px, py, lx, ly);
    menuPoint(app, lx, ly, mx, my);
    auto in = [&](float x, float w) { return mx >= x && mx < x + w && my >= 286 && my < 286 + 34; };
    if (!laterPairsOpen(app)) return in(124, 256) ? 1 : 0;
    return in(180, 120) ? 1 : in(305, 51) ? 2 : in(124, 51) ? 3 : 0;
}

void charSelectClick(App& app, float px, float py) {
    float lx, ly;
    app.renderer.toLogical(px, py, lx, ly);
    float mx, my;
    menuPoint(app, lx, ly, mx, my);
    for (int k = 0; k < 2; ++k) { // (StartPrc 2's boxes, 0xbd3d0: each pair's half of the picture above Back -
        const float x = 272.0f * float(k); // (0, 0) / (272, 0), 208 x 278 - the plate and the portrait alike)
        if (mx >= x && mx < x + 208 && my >= 0 && my < 278) {
            menuSound(app, se::kChoose);
            closeCharSelect(app, app.charSelectPage * 2 + k + 1);
            return;
        }
    }
    // StartPrc 2's ComUty_BackButtonPosSetXYProc: -1 Back (SE 3), -3 / -2 the arrows (SE 5: the other sheet)
    if (const int part = charSelectBackPart(app, px, py); part == 1) { menuSound(app, se::kBack); closeCharSelect(app, 0); }
    else if (part) charSelectTurn(app);
}

// ---- The title menu (Util 0: __menuu_Title*) ----
// Three plates (ComUty_ChoicePlateDraw) at (328, 114 + 56k): Start, Scenarios, Options (words 2,
// 32, 3; icons kanIconNoTbl - 1), the cursor's lit, and the emblems (pictures 155 and 124) at
// (320, 16) / (380, 16), over the title picture the script showed in Tex2d slot 0.
constexpr int kTitleWords[3] = {2, 32, 3};
constexpr int kTitleIcons[3] = {2, 27, 3};

bool titleChoiceReady(int) { return true; }

// TitleSet / TitlePut: title1.mct (slot 0x11) and chaos.mct (0x12) - window 0 the "Please Touch" line
// (MuTex 1 picture 7) and the logo (6), window 1 the credits strip (8), as the structs at 0xbd400
// (2.2.0) / 0xca680 (1.0.1) place them: 2.2.0's 480 x 320 sheet (its _hd twice that) 7 (184, 360,
// 112 x 32) at (184, 216), 6 (0, 0, 480 x 256) at (0, 0), 8 (0, 320, 480 x 40) at (0, 280); 1.0.1's
// iPad sheet 7 (0, 848, 1024 x 64) at tex2d (-32, 432), 6 (0, 0, 1024 x 512) at (-32, -64), 8 (0,
// 768, 1024 x 80) at (-32, 624) - the iPad screen's (0, 496), (0, 0), (0, 688)
void drawTitleArt(App& app, float logo, float strip) {
    cr::Renderer& r = app.renderer;
    static const char* const kSheet[2] = {"title1_hd.mct", "chaos_hd.mct"};
    for (int k = 0; k < 2; ++k)
        if (!app.titleArt[k].id) {
            cr::Image img;
            if (loadImage(app, kSheet[k], img)) app.titleArt[k] = r.upload(img);
        }
    const bool phoneSheet = app.phoneData || app.vitaOnly; // (the Vita's sheets are laid out as 2.2.0's)
    struct Piece { int tex; float u, v, w, h, x, y; };
    // in the sheet's own screen pixels (2.2.0's Retina 960 x 640, the iPad's 1024 x 768)
    static const Piece kPhone[3] = {{0, 368, 720, 224, 64, 368, 432}, {1, 0, 0, 960, 512, 0, 0}, {0, 0, 640, 960, 80, 0, 560}};
    static const Piece kPad[3] = {{0, 0, 848, 1024, 64, 0, 496}, {1, 0, 0, 1024, 512, 0, 0}, {0, 0, 768, 1024, 80, 0, 688}};
    const float sw = phoneSheet ? 960.0f : 1024.0f, sh = phoneSheet ? 640.0f : 768.0f;
    const float vw = r.phoneLayout() ? 960.0f : 1024.0f, vh = r.phoneLayout() ? 640.0f : 768.0f;
    const float x0 = r.phoneLayout() ? 32.0f : 0.0f, y0 = r.phoneLayout() ? 64.0f : 0.0f;
    const float kx = vw / sw, ky = vh / sh;
    r.setOrigin(r.uiX());
    for (int i = 0; i < 3; ++i) {
        const Piece& p = (phoneSheet ? kPhone : kPad)[i];
        const cr::Texture& t = app.titleArt[p.tex];
        const float a = i == 2 ? strip : logo;
        if (!t.id || a <= 0.001f) continue;
        r.drawQuadUV(t, x0 + p.x * kx, y0 + p.y * ky, p.w * kx, p.h * ky, p.u, p.v, p.w, p.h, 1, 1, 1, a);
    }
}

void drawTitleMenu(App& app) {
    cr::MenuPainter& m = app.menu;
    // TitlePrc: the art alone (windows 0 / 1 / 2) until a tap (state 1); then the logo and "Please
    // Touch" go (windows 0 and 2 off), the strip stays, and six frames on (state 2's count of 4,
    // then 3) the plates and the store's pictures slide in from x +240 (window 3)
    const bool plates = app.titleStage == 1;
    const float t = std::min(1.0f, app.titleAnim / 10.0f);
    drawTitleArt(app, plates ? std::max(0.0f, 1 - t) : t, plates ? 1.0f : t);
    m.begin(app.renderer, app.windowTex, &app.font);
    const PhoneMenu phoneMenu(app, m); // the iPhone layout: its screen's menu coordinates
    if (plates && !app.resumeNotice && !app.resumePrompt) { // (Start with a resume point: window 3 off, the strip stays)
        const float pt = std::clamp((app.titleAnim - 6) / 10.0f, 0.0f, 1.0f), ease = 1 - (1 - pt) * (1 - pt);
        app.renderer.setOrigin(wideRight(app)); // (widescreen: the emblems and plates to the screen's right)
        m.picture(155, 320, 16, pt);
        m.picture(124, 380, 16, pt);
        for (int i = 0; i < 3; ++i)
            m.plate(328 + 240 * (1 - ease), 114 + 56.0f * float(i), kTitleIcons[i], kTitleWords[i],
                    (app.titleCursor == i && menuCursorLit(app)) || pressIn(app, kPressTitle, wideRight(app), 328, 114 + 56.0f * float(i), 136, 40),
                    pt, !titleChoiceReady(i)); // (lit: the keys' cursor, or a touch holding it - TitlePrc's pad_chk_updw_box)
    }
    app.renderer.setOrigin(wideLeft(app));
    if (keyHints(app) && !app.resumeNotice && !app.resumePrompt) // (our key hints)
        m.text(plates ? "Up/Down  Enter  V: Options  Esc: quit" : "Enter: start  Esc: quit", 20, 360, false, t * 0.8f, 9);
    app.renderer.setOrigin(app.renderer.uiX());
}

// Start: the boot script goes on (flag 0 clear) to Character Select (Util 4), then the pair's save /
// load screen (Util 5)
void titleStart(App& app) {
    if (app.booting && app.runner.running()) {
        app.game.setFlag(0, false);
        app.titleOpen = false;
    } else {
        openSaveScreen(app, true);
    }
}

// TitlePrc state 1: a tap anywhere - SE 0, windows 0 and 2 off, the plates six frames on
void titleTap(App& app) {
    if (app.titleStage != 0) return;
    menuSound(app, 0);
    app.titleStage = 1;
    app.titleAnim = 0;
}

// The plates take taps from state 4 on (the frame after window 3 goes on)
bool titlePlatesLive(const App& app) { return app.titleStage == 1 && app.titleAnim >= 6; }

void titleChoose(App& app, int i) {
    app.titleCursor = i;
    if (!titleChoiceReady(i)) { menuSound(app, se::kBuzzer); return; }
    menuSound(app, se::kChoose);
    if (i == 2) { openOptions(app, 0); return; } // SetOptionMenu + ModeOptionMenu(1)
    if (i == 1) { openScenarios(app); return; }  // SetHistoryMenu
    // Start (TitlePrc 4 -> 0xf .. 0x14): the Progress Tracker's offer first, when there is a resume point
    offerResume(app);
    if (app.resumePrompt || app.resumeNotice) return;
    titleStart(app);
}

// Keys on the title; false = quit
bool titleKey(App& app, SDL_Keycode key) {
    if (app.resumeNotice) { // "About Progress Tracker": a page out, or the next
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE || key == SDLK_E) tapDialogue(app);
        return true;
    }
    if (app.resumePrompt) return true; // (its windows going: nothing to answer)
    // (the Vita's __menuu_TitlePrc 1: 0xf008 - □ △ ○ × START, no D-pad; the pad's D-pad comes as the arrows)
    const bool padArrow = app.padKeyNow && (key == SDLK_UP || key == SDLK_DOWN || key == SDLK_LEFT || key == SDLK_RIGHT);
    const bool padShoulder = app.padKeyNow && (key == SDLK_PAGEUP || key == SDLK_PAGEDOWN); // (L / R: not in 0xf008)
    if (app.titleStage == 0 && key != SDLK_ESCAPE && key != SDLK_AC_BACK) { if (!padArrow && !padShoulder) titleTap(app); return true; }
    if (!titlePlatesLive(app) && key != SDLK_ESCAPE && key != SDLK_AC_BACK) return true;
    switch (key) {
    case SDLK_UP: case SDLK_W: app.titleCursor = (app.titleCursor + 2) % 3; menuSound(app, se::kCursor); break;
    case SDLK_DOWN: case SDLK_S: app.titleCursor = (app.titleCursor + 1) % 3; menuSound(app, se::kCursor); break;
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: case SDLK_E: titleChoose(app, app.titleCursor); break;
    case SDLK_ESCAPE: case SDLK_AC_BACK: return false;
    default: break;
    }
    return true;
}

void titleClick(App& app, float px, float py) {
    float lx, ly;
    app.renderer.toLogical(px, py, lx, ly);
    if (app.resumeNotice) { tapDialogue(app); return; } // (the message window: a tap anywhere)
    if (app.resumePrompt) return;
    if (app.titleStage == 0) { titleTap(app); return; } // (a tap anywhere: _pad_chk_trg_up_box0(0, 0, 480, 320))
    if (!titlePlatesLive(app)) return;
    float mx, my;
    menuPointAt(app, lx, ly, wideRight(app), mx, my); // (widescreen: the plates on the right)
    for (int i = 0; i < 3; ++i)
        if (mx >= 328 && mx < 464 && my >= 114 + 56 * i && my < 154 + 56 * i) titleChoose(app, i);
    // the store's picture 155 (320, 16, 57 x 57): its sound (2.2.0: __effgo(2), SetStoreMenu - no store
    // here); 124 (380, 16, 80 x 72) opens the publisher's page with no sound - left out
    if (mx >= 320 && mx < 377 && my >= 16 && my < 73) menuSound(app, se::kChoose);
}

} // namespace crapp
