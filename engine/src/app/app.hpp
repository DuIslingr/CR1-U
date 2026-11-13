#pragma once

#include "platform/vita_import_ui.hpp"
#include <SDL3/SDL.h>
#ifdef SDL_PLATFORM_ANDROID
#include <jni.h>
#endif

#include <algorithm>
#include <atomic>
#include <bitset>
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
#include "assets/gltf_model.hpp"
#include "game/save.hpp"
#include "game/shop.hpp"
#include "assets/animation.hpp"
#include "assets/collision.hpp"
#include "assets/battle_data.hpp"
#include "assets/event.hpp"
#include "assets/gxt.hpp"
#include "assets/psarc.hpp"
#include "assets/obb.hpp"
#include "game/encounter.hpp"
#include "game/event_vm.hpp"
#include "game/field_walker.hpp"
#include "game/stage_tables.hpp"
#include "game/battle_rules.hpp"
#include "game/genes.hpp"
#include "game/puzzle.hpp"
#include "game/treasure.hpp"
#include "game/trophies.hpp"
#include "assets/model.hpp"
#include "assets/texture.hpp"
#include "audio/music.hpp"
#include "audio/sound.hpp"
#include "core/file.hpp"
#include "core/timing.hpp"
#include "core/math.hpp"
#include "gfx/font.hpp"
#include "gfx/hud.hpp"
#include "gfx/menu_painter.hpp"
#include "gfx/renderer.hpp"
#include "platform/paths.hpp"
#include "platform/setup.hpp"
#include "platform/input_cancel.hpp"
#include "platform/atomic_file.hpp"
#include "platform/lifecycle_clock.hpp"
#include "scene/pose.hpp"

#ifndef SDL_main_h_
extern "C" {
#ifdef SDL_PLATFORM_ANDROID
SDL_DECLSPEC
#endif
SDL_AppResult SDLCALL SDL_AppEvent(void* appstate, SDL_Event* event);
}
#endif

namespace crapp {

enum class Mode { Textures, Models, Maps };

// Original animations are keyed in frames, played a frame a game frame (core/timing.hpp: 20 a second)
using cr::kFramesPerSecond;

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
    int loopTo = 0;                      // ... up to this frame (0: its end)
    bool once = false;                   // played once, its end held (Anim's loop from -1)
    cr::MotionBlend blend;               // the 2-frame cross-fade into a new action (the player's)
    std::vector<char> slotOn;            // a battle model's package slots _btl_anm_load loads (empty: all)
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
    float pitch = 0, roll = 0; // FrmPlayEff2: the record's X and Z angles (radians)
    bool faceCamera = false;   // FrmPlayEff at a record's point: turned to the camera each frame (mdl + 0xfc7, as a battle's)
    int handle = 0;  // for stopping it
    int key = 0;     // FrmPlayEff's (effect + 1) x (actor + 1)
};

