#pragma once
// Puzzle rooms (PuzzleManager and its PuzzleObjects; the "Puzzle" rooms on the gates' maps).
//
// Area 1 (j001_p01) holds the 64 puzzle stages, maps 114..177 (_get_stgno: map - 114 while in
// area 1). Each has a record (PUZZLE_ROOM_DATA, filled by its static initializer at 0x214c, 7
// words): the touch ability its pair uses there, the ESP count (uses of it, shown on the field
// menu; -1 = no limit), the grid (rows along z, columns along x, 10-unit cells) and its corner.
// The room's NPCs become the grid's objects by their NPC table +20 bits (createPuzzleObject).

namespace cr {

enum TouchAbility { kAbilityNone = 0, kAbilityTeleport = 1, kAbilityPsychokinesis = 2, kAbilityToughness = 4,
                    kAbilityPyrokinesis = 5 };

struct PuzzleStage {
    int ability = 0;
    int esp = -1;            // the ability's uses; -1: no limit (none shown)
    int rows = 5, cols = 5;  // cells along z / along x
    float x0 = -25, y = 10, z0 = -25; // the grid's corner; y the objects' height
};

constexpr int kPuzzleStages = 64, kFirstPuzzleMap = 114, kPuzzleArea = 1;
constexpr float kPuzzleCell = 10.0f;      // 0xca8d8
constexpr float kPuzzleSwitchRise = 2.5f; // 0xca8dc: an object standing on a weigh switch
// Object types (getType) and what holds them
enum PuzzleType { kTypeNormal = 1, kTypeFixed = 2, kTypeFragile = 3, kTypeCandle = 4, kTypeRed = 5, kTypeBlue = 6,
                  kTypeCandleSwitch = 7, kTypeWeighSwitch = 8, kTypeGem = 9, kTypeCrystal = 10 };
// The type an NPC with these +20 bits is in a room of this ability (0: none)
int puzzleType(unsigned bits, int ability);
inline int puzzleLayer(int type) { return type == kTypeWeighSwitch ? 0 : type == kTypeCrystal ? 2 : 1; }
extern const PuzzleStage kPuzzleStage[kPuzzleStages];

// NPC table +20 bits (createPuzzleObject): what the NPC is in the puzzle
enum PuzzleBits : unsigned {
    kPuzzleNormalBlock = 0x001,  // NormalBlockTP (teleport rooms) / NormalBlockPK (psychokinesis)
    kPuzzleFixedBlock = 0x002,
    kPuzzleFragileBlock = 0x004,
    kPuzzleCandleBlock = 0x008,
    kPuzzleCandleSwitch = 0x010,
    kPuzzleWeighSwitch = 0x020,
    kPuzzleObject40 = 0x040,     // plain objects (vtables 0xcf084 / 0xcf080 / 0xcf07c)
    kPuzzleGem = 0x080,
    kPuzzleObject100 = 0x100,
    kPuzzleRedBlock = 0x200,
    kPuzzleBlueBlock = 0x400,
    kPuzzleObject800 = 0x800,
};

// The stage of map `map` in area `area` (-1 none)
inline int puzzleStage(int area, int map) {
    return area == kPuzzleArea && map >= kFirstPuzzleMap && map < kFirstPuzzleMap + kPuzzleStages ? map - kFirstPuzzleMap : -1;
}

} // namespace cr
