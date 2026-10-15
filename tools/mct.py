"""MCT texture container reader.

Layout (little endian), reversed from read_mdl_mem_mct / hwalCreateTexture_ES1:
  0x00  'MCT_'
  0x0a  u32   file size - 10
  0x1a  u16   texture id
  0x1e  u32   decoded payload size (0 when stored raw)
  0x22  u16   width
  0x24  u16   height
  0x26  u16   1 = payload is HI-LZW compressed, 0 = raw
  0x28  u16   pixel format (index into the engine's GL format tables, see FORMATS)
  0x2a  ...   payload
"""
import struct
import sys

import numpy as np

import hilzw

FORMATS = {
    1: "RGB888", 2: "RGBA8888", 3: "RGB565", 4: "RGBA5551", 5: "RGBA4444",
    6: "PAL8_RGB8", 7: "PAL8_RGBA8", 8: "PAL8_R5G6B5", 9: "PAL8_RGB5A1", 10: "PAL8_RGBA4",
    11: "PAL4_RGB8", 12: "PAL4_RGBA8", 13: "PAL4_R5G6B5", 14: "PAL4_RGB5A1", 15: "PAL4_RGBA4",
    16: "L8", 17: "A8", 18: "LA88", 21: "RGB888", 22: "RGBA8888",
    25: "PVRTC_RGBA_4", 26: "PVRTC_RGBA_2", 27: "PVRTC_RGB_4", 28: "PVRTC_RGB_2",
}


class MCT:
    def __init__(self, data):
        if data[:4] != b"MCT_":
            raise ValueError("not an MCT file")
        self.tex_id = struct.unpack_from("<H", data, 0x1a)[0]
        size, self.width, self.height, compressed, self.fmt = struct.unpack_from("<IHHHH", data, 0x1e)
        if compressed:
            self.payload, _ = hilzw.decode(data, 0x2a)
        else:
            self.payload = data[0x2a:]
        self.format_name = FORMATS.get(self.fmt, f"unknown({self.fmt})")

    def to_rgba(self):
        """Decode to an (h, w, 4) uint8 array, rows top to bottom as stored."""
        return decode_pixels(self.payload, self.width, self.height, self.fmt)