struct App {
    crinput::CancelWatch inputCancel;
    crtime::LifecycleClock clock;
    std::atomic<bool> graphicsReset{false};
    SDL_ThreadID gameThread = 0;
    bool pointerCancelled = false;
    bool pointerLast = false;           // the last press a touch / click (no key or pad since): touch play's look
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
    // The Android release (Google Play) as a source of assets (--android, CR_ANDROID or
    // settings.ini's android=): its expansion file's textures (8-bit palettes where the iPad's
    // are PVRTC) and the art only it has (docs/ANDROID.md), its table read from the APK beside it
    std::string androidPath, androidLoaded, androidNote;
    cr::ObbArchive android;
    bool androidOnly = false;              // no iOS data: the Android release is the whole game
    int androidTextures = 0;
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
    cr::GpuMesh fieldShadows;            // the field's shadow fans (drawShadowFans)
    std::vector<float> fieldShadowTris; // ... this frame's triangles (xyz x 3 each)
    std::vector<uint8_t> fieldShadowAlpha; // and their vertices' alpha
    int shadowsDrawn = 0;               // (tests: field shadows drawn so far)
    // (tests: the last frame's 2D order - the sequence numbers of its portraits, its front fade and
    // its message text, 0 none)
    struct Order2D { int seq = 0, portrait = 0, frontFade = 0, text = 0; } order2d;
    int frontFadeDrawnAt = -1;          // drawFrontFade: the frame it last drew (once a frame)
    int frameNo = 0;
    cr::Texture charSheet;              // tex2d slot 0x32: window_char (2.2.0: the languages' names, the boards' names)
    bool charSheetTried = false;
    float nearPlane = 11, farPlane = 2000; // setCamera3D's, the last field frame's (tests)
    std::string mapProps;               // the field's alpha model (occluders) shown with the current map, if any
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
    int mapNumber = -1, area = -1;         // the field being shown (map 0..129, area = event file)
    cr::EventFile events;                  // the current area's event file
    std::vector<cr::FieldNpc> npcs;        // this map's NPCs and trigger boxes
    std::vector<bool> npcWarps;            // per NPC: its touch script warps (from a trial run)
    cr::GpuMesh triggerMesh;               // trigger boxes, shown with the collision overlay
    cr::GpuMesh puzzleBeams;               // the candle switches' beams (CandleSwitch::draw3D)
    cr::GpuMesh battleShadows;             // the fighters' floor shadows (_btl_shadow's fans)
    cr::GameState game;                    // flags / variables the scripts use (new game)
    int lastKick = 0;                      // G+0x358e4: the touch box last kicked - kept across warps and loads,
                                           // 0 once the leader stands in none (RectWrapper::checkEventKick, 2.2.0 0x3dcfc)
    std::map<int, int> examineLinger;      // FollowWindow / FlagWindow: an NPC's window, frames left after leaving reach (10)
    std::string lastWarp;                  // shown in the status line
    std::string lastEvent;                 // last examined object / items, for the status line
    cr::Hud dialogueHud, promptHud, itemHud;
    std::array<cr::Hud, 16> lineHud;       // the message window's lines one by one (some centred; _PutMsg's 16 slots)
    cr::EventRunner runner;                // the script being played, a frame at a time
    cr::SoundSystem sound;                 // sndeff.bin through SDL audio
    cr::MusicPlayer music;                 // bgmNN.caf (AAC) through FAAD2 + SDL audio
    // Mixer, 0..100 in steps of 10 like the original's Options gauges (BGM, SE and voice 80 by
    // default - _InitSystemData; master is an addition)
    struct Mix { int master = 100, music = 80, effects = 80, voice = 80; } mix;
    bool optionsOpen = false;              // V: the Options screen (__menuu_Option*, plus our pages)
    int optionsPage = 0;                   // Game, Sound, Display, Graphics
    int optionsRow = 0;                    // the cursor: a row, or kOptionsBackRow
    int optionsScroll = 0;                 // the first row shown (a page of more than kOptVisible scrolls)
    bool optionsBarDrag = false;           // the scroll bar's thumb held
    int optionsAnim = 0;                   // frames since the page appeared (windows slide in)
    int optionsBob = 0, optionsBobDir = 1; // ComUty_CmdBack's arrows
    float optionsClock = 0;
    bool environmentEffects = true;        // the original's Environment Effects (weather; stored)
    bool lockLandscapeFlipped = false; // the landscape direction retained by the orientation lock
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
    int titleStage = 0;                    // __menuu_TitlePrc: 0 the title's art alone, a tap away (state 1); 1 the plates (2..4)
    int resumeIn = 0;                      // the Progress Tracker's Yes: frames of black left before the load (TitlePrc 0x16 / 0x17)
    int titleReturnIn = 0;                 // Options' Return to Title: frames of black left before the title (OptionPrc 0x18 / 0x19)
    cr::Texture titleArt[2];               // __menuu_TitleSet: title1.mct (MuCom slot 0x11), chaos.mct (0x12)
    bool optionsFromTitle = false;         // ModeOptionMenu(1): Reset Data instead of Return to Title
    int resetAsk = 0, resetConfirm = 1;    // Reset Data's two questions (OptionPrc 9 / 10: 1, 2), Return to Title's (0xc / 0xd: 3); No under the cursor
    bool captureTitle = false;             // --capture ... --title
    // Video. The defaults are the original presentation: a 1024x768 window, 4:3, the paintings at
    // the screen's height, native resolution, no MSAA, vsync
    struct Video {
        int width = 1024, height = 768;    // window size (windowed) or display mode (fullscreen)
        int display = 0;                   // 0 windowed, 1 fullscreen (that mode), 2 borderless (the desktop's)
#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_IOS)
        bool widescreen = true;            // the screen widens to the window (up to 21:9); else 4:3 (a phone's: wide, filling it)
#else
        bool widescreen = false;           // the screen widens to the window (up to 21:9); else 4:3
#endif
        int paintings = 0;                 // painted rooms: 0 the original's (2x, a screen-sized window following
                                           // the player), 1 Fill: the same, zoomed until the room's painting covers
                                           // the screen (no bars on a wide screen) - the port's own
        int renderScale = 100;             // % of the window's pixels (Vulkan)
        int msaa = 1;                      // 1, 2, 4, 8 (Vulkan)
        bool vsync = true;
        int frameLimit = 60;               // frames drawn a second at most (0: as many as the display takes); the
                                           // game itself runs at 20 (core/timing.hpp), the frames between show
                                           // it moving
        int upscaler = 0;                  // 0 off, 1 FSR 3.1, 2 DLSS (Vulkan; MSAA is off meanwhile)
        int upscaleMode = 0;               // kUpscaleModes: 0 native AA (DLAA) .. 4 ultra performance
        int sharpness = 25;                // % (FSR's RCAS pass; 0 = none)
        bool hdTextures = false;           // upscaled replacements from hdDir (what loads next)
        int smoothModels = 0;              // 0 off, 2 / 3: PN-triangle level for characters and monsters
        bool hdModels = false;             // replacement models from modelsDir (glTF; what loads next)
        int anisotropy = 16;               // anisotropic filtering's samples on the mipmapped textures (1 off)
        bool sharpTextures = false;        // those magnified by the nearest texel (square pixels up close)
        int layout = 0;                    // 0 the data's own (2.2.0's iPhone layout for its data), 1 iPad, 2 iPhone, 3 Android
    } video;
    bool phoneData = false;                // the data is a voiced iPhone release's (voiceevnt.bin: 2.2.0)
    std::string hdDir;                     // HD textures: textures/hd/ in the game folder, or CR_HD_TEXTURES
    int hdLoaded = 0, hdMissing = 0;       // replacements found / not found since startup
    std::set<std::string> hdRefused;       // replacements left out (not their pictures' shape): said once
    std::string modelsDir;                 // replacement models: models/hd/ in the game folder, or CR_HD_MODELS
    int hdModelsLoaded = 0;                // since startup
    int language = 1;                      // text language (cr::kLanguageName): English by default
    cr::Font font;                         // system font for the game's text (all languages)
    std::vector<std::vector<int>> dialogueVoices; // per page: voice clips it starts (0x1e)
    int dialogueSerial = 0, voicedSerial = -1, voicedPage = -1; // which page's voices were started
    std::vector<int> voiceQueue;           // further clips of the same page
    bool voiceKeep = false;                // PrcMsg 0x1e 0xffff (G+0x337f0): the clip goes on past the next tap
    bool voiceCloseKeeps = false;          // the message shown ends with 0x13 / 0x14: its close leaves the clip
    int voicesPlayed = 0;                  // for tests
    int soundTestEntry = 0;
    std::vector<int> voiceEntries;         // bank entries marked as voice (sound test, T)
    cr::Hud optionsHud;
    cr::Texture windowTex;                 // window_hd.mct: the game's UI atlas (panels, badges, …)
    bool androidWindow = false;            // this atlas keeps its dialogue parchment in window_cutin_<w>
    cr::Texture parchPaperTex, parchStreakTex; // splitParchment's: the phone parchment's paper and its name streak apart
    unsigned parchSplitFor = ~0u;          // (the window atlas they were made from)
    float parchTop = 0;                    // the split paper's top edge beside the streak (piece units): it stays put as the paper scales
    cr::Texture cutinWideTex;              // window_cutin_<w>.mct (Android, slot 0x30): androidCutin's
    std::string cutinWideName;
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
    struct Rescale {                       // AdjScale (npc_autoctl_set_scale: autoctl mode 3) from -> to over `frames`
        int npc = 0;
        float from[3] = {1, 1, 1}, to[3] = {1, 1, 1};
        int frames = 1, t = 0;
    };
    std::vector<Rescale> rescales;
    std::set<int> placedNpcs;              // actors a script placed or showed: parked slots (y 1000, origin) appear
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
        // The scroll the scripts set (GetScroll / SetScroll's): not held to the painting - only what is
        // shown is (offX / offY) - so FrmPanCam there and back again over its edge comes back where it was
        float curX = 0, curY = 0;
        bool scripted = false;
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
    cr::Texture speedLinesTex;             // syuutyuusen_hd.mct (Tex2d slot 47, tex2d_setup_tex): scene effect 1
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
    struct ItemWin {                       // its slot (_Uty_GetItem_Proc / _Put): +0 the slide (the band at
        int slide = 0, fade = 0;           // tex2d y slide x 32 - 32), +2 the fade-out once flag 0 is up
        int icon = -1;                     // +4: the item's icon (GetIconNoFromItemNo - 1), 0x3f the Oz
        int message = 0;                   // FUN_00039a5c(.., 6, n): 57010, 57085 (Oz) or 57094 (item 0x1b6)
        bool oz = false;
    } itemWin;
    // The skip corner: _PrcMsg turns a page (and ends typing) on every frame _pad_chk_circle(476, -16, 20)
    // finds a touch held there - pages turn while it's held and stop with the finger. skipHeld: 1 the X
    // key, 2 the pad's square, 4 a pointer held on the corner; skipScene = any of them
    unsigned skipHeld = 0;
    bool skipScene = false;
    int msgStyle = 2, msgLines = 2;        // the script's last Open: the message window's style and lines
    // The window itself (PrcWin: _SetWin / _ClrWin): 0 none yet, 1 / 2 fading in over 4 frames
    // (the text waits), 3 up, 4 / 5 fading out over 5 frames, 6 gone
    struct MsgWin { int state = 0, t = 0; float alpha = 0; } msgWin;
    bool fieldHidden = false;              // Screen's toggle (gac + 0x39c3e): the field not drawn
    double playSeconds = 0;                // TOTAL PLAYTIME: time on the field and in battles
    // The save / load screen (__menuu_SaveLoad*): from the Camp menu (save) or the title's Start
    // (load / new game). phase 0 choose, 1 confirm (Yes / No), 2 done (a message, then it closes)
    struct SaveScreen {
        bool open = false, load = false, haveSave = false;
        int cursor = 0, phase = 0, confirm = 0, message = 0, timer = 0, anim = 0;
        int pending = 0;                   // what Yes does: 1 save, 2 load, 3 new game
        bool toTitle = false;              // Util 6's (SetModeSaveLoadMenu 3 / 4): the title follows, saved or not (SaveLoadClr)
        bool askLeave = false;             // ... its "return to the title without saving?" (0x3c) after a No
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
        // Member: __Buy_CharChoiceProc's pair (Buy's Weapons / Armor / Jewel, Sell's all but Items)
        enum Phase { Main, Category, Member, List, Count, EquipAsk, Talk } phase = Main;
        bool open = false, selling = false;
        // listCursor: the focus row, on MuScrList's line at y 150; scroll: the rows' offset now, on
        // its way to 48 x the focus 8 a frame (listTop: kept equal to the focus)
        int cursor = 0, category = 0, listCursor = 0, listTop = 0, count = 1, confirm = 0;
        float scroll = 0;
        int mark = 0;                      // the message board's mark (ComUty_PictAnimInit(1, 4, 20): out and back)
        int bob = 0, bobDir = 1;           // ComUty_CmdBack's arrows (0..5 and back)
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
    int manualIntro = 0;                   // the page shown before a topic is chosen (56035), 0 none
    bool charSelectOpen = false;
    int charSelectCursor = 0, charSelectAnim = 0, chosenPair = 1, charSelectPage = 0;
    cr::Texture charSelectArt;
    struct Camp {                          // the Camp menu's page and cursors (__menuu_CampMain*)
        enum Page { Main, Items, Equip, Status, Genes } page = Main;
        enum Phase { List, Member, Slot, Ask, Entries, Result, Exit } phase = List; // Status: Slot = the member's
                                           // details; Entries: a plate's known-entry list (EquipProc 0x1a-0x1d,
                                           // Genes 4-5); Result: the board after an item is used (CampMainPrc 0's
                                           // 8-10); Exit: the Emergency Exit's frames (0's 7, 0xf-0x12, then
                                           // CampMain 9, 0xd-0x10)
        int cursor = 0;                    // Main: the plate
        int category = 0;                  // Items: campui::kCategoryKind index
        std::vector<int> list;
        int listCursor = 0, listTop = 0;
        int plate = 0;                     // Entries: the plate chosen
        int plateCursor = 0, plateTop = 0; // ... and where the plate list was, for Back
        int member = 0;                    // the pair's first or second
        int slot = 0;                      // Equip: weapon, armor, jewel, Human Genes, gene plates 1-3
        int slotTop = 0;                   // the slot list's first row shown (a MuScrList of seven)
        bool direct = false;               // Equip from a face board's tap (CampMain 7's 5): Back leaves the page
        int count = 1, confirm = 0;        // Items: how many seeds, Yes / No
        int resultItem = 0, resultCount = 0; // Result: the item used and how many
        int exitT = 0;                     // Exit: frames since Yes
        int statusView = 0;                // Status details: 0 the board, 1 the equipment, 2 the skills
        int viewTop[3] = {0, 0, 0};        // ... each list's first row
        int notice = 0;                    // a once-only message up over the Camp (MsgUty: 56010 / 56016), 0 none
        std::string message;
        int messageFrames = 0;
        int anim = 0;                      // game frames (20 Hz) since the page came up: the windows' moves
        // The scrolling list's offset (MuScrList, here positive, in menu units): 48 x the focus row once
        // settled, stepping 8 a frame toward it; scrollKey names the list it belongs to (-1 none yet)
        float scroll = 0;
        int scrollKey = -1;
        int mark = 0;                      // the message board's mark (one a frame)
        int leaving = 0;                   // Back on the main page: frames of the windows going, then the Camp closes
        // Back from a page to the main page (CampMainPrc's last states, then CampMain 9): frames since the page's
        // windows began going, 0 none - the main page's come back once they have gone (campPageOutFrames)
        int pageOut = 0;
        // The Items page's states (CampMainPrc 0): the one left takes its windows off, the next puts its own on a
        // frame or two later (campPhaseEase); phaseT counts frames since the change (100: settled)
        Page shownPage = Main;
        Phase shownPhase = List, prevPhase = List;
        int phaseT = 100;
        bool barIn = true;                 // the main page's Back bar (window 4) coming on with it: at the Camp's
                                           // opening and back from Items (the other pages leave it up)
        std::array<cr::Texture, 22> plateArt; // gene_m*c.mct, loaded when a board first shows them
        std::array<cr::Texture, 8> statusArt; // the records' Status portraits (j001_L01.. : 0xca490)
    } camp;
    float campClock = 0;
    struct CampPointer {
        bool down = false, dragging = false, bar = false;
        float startX = 0, startY = 0, lastY = 0, remainder = 0;
        float px = 0, py = 0;
        Camp::Page page = Camp::Main;
        Camp::Phase phase = Camp::List;
    } campPointer;
    float saveClock = 0;
    // The title's Progress Tracker offer (__menuu_TitlePrc 0xf..0x18, TitlePut windows 4..7)
    int resumePrompt = 0;                  // 1: "You have save data from your last session" (Yes / No, 0x14); 2: its windows going (0x15..)
    int resumeNotice = 0;                  // 1: "About Progress Tracker" (56036, MsgUty_WinOpen(0x24)) being read (0x10..0x12)
    float resumeWin = 0;                   // windows 4..7 (ComUty_WinDispOn / Off): 0 out .. 1 in
    int resumeMark = 0;                    // the board's tap mark (ComUty_PictAnim: 1 tick a frame, out and back)
    bool resumeYes = false;                // DAT_000d338c: Yes chosen (the music off and the black, then the load)
    bool interruptShown = false;           // DAT_000d3370 (MuTitle_GetInInterruptFlg): the message shown this launch
    // Prompts seen outside a save ("Option", OptionSet's flag 0x43): 2.2.0 sets it in the flags in
    // memory, which only a later save keeps - with no save written after it, it came back each launch.
    // Kept in settings.ini (seen_flags=) too and read as the saves' (allPairsFlags)
    std::vector<int> seenFlags;
    int resumeCursor = 0;
    cr::SaveData resumeData;
    vitaimport::Session saveImport;
    std::string saveDirOverride;           // tests: where save.dat / resume.dat go
    bool trackerArmed = false;             // G+0x34e1d: the Progress Tracker saves from the second scene's end on
    struct Trophies { // game/trophies.hpp: the Vita's 16 (with the Vita copy as the game), kept in settings.ini
        cr::trophies::Flags flags;     // G+0x34e38..: unlocked (0..15) and the shop's purchase (0x2f)
        std::vector<int> queue;        // unlocked, notification not shown yet
        cr::trophies::Notice notice;
        std::array<cr::trophies::Info, cr::trophies::kCount> info; // TROPHY.TRP's names and pictures
        std::array<cr::Texture, cr::trophies::kCount> icons;
        bool packTried = false, packRead = false;
        bool force = false;            // (self-test, captures) on without the Vita copy as the game
        bool live = true;              // unlocked at all (off in the self-test's other checks and in captures)
        bool hold = false;             // (captures) the notification held where it is
        std::vector<int> earnedLog;    // (self-test) every trophy unlocked this run, in order
    } trophy;
    // A controller, as the Vita's buttons (its manual, 56000 / 56025): the D-pad / left stick move
    // and pick, ○ confirms, × cancels (the Japanese release's; padConfirmEast false swaps them), △
    // opens / closes the menu, □ skips text, L / R turn pages; in puzzle rooms START restarts,
    // SELECT shows the help, and □ (no text to skip) uses the ability, which the Vita leaves to touch
    SDL_Gamepad* pad = nullptr;
    bool padConfirmEast = true;
    // CR1:U's own (Options, Game: Field Speed, Battle Speed): the field - while the player walks it,
    // nothing over it - and the battles run this many times as fast (1, or 2 .. 5)
    int fieldSpeed = 1, battleSpeed = 1;
    int padHeld = -1;                      // the direction held for the menus' repeat (0 up 1 down 2 left 3 right)
    float padHeldFor = 0;
    crinput::HeldOver padHeldOver;         // (dead after a cancel or a session's end, until let go)
    int itemWindowItem = 0;
    bool tapPending = false;               // action key while a script waits for a tap
    float fadeGrey = 0, fadeFrom = 0, fadeTo = 0; // Screen
    // Screen's op 58 (_SetFade_back: G+0x3a0a0, the shape 0 - the whole screen): over the 3D, under
    // every 2D (the back mask's draw order -100); Wait 0xFFFC waits for it
    struct BackFade { float grey = 0, from = 0, to = 0; int frames = 0, t = 0; } backFade;
    // Anim / Anim2 on npc 0 (the leader's model, gac + 0x83c) while a scene holds the field
    struct PlayerAnim { bool on = false; int action = 0; bool once = false; int loopFrom = 0, loopTo = 0; } playerAnim;
    // The menus' mask (MuCom_MenuSetMask / ClrMask: SetFade_back(black, 0 <-> 0xa0, 400 ms) with
    // G+0x39c60 its shape - 0 the screen, 1 its left past a slant, 2 its right, 3 fading downward)
    struct MenuMask {
        int type = 0, want = -1;           // the shape; the menu asking for it (-1 none)
        int from = 0, to = 0, t = 0, ms = 1; // the fade: from + (to - from) x t / ms, t + 50 a frame
        float clock = 0;
    } menuMask;
    int fadeFrames = 0, fadeT = 0;
    cr::SystemText systemText;             // sys.evt: messages >= 51000
    std::vector<std::vector<std::string>> dialogue; // pages of the messages the last event showed
    size_t dialoguePage = 0;
    std::vector<cr::MessageInfo> dialogueInfo; // per page: the speaker's name plate and side
    // PrcMsg's typing: the page's characters out so far (one a frame), a 0x1a pause's or the page
    // turn's frames left, the next pause; dialogueInstant: the typing flag (G+0x337df) down - a tap
    // or the skip corner brought the rest out at once (still held by pauses and line breaks)
    int dialogueTyped = 0, dialogueHold = 0, dialoguePause = 0;
    bool dialogueInstant = false;
    bool typeOn = true;                    // false: pages appear whole (tests, captures)
    int textLimit = 1 << 30;               // drawMessageLines: how many characters it draws
    std::vector<std::string> lastPage;     // the page just read: stays up while a choice asks about it
    bool heldPage = false;                 // ... or because it ended itself (0x14): up until Close (PrcMsg)
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
        int bossModel = -1, bossWait = 0;  // the boss's end (btl_main 0x3de / 0x3e3..0x3e6): G+0x202, G+0xa40
        struct Actor {
            std::string model;             // battle model package (cache key)
            int modelId = 0;               // kBattleModelFile index, for its action slots
            bool enemy = false;
            int monster = 0, level = 1;
            uint32_t hp = 0, hpMax = 0;
            std::string name;
            cr::Vec3 pos;
            float yaw = 0;                 // +0x554: the yaw shown (radians)
            // _mdl_put (0x14a70) with FUN_00013734: the shown yaw turns toward the wanted one (+0x540) by
            // turnStep degrees a frame (+0x558: 15, set again once it's there); btlmove_prc turns it home
            // over a setpos_angy move (turnHome: at the move's start)
            float wantYaw = 0, turnStep = 15;
            bool turnHome = false;
            float approachYaw = 0;         // the attack's heading out, turned to on the way back
            bool approached = false;
            int slot = 0, action = -1;     // action slot (BattleAnim) and the package's action index
            float time = 0;
            float scale = 1;               // the model's size (the pair's 1: btl_main's 0.5 at +0x540 is its yaw, half a turn)
            int fighter = -1;              // BattleRules::f index: 0, 1 the pair, 2..4 the enemies
            int then = -1;                 // the slot after this one ends (-1 loop, -2 hold the last frame)
            cr::MotionBlend blend;         // mdl_set_act_motion_nextanm's 2-frame cross-fade into a new slot
            cr::Vec3 home, from, to;       // its place; a move (btlmove_setpos) from -> to
            float homeYaw = 0;
            int moveFrom = 0, moveTo = 0;  // the move's frames of the current slot
            int loopFrom = -1, loopTo = -1; // mdl_set_act_motion_loop's frames (-1: the slot's own)
            bool fading = false;           // a fallen monster
            float alpha = 1;
            bool hidden = false;           // the model's +0x55c cleared (a boss's end): not drawn
        };
        std::vector<Actor> actors;
        // Between the battle's frames (as App::Between for the field): each step's actors and
        // camera at its start and end, and how far into the next step the clock is
        struct Between {
            bool valid = false;
            float alpha = 1;
            std::vector<Actor> actors[2];
            cr::Vec3 eye[2], target[2];
            float roll[2] = {};
            int cam[2] = {-1, -1};
        } between;
        std::map<std::string, LoadedModel> models;
        int frames = 0;
        float clock = 0;
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
        enum Phase { Intro, Command, Acting, WinTalk, BossDown, Won, Results, Lost, Fled, RoundEnd } phase = Intro;
        int phaseFrames = 0;
        bool wonTap = false; // a tap in the victory's hold (state 1099: 140 frames or pad_chk_trg_up)
        bool lostTap = false;    // a tap on the defeat (0x513: at once, or after 40 frames of an event battle's game over picture)
        cr::Texture gameOver;    // btl_main 0x4b0: gameover.mct, drawn fading in over 20 frames for a lost event battle
        bool ending = false;     // btl_main 9000 -> 0x238b: the mask to black over 10 frames (SetMask(0): 500 ms), the music out, 12 frames, then the way out
        int endFrames = 0, pendingExit = 0;
        int idleSince = -1;      // the action's frame its actors came to rest (btlact_attack 1999: ten frames more, then the next)
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
        // The pages as drawn (__menub_mainPrc's slides, +0x38): each page comes in from the right (320 -> 0,
        // 80 a frame; 480, 120 in the Android layout: battleSlideDistance) and goes out again before the next
        // comes in; it takes taps only once in. The page the logic is on is `page`; the view follows it
        struct MenuView {
            int page = -1;                 // the page shown (-1 none)
            Menu menu = Top;
            int cursor = 0, member = 0, pick = 0, pickTop = 0, confirm = 0, targetAction = 0;
            bool pair = false;
            std::vector<int> picks;
        } view;
        int page = 0;                      // bumped at each page change
        int viewState = 0;                 // 0 out, 1 coming in, 2 in, 3 going out, 4 held out
        int slide = 320;                   // the view's offset to the right
        int slideHold = 0;                 // frames held out before the next page (Defend's 20: state 199)
        int menuGone = 100;                // frames since the view went out with no page to show (0x270e, then 500 / 600)
        bool roundWait = false;            // the round's first action waits for the menu to go
        bool turnOn = false;               // an action is playing
        cr::BattleTurn turn;
        int turnFrame = 0, hitFrame = 0;
        int bounceAt = -1;                 // a reflected gene's turn: when it comes back (-1 not yet met)
        bool hitsShown = false;
        struct Popup { cr::Vec3 at; int amount = 0, type = 0, timer = 0; bool mp = false, on = false; float y = 0; };
        Popup popups[5];                   // __dmg_set's five slots (+0x988, 0x28 apart; a sixth is dropped), y as last put
        std::string banner;                // the top bar: the action, the target prompt, messages
        int bannerFrames = 0;
        // ... as drawn: a font slot's text shows from the frame after it is set (font_set_utf16; drawfont_btl a
        // frame on)
        std::string bannerShown;
        std::vector<cr::BattleNotice> notices; // G+0xa14: the calcs' messages, one per free banner
        int shownHp[5] = {}, shownMp[5] = {}; // what the HUD shows (changes as the hits land)
        int rollHp[2] = {}, rollMp[2] = {};   // the rows' numbers and bars rolling toward those (+0x60 / +0x68)
        int hudSlide = 0;                     // the left HUD's offset to the left while a talk runs (+0x3c: 120 a frame to 480)
        int gaugeShown = 0;                // the badge's gauge, a step every other frame toward the rules'
        float breakPop = 0;
        uint32_t steps = 0;                // battle steps taken (CRView::prc's +0x3c6a8 count: its parity paces the gauge)
        struct Shadow { int fighter = -1; float alpha = 0; cr::Vec3 at; float rx = 0, rz = 0; }; // _btl_shadow's fans drawn (tests)
        std::vector<Shadow> shadows;
        std::vector<float> shadowTris;     // their triangles this frame (xyz x 3 each)
        int introStep = 300;               // the opening's btl_main state past 200: 300 (the counter), 400, 499
        bool roundFirst = false;           // btl_main 500 / 600: the round's first action doesn't wait for the banner
        std::vector<int> levelled;
        bool settled = false;              // the save has the battle's results
        int exit = 0;                      // 1 back to the field (fled), 2 to the title (lost), 4 won
        struct Result {                    // __menuu_MbResult*: the results' stages (_gstMbResult)
            int stage = 0;                 // 0 counting (3), 1 a level-up's line (5 / 6), 2 an item's (7 / 8), 3 going (10..)
            int count = 0, frames = 0;     // DAT_000e50e0: counting frames left ((most levels + 1) x 60); frames in
            int message = 57007;           // the board's one line: Victory!, then 57008 / 57034, 57010 / 57035
            std::string name;
            int value = 0;
            int character[2] = {-1, -1}, level[2] = {}, levelTo[2] = {}, levels[2] = {}, hp0[2] = {};
            float exp[2] = {}, expTo[2] = {}, expStep[2] = {};
            float shownExp = 0, shownOz = 0, shownPurse = 0;
            int expTotal = 0, ozTotal = 0, purseTo = 0;
            std::vector<std::pair<int, int>> ups, items; // (member, levels), (item, how many)
            size_t upNext = 0, itemNext = 0;
        } result;
        cr::Mat4 vp;                       // the last frame's camera (the damage numbers)
        int cam = -1, camActor = 0, camFrame = 0; // btlcamera_set: the camera (+0xa8e), its actor (+0xa8c), its count (+0xa88)
        int camDur = 0, camHold = 25;      // +0xa8a its frames; +0xae0 as its 10000..10002 read it
        bool camLive = false;              // +0xa80 set: btlcamera_prc still runs it
        bool camHeld = false;              // +0xa90: set held (no easing)
        int camSets = 0;                   // btlcamera_set calls (tests)
        float camEase = 1;                 // +0xfc / +0x100: the shown camera's divisor (5 eased)
        bool camSnapped = false;           // _Clr_evcv this frame: the shown camera cut to the wanted
        float camRoll = 0;                 // +0xf8: degrees about the view (btl_prc's gluLookAt under flag 0x100)
        int attackVariant = -1;            // que +0xc: _btlact_attack_init's random(3) (the camera's variant)
        int attackCam2At = -1;             // que +0x10: the approach's move end (a variant-2 member blow's second camera)
        cr::Vec3 camEye{488, 12, 555}, camTarget{500, 4, 498}, camWantEye{488, 12, 555}, camWantTarget{500, 4, 498};
        std::vector<Effect> effects;       // eff3d: playing effects (32 at most)
        // The voiced releases' battle lines (2.2.0's _btlvoice_*): each fighter's next variant of
        // three (+0xd3), who of a pair speaks this round (+0xd2), the command lines said (+0xafc)
        int voiceVariant[5] = {};
        bool voiceSpeaks[5] = {};
        bool commandSaid[2] = {};
        int voiceRound = -1;
        int voiceStanding[2] = {};         // FUN_00026e5c's count per side (party, monsters) as an action's blows land
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
    bool captureSettleMoving = false;      // --settle-moving n: n frames a game frame apart instead
    int captureSettleFps = 0;              // --settle-fps n: those frames 1/n s apart (60: as drawn between steps)
    int captureEvent = -1;                 // --event n: start the area's script n in the room first
    int captureArea = -1;                  // --area n: the event file for the map (routes share maps)
    std::vector<int> captureFlags;         // --set-flags a,b,...: story flags set before the map loads
    bool captureWinBattles = false;        // --win-battles: a battle the scripts start is won at once
    bool testNoTutorials = false;          // self-test: battles don't stop for their tutorials (EvrStart 2)
    int captureTalk = -1;                  // --talk-portrait n: (new game) the n-th hall conversation showing a portrait
    int lastMessageId = -1;                // the last message a script showed (CR_CAPTURE_UNTIL_MSG)
    int captureMenu = 0;                   // --camp (1), --save-screen (2: save, 3: the title's load / new, 7: cleared), --char-select (4), --scenarios (5), --manual (6), --world-select (8), --last-battle (9)
    std::string captureCampView;           // --camp-view items|use|equip|slots|wear|status
    std::string captureFieldMenu;          // --field-menu closed|open|known|off|map[step]
    std::string captureTouchPad;           // --touch-pad dx,dy[,chest]: the pad held that far from the screen's middle
    int captureSceneFx[3] = {-1, 0, 0};    // --scene-fx set effect [frames]
    std::string captureBattleView;         // --battle-view top|commands|targets|items|escape|hit|results
    std::string captureShop;               // --shop main|category|member|buy|items|sell|count|equip|talk
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
        bool faded = false;  // mdl_set_alpha has run on it: its colour array off for good (fadedMaterial)
        cr::MotionBlend blend; // the 2-frame cross-fade into a new action
    };
    std::vector<NpcDraw> npcDraws;         // this map's visible NPCs
    // A puzzle room (PuzzleManager, game/puzzle.hpp): its objects - the room's NPCs that have
    // NPC table +20 bits - on a grid of 3 layers (0 the floor's switches, 1 blocks and gems, 2
    // the warp crystals), the touch ability's state and the ESP count
    struct Puzzle {
        int stage = -1;                    // -1: not in a puzzle room
        int esp = -1;                      // uses left (-1 no limit)
        int pyroCool = 0;                  // pyrokinesis (FUN_0006e558, +0x1c): frames left before another tap counts (20 after one)
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
        bool pkPointerCancelled = false; // keep a held block, but require a fresh drag after interruption
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
        // _ArrowOn / _ArrowOff (menuf_mainPut draws them): 0 left, 1 up, 2 right, 3 down around a
        // lifted block - on (1) / off (-1), the fade's frames left and kind (2 the slide's, 1 a
        // let-go's), the colour's alpha byte, the screen point (tex2d)
        struct Arrow { int on = -1, timer = 0, mode = 0, alpha = 0xff; float x = 0, y = 0; } arrows[4];
        // CheckTap (teleport / toughness / pyrokinesis): 1 a touch held, 2 a tap (released within
        // 5 frames), 3 held too long; the touch's start (tex2d)
        struct Tap { int state = 0, frames = 0; float x = 0, y = 0; bool released = false; } tap;
        bool pkHeld = false;               // PsychokinesisDetector: the touch that took hold still down
        float pkAtX = 0, pkAtY = 0;        // ... where it is now (tex2d)
        cr::Vec3 leaderWas;                // FieldPCCtrl + 0x28: the leader moved since the last frame
        bool leaderMoved = false;
        int padHold = 0;                   // gac + 0x3cde4 (2.2.0): 3 frames after the touch pad was held (taps refused)
        // ScrollCamera: a second finger within 6 frames of the first scrolls the view to it (state
        // 1, the ability paused) until the next single touch lets it go; phase / count its checkTouch
        struct Scroll { int state = 0, phase = 0, count = 0; float x = 0, y = 0; } scroll;
    } puzzle;
    int padLock = 0;                       // gac + 0x3cde6 (2.2.0): the puzzle pad's buttons and Camp / Save refused while > 0
                                           // (Util 2038 sets it from var 9: FUN_00026d1c)
    struct Fingers { int held = 0, fresh = 0; float x = 0, y = 0; } fingers; // SDL touch: down now, new since the last frame (the last one's frame pixels)
    float puzzleStartHeading = 0;          // ... and its heading (FieldPCCtrl::reset)
    float playerAlpha = 1, playerAlphaFrom = 1, playerAlphaTo = 1; // mdl_set_alpha on the leader
    int playerFadeFrames = 0, playerFadeLeft = 0;
    bool playerFaded = false;              // mdl_set_alpha has run on the leader's model (until it is read again)
    cr::Vec3 puzzleStart;                  // where the leader came into the puzzle room (FieldPCCtrl::reset)
    struct CrystalCam { int npc = -1; int phase = 0; } crystalCam; // Uty_WarpCrystalCamera: 0 to the crystal, 1 back
    bool puzzleStartPending = false;
    // G+0x358e0: the last play request __bgmgo took on the field (G+0x2c0 bit 6), the flags with it;
    // __EvtBattle zeroes it. Uty_GetBgmNo reads it and the field plays it again after a battle
    uint32_t bgmNo = 0;
    struct NowLoading { bool on = false; int frame = 0; } nowLoading; // NowLoadProc's spinner
    struct VitaLogo {                      // the Vita's boot opcodes 0x47..0x4a: a full-screen logo
        cr::Texture tex[2];                // by slot (0x47's slot + 16 in the original's Tex2d)
        int slot = -1, phase = 0, frame = 0, in = 0, hold = 0, out = 0;
        bool skip = false;                 // a tap / confirm in the hold: on to the fade-out
        bool warning[2] = {false, false};  // (the slot's picture the warning: the whole of its sheet)
        bool fromVita[2] = {false, false}; // (the slot's picture the Vita's own: framed 960 x 640, not the iPad's 1024 x 768)
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
    struct ManualMsg {                     // the manual's page (OptionPrc 0x12..0x16: MsgUty_WinOpen / SetMsg)
        int state = 0;                     // 0 the list, 1 its windows going (0x12), 2 the message up (0x13 / 0x14),
                                           // 3 its window closing (0x15 / 0x16)
        int count = 0, id = 0;             // 0x12's 4 frames; the message (56000 + item - 439, 56035)
        std::vector<std::vector<std::string>> pages;
        size_t page = 0;
        int typed = 0;                     // the page's characters out (PrcMsg)
        bool picture = false;              // one of MsgUty_SetMsg's five: its picture, the window at _mwin[4]
        MsgWin win;                        // PrcWin: the window's fade
        TutoTex pic;                       // MsgUty_TexDisp*: the picture's pages and frames
        float clock = 0;
    } manualMsg;
    bool exclamation = false;              // Uty_DrawExclamationMark: the dimmed screen and "!" (flag 0)
    struct PuzzleBanner {                  // Uty_PuzzleStart (call 2003): two pieces sliding apart into place
        bool on = false;
        float x1 = 100, y1 = 68, x2 = 156, y2 = 140; // tex2d
        int alpha = 0;
    } puzzleBanner;
    std::map<std::string, struct LoadedModel> npcModels; // NPC model cache by file
    std::map<int, std::vector<cr::TreasureBox>> treasureBoxes; // per area: selected boxes and items
    bool treasureReroll = false;           // G+0x2c0 bit 0x40000000: set by the World Select's choice
    bool showPlayer = true;                // P: the party leader walking on the field
    int playerChar = 0;                    // O: which field model, f001..f008
    LoadedModel player;
    std::string playerName;
    // the leader's model read by Set_Prtyno (read_mdl: unlit) rather than with the map (fldread_prc's
    // read_mdl_set -> mdl_mdl_init -> read_mdl_mem_mcm: lighting as the file has it)
    bool playerByReadMdl = false, playerLoadedByReadMdl = false;
    cr::FieldWalker walker;
    bool havePlayer = false;
    float walkClock = 0;                   // movement runs in fixed 20 Hz steps like the original
    // Between the game's frames: each field step's values at its start and its end - the leader,
    // the NPCs drawn, the script's camera, the scroll - and how far into the next step the clock is
    // (alpha). The frames drawn between steps show those values where they are by then (shown*());
    // only a value still as its step left it is shown so: whatever moved it since (a warp, a test
    // stepping the game itself, a new room) is drawn as it is.
    struct Between {
        bool valid = false;
        float alpha = 1;
        cr::Vec3 walker[2];
        float heading[2] = {0, 0}, playerAlpha[2] = {1, 1};
        struct Npc {
            int index = -1, id = 0;
            cr::Mat4 world[2];
            float alpha[2] = {1, 1};
        };
        std::vector<Npc> npcs;
        cr::Vec3 eye[2], target[2];
        float scroll[2][2] = {{0, 0}, {0, 0}};
    } between;
    float walkExpDistance = 0;             // G+0x35520: walked with skill 174 worn, 10 units = 1 EXP banked
    struct FieldMenu {                     // menuf_mainPut / Prc: the corner button and the bar it opens
        bool open = false;                 // G+0x3553c
        int anim = 0;                      // the bar's 4 frames of sliding (in or out)
        int fade = 0;                      // +32 a frame (to 320) on the field, -32 while a scene runs
        float clock = 0;
        int cursor = 0;                    // the Vita's: the bar's button under the buttons' cursor (DAT_81113a74)
    } fieldMenu;
    struct StickPointer {                  // a puzzle room's touch from a controller (the PS TV's way: a pointer the
        bool shown = false, down = false;  // right stick moves, R3 the finger): its place in window pixels
        float x = 0, y = 0;
        int stage = -1;                    // the room it was put in (a new room: back to the screen's middle)
    } stickPointer;
    struct TouchPad {                      // CrossKey: the field's touch pad, where a finger went down
        bool down = false;                 // a finger (or the mouse) holds it
        float ox = 0, oy = 0;              // where it went down (tex2d)
        float dx = 0, dy = 0;              // the finger from there (tex2d = the game's touch units)
        int hold = 0;                      // frames before it shows unmoved (5)
        bool shown = false;                // _SetFldMenu_pad_visible
        float knob[2] = {0, 0};            // _fldpad: the knob's offset, easing toward the finger
    } touchPad;
    bool npcNativesPending = false;        // npc_setup: the rows' natives (+8) to run for the room's NPCs as they are created
    bool bigPad = false;                   // the UI atlas has 2.2.0's larger pad (iPhone, Vita, Android)
    float promptX = -1, promptY = -1;      // the "!" bubble's centre as drawn (logical), -1 none
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
        float edgeY = 0;                   // the Android phone's edges beyond the 320 (androidEdgeY; 0 else)
        float lineStep = 0;                // the lines' rise a frame (the layout's: kLine, kPhoneLine, Android's)
        struct Line { bool on = false; float y = 0; int entry = 0; } lines[32];
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
        float scroll = 0, levelScroll = 0;  // MuScrList: the rows' offset, on its way to 48 x the focus row
        int mark = 0;                      // the board's mark (one a frame)
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
    float drawFrames = 0;       // this frame's length in 20 Hz frames (the game's dt)
    float texSpin = 0;          // _mdl_put's _vEuler z: texture 999's turn (0..1)
};

// The skip corner's sources (App::skipHeld): the scene skips while any is held
constexpr unsigned kSkipKey = 1, kSkipPad = 2, kSkipPointer = 4;

// The menus' sounds: __effgo ids (sndeff.bin entries) as the original's *Prc functions use them
namespace se {
constexpr uint32_t kMenuBase = 0; // (Omega's menu sounds sit 0x91 later)
constexpr uint32_t kChoose = 2;  // a plate, button, toggle, Yes or No (ComUty_YesNoChoiceProc's own is 2 too)
constexpr uint32_t kBack = 3;    // the Back bar (ComUty_BackButtonProc); Options' < > page arrows on it as well
constexpr uint32_t kBuzzer = 4;  // refused: nothing to sell (BuySell_CommandProc), a count past its end
                                 // (ComUty_NumCountProc), a pair-only command alone (menub_mainPrc)
constexpr uint32_t kPage = 5;    // the Back bar's < > arrows (StartPrc, MuCamp_NextProc), a list flicked a page
constexpr uint32_t kWinOpen = 6, kWinClose = 7; // a script's message window opening / closing (__EvtOpen / __EvtClose)
constexpr uint32_t kCampOpen = 8, kCampClose = 9; // menuf_mainPrc's Camp button, CampMainPrc's Back bar
constexpr uint32_t kSaved = 11;  // SaveLoadPrc, after SaveData_Save
constexpr uint32_t kLoaded = 12; // SaveLoadPrc: Yes to Load / New
constexpr uint32_t kPaid = 22;   // ShopPrc
constexpr uint32_t kVolume = 40; // MuOptionSetInfo: the SE gauge let go (the BGM gauge is silent)
// The original is touch-only, so it has no cursor sound; keyboard cursor moves are the port's own
constexpr uint32_t kCursor = 4;
// MuScrList_Draw (2.2.0 0x57b28): __effgo(1) whenever another row comes to a list's focus line -
// every MuScrList list (the Camp's, the shop's, the World Select's, the manual's, the battle's)
constexpr uint32_t kListFocus = 1;
} // namespace se

// Control Hints (Options, Game; F3): the port's own menu help. The iPhone layout has no room below.
constexpr int kManualFirstPage = 56000, kOptionIntroPage = kManualFirstPage + 0x23; // (block 5; OptionPrc 0: SetMsg(0x23))

// Field models of the party (the binary names f001/f003/f005/f007, one per route).
inline constexpr const char* kPartyModels[] = {"f001.bin", "f002.bin", "f003.bin", "f004.bin",
                                               "f005.bin", "f006.bin", "f007.bin", "f008.bin"};

// Temporal upscaling: output / render size per axis (FSR's and DLSS's modes)
struct UpscaleMode {
    const char* name;
    float ratio;
};
inline constexpr UpscaleMode kUpscaleModes[] = {
    {"Native AA", 1.0f}, {"Quality", 1.5f}, {"Balanced", 1.7f}, {"Performance", 2.0f}, {"Ultra Perf.", 3.0f}};
inline constexpr const char* kUpscalerName[] = {"Off", "FSR 3.1", "DLSS", "MetalFX", "FSR 4"};
inline constexpr cr::Upscaler kUpscaler[] = {cr::Upscaler::Off, cr::Upscaler::Fsr3, cr::Upscaler::Dlss, cr::Upscaler::MetalFx, cr::Upscaler::Fsr4};
constexpr int kUpscalers = 5;
// (MetalFX is Apple's own, FSR 4 - AMD's DLLs in fsr4/ - Linux's and Windows': the options step over the other's slot)
constexpr bool upscalerHere(int u) {
#ifdef __APPLE__
    return u != 4;
#else
    return u != 3;
#endif
}

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
    const cr::Texture* valueTex = nullptr; // Value: a picture in place of the text (2.2.0's language name)
    float valueUv[4] = {0, 0, 0, 0};
    bool enabled = true;
    std::string hint;             // shown under the rows while the cursor is on this one
    std::function<void(int)> act; // Gauge / Value: -1 / +1 (Value: 0 = Enter); Pair: 0 / 1; Check: 0
    int sound = int(se::kChoose); // played when act runs (OptionPrc: 2 for every button; gauges below)
    bool enterSounds = false;     // act(0) plays a sound of its own (Sound Test), so none from the menu
};

