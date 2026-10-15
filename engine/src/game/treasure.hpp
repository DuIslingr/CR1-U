#pragma once
// Treasure boxes (TreasureBoxManager): which boxes an area has, their items, and their flags
// on a map. See docs/FORMATS.md ("NPC models and treasure boxes").

#include <cstdint>
#include <functional>
#include <vector>

#include "assets/event.hpp"
#include "game/event_vm.hpp"

namespace cr {

struct TreasureBox {
    int id = 0;    // box NPC id (its lid is id + 1)
    int item = 1;  // item number (getItemNo; 1 when not found)
};

// randomSelect as it plays (_InitDungeon, each time the World Select leads into a dungeon): the
// same candidates and pools at the route's level band (0..9 of `band`), drawn by `random(n)`
// (0..n-1: the original's rand() % n, seeded by the clock); the items reordered as it does (each
// in turn taken out and put back before a random one), and with the band at 2 or more an item
// from pool slot 9..14 goes into the box's gold counterpart (box 100..139 + 700, 140..179 - 80:
// the gold boxes 60..99) where the area has one
std::vector<TreasureBox> drawTreasureBoxes(const EventFile& file, const std::function<int(int)>& random, int band, int route);

// getItemNo
int treasureItem(const std::vector<TreasureBox>& boxes, int id);

// setup: clears flags 3100..3259, then for each box NPC on the map that was selected sets its
// show flag, and once opened (item 0xFFFF) the hide flag of its lid NPC (id + 1).
// `byPick`: a box is opened when the draw says so (item -1), not by the area's record
void applyTreasureFlags(const EventFile& file, const std::vector<FieldNpc>& npcs, const std::vector<TreasureBox>& selected,
                        GameState& state, bool byPick = false);

} // namespace cr
