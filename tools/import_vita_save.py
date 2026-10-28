#!/usr/bin/env python3
"""Convert decrypted Chaos Rings Vita saves to the ports' save formats.

Python 3.9+, standard library only. See docs/VITA_SAVES.md. Input files are
never modified. --output is explicit; existing files require --replace and
are backed up before replacement. The same tool is shipped in all four ports.
"""
import argparse
import base64
import binascii
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import shutil
import struct
import sys
import tempfile


TITLES = {"cr1": ("PCSG00497", 0), "omega": ("PCSG00499", 10),
          "cr2": ("PCSG00498", 20), "cr3": ("PCSG00457", 30)}
CR3_DUNGEONS = [f"t{i:02}" for i in range(1, 6)] + [f"d{i:02}" for i in range(1, 18)] + ["e00"]
CR3_CHARS = ("", "NA", "LE", "DA", "EL", "AL", "JO")
# _chk_geneplateno: scenario-local flags encode these character-specific entries.
OMEGA_GENES = ((85, 114, 116, 0, 135, 187, 189, 0),
               (86, 82, 117, 0, 136, 188, 190, 0),
               (119, 0, 0, 0, 0, 0, 0, 0),
               (85, 87, 83, 0, 135, 137, 131, 0),
               (86, 82, 84, 0, 136, 190, 132, 0),
               (114, 81, 116, 0, 187, 189, 133, 0),
               (82, 88, 117, 0, 188, 134, 138, 0))


class InvalidSave(ValueError):
    pass


def unpack(data, fmt, offset):
    return struct.unpack_from("<" + fmt, data, offset)


def integer(data, offset):
    return unpack(data, "i", offset)[0]


def crc(data):
    return (~binascii.crc_hqx(data[2:], 0xffff)) & 0xffff


def binary(path, size):
    data = path.read_bytes()
    if len(data) != size or unpack(data, "H", 2)[0] != 0x3f6:
        raise InvalidSave(f"{path.name}: wrong size or version")
    if unpack(data, "H", 0)[0] != crc(data):
        raise InvalidSave(f"{path.name}: checksum mismatch")
    return data


def line(key, values):
    return key + " " + " ".join(str(int(v)) if isinstance(v, bool) else str(v) for v in values)