constexpr int kOptionPages = 5;
constexpr int kOptionsBackRow = 99;
extern const char* const kOptionPageName[kOptionPages];
// Layout, in menu units
constexpr float kOptRowTop = 52, kOptRowPitch = 36, kOptCtrlX = 224, kOptCtrlScale = 0.85f;
constexpr float kOptBackY = 344, kOptPageY = 350;
// The port's: rows shown at once (the rest scroll, clear of the help line and the Back bar), and
// the scroll bar at the rows' right
constexpr int kOptVisible = 7;
constexpr float kOptBarX = 486, kOptBarW = 8;

// The iPhone layout (2.2.0): the menus' coordinates are its screen's - (0, 0) the corner, not the
// iPad's 512 x 384 menu space - so a menu drawn there sits (+16, +32) menu units further in
struct PhoneMenu {
    cr::MenuPainter& m;
    PhoneMenu(const App& app, cr::MenuPainter& p) : m(p) { m.setOffset(app.renderer.phoneLayout() ? 16.0f : 0.0f, app.renderer.phoneLayout() ? 32.0f : 0.0f); }
    ~PhoneMenu() { m.setOffset(0, 0); }
};

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

// The viewer's side of a running script (see cr::EventHost).
struct ViewerHost : cr::EventHost {
    App& app;
    explicit ViewerHost(App& a) : app(a) {}
    std::vector<App::NpcDraw*> npc(int id);
    void showMessage(int id) override;
    bool messageOpen() override;
    bool tapped() override;
    void showItemWindow(int item, bool oz) override;
    void playAnim(int id, int slot, int loopFrom, int loopTo) override;
    void playAnim2(int id, int slot, int loopFrom, int loopTo, int start, int blend) override;
    void loadAction(int id, int action, int slot) override;
    void unloadAction(int id, int slot) override;
    bool animBusy(int id) override;
    void scaleNpc(int id, const float* s, int frames) override;
    void autoMove(int id, float x, float z, float speed) override;
    void fadeNpc(int id, float from, float to, int frames) override;
    void showNpc(int id, bool shown) override;
    void screenFade(float grey, float from, float to, int frames) override;
    bool screenFading() override;
    void backFade(float grey, float from, float to, int frames) override;
    bool backFading() override;
    void warp(const cr::WarpRequest& w) override;
    void playSound(uint32_t id) override;
    void playMusic(uint32_t v) override;
    void recoverPair() override;
    int bgmNo() override;
    void endMenu(int kind) override;
    int pairNo() override;
    void nowLoading(bool on) override;
    static constexpr float kDeg = 3.14159265f / 180.0f;
    void placeNpc(int id, const float* v, float yawDeg, Facing facing) override;
    void moveNpc(int id, const float* v, float yawDeg, Facing facing, int frames, int mode) override;
    void tex2d(int slot, uint16_t f, int image, const int16_t (*extra)[3]) override;
    void loadParty(int pair, bool clearData) override;
    void loadNpc(int id) override;
    void freeCamera(bool on) override;
    void setCamera(const float* eye, const float* target, int frames) override;
    void panCamera(const float* d, int frames) override;
    void sceneEffectSet(int set) override;
    void quake(int amp, int from, int to, int frames) override;
    void vitaLogo(int slot, int picture) override;
    void vitaLogoShow(int slot, int in, int hold, int out) override;
    bool vitaLogoBusy() override;
    void texDisp(const int32_t* g) override;
    void tutoTexDisp(const int32_t* g) override;
    void exclamationMark() override;
    void openWindow(int style, int lines) override;
    void closeWindow() override;
    bool windowClosed() override;
    void toggleFieldHidden() override;
    bool logoSeen() override;
    void setLogoSeen() override;
    void fireWall(int npc) override;
    void padArt() override;
    void battleParamUp(int fighter) override;
    void puzzleStart() override;
    void puzzlePadLock(int n) override;
    void warpCrystalCamera(int npc) override;
    void sceneEffect(bool on, int id) override;
    void frameEffect(int actor, int effect, int scale, const float* v, bool angled) override;
    void stopFrameEffect(int effect, int actor) override;
    void npcEffect(int effect, int npc, int dy) override;
    void mapScroll(int npc, float x, float y, float rate) override;
    void lookAtNpc(int npc) override;
    void battle(int group, int stage, int flags) override;
    bool menu(int n) override;
    void askYesNo(int flag) override;
    bool choiceOpen() override;
    void faceNpc(int id, int target) override;
};

