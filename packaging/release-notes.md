# CR1-U v0.2.0

A from-scratch reimplementation of *Chaos Rings* (iOS / Vita / Android): builds for Windows,
macOS, Linux and Android.

**New in this release: your copy found by itself.** Put it in the folder holding the program, in
a folder named after its release - `ios/`, `android/` or `vita/` (these come made in the download,
each with a `README.txt` saying what goes in it) - and start the program: nothing to set. HD textures
the same way: `--export-textures` writes to `textures/export/`, `tools/hd_textures.py <the game
folder>` upscales into `textures/hd/`, and the game turns them on by itself. The same layout in all
the Chaos Rings ports; the older places still work.

| File | Platform |
|---|---|
| `CR1-U-windows-x64.zip` | Windows 10+ x64 (Vulkan or OpenGL; FSR 3.1) |
| `CR1-U-macos-universal.zip` | macOS 11+, Apple silicon and Intel (Vulkan through MoltenVK, OpenGL; FSR 3.1, MetalFX on macOS 13+) |
| `CR1-U-x86_64.AppImage`, `CR1-U-linux-x86_64.tar.gz` | Linux x86-64 (Vulkan or OpenGL; FSR 3.1) |
| `CR1-U-android.apk` | Android 7+, arm64 / x86-64 (OpenGL ES 3.0) |

**The game's data** - nothing to set: put your copy in the folder holding the program (the folder
holding the `.app` on macOS, the AppImage's on Linux; on Android
`Android/data/local.preservation.chaosrings/files/` on the shared storage), in a folder named after its release:
`ios/` - the iPad 1.0.1's `ChaosRingsHD.app` (or the iPhone 2.2.0's `ChaosRings.app`, or the unpacked `.ipa`); `android/` - the 1.1.1 release's `main.100.com.square_enix.chaosrings.googleplay.obb` and its APK; `vita/` - the installed `PCSG00497` folder. Any one is enough, and each
may hold it as it was unpacked or installed (one or two folders down). The iOS copy is the game and the others add their voices, music, languages and art to it (Options, Game); alone, the Vita or the Android release is the game. If none is found it
says where it looked; the README's "Your copy" has the details.

**Also in it (from v0.1.0):** the game playable with its own menus, Options pages for display, graphics
(render scale, MSAA, FSR 3.1 / DLSS / MetalFX), and the new Detail page (HD textures, smoothed
models, anisotropic filtering, texture filtering); HD textures you make yourself (`chaosrings --export-textures` into `textures/export/`, then
`tools/hd_textures.py <the game folder>` into `textures/hd/`, read by itself: the README's "HD textures").

**Not included:** any of the game. You need your own copy; the port reads it at run time. DLSS is
not in these builds either (NVIDIA's NGX library is proprietary and the port is GPL): build it
yourself with the SDK for your own use (README, Platforms).

**First run**
- **Windows:** unzip and run `chaosrings.exe`. SmartScreen may warn about an unsigned program: *More info*, *Run anyway*.
- **macOS 11+:** unzip, then right-click `chaosrings.app` and choose *Open* (it is not signed or notarized), or run `xattr -dr com.apple.quarantine chaosrings.app`.
- **Linux:** `chmod +x CR1-U-x86_64.AppImage` and run it, or unpack the tar.gz.
- **Android 7+:** allow installing from your browser or file manager, install the APK, then copy the data over USB as below.

**Status of this build:** the Linux build is the one played and tested; the Windows build is checked
under Wine (Vulkan and OpenGL, the same frames as Linux's). The macOS and Android builds are built
by CI but have not yet been run on a real Mac or device - MetalFX, especially, is untested. Reports
welcome.

Licence: GPL-2.0-or-later; the macOS `.app` carries MoltenVK and the Vulkan loader (Apache-2.0), so
it is distributed under GPL-3.0 (THIRD_PARTY.md). Personal, non-commercial preservation project, not
affiliated with Square Enix or Media.Vision.
