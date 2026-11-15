#!/usr/bin/env python3
"""The Windows .ico, macOS .icns and AppImage .png of the port's own icon (packaging/linux/<id>.svg - two rings,
not the game's art), with ImageMagick's `magick` for the pictures:

  packaging/make_icons.py <id>      -> packaging/windows/<id>.ico, packaging/macos/<id>.icns,
                                       packaging/linux/<id>.png, packaging/ios/Assets.xcassets (AppIcon)

iOS's is square and opaque - the system rounds its corners and shows transparency black - so the
SVG's rounded backdrop is drawn to the edges there, the picture flattened onto it.
"""
import json
import re
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
    render(svg, 256, os.path.join(here, 'linux', ident + '.png')) # (appimagetool wants a PNG at the AppDir's top)
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
        # iOS: one 1024 x 1024 icon (Xcode 14's single size) in an asset catalog, its backdrop square
        with open(svg) as f:
            square = re.sub(r'(<rect [^>]*?)\s+rx="[^"]*"', r'\1', f.read(), count=1)
        backdrop = re.search(r'<rect [^>]*fill="(#[0-9a-fA-F]+)"', square).group(1)
        flat_svg = os.path.join(tmp, 'ios.svg')
        with open(flat_svg, 'w') as f:
            f.write(square)
        icon_set = os.path.join(here, 'ios', 'Assets.xcassets', 'AppIcon.appiconset')
        os.makedirs(icon_set, exist_ok=True)
        subprocess.run(['magick', '-background', 'none', '-density', '1536', flat_svg, '-resize', '1024x1024',
                        '-background', backdrop, '-alpha', 'remove', '-alpha', 'off', os.path.join(icon_set, 'icon-1024.png')], check=True)
        info = {'author': 'xcode', 'version': 1}
        with open(os.path.join(here, 'ios', 'Assets.xcassets', 'Contents.json'), 'w') as f:
            json.dump({'info': info}, f, indent=2)
        with open(os.path.join(icon_set, 'Contents.json'), 'w') as f:
            json.dump({'images': [{'filename': 'icon-1024.png', 'idiom': 'universal', 'platform': 'ios', 'size': '1024x1024'}],
                       'info': info}, f, indent=2)


if __name__ == '__main__':
    main()
