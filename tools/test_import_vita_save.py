#!/usr/bin/env python3
"""Synthetic regression tests; no proprietary save files are included."""
import base64
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock
import zlib

spec = importlib.util.spec_from_file_location("vita", Path(__file__).with_name("import_vita_save.py"))
vita = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vita)


def encoded(doc, compressed=True):
    plain = json.dumps(doc, separators=(",", ":")).encode()
    key = b"mediavision"
    out = bytearray()
    for byte in plain:
        for nibble in (byte >> 4, byte & 15):
            out.append(((nibble ^ key[len(out) % len(key)]) - 1) & 255)
    body = bytes(out)
    if compressed:
        body = zlib.compress(body)
        body = b"^q7@#=0TmLx3MC5$" + struct.pack("<I", len(body)) + body
    return struct.pack("<I", len(body)) + body


class ImportTests(unittest.TestCase):
    def test_decode_cr3_compressed_and_plain(self):
        doc = {"name": "A name with spaces", "flags": [1, 1000], "text": "日本語"}
        for compressed in (True, False):
            self.assertEqual(vita.cr3_json(encoded(doc, compressed)), doc)

    def test_cr3_trailing_bytes_are_outside_payload(self):
        self.assertEqual(vita.cr3_json(encoded({"ok": True}) + b"old data"), {"ok": True})

    def test_cr3_truncation_and_bad_nibbles_rejected(self):
        with self.assertRaises(vita.InvalidSave):
            vita.cr3_json(encoded({"ok": True})[:-1])
        with self.assertRaises(vita.InvalidSave):
            vita.cr3_json(struct.pack("<I", 2) + b"aa")

    def test_cr3_nested_play_payload(self):
        doc = {"flags": [1, 2]}
        text = base64.b64encode(zlib.compress(json.dumps(doc).encode()))
        key = b"mediavision"
        out = bytearray()
        for byte in text:
            for nibble in (byte >> 4, byte & 15):
                out.append(((nibble ^ key[len(out) % len(key)]) - 1) & 255)
        self.assertEqual(vita.cr3_json(struct.pack("<I", len(out)) + out), doc)

    def test_bad_crc_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            p = Path(folder) / "save.bin"
            d = bytearray(0x910)
            struct.pack_into("<H", d, 2, 0x3f6)
            struct.pack_into("<H", d, 0, vita.crc(d))
            p.write_bytes(d)
            self.assertEqual(vita.binary(p, len(d)), d)
            d[100] ^= 1
            p.write_bytes(d)
            with self.assertRaises(vita.InvalidSave):
                vita.binary(p, len(d))

    def test_existing_saves_need_replace_and_get_backups(self):
        with tempfile.TemporaryDirectory() as folder:
            p = Path(folder)
            (p / "save1.txt").write_text("old")
            with self.assertRaises(vita.InvalidSave):
                vita.install({"save1.txt": "new"}, p)
            self.assertEqual((p / "save1.txt").read_text(), "old")
            backup = vita.install({"save1.txt": "new"}, p, True)
            self.assertEqual((backup / "save1.txt").read_text(), "old")
            self.assertEqual((p / "save1.txt").read_text(), "new")

    def test_failed_replacement_restores_previous_saves(self):
        with tempfile.TemporaryDirectory() as folder:
            p = Path(folder)
            (p / "save1.txt").write_text("old1")
            (p / "save2.txt").write_text("old2")
            original = Path.replace
            def fail_second(source, target):
                if source.name == "save2.txt":
                    raise OSError("simulated full disk")
                return original(source, target)
            with mock.patch.object(Path, "replace", fail_second), self.assertRaises(OSError):
                vita.install({"save1.txt": "new1", "save2.txt": "new2"}, p, True)
            self.assertEqual((p / "save1.txt").read_text(), "old1")
            self.assertEqual((p / "save2.txt").read_text(), "old2")

    def test_permanent_bonuses_are_not_double_counted(self):
        data = bytearray(0xc24)
        struct.pack_into("<H", data, 0x320 + 0x48, 100)
        struct.pack_into("<i", data, 0x320 + 0xc, 32590)
        for o in (0x16, 0x1c, 0x20, 0x24, 0x28):
            struct.pack_into("<H", data, 0x320 + o, 3279)
        values = list(map(int, vita.records(data, 0x320, 1, False)[0].split()[1:]))
        self.assertEqual(values[1], 100)
        self.assertEqual(values[-6:], [190] + [39] * 5)

    def test_bags_have_different_encodings(self):
        self.assertEqual(vita.bag(struct.pack("<H", 42 | (5 << 9)), 0, 2), {42: 5})
        self.assertEqual(vita.bag(struct.pack("<HH", 500, 7), 0, 4, True), {500: 7})

    def test_play_time_keeps_completed_hours(self):
        data = bytearray(12)
        struct.pack_into("<ii", data, 4, 36, 195165)
        self.assertEqual(vita.play_seconds(data), 36 * 3600 + 195.165)

    def test_gene_uid_is_not_original_overlapping_stock_id(self):
        gene = {"_id": 0, "masterId": 483, "commands": [-1] * 16, "add": [1, 2, 3, 4]}
        a, b = vita.gene_values(gene, 1, False), vita.gene_values(gene, 2, True)
        self.assertEqual(a[-1], 1)
        self.assertEqual(b[-1], 2)
        self.assertNotEqual(a[3], b[3])

    def test_omega_encounter_switch_and_genes_are_scenario_local(self):
        data = bytearray(0x1558)
        for c in range(10):
            struct.pack_into("<H", data, 0x4a4 + c * 0x4c + 0x48, 1)
        data[0x1546:0x154a] = bytes([1, 1, 1, 0])
        data[0x154a:0x1550] = bytes([0, 1, 0, 2, 0, 0])
        f = 0x1004  # scenario 0 knows ability 85; scenario 1 does not
        data[0x24 + f // 8] |= 1 << (f % 8)
        result = vita.omega_saves(data, 0)
        self.assertIn("\nnoencounters 1 0 0 0\n", result["save1.dat"])
        self.assertIn("\nscenario_genes 1\n", result["save1.dat"])
        self.assertIn("items 85=1\n", result["save1.dat"])
        self.assertIn("otherbag 85=1\n", result["save2.dat"])

    def test_cr3_field_mapping_equipment_and_present_gene(self):
        gene = {"_id": 0, "masterId": 483, "exp": 77, "commands": [-1] * 16,
                "add": [1, 2, 3, 4], "tag": "-MNA-VLE"}
        doc = {"flags": [5], "savedata": {"main_parties": [1, 2, 3, 4, 5], "field": {
            "nDungeonNum": 9, "nAreaNum": 1, "nDungeonLevel": 1,
            "vPosition": [1, 2, 3], "qRotation": [0, 0, 0, 1],
            "bChangeCharacterEnable": True, "bTabMenuEnable": True, "bEncountEnable": False}},
            "profile": {"teamLevel": 139}, "items": [{"masterId": 200048, "count": 1, "tag": "-MNA"}],
            "genes": [gene], "geneStocks": [gene], "characters": [], "gold": 123,
            "coin": 2, "GP": 3, "geneStock": 250,
            "presents": [{"masterId": 483, "category": 1, "gene": gene}]}
        text = vita.cr3_save(doc)
        self.assertIn("\narea d0502\n", text)
        self.assertIn("\ncr3armour 1 48\n", text)
        self.assertIn("\ncr3geneof 1 1\n", text)
        self.assertNotIn("\ncr3geneof 2 ", text)  # a VS tag must not equip the main party
        self.assertIn("\ncr3geneuid 2\n", text)
        present = next(s for s in text.splitlines() if s.startswith("cr3present ")).split()
        self.assertEqual(present[5:7], ["483", "77"])


if __name__ == "__main__":
    unittest.main()
