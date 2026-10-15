# Third-party code

CR1:U's own code is licensed under the GNU General Public License, version 2 or (at your
option) any later version - see `LICENSE`. These libraries are included under their own
licences, kept beside them in `engine/third_party/`:

| Folder | What | Licence |
|---|---|---|
| `faad2/` | FAAD2 2.11.1's `libfaad` (AAC music decoding when the system has no FAAD2). Code from FAAD2 is copyright (c) Nero AG, www.nero.com | GPL-2.0-or-later (`COPYING`) |
| `libatrac9/` | LibAtrac9 by Alex Barney, commit efca2e3 (the Vita release's ATRAC9 voices and music) | MIT (`LICENSE`) |
| `hevag/` | The HE-VAG prediction filter table from vgmstream (the Vita's sound effects) | ISC (`COPYING`) |
| `fidelityfx/` | AMD FidelityFX SDK 1.1.4's FSR 3.1 GPU passes | MIT (`LICENSE.txt`) |
| `stb/` | stb_truetype by Sean Barrett | MIT / public domain (in the file) |

Not included: the **NVIDIA DLSS SDK** (proprietary, NVIDIA RTX SDKs licence). DLSS support
builds only when you place the SDK's `include/` and `lib/libnvsdk_ngx.a` in
`engine/third_party/dlss/` yourself; without it the build has FSR 3.1 only. A build with DLSS
links proprietary code into a GPL program, so it is for your own use, not for redistribution.

No game data is included or distributed: *Chaos Rings* and its assets are the property of
Square Enix and Media.Vision. The port loads them at runtime from a copy you own.