namespace campui {
inline constexpr int kWords[5] = {8, 9, 10, 11, 3};                 // Items, Equip, Genes, Status, Options
inline constexpr bool kReady[5] = {true, true, true, true, true};
constexpr int kEquipSlots = 7;                                 // weapon, armor, jewel, Human Genes, 3 plates
constexpr int kEntries = 6;                                    // and Save (1.0.1's iPad; 2.2.0 has five)
constexpr float kPlateX = 328, kPlateY = 48, kPlateStep = 48;
constexpr float kMemberY = 176, kMemberStep = 56;
constexpr int kCategories = 6;
inline constexpr int kCategoryKind[kCategories] = {14, 3, 6, 7, 8, 10}; // make_item_list (0xbd050)
inline constexpr int kCategoryLabel[kCategories] = {58071, 58072, 58073, 58074, 58075, 58079}; // (0xbd030)
constexpr float kRowX = 192, kRowY = 92, kRowH = 48;
constexpr int kRows = 4;
// MuScrList mode 0: rows at x 192, the focus row's line at y 150, the rows cut to 76..280 (the mask
// quads over 0..76 and 280..356), the gauge at (456, 72), 208 high
constexpr float kFocusY = 150, kListTop = 76, kListBottom = 280, kGaugeX = 456, kGaugeY = 72, kGaugeH = 208;
constexpr float kBackX = 192, kBackY = 344;
constexpr float kPhoneBackY = 286; // 2.2.0's (__MuCamp_NextProc's box (192, 286, 256, 34)); it has no Save plate
// kanIconNoTbl (0xc8740): a plate word's icon + 1
inline constexpr int kIconOfWord[37] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 14, 13, 13, 14, 13,
                                 14, 15, 16, 17, 18, 19, 20, 21, 23, 24, 25, 26, 27, 28, -1, -1, 14, 14};
