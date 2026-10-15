#pragma once
// Event files (.evt): per-map stage record (field camera), NPC/trigger lists, the NPC table,
// the warp table and the script bytecode. See docs/FORMATS.md ("Event files", "Event scripts").

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cr {

struct StageCamera {
    float eye[3] = {0, 0, 0};
    float target[3] = {0, 0, 0};
    float fovDegrees = 45;
    int viewWidth = 0, viewHeight = 0; // size of the pre-rendered view (= background size)
    int hitRadius = -1;                // player wall-sphere radius (+68), <= 0 = default 5
    int rect[4] = {0, 0, 0, 0};        // +44: x, y, w, h of the painting in the view (the scroll limits)
    int lookHeight = 0;                // +70: added to the player's height + 7 when the camera follows
    std::string eventFile;
    int area = -1;      // index of the area (event file) the record came from
    int mapNumber = -1;
};

// Map numbers whose collision file is `cmpName` (several maps can share one).
std::vector<int> mapNumbersFor(const std::string& cmpName);

// Reads the stage record of `mapNumber`. Map numbers are shared by the four routes' copies
// of a dungeon, so `area` picks the event file; -1 = the first area that covers the map.
bool loadStageCamera(const std::string& dataDir, int mapNumber, StageCamera& out, std::string* error = nullptr,
                     int area = -1);

// One entry of a map's NPC list (52-byte record) joined with its NPC table row (40 bytes).
struct FieldNpc {
    int id = 0;                  // also the script started when it is touched / examined
    float pos[3] = {0, 0, 0};
    float angle = 0;             // radians
    float box[3] = {0, 0, 0};    // trigger box size (x, y, z)
    float offset[3] = {0, 0, 0};
    int flagShow = -1, flagHide = -1; // NPC table +0 / +2 (flag numbers, -1 = none)
    int model = -1;              // NPC table +4, < 0 = no model (an invisible trigger)
    uint16_t kind = 0;           // NPC table +14: bit 0 + bit 15 = runs its script on touch
    uint16_t treasure = 0;       // NPC table +18: bit 0/1 = treasure box (TreasureBoxManager)
    uint16_t puzzle = 0;         // NPC table +20: the puzzle object it is (game/puzzle.hpp)
    int16_t link[4] = {0, 0, 0, 0}; // NPC table +22..+28: a candle switch's linked NPCs; a candle block's
                                    // start (+22, 0 = put away) and switch (+24)
    float scale = 1;             // NPC table +38: percent, -1 = 100

    bool touchTrigger() const { return (kind & 1) && (kind & 0x8000); }
    // Box centre as RectWrapper::checkEventKick computes it (the offset is applied twice
    // for model-less NPCs: once in npc_setup, once in the check)
    float boxCenterX() const { return pos[0] + offset[0] * (model <= 1 ? 2.0f : 1.0f); }
    float boxCenterZ() const { return pos[2] + offset[2] * (model <= 1 ? 2.0f : 1.0f); }
};

// Section 4: destinations for Warp 0xFFFD, keyed by the running event's number.
struct WarpEntry {
    float pos[3] = {0, 0, 0};
    int key = -1, area = -1, map = -1; // area -1 = stay in the current area
    int angleDegrees = 0;
};

// One row of the NPC table (section 3), by NPC id.
struct NpcRow {
    int flagShow = -1, flagHide = -1, model = -1;
    int item = -1;          // +12: a treasure box's fixed item (-1 = chosen by TreasureBoxManager)
    uint16_t kind = 0, box = 0;
    uint16_t puzzle = 0;    // +20: what a puzzle room makes of it (game/puzzle.hpp kPuzzle*)
    int16_t link[4] = {0, 0, 0, 0}; // +22..+28 (FieldNpc::link)
    float scale = 1;
};

// What a message says about its speaker (PrcMsg): 0x15 n shows the name plate - system message
// 58028 + n (font_getfont_utf16 block 7, entry n + 28; n = the jNNN character number); 0x1c picks
// the speaking side and dims the other side's portraits to 128/255 (slot +0x28/+0x2a).
struct MessageInfo {
    int nameId = -1;   // system message holding the speaker's name, -1 = no plate
    int side = 0;      // 0x1c: 1 = the left-hand portrait speaks (the right dims), 2 = the right, 0 = none
    // 0x1e n: a voice clip (voiced builds: the 2.2.0 iPhone release and the Vita). n is the entry of
    // voiceevnt.bin, numbered across all areas; 65535 = none. 1.0.1 skips the code.
    struct VoiceCue { int page; int clip; };
    std::vector<VoiceCue> voices;
    // PrcMsg types a page out a character a frame. 0x1a n: the typing stops n frames before the
    // page's character `at` (counted over its lines). 0x14 ends the message without waiting for a
    // tap: the last page closes `closeAfter` frames (the 0x1a just before it) once it's out; -1
    // waits for the tap (0 / 0x13)
    struct Pause { int page; int at; int frames; };
    std::vector<Pause> pauses;
    int closeAfter = -1;
};

constexpr int kLanguages = 8; // text sections: ja, en, fr, de, es, zh-Hans, zh-Hant, ko (1.0.1: ja, en)
extern const char* const kLanguageName[kLanguages];

class EventFile {
public:
    bool load(const std::string& dataDir, int area, std::string* error = nullptr);
    bool loaded() const { return !data_.empty(); }
    int area() const { return area_; }
    const std::vector<uint8_t>& bytes() const { return data_; }

    uint32_t scriptEntry(int n) const;                  // file offset of script n, 0 = none
    // Section 1 (EvrStart): 9 offsets of scripts the game itself starts. Entry 5 runs on every
    // room change within the area (fldread_prc); 0 when the area loads without script 0.
    uint32_t returnEntry(int n) const;
    // Section 6 (EvtFrmSet): file offset of cutscene scene n (1-based), 0 = none. A scene is
    // u32 actor count (+10000: 6 words per actor, else 3), u32 record count, the actor ids,
    // then records {u32 frame, i32 values[actors][words]}: position x100, then rotation in
    // degrees x100.
    uint32_t sceneOffset(int n) const;
    std::vector<FieldNpc> npcs(int mapNumber) const;    // the map's NPC / trigger list
    bool npcRow(int id, NpcRow& out) const;             // NPC table row
    bool warpEntry(int key, WarpEntry& out) const;      // Warp 0xFFFD lookup (EvtWarp)
    std::vector<WarpEntry> warps() const;               // the whole warp table
    // Message `id` (EvtMsg: id - 1000 -> block id / 300, entry id % 300) in `language`
    // (0 = Japanese, 1 = English), decoded to pages of UTF-8 lines. `var` supplies values for
    // the variable-insert code. Ids >= 51000 are system messages from elsewhere: false.
    bool message(int id, int language, const std::function<int(int)>& var,
                 std::vector<std::vector<std::string>>& pages, MessageInfo* info = nullptr) const;

private:
    std::vector<uint8_t> data_;
    int area_ = -1;
    uint32_t sec_[7] = {};
};

// System messages (ids >= 51000; EvtMsg's second path), from sys.evt: header u32[language] ->
// table of block offsets, blocks of 1000 message offsets.
class SystemText {
public:
    bool load(const std::string& dataDir);
    bool hasLanguage(int language) const; // its section exists (header u32[language] != 0)
    // `text` supplies the strings of the string-insert code (3 n); the menus put the item's
    // name in string 9
    bool message(int id, int language, const std::function<int(int)>& var,
                 std::vector<std::vector<std::string>>& pages, MessageInfo* info = nullptr,
                 const std::function<std::string(int)>& text = nullptr) const;

private:
    std::vector<uint8_t> data_;
};

// Area of `map` given the current area: the current one if it covers the map, else the first.
int areaForMap(int mapNumber, int currentArea);

} // namespace cr
