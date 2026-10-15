"""Reconstruct the field-descriptor tables built by the MCx loader's record readers.

Each reader fills an array of 20-byte descriptors on the stack and calls the
generic field reader (0xa3750) with r2 pointing at it:
    +0  dest     where the value is stored (stack slot or heap object field)
    +4  type*    pointer into the type-id table at 0xce2e8 (id = ptr - 0xce2e8)
    +8  count*   pointer to an element count (constant table at 0xce2f8 or a field read earlier)
    +12 alloc    nonzero = allocate dest array of `count` elements
    +16 sub      nested record reader (type 14 only)
A descriptor with type* == 0 terminates the table.

This tool symbolically executes the straight-line code of a function (taking the
fall-through path at branches, which unrolls each loop once) and prints every
descriptor table at each call to 0xa3750.

usage: descx.py <disasm.txt> <macho.py-compatible binary> <func_start_hex> [func_end_hex]
"""
import re
import struct
import sys

TYPE_BASE = 0xce2e8
TYPE_NAMES = {0: "f32", 1: "fx16.16", 2: "u32", 3: "u16", 4: "u8", 5: "u16", 6: "u32",
              7: "rgba8", 8: "rgb8", 9: "u8->int", 10: "u8->f32", 11: "u16->u32", 12: "u16->u32",
              13: "vec4?", 14: "records"}
FIELD_READER = 0xa3750


def load_binary(path):
    b = open(path, "rb").read()
    ncmds = struct.unpack_from("<I", b, 16)[0]
    off, segs = 28, []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", b, off)
        if cmd == 1:
            vm, vsz, fo, fsz = struct.unpack_from("<IIII", b, off + 24)
            segs.append((vm, fo, fsz))
        off += size

    def rd32(addr):
        for vm, fo, fsz in segs:
            if vm <= addr < vm + fsz:
                return struct.unpack_from("<I", b, fo + addr - vm)[0]
        return None
    return rd32


class Sym:
    """A symbolic value: ('const', n) | ('sp', off) | ('reg', name, off) | None (unknown)."""


def parse_imm(s):
    s = s.strip().lstrip("#")
    return int(s, 0)


