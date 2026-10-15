"""Tiny Mach-O VM-address reader for the game binary: python3 -I macho.py <bin> <addr> <len> [u32]"""
import struct, sys
b = open(sys.argv[1], 'rb').read()
ncmds = struct.unpack_from('<I', b, 16)[0]; off = 28; segs = []
for _ in range(ncmds):
    cmd, size = struct.unpack_from('<II', b, off)
    if cmd == 1:
        vm, vsz, fo, fsz = struct.unpack_from('<IIII', b, off + 24); segs.append((vm, fo, fsz))
    off += size
def rd(addr, n):
    for vm, fo, fsz in segs:
        if vm <= addr < vm + fsz: return b[fo + addr - vm: fo + addr - vm + n]
    return b''
addr, n = int(sys.argv[2], 16), int(sys.argv[3], 0)
data = rd(addr, n)
if len(sys.argv) > 4: print([hex(x) for x in struct.unpack('<%dI' % (len(data) // 4), data)])
else: print(data)
