#pragma once
// Trophies: CR1's only achievements (docs/ENGINE.md "Trophies"). iOS 2.2.0 / 1.0.1 have no Game
// Center (no GameKit, no achievement code or text) and Android 1.1.1 has no achievement_prc / list;
// the Vita release (PCSG00497, NPWR08182_00) has 16 trophies, unlocked by its own code:
//
// FUN_8106ae3a (the Vita's eboot, the trophy check): every trophy in the order below whose
// condition holds and which isn't unlocked yet is queued (FUN_8106add4: a 16-slot list the system
// unlocks from) and its bit set in G+0x34e38 (16 bits, kept with the system data - global, not per
// save; the boot syncs them from the system's own record, 0x8106eb60). Its callers: __EvtReturn /
// __EvtAbort (a script's end: 0x8102d416 / 0x8102d528), the script's save before the title (Util 6:
// 0x8102e226), the boot's opcode 0x46 (0x810316c0) and the shop's purchase (__menuu_ShopPrc 7:
// FUN_8106ad28(0x2f) - G+0x34e3d bit 7 - then the check, 0x8105c11a).
//
// The trophies (TROPHY.TRP's TROPCONF.SFM): 0 Gold (all the others), 1..15 Bronze, 1..14 hidden.

#include "game/event_vm.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace cr {
struct Image;
}

namespace cr::trophies {

constexpr int kCount = 16;
constexpr int kBought = 0x2f; // G+0x34e3d bit 7: something bought in the shop (__menuu_ShopPrc 7)

// G+0x34e38..: the trophies' bits (0..15) and the shop's (0x2f)
struct Flags {
    std::array<uint8_t, 8> bits{};
    bool has(int n) const { return n >= 0 && n < 64 && (bits[size_t(n >> 3)] >> (n & 7) & 1); }
    void set(int n) { if (n >= 0 && n < 64) bits[size_t(n >> 3)] |= uint8_t(1u << (n & 7)); }
    void clear(int n) { if (n >= 0 && n < 64) bits[size_t(n >> 3)] &= uint8_t(~(1u << (n & 7))); }
    int earned() const { int k = 0; for (int n = 0; n < kCount; ++n) k += has(n); return k; }
    // settings.ini's trophies=: 16 hex digits, byte 0 first
    std::string hex() const;
    bool fromHex(const std::string& s);
};

// FUN_8106ae3a: the trophies to unlock now, in its order (1..15, then 0 once 1..15 are all in),
// each set in `f` as it is queued
std::vector<int> check(const GameState& gs, Flags& f);

// The trophy pack's texts and pictures (TROPHY.TRP: TROP.SFM's names, TROPnnn.PNG)
struct Info {
    std::string name, detail;
    char grade = 'B';     // ttype: P, G, S, B
    bool hidden = false;
    std::vector<uint8_t> icon; // its PNG
};
// Reads sce_sys/trophy/<id>/TROPHY.TRP (unencrypted on the Vita: a TRP of SFM / PNG entries);
// false when it isn't one
bool readPack(const std::string& path, std::array<Info, kCount>& out);
// The pack's own texts, for a copy without its sce_sys (the Vita's are Japanese only)
const char* builtinName(int n);
char builtinGrade(int n);
// "(Bronze)", "(Gold)" ...: the notification's grade word
const char* gradeWord(char grade);
// The trophy's picture (its PNG) as RGBA; false without one
bool decodeIcon(const Info& info, Image& out);

// The notification as the reference playthrough shows it (Part 1 1850.6-1852.8 s: the emulator's,
// Vita3K's - the Vita draws its own; the game only unlocks): one at a time, sliding in from the
// screen's right edge, held, and out. In seconds
struct Notice {
    static constexpr float kIn = 0.23f, kHold = 1.73f, kOut = 0.23f;
    bool on = false;
    int id = -1;
    float t = 0;
    void start(int n) { on = true, id = n, t = 0; }
    void step(float dt) {
        if (!on) return;
        t += dt;
        if (t >= kIn + kHold + kOut) on = false;
    }
    // how far it is in: 0 off the screen's right edge, 1 in place
    float shown() const {
        if (!on) return 0;
        if (t < kIn) return t / kIn;
        if (t < kIn + kHold) return 1;
        return std::max(0.0f, 1 - (t - kIn - kHold) / kOut);
    }
};

} // namespace cr::trophies
