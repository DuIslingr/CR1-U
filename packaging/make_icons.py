#!/usr/bin/env python3
"""The Windows .ico and macOS .icns of the port's own icon (packaging/linux/<id>.svg - two rings,
not the game's art), with ImageMagick's `magick` for the pictures:

  packaging/make_icons.py <id>      -> packaging/windows/<id>.ico, packaging/macos/<id>.icns
"""
import os
import struct
import subprocess
import sys
import tempfile

here = os.path.dirname(os.path.abspath(__file__))


def render(svg, size, out):
    subprocess.run(['magick', '-background', 'none', '-density', '384', svg, '-resize', f'{size}x{size}', out], check=True)


def main():
    ident = sys.argv[1]
    svg = os.path.join(here, 'linux', ident + '.svg')
    os.makedirs(os.path.join(here, 'windows'), exist_ok=True)
    os.makedirs(os.path.join(here, 'macos'), exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        sizes = [16, 24, 32, 48, 64, 128, 256]
        pngs = []
        for s in sizes:
            p = os.path.join(tmp, f'{s}.png')
            render(svg, s, p)
            pngs.append(p)
        subprocess.run(['magick', *pngs, os.path.join(here, 'windows', ident + '.ico')], check=True)
        # ICNS: 'icns', its length, then (type, length, PNG) chunks
        chunks = b''
        for kind, s in (('icp4', 16), ('icp5', 32), ('icp6', 64), ('ic07', 128), ('ic08', 256), ('ic09', 512),
                        ('ic10', 1024), ('ic11', 32), ('ic12', 64), ('ic13', 256), ('ic14', 512)):
            p = os.path.join(tmp, f'icns{s}.png')
            if not os.path.exists(p):
                render(svg, s, p)
            with open(p, 'rb') as f:
                data = f.read()
            chunks += kind.encode() + struct.pack('>I', 8 + len(data)) + data
        with open(os.path.join(here, 'macos', ident + '.icns'), 'wb') as f:
            f.write(b'icns' + struct.pack('>I', 8 + len(chunks)) + chunks)


if __name__ == '__main__':
    main()
