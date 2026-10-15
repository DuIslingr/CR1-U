#pragma once
// Treasure boxes (TreasureBoxManager): which boxes an area has, their items, and their flags
// on a map. See docs/FORMATS.md ("NPC models and treasure boxes").

#include <cstdint>
#include <vector>

#include "assets/event.hpp"
#include "game/event_vm.hpp"

namespace cr {

struct TreasureBox {
    int id = 0;    // box NPC id (its lid is id + 1)
    int item = 1;  // item number (getItemNo; 1 when not found)
};

// randomSelect: candidates are every second NPC id in [100, 140) and [140, 180) whose NPC
// table +18 bit 0 is set; random ones are dropped until 7 and 8 remain. Their items come from
// one of _TBOX_TABLE_A/B/C (row `level` 0..9, 13 items) plus two from _TBOX_PAIR_TABLE_A/B/C
// [level][route], shuffled and dealt out in box order. The original seeds with the clock;
// `seed` makes it repeatable.
std::vector<TreasureBox> selectTreasureBoxes(const EventFile& file, uint32_t seed, int level, int route);

// getItemNo
int treasureItem(const std::vector<TreasureBox>& boxes, int id);

// setup: clears flags 3100..3259, then for each box NPC on the map that was selected sets its
// show flag, and once opened (item 0xFFFF) the hide flag of its lid NPC (id + 1).
void applyTreasureFlags(const EventFile& file, const std::vector<FieldNpc>& npcs, const std::vector<TreasureBox>& selected,
                        GameState& state);

} // namespace cr