constexpr uint32_t kSoundEquip = 21, kSoundUse = 20; // __ChangeEquipItem's 0x15; an item used (__MuCamp_NextProc 6 -> Yes) 0x14
constexpr int kRemove = 200;                                   // the lists' "Remove" entry (item 200, __ChangeEquipItem)
} // namespace campui

namespace staffroll {
constexpr float kLine = 2.826087f, kPic = 0.22217391f, kPicFast = 2.2217391f, kPicEnd = -1022, kPanelEnd = -280;
constexpr int kLines = 336;
// 2.2.0's (tex2d units): 17 lines 20 apart from y 320 (x 16), rising 1.9217 a frame, the next at
// 320 past -20; the picture 0.125 (1.25) a frame up to -575; its block 9 has 434 lines
constexpr float kPhoneLine = 1.92174f, kPhonePic = 0.125f, kPhonePicFast = 1.25f, kPhonePicEnd = -575;
constexpr int kPhoneShown = 17, kPhoneLines = 434, kAndroidLines = 428;
}

namespace mapui {
extern const short kMapSpot[64][2];
extern const short kMapMark[4][25][2][3];
constexpr int kFlagBase = 2601, kFlagStride = 50, kSteps = 25;
} // namespace mapui

constexpr float kPhoneHistoryLowest = 285 - (62 + 636 + 40) - 8; // the vertical scroll's end (tex2d)
constexpr int kManualFirstItem = 439, kManualLastItem = 478;
extern const float kWinMessagePanel[4];
extern const float kWinSmallPanel[4];

