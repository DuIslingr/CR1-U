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
// One file for the whole game, as 2.2.0's data.dat (SaveData_Save 0x725d0: 0xc24 bytes from
// +0x2fcc0 - the Oz +0xc and the flags shared by the four pairs, each pair's bag +0x580 + 0x100 r,
// its saved place +0xad0 + 0x14 r and its "saved" mark +0xc14 + r): data.dat, written by the Camp's
// save and the scripts' Util 5 / 6. The lines above are the pair in play's place (the leader's pair);
//   place r has area map x y z yaw leader play saved      (each pair's own, SaveData_Load_init)
//   routebag r id=n ...                                    (each other pair's own items)
// The Progress Tracker's resume point (resume.dat, as cont.dat) is a whole copy. The earlier
// releases' one save per pair (save1.dat .. save4.dat) are folded into data.dat once and left as
// they are (menus.cpp readData).

#include "game/event_vm.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace cr {

class ItemTable;

struct SaveData {
    int area = -1, map = -1;
    float pos[3] = {0, 0, 0};
    float yaw = 0;
    int leader = 0;          // party leader model (f001.bin + n)
    double playSeconds = 0;
    int64_t savedAt = 0;     // unix time
    GameState game;          // flags, vars, items, Oz, party, opened boxes (not the cutscene frame state)
    // Each pair's saved place (+0xad0 + 0x14 r) and "saved" mark (+0xc14 + r: the title's Load offers
    // the pair). A file without `place` lines (older, or a resume point) has the leader's pair's
    // from the lines above
    struct Place {
        bool has = false;
        int area = -1, map = -1;
        float pos[3] = {0, 0, 0};
        float yaw = 0;
        int leader = 0;
        double playSeconds = 0;
        int64_t savedAt = 0;
    };
    std::array<Place, 4> places{};
    // False for a file of the per-pair releases (no `routebags` line): its other pairs' bags and
    // records are not its own
    bool wholeGame = true;

    int route() const { return leader / 2 < 0 ? 0 : leader / 2 > 3 ? 3 : leader / 2; }
    Place placeInPlay() const { return Place{true, area, map, {pos[0], pos[1], pos[2]}, yaw, leader, playSeconds, savedAt}; }
    // The file as pair `r` would be loaded (Set_Prtyno + SaveData_Load_init): its place on top. The play
    // time and the date stay the file's own - 2.2.0 keeps one clock in the block (+4 / +8, 72123-72126) and
    // shows data.dat's date (_Load_Date, 72121), whichever pair saved last
    void takePlace(int r) {
        if (r < 0 || r > 3 || !places[size_t(r)].has) return;
        const Place& p = places[size_t(r)];
        area = p.area, map = p.map, pos[0] = p.pos[0], pos[1] = p.pos[1], pos[2] = p.pos[2], yaw = p.yaw;
        leader = p.leader;
    }
};

// Where 2.2.0's add_item / GetItmCnt / del_item (0x5fbc0, 0x5f058, 0x5ecd4) keep an item: kinds 4, 5,
// 9 and 10 (the genes, skills, manual and plates) in the shared bag (G+0x30640); 81..88 and
// 131..138 in flags per pair (0xf59 / 0xf2f + item + 16 route); the weapons and armor 201..424 in
// the bag of the pair whose they are ((item - 201) / 56, seen from any pair); the rest in the bag of
// the pair in play. `t` may be null (the 2.2.0 table's kinds by number)
enum class ItemHome { Shared, Gear, Route, RouteFlag };
ItemHome itemHome(const ItemTable* t, int item);
// Set_Prtyno: the pair `r`'s own items into `items`, the pair's that were there kept in routeBags
void changeRoute(GameState& g, const ItemTable* t, int r);
// Folds `s` - a newer per-pair save of its leader's pair - into `d`: its pair's records, bag, place,
// walking EXP, encounter switch, band and treasure draw; its flags added; its variables, Oz and
// place in play over d's; shared items at the larger count; opened boxes together
void foldPairSave(SaveData& d, const SaveData& s, const ItemTable* t);

bool writeSave(const std::string& path, const SaveData& s);
bool readSave(const std::string& path, SaveData& s, std::string* error = nullptr);

} // namespace cr
