#include "game/puzzle.hpp"

namespace cr {

// PUZZLE_ROOM_DATA (0xd5000, set by __GLOBAL__I_PUZZLE_ROOM_DATA): ability, ESP count, rows, columns,
// corner x, height, corner z
const PuzzleStage kPuzzleStage[kPuzzleStages] = {
    {1, 0, 5, 5, -25, 10, -25}, {1, 0, 5, 5, -25, 10, -25}, {1, 0, 5, 5, -25, 10, -25}, {1, 3, 5, 5, -25, 10, -25},
    {1, 0, 5, 5, -25, 10, -25}, {1, 1, 5, 5, -25, 10, -25}, {1, 1, 5, 5, -25, 10, -25}, {1, 3, 5, 10, -50, 10, -25},
    {1, 3, 5, 5, -25, 10, -25}, {1, 5, 5, 5, -25, 10, -25}, {1, 5, 5, 5, -25, 10, -25}, {1, 5, 5, 5, -25, 10, -25},
    {1, 3, 5, 5, -25, 10, -25}, {1, 3, 5, 5, -25, 10, -25}, {1, 3, 5, 5, -25, 10, -25}, {1, 5, 5, 5, -25, 10, -25},
    {2, -1, 5, 5, -25, 10, -25}, {2, -1, 5, 5, -25, 10, -25}, {2, -1, 5, 5, -25, 10, -25}, {2, -1, 5, 5, -25, 10, -25},
    {2, 3, 5, 5, -25, 10, -25}, {2, -1, 5, 5, -25, 10, -25}, {2, 3, 5, 10, -50, 10, -25}, {2, 8, 5, 10, -50, 10, -25},
    {2, -1, 5, 5, -25, 10, -25}, {2, -1, 5, 5, -25, 10, -25}, {2, -1, 5, 5, -25, 10, -25}, {2, -1, 5, 5, -25, 10, -25},
    {2, -1, 5, 5, -25, 10, -25}, {2, -1, 5, 5, -25, 10, -25}, {2, -1, 5, 5, -25, 10, -25}, {2, -1, 5, 5, -25, 10, -25},
    {4, 2, 5, 5, -25, 10, -25}, {4, 2, 5, 10, -50, 10, -25}, {4, 4, 5, 10, -50, 10, -25}, {4, 2, 5, 10, -50, 10, -25},
    {4, 0, 5, 5, -25, 10, -25}, {4, 0, 5, 5, -25, 10, -25}, {4, 0, 5, 10, -50, 10, -25}, {4, 0, 5, 10, -50, 10, -25},
    {4, 0, 5, 5, -25, 10, -25}, {4, 0, 5, 5, -25, 10, -25}, {4, 0, 5, 5, -25, 10, -25}, {4, 0, 5, 5, -25, 10, -25},
    {4, 10, 5, 5, -25, 10, -25}, {4, 4, 5, 5, -25, 10, -25}, {4, 0, 5, 5, -25, 10, -25}, {4, 0, 5, 5, -25, 10, -25},
    {5, 0, 5, 5, -25, 10, -25}, {5, 0, 5, 10, -50, 10, -25}, {5, 0, 5, 10, -50, 10, -25}, {5, 0, 5, 10, -50, 10, -25},
    {5, -1, 5, 5, -25, 10, -25}, {5, -1, 5, 5, -25, 10, -25}, {5, -1, 5, 10, -50, 10, -25}, {5, -1, 5, 10, -50, 10, -25},
    {5, -1, 5, 5, -25, 10, -25}, {5, -1, 5, 5, -25, 10, -25}, {5, -1, 5, 5, -25, 10, -25}, {5, -1, 5, 5, -25, 10, -25},
    {5, 2, 5, 5, -25, 10, -25}, {5, 5, 5, 5, -25, 10, -25}, {5, 7, 5, 5, -25, 10, -25}, {5, 4, 5, 5, -25, 10, -25},
};

// createPuzzleObject: bit 0 is a normal block in teleport / psychokinesis rooms only; the first
// other bit set decides
int puzzleType(unsigned bits, int ability) {
    if (bits & kPuzzleNormalBlock) return ability == kAbilityTeleport || ability == kAbilityPsychokinesis ? kTypeNormal : 0;
    if (bits & kPuzzleFixedBlock) return kTypeFixed;
    if (bits & kPuzzleFragileBlock) return kTypeFragile;
    if (bits & kPuzzleCandleBlock) return kTypeCandle;
    if (bits & kPuzzleRedBlock) return kTypeRed;
    if (bits & kPuzzleBlueBlock) return kTypeBlue;
    if (bits & kPuzzleCandleSwitch) return kTypeCandleSwitch;
    if (bits & kPuzzleWeighSwitch) return kTypeWeighSwitch;
    if (bits & kPuzzleGem) return kTypeGem;
    if (bits & (kPuzzleObject40 | kPuzzleObject100 | kPuzzleObject800)) return kTypeCrystal;
    return 0;
}

} // namespace cr
