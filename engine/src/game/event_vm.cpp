#include "game/event_vm.hpp"

#include <cstdio>
#include <cstring>

namespace cr {

namespace {
constexpr int kMaxInstructions = 200000;     // per step: a script that never waits is broken
constexpr int kInstantLoopCap = 1000;        // instant mode: waiting loops would spin forever
constexpr int kFrameLoopCap = 900;           // stepped mode: give up a wait loop after 30 s
std::function<void(size_t, int)> gTrace;
} // namespace

void setEventTrace(std::function<void(size_t pc, int op)> trace) { gTrace = std::move(trace); }
namespace { std::function<void(int, const int32_t*)> gNativeTrace; }
void setNativeTrace(std::function<void(int id, const int32_t* args)> trace) { gNativeTrace = std::move(trace); }
namespace { std::function<void(int, bool)> gFlagTrace; }
void setFlagTrace(std::function<void(int flag, bool value)> trace) { gFlagTrace = std::move(trace); }
namespace { std::function<void(int, int32_t)> gVarTrace; }
void setVarTrace(std::function<void(int var, int32_t value)> trace) { gVarTrace = std::move(trace); }

struct EventRunner::Vm {
    Vm(const EventFile& f, GameState& state, int event, int area, int map)
        : file(f), d(f.bytes()), gs(state), eventNo(event), area(area), map(map) {}
    const EventFile& file;
    const std::vector<uint8_t>& d;
    GameState& gs;
    int eventNo, area, map;
    enum class Wait { None, Frames, Fade, Message, Npc, Choice, Logo } wait = Wait::None;
    int waitFrames = 0, waitNpc = -1;
    FieldNpc self;             // NPC whose script runs: flag(-1) / flag(-2) name its flags
    size_t pc = 0;
    std::vector<size_t> calls; // Gosub return addresses (64 deep in the original)
    int32_t args[34] = {};     // argument stack (ctx +0x200)
    int sp = 0;                // ctx +0x490
    int bp[34] = {};           // frame bases (ctx +0x288), bpIndex at ctx +0x492
    int bpIndex = 0;
    bool pendingFrame = false; // PushBP marks that the next Gosub opens a frame (ctx +0x496)
    int32_t result = 0;
    std::string error;
    std::unordered_map<size_t, int> loopCount; // waiting loops spin (Wait is a no-op): cap them
    struct Loop { size_t start, end; };
    std::vector<Loop> loops;   // Rept stack (ctx +0x100, depth at +0x4a4, 32 deep)

    bool need(size_t n) {
        if (pc + n <= d.size()) return true;
        if (error.empty()) error = "read past end";
        return false;
    }
    uint8_t u8() { if (!need(1)) return 0; return d[pc++]; }
    uint16_t u16() { if (!need(2)) return 0; uint16_t v; std::memcpy(&v, &d[pc], 2); pc += 2; return v; }
    uint32_t u32() { if (!need(4)) return 0; uint32_t v; std::memcpy(&v, &d[pc], 4); pc += 4; return v; }

    int32_t& arg(int n) {
        static int32_t dummy;
        const int i = (bpIndex >= 0 && bpIndex < 34 ? bp[bpIndex] : 0) + n;
        return i >= 0 && i < 34 ? args[i] : dummy;
    }
    int32_t var(int n) const {
        if (n < -198) n += 317; // __VAR
        auto it = gs.vars.find(n);
        return it == gs.vars.end() ? 0 : it->second;
    }
    int flagIndex(int n) const { return n == -1 ? self.flagShow : n == -2 ? self.flagHide : n; }