// The battle HUD (menub_mainPut and its cmd_* pieces), in the menus' 512 x 384 units: the
// tex2d calls' 480 x 320 space moved by (16, 32). Everything comes from window_hd (slot 44) or
// plain colour (slot 45).
namespace btlui {
constexpr float kButtonX = 344, kButtonY = 128, kButtonStep = 48; // cmd_bottom at tex2d (328, 96 + 48k)
constexpr float kPlateX = 224, kPlateBottom = 312;                 // cmd_bottom_teki: y = 312 - 48 (n - i)
constexpr float kBackX = 208, kBackY = 320;                        // cmd_back at tex2d (192, 288)
// The first menu: Solo, Pair, Escape, Options; then Attack, Genes, Items, Defend (icon, word)
inline constexpr int kTopIcon[4] = {15, 14, 16, 3}, kTopWord[4] = {21, 20, 22, 3};
inline constexpr int kCmdIcon[4] = {18, 10, 8, 19}, kCmdWord[4] = {24, 10, 8, 25};
// The Break badge's eight ring pieces (0xd1fd0): x, y, u, v, 32 x 32 each
inline constexpr short kBreakTick[8][4] = {{4, 4, 96, 120},    {4, 4, 64, 120},    {4, 28, 64, 152},  {4, 28, 96, 152},
                                    {28, 28, 128, 152}, {28, 28, 160, 152}, {28, 4, 160, 120}, {28, 4, 128, 120}};
// __dmg_prc: the digits' hop (0xcb398) and the label's (0xcb390), by (383 - timer) / 16
inline constexpr int kDigitHop[8] = {16, 20, 15, 10, 5, 0, 0, 0}, kLabelHop[8] = {3, 6, 12, 6, 3, 0, 0, 0};
constexpr int kPopupTime = 0x17f; // __dmg_set; 16 off a frame
constexpr int kItemRows = 4;      // the battle lists' rows shown, 48 apart (the plates 44 tall, as the camp's)
constexpr float kItemPitch = 48;
constexpr int kBannerFrames = 40;  // font_set_utf16's 0x28 (the action's name)
constexpr int kMessageFrames = 60; // 0x3c: the calcs' messages and the queued ones
// Action slots (BattleAnim): 0 standing, 1 victory, 3 back to place, 4 defending, 5 hit,
// 6 fallen, 7 getting up; attacks use their class (8-10), genes 11, items 12
constexpr int kSlotIdle = 0, kSlotWin = 1, kSlotReturn = 3, kSlotGuard = 4, kSlotHit = 5, kSlotDown = 6, kSlotRise = 7; // (7: getting up - btlcalc_0424)
} // namespace btlui

constexpr int kIntroWhite = 4; // the steps under the encounter's white before btl_main 200 (10, 100, 199 - then 200)

namespace battletalk {
constexpr int kFlag = 4095;
} // namespace battletalk

// assets_io.cpp
void holdSkip(App& app, unsigned source, bool held);
void clearSkip(App& app);
bool skipCorner(const App& app, float lx, float ly);
bool spinsTexture(const cr::Model& m, size_t mesh);
void spinUvs(std::vector<float>& uv, float turns);
void modelDrawn(App& app);
void menuSound(App& app, uint32_t id);
SDL_EnumerationResult SDLCALL collectAssets(void* user, const char*, const char* fname);
int wrap(int i, size_t n);
void updateTitle(App& app);
void showTexture(App& app, int index);
void applyHd(App& app, cr::Image& img);
bool loadImage(App& app, const std::string& mct, cr::Image& img, bool* fromAndroid = nullptr);
bool loadOriginalImage(App& app, const std::string& mct, cr::Image& img, bool* fromAndroid = nullptr);
bool openVita(App& app, const std::string& path);
bool startSound(App& app);
void vitaAudio(App& app);
void chooseVitaFolder(App& app);
void takeVitaFolder(App& app);
bool findAndroid(const std::string& path, cr::ObbArchive& out, std::string* error);
bool openAndroid(App& app, const std::string& path);
void chooseAndroidFolder(App& app);
void takeAndroidFolder(App& app);
std::string storedVitaPath();
std::string storedSetting(const char* key);
void unloadLoaded(App& app, LoadedModel& lm);
void unloadModel(App& app);
std::vector<bool> packageActionsKept(const cr::Model& m);
void loadModelFile(App& app, const std::string& name, LoadedModel& out, bool character);
void mdlMemRules(LoadedModel& lm);
void readMdlRules(LoadedModel& lm, const std::string& name);
LoadedModel& npcModel(App& app, const std::string& file);
void showModel(App& app, int index);
void applyMix(App& app);
std::string userDir();
std::string settingsPath();
void loadMix(App& app);
bool seenFlag(const App& app, int f);
void markSeen(App& app, int f);
void saveMix(const App& app);

