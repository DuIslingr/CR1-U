#!/usr/bin/env python3
"""HD textures for the Chaos Rings ports - CR1:U, CRO-U, CR2-U and CR3-U, the same tool and files in
each: upscales the pictures the game's --export-textures wrote (<hash>.crraw) with waifu2x
(waifu2x-ncnn-vulkan) or Real-ESRGAN (realesrgan-ncnn-vulkan) into <hash>.crhd, which the game loads
in place of the originals when HD Textures is on (Options; on by itself once textures/hd/ is there).

Everything lives in the game folder (the folder holding the program - beside the .app on macOS,
beside the .AppImage on Linux, Android/data/<package>/files/ on Android):

  <game folder>/textures/export/   the originals, written by the game:   <program> --export-textures
  <game folder>/textures/hd/       the upscales, read by the game:       tools/hd_textures.py <game folder>

  tools/hd_textures.py <game folder> [--esrgan <folder with realesrgan-ncnn-vulkan and models/>]
      [--waifu2x <its executable or folder>]      (--source / --out: other folders than those two)

--engine auto (the default) picks by how the release stored each picture (the .crraw's flags):
lossless ones (8 bits a channel: RGBA8888, I8, AI88, 8-bit palettes, the Vita's U8...) and
block-compressed ones (the iPad's PVRTC, the Android release's ETC1, the Vita's UBC) go through
waifu2x's CUnet - no denoising, or level 1 for the blocks' noise - and 16-bit ones (RGBA4444 / 5551
/ RGB565: dithered or banded) through Real-ESRGAN x4plus, which smooths dithering away where waifu2x
keeps it. Measured (CR1:U's docs/FORMATS.md, "HD textures"): on the Vita's lossless art halved and
brought back 2x, waifu2x CUnet 33.1 dB / SSIM 0.968 against Real-ESRGAN x4plus's 27.7 / 0.919
(Lanczos 30.5 / 0.945) - Real-ESRGAN draws outlines and reshapes small details (eyes, round dots)
that aren't there - and about 15 times faster. Exports from before the flags (0: unknown) take
waifu2x's level 1. --engine waifu2x / esrgan puts everything through one.

waifu2x works at 2x a pass (x2, x4 ... as --scale needs), Real-ESRGAN at 4x; the colour is then
resampled (Lanczos) to --scale and alpha is resampled from the original (cut-outs keep their
shape); see-through pixels first take their opaque neighbours' colour so no dark fringes bleed in.
Alpha-only textures (A8, the Vita's U8) put their alpha through the network instead. Each release
exports its own pictures (their hashes differ); one textures/hd/ can hold them all. CR1:U and CRO-U
with Vita Assets on export the Vita's lossless version of each picture it has alike and leave
<ios hash>.cralias files naming it: each becomes a hard link (else a copy) to that picture's .crhd,
so one upscale serves the game with or without the Vita's. Personal use: the outputs are derived
from the game's art - keep them out of anything shared.
"""

import argparse
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

import numpy as np
from PIL import Image

LOSSLESS, PVRTC, BITS16 = 1, 2, 3  # the .crraw's storage (imageStorage); 0 unknown
STORAGE_NAME = {0: 'unknown', LOSSLESS: 'lossless', PVRTC: 'block-compressed', BITS16: '16-bit'}


def read_raw(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:6] != b'CRRAW1':
        raise ValueError(f'{path}: not a .crraw')
    w, h, flags = struct.unpack_from('<III', data, 6)
    px = np.frombuffer(data, np.uint8, w * h * 4, 18).reshape(h, w, 4)
    return px, bool(flags & 1), (flags >> 1) & 3


def write_hd(path, px, orig_w, orig_h, alpha_only):
    h, w = px.shape[:2]
    head = b'CRHD1\0' + struct.pack('<IIIII', w, h, orig_w, orig_h, 1 if alpha_only else 0)
    tmp = path + '.part'
    with open(tmp, 'wb') as f:
        f.write(head + zlib.compress(np.ascontiguousarray(px, np.uint8).tobytes(), 6))
    os.replace(tmp, path)


def bleed(rgb, alpha, steps=24):
    """See-through pixels take the average colour of nearby opaque ones."""
    rgb = rgb.astype(np.float32)
    known = alpha > 8
    if known.all() or not known.any():
        return rgb.astype(np.uint8)
    for _ in range(steps):
        k = known.astype(np.float32)
        acc = np.zeros_like(rgb)
        cnt = np.zeros_like(k)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                acc += np.roll(np.roll(rgb * k[..., None], dy, 0), dx, 1)
                cnt += np.roll(np.roll(k, dy, 0), dx, 1)
        grow = (~known) & (cnt > 0)
        rgb[grow] = acc[grow] / cnt[grow][:, None]
        known = known | grow
        if known.all():
            break
    rgb[~known] = rgb[known].mean(axis=0)
    return np.clip(rgb, 0, 255).astype(np.uint8)


