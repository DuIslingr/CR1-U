"""MCM_ model container reader (work in progress).

Reversed from __readModelData (0xa8a8c), __readVertexBuffer (0xa6008) and
__readAppearance (0xa69a0). Records are tightly packed little-endian; every
object record starts with a 16-byte header:
    u32 size (including this header), u32 class, u32 id, u32 user (-1 = none)
"""
import struct
import sys


class Reader:
    def __init__(self, data, pos=0):
        self.d = data
        self.pos = pos

    def u8(self):
        v = self.d[self.pos]
        self.pos += 1
        return v

    def u16(self):
        v = struct.unpack_from("<H", self.d, self.pos)[0]
        self.pos += 2
        return v

    def u32(self):
        v = struct.unpack_from("<I", self.d, self.pos)[0]
        self.pos += 4
        return v

    def i32(self):
        v = struct.unpack_from("<i", self.d, self.pos)[0]
        self.pos += 4
        return v

    def f32(self, n=1):
        v = struct.unpack_from("<%df" % n, self.d, self.pos)
        self.pos += 4 * n
        return v if n > 1 else v[0]


def record_header(r):
    start = r.pos
    size, cls, oid, user = r.u32(), r.u32(), r.i32(), r.i32()
    return {"start": start, "end": start + size, "size": size, "class": cls, "id": oid, "user": user}


# Vertex attribute data types (the u8 "A" field of each attribute slot)
VTYPE_SIZE = {0: 4, 1: 1, 2: 1, 3: 2}


def read_vertex_buffer(r):
    h = record_header(r)
    vb = {"hdr": h, "flags": r.u8(), "unk_u32": r.u32(), "attrs": []}
    slots = 5 + (1 if h["class"] > 0 else 0)
    for i in range(5):
        vb["attrs"].append({"count": r.u16(), "type": r.u8(), "comps": r.u8()})
    if h["class"] > 0:
        vb["opt_u8"] = r.u8()
    for i in range(3):  # 2 texcoord sets + 1 extra slot (see __readVertexBuffer loop)
        vb["attrs"].append({"count": r.u16(), "type": r.u8(), "comps": r.u8()})
    vb["data_start"] = r.pos
    r.pos = h["end"]
    return vb


def scan(data, base):
    """Walk the model block and print its section/record layout."""
    r = Reader(data, base)
    assert data[base:base + 4] == b"MCM_", "not an MCM block"
    r.pos = base + 12
    n_app = r.u16()
    print(f"appearances: {n_app}")
    for _ in range(n_app):
        h = record_header(r)
        print(f"  app  @{h['start']:#x} size={h['size']:#x} class={h['class']} id={h['id']} user={h['user']}")
        r.pos = h["end"]
    n_vb = r.u16()
    print(f"vertex buffers: {n_vb}")
    for _ in range(n_vb):
        vb = read_vertex_buffer(r)
        h = vb["hdr"]
        attrs = " ".join(f"[{a['count']}x{a['comps']}t{a['type']}]" for a in vb["attrs"])
        print(f"  vb   @{h['start']:#x} size={h['size']:#x} class={h['class']} id={h['id']} {attrs}")
    print(f"after vertex buffers @{r.pos:#x}: {data[r.pos:r.pos + 48].hex(' ')}")
    return r.pos


if __name__ == "__main__":
    path = sys.argv[1]
    data = open(path, "rb").read()
    off = struct.unpack_from("<I", data, 0)[0] if data[:4] != b"MCM_" else 0
    scan(data, off)
