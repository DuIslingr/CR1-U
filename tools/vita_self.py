#!/usr/bin/env python3
"""Unwraps a decrypted (fake-signed) PS Vita SELF - eboot.bin as Vita3K / NoNpDrm keep it - into a
plain ARM ELF that Ghidra's ELF loader takes, and lists its imports and exports from the module
info (libraries by name, functions by NID).

Usage: vita_self.py eboot.bin out.elf [labels.tsv] [nids.tsv]

out.elf: the SELF's own ELF (ET_SCE_RELEXEC 0xfe04 linked at its vaddrs) with e_type set to
ET_EXEC and the SCE-only program headers (relocations 0x60000000, build info 0x6fffff01) made
PT_NULL, so the loader maps the two load segments where they were linked.
labels.tsv: address, name, kind (import / export / module), library, NID. Names come from
nids.tsv (NID <tab> name, e.g. converted from vitasdk's db.yml) when given, else <library>_<NID>.
"""
import struct
import sys


def unwrap(self_bytes):
    magic, version, sdk_type, header_type, meta_off, header_len, elf_size = struct.unpack_from('<4sIHHIQQ', self_bytes, 0)
    if magic != b'SCE\0':
        raise SystemExit('not an SCE SELF')
    elf_off, phdr_off, _, secinfo_off = struct.unpack_from('<QQQQ', self_bytes, 0x40)
    ehdr = bytearray(self_bytes[elf_off:elf_off + 52])
    if ehdr[:4] != b'\x7fELF':
        raise SystemExit('no ELF header inside')
    e_type, = struct.unpack_from('<H', ehdr, 16)
    e_entry, e_phoff = struct.unpack_from('<II', ehdr, 24)
    e_phnum, = struct.unpack_from('<H', ehdr, 44)
    phdrs = [list(struct.unpack_from('<8I', self_bytes, phdr_off + 32 * i)) for i in range(e_phnum)]
    out = bytearray(max(p[1] + p[4] for p in phdrs) if phdrs else 0)
    out = bytearray(max(len(out), e_phoff + 32 * e_phnum))
    segs = []
    for i, p in enumerate(phdrs):
        off, length, compression, encryption = struct.unpack_from('<QQQQ', self_bytes, secinfo_off + 32 * i)
        if encryption == 1:
            raise SystemExit(f'segment {i} is encrypted: needs decrypting first')
        if compression == 2:
            raise SystemExit(f'segment {i} is compressed (not handled)')
        data = self_bytes[off:off + length]
        out[p[1]:p[1] + len(data)] = data
        segs.append((p, data))
    struct.pack_into('<H', ehdr, 16, 2)  # ET_EXEC
    out[0:52] = ehdr
    for i, p in enumerate(phdrs):
        q = list(p)
        if q[0] != 1:  # PT_LOAD stays, the SCE-only ones go
            q[0] = 0
        struct.pack_into('<8I', out, e_phoff + 32 * i, *q)
    return bytes(out), e_type, e_entry, segs


def module_info(segs, e_entry):
    """The module info (sceModuleInfo) at segment e_entry >> 30, offset e_entry & 0x3fffffff."""
    seg_i, off = e_entry >> 30, e_entry & 0x3fffffff
    p, data = segs[seg_i]
    base = p[2]
    attr, ver = struct.unpack_from('<HH', data, off)
    name = data[off + 4:off + 31].split(b'\0')[0].decode('ascii', 'replace')
    gp, exp_top, exp_end, imp_top, imp_end, mod_nid = struct.unpack_from('<6I', data, off + 32)
    return base, name, off, exp_top, exp_end, imp_top, imp_end


class Image:
    def __init__(self, segs):
        self.segs = [(p[2], p[2] + len(d), d) for p, d in segs if p[0] == 1]

    def u32(self, va):
        for lo, hi, d in self.segs:
            if lo <= va < hi:
                return struct.unpack_from('<I', d, va - lo)[0]
        raise KeyError(hex(va))

    def cstr(self, va):
        for lo, hi, d in self.segs:
            if lo <= va < hi:
                o = va - lo
                return d[o:d.index(b'\0', o)].decode('ascii', 'replace')
        return '?'


