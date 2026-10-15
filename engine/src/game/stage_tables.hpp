#pragma once
// Field-loading tables from the original binary (generated: tools/gen_tables.py).

namespace cr {

constexpr int kNumAreas = 28;
constexpr int kNumMaps = 178; // 114..177: the 64 puzzle stages (area 1)
constexpr int kNumNpcModels = 305;
constexpr int kNumTex2dImages = 254;
constexpr int kNumActionFiles = 205;
constexpr int kNumBattleStages = 19;
constexpr int kNumBattleModels = 89;

extern const char* const kAreaEventFile[kNumAreas];  // area -> event file (.evt)
extern const short kAreaFirstMap[kNumAreas];         // area -> first map number in its event file
extern const char* const kMapCollisionFile[kNumMaps]; // map number -> collision file (.cmp)
extern const char* const kNpcModelFile[kNumNpcModels]; // NPC table model index -> model package
extern const char* const kTex2dImage[kNumTex2dImages];  // Tex2d image number -> texture (portraits, titles, ...)
extern const char* const kActionFile[kNumActionFiles];  // LoadAct action number -> .mca file
// Battles: encounter stage number -> stage model; monster model number -> battle model package
extern const char* const kBattleStageFile[kNumBattleStages];
extern const char* const kBattleModelFile[kNumBattleModels];
// read_btlcpl: the enemies' size classes as decimal digits (first enemy = ones) -> index / 3 is
// the formation; positions [formation][slot][x, z]: 0-2 enemies, 3-5 where attackers stand to
// hit them, 6-11 the party
extern const short kBattleFormationCode[48];
extern const short kBattlePosition[16][12][2];
// Menu pictures (MuTex_GetTexInfo sheet 0): x, y, w, h in window_hd.mct at half scale (the
// menus' 512x384 space; double both for the HD screen and texture)
extern const short kMenuPicture[156][4];
// TreasureBoxManager::randomSelect item pools: [table A/B/C][level 0..9][13 items] and the two
// extra items per [table][level][route 0..3]
extern const short kTreasureItems[3][10][13];
extern const short kTreasurePairs[3][10][4][2];
// PlayFootstepSE: step frames of the run / walk animation per party (route), and three sounds
// per floor surface (colour 254,0,0 / 175,175,175 / 125,185,175 / 205,220,230 / 140,190,190)
extern const short kFootstepFrameRun[4][2];
extern const short kFootstepFrameWalk[4][2];
extern const short kFootstepSound[5][3];

} // namespace cr
