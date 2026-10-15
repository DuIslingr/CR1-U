#pragma once
// Battle data: the monster table and enemy groups from sys.evt (gameapp_init points globals at
// its header's sections), and the per-map encounter tables of the area files (fldencount_*).

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace cr {

class EventFile;

// A 204-byte row of sys.evt section 12 (header +0x30), read by read_btlcpl / btl_enemy_init_lv.
struct MonsterDef {
    int id = 0;               // 1-based, as groups name it
    uint32_t hp = 0;          // +0, plus 200 (strong growth) or 30 per level
    uint32_t exp = 0;         // +4, plus level squared
    uint32_t genePoints = 0;  // +8, plus 15 or 10 per level
    int model = -1;           // +12: kBattleModelFile index, -1 = unused row
    int size = 0;             // +26: size class 1-5, the digits of the formation code
    bool strongGrowth = false; // +28
    int level = 0;            // +32: fixed level, <= 0 = from the party's
    int atk = 0, grd = 0, spd = 0, lck = 0; // +38..+44, plus 12/10/8/9 or 6/4/5/4 per level
    std::array<uint8_t, 204> raw{};

    // The stats at level `lv` (read_btlcpl)
    uint32_t hpAt(int lv) const { return hp + uint32_t(lv) * (strongGrowth ? 200 : 30); }
    uint32_t expAt(int lv) const { return exp + uint32_t(lv * lv); }
    uint32_t genePointsAt(int lv) const { return genePoints + uint32_t(lv) * (strongGrowth ? 15 : 10); }
    int atkAt(int lv) const { return atk + lv * (strongGrowth ? 12 : 6); }
    int grdAt(int lv) const { return grd + lv * (strongGrowth ? 10 : 4); }
    int spdAt(int lv) const { return spd + lv * (strongGrowth ? 8 : 5); }
    int lckAt(int lv) const { return lck + lv * (strongGrowth ? 9 : 4); }
};

// One group a map can meet: 12 bytes of an encounter table.
struct EncounterGroup {
    int group = 0;       // 1-based enemy group
    int minLevel = 0;    // only when the party's average level is at least this
    int extra = -1;      // +12, kept for the battle (G+0x354c2)
    int weight = 0;      // relative chance among the eligible groups
    int firstStrike = 0; // % the party strikes first (50 with gene 171)
    int ambush = 0;      // % the enemy strikes first (none with gene 172)
};

struct EncounterTable {
    int stage = -1;      // kBattleStageFile index; -1 = no encounters on this map
    int distance = 0;    // units walked between checks, / 10
    int rate = 0;        // % chance at a check; +5 after each miss, back to this after a battle
    std::vector<EncounterGroup> groups;
};

// One of a battle model's 13 action slots (sys.evt section 13, 18 bytes; btlanm_get_*): slot k
// plays the package's action k + 1. Frames: loop range (0, 0 = the whole action), when the
// actor moves, when hits land.
struct BattleAnim {
    int loopStart = 0, loopEnd = 0, moveStart = 0, moveEnd = 0, hit = 0, hit2 = 0;
};
constexpr int kBattleAnimSlots = 16; // 13 for most models, up to 15 for the bosses

// A battle camera (sys.evt section 15, 32 bytes; btlcamera_set): eye and target in stage units
// (10000-based values are relative to an actor), then timing.
struct BattleCamera {
    int eye[3] = {}, target[3] = {};
    int16_t raw[16] = {};
};

class BattleData {
public:
    bool load(const std::string& dataDir); // sys.evt
    bool loaded() const { return !monsters_.empty(); }
    const MonsterDef* monster(int id) const; // 1-based
    // The (up to) three monster ids of a 1-based group; 0 = empty slot
    std::array<int, 3> group(int id) const;
    int groupCount() const { return int(groups_.size()); }
    int monsterCount() const { return int(monsters_.size()); }
    // read_btlcpl: the formation (kBattlePosition row) for these monsters, -1 if the game has none
    int formation(const std::array<int, 3>& ids) const;
    // Action slot `slot` of battle model `model` (kBattleModelFile index); zeros if none
    BattleAnim anim(int model, int slot) const;
    const BattleCamera* camera(int id) const;
    int cameraCount() const { return int(cameras_.size()); }

private:
    std::vector<MonsterDef> monsters_;
    std::vector<std::array<int, 3>> groups_;
    std::vector<std::array<BattleAnim, kBattleAnimSlots>> anims_;
    std::vector<BattleCamera> cameras_;
};

// The encounter table of `mapNumber` in the loaded area file (header +0x14: u32 per map of the
// area -> u16 stage (0xFFFF none), s16 distance, s16 rate, u16, then 12-byte groups up to 128
// ending with a 0xFFFF group). False if the map has none.
bool encounterTable(const EventFile& events, int mapNumber, EncounterTable& out);

} // namespace cr