def _expand(v, bits):
    v = v.astype(np.uint32)
    return ((v << (8 - bits)) | (v >> (2 * bits - 8))).astype(np.uint8) if bits >= 4 else \
        (v * 255 // ((1 << bits) - 1)).astype(np.uint8)


def _unpack16(px, kind):
    px = px.astype(np.uint32)
    if kind == "565":
        return np.stack([_expand(px >> 11, 5), _expand((px >> 5) & 63, 6), _expand(px & 31, 5),
                         np.full(px.shape, 255, np.uint8)], -1)
    if kind == "5551":
        return np.stack([_expand(px >> 11, 5), _expand((px >> 6) & 31, 5), _expand((px >> 1) & 31, 5),
                         ((px & 1) * 255).astype(np.uint8)], -1)
    if kind == "4444":
        return np.stack([_expand(px >> 12, 4), _expand((px >> 8) & 15, 4), _expand((px >> 4) & 15, 4),
                         _expand(px & 15, 4)], -1)
    raise ValueError(kind)


def _palette(raw, kind, n):
    if kind == "RGB8":
        p = np.frombuffer(raw, np.uint8, n * 3).reshape(n, 3)
        return np.concatenate([p, np.full((n, 1), 255, np.uint8)], 1), n * 3
    if kind == "RGBA8":
        return np.frombuffer(raw, np.uint8, n * 4).reshape(n, 4).copy(), n * 4
    px = np.frombuffer(raw, "<u2", n)
    return _unpack16(px, {"R5G6B5": "565", "RGB5A1": "5551", "RGBA4": "4444"}[kind]), n * 2


def decode_pixels(buf, w, h, fmt):
    name = FORMATS[fmt]
    n = w * h
    if name == "RGB888":
        p = np.frombuffer(buf, np.uint8, n * 3).reshape(h, w, 3)
        return np.concatenate([p, np.full((h, w, 1), 255, np.uint8)], 2)
    if name == "RGBA8888":
        return np.frombuffer(buf, np.uint8, n * 4).reshape(h, w, 4).copy()
    if name in ("RGB565", "RGBA5551", "RGBA4444"):
        kind = {"RGB565": "565", "RGBA5551": "5551", "RGBA4444": "4444"}[name]
        return _unpack16(np.frombuffer(buf, "<u2", n), kind).reshape(h, w, 4)
    if name.startswith("PAL"):
        bits = 8 if name.startswith("PAL8") else 4
        pal, used = _palette(buf, name.split("_", 1)[1], 1 << bits)
        if bits == 8:
            idx = np.frombuffer(buf, np.uint8, n, used)
        else:  # OES_compressed_paletted_texture: first texel in the high nibble
            packed = np.frombuffer(buf, np.uint8, n // 2, used)
            idx = np.stack([packed >> 4, packed & 15], -1).reshape(-1)
        return pal[idx].reshape(h, w, 4)
    if name == "L8":
        l = np.frombuffer(buf, np.uint8, n).reshape(h, w)
        return np.stack([l, l, l, np.full_like(l, 255)], -1)
    if name == "A8":
        a = np.frombuffer(buf, np.uint8, n).reshape(h, w)
        return np.stack([np.full_like(a, 255)] * 3 + [a], -1)
    if name == "LA88":
        la = np.frombuffer(buf, np.uint8, n * 2).reshape(h, w, 2)
        return np.stack([la[..., 0]] * 3 + [la[..., 1]], -1)
    if name in ("PVRTC_RGBA_4", "PVRTC_RGB_4"):
        return decode_pvrtc4(buf, w, h)
    raise NotImplementedError(name)


# ---------------------------------------------------------------- PVRTC 4bpp

def _twiddle(bx, by, bw, bh):
    """Morton index of block (bx, by) in a bw x bh block grid (PowerVR TwiddleUV)."""
    min_dim = min(bw, bh)
    out = np.zeros_like(bx)
    shift = 0
    bit = 1
    while bit < min_dim:
        out |= ((by >> shift) & 1) << (2 * shift)
        out |= ((bx >> shift) & 1) << (2 * shift + 1)
        bit <<= 1
        shift += 1
    rest = (by if bh > bw else bx) >> shift
    return out | (rest << (2 * shift))


def decode_pvrtc4(buf, w, h):
    bw, bh = max(w // 4, 2), max(h // 4, 2)
    words = np.frombuffer(buf, "<u4", bw * bh * 2).reshape(-1, 2).astype(np.uint32)
    by, bx = np.mgrid[0:bh, 0:bw]
    words = words[_twiddle(bx, by, bw, bh)]          # (bh, bw, 2) in raster order
    mod, col = words[..., 0], words[..., 1]

    def colour(c, opaque_bit, opaque, transparent):
        o = ((c >> opaque_bit) & 1).astype(bool)
        return np.where(o[..., None], opaque(c), transparent(c))

    # colour A: low 16 bits (bit 0 is the modulation mode flag), colour B: high 16 bits
    ca = colour(col, 15,
                lambda c: np.stack([_expand((c >> 10) & 31, 5), _expand((c >> 5) & 31, 5),
                                    _expand(((c >> 1) & 15) << 1 | ((c >> 4) & 1), 5),
                                    np.full(c.shape, 255, np.uint8)], -1),
                lambda c: np.stack([_expand((c >> 8) & 15, 4), _expand((c >> 4) & 15, 4),
                                    _expand((c >> 1) & 7, 3), _expand((c >> 12) & 7, 3)], -1))
    cb = colour(col, 31,
                lambda c: np.stack([_expand((c >> 26) & 31, 5), _expand((c >> 21) & 31, 5),
                                    _expand((c >> 16) & 31, 5), np.full(c.shape, 255, np.uint8)], -1),
                lambda c: np.stack([_expand((c >> 24) & 15, 4), _expand((c >> 20) & 15, 4),
                                    _expand((c >> 16) & 15, 4), _expand((c >> 28) & 7, 3)], -1))

    # Bilinearly upscale the A/B images; block colours sit at block centres (offset 2px)
    ys, xs = np.mgrid[0:h, 0:w]
    fx, fy = (xs - 2) / 4.0, (ys - 2) / 4.0
    x0, y0 = np.floor(fx).astype(int), np.floor(fy).astype(int)
    tx, ty = (fx - x0)[..., None], (fy - y0)[..., None]
    x0w, x1w, y0w, y1w = x0 % bw, (x0 + 1) % bw, y0 % bh, (y0 + 1) % bh

    def up(img):
        img = img.astype(np.float32)
        top = img[y0w, x0w] * (1 - tx) + img[y0w, x1w] * tx
        bot = img[y1w, x0w] * (1 - tx) + img[y1w, x1w] * tx
        return top * (1 - ty) + bot * ty

    A, B = up(ca), up(cb)

    # Per-pixel 2-bit modulation
    bxs, bys = xs // 4, ys // 4
    m = (mod[bys, bxs] >> (2 * ((ys % 4) * 4 + (xs % 4)))) & 3
    punch = (col[bys, bxs] & 1).astype(bool)
    weight = np.where(punch, np.array([0, 4, 4, 8])[m], np.array([0, 3, 5, 8])[m]) / 8.0
    out = A * (1 - weight[..., None]) + B * weight[..., None]
    out = np.clip(out + 0.5, 0, 255).astype(np.uint8)
    out[..., 3] = np.where(punch & (m == 2), 0, out[..., 3])
    return out


if __name__ == "__main__":
    from PIL import Image
    # Usage: mct.py <out_dir> <file.mct>...  — decode textures to PNG
    import os
    out_dir = sys.argv[1]
    os.makedirs(out_dir, exist_ok=True)
    for path in sys.argv[2:]:
        name = os.path.basename(path)
        try:
            t = MCT(open(path, "rb").read())
            Image.fromarray(t.to_rgba(), "RGBA").save(os.path.join(out_dir, name[:-4] + ".png"))
            print(f"ok   {name}: id={t.tex_id} {t.width}x{t.height} {t.format_name}")
        except Exception as e:  # keep going so one bad file doesn't stop the batch
            print(f"FAIL {name}: {e}")