def resize(arr, w, h):
    return np.asarray(Image.fromarray(arr).resize((w, h), Image.LANCZOS))


def find_waifu2x(arg):
    """(executable, CUnet models folder): the given executable or folder, else the one on PATH"""
    if arg:
        exe = os.path.join(arg, 'waifu2x-ncnn-vulkan') if os.path.isdir(arg) else arg
    else:
        exe = shutil.which('waifu2x-ncnn-vulkan')
    if not exe or not os.path.exists(exe):
        return None, None
    here = os.path.dirname(os.path.realpath(exe))
    for models in (os.path.join(here, 'models-cunet'), os.path.join(here, '..', 'share', 'waifu2x-ncnn-vulkan', 'models-cunet'),
                   '/usr/share/waifu2x-ncnn-vulkan/models-cunet'):
        if os.path.isdir(models):
            return exe, os.path.normpath(models)
    return exe, None


def choose(engine, storage, args):
    """The network for one picture: ('waifu2x', noise level) or ('esrgan', model)"""
    if engine == 'esrgan' or (engine == 'auto' and storage == BITS16):
        return ('esrgan', args.model)
    if args.noise is not None:
        return ('waifu2x', args.noise)
    return ('waifu2x', -1 if storage == LOSSLESS else 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('game', nargs='?', default='.', help='the game folder (default: here): its textures/export/ in, textures/hd/ out')
    ap.add_argument('--source', help='the .crraw folder (default: <game folder>/textures/export, the game\'s --export-textures)')
    ap.add_argument('--out', help='where the .crhd files go (default: <game folder>/textures/hd, where the game looks)')
    ap.add_argument('--engine', choices=('auto', 'waifu2x', 'esrgan'), default='auto',
                    help='auto (default): waifu2x for lossless and block-compressed pictures, Real-ESRGAN for 16-bit ones')
    ap.add_argument('--waifu2x', help='waifu2x-ncnn-vulkan: its executable or folder (default: the one on PATH)')
    ap.add_argument('--esrgan', help='folder holding realesrgan-ncnn-vulkan and models/ (for 16-bit pictures, or --engine esrgan)')
    ap.add_argument('--noise', type=int, choices=(-1, 0, 1, 2, 3),
                    help="waifu2x's denoise level for every picture (default: -1 for lossless ones, 1 otherwise)")
    ap.add_argument('--model', default='realesrgan-x4plus',
                    help='Real-ESRGAN: realesrgan-x4plus (default: sharper edges, keeps the brushwork), '
                         'realesrgan-x4plus-anime (crisper, flattens soft shading) or realesr-animevideov3 (softest)')
    ap.add_argument('--batch', type=int, default=24, help='pictures per network run (default 24)')
    ap.add_argument('--scale', type=float, default=2.0, help='output size / original (default 2)')
    ap.add_argument('--max', type=int, default=4096, help='largest side of an output (default 4096)')
    ap.add_argument('--only', nargs='*', help='just these hashes')
    ap.add_argument('--force', action='store_true', help='redo existing outputs')
    ap.add_argument('--keep', help='also keep the network outputs (PNG) here, for inspection')
    args = ap.parse_args()
    args.source = args.source or os.path.join(args.game, 'textures', 'export')
    args.out = args.out or os.path.join(args.game, 'textures', 'hd')
    if not os.path.isdir(args.source):
        print(f'{args.source}: no exported pictures - run the game with --export-textures first')
        return 2

    os.makedirs(args.out, exist_ok=True)
    names = sorted(n[:-6] for n in os.listdir(args.source) if n.endswith('.crraw'))
    if args.only:
        names = [n for n in names if n in set(args.only)]
    todo = [n for n in names if args.force or not os.path.exists(os.path.join(args.out, n + '.crhd'))]

    # Each picture's network, grouped so each runs once a batch
    groups, counts = {}, {}
    for n in todo:
        with open(os.path.join(args.source, n + '.crraw'), 'rb') as f:
            head = f.read(18)
        storage = (struct.unpack_from('<I', head, 14)[0] >> 1) & 3 if head[:6] == b'CRRAW1' else 0
        how = choose(args.engine, storage, args)
        groups.setdefault(how, []).append(n)
        counts[(STORAGE_NAME[storage], how)] = counts.get((STORAGE_NAME[storage], how), 0) + 1
    print(f'{len(names)} pictures, {len(todo)} to do (x{args.scale:g}, max {args.max})')
    for (kind, how), c in sorted(counts.items(), key=str):
        print(f'  {c:4d} {kind:8s} -> ' + (f'waifu2x CUnet, noise {how[1]}' if how[0] == 'waifu2x' else f'Real-ESRGAN {how[1]}'))
    if not todo:
        link_aliases(args)
        return 0

    w2x_exe, w2x_models = find_waifu2x(args.waifu2x)
    esr_exe = os.path.join(args.esrgan, 'realesrgan-ncnn-vulkan') if args.esrgan else None
    esr_models = os.path.join(args.esrgan, 'models') if args.esrgan else None
    if any(h[0] == 'waifu2x' for h in groups) and not (w2x_exe and w2x_models):
        print('waifu2x-ncnn-vulkan (with models-cunet) not found: install it or give --waifu2x')
        return 2
    if any(h[0] == 'esrgan' for h in groups) and not (esr_exe and os.path.exists(esr_exe)):
        print('Real-ESRGAN is needed for the 16-bit pictures (or --engine esrgan): give --esrgan <folder>, '
              'or --engine waifu2x for everything')
        return 2

    # Work files (network outputs are large) beside the outputs, on disk
    work = tempfile.mkdtemp(prefix='.work-', dir=args.out)
    done = 0
    try:
        for how, batch_names in sorted(groups.items(), key=lambda g: str(g[0])):
            net = (w2x_exe, w2x_models, how[1]) if how[0] == 'waifu2x' else (esr_exe, esr_models, how[1])
            for start in range(0, len(batch_names), args.batch):
                batch = batch_names[start:start + args.batch]
                done += run_batch(args, how[0], net, work, batch)
                print(f'{how[0]} {how[1]}: {start + len(batch)}/{len(batch_names)}', flush=True)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print(f'{done} written to {args.out}')
    link_aliases(args)
    return 0 if done == len(todo) else 1


def link_aliases(args):
    """<ios hash>.cralias (an export with --vita): that hash's .crhd is the Vita picture's"""
    linked = missing = 0
    for n in sorted(os.listdir(args.source)):
        if not n.endswith('.cralias'):
            continue
        with open(os.path.join(args.source, n)) as f:
            target = os.path.join(args.out, f.read().strip() + '.crhd')
        dest = os.path.join(args.out, n[:-8] + '.crhd')
        if not os.path.exists(target):
            missing += 1
            continue
        if os.path.exists(dest):
            if os.path.samefile(dest, target) or not args.force:
                continue
            os.remove(dest)
        try:
            os.link(target, dest)
        except OSError:
            shutil.copyfile(target, dest)
        linked += 1
    if linked or missing:
        print(f'{linked} iOS pictures linked to their Vita upscales' + (f', {missing} without one yet' if missing else ''))


def run_batch(args, engine, net, work, todo):
    src_dir, net_dir = os.path.join(work, 'in'), os.path.join(work, 'out')
    for d in (src_dir, net_dir):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
    meta = {}
    for n in todo:
        px, alpha_only, _ = read_raw(os.path.join(args.source, n + '.crraw'))
        feed = np.repeat(px[..., 3:4], 3, axis=2) if alpha_only else bleed(px[..., :3], px[..., 3])
        Image.fromarray(np.ascontiguousarray(feed)).save(os.path.join(src_dir, n + '.png'))
        meta[n] = (px, alpha_only)
    exe, models, setting = net
    # One run per batch: the network is loaded once for it
    if engine == 'waifu2x':
        # 2x a pass: the smallest power of two reaching --scale (the outputs are resampled to it)
        factor = 2 ** max(1, math.ceil(math.log2(max(args.scale, 1.0))))
        cmd = [exe, '-i', src_dir, '-o', net_dir, '-n', str(setting), '-s', str(factor), '-m', models, '-f', 'png']
    else:
        cmd = [exe, '-i', src_dir, '-o', net_dir, '-n', setting, '-s', '4', '-m', models, '-f', 'png']
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        print(r.stdout[-4000:])
        return 0
    done = 0
    for n in todo:
        px, alpha_only = meta[n]
        h, w = px.shape[:2]
        k = min(args.scale, args.max / max(w, h))
        ow, oh = max(w, int(round(w * k))), max(h, int(round(h * k)))
        net_path = os.path.join(net_dir, n + '.png')
        if not os.path.exists(net_path):
            print(f'{n}: no output from the network')
            continue
        net_px = np.asarray(Image.open(net_path).convert('RGB'))
        if args.keep:
            os.makedirs(args.keep, exist_ok=True)
            shutil.copy(net_path, os.path.join(args.keep, n + '.png'))
        out = np.empty((oh, ow, 4), np.uint8)
        if alpha_only:
            out[..., :3] = resize(np.ascontiguousarray(px[..., :3]), ow, oh)
            out[..., 3] = resize(np.ascontiguousarray(net_px[..., 0]), ow, oh)
        else:
            out[..., :3] = net_px if net_px.shape[:2] == (oh, ow) else resize(net_px, ow, oh)
            out[..., 3] = resize(np.ascontiguousarray(px[..., 3]), ow, oh)
        write_hd(os.path.join(args.out, n + '.crhd'), out, w, h, alpha_only)
        done += 1
    return done


if __name__ == '__main__':
    sys.exit(main())
