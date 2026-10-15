"""Walk an MCM_ block generically: sections are `u16 count` + `count` size-prefixed records.
usage: mcm_walk.py <file.bin>   (prints each section's records: class, id, user, size)"""
import struct, sys
data = open(sys.argv[1], 'rb').read()
base = struct.unpack_from('<I', data, 0)[0] if data[:4] != b'MCM_' else 0
model_end = base + struct.unpack_from('<I', data, 4)[0] if data[:4] != b'MCM_' else len(data)
pos = base + 12
sec = 0
while pos + 2 <= model_end:
    n = struct.unpack_from('<H', data, pos)[0]; pos += 2
    recs = []
    for _ in range(n):
        size, cls, oid, user = struct.unpack_from('<IIii', data, pos)
        if size < 16 or pos + size > model_end:
            print(f'section {sec}: bad record at {pos:#x} (size {size:#x}) - stopping'); sys.exit()
        recs.append((cls, oid, user, size)); pos += size
    classes = sorted({c for c, _, _, _ in recs})
    print(f'section {sec:2} @{pos:#8x}: {n:4} records  classes={classes}  ids={[r[1] for r in recs][:12]}{"..." if n > 12 else ""}  sizes={[hex(r[3]) for r in recs][:6]}')
    sec += 1
print(f'end at {pos:#x}, model block ends {model_end:#x}')