    // GetEval: prefix expression tree (operator table at 0xd3a20)
    int32_t eval(int depth = 0) {
        if (depth > 64) { error = "expression too deep"; return 0; }
        const uint8_t op = u8();
        auto sub = [&] { return eval(depth + 1); };
        switch (op) {
        case 0: { int32_t a = sub(), b = sub(); return a == b; }
        case 1: { int32_t a = sub(), b = sub(); return a != b; }
        // Binary operators compute (second OP first): __GT is `cmp first, second; movlt`
        case 2: { int32_t a = sub(), b = sub(); return b > a; }
        case 3: { int32_t a = sub(), b = sub(); return b >= a; }
        case 4: { int32_t a = sub(), b = sub(); return b < a; }
        case 5: { int32_t a = sub(), b = sub(); return b <= a; }
        case 6: { int32_t a = sub(), b = sub(); return a && b; }
        case 7: { int32_t a = sub(), b = sub(); return a || b; }
        case 8: { int32_t a = sub(), b = sub(); return a ^ b; }
        case 9: return !sub();
        case 10: { int32_t a = sub(), b = sub(); return a + b; }
        case 11: { int32_t a = sub(), b = sub(); return b - a; }
        case 12: { int32_t a = sub(), b = sub(); return a * b; }
        case 13: { int32_t a = sub(), b = sub(); return a ? b / a : 0; }
        case 14: { int32_t a = sub(), b = sub(); return a ? b % a : 0; }
        case 16: return int32_t(u32());
        case 17: return result;
        case 18: { int32_t n = sub(); return n > 0 ? int32_t(eventNo * 7 % n) : 0; } // rand: deterministic here
        case 19: {
            const int n = sub();
            if (gVarTrace) gVarTrace(n, var(n));
            return var(n);
        }
        case 20: {
            const int n = flagIndex(sub());
            if (gFlagTrace) gFlagTrace(n, gs.flag(n));
            return gs.flag(n);
        }
        case 21: return int32_t(u32()); // label: a script address
        case 22: return arg(u8());
        case 33: case 34: case 42: return 0; // __GOLD / __NAME / __GDS: stubs returning 0 in the binary too
        case 38: { const int n = sub(); auto it = gs.items.find(n); return it != gs.items.end() && it->second > 0; } // __ITEM: owned?
        default:
            if (error.empty()) error = "bad expression op " + std::to_string(op);
            return 0;
        }
    }

    // SetEval: value expression, then the target (setter table at 0xd3ad0)
    void assign() {
        const int32_t v = eval();
        const uint8_t op = u8();
        switch (op) {
        case 17: result = v; break;
        case 19: { int n = eval(); if (n < -198) n += 317; gs.vars[n] = v; break; }
        case 20: gs.setFlag(flagIndex(eval()), v != 0); break;
        case 22: arg(u8()) = v; break;
        case 33: case 34: case 42: break;
        case 38: { // ___ITEM: `item[-1] = n` gives item n, `item[-2] = n` takes one away
            const int n = eval();
            if (n == -1) { ++gs.items[v]; }
            else if (n == -2) { auto it = gs.items.find(v); if (it != gs.items.end() && it->second > 0) --it->second; }
            break;
        }
        default:
            if (error.empty()) error = "bad assignment target " + std::to_string(op);
        }
    }

    void skip(size_t n) { if (need(n)) pc += n; }
    int resolve(int npc) const { return npc == 0xFFFF ? eventNo : npc; } // 65535 = the running NPC