// scene_fx.cpp
void loadSceneEffectSet(App& app, int set);
void endSceneEffects(App& app);
void setSceneEffect(App& app, bool on, int id);
void stepSceneEffects(App& app);
void drawSpaceBackdrop(App& app, bool battle);
void drawSceneLayer(App& app, int slot, float x, float y, float w, float h, float u, float v, int alpha);
void drawSceneEffects(App& app, bool battle = false);
bool paintedField(const App& app);
bool scrollingField(const App& app);
bool vitaLayout(const App& app);
float fieldViewW(const App& app);
float fieldViewH(const App& app);
float fieldFillZoom(const App& app);
bool fieldViewPoint(const App& app, const cr::Vec3& p, float& vx, float& vy);
void scrollFieldTo(App& app, float vx, float vy, float rate);
void followPlayerScroll(App& app);
void stepFieldScroll(App& app);
bool stepMove(App& app, App::Move& m);
bool stepRescale(App& app, App::Rescale& r);
void stepScriptCam(App& app);
void scriptFieldScroll(App& app, float x, float y, int frames);
void loadVideo(App& app, const char* text);
void applyOrientation(App& app, bool captureDirection = false);
void applyVideo(App& app);
std::string sysLabel(const App& app, int id, const char* fallback);
std::vector<OptRow> optionRows(App& app, int page);
void openOptions(App& app, int page);
void optionsScrollFix(App& app, int n, bool follow);
void optionsBarTo(App& app, float my);
void optionsScrollBy(App& app, int rows);
void phoneMenuPoint(const App& app, float& mx, float& my);
float wideLeft(const App& app);
float wideRight(const App& app);
float androidEdgeX(const App& app);
float androidEdgeY(const App& app);
void menuPointAt(const App& app, float lx, float ly, float origin, float& mx, float& my);
void menuPointFit(const App& app, float lx, float ly, float& mx, float& my);
void menuPoint(const App& app, float lx, float ly, float& mx, float& my);
void stepOptions(App& app);
void resetAnswer(App& app, bool yes);
void optionsKey(App& app, SDL_Keycode key);
int optionsBackPart(float mx, float my);
void optionsClick(App& app, float px, float py);
void stepTexDisp(App& app);
void stepVitaLogo(App& app);
void stepTutoTex(App& app);
void stepTutoFrames(App::TutoTex& t);
void drawTutoPicture(App& app, const App::TutoTex& t);
float screenFadeAlpha(const App& app);
float backFadeAlpha(const App& app);
void drawItemBand(App& app);
std::string itemBandText(const App& app);
void updateVoices(App& app);
int androidCutinWidth(float wv);
void drawMessageBox(App& app, const std::vector<std::string>& page, const std::string& speaker, bool next, const std::string& pageNo);
void drawWinPiece(App& app, const float* piece, float x, float y, float w, float h, float alpha = 1);
void selectAction(App& app, int delta);
void fieldDepthRange(const App& app, const cr::Vec3& eye, const cr::Vec3& target, float& zNear, float& zFar);
cr::Mat4 scriptCameraView(const App& app, const cr::Vec3& eye, const cr::Vec3& target);
void loseToArk(App& app);
int menuMaskWanted(const App& app, bool& atOnce);
void settleMenuMask(App& app);
void drawFrame(App& app, bool present = true);
void setMode(App& app, Mode mode);
void step(App& app, int delta);

// menus.cpp
std::string savePath(const App& app, const char* file);
cr::SaveData captureSave(const App& app);
bool applySave(App& app, const cr::SaveData& s);
bool languageSheet(App& app);
std::string slotPath(const App& app, int pair);
std::string dataPath(const App& app);
bool readData(const App& app, cr::SaveData& d);
bool saveGame(App& app);
bool foldVitaImport(App& app, const std::vector<std::string>& files);
int currentPair(const App& app);
std::vector<int> savePlates(const App& app);
void openSaveScreen(App& app, bool load);
void closeSaveScreen(App& app);
void saveScreenYes(App& app);
void saveScreenChoose(App& app, int i);
void drawSaveScreen(App& app);
void stepSaveScreen(App& app);
void saveScreenKey(App& app, SDL_Keycode key);
void saveScreenClick(App& app, float px, float py);
int ownedCount(const App& app, int item);
void openShop(App& app);
void closeShop(App& app);
void stepShop(App& app);
void shopChooseMain(App& app, int i);
void shopChooseCategory(App& app, int c);
void shopChooseMember(App& app, int k);
void shopChooseItem(App& app, int index);
void shopPay(App& app);
std::string shopMessage(const App& app);
void drawShopNumber(cr::MenuPainter& m, int value, float x, float y, int digits, float step, float alpha, int style = 0, bool left = false);
void drawShop(App& app);
void shopKey(App& app, SDL_Keycode key);
void shopClick(App& app, float px, float py);
bool keyHints(const App& app);
int plateIcon(int word);
int campCharacter(const App& app, int member);
int campMembers(const App& app);
void drawClockAndOz(App& app, cr::MenuPainter& m, float a);
void drawFaceBoard(App& app, cr::MenuPainter& m, float x, float y, int c, int hp, int hpMax, int mp, int mpMax, float a, const cr::GameState* gs = nullptr);
void memberHpMp(const App& app, int c, int& hp, int& hpMax, int& mp, int& mpMax, const cr::GameState* gs = nullptr);
float campEase(const App& app);
float campListX(const App& app);
int campScrollCount(const App& app);
void campScrollBy(App& app, int rows);
int shopUnitPrice(const App& app, int item);
std::vector<int> campItems(const App& app, int category);
std::vector<int> campEquipList(const App& app, int c, int slot);
bool campFreeJewel(const App& app);
bool campCommonSkill(const App& app);
int staffShown(const App::StaffRoll& st);
int campFocus(const App& app);
int campEntries(const App& app);
void openCamp(App& app);
void campNotice(App& app, int id);
void campOpenPage(App& app, int i, bool sound = true);
void campChoose(App& app);
void stepCamp(App& app);
void campKey(App& app, SDL_Keycode key);
void campClick(App& app, float px, float py);
void campPointerDown(App& app, float px, float py);
void campPointerMove(App& app, float px, float py);
void campPointerUp(App& app, float px, float py);
void drawCampRow(App& app, cr::MenuPainter& m, float x, float y, int item, int count, bool lit, float a, int style = 0, const std::string& label = std::string());
void drawCampScrollBar(cr::MenuPainter& m, float scroll, int n, float a);
int staffLines(const App& app);
float staffPicEnd(const App::StaffRoll& st);
void openStaffRoll(App& app, int mode);
std::string staffLine(const App& app, int entry);
void stepStaffRoll(App& app);
void staffRollTap(App& app);
void drawStaffRoll(App& app);
std::string lastBattlePlate(const App& app, int p);
void openLastBattle(App& app);
void lastBattleChoose(App& app, int i);
void lastBattleAnswer(App& app, bool yes);
void stepLastBattle(App& app);
void drawLastBattle(App& app);
void lastBattleKey(App& app, SDL_Keycode key);
void lastBattleClick(App& app, float px, float py);
void openWorldSelect(App& app);
void closeWorldSelect(App& app);
void worldSelectChoose(App& app, int pos);
void worldSelectLevel(App& app, int n);
void worldSelectAnswer(App& app, bool yes);
void stepWorldSelect(App& app);
void drawWorldSelect(App& app);
void worldSelectKey(App& app, SDL_Keycode key);
void worldSelectClick(App& app, float px, float py);
void drawHelpWindow(App& app, cr::MenuPainter& m, int item, float a, float dx = 0, float dy = 0);
void drawCamp(App& app);
void writeResume(App& app);
void trackerSceneEnd(App& app);
void checkTrophies(App& app);
void setTrophyBit(App& app, int bit);
void loadTrophyPack(App& app);
std::string trophyName(const App& app, int n);
char trophyGrade(const App& app, int n);
void stepTrophies(App& app, float dt);
void drawTrophyNotice(App& app);
void answerResume(App& app, bool yes);
void drawResumePrompt(App& app);
void stepResume(App& app);
void resumeClick(App& app, float px, float py);
void newPairParty(App& app, int pair, bool clearData);
cr::GameState allPairsFlags(const App& app);
void openScenarios(App& app);
void closeScenarios(App& app);
int scenarioState(const cr::GameState& g, int i);
int scenarioPercent(const cr::GameState& g);
void drawScenarios(App& app);
void scenariosScrollBy(App& app, float d);
void scenariosScrollSideways(App& app, float d);
void scenariosKey(App& app, SDL_Keycode key);
std::vector<int> manualTopics(const App& app);
std::vector<std::string> manualPage(const App& app, int id);
void manualTap(App& app);
void openManual(App& app, int intro);
void manualChoose(App& app);
void stepManual(App& app);
void drawManual(App& app);
void manualKey(App& app, SDL_Keycode key);
void manualClick(App& app, float px, float py);
void openCharSelect(App& app);
void charSelectTurn(App& app);
void closeCharSelect(App& app, int pair);
void drawCharArt(App& app, float alpha, bool banner, float grey = 1);
void drawCharSelect(App& app);
void charSelectKey(App& app, SDL_Keycode key);
void charSelectClick(App& app, float px, float py);
void drawTitleMenu(App& app);
void titleStart(App& app);
void titleChoose(App& app, int i);
bool titleKey(App& app, SDL_Keycode key);
void titleClick(App& app, float px, float py);

