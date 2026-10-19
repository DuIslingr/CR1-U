#include "assets/event.hpp"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstring>

#include "core/file.hpp"
#include "core/text.hpp"
#include "game/stage_tables.hpp"

namespace cr {

namespace {

constexpr size_t kStageEntrySize = 52; // records before the stage record, terminated by 0xFFFF

template <typename T>
bool rd(const std::vector<uint8_t>& f, size_t pos, T& v) {
    if (pos + sizeof(T) > f.size()) return false;
    std::memcpy(&v, f.data() + pos, sizeof(T));
    return true;
}

// Map numbers covered by an area: [first map, next larger first map) — see npc_get_stgmapctl
int areaEnd(int area) {
    int end = kNumMaps;
    for (int a = 0; a < kNumAreas; ++a)
        if (kAreaFirstMap[a] > kAreaFirstMap[area] && kAreaFirstMap[a] < end) end = kAreaFirstMap[a];
    return end;
}

} // namespace

std::vector<int> mapNumbersFor(const std::string& cmpName) {
    std::vector<int> out;
    for (int m = 0; m < kNumMaps; ++m)
        if (cmpName == kMapCollisionFile[m]) out.push_back(m);
    return out;
}

// Search order when no area is given: the route areas first, then the special ones
// (0 = j001_t99, 26 = j001_t91, 27 = j001_d91) which reuse the town / dungeon map numbers
int areaSearchOrder(int i) {
    if (i < kNumAreas - 3) return i + 1;
    return i == kNumAreas - 3 ? 0 : i;
}

bool loadStageCamera(const std::string& dataDir, int mapNumber, StageCamera& out, std::string* error, int onlyArea) {
    for (int i = 0; i < kNumAreas; ++i) {
        const int area = areaSearchOrder(i);
        if (onlyArea >= 0 && area != onlyArea) continue;
        const int first = kAreaFirstMap[area];
        if (mapNumber < first || mapNumber >= areaEnd(area)) continue;
        std::vector<uint8_t> f;
        if (!readFile(dataDir + kAreaEventFile[area], f)) continue;

        // Header section 2 (+0x08): table of u32 offsets, one per map of the area
        uint32_t sec2 = 0, off = 0;
        if (!rd(f, 8, sec2) || !rd(f, sec2 + 4 * size_t(mapNumber - first), off) || off == 0 || off >= f.size())
            continue;
        size_t p = off;
        for (;;) {
            uint16_t tag = 0;
            if (!rd(f, p, tag)) break;
            if (tag == 0xFFFF) break;
            p += kStageEntrySize;
        }
        const size_t rec = p + kStageEntrySize;
        int16_t w = 0, h = 0, radius = -1;
        if (!rd(f, rec + 40, w) || !rd(f, rec + 42, h)) continue;
        rd(f, rec + 68, radius);
        for (int k = 0; k < 4; ++k) {
            int16_t v = 0;
            rd(f, rec + 44 + 2 * size_t(k), v);
            out.rect[k] = v;
        }
        int16_t look = 0;
        rd(f, rec + 70, look);
        out.lookHeight = look;
        std::memcpy(out.eye, f.data() + rec, 12);
        std::memcpy(out.target, f.data() + rec + 12, 12);
        std::memcpy(&out.fovDegrees, f.data() + rec + 24, 4);
        out.viewWidth = w;
        out.viewHeight = h;
        out.hitRadius = radius;
        out.eventFile = kAreaEventFile[area];
        out.area = area;
        out.mapNumber = mapNumber;
        // Reject records past the end of an area's table (e.g. j001_d05 map 90: FOV 0, junk floats)
        bool ok = out.fovDegrees > 1.0f && out.fovDegrees < 179.0f && w > 0 && h > 0;
        float d2 = 0;
        for (int i = 0; i < 3; ++i) {
            ok = ok && std::isfinite(out.eye[i]) && std::isfinite(out.target[i]) && std::fabs(out.eye[i]) < 1e5f &&
                 std::fabs(out.target[i]) < 1e5f;
            d2 += (out.eye[i] - out.target[i]) * (out.eye[i] - out.target[i]);
        }
        out.scripted = d2 <= 1e-6f;
        if (ok) return true;
    }
    if (error) *error = "no event file covers map " + std::to_string(mapNumber);
    return false;
}

int areaForMap(int mapNumber, int currentArea) {
    auto covers = [&](int a) { return mapNumber >= kAreaFirstMap[a] && mapNumber < areaEnd(a); };
    if (currentArea >= 0 && currentArea < kNumAreas && covers(currentArea)) return currentArea;
    for (int i = 0; i < kNumAreas; ++i)
        if (covers(areaSearchOrder(i))) return areaSearchOrder(i);
    return -1;
}

bool EventFile::load(const std::string& dataDir, int area, std::string* error) {
    data_.clear();
    area_ = -1;
    if (area < 0 || area >= kNumAreas) {
        if (error) *error = "bad area " + std::to_string(area);
        return false;
    }
    std::vector<uint8_t> f;
    if (!readFile(dataDir + kAreaEventFile[area], f) || f.size() < 0x30) {
        if (error) *error = std::string("cannot read ") + kAreaEventFile[area];
        return false;
    }
    for (int i = 0; i < 7; ++i) rd(f, size_t(i) * 4, sec_[i]);
    for (uint32_t o : sec_)
        if (o >= f.size()) {
            if (error) *error = std::string(kAreaEventFile[area]) + ": bad section table";
            return false;
        }
    data_ = std::move(f);
    area_ = area;
    return true;
}

uint32_t EventFile::scriptEntry(int n) const {
    // Section 0: u32 offset per script; the last entry is a sentinel pointing at itself
    uint32_t off = 0;
    if (n < 0 || sec_[0] + 4 * size_t(n) + 4 > sec_[1] || !rd(data_, sec_[0] + 4 * size_t(n), off)) return 0;
    return off < data_.size() && off != sec_[1] - 4 ? off : 0;
}

uint32_t EventFile::returnEntry(int n) const {
    // Up to 9 offsets; code follows them (and precedes section 0). The table ends where the first
    // script after it starts: j001_t99's boot script begins at slot 5, so its slots 5-8 are code
    size_t end = sec_[1] + 36;
    for (int k = 0; k < 9; ++k) {
        uint32_t v = 0;
        if (rd(data_, sec_[1] + 4 * size_t(k), v) && v > sec_[1] && v < end) end = v;
    }
    uint32_t off = 0;
    const size_t slot = sec_[1] + 4 * size_t(n);
    if (n < 0 || n >= 9 || slot + 4 > end || slot + 4 > sec_[2] || !rd(data_, slot, off)) return 0;
    return off >= 0x30 && off < data_.size() && (off < sec_[0] || off >= end) ? off : 0;
}

uint32_t EventFile::sceneOffset(int n) const {
    uint32_t first = 0, off = 0;
    if (n < 1 || !rd(data_, sec_[6], first) || first <= sec_[6] || sec_[6] + 4 * size_t(n) > first ||
        !rd(data_, sec_[6] + 4 * size_t(n - 1), off))
        return 0;
    return off + 8 <= data_.size() ? off : 0;
}

std::vector<FieldNpc> EventFile::npcs(int mapNumber) const {
    std::vector<FieldNpc> out;
    if (!loaded()) return out;
    // Section 2 holds one u32 per map of the area, followed by the lists themselves: indexes past
    // the first list are junk (j001_d05 map 90, j001_t91 maps beyond its rooms)
    const int index = mapNumber - kAreaFirstMap[area_];
    uint32_t list = 0, first = UINT32_MAX;
    for (size_t p = sec_[2]; p < first && p + 4 <= sec_[3]; p += 4) {
        uint32_t v = 0;
        rd(data_, p, v);
        if (v > sec_[2] && v < first) first = v;
    }
    if (index < 0 || sec_[2] + 4 * size_t(index) >= first) return out;
    if (!rd(data_, sec_[2] + 4 * size_t(index), list) || list == 0 || list >= data_.size()) return out;
    for (size_t p = list; p + kStageEntrySize <= data_.size(); p += kStageEntrySize) {
        uint16_t id = 0;
        rd(data_, p, id);
        if (id == 0xFFFF) break;
        FieldNpc n;
        n.id = id;
        std::memcpy(n.pos, &data_[p + 4], 12);
        std::memcpy(&n.angle, &data_[p + 20], 4);
        std::memcpy(n.box, &data_[p + 28], 12);
        std::memcpy(n.offset, &data_[p + 40], 12);
        NpcRow r;
        if (npcRow(id, r)) {
            n.flagShow = r.flagShow;
            n.flagHide = r.flagHide;
            n.model = r.model;
            n.kind = r.kind;
            n.treasure = r.box;
            n.puzzle = r.puzzle;
            std::memcpy(n.link, r.link, sizeof n.link);
            n.scale = r.scale;
        }
        out.push_back(n);
    }
    return out;
}

bool EventFile::npcRow(int id, NpcRow& out) const {
    // Section 3: 40-byte rows of s16 fields, indexed by NPC id
    const size_t row = sec_[3] + 40 * size_t(id);
    int16_t show = -1, hide = -1, model = -1, scale = -1, item = -1;
    uint16_t kind = 0, box = 0, puzzle = 0;
    if (id < 0 || !loaded() || row + 40 > sec_[4] || !rd(data_, row, show) || !rd(data_, row + 2, hide) ||
        !rd(data_, row + 4, model) || !rd(data_, row + 14, kind) || !rd(data_, row + 18, box) ||
        !rd(data_, row + 20, puzzle) ||
        !rd(data_, row + 38, scale) || !rd(data_, row + 12, item))
        return false;
    out.item = item;
    out.flagShow = show;
    out.flagHide = hide;
    out.model = model;
    out.kind = kind;
    out.box = box;
    out.puzzle = puzzle;
    for (int k = 0; k < 4; ++k) rd(data_, row + 22 + 2 * size_t(k), out.link[k]);
    out.scale = scale < 0 ? 1.0f : float(scale) / 100.0f; // npc_setup: +38 / 100, -1 = 1.0
    return true;
}

namespace {

// Decodes the message at `msg` (UTF-16LE) into pages of UTF-8 lines. Control codes (PrcMsg's
// jump table): 1/2/3/0x15/0x16/0x1a/0x1b/0x1c/0x1e take one parameter (2 = insert a variable's
// value, 3 = insert a string - the menus' item name is string 9 -, 0x1e = voice clip); 0x11 breaks the line, 0x10 clears the window, 0x12 waits for a tap (new page),
// 0/0x13/0x14 end; other codes below 0x20 only set display state.
bool decodeMessage(const std::vector<uint8_t>& data, size_t msg, const std::function<int(int)>& var,
                   std::vector<std::vector<std::string>>& pages, MessageInfo* info,
                   const std::function<std::string(int)>& text = nullptr) {
    pages.clear();
    if (info) *info = MessageInfo();
    std::vector<std::string> lines{std::string()};
    int lastPause = -1; // a 0x1a with nothing typed after it yet
    auto typedSoFar = [&] { // the page's characters so far (UTF-8 lead bytes, not the \x01 marks)
        int n = 0;
        for (const std::string& l : lines)
            for (unsigned char ch : l) n += (ch & 0xC0) != 0x80 && ch != 0x01;
        return n;
    };
    auto newPage = [&] {
        while (!lines.empty() && lines.back().empty()) lines.pop_back();
        if (!lines.empty()) pages.push_back(lines);
        lines.assign(1, std::string());
    };
    for (size_t q = msg; q + 2 <= data.size(); q += 2) {
        uint16_t u = 0;
        rd(data, q, u);
        if (u == 0 || u == 0x13 || u == 0x14) {
            if (info && u == 0x14) info->closeAfter = std::max(0, lastPause);
            break;
        }
        if (u == 0x1a) { // a timed pause in the typing
            uint16_t v = 0;
            rd(data, q + 2, v);
            if (info) info->pauses.push_back({int(pages.size()), typedSoFar(), int(v)});
            lastPause = v;
            q += 2;
            continue;
        }
        if (u >= 0x20 || u == 0x01 || u == 0x02 || u == 0x03) lastPause = -1;
        if (u == 0x02) { // variable insert
            uint16_t v = 0;
            rd(data, q + 2, v);
            lines.back() += std::to_string(var ? var(v) : 0);
            q += 2;
            continue;
        }
        if (u == 0x01 || u == 0x03) { // string insert (PrcMsg treats 1 and 3 alike: the string var v names)
            uint16_t v = 0;
            rd(data, q + 2, v);
            if (text) lines.back() += text(v);
            q += 2;
            continue;
        }
        if (u == 0x15 || u == 0x1c) { // speaker name plate / speaking side
            uint16_t v = 0;
            rd(data, q + 2, v);
            if (info && u == 0x15) info->nameId = 58028 + v;
            if (info && u == 0x1c) info->side = v;
            q += 2;
            continue;
        }
        if (u == 0x1e) { // voice clip, played from this page on
            uint16_t v = 0;
            rd(data, q + 2, v);
            if (info && v != 0xFFFF) info->voices.push_back({int(pages.size()), v});
            q += 2;
            continue;
        }
        if (u == 0x16) { // the text's colour (PrcMsg: 0 white, 1 black; other values left alone)
            uint16_t v = 0;
            rd(data, q + 2, v);
            if (info && v <= 1) info->colour = v;
            q += 2;
            continue;
        }
        if (u == 0x16 || u == 0x1a || u == 0x1b || u == 0x1c) { q += 2; continue; }
        if (u == 0x10) { // PrcMsg: the window cleared (Msg_allclar), typing again from its first line -
            // after a tap (0x12) or a pause (0x1a) that's the next page; at a page's start, nothing
            if (lines.size() > 1 || !lines[0].empty()) {
                if (info && lastPause >= 0) info->turns.push_back({int(pages.size()), lastPause});
                newPage();
            }
            continue;
        }
        if (u == 0x11) { lines.emplace_back(); continue; } // the next line
        if (u == 0x17) { // PrcMsg: this line's alignment flips (+0x10e; CmdFont centres it at tex2d 240) -
            // marked by a leading \x01 for the message window
            if (lines.back().empty() || lines.back()[0] != '\x01') lines.back().insert(0, "\x01");
            continue;
        }
        if (u == 0x12) { newPage(); continue; }
        if (u < 0x20) continue;
        uint32_t c = u;
        if (u >= 0xD800 && u < 0xDC00) { // surrogate pair
            uint16_t lo = 0;
            if (rd(data, q + 2, lo) && lo >= 0xDC00 && lo < 0xE000) {
                c = 0x10000 + (uint32_t(u - 0xD800) << 10) + (lo - 0xDC00);
                q += 2;
            }
        }
        appendUtf8(lines.back(), c);
    }
    newPage();
    return true; // found; a message can be empty (`1a 01 14`: a timed pause with no text)
}

// Two-level table: u32 block offsets (ending where the first block starts), each block a table
// of `perBlock` u32 message offsets; all relative to the file start.
bool lookupMessage(const std::vector<uint8_t>& data, uint32_t table, int m, int perBlock, uint32_t& msg) {
    uint32_t first = 0, block = 0;
    if (m < 0 || !rd(data, table, first)) return false;
    const size_t slot = table + 4 * size_t(m / perBlock);
    return slot < first && rd(data, slot, block) && rd(data, block + 4 * size_t(m % perBlock), msg) && msg != 0 &&
           msg < data.size();
}

} // namespace

bool EventFile::message(int id, int language, const std::function<int(int)>& var,
                        std::vector<std::vector<std::string>>& pages, MessageInfo* info) const {
    pages.clear();
    uint32_t table = 0, msg = 0;
    // +0x28: one text section per language (0 Japanese, 1 English, 2 French, 3 German, 4 Spanish,
    // 5 / 6 Chinese simplified / traditional, 7 Korean); 1.0.1 has only the first two
    if (!loaded() || id < 1000 || id >= 51000 || language < 0 || language >= kLanguages ||
        !rd(data_, 0x28 + 4 * size_t(language), table))
        return false;
    if (table == 0 && language != 1) return message(id, 1, var, pages, info);
    if (!lookupMessage(data_, table, id - 1000, 300, msg)) return false;
    return decodeMessage(data_, msg, var, pages, info);
}

const char* const kLanguageName[kLanguages] = {"Japanese", "English", "French", "German", "Spanish",
                                               "Chinese (simplified)", "Chinese (traditional)", "Korean"};

bool SystemText::hasLanguage(int language) const {
    uint32_t t = 0;
    return language >= 0 && language < kLanguages && rd(data_, 4 * size_t(language), t) && t != 0 && t < data_.size();
}

bool SystemText::load(const std::string& dataDir) { return readFile(dataDir + "sys.evt", data_) && data_.size() > 8; }

bool SystemText::message(int id, int language, const std::function<int(int)>& var,
                         std::vector<std::vector<std::string>>& pages, MessageInfo* info,
                         const std::function<std::string(int)>& text) const {
    pages.clear();
    uint32_t table = 0, msg = 0;
    if (data_.empty() || id < 51000 || language < 0 || language >= kLanguages || !rd(data_, 4 * size_t(language), table))
        return false;
    if (table == 0 && language != 1) return message(id, 1, var, pages, info, text);
    if (!lookupMessage(data_, table, id - 51000, 1000, msg)) return false;
    return decodeMessage(data_, msg, var, pages, info, text);
}

std::vector<WarpEntry> EventFile::warps() const {
    std::vector<WarpEntry> out;
    if (!loaded()) return out;
    for (size_t p = sec_[4]; p + 20 <= sec_[5]; p += 20) {
        WarpEntry e;
        int16_t k = -1, a = -1, m = -1, ang = 0;
        std::memcpy(e.pos, &data_[p], 12);
        rd(data_, p + 12, k); rd(data_, p + 14, a); rd(data_, p + 16, m); rd(data_, p + 18, ang);
        if (k == -1) break;
        e.key = k; e.area = a; e.map = m; e.angleDegrees = ang;
        out.push_back(e);
    }
    return out;
}

bool EventFile::warpEntry(int key, WarpEntry& out) const {
    if (!loaded()) return false;
    auto entryAt = [&](size_t i, WarpEntry& e) {
        const size_t p = sec_[4] + 20 * i;
        if (p + 20 > sec_[5]) return false;
        int16_t k = -1, a = -1, m = -1, ang = 0;
        std::memcpy(e.pos, &data_[p], 12);
        rd(data_, p + 12, k); rd(data_, p + 14, a); rd(data_, p + 16, m); rd(data_, p + 18, ang);
        e.key = k; e.area = a; e.map = m; e.angleDegrees = ang;
        return true;
    };
    // EvtWarp: entry 0 if its key is 0xFFFF or matches, else the first later match or the terminator
    WarpEntry e;
    if (!entryAt(0, e)) return false;
    if (e.key == -1 || e.key == int16_t(key)) { out = e; return e.map >= 0; }
    for (size_t i = 1; entryAt(i, e); ++i)
        if (e.key == -1 || e.key == int16_t(key)) { out = e; return e.key != -1 && e.map >= 0; }
    return false;
}

} // namespace cr
