#pragma once
// Shops (__menuu_Shop*, MuCom_Shop*): the item table, what a shop sells and at what price.
//
// Items (GetItmPara): sys.evt section +0x38, 20 bytes per item from item 1 - u32 price, s16
// HP, MP, Attack, Defense, Speed, Luck (equipment), s8 icon (+16, GetIconNoFromItemNo), u8 kind
// (+17: 1 / 2 consumables, 6 weapon, 7 armor, 8 jewel), u8 (+18), u8 owners (+19: bit c =
// character c can use it). Item n's name is system message 51000 + n - 1, its help text 52000 + n - 1.
//
// Stock (__GetShopItemTbl) depends on the character shown (0..7: each has their own weapons and
// armor) and the shop's level 2..7 (MuCom_GetShopBuyLevel, from the pair's story flags):
// consumables from a table by level (plus the four stat seeds 3..6 once flag 39 is set), weapons
// 201 + 28c.. and armor 215 + 28c.. (`level` of each), jewels 426.. (level - 1). Selling
// (MuCom_ShopMakeSellItemData) lists what the party owns of the category's range that the
// character may use, for half the price
// (GetSellPrice). A purchase is limited by the Oz and by 99 of an item.

#include <cstdint>
#include <string>
#include <vector>

#include "game/event_vm.hpp"

namespace cr {

struct ItemParam {
    int price = 0;
    int stat[6] = {};   // HP, MP, Attack, Defense, Speed, Luck
    int icon = -1;      // 0..63: 32 x 32 at (icon % 16 * 32, 248 + icon / 16 * 32) of window_hd
    int kind = 0;
    int owners = 0;
};

enum ItemKind { kKindWeapon = 6, kKindArmor = 7, kKindJewel = 8 };

class ItemTable {
public:
    bool load(const std::string& dataDir); // sys.evt
    bool loaded() const { return !items_.empty(); }
    const ItemParam* item(int n) const;    // 1-based, null if out of range
    int count() const { return int(items_.size()); }

private:
    std::vector<ItemParam> items_;
};

constexpr int kMaxOz = 9999999;  // MuCom_AddOz
constexpr int kMaxItemCount = 99;
constexpr int kShopCategories = 4; // Items, Weapons, Armor, Jewel

inline int itemNameId(int item) { return 51000 + item - 1; }
inline int itemHelpId(int item) { return 52000 + item - 1; }
inline int sellPrice(const ItemParam& p) { return p.price / 2; }
// The buying price; the shop takes 10% off when either of the pair wears skill 173 (main.cpp)
inline int buyPrice(const ItemParam& p) { return p.price; }

// MuCom_GetShopBuyLevel: 2..7 for the pair `route` (0..3)
int shopLevel(const GameState& gs, int route);
// __GetShopItemTbl / MuCom_ShopMakeBuyItemData: what the shop sells in `category` for `character`
std::vector<int> shopStock(const GameState& gs, const ItemTable& items, int character, int category, int level);
// MuCom_ShopMakeSellItemData: what the party can sell in `category` (for `character`)
std::vector<int> sellableItems(const GameState& gs, const ItemTable& items, int character, int category);
// How many of `price` the Oz buy, without going over 99 of the item
int maxPurchase(int price, int oz, int owned);
void addOz(GameState& gs, int delta); // clamped to 0..kMaxOz

} // namespace cr
