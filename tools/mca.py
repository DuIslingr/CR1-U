"""MCA_ animation container reader (reversed from 0xa7ec4 / 0xa8094).

File:   'MCA_' u32 version, u8 option, u8[3], u16 nActions, then nActions records.
Action: 16-byte record header, u32 ?, u8 compressed, u32 decodedSize, f32 duration,
        u8 ?, u8 ?, u16 nSegments, then (HI-LZW) segment data:
  segment: u16 nTracks; per track:
     u8 keyType, u8 kind, u16 nKeys, u16 a, u16 b
     kind <= 9  : constant-size keys  {u8 flags, u8 n, f32 time, f32[kind * n]}
     kind >= 10 : packed keys         {u8 flags, u8 fmt, f32 time, values}
                  fmt == 0 -> f32[comps]; else component i is 0/1/2/4 bytes per (fmt >> 2i) & 3
                  comps = COMPS[kind] (table at 0xcb6a4)
"""
import struct
import sys

import hilzw

# components per key for packed kinds (getKeyTypeCompoNum(kind, 1))
COMPS = {k: (k - 11) % 4 + 1 for k in range(11, 31)}
VALUE_SIZE = (0, 1, 2, 4)


class R:
    def __init__(self, d, p=0):
        self.d, self.p = d, p

    def take(self, fmt):
        v = struct.unpack_from("<" + fmt, self.d, self.p)
        self.p += struct.calcsize("<" + fmt)
        return v if len(v) > 1 else v[0]


def parse_action(d, pos):
    r = R(d, pos)
    size, cls, oid, user = r.take("IIii")
    end = pos + size
    unk0, compressed, dsize, duration, b0, b1, nseg = r.take("IBIfBBH")
    if compressed:
        data, _ = hilzw.decode(d, r.p)
        s = R(data)
    else:
        data = d[r.p:end]
        s = R(data)
    segments = []
    for _ in range(nseg):
        ntracks = s.take("H")
        tracks = []
        for _ in range(ntracks):
            key_type, kind, nkeys, a, b = s.take("BBHHH")
            keys = []
            for _ in range(nkeys):
                if kind <= 9:
                    flags, n, t = s.take("BBf")
                    vals = s.take("%df" % (kind * n)) if kind * n else ()
                    vals = vals if isinstance(vals, tuple) else (vals,)
                    keys.append((t, flags, n, vals))
                else:
                    flags, fmt, t = s.take("BBf")
                    comps = COMPS.get(kind, 0)
                    if fmt == 0:
                        vals = s.take("%df" % comps) if comps else ()
                        vals = vals if isinstance(vals, tuple) else (vals,)
                    else:
                        vals = []
                        for i in range(comps):
                            sz = VALUE_SIZE[(fmt >> (2 * i)) & 3]
                            vals.append(int.from_bytes(s.d[s.p:s.p + sz], "little", signed=True) if sz else None)
                            s.p += sz
                    keys.append((t, flags, fmt, tuple(vals)))
            tracks.append({"type": key_type, "kind": kind, "a": a, "b": b, "keys": keys})
        segments.append(tracks)
    return {"id": oid, "duration": duration, "b0": b0, "b1": b1, "segments": segments,
            "consumed": s.p, "stream": len(data), "end": end}


def parse_mca(d, base=0):
    assert d[base:base + 4] == b"MCA_", "not MCA"
    r = R(d, base + 4)
    version, option = r.take("IB")
    r.p += 3
    n = r.take("H")
    actions = []
    for _ in range(n):
        a = parse_action(d, r.p)
        actions.append(a)
        r.p = a["end"]
    return actions


if __name__ == "__main__":
    for path in sys.argv[1:]:
        acts = parse_mca(open(path, "rb").read())
        for i, a in enumerate(acts):
            nt = sum(len(s) for s in a["segments"])
            print(f"{path}: action {i} dur={a['duration']} segs={len(a['segments'])} tracks={nt} "
                  f"consumed {a['consumed']}/{a['stream']}")
