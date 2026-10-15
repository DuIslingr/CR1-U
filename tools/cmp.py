"""Field collision (.cmp) reader — reversed from Cmp::CmpBase::init (0x36c8),
Cmp::TreeKD::init (0x5fac) and Cmp::TreeQUAD::init (0x616c).

Layout (little endian, f32):
  0x00 u32 tree type   0 = KD tree (32-byte nodes), 1 = quad tree (40-byte nodes)
  0x04 s16 nVerts      0x06 s16 nPolys   0x08 s16 nNodes   0x0c f32 ?   (32-byte header)
  0x20 nVerts x vec3
       nPolys x 32-byte polygon
       nNodes x node (KD 32 bytes / QUAD 40 bytes)
       index lists: per node, sum(node s16 counts at +4..+10) s16 polygon indices
"""
import struct
import sys

NODE_SIZE = {0: 32, 1: 40}


def parse(data):
    tree, nv, np_, nn = struct.unpack_from("<Ihhh", data, 0)
    if tree not in NODE_SIZE:
        raise ValueError(f"unknown tree type {tree}")
    p = 32
    verts = [struct.unpack_from("<3f", data, p + 12 * i) for i in range(nv)]
    p += 12 * nv
    polys = [data[p + 32 * i:p + 32 * (i + 1)] for i in range(np_)]
    p += 32 * np_
    ns = NODE_SIZE[tree]
    nodes = [data[p + ns * i:p + ns * (i + 1)] for i in range(nn)]
    p += ns * nn
    lists = []
    for node in nodes:
        n = sum(struct.unpack_from("<4h", node, 4))
        lists.append(struct.unpack_from("<%dh" % max(n, 0), data, p) if n > 0 else ())
        p += 2 * max(n, 0)
    return {"tree": tree, "verts": verts, "polys": polys, "nodes": nodes, "lists": lists, "end": p,
            "header_f32": struct.unpack_from("<f", data, 12)[0]}


if __name__ == "__main__":
    for path in sys.argv[1:]:
        c = parse(open(path, "rb").read())
        size = len(open(path, "rb").read())
        print(f"{path}: tree={c['tree']} verts={len(c['verts'])} polys={len(c['polys'])} nodes={len(c['nodes'])} "
              f"consumed {c['end']}/{size}")