// field_puzzle.cpp
void showMap(App& app, int index, int forcedMap, int forcedArea);
void showMap(App& app, int index);
bool playerShown(const App& app);
float travelYaw(float dx, float dz); // dir 2000's facing (radians) for a move by (dx, dz)
bool npcActive(const App& app, const cr::FieldNpc& n);
void resetFieldNpcs(App& app);
bool npcVisible(const App& app, size_t index);
void playBgm(App& app, uint32_t v);
void endFieldTasks(App& app);
void enterMap(App& app);
bool performWarp(App& app, const cr::WarpRequest& w, int fromEvent);
const cr::FieldNpc* interactTarget(const App& app);
bool interact(App& app, bool instant = false);
bool languageUsable(const App& app, int language);
void applyLanguage(App& app);
std::string itemName(const App& app, int item);
void resetTyping(App& app);
void stepMsgWin(App& app);
void stepMsgWin(App::MsgWin& w);
void advanceDialogue(App& app, bool tapped = true);
int pageChars(const std::vector<std::string>& page);
std::vector<std::string> typedPage(const std::vector<std::string>& page, int limit);
const cr::MessageInfo* pageInfo(const App& app);
bool pageTyped(const App& app);
int pauseHold(int frames);
void stepTyping(App& app);
void tapDialogue(App& app);
void answerChoice(App& app, bool yes);
void answerWorldChoice(App& app, int pos);
void showMessages(App& app, const std::vector<int>& ids);
bool checkTriggers(App& app, bool instant = false);
void poseWorld(const cr::Skeleton& skel, const std::vector<cr::Action>& actions, const cr::Action* act, float time, const cr::MotionBlend& blend, std::vector<cr::Mat4>& world);
void animate(App& app, LoadedModel& lm, float dt);
void animateModel(App& app, float dt);
void placePlayer(App& app);
void stepPuzzleHud(App& app);
void drawPuzzleBanner(App& app);
int fieldRoute(const App& app);
bool fieldMenuWanted(const App& app);
bool fieldSceneRunning(const App& app);
bool fieldEncountersOff(const App& app);
void stepFieldMenu(App& app);
void fieldButtonPos(const App& app, int k, float& x, float& y);
bool fieldButtonShown(const App& app, int k);
void drawFieldMenu(App& app);
bool fieldBarPad(const App& app);
std::vector<int> fieldBarEntries(const App& app);
void toggleFieldMenu(App& app);
bool fieldBarButton(App& app, int b, bool confirm, bool cancel);
void pressFieldButton(App& app, int k);
bool fieldMenuClick(App& app, float px, float py);
void noteWindowAtlas(App& app, const cr::Image& wi);
void easyButtonCentre(const App& app, float& x, float& y);
bool easyButtonShown(const App& app);
bool touchPadDown(App& app, float px, float py);
void touchPadMove(App& app, float px, float py);
void touchPadUp(App& app);
bool touchPadStick(const App& app, float& sx, float& sy, bool& run);
void stepTouchPad(App& app);
void drawTouchPad(App& app);
void openMapScreen(App& app);
void closeMapScreen(App& app);
void stepMapScreen(App& app);
void drawMapScreen(App& app);
int npcEffect(App& app, int id, int npc, bool loop = false);
bool fieldToLogical(const App& app, cr::Vec3 p, float& lx, float& ly);
void logicalToTex2d(const App& app, float lx, float ly, float& tx, float& ty);
namespace puzzle {
const cr::PuzzleStage* stage(const App& app);
bool cleared(const App& app);
int* cellSlot(App& app, int layer, int r, int c);
int objectAt(App& app, int layer, int r, int c);
cr::Vec3 centre(const App& app, int r, int c);
bool cellOf(const App& app, cr::Vec3 p, int& r, int& c);
App::NpcDraw* drawOf(App& app, int npc);
void fade(App& app, int npc, float from, float to, int frames);
void show(App& app, App::Puzzle::Obj& o, bool on);
void place(App& app, App::Puzzle::Obj& o, cr::Vec3 p);
bool allSwitchesOn(App& app);
bool gemsLeft(App& app);
void enter(App& app);
void teleportTo(App& app, int t);
void punish(App& app, int t);
void pkGrab(App& app, int t, float sx = -1e9f, float sy = -1e9f);
void pkSend(App& app, int dir);
void toughTap(App& app, int t);
bool allCandlesLit(App& app);
void pyroTap(App& app, int t);
void reset(App& app);
bool target(const App& app, const App::Puzzle::Obj& o);
bool inWindow(int type, float dx, float dy);
int pick(App& app, float tx, float ty);
void step(App& app);
bool ask(App& app, int k);
int ahead(App& app);
bool click(App& app, float px, float py);
void drag(App& app, float px, float py);
void release(App& app, float px, float py);
void abilityKey(App& app);
void drawFx(App& app, const cr::Mat4& mvp);
} // namespace puzzle
void stepEvents(App& app);
void fieldWalked(App& app, float dist);
void playFootsteps(App& app);
bool same(const cr::Vec3& a, const cr::Vec3& b);
bool same(const cr::Mat4& a, const cr::Mat4& b);
cr::Vec3 lerp(const cr::Vec3& a, const cr::Vec3& b, float k);
float lerpAngle(float a, float b, float k);
cr::FieldWalker shownWalker(const App& app);
float shownPlayerAlpha(const App& app);
void shownCamera(const App& app, cr::Vec3& eye, cr::Vec3& target);
void shownScroll(const App& app, float& x, float& y);
void updatePlayer(App& app, float dt, float camYaw);
cr::Mat4 playerWorld(const cr::FieldWalker& w);
cr::Mat4 npcWorld(const cr::FieldNpc& n);
cr::FieldNpc* findNpc(App& app, int id);
bool actorPos(App& app, int id, cr::Vec3& out);
void setActor(App& app, int id, cr::Vec3 p, const float* yaw, bool keepY);
void buildNpcDraws(App& app);
std::vector<cr::WalkBody> walkBodies(const App& app);
void animateNpcs(App& app, float dt);
void drawNpcs(App& app, const cr::Mat4& vp);
void drawLoaded(App& app, const LoadedModel& m, const cr::Mat4& mvp, const cr::Material* override = nullptr, float alpha = 1.0f, float matTime = -1.0f, bool faded = false);
void drawFieldShadows(App& app, const cr::Mat4& vp);

// eff3d (the battle effects section)
// battle.cpp
int partyLevel(const App& app);
std::string sysText(const App& app, int id, const std::string& name = std::string());
int realSlot(const LoadedModel& lm, int slot);
int battleAction(const LoadedModel& lm, int slot);
App::Battle::Actor* battleActor(App& app, int fighter);
float actorDuration(App& app, const App::Battle::Actor& a);
void setBattleCamera(App& app, int id, int actor = 0, bool held = false);
void castCamera(App& app, int kind, int user);
void stepBattleCamera(App& app);
int monsterSize(const App& app, int fighter);
int enemyCameraSlot(const App& app, int fighter);
void cameraForTarget(App& app, int target, bool group, bool cursor = false);
void cameraForTurn(App& app, const cr::BattleTurn& t);
void hitCamera(App& app, const cr::BattleTurn& t, int target, bool bounce);
bool attackSpot(const App& app, int user, int k, int target, cr::Vec3& out);
int attackSlot(const App& app, const cr::BattleTurn& t, int k, int model, int cls);
void battleVictory(App& app);
LoadedModel* effectModel(App& app, int id);
cr::Vec3 actorPoint(App& app, const App::Battle::Actor& a, int user);
void spawnEffect(App& app, int id, int fighter);
int startEffect(App& app, std::vector<Effect>& list, int id, cr::Vec3 at, bool loop, float scale, float yaw);
void stopEffect(std::vector<Effect>& list, int handle);
void stepEffects(App& app, std::vector<Effect>& list);
void stepEffects(App& app);
const cr::Action* effectMaterialAction(const LoadedModel& lm, const cr::Material* mt);
bool effectUvMotion(const LoadedModel& lm, const cr::Material* mt, float time, float shift[2], float scale[2], int unit = 0);
void drawEffects(App& app, const std::vector<Effect>& list, const cr::Mat4& vp, const cr::Vec3* eye, const cr::Vec3* target);
void drawEffects(App& app, const cr::Mat4& vp, const cr::Vec3& eye, const cr::Vec3& target);
namespace battletalk {
int route(const App& app);
bool on(const App& app);
void rules(App& app);
void first(App& app);
bool end(App& app);
bool tutorial(App& app, int key);
void step(App& app);
} // namespace battletalk
int battleInSound(int g);
void startBattle(App& app, const cr::BattleStart& start, int flags);
void endBattle(App& app, bool won);
void setBanner(App& app, const std::string& text, int frames = btlui::kBannerFrames);
void battleChooseTop(App& app, int i);
void battleChooseGene(App& app, int ability);
void battleChooseCommand(App& app, int i);
void battleChooseItem(App& app, int item);
void battleChooseTarget(App& app, int fighter);
void battleEscapeAnswer(App& app, bool yes);
void faceFighter(App& app, App::Battle::Actor& a, int target);
void turnActors(App& app);
void turnOver(App::Battle::Actor& a, float want, float frames);
void startQuake(App::Battle::Quake& q, float amp, float from, float to, int frames, bool hold);
void startQuake(App& app, float amp, float from, float to, int frames, bool hold);
void endQuake(App::Battle::Quake& q, float amp, float from, float to, int frames);
void endQuake(App& app, float amp, float from, float to, int frames);
void stepQuake(App::Battle::Quake& q);
void stepQuake(App& app);
namespace btlvoice {
bool on(const App& app);
int speaker(const App& app, int k);
void play(App& app, int k, int a, int b2, int c);
void attack(App& app, const cr::BattleTurn& t);
void command(App& app, int member);
void turn(App& app, int k, int action);
void victory(App& app);
} // namespace btlvoice
void startTurn(App& app);
void showHits(App& app);
void playSlot(App& app, App::Battle::Actor& a, int slot, int then = -1);
int hitLabel(int type);
void placePopup(App::Battle& b, int slot, float& x, float& y, float wide);
void rollPartyNumbers(App::Battle& b);
void stepMenuView(App& app);
void startBreakFx(App& app);
bool stepBreakFx(App& app);
void endRound(App& app);
int battleSlideDistance(const App& app);
bool battleMenuReady(const App& app);
void stepBattle(App& app);
void drawBattle(App& app, float dt);
bool battleCursorLit(const App& app);
void resultsTap(App& app);
void stepResults(App& app);
void drawBattleHud(App& app, float W, float H);
void battleKey(App& app, SDL_Keycode key);
void battleClick(App& app, float px, float py);

// explore.cpp
int runSelfTest(App& app);
void startNewGame(App& app);
void bootTitle(App& app);
int runExplore(App& app, int maxActions);
int runExportModels(App& app, const char* dirArg);
int runExportTextures(App& app, const char* dirArg);
int runCapture(App& app, const char* mapName, const char* outPath);

// main.cpp
bool padField(const App& app);
void padButton(App& app, int b);
bool padLive(App& app);
bool stickPointerWanted(const App& app);
void stickPointerRelease(App& app);
void stickPointerStep(App& app, float dt);
void drawStickPointer(App& app);
void padStep(App& app, float dt);

} // namespace crapp