    // Cutscene scene (section 6) helpers
    struct Scene { int actors = 0, words = 3; uint32_t count = 0; size_t ids = 0, first = 0, stride = 0; };
    bool scene(Scene& s) const {
        const size_t o = gs.frmScene;
        if (!o || o + 8 > d.size()) return false;
        uint32_t n, count;
        std::memcpy(&n, &d[o], 4);
        std::memcpy(&count, &d[o + 4], 4);
        if (n > 9999) { n -= 10000; s.words = 6; }
        s.actors = int(n);
        s.count = count;
        s.ids = o + 8;
        s.first = o + 8 + 4 * size_t(n);
        s.stride = 4 + 4 * size_t(n) * size_t(s.words);
        return s.first + size_t(count) * s.stride <= d.size() && n < 256;
    }
    // FrmJmp's search: the record for `frame`, else the nearer of its neighbours, else the last
    uint32_t record(const Scene& s, int frame) const {
        if (s.count == 0) return 0;
        for (uint32_t i = 0; i < s.count; ++i) {
            const size_t r = s.first + i * s.stride;
            uint32_t f;
            std::memcpy(&f, &d[r], 4);
            if (int64_t(f) < frame) continue;
            if (int64_t(f) == frame || i == 0) return uint32_t(r);
            uint32_t pf;
            std::memcpy(&pf, &d[r - s.stride], 4);
            return frame - int64_t(pf) < int64_t(f) - frame ? uint32_t(r - s.stride) : uint32_t(r);
        }
        return uint32_t(s.first + (s.count - 1) * s.stride);
    }
    // Index of actor `npc` in the scene, -1 if absent
    int actorIndex(const Scene& s, int npc) const {
        for (int i = 0; i < s.actors; ++i) {
            int32_t id;
            std::memcpy(&id, &d[s.ids + 4 * size_t(i)], 4);
            if (id == npc) return i;
        }
        return -1;
    }
    // A camera shot (EvtFrmSetCam): the eye is actor `id`'s position, the target the next actor's
    bool cameraShot(const Scene& s, uint32_t rec, int id, float eye[3], float target[3]) const {
        // A 3D room's shot is an eye actor and the target actor after it; a painted field's only
        // needs the first (its x, y: the scroll), so a last actor gives target = eye
        const int i = actorIndex(s, id < 0 ? -id : id);
        if (!rec || i < 0 || i >= s.actors) return false;
        const int j = i + 1 < s.actors ? i + 1 : i;
        for (int k = 0; k < 3; ++k) {
            int32_t a, b;
            std::memcpy(&a, &d[rec + 4 + 4 * (size_t(i) * s.words + k)], 4);
            std::memcpy(&b, &d[rec + 4 + 4 * (size_t(j) * s.words + k)], 4);
            eye[k] = float(a) / 100.0f;
            target[k] = float(b) / 100.0f;
        }
        return true;
    }
    // The actor's values in record `rec` (÷100: position, then rotation in degrees)
    bool actor(const Scene& s, uint32_t rec, int npc, float out[6]) const {
        if (!rec) return false;
        for (int i = 0; i < s.actors; ++i) {
            int32_t id;
            std::memcpy(&id, &d[s.ids + 4 * size_t(i)], 4);
            if (id != npc) continue;
            for (int k = 0; k < 6; ++k) {
                int32_t v = 0;
                if (k < s.words) std::memcpy(&v, &d[rec + 4 + 4 * (size_t(i) * s.words + k)], 4);
                out[k] = float(v) / 100.0f;
            }
            return true;
        }
        return false;
    }
};

// EvtFrmSetc / FrmAdjc facing operand: degrees in -4499..999, 1000 keep, 2000 face the way it
// moves, anything else 0
static EventHost::Facing facingFor(int dir, float& yaw) {
    yaw = 0;
    if (dir >= -4499 && dir < 1000) { yaw = float(dir); return EventHost::Facing::Yaw; }
    if (dir == 1000) return EventHost::Facing::Keep;
    if (dir == 2000) return EventHost::Facing::Travel;
    return EventHost::Facing::Yaw;
}

EventRunner::EventRunner() = default;
EventRunner::~EventRunner() = default;

bool EventRunner::running() const { return vm_ != nullptr; }
void EventRunner::stop() { vm_.reset(); }
void EventRunner::setGate(int flag) { gate_ = flag; }
int EventRunner::eventNo() const { return vm_ ? vm_->eventNo : -1; }

std::string EventRunner::where() const {
    if (!vm_) return "idle";
    static const char* const kWait[] = {"running", "waiting frames", "waiting for fade", "waiting for message",
                                        "waiting for npc", "waiting for a choice", "showing a logo"};
    char buf[64];
    std::snprintf(buf, sizeof buf, "pc %#zx, %s", vm_->pc, kWait[int(vm_->wait)]);
    return buf;
}

void EventRunner::swap(EventRunner& other) {
    vm_.swap(other.vm_);
    std::swap(result_, other.result_);
}

bool EventRunner::start(const EventFile& file, int eventNo, GameState& state, int currentArea, int currentMap) {
    vm_.reset();
    result_ = EventResult();
    const uint32_t entry = file.scriptEntry(eventNo);
    if (!entry) return false;
    vm_ = std::make_unique<Vm>(file, state, eventNo, currentArea, currentMap);
    for (const FieldNpc& n : file.npcs(currentMap))
        if (n.id == eventNo) vm_->self = n;
    state.vars[62] = currentArea; // EvtStart: SetVar(62, area), SetVar(63, map)
    state.vars[63] = currentMap;
    vm_->pc = entry;
    return true;
}

bool EventRunner::startAt(const EventFile& file, uint32_t entry, int tag, GameState& state, int currentArea,
                          int currentMap) {
    vm_.reset();
    result_ = EventResult();
    if (!entry || entry >= file.bytes().size()) return false;
    vm_ = std::make_unique<Vm>(file, state, tag, currentArea, currentMap);
    state.vars[62] = currentArea;
    state.vars[63] = currentMap;
    vm_->pc = entry;
    return true;
}

bool EventRunner::step(EventHost* host) {
    if (!vm_) return false;
    Vm& vm = *vm_;
    EventResult& res = result_;
    GameState& state = vm.gs;
    const EventFile& file = vm.file;
    const int eventNo = vm.eventNo, currentArea = vm.area;
    const auto& d = vm.d;
    auto finish = [&](bool ran) { res.ran = ran; res.error = vm.error; vm_.reset(); return false; };

    // Resume a wait from an earlier frame
    if (host) {
        switch (vm.wait) {
        case Vm::Wait::Frames: if (--vm.waitFrames > 0) return true; break;
        case Vm::Wait::Fade: if (host->screenFading()) return true; break;
        case Vm::Wait::Message: if (host->messageOpen()) return true; break;
        case Vm::Wait::Npc: if (host->animBusy(vm.waitNpc)) return true; break;
        case Vm::Wait::Choice: if (host->choiceOpen()) return true; break;
        case Vm::Wait::Logo: if (host->vitaLogoBusy()) return true; break;
        case Vm::Wait::None: break;
        }
    }
    vm.wait = Vm::Wait::None;
    auto yield = [&](Vm::Wait w) { vm.wait = w; return true; };

    for (int budget = 0; vm.error.empty(); ++budget) {
        if (budget > kMaxInstructions) { vm.error = "instruction limit"; break; }
        // PrcEvnt in a battle: the script goes on only while the gate flag (0xfff) is set,
        // checked before every instruction
        if (host && gate_ >= 0 && !state.flag(gate_)) return yield(Vm::Wait::None);
        ++res.instructions;
        if (!vm.need(1)) break;
        // PrcEvnt: reaching the innermost loop's end jumps back to its Rept
        if (!vm.loops.empty() && vm.pc == vm.loops.back().end) vm.pc = vm.loops.back().start;
        if (gTrace) gTrace(vm.pc, d[vm.pc]);
        const uint8_t op = d[vm.pc++];
        switch (op) {
        case 0: // Return
            if (vm.calls.empty()) return finish(true);
            vm.pc = vm.calls.back();
            vm.calls.pop_back();
            break;
        case 1: { // Gosub
            const uint32_t to = vm.u32();
            if (vm.calls.size() < 64) vm.calls.push_back(vm.pc);
            if (vm.pendingFrame) { ++vm.bpIndex; vm.pendingFrame = false; }
            vm.pc = to;
            break;
        }
        case 2: return finish(true); // Abort
        case 9: vm.assign(); break;         // Set
        case 10: vm.pc = vm.u32(); break; // Goto
        case 11: vm.pc = vm.u32(); if (!vm.loops.empty()) vm.loops.pop_back(); break; // Break
        case 12: { // Ifn: jump when the condition is false
            const int32_t c = vm.eval();
            const uint32_t to = vm.u32();
            if (!c) vm.pc = to;
            break;
        }
        case 13: { // Rept: loop head; the body runs until `to`, then PrcEvnt jumps back here
            const size_t at = vm.pc - 1;
            const int32_t c = vm.eval();
            const uint32_t to = vm.u32();
            if (vm.loops.empty() || vm.loops.back().start != at) {
                if (vm.loops.size() >= 32) vm.loops.erase(vm.loops.begin());
                vm.loops.push_back({at, to});
            }
            // Loops that wait for something not modelled (yet) would spin forever: cap them
            if (!c || ++vm.loopCount[at] > (host ? kFrameLoopCap : kInstantLoopCap)) {
                vm.pc = to;
                vm.loops.pop_back();
            }
            break;
        }
        case 15: { // Call: native utilities (EvtCall table at 0xd3b80, args from the frame)
            const uint16_t id = vm.u16();
            const int32_t a0 = vm.arg(0);
            if (gNativeTrace) {
                int32_t a[12];
                for (int k = 0; k < 12; ++k) a[k] = vm.arg(k);
                gNativeTrace(id, a);
            }
            if (id == 2001) state.vars[0] = host ? host->tapped() : 1;                       // touch_up_chk
            else if (id == 2004 && host) host->fadeNpc(eventNo, 1, 0, 20);                  // mdl_fadeout
            else if (id == 2010 && host) host->fadeNpc(eventNo, 0, 1, 20);                  // mdl_fadein
            else if (id == 2005 && host) host->showItemWindow(vm.var(9));                   // GetItem window
            else if (id == 2011) { ++state.items[a0]; res.itemsGained.push_back(a0); }       // AddItem
            else if (id == 2012) { if (state.items[a0] > 0) --state.items[a0]; }            // DelItem
            else if (id == 2013) { // Uty_ChkItem: var (the first push) = how many of the item (the last) - a gold box's key
                const auto it = state.items.find(vm.arg(1));
                const int n = it == state.items.end() ? 0 : std::max(0, it->second);
                state.vars[a0] = n;
                vm.result = n > 0;
            }
            else if (id == 2032) state.openedBoxes.push_back(currentArea * 1000 + a0);      // SetOpenedTresureBox
            else if (id == 2022 && host) host->loadParty(vm.var(0), vm.var(1) == 3);      // LoadPrty: pair, clear data
            // ScnEffect_Init: GetXmpStkData 0 (the last pushed: on / off) and 1 (the effect)
            else if (id == 2009 && host) host->sceneEffect(vm.arg(1) != 0, vm.arg(0));
            else if (id == 2014) { state.vars[0] = 0; if (host) host->warpCrystalCamera(a0); } // Uty_WarpCrystalCamera_Init
            // GetXmpStkData(k) is arg(count - 1 - k) (k = 0 the last pushed): 2023 takes 3, TexDisp 11
            else if (id == 2023 && host) host->npcEffect(vm.arg(2), vm.arg(1), vm.arg(0)); // Uty_eff3d_set_fld: effect, npc, dy
            else if (id == 2033 && host) { // Uty_TutoTexDisp
                int32_t g[12];
                for (int k = 0; k < 12; ++k) g[k] = vm.arg(11 - k);
                host->tutoTexDisp(g);
            }
            else if (id == 2007 && host) { state.setFlag(0, true); host->exclamationMark(); } // Uty_DrawExclamationMark_Init
            else if ((id == 2030 || id == 2031) && host) host->battleParamUp(id == 2030 ? 1 : 0); // Uty_Btl_ParamUp01 / 02
            else if (id == 2035) {} // Uty_Battle_first_anm_Init: btl_anm_load preloads - the port loads on demand
            else if (id == 2029 && host) { if (vm.var(0) == 0) state.vars[0] = host->logoSeen() ? 1 : 0; } // Uty_ChkLogo_flg
            else if (id == 2008 && host) { state.setFlag(0, true); host->padArt(); }     // Uty_DrawController_Init
            else if (id == 2002 || id == 2021) {} // init_tex2d (texture setup) / LoadData (the port's boot reads the saves itself)
            else if (id == 2016 && host) { // Uty_TexDisp
                int32_t g[11];
                for (int k = 0; k < 11; ++k) g[k] = vm.arg(10 - k);
                host->texDisp(g);
            }
            else if (id == 2003 && host) host->puzzleStart();                               // Uty_PuzzleStart_Init
            else if (id == 2018 && host) host->recoverPair();                               // Uty_HpMpRecovery
            else if (id == 2024 && host) state.vars[a0] = host->bgmNo();                    // Uty_GetBgmNo: var a0
            else if (id == 2025 && host) host->endMenu(a0);                                 // Uty_EndMenu
            else if (id == 2026 && host) host->playMusic(uint32_t(vm.var(a0)));             // Uty_BgmPlayFromVarNo
            else if (id == 2027 && host) state.vars[a0] = host->pairNo();                   // Uty_GetPartyNo: var a0
            else if (id == 2028 && host) host->nowLoading(state.flag(4086));                // Uty_NowLoading
            else if (id == 2020 && host) host->sceneEffectSet(vm.var(0));                  // ScnEffectRead: set var 0
            break;
        }
        case 14: { // Wait: frames, or 0xFFFF/0xFFFC fade, 0xFFFE loading, 0xFFFD + npc
            const uint16_t w = vm.u16();
            if (w == 0xFFFD) {
                vm.waitNpc = vm.resolve(vm.u16());
                if (host) return yield(Vm::Wait::Npc);
            } else if (w == 0xFFFF || w == 0xFFFC) {
                if (host) return yield(Vm::Wait::Fade);
            } else if (w != 0xFFFE && w > 0 && host) {
                vm.waitFrames = w;
                return yield(Vm::Wait::Frames);
            }
            break;
        }
        case 18: { const int32_t v = vm.eval(); if (vm.sp < 34) vm.args[vm.sp++] = v; break; } // Push (SetSP)
        case 19: // PushBP
            if (vm.bpIndex + 1 < 34) vm.bp[vm.bpIndex + 1] = vm.sp;
            vm.pendingFrame = true;
            break;
        case 20: // PopBP
            vm.sp = vm.bp[vm.bpIndex];
            if (vm.bpIndex > 0) --vm.bpIndex;
            break;
        case 17: case 59: { // Warp / Warp2
            uint16_t w[6];
            for (auto& x : w) x = vm.u16();
            if (w[0] == 0xFC19) break; // return to title: not handled here
            WarpRequest& r = res.warp;
            if (w[0] == 0xFFFD) {
                WarpEntry e;
                if (!file.warpEntry(eventNo, e)) return finish(true); // the terminator: nowhere to go
                r.area = e.area;
                r.map = e.map;
                std::memcpy(r.pos, e.pos, sizeof r.pos);
                r.angleDegrees = float(e.angleDegrees);
            } else {
                r.area = w[0] == 0xFFFF ? -1 : int16_t(w[0]);
                r.map = int16_t(w[1]);
                r.pos[0] = float(w[2]); r.pos[1] = float(w[3]); r.pos[2] = float(w[4]); // unsigned in EvtWarp
                r.angleDegrees = float(int16_t(w[5]));
            }
            r.valid = r.map >= 0;
            if (!host) return finish(true);
            if (r.valid) host->warp(r);
            // The rest of the script (fade-in) runs on the new field - unless the area (and so
            // the event file this script lives in) changed
            if (r.valid && r.area >= 0 && r.area != currentArea) return finish(true);
            return yield(Vm::Wait::None);
        }
        // Presentation and object opcodes: skip their operands (sizes from the handlers)
        case 3: case 4: { // Open: u8 the window's style (__EvtOpen: its rect and look), u16 its lines
            const int style = vm.u8(), lines = vm.u16();
            if (host) host->openWindow(style, lines);
            break;
        }
        case 5: case 6: break;                             // Close
        case 7: case 8: { // Msg
            const int id = vm.u16();
            res.messages.push_back(id);
            if (host) {
                host->showMessage(id);
                return yield(Vm::Wait::Message);
            }
            break;
        }
        case 26: case 27: { const int n = vm.resolve(vm.u16()); if (host) host->showNpc(n, op == 26); break; } // Born / Kill
        case 33: { // FrmSet n: select cutscene scene n (from 1), its first record
            const int16_t n = int16_t(vm.u16());
            if (n >= 1) {
                state.frmScene = file.sceneOffset(n);
                Vm::Scene s;
                state.frmRecord = vm.scene(s) ? uint32_t(s.first) : 0;
            }
            break;
        }
        case 34: { // FrmJmp frame: the scene's record for that frame
            const int16_t f = int16_t(vm.u16());
            Vm::Scene s;
            if (vm.scene(s)) state.frmRecord = vm.record(s, f);
            break;
        }
        case 36: case 53: { // FrmSetc npc, dir / FrmSetc2 npc, flag: place from the current record
            const int n = vm.resolve(vm.u16());
            const int16_t dir = int16_t(vm.u16());
            Vm::Scene s;
            float v[6];
            if (!host || !vm.scene(s) || !vm.actor(s, state.frmRecord, n, v)) break;
            if (op == 53) host->placeNpc(n, v, v[4], s.words == 6 ? EventHost::Facing::Yaw : EventHost::Facing::Keep);
            else { float yaw; const auto f = facingFor(dir, yaw); host->placeNpc(n, v, yaw, f); }
            break;
        }
        case 35: case 52: { // FrmAdjc npc, frame, frames, dir / FrmAdjc2 npc, frame, frames, mode
            const int n = vm.resolve(vm.u16());
            const int16_t frame = int16_t(vm.u16());
            const int frames = vm.u16();
            const int16_t last = int16_t(vm.u16());
            Vm::Scene s;
            float v[6];
            if (!host || !vm.scene(s) || !vm.actor(s, vm.record(s, frame), n, v)) break;
            if (op == 52) {
                host->moveNpc(n, v, v[4], last == 1 && s.words == 6 ? EventHost::Facing::Yaw : EventHost::Facing::Keep,
                              frames, last == 2 ? 2 : last == 1 ? 1 : 0);
            } else {
                float yaw; const auto f = facingFor(last, yaw);
                host->moveNpc(n, v, yaw, f, frames, 0);
            }
            break;
        }
        case 57: { // SetDir npc, target: turn towards it
            const int n = vm.resolve(vm.u16()), t = vm.resolve(vm.u16());
            if (host) host->faceNpc(n, t);
            break;
        }
        case 37: case 38: { // FrmSetCam id / FrmAdjCam id, frames: cut or move to the shot
            const int id = int16_t(vm.u16());
            const int frames = op == 38 ? vm.u16() : 0;
            Vm::Scene s;
            float eye[3], target[3];
            if (host && vm.scene(s) && vm.cameraShot(s, state.frmRecord, id, eye, target))
                host->setCamera(eye, target, frames);
            break;
        }
        case 39: { // FrmPanCam dx, dy, dz, frames: shift the camera
            float dlt[3];
            for (float& v : dlt) v = float(int16_t(vm.u16()));
            const int frames = vm.u16();
            if (host) host->panCamera(dlt, frames);
            break;
        }
        case 41: case 42: if (host) host->freeCamera(op == 41); break; // CameraFree / CameraPlayer
        case 51: { const int n = vm.resolve(vm.u16()); if (host) host->loadNpc(n); break; } // NpcMdlRead
        case 28: { // Setc npc, x, y, z, dir: place at world coordinates (dir as FrmSetc)
            const int n = vm.resolve(vm.u16());
            float v[3];
            for (float& c : v) c = float(int16_t(vm.u16()));
            float yaw;
            const auto f = facingFor(int16_t(vm.u16()), yaw);
            if (host) host->placeNpc(n, v, yaw, f);
            break;
        }
        case 48: { // FrmStopEff effect + 99: every FrmPlayEff of it
            const int e = int16_t(vm.u16()) - 99;
            if (host) host->stopFrameEffect(e, -1);
            break;
        }
        case 16: case 58: { // Screen: u16 ?, u16 colour, u8 from alpha, u8 to alpha, u8 frames
            vm.u16();
            const uint16_t colour = vm.u16();
            const uint8_t from = vm.u8(), to = vm.u8(), frames = vm.u8();
            if (host) host->screenFade(float(colour & 255) / 255.0f, from / 255.0f, to / 255.0f, frames);
            break;
        }
        case 21: { // Util n: the game's menus (0 title, 1 world, 2 shop, 3 yes/no, 4 start, 5/6 save, 7 credits, 8 last battle)
            const int n = vm.u8();
            if (n == 3 && host) { host->askYesNo(vm.var(0)); return yield(Vm::Wait::Choice); } // answer in flag var(0)
            if (host && host->menu(n)) return yield(Vm::Wait::Choice);
            break;
        }
        case 23: { // Eff: sound effect id | flags << 16 (bit 23 loop, bit 29 stop)
            const uint32_t lo = vm.u16(), hi = vm.u16();
            if (host) host->playSound(lo | hi << 16);
            break;
        }
        case 22: { // Bgm: track | flags << 16 (bit 27 fade out over 20 frames, bit 29 stop, ...)
            const uint32_t lo = vm.u16(), hi = vm.u16();
            if (host) host->playMusic(lo | hi << 16);
            break;
        }
        case 32: { // Battle: u16 group, u8 stage, u8 flags. __EvtBattle returns -1: the frame ends
            // here (the battle starts), and the script goes on from the next instruction later
            const int group = vm.u16(), b = vm.u16();
            if (host) {
                host->battle(group, b & 0xff, b >> 8);
                return yield(Vm::Wait::None);
            }
            break;
        }
        case 50: { // FrmStopEff2 actor, effect + 99
            const int a = int16_t(vm.u16()), e = int16_t(vm.u16()) - 99;
            if (host) host->stopFrameEffect(e, a);
            break;
        }
        case 24: case 40:
            vm.skip(4); break;
        case 25: { // Tex2d: u16 slot, u16 flags, then u16 image if bit 0, 3 shorts each for bits 2, 5, 3
            const int slot = int16_t(vm.u16());
            const uint16_t f = vm.u16();
            const int image = (f & 1) ? int16_t(vm.u16()) : -1;
            int16_t extra[3][3] = {};
            const uint16_t bits[3] = {4, 0x20, 8};
            for (int b = 0; b < 3; ++b)
                if (f & bits[b])
                    for (int k = 0; k < 3; ++k) extra[b][k] = int16_t(vm.u16());
            if (host) host->tex2d(slot < 0 ? -slot : slot, f, image, extra);
            break;
        }
        case 55: vm.skip(10); break;                       // AdjScale
        case 29: { // Anim: u16 npc, u16 slot, s16 loop from, s16 loop to
            const int n = vm.resolve(vm.u16()), slot = vm.u16();
            const int from = int16_t(vm.u16()), to = int16_t(vm.u16());
            if (host) host->playAnim(n, slot, from, to);
            break;
        }
        case 44: { // LoadAct: s16 npc (sign picks the loader), u16 action file, u16 slot
            const int n = vm.resolve(vm.u16());
            const int action = vm.u16(), slot = vm.u16();
            if (host) host->loadAction(int16_t(n) < 0 ? -int16_t(n) : n, action, slot);
            break;
        }
        case 45: { // UnloadAct: u16 npc, u16 slot
            const int n = vm.resolve(vm.u16()), slot = vm.u16();
            if (host) host->unloadAction(n, slot);
            break;
        }
        case 31: { // Mapscroll who, rate% / Mapscroll < 0, x, y, rate%: scroll a painted field
            const int who = int16_t(vm.resolve(vm.u16())); // 0xFFFF: the running NPC; 0 the player
            const int16_t b = int16_t(vm.u16());
            if (who >= 0) {
                if (host) host->mapScroll(who, 0, 0, float(b) / 100.0f);
            } else {
                const int16_t y = int16_t(vm.u16()), rate = int16_t(vm.u16());
                if (host) host->mapScroll(-1, float(b), float(y), float(rate) / 100.0f);
            }
            break;
        }
        case 43: { // Quake: s16 size (pixels), from %, to %, frames (< 0: held until the next)
            const int amp = int16_t(vm.u16()), from = int16_t(vm.u16()), to = int16_t(vm.u16()), frames = int16_t(vm.u16());
            if (host) host->quake(amp, from, to, frames);
            break;
        }
        case 30: case 54: vm.skip(8); break;
        case 47: case 56: { // FrmPlayEff / FrmPlayEff2 actor, effect + 99, scale %
            const int a = int16_t(vm.u16()), e = int16_t(vm.u16()) - 99, scale = int16_t(vm.u16());
            Vm::Scene s;
            float v[6];
            const bool rec = vm.scene(s) && vm.actor(s, state.frmRecord, a, v);
            if (host) host->frameEffect(a, e, scale, rec ? v : nullptr, op == 56);
            break;
        }
        case 46: { // NpcFade: u16 npc (0 = stage model), u8 from, u8 to, u8 frames (mdl_set_alpha)
            const int n = vm.u16();
            const uint8_t from = vm.u8(), to = vm.u8(), frames = vm.u8();
            if (host && n != 0) host->fadeNpc(vm.resolve(n), from / 255.0f, to / 255.0f, frames);
            break;
        }
        case 49: { // Anim2: u16 npc, u16 slot, s16 loop from, s16 loop to, then 4 bytes
            const int n = vm.resolve(vm.u16()), slot = vm.u16();
            const int from = int16_t(vm.u16()), to = int16_t(vm.u16());
            vm.skip(4);
            if (host) host->playAnim(n, slot, from, to);
            break;
        }
        case 60: vm.skip(15); break;                       // AAAA
        // The Vita release's own (its j001_t99 boot only): 0x40 / 0x44 read the system and save
        // data, 0x41 sets the saves up, 0x45 writes, 0x46 then checks the trophies - nothing here;
        // 0x47 / 0x49 load a logo (u16 slot, u16 picture: 0 sqex, 1 mve, 2 mclogo, 254 warning),
        // 0x48 / 0x4a play it (u16 slot, fade in, hold, fade out frames; a tap or confirm cuts the
        // hold short) and wait for it
        case 63: case 64: case 65: case 67: case 68: break;
        case 71: case 73: {
            const int slot = int16_t(vm.u16()), pic = vm.u16();
            if (host) host->vitaLogo(slot, pic);
            break;
        }
        case 72: case 74: {
            const int slot = int16_t(vm.u16()), in = vm.u16(), hold = vm.u16(), out = vm.u16();
            if (host) {
                host->vitaLogoShow(slot, in, hold, out);
                return yield(Vm::Wait::Logo);
            }
            break;
        }
        default: vm.error = "bad opcode " + std::to_string(op); break;
        }
    }
    return finish(false);
}

EventResult runEvent(const EventFile& file, int eventNo, GameState& state, int currentArea, int currentMap) {
    EventRunner r;
    if (!r.start(file, eventNo, state, currentArea, currentMap)) return {};
    while (r.step(nullptr)) {}
    return r.result();
}

} // namespace cr
