#pragma once
// Event script interpreter (PrcEvnt / EvtPrcTbl in the original). An EventRunner runs a script
// a frame at a time: it waits where the original waits (Wait, fades, messages, the item window,
// taps) and asks an EventHost to show things. Without a host everything is instant (tests,
// dry runs). See docs/FORMATS.md ("Event scripts").

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "assets/event.hpp"
#include "game/party.hpp"

namespace cr {

// Save-game state the scripts read and write.
struct GameState {
    std::unordered_map<int, int32_t> vars; // var(n); EvtStart sets var 62 = area, 63 = map
    std::vector<uint32_t> flags = std::vector<uint32_t>(128, 0); // flag(0..4095)

    std::unordered_map<int, int> items;      // item number -> count (Uty_AddItem / Uty_DelItem)
    std::vector<int> openedBoxes;            // per area*1000 + box id (Uty_SetOpenedTresureBox)
    int oz = 0;                              // money (AppCtl +0x2f93c, MuCom_AddOz: 0..9,999,999)
    Party party = newParty();                // levels, experience, equipment (game/party.hpp)
    // Per pair (route 0..3, the save's +0xc18): encounters switched off (skill 170's field button,
    // +0xc1b + route) and the EXP banked by walking (skill 174, +0xac0 + 4 route, paid at a win)
    std::array<bool, 4> noEncounters{};
    std::array<int, 4> walkExp{};
    // Per route: the World Select's level band (+0xc10 + route) and TreasureBoxManager's draw for
    // the dungeon in play (fifteen {box id, item}; item -1 once taken), drawn again each time the
    // World Select leads into one. `route` is the pair in play (set by the field; not saved)
    std::array<int, 4> bands{};
    std::array<std::vector<std::array<int, 2>>, 4> treasure;
    int route = 0;

    // Cutscene frame data selected by FrmSet / FrmJmp (+0x1dfc / +0x22cc in the original):
    // global, so a scene set up by one script is used by the next. Not part of the save.
    uint32_t frmScene = 0;                   // file offset of the scene, 0 = none
    uint32_t frmRecord = 0;                  // file offset of the current record

