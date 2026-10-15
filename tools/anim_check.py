"""Numerically validate skeleton + animation + skinning conventions.

Skins a model with one of its animations and measures how much triangle edges
stretch compared with the bind pose. Correct conventions keep edges nearly
rigid; wrong rotation units/orders tear the mesh. Prints a table over variants.

usage: anim_check.py <model.bin> [anim.mca]   (default: first animation inside the package)
"""
import math
import struct
import sys

import numpy as np

import hilzw
import mca


def sections(d, base):
    pos = base + 12
    for sec in range(12):
        n = struct.unpack_from("<H", d, pos)[0]
        pos += 2
        for _ in range(n):
            size = struct.unpack_from("<I", d, pos)[0]
            yield sec, pos, size
            pos += size


def parse_tree(d, start):
    p = start + 16
    _ref, n = struct.unpack_from("<IH", d, p)
    p += 6
    parents, rest = [], []
    for _ in range(n):
        parent = struct.unpack_from("<H", d, p)[0]
        p += 2
        f = struct.unpack_from("<15f", d, p)
        p += 60 + 1 + 28
        X, Y, Z, T, S = (np.array(f[i:i + 3]) for i in range(0, 15, 3))
        parents.append(-1 if parent == 0xFFFF else parent)
        rest.append((X, Y, Z, T, S))
    return parents, rest


def local_matrix(X, Y, Z, T, S):
    m = np.eye(4)
    m[:3, 0], m[:3, 1], m[:3, 2], m[:3, 3] = X * S[0], Y * S[1], Z * S[2], T
    return m


def euler_xyz(a):  # R = Rz * Ry * Rx (mceTransform_setRotateEulerXYZ)
    cx, sx, cy, sy, cz, sz = math.cos(a[0]), math.sin(a[0]), math.cos(a[1]), math.sin(a[1]), math.cos(a[2]), math.sin(a[2])
    Rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
    Ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    Rz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
    return Rz @ Ry @ Rx


def eval_track(track, t, comps):
    keys = track["keys"]
    if t <= keys[0][0]:
        return np.array(keys[0][3][:comps])
    for k0, k1 in zip(keys, keys[1:]):
        if k0[0] <= t <= k1[0]:
            mode = k0[1]
            v0, v1 = np.array(k0[3][:comps]), np.array(k1[3][:comps])
            if mode == 3 or k1[0] == k0[0]:
                return v0
            s = (t - k0[0]) / (k1[0] - k0[0])
            if mode == 1:
                return v0 + s * (v1 - v0)
            out0 = np.array(k0[3][2 * comps:3 * comps]) if len(k0[3]) >= 3 * comps else np.zeros(comps)
            in1 = np.array(k1[3][comps:2 * comps]) if len(k1[3]) >= 2 * comps else np.zeros(comps)
            s2, s3 = s * s, s * s * s
            return (2 * s3 - 3 * s2 + 1) * v0 + (s3 - 2 * s2 + s) * out0 + (-2 * s3 + 3 * s2) * v1 + (s3 - s2) * in1
    return np.array(keys[-1][3][:comps])


def worlds(parents, locals_):
    n = len(parents)
    out = [None] * n

    def get(i):
        if out[i] is None:
            out[i] = locals_[i] if parents[i] < 0 else get(parents[i]) @ locals_[i]
        return out[i]
    for i in range(n):
        get(i)
    return out


