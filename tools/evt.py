"""Event script (.evt) bytecode disassembler — structure only, never message text.

Reversed from PrcEvnt (0x7ab60, dispatch through EvtPrcTbl at 0xcfac0) and the
expression evaluator GetEval/SetEval (operator tables at 0xd3a20 / 0xd3ad0).
See docs/FORMATS.md ("Event scripts").

usage: evt.py <file.evt> [entry ...]     disassemble the given entries (default: all; @offset = code there)
       evt.py --check <file.evt>...      decode every reachable instruction, report errors
"""
import struct
import sys

# opcode -> (name, operand layout). Layout items: 'b' u8, 'h' u16, 'i' u32, 'a' u32 address,
# 'e' expression, 's' assignment (value expression + target), 'w' Wait operand
OPS = {
    0: ("Return", ""), 1: ("Gosub", "a"), 2: ("Abort", ""), 3: ("Open", "bbb"), 4: ("Open", "bbb"),
    5: ("Close", ""), 6: ("Close", ""), 7: ("Msg", "h"), 8: ("Msg", "h"), 9: ("Set", "s"),
    10: ("Goto", "a"), 11: ("Break", "a"), 12: ("Ifn", "ea"), 13: ("Rept", "ea"), 14: ("Wait", "w"),
    15: ("Call", "h"), 16: ("Screen", "hhbbb"), 17: ("Warp", "hhhhhh"), 18: ("Push", "e"),
    19: ("PushBP", ""), 20: ("PopBP", ""), 21: ("Util", "b"), 22: ("Bgm", "hh"), 23: ("Eff", "hh"),
    24: ("Voice", "hh"), 25: ("Tex2d", "T"), 26: ("Born", "h"), 27: ("Kill", "h"),
    28: ("Setc", "hhhhh"), 29: ("Anim", "hhhh"), 30: ("Autoadj", "hhhh"), 31: ("Mapscroll", "hhhh"),
    32: ("Battle", "hh"), 33: ("FrmSet", "h"), 34: ("FrmJmp", "h"), 35: ("FrmAdjc", "hhhh"),
    36: ("FrmSetc", "hh"), 37: ("FrmSetCam", "h"), 38: ("FrmAdjCam", "hh"), 39: ("FrmPanCam", "hhhh"),
    40: ("FrmFaceTo", "hh"), 41: ("CameraFree", ""), 42: ("CameraPlayer", ""), 43: ("Quake", "hhhh"),
    44: ("LoadAct", "hhh"), 45: ("UnloadAct", "hh"), 46: ("NpcFade", "hbbb"), 47: ("FrmPlayEff", "hhh"),
    48: ("FrmStopEff", "h"), 49: ("Anim2", "hhhhhh"), 50: ("FrmStopEff2", "hh"), 51: ("NpcMdlRead", "h"),
    52: ("FrmAdjc2", "hhhh"), 53: ("FrmSetc2", "hh"), 54: ("SetScale", "hhhh"), 55: ("AdjScale", "hhhhh"),
    56: ("FrmPlayEff2", "hhh"), 57: ("SetDir", "hh"), 58: ("Screen", "hhbbb"), 59: ("Warp2", "hhhhhh"),
    60: ("AAAA", "bbbbbbbbbbbbbbb"),
    # the Vita's (its j001_t99 boot: the start-up and its logos)
    63: ("Vita3f", ""), 64: ("Vita40", ""), 65: ("Vita41", ""), 67: ("Vita43", ""), 68: ("Vita44", ""),
    71: ("VitaLogoLoad", "hh"), 72: ("VitaLogoShow", "hhhh"), 73: ("Vita49", "hh"), 74: ("VitaLogoShow0", "hhhh"),
}
END = {0, 2, 10}  # no fall-through (Return, Abort, Goto)

# expression operators: name, operand kinds ('e' sub-expression, 'i' i32, 'a' u32 address, 'b' u8)
EXPR = {
    0: ("==", "ee"), 1: ("!=", "ee"), 2: (">", "ee"), 3: (">=", "ee"), 4: ("<", "ee"), 5: ("<=", "ee"),
    6: ("&&", "ee"), 7: ("||", "ee"), 8: ("^", "ee"), 9: ("!", "e"), 10: ("+", "ee"), 11: ("-", "ee"),
    12: ("*", "ee"), 13: ("/", "ee"), 14: ("%", "ee"), 16: ("im", "i"), 17: ("result", ""),
    18: ("rand", "e"), 19: ("var", "e"), 20: ("flag", "e"), 21: ("label", "a"), 22: ("arg", "b"),
    33: ("gold", ""), 34: ("name", ""), 38: ("item", "e"), 42: ("gds", ""),
}
TARGETS = {17, 19, 20, 22, 33, 34, 38, 42}  # valid assignment targets (SetEval table)


class Error(Exception):
    pass