    bool flag(int n) const { return n >= 0 && n < 4096 && (flags[n >> 5] >> (n & 31) & 1); }
    void setFlag(int n, bool on) {
        if (n < 0 || n >= 4096) return;
        if (on) flags[n >> 5] |= 1u << (n & 31);
        else flags[n >> 5] &= ~(1u << (n & 31));
    }
};

struct WarpRequest {
    bool valid = false;
    int area = -1;            // -1 = stay in the current area
    int map = -1;
    float pos[3] = {0, 0, 0};
    float angleDegrees = 0;
};

struct EventResult {
    bool ran = false;         // the script exists and ran to Return / Abort (or a warp, instant mode)
    int instructions = 0;
    WarpRequest warp;
    std::vector<int> itemsGained;
    std::vector<int> messages;  // Msg ids in the order shown
    std::string error;          // decoding problem, if any
};

// What a running script asks of the game. NPC ids are already resolved (65535 = the script's
// own NPC). Every call has a do-nothing default.
class EventHost {
public:
    virtual ~EventHost() = default;
    virtual void showMessage(int /*id*/) {}
    // Open (__EvtOpen): the message window's style byte (2 the parchment, 8 a tutorial's band,
    // 0x10 none, 0x20 the black band, 0x40 the grey panel) and its lines (1 picks the rect's
    // one-line place)
    virtual void openWindow(int /*style*/, int /*lines*/) {}
    virtual bool messageOpen() { return false; }         // the script waits while true
    virtual bool tapped() { return false; }              // action key pressed this frame (touch_up_chk)
    virtual void showItemWindow(int /*item*/) {}         // Uty_GetItem
    // Anim npc, slot, a, b (mdl_set_act_motion_loop): play the action in `slot`, then loop frames
    // a..b; a = -1 plays it once and holds the end, a = b = 0 loops it whole
    virtual void playAnim(int /*npc*/, int /*slot*/, int /*loopFrom*/ = 0, int /*loopTo*/ = 0) {}
    // LoadAct npc, action, slot (read_act_set): kActionFile[action] into the actor's slot; UnloadAct frees it
    virtual void loadAction(int /*npc*/, int /*action*/, int /*slot*/) {}
    virtual void unloadAction(int /*npc*/, int /*slot*/) {}
    virtual bool animBusy(int /*npc*/) { return false; } // Wait 0xFFFD npc: still moving (npc_autoctl_chk)
    virtual void fadeNpc(int /*npc*/, float /*from*/, float /*to*/, int /*frames*/) {} // NpcFade, mdl_fadein/out
    // Born (26) shows an NPC, Kill (27) hides it, until the room reloads (EvtBorn: npc ctl +0x8c
    // bit 0); npc 0 is the player
    virtual void showNpc(int /*npc*/, bool /*shown*/) {}
    virtual void screenFade(float /*grey*/, float /*fromAlpha*/, float /*toAlpha*/, int /*frames*/) {}
    virtual bool screenFading() { return false; }        // Wait 0xFFFF / 0xFFFC
    virtual void warp(const WarpRequest& /*w*/) {}       // the script continues on the new field
    virtual void playSound(uint32_t /*id*/) {}           // Eff: _effgo(u16 | u16 << 16)
    virtual void playMusic(uint32_t /*v*/) {}            // Bgm: _bgmgo(u16 | u16 << 16)
    // Cutscene staging (EvtFrmSetc / FrmSetc2 / FrmAdjc / FrmAdjc2 / SetDir); npc 0 is the party
    // leader. Positions are world units; the y is snapped to the floor unless `mode` is 2.
    // Facing: Yaw turns to `yawDegrees`, Keep leaves it, Travel faces the way it moves.
    enum class Facing { Yaw, Keep, Travel };
    virtual void placeNpc(int /*npc*/, const float* /*pos*/, float /*yawDegrees*/, Facing /*facing*/) {}
    // Moves in a straight line over `frames` (npc_autoctl_set_fream); mode 0 position, 1 position
    // and turn, 2 position including y. Wait 0xFFFD waits for it (animBusy).
    virtual void moveNpc(int /*npc*/, const float* /*pos*/, float /*yawDegrees*/, Facing /*facing*/, int /*frames*/,
                         int /*mode*/) {}
    virtual void faceNpc(int /*npc*/, int /*target*/) {} // SetDir: npc turns towards target
    // Util 3 (the event menu, _menuu_Event*): a yes/no choice; the host sets flag `flag` for yes
    // and clears it for no. The script waits while choiceOpen().
    // NpcMdlRead: bring NPC `npc` (NPC table row) into the room even if its list doesn't have it;
    // it waits, unplaced, until a script places it (Setc / FrmSetc*)
    virtual void loadNpc(int /*npc*/) {}
    // Call 2022 (Uty_LoadPrty): the boot script's chosen pair 1..4 (var 1) becomes the party
    // Uty_LoadPrty: prty_init_new(var 0 - 1, var 1 == 3 ? 3 : 2) - the pair (1..4) and whether the
    // new game carries over the clear data
    virtual void loadParty(int /*pair*/, bool /*clearData*/) {}
    // Cutscene camera (EvtFrmSetCam / FrmAdjCam / FrmPanCam, CameraFree / CameraPlayer): in rooms
    // without a painted background the script moves the 3D camera; `frames` 0 = a cut
    virtual void freeCamera(bool /*on*/) {}
    virtual void setCamera(const float* /*eye*/, const float* /*target*/, int /*frames*/) {}
    virtual void panCamera(const float* /*delta*/, int /*frames*/) {}
    // Mapscroll (EvtMapscroll, op 31): on a painted field with the camera free, move the view
    // `rate` of the way (1 = at once) to centre on npc `npc` (0 = the player) or, with npc -1,
    // on view pixel (x, y) (FieldCamera::setScroll3D / setScroll2D)
    virtual void mapScroll(int /*npc*/, float /*x*/, float /*y*/, float /*rate*/) {}
    // Scene effects (ScnEffect*): call 2020 loads effect set n (0..5: the textures of a dungeon's
    // weather), call 2009 starts (on) or stops effect `id` (1..20)
    virtual void sceneEffectSet(int /*set*/) {}
    // Uty_WarpCrystalCamera (call 2014): the camera to a puzzle crystal NPC and back (var 0: 0 going,
    // 1 it showed, the script sets 2, 3 back at the leader)
    virtual void quake(int /*amp*/, int /*from*/, int /*to*/, int /*frames*/) {} // EvtQuake
    virtual void puzzleStart() {}
    // Uty_TexDisp: GetXmpStkData 0..10 = Tex2d slot, x, y, u, v, w, h, fade-in, hold, fade-out, ?
    virtual void texDisp(const int32_t* /*g*/) {}
    // Uty_TutoTexDisp: GetXmpStkData 0..11 = slot, pages, then (row, frames) for pages 0..3, x, y;
    // the picture is var 1's of MsgUty_TexDispInit's table
    virtual void tutoTexDisp(const int32_t* /*g*/) {}
    virtual void exclamationMark() {}
    virtual void battleParamUp(int /*fighter*/) {} // Uty_Btl_ParamUp01 (fighter 1) / 02 (fighter 0)
    virtual bool logoSeen() { return false; }     // Uty_ChkLogo_flg: the logos already played this run
    // The Vita's boot opcodes 0x47..0x4a: a logo picture into a slot; shown faded in, held and
    // faded out (frames) - the script waits while it is busy
    virtual void vitaLogo(int /*slot*/, int /*picture*/) {}
    virtual void vitaLogoShow(int /*slot*/, int /*in*/, int /*hold*/, int /*out*/) {}
    virtual bool vitaLogoBusy() { return false; }
    virtual void padArt() {}                      // Uty_DrawController: the pad's picture until flag 0 clears            // Uty_DrawExclamationMark: until flag 0 clears
    virtual void recoverPair() {}               // Uty_HpMpRecovery: the pair's HP / MP (pcparam_hpmp_recovery)
    virtual int bgmNo() { return 0; }           // Uty_GetBgmNo: the music last asked for
    virtual void endMenu(int /*kind*/) {}       // Uty_EndMenu: 2 ends the shop, its music not put back
    virtual int pairNo() { return 0; }          // Uty_GetPartyNo: the pair, 0..3
    virtual void nowLoading(bool /*on*/) {}     // Uty_NowLoading: flag 4086 shows the spinner // Uty_PuzzleStart: the "Puzzle Start" banner (var 0 / flags 0, 1 drive it)
    virtual void warpCrystalCamera(int /*npc*/) {}
    // FrmPlayEff / FrmPlayEff2: effect `effect` (looped) on actor `actor` (0 the leader, else an NPC
    // by id); an actor that isn't an NPC takes the current record's position `v` (`angled`: and
    // its angle, v[3..5]); `scale` percent. FrmStopEff (actor -1) / FrmStopEff2 stop them.
    virtual void frameEffect(int /*actor*/, int /*effect*/, int /*scale*/, const float* /*v*/, bool /*angled*/) {}
    virtual void stopFrameEffect(int /*effect*/, int /*actor*/) {}
    // Uty_eff3d_set_fld: effect `effect` once at NPC `npc`, `dy` above it
    virtual void npcEffect(int /*effect*/, int /*npc*/, int /*dy*/) {}
    virtual void sceneEffect(bool /*on*/, int /*id*/) {}
    // Not implemented yet - reported so tests can see where the story needs them:
    // Battle (_EvtBattle): u16 enemy group, u8 battle stage, u8 flags (G+0x354c6). The battle
    // starts once the frame ends; the field and its script are torn down, and EvrStart 2 runs
    // when the field comes back
    virtual void battle(int /*group*/, int /*stage*/, int /*flags*/) {}
    // Util n other than 3 (0 title, 1 world choice, 2 shop, 4 start, 5/6 save, 7 credits, 8 last
    // battle): true if the host opened it - the script then waits while choiceOpen()
    virtual bool menu(int /*n*/) { return false; }
    virtual void askYesNo(int /*flag*/) {}
    virtual bool choiceOpen() { return false; }
    // Tex2d (EvtTex2d): 2D image slot 0..15 - portraits, title cards. Flags: 0x1 load `image`
    // (kTex2dImage), 0x2 free, 0x1000 / 0x2000 / 0x4000 place left (-32, 0) / right (512, 0) /
    // centre (176, 96), 0x400 show / 0x800 hide (3 frames: fade + 32 px slide), 0x200 swap in at
    // once (others on that side vanish), 0x40 with centre: zoom 200% -> 100% over 4 frames,
    // 0x8000 shown as loaded. `extra` holds the optional 3-short blocks of flags 0x4, 0x20, 0x8
    // (no shipped script uses them).
    virtual void tex2d(int /*slot*/, uint16_t /*flags*/, int /*image*/, const int16_t (*/*extra*/)[3]) {}
};

class EventRunner {
public:
    EventRunner();
    ~EventRunner();
    // Prepares script `eventNo` of `file` (the current area's event file). False if it has none.
    bool start(const EventFile& file, int eventNo, GameState& state, int currentArea, int currentMap);
    // Starts the code at file offset `entry` (an EvrStart table entry); `tag` is what eventNo()
    // reports (no NPC is "self").
    bool startAt(const EventFile& file, uint32_t entry, int tag, GameState& state, int currentArea, int currentMap);
    // Runs one frame: until the script waits or ends. Returns true while it is still running.
    bool step(EventHost* host);
    bool running() const;
    void stop();               // abandon the running script
    void swap(EventRunner& other); // trade scripts (one waits, parked, while the other runs)
    // In a battle (PrcEvnt with G+0x2c0 bit 5): step() runs nothing while flag `flag` is clear,
    // and stops before the instruction after the one that clears it; -1 lifts the gate
    void setGate(int flag);
    int gate() const { return gate_; }
    int eventNo() const;
    std::string where() const; // "pc 0x1234, waiting for fade" - for logs and tests
    const EventResult& result() const { return result_; }

private:
    struct Vm;
    std::unique_ptr<Vm> vm_;
    EventResult result_;
    int gate_ = -1;
};

// Debugging: called with the address and opcode of every instruction any runner executes.
void setEventTrace(std::function<void(size_t pc, int op)> trace);
// Each native call (op 15) with its first 12 GetXmpStkData arguments (tools)
void setNativeTrace(std::function<void(int id, const int32_t* args)> trace);
// Each flag a condition reads, with its value (tools)
void setFlagTrace(std::function<void(int flag, bool value)> trace);
void setVarTrace(std::function<void(int var, int32_t value)> trace);

// Runs script `eventNo` of `file` instantly to its end (or its first warp).
EventResult runEvent(const EventFile& file, int eventNo, GameState& state, int currentArea, int currentMap);

} // namespace cr