def flags(data, offset, size):
    return [i for i in range(size * 8) if data[offset + i // 8] & (1 << (i % 8))]


def pairs_line(key, values):
    return line(key, [f"{k}={v}" for k, v in sorted(values.items()) if v])


def finite(values):
    if not all(math.isfinite(v) for v in values):
        raise InvalidSave("non-finite field position or rotation")
    return values


def play_seconds(data):
    # MuCom_GetPlayTimeDate: hours at +4, milliseconds within the hour at +8.
    return integer(data, 4) * 3600 + integer(data, 8) / 1000


def records(data, offset, count, omega):
    out = []
    for c in range(count):
        o = offset + 0x4c * c
        level = unpack(data, "H", o + 0x48)[0]
        if level == 0 and omega and c == 3:  # unused guest record
            level = 1
        if not 1 <= level <= (255 if omega else 100):
            raise InvalidSave(f"invalid character {c} level: {level}")
        n = max(0, level - 20)
        growth = [10 * n, n, n, n, n, n] if omega else [5 * n * (n + 1)] + [n * (n + 1) // 2] * 5
        total = [integer(data, o + 0xc)] + [unpack(data, "H", o + k)[0] for k in (0x16, 0x1c, 0x20, 0x24, 0x28)]
        # The original combines growth, seeds and skill points. Keep the exact
        # total as growth + boosts; do not add growth a second time on import.
        boosts = [a - b for a, b in zip(total, growth)]
        values = [c, level, integer(data, o), integer(data, o + 8), unpack(data, "H", o + 0x14)[0]]
        values += list(unpack(data, "3H", o + 0x2a)) + [0] * 4 + list(unpack(data, "4H", o + 0x30)) + boosts
        out.append(line("party", values))
    return out


def bag(data, offset, size, omega=False):
    result = Counter()
    if omega:
        for i in range(offset, offset + size, 4):
            item, count = unpack(data, "2H", i)
            if item and count:
                result[item] += count
    else:
        for word in unpack(data, f"{size // 2}H", offset):
            item, count = word & 0x1ff, word >> 9
            if item and count:
                result[item] += count
    if any(n > 99 for n in result.values()):
        raise InvalidSave("inventory count exceeds 99")
    return result


def cr1_saves(data, stamp):
    fs = flags(data, 0x20, 0x200)
    common = bag(data, 0x980, 0x140)
    bags = [bag(data, 0x580 + r * 0x100, 0x100) for r in range(4)]
    # Equipment for every character is accessible regardless of the active pair.
    gear = Counter({k: v for b in bags for k, v in b.items() if 201 <= k <= 424})
    base = [line("flags", [f"{v:08x}" for v in unpack(data, "128I", 0x20)]),
            pairs_line("vars", dict(enumerate(unpack(data, "64i", 0x220)))),
            line("oz", [integer(data, 0xc)])] + records(data, 0x320, 8, False)
    base += [line("walkexp", unpack(data, "4i", 0xac0)), line("noencounters", data[0xc1b:0xc1f]),
             line("bands", data[0xc10:0xc14])]
    for r in range(4):
        base.append(line("treasure", [r] + list(unpack(data, "30h", 0xb20 + r * 0x3c))))
    saves = {}
    active = data[0xc18]
    if active >= 4:
        raise InvalidSave("invalid active route")
    for r in range(4):
        if not data[0xc14 + r]:
            continue
        x, y, z, yaw = finite(unpack(data, "4f", 0xad0 + r * 0x14))
        area, map_id = unpack(data, "2H", 0xae0 + r * 0x14)
        items = common + Counter({k: v for k, v in bags[r].items() if not 201 <= k <= 424}) + gear
        for item in range(81, 89):
            if item + r * 16 + 0xf59 in fs:
                items[item] = 1
        for item in range(131, 139):
            if item + r * 16 + 0xf2f in fs:
                items[item] = 1
        rows = ["CRSAVE 1", line("area", [area]), line("map", [map_id]), line("pos", [x, y, z]),
                line("yaw", [math.radians(yaw)]), line("leader", [r * 2]),
                line("play", [play_seconds(data)]), line("saved", [stamp]), pairs_line("items", items)] + base
        saves[f"save{r + 1}.dat"] = "\n".join(rows) + "\n"
    if f"save{active + 1}.dat" not in saves:
        raise InvalidSave("active route has no saved position")
    saves["resume.dat"] = saves[f"save{active + 1}.dat"]
    return saves


def omega_saves(data, stamp):
    fs = set(flags(data, 0x24, 0x280))
    common = bag(data, 0x119c, 0x300, True)
    bags = [bag(data, 0x79c + s * 0x500, 0x500, True) for s in range(2)]
    # Character-specific genes live in flags, not the inventory arrays.
    for sc, rows in enumerate((OMEGA_GENES[:3], OMEGA_GENES[3:])):
        for r, row in enumerate(rows):
            for k, item in enumerate(row):
                if item and (0x1004 if sc == 0 else 0x1024) + r * 8 + k in fs:
                    bags[sc][item] = 1
    active = data[0x1550]
    if active >= 2:
        raise InvalidSave("invalid active scenario")
    money = list(unpack(data, "2i", 0xc))
    party = records(data, 0x4a4, 10, True)
    # The active records are authoritative; copy them back into that scenario's block.
    for k in range(3):
        party[4 + 3 * active + k] = party[k].replace(f"party {k} ", f"party {4 + 3 * active + k} ", 1)
    base = ["wornbag 1", "scenario_genes 1", line("flags", [f"{v:08x}" for v in unpack(data, "160I", 0x24)]),
            pairs_line("vars", dict(enumerate(unpack(data, "128i", 0x2a4)))),
            line("walkexp", list(unpack(data, "2i", 0x149c)) + [0, 0]),
            line("noencounters", list(data[0x1548:0x154a]) + [0, 0])]
    for sc in range(2):
        base.append(line("treasure", [sc] + list(unpack(data, "30h", 0x14cc + sc * 0x3c))))
    saves = {}
    for sc in range(2):
        if not data[0x1546 + sc]:
            continue
        x, y, z, yaw = finite(unpack(data, "4f", 0x14a4 + sc * 0x14))
        area, map_id = unpack(data, "2H", 0x14b4 + sc * 0x14)
        mine = common + bags[sc]
        block = list(party)
        for k in range(3):
            block[k] = block[4 + sc * 3 + k].replace(f"party {4 + sc * 3 + k} ", f"party {k} ", 1)
        # _prty's pair and leader arrays immediately precede the active scenario.
        scenario = [sc] + list(unpack(data, "4b", 0x154a)) + list(data[0x154e:0x1550]) + [money[1 - sc]] + list(data[0x1544:0x1546])
        rows = ["CRSAVE 1", line("area", [area]), line("map", [map_id]), line("pos", [x, y, z]),
                line("yaw", [math.radians(yaw)]), line("leader", [data[0x154e + sc]]),
                line("play", [play_seconds(data)]), line("saved", [stamp]),
                line("scenario", scenario), line("oz", [money[sc]]), pairs_line("items", mine),
                pairs_line("otherbag", bags[1 - sc])] + base + block
        saves[f"save{sc + 1}.dat"] = "\n".join(rows) + "\n"
    if f"save{active + 1}.dat" not in saves:
        raise InvalidSave("active scenario has no saved position")
    saves["resume.dat"] = saves[f"save{active + 1}.dat"]
    return saves


def cr2_save(data, system, stamp):
    fs = flags(data, 0x630, 500)
    fs = [f for f in fs if f > 1000] + flags(system, 4, 0x7c)
    dungeon, area, active = unpack(data, "3i", 0x5f0)
    if not 0 <= dungeon <= 99 or not 0 <= area < 99:
        raise InvalidSave("invalid CR2 map")
    map_name = ("t01" if dungeon == 0 else f"d{dungeon:02}") + f"{area + 1:02}"
    x, y, z = finite(unpack(data, "3f", 0x600))
    q = finite(unpack(data, "4f", 0x610))
    rows = ["CR2-U save 1", line("flags", sorted(fs)), line("party", unpack(data, "2b", 0x37)),
            line("active", [active, data[0x622], data[0x623], data[0x620]])]
    for i in range(3):
        rows.append(line("pairs", [i] + list(unpack(data, "2b", 0x39 + 2 * i))))
    for i, n in enumerate(unpack(data, "300h", 0x40)):
        if i and n:
            rows.append(line("item", [i, n]))
    rows.append(line("oz", [integer(data, 0x298)]))
    for i in range(9):
        o = 0x340 + i * 0x4c
        mp, most = unpack(data, "2h", o + 0x10)
        rows.append(line("chara", [i, integer(data, o), integer(data, o + 4), -1 if mp >= most else mp] +
                         list(unpack(data, "8h", o + 0x3c)) + list(unpack(data, "5h", o + 0x1e))))
    for i in range(32):
        rows.append(line("sopia", [i, data[0x2a0 + i], integer(data, 0x2c0 + i * 4)]))
    box = [integer(data, 0x8a4)]
    for offset, count in ((0x8a8, 10), (0x8bc, 3), (0x8c2, 3)):
        box += [int(data[offset + 2 * i] != 0) + 2 * int(data[offset + 2 * i + 1] != 0) for i in range(count)] + [0] * (10 - count)
    rows += [line("owned", unpack(data, "I", 0x29c)), line("difficulty", [data[0x34]]),
             line("boxes", box), line("global", unpack(data, "32i", 0x824)), line("map", [map_name]),
             line("place", [x, y, z, 2 * math.atan2(q[1], q[3])]), line("saved", [stamp]),
             line("playtime", unpack(data, "I", 0x10)), line("options", unpack(system, "5i", 0x80))]
    return "\n".join(rows) + "\n"


def inflate(data):
    # Bounded inflation also makes malformed input fail before allocating a huge buffer.
    import zlib
    decoder = zlib.decompressobj()
    try:
        out = decoder.decompress(data, 16 * 1024 * 1024)
    except zlib.error as err:
        raise InvalidSave("invalid compressed save") from err
    if not decoder.eof or decoder.unconsumed_tail or decoder.unused_data:
        raise InvalidSave("invalid or oversized compressed save")
    return out


def cr3_json(data):
    if len(data) < 4:
        raise InvalidSave("truncated CR3 save")
    size = unpack(data, "I", 0)[0]
    if not 0 < size <= len(data) - 4:
        raise InvalidSave("invalid CR3 payload length")
    encoded = data[4:4 + size]  # some exports have stale bytes beyond the declared payload
    if encoded.startswith(b"^q7@#=0TmLx3MC5$"):
        if len(encoded) < 20 or unpack(encoded, "I", 16)[0] != len(encoded) - 20:
            raise InvalidSave("invalid CR3 compressed length")
        encoded = inflate(encoded[20:])
    if len(encoded) % 2:
        raise InvalidSave("odd encoded CR3 payload length")
    decoded = bytearray()
    key = b"mediavision"
    for i in range(0, len(encoded), 2):
        high, low = [((encoded[j] + 1) & 0xff) ^ key[j % len(key)] for j in (i, i + 1)]
        if high > 15 or low > 15:
            raise InvalidSave("invalid CR3 encoded nibble")
        decoded.append(high * 16 + low)
    if not decoded.startswith(b"{"):
        try:
            decoded = inflate(base64.b64decode(decoded, validate=True))
        except binascii.Error as err:
            raise InvalidSave("invalid CR3 base64 payload") from err
    result = json.loads(decoded)
    if not isinstance(result, dict):
        raise InvalidSave("CR3 save is not an object")
    return result


def gene_values(g, uid, stocked):
    commands = g.get("commands", [-1] * 16)
    adds = g.get("add", [0] * 4)
    if len(commands) != 16 or len(adds) != 4:
        raise InvalidSave("invalid CR3 gene slots")
    return [g["masterId"], g.get("exp", 0), bool(g.get("locking", 0)), stocked, False] + commands + adds + [uid]


def cr3_save(doc):
    sd, profile = doc["savedata"], doc["profile"]
    field = sd["field"]
    dungeon, area = field["nDungeonNum"], field["nAreaNum"]
    if not 0 <= dungeon < len(CR3_DUNGEONS) or not 0 <= area < 99:
        raise InvalidSave("invalid CR3 area")
    area_name = CR3_DUNGEONS[dungeon] + f"{area + 1:02}"
    pos, quat = finite(field["vPosition"]), finite(field["qRotation"])
    if len(pos) != 3 or len(quat) != 4:
        raise InvalidSave("invalid CR3 field coordinates")
    party = sd["main_parties"]
    if len(party) != 5:
        raise InvalidSave("invalid CR3 party")
    rows = ["CR3-U save 1", line("flags", sorted(set(doc["flags"]))), line("cr3party", party),
            line("active", [field.get("nActivePC", 0), field["bChangeCharacterEnable"], field["bTabMenuEnable"], field["bEncountEnable"]]),
            line("select", [dungeon, field["nDungeonLevel"]]), line("area", [area_name]),
            line("place", pos + [2 * math.atan2(quat[1], quat[3])]),
            line("global", [doc.get("workFlags", {}).get(str(i), 0) for i in range(100)]),
            line("saved", [int(doc.get("clientTime", 0) // 1000)]),
            line("playtime", [int(profile.get("playtime", 0) * 20)]),
            line("cr3team", [profile["teamLevel"], profile.get("value", 0)])]
    for name, key in (("name", "mainChar"), ("team", "teamName"), ("pet", "petName"), ("food", "foodName")):
        value = profile.get(key, "")
        if any(c in value for c in "\n\r\0"):
            raise InvalidSave("invalid character in player name")
        rows.append(line("cr3name", [name, value]))
    counts = Counter()
    for item in doc["items"]:
        master = item["masterId"]
        counts[master] += item["count"]
        cat, local_id = divmod(master, 100000)
        tag = item.get("tag", "")
        if cat in (1, 2, 3):
            for ch, short in enumerate(CR3_CHARS[1:], 1):
                if "-M" + short in tag:
                    rows.append(line({1: "cr3weapon", 2: "cr3armour", 3: "cr3accessory"}[cat], [ch, local_id]))
        if item.get("enforceRemain", 0) > 0:
            rows.append(line("cr3attach", [master, item.get("enforceItem", 0), item["enforceRemain"]]))
    rows += [line("cr3item", [k, n]) for k, n in sorted(counts.items()) if n]
    uid = 0
    for stocked, group in ((False, doc["genes"]), (True, doc.get("geneStocks", []))):
        for g in group:
            uid += 1  # original active and stock identifiers overlap; give every gene a unique uid
            rows.append(line("cr3gene", gene_values(g, uid, stocked)))
            if not stocked:
                for ch, short in enumerate(CR3_CHARS[1:], 1):
                    if "-M" + short in g.get("tag", ""):
                        rows.append(line("cr3geneof", [ch, uid]))
    rows.append(line("cr3geneuid", [uid]))
    rows.append(line("cr3book", doc.get("geneCollections", [])))
    rows.append(line("cr3seen", sorted(set(doc.get("geneCollections", [])) - set(doc.get("geneCollectionMarks", [])))))
    rows.append(line("cr3money", [doc["gold"], doc["coin"], doc.get("usedCoin", 0), doc["GP"], doc.get("usedGP", 0), doc["geneStock"]]))
    rows.append(line("cr3days", [0, doc.get("vipDays", 0), doc.get("vipTrial", 0), doc.get("lastTime", 0), doc.get("cheer", 0)]))
    for sheet in doc.get("itemsheets", []):
        won = sheet["items"]
        rows.append(line("cr3sheet", [sheet["_id"], sheet.get("full", 0), False, True, len(won)] +
                         [v for n in won for v in (n, n > 0)]))
    for ch in doc["characters"]:
        rows.append(line("cr3mp", [ch["chara_id"], ch["MP"]]))
    for p in doc.get("presents", []):
        is_gene = p.get("category", 0) == 1
        vals = [p.get("masterId", 0), p.get("count", 1), p.get("from", 0), is_gene]
        if is_gene:
            vals += gene_values(p.get("gene", p), 0, False)
        rows.append(line("cr3present", vals))
    quest = doc.get("quest", {})
    rows += [line("cr3begun", quest.get("accepted", [])), line("cr3ended", quest.get("completed", [])),
             line("cr3offered", [i for i in quest.get("orders", []) if i]),
             line("cr3quests", [0, profile.get("questCount", 0), -100])]
    vs = doc.get("vs", {})
    mission, season = vs.get("mission", {}), vs.get("season", {})
    rows.append(line("cr3vs", [vs.get("score", 0), vs.get("totalScore", 0), vs.get("rewardsTotalScore", 0), vs.get("resetTime", 0),
                    mission.get("_id", 0), mission.get("score", 0), mission.get("totalScore", 0), mission.get("goal", 0),
                    mission.get("level", 0), mission.get("type", 0), mission.get("reward", 0), mission.get("earned", 0),
                    mission.get("geneLevel", 1), mission.get("teamLevel", 1), mission.get("geneLevelMax", 1), mission.get("teamLevelMax", 1),
                    mission.get("count", 0), season.get("_id", -1), season.get("totalScore", 0), season.get("count", 0), False,
                    vs.get("state", 0) != 0, bool(vs.get("earned", 0)), vs.get("tournament", 0),
                    doc.get("vs_prev_normal_tournament", -1), doc.get("vs_prev_special_tournament", -1)]))
    rows.append(line("cr3vsboosts", [v for x in vs.get("items", []) for v in (x["masterId"], x["count"])]))
    rows.append(line("cr3vscleared", vs.get("completed", [])))
    r = sd.get("records", {})
    rows.append(line("cr3records", [r.get(k, 0) for k in ("playtime", "killCount", "maxDamage", "vsTournamentCount", "acquiredGeneCount", "mergedGeneCount", "mergedGiftCount")]))
    for kind in ("tboxes", "gboxes"):
        for box in doc.get(kind, []):
            name = box["place"].removeprefix("ragdoll_")
            rows.append(line("cr3box", [box["area_no"], name, box.get("box_type", 0), int(kind == "gboxes"),
                            box.get("item_type", -1), box.get("item_no", -1), box["is_open"]]))
    opt = sd.get("option", {})
    rows.append(line("options", [opt.get("language", 0), opt.get("graphLevel", 1)] +
                     [int(opt.get(k, 100)) for k in ("volumeBGM", "volumeSE", "volumeVoice")]))
    return "\n".join(rows) + "\n"


def resolve(source, game):
    title, _ = TITLES[game]
    root = source if source.is_dir() else source.parent
    if (root / title).is_dir():
        root /= title
    return root


def convert(source, game):
    root = resolve(source, game)
    title, slot = TITLES[game]
    outputs, notes, used = {}, [], []
    def read(n, size=None):
        p = root / f"SAVEDATA{slot + n:04}.bin"
        used.append(p)
        return binary(p, size) if size is not None else p.read_bytes()
    if game == "cr3":
        read(0)  # include original global settings in the source manifest
        for i in range(1, 4):
            p = root / f"SAVEDATA{slot + i:04}.bin"
            if not p.exists():
                continue
            doc = cr3_json(read(i))
            outputs[f"save{i}.txt"] = cr3_save(doc)
            # Preserve original-only records (online metadata, achievements, alternate VS
            # loadouts, etc.) separately; the source export is never changed.
            outputs[f"vita-slot{i}.json"] = json.dumps(doc, ensure_ascii=False, indent=2) + "\n"
    else:
        if game == "cr2":
            p = root / f"SAVEDATA{slot:04}.bin"
            system = binary(p, p.stat().st_size)
            if len(system) not in (0x98, 0xb8):
                raise InvalidSave("invalid CR2 system file size")
            used.append(p)
            outputs["system.txt"] = "CR2-U system 1\n" + line("flags", flags(system, 4, 0x7c)) + "\n" + line("options", unpack(system, "5i", 0x80)) + "\n"
            make = lambda d, stamp: {"save1.txt": cr2_save(d, system, stamp)}
            size = 0x910
        else:
            make = cr1_saves if game == "cr1" else omega_saves
            size = 0xc24 if game == "cr1" else 0x1558
        p = root / f"SAVEDATA{slot + 1:04}.bin"
        outputs.update(make(read(1, size), int(p.stat().st_mtime)))
        # A stale continue file must never take priority over a newly imported manual save.
        if game == "cr2":
            outputs["cont.txt"] = outputs["save1.txt"]
        suspend = root / f"SAVEDATA{slot + 2:04}.bin"
        if suspend.exists():
            try:
                d = binary(suspend, size)
                used.append(suspend)
                resume = make(d, int(suspend.stat().st_mtime))
                outputs["cont.txt" if game == "cr2" else "resume.dat"] = resume["save1.txt" if game == "cr2" else "resume.dat"]
            except InvalidSave as err:
                notes.append(f"Skipped suspend save: {err}. Using the valid manual save for resume.")
    if not any(k.startswith("save") for k in outputs):
        raise InvalidSave("no gameplay slots found")
    manifest = {"game": game, "title": title, "notes": notes,
                "sources": {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in used},
                "outputs": sorted(outputs)}
    outputs["vita-import.json"] = json.dumps(manifest, indent=2) + "\n"
    return outputs, notes


def install(outputs, destination, replace=False):
    destination = destination.resolve()
    existing = [name for name in outputs if (destination / name).exists()]
    if existing and not replace:
        raise InvalidSave("destination already contains " + ", ".join(existing) + "; use a new folder or --replace (backs up existing files)")
    destination.mkdir(parents=True, exist_ok=True)
    backup = None
    if existing:
        backup = Path(tempfile.mkdtemp(prefix="vita-backup-" + datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-"), dir=destination))
        for name in existing:
            shutil.copy2(destination / name, backup / name)
    with tempfile.TemporaryDirectory(prefix=".vita-import-", dir=destination) as stage:
        for name, text in outputs.items():
            (Path(stage) / name).write_text(text, encoding="utf-8")
        written = []
        try:
            for name in outputs:
                (Path(stage) / name).replace(destination / name)
                written.append(name)
        except OSError:
            for name in written:
                if name in existing:
                    shutil.copy2(backup / name, destination / name)
                else:
                    (destination / name).unlink()
            raise
    return backup


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="export folder containing PCSG folders, or one game's PCSG folder")
    parser.add_argument("--game", choices=TITLES, required=True)
    parser.add_argument("--output", type=Path, required=True, help="destination profile or a new staging folder")
    parser.add_argument("--replace", action="store_true", help="replace destination files after making a backup")
    parser.add_argument("--dry-run", action="store_true", help="validate and list outputs without writing anything")
    args = parser.parse_args()
    try:
        outputs, notes = convert(args.source, args.game)
        for note in notes:
            print(note, file=sys.stderr)
        if not args.dry_run:
            source = resolve(args.source, args.game).resolve()
            dest = args.output.resolve()
            if dest == source or source in dest.parents:
                raise InvalidSave("choose an output folder outside the source export")
            backup = install(outputs, args.output, args.replace)
            if backup:
                print("Previous saves backed up to", backup)
        print(("Validated" if args.dry_run else "Imported") + f" {args.game}: " + ", ".join(outputs))
        print("Destination:", args.output.resolve())
    except (OSError, ValueError, KeyError, struct.error, binascii.Error) as err:
        parser.exit(1, f"Import failed: {err}\n")


if __name__ == "__main__":
    main()