class Evt:
    def __init__(self, data):
        self.d = data
        self.hdr = struct.unpack_from("<12I", data, 0)
        s0, s1 = self.hdr[0], self.hdr[1]
        self.entries = list(struct.unpack_from(f"<{max(0, s1 - s0) // 4}I", data, s0))
        # The last entry of table 0 is a sentinel pointing at itself. Table 1 (EvrStart) runs
        # from header[1] up to the first code byte; the code follows it.
        if self.entries and self.entries[-1] == s1 - 4: self.entries[-1] = 0
        s2 = self.hdr[2]
        n1 = max(0, s2 - s1) // 4
        t1 = list(struct.unpack_from(f"<{n1}I", data, s1))
        first_code = min([x for x in self.entries + t1[:1] if x > s1] or [s2])
        self.entries2 = t1[:max(0, first_code - s1) // 4]

    def u8(self, p):
        if p >= len(self.d): raise Error(f"read past end at {p:#x}")
        return self.d[p]

    def u16(self, p):
        if p + 2 > len(self.d): raise Error(f"read past end at {p:#x}")
        return struct.unpack_from("<H", self.d, p)[0]

    def u32(self, p):
        if p + 4 > len(self.d): raise Error(f"read past end at {p:#x}")
        return struct.unpack_from("<I", self.d, p)[0]

    def expr(self, p, assign=False):
        """Returns (text, next position)."""
        op = self.u8(p)
        p += 1
        if op not in EXPR: raise Error(f"bad expression op {op} at {p - 1:#x}")
        if assign and op not in TARGETS: raise Error(f"bad assignment target {op} at {p - 1:#x}")
        name, kinds = EXPR[op]
        args = []
        for k in kinds:
            if k == "e":
                t, p = self.expr(p)
                args.append(t)
            elif k == "i":
                args.append(str(struct.unpack("<i", struct.pack("<I", self.u32(p)))[0]))
                p += 4
            elif k == "a":
                args.append(f"@{self.u32(p):#x}")
                p += 4
            elif k == "b":
                args.append(str(self.u8(p)))
                p += 1
        if name == "im": return args[0], p
        # binary operators compute (second OP first) - print them that way
        if len(kinds) == 2 and kinds == "ee": return f"({args[1]} {name} {args[0]})", p
        return f"{name}({', '.join(args)})", p

    def insn(self, p):
        """Decodes one instruction: (name, operands, targets, next position)."""
        op = self.u8(p)
        start = p
        p += 1
        if op not in OPS: raise Error(f"bad opcode {op} at {start:#x}")
        name, layout = OPS[op]
        args, targets = [], []
        for k in layout:
            if k == "b":
                args.append(self.u8(p)); p += 1
            elif k == "h":
                args.append(self.u16(p)); p += 2
            elif k == "i":
                args.append(self.u32(p)); p += 4
            elif k == "a":
                t = self.u32(p); p += 4
                targets.append(t); args.append(f"@{t:#x}")
            elif k == "e":
                t, p = self.expr(p); args.append(t)
            elif k == "s":
                v, p = self.expr(p)
                t, p = self.expr(p, assign=True)
                args.append(f"{t} = {v}")
            elif k == "w":
                w = self.u16(p); p += 2
                args.append(w)
                if w == 0xFFFD:
                    args.append(self.u16(p)); p += 2
            elif k == "T":  # u16 slot, u16 flags, then optional blocks by flag bit
                slot, flags = self.u16(p), self.u16(p + 2)
                p += 4
                args += [slot, f"{flags:#06x}"]
                if flags & 1: args.append(("img", self.u16(p))); p += 2
                for bit in (4, 0x20, 8):
                    if flags & bit:
                        args.append((f"{bit:#x}", struct.unpack_from("<3h", self.d, p))); p += 6
        return name, args, targets, p

    def walk(self, entry):
        """All instructions reachable from `entry`: {pos: (name, args, targets, next)}."""
        seen, todo = {}, [entry]
        while todo:
            p = todo.pop()
            while p not in seen:
                ins = self.insn(p)
                seen[p] = ins
                todo.extend(ins[2])
                if self.u8(p) in END: break
                p = ins[3]
        return seen


def check(paths):
    total_ok = total_bad = 0
    from collections import Counter
    bad_ops = Counter()
    for path in paths:
        e = Evt(open(path, "rb").read())
        code = {}
        ok = bad = 0
        for i, ent in enumerate(e.entries):  # table 1 (EvrStart: menu/shop/battle callbacks) is short; not checked
            if ent == 0 or ent >= len(e.d): continue
            try:
                code.update(e.walk(ent))
                ok += 1
            except Error as ex:
                bad += 1
                msg = str(ex)
                bad_ops[msg.split(" at ")[0]] += 1
        # every jump target must be an instruction start
        starts = set(code)
        misaligned = sum(1 for ins in code.values() for t in ins[2] if t not in starts)
        print(f"{path.split('/')[-1]:16s} entries ok {ok:4d} bad {bad:3d}  insns {len(code):6d}  misaligned targets {misaligned}")
        total_ok += ok; total_bad += bad
    print(f"total entries ok {total_ok}, bad {total_bad}")
    for k, v in bad_ops.most_common(15): print(f"  {v:5d}  {k}")


def main():
    if sys.argv[1] == "--check":
        check(sys.argv[2:])
        return
    e = Evt(open(sys.argv[1], "rb").read())
    which = sys.argv[2:] or range(len(e.entries))
    for i in which:  # a script number, or @<file offset> (EvrStart code, a Gosub target)
        if isinstance(i, str) and i.startswith("@"):
            ent = int(i[1:], 0)
            print(f"code @ {ent:#x}")
        else:
            ent = e.entries[int(i)]
            if ent == 0: continue
            print(f"entry {i} @ {ent:#x}")
        try:
            code = e.walk(ent)
        except Error as ex:
            print("  error:", ex); continue
        for p in sorted(code):
            name, args, targets, _ = code[p]
            print(f"  {p:6x}: {name} {', '.join(map(str, args))}")


if __name__ == "__main__":
    main()
