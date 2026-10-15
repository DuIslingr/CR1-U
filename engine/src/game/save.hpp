#pragma once
// Save data (ours, not the original's binary layout): what the original's SaveData_Save /
// ContData_Save keep - the scripts' flags, variables, items and opened boxes, where the party
// stands and who leads it, the play time and the date - as a small text file:
//
//   CRSAVE 1
//   area 2 / map 0 / pos x y z / yaw r / leader 0 / play 1234.5 / saved 1760000000 (unix time)
//   flags <128 hex words>
//   vars n=v n=v ...   items n=c ...   oz n   party c level exp hp mp weapon armor jewel (x8)   boxes id ...
//
// One save slot (save.dat, the Camp menu's) and the Progress Tracker's resume point
// (resume.dat, written when the game is left on the field), as in the original.

#include "game/event_vm.hpp"

#include <cstdint>
#include <string>

namespace cr {

struct SaveData {
    int area = -1, map = -1;
    float pos[3] = {0, 0, 0};
    float yaw = 0;
    int leader = 0;          // party leader model (f001.bin + n)
    double playSeconds = 0;
    int64_t savedAt = 0;     // unix time
    GameState game;          // flags, vars, items, Oz, party, opened boxes (not the cutscene frame state)
};

bool writeSave(const std::string& path, const SaveData& s);
bool readSave(const std::string& path, SaveData& s, std::string* error = nullptr);

} // namespace cr