def main():
    d = open(sys.argv[1], "rb").read()
    base = struct.unpack_from("<I", d, 0)[0]
    tree = vb = skin = ib = None
    for sec, s, size in sections(d, base):
        if sec == 7 and tree is None:
            tree = parse_tree(d, s)
        if sec == 1 and vb is None:
            import mcm
            vbd = mcm.read_vertex_buffer(mcm.Reader(d, s))
            raw = hilzw.decode(d, vbd["data_start"] + 32)[0] if vbd["flags"] == 1 else d[vbd["data_start"] + 32:]
            npos = vbd["attrs"][0]["count"]
            vb = np.frombuffer(raw, "<f4", npos * 3).reshape(-1, 3).astype(float)
        if sec == 2 and ib is None:
            r = s + 16
            comp = d[r]; ptype = d[r + 5]
            nprim, nidx = struct.unpack_from("<HH", d, r + 9)
            raw = hilzw.decode(d, r + 21)[0] if comp == 1 else d[r + 21:s + size]
            lens = np.frombuffer(raw, "<i2", nprim)
            idx = np.frombuffer(raw, "<u2", nidx, nprim * 2)
            ib = (lens, idx)
        if sec == 8 and skin is None:
            r = s + 16
            comp = d[r]
            nposs, nb = struct.unpack_from("<HH", d, r + 5)
            raw = hilzw.decode(d, r + 9)[0] if comp == 1 else d[r + 9:s + size]
            bones = struct.unpack_from("<%dH" % nb, raw, 0)
            q = 2 * nb
            verts = []
            for _ in range(nposs):
                k, dup = raw[q], raw[q + 1]
                w = struct.unpack_from("<%df" % k, raw, q + 2)
                b = raw[q + 2 + 4 * k:q + 2 + 5 * k]
                q += 2 + 5 * k
                verts.append((dup, w, tuple(b)))
            skin = (bones, verts)
    if args_anim := sys.argv[2:]:
        actions = mca.parse_mca(open(args_anim[0], "rb").read())
    else:
        p = 8; zeros = 0; actions = []
        while True:
            off, sz = struct.unpack_from("<II", d, p); p += 8
            if off == 0:
                zeros += 1
                if zeros == 2: break
                continue
            if zeros == 1 and d[off:off + 4] == b"MCA_":
                actions += mca.parse_mca(d, off)
    parents, rest = tree
    action = next(a for a in actions if len(a["segments"]) == len(parents))
    print(f"nodes={len(parents)} positions={len(vb)} bones={len(skin[0])} action duration={action['duration']}")

    # Triangle edges in position space
    dup = [v[0] for v in skin[1]]
    remap = np.repeat(np.arange(len(vb)), dup)
    edges = set()
    k = 0
    for L in ib[0]:
        s_ = remap[ib[1][k:k + L]]; k += L
        for a, b in zip(s_[:-1], s_[1:]):
            if a != b: edges.add((min(a, b), max(a, b)))
    E = np.array(sorted(edges))
    # Only edges whose endpoints follow different bones are sensitive to joint
    # conventions; edges inside one rigid bone keep their length under any rotation.
    dominant = np.array([v[2][int(np.argmax(v[1]))] if v[1] else -1 for v in skin[1]])
    E = E[dominant[E[:, 0]] != dominant[E[:, 1]]]
    bind_len = np.linalg.norm(vb[E[:, 0]] - vb[E[:, 1]], axis=1)
    print(f"cross-bone edges: {len(E)}")

    rest_local = [local_matrix(*r) for r in rest]
    rest_world = worlds(parents, rest_local)
    inv_bind = [np.linalg.inv(w) for w in rest_world]

    variants = {
        "turns, Rz*Ry*Rx (decoded)": (2 * math.pi, False),
        "radians, Rz*Ry*Rx": (1.0, False),
        "turns, Rx*Ry*Rz": (2 * math.pi, True),
    }
    for name, (unit, reverse) in variants.items():
        worst = []
        for frac in (0.25, 0.5, 0.75):
            t = action["duration"] * frac
            loc = []
            for i, (X, Y, Z, T, S) in enumerate(rest):
                R = np.column_stack([X, Y, Z]); Tn = T.copy(); Sn = S.copy()
                for tr in action["segments"][i]:
                    if tr["type"] == 1: Tn = eval_track(tr, t, 3)
                    elif tr["type"] == 5:
                        a = eval_track(tr, t, 3) * unit
                        R = euler_xyz(a) if not reverse else euler_xyz(a[::-1]).T
                    elif tr["type"] == 11: Sn = eval_track(tr, t, 3)
                m = np.eye(4); m[:3, :3] = R * Sn; m[:3, 3] = Tn
                loc.append(m)
            W = worlds(parents, loc)
            # Animations hide parts by scaling nodes to ~0; leave those vertices out
            visible_bone = [abs(np.linalg.det((W[n] @ inv_bind[n])[:3, :3])) > 0.5 for n in skin[0]]
            keep = np.array([all(visible_bone[b] for b in v[2]) for v in skin[1]])
            pos = np.zeros_like(vb)
            for vi, (dp, w, b) in enumerate(skin[1]):
                p4 = np.append(vb[vi], 1.0)
                acc = np.zeros(4)
                for wi, bi in zip(w, b):
                    node = skin[0][bi]
                    acc += wi * (W[node] @ inv_bind[node] @ p4)
                pos[vi] = acc[:3] if w else vb[vi]
            vis = keep[E[:, 0]] & keep[E[:, 1]]
            anim_len = np.linalg.norm(pos[E[vis, 0]] - pos[E[vis, 1]], axis=1)
            ratio = anim_len / np.maximum(bind_len[vis], 1e-6)
            moved = np.median(np.linalg.norm(pos[keep] - vb[keep], axis=1))
            worst.append((np.mean(np.abs(ratio - 1)), np.percentile(np.abs(ratio - 1), 90), moved))
        print(f"  {name:28} cross-bone stretch mean/p90 (vertex moved median) at 25/50/75%: " +
              "  ".join(f"{m * 100:5.1f}%/{p * 100:5.1f}% ({mv:4.1f})" for m, p, mv in worst))


if __name__ == "__main__":
    main()
