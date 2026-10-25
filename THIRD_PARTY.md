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
| `stb/` | stb_truetype and stb_vorbis 1.22 (commit 1ee679c; the Android release's Ogg Vorbis music) by Sean Barrett | MIT / public domain (in the files) |
| `cgltf/` | cgltf 1.13 by Johannes Kuhlmann - reads the HD models (glTF 2.0) | MIT (in the file) |
| `stb/stb_image.h`, `stb/stb_image_write.h` | stb_image 2.30 (PNG and JPEG only) and stb_image_write 1.16 by Sean Barrett - the HD models' pictures, read and written (`gltf_libs.c` builds them with cgltf) | public domain / MIT (in the files) |
| `psvpfsparser/` | Vita3K's psvpfsparser, commit 05d43a06 (github.com/Vita3K/psvpfsparser; from motoharu-gosuto's psvpfstools), trimmed, with its own crypto in place of OpenSSL (`README.md` there) - the first start's setup decrypts a Vita NoNpDrm dump with it, as Vita3K's install does. The setup's SELF decryption follows Vita3K's `packages/sce_utils.cpp` and uses its retail app keys | GPL-2.0, as Vita3K (with it, builds of the port are GPL-2.0) |

Built from source by CMake when the system has none (fetched at build time, not in the
repository; in the CI artifacts):

| What | Licence |
|---|---|
| [SDL 3.2.0](https://github.com/libsdl-org/SDL) (linked in statically) | zlib |
| [zlib 1.3.1](https://github.com/madler/zlib) (Windows, Android) | zlib |
| [libarchive 3.7.7](https://github.com/libarchive/libarchive) (Windows, macOS: the setup reads a Vita dump's .rar, .7z, .zip, .vpk) | BSD-2-Clause |

Carried by the macOS `.app` (Contents/Frameworks, from the LunarG Vulkan SDK; their licence texts in
Contents/Resources/licenses): the [Vulkan loader](https://github.com/KhronosGroup/Vulkan-Loader) and
[MoltenVK](https://github.com/KhronosGroup/MoltenVK), both Apache-2.0. Apache-2.0 meets the GPL in
its version 3, which "or any later version" allows: a `.app` with them is under GPL-3.0.

Not included: the **NVIDIA DLSS SDK** (proprietary, NVIDIA RTX SDKs licence). DLSS support
builds only with the SDK's headers and NGX library: `-DCR_DLSS_SDK=<a checkout of
github.com/NVIDIA/DLSS>` (Linux x86-64, Windows x64 with MSVC), or its `include/` and
`lib/libnvsdk_ngx.a` in `engine/third_party/dlss/`; without it the build has FSR 3.1 only
(and MetalFX on macOS). A build with DLSS links proprietary code into a GPL program, so it is for
your own use, not for redistribution - CI builds it to prove it, and uploads only the builds
without it.

No game data is included or distributed: *Chaos Rings* and its assets are the property of
Square Enix and Media.Vision. The port loads them at runtime from a copy you own.