def main():
    disasm, binpath, start = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
    end = int(sys.argv[4], 16) if len(sys.argv) > 4 else None
    rd32 = load_binary(binpath)

    lines = []
    pat = re.compile(r"^\s+([0-9a-f]+):\s+[0-9a-f]{8}\s+\t(\S+)\t?(.*)$")
    for line in open(disasm):
        m = pat.match(line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        if addr < start:
            continue
        if end is not None and addr >= end:
            break
        if end is None and addr > start and re.match(r"push", m.group(2)) and lines and \
                re.match(r"pop", lines[-1][1]):
            break
        lines.append((addr, m.group(2), m.group(3)))

    regs = {"sp": ("sp", 0)}
    mem = {}  # stack offset -> value

    def val(op):
        op = op.strip()
        if op.startswith("#"):
            return ("const", parse_imm(op))
        return regs.get(op)

    def add(v, k):
        if v is None:
            return None
        if v[0] == "const":
            return ("const", (v[1] + k) & 0xffffffff)
        return (v[0], v[1] + k)

    def show(v):
        if v is None:
            return "?"
        if v[0] == "const":
            return hex(v[1])
        if v[0] == "sp":
            return f"sp+{v[1]:#x}"
        return f"{v[0]}+{v[1]:#x}"

    def dump_table(base):
        print(f"  table @ sp+{base:#x}")
        i = 0
        while True:
            off = base + 20 * i
            dest, typ, cnt, alloc, sub = (mem.get(off + 4 * k) for k in range(5))
            if typ is None or (typ[0] == "const" and typ[1] == 0):
                break
            t = typ[1] - TYPE_BASE if typ[0] == "const" else None
            tname = TYPE_NAMES.get(t, show(typ)) if t is not None and 0 <= t <= 14 else show(typ)
            if cnt and cnt[0] == "const" and TYPE_BASE <= cnt[1] < TYPE_BASE + 0x40:
                c = str(rd32(cnt[1]))
            else:
                c = show(cnt)
            extra = f" alloc={show(alloc)}" if alloc and alloc != ("const", 0) else ""
            extra += f" sub={show(sub)}" if sub and sub != ("const", 0) else ""
            print(f"    [{i:2}] {tname:9} x{c:<10} -> {show(dest)}{extra}")
            i += 1
            if i > 64:
                break

    for addr, op, args in lines:
        a = [x.strip() for x in re.split(r",(?![^\[]*\])", args.split("@")[0])] if args else []
        cond = op[-2:] in ("eq", "ne", "lt", "gt", "le", "ge", "hi", "ls", "lo", "hs", "mi", "pl") \
            and op not in ("bne", "beq", "blt", "bgt", "ble", "bge", "bhi", "bls", "blo", "bhs")
        base = re.sub(r"(eq|ne|lt|gt|le|ge|hi|ls|lo|hs|mi|pl)$", "", op) if cond else op
        try:
            if base == "ldr" and "[pc" in args:
                lit = int(re.search(r"@ 0x([0-9a-f]+)", args).group(1), 16)
                regs[a[0]] = ("const", rd32(lit))
            elif base in ("mov", "movw") and len(a) == 2:
                v = val(a[1])
                regs[a[0]] = v
            elif base == "mvn" and len(a) == 2 and a[1].startswith("#"):
                regs[a[0]] = ("const", (~parse_imm(a[1])) & 0xffffffff)
            elif base == "movt":
                v = regs.get(a[0])
                regs[a[0]] = ("const", ((v[1] if v and v[0] == "const" else 0) & 0xffff) |
                              (parse_imm(a[1]) << 16))
            elif base == "add" and len(a) == 3 and a[2].startswith("#"):
                regs[a[0]] = add(val(a[1]), parse_imm(a[2]))
            elif base == "sub" and len(a) == 3 and a[2].startswith("#"):
                regs[a[0]] = add(val(a[1]), -parse_imm(a[2]))
            elif base == "add" and len(a) == 3:
                x, y = val(a[1]), val(a[2])
                if x and y and y[0] == "const":
                    regs[a[0]] = add(x, y[1])
                elif x and y and x[0] == "const":
                    regs[a[0]] = add(y, x[1])
                else:
                    regs[a[0]] = None
            elif base == "str" and len(a) == 2:
                m = re.match(r"\[(\w+)(?:,\s*#(-?0x[0-9a-f]+|-?\d+))?\]$", a[1])
                if m:
                    b = regs.get(m.group(1))
                    off = int(m.group(2), 0) if m.group(2) else 0
                    if b and b[0] == "sp":
                        mem[b[1] + off] = regs.get(a[0])
                else:
                    m = re.match(r"\[(\w+),\s*(\w+)\]$", a[1])
                    if m:
                        x, y = regs.get(m.group(1)), regs.get(m.group(2))
                        tgt = None
                        if x and y and x[0] == "sp" and y[0] == "const":
                            tgt = x[1] + y[1]
                        elif x and y and y[0] == "sp" and x[0] == "const":
                            tgt = y[1] + x[1]
                        if tgt is not None:
                            mem[tgt] = regs.get(a[0])
            elif base == "stm" or base == "stmib":
                pass
            elif base == "bl":
                target = int(re.search(r"0x([0-9a-f]+)", args).group(1), 16)
                if target == FIELD_READER and regs.get("r2") and regs["r2"][0] == "sp":
                    print(f"{addr:#x}: call field reader")
                    dump_table(regs["r2"][1])
                for r in ("r0", "r1", "r2", "r3", "r12", "lr"):
                    regs[r] = None
            elif base in ("ldr", "ldrb", "ldrh", "ldrsh", "lsl", "lsr", "orr", "and", "mul", "mla",
                          "rsb", "uxtb", "sxth", "bic", "asr", "eor"):
                if a:
                    regs[a[0]] = None
        except (ValueError, IndexError, AttributeError):
            if a:
                regs[a[0]] = None


if __name__ == "__main__":
    main()
