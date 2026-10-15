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
//   Space            pause / resume           - / =         slower / faster
//   W A S D          walk the party leader on a field (Shift: run)
//   P / O            player on/off / next party character (f001-f008)
//   Esc              quit

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <functional>
#include <ctime>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <tuple>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "assets/hd_texture.hpp"
#include "audio/at9.hpp"
#include "scene/smooth.hpp"
#include "game/save.hpp"
#include "game/shop.hpp"
#include "assets/animation.hpp"
#include "assets/collision.hpp"
#include "assets/battle_data.hpp"
#include "assets/event.hpp"
#include "assets/gxt.hpp"
#include "assets/psarc.hpp"
#include "game/encounter.hpp"
#include "game/event_vm.hpp"
#include "game/field_walker.hpp"
#include "game/stage_tables.hpp"
#include "game/battle_rules.hpp"
#include "game/genes.hpp"
#include "game/puzzle.hpp"
#include "game/treasure.hpp"
#include "assets/model.hpp"
#include "assets/texture.hpp"
#include "audio/music.hpp"
#include "audio/sound.hpp"
#include "core/file.hpp"
#include "core/math.hpp"
#include "gfx/font.hpp"
#include "gfx/hud.hpp"
#include "gfx/menu_painter.hpp"
#include "gfx/renderer.hpp"
#include "platform/paths.hpp"
#include "scene/pose.hpp"

namespace {

enum class Mode { Textures, Models, Maps };

// Original animations are keyed in frames; the game ran at 30 fps (assumption, see README)
constexpr float kFramesPerSecond = 30.0f;

struct LoadedModel {
    cr::Model cpu;                       // skeleton, skin data, rest pose
    std::vector<cr::GpuMesh> meshes;
    std::vector<cr::Texture> textures;
    std::vector<cr::Action> actions;     // only those that fit the skeleton
    int builtin = 0;                     // the package's own (slots 0..builtin-1); then <stem>e*.mca
    std::map<std::string, int> loaded;   // LoadAct files already appended: .mca -> action index
    int action = -1;                     // -1 = rest pose
    float time = 0;
    int loopFrom = -1;                   // after its end, frames loopFrom.. again (mdl_set_act_motion_loop); -1 whole
    cr::Vec3 center;
    float radius = 1;
    int triangles = 0;
    std::vector<cr::Mat4> world;
    std::vector<float> skinned;
    std::vector<cr::Action> packageActions; // effects: every action, as materials' refGid number them
    std::vector<float> uvScratch;          // effects: a mesh's scrolled texture coordinates
};

// One playing eff3d effect (eff3d_set_with_pos[_and_ang]): an eff.bin model at a point, scaled
// (percent / 100) and turned about Y; deleted at its action's end unless it loops (until eff3d_delete)
struct Effect {
    int id = 0;
    cr::Vec3 pos;
    float time = 0;
    bool loop = false;
    float scale = 1, yaw = 0;
    int handle = 0;  // for stopping it
    int key = 0;     // FrmPlayEff's (effect + 1) x (actor + 1)
};

struct App {
    SDL_Window* window = nullptr;
    bool rendererReady = false;
    cr::Renderer renderer;
    cr::Hud hud;
    std::string status;  // what the title bar shows, also drawn on screen
    std::string dataDir;
    // The Vita release (PCSG00497) as a source of assets (--vita, CR_VITA or settings.ini's vita=):
    // cr1.psarc's textures, model packages and scripts (every .evt: eight languages, the voice
    // cues), its sound/ voices (voiceevnt / voicebtl.psarc) and music (bgm.psarc)
    std::string vitaPath;                  // the setting (applies at the next start)
    std::string vitaLoaded;                // what this run opened
    std::string vitaNote;                  // Options: the last folder chosen's outcome
    std::string vitaRoot;                  // its CR1 folder (resources/, sound/)
    std::string vitaSound;                 // its sound/ folder (with the trailing '/')
    bool vitaOnly = false;                 // no iOS data: the Vita copy is the whole game
    cr::PsarcArchive vita;
    cr::PsarcArchive vitaBgm, vitaVoice[2];
    int vitaTextures = 0;                // how many images / model packages came from it
    Mode mode = Mode::Textures;

    std::vector<std::string> textures;
    int texIndex = 0;
    cr::Texture currentTex;
    cr::MctInfo texInfo;

    std::vector<std::string> models;
    std::vector<std::string> animFiles;
    std::vector<std::string> maps;      // field collision meshes (*_10.cmp etc.)
    int mapIndex = 0;
    cr::GpuMesh collision;
    int collisionPolys = 0;
    std::string mapProps;               // prop model shown with the current map, if any
    bool showCollision = false;            // C: the collision and trigger meshes over the field (a debugging aid)
    bool showBackground = false;        // B: show the field's pre-rendered background
    bool hideHud = false;               // --capture renders without the text overlay
#ifdef CR_RELEASE
    bool showHud = false;               // the status / key help at the top: off in a Release build (F3)
#else
    bool showHud = true;
#endif
    cr::Texture background, foreground; // dXXXX-b00a00.mct / dXXXX-f00a00.mct
    cr::StageCamera stageCam;           // the field's camera from its event file
    bool haveStageCam = false;
    std::vector<cr::StageCamera> stageCams; // every map number using this collision file
    std::vector<float> stageFit;            // fraction of collision vertices each camera frames
    int stageCamIndex = 0;
    bool stageCamBorrowed = false;          // no camera of its own: best fit from the same family
    cr::Collision collisionData;           // the current field's collision, for walking on
    cr::Collision walkCollision;           // collisionData + walls of solid NPCs (chests, crystals…)
    int mapNumber = -1, area = -1;         // the field being shown (map 0..129, area = event file)
    cr::EventFile events;                  // the current area's event file
    std::vector<cr::FieldNpc> npcs;        // this map's NPCs and trigger boxes
    std::vector<bool> npcWarps;            // per NPC: its touch script warps (from a trial run)
    cr::GpuMesh triggerMesh;               // trigger boxes, shown with the collision overlay
    cr::GpuMesh puzzleBeams;               // the candle switches' beams (CandleSwitch::draw3D)
    cr::GameState game;                    // flags / variables the scripts use (new game)
    int lastKick = 0;                      // trigger the player is standing in (fires once)
    std::string lastWarp;                  // shown in the status line
    std::string lastEvent;                 // last examined object / items, for the status line
    cr::Hud dialogueHud, promptHud, itemHud;
    std::array<cr::Hud, 8> lineHud;        // the message window's lines one by one (some centred)
    cr::EventRunner runner;                // the script being played, a frame at a time
    cr::SoundSystem sound;                 // sndeff.bin through SDL audio
    cr::MusicPlayer music;                 // bgmNN.caf (AAC) through FAAD2 + SDL audio
    int musicArea = -1;                    // area whose default track is playing
    // Mixer, 0..100 like the original's Options sliders (music and effects default to 80 there;
    // master and voice are additions - the original has no voice audio)
    struct Mix { int master = 100, music = 80, effects = 80, voice = 100; } mix;
    bool optionsOpen = false;              // V: the Options screen (__menuu_Option*, plus our pages)
    int optionsPage = 0;                   // Game, Sound, Display, Graphics
    int optionsRow = 0;                    // the cursor: a row, or kOptionsBackRow
    int optionsAnim = 0;                   // frames since the page appeared (windows slide in)
    int optionsBob = 0, optionsBobDir = 1; // ComUty_CmdBack's arrows
    float optionsClock = 0;
    bool environmentEffects = true;        // the original's Environment Effects (weather; stored)
    bool lockOrientation = false;          // Lock Screen Orientation (an iPad setting; stored)
    cr::MenuPainter menu;
    bool persist = true;                   // settings.ini is written (not by tests and captures)
    // The title: the boot script (j001_t99's EvrStart 0: the logos, the title picture and music,
    // then Util 0's menu - Start / Scenarios / Options)
    bool booting = false;                  // no field behind it; the script ticks on its own
    float bootClock = 0;
    bool titleOpen = false;                // Util 0
    int titleCursor = 0;
    int titleAnim = 0;
    bool optionsFromTitle = false;         // ModeOptionMenu(1): Reset Data instead of Return to Title
    bool captureTitle = false;             // --capture ... --title
    // Video. The defaults are the original presentation: a 1024x768 window, 4:3, the paintings at
    // the screen's height, native resolution, no MSAA, vsync
    struct Video {
        int width = 1024, height = 768;    // window size (windowed) or display mode (fullscreen)
        int display = 0;                   // 0 windowed, 1 fullscreen (that mode), 2 borderless (the desktop's)
        bool widescreen = false;           // the screen widens to the window (up to 21:9); else 4:3
        int paintings = 0;                 // painted rooms: 0 the original's (2x, a screen-sized window following
                                           // the player), 1 the whole painting at the screen's height, 2 at its width
        int renderScale = 100;             // % of the window's pixels (Vulkan)
        int msaa = 1;                      // 1, 2, 4, 8 (Vulkan)
        bool vsync = true;
        int upscaler = 0;                  // 0 off, 1 FSR 3.1, 2 DLSS (Vulkan; MSAA is off meanwhile)
        int upscaleMode = 0;               // kUpscaleModes: 0 native AA (DLAA) .. 4 ultra performance
        int sharpness = 25;                // % (FSR's RCAS pass; 0 = none)
        bool hdTextures = false;           // upscaled replacements from hdDir (what loads next)
        int smoothModels = 0;              // 0 off, 2 / 3: PN-triangle level for characters and monsters
        int layout = 0;                    // 0 the data's own (2.2.0's iPhone layout for its data), 1 iPad, 2 iPhone
    } video;
    bool phoneData = false;                // the data is a voiced iPhone release's (voiceevnt.bin: 2.2.0)
    std::string hdDir;                     // HD textures: <save dir>/hd-textures/, or CR_HD_TEXTURES
    int hdLoaded = 0, hdMissing = 0;       // replacements found / not found since startup
    int language = 1;                      // text language (cr::kLanguageName): English by default
    cr::Font font;                         // system font for the game's text (all languages)
    std::vector<std::vector<int>> dialogueVoices; // per page: voice clips it starts (0x1e)
    int dialogueSerial = 0, voicedSerial = -1, voicedPage = -1; // which page's voices were started
    std::vector<int> voiceQueue;           // further clips of the same page
    int voicesPlayed = 0;                  // for tests
    int soundTestEntry = 0;
    std::vector<int> voiceEntries;         // bank entries marked as voice (sound test, T)
    cr::Hud optionsHud;
    cr::Texture windowTex;                 // window_hd.mct: the game's UI atlas (panels, badges, …)
    float footPrev = 0;                    // player's animation time last frame (footsteps)
    std::vector<uint32_t> soundLog;        // every sound started (self-test)
    std::vector<uint32_t> menuSoundLog;    // the menus' sounds, latest last (self-test)
    std::string runnerWhat;                // what started it, for the status line
    // Scripts the game starts itself when a field loads (fldread_prc): script 0 when the area's
    // file loads (init_evnt), EvrStart entry 5 on other room changes. Queued until the running
    // script (e.g. the exit that warped here) finishes.
    enum class Entry { None, Area, Room } pendingEntry = Entry::None;
    int entryArea = -1;                    // area whose script 0 last ran
    std::map<int, bool> npcShown;          // Born / Kill in this room (npc 0 = the player)
    struct Move {                          // FrmAdjc / FrmAdjc2 (npc_autoctl_prc), npc 0 = the player
        int npc = 0;
        cr::Vec3 from, to;
        float yawFrom = 0, yawTo = 0;      // radians, when turning
        bool turn = false;
        int mode = 0;                      // 2: y moves too, else it follows the floor
        int frames = 1, t = 0;
    };
    std::vector<Move> moves;
    std::set<int> placedNpcs;              // actors a script placed: parked slots (y 1000, origin) appear
    std::map<int, std::map<int, int>> actorSlots; // LoadAct: npc -> slot -> action index in its model
    struct ScriptCam {                     // FrmSetCam / FrmAdjCam / FrmPanCam after CameraFree
        bool free = false, set = false;
        cr::Vec3 eye, target, eyeFrom, eyeTo, tgtFrom, tgtTo;
        int frames = 0, t = 0;
    } cam;
    // The painted field's scroll (FieldCamera +4 / +8): the view's centre moved from the screen's,
    // x right and y up; ScrollAutoCtl's slide (FrmAdjCam / FrmPanCam) from -> to over `frames`
    struct FieldScroll {
        float offX = 0, offY = 0;
        bool sliding = false;
        float fromX = 0, fromY = 0, toX = 0, toY = 0;
        int t = 0, frames = 0;
        int mapScrolls = 0;                // Mapscroll ops seen (self-test)
    } scroll;
    // Scene effects (ScnEffectRead / ScnEffectStart / ScnEffectProc): a dungeon's weather drawn over
    // the field - fog, snow, rain, sun. The set's textures (Tex2d slots 49..72), which effects are on,
    // their frame counters and UpdateRandom's three fading alpha channels
    struct SceneFx {
        int set = -1;
        std::array<cr::Texture, 24> tex;   // slot 49 + i
        std::array<bool, 21> on{};
        std::array<int, 21> timerA{}, timerB{};
        struct Channel {
            int marker = -1, state = 0, count = 0, holdLen = 0;
            int alpha = 255, inc = 0, fadeIn = 1, fadeOut = 1, hold2 = 1;
        } ch[3];
        int holdMin = 60, holdMax = 120;
        uint32_t rng = 12345;
        int spaceDrift = 0, spaceRise = 0; // 0xddfb8: the fog's drift (0..480); 0xddfb4: the rise (Up +1, Down -1)
    } fx;
    struct Portrait {                      // Tex2d slots (+0xbd10 + slot * 0x70 in the original)
        cr::Texture tex;
        int image = -1;
        float x = 0, y = 0, alpha = 0;     // 1024x768 screen pixels
        float home = 0;                    // resting x (left / right / centre)
        int side = 0;                      // -1 left, 1 right: the side it slides from
        float shade = 1;                   // 0x1c in a message dims the listener's side to 128/255
        float xFrom = 0, xTo = 0, yFrom = 0, yTo = 0; // the slide (+0x14..+0x1e): t of frames
        int frames = 0, t = 0;
        float aFrom = 0, aTo = 0;          // the fade (+0x20..+0x26): aT of aFrames
        int aT = 0, aFrames = 0;
        float gFrom = 1, gTo = 1;          // the grey level (+0x28..+0x2e, low bit 0x20) into shade
        int gT = 0, gFrames = 0;
        bool dying = false;                // low bit 2 (+0xbd0c = -1): goes once its fade and slide end
        uint16_t flags = 0;                // the slot's flag word (+0x4): Tex2d flags OR'd in
        float scale = 1;                   // 0x40: centre images zoom from 200% to 100% over 4 frames
        int zoomT = 0, zoomFrames = 0;
    };
    Portrait portraits[16];
    bool itemWindowOpen = false;           // Uty_GetItem
    bool skipScene = false;                // the skip button (X): pages turn by themselves until the scene ends
    int msgStyle = 2, msgLines = 2;        // the script's last Open: the message window's style and lines
    double playSeconds = 0;                // TOTAL PLAYTIME: time on the field and in battles
    // The save / load screen (__menuu_SaveLoad*): from the Camp menu (save) or the title's Start
    // (load / new game). phase 0 choose, 1 confirm (Yes / No), 2 done (a message, then it closes)
    struct SaveScreen {
        bool open = false, load = false, haveSave = false;
        int cursor = 0, phase = 0, confirm = 0, message = 0, timer = 0, anim = 0;
        int pending = 0;                   // what Yes does: 1 save, 2 load, 3 new game
        int kind = 0;                      // the title's choice, var 1 on the way out (SaveLoadClr):
                                           // 1 the last battle, 2 new, 3 new with the clear data, 4 load
        int lines[3] = {};                 // the message board's further lines (system message ids)
        unsigned emblems = 0;              // the pair's three emblems (flags 31 + 2p, 32 + 2p, 39)
        bool allClear = false;             // flags 32, 34, 36, 38: every pair's story cleared
        bool cleared = false;              // flag 40 + p: this pair has clear data to carry over
        cr::SaveData data;                 // the save slot's contents (shown on the left)
    } saveScreen;
    bool campOpen = false;                 // the Camp menu (K on the field)
    // The shop (Util 2, __menuu_Shop*): Talk / Buy / Sell, then a category, the list, how many,
    // Yes / No. Talk hides it and plays the area's EvrStart 4 (the shopkeeper) while the script
    // that opened the shop is parked.
    struct Shop {
        enum Phase { Main, Category, List, Count, EquipAsk, Talk } phase = Main;
        bool open = false, selling = false;
        int cursor = 0, category = 0, listCursor = 0, listTop = 0, count = 1, confirm = 0;
        int member = 0;                    // the board's character: the pair's first (0) or second
        int bgmBack = 0;                   // ShopSet: var 0, the music to put back as it closes (+0x18)
        bool noBgmRecovery = false;        // SetShopBgmNoRecovery (Uty_EndMenu 2) (+0x1c)
        int anim = 0, timer = 0, message = 0; // message: the board's system message (0 none)
        int level = 2;                     // MuCom_GetShopBuyLevel
        bool talkRunning = false;          // Talk's script is the runner (the shop's own is parked)
        std::vector<int> list;             // the category's items
        cr::Texture keeper;                // c101_L01_hd.mct: the shopkeeper
    } shop;
    cr::EventRunner parkedRunner;          // the script that opened the shop, while Talk runs
    float shopClock = 0;
    cr::ItemTable itemTable;               // sys.evt: item prices, icons, kinds
    // Character Select (Util 4, __menuu_Start*): the boot script's pair choice before the save /
    // load screen; the chosen pair (1..4) goes to var 1 through that screen
    bool scenariosOpen = false;            // Scenarios (the title's; __menuu_History*): the story flow chart
    float scenariosScroll = 0;
    float scenariosHScroll = 0;            // the iPhone layout's (2.2.0): sideways, tex2d 0..-120
    int scenariosAnim = 0;
    cr::Texture scenariosSheet;            // flow_hd.mct
    cr::GameState scenariosFlags;          // every pair's saves together (the original's system data)
    bool manualOpen = false;               // Options > Instruction Manual
    int manualCursor = 0, manualTop = 0, manualAnim = 0;
    bool charSelectOpen = false;
    int charSelectCursor = 0, charSelectAnim = 0, chosenPair = 1, charSelectPage = 0;
    cr::Texture charSelectArt;
    struct Camp {                          // the Camp menu's page and cursors (__menuu_CampMain*)
        enum Page { Main, Items, Equip, Status, Genes } page = Main;
        enum Phase { List, Member, Slot, Ask } phase = List; // Status: Slot = the member's details
        int cursor = 0;                    // Main: the plate
        int category = 0;                  // Items: campui::kCategoryKind index
        std::vector<int> list;
        int listCursor = 0, listTop = 0;
        int member = 0;                    // the pair's first or second
        int slot = 0;                      // Equip: weapon, armor, jewel, Human Genes, gene plates 1-3
        int count = 1, confirm = 0;        // Items: how many seeds, Yes / No
        std::string message;
        int messageFrames = 0;
        int anim = 0;
        std::array<cr::Texture, 22> plateArt; // gene_m*c.mct, loaded when a board first shows them
    } camp;
    float saveClock = 0;
    int resumePrompt = 0;                  // 1: "You have save data from your last session" (Yes / No)
    int resumeCursor = 0;
    cr::SaveData resumeData;
    std::string saveDirOverride;           // tests: where save.dat / resume.dat go
    bool trackerArmed = false;             // G+0x34e1d: the Progress Tracker saves from the second scene's end on
    // A controller, as the Vita's buttons (its manual, 56000 / 56025): the D-pad / left stick move
    // and pick, ○ confirms, × cancels (the Japanese release's; padConfirmEast false swaps them), △
    // opens / closes the menu, □ skips text, L / R turn pages; in puzzle rooms START restarts,
    // SELECT shows the help, and □ (no text to skip) uses the ability, which the Vita leaves to touch
    SDL_Gamepad* pad = nullptr;
    bool padConfirmEast = true;
    int padHeld = -1;                      // the direction held for the menus' repeat (0 up 1 down 2 left 3 right)
    float padHeldFor = 0;
    int itemWindowItem = 0;
    bool tapPending = false;               // action key while a script waits for a tap
    float fadeGrey = 0, fadeFrom = 0, fadeTo = 0; // Screen
    int fadeFrames = 0, fadeT = 0;
    cr::SystemText systemText;             // sys.evt: messages >= 51000
    std::vector<std::vector<std::string>> dialogue; // pages of the messages the last event showed
    size_t dialoguePage = 0;
    std::vector<cr::MessageInfo> dialogueInfo; // per page: the speaker's name plate and side
    // PrcMsg's typing: the page's characters out so far (one a frame), a 0x1a pause or the closing
    // wait's frames left, the next pause, and whether the page is closing by itself (0x14)
    int dialogueTyped = 0, dialogueHold = 0, dialoguePause = 0;
    bool dialogueClosing = false;
    bool typeOn = true;                    // false: pages appear whole (tests, captures)
    int textLimit = 1 << 30;               // drawMessageLines: how many characters it draws
    std::vector<std::string> lastPage;     // the page just read: stays up while a choice asks about it
    cr::MessageInfo lastPageInfo;
    int choiceFlag = -1;                   // Util 3: the flag a yes/no choice answers (-1 = none open)
    std::vector<std::string> unsupported;  // battles / menus scripts asked for (--explore reports them)
    cr::BattleData battleData;             // sys.evt: monsters, enemy groups, battle animations and cameras
    cr::BattleActionTable battleActions;   // sys.evt: the actions (attacks, genes, items)
    cr::EncounterCounter encounters;       // this map's random encounters (fldencount_*)
    bool noEncounters = false;             // --no-encounters (the original's unencountflg)
    // _get_random's stream (CRView::setup seeds it with the clock: CFAbsoluteTime's seconds)
    cr::GameRandom rng{uint32_t(std::time(nullptr) - 978307200)};
    struct Battle {                        // btl_main: the stage, the pair and up to three enemies
        bool active = false;
        cr::BattleStart start;
        int flags = 0;                     // Battle's u8 (G+0x354c6)
        int formation = 0;
        LoadedModel stage;
        struct Actor {
            std::string model;             // battle model package (cache key)
            int modelId = 0;               // kBattleModelFile index, for its action slots
            bool enemy = false;
            int monster = 0, level = 1;
            uint32_t hp = 0, hpMax = 0;
            std::string name;
            cr::Vec3 pos;
            float yaw = 0;
            int slot = 0, action = -1;     // action slot (BattleAnim) and the package's action index
            float time = 0;
            float scale = 1;               // the model's size (the pair's 1: btl_main's 0.5 at +0x540 is its yaw, half a turn)
            int fighter = -1;              // BattleRules::f index: 0, 1 the pair, 2..4 the enemies
            int then = -1;                 // the slot after this one ends (-1 loop, -2 hold the last frame)
            cr::Vec3 home, from, to;       // its place; a move (btlmove_setpos) from -> to
            float homeYaw = 0;
            int moveFrom = 0, moveTo = 0;  // the move's frames of the current slot
            int loopFrom = -1, loopTo = -1; // mdl_set_act_motion_loop's frames (-1: the slot's own)
            bool fading = false;           // a fallen monster
            float alpha = 1;
        };
        std::vector<Actor> actors;
        std::map<std::string, LoadedModel> models;
        int frames = 0;
        float clock = 0;
        int fieldTrack = -1;               // music to resume afterwards
        bool requested = false;            // a script's Battle, started once its step ends
        bool eventBattle = false;          // G+0xa4e: a running script started it - it waits, and goes
                                           // on in the battle's talks and after it
        struct BreakFx {                   // btlbreak_effect_*: "BREAK!! The battle situation changed"
            bool on = false;
            int state = 0, t = 0, limit = 90; // +0x98 (0 in, 10 hold, 0x3e6 out), +4, +8
            int pos = -480, alpha = 0, slide = 0; // +0x34 the labels along the stripes, +0x38, +0x3c
        } breakFx;
        cr::BattleStart request;
        int requestFlags = 0;
        // The rules (game/battle_rules.hpp) and the scene around them (btl_main's states)
        cr::BattleRules rules;
        enum Phase { Intro, Command, Acting, WinTalk, Won, Results, Lost, Fled } phase = Intro;
        int phaseFrames = 0;
        enum Menu { Top, Commands, Targets, Items, EscapeAsk, Genes } menu = Top;
        int cursor = 0;
        int member = 0;                    // solo: the member choosing
        cr::BattleCommand cmds[2];
        int pendingItem = 0;               // an item chosen, its target next
        int pendingGene = 0;               // a gene ability chosen, its target next
        int targetAction = 0;
        std::vector<int> picks;            // Targets: fighters; Items: item numbers
        int pick = 0, pickTop = 0;
        int confirm = 0;                   // EscapeAsk: 0 Yes
        bool turnOn = false;               // an action is playing
        cr::BattleTurn turn;
        int turnFrame = 0, hitFrame = 0;
        bool hitsShown = false;
        struct Popup { cr::Vec3 at; int amount = 0, type = 0, timer = 0; bool mp = false; };
        std::vector<Popup> popups;         // __dmg_set's five numbers
        std::string banner;                // the top bar: the action, the target prompt, messages
        int bannerFrames = 0;
        std::vector<cr::BattleNotice> notices; // G+0xa14: the calcs' messages, one per free banner
        int shownHp[5] = {}, shownMp[5] = {}; // what the HUD shows (changes as the hits land)
        int gaugeShown = 0;                // the badge's gauge, a step a frame toward the rules'
        float breakPop = 0;
        std::vector<int> levelled;
        bool settled = false;              // the save has the battle's results
        int exit = 0;                      // 1 back to the field (fled), 2 to the title (lost)
        cr::Mat4 vp;                       // the last frame's camera (the damage numbers)
        int cam = -1, camActor = 0, camFrame = 0; // btlcamera_set: the camera, its actor, frames since
        std::vector<Effect> effects;       // eff3d: playing effects (32 at most)
        // The voiced releases' battle lines (2.2.0's _btlvoice_*): each fighter's next variant of
        // three (+0xd3), who of a pair speaks this round (+0xd2), the command lines said (+0xafc)
        int voiceVariant[5] = {};
        bool voiceSpeaks[5] = {};
        bool commandSaid[2] = {};
        int voiceRound = -1;
        uint32_t voiceRng = 1;
        struct Quake {                     // Quake (StartQuake / EndQuake): the 3D jolts a few pixels
            int mode = 0;                  // 0 still, 1 to its end, 2 held until EndQuake
            float amp = 0, now = 0, from = 0, to = 0; // pixels; percent of them now, from, to
            int frame = 0, frames = 1;
            float off[2] = {};             // this frame's offset (1024 x 768 pixels)
            uint32_t rng = 1;              // its own (the rules' random numbers stay untouched)
        } quake;
        struct Flash {                     // SetFade_back: a colour over the 3D, under the HUD
            bool on = false;
            float from = 0, to = 0, alpha = 0; // 0..255
            int at = 0, total = 1;         // 50 a frame
        } flash;
        int stageAction = 0;               // a boss's staged action playing (btlact_430..436), 0 none
        int stageCams = 0;                 // its cameras so far (+0x2c)
        int stageWait = 0;                 // its frames after the hit
    } battle;
    Battle::Quake fieldQuake;              // the field's (FieldCamera::setOffset: the view moves by it)
    std::vector<uint8_t> effBin;           // eff.bin: 100 effect packages (read when an effect is first needed)
    std::map<int, LoadedModel> effectModels; // eff.bin entries loaded (freed as a battle ends)
    std::vector<Effect> fieldEffects;      // eff3d_set_fld & co. on the field (32 at most)
    int effectHandles = 0;
    std::vector<std::array<int, 3>> frameEffects; // FrmPlayEff's: key (effect + 1) x (actor + 1), effect, handle
    std::array<int, 100> fieldEffectCount{};   // started on the field, by id (tests)
    int frameEffectCalls = 0, scriptEffectCalls = 0, scriptQuakes = 0;
    int texDispCalls = 0, tutoCalls = 0, tutoPage = -1, exclamationCalls = 0; // (tests)
    bool paramUp[2] = {};                  // Uty_Btl_ParamUp02 / 01 waiting for the next battle
    bool logoSeen = false;                 // G+0x1f85e6: the logos played (Uty_ChkLogo_flg)
    bool padArt = false;                   // Uty_DrawController: the pad's picture (flag 0)
    std::map<std::string, int> matAnimDraws;   // NPC models drawn with animated materials (tests)
    int battlesFought = 0;                 // this session (tests, status line)
    std::vector<int16_t> battleVoiceTable; // voiced data's sys.evt section +0x40: [speaker x 1000 + situation - 1] = clip
    cr::Hud battleHud;
    int choiceCursor = 0;                  // 0 = Yes, 1 = No
    int choiceAnim = 0;                    // its window's slide in (frames, 4 to settle)
    bool worldChoiceOpen = false;          // Util 1: pick a gate (_menuu_WldChoice*)
    int worldCursor = 0;
    cr::Hud choiceHud;
    cr::Hud nameHud;
    cr::Hud pageHud;                     // the dialogue's [E] n/m
    float captureAt[2] = {NAN, NAN};       // --capture ... --at x z
    bool showProps = true;                 // --no-props: skip the field's occluder geometry
    int captureMessage = 0;                // --capture ... --message <id>: show that message
    bool captureOptions = false;           // --capture ... --volume-menu
    bool newGame = false;                  // --new-game: start where a new game does
    int captureFrames = 0;                 // --capture ... --frames n: play n frames of scripts first
    int captureSettle = 0;                 // --settle n: draw the same moment n times first (temporal AA)
    bool captureSettleMoving = false;      // --settle-moving n: n frames 1/30 s apart instead
    int captureEvent = -1;                 // --event n: start the area's script n in the room first
    int captureArea = -1;                  // --area n: the event file for the map (routes share maps)
    std::vector<int> captureFlags;         // --set-flags a,b,...: story flags set before the map loads
    bool captureWinBattles = false;        // --win-battles: a battle the scripts start is won at once
    bool testNoTutorials = false;          // self-test: battles don't stop for their tutorials (EvrStart 2)
    int captureTalk = -1;                  // --talk-portrait n: (new game) the n-th hall conversation showing a portrait
    int captureMenu = 0;                   // --camp (1), --save-screen (2: save, 3: the title's load / new, 7: cleared), --char-select (4), --scenarios (5), --manual (6), --world-select (8), --last-battle (9)
    std::string captureCampView;           // --camp-view items|use|equip|slots|wear|status
    std::string captureFieldMenu;          // --field-menu closed|open|known|off|map[step]
    int captureSceneFx[3] = {-1, 0, 0};    // --scene-fx set effect [frames]
    std::string captureBattleView;         // --battle-view top|commands|targets|items|escape|hit|results
    std::string captureShop;               // --shop main|category|buy|sell|count|confirm|talk
    int captureTitlePair = 0;              // --title-new p: the title's Start, Character Select pair p, New
    int captureBattle[2] = {-1, -1};       // --capture ... --battle group stage
    bool showNpcs = true;                  // N (map mode): NPCs, treasure boxes, field objects
    struct NpcDraw {
        std::string model;
        cr::Mat4 world;
        int id = 0;
        int index = -1;      // its place in npcs (puzzle rooms repeat ids: every wall is NPC 129)
        int action = 0;      // fitting-action index; 0 = idle (loops), others play once and hold
        float time = 0;
        bool once = false;
        int loopFrom = 0, loopTo = 0; // Anim: loop frames a..b after the first play (0, 0 = whole)
        float alpha = 1, alphaFrom = 1, alphaTo = 1; // NpcFade / mdl_fadein / mdl_fadeout
        int fadeFrames = 0, fadeLeft = 0;
    };
    std::vector<NpcDraw> npcDraws;         // this map's visible NPCs
    // A puzzle room (PuzzleManager, game/puzzle.hpp): its objects - the room's NPCs that have
    // NPC table +20 bits - on a grid of 3 layers (0 the floor's switches, 1 blocks and gems, 2
    // the warp crystals), the touch ability's state and the ESP count
    struct Puzzle {
        int stage = -1;                    // -1: not in a puzzle room
        int esp = -1;                      // uses left (-1 no limit)
        struct Obj {
            int npc = -1;                  // index in npcs
            int type = 0, layer = 1, row = 0, col = 0;
            bool shown = true;
            int state = 1;                 // 1 idle, 2 gone (moving), 3 back; 0 out of play
            int pending = 0;               // 8: a teleport asked for
            int toRow = 0, toCol = 0, outFrames = 4, inFrames = 4, waitOut = 6, waitIn = 6;
            bool onSwitch = false;         // standing on a weigh switch (+0x30)
            bool on = false;               // a weigh switch pressed
            // NormalBlockPK: 0 idle, 1 rising, 2 held (bobbing), 3 sliding, 4 falling, 5 falling onto a switch
            int pk = 0, pkMsg = 0, pkTimer = 0, bob = 0;
            bool ghost = false;            // RedBlock / BlueBlock put away: drawn at 0x60 alpha, not solid
            bool hit = false;              // message 9 (Toughness) / 12 (a candle block's switch) waiting
            // CandleSwitch: 0 out, 1 catching (a neighbour's), 2 lighting, 3 lit, 4 going out
            // (a neighbour's), 5 cooling; csMsg 10 a tap, 11 a neighbour's; the beam from csFrom
            int cs = 0, csMsg = 0, csTimer = 0, csFrom = -1, beamT = 0;
            int flame = 0;                 // its flame's effect (0xc, looped)
            float vx = 0, vy = 0, vz = 0, baseY = 0;
        };
        std::vector<Obj> objs;
        std::vector<int> grid;             // [layer][row][col] -> objs index, -1 empty
        int tpState = 0, tpTimer = 0, tpTarget = -1; // Teleport: 1 vanishing, 2 appearing
        int pkState = 0, pkTarget = -1;    // Psychokinesis: 1 a block held, 2 waiting for it to land
        bool pkMoved = false;
        int pkEffect = 0;                  // effect 5 on the leader while a block is held
        float pkFromX = 0, pkFromY = 0;    // where the mouse took hold (screen pixels); -1e9 = by key
        bool busy = false;                 // +0x26: an ability under way
        int scene = 4, sceneTimer = 0;     // PuzzleScene: 4 playing, 2 fading out to reset, 3 fading in
        bool asked[2] = {};                // the two buttons' scripts (EvrStart 6 / 7) running
        int punished = 0;                  // SetPunishmentDispOn: the mark's frames (11), its blink phase (0..2)
        int punishPhase = 0;
        float punishX = 0, punishY = 0;    // its top-left (tex2d), 32 up-left of the touch
        int pop = 0;                       // menuf's +0x44: the count swells for 4 frames when it changes
        int shownEsp = -1000;              // +0x48: the count last shown
        bool hadEsp = false;               // +0x40: it has been above 0 (then the retry button blinks at 0)
        int blink = 0;                     // +0x3c: that blink, 0..10 (shown below 5)
        bool help = true;                  // +0x38 (MfMain_SetPzlHelpState): the pad's two buttons
        float tapX = -1, tapY = -1;        // the touch that asked (tex2d), < 0 from the keyboard
        int clock = 0;                     // frames the room has run (the flames' flicker)
    } puzzle;
    float playerAlpha = 1, playerAlphaFrom = 1, playerAlphaTo = 1; // mdl_set_alpha on the leader
    int playerFadeFrames = 0, playerFadeLeft = 0;
    cr::Vec3 puzzleStart;                  // where the leader came into the puzzle room (FieldPCCtrl::reset)
    struct CrystalCam { int npc = -1; int phase = 0; } crystalCam; // Uty_WarpCrystalCamera: 0 to the crystal, 1 back
    bool puzzleStartPending = false;
    int bgmNo = 0;                         // G+0x35530: the music last asked for (Uty_GetBgmNo)
    struct NowLoading { bool on = false; int frame = 0; } nowLoading; // NowLoadProc's spinner
    struct VitaLogo {                      // the Vita's boot opcodes 0x47..0x4a: a full-screen logo
        cr::Texture tex[2];                // by slot (0x47's slot + 16 in the original's Tex2d)
        int slot = -1, phase = 0, frame = 0, in = 0, hold = 0, out = 0;
        bool skip = false;                 // a tap / confirm in the hold: on to the fade-out
    } vitaLogo;
    struct TexDisp {                       // Uty_TexDisp (2016): a Tex2d slot's picture faded in, held, out
        bool on = false;
        int g[11] = {};                    // slot, x, y, u, v, w, h, fade-in, hold, fade-out, ?
        int state = 0, counter = 0, alpha = 0;
    } texDisp;
    struct TutoTex {                       // Uty_TutoTexDisp (2033) / MsgUty_TexDisp*: a tutorial's frames
        bool on = false;
        int state = 0, timer = 20, frame = 0, page = -1, pages = 0;
        int row[4] = {}, frames[4] = {};
        float x = 0, y = 0;                // tex2d, the picture's top-left (x2: the HD frame)
        cr::Texture tex;
    } tutoTex;
    bool exclamation = false;              // Uty_DrawExclamationMark: the dimmed screen and "!" (flag 0)
    struct PuzzleBanner {                  // Uty_PuzzleStart (call 2003): two pieces sliding apart into place
        bool on = false;
        float x1 = 100, y1 = 68, x2 = 156, y2 = 140; // tex2d
        int alpha = 0;
    } puzzleBanner;
    std::map<std::string, struct LoadedModel> npcModels; // NPC model cache by file
    std::map<int, std::vector<cr::TreasureBox>> treasureBoxes; // per area: selected boxes and items
    bool showPlayer = true;                // P: the party leader walking on the field
    int playerChar = 0;                    // O: which field model, f001..f008
    LoadedModel player;
    std::string playerName;
    cr::FieldWalker walker;
    bool havePlayer = false;
    float walkClock = 0;                   // movement runs in fixed 30 Hz steps like the original
    float walkExpDistance = 0;             // G+0x35520: walked with skill 174 worn, 10 units = 1 EXP banked
    struct FieldMenu {                     // menuf_mainPut / Prc: the corner button and the bar it opens
        bool open = false;                 // G+0x3553c
        int anim = 0;                      // the bar's 4 frames of sliding (in or out)
        int fade = 0;                      // +32 a frame (to 320) on the field, -32 while a scene runs
        float clock = 0;
    } fieldMenu;
    struct MapScreen {                     // __menuu_Map*: the gate's map, where the pair is, where to go
        bool open = false, closing = false;
        int fade = 0;                      // 4 frames in, 4 out
        int gate = -1;                     // map_D01..D04 (ReadMapMenu), the one in `tex`
        cr::Texture tex;
        float clock = 0;
        int spot[2] = {-1, -1};            // the pair's marker's top-left (tex2d), -1 none
        int marks[2][2] = {{-1, -1}, {-1, -1}}; // the story's two markers
        int blink = 0;                     // ComUty_PictAnimDraw's counter: 2 frames of 6
    } mapScreen;
    struct StaffRoll {                     // __menuu_StaffRoll* (Util 7): the credits over the ending's picture
        bool open = false;
        int state = 0, mode = 0, timer = 0;
        int next = 21;                     // +0x10: the next of block 9's 336 lines
        int countdown = 0;                 // +0x14: frames after the last line, then the panel goes
        bool fast = false;                 // +0x18: the corner button held (10 x)
        float panel = 0;                   // +0x1c: the black panel's x (tex2d), sliding to -280
        float picY = 0;                    // +0x174: the picture's y (logical pixels), up to -1022
        bool phone = false;                // the iPhone layout (2.2.0): tex2d units, 17 lines, its timing
        struct Line { bool on = false; float y = 0; int entry = 0; } lines[21];
        float win[5] = {}, winTo[5] = {};  // ComUty_WinDispOn / Off: each window's alpha
        cr::Texture tex;                   // EndBack0<mode + 1>_hd.mct
        float clock = 0;
    } staffRoll;
    struct WorldSelect {                   // __menuu_WldChoice* (Util 1): a gate, then its level band
        int state = 0, timer = 0;
        int pair = 0;                      // _MuCom_GetPartyNo
        std::vector<int> worlds;           // the gates offered (0..5) in the pair's order (+0x50 the mask)
        int levels = 2;                    // +0x58: 2 + the pair's eight level flags set
        int cursor = 0, top = 0, levelCursor = 0, levelTop = 0, confirm = 0;
        int world = 0, level = 0;          // +4 the gate focused / chosen, +8 the level band
        bool asking = false;               // +0x25: the board asks "Skip the first half?"
        bool skip = false;                 // +0x26: ... and Yes (flag 1)
        bool cancelled = false;            // +0x24: Back (flag 0)
        float win[11] = {}, winTo[11] = {}, fromX[11] = {}, fromY[11] = {};
        cr::Texture maps[6];               // MAP_01..06.mct (slots 0x11..)
        float clock = 0;
    } worldSelect;
    struct LastBattle {                    // __menuu_LastBt* (Util 8): which pair meets the next guardian
        bool open = false;
        int state = 0, timer = 0;
        int count = 0, mask = 0;           // rounds done, pairs used (the menu's statics, not saved)
        std::vector<int> pairs;            // those left to choose (0..3)
        int cursor = 0, chosen = -1, confirm = 0;
        float win[7] = {}, winTo[7] = {};
        cr::Texture tex;                   // hi / mizu / kaze / mu.mct: this round's guardian
        float clock = 0;
    } lastBattle;
    int modelIndex = 0;
    LoadedModel model;
    float yaw = 0.6f, pitch = 0.25f, zoom = 1.0f;
    bool zUp = false, dragging = false;
    bool playing = true;
    bool smoothFilter = false;  // original uses nearest filtering
    float speed = 1.0f;
    Uint64 lastTicks = 0;
};

// The menus' sounds: __effgo ids (sndeff.bin entries) as the original's *Prc functions use them
namespace se {
constexpr uint32_t kChoose = 2;  // a plate, button, toggle, Yes or No (ComUty_YesNoChoiceProc's own is 2 too)
constexpr uint32_t kBack = 3;    // the Back bar (ComUty_BackButtonProc); Options' < > page arrows on it as well
constexpr uint32_t kBuzzer = 4;  // refused: nothing to sell (BuySell_CommandProc), a count past its end
                                 // (ComUty_NumCountProc), a pair-only command alone (menub_mainPrc)
constexpr uint32_t kPage = 5;    // the Back bar's < > arrows (StartPrc, MuCamp_NextProc), a list flicked a page
constexpr uint32_t kCampOpen = 8, kCampClose = 9; // menuf_mainPrc's Camp button, CampMainPrc's Back bar
constexpr uint32_t kSaved = 11;  // SaveLoadPrc, after SaveData_Save
constexpr uint32_t kLoaded = 12; // SaveLoadPrc: Yes to Load / New
constexpr uint32_t kPaid = 22;   // ShopPrc
constexpr uint32_t kVolume = 40; // MuOptionSetInfo: the SE gauge let go (the BGM gauge is silent)
// The original is touch-only, so it has no cursor sound; keyboard cursor moves are the port's own
constexpr uint32_t kCursor = 4;
} // namespace se

void menuSound(App& app, uint32_t id) {
    app.sound.play(id);
    if (app.menuSoundLog.size() >= 64) app.menuSoundLog.erase(app.menuSoundLog.begin());
    app.menuSoundLog.push_back(id);
}

bool endsWith(const char* s, const char* ext) {
    const size_t n = SDL_strlen(s), e = SDL_strlen(ext);
    return n > e && SDL_strcasecmp(s + n - e, ext) == 0;
}

// Checks the package table without reading the whole file (the Vita's: read whole).
bool isModelFile(const App& app, const std::string& name) {
    if (app.vitaOnly) {
        std::vector<uint8_t> f;
        if (!app.vita.read(name, f) || f.size() < 8) return false;
        uint32_t off = 0;
        std::memcpy(&off, f.data(), 4);
        return off + 4 <= f.size() && std::memcmp(&f[off], "MCM_", 4) == 0;
    }
    const std::string path = app.dataDir + name;
    SDL_IOStream* io = SDL_IOFromFile(path.c_str(), "rb");
    if (!io) return false;
    Uint32 off = 0;
    char magic[4] = {};
    const bool ok = SDL_ReadU32LE(io, &off) && SDL_SeekIO(io, off, SDL_IO_SEEK_SET) == Sint64(off) &&
                    SDL_ReadIO(io, magic, 4) == 4 && SDL_memcmp(magic, "MCM_", 4) == 0;
    SDL_CloseIO(io);
    return ok;
}

SDL_EnumerationResult SDLCALL collectAssets(void* user, const char*, const char* fname) {
    App& app = *static_cast<App*>(user);
    if (endsWith(fname, ".mct")) app.textures.push_back(fname);
    else if (endsWith(fname, ".mca")) app.animFiles.push_back(fname);
    else if (endsWith(fname, ".cmp")) app.maps.push_back(fname);
    else if (endsWith(fname, ".bin") && isModelFile(app, fname)) app.models.push_back(fname);
    return SDL_ENUM_CONTINUE;
}

int wrap(int i, size_t n) { return n ? int(((i % int(n)) + int(n)) % int(n)) : 0; }

void updateTitle(App& app) {
    std::string title = "Chaos Rings — ";
    if (app.mode == Mode::Textures && !app.textures.empty()) {
        title += app.textures[app.texIndex] + "  (" + std::to_string(app.currentTex.width) + "x" +
                 std::to_string(app.currentTex.height) + " " + cr::mctFormatName(app.texInfo.format) + ")  " +
                 std::to_string(app.texIndex + 1) + "/" + std::to_string(app.textures.size());
    } else if (app.mode == Mode::Models && !app.models.empty()) {
        const LoadedModel& m = app.model;
        title += app.models[app.modelIndex] + "  (" + std::to_string(m.meshes.size()) + " meshes, " +
                 std::to_string(m.triangles) + " tris, " + (app.zUp ? "Z-up" : "Y-up") +
                 (app.smoothFilter ? ", smooth" : "") + ")  " +
                 std::to_string(app.modelIndex + 1) + "/" + std::to_string(app.models.size());
        if (m.action < 0) {
            title += "  |  rest pose (" + std::to_string(m.actions.size()) + " animations)";
        } else {
            const cr::Action& a = m.actions[m.action];
            title += "  |  anim " + std::to_string(m.action + 1) + "/" + std::to_string(m.actions.size()) + " " +
                     a.name + ", " + std::to_string(int(a.duration)) + " frames" +
                     (app.playing ? "" : " [paused]") +
                     (app.speed != 1.0f ? "  x" + std::to_string(app.speed).substr(0, 4) : "");
        }
    }
    if (app.mode == Mode::Maps && !app.maps.empty()) {
        title = "Chaos Rings — " + app.maps[app.mapIndex] + "  (" + std::to_string(app.collisionPolys) +
                " collision polygons" + (app.mapProps.empty() ? "" : ", props " + app.mapProps) +
                (app.showCollision ? "" : ", collision hidden") +
                (app.showBackground ? (app.background.id ? ", background" : ", no background") : "") +
                (app.showBackground && app.foreground.id ? " + foreground" : "") +
                (app.showNpcs ? ", " + std::to_string(app.npcDraws.size()) + " npcs" : ", npcs hidden") +
                (app.havePlayer ? ", player " + app.playerName.substr(0, 4)
                                : app.showPlayer ? "" : ", player hidden (P)") + ")  " +
                (app.area >= 0 ? std::string(cr::kAreaEventFile[app.area]).substr(0, 8) + " map " +
                                     std::to_string(app.mapNumber) + "  " : "") +
                (app.lastWarp.empty() ? "" : app.lastWarp + "  ") +
                (app.lastEvent.empty() ? "" : app.lastEvent + "  ") +
                (app.haveStageCam
                     ? "cam: map " + std::to_string(app.stageCam.mapNumber) + " (" + app.stageCam.eventFile + ", " +
                           std::to_string(app.stageCamIndex + 1) + "/" + std::to_string(app.stageCams.size()) +
                           ", fits " + std::to_string(int(app.stageFit[app.stageCamIndex] * 100 + 0.5f)) + "%" +
                           (app.stageCamBorrowed ? ", borrowed" : "") + ") fov " +
                           std::to_string(int(app.stageCam.fovDegrees + 0.5f)) + "  "
                     : "no camera  ") + std::to_string(app.mapIndex + 1) + "/" +
                std::to_string(app.maps.size());
    }
    SDL_SetWindowTitle(app.window, title.c_str());
    app.status = title.substr(std::string("Chaos Rings — ").size());
    SDL_Log("%s", title.c_str());
}

void showTexture(App& app, int index) {
    if (app.textures.empty()) return;
    app.texIndex = wrap(index, app.textures.size());
    app.renderer.destroy(app.currentTex);
    const std::string& name = app.textures[app.texIndex];
    std::vector<uint8_t> data;
    cr::Image img;
    std::string err;
    if (!cr::readFile(app.dataDir + name, data) || !cr::decodeMct(data, img, &app.texInfo, &err)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s: %s", name.c_str(), err.c_str());
        return;
    }
    app.currentTex = app.renderer.upload(img);
    if (app.mode == Mode::Textures) updateTitle(app); // startup loads a texture before --maps/--models
}

// With HD textures on: the upscaled replacement for `img` (found by its pixels' hash), drawn at
// the original's size with a mip chain
void applyHd(App& app, cr::Image& img) {
    if (!app.video.hdTextures || app.hdDir.empty() || img.width <= 0) return;
    cr::Image hd;
    if (!cr::loadHdImage(app.hdDir, img, hd)) {
        ++app.hdMissing;
        return;
    }
    hd.logicalW = img.width;
    hd.logicalH = img.height;
    hd.mipmaps = true;
    img = std::move(hd);
    ++app.hdLoaded;
}

bool loadOriginalImage(App& app, const std::string& mct, cr::Image& img);

// A full-image texture by its .mct name (loadOriginalImage), its HD replacement when that's on
bool loadImage(App& app, const std::string& mct, cr::Image& img) {
    if (!loadOriginalImage(app, mct, img)) return false;
    applyHd(app, img);
    return true;
}

// A full-image texture by its .mct name: the Vita's lossless .gxt of the same name when --vita is
// given (same sizes; 8 bits per channel where the iPad files use palettes, PVRTC or RGBA4444),
// else the data folder's .mct.
bool loadOriginalImage(App& app, const std::string& mct, cr::Image& img) {
    // Art the Vita redrew for its own screen layout (crtool vita: > 12/255 from the iPad's) keeps
    // the iPhone / iPad version
    static const char* const kVitaRedrawn[] = {"MAPtitle_hd.mct", "chaos_hd.mct", "chara1_hd.mct", "chara2_hd.mct",
                                               "chara3_hd.mct", "daimei01_hd.mct", "daimei02_hd.mct", "title1_hd.mct",
                                               "title2_hd.mct", "t0107-b00a00.mct"};
    const bool redrawn = std::any_of(std::begin(kVitaRedrawn), std::end(kVitaRedrawn), [&](const char* n) { return mct == n; });
    if (app.vita.isOpen() && mct.size() > 4 && (!redrawn || app.vitaOnly)) { // (alone: its own screens too)
        std::vector<uint8_t> g;
        if (app.vita.read(mct.substr(0, mct.size() - 4) + ".gxt", g) && cr::decodeGxt(g, img)) { ++app.vitaTextures; return true; }
    }
    std::vector<uint8_t> data;
    return cr::readFile(app.dataDir + mct, data) && cr::decodeMct(data, img);
}

// An iOS data file's names in the Vita's cr1.psarc: x.mct -> x.gxt (and x.gxt for x_hd.mct, which
// the Vita names without the suffix: EndBack01, i001_C00, syuutyuusen), anything else as it is
std::vector<std::string> vitaNames(const std::string& name) {
    if (name.size() < 5 || name.compare(name.size() - 4, 4, ".mct") != 0) return {name};
    const std::string base = name.substr(0, name.size() - 4);
    std::vector<std::string> out = {base + ".gxt"};
    if (base.size() > 3 && base.compare(base.size() - 3, 3, "_hd") == 0) out.push_back(base.substr(0, base.size() - 3) + ".gxt");
    return out;
}

// Opens the Vita copy at `path` (its PCSG00497 folder, CR1, resources or cr1.psarc itself): the
// textures and models (loadOriginalImage, loadModelFile), the scripts through the data folder's
// overlay, and the sound folder's archives (hooked up by vitaAudio once the sound starts)
bool openVita(App& app, const std::string& path) {
    std::string err;
    std::string root = path;
    while (root.size() > 1 && root.back() == '/') root.pop_back();
    for (const char* rel : {"", "/CR1/resources/cr1.psarc", "/resources/cr1.psarc", "/cr1.psarc"})
        if (app.vita.open(root + rel, &err)) {
            const std::string p = root + rel; // the sound folder sits beside resources/
            const size_t res = p.rfind("/resources/");
            root = res == std::string::npos ? p.substr(0, p.rfind('/')) : p.substr(0, res);
            break;
        }
    if (!app.vita.isOpen()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "vita: %s (%s)", err.c_str(), path.c_str());
        return false;
    }
    app.vitaPath = app.vitaLoaded = path;
    app.vitaRoot = root;
    if (app.vitaOnly) app.dataDir = root + "/resources/"; // (the folder of cr1.psarc: the data's own paths)
    for (const char* sound : {"/sound/", "/../sound/"}) {
        if (app.vitaBgm.open(root + sound + "bgm.psarc")) {
            app.vitaSound = root + sound;
            app.vitaVoice[0].open(root + sound + "voiceevnt.psarc");
            app.vitaVoice[1].open(root + sound + "voicebtl.psarc");
            break;
        }
    }
    // Every script, its text and voice cues: 2.2.0's code to the byte but for j001_t99's boot (return
    // entry 0), the Vita's own start-up - its logos through opcodes 0x47..0x4a (played), its save
    // and trophy housekeeping (0x40..0x46, nothing here)
    // Alone (no iOS data) every file comes from it: textures by their .gxt (decodeMct takes the
    // Vita's form), an iOS "_hd" name the Vita lacks by the name without it, the boot area's script
    // too (the event runner passes its own opcodes by)
    if (app.vitaOnly)
        cr::setDataOverlay(app.dataDir, [&app](const std::string& name, std::vector<uint8_t>& out) {
            for (const std::string& n : vitaNames(name))
                if (app.vita.read(n, out)) return true;
            return false;
        });
    else
        cr::setDataOverlay(app.dataDir, [&app](const std::string& name, std::vector<uint8_t>& out) {
            return name.size() > 4 && name.compare(name.size() - 4, 4, ".evt") == 0 && app.vita.read(name, out);
        });
    SDL_Log("vita: %zu files in %s; music %s, voices %zu / %zu", app.vita.size(), path.c_str(), app.vitaBgm.isOpen() ? "yes" : "no",
            app.vitaVoice[0].size(), app.vitaVoice[1].size());
    return true;
}

// The sound effects: the data's bank, or the Vita's when it is the whole game
bool startSound(App& app) {
    return app.sound.init(app.dataDir, app.vitaOnly && !app.vitaSound.empty() ? app.vitaSound + "sndeff.bnk" : std::string());
}

// The Vita's voices and music to the sound system and the music player
void vitaAudio(App& app) {
    for (int b = 0; b < 2; ++b) {
        if (!app.vitaVoice[b].isOpen()) continue;
        const cr::PsarcArchive* a = &app.vitaVoice[b];
        app.sound.setVitaLines(b == 1, a->names(), [a](const std::string& n, std::vector<uint8_t>& out) { return a->read(n, out); });
    }
    if (app.vitaBgm.isOpen()) {
        const cr::PsarcArchive* a = &app.vitaBgm;
        app.music.setVitaSource([a](const std::string& n, std::vector<uint8_t>& out) { return a->read(n, out); });
    }
}

void saveMix(const App& app);

// Options' Vita Assets: the folder dialog answers on its own thread; the menu takes it next frame
std::mutex vitaPickMutex;
std::string vitaPicked;
bool vitaPickedReady = false;

void chooseVitaFolder(App& app) {
    SDL_ShowOpenFolderDialog(
        [](void*, const char* const* list, int) {
            if (!list || !list[0]) return; // cancelled or failed
            std::lock_guard<std::mutex> lock(vitaPickMutex);
            vitaPicked = list[0];
            vitaPickedReady = true;
        },
        nullptr, app.window, app.vitaPath.empty() ? nullptr : app.vitaPath.c_str(), false);
}

void takeVitaFolder(App& app) {
    std::string path;
    {
        std::lock_guard<std::mutex> lock(vitaPickMutex);
        if (!vitaPickedReady) return;
        vitaPickedReady = false;
        path = vitaPicked;
    }
    cr::PsarcArchive probe;
    bool found = false;
    for (const char* rel : {"", "/CR1/resources/cr1.psarc", "/resources/cr1.psarc", "/cr1.psarc"})
        if ((found = probe.open(path + rel))) break;
    if (!found) { app.vitaNote = "No Vita data (CR1/resources/cr1.psarc) in that folder"; return; }
    app.vitaPath = path;
    app.vitaNote = path == app.vitaLoaded ? "In use" : "Used from the next start: " + path;
    saveMix(app);
}

std::string settingsPath();

// settings.ini's vita= (the whole line: a path), read before the data loads
std::string storedVitaPath() {
    size_t size = 0;
    char* text = static_cast<char*>(SDL_LoadFile(settingsPath().c_str(), &size));
    if (!text) return {};
    std::string all(text, size), path;
    SDL_free(text);
    for (size_t at = 0; at < all.size();) {
        const size_t e = std::min(all.find('\n', at), all.size());
        if (all.compare(at, 5, "vita=") == 0) path = all.substr(at + 5, e - at - 5);
        at = e + 1;
    }
    while (!path.empty() && (path.back() == '\r' || path.back() == ' ')) path.pop_back();
    return path;
}

void unloadLoaded(App& app, LoadedModel& lm) {
    for (auto& m : lm.meshes) app.renderer.destroy(m);
    for (auto& t : lm.textures) app.renderer.destroy(t);
    lm = {};
}

void unloadModel(App& app) {
    app.renderer.destroy(app.background);
    app.renderer.destroy(app.foreground);
    app.renderer.destroy(app.collision);
    app.renderer.destroy(app.triggerMesh);
    app.renderer.destroy(app.puzzleBeams);
    app.collisionPolys = 0;
    unloadLoaded(app, app.model);
}

// Loads a model package (and its fitting animations) into `out`.
void loadModelFile(App& app, const std::string& name, LoadedModel& out, bool character) {
    std::vector<uint8_t> data;
    cr::Model m;
    std::string err;
    // --vita: the Vita's package of the same name (same meshes, lossless textures)
    const bool fromVita = app.vita.isOpen() && app.vita.read(name, data) && cr::loadModelPackage(data, m, &err);
    if (fromVita) ++app.vitaTextures;
    else if (!cr::readFile(app.dataDir + name, data) || !cr::loadModelPackage(data, m, &err)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s: %s", name.c_str(), err.c_str());
        updateTitle(app);
        return;
    }
    // Smooth Models: characters, NPCs and monsters (field props and battle stages stay as built)
    if (character && app.video.smoothModels >= 2) cr::smoothModel(m, app.video.smoothModels);
    for (const auto& img : m.textures) {
        if (app.video.hdTextures) {
            cr::Image hd = img;
            applyHd(app, hd);
            out.textures.push_back(app.renderer.upload(hd));
        } else {
            out.textures.push_back(app.renderer.upload(img));
        }
    }
    for (const auto& mesh : m.meshes) out.meshes.push_back(app.renderer.upload(mesh));
    out.triangles = m.stats.triangles;

    // Appearances that name a package action (refGid: segment 1 colour, 6 / 8 texture motion) -
    // the scenes' effect props (ee*, fe*): keep every package action for them, as effects do
    for (const auto& mat : m.materials)
        if (int32_t(mat.refGid) >= 0) { out.packageActions = m.actions; break; }
    // Animations: those stored in the package plus standalone <name>e*.mca files,
    // keeping only actions that animate this skeleton (one segment per node)
    // (or, for each list's motion - its first action, "N.1" - one a few segments off)
    for (auto& a : m.actions) {
        const bool motion = a.name.size() > 2 && a.name.compare(a.name.size() - 2, 2, ".1") == 0;
        const bool fits = cr::actionFits(m.skeleton, a) || (motion && cr::actionNearlyFits(m.skeleton, a));
        if (SDL_getenv("CR_FIT_DEBUG") && !cr::actionFits(m.skeleton, a)) {
            float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
            if (fits) { // its pose halfway through: the bounds stay the model's size when the nodes line up
                std::vector<cr::Mat4> w;
                std::vector<float> sk;
                cr::computeWorld(m.skeleton, &a, a.duration / 2, w);
                for (const auto& mesh : m.meshes) {
                    cr::skinMesh(m, mesh, w, sk);
                    for (size_t k = 0; k + 2 < sk.size(); k += 3)
                        for (int c = 0; c < 3; ++c) lo[c] = std::min(lo[c], sk[k + c]), hi[c] = std::max(hi[c], sk[k + c]);
                }
            }
            std::printf("%s: %s (%zu segments, %zu nodes) %s", name.c_str(), a.name.c_str(), a.segments.size(),
                        m.skeleton.nodes.size(), fits ? "kept" : "left out");
            if (fits) std::printf(", halfway (%.1f %.1f %.1f)-(%.1f %.1f %.1f)", lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
            std::printf("\n");
        }
        if (fits) out.actions.push_back(std::move(a));
    }
    out.builtin = int(out.actions.size());
    const std::string stem = name.substr(0, name.size() - 4);
    for (const auto& file : app.animFiles) {
        if (file.rfind(stem + "e", 0) != 0) continue;
        std::vector<uint8_t> ad;
        std::vector<cr::Action> acts;
        if (!cr::readFile(app.dataDir + file, ad) || !cr::parseMca(ad.data(), ad.size(), 0, acts)) continue;
        for (size_t i = 0; i < acts.size(); ++i) {
            if (!cr::actionFits(m.skeleton, acts[i])) continue;
            acts[i].name = file.substr(0, file.size() - 4) + "." + std::to_string(i + 1);
            out.actions.push_back(std::move(acts[i]));
        }
    }
    m.actions.clear();
    out.action = out.actions.empty() ? -1 : 0;
    out.cpu = std::move(m);
    const cr::Model& cpu = out.cpu;
    const auto& b0 = cpu.boundsMin;
    const auto& b1 = cpu.boundsMax;
    out.center = {(b0[0] + b1[0]) / 2, (b0[1] + b1[1]) / 2, (b0[2] + b1[2]) / 2};
    const cr::Vec3 ext{b1[0] - b0[0], b1[1] - b0[1], b1[2] - b0[2]};
    out.radius = std::max(0.5f * std::sqrt(cr::dot(ext, ext)), 0.01f);
}

void showModel(App& app, int index) {
    app.renderer.resetHistory(); // a cut: the upscaler starts over
    if (app.models.empty()) return;
    app.modelIndex = wrap(index, app.models.size());
    unloadModel(app);
    loadModelFile(app, app.models[app.modelIndex], app.model, true);
    updateTitle(app);
}

void placePlayer(App& app);
int triggerAt(const App& app, const cr::Vec3& p);
void showMessages(App& app, const std::vector<int>& ids);
bool checkTriggers(App& app, bool instant = false);
bool interact(App& app, bool instant = false);

// Field collision as a flat-shaded mesh: floors green-grey (tinted by surface type),
// walls tan, so the walkable layout reads clearly.
void enterMap(App& app);
void buildNpcDraws(App& app);
void buildWalkCollision(App& app);
cr::FieldNpc* findNpc(App& app, int id);
std::string itemName(const App& app, int item);
void startNewGame(App& app);
void bootTitle(App& app);
void padButton(App& app, int b);
bool padMenus(const App& app);
void offerResume(App& app);
void openSaveScreen(App& app, bool load);
void openMapScreen(App& app);
cr::GameState allPairsFlags(const App& app);
void newPairParty(App& app, int pair, bool clearData);
void memberHpMp(const App& app, int c, int& hp, int& hpMax, int& mp, int& mpMax, const cr::GameState* gs = nullptr);
void drawFaceBoard(App& app, cr::MenuPainter& m, float x, float y, int c, int hp, int hpMax, int mp, int mpMax, float a,
                   const cr::GameState* gs = nullptr);
void drawShopNumber(cr::MenuPainter& m, int value, float x, float y, int digits, float step, float alpha, int style = 0,
                    bool left = false);
void openCharSelect(App& app);
void drawCharArt(App& app, float alpha, bool banner, float grey = 1);
int plateIcon(int word);
// The port's own key hints under the menus: not in a Release build (F3 shows them, with the status
// text at the top), nor in the iPhone layout - its screen has no room below
bool keyHints(const App& app);
void openManual(App& app);
void openScenarios(App& app);
void openShop(App& app);
void startBattle(App& app, const cr::BattleStart& start, int flags);
int partyLevel(const App& app);
std::string sysText(const App& app, int id, const std::string& name = std::string());
bool actorPos(App& app, int id, cr::Vec3& out);
void setActor(App& app, int id, cr::Vec3 p, const float* yaw, bool keepY);

// Shows a field by collision file. `forcedMap` / `forcedArea` select the exact map (and so
// camera and event lists) when arriving by warp; otherwise the best-framing camera is used.
void showMap(App& app, int index, int forcedMap, int forcedArea) {
    app.renderer.resetHistory(); // a cut: the upscaler starts over
    if (app.maps.empty()) return;
    app.booting = false;
    app.titleOpen = false;
    app.renderer.destroy(app.charSelectArt); // (the title's save screen showed it)
    app.mapIndex = wrap(index, app.maps.size());
    unloadModel(app);
    const std::string& name = app.maps[app.mapIndex];
    std::vector<uint8_t> data;
    cr::Collision col;
    std::string err;
    if (!cr::readFile(app.dataDir + name, data) || !cr::parseCollision(data, col, &err)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s: %s", name.c_str(), err.c_str());
        updateTitle(app);
        return;
    }
    // Props for this field: dXXXX.bin next to dXXXX_10.cmp
    const std::string field = name.substr(0, name.find('_'));
    auto loadLayer = [&](const std::string& file, cr::Texture& out) {
        cr::Image img;
        if (loadImage(app, file, img)) out = app.renderer.upload(img);
    };
    // Layer names vary (the game's file list): dXXXX-b00a00, tXXXX_b00a00 (towns, d0512/d0524/
    // d0525) or a plain dXXXX.mct (d0108, d0117, d0208, d0219, d0307, d0320)
    for (const char* suffix : {"-b00a00.mct", "_b00a00.mct", ".mct"})
        if (!app.background.id) loadLayer(field + suffix, app.background);
    for (const char* suffix : {"-f00a00.mct", "_f00a00.mct"})
        if (!app.foreground.id) loadLayer(field + suffix, app.foreground);
    SDL_Log("%s layers: background %dx%d (id %u), foreground %dx%d", field.c_str(), app.background.width,
            app.background.height, app.background.id, app.foreground.width, app.foreground.height);

    app.mapProps.clear();
    if (std::find(app.models.begin(), app.models.end(), field + ".bin") != app.models.end()) {
        app.mapProps = field + ".bin";
        loadModelFile(app, app.mapProps, app.model, false);
    }

    cr::Mesh mesh;
    const float light[3] = {0.37f, 0.86f, 0.35f};
    for (const auto& poly : col.polygons) {
        const float d = poly.normal[0] * light[0] + poly.normal[1] * light[1] + poly.normal[2] * light[2];
        const float shade = 0.55f + 0.45f * std::max(0.0f, d);
        float rgb[3];
        if (poly.isFloor()) {
            const float t = float((poly.attr1 >> 8) & 7) / 4.0f; // surface type tint
            rgb[0] = 0.40f + 0.25f * t; rgb[1] = 0.62f; rgb[2] = 0.42f - 0.15f * t;
        } else {
            rgb[0] = 0.75f; rgb[1] = 0.58f; rgb[2] = 0.40f;
        }
        const uint32_t base = uint32_t(mesh.positions.size() / 3);
        for (int i = 0; i < poly.count; ++i) {
            const float* v = &col.vertices[size_t(poly.index[i]) * 3];
            mesh.positions.insert(mesh.positions.end(), {v[0], v[1], v[2]});
            mesh.uvs.insert(mesh.uvs.end(), {0.0f, 0.0f});
            for (int c = 0; c < 3; ++c) mesh.colors.push_back(uint8_t(std::min(255.0f, rgb[c] * shade * 255.0f)));
            mesh.colors.push_back(255);
        }
        for (int i = 1; i + 1 < poly.count; ++i) mesh.indices.insert(mesh.indices.end(), {base, base + i, base + i + 1});
    }
    app.collision = app.renderer.upload(mesh);

    // Cameras: a collision file can be shared by several map numbers (p0001_10 serves
    // the 16 puzzle rooms and a dungeon room). Pick the camera that frames it best;
    // M cycles through the others.
    app.stageCams.clear();
    app.stageFit.clear();
    app.stageCamBorrowed = false;
    std::vector<int> candidates = forcedMap >= 0 ? std::vector<int>{forcedMap} : cr::mapNumbersFor(name);
    auto tryCandidates = [&]() {
    for (int mapNo : candidates) {
        cr::StageCamera sc;
        if (!(forcedArea >= 0 && cr::loadStageCamera(app.dataDir, mapNo, sc, nullptr, forcedArea)) &&
            !cr::loadStageCamera(app.dataDir, mapNo, sc))
            continue;
        const float aspect = sc.viewHeight > 0 ? float(sc.viewWidth) / float(sc.viewHeight) : 1.0f;
        const cr::Mat4 vp = cr::Mat4::perspective(sc.fovDegrees * 3.14159265f / 180.0f, aspect, 10.0f, 4000.0f) *
                            cr::Mat4::lookAt({sc.eye[0], sc.eye[1], sc.eye[2]},
                                             {sc.target[0], sc.target[1], sc.target[2]}, {0, 1, 0});
        int inside = 0;
        const size_t nv = col.vertices.size() / 3;
        for (size_t v = 0; v < nv; ++v) {
            const float* q = &col.vertices[v * 3];
            const float* m = vp.m;
            const float cx = m[0] * q[0] + m[4] * q[1] + m[8] * q[2] + m[12];
            const float cy = m[1] * q[0] + m[5] * q[1] + m[9] * q[2] + m[13];
            const float cw = m[3] * q[0] + m[7] * q[1] + m[11] * q[2] + m[15];
            if (cw > 0 && std::fabs(cx / cw) <= 1.05f && std::fabs(cy / cw) <= 1.05f) ++inside;
        }
        app.stageCams.push_back(sc);
        app.stageFit.push_back(nv ? float(inside) / float(nv) : 0.0f);
    }
    };
    tryCandidates();
    if (app.stageCams.empty()) {
        // e.g. p0002/p0004/p0006-8: puzzle-room variants with no record of their own
        candidates.clear();
        for (int mapNo = 0; mapNo < cr::kNumMaps; ++mapNo)
            if (name.compare(0, 2, cr::kMapCollisionFile[mapNo], 2) == 0 && name != cr::kMapCollisionFile[mapNo])
                candidates.push_back(mapNo);
        tryCandidates();
        app.stageCamBorrowed = !app.stageCams.empty();
    }
    app.haveStageCam = !app.stageCams.empty();
    app.stageCamIndex = 0;
    for (size_t k = 1; k < app.stageFit.size(); ++k)
        if (app.stageFit[k] > app.stageFit[app.stageCamIndex] + 1e-4f) app.stageCamIndex = int(k);
    if (app.haveStageCam) app.stageCam = app.stageCams[app.stageCamIndex];
    app.collisionPolys = int(col.polygons.size());
    // Frame the collision (props live inside it)
    const auto& b0 = col.boundsMin;
    const auto& b1 = col.boundsMax;
    app.model.center = {(b0[0] + b1[0]) / 2, (b0[1] + b1[1]) / 2, (b0[2] + b1[2]) / 2};
    const cr::Vec3 ext{b1[0] - b0[0], b1[1] - b0[1], b1[2] - b0[2]};
    app.model.radius = std::max(0.5f * std::sqrt(cr::dot(ext, ext)), 0.01f);
    app.collisionData = std::move(col);
    app.mapNumber = app.haveStageCam ? app.stageCam.mapNumber : (candidates.empty() ? -1 : candidates[0]);
    app.area = app.haveStageCam ? app.stageCam.area : cr::areaForMap(app.mapNumber, forcedArea);
    if (forcedMap >= 0) { app.mapNumber = forcedMap; app.area = forcedArea >= 0 ? forcedArea : app.area; }
    enterMap(app);
    placePlayer(app);
    updateTitle(app);
}

void showMap(App& app, int index) { showMap(app, index, -1, -1); }

// Kill 0 hides the party leader for a scene; Born 0 brings it back
bool playerShown(const App& app) {
    auto it = app.npcShown.find(0);
    return it == app.npcShown.end() || it->second;
}

bool npcActive(const App& app, const cr::FieldNpc& n) {
    // Born / Kill override the flags until the room reloads
    if (auto it = app.npcShown.find(n.id); it != app.npcShown.end()) return it->second;
    // npc_setup hides an NPC whose show-flag is clear (guess for the hide flag: hidden once set)
    return (n.flagShow < 0 || app.game.flag(n.flagShow)) && (n.flagHide < 0 || !app.game.flag(n.flagHide));
}

namespace puzzle { void enter(App& app); }
cr::Mat4 npcWorld(const cr::FieldNpc& n);

// npcActive, but a puzzle room's objects show as the puzzle has them
bool npcVisible(const App& app, size_t index) {
    if (app.puzzle.stage >= 0)
        for (const App::Puzzle::Obj& o : app.puzzle.objs)
            if (o.npc == int(index)) return o.shown;
    return index < app.npcs.size() && npcActive(app, app.npcs[index]);
}

// npcVisible, or a red / blue block put away (npc_kill, then mdl_set_alpha keeps it drawn)
bool npcDrawn(const App& app, size_t index) {
    if (app.puzzle.stage >= 0)
        for (const App::Puzzle::Obj& o : app.puzzle.objs)
            if (o.npc == int(index)) return o.shown || o.ghost;
    return npcVisible(app, index);
}

// Loads the area's event file if needed, the map's NPC list, and the trigger-box overlay.
// _bgmgo: the music command; a play request (no stop / fade bits) is the track Uty_GetBgmNo reports
void playBgm(App& app, uint32_t v) {
    if (!(v & 0x2e000000u)) app.bgmNo = int(v & 0xff);
    app.music.command(v);
}

void enterMap(App& app) {
    app.npcs.clear();
    app.fieldEffects.clear();
    app.frameEffects.clear();
    app.npcWarps.clear();
    app.renderer.destroy(app.triggerMesh);
    app.lastKick = 0;
    app.lastEvent.clear();
    app.npcShown.clear();
    app.moves.clear();
    app.placedNpcs.clear();
    app.actorSlots.clear();
    app.cam = App::ScriptCam();
    app.scroll = App::FieldScroll();
    if (app.area < 0 || app.mapNumber < 0) return;
    if (app.events.area() != app.area) {
        std::string err;
        if (!app.events.load(app.dataDir, app.area, &err)) {
            // Once per area: 2.2.0 dropped the two test areas (j001_t91 / j001_d91)
            static std::set<int> reported;
            if (reported.insert(app.area).second) SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", err.c_str());
            return;
        }
    }
    app.npcs = app.events.npcs(app.mapNumber);
    { // fldencount_init
        cr::EncounterTable table;
        const bool have = cr::encounterTable(app.events, app.mapNumber, table);
        app.encounters.enterMap(have ? &table : nullptr);
    }
    app.pendingEntry = app.area != app.entryArea ? App::Entry::Area : App::Entry::Room;
    if (app.area != app.entryArea) app.game.frmScene = app.game.frmRecord = 0; // offsets into the old file
    app.entryArea = app.area;
    // Music: each area's usual track (what its entry script mostly plays: dungeons d01-d04 ->
    // 2-5, d05 -> 17, towns -> 15, puzzle rooms -> 6; event 0 itself isn't run yet). Scripts
    // change it with Bgm.
    if (app.area != app.musicArea) {
        app.musicArea = app.area;
        const std::string ev = cr::kAreaEventFile[app.area];
        int track = -1;
        if (ev.compare(5, 2, "d0") == 0) track = ev[7] == '5' ? 17 : ev[7] - '1' + 2;
        else if (ev.compare(5, 3, "t01") == 0) track = 15;
        else if (ev.compare(5, 3, "p01") == 0) track = 6;
        else if (ev.compare(5, 3, "t91") == 0) track = 1;
        else if (ev.compare(5, 3, "d91") == 0) track = 4;
        if (track >= 0) playBgm(app, uint32_t(track));
    }
    // Treasure boxes: chosen once per area (randomSelect), flags set per map (setup)
    if (!app.treasureBoxes.count(app.area)) {
        // route from the event file (j001/j003/j005/j007), level from the dungeon number (d01 = 0;
        // the original keeps a per-route progress byte in the save data)
        const std::string ev = cr::kAreaEventFile[app.area];
        const int route = (ev[3] - '1') / 2;
        const int level = ev[5] == 'd' && ev[6] == '0' ? ev[7] - '1' : 0;
        app.treasureBoxes[app.area] = cr::selectTreasureBoxes(app.events, uint32_t(app.area) + 1, level, route);
    }
    cr::applyTreasureFlags(app.events, app.npcs, app.treasureBoxes[app.area], app.game);
    puzzle::enter(app); // PuzzleScene::init in a puzzle room (stage -1 elsewhere)
    app.puzzleStartPending = app.puzzle.stage >= 0;
    buildNpcDraws(app);
    buildWalkCollision(app);
    cr::Mesh mesh;
    for (const cr::FieldNpc& n : app.npcs) {
        bool warps = false;
        if (n.touchTrigger()) {
            cr::GameState trial = app.game; // dry run on a copy: does touching it lead somewhere?
            warps = cr::runEvent(app.events, n.id, trial, app.area, app.mapNumber).warp.valid;
        }
        app.npcWarps.push_back(warps);
        if (!n.touchTrigger() || !npcActive(app, n)) continue;
        // Box from the floor up: magenta = exit, yellow = other touch event
        const float cx = n.boxCenterX(), cz = n.boxCenterZ(), hx = n.box[0] / 2, hz = n.box[2] / 2;
        const float y0 = n.pos[1] + n.offset[1], y1 = y0 + std::max(n.box[1], 1.0f);
        const uint8_t rgb[3] = {uint8_t(warps ? 230 : 240), uint8_t(warps ? 60 : 210), uint8_t(warps ? 220 : 40)};
        const float c[8][3] = {{cx - hx, y0, cz - hz}, {cx + hx, y0, cz - hz}, {cx + hx, y0, cz + hz}, {cx - hx, y0, cz + hz},
                               {cx - hx, y1, cz - hz}, {cx + hx, y1, cz - hz}, {cx + hx, y1, cz + hz}, {cx - hx, y1, cz + hz}};
        static const int faces[6][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
        for (const auto& f : faces) {
            const uint32_t base = uint32_t(mesh.positions.size() / 3);
            for (int k : f) {
                mesh.positions.insert(mesh.positions.end(), {c[k][0], c[k][1], c[k][2]});
                mesh.uvs.insert(mesh.uvs.end(), {0.0f, 0.0f});
                mesh.colors.insert(mesh.colors.end(), {rgb[0], rgb[1], rgb[2], 255});
            }
            mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        }
    }
    if (!mesh.indices.empty()) app.triggerMesh = app.renderer.upload(mesh);
}

// The touch trigger box containing the player, 0 if none (RectWrapper::checkEventKick: x/z only).
int triggerAt(const App& app, const cr::Vec3& p) {
    for (const cr::FieldNpc& n : app.npcs)
        if (n.touchTrigger() && npcActive(app, n) && std::fabs(p.x - n.boxCenterX()) <= n.box[0] / 2 &&
            std::fabs(p.z - n.boxCenterZ()) <= n.box[2] / 2)
            return n.id;
    return 0;
}

// Arrives at a warp destination: loads the map if it changes, places and turns the player.
bool performWarp(App& app, const cr::WarpRequest& w, int fromEvent) {
    const int area = w.area >= 0 ? w.area : app.area;
    if (w.map < 0 || w.map >= cr::kNumMaps) return false;
    const auto it = std::find(app.maps.begin(), app.maps.end(), std::string(cr::kMapCollisionFile[w.map]));
    if (it == app.maps.end()) return false;
    const int fromMap = app.mapNumber;
    if (w.map != app.mapNumber || area != app.area) showMap(app, int(it - app.maps.begin()), w.map, area);
    if (!app.havePlayer) return false;
    cr::Vec3 p{w.pos[0], w.pos[1], w.pos[2]};
    float y;
    if (cr::floorHeight(app.collisionData, p.x, p.y, p.z, 5.0f, y) ||
        cr::floorHeight(app.collisionData, p.x, p.y, p.z, 1000.0f, y)) {
        p.y = y;
    } else {
        // A few destinations sit just past a floor edge (the original's floor search snaps
        // them in); take the nearest floor point within 20 units, else keep the spawn point
        float best = 1e30f;
        cr::Vec3 q = app.walker.pos;
        for (float dx = -20; dx <= 20; dx += 0.5f)
            for (float dz = -20; dz <= 20; dz += 0.5f)
                if (dx * dx + dz * dz < best &&
                    cr::floorHeight(app.collisionData, p.x + dx, p.y, p.z + dz, 1000.0f, y)) {
                    best = dx * dx + dz * dz;
                    q = {p.x + dx, y, p.z + dz};
                }
        p = q;
    }
    app.walker.pos = p;
    app.walker.heading = w.angleDegrees * 3.14159265f / 180.0f; // FieldPCCtrl::reset: DegToRad
    app.walker.gait = cr::Gait::Stand;
    app.lastKick = triggerAt(app, p); // don't fire a box we arrive in
    app.lastWarp = "warp: event " + std::to_string(fromEvent) + " map " + std::to_string(fromMap) + " -> " +
                   std::to_string(w.map);
    SDL_Log("%s (%s) at (%.1f %.1f %.1f) facing %g", app.lastWarp.c_str(), cr::kMapCollisionFile[w.map], p.x, p.y, p.z,
            w.angleDegrees);
    updateTitle(app);
    return true;
}

// The NPC in reach to examine or talk to: kind bit 3, displayed, and the player inside its box grown
// by the walker radius + 1.5. RectWrapper::checkFollowWindow gives objects (bit 4 clear) the "!"
// badge; checkFlagWindow gives people (bit 4 set) the talk badge. Both start the NPC's script.
const cr::FieldNpc* interactTarget(const App& app) {
    if (!app.havePlayer) return nullptr;
    const cr::FieldNpc* best = nullptr;
    float bestD = 1e30f;
    for (const cr::FieldNpc& n : app.npcs) {
        if (!(n.kind & 8) || !npcActive(app, n)) continue;
        if (n.pos[1] >= 900.0f || (n.pos[0] == 0 && n.pos[1] == 0 && n.pos[2] == 0)) continue;
        const float k = n.model <= 1 ? 1.0f : 0.0f;
        const float cx = n.pos[0] + k * n.offset[0] + n.offset[0], cz = n.pos[2] + k * n.offset[2] + n.offset[2];
        const float hx = n.box[0] / 2 + app.walker.radius + 1.5f, hz = n.box[2] / 2 + app.walker.radius + 1.5f;
        const float dx = app.walker.pos.x - cx, dz = app.walker.pos.z - cz;
        if (std::fabs(dx) > hx || std::fabs(dz) > hz) continue;
        if (dx * dx + dz * dz < bestD) { bestD = dx * dx + dz * dz; best = &n; }
    }
    return best;
}

// Examines / talks to the NPC in reach, as FollowWindow::TouchFollowWindowUtil does: a treasure
// box sets var 11 = box id, var 10 = its item, and runs the script of its lid (id + 1);
// anything else runs its own script.
bool interact(App& app, bool instant) {
    if (app.runner.running()) return false;
    const cr::FieldNpc* n = interactTarget(app);
    if (!n) return false;
    int script = n->id;
    const std::string what = n->model >= 0 && n->model < cr::kNumNpcModels ? cr::kNpcModelFile[n->model] : "trigger";
    if (n->treasure & 3) {
        cr::NpcRow row;
        app.game.vars[11] = n->id;
        app.game.vars[10] = app.events.npcRow(n->id, row) && row.item >= 0
                                ? row.item
                                : cr::treasureItem(app.treasureBoxes[app.area], n->id);
        script = n->id + 1;
    }
    const int id = n->id;
    if (!instant) { // played over the next frames (animations, item window, fades)
        app.runnerWhat = "examined " + what.substr(0, what.size() - 4) + " (" + std::to_string(id) + ")";
        app.lastEvent = app.runnerWhat;
        app.runner.start(app.events, script, app.game, app.area, app.mapNumber);
        updateTitle(app);
        return true;
    }
    const cr::EventResult r = cr::runEvent(app.events, script, app.game, app.area, app.mapNumber);
    if (!r.error.empty()) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "event %d: %s", script, r.error.c_str());
    app.lastEvent = "examined " + what.substr(0, what.size() - 4) + " (" + std::to_string(id) + ")";
    for (int item : r.itemsGained) { // item names: sys.evt block 0 (descriptions are block 1)
        std::vector<std::vector<std::string>> name;
        app.lastEvent += ", got " + itemName(app, item);
    }
    SDL_Log("%s, script %d, %d instructions, %zu messages", app.lastEvent.c_str(), script, r.instructions,
            r.messages.size());
    showMessages(app, r.messages);
    if (r.warp.valid && performWarp(app, r.warp, script)) return true;
    // Scripts change flags (opened boxes hide their lid): refresh what is shown and solid
    buildNpcDraws(app);
    buildWalkCollision(app);
    updateTitle(app);
    return true;
}

// An item's name (sys.evt block 0; descriptions are block 1)
// Whether `language` can be shown: the data has it, and Japanese / Chinese / Korean need the font.
bool languageUsable(const App& app, int language) {
    if (language < 0 || language >= cr::kLanguages || !app.systemText.hasLanguage(language)) return false;
    return (language >= 1 && language <= 4) || app.font.loaded();
}

// The font face for the language: the collection's regional Han forms (Noto Sans CJK JP / SC /
// TC / KR); Latin text uses the Japanese face like the Japanese release's system font.
void applyLanguage(App& app) {
    if (!languageUsable(app, app.language)) app.language = 1;
    static const char* const kRegion[cr::kLanguages] = {"JP", "JP", "JP", "JP", "JP", "SC", "TC", "KR"};
    app.font.selectRegion(kRegion[app.language]);
}

std::string itemName(const App& app, int item) {
    std::vector<std::vector<std::string>> name;
    if (app.systemText.message(cr::itemNameId(item), app.language, nullptr, name) && !name.empty() && !name[0].empty())
        return name[0][0];
    return "item " + std::to_string(item);
}

// Turns the dialogue page (E / Enter); the last page read stays available for a following choice.
void resetTyping(App& app) {
    app.dialogueTyped = app.dialogueHold = app.dialoguePause = 0;
    app.dialogueClosing = false;
}

void advanceDialogue(App& app) {
    if (app.dialogue.empty()) return;
    app.game.setFlag(4075, true); // PrcMsg: a page turned (or the last one closed) - MsgUty_TexDispProc's cue
    resetTyping(app);
    if (++app.dialoguePage < app.dialogue.size()) return;
    app.lastPage = app.dialogue.back();
    app.lastPageInfo = app.dialogueInfo.size() == app.dialogue.size() ? app.dialogueInfo.back() : cr::MessageInfo();
    app.dialogue.clear();
    app.dialoguePage = 0;
}

// A page's characters, as PrcMsg types them (UTF-8 lead bytes; not the lines' \x01 marks)
int pageChars(const std::vector<std::string>& page) {
    int n = 0;
    for (const std::string& l : page)
        for (unsigned char ch : l) n += (ch & 0xC0) != 0x80 && ch != 0x01;
    return n;
}

// The page cut to its first `limit` characters (the \x01 marks kept)
std::vector<std::string> typedPage(const std::vector<std::string>& page, int limit) {
    std::vector<std::string> out;
    for (const std::string& l : page) {
        std::string t;
        for (char c : l) {
            const unsigned char ch = static_cast<unsigned char>(c);
            if (ch != 0x01 && (ch & 0xC0) != 0x80 && limit-- <= 0) break;
            t += c;
        }
        out.push_back(t);
    }
    return out;
}

const cr::MessageInfo* pageInfo(const App& app) {
    const size_t pi = std::min(app.dialoguePage, app.dialogue.size() - 1);
    return pi < app.dialogueInfo.size() ? &app.dialogueInfo[pi] : nullptr;
}

bool pageTyped(const App& app) {
    return app.dialogue.empty() || !app.typeOn ||
           app.dialogueTyped >= pageChars(app.dialogue[std::min(app.dialoguePage, app.dialogue.size() - 1)]);
}

// PrcMsg a frame: the next character (0x1a pauses it); a page ending the message with 0x14
// closes by itself once out
void stepTyping(App& app) {
    if (app.dialogue.empty()) return;
    const cr::MessageInfo* info = pageInfo(app);
    const int total = pageChars(app.dialogue[std::min(app.dialoguePage, app.dialogue.size() - 1)]);
    if (!app.typeOn) app.dialogueTyped = total;
    if (app.dialogueHold > 0 && --app.dialogueHold > 0) return;
    if (app.dialogueTyped >= total) {
        if (!info || info->closeAfter < 0 || app.choiceFlag >= 0) return;
        if (!app.dialogueClosing) {
            app.dialogueClosing = true;
            app.dialogueHold = info->closeAfter;
            if (app.dialogueHold > 0) return;
        }
        advanceDialogue(app);
        return;
    }
    if (info && app.dialoguePause < int(info->pauses.size()) && info->pauses[size_t(app.dialoguePause)].at <= app.dialogueTyped) {
        app.dialogueHold = info->pauses[size_t(app.dialoguePause++)].frames;
        if (app.dialogueHold > 0) return;
    }
    ++app.dialogueTyped;
}

// The player's tap on a page: one still typing comes out whole (PrcMsg +0x33433), one out turns;
// a page closing by itself takes no tap
void tapDialogue(App& app) {
    if (app.dialogue.empty()) return;
    const cr::MessageInfo* info = pageInfo(app);
    if (!pageTyped(app)) {
        app.dialogueTyped = pageChars(app.dialogue[std::min(app.dialoguePage, app.dialogue.size() - 1)]);
        app.dialogueHold = 0;
        app.dialoguePause = info ? int(info->pauses.size()) : 0;
        return;
    }
    if (info && info->closeAfter >= 0) return;
    advanceDialogue(app);
}

// Answers the open yes/no choice (Util 3): yes sets its flag, no clears it (_menuu_EventClr).
void answerChoice(App& app, bool yes) {
    if (app.choiceFlag < 0) return;
    app.game.setFlag(app.choiceFlag, yes);
    app.choiceFlag = -1;
    menuSound(app, se::kChoose); // Yes and No alike (EventPrc's ComUty_YesNoChoiceProc)
}

// Closes the world choice (_menuu_WldChoiceClr): cancelling sets flag 0 (the script returns);
// a gate leaves it clear with var 0 = gate, var 1 = level, var 2 = the party (route), flag 1 clear.
void closeWorldSelect(App& app);
// The World Select answered at once (tests, the scripted walks): the gate at list position `pos`
// (-1 Back), its first level band, no skipping
void answerWorldChoice(App& app, int pos) {
    if (!app.worldChoiceOpen) return;
    App::WorldSelect& ws = app.worldSelect;
    ws.cancelled = pos < 0 || pos >= int(ws.worlds.size());
    if (!ws.cancelled) ws.world = ws.worlds[size_t(pos)], ws.level = 0, ws.skip = false;
    menuSound(app, ws.cancelled ? se::kBack : se::kChoose); // MuScrList: an item, or Back
    closeWorldSelect(app);
}

// Queues the messages an event showed (English text) for the dialogue box.
void showMessages(App& app, const std::vector<int>& ids) {
    if (app.dialogue.empty()) { app.dialogueInfo.clear(); app.dialogueVoices.clear(); ++app.dialogueSerial; resetTyping(app); }
    for (int id : ids) {
        std::vector<std::vector<std::string>> pages;
        cr::MessageInfo info;
        auto var = [&](int v) { auto it = app.game.vars.find(v); return it == app.game.vars.end() ? 0 : it->second; };
        if (app.events.message(id, app.language, var, pages, &info) ||
            app.systemText.message(id, app.language, var, pages, &info))
            app.dialogue.insert(app.dialogue.end(), pages.begin(), pages.end());
        else
            pages = {{"[message " + std::to_string(id) + "]"}}, app.dialogue.push_back(pages[0]);
        app.dialogueInfo.resize(app.dialogue.size() - pages.size());
        for (size_t k = 0; k < pages.size(); ++k) { // each page its own pauses; the last the closing
            cr::MessageInfo pi = info;
            pi.pauses.clear();
            for (const auto& pz : info.pauses)
                if (pz.page == int(k)) pi.pauses.push_back(pz);
            if (k + 1 < pages.size()) pi.closeAfter = -1;
            app.dialogueInfo.push_back(pi);
        }
        app.dialogueVoices.resize(app.dialogue.size() - pages.size());
        for (size_t k = 0; k < pages.size(); ++k) {
            std::vector<int> clips;
            for (const auto& v : info.voices)
                if (v.page == int(k) || (k + 1 == pages.size() && v.page >= int(pages.size()))) clips.push_back(v.clip);
            app.dialogueVoices.push_back(clips);
        }
    }
}

// Fires the touch trigger the player just stepped into. Returns true if it warped.
bool checkTriggers(App& app, bool instant) {
    if (app.runner.running()) return false;
    const int id = triggerAt(app, app.walker.pos);
    if (id == 0) { app.lastKick = 0; return false; }
    if (id == app.lastKick) return false;
    app.lastKick = id;
    if (!instant) {
        app.runnerWhat.clear();
        app.runner.start(app.events, id, app.game, app.area, app.mapNumber);
        return false;
    }
    const cr::EventResult r = cr::runEvent(app.events, id, app.game, app.area, app.mapNumber);
    if (!r.error.empty()) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "event %d: %s", id, r.error.c_str());
    showMessages(app, r.messages);
    return r.warp.valid && performWarp(app, r.warp, id);
}

// Advances a model's current animation and re-skins every mesh on the CPU.
void animate(App& app, LoadedModel& lm, float dt) {
    const cr::Model& m = lm.cpu;
    if (m.skeleton.nodes.empty() || lm.meshes.size() != m.meshes.size()) return;
    const cr::Action* act = lm.action >= 0 && lm.action < int(lm.actions.size()) ? &lm.actions[lm.action] : nullptr;
    if (act && app.playing) {
        lm.time += dt * kFramesPerSecond * app.speed;
        if (lm.loopFrom > 0 && lm.loopFrom < act->duration && lm.time >= act->duration)
            lm.time = float(lm.loopFrom) + std::fmod(lm.time - float(lm.loopFrom), act->duration - float(lm.loopFrom));
        else if (act->duration > 0) lm.time = std::fmod(lm.time, act->duration);
    }
    cr::computeWorld(m.skeleton, act, lm.time, lm.world);
    for (size_t i = 0; i < m.meshes.size(); ++i) {
        cr::skinMesh(m, m.meshes[i], lm.world, lm.skinned);
        app.renderer.updatePositions(lm.meshes[i], lm.skinned, m.meshes[i].uvs, m.meshes[i].colors);
    }
}

void animateModel(App& app, float dt) { animate(app, app.model, dt); }

// Field models of the party (the binary names f001/f003/f005/f007, one per route).
const char* const kPartyModels[] = {"f001.bin", "f002.bin", "f003.bin", "f004.bin",
                                    "f005.bin", "f006.bin", "f007.bin", "f008.bin"};

// Puts the party leader on the current field, where the stage camera's line of sight meets
// the floor (the real start point comes from the event script, which isn't interpreted yet).
void placePlayer(App& app) {
    app.havePlayer = false;
    if (!app.showPlayer || app.collisionData.polygons.empty()) return;
    const std::string name = kPartyModels[app.playerChar];
    if (app.playerName != name) {
        unloadLoaded(app, app.player);
        loadModelFile(app, name, app.player, true);
        app.playerName = name;
    }
    if (app.player.meshes.empty()) return;
    const cr::Collision& col = app.collisionData;
    cr::Vec3 eye, tgt;
    if (app.haveStageCam) {
        eye = {app.stageCam.eye[0], app.stageCam.eye[1], app.stageCam.eye[2]};
        tgt = {app.stageCam.target[0], app.stageCam.target[1], app.stageCam.target[2]};
    } else {
        const float cx = (col.boundsMin[0] + col.boundsMax[0]) / 2, cz = (col.boundsMin[2] + col.boundsMax[2]) / 2;
        eye = {cx, col.boundsMax[1] + 100, cz};
        tgt = {cx, col.boundsMin[1] - 100, cz};
    }
    const float aspect = app.haveStageCam && app.stageCam.viewHeight > 0
                             ? float(app.stageCam.viewWidth) / float(app.stageCam.viewHeight) : 1.0f;
    const float fov = app.haveStageCam ? app.stageCam.fovDegrees : 60.0f;
    const cr::Mat4 vp = cr::Mat4::perspective(fov * 3.14159265f / 180.0f, aspect, 10.0f, 4000.0f) *
                        cr::Mat4::lookAt(eye, tgt, app.haveStageCam ? cr::Vec3{0, 1, 0} : cr::Vec3{0, 0, -1}); // top-down otherwise
    // Start at the map's first entrance in the area's warp table (where the game puts you when
    // you arrive); without one, where the camera looks at the floor
    bool entrance = false;
    for (const cr::WarpEntry& w : app.events.warps()) {
        if (w.map != app.mapNumber || (w.area >= 0 && w.area != app.area)) continue;
        float y;
        if (!cr::floorHeight(col, w.pos[0], w.pos[1], w.pos[2], 5.0f, y) &&
            !cr::floorHeight(col, w.pos[0], w.pos[1], w.pos[2], 1000.0f, y))
            continue;
        app.walker.pos = {w.pos[0], y, w.pos[2]};
        app.walker.heading = float(w.angleDegrees) * 3.14159265f / 180.0f;
        entrance = true;
        break;
    }
    if (!entrance) {
        if (!cr::spawnPoint(col, eye, tgt, vp, app.walker.pos)) return;
        app.walker.heading = std::atan2(eye.x - tgt.x, eye.z - tgt.z); // face the camera
    }
    app.walker.radius = app.haveStageCam && app.stageCam.hitRadius > 0 ? float(app.stageCam.hitRadius) : 5.0f;
    app.walker.gait = cr::Gait::Stand;
    app.player.action = app.player.actions.empty() ? -1 : 0;
    app.player.time = 0;
    app.walkClock = 0;
    app.havePlayer = true;
    app.lastKick = triggerAt(app, app.walker.pos); // starting inside a box doesn't fire it
}

// The viewer's side of a running script (see cr::EventHost).
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

// One 30 Hz frame of every effect on: the channels, then the counters (as ScnEffectProc: drawn first)
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

void drawSceneLayer(App& app, int slot, float x, float y, float w, float h, float u, float v, int alpha);

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
void drawSceneEffects(App& app, bool battle = false) {
    const App::SceneFx& fx = app.fx;
    if (fx.set < 0) return;
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
        case 11: drawSceneLayer(app, 70, -16, 128, 512, 256, 2.1333333f * a - sx, 0, 255); break; // p0001_fog
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
bool scrollingField(const App& app) { return paintedField(app) && app.video.paintings == 0; }
float fieldViewW(const App& app) { return 2.0f * float(app.stageCam.viewWidth > 0 ? app.stageCam.viewWidth : 1024); }
float fieldViewH(const App& app) { return 2.0f * float(app.stageCam.viewHeight > 0 ? app.stageCam.viewHeight : 1024); }

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
    const float SW = app.renderer.screenW(), H = cr::Renderer::kLogicalH;
    const int* rc = app.stageCam.rect;
    const bool haveRect = rc[2] > 0 && rc[3] > 0;
    const float rx = haveRect ? 2.0f * float(rc[0]) : 0, ry = haveRect ? 2.0f * float(rc[1]) : 0;
    const float rw = haveRect ? 2.0f * float(rc[2]) : V, rh = haveRect ? 2.0f * float(rc[3]) : Vh;
    auto axis = [](float v, float lo, float len, float screen) {
        return len <= screen ? lo + len / 2 : std::clamp(v, lo + screen / 2, lo + len - screen / 2);
    };
    const float tx = V / 2 - axis(vx, rx, rw, SW), ty = axis(vy, ry, rh, H) - Vh / 2;
    app.scroll.offX += (tx - app.scroll.offX) * rate;
    app.scroll.offY += (ty - app.scroll.offY) * rate;
}

// FieldCamera::setScrollPlayer, every frame while the camera follows the player
void followPlayerScroll(App& app) {
    if (!scrollingField(app) || app.cam.free || !app.havePlayer) return;
    const cr::Vec3 head = app.walker.pos + cr::Vec3{0, 7.0f + float(app.stageCam.lookHeight), 0};
    float vx, vy;
    if (fieldViewPoint(app, head, vx, vy)) scrollFieldTo(app, vx, vy, 1.0f);
    if (SDL_getenv("CR_SCROLL_DEBUG"))
        std::printf("scroll: head view (%.0f, %.0f) of %.0f x %.0f, rect %d %d %d %d -> offset (%.0f, %.0f)\n", vx, vy,
                    fieldViewW(app), fieldViewH(app), app.stageCam.rect[0], app.stageCam.rect[1], app.stageCam.rect[2],
                    app.stageCam.rect[3], app.scroll.offX, app.scroll.offY);
}

// ScrollAutoCtl, once a 30 Hz step with the camera free: the slide
void stepFieldScroll(App& app) {
    App::FieldScroll& fs = app.scroll;
    if (!fs.sliding) return;
    if (!scrollingField(app) || !app.cam.free) { fs.sliding = false; return; }
    ++fs.t;
    const float k = fs.frames > 0 ? std::min(1.0f, float(fs.t) / float(fs.frames)) : 1.0f;
    const float x = fs.fromX + (fs.toX - fs.fromX) * k, y = fs.fromY + (fs.toY - fs.fromY) * k;
    fs.offX = fs.offY = 0;
    scrollFieldTo(app, fieldViewW(app) / 2 - x, fieldViewH(app) / 2 + y, 1.0f);
    if (fs.t >= fs.frames) fs.sliding = false;
}

// The scripts' shots on a painted field: jump (frames 0) or slide to offset (x, y)
void scriptFieldScroll(App& app, float x, float y, int frames) {
    App::FieldScroll& fs = app.scroll;
    if (frames <= 0) {
        fs.sliding = false;
        fs.offX = fs.offY = 0;
        scrollFieldTo(app, fieldViewW(app) / 2 - x, fieldViewH(app) / 2 + y, 1.0f);
        return;
    }
    fs.fromX = fs.offX;
    fs.fromY = fs.offY;
    fs.toX = x;
    fs.toY = y;
    fs.t = 0;
    fs.frames = frames;
    fs.sliding = true;
}

void openStaffRoll(App& app, int mode);
int currentPair(const App& app);
void closeShop(App& app);

void openLastBattle(App& app);
void openWorldSelect(App& app);
// eff3d (the battle effects section)
void startQuake(App::Battle::Quake& q, float amp, float from, float to, int frames, bool hold);
void endQuake(App::Battle::Quake& q, float amp, float from, float to, int frames);
void stepQuake(App::Battle::Quake& q);
int startEffect(App& app, std::vector<Effect>& list, int id, cr::Vec3 at, bool loop, float scale, float yaw);
void stopEffect(std::vector<Effect>& list, int handle);
void stepEffects(App& app, std::vector<Effect>& list);

struct ViewerHost : cr::EventHost {
    App& app;
    explicit ViewerHost(App& a) : app(a) {}
    std::vector<App::NpcDraw*> npc(int id) {
        std::vector<App::NpcDraw*> out;
        for (auto& d : app.npcDraws)
            if (d.id == id) out.push_back(&d);
        return out;
    }
    void showMessage(int id) override { showMessages(app, {id}); }
    bool messageOpen() override { return !app.dialogue.empty(); }
    bool tapped() override { const bool t = app.tapPending; app.tapPending = false; return t; }
    void showItemWindow(int item) override { app.itemWindowOpen = true; app.itemWindowItem = item; }
    void playAnim(int id, int slot, int loopFrom, int loopTo) override {
        int action = slot; // a slot LoadAct filled, else the model's own action of that number
        if (auto a = app.actorSlots.find(id); a != app.actorSlots.end())
            if (auto it = a->second.find(slot); it != a->second.end()) action = it->second;
        for (auto* d : npc(id)) {
            if (d->action != action) { d->action = action; d->time = 0; } // the same action keeps running
            d->once = loopFrom < 0;
            d->loopFrom = loopFrom;
            d->loopTo = loopTo;
        }
    }
    void loadAction(int id, int action, int slot) override {
        const cr::FieldNpc* n = findNpc(app, id);
        if (!n || n->model < 0 || n->model >= cr::kNumNpcModels || action < 0 || action >= cr::kNumActionFiles) return;
        const std::string file = cr::kNpcModelFile[n->model];
        auto it = app.npcModels.find(file);
        if (it == app.npcModels.end()) {
            LoadedModel lm;
            loadModelFile(app, file, lm, true);
            it = app.npcModels.emplace(file, std::move(lm)).first;
        }
        LoadedModel& lm = it->second;
        const std::string mca = cr::kActionFile[action];
        auto known = lm.loaded.find(mca);
        if (known == lm.loaded.end()) {
            std::vector<uint8_t> ad;
            std::vector<cr::Action> acts;
            int index = -1;
            if (cr::readFile(app.dataDir + mca, ad) && cr::parseMca(ad.data(), ad.size(), 0, acts))
                for (auto& a : acts)
                    if (cr::actionFits(lm.cpu.skeleton, a)) {
                        a.name = mca.substr(0, mca.size() - 4);
                        index = int(lm.actions.size());
                        lm.actions.push_back(std::move(a));
                        break;
                    }
            known = lm.loaded.emplace(mca, index).first;
        }
        if (known->second >= 0) app.actorSlots[id][slot] = known->second;
    }
    void unloadAction(int id, int slot) override {
        if (auto a = app.actorSlots.find(id); a != app.actorSlots.end()) a->second.erase(slot);
    }
    // Wait 0xFFFD (npc_autoctl_chk): only a scripted move keeps it waiting, not an animation
    bool animBusy(int id) override {
        for (const auto& m : app.moves)
            if (m.npc == id) return true;
        return false;
    }
    void fadeNpc(int id, float from, float to, int frames) override {
        for (auto* d : npc(id)) {
            d->alphaFrom = from; d->alphaTo = to; d->alpha = from;
            d->fadeFrames = d->fadeLeft = std::max(frames, 1);
        }
    }
    void showNpc(int id, bool shown) override {
        app.npcShown[id] = shown;
        if (id == 0) return; // the player: drawn while showPlayer and not hidden here
        buildNpcDraws(app);
        buildWalkCollision(app);
    }
    void screenFade(float grey, float from, float to, int frames) override {
        app.fadeGrey = grey; app.fadeFrom = from; app.fadeTo = to; app.fadeFrames = std::max(frames, 1); app.fadeT = 0;
    }
    bool screenFading() override { return app.fadeT < app.fadeFrames; }
    void warp(const cr::WarpRequest& w) override { performWarp(app, w, app.runner.eventNo()); }
    void playSound(uint32_t id) override { app.sound.play(id); app.soundLog.push_back(id); }
    void playMusic(uint32_t v) override { playBgm(app, v); app.soundLog.push_back(0x40000000u | v); }
    void recoverPair() override { // pcparam_hpmp_recovery: the current pair's two
        const int p = std::clamp(currentPair(app) - 1, 0, 3);
        for (int c : {2 * p, 2 * p + 1}) app.game.party[size_t(c)].hp = app.game.party[size_t(c)].mp = -1;
    }
    int bgmNo() override { return app.bgmNo; }
    void endMenu(int kind) override { // 2: the shop - SetShopBgmNoRecovery, __mc_SetMenuStat(7, 4)
        if (kind != 2 || !app.shop.open) return;
        app.shop.noBgmRecovery = true;
        closeShop(app);
    }
    int pairNo() override { return std::clamp(currentPair(app) - 1, 0, 3); }
    void nowLoading(bool on) override { // NowLoadStart / NowLoadEnd
        if (on && !app.nowLoading.on) app.nowLoading = App::NowLoading{true, 0};
        else if (!on) app.nowLoading.on = false;
    }
    static constexpr float kDeg = 3.14159265f / 180.0f;
    void placeNpc(int id, const float* v, float yawDeg, Facing facing) override {
        cr::Vec3 old;
        if (!actorPos(app, id, old)) return;
        const cr::Vec3 p{v[0], v[1], v[2]};
        float yaw = yawDeg * kDeg;
        if (facing == Facing::Travel) {
            if (std::fabs(p.x - old.x) + std::fabs(p.z - old.z) < 1e-4f) facing = Facing::Keep;
            else yaw = std::atan2(p.x - old.x, p.z - old.z);
        }
        setActor(app, id, p, facing == Facing::Keep ? nullptr : &yaw, false);
    }
    void moveNpc(int id, const float* v, float yawDeg, Facing facing, int frames, int mode) override {
        App::Move m;
        if (!actorPos(app, id, m.from)) return;
        m.npc = id;
        m.to = {v[0], v[1], v[2]};
        m.mode = mode;
        m.frames = std::max(frames, 1);
        float yawNow = id == 0 ? app.walker.heading : findNpc(app, id)->angle;
        if (facing == Facing::Travel && std::fabs(m.to.x - m.from.x) + std::fabs(m.to.z - m.from.z) > 1e-4f) {
            const float yaw = std::atan2(m.to.x - m.from.x, m.to.z - m.from.z);
            setActor(app, id, m.from, &yaw, true);
        } else if (facing == Facing::Yaw) {
            m.turn = true;
            m.yawFrom = yawNow;
            m.yawTo = yawDeg * kDeg;
        }
        app.moves.erase(std::remove_if(app.moves.begin(), app.moves.end(), [&](const App::Move& o) { return o.npc == id; }),
                        app.moves.end());
        app.moves.push_back(m);
    }
    void tex2d(int slot, uint16_t f, int image, const int16_t (*extra)[3]) override {
        if (SDL_getenv("CR_TEXDISP_DEBUG")) std::printf("tex2d: slot %d flags %04x image %d\n", slot, f, image);
        if (slot < 0 || slot >= 16) return;
        App::Portrait& p = app.portraits[slot];
        if ((f & 1) && image >= 0 && image < cr::kNumTex2dImages) { // read_img_set
            app.renderer.destroy(p.tex);
            p = App::Portrait();
            cr::Image img;
            if (loadImage(app, cr::kTex2dImage[image], img)) p.tex = app.renderer.upload(img);
            p.image = image;
            // (0x8000 only sizes it - a full-screen card; a load alone shows nothing: tex2d_img_prc
            // draws a slot once 0x200 / 0x400 has turned it on - Uty_TexDisp draws loaded ones itself)
        }
        // __EvtTex2d: the flags stay in the slot (placement bits replaced); every animation is a
        // straight blend over a few 30 Hz frames (tex2d_img_prc)
        p.flags = uint16_t((p.flags & ~0x3000) | f);
        if (f & 0x1000) p.home = -32, p.y = 0, p.side = -1;
        if (f & 0x2000) p.home = 512, p.y = 0, p.side = 1; // drawn mirrored (+0xbd0e)
        if (f & 0x4000) {
            p.home = 176, p.y = 96, p.side = 0;
            if (f & 0x40) { p.scale = 2; p.zoomT = 0; p.zoomFrames = 4; } // 200% -> 100%
        }
        if (f & 0x7000) { p.x = p.xFrom = p.xTo = p.home; p.yFrom = p.yTo = p.y; p.frames = p.t = 0; }
        if (f & 0xC00) { // show / hide: 3 frames of fade and a 32 px slide from (show) / to (hide) its side
            const bool show = f & 0x400;
            const float out = 32.0f * float(p.side);
            p.aFrom = show ? 0 : p.alpha;
            p.aTo = show ? 1 : 0;
            p.xFrom = show ? p.home + out : p.x;
            p.xTo = show ? p.home : p.home + out;
            p.yFrom = p.yTo = p.y;
            p.x = p.xFrom;
            p.alpha = p.aFrom;
            p.frames = p.aFrames = 3;
            p.t = p.aT = 0;
        }
        if (f & 0x200) { // swap in (a new expression): shown at once in place; whatever else
                         // stands on that side goes
            if (p.side)
                for (int i = 0; i < 16; ++i) {
                    App::Portrait& o = app.portraits[i];
                    if (i != slot && o.tex.id && (o.flags & (p.side < 0 ? 0x1000 : 0x2000))) {
                        o.alpha = o.aFrom = o.aTo = 0;
                        o.frames = o.t = o.aFrames = o.aT = 0;
                    }
                }
            p.x = p.xFrom = p.xTo = p.home;
            p.yFrom = p.yTo = p.y;
            p.alpha = p.aFrom = p.aTo = 1;
            p.frames = p.t = p.aFrames = p.aT = 0;
        }
        // The low byte's extra operands, 3 shorts each: 4 a fade (from, to /255, frames), 0x20 the
        // grey level the same way, 8 a slide (dx, dy, frames) - from the offset back to where it
        // stands; fewer than 0 frames: there at once, and staying
        if (f & 2) p.dying = true;
        if (f & 4) {
            p.aFrom = float(extra[0][0]) / 255.0f, p.aTo = float(extra[0][1]) / 255.0f;
            p.aFrames = std::max<int>(1, extra[0][2]), p.aT = 0;
            p.alpha = p.aFrom;
        }
        if (f & 0x20) {
            p.gFrom = float(extra[1][0]) / 255.0f, p.gTo = float(extra[1][1]) / 255.0f;
            p.gFrames = std::max<int>(1, extra[1][2]), p.gT = 0;
            p.shade = p.gFrom;
        }
        if (f & 8) {
            const float dx = extra[2][0], dy = extra[2][1];
            const int n = extra[2][2];
            if (n < 0) { p.xTo += dx, p.yTo += dy; p.home += dx; }
            p.xFrom = p.xTo + (n < 0 ? 0 : dx), p.yFrom = p.yTo + (n < 0 ? 0 : dy);
            p.x = p.xFrom, p.y = p.yFrom;
            p.frames = std::max(1, std::abs(n)), p.t = 0;
        }
        if (p.dying && p.t >= p.frames && p.aT >= p.aFrames) { app.renderer.destroy(p.tex); p = App::Portrait(); } // nothing to wait for
    }
    void loadParty(int pair, bool clearData) override { // Uty_LoadPrty: prty_init_new(pair - 1, mode)
        if (pair >= 1 && pair <= 4) newPairParty(app, pair, clearData);
    }
    void loadNpc(int id) override {
        if (findNpc(app, id)) return;
        cr::NpcRow row;
        if (!app.events.npcRow(id, row)) return;
        cr::FieldNpc n; // __EvtNpcMdlRead: at the origin, facing 0, and hidden (+0x8c bit 0) when its
        n.id = id;      // create flags fail or it's a story actor (id 400 on) - until a Born
        n.flagShow = row.flagShow;
        n.flagHide = row.flagHide;
        n.model = row.model;
        n.kind = row.kind;
        n.treasure = row.box;
        n.puzzle = row.puzzle;
        n.scale = row.scale;
        if (id >= 400 || !npcActive(app, n)) app.npcShown[id] = false;
        app.npcs.push_back(n);
        app.npcWarps.push_back(false);
    }
    void freeCamera(bool on) override { app.cam.free = on; if (!on) app.cam.set = false; }
    void setCamera(const float* eye, const float* target, int frames) override {
        if (scrollingField(app)) { // a painted field: FrmAdjCam slides the painting to the shot's (x, y);
            // FrmSetCam on the field (flag word bit 64) takes the 3D path, which starts with SetScroll(0, 0)
            if (!app.cam.free) return;
            if (frames > 0) scriptFieldScroll(app, eye[0], eye[1], frames);
            else {
                app.scroll.offX = app.scroll.offY = 0;
                app.scroll.sliding = false;
            }
            return;
        }
        if (paintedField(app)) return; // shown whole: nothing moves
        const cr::Vec3 e{eye[0], eye[1], eye[2]}, t{target[0], target[1], target[2]};
        if (std::fabs(e.x - t.x) + std::fabs(e.y - t.y) + std::fabs(e.z - t.z) < 1e-4f) return; // no target actor
        App::ScriptCam& c = app.cam;
        if (frames <= 0 || !c.set) { c.eye = e; c.target = t; c.frames = c.t = 0; }
        else { c.eyeFrom = c.eye; c.tgtFrom = c.target; c.eyeTo = e; c.tgtTo = t; c.frames = frames; c.t = 0; }
        c.set = true;
    }
    void panCamera(const float* d, int frames) override {
        if (scrollingField(app)) { // FrmPanCam's 2D path: twice the deltas on the iPad
            if (app.cam.free) scriptFieldScroll(app, app.scroll.offX + 2 * d[0], app.scroll.offY + 2 * d[1], std::max(frames, 1));
            return;
        }
        if (paintedField(app)) return;
        App::ScriptCam& c = app.cam;
        if (!c.set) return;
        const cr::Vec3 dv{d[0], d[1], d[2]};
        c.eyeFrom = c.eye; c.tgtFrom = c.target;
        c.eyeTo = c.eye + dv; c.tgtTo = c.target + dv;
        c.frames = std::max(frames, 1); c.t = 0;
    }
    void sceneEffectSet(int set) override { loadSceneEffectSet(app, set); }
    void quake(int amp, int from, int to, int frames) override { // EvtQuake: < 0 frames starts one held, else EndQuake
        ++app.scriptQuakes;
        if (frames < 0) startQuake(app.fieldQuake, float(amp), float(from), float(to), 1, true);
        else endQuake(app.fieldQuake, float(amp), float(from), float(to), frames);
    }
    void vitaLogo(int slot, int picture) override { // read_img_set(the table at 0x811122b0 [picture])
        static const char* const kLogo[8] = {"sqex", "mve", "mclogo", "title1", "title2", "chara3", "chara1", "chara2"};
        const char* name = picture == 254 ? "warning" : picture >= 0 && picture < 8 ? kLogo[picture] : nullptr;
        App::VitaLogo& l = app.vitaLogo;
        const int k = std::clamp(std::abs(slot), 0, 1);
        app.renderer.destroy(l.tex[k]);
        cr::Image img;
        if (name && loadImage(app, std::string(name) + "_hd.mct", img)) l.tex[k] = app.renderer.upload(img);
    }
    void vitaLogoShow(int slot, int in, int hold, int out) override {
        App::VitaLogo& l = app.vitaLogo;
        l.slot = std::clamp(std::abs(slot), 0, 1), l.phase = 1, l.frame = 0, l.in = in, l.hold = hold, l.out = out, l.skip = false;
        if (!l.tex[l.slot].id) l.phase = 0; // (nothing read: nothing to show)
    }
    bool vitaLogoBusy() override { return app.vitaLogo.phase != 0; }
    void texDisp(const int32_t* g) override {
        App::TexDisp& t = app.texDisp;
        t = App::TexDisp();
        t.on = true;
        for (int k = 0; k < 11; ++k) t.g[k] = g[k];
        ++app.texDispCalls;
        if (SDL_getenv("CR_TEXDISP_DEBUG"))
            std::printf("texdisp: slot %d (%s, image %d) x %d u %d v %d %dx%d fade %d/%d/%d\n", g[0],
                        g[0] >= 0 && g[0] < 16 && app.portraits[g[0]].tex.id ? "loaded" : "EMPTY", g[0] >= 0 && g[0] < 16 ? app.portraits[g[0]].image : -1,
                        g[1], g[3], g[4], g[5], g[6], g[7], g[8], g[9]);
    }
    void tutoTexDisp(const int32_t* g) override { // MsgUty_TexDispInit(var 1, ...)
        App::TutoTex& t = app.tutoTex;
        app.renderer.destroy(t.tex);
        t = App::TutoTex();
        static const char* const kTuto[14] = { // 0xc9500: the puzzles' (0..7), the battles' (8..11, 13), the field's
            "telepo_01_hd.mct", "telepo_02_hd.mct", "psyco_01_hd.mct", "psyco_02_hd.mct", "tough_01_hd.mct",
            "tough_02_hd.mct",  "pyro_01_hd.mct",   "pyro_02_hd.mct",  "battle_01_hd.mct", "battle_02_hd.mct",
            "battle_03_hd.mct", "gene_01_hd.mct",   "puzzle_01_hd.mct", "battle_04_hd.mct"};
        const int pic = app.game.vars[1];
        cr::Image img;
        if (pic >= 0 && pic < 14) { // the iPad's _hd sheet, else the iPhone's
            std::string name = kTuto[pic];
            if (loadImage(app, name, img) || loadImage(app, name.replace(name.find("_hd"), 3, ""), img)) t.tex = app.renderer.upload(img);
        }
        t.on = true, t.state = 1;
        t.pages = g[1];
        for (int k = 0; k < 4; ++k) t.row[k] = g[2 + 2 * k], t.frames[k] = g[3 + 2 * k];
        t.x = float(g[10]), t.y = float(g[11]);
        app.game.setFlag(4075, false);
        ++app.tutoCalls;
    }
    void exclamationMark() override { app.exclamation = true; ++app.exclamationCalls; }
    void openWindow(int style, int lines) override { app.msgStyle = style, app.msgLines = lines; }
    bool logoSeen() override { const bool seen = app.logoSeen; app.logoSeen = true; return seen; }
    void padArt() override { app.padArt = true; }
    // Uty_Btl_ParamUp01 / 02: battle fighter 1 / 0's attack, defense, speed and luck up for 6 rounds
    // (+0x8b..+0x8e = 6); outside a battle, for the next one
    void battleParamUp(int fighter) override {
        if (fighter < 0 || fighter > 1) return;
        if (app.battle.active) {
            for (int st = cr::kStatusAttackUp; st <= cr::kStatusLuckUp; ++st) app.battle.rules.f[size_t(fighter)].status[st] = 6;
        } else app.paramUp[fighter] = true;
    }
    void puzzleStart() override { app.puzzleBanner = App::PuzzleBanner(); app.puzzleBanner.on = true; } // Uty_PuzzleStart_Init
    void warpCrystalCamera(int npc) override { // SetFreeCamera, then the camera task
        app.crystalCam = App::CrystalCam{npc, 0};
        app.cam.free = true;
    }
    void sceneEffect(bool on, int id) override { setSceneEffect(app, on, id); }
    void frameEffect(int actor, int effect, int scale, const float* v, bool angled) override {
        ++app.frameEffectCalls;
        cr::Vec3 p;
        float yaw = 0;
        if (!actorPos(app, actor, p)) { // not an NPC: where the scene's record has it
            if (!v) return;
            p = {v[0], v[1], v[2]};
            if (angled) yaw = v[4] * kDeg;
        }
        const int h = startEffect(app, app.fieldEffects, effect, p, true, float(scale) / 100.0f, yaw);
        if (h && app.frameEffects.size() < 16) app.frameEffects.push_back({(effect + 1) * (actor + 1), effect, h});
    }
    void stopFrameEffect(int effect, int actor) override {
        for (auto it = app.frameEffects.begin(); it != app.frameEffects.end();)
            if (actor < 0 ? (*it)[1] == effect : (*it)[0] == (effect + 1) * (actor + 1)) {
                stopEffect(app.fieldEffects, (*it)[2]);
                it = app.frameEffects.erase(it);
            } else ++it;
    }
    void npcEffect(int effect, int npc, int dy) override {
        ++app.scriptEffectCalls;
        cr::Vec3 p;
        if (actorPos(app, npc, p)) startEffect(app, app.fieldEffects, effect, p + cr::Vec3{0, float(dy), 0}, false, 1.0f, 0.0f);
    }
    void mapScroll(int npc, float x, float y, float rate) override {
        ++app.scroll.mapScrolls;
        if (!scrollingField(app) || !app.cam.free) return; // setScroll2D / 3D only act with the camera free
        app.scroll.sliding = false;
        if (npc < 0) { scrollFieldTo(app, x, y, rate); return; }
        cr::Vec3 p;
        float vx, vy;
        if (actorPos(app, npc, p) && fieldViewPoint(app, p, vx, vy)) scrollFieldTo(app, vx, vy, rate);
    }
    void battle(int group, int stage, int flags) override { // starts when this frame's script step ends
        app.battle.requested = true;
        app.battle.request = {group, stage, -1, 0};
        app.battle.requestFlags = flags;
    }
    bool menu(int n) override {
        if (n == 0) { // the title menu, and the Progress Tracker's offer when there's a resume point
            app.titleOpen = true;
            app.titleCursor = 0;
            app.titleAnim = 0;
            offerResume(app);
            return true;
        }
        if (n == 1) { openWorldSelect(app); return true; }
        if (n == 2) { openShop(app); return true; }
        if (n == 4) { openCharSelect(app); return true; } // the boot script: Character Select
        if (n == 7) { openStaffRoll(app, app.game.vars[0]); return true; } // ModeStaffRollMenu(var 0)
        if (n == 8) { // flag 0 clear: StartLastBtMenu (the rounds start over); else the pair choice
            if (!app.game.flag(0)) { app.lastBattle.count = app.lastBattle.mask = 0; return false; }
            openLastBattle(app);
            return true;
        }
        if (n == 5 || n == 6) { // SetSaveLoadMenu: at the title, the chosen pair's Load / New; else "Save data?"
            openSaveScreen(app, app.booting);
            return true;
        }
        static const char* const kMenu[] = {"title", "world choice", "shop", "yes/no", "start", "save/load", "save/load",
                                            "staff roll", "last battle"};
        app.unsupported.push_back(std::string("menu ") + (n >= 0 && n < 9 ? kMenu[n] : "?") + " on map " +
                                  std::to_string(app.mapNumber));
        return false;
    }
    void askYesNo(int flag) override { app.choiceFlag = flag; app.choiceCursor = 0; app.choiceAnim = 0; }
    bool choiceOpen() override {
        return app.choiceFlag >= 0 || app.worldChoiceOpen || app.titleOpen || app.saveScreen.open || app.charSelectOpen || app.staffRoll.open || app.lastBattle.open ||
               (app.shop.open && !app.shop.talkRunning); // (the shopkeeper's talk mustn't wait on the shop)
    }
    void faceNpc(int id, int target) override {
        cr::Vec3 a, b;
        if (!actorPos(app, id, a) || !actorPos(app, target, b)) return;
        if (std::fabs(b.x - a.x) + std::fabs(b.z - a.z) < 1e-4f) return;
        const float yaw = std::atan2(b.x - a.x, b.z - a.z);
        setActor(app, id, a, &yaw, true);
    }
};

extern const float kWinMessagePanel[4];
extern const float kWinSmallPanel[4];
void drawWinPiece(App& app, const float* piece, float x, float y, float w, float h);

// Mixer: master scales the three channels; saved to settings.ini in the user's save folder
void applyMix(App& app) {
    const float m = app.mix.master / 100.0f;
    app.music.setVolume(m * app.mix.music / 100.0f);
    app.sound.setVolumes(m * app.mix.effects / 100.0f, m * app.mix.voice / 100.0f);
}

// The user's folder - or CR_SAVE_DIR's (tests and captures keep everything of theirs there)
std::string userDir() {
    if (const char* sd = SDL_getenv("CR_SAVE_DIR"); sd && *sd) return std::string(sd) + (sd[SDL_strlen(sd) - 1] == '/' ? "" : "/");
    return cr::saveDir();
}
std::string settingsPath() { return userDir() + "settings.ini"; }

void loadVideo(App& app, const char* text);

void loadMix(App& app) {
    size_t size = 0;
    char* text = static_cast<char*>(SDL_LoadFile(settingsPath().c_str(), &size));
    if (text) {
        int* fields[4] = {&app.mix.master, &app.mix.music, &app.mix.effects, &app.mix.voice};
        const char* keys[4] = {"master=", "music=", "effects=", "voice="};
        for (int i = 0; i < 4; ++i)
            if (const char* at = SDL_strstr(text, keys[i])) *fields[i] = std::clamp(SDL_atoi(at + SDL_strlen(keys[i])), 0, 100);
        if (const char* at = SDL_strstr(text, "language=")) app.language = std::clamp(SDL_atoi(at + 9), 0, cr::kLanguages - 1);
        applyLanguage(app);
        loadVideo(app, text);
        app.voiceEntries.clear();
        if (const char* at = SDL_strstr(text, "voice_entries=")) { // comma-separated bank entries
            at += 14;
            while (*at >= '0' && *at <= '9') {
                app.voiceEntries.push_back(SDL_atoi(at));
                while (*at >= '0' && *at <= '9') ++at;
                if (*at == ',') ++at;
            }
        }
        SDL_free(text);
    }
    SDL_Log("mix: master %d, music %d, effects %d, voice %d, %zu voice entries (%s)", app.mix.master, app.mix.music,
            app.mix.effects, app.mix.voice, app.voiceEntries.size(), settingsPath().c_str());
    app.sound.setVoiceEntries(app.voiceEntries);
    applyMix(app);
}

void saveMix(const App& app) {
    if (!app.persist) return;
    const std::string t = "master=" + std::to_string(app.mix.master) + "\nmusic=" + std::to_string(app.mix.music) +
                          "\neffects=" + std::to_string(app.mix.effects) + "\nvoice=" + std::to_string(app.mix.voice) + "\n";
    std::string list;
    for (int e : app.voiceEntries) list += (list.empty() ? "" : ",") + std::to_string(e);
    const App::Video& v = app.video;
    const std::string video = "width=" + std::to_string(v.width) + "\nheight=" + std::to_string(v.height) +
                              "\ndisplay=" + std::to_string(v.display) + "\nwidescreen=" + std::to_string(int(v.widescreen)) +
                              "\npainted_rooms=" + std::to_string(v.paintings) + "\nrender_scale=" +
                              std::to_string(v.renderScale) + "\nmsaa=" + std::to_string(v.msaa) +
                              "\nvsync=" + std::to_string(int(v.vsync)) + "\nupscaler=" + std::to_string(v.upscaler) +
                              "\nupscale_mode=" + std::to_string(v.upscaleMode) + "\nsharpness=" +
                              std::to_string(v.sharpness) + "\nhd_textures=" + std::to_string(int(v.hdTextures)) +
                              "\nsmooth_models=" + std::to_string(v.smoothModels) +
                              "\nlayout=" + std::to_string(v.layout) +
                              "\nenvironment_effects=" +
                              std::to_string(int(app.environmentEffects)) + "\nlock_orientation=" +
                              std::to_string(int(app.lockOrientation)) + "\n" + "vita=" + app.vitaPath + "\n" + "pad_confirm=" +
                              (app.padConfirmEast ? "east" : "south") + "\n";
    const std::string all = t + "language=" + std::to_string(app.language) + "\nvoice_entries=" + list + "\n" + video;
    SDL_SaveFile(settingsPath().c_str(), all.data(), all.size());
}



// ---- Video settings ----

// Resolutions offered (up to 3840x2160); the window list leaves out what doesn't fit the display
constexpr int kResolutions[][2] = {{1024, 768}, {1152, 768}, {1280, 720}, {1280, 960}, {1366, 768}, {1440, 960}, {1600, 900}, {1600, 1200},
                                   {1920, 1080}, {1920, 1440}, {2560, 1440}, {2880, 2160}, {3200, 1800}, {3840, 2160}};
constexpr int kRenderScales[] = {50, 67, 75, 100, 125, 150, 200};
// Temporal upscaling: output / render size per axis (FSR's and DLSS's modes)
struct UpscaleMode {
    const char* name;
    float ratio;
};
constexpr UpscaleMode kUpscaleModes[] = {
    {"Native AA", 1.0f}, {"Quality", 1.5f}, {"Balanced", 1.7f}, {"Performance", 2.0f}, {"Ultra Perf.", 3.0f}};
constexpr const char* kUpscalerName[] = {"Off", "FSR 3.1", "DLSS"};
constexpr cr::Upscaler kUpscaler[] = {cr::Upscaler::Off, cr::Upscaler::Fsr3, cr::Upscaler::Dlss};

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
    v.paintings = std::clamp(keyValue(text, "\npainted_rooms=", keyValue(text, "\nzoom_paintings=", 0) ? 2 : v.paintings), 0, 2);
    v.renderScale = std::clamp(keyValue(text, "\nrender_scale=", v.renderScale), 25, 400);
    v.msaa = std::clamp(keyValue(text, "\nmsaa=", v.msaa), 1, 8);
    v.vsync = keyValue(text, "\nvsync=", v.vsync) != 0;
    v.upscaler = std::clamp(keyValue(text, "\nupscaler=", v.upscaler), 0, 2);
    v.upscaleMode = std::clamp(keyValue(text, "\nupscale_mode=", v.upscaleMode), 0, 4);
    v.sharpness = std::clamp(keyValue(text, "\nsharpness=", v.sharpness), 0, 100);
    v.hdTextures = keyValue(text, "\nhd_textures=", v.hdTextures) != 0;
    v.smoothModels = keyValue(text, "\nsmooth_models=", v.smoothModels);
    if (v.smoothModels != 2 && v.smoothModels != 3) v.smoothModels = 0;
    v.layout = std::clamp(keyValue(text, "\nlayout=", v.layout), 0, 2);
    app.environmentEffects = keyValue(text, "\nenvironment_effects=", app.environmentEffects) != 0;
    app.lockOrientation = keyValue(text, "\nlock_orientation=", app.lockOrientation) != 0;
    if (const char* at = SDL_strstr(text, "\npad_confirm=")) app.padConfirmEast = SDL_strncmp(at + 13, "south", 5) != 0;
}

// Window size / fullscreen mode and the renderer's options from app.video
void applyVideo(App& app) {
    App::Video& v = app.video;
    cr::Renderer& r = app.renderer;
    r.setWidescreen(v.widescreen);
    r.setPhoneLayout(v.layout == 2 || (v.layout == 0 && app.phoneData)); // 2.2.0's screens (3:2)
    r.setRenderScale(v.renderScale / 100.0f);
    r.setSamples(v.msaa);
    r.setVSync(v.vsync);
    cr::UpscaleSettings up;
    up.mode = kUpscaler[std::clamp(v.upscaler, 0, 2)];
    up.ratio = kUpscaleModes[std::clamp(v.upscaleMode, 0, 4)].ratio;
    up.sharpness = v.sharpness / 100.0f;
    r.setUpscaler(up);
    if (!app.window) return;
    if (v.display == 0) {
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
// 9 upscaler, 10 upscale mode, 11 sharpness

// Anti-aliasing choices: Off, MSAA 2 / 4 / 8, FSR (Native AA), DLSS (DLAA)
int aaChoice(const App::Video& v) {
    if (v.upscaler) return 3 + v.upscaler;
    return v.msaa >= 8 ? 3 : v.msaa >= 4 ? 2 : v.msaa >= 2 ? 1 : 0;
}

std::string aaName(const App::Video& v) {
    if (v.upscaler == 1) return v.upscaleMode == 0 ? "FSR Native AA" : "FSR 3.1";
    if (v.upscaler == 2) return v.upscaleMode == 0 ? "DLAA" : "DLSS";
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
    case 3: v.paintings = (v.paintings + dir + 3) % 3; break;
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
    case 9: v.upscaler = (v.upscaler + dir + 3) % 3; break;
    case 10: v.upscaleMode = (v.upscaleMode + dir + 5) % 5; break;
    case 11: v.sharpness = std::clamp(v.sharpness + dir * 5, 0, 100); break;
    case 14: v.layout = (v.layout + dir + 3) % 3; break;
    case 13: v.smoothModels = (v.smoothModels == 0 ? (dir > 0 ? 2 : 3) : v.smoothModels == 2 ? (dir > 0 ? 3 : 0) : (dir > 0 ? 0 : 2)); break;
    case 12: { // HD textures: what loads from now on (the next room); the UI atlas at once
        v.hdTextures = !v.hdTextures;
        cr::Image wi;
        if (app.rendererReady && loadImage(app, "window_hd.mct", wi)) {
            app.renderer.destroy(app.windowTex);
            app.windowTex = app.renderer.upload(wi);
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

enum class OptControl { Check, Gauge, Pair, Value };

struct OptRow {
    std::string label;
    OptControl kind = OptControl::Check;
    int words[2] = {-1, -1};     // Pair: word pictures (Japanese / English, On / Off ...), else texts
    std::string texts[2];
    int selected = -1;           // Pair: the lit button
    int* gauge = nullptr;        // Gauge: the 0..100 setting
    int value = 0;
    std::string valueText;       // Value
    bool enabled = true;
    std::string hint;             // shown under the rows while the cursor is on this one
    std::function<void(int)> act; // Gauge / Value: -1 / +1 (Value: 0 = Enter); Pair: 0 / 1; Check: 0
    int sound = int(se::kChoose); // played when act runs (OptionPrc: 2 for every button; gauges below)
    bool enterSounds = false;     // act(0) plays a sound of its own (Sound Test), so none from the menu
};

constexpr int kOptionPages = 4;
constexpr int kOptionsBackRow = 99;
const char* const kOptionPageName[kOptionPages] = {"Game", "Sound", "Display", "Graphics"};
// Layout, in menu units
constexpr float kOptRowTop = 52, kOptRowPitch = 36, kOptCtrlX = 224, kOptCtrlScale = 0.85f;
constexpr float kOptBackY = 344, kOptPageY = 350;

std::string sysLabel(const App& app, int id, const char* fallback) {
    const std::string t = sysText(app, id);
    return t.empty() ? fallback : t;
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
        onOff(sysLabel(app, 58108, "Environment Effects"), &app.environmentEffects, nullptr);
        onOff(sysLabel(app, 58109, "Lock Screen Orientation"), &app.lockOrientation, nullptr);
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
            rows.back().enabled = !app.vitaOnly;
            rows.back().hint = app.vitaOnly ? "Running from the Vita copy alone: " + app.vitaRoot
                               : !app.vitaNote.empty() ? app.vitaNote
                               : "Enter: your Vita copy's PCSG00497 folder - its textures, voices, music and scripts; Left / Right: none";
        }
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
        manual.act = [&app](int) { openManual(app); };
        rows.push_back(manual);
        OptRow title;
        if (app.optionsFromTitle) { // ModeOptionMenu(1): erasing the save data (there is none yet)
            title.label = sysLabel(app, 58122, "Reset Data");
            title.enabled = false;
        } else {
            title.label = sysLabel(app, 58123, "Return to Title");
            title.enabled = app.mode == Mode::Maps && !app.booting;
            title.act = [&app](int) { bootTitle(app); };
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
        static const char* const kLayout[] = {"Auto", "iPad", "iPhone"};
        value("Layout", kLayout[std::clamp(v.layout, 0, 2)], [&app](int dir) { adjustVideo(app, 14, dir ? dir : 1); });
        rows.back().hint = "The screens' layout: the iPad release's (4:3) or the iPhone 2.2.0's (3:2); Auto follows the data";
        static const char* const kPaintings[] = {"Original", "Whole", "Fill Width"};
        value("Painted Rooms", kPaintings[std::clamp(v.paintings, 0, 2)], [&app](int dir) { adjustVideo(app, 3, dir ? dir : 1); });
        onOff("VSync", &v.vsync, [&app] { applyVideo(app); });
        OptRow restore;
        restore.label = "Original Presentation";
        restore.hint = "Every display and graphics option back to the original's";
        restore.act = [&app](int) { adjustVideo(app, 7, 0); };
        rows.push_back(restore);
        break;
    }
    default: { // Graphics: what the Vulkan renderer adds
        const bool scalable = app.renderer.scalable();
        const std::string why = app.renderer.upscalerProblem(kUpscaler[std::clamp(v.upscaler, 0, 2)]);
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
            if (app.video.upscaler == 1) adjustVideo(app, 11, dir ? dir : 1);
        });
        rows.back().enabled = scalable && v.upscaler == 1;
        rows.back().hint = "FSR's sharpening pass (RCAS); DLSS has none";
        onOff("HD Textures", &v.hdTextures, nullptr);
        rows.back().act = [&app](int i) {
            if (app.video.hdTextures != (i == 0)) adjustVideo(app, 12, 1);
        };
        {
            const bool have = !app.hdDir.empty() && SDL_GetPathInfo(app.hdDir.c_str(), nullptr);
            rows.back().hint = have ? "Real-ESRGAN upscales (tools/hd_textures.py); from the next room on"
                                    : "None made yet: see tools/hd_textures.py";
        }
        static const char* const kSmooth[] = {"Off", "", "Smooth", "Smoother"};
        value("Smooth Models", kSmooth[v.smoothModels], [&app](int dir) { adjustVideo(app, 13, dir ? dir : 1); });
        rows.back().hint = v.smoothModels ? "Rounder characters and monsters (4x / 9x triangles); from the next room on"
                                          : "Rounds characters' and monsters' silhouettes (PN triangles)";
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
    if (!app.optionsOpen) app.optionsFromTitle = app.titleOpen;
    app.optionsOpen = true;
    app.optionsPage = (page + kOptionPages) % kOptionPages;
    app.optionsRow = 0;
    app.optionsAnim = 0;
}

// The iPhone layout (2.2.0): the menus' coordinates are its screen's - (0, 0) the corner, not the
// iPad's 512 x 384 menu space - so a menu drawn there sits (+16, +32) menu units further in
struct PhoneMenu {
    cr::MenuPainter& m;
    PhoneMenu(const App& app, cr::MenuPainter& p) : m(p) { m.setOffset(app.renderer.phoneLayout() ? 16.0f : 0.0f, app.renderer.phoneLayout() ? 32.0f : 0.0f); }
    ~PhoneMenu() { m.setOffset(0, 0); }
};
// A tap's menu units under the same offset
void phoneMenuPoint(const App& app, float& mx, float& my) {
    if (app.renderer.phoneLayout()) mx -= 16, my -= 32;
}
// Widescreen (the iPad layout): the origins of a screen's left- and right-hand groups - kept to
// the screen's edges, as the later widescreen release does (and the shop and the battle here),
// the rest centred (uiX). The iPhone layout's one 480-wide screen doesn't spread
float wideLeft(const App& app) { return app.renderer.phoneLayout() ? app.renderer.uiX() : 0.0f; }
float wideRight(const App& app) { return app.renderer.phoneLayout() ? app.renderer.uiX() : 2 * app.renderer.uiX(); }
// A logical point in menu units of the group drawn from `origin`
void menuPointAt(const App& app, float lx, float ly, float origin, float& mx, float& my) {
    mx = (lx - origin) / 2, my = ly / 2;
    phoneMenuPoint(app, mx, my);
}
// The port's own screens (the denser Options, the manual), made for the iPad's 512 x 384: in the
// iPhone layout the whole of it scaled to the screen's height (x 0.833), centred
constexpr float kPhoneFitK = 320.0f / 384.0f, kPhoneFitX = 16 + (480 - 512 * kPhoneFitK) / 2, kPhoneFitY = 32;
struct PhoneFit {
    cr::MenuPainter& m;
    PhoneFit(const App& app, cr::MenuPainter& p) : m(p) {
        if (app.renderer.phoneLayout()) m.setFit(kPhoneFitX, kPhoneFitY, kPhoneFitK);
    }
    ~PhoneFit() { m.setFit(0, 0, 1); }
};
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
    m.begin(r, app.windowTex, &app.font);
    const PhoneFit phoneFit(app, m); // the iPhone layout: its screen's menu coordinates
    // Behind the menu: the scene, dimmed (the game shows it over the camp menu / title)
    r.setOrigin(0);
    r.fillRect(0, 0, r.screenW(), cr::Renderer::kLogicalH, 0, 0, 0, 0.6f);
    r.setOrigin(r.uiX());
    // The windows slide in: rows from the left, controls from the right (WinDispOn -240 / +240)
    const float t = std::min(1.0f, app.optionsAnim / 8.0f), ease = 1 - (1 - t) * (1 - t);
    const float rowsX = -240 * (1 - ease), ctrlX = 240 * (1 - ease), a = t;
    m.picture(102, 8, 4); // "Options"
    m.text(kOptionPageName[app.optionsPage], 236, 14, false, a, 16);
    const std::vector<OptRow> rows = optionRows(app, app.optionsPage);
    for (size_t i = 0; i < rows.size(); ++i) {
        const OptRow& row = rows[i];
        const float y = optRowY(int(i)), mid = y + 12;
        const bool cursor = app.optionsRow == int(i);
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
            m.button(cx + 60, y - 8, -1, row.valueText, false, dim, cursor && row.enabled);
            m.piece(cx + 36, y + 4, 336, 880, 16, 16, dim);
            m.piece(cx + 180, y + 4, 352, 880, 16, 16, dim);
            break;
        }
        m.setScale(0, 0, 1);
        if (cursor && row.kind == OptControl::Pair) m.text(">", kOptCtrlX + ctrlX - 12, y + 4, false, a);
    }
    // "n / total" from the game's 14 x 14 digits, and Back
    m.digit(app.optionsPage + 1, 20, kOptPageY, a);
    m.digit(-1, 34, kOptPageY, a);
    m.digit(kOptionPages, 48, kOptPageY, a);
    m.backButton(192, kOptBackY, app.optionsRow == kOptionsBackRow, app.optionsBob, a);
    if (app.optionsRow >= 0 && app.optionsRow < int(rows.size()) && !rows[size_t(app.optionsRow)].hint.empty())
        m.text(rows[size_t(app.optionsRow)].hint, 20, std::min(optRowY(int(rows.size())) + 2, kOptBackY - 16), false, a * 0.9f, 9);
    if (app.showHud) // (our key hints: off in a Release build, F3)
        m.text("Up/Down  Left/Right  Enter  Q/E or PgUp/PgDn: page  Esc: back", 20, 372, false, a * 0.8f, 9);
    r.setOrigin(r.uiX());
}

// One 30 Hz step: the slide-in and the Back arrows' bob (0..5 and back)
void stepOptions(App& app) {
    takeVitaFolder(app);
    if (!app.optionsOpen) return;
    ++app.optionsAnim;
    app.optionsBob += app.optionsBobDir;
    if (app.optionsBob >= 5 || app.optionsBob <= 0) app.optionsBobDir = -app.optionsBobDir;
}

// Keys: Up/Down rows (then Back), Left/Right change, Enter activates, Q/E PgUp/PgDn pages
void optionsKey(App& app, SDL_Keycode key) {
    std::vector<OptRow> rows = optionRows(app, app.optionsPage);
    const int n = int(rows.size());
    int& row = app.optionsRow;
    auto at = [&]() -> OptRow* { return row >= 0 && row < n ? &rows[size_t(row)] : nullptr; };
    switch (key) {
    case SDLK_ESCAPE: case SDLK_BACKSPACE: app.optionsOpen = false; menuSound(app, se::kBack); return;
    case SDLK_UP: case SDLK_W: row = row == kOptionsBackRow ? n - 1 : row > 0 ? row - 1 : kOptionsBackRow; break;
    case SDLK_DOWN: case SDLK_S: row = row == kOptionsBackRow ? 0 : row + 1 < n ? row + 1 : kOptionsBackRow; break;
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
    menuSound(app, se::kCursor);
}

// A click at frame pixel (px, py): menu units, then the control there
void optionsClick(App& app, float px, float py) {
    float lx, ly;
    app.renderer.toLogical(px, py, lx, ly);
    float mx, my;
    menuPointFit(app, lx, ly, mx, my);
    if (my >= kOptBackY && my < kOptBackY + 24 && mx >= 192 && mx < 424) { app.optionsOpen = false; menuSound(app, se::kBack); return; }
    if (my >= kOptPageY - 4 && my < kOptPageY + 18 && mx >= 16 && mx < 66) { // "n / m": the next page
        openOptions(app, app.optionsPage + 1);
        menuSound(app, se::kBack);
        return;
    }
    std::vector<OptRow> rows = optionRows(app, app.optionsPage);
    for (size_t i = 0; i < rows.size(); ++i) {
        OptRow& r = rows[i];
        const float y = optRowY(int(i));
        if (my < y - 4 || my >= y + 28) continue;
        app.optionsRow = int(i);
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
    s.leader = app.playerChar;
    s.playSeconds = app.playSeconds;
    s.savedAt = int64_t(std::time(nullptr));
    s.game = app.game;
    s.game.frmScene = s.game.frmRecord = 0;
    return s;
}

void placePlayer(App& app);

// Back on the field the save was made on, as arriving there by warp (the room's entry scripts
// run as they would)
bool applySave(App& app, const cr::SaveData& s) {
    if (s.map < 0 || s.map >= cr::kNumMaps) return false;
    bootTitle(app); // everything closed and torn down
    app.runner.stop();
    app.booting = false;
    app.titleOpen = false;
    app.game = s.game;
    app.treasureBoxes.clear();
    app.entryArea = -1;
    app.playSeconds = s.playSeconds;
    app.playerChar = std::clamp(s.leader, 0, 7);
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

// One save per pair (each pair's story is its own): save1.dat .. save4.dat
std::string slotPath(const App& app, int pair) {
    const std::string name = "save" + std::to_string(std::clamp(pair, 1, 4)) + ".dat";
    return savePath(app, name.c_str());
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
    ss.haveSave = cr::readSave(slotPath(app, load ? app.chosenPair : currentPair(app)), ss.data);
    if (load) { // __menuu_SaveLoadSet, the title's: the emblems and plates from every save's flags
        const cr::GameState sys = allPairsFlags(app);
        const int p = std::clamp(app.chosenPair - 1, 0, 3);
        const int emblem[3] = {31 + 2 * p, 32 + 2 * p, 39};
        for (int k = 0; k < 3; ++k)
            if (sys.flag(emblem[k])) ss.emblems |= 1u << k;
        ss.allClear = sys.flag(32) && sys.flag(34) && sys.flag(36) && sys.flag(38);
        ss.cleared = sys.flag(40 + p);
    }
    if (!load) { // save: straight to "Save data?"
        ss.phase = 1;
        ss.pending = 1;
        ss.message = 57012;
    }
}

void closeSaveScreen(App& app) { app.saveScreen.open = false; }

// Yes in the confirmation: do it
void saveScreenYes(App& app) {
    App::SaveScreen& ss = app.saveScreen;
    if (ss.pending == 1) {
        const cr::SaveData s = captureSave(app);
        if (cr::writeSave(slotPath(app, currentPair(app)), s)) {
            ss.data = s;
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
        if (applySave(app, s)) {
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
        ss.open = false;
        if (ss.kind == 1) app.chosenPair = 1; // Set_Prtyno(0): the last battle is the first pair's
        if (app.booting && app.runner.running()) {
            app.game.vars[1] = ss.kind; // SaveLoadClr: SetVar(1, the choice) - the boot script branches on it
        } else {
            app.titleOpen = false;
            app.runner.stop();
            startNewGame(app);
        }
        return;
    }
    ss.phase = 2;
    ss.timer = 0;
}

void saveScreenChoose(App& app, int i) { // the title's plates (savePlates): the last battle, Load, New
    App::SaveScreen& ss = app.saveScreen;
    const std::vector<int> plates = savePlates(app);
    if (i < 0 || i >= int(plates.size())) return;
    ss.cursor = i;
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
void drawSaveDate(App& app, cr::MenuPainter& m, const cr::SaveData& d, float a) {
    (void)app;
    const time_t when = time_t(d.savedAt);
    struct tm tm {};
#ifdef _WIN32
    localtime_s(&tm, &when);
#else
    localtime_r(&when, &tm);
#endif
    const float dark[4] = {0, 0, 0, 0.5f * a}, white[4] = {1, 1, 1, a};
    m.quad(324, 12, 148, 64, dark, dark);
    m.picture(125, 372, 12, a);
    m.quad(325, 28, 146, 1, white, white);
    m.picture(126 + std::clamp(tm.tm_mon + 1, 1, 12), 336, 38, a);
    drawShopNumber(m, tm.tm_mday, 378, 38, 2, 12, a);
    m.picture(139, 400, 38, a);
    drawShopNumber(m, tm.tm_year + 1900, 410, 38, 4, 12, a);
    const int digit[4] = {tm.tm_hour / 10, tm.tm_hour % 10, tm.tm_min / 10, tm.tm_min % 10};
    static const float kAt[4] = {0, 12, 36, 48};
    for (int i = 0; i < 4; ++i)
        if (i || tm.tm_hour >= 10) drawShopNumber(m, digit[i], 368 + kAt[i], 58, 1, 12, a);
    m.picture(75, 392, 58, a);
    const int secs = int(std::max(0.0, d.playSeconds));
    const int hours = std::min(999, secs / 3600), mins = std::min(59, secs / 60 % 60);
    m.quad(324, 84, 148, 40, dark, dark);
    m.picture(126, 344, 88, a);
    m.quad(325, 100, 146, 1, white, white);
    const int pd[5] = {hours / 100, hours % 100 / 10, hours % 10, mins / 10, mins % 10};
    static const float kPlay[5] = {32, 44, 56, 80, 92};
    for (int i = 0; i < 5; ++i)
        if (i || pd[i]) drawShopNumber(m, pd[i], 324 + kPlay[i], 84 + 22, 1, 12, a);
    m.picture(75, 324 + 68, 84 + 22, a);
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
    // On the title (__menuu_SaveLoadPut, mode 1) the pair's Character Select art behind it all: piece
    // 16 of texture 0x11 (0x12 with flag 4092, the second sheet) at tex2d (-32, -64), z -10
    // (dimmed a little for the plates and text)
    if (art) drawCharArt(app, 1, false, 0.65f);
    else {
        r.setOrigin(0);
        r.fillRect(0, 0, r.screenW(), cr::Renderer::kLogicalH, 0, 0, 0, 0.6f);
        r.setOrigin(r.uiX());
    }
    const float a = std::min(1.0f, ss.anim / 8.0f);
    // Widescreen: the heading, face boards, emblems to the screen's left; the date, the plates to
    // its right; Back and the message centred (as the shop)
    r.setOrigin(wideLeft(app));
    if (!ss.load) m.picture(81, 8, 4, a); // "Camp"
    // The slot (__menuu_SaveLoadPut): the pair's face boards (ComUty_FaceBoardDraw) at (0, 24) and
    // (0, 112) - the save's on the title, the party's when saving -, on the title the save's date
    // and play time (ComUty_GameTimeAndPlayTimeDraw) top right and the pair's emblems (pictures
    // 111..113 at (16 + 72k, 224))
    const cr::GameState* shown = ss.load ? (ss.haveSave ? &ss.data.game : nullptr) : &app.game;
    if (shown)
        for (int k = 0; k < 2; ++k) {
            const int ch = ss.load ? (ss.data.leader & ~1) + k : (app.playerChar & ~1) + k;
            int hp, hpMax, mp, mpMax;
            memberHpMp(app, ch, hp, hpMax, mp, mpMax, shown);
            drawFaceBoard(app, m, 0, 24 + 88.0f * float(k), ch, hp, hpMax, mp, mpMax, a, shown);
        }
    if (ss.load && !ss.haveSave) m.text("No save data", 24, 100, false, a, 16);
    for (int k = 0; k < 3; ++k)
        if (ss.emblems & (1u << k)) m.picture(111 + k, 16 + 72.0f * float(k), 224, a);
    r.setOrigin(wideRight(app));
    if (ss.load && ss.haveSave) drawSaveDate(app, m, ss.data, a);
    if (ss.phase == 0) { // the plates at (328, 136 + 48 x which): the last battle, Load, New
        const std::vector<int> plates = savePlates(app);
        static const int kWord[3] = {4, 7, 6};
        for (size_t i = 0; i < plates.size(); ++i)
            m.plate(328, 136 + 48.0f * float(plates[i]), plateIcon(kWord[plates[i]]), kWord[plates[i]], ss.cursor == int(i), a,
                    false); // (window 3; the icon is the word's: ComUty_ChoicePlateDraw)
        r.setOrigin(r.uiX());
        m.backButton(192, r.phoneLayout() ? 286 : 344, false, 0, a);
    } else if (r.phoneLayout()) { // the original's (SaveLoadPut, the same in 2.2.0): the message
        // board (ComUty_MsgBoardDraw, picture 12) at (0, 220), lines from (24, 234); Yes / No the
        // plates at window 6 + (336, 104) / (336, 160)
        m.picture(12, 0, 220, a);
        const std::string msg = ss.message > 0 ? sysText(app, ss.message) : std::string("The save couldn't be written.");
        m.text(msg.empty() ? "?" : msg, 24, 234, false, a, 14);
        int extra = 0;
        for (int l : ss.lines)
            if (l > 0) m.text(sysText(app, l), 24, 234 + 16.0f * float(++extra), false, a, 14);
        if (ss.phase == 1) {
            m.plate(336, 104, 0, 0, ss.confirm == 0, a, false);
            m.plate(336, 160, 1, 1, ss.confirm == 1, a, false);
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
        if (ss.phase == 1) {
            m.button(128, 296, 0, "", ss.confirm == 0, a, ss.confirm == 0); // Yes
            m.button(272, 296, 1, "", ss.confirm == 1, a, ss.confirm == 1); // No
        }
    }
    r.setOrigin(wideLeft(app));
    if (keyHints(app)) // (our key hints)
        m.text(ss.phase == 1 ? "Left/Right  Enter  Esc: no" : ss.phase == 0 ? "Up/Down  Enter  Esc: back" : "", 20, 372, false,
           a * 0.8f, 9);
    r.setOrigin(r.uiX());
}

// One 30 Hz step: the fade-in, and the "done" message closing itself
void stepSaveScreen(App& app) {
    App::SaveScreen& ss = app.saveScreen;
    if (!ss.open) return;
    ++ss.anim;
    if (ss.phase == 2 && ++ss.timer >= 40) {
        ss.open = false;
        if (ss.load && ss.message > 0) app.titleOpen = false; // loaded: on with the game
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
        if (key == SDLK_LEFT || key == SDLK_A || key == SDLK_RIGHT || key == SDLK_D) { ss.confirm ^= 1; menuSound(app, se::kCursor); }
        else if (enter && ss.confirm == 0) saveScreenYes(app);
        else if (enter || back) { // No
            menuSound(app, se::kChoose);
            if (ss.load && ss.kind == 3) { // then a plain new game? (57089..57091)
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
    if (my >= backY && my < backY + 24 && mx >= 192 && mx < 424) saveScreenKey(app, SDLK_ESCAPE);
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

// The price of one in the current list (half when selling)
int shopUnitPrice(const App& app, int item) {
    const cr::ItemParam* p = app.itemTable.item(item);
    if (!p) return 0;
    if (app.shop.selling) return cr::sellPrice(*p);
    const int pr = shopRoute(app) * 2; // 173 Sexy Pose on either member: 10% off
    const bool pose = cr::hasSkill(app.game, pr, 173) || cr::hasSkill(app.game, pr + 1, 173);
    return pose ? cr::buyPrice(*p) * 9 / 10 : cr::buyPrice(*p);
}

int shopMaxCount(const App& app, int item) {
    if (app.shop.selling) return std::min(ownedCount(app, item), cr::kMaxItemCount);
    return cr::maxPurchase(shopUnitPrice(app, item), app.game.oz, ownedCount(app, item));
}

void refreshShopList(App& app) {
    App::Shop& s = app.shop;
    s.list = s.selling ? cr::sellableItems(app.game, app.itemTable, shopCharacter(app), s.category)
                       : cr::shopStock(app.game, app.itemTable, shopCharacter(app), s.category, s.level);
    s.listCursor = std::clamp(s.listCursor, 0, std::max(0, int(s.list.size()) - 1));
    s.listTop = std::clamp(s.listTop, std::max(0, s.listCursor - shopui::kRows + 1), s.listCursor);
}

void openShop(App& app) {
    App::Shop& s = app.shop;
    cr::Texture keeper = s.keeper;
    s = App::Shop();
    s.keeper = keeper;
    s.open = true;
    s.level = cr::shopLevel(app.game, shopRoute(app));
    s.member = std::clamp(app.playerChar - shopRoute(app) * 2, 0, 1);
    s.bgmBack = app.game.vars[0]; // ShopSet: var 0 kept, the shop's music (_bgmgo(0x13))
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

void stepShop(App& app) {
    App::Shop& s = app.shop;
    if (!s.open) return;
    ++s.anim;
    if (s.phase == App::Shop::Talk) {
        if (s.timer >= 0 && ++s.timer > 4) { // ShopPrc waits 4 frames, then starts it
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
        } else if (s.timer < 0 && !app.runner.running()) { // done: the shop's script again
            app.runner.swap(app.parkedRunner);
            s.talkRunning = false;
            app.game.setFlag(4090, false);
            s.phase = App::Shop::Main;
            s.anim = 0;
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

void shopChooseCategory(App& app, int c) {
    App::Shop& s = app.shop;
    s.category = c;
    s.listCursor = s.listTop = 0;
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
void shopSwitchMember(App& app) {
    App::Shop& s = app.shop;
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
    }
    menuSound(app, se::kPaid);
    const cr::ItemParam* p = app.itemTable.item(item);
    if (!s.selling && p && cr::equipSlot(p->kind) >= 0 && (p->owners >> shopCharacter(app) & 1) &&
        app.game.party[size_t(shopCharacter(app))].equip[cr::equipSlot(p->kind)] != item) {
        s.phase = App::Shop::EquipAsk;
        s.message = 57076; // Equip [item]?
        s.confirm = 0;
        return;
    }
    shopBackToList(app);
}

// Yes to "Equip it?": worn by the board's character, what they wore goes back in the bag
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
    }
    shopBackToList(app);
}

void shopBack(App& app) {
    App::Shop& s = app.shop;
    menuSound(app, se::kBack);
    switch (s.phase) {
    case App::Shop::Main: closeShop(app); break;
    case App::Shop::Category: s.phase = App::Shop::Main; s.anim = 0; break;
    case App::Shop::List: s.phase = App::Shop::Category; s.anim = 0; break;
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
    const std::string t = std::to_string(std::abs(value));
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
void drawShopBoard(App& app, float bx, float by, int c, int item, float a) {
    using namespace shopui;
    cr::MenuPainter& m = app.menu;
    const cr::PartyMember& pm = app.game.party[size_t(c)];
    const float white[4] = {1, 1, 1, a};
    m.quad(bx + 12, by, 143, 16, white, white); // the name
    m.textCentred(sysText(app, 58029 + c), bx + 12 + (143 - m.textWidth(sysText(app, 58029 + c), 14)) / 2, by + 8, true, a,
                  14);
    m.picture(144 + (facePicture(c) - 29), bx - 8, by - 32, a);
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
    if (!p || cr::equipSlot(p->kind) < 0) return;
    for (int st = 0; st < 6; ++st) { // the change: ▲ / ▼ at x + 158, sign and amount from x + 128 for the stats
        const int d = cr::memberStat(app.game, c, cr::Stat(st), app.itemTable, item) - cr::memberStat(app.game, c, cr::Stat(st), app.itemTable);
        if (d == 0) continue;
        const float y = by + kStatRowY[st];
        m.picture(d > 0 ? 93 : 94, bx + 158, y, a);
        if (st < 2) continue;
        const int ad = std::abs(d);
        m.picture(d > 0 ? 89 : 92, bx + 128 + (ad >= 100 ? 0 : ad >= 10 ? 7 : 14), y, a);
        drawShopNumber(m, ad, bx + 135, y, 3, 7, a, d > 0 ? 5 : 4);
    }
}

// ComUty_NumCountDraw at (348, 68): white < and > buttons fading out at the ends around the
// number in big digits (picture 26 on: 28 x 28)
void drawNumCount(cr::MenuPainter& m, int count, int most) {
    const float on[4] = {1, 1, 1, 0xB3 / 255.0f}, off[4] = {1, 1, 1, 0}, dark[4] = {0, 0, 0, 0.55f};
    m.quad(336, 60, 15, 31, off, on);
    m.quad(351, 60, 20, 31, on, on);
    m.quad(372, 60, 63, 31, dark, dark);
    m.quad(436, 60, 19, 31, on, on);
    m.quad(455, 60, 16, 31, on, off);
    m.picture(99, 348, 68, count > 1 ? 1.0f : 0.35f);
    m.picture(100, 448, 68, count < most ? 1.0f : 0.35f);
    const short* big = cr::kMenuPicture[26];
    if (count >= 10) m.piece(376, 62, big[0] + float(count / 10 % 10) * 28, big[1], 28, 28, 1);
    m.piece(404, 62, big[0] + float(count % 10) * 28, big[1], 28, 28, 1);
}

void drawShop(App& app) {
    using namespace shopui;
    App::Shop& s = app.shop;
    if (s.phase == App::Shop::Talk) return; // the shopkeeper's scene is up instead
    cr::Renderer& r = app.renderer;
    cr::MenuPainter& m = app.menu;
    m.begin(r, app.windowTex, &app.font);
    const PhoneMenu phoneMenu(app, m); // the iPhone layout: its screen's menu coordinates
    const float a = std::min(1.0f, float(s.anim + 1) / 6.0f), slide = (1 - a) * (1 - a);
    const bool phone = r.phoneLayout(); // the iPhone layout: one 480-wide screen, no spreading
    const float leftEdge = phone ? r.uiX() : 0, centre = r.uiX(), rightEdge = phone ? r.uiX() : 2 * r.uiX(); // origins
    const bool list = s.phase == App::Shop::List;
    const bool asking = s.phase == App::Shop::Count || s.phase == App::Shop::EquipAsk;
    const bool board = list && shopEquipment(app);
    const int item = (list || asking) && !s.list.empty() ? s.list[size_t(s.listCursor)] : 0;

    // Left: the shopkeeper (or the character's board), the heading and the section labels
    r.setOrigin(leftEdge);
    if (s.keeper.id && !list && !board)
        r.drawQuad(s.keeper, (-32 - 240 * slide) * 2, 128 * 2, 256 * 2, 256 * 2, 1, 1, 1, 1);
    if (board) drawShopBoard(app, 12 - 200 * slide, 92, shopCharacter(app), s.selling ? 0 : item, a);
    m.picture(97, 8, 4, 1); // "Shop"
    if (s.phase != App::Shop::Main) {
        m.textCentred(s.selling ? "Sell" : "Buy", 12, 47, false, 1, 15); // (FontDisp's 12, 40: the box's top)
        m.picture(15, 0, 56, 1);
        if (list || asking) {
            static const char* const kCategory[4] = {"Items", "Weapons", "Armor", "Jewel"};
            m.textCentred(std::string(">") + kCategory[std::clamp(s.category, 0, 3)], 40, 65, false, 1, 15);
            m.picture(16, 24, 72, 1);
        }
    }

    // Right: Oz, the plates, the list, the count and Yes / No
    r.setOrigin(rightEdge);
    if (list && !s.list.empty()) { // HELP: the cursor's item, from the list's left to the screen's edge
        const float hx = 152, hw = 512 - hx + (rightEdge > 0 ? 0 : 0);
        m.stretch(12, hx, 0, hw, 66, a);
        m.picture(18, hx + 4, 2, a);
        std::vector<std::vector<std::string>> help;
        if (app.systemText.message(cr::itemHelpId(item), app.language, nullptr, help) && !help.empty()) {
            float y = 20;
            for (const std::string& line : help[0]) {
                m.text(line, hx + 14, y, false, a, 13);
                if ((y += 16) > 52) break;
            }
        }
    }
    m.picture(152, 316, 0, 1); // the Oz bar
    drawShopNumber(m, app.game.oz, 362, 0, 9, 12, 1);
    if (s.phase == App::Shop::Main)
        for (int i = 0; i < 3; ++i)
            m.plate(328 + 240 * slide, 144 + 48.0f * float(i), kMainIcons[i], kMainWords[i], s.cursor == i, a, false);
    if (s.phase == App::Shop::Category)
        for (int i = 0; i < 4; ++i)
            m.plate(328 + 240 * slide, 96 + 48.0f * float(i), kCategoryIcons[i], kCategoryWords[i], s.category == i, a, false);
    if (list) {
        if (s.list.empty())
            m.text(s.selling ? "Nothing to sell." : "Nothing for sale.", kRowX + 56, kRowY + 16, false, a, 15);
        for (int k = 0; k < kRows && s.listTop + k < int(s.list.size()); ++k) {
            const int index = s.listTop + k, it = s.list[size_t(index)];
            const float x = kRowX + 160 * slide, y = kRowY + kRowH * float(k);
            const float ra = shopMaxCount(app, it) > 0 ? a : a * 0.55f; // unaffordable rows dim (our addition)
            m.picture(index == s.listCursor ? 9 : 8, x, y, ra);
            if (const cr::ItemParam* p = app.itemTable.item(it); p && p->icon >= 0)
                m.piece(x + 12, y + 8, float(p->icon % 16) * 32, 248 + float(p->icon / 16) * 32, 32, 32, ra);
            m.text(itemName(app, it), x + 56, y + 12, false, ra, 14);
            drawShopNumber(m, shopUnitPrice(app, it), x + 158, y + 30, 6, 10, ra);
            m.picture(101, x + 226, y + 34, ra);
        }
        if (int(s.list.size()) > kRows) { // the scroll bar: track, ▲ ▼, the knob
            const float sx = 462, top = kRowY, h = kRowH * kRows;
            const float dark[4] = {0.05f, 0.05f, 0.08f, 0.8f * a};
            m.quad(sx, top, 8, h, dark, dark);
            m.picture(76, sx - 4, top - 14, a);
            m.picture(77, sx - 4, top + h - 2, a);
            const float span = h - 40, f = float(s.listTop) / float(int(s.list.size()) - kRows);
            m.picture(106, sx, top + span * f, a);
        }
    }
    if (asking && item > 0) {
        if (s.phase == App::Shop::Count) {
            m.picture(98, 312, 40, 1); // PRICE and the total
            drawShopNumber(m, shopUnitPrice(app, item) * s.count, 362, 40, 9, 12, 1);
            m.picture(73, 362, 49, 1);
            drawNumCount(m, s.count, shopMaxCount(app, item));
        }
        m.plate(336, 104, 0, 0, s.confirm == 0, 1, false); // Yes
        m.plate(336, 160, 1, 1, s.confirm == 1, 1, false); // No
    }

    // Centre: Back (with ◀ ▶ for equipment lists) and the message board
    r.setOrigin(centre);
    if (asking && item > 0) {
        m.picture(12, 0, 220, 1);
        m.text(shopMessage(app), 24, 234, false, 1, 15);
        std::vector<std::vector<std::string>> have;
        if (s.phase == App::Shop::Count &&
            app.systemText.message(57046, app.language, [&](int) { return ownedCount(app, item); }, have) && !have.empty() &&
            !have[0].empty())
            m.text(have[0][0], 24, 252, false, 0.85f, 14);
    } else if (list && shopEquipment(app)) {
        const float white[4] = {1, 1, 1, a};
        m.quad(kBackX + 29, kBackY, 22, 24, white, white);
        m.picture(99, kBackX + 32, kBackY + 4, a);
        m.quad(kBackX + 56, kBackY, 120, 24, white, white);
        m.piece(kBackX + 78, kBackY, 0, 224, 76, 24, a);
        m.quad(kBackX + 181, kBackY, 22, 24, white, white);
        m.picture(100, kBackX + 184, kBackY + 4, a);
    } else {
        m.backBar(kBackX, kBackY, false, a);
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
    const bool left = key == SDLK_LEFT || key == SDLK_A, right = key == SDLK_RIGHT || key == SDLK_D;
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
    case App::Shop::List: {
        const int n = int(s.list.size());
        if ((up || down) && n > 0) {
            s.listCursor = (s.listCursor + (up ? n - 1 : 1)) % n;
            s.listTop = std::clamp(s.listTop, std::max(0, s.listCursor - shopui::kRows + 1), s.listCursor);
            menuSound(app, se::kCursor);
        } else if ((left || right) && shopEquipment(app)) shopSwitchMember(app);
        else if (enter) shopChooseItem(app, s.listCursor);
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
    const bool onBack = my >= kBackY && my < kBackY + 24 && cx >= kBackX && cx < kBackX + 232;
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
    case App::Shop::List: {
        using namespace shopui;
        const bool arrows = shopEquipment(app) && my >= kBackY && my < kBackY + 24;
        if (rx >= kRowX && rx < kRowX + 256 && my >= kRowY && my < kRowY + kRowH * kRows) {
            const int index = s.listTop + int((my - kRowY) / kRowH);
            if (index < int(s.list.size())) shopChooseItem(app, index);
        } else if (rx >= 455 && rx < 480 && my >= kRowY - 16 && my < kRowY + 20 && s.listTop > 0) --s.listTop;
        else if (rx >= 455 && rx < 480 && my >= kRowY + kRowH * kRows - 20 && my < kRowY + kRowH * kRows + 16 &&
                 s.listTop + kRows < int(s.list.size()))
            ++s.listTop;
        else if (arrows && ((cx >= kBackX + 29 && cx < kBackX + 51) || (cx >= kBackX + 181 && cx < kBackX + 203)))
            shopSwitchMember(app);
        else if (onBack) shopBack(app);
        break;
    }
    case App::Shop::Count:
        if (my >= 60 && my < 91 && rx >= 336 && rx < 372) shopKey(app, SDLK_LEFT);
        else if (my >= 60 && my < 91 && rx >= 436 && rx < 472) shopKey(app, SDLK_RIGHT);
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
namespace campui {
constexpr int kWords[5] = {8, 9, 10, 11, 3};                 // Items, Equip, Genes, Status, Options
constexpr bool kReady[5] = {true, true, true, true, true};
constexpr int kEquipSlots = 7;                                 // weapon, armor, jewel, Human Genes, 3 plates
constexpr int kEntries = 6;                                    // and Save
constexpr float kPlateX = 328, kPlateY = 48, kPlateStep = 48;
constexpr float kMemberY = 176, kMemberStep = 56;
constexpr int kCategoryKind[6] = {14, 3, 6, 7, 8, 10};         // make_item_list (0xc8660)
constexpr int kCategoryLabel[6] = {58071, 58072, 58073, 58074, 58075, 58079}; // (0xc8640)
constexpr float kRowX = 192, kRowY = 92, kRowH = 48;
constexpr int kRows = 4;
constexpr float kBackX = 192, kBackY = 344;
constexpr float kPhoneBackY = 286; // 2.2.0's (__MuCamp_NextProc's box (192, 286, 256, 34)); it has no Save plate
// kanIconNoTbl (0xc8740): a plate word's icon + 1
constexpr int kIconOfWord[37] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 14, 13, 13, 14, 13,
                                 14, 15, 16, 17, 18, 19, 20, 21, 23, 24, 25, 26, 27, 28, -1, -1, 14, 14};
constexpr uint32_t kSoundEquip = 21, kSoundUse = 22;
} // namespace campui

bool keyHints(const App& app) { return app.showHud && !app.renderer.phoneLayout(); }
int plateIcon(int word) { return word >= 0 && word < 37 ? campui::kIconOfWord[word] - 1 : -1; }
int campRoute(const App& app) { return std::clamp(app.playerChar / 2, 0, 3); }
int campCharacter(const App& app, int member) { return campRoute(app) * 2 + std::clamp(member, 0, 1); }

// ComUty_BattryAndRealTimeAndOzDraw: the clock (TIME, 71, at (363, 8); the digits at +47 / +59 /
// +83 / +95, the colon (75) blinking at +71; the line (74) under it) and the purse (OZ, 72, at
// (326, 24); nine digits from (362, 26); the line at (362, 36)). The battery isn't drawn.
void drawClockAndOz(App& app, cr::MenuPainter& m, float a) {
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
    if (SDL_GetTicks() / 667 % 2 == 0) m.picture(75, 363 + 71, 6, a);
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
    m.picture((app.language == 0 ? 45 : 53) + face, x + 96, y + 24, a);
    m.picture(29 + face, x, y, a);
    m.picture(61, x + 24, y + 86, a);
    drawShopNumber(m, pm.level, x + 48, y + 86, 3, 12, a, 1, true);
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
    mp = pm.mp < 0 ? mpMax : std::min(pm.mp, mpMax);
}

// MuComMakeItemHaveData: what the bag holds of a category (the pair's own weapons and armor)
std::vector<int> campItems(const App& app, int category) {
    std::vector<int> out;
    int from = 1, to = 60;
    switch (campui::kCategoryKind[std::clamp(category, 0, 5)]) {
    case 3: from = 61, to = 80; break;
    case 6: case 7: from = 201, to = 424; break;
    case 8: from = 425, to = 438; break;
    case 10: from = 479, to = 500; break;
    default: break;
    }
    const int route = campRoute(app), bits = 3 << (2 * route);
    for (int n = from; n <= to; ++n) {
        if (ownedCount(app, n) <= 0) continue;
        const cr::ItemParam* p = app.itemTable.item(n);
        if (category == 2 && (!p || p->kind != cr::kKindWeapon || !(p->owners & bits))) continue;
        if (category == 3 && (!p || p->kind != cr::kKindArmor || !(p->owners & bits))) continue;
        out.push_back(n);
    }
    return out;
}

// What can go in member c's slot: the bag's items of the slot's kind that c may wear
std::vector<int> campEquipList(const App& app, int c, int slot) {
    std::vector<int> out;
    if (slot >= 3) { // gene plates: the bag's, but not one either member wears already
        if (slot == 3) return out; // the Human Genes stay
        const cr::PartyMember& pm = app.game.party[size_t(c)];
        if (pm.genes[slot - 3] > 0) out.push_back(0); // take it off
        for (int p = cr::kFirstPlate + 2; p < cr::kFirstPlate + cr::kPlates; ++p) {
            if (ownedCount(app, p) <= 0) continue;
            bool worn = false;
            for (int k = 0; k < 2; ++k)
                for (int g : app.game.party[size_t(campCharacter(app, k))].genes) worn |= g == p;
            if (!worn) out.push_back(p);
        }
        return out;
    }
    const int kind = slot == cr::kWeapon ? cr::kKindWeapon : slot == cr::kArmor ? cr::kKindArmor : cr::kKindJewel;
    if (slot == cr::kJewel && app.game.party[size_t(c)].equip[cr::kJewel] > 0) out.push_back(0); // take it off
    for (int n = 201; n <= 438; ++n) {
        const cr::ItemParam* p = app.itemTable.item(n);
        if (!p || p->kind != kind || ownedCount(app, n) <= 0 || !(p->owners & (1 << c))) continue;
        out.push_back(n);
    }
    return out;
}

bool campUsable(int item) { return (item >= 3 && item <= 6) || (item >= 24 && item <= 27); }

void campMessage(App& app, const std::string& text) {
    app.camp.message = text;
    app.camp.messageFrames = text.empty() ? 0 : 90;
}

int campEntries(const App& app) { return app.renderer.phoneLayout() ? campui::kEntries - 1 : campui::kEntries; }
float campBackY(const App& app) { return app.renderer.phoneLayout() ? campui::kPhoneBackY : campui::kBackY; }

void openCamp(App& app) {
    App::Camp& c = app.camp;
    app.campOpen = true;
    auto art = std::move(c.plateArt); // the plates' pictures stay loaded
    c = App::Camp();
    c.plateArt = std::move(art);
    c.cursor = campEntries(app) - 1;
}

void campRefreshList(App& app) {
    App::Camp& c = app.camp;
    if (c.page == App::Camp::Items) c.list = campItems(app, c.category);
    else if (c.page == App::Camp::Equip) c.list = campEquipList(app, campCharacter(app, c.member), c.slot);
    c.listCursor = std::clamp(c.listCursor, 0, std::max(0, int(c.list.size()) - 1));
    c.listTop = std::clamp(c.listTop, std::max(0, c.listCursor - campui::kRows + 1), c.listCursor);
}

// Items: the next category that holds anything, `dir` from the current (the current one too if 0)
void campNextCategory(App& app, int dir) {
    App::Camp& c = app.camp;
    for (int k = dir ? 1 : 0; k <= 6; ++k) {
        const int cat = ((c.category + dir * k) % 6 + 6) % 6;
        if (!campItems(app, cat).empty() || k == 6) {
            c.category = cat;
            break;
        }
    }
    c.listCursor = c.listTop = 0;
    campRefreshList(app);
}

void campOpenPage(App& app, int i) {
    App::Camp& c = app.camp;
    c.cursor = i;
    if (i < 5 && !campui::kReady[i]) {
        menuSound(app, se::kBuzzer);
        return;
    }
    menuSound(app, se::kChoose);
    c.anim = 0;
    c.member = 0;
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
        c.list.clear();
        for (int p = cr::kFirstPlate; p < cr::kFirstPlate + cr::kPlates; ++p)
            if (ownedCount(app, p) > 0) c.list.push_back(p);
        c.listCursor = c.listTop = 0;
        break;
    case 3: c.page = App::Camp::Status; c.phase = App::Camp::Member; break;
    case 4: openOptions(app, 0); break;
    default: app.campOpen = false; openSaveScreen(app, false); break; // Save
    }
}

void campBack(App& app) {
    App::Camp& c = app.camp;
    menuSound(app, se::kBack);
    c.message.clear();
    switch (c.page) {
    case App::Camp::Main: app.campOpen = false; menuSound(app, se::kCampClose); return;
    case App::Camp::Items:
        if (c.phase == App::Camp::Ask) c.phase = App::Camp::Member;
        else if (c.phase == App::Camp::Member) c.phase = App::Camp::List;
        else { c.page = App::Camp::Main; c.anim = 0; }
        return;
    case App::Camp::Equip:
        if (c.phase == App::Camp::List) { c.phase = App::Camp::Slot; return; }
        if (c.phase == App::Camp::Slot) { c.phase = App::Camp::Member; return; }
        c.page = App::Camp::Main;
        c.anim = 0;
        return;
    case App::Camp::Status:
        if (c.phase == App::Camp::Slot) { c.phase = App::Camp::Member; return; } // Slot = the detail
        c.page = App::Camp::Main;
        c.anim = 0;
        return;
    case App::Camp::Genes: c.page = App::Camp::Main; c.anim = 0; return;
    }
}

// Items: use the chosen item on member c (use_item); false when it does nothing
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
        const int mp = pm.mp < 0 ? most : std::min(pm.mp, most);
        if (mp >= most) return false;
        static const int kGain[3] = {100, 250, 500};
        pm.mp = item == 27 ? most : std::min(most, mp + kGain[item - 24]);
        count = 1;
    } else
        return false;
    if ((app.game.items[item] -= count) <= 0) app.game.items.erase(item);
    return true;
}

int campSeedLimit(const App& app, int character, int item) {
    const cr::PartyMember& pm = app.game.party[size_t(character)];
    return std::max(0, std::min({ownedCount(app, item), cr::kMaxItemCount, cr::seedRoom(pm, cr::Stat(cr::kAttack + item - 3))}));
}

void campChoose(App& app) {
    App::Camp& c = app.camp;
    const int item = c.list.empty() ? -1 : c.list[size_t(c.listCursor)];
    switch (c.page) {
    case App::Camp::Main: campOpenPage(app, c.cursor); return;
    case App::Camp::Items:
        if (c.phase == App::Camp::List) {
            if (item <= 0 || !campUsable(item)) { menuSound(app, se::kBuzzer); return; }
            menuSound(app, se::kChoose);
            c.phase = App::Camp::Member;
        } else if (c.phase == App::Camp::Member) {
            const int ch = campCharacter(app, c.member);
            if (item >= 3 && item <= 6 && campSeedLimit(app, ch, item) <= 0) { menuSound(app, se::kBuzzer); return; }
            menuSound(app, se::kChoose);
            c.phase = App::Camp::Ask;
            c.count = 1;
            c.confirm = 0;
        } else if (c.phase == App::Camp::Ask) {
            if (c.confirm != 0) { campBack(app); return; }
            if (campUseItem(app, campCharacter(app, c.member), item, c.count)) menuSound(app, campui::kSoundUse);
            else menuSound(app, se::kBuzzer); // full already (the original's buzzer, effgo 4)
            c.phase = App::Camp::List;
            campRefreshList(app);
            if (c.list.empty()) campNextCategory(app, 0);
        }
        return;
    case App::Camp::Equip:
        if (c.phase == App::Camp::Member) { menuSound(app, se::kChoose); c.phase = App::Camp::Slot; c.slot = 0; }
        else if (c.phase == App::Camp::Slot) {
            c.listCursor = c.listTop = 0;
            campRefreshList(app);
            if (c.list.empty()) { // the Human Genes, or nothing to wear
                if (c.slot == 3) campMessage(app, sysText(app, 58064));
                menuSound(app, se::kBuzzer);
                return;
            }
            menuSound(app, se::kChoose);
            c.phase = App::Camp::List;
        } else if (c.phase == App::Camp::List && !c.list.empty() && c.slot >= 3) { // a gene plate
            app.game.party[size_t(campCharacter(app, c.member))].genes[c.slot - 3] = item;
            menuSound(app, campui::kSoundEquip);
            c.phase = App::Camp::Slot;
        } else if (c.phase == App::Camp::List && !c.list.empty()) {
            int& worn = app.game.party[size_t(campCharacter(app, c.member))].equip[c.slot];
            if (worn > 0) app.game.items[worn] = std::min(cr::kMaxItemCount, ownedCount(app, worn) + 1);
            if (item > 0 && --app.game.items[item] <= 0) app.game.items.erase(item);
            worn = item;
            menuSound(app, campui::kSoundEquip);
            c.phase = App::Camp::Slot;
        }
        return;
    case App::Camp::Status:
        if (c.phase == App::Camp::Member) { menuSound(app, se::kChoose); c.phase = App::Camp::Slot; }
        return;
    case App::Camp::Genes: menuSound(app, se::kBuzzer); return; // browsing only: plates are worn from Equip
    }
}

void campKey(App& app, SDL_Keycode key) {
    App::Camp& c = app.camp;
    const bool back = key == SDLK_ESCAPE || key == SDLK_BACKSPACE || key == SDLK_AC_BACK || (key == SDLK_K && c.page == App::Camp::Main);
    const bool enter = key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE || key == SDLK_E;
    const bool up = key == SDLK_UP || key == SDLK_W, down = key == SDLK_DOWN || key == SDLK_S;
    const bool left = key == SDLK_LEFT || key == SDLK_A, right = key == SDLK_RIGHT || key == SDLK_D;
    if (back) { campBack(app); return; }
    if (enter) { campChoose(app); return; }
    const int step = up ? -1 : down ? 1 : 0;
    switch (c.page) {
    case App::Camp::Main:
        if (step) { c.cursor = (c.cursor + step + campEntries(app)) % campEntries(app); menuSound(app, se::kCursor); }
        return;
    case App::Camp::Items:
    case App::Camp::Equip:
    case App::Camp::Genes:
        if (c.phase == App::Camp::List) {
            const int n = int(c.list.size());
            if (step && n > 0) {
                c.listCursor = (c.listCursor + step + n) % n;
                c.listTop = std::clamp(c.listTop, std::max(0, c.listCursor - campui::kRows + 1), c.listCursor);
                menuSound(app, se::kCursor);
            } else if ((left || right) && c.page == App::Camp::Items) {
                campNextCategory(app, left ? -1 : 1);
                menuSound(app, se::kPage);
            }
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
            if (step) { c.slot = (c.slot + step + campui::kEquipSlots) % campui::kEquipSlots; menuSound(app, se::kCursor); }
            return;
        }
        [[fallthrough]];
    case App::Camp::Status:
        if (c.phase == App::Camp::Member && (step || left || right)) { c.member ^= 1; menuSound(app, se::kCursor); }
        else if (c.phase == App::Camp::Slot && c.page == App::Camp::Status && (left || right)) { c.member ^= 1; menuSound(app, se::kPage); }
        return;
    }
}

void campClick(App& app, float px, float py) {
    using namespace campui;
    App::Camp& c = app.camp;
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
    if (inAt(cx, kBackX, campBackY(app), 232, 24)) { campBack(app); return; }
    switch (c.page) {
    case App::Camp::Main:
        for (int i = 0; i < campEntries(app); ++i)
            if (in(kPlateX, kPlateY + kPlateStep * float(i), 136, 40)) { c.cursor = i; campChoose(app); return; }
        return;
    default: break;
    }
    if (c.phase == App::Camp::Member || (c.page == App::Camp::Status && c.phase == App::Camp::Slot)) {
        for (int k = 0; k < 2; ++k)
            if (in(kPlateX, kMemberY + kMemberStep * float(k), 136, 40)) { c.member = k; if (c.phase == App::Camp::Member) campChoose(app); return; }
        return;
    }
    if (c.phase == App::Camp::Slot) {
        for (int k = 0; k < 3; ++k)
            if (in(kRowX, kRowY + kRowH * float(k), 256, 44)) { c.slot = k; campChoose(app); return; }
        return;
    }
    if (c.phase == App::Camp::List) {
        if (in(kRowX, kRowY, 256, kRowH * kRows)) {
            const int index = c.listTop + int((my - kRowY) / kRowH);
            if (index < int(c.list.size())) { c.listCursor = index; campChoose(app); }
        } else if (c.page == App::Camp::Items && inAt(lx0, 0, 40, 180, 40))
            campNextCategory(app, 1), menuSound(app, se::kPage);
        return;
    }
    if (c.phase == App::Camp::Ask) {
        const int item = c.list.empty() ? 0 : c.list[size_t(c.listCursor)];
        if (in(336, 60, 36, 31) && item >= 3 && item <= 6) campKey(app, SDLK_LEFT);
        else if (in(436, 60, 36, 31) && item >= 3 && item <= 6) campKey(app, SDLK_RIGHT);
        else if (in(336, 104, 136, 40)) { c.confirm = 0; campChoose(app); }
        else if (in(336, 160, 136, 40)) { c.confirm = 1; campChoose(app); }
    }
}

// A list row (ComUty_CampItemPlate): plate 8 / 9, the icon, the name, the count at (222, 30)
void drawCampRow(App& app, cr::MenuPainter& m, float x, float y, int item, int count, bool lit, float a,
                 const std::string& label = std::string()) {
    m.picture(lit ? 9 : 8, x, y, a);
    if (const cr::ItemParam* p = app.itemTable.item(item); p && p->icon >= 0)
        m.piece(x + 12, y + 8, float(p->icon % 16) * 32, 248 + float(p->icon / 16) * 32, 32, 32, a);
    m.text(item > 0 ? itemName(app, item) : std::string("-"), x + 56, y + 12, false, a, 14);
    if (!label.empty()) m.text(label, x + 56, y + 30, false, a * 0.8f, 11);
    if (count > 0) drawShopNumber(m, count, x + 222 - 10, y + 30, 2, 10, a, 2);
}

// A gene plate's row (ComUty_ComPlate_Equip_GenePlate): plate 8 / 9, its icon and name - yellow
// for one the member wears, grey while none of it is known - then its six entries as pips at
// (167 + 14k, 30), the unknown ones first and dim (10, known 11), or "MASTER" (picture 96 at
// (176, 24)) once all are known
void drawGeneRow(App& app, cr::MenuPainter& m, float x, float y, int plate, bool lit, bool worn, float a) {
    m.picture(lit ? 9 : 8, x, y, a);
    if (const cr::ItemParam* p = app.itemTable.item(plate); p && p->icon >= 0)
        m.piece(x + 12, y + 8, float(p->icon % 16) * 32, 248 + float(p->icon / 16) * 32, 32, 32, a);
    const int known = cr::plateLearned(app.game, plate);
    int total = 0;
    for (int e : cr::plateContents(plate)) total += e > 0;
    const std::string name = itemName(app, plate);
    if (worn) m.textTint(name, x + 56, y + 12, 1, 1, 0.25f, a, 14);
    else if (known == 0) m.textTint(name, x + 56, y + 12, 0.6f, 0.6f, 0.6f, a, 14);
    else m.text(name, x + 56, y + 12, false, a, 14);
    if (total <= 0) return;
    if (known >= total) m.picture(96, x + 176, y + 24, a);
    else
        for (int k = 0; k < total; ++k) m.picture(k < total - known ? 10 : 11, x + 251 - 14 * float(total) + 14 * float(k), y + 30, a);
}

void drawCampScrollBar(cr::MenuPainter& m, int top, int n, float a) {
    using namespace campui;
    if (n <= kRows) return;
    const float sx = 462, h = kRowH * kRows;
    const float dark[4] = {0.05f, 0.05f, 0.08f, 0.8f * a};
    m.quad(sx, kRowY, 8, h, dark, dark);
    m.picture(76, sx - 4, kRowY - 14, a);
    m.picture(77, sx - 4, kRowY + h - 2, a);
    m.picture(106, sx, kRowY + (h - 40) * float(top) / float(n - kRows), a);
}

// ComUty_GeneBoardDraw at (x, y): the plate's name, its picture (gene_m*c.mct), "n / 6" known, then
// its abilities and skills (unknown ones as ???)
void drawGeneBoard(App& app, cr::MenuPainter& m, float x, float y, int plate, float a) {
    if (!cr::isPlate(plate)) return;
    const float dark[4] = {0, 0, 0, 0.6f * a}, white[4] = {1, 1, 1, 0.85f * a}, clear[4] = {1, 1, 1, 0};
    m.quad(x, y, 163, 236, dark, dark);
    const std::string name = itemName(app, plate);
    m.textCentred(name, x + (163 - m.textWidth(name, 14)) / 2, y + 10, false, a, 14);
    m.quad(x, y + 24, 163, 16, white, clear);
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
    if (art.id) { // the picture fitted into 76 x 76 under the bar
        const float w = float(art.width), h = float(art.height), k = std::min(76 / std::max(w, 1.0f), 76 / std::max(h, 1.0f));
        m.image(art, x + (163 - w * k) / 2, y + 44, w * k, h * k, a);
    }
    float ly = y + 126;
    for (int e : cr::plateContents(plate)) {
        if (e <= 0) continue;
        const bool known = ownedCount(app, e) > 0;
        m.text(known ? itemName(app, e) : std::string("???"), x + 12, ly, false, known ? a : a * 0.5f, 12);
        m.text(cr::isSkill(e) ? "Skill" : "Gene", x + 124, ly + 1, false, a * 0.55f, 10);
        ly += 18;
    }
}

// menuf_mainPut in a puzzle room (G+0x35526 == 1), in tex2d units from the anchor (0, 0): a dark
// panel (0, 12)-(80, 48) (0xb8 black), the infinity mark (792, 58, 72 x 28) at (4, 16) when the
// count is unlimited, else "Left" (484, 184, 30 x 12) at (4, 24) and cmd_number_puzzle's two digits
// ((512 + 28 d, 58), 28 x 28, from x 24 - 44 for one - by 20, y 16; swelling 1.4 .. 1.1 for the 4
// frames after a change); with the help state on, the retry button (420, 184, 44 x 30) at (8, 56)
// - blinking (5 frames of 11) at 0 once the count was above it - and Help (984, 64, 40 x 32) at
// (84, 14); the punishment mark (448, 120, 64 x 64) blinking over a refused touch
void drawPuzzleHud(App& app, float a) {
    cr::MenuPainter& m = app.menu;
    const App::Puzzle& pz = app.puzzle;
    auto piece = [&](float x, float y, float u, float v, float w, float h, float scale = 1) {
        m.pieceTo(x + 16 + w * (1 - scale) / 2, y + 32 + h * (1 - scale) / 2, w * scale, h * scale, u, v, w, h, a);
    };
    const float dark[4] = {0, 0, 0, float(0xb8) / 255.0f * a};
    app.renderer.setOrigin(wideLeft(app)); // (widescreen: the panel in the screen's corner)
    m.quad(16, 12 + 32, 80, 36, dark, dark);
    const int esp = pz.esp < 0 ? -1 : std::max(0, pz.esp - (pz.pkState == 2 && pz.pkMoved));
    if (esp < 0) piece(4, 16, 792, 58, 72, 28);
    else {
        static const float kSwell[5] = {1.0f, 1.1f, 1.2f, 1.3f, 1.4f};
        const char d[3] = {char('0' + esp / 10 % 10), char('0' + esp % 10), 0};
        const float scale = kSwell[std::clamp(pz.pop, 0, 4)];
        float x = esp < 10 ? 44 : 24;
        for (int k = esp < 10 ? 1 : 0; k < 2; ++k, x += 20) piece(x, 16, 512 + 28.0f * float(d[k] - '0'), 58, 28, 28, scale);
        piece(4, 24, 484, 184, 30, 12);
    }
    if (pz.help) {
        if (pz.blink < 5) piece(8, 56, 420, 184, 44, 30);
        piece(84, 14, 984, 64, 40, 32);
    }
    app.renderer.setOrigin(app.renderer.uiX()); // (the mark: where the field was touched)
    if (pz.punished > 0 && pz.punishPhase == 0) piece(pz.punishX, pz.punishY, 448, 120, 64, 64);
}

// menuf_mainPut's counters, a 30 Hz frame: the count's swell (set as it changes:
// SetFldMenu_puzzle_count), the retry button's blink at 0, the punishment mark's
void stepPuzzleHud(App& app) {
    App::Puzzle& pz = app.puzzle;
    if (pz.stage < 0) return;
    const int esp = pz.esp < 0 ? -1 : std::max(0, pz.esp - (pz.pkState == 2 && pz.pkMoved));
    if (esp != pz.shownEsp) {
        if (pz.shownEsp != -1000) pz.pop = 4;
        pz.shownEsp = esp;
    }
    if (pz.pop > 0) --pz.pop;
    if (esp > 0) pz.hadEsp = true;
    if (esp == 0 && pz.hadEsp) pz.blink = pz.blink + 1 < 11 ? pz.blink + 1 : 0;
    else pz.blink = 0;
    if (pz.punished > 0) {
        pz.punishPhase = (pz.punishPhase + 1) % 3;
        --pz.punished;
    }
}

// Uty_PuzzleStart_Put: (516, 264) 224 x 64 at (x1, y1), and (744, 264) - flag 1: (768, 900) - at
// (x2, y2), white at the banner's alpha, in front
void drawPuzzleBanner(App& app) {
    const App::PuzzleBanner& b = app.puzzleBanner;
    if (!b.on || b.alpha <= 0 || !app.windowTex.id) return;
    cr::Renderer& r = app.renderer;
    cr::MenuPainter& m = app.menu;
    m.begin(r, app.windowTex, &app.font);
    r.setOrigin(r.uiX());
    const float a = float(b.alpha) / 255.0f;
    m.piece(b.x1 + 16, b.y1 + 32, 516, 264, 224, 64, a);
    const bool other = app.game.flag(1);
    m.piece(b.x2 + 16, b.y2 + 32, other ? 768 : 744, other ? 900 : 264, 224, 64, a);
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
namespace staffroll {
constexpr float kLine = 2.826087f, kPic = 0.22217391f, kPicFast = 2.2217391f, kPicEnd = -1022, kPanelEnd = -280;
constexpr int kLines = 336;
// 2.2.0's (tex2d units): 17 lines 20 apart from y 320 (x 16), rising 1.9217 a frame, the next at
// 320 past -20; the picture 0.125 (1.25) a frame up to -575; its block 9 has 434 lines
constexpr float kPhoneLine = 1.92174f, kPhonePic = 0.125f, kPhonePicFast = 1.25f, kPhonePicEnd = -575;
constexpr int kPhoneShown = 17, kPhoneLines = 434;
}
int staffLines(const App& app) { // the scripts': 2.2.0's and the Vita's (voiced: sys.evt's +0x40) or 1.0.1's
    return app.phoneData || !app.battleVoiceTable.empty() ? staffroll::kPhoneLines : staffroll::kLines;
}
float staffPicEnd(const App::StaffRoll& st) { return st.phone ? staffroll::kPhonePicEnd : staffroll::kPicEnd; }

void staffWin(App& app, int w, bool on) { app.staffRoll.winTo[w] = on ? 1.0f : 0.0f; }

void openStaffRoll(App& app, int mode) {
    App::StaffRoll& st = app.staffRoll;
    app.renderer.destroy(st.tex);
    st = App::StaffRoll();
    st.open = true;
    st.mode = std::clamp(mode, 0, 2);
    st.phone = app.renderer.phoneLayout();
    const int shown = st.phone ? staffroll::kPhoneShown : 21;
    st.next = shown;
    for (int i = 0; i < shown; ++i)
        st.lines[i] = st.phone ? App::StaffRoll::Line{true, 320.0f + 20.0f * float(i), i} : App::StaffRoll::Line{true, 800.0f + 40.0f * float(i), i};
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
            const int total = staffLines(app), shown = st.phone ? kPhoneShown : 21;
            if (st.next <= total) {
                const int steps = st.fast ? 10 : 1;
                for (int i = 0; i < shown; ++i) {
                    auto& l = st.lines[i];
                    for (int k = 0; k < steps; ++k) {
                        l.y -= st.phone ? kPhoneLine : kLine;
                        if (l.y > (st.phone ? -20 : -40)) continue;
                        const int e = st.next++;
                        l.y = st.phone ? 320 : 800;
                        l.on = e < total && staffLine(app, e).size() >= 3;
                        l.entry = e;
                    }
                }
            }
            const float end = staffPicEnd(st);
            st.picY = std::max(end, st.picY - (st.phone ? (st.fast ? kPhonePicFast : kPhonePic) : (st.fast ? kPicFast : kPic)));
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
            r.drawQuadUV(st.tex, 32, 64 + 2 * st.picY, 1024, 1790, 0, 0, 1024 * k, 1790 * k, 1, 1, 1, st.win[0]);
        }
        if (st.win[1] > 0) r.fillRect(32 + 2 * st.panel, 64, 560, 640, 0, 0, 0, st.win[1]);
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
            if (st.phone) { if (l.y > -20 && l.y < 320) m.text(staffLine(app, l.entry), 16 + 16, l.y + 32, false, st.win[3], 13); }
            else if (l.y > -40 && l.y < 800) m.text(staffLine(app, l.entry), 16, l.y / 2, false, st.win[3], 13);
        }
    if (st.win[2] > 0 && app.windowTex.id) // the fast button (2.2.0: (444, 0))
        m.picture(114, (st.phone ? 444 : 460) + 16, (st.phone ? 0 : -32) + 32, st.win[2]);
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
    for (auto& m : app.game.party) m.hp = m.mp = -1; // pcparam_hpmp_all_recovery
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

void lastBattleAnswer(App& app, bool yes) {
    App::LastBattle& lb = app.lastBattle;
    if (lb.state != 10) return;
    menuSound(app, se::kChoose);
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
        m.textCentred(sysText(app, 58000 + 0x68 + lb.count), dx + 12, 47, false, lb.win[1], 15);
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
            m.text(lastBattlePlate(app, lb.pairs[i]), x + 56, y + 12, false, a, 14);
        }
    r.setOrigin(r.uiX());
    if (lb.win[5] > 0) { // the question (ComUty_MsgBoardDraw at y 220, sliding up from 160 below)
        const float dy = 160 * (1 - ease(lb.win[5]));
        m.picture(12, 0, 220 + dy, lb.win[5]);
        m.text(sysText(app, 57000 + 0x13 + lb.count, sysText(app, 58029 + 2 * std::max(0, lb.chosen))), 24, 234 + dy,
               false, lb.win[5], 15);
    }
    if (lb.win[6] > 0) { // Yes / No (from 360 to the right)
        const float dx = 360 * (1 - ease(lb.win[6]));
        r.setOrigin(wideRight(app));
        m.plate(336 + dx, 104, 0, 0, lb.confirm == 0, lb.win[6], false);
        m.plate(336 + dx, 160, 1, 1, lb.confirm == 1, lb.win[6], false);
    }
    r.setOrigin(r.uiX());
}
void lastBattleKey(App& app, SDL_Keycode key) {
    App::LastBattle& lb = app.lastBattle;
    if (lb.state == 3) {
        const int n = int(lb.pairs.size());
        if (n <= 0) return;
        if (key == SDLK_UP || key == SDLK_W) lb.cursor = (lb.cursor + n - 1) % n, menuSound(app, se::kCursor);
        else if (key == SDLK_DOWN || key == SDLK_S) lb.cursor = (lb.cursor + 1) % n, menuSound(app, se::kCursor);
        else if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE || key == SDLK_E) lastBattleChoose(app, lb.cursor);
    } else if (lb.state == 10) {
        if (key == SDLK_UP || key == SDLK_DOWN || key == SDLK_W || key == SDLK_S) lb.confirm ^= 1, menuSound(app, se::kCursor);
        else if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE || key == SDLK_E) lastBattleAnswer(app, lb.confirm == 0);
        else if (key == SDLK_ESCAPE || key == SDLK_BACKSPACE) lastBattleAnswer(app, false);
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
void scrollBar(cr::MenuPainter& m, float dx, int top, int n, float a) {
    if (n <= kRows) return;
    const float sx = 462 + dx, y = kListY, h = kListStep * kRows;
    const float dark[4] = {0.05f, 0.05f, 0.08f, 0.8f * a};
    m.quad(sx, y, 8, h, dark, dark);
    m.picture(76, sx - 4, y - 14, a);
    m.picture(77, sx - 4, y + h - 2, a);
    m.picture(106, sx, y + (h - 40) * float(top) / float(n - kRows), a);
}
void levelList(App& app, float from) {
    App::WorldSelect& ws = app.worldSelect;
    on(app, 3, -140, 0);
    on(app, 7, from, 0);
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
    app.game.setFlag(0, ws.cancelled);
    if (ws.cancelled) return;
    app.game.vars[0] = ws.world;
    app.game.vars[1] = ws.world == 5 ? 0 : ws.level;
    app.game.vars[2] = ws.pair;
    app.game.setFlag(1, ws.skip && ws.world != 4 && ws.world != 5);
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
        levelList(app, 480);
}

void worldSelectLevel(App& app, int n) { // state 6: a band, or Back (-1)
    using namespace wldsel;
    App::WorldSelect& ws = app.worldSelect;
    if (ws.state != 6) return;
    if (n < 0) {
        menuSound(app, se::kBack);
        off(app, 3, 0, -140), off(app, 7, 480, 0), on(app, 6, -480, 0);
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

void worldSelectAnswer(App& app, bool yes) { // states 4 (skip?) and 8 (okay?)
    using namespace wldsel;
    App::WorldSelect& ws = app.worldSelect;
    if (ws.state != 4 && ws.state != 8) return;
    menuSound(app, se::kChoose);
    if (ws.state == 4) { // ComUty_YesNoChoiceProc: 2 the first box (Yes)
        off(app, 8, -360, 0), off(app, 9, 0, 160), off(app, 10, 0, 160);
        on(app, 4), on(app, 5);
        ws.skip = yes;
        levelList(app, 480);
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

void stepWorldSelect(App& app) {
    App::WorldSelect& ws = app.worldSelect;
    if (!app.worldChoiceOpen) return;
    for (int w = 0; w < 11; ++w)
        ws.win[w] = ws.win[w] < ws.winTo[w] ? std::min(ws.winTo[w], ws.win[w] + 0.25f) : std::max(ws.winTo[w], ws.win[w] - 0.25f);
    switch (ws.state) {
    case 3: if (--ws.timer < 0) ws.state = 4; break;
    case 7: if (--ws.timer < 0) ws.state = 8; break;
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
        m.textCentred(sysText(app, 58000), dx(2) + 12 + ox, dy(2) + 47 + oy, false, ws.win[2], 15);
    }
    if (ws.win[3] > 0) {
        m.picture(16, dx(3) + 24 + ox, dy(3) + 72 + oy, ws.win[3]);
        m.textCentred(sysText(app, 58001), dx(3) + 40 + ox, dy(3) + 65 + oy, false, ws.win[3], 15);
    }
    auto plate = [&](float x, float y, bool chosen, const std::string& name, int pips, float a) {
        m.picture(chosen ? 9 : 8, x, y, a);
        m.text(name, x + 56, y + 12, false, a, 14);
        if (pips >= 0)
            for (int k = 0; k < 4; ++k) m.picture(pips & (1 << k) ? 11 : 10, x + 195 + 14 * float(k), y + 30, a);
    };
    r.setOrigin(wideRight(app));
    if (ws.win[6] > 0) { // the gates
        ownPlace(true);
        ws.top = std::clamp(ws.top, std::max(0, ws.cursor - kRows + 1), ws.cursor);
        for (int i = ws.top; i < std::min(int(ws.worlds.size()), ws.top + kRows); ++i) {
            const int g = ws.worlds[size_t(i)];
            int pips = -1;
            if (g < 5) {
                pips = 0;
                for (int k = 0; k < 4; ++k) pips |= app.game.flag(kPipFlag[g][k][ws.pair]) ? 1 << k : 0;
            }
            const float x = kListX + dx(6), y = kListY + kListStep * float(i - ws.top);
            plate(x, y, ws.state == 1 && i == ws.cursor, sysText(app, 58002 + g), pips, ws.win[6]);
            m.piece(x + 12, y + 8, float(0x3d % 16) * 32, 248 + float(0x3d / 16) * 32, 32, 32, ws.win[6]); // icon 0x3d
        }
        scrollBar(m, dx(6), ws.top, int(ws.worlds.size()), ws.win[6]);
        ownPlace(false);
        m.backBar(kBackX + dx(6), kBackY, false, ws.win[6]);
    }
    if (ws.win[7] > 0) { // the level bands
        ownPlace(true);
        ws.levelTop = std::clamp(ws.levelTop, std::max(0, ws.levelCursor - kRows + 1), ws.levelCursor);
        for (int i = ws.levelTop; i < std::min(ws.levels, ws.levelTop + kRows); ++i)
        {
            const float x = kListX + dx(7), y = kListY + kListStep * float(i - ws.levelTop);
            plate(x, y, ws.state == 6 && i == ws.levelCursor, sysText(app, 58008 + i), -1, ws.win[7]);
            m.piece(x + 12, y + 8, float(0x3d % 16) * 32, 248 + float(0x3d / 16) * 32, 32, 32, ws.win[7]);
        }
        scrollBar(m, dx(7), ws.levelTop, ws.levels, ws.win[7]);
        ownPlace(false);
        m.backBar(kBackX + dx(7), kBackY, false, ws.win[7]);
    }
    r.setOrigin(r.uiX());
    if (ws.win[9] > 0) { // the board (ComUty_MsgBoardDraw at y 220, from 160 below)
        const float a = ws.win[9], y = 220 + oy + dy(9);
        m.picture(12, ox, y, a);
        std::vector<std::string> lines;
        if (ws.asking) lines = {sysText(app, 57000 + 0x40)};
        else if (ws.world == 5) lines = {sysText(app, 58002 + ws.world), sysText(app, 58018)};
        else lines = {sysText(app, 58002 + ws.world), sysText(app, 58008 + ws.level), sysText(app, 58018)};
        for (size_t k = 0; k < lines.size(); ++k) m.text(lines[k], 24 + ox, y + 14 + 16 * float(k), false, a, 15);
    }
    r.setOrigin(wideRight(app));
    if (ws.win[8] > 0) { // Yes / No
        ownPlace(true);
        m.plate(336 + dx(8), 104, 0, 0, ws.confirm == 0, ws.win[8], false);
        m.plate(336 + dx(8), 160, 1, 1, ws.confirm == 1, ws.win[8], false);
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
        if ((up || down) && n > 0) ws.cursor = (ws.cursor + (up ? n - 1 : 1)) % n, menuSound(app, se::kCursor);
        else if (enter) worldSelectChoose(app, ws.cursor);
        else if (back) worldSelectChoose(app, -1);
    } else if (ws.state == 6) {
        if (up || down) ws.levelCursor = (ws.levelCursor + (up ? ws.levels - 1 : 1)) % ws.levels, menuSound(app, se::kCursor);
        else if (enter) worldSelectLevel(app, ws.levelCursor);
        else if (back) worldSelectLevel(app, -1);
    } else if (ws.state == 4 || ws.state == 8) {
        if (up || down || key == SDLK_LEFT || key == SDLK_RIGHT) ws.confirm ^= 1, menuSound(app, se::kCursor);
        else if (enter) worldSelectAnswer(app, ws.confirm == 0);
        else if (back) worldSelectAnswer(app, false);
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
        const int top = ws.state == 1 ? ws.top : ws.levelTop, n = ws.state == 1 ? int(ws.worlds.size()) : ws.levels;
        for (int i = top; i < std::min(n, top + kRows); ++i)
            if (in(kListX, kListY + kListStep * float(i - top), 256, 48)) {
                if (ws.state == 1) ws.cursor = i, worldSelectChoose(app, i);
                else ws.levelCursor = i, worldSelectLevel(app, i);
                return;
            }
    } else if (ws.state == 4 || ws.state == 8) {
        if (in(336, 104, 136, 40)) worldSelectAnswer(app, true);
        else if (in(336, 160, 136, 40)) worldSelectAnswer(app, false);
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
// goes on. Its pictures are full screens: sqex / mve / mclogo 1024 x 768, warning 960 x 544 (the
// Vita's own), drawn to fit
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
        // the picture's own size: 1024 x 768 (or 960 x 544 for the warning) of its sheet
        const float pw = t.width >= 1024 ? 1024.0f : 960.0f, ph = t.width >= 1024 ? 768.0f : 544.0f;
        const float k = std::min(r.screenW() / pw, float(cr::Renderer::kLogicalH) / ph), w = pw * k, h = ph * k;
        const float su = float(t.width) / (t.width >= 1024 ? 1024.0f : 960.0f); // (the sheet may be larger)
        r.drawQuadUV(t, (r.screenW() - w) / 2, (cr::Renderer::kLogicalH - h) / 2, w, h, 0, 0, pw * su, ph * su, 1, 1, 1, a);
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
    if (t.g[5] == 480 && t.g[6] == 96)
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
    if (--t.timer < 0 && t.page >= 0 && t.page < 4) {
        const int n = t.frames[t.page];
        t.timer = ++t.frame == n - 1 ? 40 : 20;
        if (t.frame >= n) t.frame = 0;
    }
}

// MsgUty_TexDispDraw: while flag 3 is set, the page's frame (frame x 384, row x 256: 384 x 256 of
// the 2048-wide sheet) at tex2d (2x, 2y) of the HD frame - the screen's (2x + 32, 2y + 64)
void drawTutoTex(App& app) {
    const App::TutoTex& t = app.tutoTex;
    if (!t.on || !t.tex.id || !app.game.flag(3) || t.page < 0 || t.page >= 4 || t.frames[t.page] < 0) return;
    cr::Renderer& r = app.renderer;
    const float ox = r.uiX(), k = float(t.tex.width) / 2048.0f;
    r.setOrigin(0);
    r.drawQuadUV(t.tex, ox + 2 * t.x + 32, 2 * t.y + 64, 384, 256, 384.0f * float(t.frame) * k, 256.0f * float(t.row[t.page]) * k,
                 384 * k, 256 * k);
    r.setOrigin(ox);
}

// Uty_DrawController_Put: the cross key (1864, 1584, 144 x 144 of window_hd's pixels) at (64, 384)
// and its centre (1864, 1448, 96 x 96) at (88, 408) of the HD frame - the screen's (x + 32, y + 64)
// - until the script clears flag 0
void drawPadArt(App& app) {
    if (!app.padArt) return;
    if (!app.game.flag(0)) { app.padArt = false; return; }
    if (!app.windowTex.id || app.renderer.phoneLayout()) return; // (2.2.0's Uty_DrawController_Put draws nothing)
    cr::Renderer& r = app.renderer;
    cr::MenuPainter& m = app.menu;
    m.begin(r, app.windowTex, &app.font);
    r.setOrigin(r.uiX());
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

// ---- The field menu (menuf_mainPut / menuf_mainPrc) ----
// A button in the top right corner opens a bar: Camp, Save, Map (maps below 64) and, once skill
// 170 is known (owned - it needn't be worn), the switch for random encounters (this pair's
// unencountflg; flag 4076 refuses it). Laid out in tex2d coordinates; menu units are those +
// (16, 32). The buttons slide 4 frames between the corner (460, -28) and their places.
namespace fieldui {
constexpr float kCorner[2] = {460, -28};
constexpr float kPlace[4][2] = {{432, 32}, {384, -24}, {312, -24}, {240, -24}}; // Camp, Save, Map, encounters
constexpr float kSize[4] = {56, 40, 40, 40};
// window_hd pieces: the corner button closed / open, then the buttons (encounters on / off)
constexpr float kCornerPiece[2][2] = {{432, 992}, {464, 992}};
constexpr float kPiece[4][2] = {{452, 516}, {468, 576}, {512, 388}, {468, 620}};
constexpr float kEncountersOff[2] = {468, 664};
constexpr int kEncounterSkill = 170;
} // namespace fieldui

int fieldRoute(const App& app) { return std::clamp(app.playerChar / 2, 0, 3); }

// 2.2.0's iPhone layout: the bar keeps to the screen's corner, (-16, +32) (its menuf_mainPut /
// Prc and kganMfMainButtonPosTbl)
float fieldDx(const App& app) { return app.renderer.phoneLayout() ? -16.0f : 0.0f; }
float fieldDy(const App& app) { return app.renderer.phoneLayout() ? 32.0f : 0.0f; }

bool fieldMenuWanted(const App& app) { // the field, the player free to walk
    return app.mode == Mode::Maps && app.havePlayer && !app.titleOpen && !app.booting && !app.battle.active && !app.campOpen &&
           !app.shop.open && !app.saveScreen.open && !app.optionsOpen && !app.resumePrompt && !app.mapScreen.open;
}

bool fieldSceneRunning(const App& app) { // G+0x2c0 bits 0 / 3: a script or a message holds the field
    return app.runner.running() || !app.dialogue.empty() || app.itemWindowOpen || app.choiceFlag >= 0 || app.worldChoiceOpen;
}

bool fieldEncountersOff(const App& app) { return app.game.noEncounters[size_t(fieldRoute(app))] || app.game.flag(4076); }

void stepFieldMenu(App& app) {
    App::FieldMenu& f = app.fieldMenu;
    f.fade = fieldSceneRunning(app) ? std::max(0, f.fade - 32) : std::min(320, f.fade + 32);
    if (f.anim > 0) --f.anim;
}

// A button's top-left this frame (tex2d coordinates)
void fieldButtonPos(const App& app, int k, float& x, float& y) {
    using namespace fieldui;
    const App::FieldMenu& f = app.fieldMenu;
    const float t = float(4 - f.anim) / 4; // how far along this slide is
    const float from[2] = {f.open ? kCorner[0] : kPlace[k][0], f.open ? kCorner[1] : kPlace[k][1]};
    const float to[2] = {f.open ? kPlace[k][0] : kCorner[0], f.open ? kPlace[k][1] : kCorner[1]};
    x = from[0] + (to[0] - from[0]) * t + fieldDx(app);
    y = from[1] + (to[1] - from[1]) * t + fieldDy(app);
}

bool fieldButtonShown(const App& app, int k) {
    if (k == 2) return app.mapNumber < 64;
    if (k == 3) return app.game.items.count(fieldui::kEncounterSkill) && app.game.items.at(fieldui::kEncounterSkill) > 0;
    return true;
}

void drawFieldMenu(App& app) {
    using namespace fieldui;
    const App::FieldMenu& f = app.fieldMenu;
    if (!app.windowTex.id || f.fade <= 0) return;
    cr::Renderer& r = app.renderer;
    cr::MenuPainter& m = app.menu;
    m.begin(r, app.windowTex, &app.font);
    r.setOrigin(r.uiX());
    const float a = float(std::min(255, f.fade)) / 255.0f;
    if (app.puzzle.stage >= 0) drawPuzzleHud(app, a);
    r.setOrigin(wideRight(app)); // (widescreen: in the screen's corner)
    const bool bar = f.open || f.anim > 0;
    m.piece(460 + 16 + fieldDx(app), -30 + 32 + fieldDy(app), kCornerPiece[bar][0], kCornerPiece[bar][1], 32, 32, a);
    if (!bar) { r.setOrigin(r.uiX()); return; }
    for (int k = 0; k < 4; ++k) {
        if (!fieldButtonShown(app, k)) continue;
        float x, y;
        fieldButtonPos(app, k, x, y);
        const float* src = k == 3 && fieldEncountersOff(app) ? kEncountersOff : kPiece[k];
        m.piece(x + 16, y + 32, src[0], src[1], kSize[k], kSize[k], a);
    }
    r.setOrigin(r.uiX());
}

void toggleFieldMenu(App& app) {
    App::FieldMenu& f = app.fieldMenu;
    if (f.anim > 0) return;
    f.open = !f.open;
    f.anim = 4;
    menuSound(app, f.open ? se::kCampOpen : se::kCampClose);
}

// The bar's buttons (the bar open and still): 0 Camp, 1 Save, 2 Map, 3 encounters
void pressFieldButton(App& app, int k) {
    if (!app.fieldMenu.open || app.fieldMenu.anim > 0 || !fieldButtonShown(app, k)) return;
    switch (k) {
    case 0:
        menuSound(app, se::kCampOpen);
        openCamp(app);
        break;
    case 1:
        menuSound(app, se::kChoose);
        app.game.vars[0] = 0;
        openSaveScreen(app, false);
        break;
    case 2: // SetMapMenu
        menuSound(app, se::kChoose);
        openMapScreen(app);
        break;
    case 3: { // the original's sounds: 3 switching off, 2 on, the buzzer while flag 4076 holds it
        if (app.game.flag(4076)) {
            menuSound(app, se::kBuzzer);
            break;
        }
        bool& off = app.game.noEncounters[size_t(fieldRoute(app))];
        off = !off;
        menuSound(app, off ? se::kBack : se::kChoose);
        break;
    }
    default: break;
    }
}

// A click in the frame's pixels; true when the field menu took it
bool fieldMenuClick(App& app, float px, float py) {
    if (!fieldMenuWanted(app) || fieldSceneRunning(app) || app.fieldMenu.fade <= 0) return false;
    float lx, ly;
    app.renderer.toLogical(px, py, lx, ly);
    const float tx = (lx - wideRight(app)) / 2 - 16 - fieldDx(app), ty = ly / 2 - 32 - fieldDy(app); // tex2d
    if (tx >= 456 && tx < 496 && ty >= -32 && ty < 8) { // _pad_chk_trg_down_box(456, -32, 40, 40) (2.2.0: 440, 0)
        toggleFieldMenu(app);
        return true;
    }
    if (!app.fieldMenu.open || app.fieldMenu.anim > 0) return false;
    static const float kCentre[4][3] = {{460, 60, 28}, {404, -4, 20}, {332, -4, 20}, {260, -4, 20}};
    for (int k = 0; k < 4; ++k) {
        const float dx = tx - kCentre[k][0], dy = ty - kCentre[k][1];
        if (fieldButtonShown(app, k) && dx * dx + dy * dy <= kCentre[k][2] * kCentre[k][2]) {
            pressFieldButton(app, k);
            return true;
        }
    }
    return false;
}

// ---- The map screen (__menuu_MapSet / MapPrc / MapPut, ReadMapMenu) ----
// The gate's map_D0n (slot 0x30, 512 x 512; MuTex group 1 pictures 13 the map 480 x 320 at
// (0, 0), 14 the pair's marker 48 x 48 at (0, 320), 15 a marker 16 x 16 at (0, 368)), the pair's
// marker on this field's spot (blinking, 2 frames of 6, the next 48 to the right) and the two
// markers of the story's step: the first of the pair's 25 flags (2601 + 50 x pair) set. A tap
// anywhere closes it. Coordinates are tex2d; menu units are + (16, 32).
namespace mapui {
// 0xc7b20: each field's spot on its gate's map (s16 x, y; -1 the 3D rooms)
constexpr short kMapSpot[64][2] = {
    {72, 216}, {122, 196}, {82, 172}, {176, 152}, {132, 152}, {188, 108}, {327, 218}, {288, 196},
    {426, 180}, {338, 176}, {386, 155}, {350, 132}, {360, 88}, {-1, -1}, {100, 204}, {144, 204},
    {110, 160}, {66, 160}, {154, 160}, {166, 116}, {284, 228}, {334, 206}, {294, 184}, {308, 140},
    {262, 140}, {220, 140}, {354, 120}, {402, 120}, {412, 74}, {-1, -1}, {80, 176}, {124, 176},
    {168, 176}, {38, 176}, {92, 132}, {330, 200}, {292, 180}, {254, 156}, {264, 112}, {304, 136},
    {380, 180}, {430, 156}, {418, 200}, {392, 136}, {352, 112}, {364, 68}, {-1, -1}, {116, 186},
    {82, 232}, {160, 188}, {127, 144}, {172, 144}, {322, 212}, {278, 212}, {234, 212}, {334, 168},
    {288, 168}, {244, 168}, {278, 124}, {430, 132}, {442, 86}, {344, 124}, {356, 78}, {-1, -1},
};
// 0xc7c30: per gate, per story step (the first of the pair's 25 flags set), two markers (x, y, kind)
constexpr short kMapMark[4][25][2][3] = {
    {
        {{68, 248, 1}, {-1, -1, 0}},
        {{75, 224, 0}, {80, 204, 1}},
        {{87, 224, 0}, {107, 224, 0}},
        {{92, 204, 0}, {112, 204, 0}},
        {{51, 192, 0}, {71, 192, 0}},
        {{133, 216, 0}, {153, 216, 0}},
        {{124, 204, 0}, {129, 184, 1}},
        {{168, 204, 1}, {173, 184, 0}},
        {{145, 172, 0}, {165, 172, 0}},
        {{180, 160, 0}, {185, 140, 1}},
        {{192, 116, 0}, {323, 252, 1}},
        {{297, 228, 1}, {317, 228, 0}},
        {{335, 252, 1}, {422, 212, 1}},
        {{257, 216, 0}, {277, 216, 0}},
        {{303, 204, 0}, {323, 204, 1}},
        {{290, 204, 0}, {338, 150, 0}},
        {{347, 208, 1}, {367, 208, 0}},
        {{353, 184, 0}, {373, 184, 1}},
        {{341, 184, 0}, {347, 164, 1}},
        {{417, 188, 0}, {397, 188, 1}},
        {{428, 188, 0}, {365, 140, 0}},
        {{353, 140, 0}, {358, 120, 1}},
        {{265, 96, 0}, {-1, -1, 0}},
        {{358, 163, 0}, {378, 163, 1}},
        {{-1, -1, -1}, {-1, -1, -1}},
    },
    {
        {{95, 236, 1}, {-1, -1, 0}},
        {{107, 236, 1}, {127, 236, 1}},
        {{113, 212, 0}, {133, 212, 0}},
        {{100, 212, 0}, {106, 192, 1}},
        {{139, 236, 1}, {134, 256, 0}},
        {{76, 191, 1}, {96, 191, 1}},
        {{82, 167, 0}, {102, 167, 0}},
        {{122, 180, 0}, {142, 180, 0}},
        {{69, 168, 0}, {74, 148, 1}},
        {{157, 168, 0}, {162, 148, 1}},
        {{169, 124, 0}, {280, 260, 1}},
        {{299, 236, 0}, {319, 236, 1}},
        {{287, 236, 0}, {292, 216, 1}},
        {{311, 192, 0}, {331, 192, 1}},
        {{299, 192, 0}, {304, 172, 1}},
        {{295, 160, 0}, {275, 160, 0}},
        {{248, 172, 1}, {228, 172, 1}},
        {{234, 148, 0}, {254, 148, 0}},
        {{215, 171, 1}, {210, 191, 0}},
        {{322, 148, 0}, {342, 148, 1}},
        {{369, 140, 0}, {389, 140, 0}},
        {{404, 128, 0}, {409, 108, 1}},
        {{416, 84, 0}, {-1, -1, 0}},
        {{-1, -1, -1}, {-1, -1, -1}},
        {{-1, -1, -1}, {-1, -1, -1}},
    },
    {
        {{77, 208, 1}, {-1, -1, 0}},
        {{88, 208, 1}, {108, 208, 1}},
        {{94, 184, 0}, {115, 184, 0}},
        {{84, 184, 0}, {89, 164, 1}},
        {{51, 184, 0}, {70, 184, 0}},
        {{44, 208, 1}, {64, 208, 1}},
        {{133, 208, 1}, {153, 208, 1}},
        {{140, 184, 0}, {159, 184, 0}},
        {{172, 184, 0}, {177, 164, 1}},
        {{40, 184, 0}, {45, 164, 1}},
        {{95, 140, 0}, {327, 232, 1}},
        {{301, 208, 1}, {321, 208, 0}},
        {{345, 208, 0}, {365, 208, 1}},
        {{262, 188, 1}, {282, 188, 0}},
        {{257, 164, 0}, {262, 144, 1}},
        {{269, 164, 0}, {289, 164, 1}},
        {{269, 120, 0}, {274, 100, 1}},
        {{395, 188, 0}, {414, 188, 1}},
        {{421, 208, 0}, {427, 188, 1}},
        {{401, 164, 1}, {421, 164, 0}},
        {{409, 252, 0}, {415, 232, 1}},
        {{363, 144, 1}, {382, 144, 0}},
        {{318, 144, 0}, {338, 144, 1}},
        {{357, 120, 0}, {362, 100, 1}},
        {{368, 76, 0}, {-1, -1, 0}},
    },
    {
        {{120, 215, 1}, {-1, -1, 0}},
        {{95, 235, 0}, {100, 215, 1}},
        {{128, 203, 0}, {148, 203, 0}},
        {{110, 190, 0}, {114, 169, 1}},
        {{50, 247, 0}, {70, 247, 0}},
        {{163, 191, 0}, {168, 171, 1}},
        {{175, 147, 0}, {180, 127, 1}},
        {{139, 159, 0}, {159, 159, 0}},
        {{131, 147, 0}, {318, 239, 1}},
        {{289, 227, 0}, {309, 227, 0}},
        {{324, 215, 0}, {330, 195, 1}},
        {{245, 227, 0}, {265, 227, 0}},
        {{236, 215, 0}, {242, 195, 1}},
        {{257, 183, 0}, {277, 183, 0}},
        {{280, 171, 0}, {286, 151, 1}},
        {{301, 183, 0}, {321, 183, 0}},
        {{270, 127, 0}, {275, 107, 1}},
        {{348, 173, 0}, {368, 173, 1}},
        {{336, 171, 0}, {342, 151, 1}},
        {{397, 155, 0}, {417, 155, 1}},
        {{434, 135, 0}, {439, 115, 1}},
        {{348, 127, 0}, {353, 107, 1}},
        {{360, 83, 0}, {-1, -1, 0}},
        {{-1, -1, -1}, {-1, -1, -1}},
        {{-1, -1, -1}, {-1, -1, -1}},
    },
};
constexpr int kFlagBase = 2601, kFlagStride = 50, kSteps = 25;
} // namespace mapui

int mapGate(int map) { return map < 13 ? 0 : map < 29 ? 1 : map < 46 ? 2 : 3; } // split by the 3D rooms 13, 29, 46

void openMapScreen(App& app) {
    using namespace mapui;
    App::MapScreen& ms = app.mapScreen;
    if (app.mapNumber < 0 || app.mapNumber >= 64) return;
    const int gate = mapGate(app.mapNumber);
    if (ms.gate != gate || !ms.tex.id) { // ReadMapMenu: only when the gate changes
        if (ms.tex.id) app.renderer.destroy(ms.tex);
        cr::Image img;
        static const char* const kFile[4] = {"map_D01.mct", "map_D02.mct", "map_D03.mct", "map_D04.mct"};
        if (loadImage(app, kFile[gate], img)) ms.tex = app.renderer.upload(img);
        ms.gate = gate;
    }
    const short* sp = kMapSpot[app.mapNumber];
    ms.spot[0] = sp[0] < 0 ? -1 : sp[0] - 24;
    ms.spot[1] = sp[1] < 0 ? -1 : sp[1] - 20;
    for (auto& m : ms.marks) m[0] = m[1] = -1;
    const int base = kFlagBase + kFlagStride * fieldRoute(app);
    for (int step = 0; step < kSteps; ++step)
        if (app.game.flag(base + step)) {
            for (int k = 0; k < 2; ++k) {
                const short* m = kMapMark[gate][step][k];
                if (m[0] - 8 > -1 && m[1] - 16 > -1) ms.marks[k][0] = m[0] - 8, ms.marks[k][1] = m[1] - 16;
            }
            break;
        }
    ms.open = true;
    ms.closing = false;
    ms.fade = 0;
    ms.blink = 0;
    ms.clock = 0;
}

void closeMapScreen(App& app) {
    if (!app.mapScreen.open || app.mapScreen.closing) return;
    menuSound(app, se::kChoose); // __effgo(2)
    app.mapScreen.closing = true;
}

void stepMapScreen(App& app) {
    App::MapScreen& ms = app.mapScreen;
    if (!ms.open) return;
    ms.blink = (ms.blink + 1) % 12;
    if (ms.closing) {
        if (--ms.fade <= 0) ms.open = false, ms.closing = false;
    } else
        ms.fade = std::min(4, ms.fade + 1);
}

void drawMapScreen(App& app) {
    App::MapScreen& ms = app.mapScreen;
    if (!ms.open) return;
    cr::Renderer& r = app.renderer;
    cr::MenuPainter& m = app.menu;
    const float a = float(ms.fade) / 4;
    r.setOrigin(0);
    r.fillRect(0, 0, r.screenW(), cr::Renderer::kLogicalH, 0, 0, 0, 0.75f * a);
    r.setOrigin(r.uiX());
    m.begin(r, app.windowTex, &app.font);
    if (!ms.tex.id) return;
    m.imagePiece(ms.tex, 16, 32, 0, 0, 480, 320, a);
    const int frame = ms.blink / 6;
    for (const auto& mk : ms.marks)
        if (mk[0] >= 0) m.imagePiece(ms.tex, float(mk[0]) + 16, float(mk[1]) + 32, float(16 * frame), 368, 16, 16, a);
    if (ms.spot[0] >= 0) m.imagePiece(ms.tex, float(ms.spot[0]) + 16, float(ms.spot[1]) + 32, float(48 * frame), 320, 48, 48, a);
}

void drawCamp(App& app) {
    using namespace campui;
    App::Camp& c = app.camp;
    cr::Renderer& r = app.renderer;
    cr::MenuPainter& m = app.menu;
    m.begin(r, app.windowTex, &app.font);
    r.setOrigin(0);
    r.fillRect(0, 0, r.screenW(), cr::Renderer::kLogicalH, 0, 0, 0, 0.6f);
    r.setOrigin(r.uiX());
    const PhoneMenu phoneMenu(app, m);
    // Widescreen: the heading, the boards and the section labels to the screen's left, the plates,
    // lists, Yes / No, the clock and Oz to its right (as the shop); Back and the message centred
    const auto left = [&] { r.setOrigin(wideLeft(app)); };
    const auto right = [&] { r.setOrigin(wideRight(app)); };
    const auto centre = [&] { r.setOrigin(r.uiX()); };
    left();
    const float t = std::min(1.0f, float(c.anim) / 8.0f), ease = 1 - (1 - t) * (1 - t);
    const bool main = c.page == App::Camp::Main;
    m.picture(81, 8, 4, 1); // "Camp"
    const float out = main ? 1 - ease : ease; // the main screen's pieces slide away on a page
    for (int k = 0; k < 2; ++k) {
        const int ch = campCharacter(app, k);
        int hp, hpMax, mp, mpMax;
        memberHpMp(app, ch, hp, hpMax, mp, mpMax);
        drawFaceBoard(app, m, -240 * out, 24 + 88.0f * float(k), ch, hp, hpMax, mp, mpMax, 1 - out);
    }
    right();
    for (int i = 0; i < campEntries(app); ++i) {
        const bool save = i == 5;
        m.plate(kPlateX + 360 * out, kPlateY + kPlateStep * float(i), save ? 6 : plateIcon(kWords[i]), save ? -1 : kWords[i],
                main && c.cursor == i, 1 - out, !save && !kReady[i], save ? "Save" : "");
    }
    centre();
    m.backButton(kBackX, campBackY(app), false, 0, 1);
    left();
    if (!main) {
        static const int kTitle[5] = {0, 58070, 58059, 58087, 58077};
        const float pa = ease;
        m.picture(15, 0 - 140 * (1 - ease), 56, pa);
        m.textCentred(sysText(app, kTitle[c.page]), 12 - 140 * (1 - ease), 47, false, pa, 15);
        std::string section;
        if (c.page == App::Camp::Items) section = sysText(app, kCategoryLabel[c.category]);
        else if (c.page == App::Camp::Equip) section = c.phase == App::Camp::Member ? sysText(app, 58060) : sysText(app, 58061 + c.slot);
        else if (c.page == App::Camp::Genes) section = sysText(app, 58079);
        else section = sysText(app, 58087);
        if (!section.empty() && section[0] != '>') section = ">" + section;
        m.picture(16, 24 - 140 * (1 - ease), 72, pa);
        m.textCentred(section, 40 - 140 * (1 - ease), 65, false, pa, 15);
        if (c.page == App::Camp::Items && c.phase == App::Camp::List) { // ◀ ▶ beside the section
            m.picture(99, 8, 60, pa);
            m.picture(100, 168, 60, pa);
        }
    }
    const int ch = campCharacter(app, c.member);
    const int item = c.list.empty() ? 0 : c.list[size_t(c.listCursor)];
    auto memberPlates = [&](bool lit) {
        right();
        for (int k = 0; k < 2; ++k) {
            const int word = 12 + campCharacter(app, k);
            m.plate(kPlateX + 200 * (1 - ease), kMemberY + kMemberStep * float(k), plateIcon(word), word, lit && c.member == k, ease, false);
        }
        left();
    };
    auto helpBoard = [&](int it) { // HELP: the item's text across the top (as the shop's)
        if (it <= 0) return;
        right();
        const float hx = 152;
        m.stretch(12, hx, 0, 512 - hx, 66, ease);
        m.picture(18, hx + 4, 2, ease);
        std::vector<std::vector<std::string>> help;
        if (app.systemText.message(cr::itemHelpId(it), app.language, nullptr, help) && !help.empty()) {
            float y = 20;
            for (const std::string& line : help[0]) {
                m.text(line, hx + 14, y, false, ease, 13);
                if ((y += 16) > 52) break;
            }
        }
        left();
    };
    switch (c.page) {
    case App::Camp::Items:
        if (c.phase == App::Camp::List) {
            helpBoard(item);
            right();
            if (c.list.empty()) m.text("Nothing here.", kRowX + 56, kRowY + 16, false, ease, 15);
            for (int k = 0; k < kRows && c.listTop + k < int(c.list.size()); ++k) {
                const int index = c.listTop + k, it = c.list[size_t(index)];
                const bool usable = campUsable(it);
                drawCampRow(app, m, kRowX + 160 * (1 - ease), kRowY + kRowH * float(k), it, ownedCount(app, it),
                            index == c.listCursor, usable || c.category != 0 ? ease : ease * 0.6f);
            }
            drawCampScrollBar(m, c.listTop, int(c.list.size()), ease);
            left();
        } else {
            drawShopBoard(app, 12, 92, ch, 0, ease);
            if (c.phase == App::Camp::Member) memberPlates(true);
            else {
                const bool seed = item >= 3 && item <= 6;
                right();
                if (seed) drawNumCount(m, c.count, campSeedLimit(app, ch, item));
                m.plate(336, 104, plateIcon(0), 0, c.confirm == 0, 1, false); // Yes
                m.plate(336, 160, plateIcon(1), 1, c.confirm == 1, 1, false); // No
                centre();
                m.picture(12, 0, 220, 1);
                m.text(sysText(app, seed ? 57077 : 57014, itemName(app, item)), 24, 234, false, 1, 15);
                left();
            }
        }
        break;
    case App::Camp::Equip: {
        const cr::PartyMember& pm = app.game.party[size_t(ch)];
        if (c.phase == App::Camp::Member) {
            drawShopBoard(app, 12, 92, ch, 0, ease);
            memberPlates(true);
        } else if (c.phase == App::Camp::Slot) {
            drawShopBoard(app, 12, 92, ch, 0, ease);
            right();
            for (int k = 0; k < kEquipSlots; ++k) { // the rows at 70%: weapon, armor, jewel, the genes
                const float rx = kRowX + 160 * (1 - ease) + 64, ry = 84 + 34.0f * float(k);
                m.setScale(rx, ry, 0.7f);
                drawCampRow(app, m, rx, ry, k < 3 ? pm.equip[k] : pm.genes[k - 3], 0, c.slot == k, k == 3 ? ease * 0.7f : ease,
                            sysText(app, 58061 + k));
                m.setScale(0, 0, 1);
            }
            left();
        } else if (c.slot >= 3) { // a gene plate for the slot: its board beside the list
            drawGeneBoard(app, m, 12, 92, item > 0 ? item : pm.genes[c.slot - 3], ease);
            right();
            for (int k = 0; k < kRows && c.listTop + k < int(c.list.size()); ++k) {
                const int index = c.listTop + k, it = c.list[size_t(index)];
                const bool worn = std::find(std::begin(pm.genes), std::end(pm.genes), it) != std::end(pm.genes);
                if (cr::isPlate(it)) drawGeneRow(app, m, kRowX + 64, kRowY + kRowH * float(k), it, index == c.listCursor, worn, ease);
                else drawCampRow(app, m, kRowX + 64, kRowY + kRowH * float(k), it, 0, index == c.listCursor, ease);
            }
            drawCampScrollBar(m, c.listTop, int(c.list.size()), ease);
            helpBoard(item);
        } else {
            drawShopBoard(app, 12, 92, ch, item > 0 ? item : 0, ease);
            right();
            for (int k = 0; k < kRows && c.listTop + k < int(c.list.size()); ++k) {
                const int index = c.listTop + k, it = c.list[size_t(index)];
                drawCampRow(app, m, kRowX + 64, kRowY + kRowH * float(k), it, it > 0 ? ownedCount(app, it) : 0,
                            index == c.listCursor, ease);
            }
            drawCampScrollBar(m, c.listTop, int(c.list.size()), ease);
            helpBoard(item);
        }
        break;
    }
    case App::Camp::Genes:
        drawGeneBoard(app, m, 14, 92, item, ease);
        right();
        if (c.list.empty()) m.text("No gene plates.", kRowX + 56, kRowY + 16, false, ease, 15);
        for (int k = 0; k < kRows && c.listTop + k < int(c.list.size()); ++k) {
            const int index = c.listTop + k, it = c.list[size_t(index)];
            const cr::PartyMember& gm = app.game.party[size_t(ch)];
            const bool worn = std::find(std::begin(gm.genes), std::end(gm.genes), it) != std::end(gm.genes);
            if (cr::isPlate(it)) drawGeneRow(app, m, kRowX + 64, kRowY + kRowH * float(k), it, index == c.listCursor, worn, ease);
            else drawCampRow(app, m, kRowX + 64, kRowY + kRowH * float(k), it, 0, index == c.listCursor, ease);
        }
        drawCampScrollBar(m, c.listTop, int(c.list.size()), ease);
        break;
    case App::Camp::Status:
        if (c.phase == App::Camp::Member) {
            drawShopBoard(app, 12, 92, ch, 0, ease);
            memberPlates(true);
        } else { // the member's board on the right, their face board on the left
            int hp, hpMax, mp, mpMax;
            memberHpMp(app, ch, hp, hpMax, mp, mpMax);
            right();
            drawShopBoard(app, 308, 68 + 32, ch, 0, ease);
            left();
            drawFaceBoard(app, m, 0, 120, ch, hp, hpMax, mp, mpMax, ease);
            const cr::PartyMember& pm = app.game.party[size_t(ch)];
            static const char* const kSeedName[4] = {"Attack", "Defense", "Speed", "Luck"};
            for (int k = 0; k < 4; ++k) { // the seeds' points (our addition: the original folds them in)
                m.text(std::string(kSeedName[k]) + " seeds", 24, 248 + 16.0f * float(k), false, ease * 0.8f, 12);
                drawShopNumber(m, pm.seeds[k], 130, 248 + 16.0f * float(k), 4, 12, ease);
            }
        }
        break;
    default: break;
    }
    const bool helpUp = c.phase == App::Camp::List && item > 0 &&
                        (c.page == App::Camp::Items || c.page == App::Camp::Equip);
    right();
    if (!helpUp) drawClockAndOz(app, m, 1); // the HELP board takes the top while it's up
    centre();
    if (c.messageFrames > 0) --c.messageFrames;
    if (c.messageFrames > 0 && !c.message.empty()) {
        m.picture(12, 0, 220, 1);
        m.text(c.message, 24, 234, false, 1, 15);
    }
    left();
    const char* help = main ? "Up/Down  Enter  Esc or K: back"
                       : c.page == App::Camp::Items && c.phase == App::Camp::List ? "Up/Down  Left/Right: category  Enter  Esc: back"
                       : c.phase == App::Camp::Ask ? "Left/Right: how many  Up/Down  Enter  Esc: back"
                       : c.page == App::Camp::Status && c.phase == App::Camp::Slot ? "Left/Right: the other member  Esc: back"
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
    cr::writeSave(savePath(app, "resume.dat"), captureSave(app));
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

// At the title: "You have save data from your last session. Resume game from last saved point?"
void offerResume(App& app) {
    if (app.resumePrompt || !app.persist) return;
    if (cr::readSave(savePath(app, "resume.dat"), app.resumeData)) {
        app.resumePrompt = 1;
        app.resumeCursor = 0;
    }
}

void answerResume(App& app, bool yes) {
    app.resumePrompt = 0;
    SDL_RemovePath(savePath(app, "resume.dat").c_str()); // used up either way, as the original's
    const bool failed = yes && !applySave(app, app.resumeData);
    menuSound(app, failed ? se::kBuzzer : se::kChoose); // Yes and No alike (TitlePrc's ComUty_YesNoChoicePosProc)
}

void drawResumePrompt(App& app) {
    cr::Renderer& r = app.renderer;
    cr::MenuPainter& m = app.menu;
    m.begin(r, app.windowTex, &app.font);
    const PhoneMenu phoneMenu(app, m); // the iPhone layout: its screen's menu coordinates
    r.setOrigin(0);
    r.fillRect(0, 0, r.screenW(), cr::Renderer::kLogicalH, 0, 0, 0, 0.6f);
    r.setOrigin(r.uiX());
    const float po = r.phoneLayout() ? 1.0f : 0.0f; // (the panel bypasses the menu's iPhone offset)
    drawWinPiece(app, kWinSmallPanel, 64 * 2 + 32 * po, 140 * 2 + 64 * po, 384 * 2, 72 * 2);
    m.textCentred(sysText(app, 57092), 84, 160, false, 1, 15);
    m.textCentred(sysText(app, 57093), 84, 188, false, 1, 15);
    m.button(128, 230, 0, "", app.resumeCursor == 0, 1, app.resumeCursor == 0);
    m.button(272, 230, 1, "", app.resumeCursor == 1, 1, app.resumeCursor == 1);
}

// The flags of every pair's story at once: what the original keeps in its system data (the
// Scenarios chart, the second Character Select sheet). Ours: the pairs' saves and the resume point
// prty_init_new(pair - 1, mode): a new game for the pair. Its flags 1001 + 400p .. 1400 + 400p
// are cleared, its two members start over from their records (0xcb3f0: level 1, their weapon,
// armor and jewel, the Human Genes, their own ability and skill), its bag holds the starting
// items (those and 0xce700: 3 Emergency Exits, 2 Chocolates). With the pair's clear data (flag
// 40 + p, its slot) the game goes on from that data - Oz and the flags outside the range kept -
// and mode 3 (var 1 = 3, "with your clear data") also keeps each member's stat bonuses (seeds
// and level-up skills' points: +0x0c HP, +0x16 MP, +0x1c..+0x28), equipment and gene plates, and
// the bag, less its kind 3 and 9 items (the story's)
void newPairParty(App& app, int pair, bool clearData) {
    const int p = std::clamp(pair, 1, 4) - 1;
    cr::SaveData slot;
    const bool fromClear = allPairsFlags(app).flag(40 + p) && cr::readSave(slotPath(app, p + 1), slot);
    cr::GameState g = fromClear ? slot.game : app.game;
    if (fromClear) {
        for (int f : {0, 4089}) g.setFlag(f, app.game.flag(f)); // the boot script's own
        g.openedBoxes.clear();
        app.treasureBoxes.clear();
    }
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
        if (fromClear && clearData) {
            const cr::PartyMember& old = g.party[c];
            std::copy(std::begin(old.seeds), std::end(old.seeds), m.seeds);
            std::copy(std::begin(old.boosts), std::end(old.boosts), m.boosts);
            std::copy(std::begin(old.equip), std::end(old.equip), m.equip);
            std::copy(std::begin(old.genes), std::end(old.genes), m.genes);
        }
        m.hp = m.mp = -1;
        g.party[c] = m;
    }
    if (fromClear && clearData) {
        for (auto it = g.items.begin(); it != g.items.end();) {
            const cr::ItemParam* ip = app.itemTable.item(it->first);
            if (ip && (ip->kind == 3 || ip->kind == 9)) it = g.items.erase(it);
            else ++it;
        }
        cr::ensureStartingGenes(g);
    } else {
        g.items.clear();
        cr::giveStartingItems(g);
    }
    if (!fromClear) g.walkExp = {}, g.noEncounters = {};
    app.game = std::move(g);
    app.playerChar = 2 * p;
}

cr::GameState allPairsFlags(const App& app) {
    cr::GameState g;
    auto take = [&](const cr::GameState& s) {
        for (size_t i = 0; i < g.flags.size() && i < s.flags.size(); ++i) g.flags[i] |= s.flags[i];
    };
    for (int pair = 1; pair <= 4; ++pair) {
        cr::SaveData d;
        if (cr::readSave(slotPath(app, pair), d)) take(d.game);
    }
    cr::SaveData d;
    if (cr::readSave(savePath(app, "resume.dat"), d)) take(d.game);
    if (!app.booting) take(app.game);
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
constexpr float kPhoneHistoryLowest = 285 - (62 + 636 + 40) - 8; // the vertical scroll's end (tex2d)

void openScenarios(App& app) {
    app.scenariosOpen = true;
    app.scenariosScroll = 0;
    app.scenariosHScroll = 0;
    app.scenariosAnim = 0;
    app.scenariosFlags = allPairsFlags(app);
    if (!app.scenariosSheet.id) {
        cr::Image img;
        if (loadImage(app, "flow_hd.mct", img)) app.scenariosSheet = app.renderer.upload(img);
    }
}

void closeScenarios(App& app) {
    app.scenariosOpen = false;
    app.renderer.destroy(app.scenariosSheet);
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
    int st = f(kHistoryState[i][1]) ? 3 : f(kHistoryState[i][0]) ? 2 : 0;
    if (f(kHistoryNext[i][0]) && !f(kHistoryNext[i][1])) st = 1;
    return st;
}

int scenarioPercent(const cr::GameState& g) {
    int p = 0;
    for (const auto& e : kHistoryPoints)
        if (g.flag(e[0])) p += e[1];
    return std::min(p, 100);
}

void drawScenarios(App& app) {
    cr::Renderer& r = app.renderer;
    const cr::Texture& t = app.scenariosSheet;
    r.setOrigin(0);
    r.fillRect(0, 0, r.screenW(), cr::Renderer::kLogicalH, 0, 0, 0, 1);
    r.setOrigin(r.uiX());
    if (!t.id) return;
    const float a = std::min(1.0f, app.scenariosAnim / 8.0f);
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
                if (st != 0) at(31, tx + 61, ty - 10);
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
        if (vs < 0) at(1, 218, 92);
        if (vs > kPhoneHistoryLowest) at(2, 218, 276);
        if (hs < 0) at(3, 0, 174);
        if (hs > -120) at(4, 469, 174);
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
        m.backButton(192, 286, false, 0, a);
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
            if (st != 0) piece(31, x + 122, y - 20); // the way in: open / shut (the open one by the strips)
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
    if (sy < 0) piece(1, 436, 100);
    if (sy > kHistoryBottom - (1272 + 40 + 80)) piece(2, 436, 616);
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
    m.backButton(272, 330, false, 0, a);
    if (keyHints(app)) // (our key hints)
        m.text("Up/Down: scroll  Esc: back", 300, 372, false, a * 0.8f, 9);
}

void scenariosScrollBy(App& app, float d) {
    const float lowest = app.renderer.phoneLayout() ? 2 * kPhoneHistoryLowest : kHistoryBottom - (1272 + 40 + 80) - 16;
    app.scenariosScroll = std::clamp(app.scenariosScroll + d, lowest, 0.0f);
}
void scenariosScrollSideways(App& app, float d) { // the iPhone layout's chart (tex2d)
    if (app.renderer.phoneLayout()) app.scenariosHScroll = std::clamp(app.scenariosHScroll + d, -120.0f, 0.0f);
}

void scenariosKey(App& app, SDL_Keycode key) {
    switch (key) {
    case SDLK_UP: case SDLK_W: scenariosScrollBy(app, 96); break;
    case SDLK_DOWN: case SDLK_S: scenariosScrollBy(app, -96); break;
    case SDLK_PAGEUP: scenariosScrollBy(app, 480); break;
    case SDLK_PAGEDOWN: scenariosScrollBy(app, -480); break;
    case SDLK_LEFT: case SDLK_A: scenariosScrollSideways(app, 40); break;
    case SDLK_RIGHT: case SDLK_D: scenariosScrollSideways(app, -40); break;
    case SDLK_ESCAPE: case SDLK_BACKSPACE: case SDLK_AC_BACK: case SDLK_RETURN: menuSound(app, se::kBack); closeScenarios(app); break;
    default: break;
    }
}

// ---- The Instruction Manual (Options; MuScrList + MsgUty: the topics, then the page) ----
// The topics are system messages 56000 on ("Title / / text"); 56007 is the manual's own
// introduction, 56006 / 56008 are fragments, "dummy" fills the gaps
// A manual message's title: its first non-empty line (the messages open with an empty one)
std::string manualTitle(const App& app, int id) {
    std::vector<std::vector<std::string>> pages;
    if (!app.systemText.message(id, app.language, nullptr, pages)) return std::string();
    for (const auto& pg : pages)
        for (const std::string& l : pg)
            if (!l.empty()) return l;
    return std::string();
}

std::vector<int> manualTopics(const App& app) {
    std::vector<int> ids;
    for (int id = 56000; id <= 56036; ++id) {
        if (id == 56006 || id == 56007 || id == 56008) continue;
        const std::string t = manualTitle(app, id);
        if (t.empty() || t == "dummy") continue;
        ids.push_back(id);
    }
    return ids;
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

void openManual(App& app) {
    app.manualOpen = true;
    app.manualCursor = -1; // the introduction first
    app.manualTop = 0;
    app.manualAnim = 0;
}

constexpr int kManualRows = 9;

void drawManual(App& app) {
    cr::Renderer& r = app.renderer;
    cr::MenuPainter& m = app.menu;
    m.begin(r, app.windowTex, &app.font);
    const PhoneFit phoneFit(app, m); // the iPhone layout: its screen's menu coordinates
    r.setOrigin(0);
    r.fillRect(0, 0, r.screenW(), cr::Renderer::kLogicalH, 0, 0, 0, 0.75f);
    r.setOrigin(r.uiX());
    const float a = std::min(1.0f, app.manualAnim / 8.0f);
    m.picture(102, 8, 4, a); // "Options"
    m.text(sysLabel(app, 58081, "Instruction Manual"), 236, 14, false, a, 16);
    const std::vector<int> topics = manualTopics(app);
    for (int i = 0; i < kManualRows && app.manualTop + i < int(topics.size()); ++i) {
        const int n = app.manualTop + i;
        std::string title = manualTitle(app, topics[size_t(n)]);
        int same = 0; // the puzzles' second pages share their title
        for (int k = 0; k < n; ++k) same += manualTitle(app, topics[size_t(k)]) == title;
        if (same) title += " (" + std::to_string(same + 1) + ")";
        m.rowBar(20, 52 + 32.0f * float(i), title, a, app.manualCursor == n);
    }
    if (app.manualTop > 0) m.text("...", 100, 40, false, a, 12);
    if (app.manualTop + kManualRows < int(topics.size())) m.text("...", 100, 52 + 32.0f * kManualRows - 6, false, a, 12);
    // The page, on the right
    const int id = app.manualCursor >= 0 && app.manualCursor < int(topics.size()) ? topics[size_t(app.manualCursor)] : 56007;
    if (r.phoneLayout()) // (the panel in the fitted menu's units)
        drawWinPiece(app, kWinMessagePanel, 2 * (kPhoneFitX + 252 * kPhoneFitK), 2 * (kPhoneFitY + 48 * kPhoneFitK),
                     2 * 252 * kPhoneFitK, 2 * 280 * kPhoneFitK);
    else drawWinPiece(app, kWinMessagePanel, 2 * 252, 2 * 48, 2 * 252, 2 * 280);
    m.text(manualTitle(app, id), 268, 58, false, a, 13);
    float y = 82;
    constexpr float kWidth = 228; // the panel's inside, in menu units: lines wrap at words
    for (const std::string& l : manualPage(app, id)) {
        std::string line;
        auto flush = [&] {
            if (y <= 316) m.text(line, 268, y, false, a, 10);
            y += 14;
            line.clear();
        };
        size_t at = 0;
        if (l.empty()) { flush(); continue; }
        while (at < l.size()) {
            const size_t sp = l.find(' ', at);
            const std::string word = l.substr(at, sp == std::string::npos ? std::string::npos : sp - at);
            const std::string tryLine = line.empty() ? word : line + " " + word;
            if (!line.empty() && m.textWidth(tryLine, 10) > kWidth) {
                flush();
                line = word;
            } else {
                line = tryLine;
            }
            if (sp == std::string::npos) break;
            at = sp + 1;
        }
        flush();
    }
    m.backButton(192, 344, false, 0, a);
    if (app.showHud) m.text("Up/Down  Esc: back", 20, 372, false, a * 0.8f, 9); // (our key hints)
}

void manualKey(App& app, SDL_Keycode key) {
    const int n = int(manualTopics(app).size());
    int& c = app.manualCursor;
    switch (key) {
    case SDLK_UP: case SDLK_W: c = std::max(0, c - 1); menuSound(app, se::kCursor); break;
    case SDLK_DOWN: case SDLK_S: c = std::min(n - 1, c + 1); menuSound(app, se::kCursor); break;
    case SDLK_PAGEUP: c = std::max(0, c - kManualRows); menuSound(app, se::kPage); break; // a list flicked a page
    case SDLK_PAGEDOWN: c = std::min(n - 1, c + kManualRows); menuSound(app, se::kPage); break;
    case SDLK_ESCAPE: case SDLK_BACKSPACE: case SDLK_AC_BACK: app.manualOpen = false; menuSound(app, se::kBack); return;
    default: return;
    }
    if (c >= 0 && c < app.manualTop) app.manualTop = c;
    if (c >= app.manualTop + kManualRows) app.manualTop = c - kManualRows + 1;
}

void manualClick(App& app, float px, float py) {
    float lx, ly;
    app.renderer.toLogical(px, py, lx, ly);
    float mx, my;
    menuPointFit(app, lx, ly, mx, my);
    if (my >= 344 && my < 368 && mx >= 192 && mx < 424) { app.manualOpen = false; menuSound(app, se::kBack); return; }
    if (mx >= 20 && mx < 240)
        for (int i = 0; i < kManualRows; ++i)
            if (my >= 52 + 32.0f * i && my < 76 + 32.0f * i) {
                app.manualCursor = std::min(app.manualTop + i, int(manualTopics(app).size()) - 1);
                menuSound(app, se::kChoose); // a topic tapped (__ScrListSelectSePlay)
            }
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
    app.charSelectPage = 0;
    app.renderer.destroy(app.charSelectArt);
    cr::Image img;
    if (loadImage(app, "chara1_hd.mct", img)) app.charSelectArt = app.renderer.upload(img);
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
    for (int k = 0; k < 2; ++k)
        m.plate(56 + 232.0f * float(k), 232, plateIcon(kCharSelectWords[app.charSelectPage][k]), // (the word's icon)
                kCharSelectWords[app.charSelectPage][k], app.charSelectCursor == k, t, false);
    m.backButton(124, 288, false, 0, t); // ComUty_BackButtonPosSetXYDraw(124, 288)
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

void charSelectClick(App& app, float px, float py) {
    float lx, ly;
    app.renderer.toLogical(px, py, lx, ly);
    float mx, my;
    menuPoint(app, lx, ly, mx, my);
    for (int k = 0; k < 2; ++k) {
        const float x = 56 + 232.0f * float(k);
        if (mx >= x && mx < x + 132 && my >= 232 && my < 272) {
            menuSound(app, se::kChoose);
            closeCharSelect(app, app.charSelectPage * 2 + k + 1);
            return;
        }
    }
    if (my >= 288 && my < 312 && mx >= 124 && mx < 356) { menuSound(app, se::kBack); closeCharSelect(app, 0); }
}

// ---- The title menu (Util 0: __menuu_Title*) ----
// Three plates (ComUty_ChoicePlateDraw) at (328, 114 + 56k): Start, Scenarios, Options (words 2,
// 32, 3; icons kanIconNoTbl - 1), the cursor's lit, and the emblems (pictures 155 and 124) at
// (320, 16) / (380, 16), over the title picture the script showed in Tex2d slot 0.
constexpr int kTitleWords[3] = {2, 32, 3};
constexpr int kTitleIcons[3] = {2, 27, 3};

bool titleChoiceReady(int) { return true; }

void drawTitleMenu(App& app) {
    cr::MenuPainter& m = app.menu;
    m.begin(app.renderer, app.windowTex, &app.font);
    const PhoneMenu phoneMenu(app, m); // the iPhone layout: its screen's menu coordinates
    const float t = std::min(1.0f, app.titleAnim / 10.0f), ease = 1 - (1 - t) * (1 - t);
    app.renderer.setOrigin(wideRight(app)); // (widescreen: the emblems and plates to the screen's right)
    m.picture(155, 320, 16, t);
    m.picture(124, 380, 16, t);
    for (int i = 0; i < 3; ++i)
        m.plate(328 + 240 * (1 - ease), 114 + 56.0f * float(i), kTitleIcons[i], kTitleWords[i], app.titleCursor == i, t,
                !titleChoiceReady(i));
    app.renderer.setOrigin(wideLeft(app));
    if (keyHints(app)) // (our key hints)
        m.text("Up/Down  Enter  V: Options  Esc: quit", 20, 360, false, t * 0.8f, 9);
    app.renderer.setOrigin(app.renderer.uiX());
}

void titleChoose(App& app, int i) {
    app.titleCursor = i;
    if (!titleChoiceReady(i)) { menuSound(app, se::kBuzzer); return; }
    menuSound(app, se::kChoose);
    if (i == 2) { openOptions(app, 0); return; } // SetOptionMenu + ModeOptionMenu(1)
    if (i == 1) { openScenarios(app); return; }  // SetHistoryMenu
    // Start: the boot script goes on (flag 0 clear) to Character Select (Util 4), then the pair's
    // save / load screen (Util 5)
    if (app.booting && app.runner.running()) {
        app.game.setFlag(0, false);
        app.titleOpen = false;
    } else {
        openSaveScreen(app, true);
    }
}

// Keys on the title; false = quit
bool titleKey(App& app, SDL_Keycode key) {
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
    float mx, my;
    menuPointAt(app, lx, ly, wideRight(app), mx, my); // (widescreen: the plates on the right)
    for (int i = 0; i < 3; ++i)
        if (mx >= 328 && mx < 464 && my >= 114 + 56 * i && my < 154 + 56 * i) titleChoose(app, i);
}

float screenFadeAlpha(const App& app) {
    if (app.fadeFrames <= 0) return 0;
    const float t = std::min(1.0f, float(app.fadeT) / float(app.fadeFrames));
    return app.fadeFrom + (app.fadeTo - app.fadeFrom) * t;
}

// One 30 Hz frame of the running script and of the things it set going.
// Voiced builds: a page's clips start when the page is shown (PrcMsg reaching code 0x1e), one
// after another; a new page's line replaces one still playing.
void updateVoices(App& app) {
    if (!app.sound.hasVoices()) return;
    if (!app.dialogue.empty()) {
        const int page = int(std::min(app.dialoguePage, app.dialogue.size() - 1));
        if (app.dialogueSerial != app.voicedSerial || page != app.voicedPage) {
            app.voicedSerial = app.dialogueSerial;
            app.voicedPage = page;
            app.voiceQueue.clear();
            if (page < int(app.dialogueVoices.size()) && !app.dialogueVoices[page].empty()) {
                const auto& clips = app.dialogueVoices[page];
                app.sound.playLine(clips[0]);
                ++app.voicesPlayed;
                app.voiceQueue.assign(clips.begin() + 1, clips.end());
            }
        }
    }
    if (!app.voiceQueue.empty() && !app.sound.linePlaying()) {
        app.sound.playLine(app.voiceQueue.front());
        ++app.voicesPlayed;
        app.voiceQueue.erase(app.voiceQueue.begin());
    }
}

// ---- Puzzle rooms (PuzzleScene, PuzzleManager, the touch abilities, the PuzzleObjects) ----
// See game/puzzle.hpp. The room's NPCs with NPC table +20 bits sit on the stage's grid; the
// pair's touch ability acts on them (teleport: tap a block or gem anywhere on the grid and swap
// places with it, one ESP each). Gems are picked up by walking onto them (+1 ESP); the gem
// crystal (WarpCrystalNG) comes when none is left, the switch crystal (WarpCrystalWS) while every
// weigh switch is pressed: the first time its NPC's script plays (flag 4000, var 1 = its event),
// which marks the room (flag 3300 + stage) - afterwards it shows. A cleared room (flag 3000 +
// stage) keeps its crystals and loses its blocks and gems. Field effects (eff3d_set_fld 2..9)
// aren't drawn yet.

// eff3d_set_fld: effect `id` where an NPC (index) or the leader (-1) stands; a looped one runs
// until stopped by its handle
int fieldEffect(App& app, int id, cr::Vec3 at, bool loop = false) {
    return startEffect(app, app.fieldEffects, id, at, loop, 1.0f, 0.0f);
}
int npcEffect(App& app, int id, int npc, bool loop = false) {
    if (npc < 0) return fieldEffect(app, id, app.walker.pos, loop);
    if (npc >= int(app.npcs.size())) return 0;
    const cr::FieldNpc& n = app.npcs[size_t(npc)];
    return fieldEffect(app, id, {n.pos[0], n.pos[1], n.pos[2]}, loop);
}

// A world point on the frame (logical 1024 x 768 pixels, the whole screen's width), as drawFrame
// lays out the stage view (false when it's off the view or behind the camera)
bool fieldToLogical(const App& app, cr::Vec3 p, float& lx, float& ly) {
    const float SW = app.renderer.screenW(), H = cr::Renderer::kLogicalH;
    float vx, vy, vw, vh;
    if (scrollingField(app)) {
        vw = fieldViewW(app), vh = fieldViewH(app);
        vx = (SW - vw) / 2 + app.scroll.offX, vy = (H - vh) / 2 - app.scroll.offY;
    } else {
        vw = vh = app.video.paintings == 2 ? std::max(SW, H) : std::min(SW, H);
        vx = (SW - vw) / 2, vy = (H - vh) / 2;
    }
    float hx, hy;
    if (!fieldViewPoint(app, p, hx, hy)) return false;
    lx = vx + hx * vw / fieldViewW(app), ly = vy + hy * vh / fieldViewH(app);
    return true;
}
// Logical pixels <-> tex2d (the 480 x 320 layout: menu units less (16, 32), half the pixels)
void logicalToTex2d(const App& app, float lx, float ly, float& tx, float& ty) {
    tx = (lx - app.renderer.uiX()) / 2 - 16, ty = ly / 2 - 32;
}

namespace puzzle {

const cr::PuzzleStage* stage(const App& app) {
    return app.puzzle.stage >= 0 ? &cr::kPuzzleStage[app.puzzle.stage] : nullptr;
}

bool cleared(const App& app) { return app.puzzle.stage >= 0 && app.game.flag(3000 + app.puzzle.stage); }

int* cellSlot(App& app, int layer, int r, int c) {
    const cr::PuzzleStage* st = stage(app);
    if (!st || layer < 0 || layer > 2 || r < 0 || c < 0 || r >= st->rows || c >= st->cols) return nullptr;
    return &app.puzzle.grid[size_t((layer * st->rows + r) * st->cols + c)];
}

int objectAt(App& app, int layer, int r, int c) {
    const int* slot = cellSlot(app, layer, r, c);
    return slot ? *slot : -1;
}

cr::Vec3 centre(const App& app, int r, int c) { // getCenterPosAtCell
    const cr::PuzzleStage* st = stage(app);
    if (!st) return {};
    return {st->x0 + cr::kPuzzleCell * (0.5f + float(c)), st->y, st->z0 + cr::kPuzzleCell * (0.5f + float(r))};
}

bool cellOf(const App& app, cr::Vec3 p, int& r, int& c) { // getCellNo
    const cr::PuzzleStage* st = stage(app);
    if (!st) return false;
    const float fr = (p.z - st->z0) / cr::kPuzzleCell, fc = (p.x - st->x0) / cr::kPuzzleCell;
    if (fr < 0 || fc < 0) return false;
    r = int(fr), c = int(fc);
    return r < st->rows && c < st->cols;
}

App::NpcDraw* drawOf(App& app, int npc) {
    for (App::NpcDraw& d : app.npcDraws)
        if (d.index == npc) return &d;
    return nullptr;
}

void fade(App& app, int npc, float from, float to, int frames) { // mdl_set_alpha
    if (App::NpcDraw* d = drawOf(app, npc)) {
        d->alphaFrom = from, d->alphaTo = to, d->alpha = from;
        d->fadeFrames = d->fadeLeft = std::max(frames, 1);
    }
}

void show(App& app, App::Puzzle::Obj& o, bool on) { // npc_born / npc_kill
    if (o.shown == on) return;
    o.shown = on;
    buildNpcDraws(app);
    buildWalkCollision(app);
}

void place(App& app, App::Puzzle::Obj& o, cr::Vec3 p) { // PuzzleObject::setPos
    if (o.npc < 0 || o.npc >= int(app.npcs.size())) return;
    cr::FieldNpc& n = app.npcs[size_t(o.npc)];
    n.pos[0] = p.x, n.pos[1] = p.y, n.pos[2] = p.z;
    if (App::NpcDraw* d = drawOf(app, o.npc)) d->world = npcWorld(n);
    buildWalkCollision(app);
}

// A weigh switch below a cell: pressed (5, `silent` at the start) or let go (6)
void press(App& app, int r, int c, bool on, bool silent = false) {
    const int s = objectAt(app, 0, r, c);
    if (s < 0) return;
    App::Puzzle::Obj& sw = app.puzzle.objs[size_t(s)];
    if (sw.type != cr::kTypeWeighSwitch) return;
    if (on && !silent) app.sound.play(0x28);
    sw.on = on;
}

bool switchAt(App& app, int r, int c) {
    const int s = objectAt(app, 0, r, c);
    return s >= 0 && app.puzzle.objs[size_t(s)].type == cr::kTypeWeighSwitch;
}

bool allSwitchesOn(App& app) { // IsOnAllSwitch
    for (const App::Puzzle::Obj& o : app.puzzle.objs)
        if (o.type == cr::kTypeWeighSwitch && !o.on) return false;
    return true;
}

bool gemsLeft(App& app) {
    for (const App::Puzzle::Obj& o : app.puzzle.objs)
        if (o.type == cr::kTypeGem && o.state != 0) return true;
    return false;
}

constexpr float kGhostAlpha = 0x60 / 255.0f;

// PuzzleManager::init (PuzzleScene::init, and again on a reset): the objects from the room's NPCs
void enter(App& app) {
    App::Puzzle& pz = app.puzzle;
    pz = App::Puzzle();
    app.fieldEffects.clear(); // the objects' effects go with them
    pz.stage = cr::puzzleStage(app.area, app.mapNumber);
    const cr::PuzzleStage* st = stage(app);
    if (!st) { pz.stage = -1; return; }
    pz.esp = st->esp;
    app.game.setFlag(3300 + pz.stage, false);
    app.game.vars[13] = 0; // the uses
    pz.grid.assign(size_t(3 * st->rows * st->cols), -1);
    const bool done = cleared(app);
    for (size_t i = 0; i < app.npcs.size(); ++i) {
        const int type = cr::puzzleType(app.npcs[i].puzzle, st->ability);
        if (!type) continue;
        App::Puzzle::Obj o;
        o.npc = int(i);
        o.type = type;
        o.layer = cr::puzzleLayer(type);
        const cr::Vec3 p{app.npcs[i].pos[0], app.npcs[i].pos[1], app.npcs[i].pos[2]};
        const bool inGrid = cellOf(app, p, o.row, o.col);
        o.shown = npcActive(app, app.npcs[i]);
        o.baseY = app.npcs[i].pos[1];
        if (type == cr::kTypeNormal || type == cr::kTypeGem) { // their init: in play unless cleared
            o.shown = !done;
            o.state = done ? 0 : 1;
        }
        if (type == cr::kTypeCrystal) o.shown = done;
        if ((type == cr::kTypeFragile || type == cr::kTypeRed) && !done) o.shown = true; // npc_born
        if (type == cr::kTypeBlue) o.shown = false, o.ghost = true; // npc_kill, alpha 0x60
        if (type == cr::kTypeCandle) { // lit at the start (+22) unless the stage is cleared
            o.shown = !done && app.npcs[i].link[0] != 0;
            o.ghost = !o.shown;
        }
        pz.objs.push_back(o);
        if (inGrid && type != cr::kTypeCrystal)
            if (int* slot = cellSlot(app, o.layer, o.row, o.col)) *slot = int(pz.objs.size() - 1);
    }
    for (App::Puzzle::Obj& o : pz.objs) // InitSwitchOn: a block standing on a switch presses it
        if (o.type == cr::kTypeNormal && o.state == 1 && switchAt(app, o.row, o.col)) {
            o.onSwitch = true;
            press(app, o.row, o.col, true, true);
            cr::Vec3 p = centre(app, o.row, o.col);
            p.y = app.npcs[size_t(o.npc)].pos[1] + cr::kPuzzleSwitchRise;
            place(app, o, p);
        }
    for (App::Puzzle::Obj& o : pz.objs) // WarpCrystalWS::postInit
        if (o.type == cr::kTypeCrystal && (app.npcs[size_t(o.npc)].puzzle & cr::kPuzzleObject40) && !done)
            o.shown = allSwitchesOn(app);
    buildNpcDraws(app);
    buildWalkCollision(app);
    for (const App::Puzzle::Obj& o : pz.objs)
        if (o.ghost) fade(app, o.npc, kGhostAlpha, kGhostAlpha, 1);
}

void stepBlock(App& app, App::Puzzle::Obj& o) { // NormalBlockTP / NormalGem (the teleported side)
    switch (o.state) {
    case 1: // onIdle: a teleport asked for - fade out
        if (o.type == cr::kTypeGem && o.pending != 8) { // picked up by walking onto it
            const cr::Vec3 c = centre(app, o.row, o.col);
            const cr::Vec3 p = app.walker.pos;
            if (std::fabs(p.x - c.x) < cr::kPuzzleCell * 0.5f && std::fabs(p.z - c.z) < cr::kPuzzleCell * 0.5f) {
                npcEffect(app, 2, o.npc);
                npcEffect(app, 3, o.npc);
                show(app, o, false);
                app.sound.play(0x27);
                if (int* slot = cellSlot(app, o.layer, o.row, o.col); slot && *slot >= 0 && &app.puzzle.objs[size_t(*slot)] == &o)
                    *slot = -1;
                if (app.puzzle.esp >= 0) ++app.puzzle.esp; // incESPCount
                o.state = 0;
                return;
            }
        }
        if (o.pending == 8) {
            if (o.type == cr::kTypeNormal) npcEffect(app, 3, o.npc); // NormalBlockTP::onIdle
            fade(app, o.npc, 1, 0, o.outFrames);
            if (o.onSwitch) press(app, o.row, o.col, false);
            o.state = 2;
            o.pending = 0;
        }
        break;
    case 2: // onDisappear: then at its new cell
        if (--o.waitOut > 0) break;
        {
            const bool sw = switchAt(app, o.toRow, o.toCol);
            if (int* from = cellSlot(app, o.layer, o.row, o.col); from && *from >= 0 && &app.puzzle.objs[size_t(*from)] == &o) *from = -1;
            const int self = int(&o - app.puzzle.objs.data());
            o.row = o.toRow, o.col = o.toCol;
            if (int* to = cellSlot(app, o.layer, o.row, o.col)) *to = self; // replacePuzzleObject
            o.onSwitch = o.type == cr::kTypeNormal && sw;
            cr::Vec3 p = centre(app, o.row, o.col);
            p.y = cr::kPuzzleStage[app.puzzle.stage].y; // the room's height, raised on a switch
            if (sw) p.y += cr::kPuzzleSwitchRise;
            place(app, o, p);
            npcEffect(app, 4, o.npc); // onDisappear (the block's and the gem's)
            fade(app, o.npc, 0, 1, o.inFrames);
            o.state = 3;
        }
        break;
    case 3: // onAppear
        if (--o.waitIn > 0) break;
        if (o.onSwitch) press(app, o.row, o.col, true);
        o.state = 1;
        break;
    default: break;
    }
}

// The warp crystals: the gem one (WarpCrystalNG) once the gems are gone, the switch one
// (WarpCrystalWS) while every switch is pressed; the first time the crystal's NPC script plays
bool allCandlesLit(App& app);

void stepCrystal(App& app, App::Puzzle::Obj& o) {
    const unsigned bits = app.npcs[size_t(o.npc)].puzzle;
    bool ready = false;
    if (bits & cr::kPuzzleObject100) ready = !gemsLeft(app);
    else if (bits & cr::kPuzzleObject40) {
        ready = allSwitchesOn(app);
        if (!ready && o.shown) { // a switch let go: it goes again
            npcEffect(app, 3, o.npc);
            fade(app, o.npc, 1, 0, 4);
            show(app, o, false);
            app.sound.play(0x29);
            return;
        }
    } else if (bits & cr::kPuzzleObject800) {
        ready = allCandlesLit(app);
        if (!ready && o.shown) { // one gone out: it goes again
            npcEffect(app, 3, o.npc);
            fade(app, o.npc, 1, 0, 4);
            show(app, o, false);
            app.sound.play(0x29);
            return;
        }
    } else return;
    if (!ready || o.shown) return;
    if (!app.game.flag(3300 + app.puzzle.stage)) { // the scene
        if (app.runner.running()) return;
        app.game.setFlag(4000, true);
        const int evt = app.npcs[size_t(o.npc)].id;
        app.game.vars[1] = evt;
        app.runner.start(app.events, evt, app.game, app.area, app.mapNumber);
        app.runnerWhat = "puzzle crystal";
        return;
    }
    show(app, o, true);
    npcEffect(app, 4, o.npc);
    fade(app, o.npc, 0, 1, 4);
    app.sound.play(0x2a);
}

void punish(App& app, int t);

// Teleport::onCheckTap's choice made: swap places with object `t`
void teleportTo(App& app, int t) {
    App::Puzzle& pz = app.puzzle;
    if (pz.busy || pz.tpState || t < 0 || t >= int(pz.objs.size())) return;
    App::Puzzle::Obj& o = pz.objs[size_t(t)];
    if ((o.type != cr::kTypeNormal && o.type != cr::kTypeGem) || o.state != 1) return;
    int pr, pc;
    if (!cellOf(app, app.walker.pos, pr, pc)) return;
    if (pz.esp == 0) { punish(app, t); return; } // no ESP left - only the mark
    pz.busy = true;
    ++app.game.vars[13]; // decESPCount: var 13 counts the uses; the count stops at 0
    if (pz.esp > 0) --pz.esp;
    o.pending = 8; // TeleportMessage(the player's cell, 4, 4, 6, 6)
    o.toRow = pr, o.toCol = pc, o.outFrames = 4, o.inFrames = 4, o.waitOut = 6, o.waitIn = 6;
    app.playerAlphaFrom = 1, app.playerAlphaTo = 0, app.playerFadeFrames = app.playerFadeLeft = 4;
    npcEffect(app, 7, -1);
    npcEffect(app, 8, -1);
    app.sound.play(0x2b);
    pz.tpState = 1, pz.tpTimer = 6, pz.tpTarget = t;
}

void stepTeleport(App& app) {
    App::Puzzle& pz = app.puzzle;
    if (pz.tpState == 1 && --pz.tpTimer <= 0) { // onDisappear: to the block's (old) cell
        const App::Puzzle::Obj& o = pz.objs[size_t(pz.tpTarget)];
        setActor(app, 0, centre(app, o.row, o.col), nullptr, false);
        app.playerAlphaFrom = 0, app.playerAlphaTo = 1, app.playerFadeFrames = app.playerFadeLeft = 4;
        npcEffect(app, 9, -1);
        app.sound.play(0x2c);
        pz.tpState = 2, pz.tpTimer = 6;
    } else if (pz.tpState == 2 && --pz.tpTimer <= 0) { // onAppear
        pz.busy = false;
        pz.tpState = 0;
    }
}


// SetPunishmentDispOn: no ESP left for what was asked of object `t` - the mark (448, 120, 64 x 64)
// over the touch, blinking a frame in three for 11 frames, the count swelling, sound 4
void punish(App& app, int t) {
    App::Puzzle& pz = app.puzzle;
    float tx = pz.tapX, ty = pz.tapY;
    if (tx < 0 && t >= 0 && t < int(pz.objs.size())) { // the keyboard: over the object
        const cr::FieldNpc& n = app.npcs[size_t(pz.objs[size_t(t)].npc)];
        float lx, ly;
        if (fieldToLogical(app, {n.pos[0], n.pos[1] + 5.0f, n.pos[2]}, lx, ly)) logicalToTex2d(app, lx, ly, tx, ty);
    }
    if (tx < 0) tx = 240, ty = 160;
    pz.punishX = tx - 32, pz.punishY = ty - 32;
    pz.punished = 11, pz.punishPhase = 0;
    pz.pop = 4;
    app.sound.play(se::kBuzzer);
}

// ---- Psychokinesis (Psychokinesis, PsychokinesisDetector, NormalBlockPK) ----
// Touch a block: the leader holds still (effect 5), the block rises (sound 0x22, +2 a frame for 2
// frames, then 5 above its place) and bobs; a swipe up / down / left / right (past a dead zone of
// |dx| < 29, -35 < dy < 20 in the 480 x 320 screen) sends it along its row or column to the cell
// before the next block or wall (or the edge): sound 0x2d, 3 units a frame, then it falls (gravity
// 0.8, sound 0x23) or settles 2.5 up on a switch and presses it. A block that reaches the leader
// on its way squashes the try: effect 0x19, sound 0x54, the room resets. A slide costs one ESP
// (counted as the block lands); letting go without one costs nothing.

void reset(App& app);

bool crushes(App& app, const App::Puzzle::Obj& o) {
    const cr::FieldNpc& n = app.npcs[size_t(o.npc)];
    const cr::Vec3 p = app.walker.pos;
    return std::fabs(p.x - n.pos[0]) <= cr::kPuzzleCell * 0.5f && std::fabs(p.z - n.pos[2]) <= cr::kPuzzleCell * 0.5f;
}

void squash(App& app) {
    fieldEffect(app, 0x19, app.walker.pos + cr::Vec3{0, 5, 0});
    app.sound.play(0x54);
    reset(app);
}

void pkLanded(App& app) { // the block's message 4: Psychokinesis::onWaitMove
    App::Puzzle& pz = app.puzzle;
    if (pz.pkMoved) {
        ++app.game.vars[13];
        if (pz.esp > 0) --pz.esp;
    }
    pz.busy = false;
    pz.pkState = 0, pz.pkTarget = -1, pz.pkMoved = false;
    stopEffect(app.fieldEffects, pz.pkEffect); // eff3d_delete(5)
    pz.pkEffect = 0;
}

void stepPkBlock(App& app, App::Puzzle::Obj& o) {
    cr::FieldNpc& n = app.npcs[size_t(o.npc)];
    auto setY = [&](float y) { place(app, o, {n.pos[0], y, n.pos[2]}); };
    switch (o.pk) {
    case 0: // onIdle: taken hold of
        if (o.pkMsg != 1) break;
        app.sound.play(0x22);
        if (o.onSwitch) press(app, o.row, o.col, false);
        o.onSwitch = false;
        o.pkTimer = 2;
        o.pkMsg = 0;
        o.pk = 1;
        break;
    case 1: // onRise
        setY(n.pos[1] + 2.0f);
        if (--o.pkTimer > 0) break;
        setY(o.baseY + 5.0f);
        o.bob = 0;
        o.pk = 2;
        break;
    case 2: { // onWaitSlide: bobbing; dropped (2) or sent (3)
        o.bob = (o.bob + 9) % 360;
        setY(o.baseY + 5.0f + std::sin(float(o.bob) * 3.14159265f / 180.0f));
        if (o.pkMsg == 2) {
            o.pkMsg = 0;
            o.vy = 0;
            o.pk = switchAt(app, o.row, o.col) ? 5 : 4;
        } else if (o.pkMsg == 3) {
            o.pkMsg = 0;
            app.sound.play(0x2d);
            const cr::Vec3 from = centre(app, o.row, o.col), to = centre(app, o.toRow, o.toCol);
            const float dx = to.x - from.x, dz = to.z - from.z, d = std::sqrt(dx * dx + dz * dz);
            o.pkTimer = std::max(1, int(d / 3.0f));
            o.vx = d > 0 ? dx / d * 3.0f : 0, o.vz = d > 0 ? dz / d * 3.0f : 0;
            o.pk = 3;
        }
        break;
    }
    case 3: // onMove
        place(app, o, {n.pos[0] + o.vx, n.pos[1], n.pos[2] + o.vz});
        if (crushes(app, o)) { squash(app); return; }
        if (--o.pkTimer > 0) break;
        {
            if (int* from = cellSlot(app, o.layer, o.row, o.col); from && *from >= 0 && &app.puzzle.objs[size_t(*from)] == &o) *from = -1;
            const int self = int(&o - app.puzzle.objs.data());
            o.row = o.toRow, o.col = o.toCol;
            if (int* to = cellSlot(app, o.layer, o.row, o.col)) *to = self;
            const cr::Vec3 c = centre(app, o.row, o.col);
            place(app, o, {c.x, n.pos[1], c.z});
            startQuake(app.fieldQuake, 0.5f, 100, 0, 8, false); // Quake::start(0.5, 100 -> 0, 8 frames)
            o.vy = 0;
            o.pk = switchAt(app, o.row, o.col) ? 5 : 4;
        }
        break;
    case 4: case 5: { // onDown / onDownToSwitch
        const float floor = o.baseY + (o.pk == 5 ? cr::kPuzzleSwitchRise : 0.0f);
        float y = n.pos[1] + o.vy;
        o.vy -= 0.8f;
        if (crushes(app, o) && y > floor) { setY(y); squash(app); return; }
        if (y <= floor) {
            setY(floor);
            if (o.pk == 5) {
                o.onSwitch = true;
                press(app, o.row, o.col, true);
            } else app.sound.play(0x23);
            o.pk = 0;
            pkLanded(app);
        } else setY(y);
        break;
    }
    default: break;
    }
}

// Take hold of block `t` (onCheckTouch)
void pkGrab(App& app, int t, float sx = -1e9f, float sy = -1e9f) {
    App::Puzzle& pz = app.puzzle;
    if (pz.busy || pz.pkState || t < 0 || t >= int(pz.objs.size())) return;
    App::Puzzle::Obj& o = pz.objs[size_t(t)];
    int pr, pc;
    if (o.type != cr::kTypeNormal || o.state != 1 || o.pk != 0 || !cellOf(app, app.walker.pos, pr, pc)) return;
    if (pz.esp == 0) { punish(app, t); return; } // no ESP left - only the mark
    pz.busy = true;
    o.pkMsg = 1;
    pz.pkState = 1, pz.pkTarget = t, pz.pkMoved = false, pz.pkFromX = sx, pz.pkFromY = sy;
    pz.pkEffect = npcEffect(app, 5, -1, true);
}

// The swipe's direction (1 up the grid / row - 1, 2 down, 3 left / column - 1, 4 right); 0 lets go
void pkSend(App& app, int dir) {
    App::Puzzle& pz = app.puzzle;
    if (pz.pkState != 1 || pz.pkTarget < 0) return;
    App::Puzzle::Obj& o = pz.objs[size_t(pz.pkTarget)];
    if (o.pk != 2) return; // still rising
    const int dr = dir == 1 ? -1 : dir == 2 ? 1 : 0, dc = dir == 3 ? -1 : dir == 4 ? 1 : 0;
    int r = o.row, c = o.col;
    if (dir)
        for (;;) {
            const int nr = r + dr, nc = c + dc;
            if (!cellSlot(app, 1, nr, nc)) break;
            const int at = objectAt(app, 1, nr, nc);
            if (at >= 0) {
                const int t = pz.objs[size_t(at)].type;
                if (t == cr::kTypeNormal || t == cr::kTypeFixed) break;
            }
            r = nr, c = nc;
        }
    if (r == o.row && c == o.col) o.pkMsg = 2; // nowhere to go: let go
    else {
        o.toRow = r, o.toCol = c, o.pkMsg = 3; // PsychokinesisMessage
        pz.pkMoved = true;
    }
    pz.pkState = 2;
}

// The mouse let go at (sx, sy): the drag's direction, or none inside the dead zone
void pkRelease(App& app, float sx, float sy) {
    App::Puzzle& pz = app.puzzle;
    if (pz.pkState != 1 || pz.pkFromX < -1e8f) return;
    const float dx = (sx - pz.pkFromX) / 2.133f, dy = (sy - pz.pkFromY) / 2.4f; // to the 480 x 320 screen
    int dir = 0;
    if (!(dx > -29 && dx < 29 && dy > -35 && dy < 20)) {
        if (std::fabs(dx) >= std::fabs(dy)) dir = dx < 0 ? 3 : 4;
        else dir = dy < 0 ? 1 : 2;
    }
    pkSend(app, dir);
}

// ---- Toughness (Toughness, FragileBlock, RedBlock, BlueBlock) ----
// A tap on a fragile, red or blue block that's there (isValid) costs one ESP (none left: only the
// mark) - effect 7 on the leader, sound 0x2f - and sends message 9: to the fragile block alone (it
// breaks: effect 10, sound 0x29, off the grid), or to every object on the block layer, where each
// red and blue block changes over: one put away goes solid again (alpha 0x60 to 1 in 4 frames,
// effect 4, sound 0x2a - once a frame, NoOverlapSE), squashing the try when the leader stands in
// its box (effect 0x19, sound 0x54, the room resets); a solid one is put away (1 to 0x60, effect
// 3, sound 0x29). Red blocks start solid, blue ones put away.

void reset(App& app);

bool inBox(App& app, const cr::FieldNpc& n) { // the leader inside the NPC's box (x, z)
    const cr::Vec3 p = app.walker.pos;
    return std::fabs(p.x - (n.pos[0] + n.offset[0])) <= n.box[0] * 0.5f &&
           std::fabs(p.z - (n.pos[2] + n.offset[2])) <= n.box[2] * 0.5f;
}

void toughTap(App& app, int t) {
    App::Puzzle& pz = app.puzzle;
    if (pz.busy || t < 0 || t >= int(pz.objs.size())) return;
    App::Puzzle::Obj& o = pz.objs[size_t(t)];
    int pr, pc;
    if ((o.type != cr::kTypeFragile && o.type != cr::kTypeRed && o.type != cr::kTypeBlue) || !o.shown ||
        !cellOf(app, app.walker.pos, pr, pc))
        return;
    if (pz.esp == 0) { punish(app, t); return; } // no ESP left - only the mark
    ++app.game.vars[13]; // decESPCount
    if (pz.esp > 0) --pz.esp;
    npcEffect(app, 7, -1);
    app.sound.play(0x2f);
    if (o.type == cr::kTypeFragile) o.hit = true;
    else
        for (App::Puzzle::Obj& b : pz.objs) b.hit |= b.layer == 1; // postMessage(1, 9)
}

void stepTough(App& app) {
    App::Puzzle& pz = app.puzzle;
    bool born = false, killed = false, rebuild = false;
    for (App::Puzzle::Obj& o : pz.objs) {
        if (!o.hit) continue;
        o.hit = false;
        const cr::FieldNpc& n = app.npcs[size_t(o.npc)];
        if (o.type == cr::kTypeFragile) {
            if (!o.shown) continue;
            o.shown = false; // npc_kill, removePuzzleObject
            npcEffect(app, 10, o.npc);
            if (int* slot = cellSlot(app, o.layer, o.row, o.col); slot && *slot == int(&o - pz.objs.data())) *slot = -1;
            app.sound.play(0x29);
            rebuild = true;
        } else if (o.type == cr::kTypeRed || o.type == cr::kTypeBlue || o.type == cr::kTypeCandle) {
            if (!o.shown) {
                o.shown = true, o.ghost = false;
                born = rebuild = true;
                npcEffect(app, 4, o.npc);
                if (inBox(app, n)) { // came in on the leader
                    buildNpcDraws(app);
                    fade(app, o.npc, kGhostAlpha, 1, 4);
                    app.sound.play(0x2a);
                    fieldEffect(app, 0x19, app.walker.pos + cr::Vec3{0, 5, 0});
                    app.sound.play(0x54);
                    reset(app);
                    return;
                }
            } else {
                o.shown = false, o.ghost = true;
                killed = rebuild = true;
                npcEffect(app, 3, o.npc);
            }
        }
    }
    if (!rebuild) return;
    buildNpcDraws(app);
    buildWalkCollision(app);
    if (born) app.sound.play(0x2a);
    if (killed) app.sound.play(0x29);
    for (const App::Puzzle::Obj& o : pz.objs) {
        if (o.type != cr::kTypeRed && o.type != cr::kTypeBlue && o.type != cr::kTypeCandle) continue;
        const App::NpcDraw* d = drawOf(app, o.npc);
        if (!d) continue;
        const bool wasGhost = d->alphaTo < 0.5f;
        if (o.ghost && !wasGhost) fade(app, o.npc, 1, kGhostAlpha, 4);
        else if (!o.ghost && wasGhost) fade(app, o.npc, kGhostAlpha, 1, 4);
    }
}

// ---- Pyrokinesis (Pyrokinesis, CandleSwitch, CandleBlock, WarpCrystalCS) ----
// A tap on a candle switch (within the window; none left: only the mark) costs one ESP and sends it
// message 10. Out (0): it lights - effect 0xd, sound 0x31, SendMessage, 15 frames, then lit (3,
// flame effect 0xc). Lit: a tap puts it out - sound 0x32, the flame off, effect 0xe, SendMessage,
// 20 frames cooling (5), then out. SendMessage: in stages 48..55, message 12 with the switch's id
// to the block layer - each candle block whose +24 is that id changes over (as a red block; +22 =
// lit at the start); from stage 56, message 11 to the objects at the cells of the (up to four)
// NPCs listed at the switch's +22: a switch out catches (5 frames, effect 0xd, sound 0x31, lit 15
// later), a lit one goes out (5 frames, then as a tap) - neither passes it on - and draw3D runs a
// red beam (0x600000ff, 1.5 wide) from the sender for 20 frames. A message waits while a switch is
// lighting or cooling. WarpCrystalCS comes once every switch on the grid is lit and goes again
// when one isn't.

void sendCandle(App& app, App::Puzzle::Obj& sw) { // (anonymous)::SendMessage
    App::Puzzle& pz = app.puzzle;
    const cr::FieldNpc& n = app.npcs[size_t(sw.npc)];
    if (pz.stage < 56) {
        for (App::Puzzle::Obj& b : pz.objs)
            if (b.type == cr::kTypeCandle && app.npcs[size_t(b.npc)].link[1] == n.id) b.hit = true;
        return;
    }
    for (int k = 0; k < 4 && n.link[k] != 0; ++k)
        for (size_t i = 0; i < app.npcs.size(); ++i) {
            if (app.npcs[i].id != n.link[k]) continue;
            int r, c;
            if (cellOf(app, {app.npcs[i].pos[0], app.npcs[i].pos[1], app.npcs[i].pos[2]}, r, c)) {
                const int t = objectAt(app, 1, r, c);
                if (t >= 0 && pz.objs[size_t(t)].type == cr::kTypeCandleSwitch) {
                    App::Puzzle::Obj& to = pz.objs[size_t(t)];
                    to.csMsg = 11, to.csFrom = sw.npc, to.beamT = 0;
                }
            }
            break; // _GetNpcCtl: the first with that id
        }
}

void stepCandleSwitch(App& app, App::Puzzle::Obj& o, bool& lightSE, bool& outSE) {
    if (o.csFrom >= 0 && ++o.beamT > 20) o.csFrom = -1;
    const cr::FieldNpc& n = app.npcs[size_t(o.npc)];
    const cr::Vec3 top{n.pos[0], n.pos[1] + 4.5f, n.pos[2]}; // the effects' point
    auto goOut = [&] { // the flame off, effect 0xe
        stopEffect(app.fieldEffects, o.flame);
        o.flame = 0;
        fieldEffect(app, 0xe, top);
    };
    switch (o.cs) {
    case 0:
        if (o.csMsg == 11) o.csTimer = 5, o.cs = 1;
        else if (o.csMsg == 10) {
            fieldEffect(app, 0xd, top);
            app.sound.play(0x31);
            sendCandle(app, o);
            o.csTimer = 15, o.cs = 2;
        }
        o.csMsg = 0;
        break;
    case 1: // onIgnitionDelay
        if (--o.csTimer < 1) { fieldEffect(app, 0xd, top); lightSE = true; o.csTimer = 15, o.cs = 2; }
        break;
    case 2:
        if (--o.csTimer < 1) { o.flame = fieldEffect(app, 0xc, top, true); o.cs = 3; }
        break;
    case 3: // onLighted
        if (o.csMsg == 11) o.csTimer = 5, o.cs = 4;
        else if (o.csMsg == 10) {
            outSE = true;
            goOut();
            sendCandle(app, o);
            o.csTimer = 20, o.cs = 5;
        }
        o.csMsg = 0;
        break;
    case 4: // onExtinguishDelay
        if (--o.csTimer < 1) { outSE = true; goOut(); o.csTimer = 20, o.cs = 5; }
        break;
    case 5:
        if (--o.csTimer < 1) o.cs = 0;
        break;
    default: break;
    }
}

bool allCandlesLit(App& app) {
    for (const App::Puzzle::Obj& o : app.puzzle.objs)
        if (o.type == cr::kTypeCandleSwitch && objectAt(app, 1, o.row, o.col) == int(&o - app.puzzle.objs.data()) && o.cs != 3)
            return false;
    return true;
}

void pyroTap(App& app, int t) {
    App::Puzzle& pz = app.puzzle;
    if (t < 0 || t >= int(pz.objs.size())) return;
    App::Puzzle::Obj& o = pz.objs[size_t(t)];
    int pr, pc;
    if (o.type != cr::kTypeCandleSwitch || !cellOf(app, app.walker.pos, pr, pc)) return;
    if (pz.esp == 0) { punish(app, t); return; } // no ESP left - only the mark
    ++app.game.vars[13]; // decESPCount
    if (pz.esp > 0) --pz.esp;
    o.csMsg = 10;
}

void stepPyro(App& app) {
    bool lightSE = false, outSE = false;
    for (App::Puzzle::Obj& o : app.puzzle.objs)
        if (o.type == cr::kTypeCandleSwitch) stepCandleSwitch(app, o, lightSE, outSE);
    if (lightSE) app.sound.play(0x31); // NoOverlapSE
    if (outSE) app.sound.play(0x32);
}

// PuzzleScene: a reset fades out (sound 0x44), builds the room again, puts the leader back where
// it came in, fades in
void reset(App& app) {
    App::Puzzle& pz = app.puzzle;
    if (pz.stage < 0 || pz.scene != 4) return;
    app.sound.play(0x44);
    app.fadeGrey = 0, app.fadeFrom = 0, app.fadeTo = 1, app.fadeFrames = 8, app.fadeT = 0;
    pz.scene = 2;
}

// One 30 Hz frame (PuzzleManager::update, under PuzzleScene::update)
void step(App& app) {
    App::Puzzle& pz = app.puzzle;
    if (pz.stage < 0) return;
    if (pz.scene == 2) { // fading out to the reset
        if (app.fadeT < app.fadeFrames) return;
        const cr::Vec3 start = app.puzzleStart;
        const int stageNo = pz.stage;
        app.npcs = app.events.npcs(app.mapNumber); // every object back where the room has it
        enter(app);
        if (app.puzzle.stage == stageNo) setActor(app, 0, start, nullptr, false);
        app.fadeFrom = 1, app.fadeTo = 0, app.fadeFrames = 8, app.fadeT = 0;
        app.puzzle.scene = 3;
        return;
    }
    if (pz.scene == 3) {
        if (app.fadeT >= app.fadeFrames) pz.scene = 4;
        return;
    }
    if (cleared(app) || app.runner.running() || !app.dialogue.empty()) return;
    for (int k = 0; k < 2; ++k) // the buttons' scripts done: retry (var 0 = 1) / the other (var 1 = 666)
        if (pz.asked[k]) {
            pz.asked[k] = false;
            if (k == 0 && app.game.vars[0] == 1) { reset(app); return; }
        }
    ++pz.clock;
    stepTeleport(app);
    if (stage(app)->ability == cr::kAbilityPyrokinesis) stepPyro(app);
    if (stage(app)->ability == cr::kAbilityToughness || stage(app)->ability == cr::kAbilityPyrokinesis) {
        stepTough(app); // the red / blue and candle blocks' changing over
        if (pz.scene != 4) return; // squashed: the room resets
    }
    const bool pk = stage(app)->ability == cr::kAbilityPsychokinesis;
    for (size_t i = 0; i < pz.objs.size(); ++i) {
        App::Puzzle::Obj& o = pz.objs[i];
        if (pk && o.type == cr::kTypeNormal) {
            stepPkBlock(app, o);
            if (app.puzzle.scene != 4) return; // squashed: the room resets
        } else if (o.type == cr::kTypeNormal || o.type == cr::kTypeGem) stepBlock(app, o);
        else if (o.type == cr::kTypeCrystal) stepCrystal(app, o);
    }
}

// The pad's buttons (EvrStart 6: retry? / 7)
void ask(App& app, int k) {
    App::Puzzle& pz = app.puzzle;
    if (pz.stage < 0 || pz.busy || app.runner.running()) return;
    const uint32_t at = app.events.returnEntry(6 + k);
    if (!at || !app.runner.startAt(app.events, at, 6 + k, app.game, app.area, app.mapNumber)) return;
    app.runnerWhat = k == 0 ? "puzzle retry" : "puzzle button 7";
    pz.asked[k] = true;
}

// What the room's ability works on: teleport - a block or gem in play; psychokinesis - a block;
// toughness - a fragile, red or blue block that's there
bool target(const App& app, const App::Puzzle::Obj& o) {
    const cr::PuzzleStage* st = stage(app);
    if (!st || o.layer != 1 || !o.shown) return false;
    switch (st->ability) {
    case cr::kAbilityTeleport: return (o.type == cr::kTypeNormal || o.type == cr::kTypeGem) && o.state == 1;
    case cr::kAbilityPsychokinesis: return o.type == cr::kTypeNormal && o.state == 1;
    case cr::kAbilityToughness: return o.type == cr::kTypeFragile || o.type == cr::kTypeRed || o.type == cr::kTypeBlue;
    case cr::kAbilityPyrokinesis: return o.type == cr::kTypeCandleSwitch;
    default: return false;
    }
}

// A tap at screen point (sx, sy) in the field view's pixels: the nearest object whose projected
// top is within reach (getNearestPuzzleObject + the abilities' window)
int pick(App& app, float sx, float sy, const std::function<bool(cr::Vec3, float&, float&)>& project) {
    int best = -1;
    float bestD = 1e30f;
    for (size_t i = 0; i < app.puzzle.objs.size(); ++i) {
        const App::Puzzle::Obj& o = app.puzzle.objs[i];
        if (o.layer != 1 || !o.shown) continue;
        const cr::FieldNpc& n = app.npcs[size_t(o.npc)];
        float x, y;
        if (!project({n.pos[0], n.pos[1] + 5.0f, n.pos[2]}, x, y)) continue;
        const float d = (x - sx) * (x - sx) + (y - sy) * (y - sy);
        if (d < bestD) bestD = d, best = int(i);
    }
    // ~ the original's 20 tex2d units, in 1024 x 768 pixels; the nearest has to be one it works on
    return bestD <= 48.0f * 48.0f && target(app, app.puzzle.objs[size_t(best)]) ? best : -1;
}

// The keyboard's way: the first object straight ahead of the leader along its row / column (past
// put-away blocks), if the ability works on it
int ahead(App& app) {
    int r, c;
    if (!cellOf(app, app.walker.pos, r, c)) return -1;
    const float h = app.walker.heading;
    const float dx = std::sin(h), dz = std::cos(h);
    const int sr = std::fabs(dz) >= std::fabs(dx) ? (dz > 0 ? 1 : -1) : 0, sc = sr ? 0 : (dx > 0 ? 1 : -1);
    for (int k = 1; k < 12; ++k) {
        const int o = objectAt(app, 1, r + sr * k, c + sc * k);
        if (o < 0 || !app.puzzle.objs[size_t(o)].shown) continue;
        return target(app, app.puzzle.objs[size_t(o)]) ? o : -1;
    }
    return -1;
}

// Uty_PuzzleStart_Proc: flag 0 ends it; var 0 = 0 slides the pieces 8 apart a frame and fades
// them in (alpha + 0x33), 1 puts them in place (60 / 196, opaque), 2 slides them on, fading out.
// The puzzle area's script 0 runs it: 5 frames of 0, 30 of 1, 5 of 2, then flag 0 (flag 1 picks
// the second piece: the start (sound 0x43) or the other banner (sound 0x45))
void stepBanner(App& app) {
    App::PuzzleBanner& b = app.puzzleBanner;
    if (!b.on) return;
    if (app.game.flag(0)) { b.on = false; return; }
    const int v = app.game.vars[0];
    if (v == 0) b.x1 -= 8, b.x2 += 8, b.alpha = std::min(255, b.alpha + 0x33);
    else if (v == 1) b.x1 = 60, b.x2 = 196, b.alpha = 255;
    else if (v == 2) b.x1 -= 8, b.x2 += 8, b.alpha = std::max(0, b.alpha - 0x33);
}

// Uty_WarpCrystalCamera_Proc: the view slides to the crystal (0.25 a frame); there it comes in
// (fade, sound 0x2a) and var 0 = 1; once the script sets 2 the view slides back to the leader,
// var 0 = 3 and the camera is the leader's again
void stepCrystalCam(App& app) {
    App::CrystalCam& cc = app.crystalCam;
    if (cc.npc < 0) return;
    const cr::FieldNpc* n = nullptr;
    int index = -1;
    for (size_t i = 0; i < app.npcs.size(); ++i)
        if (app.npcs[i].id == cc.npc) n = &app.npcs[i], index = int(i);
    if (!n) { cc.npc = -1; app.cam.free = false; return; }
    const cr::Vec3 at = cc.phase == 0 ? cr::Vec3{n->pos[0], n->pos[1], n->pos[2]}
                                      : app.walker.pos + cr::Vec3{0, 7.0f + float(app.stageCam.lookHeight), 0};
    const float ox = app.scroll.offX, oy = app.scroll.offY;
    float vx, vy;
    if (scrollingField(app) && fieldViewPoint(app, at, vx, vy)) scrollFieldTo(app, vx, vy, 0.25f);
    const bool settled = std::fabs(app.scroll.offX - ox) < 0.5f && std::fabs(app.scroll.offY - oy) < 0.5f;
    if (!settled) return;
    if (cc.phase == 0 && app.game.vars[0] == 0) { // there: the crystal comes
        for (App::Puzzle::Obj& o : app.puzzle.objs)
            if (o.npc == index) {
                show(app, o, true);
                npcEffect(app, 4, o.npc);
                fade(app, o.npc, 0, 1, 4);
            }
        app.sound.play(0x2a);
        app.game.vars[0] = 1;
    } else if (cc.phase == 0 && app.game.vars[0] == 2) cc.phase = 1;
    else if (cc.phase == 1) {
        app.game.vars[0] = 3;
        cc.npc = -1;
        app.cam.free = false; // SetPlayerCamera
    }
}

// A click in the frame's pixels: teleport to the block or gem tapped (true when one was)
bool click(App& app, float px, float py) {
    if (app.puzzle.stage < 0 || app.runner.running() || !app.dialogue.empty() || !paintedField(app)) return false;
    float lx, ly;
    app.renderer.toLogical(px, py, lx, ly);
    float tx, ty;
    logicalToTex2d(app, lx, ly, tx, ty);
    if (app.puzzle.help) { // the pad's buttons: (8..52, 56..84) Retry (EvrStart 6), (84..124, 14..46) Help (7)
        const float bx = tx + (app.renderer.uiX() - wideLeft(app)) / 2; // (widescreen: in the corner)
        if (bx >= 8 && bx < 52 && ty >= 56 && ty < 84) { ask(app, 0); return true; }
        if (bx >= 84 && bx < 124 && ty >= 14 && ty < 46) { ask(app, 1); return true; }
    }
    const float SW = app.renderer.screenW(), H = cr::Renderer::kLogicalH;
    float vx, vy, vw, vh; // the stage view on screen, as drawFrame lays it out
    if (scrollingField(app)) {
        vw = fieldViewW(app), vh = fieldViewH(app);
        vx = (SW - vw) / 2 + app.scroll.offX, vy = (H - vh) / 2 - app.scroll.offY;
    } else {
        vw = vh = app.video.paintings == 2 ? std::max(SW, H) : std::min(SW, H);
        vx = (SW - vw) / 2, vy = (H - vh) / 2;
    }
    const int t = pick(app, lx, ly, [&](cr::Vec3 p, float& x, float& y) {
        float hx, hy;
        if (!fieldViewPoint(app, p, hx, hy)) return false;
        x = vx + hx * vw / fieldViewW(app), y = vy + hy * vh / fieldViewH(app);
        return true;
    });
    if (t < 0) return false;
    app.puzzle.tapX = tx, app.puzzle.tapY = ty;
    if (stage(app)->ability == cr::kAbilityPsychokinesis) pkGrab(app, t, lx, ly);
    else if (stage(app)->ability == cr::kAbilityToughness) toughTap(app, t);
    else if (stage(app)->ability == cr::kAbilityPyrokinesis) pyroTap(app, t);
    else teleportTo(app, t);
    app.puzzle.tapX = app.puzzle.tapY = -1;
    return true;
}

// The mouse let go (the frame's pixels): a held block goes the way it was dragged
void release(App& app, float px, float py) {
    if (app.puzzle.stage < 0 || app.puzzle.pkState != 1) return;
    float lx, ly;
    app.renderer.toLogical(px, py, lx, ly);
    pkRelease(app, lx, ly);
}

// The ability's key: teleport to / take hold of the block ahead, or let a held one go
void abilityKey(App& app) {
    App::Puzzle& pz = app.puzzle;
    const cr::PuzzleStage* st = stage(app);
    if (!st) return;
    if (st->ability == cr::kAbilityPsychokinesis) {
        if (pz.pkState == 1) pkSend(app, 0);
        else pkGrab(app, ahead(app));
    } else if (st->ability == cr::kAbilityToughness) toughTap(app, ahead(app));
    else if (st->ability == cr::kAbilityPyrokinesis) pyroTap(app, ahead(app));
    else teleportTo(app, ahead(app));
}


// CandleSwitch::draw3D: the beam from a switch that passed a light on (message 11) - 1.5 wide,
// colour 0x600000ff, its head reaching the switch in 4 frames, its tail following from frame 8 to 12
void drawFx(App& app, const cr::Mat4& mvp) {
    App::Puzzle& pz = app.puzzle;
    const cr::PuzzleStage* st = stage(app);
    if (!st || st->ability != cr::kAbilityPyrokinesis) return;
    constexpr int kQuads = 64;
    auto make = [&](cr::GpuMesh& m) {
        if (m.vao) return;
        cr::Mesh mesh;
        mesh.positions.assign(kQuads * 4 * 3, 0.0f);
        mesh.uvs.assign(kQuads * 4 * 2, 0.0f);
        mesh.colors.assign(kQuads * 4 * 4, 0);
        for (uint32_t q = 0; q < kQuads; ++q)
            for (uint32_t k : {0u, 1u, 2u, 0u, 2u, 3u}) mesh.indices.push_back(q * 4 + k);
        m = app.renderer.upload(mesh);
    };
    make(app.puzzleBeams);
    struct Quads {
        std::vector<float> pos, uv;
        std::vector<uint8_t> col;
        int n = 0;
        void add(const cr::Vec3 (&v)[4], const uint8_t (&c)[4][4]) {
            if (n >= kQuads) return;
            for (int k = 0; k < 4; ++k) {
                pos.insert(pos.end(), {v[k].x, v[k].y, v[k].z});
                col.insert(col.end(), c[k], c[k] + 4);
            }
            ++n;
        }
        void finish() {
            pos.resize(kQuads * 4 * 3, 0.0f);
            col.resize(kQuads * 4 * 4, 0);
            uv.assign(kQuads * 4 * 2, 0.0f);
        }
    } beams;
    for (const App::Puzzle::Obj& o : pz.objs) {
        if (o.type != cr::kTypeCandleSwitch || !o.shown) continue;
        const cr::FieldNpc& n = app.npcs[size_t(o.npc)];
        if (o.csFrom >= 0 && o.beamT <= 20) {
            const cr::FieldNpc& f = app.npcs[size_t(o.csFrom)];
            const float dx = n.pos[0] - f.pos[0], dz = n.pos[2] - f.pos[2], dist = std::sqrt(dx * dx + dz * dz);
            const float head = std::clamp(float(o.beamT) * 2.5f, 0.0f, 10.0f) / 10.0f * dist;
            const float tail = std::clamp(float(o.beamT) * 2.5f - 20.0f, 0.0f, 10.0f) / 10.0f * dist;
            if (head > tail + 0.01f) {
                const float ang = std::atan2(dx, dz), cs = std::cos(ang), sn = std::sin(ang), y = n.pos[1] + 0.3f;
                auto at = [&](float lx, float lz) { return cr::Vec3{f.pos[0] + lx * cs + lz * sn, y, f.pos[2] + lz * cs - lx * sn}; };
                const cr::Vec3 v[4] = {at(-0.75f, tail), at(0.75f, tail), at(0.75f, head), at(-0.75f, head)};
                const uint8_t c[4][4] = {{255, 0, 0, 0x60}, {255, 0, 0, 0x60}, {255, 0, 0, 0x60}, {255, 0, 0, 0x60}};
                beams.add(v, c);
            }
        }
    }
    beams.finish();
    app.renderer.updatePositions(app.puzzleBeams, beams.pos, beams.uv, beams.col);
    cr::Material m; // untextured, double-sided, depth tested, no depth write
    m.culling = 0;
    m.properties = 8;
    m.blendSrc = 4, m.blendDst = 5; // alpha
    if (beams.n) app.renderer.drawMesh(app.puzzleBeams, nullptr, mvp, m);
}

} // namespace puzzle

void stepEvents(App& app) {
    if (app.skipScene) { // fast-forward: every page turns at once (choices still wait for an answer)
        if (!app.runner.running()) app.skipScene = false;
        else if (!app.dialogue.empty()) advanceDialogue(app);
    }
    stepTyping(app);
    if (app.titleOpen) ++app.titleAnim; // the title menu fades in
    if (app.choiceFlag >= 0 && app.choiceAnim < 4) ++app.choiceAnim;
    updateVoices(app);
    if (!app.battle.active) stepEffects(app, app.fieldEffects), stepQuake(app.fieldQuake); // eff3d_prc, UpdateQuake
    if (app.nowLoading.on) app.nowLoading.frame = (app.nowLoading.frame + 1) & 15; // NowLoadProc
    stepTexDisp(app);
    stepVitaLogo(app);
    stepTutoTex(app);
    if (app.fadeT < app.fadeFrames) ++app.fadeT;
    for (auto& d : app.npcDraws)
        if (d.fadeLeft > 0) {
            --d.fadeLeft;
            d.alpha = d.alphaTo + (d.alphaFrom - d.alphaTo) * float(d.fadeLeft) / float(d.fadeFrames);
        }
    if (app.playerFadeLeft > 0) {
        --app.playerFadeLeft;
        app.playerAlpha = app.playerAlphaTo + (app.playerAlphaFrom - app.playerAlphaTo) * float(app.playerFadeLeft) /
                                                  float(std::max(1, app.playerFadeFrames));
    }
    if (app.puzzleStartPending && app.havePlayer) app.puzzleStart = app.walker.pos, app.puzzleStartPending = false;
    puzzle::step(app);
    puzzle::stepCrystalCam(app);
    puzzle::stepBanner(app);
    // Script moves (npc_autoctl_prc): straight lines, y on the floor unless mode 2
    for (size_t i = 0; i < app.moves.size();) {
        App::Move& m = app.moves[i];
        ++m.t;
        const float t = float(m.t) / float(m.frames);
        const cr::Vec3 p{m.from.x + (m.to.x - m.from.x) * t, m.from.y + (m.to.y - m.from.y) * t,
                         m.from.z + (m.to.z - m.from.z) * t};
        const float yaw = m.yawFrom + (m.yawTo - m.yawFrom) * t;
        setActor(app, m.npc, p, m.turn ? &yaw : nullptr, m.mode == 2);
        if (m.t >= m.frames) app.moves.erase(app.moves.begin() + long(i));
        else ++i;
    }
    stepFieldScroll(app);
    stepSceneEffects(app);
    if (app.cam.t < app.cam.frames) { // camera moves are straight lines
        App::ScriptCam& c = app.cam;
        ++c.t;
        const float t = float(c.t) / float(c.frames);
        c.eye = c.eyeFrom + (c.eyeTo - c.eyeFrom) * t;
        c.target = c.tgtFrom + (c.tgtTo - c.tgtFrom) * t;
    }
    for (App::Portrait& p : app.portraits) // Tex2d slides and fades (tex2d_img_prc)
    {
        if (p.t < p.frames) {
            ++p.t;
            const float t = float(p.t) / float(p.frames);
            p.x = p.xFrom + (p.xTo - p.xFrom) * t;
            p.y = p.yFrom + (p.yTo - p.yFrom) * t;
        }
        if (p.aT < p.aFrames) {
            ++p.aT;
            p.alpha = p.aFrom + (p.aTo - p.aFrom) * float(p.aT) / float(p.aFrames);
        }
        if (p.gT < p.gFrames) {
            ++p.gT;
            p.shade = p.gFrom + (p.gTo - p.gFrom) * float(p.gT) / float(p.gFrames);
        }
        if (p.dying && p.t >= p.frames && p.aT >= p.aFrames) {
            app.renderer.destroy(p.tex);
            p = App::Portrait();
        }
        if (p.zoomT < p.zoomFrames) {
            ++p.zoomT;
            p.scale = 2.0f - float(p.zoomT) / float(p.zoomFrames);
        }
    }
    // The item window closes once the script raises flag 0 after the tap, and clears it
    if (app.itemWindowOpen && app.game.flag(0)) { app.itemWindowOpen = false; app.game.setFlag(0, false); }
    if (!app.runner.running() && app.pendingEntry != App::Entry::None) {
        const bool area = app.pendingEntry == App::Entry::Area;
        app.pendingEntry = App::Entry::None;
        const bool started = area ? app.runner.start(app.events, 0, app.game, app.area, app.mapNumber)
                                  : app.runner.startAt(app.events, app.events.returnEntry(5), 0, app.game, app.area,
                                                       app.mapNumber);
        if (started) {
            app.runnerWhat = area ? "area entry script" : "room entry script";
            SDL_Log("%s: area %d map %d", app.runnerWhat.c_str(), app.area, app.mapNumber);
        }
    }
    if (!app.runner.running()) return;
    ViewerHost host(app);
    const bool more = app.runner.step(&host);
    if (app.battle.requested) { // G+0x2c0 bit 7: the field gives way to the battle
        app.battle.requested = false;
        // btl_main: a script still running (G+0x2c0 bit 0) makes it an event battle (G+0xa4e) - the
        // script waits behind the battle's gate and goes on later; else nothing's left of it
        const bool event = more && app.runner.running();
        if (!event) app.runner.stop();
        startBattle(app, app.battle.request, app.battle.requestFlags);
        app.battle.eventBattle = event && app.battle.active;
        return;
    }
    if (more) return;
    // Finished: report, and refresh what the script's flags changed (an opened box loses its lid)
    trackerSceneEnd(app);
    const cr::EventResult& r = app.runner.result();
    if (!r.error.empty()) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "event: %s", r.error.c_str());
    if (!app.runnerWhat.empty()) {
        app.lastEvent = app.runnerWhat;
        for (int item : r.itemsGained) {
            std::vector<std::vector<std::string>> name;
            app.lastEvent += ", got " + itemName(app, item);
        }
        SDL_Log("%s, %d instructions, %zu messages", app.lastEvent.c_str(), r.instructions, r.messages.size());
    }
    app.itemWindowOpen = false;
    app.tapPending = false;
    buildNpcDraws(app);
    buildWalkCollision(app);
    updateTitle(app);
}

// fldencount_walk, for a step of `dist` units: the encounter distance counts unless encounters
// are off (flag 4076, the pair's unencountflg); with skill 174 on either member every 10 units
// banks 1 EXP for the pair (to 100000; one a step at most)
void fieldWalked(App& app, float dist) {
    if (!app.noEncounters && !fieldEncountersOff(app)) app.encounters.walked(dist);
    const int pr = fieldRoute(app) * 2;
    if (!cr::hasSkill(app.game, pr, 174) && !cr::hasSkill(app.game, pr + 1, 174)) return;
    app.walkExpDistance += dist;
    if (app.walkExpDistance >= 10) {
        int& bank = app.game.walkExp[size_t(fieldRoute(app))];
        bank = std::min(100000, bank + 1);
        app.walkExpDistance -= 10;
    }
}

void playFootsteps(App& app) {
    // PlayFootstepSE: a step sound when the walk / run animation passes one of its two step
    // frames, chosen (one of three) by the colour of the floor polygon underfoot, and effect 0 (dust)
    if (app.walker.gait != cr::Gait::Stand) {
        const int route = std::clamp(app.playerChar / 2, 0, 3);
        const short* frames = app.walker.gait == cr::Gait::Run ? cr::kFootstepFrameRun[route] : cr::kFootstepFrameWalk[route];
        const float prev = app.footPrev, now = app.player.time;
        for (int k = 0; k < 2; ++k) {
            const float f = frames[k];
            const bool crossed = prev <= now ? (prev < f && f <= now) : (f > prev || f <= now); // wrapped
            if (!crossed) continue;
            float y;
            int poly = -1;
            if (!cr::floorHeight(app.collisionData, app.walker.pos.x, app.walker.pos.y + 1, app.walker.pos.z, 2.0f, y, &poly))
                break;
            static const uint8_t kSurface[5][3] = {{254, 0, 0}, {175, 175, 175}, {125, 185, 175}, {205, 220, 230}, {140, 190, 190}};
            const uint8_t* c = app.collisionData.polygons[size_t(poly)].color;
            for (int t = 0; t < 5; ++t)
                if (c[0] == kSurface[t][0] && c[1] == kSurface[t][1] && c[2] == kSurface[t][2])
                {
                    npcEffect(app, 0, -1); // the dust (eff3d_set_fld 0 on the leader)
                    const uint32_t id = uint32_t(cr::kFootstepSound[t][SDL_rand(3)]);
                    app.sound.play(id);
                    app.soundLog.push_back(id);
                }
        }
    }
    app.footPrev = app.player.time;
}

// WASD moves the leader relative to the camera (camYaw), as the original's cross key does.
void updatePlayer(App& app, float dt, float camYaw) {
    if (!app.havePlayer) return;
    const bool* keys = SDL_GetKeyboardState(nullptr);
    // A running script, its dialogue box or item window hold the player in place
    const bool talking = !app.dialogue.empty() || app.runner.running() || app.itemWindowOpen || app.optionsOpen ||
                         app.campOpen || app.saveScreen.open || app.shop.open || app.mapScreen.open ||
                         app.puzzle.tpState != 0 || app.puzzle.pkState != 0 ||
                         (app.puzzle.stage >= 0 && app.puzzle.scene != 4);
    float sx = talking ? 0.0f : float(keys[SDL_SCANCODE_D]) - float(keys[SDL_SCANCODE_A]);
    float sy = talking ? 0.0f : float(keys[SDL_SCANCODE_S]) - float(keys[SDL_SCANCODE_W]);
    bool run = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
    if (app.pad && !talking && sx == 0 && sy == 0) { // a controller: the left stick (pushed far: runs) or the D-pad
        float lx = float(SDL_GetGamepadAxis(app.pad, SDL_GAMEPAD_AXIS_LEFTX)) / 32767.0f;
        float ly = float(SDL_GetGamepadAxis(app.pad, SDL_GAMEPAD_AXIS_LEFTY)) / 32767.0f;
        const float mag = std::sqrt(lx * lx + ly * ly);
        if (mag < 0.25f) lx = ly = 0; // (the dead zone)
        else run = run || mag > 0.8f;
        lx += float(SDL_GetGamepadButton(app.pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) - float(SDL_GetGamepadButton(app.pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT));
        ly += float(SDL_GetGamepadButton(app.pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) - float(SDL_GetGamepadButton(app.pad, SDL_GAMEPAD_BUTTON_DPAD_UP));
        sx = std::clamp(lx, -1.0f, 1.0f), sy = std::clamp(ly, -1.0f, 1.0f);
    }
    constexpr float kStep = 1.0f / kFramesPerSecond;
    app.walkClock += dt;
    while (app.walkClock >= kStep) {
        app.walkClock -= kStep;
        stepEvents(app);
        if (app.battle.active) break;
        if (app.runner.running() || !app.havePlayer) continue;
        const cr::Vec3 before = app.walker.pos;
        cr::stepWalker(app.walker, app.walkCollision, sx, sy, run, camYaw);
        const cr::Vec3 moved = app.walker.pos - before;
        fieldWalked(app, std::sqrt(cr::dot(moved, moved)));
        checkTriggers(app);
        if (app.runner.running() || !app.dialogue.empty() || !app.havePlayer) continue;
        cr::BattleStart start; // fldencount_encount
        auto random = [&](int n) { return app.rng.get(n); };
        const int pr = std::clamp(app.playerChar / 2, 0, 3) * 2; // 171 Preemptive / 172 Alert on either member
        const bool first = cr::hasSkill(app.game, pr, 171) || cr::hasSkill(app.game, pr + 1, 171);
        const bool alert = cr::hasSkill(app.game, pr, 172) || cr::hasSkill(app.game, pr + 1, 172);
        if (app.encounters.check(random, partyLevel(app), first, alert, start)) {
            writeResume(app); // (fldencount_encount: the Progress Tracker at the fight's start)
            app.game.vars[58] = 999; // the usual battle music, victory tune 10
            app.game.vars[59] = 10;
            app.game.vars[60] = 999;
            startBattle(app, start, 0);
            break;
        }
    }
    // Actions 0/1/2 = stand / walk / run (mdl_set_act_motion in FieldPCCtrl::operate); while a
    // block is held, slot 3 looped from frame 2 (Psychokinesis::onCheckTouch's
    // mdl_set_act_motion_loop(leader, 3, 2, 2))
    const bool hold = app.puzzle.pkState != 0 && app.player.actions.size() > 3;
    const int want = hold ? 3 : std::min(int(app.walker.gait), int(app.player.actions.size()) - 1);
    if (want != app.player.action) {
        app.player.action = want;
        app.player.time = 0;
        app.footPrev = 0;
    }
    app.player.loopFrom = hold ? 2 : -1;
    animate(app, app.player, dt);
    playFootsteps(app);
}

cr::Mat4 playerWorld(const cr::FieldWalker& w) {
    cr::Mat4 m;
    const float c = std::cos(w.heading), s = std::sin(w.heading);
    m.m[0] = c; m.m[2] = -s; m.m[8] = s; m.m[10] = c;
    m.m[12] = w.pos.x; m.m[13] = w.pos.y; m.m[14] = w.pos.z;
    return m;
}

// The map's visible NPCs (npc_setup): model from the NPC table, position (plus the offset
// for models 0/1), turned by the record's angle about Y, scaled by the table's percentage.
// An NPC's model matrix: position (plus the offset for models 0/1), turned about Y, scaled
cr::Mat4 npcWorld(const cr::FieldNpc& n) {
    const float k = n.model <= 1 ? 1.0f : 0.0f;
    const float c = std::cos(n.angle), sn = std::sin(n.angle);
    cr::Mat4 w;
    w.m[0] = c * n.scale; w.m[2] = -sn * n.scale;
    w.m[5] = n.scale;
    w.m[8] = sn * n.scale; w.m[10] = c * n.scale;
    w.m[12] = n.pos[0] + k * n.offset[0];
    w.m[13] = n.pos[1] + k * n.offset[1];
    w.m[14] = n.pos[2] + k * n.offset[2];
    return w;
}

cr::FieldNpc* findNpc(App& app, int id) {
    for (cr::FieldNpc& n : app.npcs)
        if (n.id == id) return &n;
    return nullptr;
}

// Where a script actor is: npc 0 is the party leader
bool actorPos(App& app, int id, cr::Vec3& out) {
    if (id == 0) { if (!app.havePlayer) return false; out = app.walker.pos; return true; }
    const cr::FieldNpc* n = findNpc(app, id);
    if (!n) return false;
    out = {n->pos[0], n->pos[1], n->pos[2]};
    return true;
}

// Moves / turns a script actor and its drawn model (get_floor snaps y unless keepY)
void setActor(App& app, int id, cr::Vec3 p, const float* yaw, bool keepY) {
    float y;
    if (!keepY && (cr::floorHeight(app.collisionData, p.x, p.y, p.z, 1000.0f, y) ||
                   cr::floorHeight(app.collisionData, p.x, p.y + 1000.0f, p.z, 2000.0f, y)))
        p.y = y;
    if (id == 0) {
        app.walker.pos = p;
        if (yaw) app.walker.heading = *yaw;
        return;
    }
    cr::FieldNpc* n = findNpc(app, id);
    if (!n) return;
    const bool wasParked = !app.placedNpcs.count(id);
    app.placedNpcs.insert(id);
    n->pos[0] = p.x; n->pos[1] = p.y; n->pos[2] = p.z;
    if (yaw) n->angle = *yaw;
    bool drawn = false;
    for (auto& d : app.npcDraws)
        if (d.id == id) { d.world = npcWorld(*n); drawn = true; }
    if (wasParked || !drawn) buildNpcDraws(app); // a parked actor enters the scene
}

void buildNpcDraws(App& app) {
    std::vector<App::NpcDraw> old;
    old.swap(app.npcDraws);
    if (!app.showNpcs) return;
    for (size_t i = 0; i < app.npcs.size(); ++i) {
        const cr::FieldNpc& n = app.npcs[i];
        if (n.model < 0 || n.model >= cr::kNumNpcModels || !npcDrawn(app, i)) continue;
        // Cutscene actor slots wait parked at y 995..1000 or at the origin until a scene places
        // them (FrmSetc2)
        if ((n.pos[1] >= 900.0f || (n.pos[0] == 0 && n.pos[1] == 0 && n.pos[2] == 0)) && !app.placedNpcs.count(n.id))
            continue;
        const std::string file = cr::kNpcModelFile[n.model];
        auto it = app.npcModels.find(file);
        if (it == app.npcModels.end()) {
            LoadedModel lm;
            loadModelFile(app, file, lm, true);
            it = app.npcModels.emplace(file, std::move(lm)).first;
        }
        if (it->second.meshes.empty()) continue;
        const cr::Mat4 w = npcWorld(n);
        App::NpcDraw d;
        for (const App::NpcDraw& o : old) // the same NPC: keep its animation and fade
            if (o.index == int(i) && o.model == file) { d = o; break; }
        d.model = file;
        d.world = w;
        d.id = n.id;
        d.index = int(i);
        app.npcDraws.push_back(d);
    }
}

// Solid NPCs (RectWrapper::init / checkHitWallToSphere): kind bit 1 set, bit 2 clear,
// displayed. Their box is the record's box (offset ± size / 2) turned by the NPC's angle and
// placed at its position; it is added to the walking collision as four vertical walls.
void buildWalkCollision(App& app) {
    app.walkCollision = app.collisionData;
    cr::Collision& c = app.walkCollision;
    for (size_t idx = 0; idx < app.npcs.size(); ++idx) {
        const cr::FieldNpc& n = app.npcs[idx];
        if (!(n.kind & 2) || (n.kind & 4) || !npcVisible(app, idx)) continue;
        if (n.pos[1] >= 900.0f || (n.pos[0] == 0 && n.pos[1] == 0 && n.pos[2] == 0)) continue;
        if (n.box[0] <= 0 || n.box[2] <= 0) continue;
        const float k = n.model <= 1 ? 1.0f : 0.0f; // npc_setup adds the offset for models 0/1
        const float bx = n.pos[0] + k * n.offset[0], bz = n.pos[2] + k * n.offset[2];
        const float cs = std::cos(n.angle), sn = std::sin(n.angle);
        const float lx[4] = {n.offset[0] - n.box[0] / 2, n.offset[0] + n.box[0] / 2, n.offset[0] + n.box[0] / 2,
                             n.offset[0] - n.box[0] / 2};
        const float lz[4] = {n.offset[2] - n.box[2] / 2, n.offset[2] - n.box[2] / 2, n.offset[2] + n.box[2] / 2,
                             n.offset[2] + n.box[2] / 2};
        float px[4], pz[4], cx = 0, cz = 0;
        for (int i = 0; i < 4; ++i) {
            px[i] = bx + cs * lx[i] + sn * lz[i];
            pz[i] = bz - sn * lx[i] + cs * lz[i];
            cx += px[i] / 4;
            cz += pz[i] / 4;
        }
        const float y0 = n.pos[1] - 5, y1 = n.pos[1] + std::max(n.box[1], 10.0f);
        for (int i = 0; i < 4; ++i) {
            const int j = (i + 1) % 4;
            cr::Vec3 v[4] = {{px[i], y0, pz[i]}, {px[j], y0, pz[j]}, {px[j], y1, pz[j]}, {px[i], y1, pz[i]}};
            cr::Vec3 nrm = cr::normalize(cr::cross(v[1] - v[0], v[3] - v[0]));
            const cr::Vec3 mid{(px[i] + px[j]) / 2 - cx, 0, (pz[i] + pz[j]) / 2 - cz};
            if (cr::dot(nrm, mid) < 0) { std::swap(v[1], v[3]); nrm = nrm * -1.0f; } // face outwards
            cr::CollisionPolygon poly;
            poly.normal[0] = nrm.x; poly.normal[1] = nrm.y; poly.normal[2] = nrm.z;
            poly.count = 4;
            poly.attr1 = 1;
            for (int q = 0; q < 4; ++q) {
                poly.index[q] = int16_t(c.vertices.size() / 3);
                c.vertices.insert(c.vertices.end(), {v[q].x, v[q].y, v[q].z});
            }
            c.polygons.push_back(poly);
        }
    }
}

// Each NPC instance has its own action and time: action 0 (idle) loops, others play once and
// hold their last frame (a chest lid stays open).
void animateNpcs(App& app, float dt) {
    for (auto& d : app.npcDraws) {
        const LoadedModel& lm = app.npcModels[d.model];
        if ((d.action < 0 || d.action >= int(lm.actions.size())) && !lm.packageActions.empty()) {
            float dur = 0; // only its materials move: they loop
            for (const auto& a : lm.packageActions) dur = std::max(dur, a.duration);
            d.time += dt * kFramesPerSecond;
            if (dur > 0) d.time = std::fmod(d.time, dur);
            continue;
        }
        if (d.action < 0 || d.action >= int(lm.actions.size())) continue;
        const float dur = lm.actions[d.action].duration;
        d.time += dt * kFramesPerSecond;
        if (dur <= 0) continue;
        if (d.once) { d.time = std::min(d.time, dur); continue; } // loop from -1: play once, hold the end
        const float end = d.loopTo > 0 && d.loopTo < dur ? float(d.loopTo) : dur;
        if (d.time >= end) { // then repeat frames loopFrom..loopTo
            const float start = std::clamp(float(d.loopFrom), 0.0f, end), len = end - start;
            d.time = len > 0 ? start + std::fmod(d.time - start, len) : start;
        }
    }
}

void drawLoaded(App& app, const LoadedModel& m, const cr::Mat4& mvp, const cr::Material* override = nullptr,
                float alpha = 1.0f, float matTime = -1.0f);
const cr::Action* effectMaterialAction(const LoadedModel& lm, const cr::Material* mt);
bool effectUvMotion(const LoadedModel& lm, const cr::Material* mt, float time, float shift[2], float scale[2]);

void drawNpcs(App& app, const cr::Mat4& vp) {
    for (auto& d : app.npcDraws) {
        if (d.alpha <= 0.001f) continue;
        LoadedModel& lm = app.npcModels[d.model];
        const cr::Model& m = lm.cpu;
        const bool matAnim = !lm.packageActions.empty();
        auto matOf = [&](size_t i) -> const cr::Material* {
            const int ap = i < m.meshes.size() ? m.meshes[i].appearance : -1;
            return ap >= 0 && ap < int(m.materials.size()) ? &m.materials[size_t(ap)] : nullptr;
        };
        const bool posed = !m.skeleton.nodes.empty() && lm.meshes.size() == m.meshes.size();
        if (posed || (matAnim && lm.meshes.size() == m.meshes.size())) { // skin this instance; scroll its textures
            const cr::Action* act = d.action >= 0 && d.action < int(lm.actions.size()) ? &lm.actions[d.action] : nullptr;
            if (posed) cr::computeWorld(m.skeleton, act, d.time, lm.world);
            for (size_t i = 0; i < m.meshes.size(); ++i) {
                float shift[2] = {0, 0}, scale[2] = {1, 1};
                const bool moves = matAnim && effectUvMotion(lm, matOf(i), d.time, shift, scale);
                if (!posed && !moves) continue;
                if (posed) cr::skinMesh(m, m.meshes[i], lm.world, lm.skinned);
                const std::vector<float>& pos = posed ? lm.skinned : m.meshes[i].positions;
                if (moves) { // uv * scale + offset (the material action's segments 8 / 6)
                    lm.uvScratch = m.meshes[i].uvs;
                    for (size_t k = 0; k + 1 < lm.uvScratch.size(); k += 2) {
                        lm.uvScratch[k] = lm.uvScratch[k] * scale[0] + shift[0];
                        lm.uvScratch[k + 1] = lm.uvScratch[k + 1] * scale[1] + shift[1];
                    }
                }
                app.renderer.updatePositions(lm.meshes[i], pos, moves ? lm.uvScratch : m.meshes[i].uvs, m.meshes[i].colors);
            }
        }
        if (matAnim) ++app.matAnimDraws[d.model];
        drawLoaded(app, lm, vp * d.world, nullptr, d.alpha, matAnim ? d.time : -1.0f);
    }
}

// Draws a model's visible meshes: opaque materials first, then blended ones by draw order.
// `override`, if given, replaces every material (texture unbound); `alpha` < 1 fades it.
void drawLoaded(App& app, const LoadedModel& m, const cr::Mat4& mvp, const cr::Material* override, float alpha, float matTime) {
    const cr::Model& cpu = m.cpu;
    static const cr::Material kDefaultMaterial;
    auto baseOf = [&](size_t i) -> const cr::Material& {
        const int a = i < cpu.meshes.size() ? cpu.meshes[i].appearance : -1;
        return a >= 0 && a < int(cpu.materials.size()) ? cpu.materials[a] : kDefaultMaterial;
    };
    // `matTime` >= 0: the Appearances' own actions colour them at that time (keyType-12 RGBA in
    // segment 1, as effects); a blended one gone fully clear isn't drawn
    std::vector<cr::Material> animated;
    if (matTime >= 0 && !m.packageActions.empty()) {
        animated.resize(m.meshes.size());
        for (size_t i = 0; i < m.meshes.size(); ++i) {
            animated[i] = baseOf(i);
            const cr::Action* ma = effectMaterialAction(m, &baseOf(i));
            if (!ma || ma->segments.size() < 2) continue;
            for (const cr::Track& t : ma->segments[1]) {
                float v[4] = {1, 1, 1, 1};
                if (t.type == 12 && t.comps == 4 && cr::evaluateTrack(t, std::min(matTime, ma->duration), v))
                    for (int k = 0; k < 4; ++k) animated[i].diffuse[k] = std::clamp(v[k], 0.0f, 1.0f);
            }
        }
    }
    auto materialOf = [&](size_t i) -> const cr::Material& { return i < animated.size() ? animated[i] : baseOf(i); };
    std::vector<size_t> order(m.meshes.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const cr::Material &ma = materialOf(a), &mb = materialOf(b);
        if (ma.blended() != mb.blended()) return !ma.blended();
        return ma.drawOrder < mb.drawOrder;
    });
    for (size_t i : order) {
        if (i < cpu.meshes.size() && !cpu.meshes[i].visible) continue;
        if (i < animated.size() && animated[i].blended() && animated[i].diffuse[3] <= 0.004f) continue;
        const cr::GpuMesh& mesh = m.meshes[i];
        const cr::Texture* tex =
            mesh.texture >= 0 && mesh.texture < int(m.textures.size()) ? &m.textures[mesh.texture] : nullptr;
        if (override) {
            app.renderer.drawMesh(mesh, nullptr, mvp, *override);
        } else if (alpha < 0.999f) {
            cr::Material faded = materialOf(i);
            faded.diffuse[3] *= alpha;
            if (!faded.blended()) { faded.blendSrc = 4; faded.blendDst = 5; }
            app.renderer.drawMesh(mesh, tex, mvp, faded, app.smoothFilter);
        } else {
            app.renderer.drawMesh(mesh, tex, mvp, materialOf(i), app.smoothFilter);
        }
    }
}

// Pieces of window_hd.mct (2048 x 2048), measured from its alpha
extern const float kWinMessagePanel[4] = {0, 1408, 960, 192}; // grey message window
extern const float kWinSmallPanel[4] = {0, 1600, 512, 96}; // dark small window
// The message windows (the reference playthrough: speech with portraits up sits on the torn
// parchment, its name on the blue streak painted into it; other speech on the grey band, the
// name on a small dark tab). The skip button and the page's "next" diamond beside them.
constexpr float kWinParchment[4] = {1024, 248, 1024, 280};
constexpr float kWinSkip[4] = {680, 1700, 50, 50};
constexpr float kWinNext[4] = {672, 1750, 62, 30};
constexpr float kWinBadge[4] = {936, 399, 81, 83};         // round "!" badge

// A message page in the game's message window, across the whole screen's width: the parchment
// while portraits are up, else the grey band; `next`: the page waits for a tap (the diamond)
// The page's lines at (x, y): as one block, or - when a line is centred (0x17 flips a line; the
// windowless style 0x10 centres them all unless flipped) - one by one, the centred ones about the
// layout's middle (CmdFont: tex2d x 240)
void drawMessageLines(App& app, const std::vector<std::string>& page, float x, float y, float scale, bool centreAll) {
    cr::Renderer& r = app.renderer;
    bool any = centreAll;
    for (const std::string& l : page) any |= !l.empty() && l[0] == '\x01';
    const std::vector<std::string> shown = typedPage(page, app.textLimit); // typing: centred on the whole line
    if (!any) {
        app.dialogueHud.drawAt(r, shown, x, y, scale, 0, 0, 0.0f);
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
        const float lx = flip != centreAll ? cr::Renderer::kLogicalW / 2 - w / 2 : x;
        const std::string& part = shown[k];
        const std::string drawn = flip && !part.empty() ? part.substr(1) : part;
        if (!drawn.empty()) h.drawAt(r, {drawn}, lx, y + step * float(k), scale, 0, 0, 0.0f);
    }
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
    if (base != 0 || style & 0x80 || style & 0x200) {
        const float wy = 2.0f * rect[0] + 64, ty = 2.0f * (rect[0] + rect[1]) + 64;
        float tx = 2.0f * 36 + 32;
        bool outline = false;
        if (style & 0x80 || style & 0x210) { // no window: the text alone (0x10: outlined in black)
            outline = (style & 0x10) != 0;
        } else if (style & 0x20) { // a black band across the bottom (tex2d y 252..352), the name on its edge
            r.fillRect(left, 2.0f * 252 + 64, width, 768 - (2.0f * 252 + 64), 0, 0, 0, 1);
            if (!speaker.empty()) { // PutMsg: drawfont_l at tex2d (16, 236), just above the band
                const float nx = 2.0f * 16 + 32, ny = 2.0f * 236 + 64;
                app.nameHud.setTint(0, 0, 0); // (an outline, so it reads over a bright scene too)
                for (int k = 0; k < 4; ++k) app.nameHud.drawAt(r, {speaker}, nx + (k & 1 ? 2 : -2), ny + (k & 2 ? 2 : -2), 2.2f, 0, 0, 0.0f);
                app.nameHud.setTint(1, 1, 1);
                app.nameHud.drawAt(r, {speaker}, nx, ny, 2.2f, 0, 0, 0.0f);
            }
        } else if (style & 8) { // a tutorial's band: dark, its edges soft (8 / 88 / 8 tex2d), the text further left
            const float a = 0.75f, y1 = wy + 16, y2 = wy + 192;
            for (int k = 0; k < 8; ++k) { // the 2D path has no per-vertex colour: the edges in strips
                const float t = (float(k) + 0.5f) / 8.0f;
                r.fillRect(left, wy + 2.0f * float(k), width, 2.0f, 0, 0, 0, a * t);
                r.fillRect(left, y2 + 2.0f * float(k), width, 2.0f, 0, 0, 0, a * (1 - t));
            }
            r.fillRect(left, y1, width, y2 - y1, 0, 0, 0, a);
            if (app.language == 1) tx = 2.0f * 8 + 32;
        } else if (style & 0x40) { // the grey panel (960 x 192 at its rect)
            drawWinPiece(app, kWinMessagePanel, 32, wy, 960, 192);
            if (!speaker.empty()) {
                drawWinPiece(app, kWinSmallPanel, 32, wy - 36, 300, 40);
                app.nameHud.setTint(1, 1, 1);
                app.nameHud.drawAt(r, {speaker}, 62, wy - 34, 1.9f, 0, 0, 0.0f);
            }
        }
        const bool centreAll = (style & 0x10) != 0;
        if (outline) {
            app.dialogueHud.setTint(0, 0, 0);
            for (int k = 0; k < 8; ++k) {
                static const float d[8][2] = {{-2, -2}, {0, -2}, {2, -2}, {-2, 0}, {2, 0}, {-2, 2}, {0, 2}, {2, 2}};
                drawMessageLines(app, page, tx + d[k][0], ty + d[k][1], 2.2f, centreAll);
            }
        }
        app.dialogueHud.setTint(1, 1, 1);
        drawMessageLines(app, page, tx, ty, 2.2f, centreAll);
        if (!pageNo.empty()) {
            app.pageHud.setTint(1, 1, 1);
            app.pageHud.drawAt(r, {pageNo}, right - 140, H - 48, 1.5f, 0, 0, 0.0f);
        }
        // PutMsg's "next" mark: tex2d x 452, at y 328 - beside a tutorial's band 84 below its
        // top; none on the grey panel
        const float markY = style & 8 ? 2.0f * (rect[0] + 84) + 64 : H - 44;
        if (next && !(style & 0x40) && (SDL_GetTicks() / 400) % 2 == 0) drawWinPiece(app, kWinNext, right - 76, markY, 46, 22);
        if (app.runner.running()) drawWinPiece(app, kWinSkip, right - 66, top + 12, 50, 50);
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
        drawWinPiece(app, kWinParchmentPhone, 32, 2.0f * (pr[0] + 16) + 64, 960, 256);
        if (!speaker.empty()) {
            app.nameHud.setTint(1, 1, 1);
            app.nameHud.drawAt(r, {speaker}, 2.0f * 24 + 32, 2.0f * 206 + 64 - 14, 2.0f, 0, 0, 0.0f);
        }
        app.dialogueHud.setTint(0.12f, 0.10f, 0.08f);
        textX = 2.0f * 36 + 32;
        textY = 2.0f * (pr[0] + pr[1]) + 64;
    } else if (true) { // the parchment (style 2, and the default)
        drawWinPiece(app, kWinParchment, left, H - 280, width, 280);
        if (!speaker.empty()) {
            app.nameHud.setTint(1, 1, 1);
            app.nameHud.drawAt(r, {speaker}, left + 44, H - 274, 2.0f, 0, 0, 0.0f); // on the streak (piece y 14..52)
        }
        app.dialogueHud.setTint(0.12f, 0.10f, 0.08f); // ink
        textY = H - 196;
    } else {
        drawWinPiece(app, kWinMessagePanel, left, H - 170, width, 170);
        if (!speaker.empty()) {
            drawWinPiece(app, kWinSmallPanel, left, H - 206, 300 - left, 40);
            app.nameHud.setTint(1, 1, 1);
            app.nameHud.drawAt(r, {speaker}, left + 30, H - 204, 1.9f, 0, 0, 0.0f);
        }
        app.dialogueHud.setTint(1, 1, 1);
        textY = H - 140;
    }
    drawMessageLines(app, page, textX, textY, 2.2f, false); // text and name from the screen's left
    if (!pageNo.empty()) {
        app.pageHud.setTint(0.12f, 0.10f, 0.08f);
        app.pageHud.drawAt(r, {pageNo}, right - 140, H - 48, 1.5f, 0, 0, 0.0f);
    }
    if (next && (SDL_GetTicks() / 400) % 2 == 0) drawWinPiece(app, kWinNext, right - 76, H - 44, 46, 22);
    if (app.runner.running()) drawWinPiece(app, kWinSkip, right - 66, top + 12, 50, 50); // a scene is playing
}

void drawWinPiece(App& app, const float* piece, float x, float y, float w, float h) {
    app.renderer.drawQuadUV(app.windowTex, x, y, w, h, piece[0], piece[1], piece[2], piece[3]);
}

// ---- Battles (btl_main) ----

// The pair's average level (the encounter tables' minimum levels)
int partyLevel(const App& app) {
    const int route = std::clamp(app.playerChar / 2, 0, 3);
    return (app.game.party[size_t(2 * route)].level + app.game.party[size_t(2 * route + 1)].level) / 2;
}

// A system text's first line (sys.evt ids >= 51000), empty if none; `name` fills string 0
std::string sysText(const App& app, int id, const std::string& name) {
    std::vector<std::vector<std::string>> pages;
    if (app.systemText.message(id, app.language, nullptr, pages, nullptr,
                               [&](int n) { return n == 0 || n == 9 ? name : std::string(); }) &&
        !pages.empty() && !pages[0].empty())
        return pages[0][0];
    return std::string();
}

// The package action for battle action slot `slot`: the built-in action numbered slot + 1
// (named "built-in <n>.<m>"); -1 if the package has none
int battleAction(const LoadedModel& lm, int slot) {
    const std::string want = "built-in " + std::to_string(slot + 1) + ".";
    for (int i = 0; i < lm.builtin && i < int(lm.actions.size()); ++i)
        if (lm.actions[size_t(i)].name.rfind(want, 0) == 0) return i;
    return -1;
}

// The battle HUD (menub_mainPut and its cmd_* pieces), in the menus' 512 x 384 units: the
// tex2d calls' 480 x 320 space moved by (16, 32). Everything comes from window_hd (slot 44) or
// plain colour (slot 45).
namespace btlui {
constexpr float kButtonX = 344, kButtonY = 128, kButtonStep = 48; // cmd_bottom at tex2d (328, 96 + 48k)
constexpr float kPlateX = 224, kPlateBottom = 312;                 // cmd_bottom_teki: y = 312 - 48 (n - i)
constexpr float kBackX = 208, kBackY = 320;                        // cmd_back at tex2d (192, 288)
// The first menu: Solo, Pair, Escape, Options; then Attack, Genes, Items, Defend (icon, word)
constexpr int kTopIcon[4] = {15, 14, 16, 3}, kTopWord[4] = {21, 20, 22, 3};
constexpr int kCmdIcon[4] = {18, 10, 8, 19}, kCmdWord[4] = {24, 10, 8, 25};
// The Break badge's eight ring pieces (0xd1fd0): x, y, u, v, 32 x 32 each
constexpr short kBreakTick[8][4] = {{4, 4, 96, 120},    {4, 4, 64, 120},    {4, 28, 64, 152},  {4, 28, 96, 152},
                                    {28, 28, 128, 152}, {28, 28, 160, 152}, {28, 4, 160, 120}, {28, 4, 128, 120}};
// __dmg_prc: the digits' hop (0xcb398) and the label's (0xcb390), by (383 - timer) / 16
constexpr int kDigitHop[8] = {16, 20, 15, 10, 5, 0, 0, 0}, kLabelHop[8] = {3, 6, 12, 6, 3, 0, 0, 0};
constexpr int kPopupTime = 0x17f; // __dmg_set; 16 off a frame
constexpr int kItemRows = 5;
constexpr int kBannerFrames = 40;  // font_set_utf16's 0x28 (the action's name)
constexpr int kMessageFrames = 60; // 0x3c: the calcs' messages and the queued ones
// Action slots (BattleAnim): 0 standing, 1 victory, 3 back to place, 4 defending, 5 hit,
// 6 fallen, 7 evading; attacks use their class (8-10), genes 11, items 12
constexpr int kSlotIdle = 0, kSlotWin = 1, kSlotReturn = 3, kSlotGuard = 4, kSlotHit = 5, kSlotDown = 6, kSlotEvade = 7;
} // namespace btlui

App::Battle::Actor* battleActor(App& app, int fighter) {
    for (App::Battle::Actor& a : app.battle.actors)
        if (a.fighter == fighter) return &a;
    return nullptr;
}

// Plays action slot `slot` from its start; `then` >= 0 follows it once it ends (else it loops)
void playSlot(App& app, App::Battle::Actor& a, int slot, int then = -1) {
    const LoadedModel& lm = app.battle.models[a.model];
    a.slot = slot;
    a.action = battleAction(lm, slot);
    if (a.action < 0) { // the model hasn't that one: stand
        a.slot = btlui::kSlotIdle;
        a.action = battleAction(lm, btlui::kSlotIdle);
    }
    a.time = 0;
    a.then = then;
    a.loopFrom = a.loopTo = -1;
}

float actorDuration(App& app, const App::Battle::Actor& a) {
    const LoadedModel& lm = app.battle.models[a.model];
    return a.action >= 0 && a.action < int(lm.actions.size()) ? lm.actions[size_t(a.action)].duration : 0.0f;
}

// Where the damage numbers start: over the actor's model
cr::Vec3 actorPoint(App& app, const App::Battle::Actor& a, int user);
cr::Vec3 actorHead(App& app, const App::Battle::Actor& a) {
    // __dmg_set: btlcamera_getpos(actor, 0x44e) - the model's node 1102 (else over its bounds)
    const cr::Vec3 p = actorPoint(app, a, 1102);
    if (p.x != a.pos.x || p.y != a.pos.y || p.z != a.pos.z) return p;
    const LoadedModel& lm = app.battle.models[a.model];
    return a.pos + cr::Vec3{0, (lm.center.y + lm.radius * 0.6f) * a.scale, 0};
}

// btlcamera_set: camera `id` (sys.evt section 15), its values over 9000 relative to fighter `actor`
void setBattleCamera(App& app, int id, int actor = 0) {
    App::Battle& b = app.battle;
    if (!app.battleData.camera(id)) return;
    b.cam = id;
    b.camActor = actor;
    b.camFrame = 0;
}

// btlcamera_prc: the camera's eye and target this frame - still, moving from its first pair of
// points to its second over raw[15] frames, or turning raw[14] degrees about its target
void battleCameraView(App& app, cr::Vec3& eye, cr::Vec3& target) {
    const App::Battle& b = app.battle;
    const cr::BattleCamera* c = app.battleData.camera(b.cam);
    if (!c) c = app.battleData.camera(std::max(0, b.formation - 2));
    if (!c) return;
    const int16_t* r = c->raw;
    cr::Vec3 base{0, 0, 0};
    for (const App::Battle::Actor& a : b.actors) // btlcamera_prc / getpos: its node 1003 (0x3eb) as it moves
        if (a.fighter == b.camActor) base = actorPoint(app, a, 1003);
    auto rel = [](int v, float at) { return v > 9000 ? float(v - 10000) + at : float(v); };
    eye = {rel(r[0], base.x), rel(r[1], base.y), rel(r[2], base.z)};
    target = {rel(r[3], base.x), rel(r[4], base.y), rel(r[5], base.z)};
    const float t = std::min(1.0f, float(b.camFrame) / float(std::max<int>(1, r[15])));
    if (r[7] != 0) {
        const cr::Vec3 e2{rel(r[7], base.x), rel(r[8], base.y), rel(r[9], base.z)};
        const cr::Vec3 t2{rel(r[10], base.x), rel(r[11], base.y), rel(r[12], base.z)};
        eye = eye + (e2 - eye) * t;
        target = target + (t2 - target) * t;
    } else if (r[14] != 0) {
        const float ang = float(r[14]) * t * 3.14159265f / 180.0f, c0 = std::cos(ang), s0 = std::sin(ang);
        const float dx = eye.x - target.x, dz = eye.z - target.z;
        eye.x = target.x + dx * c0 - dz * s0;
        eye.z = target.z + dx * s0 + dz * c0;
    }
}

// The cameras the menus and actions use (menub_mainPrc, btlord_make_select, btlact_*_init)
int monsterSize(const App& app, int fighter) {
    const cr::Fighter& f = app.battle.rules.f[size_t(std::clamp(fighter, 0, 4))];
    return f.def ? f.def->size : 1;
}
// read_btlcpl's per-slot camera index (0xd3220): formation row, enemy slot
int enemyCameraSlot(const App& app, int fighter) {
    static const short kSlot[16][3] = {{0, 0, 0},   {0, 0, 0},    {23, 24, 0},  {25, 0, 0},   {0, 0, 0},    {1, 2, 0},
                                       {3, 4, 5},   {6, 0, 0},    {7, 8, 0},    {9, 10, 11},  {12, 13, 0},  {14, 0, 0},
                                       {15, 16, 0}, {17, 18, 19}, {20, 21, 0},  {22, 0, 0}};
    if (fighter < 2) return 0;
    return kSlot[std::clamp(app.battle.formation, 0, 15)][std::clamp(fighter - 2, 0, 2)];
}

void cameraForTarget(App& app, int target, bool group) {
    App::Battle& b = app.battle;
    if (target < 2) setBattleCamera(app, b.rules.pair || group ? 30 : 28 + target);
    else if (group) setBattleCamera(app, b.formation + 0x26, 2);
    else {
        const int size = monsterSize(app, target);
        setBattleCamera(app, size == 1 ? target + 0x1d : size + 0x20, target);
    }
}

void cameraForTurn(App& app, const cr::BattleTurn& t) {
    App::Battle& b = app.battle;
    const int user = t.actors[0], target = t.hits.empty() ? -1 : t.hits[0].target;
    const bool userPair = t.actors[1] >= 0;
    if (t.kind == cr::kCmdAttack && target >= 0 && (user < 2) != (target < 2)) {
        if (user < 2) setBattleCamera(app, enemyCameraSlot(app, target) * 3 + 0x1b0 + (userPair ? 2 : user));
        else if (!userPair) setBattleCamera(app, b.rules.random(3) + enemyCameraSlot(app, user) * 4 + (target == 0 ? 0x78 : 0xe0));
        else setBattleCamera(app, b.rules.random(3) + enemyCameraSlot(app, user) * 4 + 0x148);
        return;
    }
    if (t.kind == cr::kCmdGene || t.kind == cr::kCmdItem || t.kind == cr::kCmdAttack) {
        const int first = t.kind == cr::kCmdItem ? 0x53 : 0x49;
        if (user < 2) setBattleCamera(app, userPair ? first + 2 : first + user, userPair ? 0 : user);
        else if (monsterSize(app, user) == 1) setBattleCamera(app, userPair ? first + 5 : user + first + 1, user);
        else setBattleCamera(app, monsterSize(app, user) + first + 4, user);
    }
}

// ---- Battle effects (eff3d_*) ----
// eff.bin is an index of 100 (u32 offset, u32 size) entries, each a model package. An effect plays
// its first action once at a point (an actor's origin, or the node of its bone whose record user
// id the table 0xd37c0 names: 1101 the body, 1102 the head, 1003 the root, 1201.. the limbs) and
// is deleted at its end (eff3d_prc). Its materials (diffuse alpha 0 at rest) take their colour
// from the package action their Appearance's refGid numbers (segment 1, keyType 12 RGBA).
namespace effects {
constexpr short kNode[100] = {0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
                              0,    0,    0,    0,    0,    0,    0,    1204, 1203, 1101, 1101, 1102, 1003, 1101, 0,
                              0,    1101, 1206, 0,    1101, 1101, 0,    0,    1101, 1102, 1101, 1101, 1102, 1101, 1101,
                              1102, 1101, 1101, 1102, 0,    1101, 1101, 1101, 0,    0,    0,    0,    0,    0,    1101,
                              1101, 1101, 1101, 1101, 1101, 1102, 1102, 1102, 0,    1102, 1102, 1102, 1101, 1101, 1101,
                              1101, 1101, 1101, 0,    0,    1101, 1101, 1101, 1101, 1101, 1101, 1101, 1101, 1101, 1101,
                              1101, 1101, 1101, 100,  100,  100,  200,  250,  300,  0};
// What the calcs show (btlcalc_*'s eff3d_set_btl ids)
constexpr int kHit = 0x18, kCritical = 0x19, kFall = 0x1a, kFixed = 0x1c, kSacrifice = 0x22, kGene = 0x23,
              kStatus = 0x24, kDrain = 0x25, kHeal = 0x2f, kRevive = 0x31, kCast = 0x15, kBuff = 0x35, kAnalyze = 0x36;
} // namespace effects

LoadedModel* effectModel(App& app, int id) {
    if (id < 0 || id >= 100) return nullptr;
    if (auto it = app.effectModels.find(id); it != app.effectModels.end()) return it->second.meshes.empty() ? nullptr : &it->second;
    LoadedModel& out = app.effectModels[id];
    if (app.effBin.empty() && !cr::readFile(app.dataDir + "eff.bin", app.effBin)) return nullptr;
    auto u32 = [&](size_t p) {
        return p + 4 <= app.effBin.size() ? uint32_t(app.effBin[p]) | uint32_t(app.effBin[p + 1]) << 8 |
                                                 uint32_t(app.effBin[p + 2]) << 16 | uint32_t(app.effBin[p + 3]) << 24
                                           : 0u;
    };
    const uint32_t off = u32(size_t(id) * 8), size = u32(size_t(id) * 8 + 4);
    if (!size || size_t(off) + size > app.effBin.size()) return nullptr;
    const std::vector<uint8_t> part(app.effBin.begin() + off, app.effBin.begin() + off + size);
    cr::Model m;
    if (!cr::loadModelPackage(part, m)) return nullptr;
    for (const auto& img : m.textures) {
        cr::Image t = img;
        if (app.video.hdTextures) applyHd(app, t);
        out.textures.push_back(app.renderer.upload(t));
    }
    for (const auto& mesh : m.meshes) out.meshes.push_back(app.renderer.upload(mesh));
    out.packageActions = m.actions;
    for (auto& a : m.actions) // its motion: the first action that fits the skeleton (or its first nodes)
        if (out.actions.empty() && !a.segments.empty() && a.segments.size() <= m.skeleton.nodes.size())
            out.actions.push_back(a);
    m.actions.clear();
    out.action = out.actions.empty() ? -1 : 0;
    out.cpu = std::move(m);
    return out.meshes.empty() ? nullptr : &out;
}

// btlcamera_getpos: the world position of an actor's bone by its record user id (0: the actor)
cr::Vec3 actorPoint(App& app, const App::Battle::Actor& a, int user) {
    LoadedModel& lm = app.battle.models[a.model];
    const cr::Model& m = lm.cpu;
    int node = -1;
    if (user > 0)
        for (const auto& t : m.records)
            if (t.user == user)
                for (const auto& [obj, n] : m.objectNodes)
                    if (obj == t.id) node = n;
    if (node < 0 || m.skeleton.nodes.empty()) return a.pos;
    const cr::Action* act = a.action >= 0 && a.action < int(lm.actions.size()) ? &lm.actions[size_t(a.action)] : nullptr;
    cr::computeWorld(m.skeleton, act, a.time, lm.world);
    if (node >= int(lm.world.size())) return a.pos;
    const float* w = lm.world[size_t(node)].m;
    const float c = std::cos(a.yaw), sn = std::sin(a.yaw);
    const float x = w[12] * a.scale, y = w[13] * a.scale, z = w[14] * a.scale;
    return {a.pos.x + c * x + sn * z, a.pos.y + y, a.pos.z - sn * x + c * z};
}

// eff3d_set_btl: effect `id` on fighter `fighter` (at its table node)
void spawnEffect(App& app, int id, int fighter) {
    App::Battle& b = app.battle;
    if (id < 0 || id >= 100 || b.effects.size() >= 32 || !effectModel(app, id)) return;
    const App::Battle::Actor* a = battleActor(app, fighter);
    if (!a) return;
    Effect e;
    e.id = id, e.pos = actorPoint(app, *a, effects::kNode[id]);
    b.effects.push_back(e);
}

// eff3d_set_with_pos into `list` (32 slots): its handle, 0 when none was free
int startEffect(App& app, std::vector<Effect>& list, int id, cr::Vec3 at, bool loop, float scale, float yaw) {
    if (id < 0 || id >= 100 || list.size() >= 32 || !effectModel(app, id)) return 0;
    if (&list == &app.fieldEffects) ++app.fieldEffectCount[size_t(id)];
    Effect e;
    e.id = id, e.pos = at, e.loop = loop, e.scale = scale, e.yaw = yaw, e.handle = ++app.effectHandles;
    list.push_back(e);
    return e.handle;
}

void spawnEffectAt(App& app, int id, cr::Vec3 at) { startEffect(app, app.battle.effects, id, at, false, 1.0f, 0.0f); }

void stopEffect(std::vector<Effect>& list, int handle) { // eff3d_delete
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Effect& e) { return handle && e.handle == handle; }), list.end());
}

void stepEffects(App& app, std::vector<Effect>& list) { // eff3d_prc: each ends with its action (a loop starts again)
    for (auto& e : list) {
        e.time += 1;
        auto it = app.effectModels.find(e.id);
        const float dur = it == app.effectModels.end() || it->second.actions.empty() ? 30 : it->second.actions[0].duration;
        if (e.loop && e.time >= std::max(dur, 1.0f)) e.time = std::fmod(e.time, std::max(dur, 1.0f));
    }
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const Effect& e) {
                                  auto it = app.effectModels.find(e.id);
                                  const float dur = it == app.effectModels.end() || it->second.actions.empty()
                                                        ? 30
                                                        : it->second.actions[0].duration;
                                  return !e.loop && e.time >= std::max(dur, 1.0f);
                              }),
               list.end());
}
void stepEffects(App& app) { stepEffects(app, app.battle.effects); }

// An effect material's action (its refGid): segment 1 the colour, 6 the texture's offset, 8 its scale
const cr::Action* effectMaterialAction(const LoadedModel& lm, const cr::Material* mt) {
    const int g = mt ? int(int32_t(mt->refGid)) : -1;
    return g >= 0 && g < int(lm.packageActions.size()) ? &lm.packageActions[size_t(g)] : nullptr;
}

// The texture's motion at `time`: uv * scale + shift (false: it has none)
bool effectUvMotion(const LoadedModel& lm, const cr::Material* mt, float time, float shift[2], float scale[2]) {
    const cr::Action* ma = effectMaterialAction(lm, mt);
    bool moves = false;
    for (size_t seg : {size_t(6), size_t(8)})
        if (ma && ma->segments.size() > seg)
            for (const cr::Track& t : ma->segments[seg]) {
                float v[4] = {};
                if (t.comps != 3 || !cr::evaluateTrack(t, std::min(time, ma->duration), v)) continue;
                if (seg == 6 && t.type == 1) shift[0] = v[0], shift[1] = v[1], moves = true;
                if (seg == 8 && t.type == 11) scale[0] = v[0], scale[1] = v[1], moves = true;
            }
    return moves;
}

void drawEffects(App& app, const std::vector<Effect>& list, const cr::Mat4& vp) {
    for (const Effect& e : list) {
        LoadedModel* found = effectModel(app, e.id);
        auto it = app.effectModels.find(e.id);
        if (!found || it == app.effectModels.end()) continue;
        LoadedModel& lm = it->second;
        const cr::Model& m = lm.cpu;
        auto matOf = [&](size_t i) -> const cr::Material* {
            const int ap = i < m.meshes.size() ? m.meshes[i].appearance : -1;
            return ap >= 0 && ap < int(m.materials.size()) ? &m.materials[size_t(ap)] : nullptr;
        };
        auto materialAction = [&](const cr::Material* mt) { return effectMaterialAction(lm, mt); };
        auto uvMotion = [&](const cr::Material* mt, float shift[2], float scale[2]) {
            return effectUvMotion(lm, mt, e.time, shift, scale);
        };
        if (lm.meshes.size() == m.meshes.size()) {
            const bool posed = !m.skeleton.nodes.empty();
            if (posed) cr::computeWorld(m.skeleton, lm.actions.empty() ? nullptr : &lm.actions[0], e.time, lm.world);
            for (size_t i = 0; i < m.meshes.size(); ++i) {
                float shift[2] = {0, 0}, scale[2] = {1, 1};
                static const bool noScroll = SDL_getenv("CR_NO_UV_SCROLL") != nullptr; // tests: before / after
                const bool moves = !noScroll && uvMotion(matOf(i), shift, scale);
                if (!posed && !moves) continue;
                cr::skinMesh(m, m.meshes[i], posed ? lm.world : std::vector<cr::Mat4>(), lm.skinned);
                if (moves) { // the texture scrolls (uv * scale + offset)
                    lm.uvScratch = m.meshes[i].uvs;
                    for (size_t k = 0; k + 1 < lm.uvScratch.size(); k += 2) {
                        lm.uvScratch[k] = lm.uvScratch[k] * scale[0] + shift[0];
                        lm.uvScratch[k + 1] = lm.uvScratch[k + 1] * scale[1] + shift[1];
                    }
                }
                app.renderer.updatePositions(lm.meshes[i], lm.skinned, moves ? lm.uvScratch : m.meshes[i].uvs, m.meshes[i].colors);
            }
        }
        cr::Mat4 world; // scaled, turned about Y (eff3d_set_with_pos_and_ang), at its point
        const float c = std::cos(e.yaw) * e.scale, sn = std::sin(e.yaw) * e.scale;
        world.m[0] = c, world.m[2] = -sn, world.m[5] = e.scale, world.m[8] = sn, world.m[10] = c;
        world.m[12] = e.pos.x, world.m[13] = e.pos.y, world.m[14] = e.pos.z;
        const cr::Mat4 mvp = vp * world;
        // drawn last, depth tested but not written (draw order + 1000, eff3d_set_with_pos_and_ang)
        std::vector<size_t> order(lm.meshes.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) {
            const cr::Material *a = matOf(x), *c = matOf(y);
            return (a ? a->drawOrder : 0) < (c ? c->drawOrder : 0);
        });
        for (size_t i : order) {
            if (i < m.meshes.size() && !m.meshes[i].visible) continue;
            const cr::Material* base = matOf(i);
            if (!base) continue;
            cr::Material mat = *base;
            if (const cr::Action* ma = materialAction(base); ma && ma->segments.size() > 1) // the colour track
                for (const cr::Track& t : ma->segments[1]) {
                    float v[4] = {1, 1, 1, 1};
                    if (t.type == 12 && t.comps == 4 && cr::evaluateTrack(t, std::min(e.time, ma->duration), v))
                        for (int k = 0; k < 4; ++k) mat.diffuse[k] = std::clamp(v[k], 0.0f, 1.0f);
                }
            if (mat.diffuse[3] <= 0.004f && mat.blended()) continue;
            mat.properties &= ~16u; // no depth writes
            const cr::GpuMesh& mesh = lm.meshes[i];
            const cr::Texture* tex = mesh.texture >= 0 && mesh.texture < int(lm.textures.size()) ? &lm.textures[size_t(mesh.texture)] : nullptr;
            app.renderer.drawMesh(mesh, tex, mvp, mat, app.smoothFilter);
        }
    }
}
void drawEffects(App& app, const cr::Mat4& vp) { drawEffects(app, app.battle.effects, vp); }

// read_btlcpl / btl_prty_init: the stage (kBattleStageFile), the pair at formation 0's first two
// places facing the enemies, the group's monsters at their formation's places; then the battle
// music (__bgmgo: var 58 when it names a track, else 8)
// ---- The battles' talks (_chk_karaok_*, EvrStart 2) ----
//
// In a battle the event script (PrcEvnt) runs only while flag 0xfff is set, and the battle waits
// meanwhile (btl_main's states check it); the HUD gives way to the portraits and the message
// box. Two kinds: an event battle's own script, waiting after its Battle, goes on when one of the
// story bosses' moments comes (by group and route, below) until it clears the flag; and in other
// battles the area's EvrStart 2 - the tutorials, keyed by var 1 - starts the first time each
// thing happens (flags 0xff1.. mark them seen), the battle's script ending with the flag.
namespace battletalk {
constexpr int kFlag = 4095;
constexpr int kAnyRoute = -1;
struct Moment { int route, group, when, arg, seen; }; // when: 0 a round, 1 HP <= max / arg, 2 the 0x104 case
// _chk_karaok_frist: at the start
constexpr Moment kFirst[] = {{2, 0x156, 0, 0, -1}, {kAnyRoute, 0x164, 0, 0, -1}, {kAnyRoute, 0x165, 0, 0, -1},
                             {kAnyRoute, 0x166, 0, 0, -1}, {kAnyRoute, 0x167, 0, 0, -1}};
// _chk_karaok_end: as the last enemy falls
constexpr Moment kEnd[] = {{0, 0x154, 0, 0, -1}, {0, 0x158, 0, 0, -1}, {0, 0x160, 0, 0, -1}, {1, 0x155, 0, 0, -1},
                           {1, 0x11b, 0, 0, -1}, {1, 0x159, 0, 0, -1}, {1, 0x161, 0, 0, -1}, {2, 0x162, 0, 0, -1},
                           {3, 0x157, 0, 0, -1}, {3, 0x15b, 0, 0, -1}, {3, 0x163, 0, 0, -1}};
// _chk_karaok_between: after a round - its number (when 0), or enemy 2's HP at most its most / arg
// (when 1) while flag `seen` is clear
constexpr Moment kBetween[] = {
    {0, 0x104, 2, 3, 0x4d0},  {0, 0x173, 0, 3, -1},      {0, 0xad, 0, 2, -1},       {0, 0x105, 0, 3, -1},
    {0, 0x154, 0, 3, -1},     {0, 0x158, 0, 2, -1},      {0, 0x160, 1, 2, 0x4d7},   {0, 0x170, 1, 2, 0x4d9},
    {1, 0x118, 0, 3, -1},     {1, 0x119, 0, 3, -1},      {1, 0x159, 0, 2, -1},      {1, 0x161, 1, 2, 0x665},
    {1, 0x170, 1, 2, 0x667},  {2, 0x178, 0, 2, -1},      {2, 300, 0, 3, -1},        {2, 0x176, 0, 3, -1},
    {2, 0x156, 1, 5, 0x7f3},  {2, 0x17a, 0, 3, 0x7f4},   {2, 0x15e, 0, 5, 0x7f5},   {2, 0x162, 1, 2, 0x7f6},
    {2, 0x170, 1, 2, 0x7f8},  {3, 0x140, 0, 3, -1},      {3, 0x177, 0, 1, -1},      {3, 0x15b, 0, 2, -1},
    {3, 0x163, 1, 2, 0x985},  {3, 0x170, 1, 2, 0x987},   {kAnyRoute, 0x164, 1, 2, 0x4db},
    {kAnyRoute, 0x165, 1, 2, 0x4dd}, {kAnyRoute, 0x166, 1, 2, 0x4df}, {kAnyRoute, 0x167, 1, 2, 0x4e1}};
// _chk_karaok_dmg_hp: until flag `seen` is set nobody falls in these
constexpr Moment kKeepAlive[] = {
    {0, 0x104, 0, 0, 0x4d0}, {0, 0x173, 0, 0, 0x4d1}, {0, 0xad, 0, 0, 0x4a9},  {0, 0x105, 0, 0, 0x4d2},
    {0, 0x154, 0, 0, 0x4d3}, {0, 0x158, 0, 0, 0x4d5}, {0, 0x160, 0, 0, 0x4d7}, {0, 0x170, 0, 0, 0x4d9},
    {1, 0x118, 0, 0, 0x65f}, {1, 0x119, 0, 0, 0x660}, {1, 0x159, 0, 0, 0x663}, {1, 0x161, 0, 0, 0x665},
    {1, 0x170, 0, 0, 0x667}, {2, 0x178, 0, 0, 0x7ef}, {2, 300, 0, 0, 0x7f0},   {2, 0x176, 0, 0, 0x7f1},
    {2, 0x156, 0, 0, 0x7f3}, {2, 0x17a, 0, 0, 0x7f4}, {2, 0x15e, 0, 0, 0x7f5}, {2, 0x162, 0, 0, 0x7f6},
    {2, 0x170, 0, 0, 0x7f8}, {3, 0x140, 0, 0, 0x97f}, {3, 0x177, 0, 0, 0x980}, {3, 0x15b, 0, 0, 0x983},
    {3, 0x163, 0, 0, 0x985}, {3, 0x170, 0, 0, 0x987}, {kAnyRoute, 0x164, 0, 0, 0x4db},
    {kAnyRoute, 0x165, 0, 0, 0x4dd}, {kAnyRoute, 0x166, 0, 0, 0x4df}, {kAnyRoute, 0x167, 0, 0, 0x4e1}};

int route(const App& app) { return std::clamp(app.playerChar / 2, 0, 3); } // G+0xc18
bool on(const App& app) { return app.battle.active && app.game.flag(kFlag); }
template <size_t N> bool meets(const App& app, const Moment (&list)[N], const std::function<bool(const Moment&)>& more) {
    const App::Battle& b = app.battle;
    if (!b.eventBattle) return false;
    for (const Moment& m : list)
        if ((m.route == kAnyRoute || m.route == route(app)) && m.group == b.start.group && more(m)) return true;
    return false;
}
void open(App& app) { app.game.setFlag(kFlag, true); } // (__menub_karaok_on)

// The scripted rules of this battle, as they stand now (chk_karaok_dmg_hp, btlcalc_status_prc)
void rules(App& app) {
    App::Battle& b = app.battle;
    cr::BattleRules& r = b.rules;
    r.keepAlive = meets(app, kKeepAlive, [&](const Moment& m) { return !app.game.flag(m.seen); });
    r.regenFull = {};
    if (!b.eventBattle) return;
    const int g = b.start.group, rt = route(app);
    for (int i = 2; i < 5; ++i) // enemies: whole again each round until their moment
        r.regenFull[size_t(i)] = (rt == 0 && g == 0x104 && !app.game.flag(0x4d0) && !r.lateAttack) ||
                                 (rt == 2 && i == 2 && g == 0x17a && !app.game.flag(0x7f4)) ||
                                 (rt == 2 && i == 2 && g == 0x15e && !app.game.flag(0x7f5));
}

// btl_main 300 -> 400: the battle's first moment, else the first battle's tutorial (0xff1)
bool tutorial(App& app, int key);
void first(App& app) {
    if (meets(app, kFirst, [](const Moment&) { return true; })) open(app);
    else tutorial(app, 0xff1);
}
// btl_main 899: a round over and everyone standing
void between(App& app) {
    const cr::BattleRules& r = app.battle.rules;
    const cr::Fighter& e = r.f[2];
    if (meets(app, kBetween, [&](const Moment& m) {
            if (m.when == 2) // 0x104: the third round, then the pair attack landing after it
                return r.round == 3 || (r.round > 3 && r.lateAttack && !app.game.flag(m.seen));
            if (m.seen >= 0 && app.game.flag(m.seen)) return false;
            if (m.when == 0) return r.round == m.arg;
            return e.hp <= e.hpMax / m.arg;
        }))
        open(app);
}
// btlord_chk 2: the enemies are down - a last word first? (btl_main 8000 waits for it)
bool end(App& app) {
    if (!meets(app, kEnd, [](const Moment&) { return true; })) return false;
    open(app);
    return true;
}
// EvrStart 2 with var 1 = `key` (0xff1 the first battle, 0xff2 the Break, 0xff3..0xff5 an element
// taken / a status / a gene learned, 0xfa8 / 0xfa9 the Genes / Defend commands), once each: not
// while a script runs (an event battle's waits) or a talk is on
bool tutorial(App& app, int key) {
    if (!app.battle.active || app.game.flag(kFlag) || app.game.flag(key) || app.runner.running() || app.testNoTutorials)
        return false;
    const uint32_t at = app.events.returnEntry(2);
    if (!at || !app.runner.startAt(app.events, at, 0, app.game, app.area, app.mapNumber)) return false;
    app.runnerWhat = "battle tutorial";
    app.game.vars[1] = key;
    open(app);
    app.runner.setGate(kFlag);
    return true;
}
// A battle frame with the talk on: the script's step (its pages, portraits and pictures), the
// battle held. Once the script clears the flag an event battle's waits again; another ends.
void step(App& app) {
    if (!app.runner.running()) { app.game.setFlag(kFlag, false); return; }
    stepEvents(app);
    if (app.game.flag(kFlag)) return;
    app.skipScene = false; // (the skip button ends with the talk)
    if (!app.battle.eventBattle) app.runner.stop();
}
} // namespace battletalk

void startBattle(App& app, const cr::BattleStart& start, int flags) {
    app.renderer.resetHistory(); // a cut: the upscaler starts over
    App::Battle& b = app.battle;
    const std::array<int, 3> ids = app.battleData.group(start.group);
    if (!app.battleData.loaded() || !ids[0] || start.stage < 0 || start.stage >= cr::kNumBattleStages) {
        app.unsupported.push_back("battle " + std::to_string(start.group) + " on map " + std::to_string(app.mapNumber));
        return;
    }
    b.active = true;
    b.start = start;
    b.flags = flags;
    b.eventBattle = false;
    b.breakFx = {};
    app.runner.setGate(battletalk::kFlag); // PrcEvnt in battle mode
    // CRView::prc: SetFade(white, 255, 255) as the field goes; btl_main 200: SetFade(white, 255, 0,
    // 1000 ms) - the battle comes in out of white over its opening (whatever the field's fade was)
    app.fadeGrey = 1, app.fadeFrom = 1, app.fadeTo = 0, app.fadeFrames = 30, app.fadeT = 0;
    b.frames = 0;
    b.clock = 0;
    b.formation = std::max(0, app.battleData.formation(ids));
    b.actors.clear();
    b.phase = App::Battle::Intro;
    b.phaseFrames = 0;
    b.menu = App::Battle::Top;
    b.cursor = 0;
    b.member = 0;
    b.turnOn = false;
    b.popups.clear();
    b.banner.clear();
    b.bannerFrames = 0;
    b.notices.clear();
    b.levelled.clear();
    b.settled = false;
    b.exit = 0;
    b.quake = {};
    b.voiceRng = uint32_t(SDL_GetTicks()) | 1u;
    for (int& v : b.voiceVariant) { b.voiceRng = b.voiceRng * 1103515245u + 12345u; v = int((b.voiceRng >> 16) % 3); } // reset_variants
    for (bool& c : b.commandSaid) c = false;
    b.voiceRound = -1;
    b.flash = {};
    b.stageAction = 0;
    b.breakPop = 0;
    b.gaugeShown = 0;
    b.cam = -1;
    loadModelFile(app, cr::kBattleStageFile[start.stage], b.stage, false);
    auto add = [&](int modelId, const short* place, float yaw, int fighter) -> App::Battle::Actor& {
        App::Battle::Actor a;
        a.modelId = modelId;
        a.model = cr::kBattleModelFile[modelId];
        if (!b.models.count(a.model)) loadModelFile(app, a.model, b.models[a.model], true);
        a.pos = a.home = {float(place[0]), 0.0f, float(place[1])};
        a.yaw = a.homeYaw = yaw;
        a.fighter = fighter;
        b.actors.push_back(a);
        playSlot(app, b.actors.back(), btlui::kSlotIdle);
        return b.actors.back();
    };
    // The rules: the pair from the party, the monsters grown to their level (the band: the
    // dungeon number, as the treasure boxes use it - the original keeps a per-route byte)
    const int route = std::clamp(app.playerChar / 2, 0, 3);
    cr::BattleRules& rules = b.rules;
    rules = cr::BattleRules();
    rules.useRandom(&app.rng); // the one stream, as the original's
    rules.strike = start.strike;
    rules.escapeBlocked = flags != 0;
    rules.setupParty(app.game, app.itemTable, route);
    const std::string ev = app.area >= 0 && app.area < cr::kNumAreas ? cr::kAreaEventFile[app.area] : "";
    const int band = ev.size() > 7 && ev[5] == 'd' && ev[6] == '0' ? ev[7] - '1' : 0;
    rules.setupSpecial(app.game, app.itemTable, start.group, route); // the story battles' line-ups
    rules.setupEnemies(app.battleData, ids, band);
    for (int k = 0; k < 2; ++k) // Uty_Btl_ParamUp01 / 02 called before this battle began
        if (app.paramUp[k]) {
            for (int st = cr::kStatusAttackUp; st <= cr::kStatusLuckUp; ++st) rules.f[size_t(k)].status[st] = 6;
            app.paramUp[k] = false;
        }
    for (int k = 0; k < 2; ++k) { // the pair (or who fights): j001..j008
        if (!rules.f[size_t(k)].present) continue;
        const int c = rules.f[size_t(k)].character;
        App::Battle::Actor& a = add(c, cr::kBattlePosition[0][k], 3.14159265f, k);
        a.name = sysText(app, 58028 + c + 1); // the name plates' character names
        a.level = rules.f[size_t(k)].level;
    }
    for (int k = 0; k < 3; ++k) {
        const cr::MonsterDef* m = app.battleData.monster(ids[size_t(k)]);
        if (!m || m->model >= cr::kNumBattleModels || !rules.f[size_t(2 + k)].present) continue;
        App::Battle::Actor& a = add(m->model, cr::kBattlePosition[b.formation][k], 0.0f, 2 + k);
        a.enemy = true;
        a.monster = m->id;
        a.level = rules.f[size_t(2 + k)].level;
        a.hp = a.hpMax = uint32_t(rules.f[size_t(2 + k)].hpMax);
        a.name = sysText(app, 53000 + m->id - 1); // font block 2: monster names
    }
    for (int i = 0; i < 5; ++i) {
        b.shownHp[i] = rules.f[size_t(i)].hp;
        b.shownMp[i] = rules.f[size_t(i)].mp;
    }
    setBattleCamera(app, std::max(0, b.formation - 2)); // btl_main 200: the opening turn
    const auto v58 = app.game.vars.find(58);
    const int track = v58 != app.game.vars.end() && v58->second >= 0 && v58->second <= 28 ? v58->second : 8;
    b.fieldTrack = app.music.current();
    playBgm(app, uint32_t(track));
    app.dialogue.clear();
    ++app.battlesFought;
    SDL_Log("battle: group %d on stage %s (formation %d, %s)", start.group, cr::kBattleStageFile[start.stage],
            b.formation, start.strike == 1 ? "first strike" : start.strike == 2 ? "ambush" : "normal");
    updateTitle(app);
}

// What the battle leaves in the save (once): MP back to the party, and when they won the
// experience (levels) and the Oz
void settleBattle(App& app) {
    App::Battle& b = app.battle;
    if (b.settled || !b.active) return;
    b.settled = true;
    b.levelled = b.rules.applyRewards(app.game);
}

// Back to the field: its music again, the encounter count restarted (fldencount_init), and the
// area's EvrStart 2 script (run after battles)
void endBattle(App& app, bool won) {
    app.renderer.resetHistory(); // a cut: the upscaler starts over
    app.sound.stopBattleLines(); // _btlvoice_stop_all
    App::Battle& b = app.battle;
    settleBattle(app);
    std::string names;
    for (const auto& a : b.actors)
        if (a.enemy) names += (names.empty() ? "" : ", ") + (a.name.empty() ? "monster " + std::to_string(a.monster) : a.name);
    unloadLoaded(app, b.stage);
    for (auto& [name, lm] : b.models) unloadLoaded(app, lm);
    b.models.clear();
    for (auto& [id, lm] : app.effectModels) unloadLoaded(app, lm);
    app.effectModels.clear();
    b.effects.clear();
    app.fieldEffects.clear();
    b.actors.clear();
    b.active = false;
    playBgm(app, b.fieldTrack >= 0 ? uint32_t(b.fieldTrack) : 1u << 29);
    cr::EncounterTable table;
    const bool have = cr::encounterTable(app.events, app.mapNumber, table);
    app.encounters.enterMap(have ? &table : nullptr);
    app.lastEvent = (won ? "won against " : "ran from ") + names;
    // Back on the field (fldread_prc): an event battle's script goes on from its Battle; after any
    // other the room's entry script (EvrStart 5) runs again, as the field reloads
    app.runner.setGate(-1);
    app.game.setFlag(battletalk::kFlag, false);
    app.fadeFrom = app.fadeTo = 0, app.fadeT = app.fadeFrames; // (the battle's fade doesn't follow it out)
    if (b.eventBattle) {
        b.eventBattle = false;
    } else {
        app.runner.stop(); // (a battle tutorial cut short)
        app.pendingEntry = App::Entry::Room;
    }
    updateTitle(app);
}

void setBanner(App& app, const std::string& text, int frames = btlui::kBannerFrames) {
    app.battle.banner = text;
    app.battle.bannerFrames = text.empty() ? 0 : frames;
}

// ---- The commands (menub_mainPrc) ----

namespace btlvoice { void command(App& app, int member); }

void battleMenu(App& app, App::Battle::Menu menu) {
    App::Battle& b = app.battle;
    b.menu = menu;
    b.cursor = 0;
    b.pick = b.pickTop = 0;
    if (menu != App::Battle::Targets) setBanner(app, "");
    // the camera: the field of battle for the first menu, the member (or the pair) choosing
    if (menu == App::Battle::Top || menu == App::Battle::EscapeAsk) setBattleCamera(app, b.formation + 12);
    else if (menu == App::Battle::Commands) {
        setBattleCamera(app, b.rules.pair ? 30 : 28 + b.member);
        btlvoice::command(app, b.rules.pair ? 0 : b.member); // __menub_mainPrc: the member's line
    }
}

// The fighters (or items) the menus offer
void battleOpenTargets(App& app, int action) {
    App::Battle& b = app.battle;
    const cr::BattleActionDef* a = app.battleActions.get(action);
    if (!a) return;
    b.picks = b.rules.targets(*a, b.member);
    if (b.picks.empty()) { menuSound(app, se::kBuzzer); return; }
    if (b.rules.isGroupScope(*a)) { // the whole side: no choosing (the plates show who)
    }
    b.menu = App::Battle::Targets;
    b.pick = 0;
    b.targetAction = action;
    cameraForTarget(app, b.picks[0], b.rules.isGroupScope(*a));
    setBanner(app, sysText(app, 57066 + a->scope), 1 << 20); // "Target: ..." while choosing
    menuSound(app, se::kChoose);
}

void battleStartRound(App& app) {
    App::Battle& b = app.battle;
    b.rules.beginRound(b.cmds, app.battleActions);
    for (int k = 0; k < 2; ++k) // defending holds the guard pose through the round
        if (b.rules.f[size_t(k)].guarding)
            if (App::Battle::Actor* a = battleActor(app, k)) playSlot(app, *a, btlui::kSlotGuard);
    b.phase = App::Battle::Acting;
    b.phaseFrames = 0;
    b.turnOn = false;
    setBanner(app, "");
}

// A command is set: the second member's turn (solo), else the round
void battleCommandDone(App& app) {
    App::Battle& b = app.battle;
    menuSound(app, se::kChoose);
    if (!b.rules.pair && b.member == 0 && b.rules.f[1].alive() && !b.rules.allyAuto) {
        b.member = 1;
        battleMenu(app, App::Battle::Commands);
        return;
    }
    battleStartRound(app);
}

void battleChooseTop(App& app, int i) {
    App::Battle& b = app.battle;
    b.cmds[0] = b.cmds[1] = cr::BattleCommand();
    b.pendingItem = b.pendingGene = 0;
    switch (i) {
    case 0: // Solo
        b.rules.pair = false;
        if (!b.rules.f[0].alive() && b.rules.allyAuto) { battleStartRound(app); break; } // only the guest stands
        b.member = b.rules.f[0].alive() ? 0 : 1;
        menuSound(app, se::kChoose);
        battleMenu(app, App::Battle::Commands);
        break;
    case 1: // Pair: both standing (not with a guest)
        if (!b.rules.f[0].alive() || !b.rules.f[1].alive() || b.rules.allyAuto) { menuSound(app, se::kBuzzer); break; }
        b.rules.pair = true;
        b.member = 0;
        menuSound(app, se::kChoose);
        battleMenu(app, App::Battle::Commands);
        break;
    case 2: // Escape: asked first (57015)
        if (b.rules.escapeBlocked) { menuSound(app, se::kBuzzer); break; }
        menuSound(app, se::kChoose);
        battleMenu(app, App::Battle::EscapeAsk);
        b.confirm = 0;
        break;
    default: // Options (_SetOptionMenu): the Options screen over the battle, back here when it closes
        menuSound(app, se::kChoose);
        openOptions(app, 0);
        break;
    }
}

// The genes the member (in pair mode, either of the pair) can use, and the MP to pay with
std::vector<int> battleGeneList(const App& app) {
    const App::Battle& b = app.battle;
    std::vector<int> out = cr::usableAbilities(app.game, b.rules.f[size_t(b.rules.pair ? 0 : b.member)].character);
    if (b.rules.pair)
        for (int g : cr::usableAbilities(app.game, b.rules.f[1].character))
            if (std::find(out.begin(), out.end(), g) == out.end()) out.push_back(g);
    std::sort(out.begin(), out.end());
    return out;
}
int battleGeneMp(const App& app) { // __GetBtlGeneMp: the lower of the pair's
    const App::Battle& b = app.battle;
    return b.rules.pair ? std::min(b.rules.f[0].mp, b.rules.f[1].mp) : b.rules.f[size_t(b.member)].mp;
}
int geneMpCost(const App& app, int ability) { // skill 132 halves it
    const cr::BattleActionDef* a = app.battleActions.get(cr::geneAction(ability));
    return a ? app.battle.rules.geneCost(*a, app.battle.rules.pair ? 0 : app.battle.member) : 0;
}
bool battleGenesSilenced(const App& app) {
    const App::Battle& b = app.battle;
    if (b.rules.pair) return b.rules.f[0].status[cr::kStatusSilence] || b.rules.f[1].status[cr::kStatusSilence];
    return b.rules.f[size_t(b.member)].status[cr::kStatusSilence] > 0;
}

void battleChooseGene(App& app, int ability) {
    App::Battle& b = app.battle;
    const cr::BattleActionDef* a = app.battleActions.get(cr::geneAction(ability));
    if (!a) return;
    if (battleGeneMp(app) < geneMpCost(app, ability)) { // 57081
        setBanner(app, sysText(app, 57081));
        menuSound(app, se::kBuzzer);
        return;
    }
    b.pendingGene = ability;
    cr::BattleCommand& c = b.cmds[b.rules.pair ? 0 : b.member];
    c = cr::BattleCommand();
    c.kind = cr::kCmdGene;
    c.action = a->id;
    if (b.rules.isGroupScope(*a) || a->scope == 1) {
        c.target = b.member;
        b.pendingGene = 0;
        battleCommandDone(app);
        return;
    }
    battleOpenTargets(app, a->id);
}

void battleChooseCommand(App& app, int i) {
    App::Battle& b = app.battle;
    cr::BattleCommand& c = b.cmds[b.rules.pair ? 0 : b.member];
    c = cr::BattleCommand();
    b.pendingItem = 0;
    switch (i) {
    case 0: battleOpenTargets(app, b.rules.attackAction(b.member)); break;
    case 1: // Genes: what the worn plates hold that's known
        b.picks = battleGeneList(app);
        if (b.picks.empty() || battleGenesSilenced(app)) { menuSound(app, se::kBuzzer); break; }
        menuSound(app, se::kChoose);
        b.menu = App::Battle::Genes;
        b.pick = b.pickTop = 0;
        battletalk::tutorial(app, 0xfa8); // the first time: what genes cost
        break;
    case 2: { // Items: what the bag holds of the battle items (21..46)
        b.picks.clear();
        for (int n = 21; n <= 46; ++n)
            if (auto it = app.game.items.find(n); it != app.game.items.end() && it->second > 0 && app.battleActions.get(n + 79))
                b.picks.push_back(n);
        if (b.picks.empty()) {
            setBanner(app, sysText(app, 57080));
            menuSound(app, se::kBuzzer);
            break;
        }
        menuSound(app, se::kChoose);
        b.menu = App::Battle::Items;
        b.pick = b.pickTop = 0;
        break;
    }
    case 3: // Defend
        c.kind = cr::kCmdDefend;
        battletalk::tutorial(app, 0xfa9); // the first time: what it does
        battleCommandDone(app);
        break;
    default: break;
    }
}

void battleChooseItem(App& app, int item) {
    App::Battle& b = app.battle;
    const cr::BattleActionDef* a = app.battleActions.get(item + 79);
    if (!a) return;
    b.pendingItem = item;
    cr::BattleCommand& c = b.cmds[b.rules.pair ? 0 : b.member];
    c = cr::BattleCommand();
    c.kind = cr::kCmdItem;
    c.item = item;
    if (b.rules.isGroupScope(*a) || a->scope == 1) {
        c.target = b.member;
        battleCommandDone(app);
        return;
    }
    battleOpenTargets(app, item + 79);
}

void battleChooseTarget(App& app, int fighter) {
    App::Battle& b = app.battle;
    cr::BattleCommand& c = b.cmds[b.rules.pair ? 0 : b.member];
    if (b.pendingItem) {
        c.kind = cr::kCmdItem;
        c.item = b.pendingItem;
    } else if (b.pendingGene) {
        c.kind = cr::kCmdGene;
        c.action = cr::geneAction(b.pendingGene);
        b.pendingGene = 0;
    } else
        c.kind = cr::kCmdAttack;
    c.target = fighter;
    setBanner(app, "");
    battleCommandDone(app);
}

void battleBack(App& app) {
    App::Battle& b = app.battle;
    switch (b.menu) {
    case App::Battle::Commands:
        if (!b.rules.pair && b.member == 1 && b.rules.f[0].alive()) {
            b.member = 0;
            setBattleCamera(app, 28);
        } else
            battleMenu(app, App::Battle::Top);
        break;
    case App::Battle::Targets: {
        const bool item = b.pendingItem != 0, gene = b.pendingGene != 0;
        battleMenu(app, item ? App::Battle::Items : gene ? App::Battle::Genes : App::Battle::Commands);
        if (item) b.pendingItem = 0;
        if (gene) b.pendingGene = 0, b.picks = battleGeneList(app);
        if (item) {
            b.picks.clear();
            for (int n = 21; n <= 46; ++n)
                if (auto it = app.game.items.find(n); it != app.game.items.end() && it->second > 0 && app.battleActions.get(n + 79))
                    b.picks.push_back(n);
        }
        break;
    }
    case App::Battle::Items: case App::Battle::Genes: battleMenu(app, App::Battle::Commands); break;
    case App::Battle::EscapeAsk: battleMenu(app, App::Battle::Top); break;
    default: return;
    }
    menuSound(app, se::kBack);
}

void battleEscapeAnswer(App& app, bool yes) {
    App::Battle& b = app.battle;
    if (!yes) {
        battleBack(app);
        return;
    }
    b.rules.pair = false;
    b.cmds[0] = b.cmds[1] = cr::BattleCommand();
    b.cmds[0].kind = cr::kCmdEscape;
    menuSound(app, se::kChoose);
    // the escape is the pair's one action this round (both members' commands become it)
    b.rules.pair = true;
    battleStartRound(app);
    b.rules.pair = false;
}

// ---- Playing the round (btlact_*) ----

// The fighter's model faces `target`
void faceFighter(App& app, App::Battle::Actor& a, int target) {
    if (const App::Battle::Actor* t = battleActor(app, target))
        if (t != &a) a.yaw = std::atan2(t->home.x - a.pos.x, t->home.z - a.pos.z);
}

// ---- Screen quakes and flashes (Quake, SetFade_back) ----

// StartQuake(pixels, from %, to %, frames, hold): the offset's size runs from -> to over the
// frames; a held quake goes on at its end until EndQuake, and a passing one doesn't replace it
void startQuake(App::Battle::Quake& q, float amp, float from, float to, int frames, bool hold) {
    if (!hold && q.mode == 2) return;
    q.amp = std::fabs(amp);
    q.now = q.from = std::clamp(from, 0.0f, 100.0f);
    q.to = std::clamp(to, 0.0f, 100.0f);
    q.frame = 0;
    q.frames = std::max(1, frames);
    q.mode = hold ? 2 : 1;
}

void startQuake(App& app, float amp, float from, float to, int frames, bool hold) {
    startQuake(app.battle.quake, amp, from, to, frames, hold);
}

void endQuake(App::Battle::Quake& q, float amp, float from, float to, int frames) { // EndQuake: dies away from -> to
    startQuake(q, amp, from, to, frames, true);
    q.mode = 1;
}
void endQuake(App& app, float amp, float from, float to, int frames) { endQuake(app.battle.quake, amp, from, to, frames); }

// Quake::update, once a frame: each axis 75..100% of the size now, against its last direction
// (and turned again one time in 11)
void stepQuake(App::Battle::Quake& q) {
    if (!q.mode) {
        q.off[0] = q.off[1] = 0;
        return;
    }
    for (float& o : q.off) {
        q.rng = q.rng * 1103515245u + 12345u;
        const int r = int((q.rng >> 16) & 0x7fff);
        float v = float(r % 26 + 75) * 0.01f * q.amp * q.now * 0.01f;
        if (o * v >= 0) v = -v;
        if (r % 11 == 0) v = -v;
        o = v;
    }
    q.now = q.from + (q.to - q.from) * float(q.frame) / float(q.frames);
    if (++q.frame >= q.frames) {
        q.frame = q.frames;
        if (q.mode == 1 || std::fabs(q.now) < 1e-4f) q.mode = 0;
    }
}

void stepQuake(App& app) { stepQuake(app.battle.quake); }

// SetFade_back(colour, from, to, ms): white over the 3D, 50 a frame of the time (PutMask)
void setFlash(App& app, float from, float to, int ms) {
    App::Battle::Flash& f = app.battle.flash;
    f.on = true;
    f.from = from;
    f.to = to;
    f.at = 0;
    f.total = std::max(1, ms);
}

void stepFlash(App& app) {
    App::Battle::Flash& f = app.battle.flash;
    if (!f.on) return;
    f.alpha = f.from + (f.to - f.from) * float(f.at) / float(f.total);
    f.at += 50;
    if (f.at >= f.total) {
        f.at = f.total;
        if (f.alpha <= 0) f.on = false;
    }
}

// ---- The bosses' staged actions (btlact_430..436) ----
// Each runs its own timeline instead of the gene's: the user's motion, a run of cameras (the
// next set as the last one ends), effects at points or on the targets, sounds, quakes and
// flashes at fixed frames, the hit (btlcalc_prc) at the end of it, then a wait.
bool isStagedAction(int action) { return action == 430 || (action >= 432 && action <= 436); }

bool cameraDone(const App& app) { // btlcamera_prc clears G+0x9e0 when its frames are over
    const cr::BattleCamera* c = app.battleData.camera(app.battle.cam);
    return !c || app.battle.camFrame >= std::max<int>(1, c->raw[15]);
}

std::vector<int> turnTargets(const cr::BattleTurn& t) { // the action's targets (+0x7a), each once
    std::vector<int> out;
    for (const cr::BattleHit& h : t.hits)
        if (std::find(out.begin(), out.end(), h.target) == out.end()) out.push_back(h.target);
    return out;
}

void startStaged(App& app, const cr::BattleActionDef* a) {
    App::Battle& b = app.battle;
    const cr::BattleTurn& t = b.turn;
    const int user = t.actors[0];
    App::Battle::Actor* u = battleActor(app, user);
    const int cls = a ? a->cls : 8;
    b.stageAction = t.action;
    b.stageCams = 0;
    b.stageWait = 40;
    if (u && !t.hits.empty()) faceFighter(app, *u, t.hits[0].target);
    switch (t.action) {
    case 430: // Particle Cannon: the charge, the shot, two flashes
        if (u) playSlot(app, *u, cls, btlui::kSlotIdle);
        b.hitFrame = 140;
        spawnEffectAt(app, 0x59, {500, -4, 500});
        app.sound.play(0x7b);
        setBattleCamera(app, 0x207);
        break;
    case 432: { // the slot's loop, 45 frames past its start
        if (u) playSlot(app, *u, cls, -1);
        const cr::BattleAnim an = app.battleData.anim(u ? u->modelId : 0, cls);
        b.hitFrame = an.loopStart + 45;
        app.sound.play(0xbd);
        setBattleCamera(app, 0x1fe);
        break;
    }
    case 433: // the item cameras, frames 82..83 held
        if (u) playSlot(app, *u, cls, -1), u->loopFrom = 82, u->loopTo = 83;
        b.hitFrame = 112;
        if (user < 2) setBattleCamera(app, b.rules.pair ? 0x55 : user + 0x53);
        else if (monsterSize(app, user) == 1) setBattleCamera(app, b.rules.enemyPair ? 0x58 : user + 0x54, user);
        else setBattleCamera(app, monsterSize(app, user) + 0x57, user);
        spawnEffect(app, effects::kCast, user);
        break;
    case 434:
        if (u) playSlot(app, *u, cls, btlui::kSlotIdle);
        b.hitFrame = 130;
        b.stageWait = 65;
        spawnEffect(app, 0x1e, user);
        app.sound.play(0x62);
        setBattleCamera(app, 0x20c);
        break;
    case 435: // frames 103..113 looped
        if (u) playSlot(app, *u, cls, -1), u->loopFrom = 103, u->loopTo = 113;
        b.hitFrame = 163;
        app.sound.play(0xf0);
        app.sound.play(0xf1);
        setBattleCamera(app, 0x215);
        break;
    case 436:
        if (u) playSlot(app, *u, cls, btlui::kSlotIdle);
        b.hitFrame = 61;
        b.stageWait = 50;
        app.sound.play(0x83);
        app.sound.play(0xf0);
        setBattleCamera(app, 0x210);
        break;
    default: break;
    }
}

// One frame of it before the hit (b.turnFrame already counted)
void stepStaged(App& app) {
    App::Battle& b = app.battle;
    const cr::BattleTurn& t = b.turn;
    const int f = b.turnFrame, user = t.actors[0];
    auto nextCamera = [&](int most, int first) {
        if (b.stageCams < most && cameraDone(app)) setBattleCamera(app, first + ++b.stageCams);
    };
    auto onTargets = [&](int id) {
        for (int k : turnTargets(t)) spawnEffect(app, id, k);
    };
    switch (b.stageAction) {
    case 430:
        nextCamera(3, 0x207);
        if (f == 70) {
            spawnEffectAt(app, 0x5a, {500, -2, 500});
            app.sound.play(0x7c);
            startQuake(app, 3, 0, 100, 2, true);
        }
        if (f == 133) endQuake(app, 3, 100, 0, 2);
        if (f == 79) setFlash(app, 0, 255, 150);
        if (f == 82) setFlash(app, 255, 0, 150);
        if (f == 110) setFlash(app, 0, 255, 500);
        if (f == 130) setFlash(app, 255, 0, 500);
        break;
    case 432:
        if (f == 14) app.sound.play(0xc2);
        if (f == 36) {
            if (const App::Battle::Actor* u = battleActor(app, user)) spawnEffectAt(app, 0x19, actorPoint(app, *u, 1206));
            onTargets(0x5b);
            app.sound.play(0x7d);
        }
        break;
    case 434:
        nextCamera(2, 0x20c);
        if (f == 84) app.sound.play(0xd4);
        if (f == 86) setFlash(app, 0, 255, 150);
        if (f == 89) setFlash(app, 255, 0, 150);
        if (f == 100) {
            onTargets(0x22);
            app.sound.play(0x6d);
            startQuake(app, 5, 0, 100, 2, true);
        }
        if (f == 128) endQuake(app, 5, 100, 0, 2);
        break;
    case 435:
        if (f == 103) {
            spawnEffectAt(app, 0x21, {500, 0, 523});
            app.sound.play(0x65);
            setFlash(app, 0, 255, 150);
        }
        if (f == 106) setFlash(app, 255, 0, 150);
        if (f == 133) setFlash(app, 0, 255, 1500);
        if (f == 163) setFlash(app, 255, 0, 500);
        break;
    default: break;
    }
}

// Each frame of the whole action: the cameras that go on past the hit (432, 435, 436); 432's
// fifth looks at its target (0x203, 0x204) or the pair (0x205)
void stepStagedCameras(App& app) {
    App::Battle& b = app.battle;
    if (b.stageAction == 432 && b.stageCams < 5 && cameraDone(app)) {
        ++b.stageCams;
        const int target = b.turn.hits.empty() ? 0 : std::clamp(b.turn.hits[0].target, 0, 1);
        setBattleCamera(app, b.stageCams < 5 ? 0x1fe + b.stageCams : b.rules.pair ? 0x205 : target + 0x203);
    }
    if (b.stageAction == 435 && b.stageCams < 5 && cameraDone(app)) setBattleCamera(app, 0x215 + ++b.stageCams);
    if (b.stageAction == 436 && b.stageCams < 4 && cameraDone(app)) setBattleCamera(app, 0x210 + ++b.stageCams);
}

// The hit's own extras, after showHits
void stagedHit(App& app) {
    App::Battle& b = app.battle;
    App::Battle::Actor* u = battleActor(app, b.turn.actors[0]);
    switch (b.stageAction) {
    case 432:
        for (int k : turnTargets(b.turn)) spawnEffect(app, 0x5c, k);
        app.sound.play(0x7e);
        if (u) u->then = btlui::kSlotIdle; // mdl_set_act_nextanm: the loop plays out, then stands
        break;
    case 433: setBattleCamera(app, b.formation + 12); break;
    case 435:
        if (u) u->then = btlui::kSlotIdle;
        break;
    case 436: startQuake(app, 10, 100, 0, 10, false); break;
    default: break;
    }
}

// ---- Battle voices (the voiced releases: 2.2.0's _btlvoice_*, its voicebtl.bin) ----
// A line is the clip table[speaker x 1000 + situation - 1] (sys.evt section +0x40), played with
// tag speaker x 1000: that speaker's previous line stops first. Each fighter cycles through three
// variants of a situation. Nothing here plays without the table (the iPad's 1.0.1 has none).
namespace btlvoice {

bool on(const App& app) { return !app.battleVoiceTable.empty() && app.sound.voiceClips(true) > 0; }

int rnd(App& app, int n) {
    App::Battle& b = app.battle;
    b.voiceRng = b.voiceRng * 1103515245u + 12345u;
    return n > 1 ? int((b.voiceRng >> 16) % uint32_t(n)) : 0;
}

// _btlvoice_speaker: from the fighter's name id (the party: its character x 10, +3 for
// character 7 under flag 0x997 or flag 0x996 on the fourth pair; monsters: id - 1, ids 71..73
// as id + 2 under the same flags): 0..7 the eight characters (x0..x3), 80 -> 11, 90..93 -> 8,
// 94..98 -> 9, 110..113 -> 10, 120..123 -> 12; battle models 7, 87, 88 are always speaker 7
int speaker(const App& app, int k) {
    const App::Battle& b = app.battle;
    if (k < 0 || k > 4) return -1;
    const cr::Fighter& f = b.rules.f[size_t(k)];
    if (!f.present) return -1;
    const bool alt = (app.game.flag(0x996) && app.playerChar / 2 == 3) || app.game.flag(0x997);
    int name, model = -1;
    if (k < 2) {
        name = f.character * 10;
        if (name >= 70 && name < 73 && alt) name += 3;
        for (const App::Battle::Actor& a : b.actors)
            if (a.fighter == k) model = a.modelId;
    } else {
        name = f.monster - 1;
        if (f.monster >= 0x47 && f.monster <= 0x49 && alt) name = f.monster + 2;
        model = f.def ? f.def->model : -1;
    }
    int sp = -1;
    static const int kSpan[8] = {4, 4, 4, 3, 4, 3, 4, 3}; // name ids per character (x0 .. x3)
    if (name >= 0 && name < 80) {
        if (name % 10 < kSpan[name / 10]) sp = name / 10;
    } else if (name >= 80) {
        const int u = name - 80;
        sp = u == 0 ? 11 : u >= 10 && u <= 13 ? 8 : u >= 14 && u <= 18 ? 9 : u >= 30 && u <= 33 ? 10 : u >= 40 && u <= 43 ? 12 : -1;
    }
    if (model == 7 || model == 87 || model == 88) sp = 7;
    return sp;
}

int clipFor(const App& app, int sp, int situation) {
    const size_t i = size_t(sp * 1000 + situation - 1);
    return sp >= 0 && situation > 0 && i < app.battleVoiceTable.size() ? app.battleVoiceTable[i] : 0;
}

// _btlvoice_play: fighter k says the next of its three variants (-1: nothing)
void play(App& app, int k, int a, int b2, int c) {
    if (!on(app)) return;
    const int sp = speaker(app, k);
    if (sp < 0) return;
    int& v = app.battle.voiceVariant[size_t(k)];
    const int s = v == 0 ? a : v == 1 ? b2 : c;
    v = (v + 1) % 3;
    if (s == -1) return;
    const int clip = clipFor(app, sp, s);
    if (clip > 0) app.sound.playBattleLine(clip, sp * 1000);
}

bool living(const App& app, int k) { return k >= 0 && k < 5 && app.battle.rules.f[size_t(k)].alive(); }

// One of two at random among the living (the first if neither)
int eitherOf(App& app, int x, int y) {
    const bool lx = living(app, x), ly = living(app, y);
    if (lx && ly) return rnd(app, 2) ? y : x;
    return ly ? y : x;
}

// _btlvoice_choose_speaker: 99 either of the party; a side acting as a pair, either of it
int chooseSpeaker(App& app, int k) {
    const cr::BattleRules& r = app.battle.rules;
    if (k == 99) return r.f[1].present ? eitherOf(app, 0, 1) : 0;
    if (k >= 2 && r.enemyPair && (k == 2 || k == 3)) return eitherOf(app, 2, 3);
    if (k < 2 && r.pair) return eitherOf(app, 0, 1);
    return k;
}

void playSide(App& app, int k, int a, int b2, int c) { play(app, chooseSpeaker(app, k), a, b2, c); }

// _btlvoice_pick_speakers, each round: who of each side speaks for it
void pickSpeakers(App& app) {
    App::Battle& b = app.battle;
    for (bool& s : b.voiceSpeaks) s = false;
    b.voiceSpeaks[size_t(eitherOf(app, 0, 1))] = true;
    std::vector<int> foes;
    for (int k = 2; k < 5; ++k)
        if (living(app, k)) foes.push_back(k);
    if (!foes.empty()) b.voiceSpeaks[size_t(foes[size_t(rnd(app, int(foes.size())))])] = true;
}

bool mayReact(const App& app, int k) { // a side acting as a pair reacts through its speaker only
    const App::Battle& b = app.battle;
    const bool pairSide = k < 2 ? b.rules.pair : b.rules.enemyPair;
    return !pairSide || b.voiceSpeaks[size_t(k)];
}

// _btlvoice_listed_battle: these battles' monsters coming back say 0x1f..0x21 instead
bool listedBattle(const App& app) {
    static const int kList[] = {260, 261, 262, 280, 281, 282, 300, 374, 302, 320, 321, 322};
    return std::find(std::begin(kList), std::end(kList), app.battle.start.group) != std::end(kList);
}

// _btlvoice_hp_change (from btlcalc_chk_dead): fighter k's HP goes from `before` to `after`
void hpChange(App& app, int k, int before, int after) {
    if (!on(app) || speaker(app, k) < 0 || !mayReact(app, k)) return;
    const cr::Fighter& f = app.battle.rules.f[size_t(k)];
    if (after <= 0) play(app, k, 0x22, 0x23, 0x24); // down
    else if (before <= 0) {
        if (k >= 2 && listedBattle(app)) play(app, k, 0x1f, 0x20, 0x21);
        else play(app, k, 0x1c, 0x1d, 0x1e);
    } else if (before > f.hpMax / 4) play(app, k, 0x16, 0x17, 0x18); // hit
    else play(app, k, 0x19, 0x1a, 0x1b);                             // hit, low already
}

void healed(App& app, int k) { // _btlvoice_healed: to full, or not
    if (!on(app) || speaker(app, k) < 0 || !mayReact(app, k)) return;
    const cr::Fighter& f = app.battle.rules.f[size_t(k)];
    if (f.hp >= f.hpMax) play(app, k, 0x28, 0x29, 0x2a);
    else play(app, k, 0x25, 0x26, 0x27);
}

// _btlvoice_attack: how the targets fared - 0 struck, 1 a critical, 2 one fell, 3 a miss or a
// guard, 4 a strong one fell
void attack(App& app, const cr::BattleTurn& t) {
    if (!on(app) || speaker(app, t.actors[0]) < 0) return;
    int how = 0;
    for (const cr::BattleHit& h : t.hits) {
        const cr::Fighter& f = app.battle.rules.f[size_t(std::clamp(h.target, 0, 4))];
        if (h.type == cr::kHitMiss || h.type == cr::kHitGuard || h.amount == 0) how = std::max(how, 3);
        if (f.hp <= 0 || h.fell) how = std::max(how, f.def && f.def->strongGrowth ? 4 : 2);
        else if (h.type == cr::kHitCritical && how == 0) how = 1;
    }
    static const int kLine[5] = {0x2e, 0x31, 0x34, 0x37, 0x3a};
    playSide(app, t.actors[0], kLine[how], kLine[how] + 1, kLine[how] + 2);
}

int halfOrLess(const App& app, int k) {
    const cr::Fighter& f = app.battle.rules.f[size_t(k)];
    return f.hp <= f.hpMax / 2;
}

// _btlvoice_command_hurt: the pair's command, one of it at half HP or less (10..12) or not (7..9)
void commandHurt(App& app, int member) {
    const bool hurt = (living(app, 0) && halfOrLess(app, 0)) || (living(app, 1) && halfOrLess(app, 1));
    playSide(app, member, hurt ? 10 : 7, hurt ? 11 : 8, hurt ? 12 : 9);
}

// _btlvoice_command: a member's commands open (once a round): 1..3, at half HP or less 4..6,
// the partner down 13..15 / 16..18; battles 356..359 the pair's lines instead
void command(App& app, int member) {
    App::Battle& b = app.battle;
    if (!on(app) || member < 0 || member > 1 || b.commandSaid[size_t(member)]) return;
    b.commandSaid[size_t(member)] = true;
    if (b.rules.pair) {
        b.commandSaid[0] = b.commandSaid[1] = true;
        commandHurt(app, member);
        return;
    }
    const int partner = member ^ 1;
    const int idx = halfOrLess(app, member) | ((b.rules.f[size_t(partner)].present && !living(app, partner)) ? 2 : 0);
    if (idx < 2 && b.start.group >= 0x164 && b.start.group <= 0x167) { commandHurt(app, member); return; }
    static const int kLine[4] = {1, 4, 13, 16};
    playSide(app, member, kLine[idx], kLine[idx] + 1, kLine[idx] + 2);
}

// _btlvoice_turn: as a round's order is made, its first actor's action lines (the record's
// +10.. for the party and the voiced characters, +16.. for the other monsters)
void turn(App& app, int k, int action) {
    if (!on(app)) return;
    const cr::BattleActionDef* a = app.battleActions.get(action);
    const int sp = speaker(app, k);
    if (!a || sp < 0) return;
    const bool own = sp < 8 || k < 2;
    const int who = own ? chooseSpeaker(app, k) : (app.battle.rules.enemyPair && (k == 2 || k == 3) ? eitherOf(app, 2, 3) : k);
    const int* v = own ? a->voice : a->voice + 3;
    play(app, who, v[0], v[1], v[2]);
}

// _btlvoice_victory: scripted battles' own lines (0xc93d0, by the pair), else 500..502 / with
// someone down or at a quarter HP or less 503..505
void victory(App& app) {
    if (!on(app)) return;
    App::Battle& b = app.battle;
    static const short kScripted[4][13][2] = {
        {{260, 621}, {261, 622}, {340, 623}, {348, 624}, {344, 625}, {352, 626}, {356, 627}, {357, 627}, {358, 627}, {359, 627}, {368, 628}, {262, 629}, {-1, 0}},
        {{280, 621}, {281, 622}, {341, 623}, {283, 624}, {349, 625}, {345, 626}, {353, 627}, {356, 628}, {357, 628}, {358, 628}, {359, 628}, {368, 629}, {282, 630}},
        {{300, 621}, {374, 622}, {342, 623}, {346, 624}, {377, 625}, {354, 626}, {356, 627}, {357, 627}, {358, 627}, {359, 627}, {368, 628}, {302, 629}, {-1, 0}},
        {{320, 621}, {321, 622}, {343, 623}, {351, 624}, {347, 625}, {355, 626}, {356, 627}, {357, 627}, {358, 627}, {359, 627}, {368, 628}, {322, 629}, {-1, 0}}};
    const int route = std::clamp(app.playerChar / 2, 0, 3);
    int line = 0;
    for (const auto& e : kScripted[route])
        if (e[0] == b.start.group) line = e[1];
    const int who = chooseSpeaker(app, 99);
    const int sp = speaker(app, who);
    if (sp < 0) return;
    if (!line) {
        bool hurt = false;
        for (int k = 0; k < 2; ++k)
            if (b.rules.f[size_t(k)].present) hurt |= !living(app, k) || b.rules.f[size_t(k)].hp <= b.rules.f[size_t(k)].hpMax / 4;
        int& v = b.voiceVariant[size_t(who)];
        line = (hurt ? 503 : 500) + v;
        v = (v + 1) % 3;
    }
    const int clip = clipFor(app, sp, line);
    if (clip > 0) app.sound.playBattleLine(clip, sp * 1000);
}

} // namespace btlvoice

void startTurn(App& app) {
    App::Battle& b = app.battle;
    const cr::BattleTurn& t = b.turn;
    if (b.voiceRound != b.rules.round) { // btlque_make_que: the round's order made
        b.voiceRound = b.rules.round;
        btlvoice::turn(app, t.actors[0], t.action);
        btlvoice::pickSpeakers(app);
    }
    if (t.kind == cr::kCmdEscape && t.escaped) btlvoice::playSide(app, 0, 0x13, 0x14, 0x15); // btlact_escape_init
    else if (t.kind == cr::kCmdAttack) btlvoice::attack(app, t);                 // btlact_attack_init
    b.turnFrame = 0;
    b.hitsShown = false;
    b.hitFrame = 20;
    if (t.kind == cr::kCmdEscape) {
        setBanner(app, sysText(app, t.escaped ? 57000 : 57001));
        b.hitFrame = 0; // btlact_escape: away - 30 frames (2.2.0; 1.0.1 10, no line to wait for)
        return;          // then 40; stayed - 40
    }
    const cr::BattleActionDef* a = app.battleActions.get(t.action);
    setBanner(app, sysText(app, 54000 + t.action - 1));
    for (int k : t.actors) // the gene's MP leaves as it's cast
        if (k >= 0) b.shownMp[k] = b.rules.f[size_t(k)].mp;
    b.stageAction = 0;
    if (isStagedAction(t.action) && t.actors[0] >= 2) { // btlact_430_init.. instead of btlact_gene_init
        startStaged(app, a);
        return;
    }
    cameraForTurn(app, t);
    if (t.kind == cr::kCmdGene || t.kind == cr::kCmdItem) // btlact_gene_init / item_init: the casting glow
        for (int k : t.actors)
            if (k >= 0) spawnEffect(app, effects::kCast, k);
    if (t.kind == cr::kCmdItem && t.item > 0) { // the bag gives one up
        auto it = app.game.items.find(t.item);
        if (it != app.game.items.end() && --it->second <= 0) app.game.items.erase(it);
    }
    const int target = t.hits.empty() ? -1 : t.hits[0].target;
    const bool melee = a && a->motion == 1 && target >= 0 && (target < 2) != (t.actors[0] < 2);
    for (int k = 0; k < 2; ++k) {
        App::Battle::Actor* u = t.actors[k] >= 0 ? battleActor(app, t.actors[k]) : nullptr;
        if (!u) continue;
        int slot = a && a->cls >= 8 && a->cls < cr::kBattleAnimSlots ? a->cls : 8;
        if (t.kind == cr::kCmdAttack && t.actors[1] >= 0 && k == 1) slot = 9; // the pair attack's second
        if (target >= 0) faceFighter(app, *u, target);
        playSlot(app, *u, slot, melee ? btlui::kSlotReturn : btlui::kSlotIdle);
        const cr::BattleAnim an = app.battleData.anim(u->modelId, slot);
        if (k == 0) b.hitFrame = an.hit > 0 ? an.hit : t.kind == cr::kCmdAttack ? 18 : 30;
        u->moveFrom = u->moveTo = 0;
        if (melee) { // btlmove_setpos: up to the target, a step short of it
            const App::Battle::Actor* tv = battleActor(app, target);
            if (tv) {
                const LoadedModel &lu = b.models[u->model], &lt = b.models[tv->model];
                cr::Vec3 d = u->home - tv->home;
                const float len = std::max(1.0f, std::sqrt(d.x * d.x + d.z * d.z));
                const float reach = 0.6f * (lu.radius * u->scale + lt.radius * tv->scale) + (k ? 18.0f : 0.0f);
                u->from = u->pos;
                u->to = tv->home + cr::Vec3{d.x / len * reach, 0, d.z / len * reach};
                if (k == 1) u->to = u->to + cr::Vec3{d.z / len * 18, 0, -d.x / len * 18}; // beside the first
                u->moveFrom = an.moveEnd > an.moveStart ? an.moveStart : 0;
                u->moveTo = an.moveEnd > an.moveStart ? an.moveEnd : std::max(1, b.hitFrame - 4);
            }
        }
    }
}

// The hits land: the numbers, the targets' reactions, the HP the HUD shows
void showHits(App& app) {
    App::Battle& b = app.battle;
    b.hitsShown = true;
    if (b.turn.message > 0) { // stolen Oz, a monster running, Analyze, the barrier, the Apples
        std::string name = b.turn.message == 57032 ? sysText(app, b.turn.messageValue)
                           : b.turn.message == 57005 ? itemName(app, b.turn.messageValue) : std::string();
        if (b.turn.message == 57000 || b.turn.message == 57001)
            if (App::Battle::Actor* u = battleActor(app, b.turn.actors[0])) name = u->name;
        std::vector<std::vector<std::string>> pages;
        const int value = b.turn.messageValue;
        if (app.systemText.message(b.turn.message, app.language, [&](int) { return value; }, pages, nullptr,
                                   [&](int) { return name; }) &&
            !pages.empty() && !pages[0].empty())
            setBanner(app, pages[0][0], btlui::kMessageFrames);
    }
    for (const cr::BattleNotice& n : b.turn.notices) // btl_add_msgque: one waiting per fighter
        if (std::none_of(b.notices.begin(), b.notices.end(), [&](const cr::BattleNotice& q) { return q.fighter == n.fighter; }) &&
            b.notices.size() < 5)
            b.notices.push_back(n);
    if (b.turn.gone) // the monster leaves
        if (App::Battle::Actor* u = battleActor(app, b.turn.actors[0])) u->fading = true;
    bool any = false, crit = false, guarded = false;
    for (const cr::BattleHit& h : b.turn.hits) {
        App::Battle::Actor* a = battleActor(app, h.target);
        const cr::Fighter& f = b.rules.f[size_t(h.target)];
        const int hpBefore = b.shownHp[h.target];
        if (h.revived) btlvoice::playSide(app, h.target, 0x2b, 0x2c, 0x2d); // btlcalc_0424
        else if (h.amount < 0 && !h.mp) btlvoice::healed(app, h.target);   // btlcalc_dmg_hp
        else if (h.amount > 0 && !h.mp) btlvoice::hpChange(app, h.target, hpBefore, f.hp); // btlcalc_chk_dead
        b.shownHp[h.target] = f.hp;
        b.shownMp[h.target] = f.mp;
        if (!a || h.type == cr::kHitQuiet) continue;
        App::Battle::Popup p;
        p.at = actorHead(app, *a);
        p.amount = h.amount;
        p.type = h.type;
        p.mp = h.mp;
        p.timer = btlui::kPopupTime;
        b.popups.push_back(p);
        if (h.amount > 0) any = true;
        crit |= h.type == cr::kHitCritical;
        guarded |= h.type == cr::kHitGuard;
        if (h.revived) playSlot(app, *a, btlui::kSlotIdle);
        else if (h.fell) {
            if (a->enemy) a->fading = true; // btlcalc_chk_dead: monsters fade out over 20 frames
            playSlot(app, *a, a->enemy ? btlui::kSlotHit : btlui::kSlotDown, a->enemy ? -2 : -1);
        } else if (h.amount > 0)
            playSlot(app, *a, btlui::kSlotHit, f.guarding ? btlui::kSlotGuard : btlui::kSlotIdle);
        else if (h.type == cr::kHitGuard)
            playSlot(app, *a, btlui::kSlotEvade, f.guarding ? btlui::kSlotGuard : btlui::kSlotIdle);
    }
    // The calcs' effects (eff3d_set_btl in btlcalc_*)
    if (const cr::BattleActionDef* act = app.battleActions.get(b.turn.action)) {
        const int user = b.turn.actors[0];
        const int ue = user >= 0 ? b.rules.f[size_t(user)].element : 0;
        bool area = false;
        for (const cr::BattleHit& h : b.turn.hits) {
            using namespace effects;
            const int t = h.target;
            int id = -1;
            switch (act->calc) {
            case 0: // btlcalc_0404: the element's hit, or the plain / critical one
                if (h.type == cr::kHitGuard || h.type == cr::kHitMiss || h.amount <= 0) break;
                id = h.type == cr::kHitCritical ? kCritical : ue > 0 ? 0x25 + 3 * ue : kHit;
                startQuake(app, 3, 100, 0, 5, false); // and the screen jolts (StartQuake(3, 100, 0, 5))
                break;
            case 1: // 0406: elemental genes - one burst over a whole side, else on each target
                if (h.amount >= 0 && h.type != cr::kHitGuard && h.type != cr::kHitMiss) startQuake(app, 3, 100, 0, 5, false);
                if (act->element <= 0 || h.amount < 0) break;
                if (act->scope == 5 || act->scope == 3) {
                    if (!area) spawnEffectAt(app, 0x24 + 3 * act->element, {500, 0, t < 2 ? 523.0f : 472.0f});
                    area = true;
                } else
                    id = kGene + 3 * act->element;
                break;
            case 2: case 3: case 4: case 5: case 6: case 9: case 10: id = kFixed; break;
            case 7: id = t == user ? -1 : kSacrifice; break;
            case 8: id = (t < 2) == (user < 2) ? kHeal : kDrain; break;
            case 12: case 13: case 14: case 15: case 16: case 17: case 18: case 22: case 23: case 35: case 36: id = kHeal; break;
            case 19: id = kRevive; break;
            case 24: id = kAnalyze; break;
            case 25: id = act->element > 0 ? 0x31 + act->element : -1; break;
            case 26: id = kBuff; break;
            case 27: case 28: case 29: case 30: case 32: case 33: case 34: id = kStatus; break;
            case 31: id = kDrain; break;
            default: break;
            }
            if (id >= 0) spawnEffect(app, id, t);
            if (h.fell && t >= 2) { // btlcalc_chk_dead: monsters without strong growth burst
                const cr::Fighter& f = b.rules.f[size_t(t)];
                if (f.def && !f.def->strongGrowth) spawnEffect(app, kFall, t);
            }
        }
    }
    // __effgo: 0x56 a critical, 0x53..0x55 a hit, 0x5d fended off, 0x57 healing
    if (crit) app.sound.play(0x56);
    else if (any) app.sound.play(0x53 + uint32_t(b.rules.random(3)));
    else if (guarded) app.sound.play(0x5d);
    else if (!b.turn.hits.empty() && b.turn.hits[0].amount < 0) app.sound.play(0x57);
    if (b.rules.breakGauge != b.gaugeShown && b.rules.breakGauge != 0 && (b.gaugeShown == 0 || (b.gaugeShown > 0) != (b.rules.breakGauge > 0)))
        b.breakPop = 5.0f; // btlbreak_set: the label pops when a side takes the Break
}

// btlbreak_effect_init / _prc: two white stripes at 30 degrees (alpha up to 152), "Player" and
// "Enemy" sliding along them (window_hd (704, 328) / (864, 328), 160 x 104), "BREAK!!" (512, 432,
// 512 x 72) and "The battle situation changed" (512, 504, 512 x 32) sliding in from opposite
// sides on 480 sin(t): 5 frames in, 30 held (at 15 the other side takes the Break, sound 0x7a),
// 5 out (sound 0x48, or 0x49 when the enemies hold it). Sound 0x47 as it starts.
void startBreakFx(App& app) {
    App::Battle::BreakFx& fx = app.battle.breakFx;
    fx = App::Battle::BreakFx();
    fx.on = true;
    app.sound.play(0x47);
}
bool stepBreakFx(App& app) {
    App::Battle& b = app.battle;
    App::Battle::BreakFx& fx = b.breakFx;
    const float deg = 3.14159265f / 180.0f;
    if (fx.state == 0) {
        fx.t += 18;
        fx.slide = int(480.0f * std::sin(float(fx.t) * deg));
        fx.alpha = std::min(152, fx.alpha + 64);
        fx.pos += 96;
        if (fx.t >= fx.limit) fx.t = 0, fx.limit = 30, fx.state = 10;
    } else if (fx.state == 10) {
        ++fx.t;
        if (fx.t == fx.limit / 2) {
            b.rules.takeBreak();
            b.breakPop = 5.0f;
            app.sound.play(0x7a);
        }
        if (fx.t >= fx.limit) {
            fx.t = 0, fx.limit = 90, fx.state = 0x3e6;
            app.sound.play(b.rules.breakGauge < 1 ? 0x49 : 0x48);
        }
    } else if (fx.state == 0x3e6) {
        fx.t += 18;
        fx.slide = int(480.0f * std::sin(float(fx.t) * deg) + 480.0f);
        fx.alpha = std::max(0, fx.alpha - 64);
        fx.pos += 96;
        if (fx.t >= fx.limit) fx.state = 999;
    } else
        return true;
    return false;
}
void drawBreakFx(App& app, cr::MenuPainter& m) {
    const App::Battle::BreakFx& fx = app.battle.breakFx;
    if (!fx.on) return;
    const float a = float(fx.alpha) / 255.0f, white[4] = {1, 1, 1, a};
    if (app.renderer.phoneLayout()) { // 2.2.0 (the Retina path): the stripes into the iPhone screen's
        // corners ((0, 160)-(280, 0)-(0, 216)-(376, 0), (160, 320)-(480, 140)-(256, 320)-(480, 192)),
        // "Player" at (pos cos 30 + 130, 10 - pos sin 30), "Enemy" at (234 - pos cos 30, pos sin 30
        // + 208), "BREAK!!" (528, 432, 480 x 72) at (slide - 480, 120), the line (528, 504, 480 x 32)
        // at (480 - slide, 192)
        for (float x = 0; x < 480; x += 2) {
            const float top1 = 160 - 160 * x / 280, bot1 = 216 - 216 * x / 376;
            if (bot1 > top1 && bot1 > 0) m.quad(x + 16, std::max(top1, 0.0f) + 32, 2, bot1 - std::max(top1, 0.0f), white, white);
            if (x >= 160) {
                const float top2 = std::max(140.0f, 320 - 180 * (x - 160) / 320);
                const float bot2 = x < 256 ? 320 : std::max(192.0f, 320 - 128 * (x - 256) / 224);
                if (bot2 > top2) m.quad(x + 16, top2 + 32, 2, bot2 - top2, white, white);
            }
        }
        const float c = std::cos(30 * 3.14159265f / 180), s = std::sin(30 * 3.14159265f / 180), pos = float(fx.pos);
        m.piece(pos * c + 130 + 16, 10 - pos * s + 32, 704, 328, 160, 104);
        m.piece(234 - pos * c + 16, pos * s + 208 + 32, 864, 328, 160, 104);
        m.piece(float(fx.slide) - 480 + 16, 120 + 32, 528, 432, 480, 72);
        m.piece(480 - float(fx.slide) + 16, 192 + 32, 528, 504, 480, 32);
        return;
    }
    // tex2d -> menu units: + (16, 32). The stripes as 2-unit columns between their edges
    for (float x = -16; x < 496; x += 2) {
        const float top1 = 128 - 160 * (x + 16) / 280, bot1 = 184 - 216 * (x + 16) / 376;
        if (bot1 > top1 && bot1 > -32) m.quad(x + 16, std::max(top1, -32.0f) + 32, 2, bot1 - std::max(top1, -32.0f), white, white);
        if (x >= 176) {
            const float top2 = std::max(172.0f, 352 - 180 * (x - 176) / 320);
            const float bot2 = x < 272 ? 352 : std::max(224.0f, 352 - 128 * (x - 272) / 224);
            if (bot2 > top2) m.quad(x + 16, top2 + 32, 2, bot2 - top2, white, white);
        }
    }
    const float c = std::cos(30 * 3.14159265f / 180), s = std::sin(30 * 3.14159265f / 180), pos = float(fx.pos);
    m.piece(pos * c + 130 - 16 + 16, 10 - pos * s - 32 + 32, 704, 328, 160, 104);
    m.piece(234 - pos * c + 16 + 16, pos * s + 208 + 32 + 32, 864, 328, 160, 104);
    m.piece(float(fx.slide) - 496 + 16, 114 + 32, 512, 432, 512, 72);
    m.piece(464 - float(fx.slide) + 16, 186 + 32, 512, 504, 512, 32);
}

// The enemies are down (btl_main 1000): the victory pose and tune, then the results
void battleWon(App& app) {
    App::Battle& b = app.battle;
    b.phase = App::Battle::Won;
    b.phaseFrames = 0;
    btlvoice::victory(app); // btl_main 0x3e9
    int& bank = app.game.walkExp[size_t(fieldRoute(app))]; // skill 174's walking EXP joins the battle's
    if (!b.settled) b.rules.expGained += bank, bank = 0;   // (before the Apples double it)
    settleBattle(app);
    { // var 59: the victory tune (10 for random encounters)
        const auto v59 = app.game.vars.find(59);
        playBgm(app, uint32_t(v59 != app.game.vars.end() && v59->second >= 0 && v59->second <= 28 ? v59->second : 10));
    }
    for (int k = 0; k < 2; ++k)
        if (App::Battle::Actor* a = battleActor(app, k); a && b.rules.f[size_t(k)].alive()) {
            a->yaw = a->homeYaw;
            playSlot(app, *a, btlui::kSlotWin);
        }
}

void endRound(App& app) {
    App::Battle& b = app.battle;
    battletalk::rules(app); // (status_prc's boss rules as the round ends)
    b.rules.endRound();
    for (int k = 0; k < 2; ++k) // the guard pose ends with the round
        if (App::Battle::Actor* a = battleActor(app, k); a && a->slot == btlui::kSlotGuard) playSlot(app, *a, btlui::kSlotIdle);
    for (int i = 0; i < 5; ++i) b.shownHp[i] = b.rules.f[size_t(i)].hp, b.shownMp[i] = b.rules.f[size_t(i)].mp;
    switch (b.rules.outcome()) {
    case 2: // won: the victory pose and tune, then the results (btl_main 0x44b) - a story boss's
        // last word first (8000)
        if (battletalk::end(app)) b.phase = App::Battle::WinTalk, b.phaseFrames = 0;
        else battleWon(app);
        break;
    case 3: { // lost (0x4b1): the defeat tune and "... lost the battle"
        b.phase = App::Battle::Lost;
        b.phaseFrames = 0;
        playBgm(app, 0x1a);
        btlvoice::playSide(app, 99, 0x1f, 0x20, 0x21); // btl_main 0x4b0 (as for a pair)
        setBattleCamera(app, 0x48);
        const App::Battle::Actor* lead = battleActor(app, 0);
        setBanner(app, sysText(app, 57016, lead ? lead->name : std::string()), 1 << 20);
        break;
    }
    case 1:
        b.phase = App::Battle::Fled;
        b.phaseFrames = 0;
        settleBattle(app);
        break;
    default:
        b.phase = App::Battle::Command;
        b.phaseFrames = 0;
        b.member = 0;
        b.commandSaid[0] = b.commandSaid[1] = false; // +0xafc / +0xafd
        battleMenu(app, App::Battle::Top);
        battletalk::between(app); // btl_main 899
        break;
    }
}

// One 30 Hz battle frame: the actors' motions and moves, the round, the HUD's counters
void stepBattle(App& app) {
    App::Battle& b = app.battle;
    ++b.frames;
    ++b.phaseFrames;
    ++b.camFrame;
    stepEffects(app);
    stepSceneEffects(app);
    for (App::Battle::Actor& a : b.actors) {
        const float dur = actorDuration(app, a);
        const cr::BattleAnim an = app.battleData.anim(a.modelId, a.slot);
        a.time += 1.0f;
        if (a.then == -2) { // held on its last frame (a fallen monster)
            a.time = std::max(dur - 1, 0.0f);
        } else if (a.then >= 0) {
            if (a.time >= std::max(dur, 1.0f)) {
                const int next = a.then;
                const bool back = next == btlui::kSlotReturn;
                playSlot(app, a, next, back ? btlui::kSlotIdle : -1);
                if (back) { // and walks back to its place meanwhile
                    const cr::BattleAnim ra = app.battleData.anim(a.modelId, btlui::kSlotReturn);
                    a.from = a.pos;
                    a.to = a.home;
                    a.moveFrom = ra.moveEnd > ra.moveStart ? ra.moveStart : 0;
                    a.moveTo = ra.moveEnd > ra.moveStart ? ra.moveEnd : std::max(1, int(actorDuration(app, a)));
                } else if (next == btlui::kSlotIdle) {
                    a.pos = a.home;
                    a.yaw = a.homeYaw;
                    a.moveFrom = a.moveTo = 0;
                }
            }
        } else if (a.loopTo > a.loopFrom && a.time >= float(a.loopTo))
            a.time = float(a.loopFrom) + (a.time - float(a.loopTo));
        else if (a.loopTo <= a.loopFrom && an.loopEnd > an.loopStart && a.time >= float(an.loopEnd))
            a.time = float(an.loopStart) + (a.time - float(an.loopEnd));
        else if (dur > 0 && a.time >= dur)
            a.time = std::fmod(a.time, dur);
        if (a.moveTo > a.moveFrom) {
            const float t = std::clamp((a.time - float(a.moveFrom)) / float(a.moveTo - a.moveFrom), 0.0f, 1.0f);
            a.pos = a.from + (a.to - a.from) * t;
        }
        if (a.fading) a.alpha = std::max(0.0f, a.alpha - 1.0f / 20.0f);
    }
    // The badge's gauge follows the rules' a step a frame (menub_mainPut), the pop shrinks
    if (b.gaugeShown != b.rules.breakGauge) b.gaugeShown += b.gaugeShown < b.rules.breakGauge ? 1 : -1;
    if (b.breakPop >= 1.0f) b.breakPop -= 4.0f / 7.0f;
    if (b.bannerFrames > 0) --b.bannerFrames;
    if (b.bannerFrames == 0 && !b.notices.empty()) { // menub_mainPut: the next queued message
        const cr::BattleNotice n = b.notices.front();
        b.notices.erase(b.notices.begin());
        std::string name; // var 9 the item (Steal Item), else var 0 the fighter
        if (n.fighter < 0) name = itemName(app, -n.fighter);
        else if (const App::Battle::Actor* u = battleActor(app, n.fighter)) name = u->name;
        setBanner(app, sysText(app, 57000 + n.entry, name), btlui::kMessageFrames);
    }
    for (App::Battle::Popup& p : b.popups) p.timer -= 16;
    b.popups.erase(std::remove_if(b.popups.begin(), b.popups.end(), [](const App::Battle::Popup& p) { return p.timer < 0; }),
                   b.popups.end());

    battletalk::rules(app);
    if (app.game.flag(battletalk::kFlag)) { // a talk: the script runs (its fades with it), the battle waits
        battletalk::step(app);
        stepQuake(app);
        stepFlash(app);
        return;
    }
    if (app.fadeT < app.fadeFrames) ++app.fadeT; // the screen's fade (stepEvents' while a talk runs)
    switch (b.phase) {
    case App::Battle::Intro: // the fade and the opening camera (btl_main 200 / 300)
        if (b.phaseFrames >= 30) {
            b.phase = App::Battle::Command;
            b.phaseFrames = 0;
            battleMenu(app, App::Battle::Top);
            if (b.start.strike == 2) battleStartRound(app); // ambushed: the enemies' round first
            else if (b.start.strike == 1) setBanner(app, "First strike!");
            battletalk::first(app); // btl_main 300 -> 400
        }
        break;
    case App::Battle::WinTalk: // btl_main 8000: the last word said, the victory
        battleWon(app);
        break;
    case App::Battle::Acting:
        if (b.breakFx.on) { // the battle waits for it; then the Break's tutorial (0xff2)
            if (stepBreakFx(app)) b.breakFx.on = false, battletalk::tutorial(app, 0xff2);
            break;
        }
        if (!b.turnOn) {
            // btl_main: no next action while a message is up or waiting
            if (!b.notices.empty() || (b.bannerFrames > 0 && b.bannerFrames <= btlui::kMessageFrames)) break;
            if (b.rules.nextTurn(app.battleActions, b.turn)) {
                b.turnOn = true;
                startTurn(app);
            } else if (b.popups.empty())
                endRound(app);
            break;
        }
        ++b.turnFrame;
        if (b.stageAction) {
            stepStagedCameras(app);
            if (!b.hitsShown) stepStaged(app);
        }
        if (!b.hitsShown && b.turnFrame >= b.hitFrame) {
            showHits(app);
            if (b.stageAction) stagedHit(app);
        }
        if (b.hitsShown) {
            bool busy = false;
            for (int k = 0; k < 2; ++k)
                if (const App::Battle::Actor* u = b.turn.actors[k] >= 0 ? battleActor(app, b.turn.actors[k]) : nullptr)
                    busy |= u->then >= 0 && u->slot != btlui::kSlotHit;
            const bool over = b.turn.kind == cr::kCmdEscape ? b.turnFrame >= (b.turn.escaped ? 30 + 40 : 40)
                              : b.stageAction ? b.turnFrame >= b.hitFrame + b.stageWait // the staged ones' wait
                                              : (!busy && b.turnFrame >= b.hitFrame + 24) || b.turnFrame > 400;
            if (over) {
                if (b.stageAction == 433) // 9999: mdl_set_act_motion(0), it stands
                    if (App::Battle::Actor* u = battleActor(app, b.turn.actors[0])) playSlot(app, *u, btlui::kSlotIdle);
                b.stageAction = 0;
                b.turnOn = false;
                // btlord_chk: a side lost the Break - its effect first (btl_main 0x5dc)
                if (b.rules.breakChanging()) startBreakFx(app);
                // btl_main 700: the action's tutorials - an element taken, a status, a gene learned
                if (b.rules.elementTaken) battletalk::tutorial(app, 0xff3);
                else if (b.rules.statusTaken) battletalk::tutorial(app, 0xff4);
                else if (b.rules.learnedNew) battletalk::tutorial(app, 0xff5);
                for (int k = 0; k < 2; ++k)
                    if (App::Battle::Actor* u = b.turn.actors[k] >= 0 ? battleActor(app, b.turn.actors[k]) : nullptr) {
                        u->pos = u->home;
                        u->moveFrom = u->moveTo = 0;
                        if (u->slot != btlui::kSlotGuard) u->yaw = u->homeYaw;
                    }
            }
        }
        break;
    case App::Battle::Won:
        if (b.phaseFrames >= 50) {
            b.phase = App::Battle::Results;
            b.phaseFrames = 0;
        }
        break;
    case App::Battle::Lost: // 0x513, then (9000) btl_lose: a story battle starts over from the title,
        // any other goes back to the pair's Ark (drawFrame does either, between frames)
        if (b.phaseFrames >= 120) b.exit = b.eventBattle ? 2 : 3;
        break;
    case App::Battle::Fled:
        if (b.phaseFrames >= 30) b.exit = 1;
        break;
    default: break;
    }
    stepQuake(app); // gameapp_prc: UpdateQuake and PutMask after the battle's frame
    stepFlash(app);
}

// btl_prc: the whole screen through the formation's camera (btl_main: camera formation - 2;
// perspective 0.0792 turns = 28.5 degrees vertical, 4:3, near 9, far 2000)
void drawBattle(App& app, float dt) {
    App::Battle& b = app.battle;
    auto& r = app.renderer;
    const float W = r.screenW(), H = cr::Renderer::kLogicalH; // 3D across the whole screen
    for (b.clock += dt; b.clock >= 1.0f / kFramesPerSecond && b.active; b.clock -= 1.0f / kFramesPerSecond) stepBattle(app);
    if (!b.active) return;
    r.fillRect(0, 0, W, H, 0, 0, 0, 1);
    drawSpaceBackdrop(app, true); // ScnEffectProc runs in battles too: the Space backdrop at the back
    r.setOrigin(0);
    cr::Vec3 eye{488, 12, 555}, target{500, 4, 498};
    battleCameraView(app, eye, target);
    cr::Mat4 proj = cr::Mat4::perspective(28.5f * 3.14159265f / 180.0f, W / H, 9.0f, 2000.0f);
    // btl_prc: the quake shifts the frustum by its offset in the 1024 x 768 screen's pixels
    proj.m[8] -= 2.0f * b.quake.off[0] / W;
    proj.m[9] -= 2.0f * b.quake.off[1] / H;
    const cr::Mat4 vp = proj * cr::Mat4::lookAt(eye, target, {0, 1, 0});
    b.vp = vp;
    r.setCamera(9.0f, 2000.0f, 28.5f * 3.14159265f / 180.0f);
    r.begin3DRect(0, 0, W, H);
    drawLoaded(app, b.stage, vp);
    for (const App::Battle::Actor& a : b.actors) {
        if (a.alpha <= 0.001f) continue;
        LoadedModel& lm = b.models[a.model];
        const cr::Model& m = lm.cpu;
        if (!m.skeleton.nodes.empty() && lm.meshes.size() == m.meshes.size()) { // skin this instance
            const cr::Action* act = a.action >= 0 && a.action < int(lm.actions.size()) ? &lm.actions[size_t(a.action)] : nullptr;
            cr::computeWorld(m.skeleton, act, a.time, lm.world);
            for (size_t i = 0; i < m.meshes.size(); ++i) {
                cr::skinMesh(m, m.meshes[i], lm.world, lm.skinned);
                r.updatePositions(lm.meshes[i], lm.skinned, m.meshes[i].uvs, m.meshes[i].colors);
            }
        }
        cr::Mat4 world;
        const float c = std::cos(a.yaw), sn = std::sin(a.yaw);
        world.m[0] = c * a.scale; world.m[2] = -sn * a.scale; world.m[5] = a.scale;
        world.m[8] = sn * a.scale; world.m[10] = c * a.scale;
        world.m[12] = a.pos.x; world.m[13] = a.pos.y; world.m[14] = a.pos.z;
        drawLoaded(app, lm, vp * world, nullptr, a.alpha);
    }
    drawEffects(app, vp);
    r.end3D();
    drawSceneEffects(app, true); // the field's weather goes on over the battle
    r.setOrigin(0);
    if (b.flash.on && b.flash.alpha > 0) r.fillRect(0, 0, W, H, 1, 1, 1, b.flash.alpha / 255.0f); // PutMask
}

// ---- The HUD (menub_mainPut, __dmg_prc, MbResultPut) ----

// cmd_number: `digits` places 10 apart, right-aligned; the HP's red under a quarter, the
// level's orange style (width 3)
void drawBattleNumber(cr::MenuPainter& m, int value, float x, float y, int digits, bool low, float a, bool level = false) {
    const std::string t = std::to_string(std::max(0, value));
    const int n = int(t.size());
    for (int k = 0; k < n; ++k) {
        const int d = t[size_t(k)] - '0';
        const float u = level ? 588 + 14.0f * d : 14.0f * d + (low ? 140 : 0);
        m.piece(x + 10.0f * float(digits - n + k), y, u, 0, 14, 14, a);
    }
}

// cmd_bottom: a battle button (`x` and `y` its corner): base, face (lit when selected), icon, word
void drawBattleButton(cr::MenuPainter& m, float x, float y, int icon, int word, bool selected, bool dimmed, float a) {
    const float c = dimmed ? 0.25f : 1.0f;
    m.piece(x, y, 0, 184, 24, 40, a, c, c, c);
    m.pieceTo(x + 24, y, 84, 40, 28, 184, 4, 40, a, c, c, c);
    m.piece(x + 108, y, 28, 184, 24, 40, a, c, c, c);
    const float face = selected && !dimmed ? 96 : 52;
    m.piece(x + 28, y, face, 184, 24, 40, a, c, c, c);
    m.pieceTo(x + 52, y, 68, 40, face + 24, 184, 4, 40, a, c, c, c);
    m.piece(x + 120, y, face + 28, 184, 16, 40, a, c, c, c);
    m.piece(x + 8, y + 4, float(icon % 16 * 32), float(icon / 16 * 32 + 376), 32, 32, a, c, c, c);
    m.piece(x + 40, y + 8, float(word % 5 * 88), float(word / 5 * 24 + 512), 88, 24, a, c, c, c);
}

// The red -> orange HP bar of the plates and rows (tex2d_add4 on slot 45), `w` long when full
void drawBattleHpBar(cr::MenuPainter& m, float x, float y, float w, int value, int most, bool mp, float a) {
    const float black[4] = {0, 0, 0, a};
    m.quad(x + 1, y + 1, w, 3, black, black);
    if (value <= 0 || most <= 0) return;
    const float f = std::clamp(float(value) / float(most), 0.0f, 1.0f);
    const float fill = std::max(1.0f, w * f);
    const float left[4] = {mp ? 0x07 / 255.f : 0xdd / 255.f, mp ? 0xaf / 255.f : 0x02 / 255.f, mp ? 0x73 / 255.f : 0, a};
    const float right[4] = {left[0] + (1 - left[0]) * f, left[1] + (0xc3 / 255.f - left[1]) * f,
                            left[2] + (0x73 / 255.f - left[2]) * f, a};
    m.quad(x, y, fill, 3, left, right);
}

// menub_param_put: the element, then each status on, 20 apart
void drawBattleStatus(cr::MenuPainter& m, const cr::Fighter& f, float x, float y, float a) {
    m.piece(x, y, float(f.element * 20), 924, 20, 20, a);
    float at = x + 20;
    for (int k = 0; k < cr::kStatusCount; ++k)
        if (f.status[k] > 0 && k != cr::kStatusUnused92) {
            m.piece(at, y, float(140 + 20 * k), 924, 20, 20, a);
            at += 20;
        }
}

// cmd_bottom_teki: a target's plate at (x, y): its level, name and HP bar
void drawBattlePlate(App& app, cr::MenuPainter& m, float x, float y, int fighter, bool selected, float a) {
    const App::Battle& b = app.battle;
    const cr::Fighter& f = b.rules.f[size_t(fighter)];
    m.piece(x, y, 0, 184, 24, 40, a);
    m.pieceTo(x + 24, y, 200, 40, 28, 184, 4, 40, a);
    m.piece(x + 224, y, 28, 184, 24, 40, a);
    const float face = selected ? 96 : 52;
    m.piece(x + 36, y, face, 184, 24, 40, a);
    m.pieceTo(x + 60, y, 180, 40, face + 24, 184, 4, 40, a);
    m.piece(x + 240, y, face + 28, 184, 16, 40, a);
    m.piece(x + 6, y + 25, 116, 14, 38, 10, a); // "Lv"
    drawBattleNumber(m, f.level, x + 6, y + 12, 3, false, a, true);
    drawBattleHpBar(m, x + 96, y + 25, 100, b.shownHp[fighter], f.hpMax, false, a);
    std::string name;
    for (const App::Battle::Actor& ac : b.actors)
        if (ac.fighter == fighter) name = ac.name;
    m.textCentred(name, x + 146 - m.textWidth(name, 13) / 2, y + 15, selected, a, 13);
    drawBattleStatus(m, f, x + 56, y - 8, a);
}

// A member's row: portrait, HP and MP numbers and bars, statuses (actor 1 at the bottom, actor
// 0 40 to the right and 24 up)
void drawBattleRow(App& app, cr::MenuPainter& m, int k, bool dimmed, float a) {
    const App::Battle& b = app.battle;
    const cr::Fighter& f = b.rules.f[size_t(k)];
    if (!f.present) return;
    const bool phone = app.renderer.phoneLayout(); // 2.2.0: (+16, -32), into the iPhone's corner
    const float dx = (k == 0 ? 40 : 0) + (phone ? 16 : 0), dy = (k == 0 ? -24 : 0) + (phone ? -32 : 0);
    const float c = dimmed ? 0.25f : 1.0f;
    m.piece((k == 0 ? 52 : 0) + (phone ? 16 : 0), 328 + dy, float(f.character * 48), 440, 48, 48, a, c, c, c);
    m.piece(2 + dx, 370 + dy, 0, 14, 22, 10, a); // "HP"
    m.piece(86 + dx, 370 + dy, 22, 14, 22, 10, a); // "MP"
    drawBattleNumber(m, b.shownHp[k], 24 + dx, 364 + dy, 5, b.shownHp[k] <= f.hpMax / 4, a);
    drawBattleNumber(m, b.shownMp[k], 108 + dx, 364 + dy, 4, false, a);
    drawBattleHpBar(m, 20 + dx, 376 + dy, 59, b.shownHp[k], f.hpMax, false, a);
    drawBattleHpBar(m, 100 + dx, 376 + dy, 49, b.shownMp[k], f.mpMax, true, a);
    drawBattleStatus(m, f, 152 + dx, 364 + dy, a);
}

// The results (MbResultPut): "Results", the pair's boards (ComUty_FaceBoardDraw, 88 apart),
// EXP Gained and OZ Earned (ComUty_ExpAndOzDraw at (288, 76)), the play time and the purse
void drawBattleResults(App& app, cr::MenuPainter& m, float a) {
    const App::Battle& b = app.battle;
    const PhoneMenu phoneMenu(app, m); // MbResultPut: the iPhone screen's menu coordinates
    cr::Renderer& r = app.renderer;
    r.setOrigin(0);
    r.fillRect(0, 0, r.screenW(), cr::Renderer::kLogicalH, 0, 0, 0, 0.45f * a);
    r.setOrigin(wideLeft(app)); // (widescreen: the heading and boards left, the takings right)
    m.picture(19, 8, 4, a); // "Results"
    for (int k = 0; k < 2; ++k) {
        const cr::Fighter& f = b.rules.f[size_t(k)];
        if (!f.present) continue;
        const float y = 24 + 88.0f * float(k);
        drawFaceBoard(app, m, 0, y, f.character, f.hp, f.hpMax, f.mp, f.mpMax, a);
        if (std::find(b.levelled.begin(), b.levelled.end(), f.character) != b.levelled.end())
            m.text("Level up!", 180, y + 86, false, a, 13);
    }
    r.setOrigin(wideRight(app));
    m.picture(67, 304, 48, a);
    m.picture(68, 288, 76, a); // EXP Gained
    m.picture(70, 340, 96, a);
    drawShopNumber(m, b.rules.expGained * (b.rules.doubleExp ? 2 : 1), 358, 98, 6, 12, a);
    m.picture(69, 288, 132, a); // OZ Earned
    m.picture(70, 340, 152, a);
    drawShopNumber(m, b.rules.ozGained * (b.rules.doubleOz ? 2 : 1), 358, 154, 6, 12, a);
    drawClockAndOz(app, m, a);
    // The message board (MbResultPut's, at (0, 220)): what was won and learned
    std::vector<std::string> lines;
    for (int item : b.rules.drops) lines.push_back(sysText(app, 57010, itemName(app, item)));
    for (int g : b.rules.learned) lines.push_back(sysText(app, cr::isSkill(g) ? 57050 : 57049, itemName(app, g)));
    for (int item : b.rules.stolen) lines.push_back(sysText(app, 57010, itemName(app, item)));
    r.setOrigin(r.uiX());
    if (!lines.empty()) {
        m.picture(12, 0, 220, a);
        const size_t first = lines.size() > 4 ? lines.size() - 4 : 0;
        for (size_t i = first; i < lines.size(); ++i) m.text(lines[i], 24, 234 + 16.0f * float(i - first), false, a, 14);
    }
    r.setOrigin(wideLeft(app));
    if (keyHints(app)) // (our key hints)
        m.text("Enter: continue", 20, 372, false, a * 0.8f, 9);
    r.setOrigin(r.uiX());
}

void drawBattleHud(App& app, float W, float H) {
    (void)W;
    (void)H;
    using namespace btlui;
    App::Battle& b = app.battle;
    cr::Renderer& r = app.renderer;
    cr::MenuPainter& m = app.menu;
    if (battletalk::on(app)) return; // a talk: the portraits and the message box instead
    m.begin(r, app.windowTex, &app.font);
    if (b.phase == App::Battle::Results) {
        drawBattleResults(app, m, std::min(1.0f, float(b.phaseFrames + 1) / 8.0f));
        r.setOrigin(r.uiX());
        return;
    }
    const float a = std::min(1.0f, float(b.frames) / 10.0f);
    // The iPhone layout (2.2.0's __menub_mainPut): the badge and the rows into its screen's
    // corners - (+16, +32) / (+16, -32) - the banner 32 lower, the menus where they are
    const bool phone = r.phoneLayout();
    const float edge = phone ? r.uiX() : 0.0f; // the left-hand pieces' origin

    // Damage numbers (__dmg_prc): over their targets, projected from the 3D across the screen
    r.setOrigin(0);
    for (const App::Battle::Popup& p : b.popups) {
        const float* v = b.vp.m;
        const float cx = v[0] * p.at.x + v[4] * p.at.y + v[8] * p.at.z + v[12];
        const float cy = v[1] * p.at.x + v[5] * p.at.y + v[9] * p.at.z + v[13];
        const float cw = v[3] * p.at.x + v[7] * p.at.y + v[11] * p.at.z + v[15];
        if (cw <= 0.01f) continue;
        const std::string t = std::to_string(std::abs(p.amount));
        const int width = int(t.size()) < 2 ? 20 : 20 * int(t.size()); // kept on the screen
        float px = (cx / cw * 0.5f + 0.5f) * r.screenW(), py = (1 - (cy / cw * 0.5f + 0.5f)) * cr::Renderer::kLogicalH;
        px = std::clamp(px, 64.0f, r.screenW() - float(width));
        py = std::clamp(py, 72.0f, 672.0f);
        r.screenToLayout(px, py); // (the iPhone layout draws 2D scaled; the projection is the screen's)
        const float x = px / 2, y = py / 2;
        const int step = (kPopupTime - p.timer) >> 4;
        if (p.type >= 1 && p.type <= 7) { // the label: Blowout! Critical! Weak! / No Effect! Guard! Miss!
            float ly = y - 24, la = 1;
            const int idx = p.type - 1;
            if (p.type < 4) {
                if (p.timer >= 320) { const int k = 4 - (step); ly += -18.0f * float(std::max(0, k)); la = 1 - 0.25f * float(std::max(0, k)); }
                else if (p.timer < 64) { const int k = 4 - (p.timer >> 4); ly += -18.0f * float(k); la = 1 - 0.25f * float(k); }
            } else if (step < 8)
                ly -= float(kLabelHop[step]);
            m.piece(x - 72, ly, float(idx % 4 * 144), float(idx / 4 * 24 + 72), 144, 24, a * la);
        }
        const bool number = p.type == cr::kHitNormal || p.type == cr::kHitCritical || p.type == cr::kHitWeak ||
                            p.type == cr::kHitResist;
        if (!number) continue;
        const int style = p.amount < 0 ? 3 : p.type == cr::kHitResist ? 2 : p.type == cr::kHitWeak ? 1 : 0;
        float dx = x - 10.0f * float(t.size());
        const float dy = y - (p.amount < 0 ? 24 : 40);
        for (size_t i = 0; i < t.size(); ++i, dx += 19) {
            const int s = step - int(i);
            const float hop = s >= 0 && s < 8 ? float(kDigitHop[s]) : 0;
            m.piece(dx, dy - hop, float((t[i] - '0') * 24 + style % 2 * 240), float(style / 2 * 24 + 24), 24, 24, a);
        }
    }

    // Top left: the Break badge - the ring, a piece per point, the holder's name
    const int g = b.gaugeShown;
    r.setOrigin(edge);
    m.setOffset(phone ? 16.0f : 0.0f, phone ? 32.0f : 0.0f);
    m.piece(0, 0, 0, 120, 64, 64, a);
    for (int i = 0; i < std::min(8, std::abs(g)); ++i) {
        const short* t = kBreakTick[i];
        if (g > 0) m.piece(t[0], t[1], t[2], t[3], 32, 32, a, 0x38 / 255.f, 1, 0xe3 / 255.f);
        else m.piece(t[0], t[1], t[2], t[3], 32, 32, a, 1, 0x14 / 255.f, 0x23 / 255.f);
    }
    if (g != 0 && b.breakPop > 1.0f) { // the label pops in large
        const float s = b.breakPop, sz = 64 * s;
        m.pieceTo(32 - sz / 2, 32 - sz / 2, sz, sz, g < 0 ? 512 : 384, 240, 128, 128, a * std::min(1.0f, 2.0f / s));
    } else
        m.piece(0, 0, g == 0 ? 320 : g < 0 ? 256 : 192, 120, 64, 64, a);
    m.setOffset(0, 0);

    r.setOrigin(r.uiX());
    drawBreakFx(app, m); // btl_main 0x5dc
    r.setOrigin(edge);
    // Bottom left: the pair's rows (the one not choosing dimmed)
    const bool choosing = b.phase == App::Battle::Command && b.menu != App::Battle::Top && b.menu != App::Battle::EscapeAsk;
    for (int k = 1; k >= 0; --k)
        drawBattleRow(app, m, k, choosing && !b.rules.pair && b.member != k, a);

    // The banner: the action's name, the target prompt, the messages
    r.setOrigin(r.uiX());
    if (b.bannerFrames > 0 && !b.banner.empty()) {
        const float clear[4] = {0, 0, 0, 0}, dark[4] = {0, 0, 0, 0x98 / 255.f};
        const float by = phone ? 32.0f : 0.0f;
        m.quad(88, 4 + by, 24, 24, clear, dark);
        m.quad(112, 4 + by, 351, 24, dark, dark);
        m.quad(463, 4 + by, 24, 24, dark, clear);
        m.textCentred(b.banner, 287 - m.textWidth(b.banner, 14) / 2, 16 + by, false, 1, 14);
    }

    // Right: the menus
    r.setOrigin(phone ? r.uiX() : 2 * r.uiX());
    if (b.phase == App::Battle::Command && !app.optionsOpen) { // (Options covers them while it's up)
        switch (b.menu) {
        case App::Battle::Top:
            for (int i = 0; i < 4; ++i) {
                const bool dim = (i == 1 && (!(b.rules.f[0].alive() && b.rules.f[1].alive()) || b.rules.allyAuto)) ||
                                 (i == 2 && b.rules.escapeBlocked);
                drawBattleButton(m, kButtonX, kButtonY + kButtonStep * float(i), kTopIcon[i], kTopWord[i], b.cursor == i, dim, a);
            }
            break;
        case App::Battle::Commands:
            for (int i = 0; i < 4; ++i) {
                const bool dim = i == 1 && (battleGeneList(app).empty() || battleGenesSilenced(app));
                drawBattleButton(m, kButtonX, kButtonY + kButtonStep * float(i), kCmdIcon[i], kCmdWord[i], b.cursor == i, dim, a);
            }
            m.backBar(kBackX, kBackY, false, a);
            break;
        case App::Battle::Targets: {
            const int n = int(b.picks.size());
            const cr::BattleActionDef* act = app.battleActions.get(b.targetAction);
            const bool all = act && b.rules.isGroupScope(*act);
            for (int i = 0; i < n; ++i)
                drawBattlePlate(app, m, kPlateX, kPlateBottom - 48.0f * float(n - i), b.picks[size_t(i)], all || b.pick == i, a);
            m.backBar(kBackX, kBackY, false, a);
            break;
        }
        case App::Battle::Items: { // MuScrList: the battle items in the bag, how many of each
            const int n = int(b.picks.size());
            for (int k = 0; k < kItemRows && b.pickTop + k < n; ++k) {
                const int index = b.pickTop + k, item = b.picks[size_t(index)];
                const float y = 112 + 40.0f * float(k);
                const bool sel = index == b.pick;
                const float white[4] = {1, 1, 1, (sel ? 0.95f : 0.7f) * a}, clear[4] = {1, 1, 1, 0};
                const float cyan[4] = {0x4b / 255.f, 1, 0xeb / 255.f, 0.95f * a};
                m.quad(232, y, 220, 32, sel ? cyan : white, white);
                m.quad(452, y, 40, 32, white, clear);
                const cr::ItemParam* ip = app.itemTable.item(item);
                if (ip && ip->icon >= 0) m.piece(236, y, float(ip->icon % 16 * 32), float(248 + ip->icon / 16 * 32), 32, 32, a);
                m.textCentred(itemName(app, item), 272, y + 16, true, a, 14);
                const auto it = app.game.items.find(item);
                drawShopNumber(m, it == app.game.items.end() ? 0 : it->second, 448, y + 8, 2, 12, a, 2);
            }
            m.backBar(kBackX, kBackY, false, a);
            break;
        }
        case App::Battle::Genes: { // the gene list: name, MP / cost (dim when the MP won't pay)
            const int n = int(b.picks.size()), have = battleGeneMp(app);
            for (int k = 0; k < kItemRows && b.pickTop + k < n; ++k) {
                const int index = b.pickTop + k, g = b.picks[size_t(index)], cost = geneMpCost(app, g);
                const float y = 112 + 40.0f * float(k), ra = cost <= have ? a : a * 0.5f;
                const bool sel = index == b.pick;
                const float white[4] = {1, 1, 1, (sel ? 0.95f : 0.7f) * ra}, clear[4] = {1, 1, 1, 0};
                const float cyan[4] = {0x4b / 255.f, 1, 0xeb / 255.f, 0.95f * ra};
                m.quad(232, y, 220, 32, sel ? cyan : white, white);
                m.quad(452, y, 40, 32, white, clear);
                if (const cr::ItemParam* ip = app.itemTable.item(g); ip && ip->icon >= 0)
                    m.piece(236, y, float(ip->icon % 16 * 32), float(248 + ip->icon / 16 * 32), 32, 32, ra);
                m.textCentred(itemName(app, g), 272, y + 16, true, ra, 14);
                m.piece(392, y + 18, 22, 14, 22, 10, ra); // MP
                drawShopNumber(m, cost, 418, y + 12, 3, 12, ra, 2);
            }
            m.backBar(kBackX, kBackY, false, a);
            if (b.pick >= 0 && b.pick < n) { // the HELP board across the top: the gene's target and effect
                const float hx = 152;
                m.stretch(12, hx, 0, 512 - hx, 66, a);
                m.picture(18, hx + 4, 2, a);
                std::vector<std::vector<std::string>> help;
                if (app.systemText.message(cr::itemHelpId(b.picks[size_t(b.pick)]), app.language, nullptr, help) && !help.empty()) {
                    float hy = 20;
                    for (const std::string& line : help[0]) {
                        m.text(line, hx + 14, hy, false, a, 13);
                        if ((hy += 16) > 52) break;
                    }
                }
            }
            break;
        }
        case App::Battle::EscapeAsk: { // 57015 on the board, Yes / No beneath
            const float mx = 96, my = 200;
            r.setOrigin(r.uiX());
            drawWinPiece(app, kWinSmallPanel, mx * 2, my * 2, 320 * 2, 48 * 2);
            m.textCentred(sysText(app, 57015), mx + 20, my + 24, false, a, 15);
            m.button(128, 264, 0, "", b.confirm == 0, a, b.confirm == 0); // Yes
            m.button(272, 264, 1, "", b.confirm == 1, a, b.confirm == 1); // No
            break;
        }
        }
    }
    r.setOrigin(r.uiX());
    const char* help = b.phase != App::Battle::Command ? ""
                       : b.menu == App::Battle::Top ? "Up/Down  Enter"
                       : b.menu == App::Battle::EscapeAsk ? "Left/Right  Enter  Esc: no"
                                                          : "Up/Down  Enter  Esc: back";
    if (app.showHud) m.text(help, 300, 368, false, a * 0.6f, 9); // (our key hints)
}

// ---- Battle input ----

void battleKey(App& app, SDL_Keycode key) {
    App::Battle& b = app.battle;
    const bool back = key == SDLK_ESCAPE || key == SDLK_BACKSPACE || key == SDLK_AC_BACK;
    const bool enter = key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE || key == SDLK_E;
    if (battletalk::on(app)) { // a talk: the pages turn as on the field
        if (enter) {
            if (!app.dialogue.empty()) tapDialogue(app);
            else app.tapPending = true;
        }
        return;
    }
    const bool up = key == SDLK_UP || key == SDLK_W, down = key == SDLK_DOWN || key == SDLK_S;
    const bool left = key == SDLK_LEFT || key == SDLK_A, right = key == SDLK_RIGHT || key == SDLK_D;
    if (b.phase == App::Battle::Results) {
        if (enter || back) endBattle(app, true);
        return;
    }
    if (b.phase != App::Battle::Command) return;
    switch (b.menu) {
    case App::Battle::Top:
    case App::Battle::Commands:
        if (up || down) { b.cursor = (b.cursor + (up ? 3 : 1)) % 4; menuSound(app, se::kCursor); }
        else if (enter) { if (b.menu == App::Battle::Top) battleChooseTop(app, b.cursor); else battleChooseCommand(app, b.cursor); }
        else if (back && b.menu == App::Battle::Commands) battleBack(app);
        break;
    case App::Battle::Targets:
    case App::Battle::Items:
    case App::Battle::Genes: {
        const int n = int(b.picks.size());
        const bool targets = b.menu == App::Battle::Targets;
        if ((up || down || (targets && (left || right))) && n > 0) {
            b.pick = (b.pick + ((up || left) ? n - 1 : 1)) % n;
            b.pickTop = std::clamp(b.pickTop, std::max(0, b.pick - btlui::kItemRows + 1), b.pick);
            if (targets) {
                const cr::BattleActionDef* act = app.battleActions.get(b.targetAction);
                cameraForTarget(app, b.picks[size_t(b.pick)], act && b.rules.isGroupScope(*act));
            }
            menuSound(app, se::kCursor);
        } else if (enter && n > 0) {
            if (targets) battleChooseTarget(app, b.picks[size_t(b.pick)]);
            else if (b.menu == App::Battle::Genes) battleChooseGene(app, b.picks[size_t(b.pick)]);
            else battleChooseItem(app, b.picks[size_t(b.pick)]);
        } else if (back)
            battleBack(app);
        break;
    }
    case App::Battle::EscapeAsk:
        if (up || down || left || right) { b.confirm ^= 1; menuSound(app, se::kCursor); }
        else if (enter) battleEscapeAnswer(app, b.confirm == 0);
        else if (back) battleEscapeAnswer(app, false);
        break;
    }
}

void battleClick(App& app, float px, float py) {
    using namespace btlui;
    App::Battle& b = app.battle;
    float lx, ly;
    app.renderer.toLogical(px, py, lx, ly);
    const float uiX = app.renderer.uiX(), rightX = app.renderer.phoneLayout() ? uiX : 2 * uiX;
    const float cx = (lx - uiX) / 2, rx = (lx - rightX) / 2, my = ly / 2;
    if (battletalk::on(app)) { // a talk: the skip corner, else the next page
        if (lx >= app.renderer.layoutRight() - 76 && ly < app.renderer.layoutTop() + 72) app.skipScene = true;
        else if (!app.dialogue.empty()) tapDialogue(app);
        else app.tapPending = true;
        return;
    }
    if (b.phase == App::Battle::Results) {
        endBattle(app, true);
        return;
    }
    if (b.phase != App::Battle::Command) return;
    const bool onBack = my >= kBackY && my < kBackY + 24 && rx >= kBackX && rx < kBackX + 232;
    auto buttonAt = [&]() {
        for (int i = 0; i < 4; ++i)
            if (rx >= kButtonX && rx < kButtonX + 136 && my >= kButtonY + kButtonStep * i && my < kButtonY + kButtonStep * i + 40) return i;
        return -1;
    };
    switch (b.menu) {
    case App::Battle::Top:
        if (const int i = buttonAt(); i >= 0) { b.cursor = i; battleChooseTop(app, i); }
        break;
    case App::Battle::Commands:
        if (const int i = buttonAt(); i >= 0) { b.cursor = i; battleChooseCommand(app, i); }
        else if (onBack) battleBack(app);
        break;
    case App::Battle::Targets: {
        const int n = int(b.picks.size());
        for (int i = 0; i < n; ++i) {
            const float y = kPlateBottom - 48.0f * float(n - i);
            if (rx >= kPlateX && rx < kPlateX + 256 && my >= y && my < y + 40) {
                b.pick = i;
                battleChooseTarget(app, b.picks[size_t(i)]);
                return;
            }
        }
        if (onBack) battleBack(app);
        break;
    }
    case App::Battle::Items:
    case App::Battle::Genes:
        if (rx >= 232 && rx < 492 && my >= 112 && my < 112 + 40 * kItemRows) {
            const int index = b.pickTop + int((my - 112) / 40);
            if (index < int(b.picks.size())) {
                b.pick = index;
                if (b.menu == App::Battle::Genes) battleChooseGene(app, b.picks[size_t(index)]);
                else battleChooseItem(app, b.picks[size_t(index)]);
            }
        } else if (onBack)
            battleBack(app);
        break;
    case App::Battle::EscapeAsk:
        if (my >= 264 && my < 304 && cx >= 128 && cx < 240) battleEscapeAnswer(app, true);
        else if (my >= 264 && my < 304 && cx >= 272 && cx < 384) battleEscapeAnswer(app, false);
        break;
    }
}


// "[E]" over the examinable NPC in reach (the original's FollowWindow bubble). `vp` maps the
// world to the 3D rectangle (rx, ry, rw, rh) of the logical screen.
void drawPrompt(App& app, const cr::Mat4& vp, float rx, float ry, float rw, float rh) {
    if (app.runner.running() || !app.dialogue.empty() || app.itemWindowOpen) return;
    const cr::FieldNpc* n = interactTarget(app);
    if (!n) return;
    const float k = n->model <= 1 ? 1.0f : 0.0f;
    const float p[3] = {n->pos[0] + k * n->offset[0] + n->offset[0], n->pos[1] + std::max(n->box[1], 6.0f) + 4,
                        n->pos[2] + k * n->offset[2] + n->offset[2]};
    const float* m = vp.m;
    const float cw = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
    if (cw <= 0) return;
    const float sx = rx + ((m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12]) / cw * 0.5f + 0.5f) * rw;
    const float sy = ry + (0.5f - (m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13]) / cw * 0.5f) * rh;
    if (app.windowTex.id) drawWinPiece(app, kWinBadge, sx - 20, sy - 41, 40, 41); // the "!" badge
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
cr::Mat4 scriptCameraView(const App& app, const cr::Vec3& eye, const cr::Vec3& target) {
    const cr::StageCamera& sc = app.stageCam;
    const float SW = app.renderer.screenW(), H = cr::Renderer::kLogicalH;
    const float w = fieldViewW(app), h = fieldViewH(app);
    const float qx = std::trunc(app.fieldQuake.off[0]), qy = std::trunc(app.fieldQuake.off[1]);
    const float rx = app.renderer.uiX() + (cr::Renderer::kLogicalW - w) / 2 + qx, ry = (H - h) / 2 + qy;
    cr::Mat4 toScreen;
    toScreen.m[0] = w / SW, toScreen.m[5] = h / H;
    toScreen.m[12] = (2 * rx + w) / SW - 1, toScreen.m[13] = 1 - (2 * ry + h) / H;
    const float whole = float(std::max(1, sc.viewWidth / std::max(1, sc.viewHeight)));
    return toScreen * cr::Mat4::perspective(sc.fovDegrees * 3.14159265f / 180.0f, whole, 1.0f, 4000.0f) *
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
    app.pendingEntry = App::Entry::Room;
    app.lastEvent = "lost: back to the Ark";
}

void drawFrame(App& app, bool present = true) {
    if (app.battle.active && app.battle.exit) { // the battle's way out, between frames
        const int e = app.battle.exit;
        app.battle.exit = 0;
        if (e == 1) endBattle(app, false);
        else if (e == 3) loseToArk(app);
        else bootTitle(app);
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(app.window, &w, &h);
    auto& r = app.renderer;
    r.beginFrame(w, h);
    // W: the game's 2D layout (1024); SW: the whole logical screen (wider in widescreen)
    const float W = cr::Renderer::kLogicalW, H = cr::Renderer::kLogicalH, SW = r.screenW();
    const Uint64 now = SDL_GetTicksNS();
    const float dt = std::min(app.lastTicks ? float(double(now - app.lastTicks) * 1e-9) : 0.0f, 0.1f);
    app.lastTicks = now;
    if (app.mode == Mode::Maps && !app.booting && !app.titleOpen && (app.havePlayer || app.battle.active))
        app.playSeconds += dt;

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
        // The title: no field; the boot script runs at 30 Hz by itself (it shows its pictures
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
        const bool scriptCam = app.cam.free && app.cam.set && !app.background.id;
        cr::Vec3 eye, tgt;
        cr::Mat4 mvp;
        const bool have3D = app.haveStageCam || scriptCam;
        if (have3D) {
            const cr::StageCamera& sc = app.stageCam;
            eye = scriptCam ? app.cam.eye : cr::Vec3{sc.eye[0], sc.eye[1], sc.eye[2]};
            tgt = scriptCam ? app.cam.target : cr::Vec3{sc.target[0], sc.target[1], sc.target[2]};
            const float aspect = scriptCam ? SW / H : sc.viewHeight > 0 ? float(sc.viewWidth) / float(sc.viewHeight) : 1.0f;
            const float fov = app.haveStageCam ? sc.fovDegrees : 60.0f;
            mvp = cr::Mat4::perspective(fov * 3.14159265f / 180.0f, aspect, scriptCam ? 1.0f : 10.0f, 4000.0f) *
                  cr::Mat4::lookAt(eye, tgt, {0, 1, 0});
            if (scriptCam && app.haveStageCam && sc.viewWidth > 0 && sc.viewHeight > 0) mvp = scriptCameraView(app, eye, tgt);
            r.setCamera(scriptCam ? 1.0f : 10.0f, 4000.0f, fov * 3.14159265f / 180.0f);
            animateModel(app, dt);
            animateNpcs(app, dt);
            updatePlayer(app, dt, std::atan2(eye.x - tgt.x, eye.z - tgt.z));
        }
        // The stage view on screen (vx, vy, vw, vh)
        float vx, vy, vw, vh;
        if (scrollingField(app)) {
            followPlayerScroll(app);
            vw = fieldViewW(app);
            vh = fieldViewH(app);
            vx = (SW - vw) / 2 + app.scroll.offX;
            vy = (H - vh) / 2 - app.scroll.offY;
        } else {
            vw = vh = app.video.paintings == 2 ? std::max(SW, H) : std::min(SW, H);
            vx = (SW - vw) / 2;
            vy = (H - vh) / 2;
        }
        // FieldCamera::setCameraBack / setCamera3D: the viewport moves by the quake's offset
        // (whole pixels: it's set in ints)
        const float qx = std::trunc(app.fieldQuake.off[0]), qy = std::trunc(app.fieldQuake.off[1]);
        vx += qx, vy += qy;
        // Layers are drawn at native pixel size, centred in the stage view (viewW x viewH:
        // 1024 for most fields, 512 for d0108/d0117/d0208/d0219). 512x1024 backgrounds such
        // as d0501/d0507/d0511 occupy the middle strip x 256..768
        const float viewSize = app.haveStageCam && app.stageCam.viewWidth > 0 ? float(app.stageCam.viewWidth) : 1024.0f;
        auto drawLayer = [&](const cr::Texture& t) {
            const float k = vw / viewSize, w = t.width * k, h = t.height * k;
            r.drawQuad(t, vx + (vw - w) / 2, vy + (vh - h) / 2, w, h);
        };
        if (app.background.id) drawLayer(app.background);
        drawSpaceBackdrop(app, false); // the tower's scrolling backdrop, behind the 3D
        r.setOrigin(0);
        if (have3D) {
            if (scriptCam) r.begin3DRect(app.haveStageCam && app.stageCam.viewWidth > 0 ? 0 : qx, app.haveStageCam && app.stageCam.viewWidth > 0 ? 0 : qy, SW, H);
            else r.begin3DRect(vx, vy, vw, vh);
            // Field props are occlusion geometry: all their materials are untextured / solid-alpha
            // and black-diffuse, placed over pillars and door frames of the painting. They only
            // write depth, so the player and NPCs disappear behind them
            r.setColorWrite(false);
            if (app.showProps) drawLoaded(app, app.model, mvp);
            r.setColorWrite(true);
            drawNpcs(app, mvp);
            if (app.havePlayer && playerShown(app)) drawLoaded(app, app.player, mvp * playerWorld(app.walker), nullptr, app.playerAlpha);
            if (app.puzzle.stage >= 0) puzzle::drawFx(app, mvp);
            drawEffects(app, app.fieldEffects, mvp);
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
            if (scriptCam) drawPrompt(app, mvp, 0, 0, SW, H);
            else drawPrompt(app, mvp, vx, vy, vw, vh);
        }
        if (app.foreground.id) drawLayer(app.foreground);
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
            drawLoaded(app, app.player, mvp * playerWorld(app.walker), nullptr, app.playerAlpha);
        if (app.mode == Mode::Maps && app.puzzle.stage >= 0) puzzle::drawFx(app, mvp);
        if (app.mode == Mode::Maps) drawEffects(app, app.fieldEffects, mvp);
        if (app.mode == Mode::Maps && app.collision.vao && app.showCollision) {
            cr::Material colMat;          // opaque, untextured, double-sided, depth test + write
            colMat.culling = 0;
            colMat.properties = 8 | 16;
            colMat.ambient[0] = colMat.ambient[1] = colMat.ambient[2] = 0;
            r.drawMesh(app.collision, nullptr, mvp, colMat);
            if (app.triggerMesh.vao) r.drawMesh(app.triggerMesh, nullptr, mvp, colMat);
        }
        r.end3D();
        if (app.mode == Mode::Maps) drawPrompt(app, mvp, 0, 0, SW, H);
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
    if (app.mode == Mode::Maps) {
        const float a = screenFadeAlpha(app); // Screen: fades to / from a grey level
        if (a > 0.001f) r.fillRect(0, 0, SW, H, app.fadeGrey, app.fadeGrey, app.fadeGrey, a);
    }
    r.setOrigin(r.uiX()); // the game's 2D from here on: its 1024 x 768 layout, centred
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
                    continue;
                }
                const float w = float(p.tex.width) * p.scale, h = float(p.tex.height) * p.scale; // zooms about its centre
                // Widescreen: side portraits keep to the screen's edges (as the later widescreen
                // release does), centre images to the middle
                const float edge = p.side < 0 ? -r.uiX() : p.side > 0 ? r.uiX() : 0.0f;
                const float x = edge + p.x - (w - float(p.tex.width)) / 2, y = p.y - (h - float(p.tex.height)) / 2;
                r.drawQuadUV(p.tex, x, y, w, h, p.side == 1 ? float(p.tex.width) : 0, 0,
                             p.side == 1 ? -float(p.tex.width) : float(p.tex.width), float(p.tex.height), p.shade,
                             p.shade, p.shade, p.alpha); // right-hand portraits are mirrored (slot +0xe)
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
    }
    if (fieldMenuWanted(app)) { // the corner button and its bar
        for (app.fieldMenu.clock += dt; app.fieldMenu.clock >= 1.0f / kFramesPerSecond; app.fieldMenu.clock -= 1.0f / kFramesPerSecond)
            stepFieldMenu(app), stepPuzzleHud(app);
        drawFieldMenu(app);
        r.setOrigin(r.uiX());
    } else
        app.fieldMenu.fade = 0, app.fieldMenu.clock = 0;
    if (app.mode == Mode::Maps && app.puzzleBanner.on) { // Uty_PuzzleStart_Put, over the scene that runs it
        drawPuzzleBanner(app);
        r.setOrigin(r.uiX());
    }
    if (app.mode == Mode::Maps && app.texDisp.on) drawTexDisp(app);
    if (app.mode == Mode::Maps && app.vitaLogo.phase) drawVitaLogo(app);
    if (app.mode == Mode::Maps && app.tutoTex.on) drawTutoTex(app);
    if (app.mode == Mode::Maps && app.exclamation) drawExclamation(app);
    if (app.mode == Mode::Maps && app.padArt) drawPadArt(app);
    if (app.nowLoading.on && app.windowTex.id) { // NowLoadProc: 8 frames of (820 + 24k, 96, 24 x 24) at tex2d (464, 320)
        app.menu.begin(r, app.windowTex, &app.font);
        r.setOrigin(r.uiX());
        const bool phone = app.renderer.phoneLayout(); // 2.2.0: (448, 288), the iPhone screen's corner
        app.menu.piece((phone ? 448 : 464) + 16, (phone ? 288 : 320) + 32, 820 + 24.0f * float(app.nowLoading.frame % 8), 96, 24, 24, 1);
    }
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
            app.menu.plate(336 + dx, 104, 0, 0, app.choiceCursor == 0, 1, false);
            app.menu.plate(336 + dx, 160, 1, 1, app.choiceCursor == 1, 1, false);
        } else {
            const float cx = W - 260, cy = H - 360;
            r.fillRect(cx, cy, 200, 110, 0.05f, 0.08f, 0.2f, 0.85f);
            app.choiceHud.drawAt(r, {std::string(app.choiceCursor == 0 ? "> " : "  ") + "Yes",
                                     std::string(app.choiceCursor == 1 ? "> " : "  ") + "No"},
                                 cx + 36, cy + 24, 2.0f, 0, 0, 0.0f);
        }
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
        drawMessageBox(app, app.dialogue[pi], speaker, pageTyped(app) && !(info && info->closeAfter >= 0),
                       app.dialogue.size() > 1 ? std::to_string(app.dialoguePage + 1) + "/" + std::to_string(app.dialogue.size())
                                               : std::string());
        app.textLimit = 1 << 30;
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
        ++app.camp.anim;
        drawCamp(app);
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
        ++app.manualAnim;
        drawManual(app);
    }
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

// --selftest: renders every model through every animation and reports GL errors.
// Battles: every (stage, group) the encounter tables can start: stage, pair and monsters load,
// names resolve, a few frames draw; and one map's counter starts one by walking. Returns problems.
int battleSelfTest(App& app, int& frames, int& glErrors) {
    {
        std::set<std::pair<int, int>> starts;
        for (int area = 0; area < cr::kNumAreas; ++area) {
            cr::EventFile ef;
            if (!ef.load(app.dataDir, area)) continue;
            for (int map = 0; map < cr::kNumMaps; ++map) {
                cr::EncounterTable t;
                if (cr::encounterTable(ef, map, t))
                    for (const cr::EncounterGroup& g : t.groups) starts.insert({t.stage, g.group});
            }
        }
        if (!app.events.loaded() || app.mapNumber < 0) showMap(app, 0, 0, 2);
        int started = 0, actors = 0, unnamed = 0, noModel = 0, noAction = 0, errs = 0;
        for (const auto& [stage, group] : starts) {
            if (SDL_getenv("CR_BATTLE_TRACE")) std::printf("  battle %d on stage %d\n", group, stage), std::fflush(stdout);
            startBattle(app, {group, stage, -1, 0}, 0);
            if (!app.battle.active) continue;
            ++started;
            const std::array<int, 3> ids = app.battleData.group(group);
            const size_t want = size_t(app.battle.rules.f[0].present) + size_t(app.battle.rules.f[1].present) + // who fights
                                size_t(std::count_if(ids.begin(), ids.end(), [](int id) { return id > 0; }));
            if (app.battle.actors.size() != want) ++noModel;
            noModel += app.battle.stage.meshes.empty();
            for (const App::Battle::Actor& a : app.battle.actors) {
                ++actors;
                unnamed += a.name.empty();
                noModel += app.battle.models[a.model].meshes.empty();
                noAction += a.action < 0;
            }
            const bool trace = SDL_getenv("CR_BATTLE_TRACE") != nullptr;
            if (trace) std::printf("    started, %zu actors\n", app.battle.actors.size()), std::fflush(stdout);
            for (int f = 0; f < 3; ++f) {
                app.lastTicks = 0;
                drawFrame(app, false);
                stepBattle(app);
                ++frames;
            }
            if (trace) std::printf("    drawn\n"), std::fflush(stdout);
            errs += app.renderer.takeErrors();
            endBattle(app, true);
            if (trace) std::printf("    ended\n"), std::fflush(stdout);
            app.runner.stop();
        }
        // The first dungeon's first room: walk a unit a frame until a battle starts
        showMap(app, 0, 0, 2);
        app.rng.seed(1);
        int walkedFrames = 0;
        cr::BattleStart bs;
        auto random = [&](int n) { return app.rng.get(n); };
        bool met = false;
        for (; walkedFrames < 20000 && !met; ++walkedFrames) {
            app.encounters.walked(1.0f);
            met = app.encounters.check(random, 1, false, false, bs);
        }
        std::printf("battles: %d of %zu stage/group pairs started, %d actors (%d unnamed, %d without a model, %d without "
                    "an action), %d render API errors; walking map 0 met group %d after %d units\n",
                    started, starts.size(), actors, unnamed, noModel, noAction, errs, met ? bs.group : -1, walkedFrames);
        glErrors += errs;
        int problems = int(starts.size()) - started + unnamed + noModel + noAction + errs + !met;

        // The rules (game/battle_rules.hpp) against numbers worked out by hand
        {
            int bad = 0;
            cr::BattleRules r;
            cr::GameState gs;
            r.setupParty(gs, app.itemTable, 0);
            bad += r.f[0].hpMax != 896 || r.f[1].hpMax != 786 || r.f[0].mp != 60 || r.f[1].mp != 166; // the video's level 1
            // physical: (50 solo bonus + 150) x 200 / 100 - 100 = 300, a critical (+50) 400
            for (cr::Fighter& x : r.f) x = cr::Fighter();
            r.f[0].present = r.f[2].present = true;
            r.f[0].atk = 200, r.f[0].spd = 100, r.f[0].level = r.f[2].level = 1, r.f[0].hp = r.f[0].hpMax = 100;
            r.f[2].grd = 100, r.f[2].lck = 0, r.f[2].hp = r.f[2].hpMax = 10000, r.f[2].enemy = true;
            cr::BattleActionDef atk;
            atk.id = 10, atk.power = 150, atk.calc = 0, atk.alwaysHits = true, atk.scope = 4, atk.side = 3;
            const int who[2] = {0, -1};
            int normal = 0, crits = 0, other = 0;
            for (uint32_t seed = 1; seed <= 400; ++seed) {
                r.seed(seed);
                const cr::BattleHit h = r.physical(atk, who, 2);
                if (h.type == cr::kHitNormal && h.amount == 300) ++normal;
                else if (h.type == cr::kHitCritical && h.amount == 400) ++crits;
                else ++other;
            }
            bad += other != 0 || normal == 0 || crits == 0 || crits > 60; // 20 in 256
            r.f[0].status[cr::kStatusAttackUp] = 1; // x1.25: (200) x 250 / 100 - 100 = 400
            r.seed(7);
            for (int k = 0; k < 20; ++k) {
                const cr::BattleHit h = r.physical(atk, who, 2);
                bad += h.amount != (h.type == cr::kHitCritical ? 525 : 400);
            }
            r.f[0].status[cr::kStatusAttackUp] = 0;
            r.f[2].guarding = true; // guarding only counts for the pair
            bad += r.grdOf(2) != 100;
            r.f[0].guarding = true, r.f[0].grd = 70;
            bad += r.grdOf(0) != 140;
            // healing: -(0 + 150) x luck 80 / 100 = -120 for the pair's actor 0 casting
            cr::BattleActionDef cure;
            cure.id = 228, cure.power = 150, cure.calc = 12;
            r.f[0].lck = 80;
            bad += r.heal(cure, who, 0).amount != -120;
            // the Break: the first event puts it at 4; the enemies' damage takes 1 an action
            r.breakEvent(0, 2, 2);
            r.catchUpBreak();
            bad += r.breakGauge != 4;
            r.breakFlags &= ~0x3200u;
            r.breakEvent(2, 2, 0);
            r.catchUpBreak();
            bad += r.breakGauge != 3;
            r.breakEvent(2, 8, 0); // its critical too
            r.catchUpBreak();
            bad += r.breakGauge != 2;
            r.breakEvent(0, 2, 2); // the holder's damage doesn't count
            r.catchUpBreak();
            bad += r.breakGauge != 2;
            r.breakEvent(0, 0x10, 2); // its kill does
            r.catchUpBreak();
            bad += r.breakGauge != 3;
            r.breakEvent(2, 0x10, 0), r.breakEvent(2, 0x10, 1); // two kills by the enemies: gone, even
            r.catchUpBreak();
            bad += r.breakGauge != 0 || !(r.breakFlags & 0x400);
            // enemies: Garm (group 1) at band 0 with the pair at level 1: levels 1..3, the row's growth
            cr::BattleRules e;
            e.setupParty(gs, app.itemTable, 0);
            int lo = 99, hi = 0;
            for (uint32_t seed = 1; seed <= 50; ++seed) {
                e.seed(seed);
                e.setupEnemies(app.battleData, app.battleData.group(1), 0);
                lo = std::min(lo, e.f[2].level), hi = std::max(hi, e.f[2].level);
                const cr::MonsterDef* m = e.f[2].def;
                bad += !m || e.f[2].hpMax != int(m->hp) + 30 * e.f[2].level || e.f[2].exp != int(m->exp) + e.f[2].level * e.f[2].level;
            }
            bad += lo != 1 || hi != 3;
            // items: 21 (Chocolate) is action 100, 1000 HP
            const cr::BattleActionDef* choc = app.battleActions.get(21 + 79);
            bad += !choc || choc->calc != 17 || choc->power != 100;
            // escaping: certain while the pair holds the Break
            e.breakGauge = e.breakTarget = 2;
            cr::BattleCommand esc[2];
            esc[0].kind = cr::kCmdEscape;
            e.pair = true;
            e.beginRound(esc, app.battleActions);
            cr::BattleTurn t;
            bool escaped = false;
            while (e.nextTurn(app.battleActions, t))
                if (t.kind == cr::kCmdEscape) escaped = t.escaped;
            bad += !escaped || e.outcome() != 1;
            // genes: the new game's own, a plate's known ones, plate drops, learning, pair MP
            {
                cr::GameState g;
                cr::giveStartingItems(g);
                const std::vector<int> own = cr::usableAbilities(g, 0);
                bad += own != std::vector<int>{81} || !cr::hasSkill(g, 0, 131) || cr::hasSkill(g, 0, 133);
                g.items[481] = 1, g.items[108] = 1;
                g.party[0].genes[1] = 481;
                const std::vector<int> more = cr::usableAbilities(g, 0);
                bad += more.size() != 2 || cr::plateLearned(g, 481) != 1;
                bad += cr::plateDrop(13) != 481 || cr::plateDrop(16) != 482 || cr::plateDrop(72) != 500 || cr::plateDrop(73) != 0;
                int plates = 0, taught = 0;
                for (uint32_t seed = 1; seed <= 40; ++seed) { // monster 200 (model 13) falls to a level-1 attack
                    cr::GameState fresh;
                    cr::giveStartingItems(fresh);
                    cr::BattleRules k;
                    k.seed(seed);
                    k.setupParty(fresh, app.itemTable, 0);
                    std::array<int, 3> one{200, 0, 0};
                    k.setupEnemies(app.battleData, one, 0);
                    k.f[2].hp = 1;
                    cr::BattleCommand cmd[2];
                    cmd[0].kind = cr::kCmdAttack, cmd[0].target = 2;
                    cmd[1].kind = cr::kCmdDefend;
                    k.f[2].lck = -1000; // always hit
                    k.beginRound(cmd, app.battleActions);
                    cr::BattleTurn tt;
                    while (k.nextTurn(app.battleActions, tt)) {}
                    plates += std::count(k.drops.begin(), k.drops.end(), 481) > 0;
                    taught += !k.learned.empty();
                    if (k.outcome() == 2) {
                        k.applyRewards(fresh);
                        bad += fresh.items[481] != 1;
                    }
                }
                bad += plates != 40 || taught == 0 || taught == 40; // the plate every time, a gene 40% of the time
                cr::BattleRules pr; // a pair gene: both members pay its MP
                pr.setupParty(g, app.itemTable, 0);
                pr.setupEnemies(app.battleData, app.battleData.group(1), 0);
                pr.pair = true;
                cr::BattleCommand gc[2];
                gc[0].kind = cr::kCmdGene, gc[0].action = cr::geneAction(94), gc[0].target = 2;
                const int mp0 = pr.f[0].mp, mp1 = pr.f[1].mp;
                const int cost = pr.geneCost(*app.battleActions.get(cr::geneAction(94)), 0); // Musiea's MP 1/2 halves it
                pr.beginRound(gc, app.battleActions);
                cr::BattleTurn gt;
                bool cast = false;
                while (pr.nextTurn(app.battleActions, gt)) cast |= gt.kind == cr::kCmdGene && gt.actors[0] == 0;
                bad += !cast || pr.f[0].mp != mp0 - cost || pr.f[1].mp != mp1 - cost;
                std::printf("genes: new game knows %zu, a plate adds %zu, plates dropped %d/40, genes taught %d/40\n", own.size(),
                            more.size() - own.size(), plates, taught);
            }
            { // 1.0.1 / 2.2.0 decompile checks: levels from 20 grow every stat (pcparam_change_lv),
              // seeds share that bonus's 5000, guests' Speed / Luck are their Defense, a monster
              // teaches the first of its six genes not known (btlcalc_0506)
                int gbad = 0;
                gbad += cr::levelGrowth(20, cr::kHp) != 0 || cr::levelGrowth(21, cr::kHp) != 10 ||
                        cr::levelGrowth(22, cr::kHp) != 30 || cr::levelGrowth(40, cr::kHp) != 2100 ||
                        cr::levelGrowth(99, cr::kHp) != 31600 || cr::levelGrowth(21, cr::kAttack) != 1 ||
                        cr::levelGrowth(99, cr::kLuck) != 3160;
                cr::PartyMember pm;
                pm.level = 99;
                gbad += cr::seedRoom(pm, cr::kAttack) != 5000 - 3160;
                pm.seeds[0] = 5000;
                gbad += cr::statBonus(pm, cr::kAttack) != 5000 || cr::seedRoom(pm, cr::kAttack) != 0;
                cr::GameState fresh;
                cr::giveStartingItems(fresh);
                cr::BattleRules gr;
                gr.setupSpecial(fresh, app.itemTable, 372, 1); // Ayuta's guest
                const bool quirk = gr.f[1].guest && gr.f[1].spd == gr.f[1].grd && gr.f[1].lck == gr.f[1].grd &&
                                   gr.f[1].hpMax == cr::baseStat(gr.f[1].character, cr::kHp) + 50 * gr.f[1].level;
                gbad += !quirk;
                int firstOk = 0, tries = 0; // monster 203 (row +0xb4.. 92 170 89 175 100 182) at level 30
                for (uint32_t seed = 1; seed <= 60 && firstOk == tries; ++seed) {
                    cr::GameState g2;
                    cr::giveStartingItems(g2);
                    g2.items[92] = 1; // the first already known: the second comes next
                    cr::BattleRules k;
                    k.seed(seed);
                    k.setupParty(g2, app.itemTable, 0);
                    std::array<int, 3> one{203, 0, 0};
                    k.setupEnemies(app.battleData, one, 0);
                    k.f[2].level = 30, k.f[2].hp = 1, k.f[2].lck = -1000;
                    cr::BattleCommand cmd[2];
                    cmd[0].kind = cr::kCmdAttack, cmd[0].target = 2;
                    cmd[1].kind = cr::kCmdDefend;
                    k.beginRound(cmd, app.battleActions);
                    cr::BattleTurn tt;
                    while (k.nextTurn(app.battleActions, tt)) {}
                    if (!k.learned.empty()) ++tries, firstOk += k.learned.size() == 1 && k.learned[0] == 170;
                }
                gbad += tries == 0 || firstOk != tries;
                const bool armor = cr::armorSkill(222) == 148 && cr::armorSkill(226) == 149 && cr::armorSkill(227) == 150 &&
                                   cr::armorSkill(228) == 0 && cr::armorSkill(421) == 148 && cr::armorSkill(446) == 0 &&
                                   cr::armorSkill(215) == 0;
                gbad += !armor;
                // _get_random: MT19937 (seed 5489 when unseeded) and the original's bounded draw
                cr::GameRandom gr0, gr1;
                std::mt19937 ref;
                bool mt = true;
                for (int k = 0; k < 1000; ++k) mt &= gr0.next() == uint32_t(ref());
                const int first = gr1.below(10000); // 3499211612 >> 1, less 10000 << 17, below 10000 << 16
                int hist[6] = {};
                cr::GameRandom gr2(77);
                for (int k = 0; k < 60000; ++k) ++hist[gr2.get(6)];
                bool flat = true;
                for (int h : hist) flat &= h > 9500 && h < 10500;
                gbad += !mt || first != 6696 || !flat || gr2.get(0) != 0;
                std::printf("random: MT19937 %s, first draw below 10000 %d (6696), get_random(6) even %s\n", mt ? "yes" : "NO",
                            first, flat ? "yes" : "NO");
                std::printf("level growth: +%d HP / +%d each by level 99, seeds share the 5000, guests' Speed / Luck their "
                            "Defense %s, the first unknown gene taught %d/%d, resisting armors %s\n", cr::levelGrowth(99, cr::kHp),
                            cr::levelGrowth(99, cr::kAttack), quirk ? "yes" : "NO", firstOk, tries, armor ? "yes" : "NO");
                bad += gbad;
            }
            { // btlcalc_0404's Human Genes: Risk Breaker (203) and Trompe-L'oeil (201)
                int rbBad = 0, cancels = 0, rbLanded = 0, rbPaid = 0;
                bool once = true;
                for (uint32_t seed = 1; seed <= 200; ++seed) {
                    cr::GameState fresh;
                    cr::giveStartingItems(fresh);
                    for (int pass = 0; pass < 2; ++pass) {
                        cr::BattleRules k;
                        k.seed(seed);
                        k.setupParty(fresh, app.itemTable, 0);
                        k.setupEnemies(app.battleData, app.battleData.group(1), 0);
                        for (int i = 0; i < 5; ++i)
                            if (k.f[size_t(i)].present) k.f[size_t(i)].mp = k.f[size_t(i)].mpMax = 999;
                        k.f[0].spd = 9999;            // acts first
                        k.f[2].lck = -1000;           // always hit
                        k.f[2].hp = k.f[2].hpMax = 99999;
                        k.breakStarted = true;
                        k.breakGauge = k.breakTarget = 4; // the pair holds the Break
                        cr::BattleCommand cmd[2];
                        cmd[0].kind = cr::kCmdGene, cmd[0].action = pass ? 201 : 203, cmd[0].target = 2;
                        cmd[1].kind = cr::kCmdDefend;
                        const int hp0 = k.f[0].hp;
                        k.beginRound(cmd, app.battleActions);
                        cr::BattleTurn tt;
                        bool enemyActed = false, cancelled = false;
                        while (k.nextTurn(app.battleActions, tt)) {
                            if (tt.actors[0] == 2) enemyActed = true;
                            if (tt.actors[0] != 0) continue;
                            int self = -1, dealt = 0;
                            for (const cr::BattleHit& h : tt.hits)
                                if (h.target == 0) self = h.amount;
                                else dealt = std::max(dealt, h.amount);
                            if (pass == 0 && dealt > 0) ++rbLanded, rbPaid += self == hp0 / 4 && k.f[0].hp == hp0 - hp0 / 4;
                            if (pass == 0 && dealt <= 0) rbBad += self != -1; // nothing landed: nothing paid
                            for (const cr::BattleNotice& n : tt.notices) cancelled |= n.entry == 3 && n.fighter == 2;
                        }
                        if (pass == 1 && cancelled) {
                            ++cancels;
                            once &= !enemyActed;
                            for (int r = 0; r < 20; ++r) { // never again this battle
                                k.f[2].hp = k.f[2].hpMax;
                                k.beginRound(cmd, app.battleActions);
                                while (k.nextTurn(app.battleActions, tt))
                                    for (const cr::BattleNotice& n : tt.notices) once &= n.entry != 3;
                            }
                        }
                    }
                }
                rbBad += rbLanded == 0 || rbPaid != rbLanded || cancels == 0 || cancels > 60 || !once;
                std::printf("human genes: Risk Breaker cost a quarter of the HP %d/%d times, Trompe-L'oeil cancelled %d/200 "
                            "(once a battle %s)\n", rbPaid, rbLanded, cancels, once ? "yes" : "NO");
                bad += rbBad;
            }
            // skills: the damage scaling, criticals, MP, healing, regen, the Gates, level-up boosts
            {
                int sbad = 0;
                cr::BattleRules q;
                for (cr::Fighter& x : q.f) x = cr::Fighter();
                q.f[0].present = q.f[2].present = true;
                q.f[0].atk = 200, q.f[0].spd = 100, q.f[0].level = q.f[2].level = 1, q.f[0].hp = q.f[0].hpMax = 100;
                q.f[2].grd = 100, q.f[2].lck = -1000, q.f[2].hp = q.f[2].hpMax = 10000, q.f[2].enemy = true;
                cr::BattleActionDef hit;
                hit.id = 10, hit.power = 150, hit.calc = 0, hit.alwaysHits = true;
                const int who[2] = {0, -1};
                auto plain = [&](int want, bool critOnly) { // every non-critical (or critical) hit equals `want`
                    for (uint32_t seed = 1; seed <= 300; ++seed) {
                        q.seed(seed);
                        const cr::BattleHit h = q.physical(hit, who, 2);
                        if ((h.type == cr::kHitCritical) == critOnly && h.amount != want) return false;
                    }
                    return true;
                };
                q.f[2].skills[146 - 131] = true; // Physical Resistance: 300 x 75%
                sbad += !plain(225, false);
                q.f[0].skills[139 - 131] = true; // and Physical Attack: back to 100%
                sbad += !plain(300, false);
                q.f[2].skills[146 - 131] = q.f[0].skills[139 - 131] = false;
                q.f[0].skills[137 - 131] = true; // Critical Pierce: (250) x 2 - 0
                sbad += !plain(500, true);
                q.f[0].skills[134 - 131] = true; // and Boost Critical: x2
                sbad += !plain(1000, true);
                q.f[0].skills[137 - 131] = q.f[0].skills[134 - 131] = false;
                q.f[0].skills[135 - 131] = true; // Critical First Attack: once
                q.seed(9);
                sbad += q.physical(hit, who, 2).type != cr::kHitCritical;
                q.f[0].firstAttack = false;
                q.f[0].skills[135 - 131] = false;
                cr::BattleActionDef gene;
                gene.mp = 20;
                q.f[0].skills[132 - 131] = true;
                sbad += q.geneCost(gene, 0) != 10;
                cr::BattleActionDef cure;
                cure.power = 150, cure.calc = 12;
                q.f[0].lck = 80, q.f[0].skills[155 - 131] = true;
                sbad += q.heal(cure, who, 0).amount != -240;
                q.f[0].hp = 50, q.f[0].skills[133 - 131] = true; // regen: +5 of 100
                q.endRound();
                sbad += q.f[0].hp != 55;
                // the Gates: Dragon Gate Stage 1 (175, on plate 482) adds its item's Attack
                cr::GameState g;
                cr::giveStartingItems(g);
                const int base = cr::memberStat(g, 0, cr::kAttack, app.itemTable);
                g.items[482] = 1, g.items[175] = 1;
                g.party[0].genes[1] = 482;
                const cr::ItemParam* gate = app.itemTable.item(175);
                sbad += !gate || cr::memberStat(g, 0, cr::kAttack, app.itemTable) != base + gate->stat[cr::kAttack];
                g.items[164] = 1; // HP Boost isn't on a worn plate: nothing
                cr::levelUpBoosts(g, 0, 3);
                sbad += g.party[0].boosts[0] != 0;
                g.items[166] = 1, g.items[483] = 1, g.party[0].genes[2] = 483; // Attack Boost (plate 483): +1 a level
                cr::levelUpBoosts(g, 0, 3);
                sbad += g.party[0].boosts[2] != 3;
                // the other calcs, each through a round: member 0 uses the action on its target
                auto runAs = [&](int action, int target, cr::BattleRules& rr) {
                    for (cr::Fighter& x : rr.f) x = cr::Fighter();
                    for (int i : {0, 1, 2, 3}) {
                        cr::Fighter& x = rr.f[size_t(i)];
                        x.present = true, x.enemy = i >= 2, x.level = 1, x.hp = x.hpMax = 5000, x.mp = x.mpMax = 400;
                        x.atk = 100, x.grd = 0, x.spd = i == 0 ? 10000 : 1, x.lck = 0, x.character = i < 2 ? i : -1;
                    }
                    rr.purse = 1000;
                    cr::BattleCommand cc[2];
                    cc[0].kind = cr::kCmdGene, cc[0].action = action, cc[0].target = target;
                    cc[1].kind = cr::kCmdDefend;
                    rr.beginRound(cc, app.battleActions);
                    cr::BattleTurn tt, first;
                    bool got = false;
                    while (rr.nextTurn(app.battleActions, tt))
                        if (!got && tt.actors[0] == 0) first = tt, got = true;
                    return first;
                };
                int cbad = 0;
                { cr::BattleRules rr; runAs(402, 2, rr); if (rr.f[2].hp != 4000) std::printf("  fixed: hp %d %d mp %d oz %d\n", rr.f[2].hp, rr.f[3].hp, rr.f[2].mp, rr.ozStolen), ++cbad; }
                { cr::BattleRules rr; runAs(433, 2, rr); if (rr.f[2].hp != 3000 || rr.f[3].hp != 5000) std::printf("  shared: hp %d %d mp %d oz %d\n", rr.f[2].hp, rr.f[3].hp, rr.f[2].mp, rr.ozStolen), ++cbad; }
                { cr::BattleRules rr; runAs(432, 2, rr); if (rr.f[2].hp != 1) std::printf("  to1: hp %d %d mp %d oz %d\n", rr.f[2].hp, rr.f[3].hp, rr.f[2].mp, rr.ozStolen), ++cbad; }
                { cr::BattleRules rr; runAs(404, 2, rr); if (rr.f[2].hp != 3500) std::printf("  pct: hp %d %d mp %d oz %d\n", rr.f[2].hp, rr.f[3].hp, rr.f[2].mp, rr.ozStolen), ++cbad; }
                { cr::BattleRules rr; runAs(403, 2, rr); if (rr.f[2].mp != 300) std::printf("  mp: hp %d %d mp %d oz %d\n", rr.f[2].hp, rr.f[3].hp, rr.f[2].mp, rr.ozStolen), ++cbad; }
                { cr::BattleRules rr; runAs(409, 2, rr); if (rr.ozStolen != 100 || rr.purse != 900) std::printf("  oz: hp %d %d mp %d oz %d\n", rr.f[2].hp, rr.f[3].hp, rr.f[2].mp, rr.ozStolen), ++cbad; }
                { cr::BattleRules rr; runAs(109, 0, rr); cbad += !rr.doubleExp; }
                { // a barrier stops the next hostile gene, then is gone
                    cr::BattleRules rr;
                    runAs(212, 2, rr);
                    const int plain = 5000 - rr.f[2].hp;
                    cr::BattleRules rc;
                    for (cr::Fighter& x : rc.f) x = cr::Fighter();
                    for (int i : {0, 1, 2, 3}) {
                        cr::Fighter& x = rc.f[size_t(i)];
                        x.present = true, x.enemy = i >= 2, x.level = 1, x.hp = x.hpMax = 5000, x.mp = x.mpMax = 400;
                        x.atk = 100, x.spd = i == 0 ? 10000 : 1, x.character = i < 2 ? i : -1;
                    }
                    rc.f[2].status[cr::kStatusBarrier] = 5;
                    cr::BattleCommand cc[2];
                    cc[0].kind = cr::kCmdGene, cc[0].action = 212, cc[0].target = 2;
                    cc[1].kind = cr::kCmdDefend;
                    rc.beginRound(cc, app.battleActions);
                    cr::BattleTurn tt;
                    int hp2 = -1;
                    while (rc.nextTurn(app.battleActions, tt))
                        if (tt.actors[0] == 0) hp2 = rc.f[2].hp;
                    if (plain <= 0 || hp2 != 5000 || rc.f[2].status[cr::kStatusBarrier] != 0)
                        std::printf("  barrier: plain %d, with %d, left %d\n", plain, hp2, rc.f[2].status[cr::kStatusBarrier]), ++cbad;
                }
                { // revive: a fallen member back with half its HP
                    cr::BattleRules rr;
                    for (cr::Fighter& x : rr.f) x = cr::Fighter();
                    for (int i : {0, 1, 2}) {
                        cr::Fighter& x = rr.f[size_t(i)];
                        x.present = true, x.enemy = i >= 2, x.level = 1, x.hp = x.hpMax = 1000, x.mp = x.mpMax = 400;
                        x.atk = 100, x.spd = i == 0 ? 10000 : 1, x.character = i < 2 ? i : -1;
                    }
                    rr.f[1].hp = 0;
                    cr::BattleCommand cc[2];
                    cc[0].kind = cr::kCmdGene, cc[0].action = 230, cc[0].target = 1;
                    rr.beginRound(cc, app.battleActions);
                    cr::BattleTurn tt;
                    while (rr.nextTurn(app.battleActions, tt)) {}
                    cbad += rr.f[1].hp != 500;
                }
                { // poison: 10% a round; invincible: no damage
                    cr::BattleRules rr;
                    for (cr::Fighter& x : rr.f) x = cr::Fighter();
                    rr.f[2].present = true, rr.f[2].hp = rr.f[2].hpMax = 1000, rr.f[2].status[cr::kStatusPoison] = 5;
                    rr.f[0].present = true, rr.f[0].hp = rr.f[0].hpMax = 1000;
                    rr.endRound();
                    cbad += rr.f[2].hp != 900;
                }
                { // story line-ups and the guest's rotation
                    cr::GameState g;
                    cr::giveStartingItems(g);
                    cr::BattleRules a;
                    a.setupParty(g, app.itemTable, 0);
                    a.setupSpecial(g, app.itemTable, 371, 0);
                    cbad += a.f[0].character != 0 || a.f[1].present;
                    cr::BattleRules gb;
                    gb.setupParty(g, app.itemTable, 0);
                    gb.setupSpecial(g, app.itemTable, 370, 0);
                    cbad += !gb.allyAuto || gb.f[1].character != 4 || gb.f[1].level != gb.f[0].level + 5 || !gb.f[1].guest;
                    gb.setupEnemies(app.battleData, app.battleData.group(370), 0);
                    cr::BattleCommand cc[2];
                    cc[0].kind = cr::kCmdDefend;
                    int ally[3] = {0, 0, 0};
                    for (int r = 0; r < 3; ++r) {
                        gb.f[2].hp = gb.f[2].hpMax = 99999;
                        if (gb.f[3].present) gb.f[3].hp = gb.f[3].hpMax = 99999;
                        gb.f[1].hp = gb.f[1].hpMax, gb.f[0].hp = gb.f[0].hpMax, gb.f[1].mp = gb.f[1].mpMax;
                        gb.beginRound(cc, app.battleActions);
                        cr::BattleTurn tt;
                        while (gb.nextTurn(app.battleActions, tt))
                            if (tt.actors[0] == 1 && !ally[r]) ally[r] = tt.action;
                        gb.endRound();
                    }
                    if (ally[0] != 212 || ally[1] != 14 || ally[2] != 14)
                        std::printf("  guest rotation %d %d %d\n", ally[0], ally[1], ally[2]), ++cbad;
                    cr::BattleRules zb;
                    zb.setupParty(g, app.itemTable, 1);
                    zb.setupSpecial(g, app.itemTable, 372, 1);
                    cbad += zb.f[0].character != 3 || zb.f[1].character != 6 || zb.allyScript != 2;
                }
                { // a monster with row +0x3e 2 acts twice; Asura Lotus strikes 2..5 times
                    cr::BattleRules rr;
                    cr::GameState g;
                    rr.setupParty(g, app.itemTable, 0);
                    std::array<int, 3> one{81, 0, 0};
                    rr.setupEnemies(app.battleData, one, 0);
                    rr.f[0].hp = rr.f[0].hpMax = rr.f[1].hp = rr.f[1].hpMax = 99999;
                    rr.f[2].hp = rr.f[2].hpMax = 99999;
                    cr::BattleCommand cc[2];
                    cc[0].kind = cc[1].kind = cr::kCmdDefend;
                    rr.beginRound(cc, app.battleActions);
                    cr::BattleTurn tt;
                    int turns = 0;
                    while (rr.nextTurn(app.battleActions, tt)) turns += tt.actors[0] == 2;
                    if (turns != 2) std::printf("  twice-acting monster acted %d times\n", turns), ++cbad;
                    cr::BattleRules lo;
                    int fewest = 99, most = 0;
                    for (uint32_t seed = 1; seed <= 30; ++seed) {
                        lo.seed(seed);
                        int n = 0;
                        runAs(207, 2, lo);
                        cr::BattleRules q2;
                        q2.seed(seed);
                        for (cr::Fighter& x : q2.f) x = cr::Fighter();
                        for (int i : {0, 1, 2}) {
                            cr::Fighter& x = q2.f[size_t(i)];
                            x.present = true, x.enemy = i >= 2, x.level = 1, x.hp = x.hpMax = 99999, x.mp = x.mpMax = 400;
                            x.atk = 10, x.spd = i == 0 ? 10000 : 1, x.character = i < 2 ? i : -1;
                        }
                        cr::BattleCommand c2[2];
                        c2[0].kind = cr::kCmdGene, c2[0].action = 207, c2[0].target = 2;
                        c2[1].kind = cr::kCmdDefend;
                        q2.beginRound(c2, app.battleActions);
                        while (q2.nextTurn(app.battleActions, tt)) n += tt.actors[0] == 0;
                        fewest = std::min(fewest, n), most = std::max(most, n);
                    }
                    if (fewest != 2 || most != 5) std::printf("  Asura Lotus %d..%d times\n", fewest, most), ++cbad;
                }
                { // Steal Item: monster 1's item, once
                    int got = 0, twice = 0;
                    for (uint32_t seed = 1; seed <= 30; ++seed) {
                        cr::BattleRules rr;
                        rr.seed(seed);
                        cr::GameState g;
                        rr.setupParty(g, app.itemTable, 0);
                        std::array<int, 3> one{1, 0, 0};
                        rr.setupEnemies(app.battleData, one, 0);
                        rr.f[2].hp = rr.f[2].hpMax = 99999, rr.f[2].lck = 0;
                        rr.f[0].lck = 500;
                        for (int r = 0; r < 2; ++r) {
                            cr::BattleCommand cc[2];
                            cc[0].kind = cr::kCmdGene, cc[0].action = 223, cc[0].target = 2;
                            cc[1].kind = cr::kCmdDefend;
                            rr.beginRound(cc, app.battleActions);
                            cr::BattleTurn tt;
                            while (rr.nextTurn(app.battleActions, tt)) {}
                            rr.endRound();
                        }
                        got += !rr.stolen.empty() && rr.stolen[0] == 3;
                        twice += rr.stolen.size() > 1;
                    }
                    if (got == 0 || twice) std::printf("  steal: %d got, %d twice\n", got, twice), ++cbad;
                }
                std::printf("calcs: fixed, split by targets, to 1, %%HP, MP, Oz stolen, Apples, barrier, revive, poison: %d problems\n", cbad);
                sbad += cbad;
                std::printf("skills: resistance, attack, pierce, boost critical, first attack, MP 1/2, recovery, regen, "
                            "Gate +%d, boosts: %d problems\n", gate ? gate->stat[cr::kAttack] : 0, sbad);
                bad += sbad;
            }
            std::printf("battle rules: %d physical hits (%d critical) as worked out, healing, Break, enemy levels %d..%d, "
                        "items, escape: %d problems\n", normal + crits, crits, lo, hi, bad);
            problems += bad;
        }

        // Every effect (eff.bin's 98): loaded, played on an enemy, drawn without render API errors
        {
            int loaded = 0, played = 0, ferrs = 0;
            startBattle(app, {1, 0, -1, 0}, 0);
            if (app.battle.active) {
                for (int id = 0; id < 100; ++id) {
                    if (!effectModel(app, id)) continue;
                    ++loaded;
                    spawnEffect(app, id, 2);
                    played += !app.battle.effects.empty();
                    for (int f = 0; f < 6; ++f) stepBattle(app);
                    app.lastTicks = 0;
                    drawFrame(app, false);
                    ferrs += app.renderer.takeErrors();
                    app.battle.effects.clear();
                }
                endBattle(app, false);
                app.runner.stop();
            }
            std::printf("battle effects: %d of eff.bin's entries loaded, %d played, %d render API errors\n", loaded, played, ferrs);
            problems += (loaded < 98) + (played < loaded) + ferrs;
            glErrors += ferrs;
        }

        // The bosses' staged actions (btlact_430..436): each plays its own slot (the boss models'
        // 14th / 15th, read past the usual 13), steps through its cameras, flashes and shakes
        // where it has them, hits at its frame and ends after its wait; hits shake the screen;
        // and the effects' textures scroll with their materials' actions
        {
            // quake: one before the hit; after: the strongest from the hit on (3 = the calc's own, when a hit lands)
            struct Want { int action, group, hit, wait, cams; bool flash, quake; float after; };
            const Want wants[] = {{430, 340, 140, 40, 3, true, true, 3},    {432, 348, -1, 40, 5, false, false, 0},
                                  {433, 352, 112, 40, 0, false, false, 0}, {434, 352, 130, 65, 2, true, true, 3},
                                  {435, 356, 163, 40, 5, true, false, 3},   {436, 356, 61, 50, 4, false, false, 10}};
            int sbad = 0, rerrs = 0, ran = 0, hitQuakes = 0;
            const cr::GameState saved = app.game;
            for (const Want& w : wants) {
                app.game = saved;
                startBattle(app, {w.group, 0, -1, 0}, 0);
                App::Battle& b = app.battle;
                const cr::BattleActionDef* def = app.battleActions.get(w.action);
                if (!b.active || !def) { ++sbad; continue; }
                b.rules.forceAction = w.action;
                int hitAt = -1, endAt = -1, cams = 0, slot = -1;
                float flash = 0, quake = 0, after = 0;
                bool started = false, landed = false;
                for (int f = 0; f < 8000 && b.active && endAt < 0; ++f) {
                    if (b.phase == App::Battle::Command && b.menu == App::Battle::Top) { // the pair defends
                        battleChooseTop(app, 0);
                        battleChooseCommand(app, 3);
                        if (b.menu == App::Battle::Commands) battleChooseCommand(app, 3);
                    }
                    const bool was = b.turnOn && b.turn.action == w.action;
                    stepBattle(app);
                    const bool on = b.turnOn && b.turn.action == w.action;
                    if (on && !started) {
                        started = true;
                        if (const App::Battle::Actor* u = battleActor(app, b.turn.actors[0])) slot = u->slot;
                    }
                    if (on) {
                        cams = std::max(cams, b.stageCams);
                        flash = std::max(flash, b.flash.on ? b.flash.alpha : 0.0f);
                        if (!b.hitsShown) quake = std::max({quake, std::fabs(b.quake.off[0]), std::fabs(b.quake.off[1])});
                        else if (b.quake.mode) after = std::max(after, b.quake.amp);
                        if (b.hitsShown && hitAt < 0) {
                            hitAt = b.turnFrame;
                            for (const cr::BattleHit& h : b.turn.hits) // btlcalc_0404 / 0406 shake for a hit that lands
                                landed |= h.type != cr::kHitGuard && h.type != cr::kHitMiss && (def->calc == 1 ? h.amount >= 0 : h.amount > 0);
                        }
                        if (b.turnFrame == 20 || b.turnFrame == hitAt) {
                            app.lastTicks = 0;
                            drawFrame(app, false);
                            rerrs += app.renderer.takeErrors();
                        }
                    }
                    if (was && !b.turnOn) endAt = b.turnFrame;
                }
                const float wantAfter = w.after > 3 ? w.after : w.after > 0 && landed ? 3.0f : 0.0f;
                const int wantHit = w.hit >= 0 ? w.hit : app.battleData.anim(battleActor(app, 2)->modelId, def->cls).loopStart + 45;
                const int bad = !started + (slot != def->cls) + (hitAt != wantHit) + (endAt != wantHit + w.wait) +
                                (cams != w.cams) + (w.flash != (flash >= 254)) + (w.quake != (quake > 0.5f)) + (after != wantAfter);
                if (bad)
                    std::printf("  staged %d: slot %d (cls %d), hit at %d (want %d), ended at %d, %d cameras, flash %.0f, "
                                "quake %.1f, after the hit %.0f\n", w.action, slot, def->cls, hitAt, wantHit, endAt, cams, flash,
                                quake, after);
                sbad += bad;
                ran += started;
                endBattle(app, false);
                app.runner.stop();
            }
            // a plain hit (btlcalc_0404) shakes the screen for 5 frames
            app.game = saved;
            startBattle(app, {1, 0, -1, 0}, 0);
            if (app.battle.active) {
                App::Battle& b = app.battle;
                for (int f = 0; f < 3000 && b.active && !hitQuakes; ++f) {
                    if (b.phase == App::Battle::Command && b.menu == App::Battle::Top) {
                        battleChooseTop(app, 1);
                        battleChooseCommand(app, 0);
                        if (b.menu == App::Battle::Targets) battleChooseTarget(app, b.picks[0]);
                    }
                    stepBattle(app);
                    if (b.turnOn && b.hitsShown && b.turn.actors[0] < 2 && !b.turn.hits.empty() && b.turn.hits[0].amount > 0)
                        hitQuakes = b.quake.mode == 1 && b.quake.frames == 5;
                }
                endBattle(app, false);
                app.runner.stop();
            }
            // effect 48's light shafts: their offset moves between frames 5 and 15
            int scrolling = 0, moved = 0;
            startBattle(app, {1, 0, -1, 0}, 0);
            if (const LoadedModel* fx = app.battle.active ? effectModel(app, 48) : nullptr)
                for (const cr::Material& mt : fx->cpu.materials) {
                    float s0[2] = {0, 0}, s1[2] = {0, 0}, k0[2] = {1, 1}, k1[2] = {1, 1};
                    if (!effectUvMotion(*fx, &mt, 5, s0, k0)) continue;
                    ++scrolling;
                    effectUvMotion(*fx, &mt, 15, s1, k1);
                    moved += s0[0] != s1[0] || s0[1] != s1[1] || k0[0] != k1[0] || k0[1] != k1[1];
                }
            if (app.battle.active) endBattle(app, false);
            app.runner.stop();
            app.game = saved;
            const int boss15 = app.battleData.anim(9, 14).loopEnd > 0 || app.battleData.anim(9, 14).hit > 0;
            std::printf("battle staging: %d of 6 boss actions staged, hit quake %s, effect 48: %d of %d scrolling materials move, "
                        "boss slots past 13 %s; %d problems, %d render API errors\n", ran, hitQuakes ? "yes" : "no", moved,
                        scrolling, boss15 ? "read" : "missing", sbad, rerrs);
            problems += sbad + rerrs + (ran != 6) + !hitQuakes + (scrolling == 0 || moved == 0);
            glErrors += rerrs;
        }

        // The weather goes on in battles with the "(128)" copies (snow: slots 62 / 65, stepped by the
        // battle's frames), and the tower's Space backdrop scrolls (Up +1, Down -1 a frame, Stop
        // still; the fog's drift wraps past 480), drawn on the field and in battles without errors
        {
            int fbad = 0, ferrs = 0;
            loadSceneEffectSet(app, 0);
            setSceneEffect(app, true, 5);
            startBattle(app, {1, 0, -1, 0}, 0);
            if (app.battle.active) {
                fbad += !app.fx.tex[62 - 49].id || !app.fx.tex[65 - 49].id || !app.fx.tex[61 - 49].id;
                const int t0 = app.fx.timerA[5];
                for (int f = 0; f < 10; ++f) stepBattle(app);
                fbad += app.fx.timerA[5] != t0 + 10;
            app.environmentEffects = false; // Options: Environment Effects off - the weather stands still, undrawn
            for (int f = 0; f < 5; ++f) stepBattle(app);
            fbad += app.fx.timerA[5] != t0 + 10;
            app.environmentEffects = true;
                app.lastTicks = 0;
                drawFrame(app, false);
                ferrs += app.renderer.takeErrors();
                endBattle(app, false);
                app.runner.stop();
            } else ++fbad;
            endSceneEffects(app);
            loadSceneEffectSet(app, 4);
            fbad += !app.fx.tex[66 - 49].id || !app.fx.tex[68 - 49].id || !app.fx.tex[72 - 49].id;
            setSceneEffect(app, true, 13); // d0507, rising
            for (int f = 0; f < 100; ++f) stepSceneEffects(app);
            const int rose = app.fx.spaceRise;
            setSceneEffect(app, false, 13);
            setSceneEffect(app, true, 17); // d0511, falling
            for (int f = 0; f < 30; ++f) stepSceneEffects(app);
            const int fell = app.fx.spaceRise;
            setSceneEffect(app, false, 17);
            setSceneEffect(app, true, 6); // d0501, still
            for (int f = 0; f < 400; ++f) stepSceneEffects(app);
            const int drift = app.fx.spaceDrift;
            fbad += rose != 100 || fell != 70 || app.fx.spaceRise != 70 || drift != 530 - 481;
            startBattle(app, {1, 0, -1, 0}, 0);
            if (app.battle.active) {
                app.lastTicks = 0;
                drawFrame(app, false);
                ferrs += app.renderer.takeErrors();
                endBattle(app, false);
                app.runner.stop();
            }
            endSceneEffects(app);
            std::printf("battle weather: snow over a battle with its (128) copies, Space backdrop rose %d, fell to %d, "
                        "drift %d; %d problems, %d render API errors\n", rose, fell, drift, fbad, ferrs);
            problems += fbad + ferrs;
            glErrors += ferrs;
        }

        // Battle voices (voiced data only): the 2.2.0 action records line up (Particle Cannon's class
        // 13, Attack's motion), the speakers come from the name ids, and a fought battle says its
        // lines - commands, attacks, hits, the victory - from the table's clips
        if (!app.battleVoiceTable.empty() && app.sound.voiceClips(true) == 0) startSound(app), vitaAudio(app); // (tests run before it)
        if (btlvoice::on(app)) {
            int vbad = 0;
            const cr::BattleActionDef* cannon = app.battleActions.get(430);
            vbad += !cannon || cannon->cls != 13 || cannon->calc != 0 || app.battleActions.count() < 450;
            startBattle(app, {1, 0, -1, 0}, 0);
            App::Battle& b = app.battle;
            if (b.active) {
                vbad += btlvoice::speaker(app, 0) != b.rules.f[0].character; // the party's own
                app.sound.battleLineLog.clear();
                for (int f = 0; f < 6000 && b.active && b.phase != App::Battle::Results && b.phase != App::Battle::Lost; ++f) {
                    if (b.phase == App::Battle::Command && b.menu == App::Battle::Top) {
                        battleChooseTop(app, 0);
                        for (int k = 0; k < 2 && b.phase == App::Battle::Command && b.menu == App::Battle::Commands; ++k) {
                            battleChooseCommand(app, 0);
                            if (b.menu == App::Battle::Targets) battleChooseTarget(app, b.picks[0]);
                        }
                    }
                    stepBattle(app);
                }
                const std::vector<int> log = app.sound.battleLineLog;
                const bool won = b.phase == App::Battle::Results;
                int inRange = 0;
                for (int c : log) inRange += c > 0 && c < app.sound.voiceClips(true);
                vbad += log.size() < 3 || inRange != int(log.size());
                if (SDL_getenv("CR_BATTLE_DEBUG"))
                    for (int c : log)
                        for (size_t i = 0; i < app.battleVoiceTable.size(); ++i)
                            if (app.battleVoiceTable[i] == c) { std::printf("  line: speaker %zu situation %zu (clip %d)\n", i / 1000, i % 1000 + 1, c); break; }
                std::printf("battle voices: %zu lines in a %s battle (%d clips in range), action 430 class %d; %d problems\n",
                            log.size(), won ? "won" : "lost", inRange, cannon ? cannon->cls : -1, vbad);
                endBattle(app, won);
                app.runner.stop();
            } else ++vbad;
            problems += vbad;
        } else
            std::printf("battle voices: none in this data (%zu situations, %d clips)\n", app.battleVoiceTable.size(),
                        app.sound.voiceClips(true));

        // Whole battles through the scene and its menus: pair or solo attacks, defending, an item,
        // escaping - every one ends (won, lost or fled) and draws without render API errors
        {
            app.rng.seed(5);
            int won = 0, lost = 0, fled = 0, stuck = 0, rerrs = 0, n = 0, levelled = 0;
            const cr::GameState saved = app.game;
            std::vector<std::pair<int, int>> list(starts.begin(), starts.end());
            for (size_t i = 0; i < list.size() && n < 24; i += std::max<size_t>(1, list.size() / 24), ++n) {
                app.game = saved;
                app.game.items[21] = 2;
                startBattle(app, {list[i].second, list[i].first, -1, int(i % 3 == 2 ? 1 : 0)}, 0);
                App::Battle& b = app.battle;
                if (!b.active) continue;
                const int style = n % 4; // 0 pair attacks, 1 solo attacks, 2 defend then attack + an item, 3 escape
                int frame = 0, ended = 0;
                for (; frame < 30000 && b.active; ++frame) {
                    if (b.phase == App::Battle::Command && b.menu == App::Battle::Top) {
                        if (style == 3 && b.rules.round >= 1) battleChooseTop(app, 2), battleEscapeAnswer(app, true);
                        else {
                            const bool both = b.rules.f[0].alive() && b.rules.f[1].alive();
                            battleChooseTop(app, style == 0 && both ? 1 : 0);
                            for (int k = 0; k < 2 && b.phase == App::Battle::Command; ++k) {
                                if (style == 2 && b.rules.round == 0 && k == 0) { battleChooseCommand(app, 3); continue; }
                                if (style == 2 && b.rules.round == 1 && k == 0 && app.game.items[21] > 0) {
                                    battleChooseCommand(app, 2);
                                    if (b.menu == App::Battle::Items) battleChooseItem(app, b.picks[0]);
                                    if (b.menu == App::Battle::Targets) battleChooseTarget(app, b.picks[0]);
                                    continue;
                                }
                                battleChooseCommand(app, 0);
                                if (b.menu == App::Battle::Targets) battleChooseTarget(app, b.picks.back());
                            }
                        }
                    }
                    if (b.phase == App::Battle::Results) { ++won; levelled += int(b.levelled.size()); ended = 1; endBattle(app, true); break; }
                    if (b.phase == App::Battle::Lost && b.phaseFrames > 5) { ++lost; ended = 1; endBattle(app, false); break; }
                    if (b.exit == 1) {
                        ++fled;
                        ended = 1;
                        if (SDL_getenv("CR_BATTLE_TRACE")) std::printf("  fled: style %d round %d\n", style, b.rules.round);
                        endBattle(app, false);
                        break;
                    }
                    stepBattle(app);
                    if (b.exit) continue; // counted above, before drawFrame would take it
                    if (frame % 40 == 0) {
                        app.lastTicks = 0;
                        drawFrame(app, false);
                        rerrs += app.renderer.takeErrors();
                    }
                }
                if (!ended) {
                    ++stuck;
                    std::printf("  battle %d on stage %d didn't end (phase %d, menu %d, round %d)\n", list[i].second,
                                list[i].first, int(b.phase), int(b.menu), b.rules.round);
                    if (b.active) endBattle(app, false);
                }
                app.runner.stop();
            }
            app.game = saved;
            std::printf("battle scenes: %d played - %d won (%d level-ups), %d lost, %d fled, %d didn't end; %d render API errors\n",
                        n, won, levelled, lost, fled, stuck, rerrs);
            problems += stuck + rerrs + (won == 0) + (fled == 0);
            glErrors += rerrs;
        }
        return problems;
    }
}

// The last drawn frame to a .bmp (self-test looks)
void saveFrame(App& app, const char* path) {
    int sw = 0, sh = 0;
    std::vector<uint8_t> px;
    if (!app.renderer.readPixels(px, sw, sh)) return;
    SDL_Surface* surf = SDL_CreateSurface(sw, sh, SDL_PIXELFORMAT_RGBA32);
    for (int y = 0; y < sh; ++y)
        std::memcpy(static_cast<uint8_t*>(surf->pixels) + size_t(y) * surf->pitch, &px[size_t(y) * sw * 4], size_t(sw) * 4);
    SDL_SaveBMP(surf, path);
    SDL_DestroySurface(surf);
}

int runSelfTest(App& app) {
    app.persist = false;
    app.typeOn = false; // pages whole (the typing has its own test)
    app.video.layout = 1; // the iPad layout the tests measure (the iPhone one has its own)
    applyVideo(app);
    app.testNoTutorials = true; // the battles' tutorials only where a test asks for them
    app.renderer.setVSync(false); // as fast as it renders, not at the display's rate
    if (SDL_getenv("CR_SELFTEST_BATTLES")) { // only the battle pass
        int frames = 0, glErrors = 0;
        setMode(app, Mode::Maps);
        return battleSelfTest(app, frames, glErrors) ? 1 : 0;
    }
    setMode(app, Mode::Models);
    int glErrors = 0, frames = 0, failedModels = 0;
    for (size_t i = 0; i < app.models.size(); ++i) {
        showModel(app, int(i));
        int errs = 0;
        const int steps = int(app.model.actions.size()) + 1;
        for (int a = 0; a < steps; ++a) {
            for (int f = 0; f < 3; ++f) {
                animateModel(app, 0.25f);
                drawFrame(app);
                ++frames;
            }
            errs += app.renderer.takeErrors();
            selectAction(app, 1);
        }
        if (errs) {
            ++failedModels;
            glErrors += errs;
            std::printf("GL errors in %s: %d\n", app.models[i].c_str(), errs);
        }
    }
    // Every field map (collision + props)
    setMode(app, Mode::Maps);
    int mapErrors = 0, camMaps = 0, spawned = 0, walked = 0;
    for (size_t i = 0; i < app.maps.size(); ++i) {
        showMap(app, int(i));
        camMaps += app.haveStageCam;
        // Walk the leader around for 4 seconds of game time (stick turning a full circle) and
        // check it stays finite, on a floor, and gets somewhere
        if (app.havePlayer) {
            ++spawned;
            const cr::Vec3 start = app.walker.pos;
            float far = 0;
            bool ok = true;
            for (int f = 0; f < 120; ++f) {
                const float a = float(f) / 120.0f * 6.2831853f;
                cr::stepWalker(app.walker, app.collisionData, std::sin(a), -std::cos(a), f % 40 < 20, 0.0f);
                const cr::Vec3 p = app.walker.pos;
                float fy;
                ok = ok && std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
                     cr::floorHeight(app.collisionData, p.x, p.y + 0.01f, p.z, 0.0f, fy) && std::fabs(fy - p.y) < 0.02f;
                const cr::Vec3 d = p - start;
                far = std::max(far, std::sqrt(cr::dot(d, d)));
            }
            if (!ok) {
                ++mapErrors;
                std::printf("player left the floor in %s\n", app.maps[i].c_str());
            }
            walked += far > 5.0f;
        }
        for (int f = 0; f < 2; ++f) { // 3D view, then the composite (background + stage camera)
            app.showBackground = f == 1;
            app.lastTicks = 0; // no game time passes here, however long the load took
            drawFrame(app);
            ++frames;
        }
        app.showBackground = false;
        int errs = 0;
        errs += app.renderer.takeErrors();
        if (errs || app.collisionPolys == 0) {
            ++mapErrors;
            std::printf("map problem in %s: %d GL errors, %d polygons\n", app.maps[i].c_str(), errs, app.collisionPolys);
        }
        glErrors += errs;
    }
    // Every exit of every map: stand in its box, fire it, check where the player arrives
    int exits = 0, arrived = 0, onFloor = 0, exitFailures = 0;
    for (int area0 = 0; area0 < cr::kNumAreas; ++area0)
    for (int map0 = 0; map0 < cr::kNumMaps; ++map0) {
        if (cr::areaForMap(map0, area0) != area0) continue;
        const auto at = std::find(app.maps.begin(), app.maps.end(), std::string(cr::kMapCollisionFile[map0]));
        if (at == app.maps.end()) continue;
        const int i = int(at - app.maps.begin());
        showMap(app, i, map0, area0);
        const int area = app.area, map = app.mapNumber;
        const std::vector<cr::FieldNpc> npcs = app.npcs;
        const std::vector<bool> warps = app.npcWarps;
        for (size_t k = 0; k < npcs.size(); ++k) {
            if (!warps[k] || !npcActive(app, npcs[k])) continue;
            ++exits;
            if (app.mapNumber != map || app.area != area) showMap(app, i, map, area);
            cr::GameState trial = app.game;
            const cr::EventResult r = cr::runEvent(app.events, npcs[k].id, trial, area, map);
            if (!app.havePlayer) { ++exitFailures; continue; }
            app.walker.pos = {npcs[k].boxCenterX(), npcs[k].pos[1], npcs[k].boxCenterZ()};
            app.lastKick = 0;
            const bool warped = checkTriggers(app, true);
            const int wantArea = r.warp.area >= 0 ? r.warp.area : area;
            if (!warped || app.mapNumber != r.warp.map || app.area != wantArea) {
                ++exitFailures;
                std::printf("exit problem: %s map %d event %d -> map %d: %s\n", cr::kAreaEventFile[area], map,
                            npcs[k].id, r.warp.map, warped ? "arrived elsewhere" : "did not warp");
                continue;
            }
            ++arrived;
            float fy;
            const cr::Vec3 p = app.walker.pos;
            if (cr::floorHeight(app.collisionData, p.x, p.y + 0.01f, p.z, 0.0f, fy) && std::fabs(fy - p.y) < 0.05f) ++onFloor;
            else std::printf("exit lands off the floor: %s map %d event %d -> map %d at (%.1f %.1f %.1f)\n",
                             cr::kAreaEventFile[area], map, npcs[k].id, app.mapNumber, p.x, p.y, p.z);
        }
    }
    std::printf("exits: %d fired, %d arrived on the right map, %d on a floor, %d problems\n", exits, arrived, onFloor,
                exitFailures);
    // Every visible NPC of every (area, map) should stand on a floor
    int npcsDrawn = 0, npcsOnFloor = 0, npcMaps = 0;
    std::map<std::string, int> offFloor;
    for (int area0 = 0; area0 < cr::kNumAreas; ++area0)
        for (int map0 = 0; map0 < cr::kNumMaps; ++map0) {
            if (cr::areaForMap(map0, area0) != area0) continue;
            const auto at = std::find(app.maps.begin(), app.maps.end(), std::string(cr::kMapCollisionFile[map0]));
            if (at == app.maps.end()) continue;
            showMap(app, int(at - app.maps.begin()), map0, area0);
            npcMaps += !app.npcDraws.empty();
            for (const auto& d : app.npcDraws) {
                ++npcsDrawn;
                const float x = d.world.m[12], y = d.world.m[13], z = d.world.m[14];
                float fy;
                if (cr::floorHeight(app.collisionData, x, y, z, 2.0f, fy) && std::fabs(fy - y) < 2.0f) ++npcsOnFloor;
                else ++offFloor[d.model];
            }
        }
    // Walk into every solid NPC from 12 units away: the player must never end up inside its box
    int solidTests = 0, entered = 0;
    for (int area0 = 0; area0 < cr::kNumAreas; ++area0)
        for (int map0 = 0; map0 < cr::kNumMaps; ++map0) {
            if (cr::areaForMap(map0, area0) != area0) continue;
            const auto at = std::find(app.maps.begin(), app.maps.end(), std::string(cr::kMapCollisionFile[map0]));
            if (at == app.maps.end()) continue;
            showMap(app, int(at - app.maps.begin()), map0, area0);
            if (!app.havePlayer) continue;
            const size_t base = app.collisionData.polygons.size();
            for (size_t w = base; w + 3 < app.walkCollision.polygons.size(); w += 4) {
                // the box's four corners are the bottom corners of its four walls
                float cx = 0, cz = 0, y0 = 0, reach = 0;
                for (int q = 0; q < 4; ++q) {
                    const float* v = &app.walkCollision.vertices[size_t(app.walkCollision.polygons[w + q].index[0]) * 3];
                    cx += v[0] / 4; cz += v[2] / 4; y0 = v[1] + 5;
                }
                for (int q = 0; q < 4; ++q) {
                    const float* v = &app.walkCollision.vertices[size_t(app.walkCollision.polygons[w + q].index[0]) * 3];
                    reach = std::max(reach, std::hypot(v[0] - cx, v[2] - cz));
                }
                reach += app.walker.radius + 2; // start outside the box
                for (int dir = 0; dir < 4; ++dir) {
                    const float a = dir * 1.5707963f;
                    cr::Vec3 start{cx + reach * std::sin(a), y0, cz + reach * std::cos(a)};
                    float fy;
                    if (!cr::floorHeight(app.walkCollision, start.x, start.y, start.z, 3.0f, fy)) continue;
                    app.walker.pos = {start.x, fy, start.z};
                    ++solidTests;
                    bool inside = false;
                    for (int f = 0; f < 120 && !inside; ++f) {
                        const cr::Vec3 d = cr::Vec3{cx, 0, cz} - cr::Vec3{app.walker.pos.x, 0, app.walker.pos.z};
                        cr::stepWalker(app.walker, app.walkCollision, d.x, -d.z, false, 3.14159265f / 2 * 0); // camYaw 0: stick x = world x, stick -y = world z
                        // inside = on the inner side of all four walls
                        int in = 0;
                        for (int q = 0; q < 4; ++q) {
                            const auto& pg = app.walkCollision.polygons[w + q];
                            const float* v = &app.walkCollision.vertices[size_t(pg.index[0]) * 3];
                            if ((app.walker.pos.x - v[0]) * pg.normal[0] + (app.walker.pos.z - v[2]) * pg.normal[2] < 0) ++in;
                        }
                        inside = in == 4;
                    }
                    if (inside && entered < 8)
                        std::printf("  inside: %s map %d box centre (%.1f %.1f) reach %.1f, from dir %d, now (%.1f %.1f)\n",
                                    cr::kAreaEventFile[area0], map0, cx, cz, reach, dir, app.walker.pos.x, app.walker.pos.z);
                    entered += inside;
                }
            }
        }
    std::printf("solid npcs: %d walk-ins, %d ended inside a box\n", solidTests, entered);
    // Examine everything examinable on every (area, map)
    int examined = 0, gotItems = 0, lidsHidden = 0, examineWarps = 0, examineErrors = 0;
    std::map<std::string, int> examinedByModel;
    for (int area0 = 0; area0 < cr::kNumAreas; ++area0)
        for (int map0 = 0; map0 < cr::kNumMaps; ++map0) {
            if (cr::areaForMap(map0, area0) != area0) continue;
            const auto at = std::find(app.maps.begin(), app.maps.end(), std::string(cr::kMapCollisionFile[map0]));
            if (at == app.maps.end()) continue;
            showMap(app, int(at - app.maps.begin()), map0, area0);
            std::vector<cr::FieldNpc> targets;
            for (const cr::FieldNpc& n : app.npcs)
                if ((n.kind & 8) && npcActive(app, n) && n.pos[1] < 900.0f &&
                    !(n.pos[0] == 0 && n.pos[1] == 0 && n.pos[2] == 0))
                    targets.push_back(n);
            for (const cr::FieldNpc& n : targets) {
                if (app.mapNumber != map0 || app.area != area0) showMap(app, int(at - app.maps.begin()), map0, area0);
                if (!app.havePlayer) break;
                const float k = n.model <= 1 ? 1.0f : 0.0f;
                const float cx = n.pos[0] + k * n.offset[0] + n.offset[0], cz = n.pos[2] + k * n.offset[2] + n.offset[2];
                app.walker.pos = {cx + n.box[0] / 2 + app.walker.radius, n.pos[1], cz}; // just beside it
                const cr::FieldNpc* t = interactTarget(app);
                if (!t) continue;
                const size_t drawsBefore = app.npcDraws.size();
                ++examined;
                ++examinedByModel[n.model >= 0 ? cr::kNpcModelFile[n.model] : "none"];
                const bool warped = interact(app, true);
                if (app.mapNumber != map0) { ++examineWarps; continue; }
                const bool gave = app.lastEvent.find(", got ") != std::string::npos;
                gotItems += gave;
                lidsHidden += (n.treasure & 3) && app.npcDraws.size() < drawsBefore;
                if ((n.treasure & 3) && !gave)
                    std::printf("  box without item: %s map %d box %d (lid script %d)\n", cr::kAreaEventFile[area0], map0, n.id, n.id + 1);
                (void)warped;
            }
        }
    std::printf("examine: %d examined, %d gave items (%d different, item 0 x%d), %d boxes lost their lid, %d warped; "
                "by model:", examined, gotItems, int(app.game.items.size()),
                app.game.items.count(0) ? app.game.items[0] : 0, lidsHidden, examineWarps);
    for (const auto& [m, c] : examinedByModel) std::printf(" %s x%d", m.c_str(), c);
    std::printf("\n");
    (void)examineErrors;
    mapErrors += entered;
    std::printf("npcs: %d drawn on %d maps, %d stand on a floor; off the floor by model:", npcsDrawn, npcMaps, npcsOnFloor);
    for (const auto& [m, c] : offFloor) std::printf(" %s x%d", m.c_str(), c);
    std::printf("\n");
    mapErrors += exitFailures;
    { // a gold box (j001_d01's boxes 60..98, lid scripts 61..99 - unused in every release): Uty_ChkItem
      // counts key 2 into var 0 - none, a message; with one, a Yes opens it and DelItem takes the key
        int gotWithout = -1, gotWith = -1, keysLeft = -1, boxId = -1, boxMap = -1, item = -1;
        for (int m = 0; m < cr::kNumMaps && boxId < 0; ++m) {
            if (cr::areaForMap(m, 2) != 2) continue;
            const auto at = std::find(app.maps.begin(), app.maps.end(), std::string(cr::kMapCollisionFile[m]));
            if (at == app.maps.end()) continue;
            app.game = cr::GameState();
            showMap(app, int(at - app.maps.begin()), m, 2);
            app.pendingEntry = App::Entry::None;
            app.runner.stop();
            // (no release shows them: TreasureBoxManager::setup clears their flags, 3180.., and picks
            // only from 100..179 - shown here by hand)
            for (int f = 3180; f < 3200; ++f) app.game.setFlag(f, true);
            for (const cr::FieldNpc& n : app.npcs)
                if ((n.treasure & 3) && n.id >= 60 && n.id <= 98 && n.id % 2 == 0 && npcActive(app, n) && app.havePlayer) {
                    boxId = n.id, boxMap = m;
                    const cr::FieldNpc b = n;
                    for (int withKey = 0; withKey < 2; ++withKey) {
                        if (item > 0) app.game.items.erase(item);
                        if (withKey) app.game.items[2] = 1;
                        app.walker.pos = {b.pos[0] + b.box[0] / 2 + app.walker.radius, b.pos[1], b.pos[2]};
                        app.runner.stop();
                        interact(app);
                        item = app.game.vars[10];
                        for (int f = 0; f < 900 && (app.runner.running() || app.itemWindowOpen); ++f) {
                            stepEvents(app);
                            if (app.choiceFlag >= 0) answerChoice(app, true);
                            if (app.itemWindowOpen) app.tapPending = true;
                            if (!app.dialogue.empty()) app.dialogue.clear(), app.tapPending = true;
                        }
                        (withKey ? gotWith : gotWithout) = item > 0 && app.game.items.count(item) ? app.game.items[item] : 0;
                    }
                    keysLeft = app.game.items.count(2) ? app.game.items[2] : 0;
                    app.runner.stop();
                    app.dialogue.clear();
                    break;
                }
        }
        const bool ok = boxId >= 0 && gotWithout == 0 && gotWith >= 1 && keysLeft == 0;
        std::printf("gold box %d (map %d): item %d - without a key %d, with one %d (keys left %d) %s\n", boxId, boxMap, item,
                    gotWithout, gotWith, keysLeft, ok ? "ok" : "WRONG");
        mapErrors += !ok;
    }
    // Stepped scripts: open a chest frame by frame, then walk into an exit
    {
        app.game = cr::GameState(); // fresh: the examine pass above opened every box
        app.treasureBoxes.clear();
        const auto at = std::find(app.maps.begin(), app.maps.end(), std::string("d0101_10.cmp"));
        showMap(app, int(at - app.maps.begin()), 0, cr::areaForMap(0, 2));
        app.pendingEntry = App::Entry::None; // these tests play one script each
        int lidAnimFrame = -1, windowFrame = -1, fadedFrame = -1, endFrame = -1, frames = 0;
        const cr::FieldNpc* box = nullptr;
        for (const cr::FieldNpc& n : app.npcs)
            if ((n.treasure & 3) && npcActive(app, n)) { box = &n; break; }
        if (box && app.havePlayer) {
            const int boxId = box->id;
            app.walker.pos = {box->pos[0] + box->box[0] / 2 + app.walker.radius, box->pos[1], box->pos[2]};
            interact(app);
            app.soundLog.clear();
            const size_t itemsBefore = app.game.items.size();
            for (frames = 0; frames < 900 && (app.runner.running() || app.itemWindowOpen); ++frames) {
                stepEvents(app);
                for (const auto& d : app.npcDraws)
                    if (d.id == boxId + 1) {
                        if (lidAnimFrame < 0 && d.action == 1) lidAnimFrame = frames;
                        if (fadedFrame < 0 && d.alpha < 0.1f) fadedFrame = frames;
                    }
                if (windowFrame < 0 && app.itemWindowOpen) windowFrame = frames;
                if (app.itemWindowOpen && frames - windowFrame == 30) app.tapPending = true; // read it, then tap
                if (!app.dialogue.empty()) app.dialogue.clear();
                if (!app.runner.running() && endFrame < 0) endFrame = frames;
            }
            const bool lidGone = std::none_of(app.npcDraws.begin(), app.npcDraws.end(),
                                              [&](const App::NpcDraw& d) { return d.id == boxId + 1; });
            std::printf("stepped chest sounds:");
            for (uint32_t id : app.soundLog) std::printf(" %u", id);
            std::printf("\n");
            std::printf("stepped chest %d: lid anim at frame %d, item window %d, lid faded %d, done %d, item %s, lid %s\n",
                        boxId, lidAnimFrame, windowFrame, fadedFrame, endFrame,
                        app.game.items.size() > itemsBefore || !app.game.items.empty() ? "gained" : "MISSING",
                        lidGone ? "removed" : "STILL THERE");
            if (lidAnimFrame < 0 || windowFrame < 0 || endFrame < 0 || !lidGone) ++mapErrors;
        }
        // exit: walk into trigger 2's box and let the script play
        for (const cr::FieldNpc& n : app.npcs)
            if (n.id == 2 && n.touchTrigger()) {
                app.walker.pos = {n.boxCenterX(), n.pos[1], n.boxCenterZ()};
                app.lastKick = 0;
                app.soundLog.clear();
                checkTriggers(app);
                int warpFrame = -1, maxFade = 0;
                for (frames = 0; frames < 600 && app.runner.running(); ++frames) {
                    stepEvents(app);
                    if (warpFrame < 0 && app.mapNumber != 0) warpFrame = frames;
                    maxFade = std::max(maxFade, int(screenFadeAlpha(app) * 100));
                }
                std::printf("stepped exit sounds:");
                for (uint32_t id : app.soundLog) std::printf(" %#x", id);
                std::printf("\n");
                std::printf("stepped exit: warped to map %d at frame %d, script done at frame %d, peak fade %d%%, final fade %d%%\n",
                            app.mapNumber, warpFrame, frames, maxFade, int(screenFadeAlpha(app) * 100));
                if (warpFrame < 0 || screenFadeAlpha(app) > 0.01f) ++mapErrors;
                break;
            }
    }
    // Entry scripts on a new game: each area's script 0 on its first room, then every room's
    // EvrStart 5, played frame by frame to the end (dialogue paged, taps given)
    {
        int areasRun = 0, roomsRun = 0, stuck = 0, errors = 0, pages = 0, warps = 0, longest = 0, choices = 0;
        auto play = [&](const char* what, int area, int map) {
            const int startMap = app.mapNumber;
            int f = 0;
            app.tapPending = false;
            for (; f < 20000 && (app.runner.running() || app.pendingEntry != App::Entry::None); ++f) {
                stepEvents(app);
                if (!app.dialogue.empty()) { pages += int(app.dialogue.size()); app.dialogue.clear(); }
                if (app.choiceFlag >= 0) { ++choices; answerChoice(app, true); }
                if (app.worldChoiceOpen) { ++choices; answerWorldChoice(app, 0); }
                app.tapPending = true;
                if (app.itemWindowOpen) app.game.setFlag(0, true);
            }
            longest = std::max(longest, f);
            if (app.mapNumber != startMap) ++warps;
            if (!app.runner.result().error.empty()) {
                ++errors;
                std::printf("  %s area %d map %d: %s\n", what, area, map, app.runner.result().error.c_str());
            }
            if (app.runner.running()) {
                ++stuck;
                std::printf("  %s area %d map %d: still running after %d frames (%s, event %d)\n", what, area, map, f,
                            app.runner.where().c_str(), app.runner.eventNo());
                app.runner.stop();
            }
            return f;
        };
        for (int area = 0; area < cr::kNumAreas; ++area) {
            const int map = cr::kAreaFirstMap[area];
            const auto it = std::find(app.maps.begin(), app.maps.end(), std::string(cr::kMapCollisionFile[map]));
            if (it == app.maps.end()) continue;
            app.game = cr::GameState();
            app.entryArea = -1;
            showMap(app, int(it - app.maps.begin()), map, area);
            const int f = play("area script", area, map);
            ++areasRun;
            std::printf("  area %2d %s: script 0 on map %d took %d frames\n", area, cr::kAreaEventFile[area], map, f);
        }
        for (int area = 0; area < cr::kNumAreas; ++area)
            for (int map = 0; map < cr::kNumMaps; ++map) {
                if (cr::areaForMap(map, area) != area) continue;
                const auto it = std::find(app.maps.begin(), app.maps.end(), std::string(cr::kMapCollisionFile[map]));
                if (it == app.maps.end()) continue;
                app.game = cr::GameState();
                app.entryArea = area; // arriving from another room of the area
                showMap(app, int(it - app.maps.begin()), map, area);
                play("room script", area, map);
                ++roomsRun;
            }
        std::printf("entry scripts: %d areas, %d rooms; %d dialogue pages, %d choices, %d warped, %d errors, %d stuck, "
                    "longest %d frames\n", areasRun, roomsRun, pages, choices, warps, errors, stuck, longest);
        mapErrors += errors + stuck;
    }
    // New game: talk to everyone in the Ark's hall, frame by frame (pages turned, choices answered yes)
    {
        startNewGame(app);
        int openingFrames = 0, openingPages = 0;
        for (; openingFrames < 20000 && (app.runner.running() || app.pendingEntry != App::Entry::None); ++openingFrames) {
            stepEvents(app);
            if (!app.dialogue.empty()) {
                openingPages += int(app.dialogue.size());
                for (const auto& v : app.dialogueVoices) app.voicesPlayed += int(v.size());
                app.dialogue.clear();
            }
            if (app.choiceFlag >= 0) answerChoice(app, true);
            if (app.worldChoiceOpen) answerWorldChoice(app, 0);
            app.tapPending = true;
        }
        std::printf("new game: the opening took %d frames, %d pages, %d voice lines, ends on map %d%s\n", openingFrames,
                    openingPages, app.voicesPlayed, app.mapNumber, app.runner.running() ? " (STILL RUNNING)" : "");
        const int voicesBefore = app.voicesPlayed;
        if (app.runner.running() || app.mapNumber != 97) ++mapErrors;
        int talked = 0, pages = 0, choices = 0, maxPortraits = 0, plates = 0, unfinished = 0;
        std::vector<int> ids;
        for (const cr::FieldNpc& n : app.npcs)
            if ((n.kind & 8) && npcActive(app, n) && n.pos[1] < 900.0f) ids.push_back(n.id);
        for (int id : ids) {
            const cr::FieldNpc* n = findNpc(app, id);
            if (!n || !app.havePlayer) continue;
            app.walker.pos = {n->pos[0] + n->box[0] / 2 + app.walker.radius, n->pos[1], n->pos[2]};
            if (!interact(app)) continue;
            ++talked;
            int f = 0;
            for (; f < 6000 && (app.runner.running() || app.pendingEntry != App::Entry::None); ++f) {
                stepEvents(app);
                int shown = 0;
                for (const App::Portrait& p : app.portraits) shown += p.tex.id && p.alpha > 0.5f;
                maxPortraits = std::max(maxPortraits, shown);
                if (!app.dialogue.empty()) {
                    for (const auto& mi : app.dialogueInfo) plates += mi.nameId >= 0;
                    pages += int(app.dialogue.size());
                    for (const auto& v : app.dialogueVoices) app.voicesPlayed += int(v.size());
                    app.dialogue.clear();
                }
                if (app.choiceFlag >= 0) { ++choices; answerChoice(app, true); }
                if (app.worldChoiceOpen) { ++choices; answerWorldChoice(app, 0); }
                app.tapPending = true;
            }
            if (app.runner.running()) {
                ++unfinished;
                std::printf("  talk to %d: still running (%s)\n", id, app.runner.where().c_str());
                app.runner.stop();
            }
        }
        std::printf("new game: talked to %d of %zu in the hall; %d pages (%d with a name plate), %d voice lines, %d choices, "
                    "up to %d portraits at once, %d unfinished\n", talked, ids.size(), pages, plates,
                    app.voicesPlayed - voicesBefore, choices, maxPortraits, unfinished);
        mapErrors += unfinished;
    }
    mapErrors += battleSelfTest(app, frames, glErrors);
    // The menus' sounds since the last call against the original's __effgo ids (2 choose, 3 back, 4 buzzer /
    // keyboard cursor, 5 page, 11 saved, 12 loaded, 22 paid, 40 SE gauge): 1 if they differ
    auto expectSounds = [&app](const char* what, std::initializer_list<uint32_t> want) {
        const bool same = std::equal(app.menuSoundLog.begin(), app.menuSoundLog.end(), want.begin(), want.end());
        if (!same) {
            std::printf("menu sounds, %s:", what);
            for (uint32_t id : app.menuSoundLog) std::printf(" %u", id);
            std::printf(" (want");
            for (uint32_t id : want) std::printf(" %u", id);
            std::printf(")\n");
        }
        app.menuSoundLog.clear();
        return same ? 0 : 1;
    };
    { // The Options screen: every page builds and draws; keys change what they should
        const App::Mix mix = app.mix;
        const App::Video video = app.video;
        const int language = app.language;
        int rows = 0, problems = 0;
        for (int page = 0; page < kOptionPages; ++page) {
            openOptions(app, page);
            rows += int(optionRows(app, page).size());
            app.lastTicks = 0;
            drawFrame(app, false);
            ++frames;
        }
        app.menuSoundLog.clear();
        openOptions(app, 1);         // Sound
        optionsKey(app, SDLK_DOWN);  // BGM
        const int music = app.mix.music;
        optionsKey(app, SDLK_LEFT);
        problems += app.mix.music != std::max(0, music - 5);
        optionsKey(app, SDLK_DOWN);  // Sound Effects
        const int effects = app.mix.effects;
        optionsKey(app, SDLK_LEFT);
        problems += app.mix.effects != std::max(0, effects - 5);
        problems += expectSounds("options gauges", {4, 4, 40}); // the BGM's is silent
        openOptions(app, 0);         // Game: Language
        optionsKey(app, SDLK_LEFT);  // Japanese
        problems += app.language != 0;
        optionsKey(app, SDLK_RIGHT); // English
        problems += app.language != 1;
        optionsKey(app, SDLK_Q);     // wraps to the last page (Graphics)
        problems += app.optionsPage != kOptionPages - 1;
        optionsKey(app, SDLK_UP);    // from row 0 to Back
        problems += app.optionsRow != kOptionsBackRow;
        problems += expectSounds("options buttons, page", {2, 2, 3, 4});
        // Graphics: Anti-aliasing steps through MSAA to FSR Native AA and DLAA (the upscaler at
        // native size), the Upscaler row on to its other modes
        if (app.renderer.scalable()) { // (OpenGL: the row is off)
            app.optionsRow = 1;
            int aaSteps = 0;
            while (app.video.upscaler != 1 && aaSteps++ < 6) optionsKey(app, SDLK_RIGHT);
            problems += app.video.upscaler != 1 || app.video.upscaleMode != 0;
            optionsKey(app, SDLK_RIGHT);
            problems += app.video.upscaler != 2 || app.video.upscaleMode != 0;
            optionsKey(app, SDLK_RIGHT); // wraps to Off
            problems += app.video.upscaler != 0 || app.video.msaa != 1;
        }
        // FSR 3.1 where it runs (Vulkan): frames come out the frame's size, not black, error free
        int upscaled = 0;
        if (app.renderer.upscalerProblem(cr::Upscaler::Fsr3).empty()) {
            for (int mode : {0, 1, 3}) {
                app.video.upscaler = 1;
                app.video.upscaleMode = mode;
                applyVideo(app);
                app.optionsOpen = false;
                for (int f = 0; f < 8; ++f) {
                    app.lastTicks = 0;
                    drawFrame(app, false);
                    ++frames;
                }
                std::vector<uint8_t> px;
                int w = 0, h = 0;
                uint64_t sum = 0;
                if (app.renderer.readPixels(px, w, h))
                    for (size_t i = 0; i < px.size(); i += 4) sum += px[i] + px[i + 1] + px[i + 2];
                const int errs = app.renderer.takeErrors();
                if (!sum || errs || w != app.renderer.frameWidth()) {
                    std::printf("FSR 3.1 %s: %d errors, %dx%d, %s\n", kUpscaleModes[mode].name, errs, w, h, sum ? "drawn" : "black");
                    ++problems;
                }
                ++upscaled;
            }
        }
        { // Smooth Models + HD textures: battles load and draw with them, HD textures keep the
          // original's layout size
            app.video.smoothModels = 3;
            app.video.hdTextures = true;
            const int hdBefore = app.hdLoaded;
            int drawn = 0;
            for (int g : {1, 5, 9}) {
                startBattle(app, {g, g % 9, -1, 0}, 0);
                for (int f = 0; f < 4; ++f) {
                    app.lastTicks = 0;
                    drawFrame(app, false);
                    ++frames;
                }
                drawn += app.battle.active;
                const int errs = app.renderer.takeErrors();
                if (errs) std::printf("smooth/hd battle %d: %d render API errors\n", g, errs);
                problems += errs;
                endBattle(app, true);
            }
            cr::Image wi;
            if (loadImage(app, "window_hd.mct", wi) && wi.logicalW) {
                const cr::Texture t = app.renderer.upload(wi);
                problems += t.width != wi.logicalW || t.height != wi.logicalH;
                cr::Texture tt = t;
                app.renderer.destroy(tt);
            }
            std::printf("smooth models + hd textures: %d battles drawn, %d HD textures used (%s)\n", drawn,
                        app.hdLoaded - hdBefore, app.hdDir.c_str());
            problems += drawn != 3;
            app.video.smoothModels = 0;
            app.video.hdTextures = false;
        }
        openOptions(app, 2);         // Display
        app.video.widescreen = true; // "Original Presentation" restores the defaults (upscaler off)
        app.video.upscaler = 1;
        app.optionsRow = int(optionRows(app, 2).size()) - 1;
        app.menuSoundLog.clear();
        optionsKey(app, SDLK_RETURN);
        problems += app.video.widescreen || app.video.width != 1024 || app.video.upscaler != 0;
        optionsKey(app, SDLK_ESCAPE);
        problems += app.optionsOpen;
        problems += expectSounds("options Enter, Back", {2, 3});
        // The title: the boot script reaches the title menu; its Options are title mode (Reset
        // Data); Start begins a new game; in a game, Return to Title boots again
        bootTitle(app);
        int bootFrames = 0;
        for (; bootFrames < 1000 && !app.titleOpen; ++bootFrames) stepEvents(app);
        problems += !app.titleOpen;
        openOptions(app, 0);
        const std::vector<OptRow> titlePage = optionRows(app, 0);
        problems += titlePage.size() != 8 || titlePage.back().label != sysLabel(app, 58122, "Reset Data");
        optionsKey(app, SDLK_ESCAPE);
        problems += !app.titleOpen; // back on the title menu
        app.menuSoundLog.clear();
        int openingMap = -1;
        {
            const std::string keepDir = app.saveDirOverride; // (no save there: New alone)
            app.saveDirOverride = userDir() + "selftest-empty/";
            titleChoose(app, 0);    // Start: the boot script goes on to Character Select (Util 4)
            for (int f = 0; f < 300 && !app.charSelectOpen; ++f) stepEvents(app);
            problems += !app.charSelectOpen;
            app.charSelectCursor = 0, app.charSelectPage = 0; // Escher & Musiea
            charSelectKey(app, SDLK_RETURN); // Escher & Musiea -> the pair's save / load screen (Util 5)
            for (int f = 0; f < 300 && !app.saveScreen.open; ++f) stepEvents(app);
            problems += !app.saveScreen.open || app.saveScreen.haveSave || !app.saveScreen.load;
            saveScreenKey(app, SDLK_RETURN); // New -> "Begin new game?"
            problems += app.saveScreen.message != 57057;
            saveScreenKey(app, SDLK_RETURN); // Yes: the script plays the pair's opening
            problems += expectSounds("title Start, pair, New, Yes", {2, 2, 2, 12});
            for (int f = 0; f < 600 && app.booting; ++f) stepEvents(app);
            openingMap = app.mapNumber;
            app.saveDirOverride = keepDir;
        }
        problems += app.booting || openingMap != 88 || app.playerChar != 0; // the opening (d0550); 82 would be the last battle
        if (problems) std::printf("title flow: map %d, leader %d, booting %d\n", openingMap, app.playerChar, int(app.booting));
        app.runner.stop();
        startNewGame(app); // (the hall, for the in-game Options checks below)
        openOptions(app, 0);
        std::vector<OptRow> gamePage = optionRows(app, 0);
        problems += gamePage.size() != 8 || gamePage.back().label != sysLabel(app, 58123, "Return to Title") || !gamePage.back().enabled;
        if (gamePage.size() == 8 && gamePage.back().act) gamePage.back().act(0);
        problems += !app.booting;
        app.runner.stop();
        std::printf("options: %d pages, %d rows; FSR 3.1 modes drawn: %d; title menu after %d frames; %d problems\n",
                    kOptionPages, rows, upscaled, bootFrames, problems);
        mapErrors += problems;
        app.mix = mix; // put the settings back as they were
        app.video = video;
        app.language = language;
        applyLanguage(app);
        applyMix(app);
        applyVideo(app);
    }
    { // Saves: a field's state written, changed, read back and restored; the title's Start shows it
        int problems = 0;
        const std::string keepDir = app.saveDirOverride;
        app.saveDirOverride = userDir() + "selftest-saves/";
        SDL_CreateDirectory(app.saveDirOverride.c_str());
        startNewGame(app);
        for (int f = 0; f < 3000 && (app.runner.running() || app.pendingEntry != App::Entry::None); ++f) {
            stepEvents(app);
            app.dialogue.clear();
            if (app.choiceFlag >= 0) answerChoice(app, true);
            app.tapPending = true;
        }
        app.game.setFlag(3333, true);
        app.game.items[7] = 3;
        app.game.vars[100] = -42;
        app.game.oz = 777;
        app.game.party[1].level = 5;
        app.game.party[1].exp = 600;
        app.game.party[0].equip[cr::kJewel] = 426;
        app.game.party[0].seeds[2] = 7;
        app.game.party[0].genes[2] = 485;
        app.playSeconds = 1234.5;
        app.walker.pos.x += 3;
        const cr::SaveData before = captureSave(app);
        app.menuSoundLog.clear();
        openSaveScreen(app, false);
        saveScreenKey(app, SDLK_RETURN); // Yes
        problems += app.saveScreen.message != 57082;
        problems += expectSounds("save", {11});
        app.saveScreen.open = false;
        app.game = cr::GameState();
        app.playSeconds = 0;
        bootTitle(app);
        for (int f = 0; f < 1000 && !app.titleOpen; ++f) stepEvents(app);
        titleChoose(app, 0); // Start -> Character Select -> the pair's Load / New
        for (int f = 0; f < 300 && !app.charSelectOpen; ++f) stepEvents(app);
        app.charSelectCursor = 0, app.charSelectPage = 0; // Escher & Musiea
        charSelectKey(app, SDLK_RETURN);
        for (int f = 0; f < 300 && !app.saveScreen.open; ++f) stepEvents(app);
        problems += !app.saveScreen.open || !app.saveScreen.haveSave;
        saveScreenKey(app, SDLK_RETURN); // Load
        problems += app.saveScreen.message != 57013;
        saveScreenKey(app, SDLK_RETURN); // Yes
        problems += expectSounds("title Start, pair, Load, Yes", {2, 2, 2, 12});
        const bool loaded = app.saveScreen.message == 57083;
        problems += !loaded;
        const cr::SaveData after = captureSave(app);
        problems += after.map != before.map || after.area != before.area || !app.game.flag(3333) ||
                    app.game.items[7] != 3 || app.game.vars[100] != -42 || app.game.oz != 777 ||
                    app.game.party[1].level != 5 || app.game.party[1].exp != 600 || app.game.party[0].equip[cr::kJewel] != 426 || app.game.party[0].seeds[2] != 7 || app.game.party[0].genes[2] != 485 ||
                    app.game.party[2].equip[cr::kWeapon] != 257 ||
                    std::fabs(app.playSeconds - 1234.5) > 1e-6 ||
                    std::fabs(after.pos[0] - before.pos[0]) > 1e-3f || app.titleOpen || app.booting;
        for (int f = 0; f < 60; ++f) stepSaveScreen(app);
        problems += app.saveScreen.open;
        for (int f = 0; f < 3; ++f) {
            app.lastTicks = 0;
            drawFrame(app, false);
            ++frames;
        }
        // The Progress Tracker
        app.persist = true;
        writeResume(app);
        app.persist = false;
        cr::SaveData resume;
        problems += !cr::readSave(savePath(app, "resume.dat"), resume) || resume.map != before.map;
        // ... offered at the title, and used up by the answer
        app.persist = true;
        bootTitle(app);
        for (int f = 0; f < 1000 && !app.titleOpen; ++f) stepEvents(app);
        const bool offered = app.resumePrompt == 1;
        problems += !offered;
        app.menuSoundLog.clear();
        answerResume(app, true);
        problems += expectSounds("resume", {2});
        problems += app.titleOpen || app.mapNumber != before.map || SDL_GetPathInfo(savePath(app, "resume.dat").c_str(), nullptr);
        // ... and saved as you go (the Vita's): a scene's end after the load arms it, the next writes it
        trackerSceneEnd(app);
        const bool armedOnly = !SDL_GetPathInfo(savePath(app, "resume.dat").c_str(), nullptr);
        trackerSceneEnd(app);
        const bool asYouGo = armedOnly && cr::readSave(savePath(app, "resume.dat"), resume) && resume.map == before.map;
        problems += !asYouGo;
        SDL_RemovePath(savePath(app, "resume.dat").c_str());
        app.persist = false;
        // The title's New (pair 1, no save): New alone, at 232; Yes hands var 1 = 2 to the boot
        // script, which goes to the opening (map 88) - var 1 = 1 would be the last battle (map 82)
        for (const char* f : {"save1.dat", "resume.dat"}) SDL_RemovePath(savePath(app, f).c_str());
        app.game = cr::GameState();
        bootTitle(app);
        for (int f = 0; f < 1000 && !app.titleOpen; ++f) stepEvents(app);
        titleChoose(app, 0);
        for (int f = 0; f < 300 && !app.charSelectOpen; ++f) stepEvents(app);
        app.charSelectCursor = 0;
        charSelectKey(app, SDLK_RETURN);
        for (int f = 0; f < 300 && !app.saveScreen.open; ++f) stepEvents(app);
        problems += !app.saveScreen.open || app.saveScreen.haveSave || savePlates(app) != std::vector<int>{2} ||
                    app.saveScreen.emblems != 0;
        saveScreenKey(app, SDLK_RETURN); // New
        problems += app.saveScreen.message != 57057 || app.saveScreen.kind != 2;
        saveScreenKey(app, SDLK_RETURN); // Yes
        const int var1 = app.game.vars[1];
        int openingMap = -1;
        for (int f = 0; f < 1500 && openingMap < 0; ++f) {
            stepEvents(app);
            if (!app.booting && app.mapNumber >= 0) openingMap = app.mapNumber;
        }
        problems += var1 != 2 || openingMap != 88;
        app.runner.stop();
        // Every story cleared: the emblems, the Final plate, New asking about the clear data first
        {
            cr::SaveData sd = before;
            for (int f : {31, 32, 34, 36, 38, 39, 40}) sd.game.setFlag(f, true);
            sd.leader = 0;
            cr::writeSave(slotPath(app, 1), sd);
        }
        app.game = cr::GameState();
        app.chosenPair = 1;
        openSaveScreen(app, true);
        App::SaveScreen& ssc = app.saveScreen;
        problems += ssc.emblems != 7 || !ssc.allClear || !ssc.cleared || savePlates(app) != std::vector<int>{0, 1, 2};
        saveScreenChoose(app, 2); // New: with the clear data?
        problems += ssc.kind != 3 || ssc.message != 57086 || ssc.lines[0] != 57087 || ssc.lines[1] != 57088;
        saveScreenKey(app, SDLK_ESCAPE); // No: a plain new game, then?
        problems += ssc.phase != 1 || ssc.kind != 2 || ssc.message != 57089 || ssc.lines[1] != 57091;
        saveScreenKey(app, SDLK_ESCAPE); // No again: back to the plates
        problems += ssc.phase != 0;
        saveScreenChoose(app, 0); // the last battle
        problems += ssc.kind != 1 || ssc.message != 57056;
        // Uty_LoadPrty with the clear data: mode 3 keeps the bonuses, gear, plates and bag (less the
        // story's kind 3 / 9 items) and Oz; mode 2 keeps Oz only; either clears the pair's flags
        int storyItem = 0;
        for (int it = 1; it < 500 && !storyItem; ++it)
            if (const cr::ItemParam* ip = app.itemTable.item(it); ip && (ip->kind == 3 || ip->kind == 9)) storyItem = it;
        {
            cr::SaveData sd = before;
            for (int f : {31, 32, 34, 36, 38, 39, 40, 1100, 2000, 3000, 3020, 3004, 3310}) sd.game.setFlag(f, true);
            sd.game.items[storyItem] = 1;
            cr::writeSave(slotPath(app, 1), sd);
        }
        ViewerHost host(app);
        app.game = cr::GameState();
        host.loadParty(1, true);
        const cr::PartyMember& e0 = app.game.party[0];
        int carry = (e0.level == 1) + (e0.seeds[2] == 7) + (e0.genes[2] == 485) + (e0.equip[cr::kJewel] == 426) +
                    (app.game.party[1].level == 1) + (app.game.items[7] == 3) + !app.game.items.count(storyItem) +
                    (app.game.oz == 777) + !app.game.flag(1100) + app.game.flag(2000) + (app.playerChar == 0);
        // ResetFieldFlag_NewGame: pair 1's blocks 0 and 5 (3000, 3020) and 3310 go, block 1 (pair 2's) stays
        problems += (carry != 11) + (storyItem == 0) + app.game.flag(3000) + app.game.flag(3020) + app.game.flag(3310) +
                    !app.game.flag(3004);
        app.game = cr::GameState();
        host.loadParty(1, false);
        problems += app.game.party[0].seeds[2] != 0 || app.game.items.count(7) || app.game.items[1] != 3 ||
                    app.game.oz != 777 || app.game.flag(1100) || !app.game.flag(2000);
        // a pair without clear data: a fresh start (pair 2: Eluca's leader 2)
        app.game = cr::GameState();
        host.loadParty(2, false);
        problems += app.game.oz != 0 || app.game.items[21] != 2 || app.playerChar != 2 || app.game.party[2].level != 1;
        std::printf("new game with clear data: mode 3 carried %d of 11, mode 2 Oz only, a fresh pair fresh\n", carry);
        app.game = cr::GameState();
        app.playerChar = 0;
        for (int f = 0; f < 2; ++f) {
            app.lastTicks = 0;
            drawFrame(app, false);
            ++frames;
        }
        problems += app.renderer.takeErrors();
        std::printf("save screen: the title's New -> var 1 = %d, opening map %d; cleared: emblems %u, %zu plates; %d problems so far\n",
                    var1, openingMap, ssc.emblems, savePlates(app).size(), problems);
        ssc.open = false;
        app.menuSoundLog.clear();
        for (const char* f : {"save1.dat", "resume.dat"}) SDL_RemovePath(savePath(app, f).c_str());
        SDL_RemovePath(app.saveDirOverride.c_str());
        app.saveDirOverride = keepDir;
        // The Instruction Manual (Options) and Scenarios (the title)
        openManual(app);
        const int topics = int(manualTopics(app).size());
        manualKey(app, SDLK_DOWN);
        manualKey(app, SDLK_DOWN);
        problems += topics < 25 || app.manualCursor != 1 || manualPage(app, manualTopics(app)[1]).empty();
        manualKey(app, SDLK_PAGEDOWN);
        manualKey(app, SDLK_ESCAPE);
        problems += app.manualOpen;
        problems += expectSounds("manual", {4, 4, 5, 3});
        cr::GameState chart;
        chart.setFlag(1092, true);
        chart.setFlag(1093, true);
        problems += scenarioState(chart, 0) != 3 || scenarioState(chart, 1) != 1 || scenarioState(chart, 2) != 0 ||
                    scenarioState(chart, 40) != 0 || scenarioPercent(chart) != 3;
        openScenarios(app);
        problems += !app.scenariosOpen || !app.scenariosSheet.id;
        app.lastTicks = 0;
        drawFrame(app, false);
        scenariosKey(app, SDLK_DOWN);
        problems += app.scenariosScroll >= 0;
        scenariosKey(app, SDLK_ESCAPE);
        problems += app.scenariosOpen;
        problems += expectSounds("scenarios", {3}); // scrolling is silent
        std::printf("manual: %d topics; scenarios chart drawn\n", topics);
        std::printf("saves: save -> title Start -> load %s (map %d, flags, items, vars, Oz, party, play time, position); "
                    "resume point written, offered at the title and resumed, saved as you go %s; %d problems\n", loaded ? "ok" : "FAILED",
                    after.map, asYouGo ? "yes" : "NO", problems);
        mapErrors += problems;
        bootTitle(app);
        app.runner.stop();
    }
    { // Shops: the item table, stock and level rules, then the Ark's shop (area 7) opened by its
      // shopkeeper's script: buy, sell, Talk (EvrStart 4 while the script waits parked), leave
        int problems = 0;
        const cr::ItemParam* sword = app.itemTable.item(201);
        problems += app.itemTable.count() < 438 || !sword || sword->price != 450 || sword->kind != cr::kKindWeapon ||
                    sword->stat[2] != 3 || itemName(app, 201) == "item 201" || itemName(app, 1) == itemName(app, 2);
        cr::GameState g;
        problems += cr::shopLevel(g, 0) != 2;
        problems += cr::shopStock(g, app.itemTable, 0, 1, 2) != std::vector<int>{201, 202};
        problems += cr::shopStock(g, app.itemTable, 1, 2, 3) != std::vector<int>{243, 244, 245};
        problems += cr::shopStock(g, app.itemTable, 0, 0, 2) != std::vector<int>{1, 21, 28};
        problems += cr::shopStock(g, app.itemTable, 0, 3, 2) != std::vector<int>{426};
        g.setFlag(1072, true);
        problems += cr::shopLevel(g, 0) != 4; // two flags short of the progress flag
        g.setFlag(1074, true);
        problems += cr::shopLevel(g, 0) != 6;
        g.setFlag(1104, true);
        problems += cr::shopLevel(g, 0) != 7 || cr::shopLevel(g, 1) != 2;
        problems += cr::maxPurchase(450, 1000, 0) != 2 || cr::maxPurchase(10, 100000, 95) != 4;
        // The Ark's shop room (t0108, area 7) and NPC 251's script
        app.runner.stop();
        app.game = cr::GameState();
        app.mapNumber = -1;
        cr::WarpRequest w;
        w.valid = true;
        w.area = 7;
        w.map = 103;
        performWarp(app, w, -1);
        for (int f = 0; f < 3000 && (app.runner.running() || app.pendingEntry != App::Entry::None); ++f) {
            stepEvents(app);
            app.dialogue.clear();
            if (app.choiceFlag >= 0) answerChoice(app, true);
            app.tapPending = true;
        }
        problems += app.area != 7 || app.mapNumber != 103;
        app.runner.start(app.events, 251, app.game, app.area, app.mapNumber);
        for (int f = 0; f < 600 && app.runner.running() && !app.shop.open; ++f) {
            stepEvents(app);
            app.dialogue.clear();
            app.tapPending = true;
        }
        const bool opened = app.shop.open && app.runner.running();
        problems += !opened || app.shop.level != 2;
        app.game.oz = 1000;
        shopKey(app, SDLK_DOWN);   // Buy
        shopKey(app, SDLK_RETURN);
        shopKey(app, SDLK_DOWN);   // Weapons
        shopKey(app, SDLK_RETURN);
        problems += app.shop.phase != App::Shop::List || app.shop.list != std::vector<int>{201, 202};
        app.menuSoundLog.clear();
        shopKey(app, SDLK_RETURN); // Trusty Longsword, 450: how many, with Yes / No beside
        for (int k = 0; k < 3; ++k) shopKey(app, SDLK_RIGHT); // as many as the Oz buy: 2, then past the end
        problems += app.shop.phase != App::Shop::Count || app.shop.count != 2 ||
                    shopMessage(app).find(itemName(app, 201)) == std::string::npos;
        shopKey(app, SDLK_RETURN); // Yes (Escher wears one already: no "Equip it?")
        problems += app.game.oz != 100 || app.game.items[201] != 2 || app.shop.phase != App::Shop::List;
        shopKey(app, SDLK_DOWN);   // Bastard Sword: 1800, too dear
        shopKey(app, SDLK_RETURN);
        problems += app.shop.phase != App::Shop::List;
        problems += expectSounds("shop: item, count 2 then its end twice, Yes, too dear", {2, 2, 4, 4, 22, 4, 4});
        // Armor for Escher, worn at once: what he wore goes in the bag
        app.game.oz = 2000;
        shopKey(app, SDLK_ESCAPE);
        shopKey(app, SDLK_DOWN);   // Armor
        shopKey(app, SDLK_RETURN);
        problems += app.shop.list != std::vector<int>{215, 216};
        shopKey(app, SDLK_DOWN);
        shopKey(app, SDLK_RETURN); // 216, 1350
        shopKey(app, SDLK_RETURN); // Yes
        const bool asked = app.shop.phase == App::Shop::EquipAsk && shopMessage(app).find(itemName(app, 216)) != std::string::npos;
        shopKey(app, SDLK_RETURN); // Yes, equip it
        const cr::PartyMember& escher = app.game.party[0];
        problems += !asked || app.game.oz != 650 || escher.equip[cr::kArmor] != 216 || app.game.items[216] != 0 ||
                    app.game.items[215] != 1 || app.shop.phase != App::Shop::List;
        problems += cr::maxStat(escher, 0, cr::kHp, app.itemTable) != 842 + 50 + 24 ||
                    cr::maxStat(escher, 0, cr::kAttack, app.itemTable) != 180 + 5 + 3 ||
                    cr::maxStat(escher, 0, cr::kDefense, app.itemTable, 215) != 135 + 5 + 5;
        shopKey(app, SDLK_RIGHT);  // Musiea: her own armor
        problems += app.shop.member != 1 || app.shop.list != std::vector<int>{243, 244};
        shopKey(app, SDLK_LEFT);
        app.lastTicks = 0;
        drawFrame(app, false); // the board, the list
        ++frames;
        app.menuSoundLog.clear();
        shopKey(app, SDLK_ESCAPE);
        shopKey(app, SDLK_ESCAPE); // back to Talk / Buy / Sell
        shopKey(app, SDLK_DOWN);   // Sell
        shopKey(app, SDLK_RETURN);
        shopKey(app, SDLK_UP);     // Weapons
        shopKey(app, SDLK_RETURN);
        problems += app.shop.list != std::vector<int>{201};
        shopKey(app, SDLK_RETURN);
        shopKey(app, SDLK_RETURN); // one, for half the price
        problems += app.game.oz != 875 || app.game.items[201] != 1 || app.shop.phase != App::Shop::List;
        problems += expectSounds("shop: Back, Back, Sell, Weapons, the sword, Yes", {3, 3, 4, 2, 4, 2, 2, 22});
        app.lastTicks = 0;
        drawFrame(app, false);
        ++frames;
        shopKey(app, SDLK_ESCAPE);
        shopKey(app, SDLK_ESCAPE);
        shopKey(app, SDLK_UP);
        shopKey(app, SDLK_UP);     // Talk
        shopKey(app, SDLK_RETURN);
        problems += app.shop.phase != App::Shop::Talk;
        int talkFrames = 0;
        for (; talkFrames < 3000 && app.shop.phase == App::Shop::Talk; ++talkFrames) {
            stepShop(app);
            stepEvents(app);
            app.dialogue.clear();
            if (app.choiceFlag >= 0) answerChoice(app, true);
            app.tapPending = true;
        }
        const size_t talkMessages = app.parkedRunner.result().messages.size();
        problems += app.shop.phase != App::Shop::Main || app.game.flag(4090) || !app.runner.running() || talkMessages == 0;
        shopKey(app, SDLK_ESCAPE); // leave: the shopkeeper's script goes on to its end
        for (int f = 0; f < 600 && app.runner.running(); ++f) {
            stepEvents(app);
            app.dialogue.clear();
            app.tapPending = true;
        }
        problems += app.shop.open || app.runner.running();
        std::printf("shops: %d items; Ark shop opened by its script %s, bought 2 and sold 1 (%d Oz left), Talk played %zu "
                    "messages in %d frames; %d problems\n",
                    app.itemTable.count(), opened ? "ok" : "FAILED", app.game.oz, talkMessages, talkFrames, problems);
        mapErrors += problems;
        bootTitle(app);
        app.runner.stop();
    }
    { // The Camp menu's pages: seeds and an MP item used on a member, a weapon worn, drawn without errors
        int problems = 0;
        app.game = cr::GameState();
        app.playerChar = 0;
        app.game.items = {{3, 3}, {24, 1}, {21, 1}, {202, 1}};
        app.game.party[0].mp = 10;
        openCamp(app);
        auto key = [&](SDL_Keycode k) { campKey(app, k); };
        App::Camp& c = app.camp;
        const int atk0 = cr::maxStat(app.game.party[0], 0, cr::kAttack, app.itemTable);
        c.cursor = 0, key(SDLK_RETURN);                       // Items: Power Seed first
        problems += c.page != App::Camp::Items || c.list.empty() || c.list[0] != 3;
        key(SDLK_RETURN), key(SDLK_RETURN);                   // on Escher: how many?
        key(SDLK_RIGHT), key(SDLK_RIGHT), key(SDLK_RIGHT);    // 3 (the bag's all)
        problems += c.count != 3;
        key(SDLK_RETURN);                                     // Yes
        const int atk1 = cr::maxStat(app.game.party[0], 0, cr::kAttack, app.itemTable);
        problems += atk1 != atk0 + 3 || app.game.party[0].seeds[0] != 3 || ownedCount(app, 3) != 0;
        const auto plum = std::find(c.list.begin(), c.list.end(), 24); // Plum: MP + 100
        problems += plum == c.list.end();
        if (plum != c.list.end()) c.listCursor = int(plum - c.list.begin());
        key(SDLK_RETURN), key(SDLK_RETURN), key(SDLK_RETURN);
        problems += app.game.party[0].mp != 60 || ownedCount(app, 24) != 0; // 10 + 100, capped at 60
        const auto choc = std::find(c.list.begin(), c.list.end(), 21); // HP items do nothing on the field
        if (choc != c.list.end()) c.listCursor = int(choc - c.list.begin()), key(SDLK_RETURN);
        problems += c.phase != App::Camp::List || ownedCount(app, 21) != 1;
        int frames = 0;
        for (int f = 0; f < 3; ++f) { app.lastTicks = 0; drawFrame(app, false); ++frames; }
        key(SDLK_ESCAPE);
        c.cursor = 1, key(SDLK_RETURN), key(SDLK_RETURN);   // Equip: Escher's weapon
        key(SDLK_RETURN);
        problems += c.phase != App::Camp::List || c.list.empty() || c.list[0] != 202;
        for (int f = 0; f < 3; ++f) { app.lastTicks = 0; drawFrame(app, false); ++frames; }
        key(SDLK_RETURN);                                     // wear the Bastard Sword
        problems += app.game.party[0].equip[0] != 202 || ownedCount(app, 201) != 1 || ownedCount(app, 202) != 0;
        key(SDLK_ESCAPE), key(SDLK_ESCAPE), key(SDLK_ESCAPE);
        c.cursor = 3, key(SDLK_RETURN), key(SDLK_RETURN), key(SDLK_RIGHT); // Status: Musiea's
        problems += c.page != App::Camp::Status || c.member != 1;
        for (int f = 0; f < 3; ++f) { app.lastTicks = 0; drawFrame(app, false); ++frames; }
        const int errs = app.renderer.takeErrors();
        key(SDLK_ESCAPE), key(SDLK_ESCAPE), key(SDLK_ESCAPE);
        problems += app.campOpen + errs;
        std::printf("camp: 3 seeds +%d attack, Plum to MP %d, Bastard Sword worn, status page; %d frames, %d render API "
                    "errors; %d problems\n", atk1 - atk0, app.game.party[0].mp, frames, errs, problems);
        mapErrors += problems;
        glErrors += errs;
        app.game = cr::GameState();
    }
    { // The field menu (menuf_mainPrc): the corner button slides the bar out and back in 4 frames;
      // skill 170 known adds the encounter switch (per pair, refused under flag 4076), and while
      // it's off the walked distance doesn't count; skill 174 worn banks 1 EXP every 10 units,
      // paid into the next win; both are kept in the save
        int problems = 0;
        app.game = cr::GameState();
        app.playerChar = 0;
        app.menuSoundLog.clear();
        App::FieldMenu& fm = app.fieldMenu;
        fm = App::FieldMenu();
        toggleFieldMenu(app);
        float x0, y0, x4, y4;
        fieldButtonPos(app, 0, x0, y0);
        for (int f = 0; f < 4; ++f) stepFieldMenu(app);
        fieldButtonPos(app, 0, x4, y4);
        problems += !fm.open || fm.anim != 0 || x0 != 460 || y0 != -28 || x4 != 432 || y4 != 32;
        problems += fieldButtonShown(app, 3); // not known yet
        pressFieldButton(app, 3);
        problems += app.game.noEncounters[0];
        app.game.items[170] = 1;
        pressFieldButton(app, 3); // off
        const float d0 = app.encounters.walkedDistance();
        fieldWalked(app, 50);
        problems += !app.game.noEncounters[0] || app.encounters.walkedDistance() != d0;
        app.playerChar = 2; // the next pair's switch is its own
        problems += fieldEncountersOff(app);
        app.playerChar = 0;
        app.game.setFlag(4076, true);
        pressFieldButton(app, 3); // refused
        problems += !app.game.noEncounters[0];
        app.game.setFlag(4076, false);
        pressFieldButton(app, 3); // on again
        fieldWalked(app, 50);
        problems += app.game.noEncounters[0] || app.encounters.walkedDistance() != d0 + 50;
        problems += expectSounds("field menu, encounter switch", {8, 3, 4, 2});
        toggleFieldMenu(app);
        for (int f = 0; f < 4; ++f) stepFieldMenu(app);
        fieldButtonPos(app, 3, x4, y4);
        problems += fm.open || x4 != 460 || y4 != -28;
        app.menuSoundLog.clear();
        // skill 174 on Escher (plate 499): 100 one-unit steps and a 25-unit one
        app.game.items[174] = 1, app.game.items[499] = 1;
        app.game.party[0].genes[1] = 499;
        app.walkExpDistance = 0;
        for (int k = 0; k < 100; ++k) fieldWalked(app, 1);
        const int bank100 = app.game.walkExp[0];
        fieldWalked(app, 25);
        fieldWalked(app, 0.5f);
        const int bank = app.game.walkExp[0];
        problems += bank100 != 10 || bank != 12;
        // kept in the save
        cr::SaveData sd = captureSave(app);
        const std::string path = savePath(app, "fieldmenu-test.dat");
        cr::SaveData back;
        problems += !cr::writeSave(path, sd) || !cr::readSave(path, back) || back.game.walkExp[0] != bank ||
                    back.game.noEncounters != sd.game.noEncounters;
        sd.game.noEncounters[3] = true;
        problems += !cr::writeSave(path, sd) || !cr::readSave(path, back) || !back.game.noEncounters[3];
        SDL_RemovePath(path.c_str());
        // paid into a win, before the Apples double it
        int paid = -1;
        startBattle(app, {1, 0, -1, 0}, 0);
        if (app.battle.active) {
            App::Battle& b = app.battle;
            for (int i = 2; i < 5; ++i) b.rules.f[size_t(i)].hp = 0;
            b.phase = App::Battle::Acting;
            b.turnOn = false;
            const int before = b.rules.expGained;
            endRound(app);
            paid = b.rules.expGained - before;
            endBattle(app, true);
            app.runner.stop();
        }
        problems += paid != bank || app.game.walkExp[0] != 0;
        // The map screen: every field has its spot (the 3D rooms 13 / 29 / 46 / 63 none) and every
        // marker lies on its gate's 480 x 320 map; map 0 with the story at step 3
        int spots = 0, offMap = 0;
        for (int mp = 0; mp < 64; ++mp) {
            const bool room3d = mp == 13 || mp == 29 || mp == 46 || mp == 63;
            spots += mapui::kMapSpot[mp][0] >= 0;
            offMap += room3d != (mapui::kMapSpot[mp][0] < 0) ||
                      (!room3d && (mapui::kMapSpot[mp][0] - 24 < 0 || mapui::kMapSpot[mp][0] + 24 > 480 ||
                                   mapui::kMapSpot[mp][1] - 20 < 0 || mapui::kMapSpot[mp][1] + 28 > 320));
        }
        for (const auto& gate : mapui::kMapMark)
            for (const auto& step : gate)
                for (const auto& mk : step)
                    offMap += mk[0] >= 0 && (mk[0] - 8 < 0 || mk[0] + 8 > 480 || mk[1] - 16 < 0 || mk[1] > 320);
        const int keepMap = app.mapNumber;
        app.mapNumber = 0;
        app.game.setFlag(mapui::kFlagBase + 3, true);
        app.menuSoundLog.clear();
        fm.open = true, fm.anim = 0;
        pressFieldButton(app, 2);
        const App::MapScreen& ms = app.mapScreen;
        problems += !ms.open || ms.gate != 0 || !ms.tex.id || ms.spot[0] != 48 || ms.spot[1] != 196 ||
                    ms.marks[0][0] != 84 || ms.marks[1][0] != 104;
        for (int f = 0; f < 6; ++f) stepMapScreen(app);
        app.lastTicks = 0;
        drawFrame(app, false);
        const int mapErrs = app.renderer.takeErrors();
        closeMapScreen(app);
        for (int f = 0; f < 4; ++f) stepMapScreen(app);
        problems += ms.open + mapErrs + expectSounds("map screen", {2, 2}) + offMap + (spots != 60);
        app.mapNumber = keepMap;
        std::printf("field menu: bar slides out and back, encounter switch per pair (flag 4076 refuses it), skill 174 "
                    "banked %d EXP, %d paid into a win; map screen: %d spots, %d off the map; %d problems\n", bank, paid,
                    spots, offMap, problems);
        mapErrors += problems;
        app.game = cr::GameState();
        fm = App::FieldMenu();
    }
    { // A puzzle room (stage 0, map 114: teleport): its objects on the grid, a swap with a block,
      // both gems picked up (+1 ESP each), then the gem crystal's scene, and the room cleared
        int problems = 0;
        app.runner.stop();
        app.game = cr::GameState();
        app.mapNumber = -1;
        cr::WarpRequest w;
        w.valid = true, w.area = cr::kPuzzleArea, w.map = cr::kFirstPuzzleMap;
        app.game.setFlag(3070, true); // as a dungeon's way into a puzzle room sets it: the area's script 0 shows the banner
        performWarp(app, w, -1);
        int bannerFrames = 0, bannerPeak = 0; // Uty_PuzzleStart: up, held, gone
        float bannerX1 = 0;
        for (int f = 0; f < 3000 && (app.runner.running() || app.pendingEntry != App::Entry::None || app.puzzleBanner.on); ++f) {
            stepEvents(app);
            if (app.puzzleBanner.on) {
                ++bannerFrames;
                if (app.puzzleBanner.alpha > bannerPeak) bannerPeak = app.puzzleBanner.alpha, bannerX1 = app.puzzleBanner.x1;
                if (bannerFrames == 20) { // a look at it held (CR_PUZZLE_SHOT_BANNER: saved)
                    app.lastTicks = 0;
                    drawFrame(app, false);
                    if (const char* path = SDL_getenv("CR_PUZZLE_SHOT_BANNER")) saveFrame(app, path);
                }
            }
            app.dialogue.clear();
            if (app.choiceFlag >= 0) answerChoice(app, true);
            app.tapPending = true;
        }
        const bool banner = bannerFrames >= 38 && bannerFrames <= 44 && bannerPeak == 255 && bannerX1 == 60 && !app.puzzleBanner.on &&
                            !app.game.flag(3070);
        problems += !banner;
        std::printf("puzzle room: \"Puzzle Start\" banner %s (%d frames, alpha up to %d, at x %.0f)\n",
                    banner ? "shown" : "WRONG", bannerFrames, bannerPeak, bannerX1);
        App::Puzzle& pz = app.puzzle;
        int blocks = 0, gems = 0;
        for (const App::Puzzle::Obj& o : pz.objs) blocks += o.type == cr::kTypeNormal, gems += o.type == cr::kTypeGem;
        problems += pz.stage != 0 || blocks != 2 || gems != 2 || pz.esp != 0;
        // no ESP yet: a swap is refused (only the mark)
        setActor(app, 0, puzzle::centre(app, 2, 2), nullptr, false);
        const int block = puzzle::objectAt(app, 1, 2, 1);
        problems += block < 0;
        if (block >= 0) puzzle::teleportTo(app, block);
        const bool refused = !pz.busy && pz.tpState == 0 && app.game.vars[13] == 0 && pz.punished > 0;
        problems += !refused;
        // the gem at (0, 0), walked to (ESP 1): effects 2 and 3 there
        const int gemFx = app.fieldEffectCount[2] + app.fieldEffectCount[3];
        setActor(app, 0, puzzle::centre(app, 0, 0), nullptr, true);
        for (int f = 0; f < 3; ++f) stepEvents(app);
        const bool firstGem = pz.esp == 1 && app.fieldEffectCount[2] + app.fieldEffectCount[3] == gemFx + 2;
        stepPuzzleHud(app); // the HUD has shown a count above 0
        const bool popped = pz.pop > 0;
        // stand at the centre (2, 2) and swap with the block at (2, 1) (ESP 0)
        setActor(app, 0, puzzle::centre(app, 2, 2), nullptr, false);
        app.menuSoundLog.clear();
        if (block >= 0) puzzle::teleportTo(app, block);
        for (int f = 0; f < 30; ++f) stepEvents(app);
        int pr = -1, pc = -1;
        puzzle::cellOf(app, app.walker.pos, pr, pc);
        for (int f = 0; f < 3; ++f) stepPuzzleHud(app);
        if (const char* path = SDL_getenv("CR_PUZZLE_SHOT_HUD")) { // the pad at 0 ESP (CR_PUZZLE_SHOT_HUD: saved)
            pz.blink = 0;
            const int keepFade = app.fieldMenu.fade;
            app.fieldMenu.fade = 320;
            app.lastTicks = 0;
            drawFrame(app, false);
            saveFrame(app, path);
            app.fieldMenu.fade = keepFade;
        }
        const bool hudBlink = popped && pz.hadEsp && pz.blink > 0 && pz.shownEsp == 0;
        problems += !hudBlink;
        const bool swapped = firstGem && block >= 0 && pz.objs[size_t(block)].row == 2 && pz.objs[size_t(block)].col == 2 &&
                             pr == 2 && pc == 1 && puzzle::objectAt(app, 1, 2, 2) == block && app.game.vars[13] == 1 && pz.esp == 0;
        // the gem at (4, 4) (ESP 1, none left)
        setActor(app, 0, puzzle::centre(app, 4, 4), nullptr, true);
        for (int f = 0; f < 3; ++f) stepEvents(app);
        const int espAfterGems = pz.esp;
        problems += espAfterGems != 1;
        problems += !swapped || puzzle::gemsLeft(app);
        // the crystal's scene starts by itself, sets the room's flag, then the crystal shows
        bool sceneRan = false;
        for (int f = 0; f < 6000; ++f) {
            stepEvents(app);
            sceneRan |= app.runner.running();
            app.dialogue.clear();
            if (app.choiceFlag >= 0) answerChoice(app, true);
            app.tapPending = true;
            if (sceneRan && !app.runner.running() && app.pendingEntry == App::Entry::None) break;
        }
        for (int f = 0; f < 3; ++f) stepEvents(app); // the crystal shows the frame after
        bool crystal = false;
        for (const App::Puzzle::Obj& o : pz.objs) crystal |= o.type == cr::kTypeCrystal && o.shown;
        const bool roomFlag = app.game.flag(3300), clearFlag = app.game.flag(3000);
        problems += !sceneRan || (!crystal && !clearFlag);
        app.lastTicks = 0;
        drawFrame(app, false);
        const int errs = app.renderer.takeErrors();
        problems += errs;
        // stage 8 (map 122): the block at (4, 4) teleported onto the switch at (0, 0) presses it - the switch crystal's scene
        app.runner.stop();
        app.game = cr::GameState();
        app.mapNumber = -1;
        w.map = cr::kFirstPuzzleMap + 8;
        performWarp(app, w, -1);
        for (int f = 0; f < 3000 && (app.runner.running() || app.pendingEntry != App::Entry::None); ++f) {
            stepEvents(app);
            app.dialogue.clear();
            if (app.choiceFlag >= 0) answerChoice(app, true);
            app.tapPending = true;
        }
        for (int quiet = 0, f = 0; f < 3000 && quiet < 10; ++f) { // the room's entry script, to its end
            stepEvents(app);
            app.dialogue.clear();
            if (app.choiceFlag >= 0) answerChoice(app, true);
            app.tapPending = true;
            quiet = app.runner.running() || app.pendingEntry != App::Entry::None ? 0 : quiet + 1;
        }
        auto crystalShown = [&] {
            for (const App::Puzzle::Obj& o : pz.objs)
                if (o.type == cr::kTypeCrystal && o.shown) return true;
            return false;
        };
        for (int f = 0; f < 3; ++f) stepEvents(app);
        const bool hiddenBefore = !crystalShown() && !app.runner.running(); // the room's script done, the switch up
        setActor(app, 0, puzzle::centre(app, 0, 0), nullptr, false);
        const int b8 = puzzle::objectAt(app, 1, 4, 4);
        if (b8 >= 0) puzzle::teleportTo(app, b8);
        for (int quiet = 0, f = 0; f < 3000 && quiet < 10; ++f) {
            stepEvents(app);
            app.dialogue.clear();
            if (app.choiceFlag >= 0) answerChoice(app, true);
            app.tapPending = true;
            quiet = app.runner.running() || app.puzzle.tpState ? 0 : quiet + 1;
        }
        const bool switchScene = hiddenBefore && crystalShown();
        const bool pressed = puzzle::allSwitchesOn(app);
        if (SDL_getenv("CR_PUZZLE_DEBUG"))
            for (const App::Puzzle::Obj& o : pz.objs)
                if (o.type == cr::kTypeCrystal || o.type == cr::kTypeWeighSwitch)
                    std::printf("  obj npc %d id %d type %d bits %03x shown %d on %d; flag 3308 %d 4000 %d, runner %s, last %s\n", o.npc,
                                app.npcs[size_t(o.npc)].id, o.type, app.npcs[size_t(o.npc)].puzzle, o.shown, o.on, app.game.flag(3308),
                                app.game.flag(4000), app.runner.running() ? app.runner.where().c_str() : "idle", app.lastEvent.c_str());
        problems += b8 < 0 || !pressed || !switchScene;
        app.runner.stop();
        std::printf("puzzle room: stage 8 - the block teleported onto the switch %s, the switch crystal %s\n",
                    pressed ? "presses it" : "DIDN'T press it", switchScene ? "came" : "DIDN'T come");
        // stage 24 (map 138, psychokinesis): (3, 3) right to (3, 4), up to (1, 4) under the fixed
        // block at (0, 4), then (1, 1) right onto the switch at (1, 3); a letting go costs nothing;
        // then (3, 1) sent right into the leader at (3, 2) squashes the try
        {
            app.game = cr::GameState();
            app.mapNumber = -1;
            w.map = cr::kFirstPuzzleMap + 24;
            performWarp(app, w, -1);
            for (int quiet = 0, f = 0; f < 3000 && quiet < 10; ++f) {
                stepEvents(app);
                app.dialogue.clear();
                if (app.choiceFlag >= 0) answerChoice(app, true);
                app.tapPending = true;
                quiet = app.runner.running() || app.pendingEntry != App::Entry::None ? 0 : quiet + 1;
            }
            setActor(app, 0, puzzle::centre(app, 2, 2), nullptr, false);
            int poseOk = 0, quakes = 0, slides = 0;
            auto slide = [&](int r, int c, int dir) { // the block's cell after the slide (-1 none)
                const int t = puzzle::objectAt(app, 1, r, c);
                if (t < 0) return std::pair<int, int>{-1, -1};
                puzzle::pkGrab(app, t);
                for (int f = 0; f < 20 && pz.objs[size_t(t)].pk != 2; ++f) stepEvents(app);
                updatePlayer(app, 0, 0); // the leader's pose while it's held
                poseOk += app.player.action == 3 && app.player.loopFrom == 2;
                puzzle::pkSend(app, dir);
                bool quaked = false;
                for (int f = 0; f < 200 && pz.pkState && pz.scene == 4; ++f) {
                    stepEvents(app);
                    quaked |= app.fieldQuake.mode == 1 && app.fieldQuake.frames == 8 && app.fieldQuake.amp == 0.5f;
                }
                ++slides, quakes += quaked;
                updatePlayer(app, 0, 0);
                poseOk -= app.player.action == 3; // standing again
                return std::pair<int, int>{pz.objs[size_t(t)].row, pz.objs[size_t(t)].col};
            };
            const bool pkRoom = pz.stage == 24 && pz.esp == -1;
            const auto drop = slide(3, 3, 2);  // down: (4, 3) free, then the edge
            const auto drop2 = slide(4, 3, 2); // at the edge: nowhere to go - let go, no use counted
            const bool letGo = drop2 == std::pair<int, int>{4, 3} && app.game.vars[13] == 1 && !pz.busy;
            // the solution, from the room as it began
            app.runner.stop();
            app.npcs = app.events.npcs(app.mapNumber);
            puzzle::enter(app);
            pz.scene = 4;
            setActor(app, 0, puzzle::centre(app, 2, 2), nullptr, false);
            const auto s1 = slide(3, 3, 4), s2 = slide(3, 4, 1), s3 = slide(1, 1, 4);
            const bool solved = s1 == std::pair<int, int>{3, 4} && s2 == std::pair<int, int>{1, 4} &&
                                s3 == std::pair<int, int>{1, 3} && puzzle::allSwitchesOn(app) && app.game.vars[13] == 3;
            for (int quiet = 0, f = 0; f < 3000 && quiet < 10; ++f) { // the switch crystal's scene
                stepEvents(app);
                app.dialogue.clear();
                if (app.choiceFlag >= 0) answerChoice(app, true);
                app.tapPending = true;
                quiet = app.runner.running() ? 0 : quiet + 1;
            }
            // the squash
            setActor(app, 0, puzzle::centre(app, 3, 2), nullptr, false);
            app.menuSoundLog.clear();
            slide(3, 1, 4);
            const bool squashed = pz.scene == 2;
            if (SDL_getenv("CR_PUZZLE_DEBUG"))
                std::printf("  squash: scene %d pkState %d busy %d runner %s cleared %d dialogue %d, player %.1f %.1f\n", pz.scene,
                            pz.pkState, pz.busy, app.runner.running() ? app.runner.where().c_str() : "idle", puzzle::cleared(app),
                            int(app.dialogue.size()), app.walker.pos.x, app.walker.pos.z);
            for (int f = 0; f < 40 && pz.scene != 4; ++f) stepEvents(app);
            const bool restored = pz.scene == 4 && !puzzle::allSwitchesOn(app) && puzzle::objectAt(app, 1, 1, 1) >= 0 &&
                                  puzzle::objectAt(app, 1, 3, 3) >= 0 && app.game.vars[13] == 0;
            problems += poseOk < 3 || quakes < 3;
            std::printf("puzzle room: stage 24 - the holding pose %d of %d, landing quakes %d\n", poseOk, slides, quakes);
            problems += !pkRoom || drop != std::pair<int, int>{4, 3} || !letGo ||
                        !solved || !squashed || !restored;
            std::printf("puzzle room: stage 24 (psychokinesis) - slides %s, letting go %s, the switch %s in 3, squash %s, "
                        "room %s\n", drop == std::pair<int, int>{4, 3} ? "ok" : "FAILED", letGo ? "free" : "COUNTED", solved ? "pressed" : "NOT pressed",
                        squashed ? "resets" : "DIDN'T reset", restored ? "restored" : "NOT restored");
            app.runner.stop();
        }
        // stage 32 (map 146, toughness, ESP 2): from (1, 1) the fragile block at (0, 1) breaks
        // (ESP 1), the gem at (0, 0) is reached through its cell (ESP 2); at 0 ESP a tap is refused.
        // Stage 40 (map 154, ESP 0): the gem at (1, 1) (ESP 1), a tap on a red block puts both
        // away and brings both blue ones; the gem at (1, 3) (ESP 1), then from the red block's cell
        // (0, 2) a tap on a blue one brings the reds back onto the leader - the room resets
        {
            auto room = [&](int stageNo) {
                app.runner.stop();
                app.game = cr::GameState();
                app.mapNumber = -1;
                w.map = cr::kFirstPuzzleMap + stageNo;
                performWarp(app, w, -1);
                for (int quiet = 0, f = 0; f < 3000 && quiet < 10; ++f) {
                    stepEvents(app);
                    app.dialogue.clear();
                    if (app.choiceFlag >= 0) answerChoice(app, true);
                    app.tapPending = true;
                    quiet = app.runner.running() || app.pendingEntry != App::Entry::None ? 0 : quiet + 1;
                }
            };
            auto walkTo = [&](int r, int c) {
                setActor(app, 0, puzzle::centre(app, r, c), nullptr, true);
                for (int f = 0; f < 3; ++f) stepEvents(app);
            };
            room(32);
            walkTo(1, 1);
            const int frag = puzzle::objectAt(app, 1, 0, 1);
            const bool fragOk0 = pz.stage == 32 && pz.esp == 2 && frag >= 0 && pz.objs[size_t(frag)].type == cr::kTypeFragile &&
                                 pz.objs[size_t(frag)].shown;
            app.walker.heading = 3.14159265f; // facing row - 1
            puzzle::abilityKey(app);
            for (int f = 0; f < 3; ++f) stepEvents(app);
            const bool broke = frag >= 0 && !pz.objs[size_t(frag)].shown && puzzle::objectAt(app, 1, 0, 1) < 0 && pz.esp == 1 &&
                               app.game.vars[13] == 1 && !npcVisible(app, size_t(pz.objs[size_t(frag)].npc));
            walkTo(0, 1);
            walkTo(0, 0);
            const bool gem = pz.esp == 2;
            pz.esp = 0;
            const int frag2 = puzzle::objectAt(app, 1, 1, 0);
            puzzle::toughTap(app, frag2);
            for (int f = 0; f < 3; ++f) stepEvents(app);
            const bool refusedT = frag2 >= 0 && pz.objs[size_t(frag2)].shown && app.game.vars[13] == 1 && pz.punished > 0;
            room(40);
            walkTo(1, 1);
            const int red = puzzle::objectAt(app, 1, 0, 2), blue = puzzle::objectAt(app, 1, 2, 0);
            auto solid = [&](int t) { return t >= 0 && pz.objs[size_t(t)].shown && !pz.objs[size_t(t)].ghost; };
            auto ghostOf = [&](int t) {
                const App::NpcDraw* d = t >= 0 ? puzzle::drawOf(app, pz.objs[size_t(t)].npc) : nullptr;
                return t >= 0 && pz.objs[size_t(t)].ghost && d && d->alpha < 0.4f;
            };
            const bool start = pz.stage == 40 && solid(red) && ghostOf(blue) && pz.esp == 1;
            puzzle::toughTap(app, red);
            for (int f = 0; f < 8; ++f) stepEvents(app);
            const int red2 = puzzle::objectAt(app, 1, 4, 2), blue2 = puzzle::objectAt(app, 1, 2, 4);
            const bool swappedRB = ghostOf(red) && ghostOf(red2) && solid(blue) && solid(blue2) && pz.esp == 0;
            if (const char* shot = SDL_getenv("CR_PUZZLE_SHOT")) { // a look at the changed-over blocks
                app.lastTicks = 0;
                drawFrame(app, false);
                int sw = 0, sh = 0;
                std::vector<uint8_t> px;
                if (app.renderer.readPixels(px, sw, sh)) {
                    SDL_Surface* surf = SDL_CreateSurface(sw, sh, SDL_PIXELFORMAT_RGBA32);
                    for (int y = 0; y < sh; ++y)
                        std::memcpy(static_cast<uint8_t*>(surf->pixels) + size_t(y) * surf->pitch, &px[size_t(y) * sw * 4], size_t(sw) * 4);
                    SDL_SaveBMP(surf, shot);
                    SDL_DestroySurface(surf);
                }
            }
            walkTo(1, 3);
            setActor(app, 0, puzzle::centre(app, 0, 2), nullptr, false);
            puzzle::toughTap(app, blue);
            stepEvents(app);
            const bool squashedRB = pz.scene == 2;
            for (int f = 0; f < 40 && pz.scene != 4; ++f) stepEvents(app);
            const bool restoredRB = pz.scene == 4 && solid(puzzle::objectAt(app, 1, 0, 2)) && ghostOf(puzzle::objectAt(app, 1, 2, 0));
            problems += !fragOk0 || !broke || !gem || !refusedT || !start || !swappedRB || !squashedRB || !restoredRB;
            std::printf("puzzle room: stage 32 (toughness) - fragile block %s, gem %s, at 0 ESP %s; stage 40 - red / blue %s, "
                        "%s, squash %s, room %s\n", fragOk0 && broke ? "breaks" : "DIDN'T break", gem ? "ok" : "MISSED",
                        refusedT ? "refused" : "NOT refused", start ? "start right" : "start WRONG", swappedRB ? "change over" : "DON'T change",
                        squashedRB ? "resets" : "DIDN'T reset", restoredRB ? "restored" : "NOT restored");
            app.runner.stop();
        }
        // stage 48 (map 162, pyrokinesis, ESP 0): the gem at (0, 2) (ESP 1), then the candle switch
        // at (2, 2) lights (lit 15 frames on) and changes its candle blocks over - the three lit ones
        // (+22 = 1) put away, the three out lit. Stage 56 (map 170): switches linked in pairs (1, 2)
        // - (3, 2) and (2, 1) - (2, 3): one lit lights the other (5 + 15 frames), with a beam; one put
        // out puts the other out; all four lit bring the crystal (its scene)
        {
            auto room = [&](int stageNo) {
                app.runner.stop();
                app.game = cr::GameState();
                app.mapNumber = -1;
                w.map = cr::kFirstPuzzleMap + stageNo;
                performWarp(app, w, -1);
                for (int quiet = 0, f = 0; f < 3000 && quiet < 10; ++f) {
                    stepEvents(app);
                    app.dialogue.clear();
                    if (app.choiceFlag >= 0) answerChoice(app, true);
                    app.tapPending = true;
                    quiet = app.runner.running() || app.pendingEntry != App::Entry::None ? 0 : quiet + 1;
                }
            };
            auto steps = [&](int n) { for (int f = 0; f < n; ++f) stepEvents(app); };
            room(48);
            setActor(app, 0, puzzle::centre(app, 0, 2), nullptr, true);
            steps(3);
            const int sw = puzzle::objectAt(app, 1, 2, 2);
            int litBlocks = 0, outBlocks = 0;
            for (const App::Puzzle::Obj& o : pz.objs)
                if (o.type == cr::kTypeCandle) (o.shown ? litBlocks : outBlocks) += 1;
            const bool start48 = pz.stage == 48 && pz.esp == 1 && sw >= 0 && pz.objs[size_t(sw)].type == cr::kTypeCandleSwitch &&
                                 litBlocks == 3 && outBlocks == 3;
            const int b01 = puzzle::objectAt(app, 1, 0, 1), b03 = puzzle::objectAt(app, 1, 0, 3);
            setActor(app, 0, puzzle::centre(app, 1, 1), nullptr, false);
            puzzle::pyroTap(app, sw);
            steps(2);
            const bool lighting = sw >= 0 && pz.objs[size_t(sw)].cs == 2 && pz.esp == 0 && app.game.vars[13] == 1;
            steps(16);
            const bool lit48 = sw >= 0 && pz.objs[size_t(sw)].cs == 3;
            const bool changed = b01 >= 0 && b03 >= 0 && !pz.objs[size_t(b01)].shown && pz.objs[size_t(b01)].ghost &&
                                 pz.objs[size_t(b03)].shown && !npcVisible(app, size_t(pz.objs[size_t(b01)].npc));
            puzzle::pyroTap(app, sw); // no ESP: refused
            steps(2);
            const bool refusedP = sw >= 0 && pz.objs[size_t(sw)].cs == 3 && pz.punished > 0;
            room(56);
            const int s12 = puzzle::objectAt(app, 1, 1, 2), s32 = puzzle::objectAt(app, 1, 3, 2), s21 = puzzle::objectAt(app, 1, 2, 1),
                      s23 = puzzle::objectAt(app, 1, 2, 3);
            auto csOf = [&](int t) { return t >= 0 ? pz.objs[size_t(t)].cs : -1; };
            const bool start56 = pz.stage == 56 && csOf(s12) == 0 && csOf(s32) == 0 && csOf(s21) == 0 && csOf(s23) == 0;
            setActor(app, 0, puzzle::centre(app, 0, 0), nullptr, false);
            puzzle::pyroTap(app, s12);
            steps(3);
            const bool beam = s32 >= 0 && pz.objs[size_t(s32)].csFrom == pz.objs[size_t(s12)].npc;
            auto shot = [&](const char* var) { // CR_PUZZLE_SHOT56A / B: a look at the stand-ins
                const char* path = SDL_getenv(var);
                if (!path) return;
                app.lastTicks = 0;
                drawFrame(app, false);
                int sw = 0, sh = 0;
                std::vector<uint8_t> px;
                if (!app.renderer.readPixels(px, sw, sh)) return;
                SDL_Surface* surf = SDL_CreateSurface(sw, sh, SDL_PIXELFORMAT_RGBA32);
                for (int y = 0; y < sh; ++y)
                    std::memcpy(static_cast<uint8_t*>(surf->pixels) + size_t(y) * surf->pitch, &px[size_t(y) * sw * 4], size_t(sw) * 4);
                SDL_SaveBMP(surf, path);
                SDL_DestroySurface(surf);
            };
            app.lastTicks = 0;
            drawFrame(app, false); // the beam and the first flame drawn
            const int fxErrs = app.renderer.takeErrors();
            shot("CR_PUZZLE_SHOT56A");
            steps(25);
            shot("CR_PUZZLE_SHOT56B");
            auto flames = [&] {
                int n = 0;
                for (const Effect& e : app.fieldEffects) n += e.id == 0xc && e.loop;
                return n;
            };
            const bool pair = csOf(s12) == 3 && csOf(s32) == 3 && csOf(s21) == 0 && flames() == 2;
            puzzle::pyroTap(app, s32); // out again, and its pair
            steps(40);
            const bool outPair = csOf(s12) == 0 && csOf(s32) == 0 && flames() == 0;
            puzzle::pyroTap(app, s12);
            puzzle::pyroTap(app, s21);
            bool crystal56 = false;
            for (int quiet = 0, f = 0; f < 3000 && (quiet < 10 || f < 40); ++f) {
                stepEvents(app);
                app.dialogue.clear();
                if (app.choiceFlag >= 0) answerChoice(app, true);
                app.tapPending = true;
                quiet = app.runner.running() ? 0 : quiet + 1;
            }
            steps(3);
            for (const App::Puzzle::Obj& o : pz.objs) crystal56 |= o.type == cr::kTypeCrystal && o.shown;
            const bool allLit = puzzle::allCandlesLit(app);
            problems += !start48 || !lighting || !lit48 || !changed || !refusedP || !start56 || !beam || fxErrs || !pair || !outPair ||
                        !allLit || !crystal56;
            glErrors += fxErrs;
            std::printf("puzzle room: stage 48 (pyrokinesis) - %s, switch %s, candle blocks %s, at 0 ESP %s; stage 56 - %s, beam %s, "
                        "pair %s, out %s, all lit %s, crystal %s\n", start48 ? "start right" : "start WRONG",
                        lighting && lit48 ? "lights" : "DOESN'T light", changed ? "change over" : "DON'T change",
                        refusedP ? "refused" : "NOT refused", start56 ? "start right" : "start WRONG", beam ? "drawn" : "MISSING",
                        pair ? "lit" : "NOT lit", outPair ? "together" : "NOT together", allLit ? "yes" : "no",
                        crystal56 ? "came" : "DIDN'T come");
            app.runner.stop();
        }
        std::printf("puzzle room: stage 0 - %d blocks, %d gems, swap %s, ESP %d after the gems, crystal scene %s (flag 3300 %d, "
                    "3000 %d, crystal %s); %d problems\n", blocks, gems, refused ? (swapped ? "refused at 0 ESP, then ok" : "FAILED") : "NOT refused at 0 ESP", espAfterGems,
                    sceneRan ? "ran" : "didn't run", roomFlag, clearFlag, crystal ? "shown" : "hidden", problems);
        mapErrors += problems;
        glErrors += errs;
        app.runner.stop();
        app.game = cr::GameState();
        app.puzzle = App::Puzzle();
    }
    { // The field camera: the doubled view follows the player within the painting, the scripts pan it
        int problems = 0;
        const int keepPaintings = app.video.paintings;
        app.video.paintings = 0;
        app.runner.stop();
        app.game = cr::GameState();
        app.mapNumber = -1;
        cr::WarpRequest w;
        w.valid = true;
        w.area = 2;
        w.map = 0; // d0101, the first gate's first field
        performWarp(app, w, -1);
        app.pendingEntry = App::Entry::None;
        app.runner.stop();
        app.cam = App::ScriptCam();
        const float SW = app.renderer.screenW(), H = cr::Renderer::kLogicalH;
        auto view = [&](float& x, float& y) { // the view's top-left on screen
            x = (SW - fieldViewW(app)) / 2 + app.scroll.offX;
            y = (H - fieldViewH(app)) / 2 - app.scroll.offY;
        };
        auto inside = [&] { // the screen stays inside the doubled view
            float x, y;
            view(x, y);
            return x <= 0.5f && y <= 0.5f && x + fieldViewW(app) >= SW - 0.5f && y + fieldViewH(app) >= H - 0.5f;
        };
        problems += !scrollingField(app) || fieldViewW(app) != 2048 || fieldViewH(app) != 2048;
        int followed = 0;
        for (const cr::Vec3 p : {cr::Vec3{120.4f, 20.0f, 69.5f}, cr::Vec3{100, 20, 60}, cr::Vec3{80, 20, 50}}) {
            app.walker.pos = p;
            followPlayerScroll(app);
            float hx, hy, x, y;
            fieldViewPoint(app, p + cr::Vec3{0, 7, 0}, hx, hy);
            view(x, y);
            const bool onScreen = x + hx >= 0 && x + hx <= SW && y + hy >= 0 && y + hy <= H;
            followed += onScreen && inside();
        }
        problems += followed != 3;
        // CameraFree: FrmPanCam moves twice its deltas over its frames, kept in the painting
        ViewerHost host(app);
        host.freeCamera(true);
        const float startY = app.scroll.offY;
        const float pan[3] = {0, -100, 0};
        host.panCamera(pan, 10);
        for (int f = 0; f < 5; ++f) stepFieldScroll(app);
        const float halfway = app.scroll.offY;
        for (int f = 0; f < 10; ++f) stepFieldScroll(app);
        const float wantY = std::clamp(startY - 200, -(fieldViewH(app) - H) / 2, (fieldViewH(app) - H) / 2);
        const float panned = app.scroll.offY;
        problems += std::fabs(halfway - (startY + wantY) / 2) > 1.0f || std::fabs(panned - wantY) > 0.5f ||
                    app.scroll.sliding || !inside();
        // FrmAdjCam: slides to a shot's (x, y); FrmSetCam: back to the centre; Mapscroll: centre on an NPC (the player)
        const float shot[3] = {-200, 150, 0};
        host.setCamera(shot, shot, 4);
        for (int f = 0; f < 4; ++f) stepFieldScroll(app);
        problems += std::fabs(app.scroll.offX + 200) > 0.5f || std::fabs(app.scroll.offY - 150) > 0.5f;
        host.setCamera(shot, shot, 0);
        problems += app.scroll.offX != 0 || app.scroll.offY != 0;
        host.mapScroll(0, 0, 0, 1.0f);
        float hx, hy, x, y;
        fieldViewPoint(app, app.walker.pos, hx, hy);
        view(x, y);
        problems += !inside() || x + hx < 0 || x + hx > SW;
        // CameraPlayer: following again
        host.freeCamera(false);
        followPlayerScroll(app);
        problems += !inside();
        app.lastTicks = 0;
        drawFrame(app, false);
        ++frames;
        std::printf("field camera: d0101's view %gx%g follows the player (%d of 3 spots), pans %g -> %g, shots and "
                    "Mapscroll; %d problems\n",
                    fieldViewW(app), fieldViewH(app), followed, startY, panned, problems);
        // Scene effects: the first gate's entry script loads set 0, turns everything off and starts the snow (5);
        // d0117's fog (3) is another room's
        int fxProblems = 0;
        endSceneEffects(app);
        app.runner.start(app.events, 0, app.game, app.area, app.mapNumber);
        for (int f = 0; f < 3000 && app.runner.running(); ++f) {
            stepEvents(app);
            app.dialogue.clear();
            if (app.choiceFlag >= 0) answerChoice(app, true);
            app.tapPending = true;
        }
        int loaded = 0;
        for (const cr::Texture& t : app.fx.tex) loaded += t.id != 0;
        fxProblems += app.fx.set != 0 || loaded != 6 || app.fx.on[3] || !app.fx.on[5]; // 3 and their "(128)" copies
        setSceneEffect(app, true, 3);
        int lo = 255, hi = 0;
        for (int f = 0; f < 600; ++f) {
            stepSceneEffects(app);
            lo = std::min(lo, app.fx.ch[0].alpha);
            hi = std::max(hi, app.fx.ch[0].alpha);
            fxProblems += app.fx.timerA[5] < 0 || app.fx.timerA[5] > 79 || app.fx.timerA[3] > 400 || app.fx.timerB[3] > 440;
        }
        fxProblems += lo != 55 || hi != 255 || app.fx.ch[2].alpha != 135; // UpdateRandom: 55 <-> 255; channel 2 stays
        app.lastTicks = 0;
        drawFrame(app, false);
        ++frames;
        setSceneEffect(app, false, 5);
        fxProblems += app.fx.on[5];
        std::printf("scene effects: set %d (%d textures), snow on, fog added, snow's fog alpha %d..%d; %d problems\n", app.fx.set,
                    loaded, lo, hi, fxProblems);
        problems += fxProblems;
        endSceneEffects(app);
        app.video.paintings = keepPaintings;
        mapErrors += problems;
        bootTitle(app);
        app.runner.stop();
    }
    { // The puzzle tutorial (Uty_TutoTexDisp): entering stage 0 from a dungeon, its pages turned as a
      // player would (advanceDialogue): the picture's pages follow; the frames step
        int problems = 0, maxPage = -1, framesSeen = 0, calls0 = app.tutoCalls;
        bool shot = false;
        app.runner.stop();
        app.game = cr::GameState();
        app.mapNumber = -1;
        app.game.setFlag(3070, true);
        cr::WarpRequest w;
        w.valid = true, w.area = cr::kPuzzleArea, w.map = cr::kFirstPuzzleMap;
        performWarp(app, w, -1);
        for (int f = 0; f < 4000 && (app.runner.running() || app.pendingEntry != App::Entry::None || app.tutoTex.on); ++f) {
            stepEvents(app);
            if (app.tutoTex.on) {
                maxPage = std::max(maxPage, app.tutoTex.page);
                framesSeen = std::max(framesSeen, app.tutoTex.frame + 1);
                if (!shot && app.tutoTex.page >= 0 && app.tutoTex.frame >= 1) {
                    shot = true;
                    app.lastTicks = 0;
                    drawFrame(app, false);
                    if (const char* path = SDL_getenv("CR_TUTO_SHOT")) saveFrame(app, path);
                }
            }
            if (f % 60 == 59 && !app.dialogue.empty()) advanceDialogue(app); // a page read, then turned
            if (app.choiceFlag >= 0) answerChoice(app, true);
            app.tapPending = true;
        }
        const bool ran = app.tutoCalls > calls0;
        problems += !ran || maxPage < 1 || framesSeen < 2 || app.tutoTex.on || app.renderer.takeErrors();
        std::printf("puzzle tutorial: %s, pages shown up to %d, frames stepped %d, ended %s; %d problems\n",
                    ran ? "started" : "NOT started", maxPage, framesSeen, app.tutoTex.on ? "NO" : "yes", problems);
        mapErrors += problems;
        app.runner.stop();
        app.game = cr::GameState();
        app.puzzle = App::Puzzle();
    }
    { // Music: the AAC decoder (the system's FAAD2 or the bundled copy) decodes a whole track
        int64_t frames = 0;
        uint64_t hash = 0;
        std::string err;
#ifdef CR_HAVE_FAAD2
        if (app.vitaOnly) std::printf("music: the Vita's (no AAC tracks)\n");
        else {
        const bool ok = cr::decodeTrack(app.dataDir, 10, frames, hash, &err) && frames > 1000000;
        mapErrors += !ok;
        std::printf("music: track 10 decoded %s (%lld frames)\n", ok ? "ok" : ("FAILED: " + err).c_str(), (long long)frames);
        }
#else
        std::printf("music: built without an AAC decoder\n");
#endif
    }
    if (app.vita.isOpen()) { // --vita: the Vita's scripts (eight languages), voices (sorted names = 2.2.0's
                             // clips) and music (cr_mNN.at9 for bgmNN, looping between its smpl points)
        int problems = 0;
        int languages = 0;
        for (int l = 0; l < cr::kLanguages; ++l) languages += app.systemText.hasLanguage(l);
        problems += languages != cr::kLanguages;
        if (app.sound.voiceClips(false) == 0 && app.vitaVoice[0].isOpen()) startSound(app), vitaAudio(app); // (tests run before it)
        const bool voices = app.sound.vitaVoices() && app.sound.voiceClips(false) == 2900 && app.sound.voiceClips(true) == 1220;
        problems += !voices;
        int decoded = 0, rate = 0, ch = 0;
        std::vector<uint8_t> f;
        std::vector<int16_t> pcm;
        for (int b = 0; b < 2; ++b) {
            const std::vector<std::string> names = app.vitaVoice[b].names();
            if (names.size() > 1 && app.vitaVoice[b].read(names[1], f) && cr::decodeAt9(f, pcm, rate, ch) && rate == 48000 && ch == 1 && pcm.size() > 4800)
                ++decoded;
        }
        problems += decoded != 2;
        cr::At9File track;
        int64_t frames = 0;
        const bool music = app.vitaBgm.read("cr_m01.at9", f) && cr::parseAt9(f, track) && track.loopStart > 0 &&
                           track.loopEnd > track.loopStart && cr::decodeAt9(f, pcm, rate, ch) && (frames = int64_t(pcm.size()) / std::max(ch, 1)) == track.samples;
        problems += !music;
        std::printf("vita assets: %d languages, voices %s (%d / 2 clips decoded), track 1 %s (%lld frames, loop %.3f..%.3f s); %d problems\n",
                    languages, voices ? "yes" : "NO", decoded, music ? "decoded" : "NOT decoded", (long long)frames,
                    double(track.loopStart) / 48000, double(track.loopEnd) / 48000, problems);
        mapErrors += problems;
    }
    { // The scripts' small natives: 2018 the pair's HP / MP, 2024 / 2026 the music by var, 2025 the shop
      // ended without its music put back, 2027 the pair, 2028 the loading spinner; the shop's music
        int problems = 0;
        ViewerHost host(app);
        const cr::GameState keepGame = app.game;
        const int keepChar = app.playerChar;
        app.playerChar = 2; // the second pair
        for (auto& m : app.game.party) m.hp = 1, m.mp = 1;
        host.recoverPair();
        const bool healed = app.game.party[2].hp < 0 && app.game.party[3].mp < 0 && app.game.party[0].hp == 1;
        const bool pair = host.pairNo() == 1;
        host.playMusic(7);
        const bool bgm = host.bgmNo() == 7;
        host.playMusic(1u << 27 | 7); // a fade-out isn't a track
        const bool fadeKept = host.bgmNo() == 7;
        app.game.vars[0] = 7;
        openShop(app);
        const bool shopTune = host.bgmNo() == 0x13 && app.shop.bgmBack == 7;
        closeShop(app);
        const bool back = host.bgmNo() == 7;
        openShop(app);
        host.endMenu(2);
        const bool ended = !app.shop.open && host.bgmNo() == 0x13; // SetShopBgmNoRecovery
        // 2030 / 2031: queued outside a battle, the next battle's fighter 1 / 0 starts with the four ups
        host.battleParamUp(1);
        const bool queued = app.paramUp[1] && !app.paramUp[0];
        app.game = keepGame;
        startBattle(app, {1, 0, -1, 0}, 0);
        bool boosted = false;
        if (app.battle.active) {
            const cr::Fighter& f1 = app.battle.rules.f[1];
            boosted = f1.status[cr::kStatusAttackUp] == 6 && f1.status[cr::kStatusDefenseUp] == 6 &&
                      f1.status[cr::kStatusSpeedUp] == 6 && f1.status[cr::kStatusLuckUp] == 6 &&
                      app.battle.rules.f[0].status[cr::kStatusAttackUp] == 0 && !app.paramUp[1];
            host.battleParamUp(0); // in a battle: at once
            boosted &= app.battle.rules.f[0].status[cr::kStatusSpeedUp] == 6;
            endBattle(app, false);
        }
        problems += !queued || !boosted;
        // 2029 / 2008: the logos once a run; the pad's picture while flag 0 is set
        app.logoSeen = false;
        const bool logoOnce = !host.logoSeen() && host.logoSeen();
        app.game.setFlag(0, true);
        host.padArt();
        app.lastTicks = 0;
        drawFrame(app, false);
        if (const char* path = SDL_getenv("CR_PAD_SHOT")) saveFrame(app, path);
        app.game.setFlag(0, false);
        drawFrame(app, false);
        const bool padGone = !app.padArt;
        problems += !logoOnce || !padGone || app.renderer.takeErrors();
        std::printf("natives: battle stat ups (2030 / 2031) %s\n", queued && boosted ? "ok" : "WRONG");
        app.runner.stop(); // (a script of the run before might turn it off: flag 4086 clear)
        host.nowLoading(true);
        const int frame0 = app.nowLoading.frame;
        for (int f = 0; f < 3; ++f) stepEvents(app);
        const bool spin = app.nowLoading.on && app.nowLoading.frame == ((frame0 + 3) & 15);
        app.lastTicks = 0;
        drawFrame(app, false);
        host.nowLoading(false);
        problems += !healed || !pair || !bgm || !fadeKept || !shopTune || !back || !ended || !spin || app.renderer.takeErrors();
        std::printf("natives: pair recovery %s, pair number %s, music by number %s, shop music %s / put back %s / kept by EndMenu %s, "
                    "loading spinner %s; %d problems\n", healed ? "ok" : "WRONG", pair ? "ok" : "WRONG", bgm && fadeKept ? "ok" : "WRONG",
                    shopTune ? "on" : "OFF", back ? "yes" : "NO", ended ? "yes" : "NO", spin ? "turns" : "STILL", problems);
        mapErrors += problems;
        app.game = keepGame;
        app.playerChar = keepChar;
        playBgm(app, 1u << 29);
    }
    { // The staff roll (Util 7), fast: the credits run out, the picture reaches its bottom, the panel
      // slides away, the end card waits for a tap, then it all closes
        int problems = 0;
        openStaffRoll(app, 0);
        App::StaffRoll& st = app.staffRoll;
        int lines = 0;
        for (int e = 0; e < staffLines(app); ++e) lines += staffLine(app, e).size() >= 3;
        const bool tex = st.tex.id != 0;
        st.fast = true;
        int frames = 0, shot = 0;
        while (st.open && st.state != 7 && frames < 3000) {
            st.fast = true; // (drawFrame reads the keys)
            stepStaffRoll(app);
            ++frames;
            if (frames == 150) { // a look mid-way
                app.lastTicks = 0;
                drawFrame(app, false);
                const char* path = SDL_getenv(shot == 0 ? "CR_STAFF_SHOT_A" : "CR_STAFF_SHOT_B");
                if (path && shot < 2) saveFrame(app, path);
                ++shot;
            }
        }
        for (int f = 0; f < 4; ++f) stepStaffRoll(app); // the end card faded in, waiting
        if (const char* path = SDL_getenv("CR_STAFF_SHOT_B")) {
            app.lastTicks = 0;
            drawFrame(app, false);
            saveFrame(app, path);
        }
        const bool ended = st.state == 7 && st.picY == staffPicEnd(st) && st.panel < staffroll::kPanelEnd && st.next > staffLines(app) &&
                           st.win[4] == 1.0f;
        staffRollTap(app);
        for (int f = 0; f < 20 && st.open; ++f) stepStaffRoll(app);
        problems += !tex || lines < 100 || !ended || st.open || app.renderer.takeErrors();
        std::printf("staff roll: picture %s, %d of block 9's %d lines with text, end card after %d fast frames %s, closed %s; %d problems\n",
                    tex ? "read" : "MISSING", lines, staffLines(app), frames, ended ? "reached" : "NOT reached",
                    st.open ? "NO" : "yes", problems);
        mapErrors += problems;
    }
    { // The last battle's pair choice (Util 8): four rounds, each pair once; a No goes back to the
      // plates; the camp menu opens for the chosen pair; flag 1 clears after the fourth
        int problems = 0;
        const cr::GameState keepGame = app.game;
        const int keepChar = app.playerChar;
        app.game.setFlag(1, true);
        App::LastBattle& lb = app.lastBattle;
        lb.count = lb.mask = 0;
        auto stepTo = [&](int state) { for (int f = 0; f < 60 && lb.open && lb.state != state; ++f) stepLastBattle(app); };
        auto look = [&](const char* var) {
            if (const char* path = SDL_getenv(var)) {
                for (int f = 0; f < 4; ++f) stepLastBattle(app);
                app.lastTicks = 0;
                drawFrame(app, false);
                saveFrame(app, path);
            }
        };
        std::vector<int> order;
        bool campFor = true, backOk = true, texOk = true, healed = true;
        for (int round = 0; round < 4; ++round) {
            app.game.party[3].hp = 1;
            openLastBattle(app);
            texOk &= lb.tex.id != 0;
            healed &= app.game.party[3].hp < 0;
            stepTo(3);
            if (round == 0) look("CR_LASTBT_SHOT_A");
            const int left = int(lb.pairs.size());
            backOk &= left == 4 - round;
            lastBattleChoose(app, left - 1); // the last one listed
            stepTo(7);
            campFor &= app.campOpen && app.playerChar == 2 * lb.chosen;
            app.campOpen = false;
            stepTo(10);
            if (round == 0) {
                look("CR_LASTBT_SHOT_B");
                lastBattleAnswer(app, false); // No: the plates again, the same pairs
                stepTo(3);
                backOk &= lb.state == 3 && int(lb.pairs.size()) == left;
                lastBattleChoose(app, left - 1);
                stepTo(7);
                app.campOpen = false;
                stepTo(10);
            }
            const int chosen = lb.chosen;
            lastBattleAnswer(app, true);
            stepTo(-1);
            order.push_back(chosen);
            problems += lb.open || app.game.vars[0] != chosen;
        }
        bool leaders = true; // the plates name the leaders, not the partners (entry 0x1e + 2p)
        for (int p = 0; p < 4; ++p)
            leaders &= !lastBattlePlate(app, p).empty() && lastBattlePlate(app, p) != sysText(app, 58030 + 2 * p);
        problems += !leaders;
        const bool reset = lb.count == 0 && lb.mask == 0 && !app.game.flag(1);
        std::sort(order.begin(), order.end());
        problems += !texOk || !healed || !campFor || !backOk || !reset || order != std::vector<int>{0, 1, 2, 3} || app.renderer.takeErrors();
        std::printf("last battle: 4 rounds, each pair once %s, its camp menu %s, No back to the plates %s, HP / MP restored %s, "
                    "flag 1 and the rounds clear %s, plates name the leaders %s; %d problems\n", order == std::vector<int>{0, 1, 2, 3} ? "yes" : "NO",
                    campFor ? "opens" : "DOESN'T open", backOk ? "yes" : "NO", healed ? "yes" : "NO", reset ? "yes" : "NO", leaders ? "yes" : "NO", problems);
        mapErrors += problems;
        app.game = keepGame;
        app.playerChar = keepChar;
    }
    { // The Break changing hands (btlord_chk -1, btlbreak_effect_*): the effect plays 41 frames,
      // the battle waiting; half way the pair takes the Break at 4
        int problems = 0;
        const cr::GameState keepGame = app.game;
        app.game = cr::GameState();
        cr::giveStartingItems(app.game);
        showMap(app, 0, 0, 2);
        app.pendingEntry = App::Entry::None;
        app.runner.stop();
        startBattle(app, {1, 0, -1, 0}, 0);
        App::Battle& b = app.battle;
        int frames = 0, gaugeAt = -1;
        bool waited = true;
        if (b.active) {
            for (int f = 0; f < 60 && b.phase != App::Battle::Command; ++f) stepBattle(app);
            b.phase = App::Battle::Acting, b.turnOn = false;
            b.rules.breakStarted = true;
            b.rules.breakGauge = b.rules.breakTarget = 0;
            b.rules.breakFlags |= 0x400 | 0x800; // the enemies lost it
            startBreakFx(app);
            for (; frames < 200 && b.breakFx.on; ++frames) {
                const int round = b.rules.round;
                stepBattle(app);
                waited &= !b.turnOn && b.rules.round == round;
                if (gaugeAt < 0 && b.rules.breakGauge == 4) gaugeAt = frames;
                if (frames == 20 && SDL_getenv("CR_BREAK_SHOT")) {
                    app.lastTicks = 0;
                    drawFrame(app, false);
                    saveFrame(app, SDL_getenv("CR_BREAK_SHOT"));
                }
            }
            endBattle(app, false);
        }
        problems += frames != 41 || gaugeAt != 19 || !waited || b.rules.breakChanging();
        std::printf("break effect: %d frames, the pair holds 4 from frame %d, the battle waited %s; %d problems\n", frames,
                    gaugeAt, waited ? "yes" : "NO", problems);
        mapErrors += problems;
        app.runner.stop();
        app.game = keepGame;
    }
    { // The iPhone layout (2.2.0's screens): a 3:2 screen, the layout's safe area filling it, taps
      // mapped back; the field menu's corner button where 2.2.0 has it, and the converted screens
      // drawing without render API errors
        int problems = 0;
        const cr::GameState keepGame = app.game;
        app.video.layout = 2;
        applyVideo(app);
        cr::Renderer& r = app.renderer;
        drawFrame(app, false); // (the frame maps the 3:2 screen)
        float x0 = 32, y0 = 64, x1 = 992 + r.uiX(), y1 = 704;
        x0 += r.uiX();
        r.layoutToScreen(x0, y0);
        r.layoutToScreen(x1, y1);
        const bool frame = r.phoneLayout() && std::fabs(r.screenW() / cr::Renderer::kLogicalH - 1.5f) < 0.01f;
        const bool fills = std::fabs(x0) < 0.5f && std::fabs(y0) < 0.5f && std::fabs(x1 - r.screenW()) < 0.5f &&
                           std::fabs(y1 - cr::Renderer::kLogicalH) < 0.5f;
        float bx = 500, by = 300;
        r.layoutToScreen(bx, by);
        r.screenToLayout(bx, by);
        const bool inverse = std::fabs(bx - 500) < 0.01f && std::fabs(by - 300) < 0.01f;
        app.game = cr::GameState();
        cr::giveStartingItems(app.game);
        showMap(app, 0, 0, 2);
        app.pendingEntry = App::Entry::None;
        app.runner.stop();
        int errs = 0;
        // the corner button: 2.2.0's box (440, 0, 40, 40) - tex2d -> layout -> screen -> pixels
        float cx = 2.0f * (440 + 20) + 32 + r.uiX(), cy = 2.0f * 20 + 64;
        r.layoutToScreen(cx, cy);
        const float kx = float(r.frameWidth()) / r.screenW();
        app.fieldMenu = App::FieldMenu();
        app.fieldMenu.fade = 320;
        const float ox = (float(r.frameWidth()) - r.screenW() * std::min(kx, float(r.frameHeight()) / cr::Renderer::kLogicalH)) / 2;
        const float sc = std::min(kx, float(r.frameHeight()) / cr::Renderer::kLogicalH);
        const float oy = (float(r.frameHeight()) - cr::Renderer::kLogicalH * sc) / 2;
        const bool corner = fieldMenuClick(app, ox + cx * sc, oy + cy * sc) && app.fieldMenu.open;
        app.fieldMenu = App::FieldMenu();
        openCamp(app);
        const int entries = campEntries(app);
        app.lastTicks = 0;
        drawFrame(app, false);
        errs += r.takeErrors();
        app.campOpen = false;
        startBattle(app, {1, 0, -1, 0}, 0);
        for (int f = 0; f < 40 && app.battle.active; ++f) stepBattle(app);
        app.lastTicks = 0;
        drawFrame(app, false);
        errs += r.takeErrors();
        if (app.battle.active) endBattle(app, false);
        showMessages(app, {57006});
        drawFrame(app, false);
        errs += r.takeErrors();
        app.dialogue.clear();
        openScenarios(app); // 2.2.0's chart scrolls sideways too, to -120, and down to its own end
        for (int k = 0; k < 5; ++k) scenariosKey(app, SDLK_RIGHT);
        for (int k = 0; k < 8; ++k) scenariosKey(app, SDLK_PAGEDOWN);
        const bool chart = app.scenariosHScroll == -120 && app.scenariosScroll == 2 * kPhoneHistoryLowest;
        drawFrame(app, false);
        errs += r.takeErrors();
        closeScenarios(app);
        problems += !frame || !fills || !inverse || !corner || entries != 5 || !chart || errs;
        std::printf("iphone layout: 3:2 screen %s, the safe area fills it %s, taps map back %s, the corner button %s, "
                    "camp %d plates, History scrolls both ways %s, %d render API errors; %d problems\n", frame ? "yes" : "NO",
                    fills ? "yes" : "NO", inverse ? "yes" : "NO", corner ? "opens" : "DOESN'T open", entries,
                    chart ? "yes" : "NO", errs, problems);
        mapErrors += problems;
        app.video.layout = 1;
        applyVideo(app);
        app.runner.stop();
        app.game = keepGame;
    }
    { // Widescreen (16:9): the screens' left- and right-hand groups at the screen's edges, taps
      // following them - the field's corner button, the Camp's plates, the title's, the save
      // screen's - and full-screen pictures covering the whole width
        int problems = 0;
        const cr::GameState keepGame = app.game;
        const App::Video keepVideo = app.video;
        app.video.widescreen = true, app.video.layout = 1, app.video.display = 0;
        app.video.width = 1280, app.video.height = 720;
        applyVideo(app);
        if (app.window) SDL_SyncWindow(app.window);
        cr::Renderer& r = app.renderer;
        app.lastTicks = 0;
        drawFrame(app, false);
        const float ux = r.uiX();
        float l0x, l0y, l1x, l1y; // logical <-> the frame's pixels (straight lines off the iPhone layout)
        r.toLogical(0, 0, l0x, l0y);
        r.toLogical(1000, 1000, l1x, l1y);
        auto pixel = [&](float lx, float ly, float& px, float& py) {
            px = (lx - l0x) * 1000 / (l1x - l0x), py = (ly - l0y) * 1000 / (l1y - l0y);
        };
        auto tap = [&](float origin, float mx, float my, float& px, float& py) { pixel(origin + 2 * mx, 2 * my, px, py); };
        float px, py;
        app.game = cr::GameState();
        cr::giveStartingItems(app.game);
        showMap(app, 0, 0, 2);
        app.pendingEntry = App::Entry::None;
        app.runner.stop();
        app.fieldMenu = App::FieldMenu();
        app.fieldMenu.fade = 320;
        tap(wideRight(app), 456 + 20 + 16, -32 + 20 + 32, px, py); // the corner button's box (tex2d + (16, 32))
        const bool corner = fieldMenuClick(app, px, py) && app.fieldMenu.open;
        app.fieldMenu = App::FieldMenu();
        openCamp(app);
        app.camp.anim = 100;
        tap(wideRight(app), campui::kPlateX + 60, campui::kPlateY + 20, px, py); // Items
        campClick(app, px, py);
        const bool camp = app.camp.page == App::Camp::Items;
        app.lastTicks = 0;
        drawFrame(app, false);
        int errs = r.takeErrors();
        app.campOpen = false;
        openSaveScreen(app, false);
        tap(wideRight(app), 328 + 60, 136 + 48.0f * float(savePlates(app).empty() ? 0 : savePlates(app)[0]) + 20, px, py);
        saveScreenClick(app, px, py);
        const bool save = app.saveScreen.phase == 1; // asking "Save data?"
        drawFrame(app, false);
        errs += r.takeErrors();
        app.saveScreen.open = false;
        app.titleOpen = true;
        app.titleCursor = 2;
        tap(wideRight(app), 328 + 60, 114 + 20, px, py); // Start's plate
        const int before = app.titleCursor;
        titleClick(app, px, py);
        const bool title = app.titleCursor == 0 && before == 2;
        app.titleOpen = false;
        app.saveScreen.open = false; // (Start went on to the save / load screen)
        if (app.charSelectOpen) closeCharSelect(app, 0);
        app.optionsOpen = false;
        problems += ux <= 0 || !corner || !camp || !save || !title || errs;
        std::printf("widescreen: the screen %.0f wide, the corner button %s, Camp's Items %s, the save plate %s, the title's Start %s, "
                    "%d render API errors; %d problems\n", r.screenW(), corner ? "opens" : "DOESN'T open", camp ? "yes" : "NO",
                    save ? "yes" : "NO", title ? "yes" : "NO", errs, problems);
        mapErrors += problems;
        app.video = keepVideo;
        applyVideo(app);
        if (app.window) SDL_SyncWindow(app.window);
        app.runner.stop();
        app.game = keepGame;
    }
    { // A controller, as the Vita's buttons: a virtual one - the stick walks the leader, △ opens the
      // Camp and closes it, ○ confirms, × goes back (and does nothing on the field), the D-pad
      // moves the cursor; ×-confirm swaps the two
        int problems = 0;
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = 6, desc.nbuttons = 15;
        desc.axis_mask = (1u << 6) - 1, desc.button_mask = (1u << 15) - 1; // (the gamepad's own order)
        desc.name = "selftest pad";
        const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
        SDL_Gamepad* pad = id ? SDL_OpenGamepad(id) : nullptr;
        SDL_Joystick* js = pad ? SDL_GetGamepadJoystick(pad) : nullptr;
        if (!pad) {
            std::printf("controller: no virtual controller here (%s), skipped\n", SDL_GetError());
        } else {
            const cr::GameState keepGame = app.game;
            SDL_Gamepad* keepPad = app.pad;
            app.pad = pad;
            app.game = cr::GameState();
            cr::giveStartingItems(app.game);
            showMap(app, 0, 0, 2);
            app.pendingEntry = App::Entry::None;
            app.runner.stop();
            app.dialogue.clear();
            const cr::Vec3 from = app.walker.pos;
            SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_LEFTX, 32767);
            SDL_UpdateJoysticks();
            for (int f = 0; f < 15; ++f) updatePlayer(app, 1.0f / kFramesPerSecond, 0);
            SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_LEFTX, 0);
            SDL_UpdateJoysticks();
            const cr::Vec3 d = app.walker.pos - from;
            const bool walked = std::sqrt(cr::dot(d, d)) > 1.0f;
            app.walker.pos = from;
            app.runner.stop(); // (anything the walk set off)
            app.dialogue.clear();
            app.pendingEntry = App::Entry::None;
            padButton(app, SDL_GAMEPAD_BUTTON_SOUTH); // × on the field: nothing
            const bool fieldQuiet = !app.campOpen && app.havePlayer;
            padButton(app, SDL_GAMEPAD_BUTTON_NORTH); // △: the Camp
            const bool opened = app.campOpen;

            const int c0 = app.camp.cursor;
            padButton(app, SDL_GAMEPAD_BUTTON_DPAD_UP);
            const bool moved = app.camp.cursor != c0;
            app.camp.cursor = 0;
            padButton(app, SDL_GAMEPAD_BUTTON_EAST); // ○: Items
            const bool confirmed = app.camp.page == App::Camp::Items;
            padButton(app, SDL_GAMEPAD_BUTTON_SOUTH); // ×: back to the plates
            const bool backed = app.camp.page == App::Camp::Main && app.campOpen;
            bool swapRow = false; // Options, Game: "Swap ○ and ×" On - the other way round
            for (const OptRow& o : optionRows(app, 0))
                if (o.label.rfind("Swap", 0) == 0 && o.act) { o.act(0); swapRow = true; }
            swapRow = swapRow && !app.padConfirmEast;
            app.camp.cursor = 0;
            padButton(app, SDL_GAMEPAD_BUTTON_SOUTH);
            const bool swapped = swapRow && app.camp.page == App::Camp::Items;
            for (const OptRow& o : optionRows(app, 0))
                if (o.label.rfind("Swap", 0) == 0 && o.act) o.act(1); // Off: as on the Vita
            app.padConfirmEast = true;
            padButton(app, SDL_GAMEPAD_BUTTON_NORTH); // △ again: closed, from any page
            const bool closed = !app.campOpen;
            problems += !walked || !fieldQuiet || !opened || !moved || !confirmed || !backed || !swapped || !closed;
            std::printf("controller: the stick walks %s, △ opens / closes the Camp %s / %s, the D-pad moves %s, ○ confirms %s, × back %s "
                        "(on the field nothing %s), ×-confirm %s; %d problems\n", walked ? "yes" : "NO", opened ? "yes" : "NO",
                        closed ? "yes" : "NO", moved ? "yes" : "NO", confirmed ? "yes" : "NO", backed ? "yes" : "NO",
                        fieldQuiet ? "yes" : "NO", swapped ? "yes" : "NO", problems);
            app.campOpen = false;
            app.pad = keepPad;
            app.game = keepGame;
            SDL_CloseGamepad(pad);
        }
        if (id) SDL_DetachVirtualJoystick(id);
        mapErrors += problems;
    }
    { // btl_lose: an ordinary battle lost goes back to the pair's Ark, a fifth of the Oz gone
        int problems = 0;
        const cr::GameState keepGame = app.game;
        const int keepChar = app.playerChar;
        app.game = cr::GameState();
        cr::giveStartingItems(app.game);
        app.game.oz = 1005;
        app.playerChar = 2; // Zhamo and Eluca: area 13
        showMap(app, 0, 0, 2);
        app.pendingEntry = App::Entry::None;
        app.runner.stop();
        startBattle(app, {1, 0, -1, 0}, 0);
        const bool ordinary = app.battle.active && !app.battle.eventBattle;
        if (app.battle.active) loseToArk(app);
        const bool ark = app.area == 13 && app.mapNumber == 103 && !app.battle.active;
        const bool oz = app.game.oz == 1005 - 201;
        problems += !ordinary || !ark || !oz;
        std::printf("battle lost: back to the Ark (area %d, room %d) %s, Oz %d (a fifth gone %s); %d problems\n", app.area,
                    app.mapNumber, ark ? "yes" : "NO", app.game.oz, oz ? "yes" : "NO", problems);
        mapErrors += problems;
        app.runner.stop();
        app.pendingEntry = App::Entry::None;
        app.game = keepGame;
        app.playerChar = keepChar;
    }
    { // PrcMsg's typing: a character a frame, a tap brings the page out whole and the next turns
      // it; 0x1a pauses it; 0x14 closes the message by itself (j005_d05's 5203: five pauses, then
      // closes after the last)
        int problems = 0;
        const cr::EventFile keepEvents = app.events;
        const bool keepType = app.typeOn;
        app.typeOn = true;
        app.dialogue.clear();
        showMessages(app, {57006}); // a page with no codes
        const int chars = app.dialogue.empty() ? 0 : pageChars(app.dialogue[0]);
        for (int f = 0; f < 10; ++f) stepTyping(app);
        const bool rate = chars > 10 && app.dialogueTyped == 10 && !pageTyped(app);
        tapDialogue(app);
        const bool whole = pageTyped(app) && !app.dialogue.empty();
        tapDialogue(app);
        const bool turned = app.dialogue.empty();
        problems += !rate || !whole || !turned;
        int frames = 0, taps = 0, pauses = 0, pauseFrames = 0, total = 0, closeAfter = -2;
        bool closed = false;
        if (app.events.load(app.dataDir, 18)) {
            showMessages(app, {5203});
            for (size_t k = 0; k < app.dialogueInfo.size() && k < app.dialogue.size(); ++k) // (one after the last
                for (const auto& pz : app.dialogueInfo[k].pauses)                           // character is the closing wait)
                    if (pz.at < pageChars(app.dialogue[k])) ++pauses, pauseFrames += pz.frames;
            for (const auto& pg : app.dialogue) total += pageChars(pg);
            closeAfter = app.dialogueInfo.empty() ? -2 : app.dialogueInfo.back().closeAfter;
            const size_t pages = app.dialogue.size();
            for (; frames < 4000 && !app.dialogue.empty(); ++frames) {
                stepTyping(app);
                if (!app.dialogue.empty() && pageTyped(app) && app.dialoguePage + 1 < app.dialogue.size()) {
                    tapDialogue(app); // the reader turns each page once it's out
                    ++taps;
                }
            }
            closed = app.dialogue.empty() && size_t(taps) + 1 == pages;
        }
        const int least = total + pauseFrames + std::max(0, closeAfter);
        problems += pauses == 0 || closeAfter <= 0 || !closed || frames < least || frames > least + 2 * (taps + 2);
        std::printf("message typing: a character a frame %s, a tap brings it out %s, the next turns it %s; 5203: %d "
                    "characters, %d pauses (%d frames), then closes by itself %d frames after the last %s in %d frames; %d problems\n",
                    rate ? "yes" : "NO", whole ? "yes" : "NO", turned ? "yes" : "NO", total, pauses, pauseFrames, closeAfter,
                    closed ? "yes" : "NO", frames, problems);
        mapErrors += problems;
        app.dialogue.clear();
        app.events = keepEvents;
        app.typeOn = keepType;
    }
    { // The calcs' queued messages (btl_add_msgque / menub_mainPut): a monster's poison queues
      // "<name> was poisoned!" for its target (one per fighter); the banner shows each 60 frames
      // once free and the battle waits for the queue before the next action
        int problems = 0;
        const cr::GameState keepGame = app.game;
        app.game = cr::GameState();
        cr::giveStartingItems(app.game);
        showMap(app, 0, 0, 2);
        app.pendingEntry = App::Entry::None;
        app.runner.stop();
        startBattle(app, {1, 0, -1, 0}, 0);
        App::Battle& b = app.battle;
        bool queued = false, oneEach = false, waited = true, shown = false;
        int held = 0, nextAt = -1;
        if (b.active) {
            for (int f = 0; f < 60 && b.phase != App::Battle::Command; ++f) stepBattle(app);
            b.rules.forceAction = 122; // enemy 2 casts poison
            cr::BattleCommand cmd[2];
            cmd[0].kind = cmd[1].kind = cr::kCmdDefend;
            b.rules.beginRound(cmd, app.battleActions);
            cr::BattleTurn t;
            while (b.rules.nextTurn(app.battleActions, t) && t.action != 122) {}
            int target = -1;
            for (const cr::BattleHit& h : t.hits) target = h.target;
            queued = t.action == 122 && target >= 0 && t.notices.size() == 1 && t.notices[0].fighter == target &&
                     t.notices[0].entry == 6 && b.rules.f[size_t(target)].status[cr::kStatusPoison] == 5;
            t.notices.push_back({target, 0x19}); // the same fighter again: dropped
            t.notices.push_back({2, 0x1d});
            b.phase = App::Battle::Acting;
            b.turn = t;
            b.turnOn = false;
            b.bannerFrames = 0;
            showHits(app);
            oneEach = b.notices.size() == 2;
            const std::string want = target >= 0 ? sysText(app, 57006, battleActor(app, target) ? battleActor(app, target)->name : "") : "";
            for (int f = 0; f < 400 && nextAt < 0; ++f) {
                const bool before = !b.notices.empty() || b.bannerFrames > 1;
                stepBattle(app);
                if (b.banner == want && b.bannerFrames > 0) shown = true, ++held;
                if (b.turnOn || b.phase != App::Battle::Acting) nextAt = f;
                else if (!before) break;
                waited &= !(b.turnOn && before);
            }
            endBattle(app, false);
        }
        problems += !queued || !oneEach || !shown || held != btlui::kMessageFrames || !waited || nextAt < 2 * btlui::kMessageFrames;
        std::printf("battle messages: poison queued for its target %s, one per fighter %s, shown %d frames, the next action after "
                    "frame %d, waited %s; %d problems\n", queued ? "yes" : "NO", oneEach ? "yes" : "NO", held, nextAt,
                    waited ? "yes" : "NO", problems);
        mapErrors += problems;
        app.runner.stop();
        app.game = keepGame;
    }
    { // The World Select (Util 1): the gates the pair has reached in its order, the level bands,
      // "Skip the first half?" where its flag is set, Back, and the sixth gate without bands
        int problems = 0;
        const cr::GameState keepGame = app.game;
        const int keepChar = app.playerChar;
        App::WorldSelect& ws = app.worldSelect;
        auto look = [&](const char* var) {
            if (const char* path = SDL_getenv(var)) {
                app.lastTicks = 0;
                drawFrame(app, false);
                saveFrame(app, path);
            }
        };
        auto settle = [&] { for (int f = 0; f < 12 && app.worldChoiceOpen; ++f) stepWorldSelect(app); };
        auto finish = [&] { for (int f = 0; f < 30 && app.worldChoiceOpen; ++f) stepWorldSelect(app); };
        app.game = cr::GameState();
        app.playerChar = 0;
        for (int f : {1076, 1077, 1123, 1052}) app.game.setFlag(f, true); // gates 0, 1; one band more; a pip
        openWorldSelect(app);
        settle();
        look("CR_WORLD_SHOT_A");
        const bool offered = ws.worlds == std::vector<int>{0, 1} && ws.levels == 3;
        ws.cursor = 1;
        worldSelectChoose(app, 1);
        settle();
        const bool bands = ws.state == 6;
        look("CR_WORLD_SHOT_B");
        worldSelectLevel(app, 2);
        settle();
        const bool asked = ws.state == 8;
        look("CR_WORLD_SHOT_C");
        worldSelectAnswer(app, true);
        finish();
        const bool chosen = !app.worldChoiceOpen && app.game.vars[0] == 1 && app.game.vars[1] == 2 && app.game.vars[2] == 0 &&
                            !app.game.flag(0) && !app.game.flag(1);
        openWorldSelect(app); // Back
        settle();
        worldSelectChoose(app, -1);
        finish();
        const bool back = !app.worldChoiceOpen && app.game.flag(0);
        app.game.setFlag(1122, true); // the first gate asks to skip its first half
        openWorldSelect(app);
        settle();
        worldSelectChoose(app, 0);
        settle();
        const bool skipAsked = ws.state == 4 && ws.asking;
        worldSelectAnswer(app, true);
        settle();
        worldSelectLevel(app, 0);
        settle();
        worldSelectAnswer(app, true);
        finish();
        const bool skipped = app.game.vars[0] == 0 && app.game.flag(1) && !app.game.flag(0);
        app.game.setFlag(1104, true); // the sixth gate
        openWorldSelect(app);
        settle();
        const int last = int(ws.worlds.size()) - 1;
        worldSelectChoose(app, last);
        settle();
        const bool sixthAsked = ws.state == 8;
        worldSelectAnswer(app, true);
        finish();
        const bool sixth = last >= 0 && app.game.vars[0] == 5 && app.game.vars[1] == 0 && !app.game.flag(1);
        app.game = cr::GameState(); // the second pair's order: gate 1 first
        app.playerChar = 2;
        for (int f : {1476, 1477}) app.game.setFlag(f, true);
        openWorldSelect(app);
        const bool order = ws.worlds == std::vector<int>{1, 0};
        answerWorldChoice(app, 0);
        const bool second = app.game.vars[0] == 1 && app.game.vars[2] == 1;
        problems += !offered + !bands + !asked + !chosen + !back + !skipAsked + !skipped + !sixthAsked + !sixth + !order + !second;
        problems += app.renderer.takeErrors();
        std::printf("world select: gates offered %s, bands %s, okay? %s, chosen %s, Back %s, skip asked %s and kept %s, "
                    "the sixth %s / %s, the second pair's order %s / %s; %d problems\n",
                    offered ? "yes" : "NO", bands ? "yes" : "NO", asked ? "yes" : "NO", chosen ? "yes" : "NO",
                    back ? "yes" : "NO", skipAsked ? "yes" : "NO", skipped ? "yes" : "NO", sixthAsked ? "yes" : "NO",
                    sixth ? "yes" : "NO", order ? "yes" : "NO", second ? "yes" : "NO", problems);
        mapErrors += problems;
        app.game = keepGame;
        app.playerChar = keepChar;
    }
    { // A script's camera in a 3D room (setCamera3D): map 113's 1024 x 1024 stage view, doubled,
      // centred on the 768-row screen - the target at the screen's centre, the frustum's top edge
      // 640 rows above the screen, its sides 512 past the 1024 layout's edges
        int problems = 0;
        const cr::StageCamera keep = app.stageCam;
        app.stageCam.fovDegrees = 53.9f, app.stageCam.viewWidth = app.stageCam.viewHeight = 1024;
        const cr::Vec3 eye{0, 10, 50}, tgt{0, 10, 0};
        const cr::Mat4 vp = scriptCameraView(app, eye, tgt);
        auto toScreen = [&](const cr::Vec3& p, float& x, float& y) {
            const float* m = vp.m;
            const float cx = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], cy = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
            const float cw = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
            x = (cx / cw * 0.5f + 0.5f) * app.renderer.screenW();
            y = (0.5f - cy / cw * 0.5f) * cr::Renderer::kLogicalH;
        };
        const float t = std::tan(53.9f * 3.14159265f / 360.0f);
        float x0, y0, x1, y1, x2, y2;
        toScreen(tgt, x0, y0);
        toScreen({0, 10 + 50 * t, 0}, x1, y1);   // the frustum's top edge at the target's distance
        toScreen({50 * t, 10, 0}, x2, y2);       // its right edge
        const float cx = app.renderer.uiX() + 512;
        problems += std::fabs(x0 - cx) > 0.5f || std::fabs(y0 - 384) > 0.5f;
        problems += std::fabs(y1 - (-640)) > 0.5f || std::fabs(x2 - (cx + 1024)) > 0.5f;
        std::printf("script camera: target at (%.1f, %.1f), top edge at row %.1f, right edge at %.1f; %d problems\n", x0 - app.renderer.uiX(),
                    y0, y1, x2 - app.renderer.uiX(), problems);
        app.stageCam = keep;
        mapErrors += problems;
    }
    { // The battles' talks: the first battle's tutorial (EvrStart 2, var 1 = 0xff1) holds the
      // battle and hides its HUD until its pages are read; an event battle's script waits behind
      // the gate and goes on after it (j001_d04 script 201: on to map 63, NpcMdlRead 508 and its Born);
      // the 1st Gate boss (group 260, j001_d05 script 201) talks after round 3 and again once a
      // pair attack lands after it, can't fall before that and heals whole each round meanwhile
        int problems = 0;
        const cr::GameState keepGame = app.game;
        const int keepChar = app.playerChar;
        App::Battle& b = app.battle;
        auto look = [&](const char* var) {
            if (const char* path = SDL_getenv(var)) {
                app.lastTicks = 0;
                drawFrame(app, false);
                saveFrame(app, path);
            }
        };
        auto mapIndex = [&](const char* name) {
            const auto it = std::find(app.maps.begin(), app.maps.end(), std::string(name));
            return it == app.maps.end() ? -1 : int(it - app.maps.begin());
        };
        auto playScript = [&](int area, int map, int script) {
            app.battle.active = false;
            app.dialogue.clear();
            showMap(app, mapIndex(cr::kMapCollisionFile[map]), map, area);
            app.pendingEntry = App::Entry::None;
            app.runner.stop();
            app.runner.setGate(-1);
            return app.runner.start(app.events, script, app.game, app.area, app.mapNumber);
        };
        auto readOn = [&](int f) { // pages turn, item windows close
            if (!app.dialogue.empty() && f % 3 == 2) advanceDialogue(app);
            if (app.choiceFlag >= 0) answerChoice(app, true);
            app.tapPending = true;
        };
        // 1. The first battle's tutorial
        app.testNoTutorials = false;
        app.game = cr::GameState();
        app.playerChar = 0;
        cr::giveStartingItems(app.game);
        showMap(app, 0, 0, 2);
        app.pendingEntry = App::Entry::None;
        app.runner.stop();
        app.puzzleBanner = App::PuzzleBanner(); // (an earlier test's)
        startBattle(app, {1, 0, -1, 0}, 0);
        int talkFrames = 0, pagesSeen = 0, pageFrames = 0;
        bool tutorialSeen = false;
        for (int f = 0; f < 2000 && b.active; ++f) {
            stepBattle(app);
            if (battletalk::on(app)) {
                ++talkFrames;
                pagesSeen = std::max(pagesSeen, int(app.dialogue.size()));
                if (!app.dialogue.empty() && ++pageFrames == 12) look("CR_TALK_SHOT_A"); // the first page and its picture
                if (pageFrames >= 12) readOn(f);
            } else if (talkFrames > 0) {
                tutorialSeen = app.game.flag(0xff1) && !app.runner.running() && b.phase == App::Battle::Command;
                break;
            }
        }
        problems += !tutorialSeen || pagesSeen == 0;
        if (b.active) endBattle(app, false);
        // 2. An event battle: the script goes on after it
        app.game = cr::GameState();
        cr::giveStartingItems(app.game);
        app.game.setFlag(0xff1, true);
        bool waited = true, continued = false, bossIn = false, gated = false;
        if (playScript(5, 47, 201)) {
            int f = 0;
            for (; f < 2000 && !b.active && app.runner.running(); ++f) stepEvents(app), readOn(f);
            gated = b.active && b.eventBattle && app.runner.running() && app.runner.gate() == battletalk::kFlag;
            const std::string where = app.runner.where();
            for (int k = 0; k < 90 && b.active; ++k) stepBattle(app);
            waited = app.runner.running() && app.runner.where() == where;
            if (b.active) {
                for (int k = 2; k < 5; ++k) b.rules.f[size_t(k)].hp = 0;
                endBattle(app, true);
            }
            for (f = 0; f < 4000 && app.runner.running() && !b.active; ++f) {
                stepEvents(app);
                animateNpcs(app, 1.0f / 30.0f);
                readOn(f);
                if (const cr::FieldNpc* n = findNpc(app, 508); n && npcActive(app, *n) && app.mapNumber == 63 && !bossIn) {
                    bossIn = true;
                    look("CR_TALK_SHOT_B");
                }
                continued |= app.mapNumber == 63;
            }
        }
        problems += !gated + !waited + !continued + !bossIn;
        if (b.active) endBattle(app, false);
        app.runner.stop();
        // 3. The 1st Gate boss's talks
        app.game = cr::GameState();
        cr::giveStartingItems(app.game);
        app.game.setFlag(0xff1, true);
        int talk1 = 0, talk2 = 0, heldAt1 = 0, healedWhole = 0, lateRound = 0;
        bool bossWon = false, scriptEnded = false;
        if (playScript(6, 64, 201)) {
            for (int f = 0; f < 4000 && !b.active && app.runner.running(); ++f) stepEvents(app), readOn(f);
            if (b.active && b.start.group == 260) {
                for (int k = 0; k < 2; ++k) b.rules.f[size_t(k)].hp = b.rules.f[size_t(k)].hpMax = 999999;
                bool shot = false;
                for (int f = 0; f < 30000 && b.active; ++f) {
                    if (battletalk::on(app)) {
                        bool up = false;
                        for (const App::Portrait& pt : app.portraits) up |= pt.tex.id && pt.alpha > 0.99f;
                        if (!shot && !app.dialogue.empty() && up) look("CR_TALK_SHOT_C"), shot = true;
                        readOn(f);
                    } else if (b.phase == App::Battle::Command && b.menu == App::Battle::Top) { // the pair attacks
                        battleChooseTop(app, 1);
                        battleChooseCommand(app, 0);
                        if (b.menu == App::Battle::Targets && !b.picks.empty()) battleChooseTarget(app, b.picks[0]);
                    } else if (b.phase == App::Battle::Results) {
                        bossWon = true;
                        endBattle(app, true);
                        break;
                    }
                    const int hpBefore = b.rules.f[2].hp, roundBefore = b.rules.round;
                    stepBattle(app);
                    if (!talk1 && app.game.flag(1231)) talk1 = b.rules.round;
                    if (!talk2 && app.game.flag(1232)) talk2 = b.rules.round;
                    if (b.rules.lateAttack && !lateRound) lateRound = b.rules.round;
                    if (!app.game.flag(1232) && b.rules.f[2].hp == 1) ++heldAt1;
                    if (roundBefore == b.rules.round && b.phase == App::Battle::Command && hpBefore < b.rules.f[2].hpMax &&
                        b.rules.f[2].hp == b.rules.f[2].hpMax && !app.game.flag(1232))
                        ++healedWhole;
                }
                for (int f = 0; f < 3000 && app.runner.running() && !b.active; ++f) stepEvents(app), readOn(f);
                scriptEnded = !app.runner.running();
            }
        }
        problems += talk1 != 3 || talk2 <= 3 || lateRound <= 3 || !bossWon || !scriptEnded || healedWhole == 0;
        if (b.active) endBattle(app, false);
        app.runner.stop();
        app.runner.setGate(-1);
        problems += app.renderer.takeErrors();
        std::printf("battle talks: tutorial %s (%d pages, %d frames held), event battle %s, waits %s, goes on to map 63 %s, "
                    "NPC 508 read and born %s; group 260 talks after rounds %d and %d (pair attack late in round %d), held at 1 HP "
                    "%d frames, healed whole %d times, won %s, script ended %s; %d problems\n",
                    tutorialSeen ? "shown" : "NOT shown", pagesSeen, talkFrames, gated ? "gated" : "NOT gated",
                    waited ? "yes" : "NO", continued ? "yes" : "NO", bossIn ? "yes" : "NO", talk1, talk2, lateRound, heldAt1,
                    healedWhole, bossWon ? "yes" : "NO", scriptEnded ? "yes" : "NO", problems);
        mapErrors += problems;
        app.testNoTutorials = true;
        app.game = keepGame;
        app.playerChar = keepChar;
    }
    { // The NPC model table's story models (197..304: the scenes' bosses and the party's battle
      // models - rows 501.. of every dungeon and town use 198, 200..214): each loads
        int loaded = 0, failed = 0, animated = 0, absent = 0;
        for (int k = 197; k < cr::kNumNpcModels; ++k) {
            const std::string file = cr::kNpcModelFile[k];
            if (app.npcModels.count(file)) { ++loaded; continue; }
            if (app.vitaOnly ? !app.vita.contains(file) : !SDL_GetPathInfo((app.dataDir + file).c_str(), nullptr)) { ++absent; continue; } // 197: not shipped, unused
            LoadedModel lm;
            loadModelFile(app, file, lm, true);
            if (lm.meshes.empty()) ++failed;
            else ++loaded, animated += !lm.actions.empty();
            unloadLoaded(app, lm);
        }
        mapErrors += failed;
        std::printf("NPC model table: %d story models (197..%d) load, %d with fitting actions, %d not in the data, %d failed\n",
                    loaded, cr::kNumNpcModels - 1, animated, absent, failed);
    }
    { // eff3d on the field over the whole run: the puzzle rooms', the footsteps' dust, the scripts'
        // The dust: walk the leader on the maps in turn until a step lands on a floor with a sound
        int stepMap = -1, dust = 0, steps = 0;
        for (size_t m = 0; m < app.maps.size() && stepMap < 0; ++m) {
            app.runner.stop();
            showMap(app, int(m));
            app.runner.stop();
            app.pendingEntry = App::Entry::None;
            if (!app.havePlayer || app.player.actions.size() < 2) continue;
            const int before = app.fieldEffectCount[0];
            const size_t sounds = app.soundLog.size();
            app.walker.gait = cr::Gait::Walk;
            for (int f = 0; f < 60; ++f) {
                const int want = std::min(int(app.walker.gait), int(app.player.actions.size()) - 1);
                if (want != app.player.action) app.player.action = want, app.player.time = 0, app.footPrev = 0;
                animate(app, app.player, 1.0f / 30.0f);
                playFootsteps(app);
                stepEffects(app, app.fieldEffects);
            }
            app.walker.gait = cr::Gait::Stand;
            dust = app.fieldEffectCount[0] - before;
            steps = int(app.soundLog.size() - sounds);
            if (dust > 0) stepMap = int(m);
        }
        mapErrors += dust == 0 || dust != steps;
        std::printf("footstep dust: %d puffs for %d step sounds on %s\n", dust, steps,
                    stepMap >= 0 ? app.maps[size_t(stepMap)].c_str() : "no map");
        std::string ids;
        int kinds = 0;
        for (int i = 0; i < 100; ++i)
            if (app.fieldEffectCount[size_t(i)]) ids += (kinds++ ? " " : "") + std::to_string(i) + "x" + std::to_string(app.fieldEffectCount[size_t(i)]);
        const int fxProblems = !app.fieldEffectCount[0] + !app.fieldEffectCount[0xc] + !app.fieldEffectCount[5];
        std::printf("field effects: %d kinds started (%s); FrmPlayEff %d, Uty_eff3d_set_fld %d, script quakes %d; %d problems\n",
                    kinds, ids.c_str(), app.frameEffectCalls, app.scriptEffectCalls, app.scriptQuakes, fxProblems);
        std::printf("pictures: TexDisp %d, TutoTexDisp %d (pages up to %d), the \"!\" %d\n", app.texDispCalls, app.tutoCalls,
                    app.tutoPage, app.exclamationCalls);
        std::string props;
        for (const auto& [name, n] : app.matAnimDraws) props += " " + name + "x" + std::to_string(n);
        std::printf("props with animated materials drawn:%s\n", props.empty() ? " none" : props.c_str());
        mapErrors += fxProblems;
    }
    std::printf("selftest: %zu models, %zu maps (%d with a stage camera), %d frames, %d render API errors in %d models, "
                "%d map problems; player spawned on %d maps, walked > 5 units on %d\n",
                app.models.size(), app.maps.size(), camMaps, frames, glErrors, failedModels, mapErrors, spawned, walked);
    glErrors += mapErrors;
    return glErrors ? 1 : 0;
}

// A new game (route 1): the Ark's hall (j001_t01, map 97) with only the new-game flag set; its
// script 0 plays the opening.
void startNewGame(App& app) {
    app.game = cr::GameState();
    // Set by the title's new-game path: t01's script 0 tests it to play the opening (event 201's
    // scene, gosub 0x5f98) instead of treating the opening as seen
    app.game.setFlag(4089, true);
    cr::giveStartingItems(app.game); // prty_init_new: the Human Genes, 3 Emergency Exits, 2 Chocolates
    app.treasureBoxes.clear();
    app.entryArea = -1;
    const int area = 7, map = 97;
    const auto it = std::find(app.maps.begin(), app.maps.end(), std::string(cr::kMapCollisionFile[map]));
    if (it != app.maps.end()) showMap(app, int(it - app.maps.begin()), map, area);
}

// The title, as the game boots: area 0 (j001_t99) and its EvrStart 0 - the logos, the title
// picture and music, then the title menu. Also "Return to Title".
void bootTitle(App& app) {
    app.trackerArmed = false; // (SaveData_Load_init: a load or a new game starts it again)
    endSceneEffects(app); // MuCom_PreReturnTitle: ScnEffectEnd
    app.renderer.resetHistory(); // a cut: the upscaler starts over
    app.runner.stop();
    unloadModel(app);
    if (app.battle.active) endBattle(app, false);
    app.runner.stop();
    app.haveStageCam = false;
    app.havePlayer = false;
    app.npcs.clear();
    app.npcDraws.clear();
    app.dialogue.clear();
    app.dialogueInfo.clear();
    app.choiceFlag = -1;
    app.worldChoiceOpen = false;
    app.optionsOpen = false;
    for (App::Portrait& p : app.portraits) {
        app.renderer.destroy(p.tex);
        p = App::Portrait();
    }
    app.game = cr::GameState();
    app.treasureBoxes.clear();
    app.mode = Mode::Maps;
    app.showBackground = true;
    app.area = 0;
    app.mapNumber = cr::kAreaFirstMap[0];
    app.entryArea = 0;
    app.pendingEntry = App::Entry::None;
    app.musicArea = -1;
    playBgm(app, 1u << 29); // the script starts the title music
    std::string err;
    if (!app.events.load(app.dataDir, 0, &err)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "title: %s", err.c_str());
        return;
    }
    app.booting = true;
    app.bootClock = 0;
    app.titleOpen = false;
    if (const uint32_t at = app.events.returnEntry(0))
        if (app.runner.startAt(app.events, at, 0, app.game, 0, app.mapNumber)) app.runnerWhat = "title";
    updateTitle(app);
}

// --explore n: plays a new game by itself - talks to everyone and walks into every trigger not yet
// tried with the current story flags (people first, exits last), answering yes and winning
// battles - and logs how far the story gets and what it asked for that isn't there yet.
int runExplore(App& app, int maxActions) {
    app.persist = false;
    app.typeOn = false;
    setMode(app, Mode::Maps);
    startNewGame(app);
    int totalFrames = 0, pagesRead = 0;
    auto settle = [&](int limit) {
        int f = 0;
        for (; f < limit && (app.runner.running() || app.pendingEntry != App::Entry::None); ++f) {
            stepEvents(app);
            if (!app.dialogue.empty()) { pagesRead += int(app.dialogue.size()); app.dialogue.clear(); }
            if (app.choiceFlag >= 0) answerChoice(app, true);
            if (app.worldChoiceOpen) answerWorldChoice(app, 0);
            if (app.itemWindowOpen) app.game.setFlag(0, true);
            app.tapPending = true;
        }
        totalFrames += f;
        return f;
    };
    auto flagCount = [&] {
        int c = 0;
        for (uint32_t w : app.game.flags) c += __builtin_popcount(w);
        return c;
    };
    auto stateKey = [&] {
        uint64_t h = 1469598103934665603ull;
        for (uint32_t w : app.game.flags) h = (h ^ w) * 1099511628211ull;
        return h;
    };
    settle(20000);
    std::set<std::tuple<int, int, uint64_t>> tried;
    std::set<int> mapsSeen{app.mapNumber};
    int step = 0, stuckReason = 0;
    for (; step < maxActions; ++step) {
        // candidates: people / objects (kind bit 3), then touch boxes that don't warp, then exits
        struct Cand { int id; bool examine; int rank; };
        std::vector<Cand> cands;
        // a dry run on a copy of the state tells where a script leads: new rooms first, back last
        auto rankOf = [&](int id, int base) {
            cr::GameState trial = app.game;
            const cr::EventResult r = cr::runEvent(app.events, id, trial, app.area, app.mapNumber);
            if (!r.warp.valid) return base;
            return mapsSeen.count(r.warp.map) ? 5 : 2;
        };
        for (size_t i = 0; i < app.npcs.size(); ++i) {
            const cr::FieldNpc& n = app.npcs[i];
            if (!npcActive(app, n)) continue;
            const bool parked = (n.pos[1] >= 900.0f || (n.pos[0] == 0 && n.pos[1] == 0 && n.pos[2] == 0)) &&
                                !app.placedNpcs.count(n.id);
            if ((n.kind & 8) && !parked) cands.push_back({n.id, true, rankOf(n.treasure & 3 ? n.id + 1 : n.id, 0)});
            if (n.touchTrigger()) cands.push_back({n.id, false, rankOf(n.id, 1)});
        }
        std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.rank < b.rank; });
        const uint64_t key = stateKey();
        const Cand* pick = nullptr;
        for (const Cand& c : cands)
            if (!tried.count({app.mapNumber, c.id * 2 + c.examine, key})) { pick = &c; break; }
        if (!pick) { stuckReason = 1; break; }
        tried.insert({app.mapNumber, pick->id * 2 + pick->examine, key});
        const int fromMap = app.mapNumber, flagsBefore = flagCount(), pagesBefore = pagesRead;
        const size_t unsupportedBefore = app.unsupported.size();
        const cr::FieldNpc* n = findNpc(app, pick->id);
        if (!n || !app.havePlayer) continue;
        const char* traceStep = SDL_getenv("CR_EXPLORE_TRACE"); // debugging: every instruction of one step
        if (traceStep && SDL_atoi(traceStep) == step)
            cr::setEventTrace([](size_t pc, int op) { std::printf("    pc %#zx op %d\n", pc, op); });
        bool started;
        if (pick->examine) {
            app.walker.pos = {n->pos[0] + n->box[0] / 2 + app.walker.radius, n->pos[1], n->pos[2]};
            started = interact(app);
        } else {
            app.walker.pos = {n->boxCenterX(), n->pos[1], n->boxCenterZ()};
            app.lastKick = 0;
            checkTriggers(app);
            started = app.runner.running();
        }
        const int frames = settle(20000);
        cr::setEventTrace(nullptr);
        if (!started) continue;
        mapsSeen.insert(app.mapNumber);
        std::string extra;
        for (size_t u = unsupportedBefore; u < app.unsupported.size(); ++u) extra += ", " + app.unsupported[u];
        if (app.runner.running()) extra += ", STILL RUNNING (" + app.runner.where() + ")";
        std::printf("%4d map %3d %s %4d: %+d flags, %d pages, %d frames%s%s\n", step, fromMap,
                    pick->examine ? "talk " : "touch", pick->id, flagCount() - flagsBefore, pagesRead - pagesBefore,
                    frames, app.mapNumber != fromMap ? (" -> map " + std::to_string(app.mapNumber)).c_str() : "",
                    extra.c_str());
        if (app.runner.running()) { app.runner.stop(); stuckReason = 2; break; }
    }
    std::printf("explore: %d actions, %d frames, %d pages, %zu maps seen, %d flags set, ends on map %d (%s)\n", step,
                totalFrames, pagesRead, mapsSeen.size(), flagCount(), app.mapNumber,
                stuckReason == 1 ? "nothing new to try" : stuckReason == 2 ? "a script never finished" : "action limit");
    std::map<std::string, int> counts;
    for (const std::string& u : app.unsupported) counts[u.substr(0, u.find(" on map"))]++;
    for (const auto& [what, c] : counts) std::printf("  not implemented: %s x%d\n", what.c_str(), c);
    return 0;
}

// --capture <map.cmp> <out.bmp>: renders that map's composite (background + collision through
// the stage camera, no HUD) and saves the frame — for checking alignment without a person.
// --export-textures <dir>: every distinct picture the game draws (its .mct files and the textures
// inside model packages) as <hash>.crraw, for tools/hd_textures.py to upscale into <hash>.crhd.
// With --vita, the Vita's lossless version of each picture it has alike (same size, alpha kept, the
// same picture: within 12/255 a channel on average - not the art it redrew) is exported instead,
// and <ios hash>.cralias (the Vita's hash, 16 hex digits) lets the one upscale serve the iOS
// version too: the game finds it whichever it decoded
int runExportTextures(App& app, const char* dirArg) {
    std::string dir = dirArg;
    if (dir.back() != '/') dir += '/';
    SDL_CreateDirectory(dir.c_str());
    std::set<uint64_t> seen;
    int written = 0, failed = 0, dupes = 0, fromVita = 0, aliases = 0;
    auto put = [&](const cr::Image& img) {
        const uint64_t h = cr::imageHash(img);
        if (!seen.insert(h).second) {
            ++dupes;
            return h;
        }
        const std::string path = dir + cr::hashName(h) + ".crraw";
        if (SDL_GetPathInfo(path.c_str(), nullptr)) return h; // already exported
        if (cr::saveRawImage(path, img)) ++written;
        else ++failed;
        return h;
    };
    // The Vita's picture for the iOS one, when it is the same picture losslessly
    auto alike = [](const cr::Image& ios, const cr::Image& vita) {
        if (ios.alphaOnly || ios.width != vita.width || ios.height != vita.height || ios.rgba.size() != vita.rgba.size()) return false;
        double sum = 0;
        for (size_t i = 0; i < ios.rgba.size(); ++i) sum += std::abs(int(ios.rgba[i]) - int(vita.rgba[i]));
        return sum / double(ios.rgba.size()) < 12;
    };
    auto pair = [&](const cr::Image& ios, const cr::Image* vita) {
        if (!vita || !alike(ios, *vita)) { // (the art the Vita redrew: both, each for its own)
            put(ios);
            if (vita) put(*vita);
            return;
        }
        const uint64_t v = put(*vita), i = cr::imageHash(ios);
        ++fromVita;
        if (i == v) return;
        const std::string path = dir + cr::hashName(i) + ".cralias", target = cr::hashName(v);
        if (SDL_SaveFile(path.c_str(), target.data(), target.size())) ++aliases;
    };
    for (const std::string& name : app.textures) {
        cr::Image ios, vita;
        std::vector<uint8_t> data;
        const bool haveIos = !app.vitaOnly && cr::readFile(app.dataDir + name, data) && cr::decodeMct(data, ios);
        bool haveVita = false;
        if (app.vita.isOpen() && name.size() > 4) {
            std::vector<uint8_t> g;
            haveVita = app.vita.read(name.substr(0, name.size() - 4) + ".gxt", g) && cr::decodeGxt(g, vita);
        }
        if (haveIos) pair(ios, haveVita ? &vita : nullptr);
        else if (haveVita) put(vita);
        else ++failed;
    }
    if (app.vita.isOpen() && !app.vitaOnly) // the Vita's own pictures (its warning screen, the titles in other languages ...)
        for (const std::string& n : app.vita.names()) {
            if (n.size() < 5 || n.compare(n.size() - 4, 4, ".gxt") != 0) continue;
            if (std::find(app.textures.begin(), app.textures.end(), n.substr(0, n.size() - 4) + ".mct") != app.textures.end()) continue;
            std::vector<uint8_t> g;
            cr::Image img;
            if (app.vita.read(n, g) && cr::decodeGxt(g, img)) put(img);
        }
    for (const std::string& name : app.models) {
        std::vector<uint8_t> data;
        cr::Model ios, vita;
        const bool haveIos = !app.vitaOnly && cr::readFile(app.dataDir + name, data) && cr::loadModelPackage(data, ios, nullptr);
        const bool haveVita = app.vita.isOpen() && app.vita.read(name, data) && cr::loadModelPackage(data, vita, nullptr);
        if (haveIos) {
            const bool paired = haveVita && vita.textures.size() == ios.textures.size();
            for (size_t k = 0; k < ios.textures.size(); ++k) pair(ios.textures[k], paired ? &vita.textures[k] : nullptr);
            if (haveVita && !paired)
                for (const cr::Image& img : vita.textures) put(img);
        } else if (haveVita)
            for (const cr::Image& img : vita.textures) put(img);
    }
    if (app.vita.isOpen() && !app.vitaOnly) // the Vita's own model packages (t0101_1..3.bin)
        for (const std::string& n : app.vita.names()) {
            if (n.size() < 5 || n.compare(n.size() - 4, 4, ".bin") != 0) continue;
            if (std::find(app.models.begin(), app.models.end(), n) != app.models.end()) continue;
            std::vector<uint8_t> data;
            cr::Model m;
            if (app.vita.read(n, data) && cr::loadModelPackage(data, m, nullptr))
                for (const cr::Image& img : m.textures) put(img);
        }
    std::printf("export: %zu distinct pictures (%d written, %d already there, %d repeats, %d failed; %d from the Vita, %d "
                "iOS hashes aliased to them) -> %s\n", seen.size(), written, int(seen.size()) - written - failed, dupes,
                failed, fromVita, aliases, dir.c_str());
    return failed ? 1 : 0;
}

int runCapture(App& app, const char* mapName, const char* outPath) {
    app.persist = false;
    app.typeOn = SDL_getenv("CR_CAPTURE_TYPED") != nullptr; // CR_CAPTURE_TYPED=n: n frames of typing
    setMode(app, Mode::Maps);
    int idx = -1;
    for (size_t i = 0; i < app.maps.size(); ++i)
        if (app.maps[i] == mapName) idx = int(i);
    if (idx < 0) {
        std::printf("unknown map %s\n", mapName);
        return 1;
    }
    if (app.captureTitlePair) { // --title-new p: through the title as a player would
        app.saveDirOverride = userDir() + "capture-empty/"; // (no saves: New)
        bootTitle(app);
        for (int f = 0; f < 1000 && !app.titleOpen; ++f) stepEvents(app);
        // (CR_TITLE_NEW_STOP=chars / save: stays on Character Select / the save screen)
        const char* stop = SDL_getenv("CR_TITLE_NEW_STOP");
        const std::string stopAt = stop ? stop : "";
        titleChoose(app, 0);
        for (int f = 0; f < 300 && !app.charSelectOpen; ++f) stepEvents(app);
        app.charSelectCursor = app.captureTitlePair - 1;
        if (stopAt != "chars") {
            charSelectKey(app, SDLK_RETURN);
            for (int f = 0; f < 300 && !app.saveScreen.open; ++f) stepEvents(app);
            if (stopAt != "save") {
                saveScreenKey(app, SDLK_RETURN);
                saveScreenKey(app, SDLK_RETURN);
                app.saveDirOverride.clear();
            } else app.saveScreen.anim = 100; // (faded in: a capture draws one frame)
        } else app.charSelectAnim = 100;
    } else if (app.captureTitle) bootTitle(app); // --title: the boot script, --frames n into it
    else if (app.newGame) startNewGame(app);
    else {
        for (int f : app.captureFlags) app.game.setFlag(f, true);
        const std::vector<int> numbers = cr::mapNumbersFor(mapName);
        if (app.captureArea >= 0 && !numbers.empty()) showMap(app, idx, numbers[0], app.captureArea);
        else showMap(app, idx);
    }
    app.showBackground = true;
    app.hideHud = SDL_getenv("CR_CAPTURE_HUD") == nullptr; // (CR_CAPTURE_HUD=1: as the build shows it)
    if (app.captureEvent >= 0) { // --event n: as if something in the room had started it
        app.pendingEntry = App::Entry::None;
        app.runner.stop();
        if (!app.runner.start(app.events, app.captureEvent, app.game, app.area, app.mapNumber))
            std::printf("area %d has no script %d\n", app.area, app.captureEvent);
    }
    const bool traceRunner = SDL_getenv("CR_CAPTURE_TRACE") != nullptr; // the script's state each frame it changes
    std::string lastWhere;
    for (int f = 0; f < app.captureFrames; ++f) { // --frames: play the scripts, reading each page for 45 frames
        if (app.battle.active && app.captureWinBattles) { // its talks play; then it's won
            stepBattle(app);
            if (traceRunner && f % 100 == 0)
                std::printf("frame %d: battle phase %d frames %d flag %d\n", f, int(app.battle.phase), app.battle.phaseFrames,
                            int(app.game.flag(battletalk::kFlag)));
            if (traceRunner && app.battle.phaseFrames == 1)
                std::printf("frame %d: battle %d phase %d%s\n", f, app.battle.start.group, int(app.battle.phase),
                            battletalk::on(app) ? " talk" : "");
            if (!battletalk::on(app) && app.battle.phase == App::Battle::Command) {
                for (int k = 2; k < 5; ++k) app.battle.rules.f[size_t(k)].hp = 0;
                endBattle(app, true);
            }
            if (f % 45 == 44 && !app.dialogue.empty()) advanceDialogue(app);
            continue;
        }
        stepEvents(app);
        if (traceRunner) {
            const std::string where = (app.runner.running() ? app.runner.where()
                                                             : "done (" + std::to_string(app.runner.result().instructions) +
                                                                   " instructions" +
                                                                   (app.runner.result().error.empty() ? "" : ", " + app.runner.result().error) +
                                                                   ")") + " map " +
                                      std::to_string(app.mapNumber) + " (" + app.runnerWhat + ")" +
                                      (app.battle.active ? " battle " + std::to_string(app.battle.start.group) + " phase " +
                                                               std::to_string(int(app.battle.phase)) +
                                                               (battletalk::on(app) ? " talk" : "")
                                                         : std::string()) +
                                      " gate " + std::to_string(app.runner.gate());
            if (where != lastWhere) std::printf("frame %d: %s\n", f, where.c_str());
            lastWhere = where;
        }
        animateNpcs(app, 1.0f / 30.0f);
        if (f % 45 == 44 && f + 1 < app.captureFrames) {
            if (app.choiceFlag >= 0) answerChoice(app, true);
            else if (app.worldChoiceOpen) answerWorldChoice(app, 0);
            else advanceDialogue(app);
            app.tapPending = true;
            if (app.itemWindowOpen) app.game.setFlag(0, true);
        }
    }
    if (app.captureTalk >= 0) { // the opening to its end, then the hall's people until a portrait is up
        for (int f = 0; f < 20000 && (app.runner.running() || app.pendingEntry != App::Entry::None); ++f) {
            stepEvents(app);
            app.dialogue.clear();
            if (app.choiceFlag >= 0) answerChoice(app, true);
            app.tapPending = true;
        }
        int found = -1;
        std::vector<int> ids;
        for (const cr::FieldNpc& n : app.npcs)
            if ((n.kind & 8) && npcActive(app, n) && n.pos[1] < 900.0f) ids.push_back(n.id);
        for (int id : ids) {
            const cr::FieldNpc* n = findNpc(app, id);
            if (!n || !app.havePlayer) continue;
            app.walker.pos = {n->pos[0] + n->box[0] / 2 + app.walker.radius, n->pos[1], n->pos[2]};
            if (!interact(app)) continue;
            for (int f = 0; f < 6000 && app.runner.running(); ++f) {
                stepEvents(app);
                bool up = false;
                for (const App::Portrait& p : app.portraits) up = up || (p.tex.id && p.alpha > 0.99f && p.t >= p.frames && p.aT >= p.aFrames);
                if (up && !app.dialogue.empty() && ++found == app.captureTalk) break;
                if (!app.dialogue.empty()) advanceDialogue(app);
                if (app.choiceFlag >= 0) answerChoice(app, true);
                app.tapPending = true;
            }
            if (found >= app.captureTalk) {
                std::printf("talk-portrait: npc %d\n", id);
                break;
            }
            app.runner.stop();
        }
        if (found < app.captureTalk) std::printf("talk-portrait: only %d moments with a portrait\n", found + 1);
    }
    if (app.vita.isOpen()) std::printf("vita textures used: %d\n", app.vitaTextures);
    if (app.video.hdTextures) std::printf("hd textures: %d replaced, %d without one (%s)\n", app.hdLoaded, app.hdMissing, app.hdDir.c_str());
    if (app.captureTitle) {
        std::printf("title: script %s, title menu %s, fade grey %.2f alpha %.2f", app.runner.running() ? app.runner.where().c_str() : "done",
                    app.titleOpen ? "open" : "closed", app.fadeGrey, screenFadeAlpha(app));
        for (int i = 0; i < 16; ++i)
            if (app.portraits[i].tex.id) std::printf(", slot %d image %d (%d x %d) alpha %.2f at (%.0f %.0f)", i, app.portraits[i].image, app.portraits[i].tex.width,
                                                    app.portraits[i].tex.height, app.portraits[i].alpha, app.portraits[i].x, app.portraits[i].y);
        std::printf("\n");
    }
    if (app.captureFrames) {
        const size_t pi = std::min(app.dialoguePage, app.dialogue.size() ? app.dialogue.size() - 1 : 0);
        const cr::MessageInfo mi = pi < app.dialogueInfo.size() ? app.dialogueInfo[pi] : cr::MessageInfo();
        int shown = 0;
        for (const App::Portrait& p : app.portraits) shown += p.tex.id && p.alpha > 0.5f;
        std::printf("after %d frames: area %d, map %d, %s, %zu dialogue pages open (name plate %d, side %d), %d portraits shown, "
                    "fade %.0f%%\n", app.captureFrames, app.area, app.mapNumber,
                    app.runner.running() ? app.runner.where().c_str() : "scripts done", app.dialogue.size(), mi.nameId,
                    mi.side, shown, screenFadeAlpha(app) * 100);
        if (SDL_getenv("CR_CAPTURE_TRACE")) {
            const cr::StageCamera& sc = app.stageCam;
            std::printf("camera: stage %s fov %.1f view %dx%d; script %s eye (%.1f %.1f %.1f) target (%.1f %.1f %.1f)\n",
                        app.haveStageCam ? "yes" : "no", sc.fovDegrees, sc.viewWidth, sc.viewHeight,
                        app.cam.free && app.cam.set ? "on" : "off", app.cam.eye.x, app.cam.eye.y, app.cam.eye.z,
                        app.cam.target.x, app.cam.target.y, app.cam.target.z);
        }
    }
    if (app.captureSceneFx[0] >= 0) { // --scene-fx set effect [frames]: that weather / backdrop on
        loadSceneEffectSet(app, app.captureSceneFx[0]);
        setSceneEffect(app, true, app.captureSceneFx[1]);
        for (int f = 0; f < app.captureSceneFx[2]; ++f) stepSceneEffects(app);
        std::printf("scene fx: set %d, effect %d on, %d frames, drift %d rise %d\n", app.fx.set, app.captureSceneFx[1],
                    app.captureSceneFx[2], app.fx.spaceDrift, app.fx.spaceRise);
    }
    if (app.captureBattle[0] > 0) { // --battle group stage: that battle, a second in
        cr::ensureStartingGenes(app.game);
        if (app.captureBattleView == "genes" || app.captureBattleView == "genehit") { // a plate with known genes
            app.game.items[481] = 1, app.game.items[108] = 1, app.game.items[94] = 1;
            app.game.party[0].genes[1] = 481;
        }
        if (!SDL_getenv("CR_CAPTURE_TUTORIALS")) // the battles' tutorials seen (else the first one holds the view)
            for (int f : {0xff1, 0xff2, 0xff3, 0xff4, 0xff5, 0xfa8, 0xfa9}) app.game.setFlag(f, true);
        startBattle(app, {app.captureBattle[0], app.captureBattle[1], -1, 0}, 0);
        for (int f = 0; f < 30 && app.battle.active; ++f) stepBattle(app);
        // --battle-view top|commands|targets|items|escape|hit|results: a screen of it
        const std::string& v = app.captureBattleView;
        App::Battle& b = app.battle;
        for (int f = 0; f < 10 && b.active && b.phase != App::Battle::Command; ++f) stepBattle(app);
        if (v == "items") app.game.items[21] = 3, app.game.items[22] = 1, app.game.items[24] = 2;
        if (v == "commands" || v == "targets" || v == "items") battleChooseTop(app, 0);
        if (v == "targets") battleChooseCommand(app, 0);
        if (v == "items") battleChooseCommand(app, 2);
        if (v == "escape") battleChooseTop(app, 2);
        if (v == "options") battleChooseTop(app, 3), app.optionsAnim = 100;
        if (v.rfind("effect", 0) == 0) { // effect<id>[@frame]: that effect on the first enemy, some frames in
            const int id = SDL_atoi(v.c_str() + 6);
            const size_t at = v.find('@');
            const int frames = at == std::string::npos ? 8 : SDL_atoi(v.c_str() + at + 1);
            setBattleCamera(app, b.formation + 12);
            spawnEffect(app, id, 2);
            for (int f = 0; f < frames; ++f) stepBattle(app);
            std::printf("battle view %s: %zu effects playing\n", v.c_str(), b.effects.size());
        }
        if (v.rfind("stage", 0) == 0) { // stage<action>[@frame]: the first enemy's staged action, that frame of it
            const int action = SDL_atoi(v.c_str() + 5);
            const size_t at = v.find('@');
            const int frame = at == std::string::npos ? -1 : SDL_atoi(v.c_str() + at + 1);
            b.rules.forceAction = action;
            for (int f = 0; f < 6000 && b.active; ++f) {
                if (b.phase == App::Battle::Command && b.menu == App::Battle::Top) { // the pair defends
                    battleChooseTop(app, 0);
                    battleChooseCommand(app, 3);
                    if (b.menu == App::Battle::Commands) battleChooseCommand(app, 3);
                }
                if (b.turnOn && b.turn.action == action && b.turnFrame >= (frame >= 0 ? frame : b.hitFrame + 6)) break;
                if (b.phase == App::Battle::Won || b.phase == App::Battle::Lost) break;
                stepBattle(app);
            }
            std::printf("battle view %s: action %d frame %d (hit %d), camera %d (%d so far), flash %.0f, quake %.1f %.1f, "
                        "%zu effects, hp %d/%d %d/%d, user slot %d\n", v.c_str(), b.turn.action, b.turnFrame, b.hitFrame, b.cam, b.stageCams,
                        b.flash.on ? b.flash.alpha : 0.0f, b.quake.off[0], b.quake.off[1], b.effects.size(), b.rules.f[0].hp,
                        b.rules.f[0].hpMax, b.rules.f[1].hp, b.rules.f[1].hpMax,
                        battleActor(app, b.turn.actors[0]) ? battleActor(app, b.turn.actors[0])->slot : -1);
            if (SDL_getenv("CR_BATTLE_DEBUG"))
                if (const App::Battle::Actor* u = battleActor(app, b.turn.actors[0])) {
                    const LoadedModel& lm = b.models[u->model];
                    std::printf("user %s: %d built-in of %zu actions:", u->model.c_str(), lm.builtin, lm.actions.size());
                    for (const auto& ac : lm.actions) std::printf(" '%s' (%.0f)", ac.name.c_str(), ac.duration);
                    std::printf("\n");
                }
        }
        if (v == "genes" || v == "genehit") battleChooseTop(app, 0), battleChooseCommand(app, 1);
        if (v == "genehit") {
            for (size_t i = 0; i < b.picks.size(); ++i)
                if (b.picks[i] == 94) b.pick = int(i);
            battleChooseGene(app, b.picks[size_t(b.pick)]);
            if (b.menu == App::Battle::Targets) battleChooseTarget(app, b.picks[0]);
            if (b.menu == App::Battle::Commands) battleChooseCommand(app, 3); // Musiea defends
            for (int f = 0; f < 2000 && b.active; ++f) {
                if (b.turnOn && b.turn.kind == cr::kCmdGene && b.hitsShown && b.turnFrame >= b.hitFrame + 6) break;
                stepBattle(app);
            }
            std::printf("battle view genehit: escher mp %d, enemy hp %d/%d\n", b.rules.f[0].mp, b.rules.f[2].hp, b.rules.f[2].hpMax);
        }
        const int hitWanted = v.rfind("hit", 0) == 0 ? std::max(1, SDL_atoi(v.c_str() + 3)) : 0;
        int hitsSeen = 0;
        bool wasShown = false;
        if (hitWanted || v == "results") {
            // pair attacks on the first enemy standing until the first hit lands (or the battle's won)
            for (int f = 0; f < 6000 && b.active; ++f) {
                if (b.phase == App::Battle::Command && b.menu == App::Battle::Top) {
                    battleChooseTop(app, b.rules.f[0].alive() && b.rules.f[1].alive() ? 1 : 0);
                    battleChooseCommand(app, 0);
                    if (b.menu == App::Battle::Targets) battleChooseTarget(app, b.picks[0]);
                    if (b.menu == App::Battle::Commands) { battleChooseCommand(app, 0); battleChooseTarget(app, b.picks[0]); }
                }
                if (v == "results") {
                    if (b.phase == App::Battle::Results && b.phaseFrames > 10) break;
                    if (b.phase == App::Battle::Lost) break;
                } else {
                    if (b.turnOn && b.hitsShown && !wasShown) ++hitsSeen;
                    wasShown = b.turnOn && b.hitsShown;
                    if (hitsSeen >= hitWanted && b.hitsShown && b.turnFrame >= b.hitFrame + 6) break;
                    if (b.phase == App::Battle::Won || b.phase == App::Battle::Lost) break;
                }
                stepBattle(app);
            }
            std::printf("battle view %s: phase %d, round %d, break %d, hp", v.c_str(), int(b.phase), b.rules.round,
                        b.rules.breakGauge);
            for (int i = 0; i < 5; ++i) std::printf(" %d/%d", b.rules.f[size_t(i)].hp, b.rules.f[size_t(i)].hpMax);
            std::printf(", exp %d oz %d\n", b.rules.expGained, b.rules.ozGained);
        }
        if (SDL_getenv("CR_BATTLE_DEBUG"))
            for (const App::Battle::Actor& a : app.battle.actors) {
                LoadedModel& lm = app.battle.models[a.model];
                const cr::Action* act = a.action >= 0 && a.action < int(lm.actions.size()) ? &lm.actions[size_t(a.action)] : nullptr;
                cr::computeWorld(lm.cpu.skeleton, act, a.time, lm.world);
                cr::Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
                for (const auto& mesh : lm.cpu.meshes) {
                    cr::skinMesh(lm.cpu, mesh, lm.world, lm.skinned);
                    for (size_t v = 0; v + 2 < lm.skinned.size(); v += 3) {
                        lo = {std::min(lo.x, lm.skinned[v]), std::min(lo.y, lm.skinned[v + 1]), std::min(lo.z, lm.skinned[v + 2])};
                        hi = {std::max(hi.x, lm.skinned[v]), std::max(hi.y, lm.skinned[v + 1]), std::max(hi.z, lm.skinned[v + 2])};
                    }
                }
                std::printf("actor %s at (%.0f %.0f) action %d '%s' t=%.0f: posed bounds (%.1f %.1f %.1f)-(%.1f %.1f %.1f)\n",
                            a.model.c_str(), a.pos.x, a.pos.z, a.action, act ? act->name.c_str() : "-", a.time, lo.x, lo.y,
                            lo.z, hi.x, hi.y, hi.z);
            }
    }
    if (app.captureMessage > 0) showMessages(app, {app.captureMessage}); // --message <id>
    if (const char* st = SDL_getenv("CR_CAPTURE_STYLE")) app.msgStyle = SDL_atoi(st), app.msgLines = 2; // Open's style
    if (const char* pg = SDL_getenv("CR_CAPTURE_PAGE"))
        app.dialoguePage = size_t(std::clamp(SDL_atoi(pg), 0, std::max(0, int(app.dialogue.size()) - 1)));
    if (SDL_getenv("CR_CAPTURE_LINES") && !app.dialogue.empty()) // the page's lines' first bytes (tests)
        for (const std::string& l : app.dialogue[std::min(app.dialoguePage, app.dialogue.size() - 1)])
            std::printf("line: %02x %02x %02x len %zu\n", l.size() > 0 ? (unsigned char)l[0] : 0,
                        l.size() > 1 ? (unsigned char)l[1] : 0, l.size() > 2 ? (unsigned char)l[2] : 0, l.size());
    if (const char* ty = SDL_getenv("CR_CAPTURE_TYPED")) { // the page n frames into its typing
        resetTyping(app);
        for (int f = SDL_atoi(ty); f > 0 && !app.dialogue.empty(); --f) stepTyping(app);
        std::printf("typed: %d of %d characters\n", app.dialogueTyped,
                    app.dialogue.empty() ? 0 : pageChars(app.dialogue[std::min(app.dialoguePage, app.dialogue.size() - 1)]));
    }
    if (SDL_getenv("CR_CAPTURE_CHOICE")) { // the event Yes / No over it (Util 3), settled
        app.msgStyle = 64, app.msgLines = 1;
        app.choiceFlag = 0, app.choiceCursor = 0, app.choiceAnim = 4;
    }
    if (!app.captureShop.empty()) { // the Ark's shop (area 7), with some Oz and things to sell
        if (app.area != 7 && app.mapNumber >= 97) {
            const auto it = std::find(app.maps.begin(), app.maps.end(), std::string(cr::kMapCollisionFile[app.mapNumber]));
            if (it != app.maps.end()) showMap(app, int(it - app.maps.begin()), app.mapNumber, 7);
        }
        app.game.oz = 45070;
        app.game.items[21] = 3;
        app.game.items[1] = 1;
        app.game.items[202] = 1;
        openShop(app);
        App::Shop& s = app.shop;
        const std::string& p = app.captureShop; // main category buy items sell count equip talk
        if (p == "talk") {
            shopChooseMain(app, 0);
            for (int f = 0; f < 40; ++f) { stepShop(app); stepEvents(app); }
        } else if (p != "main") {
            shopChooseMain(app, p == "sell" ? 2 : 1);
            if (p != "category") shopChooseCategory(app, p == "sell" || p == "items" ? 0 : 1);
            if (p == "count" || p == "equip") {
                shopChooseItem(app, 1);
                if (p == "count") s.count = 2;
                else { s.count = 1; shopPay(app); }
            } else if (p == "buy") {
                s.listCursor = 1; // Bastard Sword: Attack up
            }
        }
        s.anim = 100;
        std::printf("shop: level %d, %zu in the list, phase %d, %d oz\n", s.level, s.list.size(), int(s.phase), app.game.oz);
    }
    if (app.puzzle.stage >= 0) {
        int counts[11] = {};
        for (const App::Puzzle::Obj& o : app.puzzle.objs) ++counts[std::clamp(o.type, 0, 10)];
        std::printf("puzzle: stage %d (map %d), ability %d, ESP %d, %zu objects: %d blocks, %d fixed, %d gems, %d switches, %d crystals\n",
                    app.puzzle.stage, app.mapNumber, cr::kPuzzleStage[app.puzzle.stage].ability, app.puzzle.esp,
                    app.puzzle.objs.size(), counts[1], counts[2], counts[9], counts[8], counts[10]);
    }
    if (!app.captureFieldMenu.empty()) { // --field-menu closed|open|known|off: the corner button, the bar
        const std::string& v = app.captureFieldMenu;
        if (v != "closed") app.fieldMenu.open = true;
        if (v == "known" || v == "off") app.game.items[170] = 1;
        if (v == "off") app.game.noEncounters[size_t(fieldRoute(app))] = true;
        app.fieldMenu.fade = 320;
        if (v.rfind("map", 0) == 0) { // map[step]: the map screen, the story at that step
            if (v.size() > 3) app.game.setFlag(mapui::kFlagBase + mapui::kFlagStride * fieldRoute(app) + SDL_atoi(v.c_str() + 3), true);
            openMapScreen(app);
            app.mapScreen.fade = 4;
            const App::MapScreen& ms = app.mapScreen;
            std::printf("map screen: map %d, gate %d, texture %dx%d, spot (%d %d), marks (%d %d) (%d %d)\n", app.mapNumber, ms.gate,
                        ms.tex.width, ms.tex.height, ms.spot[0], ms.spot[1], ms.marks[0][0], ms.marks[0][1], ms.marks[1][0],
                        ms.marks[1][1]);
        }
        std::printf("field menu: %s, %s, route %d encounters %s\n", fieldMenuWanted(app) ? "shown" : "hidden",
                    app.fieldMenu.open ? "open" : "closed", fieldRoute(app), fieldEncountersOff(app) ? "off" : "on");
    }
    if (app.captureMenu == 1) {
        openCamp(app);
        app.camp.anim = 100;
        // --camp-view items|use|equip|slots|wear|status: a page, with a bag to show
        const std::string& v = app.captureCampView;
        if (!v.empty()) {
            app.game.items[3] = 4, app.game.items[24] = 2, app.game.items[21] = 3, app.game.items[27] = 1;
            app.game.items[202] = 1, app.game.items[216] = 1, app.game.items[427] = 1;
            app.game.party[size_t(campCharacter(app, 0))].mp = 10;
            auto key = [&](SDL_Keycode k) { campKey(app, k); };
            if (v == "items" || v == "use") app.camp.cursor = 0, key(SDLK_RETURN);
            if (v == "use") key(SDLK_RIGHT), key(SDLK_LEFT), key(SDLK_RETURN), key(SDLK_RETURN), key(SDLK_RIGHT), key(SDLK_RIGHT);
            if (v == "equip" || v == "slots" || v == "wear") app.camp.cursor = 1, key(SDLK_RETURN);
            if (v == "slots" || v == "wear") key(SDLK_RETURN);
            if (v == "wear") key(SDLK_RETURN);
            if (v == "status") app.camp.cursor = 3, key(SDLK_RETURN), key(SDLK_RETURN);
            if (v == "genes" || v == "plate") app.game.items[481] = 1, app.game.items[108] = 1, app.game.items[94] = 1;
            if (v == "genes") app.camp.cursor = 2, key(SDLK_RETURN), key(SDLK_DOWN), key(SDLK_DOWN);
            if (v == "plate") {
                app.camp.cursor = 1, key(SDLK_RETURN), key(SDLK_RETURN);
                for (int k = 0; k < 4; ++k) key(SDLK_DOWN); // Gene Plate 1
                key(SDLK_RETURN);
            }
            app.camp.anim = 100;
        }
    }
    if (app.captureMenu == 7) { // --save-screen clear: the title's, pair 1 with every story cleared (a save of its own)
        app.saveDirOverride = userDir() + "capture-clear/";
        SDL_CreateDirectory(app.saveDirOverride.c_str());
        cr::SaveData sd = captureSave(app);
        for (int f : {31, 32, 34, 36, 38, 39, 40}) sd.game.setFlag(f, true);
        sd.leader = 0;
        sd.playSeconds = 3600 * 23 + 60 * 7;
        cr::writeSave(slotPath(app, 1), sd);
        app.chosenPair = 1;
        app.captureMenu = 3;
    }
    if (app.captureMenu == 2 || app.captureMenu == 3) { openSaveScreen(app, app.captureMenu == 3); app.saveScreen.anim = 100; }
    if (app.captureMenu == 4) { openCharSelect(app); app.charSelectAnim = 100; }
    if (app.captureMenu == 8) { // (stepped in: a capture draws one frame)
        openWorldSelect(app);
        for (int f = 0; f < 40 && app.worldChoiceOpen; ++f) stepWorldSelect(app);
    }
    if (app.captureMenu == 9) {
        openLastBattle(app);
        for (int f = 0; f < 40 && app.lastBattle.open; ++f) stepLastBattle(app);
    }
    if (app.captureMenu == 5) { openScenarios(app); app.scenariosAnim = 100; app.scenariosScroll = app.captureAt[0] == app.captureAt[0] ? -app.captureAt[0] : 0;
        if (app.captureAt[1] == app.captureAt[1]) scenariosScrollSideways(app, -app.captureAt[1]); }
    if (app.captureMenu == 6) {
        openManual(app);
        app.manualAnim = 100;
        if (app.captureAt[0] == app.captureAt[0]) app.manualCursor = int(app.captureAt[0]);
        std::printf("manual: %zu topics\n", manualTopics(app).size());
    }
    if (app.captureOptions) { // --volume-menu (page 1) / --video-menu (page 4)
        loadMix(app);
        const int page = app.optionsPage;
        openOptions(app, page);
        app.optionsAnim = 100;
    }
    if (app.havePlayer && app.captureAt[0] == app.captureAt[0]) { // --at x z: put the player there
        float y;
        if (cr::floorHeight(app.collisionData, app.captureAt[0], 1000.0f, app.captureAt[1], 0.0f, y))
            app.walker.pos = {app.captureAt[0], y, app.captureAt[1]};
    }
    // The capture shows exactly the frames stepped above; --settle draws that moment first (an
    // upscaler's history converges on it)
    const Uint64 frame = SDL_NS_PER_SECOND / 30;
    for (int f = 0; f < app.captureSettle; ++f) {
        app.lastTicks = app.captureSettleMoving ? SDL_GetTicksNS() - frame : 0;
        drawFrame(app, false);
    }
    app.lastTicks = app.captureSettleMoving && app.captureSettle ? SDL_GetTicksNS() - frame : 0;
    drawFrame(app, false);
    int w = 0, h = 0;
    std::vector<uint8_t> px;
    if (!app.renderer.readPixels(px, w, h)) {
        std::printf("FAILED to read the frame back\n");
        return 1;
    }
    SDL_Surface* surf = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    for (int y = 0; y < h; ++y)
        std::memcpy(static_cast<uint8_t*>(surf->pixels) + size_t(y) * surf->pitch, &px[size_t(y) * w * 4], size_t(w) * 4);
    const bool ok = SDL_SaveBMP(surf, outPath);
    SDL_DestroySurface(surf);
    std::printf("%s %s -> %s (%dx%d)\n", ok ? "captured" : "FAILED", mapName, outPath, w, h);
    if (app.haveStageCam) {
        const cr::StageCamera& sc = app.stageCam;
        const cr::Mat4 vp = cr::Mat4::perspective(sc.fovDegrees * 3.14159265f / 180.0f, 1.0f, 10.0f, 4000.0f) *
                            cr::Mat4::lookAt({sc.eye[0], sc.eye[1], sc.eye[2]}, {sc.target[0], sc.target[1], sc.target[2]},
                                             {0, 1, 0});
        for (const auto& d : app.npcDraws) {
            const float o[3] = {0, 0, 0};
            const cr::Vec3 p = d.world.transformPoint(o);
            const float* m = vp.m;
            const float cw = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
            const float sx = (m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12]) / cw * 512 + 512;
            const float sy = 512 - (m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13]) / cw * 512;
            const LoadedModel& lm = app.npcModels[d.model];
            const std::string act = d.action >= 0 && d.action < int(lm.actions.size())
                                        ? lm.actions[d.action].name + (d.action < lm.builtin ? " (own)" : "") : "-";
            std::printf("npc %-16s at (%.1f %.1f %.1f) view (%.0f, %.0f) action %s t %.0f\n", d.model.c_str(), p.x, p.y,
                        p.z, sx, sy, act.c_str(), d.time);
        }
    }
    if (app.havePlayer && app.haveStageCam) {
        // Where the leader's bounding box lands in the 1024x1024 stage view
        const cr::StageCamera& sc = app.stageCam;
        const float aspect = sc.viewHeight > 0 ? float(sc.viewWidth) / float(sc.viewHeight) : 1.0f;
        const cr::Mat4 mvp = cr::Mat4::perspective(sc.fovDegrees * 3.14159265f / 180.0f, aspect, 10.0f, 4000.0f) *
                             cr::Mat4::lookAt({sc.eye[0], sc.eye[1], sc.eye[2]}, {sc.target[0], sc.target[1], sc.target[2]},
                                              {0, 1, 0}) * playerWorld(app.walker);
        const auto& b0 = app.player.cpu.boundsMin;
        const auto& b1 = app.player.cpu.boundsMax;
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (int c = 0; c < 8; ++c) {
            const float p[3] = {c & 1 ? b1[0] : b0[0], c & 2 ? b1[1] : b0[1], c & 4 ? b1[2] : b0[2]};
            const float* m = mvp.m;
            const float cw = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
            const float sx = (m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12]) / cw * 512 + 512;
            const float sy = 512 - (m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13]) / cw * 512;
            x0 = std::min(x0, sx); x1 = std::max(x1, sx); y0 = std::min(y0, sy); y1 = std::max(y1, sy);
        }
        std::printf("player %s at (%.1f %.1f %.1f) r=%.0f: view box x %.0f..%.0f y %.0f..%.0f (%.0f px tall)\n",
                    app.playerName.c_str(), app.walker.pos.x, app.walker.pos.y, app.walker.pos.z, app.walker.radius,
                    x0, x1, y0, y1, y1 - y0);
    }
    return ok ? 0 : 1;
}

} // namespace

// Video settings from the command line (after settings.ini): --original, --resolution WxH,
// --windowed / --fullscreen / --borderless, --widescreen, --zoom-paintings, --render-scale %,
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
        else if (SDL_strcmp(a, "--layout") == 0 && i + 1 < argc) // --layout auto | ipad | iphone
            v.layout = SDL_strcmp(argv[i + 1], "iphone") == 0 ? 2 : SDL_strcmp(argv[i + 1], "ipad") == 0 ? 1 : 0;
        else if (SDL_strcmp(a, "--zoom-paintings") == 0) v.paintings = 2;
        else if (SDL_strcmp(a, "--whole-paintings") == 0) v.paintings = 1;
        else if (SDL_strcmp(a, "--render-scale") == 0 && next) v.renderScale = std::clamp(SDL_atoi(next), 25, 400);
        else if (SDL_strcmp(a, "--msaa") == 0 && next) v.msaa = std::clamp(SDL_atoi(next), 1, 8);
        else if (SDL_strcmp(a, "--no-vsync") == 0) v.vsync = false;
        else if (SDL_strcmp(a, "--upscaler") == 0 && next) // off | fsr | dlss
            v.upscaler = SDL_strcmp(next, "fsr") == 0 ? 1 : SDL_strcmp(next, "dlss") == 0 ? 2 : 0;
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

SDL_AppResult SDL_AppInit(void** state, int argc, char** argv) {
    SDL_SetAppMetadata("Chaos Rings HD (reimplementation)", "0.1", "local.preservation.chaosrings");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_Init: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    auto* app = new App;
    *state = app;

    app->dataDir = cr::findDataDir(argc, argv);
    // A build that carries its own (beside the executable): hd-textures/ (on unless settings.ini says
    // otherwise) and vita/ (the Vita copy's CR1 folder: the game itself when no iOS data is found)
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
    std::string vitaBeside; // (the build's own: used, not written to settings.ini)
    if (!vitaGiven && app->vitaPath.empty() && !beside.empty() && isDir(beside + "vita")) vitaBeside = beside + "vita";
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
    } else if (!app->vitaPath.empty())
        openVita(*app, app->vitaPath);
    else if (!vitaBeside.empty()) {
        openVita(*app, vitaBeside);
        app->vitaPath.clear(); // (openVita takes it as the setting)
    }
    if (app->vitaOnly && app->vitaPath == app->vitaLoaded && app->vitaLoaded == vitaBeside) app->vitaPath.clear();
    if (const char* sd = SDL_getenv("CR_SAVE_DIR")) { // save.dat / resume.dat elsewhere (tests)
        app->saveDirOverride = sd;
        if (!app->saveDirOverride.empty() && app->saveDirOverride.back() != '/') app->saveDirOverride += '/';
    }
    bool hdBundled = false;
    if (const char* hd = SDL_getenv("CR_HD_TEXTURES")) app->hdDir = hd;
    else if (!beside.empty() && isDir(beside + "hd-textures")) app->hdDir = beside + "hd-textures", hdBundled = true;
    else app->hdDir = cr::saveDir() + "hd-textures";
    if (!app->hdDir.empty() && app->hdDir.back() != '/') app->hdDir += '/';
    if (app->dataDir.empty()) {
        const std::string msg = "Could not find the original game data (dataHD folder).\n\n"
                                "Copy dataHD from your ChaosRingsHD.app to:\n" +
                                cr::defaultDataDirHint() +
                                "\n\nor run with --data <path-to-dataHD>, or set CR_DATA - or --data / --vita "
                                "<your Vita copy's PCSG00497 folder>.";
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Chaos Rings", msg.c_str(), nullptr);
        return SDL_APP_FAILURE;
    }

    // --renderer vulkan | gl (or CR_RENDERER): Vulkan by default, OpenGL if Vulkan can't start
    cr::Backend want = cr::Backend::Vulkan;
    const char* choice = SDL_getenv("CR_RENDERER");
    for (int i = 1; i + 1 < argc; ++i)
        if (SDL_strcmp(argv[i], "--renderer") == 0) choice = argv[i + 1];
    if (choice && (SDL_strcasecmp(choice, "gl") == 0 || SDL_strcasecmp(choice, "opengl") == 0)) want = cr::Backend::OpenGL;
    for (cr::Backend b : {want, cr::Backend::OpenGL}) {
        const uint64_t flags = cr::Renderer::windowFlags(b);
        if (!flags) continue; // not built in
        app->window = SDL_CreateWindow("Chaos Rings", 1024, 768,
                                       SDL_WindowFlags(flags) | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!app->window) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateWindow: %s", SDL_GetError());
            continue;
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
    else SDL_EnumerateDirectory(app->dataDir.c_str(), collectAssets, app);
    app->systemText.load(app->dataDir);
    // The game's text in every language: a system font (CR_FONT overrides), else the 8x8 font
    if (cr::loadSystemFont(app->font)) SDL_Log("font: %s", app->font.path().c_str());
    else SDL_Log("no CJK font found (set CR_FONT): Japanese, Chinese and Korean text unavailable");
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
    {
        cr::Image wi;
        if (loadImage(*app, "window_hd.mct", wi)) app->windowTex = app->renderer.upload(wi);
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
    for (int i = 1; i < argc; ++i) {
        if (SDL_strcmp(argv[i], "--capture") == 0 && i + 2 < argc) {
            SDL_SetLogPriorities(SDL_LOG_PRIORITY_WARN);
            return runCapture(*app, argv[i + 1], argv[i + 2]) ? SDL_APP_FAILURE : SDL_APP_SUCCESS;
        }
        if (SDL_strcmp(argv[i], "--explore") == 0) {
            SDL_SetLogPriorities(SDL_LOG_PRIORITY_WARN);
            return runExplore(*app, i + 1 < argc ? SDL_atoi(argv[i + 1]) : 200) ? SDL_APP_FAILURE : SDL_APP_SUCCESS;
        }
        if (SDL_strcmp(argv[i], "--export-textures") == 0 && i + 1 < argc) {
            SDL_SetLogPriorities(SDL_LOG_PRIORITY_WARN);
            return runExportTextures(*app, argv[i + 1]) ? SDL_APP_FAILURE : SDL_APP_SUCCESS;
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
    applyVideo(*app);
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
        if (!viewer) bootTitle(*app);
    }

    return SDL_APP_CONTINUE;
}

namespace {
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
void padKey(App& app, SDL_Keycode key) { // the button as its key, through the same handling
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = key;
    e.key.down = true;
    SDL_AppEvent(&app, &e);
}
void padButton(App& app, int b) {
    const bool east = b == SDL_GAMEPAD_BUTTON_EAST, south = b == SDL_GAMEPAD_BUTTON_SOUTH;
    const bool confirm = app.padConfirmEast ? east : south, cancel = app.padConfirmEast ? south : east;
    const bool puzzle = app.puzzle.stage >= 0 && fieldMenuWanted(app) && !fieldSceneRunning(app);
    if (app.vitaLogo.phase) { if (confirm && app.vitaLogo.phase == 2) app.vitaLogo.skip = true; return; }
    switch (b) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: if (padMenus(app)) padKey(app, SDLK_UP); return;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: if (padMenus(app)) padKey(app, SDLK_DOWN); return;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: if (padMenus(app)) padKey(app, SDLK_LEFT); return;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: if (padMenus(app)) padKey(app, SDLK_RIGHT); return;
    case SDL_GAMEPAD_BUTTON_NORTH: // △: the menu
        if (app.campOpen) { app.campOpen = false; menuSound(app, se::kCampClose); }
        else if (padField(app)) padKey(app, SDLK_K);
        return;
    case SDL_GAMEPAD_BUTTON_WEST: // □: text skipped; in a puzzle room, the ability
        if (padField(app) && puzzle) padKey(app, SDLK_T);
        else if (app.mode == Mode::Maps && app.runner.running()) padKey(app, SDLK_X);
        return;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: padKey(app, SDLK_PAGEUP); return;   // L: a page back
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: padKey(app, SDLK_PAGEDOWN); return; // R: a page on
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
// Each frame: the stick in the menus as the cursor keys, and either held repeating (after 0.35 s,
// every 0.08 s)
void padStep(App& app, float dt) {
    if (!app.pad || !padMenus(app)) { app.padHeld = -1; return; }
    const float lx = float(SDL_GetGamepadAxis(app.pad, SDL_GAMEPAD_AXIS_LEFTX)) / 32767.0f;
    const float ly = float(SDL_GetGamepadAxis(app.pad, SDL_GAMEPAD_AXIS_LEFTY)) / 32767.0f;
    int dir = -1;
    if (SDL_GetGamepadButton(app.pad, SDL_GAMEPAD_BUTTON_DPAD_UP)) dir = 0;
    else if (SDL_GetGamepadButton(app.pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) dir = 1;
    else if (SDL_GetGamepadButton(app.pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) dir = 2;
    else if (SDL_GetGamepadButton(app.pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) dir = 3;
    const bool fromPad = dir >= 0;
    if (!fromPad && std::max(std::fabs(lx), std::fabs(ly)) > 0.5f) // (the Vita's: past half, a direction)
        dir = std::fabs(ly) >= std::fabs(lx) ? (ly < 0 ? 0 : 1) : (lx < 0 ? 2 : 3);
    static const SDL_Keycode kDir[4] = {SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT};
    if (dir != app.padHeld) {
        app.padHeld = dir, app.padHeldFor = 0;
        if (dir >= 0 && !fromPad) padKey(app, kDir[dir]); // (the D-pad's first press came as its button event)
        return;
    }
    if (dir < 0) return;
    const float before = app.padHeldFor;
    app.padHeldFor += dt;
    if (app.padHeldFor >= 0.35f && std::floor((app.padHeldFor - 0.35f) / 0.08f) != std::floor((before - 0.35f) / 0.08f))
        padKey(app, kDir[dir]);
}
} // namespace

SDL_AppResult SDL_AppEvent(void* state, SDL_Event* event) {
    auto& app = *static_cast<App*>(state);
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
        if (app.pad && SDL_GetGamepadID(app.pad) == event->gdevice.which) SDL_CloseGamepad(app.pad), app.pad = nullptr;
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (app.pad && SDL_GetGamepadID(app.pad) == event->gbutton.which) padButton(app, event->gbutton.button);
        break;
    case SDL_EVENT_KEY_DOWN:
        if (app.vitaLogo.phase) { // the Vita's logos: confirm cuts the hold short
            const SDL_Keycode k = event->key.key;
            if (app.vitaLogo.phase == 2 && (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE || k == SDLK_E || k == SDLK_ESCAPE))
                app.vitaLogo.skip = true;
            break;
        }
        if (event->key.key == SDLK_F3) { app.showHud = !app.showHud; break; } // the status / key help at the top
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
        if (app.resumePrompt) {
            switch (event->key.key) {
            case SDLK_LEFT: case SDLK_RIGHT: case SDLK_A: case SDLK_D: app.resumeCursor ^= 1; menuSound(app, se::kCursor); break;
            case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: case SDLK_E: answerResume(app, app.resumeCursor == 0); break;
            case SDLK_ESCAPE: case SDLK_BACKSPACE: answerResume(app, false); break;
            default: break;
            }
            break;
        }
        if (app.staffRoll.open) { // the end card waits for a tap
            switch (event->key.key) {
            case SDLK_ESCAPE: case SDLK_BACKSPACE: case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: case SDLK_E:
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
            if (!titleKey(app, event->key.key)) return SDL_APP_SUCCESS;
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
            case SDLK_ESCAPE: case SDLK_BACKSPACE: answerChoice(app, false); return SDL_APP_CONTINUE;
            default: break;
            }
        }
        switch (event->key.key) {
        case SDLK_ESCAPE: case SDLK_AC_BACK: return SDL_APP_SUCCESS;
        case SDLK_RIGHT: step(app, 1); break;
        case SDLK_LEFT: step(app, -1); break;
        case SDLK_PAGEDOWN: step(app, 10); break;
        case SDLK_PAGEUP: step(app, -10); break;
        case SDLK_TAB:
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
        case SDLK_E: case SDLK_RETURN:
            if (app.mode != Mode::Maps) break;
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
        case SDLK_X: if (app.mode == Mode::Maps && app.runner.running()) app.skipScene = true; break;
        case SDLK_N:
            if (app.mode == Mode::Models) selectAction(app, 1);
            else if (app.mode == Mode::Maps) { app.showNpcs = !app.showNpcs; buildNpcDraws(app); updateTitle(app); }
            break;
        case SDLK_B: // previous animation in model mode, background view in map mode
            if (app.mode == Mode::Models) selectAction(app, -1);
            else if (app.mode == Mode::Maps) { app.showBackground = !app.showBackground; updateTitle(app); }
            break;
        case SDLK_SPACE: app.playing = !app.playing; updateTitle(app); break;
        case SDLK_MINUS: app.speed = std::max(0.125f, app.speed * 0.5f); updateTitle(app); break;
        case SDLK_EQUALS: app.speed = std::min(8.0f, app.speed * 2.0f); updateTitle(app); break;
        default: break;
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (app.vitaLogo.phase) { // (a tap: the same)
            if (app.vitaLogo.phase == 2) app.vitaLogo.skip = true;
            break;
        }
        if (app.manualOpen || app.scenariosOpen) {
            int ww = 0, wh = 0;
            SDL_GetWindowSize(app.window, &ww, &wh);
            const float kx = ww > 0 ? float(app.renderer.frameWidth()) / float(ww) : 1.0f;
            const float ky = wh > 0 ? float(app.renderer.frameHeight()) / float(wh) : 1.0f;
            if (app.manualOpen) manualClick(app, event->button.x * kx, event->button.y * ky);
            else {
                float lx, ly;
                app.renderer.toLogical(event->button.x * kx, event->button.y * ky, lx, ly);
                float mx, my;
    menuPoint(app, lx, ly, mx, my);
                const float backX = app.renderer.phoneLayout() ? 192 : 272, backY = app.renderer.phoneLayout() ? 286 : 330;
                if (my >= backY && my < backY + 24 && mx >= backX && mx < backX + 232) { menuSound(app, se::kBack); closeScenarios(app); }
                else app.dragging = true; // drag to scroll
            }
            break;
        }
        if (app.optionsOpen) { // window points -> the frame's pixels
            int ww = 0, wh = 0;
            SDL_GetWindowSize(app.window, &ww, &wh);
            const float kx = ww > 0 ? float(app.renderer.frameWidth()) / float(ww) : 1.0f;
            const float ky = wh > 0 ? float(app.renderer.frameHeight()) / float(wh) : 1.0f;
            optionsClick(app, event->button.x * kx, event->button.y * ky);
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
            if (app.saveScreen.open) saveScreenClick(app, px, py);
            else if (app.charSelectOpen) charSelectClick(app, px, py);
            else if (app.campOpen) campClick(app, px, py);
            else if (app.lastBattle.open) lastBattleClick(app, px, py);
            else if (app.worldChoiceOpen) worldSelectClick(app, px, py);
            else if (app.shop.open) shopClick(app, px, py);
            else if (app.mode == Mode::Maps && app.battle.active) battleClick(app, px, py);
            else if (app.resumePrompt) {
                float lx, ly;
                app.renderer.toLogical(px, py, lx, ly);
                float mx, my;
    menuPoint(app, lx, ly, mx, my);
                if (my >= 222 && my < 270 && mx >= 128 && mx < 240) answerResume(app, true);
                else if (my >= 222 && my < 270 && mx >= 272 && mx < 384) answerResume(app, false);
            } else titleClick(app, px, py);
            break;
        }
        if (app.staffRoll.open) { // _pad_chk_updw_box(-16, -32, 512, 384): the end card's tap
            staffRollTap(app);
            break;
        }
        if (app.mapScreen.open) { // _pad_chk_updw_box(0, 0, 480, 320): a tap closes it
            closeMapScreen(app);
            break;
        }
        if (app.mode == Mode::Maps) { // the field menu's corner button and bar
            int ww = 0, wh = 0;
            SDL_GetWindowSize(app.window, &ww, &wh);
            const float kx = ww > 0 ? float(app.renderer.frameWidth()) / float(ww) : 1.0f;
            const float ky = wh > 0 ? float(app.renderer.frameHeight()) / float(wh) : 1.0f;
            if (fieldMenuClick(app, event->button.x * kx, event->button.y * ky)) break;
            if (puzzle::click(app, event->button.x * kx, event->button.y * ky)) break; // a tap on a block
        }
        if (app.mode == Mode::Maps && app.runner.running() && app.windowTex.id) {
            // A scene: the skip button (top right) fast-forwards it; a tap elsewhere turns the page
            int ww = 0, wh = 0;
            SDL_GetWindowSize(app.window, &ww, &wh);
            const float kx = ww > 0 ? float(app.renderer.frameWidth()) / float(ww) : 1.0f;
            const float ky = wh > 0 ? float(app.renderer.frameHeight()) / float(wh) : 1.0f;
            float lx, ly;
            app.renderer.toLogical(event->button.x * kx, event->button.y * ky, lx, ly);
            if (lx >= app.renderer.layoutRight() - 76 && ly < app.renderer.layoutTop() + 72) {
                app.skipScene = true;
                break;
            }
            if (app.choiceFlag >= 0) { // the Yes / No plates (0x88 x 0x28)
                float mx, my;
    menuPoint(app, lx, ly, mx, my);
                if (mx >= 336 && mx < 472 && my >= 104 && my < 144) answerChoice(app, true);
                else if (mx >= 336 && mx < 472 && my >= 160 && my < 200) answerChoice(app, false);
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
        app.dragging = false;
        if (app.mode == Mode::Maps && app.puzzle.pkState == 1) {
            int ww = 0, wh = 0;
            SDL_GetWindowSize(app.window, &ww, &wh);
            const float kx = ww > 0 ? float(app.renderer.frameWidth()) / float(ww) : 1.0f;
            const float ky = wh > 0 ? float(app.renderer.frameHeight()) / float(wh) : 1.0f;
            puzzle::release(app, event->button.x * kx, event->button.y * ky);
        }
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (app.dragging && app.scenariosOpen) { // the chart follows the finger
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
    static_cast<App*>(state)->sound.update();
    {
        static Uint64 padLast = SDL_GetTicksNS();
        const Uint64 now = SDL_GetTicksNS();
        padStep(*static_cast<App*>(state), float(double(now - padLast) * 1e-9));
        padLast = now;
    }
    {
        static Uint64 last = SDL_GetTicksNS();
        const Uint64 now = SDL_GetTicksNS();
        static_cast<App*>(state)->music.update(float(double(now - last) * 1e-9));
        last = now;
    }
    drawFrame(*static_cast<App*>(state));
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* state, SDL_AppResult) {
    auto* app = static_cast<App*>(state);
    if (!app) return;
    writeResume(*app); // the Progress Tracker: left on the field, resumed from the title next time
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