def main():
    raw = open(sys.argv[1], 'rb').read()
    elf, e_type, e_entry, segs = unwrap(raw)
    open(sys.argv[2], 'wb').write(elf)
    print(f'ELF: {len(elf)} bytes, original e_type {e_type:#x}, {len(segs)} segments')
    for p, d in segs:
        print(f'  type {p[0]:#010x} vaddr {p[2]:#x} filesz {p[4]:#x} memsz {p[5]:#x} flags {p[6]:#x}')
    if len(sys.argv) < 4:
        return
    nids = {}
    if len(sys.argv) > 4:
        for line in open(sys.argv[4], encoding='utf-8'):
            k, _, v = line.rstrip('\n').partition('\t')
            if v:
                nids[int(k, 16)] = v
    img = Image(segs)
    base, name, mi_off, exp_top, exp_end, imp_top, imp_end = module_info(segs, e_entry)
    rows = [(base + mi_off, 'module_info', 'module', name, '')]
    # imports: entries of 0x24 or 0x34 bytes (size in the first u16)
    va = base + imp_top
    nfuncs = 0
    libs = []
    while va < base + imp_end:
        size = img.u32(va) & 0xffff
        if size == 0x34:
            nf = img.u32(va + 4) >> 16
            nv = img.u32(va + 8) & 0xffff
            lib_nid, lib_name = img.u32(va + 0x10), img.u32(va + 0x14)
            fnid, fent, vnid, vent = (img.u32(va + o) for o in (0x1c, 0x20, 0x24, 0x28))
        elif size == 0x24:
            nf = img.u32(va + 4) >> 16
            nv = img.u32(va + 8) & 0xffff
            lib_nid, lib_name = img.u32(va + 0x0c), img.u32(va + 0x10)
            fnid, fent, vnid, vent = (img.u32(va + o) for o in (0x14, 0x18, 0x1c, 0x20))
        else:
            raise SystemExit(f'import entry of size {size:#x} at {va:#x}')
        lib = img.cstr(lib_name)
        libs.append((lib, nf, nv))
        for i in range(nf):
            nid, stub = img.u32(fnid + 4 * i), img.u32(fent + 4 * i)
            rows.append((stub, nids.get(nid, f'{lib}_{nid:08X}'), 'import', lib, f'{nid:08X}'))
            nfuncs += 1
        for i in range(nv):
            nid, at = img.u32(vnid + 4 * i), img.u32(vent + 4 * i)
            rows.append((at, nids.get(nid, f'{lib}_var_{nid:08X}'), 'import-var', lib, f'{nid:08X}'))
        va += size
    # exports: 0x20 bytes each - attr u16 (size u8), version, n funcs, n vars, n tls, ..., nid tables
    va = base + exp_top
    while va < base + exp_end:
        size = img.u32(va) & 0xff
        nf, nv = img.u32(va + 4) >> 16, img.u32(va + 8) & 0xffff
        lib_nid, lib_name, nid_tab, ent_tab = (img.u32(va + o) for o in (0x10, 0x14, 0x18, 0x1c))
        lib = img.cstr(lib_name) if lib_name else 'syslib'
        for i in range(nf + nv):
            nid, at = img.u32(nid_tab + 4 * i), img.u32(ent_tab + 4 * i)
            known = {0x935CD196: 'module_start', 0x79F8E492: 'module_stop', 0x913482A9: 'module_exit',
                     0x6C2224BA: 'module_info', 0x70FBA1E7: 'module_proc_param', 0x8F2C5CDD: 'sce_process_param'}
            rows.append((at, nids.get(nid, known.get(nid, f'{lib}_export_{nid:08X}')),
                         'export' if i < nf else 'export-var', lib, f'{nid:08X}'))
        va += size or 0x20
    with open(sys.argv[3], 'w', encoding='utf-8') as out:
        out.write('address\tname\tkind\tlibrary\tnid\n')
        for at, n, kind, lib, nid in rows:
            out.write(f'{at:08x}\t{n}\t{kind}\t{lib}\t{nid}\n')
    print(f'module "{name}": {len(libs)} libraries imported, {nfuncs} functions; '
          f'{sum(1 for r in rows if r[2].startswith("export"))} exports -> {sys.argv[3]}')
    for lib, nf, nv in libs:
        print(f'  {lib}: {nf} functions' + (f', {nv} variables' if nv else ''))


if __name__ == '__main__':
    main()
